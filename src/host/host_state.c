/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The machine's state that the whole host shares: the virtual devices, the guest
 *   CPU context and the run-wide flags.
 *
 * Its own translation unit (#335): the variables are declared in host_state.h.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include "host_state.h"
#include "host_strings.h"   /* defines only: the initialisers' text */

PFN_ADD_VECTORED_EXCEPTION_HANDLER                  g_PfnAddVeh;
PFN_REGISTER_RAW_INPUT_DEVICES g_PfnRegisterRawInput;
PFN_GET_RAW_INPUT_DATA         g_PfnGetRawInput;
DOS_SAFE_SKIPS g_Safe;           /* s90 #132: all zero unless SAFE MODE (dos_recovery.h) */
/* The live machine, for the WATCHDOG THREAD. A wedged run is terminated forcefully and
 * therefore skips the normal wind-down -- which is where captured DOS output is flushed,
 * so everything the program printed was thrown away in exactly the case where it matters
 * most. Doom prints its whole startup and then hangs; without this the log proved it had
 * run but could not show what it said.
 */
DOS_MACHINE *g_Machine = NULL;

/* The device bus + its VDDs + the presentation layer live for the host's life. */
VDD_BUS      g_Bus;
PIT_STATE    g_Pit;       NTVDD_DEVICE g_PitDevice;
PIC_STATE    g_Pic;       NTVDD_DEVICE g_PicDevice;
VIDEO_STATE  g_Video;       NTVDD_DEVICE g_VideoDevice;
INPUT_STATE  g_Input;        NTVDD_DEVICE g_InputDevice;
SPEAKER_STATE g_Speaker;      NTVDD_DEVICE g_SpeakerDevice;
/* The REAL speaker, and whether the setting wants it. g_Speaker drives the mixer;
 * this drives Beep.sys. They are independent -- "Both" means both.
 */
PCSPEAKER  g_PcSpeaker = { INVALID_HANDLE_VALUE, 0, 0, 0, 0, 0 };
INT    g_SpeakerReal;
OPL_STATE    g_Opl;       NTVDD_DEVICE g_OplDevice;
SB_STATE     g_Sb;        NTVDD_DEVICE g_SbDevice;
/* The Gravis UltraSound (s80, north star 2): 240h, IRQ 11, DMA 3 -- off the SB's 220h/5/1/5. */
GUS_STATE    g_Gus;       NTVDD_DEVICE g_GusDevice;
COMM_STATE   g_Comm;      NTVDD_DEVICE g_CommDevice;   /* GH #9 */
/* -- THE GAMEPORT (session 62). The VDD models the 558 one-shot behind port
 * 0x201; this side feeds it: a winmm poll thread (joyGetPosEx -- XP-safe,
 * loaded dynamically like waveOut, no new import) writes present/axes/
 * buttons, and SettingsApply writes the adapter type. Until now the port
 * was UNCLAIMED and Mario Bros died right after a CLI poll of it.
 */
JOYSTICK_STATE    g_Joystick;       NTVDD_DEVICE g_JoystickDevice;
AUDIO_STATE  g_Audio;     AUDIO_WAVE g_Wave;
PRESENT_DDRAW g_PresentDdraw;
DOS_XMS_STATE    g_Xms;       /* M4: XMS extended-memory manager */
volatile LONG g_Irq0Pending = 0;    /* PIT raised IRQ0 (UI thread sets, V86 thread delivers) */
/* s90 (#278): hardware interrupts raised by a 32-bit component (call_ica_hw_interrupt,
 * bin\wowshim\NTVDM.EXE), one bit per PIC line: bits 0-7 master (vectors 08h-0Fh),
 * 8-15 slave (70h-77h). Set from ANY thread; delivered on the guest thread.
 */
volatile LONG g_IcaPending = 0;
volatile LONG g_PmTickOwed = 0;
volatile LONG g_Irq1Pending = 0;    /* count of un-delivered keyboard IRQ1s (one per scancode byte) */
INT g_InPmIrq     = 0;             /* #2b: re-entrancy guard while inside an injected PM ISR */
/* LOCK CONTENTION INSTRUMENT:
 * MEASURED (gameplay run, 2026-08-21): the guest's clock went 448 ms without
 * advancing and then received all 448 ms at once. At Skyroads' ~291 Hz tick that
 * is ~130 sequencer steps in a single burst -- the "speeds up for a few
 * milliseconds and then returns to normal" the user reports. It is very likely
 * also the held-key bug, because a UI thread that is not pumping messages
 * delivers no auto-repeat, and IRQ1 is only delivered when the exec loop gets a
 * turn: 2274 scancodes over several minutes is ~4.7 events/s, far too few for
 * held-key play.
 *
 * - THE FORK THIS EXISTS TO SETTLE. Is the UI thread BLOCKED ON THIS LOCK, or is
 *   it simply not being scheduled? The two need opposite fixes -- split the lock
 *   versus change thread priority/pumping -- and choosing between them by
 *   reasoning is exactly what went wrong earlier today, when a threshold picked
 *   from an unmeasured assumption made the timing worse rather than better.
 *   lk_wait  longest time a thread sat waiting in EnterCriticalSection
 *   lk_hold  longest OUTERMOST hold, with the source line that took it
 *   ui_gap   longest gap between WM_TIMER entries, taken BEFORE any lock
 *   Read them together: a large ui_gap with a small lk_wait means the thread is
 *   not running at all; a large lk_wait means contention, and lk_hold names the
 *   culprit by line number.
 *
 * - Only the OUTERMOST acquisition is timed. g_Lock is recursive for the exec
 *   thread (HostIoDo already holds it when HostPitSync re-enters), so timing
 *   every acquisition would report near-zero holds for the nested ones and bury
 *   the one that actually matters.
 */
LARGE_INTEGER g_QpcFrequency;
INT      g_LockSite, g_LockHoldSite, g_LockWaitSite;
UINT32 g_LockHoldMicroseconds, g_LockWaitMicroseconds, g_UiGapMicroseconds;
INT      g_UiTickMinimumMs = UITICK_AUTO;
DWORD    g_KeyMessageHistogram[8];     /* queue delay ms: 0,1,2,4,8,16,32,64+ */
DWORD    g_KeyMessageMaximumMs, g_KeyMessageCount;
DWORD    g_KeyDeliveryHistogram[8];     /* queue->INT 09h ms: same buckets */
DWORD    g_KeyDeliveryMaximumMs, g_KeyDeliveryCount;
INT      g_KeyIrqRetry = KEYIRQ_RETRY_ON;      /* keyirq.txt = 0 restores single-attempt */
HWND         g_Window;
HANDLE       g_KeyEvent;            /* signalled when a key is pushed */
volatile LONG g_Running = 1;         /* 0 once the window is closed */
INT g_DpmiPm = 0;                   /* set once the guest is switched to PM (spike) */
/* A CLIENT THAT EXITS INSIDE A NESTED RUN HAS STILL EXITED. (s80):
 * The PM `AH=4Ch` arm answers 0, and the main loop reads 0 as "the client is gone". But
 * the IRQ, mouse-callback and 0303 injectors run the client's code in loops of their
 * own, and to them 0 was just "the handler did not IRET". So an exit taken inside an
 * interrupt handler -- DOS/4GW's own abort path is exactly that -- was logged as
 * "PM ISR ABANDONED", the dead client's context was restored, and the main loop entered
 * it again: every selector freed, and the host access-violated in the fault trampoline
 * (`DPMI FATAL c0000005`, bytes `1f 0f a9 0f a1 61` = pop ds/gs/fs, popa). One flag,
 * set by the one arm that means "exited", checked where the main loop enters PM.
 */
INT g_PmClientExited = 0;
DWORD g_DpmiCodeBase = 0;          /* linear base of the guest PM code seg (retcs<<4) */
DWORD g_PatchMapLinear[DPMI_PMAP_SLOTS];      /* 0 = empty (linear 0 is never a site) */
BYTE  g_PatchMapVector[DPMI_PMAP_SLOTS];
INT g_PmNoIrq = 0;
/* THE EIP WE HANDED TO VdmStartExecution. The kernel's exception record reports a
 * fault EIP that is NOT reliable -- E+0, E+1 and E+3 all measured, and pmal.com and
 * pmstep.com fault at DIFFERENT offsets on byte-identical code. Since the guest has
 * provably executed nothing when the fault arrives (pmal makes AL a program counter:
 * AL=0 at the first fault, and unchanged at the second), this is where it really is.
 */
volatile LONG g_PmEntryEip = -1;
INT   g_BreakpointCount = 0;
/* --- run 52 hang-diagnostic telemetry (GH #2) ---------------------------------------
 * The PM loop can stop advancing in three indistinguishable-in-the-log ways: (a) the
 * main thread wedges INSIDE one DpmiEnterProtectedMode() because the kernel silently swallowed a
 * plain-instruction PM #GP and skip-resumes it forever (the deep wall, runs 20-34);
 * (b) the client busy-polls something we don't provide, so the host `for steps` loop
 * spins servicing the SAME patched INT over and over; (c) genuine slow progress. The
 * watchdog thread samples these to tell them apart: a live host-loop heartbeat (advances
 * only when the loop iterates -> distinguishes (b)/(c) from (a)), the guest CS:EIP handed
 * to the LAST DpmiEnterProtectedMode (where a frozen guest is wedged), and VEH fire-counters
 * (whether a real exception was ever delivered to us at all).
 */
volatile LONG  g_DpmiIteration     = 0;  /* host PM-loop iteration heartbeat (pre-enter) */
volatile LONG  g_DpmiDone     = 0;  /* PM loop finished (client exited cleanly) -> watchdog must NOT kill */
DWORD          g_IoExtra      = 0;  /* accesses absorbed by the LOOP burst */
DWORD          g_Irq0Injected      = 0;  /* INT 08h injections into the guest */
DWORD          g_Irq0TimeLast[IRQ0TL_SECS];   /* deliveries per second of run */
DWORD          g_Irq0GapHistogram[8];       /* ms: <1,1,2,4,8,16,32,64+ */
DWORD          g_Irq0GapMaximumMs, g_Irq0GapCount;
/* Anomalous gaps (>= 2 programmed periods) and normal ones, with I/O as a RATE. */
DWORD          g_Irq0AnomalyCount, g_Irq0AnomalyMicroseconds, g_Irq0AnomalyIo;
DWORD          g_Irq0AnomalyRaise, g_Irq0AnomalyAttempts, g_Irq0AnomalyNie, g_Irq0AnomalyYield;
DWORD          g_Irq0AnomalyGeneration, g_Irq0AnomalyDelete;   /* B and A, by raise count */
volatile LONG  g_WoundDown    = 0;  /* exec loop exited: clean shutdown in progress */
INT   g_IoHotCount = 0;
WORD g_Unclaimed[IO_UNCLAIMED_MAX];
INT      g_UnclaimedCount = 0;
volatile DWORD g_DpmiLastCs  = 0;  /* guest CS after the last return */
volatile DWORD g_DpmiLastEip = 0;  /* guest EIP after the last return */
DWORD g_PmVector8ArmedMs = 0;
INT   g_PmAppHookedTimer = 0;
INT   g_DpmiVi = 1;                 /* DPMI virtual interrupt flag (INT 31h 0900/0901/0902) */
BYTE g_BiosUnimplemented[BYTE_VALUES];   /* GH #27: BIOS services a run actually wanted */

INT g_ExecDepth;
CHAR g_ProgramName[64] = HOST_PROGRAM_NONE;      /* first part of the status strip */

WORD  g_PmReturnSelector = 0;
/* GH #18: the PM-fault reflect selectors (0 = not installed). Run 67 corrected model:
 * g_DpmiFaultSelector = the handler STACK selector (writable-data) written to [TIB+0x638];
 * g_DpmiFaultCodeSelector = the handler CODE selector (with a BOP at DPMI_FAULT_COFF) whose
 * {sel,off} we plant in the class table; g_FaultTable = the handler table (stride 0x10) the
 * kernel reads via [VDM_TIB+8], indexed by fault class -- DOS_FLTSITE_N entries, each
 * pointing at its OWN BOP so the index survives the reflect (see dos_layout.h).
 */
WORD  g_DpmiFaultSelector = 0;
WORD  g_DpmiFaultCodeSelector = 0;
INT   g_LdtNext = DPMI_LDT_FIRSTFREE;
volatile BYTE *g_TibDebug = 0;        /* VDM_TIB, for the crash VEH to dump guest state */

/* Device IRQs 2-7 (the Sound Blaster's block-completion IRQ 5 above all). IRQ 0
 * and 1 keep their existing dedicated paths; everything else latches here and is
 * injected as INT (8 + irq), the PC's standard master-PIC vector mapping. Without
 * this an SB transfer completes, raises IRQ 5, and the game waits forever for an
 * interrupt the host quietly dropped.
 */
/* SIXTEEN LINES, NOT EIGHT. (s80, north star 2):
 * Device IRQs were latched and delivered for the MASTER 8259 only (lines 2-7), though
 * vdd_pic models the slave and the cascade completely. A GUS's period-typical IRQ 11
 * or 12 -- or the RTC's 8 -- could be raised and never reach a guest. The latch now
 * covers all sixteen lines and every cooperative delivery path walks them in the AT's
 * real priority order: the slave's eight hang off IRQ2, so they outrank IRQ 3-7.
 * The ASYNCHRONOUS injector covers the slave too: a latch that only waits for the
 * next trap delivers once per timer tick to a guest that spins -- measured with
 * p_irq8.com, 5 RTC interrupts in 5 BIOS ticks against ~280 on three real machines.
 */
volatile LONG  g_IrqNPending[PIC_LINES];
const BYTE  g_IrqOrder[14] = { 2, 8, 9, 10, 11, 12, 13, 14, 15, 3, 4, 5, 6, 7 };
DWORD          g_IrqRaised[PIC_LINES];     /* VddRaiseIrq calls, per line */
/* Async preemption (session 11). g_HostCpu is a handle to the thread that runs the guest
 * -- VdmQueueInterrupt's ServiceData -- duplicated once from the exec thread itself.
 * g_QiBits are the [0x714] pending bits to set alongside the queue call, and
 * g_QiRaise enables the periodic IRQ 5 the qirq probe listens for; both come from
 * QIMODE_PATH so a mode can be retried without a rebuild.
 */
HANDLE         g_HostCpu          = NULL;
/* ON BY DEFAULT since v132: async injection is what makes a real game playable (a guest
 * parked in its own handler never traps, so nothing else can deliver its timer or its
 * keystrokes), and it is now gated by a real PIC. qimode can still turn it OFF (bit 6) for
 * A/B testing, but nothing should depend on a flag file being present to work.
 */
INT            g_QiSuspended       = 1;   /* async-inject via SuspendThread+SetThreadContext */
/* Set only while the exec thread is inside VdmStartExecution, i.e. while the thread's
 * CONTEXT genuinely is the guest's frame and our loop is not touching the VDM_TIB. The
 * async injector refuses to act unless this is set, so it can never race the exec loop.
 */
volatile LONG  g_InExec       = 0;
/* #219: the window is inactive, so the machine is paused -- see HostPauseSet(). */
volatile LONG  g_PauseWant    = 0;
/* Signalled by the IRQ0 raise site when a tick is still pending after its one attempt.
 * Declared here because HostIrqSink is above the courier itself; see
 * TickCourierThread for what waits on it.
 */
HANDLE         g_CourierEvent;
DWORD          g_AsyncInjected     = 0;   /* successful async injections */
DWORD          g_AsyncBail    = 0;   /* attempts declined (guest not in a safe spot) */
/* ASYNCHRONOUS DELIVERY INTO **PROTECTED MODE**:
 * The V86 arm below has always bailed when the guest is not in V86, and that hole is
 * exactly where a DOS/4GW game lives. It is not a detail: a protected-mode guest that
 * is spinning on a memory location NEVER LEAVES PROTECTED MODE, so the cooperative
 * injector in the main loop -- which only runs BETWEEN entries -- can never reach it.
 * Doom proves it. Its millisecond delay is two instructions:
 *   153dc:  cmp  [0x28820],eax
 *   153e2:  je   153dc
 * waiting for a counter its own INT 08h handler increments. No I/O, no INT, no HLT, so
 * DpmiEnterProtectedMode() never returns and the watchdog eventually calls it a wedge. The only
 * way in is the same one this file already uses for real mode: suspend the CPU thread,
 * rewrite its context, resume. Defined next to DpmiInjectPmIrq() because it shares
 * that function's frame rules; declared here because AsyncInjectIrq() needs it.
 */
/* WHICH gate refused the last async PM injection. Doom's log showed ok=0 on all six
 * attempts with `from=0x187:...`, i.e. the thread WAS in 32-bit client PM code, so
 * DpmiAsyncInjectPm() was reached and returned 0 -- and nothing said which of its
 * ten early-outs fired. Session 18 concluded "the async mechanism is what tears the
 * VDM down" from a control where async was ON; if it never injects, that attribution
 * was to a mechanism that was not running.
 */
LONG g_AsyncWhy = 0;
DWORD g_AsyncWhyHistogram[PIC_LINES_PER_CHIP][ASYNC_WHY_MAX];
volatile LONG g_AsyncPmActive = 0;  /* an async PM interrupt is in flight */
DWORD g_AsyncPmEip = 0, g_AsyncPmEsp = 0, g_AsyncPmEflags = 0;
WORD  g_AsyncPmCs  = 0, g_AsyncPmSs  = 0;
DWORD g_LeLoadBase   = 0;           /* first [LE CODE OBJECT] allocation */
/* -- A DPMI REAL-MODE SIMULATION IS A WINDOW IN WHICH THE VDM HAS NO SETTLED
 * MODE, AND THE ASYNC INJECTOR MUST NOT LOOK INTO IT. (s72, Doom's E1M1 crash)
 * INT 31h AX=0300h/0301h/0302h save the client's protected-mode register file,
 * overwrite the TIB with a REAL-MODE one, run the handler, and put the first back.
 * AsyncInjectIrq() decides "protected mode or V86?" from the VM bit of the context
 * it has just read -- which is precisely the field being rewritten. Injecting into
 * that window builds an interrupt frame for the mode the guest is no longer in, NT
 * sees a VDM whose state makes no sense, and it TERMINATES THE PROCESS: no user-mode
 * exception (our VEH reported any=0 fatal=0), no Application Error in the event log,
 * no shutdown path -- the log simply stops. That is the signature of this crash, of
 * the Mario one, and of s69's Lemmings death.
 * - MEASURED: two of the user's Doom crashes, one with sound and one with the Sound
 *   Blaster unfitted, END ON THE SAME TWO LINES -- a pair of `simInt 0x33` (Doom polls
 *   the mouse twice a frame through DPMI). Killing a distant enemy is a heavy frame, so
 *   more IRQ0s land inside the window; that is why it looks like "shooting the imp
 *   crashes it" rather than a random fault.
 *
 * [CAUTION]: NOTHING IS LOST by refusing here. The injector's contract is already "observed
 * only -- the next tick injects", so the interrupt is simply delivered a moment
 * later, once the guest is back in a mode that exists.
 */
volatile LONG g_SimIntBusy = 0;
/* ...BUT INSIDE THAT WINDOW THE MODE *IS* SETTLED WHILE VdmRunGuest IS RUNNING. (s81, ZAR):
 * The nested 0301/0302 loop rewrites the TIB to V86, then calls VdmRunGuest() exactly as the
 * main loop does -- and for the length of that call the frame is an ordinary V86 one.
 * The guard above covered the whole window, and the loop never set g_InExec either, so
 * no interrupt could EVER reach a real-mode procedure while it ran. ZAR's Miles driver is
 * one: it starts a single-cycle SB transfer and spins on a memory flag its IRQ 5 ISR sets.
 * IRQ 5 was raised once and refused 1631 times with why=0x14 -- which s59 read as HOST_CS
 * (14) but is 20 decimal, `not_in_exec`: the bracket was missing, not the thread elsewhere.
 * - So the nested loop sets g_NestedRm (and g_InExec) around VdmRunGuest ONLY, clearing
 *   g_InExec first on return -- the re-check after the suspend then catches a thread that
 *   has left. Only DEVICE lines whose real-mode vector is the guest's own code are let
 *   through: our stubs in DOS_HDLR_SEG BOP, and the nested loop services no such BOP.
 */
volatile LONG g_NestedRm = 0;
INT    g_PitPaceOn = 1, g_PitPaceMs = 1;
volatile LONGLONG g_Int15EventEnd;     /* QPC of the AH=83h deadline; 0 = none */
DWORD g_Int15Waits, g_Int15Events, g_Int15Posted, g_Int15Busy;
/* [CAUTION]: DEFAULT OFF, AND THE REASON IS THE MEASUREMENT, NOT THE MECHANISM (Importance = 2):
 * The mechanism above is established: raises - attempts == yields, exactly, in
 * every run taken. What is NOT established is that this thread is a net win, and
 * one 45 s run cannot establish it. Five runs of nominally the same configuration
 * produced anomalous-gap counts of 50, 75, 81, 85 and 240 -- a 5x spread that is
 * larger than any effect either knob produced. (Two run SHAPES are mixed in there:
 * Skyroads sometimes plays a ~10 s intro at the BIOS 18.2 Hz and sometimes goes
 * straight to 180 Hz. That explains some of the spread and not all of it.)
 * The one courier run also showed key latency worse, not better -- which is the
 * shape you would expect if it takes interrupts-enabled windows from IRQ1, i.e.
 * exactly the competition it was built to relieve, running the other way.
 *
 * So it ships OFF and is turned on by courier.txt = 1, and the next session's job
 * is N repeated runs per arm with the run shape checked (IRQ0TL's first second says
 * which), not another single-run A/B. Shipping it on would be calling a result that
 * the data does not support -- and this file's history is mostly the cost of doing
 * exactly that.
 */
INT    g_CourierOn = 0;              /* courier.txt = 1 enables */
DWORD  g_CourierWakes, g_CourierInjected, g_CourierTries, g_CourierGiveUp;
/* APPROXIMATE CPU SPEED: THE V86 HALF. (GH #56) (Importance = 1):
 * The arithmetic and the whole argument for it are in src/host/cpuspeed.h. This
 * is the mechanism: a thread that, for the milliseconds the Bresenham says the
 * guest does not get, HOLDS THE EXEC THREAD WHERE IT STANDS.
 *
 * - WHY SUSPENSION RATHER THAN A WAIT AT A CONTROL POINT. The obvious cheaper
 *   design is to sleep at the top of the exec loop, and it covers most guests --
 *   but not the ones this feature exists for. A program doing pure computation
 *   with no I/O never faults, never BOPs and never returns from VdmStartExecution
 *   at all (the headless watchdog has a whole essay about that shape), so the exec
 *   loop never gets a turn to throttle at. Suspension is the only lever that
 *   reaches it, and the machinery is proven: AsyncInjectIrq() has suspended this
 *   same thread on every timer tick for twenty sessions.
 *
 * [CAUTION]: THE ONE RULE: NEVER SUSPEND A LOCK HOLDER. Hold the exec thread while it owns
 * g_Lock and the audio pump blocks behind it, which is a dropout you can hear.
 * g_InExec is exactly the flag that says otherwise -- it is set only around
 * VdmStartExecution, where the thread holds nothing -- but reading it and then
 * suspending is a race: the guest can trap in between and the thread be inside
 * host code by the time the suspend lands.
 * So READ IT AGAIN AFTER THE SUSPEND HAS TAKEN EFFECT. That is sound because of
 * the ORDER in the exec loop: VdmRunGuest() returns, and the very next statement
 * clears g_InExec, before any HOST_LOCK. A 1 observed on a thread that is
 * already stopped therefore means the thread is still inside the kernel call.
 *
 * [CAUTION]: GetThreadContext is what makes "has taken effect" true. SuspendThread only
 * REQUESTS the suspend; on a multiprocessor box the thread may still be running
 * when it returns, and reading its context is the documented way to wait for it.
 *
 * [CAUTION]: NESTING WITH THE IRQ INJECTOR IS FINE AND IS NOT AN ACCIDENT. Suspend counts
 * nest, both sides balance their own Suspend/Resume, and a context written into
 * a thread we are holding simply takes effect when we let go -- so an interrupt
 * raised during a held millisecond is delivered late rather than lost. That is
 * also the CORRECT semantics for this feature: the PIT still advances by real
 * elapsed wall time, so a throttled guest gets the same 18.2 ticks a second it
 * would on a slow real machine, rather than a compressed clock. Session 22
 * proved that compressing game time is catastrophic; this deliberately does not.
 */
INT    g_CpuSpeedIndex      = 0;      /* CPUSPEED_* index; 0 = unlimited */
volatile LONG g_CpuSpeedDuty = CPUSPEED_BP_FULL;   /* basis points, read by the thread */
volatile LONG g_CpuSpeedDutyRm = CPUSPEED_BP_FULL;   /* #225: the same, for a real-mode program */
DWORD  g_CpuSpeedRunMs, g_CpuSpeedHeldMs;   /* what the throttle really did */
DWORD  g_CpuSpeedMissed;   /* held millisecond the guest was not in exec for */
DWORD  g_CpuSpeedHoldMaximumMicroseconds;   /* #225: the longest single hold, and */
DWORD    g_CpuSpeedPeriods;      /* how many run/hold cycles were completed */
DWORD    g_StartMs;            /* GetTickCount at throttle start, for exec_bp */

DWORD         g_CpuSpeedCooperativeCatches, g_CpuSpeedCooperativeTimeouts;
UINT32 g_TypematicPeriodMicroseconds =  TYPEMATIC_DEFAULT_PERIOD_US;
INT           g_MouseSensitivity = PERCENT;       /* percent; msens.txt tunes feel per-guest */
/* DPMI 0300 (simulate real-mode interrupt) vectors we do NOT service. See the 0300 arm. */
DWORD g_SimIntUnhandled, g_SimIntVector[IVT_VECTORS];
/* 0Ch / 14h: THE EVENT HANDLER. STORED AND REPORTED; NOT YET CALLED:
 * A guest installs a far pointer and a call mask and expects the driver to CALL IT
 * on the masked events. We do not do that yet -- invoking guest code out of band
 * needs the same care AsyncInjectIrq takes and is its own piece of work.
 * - BUT STORING IT IS NOT COSMETIC, IT IS THE DIFFERENCE BETWEEN A KNOWN GAP AND A
 *   SILENT ONE. Before this, 0Ch fell into `default:` and returned with the guest's
 *   registers untouched -- "did nothing, reported success", the shape this codebase
 *   has now been bitten by six times. 14h in particular MUST hand back the PREVIOUS
 *   handler, and a guest that chains handlers on a zero it was never told about will
 *   jump to 0000:0000. Now the pointer round-trips, and the install is COUNTED so a
 *   log says plainly "this guest wants callbacks and is not getting them" instead of
 *   leaving it to be re-diagnosed from behaviour.
 */
volatile LONG g_MouseEventMask, g_MouseEventSegment, g_MouseEventOffset;
INT    g_MouseCallbackActive;           /* a callback is in flight */
DWORD  g_MouseCallbackInjected, g_MouseCallbackDone, g_MouseCallbackLost, g_MouseCallbackPm, g_MouseCallbackStray;
/* Why a delivery attempt did NOT happen, by reason -- a zero cb_inj must be readable:
 * [0] in flight  [1] no mask/no events  [2] no handler  [3] inside our stub  [4] IF off
 */
DWORD  g_MouseCallbackWhy[MOUSE_CB_WHY_COUNT];           /* [5] = return stub clobbered (see MS_CB_RET_OFF) */
DWORD  g_MouseEventRaised;          /* event bits ever raised by the UI side */
/* Input capture ("exclusivity") -- see InputCaptureSet. Declared up here because
 * StatusUpdate, which is defined above it, reports the capture state and the chord
 * that changes it on the right-hand half of the status strip.
 */
volatile LONG g_Captured = 0;

/* THE GUEST ASKING FOR THE MOUSE IS WHAT TAKES IT (Importance = 1):
 * A DOS program that wants the mouse says so, through INT 33h, and a program that
 * does not never calls it at all. That is a better signal than any menu item: it
 * is the guest's own declaration, and it separates the two cases exactly the way
 * the user described them -- Skyroads never calls INT 33h, so its mouse stays on
 * the Windows desktop; Doom does, so the mouse goes into the game and the desktop
 * arrow disappears.
 * - WHICH CALLS COUNT: the ones that USE the mouse (01 show cursor, 03 get position,
 *   05/06 button state, 0B read motion), NOT 00 (reset / is-a-driver-present). Very
 *   many DOS programs probe for a driver at startup and then never touch it again,
 *   and grabbing the pointer away from the desktop for one of those would be a
 *   nuisance with no benefit. Detection is not use.
 *
 * [CAUTION]: AND `00` IS DELIBERATELY EXCLUDED FOR A SECOND REASON. A mis-patched `CD 33`
 * site calls this servicer with ARBITRARY AX -- that is the `i33oth=1079` shape
 * documented on g_MouseI33Ax, and it is the class of bug that killed Doom for five
 * sessions. Keying the grab on a small set of plausible function numbers rather
 * than on "any INT 33h at all" means one stray call cannot take the user's mouse.
 *
 * [CAUTION]: THE FLAG IS SET ON THE V86/EXEC THREAD AND ACTED ON BY THE UI THREAD. ClipCursor,
 * SetWindowsHookEx and SetCursor all belong to the thread that owns the window, so
 * MouseInt33 may only ever raise a request; WM_TIMER performs it.
 *
 * [INFO]: ONCE PER PROGRAM, AND THAT IS WHAT MAKES Win+F10 MEAN SOMETHING. The latch is
 * set when we auto-capture and never cleared, so a user who escapes with Win+F10
 * stays escaped -- the guest goes on polling INT 33h every frame, and without the
 * latch every one of those polls would drag the pointer straight back in.
 */
volatile LONG g_MouseWantCapture = 0;   /* guest used the mouse; UI: please grab */
/* RULE 1 of the capture policy, and the ONE predicate for it -- "has this guest ever
 * used the mouse". Defined here rather than beside the rules it serves because the
 * status strip, which is built long before InputCaptureSet, has to answer it too.
 * The full policy is written out above InputCaptureSet; do not add a second latch.
 */
/* -- #136: SEAMLESS MOUSE. Settings > Input: "In seamless mode the program's pointer
 * follows Windows' own pointer, and no capture is needed." So it is the capture policy
 * with rules 2 and 5 switched off: a guest that uses the mouse is treated exactly like
 * one that never did -- the pointer stays the desktop's, WM_MOUSEMOVE positions and
 * buttons reach the guest (rule 6's "ordinary window" arm), and nothing ever
 * ClipCursors it. CaptureAllowed() is the one place the policy reads it.
 *
 * [CAUTION]: THE RAW DELTAS ARE GATED ON THE POINTER BEING OVER OUR VIDEO (WM_INPUT). Raw input
 * follows focus, so without that a seamless guest that reads mickeys (0Bh) would keep
 * moving while the pointer was dragged across someone else's window.
 *
 * [CAUTION]: NEITHER POINTER IS HIDDEN FOR IT: Show Host Mouse Cursor governs the arrow exactly as
 * for a guest that never used the mouse (Smart hides it after 5 s still), and the guest draws its
 * own as it always did -- so with Always, two pointers show. Hiding the arrow over the video would
 * leave NO pointer in a graphics mode where our INT 33h draws none (09h shapes are accepted and
 * discarded); unmeasured which guests that is. Default OFF = capture, the behaviour every build so
 * far has had. Live: OK in the dialog releases a held capture at once (SettingsApply).
 */
volatile LONG g_MouseSeamless = 0;
NE_MODULE g_WowModule[WOW_MAX_MOD];
BYTE  *g_WowImage[WOW_MAX_MOD];
INT       g_WowModuleCount = 0;

WORD      g_PmTransferSegment  = 0;
WORD      g_WowPspSelector[WOW_PSP_TRACK];
INT       g_WowPspCount = 0;

INT  g_WowLaunch = 0;                /* `-w`: this VDM hosts Win16 */
/* TWO COPIES, BECAUSE THE MENU AND THE DIALOG MEAN DIFFERENT THINGS (Importance = 1):
 * g_Settings is WHAT IS IN FORCE. g_SettingsDisk is WHAT THE REGISTRY HOLDS. They start
 * identical and diverge only when something is tried from a menu.
 * - The View menu (and Machine > CPU Speed) write g_Settings and never save, so a scaler
 *   or a speed tried while watching something run is gone at the next launch.
 * - The Settings dialog is populated from g_SettingsDisk and OK writes BOTH -- it is
 *   the editor for the saved configuration, and pressing OK is what keeping
 *   something means.
 *
 * [CAUTION]: SO OK ALSO DISCARDS ANY SESSION OVERRIDE, and that is the point rather than an
 * oversight: if the dialog showed the live values instead, then changing an audio
 * setting and pressing OK would silently make every display experiment permanent.
 * Two meanings, two copies, and the one you edit is the one you save.
 */
NTVDMEX_SETTINGS g_Settings;
DOS_MACHINE   *g_DosMachine;          /* so the DOS version can be changed live */

/* THE CARD, IN ONE PLACE, BECAUSE THREE THINGS HAVE TO AGREE ABOUT IT:
 * vdd_sb answers at this port and raises this IRQ; the DMA controller moves the
 * bytes on this channel; and the guest is TOLD all three in BLASTER. Telling it
 * one thing while doing another is worse than saying nothing at all -- a driver
 * that believes the string waits on an interrupt that arrives elsewhere -- so the
 * numbers exist once and every consumer reads them from here.
 */
DOS_SB_CONFIG g_SbConfig = { SB_DEFAULT_BASE, SB_DEFAULT_IRQ, SB_DEFAULT_DMA8,
                             0 /* H is not advertised by default -- see dos_env.h */,
                             DOS_SB_DEFAULT_TYPE, 0, 0 };
/* Whether the extended/expanded memory managers announce themselves at all. Off
 * means INT 2Fh AX=4300 does not answer and there is no INT 67h vector, which is
 * the state a real machine is in with no HIMEM/EMM386 line in CONFIG.SYS -- and
 * which some games specifically want.
 */
INT g_XmsOn = 1, g_EmsOn = 1;

/* PROBE A GUEST POINTER WITHOUT FAULTING. Session 17, and it cost a run:
 * IsBadReadPtr does its job by TOUCHING the memory inside an SEH frame -- so on a bad
 * pointer it raises an access violation, and a VECTORED handler sees that before the
 * SEH frame swallows it. DpmiCrashVeh below is exactly such a handler, and while a
 * PM client is running it treats any fault with a flat CS as a reflected guest INT
 * 31h: it rewrote our OWN thread's CONTEXT and resumed it. A diagnostic that guards
 * itself with IsBadReadPtr therefore KILLS the run it is diagnosing, and the log ends
 * one line before the thing you added it to see. That is what happened here.
 * VirtualQuery answers the same question by asking the memory manager instead of the
 * CPU, so it cannot raise. Committed + readable (any of the four read-capable
 * protections) + the whole span inside one region is the test.
 */
/* The thread that RUNS THE GUEST (the main one). Recorded so the fatal dump can say
 * whether a host-side crash happened on it or on one of the worker threads -- audio,
 * present, watchdog. Those are different bugs and the dump used to name neither.
 */
DWORD g_GuestThreadId = 0;
INT   g_WowSchedOn = 0;
/* s92 (#306): every task that is not running, parked (wowsched.h WOWSCHED_MAX). */
WOWSCHED_SLOT g_WowSchedSlots[WOWSCHED_MAX];
INT g_WowWindowNested = 0;
BYTE *g_WowShadow = NULL;
/* -- THE DEFAULT PM HANDLER FOR A HARDWARE IRQ, WHEN THE GUEST OWNS THE REAL-MODE
 * VECTOR: A TRUE NESTED-V86 REFLECTION. (s81, ZAR's streaming audio) ---------------
 * DPMI 0.9: a protected-mode interrupt nobody hooked in PM is reflected to the real-mode
 * vector. DOS/4GW hooks every IRQ in PM with a pass-up handler that chains to OUR default
 * stub, so every real-mode ISR a DOS/4GW program relies on is reached through here. The
 * IVT-is-ours case was handled (s80, keyboard); this is the other one. ZAR's Miles
 * driver is a REAL-MODE ISR at IVT[0Dh] (SBLASTER.DIG +0x777), and every one of its SB
 * block interrupts arrived at 177:0027 and was abandoned -- 3,323 "PM ISR ABANDONED" in
 * one run -- so the double buffer was never swapped and the card fell silent after init.
 * - This is 0302 with no RMCS: save the PM register file, enter V86 at IVT[vec] with an
 *   IRET frame onto the DPMI_RMRET catcher and interrupts OFF (a hardware ISR is entered
 *   with IF clear; the frame carries IF set, which its IRET restores), run it to the
 *   catcher, restore PM. General registers are not an input or an output of a hardware
 *   ISR, so nothing is marshalled. Returns 1 when the ISR ran to its IRET.
 *
 * [CAUTION]: Its own stack, below 0301's default one (the code segment at FF00), so a reflection
 * can never land on a frame that call is using.
 */
DWORD g_PmIrqRmReflects = 0, g_PmIrqRmFail = 0;

/* The shared tables that had anonymous types (#335). */
SKIP_IF_SITE g_SkipIfSite[SKIPIF_SITES];
IO_HOT_PORT g_IoHot[IO_HOT_MAX];
/* DPMI PM interrupt-vector table (INT 31h 0204/0205). A client installs its own PM
 * handlers here; we store them so a get/set/restore round-trips faithfully. (We still
 * service patched INT 21h/31h ourselves -- routing to a client-installed PM handler is
 * a deeper item; storing the vectors is what real extenders' save/restore needs.)
 */
/* `client` distinguishes a vector the CLIENT installed (0205 / INT 21h AH=25h) from
 * the host default we pre-load into every entry at mode-switch time. The difference is
 * load-bearing in two places: we only ROUTE an interrupt to a handler the client chose,
 * and we only INJECT IRQ0 into an INT 08h the client actually hooked -- injecting into
 * our own default would be a very confusing way to talk to ourselves.
 */
PM_INTERRUPT_VECTOR g_PmInt[IVT_VECTORS];
DPMI_MEMORY_BLOCK g_DpmiBlock[DPMI_MEMBLK_MAX];
/* DPMI PM EXCEPTION-handler table (INT 31h 0202/0203). Separate from g_PmInt on
 * purpose: 0202/0203 address CPU exceptions 00h-1Fh, which are a different namespace
 * from the interrupt vectors 0204/0205 addresses -- a client may legitimately install
 * a #GP (0Dh) exception handler and an INT 0Dh (IRQ5) interrupt handler at once, and
 * collapsing them into one table would make each silently overwrite the other.
 * Doom's DOS/4GW makes 45 of these calls -- the biggest single block of UNSUP in the
 * session-16 trace -- installing its own fault handlers before it runs the game.
 */
PM_EXCEPTION_VECTOR g_PmException[X86_EXCEPTIONS];
/* DPMI 0303 real-mode callbacks: each slot records the client's PM handler (sel:off)
 * and the RMCS buffer (sel:off) to marshal register state through. g_PmReturnSelector is a
 * code selector based at DOS_HDLR_SEG (0x500) so the PM handler's IRET lands on the
 * planted DPMI_PMRET catcher; allocated lazily on the first 0303.
 */
DPMI_CALLBACK g_Callbacks[DPMI_CB_SLOTS];
IFV_TRACE_ENTRY g_IfvTrace[IFV_TRACE_MAX];
PM_INJECT_SITE g_PmInjectSite[PMINJ_SITES];
/* --- menu + status bar (scaffold; most items are stubs for now) ------------ */
I33_FUNCTION_COUNT g_MouseI33Ax[I33_AXN];
I33_CALL_SITE g_MouseI33Site[I33_SITEN];
RETRACE_SITE g_RetraceSite[RT_SITES];
ISV_IO_HOOK g_IsvHooks[ISV_MAX_HOOKS];

/* The DPMI host's fault trampolines and the stack they run on (#205), and its LDT shadow:
 * until #335 these were tentative definitions in a header main.c included.
 */
BYTE  g_FaultTable[DOS_FLTSITE_N * DPMI_FAULT_TABLE_ENTRY] __attribute__((aligned(16)));
BYTE  g_FaultStack[DPMI_FAULT_STK_SIZE] __attribute__((aligned(16)));
DPMI_DESCRIPTOR g_Ldt[DPMI_LDT_MAX];
