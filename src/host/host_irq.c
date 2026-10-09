/* host_irq.c -- interrupt injection: the asynchronous IRQ path into V86 and protected mode,
 *   IF/VIF, and the host's IRQ sink.
 *
 * Its own translation unit (#335): declared in host_irq.h. */
#include "host_state.h"
#include "log.h"
#include "host_irq.h"
#include "host_video.h"
#include "host_timing.h"
#include "host_dpmi.h"
#include "host_diag.h"
#include "host_bios.h"
#include "host_input.h"


static DWORD g_KeyIrqLogged = 0;           /* bounded KEYIRQ account; see HostIrqSink */
DWORD    g_Irq1AsyncInjected;        /* IRQ1s placed on the ASYNC path (see above) */
static DWORD          g_IrqNRefuseLog = 0; /* bounded refusal-log budget (see the gate)  */
VOID SkipIfSiteNote(DWORD codeSegment, DWORD instructionPointer, DWORD stub)
{
    INT index;
    for (index = 0; index < SKIPIF_SITES; ++index) {
        if (g_SkipIfSite[index].Count && g_SkipIfSite[index].Cs == codeSegment && g_SkipIfSite[index].Ip == instructionPointer) { g_SkipIfSite[index].Count++; return; }
        if (!g_SkipIfSite[index].Count) {
            g_SkipIfSite[index].Cs = (WORD)codeSegment; g_SkipIfSite[index].Ip = (WORD)instructionPointer;
            g_SkipIfSite[index].Stub = (WORD)stub; g_SkipIfSite[index].Count = 1; return;
        }
    }
}
/* The PROTECTED-mode vector a DPMI client hooks for a line: DPMI 0.9 reflects hardware
   interrupts at the PIC's own vector numbers, 08h-0Fh and 70h-77h. */
UINT IrqPmVector(UINT irq) { return irq < PIC_LINES_PER_CHIP ? PIC_MASTER_VECTOR_BASE + irq : PIC_SLAVE_VECTOR_BASE + (irq - PIC_LINES_PER_CHIP); }
DWORD          g_IrqRaisedAny = 0;
DWORD          g_QiBits       = 0;
INT            g_QiKeysAsync = 0;   /* opt-in: async-deliver IRQ1 (see HostIrqSink) */
/* ── ⚠⚠ ONE THREAD MAY OWN THE GUEST'S CONTEXT AT A TIME, AND UNTIL THE COURIER
     THERE WAS ONLY EVER ONE. AsyncInjectIrq() reads the context, computes an IRET
     frame from it and writes it back; two threads doing that concurrently would each
     build a frame from a context the other had already superseded, and the two frames
     would collide on the guest's stack. It was safe by accident: every caller ran
     inside HostPitSync, under g_Lock. The tick courier deliberately runs OUTSIDE
     that lock (that is the whole point of it), so the guarantee has to become explicit
     rather than incidental.
   ⚠ This is NOT the g_Lock interlock and does not replace it. g_Lock answers "is the
     thread I am about to suspend holding a lock?"; this answers "is another thread
     already rewriting the context?". The courier satisfies the first by confirming
     g_InExec AFTER the suspend has landed -- see the throttle's note for why that
     order is sound -- and the second by holding this.
   ⚠ The CPU throttle also suspends this thread, and that stays safe without taking
     this: it only READS the context, and suspend counts nest. */
volatile LONG  g_AsyncContextWrite   = 0;   /* 1 while a thread owns the guest CONTEXT */
DWORD          g_QiCalls      = 0;
volatile LONG  g_QiStatus     = 0;  /* NTSTATUS of the last queue call */
/* ASYNC INJECTION, DONE OURSELVES. VdmQueueInterrupt turned out to be transition-only, so
   the kernel will not break a spinning guest out for us. But a suspended thread's CONTEXT is
   readable and writable even while it sits inside VdmStartExecution, and for a V86 thread
   that context IS the guest's frame. So we can do exactly what the CPU would: push the IRET
   frame on the guest's stack and point CS:EIP at the vector -- from another thread, with no
   kernel cooperation. Called from the device thread that raises the IRQ.

   Guard rails, because we are rewriting a context the kernel is actively running:
     - only when EFLAGS.VM is set (the thread really is running guest code, not our host);
     - only when the guest's interrupts are on -- IF or VIF, since under VME its STI sets VIF;
     - never while CS is our own handler segment (we would re-enter a BOP stub mid-service);
     - never while the exec loop owns the context (g_InExec), or we would race it.
   Any of those => decline and count it; the normal in-loop path will catch the IRQ later. */
/* AN IN-SERVICE INTERLOCK -- the bit of the 8259 we do not have.
   A real PIC sets an in-service bit when it delivers a line and will not deliver again until
   the handler acknowledges with an EOI. We do not emulate the PIC at all (0x20/0x21 are still
   unclaimed), so nothing stopped us re-entering a handler that had already been entered: a
   keyboard ISR typically re-enables interrupts early, so the very next injected IRQ1 landed
   inside it, and the next inside that, until the guest drowned in nested handlers -- exactly
   the "press a key and everything hangs" regression that async IRQ1 delivery introduced.

   Until there is a real PIC VDD, approximate the in-service bit with the guest's own stack:
   an injected handler has our 6-byte frame pushed, so while the guest's SP is BELOW where it
   was at injection (same SS), that handler has not IRETed yet. Refuse to inject while that
   holds. A handler that switches stacks, or never returns, would latch this forever, so it
   also times out. This is a guard, not a PIC -- the real fix is the PIC VDD (resume item 4),
   which also gets us IRQ masking. */
DWORD          g_AsyncNestBlocked = 0;   /* refused: line masked or in service */
static VOID AsyncWhyNote(UINT irq, UINT why)
{
    g_AsyncWhy = (LONG)why;
    if (why < ASYNC_WHY_MAX) g_AsyncWhyHistogram[irq & (PIC_LINES_PER_CHIP - 1)][why]++;
}
/* ── THE IF/VIF CENSUS (s81). MEASURES; DECIDES NOTHING. ─────────────────────────────
     `irq8.nested` (4 vs 0 on three oracles) is the gate asking "IF **or** VIF": a handler
     that EOIs before its IRET is re-entered because something in the frame still reads as
     "interrupts on". The suspect is IF itself -- a user-mode frame written back through
     SetThreadContext has IF forced on, so in a LIVE V86 frame it would carry no
     information and VIF would be the only signal. The fear that parked the fix is the
     other direction: a guest whose interrupts are logically on while VIF reads clear
     (never executed STI; DPMI 0301/0302 entering V86 with IF set), which a VIF-only gate
     would starve.
     Both are claims about what the bits READ, so count them rather than argue:
       path 0  the async injector's live V86 frame (GetThreadContext on the CPU thread)
       path 1  the cooperative IRQ0/IRQ1 gate, reading the VTIB after an event exit
       path 2  the cooperative device-line gate (GuestIfEnabled), same VTIB
     state = IF<<2 | VIF<<1 | S, where S is bit 9 of the kernel's FIXED_NTVDMSTATE word
     at 0x714 -- the VDM's virtual IF, which s11 found the kernel maintaining itself.
     "shadow" counts deliveries the current gate made that a VIF-only gate would have
     refused, per line; `starve` is the longest unbroken stretch in which path 0 saw VIF
     clear -- how long a VIF-only gate would have held every line off.
     s81 run 1 (p_irq8): live IF was 1 in every sample; the nested IRQ 8s came through
     path 2, which the first cut did not count -- hence S and the delivery trace. */
DWORD g_IfvCensus[IFV_PATHS][8];
DWORD g_IfvShadow[PIC_LINES];
DWORD g_IfvStarveT0, g_IfvStarveMaximumMs, g_IfvStarveCount;
INT   g_IfvStarveOpen;
static INT   g_VifLiveSeen;   /* a live V86 frame has shown VIF set: VME is keeping it */
static DWORD IfvState(DWORD flags)
{
    DWORD virtualIf = (*(volatile DWORD *)(ULONG_PTR)FIXED_NTVDMSTATE_LINEAR >> 9) & 1u;
    return ((flags >> 7) & 4u) | ((flags & EFLAGS_VIF) ? 2u : 0u) | virtualIf;
}
VOID IfvNote(INT path, DWORD flags)
{
    INT virtualInterruptFlag = (flags & EFLAGS_VIF) != 0;
    g_IfvCensus[path][IfvState(flags)]++;
    if (path != 0) return;
    if (!virtualInterruptFlag) { if (!g_IfvStarveOpen) { g_IfvStarveT0 = GetTickCount(); g_IfvStarveOpen = 1; ++g_IfvStarveCount; } }
    else if (g_IfvStarveOpen) {
        DWORD starvedMs = GetTickCount() - g_IfvStarveT0;
        if (starvedMs > g_IfvStarveMaximumMs) g_IfvStarveMaximumMs = starvedMs;
        g_IfvStarveOpen = 0;
    }
}
static VOID IfvTrace(UINT irq, INT path, DWORD flags, DWORD codeSegment, DWORD instructionPointer)
{
    LONG index;
    UINT vector = VddPicVector(&g_Pic, (BYTE)irq);
    INT isReentry = (codeSegment == PeekWord(IVT_SEGMENT_ADDRESS(vector))) && ((WORD)(instructionPointer - PeekWord(IVT_OFFSET_ADDRESS(vector))) < 0x60);
    if (isReentry) ++g_IfvReenter[irq & (PIC_LINES - 1)];
    if (!isReentry && g_IfvTraceCount >= 8) return;
    index = InterlockedIncrement(&g_IfvTraceCount) - 1;
    if (index >= IFV_TRACE_MAX) return;
    g_IfvTrace[index].Irq = (BYTE)irq; g_IfvTrace[index].Path = (BYTE)path;
    g_IfvTrace[index].State = (BYTE)IfvState(flags); g_IfvTrace[index].Flags = flags;
    g_IfvTrace[index].Cs = (WORD)codeSegment; g_IfvTrace[index].Ip = (WORD)instructionPointer;
}
DWORD g_AsyncPmInjected = 0;             /* delivered                              */
DWORD g_AsyncInjectedLine[PIC_LINES];            /* ...and which IRQ line each one was      */
static DWORD g_AsyncPmBail2 = 0;           /* PM async attempts that did not commit  */
DWORD g_PmWatch[DPMI_WATCH_MAX];     /* linear addresses to watch (whitespace-separated) */
/* ── ★★★ A GUEST'S OWN OFFSETS ARE THE ONLY ONES WORTH WRITING DOWN. ────────────────
     pmwatch.txt took absolute linear addresses, and an extended guest IS NOT LOADED AT
     A FIXED ADDRESS: ZAR's LE code object came up at 0x03f70000 on one run and
     0x03b70000 on the next, so an address read out of one run's log is wrong for the
     next one and the watch silently reports a neighbouring allocation instead. That is
     the "instrument that fails by printing a plausible wrong answer" this project has
     already been bitten by.
   ► A LEADING `+` MEANS "RELATIVE TO THE GUEST'S CODE-OBJECT LOAD BASE" -- the address
     the host itself prints as `[LE CODE OBJECT] -> mem 0x...`. That is stable across
     runs and it is the form a disassembly gives you, so `+3c878` can be copied straight
     out of a listing and stays right tomorrow.
   ⚠ RESOLVED LAZILY, NOT AT PARSE TIME. pmwatch.txt is read during the mode switch and
     the client does not ask for its code object until AFTER that, so the base is still
     zero when the file is parsed. A watch whose base is not known yet reads as
     `????????` rather than as address 0x3c878, which would be a wrong answer wearing a
     right one's clothes. */
