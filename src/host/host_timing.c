/* host_timing.c -- time: the PIT and its pacer, BIOS ticks, IRQ0 delivery, the CPU-speed governor,
 *   the RTC and vertical retrace.
 *
 * Part of the host's single translation unit: #included by main.c after host_internal.h. */

#define MEMDUMP_PATH   OUT_("memdump.bin")
static CMOS_STATE   g_Cmos;      static NTVDD_DEVICE g_CmosDevice;
/* PENDING TIMER TICKS, as a saturating COUNT rather than a flag. A boolean coalesces: every
   tick that falls while the guest has interrupts off -- and Skyroads spends most of its time
   in exactly that state, CLI'd around its 256-colour palette writes -- was silently thrown
   away, so the music's tempo wandered with whatever the guest happened to be doing. A real
   8259 latches the request and delivers it the moment IF comes back. Saturating at 4 keeps
   that fidelity without letting a long CLI region accumulate a burst that would then flood
   the guest with back-to-back timer interrupts. */
#define IRQ0_PENDING_MAX 4
/* ── HOW MANY TIMER TICKS DOES THE PROTECTED-MODE CLIENT ACTUALLY OWE? ───────────────
     Separate from g_Irq0Pending, which SATURATES AT FOUR on purpose (see above) and so
     cannot answer the question. The PM catch-up batch needs a true count, and without
     one it invented its own: it injected a fixed DPMI_IRQ0_BATCH of 64 ticks on every
     asynchronous return, with no reference to elapsed time at all. Measured on Doom,
     which programs 140 Hz (PIT reload 0x214a): 2612 asynchronous returns x 64 =
     169,032 ISR entries in 45 seconds, against the 6,300 it asked for -- a game clock
     running 27 times too fast, which is not a small error in a program whose entire
     frame pacing is I_GetTime().
     Bounded, for the same reason the saturating latch is: a host stall must not be
     repaid as one enormous burst. 64 at 140 Hz is ~460 ms of catch-up, well past any
     stall this host produces and still short enough that the tempo cannot lurch a
     visible amount. */
#define PM_TICK_OWED_MAX 64
/* ── WHERE THE TIMER GOES FROM 140 Hz TO 55 Hz. ──────────────────────────────────────
     Doom programs 140 Hz and gets 55 delivered (39%), and DMX mixes PCM in the timer
     ISR -- so the missing 61% of ticks are the missing PCM refills, and the echo. Four
     numbers separate the candidate losses, and nothing currently distinguishes them:
       syncs    HostPitSync() calls that advanced the clock. This is the CEILING on
                async delivery, because g_AsyncTriedThisSync allows ONE attempt each.
       raises   what the 8254 model actually generated: should be ~140/s.
       attempts AsyncInjectIrq(0) calls -- syncs that got as far as trying.
       owed_max the backlog high-water mark: how far behind delivery ever fell.
     ⚠ SUSPECT THE LOCK. HostPitSync takes g_Lock, and Doom's mode-Y drawing does
       ~43,000 port writes a second (1.95M mask changes a run), every one of which also
       takes it. If syncs come in well under the UI thread's 5 ms tick, the video path
       is starving the timer, which is starving the audio. */
static UINT32 g_PitSyncs;
static LONG     g_PmTickOwedMaximum;
/* ── ...AND owed_max IS A HIGH-WATER MARK, NOT AN OCCUPANCY. ─────────────────────────
     "owed_max = 0x40 = PM_TICK_OWED_MAX, the backlog is PERMANENTLY SATURATED" reads a
     maximum as a steady state: ONE stall anywhere in 45 s pins it at the cap for the
     rest of the run and it can never come back down. A saturated backlog and a backlog
     that touched the cap once look identical in that number, and they mean opposite
     things about whether delivery is keeping up.
     So sample the DEPTH at every sync. Nine buckets, one increment, no lock. */
static UINT32 g_PmOwedHistogram[9];
static VOID PmOwedSample(LONG owed)
{
    UINT bucket = 0;
    if      (owed <= 0)  bucket = 0;
    else if (owed <  4)  bucket = (UINT)owed;          /* 1, 2, 3 exactly */
    else if (owed <  8)  bucket = 4;
    else if (owed < 16)  bucket = 5;
    else if (owed < 32)  bucket = 6;
    else if (owed < 64)  bucket = 7;
    else              bucket = 8;
    g_PmOwedHistogram[bucket]++;
}
VOID Irq0Latch(VOID)
{
    if (g_Irq0Pending < IRQ0_PENDING_MAX) InterlockedIncrement(&g_Irq0Pending);
    if (g_PmTickOwed < PM_TICK_OWED_MAX)  InterlockedIncrement(&g_PmTickOwed);
    if (g_PmTickOwed > g_PmTickOwedMaximum) g_PmTickOwedMaximum = g_PmTickOwed;
}
/* Consume one owed tick. Returns 0 if none is owed, i.e. "the client is up to date --
   do not manufacture time it has not been billed for". */
INT PmTickTake(VOID)
{
    if (g_PmTickOwed <= 0) return 0;
    InterlockedDecrement(&g_PmTickOwed);
    return 1;
}
static INT   g_AsyncTriedThisSync = 0;   /* see HostIrqSink: one attempt per PIT sync */
/* ── HOW EVENLY DO IRQ0s ACTUALLY LAND? ──────────────────────────────────────────────
     DMX's mixer is armed by the SB block IRQ (next_due = NOW) and SERVICED on the next
     timer interrupt. A block is 11.6 ms; a tick period at the 135/s we deliver is
     7.4 ms, so every arm should be serviced well inside its block -- and yet a block
     finds the previous arm unserviced 32.8% of the time, and the two arms collapse into
     one refill. Either the gaps between DELIVERED ticks are exceeding 11.6 ms a third
     of the time (ours), or they are even and DMX is not servicing at the first
     available tick (the guest's). The RATE cannot tell those apart -- 135/s is 135/s
     either way -- so bucket the interval. QPC, because a tick period is smaller than
     GetTickCount's granularity. */
static DWORD g_TickGap[12], g_TickGapMaximumMicroseconds, g_TickGapOver;   /* >11.6ms = a block */
VOID TickDeliveredNote(VOID)
{
    static LARGE_INTEGER frequency, prev;
    LARGE_INTEGER now;
    if (!frequency.QuadPart && !QueryPerformanceFrequency(&frequency)) return;
    if (!QueryPerformanceCounter(&now)) return;
    if (prev.QuadPart) {
        LONGLONG microseconds = ((now.QuadPart - prev.QuadPart) * MICROSECONDS_PER_SECOND) / frequency.QuadPart;
        UINT bucket = 0;
        while (bucket < 11 && microseconds >= (LONGLONG)500 << bucket) ++bucket;      /* 0.5,1,2,4..512 ms */
        g_TickGap[bucket]++;
        if (microseconds > (LONGLONG)g_TickGapMaximumMicroseconds) g_TickGapMaximumMicroseconds = (DWORD)microseconds;
        if (microseconds > 11600) g_TickGapOver++;   /* longer than one 128-byte block at 11111 Hz */
    }
    prev = now;
}
static INT      g_Irq0Yielded;
#define KEYIRQ_MAX_YIELD 3

#define HOST_LOCK_TRY() HostLockTry(__LINE__)
/* ── ★★★ THE CRYSTAL'S OWN LOCK. (s61) ───────────────────────────────────────────
     A real 8254 counts on a crystal that cannot be made to wait. Ours counted only
     under g_Lock -- the same lock the video renderer holds for 13-22 ms a frame, the
     audio mixer holds per fill, and ~68,000 port traps a second pass through -- and
     the measured result, from a session the user played, was that 86% of all timing
     stalls were the clock simply NEVER TICKING on time (STAGE2: IRQ0WHY gen vs del).
     Every arbitration scheme downstream failed because they rationed delivery while
     GENERATION was what starved.
     So tick generation now runs under this micro-lock only. Rules that keep it safe:
       - held for microseconds: QPC arithmetic, the counter model, the PIC's IRR
         latch, an event signal. NEVER file I/O, NEVER a suspend, NEVER g_Lock.
       - ordering is strictly g_Lock -> g_PitCs (the exec thread reaches the PIT's
         port handlers with g_Lock held). Nothing may take g_Lock while holding this;
         delivery attempts run AFTER release, and only via HOST_LOCK_TRY. */
static CRITICAL_SECTION g_PitCs;
/* Exec-loop instrumentation, reported once at wind-down (cheap counters, no I/O in
   the hot path). These separate the two costs that look identical from outside: how
   many times we round-tripped to service port I/O, how many accesses the burst fast
   path absorbed without a round trip, and how many timer IRQs actually reached the
   guest -- the last one being how we caught the BIOS tick starving during an I/O
   storm (iobench case 1 could not accumulate 5 ticks in 30 s). */
static DWORD          g_EventIo         = 0;  /* port-I/O events serviced            */
LONGLONG       g_Irq0Start, g_Irq0TimePrevious;
static DWORD          g_Irq0NoteCs, g_Irq0NoteIp;      /* set by the caller      */
/* ── ★★ WHICH OF THREE MECHANISMS MAKES A LONG GAP? (Skyroads wobble, s61) ────────
     The s60 root-cause note reads the 8-20 ms gaps as ASYNC DELIVERY starving, and
     the fix that follows from that is a lock-free courier thread. But two other
     mechanisms produce a gap of exactly that size, and NOTHING IN THE TREE TELLS
     THEM APART -- so the courier would be a fix chosen by argument, which is the
     move that has cost this project whole sessions before:

       A  DELIVERY    the tick WAS raised and the injector could not place it: the
                      exec thread was in host code (why=20 not_in_exec, measured at
                      75% of attempts). A courier that retries OUTSIDE g_Lock fixes
                      this and nothing else.
       B  GENERATION  the tick was never raised at all, because HostPitSync could
                      not run -- every caller takes g_Lock and HostAudioFill holds
                      it across a whole buffer mix. A gap with NO RAISES inside it is
                      not a delivery fault, and a courier cannot fix it: there is
                      nothing pending to deliver. The fix would be the lock.
       C  YIELD       HostIrqSink hands the IRQ0 async opportunity to a pending KEY
                      (g_KeyIrqRetry, ON by default), up to KEYIRQ_MAX_YIELD = 3
                      times in a row. Three yields at 180 Hz is 16.7 ms -- the exact
                      observed gap size -- and it can only happen WHILE A KEY IS
                      DOWN, which is when the player is steering and is precisely
                      when the wobble is reported. That code calls the yield "free"
                      because the tick is not LOST; the wobble is a LATENCY symptom,
                      and latency is what a yield spends.

   ► AND THE RAISE COUNT SEPARATES A FROM B WITH NO THRESHOLD AT ALL, which is why
     it is the field to read first. Whatever rate the guest programmed, a HEALTHY
     gap contains exactly ONE raise: one tick generated, one tick delivered. So
       raises-in-gap >= 2  => the 8254 DID generate the ticks and we failed to place
                              them. That is A, delivery, and a courier is the fix.
       raises-in-gap <= 1  => the ticks were never generated. The clock itself
                              stalled, so there was nothing pending and no injector
                              of any kind could have helped. That is B, and the fix
                              is the lock that HostPitSync waits on.
     A gap is ANOMALOUS when it spans at least two programmed periods -- the same
     "the guest missed a tick" test, expressed in the guest's own units. */
DWORD          g_Irq0RaiseCount, g_Irq0AttemptsCount, g_Irq0NieCount, g_Irq0YieldCount;
static DWORD          g_Irq0PrRaise, g_Irq0PrAttempts, g_Irq0PrNie, g_Irq0PrYield;
static VOID Irq0DeliveredNote(VOID)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!g_Irq0Start) { g_Irq0Start = now.QuadPart; g_Irq0TimePrevious = now.QuadPart;
                      g_Irq0IoPrevious = g_EventIo;
                      g_Irq0PrRaise = g_Irq0RaiseCount; g_Irq0PrAttempts   = g_Irq0AttemptsCount;
                      g_Irq0PrNie   = g_Irq0NieCount;   g_Irq0PrYield = g_Irq0YieldCount;
                      return; }
    {   DWORD seconds = QpcMicroseconds(now.QuadPart - g_Irq0Start) / MICROSECONDS_PER_SECOND_U;
        DWORD gus = QpcMicroseconds(now.QuadPart - g_Irq0TimePrevious);
        DWORD gapMilliseconds = gus / MICROSECONDS_PER_MILLISECOND_U;
        DWORD ioDelta = g_EventIo - g_Irq0IoPrevious;
        /* The A/B/C discriminator -- see the block above. Same shape as ioDelta. */
        DWORD raiseDelta = g_Irq0RaiseCount - g_Irq0PrRaise;
        DWORD attemptsDelta   = g_Irq0AttemptsCount   - g_Irq0PrAttempts;
        DWORD nieCountDelta   = g_Irq0NieCount   - g_Irq0PrNie;
        DWORD yieldDelta   = g_Irq0YieldCount - g_Irq0PrYield;
        /* THE GUEST'S OWN PERIOD, not a constant of ours. Skyroads runs its intro at
           the BIOS 18.2 Hz and its game at 180 Hz, and a fixed threshold scores the
           whole intro as faulty. Sampled per delivery: reprogramming the 8254 is
           exactly the event that must move this. */
        DWORD perMicroseconds = (DWORD)(((UINT64)VddPitEffectiveReload(&g_Pit) * MICROSECONDS_PER_SECOND_ULL)
                               / PIT_INPUT_HZ);
        UINT bucket = 0;
        if (seconds < IRQ0TL_SECS) g_Irq0TimeLast[seconds]++;
        while (bucket < 7 && gapMilliseconds >= (1u << bucket)) ++bucket;     /* 0:<1ms 1:1 2:2 3:4 ... 7:64+  */
        g_Irq0GapHistogram[bucket]++;
        ++g_Irq0GapCount;
        if (perMicroseconds && gus >= 2u * perMicroseconds) {        /* the guest missed a whole tick  */
            ++g_Irq0AnomalyCount; g_Irq0AnomalyMicroseconds += gus; g_Irq0AnomalyIo += ioDelta;
            g_Irq0AnomalyRaise += raiseDelta; g_Irq0AnomalyAttempts += attemptsDelta;
            g_Irq0AnomalyNie   += nieCountDelta;   g_Irq0AnomalyYield += yieldDelta;
            /* A or B, by whether the ticks were ever generated. See the essay. */
            if (raiseDelta >= 2u) ++g_Irq0AnomalyDelete; else ++g_Irq0AnomalyGeneration;
        } else {
            ++g_Irq0NormalCount; g_Irq0NormalMicroseconds += gus; g_Irq0NormalIo += ioDelta;
        }
        if (gapMilliseconds > g_Irq0WorstGapMs) {
            g_Irq0WorstGapMs = gapMilliseconds; g_Irq0WorstGapIo = ioDelta;
            g_Irq0WorstCs = g_Irq0NoteCs; g_Irq0WorstIp = g_Irq0NoteIp;
            g_Irq0WorstRaise = raiseDelta; g_Irq0WorstAttempts   = attemptsDelta;
            g_Irq0WorstNie   = nieCountDelta;   g_Irq0WorstYield = yieldDelta;
            g_Irq0WorstPerMicroseconds = perMicroseconds;
        }
        if (gapMilliseconds > g_Irq0GapMaximumMs) g_Irq0GapMaximumMs = gapMilliseconds;
        g_Irq0TimePrevious = now.QuadPart;
        g_Irq0IoPrevious = g_EventIo;
        g_Irq0PrRaise = g_Irq0RaiseCount; g_Irq0PrAttempts   = g_Irq0AttemptsCount;
        g_Irq0PrNie   = g_Irq0NieCount;   g_Irq0PrYield = g_Irq0YieldCount;
    }
}
static DWORD          g_Irq0Skip     = 0;  /* IRQ0 delivery gated off (IF=0 etc.) */
static DWORD          g_Irq0SkipIf  = 0;  /* ...because the guest had interrupts off  */
static DWORD          g_Irq0SkipStub= 0;  /* ...because we were inside our INT 08h stub */
static DWORD          g_PitLatchDumps = 0;   /* PIT-LATCH poll-ring dumps printed (max 2) */
static DWORD          g_EventIntPending    = 0;  /* event-3 interrupt-pending notifications */
DWORD          g_IrqNInjected      = 0;  /* device IRQs (2-7) injected into the guest */
DWORD          g_IrqNRefuseTotal = 0;
static DWORD g_EventHistogram[EV_HIST_MAX];
/* ── #238: WHERE THE CPU THREAD IS WHEN IT IS NOT IN THE GUEST. (s85) ─────────────────
     3DBench's 1 kHz timer: 84% of async attempts bailed `not_in_exec`, with only ~410
     traps a second -- so the thread was somewhere in the host between v86_runs most of
     the time. Split the wall clock: microseconds inside VdmRunGuest, and the host time
     between one VdmRunGuest's return and the next's entry, charged to the event that
     returned (whatever the loop did to service it, interpreter slices included). */
static DWORD g_V86MicrosecondsTotal, g_HostMicrosecondsEvent[EV_HIST_MAX];
static DWORD g_BopHistogram[BYTE_VALUES];            /* V86 BOP events by number (the busiest are printed) */
static DWORD g_PitPaceCalls;              /* PitPacerThread wakes (declared here for g_XsSnapshot) */
static DWORD g_XsStart, g_XsSeconds, g_XsSnapshot[XS_SECS][XS_N];
#define PM_HEADLESS_GRACE_MS 3000                /* grace for a clean wind-down before the hard backstop forces exit */
/* Retry accounting for the one-attempt-per-sync device-IRQ offer in HostPitSync.
   `try` counts syncs where something was pending and we spent a round trip on it;
   `ok` counts the ones that landed. try==ok==0 is the healthy steady state -- it
   means every device IRQ was placed at its raise instant and this cost nothing. */
static DWORD g_IrqNRetryTry = 0, g_IrqNRetryOk = 0, g_IrqNRetryWhy = 0;
/* True when a line's vector still points at one of our own do-nothing stubs (the shared
   device IRET at DOS_HDLR_SEG:0x66, or the default INT 09h at 0x4C). Those never send an
   EOI, so anything delivered through them must be auto-EOI'd or the line latches. */
