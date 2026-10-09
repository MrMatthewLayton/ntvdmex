/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Protected mode: DPMI's interrupt service -- INT 31h and the PM INT 21h/2Fh/33h
 *   paths, in DpmiServicePmIntBody.
 *
 * Its own translation unit (#335): declared in host_dpmi_int.h.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include "host_state.h"
#include "log.h"
#include "ne.h"
#include "wow32.h"
#include "wowanchors.h"
#include "wowsched.h"
#include "wowcall.h"
#include "wowmsg.h"
#include "wowres.h"
#include "wowwin.h"
#include "wowgdi.h"
#include "wowuser.h"
#include "wowdlg.h"
#include "wowenum.h"
#include "wowshell.h"
#include "wowcommdlg.h"
#include "wowkbd.h"
#include "wowsound.h"
#include "wowmmedia.h"
#include "host_dpmi_int.h"
#include "host_bios.h"
#include "host_dos.h"
#include "host_dpmi.h"
#include "host_io.h"
#include "host_irq.h"
#include "host_mouse.h"
#include "host_timing.h"
#include "host_video.h"
#include "host_wow.h"

/* -- FOLDING RUNS OF THE SAME WOW32 CALL. (session 56) See the WOWFOLD note in
 * the BOP handler. `mute` is set only while a run is being folded and is
 * cleared the instant a different call arrives, so it can never outlive the
 * pattern that justified it.
 */
#define WOWFOLD_KEEP    256u    /* full dumps kept per function id */

/* AND A SECOND, MUCH HIGHER CAP ON THE VERDICT LINE ITSELF:
 * The dump fold took TERMINAL from 177 MB to 85 MB and left 900,374 verdict
 * lines, which are then the whole remaining volume. They are kept on purpose
 * -- bmwow.sh's signature is a COUNT of them -- so the second cap has to sit
 * far above anything the gate can produce. The gate's ENTIRE run is 85
 * serviced / 113 declined / 57 unimpl, i.e. under a hundred per function;
 * 4096 is fifty times that. A guest that passes it is not being measured any
 * more, it is looping.
 */
#define WOWFOLD_HARD    4096u
#define WOWFOLD_SLOTS   1024u   /* Power of two; see the collision note */
static DWORD g_WowFoldSeen[WOWFOLD_SLOTS];
DWORD g_WowIdleWaits = 0;              /* #306: krnl386 idle waits that blocked */
/* WHERE krnl386's SEGMENT 1 ENDED UP IN PROTECTED MODE (Importance = 1):
 * krnl386 copies its own segment 1 out of conventional memory, relocates it, and
 * commits a code selector over the copy -- and the base of that copy is DIFFERENT
 * EVERY RUN (0x1ad00 and 0x2aec0 were measured one run apart). Every interesting
 * address in this investigation is `seg1:0xNNNN`, so a breakpoint list of absolute
 * linear addresses is a list that is wrong by the next run: session 32 lost readings
 * to exactly that. This is filled in when the selector is committed, and mode bit 1
 * in pmbp.txt means "the address column is an OFFSET IN THIS SEGMENT".
 */
static DWORD g_WowPmSegment1Base = 0;
DWORD g_WowPmBase[WOW_PMBASE_MAX];
/* -- THE CHANGE DETECTOR (pmchg.txt). One line: `<hex offset> [segment]`, the
 * segment defaulting to 4 (krnl386's DGROUP), because the addresses worth watching
 * are data whose base moves every run. Resolved lazily, the first PM event after
 * that segment's selector is committed. See the sampler in DpmiServicePmInt.
 */
DWORD g_PmWatchOffset = 0;      /* offset within the segment; 0 = disabled */
UINT g_PmWatchSegment = 4;
static DWORD g_PmWatchLinear = 0;      /* resolved linear address, 0 = not yet */
static BYTE  g_PmWatchLast = 0;
static BYTE  g_PmWatchHave = 0;
/* A ONE-SHOT BREAKPOINT IS RETIRED BY ITS HIT, AND NOTHING SAID SO (Importance = 2):
 * DpmiBreakpointArm() runs before every PM entry and re-plants anything not currently
 * armed. The "never re-plant a site the guest is standing on" guard keys off
 * g_BreakpointPending, and g_BreakpointPending is set ONLY for repeating and skip breakpoints --
 * so the ONE kind with no protection was the plain one-shot, which is the kind you
 * reach for first. Measured (session 36): a one-shot in krnl386's loader fired 512 times
 * with byte-identical registers and one millisecond on the clock, hit the arm
 * ceiling, and was then never re-planted -- so the SECOND time the guest reached
 * that site, which was the pass the breakpoint existed to observe, there was no
 * breakpoint there. The log showed 512 confident hits and answered nothing.
 *
 * Retire it explicitly. `pending` now means "the guest is standing on this
 * footprint" for EVERY kind of hit, and `done` means "this one-shot has fired".
 * The two were conflated, and the conflation is what let a one-shot loop.
 */
BYTE  g_BreakpointDone[DPMI_BP_MAX];
DWORD          g_PmIrqReflects = 0;      /* PM default IRQ stub -> BIOS action (s80) */
static DWORD          g_PmIrqReflectLogged = 0; /* bounded log budget for the above */
WORD  g_DpmiDosBlock[DPMI_DOSBLK_MAX]; /* live 0100 DOS blocks (segments) */
INT   g_DpmiDosBlockCount = 0;
INT   g_PmExitCode = 0;             /* AL of the client's PM AH=4Ch */
DWORD g_LeCodeSize[DPMI_LE_MAX];   /* page-rounded sizes of the EXEC objects */
/* ...and WHERE it is. Kept apart from g_PmInt[8] on purpose: that table is what INT 31h
 * 0204 reports back, and it must keep saying exactly what the client installed through
 * 0205. Answering 0204 with a handler DOS/4GW never set is how the first attempt at this
 * produced "fatal error (1001): error in interrupt chain" -- the extender queried the
 * vector during shutdown, did not recognise it, and concluded its chain was corrupt.
 * Where we DELIVER and what the vector table REPORTS are two different questions.
 */
WORD  g_PmAppTimerSelector = 0;
DWORD g_PmAppTimerOffset = 0;
INT g_PmDispatchTop;   /* ...as captured by the dispatch it applies to */
#define I33_SRC_SIM     3   /* DPMI 0300 simulate-real-mode-interrupt */
INT g_SimIntReflect = 0;
/* WHICH SELECTOR IS USER'S CODE SEGMENT? LEARN IT FROM A STUB (Importance = 2):
 * WowModuleOfSelector() cannot answer this. g_WowModule[] is the BIND-STAGE view --
 * the host's own NE load, used to verify and relocate -- and the modules that
 * matter at run time are loaded by krnl386 itself, which allocates their
 * selectors through `INT 31h 0x0501`. USER's segment 1 is 0x0327 in the run and
 * appears nowhere in g_WowModule[]. Measured: the first cut used the module lookup
 * and the dispatcher never fired once.
 *
 * Identify the TABLE by a stub in it. Every thunk call arrives carrying an id, an
 * argument byte count and the offset it returns to inside its 13-byte stub, at a
 * fixed place in USER's segment 1, so that triple pins one specific stub. Two
 * anchors, either of which is enough, both named by USER's own export table and
 * both seen in real runs:
 *    id 0x190  0 args  retstub 0x0659   FINALUSERINIT  (called once per task at
 *                                       task startup -- observed)
 *    id 0x039  4 args  retstub 0x0c25   REGISTERCLASS
 *
 * [CAUTION]: The offsets are this USER.EXE's. Regenerate with
 * `tools/ne/wowmap.py guest/ne/user.exe` if the box's USER.EXE ever differs --
 * and note that a WRONG anchor cannot silently mis-fire: all three fields have
 * to agree, and if none ever matches the dispatcher simply never engages and
 * every USER call stays honestly unimplemented.
 */
static WORD g_WowUserSegment = 0;

/* SHELL.DLL's TABLE -- A FOURTH ID SPACE. See src/wow/wowshell.h (Importance = 3):
 * Identified exactly the way USER's is, and for the same reason: krnl386 loads
 * SHELL.DLL itself through our DOS layer, so it is not in g_WowModule[] and there
 * is no file image on this side to check bytes against -- but the triple
 * (id, argument bytes, return-into-stub offset) still pins one specific stub,
 * because a stub is 13 fixed-shape bytes at a fixed offset in the module's own
 * segment 1. The one anchor was `ShellAbout` itself:
 *    id 0x016  12 args  retstub 0x00c7   SHELLABOUT
 * -- ordinal 22 (NE entry table offset 0x00ba), and the call as it arrives at
 * run time carries exactly those 12 argument bytes and that id.
 *
 * [INFO]: AND ANCHORING ON THE ONE CALL WE SERVICE WAS WRONG. It was written down as
 * deliberate -- "the table is identified by the first call that needs it" --
 * and it holds only while every guest that needs SHELL calls `ShellAbout`.
 * Notepad does. MS PAINT DOES NOT: it calls `RegCreateKey`, `RegSetValue`,
 * `RegQueryValue` and `DragAcceptFiles`, so SHELL was never identified at all,
 * all four were logged as "?'s table -- a DIFFERENT id space", and
 * `DragAcceptFiles` -- implemented since session 44 -- went unanswered. In the
 * log a module nobody identified and a service nobody wrote look the same.
 *
 * The anchor is now the module's WHOLE stub table, generated from the file by
 * `tools/ne/wowthunks.py --anchor`. See src/wow/wowanchors.h for why matching
 * any row is still safe, and regenerate there if the box's SHELL.DLL differs.
 */
static WORD g_WowShellSegment = 0;

/* COMMDLG.DLL's TABLE -- A FIFTH ID SPACE. See src/wow/wowcommdlg.h (Importance = 3):
 * Identified the same way, and both anchors are the calls we service:
 *    id 0x001   4 args  retstub 0x0012   GETOPENFILENAME
 *    id 0x002   4 args  retstub 0x0024   GETSAVEFILENAME
 * The run that drove Alt-F-O on a live Notepad reported `id 0x01 ... retstub=0x0012`
 * from a table this host had never seen. The anchor was written to match a
 * measurement, not the other way round.
 *
 * [CAUTION]: `0x01` is MessageBox in USER's numbering (12 args) and GetOpenFileName here
 * (4), which is exactly why the argument count is part of the anchor and why
 * the guard below excludes every table already identified.
 *
 * [CAUTION]: Regenerate with `tools/ne/wowthunks.py guest/ne/commdlg.dll` if the box's
 * COMMDLG ever differs.
 */
static WORD g_WowCommonDialogSegment = 0;

/* KEYBOARD.DRV's TABLE -- A SIXTH ID SPACE. See src/wow/wowkbd.h (Importance = 2):
 * Two anchors, both the calls we service:
 *    id 0x005  8 args  retstub 0x0079   ANSITOOEM
 *    id 0x006  8 args  retstub 0x0086   OEMTOANSI
 *
 * [CAUTION]: `0x05` is CHOOSECOLOR in COMMDLG's numbering and something else again in
 * USER's, which is why all three fields are matched and why the guard below
 * excludes every table already identified.
 */
static WORD g_WowKeyboardSegment = 0;

/* -- SOUND.DRV's TABLE (s90, #299). See src/wow/wowsound.h. Checked LAST of the
 * anchored tables, after GDI's, and excluding every segment already identified:
 * its ids are 1..0x11 with small argument counts, the shape most likely to
 * collide with another module's stub before that module is learned.
 */
static WORD g_WowSoundSegment = 0;
static WORD g_WowMultimediaSegment = 0;   /* s90 #278: MMSYSTEM's stub segment */

/* GDI.EXE's TABLE -- A SEVENTH ID SPACE. See src/wow/wowgdi.h (Importance = 2):
 * GDI's exports are TAIL-JUMPS like USER's, so these are the stubs one hop
 * past the entry points, resolved by the same walk neneeds.py does:
 *    id 0x044  2 args  retstub 0x033a   DELETEDC
 *    id 0x045  2 args  retstub 0x0354   DELETEOBJECT
 *    id 0x050  4 args  retstub 0x05de   GETDEVICECAPS
 *
 * [CAUTION]: Regenerate with `tools/ne/neneeds.py` if the box's GDI.EXE ever differs.
 */
static WORD g_WowGdiSegment = 0;

/* krnl386's SECOND TABLE -- A THIRD ID SPACE, AND NOW IDENTIFIED (Importance = 3):
 * krnl386's segment 2 carries 121 stubs of its own, far-calling the same common
 * thunk with their own numbering. `f.krnl` is false for every one of them (the
 * stub is not in the segment the thunk executes in, which is what that flag
 * tests), and WowModuleOfSelector() cannot name the selector either, because
 * krnl386 loads its own segments at run time and allocates their selectors
 * through `INT 31h 0501` -- so these calls have been logged as
 * "?'s table -- a DIFFERENT id space" and answered by nobody.
 *
 * Identify the table the way USER's was identified -- by a stub in it -- but
 * with a stronger check, because for krnl386 we HAVE the file. The stub that
 * made the call sits at a known offset in seg2's file image, and its own bytes
 * must agree with the id and the return address the call carries: the id as a
 * 16-bit immediate push at retstub-8, followed by a far call at retstub-5 (the
 * generic x86 encodings, opcodes 0x68 and 0x9A -- the check below).
 * Nothing is inferred; a wrong segment cannot pass, because the file would have
 * to hold a push of THIS id at exactly the offset this call returns to. And if
 * it never matches, the dispatcher simply never engages and every seg2 call
 * stays honestly unimplemented -- the same failure mode as USER's anchor.
 */
static WORD g_WowKernel2Segment = 0;
/* seg2 ids. Numbered in THEIR OWN space -- 0xd1 here is not 0xd1 in wow32.h. */
#define WOW32K2_TASKENV     0x00d1  /* The new task's environment; args: block offset 2, selector 4 */
DWORD     g_WowPspLinear[WOW_PSP_TRACK];
WORD      g_WowPspEnvironment[WOW_PSP_TRACK];   /* last seen +0x2c, for the change log */
/* WHICH WOW32 CALL WAS THE HOST INSIDE? (session 38) (Importance = 1):
 * The WOWBOP log line is accumulated into `p` and only flushed WITH its result, so
 * a host-side crash inside a service loses the whole line -- header included. The
 * log then ends at the last call that SUCCEEDED, and the crashing one is invisible.
 * That cost a wrong first reading of the GetProfileIntA crash: the tail named 0x57,
 * stepped over and harmless, while the fault was in the call after it.
 *
 * Record the id and call site on entry. This is "the last call ENTERED", not "the
 * call in flight" -- if the run ended cleanly it names a call that completed. The
 * fatal dump says so rather than implying more than it knows.
 */
WORD g_WowLastId   = WOW_ID_NONE;
WORD g_WowLastFrom = 0;

/* A descriptor access byte names CODE iff it is a segment (S, bit 4) and executable
 * (bit 3). 0xFB -- what DOS/4GW writes -- is present/DPL3/S/code/readable/accessed.
 */
#define DPMI_ACC_IS_CODE(a)     (((a) & DPMI_ACCESS_CODE_TYPE) == DPMI_ACCESS_CODE_TYPE)
DWORD g_WowSchedSwitches = 0;
/* s92 (#306): the task launched last and not yet run -- see "(F) LAUNCH-FIRST" -- and
 * WOWEXEC (the first task resumed at (C)), whose launches keep the measured order.
 */
WORD g_WowSchedLaunchChild;
WORD g_WowSchedShell;

/* wowquiet.txt -- SILENCE THE TRACE, TO MEASURE WHAT IT COSTS (Importance = 3):
 * Session 51, from a user report that both Win16 games feel "laggy, like an
 * early 486" and that GDI redraw is visibly slow when windows overlap.
 *
 * [INFO]: THE SUSPECT IS THE INSTRUMENT, AND THIS PROJECT HAS ALREADY BEEN HERE ONCE:
 * per-line LogAppend under the device lock cost SKYROADS 24% of its delivered
 * timer ticks, and only a player's ear caught it (see HostIrqSink). On the
 * WOW path every BOP writes a multi-line block -- a Solitaire startup is 2.7 MB
 * -- so the same shape is back, on a path nobody has costed.
 *
 * [CAUTION]: The dev machine CANNOT settle this: the headless rig cannot see input lag.
 * So it is a one-file A/B for the person who can feel it -- `touch wowquiet.txt`
 * for the fast run, delete it for the instrumented one, same binary both times.
 *
 * [CAUTION]: AND IT IS NOT A FIX. If the trace is the whole difference, the answer is to
 * stop writing a kilobyte per BOP, not to ship with the trace off -- every
 * session's debugging depends on it.
 *
 * -- [CAUTION] RESULT: **REFUTED.** The user played both games with this ON -- log I/O
 * for a Solitaire run fell from 2.7 MB to 10 KB -- and reported the redraw
 * "still slow". So the FILE trace is not the cost, and the remaining suspects
 * are the per-BOP work that happens either way: this flag gates the WRITE, not
 * the ~1.3 KB of string FORMATTING each BOP still does, nor WowPspEnvironmentCheck
 * reading guest memory on every one, nor WowWinPump's PeekMessage, nor the
 * BOP round trip itself.
 * -- [CAUTION] ALSO REFUTED, same round: `SerialOut` does WriteFile + FlushFileBuffers
 * per line at 115200 baud, which would block until the bytes were physically
 * out of the UART and would have been a spectacular per-line cost. It is NOT
 * gated by this flag, so it looked like the answer. `mode` on the rig lists
 * only `CON:` -- THERE IS NO COM1 -- so g_Serial is INVALID_HANDLE_VALUE and
 * SerialOut has been a no-op all along. Measured before it was believed.
 */
/* BOPs serviced, and when the last WOWPERF line was printed. See the note at the
 * emission site.
 */
static DWORD g_WowBops = 0;
static DWORD g_WowPerfMs = 0;

/* THE DESCRIPTOR-TABLE SHADOW:
 * krnl386 wants a writable window onto the descriptor table and uses it as its own
 * allocator (observed through the shadow): it reads a free-list head through it,
 * walks links stored IN the descriptor bytes, and claims a slot by writing access
 * byte 0x0F (P=0).
 * Stock ntvdm hands it the real table. We measured (session 30 part 12) that OUR
 * LDT is not mapped into our address space, so we cannot.
 *
 * So: a shadow. A page-aligned block holding the same 8-byte encodings, handed out
 * as a writable data selector, seeded from g_Ldt[] and RECONCILED into the real LDT
 * whenever krnl386 next talks to us.
 *
 * [CAUTION]: THE HONEST LIMIT OF THIS, STATED UP FRONT. A shadow is only equivalent to the
 * real thing if every write is pushed before it matters. We push on entry to any
 * protected-mode interrupt service, which covers the sequence krnl386 actually runs
 * -- claim a slot in the table, then call INT 31h to configure it -- but it does
 * NOT cover "write a descriptor and immediately load a selector for it" with no
 * intervening call. If that happens the guest gets a stale descriptor.
 * Rather than guess whether krnl386 does that, the reconcile is LOUD: it logs every
 * entry it finds changed. So the shadow is also the instrument that answers the
 * question. If the log shows krnl386 writing descriptors we did not already know
 * about, a page-protection trap (the A0000 planar pattern) is required and this is
 * not enough. If it only ever writes free-list links, this is exactly enough.
 */
/* WOW32 dispatch state (GH #128). See src/wow/wow32.h:
 * `serviced` and `unimpl` are kept apart on purpose: a run where krnl386 gets
 * further because we answered N calls reads very differently from one where it
 * got further having been LIED to N times by the step-over path, and a single
 * total could not tell them apart.
 */
static WOW32_DOSDATA g_WowDosData;
DWORD g_Wow32Serviced = 0;
DWORD g_Wow32Unimplemented = 0;
DWORD g_Wow32Declined = 0;

DWORD g_WowSyncWrites = 0;      /* how many entries krnl386 has changed */

enum
{
    WOW_PUMP_BUDGET = 64, WOW_PUMP_BUDGET_BRIEF = 32, WOW_INPUT_WAIT_MS = 50, WOW32K2_TASKENV_ARG_BLOCK_OFFSET = 2, WOW32K2_TASKENV_ARG_BLOCK_SELECTOR = 4
};   /* the WOW32 BOP service */
#define DPMI_REPORTED_POOL_BYTES_U  0x04000000u     /* 0500h's answer: 64 MB */
enum
{
    WOW_SEGMENT_LIMIT_SLACK = 0x100
};   /* DpmiServicePmIntBody */

/* INT 31h DPMI_FN_CALL_REAL_MODE_FAR: call real-mode FAR proc: ES:DI=RMCS, CX=stack words */
static PSTR DpmiInt31CallRealModeFar(
    PSTR cursor,
    PSTR const base,
    volatile BYTE * const tib,
    const INT simulatedVector,
    const DWORD ax,
    const WORD simulatedCs,
    const WORD simulatedIp,
    DOS_MACHINE * const machine)
{
    /* This is the first PM->V86->PM round-trip. Unlike 0300's fast path (which
     * answers 21h/33h/10h host-side -- since #247 every OTHER 0300 comes
     * through here), 0301 must actually RUN
     * the client's real-mode procedure in V86: we rewrite the CONTEXT to
     * V86, push a far-return frame pointing at the DPMI_RMRET_BOP catcher,
     * run VdmRunGuest() until that BOP (servicing any INT 21h the proc makes),
     * copy the real-mode regs back into the RMCS, then restore PM.
     * 0302 IS THE SAME CALL WITH AN IRET FRAME:
     * The ONLY difference is the frame pushed on the real-mode stack:
     * 0301's procedure is entered as if FAR CALLed and ends in RETF, so
     * the frame is CS:IP; 0302's is entered as if by an INTERRUPT and ends
     * in IRET, so FLAGS is pushed underneath. Same catcher, same run loop,
     * same RMCS marshalling -- sharing the case is not a shortcut, it is
     * the actual relationship between the two services.
     * DOS/4GW's own protected-mode INT 21h handler needs 0302 the moment
     * it starts handling calls itself (session 17), because the real-mode
     * DOS entry it forwards to is an interrupt handler and returns by
     * IRET; giving it a RETF frame would leave FLAGS on the stack.
     */
    /* -- #247: AND 0300h IS THIS CALL TOO, with CS:IP = IVT[BL] (simint_vec >= 0,
     * set at the decision site above the switch). Everything below is
     * shared; the three places 0300h differs say so.
     */
    DWORD esBase = DpmiSelectorBase((WORD)VDM_REG(tib, VTIB_ES));
    volatile BYTE *registers = (volatile BYTE *)(ULONG_PTR)DpmiRmcsPointer(tib, esBase);

    if (simulatedVector >= 0)
        DpmiRmcsProbe(tib, esBase, 0, (DWORD)simulatedVector);
    else
        DpmiRmcsProbe(tib, esBase, (ax == DPMI_FN_CALL_REAL_MODE_IRET) ? 2 : 1, 0);   /* observation only */
    /* --- save the client's PM CONTEXT (full register file + MSW) --- */
    DWORD savedEax=VDM_REG(tib,VTIB_EAX);
    DWORD savedEbx=VDM_REG(tib,VTIB_EBX);
    DWORD savedEcx=VDM_REG(tib,VTIB_ECX);
    DWORD savedEdx=VDM_REG(tib,VTIB_EDX);
    DWORD savedEsi=VDM_REG(tib,VTIB_ESI);
    DWORD savedEdi=VDM_REG(tib,VTIB_EDI);
    DWORD pmBp=VDM_REG(tib,VTIB_EBP);
    DWORD pmDs=VDM_REG(tib,VTIB_DS);
    DWORD pmEs=VDM_REG(tib,VTIB_ES);
    DWORD pmFs=VDM_REG(tib,VTIB_FS);
    DWORD pmGs=VDM_REG(tib,VTIB_GS);
    DWORD pmCs=VDM_REG(tib,VTIB_CS);
    DWORD pmIp=VDM_REG(tib,VTIB_EIP);
    DWORD pmSs=VDM_REG(tib,VTIB_SS);
    DWORD pmSp=VDM_REG(tib,VTIB_ESP);
    DWORD pFlags=VDM_REG(tib,VTIB_EFLAGS);
    WORD machineStatusWord = *(volatile WORD *)(tib + VTIB_MSW);
    /* real-mode target + stack from the RMCS (default SS:SP to the code seg) */
    WORD realCs = *(volatile WORD*)(registers+RMCS_CS);
    WORD realIp = *(volatile WORD*)(registers+RMCS_IP);
    WORD realSs = *(volatile WORD*)(registers+RMCS_SS);
    WORD realSp = *(volatile WORD*)(registers+RMCS_SP);
    UINT round;
    INT done = 0;
    DWORD stackWordsToCopy = savedEcx & WORD_MASK;   /* words of PM stack to copy (DPMI 0.9) */
    RMCS_REGS rmcsRegisters;
    /* 0300h: the target is the IVT's, never the structure's. See the decision
     * site: RMCS.CS:IP is ignored on the way in and not written on the way out.
     */
    if (simulatedVector >= 0)
    {
        realCs = simulatedCs;
        realIp = simulatedIp;
    }
    /* 0301h/0302h rewrite the VDM into V86 and back for real, so they are
     * the SAME window as 0300h and share its guard -- see g_SimIntBusy.
     */
    InterlockedExchange(&g_SimIntBusy, 1);
    if (realSs == 0)
    {
        realSs = (WORD)(g_DpmiCodeBase >> PARAGRAPH_SHIFT);
        realSp = RMCS_DEFAULT_SP;
    }
    cursor = LogPut(cursor, (ax == DPMI_FN_CALL_REAL_MODE_IRET) ? " -> callRM(iret) 0x" : " -> callRM 0x");
    cursor = LogHex(cursor, realCs); cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, realIp);
    cursor = LogPut(cursor, " SS:SP=0x"); cursor = LogHex(cursor, realSs); cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, realSp);
    /* -- #247: CX WORDS OF THE PROTECTED-MODE STACK. All three services take
     * them, and all three ignored them -- a real-mode procedure that reads
     * its arguments off its stack read whatever was below our frame.
     * They go above the return frame, in order (RmcsStackPlan). The
     * PM stack top is the client's SS:(E)SP exactly as it executed
     * INT 31h: the INT is a patched BOP here, so nothing was pushed.
     *
     * [CAUTION]: A CX THAT DOES NOT FIT, OR A PM STACK WE CANNOT READ, COPIES
     * NOTHING and the call runs exactly as it did before #247, with a
     * log line saying so. A stale CX in a client that never meant to
     * pass arguments must not turn a call that has always worked into
     * a refused one.
     */
    if (stackWordsToCopy)
    {
        WORD newSp;
        WORD protectedSs = (WORD)(pmSs & WORD_MASK);
        DWORD protectedOffset = DpmiSelectorIs32(protectedSs) ? pmSp : (pmSp & WORD_MASK);
        const BYTE *source = (const BYTE *)(ULONG_PTR)(DpmiSelectorBase(protectedSs) + protectedOffset);
        cursor = LogPut(cursor, " copy=0x"); cursor = LogHex(cursor, stackWordsToCopy);
        if (RmcsStackPlan(realSp, (UINT)stackWordsToCopy, (ax == DPMI_FN_CALL_REAL_MODE_IRET) ? X86_IRET16_SIZE : X86_FAR_RETURN16_SIZE, &newSp)
            && HostReadable(source, stackWordsToCopy * 2u))
        {
            DWORD item;
            for (item = 0; item < stackWordsToCopy * X86_WORD_SIZE_U; ++item)
                *(volatile BYTE *)(ULONG_PTR)(((DWORD)realSs << PARAGRAPH_SHIFT) + (WORD)(newSp + item)) = source[item];
            realSp = newSp;
            cursor = LogPut(cursor, " words");
        }
        else
        {
            cursor = LogPut(cursor, " words NOT COPIED (do not fit below SP, or PM stack unreadable)");
        }
    }
    /* - THE POINTER ARGUMENT, BECAUSE THAT IS WHAT GOES WRONG HERE.
     * Every pointer-taking DOS call arrives as DS:DX in the RMCS, and
     * the client is responsible for having copied the string DOWN into
     * real-mode-addressable memory first. When Doom's AH=3Dh open came
     * through with an EMPTY name there was no way to tell whether the
     * client had copied nothing or we were reading the wrong place.
     * Print both the pointer and what is actually AT it.
     */
    { WORD realDs = *(volatile WORD*)(registers+RMCS_DS), realDx = *(volatile WORD*)(registers+RMCS_EDX);
      DWORD linear = ((DWORD)realDs << PARAGRAPH_SHIFT) + realDx;
      const BYTE *stackBytes = (const BYTE *)(ULONG_PTR)linear;
      cursor = LogPut(cursor, " AX=0x"); cursor = LogHex(cursor, *(volatile WORD*)(registers+0x1C));
      cursor = LogPut(cursor, " BX=0x"); cursor = LogHex(cursor, *(volatile WORD*)(registers+RMCS_EBX));
      cursor = LogPut(cursor, " CX=0x"); cursor = LogHex(cursor, *(volatile WORD*)(registers+0x18));
      cursor = LogPut(cursor, " DS:DX=0x"); cursor = LogHex(cursor, realDs); cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, realDx);
      /* And WHERE we read the RMCS from -- ES:EDI, full width. The
       * offset is masked to 16 bits below, which is right only while
       * the caller is 16-bit code; print it so a garbage RMCS can be
       * told apart from a correctly-read one that says something odd.
       */
      cursor = LogPut(cursor, " [RMCS ES:EDI=0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_ES));
      cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, VDM_REG(tib, VTIB_EDI));
      cursor = LogPut(cursor, " @0x"); cursor = LogHex(cursor, (DWORD)(ULONG_PTR)registers); cursor = LogPut(cursor, "]");
      cursor = LogPut(cursor, " @=");
      if (!HostReadable(stackBytes, 16))
          cursor = LogPut(cursor, "<unreadable>");
      else
          cursor = LogDump(cursor, stackBytes, 16); }
    cursor = LogPut(cursor, "\r\n"); LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
    /* push the return frame on the RM stack: [FLAGS] CS IP, with FLAGS
     * present only for 0302 (the procedure will IRET, not RETF).
     */
    if (ax == DPMI_FN_CALL_REAL_MODE_IRET)
    {
        realSp -= X86_WORD_SIZE;
        PokeWord(((DWORD)realSs << PARAGRAPH_SHIFT) + realSp,
                        *(volatile WORD*)(registers+RMCS_FLAGS));   /* RMCS.Flags */
    }
    realSp -= X86_WORD_SIZE;
    PokeWord(((DWORD)realSs << PARAGRAPH_SHIFT) + realSp, DOS_HDLR_SEG);   /* return CS */
    realSp -= X86_WORD_SIZE;
    PokeWord(((DWORD)realSs << PARAGRAPH_SHIFT) + realSp, DPMI_RMRET_OFF);  /* return IP */
    DpmiUnpatch();   /* restore real `CD nn` so RM ints in the proc vector natively */
    /* --- rewrite the CONTEXT to V86 with the RMCS register file --- */
    *(volatile WORD *)(tib + VTIB_MSW) = (WORD)(machineStatusWord & ~MSW_PE_BIT);  /* leave PM */
    VDM_REG(tib,VTIB_EFLAGS) = EFLAGS_VM | EFLAGS_IF | EFLAGS_RESERVED_ONE;    /* VM + IF + reserved bit-1 */
    /* #247: THE WHOLE REGISTER FILE, 32 BITS WIDE, FS AND GS INCLUDED:
     * This read the low WORD of each general register (zeroing the top
     * half a 386 real-mode handler may take as input) and set FS = GS =
     * the stack segment, where the spec loads them from the structure
     * like ES and DS. See dpmi_rmcs.h for the layout and the rule.
     */
    RmcsRead(registers, &rmcsRegisters);
    RmcsToTib(tib, &rmcsRegisters);
    /* -- 0300h: AN INTERRUPT IS ENTERED WITH THE CALLER'S STATUS FLAGS. A real
     * `INT nn` pushes FLAGS and leaves CF/ZF/SF/OF/PF/AF/DF as they were, so
     * a handler that reads one as an input (and the IRET frame below
     * carries the same word out) sees the caller's. 0301h/0302h keep their
     * measured entry state (flags clean) -- unchanged by #247.
     *
     * [CAUTION]: IF STAYS SET, AGAINST THE LETTER OF THE SPEC ("called with the
     * interrupt and trace flags clear"). Deliberately: this is the entry
     * state ZAR's INT 66h (Miles) handler was proven on in s81, with its
     * SB IRQ 5 delivered INTO this nested call -- and our own stubs do
     * not care. Clearing it is a change to make with ZAR on the rig.
     */
    if (simulatedVector >= 0)
        VDM_REG(tib,VTIB_EFLAGS) |= (DWORD)(rmcsRegisters.Flags & EFLAGS_STATUS_DF_U);  /* CF PF AF ZF SF DF OF */
    VDM_SET16(tib,VTIB_CS,realCs);
    VDM_REG(tib,VTIB_EIP)=realIp;
    VDM_SET16(tib,VTIB_SS,realSs);
    VDM_REG(tib,VTIB_ESP)=realSp;
    /* --- nested V86 run loop: run the proc until the return-BOP --- */
    for (round = 0; round < NESTED_V86_ROUNDS_MAX && !done; ++round)
    {
        LONG runStatus;
        DWORD rev;
        /* -- A DEVICE IRQ RAISED IN HERE HAD NOWHERE TO GO.
         * The cooperative delivery gate lived in the MAIN exec
         * loop, and a guest inside this nested real-mode call
         * never reaches it -- while the ASYNC injector refuses,
         * because from its side the thread is in host code
         * (`ASYNC-EARLY bail irq=05 why=0x14`). So an interrupt
         * raised while a real-mode procedure runs was simply
         * lost, and a driver waiting on ONE of them (a
         * single-cycle SB transfer raises exactly one) waited
         * for ever. ZAR's Miles init is precisely that case.
         * - Same gate, same clauses, offered every pass -- the
         *   guest is in V86 here, which is the mode this delivery
         *   was written for. The latch persists, so nothing is
         *   delivered twice and nothing is invented.
         */
        V86DeliverDeviceIrq(tib);
        InterlockedExchange(&g_NestedRm, 1);
        InterlockedExchange(&g_InExec, 1);   /* see g_NestedRm */
        rev = VdmRunGuest(tib, &runStatus);
        InterlockedExchange(&g_InExec, 0);
        InterlockedExchange(&g_NestedRm, 0);
        DWORD info = VDM_REG(tib, VTIB_EVENT_INFO) & BYTE_MASK;
        if (rev == VDM_EVENT_BOP && info == DPMI_RMRET_BOP)
        {
            done = 1;
            break;                /* proc RETF'd -> finished */
        }
        if (rev == VDM_EVENT_BOP && info == DOS_BOP_INT21)     /* INT 21h from the proc */
        {
            machine->TraceCursor = cursor;
            machine->CanTrampoline = 1;
            DosInt21(machine);
            machine->CanTrampoline = 0;
            cursor = machine->TraceCursor;
            if (machine->Trampoline)                 /* #251: AUX/PRN driver code; its */
            {
                VDM_SET16(tib, VTIB_CS, DOS_CTAB_SEG);   /* INT 14h/17h are */
                VDM_REG(tib, VTIB_EIP) = machine->Trampoline;    /* served below */
                machine->Trampoline = 0;
            }
            else
            VDM_REG(tib, VTIB_EIP) += VDM_BOP_LENGTH;    /* past the BOP -> the stub IRET */
            LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
            continue;
        }
        if (rev == VDM_EVENT_BOP && info == DPMI_CB_BOP)    /* proc far-called a 0303 callback */
        {
            INT callbackSlotAtCs = DpmiCallbackSlotAt(DPMI_CB_BASE_OFF, (WORD)VDM_REG(tib,VTIB_CS),
                                         DOS_HDLR_SEG, (WORD)VDM_REG(tib,VTIB_EIP));
            if (callbackSlotAtCs >= 0 && g_Callbacks[callbackSlotAtCs].IsUsed)
            {
                DpmiInvokeCallback(machine, tib, callbackSlotAtCs);   /* V86->PM handler->V86; sets CS:IP to the return */
                continue;
            }
            cursor = LogPut(cursor, "0301: bad cb slot\r\n"); LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
            break;
        }
        /* -- #247: OUR STUBS' BOPS, through the exec loop's own code. This is
         * what lets 0300h run IVT[BL] when it is ours (INT 16h, 1Ah, 2Fh,
         * 67h, the BIOS block), and a 0301h/0302h procedure call the BIOS,
         * instead of stopping here as an "unexpected RM event".
         *
         * [CAUTION]: ONLY FROM OUR OWN SEGMENTS -- the s78 rule: a BOP is ours by
         * where it executes, never by its number. A guest's own
         * `C4 C4 16` still falls through to the stop below.
         *
         * [CAUTION]: A RERUN (INT 16h AH=00h with no key; INT 15h AH=86h waiting)
         * costs one of this loop's 128 passes like any other event, so
         * a wait that outlasts them ends as NO-RET rather than parking
         * the thread -- the keyboard IRQ is not delivered in here (our
         * INT 09h stub is not a guest-owned vector), so a key could
         * never arrive to end an INT 16h wait anyway.
         */
        if (rev == VDM_EVENT_BOP)
        {
            DWORD bopCs = VDM_REG16(tib, VTIB_CS);
            if ((bopCs == DOS_HDLR_SEG || bopCs == DOS_CTAB_SEG)
                && V86BiosBop(tib, info, &cursor, base) != V86BOP_NONE)
            {
                if (cursor != base)
                {
                    LogAppend(LOG_PATH, base, cursor);
                    SerialOut(base, cursor);
                    cursor = base;
                }
                continue;
            }
        }
        if (rev == VDM_EVENT_IO || rev == VDM_EVENT_IO_HW || rev == VDM_EVENT_GPFAULT)
        {
            INT handled;
            HOST_LOCK();
            handled = HostTryIo(tib, &g_Bus);
            HOST_UNLOCK();
            if (handled)
                continue;
        }
        cursor = LogPut(cursor, "0301: unexpected RM event=0x"); cursor = LogHex(cursor, rev);
        cursor = LogPut(cursor, " info=0x"); cursor = LogHex(cursor, info);
        cursor = LogPut(cursor, " CS:IP=0x"); cursor = LogHex(cursor, VDM_REG16(tib,VTIB_CS));
        cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, VDM_REG16(tib,VTIB_EIP)); cursor = LogPut(cursor, "\r\n");
        LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
        break;
    }
    DpmiRepatch();   /* re-arm the BOP patch before the PM client resumes */
    /* --- copy the real-mode register file back into the RMCS ---
     * #247: all of it -- 32-bit general registers, FLAGS, ES DS FS GS -- and
     * never CS:IP/SS:SP (RmcsWrite cannot). Was: the low words, ES, DS.
     */
    TibToRmcs(tib, &rmcsRegisters, (WORD)VDM_REG(tib, VTIB_EFLAGS));
    RmcsWrite(registers, &rmcsRegisters);
    /* AND THE FLAGS, WHICH ARE THE ANSWER, NOT A DETAIL:
     * This block copied eight registers back and silently dropped the
     * ninth. Every DOS service reports failure in CF, so discarding
     * FLAGS told the client that every call SUCCEEDED -- and the client
     * passes that verdict to the application, which believes it.
     * Doom shows exactly how far a false success travels: `access()`
     * on doom2f.wad returned "error 2, file not found" with CF=0, so
     * the runtime concluded the French WAD existed, selected it, opened
     * it (open failed, also as a "success"), and then read forever from
     * the handle it never got -- 20969 iterations of `mov ah,3Fh; int
     * 21h` at obj1+0x40985 until the watchdog killed the run.
     * Watcom's own idiom makes the point: `int 21h; rcl eax,1; ror
     * eax,1` folds CF into the sign bit of EAX and branches on it, so
     * CF is not one output among many -- it is the only one it reads.
     * The DPMI 0.9 spec is explicit that 0300/0301/0302 return the
     * real-mode register state in the RMCS, and FLAGS is part of it.
     * (Written by RmcsWrite above since #247.)
     */
    /* --- restore the client's PM CONTEXT --- */
    *(volatile WORD *)(tib + VTIB_MSW) = machineStatusWord;       /* re-enter PM */
    VDM_REG(tib,VTIB_EAX)=savedEax;
    VDM_REG(tib,VTIB_EBX)=savedEbx;
    VDM_REG(tib,VTIB_ECX)=savedEcx;
    VDM_REG(tib,VTIB_EDX)=savedEdx;
    VDM_REG(tib,VTIB_ESI)=savedEsi;
    VDM_REG(tib,VTIB_EDI)=savedEdi;
    VDM_REG(tib,VTIB_EBP)=pmBp;
    VDM_SET16(tib,VTIB_DS,pmDs);
    VDM_SET16(tib,VTIB_ES,pmEs);
    VDM_SET16(tib,VTIB_FS,pmFs);
    VDM_SET16(tib,VTIB_GS,pmGs);
    VDM_SET16(tib,VTIB_CS,pmCs);
    VDM_REG(tib,VTIB_EIP)=pmIp;
    VDM_SET16(tib,VTIB_SS,pmSs);
    VDM_REG(tib,VTIB_ESP)=pmSp;
    VDM_REG(tib,VTIB_EFLAGS)=pFlags;
    VDM_REG(tib,VTIB_EFLAGS) &= ~EFLAGS_CF_U;        /* CF=0: success */
    InterlockedExchange(&g_SimIntBusy, 0);
    if (simulatedVector >= 0)
    {
        /* A 0300h that did not come back is a vector we did not service:
         * it belongs in the same STAGE2 count the unrun ones go to.
         */
        if (!done)
        {
            g_SimIntUnhandled++;
            g_SimIntVector[simulatedVector & BYTE_MASK]++;
        }
        cursor = LogPut(cursor, "0300 -> simInt 0x"); cursor = LogHexByte(cursor, (BYTE)simulatedVector);
        cursor = LogPut(cursor, " RM handler returned after ");
    }
    else
        cursor = LogPut(cursor, "0301 -> RM proc returned after ");
    cursor = LogHex(cursor, round);
    cursor = LogPut(cursor, done ? " steps (OK)" : " steps (NO-RET)");
    return cursor;
}

/* INT 31h DPMI_FN_SIMULATE_REAL_MODE_INTERRUPT: simulate real-mode interrupt: BL=int, ES:DI=RMCS */
static VOID DpmiInt31SimulateRealModeInterrupt(
    PSTR *cursorIo,
    volatile BYTE * const tib,
    DOS_MACHINE * const machine)
{
    PSTR cursor = *cursorIo;
    DWORD interruptNumber = VDM_REG(tib, VTIB_EBX) & BYTE_MASK;
    DWORD esBase = DpmiSelectorBase((WORD)VDM_REG(tib, VTIB_ES));
    volatile BYTE *registers = (volatile BYTE *)(ULONG_PTR)DpmiRmcsPointer(tib, esBase);

    DpmiRmcsProbe(tib, esBase, 0, interruptNumber);   /* observation only */
    /* -- #247: ONLY TWO WAYS IN HERE NOW. The decision site above sent every
     * vector RmcsSimIntRoute() calls SIMINT_RUN to the 0302 arm; what is
     * left is SIMINT_FAST (21h; 33h/10h while the IVT holds our stub)
     * and SIMINT_NONE (a null vector, or simintrefl_off.flag), which
     * runs nothing and is counted below, as every vector but three
     * used to be.
     */
    { WORD handlerSegment = PeekWord(IVT_SEGMENT_ADDRESS(interruptNumber)), handlerOffset = PeekWord(IVT_OFFSET_ADDRESS(interruptNumber));
      if (RmcsSimIntRoute(interruptNumber, handlerSegment, handlerOffset, g_SimIntReflect, DOS_HDLR_SEG) != SIMINT_FAST)
      {
          g_SimIntUnhandled++;
          if (interruptNumber < IVT_VECTORS)
              g_SimIntVector[interruptNumber]++;
          cursor = LogPut(cursor, " -> simInt 0x"); cursor = LogHex(cursor, interruptNumber);
          cursor = LogPut(cursor, (handlerSegment | handlerOffset) ? " NOT RUN (simintrefl_off.flag)" : " NOT RUN (null vector)");
          {
              *cursorIo = cursor;
              return;
          }
      } }
    InterlockedExchange(&g_SimIntBusy, 1);  /* no async injection in here */
    /* save the client's PM register file */
    DWORD callerEax=VDM_REG(tib,VTIB_EAX);
    DWORD callerEbx=VDM_REG(tib,VTIB_EBX);
    DWORD callerEcx=VDM_REG(tib,VTIB_ECX);
    DWORD callerEdx=VDM_REG(tib,VTIB_EDX);
    DWORD callerEsi=VDM_REG(tib,VTIB_ESI);
    DWORD callerEdi=VDM_REG(tib,VTIB_EDI);
    DWORD callerEbp=VDM_REG(tib,VTIB_EBP);
    DWORD callerDs=VDM_REG(tib,VTIB_DS);
    DWORD callerEs=VDM_REG(tib,VTIB_ES);
    DWORD callerFs=VDM_REG(tib,VTIB_FS);
    DWORD callerGs=VDM_REG(tib,VTIB_GS);
    DWORD callerSs=VDM_REG(tib,VTIB_SS);
    DWORD callerSp=VDM_REG(tib,VTIB_ESP);
    DWORD sFlags=VDM_REG(tib,VTIB_EFLAGS);
    RMCS_REGS rmcsRegisters;
    /* -- #247: THE WHOLE REGISTER FILE IN, AND FLAGS WHERE THE ANSWER LANDS.
     * This loaded the low WORD of seven registers plus ES and DS, and
     * parked SS:SP on a "host scratch stack" at 0100:FF00 -- which is
     * DOS_PSP_SEG, the FIRST PROGRAM'S OWN SEGMENT, not ours.
     *
     * [WARNING]: AND THAT IS WHERE EVERY INT 21h ERROR WENT. DosInt21() returns
     * CF/ZF by editing the FLAGS word of the caller's IRET frame at
     * SS:SP+4 -- so each 0300h INT 21h OR'd its carry into linear
     * 0x10F04, inside the guest's image, and the RMCS got the CLIENT'S
     * protected-mode flags with CF already cleared at the top of the
     * INT 31h dispatcher. 0300h INT 21h has never once reported a DOS
     * error. Same shape as the 0301 FLAGS bug below, one level deeper.
     * - Serve the call the way the dispatcher serves a PM client's own
     *   INT 21h: DosInt21SetProtectedMode(1) points DosInt21's CF/ZF at the live
     *   VTIB_EFLAGS, which is loaded with RMCS.FLAGS first -- so FLAGS comes
     *   back exactly as an IRET from the stub would return it (the
     *   caller's flags, with CF/ZF as DOS set them), and no stack is
     *   touched at all. 33h and 10h never write the frame, so for them
     *   FLAGS is the caller's own, echoed -- also what the stub's IRET
     *   gives.
     * - The full 32-bit fields, and FS/GS: see dpmi_rmcs.h.
     */
    RmcsRead(registers, &rmcsRegisters);
    RmcsToTib(tib, &rmcsRegisters);
    VDM_REG(tib,VTIB_EFLAGS) = (sFlags & HIGH_WORD_MASK_U) | rmcsRegisters.Flags;
    /* 0300 SERVICED EXACTLY ONE VECTOR, AND THAT IS THE BUG:
     * Everything except INT 21h loaded the real-mode register
     * block, did NOTHING, and copied it straight back -- so the
     * client got its own registers echoed and read that as a
     * successful call returning "nothing happened".
     * That is why DOOM HAS NO MOUSE. I_ReadMouse does not use a
     * plain `int 33h`; it calls DPMIInt(), i.e. 0300 with BL=33h,
     * for both "read buttons" (AX=3) and "read counters" (AX=0Bh).
     * Measured with the mouse instrument: raw input registered,
     * 3927 WM_INPUT packets, real deltas accumulated -- and
     * i33[3]=0, i33[B]=0. The guest was never asking, because we
     * never answered. Its INT 33h RESET works only because that
     * one goes through the PROTECTED-mode INT 33h path instead.
     * - The note at the PM-handler routing site says to widen this
     *   "when the evidence names an interrupt". It names 33h.
     *
     * [CAUTION]: Unhandled vectors are COUNTED now, not silently ignored --
     * a service that does nothing and reports success is exactly
     * what cost this one a session to find.
     */
    /* -- ...AND NOW THE EVIDENCE NAMES **INT 10h**. (session 59)
     * THIS IS WHY ZAR NEVER SHOWS A PICTURE (GH #23), and the
     * shape is identical to the mouse case above.
     *
     * [INFO]: A Watcom/DOS4GW program does not write `int 10h`; it calls
     * int86(), and int86 under an extender is 0300 with BL=10h.
     * ZAR's video driver sets its mode that way: **0x13** in the
     * register block, then INT 31h 0300 for vector 10h.
     *
     * [INFO]: MEASURED, with a breakpoint on ZAR's mode-set worker: reached
     * (`DPMI-BP HIT linear 0x03ffbf85`), and the very next line
     * of the log is `-> simInt 0x00000010`. Seven of them in a
     * 45 s run -- one VESA probe (AX=4F00) and the mode set --
     * and every one landed in the `else` below and did NOTHING.
     * `STAGE2: mode sets: none`, 0xA0000 never written, and the
     * guest sat in its demo loop rendering to a screen it had
     * never been allowed to open.
     *
     * [CAUTION]: AND THE HOST HAD BEEN COUNTING THEM ALL ALONG: `simint_unh`
     * and the per-vector histogram existed, but they are printed
     * in the periodic keylog block, which a HEADLESS run never
     * reaches. An instrument nobody can read is not an
     * instrument -- they are in STAGE2 now.
     * - Routed to the video VDD through exactly the path the
     *   PROTECTED-mode INT 10h arm uses (see `vec == 0x10` in
     *   DpmiServicePmIntBody), VideoTrapSync() included, so
     *   the two cannot drift into disagreeing about what a video
     *   BIOS call does depending on how the guest asked.
     */
    if (interruptNumber == VECTOR_DOS)
    {
        /* CF/ZF into VTIB_EFLAGS, not into an IRET frame there is none of --
         * see the #247 note above.
         */
        machine->TraceCursor = cursor;
        DosInt21SetProtectedMode(TRUE);
        DosInt21(machine);
        DosInt21SetProtectedMode(FALSE);
        cursor = machine->TraceCursor;
    }
    else if (interruptNumber == VECTOR_MOUSE)
        MouseInt33(tib, I33_SRC_SIM);
    else if (interruptNumber == VECTOR_VIDEO)
    {
        NTVDD_REGISTERS videoRegisters;
        RegistersLoad(&videoRegisters, tib);
        WORD inAx = (WORD)videoRegisters.Eax;
        WORD inCx = (WORD)videoRegisters.Ecx;
        HOST_LOCK();
        VddBusDeliverInterrupt(&g_Bus, VECTOR_VIDEO, &videoRegisters);
        HOST_UNLOCK();
        Int10WaitAfter();                     /* #226: 4F07h BL=80h */
        /* s84: ZAR stopped seeing VESA. Its VBE traffic arrives HERE
         * (int86 = 0300 BL=10h), where nothing counted it and the
         * close path skips the STAGE2 summary. Say what we answered,
         * bounded. For 4F01 the ModeAttributes word leads the block.
         */
        if ((inAx >> BYTE_SHIFT) == VIDEO_FUNCTION_VESA)
        {
            static INT vbeLogged;
            if (vbeLogged++ < 48)
            {
                CHAR vectorLine[160];
                CHAR *vectorCursor = vectorLine;
                vectorCursor = LogPut(vectorCursor, "SIMINT10 VBE in=0x"); vectorCursor = LogHex(vectorCursor, inAx);
                vectorCursor = LogPut(vectorCursor, " cx=0x"); vectorCursor = LogHex(vectorCursor, inCx);
                vectorCursor = LogPut(vectorCursor, " -> ax=0x"); vectorCursor = LogHex(vectorCursor, (WORD)videoRegisters.Eax);
                if (inAx == VIDEO_VBE_MODE_INFO_AX || inAx == VIDEO_VBE_CONTROLLER_INFO_AX)
                {
                    DWORD linear = ((DWORD)(WORD)videoRegisters.Es << PARAGRAPH_SHIFT) + (WORD)videoRegisters.Edi;
                    if (HostReadable((const VOID *)(ULONG_PTR)linear, 8))
                    {
                        vectorCursor = LogPut(vectorCursor, inAx == VIDEO_VBE_MODE_INFO_AX ? " attr=0x" : " sig/ver=");
                        vectorCursor = LogHex(vectorCursor, *(volatile DWORD *)(ULONG_PTR)linear);
                        if (inAx == VIDEO_VBE_CONTROLLER_INFO_AX)
                        {
                            vectorCursor = LogPut(vectorCursor, "/0x");
                            vectorCursor = LogHex(vectorCursor, *(volatile WORD *)(ULONG_PTR)(linear + 4));
                        }
                    }
                }
                vectorCursor = LogPut(vectorCursor, "\r\n"); LogAppend(LOG_PATH, vectorLine, vectorCursor);
            }
        }
        RegistersStore(&videoRegisters, tib);
        VideoTrapSync();   /* mode 12h: interpret; no-op in 13h */
    }
    /* -- #247: write back EVERYTHING the spec returns. This wrote AX BX CX
     * DX SI DI FLAGS and dropped BP, ES and DS -- so every INT 21h that
     * answers in ES:BX (35h get vector, 2Fh DTA, 34h InDOS, 52h List of
     * Lists) handed the caller back its OWN ES, and an INT 10h answer in
     * ES:BP (AX=1130h, the font pointer) reached the client as its own
     * ES and BP.
     * RegistersStore() was fixed for exactly this in the V86 path ("STORE
     * EVERYTHING LOAD READS"); this was the last copy of the old shape.
     *
     * [CAUTION]: CS:IP and SS:SP are NOT written -- RmcsWrite() has no way to.
     */
    TibToRmcs(tib, &rmcsRegisters, (WORD)VDM_REG(tib, VTIB_EFLAGS));
    RmcsWrite(registers, &rmcsRegisters);
    /* restore the client's PM register file */
    VDM_REG(tib,VTIB_EAX)=callerEax;
    VDM_REG(tib,VTIB_EBX)=callerEbx;
    VDM_REG(tib,VTIB_ECX)=callerEcx;
    VDM_REG(tib,VTIB_EDX)=callerEdx;
    VDM_REG(tib,VTIB_ESI)=callerEsi;
    VDM_REG(tib,VTIB_EDI)=callerEdi;
    VDM_REG(tib,VTIB_EBP)=callerEbp;
    VDM_REG(tib,VTIB_DS)=callerDs;
    VDM_REG(tib,VTIB_ES)=callerEs;
    VDM_REG(tib,VTIB_FS)=callerFs;
    VDM_REG(tib,VTIB_GS)=callerGs;
    VDM_REG(tib,VTIB_SS)=callerSs;
    VDM_REG(tib,VTIB_ESP)=callerSp;
    VDM_REG(tib,VTIB_EFLAGS)=sFlags;
    VDM_REG(tib,VTIB_EFLAGS) &= ~EFLAGS_CF_U;       /* 0300 succeeds */
    InterlockedExchange(&g_SimIntBusy, 0);
    cursor = LogPut(cursor, " -> simInt 0x"); cursor = LogHex(cursor, interruptNumber);
    *cursorIo = cursor;
}

/* INT 31h DPMI_FN_RESIZE_MEMORY: resize memory block: BX:CX = size, SI:DI = handle */
static VOID DpmiInt31ResizeMemory(PSTR *cursorIo, volatile BYTE * const tib, INT *needScanIo)
{
    PSTR cursor = *cursorIo;
    INT needScan = *needScanIo;
    /* -- #248: DPMI 0.9 CORE, AND IT WAS UNSUP. A client that grows its
     * heap by resizing (rather than allocate-copy-free) was refused, and
     * most give up there. Plan in dpmi_svc.h: in place while the new size
     * fits the pages the block already has; otherwise a new block, the old
     * contents copied, the old one freed -- and BX:CX / SI:DI name the NEW
     * address and handle, which the spec allows. Fixing up descriptors
     * that pointed at the old block is the client's job (spec).
     *
     * [CAUTION]: THE PATCH MAP MOVES WITH THE BYTES. INT sites we rewrote in the
     * old block are copied as BOPs; their pmap entries are re-keyed to
     * the new addresses, or DpmiUnpatch()/repatch() would walk freed
     * memory (0502h's s80 teardown AV) and the copies would never be
     * restored.
     */
    DWORD handle  = (VDM_REG16(tib, VTIB_ESI) << WORD_SHIFT) | VDM_REG16(tib, VTIB_EDI);
    DWORD newSize = (VDM_REG16(tib, VTIB_EBX) << WORD_SHIFT) | VDM_REG16(tib, VTIB_ECX);
    INT ownedIndex = DpmiOwnedFind(handle);
    INT plan;
    INT blockIndex;
    DWORD end = handle;
    DWORD committed;
    DWORD newHandle;
    UINT32 copy = 0;
    MEMORY_BASIC_INFORMATION memoryInfo;

    cursor = LogPut(cursor, " handle 0x"); cursor = LogHex(cursor, handle); cursor = LogPut(cursor, " size 0x"); cursor = LogHex(cursor, newSize);
    if (ownedIndex < 0)
    {
        VDM_REG(tib, VTIB_EFLAGS) |= EFLAGS_CF_U;
        VDM_SET16(tib, VTIB_EAX, DPMI_E_INVALID_HANDLE);
        cursor = LogPut(cursor, " -> REFUSED: not a live 0501h handle (8023h)");
        {
            *cursorIo = cursor;
            *needScanIo = needScan;
            return;
        }
    }
    while (VirtualQuery((LPCVOID)(ULONG_PTR)end, &memoryInfo, sizeof memoryInfo) == sizeof memoryInfo
           && (DWORD)(ULONG_PTR)memoryInfo.AllocationBase == handle && memoryInfo.RegionSize
           && memoryInfo.State == MEM_COMMIT)
        end = (DWORD)(ULONG_PTR)memoryInfo.BaseAddress + (DWORD)memoryInfo.RegionSize;
    committed = end - handle;
    plan = DpmiResizePlan(newSize, committed, &copy);
    if (plan == DPMI_RESIZE_BAD)
    {
        VDM_REG(tib, VTIB_EFLAGS) |= EFLAGS_CF_U;
        VDM_SET16(tib, VTIB_EAX, DPMI_E_INVALID_VALUE);
        cursor = LogPut(cursor, " -> REFUSED: size 0 (8021h)");
        {
            *cursorIo = cursor;
            *needScanIo = needScan;
            return;
        }
    }
    newHandle = handle;
    if (plan == DPMI_RESIZE_MOVE)
    {
        PVOID memory = VirtualAlloc(NULL, newSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        DWORD slotIndex;
        DWORD delta;
        DWORD moved = 0;
        if (!memory)
        {
            VDM_REG(tib, VTIB_EFLAGS) |= EFLAGS_CF_U;
            VDM_SET16(tib, VTIB_EAX, DPMI_E_PHYS_UNAVAIL);
            cursor = LogPut(cursor, " -> ENOMEM (block unchanged)");
            {
                *cursorIo = cursor;
                *needScanIo = needScan;
                return;
            }
        }
        newHandle = (DWORD)(ULONG_PTR)memory;
        memcpy(memory, (const VOID *)(ULONG_PTR)handle, copy);
        delta = newHandle - handle;
        for (slotIndex = 0; slotIndex < DPMI_PMAP_SLOTS; ++slotIndex)
            if (g_PatchMapLinear[slotIndex] >= handle && g_PatchMapLinear[slotIndex] < end && g_PatchMapVector[slotIndex])
            {
                PatchMapSet(g_PatchMapLinear[slotIndex] + delta, g_PatchMapVector[slotIndex]);
                g_PatchMapVector[slotIndex] = 0;            /* tombstone the old key */
                ++moved;
            }
        VirtualFree((VOID *)(ULONG_PTR)handle, 0, MEM_RELEASE);
        g_DpmiOwned[ownedIndex] = newHandle;
        if (g_LeLoadBase == handle)
            g_LeLoadBase = newHandle;
        cursor = LogPut(cursor, " -> MOVED to 0x"); cursor = LogHex(cursor, newHandle);
        cursor = LogPut(cursor, " (copied 0x"); cursor = LogHex(cursor, copy);
        cursor = LogPut(cursor, ", 0x"); cursor = LogHex(cursor, moved); cursor = LogPut(cursor, " patch sites re-keyed)");
        needScan = 1;
    }
    else
    {
        cursor = LogPut(cursor, " -> in place (committed 0x"); cursor = LogHex(cursor, committed); cursor = LogPut(cursor, ")");
    }
    for (blockIndex = 0; blockIndex < g_DpmiBlockCount; ++blockIndex)
        if (g_DpmiBlock[blockIndex].Base == handle)
        {
            g_DpmiBlock[blockIndex].Base = newHandle;
            g_DpmiBlock[blockIndex].Size = newSize;
            break;
        }
    VDM_SET16(tib, VTIB_EBX, (WORD)(newHandle >> WORD_SHIFT));
    VDM_SET16(tib, VTIB_ECX, (WORD)(newHandle & WORD_MASK));
    VDM_SET16(tib, VTIB_ESI, (WORD)(newHandle >> WORD_SHIFT));
    VDM_SET16(tib, VTIB_EDI, (WORD)(newHandle & WORD_MASK));
    *cursorIo = cursor;
    *needScanIo = needScan;
}

/* INT 31h DPMI_FN_FREE_MEMORY: free memory block SI:DI = handle */
static VOID DpmiInt31FreeMemory(PSTR *cursorIo, volatile BYTE * const tib)
{
    PSTR cursor = *cursorIo;
    DWORD handle = (VDM_REG16(tib, VTIB_ESI) << WORD_SHIFT) | VDM_REG16(tib, VTIB_EDI);

    /* -- #248: ONLY A HANDLE WE GAVE OUT. The handle is a host address,
     * and this arm VirtualFree'd whatever it was handed -- a stale or
     * corrupt handle would have released the HOST's memory. 8023h.
     */
    if (DpmiOwnedFind(handle) < 0)
    {
        VDM_REG(tib, VTIB_EFLAGS) |= EFLAGS_CF_U;
        VDM_SET16(tib, VTIB_EAX, DPMI_E_INVALID_HANDLE);
        cursor = LogPut(cursor, " handle 0x"); cursor = LogHex(cursor, handle);
        cursor = LogPut(cursor, " -> REFUSED: not a live 0501h handle (8023h)");
        {
            *cursorIo = cursor;
            return;
        }
    }
    /* -- THE PATCH MAP MUST FORGET THE BLOCK TOO. (s80) pmap holds
     * every INT site we rewrote, by address, and DpmiUnpatch()
     * / DpmiRepatch() dereference all of them on every 0301.
     * A block released here left its sites behind, so the next
     * walk read freed memory -- found as a host AV at teardown
     * (addr 0x04581e53, a site in a block Doom freed on its way
     * out). Measure the whole allocation before releasing it.
     */
    if (handle)
    {
        DWORD end = handle;
        MEMORY_BASIC_INFORMATION memoryInfo;
        DWORD slotIndex;
        while (VirtualQuery((LPCVOID)(ULONG_PTR)end, &memoryInfo, sizeof memoryInfo) == sizeof memoryInfo
               && (DWORD)(ULONG_PTR)memoryInfo.AllocationBase == handle && memoryInfo.RegionSize)
            end = (DWORD)(ULONG_PTR)memoryInfo.BaseAddress + (DWORD)memoryInfo.RegionSize;
        for (slotIndex = 0; slotIndex < DPMI_PMAP_SLOTS; ++slotIndex)
            if (g_PatchMapLinear[slotIndex] >= handle && g_PatchMapLinear[slotIndex] < end)
                g_PatchMapVector[slotIndex] = 0;
    }
    if (handle)
        VirtualFree((VOID *)(ULONG_PTR)handle, 0, MEM_RELEASE);
    /* ...and forget it in BOTH lists. g_DpmiBlock[] kept freed blocks,
     * so the code-block scan could walk released memory, and
     * teardown would have released it a second time -- by then
     * possibly someone else's allocation at the same address.
     */
    { INT index;
      for (index = 0; index < g_DpmiOwnedCount; ++index)
          if (g_DpmiOwned[index] == handle)
          {
              g_DpmiOwned[index] = g_DpmiOwned[--g_DpmiOwnedCount];
              break;
          }
      for (index = 0; index < g_DpmiBlockCount; ++index)
          if (g_DpmiBlock[index].Base == handle)
          {
              g_DpmiBlock[index] = g_DpmiBlock[--g_DpmiBlockCount];
              break;
          }
          }
    cursor = LogPut(cursor, " -> freed");
    *cursorIo = cursor;
}

/* INT 31h DPMI_FN_ALLOCATE_MEMORY: allocate memory block BX:CX bytes */
static VOID DpmiInt31AllocateMemory(
    PSTR *cursorIo,
    PSTR const base,
    volatile BYTE * const tib,
    INT *needScanIo)
{
    PSTR cursor = *cursorIo;
    INT needScan = *needScanIo;
    DWORD size = (VDM_REG16(tib, VTIB_EBX) << WORD_SHIFT) | VDM_REG16(tib, VTIB_ECX);
    PVOID memory;

    if (g_DpmiOwnedCount >= DPMI_OWNED_MAX)    /* #248: no handle to give */
    {
        VDM_REG(tib, VTIB_EFLAGS) |= EFLAGS_CF_U;
        VDM_SET16(tib, VTIB_EAX, DPMI_E_HANDLE_UNAVAIL);
        cursor = LogPut(cursor, " -> no handle (8016h)");
        {
            *cursorIo = cursor;
            *needScanIo = needScan;
            return;
        }
        }
    memory = VirtualAlloc(NULL, size ? size : 1, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!memory) { VDM_REG(tib, VTIB_EFLAGS) |= EFLAGS_CF_U;
    VDM_SET16(tib, VTIB_EAX, DPMI_E_PHYS_UNAVAIL);
                cursor = LogPut(cursor, " -> ENOMEM");
                {
                    *cursorIo = cursor;
                    *needScanIo = needScan;
                    return;
                }
                }
    g_DpmiOwned[g_DpmiOwnedCount++] = (DWORD)(ULONG_PTR)memory;   /* the client's until 0502 or exit */
    if (g_DpmiBlockCount < DPMI_MEMBLK_MAX)     /* remember it: see the flat-selector case */
    {
        INT codeIndex;
        BYTE isCode = 0;
        /* Does this allocation match one of the program's EXEC objects?
         * DOS/4GW asks for exactly the object's page-rounded virtual
         * size, so the size IS the client telling us "this block is
         * about to hold my code". See DpmiLeLearn().
         */
        for (codeIndex = 0; codeIndex < g_LeCodeCount; ++codeIndex)
            if (g_LeCodeSize[codeIndex] == ((size + PAGE_LAST_BYTE_U) & ~PAGE_LAST_BYTE_U))
            {
                isCode = 1;
                break;
            }
        g_DpmiBlock[g_DpmiBlockCount].Base = (DWORD)(ULONG_PTR)memory;
        g_DpmiBlock[g_DpmiBlockCount].Size = size ? size : 1;
        g_DpmiBlock[g_DpmiBlockCount].Code = isCode;
        ++g_DpmiBlockCount;
        if (isCode) { cursor = LogPut(cursor, " [LE CODE OBJECT]");
            /* The FIRST one is the image base every offset in a
             * disassembly is relative to. See PmWatchAddress().
             */
            if (!g_LeLoadBase)
            {
                g_LeLoadBase = (DWORD)(ULONG_PTR)memory;
                /* Flush the pending line before the resolver
                 * writes its own, or the two interleave.
                 */
                cursor = LogPut(cursor, "\r\n");
                LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
                DpmiBreakpointResolveCodeBase(g_LeLoadBase);
                DpmiBreakpointArm();   /* it may now be plantable */
            } }
    }
    { DWORD linear = (DWORD)(ULONG_PTR)memory;   /* in-process: linear = host ptr */
      VDM_SET16(tib, VTIB_EBX, linear >> WORD_SHIFT);
      VDM_SET16(tib, VTIB_ECX, linear & WORD_MASK);
      VDM_SET16(tib, VTIB_ESI, linear >> WORD_SHIFT);
      VDM_SET16(tib, VTIB_EDI, linear & WORD_MASK); /* handle=addr */
      cursor = LogPut(cursor, " -> mem 0x");
      cursor = LogHex(cursor, linear); }
    /* A new block may be the one the image is being read into, and an
     * EARLIER code block may have finished filling since we last looked.
     */
    needScan = 1;
    *cursorIo = cursor;
    *needScanIo = needScan;
}

/* INT 31h DPMI_FN_GET_FREE_MEMORY_INFO: get free memory info -> ES:DI */
static VOID DpmiInt31GetFreeMemoryInfo(PSTR *cursorIo, volatile BYTE * const tib)
{
    PSTR cursor = *cursorIo;
    /* REPORT COHERENT NUMBERS, NOT A FIELD OF -1s:
     * The 0.9 spec's 30h-byte block is
     * 00 largest available free block (BYTES)
     * 04 max unlocked page allocation   08 max locked page allocation
     * 0C total linear address space (PAGES, incl. already allocated)
     * 10 total unlocked pages           14 free pages
     * 18 total physical pages           1C free linear address space
     * 20 size of paging file/partition  24.. reserved
     * and says a host sets fields it does not support to -1. We used to
     * set ALL of them to -1 except the first, which is legal but tells a
     * client that sizes itself from the PAGE counts precisely nothing --
     * and 0xFFFFFFFF is a value a client may well arithmetic on. We do
     * know these numbers: 0501 is VirtualAlloc in our own process, so the
     * pool is what we say it is. Report it consistently in both units
     * rather than making the client guess. Reserved fields stay -1.
     */
    DWORD esBase = DpmiSelectorBase((WORD)VDM_REG(tib, VTIB_ES));
    /* ES:(E)DI by the caller's D/B bit -- see DpmiCallerOffset. Duke3D
     * passes a 32-bit stack offset here and the old 16-bit mask sent
     * the block to low memory; it then read its own uninitialised
     * buffer and refused to start.
     */
    DWORD infoAddress = esBase + DpmiCallerOffset(tib, VDM_REG(tib, VTIB_EDI));
    volatile DWORD *info = (volatile DWORD *)(ULONG_PTR)infoAddress;
    const DWORD poolBytes = DPMI_REPORTED_POOL_BYTES_U;          /* 64 MB */
    const DWORD poolPages = poolBytes >> PAGE_SHIFT;     /* 0x4000 pages */
    INT index;

    if (!MemoryReadable((ULONG_PTR)infoAddress, DPMI_FREE_INFO_SIZE))
    {
        VDM_REG(tib, VTIB_EFLAGS) |= EFLAGS_CF_U;
        VDM_SET16(tib, VTIB_EAX, DPMI_E_INVALID_VALUE);
        cursor = LogPut(cursor, " -> meminfo REFUSED: ES:EDI 0x"); cursor = LogHex(cursor, infoAddress);
        cursor = LogPut(cursor, " unreadable");
        {
            *cursorIo = cursor;
            return;
        }
    }
    for (index = 0; index < DPMI_FREE_INFO_DWORDS; ++index)
        info[index] = DPMI_FREE_INFO_UNAVAILABLE_U;
    info[DPMI_FREE_INFO_LARGEST_BLOCK] = poolBytes;                  /* largest free block, bytes */
    info[DPMI_FREE_INFO_MAX_UNLOCKED] = poolPages;                  /* max unlocked page alloc */
    info[DPMI_FREE_INFO_MAX_LOCKED] = poolPages;                  /* max locked page alloc */
    info[DPMI_FREE_INFO_LINEAR_TOTAL] = poolPages;                  /* total linear address space */
    info[DPMI_FREE_INFO_UNLOCKED] = poolPages;                  /* total unlocked pages */
    info[DPMI_FREE_INFO_FREE] = poolPages;                  /* free pages */
    info[DPMI_FREE_INFO_PHYSICAL] = poolPages;                  /* total physical pages */
    info[DPMI_FREE_INFO_LINEAR_FREE] = poolPages;                  /* free linear address space */
    info[DPMI_FREE_INFO_PAGING_FILE] = 0;                           /* no paging file */
    cursor = LogPut(cursor, " -> meminfo 64MB/0x4000 pages");
    *cursorIo = cursor;
}

/* INT 31h DPMI_FN_SET_DESCRIPTOR: set descriptor of sel BX from ES:(E)DI */
static VOID DpmiInt31SetDescriptor(PSTR *cursorIo, volatile BYTE * const tib, INT *needScanIo)
{
    PSTR cursor = *cursorIo;
    INT needScan = *needScanIo;
    INT ldtIndex = DPMI_SELECTOR_INDEX(VDM_REG16(tib, VTIB_EBX));
    DWORD esBase = DpmiSelectorBase((WORD)VDM_REG(tib, VTIB_ES));
    volatile DWORD *descriptor = (volatile DWORD *)(ULONG_PTR)DpmiRmcsPointer(tib, esBase);
    DWORD low;
    DWORD high;

    DpmiRmcsProbe(tib, esBase, 4, 0);       /* observation only */
    if (ldtIndex < 1 || ldtIndex >= DPMI_LDT_LEGACY_LIMIT) { VDM_REG(tib, VTIB_EFLAGS) |= EFLAGS_CF_U;
                                 cursor = LogPut(cursor, " -> bad sel");
                                 {
                                     *cursorIo = cursor;
                                     *needScanIo = needScan;
                                     return;
                                 }
                                 }
    low = descriptor[0];
    high = descriptor[1];
    /* Exact inverse of DpmiBuildDescriptor(). */
    g_Ldt[ldtIndex].Limit  = (low & WORD_MASK) | (((high >> WORD_SHIFT) & NIBBLE_MASK) << WORD_SHIFT);
    g_Ldt[ldtIndex].Base   = ((low >> WORD_SHIFT) & WORD_MASK) | ((high & BYTE_MASK) << WORD_SHIFT)
                      | (((high >> TOP_BYTE_SHIFT) & BYTE_MASK) << TOP_BYTE_SHIFT);
    g_Ldt[ldtIndex].Access = (BYTE)((high >> BYTE_SHIFT) & BYTE_MASK);
    g_Ldt[ldtIndex].Flags  = (BYTE)((high >> X86_DESCRIPTOR_FLAGS_SHIFT) & DPMI_DESCRIPTOR_FLAGS_MASK);
    DpmiInstall(ldtIndex);
    if (DPMI_ACC_IS_CODE(g_Ldt[ldtIndex].Access))   /* same rule as 0009 */
    {
        DpmiPatchCodeRegion(g_Ldt[ldtIndex].Base, g_Ldt[ldtIndex].Limit,
                               (g_Ldt[ldtIndex].Flags & DPMI_DESCRIPTOR_FLAG_BIG) != 0);
        needScan = 1;
    }
    cursor = LogPut(cursor, " sel 0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_EBX));
    cursor = LogPut(cursor, " <- desc 0x"); cursor = LogHex(cursor, low);
    cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, high);
    cursor = LogPut(cursor, " (base 0x"); cursor = LogHex(cursor, g_Ldt[ldtIndex].Base);
    cursor = LogPut(cursor, " limit 0x"); cursor = LogHex(cursor, g_Ldt[ldtIndex].Limit);
    cursor = LogPut(cursor, " acc 0x"); cursor = LogHexByte(cursor, g_Ldt[ldtIndex].Access);
    cursor = LogPut(cursor, " flg 0x"); cursor = LogHexByte(cursor, g_Ldt[ldtIndex].Flags); cursor = LogPut(cursor, ")");
    *cursorIo = cursor;
    *needScanIo = needScan;
}

/* INT 31h DPMI_FN_SET_PROTECTED_MODE_VECTOR: set PM interrupt vector: BL = CX:(E)DX */
static PSTR DpmiInt31SetProtectedModeVector(PSTR cursor, volatile BYTE * const tib)
{
    DWORD bl = VDM_REG(tib, VTIB_EBX) & BYTE_MASK;
    WORD handlerSelector = (WORD)VDM_REG(tib, VTIB_ECX);

    g_PmInt[bl].Selector = handlerSelector;
    /* 32-bit handler selector -> take the full 32-bit offset (GH #18 run 83);
     * a 16-bit handler keeps the word-masked offset as before
     */
    g_PmInt[bl].Offset = DpmiSelectorIs32(handlerSelector) ? VDM_REG(tib, VTIB_EDX)
                                           : VDM_REG16(tib, VTIB_EDX);
    g_PmInt[bl].Client = 1;         /* the client owns it now */
    /* A HOOK IS NOT AN INVITATION TO INTERRUPT IMMEDIATELY:
     * We latch IRQ0 and deliver it as soon as the vector exists and
     * virtual-IF is on, which in practice means THE INSTRUCTION AFTER
     * the client installs it. Real hardware cannot do that: IRQ0 runs
     * at 18.2 Hz, so the next tick is up to 55 ms -- millions of
     * instructions -- away, and no DOS program is written to survive a
     * timer interrupt arriving inside its own vector-arming loop.
     * DOS/4GW is arming a TABLE when this bites: vectors 0..8 in one
     * sequential pass, each a 4-byte default stub at 0x97:0x00, 0x04,
     * ... 0x20. Injecting on the install of vector 8 vectors into a
     * stub that is a placeholder, not the timer handler (Doom installs
     * the real one much later, in I_StartupTimer), and the run ends
     * there -- which is why `pmnoirq.flag` had to exist at all.
     * Record when the vector was armed and let a tick period pass.
     */
    if (bl == VECTOR_TIMER)
        g_PmVector8ArmedMs = GetTickCount();
    cursor = LogPut(cursor, " -> setPMvec int 0x"); cursor = LogHex(cursor, bl);
    cursor = LogPut(cursor, " = 0x"); cursor = LogHex(cursor, g_PmInt[bl].Selector);
    cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, g_PmInt[bl].Offset);
    return cursor;
}

/* INT 31h DPMI_FN_FREE_DOS_MEMORY: free DOS memory: DX = selector */
static VOID DpmiInt31FreeDosMemory(PSTR *cursorIo, volatile BYTE * const tib)
{
    PSTR cursor = *cursorIo;
    /* -- #248: TWO DEFECTS. (1) A bad selector answered success -- and
     * "bad" included any selector that was not a 0100h block: the
     * PSP selector would have freed the program's own memory. Now
     * the selector must be valid AND name a block 0100h handed out
     * (8022h otherwise), and DOS's own refusal (9: not a block)
     * comes back as DOS's error code, as the spec has it.
     * (2) The descriptor LEAKED: base/limit were zeroed but it never
     * went back on the free list, so a client that allocates and
     * frees DOS memory in a loop ran the LDT dry after ~2000 calls.
     * It is released exactly as 0001h releases one.
     */
    WORD dxSelector = (WORD)VDM_REG16(tib, VTIB_EDX);
    INT ldtIndex = DPMI_SELECTOR_INDEX(dxSelector);
    INT blockIndex;
    INT known = -1;
    INT freeError;
    WORD segment = 0;

    if (DpmiClientSelectorOk(dxSelector))
    {
        segment = (WORD)(g_Ldt[ldtIndex].Base >> PARAGRAPH_SHIFT);
        for (blockIndex = 0; blockIndex < g_DpmiDosBlockCount; ++blockIndex)
            if (g_DpmiDosBlock[blockIndex] == segment && (g_Ldt[ldtIndex].Base & PARAGRAPH_LAST_BYTE) == 0)
            {
                known = blockIndex;
                break;
            }
    }
    /* The record is capped at DPMI_DOSBLK_MAX; past that a genuine block
     * may be unrecorded, so a full record cannot refuse -- DOS decides.
     */
    if (known < 0 && !(g_DpmiDosBlockCount >= DPMI_DOSBLK_MAX && segment))
    {
        VDM_REG(tib, VTIB_EFLAGS) |= EFLAGS_CF_U;
        VDM_SET16(tib, VTIB_EAX, DPMI_E_INVALID_SEL);
        cursor = LogPut(cursor, " -> DOSfree REFUSED: not a 0100h selector (8022h)");
        {
            *cursorIo = cursor;
            return;
        }
    }
    freeError = DosMcbFree(NULL, (WORD)segment);
    if (freeError)
    {
        VDM_REG(tib, VTIB_EFLAGS) |= EFLAGS_CF_U;
        VDM_SET16(tib, VTIB_EAX, (WORD)freeError);
        cursor = LogPut(cursor, " -> DOSfree FAILED, DOS error "); cursor = LogHex(cursor, (DWORD)freeError);
        {
            *cursorIo = cursor;
            return;
        }
    }
    if (known >= 0)
        g_DpmiDosBlock[known] = g_DpmiDosBlock[--g_DpmiDosBlockCount];
    DpmiLdtRelease(ldtIndex);
    cursor = LogPut(cursor, " -> DOSfree seg=0x"); cursor = LogHex(cursor, segment);
    cursor = LogPut(cursor, " sel 0x"); cursor = LogHex(cursor, dxSelector); cursor = LogPut(cursor, " released");
    *cursorIo = cursor;
}

/* INT 31h DPMI_FN_ALLOCATE_DOS_MEMORY: allocate DOS memory: BX paras -> AX=seg, DX=sel */
static VOID DpmiInt31AllocateDosMemory(
    PSTR *cursorIo,
    PSTR const base,
    volatile BYTE * const tib,
    DOS_MACHINE * const machine)
{
    PSTR cursor = *cursorIo;
    WORD want = (WORD)VDM_REG16(tib, VTIB_EBX);
    WORD segment = 0;
    WORD maximum = 0;
    INT error = DosMcbAllocate(NULL, machine->FirstMcb, want, &segment, &maximum);
    INT ldtIndex = error ? -1 : DpmiLdtTake();   /* #248: free list first */

    if (!error && ldtIndex < 0)                   /* no descriptor: give the block back */
    {
        DosMcbFree(NULL, segment);
        VDM_REG(tib, VTIB_EFLAGS) |= EFLAGS_CF_U;
        VDM_SET16(tib, VTIB_EAX, DPMI_E_DESC_UNAVAIL);
        cursor = LogPut(cursor, " -> DOSmem: no descriptor (8011h)");
        {
            *cursorIo = cursor;
            return;
        }
    }
    if (error)
    {
        VDM_REG(tib, VTIB_EFLAGS) |= EFLAGS_CF_U;
        VDM_SET16(tib, VTIB_EAX, error ? error : DOS_ERR_INSUFFICIENT_MEMORY);   /* 8 = insufficient memory */
        VDM_SET16(tib, VTIB_EBX, maximum);                  /* largest available (paras) */
        cursor = LogPut(cursor, " -> DOSmem ENOMEM max=0x"); cursor = LogHex(cursor, maximum);
        /* [CAUTION]: AND SAY WHO HAS THE MEMORY. (s73) Heretic dies here
         * -- "I_AllocLow: DOS alloc of 1024 failed, 256 free" --
         * and "insufficient memory" on its own cannot be told
         * from a chain we corrupted. Walk it and name every
         * block's owner and size; a free tail that is not being
         * coalesced looks completely different from a guest
         * that really did take everything.
         */
        {   WORD mcbSegment = machine->FirstMcb;
        INT chainGuard = 0;
            cursor = LogPut(cursor, " chain:");
            for (;;)
            {
                volatile BYTE *mcb = (volatile BYTE *)((DWORD)mcbSegment << PARAGRAPH_SHIFT);
                BYTE signature = mcb[0];
                /* [WARNING]: #248: THIS WALK OVERRAN `report` AND KILLED THE HOST.
                 * 48 blocks x ~40 characters is ~1.9 KB on top of the
                 * line so far, in a 2 KB stack buffer: p_dpmi31's
                 * 0100h/0101h loop on the pre-#248 host fragmented the
                 * chain, hit ENOMEM, and the host died with an AV at
                 * an EIP made of ASCII (runs/s87_dpmi). Stop with room
                 * to spare and say so.
                 */
                if (cursor - base > 1600)
                {
                    cursor = LogPut(cursor, " ...(chain dump truncated)");
                    break;
                }
                WORD owner = (WORD)(mcb[DOS_MCB_OWNER] | (mcb[DOS_MCB_OWNER + 1] << BYTE_SHIFT));
                WORD size  = (WORD)(mcb[DOS_MCB_SIZE] | (mcb[DOS_MCB_SIZE + 1] << BYTE_SHIFT));
                if ((signature != DOS_MCB_MEMBER && signature != DOS_MCB_LAST) || ++chainGuard > DOS_MCB_DUMP_GUARD)
                {
                    /* [CAUTION]: AND SHOW THE BYTES WHERE THE WALK STOPPED. (s74)
                     * A chain that ends without a 'Z' is a chain
                     * somebody wrote over, and the 16 bytes say WHO:
                     * guest data, zeros, or -- as with Heretic --
                     * our own text. The theory comes after the bytes.
                     */
                    INT blockIndex;
                    cursor = LogPut(cursor, " STOP@0x"); cursor = LogHex(cursor, mcbSegment); cursor = LogPut(cursor, " bytes=[");
                    for (blockIndex = 0; blockIndex < 16; ++blockIndex)
                    {
                        cursor = LogHexByte(cursor, mcb[blockIndex]);
                        cursor = LogPut(cursor, blockIndex < 15 ? " " : "]");
                    }
                    break;
                }
                cursor = LogPut(cursor, " 0x");   cursor = LogHex(cursor, mcbSegment);
                cursor = LogPut(cursor, ":");     cursor = LogPut(cursor, owner ? "own=0x" : "FREE sz=0x");
                if (owner)
                {
                    cursor = LogHex(cursor, owner);
                    cursor = LogPut(cursor, "/sz=0x");
                }
                cursor = LogHex(cursor, size);
                if (signature == 'Z')
                    break;
                mcbSegment = (WORD)(mcbSegment + 1 + size);
            }
            cursor = LogPut(cursor, "\r\n"); }
    }
    else
    {
        g_Ldt[ldtIndex].Base = (DWORD)segment << PARAGRAPH_SHIFT;
        g_Ldt[ldtIndex].Limit = want ? ((DWORD)want << PARAGRAPH_SHIFT) - 1 : 0;
        g_Ldt[ldtIndex].Access = DPMI_ACCESS_DATA;
        g_Ldt[ldtIndex].Flags = 0;  /* data, RPL3 */
        DpmiInstall(ldtIndex);
        if (g_DpmiDosBlockCount < DPMI_DOSBLK_MAX)
            g_DpmiDosBlock[g_DpmiDosBlockCount++] = segment;
        VDM_SET16(tib, VTIB_EAX, segment);
        VDM_SET16(tib, VTIB_EDX, (WORD)DPMI_LDT_SELECTOR(ldtIndex));
        cursor = LogPut(cursor, " -> DOSmem seg=0x"); cursor = LogHex(cursor, segment);
        cursor = LogPut(cursor, " sel=0x"); cursor = LogHex(cursor, DPMI_LDT_SELECTOR(ldtIndex));
    }
    *cursorIo = cursor;
}

/* INT 31h DPMI_FN_ALLOCATE_SPECIFIC_DESCRIPTOR: allocate SPECIFIC descriptor */
static PSTR DpmiInt31AllocateSpecificDescriptor(PSTR cursor, volatile BYTE * const tib)
{
    /* DPMI 1.0. The client names the selector it wants rather
     * than taking whatever 0000 hands out -- which is exactly
     * what krnl386 does once it is managing the descriptor
     * table itself through the vendor window: it picks a free
     * slot out of its own free list and then asks us to make
     * that one real.
     *
     * [CAUTION]: "Free" has to mean free to US as well. Refusing a slot we
     * have already given to a module segment is the difference
     * between a failed allocation the client can handle and two
     * owners of one descriptor, which is unfindable later.
     */
    WORD want = (WORD)VDM_REG16(tib, VTIB_EBX);
    INT wantedIndex = DPMI_SELECTOR_INDEX(want);

    if (wantedIndex < DPMI_LDT_RESERVED || wantedIndex >= DPMI_LDT_MAX
        || g_Ldt[wantedIndex].Access != 0)
    {
        VDM_REG(tib, VTIB_EFLAGS) |= EFLAGS_CF_U;
        VDM_SET16(tib, VTIB_EAX, DPMI_E_INVALID_SEL);   /* invalid selector */
        cursor = LogPut(cursor, " -> REFUSED (reserved, out of range, or in use)");
    }
    else
    {
        g_Ldt[wantedIndex].Base = 0;
        g_Ldt[wantedIndex].Limit = 0;
        g_Ldt[wantedIndex].Access = DPMI_ACCESS_DATA;
        g_Ldt[wantedIndex].Flags = 0;
        DpmiInstall(wantedIndex);
        if (wantedIndex >= g_LdtNext)
            g_LdtNext = wantedIndex + 1;
        VDM_REG(tib, VTIB_EFLAGS) &= ~EFLAGS_CF_U;
        cursor = LogPut(cursor, " -> allocated specific sel 0x"); cursor = LogHex(cursor, want);
    }
    return cursor;
}

/* INT 31h DPMI_FN_NTVDM_COMMIT: NTVDM-private: COMMIT */
static VOID DpmiInt31NtvdmCommit(PSTR *cursorIo, volatile BYTE * const tib, INT *needScanIo)
{
    PSTR cursor = *cursorIo;
    INT needScan = *needScanIo;
    /* [INFO]: THE SYNC POINT, AND IT IS THE GUEST TELLING US.
     * Every call arrives the same way (observed): descriptors
     * already written through the vendor window, BX = the
     * first selector (RPL 3, TI set), CX = how many,
     * i.e. "I have modified CX descriptors starting at BX --
     * make them real". Stock ntvdm needs this for the same
     * reason we do: writing the table's memory is not enough,
     * the kernel has to be told.
     * So this REPLACES guesswork with the guest's own
     * declaration. The blanket reconcile on interrupt entry
     * stays as a backstop, and if it ever reports anything
     * after this is implemented, that is a call site issuing a
     * descriptor write WITHOUT a commit -- worth knowing.
     */
    WORD firstSelector = (WORD)VDM_REG16(tib, VTIB_EBX);
    DWORD selectorCount = VDM_REG16(tib, VTIB_ECX);
    INT firstIndex = DPMI_SELECTOR_INDEX(firstSelector);
    INT done = 0;

    if (!selectorCount)
        selectorCount = 1;
    cursor = LogPut(cursor, " commit "); cursor = LogHex(cursor, selectorCount);
    cursor = LogPut(cursor, " from sel 0x"); cursor = LogHex(cursor, firstSelector);
    if (g_WowShadow)
    {
        DWORD index2;
        for (index2 = 0; index2 < selectorCount; ++index2)
        {
            INT ldtEntry = firstIndex + (INT)index2;
            const DWORD *entry;
            if (ldtEntry < 1 || ldtEntry >= WOW_SHADOW_ENTRIES)
                continue;
            entry = (const DWORD *)(g_WowShadow + ldtEntry * X86_DESCRIPTOR_SIZE);
            /* A RESERVED INDEX IS NOT A READ-ONLY ONE (Importance = 2):
             * This used to `continue` for every index below
             * DPMI_LDT_RESERVED, so a commit naming selector 0x17
             * (index 2) installed NOTHING and said so -- "installed
             * 00000000" -- with no other complaint.
             *
             * [CAUTION]: AND krnl386 STAGES THROUGH THOSE SELECTORS. Session
             * 34: it re-bases 0x17, reads SYSTEM.DRV's segment 1 +
             * relocation records into 0x17:0000, then walks the
             * records through a selector it bases at the segment's
             * real home. With the re-base dropped, the read landed
             * at the STALE base and the walk read whatever was at
             * the new one -- a read through ES with bx=0x38a from a
             * garbage module index, #GP, and krnl386's own handler
             * turning that into ExitKernelThunk(1). The module
             * database was provably fine (`NE`, cseg=2, cmod=1,
             * modtab=0x7c); only the image was in the wrong place.
             * The reason the guard exists is the ACCESS byte, not the
             * base: a client that retypes the initial DS/SS to code
             * faults on its next stack write (GH #18 run 69). So
             * take the base and limit, and let DpmiInstall() apply
             * its existing idx-2/3 force-to-writable-data rule --
             * which is where that rule already lives. Index 0 is the
             * null selector and stays untouchable.
             */
            if (ldtEntry < DPMI_LDT_RESERVED)
            {
                /* [CAUTION]: ONLY A PRESENT DESCRIPTOR IS A RE-BASE. An empty
                 * shadow slot commits as base=0 access=0, and honouring
                 * THAT would hand the initial DS a base of zero -- the
                 * log shows two such commits (idx 1 and idx 2) before
                 * krnl386 has written anything real. A blank entry is
                 * not the guest asking for anything; it is the guest
                 * having asked for a slot. Require the present bit.
                 */
                if (!(((entry[1] >> BYTE_SHIFT) & BYTE_MASK) & X86_DESCRIPTOR_PRESENT))
                    continue;
                g_Ldt[ldtEntry].Base   = (entry[1] & X86_DESCRIPTOR_BASE_HIGH_U)
                                | ((entry[1] & BYTE_MASK_U) << WORD_SHIFT) | (entry[0] >> WORD_SHIFT);
                g_Ldt[ldtEntry].Limit  = (entry[0] & WORD_MASK_U) | (entry[1] & X86_DESCRIPTOR_LIMIT_HIGH_U);
                g_Ldt[ldtEntry].Access = (BYTE)((entry[1] >> BYTE_SHIFT) & BYTE_MASK);
                g_Ldt[ldtEntry].Flags  = (BYTE)((entry[1] >> X86_DESCRIPTOR_FLAGS_SHIFT) & DPMI_DESCRIPTOR_FLAGS_MASK);
                DpmiInstall(ldtEntry);       /* force-types idx 2 and 3 */
                ++done;
                continue;
            }
            VdmInstallLdtEntries((WORD)DPMI_LDT_SELECTOR(ldtEntry), entry[0], entry[1],
                                (WORD)DPMI_LDT_SELECTOR(ldtEntry), entry[0], entry[1]);
            g_Ldt[ldtEntry].Base   = (entry[1] & X86_DESCRIPTOR_BASE_HIGH_U)
                            | ((entry[1] & BYTE_MASK_U) << WORD_SHIFT) | (entry[0] >> WORD_SHIFT);
            g_Ldt[ldtEntry].Limit  = (entry[0] & WORD_MASK_U) | (entry[1] & X86_DESCRIPTOR_LIMIT_HIGH_U);
            g_Ldt[ldtEntry].Access = (BYTE)((entry[1] >> BYTE_SHIFT) & BYTE_MASK);
            g_Ldt[ldtEntry].Flags  = (BYTE)((entry[1] >> X86_DESCRIPTOR_FLAGS_SHIFT) & DPMI_DESCRIPTOR_FLAGS_MASK);
            if (ldtEntry >= g_LdtNext)
                g_LdtNext = ldtEntry + 1;
            ++done;
        }
    }
    VDM_REG(tib, VTIB_EFLAGS) &= ~EFLAGS_CF_U;
    cursor = LogPut(cursor, " -> installed "); cursor = LogHex(cursor, (DWORD)done);
    if (done)
    {
        cursor = LogPut(cursor, " (idx 0x"); cursor = LogHex(cursor, (DWORD)firstIndex);
        cursor = LogPut(cursor, " base=0x"); cursor = LogHex(cursor, g_Ldt[firstIndex].Base);
        cursor = LogPut(cursor, " acc=0x");  cursor = LogHexByte(cursor, g_Ldt[firstIndex].Access);
        cursor = LogPut(cursor, ")");
    }
    /* AND PATCH ITS INT SITES, EXACTLY AS 0009 AND 000C DO (Importance = 3):
     * Those two apply the rule "the client naming a region CODE is
     * the only notice we get that something it just loaded is about
     * to be executed" -- and this path did not, which made it a hole
     * exactly the size of krnl386.
     *
     * [CAUTION]: WHY IT IS FATAL. Our INT->BOP scan runs over the image we
     * placed in conventional memory. krnl386 then LOADS ITS OWN
     * SEGMENTS: it copies segment 1 to linear 0x20760, relocates it,
     * writes a code descriptor through the vendor window and commits
     * it HERE (`04F2 ... base=0x00020760 acc=0xfb`). That copy is
     * memory we have never scanned, so every `CD nn` in it is raw --
     * and a PM guest cannot reach the IVT, so the first one silently
     * kills the VDM. WOW BOPs still work in the copy because `C4 C4 nn`
     * is literal in the binary, which is why the run looked healthy
     * right up to the moment it stopped.
     *
     * MEASURED SHAPE OF THE STALL: the whole logged run is 282 ms
     * (read off the async thread's `ms=` stamps), and at the end BOTH
     * auxiliary threads fall silent in the same instant -- no watchdog
     * tick, no async tick, no further PM event. That is a process that
     * stopped, not a guest that is merely spinning, and session 31's
     * "the watchdog logs one sample and stops" was the instrument
     * reporting it correctly rather than failing.
     */
    if (done)
    {
        DWORD index2;
        for (index2 = 0; index2 < selectorCount; ++index2)
        {
            INT ldtEntry = firstIndex + (INT)index2;
            /* [CAUTION]: THE SAME GUARD, AND THE SAME BUG. The commit loop above
             * used to skip low indices; this one did too, and fixing
             * only the first half installs a CODE descriptor whose
             * `CD nn` sites are still raw. Measured: krnl386 loads
             * SYSTEM.DRV into the block WOW32 0xb8 VirtualAlloc'd and
             * re-bases the INITIAL CS (index 1, selector 0x0f) over it
             * -- `04F2 ... installed idx 1 base=0x03b30000 acc=0xfb` --
             * then executes there. The first INT 21h in that code
             * faulted at 0x000f:0x01f7 with #GP err=0x010a, whose IDT
             * bit and index 0x21 name the vector exactly. A region the
             * client declares CODE must be patched wherever it lands.
             */
            if (ldtEntry < 1 || ldtEntry >= WOW_SHADOW_ENTRIES)
                continue;
            if (!DPMI_ACC_IS_CODE(g_Ldt[ldtEntry].Access))
                continue;
            DpmiPatchCodeRegion(g_Ldt[ldtEntry].Base, g_Ldt[ldtEntry].Limit,
                                   (g_Ldt[ldtEntry].Flags & DPMI_DESCRIPTOR_FLAG_BIG) != 0);
            needScan = 1;
            cursor = LogPut(cursor, " [CODE sel 0x"); cursor = LogHex(cursor, (DWORD)DPMI_LDT_SELECTOR(ldtEntry));
            cursor = LogPut(cursor, " base=0x");   cursor = LogHex(cursor, g_Ldt[ldtEntry].Base);
            cursor = LogPut(cursor, " limit=0x");  cursor = LogHex(cursor, g_Ldt[ldtEntry].Limit);
            cursor = LogPut(cursor, " -> INT sites patched]");
            /* [INFO]: Is this one of krnl386's OWN segments? Match the
             * LIMIT against the segment's length rounded up, and
             * hand the base to any seg-relative breakpoint
             * waiting for it.
             *
             * [CAUTION]: THE ROUNDING IS NOT A PARAGRAPH. This window was
             * `[ln-1, ln+16)` on the evidence of segments 1 and 3
             * (0xd7fa -> 0xd7ff, 0x1278 -> 0x127f) -- and
             * SEGMENT 2 ROUNDS TO 0x100: 0x3ee2 -> 0x3eff, which
             * is ln+0x1d and fell outside. So seg 2 was never
             * identified, and it is the segment the module-load
             * path lives in. Widened to the next 0x100 boundary,
             * which is the largest round-up observed.
             *
             * [CAUTION]: Widening a match is a licence to mis-identify, so
             * check it stays unambiguous: krnl386's four segment
             * lengths are 0xd7fa, 0x3ee2, 0x1278, 0x1ba2 and the
             * windows [ln-1, ln+0x100) do not overlap. The delta
             * is logged, so a fit that stops looking like a
             * round-up is visible rather than assumed.
             */
            if (g_WowModuleCount)
            {
                INT neSegment;
                for (neSegment = 0; neSegment < (INT)g_WowModule[0].SegmentCount &&
                             neSegment < WOW_PMBASE_MAX; ++neSegment)
                {
                    DWORD segmentLength = g_WowModule[0].Segments[neSegment].Length;
                    if (!segmentLength || g_Ldt[ldtEntry].Limit < segmentLength - 1 ||
                        g_Ldt[ldtEntry].Limit >= segmentLength + WOW_SEGMENT_LIMIT_SLACK)
                        continue;
                    if (g_WowPmBase[neSegment] != g_Ldt[ldtEntry].Base)
                    {
                        g_WowPmBase[neSegment] = g_Ldt[ldtEntry].Base;
                        /* Every segment, not just seg 1 -- the
                         * module-load path is in seg 2.
                         */
                        DpmiBreakpointResolveSegment((UINT)neSegment + 1,
                                            g_Ldt[ldtEntry].Base);
                    }
                    if (neSegment == 0)
                        g_WowPmSegment1Base = g_Ldt[ldtEntry].Base;
                    cursor = LogPut(cursor, " [= krnl386 seg ");
                    cursor = LogHex(cursor, (DWORD)(neSegment + 1));
                    cursor = LogPut(cursor, " len=0x"); cursor = LogHex(cursor, segmentLength);
                    cursor = LogPut(cursor, " limit-len=0x");
                    cursor = LogHex(cursor, g_Ldt[ldtEntry].Limit - segmentLength);
                    cursor = LogPut(cursor, "]");
                    break;
                }
            }
        }
    }
    *cursorIo = cursor;
    *needScanIo = needScan;
}

/* INT 31h DPMI_FN_NTVDM_ALLOCATE: NTVDM-private: allocate */
static VOID DpmiInt31NtvdmAllocate(PSTR *cursorIo, volatile BYTE * const tib)
{
    PSTR cursor = *cursorIo;
    /* Private twin of 0000. Observed: krnl386's descriptor
     * allocations arrive here, while its Get Descriptor (0x0B)
     * requests never reach us at all -- it reads those straight
     * out of the window. So the private family is a fast
     * path for descriptor management: reads come from the
     * window, and only the operations that must reach the host
     * become calls. Same contract as 0000: CX descriptors,
     * base selector in AX.
     */
    DWORD cxValue = VDM_REG16(tib, VTIB_ECX);
    WORD baseSelector;
    DWORD descriptorIndex;

    if (!cxValue)
        cxValue = 1;
    if (g_LdtNext + (INT)cxValue > DPMI_LDT_MAX)
    {
        VDM_REG(tib, VTIB_EFLAGS) |= EFLAGS_CF_U;
        VDM_SET16(tib, VTIB_EAX, DPMI_E_DESC_UNAVAIL);
        cursor = LogPut(cursor, " -> ENOMEM");
        {
            *cursorIo = cursor;
            return;
        }
    }
    baseSelector = (WORD)DPMI_LDT_SELECTOR(g_LdtNext);
    for (descriptorIndex = 0; descriptorIndex < cxValue; ++descriptorIndex)
    {
        INT ldtEntry = g_LdtNext++;
        g_Ldt[ldtEntry].Base = 0;
        g_Ldt[ldtEntry].Limit = 0;
        g_Ldt[ldtEntry].Access = DPMI_ACCESS_DATA;
        g_Ldt[ldtEntry].Flags = 0;
        DpmiInstall(ldtEntry);
    }
    VDM_REG(tib, VTIB_EFLAGS) &= ~EFLAGS_CF_U;
    VDM_SET16(tib, VTIB_EAX, baseSelector);
    cursor = LogPut(cursor, " -> private-alloc "); cursor = LogHex(cursor, cxValue);
    cursor = LogPut(cursor, " sel 0x"); cursor = LogHex(cursor, baseSelector);
    *cursorIo = cursor;
}

/* INT 31h DPMI_FN_FREE_DESCRIPTOR: free descriptor BX */
static VOID DpmiInt31FreeDescriptor(PSTR *cursorIo, volatile BYTE * const tib)
{
    PSTR cursor = *cursorIo;
    WORD firstSelector = (WORD)VDM_REG16(tib, VTIB_EBX);
    INT ldtIndex = DPMI_SELECTOR_INDEX(firstSelector);
    /* Never recycle OUR OWN selectors: the mode-switch trio, the PSP
     * and environment descriptors, and the stubs/trampoline. A client
     * is entitled to free anything it was given, but it was not given
     * these, and handing one back out later would pull the floor up.
     */
    /* [CAUTION]: idx >= 1 GUARDS THE WHOLE TEST, not just the range: the
     * host selectors below are 0 until first used, so selector 0
     * "matched" g_PmReturnSelector and the null free answered success
     * (#248, measured by p_dpmi31 on the first cut).
     */
    INT reserved = ldtIndex >= 1
        && (ldtIndex < DPMI_LDT_RESERVED
            || firstSelector == g_DpmiHandlerSelector || firstSelector == g_PmReturnSelector
            || firstSelector == g_DpmiFaultSelector || firstSelector == g_DpmiFaultCodeSelector);

    /* -- #248: A SELECTOR THAT WAS NEVER THE CLIENT'S IS 8022h, NOT
     * SUCCESS. This arm used to fall through to "kept" with CF=0 for
     * a null, GDT, out-of-range, unallocated or already-freed
     * selector alike -- a double free read as a free. Our OWN
     * selectors (`reserved`) still answer success: the client was
     * handed the initial CS/DS/SS, PSP and environment selectors as
     * its own and may free them; we just never recycle them. [CAUTION] Under
     * WOW only the range is checked (dpmi_svc.h), and a double free
     * is then krnl386's own business.
     */
    if (!reserved && !DpmiClientSelectorOk(firstSelector))
    {
        VDM_REG(tib, VTIB_EFLAGS) |= EFLAGS_CF_U;
        VDM_SET16(tib, VTIB_EAX, DPMI_E_INVALID_SEL);
        cursor = LogPut(cursor, " -> free REFUSED: invalid selector (8022h)");
        {
            *cursorIo = cursor;
            return;
        }
    }
    if (ldtIndex >= 1 && ldtIndex < DPMI_LDT_MAX && !reserved
        && g_Ldt[ldtIndex].Access != 0 && g_LdtFreeCount < DPMI_LDT_MAX)
    {
        INT duplicate;
        INT isDuplicate = 0;
        for (duplicate = 0; duplicate < g_LdtFreeCount; ++duplicate)   /* refuse a double free */
            if (g_LdtFree[duplicate] == (WORD)ldtIndex)
            {
                isDuplicate = 1;
                break;
            }
        if (!isDuplicate)
        {
            DpmiLdtRelease(ldtIndex);          /* not present, on the free list */
            /* If this was a 0002 mapping, stop claiming it. */
            DpmiSegmentToDescriptorForget(firstSelector);
            cursor = LogPut(cursor, " -> freed sel 0x"); cursor = LogHex(cursor, firstSelector);
            cursor = LogPut(cursor, " ("); cursor = LogHex(cursor, (DWORD)g_LdtFreeCount);
            cursor = LogPut(cursor, " on the free list)");
            {
                *cursorIo = cursor;
                return;
            }
        }
    }
    cursor = LogPut(cursor, " -> free (kept: reserved or not allocated)");
    *cursorIo = cursor;
}

/* INT 31h DPMI_FN_ALLOCATE_DESCRIPTORS: allocate CX descriptors */
static VOID DpmiInt31AllocateDescriptors(PSTR *cursorIo, volatile BYTE * const tib)
{
    PSTR cursor = *cursorIo;
    DWORD cx = VDM_REG16(tib, VTIB_ECX);
    DWORD index;
    WORD firstAllocatedSelector;

    if (cx == 0)
        cx = 1;
    /* Recycle first, and only for CX==1: 0000 promises CONTIGUOUS
     * selectors (that is what 0003's increment is for), and a free
     * list cannot promise that. Single allocations are the whole of
     * what real clients ask for in bulk.
     */
    if (cx == 1 && g_LdtFreeCount > 0)
    {
        INT ldtIndex = g_LdtFree[--g_LdtFreeCount];
        g_Ldt[ldtIndex].Base = 0;
        g_Ldt[ldtIndex].Limit = 0;
        g_Ldt[ldtIndex].Access = DPMI_ACCESS_DATA;
        g_Ldt[ldtIndex].Flags = 0;
        DpmiInstall(ldtIndex);
        VDM_SET16(tib, VTIB_EAX, (WORD)DPMI_LDT_SELECTOR(ldtIndex));
        cursor = LogPut(cursor, " -> sel 0x"); cursor = LogHex(cursor, DPMI_LDT_SELECTOR(ldtIndex));
        cursor = LogPut(cursor, " (recycled)");
        {
            *cursorIo = cursor;
            return;
        }
    }
    if (g_LdtNext + (INT)cx > DPMI_LDT_MAX)        /* out of descriptors */
    {
        VDM_REG(tib, VTIB_EFLAGS) |= EFLAGS_CF_U;
        VDM_SET16(tib, VTIB_EAX, DPMI_E_DESC_UNAVAIL);
        cursor = LogPut(cursor, " -> ENOMEM");
        {
            *cursorIo = cursor;
            return;
        }
    }
    firstAllocatedSelector = (WORD)DPMI_LDT_SELECTOR(g_LdtNext);
    for (index = 0; index < cx; ++index)
    {
        INT ldtIndex = g_LdtNext++;
        g_Ldt[ldtIndex].Base = 0;
        g_Ldt[ldtIndex].Limit = 0;
        g_Ldt[ldtIndex].Access = DPMI_ACCESS_DATA;
        g_Ldt[ldtIndex].Flags = 0;  /* data, RPL3 */
        DpmiInstall(ldtIndex);
    }
    VDM_SET16(tib, VTIB_EAX, firstAllocatedSelector);
    cursor = LogPut(cursor, " -> sel 0x"); cursor = LogHex(cursor, firstAllocatedSelector);
    *cursorIo = cursor;
}

/* A GDI.EXE id: service it through WowGdiCall, step past the BOP and log the result. */
static INT Wow32ServiceGdi(
    PSTR *cursorIo,
    PSTR const base,
    WOW32_FRAME *frame,
    volatile BYTE * const tib,
    INT *exitCodeOut)
{
    PSTR cursor = *cursorIo;

    if (!frame->IsKernel && g_WowGdiSegment && frame->StubSegment == g_WowGdiSegment)
    {
        CHAR note[320];
        if (WowGdiCall(frame, note, sizeof note))
        {
            ++g_Wow32Serviced;
            VDM_REG(tib, VTIB_EIP) += VDM_BOP_LENGTH;
            cursor = LogPut(cursor, " -> SERVICED (GDI), returned 0x");
            cursor = LogHex(cursor, frame->Result);
            if (note[0])
            {
                cursor = LogPut(cursor, " -- ");
                cursor = LogPut(cursor, note);
            }
            cursor = LogPut(cursor, "\r\n");
            /* [WARNING]: s89: AND GDI'S ENUMERATIONS RUN. This branch never acted on
             * f.enumreq -- only USER's did -- so LineDDA and
             * EnumFontFamilies armed a walk that never started: Charmap
             * got "60 fonts" and not one callback, i.e. an empty list.
             * Same first step as USER's branch.
             */
            if (frame->IsEnumerationRequested)
            {
                CHAR enumNote[256];
                WORD  enumCallbackSelector = WowCallbackSelector();
                DWORD enumStackBase  = DpmiSelectorBase(
                    (WORD)VDM_REG16(tib, VTIB_SS));
                enumNote[0] = 0;
                WowEnumStep(tib, enumStackBase, enumCallbackSelector, WOWENUM_FIRST, 0, enumNote, sizeof enumNote);
                cursor = LogPut(cursor, "WOWENUM: "); cursor = LogPut(cursor, enumNote);
                cursor = LogPut(cursor, "\r\n");
            }
            WowLogFlush(base, &cursor);
            {
                *cursorIo = cursor;
                *exitCodeOut = 1;
                return HOST_FLOW_RETURN;
            }
        }
    }
    *cursorIo = cursor;
    return HOST_FLOW_NEXT;
}

/* A COMMDLG.DLL id: service it, step past the BOP and log the result. */
static INT Wow32ServiceCommonDialog(
    PSTR *cursorIo,
    PSTR const base,
    WOW32_FRAME *frame,
    volatile BYTE * const tib,
    INT *exitCodeOut)
{
    PSTR cursor = *cursorIo;

    if (!frame->IsKernel && g_WowCommonDialogSegment && frame->StubSegment == g_WowCommonDialogSegment)
    {
        CHAR note[416];
        WowWinPump(WOW_PUMP_BUDGET_BRIEF);
        /* [CAUTION]: Same reason as ShellAbout: a modal service does not return
         * until a human dismisses it, and the SERVICED line is
         * written afterwards. Say it before it blocks, or the log
         * looks like a run that stopped at the call before.
         */
        if (frame->Id == WOWCDLG_GETOPENFILENAME
            || frame->Id == WOWCDLG_GETSAVEFILENAME
            || frame->Id == WOWCDLG_CHOOSEFONT
            || frame->Id == WOWCDLG_CHOOSECOLOR)
        {
            cursor = LogPut(cursor, "\n     WOWCOMMDLG: this common dialog is MODAL --"
                        " the VDM stops here until it is dismissed;"
                        " the SERVICED line follows when it is\r\n");
            WowLogFlush(base, &cursor);
        }
        if (WowCommdlgCall(frame, note, sizeof note))
        {
            ++g_Wow32Serviced;
            VDM_REG(tib, VTIB_EIP) += VDM_BOP_LENGTH;
            cursor = LogPut(cursor, " -> SERVICED (COMMDLG), returned 0x");
            cursor = LogHex(cursor, frame->Result);
            if (note[0])
            {
                cursor = LogPut(cursor, " -- ");
                cursor = LogPut(cursor, note);
            }
            cursor = LogPut(cursor, "\r\n");
            WowLogFlush(base, &cursor);
            {
                *cursorIo = cursor;
                *exitCodeOut = 1;
                return HOST_FLOW_RETURN;
            }
        }
    }
    *cursorIo = cursor;
    return HOST_FLOW_NEXT;
}

/* A SHELL.DLL id: service it, step past the BOP and log the result. */
static INT Wow32ServiceShell(
    PSTR *cursorIo,
    PSTR const base,
    WOW32_FRAME *frame,
    volatile BYTE * const tib,
    INT *exitCodeOut)
{
    PSTR cursor = *cursorIo;

    if (!frame->IsKernel && g_WowShellSegment && frame->StubSegment == g_WowShellSegment)
    {
        CHAR note[320];
        /* The About box runs a modal loop on this thread, so drain
         * what is already queued for the guest's windows first --
         * same reason the USER branch pumps.
         */
        WowWinPump(WOW_PUMP_BUDGET_BRIEF);
        /* [CAUTION]: SAY IT BEFORE IT BLOCKS, NOT AFTER (Importance = 1):
         * A modal service does not return until a human dismisses
         * it, and the "SERVICED" line is written afterwards -- so
         * while the box is up the log's last line is the LoadIcon
         * before it, and a reader collecting the log at that moment
         * sees a run that appears to have STOPPED at a call that
         * completed fine. Measured, on the first run that opened
         * one. This line is written and flushed first, so the log
         * says what the host is waiting for while it waits.
         */
        if (frame->Id == WOWSHELL_SHELLABOUT)
        {
            cursor = LogPut(cursor, "\n     WOWSHELL: ShellAbout is MODAL -- the VDM"
                        " stops here until the box is dismissed; the"
                        " SERVICED line follows when it is\r\n");
            WowLogFlush(base, &cursor);
        }
        if (WowShellCall(frame, note, sizeof note))
        {
            ++g_Wow32Serviced;
            VDM_REG(tib, VTIB_EIP) += VDM_BOP_LENGTH;
            cursor = LogPut(cursor, " -> SERVICED (SHELL), returned 0x");
            cursor = LogHex(cursor, frame->Result);
            if (note[0])
            {
                cursor = LogPut(cursor, " -- ");
                cursor = LogPut(cursor, note);
            }
            cursor = LogPut(cursor, "\r\n");
            WowLogFlush(base, &cursor);
            {
                *cursorIo = cursor;
                *exitCodeOut = 1;
                return HOST_FLOW_RETURN;
            }
        }
    }
    *cursorIo = cursor;
    return HOST_FLOW_NEXT;
}

/* USER serviced the call: step past the BOP and log the result -- and when the answer is a 16-bit callback (a window procedure and its message), set that call up to run in the client. */
static INT Wow32FinishUserCall(
    PSTR *cursorIo,
    PSTR const base,
    WOW32_FRAME *frame,
    CHAR *note,
    volatile BYTE * const tib,
    INT *exitCodeOut)
{
    PSTR cursor = *cursorIo;

    if (WowUserCall(frame, note, sizeof note))
    {
        ++g_Wow32Serviced;
        VDM_REG(tib, VTIB_EIP) += VDM_BOP_LENGTH;
        cursor = LogPut(cursor, " -> SERVICED (USER), returned 0x");
        cursor = LogHex(cursor, frame->Result);
        if (note[0])
        {
            cursor = LogPut(cursor, " -- ");
            cursor = LogPut(cursor, note);
        }
        cursor = LogPut(cursor, "\r\n");
        /* AND NOW THE OTHER DIRECTION. (session 40) (Importance = 5):
         * The answer is already in the return hole and EIP is
         * already past the BOP, so what gets parked here is the
         * guest EXACTLY as it will be resumed -- see the ordering
         * note in WowCallEnter. Everything after this point in
         * the run belongs to the 16-bit procedure until its
         * `retf` reaches our stub.
         */
        if (frame->CallbackProcedure)
        {
            WORD  callbackSelector = WowCallbackSelector();
            DWORD callbackStackBase = DpmiSelectorBase(
                (WORD)VDM_REG16(tib, VTIB_SS));
            INT argumentIndex;
            INT callbackAbsent = 0;
            cursor = LogPut(cursor, "WOWCALL: -> 0x");
            cursor = LogHex(cursor, frame->CallbackProcedure >> WORD_SHIFT);
            cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, frame->CallbackProcedure & WORD_MASK);
            cursor = LogPut(cursor, "(");
            for (argumentIndex = 0; argumentIndex < frame->CallbackArgumentCount; ++argumentIndex)
            {
                if (argumentIndex)
                    cursor = LogPut(cursor, " ");
                cursor = LogHex(cursor, frame->CallbackArguments[argumentIndex]);
            }
            cursor = LogPut(cursor, ") ds=0x"); cursor = LogHex(cursor, frame->CallbackDataSelector);
            if (frame->CallbackMessage)
            {
                cursor = LogPut(cursor, " [hwnd=0x"); cursor = LogHex(cursor, frame->CallbackWindow);
                cursor = LogPut(cursor, " msg=0x"); cursor = LogHex(cursor, frame->CallbackMessage);
                cursor = LogPut(cursor, "]");
            }
            cursor = LogPut(cursor, " ss=0x");
            cursor = LogHex(cursor, VDM_REG16(tib, VTIB_SS));
            cursor = LogPut(cursor, ":0x");
            cursor = LogHex(cursor, VDM_REG16(tib, VTIB_ESP));
            cursor = LogPut(cursor, " ret=0x"); cursor = LogHex(cursor, callbackSelector);
            cursor = LogPut(cursor, ":0000");
            /* [CAUTION]: lParam is 0 and that is a NAMED GAP, not an
             * oversight -- WM_CREATE's lParam is an
             * LPCREATESTRUCT and this host has never built one.
             * Saying so on the line is what stops a later reader
             * taking the zero for a measurement.
             */
            if (frame->CallbackMessage == WM_CREATE16 && frame->CallbackBlobLength)
                cursor = LogPut(cursor, " [lParam -> a CREATESTRUCT on the"
                            " guest's own stack]");
            else if (frame->CallbackMessage == WM_CREATE16)
                cursor = LogPut(cursor, " [lParam=0: NO CREATESTRUCT -- a"
                            " procedure that reads it will fault]");
            /* IS THE PROCEDURE'S SEGMENT ACTUALLY LOADED? (Importance = 1):
             * A Win16 code segment is loaded on demand, and
             * until it is, its descriptor's PRESENT bit is
             * clear. Writing that selector into the TIB's CS
             * kills the VDM silently; see WOWCALL_RETF_OFF.
             * Say which one it is, because "not present" is a
             * fact about the guest's loader and belongs in the
             * log next to the call it changes.
             */
            { WORD procedureCs = (WORD)(frame->CallbackProcedure >> WORD_SHIFT);
              WORD procedureIndex = (WORD)(DPMI_SELECTOR_INDEX(procedureCs));
              callbackAbsent = (procedureIndex && procedureIndex < DPMI_LDT_MAX
                          && !(g_Ldt[procedureIndex].Access & X86_DESCRIPTOR_PRESENT));
              if (callbackAbsent)
                  cursor = LogPut(cursor, " [code segment NOT PRESENT --"
                              " entering via the RETF trampoline"
                              " so krnl386 loads it]"); }
            if (!callbackSelector)
                cursor = LogPut(cursor, " -- NO RETURN SELECTOR (LDT full);"
                            " the call was NOT made");
            else if (!WowCallEnter(tib, callbackStackBase, callbackSelector, frame->CallbackProcedure,
                                    frame->CallbackDataSelector, frame->CallbackArguments, frame->CallbackArgumentCount,
                                    (DWORD)(ULONG_PTR)
                                        (frame->FrameBase + WOW32_OFF_RET),
                                    frame->CallbackReturnMode, frame->CallbackSink,
                                    frame->CallbackWindow, frame->CallbackMessage,
                                    frame->CallbackBlob, frame->CallbackBlobLength,
                                    frame->CallbackBlobArgument, callbackAbsent))
                cursor = LogPut(cursor, " -- REFUSED (depth, or an unusable"
                            " stack/procedure); the call was NOT"
                            " made and the guest keeps the answer"
                            " above");
            else
            {
                /* The action belongs to the frame we have just
                 * pushed; setting it here rather than through
                 * WowCallEnter's argument list keeps that list
                 * about the CALL and not about what follows it.
                 */
                if (g_WowCallDepth > 0)
                {
                    g_WowCallFrames[g_WowCallDepth - 1].Action = frame->CallbackAction;
                    g_WowCallFrames[g_WowCallDepth - 1].ActionArgument = frame->CallbackActionArgument;
                }
                cursor = LogPut(cursor, " -- ENTERED, depth ");
                cursor = LogHex(cursor, (DWORD)g_WowCallDepth);
                if (g_WowCallDepth > 0 && g_WowCallFrames[g_WowCallDepth - 1].PreviousTask)
                {
                    cursor = LogPut(cursor, " [INTER-TASK: runs as task 0x");
                    cursor = LogHex(cursor, WowSchedCurrentTask());
                    cursor = LogPut(cursor, " on its own stack, from task 0x");
                    cursor = LogHex(cursor, g_WowCallFrames[g_WowCallDepth - 1].PreviousTask);
                    cursor = LogPut(cursor, "]");
                }
            }
            cursor = LogPut(cursor, "\r\n");
        }
        /* OR THE CALLER DOES NOT RESUME AT ALL (Importance = 5):
         * (session 57) DialogBox is defined as not returning
         * until EndDialog, so its BOP does not complete here:
         * the modal loop takes over, and the guest carries on
         * inside the dialog's own procedure instead of after
         * the call it made. The context that is parked is the
         * one this handler has already prepared -- EIP past
         * the BOP -- so when the loop finally runs out, the
         * guest is standing exactly where DialogBox returns
         * to, with the answer in its hole.
         *
         * [CAUTION]: `else if`, NOT a second `if`. A service that asked
         * for both would otherwise have its callback entered
         * and then be immediately displaced by the loop's
         * first message, and the first call would never
         * return. Nothing asks for both today; this is what
         * stops the day it does from being a mystery.
         */
        else if (frame->IsEnumerationRequested)
        {
            CHAR enumNote[256];
            WORD  enumCallbackSelector = WowCallbackSelector();
            DWORD enumStackBase  = DpmiSelectorBase(
                (WORD)VDM_REG16(tib, VTIB_SS));
            enumNote[0] = 0;
            WowEnumStep(tib, enumStackBase, enumCallbackSelector, WOWENUM_FIRST, 0,
                         enumNote, sizeof enumNote);
            cursor = LogPut(cursor, "WOWENUM: "); cursor = LogPut(cursor, enumNote);
            cursor = LogPut(cursor, "\r\n");
        }
        else if (frame->IsModalDialog)
        {
            CHAR modalNote[512];
            WORD  modalCallbackSelector = WowCallbackSelector();
            DWORD modalStackBase  = DpmiSelectorBase(
                (WORD)VDM_REG16(tib, VTIB_SS));
            modalNote[0] = 0;
            cursor = LogPut(cursor, "WOWDLG: the caller is PARKED inside"
                        " DialogBox; the modal loop has it\r\n");
            WowLogFlush(base, &cursor);
            WowDlgStep(tib, modalStackBase, modalCallbackSelector, &g_Running,
                        modalNote, sizeof modalNote);
            cursor = LogPut(cursor, "WOWDLG: "); cursor = LogPut(cursor, modalNote);
            cursor = LogPut(cursor, "\r\n");
        }
        WowLogFlush(base, &cursor);
        {
            *cursorIo = cursor;
            *exitCodeOut = 1;
            return HOST_FLOW_RETURN;
        }
    }
    *cursorIo = cursor;
    return HOST_FLOW_NEXT;
}

/* A USER.EXE id. GetMessage with nothing queued first yields to another runnable task (when the
 * scheduler is on), then waits here for the host's input; everything else goes to WowUserCall.
 */
static INT Wow32ServiceUser(
    PSTR *cursorIo,
    PSTR const base,
    WOW32_FRAME *frame,
    volatile BYTE * const tib,
    INT *exitCodeOut)
{
    PSTR cursor = *cursorIo;

    if (!frame->IsKernel && frame->StubSegment == g_WowUserSegment && g_WowUserSegment)
    {
        CHAR note[224];
        /* KEEP THE REAL WINDOWS ALIVE. (GH #128, session 42) (Importance = 2):
         * They belong to this thread, so nothing about them happens
         * -- no paint, no move, no click, no title bar -- unless
         * this thread dispatches, and a WOW32 BOP is the only
         * regular moment it is not inside the guest. Bounded, so a
         * flood of mouse moves cannot starve the thing we are here
         * to run.
         */
        WowWinPump(WOW_PUMP_BUDGET_BRIEF);
        /* -- (E) s92 (#306): GetMessage WITH NOTHING TO GET, AND A TASK THAT HAS
         * NEVER RUN. The idle yield (D) is krnl386's WowWaitForMsgAndEvent --
         * which only WOWEXEC's loop calls. An APPLICATION idles here, in our
         * GetMessage, which blocks in the host; so a task it launched (Calc's
         * WinHelp, a program started from Program Manager) stayed parked at
         * its launch forever. Real USER yields inside an empty GetMessage, so
         * this does too -- to a FRESH task only (one already running gets its
         * turn at (D) or when this one retires), parked AT THIS BOP: EIP is
         * not advanced, so on resume the GetMessage is simply issued again.
         *
         * [CAUTION]: Not from inside anything: no callback in flight, no nested run, no
         * modal loop -- a context swapped there would be resumed under a frame
         * the other task cannot unwind.
         */
        /* -- s92 (#306): "nothing to get" is now THIS TASK's queue (wowmsg.h,
         * g_WowMsgTaker), and the task that yields here is parked WAITING FOR
         * MESSAGES: it is runnable again once one arrives for it -- which is
         * how WinHelp gets the message it posted itself while Calc ran, and
         * how a click on one task's window wakes it while another is idle
         * in the wait below (which then comes back here).
         */
        WORD messageTask = WowSchedCurrentTask();
        INT  canYield = g_WowSchedOn && frame->Id == WOWUSER_GETMESSAGE
                    && g_WowWindowNested == 0 && !WowDlgActive()
                    && messageTask && messageTask != WOWUSER_TASK_NONE16 && !WowSchedInterTaskLive();
    wowSchedRetry:
        if (canYield && !WowMsgCountFor(messageTask))
        {
            WORD yieldFrom = messageTask;
            INT  yieldSlotIndex  = WowSchedRunnable(yieldFrom, g_WowCallDepth);
            if (yieldSlotIndex >= 0)
            {
                WORD  yieldTo = g_WowSchedSlots[yieldSlotIndex].Task;
                DWORD yieldModeLinear = (DWORD)(ULONG_PTR)(frame->FrameBase + WOW32_OFF_MODE);
                INT   yieldTopLevel = WowSchedTopLevel(&g_WowSchedSlots[yieldSlotIndex]);
                WowSchedPoke(g_WowSchedSlots[yieldSlotIndex].ModeLinear, WOW32_MODE_ORDINARY);
                WowSchedSwap(&g_WowSchedSlots[yieldSlotIndex], tib, yieldModeLinear, yieldFrom, 0);
                g_WowSchedSlots[yieldSlotIndex].IsWaitingForMessage = 1;          /* the one that yielded */
                g_WowSchedSlots[yieldSlotIndex].CallbackDepth = g_WowCallDepth;
                if (yieldTopLevel)
                    g_WowSchedCurrentBase = g_WowCallDepth;
                WowSchedSetCurrent(yieldTo);
                WowTaskChdir(yieldTo, &cursor);
                ++g_WowSchedSwitches;
                cursor = LogPut(cursor, "\n     WOWSCHED: task 0x"); cursor = LogHex(cursor, yieldFrom);
                cursor = LogPut(cursor, " has an empty GetMessage -- YIELDING to task 0x");
                cursor = LogHex(cursor, yieldTo);
                cursor = LogPut(cursor, ", launched and never run or parked mid-work (#306); its GetMessage"
                            " is re-issued when it resumes\r\n");
                WowLogFlush(base, &cursor);
                {
                    *cursorIo = cursor;
                    *exitCodeOut = 1;
                    return HOST_FLOW_RETURN;
                }
            }
        }
        /* GetMessage BLOCKS, AND THIS IS THE BLOCK (Importance = 3):
         * There is no "no message" answer to GetMessage: a task with
         * an empty queue waits. The waiting is done HERE rather than
         * in wowuser.h because the thing being waited for is the
         * host's keyboard event, which belongs to the host -- and
         * because a service that blocks is a service that cannot be
         * reasoned about from the id space it lives in.
         *
         * [CAUTION]: IT IS BOUNDED, AND THE BOUND IS THE HONEST PART. A real
         * Win16 task blocks forever; a harness run must end. So the
         * wait is finite, and when it expires wowuser.h answers
         * WM_QUIT and SAYS the wait expired -- so "the application
         * quit" is never again confused with "nobody typed".
         *
         * [CAUTION]: THE HOST LOCK IS NOT HELD ACROSS THE WAIT. The UI thread
         * takes it to push a keystroke, so holding it here would
         * make the thing we are waiting for impossible.
         */
        if (frame->Id == WOWUSER_GETMESSAGE && !WowMsgCountFor(messageTask == WOWUSER_TASK_NONE16 ? 0 : messageTask)
            && !WowMsgQuitFor(messageTask == WOWUSER_TASK_NONE16 ? 0 : messageTask))
        {
            INT woke = 0;
            DWORD start = GetTickCount();
            DWORD waited;
            /* [INFO]: SAY THE SETTING AT THE POINT OF USE, ONCE. The startup
             * knob-read logs where the answer is decided, which is
             * not where it matters and -- measured -- did not reach
             * the log at all. Here it cannot be missed: the first
             * task that blocks prints what it is waiting for, so
             * "the guest quit after six seconds" and "the guest is
             * waiting for you" are never the same line.
             */
            if (!g_WowMsgIsWaitAnnounced)
            {
                CHAR siteLine[128];
                CHAR *siteCursor = siteLine;
                g_WowMsgIsWaitAnnounced = 1;
                siteCursor = LogPut(siteCursor, "\n     WOWMSG: a blocked GetMessage waits ");
                if (g_WowMsgWaitMs) { siteCursor = LogHex(siteCursor, g_WowMsgWaitMs);
                                        siteCursor = LogPut(siteCursor, " ms then answers"
                                                      " WM_QUIT"); }
                else
                    siteCursor = LogPut(siteCursor, "FOREVER (wowidle.txt = 0)");
                siteCursor = LogPut(siteCursor, "\r\n");
                LogAppend(LOG_PATH, siteLine, siteCursor); SerialOut(siteLine, siteCursor);
            }
            /* -- A BLOCKED Win16 TASK IS A Win32 MESSAGE PUMP.
             * (session 42, replacing session 41's keyboard-event
             * wait.) The input no longer comes from the DOS 8042
             * path -- it comes from the REAL WINDOW, and the real
             * window's messages arrive on this thread's Win32
             * queue. So "the Win16 queue is empty" and "wait for
             * something to happen" are the same statement as
             * "dispatch Win32 messages until one of them turns into
             * a Win16 message", which is exactly what a Win16 task
             * blocking in GetMessage is FOR.
             *
             * [CAUTION]: MsgWaitForMultipleObjects, not Sleep: it wakes the
             * instant a message arrives, so a keystroke is not
             * delayed by the poll interval, and it does not spin.
             */
            {   /* -- A HEARTBEAT, BECAUSE "NOT RESPONDING" IS A
                     QUESTION ABOUT THIS LOOP. (session 43) The windows
                     belong to this thread, so if XP calls the guest's
                     window "Not Responding" the answer is either "this
                     loop is not running" or "it is running and
                     dispatching nothing" -- and those need completely
                     different fixes. One line every two seconds says
                     which, and a bounded count keeps a long idle from
                     filling the log. */
                DWORD beat = start;
                UINT beats = 0;
                DWORD pumpedStart = g_WowWinPumped;
                /* [INFO]: Tell the freeze watchdog this stall is deliberate --
                 * see g_WowMsgInWait in wowmsg.h. Set BEFORE the loop and
                 * cleared after it on every exit path, because the
                 * alternative is a flag that stays set once and
                 * disables the watchdog for the rest of the run.
                 */
                g_WowMsgInWait = 1;
                while (g_Running && !WowMsgCountFor(messageTask == WOWUSER_TASK_NONE16 ? 0 : messageTask)
                       && !WowMsgQuitFor(messageTask == WOWUSER_TASK_NONE16 ? 0 : messageTask)
                       && (!g_WowMsgWaitMs
                           || GetTickCount() - start < g_WowMsgWaitMs))
                {
                    /* s92 (#306): another parked task's message arrived */
                    if (canYield && WowSchedRunnable(messageTask, g_WowCallDepth) >= 0)
                    {
                        woke = 1;
                        break;
                    }
                    /* -- THE IDLE WAIT, AND ITS TIMEOUT IS A
                     * LATENCY FLOOR. A WM_PAINT arriving while the
                     * guest is parked here should wake the wait
                     * through QS_ALLINPUT, and usually does --
                     * measured paint latency is 0 ms three times
                     * in five. But MsgWaitForMultipleObjects has a
                     * documented race: a message that arrives
                     * between the PeekMessage above and the wait
                     * below can leave the queue state already
                     * "seen", and the wait then sleeps the FULL
                     * timeout. The other two measurements were
                     * 47 ms and 94 ms -- one and two timeouts.
                     *
                     * [CAUTION]: REFUTED, and the timeout is left at 50.
                     * Dropping it to 10 ms was tried and MEASURED:
                     * the same workload produced 0, 0, 0, 46, 93 ms
                     * against the 50 ms build's 0, 0, 0, 47, 94.
                     * Identical. So the two slow paints are NOT
                     * this wait sleeping through a lost wake-up --
                     * the guest is simply not parked here when they
                     * arrive, and the delay is its own work
                     * (raising a window also delivers WM_ACTIVATE
                     * and WM_SETFOCUS, each of which re-enters
                     * 16-bit code). A shorter timeout would cost an
                     * idle guest ~100 wake-ups a second and buy
                     * nothing, so it was reverted rather than kept
                     * on the grounds that it "should" help.
                     */
                    if (g_IcaPending)         /* s90 #278 */
                        WowIcaDeliver(g_DosMachine, tib, 0);
                    if (!WowWinPump(WOW_PUMP_BUDGET))
                        MsgWaitForMultipleObjects(0, NULL, FALSE, WOW_INPUT_WAIT_MS,
                                                  QS_ALLINPUT);
                    if (beats < 20 && GetTickCount() - beat >= 2000)
                    {
                        CHAR handlerLine[160];
                        CHAR *handlerCursor = handlerLine;
                        beat = GetTickCount();
                        ++beats;
                        handlerCursor = LogPut(handlerCursor, "     WOWMSG: blocked 0x");
                        handlerCursor = LogHex(handlerCursor, beat - start);
                        handlerCursor = LogPut(handlerCursor, " ms; Win32 messages dispatched on"
                                      " this thread since blocking 0x");
                        handlerCursor = LogHex(handlerCursor, g_WowWinPumped - pumpedStart);
                        handlerCursor = LogPut(handlerCursor, " (total 0x");
                        handlerCursor = LogHex(handlerCursor, g_WowWinPumped);
                        handlerCursor = LogPut(handlerCursor, "), Win16 queued 0x");
                        handlerCursor = LogHex(handlerCursor, (DWORD)g_WowMsgCount);
                        handlerCursor = LogPut(handlerCursor, "\r\n");
                        LogAppend(LOG_PATH, handlerLine, handlerCursor); SerialOut(handlerLine, handlerCursor);
                    }
                }
                g_WowMsgInWait = 0;
            }
            waited = GetTickCount() - start;
            cursor = LogPut(cursor, "\n     WOWMSG: GetMessage with an empty queue"
                        " -- BLOCKED for 0x");
            cursor = LogHex(cursor, waited);
            cursor = LogPut(cursor, " ms; ");
            cursor = LogHex(cursor, (DWORD)g_WowMsgCount);
            cursor = LogPut(cursor, " message(s) arrived\r\n");
            if (woke)
            {
                cursor = LogPut(cursor, "     WOWSCHED: a parked task's message arrived"
                            " -- yielding to it\r\n");
                WowLogFlush(base, &cursor);
                goto wowSchedRetry;
            }
            WowLogFlush(base, &cursor);
        }
        /* [CAUTION]: Same rule as ShellAbout and the file dialog: a modal
         * service does not return until a human dismisses it, and
         * the SERVICED line carrying its text is written
         * afterwards. Announce it first, or a log collected while
         * the box is up stops dead at the call before it -- which
         * has already misled a reading twice this session.
         */
        if (frame->Id == WOWUSER_MESSAGEBOX)
        {
            cursor = LogPut(cursor, "\n     WOWUSER: MessageBox is MODAL -- the VDM"
                        " stops here until it is dismissed; the"
                        " SERVICED line carries its TEXT and follows"
                        " when it is\r\n");
            WowLogFlush(base, &cursor);
        }
        {
            INT exitCode;
            INT flow = Wow32FinishUserCall(&cursor, base, frame, note, tib, &exitCode);
            if (flow == HOST_FLOW_RETURN)
            {
                *cursorIo = cursor;
                *exitCodeOut = exitCode;
                return HOST_FLOW_RETURN;
            }
        }
    }
    *cursorIo = cursor;
    return HOST_FLOW_NEXT;
}

/* krnl386's second table, id 0xd1: the new task's environment -- returned once per launch, before the new task's PSP is built, and what that PSP's environment field then holds. */
static INT Wow32ServiceKernelSecondTable(
    PSTR *cursorIo,
    PSTR const base,
    WOW32_FRAME *frame,
    volatile BYTE * const tib,
    INT *exitCodeOut)
{
    PSTR cursor = *cursorIo;

    /* seg2 0xd1: THE NEW TASK'S ENVIRONMENT (Importance = 4):
     *
     * [INFO]: WHAT IT IS, AND HOW THAT IS KNOWN. It is called once per task
     * launch, before the new task's PSP is built (`AH=55h` follows
     * it), and what it returns is what then appears in that new PSP's
     * environment field, +0x2c -- the `PSPENV CHANGED` watch shows the
     * answer land there. A 0 answer fails the launch. So the value
     * this call returns IS the environment of the task about to run,
     * and krnl386 goes on to give it an owner as a global object of
     * its own -- a real object, not a token.
     *
     * [INFO]: AND THAT CLOSES A FAULT WE CAUSED OURSELVES. Session 39 ran
     * `0xd1` as an EXPERIMENT (`wow32ret.txt`, `d1 00000001`) on the
     * reading that "it only has to be non-zero", and SYSEDIT then
     * died at `0x0abf:0x09f0` loading ES with 1 -- the null
     * descriptor -- read from its own `PSP+0x2c`. The 1 in that field
     * was OUR OWN EXPERIMENT VALUE, and the run log says so without
     * any new measurement: the `0xd1` answer (`ANSWERED
     * 0x00000001`), then AH=55h, then
     * `PSPENV CHANGED: sel 0x0adf +0x2c 0x03c7 -> 0x00000001`, then
     * the next call in.
     * "Non-zero" was a measurement of the abort, not of the meaning.
     *
     * [CAUTION]: SO THE ANSWER MUST BE A SELECTOR THE GUEST CAN LOAD, and one
     * krnl386 owns. Two sources, in the DOS EXEC order, and the log
     * says which one was taken:
     *   1. LOADPARMS.segEnv -- the far pointer at args +2/+4 is the
     *      parameter block (`0x03df:0x1ce4` in the run, WOWEXEC's own
     *      stack), and its first word is the environment.
     *   2. Zero there means INHERIT, and the parent is the current
     *      task -- whose PSP is the last one this host built, because
     *      the child's does not exist yet (`AH=55h` for it comes
     *      AFTER this call: log lines 5355 then 5376).
     *
     * [CAUTION]: And WOWEXEC's launcher is visibly playing exactly that
     * protocol: it installs 0x0aff into its OWN PSP+0x2c before the
     * launch and restores 0x03c7 after it, which is the DOS way of
     * handing an environment to a child and is why (2) is not a
     * guess.
     *
     * [CAUTION]: IF NEITHER SOURCE YIELDS A SELECTOR THAT RESOLVES, SAY SO AND
     * DO NOT ANSWER. Inventing a value here is how the 1 got in.
     *
     * [CAUTION]: AND IT MUST BE A **COPY**. The first cut handed the source
     * selector straight back, and the run said no: loading ES with
     * 0x0aff now faulted at the LOAD (`err=0x0afc`, the
     * selector's own index) instead of at the first use, because
     * WOWEXEC's launcher FREES the block as soon as LoadModule
     * returns -- `LDTSYNC idx 0x15f <- base=0 acc=0x00` one line
     * before `PSPENV CHANGED: sel 0x03bf +0x2c 0x0aff -> 0x03c7`.
     * That is the DOS EXEC protocol played out in full: build an
     * environment for the child, launch, free it, restore your own.
     * The child's copy is what the call is FOR -- on real WOW the
     * 32-bit side allocates a Win16 global and copies into it. This
     * host cannot call GlobalAlloc (nothing here has ever called
     * INTO 16-bit code), so the copy lives in a host paragraph with
     * a host selector, which over-lives rather than under-lives.
     *
     * [CAUTION]: THE DIFFERENCE THAT LEAVES: the block is not in krnl386's
     * global arena, so krnl386's owner write for it (FarSetOwner)
     * will not find an arena entry. Recorded, not hidden.
     */
    if (!frame->IsKernel && g_WowKernel2Segment && frame->StubSegment == g_WowKernel2Segment
        && frame->Id == WOW32K2_TASKENV)
    {
        WORD parameterBlockSelector = Wow32ArgWord(frame, WOW32K2_TASKENV_ARG_BLOCK_SELECTOR);
        WORD parameterBlockOffset = Wow32ArgWord(frame, WOW32K2_TASKENV_ARG_BLOCK_OFFSET);
        DWORD parameterBlockLinear = parameterBlockSelector ? DpmiSelectorBase(parameterBlockSelector) : 0;
        WORD  environment = 0;
        PCSTR source = "";
        cursor = LogPut(cursor, "\n     WOW32 seg2 0xd1 task environment: parmblock 0x");
        cursor = LogHex(cursor, parameterBlockSelector); cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, parameterBlockOffset);
        if (parameterBlockLinear && HostReadable((const VOID *)(ULONG_PTR)(parameterBlockLinear + parameterBlockOffset), 2))
        {
            const volatile BYTE *pathBytes =
                (const volatile BYTE *)(ULONG_PTR)(parameterBlockLinear + parameterBlockOffset);
            environment = (WORD)(pathBytes[0] | (pathBytes[1] << BYTE_SHIFT));
            source = "LOADPARMS.segEnv";
        }
        if (!environment && g_WowPspCount > 0)
        {
            WORD  pspSelector = g_WowPspSelector[g_WowPspCount - 1];
            DWORD pspLinear = DpmiSelectorBase(pspSelector);
            cursor = LogPut(cursor, " segEnv=0 (inherit) parent PSP 0x");
            cursor = LogHex(cursor, pspSelector);
            if (pspLinear && HostReadable((const VOID *)(ULONG_PTR)pspLinear, DOS_PSP_ENVIRONMENT + sizeof(WORD)))
            {
                const volatile BYTE *psp =
                    (const volatile BYTE *)(ULONG_PTR)pspLinear;
                environment = (WORD)(psp[DOS_PSP_ENVIRONMENT] | (psp[DOS_PSP_ENVIRONMENT + 1] << BYTE_SHIFT));
                source = "the parent PSP's +0x2c";
            }
        }
        {   DWORD environmentLinear = environment ? DpmiSelectorBase(environment) : 0;
            DWORD cap  = (DWORD)WOW_ENV_PARAS * PARAGRAPH_SIZE_U;
            cursor = LogPut(cursor, " src 0x"); cursor = LogHex(cursor, environment);
            cursor = LogPut(cursor, " ("); cursor = LogPut(cursor, source[0] ? source : "nothing");
            cursor = LogPut(cursor, ")");
            if (environmentLinear && g_WowEnvironmentSegment &&
                HostReadable((const VOID *)(ULONG_PTR)environmentLinear, cap))
            {
                const volatile BYTE *sourceBytes =
                    (const volatile BYTE *)(ULONG_PTR)environmentLinear;
                volatile BYTE *destinationBytes = (volatile BYTE *)(ULONG_PTR)
                                   ((DWORD)g_WowEnvironmentSegment << PARAGRAPH_SHIFT);
                DWORD index = 0;
                DWORD item;
                WORD  selector;
                /* The MS-DOS 3.0+ block: the strings, the empty string
                 * that ends them, a WORD count, then the program's own
                 * pathname. krnl386 finds its OWN exe through exactly
                 * this tail (see WowPlaceV86), so it is part of the
                 * environment and not an optional extra.
                 */
                while (index < cap && sourceBytes[index])
                {
                    while (index < cap && sourceBytes[index])
                        ++index;
                    ++index;                          /* the string's NUL */
                }
                ++index;                              /* the empty string */
                if (index + X86_WORD_SIZE <= cap)
                {
                    index += X86_WORD_SIZE;                       /* the WORD count */
                    while (index < cap && sourceBytes[index])
                        ++index;
                    ++index;                          /* the pathname's NUL */
                }
                if (index > cap)
                    index = cap;
                for (item = 0; item < index; ++item)
                    destinationBytes[item] = sourceBytes[item];
                selector = DpmiSegmentToDescriptor(g_WowEnvironmentSegment);
                cursor = LogPut(cursor, " -> copied 0x"); cursor = LogHex(cursor, index);
                cursor = LogPut(cursor, " bytes to 0x"); cursor = LogHex(cursor, g_WowEnvironmentSegment);
                cursor = LogPut(cursor, ":0000 as sel 0x"); cursor = LogHex(cursor, selector);
                if (selector)
                {
                    Wow32SetReturn(frame, selector);
                    ++g_Wow32Serviced;
                    VDM_REG(tib, VTIB_EIP) += VDM_BOP_LENGTH;
                    cursor = LogPut(cursor, "\r\n -> SERVICED (task environment),"
                                " returned 0x");
                    cursor = LogHex(cursor, frame->Result); cursor = LogPut(cursor, "\r\n");
                    WowLogFlush(base, &cursor);
                    cursor = base;
                    {
                        *cursorIo = cursor;
                        *exitCodeOut = 1;
                        return HOST_FLOW_RETURN;
                    }
                }
                cursor = LogPut(cursor, " -- NO SELECTOR (LDT full)");
            }
            cursor = LogPut(cursor, " -> NO ENVIRONMENT TO COPY -- left unimplemented"
                        " rather than invented");
        }
    }
    *cursorIo = cursor;
    return HOST_FLOW_NEXT;
}

/* KERNEL WOW32_RESOLVEMODULEPATH: resolve a module name to a full path for krnl386 -- with SearchPathA,
 * the Win32 search order, which its note explains is close enough.
 */
static INT Wow32ServiceResolveModulePath(
    PSTR *cursorIo,
    PSTR const base,
    WOW32_FRAME *frame,
    volatile BYTE * const tib,
    INT *exitCodeOut)
{
    PSTR cursor = *cursorIo;

    /* 0xc5 ResolveModulePath -- WHERE IS THIS MODULE? (Importance = 3):
     * Serviced here rather than in wow32.h because the answer is a
     * 16:16 far pointer: it needs memory the guest can reach and a
     * selector for it, and both live on this side. See the note on
     * WOW32_RESOLVEMODULEPATH for how the two call sites pin the
     * resolve/release pair and prove `dst` takes a POINTER.
     *
     * [INFO]: WHAT IT FIXES, MEASURED. krnl386 asks this about `SHELL.DLL`
     * while binding SYSEDIT's imports. Answered 0, it falls back to
     * composing the bare name against the CURRENT DIRECTORY and
     * opens `C:\Documents and Settings\Matthew\SHELL.DLL`, gle=2 --
     * and the whole launch fails there. The same thing happened
     * earlier to MMSYSTEM.DLL and WFWNET.DRV out of SYSTEM.INI's
     * [drivers].
     *
     * [CAUTION]: NOT-FOUND MUST STILL ANSWER 0. The fallback tail is the
     * guest's own handling for "I could not resolve this", and a
     * host that invented a path for a module that is not there
     * would turn a clean failure into a wrong filename.
     *
     * [CAUTION]: SearchPathA IS THE WIN32 ORDER, NOT WIN16'S. Close enough for
     * what krnl386 needs -- it reaches the Windows and system
     * directories, which is where the 16-bit modules are, and this
     * rig's own WOWEXEC/KRNL386 were found there. It also consults
     * the current directory, which Win16 did too. Recorded as a
     * difference rather than claimed as equivalence.
     */
    if (frame->IsKernel && frame->Id == WOW32_RESOLVEMODULEPATH && g_WowPathSegment)
    {
        volatile BYTE *source = Wow32ArgPointer(frame, WOW32_RESOLVEMODULEPATH_ARG_SOURCE);
        volatile BYTE *destination = Wow32ArgPointer(frame, WOW32_RESOLVEMODULEPATH_ARG_DESTINATION);
        CHAR name[300];
        CHAR full[300];
        INT item;
        cursor = LogPut(cursor, "  WOW32 0xc5 ResolveModulePath ");
        if (!source)
        {
            /* (dst, NULL) is the RELEASE half of the pair. Nothing to
             * free -- the scratch is a fixed paragraph -- but it must
             * still be answered, and it must say so in the log rather
             * than look like a resolve that found nothing.
             */
            cursor = LogPut(cursor, "RELEASE");
            Wow32SetReturn(frame, 1);
        }
        else
        {
            DWORD length = 0;
            PSTR filePart = 0;
            for (item = 0; item < (INT)sizeof name - 1 && source[item]; ++item)
                name[item] = (CHAR)source[item];
            name[item] = 0;
            cursor = LogPut(cursor, "\""); cursor = LogPut(cursor, name); cursor = LogPut(cursor, "\" -> ");
            if (name[0])
                length = SearchPathA(NULL, name, NULL, sizeof full, full, &filePart);
            if (length && length < sizeof full)         /* krnl386 opens this via DOS: 8.3 only */
            {
                WowShorten(full, sizeof full);
                for (length = 0; full[length]; ++length) ;
            }
            if (!length || length >= sizeof full || !destination)
            {
                cursor = LogPut(cursor, "NOT FOUND (krnl386 will fall back to its own"
                            " name, as it does today)");
                Wow32SetReturn(frame, 0);
            }
            else
            {
                volatile BYTE *pathBytes = (volatile BYTE *)(ULONG_PTR)
                                    ((DWORD)g_WowPathSegment << PARAGRAPH_SHIFT);
                WORD selector = DpmiSegmentToDescriptor(g_WowPathSegment);
                for (item = 0; item <= (INT)length && item < WOW_PATH_PARAS * PARAGRAPH_SIZE - 1; ++item)
                    pathBytes[item] = (BYTE)full[item];
                pathBytes[item < WOW_PATH_PARAS * PARAGRAPH_SIZE - 1 ? item : WOW_PATH_PARAS * PARAGRAPH_SIZE - 1] = 0;
                if (!selector)
                {
                    cursor = LogPut(cursor, "NO SELECTOR (LDT full)");
                    Wow32SetReturn(frame, 0);
                }
                else
                {
                    Wow32PokeWord(destination,     0);        /* offset */
                    Wow32PokeWord(destination + X86_FAR_POINTER_SEGMENT, selector);      /* segment */
                    cursor = LogPut(cursor, "\""); cursor = LogPut(cursor, full);
                    cursor = LogPut(cursor, "\" at 0x"); cursor = LogHex(cursor, selector);
                    cursor = LogPut(cursor, ":0000");
                    Wow32SetReturn(frame, 1);
                }
            }
        }
        cursor = LogPut(cursor, "\r\n");
        ++g_Wow32Serviced;
        VDM_REG(tib, VTIB_EIP) += VDM_BOP_LENGTH;
        cursor = LogPut(cursor, " -> SERVICED (host path search), returned 0x");
        cursor = LogHex(cursor, frame->Result); cursor = LogPut(cursor, "\r\n");
        WowLogFlush(base, &cursor);
        {
            *cursorIo = cursor;
            *exitCodeOut = 1;
            return HOST_FLOW_RETURN;
        }
    }
    *cursorIo = cursor;
    return HOST_FLOW_NEXT;
}

/* KERNEL WOW32_GETCURDIR: the current directory, answered through the PM transfer buffer. */
static INT Wow32ServiceGetCurrentDirectory(
    PSTR *cursorIo,
    PSTR const base,
    WOW32_FRAME *frame,
    volatile BYTE * const tib,
    DOS_MACHINE * const machine,
    INT *exitCodeOut)
{
    PSTR cursor = *cursorIo;

    if (frame->IsKernel && frame->Id == WOW32_GETCURDIR && g_PmTransferSegment)
    {
        DWORD directoryOffset  = Wow32ArgWord(frame, WOW32_GETCURDIR_ARG_BUFFER_OFFSET);
        WORD  directorySelector = Wow32ArgWord(frame, WOW32_GETCURDIR_ARG_BUFFER_SELECTOR);
        DWORD drive  = Wow32ArgWord(frame, WOW32_GETCURDIR_ARG_DRIVE) & BYTE_MASK;
        DWORD savedAx = VDM_REG(tib, VTIB_EAX);
        DWORD savedDx = VDM_REG(tib, VTIB_EDX);
        DWORD savedDs = VDM_REG(tib, VTIB_DS);
        DWORD savedSi = VDM_REG(tib, VTIB_ESI);
        DWORD directoryBase = DpmiSelectorBase(directorySelector);
        volatile BYTE *transfer = (volatile BYTE *)(ULONG_PTR)
                            ((DWORD)g_PmTransferSegment << PARAGRAPH_SHIFT);
        INT item;
        for (item = 0; item < WOW32_GETCURDIR_BUFFER_SIZE; ++item)
            transfer[item] = 0;
        VDM_SET16(tib, VTIB_EAX, DOS_FN_GET_CURRENT_DIRECTORY << BYTE_SHIFT);
        VDM_SET16(tib, VTIB_EDX, (WORD)drive);
        VDM_SET16(tib, VTIB_DS,  g_PmTransferSegment);
        VDM_SET16(tib, VTIB_ESI, 0);
        machine->TraceCursor = cursor;
        DosInt21SetProtectedMode(TRUE);
        DosInt21(machine);
        DosInt21SetProtectedMode(FALSE);
        cursor = machine->TraceCursor;
        {   DWORD carryFlag = VDM_REG(tib, VTIB_EFLAGS) & EFLAGS_CF_U;
            VDM_REG(tib, VTIB_EAX) = savedAx;
            VDM_REG(tib, VTIB_EDX) = savedDx;
            VDM_REG(tib, VTIB_DS)  = savedDs;
            VDM_REG(tib, VTIB_ESI) = savedSi;
            cursor = LogPut(cursor, "  WOW32 0xc9 GetCurrentDirectory drive=0x");
            cursor = LogHex(cursor, drive);
            cursor = LogPut(cursor, " -> \""); cursor = LogPut(cursor, (PCSTR)transfer);
            cursor = LogPut(cursor, "\" cf="); cursor = LogHex(cursor, carryFlag);
            if (!carryFlag && directoryBase)
            {
                volatile BYTE *destinationBytes = (volatile BYTE *)(ULONG_PTR)(directoryBase + directoryOffset);
                if (MemoryReadable((ULONG_PTR)destinationBytes, WOW32_GETCURDIR_BUFFER_SIZE))
                    for (item = 0; item < WOW32_GETCURDIR_BUFFER_SIZE; ++item)
                        destinationBytes[item] = transfer[item];
                else
                {
                    carryFlag = 1;
                    cursor = LogPut(cursor, " (BUFFER UNREACHABLE)");
                }
            }
            /* DX must not come back 0xFFFF -- that is this call site's
             * failure sentinel, and it is checked before AX.
             */
            Wow32SetReturn(frame, carryFlag ? DOS_ERR_INVALID_DRIVE : 0u);
            cursor = LogPut(cursor, "\r\n");
        }
        ++g_Wow32Serviced;
        VDM_REG(tib, VTIB_EIP) += VDM_BOP_LENGTH;
        cursor = LogPut(cursor, " -> SERVICED (DOS-backed), returned 0x");
        cursor = LogHex(cursor, frame->Result); cursor = LogPut(cursor, "\r\n");
        WowLogFlush(base, &cursor);
        {
            *cursorIo = cursor;
            *exitCodeOut = 1;
            return HOST_FLOW_RETURN;
        }
    }
    *cursorIo = cursor;
    return HOST_FLOW_NEXT;
}

/* KERNEL WOW32_FILE_READ, at a call site that may not decline it: read the file into the client's buffer. */
static INT Wow32ServiceFileRead(
    PSTR *cursorIo,
    PSTR const base,
    WOW32_FRAME *frame,
    DOS_MACHINE * const machine,
    volatile BYTE * const tib,
    INT *exitCodeOut)
{
    PSTR cursor = *cursorIo;

    /* DOS-DEPENDENT WOW32 SERVICES. (GH #128):
     * Most of the surface is pure Win32 and lives in wow32.h. A few
     * need the DOS machine, so they are answered here where it is in
     * scope. 0xc9 is the first: krnl386's INT 21h AH=47h arm calls it
     * with (drive, selector, offset) and, unlike the file family, it
     * may NOT be declined -- its call site treats DX=0xFFFF as a hard
     * error and reports failure to the caller rather than chaining to
     * DOS (tools/ne/wowdecline.py). Leaving it unimplemented is what
     * produced "Unable to open KERNEL executable": krnl386 could not
     * learn the current directory, so it never built a path to open.
     */
    /* THE READ THAT CANNOT BE DECLINED. (GH #128, session 34) (Importance = 1):
     * krnl386 calls 0x97 (read) from two call sites (told apart by the
     * frame's `from` word). At one the sentinel means "ask real DOS" and
     * declining is the whole answer. At the other it does NOT chain --
     * the failure goes straight back to its own caller (observed) -- so
     * here the read has to actually happen.
     * That is the read of SYSTEM.DRV's segment data, and stepping it
     * over is what left krnl386 saying "Missing 16-bit system module"
     * after it had opened the file successfully.
     * The argument layout is measured, not assumed: two calls in one run,
     * `(.. 0x40 0 0x0eaa 0x1f 5)` and `(.. 0x560 0 0 0x17 5)`, and the
     * first was followed by the chained `AH=3Fh` reading exactly 0x40
     * bytes into that same buffer. So words 4-5 are a DWORD count, 6-7 a
     * 16:16 buffer pointer, 8 the handle.
     *
     * [CAUTION]: DX:AX IS A 32-BIT BYTE COUNT, not a flag. Failure is
     * 0xFFFFFFFF in DX:AX, so a short read must return the SHORT COUNT
     * and only a real failure may return the sentinel.
     */
    if (frame->IsKernel && frame->Id == WOW32_FILE_READ && !Wow32MayDecline(frame->Id, frame->CallSite))
    {
        DWORD count  = (DWORD)Wow32ArgWord(frame, WOW32_FILE_READ_ARG_COUNT)
                   | ((DWORD)Wow32ArgWord(frame, WOW32_FILE_READ_ARG_COUNT + WOW_WORD_BYTES) << WORD_SHIFT);
        DWORD bufferOffset = Wow32ArgWord(frame, WOW32_FILE_READ_ARG_BUFFER_OFFSET);
        WORD  bufferSelector = Wow32ArgWord(frame, WOW32_FILE_READ_ARG_BUFFER_SELECTOR);
        DWORD argumentHandle    = Wow32ArgWord(frame, WOW32_FILE_READ_ARG_HANDLE);
        DWORD bufferBase = DpmiSelectorBase(bufferSelector);
        DWORD bytesRead = 0;
        INT isOk = 0;
        cursor = LogPut(cursor, "\n     WOW32 0x97 read (site 0x"); cursor = LogHex(cursor, frame->CallSite);
        cursor = LogPut(cursor, ", may NOT decline) h="); cursor = LogHex(cursor, argumentHandle);
        cursor = LogPut(cursor, " cnt=0x"); cursor = LogHex(cursor, count);
        cursor = LogPut(cursor, " -> 0x"); cursor = LogHex(cursor, bufferSelector);
        cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, bufferOffset);
        cursor = LogPut(cursor, " lin=0x"); cursor = LogHex(cursor, bufferBase + bufferOffset);
        if (argumentHandle < DOS_MAX_FILES && machine->FileHandles[argumentHandle] && bufferBase
            && HostWritable((VOID *)(ULONG_PTR)(bufferBase + bufferOffset), count))
        {
            isOk = ReadFile(machine->FileHandles[argumentHandle], (VOID *)(ULONG_PTR)(bufferBase + bufferOffset),
                          count, &bytesRead, NULL) ? 1 : 0;
        }
        if (isOk) { Wow32SetReturn(frame, bytesRead);
        ++g_Wow32Serviced;
                  cursor = LogPut(cursor, " -> read 0x");
                  cursor = LogHex(cursor, bytesRead);
                  cursor = LogPut(cursor, "b"); }
        else    { Wow32SetReturn(frame, WOW32_FILE_READ_FAILED_U);
        ++g_Wow32Unimplemented;
                  cursor = LogPut(cursor, " -> FAILED (bad handle/selector/buffer)"); }
        cursor = LogPut(cursor, "\r\n");
        WowLogFlush(base, &cursor);
        VDM_REG(tib, VTIB_EIP) += VDM_BOP_LENGTH;
        {
            *cursorIo = cursor;
            *exitCodeOut = 1;
            return HOST_FLOW_RETURN;
        }
    }
    *cursorIo = cursor;
    return HOST_FLOW_NEXT;
}

/* The Win16 scheduler at a KERNEL call: switch tasks at a launch (here, while the frame is still the
 * creator's) and at WowWaitForMsgAndEvent, krnl386's idle yield.
 */
static INT Wow32ScheduleKernelCall(
    PSTR *cursorIo,
    PSTR const base,
    WOW32_FRAME *frame,
    volatile BYTE * const tib,
    INT *exitCodeOut)
{
    PSTR cursor = *cursorIo;

    if (g_WowSchedOn && frame->IsKernel)
    {
        DWORD modeLinear = (DWORD)(ULONG_PTR)(frame->FrameBase + WOW32_OFF_MODE);
        WORD  current     = WowSchedCurrentTask();
        /* (A) THE LAUNCH -- AND THE SWITCH HAS TO HAPPEN HERE.
         *
         * [CAUTION]: MEASURED THE HARD WAY. The first cut saved this frame,
         * let the new task run first (as it does today) and meant to
         * hand the creator back at the task's WaitEvent. It faulted
         * in SwitchToTask, because THE FRAME'S MEMORY IS THE NEW
         * TASK'S OWN STACK: the epilogue pops it, SP moves up, and
         * the task pushes straight back over it. By WaitEvent the
         * frame is gone. Switching HERE is the only ordering that
         * works, and it is also the one that leaves the new task's
         * stack frozen and untouched while the creator runs -- which
         * is exactly what makes resuming it later sound.
         * So: park the new task at this instruction, and send the
         * creator home through epilogue mode 25.
         */
        INT freedSlotIndex = (frame->Id == WOW32_TASK_LAUNCH) ? WowSchedFree() : -1;
        if (frame->Id == WOW32_TASK_LAUNCH && freedSlotIndex >= 0)
        {
            DWORD taskBase = DpmiSelectorBase(current);
            WORD  taskInstance = 0;
            INT   isFromTdb = 0;
            if (taskBase)
            {
                const volatile BYTE *task = (const volatile BYTE *)(ULONG_PTR)taskBase;
                taskInstance = (WORD)(task[WOW_TDB_INSTANCE] | (task[WOW_TDB_INSTANCE + 1] << BYTE_SHIFT));
                isFromTdb = taskInstance != 0;
            }
            /* [INFO]: LoadModule's result is the new task's instance handle, and
             * InitTask has NOT run yet, so TDB+0x1c is still zero here.
             * The fallback is Win16's own invariant, not a guess about
             * this one program: a task's SS and DS are two aliases of one
             * descriptor, so its instance handle is its stack selector
             * with the low bits clear. The stock oracle shows it twice
             * (SS=0x16bf/hInst=0x16be, SS=0x03af/hInst=0x03ae) and our
             * own task a third time. It is VERIFIED at (C) below against
             * the value krnl386 itself writes, and a mismatch is loud.
             */
            if (!taskInstance)
                taskInstance = (WORD)(VDM_REG(tib, VTIB_SS) & WOW_INSTANCE_FROM_SELECTOR);
            WowSchedSave(&g_WowSchedSlots[freedSlotIndex], tib, modeLinear, current, VDM_BOP_LENGTH);
            g_WowSchedSlots[freedSlotIndex].IsFresh = 1;          /* s92: not run yet (#306) */
            g_WowSchedLaunchChild = current;            /* the parent is the next caller */
            WowTaskDirectoryHere(current);             /* #164: its launch directory */
            Wow32SetReturn(frame, taskInstance);
            Wow32PokeWord(frame->FrameBase + WOW32_OFF_MODE, WOW32_MODE_SWITCHBACK);
            ++g_WowSchedSwitches;
            cursor = LogPut(cursor, "\n     WOWSCHED: task 0x"); cursor = LogHex(cursor, current);
            cursor = LogPut(cursor, " parked at its launch; creator sent home through"
                        " epilogue mode 25 with LoadModule result 0x");
            cursor = LogHex(cursor, taskInstance);
            cursor = LogPut(cursor, isFromTdb ? " (TDB+0x1c)" : " (SS&~1 -- verified at the"
                                                  " resume)");
            cursor = LogPut(cursor, "\r\n");
            WowLogFlush(base, &cursor);
            VDM_REG(tib, VTIB_EIP) += VDM_BOP_LENGTH;
            {
                *cursorIo = cursor;
                *exitCodeOut = 1;
                return HOST_FLOW_RETURN;
            }
        }
        /* -- (D) A TASK ASKS TO WAIT FOR A MESSAGE. THAT IS THE
         *   YIELD, AND IT IS THE MOMENT THAT WAS MISSING. --------
         * Moment (C) resumes the parked task when the creator
         * RETIRES. WOWEXEC never retires -- it registers its
         * classes, opens its windows and settles into its message
         * loop, so the current-task word never reaches 0 and a
         * task parked at its launch waits forever. That is
         * exactly what SYSEDIT.EXE does today: it gets a task
         * database, a launch frame and a task id, and never runs.
         *
         * [INFO]: WowWaitForMsgAndEvent IS the Win16 "I have nothing to do"
         * primitive -- krnl386 export 262, and the only thing
         * WOWEXEC's pump calls when the queue is empty. A task
         * blocking on it is a task offering the CPU, so this is
         * where a cooperative scheduler is supposed to act, and it
         * needs no new lever: the frame is already a WOW32 frame
         * with a mode word and a return hole.
         *
         * [INFO]: THE WAIT RETURNS 0, AND THAT IS OBSERVED, not chosen: on
         * a non-zero answer WOWEXEC waits again; on zero it goes on
         * to PeekMessage. So 0 is "carry on and look", which
         * is what a task that has just been given its turn back
         * should do.
         *
         * [INFO]: s92 (#306): NO LONGER TWO TASKS ONLY. Every task that is not
         * running sits in g_WowSchedSlots; this yield goes ROUND ROBIN to the
         * next one (WowSchedPick). Round robin and not krnl386's own priority
         * order (TDB+0x08): every Win16 task here runs at the same priority, and
         * the yield only happens when the running one
         * has nothing to do. The other new yield is at GetMessage -- see
         * "(E)" at the USER dispatch -- for a task launched and never run.
         */
        else if (frame->Id == WOW32_WOWWAITFORMSGANDEVENT
                 && current != 0 && current != WOWUSER_TASK_NONE16 && !WowSchedInterTaskLive()
                 && (freedSlotIndex = WowSchedPick(current)) >= 0)
        {
            WORD toTask = g_WowSchedSlots[freedSlotIndex].Task;
            /* Written into the WAITING task's frame now; its epilogue
             * reads them whenever it is resumed, off its own stack.
             */
            Wow32SetReturn(frame, 0);
            Wow32PokeWord(frame->FrameBase + WOW32_OFF_MODE, WOW32_MODE_ORDINARY);
            INT isTopLevel = WowSchedTopLevel(&g_WowSchedSlots[freedSlotIndex]);
            WowSchedPoke(g_WowSchedSlots[freedSlotIndex].ModeLinear, WOW32_MODE_ORDINARY);
            WowSchedSwap(&g_WowSchedSlots[freedSlotIndex], tib, modeLinear, current, VDM_BOP_LENGTH);
            if (isTopLevel)
                g_WowSchedCurrentBase = g_WowCallDepth;
            WowSchedSetCurrent(toTask);
            WowTaskChdir(toTask, &cursor);           /* #164 */
            ++g_WowSchedSwitches;
            cursor = LogPut(cursor, "\n     WOWSCHED: task 0x"); cursor = LogHex(cursor, current);
            cursor = LogPut(cursor, " waited for a message -- YIELDING to parked task 0x");
            cursor = LogHex(cursor, toTask);
            cursor = LogPut(cursor, " ([0x228] follows the context)\r\n");
            WowLogFlush(base, &cursor);
            /* EIP is NOT advanced here: the whole context has been
             * replaced, and the resumed one already points where it
             * should. Advancing would step the OTHER task's EIP.
             */
            {
                *cursorIo = cursor;
                *exitCodeOut = 1;
                return HOST_FLOW_RETURN;
            }
        }
    }
    *cursorIo = cursor;
    return HOST_FLOW_NEXT;
}

/* (F) s92 (#306), launch-first: WinExec/LoadModule return only after the new task has run to its first yield, so the parent's next call is where it yields to the child. */
static INT Wow32RunLaunchedTaskFirst(
    PSTR *cursorIo,
    PSTR const base,
    WOW32_FRAME *frame,
    volatile BYTE * const tib,
    INT *exitCodeOut)
{
    PSTR cursor = *cursorIo;

    /* -- (F) s92 (#306): LAUNCH-FIRST. Win16's WinExec/LoadModule does not return
     * before the new task has run to its first yield (on real WOW the task has
     * its own thread and the creator waits for it). USER's WinHelp() depends on
     * it: it starts WINHELP.EXE and at once looks for the "MS_WINHELP" window;
     * with the new task merely parked the window did not exist and Calc and
     * Notepad said "Not enough memory available" (runs/s92/gate3). So the
     * PARENT's next call is where it yields: parked AT this BOP (re-issued on
     * resume), marked runnable at the current callback depth; the child runs,
     * and its first empty GetMessage at that same depth (E) hands back.
     *
     * [CAUTION]: Not for the BOOT launch of WOWEXEC (g_WowSchedShell still 0) -- that creator
     * retires, moment (C), and yielding there killed every launch (gate4) --
     * nor for WOWEXEC's own launches, which every shelf program was measured
     * with. Never inside a C-stack nested run or a modal loop.
     */
    if (g_WowSchedOn && g_WowSchedLaunchChild)
    {
        WORD freshCurrentTask = WowSchedCurrentTask();
        if (freshCurrentTask && freshCurrentTask != WOWUSER_TASK_NONE16 && freshCurrentTask != g_WowSchedLaunchChild)
        {
            WORD child = g_WowSchedLaunchChild;
            INT freshIndex;
            INT freshSlotIndex = -1;
            g_WowSchedLaunchChild = 0;
            for (freshIndex = 0; freshIndex < WOWSCHED_MAX; ++freshIndex)
                if (g_WowSchedSlots[freshIndex].IsUsed && g_WowSchedSlots[freshIndex].IsFresh
                    && g_WowSchedSlots[freshIndex].Task == child)
                    freshSlotIndex = freshIndex;
            if (freshSlotIndex >= 0 && g_WowSchedShell && freshCurrentTask != g_WowSchedShell
                && g_WowWindowNested == 0 && !WowDlgActive() && !WowSchedInterTaskLive())
            {
                DWORD freshModeLinear = (DWORD)(ULONG_PTR)(frame->FrameBase + WOW32_OFF_MODE);
                WowSchedPoke(g_WowSchedSlots[freshSlotIndex].ModeLinear, WOW32_MODE_ORDINARY);
                WowSchedSwap(&g_WowSchedSlots[freshSlotIndex], tib, freshModeLinear, freshCurrentTask, 0);
                g_WowSchedSlots[freshSlotIndex].IsRunnable = 1;          /* the parent, mid-work */
                g_WowSchedSlots[freshSlotIndex].CallbackDepth  = g_WowCallDepth;
                g_WowSchedCurrentBase = g_WowCallDepth;             /* the child's top level */
                WowSchedSetCurrent(child);
                WowTaskChdir(child, &cursor);
                ++g_WowSchedSwitches;
                cursor = LogPut(cursor, "\n     WOWSCHED: task 0x"); cursor = LogHex(cursor, freshCurrentTask);
                cursor = LogPut(cursor, " launched task 0x"); cursor = LogHex(cursor, child);
                cursor = LogPut(cursor, " -- LAUNCH-FIRST at depth 0x"); cursor = LogHex(cursor, (DWORD)g_WowCallDepth);
                cursor = LogPut(cursor, ": the child runs to its first yield before the"
                            " parent's call (id 0x");
                cursor = LogHex(cursor, frame->Id); cursor = LogPut(cursor, ") is serviced (#306)\r\n");
                WowLogFlush(base, &cursor);
                {
                    *cursorIo = cursor;
                    *exitCodeOut = 1;
                    return HOST_FLOW_RETURN;
                }
            }
        }
    }
    *cursorIo = cursor;
    return HOST_FLOW_NEXT;
}

/* Log a WOW32 call's frame: its function id (and name, for krnl386's own thunks), whose table the id
 * belongs to, the stub, and the arguments.
 */
static PSTR Wow32LogFrame(
    PSTR cursor,
    PSTR const base,
    const BYTE bopCode,
    const volatile BYTE * const frameBytes,
    volatile BYTE * const tib,
    SIZE_T const reportSize)
{
    /* [INFO]: THE FUNCTION ID. Every 32-bit call goes through one common
     * thunk, reached by a FAR call from a per-function stub that has
     * pushed its arguments and then its ID (observed in every frame).
     * So bp+2/bp+4 are the return address back into the stub, and
     * bp+6 is the ID the stub pushed last.
     * That is the whole WOW32 interface: a small integer namespace.
     * Naming it here is what turns a wall of identical BOP lines into
     * a list of functions to implement.
     */
    if (bopCode == WOW32_BOP)
    {
        /* [CAUTION]: CORRECTED, AND THE CORRECTION IS THE WHOLE POINT.
         * This used to read the arguments at bp+12 and so printed
         * the CALLER's far return address as the first two argument
         * words. That is why session 30 filed VirtualAlloc's argument
         * ORDER as "not pinned down, two readings possible" -- there
         * was only ever one reading, and the instrument was lying.
         * The arguments are at bp+16: above bp+2..bp+10 (the stub's
         * return, its id, its argument byte count) and the CALLER's far
         * return at bp+12/+14 -- which is where the guest resumes, and
         * the N argument bytes above it are what it discards. See
         * wow32.h for the pinned layout.
         */
        DWORD frameId  = (DWORD)(frameBytes[WOW32_OFF_ID]
                             | (frameBytes[WOW32_OFF_ID + 1] << BYTE_SHIFT));
        DWORD argumentBytes  = (DWORD)(frameBytes[WOW32_OFF_ARGB]
                             | (frameBytes[WOW32_OFF_ARGB + 1] << BYTE_SHIFT));
        DWORD argumentCount   = argumentBytes / WOW_WORD_BYTES;
        DWORD item;
        /* [INFO]: AN ID IS ONLY MEANINGFUL WITH ITS TABLE. The stub lives
         * in the module that owns the numbering, so print that
         * segment and only apply krnl386's names when the stub is
         * krnl386's. This log said "GetProfileInt" over WOWEXEC's
         * RegisterClass for one run, which is how the whole per-module
         * id space came to light -- an instrument must not put a name
         * on something it has not identified.
         */
        DWORD frameSegment = (DWORD)(frameBytes[WOW32_OFF_STUB_SEGMENT] | (frameBytes[WOW32_OFF_STUB_SEGMENT + 1] << BYTE_SHIFT));
        INT   isKernelThunk   = frameSegment == VDM_REG16(tib, VTIB_CS);
        PCSTR thunkName = isKernelThunk ? Wow32Name((WORD)frameId) : NULL;
        cursor = LogPut(cursor, " FUNC=0x"); cursor = LogHex(cursor, frameId);
        if (thunkName)
        {
            cursor = LogPut(cursor, " ");
            cursor = LogPut(cursor, thunkName);
        }
        if (isKernelThunk)
            cursor = LogPut(cursor, " [krnl]");
        else
        {
            INT module = WowModuleOfSelector((WORD)frameSegment);
            cursor = LogPut(cursor, " [");
            /* [CAUTION]: `?` MEANT TWO DIFFERENT THINGS AND THAT COST A
             * READING. WowModuleOfSelector is a BIND-STAGE table:
             * it cannot name a selector krnl386 allocated at run
             * time, so USER's own calls printed as "?'s table"
             * even though the dispatcher had identified that very
             * segment from a stub and was routing them to
             * wowuser.h. A line saying "unknown" about something
             * the host knows is an instrument lying quietly.
             * The two tables we HAVE identified say so by name.
             */
            if (g_WowUserSegment && frameSegment == g_WowUserSegment)
                cursor = LogPut(cursor, "USER");
            else if (g_WowKernel2Segment && frameSegment == g_WowKernel2Segment)
                cursor = LogPut(cursor, "krnl386 seg2");
            else if (g_WowSoundSegment && frameSegment == g_WowSoundSegment)
                cursor = LogPut(cursor, "SOUND");
            else if (g_WowMultimediaSegment && frameSegment == g_WowMultimediaSegment)
                cursor = LogPut(cursor, "MMSYSTEM");
            else
                cursor = LogPut(cursor, module >= 0 ? g_WowName[module] : "?");
            cursor = LogPut(cursor, "'s table -- a DIFFERENT id space]");
        }
        cursor = LogPut(cursor, " stub=0x"); cursor = LogHex(cursor, frameSegment);
        /* WHICH TASK IS CALLING. (GH #128, session 38) (Importance = 1):
         * krnl386 keeps the current task's TDB selector in its DGROUP
         * at offset 0x228 (observed), and at EVERY WOW32 call the
         * guest's own DS already selects DGROUP (observed) -- so it
         * selects the segment this lives in. No base to resolve, nothing that
         * moves between runs, and it costs one read.
         *
         * [INFO]: IT TURNS THE LOG INTO A TASK TIMELINE, which is the thing
         * the frontier needs. WOWEXEC executes, calls WaitEvent(0) --
         * the handshake between InitTask and InitApp in every Win16
         * startup -- and never gives control back, so krnl386's boot
         * task never returns from LoadModule and never unlinks its own
         * bring-up record. That record is what GetExePtr(NULL) then
         * matches, and the #GP that follows is the consequence. Every one of
         * those claims is about WHO WAS
         * RUNNING, and until now the log could not say.
         *
         * [CAUTION]: It is a READING, not a lever. krnl386 reacts when this word
         * differs across a BOP, which reads like an invitation to
         * schedule by writing it -- but measured, what that does is
         * restore the caller after someone else ran. Writing it here
         * would park the wrong stack in the wrong TDB.
         */
        {   DWORD instanceBase = DpmiSelectorBase((WORD)(VDM_REG(tib, VTIB_DS)
                                             & WORD_MASK));
            if (instanceBase)
            {
                const volatile BYTE *dgroup =
                    (const volatile BYTE *)(ULONG_PTR)instanceBase;
                cursor = LogPut(cursor, " task=0x");
                cursor = LogHex(cursor, (DWORD)(dgroup[0x228] | (dgroup[0x229] << BYTE_SHIFT)));
                /* [INFO]: And keep it where USER's GetWindowTask can
                 * see it -- the same word, read once.
                 */
                g_WowUserCurrentTask = (WORD)(dgroup[WOWUSER_KRNL_CURRENT_TASK] | (dgroup[WOWUSER_KRNL_CURRENT_TASK + 1] << BYTE_SHIFT));
            }
        }
        /* -- AND WHICH EPILOGUE THIS CALL WILL RETURN THROUGH.
         * The mode word at bp-24 picks how the call returns
         * (see WOW32_OFF_MODE in wow32.h). krnl386 always passes
         * 0 (observed), so this MUST read `mode=0` on
         * every line -- and the day something writes it, the log
         * says so instead of the guest quietly returning
         * somewhere else. Predict the number before the run.
         */
        cursor = LogPut(cursor, " mode=");
        cursor = LogHex(cursor, (DWORD)(frameBytes[WOW32_OFF_MODE]
                            | (frameBytes[WOW32_OFF_MODE + 1] << BYTE_SHIFT)));
        cursor = LogPut(cursor, " args=");   cursor = LogHex(cursor, argumentBytes);
        cursor = LogPut(cursor, "b retstub=0x");
        cursor = LogHex(cursor, (DWORD)(frameBytes[2] | (frameBytes[3] << BYTE_SHIFT)));
        /* [CAUTION]: A CALL SITE IS A SEGMENT AND AN OFFSET. This printed the
         * offset alone, and every reader (including this session)
         * assumed segment 1 -- krnl386's main segment, where most of
         * them are. Session 34 chased `from=0x09bf` into seg1, found
         * it landing mid-instruction, and only then looked at the
         * frame word above it: the caller was 0x01d7, a DIFFERENT
         * segment of krnl386. An instrument that names half an address
         * invites looking in the wrong module.
         */
        cursor = LogPut(cursor, " from=0x");
        cursor = LogHex(cursor, (DWORD)(frameBytes[14] | (frameBytes[15] << BYTE_SHIFT)));
        cursor = LogPut(cursor, ":0x");
        cursor = LogHex(cursor, (DWORD)(frameBytes[12] | (frameBytes[13] << BYTE_SHIFT)));
        if (argumentCount && argumentCount <= 16)
        {
            cursor = LogPut(cursor, " (");
            for (item = 0; item < argumentCount; ++item)
            {
                if (item)
                    cursor = LogPut(cursor, " ");
                cursor = LogHex(cursor, (DWORD)(frameBytes[WOW32_OFF_ARGS + item * WOW_WORD_BYTES]
                             | (frameBytes[WOW32_OFF_ARGS + item * WOW_WORD_BYTES + 1] << BYTE_SHIFT)));
            }
            cursor = LogPut(cursor, ")");
            /* krnl386 SAYS WHY IT IS GIVING UP, AND WE NEVER READ IT (Importance = 2):
             * Its last act before ExitKernelThunk is WOW32 0xc4 -- the fatal
             * error box -- and two of the words in this frame are a far
             * pointer to the message. Session 34 got "NTVDM KERNEL: Missing
             * 16-bit system module" only by taking the pointer out of the log
             * by hand and reading the string it named. Several different
             * failures end in this same call; guessing which one from
             * surrounding behaviour is
             * exactly the reasoning-instead-of-measuring this project keeps
             * paying for. Print it.
             *
             * [CAUTION]: NO ASSUMED FRAME. Which words hold the pointer is not fixed by
             * anything we have measured -- only that some adjacent pair does.
             * So try every (offset, segment) pair, and print only the ones
             * that resolve to a real selector AND look like text. A pair that
             * is not a string prints nothing rather than a plausible lie.
             */
            /* [CAUTION]: AND NOT ONLY FOR ID 0xc4. (session 37) (Importance = 1):
             * This was gated on krnl386's MessageBox, and the very
             * next message the guest tried to show came through a
             * DIFFERENT id from a DIFFERENT module (0x140, same
             * 7-word frame, same 0x8008 style) -- so the run printed
             * seven hex words where it could have printed
             * "Application Error". A string argument is worth
             * decoding whoever is passing it; the scan already
             * refuses anything that is not a NUL-terminated run of
             * text through a selector that resolves, so a non-string
             * still prints nothing rather than a plausible lie.
             */
            if (argumentCount >= 2)
            {
                for (item = 0; item + 1 < argumentCount; ++item)
                {
                    DWORD argumentOffset = (DWORD)(frameBytes[WOW32_OFF_ARGS + item * WOW_WORD_BYTES]
                                  | (frameBytes[WOW32_OFF_ARGS + item * WOW_WORD_BYTES + 1] << BYTE_SHIFT));
                    DWORD argumentSelector = (DWORD)(frameBytes[WOW32_OFF_ARGS + item * WOW_WORD_BYTES + 2]
                                  | (frameBytes[WOW32_OFF_ARGS + item * WOW_WORD_BYTES + 3] << BYTE_SHIFT));
                    DWORD abase = argumentSelector ? DpmiSelectorBase((WORD)argumentSelector) : 0;
                    const volatile BYTE *sourceBytes;
                    UINT length2 = 0;
                    if (!argumentSelector || !abase)
                        continue;
                    sourceBytes = (const volatile BYTE *)(ULONG_PTR)(abase + argumentOffset);
                    if (!HostReadable((const VOID *)sourceBytes, 8))
                        continue;
                    /* -- [CAUTION] TAB AND CRLF ARE PART OF THE MESSAGE, NOT THE END
                     *   OF IT. (session 36) ----------------------------
                     * This scan accepted only 0x20..0x7E, and the ONE
                     * string in this frame that names the actual fault --
                     * the formatted body, "Please re-install the following
                     * module to your system32 directory:\r\n\t\t<NAME>" --
                     * has a `\r` at offset 66. So the walk stopped there,
                     * `s[n2] != 0` rejected it as "not a C string", and the
                     * log printed only the CAPTION, which is the one part of
                     * the message that is the same for every missing module.
                     * The run named the class of failure and withheld the
                     * instance -- and the instance is the whole question.
                     *
                     * [CAUTION]: Accept them in the SCAN, escape them in the OUTPUT: a
                     * raw CRLF here would split one log line into three and
                     * make the message look like unrelated records.
                     */
                    while (length2 < 120 && ((sourceBytes[length2] >= 0x20 && sourceBytes[length2] < 0x7F)
                                        || sourceBytes[length2] == '\t' || sourceBytes[length2] == '\r'
                                        || sourceBytes[length2] == '\n'))
                        ++length2;
                    if (length2 < 6 || sourceBytes[length2] != 0)
                        continue;                                             /* not a C string */
                    /* Escaping can double a 120-char string, and several args
                     * can match. Stop before `report[2048]` overflows -- an
                     * instrument that corrupts its own stack to print one more
                     * message is worse than one that prints fewer.
                     */
                    if ((SIZE_T)(cursor - base) > reportSize - 400)
                        break;
                    cursor = LogPut(cursor, "\r\n    ★ arg["); cursor = LogHex(cursor, item);
                    cursor = LogPut(cursor, "] 0x"); cursor = LogHex(cursor, argumentSelector);
                    cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, argumentOffset);
                    cursor = LogPut(cursor, " = \"");
                    { UINT index2;
                      for (index2 = 0; index2 < length2; ++index2)
                      {
                          BYTE byteCharacter = sourceBytes[index2];
                          if      (byteCharacter == '\t')
                          {
                              *cursor++ = '\\';
                              *cursor++ = 't';
                          }
                          else if (byteCharacter == '\r')
                          {
                              *cursor++ = '\\';
                              *cursor++ = 'r';
                          }
                          else if (byteCharacter == '\n')
                          {
                              *cursor++ = '\\';
                              *cursor++ = 'n';
                          }
                          else
                              *cursor++ = (CHAR)byteCharacter;
                      } }
                    cursor = LogPut(cursor, "\"");
                }
            }
        }
    }
    return cursor;
}

/* The WOW32 BOP: a thunk calling out to the host. Decode its frame, let the scheduler act, then service the id by module -- KERNEL's own, krnl386's second table, USER, SHELL, COMMDLG, GDI -- or through Wow32Call, stepping past the BOP. */
static INT Wow32ServiceBop(
    PSTR *cursorIo,
    PSTR const base,
    const BYTE bopCode,
    volatile BYTE * const tib,
    DOS_MACHINE * const machine,
    DWORD *wowStaleIo,
    INT *wowStaleOkIo,
    DWORD *wowAnswerIo,
    INT *exitCodeOut)
{
    PSTR cursor = *cursorIo;
    INT commandTakenBefore = 0;
    DWORD wowStale = *wowStaleIo;
    INT wowStaleOk = *wowStaleOkIo;
    DWORD wowAnswer = *wowAnswerIo;

    if (bopCode == WOW32_BOP)
    {
        DWORD wow32StackBase = DpmiSelectorBase((WORD)VDM_REG16(tib, VTIB_SS));
        WOW32_FRAME frame;
        frame.FrameBase      = (volatile BYTE *)(ULONG_PTR)
                    (wow32StackBase + VDM_REG16(tib, VTIB_EBP));
        frame.Id      = Wow32PeekWord(frame.FrameBase + WOW32_OFF_ID);
        frame.ArgumentBytes    = Wow32PeekWord(frame.FrameBase + WOW32_OFF_ARGB);
        frame.CallSite    = Wow32PeekWord(frame.FrameBase + WOW32_OFF_FROM);
        frame.StubSegment = Wow32PeekWord(frame.FrameBase + WOW32_OFF_STUB_SEGMENT);          /* the stub's own segment */
        /* [INFO]: WHOSE ID SPACE IS THIS? The per-function stub lives in the module
         * that owns the numbering, so resolving its segment against
         * krnl386's own segment bases answers it exactly. Everything we
         * know about this interface came out of krnl386, so a call that did
         * NOT come through krnl386's table gets the honest "unimplemented"
         * rather than an answer from the wrong function.
         */
        /* [CAUTION]: NOT via g_WowPmBase[]: that table is filled by a descriptor-limit
         * match that has not fired yet when the earliest calls arrive, so a
         * gate keyed on it rejected EVERYTHING and the run collapsed from 258
         * calls to 3. Measured, not reasoned about.
         *
         * The BOP itself is the reference. It lives in krnl386's own common
         * thunk (observed), so the executing CS at a BOP IS krnl386's
         * code segment -- and a stub in that same segment is krnl386's stub.
         * Exact, self-contained, and true from the first call onward.
         */
        frame.IsKernel = (frame.StubSegment == (WORD)VDM_REG16(tib, VTIB_CS));
        frame.SelectorToLinear = Wow32HostSelectorToLinear;
        frame.Context     = NULL;
        frame.Result     = 0;
        frame.IsServiced = 0;
        /* A service may ASK for a 16-bit call; whether one happens is the
         * host's decision, and `cbok` is where that decision is made.
         */
        frame.IsCallbackAllowed    = g_WowCallOn;
        frame.CallbackProcedure  = 0;
        frame.CallbackDataSelector    = 0;
        frame.CallbackArgumentCount  = 0;
        frame.CallbackSink = NULL;
        frame.CallbackWindow  = 0;
        frame.CallbackMessage = 0;
        frame.CallbackBlobLength = 0;
        frame.CallbackBlobArgument = -1;
        frame.CallbackReturnMode   = WOWCALL_RET_KEEP;
        /* [CAUTION]: AND THESE TWO, WHICH COST A RUN BY BEING LEFT OUT. `f` is a
         * stack local, so an un-set field is whatever was there before
         * -- and `cbact` is read as a DECISION about what the host does
         * with a returned value. Uninitialised, it made a WM_CREATE
         * callback and a LocalAlloc callback both run the EDIT-text
         * action, following a pointer that was never a pointer and
         * issuing a LocalUnlock against a lock nobody had taken. Every
         * field of this frame is initialised here for that reason.
         */
        frame.CallbackAction   = WOWCALL_ACT_NONE;
        frame.CallbackActionArgument = 0;
        frame.IsEnumerationRequested  = 0;                /* ...and this one too */
        frame.GuestDataSelector      = (WORD)VDM_REG16(tib, VTIB_DS);
        frame.IsModalDialog = 0;                /* ...and this one, for the same
                                          reason: it decides whether the
                                          guest is resumed at all */
        /* [INFO]: krnl386's segment 1 as a LIVE selector, for the day a
         * service needs to call a KERNEL export. The WOW32 common
         * thunk is IN that segment, so the CS at this BOP is it --
         * exact, free, and true from the first call onward.
         */
        if (frame.IsKernel)
            g_WowUserKernelSegment = (WORD)VDM_REG16(tib, VTIB_CS);
        g_WowLastId = (WORD)frame.Id;
        g_WowLastFrom = frame.CallSite;
        /* THE EPILOGUE-MODE EXPERIMENT (wowmode.txt) (Importance = 2):
         * Written BEFORE anything is serviced, because the guest reads
         * the mode after the BOP whatever we do here -- a stepped-over
         * call returns through it just as a serviced one does, and the
         * one call this exists for (0x74, the task launch) is stepped
         * over. See WOW32_OFF_MODE in wow32.h for what the modes are and
         * why krnl386 itself never sets one.
         */
        {   INT modeOverride = Wow32ModeOverride((WORD)frame.Id);
            if (modeOverride >= 0)
            {
                Wow32PokeWord(frame.FrameBase + WOW32_OFF_MODE, (WORD)modeOverride);
                cursor = LogPut(cursor, "\n     ** wowmode.txt OVERRIDE: returning"
                            " through epilogue mode ");
                cursor = LogHex(cursor, (DWORD)modeOverride);
                cursor = LogPut(cursor, " -- an EXPERIMENT, not a service **");
            }
        }
        /* THE SCHEDULER'S TWO BOP HOOKS. See src/wow/wowsched.h (Importance = 3):
         * DS is krnl386's DGROUP at every WOW32 BOP (observed on every
         * call), which is the only reason the
         * current-task word is reachable from outside a call. Learn it
         * unconditionally -- it costs nothing and the fault hook, which
         * runs where DS is anybody's, depends on having it.
         */
        g_WowDgroupSelector = (WORD)VDM_REG16(tib, VTIB_DS);
        if (g_WowSchedOn && !g_WowCallRetarget)       /* s92 #306: inter-task calls */
        {
            g_WowCallCurrentTask = WowSchedCurrentTask;
            g_WowCallRetarget = WowSchedRetarget;
            g_WowCallUntarget = WowSchedUntarget;
        }
        {
            INT exitCode;
            INT flow = Wow32RunLaunchedTaskFirst(&cursor, base, &frame, tib, &exitCode);
            if (flow == HOST_FLOW_RETURN)
            {
                *cursorIo = cursor;
                *wowStaleIo = wowStale;
                *wowStaleOkIo = wowStaleOk;
                *wowAnswerIo = wowAnswer;
                *exitCodeOut = exitCode;
                return HOST_FLOW_RETURN;
            }
        }
        {
            INT exitCode;
            INT flow = Wow32ScheduleKernelCall(&cursor, base, &frame, tib, &exitCode);
            if (flow == HOST_FLOW_RETURN)
            {
                *cursorIo = cursor;
                *wowStaleIo = wowStale;
                *wowStaleOkIo = wowStaleOk;
                *wowAnswerIo = wowAnswer;
                *exitCodeOut = exitCode;
                return HOST_FLOW_RETURN;
            }
        }
        /* -- #306 (s90): WowWaitForMsgAndEvent WITH NOBODY TO YIELD TO MUST
         * WAIT. The arm above handles it when another task is parked; with
         * none it fell through to "unimplemented", answered 0 instantly, and
         * the guest's idle loop (wait -> PeekMessage -> wait) ran at 100% CPU
         * -- measured after Calc closed with its Help task alive: 0x3e46a
         * stepped-over calls, all of them this one.
         * 0 is still the answer (see above: "carry on and look"); what was
         * missing is the block before it. Real windows
         * are pumped (that is where a Win16 message comes from), then up to
         * 50 ms of waiting for input -- the same bound and reasoning as the
         * GetMessage wait -- and any IRQ a 32-bit component raised.
         */
        if (frame.IsKernel && frame.Id == WOW32_WOWWAITFORMSGANDEVENT)
        {
            if (!g_WowMsgCount && !WowWinPump(WOW_PUMP_BUDGET))
                MsgWaitForMultipleObjects(0, NULL, FALSE, WOW_INPUT_WAIT_MS, QS_ALLINPUT);
            if (g_IcaPending)
                WowIcaDeliver(g_DosMachine, tib, 0);
            Wow32SetReturn(&frame, 0);
            ++g_Wow32Serviced;
            ++g_WowIdleWaits;
            VDM_REG(tib, VTIB_EIP) += VDM_BOP_LENGTH;
            if (g_WowIdleWaits <= 4)
            {
                cursor = LogPut(cursor, " -> SERVICED: idle wait (no parked task to yield to),"
                            " answered 0 after up to 50 ms\r\n");
                WowLogFlush(base, &cursor);
            }
            else
                cursor = base;
            {
                *cursorIo = cursor;
                *wowStaleIo = wowStale;
                *wowStaleOkIo = wowStaleOk;
                *wowAnswerIo = wowAnswer;
                *exitCodeOut = 1;
                return HOST_FLOW_RETURN;
            }
        }
        {
            INT exitCode;
            INT flow = Wow32ServiceFileRead(&cursor, base, &frame, machine, tib, &exitCode);
            if (flow == HOST_FLOW_RETURN)
            {
                *cursorIo = cursor;
                *wowStaleIo = wowStale;
                *wowStaleOkIo = wowStaleOk;
                *wowAnswerIo = wowAnswer;
                *exitCodeOut = exitCode;
                return HOST_FLOW_RETURN;
            }
        }
        {
            INT exitCode;
            INT flow = Wow32ServiceGetCurrentDirectory(&cursor, base, &frame, tib, machine, &exitCode);
            if (flow == HOST_FLOW_RETURN)
            {
                *cursorIo = cursor;
                *wowStaleIo = wowStale;
                *wowStaleOkIo = wowStaleOk;
                *wowAnswerIo = wowAnswer;
                *exitCodeOut = exitCode;
                return HOST_FLOW_RETURN;
            }
        }
        {
            INT exitCode;
            INT flow = Wow32ServiceResolveModulePath(&cursor, base, &frame, tib, &exitCode);
            if (flow == HOST_FLOW_RETURN)
            {
                *cursorIo = cursor;
                *wowStaleIo = wowStale;
                *wowStaleOkIo = wowStaleOk;
                *wowAnswerIo = wowAnswer;
                *exitCodeOut = exitCode;
                return HOST_FLOW_RETURN;
            }
        }
        /* ExitKernelThunk: krnl386 SAYS IT IS DONE, SO STOP (Importance = 1):
         * krnl386 does not expect to be returned to from this call:
         * stepped over, the guest's next instruction is a deliberate UD0
         * (`0f ff`, in the fault bytes). That UD0 is reflected to its own
         * handler, which sets the vector again and faults again: a run
         * that has ENDED then fills the log to its 268 MB cap, and every
         * one of those bytes is after the last thing that happened.
         *
         * End the run where the guest ended it, and say which code.
         */
        if (frame.IsKernel && frame.Id == WOW32_EXITKERNELTHUNK)
        {
            cursor = LogPut(cursor, "\n     ★ ExitKernelThunk(0x");
            cursor = LogHex(cursor, Wow32ArgWord(&frame, 0));
            cursor = LogPut(cursor, ") -- krnl386 is shutting the VDM down; ending the run"
                        " here rather than looping on the UD0 behind it\r\n");
            WowLogFlush(base, &cursor);
            {
                *cursorIo = cursor;
                *wowStaleIo = wowStale;
                *wowStaleOkIo = wowStaleOk;
                *wowAnswerIo = wowAnswer;
                *exitCodeOut = -1;
                return HOST_FLOW_RETURN;
            }
        }
        /* -- krnl386's SEGMENT-2 TABLE. Learn its selector from a stub.
         * Same shape as USER's anchor below, and for the same reason:
         * the id space is per TABLE, so nothing here may be answered
         * until the table has identified itself. See WowKernel2Stub.
         */
        if (!frame.IsKernel && !g_WowKernel2Segment && frame.StubSegment != g_WowUserSegment
            && WowKernel2Stub(frame.Id, Wow32PeekWord(frame.FrameBase + WOW32_OFF_RETURN_STUB)))
        {
            g_WowKernel2Segment = frame.StubSegment;
            cursor = LogPut(cursor, "\n     WOWKRNL2: krnl386's SECOND stub table is in"
                        " segment 0x");
            cursor = LogHex(cursor, g_WowKernel2Segment);
            cursor = LogPut(cursor, " (learned from the stub's own bytes in the file)");
        }
        {
            INT exitCode;
            INT flow = Wow32ServiceKernelSecondTable(&cursor, base, &frame, tib, &exitCode);
            if (flow == HOST_FLOW_RETURN)
            {
                *cursorIo = cursor;
                *wowStaleIo = wowStale;
                *wowStaleOkIo = wowStaleOk;
                *wowAnswerIo = wowAnswer;
                *exitCodeOut = exitCode;
                return HOST_FLOW_RETURN;
            }
        }
        /* USER'S OWN ID SPACE. See src/wow/wowuser.h (Importance = 3):
         * Deliberately a separate dispatcher behind a separate check:
         * `0x39` is GetProfileInt in krnl386's table and RegisterClass
         * in USER's, and one switch holding both id spaces is exactly
         * how this host came to answer the second with the first.
         */
        if (!frame.IsKernel && !g_WowUserSegment
            && WowUserAnchor(frame.Id, frame.ArgumentBytes, Wow32PeekWord(frame.FrameBase + WOW32_OFF_RETURN_STUB)))
        {
            g_WowUserSegment = frame.StubSegment;
            cursor = LogPut(cursor, "\n     WOWUSER: USER's code segment is 0x");
            cursor = LogHex(cursor, g_WowUserSegment);
            cursor = LogPut(cursor, " (learned from its own stub, not from the module table)");
        }
        {
            INT exitCode;
            INT flow = Wow32ServiceUser(&cursor, base, &frame, tib, &exitCode);
            if (flow == HOST_FLOW_RETURN)
            {
                *cursorIo = cursor;
                *wowStaleIo = wowStale;
                *wowStaleOkIo = wowStaleOk;
                *wowAnswerIo = wowAnswer;
                *exitCodeOut = exitCode;
                return HOST_FLOW_RETURN;
            }
        }
        /* SHELL.DLL'S OWN ID SPACE. See src/wow/wowshell.h (Importance = 3):
         * A fourth table, behind a fourth check, for the reason the
         * third one exists: `0x16` is SetFocus in USER's numbering and
         * ShellAbout in SHELL's, and the two must never meet. The
         * anchor and the service are the SAME call -- the table names
         * itself with the first thing we are asked to do out of it.
         */
        if (!frame.IsKernel && !g_WowShellSegment
            && frame.StubSegment != g_WowUserSegment && frame.StubSegment != g_WowKernel2Segment
            && WowShellAnchor(frame.Id, frame.ArgumentBytes, Wow32PeekWord(frame.FrameBase + WOW32_OFF_RETURN_STUB)))
        {
            g_WowShellSegment = frame.StubSegment;
            cursor = LogPut(cursor, "\n     WOWSHELL: SHELL.DLL's code segment is 0x");
            cursor = LogHex(cursor, g_WowShellSegment);
            cursor = LogPut(cursor, " (learned from its own stub, not from the module"
                        " table)");
        }
        {
            INT exitCode;
            INT flow = Wow32ServiceShell(&cursor, base, &frame, tib, &exitCode);
            if (flow == HOST_FLOW_RETURN)
            {
                *cursorIo = cursor;
                *wowStaleIo = wowStale;
                *wowStaleOkIo = wowStaleOk;
                *wowAnswerIo = wowAnswer;
                *exitCodeOut = exitCode;
                return HOST_FLOW_RETURN;
            }
        }
        /* -- COMMDLG.DLL'S OWN ID SPACE. See src/wow/wowcommdlg.h. -
         * The fifth table, behind the fifth check. Same shape as
         * SHELL's: the anchor and the service are the same call, so
         * nothing is answered before the table has named itself.
         */
        if (!frame.IsKernel && !g_WowCommonDialogSegment
            && frame.StubSegment != g_WowUserSegment && frame.StubSegment != g_WowKernel2Segment
            && frame.StubSegment != g_WowShellSegment
            && WowCommonDialogAnchor(frame.Id, frame.ArgumentBytes, Wow32PeekWord(frame.FrameBase + WOW32_OFF_RETURN_STUB)))
        {
            g_WowCommonDialogSegment = frame.StubSegment;
            cursor = LogPut(cursor, "\n     WOWCOMMDLG: COMMDLG.DLL's code segment is 0x");
            cursor = LogHex(cursor, g_WowCommonDialogSegment);
            cursor = LogPut(cursor, " (learned from its own stub, not from the module"
                        " table)");
        }
        {
            INT exitCode;
            INT flow = Wow32ServiceCommonDialog(&cursor, base, &frame, tib, &exitCode);
            if (flow == HOST_FLOW_RETURN)
            {
                *cursorIo = cursor;
                *wowStaleIo = wowStale;
                *wowStaleOkIo = wowStaleOk;
                *wowAnswerIo = wowAnswer;
                *exitCodeOut = exitCode;
                return HOST_FLOW_RETURN;
            }
        }
        /* KEYBOARD.DRV'S OWN ID SPACE. See src/wow/wowkbd.h (Importance = 2):
         * The sixth table, behind the sixth check, same shape as the
         * two before it.
         */
        if (!frame.IsKernel && !g_WowKeyboardSegment
            && frame.StubSegment != g_WowUserSegment && frame.StubSegment != g_WowKernel2Segment
            && frame.StubSegment != g_WowShellSegment && frame.StubSegment != g_WowCommonDialogSegment
            && WowKeyboardAnchor(frame.Id, frame.ArgumentBytes, Wow32PeekWord(frame.FrameBase + WOW32_OFF_RETURN_STUB)))
        {
            g_WowKeyboardSegment = frame.StubSegment;
            cursor = LogPut(cursor, "\n     WOWKBD: KEYBOARD.DRV's code segment is 0x");
            cursor = LogHex(cursor, g_WowKeyboardSegment);
            cursor = LogPut(cursor, " (learned from its own stub, not from the module"
                        " table)");
        }
        if (!frame.IsKernel && g_WowKeyboardSegment && frame.StubSegment == g_WowKeyboardSegment)
        {
            CHAR note[320];
            if (WowKeyboardCall(&frame, note, sizeof note))
            {
                ++g_Wow32Serviced;
                VDM_REG(tib, VTIB_EIP) += VDM_BOP_LENGTH;
                cursor = LogPut(cursor, " -> SERVICED (KEYBOARD), returned 0x");
                cursor = LogHex(cursor, frame.Result);
                if (note[0])
                {
                    cursor = LogPut(cursor, " -- ");
                    cursor = LogPut(cursor, note);
                }
                cursor = LogPut(cursor, "\r\n");
                WowLogFlush(base, &cursor);
                {
                    *cursorIo = cursor;
                    *wowStaleIo = wowStale;
                    *wowStaleOkIo = wowStaleOk;
                    *wowAnswerIo = wowAnswer;
                    *exitCodeOut = 1;
                    return HOST_FLOW_RETURN;
                }
            }
        }
        /* GDI.EXE'S OWN ID SPACE. See src/wow/wowgdi.h (Importance = 2):
         * The seventh table, and the one MS Paint lives behind.
         */
        if (!frame.IsKernel && !g_WowGdiSegment
            && frame.StubSegment != g_WowUserSegment && frame.StubSegment != g_WowKernel2Segment
            && frame.StubSegment != g_WowShellSegment && frame.StubSegment != g_WowCommonDialogSegment
            && frame.StubSegment != g_WowKeyboardSegment
            && WowGdiAnchor(frame.Id, frame.ArgumentBytes, Wow32PeekWord(frame.FrameBase + WOW32_OFF_RETURN_STUB)))
        {
            g_WowGdiSegment = frame.StubSegment;
            cursor = LogPut(cursor, "\n     WOWGDI: GDI.EXE's code segment is 0x");
            cursor = LogHex(cursor, g_WowGdiSegment);
            cursor = LogPut(cursor, " (learned from its own stub, not from the module"
                        " table)");
        }
        {
            INT exitCode;
            INT flow = Wow32ServiceGdi(&cursor, base, &frame, tib, &exitCode);
            if (flow == HOST_FLOW_RETURN)
            {
                *cursorIo = cursor;
                *wowStaleIo = wowStale;
                *wowStaleOkIo = wowStaleOk;
                *wowAnswerIo = wowAnswer;
                *exitCodeOut = exitCode;
                return HOST_FLOW_RETURN;
            }
        }
        /* -- SOUND.DRV'S OWN ID SPACE (s90, #299). See src/wow/wowsound.h. */
        if (!frame.IsKernel && !g_WowSoundSegment
            && frame.StubSegment != g_WowUserSegment && frame.StubSegment != g_WowKernel2Segment
            && frame.StubSegment != g_WowShellSegment && frame.StubSegment != g_WowCommonDialogSegment
            && frame.StubSegment != g_WowKeyboardSegment && frame.StubSegment != g_WowGdiSegment
            && WowSoundAnchor(frame.Id, frame.ArgumentBytes, Wow32PeekWord(frame.FrameBase + WOW32_OFF_RETURN_STUB)))
        {
            g_WowSoundSegment = frame.StubSegment;
            cursor = LogPut(cursor, "\n     WOWSOUND: SOUND.DRV's code segment is 0x");
            cursor = LogHex(cursor, g_WowSoundSegment);
            cursor = LogPut(cursor, " (learned from its own stub, not from the module"
                        " table)");
        }
        if (!frame.IsKernel && g_WowSoundSegment && frame.StubSegment == g_WowSoundSegment)
        {
            CHAR note[160];
            if (WowSoundCall(&frame, note, sizeof note))
            {
                ++g_Wow32Serviced;
                VDM_REG(tib, VTIB_EIP) += VDM_BOP_LENGTH;
                cursor = LogPut(cursor, " -> SERVICED (SOUND), returned 0x");
                cursor = LogHex(cursor, frame.Result);
                if (note[0])
                {
                    cursor = LogPut(cursor, " -- ");
                    cursor = LogPut(cursor, note);
                }
                cursor = LogPut(cursor, "\r\n");
                WowLogFlush(base, &cursor);
                {
                    *cursorIo = cursor;
                    *wowStaleIo = wowStale;
                    *wowStaleOkIo = wowStaleOk;
                    *wowAnswerIo = wowAnswer;
                    *exitCodeOut = 1;
                    return HOST_FLOW_RETURN;
                }
            }
        }
        /* -- MMSYSTEM'S TWO IDS (s90, #278). See src/wow/wowmmedia.h. */
        if (!frame.IsKernel && !g_WowMultimediaSegment
            && frame.StubSegment != g_WowUserSegment && frame.StubSegment != g_WowKernel2Segment
            && frame.StubSegment != g_WowShellSegment && frame.StubSegment != g_WowCommonDialogSegment
            && frame.StubSegment != g_WowKeyboardSegment && frame.StubSegment != g_WowGdiSegment
            && frame.StubSegment != g_WowSoundSegment
            && WowAnchorHit(g_WowMmediaAnchors,
                              (INT)(sizeof g_WowMmediaAnchors / sizeof g_WowMmediaAnchors[0]),
                              frame.Id, frame.ArgumentBytes, Wow32PeekWord(frame.FrameBase + WOW32_OFF_RETURN_STUB)))
        {
            g_WowMultimediaSegment = frame.StubSegment;
            cursor = LogPut(cursor, "\n     WOWMMEDIA: MMSYSTEM's stub segment is 0x");
            cursor = LogHex(cursor, g_WowMultimediaSegment);
        }
        if (!frame.IsKernel && g_WowMultimediaSegment && frame.StubSegment == g_WowMultimediaSegment)
        {
            CHAR note[200];
            if (WowMultimediaCall(&frame, note, sizeof note))
            {
                ++g_Wow32Serviced;
                VDM_REG(tib, VTIB_EIP) += VDM_BOP_LENGTH;
                cursor = LogPut(cursor, " -> SERVICED (MMSYSTEM), returned 0x");
                cursor = LogHex(cursor, frame.Result);
                if (note[0])
                {
                    cursor = LogPut(cursor, " -- ");
                    cursor = LogPut(cursor, note);
                }
                cursor = LogPut(cursor, "\r\n");
                WowLogFlush(base, &cursor);
                {
                    *cursorIo = cursor;
                    *wowStaleIo = wowStale;
                    *wowStaleOkIo = wowStaleOk;
                    *wowAnswerIo = wowAnswer;
                    *exitCodeOut = 1;
                    return HOST_FLOW_RETURN;
                }
            }
        }
        /* Snapshot before the dispatch: the handler sets this when it
         * hands the program over, and "delivered now" and "delivered
         * earlier" are different events that must not read the same.
         */
        commandTakenBefore = g_WowCommandIsTaken;
        if (Wow32Call(&frame, &g_WowDosData))
        {
            /* [CAUTION]: A DECLINE IS NOT A SERVICE and the log must not blur
             * them. "krnl386 got further because we answered 9 calls"
             * and "because we told it 7 times to ask DOS instead" are
             * different claims about the same run, and only separate
             * counters can tell them apart afterwards.
             */
            INT isDeclined = (frame.Result == WOW32_DECLINE
                        && Wow32MayDecline(frame.Id, frame.CallSite));
            if (isDeclined)
                ++g_Wow32Declined;
            else
                ++g_Wow32Serviced;
            VDM_REG(tib, VTIB_EIP) += VDM_BOP_LENGTH;
            cursor = LogPut(cursor, isDeclined ? " -> DECLINED (krnl386 will chain to real"
                               " DOS) 0x" : " -> SERVICED, returned 0x");
            cursor = LogHex(cursor, frame.Result);
            if (frame.Id == WOW32_REGISTERDOSDATA)
            {
                cursor = LogPut(cursor, " (DOS data area at 0x");
                cursor = LogHex(cursor, g_WowDosData.FarPointer); cursor = LogPut(cursor, ")");
            }
            /* [INFO]: SAY WHAT WAS HANDED OVER, not just that something was.
             * This one call decides which program the VDM runs, and a
             * line reading "returned 0x1" would leave the single most
             * important fact of the run unrecorded.
             */
            if (frame.Id == WOW32_WOWGETNEXTVDMCOMMAND)
            {
                if (g_WowCommandProgram[0])
                {
                    cursor = LogPut(cursor, " -- LAUNCH ["); cursor = LogPut(cursor, g_WowCommandProgram);
                    if (g_WowCommandArguments[0])
                    {
                        cursor = LogPut(cursor, "] args["); cursor = LogPut(cursor, g_WowCommandArguments);
                    }
                    cursor = LogPut(cursor, "]");
                    if (commandTakenBefore) cursor = LogPut(cursor, " -- ALREADY DELIVERED,"
                                                      " answered \"nothing more\"");
                }
                else
                {
                    cursor = LogPut(cursor, " -- no command (this VDM was not told what"
                                " Win16 program to run)");
                }
            }
            cursor = LogPut(cursor, "\r\n");
            WowLogFlush(base, &cursor);
            {
                *cursorIo = cursor;
                *wowStaleIo = wowStale;
                *wowStaleOkIo = wowStaleOk;
                *wowAnswerIo = wowAnswer;
                *exitCodeOut = 1;
                return HOST_FLOW_RETURN;
            }
        }
        ++g_Wow32Unimplemented;
        /* Read the litter FIRST, then overwrite it -- the value that would
         * have been used is evidence, and it is gone a line later.
         */
        wowStale = Wow32PeekReturn(&frame);
        wowStaleOk = 1;
        wowAnswer = Wow32ReturnOverride(frame.Id);   /* wow32ret.txt, if any */
        Wow32SetReturn(&frame, wowAnswer);            /* see WOW32_UNIMPL_RET */
    }
    *cursorIo = cursor;
    *wowStaleIo = wowStale;
    *wowStaleOkIo = wowStaleOk;
    *wowAnswerIo = wowAnswer;
    return HOST_FLOW_NEXT;
}

/* PM INT 21h AH=55h/26h: create a (child) PSP for a PM client. */
static INT DpmiInt21CreatePsp(
    PSTR *cursorIo,
    PSTR const base,
    const DWORD ah,
    volatile BYTE * const tib,
    INT *exitCodeOut)
{
    PSTR cursor = *cursorIo;

    /* AH=55h / 26h: CREATE A PSP, IN PROTECTED MODE (Importance = 4):
     * krnl386 builds one of these per Win16 task -- measured, two
     * per run: DX=0x03bf for WOWEXEC and DX=0x0ad7 for SYSEDIT --
     * and BOTH were landing in the TODO arm, so neither task ever
     * got a PSP at all.
     *
     * [INFO]: AND THAT IS THE NULL-ES FAULT, TRACED END TO END. SYSEDIT
     * reads the ENVIRONMENT segment from its own PSP+0x2c; it treats
     * 0 as "none", and loads anything else into ES and reads through
     * it (observed at the fault: ES loaded with 1, #GP on the null
     * descriptor). With no PSP built, +0x2c held whatever was in that
     * memory: 0 for WOWEXEC (whose launcher then read
     * lstrlen(0000:0000) and took a reflected #GP inside krnl386) and
     * 1 for SYSEDIT, which gets past the "none" check and loads the
     * null descriptor. Same field, same cause, two different symptoms.
     *
     * [CAUTION]: DX IS A SELECTOR HERE, NOT A PARAGRAPH. The V86 arm in
     * dos_int21.c writes to `(DX & 0xFFFF) << 4`, which is right
     * there and meaningless here -- it would build the PSP a
     * megabyte away from where krnl386 is about to read it.
     *
     * [CAUTION]: AND +0x2c MUST BE A SELECTOR TOO. DosPspBuild stores the
     * environment as a PARAGRAPH, which is correct for a DOS program
     * and wrong for this one: the guest loads that word straight
     * into ES. So the copied PSP gets a descriptor over
     * the same environment instead -- the same treatment AH=34h and
     * AH=52h already get, for the same reason.
     */
    if (ah == DOS_FN_CREATE_CHILD_PSP || ah == DOS_FN_CREATE_PSP)
    {
        WORD  dxSelector = (WORD)VDM_REG16(tib, VTIB_EDX);
        DWORD dxLinear = DpmiSelectorBase(dxSelector);
        const volatile BYTE *source =
            (const volatile BYTE *)(ULONG_PTR)((DWORD)DOS_PSP_SEG << PARAGRAPH_SHIFT);
        WORD environmentSelector = DpmiSegmentToDescriptor((WORD)DOS_ENV_SEG);
        cursor = LogPut(cursor, "INT21h AH=0x"); cursor = LogHex(cursor, ah);
        cursor = LogPut(cursor, " (PM) create PSP at sel 0x"); cursor = LogHex(cursor, dxSelector);
        if (!dxLinear || !HostWritable((VOID *)(ULONG_PTR)dxLinear, DOS_PSP_SIZE))
        {
            cursor = LogPut(cursor, " -- NO/UNWRITABLE BASE, refusing");
            VDM_REG(tib, VTIB_EFLAGS) |= EFLAGS_CF_U;          /* CF = failure */
        }
        else
        {
            volatile BYTE *destination = (volatile BYTE *)(ULONG_PTR)dxLinear;
            INT item;
            for (item = 0; item < DOS_PSP_SIZE; ++item)
                destination[item] = source[item];
            destination[DOS_PSP_PARENT] = (BYTE)(DOS_PSP_SEG & BYTE_MASK);   /* parent PSP */
            destination[DOS_PSP_PARENT + 1] = (BYTE)(DOS_PSP_SEG >> BYTE_SHIFT);
            if (ah == DOS_FN_CREATE_CHILD_PSP)                            /* memory top */
            {
                WORD si = (WORD)VDM_REG16(tib, VTIB_ESI);
                destination[DOS_PSP_MEMORY_TOP] = (BYTE)(si & BYTE_MASK);
                destination[DOS_PSP_MEMORY_TOP + 1] = (BYTE)(si >> BYTE_SHIFT);
            }
            /* What was there BEFORE we wrote -- if krnl386 had
             * already filled the field, overwriting it would be
             * the defect rather than the fix.
             */
            cursor = LogPut(cursor, " (+0x2c was 0x");
            cursor = LogHex(cursor, (DWORD)(destination[DOS_PSP_ENVIRONMENT] | (destination[DOS_PSP_ENVIRONMENT + 1] << BYTE_SHIFT)));
            cursor = LogPut(cursor, ")");
            destination[DOS_PSP_ENVIRONMENT] = (BYTE)(environmentSelector & BYTE_MASK);           /* env SELECTOR */
            destination[DOS_PSP_ENVIRONMENT + 1] = (BYTE)(environmentSelector >> BYTE_SHIFT);
            if (g_WowPspCount < WOW_PSP_TRACK)
            {
                g_WowPspSelector[g_WowPspCount] = dxSelector;
                g_WowPspLinear[g_WowPspCount] = dxLinear;
                g_WowPspEnvironment[g_WowPspCount] = environmentSelector;
                ++g_WowPspCount;
            }
            VDM_REG(tib, VTIB_EFLAGS) &= ~EFLAGS_CF_U;          /* CF = ok */
            cursor = LogPut(cursor, " lin=0x"); cursor = LogHex(cursor, dxLinear);
            cursor = LogPut(cursor, " env sel=0x"); cursor = LogHex(cursor, environmentSelector);
            if (!environmentSelector)
                cursor = LogPut(cursor, " (NO DESCRIPTOR -- env unusable)");
        }
        cursor = LogPut(cursor, "\r\n");
        LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
        VDM_REG(tib, VTIB_EIP) += DPMI_PM_BOP_LENGTH;
        {
            *cursorIo = cursor;
            *exitCodeOut = 1;
            return HOST_FLOW_RETURN;
        }
    }
    *cursorIo = cursor;
    return HOST_FLOW_NEXT;
}

/* PM INT 21h AH=4Ah: resize a memory block for a PM client. */
static INT DpmiInt21Resize(
    PSTR *cursorIo,
    PSTR const base,
    const DWORD ah,
    volatile BYTE * const tib,
    INT *exitCodeOut)
{
    PSTR cursor = *cursorIo;

    if (ah == DOS_FN_RESIZE)
    {
        /* DOS RESIZE, FROM PROTECTED MODE:
         * ES is a selector, exactly as for 49h, and the evidence arrived
         * the same way: session 16 left 4Ah loud pending a client that
         * actually calls it, and DOS/4GW does -- immediately after the
         * 48h whose block it is shrinking (ES=0xcf, BX=0x40 paras).
         * Resolve ES through the LDT, resize the real block, and then
         * UPDATE THE DESCRIPTOR: the client goes on using the selector it
         * already holds, so a limit left describing the old size is either
         * a spurious #GP (grown block) or a licence to run off the end of
         * the heap (shrunk one).
         * Register footprint is DOS's: nothing on success; AX = error and
         * BX = largest available on failure. See the AH=48h note above for
         * what happens when we improvise extra return values.
         */
        WORD selector = (WORD)VDM_REG16(tib, VTIB_ES);
        INT ldtIndex = DPMI_SELECTOR_INDEX(selector);
        DWORD segmentBase = DpmiSelectorBase(selector);
        DWORD want = VDM_REG16(tib, VTIB_EBX);
        WORD maximum = 0;
        cursor = LogPut(cursor, "INT21h AH=4A (PM) resize sel 0x"); cursor = LogHex(cursor, selector);
        cursor = LogPut(cursor, " base 0x"); cursor = LogHex(cursor, segmentBase);
        cursor = LogPut(cursor, " to 0x"); cursor = LogHex(cursor, want); cursor = LogPut(cursor, " paras");
        if ((segmentBase & PARAGRAPH_LAST_BYTE) || segmentBase > X86_REAL_MODE_LAST_U)
        {
            VDM_REG(tib, VTIB_EFLAGS) |= EFLAGS_CF_U;
            VDM_SET16(tib, VTIB_EAX, DOS_ERR_INVALID_BLOCK);   /* invalid memory block address */
            cursor = LogPut(cursor, " -> REFUSED (not a DOS paragraph)");
        }
        else
        {
            INT error = DosMcbResize(NULL, (WORD)(segmentBase >> PARAGRAPH_SHIFT),
                                 (WORD)want, &maximum);
            if (error)
            {
                VDM_REG(tib, VTIB_EFLAGS) |= EFLAGS_CF_U;
                VDM_SET16(tib, VTIB_EAX, error);
                if (error == DOS_ERR_INSUFFICIENT_MEMORY)
                    VDM_SET16(tib, VTIB_EBX, maximum);
                cursor = LogPut(cursor, " -> err 0x"); cursor = LogHex(cursor, error);
                cursor = LogPut(cursor, " max 0x"); cursor = LogHex(cursor, maximum);
            }
            else
            {
                VDM_REG(tib, VTIB_EFLAGS) &= ~EFLAGS_CF_U;
                if (ldtIndex >= 1 && ldtIndex < DPMI_LDT_MAX)
                {
                    g_Ldt[ldtIndex].Limit = want ? (want * PARAGRAPH_SIZE_U - 1u) : 0;
                    DpmiInstall(ldtIndex);
                    cursor = LogPut(cursor, " -> ok, sel limit now 0x");
                    cursor = LogHex(cursor, g_Ldt[ldtIndex].Limit);
                }
                else
                    cursor = LogPut(cursor, " -> ok (no descriptor to update)");
            }
        }
        cursor = LogPut(cursor, "\r\n");
        LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
        VDM_REG(tib, VTIB_EIP) += DPMI_PM_BOP_LENGTH;
        {
            *cursorIo = cursor;
            *exitCodeOut = 1;
            return HOST_FLOW_RETURN;
        }
    }
    *cursorIo = cursor;
    return HOST_FLOW_NEXT;
}

/* PM INT 21h AH=25h/35h: set or get an interrupt vector for a PM client. */
static INT DpmiInt21SetGetVector(
    PSTR *cursorIo,
    PSTR const base,
    const DWORD ah,
    const DWORD ax,
    volatile BYTE * const tib,
    INT *exitCodeOut)
{
    PSTR cursor = *cursorIo;

    if (ah == DOS_FN_SET_VECTOR || ah == DOS_FN_GET_VECTOR)
    {
        /* SET/GET INTERRUPT VECTOR FROM PROTECTED MODE:
         * These operate on the PROTECTED-MODE vector, i.e. they are
         * INT 31h 0205/0204 wearing a DOS hat, and must never reach
         * DosInt21() -- which writes DS:DX straight into the real-mode
         * IVT at linear (AL*4). In PM that would store a SELECTOR where
         * a segment belongs, in the first kilobyte of guest memory.
         *
         * - THE SPEC IS SILENT HERE. DPMI 0.9 defines INT 31h 0200-0206
         *   for vectors and says only that "DPMI defines a specific
         *   subset of DOS and BIOS calls that can be made by protected
         *   mode DOS programs" -- it does not say which side 25h/35h act
         *   on. Checked, not remembered.
         * - WHAT SETTLES IT IS THE CLIENT, as usual. DOS/4GW does
         *     mov ax,0x3500 / int 21h        ; save the old vector
         *     mov ax,0x2500 / mov dx,0x2cf3 / int 21h
         *   with DS = 0x9F -- its own CODE SELECTOR -- and 0x2cf3 is a
         *   handler inside that selector. A selector:offset pair cannot
         *   be installed in the real-mode IVT, and a chain built from a
         *   35h that read the real vector and a 25h that wrote the PM one
         *   would be incoherent. So both act on the PM table.
         * - OUTSTANDING VERIFICATION: confirm against stock ntvdm's own
         *   DPMI host with a text-mode probe (`stock <target>`). Until
         *   then this is forced-by-the-data, not oracle-confirmed.
         */
        DWORD al = ax & BYTE_MASK;
        if (ah == DOS_FN_SET_VECTOR)
        {
            WORD handlerSelector = (WORD)VDM_REG16(tib, VTIB_DS);
            g_PmInt[al].Selector = handlerSelector;
            g_PmInt[al].Offset = DpmiSelectorIs32(handlerSelector) ? VDM_REG(tib, VTIB_EDX)
                                                   : VDM_REG16(tib, VTIB_EDX);
            g_PmInt[al].Client = 1;
            cursor = LogPut(cursor, "INT21h AH=25 (PM) set PM vector 0x"); cursor = LogHex(cursor, al);
            cursor = LogPut(cursor, " = 0x"); cursor = LogHex(cursor, g_PmInt[al].Selector);
            cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, g_PmInt[al].Offset);
        }
        else
        {
            VDM_SET16(tib, VTIB_ES, g_PmInt[al].Selector);
            if (DpmiSelectorIs32(g_PmInt[al].Selector))
                VDM_REG(tib, VTIB_EBX) = g_PmInt[al].Offset;
            else
                VDM_SET16(tib, VTIB_EBX, g_PmInt[al].Offset & WORD_MASK);
            cursor = LogPut(cursor, "INT21h AH=35 (PM) get PM vector 0x"); cursor = LogHex(cursor, al);
            cursor = LogPut(cursor, " -> 0x"); cursor = LogHex(cursor, g_PmInt[al].Selector);
            cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, g_PmInt[al].Offset);
        }
        VDM_REG(tib, VTIB_EFLAGS) &= ~EFLAGS_CF_U;
        cursor = LogPut(cursor, "\r\n");
        LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
        VDM_REG(tib, VTIB_EIP) += DPMI_PM_BOP_LENGTH;
        {
            *cursorIo = cursor;
            *exitCodeOut = 1;
            return HOST_FLOW_RETURN;
        }
    }
    *cursorIo = cursor;
    return HOST_FLOW_NEXT;
}

/* PM INT 21h AH=49h: free a memory block for a PM client. */
static INT DpmiInt21Free(
    PSTR *cursorIo,
    PSTR const base,
    const DWORD ah,
    volatile BYTE * const tib,
    INT *exitCodeOut)
{
    PSTR cursor = *cursorIo;

    if (ah == DOS_FN_FREE)
    {
        /* DOS FREE, FROM PROTECTED MODE: ES IS A SELECTOR:
         * The mirror image of 48h above, and the convention is forced by
         * it rather than chosen: 48h handed the client a SELECTOR in AX,
         * the client did `mov es,ax`, and the only thing it can pass back
         * to 49h is that selector. So resolve ES through the LDT and free
         * the paragraph its base names. Treating ES as a raw segment here
         * would free whatever MCB happens to live at the selector VALUE --
         * a silent heap corruption, which is precisely the hazard the
         * whitelist comment below exists to prevent.
         * The LDT slot is zeroed rather than reused: g_LdtNext is a bump
         * allocator, so a freed slot is left reclaimable (same treatment
         * as INT 31h 0101) instead of pretending to a free list we do not
         * have.
         */
        WORD selector = (WORD)VDM_REG16(tib, VTIB_ES);
        INT ldtIndex = DPMI_SELECTOR_INDEX(selector);
        DWORD segmentBase = DpmiSelectorBase(selector);
        INT error;
        cursor = LogPut(cursor, "INT21h AH=49 (PM) free sel 0x"); cursor = LogHex(cursor, selector);
        cursor = LogPut(cursor, " base 0x"); cursor = LogHex(cursor, segmentBase);
        if ((segmentBase & PARAGRAPH_LAST_BYTE) || segmentBase > X86_REAL_MODE_LAST_U)
        {
            /* Not a paragraph-aligned conventional-memory base: this is not
             * a block DOS ever handed out, so refuse LOUDLY rather than
             * corrupt the MCB chain guessing.
             */
            VDM_REG(tib, VTIB_EFLAGS) |= EFLAGS_CF_U;
            VDM_SET16(tib, VTIB_EAX, DOS_ERR_INVALID_BLOCK);       /* invalid memory block address */
            cursor = LogPut(cursor, " -> REFUSED (not a DOS paragraph)");
        }
        else
        {
            error = DosMcbFree(NULL, (WORD)(segmentBase >> PARAGRAPH_SHIFT));
            if (error) { VDM_REG(tib, VTIB_EFLAGS) |= EFLAGS_CF_U;
            VDM_SET16(tib, VTIB_EAX, error);
                       cursor = LogPut(cursor, " -> err 0x");
                       cursor = LogHex(cursor, error); }
            else { VDM_REG(tib, VTIB_EFLAGS) &= ~EFLAGS_CF_U;
                   if (ldtIndex >= 3 && ldtIndex < DPMI_LDT_MAX)
                   {
                       g_Ldt[ldtIndex].Base = g_Ldt[ldtIndex].Limit = 0;
                   }
                   cursor = LogPut(cursor, " -> freed seg 0x");
                   cursor = LogHex(cursor, segmentBase >> PARAGRAPH_SHIFT); }
        }
        cursor = LogPut(cursor, "\r\n");
        LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
        VDM_REG(tib, VTIB_EIP) += DPMI_PM_BOP_LENGTH;
        {
            *cursorIo = cursor;
            *exitCodeOut = 1;
            return HOST_FLOW_RETURN;
        }
    }
    *cursorIo = cursor;
    return HOST_FLOW_NEXT;
}

/* PM INT 21h AH=48h: allocate a memory block for a PM client. */
static INT DpmiInt21Allocate(
    PSTR *cursorIo,
    PSTR const base,
    const DWORD ah,
    volatile BYTE * const tib,
    DOS_MACHINE * const machine,
    INT *exitCodeOut)
{
    PSTR cursor = *cursorIo;

    /* REGISTER-ONLY INT 21h: DELEGATE TO THE V86 DOS IMPLEMENTATION:
     * DosInt21() reads and writes the same VDM_TIB register fields the PM
     * client left behind, so a service that takes NO pointer argument needs
     * no thunking at all -- there is nothing to translate.
     * - THE WHITELIST IS DELIBERATE, NOT LAZINESS. A service that takes a
     *   DS:DX / ES:BX pointer must NOT come through here: in protected mode
     *   those registers hold SELECTORS, and handing a selector to code that
     *   treats it as a real-mode segment reads or writes whatever happens to
     *   live at selector<<4. That is a silent wrong-memory bug of exactly the
     *   kind that cost this session a day (see the D/B fix). Pointer-taking
     *   services are hand-rolled above with DpmiSelectorBase(), or stay loud.
     * - ES-as-SEGMENT services (49h free, 4Ah resize) are ALSO excluded from
     *   the whitelist: the block address they want is a real-mode paragraph,
     *   but in PM ES holds a selector. Session 16 said "if Doom calls 49h the
     *   log will say so and we can settle the convention on evidence" -- IT
     *   DID (twice), so 49h is hand-rolled below, resolving ES through the
     *   LDT. 4Ah is still unevidenced and still stays loud.
     * - 33h (Ctrl-Break / true version) joined the whitelist on the same
     *   evidence: Doom calls it twice, and every subfunction DosInt21
     *   implements (AL=00/01/05/06) reads and writes GPRs only. Leaving it
     *   unhandled was NOT neutral -- the TODO arm returned with AX still
     *   0x33xx and CF untouched, and the caller's very next instructions are
     *   `xchg ax,cx / cbw / retn`, i.e. it propagates whatever we left.
     * Doom (DOS/4GW) needs 48h to allocate the memory it loads its LE image
     * into -- it is the first DOS call it makes from protected mode.
     */
    if (ah == DOS_FN_ALLOCATE)
    {
        /* DOS ALLOCATE, FROM PROTECTED MODE, RETURNS A SELECTOR:
         * A raw real-mode segment is useless to a PM client, and Doom
         * proves the convention by what it does: on success it loads the
         * AX it got back straight into ES (observed at the fault). That
         * only makes sense if AX is a SELECTOR -- with the raw segment
         * 0x151c it is GDT index 0x2A3, which #GPs and silently kills the
         * VDM. That segment load IS the specification here, the same way
         * DOS/4GW clearing D/B itself settled the
         * initial-selector width.
         * So: do the real DOS allocation (DosInt21 owns the MCB chain),
         * then hand back a descriptor covering it, IN AX ONLY.
         *
         * - DO NOT ALSO PUT IT IN DX. Session 16 did, reasoning that it
         *   "matches INT 31h 0100's shape, which costs nothing and is
         *   what a client written against 0100 would expect". It cost
         *   Doom. Real DOS's AH=48h returns AX (and BX on failure) and
         *   PRESERVES EVERYTHING ELSE, so callers keep live values in
         *   the other registers across it -- and DOS/4GW keeps the
         *   request's BYTE SIZE in DX:
         *     mov dx,cx / add dx,0x27 / and dl,0xf0   ; DX = bytes
         *     mov bx,dx / ...shift...                 ; BX = paragraphs
         *     mov ah,48h / int 21h
         *     ...
         *     mov ax,dx                               ; DX still = bytes
         *     mov di,ax / add di,bx / dec di / dec di
         *     movw [di],0xfffe                        ; last word of block
         *   With DX clobbered to the selector (0xcf) instead of the size
         *   (0x40), DI became 0xcd against a 0x4f limit: a write past the
         *   segment end, #GP, and XP terminated the VDM with nothing in
         *   the log. Bisected to the instruction with the pmbp.txt
         *   breakpoints.
         * - THE GENERAL RULE THIS EARNS: a service's register footprint
         *   is part of its contract. Writing a register the real service
         *   leaves alone is not a harmless bonus, it is a silent
         *   corruption of the caller's state. Return what DOS returns.
         */
        DWORD want = VDM_REG16(tib, VTIB_EBX);
        machine->TraceCursor = cursor;
        DosInt21SetProtectedMode(TRUE);
        DosInt21(machine);
        DosInt21SetProtectedMode(FALSE);
        cursor = machine->TraceCursor;
        cursor = LogPut(cursor, "INT21h AH=48 (PM) alloc 0x"); cursor = LogHex(cursor, want);
        if (VDM_REG(tib, VTIB_EFLAGS) & EFLAGS_CF_U)
        {
            cursor = LogPut(cursor, " -> FAILED, largest 0x");
            cursor = LogHex(cursor, VDM_REG16(tib, VTIB_EBX));
        }
        else if (g_LdtNext >= DPMI_LDT_MAX)
        {
            VDM_REG(tib, VTIB_EFLAGS) |= EFLAGS_CF_U;
            VDM_SET16(tib, VTIB_EAX, DOS_ERR_INSUFFICIENT_MEMORY);       /* insufficient memory */
            cursor = LogPut(cursor, " -> no free LDT slot");
        }
        else
        {
            DWORD segment = VDM_REG16(tib, VTIB_EAX);
            INT ldtIndex = g_LdtNext++;
            WORD selector;
            g_Ldt[ldtIndex].Base   = segment << PARAGRAPH_SHIFT;
            g_Ldt[ldtIndex].Limit  = want ? (want * PARAGRAPH_SIZE_U - 1u) : X86_SEGMENT_LIMIT_64K;
            g_Ldt[ldtIndex].Access = DPMI_ACCESS_DATA;          /* present, DPL3, data R/W */
            g_Ldt[ldtIndex].Flags  = 0;             /* 16-bit, byte granular */
            DpmiInstall(ldtIndex);
            selector = (WORD)DPMI_LDT_SELECTOR(ldtIndex);
            VDM_SET16(tib, VTIB_EAX, selector);   /* AX only -- see above */
            cursor = LogPut(cursor, " -> seg 0x"); cursor = LogHex(cursor, segment);
            cursor = LogPut(cursor, " as sel 0x"); cursor = LogHex(cursor, selector);
            cursor = LogPut(cursor, " limit 0x"); cursor = LogHex(cursor, g_Ldt[ldtIndex].Limit);
        }
        cursor = LogPut(cursor, "\r\n");
        LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
        VDM_REG(tib, VTIB_EIP) += DPMI_PM_BOP_LENGTH;
        {
            *cursorIo = cursor;
            *exitCodeOut = 1;
            return HOST_FLOW_RETURN;
        }
    }
    *cursorIo = cursor;
    return HOST_FLOW_NEXT;
}

/* PM INT 21h AH=3Fh: read from a handle into a PM client buffer (DS:DX). */
static INT DpmiInt21Read(
    PSTR *cursorIo,
    PSTR const base,
    const DWORD ah,
    volatile BYTE * const tib,
    DOS_MACHINE * const machine,
    INT *exitCodeOut)
{
    PSTR cursor = *cursorIo;

    if (ah == DOS_FN_READ)                            /* read: BX=handle CX=cnt -> DS:DX */
    {
        DWORD handle = VDM_REG16(tib, VTIB_EBX);
        DWORD count = VDM_REG16(tib, VTIB_ECX);
        DWORD bytesRead = 0;
        DWORD dsBase = DpmiSelectorBase((WORD)VDM_REG(tib, VTIB_DS));
        PVOID buffer = (VOID *)(ULONG_PTR)(dsBase + VDM_REG16(tib, VTIB_EDX));
        VDM_REG(tib, VTIB_EFLAGS) &= ~EFLAGS_CF_U;
        if (handle < DOS_MAX_FILES && machine->FileHandles[handle])
        {
            DWORD writeError = 0;
            if (!ReadFile(machine->FileHandles[handle], buffer, count, &bytesRead, NULL))
                writeError = GetLastError();
            if (!PmRwHardwareFail(machine, tib, DOS_FN_READ, writeError, &cursor))   /* #275 */
                VDM_SET16(tib, VTIB_EAX, bytesRead);
        }
        else
        {
            VDM_REG(tib, VTIB_EFLAGS) |= EFLAGS_CF_U;
            VDM_SET16(tib, VTIB_EAX, DOS_ERR_INVALID_HANDLE);
        }
        /* WHERE IT LANDED, NOT JUST HOW MUCH:
         * "read 0x40b" cannot distinguish a read that filled the
         * buffer the guest meant from one that filled a different
         * one, and session 34 needed exactly that distinction: the
         * selector krnl386 later parsed as an NE header held
         * ne_modtab = 0x38a instead of 0x78, and the only way to
         * tell a mis-delivered read from a mis-parsed one is to
         * print the destination and the bytes.
         */
        /* [CAUTION]: AND WHAT THE GUEST WILL SEE. This printed the byte
         * count and not the ANSWER -- half an instrument, by
         * this project's own rule -- and it cost a reading:
         * SYSEDIT reports "Cannot read this file" about a
         * 0-byte file, which the stock-ntvdm oracle shows it
         * does NOT do, and the whole question is what `_lread`
         * came back with. `rd` and `AX` are the same number
         * today and printing only one of them is a claim that
         * they always will be.
         */
        cursor = LogPut(cursor, "INT21h AH=3F read "); cursor = LogHex(cursor, bytesRead);
        cursor = LogPut(cursor, "b of "); cursor = LogHex(cursor, count);
        cursor = LogPut(cursor, " -> AX=0x");
        cursor = LogHex(cursor, VDM_REG16(tib, VTIB_EAX));
        cursor = LogPut(cursor, " CF="); cursor = LogHex(cursor, VDM_REG(tib, VTIB_EFLAGS) & EFLAGS_CF_U);
        cursor = LogPut(cursor, " h="); cursor = LogHex(cursor, handle);
        cursor = LogPut(cursor, " -> 0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_DS));
        cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_EDX));
        cursor = LogPut(cursor, " lin=0x"); cursor = LogHex(cursor, (DWORD)(ULONG_PTR)buffer);
        cursor = LogPut(cursor, " pos=0x");
        cursor = LogHex(cursor, (handle < DOS_MAX_FILES && machine->FileHandles[handle])
                    ? SetFilePointer(machine->FileHandles[handle], 0, NULL, FILE_CURRENT) : 0);
        cursor = LogPut(cursor, " first=");
        if (bytesRead && HostReadable(buffer, 8))
            cursor = LogDump(cursor, buffer, 8);
        else
            cursor = LogPut(cursor, "-");
        cursor = LogPut(cursor, "\r\n");
        LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
        VDM_REG(tib, VTIB_EIP) += DPMI_PM_BOP_LENGTH;
        {
            *cursorIo = cursor;
            *exitCodeOut = 1;
            return HOST_FLOW_RETURN;
        }
    }
    *cursorIo = cursor;
    return HOST_FLOW_NEXT;
}

/* PM INT 21h AH=3Ch/3Dh/5Bh: create or open a file named by an ASCIIZ path at DS:DX. */
static INT DpmiInt21CreateOpen(
    PSTR *cursorIo,
    PSTR const base,
    const DWORD ah,
    volatile BYTE * const tib,
    const DWORD ax,
    DOS_MACHINE * const machine,
    INT *exitCodeOut)
{
    PSTR cursor = *cursorIo;

    /* -- AND 5Bh, "CREATE NEW", WHICH IS HOW A GUEST MAKES A
     * TEMPORARY FILE. (session 56) -----------------------------
     * It is 3Ch with one difference -- it FAILS if the name
     * already exists -- and that difference is the whole point:
     * the caller invents a name, tries to create it, and retries
     * with another on error 50h. Landing in "PM thunk TODO"
     * therefore does not look like an unimplemented call, it
     * looks like a full disk.
     *
     * [INFO]: MEASURED. CARDFILE calls AH=2Ch for the time, builds a name
     * from it, calls 5Bh, and puts up
     *     "Cannot create temporary file. Delete one or more files
     *      to increase available disk space, and then try again."
     * on a 243 GB volume -- which is the recorded WRITE defect
     * too, and the reason both were filed under "runs but lies"
     * rather than as a missing DOS call.
     */
    if (ah == DOS_FN_CREATE || ah == DOS_FN_OPEN || ah == DOS_FN_CREATE_NEW)    /* create / open: DS:DX = ASCIIZ name */
    {
        DWORD dsBase = DpmiSelectorBase((WORD)VDM_REG(tib, VTIB_DS));
        PCSTR fileName = (PCSTR)(ULONG_PTR)(dsBase + VDM_REG16(tib, VTIB_EDX));
        /* AL IS A BIT FIELD, NOT A NUMBER. (#128, session 37) (Importance = 2):
         * DOS open mode: bits 0-2 = access (0 read, 1 write, 2 r/w),
         * bits 4-6 = the SHARE.EXE sharing mode, bit 7 = no-inherit.
         * This compared the WHOLE byte against 0 and 1, so krnl386's
         * `al = 0x80` -- read access with the inheritance bit, the
         * most ordinary open there is -- fell through to "anything
         * else" and asked Windows for GENERIC_READ | GENERIC_WRITE.
         * That is what cost GDI.EXE: USER.EXE imports GDI, so the
         * file was ALREADY open (handle 7, never closed) when the
         * boot list loaded it by name, and a second open asking for
         * WRITE against a FILE_SHARE_READ handle is
         * ERROR_SHARING_VIOLATION. It came back as DOS error 2, which
         * krnl386 read as a short header read and reported as 0x0B
         * ERROR_BAD_FORMAT -- so the module it could not open looked
         * like a module it had rejected. Access now comes from AL & 7.
         *
         * [CAUTION]: AND WE DO NOT EMULATE SHARE.EXE. Bare DOS enforces no
         * locking at all, so imposing FILE_SHARE_READ invented a
         * restriction the guest's DOS does not have. Share everything
         * and let the guest be as reckless as DOS lets it be.
         */
        DWORD mode = ax & DOS_INT21_OPEN_ACCESS_MASK;
        DWORD desiredAccess = (mode == DOS_INT21_OPEN_WRITE) ? GENERIC_WRITE
                  : (mode == DOS_INT21_OPEN_READ_WRITE) ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_READ;
        DWORD shareMode = FILE_SHARE_READ | FILE_SHARE_WRITE;
        HANDLE file = (ah == DOS_FN_CREATE || ah == DOS_FN_CREATE_NEW)
            ? CreateFileA(fileName, GENERIC_READ | GENERIC_WRITE, shareMode, NULL,
                          (ah == DOS_FN_CREATE_NEW) ? CREATE_NEW : CREATE_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL, NULL)
            : CreateFileA(fileName, desiredAccess, shareMode, NULL, OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL, NULL);
        DWORD lastError = (file == INVALID_HANDLE_VALUE) ? GetLastError() : 0;
        VDM_REG(tib, VTIB_EFLAGS) &= ~EFLAGS_CF_U;
        /* Report the failure the guest actually suffered. Answering 2
         * "file not found" to an access or sharing failure is the
         * "runs but lies" class: the caller retries a path that is
         * right and concludes the file is missing.
         */
        if (file == INVALID_HANDLE_VALUE) { VDM_REG(tib, VTIB_EFLAGS) |= EFLAGS_CF_U;
               VDM_SET16(tib, VTIB_EAX,
                   (lastError == ERROR_PATH_NOT_FOUND)     ? DOS_ERR_PATH_NOT_FOUND :
                   (lastError == ERROR_ACCESS_DENIED)      ? DOS_ERR_ACCESS_DENIED :
                   (lastError == ERROR_SHARING_VIOLATION)  ? DOS_ERR_SHARING_VIOLATION :
                   /* 50h is the ANSWER 5Bh exists to give: the name
                    * is taken, try another. Mapping it to 2 would
                    * tell the caller its own file is missing.
                    */
                   (lastError == ERROR_FILE_EXISTS ||
                    lastError == ERROR_ALREADY_EXISTS)     ? DOS_ERR_FILE_EXISTS :
                   (lastError == ERROR_TOO_MANY_OPEN_FILES)? DOS_ERR_TOO_MANY_OPEN_FILES : DOS_ERR_FILE_NOT_FOUND); }
        else { INT slot;
        for (slot = DOS_STD_HANDLES; slot < DOS_MAX_FILES && machine->FileHandles[slot]; ++slot)
        {
        }
               if (slot < PM_INT21_HANDLE_LIMIT) { machine->FileHandles[slot] = file;
               VDM_SET16(tib, VTIB_EAX, slot);
                                if (ah == DOS_FN_CREATE || ah == DOS_FN_CREATE_NEW)
                                    DosStampVdmNow(file); /* #263 */ }
               else
               {
                   CloseHandle(file);
                   VDM_REG(tib, VTIB_EFLAGS) |= EFLAGS_CF_U;
                   VDM_SET16(tib, VTIB_EAX, DOS_ERR_TOO_MANY_OPEN_FILES);
               }
               }
        /* [CAUTION]: "-> AX=0x2" MEANT TWO OPPOSITE THINGS. (session 37) (Importance = 1):
         * This printed AX and nothing else, so a failed open reading
         * "AX=0x0002" (error 2, file not found) was indistinguishable
         * from a successful open that returned handle 2 -- and the
         * whole GDI.EXE wall was read the wrong way round off this one
         * line. Print the REQUEST (AL carries the access mode AND the
         * DOS sharing mode), the VERDICT (CF), and, when it failed,
         * the Win32 error, which is the only thing that says WHY.
         */
        cursor = LogPut(cursor, "INT21h AH="); cursor = LogHex(cursor, ah); cursor = LogPut(cursor, " open \"");
        cursor = LogPut(cursor, fileName); cursor = LogPut(cursor, "\" al=0x"); cursor = LogHexByte(cursor, (BYTE)(ax & BYTE_MASK));
        cursor = LogPut(cursor, " -> AX=0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_EAX));
        cursor = LogPut(cursor, " CF="); cursor = LogHex(cursor, VDM_REG(tib, VTIB_EFLAGS) & EFLAGS_CF_U);
        if (lastError)
        {
            cursor = LogPut(cursor, " FAILED gle=");
            cursor = LogHex(cursor, lastError);
        }
        cursor = LogPut(cursor, "\r\n"); LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
        VDM_REG(tib, VTIB_EIP) += DPMI_PM_BOP_LENGTH;
        {
            *cursorIo = cursor;
            *exitCodeOut = 1;
            return HOST_FLOW_RETURN;
        }
    }
    *cursorIo = cursor;
    return HOST_FLOW_NEXT;
}

/* PM INT 21h AH=40h: write BX=handle CX=count from a PM client buffer (DS:DX). */
static INT DpmiInt21Write(
    PSTR *cursorIo,
    PSTR const base,
    const DWORD ah,
    volatile BYTE * const tib,
    DOS_MACHINE * const machine,
    INT *exitCodeOut)
{
    PSTR cursor = *cursorIo;

    if (ah == DOS_FN_WRITE)                            /* write BX=handle CX=cnt DS:DX=buf */
    {
        DWORD bh = VDM_REG16(tib, VTIB_EBX);
        DWORD count = VDM_REG16(tib, VTIB_ECX);
        DWORD dsBase = DpmiSelectorBase((WORD)VDM_REG(tib, VTIB_DS));
        const volatile BYTE *bytes = (const volatile BYTE *)(ULONG_PTR)
            (dsBase + VDM_REG16(tib, VTIB_EDX));
        /* #256: A BOUND HANDLE IS A FILE, WHATEVER ITS NUMBER:
         * This tested `bh == 1 || bh == 2` FIRST, so a DPMI program run
         * as `prog > out.txt` printed to the screen and left out.txt
         * empty -- the defect #133 fixed for real mode (see the V86
         * AH=40h). The same two rules as there: a bound slot is a file;
         * an unbound open device slot is the console, except 3/4,
         * which are AUX and PRN (#251).
         */
        INT bound = (bh < DOS_MAX_FILES && machine->FileHandles[bh] != 0);
        INT device   = (!bound && bh < BITS_PER_DWORD && ((machine->StdOpen >> bh) & 1u));
        VDM_REG(tib, VTIB_EFLAGS) &= ~EFLAGS_CF_U;
        if (device && (bh == DOS_HANDLE_AUX || bh == DOS_HANDLE_PRN))       /* AUX / PRN */
        {
            DWORD item;
            for (item = 0; item < count; ++item)
            {
                if (bh == DOS_HANDLE_PRN)
                    (VOID)DosPrnOut(NULL, bytes[item]);
                else
                    DosAuxOut(NULL, bytes[item]);
            }
            VDM_SET16(tib, VTIB_EAX, count);
        }
        else if (device)                        /* the console */
        {
            CHAR outputLine[300];
            PSTR outputCursor = outputLine;
            DWORD item;
            outputCursor = LogPut(outputCursor, "INT21h AH=40 write: \"");
            for (item = 0; item < count && item < 250; ++item)
            {
                if (bytes[item] >= ASCII_SPACE && outputCursor < outputLine + 270)
                    *outputCursor++ = (CHAR)bytes[item];
                if (machine->ConsoleOut)
                    machine->ConsoleOut(machine->ConsoleOutContext, bytes[item]);
                if (machine->OutputLength < machine->OutputCapacity - 1)
                    machine->Output[machine->OutputLength++] = (CHAR)bytes[item];
                else
                    machine->IsOutputTruncated = 1;
            }
            outputCursor = LogPut(outputCursor, "\"\r\n");
            LogAppend(LOG_PATH, outputLine, outputCursor); SerialOut(outputLine, outputCursor);
            VDM_SET16(tib, VTIB_EAX, count);     /* AX = bytes written */
        }
        else if (bound)                       /* file handle */
        {
            DWORD bytesWritten = 0;
            DWORD writeError = 0;
            if (!WriteFile(machine->FileHandles[bh], (const VOID *)bytes, count, &bytesWritten, NULL))
                writeError = GetLastError();
            if (!PmRwHardwareFail(machine, tib, DOS_FN_WRITE, writeError, &cursor))     /* #275 */
            {
                VDM_SET16(tib, VTIB_EAX, bytesWritten);
                DosStampVdmNow(machine->FileHandles[bh]);   /* #263, as the V86 AH=40h */
            }
            cursor = LogPut(cursor, "INT21h AH=40 file write "); cursor = LogHex(cursor, bytesWritten); cursor = LogPut(cursor, "b\r\n");
            LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
        }
        else
        {
            VDM_REG(tib, VTIB_EFLAGS) |= EFLAGS_CF_U;
            VDM_SET16(tib, VTIB_EAX, DOS_ERR_INVALID_HANDLE);
        }
        VDM_REG(tib, VTIB_EIP) += DPMI_PM_BOP_LENGTH;
        {
            *cursorIo = cursor;
            *exitCodeOut = 1;
            return HOST_FLOW_RETURN;
        }
    }
    *cursorIo = cursor;
    return HOST_FLOW_NEXT;
}

/* A dialog procedure returned inside a modal DialogBox: if it said FALSE to WM_CLOSE, WM_PAINT or
 * WM_ERASEBKGND, apply DefDlgProc's default; then step the modal loop (WowDlgStep) -- DialogBox does
 * not return until the dialog ends.
 */
static PSTR WowCallbackModalPump(
    PSTR cursor,
    PSTR const base,
    const INT callAction,
    volatile BYTE * const tib,
    const DWORD result,
    const WORD actionArgument)
{
    if (callAction == WOWCALL_ACT_MODALPUMP)
    {
        CHAR modalNote[512];
        WORD  modalCallbackSelector = WowCallbackSelector();
        DWORD modalStackBase  = DpmiSelectorBase(
            (WORD)VDM_REG16(tib, VTIB_SS));
        /* s88: the modal loop calls a #32770 dialog's DLGPROC directly, so a
         * FALSE answer to WM_CLOSE got no DefDlgProc default -- Task List's X
         * did nothing. Same default as DefDlgProc's (IDCANCEL posted, which
         * the loop delivers next). WM_CLOSE, and (s92) WM_PAINT / WM_ERASEBKGND:
         * TASKMAN's DLGPROC answers FALSE to WM_PAINT and nothing ever erased
         * the dialog, so the Task List showed the desktop behind it
         * (runs/s92/untitled2.bmp). DefDlgProc's paint is the dialog colour.
         */
        if (g_WowDlgIsDialogCall[g_WowCallDepth] && (WORD)result == 0
            && (g_WowDlgMessage[g_WowCallDepth] == WM_CLOSE
                || g_WowDlgMessage[g_WowCallDepth] == WM_PAINT
                || g_WowDlgMessage[g_WowCallDepth] == WM_ERASEBKGND))
        {
            CHAR defaultName[200];
            INT defaultLength = 0;
            WowUserDlgDefault(WowUserFindWindow(actionArgument), actionArgument,
                                g_WowDlgMessage[g_WowCallDepth],
                                g_WowUserDlgDefaults[g_WowCallDepth].WParam,
                                g_WowUserDlgDefaults[g_WowCallDepth].LParam,
                                defaultName, (INT)sizeof defaultName, &defaultLength);
            defaultName[defaultLength < (INT)sizeof defaultName ? defaultLength : (INT)sizeof defaultName - 1] = 0;
            cursor = LogPut(cursor, " -- DLGPROC said FALSE; DefDlgProc default:");
            cursor = LogPut(cursor, defaultName);
        }
        g_WowDlgIsDialogCall[g_WowCallDepth] = 0;
        modalNote[0] = 0;
        cursor = LogPut(cursor, "\r\n");
        LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
        WowDlgStep(tib, modalStackBase, modalCallbackSelector, &g_Running, modalNote, sizeof modalNote);
        cursor = LogPut(cursor, "WOWDLG: "); cursor = LogPut(cursor, modalNote);
    }
    return cursor;
}

/* The clipboard bridge (#160): each step runs with the parked caller restored, as the EDIT chain does, so a follow-up call re-parks it at the same SS:SP. */
static PSTR WowCallbackClipboard(
    PSTR cursor,
    const INT callAction,
    volatile BYTE * const tib,
    const DWORD result,
    const WORD actionArgument)
{
    /* -- THE CLIPBOARD BRIDGE (#160). See WOWCALL_ACT_CLIP* in wowcall.h.
     * Each step runs with the parked caller restored, exactly like the EDIT
     * chain above, so a follow-up call re-parks it at the same SS:SP.
     */
    if (callAction == WOWCALL_ACT_CLIPLOCK || callAction == WOWCALL_ACT_CLIPFILL
        || callAction == WOWCALL_ACT_CLIPPUT)
    {
        WORD  clipCallbackSelector = WowCallbackSelector();
        DWORD clipStackBase  = DpmiSelectorBase((WORD)VDM_REG16(tib, VTIB_SS));
        WORD  clipDs   = (WORD)VDM_REG16(tib, VTIB_DS);
        WORD  handleArgument  = (callAction == WOWCALL_ACT_CLIPLOCK) ? (WORD)result : actionArgument;
        DWORD farPointerLinear = 0;
        if (callAction != WOWCALL_ACT_CLIPLOCK && (result >> WORD_SHIFT))
            farPointerLinear = DpmiSelectorBase((WORD)(result >> WORD_SHIFT)) + (result & WORD_MASK);
        if (callAction == WOWCALL_ACT_CLIPLOCK)
        {
            cursor = LogPut(cursor, " -- CLIP GlobalAlloc -> 0x"); cursor = LogHex(cursor, handleArgument);
        }
        else if (callAction == WOWCALL_ACT_CLIPFILL)
        {
            /* GlobalAlloc was sized n+1, so the copy is bounded by what was
             * asked for; the NUL goes in with it.
             */
            INT index;
            cursor = LogPut(cursor, " -- CLIP fill 0x"); cursor = LogHex(cursor, (DWORD)g_WowUserClipboardLength);
            cursor = LogPut(cursor, " byte(s) at 0x"); cursor = LogHex(cursor, result);
            if (farPointerLinear && MemoryReadable((ULONG_PTR)farPointerLinear, (DWORD)g_WowUserClipboardLength + 1))
            {
                volatile BYTE *destinationBytes = (volatile BYTE *)(ULONG_PTR)farPointerLinear;
                for (index = 0; index <= g_WowUserClipboardLength; ++index)
                    destinationBytes[index] = (BYTE)g_WowUserClipboard[index];
            }
            else cursor = LogPut(cursor, " -- ★ GlobalLock gave no usable pointer; the"
                               " block stays EMPTY");
        }
        else
        {
            /* The guest's text, to a block the HOST clipboard will own. */
            INT length = 0;
            cursor = LogPut(cursor, " -- CLIP put from 0x"); cursor = LogHex(cursor, result);
            if (farPointerLinear)
            {
                const volatile BYTE *sourceBytes = (const volatile BYTE *)(ULONG_PTR)farPointerLinear;
                while (length < (INT)sizeof g_WowUserClipboard - 1
                       && MemoryReadable((ULONG_PTR)(sourceBytes + length), 1) && sourceBytes[length])
                    {
                        g_WowUserClipboard[length] = (CHAR)sourceBytes[length];
                        ++length;
                    }
            }
            g_WowUserClipboard[length] = 0;
            if (farPointerLinear)
            {
                HGLOBAL clipboardMemory = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)length + 1);
                PSTR clipboardText = clipboardMemory ? (PSTR)GlobalLock(clipboardMemory) : NULL;
                INT index;
                if (clipboardText)
                {
                    for (index = 0; index <= length; ++index)
                        clipboardText[index] = g_WowUserClipboard[index];
                    GlobalUnlock(clipboardMemory);
                }
                if (clipboardText && SetClipboardData(g_WowUserClipboardFormat, clipboardMemory))
                {
                    cursor = LogPut(cursor, " -> 0x"); cursor = LogHex(cursor, (DWORD)length);
                    cursor = LogPut(cursor, " byte(s) on the host's clipboard");
                }
                else
                {
                    if (clipboardMemory)
                        GlobalFree(clipboardMemory);
                    cursor = LogPut(cursor, " -- ★ the host clipboard REFUSED it (not open?)");
                }
            }
            else cursor = LogPut(cursor, " -- ★ GlobalLock gave no usable pointer; nothing"
                               " was copied");
        }
        /* Next link: Alloc -> Lock, and Lock -> Unlock once the bytes moved. */
        if (handleArgument && g_WowUserKernelSegment && clipCallbackSelector && clipStackBase)
        {
            WORD firstArgument = handleArgument;
            DWORD proc = ((DWORD)g_WowUserKernelSegment << WORD_SHIFT)
                       | (callAction == WOWCALL_ACT_CLIPLOCK ? WOWUSER_KRNL_GLOBALLOCK_OFF
                                                      : WOWUSER_KRNL_GLOBALUNLOCK_OFF);
            if (WowCallEnter(tib, clipStackBase, clipCallbackSelector, proc, clipDs, &firstArgument, 1, 0,
                              WOWCALL_RET_KEEP, NULL, 0, 0, NULL, 0, -1, WOWCALL_PROCEDURE_PRESENT))
            {
                if (callAction == WOWCALL_ACT_CLIPLOCK && g_WowCallDepth > 0)
                {
                    g_WowCallFrames[g_WowCallDepth - 1].Action = WOWCALL_ACT_CLIPFILL;
                    g_WowCallFrames[g_WowCallDepth - 1].ActionArgument = handleArgument;
                }
                cursor = LogPut(cursor, callAction == WOWCALL_ACT_CLIPLOCK ? "; GlobalLock in flight"
                                                        : "; GlobalUnlock in flight");
            }
            else
                cursor = LogPut(cursor, "; ★ the next KERNEL call was REFUSED");
        }
    }
    return cursor;
}

/* The save direction, step 3: write the EDIT control's text into the block the guest locked -- the mirror of WOWCALL_ACT_EDITTEXT. */
static PSTR WowCallbackEditFill(
    PSTR cursor,
    const INT callAction,
    const WORD actionArgument,
    const DWORD result,
    volatile BYTE * const tib)
{
    /* -- STEP 3: WRITE THE CONTROL'S TEXT INTO THE GUEST'S BLOCK.
     * This is the exact mirror of ACT_EDITTEXT. There the block was
     * read and handed to the control; here the control is read and the
     * bytes are handed to the block, which is what the application is
     * about to `_lwrite` to its file.
     *
     * [CAUTION]: BOUNDED BY WHAT WAS ALLOCATED. The block was sized from
     * GetWindowTextLength at EM_GETHANDLE time and the text is fetched
     * again here, so a keystroke landing between the two would
     * overflow -- GetWindowTextA is therefore given the SAME bound the
     * allocation used, and the log says if it had to truncate.
     *
     * [CAUTION]: AND UNLOCK. We took the lock, we owe the release.
     */
    if (callAction == WOWCALL_ACT_EDITFILL)
    {
        WOWUSER_WINDOW *editWindow = WowUserFindWindow(actionArgument);
        DWORD offset = result & WORD_MASK;
        DWORD instanceBase = editWindow ? DpmiSelectorBase(editWindow->Instance) : 0;
        cursor = LogPut(cursor, " -- EDIT block at 0x"); cursor = LogHex(cursor, editWindow ? editWindow->Instance : 0);
        cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, offset);
        if (!editWindow || !editWindow->Window32 || !instanceBase || !offset)
        {
            cursor = LogPut(cursor, " -- ★ UNWRITABLE; the control's text is NOT saved");
        }
        else
        {
            static CHAR savedEditText[16384];
            volatile BYTE *destinationBytes = (volatile BYTE *)(ULONG_PTR)(instanceBase + offset);
            INT cap = GetWindowTextLengthA(editWindow->Window32) + 1;
            INT length;
            INT index;
            if (cap > (INT)sizeof savedEditText)
                cap = (INT)sizeof savedEditText;
            length = GetWindowTextA(editWindow->Window32, savedEditText, cap);
            for (index = 0; index <= length; ++index)
                destinationBytes[index] = (BYTE)savedEditText[index];
            cursor = LogPut(cursor, " <- 0x"); cursor = LogHex(cursor, (DWORD)length);
            cursor = LogPut(cursor, " byte(s) from the real control");
            if (length == cap - 1 && cap == (INT)sizeof savedEditText)
                cursor = LogPut(cursor, " (★ TRUNCATED at the host's buffer)");
        }
        if (editWindow && editWindow->Memory16 && g_WowUserKernelSegment)
        {
            WORD  editCallbackSelector = WowCallbackSelector();
            DWORD editStackBase  = DpmiSelectorBase(
                            (WORD)VDM_REG16(tib, VTIB_SS));
            WORD  unlockArgument  = editWindow->Memory16;
            if (editCallbackSelector && editStackBase
                && WowCallEnter(tib, editStackBase, editCallbackSelector,
                                 ((DWORD)g_WowUserKernelSegment << WORD_SHIFT)
                                     | WOWUSER_KRNL_LOCALUNLOCK_OFF,
                                 editWindow->Instance, &unlockArgument, 1, 0,
                                 WOWCALL_RET_KEEP, NULL, editWindow->Window16, 0,
                                 NULL, 0, -1, WOWCALL_PROCEDURE_PRESENT))
                cursor = LogPut(cursor, "; LocalUnlock in flight");
            else
                cursor = LogPut(cursor, "; ★ LocalUnlock REFUSED -- the block stays"
                            " locked");
        }
    }
    return cursor;
}

/* The save direction, step 2: lock the local handle the allocator just answered with, to get a pointer to its block. */
static PSTR WowCallbackEditLock(
    PSTR cursor,
    const INT callAction,
    const WORD actionArgument,
    volatile BYTE * const tib)
{
    /* THE SAVE DIRECTION, STEP 2: LOCK WHAT WE JUST GOT (Importance = 5):
     * The allocator has answered with a local handle (already stored
     * through the sink, so `ew->hmem` is the NEW one even if
     * LocalReAlloc moved the block). Locking it is the only way to get
     * an address to write the text at, and only the guest's KERNEL can
     * do it -- the same call, in the same DGROUP, as the load path.
     */
    if (callAction == WOWCALL_ACT_EDITLOCK)
    {
        WOWUSER_WINDOW *editWindow = WowUserFindWindow(actionArgument);
        cursor = LogPut(cursor, " -- EDIT block 0x"); cursor = LogHex(cursor, editWindow ? editWindow->Memory16 : 0);
        if (!editWindow || !editWindow->Memory16 || !editWindow->Window32 || !g_WowUserKernelSegment)
        {
            cursor = LogPut(cursor, " -- ★ NOT USABLE; the control's text is NOT saved");
        }
        else
        {
            WORD  editCallbackSelector = WowCallbackSelector();
            DWORD editStackBase  = DpmiSelectorBase(
                            (WORD)VDM_REG16(tib, VTIB_SS));
            WORD  lockArgument  = editWindow->Memory16;
            if (editCallbackSelector && editStackBase
                && WowCallEnter(tib, editStackBase, editCallbackSelector,
                                 ((DWORD)g_WowUserKernelSegment << WORD_SHIFT)
                                     | WOWUSER_KRNL_LOCALLOCK_OFF,
                                 editWindow->Instance, &lockArgument, 1, 0,
                                 WOWCALL_RET_KEEP, NULL, editWindow->Window16, 0,
                                 NULL, 0, -1, WOWCALL_PROCEDURE_PRESENT))
            {
                if (g_WowCallDepth > 0)
                {
                    g_WowCallFrames[g_WowCallDepth - 1].Action = WOWCALL_ACT_EDITFILL;
                    g_WowCallFrames[g_WowCallDepth - 1].ActionArgument = editWindow->Window16;
                }
                cursor = LogPut(cursor, "; LocalLock in flight, then fill");
            }
            else
            {
                cursor = LogPut(cursor, "; ★ LocalLock REFUSED -- text NOT saved");
            }
        }
    }
    return cursor;
}

/* LocalLock answered with a near offset into the application's data segment: read the text there and hand it to the EDIT control. */
static PSTR WowCallbackEditText(
    PSTR cursor,
    const INT callAction,
    const WORD actionArgument,
    const DWORD result,
    volatile BYTE * const tib)
{
    /* FOLLOW THE POINTER THE GUEST JUST HANDED BACK (Importance = 5):
     * `LocalLock` answered with a near offset into the application's own
     * data segment, and the text an EDIT control is supposed to show is
     * there. This is the moment it can be read -- the guest has
     * returned, its DGROUP is still what it was, and the block is
     * locked, which is the whole reason for having locked it.
     *
     * [CAUTION]: THE SELECTOR IS THE CONTROL'S OWN hInstance, which in Win16 IS
     * the instance's DGROUP selector -- the same one the call was
     * entered with, so the offset and the segment come from one place.
     *
     * [CAUTION]: AND UNLOCK IT AGAIN. We took the lock, so we owe the release, and
     * leaving a moveable block permanently locked would quietly pin the
     * application's heap. The unlock is a second call made from here,
     * which is sound for exactly the reason the first one was: the guest
     * is parked at our stub with its own stack under it.
     */
    if (callAction == WOWCALL_ACT_EDITTEXT)
    {
        WOWUSER_WINDOW *editWindow = WowUserFindWindow(actionArgument);
        DWORD offset = result & WORD_MASK;
        DWORD instanceBase = editWindow ? DpmiSelectorBase(editWindow->Instance) : 0;
        cursor = LogPut(cursor, " -- EDIT text at 0x"); cursor = LogHex(cursor, editWindow ? editWindow->Instance : 0);
        cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, offset);
        if (!editWindow || !editWindow->Window32 || !instanceBase || !offset)
        {
            cursor = LogPut(cursor, " -- ★ UNREADABLE, the control keeps no text");
        }
        else
        {
            static CHAR editText[16384];
            const volatile BYTE *sourceBytes =
                (const volatile BYTE *)(ULONG_PTR)(instanceBase + offset);
            INT length = 0;
            while (length < (INT)sizeof editText - 1
                   && MemoryReadable((ULONG_PTR)(sourceBytes + length), 1) && sourceBytes[length])
                {
                    editText[length] = (CHAR)sourceBytes[length];
                    ++length;
                }
            editText[length] = 0;
            SetWindowTextA(editWindow->Window32, editText);
            cursor = LogPut(cursor, " -> 0x"); cursor = LogHex(cursor, (DWORD)length);
            cursor = LogPut(cursor, " bytes into the real control");
            if (length == (INT)sizeof editText - 1)
                cursor = LogPut(cursor, " (★ TRUNCATED at the host's buffer)");
        }
        /* Release the lock we took, whatever came of the read. */
        if (editWindow && editWindow->Memory16 && g_WowUserKernelSegment)
        {
            WORD  editCallbackSelector = WowCallbackSelector();
            DWORD editStackBase  = DpmiSelectorBase(
                            (WORD)VDM_REG16(tib, VTIB_SS));
            WORD  unlockArgument  = editWindow->Memory16;
            if (editCallbackSelector && editStackBase
                && WowCallEnter(tib, editStackBase, editCallbackSelector,
                                 ((DWORD)g_WowUserKernelSegment << WORD_SHIFT)
                                     | WOWUSER_KRNL_LOCALUNLOCK_OFF,
                                 editWindow->Instance, &unlockArgument, 1, 0,
                                 WOWCALL_RET_KEEP, NULL, editWindow->Window16, 0,
                                 NULL, 0, -1, WOWCALL_PROCEDURE_PRESENT))
                cursor = LogPut(cursor, "; LocalUnlock in flight");
            else
                cursor = LogPut(cursor, "; ★ LocalUnlock REFUSED -- the block stays"
                            " locked");
        }
    }
    return cursor;
}

/* DOS INT 21h issued by a PM client. */
static INT DpmiServiceInt21(
    PSTR *cursorIo,
    PSTR const base,
    const DWORD ax,
    const UINT steps,
    volatile BYTE * const tib,
    DOS_MACHINE * const machine,
    INT *exitCodeOut)
{
    PSTR cursor = *cursorIo;
        DWORD ah = (ax >> BYTE_SHIFT) & BYTE_MASK;

        if (ah == DOS_FN_EXIT)                            /* terminate */
        {
            /* (The `ver` canary that stood here read linear 0x1600 and
             * expected 0x005A -- a leftover from the first DPMI spike, and
             * it printed "MISMATCH" on every real client's exit.)
             */
            g_PmClientExited = 1;
            g_PmExitCode = (INT)(ax & BYTE_MASK);
            cursor = LogPut(cursor, "INT21h AH=4Ch -> client EXIT after "); cursor = LogHex(cursor, steps);
            cursor = LogPut(cursor, " svc, code=0x"); cursor = LogHexByte(cursor, (BYTE)(ax & BYTE_MASK));
            cursor = LogPut(cursor, g_InPmIrq ? " (INSIDE an injected interrupt handler)\r\n"
                                    : "\r\n");
            LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
            {
                *cursorIo = cursor;
                *exitCodeOut = 0;
                return HOST_FLOW_RETURN;
            }
        }
        if (ah == DOS_FN_PRINT_STRING)                            /* print $-string DS:DX */
        {
            /* Resolve DS's linear base from the LDT so a client can print
             * through a descriptor it allocated + based itself.
             */
            DWORD dsBase = DpmiSelectorBase((WORD)VDM_REG(tib, VTIB_DS));
            const volatile BYTE *sourceBytes = (const volatile BYTE *)(ULONG_PTR)
                (dsBase + VDM_REG16(tib, VTIB_EDX));
            CHAR outputLine[256];
            PSTR outputCursor = outputLine;
            INT item;
            outputCursor = LogPut(outputCursor, "INT21h AH=09 print: \"");
            for (item = 0; item < 200 && *sourceBytes != '$'; ++item, ++sourceBytes)
            {
                BYTE printCharacter = *sourceBytes;
                if (printCharacter >= ASCII_SPACE)
                    *outputCursor++ = (CHAR)printCharacter;                                  /* printable -> serial echo */
                if (machine->FileHandles[1])                        /* #256: redirected stdout */
                {
                    DWORD bytesWritten9 = 0;
                    WriteFile(machine->FileHandles[1], &printCharacter, 1, &bytesWritten9, NULL);
                    continue;
                }
                if (machine->ConsoleOut)
                    machine->ConsoleOut(machine->ConsoleOutContext, printCharacter);                       /* -> the Luna console */
                if (machine->OutputLength < machine->OutputCapacity - 1)
                    machine->Output[machine->OutputLength++] = (CHAR)printCharacter;
                else
                    machine->IsOutputTruncated = 1;
            }
            outputCursor = LogPut(outputCursor, "\"\r\n");
            LogAppend(LOG_PATH, outputLine, outputCursor); SerialOut(outputLine, outputCursor);
            VDM_REG(tib, VTIB_EFLAGS) &= ~EFLAGS_CF_U;
            VDM_REG(tib, VTIB_EIP) += DPMI_PM_BOP_LENGTH;
            {
                *cursorIo = cursor;
                *exitCodeOut = 1;
                return HOST_FLOW_RETURN;
            }
        }
        if (ah == DOS_FN_CHAR_OUTPUT)                            /* print char DL */
        {
            BYTE ch = VDM_REG(tib, VTIB_EDX) & BYTE_MASK;
            /* #256: standard output, so a redirected handle 1 gets it --
             * as the V86 OUTC does.
             */
            if (machine->FileHandles[1])
            {
                DWORD bytesWritten1 = 0;
                WriteFile(machine->FileHandles[1], &ch, 1, &bytesWritten1, NULL);
            }
            else
            {
                if (machine->ConsoleOut)
                    machine->ConsoleOut(machine->ConsoleOutContext, ch);
                if (machine->OutputLength < machine->OutputCapacity - 1)
                    machine->Output[machine->OutputLength++] = (CHAR)ch;
                else
                    machine->IsOutputTruncated = 1;
            }
            VDM_REG(tib, VTIB_EFLAGS) &= ~EFLAGS_CF_U;
            VDM_REG(tib, VTIB_EIP) += DPMI_PM_BOP_LENGTH;
            {
                *cursorIo = cursor;
                *exitCodeOut = 1;
                return HOST_FLOW_RETURN;
            }
        }
        {
            INT exitCode;
            INT flow = DpmiInt21Write(&cursor, base, ah, tib, machine, &exitCode);
            if (flow == HOST_FLOW_RETURN)
            {
                *cursorIo = cursor;
                *exitCodeOut = exitCode;
                return HOST_FLOW_RETURN;
            }
        }
        {
            INT exitCode;
            INT flow = DpmiInt21CreateOpen(&cursor, base, ah, tib, ax, machine, &exitCode);
            if (flow == HOST_FLOW_RETURN)
            {
                *cursorIo = cursor;
                *exitCodeOut = exitCode;
                return HOST_FLOW_RETURN;
            }
        }
        if (ah == DOS_FN_CLOSE)                            /* close: BX=handle */
        {
            DWORD handle = VDM_REG16(tib, VTIB_EBX);
            INT wasOpen = (handle < DOS_MAX_FILES && machine->FileHandles[handle]) ? 1 : 0;
            if (handle >= DOS_STD_HANDLES && handle < DOS_MAX_FILES && machine->FileHandles[handle])
                DosHandleRelease(machine, handle);                                                                                        /* s81: a parent may hold it */
            VDM_REG(tib, VTIB_EFLAGS) &= ~EFLAGS_CF_U;
            /* Silent until session 37, which needed to know whether a handle
             * was still open when the NEXT open of the same file failed --
             * "who still holds it" is unanswerable from a trace that logs
             * every open and no close.
             */
            cursor = LogPut(cursor, "INT21h AH=3E close h=0x"); cursor = LogHex(cursor, handle);
            cursor = LogPut(cursor, wasOpen ? " (was open)\r\n" : " (was NOT open)\r\n");
            LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
            VDM_REG(tib, VTIB_EIP) += DPMI_PM_BOP_LENGTH;
            {
                *cursorIo = cursor;
                *exitCodeOut = 1;
                return HOST_FLOW_RETURN;
            }
        }
        {
            INT exitCode;
            INT flow = DpmiInt21Read(&cursor, base, ah, tib, machine, &exitCode);
            if (flow == HOST_FLOW_RETURN)
            {
                *cursorIo = cursor;
                *exitCodeOut = exitCode;
                return HOST_FLOW_RETURN;
            }
        }
        if (ah == DOS_FN_SEEK)                            /* lseek: AL=org BX=h CX:DX=off */
        {
            DWORD handle = VDM_REG16(tib, VTIB_EBX);
            DWORD seekMethod = ax & BYTE_MASK;
            LONG seekDistance = (LONG)((VDM_REG16(tib, VTIB_ECX) << WORD_SHIFT) | VDM_REG16(tib, VTIB_EDX));
            VDM_REG(tib, VTIB_EFLAGS) &= ~EFLAGS_CF_U;
            if (handle >= DOS_STD_HANDLES && handle < DOS_MAX_FILES && machine->FileHandles[handle])
            {
                DWORD newPosition = SetFilePointer(machine->FileHandles[handle], seekDistance, NULL, seekMethod);
                VDM_SET16(tib, VTIB_EDX, newPosition >> WORD_SHIFT);
                VDM_SET16(tib, VTIB_EAX, newPosition & WORD_MASK);
                cursor = LogPut(cursor, "INT21h AH=42 seek h="); cursor = LogHex(cursor, handle);
                cursor = LogPut(cursor, " org="); cursor = LogHex(cursor, seekMethod);
                cursor = LogPut(cursor, " dist=0x"); cursor = LogHex(cursor, (DWORD)seekDistance);
                cursor = LogPut(cursor, " -> pos=0x"); cursor = LogHex(cursor, newPosition);
            }
            else { VDM_REG(tib, VTIB_EFLAGS) |= EFLAGS_CF_U;
                cursor = LogPut(cursor, "INT21h AH=42 seek BAD HANDLE ");
                cursor = LogHex(cursor, handle); }
            /* [CAUTION]: THIS ARM LOGGED NOTHING AT ALL, and that cost a run: the
             * seek WAS being serviced and the log's silence read exactly
             * like it never happened, which sent the search after the
             * chain-to-DOS path instead of after the real blocker. Every
             * other file arm here prints; this one did not.
             */
            cursor = LogPut(cursor, "\r\n");
            LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
            VDM_REG(tib, VTIB_EIP) += DPMI_PM_BOP_LENGTH;
            {
                *cursorIo = cursor;
                *exitCodeOut = 1;
                return HOST_FLOW_RETURN;
            }
        }
        {
            INT exitCode;
            INT flow = DpmiInt21Allocate(&cursor, base, ah, tib, machine, &exitCode);
            if (flow == HOST_FLOW_RETURN)
            {
                *cursorIo = cursor;
                *exitCodeOut = exitCode;
                return HOST_FLOW_RETURN;
            }
        }
        {
            INT exitCode;
            INT flow = DpmiInt21Free(&cursor, base, ah, tib, &exitCode);
            if (flow == HOST_FLOW_RETURN)
            {
                *cursorIo = cursor;
                *exitCodeOut = exitCode;
                return HOST_FLOW_RETURN;
            }
        }
        {
            INT exitCode;
            INT flow = DpmiInt21SetGetVector(&cursor, base, ah, ax, tib, &exitCode);
            if (flow == HOST_FLOW_RETURN)
            {
                *cursorIo = cursor;
                *exitCodeOut = exitCode;
                return HOST_FLOW_RETURN;
            }
        }
        {
            INT exitCode;
            INT flow = DpmiInt21Resize(&cursor, base, ah, tib, &exitCode);
            if (flow == HOST_FLOW_RETURN)
            {
                *cursorIo = cursor;
                *exitCodeOut = exitCode;
                return HOST_FLOW_RETURN;
            }
        }
        /* AH=44h IOCTL: only the register-only subfunctions. AL=00 (get
         * device info, BX in / DX out) is what a C runtime's isatty() uses
         * and what DOS/4GW calls for handles 0..4; AL=06/07 (input/output
         * status) likewise touch no memory. Everything else takes a DS:DX
         * buffer and stays loud -- the whitelist rule, applied within a
         * function rather than to it.
         */
        if (ah == DOS_FN_IOCTL)
        {
            /* [CAUTION]: THE WHITELIST'S COMMENT WAS WRONG ABOUT 08/09/0E.
             * "Everything else takes a DS:DX buffer" is true of most
             * of AH=44h and false of exactly these three, which are
             * the drive-classification trio (removable? remote? drive
             * map?) and answer purely in registers. Excluding them
             * sent krnl386's per-drive probe loop into the TODO arm
             * once per drive -- see the note in dos_int21.c for what
             * that cost.
             */
            DWORD al = ax & BYTE_MASK;
            if (al != DOS_INT21_IOCTL_GET_DEVICE_INFO && al != DOS_INT21_IOCTL_INPUT_STATUS && al != DOS_INT21_IOCTL_OUTPUT_STATUS &&
                al != DOS_INT21_IOCTL_REMOVABLE && al != DOS_INT21_IOCTL_REMOTE_DRIVE && al != DOS_INT21_IOCTL_GET_DRIVE_MAP)
                goto pmInt21Unhandled;
        }
        /* AH=06h direct console I/O is register-only in BOTH directions
         * (DL=char out, DL=FFh -> AL=char in, ZF), so it thunks with no
         * translation. It is also how DOS/4GW prints its FATAL ERRORS --
         * leaving it unimplemented is why "not enough memory for dispatcher
         * data" was invisible for a whole session and had to be
         * reconstructed character by character out of the TODO log lines.
         */
        /* THE PM FILE API. (GH #128):
         * krnl386 hooks INT 21h in protected mode, offers each call to
         * its 32-bit companion, and chains the ones it declines to real
         * DOS -- which is us. So its file I/O arrives HERE, and until
         * now landed in "PM thunk TODO": AH=3Dh, 3Fh, 43h, 57h and the
         * rest were simply not answered, which is why it read an NE
         * header out of a buffer nothing had filled.
         * Register-only calls just need the whitelist below. The ones that
         * take a POINTER cannot use it, because dos_int21.c resolves a
         * guest pointer as `(DS << 4) + DX` -- correct for V86 and
         * meaningless for a selector. PmInt21Transfer() bridges that by
         * copying through a conventional-memory buffer.
         */
        cursor = WowPspEnvironmentCheck(cursor, "a PM INT 21h");
        /* #210: the long-filename API -- PmInt21Lfn says how and why. */
        if (ah == DOS_FN_LFN)
        {
            if (g_PmTransferSegment)
            {
                machine->TraceCursor = cursor;
                cursor = PmInt21Lfn(machine, tib, cursor);
            }
            else
            {
                VDM_SET16(tib, VTIB_EAX, DOS_FN_LFN << BYTE_SHIFT);
                VDM_REG(tib, VTIB_EFLAGS) |= EFLAGS_CF_U;
                cursor = LogPut(cursor, "INT21h AX=71xx (PM, no xfer buffer) -> AX=7100 CF=1"
                            " (no LFN API for this client)\r\n");
            }
            LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
            VDM_REG(tib, VTIB_EIP) += DPMI_PM_BOP_LENGTH;
            {
                *cursorIo = cursor;
                *exitCodeOut = 1;
                return HOST_FLOW_RETURN;
            }
        }
        if (g_PmTransferSegment && (ah == DOS_FN_OPEN || ah == DOS_FN_READ || ah == DOS_FN_WRITE ||
                              ah == DOS_FN_DELETE || ah == DOS_FN_FILE_ATTRIBUTES || ah == DOS_FN_FIND_FIRST ||
                              ah == DOS_FN_MKDIR || ah == DOS_FN_RMDIR || ah == DOS_FN_CHDIR))
        {
            machine->TraceCursor = cursor;
            cursor = PmInt21Transfer(machine, tib, ah, cursor);
            LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
            VDM_REG(tib, VTIB_EIP) += DPMI_PM_BOP_LENGTH;
            {
                *cursorIo = cursor;
                *exitCodeOut = 1;
                return HOST_FLOW_RETURN;
            }
        }
        /* AH=34h hands back a FAR POINTER to the InDOS flag. In V86 that is
         * a segment; a PM caller needs a SELECTOR over the same linear
         * address, which is exactly what INT 31h 0002 builds -- so build one
         * and answer with it rather than handing back a paragraph the guest
         * would shift by four and miss by a megabyte.
         */
        /* AH=52h IS THE SAME SHAPE, AND IT #GP'd THE RUN. (GH #128):
         * krnl386 calls it from PROTECTED MODE, having just allocated one
         * descriptor for the answer, and then immediately reads through
         * the ES:BX it gets back. In the TODO arm ES was left at 0 -- the
         * null selector -- so that read was a #GP, which is the fault
         * session 34 first delivered to krnl386's own handler and watched
         * it turn into ExitKernelThunk(2). It is not a mysterious fault:
         * it is this function not answering. Same treatment as 34h --
         * the V86 handler fills ES:BX with the SysVars segment, and the
         * segment becomes a selector over the same linear address.
         */
        if (ah == DOS_FN_GET_INDOS_FLAG || ah == DOS_FN_GET_LIST_OF_LISTS)
        {
            WORD esSelector;
            machine->TraceCursor = cursor;
            DosInt21SetProtectedMode(TRUE);
            DosInt21(machine);
            DosInt21SetProtectedMode(FALSE);
            cursor = machine->TraceCursor;
            esSelector = DpmiSegmentToDescriptor((WORD)VDM_REG16(tib, VTIB_ES));
            cursor = LogPut(cursor, "INT21h AH=0x"); cursor = LogHex(cursor, ah);
            cursor = LogPut(cursor, " (PM) far pointer at V86 0x");
            cursor = LogHex(cursor, VDM_REG16(tib, VTIB_ES));
            cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_EBX));
            if (esSelector)
            {
                VDM_SET16(tib, VTIB_ES, esSelector);
                cursor = LogPut(cursor, " -> sel 0x"); cursor = LogHex(cursor, esSelector);
            }
            else
                cursor = LogPut(cursor, " -> NO DESCRIPTOR AVAILABLE (left as a segment)");
            cursor = LogPut(cursor, "\r\n");
            LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
            VDM_REG(tib, VTIB_EIP) += DPMI_PM_BOP_LENGTH;
            {
                *cursorIo = cursor;
                *exitCodeOut = 1;
                return HOST_FLOW_RETURN;
            }
        }
        /* 0Eh select disk, 3Eh close, 42h seek, 45h dup, 46h dup2, 57h
         * get/set file date-time and DCh get-logical-drive-map are all
         * register-only in both directions -- krnl386 asks for 0Eh, 57h and
         * DCh within the first dozen calls, and every one of them was
         * landing in the TODO arm for want of an entry in this list.
         * -- #166: THE REST OF DOS'S REGISTER-ONLY FUNCTIONS, NOT JUST THE
         * ONES A MEASURED GUEST ASKED FOR. The list grew one TODO line at a
         * time, and "this path has broken krnl386 several times" is what
         * that costs. Added, each taking and returning registers only:
         * 03/04/05 AUX and printer I/O, 0Bh input status, 0Dh disk reset,
         * 2Bh/2Dh set date/time, 2Eh/54h verify flag, 36h free space (DL
         * in, AX/BX/CX/DX out), 37h switch character, 4Dh child return
         * code, 5Ch lock/unlock (BX, CX:DX, SI:DI), 66h code page, 67h
         * handle count, 68h/6Ah commit.
         *
         * [CAUTION]: NOT 01h/07h/08h/0Ch: those READ THE KEYBOARD by re-entering
         * (`m->retry`), which this synchronous path cannot honour -- they
         * would return with no key. They stay in the loud arm.
         */
        if (ah == DOS_FN_GET_DRIVE || ah == DOS_FN_GET_DATE || ah == DOS_FN_GET_TIME || ah == DOS_FN_GET_VERSION ||
            ah == DOS_FN_BREAK_STATE || ah == DOS_FN_ALLOCATION_STRATEGY || ah == DOS_FN_DIRECT_CONSOLE_IO || ah == DOS_FN_IOCTL ||
            ah == DOS_FN_SELECT_DRIVE || ah == DOS_FN_CLOSE || ah == DOS_FN_SEEK || ah == DOS_FN_DUP ||
            ah == DOS_FN_DUP2 || ah == DOS_FN_FILE_DATE_TIME || ah == DOS_FN_GET_LOGICAL_DRIVE_MAP ||
            ah == DOS_FN_AUX_INPUT || ah == DOS_FN_AUX_OUTPUT || ah == DOS_FN_PRINTER_OUTPUT || ah == DOS_FN_INPUT_STATUS ||
            ah == DOS_FN_DISK_RESET || ah == DOS_FN_SET_DATE || ah == DOS_FN_SET_TIME || ah == DOS_FN_SET_VERIFY ||
            ah == DOS_FN_GET_FREE_SPACE || ah == DOS_FN_SWITCH_CHAR || ah == DOS_FN_GET_RETURN_CODE || ah == DOS_FN_GET_VERIFY ||
            ah == DOS_FN_LOCK || ah == DOS_FN_CODE_PAGE || ah == DOS_FN_SET_HANDLE_COUNT || ah == DOS_FN_COMMIT ||
            ah == DOS_FN_COMMIT_6A)
        {
            machine->TraceCursor = cursor;
            DosInt21SetProtectedMode(TRUE);
            DosInt21(machine);
            DosInt21SetProtectedMode(FALSE);
            cursor = machine->TraceCursor;
            cursor = LogPut(cursor, "INT21h AH=0x"); cursor = LogHex(cursor, ah);
            cursor = LogPut(cursor, " (PM, register-only -> V86 DOS) -> AX=0x");
            cursor = LogHex(cursor, VDM_REG16(tib, VTIB_EAX));
            cursor = LogPut(cursor, " BX=0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_EBX));
            cursor = LogPut(cursor, " CF="); cursor = LogHex(cursor, VDM_REG(tib, VTIB_EFLAGS) & EFLAGS_CF_U);
            cursor = LogPut(cursor, "\r\n");
            LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
            VDM_REG(tib, VTIB_EIP) += DPMI_PM_BOP_LENGTH;
            {
                *cursorIo = cursor;
                *exitCodeOut = 1;
                return HOST_FLOW_RETURN;
            }
        }
        {
            INT exitCode;
            INT flow = DpmiInt21CreatePsp(&cursor, base, ah, tib, &exitCode);
            if (flow == HOST_FLOW_RETURN)
            {
                *cursorIo = cursor;
                *exitCodeOut = exitCode;
                return HOST_FLOW_RETURN;
            }
        }
    pmInt21Unhandled:
        /* -- INT 21h AX=FF80h -- "LOCK THIS MEMORY", AND THE ANSWER
         * HERE IS YES. -------------------------------------------------
         * Rational's DOS/16M asks its host to lock a region through this
         * call before it will run (AX=FF80h, DX=1301h, ES = the region,
         * as the call arrives) and treats CF=1 as a hard failure --
         * message 34 of its table, exactly what ZAR printed on the desktop:
         *     DOS/16M error: [34]  DPMI host error (cannot lock stack)
         * - THE ANSWER IS THE ONE THIS HOST ALREADY GIVES FOR THE DPMI TWIN.
         *   INT 31h AX=0600h (lock linear region) is answered CF=0 with the
         *   note "there is no virtual memory to lock, so lock is already
         *   true". Guest memory here is committed conventional/extended
         *   memory that is never paged out, so a lock request is a statement
         *   that is already satisfied -- and saying CF=1 does not describe
         *   the machine, it invents a failure.
         *
         * [CAUTION]: SCOPED TO FF80h EXACTLY, and the fallthrough below is unchanged.
         * The register dump under it exists because AH=0xFF is not a DOS
         * function and the session-16 trace could not tell a real call from
         * inherited garbage; that reasoning still applies to every OTHER
         * FFxx, so only the subfunction with a guest and a decoded call
         * site behind it is answered. Anything else still lands in the
         * dump, which is what names the next one.
         */
        if (VDM_REG16(tib, VTIB_EAX) == PM_INT21_LOCK_REGION)
        {
            VDM_REG(tib, VTIB_EFLAGS) &= ~EFLAGS_CF_U;      /* CF=0 -- locked */
            cursor = LogPut(cursor, "INT21h AX=FF80h lock region -> CF=0 (guest memory is"
                        " never paged here, so it is already locked)\r\n");
            LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
            VDM_REG(tib, VTIB_EIP) += DPMI_PM_BOP_LENGTH;
            {
                *cursorIo = cursor;
                *exitCodeOut = 1;
                return HOST_FLOW_RETURN;
            }
        }
        /* UNHANDLED INT 21h FROM PM: SAY ENOUGH TO IDENTIFY IT:
         * The session-16 trace reported five calls with "AH=0xff", which is
         * not a DOS function at all -- so either the client really is passing
         * 0xFF, or AH is inherited garbage from the caller, or the vector
         * resolution put us here wrongly. The old one-line log could not tell
         * those apart, and guessing between them is exactly the reasoning-
         * instead-of-measuring failure this project keeps paying for. Dump the
         * full register file and the caller's bytes so the NEXT run names it.
         * Note the caller's INT is 2 bytes back from EIP (the patched BOP).
         */
        { DWORD cs = VDM_REG16(tib, VTIB_CS);
          DWORD codeBase = DpmiSelectorBase((WORD)cs);
          DWORD ip = VDM_REG(tib, VTIB_EIP);
          cursor = LogPut(cursor, "INT21h AH=0x"); cursor = LogHex(cursor, ah); cursor = LogPut(cursor, " (PM thunk TODO)");
          cursor = LogPut(cursor, " EAX=0x"); cursor = LogHex(cursor, VDM_REG(tib, VTIB_EAX));
          cursor = LogPut(cursor, " EBX=0x"); cursor = LogHex(cursor, VDM_REG(tib, VTIB_EBX));
          cursor = LogPut(cursor, " ECX=0x"); cursor = LogHex(cursor, VDM_REG(tib, VTIB_ECX));
          cursor = LogPut(cursor, " EDX=0x"); cursor = LogHex(cursor, VDM_REG(tib, VTIB_EDX));
          cursor = LogPut(cursor, " DS=0x");  cursor = LogHex(cursor, VDM_REG16(tib, VTIB_DS));
          cursor = LogPut(cursor, " ES=0x");  cursor = LogHex(cursor, VDM_REG16(tib, VTIB_ES));
          cursor = LogPut(cursor, " cs:eip=0x"); cursor = LogHex(cursor, cs);
          cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, ip);
          cursor = LogPut(cursor, " bytes@int=");
          /* Same guard as the checkpoint dump, and for the same reason it
           * must be HostReadable() and never IsBadReadPtr: a probe that
           * faults on purpose is caught by our own VEH mid-PM-run.
           */
          { const BYTE *codeBytes = (const BYTE *)(ULONG_PTR)(codeBase + ip - DPMI_PM_BOP_LENGTH);
            if (!HostReadable(codeBytes, 16))
                cursor = LogPut(cursor, "<unreadable from host>");
            else
                cursor = LogDump(cursor, codeBytes, 16); }
          cursor = LogPut(cursor, "\r\n"); }
        /* -- AND ANSWER IT THE WAY OUR OWN DOS ANSWERS AN UNHANDLED
         *  SERVICE: CF=1. ------------------------------------------
         * This arm used to return with the flags exactly as the client
         * left them, which in practice means CF=0 -- it told the client
         * its request SUCCEEDED. DosInt21()'s unhandled arm sets CF=1
         * and says why: "a quiet success would tell the program its
         * request worked when nothing happened". The protected-mode path
         * has no business disagreeing with the real-mode path about that.
         * It matters here: DOS/4GW routes these through a generic register-
         * block thunk, so whatever it is probing for, a false success sends
         * it down the branch for a feature we do not have. AX is left alone,
         * same as ERRCF() in DosInt21 -- CF is the answer, not a code we
         * would be inventing.
         */
        VDM_REG(tib, VTIB_EFLAGS) |= EFLAGS_CF_U;
        LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
        VDM_REG(tib, VTIB_EIP) += DPMI_PM_BOP_LENGTH;
        {
            *cursorIo = cursor;
            *exitCodeOut = 1;
            return HOST_FLOW_RETURN;
        }
    *cursorIo = cursor;
    return HOST_FLOW_NEXT;
}

/* DPMI INT 31h: the client's DPMI calls, dispatched by AX. */
static INT DpmiServiceInt31(
    PSTR *cursorIo,
    PSTR const base,
    DWORD *axIo,
    volatile BYTE * const tib,
    DOS_MACHINE * const machine,
    INT *exitCodeOut)
{
    PSTR cursor = *cursorIo;
    DWORD ax = *axIo;
    INT needScan = 0;   /* deferred: scanning logs, so do it after the flush */

    cursor = LogPut(cursor, "INT31h AX=0x"); cursor = LogHex(cursor, ax);
    cursor = LogPut(cursor, " BX=0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_EBX));
    cursor = LogPut(cursor, " CX=0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_ECX));
    VDM_REG(tib, VTIB_EFLAGS) &= ~EFLAGS_CF_U;          /* default CF=0 (success) */
    /* 0300 MUST INVOKE THE GUEST'S OWN REAL-MODE HANDLER (Importance = 4):
     * DPMI 0300 is specified as "simulate a real-mode interrupt": take
     * CS:IP from the REAL-MODE IVT and run the handler there. Ours
     * services a few vectors host-side (21h, 33h, 10h) and, for
     * everything else, did nothing while reporting success -- so a
     * guest that INSTALLS ITS OWN handler and then asks us to call it
     * was answered "done" and its handler never ran.
     *
     * [INFO]: ZAR does exactly that (GH #23): `INT 31h 0201` sets the
     * real-mode INT 66h vector to its own trampoline at
     * 0x34d3:0x01d1, it calls `0300 BL=66h` three times (AX=0300,
     * 0301, 0304 -- an AIL-style private API), and then RESTORES the
     * vector. Measured: `simInt UNHANDLED int66h x3`, and of 2,386
     * real-mode calls in a run EVERY ONE went to our own DOS handler
     * at 0050:0000 -- the Miles driver it loads (SOUND\SBLASTER.DIG,
     * opened, seeked and read) is never once called, and no SB port
     * is ever touched.
     * - REUSE 0302 RATHER THAN GROWING A SECOND RUN LOOP. 0302 already
     *   does the whole PM->V86->PM round trip with an IRET frame, which
     *   is precisely an interrupt's frame; the only thing 0300 adds is
     *   "and the address comes from the IVT". So write the IVT entry
     *   into the RMCS's CS:IP and let the 0302 arm run it. The printed
     *   `AX=0x00000300 ... -> callRM(iret) 0x34d3:0x01d1` says plainly
     *   what happened.
     *
     * [CAUTION]: ONLY WHEN THE GUEST OWNS THE VECTOR. If the IVT still points at
     * one of OUR stubs (DOS_HDLR_SEG) there is no guest handler to
     * run, and reflecting would re-enter our own BOP through a path
     * it was not written for. Those keep today's behaviour and stay
     * visible in `STAGE2: simInt (DPMI 0300) UNHANDLED`, which is
     * where the next one of these will be found.
     */
    /* -- OK s81: NOW ON BY DEFAULT. The wedge below was three gaps, all
     * closed: the nested loop never set g_InExec (so IRQ 5 was refused
     * as not_in_exec, which s59 misread as HOST_CS); the BIOS tick did
     * not advance inside a nested call (Miles times its self-test by
     * it); and the PM default IRQ stub could not reflect to a guest-
     * owned real-mode ISR (DpmiReflectIrqToRm). History kept:
     * -- [CAUTION] ...AND IT WAS OFF BY DEFAULT, BECAUSE IT WEDGED ZAR.
     * MEASURED, and the result is worth the knob. With it on, ZAR's
     * INT 66h handler runs and **the Sound Blaster is programmed for
     * the first time** -- `SNDIO out 0x22c`, `sb{blocks=1
     * rate=0x56ce}` = 22222 Hz, exactly the `sound SamplingRate
     * 22222` in USER1.CFG. So the reflection is right and it is what
     * stands between this host and ZAR's audio.
     *
     * [CAUTION]: But the guest then SPINS IN REAL MODE at 0x34d3:0x06b1 with a
     * DMA block queued and never draining (`left=0x10 len=0x10
     * blocks=1`, `irq0=1 intpend=1`), until the watchdog kills it.
     * The driver is waiting on a completion that never arrives: the
     * nested V86 loop 0301/0302 runs the real-mode procedure in does
     * not appear to deliver the SB's IRQ, so `VdmRunGuest` never returns
     * and the poll never ends. That is the NEXT gap, not this one.
     * - A wedge is strictly worse than "renders, silent", so the
     *   correct default is OFF and the correct shape is a one-file
     *   switch -- the same call `wowquiet.txt` and `pitinj.txt` make.
     * - Turn it on to work the audio thread; leave it off to play.
     */
    /* GH #247: 0300h RUNS WHATEVER THE IVT HOLDS -- OURS INCLUDED (Importance = 5):
     * The guard above ("ONLY WHEN THE GUEST OWNS THE VECTOR") left every
     * vector whose IVT entry is one of OUR stubs -- 16h, 1Ah, 2Fh, 67h, the
     * BIOS block -- answered by nothing: registers echoed, CF=0. A
     * Watcom/DJGPP int86() for the keyboard, the clock, INT 15h or the
     * multiplex got its own question back as the answer. And the BIOS
     * block's stubs live at DOS_CTAB_SEG, not DOS_HDLR_SEG, so they DID
     * pass the guard -- into a nested loop that did not know their BOPs,
     * which abandoned the call as an "unexpected RM event".
     * - Now ONE rule, the spec's: run IVT[BL] in V86 through the 0302
     *   machinery. The nested loop services our stubs' BOPs through
     *   V86BiosBop(), the same code the exec loop runs, so "our stub" is
     *   just another real-mode handler and needs no special case.
     * - The host-side 21h/33h/10h arms in `case 0x0300` stay as a FAST
     *   PATH, and only where they give the same answer -- see
     *   RmcsSimIntRoute() in dpmi_rmcs.h for exactly when, and why INT 21h
     *   is always one.
     *
     * [CAUTION]: THE CALLER'S RMCS CS:IP IS NO LONGER WRITTEN. The first cut of the
     * reflection borrowed 0302 by storing IVT[BL] into RMCS.CS:IP; the
     * spec says 0300h ignores that field on the way in and leaves it
     * unmodified on the way out. The target now travels in simint_cs/ip.
     */
    INT simulatedVector = -1;
    WORD simulatedCs = 0;
    WORD simulatedIp = 0;
    if (ax == DPMI_FN_SIMULATE_REAL_MODE_INTERRUPT)
    {
        DWORD interruptVector = VDM_REG(tib, VTIB_EBX) & BYTE_MASK;
        WORD handlerSegment = PeekWord(IVT_SEGMENT_ADDRESS(interruptVector));
        WORD handlerOffset = PeekWord(IVT_OFFSET_ADDRESS(interruptVector));
        if (RmcsSimIntRoute(interruptVector, handlerSegment, handlerOffset, g_SimIntReflect, DOS_HDLR_SEG) == SIMINT_RUN)
        {
            simulatedVector = (INT)interruptVector;
            simulatedCs = handlerSegment;
            simulatedIp = handlerOffset;
            cursor = LogPut(cursor, " [simInt 0x"); cursor = LogHexByte(cursor, (BYTE)interruptVector);
            cursor = LogPut(cursor, (handlerSegment == DOS_HDLR_SEG || handlerSegment == DOS_CTAB_SEG)
                        ? " -> our real-mode stub 0x"
                        : " -> the guest's OWN real-mode handler 0x");
            cursor = LogHex(cursor, handlerSegment); cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, handlerOffset); cursor = LogPut(cursor, "]");
            ax = DPMI_FN_CALL_REAL_MODE_IRET;      /* ...which is a real-mode call with an IRET frame */
        }
    }
    switch (ax)
    {
    case DPMI_FN_GET_VERSION:                               /* get DPMI version */
        /* -- #248: CL IS THE SAME BYTE 1687h REPORTS. It was a hardcoded 3
         * here while 1687h said DPMI_CPU_CLASS (4) -- the s79 GetWinFlags
         * fix changed one site of two. CH stays 0, as it always was here.
         */
        VDM_SET16(tib, VTIB_EAX, DPMI_VERSION_090);  /* 0.90 */
        VDM_SET16(tib, VTIB_EBX, DPMI_VER_BX);
        VDM_SET16(tib, VTIB_ECX, DPMI_CPU_CLASS);
        VDM_SET16(tib, VTIB_EDX, DPMI_VER_DX);
        cursor = LogPut(cursor, " -> ver 0.90 cpu "); cursor = LogHexByte(cursor, DPMI_CPU_CLASS);
        break;

    case DPMI_FN_ALLOCATE_DESCRIPTORS:                               /* allocate CX descriptors */
    {
        DpmiInt31AllocateDescriptors(&cursor, tib);
        break;
    }

    case DPMI_FN_FREE_DESCRIPTOR:                               /* free descriptor BX */
    {
        DpmiInt31FreeDescriptor(&cursor, tib);
        break;
    }

    case DPMI_FN_NTVDM_ALLOCATE:                               /* NTVDM-private: allocate */
    {
        DpmiInt31NtvdmAllocate(&cursor, tib);
        break;
    }

    case DPMI_FN_NTVDM_COMMIT:                               /* NTVDM-private: COMMIT */
    {
        DpmiInt31NtvdmCommit(&cursor, tib, &needScan);
        break;
    }

    case DPMI_FN_ALLOCATE_SPECIFIC_DESCRIPTOR:                               /* allocate SPECIFIC descriptor */
    {
        cursor = DpmiInt31AllocateSpecificDescriptor(cursor, tib);
        break;
    }

    case DPMI_FN_ALLOCATE_DOS_MEMORY:                               /* allocate DOS memory: BX paras -> AX=seg, DX=sel */
    {
        DpmiInt31AllocateDosMemory(&cursor, base, tib, machine);
        break;
    }

    case DPMI_FN_FREE_DOS_MEMORY:                               /* free DOS memory: DX = selector */
    {
        DpmiInt31FreeDosMemory(&cursor, tib);
        break;
    }

    case DPMI_FN_RESIZE_DOS_MEMORY:                               /* resize DOS memory block: BX=new paras, DX=sel */
    {
        INT ldtIndex = DPMI_SELECTOR_INDEX(VDM_REG16(tib, VTIB_EDX));
        WORD want = (WORD)VDM_REG16(tib, VTIB_EBX);
        WORD maximum = 0;
        if (ldtIndex >= 1 && ldtIndex < DPMI_LDT_MAX && g_Ldt[ldtIndex].Base)
        {
            WORD segment = (WORD)(g_Ldt[ldtIndex].Base >> PARAGRAPH_SHIFT);
            INT error = DosMcbResize(NULL, segment, want, &maximum);
            if (error)
            {
                VDM_REG(tib, VTIB_EFLAGS) |= EFLAGS_CF_U;
                VDM_SET16(tib, VTIB_EAX, error);       /* DOS error (7/8/9) */
                VDM_SET16(tib, VTIB_EBX, maximum);       /* largest available (paras) */
                cursor = LogPut(cursor, " -> resize FAIL max=0x"); cursor = LogHex(cursor, maximum);
            }
            else
            {
                g_Ldt[ldtIndex].Limit = want ? ((DWORD)want << PARAGRAPH_SHIFT) - 1 : 0;
                DpmiInstall(ldtIndex);
                cursor = LogPut(cursor, " -> resize seg=0x"); cursor = LogHex(cursor, segment);
                cursor = LogPut(cursor, " to 0x"); cursor = LogHex(cursor, want); cursor = LogPut(cursor, " paras");
            }
        }
        else
        {
            VDM_REG(tib, VTIB_EFLAGS) |= EFLAGS_CF_U;
            VDM_SET16(tib, VTIB_EAX, DPMI_E_INVALID_SEL);  /* invalid sel */
            cursor = LogPut(cursor, " -> resize bad sel");
        }
        break; }

    /* -- THE REAL-MODE VECTOR PAIR. DPMI 0.9 CORE, AND WE HAD
     * NEITHER HALF. -----------------------------------------------
     * 0200h/0201h are the real-mode twins of the 0204h/0205h pair
     * below: a protected-mode client reads and writes the V86 IVT
     * through them, which is how an extender hooks a real-mode
     * interrupt on behalf of the program it is hosting. Both
     * answered UNSUP, and the asymmetry made it invisible -- the
     * log showed 0204/0205 working all around them.
     * - THE GUEST THAT FOUND IT: ZAR (GH #23). Its DOS/16M reads ALL
     *   256 real-mode vectors (0200h x 256) and then installs its own
     *   (0201h x 39). Every one was refused, and it gave up with
     *   `DOS/16M error: [34] DPMI host error (cannot lock stack)`.
     *
     * [CAUTION]: THE IVT IS THE STORE, not a shadow table of ours. A guest that
     * hooks a real-mode vector must be visible to real-mode code that
     * later executes `int nn` in V86 -- which dispatches through the
     * IVT -- and to anything reading the vector back with INT 21h
     * AH=35h. Keeping a private copy would make those three disagree.
     * It is also exactly what the guest could already do by switching
     * to real mode and storing the vector itself, so this grants no
     * access it did not have; it only makes the documented route work.
     *
     * [CAUTION]: AND IT CAN DISPLACE ONE OF OUR OWN BOP STUBS, deliberately. If
     * a guest hooks a vector we service, its handler is the one that
     * must run -- that is an ordinary TSR hooking an interrupt, and
     * refusing it is how a host lies about whose machine it is.
     */
    case DPMI_FN_GET_REAL_MODE_VECTOR:                               /* get real-mode vector: BL -> CX:DX */
    {
        DWORD bl = VDM_REG(tib, VTIB_EBX) & BYTE_MASK;
        const volatile WORD *interruptVector = (const volatile WORD *)(ULONG_PTR)(IVT_OFFSET_ADDRESS(bl));
        VDM_SET16(tib, VTIB_EDX, interruptVector[0]);       /* offset */
        VDM_SET16(tib, VTIB_ECX, interruptVector[1]);       /* segment */
        VDM_REG(tib, VTIB_EFLAGS) &= ~EFLAGS_CF_U;
        cursor = LogPut(cursor, " -> getRMvec int 0x"); cursor = LogHex(cursor, bl);
        cursor = LogPut(cursor, " = 0x"); cursor = LogHex(cursor, interruptVector[1]);
        cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, interruptVector[0]);
        break; }

    case DPMI_FN_SET_REAL_MODE_VECTOR:                               /* set real-mode vector: BL = CX:DX */
    {
        DWORD bl = VDM_REG(tib, VTIB_EBX) & BYTE_MASK;
        volatile WORD *interruptVector = (volatile WORD *)(ULONG_PTR)(IVT_OFFSET_ADDRESS(bl));
        WORD previousSegment = interruptVector[1];
        WORD previousOffset = interruptVector[0];
        interruptVector[0] = (WORD)VDM_REG16(tib, VTIB_EDX);
        interruptVector[1] = (WORD)VDM_REG16(tib, VTIB_ECX);
        VDM_REG(tib, VTIB_EFLAGS) &= ~EFLAGS_CF_U;
        cursor = LogPut(cursor, " -> setRMvec int 0x"); cursor = LogHex(cursor, bl);
        cursor = LogPut(cursor, " = 0x"); cursor = LogHex(cursor, interruptVector[1]);
        cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, interruptVector[0]);
        cursor = LogPut(cursor, " (was 0x"); cursor = LogHex(cursor, previousSegment);
        cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, previousOffset); cursor = LogPut(cursor, ")");
        break; }

    case DPMI_FN_GET_PROTECTED_MODE_VECTOR:                               /* get PM interrupt vector: BL -> CX:(E)DX */
    {
        DWORD bl = VDM_REG(tib, VTIB_EBX) & BYTE_MASK;
        VDM_SET16(tib, VTIB_ECX, g_PmInt[bl].Selector);
        /* -- THE RETURNED OFFSET IS AS WIDE AS THE CLIENT, NOT AS WIDE AS
         * THE HANDLER'S SELECTOR. This tested only the handler selector,
         * so a 16-bit handler (the extender's own stubs are all 16-bit)
         * was reported with VDM_SET16 -- which preserves the top half of
         * EDX. A 32-bit client reads all of EDX, so it got 0x????0020:
         * the right offset with sixteen bits of whatever was there
         * before glued on top.
         * That is what broke Doom's timer. DOS/4GW services the game's
         * `AH=25h` hook by first reading the vector back with 0204 to
         * walk its chain; handed a garbage offset it does not recognise
         * its own stub, fails the hook with CF=1, and later reports
         *   DOS/4GW Professional fatal error (1001):
         *   error in interrupt chain
         * Zero-extend for a 32-bit client and the offset is just 0x20.
         */
        if (DpmiSelectorIs32(g_PmInt[bl].Selector) || g_DpmiIsClient32)
            VDM_REG(tib, VTIB_EDX) = g_PmInt[bl].Offset;
        else
            VDM_SET16(tib, VTIB_EDX, g_PmInt[bl].Offset & WORD_MASK);
        cursor = LogPut(cursor, " -> getPMvec int 0x"); cursor = LogHex(cursor, bl);
        break; }

    case DPMI_FN_SET_PROTECTED_MODE_VECTOR:                               /* set PM interrupt vector: BL = CX:(E)DX */
    {
        cursor = DpmiInt31SetProtectedModeVector(cursor, tib);
        break;
    }

    /* 0202/0203 GET/SET PROTECTED-MODE EXCEPTION HANDLER:
     * DPMI 0.9: BL = exception 00h..1Fh; CX:(E)DX = handler sel:off.
     * BL > 1Fh is the one documented error (8021h, "invalid value").
     * - WHAT 0202 RETURNS BEFORE THE CLIENT HAS SET ANYTHING is a real
     *   decision, not a detail. A client that CHAINS (the usual pattern:
     *   get, save, set, and far-call the saved one for exceptions it
     *   does not want) will store whatever we hand back and jump to it.
     *   0000:0000 is therefore an unexecutable address dressed up as a
     *   valid answer. We hand back the host's own handler segment and
     *   the register-preserving RETF at DPMI_SSR_OFF, so a chain lands
     *   on a real instruction inside a selector that exists.
     *   This is NOT a working default exception handler -- the DPMI spec
     *   says a host's default terminates the client, and ours cannot yet
     *   do that from PM. It is the inert answer, and it is only ever
     *   reached if an exception actually fires, at which point the run
     *   is already lost. Recorded so the next reader does not mistake it
     *   for a considered exception-delivery design: there isn't one yet.
     */
    case DPMI_FN_GET_EXCEPTION_HANDLER:                               /* get PM exception handler: BL -> CX:(E)DX */
    {
        DWORD bl = VDM_REG(tib, VTIB_EBX) & BYTE_MASK;
        if (bl >= X86_EXCEPTIONS)
        {
            VDM_REG(tib, VTIB_EFLAGS) |= EFLAGS_CF_U;
            VDM_SET16(tib, VTIB_EAX, DPMI_E_INVALID_VALUE);
            cursor = LogPut(cursor, " -> getEXC bad exception 0x");
            cursor = LogHex(cursor, bl);
            break;
        }
        if (!g_PmException[bl].IsSet)
        {
            g_PmException[bl].Selector = DpmiHandlerCodeSelector();
            g_PmException[bl].Offset = DPMI_SSR_OFF;
        }
        VDM_SET16(tib, VTIB_ECX, g_PmException[bl].Selector);
        if (DpmiSelectorIs32(g_PmException[bl].Selector))
            VDM_REG(tib, VTIB_EDX) = g_PmException[bl].Offset;
        else
            VDM_SET16(tib, VTIB_EDX, g_PmException[bl].Offset & WORD_MASK);
        cursor = LogPut(cursor, " -> getEXC 0x"); cursor = LogHex(cursor, bl);
        cursor = LogPut(cursor, " = 0x"); cursor = LogHex(cursor, g_PmException[bl].Selector);
        cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, g_PmException[bl].Offset);
        break; }

    case DPMI_FN_SET_EXCEPTION_HANDLER:                               /* set PM exception handler: BL = CX:(E)DX */
    {
        DWORD bl = VDM_REG(tib, VTIB_EBX) & BYTE_MASK;
        WORD handlerSelector = (WORD)VDM_REG(tib, VTIB_ECX);
        if (bl >= X86_EXCEPTIONS)
        {
            VDM_REG(tib, VTIB_EFLAGS) |= EFLAGS_CF_U;
            VDM_SET16(tib, VTIB_EAX, DPMI_E_INVALID_VALUE);
            cursor = LogPut(cursor, " -> setEXC bad exception 0x");
            cursor = LogHex(cursor, bl);
            break;
        }
        g_PmException[bl].Selector = handlerSelector;
        /* same 16/32 offset rule as 0205: a 32-bit handler selector means
         * the client passed a full EDX (GH #18 run 83).
         */
        g_PmException[bl].Offset = DpmiSelectorIs32(handlerSelector) ? VDM_REG(tib, VTIB_EDX)
                                               : VDM_REG16(tib, VTIB_EDX);
        g_PmException[bl].IsSet = 1;
        cursor = LogPut(cursor, " -> setEXC 0x"); cursor = LogHex(cursor, bl);
        cursor = LogPut(cursor, " = 0x"); cursor = LogHex(cursor, g_PmException[bl].Selector);
        cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, g_PmException[bl].Offset);
        break; }

    /* 06xx PAGE LOCKING / 07xx DEMAND-PAGING HINTS:
     * These are all trivially satisfiable here, and that is a FACT about
     * this host rather than a shortcut: our 0501 blocks are VirtualAlloc'd
     * MEM_COMMIT in our own process and are never paged out by us. There
     * is no virtual memory to lock, so "lock" is already true and "unlock"
     * costs nothing; the 07xx pair are explicitly advisory in the spec
     * ("the host may ignore this call"). Returning CF=0 is the correct
     * answer, not a stub. Doom asks 0702 six times.
     * 0604 (get page size) is the one that carries information: 4096 on
     * every x86, which is not a guess -- it is architecture.
     */
    case DPMI_FN_LOCK_LINEAR_REGION:                               /* lock linear region */

    case DPMI_FN_UNLOCK_LINEAR_REGION:                               /* unlock linear region */

    case DPMI_FN_UNLOCK_REAL_MODE_REGION:                               /* unlock real-mode region */

    case DPMI_FN_RELOCK_REAL_MODE_REGION:                               /* relock real-mode region */

    case DPMI_FN_DISCARD_PAGES:                               /* discard page contents (advisory) */

    case DPMI_FN_MARK_DEMAND_PAGING:                               /* mark pages demand-paging candidates (advisory) */

    /* 0703 is the same advisory family and krnl386 asks for it
     * twice per heap it builds (GH #128). It was answered UNSUP,
     * which for an advisory call is a worse answer than silence:
     * the spec says the host may ignore it, so CF=1 tells the
     * guest something failed when nothing did.
     */
    case DPMI_FN_DISCARD_PAGE_CONTENTS:                               /* discard page contents, DPMI 1.0 (advisory) */
        VDM_REG(tib, VTIB_EFLAGS) &= ~EFLAGS_CF_U;
        cursor = LogPut(cursor, " -> paging no-op (no virtual memory here)");
        break;

    case DPMI_FN_GET_PAGE_SIZE:                               /* get page size -> BX:CX */
        VDM_SET16(tib, VTIB_EBX, 0);
        VDM_SET16(tib, VTIB_ECX, X86_PAGE_SIZE);
        VDM_REG(tib, VTIB_EFLAGS) &= ~EFLAGS_CF_U;
        cursor = LogPut(cursor, " -> page size 4096");
        break;

    case DPMI_FN_GET_AND_DISABLE_VI:                               /* get + disable virtual interrupt state */
        VDM_SET16(tib, VTIB_EAX, (DPMI_FN_GET_AND_DISABLE_VI & HIGH_BYTE_MASK) | (g_DpmiVi & 1));
        g_DpmiVi = 0; cursor = LogPut(cursor, " -> cli");
        break;

    case DPMI_FN_GET_AND_ENABLE_VI:                               /* get + enable virtual interrupt state */
        VDM_SET16(tib, VTIB_EAX, (DPMI_FN_GET_AND_DISABLE_VI & HIGH_BYTE_MASK) | (g_DpmiVi & 1));
        g_DpmiVi = 1; cursor = LogPut(cursor, " -> sti");
        break;

    case DPMI_FN_GET_VI_STATE:                               /* get virtual interrupt state -> AL */
        VDM_SET16(tib, VTIB_EAX, (DPMI_FN_GET_AND_DISABLE_VI & HIGH_BYTE_MASK) | (g_DpmiVi & 1));
        cursor = LogPut(cursor, " -> getIF ");  cursor = LogHex(cursor, g_DpmiVi);
        break;

    case DPMI_FN_GET_SEGMENT_BASE:                               /* get base of sel BX -> CX:DX */
    {
        DWORD selectorBase = DpmiSelectorBase((WORD)(VDM_REG(tib, VTIB_EBX)));
        VDM_SET16(tib, VTIB_ECX, selectorBase >> WORD_SHIFT);
        VDM_SET16(tib, VTIB_EDX, selectorBase & WORD_MASK);
        cursor = LogPut(cursor, " sel 0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_EBX));
        cursor = LogPut(cursor, " -> base 0x"); cursor = LogHex(cursor, selectorBase);
        break; }

    /* 0007/0008/0009 now act on idx>=1: since run 48 records the switch's
     * code/data/stack selectors (0x0F/0x17/0x1F) in g_Ldt[1..3], a client that
     * reconfigures its INITIAL selectors (e.g. a C runtime narrowing DS's limit)
     * must take effect -- else the change silently no-ops and the client faults.
     */
    /* -- #248: AN INVALID SELECTOR IS 8022h, NOT A SILENT NO-OP. 0007h-0009h
     * ignored a selector that was null, GDT, off the table or never
     * allocated and returned CF=0, so the client believed a descriptor
     * had been set that never was. DpmiClientSelectorOk() is the rule
     * (and its WOW exception: krnl386 owns its table).
     */
    case DPMI_FN_SET_SEGMENT_BASE:
    case DPMI_FN_SET_SEGMENT_LIMIT:
    case DPMI_FN_SET_ACCESS_RIGHTS:
        if (!DpmiClientSelectorOk((WORD)VDM_REG(tib, VTIB_EBX)))
        {
            VDM_REG(tib, VTIB_EFLAGS) |= EFLAGS_CF_U;
            VDM_SET16(tib, VTIB_EAX, DPMI_E_INVALID_SEL);
            cursor = LogPut(cursor, " sel 0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_EBX));
            cursor = LogPut(cursor, " -> REFUSED: invalid selector (8022h)");
            break;
        }
        if (ax == DPMI_FN_SET_SEGMENT_BASE)                      /* set base of sel BX = CX:DX */
        {
        INT ldtIndex = DPMI_SELECTOR_INDEX(VDM_REG16(tib, VTIB_EBX));
        DWORD newBase = (VDM_REG16(tib, VTIB_ECX) << WORD_SHIFT) | VDM_REG16(tib, VTIB_EDX);
        if (ldtIndex >= 1 && ldtIndex < DPMI_LDT_MAX)
        {
            g_Ldt[ldtIndex].Base = newBase;
            DpmiInstall(ldtIndex);
        }
        cursor = LogPut(cursor, " sel 0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_EBX));
        cursor = LogPut(cursor, " -> setbase 0x"); cursor = LogHex(cursor, newBase);
        break; }
        if (ax == DPMI_FN_SET_SEGMENT_LIMIT)                      /* set limit of sel BX = CX:DX */
        {
        INT ldtIndex = DPMI_SELECTOR_INDEX(VDM_REG16(tib, VTIB_EBX));
        DWORD limit = (VDM_REG16(tib, VTIB_ECX) << WORD_SHIFT) | VDM_REG16(tib, VTIB_EDX);
        /* 0008's limit is in BYTES and the host chooses G (DpmiInstall
         * scales anything past 1 MB). A G bit the client left in the
         * flags via 0009 CH must not survive into a byte limit: ZAR
         * sets 8092 (G=1) first and 0x4afff bytes second, and G=1 over
         * a raw 0x4afff field is a 1.2 GB segment NT refuses.
         */
        if (ldtIndex >= 1 && ldtIndex < DPMI_LDT_MAX)
        {
            g_Ldt[ldtIndex].Limit = limit;
            g_Ldt[ldtIndex].Flags &= (BYTE)~DPMI_DESCRIPTOR_FLAG_GRANULARITY;
            DpmiInstall(ldtIndex);
        }
        cursor = LogPut(cursor, " sel 0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_EBX));
        cursor = LogPut(cursor, " -> setlimit 0x"); cursor = LogHex(cursor, limit);
        break; }
        {                                      /* 0009: set access rights of sel BX (CX) */
        INT ldtIndex = DPMI_SELECTOR_INDEX(VDM_REG16(tib, VTIB_EBX));
        if (ldtIndex >= 1 && ldtIndex < DPMI_LDT_MAX)
        {
            /* CL = access byte (P|DPL|S|type). CH = descriptor byte 6
             * (G|D/B|L|AVL|limit19:16); its HIGH nibble carries G/D/B/L/AVL,
             * which maps 1:1 onto our flags nibble (see DpmiBuildDescriptor).
             * #3 (DOS/4GW): a 32-bit code selector arrives here with CH bit6
             * (D/B) set -> flags bit2 -> DpmiSelectorIs32() true.
             */
            g_Ldt[ldtIndex].Access = VDM_REG(tib, VTIB_ECX) & BYTE_MASK;
            g_Ldt[ldtIndex].Flags  = (VDM_REG(tib, VTIB_ECX) >> DPMI_SET_RIGHTS_FLAGS_SHIFT) & NIBBLE_MASK;  /* CH high nibble */
            DpmiInstall(ldtIndex);
            /* "This region is now code" is the client naming its own
             * requirement -- and the only notice we get that a module
             * it just loaded is about to be executed. Patch its INT
             * sites now; see DpmiPatchCodeRegion().
             */
            if (DPMI_ACC_IS_CODE(g_Ldt[ldtIndex].Access))
            {
                DpmiPatchCodeRegion(g_Ldt[ldtIndex].Base, g_Ldt[ldtIndex].Limit,
                                       (g_Ldt[ldtIndex].Flags & DPMI_DESCRIPTOR_FLAG_BIG) != 0);
                /* The client naming ANY region code means it has finished
                 * loading something -- a good moment to re-look at the
                 * blocks holding its EXEC objects. On DOOM.EXE this is the
                 * 25-byte 16-bit alias object, declared code four log lines
                 * after the last page of the game's code object arrived.
                 */
                needScan = 1;
            }
        }
        cursor = LogPut(cursor, " sel 0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_EBX));
        cursor = LogPut(cursor, " -> setaccess 0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_ECX));
        break; }

    case DPMI_FN_CREATE_ALIAS:                               /* create data alias of sel BX */
    {
        INT source = DPMI_SELECTOR_INDEX(VDM_REG16(tib, VTIB_EBX));
        INT ldtIndex;
        /* #248: an invalid source is 8022h. It used to alias the CODE BASE
         * instead -- a working selector onto memory the client never named.
         */
        if (!DpmiClientSelectorOk((WORD)VDM_REG(tib, VTIB_EBX)))
        {
            VDM_REG(tib, VTIB_EFLAGS) |= EFLAGS_CF_U;
            VDM_SET16(tib, VTIB_EAX, DPMI_E_INVALID_SEL);
            cursor = LogPut(cursor, " -> alias REFUSED: invalid selector (8022h)");
            break;
        }
        if (g_LdtNext >= DPMI_LDT_MAX) { VDM_REG(tib, VTIB_EFLAGS) |= EFLAGS_CF_U;
            VDM_SET16(tib, VTIB_EAX, DPMI_E_DESC_UNAVAIL);
            cursor = LogPut(cursor, " -> ENOMEM");
            break; }
        ldtIndex = g_LdtNext++;
        g_Ldt[ldtIndex] = g_Ldt[source];
        g_Ldt[ldtIndex].Access = DPMI_ACCESS_DATA;              /* data alias */
        DpmiInstall(ldtIndex);
        VDM_SET16(tib, VTIB_EAX, (WORD)DPMI_LDT_SELECTOR(ldtIndex));
        cursor = LogPut(cursor, " -> alias sel 0x"); cursor = LogHex(cursor, DPMI_LDT_SELECTOR(ldtIndex));
        break; }

    /* 000B/000C GET/SET DESCRIPTOR -- the raw 8-byte descriptor form of
     * 0006-0009. DOS/4GW (Doom) is the client that needs them, and the
     * sequence it runs is worth recording because it names its intent:
     *   mov bx,ds / mov di,sp / mov es,bx   ; 8-byte buffer on the stack
     *   mov ax,000Bh / int 31h              ; read descriptor
     *   and byte [es:di+6],0BFh             ; CLEAR the D/B bit
     *   mov ax,000Ch / int 31h              ; write it back
     *   add sp,8
     * i.e. the extender manages segment widths itself -- which is the same
     * conclusion DpmiSwitchToProtectedMode() reaches from the other direction.
     */
    case DPMI_FN_SEGMENT_TO_DESCRIPTOR:                               /* segment (BX) -> descriptor */
    {
        WORD realSegment = (WORD)VDM_REG16(tib, VTIB_EBX);
        WORD realSelector  = DpmiSegmentToDescriptor(realSegment);
        if (!realSelector)
        {
            VDM_REG(tib, VTIB_EFLAGS) |= EFLAGS_CF_U;
            VDM_SET16(tib, VTIB_EAX, DPMI_E_DESC_UNAVAIL);  /* descriptor unavailable */
            cursor = LogPut(cursor, " -> seg2desc ENOMEM");
            break;
        }
        VDM_SET16(tib, VTIB_EAX, realSelector);
        VDM_REG(tib, VTIB_EFLAGS) &= ~EFLAGS_CF_U;
        cursor = LogPut(cursor, " -> seg 0x");  cursor = LogHex(cursor, realSegment);
        cursor = LogPut(cursor, " = sel 0x");   cursor = LogHex(cursor, realSelector);
        /* [CAUTION]: Say so the moment the host-private pool overflows:
         * past that point host selectors come from the client's
         * arena again and the collision this pool exists to
         * prevent is back, silently.
         */
        if (g_HostPoolSpill)
            cursor = LogPut(cursor, " ** HOST LDT POOL EXHAUSTED -- minting from"
                        " the client arena, collisions possible **");
        break; }

    case DPMI_FN_GET_SELECTOR_INCREMENT:                               /* get selector increment value */
        /* The amount to add to a selector to reach the next one in a block
         * allocated by 0000. Our selectors are LDT entries, so 8.
         */
        VDM_SET16(tib, VTIB_EAX, X86_DESCRIPTOR_SIZE);
        VDM_REG(tib, VTIB_EFLAGS) &= ~EFLAGS_CF_U;
        cursor = LogPut(cursor, " -> sel increment 8");
        break;

    case DPMI_FN_GET_VENDOR_API:                               /* get vendor-specific API entry */
        /* Correct answer is "no such vendor API": CF=1. The default arm
         * already does that, but naming it here stops it reading as a gap
         * in the Doom trace -- DOS/4GW asks, is refused, and carries on.
         */
        VDM_REG(tib, VTIB_EFLAGS) |= EFLAGS_CF_U;
        cursor = LogPut(cursor, " -> no vendor API (CF=1, correct)");
        break;

    case DPMI_FN_GET_DESCRIPTOR:                               /* get descriptor of sel BX -> ES:(E)DI */
    {
        INT ldtIndex = DPMI_SELECTOR_INDEX(VDM_REG16(tib, VTIB_EBX));
        DWORD esBase = DpmiSelectorBase((WORD)VDM_REG(tib, VTIB_ES));
        /* Same ES:(E)DI rule as the RMCS -- see DpmiRmcsPointer. A no-op for
         * the 16-bit extender code that calls this today; the probe says so
         * in the log rather than leaving it as an assumption.
         */
        volatile DWORD *descriptor = (volatile DWORD *)(ULONG_PTR)DpmiRmcsPointer(tib, esBase);
        DWORD low = 0;
        DWORD high = 0;
        DpmiRmcsProbe(tib, esBase, 3, 0);       /* observation only */
        if (ldtIndex >= 1 && ldtIndex < DPMI_LDT_MAX)
            DpmiBuildDescriptor(g_Ldt[ldtIndex].Base, g_Ldt[ldtIndex].Limit,
                            g_Ldt[ldtIndex].Access, g_Ldt[ldtIndex].Flags, &low, &high);
        else
        {
            VDM_REG(tib, VTIB_EFLAGS) |= EFLAGS_CF_U;
            cursor = LogPut(cursor, " -> bad sel");
            break;
        }
        descriptor[0] = low;
        descriptor[1] = high;
        cursor = LogPut(cursor, " sel 0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_EBX));
        cursor = LogPut(cursor, " -> desc 0x"); cursor = LogHex(cursor, low);
        cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, high);
        break; }

    case DPMI_FN_SET_DESCRIPTOR:                               /* set descriptor of sel BX from ES:(E)DI */
    {
        DpmiInt31SetDescriptor(&cursor, tib, &needScan);
        break;
    }

    case DPMI_FN_GET_STATE_SAVE_ADDRESSES:                               /* get state save/restore addresses */
    {
        /* AX = buffer size, BX:CX = real-mode routine, SI:(E)DI = PM routine.
         * Both routines are register-preserving no-ops here (DPMI_SSR_OFF).
         * The size is nominal rather than 0: nothing is written to the
         * buffer, but a client that allocates AX bytes should not be handed
         * a zero-size allocation.
         */
        WORD selector = DpmiHandlerCodeSelector();
        VDM_SET16(tib, VTIB_EAX, DPMI_STATE_SAVE_SIZE);
        VDM_SET16(tib, VTIB_EBX, DOS_HDLR_SEG);
        VDM_SET16(tib, VTIB_ECX, DPMI_SSR_OFF);
        VDM_SET16(tib, VTIB_ESI, selector);
        VDM_REG  (tib, VTIB_EDI) = DPMI_SSR_OFF;
        VDM_REG(tib, VTIB_EFLAGS) &= ~EFLAGS_CF_U;      /* CF=0: always succeeds */
        cursor = LogPut(cursor, " -> saverestore rm=0x"); cursor = LogHex(cursor, DOS_HDLR_SEG);
        cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, DPMI_SSR_OFF);
        cursor = LogPut(cursor, " pm=0x"); cursor = LogHex(cursor, selector);
        cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, DPMI_SSR_OFF);
        cursor = LogPut(cursor, " size=0x40");
        break; }

    case DPMI_FN_GET_RAW_SWITCH_ADDRESSES:                               /* get raw mode switch addresses */
    {
        /* THE CALL DOOM DIES ON. Its code right after this BOP is
         * `73 03 e9 44 02` = jnc +3 / jmp +0x244, so CF=1 sends DOS/4GW
         * straight to its abort path. Returning the two entries is what
         * lets it proceed.
         */
        WORD selector = DpmiHandlerCodeSelector();
        VDM_SET16(tib, VTIB_EBX, DOS_HDLR_SEG);        /* real->prot seg */
        VDM_SET16(tib, VTIB_ECX, DPMI_RAW2PM_OFF);     /* real->prot off */
        VDM_SET16(tib, VTIB_ESI, selector);                 /* prot->real sel */
        VDM_REG  (tib, VTIB_EDI) = DPMI_RAW2RM_OFF;    /* prot->real off */
        VDM_REG(tib, VTIB_EFLAGS) &= ~EFLAGS_CF_U;              /* CF=0 */
        cursor = LogPut(cursor, " -> rawswitch r2p=0x"); cursor = LogHex(cursor, DOS_HDLR_SEG);
        cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, DPMI_RAW2PM_OFF);
        cursor = LogPut(cursor, " p2r=0x"); cursor = LogHex(cursor, selector);
        cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, DPMI_RAW2RM_OFF);
        break; }

    case DPMI_FN_GET_FREE_MEMORY_INFO:                               /* get free memory info -> ES:DI */
    {
        DpmiInt31GetFreeMemoryInfo(&cursor, tib);
        break;
    }

    /* -- 0800: MAP A PHYSICAL ADDRESS INTO THE LINEAR SPACE. (s74)
     * The other half of the VESA linear framebuffer. 4F01 reports
     * PhysBasePtr = VIDEO_VESA_LFB_PHYSICAL; a DPMI client then asks this
     * function to map it and writes pixels straight at the answer.
     * With it unimplemented the whole LFB advertisement was a
     * promise we could not keep, and heaven7 refused all twelve
     * modes rather than call 4F02.
     * We answer with the HOST VA of the VDD's vesa_vram, which is
     * exactly the convention 0501 already uses -- it hands the
     * client a host VA as its linear address, and g_DpmiBlock[]
     * records them as such. So the client's "linear" really is a
     * pointer here, and the picture it draws lands in the buffer
     * the presenter reads.
     *
     * [CAUTION]: ONLY OUR OWN APERTURE. A request for any other physical
     * address is refused rather than identity-mapped: handing a
     * guest a pointer to arbitrary host memory because it asked
     * for a physical address is not a mapping, it is a hole.
     */
    case DPMI_FN_MAP_PHYSICAL_ADDRESS:
    {
        DWORD physical = (VDM_REG16(tib, VTIB_EBX) << WORD_SHIFT)
                 |  VDM_REG16(tib, VTIB_ECX);
        DWORD size = (VDM_REG16(tib, VTIB_ESI) << WORD_SHIFT)
                 |  VDM_REG16(tib, VTIB_EDI);
        cursor = LogPut(cursor, " phys=0x"); cursor = LogHex(cursor, physical);
        cursor = LogPut(cursor, " size=0x"); cursor = LogHex(cursor, size);
        if (physical == VIDEO_VESA_LFB_PHYSICAL && size <= VIDEO_VESA_VRAM)
        {
            DWORD linear = (DWORD)(ULONG_PTR)&g_Video.VesaVram[0];
            VDM_SET16(tib, VTIB_EBX, (WORD)(linear >> WORD_SHIFT));
            VDM_SET16(tib, VTIB_ECX, (WORD)(linear & WORD_MASK));
            VDM_REG(tib, VTIB_EFLAGS) &= ~EFLAGS_CF_U;
            cursor = LogPut(cursor, " -> VESA LFB linear=0x"); cursor = LogHex(cursor, linear);
        }
        else
        {
            VDM_REG(tib, VTIB_EFLAGS) |= EFLAGS_CF_U;
            VDM_SET16(tib, VTIB_EAX, DPMI_E_INVALID_VALUE);   /* invalid value */
            cursor = LogPut(cursor, " -> REFUSED (not our aperture)");
        }
        break; }

    case DPMI_FN_FREE_PHYSICAL_MAPPING:                               /* free physical mapping */
        /* Nothing to undo: the mapping is the VDD's own buffer and it
         * outlives the client. Succeeding is honest; failing would
         * make a tidy client think it had leaked something.
         */
        VDM_REG(tib, VTIB_EFLAGS) &= ~EFLAGS_CF_U;
        cursor = LogPut(cursor, " -> free physical mapping (no-op, buffer is ours)");
        break;

    case DPMI_FN_ALLOCATE_MEMORY:                               /* allocate memory block BX:CX bytes */
    {
        DpmiInt31AllocateMemory(&cursor, base, tib, &needScan);
        break;
    }

    case DPMI_FN_FREE_MEMORY:                               /* free memory block SI:DI = handle */
    {
        DpmiInt31FreeMemory(&cursor, tib);
        break;
    }

    case DPMI_FN_RESIZE_MEMORY:                               /* resize memory block: BX:CX = size, SI:DI = handle */
    {
        DpmiInt31ResizeMemory(&cursor, tib, &needScan);
        break;
    }

    case DPMI_FN_SIMULATE_REAL_MODE_INTERRUPT:                               /* simulate real-mode interrupt: BL=int, ES:DI=RMCS */
    {
        DpmiInt31SimulateRealModeInterrupt(&cursor, tib, machine);
        break;
    }

    case DPMI_FN_CALL_REAL_MODE_IRET:                               /* ...with an IRET frame */

    case DPMI_FN_CALL_REAL_MODE_FAR:                               /* call real-mode FAR proc: ES:DI=RMCS, CX=stack words */
    {
        cursor = DpmiInt31CallRealModeFar(cursor, base, tib, simulatedVector, ax, simulatedCs, simulatedIp, machine);
        break;
    }

    case DPMI_FN_ALLOCATE_CALLBACK:                               /* allocate real-mode callback: DS:SI=handler, ES:DI=RMCS -> CX:DX */
    {
        INT callbackSlot;
        DpmiEnsurePmReturnSelector();   /* lazily install the PM-return catcher selector */
        for (callbackSlot = 0; callbackSlot < DPMI_CB_SLOTS && g_Callbacks[callbackSlot].IsUsed; ++callbackSlot)
        {
        }
        if (callbackSlot >= DPMI_CB_SLOTS || g_PmReturnSelector == 0)
        {
            VDM_REG(tib, VTIB_EFLAGS) |= EFLAGS_CF_U;
            VDM_SET16(tib, VTIB_EAX, DPMI_E_CB_UNAVAIL);
            cursor = LogPut(cursor, " -> cb ENOMEM");
            break;
        }
        g_Callbacks[callbackSlot].IsUsed = 1;
        /* #248: the entry address comes from DpmiCallbackEntry(), the same
         * function 0304h decodes it with.
         */
        /* DS:(E)SI handler and ES:(E)DI RMCS follow the caller's D/B bit (DpmiCallerOffset):
         * a flat 32-bit client's handler offset is its linear address.
         */
        g_Callbacks[callbackSlot].PmSelector = (WORD)VDM_REG(tib, VTIB_DS);
        g_Callbacks[callbackSlot].PmOffset = DpmiCallerOffset(tib, VDM_REG(tib, VTIB_ESI));
        g_Callbacks[callbackSlot].RmEs  = (WORD)VDM_REG(tib, VTIB_ES);
        g_Callbacks[callbackSlot].RmDi  = DpmiCallerOffset(tib, VDM_REG(tib, VTIB_EDI));
        VDM_SET16(tib, VTIB_ECX, DOS_HDLR_SEG);
        VDM_SET16(tib, VTIB_EDX, DpmiCallbackEntry(DPMI_CB_BASE_OFF, callbackSlot));
        cursor = LogPut(cursor, " -> cb slot "); cursor = LogHex(cursor, callbackSlot); cursor = LogPut(cursor, " = 0x");
        cursor = LogHex(cursor, DOS_HDLR_SEG); cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, DpmiCallbackEntry(DPMI_CB_BASE_OFF, callbackSlot));
        cursor = LogPut(cursor, " handler 0x"); cursor = LogHex(cursor, g_Callbacks[callbackSlot].PmSelector); cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, g_Callbacks[callbackSlot].PmOffset);
        break; }

    case DPMI_FN_FREE_CALLBACK:                               /* free real-mode callback CX:DX */
    {
        /* -- #248: DPMI 0.9 CORE, AND IT WAS UNSUP. A callback, once
         * allocated, was held for the life of the client -- with four
         * slots, the fifth allocation of a client that hooks and unhooks
         * failed for good. The address must be EXACTLY one we handed out
         * and still live; anything else is 8024h.
         */
        WORD callbackSegment = (WORD)VDM_REG(tib, VTIB_ECX);
        WORD callbackOffset = (WORD)VDM_REG(tib, VTIB_EDX);
        INT callbackSlot = DpmiCallbackSlotOf(DPMI_CB_BASE_OFF, callbackSegment, DOS_HDLR_SEG, callbackOffset);
        cursor = LogPut(cursor, " cb 0x"); cursor = LogHex(cursor, callbackSegment); cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, callbackOffset);
        if (callbackSlot < 0 || !g_Callbacks[callbackSlot].IsUsed)
        {
            VDM_REG(tib, VTIB_EFLAGS) |= EFLAGS_CF_U;
            VDM_SET16(tib, VTIB_EAX, DPMI_E_INVALID_CB);
            cursor = LogPut(cursor, " -> free REFUSED: not a live callback (8024h)");
            break;
        }
        g_Callbacks[callbackSlot].IsUsed = 0;
        cursor = LogPut(cursor, " -> cb slot "); cursor = LogHex(cursor, callbackSlot); cursor = LogPut(cursor, " freed");
        break; }

    default:
        VDM_REG(tib, VTIB_EFLAGS) |= EFLAGS_CF_U;       /* CF=1: unsupported */
        cursor = LogPut(cursor, " -> UNSUP");
        break;
    }
    cursor = LogPut(cursor, "\r\n"); LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
    if (needScan)
        DpmiScanCodeBlocks();
    VDM_REG(tib, VTIB_EIP) += DPMI_PM_BOP_LENGTH;               /* past the 2-byte INT */
    {
        *cursorIo = cursor;
        *axIo = ax;
        *exitCodeOut = 1;
        return HOST_FLOW_RETURN;
    }
    *cursorIo = cursor;
    *axIo = ax;
    return HOST_FLOW_NEXT;
}

/* INT 2Fh from a PM client, including the DPMI vendor-API entry query. */
static INT DpmiServiceInt2F(
    PSTR *cursorIo,
    PSTR const base,
    const DWORD ax,
    volatile BYTE * const tib,
    DOS_MACHINE * const machine,
    INT *exitCodeOut)
{
    PSTR cursor = *cursorIo;

    /* The PM twin of the V86 BOP2F arm:
     * Answers exactly what the V86 side answers and no more, so
     * the two cannot drift into disagreeing about whether a DPMI
     * host exists depending on which mode asked.
     * Measured against krnl386, which is the only guest that has
     * ever reached here (see the patch list): it queries 168A for
     * the "MS-DOS" vendor API, where AL UNCHANGED means "not
     * supported" (documented). 1689 (the kernel idle call) has no
     * return registers at all. 1684 must return ES:DI = 0:0
     * because it returns a POINTER the caller far-calls.
     */
    cursor = LogPut(cursor, "INT2Fh(PM) AX=0x"); cursor = LogHex(cursor, ax);
    cursor = LogPut(cursor, " BX=0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_EBX));
    if (ax == MULTIPLEX_DPMI_INSTALLATION_CHECK)
    {
        /* A DPMI client asking, from inside protected mode, whether
         * a DPMI host exists. It does -- it is us, and it is already
         * running this guest. Answer identically to the V86 arm.
         */
        VDM_SET16(tib, VTIB_EAX, 0);
        VDM_SET16(tib, VTIB_EBX, 1);
        VDM_SET16(tib, VTIB_ECX, (VDM_REG(tib, VTIB_ECX) & HIGH_BYTE_MASK) | DPMI_CPU_CLASS);
        VDM_SET16(tib, VTIB_EDX, DPMI_VERSION_090);      /* DPMI 0.90 */
        VDM_SET16(tib, VTIB_ESI, 0);
        VDM_SET16(tib, VTIB_ES,  DOS_HDLR_SEG);
        VDM_SET16(tib, VTIB_EDI, DPMI_ENTRY_OFF);
        cursor = LogPut(cursor, " -> DPMI present");
    }
    else if (ax == MULTIPLEX_DEVICE_API_ENTRY)
    {
        VDM_SET16(tib, VTIB_ES, 0);
        VDM_REG(tib, VTIB_EDI) = 0;
        cursor = LogPut(cursor, " -> no device API (ES:DI=0)");
    }
    else if (ax == MULTIPLEX_DPMI_VENDOR_API)
    {
        /* DS:SI names the vendor. Match it rather than answering
         * every caller: the entry we hand back is specific to the
         * "MS-DOS" contract and means nothing to anyone else.
         */
        DWORD dataBase = DpmiSelectorBase((WORD)VDM_REG16(tib, VTIB_DS));
        DWORD sourceOffset = VDM_REG16(tib, VTIB_ESI);
        const volatile BYTE *dataBytes = (const volatile BYTE *)(ULONG_PTR)(dataBase + sourceOffset);
        static const CHAR want[] = "MS-DOS";
        INT item = 0;
        INT same = 1;
        for (item = 0; item < (INT)sizeof want; ++item)
            if (dataBytes[item] != (BYTE)want[item])
            {
                same = 0;
                break;
            }
        cursor = LogPut(cursor, " vendor=[");
        for (item = 0; item < 8 && dataBytes[item] >= 0x20 && dataBytes[item] < 0x7F; ++item)
        {
            CHAR text[2];
            text[0] = (CHAR)dataBytes[item];
            text[1] = 0;
            cursor = LogPut(cursor, text);
        }
        cursor = LogPut(cursor, "]");
        if (same)
        {
            WORD vendorSegment = 0;
            WORD vendorOffset = 0;
            if (WowVendorApiEntry(machine, &vendorSegment, &vendorOffset) == 0)
            {
                VDM_SET16(tib, VTIB_EAX, (WORD)(ax & HIGH_BYTE_MASK));  /* AL=0 */
                VDM_SET16(tib, VTIB_ES, vendorSegment);
                VDM_REG(tib, VTIB_EDI) = vendorOffset;
                cursor = LogPut(cursor, " -> SUPPORTED, entry 0x"); cursor = LogHex(cursor, vendorSegment);
                cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, vendorOffset);
            }
            else
            {
                cursor = LogPut(cursor, " -> wanted, but no memory for the stub");
            }
        }
        else
        {
            cursor = LogPut(cursor, " -> not ours (AL unchanged = no)");
        }
    }
    else
    {
        cursor = LogPut(cursor, " -> untouched (unchanged AX is the 'no' answer)");
    }
    cursor = LogPut(cursor, "\r\n");
    LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
    VDM_REG(tib, VTIB_EIP) += DPMI_PM_BOP_LENGTH;               /* past the BOP */
    {
        *cursorIo = cursor;
        *exitCodeOut = 1;
        return HOST_FLOW_RETURN;
    }
    *cursorIo = cursor;
    return HOST_FLOW_NEXT;
}

/* BIOS INT 15h (system services) from a PM client. */
static INT DpmiServiceInt15(
    PSTR *cursorIo,
    PSTR const base,
    volatile BYTE * const tib,
    const DWORD ax,
    INT *exitCodeOut)
{
    PSTR cursor = *cursorIo;

    /* THE SESSION-36 WALL, AND IT IS A MISSING ARM, NOT A BUG (Importance = 2):
     * A Win16 driver segment runs `b4 c0 / cd 15` -- INT 15h AH=C0h,
     * "get system configuration table" -- in protected mode. Every
     * other BIOS vector krnl386 uses has a PM twin here; 15h did not,
     * so the raw `CD 15` reached the #GP reflect, was correctly
     * identified as a raw INT, was serviced... by a function with no
     * arm for it, and fell through to "unexpected PM stop event=0x4".
     * The run died two bytes into a driver, naming an address rather
     * than a cause.
     *
     * [INFO]: ANSWER EXACTLY WHAT THE V86 ARM ANSWERS -- the same reason the
     * INT 11h twin above shares its constant. These are statements
     * about OUR virtual machine's configuration, and a guest that
     * gets a different machine depending on which mode it asked from
     * is a guest we cannot reason about.
     *   AH=88h -> CMOS_EXTENDED_KB (0x3C00) KB extended, the CMOS figure
     *             (#48: the XMS pool is now that less the HMA).
     *   AH=86h -> wait: the PIT already paces us, so CF=0 and return.
     *   anything else, C0h INCLUDED -> AH=86h, CF=1, "unsupported".
     *
     * [CAUTION]: AH=C0h IS DELIBERATELY REFUSED, not stubbed with a table. The
     * caller checks CF and, without carry, reads the MODEL BYTE at
     * ES:BX+2 (the documented table layout) out of the table we
     * would have to invent. CF=1 sends it
     * down the path a real PC/AT without the call takes; a fabricated
     * table sends it down a path chosen by a number we made up.
     * If a later run shows a driver needs the table, build it from the
     * oracle, not from memory.
     *
     * [CAUTION]: s81 (#54): THE V86 ARM NOW ANSWERS C0h, from the oracle, so the
     * two modes DISAGREE here, knowingly. Answering in PM means handing
     * back a SELECTOR for DOS_CTAB_SEG:DOS_SYSCONF_OFF and sending that
     * Win16 driver down its "model FC" path, which nothing has tested --
     * a change to do deliberately with the Win16 shelf re-run, not in
     * passing. Likewise 87h stays V86-only.
     *
     * [CAUTION]: #253: AND C1h. The V86 arm now hands back the EBDA segment
     * (9FC0h) in ES; in PM that would be a raw paragraph loaded into
     * a selector register, and no PM caller of C1h has been seen.
     * Refused here until one is, and then answered with a selector.
     *
     * [INFO]: #244: C0h AND 87h NOW ANSWER AS IN V86 -- the "two modes
     * disagree knowingly" above is retired for these two:
     *   C0h -> ES:BX = a selector over DOS_CTAB_SEG (built as INT 31h
     *          0002h builds one, DpmiSegmentToDescriptor -- the INT 21h
     *          AH=34h/52h treatment) : DOS_SYSCONF_OFF, AH=0, CF=0.
     *          [CAUTION]: So the session-36 Win16 driver now reads model FCh
     *          and takes its AT path instead of its "no C0h" path.
     *          Unmeasured on the Win16 shelf -- owed a re-run.
     *   87h -> ES:(E)SI is read as a PM pointer to the GDT (selector
     *          base + offset; ESI for a 32-bit client). The 24-bit
     *          addresses INSIDE the descriptors are linear in both
     *          modes, so the copy itself is the V86 one.
     *
     * [CAUTION]: THE DPMI SPEC DOES NOT ASK FOR EITHER. DPMI 0.9 section "Interrupts"
     * reflects INT 15h to real mode WITHOUT translating segment
     * registers -- only INT 21h-style APIs get translation, and that
     * from the extender, not the host -- so a strict host hands back
     * ES:BX as a real-mode segment the client cannot load, and runs
     * 87h with whatever real-mode ES it had. Neither is usable, so a
     * PM caller of either can only have meant the translated form;
     * Windows' DPMI host is said to translate C0h (that is what the
     * Win16 driver's `es:[bx+2]` read assumes) -- not measured here.
     */
    { DWORD int15Ah = (VDM_REG(tib, VTIB_EAX) >> BYTE_SHIFT) & BYTE_MASK;
      if (int15Ah == BIOS_SYSTEM_EXTENDED_MEMORY)
      {
          VDM_SET16(tib, VTIB_EAX, (WORD)CMOS_EXTENDED_KB);
          VDM_REG(tib, VTIB_EFLAGS) &= ~EFLAGS_CF_U;
      }
      else if (int15Ah == BIOS_SYSTEM_GET_CONFIGURATION)
      {
          WORD controlTableSelector = DpmiSegmentToDescriptor(DOS_CTAB_SEG);
          if (controlTableSelector)
          {
              VDM_SET16(tib, VTIB_ES, controlTableSelector);
              VDM_SET16(tib, VTIB_EBX, DOS_SYSCONF_OFF);
              VDM_SET16(tib, VTIB_EAX, (WORD)(VDM_REG(tib, VTIB_EAX) & BYTE_MASK));
              VDM_REG(tib, VTIB_EFLAGS) &= ~EFLAGS_CF_U;
          }
          else                     /* no descriptor: say "unsupported" */
          {
              VDM_SET16(tib, VTIB_EAX,
                        (WORD)((VDM_REG(tib, VTIB_EAX) & BYTE_MASK) | (BIOS_SYSTEM_STATUS_UNSUPPORTED << BYTE_SHIFT)));
              VDM_REG(tib, VTIB_EFLAGS) |= EFLAGS_CF_U;
          }
      }
      else if (int15Ah == BIOS_SYSTEM_MOVE_BLOCK)
      {
          WORD  es87 = (WORD)VDM_REG16(tib, VTIB_ES);
          DWORD offset  = DpmiSelectorIs32((WORD)VDM_REG16(tib, VTIB_CS))
                       ? VDM_REG(tib, VTIB_ESI) : VDM_REG16(tib, VTIB_ESI);
          DWORD guestLinear   = DpmiSelectorBase(es87) + offset;
          UINT moveBlockResult = Int15MoveBlockAt(tib,
              (es87 && HostReadable((const VOID *)(ULONG_PTR)guestLinear, 0x30)) ? guestLinear : 0);
          VDM_SET16(tib, VTIB_EAX, (WORD)((moveBlockResult << BYTE_SHIFT) | (VDM_REG(tib, VTIB_EAX) & BYTE_MASK)));
          /* AT BIOS: success is AH=0 with CF=0 AND ZF=1 (as the V86 arm) */
          if (moveBlockResult)
              VDM_REG(tib, VTIB_EFLAGS) = (VDM_REG(tib, VTIB_EFLAGS) | EFLAGS_CF_U) & ~EFLAGS_ZF_U;
          else
              VDM_REG(tib, VTIB_EFLAGS) = (VDM_REG(tib, VTIB_EFLAGS) & ~EFLAGS_CF_U) | EFLAGS_ZF_U;
      }
      else if (int15Ah == BIOS_SYSTEM_WAIT)
      {
          /* #256: WAIT CX:DX MICROSECONDS, AS THE V86 ARM DOES (#206):
           * This answered CF=0 at once ("the PIT already paces us"), so a
           * DPMI client delaying with it waited zero.
           * - From the top-level PM loop the BOP is RE-EXECUTED until the
           *   deadline (EIP left on it), so that loop keeps advancing the
           *   BIOS tick and delivering IRQ0 to a hooked client while it
           *   waits -- what a real BIOS's wait does with IF=1.
           * - Anywhere else (inside an injected ISR, a callback, a nested
           *   dispatch) a re-execution would spend that loop's bounded
           *   phases and could ABANDON the handler, so the wait is served
           *   here, blocking, with the watchdog fed. Interrupts are held
           *   until it ends -- the INT 16h PM read's shape.
           */
          DWORD microseconds = (VDM_REG16(tib, VTIB_ECX) << WORD_SHIFT)
                   | VDM_REG16(tib, VTIB_EDX);
          LARGE_INTEGER waitNow;
          LARGE_INTEGER waitFrequency;
          QueryPerformanceCounter(&waitNow);
          QueryPerformanceFrequency(&waitFrequency);
          if (g_Int15EventEnd)                     /* an 83h countdown runs */
          {
              VDM_SET16(tib, VTIB_EAX,
                        (WORD)((VDM_REG(tib, VTIB_EAX) & BYTE_MASK) | (BIOS_SYSTEM_STATUS_BUSY << BYTE_SHIFT)));
              VDM_REG(tib, VTIB_EFLAGS) |= EFLAGS_CF_U;
              ++g_Int15Busy;
          }
          else if (g_PmDispatchTop)
          {
              if (!g_Int15WaitEnd)
              {
                  if (microseconds)
                  {
                      g_Int15WaitEnd = Int15QpcAfterMicroseconds(microseconds);
                      ++g_Int15Waits;
                  }
              }
              else if (waitNow.QuadPart >= g_Int15WaitEnd)
                  g_Int15WaitEnd = 0;
              if (g_Int15WaitEnd)                /* not yet: run it again */
              {
                  if ((g_Int15WaitEnd - waitNow.QuadPart) * MILLISECONDS_PER_SECOND > INT15_WAIT_SLEEP_MS * waitFrequency.QuadPart)
                      Sleep(1);
                  { /* EIP stays on the BOP */
                      *cursorIo = cursor;
                      *exitCodeOut = 1;
                      return HOST_FLOW_RETURN;
                  }
              }
              VDM_REG(tib, VTIB_EFLAGS) &= ~EFLAGS_CF_U;
          }
          else
          {
              LONGLONG end = Int15QpcAfterMicroseconds(microseconds);
              if (microseconds)
                  ++g_Int15Waits;
              for (;;)
              {
                  QueryPerformanceCounter(&waitNow);
                  if (waitNow.QuadPart >= end || !g_Running)
                      break;
                  InterlockedIncrement(&g_DpmiIteration);
                  if ((end - waitNow.QuadPart) * MILLISECONDS_PER_SECOND > INT15_WAIT_SLEEP_MS * waitFrequency.QuadPart)
                      Sleep(1);
                  else
                      Sleep(0);
              }
              VDM_REG(tib, VTIB_EFLAGS) &= ~EFLAGS_CF_U;
          }
      }
      else
      {
          VDM_SET16(tib, VTIB_EAX,
                    (WORD)((VDM_REG(tib, VTIB_EAX) & BYTE_MASK) | (BIOS_SYSTEM_STATUS_UNSUPPORTED << BYTE_SHIFT)));
          VDM_REG(tib, VTIB_EFLAGS) |= EFLAGS_CF_U;
          g_BiosUnimplemented[VECTOR_SYSTEM] = 1;
      }
      /* Log the REQUEST as it arrived, which means capturing AX before
       * the arms above overwrite AH -- the V86 twin shipped with that
       * exact defect and printed its own write-back as the guest's
       * request. `ax` was read at function entry, so it is already the
       * guest's; use it and not VTIB_EAX.
       */
      cursor = LogPut(cursor, "INT15h(PM) ax=0x"); cursor = LogHex(cursor, ax);
      cursor = LogPut(cursor, " -> ax=0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_EAX));
      cursor = LogPut(cursor, " cf="); cursor = LogHex(cursor, VDM_REG(tib, VTIB_EFLAGS) & EFLAGS_CF_U);
      cursor = LogPut(cursor, "\r\n");
      LogAppend(LOG_PATH, base, cursor);
      SerialOut(base, cursor);
      cursor = base; }
    VDM_REG(tib, VTIB_EIP) += DPMI_PM_BOP_LENGTH;
    {
        *cursorIo = cursor;
        *exitCodeOut = 1;
        return HOST_FLOW_RETURN;
    }
    *cursorIo = cursor;
    return HOST_FLOW_NEXT;
}

/* A guest breakpoint (DPMI_BP_VEC) was hit: report it and disarm it. */
static INT DpmiServiceBreakpoint(
    PSTR *cursorIo,
    PSTR const base,
    volatile BYTE * const tib,
    const DWORD eip,
    const UINT steps,
    INT *exitCodeOut)
{
    PSTR cursor = *cursorIo;
    /* Report EVERYTHING -- the whole point is that the run may not
     * survive to the next line. Then restore the displaced bytes and
     * leave EIP WHERE IT IS, so the real instruction runs next and the
     * client carries on as if we had never been here.
     */
    DWORD cs = VDM_REG16(tib, VTIB_CS);
    DWORD codeBase = DpmiSelectorBase((WORD)cs);
    DWORD linear = codeBase + eip;
    WORD  ss  = (WORD)VDM_REG16(tib, VTIB_SS);
    DWORD stackBase  = DpmiSelectorBase(ss);
    DWORD stackPointer  = DpmiSelectorIs32(ss) ? VDM_REG(tib, VTIB_ESP)
                                  : VDM_REG16(tib, VTIB_ESP);

    cursor = LogPut(cursor, "DPMI-BP HIT linear 0x"); cursor = LogHex(cursor, linear);
    cursor = LogPut(cursor, " cs:eip=0x"); cursor = LogHex(cursor, cs);
    cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, eip);
    cursor = LogPut(cursor, " after "); cursor = LogHex(cursor, steps); cursor = LogPut(cursor, " svc");
    /* - A CLOCK, NOT JUST A SERVICE COUNT. Session 20 could bracket the
     * R_ExecuteSetViewSize death in INSTRUCTIONS but not in TIME, and
     * those two answers point at different culprits: a threshold in
     * instructions is the guest doing something illegal, a threshold in
     * milliseconds is the kernel's timer. `svc` counts host services,
     * which stop entirely inside a BOP-free stretch -- so it is exactly
     * the wrong unit for the question. GetTickCount is free.
     */
    cursor = LogPut(cursor, " ms="); cursor = LogHex(cursor, GetTickCount());
    cursor = LogPut(cursor, " EAX=0x"); cursor = LogHex(cursor, VDM_REG(tib, VTIB_EAX));
    cursor = LogPut(cursor, " EBX=0x"); cursor = LogHex(cursor, VDM_REG(tib, VTIB_EBX));
    cursor = LogPut(cursor, " ECX=0x"); cursor = LogHex(cursor, VDM_REG(tib, VTIB_ECX));
    cursor = LogPut(cursor, " EDX=0x"); cursor = LogHex(cursor, VDM_REG(tib, VTIB_EDX));
    cursor = LogPut(cursor, " ESI=0x"); cursor = LogHex(cursor, VDM_REG(tib, VTIB_ESI));
    cursor = LogPut(cursor, " EDI=0x"); cursor = LogHex(cursor, VDM_REG(tib, VTIB_EDI));
    cursor = LogPut(cursor, " EBP=0x"); cursor = LogHex(cursor, VDM_REG(tib, VTIB_EBP));
    cursor = LogPut(cursor, " DS=0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_DS));
    cursor = LogPut(cursor, " ES=0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_ES));
    cursor = LogPut(cursor, " SS:SP=0x"); cursor = LogHex(cursor, ss); cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, stackPointer);
    cursor = LogPut(cursor, " efl=0x"); cursor = LogHex(cursor, VDM_REG(tib, VTIB_EFLAGS));
    cursor = LogPut(cursor, " stack=");
    /* 64 bytes, not 16: when a breakpoint sits inside a leaf routine the
     * question is almost always "who called this", and one frame is never
     * enough -- the whole return CHAIN is what names the decision.
     */
    { const BYTE *stackDump = (const BYTE *)(ULONG_PTR)(stackBase + stackPointer);
      if (!HostReadable(stackDump, 64))
          cursor = LogPut(cursor, "<unreadable>");
      else
          cursor = LogDump(cursor, stackDump, 64); }
    /* [INFO]: RESOLVE THE SELECTORS, and show what the instruction is
     * about to touch. pmbp.txt's dump column takes a fixed
     * LINEAR address, which cannot follow a segment register --
     * so answering "what is at ES:DI" meant computing the base
     * by hand from an older log line and hoping it still held.
     * It did not: two runs were spent dumping linear 0x1100 on
     * the strength of an `04F2 installed base=0x1100` line, and
     * the bytes there were nothing to do with ES.
     * A debugger that makes you guess where a selector points is
     * most of the way to being no debugger.
     */
    {   WORD dsValue = (WORD)VDM_REG16(tib, VTIB_DS);
        WORD esValue = (WORD)VDM_REG16(tib, VTIB_ES);
        DWORD dsBase = DpmiSelectorBase(dsValue);
        DWORD esBase = DpmiSelectorBase(esValue);
        DWORD siValue = VDM_REG16(tib, VTIB_ESI);
        DWORD diValue = VDM_REG16(tib, VTIB_EDI);
        cursor = LogPut(cursor, "\r\n  dsbase=0x"); cursor = LogHex(cursor, dsBase);
        cursor = LogPut(cursor, " esbase=0x");      cursor = LogHex(cursor, esBase);
        if (dsBase && MemoryReadable((ULONG_PTR)(dsBase + siValue), 32))
        {
            cursor = LogPut(cursor, "\r\n  @ds:si=");
            cursor = LogDump(cursor, (const BYTE *)(ULONG_PTR)(dsBase + siValue), 32);
        }
        if (esBase && MemoryReadable((ULONG_PTR)(esBase + diValue), 32))
        {
            cursor = LogPut(cursor, "\r\n  @es:di=");
            cursor = LogDump(cursor, (const BYTE *)(ULONG_PTR)(esBase + diValue), 32);
        }
        /* [INFO]: AND THE OTHER TWO PAIRINGS. ds:si/es:di is the string-move
         * convention, and it is the wrong pair for the code this is
         * pointed at: a routine that walks a structure commonly
         * addresses it through es:si, and the krnl386 walk this
         * was aimed at did (its ES:SI pointed into the segment
         * table at the hit). Dumping only ds:si and es:di answered a question
         * nobody had while hiding the one on the screen.
         */
        if (esBase && MemoryReadable((ULONG_PTR)(esBase + siValue), 32))
        {
            cursor = LogPut(cursor, "\r\n  @es:si=");
            cursor = LogDump(cursor, (const BYTE *)(ULONG_PTR)(esBase + siValue), 32);
        }
        if (dsBase && MemoryReadable((ULONG_PTR)(dsBase + diValue), 32))
        {
            cursor = LogPut(cursor, "\r\n  @ds:di=");
            cursor = LogDump(cursor, (const BYTE *)(ULONG_PTR)(dsBase + diValue), 32);
        }
    }
    { INT breakpoint = DpmiBreakpointDisarm(linear);
      /* [INFO]: EVERY hit leaves the guest standing on the footprint we
       * just restored, so EVERY hit is pending -- not only the
       * repeating ones. A one-shot is additionally retired, so
       * that clearing `pending` does not silently turn it into a
       * repeating breakpoint. See g_BreakpointDone for what this cost.
       */
      if (breakpoint >= 0)
      {
          g_BreakpointPending[breakpoint] = 1;
          /* Skip mode implies repeating -- its own note below says a
           * skipped site is a loop or a shared wrapper, so retiring
           * it after one pass would defeat the point.
           */
          if (!g_BreakpointReport[breakpoint] && !g_BreakpointSkip[breakpoint])
              g_BreakpointDone[breakpoint] = 1;
      }
      if (breakpoint >= 0 && g_BreakpointSkip[breakpoint])
      {
          /* SKIP MODE: step over the instruction entirely. The bytes are
           * already restored, so advancing EIP lands on whatever follows
           * the skipped instruction -- the caller states its length.
           */
          VDM_REG(tib, VTIB_EIP) += g_BreakpointSkip[breakpoint];
          cursor = LogPut(cursor, " [SKIPPED "); cursor = LogHex(cursor, g_BreakpointSkip[breakpoint]);
          cursor = LogPut(cursor, " byte(s)]");
          /* A skipped site is usually in a loop or a shared wrapper, so
           * one-shot is useless there. We cannot re-plant NOW -- EIP is
           * inside the two-byte footprint we just restored -- so mark it
           * and let DpmiBreakpointRearmPending() do it once the guest has
           * moved on.
           */
          g_BreakpointPending[breakpoint] = 1;
      }
      if (breakpoint < 0)
          cursor = LogPut(cursor, " [WARN: no BP record here]");
      else if (g_BreakpointDump[breakpoint])
      {
          /* [INFO]: mode bit 2 (4): the dump column is an offset from DS's
           * BASE, not a linear address. The variables worth watching
           * in krnl386 are DGROUP variables -- its arena bookkeeping
           * lives at [0x59e], [0x5a4], [0x5a6], [0x148e] -- and
           * DGROUP's base moves between runs, so a linear dump column
           * means computing it by hand from an older log line and
           * hoping. Two runs were lost to exactly that (see the
           * selector-resolution note above); this closes the same gap
           * for the dump column.
           */
          DWORD dumpAddress = g_BreakpointDump[breakpoint];
          const BYTE *dumpBytes;
          if (g_BreakpointMode[breakpoint] & BREAKPOINT_MODE_DUMP_DS)
          {
              DWORD dataSegmentBase = DpmiSelectorBase((WORD)(VDM_REG(tib, VTIB_DS)
                                                & WORD_MASK));
              cursor = LogPut(cursor, "\r\n  dump@ds:0x"); cursor = LogHex(cursor, dumpAddress);
              dumpAddress += dataSegmentBase;
              cursor = LogPut(cursor, " (linear 0x"); cursor = LogHex(cursor, dumpAddress); cursor = LogPut(cursor, ")");
          }
          else
          {
              cursor = LogPut(cursor, "\r\n  dump@0x"); cursor = LogHex(cursor, dumpAddress);
          }
          dumpBytes = (const BYTE *)(ULONG_PTR)dumpAddress;
          cursor = LogPut(cursor, "=");
          if (!HostReadable(dumpBytes, 64))
              cursor = LogPut(cursor, "<unreadable from host>");
          else
              cursor = LogDump(cursor, dumpBytes, 64);
      } }
    cursor = LogPut(cursor, "\r\n");
    LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
    { /* EIP unchanged on purpose */
        *cursorIo = cursor;
        *exitCodeOut = 1;
        return HOST_FLOW_RETURN;
    }
    *cursorIo = cursor;
    return HOST_FLOW_NEXT;
}

/* INT 21h from a 32-bit client: AH=25h/35h on a hardware-IRQ vector is answered here, ahead of the general path. */
static INT DpmiServiceClient32Int21(
    PSTR *cursorIo,
    PSTR const base,
    const DWORD ax,
    volatile BYTE * const tib,
    INT *exitCodeOut)
{
    PSTR cursor = *cursorIo;
    DWORD requestAh = (ax >> BYTE_SHIFT) & BYTE_MASK;
    DWORD vectorNumber = ax & BYTE_MASK;

    if ((requestAh == DOS_FN_SET_VECTOR || requestAh == DOS_FN_GET_VECTOR) && vectorNumber >= VECTOR_IRQ0 && vectorNumber <= VECTOR_IRQ7)
    {
        if (requestAh == DOS_FN_SET_VECTOR)
        {
            WORD handlerSegment = (WORD)VDM_REG16(tib, VTIB_DS);
            g_PmInt[vectorNumber].Selector = handlerSegment;
            g_PmInt[vectorNumber].Offset = DpmiSelectorIs32(handlerSegment) ? VDM_REG(tib, VTIB_EDX)
                                                   : VDM_REG16(tib, VTIB_EDX);
            g_PmInt[vectorNumber].Client = 1;
            if (vectorNumber == VECTOR_TIMER) { g_PmAppHookedTimer = 1;
                                g_PmAppTimerSelector = handlerSegment;
                                g_PmAppTimerOffset = g_PmInt[vectorNumber].Offset;
                                g_PmVector8ArmedMs = GetTickCount(); }
            cursor = LogPut(cursor, "PM INT 21h AH=25 (host-owned IRQ vector) 0x");
            cursor = LogHex(cursor, vectorNumber); cursor = LogPut(cursor, " = 0x"); cursor = LogHex(cursor, handlerSegment);
            cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, g_PmInt[vectorNumber].Offset);
        }
        else
        {
            /* WHAT THE CALLER CHAINS TO MUST BE SAFE TO CHAIN TO:
             * A game saves the "old" handler here and far-jumps to it
             * periodically -- Doom's timer ISR passes the tick down
             * every Nth interrupt to keep the BIOS clock. If we hand
             * back the extender's arming-pass stub, that chain lands in
             * DOS/4GW's dispatcher, entered from a hardware interrupt
             * WE synthesised rather than through its own IDT, on a
             * stack it did not switch: measured, that kills the VDM
             * outright, with no fault and no log. It is why delivery
             * died after a handful of ticks however it was arranged --
             * the deaths were never the injections, they were the Nth
             * one, when the ISR chained.
             * Once we own the line, the far end of the chain is OURS:
             * hand back the host's own default stub for the vector
             * (BOP; IRET), which accepts the chain and returns.
             */
            WORD  previousSelector = g_PmInt[vectorNumber].Selector;
            DWORD previousOffset = g_PmInt[vectorNumber].Offset;
            if (!DpmiSelectorIs32(previousSelector) && g_PmDefaultSelector)
            {
                previousSelector = g_PmDefaultSelector;
                previousOffset = (DWORD)vectorNumber * DPMI_PMDEF_STRIDE;
            }
            VDM_SET16(tib, VTIB_ES, previousSelector);
            VDM_REG(tib, VTIB_EBX) = previousOffset;                 /* client is 32-bit */
            cursor = LogPut(cursor, "PM INT 21h AH=35 (host-owned IRQ vector) 0x");
            cursor = LogHex(cursor, vectorNumber); cursor = LogPut(cursor, " -> 0x"); cursor = LogHex(cursor, previousSelector);
            cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, previousOffset);
        }
        VDM_REG(tib, VTIB_EFLAGS) &= ~EFLAGS_CF_U;          /* success */
        cursor = LogPut(cursor, "\r\n");
        LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
        VDM_REG(tib, VTIB_EIP) += DPMI_PM_BOP_LENGTH;
        {
            *cursorIo = cursor;
            *exitCodeOut = 1;
            return HOST_FLOW_RETURN;
        }
    }
    *cursorIo = cursor;
    return HOST_FLOW_NEXT;
}

/* A BOP the client hit in PM that is not a callback's return: the WOW32 thunks -- a performance line every 1024 BOPs, the frame's log, then Wow32ServiceBop -- and the dispatch-pointer BOP. */
static INT DpmiServiceWowBop(
    PSTR *cursorIo,
    PSTR const base,
    const volatile BYTE * const bopBytes,
    volatile BYTE * const tib,
    const DWORD eip,
    SIZE_T const reportSize,
    DOS_MACHINE * const machine,
    INT *exitCodeOut)
{
    PSTR cursor = *cursorIo;
    BYTE bopCode = bopBytes[VDM_BOP_NUMBER_OFFSET];
    BYTE bopSubcode = bopBytes[VDM_BOP_LENGTH];
    /* What a stepped-over 0x51 will hand back -- see the step-over note below.
     * Captured while the frame is still in scope. A separate flag, because
     * every 32-bit value is a possible hole value (0xFFFF is DECLINE) and a
     * sentinel that collides with real data is how an instrument starts lying.
     */
    DWORD wowStale = 0;
    INT wowStaleOk = 0;
    DWORD wowAnswer = (DWORD)WOW32_UNIMPL_RET;

    /* [INFO]: WATCH PSP+0x2c ACROSS EVERY WOW32 CALL, AND REPORT ONLY CHANGES.
     * Knowing the field ends up wrong is not knowing who wrote it. Sampling
     * it here names the call the change happened across, which is as close
     * to the writer as a host-side instrument gets without single-stepping
     * -- and it is the same move `pmchg.txt` makes for module segments,
     * which cannot reach a selector krnl386 allocated at run time.
     */
    cursor = WowPspEnvironmentCheck(cursor, "a WOW32 BOP");
    /* WHERE THE TIME ACTUALLY GOES, EVERY 4096 BOPs (Importance = 3):
     * The user has reported the Win16 guests as slow twice, and both
     * times the cause was GUESSED and both guesses were wrong (the
     * file trace -- refuted by a wowquiet.txt A/B; the serial port --
     * refuted by `mode`, there is no COM1). So this stops guessing and
     * prints the apportionment: how many BOPs, over how long, and how
     * much of that was spent writing the log and pumping Win32.
     *
     * [CAUTION]: ONE LINE PER 4096 BOPs, so the instrument cannot become the
     * thing it is measuring -- which is the exact trap it exists to
     * investigate.
     */
    if ((++g_WowBops & 0x3FF) == 0)
    {
        DWORD now = GetTickCount();
        LARGE_INTEGER qpcFrequency;
        QueryPerformanceFrequency(&qpcFrequency);
        cursor = LogPut(cursor, "WOWPERF: bops=0x");      cursor = LogHex(cursor, g_WowBops);
        cursor = LogPut(cursor, " ms_since_last=0x");     cursor = LogHex(cursor, now - g_WowPerfMs);
        cursor = LogPut(cursor, " log_calls=0x");         cursor = LogHex(cursor, g_LogCalls);
        cursor = LogPut(cursor, " log_kb=0x");            cursor = LogHex(cursor, g_LogBytes >> 10);
        cursor = LogPut(cursor, " log_ms=0x");
        cursor = LogHex(cursor, qpcFrequency.QuadPart ? (DWORD)(g_LogQpc * MILLISECONDS_PER_SECOND / qpcFrequency.QuadPart) : 0);
        cursor = LogPut(cursor, " pump_calls=0x");        cursor = LogHex(cursor, g_WowWinPumpCalls);
        cursor = LogPut(cursor, " pump_ms=0x");
        cursor = LogHex(cursor, qpcFrequency.QuadPart ? (DWORD)(g_WowWinPumpTicks * MILLISECONDS_PER_SECOND / qpcFrequency.QuadPart) : 0);
        /* [INFO]: AND HOW MUCH THE FOLD SAVED, because a suppression that is
         * not counted is indistinguishable from a call that never
         * happened -- which is the whole reason this line exists.
         */
        cursor = LogPut(cursor, " folded=0x");            cursor = LogHex(cursor, g_WowFoldDropped);
        cursor = LogPut(cursor, "\r\n");
        g_WowPerfMs = now;
    }
    /* STOP WRITING A KILOBYTE PER BOP. (session 56) (Importance = 4):
     * The note on g_LogIsQuiet in log.h prescribes exactly this: the
     * answer to "is the trace the bottleneck" is not to ship the
     * silencer on, it is to stop producing the volume. TERMINAL made
     * the case unanswerable -- 155,626 GetCurrentTime calls in a
     * twenty-second run and a 158 MB log (s55 recorded 182 MB for the
     * same guest). A grep over it TIMED OUT, which is the point at
     * which an instrument has stopped being one.
     *
     * [INFO]: THE CLOCK IS NOT THE BUG, and that was checked before any of
     * this was written: GetCurrentTime returned 321 DISTINCT values
     * stepping ~16 ms, so it advances correctly and the guest is
     * polling it ~485 times per tick. A delay loop is entitled to do
     * that. What is not entitled is us minuting every iteration.
     *
     * [CAUTION]: AND THE FIRST DESIGN WAS THE WRONG SHAPE, MEASURED. It folded
     * RUNS of the same call back to back, which sounds like the same
     * thing and is not: the longest identical run in that log is
     * TWENTY-THREE. The 155,626 calls are interleaved into a repeating
     * CYCLE, so a run-based fold never fired once (`folded=0x0`) and
     * the log came back BIGGER. The pattern is "one function called an
     * enormous number of times", not "one function repeated".
     *
     * So cap PER FUNCTION. The first WOWFOLD_KEEP calls of each id are
     * traced in full -- which is where the arguments worth reading
     * are -- and after that the block's DUMP is dropped while the
     * verdict line is kept. Interleaving cannot defeat it.
     */
    {   DWORD frameStackBase  = DpmiSelectorBase((WORD)VDM_REG16(tib, VTIB_SS));
        DWORD frameBp  = VDM_REG16(tib, VTIB_EBP);
        g_WowFoldMute = WOWFOLD_MUTE_NONE;
        if (bopCode == WOW32_BOP && frameStackBase
            && HostReadable((const VOID *)(ULONG_PTR)(frameStackBase + frameBp), 16))
        {
            const volatile BYTE *fromFrame =
                (const volatile BYTE *)(ULONG_PTR)(frameStackBase + frameBp);
            DWORD fromFrameId = (DWORD)(fromFrame[WOW32_OFF_ID] | (fromFrame[WOW32_OFF_ID + 1] << BYTE_SHIFT));
            /* [CAUTION]: INDEXED BY ID ALONE, so two modules' ids can share a
             * slot. That is acceptable HERE and nowhere else: the id
             * space being per-module is a hard rule for DISPATCH (it
             * cost a session to learn) but this table only decides how
             * much to PRINT, and a collision can only make one
             * function's dump stop early. It can never change what is
             * serviced, and the verdict line is kept either way.
             */
            DWORD slot = fromFrameId & (WOWFOLD_SLOTS - 1u);
            if (g_WowFoldSeen[slot] < MAXDWORD)
                ++g_WowFoldSeen[slot];
            if (g_WowFoldSeen[slot] == WOWFOLD_KEEP)
            {
                cursor = LogPut(cursor, "WOWFOLD: FUNC=0x"); cursor = LogHex(cursor, fromFrameId);
                cursor = LogPut(cursor, " has now been traced 0x");
                cursor = LogHex(cursor, (DWORD)WOWFOLD_KEEP);
                cursor = LogPut(cursor, " times; from here its REGISTER AND STACK DUMP"
                            " is dropped and only the verdict line is kept."
                            " Nothing about what is serviced changes.\r\n");
                LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
            }
            if (g_WowFoldSeen[slot] == WOWFOLD_HARD)
            {
                cursor = LogPut(cursor, "WOWFOLD: FUNC=0x"); cursor = LogHex(cursor, fromFrameId);
                cursor = LogPut(cursor, " has now been seen 0x"); cursor = LogHex(cursor, (DWORD)WOWFOLD_HARD);
                cursor = LogPut(cursor, " times; from here it is COUNTED AND NOT TRACED AT"
                            " ALL. Line counts taken from this log past this"
                            " point are a floor, not a total.\r\n");
                LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
            }
            if (g_WowFoldSeen[slot] > WOWFOLD_KEEP)
                g_WowFoldMute = WOWFOLD_MUTE_FOLD;
            if (g_WowFoldSeen[slot] > WOWFOLD_HARD)
                g_WowFoldMute = WOWFOLD_MUTE_DROP;
        }
    }
    cursor = LogPut(cursor, "WOWBOP 0x"); cursor = LogHexByte(cursor, bopCode);
    /* Only 0x53 carries a sub-function byte. Printing bb[3] for the others
     * shows the first byte of the NEXT instruction and reads like data.
     */
    if (bopCode == 0x53)
    {
        cursor = LogPut(cursor, " sub=0x");
        cursor = LogHexByte(cursor, bopSubcode);
    }
    cursor = LogPut(cursor, " at 0x");    cursor = LogHex(cursor, eip);
    /* ASK pmap FIRST, BECAUSE IT ANSWERS FOR EVERY GUEST (Importance = 2):
     * The file-image check below is the WOW one and needs g_WowModuleCount, so for a
     * DOS or DPMI guest -- ZAR, GH #23 -- it cannot run at all, and the question
     * "is this BOP OURS or the guest's own bytes" was left to inference twice.
     * pmap is keyed by LINEAR ADDRESS and is exactly the record of what we
     * rewrote, so one lookup settles it whatever the guest is. Print the address
     * too: a BOP at an EIP with no pmap entry is the guest's own `C4 C4`, and
     * that is a completely different bug from a patched INT whose vector we lost.
     */
    {   DWORD currentCodeBase = DpmiSelectorBase((WORD)VDM_REG16(tib, VTIB_CS));
        DWORD lin3 = currentCodeBase + eip;
        BYTE  patchMark3 = PatchMapGet(lin3);
        cursor = LogPut(cursor, " cs=0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_CS));
        cursor = LogPut(cursor, " fltsel=0x"); cursor = LogHex(cursor, g_DpmiFaultCodeSelector);
        cursor = LogPut(cursor, " lin=0x"); cursor = LogHex(cursor, lin3);
        if (patchMark3)
        {
            cursor = LogPut(cursor, " [pmap: ★ THIS IS A SITE WE PATCHED, it was INT 0x");
            cursor = LogHexByte(cursor, patchMark3); cursor = LogPut(cursor, "]");
        }
        else
        {
            cursor = LogPut(cursor, " [pmap: not ours -- the guest's own C4 C4]");
        }
    }
    /* IS THIS ACTUALLY A PATCHED `INT nn` WHOSE VECTOR WE LOST? (Importance = 1):
     * Our patcher rewrites `CD nn` (two bytes) as `C4 C4` (two bytes) and keeps
     * the vector in a side map keyed by LINEAR ADDRESS. A BOP whose code byte we
     * do not recognise is therefore ambiguous: it may be a real native BOP, or it
     * may be a patched INT site read one byte too far -- bb[2] is then the NEXT
     * instruction's first byte. One site reported "BOP 0x1f" where the file has
     * `cd 21`: an INT 21h, and the 0x1f merely the next opcode byte.
     * So ask the module's own file image what those bytes are. It is the one
     * source that cannot have been rewritten, and it turns "unimplemented BOP
     * 0x1f" into "a swallowed INT 21h", which is a different bug entirely.
     */
    if (g_WowModuleCount && g_WowImage[0])
    {
        INT moduleSegment;
        DWORD codeSegmentBase = DpmiSelectorBase((WORD)VDM_REG16(tib, VTIB_CS));
        for (moduleSegment = 0; moduleSegment < (INT)g_WowModule[0].SegmentCount; ++moduleSegment)
        {
            NE_SEGMENT *neSegment = &g_WowModule[0].Segments[moduleSegment];
            if (!neSegment->Sector || eip + 1 >= neSegment->Length)
                continue;
            /* Only the segment this CS actually is: match the PM copy's base
             * against the code selector we recognised for segment 1, and the
             * V86 paragraph for the rest.
             */
            if (moduleSegment == 0 && codeSegmentBase != g_WowPmSegment1Base &&
                codeSegmentBase != (DWORD)neSegment->Selector << PARAGRAPH_SHIFT)
                continue;
            if (moduleSegment != 0 && codeSegmentBase != (DWORD)neSegment->Selector << PARAGRAPH_SHIFT)
                continue;
            {   const BYTE *fileBytes = g_WowImage[0] + neSegment->FileOffset + eip;
                cursor = LogPut(cursor, " [file seg"); cursor = LogHex(cursor, (DWORD)(moduleSegment + 1));
                cursor = LogPut(cursor, "+0x"); cursor = LogHex(cursor, eip);
                cursor = LogPut(cursor, " = "); cursor = LogHexByte(cursor, fileBytes[0]);
                cursor = LogPut(cursor, " "); cursor = LogHexByte(cursor, fileBytes[1]);
                if (fileBytes[0] == X86_OP_INT)
                    cursor = LogPut(cursor, " -- ★ A PATCHED INT, NOT A BOP; vector lost");
                cursor = LogPut(cursor, "]");
            }
            break;
        }
    }
    /* WHICH 32-BIT CALL IS THIS?:
     * 0x51 is not a service, it is the generic 16->32 GATEWAY: every thunked
     * call arrives through it with the caller's registers saved below the
     * frame at SS:BP (observed). 0x56 is the same idea inline -- its
     * arguments are on the stack too.
     * So the useful question is never "implement 0x51", it is "WHICH function
     * is being asked for", and that is on the guest stack. Dump it. This
     * turns "implement wow32.dll" into a short, specific list.
     */
    {   DWORD stackSegmentBase = DpmiSelectorBase((WORD)VDM_REG16(tib, VTIB_SS));
        DWORD sp16 = VDM_REG16(tib, VTIB_ESP);
        const volatile BYTE *stack = (const volatile BYTE *)(ULONG_PTR)(stackSegmentBase + sp16);
        INT word;
        cursor = LogPut(cursor, " ax=0x");  cursor = LogHex(cursor, VDM_REG16(tib, VTIB_EAX));
        cursor = LogPut(cursor, " bx=0x");  cursor = LogHex(cursor, VDM_REG16(tib, VTIB_EBX));
        cursor = LogPut(cursor, " cx=0x");  cursor = LogHex(cursor, VDM_REG16(tib, VTIB_ECX));
        cursor = LogPut(cursor, " dx=0x");  cursor = LogHex(cursor, VDM_REG16(tib, VTIB_EDX));
        cursor = LogPut(cursor, " si=0x");  cursor = LogHex(cursor, VDM_REG16(tib, VTIB_ESI));
        cursor = LogPut(cursor, " di=0x");  cursor = LogHex(cursor, VDM_REG16(tib, VTIB_EDI));
        cursor = LogPut(cursor, " bp=0x");  cursor = LogHex(cursor, VDM_REG16(tib, VTIB_EBP));
        cursor = LogPut(cursor, " ds=0x");  cursor = LogHex(cursor, VDM_REG16(tib, VTIB_DS));
        /* SP-relative lands in the thunk's saved-register block. The CALLER's
         * arguments are above the frame, so dump from BP too -- and for the
         * inline 0x56 sites, which have no such prologue, SP is the right
         * end. Both, rather than choosing wrong.
         */
        cursor = LogPut(cursor, "\r\n    @ss:sp");
        for (word = 0; word < 10; ++word)
        {
            cursor = LogPut(cursor, " ");
            cursor = LogHex(cursor, (DWORD)(stack[word * 2] | (stack[word * 2 + 1] << BYTE_SHIFT)));
        }
        {   DWORD bpValue = VDM_REG16(tib, VTIB_EBP);
            const volatile BYTE *frameBytes =
                (const volatile BYTE *)(ULONG_PTR)(stackSegmentBase + bpValue);
            cursor = Wow32LogFrame(cursor, base, bopCode, frameBytes, tib, reportSize);
            cursor = LogPut(cursor, "\r\n    @ss:bp");
            for (word = 0; word < 12; ++word)
            {
                cursor = LogPut(cursor, " ");
                cursor = LogHex(cursor, (DWORD)(frameBytes[word * 2] | (frameBytes[word * 2 + 1] << BYTE_SHIFT)));
            }
        }
        cursor = LogPut(cursor, "\r\n   ");
    }
    if (bopCode == WOW32_BOP_DISPATCH && bopSubcode == WOW32_DISPATCH_POINTER)
    {
        VDM_SET16(tib, VTIB_EBX, 0);
        VDM_SET16(tib, VTIB_EDX, 0);
        VDM_SET16(tib, VTIB_ES,  0);
        VDM_REG(tib, VTIB_EIP) += VDM_BOP_SUBFUNCTION_LENGTH;
        cursor = LogPut(cursor, " -> no 32-bit companion (BX:DX=0) => it will use the "
                    "per-call BOP path\r\n");
        WowLogFlush(base, &cursor);
        {
            *cursorIo = cursor;
            *exitCodeOut = 1;
            return HOST_FLOW_RETURN;
        }
    }
    /* SERVICE THE CALL, IF WE KNOW HOW. (GH #128) (Importance = 1):
     * 0x51 is the generic 16->32 gateway, so everything interesting
     * arrives here. wow32.h holds the frame layout, the argument
     * convention and the services themselves; this is only the wiring.
     *
     * [CAUTION]: The return value goes in a HOLE ON THE GUEST STACK, not in AX/DX
     * -- the thunk loads AX/DX from that hole after the BOP (observed:
     * anything we put in registers is overwritten before the caller
     * sees it). Wow32SetReturn() is the only correct way.
     */
    {
        INT exitCode;
        INT flow = Wow32ServiceBop(&cursor, base, bopCode, tib, machine, &wowStale, &wowStaleOk, &wowAnswer, &exitCode);
        if (flow == HOST_FLOW_RETURN)
        {
            *cursorIo = cursor;
            *exitCodeOut = exitCode;
            return HOST_FLOW_RETURN;
        }
    }
    /* STEP OVER AND KEEP GOING, RATHER THAN STOPPING THE GUEST:
     * Returning -1 here halts the run at the first unimplemented service,
     * so each rig round reveals exactly one BOP. Stepping over turns the
     * wall into a TRACE: one run lists every service krnl386 asks for, in
     * order, which is the shape of the work rather than the next item of it.
     *
     * [CAUTION]: THIS IS A DELIBERATE LIE TO THE GUEST and it is logged as one. The
     * call did not happen; whatever it returns is whatever was already in
     * the registers. Findings that depend on execution AFTER an unimplemented
     * BOP are suspect, and the log is what lets a reader tell. All the
     * non-0x53 sites are 3 bytes -- see the length note above.
     */
    VDM_REG(tib, VTIB_EIP) += VDM_BOP_LENGTH;
    cursor = LogPut(cursor, " -> UNIMPLEMENTED, STEPPED OVER (the call did NOT happen)");
    /* [CAUTION]: BUT IT IS STILL ANSWERED, because krnl386 pops the hole into AX:DX and
     * BRANCHES on it whether we wrote it or not. Print BOTH numbers: what was
     * lying in the hole, and what we put there instead. The first is what
     * previous sessions were unknowingly measuring; the second is what this
     * run actually decided on, and a reader must be able to tell them apart.
     * They are only equal by coincidence.
     */
    if (wowStaleOk)
    {
        cursor = LogPut(cursor, "; hole held 0x");
        cursor = LogHex(cursor, wowStale);
        cursor = LogPut(cursor, ", ANSWERED 0x");
        cursor = LogHex(cursor, wowAnswer);
        cursor = LogPut(cursor, (wowAnswer == (DWORD)WOW32_UNIMPL_RET)
                  ? " (harness sentinel, NOT krnl386's answer)"
                  : " (** wow32ret.txt OVERRIDE -- an EXPERIMENT, not a service **)");
    }
    cursor = LogPut(cursor, "\r\n");
    WowLogFlush(base, &cursor);
    {
        *cursorIo = cursor;
        *exitCodeOut = 1;
        return HOST_FLOW_RETURN;
    }
    *cursorIo = cursor;
    return HOST_FLOW_NEXT;
}

/* A 16-bit callback the host made has returned (its BOP at the callback address): leave the call with
 * its result (WowCallLeave restores the caller the host parked), then do what the call was for (callAction).
 */
static INT WowFinishCallback(
    PSTR *cursorIo,
    PSTR const base,
    volatile BYTE * const tib,
    INT *exitCodeOut)
{
    PSTR cursor = *cursorIo;
    DWORD result = VDM_REG16(tib, VTIB_EAX)
              | (VDM_REG16(tib, VTIB_EDX) << WORD_SHIFT);
    WOWCALL_FRAME *callFrame = WowCallLeave(tib, result);
    INT  callAction = WOWCALL_ACT_NONE;   /* copied out of fr -- see below */
    WORD actionArgument = 0;

    cursor = LogPut(cursor, "WOWCALL: <- returned 0x"); cursor = LogHex(cursor, result);
    if (!callFrame)
    {
        cursor = LogPut(cursor, " -- ★ NOTHING WAS IN FLIGHT. Something reached the "
                    "callback return stub that this host did not send "
                    "there; ending the run rather than resuming a context "
                    "we did not park.\r\n");
        LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
        {
            *cursorIo = cursor;
            *exitCodeOut = -1;
            return HOST_FLOW_RETURN;
        }
    }
    /* [CAUTION]: COPY THE ACTION OUT BEFORE ACTING ON IT (Importance = 2):
     * `WowCallLeave` POPS the frame and hands back a pointer to the
     * slot it just vacated. An action that issues a follow-up call --
     * which is the whole mechanism the EDIT chain is built on --
     * pushes a new frame into THAT SAME SLOT, so setting the new
     * frame's action through `g_WowCallFrames[g_WowCallDepth - 1]` also rewrites
     * `fr->action` underneath us. Measured: the LocalLock step armed
     * EDITFILL and the very next `if` in this function fired
     * immediately, with `res` still holding LocalAlloc's handle rather
     * than LocalLock's offset -- so the block was "filled" at offset
     * 0x2e72 (a handle, not an address), the text never arrived, and
     * the block was left in a state LocalReAlloc then refused, which
     * Notepad reports as "the file is too large for Notepad".
     *
     * The dispatch below reads these locals, never the frame.
     */
    callAction    = callFrame->Action;
    actionArgument = callFrame->ActionArgument;
    /* s88: DefDlgProc's DLGPROC answered (see WOWUSER_DEFDLGPROC). FALSE means
     * "not handled": the dialog manager's default runs and ITS value is what
     * the DefDlgProc caller gets. The frame's slot index is g_WowCallDepth now
     * that it is popped, which is where the service parked the parameters.
     */
    if (callAction == WOWCALL_ACT_DLGDEFAULT)
    {
        DWORD hole = callFrame->ReturnLinear;
        WORD frameMessage = callFrame->Message;
        if ((WORD)result == 0)
        {
            CHAR defaultName[200];
            INT defaultLength = 0;
            LRESULT defaultResult = WowUserDlgDefault(WowUserFindWindow(actionArgument), actionArgument, frameMessage,
                                             g_WowUserDlgDefaults[g_WowCallDepth].WParam,
                                             g_WowUserDlgDefaults[g_WowCallDepth].LParam,
                                             defaultName, (INT)sizeof defaultName, &defaultLength);
            defaultName[defaultLength < (INT)sizeof defaultName ? defaultLength : (INT)sizeof defaultName - 1] = 0;
            if (hole)
            {
                volatile BYTE *holeBytes = (volatile BYTE *)(ULONG_PTR)hole;
                DWORD value = (DWORD)defaultResult;
                holeBytes[0] = (BYTE)value;
                holeBytes[1] = (BYTE)(value >> BYTE_SHIFT);
                holeBytes[2] = (BYTE)(value >> WORD_SHIFT);
                holeBytes[3] = (BYTE)(value >> TOP_BYTE_SHIFT);
            }
            cursor = LogPut(cursor, " -- DLGPROC said FALSE; DefDlgProc default:");
            cursor = LogPut(cursor, defaultName);
        }
        else
        {
            cursor = LogPut(cursor, " -- DLGPROC handled it");
        }
        callAction = WOWCALL_ACT_NONE;
    }
    cursor = LogPut(cursor, " from 0x");  cursor = LogHex(cursor, callFrame->Procedure >> WORD_SHIFT);
    cursor = LogPut(cursor, ":0x");       cursor = LogHex(cursor, callFrame->Procedure & WORD_MASK);
    cursor = LogPut(cursor, " (hwnd=0x"); cursor = LogHex(cursor, callFrame->Window);
    cursor = LogPut(cursor, " msg=0x");   cursor = LogHex(cursor, callFrame->Message);
    cursor = LogPut(cursor, ", depth now "); cursor = LogHex(cursor, (DWORD)g_WowCallDepth);
    cursor = LogPut(cursor, ")");
    if (callFrame->ReturnMode == WOWCALL_RET_RESULT
        || callFrame->ReturnMode == WOWCALL_RET_RESULTW)
    {
        cursor = LogPut(cursor, " -- ★ the caller returns 0x");
        cursor = LogHex(cursor, callFrame->Written);
        /* [CAUTION]: Say when DX was DISCARDED. A WORD-returning Win16 function
         * leaves DX holding whatever it happened to hold, and printing
         * the raw DX:AX made LocalAlloc's handle read as 0x00422502 --
         * a 32-bit number that was not a value.
         */
        if (callFrame->ReturnMode == WOWCALL_RET_RESULTW && (result >> WORD_SHIFT))
            cursor = LogPut(cursor, " (WORD result; DX was litter and is discarded)");
    }
    else if (callFrame->ReturnLinear && callFrame->Message == WM_CREATE16 && (WORD)result == WOWUSER_MINUS_ONE16)
        cursor = LogPut(cursor, " -- ★ WM_CREATE REFUSED: the call that made the window"
                    " now returns 0");
    cursor = WowCallbackEditText(cursor, callAction, actionArgument, result, tib);
    cursor = WowCallbackEditLock(cursor, callAction, actionArgument, tib);
    cursor = WowCallbackEditFill(cursor, callAction, actionArgument, result, tib);
    cursor = WowCallbackClipboard(cursor, callAction, tib, result, actionArgument);
    /* THE MODAL LOOP'S NEXT TURN. (session 57) (Importance = 5):
     * A dialog procedure has just returned, so the question the loop
     * exists to ask can be asked again: has EndDialog been called? If
     * not, wait for the next message and call it again; if it has,
     * write nResult into the DialogBox return hole we have been
     * holding open and let the guest resume past a BOP it entered a
     * long time ago. Either way the context to run next is already in
     * the TIB and `return 1` runs it -- see src/wow/wowdlg.h.
     *
     * [CAUTION]: THIS RUNS AFTER WowCallLeave HAS RESTORED THE CONTEXT, which
     * is what makes the loop cost nothing: the restored context IS the
     * parked DialogBox caller, so re-entering the dialog procedure
     * simply parks it again at the same SS:SP. Nothing accumulates.
     */
    /* -- THE NEXT ITEM. (session 57) The callback has answered; 0
     * means stop, anything else means carry on. Same restored-context
     * property as the modal loop: what WowCallLeave put back IS the
     * parked caller, so the next call re-parks it at the same SS:SP
     * and nothing accumulates. See src/wow/wowenum.h.
     */
    if (callAction == WOWCALL_ACT_ENUMNEXT)
    {
        CHAR enumNote[256];
        WORD  enumCallbackSelector = WowCallbackSelector();
        DWORD enumStackBase  = DpmiSelectorBase(
            (WORD)VDM_REG16(tib, VTIB_SS));
        enumNote[0] = 0;
        WowEnumStep(tib, enumStackBase, enumCallbackSelector, WOWENUM_NEXT, result, enumNote, sizeof enumNote);
        cursor = LogPut(cursor, " -- "); cursor = LogPut(cursor, enumNote);
    }
    cursor = WowCallbackModalPump(cursor, base, callAction, tib, result, actionArgument);
    cursor = LogPut(cursor, "\r\n");
    LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
    {
        *cursorIo = cursor;
        *exitCodeOut = 1;
        return HOST_FLOW_RETURN;
    }
    *cursorIo = cursor;
    return HOST_FLOW_NEXT;
}

INT DpmiServicePmIntBody(DOS_MACHINE *machine, volatile BYTE *tib, DWORD vector, UINT steps)
{
    CHAR report[2048];
    PSTR base = report;
    PSTR cursor = report;
    SIZE_T const reportSize = sizeof report;
    DWORD ax = VDM_REG16(tib, VTIB_EAX);
    DWORD event = VDM_REG(tib, VTIB_EVENT);
    DWORD eip = DpmiPmEip(tib);

    (VOID)steps;
    /* First safe moment to re-plant anything the skip mode stepped over. */
    if (g_BreakpointCount)
        DpmiBreakpointRearmPending(DpmiSelectorBase((WORD)VDM_REG16(tib, VTIB_CS)) + eip);
    /* A CHANGE DETECTOR, NOT A DUMP. (GH #128, session 37) (Importance = 1):
     * pmbp.txt's dump column answers "what is there when I stop here", which needs
     * you to already know where to stop. The question that costs sessions is the
     * other one: "WHO WROTE THIS", for a byte whose writer is not in the guest's
     * own code -- krnl386's per-drive flag table at DGROUP 0x2a2 is read from two
     * places in its segments 1-3 and written from none, and it is nonetheless wrong
     * by the time the Win16 program is loaded.
     * Sampling one byte at every PM event cannot name the instruction, but it
     * brackets the write between two events that DO name themselves, which is the
     * whole of a bisect's first step and costs one run instead of five.
     */
    /* [CAUTION]: Segment 0 means "the offset IS a linear address". krnl386 GROWS its DGROUP,
     * so the descriptor-limit match that records g_WowPmBase[] never fires for
     * segment 4 -- the watch printed "watching" and never "armed", which is the
     * silent-instrument failure this project keeps paying for, one level down. Take
     * the linear address by hand off a `dsbase=` line when that happens.
     */
    if (g_PmWatchOffset && !g_PmWatchLinear && g_PmWatchSegment == 0)
        g_PmWatchLinear = g_PmWatchOffset;
    if (g_PmWatchOffset && !g_PmWatchLinear && g_PmWatchSegment >= 1 &&
        g_PmWatchSegment <= WOW_PMBASE_MAX && g_WowPmBase[g_PmWatchSegment - 1])
    {
        CHAR watchLine[160];
        CHAR *watchCursor = watchLine;
        g_PmWatchLinear = g_WowPmBase[g_PmWatchSegment - 1] + g_PmWatchOffset;
        watchCursor = LogPut(watchCursor, "PMWATCH armed: seg "); watchCursor = LogHex(watchCursor, g_PmWatchSegment);
        watchCursor = LogPut(watchCursor, " + 0x"); watchCursor = LogHex(watchCursor, g_PmWatchOffset);
        watchCursor = LogPut(watchCursor, " = linear 0x"); watchCursor = LogHex(watchCursor, g_PmWatchLinear);
        watchCursor = LogPut(watchCursor, "\r\n"); LogAppend(LOG_PATH, watchLine, watchCursor); SerialOut(watchLine, watchCursor);
    }
    if (g_PmWatchLinear)
    {
        const volatile BYTE *watch = (const volatile BYTE *)(ULONG_PTR)g_PmWatchLinear;
        if (MemoryReadable((ULONG_PTR)watch, 1))
        {
            BYTE now = *watch;
            if (!g_PmWatchHave)
            {
                g_PmWatchHave = 1;
                g_PmWatchLast = now;
            }
            else if (now != g_PmWatchLast)
            {
                CHAR watchLine[200];
                CHAR *watchCursor = watchLine;
                watchCursor = LogPut(watchCursor, "PMWATCH linear 0x"); watchCursor = LogHex(watchCursor, g_PmWatchLinear);
                watchCursor = LogPut(watchCursor, " CHANGED 0x"); watchCursor = LogHexByte(watchCursor, g_PmWatchLast);
                watchCursor = LogPut(watchCursor, " -> 0x"); watchCursor = LogHexByte(watchCursor, now);
                watchCursor = LogPut(watchCursor, " -- first seen at cs:eip=0x");
                watchCursor = LogHex(watchCursor, VDM_REG16(tib, VTIB_CS));
                watchCursor = LogPut(watchCursor, ":0x"); watchCursor = LogHex(watchCursor, eip);
                watchCursor = LogPut(watchCursor, " ev=0x"); watchCursor = LogHex(watchCursor, event);
                watchCursor = LogPut(watchCursor, " ax=0x"); watchCursor = LogHex(watchCursor, ax);
                watchCursor = LogPut(watchCursor, "\r\n");
                LogAppend(LOG_PATH, watchLine, watchCursor); SerialOut(watchLine, watchCursor);
                g_PmWatchLast = now;
            }
        }
    }
    /* NATIVE BOPs: krnl386 CALLING ITS 32-BIT COMPANION. (GH #128):
     * `C4 C4 nn` in krnl386's OWN code -- not one our INT-site patcher planted, so
     * DpmiBopVector() finds nothing in the patch map and hands us vec 0. That used to
     * fall through to "unexpected PM stop", which stopped the guest while leaving
     * the host alive: a hang, not a crash, and misleading to look at.
     *
     * This is the WOW32 half of WOW -- what real Windows implements in wow32.dll --
     * reached exactly the way our own DOS layer uses BOPs in the other direction.
     * The BOP numbers seen from segment 1: 0x51, 0x53, 0x56 and 0xFE.
     *
     * [INFO]: 0x53 SUB 0x03 IS THE ONE THAT MATTERS FIRST. It arrives early, asking for a
     * far pointer to a 32-bit dispatch routine, and the answer decides the path
     * (observed): with a pointer, later calls go through
     * it; with NULL, every later call arrives as its own BOP 0x56. So answering
     * NULL is not a stub: it selects the per-call BOP path that krnl386 already
     * implements, and is the honest answer while no 32-bit companion exists.
     *
     * [CAUTION]: Note the LENGTHS DIFFER. 0x53 carries a sub-function byte (4 bytes total);
     * 0x51/0x56/0xFE do not (3) -- established by where the guest's next
     * instruction begins, and 0x56 is followed by the caller's own stack cleanup,
     * so it is a call with stack arguments. Getting a length wrong here resumes the
     * guest mid-instruction.
     */
    if (vector == 0 && event == VDM_EVENT_BOP)
    {
        DWORD codeLinear = DpmiSelectorBase((WORD)VDM_REG16(tib, VTIB_CS)) + eip;
        const volatile BYTE *bopBytes = (const volatile BYTE *)(ULONG_PTR)codeLinear;
        /* A 16-BIT PROCEDURE THIS HOST CALLED HAS RETURNED (Importance = 5):
         * The one BOP in the whole VDM that is not the guest asking us for
         * something -- it is the guest finishing something we asked IT for.
         * Matched on the LINEAR ADDRESS of our own three bytes, which is exact:
         * the code byte would be a guess about a namespace our own INT-site
         * patcher also writes into. See src/wow/wowcall.h.
         *
         * [INFO]: THE RESULT IS DX:AX, and that is the Win16 convention for a LONG,
         * not a reading of any one procedure.
         *
         * [CAUTION]: AN UNEXPECTED ARRIVAL IS NOT INERT. If nothing is in flight, some
         * guest reached our stub on its own, and the honest thing is to say so
         * and stop -- resuming would run whatever context happened to be live.
         */
        if (bopBytes[0] == VDM_BOP0 && bopBytes[1] == VDM_BOP1 && g_WowCallbackLinear && codeLinear == g_WowCallbackLinear)
        {
            {
                INT exitCode;
                INT flow = WowFinishCallback(&cursor, base, tib, &exitCode);
                if (flow == HOST_FLOW_RETURN)
                    return exitCode;
            }
        }
        if (bopBytes[0] == VDM_BOP0 && bopBytes[1] == VDM_BOP1)
        {
            {
                INT exitCode;
                INT flow = DpmiServiceWowBop(&cursor, base, bopBytes, tib, eip, reportSize, machine, &exitCode);
                if (flow == HOST_FLOW_RETURN)
                    return exitCode;
            }
        }
    }

    /* PUSH ANYTHING THE GUEST WROTE INTO THE DESCRIPTOR SHADOW. (GH #128):
     * krnl386 claims descriptor slots by writing the table directly and then calls
     * INT 31h to configure them, so reconciling HERE -- before servicing anything --
     * puts its direct writes into the real LDT in the right order. The log line is
     * deliberate: it is how we find out whether a write-trapping shadow is needed
     * instead of this one. See WowShadowSync.
     */
    if (g_WowShadow)
    {
        PSTR syncCursor = cursor;
        INT syncedCount = WowShadowSync(&syncCursor);
        if (syncedCount)
        {
            cursor = syncCursor;
            cursor = LogPut(cursor, "  LDTSYNC: "); cursor = LogHex(cursor, (DWORD)syncedCount);
            cursor = LogPut(cursor, " descriptor(s) pushed from the shadow (total 0x");
            cursor = LogHex(cursor, g_WowSyncWrites); cursor = LogPut(cursor, ")\r\n");
            LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
        }
    }
                    /* THE CLIENT'S OWN PM HANDLER WINS, IF IT INSTALLED ONE:
                     * Scoped to INT 21h ON PURPOSE, for now. DOS/4GW also installs PM
                     * handlers for 10h/16h/1Ah/... and by the spec those should route to
                     * it too -- but our 0300 (simulate real-mode interrupt) currently only
                     * implements INT 21h, so routing video and keyboard to a handler that
                     * then asks us to simulate a real-mode INT 10h would trade a working
                     * path for a broken one. Route the interrupt the evidence names,
                     * measure, then widen. The order matters: this test comes before every
                     * service arm below, so it cannot be shadowed by one of them.
                     */
                    /* SNOOP `AH=25h` ON THE WAY PAST: IT IS HOW A GAME HOOKS ITS TIMER:
                     * INT 31h 0205 is not the only way a client installs a protected-mode
                     * handler, and for the case that matters it is not the way used at all.
                     * Doom hooks its timer with DOS's own call:
                     *   AX=3508 int 21h          ; save the old vector
                     *   AX=2508 int 21h          ; DS:EDX = 0x187:0x03ae31f0
                     * and DS is its OWN 32-bit code selector, with `60 1e 06 0f a0 0f a8`
                     * (pushad; push ds/es/fs/gs) at the target -- an ISR prologue.
                     * DOS/4GW's own PM INT 21h handler services that internally, in its own
                     * IDT, and never tells us: no 0205 for vector 8 appears in a whole run.
                     * So without this we do not know where the game's timer ISR is, and the
                     * only INT 08h we can see is the extender's arming-pass stub -- which is
                     * both the wrong target and, measured, fatal to enter asynchronously.
                     * Record it and let the client's handler run as well; both want it, and
                     * observing a call costs the client nothing.
                     */
                    /* THE HOST OWNS THE HARDWARE-INTERRUPT VECTORS OF A 32-BIT CLIENT:
                     * Doom hooks its timer with DOS's own call rather than INT 31h 0205:
                     *   AX=3508 int 21h ; AX=2508 int 21h, DS:EDX = 0x187:0x03ae31f0
                     * with DS its own 32-bit code selector and `60 1e 06 0f a0 0f a8`
                     * (pushad; push ds/es/fs/gs) at the target -- an ISR prologue.
                     * Letting DOS/4GW service that does not work: it FAILS the hook (CF=1,
                     * measured) and then reports "fatal error (1001): error in interrupt
                     * chain", because the extender is trying to splice a handler into a
                     * chain whose hardware end WE are, not it. It cannot see our injection
                     * and we cannot see its internal IDT, and a chain with two owners is
                     * exactly what error 1001 describes.
                     * So for vectors 08h-0Fh -- the PIC lines, the ones this host actually
                     * delivers -- answer 25h/35h ourselves against g_PmInt[], the table
                     * injection reads. 21h and the rest still go to the client's handler,
                     * untouched. Scoped to a 32-bit client so nothing 16-bit changes.
                     */
                    if (vector == VECTOR_DOS && g_DpmiIsClient32)
                    {
                        {
                            INT exitCode;
                            INT flow = DpmiServiceClient32Int21(&cursor, base, ax, tib, &exitCode);
                            if (flow == HOST_FLOW_RETURN)
                                return exitCode;
                        }
                    }
                    /* [CAUTION]: AND DO NOT ADD THE FP RANGE HERE. (session 56.) WIN87EM's
                     * handlers ARE installed in g_PmInt[] and it is tempting to widen
                     * this test to reach them -- but this dispatcher resumes the client
                     * at `sEIP + 2` unconditionally (see the assignment near the end of
                     * DpmiDispatchToPmHandler), and an FP-emulator handler ADJUSTS
                     * ITS OWN RETURN ADDRESS: `CD 39` is followed by the x87
                     * instruction's modrm and displacement, which the handler decodes
                     * and then steps over. Coming back at sEIP+2 would drop the guest
                     * onto its own operand bytes and execute them as code.
                     * The FP range is reflected as a REAL interrupt instead, on the
                     * guest's own stack, where the handler's IRET is the thing that
                     * decides where it returns to. See the #GP(IDT) arm.
                     */
                    if (vector == VECTOR_DOS && g_PmInt[vector].Client && !g_PmDispatch[vector])
                    {
                        INT status = DpmiDispatchToPmHandler(machine, tib, vector, steps);
                        if (status != 0 || g_PmInt[vector].Selector)
                            return status;
                        /* rc==0 with no handler means dispatch declined -> fall through */
                    }
                    if (vector == DPMI_BP_VEC)                        /* guest breakpoint hit */
                    {
                        {
                            INT exitCode;
                            INT flow = DpmiServiceBreakpoint(&cursor, base, tib, eip, steps, &exitCode);
                            if (flow == HOST_FLOW_RETURN)
                                return exitCode;
                        }
                    }
                    if (vector == VECTOR_VIDEO)                               /* video BIOS in PM -> VDD */
                    {
                        NTVDD_REGISTERS registers;
                        RegistersLoad(&registers, tib);
                        HOST_LOCK();
                        VddBusDeliverInterrupt(&g_Bus, VECTOR_VIDEO, &registers);
                        HOST_UNLOCK();
                        Int10WaitAfter();                     /* #226: 4F07h BL=80h */
                        RegistersStore(&registers, tib);
                        VideoTrapSync();          /* mode 12h: interpret (GH #55); no-op in 13h */
                        VDM_REG(tib, VTIB_EFLAGS) &= ~EFLAGS_CF_U;
                        VDM_REG(tib, VTIB_EIP) += DPMI_PM_BOP_LENGTH;              /* past the 2-byte PM BOP */
                        cursor = LogPut(cursor, "INT10h (PM) -> video VDD AX=0x"); cursor = LogHex(cursor, ax);
                        cursor = LogPut(cursor, "\r\n"); LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
                        return 1;
                    }
                    if (vector == VECTOR_KEYBOARD_SERVICES)                               /* keyboard BIOS in PM -> VDD */
                    {
                        NTVDD_REGISTERS registers;
                        BYTE int16Ah;
                        RegistersLoad(&registers, tib);
                        int16Ah = VddGetAh(&registers);
                        for (;;)                                   /* AH=00/10 block until a key */
                        {
                            HOST_LOCK();
                            VddBusDeliverInterrupt(&g_Bus, VECTOR_KEYBOARD_SERVICES, &registers);
                            HOST_UNLOCK();
                            if ((int16Ah != BIOS_KEYBOARD_READ && int16Ah != BIOS_KEYBOARD_READ_EXTENDED) || registers.ZeroFlag == 0 || !g_Running)
                                break;
                            InterlockedIncrement(&g_DpmiIteration);    /* keep the watchdog happy while blocked */
                            WaitForSingleObject(g_KeyEvent, INPUT_KEY_WAIT_MS);
                        }
                        RegistersStore(&registers, tib);
                        /* set CF/ZF directly in the PM eflags (no real-mode IRET frame in PM) */
                        if (registers.CarryFlag)
                            VDM_REG(tib, VTIB_EFLAGS) |= EFLAGS_CF_U;
                        else
                            VDM_REG(tib, VTIB_EFLAGS) &= ~EFLAGS_CF_U;
                        if (registers.ZeroFlag)
                            VDM_REG(tib, VTIB_EFLAGS) |= EFLAGS_ZF_U;
                        else
                            VDM_REG(tib, VTIB_EFLAGS) &= ~EFLAGS_ZF_U;
                        VDM_REG(tib, VTIB_EIP) += DPMI_PM_BOP_LENGTH;               /* past the 2-byte PM BOP */
                        (VOID)base;                                 /* no per-poll logging (would flood) */
                        return 1;
                    }
                    if (vector == VECTOR_MOUSE)                               /* mouse in PM -> INT 33h */
                    {
                        MouseInt33(tib, I33_SRC_PM);
                        VDM_REG(tib, VTIB_EIP) += DPMI_PM_BOP_LENGTH;
                        return 1;
                    }
                    if (vector == VECTOR_TIME || vector == VECTOR_TIMER)                /* BIOS time / timer tick in PM */
                    {
                        NTVDD_REGISTERS registers;
                        RegistersLoad(&registers, tib);
                        HOST_LOCK();
                        VddBusDeliverInterrupt(&g_Bus, (BYTE)vector, &registers);   /* INT 1Ah get/set tick, or INT 08h increment */
                        /* The BIOS timer ISR ends with its EOI; a PM handler that chains
                         * here is relying on it, as in V86 (#173).
                         */
                        if (vector == VECTOR_TIMER)
                            VddPicEndOfInterrupt(&g_Pic, PIC_IRQ_TIMER);
                        HOST_UNLOCK();
                        RegistersStore(&registers, tib);
                        VDM_REG(tib, VTIB_EIP) += DPMI_PM_BOP_LENGTH;
                        return 1;
                    }
                    /* -- THE DEFAULT HANDLER FOR A HARDWARE IRQ REFLECTS IT TO REAL
                     * MODE -- AND OURS HAD NO ARM FOR THE KEYBOARD. (s80) ------------
                     * DOS/4GW hooks PM INT 09h at startup (`setPMvec 09 = 1cf:0024`) with
                     * a pass-up handler that CHAINS to the previous PM vector, i.e. to our
                     * `C4 C4 CF` default stub. DPMI 0.9 says that default reflects the
                     * interrupt to the real-mode vector. This dispatcher had no arm for 09h,
                     * so the chain landed in "unexpected PM stop", the injector ABANDONED
                     * the ISR at 0 phases, and the key stayed pending and was re-offered
                     * every pass.
                     *
                     * [INFO]: MEASURED -- this is "run Doom from COMMAND.COM crashes". The key
                     * that launches the program (Enter's break code) arrives BEFORE Doom
                     * installs its own INT 09h, so it is DOS/4GW's pass-up handler that
                     * takes it. Eleven abandonments at 177:001b, then the twelfth ran
                     * DOS/4GW's own abort, printed through AH=06h one byte at a time:
                     *     DOS/4GW Professional error (2002): transfer stack overflow
                     *     on interrupt 09h at 1BF:00000070
                     * -- each abandoned dispatch had leaked one frame of its transfer
                     * stack. Launched directly, no key arrives until Doom has hooked 09h,
                     * which is why this was never seen. SETUP's "save and launch" is the
                     * same Enter key.
                     * - Reflect only while IVT[n] is still OUR stub (no guest hooked the
                     *   real-mode vector); then the real-mode handler is ours and its whole
                     *   job is known -- the V86 `BOP 09` arm: consume the byte, and the EOI
                     *   the BIOS handler ends with. A guest-hooked real-mode vector needs a
                     *   true nested-V86 reflection, which does not exist yet; it keeps
                     *   falling through to the loud stop below rather than being faked.
                     *   IRQ2-7 get the same treatment with no device work: their real-mode
                     *   default is a bare IRET that the host EOIs for (AsyncVectorIsOurStub).
                     */
                    if (((vector >= VECTOR_IRQ1 && vector <= VECTOR_IRQ7 && AsyncVectorIsOurStub(vector - PIC_MASTER_VECTOR_BASE))
                         || (vector >= VECTOR_IRQ8 && vector <= VECTOR_IRQ15 && AsyncVectorIsOurStub(vector - PIC_SLAVE_VECTOR_BASE + PIC_LINES_PER_CHIP))))
                    {
                        BYTE line = (BYTE)(vector >= VECTOR_IRQ8 ? vector - PIC_SLAVE_VECTOR_BASE + PIC_LINES_PER_CHIP : vector - PIC_MASTER_VECTOR_BASE);
                        HOST_LOCK();
                        /* take the byte, re-arm if more queued. #254: no guest code runs from
                         * here, so a Pause cannot spin -- drop its flag rather than let it
                         * swallow the next key. (Ctrl-Break's ring/0071h part is done.)
                         */
                        if (vector == VECTOR_KEYBOARD && VddInputBiosConsume(&g_Input) == INPUT_ACTION_PAUSE)
                            VddInputPauseCancel(&g_Input);
                        VddPicEndOfInterrupt(&g_Pic, line);   /* the slave's EOI also releases the cascade */
                        HOST_UNLOCK();
                        if (g_PmIrqReflectLogged < 16)
                        {
                            ++g_PmIrqReflectLogged;
                            cursor = LogPut(cursor, "PM INT 0x"); cursor = LogHexByte(cursor, (BYTE)vector);
                            cursor = LogPut(cursor, " default handler -> reflected to the BIOS (IVT is ours): consume+EOI\r\n");
                            LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
                        }
                        ++g_PmIrqReflects;
                        VDM_REG(tib, VTIB_EIP) += DPMI_PM_BOP_LENGTH;               /* past the BOP -> the stub's IRET */
                        return 1;
                    }
                    /* ...and when the GUEST owns the real-mode vector, run its ISR there.
                     * See DpmiReflectIrqToRm (s81, ZAR).
                     */
                    if ((vector >= VECTOR_IRQ1 && vector <= VECTOR_IRQ7) || (vector >= VECTOR_IRQ8 && vector <= VECTOR_IRQ15))
                    {
                        if (DpmiReflectIrqToRm(machine, tib, (UINT)vector))
                        {
                            if (g_PmIrqRmReflects <= 8)
                            {
                                cursor = LogPut(cursor, "PM INT 0x"); cursor = LogHexByte(cursor, (BYTE)vector);
                                cursor = LogPut(cursor, " default handler -> reflected to the guest's real-mode ISR 0x");
                                cursor = LogHex(cursor, PeekWord(IVT_SEGMENT_ADDRESS(vector))); cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, PeekWord(IVT_OFFSET_ADDRESS(vector)));
                                cursor = LogPut(cursor, "\r\n");
                                LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
                            }
                            VDM_REG(tib, VTIB_EIP) += DPMI_PM_BOP_LENGTH;           /* past the BOP -> the stub's IRET */
                            return 1;
                        }
                    }
                    if (vector == VECTOR_EQUIPMENT)                               /* BIOS equipment, in PM */
                    {
                        /* Same answer as the V86 arm below, and now the SAME
                         * FUNCTION rather than the same constant copied twice --
                         * see BiosEquipmentWord(), which also explains why the
                         * serial count is derived from the VDD instead of being
                         * asserted in a comment that was wrong at both sites.
                         */
                        WORD equipmentWord = BiosEquipmentWord();
                        VDM_SET16(tib, VTIB_EAX, equipmentWord);
                        cursor = LogPut(cursor, "INT11h(PM) equipment -> 0x"); cursor = LogHex(cursor, equipmentWord);
                        cursor = LogPut(cursor, "\r\n");
                        LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
                        VDM_REG(tib, VTIB_EIP) += DPMI_PM_BOP_LENGTH;
                        return 1;
                    }
                    if (vector == VECTOR_SYSTEM)                               /* BIOS misc/system, in PM */
                    {
                        {
                            INT exitCode;
                            INT flow = DpmiServiceInt15(&cursor, base, tib, ax, &exitCode);
                            if (flow == HOST_FLOW_RETURN)
                                return exitCode;
                        }
                    }
                    if (vector == VECTOR_KERNEL_DEBUGGER)                               /* Windows kernel debugger */
                    {
                        /* [CAUTION]: THIS ARM EXISTS TO STOP A SILENT DEATH, NOT TO PROVIDE A
                         * SERVICE. INT 41h is the WDEB386 kernel-debugger interface;
                         * krnl386 calls it six times during init (AX=0001, 000F,
                         * 0012, 0040, 004F). With no debugger present the correct
                         * behaviour is to return with registers untouched -- the
                         * "is a debugger there?" query (AX=004Fh) answers by
                         * returning 0xF386 when one IS, so anything else means no.
                         * Without the arm the site stays an unpatched `CD 41`, and a
                         * PM guest executing that reaches the kernel #GP reflect,
                         * which does not reflect: it terminates the VDM with no
                         * exception and no log line. Doing nothing, visibly, beats
                         * doing nothing invisibly.
                         */
                        cursor = LogPut(cursor, "INT41h(PM) kernel-debugger AX=0x"); cursor = LogHex(cursor, ax);
                        cursor = LogPut(cursor, " -> no debugger (registers untouched)\r\n");
                        LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
                        VDM_REG(tib, VTIB_EIP) += DPMI_PM_BOP_LENGTH;
                        return 1;
                    }
                    if (vector == VECTOR_MULTIPLEX)                               /* INT 2Fh, from PM */
                    {
                        {
                            INT exitCode;
                            INT flow = DpmiServiceInt2F(&cursor, base, ax, tib, machine, &exitCode);
                            if (flow == HOST_FLOW_RETURN)
                                return exitCode;
                        }
                    }
                    if (vector == VECTOR_DPMI)                               /* DPMI INT 31h */
                    {
                        {
                            INT exitCode;
                            INT flow = DpmiServiceInt31(&cursor, base, &ax, tib, machine, &exitCode);
                            if (flow == HOST_FLOW_RETURN)
                                return exitCode;
                        }
                    }
                    if (vector == VECTOR_DOS)                               /* DOS INT 21h (in PM) */
                    {
                        {
                            INT exitCode;
                            INT flow = DpmiServiceInt21(&cursor, base, ax, steps, tib, machine, &exitCode);
                            if (flow == HOST_FLOW_RETURN)
                                return exitCode;
                        }
                    }
                    /* THE LAST THING THE RUN SAYS SHOULD NAME THE WALL:
                     * This printed only the event and a CS:EIP, and that is how the
                     * 32-bit EIP truncation hid: "0x187:0x0be7" looked like a wild jump
                     * into low memory when it was really 0x03b10be7, two bytes past the
                     * BOP we had just planted in Doom's own code. Dump the linear
                     * address, the instruction bytes and the register file, so the run
                     * that ends here identifies its own cause instead of needing a
                     * breakpoint sweep to re-find it.
                     */
                    { DWORD csValue = VDM_REG16(tib, VTIB_CS);
                      DWORD linear = DpmiSelectorBase((WORD)csValue) + eip;
                      cursor = LogPut(cursor, "DPMI: unexpected PM stop event=0x"); cursor = LogHex(cursor, event);
                      cursor = LogPut(cursor, " CS:EIP=0x"); cursor = LogHex(cursor, csValue);
                      cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, eip);
                      cursor = LogPut(cursor, " linear=0x"); cursor = LogHex(cursor, linear);
                      cursor = LogPut(cursor, (csValue && DpmiSelectorIs32((WORD)csValue)) ? " (32-bit CS)" : " (16-bit CS)");
                      cursor = LogPut(cursor, " EAX=0x"); cursor = LogHex(cursor, VDM_REG(tib, VTIB_EAX));
                      cursor = LogPut(cursor, " EBX=0x"); cursor = LogHex(cursor, VDM_REG(tib, VTIB_EBX));
                      cursor = LogPut(cursor, " ECX=0x"); cursor = LogHex(cursor, VDM_REG(tib, VTIB_ECX));
                      cursor = LogPut(cursor, " EDX=0x"); cursor = LogHex(cursor, VDM_REG(tib, VTIB_EDX));
                      cursor = LogPut(cursor, " DS=0x");  cursor = LogHex(cursor, VDM_REG16(tib, VTIB_DS));
                      cursor = LogPut(cursor, " SS:ESP=0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_SS));
                      cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, VDM_REG(tib, VTIB_ESP));
                      /* Two bytes back is the BOP/INT itself, on the same +2 convention
                       * the patched-INT path uses. HostReadable(), never IsBadReadPtr:
                       * a probe that faults on purpose kills the run it exists to watch.
                       */
                      cursor = LogPut(cursor, " bytes@eip-2=");
                      { const BYTE *codeBytes = (const BYTE *)(ULONG_PTR)(linear - DPMI_PM_BOP_LENGTH);
                        if (linear < DPMI_PM_BOP_LENGTH || !HostReadable(codeBytes, 16))
                            cursor = LogPut(cursor, "<unreadable from host>");
                        else
                            cursor = LogDump(cursor, codeBytes, 16); }
                      /* -- WAS THAT `C4 C4` OURS? SAY SO, RATHER THAN LEAVING
                       * IT AMBIGUOUS. (session 55) When a run dies on a byte
                       * pair that looks like a BOP, the log has always left
                       * the reader to guess between three things: a real
                       * native BOP, an INT SITE WE PATCHED whose vector we
                       * then failed to resolve, and a coincidence in data.
                       * pmap is the record of every site we rewrote, keyed by
                       * linear address, so it can answer directly -- and the
                       * answer changes the whole diagnosis. Both candidate
                       * addresses are probed because this dump's own +/-2
                       * convention is borrowed from the patched-INT path and
                       * is not certain to apply to this event.
                       * CALC and TASKMAN both end here, so this line is meant to
                       * name their blocker on the next run rather than after
                       * another session of reading disassembly.
                       */
                      { BYTE vector0 = PatchMapGet(linear), vector2 = PatchMapGet(linear - DPMI_PM_BOP_LENGTH);
                        cursor = LogPut(cursor, " pmap[eip]=");
                        if (vector0) { cursor = LogPut(cursor, "INT 0x"); cursor = LogHexByte(cursor, vector0);
                                  cursor = LogPut(cursor, " ★ THIS IS A SITE WE PATCHED"); }
                        else
                            cursor = LogPut(cursor, "none");
                        cursor = LogPut(cursor, " pmap[eip-2]=");
                        if (vector2) { cursor = LogPut(cursor, "INT 0x"); cursor = LogHexByte(cursor, vector2);
                                  cursor = LogPut(cursor, " ★ THIS IS A SITE WE PATCHED"); }
                        else
                            cursor = LogPut(cursor, "none");
                        if (!vector0 && !vector2)
                            cursor = LogPut(cursor, "  => NOT one of our patches: either a"
                                        " real BOP or data being executed"); }
                      cursor = LogPut(cursor, "\r\n"); }
                    LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
                    return -1;
}