BYTE  g_PmWatchRel[DPMI_WATCH_MAX]; /* 1 = offset from g_LeLoadBase */
DWORD PmWatchAddress(INT index)
{
    if (!g_PmWatchRel[index]) return g_PmWatch[index];
    return g_LeLoadBase ? g_LeLoadBase + g_PmWatch[index] : 0;
}
DWORD g_PmInjectDecl[2], g_PmInjectDeclTl[IRQ0TL_SECS];
VOID PmInjectDeclineNote(INT why, WORD cs, DWORD eip)
{
    g_PmInjectDecl[why]++;
    if (g_Irq0Start) {
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        { DWORD seconds = QpcMicroseconds(now.QuadPart - g_Irq0Start) / MICROSECONDS_PER_SECOND_U;
          if (seconds < IRQ0TL_SECS) g_PmInjectDeclTl[seconds]++; }
    }
    if (why == ASYNC_DELIVERED) {
        INT index;
        for (index = 0; index < PMINJ_SITES; ++index) {
            if (g_PmInjectSite[index].Count && g_PmInjectSite[index].Cs == cs && g_PmInjectSite[index].Eip == eip) { g_PmInjectSite[index].Count++; return; }
            if (!g_PmInjectSite[index].Count) { g_PmInjectSite[index].Cs = cs; g_PmInjectSite[index].Eip = eip; g_PmInjectSite[index].Count = 1; return; }
        }
    }
}
#define ASYNC_SITE_MAX 96
static DWORD g_AsyncSiteEip[ASYNC_SITE_MAX];
static WORD  g_AsyncSiteCs[ASYNC_SITE_MAX];
INT   g_AsyncSiteCount = 0;
INT   g_AsyncSiteFull = 0;
/* 1 = not seen before (and now recorded). Runs on the timer/UI thread only, so the
   table needs no lock: AsyncInjectIrq() bails at why=20 unless the CPU thread is
   inside guest execution, which is precisely when it is not in here. */
static INT AsyncSiteNew(WORD cs, DWORD eip)
{
    INT index;
    for (index = 0; index < g_AsyncSiteCount; ++index)
        if (g_AsyncSiteEip[index] == eip && g_AsyncSiteCs[index] == cs) return 0;
    if (g_AsyncSiteCount >= ASYNC_SITE_MAX) { g_AsyncSiteFull = 1; return 0; }
    g_AsyncSiteCs[g_AsyncSiteCount] = cs; g_AsyncSiteEip[g_AsyncSiteCount] = eip;
    g_AsyncSiteCount++;
    return 1;
}
/* ► TAKES THE LINE NOW. It recorded the reason and threw away WHICH INTERRUPT was
     refused, so the SB's losses and the timer's landed in the same number -- and those
     two have different causes and different fixes (session 23 fixed the SB's by giving
     the device lines a cooperative path; that would have been invisible here). */