/* ── ★★★★★ IRQ0 IS HELD IN SERVICE UNTIL THE GUEST EOIs -- LIKE EVERY OTHER LINE. ──────
     Since ba927ac the timer was the ONE line the host auto-EOI'd on delivery, "gated by
     the guest's own IF discipline". That discipline is not enough, and Lemmings is the
     proof (s69/s70): its High-Performance-PC timer ISR does `sti` and then SPINS on
     0x3DA for the vertical retrace before it reprograms the PIT and does its work
     (guest CS:17C7..18B1). On a real PC the 8259's in-service bit keeps the next IRQ0
     out until the `out 20h,20h` at the end. With auto-EOI, a tick raised during that
     spin RE-ENTERED the handler; the inner instance took the retrace the outer one was
     waiting for, and from then on every tick nested one level deeper, forever: no ISR
     body ever ran (no music), the game loop never ran (black screen, "stuck fade"),
     and the stack grew ~30 bytes a tick until it overran the data segment (the
     sprite-blitter AV with es=0xD000). The heartbeat's tell: irq0/s == flips/s == 70
     with the guest 100% inside the 0x3DA poll (p3da 3.3M reads/s) -- everything
     "ticking" and nothing finishing. It only surfaced once the PIT was corrected: the
     old random reload happened to be slow enough for the nesting to unwind.
   ► So IRQ0 is acknowledged like any line and released by the guest's EOI -- or by our
     own INT 08h BOP, which EOIs where the BIOS handler would (the Skyroads case that
     motivated the deviation: it chains to the BIOS on the ticks it does not EOI).
   ⚠ TWO SAFETY NETS, both counted in STAGE2 (`irq0_isr=`):
       1. A handler that never EOIs and never chains cannot exist on real hardware (the
          timer would stop dead), but a guest that dies inside its handler can. If IRQ0
          sits in service for IRQ0_ISR_TIMEOUT_MS it is released and counted; after
          IRQ0_ISR_TIMEOUTS_MAX of those the guest is judged not to EOI its timer and
          the old auto-EOI behaviour comes back for the rest of the run, logged.
       2. `g_Irq0Pending` still saturates at four, so a long handler is followed by a
          short burst; a real PIC latches one. Left as is: our delivery latency is what
          the backlog compensates for. */
#define IRQ0_ISR_TIMEOUT_MS   250u
#define IRQ0_ISR_TIMEOUTS_MAX 3u
static DWORD g_Irq0IsrSince    = 0;   /* GetTickCount()|1 when IRQ0 went in service; 0 = not */
static DWORD g_Irq0IsrBlocks   = 0;   /* deliveries refused because IRQ0 was in service    */
static DWORD g_Irq0IsrTimeouts = 0;   /* in-service bits released by the timeout           */
static DWORD g_Irq0IsrStrict   = 0;   /* acknowledges that HELD the line (guest EOIs)      */
static DWORD g_Irq0IsrAuto     = 0;   /* acknowledges that auto-EOI'd (stub or fallback)   */
static INT   g_Irq0AutoEoi      = 0;   /* fallback engaged: this guest does not EOI IRQ0    */

static UINT32 g_PitRestartsSeen = 0;
VOID HostPitResyncCheck(VOID)
{
    UINT32 restarts = g_Pit.Restarts;             /* monotonic; a stale read only defers us */
    if (restarts == g_PitRestartsSeen) return;
    g_PitRestartsSeen = restarts;
    if ((g_Pic.Master.Isr & 1) && !g_Irq0AutoEoi) {
        LONG pend = InterlockedExchange(&g_Irq0Pending, 0);
        if (pend || (g_Pic.Master.Irr & 1)) {
            __sync_fetch_and_and(&g_Pic.Master.Irr, (BYTE)~1u);
            g_Irq0ResyncDrop++;
        }
    }
}
enum { IRQ0_ISR_GAP_RESET_MS = 100 };   /* Irq0CanDeliver: a stall this long restarts the ISR clock */
/* Can IRQ0 be delivered now? The PIC's answer, plus safety net 1. Called at both
   delivery sites (cooperative exec loop and the async courier). */
static DWORD g_Irq0LastAttempt = 0;   /* GetTickCount()|1 at the last delivery attempt */
INT Irq0CanDeliver(VOID)
{
    DWORD now = GetTickCount() | 1, gap = now - g_Irq0LastAttempt;
    g_Irq0LastAttempt = now;
    if (VddPicCanDeliver(&g_Pic, PIC_IRQ_TIMER)) return 1;
    if (g_Pic.Master.Isr & 1) {
        DWORD since = g_Irq0IsrSince;
        /* ── A HOST STALL IS NOT A GUEST THAT FORGOT TO EOI. s70: a headless run with
             capture.flag hit the timeout three times and engaged the fallback -- but no
             IRQ0-ISR-LONG line was ever written, i.e. nothing ATTEMPTED delivery during
             those episodes: the whole host was stopped (a 24bpp shot written to the SMB
             share under the device lock), the guest's handler included. Time the guest
             did not get cannot count against it. If this is the first attempt after a
             gap longer than the timeout's own resolution, restart the clock instead. */
        if (since && gap > IRQ0_ISR_GAP_RESET_MS) { g_Irq0IsrSince = now; since = now; }
        if (since && (now - since) > IRQ0_ISR_TIMEOUT_MS) {
            VddPicEndOfInterrupt(&g_Pic, PIC_IRQ_TIMER);
            g_Irq0IsrSince = 0;
            if (++g_Irq0IsrTimeouts >= IRQ0_ISR_TIMEOUTS_MAX && !g_Irq0AutoEoi) {
                static const CHAR message[] = "IRQ0-ISR: in service past the timeout 3x -- this guest "
                    "does not EOI its timer; auto-EOI fallback engaged for the rest of the run\r\n";
                g_Irq0AutoEoi = 1;
                LogAppend(LOG_PATH, message, message + sizeof(message) - 1);
            }
            return VddPicCanDeliver(&g_Pic, PIC_IRQ_TIMER);
        }
        g_Irq0IsrBlocks++;
        /* ── NAME THE LONG ONE. The s70 by-hand stall began with a single in-service
             episode of >250 ms (the trapdoor moment) and the log could not say where the
             guest was. Bounded: one line per 50 ms step of an episode, 8 lines per run.
             cs:ip is the TIB's, i.e. the last V86 exit -- biased, but it is what the
             heartbeat prints and it has named sites before. */
        if (since && g_TibDebug) {
            static DWORD lines = 0, episode = 0, step = 0;
            DWORD milliseconds = GetTickCount() - since;
            if (episode != since) { episode = since; step = 0; }
            if (milliseconds >= (step + 1) * 50u && lines < 8) {
                CHAR buffer[160], *cursor = buffer;
                step = milliseconds / 50u; ++lines;
                cursor = LogPut(cursor, "IRQ0-ISR-LONG in service ms=");  cursor = LogDecimal(cursor, milliseconds);
                cursor = LogPut(cursor, " guest cs:ip=0x"); cursor = LogHex(cursor, VDM_REG16(g_TibDebug, VTIB_CS));
                cursor = LogPut(cursor, ":0x");              cursor = LogHex(cursor, VDM_REG16(g_TibDebug, VTIB_EIP));
                cursor = LogPut(cursor, " pending=");        cursor = LogDecimal(cursor, (DWORD)g_Irq0Pending);
                cursor = LogPut(cursor, "\r\n");
                LogAppend(LOG_PATH, buffer, cursor);
            }
        }
    }
    return 0;
}

/* Acknowledge IRQ0 at delivery: hold it in service unless the vector is one of our
   never-EOIing stubs or the fallback is engaged. (Our INT 08h BOP is NOT such a stub:
   it EOIs, as the BIOS handler does.) */
VOID Irq0Ack(VOID)
{
    if (g_Irq0AutoEoi || AsyncVectorIsOurStub(PIC_IRQ_TIMER)) {
        VddPicAcknowledgeAutoEoi(&g_Pic, 0);
        g_Irq0IsrAuto++;
    } else {
        VddPicAcknowledge(&g_Pic, 0);
        g_Irq0IsrSince = GetTickCount() | 1;
        g_Irq0IsrStrict++;
    }
}
/* ── #173: THE PROTECTED-MODE ARMS HOLD IRQ0 IN SERVICE TOO. ─────────────────────────
     Until s81 the async PM arm acknowledged IRQ0 and EOI'd it on the spot, and the two
     synchronous PM injectors (the #2b latch and the catch-up batch) never told the PIC
     at all. Either way a DPMI client's timer ISR ran with IRQ0 NOT in service, so its own
     `out 20h,20h` -- a NON-SPECIFIC EOI -- cleared the highest bit that WAS in service:
     a sound card's or the keyboard's, whose handler was still running. Doom's DMX EOIs
     every tick it does not chain.
   ► A synchronous injector claims the line before it runs the handler (the handler EOIs
     from inside the call, through HostTryIoPm) and hands it back when the injector
     declined, since then no handler ran to EOI it. A handler that chains to our default
     PM INT 08h gets the BIOS's EOI there, as the V86 BOP arm gives it. The safety nets
     (250 ms timeout, auto-EOI fallback) are Irq0CanDeliver's and cover these arms. */
static INT Irq0PmClaim(VOID)
{
    if (!Irq0CanDeliver()) return 0;
    Irq0Ack();
    return 1;
}
static VOID Irq0PmUnclaim(VOID)
{
    if (g_Irq0AutoEoi || AsyncVectorIsOurStub(PIC_IRQ_TIMER)) { g_Irq0IsrAuto--; return; }
    VddPicEndOfInterrupt(&g_Pic, PIC_IRQ_TIMER);
    g_Irq0IsrSince = 0;
    g_Irq0IsrStrict--;
}
#define TSC_RESYNC_INTERVAL_ULL 2000ull   /* ~2 ms of ticks between re-anchors */
/* Advance the OPL timers from the real clock. The AdLib detect measures an 80us
   timer and games pace music on timer overflow, so the ~16ms bus frame tick is far
   too coarse -- the status register has to be current the moment the guest reads
   it, or detection sees 0x00 and concludes there is no card. We therefore pump
   from the exec loop, which by construction gets a turn on every port access.
   A 20us quantum keeps the lock traffic negligible while staying well inside one
   80us timer step; sub-quantum time is carried, not dropped. */
/* Free-running microsecond clock for the video VDD's CRT timebase (GH #55
   follow-up). The VDD takes its clock as a hook so it stays pure C and off-VM
   testable; this is what the host hands it. Monotonic and never reset -- the VDD
   only ever takes it modulo a frame period, so the origin does not matter.
   Same source as HostPitSync(): QueryPerformanceCounter, which is why the guest's
   retrace and its PIT cannot drift against each other. */
static UINT64 HostTimeMicrosecondsQpc(VOID)
{
    static LARGE_INTEGER frequency, base;
    LARGE_INTEGER now;
    if (!frequency.QuadPart) {
        if (!QueryPerformanceFrequency(&frequency) || !frequency.QuadPart) return 0;
        QueryPerformanceCounter(&base);
    }
    QueryPerformanceCounter(&now);
    return (UINT64)(((now.QuadPart - base.QuadPart) * MICROSECONDS_PER_SECOND) / frequency.QuadPart);
}
enum { TSC_RESYNC_MIN_US = 500 };   /* HostTimeMicroseconds: an interval long enough to re-derive the rate */
/* ── #183: THE BEAM CLOCK WITHOUT A SYSCALL PER READ. Every 3DAh status read asks this
     for the time, and a retrace-wait loop reads 3DAh flat out: s82's profiler put 40%
     of Wolf3D's exec-thread samples in ntdll, i.e. QueryPerformanceCounter, which is a
     system call on XP, plus a 64-bit divide. Now: the CPU's own time-stamp counter,
     interpolated between QPC ANCHORS taken at most every ~2 ms, with the divide turned
     into a multiply. Per thread (__thread), so no lock and no torn shared state; the
     rate is re-derived at every anchor, so a CPU that changes its clock cannot drift
     us more than one anchor interval; and it never runs backwards within a thread.
     Falls back to plain QPC until the first two anchors have given it a rate. */
typedef struct { UINT64 TscBase, MicrosecondsBase, Last, Resync; UINT32 Multiplier; } HOST_CLOCK;
static DWORD g_ClockTls = TLS_OUT_OF_INDEXES;
UINT64 HostTimeMicroseconds(VOID)
{
    UINT32 low, high;
    UINT64 tsc, microseconds;
    HOST_CLOCK *clock;
    /* Per thread through Win32 TLS (this build has no CRT, so no __thread): the slot
       is allocated once, and each thread's clock on its first call. */
    if (g_ClockTls == TLS_OUT_OF_INDEXES) {
        DWORD tlsIndex = TlsAlloc();
        if (tlsIndex == TLS_OUT_OF_INDEXES) return HostTimeMicrosecondsQpc();
        if (InterlockedCompareExchange((volatile LONG *)&g_ClockTls, (LONG)tlsIndex,
                                       (LONG)TLS_OUT_OF_INDEXES) != (LONG)TLS_OUT_OF_INDEXES)
            TlsFree(tlsIndex);                            /* another thread won the race */
    }
    clock = (HOST_CLOCK *)TlsGetValue(g_ClockTls);
    if (!clock) {
        clock = (HOST_CLOCK *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *clock);
        if (!clock) return HostTimeMicrosecondsQpc();
        TlsSetValue(g_ClockTls, clock);
    }
    __asm__ __volatile__("rdtsc" : "=a"(low), "=d"(high));
    tsc = ((UINT64)high << DWORD_SHIFT) | low;
    if (!clock->Multiplier || tsc - clock->TscBase >= clock->Resync) {
        UINT64 qpcMicroseconds = HostTimeMicrosecondsQpc();
        if (clock->TscBase && qpcMicroseconds > clock->MicrosecondsBase && tsc > clock->TscBase) {
            UINT64 tscDelta = tsc - clock->TscBase, microsecondsDelta = qpcMicroseconds - clock->MicrosecondsBase;
            if (microsecondsDelta >= TSC_RESYNC_MIN_US) {                       /* a usable interval: re-derive rate */
                UINT64 multiplier = (microsecondsDelta << DWORD_SHIFT) / tscDelta;
                clock->Multiplier = (UINT32)(multiplier > MAXDWORD ? MAXDWORD : multiplier);
                if (clock->Multiplier) clock->Resync = (TSC_RESYNC_INTERVAL_ULL << DWORD_SHIFT) / clock->Multiplier;   /* ~2 ms of ticks */
            }
        }
        if (!clock->Multiplier || !clock->TscBase || qpcMicroseconds - clock->MicrosecondsBase >= TSC_RESYNC_MIN_US) { clock->TscBase = tsc; clock->MicrosecondsBase = qpcMicroseconds; }
        if (!clock->Multiplier) { if (qpcMicroseconds > clock->Last) clock->Last = qpcMicroseconds; return clock->Last; }
    }
    microseconds = clock->MicrosecondsBase + (((tsc - clock->TscBase) * (UINT64)clock->Multiplier) >> DWORD_SHIFT);
    if (microseconds < clock->Last) microseconds = clock->Last;            /* never backwards within a thread */
    clock->Last = microseconds;
    return microseconds;
}

enum { PIT_PACE_WAIT_PERIODS = 4 };   /* PitPacerThread: the event wait, in pacer periods */
#define IRQ0_NOTE_ASYNC_CS 0xFFFF   /* g_Irq0NoteCs: the tick was delivered async, no CS:IP to note */
#define COURIER_NORMAL_BUDGET_US 300u
/* ── TWO LEVERS, BECAUSE THE PERIOD IS NOT ONE. ──────────────────────────────────────
     Session 26, user-confirmed on bare metal: the pacer costs SKYROADS its input --
     "pressing left/right arrows throws you off the road", gone the moment pitpace=0.
     It is NOT the clock (three port-profile-matched runs: 183.6 pre-pacer, 185.0, 184.1
     raises/s, within 0.8%) and it is NOT the pacer's CALL RATE either: 4 ms felt exactly
     like 1 ms. What does not change with the period is the number of RAISES, and a raise
     is where HostIrqSink does a SuspendThread/SetThreadContext round trip on the guest
     **while holding g_Lock** -- deliberately, because the lock is the interlock that
     stops us suspending a lock holder (see the note in HostIrqSink).
     Before the pacer, the UI thread called HostPitSync itself and therefore DID that
     suspend inline and carried on. Now the pacer does it and the UI thread BLOCKS on the
     lock instead -- and the UI thread is the one that turns WM_KEYUP into a break code,
     so a key stays down. That is rate-independent, which is exactly the shape measured.
   ► So make the two candidate mechanisms separately testable, from the share, without a
     rebuild -- this is an empirical loop with a human as the instrument and each round
     costs the user a play session:
       pitprio.txt  0-4  pacer thread priority (idle/below/normal/above/HIGHEST=default)
       pitinj.txt   0/1  may the PACER perform the async injection? 1 = as shipped.
                         0 = the pacer only ADVANCES THE CLOCK and leaves delivery to the
                             UI and exec threads, exactly as before the pacer existed.
   ⚠ Both default to the shipped behaviour, so an absent file changes nothing. */
/* ── ★★★ NORMAL, NOT HIGHEST -- and the reason is the s61 crystal split. ─────────
     The pacer was HIGHEST because in the OLD design it fought the renderer for
     g_Lock every millisecond and had to win to keep the clock moving. Now generation
     runs under its own micro-lock (g_PitCs, see HostPitGenerate) and never blocks
     on g_Lock at all -- so the pacer no longer NEEDS to outrank anything, and at
     HIGHEST it did active harm: on the single-core rig it starved the ~64 Hz present
     thread, which the user saw as stepped palette fades and dropped in-game frames.
     Dropped to NORMAL, the run was user-confirmed "absolutely perfect".
   ⚠ It only needs to WAKE ~1000/s (timeBeginPeriod(1) + Sleep(1)); NORMAL does that
     fine, and the crystal is covered by cooperative delivery if a wake is ever late.
     pitprio.txt still overrides for A/B (0=idle..4=highest) but the default is the
     validated one. */