static VOID AsyncEarlyBail(UINT irq, UINT why)
{
    CHAR lineBuffer[96], *lineCursor = lineBuffer;
    g_AsyncBail++;
    AsyncWhyNote(irq, why);
    /* A/B/C discriminator: "the tick was raised and we could not place it". 20 is
       not_in_exec, the bail the s60 note measured at 75%. See Irq0DeliveredNote. */
    if (irq == 0 && why == ASYNC_WHY_NOT_IN_EXEC) ++g_Irq0NieCount;
    /* ── ⚠⚠ THE CAP WAS 4000 AND IT COST SKYROADS A FIFTH OF ITS TIMER. ──────────────
         This log is reached from HostIrqSink, which runs inside HostPitSync --
         **while it holds g_Lock**. Every line is a LogAppend (open/write/close) plus a
         SerialOut, so a bail that fires at the PIT's rate is FILE I/O UNDER THE DEVICE
         LOCK at that rate. Session 23 raised the cap from 40 to 4000 and justified it on
         LOG SIZE ("~900KB, well inside LOG_MAX_BYTES") against a Doom run, where the
         early bails are ~1000. Nobody costed it, and nobody tried it on a V86 guest.
         Measured on Skyroads (30 s cap, same binary path, matched durations):
             Aug 21 (before)   irq0_inj 4487   io_events 6,383,494   ASYNC-EARLY    0
             session 23        irq0_inj 3418   io_events 4,172,879   ASYNC-EARLY 3249
         -24% of the guest's delivered timer ticks and -34% of its total I/O -- which is
         the "definite timing issue affecting OPL and graphics" a player reported on a
         title that had been fully playable since session 19. **A DIAGNOSTIC INSTRUMENT
         WAS DEGRADING THE PRODUCT, AND ONLY THE USER'S EAR CAUGHT IT.**
       ► THE ACCOUNT DOES NOT NEED THE LINES. g_AsyncWhyHistogram records every bail, per
         line and per code, with no I/O at all -- strictly more information than these
         lines carried. So keep a handful for SHAPE (when in the run they start, what
         the early ones are) and let the histogram carry the totals. That is what the
         histogram was for; this is the first thing it pays for. */
    if (g_AsyncEarlyBailLogged++ >= ASYNC_EARLY_BAIL_LOG_MAX) return;
    lineCursor = LogPut(lineCursor, "ASYNC-EARLY bail irq="); lineCursor = LogHexByte(lineCursor, irq);
    lineCursor = LogPut(lineCursor, " why="); lineCursor = LogHex(lineCursor, (DWORD)why);
    lineCursor = LogPut(lineCursor, " ms="); lineCursor = LogHex(lineCursor, GetTickCount());
    lineCursor = LogPut(lineCursor, "\r\n"); LogAppend(LOG_PATH, lineBuffer, lineCursor); SerialOut(lineBuffer, lineCursor);
}
enum { ASYNC_FIRST_DEVICE_IRQ = 2 };   /* IRQ 0 and 1 (timer, keyboard) have their own paths */
INT AsyncInjectIrq(UINT irq)
{
    if (irq >= PIC_LINES) { AsyncEarlyBail(irq, ASYNC_WHY_BAD_IRQ); return 0; }
    CONTEXT context;
    DWORD eflags, ss, sp, cs, ip;
    WORD flags;
    INT isOk = 0;

    /* ► THE EARLY BAILS LOG TOO, BECAUSE THEY ARE THE ONES THAT MATTER. Everything below
         reports itself through g_AsyncWhy, but these four returned in silence -- so when
         Doom's R_ExecuteSetViewSize died inside a 3.5M-instruction BOP-free stretch, the
         log showed NO async attempt at all in the window, and "the injector never tried"
         was indistinguishable from "the injector tried and bailed before it could say so".
         That is the difference between a measurement and an unread instrument, so give
         each early exit a why code (20+) and let AsyncEarlyBail() say it out loud. */
    /* ⚠ NEVER SUSPEND ONCE WE ARE WINDING DOWN. A suspend that lands during teardown
         can leave the thread stopped forever, and a process with a suspended thread
         does not finish exiting -- which strands the system-wide hook and cursor clip
         (see HostPanicRelease). g_Running is cleared first thing in WM_DESTROY. */
    if (!g_Running) { AsyncEarlyBail(irq, ASYNC_WHY_NOT_IN_EXEC); return 0; }
    if (!g_HostCpu || g_InExec == 0) { AsyncEarlyBail(irq, ASYNC_WHY_NOT_IN_EXEC); return 0; }
    /* Mid real-mode simulation: the guest's mode is being rewritten under us. See
       g_SimIntBusy -- this is the Doom E1M1 crash. */
    if (g_SimIntBusy) {
        UINT vectorN = VddPicVector(&g_Pic, (BYTE)irq);
        /* ── THE BIOS TICK MUST STILL ADVANCE INSIDE A NESTED REAL-MODE CALL. (s81, ZAR) ──
             IRQ 0 is not delivered in here (its vector is our BOP stub, which the nested
             loop does not service), so 0040:006C stood still for the length of every
             0300/0301/0302. Miles' SB self-test (SBLASTER.DIG +0xa53) times itself by
             exactly that word -- `mov ax,es:[46Ch] / cmp ax,es:[46Ch] / je $-5`, twice,
             up to 10 times -- so with its IRQ 5 finally arriving it spun on the clock
             instead. Do the BIOS's bookkeeping here, the same rule as the PM arm's
             no_app_timer case: billed against the owed-tick count, pending consumed. */
        if (irq == 0 && g_NestedRm) {
            if (PmTickTake()) {
                /* the one BIOS tick body, witness included (#262 -- vdd_pit.h) */
                VddPitBiosTick(&g_Pit, (volatile UINT32 *)(ULONG_PTR)(BIOS_BDA_BASE + BIOS_BDA_TICK_COUNT),
                              (volatile BYTE *)(ULONG_PTR)(BIOS_BDA_BASE + BIOS_BDA_MIDNIGHT_FLAG));
                if (g_Irq0Pending > 0) InterlockedDecrement(&g_Irq0Pending);
            }
            AsyncEarlyBail(irq, ASYNC_WHY_NESTED_TICK); return 0;
        }
        if (!(g_NestedRm && irq >= ASYNC_FIRST_DEVICE_IRQ && PeekWord(IVT_SEGMENT_ADDRESS(vectorN)) != DOS_HDLR_SEG)) {
            AsyncEarlyBail(irq, ASYNC_WHY_SIMINT_RM); return 0; }
    }
    /* Ask the PIC, exactly as the hardware would: is this line unmasked, and is nothing of
       equal or higher priority still in service? That is what stops us re-entering a handler
       that has not EOI'd yet -- the fault behind "press a key and everything hangs". */
    if (irq == 0 ? !Irq0CanDeliver() : !VddPicCanDeliver(&g_Pic, (BYTE)irq)) {
        g_AsyncNestBlocked++; AsyncEarlyBail(irq, ASYNC_WHY_PIC_REFUSE); return 0; }
    /* Never deliver a line the guest has not hooked. Its vector still points at our default
       IRET stub, which means no ISR is installed -- and on a real PC an unused line sits
       masked in the PIC, so nothing would arrive at all. Delivering anyway is not harmless:
       it perturbs the guest's stack and control flow for no benefit, and it demonstrably
       derailed Skyroads (which never installs a Sound Blaster ISR) into executing junk in
       our own handler segment at 0050:006c, where it "terminated" via a garbage INT 21h. */
    /* ► A PROTECTED-MODE CLIENT HOOKS THE PM VECTOR, NOT THE IVT. This test only ever
         looked at the real-mode IVT, so for a DPMI client every device line looked
         unhooked and was refused -- including the one the Sound Blaster's own
         detection depends on. DMX resets the DSP and then issues command 0xF2, whose
         entire purpose is to make the card assert its interrupt so the driver can find
         out which line it is wired to; refusing that interrupt is exactly how Doom ends
         up printing "SB isn't responding at p=0x220, i=7, d=1" about a card that
         answered its reset with 0xAA and reported DSP version 4.05 two lines earlier.
         Ask both tables: the client has hooked the line if EITHER the real-mode vector
         has moved off our IRET stub or it has installed a protected-mode handler. */
    { UINT vector0 = VddPicVector(&g_Pic, (BYTE)irq);
      INT rmHooked = !(PeekWord(IVT_SEGMENT_ADDRESS(vector0)) == DOS_HDLR_SEG
                        && PeekWord(IVT_OFFSET_ADDRESS(vector0)) == DOS_IRET_STUB_OFF);
      INT pmHooked = g_DpmiPm && g_PmInt[IrqPmVector(irq)].Client;
      if (irq >= ASYNC_FIRST_DEVICE_IRQ && !rmHooked && !pmHooked) { AsyncEarlyBail(irq, ASYNC_WHY_UNHOOKED); return 0; } }
    /* Exclusive ownership of the guest's context for the whole suspend/rewrite/resume.
       See g_AsyncContextWrite. Declining is free -- the other owner is placing an interrupt
       right now, so this line simply takes the next opportunity. */
    if (InterlockedCompareExchange(&g_AsyncContextWrite, 1, 0) != 0) { AsyncEarlyBail(irq, ASYNC_WHY_CTX_BUSY); return 0; }
    if (SuspendThread(g_HostCpu) == (DWORD)-1) { ASYNC_CTX_RELEASE(); AsyncEarlyBail(irq, ASYNC_WHY_SUSPEND_FAIL); return 0; }
    { UINT index; PSTR bytes = (PSTR)&context; for (index = 0; index < sizeof context; ++index) bytes[index] = 0; }
    context.ContextFlags = CONTEXT_CONTROL | CONTEXT_SEGMENTS;
    if (!GetThreadContext(g_HostCpu, &context)) { ResumeThread(g_HostCpu); ASYNC_CTX_RELEASE();
                                          AsyncEarlyBail(irq, ASYNC_WHY_GETCTX_FAIL); return 0; }
    /* ── ⚠⚠ RE-READ g_InExec NOW THAT THE SUSPEND HAS LANDED. ────────────────────
         The check at the top of this function is a sample: the guest can trap between
         it and the suspend taking effect, leaving the thread inside HOST code -- and
         possibly holding g_Lock -- while we hold it frozen. Callers that already hold
         g_Lock are immune (a thread blocked on it cannot be inside it), but the tick
         courier holds nothing, so the guarantee has to be re-established here.
         GetThreadContext above is what makes "the suspend has landed" true: on a
         multiprocessor SuspendThread only REQUESTS it, and reading the context is the
         documented way to wait. The exec loop clears g_InExec immediately on return
         from VdmRunGuest, BEFORE any HOST_LOCK, so a 1 observed on an already-stopped
         thread means it is still inside the kernel call, holding nothing.
       ► Resuming instantly on a 0 is the correct trade: a microsecond probe of a
         thread that MIGHT hold a lock is harmless, holding one is the catastrophe. */
    if (g_InExec == 0) { ResumeThread(g_HostCpu); ASYNC_CTX_RELEASE();
                          AsyncEarlyBail(irq, ASYNC_WHY_LEFT_EXEC); return 0; }

    eflags = context.EFlags;
    cs  = context.SegCs & WORD_MASK;
    /* ── THE OBSERVATION PASS. See AsyncSiteNew(). Protected mode only: in V86 the
         guest's EIP wanders over the whole real-mode image and would fill the table with
         noise, burying the one site this exists to catch. */
    if (!(eflags & EFLAGS_VM) && (cs & DPMI_SELECTOR_TI) && AsyncSiteNew((WORD)cs, context.Eip)) {
        CHAR lineBuffer[160], *lineCursor = lineBuffer;
        DWORD codeLinear = DpmiSelectorBase((WORD)cs) + context.Eip;
        ResumeThread(g_HostCpu);                    /* NEVER log while the guest is held */
        ASYNC_CTX_RELEASE();
        lineCursor = LogPut(lineCursor, "ASYNC-SITE #"); lineCursor = LogHex(lineCursor, (DWORD)g_AsyncSiteCount);
        lineCursor = LogPut(lineCursor, " irq="); lineCursor = LogHex(lineCursor, irq);
        lineCursor = LogPut(lineCursor, " cs:eip=0x"); lineCursor = LogHex(lineCursor, cs);
        lineCursor = LogPut(lineCursor, ":0x"); lineCursor = LogHex(lineCursor, context.Eip);
        lineCursor = LogPut(lineCursor, " lin=0x"); lineCursor = LogHex(lineCursor, codeLinear);
        lineCursor = LogPut(lineCursor, " ss:esp=0x"); lineCursor = LogHex(lineCursor, context.SegSs & WORD_MASK);
        lineCursor = LogPut(lineCursor, ":0x"); lineCursor = LogHex(lineCursor, context.Esp);
        lineCursor = LogPut(lineCursor, " efl=0x"); lineCursor = LogHex(lineCursor, eflags);
        lineCursor = LogPut(lineCursor, " ms="); lineCursor = LogHex(lineCursor, GetTickCount());
        lineCursor = LogPut(lineCursor, "\r\n"); LogAppend(LOG_PATH, lineBuffer, lineCursor); SerialOut(lineBuffer, lineCursor);
        AsyncWhyNote(irq, ASYNC_WHY_OBSERVED);                 /* accounted for, so the histogram sums */
        return 0;                                /* observed only -- the next tick injects */
    }
    /* ► PROTECTED MODE IS A DIFFERENT FRAME AND A DIFFERENT VECTOR TABLE, so it gets its
         own arm rather than a widened condition. The test for "is this the guest at all"
         is the selector's TABLE INDICATOR: everything we hand the client comes out of our
         LDT (TI=1, bit 2 set), and the host's own flat CS is a GDT selector. So `cs & 4`
         separates "the thread is executing client PM code" from "the thread is in our own
         code between entries" exactly, with nothing to keep in sync. */
    if (!(eflags & EFLAGS_VM)) {
        g_AsyncWhy = ASYNC_DELIVERED;
        /* ► TWO EXITS USED TO LEAVE why=0, WHICH IS THE CODE FOR SUCCESS. A thread found
             in PM on a GDT selector (we are inside the HOST, not the client) and a failed
             SetThreadContext both returned ok=0 with why untouched, so a histogram keyed
             on it would have booked them as deliveries. Give each its own code: 14 is the
             one to watch, because "the CPU thread was in host code when the clock asked"
             is exactly what g_Lock starvation looks like from this side. */
        if (!(cs & DPMI_SELECTOR_TI)) AsyncWhyNote(irq, ASYNC_WHY_HOST_CS);
        else if (DpmiAsyncInjectPm(irq, &context)) {
            context.ContextFlags = CONTEXT_CONTROL | CONTEXT_SEGMENTS;
            isOk = SetThreadContext(g_HostCpu, &context) ? 1 : 0;
            /* Same acknowledge as the V86 arm below: IRQ0 in service until the guest
               EOIs (#173), a stub-vectored line released at once. */
            if (isOk) { if (irq == 0)                        Irq0Ack();
                      else if (AsyncVectorIsOurStub(irq)) VddPicAcknowledgeAutoEoi(&g_Pic, (BYTE)irq);
                      else                                 VddPicAcknowledge(&g_Pic, (BYTE)irq); }
            else    { g_AsyncPmActive = 0; }     /* never leave the flag set on failure */
            AsyncWhyNote(irq, isOk ? ASYNC_DELIVERED : ASYNC_WHY_SETCTX_FAIL);
        }
        else AsyncWhyNote(irq, (UINT)g_AsyncWhy);   /* the clause that said no */
        ResumeThread(g_HostCpu);
        ASYNC_CTX_RELEASE();
        if (isOk) { g_AsyncInjected++; g_AsyncPmInjected++; g_AsyncInjectedLine[irq & (PIC_LINES - 1)]++;
                  if (!(irq & (PIC_LINES_PER_CHIP - 1))) TickDeliveredNote(); } else g_AsyncBail++;
        /* Log AFTER the resume, never while the guest is held -- and bounded, because this
           fires at the PIT's rate. Without it an async injection that kills the run is
           completely silent: the cooperative path prints its entry and exit, so a log that
           simply STOPS after a clean tick points here by elimination, which is not the same
           as evidence. */
        /* ► LOG EVERY SUCCESSFUL INJECTION, CAP ONLY THE BAILS. The 40-entry cap
             counted bails and successes together, and the bails (why=9, the arm
             hold-off) burn it long before the interesting part: in a Doom run the
             last logged async sits at line 42024 of 55124, so the injections around
             the death were invisible. Every logged one interrupts the guest at
             0x03ae53dc -- the millisecond-delay spin -- which is the SAFE case. The
             question is whether a later one lands somewhere else, e.g. inside
             R_ExecuteSetViewSize's long arithmetic, which is the only stretch where
             the guest runs thousands of instructions with no BOP. Successes are rare
             (one per delivered tick at most), so this is not a firehose. */
        /* ► AND THE BAIL CAP IS 4000, NOT 40, FOR THE SAME REASON ONE LEVEL DOWN.
             Raising the SUCCESS logging was not enough: the why=9 arm hold-off burns a
             40-entry bail budget in the first second, so by the time the guest reaches
             R_ExecuteSetViewSize every bail is silent too -- and "no async line near the
             death" then means "we stopped looking", not "nothing was attempted". That is
             the difference between evidence and an unread instrument. Bails fire at the
             PIT's rate (~100/s) against a 45s headless cap, so 4000 covers a whole run
             with ~900KB of log, well inside LOG_MAX_BYTES. */
        if (isOk || g_AsyncPmBail2 <= 4000) {
            CHAR lineBuffer[224], *lineCursor = lineBuffer;
            /* ► SAY WHICH LINE. This said "vec=0x08" literally, whatever interrupt it
                 had just delivered, so a run could not be asked "did any keyboard
                 interrupt reach the client?" -- every line claimed to be the timer. */
            lineCursor = LogPut(lineCursor, "ASYNC-PM vec=0x"); lineCursor = LogHexByte(lineCursor, IrqPmVector(irq));
            lineCursor = LogPut(lineCursor, " ok=0x"); lineCursor = LogHex(lineCursor, (DWORD)isOk);
            lineCursor = LogPut(lineCursor, " why="); lineCursor = LogHex(lineCursor, (DWORD)g_AsyncWhy);
            lineCursor = LogPut(lineCursor, " from=0x");   lineCursor = LogHex(lineCursor, cs);
            lineCursor = LogPut(lineCursor, ":0x");        lineCursor = LogHex(lineCursor, g_AsyncPmEip);
            lineCursor = LogPut(lineCursor, " -> 0x");     lineCursor = LogHex(lineCursor, (DWORD)DPMI_IRQ_TARGET_SEL(IrqPmVector(irq)));
            lineCursor = LogPut(lineCursor, ":0x");        lineCursor = LogHex(lineCursor, DPMI_IRQ_TARGET_OFF(IrqPmVector(irq)));
            lineCursor = LogPut(lineCursor, " SS:ESP=0x"); lineCursor = LogHex(lineCursor, (DWORD)g_AsyncPmSs);
            lineCursor = LogPut(lineCursor, ":0x");        lineCursor = LogHex(lineCursor, g_AsyncPmEsp);
            lineCursor = LogPut(lineCursor, " efl=0x");    lineCursor = LogHex(lineCursor, g_AsyncPmEflags);
            /* ── ★ WHAT CODE WAS INTERRUPTED. A timer injection reports WHERE the guest
                 was; when the guest is STUCK, where it was is the whole question, and the
                 address alone cannot answer it -- a spin on a memory flag, a spin on a
                 port and a spin waiting for an interrupt all look identical as a number.
                 Sixteen bytes at the interrupted EIP can be disassembled, so "it is
                 looping around 0x03b71317" becomes "it is polling X".
                 (ZAR, GH #23: it reaches "Game loading..." and then spins.) */
            {   DWORD instructionBase = DpmiSelectorBase((WORD)cs) + g_AsyncPmEip;
                const volatile BYTE *ip = (const volatile BYTE *)(ULONG_PTR)instructionBase;
                lineCursor = LogPut(lineCursor, " code@eip=");
                if (MemoryReadable((ULONG_PTR)instructionBase, 16)) lineCursor = LogDump(lineCursor, (const VOID *)ip, 16);
                else                                 lineCursor = LogPut(lineCursor, "<unreadable>"); }
            lineCursor = LogPut(lineCursor, "\r\n"); LogAppend(LOG_PATH, lineBuffer, lineCursor); SerialOut(lineBuffer, lineCursor);
            if (!isOk) g_AsyncPmBail2++;
        }
        return isOk;
    }
    /* ► THIS ARM USED TO RETURN IN SILENCE, AND THAT SILENCE WAS READ AS EVIDENCE.
         Session 20 recorded "ZERO async attempts of ANY kind across the death window"
         and struck the injector off the suspect list. But Doom is doing real-mode file
         I/O right up to the death (INT 31h 0302 -> callRM), so the CPU is in V86 for
         most of those attempts and every one of them left here without a word. Give it
         a why code like every other exit; AsyncEarlyBail's cap keeps it bounded. */
    /* ── IN A LIVE V86 FRAME, IF IS NOT THE GUEST'S. VIF IS. (s81, irq8.nested) ─────────
         Measured with the census (IfvNote): every live V86 frame this function has read
         has IF=1, including those inside a handler that has not executed STI -- the
         kernel keeps the real IF on under VME, and reads back IF=1 even straight after we
         SetThreadContext it clear. So "IF or VIF" here was "always". What that cost was
         not only the re-entry itself: a tick let in while VIF was clear pushes a FLAGS
         image with IF=1 (below), and its IRET then turns the interrupted handler's
         interrupts ON -- which is how p_irq8's INT 70h handler was re-entered 4 times
         against 0 on three oracles (the re-entries themselves came through the
         cooperative path, reading an honest VTIB that had been made wrong).
         So VIF alone decides -- once a live frame has shown VIF set at all, which is the
         proof that VME is maintaining it. Before that (or on a CPU without VME, where VIF
         is never set and never means anything) the old test stands, so nothing starves.
         The fear that parked this -- a guest logically on with VIF clear -- did not show:
         Skyroads' live frames had VIF set every time, and ZAR's and Doom's real-mode
         stretches never reach this gate (census live{} empty for both). */
    IfvNote(IFV_PATH_LIVE, eflags);
    if (eflags & EFLAGS_VIF) g_VifLiveSeen = 1;
    if (!(eflags & (g_VifLiveSeen ? EFLAGS_VIF : (EFLAGS_IF_U | EFLAGS_VIF)))
        || cs == DOS_HDLR_SEG) {
        ResumeThread(g_HostCpu);
        ASYNC_CTX_RELEASE();
        AsyncEarlyBail(irq, (cs == DOS_HDLR_SEG) ? ASYNC_WHY_IN_OUR_HANDLER : ASYNC_WHY_V86_IF_OFF);
        return 0;
    }
    /* s92 (#239): a line only a PROTECTED-MODE handler wants is not delivered into V86
         through the IVT -- whose entry is our IRET stub, where it would be acknowledged
         and lost. Left pending (return 0) for the PM path; see V86DeliverDeviceIrq. */
    if (irq >= ASYNC_FIRST_DEVICE_IRQ && g_DpmiPm && g_PmInt[IrqPmVector(irq)].Client) {
        UINT vector1 = VddPicVector(&g_Pic, (BYTE)irq);
        if (PeekWord(IVT_SEGMENT_ADDRESS(vector1)) == DOS_HDLR_SEG && PeekWord(IVT_OFFSET_ADDRESS(vector1)) == DOS_IRET_STUB_OFF) {
            ResumeThread(g_HostCpu);
            ASYNC_CTX_RELEASE();
            AsyncEarlyBail(irq, ASYNC_WHY_PM_ONLY_LINE);
            return 0;
        }
    }
    ss = context.SegSs & WORD_MASK; sp = context.Esp & WORD_MASK; ip = context.Eip & WORD_MASK;

    flags = (WORD)eflags;
    if (eflags & EFLAGS_VIF) flags |= EFLAGS_IF;      /* same VIF fold as InjectInt */
    sp = (sp - X86_WORD_SIZE) & WORD_MASK; PokeWord((ss << PARAGRAPH_SHIFT) + sp, flags);
    sp = (sp - X86_WORD_SIZE) & WORD_MASK; PokeWord((ss << PARAGRAPH_SHIFT) + sp, (WORD)cs);
    sp = (sp - X86_WORD_SIZE) & WORD_MASK; PokeWord((ss << PARAGRAPH_SHIFT) + sp, (WORD)ip);
    context.Esp    = sp;
    { UINT vector = VddPicVector(&g_Pic, (BYTE)irq);
      context.Eip   = PeekWord(IVT_OFFSET_ADDRESS(vector));
      context.SegCs = PeekWord(IVT_SEGMENT_ADDRESS(vector)); }
    context.EFlags = eflags & ~(EFLAGS_TF_U | EFLAGS_IF_U | EFLAGS_VIF);
    context.ContextFlags = CONTEXT_CONTROL | CONTEXT_SEGMENTS;
    isOk = SetThreadContext(g_HostCpu, &context) ? 1 : 0;
    if (isOk && !(eflags & EFLAGS_VIF)) ++g_IfvShadow[irq & (PIC_LINES - 1)];   /* see IfvNote */
    if (isOk) IfvTrace(irq, IFV_PATH_LIVE, eflags, cs, ip);
    if (isOk) {
        /* Acknowledge: in service until the guest EOIs.
           Release it immediately for a line vectored at one of OUR default stubs, which
           do nothing and never EOI.
           ⚠ s70: THE TIMER USED TO BE THE OTHER EXCEPTION, and it is not any more. The
             ba927ac measurement ("Skyroads EOIs only ~36 times a second against a 180 Hz
             timer ... modelling IRQ0's in-service bit strictly starves it, 540 -> 15
             ticks per 3 s") was taken before our INT 08h BOP sent the BIOS's EOI; with
             that EOI in place, strict IRQ0 delivers every raise on Skyroads (s70 rig:
             raises == strict acks, 0 blocked, 0 timeouts, V86STR/ui_gap on baseline).
             And "gated by the guest's own IF discipline" was the bug: Lemmings' timer
             ISR does `sti` before it spins for the retrace. See Irq0Ack. */
        /* ⚠ THE AUTO-EOI CASE IS ONE OPERATION, NOT ack-then-eoi. The pair's transient
             set/clear of the shared ISR byte is not safe from the tick courier, which
             runs without the device lock; the net effect is identical. See
             VddPicAcknowledgeAutoEoi. The strict case (IRQ1) keeps plain acknowledge, so its
             in-service bit is still held until the guest EOIs. */
        if (irq == 0)                     Irq0Ack();
        else if (AsyncVectorIsOurStub(irq)) VddPicAcknowledgeAutoEoi(&g_Pic, (BYTE)irq);
        else                              VddPicAcknowledge(&g_Pic, (BYTE)irq);
        /* ⚠ A KEY DELIVERED HERE WAS INVISIBLE. g_Irq1Injected and KeyLatencyPop() both live in
           the COOPERATIVE block only, so an asynchronously-placed keystroke counted as
           neither delivered nor timed -- and the retry experiment therefore read as
           "places 78 of 186 interrupts, loses keys" when it had in fact placed all 186
           (78 cooperative + 108 here) and the histogram was measuring only the slow
           leftovers. Time it where it happens. */
        if (irq == 1) { ++g_Irq1AsyncInjected; KeyLatencyPop(); }
    }
    ResumeThread(g_HostCpu);
    ASYNC_CTX_RELEASE();
    if (isOk) g_AsyncInjected++; else g_AsyncBail++;
    AsyncWhyNote(irq, isOk ? ASYNC_DELIVERED : ASYNC_WHY_SETCTX_FAIL);     /* the V86 arm's only failure is SetThreadContext */
    /* Log AFTER the resume (never hold the guest suspended across file I/O). The IVT dump
       is the point: vectoring an IRQ the guest never hooked lands it in unowned ROM, which
       is exactly what happened first time out -- Skyroads ended up at F000:A390. Printing
       the whole IRQ3-7 vector range shows which line the game is actually listening on. */
    if (g_AsyncInjected + g_AsyncBail <= 4) {
        CHAR lineBuffer[256], *lineCursor = lineBuffer; INT logVector;
        lineCursor = LogPut(lineCursor, "ASYNC-INJ vec=0x");  lineCursor = LogHex(lineCursor, (DWORD)VddPicVector(&g_Pic, (BYTE)irq));
        lineCursor = LogPut(lineCursor, " ok=0x");            lineCursor = LogHex(lineCursor, (DWORD)isOk);
        lineCursor = LogPut(lineCursor, " from=0x");          lineCursor = LogHex(lineCursor, cs);
        lineCursor = LogPut(lineCursor, ":0x");               lineCursor = LogHex(lineCursor, ip);
        lineCursor = LogPut(lineCursor, " ivt[0B..0F]=");
        for (logVector = VECTOR_IRQ3; logVector <= VECTOR_IRQ7; ++logVector) {
            lineCursor = LogPut(lineCursor, "0x");  lineCursor = LogHex(lineCursor, PeekWord(IVT_SEGMENT_ADDRESS(logVector)));
            lineCursor = LogPut(lineCursor, ":0x"); lineCursor = LogHex(lineCursor, PeekWord(IVT_OFFSET_ADDRESS(logVector)));
            lineCursor = LogPut(lineCursor, " ");
        }
        lineCursor = LogPut(lineCursor, "\r\n");
        LogAppend(LOG_PATH, lineBuffer, lineCursor); SerialOut(lineBuffer, lineCursor);
    }
    return isOk;
}