static INT  g_PitPacePriority = THREAD_PRIORITY_NORMAL;
static INT  g_PitPaceInject = 1;
static LONGLONG Int15QpcAfterMicroseconds(DWORD microseconds)
{
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now); QueryPerformanceFrequency(&frequency);
    return now.QuadPart + (LONGLONG)(((UINT64)microseconds * (UINT64)frequency.QuadPart) / MICROSECONDS_PER_SECOND_ULL);
}
static VOID Int15EventPoll(VOID)            /* pacer thread */
{
    LONGLONG end = g_Int15EventEnd;
    LARGE_INTEGER now;
    if (!end) return;
    QueryPerformanceCounter(&now);
    if (now.QuadPart < end) return;
    g_Int15EventEnd = 0;
    *(volatile BYTE *)(ULONG_PTR)g_Int15EventLinear |= BIOS_EVENT_WAIT_POSTED;      /* the caller's flag: time is up */
    *(volatile BYTE *)(ULONG_PTR)(BIOS_BDA_BASE + BIOS_BDA_WAIT_ACTIVE) = BIOS_BDA_WAIT_NONE;               /* 40:A0 wait no longer active  */
    ++g_Int15Posted;
}
typedef MMRESULT (WINAPI *PFN_TIME_SET_EVENT)(UINT, UINT, LPTIMECALLBACK, DWORD_PTR, UINT);
static HANDLE   g_PitPaceEvent;
static MMRESULT g_PitPaceTimer;
static VOID PitPacerTimerStart(HMODULE winmmModule)
{
    PFN_TIME_SET_EVENT timeSetEvent = winmmModule ? (PFN_TIME_SET_EVENT)GetProcAddress(winmmModule, HOST_EXPORT_TIME_SET_EVENT) : NULL;
    if (!timeSetEvent || g_PitPaceMs <= 0) return;
    g_PitPaceEvent = CreateEventA(NULL, FALSE, FALSE, NULL);      /* auto-reset */
    if (!g_PitPaceEvent) return;
    g_PitPaceTimer = timeSetEvent((UINT)g_PitPaceMs, 0, (LPTIMECALLBACK)g_PitPaceEvent, 0,
                          TIME_PERIODIC | TIME_CALLBACK_EVENT_SET);
    if (!g_PitPaceTimer) { CloseHandle(g_PitPaceEvent); g_PitPaceEvent = NULL; }
}
static DWORD WINAPI PitPacerThread(LPVOID param)
{
    (VOID)param;
    /* Above the guest but below the audio pump, so it can never starve either. */
    SetThreadPriority(GetCurrentThread(), g_PitPacePriority);
    while (g_Running) {
        /* s61: the pacer is the crystal's drive shaft and touches ONLY the crystal's
           lock; pitinj.txt=0 still means "advance the clock, attempt nothing", now
           expressed as which function runs instead of a flag threaded through a
           shared sink. It can no longer be made to wait by the renderer. */
        HostPitGenerate();
        if (g_PitPaceInject) HostPitDeliver();
        Int15EventPoll();                                    /* #206 */
        ++g_PitPaceCalls;
        /* The timeout only matters if the timer stops: then this is the Sleep loop. */
        if (g_PitPaceTimer) WaitForSingleObject(g_PitPaceEvent, (DWORD)g_PitPaceMs * PIT_PACE_WAIT_PERIODS);
        else                 Sleep((DWORD)g_PitPaceMs);
    }
    return 0;
}
enum { COURIER_OFF = 0, COURIER_ON = 1, COURIER_NORMAL_PRIORITY = 2, COURIER_WAIT_MS = 50, COURIER_NORMAL_TRIES = 2, COURIER_TRIES_MAX = 1000000 };   /* courier.txt's modes; the courier's wait and tries */
/* ── ★★★ THE TICK COURIER: A RETRY THAT IS NOT TIED TO THE RAISE CADENCE. (s61) ──
     MEASURED, in-game, 45 s, Unlimited, synthetic keys (the fair A/B is in
     docs/STATE.md SESSION 61). Across the 75 gaps where Skyroads missed at least one
     of its own 180 Hz ticks:

         IRQ0s the 8254 generated                192
         injection attempts actually made         76
         attempts NEVER MADE                     116   <-- and yields = 116, exactly
         attempts that bailed not_in_exec          1

     Every tick that lost its injection attempt lost it to the KEYBOARD YIELD in
     HostIrqSink: when a key is pending, the timer's one async opportunity per raise
     is handed to IRQ1 instead. That is not a bug in the yield -- turning it off
     (keyirq.txt = 0) is measured to put HALF of all keystrokes past 64 ms, worst
     1864 ms, which is the "loses keys" result this file already records twice. The
     yield is load-bearing for input.

   ► SO THE FAULT IS NOT WHO WINS, IT IS THAT WINNING COSTS THE LOSER A WHOLE PERIOD.
     IRQ0 and IRQ1 compete for one scarce thing: a moment when the guest is in exec
     with interrupts on. HostIrqSink offers exactly ONE such moment per raise, so a
     tick that loses waits 5.56 ms for the next one -- and KEYIRQ_MAX_YIELD allows
     three losses in a row, which is 16.7 ms. The worst gap measured is 20 ms and its
     account reads raise=3, yld=2, att=1: three ticks generated, two given to keys,
     one attempted. That is the wobble, in four numbers.

   ► AND AN IMMEDIATE SECOND ATTEMPT CANNOT WORK, which is why this is a thread and
     not two lines in the sink. Injecting IRQ1 leaves the guest entering its INT 09h
     with interrupts off, so a retry in the same breath is refused on IF -- correctly.
     The tick has to wait for the guest's key handler to IRET, which is microseconds
     away, not milliseconds. Nothing in the old structure could wait that long or that
     precisely: the sink is called from the PIT and only from the PIT.

   ⚠⚠ WHY IT MUST NOT TAKE g_Lock, and why that is safe HERE and was not before.
     HostIrqSink's long note is right: holding g_Lock across a suspend is what
     guarantees the thread being suspended is not a lock holder, and simply moving the
     call outside was tried and made things worse. This is the "separate suspend-safe
     handshake" that note names as the prerequisite -- the same one the CPU throttle
     already uses and has shipped with for a session: confirm g_InExec AFTER the
     suspend has landed (GetThreadContext is what makes "landed" true), and resume
     instantly if it reads 0. g_InExec is set only around VdmRunGuest, where the thread
     holds nothing, and is cleared before any HOST_LOCK. See AsyncInjectIrq's
     re-check, which is where that handshake actually lives.
     Taking g_Lock would also defeat the purpose: the measured worst lock WAIT is
     17 ms, which is longer than the gap being repaired.

   ⚠ V86 ONLY, ON PURPOSE. A protected-mode client is fed almost entirely by the async
     path and is deliberately throttled to one attempt per sync; session 23 measured
     that buying more attempts there tripled the cost and moved delivery by nothing.
     This is a different mechanism aimed at a different fault, and gating it on
     !g_DpmiPm means it CANNOT regress Doom -- the one guest that throttle protects.
   ⚠ ONE TICK PER WAKE. The backlog is not drained in a burst: g_Irq0Pending is a
     saturating latch precisely so a stall is not repaid as a flood, and session 22
     proved that compressing game time is catastrophic. Placing one tick and going
     back to sleep keeps the guest's clock monotonic.
   ⚠ BOUNDED IN TIME, NOT JUST IN SPINS. Each retry is a full suspend round trip
     (~tens of us), so an unbounded spin against a guest that keeps interrupts off
     would burn a core to no purpose. It gives up after COURIER_BUDGET_US and lets the
     next raise re-arm it -- which is exactly the old behaviour, so the worst case is
     no worse than today. */
#define COURIER_BUDGET_US 3000u
static DWORD WINAPI TickCourierThread(LPVOID parameter)
{
    (VOID)parameter;
    /* ── ⛔ courier = 1 IS REFUTED, USER-CONFIRMED. IT COLLAPSES PROGRESSIVELY. ─────
         Four runs of the same level: "1. Absolutely fine  2. Degraded slightly
         3. Even worse, and then it was like time flew forward instantly ... it just
         instantly went to the beginning of the level  4. Basically like I have
         dementia." The burst at (3) is a drained backlog, and the monotonic decay is
         a POSITIVE FEEDBACK LOOP that this design has by construction:
             tick undelivered -> more pending -> courier spins harder (up to
             COURIER_BUDGET_US of suspend/get-context/resume round trips, at
             THREAD_PRIORITY_HIGHEST, on a SINGLE-CORE box) -> the guest gets LESS
             CPU -> the clock falls further behind -> the courier spins harder still.
         The courier spends the very resource it is trying to create. Session 22 and
         session 23 each learned a version of this ("extra attempts merely pay full
         SuspendThread round trips to be told no"); this is the third.
       ► courier = 2 is the gentle arm, and the difference is entirely about not
         competing with the guest: NORMAL priority (it can no longer preempt the
         thread it is waiting on), a budget in HUNDREDS of microseconds rather than
         milliseconds, and a hard cap of two attempts per wake so a wake cannot
         become a spin at all. If that still decays, the retry idea is dead and the
         answer is the port-trap cost, not arbitration. */
    SetThreadPriority(GetCurrentThread(),
                      g_CourierOn == COURIER_NORMAL_PRIORITY ? THREAD_PRIORITY_NORMAL
                                        : THREAD_PRIORITY_HIGHEST);
    while (g_Running) {
        LARGE_INTEGER start, now;
        /* The 50 ms cap is a backstop, not the mechanism: the raise site signals us.
           Without it a lost signal would park the courier for the rest of the run. */
        WaitForSingleObject(g_CourierEvent, COURIER_WAIT_MS);
        ++g_CourierWakes;
        if (!g_CourierOn || !g_QiSuspended || g_DpmiPm || !g_HostCpu) continue;
        if (g_Irq0Pending <= 0) continue;
        QueryPerformanceCounter(&start);
        {   UINT budgetMicroseconds = (g_CourierOn == COURIER_NORMAL_PRIORITY) ? COURIER_NORMAL_BUDGET_US : COURIER_BUDGET_US;
            INT triesLeft     = (g_CourierOn == COURIER_NORMAL_PRIORITY) ? COURIER_NORMAL_TRIES : COURIER_TRIES_MAX;
        for (;;) {
            if (!g_Running || g_Irq0Pending <= 0) break;
            if (triesLeft-- <= 0) { ++g_CourierGiveUp; break; }
            if (g_InExec == 0) {
                /* Not executing guest code: nothing to inject into, and a suspend
                   would only probe a thread that may be holding a lock. Yield and
                   look again -- the guest is being serviced and will be back. */
                SwitchToThread();
            } else {
                ++g_CourierTries;
                if (AsyncInjectIrq(PIC_IRQ_TIMER)) {
                    InterlockedDecrement(&g_Irq0Pending);
                    PmTickTake();
                    g_Irq0NoteCs = IRQ0_NOTE_ASYNC_CS; g_Irq0NoteIp = 0;   /* delivered async */
                    Irq0DeliveredNote();
                    ++g_CourierInjected;
                    break;                      /* one tick per wake -- see above */
                }
                SwitchToThread();
            }
            QueryPerformanceCounter(&now);
            if (QpcMicroseconds(now.QuadPart - start.QuadPart) >= budgetMicroseconds) {
                ++g_CourierGiveUp;
                break;
            }
        } }
    }
    return 0;
}
#define CPU_MHZ_UNKNOWN_U 0xFFFFFFFFu   /* HostCpuMhz: not read yet */
/* #224: THIS PC's own clock in MHz (0 = unknown), from the CPU's ~MHz registry value
   -- what Windows itself shows in System Properties. Rungs at or above it are greyed. */
static UINT HostCpuMhz(VOID)
{
    static UINT mhz = CPU_MHZ_UNKNOWN_U;
    if (mhz == CPU_MHZ_UNKNOWN_U) {
        HKEY key; DWORD value = 0, size = sizeof value, valueType = 0;
        mhz = 0;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, HOST_REG_CPU_KEY,
                          0, KEY_READ, &key) == ERROR_SUCCESS) {
            if (RegQueryValueExA(key, HOST_REG_CPU_MHZ, NULL, &valueType, (BYTE *)&value, &size) == ERROR_SUCCESS
                && valueType == REG_DWORD) mhz = (UINT)value;
            RegCloseKey(key);
        }
    }
    return mhz;
}
static DWORD  g_CpuSpeedDebtMaximumMicroseconds;   /*   the largest debt it was cut from */
/* ★ THE MEASURED RUN PHASE, in microseconds, and it is the number that made this
     feature work. We ASK for a 1 ms run; what the guest actually gets is that plus
     Sleep's inaccuracy plus whatever it costs the kernel to stop a thread inside
     VdmStartExecution -- and the hold is priced off this, not off the 1 ms. */
static DWORD  g_CpuSpeedRanMicroseconds;
/* The same window measured as WALL CLOCK. Printed next to ran_us so the gap between
   them -- our own servicing overhead, the thing the guest is no longer billed for --
   is a number in the log rather than an inference. */
static DWORD  g_CpuSpeedWallMicroseconds;
/* ── ★ THE GRANULARITY SLIDER AND ITS AUTO-DETECT. See cpuspeed.h. ───────────────
     g_CpuSpeedGranularityMs is the TARGET PERIOD: 0 = auto (measure and choose). The
     measured round-trip cost is what auto is derived from, and it is reported so a
     surprising period can be traced to the machine rather than guessed at. */
static UINT g_CpuSpeedGranularityMs  = CPUSPEED_GRAN_AUTO;   /* 0=auto; cpugran.txt knob */
static DWORD    g_CpuSpeedRoundTripMicroseconds;        /* measured suspend round trip, microseconds */
static DWORD    g_CpuSpeedPeriodMs;    /* what auto actually chose, or the setting  */
/* ── ★★ THE ROUND TRIP IS MEASURED WHERE IT HAPPENS, NOT IN A SYNTHETIC BURST. ───
 * ⚠⚠ THE FIRST CUT DID IT AT STARTUP and the number was meaningless: it reported
 *    **3 us**, because at that moment the exec thread is not necessarily inside
 *    VdmStartExecution, and suspending an idle thread costs almost nothing while
 *    suspending one inside a syscall makes the kernel unwind it out of V86 first.
 *    The instrument was not measuring the thing it is named for -- the same defect
 *    class as [[instrument-must-not-infer-its-own-frame]], and it fed a plausible
 *    wrong answer straight into the granularity arithmetic (auto picked the 2 ms
 *    floor on the strength of it).
 * ⇒ So sample it in the REAL loop: the interval from calling SuspendThread to
 *   GetThreadContext returning, on a thread that is genuinely in exec, which is
 *   exactly the cost the granularity floor is made of. Free, self-correcting, and
 *   it cannot be measured under conditions that do not occur.
 * ⚠ KEEP THE MINIMUM, not the mean. Contention makes individual round trips long
 *   and a slider cannot fix contention; the floor is what the mechanism can do when
 *   nothing is in the way, which is the right basis for "how fine can this go".
 * ⚠ AND IT WARMS UP. The first few periods run at the default granularity because
 *   nothing has been sampled yet; the period is recomputed as the estimate settles,
 *   which is why g_CpuSpeedPeriodMs is cleared on a change rather than latched. */
static VOID CpuSpeedNoteRoundTrip(DWORD microseconds)
{
    if (!microseconds) return;
    if (!g_CpuSpeedRoundTripMicroseconds || microseconds < g_CpuSpeedRoundTripMicroseconds) {
        g_CpuSpeedRoundTripMicroseconds = (DWORD)microseconds;
        g_CpuSpeedPeriodMs = 0;          /* re-derive auto from the better estimate */
    }
}

/* ── ★★★ THE GUEST IS ONLY CHARGED FOR TIME IT WAS ACTUALLY EXECUTING. ───────────
 * ⚠⚠ THE BUG THIS FIXES, MEASURED ON THE RIG 2026-09-09 WITH tests/probes/dos/mixbench:
 *    at index 11 -- the menu says 66 MHz -- five shapes of work were delivered at
 *        ALU 68   MEM 209   VID13 92   VID12 23   PORT 1     (MHz apparent)
 *    The ALU figure is right because ALU code is what CPUSPEED_REF_MHZ was
 *    calibrated from (cpubench.asm: "NO REGISTER IN THE LOOP TOUCHES MEMORY"). The
 *    PORT figure is 66x LOW, and port I/O is not a corner case: Doom's mode-Y
 *    drawing does ~43,000 port writes a second and Skyroads' AdLib helper spends 43
 *    port accesses per OPL register. The user's report was "66 MHz is unplayably
 *    slow; my real 486 DX2-66 played these fine", and this is why.
 *
 * ► THE CAUSE IS WHAT `ran_us` MEASURED. It was wall clock from the moment we let
 *   the guest go to the moment a suspend landed -- and a DOS guest spends much of
 *   that window NOT EXECUTING but trapped inside US, being serviced. Every port
 *   write is an IOPL-0 #GP reflected out to host code and costs 2.33 us of wall
 *   clock (measured: iobench case 3, 117,920 accesses in 5 ticks = 429,400/s).
 *   Charging that to the guest bills it for our own overhead at the guest's rate,
 *   and then the duty cycle multiplies the bill: at a 1.8% duty the guest is held
 *   fifty-four times as long as it "ran", so every microsecond we mis-attribute
 *   costs it fifty-four more. Hardware-touching code is therefore penalised in
 *   proportion to how slow WE are at servicing it, which is precisely backwards.
 *
 * ⇒ So count only the time the guest was inside VdmRunGuest/DpmiEnterProtectedMode, which is
 *   exactly what g_InExec already brackets, and price the hold off THAT. Our
 *   servicing time is simply not the guest's to pay for.
 *
 * ⚠ 32 BITS OF MICROSECONDS, ON PURPOSE. A 64-bit accumulator cannot be read
 *   atomically on 32-bit x86, and the throttle samples it while the exec thread is
 *   RUNNING (right after ResumeThread), so a torn read is a real possibility and
 *   would produce a garbage hold. A uint32 of microseconds wraps every ~71 minutes
 *   and unsigned subtraction gives the correct delta across the wrap, which is all
 *   this is ever used for -- differences of milliseconds.
 * ⚠ AND IT IS READABLE MID-INTERVAL. The whole point is to sample it at the instant
 *   the guest is suspended, which is INSIDE a VdmRunGuest that has not returned yet, so
 *   the accumulated total alone would be stale by exactly the interval that matters.
 *   ExecMicrosecondsNow() adds the open interval. Safe when the target is suspended (nothing
 *   can change under us) and harmlessly approximate when it is not.
 * ⚠ GATED ON THROTTLING BEING ON. Two QueryPerformanceCounter calls per trap is
 *   nothing next to a 2.33 us trap, but at Unlimited -- the default, and how every
 *   measurement in this project was taken -- it buys nothing, so it is not paid. */
static volatile LONG g_ExecMicrosecondsAccumulated;      /* completed guest-execution time, us      */
/* ── ⚠⚠ THE OPEN INTERVAL IS ONE 32-BIT VALUE, AND THE FIRST CUT GOT THIS WRONG. ──
     It stored the entry QueryPerformanceCounter as TWO LONGs, hi and lo, and
     reassembled them in the reader -- which is the identical torn-read hazard the
     comment above rejects for the accumulator, reintroduced on the very next line.
     The reader runs on the throttle thread while the exec thread is RUNNING, so an
     entry landing between the two reads yields a value from neither sample: a
     garbage interval, a garbage `ran_us`, and a hold computed from it.
     Measured symptom: `ran_us` stayed ~1500 us with no sleep in the run phase at
     all, and ALU delivered 123 MHz against accounting that said 64.
   ⇒ Store MICROSECONDS SINCE A SESSION BASE in a single LONG. 32 bits of
     microseconds wraps every ~71 minutes and unsigned subtraction spans the wrap,
     which is all this is used for. One aligned 32-bit store is atomic on x86, so
     the reader cannot see half of it. 0 is the "no interval open" sentinel. */
static LARGE_INTEGER g_ExecQpcBase;    /* fixed at first use; never moves         */
static volatile LONG g_ExecEnterMicroseconds;    /* open interval start, us since base; 0=none */
static volatile LONG g_ExecTimingOn;   /* only while a throttle is actually set   */

static LONG ExecClockMicroseconds(VOID)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!g_ExecQpcBase.QuadPart) g_ExecQpcBase = now;
    /* +1 so a genuine reading can never collide with the 0 sentinel. */
    return (LONG)QpcMicroseconds(now.QuadPart - g_ExecQpcBase.QuadPart) + 1;
}
static VOID ExecEnterMark(VOID)
{
    if (!g_ExecTimingOn) return;
    InterlockedExchange(&g_ExecEnterMicroseconds, ExecClockMicroseconds());
}
static VOID ExecLeaveMark(VOID)
{
    LONG enterMicroseconds, now;
    if (!g_ExecTimingOn) return;
    enterMicroseconds = g_ExecEnterMicroseconds;
    if (!enterMicroseconds) return;
    now = ExecClockMicroseconds();
    InterlockedExchangeAdd(&g_ExecMicrosecondsAccumulated, (LONG)(ULONG)(now - enterMicroseconds));
    InterlockedExchange(&g_ExecEnterMicroseconds, 0);
}
/* The accumulator PLUS the interval currently open, so a sample taken while the
   guest is stopped inside VdmRunGuest is not short by that whole interval. */
static LONG ExecMicrosecondsNow(VOID)
{
    LONG accumulated = g_ExecMicrosecondsAccumulated;
    LONG enterMicroseconds = g_ExecEnterMicroseconds;                  /* one atomic 32-bit read */
    if (enterMicroseconds) accumulated += (LONG)(ULONG)(ExecClockMicroseconds() - enterMicroseconds);
    return accumulated;
}
enum { CPUSPEED_RELEASE_WAIT_MS = 5, CPUSPEED_CATCH_GRACE_MS = 250 };   /* the cooperative park */
/* ── ★★ #225: THE COOPERATIVE CATCH. (user, round 11: "slow at busy times -- the
     crossfade, keyboard input") The throttle catches the guest by suspending it
     INSIDE VdmRunGuest; a guest that is in OUR servicing (a port trap, a BOP) cannot be
     suspended, so it ran free -- 56,820 missed catches in one Skyroads run at
     486DX2-66 -- and the debt came back as one hold of up to a second. A 178 ms hold
     overflows the four-tick IRQ0 latch (22 ms at 180 Hz), so game time was thrown
     away: IRQ0 fell from 180/s to 91..170/s exactly when the game was busiest.
   ► So when the throttle wants the guest and finds it outside VdmRunGuest, it RAISES
     g_CpuSpeedCatchRequest, and the exec thread parks here at its next re-entry -- the
     spot the #219 pause already parks at, where it holds no lock -- until the
     throttle has taken its hold and lowers the request. The guest never runs free for
     more than one trap, so the holds stay at their normal size.
   ► Only a throttle raises the request, so at Unlimited this is one volatile read.
   ⚠ BOUNDED: a throttle that stops answering (its thread wedged, g_Running falling)
     cannot park the guest for longer than the longest legal hold plus slack. */
static volatile LONG g_CpuSpeedCatchRequest;   /* throttle -> exec: park at the re-entry   */
static volatile LONG g_CpuSpeedParked;      /* exec -> throttle: parked, not in exec     */
static HANDLE        g_CpuSpeedRelease;     /* auto-reset: wakes the park early          */
static VOID CpuSpeedCooperativePark(VOID)
{
    DWORD start;
    if (!g_CpuSpeedCatchRequest) return;
    start = GetTickCount();
    InterlockedExchange(&g_CpuSpeedParked, 1);
    while (g_CpuSpeedCatchRequest && g_Running && !g_PauseWant) {
        if (GetTickCount() - start > CPUSPEED_MAX_OFF_MS + CPUSPEED_CATCH_GRACE_MS) { ++g_CpuSpeedCooperativeTimeouts; break; }
        if (g_CpuSpeedRelease) WaitForSingleObject(g_CpuSpeedRelease, CPUSPEED_RELEASE_WAIT_MS);
        else Sleep(1);
    }
    InterlockedExchange(&g_CpuSpeedParked, 0);
}

/* ── #225: WHAT THE THROTTLE DID, SECOND BY SECOND, on IRQ0TL's time base, so a
     dip in the guest's clock can be read against exec / hold / missed catches /
     port traps / timer raises in that same second. Written by the throttle thread
     only (the io and raise columns are snapshots of counters owned elsewhere). */
static DWORD g_ControlExecMicroseconds[IRQ0TL_SECS], g_ControlHoldMicroseconds[IRQ0TL_SECS], g_ControlMissed[IRQ0TL_SECS];
static DWORD g_ControlCooperative[IRQ0TL_SECS], g_ControlIo[IRQ0TL_SECS], g_ControlRaise[IRQ0TL_SECS];
static DWORD g_ControlRunMicroseconds[IRQ0TL_SECS];
static INT CpuSpeedTimelineSeconds(VOID)
{
    LARGE_INTEGER now; DWORD seconds;
    if (!g_Irq0Start) return -1;
    QueryPerformanceCounter(&now);
    seconds = QpcMicroseconds(now.QuadPart - g_Irq0Start) / MICROSECONDS_PER_SECOND_U;
    if (seconds >= IRQ0TL_SECS) return -1;
    g_ControlIo[seconds] = g_EventIo; g_ControlRaise[seconds] = g_Irq0RaiseCount;
    return (INT)seconds;
}
static VOID CpuSpeedTimelineDump(PCSTR tag)
{
    static PCSTR const names[7] = { "exec_ms", "hold_ms", "missed", "coop", "io", "raise", "run_us" };
    CHAR buffer[1400], *cursor; UINT column, second, last = 0;
    for (second = 0; second < IRQ0TL_SECS; ++second) if (g_ControlExecMicroseconds[second] || g_ControlHoldMicroseconds[second]) last = second;
    for (column = 0; column < 7; ++column) {
        DWORD prev = 0;
        cursor = buffer; cursor = LogPut(cursor, tag); cursor = LogPut(cursor, " "); cursor = LogPut(cursor, names[column]); cursor = LogPut(cursor, "=");
        for (second = 0; second <= last; ++second) {
            DWORD value = 0;
            switch (column) {
            case 0: value = g_ControlExecMicroseconds[second] / MICROSECONDS_PER_MILLISECOND_U; break;
            case 1: value = g_ControlHoldMicroseconds[second] / MICROSECONDS_PER_MILLISECOND_U; break;
            case 2: value = g_ControlMissed[second]; break;
            case 3: value = g_ControlCooperative[second]; break;
            case 4: value = g_ControlIo[second]    ? g_ControlIo[second]    - prev : 0; if (g_ControlIo[second])    prev = g_ControlIo[second];    break;
            case 5: value = g_ControlRaise[second] ? g_ControlRaise[second] - prev : 0; if (g_ControlRaise[second]) prev = g_ControlRaise[second]; break;
            case 6: value = g_ControlRunMicroseconds[second]; break;
            }
            cursor = LogPut(cursor, second ? "," : ""); cursor = LogDecimal(cursor, value);
        }
        cursor = LogPut(cursor, "\r\n");
        LogAppend(LOG_PATH, buffer, cursor);
    }
}

/* Recompute the duty from the setting. One function so the menu, the dialog and
   the file knob cannot each arrive at a different answer. */
static VOID CpuSpeedRecompute(VOID)
{
    LONG duty = (LONG)CpuSpeedDutyBp((UINT)g_CpuSpeedIndex, g_CpuSpeedReferenceMhz);
    InterlockedExchange(&g_CpuSpeedDuty, duty);
    InterlockedExchange(&g_CpuSpeedDutyRm, (LONG)CpuSpeedRealModeDutyBp((UINT)duty));   /* #225 */
    /* Pay for the per-trap timestamping only while it is actually being used. */
    InterlockedExchange(&g_ExecTimingOn, duty < CPUSPEED_BP_FULL ? 1 : 0);
    /* #225: a held guest must still see every vertical retrace (see vbl_owe_on). */
    g_Video.IsVblOweOn = (BYTE)(duty < CPUSPEED_BP_FULL ? 1 : 0);
    /* The period depends on the duty, so a speed change invalidates it. Zero means
       "work it out again on the next pass" -- including re-running auto-detect's
       arithmetic, which is why the measured round trip is kept separately. */
    g_CpuSpeedPeriodMs = 0;
}

/* ── ★★ PIN THE GUEST TO ONE CORE. (user request, 2026-09-09) ────────────────────
 * The rig is an Intel Core 2 Duo E8600 -- TWO cores -- and this host runs at least
 * five threads across them: the exec thread in V86, the UI thread, the PIT pacer at
 * 1 kHz, the audio pump, and the throttle. Three reasons that matters here:
 *
 *   1. THE CLOCK. Every number the throttle computes comes from
 *      QueryPerformanceCounter, and on XP QPC can be backed by the TSC -- which is
 *      PER CORE. A thread migrating between cores can therefore see time move
 *      unevenly or backwards, and a negative interval silently becomes a garbage
 *      hold. Pinning the exec thread removes the migration entirely.
 *   2. THE SUSPEND. The throttle catches the guest by suspending it; measured, the
 *      round trip is ~1 us when it lands and the cost is all in CATCHING it. Keeping
 *      the guest on its own core and the throttle off that core means the two are
 *      never competing for the same one.
 *   3. THE GUEST IS SINGLE-THREADED BY CONSTRUCTION. A DOS program cannot use a
 *      second core, so nothing is lost by confining it to one.
 *
 * ⚠ THE GUEST GETS A CORE TO ITSELF; EVERYTHING ELSE GETS THE REST. Pinning them all
 *   to the same core would be worse than not pinning at all -- the pacer alone wakes
 *   1000 times a second and would preempt the guest constantly. On a single-core box
 *   this does nothing at all, correctly, and says so rather than pretending.
 * ⚠ NOT DEFAULT. This changes scheduling for every guest on the machine and its
 *   benefit is unmeasured; a knob that alters timing must be opt-in until there is a
 *   number attached to it. cpuaff.txt = 1 to try it.
 */