INT AsyncVectorIsOurStub(UINT irq)
{
    UINT vector = VddPicVector(&g_Pic, (BYTE)irq);
    WORD segment = PeekWord(IVT_SEGMENT_ADDRESS(vector)), offset = PeekWord(IVT_OFFSET_ADDRESS(vector));
    return segment == DOS_HDLR_SEG && (offset == DOS_IRET_STUB_OFF || offset == DOS_HDLR_INT09_STUB_OFF);
}

VOID HostIrqSink(PVOID context, BYTE irq)
{
    (VOID)context;
    /* Every raise, counted by line. sb_blocks reached 1 while irqn_inj AND irqn_refused
       both stayed 0 -- i.e. the SB's completion IRQ was raised but nothing was ever
       latched -- so the line number this arrives on is the missing fact. */
    g_IrqRaised[irq & (PIC_LINES - 1)]++;
    g_IrqRaisedAny++;
    /* ⚠ ATOMICALLY: the timer's raise now arrives under g_PitCs while every other
       PIC mutation runs under g_Lock, and IRR |= bit compiled as a plain byte RMW
       could undo a concurrent acknowledge on a DIFFERENT line. One atomic OR closes
       it; the slave (irq >= 8) keeps the plain path, nothing raises it cross-lock. */
    if (irq < PIC_LINES_PER_CHIP) __sync_fetch_and_or(&g_Pic.Master.Irr, (BYTE)(1u << irq));
    else         VddPicRaise(&g_Pic, irq);
    if (irq == 0) {
        Irq0Latch();
        ++g_Irq0RaiseCount;          /* A/B/C discriminator: generation. See Irq0DeliveredNote. */
        /* THE TIMER NEEDS THE ASYNC PATH TOO -- arguably more than the devices do. A game
           that parks in its own handler stops trapping, so the exec loop never gets a turn
           and the tick it is waiting for can never arrive. A real PC interrupts it regardless.
           NO RATE LIMIT: this sink is now called by the PIT VDD at exactly the rate the GUEST
           programmed into channel 0, so limiting it here would override the guest. The 55 ms
           limit that used to be here did precisely that -- it pinned the timer to 18 Hz while
           Skyroads was asking for 180, which is most of why the game ran ~10x too slow and
           why its intro stalled after ~3 s (measured: d_irq0 fell to ~1/s while the guest sat
           at 0110:3b40 waiting, having done all its work in the first three seconds). */
        /* ── ONE ASYNCHRONOUS ATTEMPT PER SYNC, NOT ONE PER TICK RAISED. ─────────────
             VddPitAddClocks() raises IRQ0 once per reload period for the whole
             elapsed gap, synchronously, from inside HostPitSync's lock. Doom's music
             driver programs the 8254 fast (reload 0x4a = 16 kHz, measured), so a 50 ms
             gap is EIGHT HUNDRED raises -- and this used to answer every one of them
             with a SuspendThread / GetThreadContext / SetThreadContext / ResumeThread
             round trip plus a log write, all still holding the device lock.
             Measured on a ten-minute play session: a 44 ms lock hold attributed to
             HostPitSync, with the AUDIO thread blocked 36.8 ms behind it waiting to
             mix. That is the user's "chk-a-chk-a" in one number, and it is ours.
             Only ONE of those attempts can ever deliver anything -- the rest bail on
             g_AsyncPmActive because an injection is already in flight -- and they pay
             the full round trip to find out. The backlog is not lost by skipping them:
             g_PmTickOwed counts every raise and the catch-up batch drains it.
             After: hold 44ms -> 15.9ms, audio-thread wait 36.8ms -> 5.8ms, longest UI
             gap 61ms -> 31.8ms, with the delivered tick rate unchanged. */
        /* ── ...BUT ONE ATTEMPT PER SYNC IS A CEILING, AND WE ARE UNDER IT. ──────────
             The cap above is right about the COST and wrong about the BUDGET. Measured:
                 raises 144/s   syncs 65/s   attempts 62/s   delivered 56/s
                 owed_max = 64 = PM_TICK_OWED_MAX, i.e. the backlog is SATURATED
             The 8254 generates every one of Doom's 140 ticks; they die at HostPitSync,
             which runs 65 times a second rather than the UI thread's intended 200 because
             it takes g_Lock and Doom's mode-Y drawing does ~43,000 port writes a second
             through the same lock. One attempt per sync then caps delivery at 65/s -- and
             DMX mixes PCM in the timer ISR, so the missing ticks ARE the missing refills
             and the 186 ms echo.
           ► SO SPEND MORE ONLY WHEN THE BACKLOG SAYS IT IS WORTH IT, AND STOP AT THE
             FIRST REFUSAL. Session 22's disaster was ~800 unbounded attempts per sync,
             each a full SuspendThread round trip under the lock; this is at most
             PIT_ASYNC_PER_SYNC, only while ticks are actually owed, and it gives up the
             instant one declines -- because if the guest is not in an injectable spot now
             it will not be three microseconds from now, and the rest of the burst is pure
             round-trip cost. That is the distinction the old cap could not express. */
        /* ── ...AND RAISING THAT BUDGET WAS TRIED, MEASURED, AND REVERTED. ───────────
             The reasoning was: one attempt per sync x 65 syncs/s caps delivery at 65/s
             against Doom's 144 raises/s, the backlog is saturated, so buy more attempts
             when it is deep (up to 4, stopping at the first refusal). It does not work,
             and the numbers are unambiguous:
                                attempts/s   delivered/s   ui_gap_us   lock hold
                 one per sync           62            56      24,415      ~15 ms
                 up to four            189            55     260,009      188 ms
             **ATTEMPTS TRIPLED AND DELIVERY DID NOT MOVE.** So the ceiling was never the
             attempt budget: it is how often the guest is in an INJECTABLE STATE, and
             extra attempts merely pay full SuspendThread round trips under g_Lock to be
             told no -- which is session 22's lesson arriving from a new direction, and a
             10x worse UI gap for nothing.
           ▶ WHAT THIS RULES OUT, AND WHERE TO GO. Do not spend effort on the delivery
             RATE; spend it on the guest's INJECTABILITY, or bypass the async path for
             the timer entirely. `AsyncInjectIrq` refuses on g_AsyncPmActive (an
             injection still in flight), on VddPicCanDeliver, and on the client's
             virtual-IF -- instrument WHICH of those says no at 144 Hz before changing
             anything else. Note that the injectable window may simply be scarce while
             Doom holds its own ISR, in which case the answer is the cooperative PM-loop
             path (which needs no suspend at all), not the asynchronous one. */
        /* ── ⚠ ONE ATTEMPT PER SYNC IS RIGHT FOR A PM CLIENT AND WRONG FOR A V86 GUEST.
             The throttle above was introduced for DOOM, whose music driver programs the
             8254 at 16 kHz: VddPitAddClocks then raises 800 times for a single 50 ms
             catch-up gap, each answered with a full SuspendThread round trip inside this
             lock. That pathology is real and the throttle fixes it.
             But it was applied to every guest, and SKYROADS -- V86, an 180 Hz timer, at
             most a raise or two per sync, so the burst it guards against cannot occur --
             lost a fifth of its clock to it. BISECTED to e2f7486 against an Aug-21
             reference, then confirmed by disabling the throttle alone (30 s cap each):
                 session 21 (c740f4e)   irq0_inj 4485    <- and the Aug-21 log says 4487
                 141f347                irq0_inj 4588
                 07835a5                irq0_inj 4505
                 e2f7486                irq0_inj 3413    <- the throttle lands here
                 HEAD, throttle off     irq0_inj 4537    <- restored
             The player heard this as an OPL and graphics timing fault on a title that
             had been fully playable since session 19, and no instrument reported it:
             every counter this host prints was inside its normal range.
           ► SO SCOPE THE THROTTLE TO WHAT IT WAS MEASURED ON. A protected-mode client
             keeps the exact behaviour session 22 measured and session 23 tuned -- not
             one attempt more -- and the V86 path goes back to what it did before, which
             is the behaviour every V86 measurement in this project was taken against.
           ⚠ A V86 guest that programs a Doom-like timer rate would be exposed to the
             800-raise burst again. None that we run does (Skyroads 180 Hz is the
             fastest measured), and the honest fix if one appears is to bound the BURST
             -- attempts per sync, not per guest -- rather than to widen this back. */
        /* ── ★★★ THE DELIVERY ATTEMPT NO LONGER LIVES HERE. (s61 restructure) ────────
             This sink now runs under g_PitCs -- the crystal's own micro-lock -- so a
             raise may only LATCH: the PIC request bit, the pending counter, the
             courier signal. Anything slower would put the renderer back between the
             crystal and the next tick, which is the measured 86%% failure this
             restructure exists to kill. The one asynchronous attempt per sync
             happens in HostPitDeliver(), after the crystal is released -- still
             strictly under g_Lock (the never-suspend-a-lock-holder interlock), but
             acquired with a TRY so the clock never queues for it. The attempt's
             full history (session 22's 800-round-trip disaster, session 23's budget
             experiment, the keyboard yield and its refuted variants) moved with the
             code. */
        /* ── ★ HAND ANY STILL-PENDING TICK TO THE COURIER. The one attempt above is
             all this site can afford (see the long note), and it is exactly the
             attempt a pending KEY takes for itself. Signalling costs a SetEvent on an
             already-signalled auto-reset event in the common case, which is a few
             hundred nanoseconds -- affordable even here, inside the lock, at the PIT's
             rate. The courier does the waiting, outside the lock, on its own thread. */
        if (g_CourierEvent && g_Irq0Pending > 0) SetEvent(g_CourierEvent);
    }
    else if (irq == 1) {
        /* One pending interrupt at a time: the 8042 has a single output buffer, and the
           input VDD now re-raises as the guest drains it, so this can never legitimately
           run ahead. (The earlier cap-with-a-backlog is what stranded break codes and
           killed the arrow keys -- see VddInputPushScanCode.) */
        if (g_Irq1Pending < 1) InterlockedIncrement(&g_Irq1Pending);
        /* ── THREE FIXES FOR THE V86 KEY-DELIVERY LAG, ALL MEASURED, ALL REFUTED. ──────
             Symptom (headless repro: tests/probes/dos/skyroads-play.keys, pacer on):
                 baseline pacer ON   n=102  >=64ms = 44 (43%)  max  684 ms  inj=186
                 baseline pacer OFF  n=102  >=64ms =  0 ( 0%)  max   52 ms  inj=186
             The guest has interrupts off ~96% of the time in gameplay, so IRQ0 and IRQ1
             compete for a scarce supply of interrupts-enabled moments, and the timer wins.
             What does NOT work -- do not spend another round on these:
               1. ASYNC-INJECT IRQ1 at the raise site, taking the IRQ0 opportunity (which
                  AsyncInjectIrq has already verified as IF-set):
                      n=50  >=64ms = 12 (24%)  max 2552 ms  inj=78  retries=108
                  Faster for keys that land, but only 78 of 186 interrupts are placed --
                  it LOSES KEYS. Suspect the IRQ1 in-service bit never clearing (we do not
                  auto-EOI a line vectored at the game's own handler).
               2. YIELD the async IRQ0 opportunity when a key is pending, so the guest's
                  enabled window survives for the cooperative path:
                      n=37  >=64ms = 1 (3%)  max 1468 ms  inj=66  yields=2218
                  Also loses keys, and no_if stayed at 96% -- which REFUTES the premise
                  that our own injections are what keep the guest's interrupts off.
               3. DEFER the cooperative IRQ0 so the keyboard block below gets the pass:
                      n=102  >=64ms = 32 (31%)  max 5320 ms  inj=185  defers=14
                  Harmless but pointless: with the pacer running, IRQ0 is delivered on the
                  ASYNCHRONOUS path, so the cooperative block has almost nothing to yield.
             ► The unexplored direction is the PIC: every attempt that placed IRQ1 early
               also lost later ones, which smells like in-service never being released.
               Measure vdd_pic in-service for line 1 across a run BEFORE trying again. */
        /* Keys deliberately do NOT take the async path by default (qimode bit 7 turns it
           on for experiments). Async keyboard delivery is what turned "playable" into
           "dies as soon as you press a key", and while the PIC stopped the re-entry it did
           not stop that: with a faithful held-arrow probe the game still degrades to a
           stop. Until that is understood, keys go back to the exec-loop path they used
           when input merely felt laggy -- a known-good behaviour beats an unexplained one.
           The TIMER keeps async delivery, which is what makes the game playable at all.
           ► ...BUT A PROTECTED-MODE CLIENT HAS NO OTHER PATH. That reasoning is about the
             V86 exec loop, which drains g_Irq1Pending when the guest traps. There is no
             equivalent for a DPMI client: the only cooperative injection the PM loop does
             is IRQ0, so a key raised while Doom is inside its own game loop is simply
             never delivered and the guest never sees a keystroke at all. When the client
             has installed a protected-mode INT 09h handler, asynchronous delivery is not
             a preference, it is the mechanism. */
        { INT gate = (g_QiKeysAsync || (g_DpmiPm && g_PmInt[VECTOR_KEYBOARD].Client));
          INT isOk   = gate ? AsyncInjectIrq(PIC_IRQ_KEYBOARD) : 0;
          if (isOk) InterlockedDecrement(&g_Irq1Pending);
          /* ► EVERY KEYBOARD INTERRUPT, ACCOUNTED FOR, FOR THE FIRST FEW DOZEN. A key
               press is a rare, deliberate event -- there is no firehose to guard against
               -- and "the guest never saw my keystroke" has at least four different
               causes between here and the client's ISR. Naming the gate values at the
               moment of the raise turns that into one line. */
          if (g_KeyIrqLogged++ < 64) {
              CHAR lineBuffer[160], *lineCursor = lineBuffer;
              lineCursor = LogPut(lineCursor, "KEYIRQ raise gate="); lineCursor = LogHex(lineCursor, (DWORD)gate);
              lineCursor = LogPut(lineCursor, " ok=");        lineCursor = LogHex(lineCursor, (DWORD)isOk);
              lineCursor = LogPut(lineCursor, " pm=");        lineCursor = LogHex(lineCursor, (DWORD)g_DpmiPm);
              lineCursor = LogPut(lineCursor, " pmhook=");    lineCursor = LogHex(lineCursor, (DWORD)g_PmInt[0x09].Client);
              lineCursor = LogPut(lineCursor, " in_exec=");   lineCursor = LogHex(lineCursor, (DWORD)g_InExec);
              lineCursor = LogPut(lineCursor, " why=");       lineCursor = LogHex(lineCursor, (DWORD)g_AsyncWhy);
              lineCursor = LogPut(lineCursor, " ms=");        lineCursor = LogHex(lineCursor, GetTickCount());
              lineCursor = LogPut(lineCursor, "\r\n"); LogAppend(LOG_PATH, lineBuffer, lineCursor); SerialOut(lineBuffer, lineCursor);
          } }
    }
    else if (irq < PIC_LINES) {
        InterlockedExchange(&g_IrqNPending[irq], 1);
        /* A device IRQ is raised from the AUDIO thread, while the guest may be
           spinning in V86 code that never faults -- and our exec loop only gets a
           turn when the guest traps, so the interrupt would never be injected.
           Setting the kernel's FIXED_NTVDMSTATE hardware-interrupt-pending bit is
           how a VDM asks to be preempted: the kernel breaks out of V86 execution
           and hands us event 3, which the exec loop already services. Without
           this an SB block completes, the IRQ is latched, and the game waits
           forever inside its own handler.
           SESSION 10 MEASURED THAT THIS IS NOT ENOUGH: the word is only consulted at
           the kernel's own transition points, so a guest spinning in V86 never notices
           (intpend stayed 1, irqn_inj stayed 0). Session 11's RE found the missing
           half -- NtVdmControl(VdmQueueInterrupt, thread) queues an APC that forces
           the thread out of V86 so those bits are read. Gated on QIMODE_PATH until the
           rig says which of the kernel's two delivery paths we land on. */
        /* ...AND SETTING IT UNCONDITIONALLY IS WORSE THAN USELESS (measured, session 11,
           qirq.com on the rig): VDM_INT_HARDWARE tells the kernel a hardware interrupt
           is pending and to dispatch it through its own virtual ICA -- which we have
           never programmed (v86.c registers zeroed buffers). On VME hardware the kernel
           then sets EFLAGS.VIP and the guest's next IRET/STI faults into a dispatch that
           finds nothing, forever: the guest froze at DOS_HDLR_SEG:0x0003 with the exec
           loop starved (io=0, irq0 stuck at 1) in every run that set the bit, including
           the control that made no queue call. That is almost certainly the same freeze
           session 10 recorded for Skyroads at 0x0050:0x0037. So the bit is now set only
           when an experiment asks for it; the ICA must be programmed before this can be
           the real delivery path. */
        if (g_QiBits && irq < PIC_LINES_PER_CHIP) {
            /* The kernel dispatches from its virtual PIC, so request the line there
               first -- otherwise the APC wakes up, finds nothing requested, and the
               pending bit just sits there (which is the whole of session 10's
               "already tried and failed"). */
            if (g_QiBits & 1) VdmIcaRaise(irq);
            *(volatile DWORD *)(ULONG_PTR)FIXED_NTVDMSTATE_LINEAR |= g_QiBits;
        }
        if (g_QiBits && g_HostCpu) {
            LONG status = VdmControl(VDM_SVC_VdmQueueInterrupt, (PVOID)g_HostCpu);
            InterlockedExchange(&g_QiStatus, status);
            g_QiCalls++;
        }
        if (g_QiSuspended && AsyncInjectIrq(irq))
            InterlockedExchange(&g_IrqNPending[irq], 0);  /* delivered; don't double-inject */
    }
}


/* The guest's effective interrupt-enable flag. When we regain control inside one
   of our own BOP stubs the LIVE IF is the stub's (the CD nn that vectored in
   cleared it), so the guest's real IF is the FLAGS the stub will IRET to. */
/* "Are the guest's interrupts enabled?" -- and on VME hardware that is TWO bits, which
   is what kept the Sound Blaster's IRQ from ever being injected. Virtual Mode Extensions
   are enabled for our VDM (proven: the kernel sets EFLAGS.VIP in our guest's frame, a
   branch it only takes when KeI386VirtualIntExtensions has V86 VME on), and under VME a
   V86 guest's CLI/STI do not touch IF at all -- they manipulate the VIRTUAL interrupt
   flag, VIF (bit 19). So a game that enables interrupts by executing STI, rather than by
   inheriting IF=1 from the entry EFLAGS we set, reads as "interrupts disabled" to a gate
   that only looks at IF -- forever. Skyroads does exactly that inside its INT 1Ch
   handler. A 16-bit FLAGS image pushed on the guest stack is unaffected: VME pushes the
   virtual flag into the IF bit position. */
INT IfOrVif(DWORD flags) { return (flags & (EFLAGS_IF_U | EFLAGS_VIF)) != 0; }
/* ── #206: OUR BIOS STUBS ARE OUR STUBS TOO. ──────────────────────────────────────────
     Inside one of our INT stubs the live IF is the stub's -- the INT that vectored in
     cleared it -- and the guest's own is the FLAGS the stub will IRET to, at SS:SP+4.
     That was honoured for DOS_HDLR_SEG only; the BIOS stubs (INT 11h-29h, at
     DOS_CTAB_SEG:DOS_BIOS_STUBS) read the live IF, i.e. always "off". Harmless while
     every BIOS call returned at once; not once INT 15h AH=86h waits by re-executing
     its BOP, because a real BIOS takes interrupts during that wait. */