static INT   g_CpuAffinityOn;              /* cpuaff.txt = 1 (file knob only): pin the guest */
static DWORD g_CpuAffinityGuest, g_CpuAffinityRest, g_CpuAffinityCpuCount;   /* what we chose       */
static VOID CpuAffinityApply(VOID)
{
    SYSTEM_INFO systemInfo;
    DWORD_PTR processMask = 0, systemMask = 0;
    if (!g_CpuAffinityOn || !g_HostCpu) return;
    GetSystemInfo(&systemInfo);
    g_CpuAffinityCpuCount = systemInfo.dwNumberOfProcessors;
    if (g_CpuAffinityCpuCount < 2) return;     /* one core: nothing to separate */
    if (!GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask) || !processMask)
        return;
    /* Lowest core the process may use goes to the guest; everything else to us. */
    {   DWORD_PTR guest = processMask & (~processMask + 1);   /* lowest set bit */
        DWORD_PTR rest  = processMask & ~guest;
        if (!rest) return;                              /* only one core available */
        g_CpuAffinityGuest = (DWORD)guest;
        g_CpuAffinityRest  = (DWORD)rest;
        SetThreadAffinityMask(g_HostCpu, guest);
        /* The threads that must NOT sit on the guest's core. The throttle pins
           itself below; the pacer and UI are pinned where they are created. */
        SetThreadAffinityMask(GetCurrentThread(), rest);
    }
}
enum { CPUSPEED_IDLE_SLEEP_MS = 4, CPUSPEED_CATCH_TIMEOUT_MS = 400, CPUSPEED_CATCH_NONE = 0, CPUSPEED_CATCH_CONTEXT = 1, CPUSPEED_CATCH_COOPERATIVE = 2 };   /* CpuSpeedThread */
#define CPUSPD_RUN_MIN_US 100ul   /* #225: shortest spun run slice */
static DWORD WINAPI CpuSpeedThread(LPVOID param)
{
    /* ── ★★★ THE CLOSED-LOOP THROTTLE. The whole control law is CpuSpeedStep (the
         long note in cpuspeed.h); this loop only FEEDS it two measured numbers and
         applies the hold it returns. It replaced ~150 lines of debt bookkeeping
         (owed_us + hold_us + pay_ms + two baselines that fell out of step and leaked
         TWICE) with one invariant: guest EXECUTION time E held to `duty` of WALL time
         T. A deterministic test drives the identical CpuSpeedStep against a
         simulated clock (tests/unit/cpuspeed_test.c), so the accuracy is proven
         off-hardware rather than argued from a rig number that reads the guest's own
         throttled clock.
       ► E EXCLUDES BOTH HOLDS AND HOST SERVICING, for free. ExecMicrosecondsNow() counts only
         wall time spent INSIDE VdmRunGuest, and it is sampled at RESUME and at SUSPEND:
         the hold (before the resume) and every trap-servicing gap (outside VdmRunGuest)
         are simply not in the difference. So E is true guest execution and E/T is the
         honest delivered speed -- the port-trap ceiling is not a special case, it is
         E naturally landing below T.
       ► T IS WINDOWED and rebaselines only when the guest is at or ahead of target
         (CpuSpeedStep's *reset), which banks no credit for having run slow, absorbs
         a stall, and keeps the 64-bit totals small. */
    LARGE_INTEGER wWin;                 /* wall origin of the current window         */
    LARGE_INTEGER wRun0;                /* wall clock at the last resume (run start)  */
    LONG eRun0;                         /* exec clock at the last resume (run start)  */
    UINT64 executedUs = 0ull;         /* guest execution this window, microseconds  */
    INT clockSet = 0;                   /* g_StartMs anchored at first throttled catch */
    UINT lastDuty = CPUSPEED_BP_FULL_U;         /* the duty the current window was priced at   */
    const UINT64 cAPMicroseconds = (UINT64)CPUSPEED_MAX_OFF_MS * MICROSECONDS_PER_MILLISECOND_ULL;
    (VOID)param;
    /* Above the guest, so a hold is actually a hold; below the audio pump and the PIT
       pacer, so throttling can never starve the clock or the mixer. */
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    CpuAffinityApply();
    if (g_CpuAffinityRest) SetThreadAffinityMask(GetCurrentThread(), g_CpuAffinityRest);
    g_StartMs = GetTickCount();
    QueryPerformanceCounter(&wWin);
    wRun0 = wWin;
    eRun0 = ExecMicrosecondsNow();
    while (g_Running) {
        /* #225: a real-mode program has its own share -- see CpuSpeedRealModeDutyBp. g_DpmiPm
           is set for the whole life of a protected-mode client, so this does not flip at
           every reflected real-mode interrupt (a flip would rebaseline the window). */
        UINT duty = (UINT)InterlockedCompareExchange(g_DpmiPm ? &g_CpuSpeedDuty
                                                                       : &g_CpuSpeedDutyRm, 0, 0);
        if (duty >= CPUSPEED_BP_FULL_U) {            /* Unlimited: no hold, no window to keep */
            if (g_CpuSpeedCatchRequest) { InterlockedExchange(&g_CpuSpeedCatchRequest, 0);
                                      if (g_CpuSpeedRelease) SetEvent(g_CpuSpeedRelease); }
            executedUs = 0ull; clockSet = 0; lastDuty = duty; Sleep(CPUSPEED_IDLE_SLEEP_MS);
            QueryPerformanceCounter(&wWin); wRun0 = wWin; eRun0 = ExecMicrosecondsNow();
            continue;
        }
        /* ── ⚠⚠ #225 (user, round 9): "the speeds degrade over time", and 100 -> 66 MHz
             "pretty much locked up" while Unlimited always recovered. TWO DEBTS THE
             WINDOW NEVER FORGAVE, both of which only the Unlimited branch above cleared:
           ► A #219 PAUSE WAS BILLED AS EXECUTION. The pause suspends the guest INSIDE
             VdmRunGuest, so g_InExec stays 1 and ExecMicrosecondsNow()'s open interval runs on for
             the whole pause: E grew by the pause's full length, and the hold priced off
             it (E/duty - T) was minutes of 1 s holds at a slow rung -- until the 60 s
             window cap finally rebaselined. So do not catch or bill while paused, and
             start a fresh window when the pause ends.
           ► A SPEED CHANGE KEPT THE OLD RUNG'S EXECUTION. E earned at 100 MHz, re-priced
             at 66 MHz's duty, is a debt of E x (1/d66 - 1/d100) that the guest never ran
             up. A new rung is a new window. */
        if (g_PauseWant || duty != lastDuty) {
            executedUs = 0ull; lastDuty = duty;
            QueryPerformanceCounter(&wWin); wRun0 = wWin; eRun0 = ExecMicrosecondsNow();
            if (g_PauseWant) {
                if (g_CpuSpeedCatchRequest) { InterlockedExchange(&g_CpuSpeedCatchRequest, 0);
                                          if (g_CpuSpeedRelease) SetEvent(g_CpuSpeedRelease); }
                Sleep(CPUSPEED_IDLE_SLEEP_MS); continue;
            }
        }
        /* ── THE RUN SLICE, AND IT MUST BE AT LEAST A MILLISECOND. ────────────────
             The invariant delivers the requested duty EXACTLY given a clean run and
             hold (proven off-hardware, cpuspeed_test.c). On real hardware it is
             defeated by ONE thing: the per-period cost of CATCHING the guest (a burst
             of SwitchToThread yields, measured ~hundreds of us). If the run phase is
             so short that the hold it earns is smaller than that catch cost, the catch
             cost itself becomes the throttle and the guest gets LESS than asked --
             measured on the rig, the fast settings delivered 0.37-0.62x while the slow
             ones (whose holds dwarf the catch cost) were dead on.
           ⇒ So let the guest run a Sleep-able chunk -- at least 1 ms -- every period.
             The hold that earns is then 1ms x (1-duty)/duty, which at these heavy
             throttles is tens to hundreds of ms: far larger than the catch cost, which
             vanishes into the noise. The price is CHUNKIER bursts (period = ~1ms/duty),
             and that is the honest, unavoidable trade of Sleep-based throttling on a
             fast host -- the only way to be both smooth AND accurate is a
             cycle-counting interpreter, which this project is not.
           ⚠ ...BUT ONLY WHERE IT IS NEEDED, AND THE TWO REGIMES ARE REAL. At a VERY
             slow setting the guest, caught immediately after a few microseconds, already
             earns a hold of many milliseconds -- far larger than the catch cost -- so it
             is accurate WITHOUT a run Sleep, and forcing a 1 ms run there makes the
             earned hold enormous and OVER-shoots (measured: 8 MHz delivered 3x with the
             floor, dead-on without it). So the floor applies only above a duty where the
             immediate-catch hold would otherwise be swamped by the catch cost. The
             threshold is rig-tuned (8 MHz = 14 bp fails with the floor, 33 MHz = 56 bp
             needs it) and keyed on the DUTY, which already folds in the reference. */
        {   DWORD runMicroseconds = g_CpuSpeedGranularityMs
                ? (DWORD)g_CpuSpeedGranularityMs * MICROSECONDS_PER_MILLISECOND_UL * duty / CPUSPEED_BP_FULL_UL : 0ul;
            if (duty >= CPUSPEED_RUN_FLOOR_BP && runMicroseconds < MICROSECONDS_PER_MILLISECOND_UL)
                runMicroseconds = MICROSECONDS_PER_MILLISECOND_UL;                       /* Sleep-able floor, fast half */
            /* ── #225: A CYCLE SHORTER THAN THE GUEST'S TIMER. The per-second record
                 put Skyroads' slow seconds on pure compute, IRQ0 capped at 97..133/s of
                 180: a tick placed during a hold stays in service until the handler runs
                 in the NEXT run slice, so one run+hold cycle passes at most one tick --
                 and at 486DX2-66 a 1 ms run earns a 7.2 ms hold, an 8.3 ms cycle against
                 a 5.6 ms tick. (Where the guest traps, each trap is another chance,
                 which is why busy I/O seconds reached 180.) So shorten the run until a
                 cycle is at most HALF the guest's timer period: the duty -- the speed --
                 is the same, only the rhythm is finer. Sub-millisecond runs cannot be
                 Slept, so they spin on the clock, yielding; only ever SHORTER than the
                 1 ms floor, so a guest on the BIOS 18.2 Hz tick is untouched. Placing
                 ticks from this thread instead was tried and REJECTED: without g_Lock it
                 raced the PIT's own delivery and injected 12,170 ticks of 5,934 raised. */
            if (runMicroseconds == MICROSECONDS_PER_MILLISECOND_UL && duty < CPUSPEED_BP_FULL_U) {
                DWORD reload = g_Pit.Reload ? (DWORD)g_Pit.Reload : PIT_FULL_COUNT_U;
                DWORD tickMicroseconds = (DWORD)((UINT64)reload * MICROSECONDS_PER_SECOND_ULL / PIT_INPUT_HZ_ULL);
                DWORD want = (DWORD)((UINT64)(tickMicroseconds / 2ul) * duty / CPUSPEED_BP_FULL_U);   /* cycle = run/duty */
                if (want < MICROSECONDS_PER_MILLISECOND_UL) runMicroseconds = want < CPUSPD_RUN_MIN_US ? CPUSPD_RUN_MIN_US : want;
            }
            {   INT second = CpuSpeedTimelineSeconds(); if (second >= 0) g_ControlRunMicroseconds[second] = (DWORD)runMicroseconds; }
            if (runMicroseconds >= MICROSECONDS_PER_MILLISECOND_UL) Sleep((DWORD)(runMicroseconds / MICROSECONDS_PER_MILLISECOND_UL));
            else if (runMicroseconds && duty >= CPUSPEED_RUN_FLOOR_BP) {
                LARGE_INTEGER runStart, runNow;
                QueryPerformanceCounter(&runStart);
                do { SwitchToThread(); QueryPerformanceCounter(&runNow); }
                while (g_Running && QpcMicroseconds(runNow.QuadPart - runStart.QuadPart) < runMicroseconds);
            }
            /* else: immediate catch -- correct for the slow half. */
        }
        /* ── CATCH THE GUEST INSIDE VdmRunGuest, measure, hold. The retry YIELDS rather
             than sleeping so it catches within microseconds of a re-entry; bounded,
             then it gives up (a guest blocked in a host call is not executing, so
             there is nothing to throttle -- and NOT resetting the window here is
             correct: the stall shows up as T growing while E does not, and the next
             at-or-ahead step forgives it). */
        {   DWORD started = GetTickCount();
            INT done = 0, spins = 0;
            LARGE_INTEGER parkStart;
            QueryPerformanceCounter(&parkStart);
#define CPUSPD_YIELD_BURST 200
            while (!done && g_Running && !g_PauseWant && GetTickCount() - started < CPUSPEED_CATCH_TIMEOUT_MS) {
                CONTEXT context; LARGE_INTEGER roundTrip0, roundTrip1, now; INT gotContext, caught = CPUSPEED_CATCH_NONE;
                if (!g_HostCpu || g_InExec == 0) {
                    /* #225: outside VdmRunGuest -- ask it to park at its next re-entry. */
                    if (g_HostCpu) {
                        InterlockedExchange(&g_CpuSpeedCatchRequest, 1);
                        if (g_CpuSpeedParked && g_InExec == 0) caught = CPUSPEED_CATCH_COOPERATIVE;
                    }
                    if (!caught) {
                        ++g_CpuSpeedMissed;
                        { INT second = CpuSpeedTimelineSeconds(); if (second >= 0) ++g_ControlMissed[second]; }
                        if (++spins < CPUSPD_YIELD_BURST) SwitchToThread(); else Sleep(1);
                        continue;
                    }
                } else {
                    QueryPerformanceCounter(&roundTrip0);
                    if (SuspendThread(g_HostCpu) == (DWORD)-1) { ++g_CpuSpeedMissed; Sleep(1); continue; }
                    context.ContextFlags = CONTEXT_CONTROL;
                    gotContext = GetThreadContext(g_HostCpu, &context);
                    QueryPerformanceCounter(&roundTrip1);
                    CpuSpeedNoteRoundTrip((DWORD)QpcMicroseconds(roundTrip1.QuadPart - roundTrip0.QuadPart));
                    if (gotContext && g_InExec != 0) caught = CPUSPEED_CATCH_CONTEXT;
                    else {
                        ResumeThread(g_HostCpu);
                        ++g_CpuSpeedMissed;
                        if (++spins < CPUSPD_YIELD_BURST) SwitchToThread(); else Sleep(1);
                        continue;
                    }
                }
                /* Caught: suspended inside VdmRunGuest (1) or parked at the re-entry (2).
                   Parked, the exec clock has no open interval, so the same
                   accounting holds -- the servicing time was never guest execution. */
                {   UINT64 windowUs, holdMicroseconds; INT reset;
                    UINT executedDelta;
                    LONG eNow = ExecMicrosecondsNow();
                    QueryPerformanceCounter(&now);
                    executedDelta = (UINT)(ULONG)(eNow - eRun0);   /* run exec, no hold  */
                    /* ⚠ EXEC CAN NEVER EXCEED WALL. A torn read of the exec clock on
                         this thread while the exec thread mutates it can wrap executedDelta to
                         a garbage ~4e9; clamping to the run's own wall (now - resume)
                         kills that at the source, so one bad sample cannot corrupt E or
                         the lifetime exec_ms. Measured: idx 2 reported exec_ms=8.5M in a
                         46 s run before this. */
                    {   UINT runWall = QpcMicroseconds((LONGLONG)(now.QuadPart - wRun0.QuadPart));
                        if (executedDelta > runWall) executedDelta = runWall; }
                    if (!clockSet) { g_StartMs = GetTickCount(); clockSet = 1; }
                    executedUs += (UINT64)executedDelta;
                    windowUs = QpcMicroseconds64(now.QuadPart - wWin.QuadPart);  /* window wall, holds in */
                    holdMicroseconds = CpuSpeedStep(executedUs, windowUs, duty, cAPMicroseconds, &reset);
                    {   UINT64 raw = CpuSpeedHoldFor(executedUs, windowUs, duty);
                        if (raw > g_CpuSpeedDebtMaximumMicroseconds) g_CpuSpeedDebtMaximumMicroseconds = raw > MAXDWORD ? MAXDWORD : (DWORD)raw;
                        if (holdMicroseconds > g_CpuSpeedHoldMaximumMicroseconds) g_CpuSpeedHoldMaximumMicroseconds = (DWORD)holdMicroseconds; }
                    g_CpuSpeedRanMicroseconds   = executedDelta;
                    g_CpuSpeedWallMicroseconds  = (DWORD)windowUs;
                    /* Carry the sub-millisecond remainders: with #225's short runs
                       (~340 us) rounding each one reported half the real exec. */
                    {   static DWORD runRemainder, holdRemainder;
                        runRemainder  += executedDelta;           g_CpuSpeedRunMs  += runRemainder / MICROSECONDS_PER_MILLISECOND_U;  runRemainder  %= MICROSECONDS_PER_MILLISECOND_U;
                        holdRemainder += (DWORD)holdMicroseconds;  g_CpuSpeedHeldMs += holdRemainder / MICROSECONDS_PER_MILLISECOND_U; holdRemainder %= MICROSECONDS_PER_MILLISECOND_U; }
                    ++g_CpuSpeedPeriods;
                    if (caught == CPUSPEED_CATCH_COOPERATIVE) ++g_CpuSpeedCooperativeCatches;
                    {   INT second = CpuSpeedTimelineSeconds();
                        if (second >= 0) { g_ControlExecMicroseconds[second] += executedDelta; g_ControlHoldMicroseconds[second] += (DWORD)holdMicroseconds;
                                       if (caught == CPUSPEED_CATCH_COOPERATIVE) ++g_ControlCooperative[second]; } }
                    if (holdMicroseconds >= MICROSECONDS_PER_MILLISECOND_ULL) Sleep((DWORD)(holdMicroseconds / MICROSECONDS_PER_MILLISECOND_ULL));
                    /* Release: lower the request FIRST, so a parked guest cannot see it
                       still raised and park again, then let it go. */
                    InterlockedExchange(&g_CpuSpeedCatchRequest, 0);
                    if (caught == 1) ResumeThread(g_HostCpu);
                    else if (g_CpuSpeedRelease) SetEvent(g_CpuSpeedRelease);
                    QueryPerformanceCounter(&wRun0);
                    eRun0 = ExecMicrosecondsNow();           /* new run baseline, POST-hold */
                    if (reset) { wWin = wRun0; executedUs = 0ull; }
                    done = 1;
                }
            }
#undef CPUSPD_YIELD_BURST
        }
    }
    return 0;
}
enum { HEARTBEAT_MS = 500 };   /* HeartbeatThread: one line this often */
/* Headless heartbeat (session 11). Both qirq runs stopped logging mid-run and reached
   NO exit path -- not the guest's 4Ch flush, not the deadline backstop's report -- which
   means the process died without user-mode notice. A once-per-500ms beat carrying the
   guest's CS:IP/EFLAGS and the counters turns that silence into a timestamped last known
   position, which is the only way to tell "guest still spinning" from "process killed". */
static DWORD WINAPI HeartbeatThread(LPVOID parameter)
{
    INT beat;
    (VOID)parameter;
    for (beat = 0; beat < 400 && g_Running; ++beat) {   /* 200 s: a live run outlasts 40 s */
        /* ⚠⚠ 640, AND MEASURE THE LINE IN A REAL LOG BEFORE ADDING A FIELD. A fixed log
             buffer in this file is a silent budget: adding one field to a 247-char line
             in a char[256] killed the host once (see DpmiDispatchToPmHandler's lb),
             and this line was 392 bytes in a char[384] from s62's `t_ms=`/`code@csip=`
             until s68 -- an 8-byte stack overwrite on every beat that nothing reported.
             With the `crtc=` fields it measures ~434.
           ⛔ AND AGAIN (s81): the pic{}/pend/ivt0d/aw5{} fields took it past 640 on ZAR and
             the heartbeat thread died writing " 1f7c=" over the top of its own stack
             (a host AV on a worker, mid-run). 2048 now, and aw5{} is capped at 6 codes. */
        CHAR buffer[2048], *cursor = buffer;
        DWORD cs = 0, ip = 0, eflags = 0;
        if (g_TibDebug) {
            cs  = VDM_REG16(g_TibDebug, VTIB_CS);
            ip  = VDM_REG16(g_TibDebug, VTIB_EIP);
            eflags = VDM_REG(g_TibDebug, VTIB_EFLAGS);
            g_HeartbeatDs = VDM_REG16(g_TibDebug, VTIB_DS);   /* s69: for the fade dump */
        }
        cursor = LogPut(cursor, "HB 0x");        cursor = LogHex(cursor, (DWORD)beat);
        cursor = LogPut(cursor, " cs:ip=0x");    cursor = LogHex(cursor, cs); cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, ip);
        cursor = LogPut(cursor, " efl=0x");      cursor = LogHex(cursor, eflags);
        cursor = LogPut(cursor, " state=0x");    cursor = LogHex(cursor, *(volatile DWORD *)(ULONG_PTR)FIXED_NTVDMSTATE_LINEAR);
        cursor = LogPut(cursor, " io=0x");       cursor = LogHex(cursor, g_EventIo);
        cursor = LogPut(cursor, " irq0=0x");     cursor = LogHex(cursor, g_Irq0Injected);
        cursor = LogPut(cursor, " irqn=0x");     cursor = LogHex(cursor, g_IrqNInjected);
        /* ► THE HEARTBEAT IS THE ONLY INSTRUMENT THAT SURVIVES A WEDGE -- STAGE2 never
             prints when the watchdog kills the run, which is exactly the case being
             debugged. `r5` is raises on line 5 (the SB); `rtry` is the async retry
             offer/land pair. */
        cursor = LogPut(cursor, " r5=0x");       cursor = LogHex(cursor, g_IrqRaised[5]);
        cursor = LogPut(cursor, " rtry=0x");     cursor = LogHex(cursor, g_IrqNRetryTry);
        cursor = LogPut(cursor, "/0x");          cursor = LogHex(cursor, g_IrqNRetryOk);
        cursor = LogPut(cursor, " why=0x");      cursor = LogHex(cursor, g_IrqNRetryWhy);
        cursor = LogPut(cursor, " intpend=0x");  cursor = LogHex(cursor, g_EventIntPending);
        /* s81: WHY IS A RAISED LINE NOT DELIVERED? The PIC's own view (mask, request,
           in service, both chips), which lines we still hold pending, and where IRQ 5's
           real-mode vector points -- the three things that decide it. */
        { INT index; DWORD pendingMask = 0; WORD int0DOffset = PeekWord(IVT_OFFSET_ADDRESS(VECTOR_IRQ5)), int0DSegment = PeekWord(IVT_SEGMENT_ADDRESS(VECTOR_IRQ5));
          for (index = 0; index < PIC_LINES; ++index) if (g_IrqNPending[index]) pendingMask |= 1u << index;
          cursor = LogPut(cursor, " pic{imr=");  cursor = LogHexByte(cursor, g_Pic.Master.Imr); cursor = LogPut(cursor, "/"); cursor = LogHexByte(cursor, g_Pic.Slave.Imr);
          cursor = LogPut(cursor, " irr=");      cursor = LogHexByte(cursor, g_Pic.Master.Irr); cursor = LogPut(cursor, "/"); cursor = LogHexByte(cursor, g_Pic.Slave.Irr);
          cursor = LogPut(cursor, " isr=");      cursor = LogHexByte(cursor, g_Pic.Master.Isr); cursor = LogPut(cursor, "/"); cursor = LogHexByte(cursor, g_Pic.Slave.Isr);
          cursor = LogPut(cursor, "} pend=0x");  cursor = LogHex(cursor, pendingMask);
          cursor = LogPut(cursor, " ivt0d=");    cursor = LogHex(cursor, int0DSegment); cursor = LogPut(cursor, ":"); cursor = LogHex(cursor, int0DOffset);
          cursor = LogPut(cursor, " nested=");   cursor = LogHex(cursor, (DWORD)g_NestedRm);
          cursor = LogPut(cursor, " aw5{");
          { INT shown = 0;
            for (index = 0; index < ASYNC_WHY_MAX && shown < 6; ++index) if (g_AsyncWhyHistogram[5][index]) {
                cursor = LogPut(cursor, " "); cursor = LogHex(cursor, (DWORD)index); cursor = LogPut(cursor, "=");
                cursor = LogHex(cursor, g_AsyncWhyHistogram[5][index]); ++shown; } }
          cursor = LogPut(cursor, " }"); }
        /* SB transfer state on the beat. irqn_refused=0 across 4.6M gate evaluations
           proves the completion IRQ was raised only AFTER the exec loop ended, so what
           matters now is WHEN the block starts and how fast it drains -- neither of which
           any counter shows after the fact. */
        cursor = LogPut(cursor, " sb{mode=0x");  cursor = LogHex(cursor, (DWORD)g_Sb.TransferMode);
        cursor = LogPut(cursor, " left=0x");     cursor = LogHex(cursor, g_Sb.BlockRemaining);
        cursor = LogPut(cursor, " len=0x");      cursor = LogHex(cursor, g_Sb.BlockLength);
        cursor = LogPut(cursor, " blocks=0x");   cursor = LogHex(cursor, g_Sb.Blocks);
        cursor = LogPut(cursor, " rate=0x");     cursor = LogHex(cursor, g_Sb.RateHz);
        cursor = LogPut(cursor, "} mixed=0x");   cursor = LogHex(cursor, g_Audio.FramesMixed);
        /* ► THE 3DA VIEW: did the guest's vblank polls reach the VDD, and what was
             it told last? Mario spins >1s on `in al,3DA / test al,8 / jz` before
             the breakpoint kill; 700k reads with no edge is either "the reads
             never reach status_in" or "the model never asserts bit 3", and only
             these two numbers can tell them apart. (session 62) */
        cursor = LogPut(cursor, " p3da=0x");     cursor = LogHex(cursor, g_Video.Port3DaReads);
        cursor = LogPut(cursor, "/edges=0x");    cursor = LogHex(cursor, g_Video.VblEdges);
        cursor = LogPut(cursor, "/last=0x");     cursor = LogHexByte(cursor, g_Video.Retrace);
        /* ► THE LIVE DISPLAY START, so a VRAM watchpoint can be AIMED. The exit
             report prints it once, at exit, when the guest is back in text mode --
             useless for `offset = start + y*stride + x/8` during gameplay. (s68) */
        cursor = LogPut(cursor, " crtc=0x");     cursor = LogHex(cursor, g_Video.CrtcStartLive);
        cursor = LogPut(cursor, "/flips=0x");    cursor = LogHex(cursor, g_Video.CrtcStartWrites);
        cursor = LogPut(cursor, "/ofs=0x");      cursor = LogHexByte(cursor, g_Video.CrtcOffset);
        /* ► AND THE CLOCK THOSE TWO ARE RATES AGAINST. Without it the only time axis
             on this line is irq0, and irq0 is the PIT -- which a guest REPROGRAMS.
             Reading frames-per-second off a beat count that the guest itself can
             change the rate of is how "26 fps" and "3.5 fps" both got computed from
             the same run. This is the same microsecond clock the retrace model and
             the PIT sync derive from, so edges/ms on this line is a measurement and
             not a conversion. */
        cursor = LogPut(cursor, " t_ms=0x");     cursor = LogHex(cursor, (UINT32)(HostTimeMicroseconds() / MICROSECONDS_PER_MILLISECOND_U));
        /* ► THE BYTES AT THE BEAT'S CS:IP. Mario's host dies between beats with
             exit 0x80000003 and NO user-mode dispatch, so the last heartbeat is
             the only witness -- and a bare cs:ip in a moving guest names nothing
             once the process is gone. Eight bytes make it decodable after the
             fact. (session 62) */
        /* ⛔ READ GUEST MEMORY HERE ONLY THROUGH ReadProcessMemory. (s81, Mario) A
             check-then-dereference is a race on this thread: the A0000 window is remapped
             under a mode switch, and a pal2668 read with DS=A000 took an AV in between
             InterpreterMemoryPageOk() and the load -- the diagnostic killed the host it was watching.
             RPM on our own process fails cleanly instead of faulting. */
        if (cs || ip) {
            BYTE codeBytes[8]; SIZE_T got = 0;
            cursor = LogPut(cursor, " code@csip=");
            if (ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(ULONG_PTR)((cs << PARAGRAPH_SHIFT) + ip), codeBytes, 8, &got)
                && got == 8) cursor = LogDump(cursor, codeBytes, 8);
            else cursor = LogPut(cursor, "<unreadable>");
        }
        /* ── s69: THE PALETTE FADE, ON THE BEAT. The blank-screen bug is a black
             per-frame palette buffer (ds:0x2668) while the colours are loaded; it is
             by-hand-only and the by-hand host dies before the exit report, so put the
             live state where the heartbeat can carry it out. `pal2668` = the first 3
             DAC entries the frame routine will push; `1f7c` = the palette state
             selector (oracle=4). All zero here = the fade is stuck black. */
        if (g_HeartbeatDs) {
            UINT32 dataSegmentBase = (UINT32)g_HeartbeatDs << PARAGRAPH_SHIFT;
            cursor = LogPut(cursor, " reload=0x"); cursor = LogHex(cursor, (DWORD)g_Pit.Reload);
            cursor = LogPut(cursor, " dacrow[0-1,2-159,160+,vbl]="); cursor = LogDecimal(cursor, g_Video.DacRowHistogram[0]);
            cursor = LogPut(cursor, "/"); cursor = LogDecimal(cursor, g_Video.DacRowHistogram[1]);
            cursor = LogPut(cursor, "/"); cursor = LogDecimal(cursor, g_Video.DacRowHistogram[2]);
            cursor = LogPut(cursor, "/"); cursor = LogDecimal(cursor, g_Video.DacRowHistogram[3]);
            cursor = LogPut(cursor, " lastrow=0x"); cursor = LogHex(cursor, (DWORD)g_Video.DacLastRow);
            BYTE probeBytes[6], probeByte; SIZE_T got = 0;
            cursor = LogPut(cursor, " pal2668=");
            if (ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(ULONG_PTR)(dataSegmentBase + 0x2668), probeBytes, 6, &got)
                && got == 6) {
                INT byteIndex; for (byteIndex = 0; byteIndex < 6; ++byteIndex) cursor = LogHexByte(cursor, probeBytes[byteIndex]);
            } else cursor = LogPut(cursor, "??");
            cursor = LogPut(cursor, " 1f7c=");
            if (ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(ULONG_PTR)(dataSegmentBase + 0x1f7c), &probeByte, 1, &got)
                && got == 1) cursor = LogHexByte(cursor, probeByte);
            else cursor = LogPut(cursor, "??");
        }
        cursor = LogPut(cursor, "\r\n");
        LogAppend(LOG_PATH, buffer, cursor); SerialOut(buffer, cursor);
        Sleep(HEARTBEAT_MS);
    }
    return 0;
}

/* #238: guest time against host time, the host's share charged per event (g_HostMicrosecondsEvent). */
static VOID ExecShareReport(VOID)
{
    CHAR base[1024], *cursor = base;
    INT event;
    cursor = LogPut(cursor, "STAGE2: exec share (#238) v86_ms="); cursor = LogDecimal(cursor, g_V86MicrosecondsTotal / MICROSECONDS_PER_MILLISECOND_U);
    cursor = LogPut(cursor, " host_ms_after_event[ev:ms/count]=");
    for (event = 0; event < EV_HIST_MAX; ++event) {
        if (!g_HostMicrosecondsEvent[event] && !g_EventHistogram[event]) continue;
        cursor = LogPut(cursor, " "); cursor = LogHex(cursor, (DWORD)event); cursor = LogPut(cursor, ":");
        cursor = LogDecimal(cursor, g_HostMicrosecondsEvent[event] / MICROSECONDS_PER_MILLISECOND_U); cursor = LogPut(cursor, "/"); cursor = LogDecimal(cursor, g_EventHistogram[event]);
    }
    {   /* the six busiest BOP numbers */
        DWORD seen[BYTE_VALUES]; INT index, rank;
        for (index = 0; index < BYTE_VALUES; ++index) seen[index] = g_BopHistogram[index];
        cursor = LogPut(cursor, " bops:");
        for (rank = 0; rank < 6; ++rank) {
            INT best = -1;
            for (index = 0; index < BYTE_VALUES; ++index) if (seen[index] && (best < 0 || seen[index] > seen[best])) best = index;
            if (best < 0) break;
            cursor = LogPut(cursor, " "); cursor = LogHexByte(cursor, (BYTE)best); cursor = LogPut(cursor, "="); cursor = LogDecimal(cursor, seen[best]);
            seen[best] = 0;
        }
    }
    cursor = LogPut(cursor, "\r\nSTAGE2: irq0 coop skip="); cursor = LogDecimal(cursor, g_Irq0Skip);
    cursor = LogPut(cursor, " if="); cursor = LogDecimal(cursor, g_Irq0SkipIf);
    cursor = LogPut(cursor, " stub="); cursor = LogDecimal(cursor, g_Irq0SkipStub);
    cursor = LogPut(cursor, " inj="); cursor = LogDecimal(cursor, g_Irq0Injected);
    cursor = LogPut(cursor, " if-off callers of our stubs:");
    for (event = 0; event < SKIPIF_SITES && g_SkipIfSite[event].Count; ++event) {
        cursor = LogPut(cursor, " "); cursor = LogHex(cursor, g_SkipIfSite[event].Cs); cursor = LogPut(cursor, ":");
        cursor = LogHex(cursor, g_SkipIfSite[event].Ip); cursor = LogPut(cursor, "(stub+"); cursor = LogHex(cursor, g_SkipIfSite[event].Stub);
        cursor = LogPut(cursor, ")x"); cursor = LogDecimal(cursor, g_SkipIfSite[event].Count);
    }
    cursor = LogPut(cursor, "\r\n");
    LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
    {   static PCSTR const names[XS_N] = { "raise","async","coop","nie","bop","io","hostms","pace" };
        INT column; DWORD second;
        for (column = 0; column < XS_N; ++column) {
            cursor = LogPut(cursor, "STAGE2: persec "); cursor = LogPut(cursor, names[column]); cursor = LogPut(cursor, "=");
            for (second = 1; second < g_XsSeconds; ++second) {
                cursor = LogPut(cursor, second > 1 ? "," : "");
                cursor = LogDecimal(cursor, g_XsSnapshot[second][column] - g_XsSnapshot[second - 1][column]);
            }
            cursor = LogPut(cursor, "\r\n");
            LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
        }
    }
    LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor);
}

enum { HEADLESS_EXIT_CODE = 3 };   /* the deadline ended the run */
/* Headless deadline watchdog (session-9). A headless run must self-bound even when the
   guest blocks INSIDE a host INT handler -- e.g. a blocking INT 16h/21h key read at a
   "press any key" prompt or a game menu (HostConsoleIn + the INT 16h loops spin until
   g_Running clears). The per-loop wall-clock caps can't fire then (the exec loop isn't
   iterating), so this thread forces the wind-down: after PM_HEADLESS_MS it clears
   g_Running -- which unblocks every blocking key-read (all check !g_Running) AND exits
   both exec loops -- and wakes any blocked reader. Started only when g_Headless. */
static DWORD WINAPI HeadlessDeadlineThread(LPVOID parameter)
{
    /* ⛔ 2048, NOT 512 (s80). The hot-ports line alone is up to IO_HOT_MAX (48) x ~22
         chars = ~1.1 KB -- IO_HOT_MAX grew from 12 and this buffer did not, so a run with
         many hot ports smashed the stack and the host faulted INSIDE the report meant to
         explain the run (`HOSTFAULT at=0x36303030`, i.e. "0006" as a return address).
         Worst case per flush: ~1.1 KB, then ~0.6 KB (unclaimed + VESA). */
    CHAR buffer[2048], *cursor = buffer;
    DWORD snapshotCs = 0, snapshotIp = 0, snapshotEflags = 0, snapshotTick = 0;
    BYTE  snapshotBytes[12];
    INT   snapshotOk = 0;
    (VOID)parameter;
    Sleep(PM_HEADLESS_MS);
    /* Snapshot the guest NOW, before anything winds down: by the time the grace
       period below expires the VDM address space may already be torn down, and
       reading it then faults this thread and loses the report entirely. */
    if (g_TibDebug) {
        snapshotCs  = VDM_REG16(g_TibDebug, VTIB_CS);
        snapshotIp  = VDM_REG16(g_TibDebug, VTIB_EIP);
        snapshotEflags = VDM_REG(g_TibDebug, VTIB_EFLAGS);
        { const volatile BYTE *code = (const volatile BYTE *)((snapshotCs << PARAGRAPH_SHIFT) + snapshotIp);
          UINT byteIndex; for (byteIndex = 0; byteIndex < 12; ++byteIndex) snapshotBytes[byteIndex] = code[byteIndex]; }
        snapshotTick = ((DWORD)PeekWord(BIOS_BDA_BASE + BIOS_BDA_TICK_COUNT_HIGH) << WORD_SHIFT) | PeekWord(BIOS_BDA_BASE + BIOS_BDA_TICK_COUNT);
        snapshotOk = 1;
    }
    InterlockedExchange(&g_Running, 0);         /* stop exec loops + unblock key reads */
    if (g_KeyEvent) SetEvent(g_KeyEvent);     /* wake a blocked HostConsoleIn/INT16 wait */
    cursor = LogPut(cursor, "HEADLESS: deadline reached -> g_running=0 (wind down)\r\n");
    LogAppend(LOG_PATH, buffer, cursor); SerialOut(buffer, cursor);
    if (g_MemoryDumpLength) {                        /* MEMDUMP_FLAG: the guest is still mapped */
        HANDLE dumpHandle = CreateFileA(MEMDUMP_PATH, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        DWORD done = 0, offset = 0;
        if (dumpHandle != INVALID_HANDLE_VALUE) {
            while (offset < g_MemoryDumpLength) {       /* page at a time, skipping what is not mapped */
                DWORD chunk = X86_PAGE_SIZE, bytesWritten = 0; PCVOID source = (const VOID *)(ULONG_PTR)(g_MemoryDumpLinear + offset);
                if (chunk > g_MemoryDumpLength - offset) chunk = g_MemoryDumpLength - offset;
                if (HostReadable(source, chunk)) { WriteFile(dumpHandle, source, chunk, &bytesWritten, NULL); done += bytesWritten; }
                else { static const BYTE zeros[X86_PAGE_SIZE]; WriteFile(dumpHandle, zeros, chunk, &bytesWritten, NULL); }
                offset += chunk;
            }
            CloseHandle(dumpHandle);
        }
        cursor = buffer; cursor = LogPut(cursor, "HEADLESS: memdump 0x"); cursor = LogHex(cursor, g_MemoryDumpLinear);
        cursor = LogPut(cursor, " +0x"); cursor = LogHex(cursor, g_MemoryDumpLength); cursor = LogPut(cursor, " -> memdump.bin, readable bytes 0x");
        cursor = LogHex(cursor, done); cursor = LogPut(cursor, "\r\n"); LogAppend(LOG_PATH, buffer, cursor); SerialOut(buffer, cursor);
    }

    /* HARD BACKSTOP. Clearing g_Running only stops a loop that gets a turn, and the
       exec loops only get one when the guest faults, BOPs or takes an interrupt. A
       guest spinning in pure V86 code -- Skyroads after its sound init, or any
       `jmp $`-shaped wait -- never returns from VdmRunGuest at all, so neither loop
       reaches its check and the process hangs forever. That wedges the SMB harness's
       `start /wait`, and the box needs a manual `controld kill` to recover, which is
       precisely the loop we cannot afford once every sound test is a real-mode DOS
       program. So: give the clean wind-down a grace period to flush its log and DOS
       output, then take the process down ourselves. rt.bat then collects the log and
       the watcher survives, which is the whole point of headless mode. */
    Sleep(PM_HEADLESS_GRACE_MS);
    if (!g_WoundDown) {
        cursor = buffer;
        AsyncWhyReport();
        ExecShareReport();
        IfvReport();
        cursor = LogPut(cursor, "HEADLESS: exec loop never wound down (guest spinning in V86 with"
                    " no traps) -> forcing process exit\r\n");
        /* Report WHERE it froze and what the counters say. Without this the forced
           exit throws away the only evidence of why the guest stopped trapping,
           which is exactly what we need to disassemble the offending loop. */
        if (snapshotOk) {
            cursor = LogPut(cursor, "  frozen at CS:IP=0x"); cursor = LogHex(cursor, snapshotCs);
            cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, snapshotIp);
            cursor = LogPut(cursor, " EFL=0x"); cursor = LogHex(cursor, snapshotEflags);
            cursor = LogPut(cursor, " tick=0x"); cursor = LogHex(cursor, snapshotTick);
            cursor = LogPut(cursor, "\r\n  bytes: "); cursor = LogDump(cursor, snapshotBytes, 12);
            cursor = LogPut(cursor, "\r\n");
        }
        LogAppend(LOG_PATH, buffer, cursor); SerialOut(buffer, cursor); cursor = buffer;
        cursor = LogPut(cursor, "  io_events=0x");  cursor = LogHex(cursor, g_EventIo);
        cursor = LogPut(cursor, " io_burst=0x");    cursor = LogHex(cursor, g_IoExtra);
        cursor = LogPut(cursor, " iostr=0x");       cursor = LogHex(cursor, g_EventIoString);
        cursor = LogPut(cursor, " irq0_inj=0x");    cursor = LogHex(cursor, g_Irq0Injected);
        cursor = LogPut(cursor, " irq0_skip=0x");   cursor = LogHex(cursor, g_Irq0Skip);
        cursor = LogPut(cursor, " intpend=0x");     cursor = LogHex(cursor, g_EventIntPending);
        cursor = LogPut(cursor, " irqn_inj=0x");    cursor = LogHex(cursor, g_IrqNInjected);
        cursor = LogPut(cursor, " irqn_refused=0x"); cursor = LogHex(cursor, g_IrqNRefuseTotal);
        cursor = LogPut(cursor, " raised_any=0x"); cursor = LogHex(cursor, g_IrqRaisedAny);
        cursor = LogPut(cursor, " async_inj=0x");  cursor = LogHex(cursor, g_AsyncInjected);
        cursor = LogPut(cursor, " async_bail=0x"); cursor = LogHex(cursor, g_AsyncBail);
        { INT irq; cursor = LogPut(cursor, " raised[0..15]=");
          for (irq = 0; irq < 16; ++irq) { cursor = LogPut(cursor, "0x"); cursor = LogHex(cursor, g_IrqRaised[irq]); cursor = LogPut(cursor, " "); } }
        cursor = LogPut(cursor, " sb_irq=0x"); cursor = LogHex(cursor, (DWORD)g_Sb.Irq);
        cursor = LogPut(cursor, " qi_calls=0x");    cursor = LogHex(cursor, g_QiCalls);
        cursor = LogPut(cursor, " qi_st=0x");       cursor = LogHex(cursor, (DWORD)g_QiStatus);
        cursor = LogPut(cursor, " state714=0x");    cursor = LogHex(cursor, *(volatile DWORD *)(ULONG_PTR)FIXED_NTVDMSTATE_LINEAR);
        cursor = LogPut(cursor, "\r\n  audio: silent=0x"); cursor = LogHex(cursor, (DWORD)g_Wave.IsSilent);
        cursor = LogPut(cursor, " mixed=0x");        cursor = LogHex(cursor, g_Audio.FramesMixed);
        cursor = LogPut(cursor, " sb_dspwr=0x");     cursor = LogHex(cursor, g_Sb.DspWrites);
        cursor = LogPut(cursor, " sb_blocks=0x");    cursor = LogHex(cursor, g_Sb.Blocks);
        cursor = LogPut(cursor, " sb_mode=0x");      cursor = LogHex(cursor, (DWORD)g_Sb.TransferMode);
        cursor = LogPut(cursor, " sb_rate=0x");      cursor = LogHex(cursor, g_Sb.RateHz);
        /* Is what it streamed SOUND? A forced exit never prints the sb OUTPUT block, and
           that was how ZAR's runs end (s81). Flat = no dynamic range (SB_FLAT_RANGE). */
        cursor = LogPut(cursor, " sb_checked=0x");   cursor = LogHex(cursor, g_Sb.BlocksChecked);
        cursor = LogPut(cursor, " sb_flat=0x");      cursor = LogHex(cursor, g_Sb.BlocksFlat);
        cursor = LogPut(cursor, " bda_tick=0x");    cursor = LogHex(cursor, ((DWORD)PeekWord(BIOS_BDA_BASE + BIOS_BDA_TICK_COUNT_HIGH) << WORD_SHIFT) | PeekWord(BIOS_BDA_BASE + BIOS_BDA_TICK_COUNT));
        cursor = LogPut(cursor, "\r\n");
        LogAppend(LOG_PATH, buffer, cursor); SerialOut(buffer, cursor); cursor = buffer;
        { INT index; cursor = LogPut(cursor, "  hot ports:");
          for (index = 0; index < g_IoHotCount; ++index) {
              cursor = LogPut(cursor, " 0x"); cursor = LogHex(cursor, g_IoHot[index].Port);
              cursor = LogPut(cursor, "=0x"); cursor = LogHex(cursor, g_IoHot[index].Count);
          }
          cursor = LogPut(cursor, "\r\n  pit_reload=0x"); cursor = LogHex(cursor, (DWORD)g_Pit.Reload);
          cursor = LogPut(cursor, " pit_mode=0x");          cursor = LogHex(cursor, (DWORD)g_Pit.Mode);
          cursor = LogPut(cursor, "\r\n");
          LogAppend(LOG_PATH, buffer, cursor); SerialOut(buffer, cursor); cursor = buffer; }
        { INT index; cursor = LogPut(cursor, "  unclaimed ports touched:");
          for (index = 0; index < g_UnclaimedCount; ++index) { cursor = LogPut(cursor, " 0x"); cursor = LogHex(cursor, g_Unclaimed[index]); }
          cursor = LogPut(cursor, "\r\n"); }
        /* The VESA inventory too: a guest that spins at wind-down (Heretic, Hexen,
           ZAR) never reaches the STAGE2 summary, and the corpus sweep read "no line"
           as "no data" for exactly the three guests it most wanted. (s74b) */
        { INT index, any = 0; cursor = LogPut(cursor, "  VESA calls by sub-function:");
          for (index = 0; index < 0x16; ++index) if (g_Video.VesaCalls[index]) {
              any = 1; cursor = LogPut(cursor, " 4F"); cursor = LogHexByte(cursor, (UINT)index); cursor = LogPut(cursor, "x"); cursor = LogHex(cursor, g_Video.VesaCalls[index]); }
          if (!any) cursor = LogPut(cursor, " none");
          if (g_Video.VbePmBankCount | g_Video.VbePmStartCount | g_Video.VbePmRejected) {   /* #53 */
              cursor = LogPut(cursor, " | 4F0A-block banks=0x"); cursor = LogHex(cursor, g_Video.VbePmBankCount);
              cursor = LogPut(cursor, " starts=0x"); cursor = LogHex(cursor, g_Video.VbePmStartCount);
              cursor = LogPut(cursor, " refused=0x"); cursor = LogHex(cursor, g_Video.VbePmRejected); }
          cursor = LogPut(cursor, "\r\n"); }
        LogAppend(LOG_PATH, buffer, cursor); SerialOut(buffer, cursor);
        /* ── ⛔ AND THE REGISTER FILE HERE TOO, NOT ONLY ON THE CLEAN PATH. ──────────
             The STAGE2 summary is printed by the wind-down path, and a guest that has
             to be forced out never reaches it -- Doom headless and ZAR both leave this
             way. An instrument that only reports on the tidy exit cannot see the
             guests it exists for; that is the "an absence in the report means nothing"
             trap, and it has already cost this project a session on ZAR. */
        ModeYTimelineReport();   /* north star 1: the guests that need it leave this way */
        GusReport();        /* north star 2: and so does heaven7 */
        {   static CHAR registersDump[2048];
            INT length = VddVideoRegistersDump(&g_Video, registersDump, (INT)sizeof registersDump);
            if (length > 0) { LogAppend(LOG_PATH, registersDump, registersDump + length); SerialOut(registersDump, registersDump + length); } }
        HostRecordFinish();
        ExitProcess(HEADLESS_EXIT_CODE);
    }
    return 0;
}