#define DOS_BIOS_STUB_N 14            /* entries in WinMain's bios_ints[] (s91: +2Ah, 5Ch) */
INT IsOurStubCsIp(DWORD cs, DWORD ip)
{
    if (cs == DOS_HDLR_SEG) return 1;
    return cs == DOS_CTAB_SEG && ip >= DOS_BIOS_STUBS && ip < DOS_BIOS_STUBS + DOS_BIOS_STUB_N * DOS_BIOS_STUB_SIZE;
}
static INT GuestIfEnabled(volatile BYTE *tib)
{
    DWORD cs = VDM_REG16(tib, VTIB_CS);
    if (IsOurStubCsIp(cs, VDM_REG16(tib, VTIB_EIP))) {
        DWORD ss = VDM_REG16(tib, VTIB_SS), sp = VDM_REG16(tib, VTIB_ESP);
        return (PeekWord((ss << PARAGRAPH_SHIFT) + ((sp + X86_FRAME16_FLAGS) & WORD_MASK)) & EFLAGS_IF) != 0;
    }
    return IfOrVif(VDM_REG(tib, VTIB_EFLAGS));
}

/* GH #18 / sound epic: sample the kernel's FIXED_NTVDMSTATE word next to the
   reflected EFLAGS at a labelled point, for the first few occurrences of each
   label. iobench proved IRQ0 delivery is gated off forever during port I/O
   (irq0_inj=0 over 34M I/O events, BIOS tick frozen), which can only mean the
   reflected EFLAGS reports IF=0. The guest's REAL interrupt-enable state is the
   one the kernel virtualises in [0x714]; this sampler is how we identify which
   bit carries it -- compare a known-IF=1 moment (a BOP, where the guest's own
   FLAGS are on its stack) against an I/O reflect in the same run. */