enum { EXEC_PRIORITY_UNSET = 0x7FFF, BACKGROUND_PRIORITY_TICK_MS = 500 };   /* BackgroundPriorityTick */
static INT    g_ExecPriorityNow = EXEC_PRIORITY_UNSET;       /* what BackgroundPriorityTick last set             */
/* See OtherHostsRunning. From the UI timer, at most twice a second.
   ► PRIORITY ALONE WAS MEASURED NOT TO BE ENOUGH (s81 rig, interleaved A/B): with QBasic
     idling in a background host at BELOW_NORMAL, the foreground Skyroads went from
     n8=0 max_ms=6 (alone, twice) to n8=0xb2/0xab with one 2.7 s stall -- and holding
     the background guest to 10% speed through the CPU-speed duty cycle changed nothing
     (n8=0xa3/0xa4/0xad). The stretches were 8-32 ms, i.e. scheduler quanta: the
     background host's OTHER threads share the foreground guest's core at its level.
     Dropping the whole background process to the idle class took n8 to 0xf/0x16/0xb,
     max_ms 0x11/0xe/0x15. The idle class costs a background host nothing when the CPU
     is free (it runs at full speed); it only yields. A throttle would slow two DOS
     windows to a crawl the moment the user clicked on anything else. */
static VOID BackgroundPriorityTick(HWND window)
{
    static DWORD last; static INT logged;
    DWORD now = GetTickCount();
    HWND foreground; INT want, isBackground;
    if (!g_ExecThread || now - last < BACKGROUND_PRIORITY_TICK_MS) return;
    last = now;
    foreground = GetForegroundWindow();
    isBackground = !(foreground && (foreground == window || GetAncestor(foreground, GA_ROOTOWNER) == window)) && OtherHostsRunning();
    want = isBackground ? THREAD_PRIORITY_BELOW_NORMAL : g_ExecPriorityForeground;
    if (want == g_ExecPriorityNow) return;
    if (!SetThreadPriority(g_ExecThread, want)) return;
    g_ExecPriorityNow = want;
    /* ...and the whole PROCESS, because its other threads (courier and watchdog at
       HIGHEST, the throttle at ABOVE_NORMAL) otherwise share the foreground guest's
       core at its own level. In the idle class all of them sit below it; only the
       audio pump (TIME_CRITICAL, 15 in any class) keeps its place. */
    SetPriorityClass(GetCurrentProcess(), isBackground ? IDLE_PRIORITY_CLASS : NORMAL_PRIORITY_CLASS);
    if (logged++ < 32) {
        CHAR buffer[128], *cursor = LogPut(buffer, "PRIO: guest thread -> ");
        cursor = LogPut(cursor, isBackground ? "idle class (background, another host running)"
                       : "its own priority");
        cursor = LogPut(cursor, " (#211)\r\n");
        LogAppend(LOG_PATH, buffer, cursor);
    }
}

/* Perform ONE decoded port access on the bus and write an IN result back into
   the guest's EAX at the right width. Factored out of the three I/O servicers
   (V86 / retro / PM) so the burst fast path below can repeat an access without
   re-decoding it. */
/* THE GUEST'S CLOCK. Skyroads -- like a lot of DOS games -- does not ask the BIOS what time
   it is; it latches PIT counter 0 and reads it, over and over (`out 43h,al; in al,40h; in
   al,40h`, its hot loop at 0110:5a85). So the guest's ENTIRE sense of elapsed time is
   whatever that counter says. Until now the counter was advanced by the UI thread's frame
   loop, once per presented frame -- and that thread is starved precisely when the guest is
   hammering I/O, which is exactly when the game is asking. The guest therefore saw time
   crawl, and everything it paces on time crawled with it: the palette fade, the music tempo
   (pitch was right -- that is the OPL, which is correct -- only the sequencer was slow), and
   the rate it fed PCM (slow AND pitched down). None of that was a sound bug.

   A real 8254 is a free-running counter, so model it as one: derive elapsed clocks from a
   high-resolution host clock at the moment the guest looks. QueryPerformanceCounter is
   KERNEL32, so it stays inside the no-CRT import rules. */
/* ★ A MEASUREMENT THRESHOLD, NOT A LIMIT. NOTHING IS CLAMPED TO IT. READ THIS
   BEFORE "fixing" the catch-up burst, because I already tried and it was worse.
   ► THE SYMPTOM (user, 2026-08-21): the music "speeds up for a few milliseconds and
     then returns to normal", dropping a few notes. That is a real CATCH-UP BURST:
     Skyroads runs its timer at about 16x the BIOS 18.2 Hz and advances its
     sequencer one step per tick, so a host stall hands it dozens of ticks at once.
     The tempo lurches, and a note whose on AND off both land inside the burst never
     sounds at all.
   ► WHAT I DID, AND WHY IT WAS A REGRESSION. I capped delivery at 10 ms and
     DISCARDED the excess. The user's verdict was immediate: "speed is all over the
     place now -- slow, normal, fast, normal, fast, slow". The reasoning behind the
     cap contained an assumption I never measured: that syncs are always much closer
     together than 10 ms because the UI tick is 5 ms. They are not. HostPitSync
     takes g_Lock, and a heavy I/O-trap loop starves the UI thread (that is what the
     comment above the exec-loop call at the bottom of this file is about) -- so gaps
     past 10 ms are ORDINARY, and the cap threw away real time on every one of them.
   ► THE LESSON, which is this project's own cardinal rule wearing a new hat: the old
     burst was at least CORRECT ON AVERAGE. Discarding time is not a smaller version
     of that error, it is a bigger and more constant one. Do not trade an occasional
     artefact for a permanent one.
   ► IF YOU PICK THIS UP: never discard. Keep the total exact and SMOOTH the
     delivery -- carry the backlog and release it at a bounded rate over the next few
     syncs, so the average tempo is preserved and only the lurch is removed. And
     measure the real gap distribution FIRST: that is what g_PitGapMaximum and the
     counter below exist for. A second, independent cause is also in play -- with no
     CPU affinity set anywhere, a TSC-backed QueryPerformanceCounter can JUMP FORWARD
     when a thread migrates cores, which is indistinguishable from a stall in here. */
#define PIT_CATCHUP_MAX (PIT_INPUT_HZ / 100u)      /* 10 ms: the reporting threshold */
static UINT32 g_PitCatchupClamped;             /* gaps past it (STAGE2)          */
static UINT32 g_PitGapMaximum;                     /* the worst one, in 8254 clocks  */

/* Wired onto g_Pit at startup; see g_PitCs and PIT_STATE.guard. */
/* ── THE MACHINE'S CLOCK, for INT 1Ah AH=02h/04h. ────────────────────────────────
     vdd_pit.c is a portable VDD and stays free of <time.h> (which the XP-targeting
     CRT does not link in any case), so the host hands it the reading. Local time,
     not UTC: a DOS guest's clock is the wall clock on the machine in front of you,
     and that is what `DATE` and `TIME` and every file timestamp are compared against. */
/* ► GH #250: THE VDM'S RTC, NOT THE HOST'S. Host-now moved by g_DosClock.RtcOffset,
     which a guest's INT 1Ah AH=03h/05h or INT 21h AH=2Bh/2Dh sets (dos_clock.h). Zero
     until a guest sets it, so an untouched VDM reads exactly GetLocalTime as before. */
static VOID HostRtcNow(PVOID context, PIT_RTC_READING *out)
{
    DOS_CLOCK_TIME clock;
    (VOID)context;
    DosClockRead(g_DosClock.RtcOffset, &clock);
    out->Century  = clock.Year / YEARS_PER_CENTURY_U;
    out->Year  = clock.Year % YEARS_PER_CENTURY_U;
    out->Month = clock.Month;
    out->Day   = clock.Day;
    out->Hour  = clock.Hour;
    out->Minute   = clock.Minute;
    out->Second   = clock.Second;
    out->DayOfWeek   = clock.DayOfWeek + 1u;                     /* DOS 0=Sunday; the chip 1=Sunday */
}

/* INT 1Ah AH=03h (what=0: hour/min/sec) and AH=05h (what=1: century/year/month/day),
   already decoded from BCD by the PIT. Moves only the RTC's offset: DOS keeps its own
   clock, as on an AT (p_clock clk.2c.after.1a03 / clk.2a.after.1a05). Returns 0 --
   the clock untouched -- for a reading no calendar has. */
static INT HostRtcSet(PVOID context, const PIT_RTC_READING *reading, INT what)
{
    DOS_CLOCK_TIME host;
    (VOID)context;
    DosClockHostNow(&host);
    if (what == 0) {
        if (!DosClockIsTimeValid(reading->Hour, reading->Minute, reading->Second, 0)) return 0;
        DosClockSetTime(&host, &g_DosClock.RtcOffset, reading->Hour, reading->Minute, reading->Second, 0);
    } else {
        UINT year = reading->Century * YEARS_PER_CENTURY_U + reading->Year;
        if (!DosClockIsRealDate(year, reading->Month, reading->Day)) return 0;
        DosClockSetDate(&host, &g_DosClock.RtcOffset, year, reading->Month, reading->Day);
    }
    return 1;
}

/* GH #262 case B: has 0040:006C been set by anything but the BIOS since DOS last
   looked? The PIT keeps the witness (vdd_pit.h); this is DOS's door onto it, under the
   crystal's lock like every other host touch of the count. */
static INT HostTickTake(UINT32 *ticks, UINT32 *wraps, UINT32 *since)
{
    INT result;
    /* ⚠ THE COMMON ANSWER IS "NOTHING", AND IT MUST NOT QUEUE BEHIND THE PACER. Some
         programs time themselves with AH=2Ch in a tight loop; taking the crystal's lock
         on every call would put each of them behind VddPitAddClocks. An unlocked look
         first: equal and not foreign = nothing to do (a tick racing this read leaves
         them equal again, or sends us to the locked re-check below, which decides). */
    if (!g_Pit.IsTickForeign && *(volatile DWORD *)(ULONG_PTR)(BIOS_BDA_BASE + BIOS_BDA_TICK_COUNT) == g_Pit.TickWitness)
        return 0;
    EnterCriticalSection(&g_PitCs);
    result = VddPitTickTake(&g_Pit, *(volatile DWORD *)(ULONG_PTR)(BIOS_BDA_BASE + BIOS_BDA_TICK_COUNT), ticks, wraps, since);
    LeaveCriticalSection(&g_PitCs);
    return result;
}

/* GH #262: INT 1Ah AH=01h moved the tick count; DOS's clock follows it, as CLOCK$
   would read it. DOS's DATE is kept -- plus any midnights a raw store before this one
   had already carried it through; the RTC is not touched -- on an AT the two clocks
   are separate (p_clock / p_tick2c). The take also makes the new count the BIOS's own,
   so the next tick does not mistake it for a guest's store. */
static VOID HostTicksSet(PVOID context, UINT32 ticks)
{
    UINT32 takenTicks = ticks, wraps = 0, since = 0;
    (VOID)context;
    if (!HostTickTake(&takenTicks, &wraps, &since)) { takenTicks = ticks; wraps = 0; since = 0; }
    DosClockFollow(takenTicks, wraps, since);
}

/* INT 21h AH=2Dh's tick reload (GH #250): what DOS's CLOCK$ does through INT 1Ah
   AH=01h -- the new count, midnight flag cleared. Under the crystal's lock so it
   cannot interleave with anything that holds it while touching the count. */
static VOID HostSetTicks(PVOID context, UINT32 ticks)
{
    (VOID)context;
    EnterCriticalSection(&g_PitCs);
    *(volatile DWORD *)(ULONG_PTR)(BIOS_BDA_BASE + BIOS_BDA_TICK_COUNT) = ticks;
    *(volatile BYTE *)(ULONG_PTR)(BIOS_BDA_BASE + BIOS_BDA_MIDNIGHT_FLAG) = 0;
    VddPitTickOwned(&g_Pit, ticks);          /* DOS's own reload, not a store (#262) */
    LeaveCriticalSection(&g_PitCs);
}

static VOID HostPitGuard(PVOID context, INT enter)
{
    (VOID)context;
    if (enter) EnterCriticalSection(&g_PitCs);
    else       LeaveCriticalSection(&g_PitCs);
}

/* ── ★★★ THE CRYSTAL. (s61) Advance the 8254 to NOW and latch what fell due. ────
     g_PitCs only, held for microseconds, and NOTHING slow inside: this is the part
     of the machine that must behave like silicon. It used to run under g_Lock, and
     the measured price -- from a session the user PLAYED -- was that 86%% of all
     timing stalls were ticks never generated on time because the renderer, the
     mixer or a port-trap storm held the lock (STAGE2: IRQ0WHY gen=443 del=71).
     Delivery is a separate concern with separate rules: HostPitDeliver below. */
static VOID HostPitGenerate(VOID)
{
    static LARGE_INTEGER frequency, last;
    LARGE_INTEGER now;
    ULONGLONG delta;
    /* Called from the exec thread (so a guest polling the counter reads real time),
       the UI thread, and the PACER (so the clock keeps running while the guest spins
       and never traps). One shared clock, hence one lock -- but the CRYSTAL's lock,
       not g_Lock: this function must be able to run while the renderer holds the
       device lock for 20 ms, or the clock stops with it. s61, measured. */
    EnterCriticalSection(&g_PitCs);
    if (!frequency.QuadPart) {
        if (QueryPerformanceFrequency(&frequency) && frequency.QuadPart)
            QueryPerformanceCounter(&last);
        LeaveCriticalSection(&g_PitCs);
        return;
    }
    if (!QueryPerformanceCounter(&now) || now.QuadPart <= last.QuadPart) {
        LeaveCriticalSection(&g_PitCs);
        return;
    }
    /* #219: PAUSED TIME IS NOT GUEST TIME. The crystal does not run while the window is
       paused, and the time is dropped rather than owed -- resuming must not replay the
       pause as a burst of ticks (the clock would jump, and s22 measured what compressing
       game time does). The 8254 and the RTC's periodic interrupt both ride this delta. */
    if (g_PauseWant) {
        last = now;
        LeaveCriticalSection(&g_PitCs);
        return;
    }
    g_AsyncTriedThisSync = 0;        /* a fresh burst of raises gets one attempt */
    ++g_PitSyncs;
    /* Before this sync's raises are added: how far behind had delivery fallen? */
    PmOwedSample(g_PmTickOwed);
    delta = (ULONGLONG)(now.QuadPart - last.QuadPart);
    /* clocks = delta * 1193182 / freq, without overflowing: delta is small (microseconds). */
    { ULONGLONG clocks = (delta * PIT_INPUT_HZ) / (ULONGLONG)frequency.QuadPart;
      if (clocks) {
          /* Consume the WHOLE delta from the clock, then deliver only a bounded part of
             it: past the cap, time is DISCARDED rather than queued up and replayed. */
          last.QuadPart += (LONGLONG)((clocks * (ULONGLONG)frequency.QuadPart) / PIT_INPUT_HZ);
          /* OBSERVE ONLY -- do not act on this. See PIT_CATCHUP_MAX. */
          if (clocks > PIT_CATCHUP_MAX) {
              g_PitCatchupClamped++;
              if (clocks > g_PitGapMaximum) g_PitGapMaximum = (UINT32)clocks;
          }
          if (clocks > PIT_INPUT_HZ) clocks = PIT_INPUT_HZ;   /* cap a long stall at 1 s */
          VddPitAddClocks(&g_Pit, (UINT32)clocks);
          /* ── THE RTC'S PERIODIC INTERRUPT RIDES THE SAME DELTA. ─────────────
               IRQ8 at the rate in CMOS Status A -- a steady tick INDEPENDENT of
               the 8254, which is why Windows and DOS extenders use it: a guest
               that has reprogrammed the PIT has not touched this one. Driven
               here rather than from a second thread so there is one pacer, one
               lock and ONE OPINION ABOUT HOW MUCH TIME HAS PASSED; s61 measured
               what a second clock does to this project.
             ⚠ DORMANT UNLESS THE GUEST ASKED. add_clocks returns immediately
               unless PIE is set in Status B with a non-zero rate select, and
               even then nothing reaches the guest until IRQ8 is unmasked on the
               slave PIC and IRQ2 on the master. All of that is off at reset, so
               a guest that does not program it sees no change at all. */
          VddCmosAddClocks(&g_Cmos, (UINT32)clocks);
      } }
    LeaveCriticalSection(&g_PitCs);
}