VOID VdmStateSample(PCSTR label, volatile BYTE *tib, INT *budget)
{
    CHAR buffer[128], *cursor = buffer;
    if (*budget <= 0) return;
    (*budget)--;
    cursor = LogPut(cursor, "GH#18 vdmstate ");
    cursor = LogPut(cursor, label);
    cursor = LogPut(cursor, ": [0x714]=0x"); cursor = LogHex(cursor, *(volatile DWORD *)(ULONG_PTR)FIXED_NTVDMSTATE_LINEAR);
    cursor = LogPut(cursor, " EFL=0x");      cursor = LogHex(cursor, VDM_REG(tib, VTIB_EFLAGS));
    cursor = LogPut(cursor, " CS:IP=0x");    cursor = LogHex(cursor, VDM_REG16(tib, VTIB_CS));
    cursor = LogPut(cursor, ":0x");          cursor = LogHex(cursor, VDM_REG16(tib, VTIB_EIP));
    cursor = LogPut(cursor, "\r\n");
    LogAppend(LOG_PATH, buffer, cursor); SerialOut(buffer, cursor);
}

VOID InjectInt(volatile BYTE *tib, UINT vector)
{
    WORD ss = (WORD)VDM_REG(tib, VTIB_SS),  sp = (WORD)VDM_REG(tib, VTIB_ESP);
    WORD cs = (WORD)VDM_REG(tib, VTIB_CS),  ip = (WORD)VDM_REG(tib, VTIB_EIP);
    DWORD eflags = VDM_REG(tib, VTIB_EFLAGS);
    WORD flags = (WORD)eflags;                           /* push the live frame's FLAGS */
    ModeYRingNoteIrq(vector, cs, ip, ss, sp);         /* north star 1: where did it land? */
    /* ★ Fold VIF into the pushed IF. On VME hardware the guest's REAL interrupt-enable
       lives in EFLAGS.VIF (bit 19), because that is what its own STI sets -- but an IRET
       frame is 16 bits wide, so a straight truncation drops VIF and pushes IF=0. The
       guest's IRET then restores its virtual interrupt state from that zero and comes back
       with interrupts off FOREVER. Measured exactly that: after the very first injected
       INT 08h, irq0_inj stuck at 1 for the rest of the run and every later IRQ was gated
       off (Skyroads: irq0_skip=123k, and the SB completion IRQ could never be taken). The
       CPU does this fold itself for a hardware-vectored interrupt; we synthesise the frame,
       so we must do it too. */
    if (eflags & EFLAGS_VIF) flags |= EFLAGS_IF;
    sp -= X86_WORD_SIZE; PokeWord(((DWORD)ss << PARAGRAPH_SHIFT) + sp, flags);     /* push FLAGS */
    sp -= X86_WORD_SIZE; PokeWord(((DWORD)ss << PARAGRAPH_SHIFT) + sp, cs);     /* push CS    */
    sp -= X86_WORD_SIZE; PokeWord(((DWORD)ss << PARAGRAPH_SHIFT) + sp, ip);     /* push IP    */
    VDM_SET16(tib, VTIB_ESP, sp);
    /* Clear IF + TF, and VIF with them: vectoring an interrupt disables the guest's
       interrupts, and under VME "the guest's interrupts" means VIF. */
    VDM_REG(tib, VTIB_EFLAGS) &= ~(EFLAGS_TF_U | EFLAGS_IF_U | EFLAGS_VIF);
    VDM_SET16(tib, VTIB_EIP, PeekWord(IVT_OFFSET_ADDRESS(vector)));      /* IVT[vec].offset */
    VDM_SET16(tib, VTIB_CS,  PeekWord(IVT_SEGMENT_ADDRESS(vector)));  /* IVT[vec].segment */
}
enum { QIRQ_PROBE_IRQ = 5 };   /* qimode bit 2: the device line the probe raises */
DWORD WINAPI QueueIrqProbeThread(LPVOID parameter)
{
    INT round;
    (VOID)parameter;
    Sleep(500);                                  /* let the guest install its ISRs */
    for (round = 0; round < 40 && g_Running; ++round) {
        DWORD before = *(volatile DWORD *)(ULONG_PTR)FIXED_NTVDMSTATE_LINEAR, after = before;
        INT attempt;
        HostIrqSink(NULL, QIRQ_PROBE_IRQ);
        for (attempt = 0; attempt < 50; ++attempt) {
            Sleep(1);
            after = *(volatile DWORD *)(ULONG_PTR)FIXED_NTVDMSTATE_LINEAR;
            if (after != before) break;
        }
        if (round < 8) {
            CHAR buffer[256], *cursor = buffer;
            cursor = LogPut(cursor, "QIRQ: raise#0x");   cursor = LogHex(cursor, (DWORD)round);
            cursor = LogPut(cursor, " bits=0x");         cursor = LogHex(cursor, g_QiBits);
            cursor = LogPut(cursor, " st=0x");           cursor = LogHex(cursor, (DWORD)g_QiStatus);
            cursor = LogPut(cursor, " state 0x");        cursor = LogHex(cursor, before);
            cursor = LogPut(cursor, "->0x");             cursor = LogHex(cursor, after);
            cursor = LogPut(cursor, " after 0x");        cursor = LogHex(cursor, (DWORD)attempt);
            cursor = LogPut(cursor, "ms pend5=0x");      cursor = LogHex(cursor, (DWORD)g_IrqNPending[5]);
            cursor = LogPut(cursor, " ica=0x");          cursor = LogHex(cursor, VdmIcaGetState(5));
            cursor = LogPut(cursor, "\r\n");
            LogAppend(LOG_PATH, buffer, cursor); SerialOut(buffer, cursor);
        }
        Sleep(250);
    }
    return 0;
}
/* ── ★★★ COOPERATIVE DEVICE-IRQ DELIVERY IN V86, FACTORED SO IT HAS ONE HOME. ──────
     This was inline in the main exec loop, which meant it only ran when the guest
     reached that loop -- and a guest inside a NESTED real-mode call (DPMI 0301/0302)
     never does. MEASURED ON ZAR (#23): its Miles driver issues an 8-bit SINGLE-CYCLE
     16-byte transfer as an init-time DMA/IRQ self-test, the block drains, IRQ 5 is
     raised, and the async injector refuses it -- `ASYNC-EARLY bail irq=05 why=0x14`,
     "the CPU thread was in HOST code", because the thread is down inside the nested
     VdmRunGuest. Single-cycle means one IRQ and no second chance, so the driver spins at
     0x34d3:0x06b1 until the watchdog kills it.
   ► The latch (g_IrqNPending) already persists, and the guest inside 0301/0302 is in
     V86, so the delivery this loop was already doing is exactly the right delivery --
     it simply was not reachable from there. Extracted verbatim rather than copied: two
     copies of an interrupt-delivery gate is how they drift, and every clause here was
     paid for (the in_bop window, the VME-aware GuestIfEnabled, the unhooked-drop, the
     acknowledge/EOI rule, one-per-turn).
   ⚠ Returns 1 if an interrupt was injected, so a caller that must let the handler IRET
     before doing anything else can tell. */
INT V86DeliverDeviceIrq(volatile BYTE *tib)
{
    INT injected = 0;
/* Device IRQs (SB block completion on 5, etc.): same IF gating as IRQ0/1,
   vectored as INT 8+irq. Skip while inside our own timer/keyboard stubs. */
{ INT irq;
  DWORD currentCs = VDM_REG16(tib, VTIB_CS), currentIp = VDM_REG16(tib, VTIB_EIP);
  /* Only the BOP itself is off-limits (we would re-enter mid-service);
     once past it the guest is running a handler with IF set, and a real
     PC delivers a device IRQ there quite happily. */
  INT inBop = (currentCs == DOS_HDLR_SEG &&
                ((currentIp >= DOS_HDLR_INT08_STUB_OFF && currentIp < DOS_HDLR_INT08_STUB_OFF + VDM_BOP_LENGTH) || (currentIp >= DOS_HDLR_INT09_STUB_OFF && currentIp < DOS_HDLR_INT09_STUB_OFF + VDM_BOP_LENGTH)));
  if (!inBop && currentCs != DOS_HDLR_SEG) IfvNote(IFV_PATH_VTIB_DEVICE, VDM_REG(tib, VTIB_EFLAGS));
  if (!inBop && GuestIfEnabled(tib)) {
      INT index;
      for (index = 0; index < (INT)sizeof g_IrqOrder; ++index) {
          UINT vector;
          irq = g_IrqOrder[index];
          vector = VddPicVector(&g_Pic, (BYTE)irq);        /* 08h+q or 70h+(q-8), as programmed */
          if (!g_IrqNPending[irq]) continue;
          if (PeekWord(IVT_SEGMENT_ADDRESS(vector)) == DOS_HDLR_SEG
              && PeekWord(IVT_OFFSET_ADDRESS(vector)) == DOS_IRET_STUB_OFF) {
              /* ── s92 (#239): UNHOOKED IN REAL MODE IS NOT UNHOOKED. A DPMI client that
                   installed a PROTECTED-MODE handler for the line owns it whatever mode
                   the CPU is in (DPMI 0.9: a hardware interrupt is passed to the PM
                   handler if there is one). ZAR hooks IRQ5 only in PM; while its start-up
                   check polls DOS time, IRQ5 often became deliverable inside a real-mode
                   excursion (0300h), here -- and was DROPPED. That is the intermittent
                   silent ZAR: delivered only if it happened to wait for a 32-bit window.
                   Kept pending for the PM path instead; dropped only if nobody wants it. */
              if (g_DpmiPm && g_PmInt[IrqPmVector((UINT)irq)].Client) continue;
              InterlockedExchange(&g_IrqNPending[irq], 0);   /* unhooked: drop it */
              continue;
          }
          if (!VddPicCanDeliver(&g_Pic, (BYTE)irq)) continue;
          if (InterlockedExchange(&g_IrqNPending[irq], 0)) {
              VddPicAcknowledge(&g_Pic, (BYTE)irq);
              if (AsyncVectorIsOurStub((UINT)irq)) VddPicEndOfInterrupt(&g_Pic, (BYTE)irq);
              g_IrqNInjected++;
              IfvTrace((UINT)irq, IFV_PATH_VTIB_DEVICE, VDM_REG(tib, VTIB_EFLAGS), currentCs, currentIp);
              InjectInt(tib, vector);
              break;                    /* one per turn: let it IRET first */
          }
      }
  } else {
      /* REFUSAL LOG. A pending device IRQ we decline to inject is indistinguishable
         from "no interrupt was ever raised" from outside, so record the first few
         with everything needed to name the clause. MEASURED ANSWER for Skyroads:
         irqn_refused stayed 0 across the whole run, so we never refuse -- the
         completion IRQ is raised (raised[5]=1, sb_irq=5) about a second AFTER the
         guest stops trapping. Heartbeat timeline: the block is programmed at ~8.5 s
         (len 0x7d64 @ 6024 Hz), drains at exactly the right rate, and completes at
         ~14 s; `io_events` freezes at ~13 s with the guest at DOS_HDLR_SEG:0x0037
         (the `CD 1C`), i.e. inside its own INT 1Ch handler spinning with no traps.
         So there is genuinely NO injection point at the moment that matters -- the
         4.5M traps all happen in the first 6 s, before the block even exists. Async
         delivery is required; this log stays as the discriminator if that changes. */
      INT pend = 0;
      for (irq = ASYNC_FIRST_DEVICE_IRQ; irq < PIC_LINES; ++irq) if (g_IrqNPending[irq]) { pend = irq; break; }
      if (pend) g_IrqNRefuseTotal++;
      if (pend && g_IrqNRefuseLog < 16) {
          CHAR lineBuffer[256], *lineCursor = lineBuffer;
          DWORD ss = VDM_REG16(tib, VTIB_SS), sp = VDM_REG16(tib, VTIB_ESP);
          g_IrqNRefuseLog++;
          lineCursor = LogPut(lineCursor, "IRQN-REFUSE irq=0x");  lineCursor = LogHex(lineCursor, (DWORD)pend);
          lineCursor = LogPut(lineCursor, " cs:ip=0x");           lineCursor = LogHex(lineCursor, currentCs);
          lineCursor = LogPut(lineCursor, ":0x");                 lineCursor = LogHex(lineCursor, currentIp);
          lineCursor = LogPut(lineCursor, " efl=0x");             lineCursor = LogHex(lineCursor, VDM_REG(tib, VTIB_EFLAGS));
          lineCursor = LogPut(lineCursor, " ss:sp=0x");           lineCursor = LogHex(lineCursor, ss);
          lineCursor = LogPut(lineCursor, ":0x");                 lineCursor = LogHex(lineCursor, sp);
          lineCursor = LogPut(lineCursor, " stkflags=0x");
          lineCursor = LogHex(lineCursor, PeekWord((ss << PARAGRAPH_SHIFT) + ((sp + X86_FRAME16_FLAGS) & WORD_MASK)));
          lineCursor = LogPut(lineCursor, inBop ? " why=in_bop" : " why=if_gate");
          lineCursor = LogPut(lineCursor, "\r\n");
          LogAppend(LOG_PATH, lineBuffer, lineCursor); SerialOut(lineBuffer, lineCursor);
      }
  } }

    return injected;
}