/* ── ★★★ DELIVERY: one bounded pass of attempts, and only if g_Lock is FREE. ────
     The old shape held g_Lock across generation AND attempts, which was load-bearing
     in one way (holding g_Lock guarantees the thread we suspend is not holding it)
     and disastrous in another (the clock queued behind the renderer to tick). The
     split keeps the guarantee -- attempts still happen ONLY under g_Lock -- and
     deletes the queueing: HOST_LOCK_TRY means a busy lock skips the attempt, and the
     tick is already LATCHED, so the cooperative path delivers it at the guest's next
     trap (~68,000/s on Skyroads). A skip costs latency bounded by the next trap or
     the next pacer round; the old cost was unbounded clock stall. The s61 suspend
     handshake (post-suspend g_InExec re-check, ctx-writer interlock) additionally
     protects the suspend itself; both layers stay. */
static VOID HostPitDeliver(VOID)
{
    if (!HOST_LOCK_TRY()) { ++g_PitDeliverSkipped; return; }
    if (g_Irq0Pending > 0 && g_QiSuspended && (!g_DpmiPm || !g_AsyncTriedThisSync)) {
            g_AsyncTriedThisSync = 1;
            /* A PENDING KEY IS A STATE, NOT A MOMENT. IRQ1 gets ONE async attempt, at the
               raise; if the guest had interrupts off (96% of gameplay) it falls to the
               exec loop, which can only place it on a pass where they are back on. This is
               the retry, and it is free: AsyncInjectIrq VERIFIES IF/VIF before it
               injects, so an IRQ0 opportunity IS a proven enabled moment, ~184/s. The key
               takes it and the tick waits -- g_Irq0Pending is not decremented, so the
               tick is not lost, just delivered on the next raise. Bounded so a key that
               can never be placed cannot stop the clock. */
            /* ── ★★★ keyirq = 2: YIELD ONLY WHILE THE CLOCK IS ON SCHEDULE. ──────────
                 USER-CONFIRMED SYMPTOM (2026-09-09, in-game, at the box): "butter
                 smooth, until you start pressing keys, then it lags. When you release
                 the keys, it eventually goes smooth again." That is this branch, felt
                 rather than measured -- and it matches the measurement exactly, where
                 `raises - attempts == yields` in every run taken.
               ► The yield itself is NOT the mistake: turning it off (keyirq = 0) is
                 measured to put 51 of 102 keystrokes past 64 ms, worst 1864 ms. Keys
                 and the timer are competing for one scarce thing -- a moment when the
                 guest is in exec with interrupts on -- and somebody has to lose.
               ⇒ So lose the slot only when losing it is FREE. g_Irq0Pending is the
                 saturating tick latch and Irq0Latch() has already counted this raise,
                 so <= 1 means "nothing is owed but the tick we just made": the clock is
                 on schedule and can afford to wait one period. Above that the timer is
                 already behind, which is exactly when a further 5.56 ms of delay is
                 what the player feels, so the key waits instead -- for ONE period, not
                 the three KEYIRQ_MAX_YIELD would allow.
                 0 = never yield (measured: loses keys), 1 = always (the old default,
                 what the user felt), 2 = only when not behind. */
            /* ── ⚠⚠ keyirq = 2 IS REFUTED, USER-CONFIRMED. DO NOT RETRY IT. ─────────
                 "Now it reads too many keys in a row and doesn't stop reading them
                 when the key is released. I flew straight off the road. Twice."
                 Making the KEY wait strands its BREAK code: g_Irq1Pending is one
                 deep, so a make code still queued when the release arrives coalesces
                 the release away and the guest never sees the key come up. This file
                 already recorded that shape once ("the earlier cap-with-a-backlog is
                 what stranded break codes and killed the arrow keys") and I walked
                 into it anyway. ⇒ ANY FIX THAT DELAYS THE KEY IS OFF THE TABLE.
               ► keyirq = 3 is the survivor of that: never delay a key, but allow only
                 ONE yield in a row instead of KEYIRQ_MAX_YIELD's three. The key is
                 served instantly exactly as in mode 1, and the clock's worst-case
                 loss falls from three periods (16.7 ms at 180 Hz) to one. It cannot
                 strand a break code, because a key is never made to wait. */
            {   INT maximumYields = (g_KeyIrqRetry == KEYIRQ_RETRY_ONE_YIELD) ? 1 : KEYIRQ_MAX_YIELD;
            if (g_KeyIrqRetry && !g_DpmiPm && g_Irq1Pending > 0
                && (g_KeyIrqRetry != KEYIRQ_RETRY_CLOCK_ON_SCHEDULE || g_Irq0Pending <= 1)
                && g_Irq0Yielded < maximumYields
                && VddPicCanDeliver(&g_Pic, PIC_IRQ_KEYBOARD) && AsyncInjectIrq(PIC_IRQ_KEYBOARD)) {
                InterlockedDecrement(&g_Irq1Pending);
                ++g_Irq0Yielded;
                ++g_Irq1AsyncRetry;
                ++g_Irq0YieldCount;   /* A/B/C discriminator: the key took the clock's turn */
            } else {
                g_Irq0Yielded = 0;
                ++g_PitAsyncAttempts;
                ++g_Irq0AttemptsCount;     /* A/B/C discriminator: an attempt was actually made */
                if (AsyncInjectIrq(PIC_IRQ_TIMER)) {
                    InterlockedDecrement(&g_Irq0Pending);
                    PmTickTake();
                    /* ⚠ BOTH DELIVERY PATHS, OR THE TIMELINE IS A LIE. A PM guest
                       is fed almost entirely from here and a V86 one from the
                       cooperative site; counting one would show a clock stopping
                       exactly where the other took over. */
                    g_Irq0NoteCs = IRQ0_NOTE_ASYNC_CS; g_Irq0NoteIp = 0;  /* delivered async */
                    Irq0DeliveredNote();
                }
            } }
        }
    /* ── ★★★★ A DEVICE IRQ GETS EXACTLY ONE ASYNCHRONOUS ATTEMPT, AT THE INSTANT IT IS
         RAISED -- AND THAT IS NOT ENOUGH FOR A ONE-SHOT INTERRUPT. ────────────────────
         HostIrqSink tries once when the device raises, and if the CPU thread happens
         to be in HOST code at that microsecond the attempt bails (why=0x14) and nothing
         ever offers it again. For a line that re-raises (Doom's auto-init SB fires 86
         times a second) that is invisible. For a ONE-SHOT it is fatal.
       ★ MEASURED ON ZAR (#23): its Miles driver's init-time DMA/IRQ self-test is an
         8-bit SINGLE-CYCLE transfer, so the SB raises IRQ 5 exactly ONCE. The block
         drains, the IRQ is raised, `ASYNC-EARLY bail irq=05 why=0x14`, and the driver
         then waits for ever on a MEMORY flag its ISR would have set -- so it never traps
         again and no cooperative path can reach it either (measured: zero IRQN-REFUSE).
       ► The latch already persists (g_IrqNPending, cleared only on delivery), so the
         missing piece is merely to OFFER IT AGAIN. Here, because this already runs on
         the pacer thread and already holds g_Lock -- and holding the lock is precisely
         what guarantees the thread we are about to suspend is not holding it (see the
         long note in HostIrqSink; do not move this outside).
       ⚠⚠ ONE ATTEMPT PER SYNC, AND ONLY WHILE SOMETHING IS ACTUALLY PENDING. Session
         22's disaster was ~800 unbounded SuspendThread round trips per sync under this
         lock, and the throttle written to stop it later cost SKYROADS a fifth of its
         clock. So: at most one retry per sync, the first pending hooked line only, and
         nothing at all in the overwhelmingly common case where no device IRQ is
         outstanding -- which is a single predictable branch, not a syscall. */
    { INT index, irq;
      for (index = 0; index < (INT)sizeof g_IrqOrder; ++index) {
          irq = g_IrqOrder[index];
          if (!g_IrqNPending[irq]) continue;
          if (!VddPicCanDeliver(&g_Pic, (BYTE)irq)) break;
          ++g_IrqNRetryTry;
          if (g_QiSuspended && AsyncInjectIrq((UINT)irq)) {
              InterlockedExchange(&g_IrqNPending[irq], 0);
              ++g_IrqNRetryOk;
          }
          /* WHICH CLAUSE SAID NO. 1597 refusals with no reason is not a measurement;
             AsyncInjectIrq already records one, so keep the last of them. */
          else g_IrqNRetryWhy = g_AsyncWhy;
          break;                  /* one per sync, pending or not: see the note above */
      } }
    HOST_UNLOCK();
}

/* One clock, two concerns: callers that used to call HostPitSync still can. */
VOID HostPitSync(VOID)
{
    HostPitGenerate();
    HostPitDeliver();
}

/* ── A COUNTER-0 LATCH ENDS A MEASUREMENT: SHOW WHAT WAS MEASURED. ──────────────
     A guest that latches the 8254 has just timed something, and what Lemmings times
     is 320 scanlines on 0x3DA bit 0 (s69). Those ~1000 polls are invisible among 85
     million in any histogram, so dump the tail of the 0x3DA poll ring here -- model
     microseconds relative to the first entry, and the byte returned -- for the first
     two counter-0 latches only. Read it as: gaps over ~32us straddle a whole line,
     and a `..:00` followed by `..:01` in a later line is a synthesised owed blank.
     `cmd` is the byte written to 0x43; a counter-0 latch is SC=00, RW=00. */
VOID PitLatchNote(BYTE command)
{
    UINT32 total, count, first, base, index, column = 0;
    CHAR buffer[1200], *cursor = buffer;
    /* ⚠ The ring is opt-in (cfg\pitlatch.flag) and so is this: the dump is ~43
         LogAppend calls issued from V86HostOut, i.e. FILE I/O UNDER g_Lock from inside
         the planar interpreter. Fine for a debugging run, not for a build a person
         plays on -- shipping it enabled in s69 was a mistake, see vdd_video.h. */
    if (!g_Video.IsPort3DaRingOn) return;
    if ((command & PIT_CONTROL_SELECT_ACCESS_MASK) != PIT_CONTROL_LATCH_COUNTER0 || g_PitLatchDumps >= 2 || !g_Video.Port3DaRingCount) return;
    total = g_Video.Port3DaRingCount; count = total < VIDEO_PORT_3DA_RING ? total : VIDEO_PORT_3DA_RING;
    first = total - count; base = g_Video.Port3DaRingUs[first & (VIDEO_PORT_3DA_RING - 1)];
    g_PitLatchDumps++;
    cursor = LogPut(cursor, "PIT-LATCH #"); cursor = LogDecimal(cursor, g_PitLatchDumps);
    cursor = LogPut(cursor, " reload=0x"); cursor = LogHex(cursor, (DWORD)g_Pit.Reload);
    cursor = LogPut(cursor, " mode="); cursor = LogDecimal(cursor, g_Pit.Mode);
    cursor = LogPut(cursor, " clocks_since_load="); cursor = LogDecimal(cursor, (UINT32)(g_Pit.TotalClocks - g_Pit.LoadClocks));
    cursor = LogPut(cursor, " hbl_owed="); cursor = LogDecimal(cursor, g_Video.Port3DaHblOwed);
    cursor = LogPut(cursor, " last "); cursor = LogDecimal(cursor, count); cursor = LogPut(cursor, " 0x3DA polls (us:val):\r\n");
    for (index = first; index < total; ++index) {
        UINT32 slot = index & (VIDEO_PORT_3DA_RING - 1);
        cursor = LogDecimal(cursor, g_Video.Port3DaRingUs[slot] - base); cursor = LogPut(cursor, ":");
        cursor = LogHexByte(cursor, g_Video.Port3DaRingValue[slot]); cursor = LogPut(cursor, " ");
        if (++column == 24 || index + 1 == total) {
            cursor = LogPut(cursor, "\r\n"); LogAppend(LOG_PATH, buffer, cursor); cursor = buffer; column = 0;
        }
    }
}
#define RTIDLE_OFF_FLAG CFG_("rtidle.off")
enum { INT10_WAIT_MAX_MS = 50 };   /* Int10WaitAfter: never hold a video call longer */
#define RETRACE_IDLE_MIN_US 1500u   /* shorter waits are not worth idling for */
/* Decode + service a V86 IN/OUT that #GP-faulted (event 2), dispatch it to the
   bus, and advance EIP past the instruction so the guest resumes. Returns 1 if
   the faulting instruction was a (supported) I/O op we handled, 0 if it was a
   genuine GP fault the caller should stop on. No per-call logging -- I/O traps
   are hot (a palette set is ~768 OUTs); flushing the trace file each one stalls. */
/* ── #183: A RETRACE WAIT SLEEPS INSTEAD OF SPINNING. The standing decision: make the
     waits cheaper. A guest waiting for vertical retrace spins `in al,dx / test al,8 /
     jz back` against 3DAh, trapping every iteration -- 3M traps a second, a whole core,
     and (#172) the timer's pacer starved of CPU on a small machine. The handler notes
     the read here (under the lock); after the lock is dropped RetraceIdle() decodes the
     guest's OWN next instructions, and if they are exactly that loop and the edge it is
     waiting for is more than 1.5 ms away, sleeps ONE millisecond. The guest then loops
     once and traps again: interrupts are still delivered between iterations (Lemmings'
     timer ISR spins here with IF on; Skyroads ticks at 180 Hz), and once the edge is
     close it spins as before, so the edge is caught exactly. V86 only; cfg\rtidle.off
     turns it off. */
/* ── #226: VBE 4F07h BL=80h/82h "set display start DURING VERTICAL RETRACE". The
     video device answers the call and records when it may complete; the host waits
     that out here, after dropping the lock -- sleeping while the retrace is more than
     1.5 ms away, spinning for the last stretch -- so a guest that flips pages with it
     gets at most one flip per frame, as on a real card. Bounded at 50 ms. */
static DWORD g_VbeWaits;
static VOID Int10WaitAfter(VOID)
{
    DWORD start = GetTickCount();
    UINT32 microseconds;
    INT waited = 0;
    while ((microseconds = VddVideoInt10WaitUs(&g_Video)) != 0) {
        waited = 1;
        if (GetTickCount() - start > INT10_WAIT_MAX_MS) break;
        if (microseconds > RETRACE_IDLE_MIN_US) Sleep(1); else Sleep(0);
    }
    if (waited) ++g_VbeWaits;
}
enum { RETRACE_IDLE_PATTERN_LENGTH = 4, RETRACE_IDLE_BACK_MAX = 3 };   /* RetraceIdle: test/and al ; jcc back to the IN */
static volatile DWORD g_RetracePending, g_RetraceCs, g_RetraceIp, g_RetraceAl, g_RetraceCx, g_RetraceIdles;
static INT g_RetraceOffset = -1;
VOID RetraceNote(volatile BYTE *tib, WORD port, INT isIn, DWORD cs, DWORD ipAfter)
{
    if (!isIn || port != VIDEO_PORT_STATUS1_COLOUR) return;
    {   INT slot;
        for (slot = 0; slot < RT_SITES; ++slot) {
            if (g_RetraceSite[slot].Count && g_RetraceSite[slot].Cs == cs && g_RetraceSite[slot].Ip == ipAfter) { g_RetraceSite[slot].Count++; break; }
            if (!g_RetraceSite[slot].Count) {
                const volatile BYTE *code = (const volatile BYTE *)(ULONG_PTR)((cs << PARAGRAPH_SHIFT) + ipAfter);
                INT index;
                g_RetraceSite[slot].Cs = cs; g_RetraceSite[slot].Ip = ipAfter; g_RetraceSite[slot].Count = 1;
                for (index = 0; index < 10; ++index) g_RetraceSite[slot].Bytes[index] = code[index];
                break;
            }
        } }
    g_RetraceCs = cs; g_RetraceIp = ipAfter; g_RetraceAl = VDM_REG(tib, VTIB_EAX) & BYTE_MASK;
    g_RetraceCx = VDM_REG16(tib, VTIB_ECX); g_RetracePending = 1;
}
static VOID RetraceIdle(VOID)
{
    const volatile BYTE *code;
    INT want, bit3;
    DWORD ip;
    UINT32 microseconds;
    if (!g_RetracePending) return;
    g_RetracePending = 0;
    if (g_RetraceOffset < 0) g_RetraceOffset = (GetFileAttributesA(RTIDLE_OFF_FLAG) != INVALID_FILE_ATTRIBUTES);
    if (g_RetraceOffset) return;
    ip = g_RetraceIp;
    code = (const volatile BYTE *)(ULONG_PTR)((g_RetraceCs << PARAGRAPH_SHIFT) + ip);
    /* test al,08h (A8 08) or and al,08h (24 08), then back to the IN with jz/jnz (74/75)
       -- or, #183 (s84 census: Skyroads' only hot site, 276:6287, 5.5M reads a run), with
       loopz/loopnz (E1/E0): the same wait with a CX timeout. */
    if (!((code[0] == X86_OP_TEST_IMM_BYTE || code[0] == X86_OP_AND_AL_IMM) && code[1] == VIDEO_STATUS1_VERTICAL_RETRACE
          && (code[2] == X86_OP_JZ_SHORT || code[2] == X86_OP_JNZ_SHORT || code[2] == X86_OP_LOOPE || code[2] == X86_OP_LOOPNE)))
        return;
    {   INT target = (INT)((ip + RETRACE_IDLE_PATTERN_LENGTH + (INT8)code[3]) & WORD_MASK);
        if (target > (INT)ip || (INT)ip - target > RETRACE_IDLE_BACK_MAX) return;    /* must jump back to the IN */ }
    /* A LOOP whose CX runs out THIS time round exits on its own: leave it be. (It
       decrements first, so CX == 1 is the last pass.) Sleeping otherwise only spends
       fewer iterations of the timeout per millisecond, which a real slow bus did too. */
    if ((code[2] == X86_OP_LOOPE || code[2] == X86_OP_LOOPNE) && g_RetraceCx <= 1u) return;
    /* jz / loopz loop while the bit is CLEAR -> waiting for bit 3 SET */
    want = (code[2] == X86_OP_JZ_SHORT || code[2] == X86_OP_LOOPE);
    bit3 = (g_RetraceAl & VIDEO_STATUS1_VERTICAL_RETRACE) != 0;
    if (bit3 == want) return;          /* the loop exits this time round */
    microseconds = VddVideoUsToRetrace(&g_Video, want);
    if (microseconds == VIDEO_RETRACE_UNKNOWN_U || microseconds < RETRACE_IDLE_MIN_US) return;
    ++g_RetraceIdles;
    Sleep(1);
}
