/* main.c -- the clean DOS VDM host (windowed). Orchestrates the pipeline the
 * tools/vdmhost spike proved, now wired through the src/ modules:
 *   log -> CSRSS handshake (csrss) -> V86 bring-up (v86) -> load + build the DOS
 *   process (dos_loader/dos_psp/dos_mcb) -> service INT 21h/10h + I/O in V86,
 *   routing screen output to the video VDD and presenting via DirectDraw.
 * No CRT (src/runtime.c supplies the entry + mem*); imports only XP system DLLs.
 *
 * M3 merge: the host now owns a GUI window on a UI thread (present_ddraw) and the
 * V86/DOS engine runs on the main thread (the VdmInitialize thread). DOS console
 * output (INT 21h AH=02/09/40) and INT 10h are routed into the video VDD, whose
 * 80x25 cell grid is rendered + blitted into the Luna-themed window. The host
 * stays a CUI subsystem image so the CSRSS VDM handshake still binds.
 */
#include <windows.h>
#include <commctrl.h>
#include "ntvdm.h"
#include "v86.h"
#include "dpmi.h"
#include "csrss.h"
#include "log.h"
#include "x86len.h"     /* which `CD nn` byte pairs are really INT instructions */
#include "dos_mcb.h"
#include "dos_loader.h"
#include "dos_psp.h"
#include "dos_env.h"
#include "dos_int21.h"
#include "dos_layout.h"
#include "dos_ctab.h"
#include "dos_xms.h"
#include "dos_ems.h"
#include "vdd_bus.h"
#include "vdd_pit.h"
#include "vdd_pic.h"
#include "vdd_video.h"
#include "vdd_input.h"
#include "vdd_speaker.h"
#include "vdd_dma.h"
#include "vdd_opl.h"
#include "vdd_sb.h"
#include "vdd_mpu.h"
#include "vdd_audio.h"
#include "audio_wave.h"
#include "present_ddraw.h"

#define LOG_PATH    "C:\\ntvdmex\\ntvdmhost.log"
#define TARGET_PATH "C:\\ntvdmex\\target.txt"
#define AUTOEXIT_PATH "C:\\ntvdmex\\autoexit"   /* marker: headless test mode -> exit when the guest exits */
/* Opt-in screenshot flag. Lives on the SMB SHARE folder so the remote driver can
   toggle it (create it before a GRAPHICAL test, remove it otherwise). Non-graphical
   tests (selftest/dpmitest) then never touch the self-capture path -- keeping the
   common case off the capture code entirely. */
#define CAPTURE_FLAG "C:\\Documents and Settings\\All Users\\Documents\\ntvdmex\\capture.flag"
/* Mode-Y de-interleave tuning; see modey_flush() in vdd_video.c. Contents = the run
   coalescing slack in dwords. Absent = the built-in default. */
#define MODEY_PATH "C:\\Documents and Settings\\All Users\\Documents\\ntvdmex\\modey.txt"
/* Per-plane backing for mode Y is ON by default -- see the MODE-Y PLANE BACKING block.
   This file DISABLES it and falls back to the de-interleave heuristic, which is worth
   keeping only because it is what a machine that refuses the remap will use. */
#define SBDUMP_FLAG  "C:\\Documents and Settings\\All Users\\Documents\\ntvdmex\\sbdump.flag"
#define SBDUMP_PATH  "C:\\ntvdmex\\sb.raw"
#define NOREMAP_FLAG "C:\\Documents and Settings\\All Users\\Documents\\ntvdmex\\noremap.flag"
/* Diagnostic knob: disable the mode-12h A0000 NOACCESS trap. With it off, planar
   writes land in the raw aperture instead of the VGA engine, so the PICTURE will
   be wrong -- the question it answers is whether the guest EXECUTES AT ALL.
   Absent = normal behaviour, so ordinary runs are untouched. Delete after use. */
#define NOA000_FLAG  "C:\\Documents and Settings\\All Users\\Documents\\ntvdmex\\noa000.flag"
/* Mode 12h WITHOUT the A0000 page trap (GH #55). Arming that trap stops the V86
   guest running at all -- 10 I/O events in 30s against 22.5 MILLION with it off.
   But we do not actually need it: in mode 12h QuickBASIC reprograms a VGA
   register via OUT between pixels, so the PORT traps alone hand us control
   constantly, and the batching interpreter can then run the pixel loop with its
   A0000 stores going through the planar engine. This knob keys the interpreter
   off "planar mode is active" instead of "the page is protected". */
#define INTERP12_FLAG "C:\\Documents and Settings\\All Users\\Documents\\ntvdmex\\interp12.flag"
/* Escape hatch for the planar policy (GH #55): present = go back to the A0000
   page trap. Interpreting the guest for the whole time a planar mode is set is
   the DEFAULT because the page trap demonstrably freezes the guest on real
   hardware; this knob exists so the old path is still one file away. */
#define P12OFF_FLAG   "C:\\Documents and Settings\\All Users\\Documents\\ntvdmex\\p12off.flag"
/* Dev-only: capture the exact OPL register stream a game sends, with timestamps,
   so it can be replayed offline through BOTH our synth and a reference core and
   the audio diffed. Counting register writes cannot say WHY an instrument sounds
   wrong; comparing waveforms from identical input can. Absent = no cost at all. */
#define OPLTRACE_FLAG "C:\\Documents and Settings\\All Users\\Documents\\ntvdmex\\opltrace.flag"
#define OPLTRACE_PATH "C:\\Documents and Settings\\All Users\\Documents\\ntvdmex\\opltrace.txt"
/* Async-preemption experiment knob, also on the share so it can be changed between
   runs without a rebuild. One digit: bits 0-1 = the FIXED_NTVDMSTATE pending bits to
   set before NtVdmControl(VdmQueueInterrupt) (1 = VDM_INT_HARDWARE, 2 = VDM_INT_TIMER,
   3 = both), bit 2 = raise a periodic device IRQ 5 for the qirq probe. Absent = the
   pre-session-11 behaviour (latch the pending bit only, never queue). */
#define QIMODE_PATH "C:\\Documents and Settings\\All Users\\Documents\\ntvdmex\\qimode.txt"
/* Headless wall-clock cap override, decimal milliseconds, also on the share. The 30 s
   default is right for an unattended test that must not wedge the watcher, but an
   INTERACTIVE test on the box -- keylog, where a human walks over and presses every key
   -- needs minutes, and the default would kill the guest mid-typing. Absent = the
   default. */
#define HEADLESS_MS_PATH "C:\\Documents and Settings\\All Users\\Documents\\ntvdmex\\headless_ms.txt"
#define AWBUFS_PATH      "C:\\Documents and Settings\\All Users\\Documents\\ntvdmex\\awbufs.txt"
#define AWFRAMES_PATH    "C:\\Documents and Settings\\All Users\\Documents\\ntvdmex\\awframes.txt"
#define EXECPRIO_PATH    "C:\\Documents and Settings\\All Users\\Documents\\ntvdmex\\execprio.txt"
#define DSPVER_PATH      "C:\\Documents and Settings\\All Users\\Documents\\ntvdmex\\dspver.txt"
#define SBGATE_PATH      "C:\\Documents and Settings\\All Users\\Documents\\ntvdmex\\sbgate.txt"
#define PITPACE_PATH     "C:\\Documents and Settings\\All Users\\Documents\\ntvdmex\\pitpace.txt"
/* Scripted synthetic keystrokes, on the share so a test sequence can be changed between
   runs without a rebuild. Whitespace-separated tokens, played once in order:
     4d     -- scancode 4D: make, brief hold, break
     e4d    -- the same as an EXTENDED key (E0 prefix) -- every arrow is one of these
     w1500  -- wait 1500 ms
   A hardcoded "tap UP 400 times" cannot reach a specific screen, and worse, UP is a no-op
   on a menu whose first item is already selected -- a probe that cannot tell success from
   failure. A script can say "wait for the intro, Enter, DOWN, DOWN, Enter". */
#define KEYS_PATH "C:\\Documents and Settings\\All Users\\Documents\\ntvdmex\\keys.txt"

/* Offset, within DOS_HDLR_SEG, of the XMS API far-call entry stub (BOP 0x43; RETF).
   Lives just past the INT 1Ah stub (which ends at 0x40) and the INT 2Fh stub (4 bytes
   at 0x40). INT 2Fh AX=4310 hands the guest DOS_HDLR_SEG:XMS_ENTRY_OFF to far-call. */
#define XMS_ENTRY_OFF 0x0044

/* DPMI (M4 slice 3, spike): the mode-switch entry far-called by a client after it
   detects DPMI via INT 2Fh AX=1687h. Lives past the INT 67h stub (0x48..0x4B).
   The stub is `BOP 0x50 ; RETF`: the host services the BOP by switching the VDM to
   protected mode (rewriting CS:IP), so the RETF only runs if the switch FAILS (it
   returns to the client in real mode with CF=1). See src/vdm/dpmi.c. */
#define DPMI_ENTRY_OFF 0x0050
#define DPMI_BOP       0x50
/* DPMI 0301 (call real-mode procedure): the far-return catcher. To run a client's
   real-mode proc we switch the VDM back to V86 and push a far-return frame pointing
   here; when the proc RETFs it lands on this BOP, which the 0301 handler recognises
   as "the real-mode call finished" and switches back to protected mode. Lives just
   past the mode-switch entry (0x50..0x53). */
#define DPMI_RMRET_OFF 0x0054
#define DPMI_RMRET_BOP 0x54
/* DPMI 0303 (allocate real-mode callback): planted real-mode BOP entries (one per
   callback slot) that a client's real-mode code far-calls; the host switches V86->PM
   and runs the client's PM handler. DPMI_PMRET is the PM-side return catcher the
   handler IRETs to (reached via g_pmret_sel, a code selector based at DOS_HDLR_SEG).
   All within the 0x500..0x5FF handler segment (env seg starts at 0x600). */
#define DPMI_CB_BOP      0x55
#define DPMI_CB_BASE_OFF 0x0060      /* slot i entry at DOS_HDLR_SEG:(base + i*4) */
#define DPMI_CB_SLOTS    4
#define DPMI_PMRET_BOP   0x56
#define DPMI_PMRET_OFF   0x0070
/* INT 31h 0306 RAW MODE SWITCH (Doom/DOS/4GW needs it -- it tests CF from 0306 and
   `jmp`s to its abort path when the call fails, which is exactly where it died).
   Spec (DPMI 1.0 0306): returns BX:CX = real-to-protected entry, SI:(E)DI =
   protected-to-real entry. Both are entered by FAR JMP -- not call -- with
     AX = new DS, CX = new ES, DX = new SS, (E)BX = new (E)SP,
     SI = new CS, (E)DI = new (E)IP
   (E)BP is preserved across the switch; FS/GS read 0 afterwards; the other GPRs are
   undefined. So each entry is just a BOP the host traps and completes by rewriting
   the CONTEXT -- there is no return address to honour, which is why a FAR JMP is
   safe. NB offset 0x58 is DOS_IRET_STUB_OFF and BOP 0x57 is DPMI_FAULT_BOP; these
   take the next free slots in both namespaces. */
#define DPMI_RAW2PM_BOP  0x58        /* real -> protected (entered in V86)          */
#define DPMI_RAW2PM_OFF  0x005C
#define DPMI_RAW2RM_BOP  0x59        /* protected -> real (entered in PM)           */
#define DPMI_RAW2RM_OFF  0x0074
/* INT 31h 0305 state save/restore. Both procedures are FAR CALLed with AL=0 save /
   AL=1 restore, ES:(E)DI = buffer, and MUST PRESERVE ALL REGISTERS. We keep the whole
   guest register file in the VDM_TIB across every mode switch we perform, so there is
   no host-side state the client has to hand back to us -- a bare RETF is a correct,
   register-preserving implementation. Planted as data (0xCB), not a BOP: it never
   needs to reach the host at all. */
#define DPMI_SSR_OFF     0x0078
/* Highest linear address XP's LDT descriptor validator will accept for base+limit
   (MmHighestUserAddress on a 2GB-user build). Kernel RE session 7 recovered the rule
   from PspIsDescriptorValid; run 30 confirmed base 0 / limit 0x7FFEF / G=1 installs
   while a true 4GB selector does not. Used to clamp a client's flat selector. */
#define XP_LDT_MAX_LINEAR 0x7FFEFFFFu

/* Shared IRET stub for every vector that has no real handler.
 *
 * IT USED TO LIVE AT 0x66 AND WAS BEING CLOBBERED. The DPMI real-mode callback
 * slots are based at 0x60 with a 4-byte stride, so slot 1 occupies 0x64-0x66 and
 * its third byte (DPMI_CB_BOP, 0x55) is written AFTER the stub -- leaving 0x55
 * there, which decodes as PUSH BP and then runs into uninitialised memory. Every
 * vector pointed at the "safe" stub was therefore pointed at a crash. Latent for
 * IRQ 2-7/8-15, and much worse once #27 started filling every null vector with it.
 *
 * HANDLER SEGMENT MAP -- check this before adding anything:
 *   0x00-0x1F  INT 21h BOP + DBCS(0x18) + EMM name(0x0A)
 *   0x20,0x28,0x30,0x34,0x3A,0x3C,0x40,0x48,0x4C  INT 10/16/33/08/1C/1A/2F/67/09 stubs
 *   0x44       XMS entry          0x50-0x53  DPMI entry     0x54-0x56  DPMI RMRET
 *   0x58       >>> this stub <<<  0x60-0x6F  DPMI CB slots  0x70-0x72  DPMI PMRET
 *   0x80       DPMI fault BOP (code selector)
 */
#define DOS_IRET_STUB_OFF 0x0058
#define DOS_SYSVARS_OFF   0x0090      /* AH=52h list of lists; MCB head at -2 (GH #35) */

/* GH #18 real-CPU PM-fault trampoline. When a raw (non-BOP) protected-mode #GP faults,
   the kernel reflect path jumps the guest to [VDM_TIB+0x638]:0x1000 (see ntvdm.h
   VTIB_FLT_*). We install a code selector H (g_dpmi_fault_sel) based at DOS_HDLR_SEG<<4
   and plant a BOP (C4 C4 57) at offset 0x1000 within it -- linear (DOS_HDLR_SEG<<4)+0x1000
   = 0x1500, in the mapped V86 window, in the unused gap below the env block (0x6000) and
   the program (PSP 0x10000). The reflect therefore surfaces to the host as VTIB_EVENT=4
   with CS==H and EIP==0x1000, and the saved faulting CS:EIP/SS:ESP sit in VTIB_FLT_SAV*.
   This routes a raw PM #GP (SS-retype, HLT, privileged op) into the same host loop that
   services the INT->BOP path -- ntvdm's mechanism (Kernel RE session 6), no ntvdm globals. */
/* Run 67 corrected mechanism (static disasm of 0x4f6e6f/0x4f6f67/0x4f6efd):
   - [TIB+0x638] is the fault handler's STACK selector (writable-DATA); the reflect sets
     new SS:ESP = [TIB+0x638]:0x1000 and builds an IRET frame there (0x4f6dc0 validates it
     as writable-data -- a CODE selector is REJECTED, which is why run 65/66 failed).
   - The handler CS:EIP comes from a table pointed to by [VDM_TIB+8], indexed by fault
     CLASS (KiTrap0D pushes 6 for a #GP), stride 0x10: entry = {CS@+0 word, EIP@+4 dword}.
     0x4f6efd reads it; 0x4f6d3c validates CS as code + EIP<=limit.
   So we need TWO selectors + a table: a data stack selector and a code selector with a BOP,
   and the table[6] = {code_sel, bop_off}, with [VDM_TIB+8] pointing at the table. */
#define DPMI_FAULT_STK_SEG 0x0200    /* stack sel base = 0x2000; Sd:0x1000 = linear 0x3000 */
#define DPMI_FAULT_COFF    0x0080    /* BOP offset within the handler code selector (linear 0x580) */
#define DPMI_FAULT_BOP     0x57      /* BOP number planted at the handler code:COFF        */
#define DPMI_FLT_CLASS_GP  6         /* KiTrap0D's fault class for a #GP (push 6)           */
#define DPMI_TIB_FLTTBL    0x08      /* VDM_TIB offset holding the handler-table pointer    */

/* EMS (M4): the LIM page frame is a 64KB RAM window in the UMA. v86_map_ems_frame
   scans the conventional page-frame segments AFTER VdmInitialize for a free 64KB
   hole and maps it there; g_ems_frame_lin holds the linear base actually chosen
   (0 => none found, EMS unavailable). The guest learns the segment via AH=41. */
#define EMS_POOL_PAGES 512                           /* 512 * 16KB = 8MB of EMS */
static DWORD g_ems_frame_lin;                        /* set by v86_map_ems_frame */

/* CSRSS receive buffers + program image (no CRT heap; static = zero-init). */
static char g_cmd[1024], g_app[1024], g_cur[512], g_pif[512];
static char g_env[8192], g_desk[512], g_title[512], g_rsv[512];
static VDM_COMMAND_INFO g_ci;
static BYTE filebuf[0x80000];   /* 512KB: hold a real game's MZ image (DOS/4GW stub etc.), run 85 */

/* The live machine, for the WATCHDOG THREAD. A wedged run is terminated forcefully and
   therefore skips the normal wind-down -- which is where captured DOS output is flushed,
   so everything the program printed was thrown away in exactly the case where it matters
   most. Doom prints its whole startup and then hangs; without this the log proved it had
   run but could not show what it said. */
static dos_machine_t *g_mach = NULL;

/* The device bus + its VDDs + the presentation layer live for the host's life. */
static vdd_bus      g_bus;
static pit_state    g_pit;       static ntvdd g_pit_dev;
static pic_state    g_pic;       static ntvdd g_pic_dev;
static video_state  g_vid;       static ntvdd g_vid_dev;
static input_state  g_in;        static ntvdd g_in_dev;
static speaker_state g_spk;      static ntvdd g_spk_dev;
static dma_state    g_dma;       static ntvdd g_dma_dev;
static opl_state    g_opl;       static ntvdd g_opl_dev;
static sb_state     g_sb;        static ntvdd g_sb_dev;
static mpu_state    g_mpu;       static ntvdd g_mpu_dev;
static audio_state  g_audio;     static audio_wave g_wave;
static present_ddraw g_pd;
static xms_state    g_xms;       /* M4: XMS extended-memory manager           */
static ems_state    g_ems;       /* M4: EMS expanded-memory manager           */
/* PENDING TIMER TICKS, as a saturating COUNT rather than a flag. A boolean coalesces: every
   tick that falls while the guest has interrupts off -- and Skyroads spends most of its time
   in exactly that state, CLI'd around its 256-colour palette writes -- was silently thrown
   away, so the music's tempo wandered with whatever the guest happened to be doing. A real
   8259 latches the request and delivers it the moment IF comes back. Saturating at 4 keeps
   that fidelity without letting a long CLI region accumulate a burst that would then flood
   the guest with back-to-back timer interrupts. */
#define IRQ0_PENDING_MAX 4
static volatile LONG g_irq0_pending = 0;    /* PIT raised IRQ0 (UI thread sets, V86 thread delivers) */
/* ── HOW MANY TIMER TICKS DOES THE PROTECTED-MODE CLIENT ACTUALLY OWE? ───────────────
     Separate from g_irq0_pending, which SATURATES AT FOUR on purpose (see above) and so
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
       syncs    host_pit_sync() calls that advanced the clock. This is the CEILING on
                async delivery, because g_async_tried_this_sync allows ONE attempt each.
       raises   what the 8254 model actually generated: should be ~140/s.
       attempts async_inject_irq(0) calls -- syncs that got as far as trying.
       owed_max the backlog high-water mark: how far behind delivery ever fell.
     ⚠ SUSPECT THE LOCK. host_pit_sync takes g_lock, and Doom's mode-Y drawing does
       ~43,000 port writes a second (1.95M mask changes a run), every one of which also
       takes it. If syncs come in well under the UI thread's 5 ms tick, the video path
       is starving the timer, which is starving the audio. */
static uint32_t g_pit_syncs;
static uint32_t g_pit_async_attempts;
static LONG     g_pm_tick_owed_max;
/* ── ...AND owed_max IS A HIGH-WATER MARK, NOT AN OCCUPANCY. ─────────────────────────
     "owed_max = 0x40 = PM_TICK_OWED_MAX, the backlog is PERMANENTLY SATURATED" reads a
     maximum as a steady state: ONE stall anywhere in 45 s pins it at the cap for the
     rest of the run and it can never come back down. A saturated backlog and a backlog
     that touched the cap once look identical in that number, and they mean opposite
     things about whether delivery is keeping up.
     So sample the DEPTH at every sync. Nine buckets, one increment, no lock. */
static uint32_t g_pm_owed_hist[9];
static void pm_owed_sample(LONG d)
{
    unsigned b = 0;
    if      (d <= 0)  b = 0;
    else if (d <  4)  b = (unsigned)d;          /* 1, 2, 3 exactly */
    else if (d <  8)  b = 4;
    else if (d < 16)  b = 5;
    else if (d < 32)  b = 6;
    else if (d < 64)  b = 7;
    else              b = 8;
    g_pm_owed_hist[b]++;
}
static volatile LONG g_pm_tick_owed = 0;
static void irq0_latch(void)
{
    if (g_irq0_pending < IRQ0_PENDING_MAX) InterlockedIncrement(&g_irq0_pending);
    if (g_pm_tick_owed < PM_TICK_OWED_MAX)  InterlockedIncrement(&g_pm_tick_owed);
    if (g_pm_tick_owed > g_pm_tick_owed_max) g_pm_tick_owed_max = g_pm_tick_owed;
}
/* Consume one owed tick. Returns 0 if none is owed, i.e. "the client is up to date --
   do not manufacture time it has not been billed for". */
static int pm_tick_take(void)
{
    if (g_pm_tick_owed <= 0) return 0;
    InterlockedDecrement(&g_pm_tick_owed);
    return 1;
}
static int   g_async_tried_this_sync = 0;   /* see host_irq_sink: one attempt per PIT sync */
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
static DWORD g_tickgap[12], g_tickgap_max_us, g_tickgap_over;   /* >11.6ms = a block */
static void tick_delivered_note(void)
{
    static LARGE_INTEGER pf, prev;
    LARGE_INTEGER now;
    if (!pf.QuadPart && !QueryPerformanceFrequency(&pf)) return;
    if (!QueryPerformanceCounter(&now)) return;
    if (prev.QuadPart) {
        LONGLONG us = ((now.QuadPart - prev.QuadPart) * 1000000) / pf.QuadPart;
        unsigned b = 0;
        while (b < 11 && us >= (LONGLONG)500 << b) ++b;      /* 0.5,1,2,4..512 ms */
        g_tickgap[b]++;
        if (us > (LONGLONG)g_tickgap_max_us) g_tickgap_max_us = (DWORD)us;
        if (us > 11600) g_tickgap_over++;   /* longer than one 128-byte block at 11111 Hz */
    }
    prev = now;
}
static DWORD g_keypm_logged  = 0;           /* bounded KEYPM account; see the PM exec loop */
static DWORD g_keyirq_logged = 0;           /* bounded KEYIRQ account; see host_irq_sink */
static volatile LONG g_irq1_pending = 0;    /* count of un-delivered keyboard IRQ1s (one per scancode byte) */
static int g_pm_irq0_latch = 0;             /* #2b: a virtual IRQ0 awaiting injection into the PM hook */
static int g_in_pm_irq     = 0;             /* #2b: re-entrancy guard while inside an injected PM ISR   */
static CRITICAL_SECTION g_lock;             /* serialises all bus dispatch       */

/* ── LOCK CONTENTION INSTRUMENT ──────────────────────────────────────────────
   MEASURED (gameplay run, 2026-08-21): the guest's clock went 448 ms without
   advancing and then received all 448 ms at once. At Skyroads' ~291 Hz tick that
   is ~130 sequencer steps in a single burst -- the "speeds up for a few
   milliseconds and then returns to normal" the user reports. It is very likely
   also the held-key bug, because a UI thread that is not pumping messages
   delivers no auto-repeat, and IRQ1 is only delivered when the exec loop gets a
   turn: 2274 scancodes over several minutes is ~4.7 events/s, far too few for
   held-key play.

   ► THE FORK THIS EXISTS TO SETTLE. Is the UI thread BLOCKED ON THIS LOCK, or is
     it simply not being scheduled? The two need opposite fixes -- split the lock
     versus change thread priority/pumping -- and choosing between them by
     reasoning is exactly what went wrong earlier today, when a threshold picked
     from an unmeasured assumption made the timing worse rather than better.
       lk_wait  longest time a thread sat waiting in EnterCriticalSection
       lk_hold  longest OUTERMOST hold, with the source line that took it
       ui_gap   longest gap between WM_TIMER entries, taken BEFORE any lock
     Read them together: a large ui_gap with a small lk_wait means the thread is
     not running at all; a large lk_wait means contention, and lk_hold names the
     culprit by line number.

   ► Only the OUTERMOST acquisition is timed. g_lock is recursive for the exec
     thread (host_io_do already holds it when host_pit_sync re-enters), so timing
     every acquisition would report near-zero holds for the nested ones and bury
     the one that actually matters. */
static LARGE_INTEGER g_qpf;
static DWORD    g_lk_owner, g_lk_depth;
static LONGLONG g_lk_since;
static int      g_lk_site, g_lk_hold_site, g_lk_wait_site;
static uint32_t g_lk_hold_us, g_lk_wait_us, g_ui_gap_us;

static uint32_t qpc_us(LONGLONG d)
{
    if (!g_qpf.QuadPart || d <= 0) return 0;
    return (uint32_t)((d * 1000000) / g_qpf.QuadPart);
}

static void host_lock_enter(int site)
{
    LARGE_INTEGER a, b;
    DWORD me = GetCurrentThreadId();
    int nested = (g_lk_owner == me && g_lk_depth != 0);
    QueryPerformanceCounter(&a);
    EnterCriticalSection(&g_lock);
    if (!nested) {
        uint32_t w;
        QueryPerformanceCounter(&b);
        w = qpc_us(b.QuadPart - a.QuadPart);
        if (w > g_lk_wait_us) { g_lk_wait_us = w; g_lk_wait_site = site; }
        g_lk_owner = me; g_lk_since = b.QuadPart; g_lk_site = site; g_lk_depth = 0;
    }
    g_lk_depth++;
}

static void host_lock_leave(void)
{
    if (g_lk_depth && --g_lk_depth == 0) {
        LARGE_INTEGER n;
        uint32_t h;
        QueryPerformanceCounter(&n);
        h = qpc_us(n.QuadPart - g_lk_since);
        if (h > g_lk_hold_us) { g_lk_hold_us = h; g_lk_hold_site = g_lk_site; }
        g_lk_owner = 0;                 /* clear BEFORE releasing: the next owner
                                           must not see us as the holder */
    }
    LeaveCriticalSection(&g_lock);
}
/* __LINE__ gives every site its own identity without touching 28 call sites by hand. */
#define HOST_LOCK()   host_lock_enter(__LINE__)
#define HOST_UNLOCK() host_lock_leave()
static HWND         g_hwnd;
static HANDLE       g_key_event;            /* signalled when a key is pushed     */
static volatile LONG g_running = 1;         /* 0 once the window is closed         */
static int g_dpmi_pm = 0;                   /* set once the guest is switched to PM (spike) */
static DWORD g_dpmi_code_base = 0;          /* linear base of the guest PM code seg (retcs<<4) */
/* ── THE INT->BOP PATCH MAP, KEYED BY LINEAR ADDRESS. ─────────────────────────────
   It used to be keyed by OFFSET INTO A SINGLE 64K WINDOW at g_dpmi_code_base, and that
   is exactly as much of the guest as the one up-front scan could reach. Session 17
   measured what that misses: DOS/4GW allocates conventional memory with INT 21h AH=48h,
   READS A PROTECTED-MODE MODULE INTO IT from DOOM.EXE, retypes the descriptor to code
   (INT 31h 0009, access 0xFB) and far-jumps in. That module's first instruction is
   `mov ah,0x30 / CD 21` -- a RAW int, outside the window, never patched. A raw INT in
   PM raises a #GP the kernel will not reflect, so the VDM died silently, at the same
   address, every run since session 15.
   Keying by linear address lets the map cover every region the client later declares to
   be code, and makes dpmi_bop_vec's alias handling fall out for free. Sized to the V86
   window plus the HMA; the DOS blocks Doom loads into land around 0x15000-0x2F000. */
/* Keyed by LINEAR ADDRESS and stored as a HASH, not a flat array.
   It was a flat byte array over the low 1.1 MB, which is exactly as much of the guest
   as it could describe -- and Doom walked straight out of it. Once the extender is
   working properly it loads its protected-mode modules into EXTENDED memory (INT 31h
   0501 -> VirtualAlloc, addresses around 0x0398xxxx), retypes those descriptors to code
   and jumps in. Those modules' `CD 21` instructions were therefore never patched, and a
   raw INT in protected mode is the #GP XP will not reflect: instant silent VDM death at
   the handoff.
   A flat array cannot cover arbitrary VirtualAlloc addresses, so this is an open-
   addressed hash of linear -> original vector. It is also FASTER than what it replaces:
   unpatch/repatch used to sweep up to 1.1 MB of array per INT 31h 0301/0302, and now
   sweep 64K slots. */
#define DPMI_PMAP_SLOTS 65536u                 /* power of two, open addressing */
#define DPMI_PMAP_MASK  (DPMI_PMAP_SLOTS - 1u)
static DWORD g_pmap_lin[DPMI_PMAP_SLOTS];      /* 0 = empty (linear 0 is never a site) */
static BYTE  g_pmap_vec[DPMI_PMAP_SLOTS];
static DWORD g_pmap_n;

static DWORD pmap_hash(DWORD lin) { return ((lin * 2654435761u) >> 8) & DPMI_PMAP_MASK; }

static BYTE pmap_get(DWORD lin)
{
    DWORD i = pmap_hash(lin), k;
    if (!lin) return 0;
    for (k = 0; k < DPMI_PMAP_SLOTS; ++k) {
        DWORD sl = (i + k) & DPMI_PMAP_MASK;
        if (!g_pmap_lin[sl]) return 0;
        if (g_pmap_lin[sl] == lin) return g_pmap_vec[sl];
    }
    return 0;
}

static void pmap_set(DWORD lin, BYTE vec)
{
    DWORD i = pmap_hash(lin), k;
    if (!lin || g_pmap_n >= DPMI_PMAP_SLOTS - 16) return;   /* leave headroom, never fill */
    for (k = 0; k < DPMI_PMAP_SLOTS; ++k) {
        DWORD sl = (i + k) & DPMI_PMAP_MASK;
        if (!g_pmap_lin[sl]) { g_pmap_lin[sl] = lin; g_pmap_vec[sl] = vec; ++g_pmap_n; return; }
        if (g_pmap_lin[sl] == lin) { g_pmap_vec[sl] = vec; return; }
    }
}

/* Clearing leaves the key in place with vec=0: a tombstone, so probe chains that ran
   through this slot still find what is past it. Slots are never reused, which is fine
   at these counts and is the whole reason for the headroom check above. */
static void pmap_clear(DWORD lin)
{
    DWORD i = pmap_hash(lin), k;
    for (k = 0; k < DPMI_PMAP_SLOTS; ++k) {
        DWORD sl = (i + k) & DPMI_PMAP_MASK;
        if (!g_pmap_lin[sl]) return;
        if (g_pmap_lin[sl] == lin) { g_pmap_vec[sl] = 0; return; }
    }
}

/* ── GUEST BREAKPOINTS IN PROTECTED MODE. ─────────────────────────────────────────
   THE PROBLEM THIS EXISTS FOR, because it has now cost three sessions. When a PM
   client dies, the kernel terminates the whole VDM: no exception reaches our VEH, the
   fault trampoline does not catch, and the log simply stops. All we ever learn is the
   last INT the client executed -- and between two INTs there can be thousands of
   instructions. Session 17 broke one such wall by dumping the guest stack and reading
   DOS/4GW's handoff frame, but that worked only because the dead stretch happened to
   end in a far transfer whose operands were in memory. The next stretch is straight-
   line code, and that trick does not generalise.
   A breakpoint does. The INT->BOP patch already proves we can make the guest stop at a
   chosen byte and hand us its full register file; a breakpoint is the same mechanism
   pointed at an address WE choose rather than one the client's INT happens to sit on.
   Bisecting a dead stretch then costs one run per step instead of one rebuild per idea.
   DRIVEN FROM A FILE ON THE SHARE, deliberately: addresses come from disassembling the
   client, they change every time you halve the interval, and a rebuild-and-deploy cycle
   per guess is the thing that makes people stop bisecting and start speculating.
   ONE-SHOT by design: on hit we restore the original bytes and do NOT advance EIP, so
   the real instruction executes and the client runs on undisturbed. A loop therefore
   reports its first pass, not its ten-thousandth. */
#define PMBP_PATH "C:\\Documents and Settings\\All Users\\Documents\\ntvdmex\\pmbp.txt"
/* Suppress the asynchronous IRQ0 -> PM INT 08h injection. A knob, not a feature: when a
   client dies the instant we deliver a timer tick, the first question is whether the
   DELIVERY is wrong or merely BADLY TIMED, and the cheapest way to ask it is to stop
   delivering and see how much further the client gets. Absent file = normal behaviour. */
#define PMNOIRQ_PATH "C:\\Documents and Settings\\All Users\\Documents\\ntvdmex\\pmnoirq.flag"
#define PMVEHPASS_PATH "C:\\Documents and Settings\\All Users\\Documents\\ntvdmex\\pmvehpass.flag"
#define NOSB_PATH      "C:\\Documents and Settings\\All Users\\Documents\\ntvdmex\\nosb.flag"
/* ── A WATCH ADDRESS: ONE HEX LINEAR ADDRESS, DUMPED EITHER SIDE OF EACH INJECTED
     INTERRUPT. ─────────────────────────────────────────────────────────────────────
   The question an injected timer tick always raises is not "did the handler run" --
   the phases/done counters answer that -- but "did it have the EFFECT the guest is
   waiting for". Those are different, and confusing them is how this turned into
   guesswork: Doom's ISR enters and IRETs cleanly while the spin it should release
   goes round for ever.
   So watch the thing the guest is actually testing. Doom's delay is
       153dc:  cmp [0x28820],eax
       153e2:  je  153dc
   and after relocation that counter is linear 0x03b68820 (obj3 base 0x03b40000).
   Put `03b68820` in the file and every injection prints the dword before and after.
     counter MOVES but the spin does not exit -> the wait is hundreds of ticks: a RATE
                                                 problem, coalesce far harder
     counter DOES NOT move while ticks complete -> the handler we enter is not the one
                                                 that increments it: a different bug
   Absent file = no watch and no cost, like every other knob here. */
#define PMWATCH_PATH "C:\\Documents and Settings\\All Users\\Documents\\ntvdmex\\pmwatch.txt"
/* ── RUN PROTECTED MODE UNDER THE KERNEL MONITOR INSTEAD OF IN-PROCESS. ──────────
   This host far-jmps into PM (dpmi_enter.S) because an early spike found
   VdmStartExecution faulting when it ran PM -- and everything expensive we have
   built since exists to work around that one decision: the INT->BOP patch map
   (because a raw PM INT is not reflected to us) and the asynchronous
   SuspendThread injector (because the kernel will not deliver interrupts to us).
   The second is not a preference. Measured: PM entry loads flags with `popfd`,
   POPFD at CPL 3 cannot modify VIF, and the kernel's delivery gate reads VIF --
   so an in-process PM guest can NEVER be given a hardware interrupt by the
   kernel, whatever we write. Only ring 0 can set those flags, which is exactly
   what stock ntvdm gets and why it reaches Doom's title screen on this box.
   So it is worth asking the original question again, against a host that now has
   working descriptors, services and thunks rather than almost nothing. Opt-in,
   because the far-jmp path is what currently works. */
#define PMKERNEL_PATH "C:\\Documents and Settings\\All Users\\Documents\\ntvdmex\\pmkernel.flag"
static int g_pm_noirq = 0;
static int g_pm_veh_pass = 0;   /* pmvehpass.flag: let a non-INT PM fault fall THROUGH the VEH */
/* THE EIP WE HANDED TO VdmStartExecution. The kernel's exception record reports a
   fault EIP that is NOT reliable -- E+0, E+1 and E+3 all measured, and pmal.com and
   pmstep.com fault at DIFFERENT offsets on byte-identical code. Since the guest has
   provably executed nothing when the fault arrives (pmal makes AL a program counter:
   AL=0 at the first fault, and unchanged at the second), this is where it really is. */
static volatile LONG g_pm_entry_eip = -1;
/* Per-event checkpoint verbosity. The full dump -- registers, stack, frame, entry code
   -- was built for the era when the client died inside the FIRST dpmi_enter_pm and the
   only question was "did we get there at all". Now that a client runs for thousands of
   events it is the thing stopping it: a Doom run hit the 4 MB log cap at event 0xdb1
   with the game still loading. So: a handful of checkpoints always (they still catch a
   death at the switch), and the full firehose only when asked for. */
#define PMVERBOSE_PATH "C:\\Documents and Settings\\All Users\\Documents\\ntvdmex\\pmverbose.flag"
static unsigned g_dpmi_cp_max = 8;
#define DPMI_BP_VEC  0xEE               /* sentinel in g_int_vec[]: not a real vector */
#define DPMI_BP_MAX  32
static DWORD g_bp_lin[DPMI_BP_MAX];     /* requested linear addresses (from PMBP_PATH) */
static DWORD g_bp_dump[DPMI_BP_MAX];    /* optional 2nd column: linear addr to dump on hit */
/* Optional 3rd column: bytes to SKIP on hit instead of re-executing the instruction.
   This turns a breakpoint into a one-instruction PATCH, which is how you test "would
   the client survive if this instruction simply did not happen?" without a rebuild.
   Session 17 needed exactly that for a `STI`: at CPL 3 with IOPL 0 it raises the one
   #GP XP will not reflect, so it kills the VDM -- and the question "is STI the only
   thing in the way" is answerable in one run by skipping it. */
static DWORD g_bp_skip[DPMI_BP_MAX];
/* Optional 4th column: 1 = plant a ONE-BYTE INT3 (0xCC) instead of the two-byte BOP.
   This exists to answer one question that forks the whole CLI/STI strategy: does a
   protected-mode TRAP reach our VEH at all? Runs 20-34 had the kernel reflecting PM
   SOFTWARE interrupts to us (the SegCs==0x1B arm), and #BP is software-generated, so
   there is reason to hope -- but hoping is not measuring. An INT3 in guest code arrives
   with the GUEST's CS, so it falls to dpmi_crash_veh's fatal arm and prints
   "DPMI FATAL: exception code=0x80000003". Seeing that line instead of a silent death
   is the answer. A one-byte trap is also the only patch that FITS over CLI/STI. */
static DWORD g_bp_mode[DPMI_BP_MAX];
static BYTE  g_bp_pending[DPMI_BP_MAX];  /* skipped -> needs re-arming once EIP moves on */
/* Optional 5th column: 1 = REPEATING. One-shot is right for a "how far did it get"
   sweep, and useless for a loop -- the first pass eats every breakpoint and the failing
   iteration is the thousandth. A repeating breakpoint cannot re-plant itself while the
   guest is standing on its footprint, so it re-arms the way skip mode does: mark it
   pending and let the NEXT event (typically the other breakpoint in the same loop) put
   it back. Put at least TWO repeating breakpoints in a loop and they alternate, which
   gives a register dump per iteration. */
static DWORD g_bp_rep[DPMI_BP_MAX];
static BYTE  g_bp_orig[DPMI_BP_MAX][2]; /* the two bytes we displaced                  */
static BYTE  g_bp_armed[DPMI_BP_MAX];
static int   g_bp_n = 0;

/* --- run 52 hang-diagnostic telemetry (GH #2) ---------------------------------------
   The PM loop can stop advancing in three indistinguishable-in-the-log ways: (a) the
   main thread wedges INSIDE one dpmi_enter_pm() because the kernel silently swallowed a
   plain-instruction PM #GP and skip-resumes it forever (the deep wall, runs 20-34);
   (b) the client busy-polls something we don't provide, so the host `for steps` loop
   spins servicing the SAME patched INT over and over; (c) genuine slow progress. The
   watchdog thread samples these to tell them apart: a live host-loop heartbeat (advances
   only when the loop iterates -> distinguishes (b)/(c) from (a)), the guest CS:EIP handed
   to the LAST dpmi_enter_pm (where a frozen guest is wedged), and VEH fire-counters
   (whether a real exception was ever delivered to us at all). */
static volatile LONG  g_dpmi_iter     = 0;  /* host PM-loop iteration heartbeat (pre-enter) */
static volatile LONG  g_dpmi_done     = 0;  /* PM loop finished (client exited cleanly) -> watchdog must NOT kill */
static int            g_headless      = 0;  /* AUTOEXIT marker present: SMB test harness -> bound infinite runs */
/* Exec-loop instrumentation, reported once at wind-down (cheap counters, no I/O in
   the hot path). These separate the two costs that look identical from outside: how
   many times we round-tripped to service port I/O, how many accesses the burst fast
   path absorbed without a round trip, and how many timer IRQs actually reached the
   guest -- the last one being how we caught the BIOS tick starving during an I/O
   storm (iobench case 1 could not accumulate 5 ticks in 30 s). */
static DWORD          g_ev_io         = 0;  /* port-I/O events serviced            */
static DWORD          g_io_extra      = 0;  /* accesses absorbed by the LOOP burst */
static DWORD          g_irq0_inj      = 0;  /* INT 08h injections into the guest   */
static DWORD          g_irq0_skip     = 0;  /* IRQ0 delivery gated off (IF=0 etc.) */
static DWORD          g_irq0_skip_if  = 0;  /* ...because the guest had interrupts off  */
static DWORD          g_irq0_skip_stub= 0;  /* ...because we were inside our INT 08h stub */
static DWORD          g_pit_reload_log = 0;
static DWORD          g_ev_intpend    = 0;  /* event-3 interrupt-pending notifications */
static DWORD          g_ev_iostr      = 0;  /* REP INS/OUTS (event 1) reflects serviced */
static DWORD          g_irq1_inj      = 0;  /* INT 09h injections (should track scancodes) */
static DWORD          g_irqn_inj      = 0;  /* device IRQs (2-7) injected into the guest */
static DWORD          g_irqn_refuse_log = 0; /* bounded refusal-log budget (see the gate)  */
static DWORD          g_irqn_refuse_total = 0;
static volatile LONG  g_wound_down    = 0;  /* exec loop exited: clean shutdown in progress */
#define IO_HOT_MAX 48   /* 12 filled up before the hottest port was even seen */
static uint16_t g_io_last_port = 0;      /* port the last serviced access touched */
static struct { uint16_t port; DWORD n; } g_io_hot[IO_HOT_MAX];
static int   g_io_hot_n = 0;
static DWORD g_io_site_logged = 0;
#define IO_UNCLAIMED_MAX 24
static uint16_t g_unclaimed[IO_UNCLAIMED_MAX];
static int      g_unclaimed_n = 0;

static unsigned       g_capture_ms    = 300; /* CAPTURE_FLAG contents: ms between shots */
static int            g_capture       = 0;  /* CAPTURE_FLAG present: opt-in self-screenshot for graphical tests */
static int            g_no_a000       = 0;  /* NOA000_FLAG present: leave A0000 mapped (diagnostic) */
static int            g_interp12      = 0;  /* INTERP12_FLAG: interpret mode 12h, no page trap */
static int            g_p12_off       = 0;  /* P12OFF_FLAG: revert to the A0000 page trap      */
static DWORD          g_run_start_tick= 0;  /* exec-loop start, so STAGE2 can report a RATE    */
/* OPL register trace (opltrace.flag). One entry per write; a busy run is ~6k
   writes a minute, so the cap is far above anything real and exists only so a
   runaway cannot eat memory. Dropped writes are reported, never silently lost. */
#define OPLTRACE_MAX 262144
static struct { DWORD us; BYTE reg, val; } g_opltrace[OPLTRACE_MAX];
static DWORD          g_opltrace_n    = 0;
static DWORD          g_opltrace_drop = 0;
static int            g_opltrace_on   = 0;
/* The UI tick must be well ABOVE the guest refresh (60/70 Hz) or the phase window
   is unreachable and every present falls back to the staleness path. */
#define VID_PRESENT_TICK_MS   5
#define VID_PRESENT_STALE_MS 25    /* never let the screen go quiet longer than this */
/* Planar-mode interpretation, measured. `batches` is how many times we drove the
   guest from the host, `instrs` how many instructions that came to, and `bails`
   how often the interpreter declined the very first opcode and had to let V86 run
   (each bail is a stretch of guest execution whose A0000 writes we do NOT see). */
static DWORD          g_p12_batches   = 0;
static DWORD          g_p12_instrs    = 0;
static DWORD          g_p12_bails     = 0;
#define PM_HEADLESS_MS_DEFAULT 30000             /* headless exec-loop wall-clock cap (an infinite visual demo self-exits) */
static DWORD g_headless_ms = PM_HEADLESS_MS_DEFAULT;   /* overridable via HEADLESS_MS_PATH */
#define PM_HEADLESS_MS g_headless_ms
#define PM_HEADLESS_GRACE_MS 3000                /* grace for a clean wind-down before the hard backstop forces exit */
static volatile DWORD g_dpmi_enter_cs = 0;  /* guest CS handed to the last dpmi_enter_pm    */
static volatile DWORD g_dpmi_enter_eip= 0;  /* guest EIP handed to the last dpmi_enter_pm   */
static volatile DWORD g_dpmi_last_ev  = 0;  /* VTIB_EVENT reported by the last return        */
static volatile DWORD g_dpmi_last_cs  = 0;  /* guest CS after the last return                */
static volatile DWORD g_dpmi_last_eip = 0;  /* guest EIP after the last return               */
static volatile DWORD g_dpmi_last_vec = 0;  /* vector serviced on the last iteration         */
static volatile LONG  g_veh_any       = 0;  /* # PM-context exceptions delivered to the VEH  */
static volatile LONG  g_veh_fatal     = 0;  /* # of those that took the non-reflect fatal path */
static DWORD dpmi_sel_base(WORD sel);       /* fwd: watchdog resolves the frozen selector base */
/* DPMI PM interrupt-vector table (INT 31h 0204/0205). A client installs its own PM
   handlers here; we store them so a get/set/restore round-trips faithfully. (We still
   service patched INT 21h/31h ourselves -- routing to a client-installed PM handler is
   a deeper item; storing the vectors is what real extenders' save/restore needs.) */
/* `client` distinguishes a vector the CLIENT installed (0205 / INT 21h AH=25h) from
   the host default we pre-load into every entry at mode-switch time. The difference is
   load-bearing in two places: we only ROUTE an interrupt to a handler the client chose,
   and we only INJECT IRQ0 into an INT 08h the client actually hooked -- injecting into
   our own default would be a very confusing way to talk to ourselves. */
static struct { WORD sel; DWORD off; BYTE client; } g_pm_int[256];
/* ── THE HOST'S DEFAULT PROTECTED-MODE INTERRUPT HANDLERS. ────────────────────────
   0204 (get PM interrupt vector) used to return 0000:0000 for anything the client had
   not yet installed, and that is not an answer -- it is a null pointer wearing the
   shape of one. A DOS extender reads the CURRENT vector before installing its own,
   precisely so it can CHAIN to it for anything it does not handle itself; Doom's
   DOS/4GW does exactly that for INT 21h, and then failed every call it wanted to pass
   down (its private AX=FF00 among them) because the chain led nowhere.
   So every vector starts out pointing at a three-byte stub of our own:
       C4 C4 CF     BOP ; IRET
   The third byte does double duty -- it is the BOP's immediate AND the IRET that
   returns to the client -- which is exactly the +2 EIP convention the patched-INT path
   already uses, so it needs no special case in the dispatcher. Each stub's LINEAR
   address is registered in the patch map against its vector, so dpmi_bop_vec() resolves
   a chained call straight back to the service the client was asking for. */
/* Every block we have handed the client through INT 31h 0501. We know exactly which
   memory is the client's because we allocated it -- and that is the only usable answer
   when the client declares a FLAT code selector. See dpmi_patch_code_region().
   `code` marks a block that holds one of the program's EXECUTABLE objects, matched by
   size against the LE object table -- see dpmi_le_learn(). */
#define DPMI_MEMBLK_MAX 64
static struct { DWORD base, size; BYTE code; } g_dpmi_blk[DPMI_MEMBLK_MAX];
static int g_dpmi_nblk = 0;

/* ── THE CLIENT'S EXECUTABLE DECLARES WHICH OF ITS MEMORY IS CODE. ────────────────
   A flat code selector (base 0, limit 4 GB) cannot be scanned for INT sites, so an
   application whose own code lives behind one -- every DOS/4GW game -- runs with its
   `CD 21`/`CD 31` unpatched, and a raw INT in protected mode is the one fault XP will
   not reflect. Scanning "every block we handed out" was tried and is WORSE (session 17:
   it patched bytes inside DATA and the run ended earlier), because the client's memory
   is code and data mixed.
   But the client is an LE ("linear executable") image, and an LE names its own objects:
   each carries a flags word with bit 2 = EXECUTABLE. DOS/4GW allocates ONE 0501 block
   per object, sized to the object's virtual size rounded up to a page -- so the object
   table tells us which blocks are code, exactly, with no guessing at content.
   Measured on DOOM.EXE, and the correspondence is exact in all three cases:
       obj1  vsize 0x44f71  READ|EXEC|BIG32   -> 0501 of 0x45000  = the game's code
       obj2  vsize 0x00019  READ|EXEC|ALIAS16 -> 0501 of 0x01000  (also gets a based
                                                  descriptor, so it was already patched)
       obj3  vsize 0x85e10  READ|WRITE|BIG32  -> 0501 of 0x86000  = data, NOT scanned
   ► ONLY OBJECTS OF 64 KB AND UP ARE MATCHED. A page-rounded size is a weak key when it
     is small: 0x1000 is the commonest allocation there is, and Doom makes an unrelated
     one. Small code objects need a based descriptor to be reachable at all (obj2 does),
     which the 0009/000C path already patches -- so the size key is only ever asked to
     identify the big flat-addressed image, where it is distinctive. */
#define DPMI_LE_MAX 16
static DWORD g_le_code_sz[DPMI_LE_MAX];   /* page-rounded sizes of the EXEC objects */
static int   g_le_ncode = 0;

/* When the client last installed a PM INT 08h handler, and how long IRQ0 injection must
   then hold off. One 18.2 Hz tick period is 54.9 ms -- the shortest gap real hardware can
   put between "the vector exists" and "the timer fires". See INT 31h 0205. */
#define DPMI_IRQ0_ARM_QUIET_MS 55
/* Ticks run per asynchronous entry -- see the drain in the main loop. */
/* ► A BATCH IS CATCH-UP, NOT A LICENCE TO COMPRESS TIME. At Doom's 140 Hz, draining
     64 ticks back to back hands the guest 0.45 SECONDS of game time in microseconds --
     and its timer ISR is where DMX writes PCM into the DMA ring, so the ring is filled
     in bursts while the mixer reads it smoothly. That is a chk-a-chk-a in the sampled
     audio no amount of mixer accuracy can undo. The exec loop gets a turn thousands of
     times a second, so a small batch still clears any real backlog. */
#define DPMI_IRQ0_BATCH 4
/* How far an injected protected-mode ISR may run before we stop waiting for its IRET.
   See the commentary at the phase loop in dpmi_inject_pm_irq(): a phase is one PM entry,
   not a unit of time, so the real bound is the clock; the phase count is only a backstop
   against a handler that traps forever without making progress. */
#define DPMI_IRQ0_PHASE_MAX 65536u
#define DPMI_IRQ0_MS_MAX    500u
static DWORD g_pm_vec8_armed_ms = 0;
/* Set when the APPLICATION (not the extender's arming pass) installs a timer ISR. Until
   then vector 8 holds a placeholder stub and delivering to it is both pointless and, on
   DOS/4GW, fatal. Learned by snooping INT 21h AH=25h -- see the routing path. */
static int   g_dpmi_use_kernel = 0;         /* pmkernel.flag: run PM via VdmStartExecution */
static int   g_pm_app_hooked_timer = 0;
/* ...and WHERE it is. Kept apart from g_pm_int[8] on purpose: that table is what INT 31h
   0204 reports back, and it must keep saying exactly what the client installed through
   0205. Answering 0204 with a handler DOS/4GW never set is how the first attempt at this
   produced "fatal error (1001): error in interrupt chain" -- the extender queried the
   vector during shutdown, did not recognise it, and concluded its chain was corrupt.
   Where we DELIVER and what the vector table REPORTS are two different questions. */
static WORD  g_pm_app_timer_sel = 0;
static DWORD g_pm_app_timer_off = 0;
/* Where an injected IRQ should actually land. For the timer on a 32-bit client that is the
   application's own ISR once we have seen it installed; otherwise the vector table. */
/* ► DELIVER THROUGH THE EXTENDER'S OWN STUB, NOT STRAIGHT AT THE APPLICATION'S ISR.
     DOS/4GW owns the IDT and keeps per-vector nesting state; entering the game's handler
     behind its back leaves that state unbalanced the moment the handler chains onward,
     and the extender says so:
         DOS/4GW Professional fatal error (1001): error in interrupt chain
     So the vector table is the delivery target, as it always was. What the AH=25h snoop
     is for is only KNOWING that the application has an ISR at all -- before that, vector
     8 holds an arming-pass placeholder and delivering to it is fatal. The app handler is
     still recorded, because it is what makes that judgement possible and it is the thing
     to print when this goes wrong again. */
#define DPMI_IRQ_TARGET_SEL(iv) (g_pm_int[iv].sel)
#define DPMI_IRQ_TARGET_OFF(iv) (g_pm_int[iv].off)

#define DPMI_PMDEF_STRIDE 3
static WORD  g_pm_defsel  = 0;      /* code selector over the stub block */
static DWORD g_pm_defbase = 0;      /* its linear base                   */
static int   g_pm_defidx  = -1;     /* its LDT slot, so its D/B can follow the client */
/* DPMI PM EXCEPTION-handler table (INT 31h 0202/0203). Separate from g_pm_int on
   purpose: 0202/0203 address CPU exceptions 00h-1Fh, which are a different namespace
   from the interrupt vectors 0204/0205 addresses -- a client may legitimately install
   a #GP (0Dh) exception handler and an INT 0Dh (IRQ5) interrupt handler at once, and
   collapsing them into one table would make each silently overwrite the other.
   Doom's DOS/4GW makes 45 of these calls -- the biggest single block of UNSUP in the
   session-16 trace -- installing its own fault handlers before it runs the game. */
static struct { WORD sel; DWORD off; int set; } g_pm_exc[32];
static int   g_dpmi_vi = 1;                 /* DPMI virtual interrupt flag (INT 31h 0900/0901/0902) */
/* DPMI 0303 real-mode callbacks: each slot records the client's PM handler (sel:off)
   and the RMCS buffer (sel:off) to marshal register state through. g_pmret_sel is a
   code selector based at DOS_HDLR_SEG (0x500) so the PM handler's IRET lands on the
   planted DPMI_PMRET catcher; allocated lazily on the first 0303. */
static struct { WORD pm_sel; DWORD pm_off; WORD rm_es; DWORD rm_di; int used; } g_cb[DPMI_CB_SLOTS];
static BYTE g_bios_unimpl[256];   /* GH #27: BIOS services a run actually wanted */

/* ---- INT 21h AH=4Bh EXEC.  GH #30. ------------------------------------------
 *
 * A parent calls EXEC, a child runs to completion, and the parent carries on at
 * the instruction after its INT 21h with the child's exit code retrievable via
 * AH=4Dh.  That is what turns COMMAND.COM from a prompt into a shell.
 *
 * The work is split: dos_int21 only RECORDS the request, because the loader, the
 * file I/O and the guest's register frame all live out here.
 *
 * HOW THE RETURN WORKS, which is the part worth understanding.  The parent
 * entered through `INT 21h`, so the CPU pushed FLAGS/CS/IP on the parent's stack
 * and we are executing inside our BOP stub.  We snapshot the parent's ENTIRE
 * register frame -- including CS:IP pointing AT the BOP and SS:SP pointing at
 * that IRET frame -- then overwrite the frame with the child's entry state.
 * When the child terminates we put the parent's frame back and step EIP past the
 * BOP, so the stub's IRET pops the parent's own frame and lands exactly where it
 * would have if EXEC had simply returned.  No stack is unwound by hand.
 */
#define EXEC_MAX_DEPTH 8
static struct {
    DWORD eax, ebx, ecx, edx, esi, edi, ebp, esp, eip, efl;
    WORD  cs, ss, ds, es;
    WORD  psp, dta_seg, dta_off;
    WORD  child_seg;                  /* freed when the child terminates */
} g_exec[EXEC_MAX_DEPTH];
static int g_exec_depth;

static BYTE exec_filebuf[0x80000];    /* child image; separate from the parent's */

/* Perform a recorded EXEC: load the child, snapshot the parent, hand over. */
static char *exec_begin(dos_machine_t *m, volatile BYTE *tib, char *p)
{
    HANDLE hf;
    DWORD nread = 0;
    uint16_t child = 0, maxpara = 0, want;
    dos_image_t img;
    volatile WORD *pfl;
    int d = g_exec_depth;

    pfl = (volatile WORD *)(((VDM_REG(tib, VTIB_SS) & 0xFFFF) << 4)
           + (((VDM_REG(tib, VTIB_ESP) & 0xFFFF) + 4) & 0xFFFF));

    p = zput(p, "  EXEC: \""); p = zput(p, m->exec_path); p = zput(p, "\"\r\n");

    if (d >= EXEC_MAX_DEPTH) {
        p = zput(p, "  EXEC: nesting limit reached\r\n");
        VDM_REG(tib, VTIB_EAX) = (VDM_REG(tib, VTIB_EAX) & 0xFFFF0000u) | 8;
        *pfl |= 1; VDM_REG(tib, VTIB_EIP) += 3;
        return p;
    }
    hf = CreateFileA(m->exec_path, GENERIC_READ, FILE_SHARE_READ, NULL,
                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) {
        p = zput(p, "  EXEC: file not found\r\n");
        VDM_REG(tib, VTIB_EAX) = (VDM_REG(tib, VTIB_EAX) & 0xFFFF0000u) | 2;
        *pfl |= 1; VDM_REG(tib, VTIB_EIP) += 3;
        return p;
    }
    ReadFile(hf, exec_filebuf, sizeof(exec_filebuf), &nread, NULL);
    CloseHandle(hf);

    /* Ask for everything: DOS gives a .COM all of free memory, and an .EXE at
       least its minalloc. Probe the largest block by asking for too much. */
    if (dos_alloc(NULL, m->first_mcb, 0xFFFF, &child, &maxpara) == 0) maxpara = 0;
    want = maxpara;
    if (!want || dos_alloc(NULL, m->first_mcb, want, &child, &maxpara) != 0) {
        p = zput(p, "  EXEC: no memory\r\n");
        VDM_REG(tib, VTIB_EAX) = (VDM_REG(tib, VTIB_EAX) & 0xFFFF0000u) | 8;
        *pfl |= 1; VDM_REG(tib, VTIB_EIP) += 3;
        return p;
    }

    /* Snapshot the parent BEFORE anything is overwritten. */
    g_exec[d].eax = VDM_REG(tib, VTIB_EAX); g_exec[d].ebx = VDM_REG(tib, VTIB_EBX);
    g_exec[d].ecx = VDM_REG(tib, VTIB_ECX); g_exec[d].edx = VDM_REG(tib, VTIB_EDX);
    g_exec[d].esi = VDM_REG(tib, VTIB_ESI); g_exec[d].edi = VDM_REG(tib, VTIB_EDI);
    g_exec[d].ebp = VDM_REG(tib, VTIB_EBP); g_exec[d].esp = VDM_REG(tib, VTIB_ESP);
    g_exec[d].eip = VDM_REG(tib, VTIB_EIP); g_exec[d].efl = VDM_REG(tib, VTIB_EFLAGS);
    g_exec[d].cs  = (WORD)VDM_REG(tib, VTIB_CS); g_exec[d].ss = (WORD)VDM_REG(tib, VTIB_SS);
    g_exec[d].ds  = (WORD)VDM_REG(tib, VTIB_DS); g_exec[d].es = (WORD)VDM_REG(tib, VTIB_ES);
    g_exec[d].psp = m->psp_seg;
    g_exec[d].dta_seg = m->dta_seg; g_exec[d].dta_off = m->dta_off;
    g_exec[d].child_seg = child;

    /* Build the child's PSP and copy in its command tail, then load the image. */
    dos_psp_build(NULL, child, m->exec_env ? m->exec_env : DOS_ENV_SEG,
                  (uint16_t)(child + want));
    { volatile BYTE *dpsp = (volatile BYTE *)(child << 4);
      const volatile BYTE *tail = (const volatile BYTE *)
          ((m->exec_tail_seg << 4) + m->exec_tail_off);
      int k, n = tail[0] > 126 ? 126 : tail[0];
      for (k = 0; k <= n; ++k) dpsp[0x80 + k] = tail[k];
      dpsp[0x81 + n] = 0x0D;
      dpsp[0x16] = (BYTE)(m->psp_seg & 0xFF);       /* parent PSP */
      dpsp[0x17] = (BYTE)(m->psp_seg >> 8); }

    img = dos_load(NULL, exec_filebuf, nread, child);

    if (m->exec_mode == 0x01) {
        /* Load without executing: DOS hands the entry point back in the caller's
           parameter block and returns. We have not wired that back, so say so
           rather than pretend the program ran. */
        p = zput(p, "  EXEC: AL=01 load-without-execute UNIMPLEMENTED\r\n");
        m->unimpl21[0x4B >> 3] |= (uint8_t)(1u << (0x4B & 7));
        dos_free(NULL, child);
        VDM_REG(tib, VTIB_EAX) = (VDM_REG(tib, VTIB_EAX) & 0xFFFF0000u) | 1;
        *pfl |= 1; VDM_REG(tib, VTIB_EIP) += 3;
        return p;
    }

    ++g_exec_depth;
    m->psp_seg = child;
    m->dta_seg = child; m->dta_off = 0x0080;        /* DOS resets the DTA to PSP:80 */
    VDM_REG(tib, VTIB_CS)  = img.cs; VDM_REG(tib, VTIB_EIP) = img.ip;
    VDM_REG(tib, VTIB_SS)  = img.ss; VDM_REG(tib, VTIB_ESP) = img.sp;
    VDM_REG(tib, VTIB_DS)  = child;  VDM_REG(tib, VTIB_ES)  = child;
    VDM_REG(tib, VTIB_EAX) = 0;
    p = zput(p, "  EXEC: child at seg=0x"); p = zhex(p, child);
    p = zput(p, " entry="); p = zhex(p, img.cs); p = zput(p, ":"); p = zhex(p, img.ip);
    p = zput(p, img.is_exe ? " (EXE)" : " (COM)");
    p = zput(p, " depth="); p = zhexb(p, (unsigned)g_exec_depth);
    p = zput(p, "\r\n");
    return p;
}
static WORD  g_pmret_sel = 0;
/* GH #18: the PM-fault reflect selectors (0 = not installed). Run 67 corrected model:
   g_dpmi_fault_sel = the handler STACK selector (writable-data) written to [TIB+0x638];
   g_dpmi_flt_code_sel = the handler CODE selector (with a BOP at DPMI_FAULT_COFF) whose
   {sel,off} we plant in the class table; g_flt_tbl = the 8-entry handler table (stride
   0x10) the kernel reads via [VDM_TIB+8], indexed by fault class. */
static WORD  g_dpmi_fault_sel = 0;
static WORD  g_dpmi_flt_code_sel = 0;
static BYTE  g_flt_tbl[8 * 0x10] __attribute__((aligned(16)));
/* DPMI LDT descriptor allocator. Indices 0=null,1=code(0x0F),2=data(0x17) are the
   switch's; DPMI clients allocate from 3+. We keep base/limit/access so INT 31h
   06/07/08/09 can get/modify them and reinstall via svc 10 (NtSetLdtEntries). */
/* ── THE LDT, WITH A REAL FREE LIST. ─────────────────────────────────────────────
   INT 31h 0001 (free descriptor) used to be a no-op that logged " -> free", which is
   fine right up until a client actually recycles descriptors -- and Doom does, heavily:
   360 allocations against 315 frees in a single startup. Leaking every one exhausted
   the table, 0000 started returning ENOMEM, and the client carried on using the garbage
   selector it got back (0x8011, index 4098). A no-op is not a safe stub when the thing
   being stubbed is a RESOURCE.
   The table is also bigger: 512 was an arbitrary bound from the spike era. */
#define DPMI_LDT_MAX 2048
static struct dpmi_desc { DWORD base, limit; BYTE access, flags; } g_ldt[DPMI_LDT_MAX];
static int   g_ldt_next = 3;
static WORD  g_ldt_free[DPMI_LDT_MAX];   /* recycled indices, LIFO */
static int   g_ldt_nfree = 0;
static volatile BYTE *g_tib_dbg = 0;        /* VDM_TIB, for the crash VEH to dump guest state */

/* Serial debug sink (DPMI harness): COM1 is captured by QEMU (-serial file:vm/serial.log)
   so the host reads the log directly -- no GUI screendump, no stale-host ambiguity. */
static HANDLE g_serial = INVALID_HANDLE_VALUE;
static void serial_init(void)
{
    DCB dcb;
    g_serial = CreateFileA("\\\\.\\COM1", GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (g_serial == INVALID_HANDLE_VALUE) return;
    { unsigned i; char *q = (char *)&dcb; for (i = 0; i < sizeof dcb; ++i) q[i] = 0; }
    dcb.DCBlength = sizeof dcb;
    if (GetCommState(g_serial, &dcb)) {
        dcb.BaudRate = 115200; dcb.ByteSize = 8; dcb.Parity = 0; dcb.StopBits = 0;
        SetCommState(g_serial, &dcb);
    }
}
/* Write [buf..end) to COM1 (and it's already in the file log via log_*). */
static void serial_out(const char *buf, const char *end)
{
    DWORD wrote;
    if (g_serial != INVALID_HANDLE_VALUE && end > buf) {
        WriteFile(g_serial, buf, (DWORD)(end - buf), &wrote, NULL);
        FlushFileBuffers(g_serial);
    }
}
static HMENU        g_savedmenu;            /* stashed menu while hidden           */

/* Device IRQs 2-7 (the Sound Blaster's block-completion IRQ 5 above all). IRQ 0
   and 1 keep their existing dedicated paths; everything else latches here and is
   injected as INT (8 + irq), the PC's standard master-PIC vector mapping. Without
   this an SB transfer completes, raises IRQ 5, and the game waits forever for an
   interrupt the host quietly dropped. */
static volatile LONG  g_irqn_pending[8];
static DWORD          g_irq_raised[8];      /* vdd_raise_irq calls, per line */
static DWORD          g_irq_raised_any = 0;
/* Async preemption (session 11). g_hcpu is a handle to the thread that runs the guest
   -- VdmQueueInterrupt's ServiceData -- duplicated once from the exec thread itself.
   g_qi_bits are the [0x714] pending bits to set alongside the queue call, and
   g_qi_raise enables the periodic IRQ 5 the qirq probe listens for; both come from
   QIMODE_PATH so a mode can be retried without a rebuild. */
static HANDLE         g_hcpu          = NULL;
static DWORD          g_exec_prio     = 0;   /* guest thread priority class; see EXECPRIO_PATH */
static DWORD          g_qi_bits       = 0;
static int            g_qi_raise      = 0;
static int            g_qi_vif        = 0;   /* start the guest with EFLAGS.VIF set */
/* ON BY DEFAULT since v132: async injection is what makes a real game playable (a guest
   parked in its own handler never traps, so nothing else can deliver its timer or its
   keystrokes), and it is now gated by a real PIC. qimode can still turn it OFF (bit 6) for
   A/B testing, but nothing should depend on a flag file being present to work. */
static int            g_qi_susp       = 1;   /* async-inject via SuspendThread+SetThreadContext */
static int            g_qi_keys       = 0;   /* synthesise keypresses (repro the hang) */
static int            g_qi_keys_async = 0;   /* opt-in: async-deliver IRQ1 (see host_irq_sink) */
/* Set only while the exec thread is inside VdmStartExecution, i.e. while the thread's
   CONTEXT genuinely is the guest's frame and our loop is not touching the VDM_TIB. The
   async injector refuses to act unless this is set, so it can never race the exec loop. */
static volatile LONG  g_in_exec       = 0;
static DWORD          g_async_inj     = 0;   /* successful async injections */
static DWORD          g_interp_refused = 0;  /* interpreter declined the faulting opcode */
static DWORD          g_async_bail    = 0;   /* attempts declined (guest not in a safe spot) */
static DWORD          g_qi_calls      = 0;
static volatile LONG  g_qi_status     = 0;  /* NTSTATUS of the last queue call */
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
     - never while the exec loop owns the context (g_in_exec), or we would race it.
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
static DWORD          g_async_nest_blocked = 0;   /* refused: line masked or in service */
/* True when a line's vector still points at one of our own do-nothing stubs (the shared
   device IRET at DOS_HDLR_SEG:0x66, or the default INT 09h at 0x4C). Those never send an
   EOI, so anything delivered through them must be auto-EOI'd or the line latches. */
static int async_vec_is_our_stub(unsigned irq);


static void pokew(DWORD lin, WORD v);        /* fwd: guest-memory helpers, defined below */
static WORD peekw(DWORD lin);
static void host_pit_sync(void);             /* fwd: the guest's clock, driven by both threads */

/* ── ASYNCHRONOUS DELIVERY INTO **PROTECTED MODE**. ───────────────────────────────
   The V86 arm below has always bailed when the guest is not in V86, and that hole is
   exactly where a DOS/4GW game lives. It is not a detail: a protected-mode guest that
   is spinning on a memory location NEVER LEAVES PROTECTED MODE, so the cooperative
   injector in the main loop -- which only runs BETWEEN entries -- can never reach it.
   Doom proves it. Its millisecond delay is two instructions:
       153dc:  cmp  [0x28820],eax
       153e2:  je   153dc
   waiting for a counter its own INT 08h handler increments. No I/O, no INT, no HLT, so
   dpmi_enter_pm() never returns and the watchdog eventually calls it a wedge. The only
   way in is the same one this file already uses for real mode: suspend the CPU thread,
   rewrite its context, resume. Defined next to dpmi_inject_pm_irq() because it shares
   that function's frame rules; declared here because async_inject_irq() needs it. */
static int  dpmi_async_inject_pm(unsigned irq, CONTEXT *cx);
/* WHICH gate refused the last async PM injection. Doom's log showed ok=0 on all six
   attempts with `from=0x187:...`, i.e. the thread WAS in 32-bit client PM code, so
   dpmi_async_inject_pm() was reached and returned 0 -- and nothing said which of its
   ten early-outs fired. Session 18 concluded "the async mechanism is what tears the
   VDM down" from a control where async was ON; if it never injects, that attribution
   was to a mechanism that was not running. */
static LONG g_async_why = 0;
/* ── ...AND `g_async_why` ALONE STILL CANNOT ANSWER THE QUESTION THAT MATTERS. ───────
     It holds the LAST refusal, so a run can say "62 attempts, 56 delivered" and not say
     which clause consumed the other six -- nor, far more importantly, what the refusal
     PROFILE looks like when the sync rate itself is the binding constraint. Session 23
     measured that tripling the attempt budget moved delivery by one tick per second:
     the ceiling is not how often we ask, it is how often the guest is in an injectable
     state, and "injectable" is a dozen different conditions wearing one number.
     So keep the whole distribution, per LINE -- the timer and the Sound Blaster fail for
     different reasons and averaging them together hides both. Three buckets need
     OPPOSITE fixes and only a histogram tells them apart:
       10  g_async_pm_active   an injection is still in flight -> the guest's ISR is slow
                               to IRET, and MORE attempts can never help
       21  vdd_pic_can_deliver IRQ0's in-service bit is still set -> we are not seeing the
                               guest's EOI, which would be OUR bug, not a rate one
       7/8 virtual-IF clear    the client has interrupts off -> only the cooperative path
                               can ever deliver
       14  host CS             the CPU thread was inside the HOST, not the client, when
                               the clock asked -- the g_lock starvation showing up here
     Bucket 0 is delivery. Two DWORDs per line per code is 1 KB of BSS, no lock (the
     timer/UI thread is the only writer; a torn count would cost a unit, not a wrong
     conclusion) and no I/O, so it costs nothing at the PIT's rate. */
#define ASYNC_WHY_MAX 32
static DWORD g_async_why_hist[8][ASYNC_WHY_MAX];
static void async_why_note(unsigned irq, unsigned why)
{
    g_async_why = (LONG)why;
    if (why < ASYNC_WHY_MAX) g_async_why_hist[irq & 7][why]++;
}
static volatile LONG g_async_pm_active = 0;  /* an async PM interrupt is in flight     */
static DWORD g_async_pm_eip = 0, g_async_pm_esp = 0, g_async_pm_efl = 0;
static WORD  g_async_pm_cs  = 0, g_async_pm_ss  = 0;
static DWORD g_async_pm_inj = 0;             /* delivered                              */
static DWORD g_async_inj_line[8];            /* ...and which IRQ line each one was      */
static DWORD g_async_pm_bail2 = 0;           /* PM async attempts that did not commit  */
#define DPMI_WATCH_MAX 4
static DWORD g_pm_watch[DPMI_WATCH_MAX];     /* linear addresses to watch (whitespace-separated) */
static int   g_pm_nwatch      = 0;
static DWORD g_pm_irq0_done   = 0;           /* cooperative injections that reached an IRET */
/* ── THE OTHER HALF OF THE DELIVERY ACCOUNT. ─────────────────────────────────────────
     `pit budget` reads "raises 144/s ... delivered 56/s" and concludes the guest's clock
     runs at 39% of the rate it programmed. But `delivered` is g_async_inj_line[0] -- the
     ASYNCHRONOUS arm only -- and the client's INT 08h handler is entered by TWO
     mechanisms: that one, and the cooperative dpmi_inject_pm_irq() the PM loop runs (the
     per-pass latch at #2b, and the catch-up batch on the catcher's return). Putting an
     async-only counter next to `raises` invites reading it as the total, which is a
     units error of exactly the kind that has cost this project rig runs before.
     Count the cooperative arm per VECTOR and print both arms against `raises`, so the
     line answers the question it appears to answer. */
static DWORD g_pm_coop_line[8];
/* ── WHICH INJECTION PATH ACTUALLY PRODUCES A REFILL? ────────────────────────────────
     Measured: DMX polls the 8237's channel-1 CURRENT COUNT ~55 times a second (all
     8-bit reads, so two per poll) while 82 DMA blocks complete -- and 32% of audible
     blocks are lap repeats, which is the same fraction as the blocks that complete
     without DMX ever having looked. 55/s is also, to within 1.5% in two separate runs,
     the ASYNCHRONOUS arm's delivery rate -- while the cooperative arm delivers 79/s
     more on top and the total is 135/s.
     If that is a coincidence, DMX's poll rate is its own and the refill headroom is the
     thing to attack. If it is not, then the two injection paths are NOT equivalent from
     the guest's side -- a cooperative tick enters the same handler and does not produce
     the same work -- and making them equivalent would take refills from 55/s to 135/s.
     Two ratios agreeing is not a mechanism, so measure it directly: count the count-reads
     that happen INSIDE a cooperative INT 08h. The handler runs synchronously within
     dpmi_inject_pm_irq(), so a before/after snapshot of the counter brackets it exactly,
     with no new plumbing into the device model. Near zero here confirms it. */
static uint32_t g_coop_dma_polls;
static uint32_t g_coop_dma_polls_dev[8];   /* ...and the same, per DEVICE line */
/* Count-register reads split by whether an ASYNC injection was in flight. Note the
   pair does NOT have to sum to the device's own rd_count[1]: this sees only reads
   dispatched through host_io_do, and a gap between the two is itself informative. */
static uint32_t g_dmapoll_in_async, g_dmapoll_mainline;
/* Cooperative delivery of DEVICE lines (2-7) to a PM client -- the retry the async
   path never had. `inj` is the interrupts that would previously have been LOST. */
static DWORD g_pm_devirq_inj  = 0;
static DWORD g_pm_devirq_fail = 0;
static DWORD g_pm_devirq_drop = 0;           /* pending on a line the client never hooked */
/* Record and (boundedly) report an async attempt that gave up BEFORE the guest context
   was ever inspected. Bounded for the same reason the PM bail log is: these fire at the
   PIT's rate, so an uncapped line per tick would bury the run it exists to explain. The
   cap is generous enough to span a whole 45s headless run. */
/* Enough to show WHEN the bails start and what the first ones are; the totals live in
   g_async_why_hist, which costs nothing. See async_early_bail() for what 4000 cost. */
#define ASYNC_EARLY_BAIL_LOG_MAX 32
static DWORD g_async_early_bail_logged = 0;
/* ── WHERE WAS THE GUEST WHEN THE CLOCK ASKED FOR A TURN? ────────────────────────────
     The asynchronous injector is the ONLY thing that can touch a protected-mode guest
     inside a BOP-free stretch, and session 20 localised Doom's death to exactly such a
     stretch (R_InitTextureMapping's ~3.5M-instruction loop 2). The commentary above
     async_inject_irq() already names the open question: every LOGGED injection lands at
     0x03ae53dc -- the millisecond-delay spin, the safe case -- and "whether a later one
     lands somewhere else" was never measured.
     It could not be measured with a per-attempt line: the guest's timer runs at 16124 Hz
     (PIT-RELOAD 0x4a, measured), so an uncapped line per attempt is ~700k lines a run and
     a capped one goes quiet long before the interesting part. Session 20 raised the cap
     and still saw nothing near the death, and concluded "zero async attempts" -- but the
     V86 arm of async_inject_irq() returns SILENTLY, so that was an unread instrument, not
     a measurement.
     So dedupe by SITE instead of counting attempts: log each distinct protected-mode
     CS:EIP the injector finds the CPU at, ONCE. Doom's PM code has a handful of such
     sites, so this is tens of lines for a whole run and it CANNOT miss a new one -- a
     tick landing inside loop 2 is a new EIP by construction.
   ► THE OBSERVATION PASS DOES NOT INJECT. Logging while the guest thread is suspended
     is the one thing this file has always refused to do, and resuming-then-logging
     races the very death we are trying to catch. So a new site costs one DROPPED tick:
     resume, log, return 0. The next tick injects. At 16 kHz that is unmeasurable, and
     it means the site line is on disk BEFORE anything is rewritten. */
/* The longest single dpmi_enter_pm() -- i.e. the longest run of guest protected-mode
   code that gave the host no turn at all -- and the record-breakers past the threshold.
   Only NEW maxima log, so a run reports a growth curve of a few dozen lines instead of
   one line per entry. */
#define PM_STRETCH_LOG_US 300u
static DWORD g_pm_stretch_max_us = 0;
static DWORD g_pm_stretch_logged = 0;
#define ASYNC_SITE_MAX 96
static DWORD g_async_site_eip[ASYNC_SITE_MAX];
static WORD  g_async_site_cs[ASYNC_SITE_MAX];
static int   g_async_nsite = 0;
static int   g_async_site_full = 0;
/* 1 = not seen before (and now recorded). Runs on the timer/UI thread only, so the
   table needs no lock: async_inject_irq() bails at why=20 unless the CPU thread is
   inside guest execution, which is precisely when it is not in here. */
static int async_site_new(WORD cs, DWORD eip)
{
    int i;
    for (i = 0; i < g_async_nsite; ++i)
        if (g_async_site_eip[i] == eip && g_async_site_cs[i] == cs) return 0;
    if (g_async_nsite >= ASYNC_SITE_MAX) { g_async_site_full = 1; return 0; }
    g_async_site_cs[g_async_nsite] = cs; g_async_site_eip[g_async_nsite] = eip;
    g_async_nsite++;
    return 1;
}
/* ► TAKES THE LINE NOW. It recorded the reason and threw away WHICH INTERRUPT was
     refused, so the SB's losses and the timer's landed in the same number -- and those
     two have different causes and different fixes (session 23 fixed the SB's by giving
     the device lines a cooperative path; that would have been invisible here). */
static void async_early_bail(unsigned irq, unsigned why)
{
    char pb[96], *pq = pb;
    g_async_bail++;
    async_why_note(irq, why);
    /* ── ⚠⚠ THE CAP WAS 4000 AND IT COST SKYROADS A FIFTH OF ITS TIMER. ──────────────
         This log is reached from host_irq_sink, which runs inside host_pit_sync --
         **while it holds g_lock**. Every line is a log_append (open/write/close) plus a
         serial_out, so a bail that fires at the PIT's rate is FILE I/O UNDER THE DEVICE
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
       ► THE ACCOUNT DOES NOT NEED THE LINES. g_async_why_hist records every bail, per
         line and per code, with no I/O at all -- strictly more information than these
         lines carried. So keep a handful for SHAPE (when in the run they start, what
         the early ones are) and let the histogram carry the totals. That is what the
         histogram was for; this is the first thing it pays for. */
    if (g_async_early_bail_logged++ >= ASYNC_EARLY_BAIL_LOG_MAX) return;
    pq = zput(pq, "ASYNC-EARLY bail irq="); pq = zhexb(pq, irq);
    pq = zput(pq, " why="); pq = zhex(pq, (DWORD)why);
    pq = zput(pq, " ms="); pq = zhex(pq, GetTickCount());
    pq = zput(pq, "\r\n"); log_append(LOG_PATH, pb, pq); serial_out(pb, pq);
}
static int async_inject_irq(unsigned irq)
{
    CONTEXT cx;
    DWORD efl, ss, sp, cs, ip;
    WORD fl;
    int ok = 0;

    /* ► THE EARLY BAILS LOG TOO, BECAUSE THEY ARE THE ONES THAT MATTER. Everything below
         reports itself through g_async_why, but these four returned in silence -- so when
         Doom's R_ExecuteSetViewSize died inside a 3.5M-instruction BOP-free stretch, the
         log showed NO async attempt at all in the window, and "the injector never tried"
         was indistinguishable from "the injector tried and bailed before it could say so".
         That is the difference between a measurement and an unread instrument, so give
         each early exit a why code (20+) and let async_early_bail() say it out loud. */
    if (!g_hcpu || g_in_exec == 0) { async_early_bail(irq, 20); return 0; }
    /* Ask the PIC, exactly as the hardware would: is this line unmasked, and is nothing of
       equal or higher priority still in service? That is what stops us re-entering a handler
       that has not EOI'd yet -- the fault behind "press a key and everything hangs". */
    if (!vdd_pic_can_deliver(&g_pic, (uint8_t)irq)) { g_async_nest_blocked++; async_early_bail(irq, 21); return 0; }
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
    { unsigned v0 = vdd_pic_vector(&g_pic, (uint8_t)irq);
      int rm_hooked = !(peekw(v0 * 4 + 2) == DOS_HDLR_SEG
                        && peekw(v0 * 4) == DOS_IRET_STUB_OFF);
      int pm_hooked = g_dpmi_pm && g_pm_int[0x08u + irq].client;
      if (irq >= 2 && !rm_hooked && !pm_hooked) { async_early_bail(irq, 22); return 0; } }
    if (SuspendThread(g_hcpu) == (DWORD)-1) { async_early_bail(irq, 23); return 0; }
    { unsigned i; char *z = (char *)&cx; for (i = 0; i < sizeof cx; ++i) z[i] = 0; }
    cx.ContextFlags = CONTEXT_CONTROL | CONTEXT_SEGMENTS;
    if (!GetThreadContext(g_hcpu, &cx)) { ResumeThread(g_hcpu); async_early_bail(irq, 24); return 0; }

    efl = cx.EFlags;
    cs  = cx.SegCs & 0xFFFF;
    /* ── THE OBSERVATION PASS. See async_site_new(). Protected mode only: in V86 the
         guest's EIP wanders over the whole real-mode image and would fill the table with
         noise, burying the one site this exists to catch. */
    if (!(efl & EFLAGS_VM_BIT) && (cs & 4) && async_site_new((WORD)cs, cx.Eip)) {
        char sb2[160], *sq = sb2;
        DWORD sblin = dpmi_sel_base((WORD)cs) + cx.Eip;
        ResumeThread(g_hcpu);                    /* NEVER log while the guest is held */
        sq = zput(sq, "ASYNC-SITE #"); sq = zhex(sq, (DWORD)g_async_nsite);
        sq = zput(sq, " irq="); sq = zhex(sq, irq);
        sq = zput(sq, " cs:eip=0x"); sq = zhex(sq, cs);
        sq = zput(sq, ":0x"); sq = zhex(sq, cx.Eip);
        sq = zput(sq, " lin=0x"); sq = zhex(sq, sblin);
        sq = zput(sq, " ss:esp=0x"); sq = zhex(sq, cx.SegSs & 0xFFFF);
        sq = zput(sq, ":0x"); sq = zhex(sq, cx.Esp);
        sq = zput(sq, " efl=0x"); sq = zhex(sq, efl);
        sq = zput(sq, " ms="); sq = zhex(sq, GetTickCount());
        sq = zput(sq, "\r\n"); log_append(LOG_PATH, sb2, sq); serial_out(sb2, sq);
        async_why_note(irq, 27);                 /* accounted for, so the histogram sums */
        return 0;                                /* observed only -- the next tick injects */
    }
    /* ► PROTECTED MODE IS A DIFFERENT FRAME AND A DIFFERENT VECTOR TABLE, so it gets its
         own arm rather than a widened condition. The test for "is this the guest at all"
         is the selector's TABLE INDICATOR: everything we hand the client comes out of our
         LDT (TI=1, bit 2 set), and the host's own flat CS is a GDT selector. So `cs & 4`
         separates "the thread is executing client PM code" from "the thread is in our own
         code between entries" exactly, with nothing to keep in sync. */
    if (!(efl & EFLAGS_VM_BIT)) {
        g_async_why = 0;
        /* ► TWO EXITS USED TO LEAVE why=0, WHICH IS THE CODE FOR SUCCESS. A thread found
             in PM on a GDT selector (we are inside the HOST, not the client) and a failed
             SetThreadContext both returned ok=0 with why untouched, so a histogram keyed
             on it would have booked them as deliveries. Give each its own code: 14 is the
             one to watch, because "the CPU thread was in host code when the clock asked"
             is exactly what g_lock starvation looks like from this side. */
        if (!(cs & 4)) async_why_note(irq, 14);
        else if (dpmi_async_inject_pm(irq, &cx)) {
            cx.ContextFlags = CONTEXT_CONTROL | CONTEXT_SEGMENTS;
            ok = SetThreadContext(g_hcpu, &cx) ? 1 : 0;
            if (ok) { vdd_pic_acknowledge(&g_pic, (uint8_t)irq);
                      if (irq == 0 || async_vec_is_our_stub(irq)) vdd_pic_eoi(&g_pic, (uint8_t)irq); }
            else    { g_async_pm_active = 0; }     /* never leave the flag set on failure */
            async_why_note(irq, ok ? 0u : 13u);
        }
        else async_why_note(irq, (unsigned)g_async_why);   /* the clause that said no */
        ResumeThread(g_hcpu);
        if (ok) { g_async_inj++; g_async_pm_inj++; g_async_inj_line[irq & 7]++;
                  if (!(irq & 7)) tick_delivered_note(); } else g_async_bail++;
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
        if (ok || g_async_pm_bail2 <= 4000) {
            char pb[224], *pq = pb;
            /* ► SAY WHICH LINE. This said "vec=0x08" literally, whatever interrupt it
                 had just delivered, so a run could not be asked "did any keyboard
                 interrupt reach the client?" -- every line claimed to be the timer. */
            pq = zput(pq, "ASYNC-PM vec=0x"); pq = zhexb(pq, 0x08u + irq);
            pq = zput(pq, " ok=0x"); pq = zhex(pq, (DWORD)ok);
            pq = zput(pq, " why="); pq = zhex(pq, (DWORD)g_async_why);
            pq = zput(pq, " from=0x");   pq = zhex(pq, cs);
            pq = zput(pq, ":0x");        pq = zhex(pq, g_async_pm_eip);
            pq = zput(pq, " -> 0x");     pq = zhex(pq, (DWORD)DPMI_IRQ_TARGET_SEL(0x08u + irq));
            pq = zput(pq, ":0x");        pq = zhex(pq, DPMI_IRQ_TARGET_OFF(0x08u + irq));
            pq = zput(pq, " SS:ESP=0x"); pq = zhex(pq, (DWORD)g_async_pm_ss);
            pq = zput(pq, ":0x");        pq = zhex(pq, g_async_pm_esp);
            pq = zput(pq, " efl=0x");    pq = zhex(pq, g_async_pm_efl);
            pq = zput(pq, "\r\n"); log_append(LOG_PATH, pb, pq); serial_out(pb, pq);
            if (!ok) g_async_pm_bail2++;
        }
        return ok;
    }
    /* ► THIS ARM USED TO RETURN IN SILENCE, AND THAT SILENCE WAS READ AS EVIDENCE.
         Session 20 recorded "ZERO async attempts of ANY kind across the death window"
         and struck the injector off the suspect list. But Doom is doing real-mode file
         I/O right up to the death (INT 31h 0302 -> callRM), so the CPU is in V86 for
         most of those attempts and every one of them left here without a word. Give it
         a why code like every other exit; async_early_bail's cap keeps it bounded. */
    if (!(efl & (0x200u | EFLAGS_VIF_BIT)) || cs == DOS_HDLR_SEG) {
        ResumeThread(g_hcpu);
        async_early_bail(irq, (cs == DOS_HDLR_SEG) ? 26 : 25);
        return 0;
    }
    ss = cx.SegSs & 0xFFFF; sp = cx.Esp & 0xFFFF; ip = cx.Eip & 0xFFFF;

    fl = (WORD)efl;
    if (efl & EFLAGS_VIF_BIT) fl |= 0x200;      /* same VIF fold as inject_int */
    sp = (sp - 2) & 0xFFFF; pokew((ss << 4) + sp, fl);
    sp = (sp - 2) & 0xFFFF; pokew((ss << 4) + sp, (WORD)cs);
    sp = (sp - 2) & 0xFFFF; pokew((ss << 4) + sp, (WORD)ip);
    cx.Esp    = sp;
    { unsigned vec = vdd_pic_vector(&g_pic, (uint8_t)irq);
      cx.Eip   = peekw(vec * 4);
      cx.SegCs = peekw(vec * 4 + 2); }
    cx.EFlags = efl & ~(0x300u | EFLAGS_VIF_BIT);
    cx.ContextFlags = CONTEXT_CONTROL | CONTEXT_SEGMENTS;
    ok = SetThreadContext(g_hcpu, &cx) ? 1 : 0;
    if (ok) {
        vdd_pic_acknowledge(&g_pic, (uint8_t)irq);       /* in service until the guest EOIs */
        /* Release it immediately in the two cases where nobody ever will:
           - a line vectored at one of OUR default stubs, which do nothing and never EOI;
           - THE TIMER. Measured on Skyroads: it EOIs only ~36 times a second against a
             180 Hz timer, i.e. only on the ~1-in-16 ticks where its handler chains to the
             BIOS -- so it is relying on something other than a per-tick EOI. Our INT 08h
             stand-in is a BOP with nowhere to put an EOI at the END of the handler, so
             modelling IRQ0's in-service bit strictly starves the guest (measured: 540 -> 15
             delivered ticks per 3 s). IRQ0 is gated by the guest's own IF discipline, which
             has always worked; the re-entrancy this whole mechanism exists to stop was on
             IRQ1, and that stays strict. */
        if (irq == 0 || async_vec_is_our_stub(irq)) vdd_pic_eoi(&g_pic, (uint8_t)irq);
    }
    ResumeThread(g_hcpu);
    if (ok) g_async_inj++; else g_async_bail++;
    async_why_note(irq, ok ? 0u : 13u);     /* the V86 arm's only failure is SetThreadContext */
    /* Log AFTER the resume (never hold the guest suspended across file I/O). The IVT dump
       is the point: vectoring an IRQ the guest never hooked lands it in unowned ROM, which
       is exactly what happened first time out -- Skyroads ended up at F000:A390. Printing
       the whole IRQ3-7 vector range shows which line the game is actually listening on. */
    if (g_async_inj + g_async_bail <= 4) {
        char ab[256], *aq = ab; int v;
        aq = zput(aq, "ASYNC-INJ vec=0x");  aq = zhex(aq, (DWORD)(8 + irq));
        aq = zput(aq, " ok=0x");            aq = zhex(aq, (DWORD)ok);
        aq = zput(aq, " from=0x");          aq = zhex(aq, cs);
        aq = zput(aq, ":0x");               aq = zhex(aq, ip);
        aq = zput(aq, " ivt[0B..0F]=");
        for (v = 0x0B; v <= 0x0F; ++v) {
            aq = zput(aq, "0x");  aq = zhex(aq, peekw(v * 4 + 2));
            aq = zput(aq, ":0x"); aq = zhex(aq, peekw(v * 4));
            aq = zput(aq, " ");
        }
        aq = zput(aq, "\r\n");
        log_append(LOG_PATH, ab, aq); serial_out(ab, aq);
    }
    return ok;
}

static int async_vec_is_our_stub(unsigned irq)
{
    unsigned vec = vdd_pic_vector(&g_pic, (uint8_t)irq);
    WORD seg = peekw(vec * 4 + 2), off = peekw(vec * 4);
    return seg == DOS_HDLR_SEG && (off == DOS_IRET_STUB_OFF || off == 0x004C);
}

static void host_irq_sink(void *ctx, uint8_t irq)
{
    (void)ctx;
    /* Every raise, counted by line. sb_blocks reached 1 while irqn_inj AND irqn_refused
       both stayed 0 -- i.e. the SB's completion IRQ was raised but nothing was ever
       latched -- so the line number this arrives on is the missing fact. */
    g_irq_raised[irq & 7]++;
    g_irq_raised_any++;
    vdd_pic_raise(&g_pic, irq);
    if (irq == 0) {
        irq0_latch();
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
             vdd_pit_add_clocks() raises IRQ0 once per reload period for the whole
             elapsed gap, synchronously, from inside host_pit_sync's lock. Doom's music
             driver programs the 8254 fast (reload 0x4a = 16 kHz, measured), so a 50 ms
             gap is EIGHT HUNDRED raises -- and this used to answer every one of them
             with a SuspendThread / GetThreadContext / SetThreadContext / ResumeThread
             round trip plus a log write, all still holding the device lock.
             Measured on a ten-minute play session: a 44 ms lock hold attributed to
             host_pit_sync, with the AUDIO thread blocked 36.8 ms behind it waiting to
             mix. That is the user's "chk-a-chk-a" in one number, and it is ours.
             Only ONE of those attempts can ever deliver anything -- the rest bail on
             g_async_pm_active because an injection is already in flight -- and they pay
             the full round trip to find out. The backlog is not lost by skipping them:
             g_pm_tick_owed counts every raise and the catch-up batch drains it.
             After: hold 44ms -> 15.9ms, audio-thread wait 36.8ms -> 5.8ms, longest UI
             gap 61ms -> 31.8ms, with the delivered tick rate unchanged. */
        /* ── ...BUT ONE ATTEMPT PER SYNC IS A CEILING, AND WE ARE UNDER IT. ──────────
             The cap above is right about the COST and wrong about the BUDGET. Measured:
                 raises 144/s   syncs 65/s   attempts 62/s   delivered 56/s
                 owed_max = 64 = PM_TICK_OWED_MAX, i.e. the backlog is SATURATED
             The 8254 generates every one of Doom's 140 ticks; they die at host_pit_sync,
             which runs 65 times a second rather than the UI thread's intended 200 because
             it takes g_lock and Doom's mode-Y drawing does ~43,000 port writes a second
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
             extra attempts merely pay full SuspendThread round trips under g_lock to be
             told no -- which is session 22's lesson arriving from a new direction, and a
             10x worse UI gap for nothing.
           ▶ WHAT THIS RULES OUT, AND WHERE TO GO. Do not spend effort on the delivery
             RATE; spend it on the guest's INJECTABILITY, or bypass the async path for
             the timer entirely. `async_inject_irq` refuses on g_async_pm_active (an
             injection still in flight), on vdd_pic_can_deliver, and on the client's
             virtual-IF -- instrument WHICH of those says no at 144 Hz before changing
             anything else. Note that the injectable window may simply be scarce while
             Doom holds its own ISR, in which case the answer is the cooperative PM-loop
             path (which needs no suspend at all), not the asynchronous one. */
        /* ── ⚠ ONE ATTEMPT PER SYNC IS RIGHT FOR A PM CLIENT AND WRONG FOR A V86 GUEST.
             The throttle above was introduced for DOOM, whose music driver programs the
             8254 at 16 kHz: vdd_pit_add_clocks then raises 800 times for a single 50 ms
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
        /* ── ⚠⚠ DO NOT MOVE THIS ATTEMPT OUTSIDE THE LOCK. TRIED, MEASURED, REVERTED.
             It looks wrong to hold g_lock across a SuspendThread / GetThreadContext /
             SetThreadContext / ResumeThread round trip -- SuspendThread does not return
             until the target reaches a safe point, and honouring GR4 in the video path
             pushed the hold attributed to host_pit_sync from 14.6 ms to 45.7 ms against
             DMX's 7.4 ms tick period. Deferring the syscall until after HOST_UNLOCK is
             the obvious repair, and it makes things WORSE:
                 longest hold   45.7 ms -> 149 ms   (and it moves to host_audio_fill)
                 longest wait   41.5 ms -> 149 ms
                 tick delivery  135/s   -> 136/s    REPLAYED_LOUD  30% -> 30%
           ► WHY, AND IT IS THE WHOLE POINT: holding the lock is what GUARANTEES THE
             THREAD WE ARE ABOUT TO SUSPEND IS NOT HOLDING IT. Suspend the exec thread
             from outside and it can be frozen mid-critical-section, so g_lock stays
             taken for the entire suspend and every other thread piles up behind it -- a
             bounded hold traded for an unbounded one. The lock is not merely protecting
             device state here; it is an interlock against suspending a lock holder.
             If this is ever revisited, the prerequisite is a separate suspend-safe
             handshake (the exec thread marking itself un-suspendable while it holds
             g_lock), not simply moving the call. */
        if (g_qi_susp && (!g_dpmi_pm || !g_async_tried_this_sync)) {
            g_async_tried_this_sync = 1;
            ++g_pit_async_attempts;
            if (async_inject_irq(0)) {
                InterlockedDecrement(&g_irq0_pending);
                pm_tick_take();
            }
        }
    }
    else if (irq == 1) {
        /* One pending interrupt at a time: the 8042 has a single output buffer, and the
           input VDD now re-raises as the guest drains it, so this can never legitimately
           run ahead. (The earlier cap-with-a-backlog is what stranded break codes and
           killed the arrow keys -- see vdd_input_push_scancode.) */
        if (g_irq1_pending < 1) InterlockedIncrement(&g_irq1_pending);
        /* Keys deliberately do NOT take the async path by default (qimode bit 7 turns it
           on for experiments). Async keyboard delivery is what turned "playable" into
           "dies as soon as you press a key", and while the PIC stopped the re-entry it did
           not stop that: with a faithful held-arrow probe the game still degrades to a
           stop. Until that is understood, keys go back to the exec-loop path they used
           when input merely felt laggy -- a known-good behaviour beats an unexplained one.
           The TIMER keeps async delivery, which is what makes the game playable at all.
           ► ...BUT A PROTECTED-MODE CLIENT HAS NO OTHER PATH. That reasoning is about the
             V86 exec loop, which drains g_irq1_pending when the guest traps. There is no
             equivalent for a DPMI client: the only cooperative injection the PM loop does
             is IRQ0, so a key raised while Doom is inside its own game loop is simply
             never delivered and the guest never sees a keystroke at all. When the client
             has installed a protected-mode INT 09h handler, asynchronous delivery is not
             a preference, it is the mechanism. */
        { int gate = (g_qi_keys_async || (g_dpmi_pm && g_pm_int[0x09].client));
          int ok   = gate ? async_inject_irq(1) : 0;
          if (ok) InterlockedDecrement(&g_irq1_pending);
          /* ► EVERY KEYBOARD INTERRUPT, ACCOUNTED FOR, FOR THE FIRST FEW DOZEN. A key
               press is a rare, deliberate event -- there is no firehose to guard against
               -- and "the guest never saw my keystroke" has at least four different
               causes between here and the client's ISR. Naming the gate values at the
               moment of the raise turns that into one line. */
          if (g_keyirq_logged++ < 64) {
              char kb[160], *kq = kb;
              kq = zput(kq, "KEYIRQ raise gate="); kq = zhex(kq, (DWORD)gate);
              kq = zput(kq, " ok=");        kq = zhex(kq, (DWORD)ok);
              kq = zput(kq, " pm=");        kq = zhex(kq, (DWORD)g_dpmi_pm);
              kq = zput(kq, " pmhook=");    kq = zhex(kq, (DWORD)g_pm_int[0x09].client);
              kq = zput(kq, " in_exec=");   kq = zhex(kq, (DWORD)g_in_exec);
              kq = zput(kq, " why=");       kq = zhex(kq, (DWORD)g_async_why);
              kq = zput(kq, " ms=");        kq = zhex(kq, GetTickCount());
              kq = zput(kq, "\r\n"); log_append(LOG_PATH, kb, kq); serial_out(kb, kq);
          } }
    }
    else if (irq < 8) {
        InterlockedExchange(&g_irqn_pending[irq], 1);
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
        if (g_qi_bits) {
            /* The kernel dispatches from its virtual PIC, so request the line there
               first -- otherwise the APC wakes up, finds nothing requested, and the
               pending bit just sits there (which is the whole of session 10's
               "already tried and failed"). */
            if (g_qi_bits & 1) v86_ica_raise(irq);
            *(volatile DWORD *)(ULONG_PTR)0x714 |= g_qi_bits;
        }
        if (g_qi_bits && g_hcpu) {
            LONG st = v86_vdmcontrol(VDM_SVC_VdmQueueInterrupt, (PVOID)g_hcpu);
            InterlockedExchange(&g_qi_status, st);
            g_qi_calls++;
        }
        if (g_qi_susp && async_inject_irq(irq))
            InterlockedExchange(&g_irqn_pending[irq], 0);  /* delivered; don't double-inject */
    }
}


/* Real/synthesised real-mode interrupt dispatch. The guest runs cooperatively
   (we only regain control at event boundaries), so when a hardware IRQ becomes
   pending and the guest's IF is set, we do here exactly what the CPU does on a
   hardware interrupt: push FLAGS/CS/IP, clear IF+TF, and vector CS:IP through the
   real-mode IVT. The guest's handler IRETs back normally. (`CD nn` software ints
   still vector natively via VME; this is only for asynchronous IRQ delivery.) */
static void pokew(DWORD lin, WORD v)
{ volatile BYTE *m = (volatile BYTE *)0; m[lin] = (BYTE)v; m[lin + 1] = (BYTE)(v >> 8); }
static void poked(DWORD lin, DWORD v)   /* dword store: 32-bit IRET frame slots (GH #18 run 83) */
{ volatile BYTE *m = (volatile BYTE *)0;
  m[lin] = (BYTE)v; m[lin+1] = (BYTE)(v >> 8); m[lin+2] = (BYTE)(v >> 16); m[lin+3] = (BYTE)(v >> 24); }
static WORD peekw(DWORD lin)
{ const volatile BYTE *m = (const volatile BYTE *)0; return (WORD)(m[lin] | (m[lin + 1] << 8)); }
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
static int if_or_vif(DWORD fl) { return (fl & (0x200u | EFLAGS_VIF_BIT)) != 0; }
static int guest_if_enabled(volatile BYTE *tib)
{
    DWORD cs = VDM_REG(tib, VTIB_CS) & 0xFFFF;
    if (cs == DOS_HDLR_SEG) {
        DWORD ss = VDM_REG(tib, VTIB_SS) & 0xFFFF, sp = VDM_REG(tib, VTIB_ESP) & 0xFFFF;
        return (peekw((ss << 4) + ((sp + 4) & 0xFFFF)) & 0x200) != 0;
    }
    return if_or_vif(VDM_REG(tib, VTIB_EFLAGS));
}

/* Width-selected guest memory access (1/2/4 bytes) for the string-I/O servicer. */
static DWORD peekw_n(DWORD lin, int width)
{ const volatile BYTE *m = (const volatile BYTE *)0;
  if (width == 1) return m[lin];
  if (width == 2) return peekw(lin);
  return (DWORD)peekw(lin) | ((DWORD)peekw(lin + 2) << 16); }
static void pokew_n(DWORD lin, DWORD v, int width)
{ volatile BYTE *m = (volatile BYTE *)0;
  if (width == 1) { m[lin] = (BYTE)v; return; }
  if (width == 2) { pokew(lin, (WORD)v); return; }
  poked(lin, v); }

/* GH #18 / sound epic: sample the kernel's FIXED_NTVDMSTATE word next to the
   reflected EFLAGS at a labelled point, for the first few occurrences of each
   label. iobench proved IRQ0 delivery is gated off forever during port I/O
   (irq0_inj=0 over 34M I/O events, BIOS tick frozen), which can only mean the
   reflected EFLAGS reports IF=0. The guest's REAL interrupt-enable state is the
   one the kernel virtualises in [0x714]; this sampler is how we identify which
   bit carries it -- compare a known-IF=1 moment (a BOP, where the guest's own
   FLAGS are on its stack) against an I/O reflect in the same run. */
static void vdmstate_sample(const char *label, volatile BYTE *tib, int *budget)
{
    char b[128], *q = b;
    if (*budget <= 0) return;
    (*budget)--;
    q = zput(q, "GH#18 vdmstate ");
    q = zput(q, label);
    q = zput(q, ": [0x714]=0x"); q = zhex(q, *(volatile DWORD *)(ULONG_PTR)0x714);
    q = zput(q, " EFL=0x");      q = zhex(q, VDM_REG(tib, VTIB_EFLAGS));
    q = zput(q, " CS:IP=0x");    q = zhex(q, VDM_REG(tib, VTIB_CS) & 0xFFFF);
    q = zput(q, ":0x");          q = zhex(q, VDM_REG(tib, VTIB_EIP) & 0xFFFF);
    q = zput(q, "\r\n");
    log_append(LOG_PATH, b, q); serial_out(b, q);
}

static void inject_int(volatile BYTE *tib, unsigned vec)
{
    WORD ss = (WORD)VDM_REG(tib, VTIB_SS),  sp = (WORD)VDM_REG(tib, VTIB_ESP);
    WORD cs = (WORD)VDM_REG(tib, VTIB_CS),  ip = (WORD)VDM_REG(tib, VTIB_EIP);
    DWORD efl = VDM_REG(tib, VTIB_EFLAGS);
    WORD fl = (WORD)efl;                           /* push the live frame's FLAGS */
    /* ★ Fold VIF into the pushed IF. On VME hardware the guest's REAL interrupt-enable
       lives in EFLAGS.VIF (bit 19), because that is what its own STI sets -- but an IRET
       frame is 16 bits wide, so a straight truncation drops VIF and pushes IF=0. The
       guest's IRET then restores its virtual interrupt state from that zero and comes back
       with interrupts off FOREVER. Measured exactly that: after the very first injected
       INT 08h, irq0_inj stuck at 1 for the rest of the run and every later IRQ was gated
       off (Skyroads: irq0_skip=123k, and the SB completion IRQ could never be taken). The
       CPU does this fold itself for a hardware-vectored interrupt; we synthesise the frame,
       so we must do it too. */
    if (efl & EFLAGS_VIF_BIT) fl |= 0x200;
    sp -= 2; pokew(((DWORD)ss << 4) + sp, fl);     /* push FLAGS */
    sp -= 2; pokew(((DWORD)ss << 4) + sp, cs);     /* push CS    */
    sp -= 2; pokew(((DWORD)ss << 4) + sp, ip);     /* push IP    */
    VDM_SET16(tib, VTIB_ESP, sp);
    /* Clear IF + TF, and VIF with them: vectoring an interrupt disables the guest's
       interrupts, and under VME "the guest's interrupts" means VIF. */
    VDM_REG(tib, VTIB_EFLAGS) &= ~(0x300u | EFLAGS_VIF_BIT);
    VDM_SET16(tib, VTIB_EIP, peekw(vec * 4));      /* IVT[vec].offset */
    VDM_SET16(tib, VTIB_CS,  peekw(vec * 4 + 2));  /* IVT[vec].segment */
}

/* DOS console output (INT 21h AH=02/09/40) -> the video VDD teletype. */
static void host_conout(void *ctx, uint8_t ch)
{
    (void)ctx;
    HOST_LOCK();
    vdd_video_putc(&g_vid, ch);
    HOST_UNLOCK();
}

/* DOS console input (INT 21h AH=01/07/08/0A) -> the keyboard VDD ring, blocking
   on the V86 thread until the UI thread pushes a key (or the window closes). */
/* HOW DOS HANDS OVER AN ARROW. A BIOS keycode is a PAIR (AH=scancode, AL=ascii), but every
   INT 21h console read returns ONE byte. For the keys with no ascii -- arrows, F-keys, the
   nav cluster, i.e. AL=0 -- DOS returns 0x00 first and the SCANCODE on the NEXT call, and a
   program reading arrows is written to expect exactly that. We returned `k & 0xFF` and threw
   the scancode away, so an arrow arrived as a lone NUL that never had a second half: every
   extended key was unreadable through DOS. That is the Skyroads menu, which sits in INT 21h
   (measured: the guest parks at DOS_HDLR_SEG:0000, the INT 21h BOP, for the whole run).
   g_conin_pending holds that second byte between the two calls. */
static int g_conin_pending = -1;                /* scancode owed to the next read, or -1 */

static int host_conin(void *ctx)
{
    uint16_t k; int got;
    (void)ctx;
    if (g_conin_pending >= 0) { int c = g_conin_pending; g_conin_pending = -1; return c; }
    for (;;) {
        HOST_LOCK();
        got = vdd_input_pop(&g_in, &k);
        HOST_UNLOCK();
        if (got) {
            if ((k & 0xFF) == 0) { g_conin_pending = (k >> 8) & 0xFF; return 0x00; }
            return k & 0xFF;
        }
        if (!g_running) return 0x1B;            /* window gone -> unblock as ESC   */
        WaitForSingleObject(g_key_event, 50);
    }
}

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
   Same source as host_pit_sync(): QueryPerformanceCounter, which is why the guest's
   retrace and its PIT cannot drift against each other. */
static uint64_t host_time_us(void)
{
    static LARGE_INTEGER s_freq, s_base;
    LARGE_INTEGER now;
    if (!s_freq.QuadPart) {
        if (!QueryPerformanceFrequency(&s_freq) || !s_freq.QuadPart) return 0;
        QueryPerformanceCounter(&s_base);
    }
    QueryPerformanceCounter(&now);
    return (uint64_t)(((now.QuadPart - s_base.QuadPart) * 1000000) / s_freq.QuadPart);
}

/* The trace hook handed to the OPL VDD. Timestamped from the same clock the CRT
   and PIT use, so a replay reproduces the guest's real WRITE TIMING -- which is
   most of what makes music sound like itself. */
static void opl_trace_write(BYTE reg, BYTE val)
{
    if (g_opltrace_n >= OPLTRACE_MAX) { g_opltrace_drop++; return; }
    g_opltrace[g_opltrace_n].us  = (DWORD)host_time_us();
    g_opltrace[g_opltrace_n].reg = reg;
    g_opltrace[g_opltrace_n].val = val;
    g_opltrace_n++;
}

/* Write the trace out as text: one `us reg val` triple per line, hex. Text so it
   is diffable and survives the SMB round trip; a long run is well under a MB. */
static void opl_trace_dump(void)
{
    HANDLE h; DWORD i, wr;
    static char buf[64];
    if (!g_opltrace_on || !g_opltrace_n) return;
    h = CreateFileA(OPLTRACE_PATH, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    { char *p = buf;
      p = zput(p, "# opl2 register trace: us reg val (hex). writes=");
      p = zhex(p, g_opltrace_n); p = zput(p, " dropped="); p = zhex(p, g_opltrace_drop);
      p = zput(p, "\r\n");
      WriteFile(h, buf, (DWORD)(p - buf), &wr, NULL); }
    for (i = 0; i < g_opltrace_n; ++i) {
        char *p = buf;
        p = zhex(p, g_opltrace[i].us); p = zput(p, " ");
        p = zhexb(p, g_opltrace[i].reg); p = zput(p, " ");
        p = zhexb(p, g_opltrace[i].val); p = zput(p, "\r\n");
        WriteFile(h, buf, (DWORD)(p - buf), &wr, NULL);
    }
    CloseHandle(h);
}

static void opl_pump_time(void)
{
    static LARGE_INTEGER s_freq, s_last;
    LARGE_INTEGER now;
    LONGLONG d;
    DWORD us;
    if (!s_freq.QuadPart) {
        if (!QueryPerformanceFrequency(&s_freq)) return;
        QueryPerformanceCounter(&s_last);
        return;
    }
    QueryPerformanceCounter(&now);
    d = now.QuadPart - s_last.QuadPart;
    if (d <= 0) return;
    if (d > s_freq.QuadPart) d = s_freq.QuadPart;       /* clamp a long stall to 1s */
    us = (DWORD)((d * 1000000) / s_freq.QuadPart);
    if (us < 20) return;                                /* carry sub-quantum time   */
    s_last = now;
    HOST_LOCK();
    vdd_opl_add_us(&g_opl, us);
    HOST_UNLOCK();
}

/* The audio thread's fill callback. Mixing touches the DMA controller, guest
   memory and the IRQ path, so it takes the same lock the exec thread uses. */
/* ── WATCH DMX'S TASK TABLE FROM OUTSIDE. ────────────────────────────────────────
     The SB interrupt only ARMS the mixer (sets next_due = now, DOOM.EXE 0x571b4);
     the TIMER services it, and the scheduler's first act on a busy task is
     `jne 0x572ed` -- the loop EXIT, not the next task -- so ONE busy task abandons
     the whole pass and there are up to 12. Doom runs a MIDI task alongside the PCM
     mixer, so a task that overruns can starve the refill wholesale.
     Addresses are settled and self-checked: the IRQ table was FOUND at 0x03bc81ac and
     the code says it lives at virtual 0x281ac, so data guest = virtual + 0x03BA0000.
     That puts the task table at 0x03bc86a0 and the tick clock at 0x03bc8820 -- and it
     predicts the mixer task at index 4 (0x03bc8720), which is exactly where the
     earlier structure search found it. `mixer_ok` re-checks that in-run; if it is 0
     every number here is meaningless.
     Sampled from the audio fill, which runs ~86 times a second -- one sample per
     block, i.e. exactly the rate the refill is supposed to happen at. */
#define DMX_TASKS   0x03bc86a0u
#define DMX_CLOCK   0x03bc8820u
#define DMX_MIXER_I 4u
static DWORD g_dmx_samples, g_dmx_busy[12], g_dmx_mixer_ok;
static DWORD g_dmx_overdue, g_dmx_overdue_max, g_dmx_anybusy;
static void dmx_sample(void)
{
    static int ok = 0;
    const volatile BYTE *tt = (const volatile BYTE *)(ULONG_PTR)DMX_TASKS;
    const volatile DWORD *clk = (const volatile DWORD *)(ULONG_PTR)DMX_CLOCK;
    unsigned t; int anybusy = 0;
    /* ⚠ RE-PROBE UNTIL IT APPEARS. Probing once latched a failure: this runs from the
         audio thread, which starts long before the guest has allocated the zone this
         table lives in, so the first call always sees unmapped memory and a one-shot
         probe would report `ok=0` for the whole run -- which is exactly what it did. */
    if (!ok) {
        MEMORY_BASIC_INFORMATION mb;
        if (!(VirtualQuery((LPCVOID)tt, &mb, sizeof mb) == sizeof mb
              && mb.State == MEM_COMMIT && !(mb.Protect & (PAGE_NOACCESS | PAGE_GUARD))))
            return;
        ok = 1;
    }
    /* the mixer must be where the addressing predicts, or none of this means anything */
    if (*(const volatile DWORD *)(tt + DMX_MIXER_I * 32u) == 0x56884u + 0x03AEDFECu)
        g_dmx_mixer_ok = 1;
    else return;
    ++g_dmx_samples;
    for (t = 0; t < 12; ++t) {
        if (tt[t * 32u + 0x1c]) { g_dmx_busy[t]++; anybusy = 1; }
    }
    if (anybusy) ++g_dmx_anybusy;
    { DWORD due = *(const volatile DWORD *)(tt + DMX_MIXER_I * 32u + 0x14), now = *clk;
      if ((LONG)(now - due) >= 0) {            /* armed and still not serviced */
          DWORD late = now - due;
          ++g_dmx_overdue;
          if (late > g_dmx_overdue_max) g_dmx_overdue_max = late;
      } }
}

/* ── PACE THE PIT. ──────────────────────────────────────────────────────────────────
     host_pit_sync() advances the emulated 8254 by however much wall-clock has elapsed
     since the last call, raising one IRQ0 per reload period -- so its CALL RATE sets
     how evenly the guest's ticks land. It is driven by the UI thread and by guest I/O
     traps, and measured it runs 65 times a second against a 140 Hz timer: each call
     therefore raises about two ticks, which go out back-to-back.
     Measured interval between DELIVERED IRQ0s (n=6069, 135/s, so 7.4 ms if even):
         <0.5ms  52.7%     8-16ms  22.9%     16-32ms  18.6%     max 48 ms
     53% arrive in BURSTS and 28% of gaps exceed 11.6 ms -- one DMA block. That is the
     whole audio defect: DMX's mixer is armed by the SB block IRQ (next_due = NOW) and
     serviced on the next timer tick, so a gap longer than a block lets a second block
     arm before the first is serviced. The two arms COLLAPSE into one refill and a block
     is never filled -- 32.8% measured, against 28% of gaps being over-length.
   ► So call it far more often. At ~1 kHz each sync raises at most one tick and the
     ticks come out evenly, WITHOUT changing the rate: the 8254 still advances by real
     elapsed time and the guest still gets the 140 Hz it programmed. This is a pacing
     change, not a rate change -- which matters, because session 22 proved that
     delivering MORE ticks per opportunity (DPMI_IRQ0_BATCH) compresses game time and
     is catastrophic.
   ⚠ 1 ms Sleep needs the multimedia timer resolution raised; without timeBeginPeriod
     XP's default granularity is ~15.6 ms and this thread would run slower than the UI
     one it is meant to replace. Loaded dynamically, as audio_wave.c already does for
     waveOut, so the import allowlist is unaffected.
   ⚠ It takes g_lock like every other caller, so it is a knob (pitpace.txt = 0 to
     disable) and the lock figures must be read on the first run with it on. */
typedef MMRESULT (WINAPI *PFN_timeBeginPeriod)(UINT);
static HANDLE g_pitpace_thread;
static int    g_pitpace_on = 1, g_pitpace_ms = 1;
static DWORD  g_pitpace_calls;
static DWORD WINAPI pit_pacer_thread(LPVOID param)
{
    (void)param;
    /* Above the guest but below the audio pump, so it can never starve either. */
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    while (g_running) {
        host_pit_sync();
        ++g_pitpace_calls;
        Sleep((DWORD)g_pitpace_ms);
    }
    return 0;
}

static void host_audio_fill(void *ctx, int16_t *out, uint32_t frames)
{
    (void)ctx;
    dmx_sample();
    HOST_LOCK();
    vdd_audio_mix(&g_audio, out, frames);
    HOST_UNLOCK();
}

/* MPU-401 output -> the host's MIDI synth (XP ships a GS Wavetable device). */
static void host_midi_sink(void *ctx, uint32_t msg)
{
    (void)ctx;
    audio_wave_midi(&g_wave, msg);
}

/* Async-preemption probe driver (session 11, QIMODE_PATH bit 2). Raises IRQ 5 from a
   thread that is NOT the exec thread -- exactly how the audio thread raises the Sound
   Blaster's completion IRQ -- while the guest (qirq.com) spins in pure V86 code that
   never traps. After each raise it watches [0x714] for up to 50 ms: the kernel clears
   VDM_INT_HARDWARE when its APC actually dispatches, so a transition here is proof the
   APC ran even if the guest never sees a vector. Logs the first few raises with the
   queue call's NTSTATUS, which is the whole experimental record. */
/* SYNTHETIC KEYPRESSES (qimode bit 5). The "press a key and it hangs" regression cannot be
   reproduced from here -- the rig has no remote input -- so drive the exact same path the UI
   thread uses for a real key: push a make code into the 0x60 FIFO, raise IRQ1, then the break
   code, repeatedly. If the in-service interlock is wrong this will hang the guest just as a
   human would, and if it is right the run completes with the key counts advancing. */
/* Capture > Take Screenshot (and Ctrl+F5). Puts the current frame on the clipboard as a
   CF_DIB so it can be pasted straight into Paint, AND writes the same image as a .bmp next
   to the test results on the share -- the second half means a screenshot can be looked at
   from the build machine without anyone having to move a file around. 8bpp frames carry
   their palette in the DIB colour table, which is what makes a mode-13h capture come out
   with the right colours rather than a grey mush. */
static void host_screenshot(void)
{
    static int seq = 0;
    BITMAPINFOHEADER *bih;
    HGLOBAL hmem;
    DWORD w, h, stride, pal_n, img_sz, dib_sz;
    BYTE *dib, *bits;
    const uint8_t *src;

    HOST_LOCK();
    w = g_vid.frame.w; h = g_vid.frame.h;
    src = g_vid.frame.pixels;
    if (!src || !w || !h || g_vid.frame.bpp != 8) { HOST_UNLOCK(); return; }
    stride = (w + 3) & ~3u;                  /* DIB rows are 4-byte aligned          */
    pal_n  = 256;
    img_sz = stride * h;
    dib_sz = sizeof(BITMAPINFOHEADER) + pal_n * 4 + img_sz;
    hmem = GlobalAlloc(GMEM_MOVEABLE, dib_sz);
    if (!hmem) { HOST_UNLOCK(); return; }
    dib = (BYTE *)GlobalLock(hmem);
    { unsigned i; for (i = 0; i < dib_sz; ++i) dib[i] = 0; }
    bih = (BITMAPINFOHEADER *)dib;
    bih->biSize = sizeof(BITMAPINFOHEADER);
    bih->biWidth = (LONG)w; bih->biHeight = (LONG)h;   /* positive => bottom-up      */
    bih->biPlanes = 1; bih->biBitCount = 8; bih->biCompression = 0;
    bih->biSizeImage = img_sz; bih->biClrUsed = pal_n; bih->biClrImportant = pal_n;
    { DWORD i; BYTE *pal = dib + sizeof(BITMAPINFOHEADER);
      for (i = 0; i < pal_n; ++i) {
          uint32_t c = g_vid.frame.palette ? g_vid.frame.palette[i] : 0;
          pal[i*4+0] = (BYTE)(c & 0xFF);          /* B */
          pal[i*4+1] = (BYTE)((c >> 8) & 0xFF);   /* G */
          pal[i*4+2] = (BYTE)((c >> 16) & 0xFF);  /* R */
          pal[i*4+3] = 0;
      } }
    bits = dib + sizeof(BITMAPINFOHEADER) + pal_n * 4;
    { DWORD y, x;
      for (y = 0; y < h; ++y) {                    /* flip: DIB row 0 is the bottom   */
          const uint8_t *sr = src + (size_t)(h - 1 - y) * g_vid.frame.stride;
          BYTE *dr = bits + (size_t)y * stride;
          for (x = 0; x < w; ++x) dr[x] = sr[x];
      } }
    HOST_UNLOCK();

    if (OpenClipboard(g_hwnd)) {                   /* paste-into-Paint path           */
        EmptyClipboard();
        if (!SetClipboardData(CF_DIB, hmem)) GlobalFree(hmem);   /* else clipboard owns it */
        CloseClipboard();
    } else GlobalFree(hmem);

    /* ...and the same image on the share, so it can be read from the build machine. */
    {
        char path[160];
        const char *dir = "C:\\Documents and Settings\\All Users\\Documents\\ntvdmex\\";
        int i = 0, j;
        while (dir[i]) { path[i] = dir[i]; ++i; }
        { const char *nm = "shot_manual_00.bmp";
          for (j = 0; nm[j]; ++j) path[i + j] = nm[j];
          path[i + 12] = (char)('0' + (seq / 10) % 10);
          path[i + 13] = (char)('0' + seq % 10);
          path[i + j] = 0; }
        ++seq;
        { HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
          if (f != INVALID_HANDLE_VALUE) {
              BYTE fh[14]; DWORD wr; DWORD off = 14 + sizeof(BITMAPINFOHEADER) + pal_n * 4;
              DWORD tot = 14 + dib_sz;
              fh[0]='B'; fh[1]='M';
              fh[2]=(BYTE)tot; fh[3]=(BYTE)(tot>>8); fh[4]=(BYTE)(tot>>16); fh[5]=(BYTE)(tot>>24);
              fh[6]=fh[7]=fh[8]=fh[9]=0;
              fh[10]=(BYTE)off; fh[11]=(BYTE)(off>>8); fh[12]=(BYTE)(off>>16); fh[13]=(BYTE)(off>>24);
              WriteFile(f, fh, 14, &wr, NULL);
              WriteFile(f, dib, dib_sz, &wr, NULL);
              CloseHandle(f);
          } }
    }
    if (GlobalLock(hmem)) GlobalUnlock(hmem);
}

/* ONE path for a keystroke, whoever produced it. The window proc used to latch
   g_irq1_pending itself and never call host_irq_sink, which meant real keys bypassed BOTH
   the async delivery added for input lag AND the PIC that gates re-entry -- so every fix
   aimed at the keyboard was dead code for actual keys, and the synthetic probe tested a path
   real keys do not take. Everything goes through here now, so the probe and a human press
   the same button. */
static void host_key_scancode(uint8_t rawsc, int ext, int is_break)
{
    HOST_LOCK();
    if (ext) vdd_input_push_scancode(&g_in, 0xE0);
    vdd_input_push_scancode(&g_in, is_break ? (uint8_t)(rawsc | 0x80) : rawsc);
    HOST_UNLOCK();
    /* The VDD raises IRQ1 itself now, on the 8042's empty->full transition and again as the
       guest drains the FIFO -- so the host must NOT latch one per byte here as well, or the
       interrupts run ahead of the bytes again. */
    if (g_key_event) SetEvent(g_key_event);
}

/* ── TYPEMATIC REPEAT: WE ARE THE KEYBOARD, SO WE MUST DO ITS REPEATING ───────
   THE BUG (user, reported twice): crash the ship in Skyroads while holding the up
   arrow and, on real DOS or stock ntvdm, the restarted level accelerates
   immediately. Under NTVDMEX you must release the key and press it again, and it
   works about one time in ten.
   ► WHY. The game hooks INT 09h and tracks key state from make/break codes. Its
     restart path clears that state, so the key has to be RE-ASSERTED -- and on real
     hardware it is, because an AT keyboard repeats the held key by itself, roughly
     every 92 ms after a ~500 ms delay. The guest sees a fresh make and carries on.
   ► WHAT WE WERE DOING. Outsourcing that to Windows: one make per WM_KEYDOWN, OS
     auto-repeat included. But auto-repeat arrives as WINDOW MESSAGES, and the UI
     thread was measured stalling up to 857 ms at a time -- so the repeats simply do
     not arrive. MEASURED: 3205 scancodes across a whole session, when a single
     60-second hold should exceed that on its own. The one-in-ten is the race
     between the restart and a repeat that mostly is not coming.
   ► THE FIX IS A MODELLING FIX, not a tuning one. A real keyboard's repeat does not
     depend on how busy the host is, so ours must not either: the deadline is driven
     from QueryPerformanceCounter and pumped from BOTH the exec loop and the UI
     timer, exactly as the PIT already is. When one thread stalls the other covers.
   ► ONE KEY, ONE PATH. OS auto-repeat is now SUPPRESSED (WM_KEYDOWN bit 30) because
     we generate our own; letting both through would double the rate and make typing
     stutter. That is the same principle that fixed the earlier keyboard bug, where
     a parallel host-side ring meant INT 16h appeared to work while the BIOS ring
     stayed empty forever.
   Only the most recently pressed key repeats, which is what AT hardware does. */
/* ► THE RATE IS NOT A CONSTANT AND MUST NOT BE ONE HERE. These started as 500 ms
     and 10.9/s, remembered AT hardware defaults. MEASURED against stock ntvdm on
     the same box with the same keyboard (tools/dostest/tymat.asm, run under both
     hosts via rt_stock.bat):
         stock ntvdm   delay 7 ticks ~385 ms   102 repeats -> 22.1/s
         us            delay 9 ticks ~495 ms    49 repeats -> 10.9/s
     Half the rate and a third again the delay. Hardcoding 385/22.1 would repeat
     the same mistake with a fresher number, because that is THIS box's XP setting,
     not a universal truth -- the user can change it in Control Panel and a real DOS
     box would follow. So take it from the system, which is the thing stock ntvdm is
     effectively passing through, and keep the measured pair above as the
     VERIFICATION target rather than the source. */
static uint32_t g_ty_delay_us  = 500000u;   /* replaced at startup from XP's setting */
static uint32_t g_ty_period_us =  92000u;
static DWORD    g_ty_spi_delay, g_ty_spi_speed;     /* raw, so STAGE2 can show them */

/* XP exposes the two values it programs into the keyboard controller:
     SPI_GETKEYBOARDDELAY  0..3  -> 250, 500, 750, 1000 ms
     SPI_GETKEYBOARDSPEED  0..31 -> about 2.5/s at 0 up to about 30/s at 31,
                                    linear in PERIOD rather than in rate. */
static void host_key_typematic_init(void)
{
    DWORD d = 1, s = 31;
    if (!SystemParametersInfoA(SPI_GETKEYBOARDDELAY, 0, &d, 0)) d = 1;
    if (!SystemParametersInfoA(SPI_GETKEYBOARDSPEED, 0, &s, 0)) s = 31;
    if (d > 3)  d = 3;
    if (s > 31) s = 31;
    g_ty_spi_delay = d; g_ty_spi_speed = s;
    g_ty_delay_us  = (d + 1) * 250000u;
    g_ty_period_us = 400000u - (s * 12000u);        /* 0 -> 400 ms, 31 -> 28 ms */
}
#define KEY_TYPEMATIC_DELAY_US  g_ty_delay_us
#define KEY_TYPEMATIC_PERIOD_US g_ty_period_us
static uint8_t  g_ty_sc, g_ty_ext, g_ty_on;
static LONGLONG g_ty_due;
static uint32_t g_ty_sent, g_ty_os_repeats;  /* ours generated / OS ones suppressed */

static LONGLONG qpc_ticks(uint32_t us)
{ return g_qpf.QuadPart ? (LONGLONG)((g_qpf.QuadPart / 1000) * us / 1000) : 0; }

static void host_key_typematic_press(uint8_t sc, int ext)
{
    LARGE_INTEGER n;
    QueryPerformanceCounter(&n);
    g_ty_sc = sc; g_ty_ext = (uint8_t)(ext ? 1 : 0); g_ty_on = 1;
    g_ty_due = n.QuadPart + qpc_ticks(KEY_TYPEMATIC_DELAY_US);
}

static void host_key_typematic_release(uint8_t sc, int ext)
{
    if (g_ty_on && g_ty_sc == sc && g_ty_ext == (uint8_t)(ext ? 1 : 0)) g_ty_on = 0;
}

/* Pumped from both threads; cheap and lock-free until it actually fires. */
static void host_key_typematic(void)
{
    LARGE_INTEGER n;
    if (!g_ty_on || !g_qpf.QuadPart) return;
    QueryPerformanceCounter(&n);
    if (n.QuadPart < g_ty_due) return;
    /* A stall must not turn into a burst of makes: schedule from NOW, not from the
       missed deadline, so we never try to "catch up" the repeats we owe. That is
       the same mistake the PIT catch-up made, and it is worse here -- a burst of
       makes is indistinguishable to the guest from the player hammering the key. */
    g_ty_due = n.QuadPart + qpc_ticks(KEY_TYPEMATIC_PERIOD_US);
    host_key_scancode(g_ty_sc, g_ty_ext, 0);
    g_ty_sent++;
}

static DWORD WINAPI synthkey_thread(LPVOID pv)
{
    int n;
    (void)pv;
    /* Reach the MENU before testing menu keys. The intro/attract loop reads no keyboard at
       all (measured: int16=[0,0,0,0], p60=0 for a whole run), so arrows sent during it prove
       nothing -- Enter is what gets from the intro to the menu, per the bug report. */
    /* A script on the share wins, if there is one: it can aim at a particular screen. */
    { HANDLE h = CreateFileA(KEYS_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             NULL, OPEN_EXISTING, 0, NULL);
      if (h != INVALID_HANDLE_VALUE) {
          char s[1024]; DWORD rd = 0; DWORD i = 0;
          ReadFile(h, s, sizeof s - 1, &rd, NULL);
          CloseHandle(h);
          s[rd] = 0;
          while (i < rd && g_running) {
              int ext = 0; DWORD v = 0; int digits = 0;
              while (i < rd && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i;
              if (i >= rd) break;
              if (s[i] == 'w' || s[i] == 'W') {           /* w<decimal ms> */
                  ++i;
                  while (i < rd && s[i] >= '0' && s[i] <= '9') { v = v*10 + (DWORD)(s[i]-'0'); ++i; }
                  { DWORD slept = 0;                       /* sleep in slices so a wind-down
                                                              is not stuck behind a long wait */
                    while (slept < v && g_running) { Sleep(v - slept > 100 ? 100 : v - slept);
                                                     slept += 100; } }
                  continue;
              }
              if (s[i] == 'e' || s[i] == 'E') { ext = 1; ++i; }
              while (i < rd && digits < 2) {               /* up to two hex digits */
                  char c = s[i]; int d = -1;
                  if (c >= '0' && c <= '9') d = c - '0';
                  else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
                  else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
                  if (d < 0) break;
                  v = (v << 4) | (DWORD)d; ++i; ++digits;
              }
              if (!digits) { ++i; continue; }              /* skip a token we do not grok */
              host_key_scancode((uint8_t)v, ext, 0);
              Sleep(60);                                    /* a human-length hold */
              host_key_scancode((uint8_t)v, ext, 1);
              Sleep(250);
          }
          return 0;
      } }

    Sleep(9000);
    for (n = 0; n < 400 && g_running; ++n) {
        /* An EXTENDED key (the arrows a player actually holds) at the OS auto-repeat rate,
           through exactly what WM_KEYDOWN does: E0 prefix + make code on the raw FIFO with
           an IRQ1 per byte, AND the BIOS ring entry that INT 16h returns. Feeding only the
           FIFO -- which the first version of this probe did -- means a game that reads INT
           16h never sees the key at all, so the probe passed while a real press killed it. */
        /* DOWN, not UP, and no Enter first. The menu opens with "Start!" already selected --
           the TOP item -- so a working UP arrow moves the highlight nowhere and a probe
           built on it cannot tell success from failure. DOWN has somewhere to go, and
           staying out of the game keeps the highlight on screen where a screenshot sees it. */
        host_key_scancode(0x50, 1, 0);                   /* INT 09h fills the BIOS ring now */
        Sleep(250);                                      /* menu-paced taps, not a hold */
        host_key_scancode(0x50, 1, 1);                   /* release each time          */
        Sleep(250);
    }
    return 0;
}

static DWORD WINAPI qirq_probe_thread(LPVOID pv)
{
    int n;
    (void)pv;
    Sleep(500);                                  /* let the guest install its ISRs */
    for (n = 0; n < 40 && g_running; ++n) {
        DWORD before = *(volatile DWORD *)(ULONG_PTR)0x714, after = before;
        int k;
        host_irq_sink(NULL, 5);
        for (k = 0; k < 50; ++k) {
            Sleep(1);
            after = *(volatile DWORD *)(ULONG_PTR)0x714;
            if (after != before) break;
        }
        if (n < 8) {
            char b[256], *q = b;
            q = zput(q, "QIRQ: raise#0x");   q = zhex(q, (DWORD)n);
            q = zput(q, " bits=0x");         q = zhex(q, g_qi_bits);
            q = zput(q, " st=0x");           q = zhex(q, (DWORD)g_qi_status);
            q = zput(q, " state 0x");        q = zhex(q, before);
            q = zput(q, "->0x");             q = zhex(q, after);
            q = zput(q, " after 0x");        q = zhex(q, (DWORD)k);
            q = zput(q, "ms pend5=0x");      q = zhex(q, (DWORD)g_irqn_pending[5]);
            q = zput(q, " ica=0x");          q = zhex(q, v86_ica_state(5));
            q = zput(q, "\r\n");
            log_append(LOG_PATH, b, q); serial_out(b, q);
        }
        Sleep(250);
    }
    return 0;
}

/* Headless heartbeat (session 11). Both qirq runs stopped logging mid-run and reached
   NO exit path -- not the guest's 4Ch flush, not the deadline backstop's report -- which
   means the process died without user-mode notice. A once-per-500ms beat carrying the
   guest's CS:IP/EFLAGS and the counters turns that silence into a timestamped last known
   position, which is the only way to tell "guest still spinning" from "process killed". */
static DWORD WINAPI heartbeat_thread(LPVOID pv)
{
    int n;
    (void)pv;
    for (n = 0; n < 80 && g_running; ++n) {
        char b[256], *q = b;
        DWORD cs = 0, ip = 0, efl = 0;
        if (g_tib_dbg) {
            cs  = VDM_REG(g_tib_dbg, VTIB_CS)  & 0xFFFF;
            ip  = VDM_REG(g_tib_dbg, VTIB_EIP) & 0xFFFF;
            efl = VDM_REG(g_tib_dbg, VTIB_EFLAGS);
        }
        q = zput(q, "HB 0x");        q = zhex(q, (DWORD)n);
        q = zput(q, " cs:ip=0x");    q = zhex(q, cs); q = zput(q, ":0x"); q = zhex(q, ip);
        q = zput(q, " efl=0x");      q = zhex(q, efl);
        q = zput(q, " state=0x");    q = zhex(q, *(volatile DWORD *)(ULONG_PTR)0x714);
        q = zput(q, " io=0x");       q = zhex(q, g_ev_io);
        q = zput(q, " irq0=0x");     q = zhex(q, g_irq0_inj);
        q = zput(q, " irqn=0x");     q = zhex(q, g_irqn_inj);
        q = zput(q, " intpend=0x");  q = zhex(q, g_ev_intpend);
        /* SB transfer state on the beat. irqn_refused=0 across 4.6M gate evaluations
           proves the completion IRQ was raised only AFTER the exec loop ended, so what
           matters now is WHEN the block starts and how fast it drains -- neither of which
           any counter shows after the fact. */
        q = zput(q, " sb{mode=0x");  q = zhex(q, (DWORD)g_sb.xfer_mode);
        q = zput(q, " left=0x");     q = zhex(q, g_sb.block_left);
        q = zput(q, " len=0x");      q = zhex(q, g_sb.block_len);
        q = zput(q, " blocks=0x");   q = zhex(q, g_sb.blocks);
        q = zput(q, " rate=0x");     q = zhex(q, g_sb.rate_hz);
        q = zput(q, "} mixed=0x");   q = zhex(q, g_audio.frames_mixed);
        q = zput(q, "\r\n");
        log_append(LOG_PATH, b, q); serial_out(b, q);
        Sleep(500);
    }
    return 0;
}

/* Headless deadline watchdog (session-9). A headless run must self-bound even when the
   guest blocks INSIDE a host INT handler -- e.g. a blocking INT 16h/21h key read at a
   "press any key" prompt or a game menu (host_conin + the INT 16h loops spin until
   g_running clears). The per-loop wall-clock caps can't fire then (the exec loop isn't
   iterating), so this thread forces the wind-down: after PM_HEADLESS_MS it clears
   g_running -- which unblocks every blocking key-read (all check !g_running) AND exits
   both exec loops -- and wakes any blocked reader. Started only when g_headless. */
static DWORD WINAPI headless_deadline_thread(LPVOID pv)
{
    char b[512], *q = b;
    DWORD f_cs = 0, f_ip = 0, f_efl = 0, f_tick = 0;
    BYTE  f_bytes[12];
    int   f_ok = 0;
    (void)pv;
    Sleep(PM_HEADLESS_MS);
    /* Snapshot the guest NOW, before anything winds down: by the time the grace
       period below expires the VDM address space may already be torn down, and
       reading it then faults this thread and loses the report entirely. */
    if (g_tib_dbg) {
        f_cs  = VDM_REG(g_tib_dbg, VTIB_CS)  & 0xFFFF;
        f_ip  = VDM_REG(g_tib_dbg, VTIB_EIP) & 0xFFFF;
        f_efl = VDM_REG(g_tib_dbg, VTIB_EFLAGS);
        { const volatile BYTE *cp = (const volatile BYTE *)((f_cs << 4) + f_ip);
          unsigned k; for (k = 0; k < 12; ++k) f_bytes[k] = cp[k]; }
        f_tick = ((DWORD)peekw(0x46E) << 16) | peekw(0x46C);
        f_ok = 1;
    }
    InterlockedExchange(&g_running, 0);         /* stop exec loops + unblock key reads */
    if (g_key_event) SetEvent(g_key_event);     /* wake a blocked host_conin/INT16 wait */
    q = zput(q, "HEADLESS: deadline reached -> g_running=0 (wind down)\r\n");
    log_append(LOG_PATH, b, q); serial_out(b, q);

    /* HARD BACKSTOP. Clearing g_running only stops a loop that gets a turn, and the
       exec loops only get one when the guest faults, BOPs or takes an interrupt. A
       guest spinning in pure V86 code -- Skyroads after its sound init, or any
       `jmp $`-shaped wait -- never returns from v86_run at all, so neither loop
       reaches its check and the process hangs forever. That wedges the SMB harness's
       `start /wait`, and the box needs a manual `controld kill` to recover, which is
       precisely the loop we cannot afford once every sound test is a real-mode DOS
       program. So: give the clean wind-down a grace period to flush its log and DOS
       output, then take the process down ourselves. rt.bat then collects the log and
       the watcher survives, which is the whole point of headless mode. */
    Sleep(PM_HEADLESS_GRACE_MS);
    if (!g_wound_down) {
        q = b;
        q = zput(q, "HEADLESS: exec loop never wound down (guest spinning in V86 with"
                    " no traps) -> forcing process exit\r\n");
        /* Report WHERE it froze and what the counters say. Without this the forced
           exit throws away the only evidence of why the guest stopped trapping,
           which is exactly what we need to disassemble the offending loop. */
        if (f_ok) {
            q = zput(q, "  frozen at CS:IP=0x"); q = zhex(q, f_cs);
            q = zput(q, ":0x"); q = zhex(q, f_ip);
            q = zput(q, " EFL=0x"); q = zhex(q, f_efl);
            q = zput(q, " tick=0x"); q = zhex(q, f_tick);
            q = zput(q, "\r\n  bytes: "); q = zdump(q, f_bytes, 12);
            q = zput(q, "\r\n");
        }
        log_append(LOG_PATH, b, q); serial_out(b, q); q = b;
        q = zput(q, "  io_events=0x");  q = zhex(q, g_ev_io);
        q = zput(q, " io_burst=0x");    q = zhex(q, g_io_extra);
        q = zput(q, " iostr=0x");       q = zhex(q, g_ev_iostr);
        q = zput(q, " irq0_inj=0x");    q = zhex(q, g_irq0_inj);
        q = zput(q, " irq0_skip=0x");   q = zhex(q, g_irq0_skip);
        q = zput(q, " intpend=0x");     q = zhex(q, g_ev_intpend);
        q = zput(q, " irqn_inj=0x");    q = zhex(q, g_irqn_inj);
        q = zput(q, " irqn_refused=0x"); q = zhex(q, g_irqn_refuse_total);
        q = zput(q, " raised_any=0x"); q = zhex(q, g_irq_raised_any);
        q = zput(q, " async_inj=0x");  q = zhex(q, g_async_inj);
        q = zput(q, " async_bail=0x"); q = zhex(q, g_async_bail);
        { int r; q = zput(q, " raised[0..7]=");
          for (r = 0; r < 8; ++r) { q = zput(q, "0x"); q = zhex(q, g_irq_raised[r]); q = zput(q, " "); } }
        q = zput(q, " sb_irq=0x"); q = zhex(q, (DWORD)g_sb.irq);
        q = zput(q, " qi_calls=0x");    q = zhex(q, g_qi_calls);
        q = zput(q, " qi_st=0x");       q = zhex(q, (DWORD)g_qi_status);
        q = zput(q, " state714=0x");    q = zhex(q, *(volatile DWORD *)(ULONG_PTR)0x714);
        q = zput(q, "\r\n  audio: silent=0x"); q = zhex(q, (DWORD)g_wave.silent);
        q = zput(q, " mixed=0x");        q = zhex(q, g_audio.frames_mixed);
        q = zput(q, " sb_dspwr=0x");     q = zhex(q, g_sb.dsp_writes);
        q = zput(q, " sb_blocks=0x");    q = zhex(q, g_sb.blocks);
        q = zput(q, " sb_mode=0x");      q = zhex(q, (DWORD)g_sb.xfer_mode);
        q = zput(q, " sb_rate=0x");      q = zhex(q, g_sb.rate_hz);
        q = zput(q, " bda_tick=0x");    q = zhex(q, ((DWORD)peekw(0x46E) << 16) | peekw(0x46C));
        q = zput(q, "\r\n");
        log_append(LOG_PATH, b, q); serial_out(b, q); q = b;
        { int i; q = zput(q, "  hot ports:");
          for (i = 0; i < g_io_hot_n; ++i) {
              q = zput(q, " 0x"); q = zhex(q, g_io_hot[i].port);
              q = zput(q, "=0x"); q = zhex(q, g_io_hot[i].n);
          }
          q = zput(q, "\r\n  pit_reload=0x"); q = zhex(q, (DWORD)g_pit.reload);
          q = zput(q, " pit_mode=0x");          q = zhex(q, (DWORD)g_pit.mode);
          q = zput(q, "\r\n");
          log_append(LOG_PATH, b, q); serial_out(b, q); q = b; }
        { int i; q = zput(q, "  unclaimed ports touched:");
          for (i = 0; i < g_unclaimed_n; ++i) { q = zput(q, " 0x"); q = zhex(q, g_unclaimed[i]); }
          q = zput(q, "\r\n"); }
        log_append(LOG_PATH, b, q); serial_out(b, q);
        ExitProcess(3);
    }
    return 0;
}

/* Non-blocking console read (INT 21h AH=06 DL=FF): a key char, or -1 if none. */
static int host_coninnb(void *ctx)
{
    uint16_t k; int got;
    (void)ctx;
    if (g_conin_pending >= 0) { int c = g_conin_pending; g_conin_pending = -1; return c; }
    HOST_LOCK();
    got = vdd_input_pop(&g_in, &k);
    HOST_UNLOCK();
    if (!got) return -1;
    if ((k & 0xFF) == 0) { g_conin_pending = (k >> 8) & 0xFF; return 0x00; }
    return k & 0xFF;
}

/* Non-blocking console status (INT 21h AH=0B / AH=06 DL=FF peek): 1 if a key is ready.
   An owed scancode counts as ready, or a program that polls status before reading would
   stall halfway through an arrow. */
static int host_conpeek(void *ctx)
{
    uint16_t k; int got;
    (void)ctx;
    if (g_conin_pending >= 0) return 1;
    HOST_LOCK();
    got = vdd_input_peek(&g_in, &k);
    HOST_UNLOCK();
    return got;
}

/* Reflect CF/ZF a bus interrupt returned onto the FLAGS the INT pushed on the
   V86 stack (SS:SP+4) -- the handler's IRET restores them. */
static void host_set_flags(volatile BYTE *tib, uint8_t cf, uint8_t zf)
{
    volatile WORD *pfl = (volatile WORD *)(((VDM_REG(tib, VTIB_SS) & 0xFFFF) << 4)
                         + (((VDM_REG(tib, VTIB_ESP) & 0xFFFF) + 4) & 0xFFFF));
    if (cf) *pfl |= 0x0001; else *pfl &= (WORD)~0x0001;
    if (zf) *pfl |= 0x0040; else *pfl &= (WORD)~0x0040;
}

/* --- XMS (M4) -------------------------------------------------------------- *
 * Extended memory lives on the host heap (above the 1MB the V86 map covers), so
 * each EMB is a VirtualAlloc block; xms_move() memcpys between it and the guest's
 * conventional window. The XMS entry point is a BOP stub reached by FAR CALL (so
 * it ends in RETF, not IRET); INT 2Fh AX=4300/4310 advertise it. */
static void *xms_host_alloc(void *ctx, uint32_t kb)
{
    (void)ctx;
    return VirtualAlloc(NULL, (SIZE_T)kb * 1024, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}
static void xms_host_free(void *ctx, void *p, uint32_t kb)
{
    (void)ctx; (void)kb;
    if (p) VirtualFree(p, 0, MEM_RELEASE);
}

/* Service one XMS far-call (function in AH). XMS returns AX=1 success / AX=0 fail
   with BL=error code -- it does NOT use the carry flag, so no pushed-FLAGS edit. */
static void host_xms(volatile BYTE *tib)
{
    DWORD ah = (VDM_REG(tib, VTIB_EAX) >> 8) & 0xFF;
    uint8_t err = XMSERR_NOTIMPL;
    uint16_t handle, nh;
    uint32_t largest, totfree, lin;
    uint8_t lock, freeh; uint32_t size_kb;

    #define X_SETAX(v) VDM_SET16(tib, VTIB_EAX, (v))
    #define X_SETBX(v) VDM_SET16(tib, VTIB_EBX, (v))
    #define X_SETDX(v) VDM_SET16(tib, VTIB_EDX, (v))
    #define X_SETBL(v) VDM_REG(tib, VTIB_EBX) = (VDM_REG(tib, VTIB_EBX) & 0xFFFFFF00u) | ((v) & 0xFF)
    #define X_SETBH(v) VDM_REG(tib, VTIB_EBX) = (VDM_REG(tib, VTIB_EBX) & 0xFFFF00FFu) | (((DWORD)(v) & 0xFF) << 8)
    #define X_FAIL(e)  do { X_SETAX(0); X_SETBL(e); } while (0)

    switch (ah) {
    case 0x00:                                  /* get version */
        X_SETAX(XMS_VERSION); X_SETBX(XMS_REVISION); X_SETDX(0);  /* DX=0: no HMA */
        break;
    case 0x01: X_FAIL(XMSERR_HMA_NONE);   break; /* request HMA (none provided) */
    case 0x02: X_FAIL(XMSERR_HMA_NOTALL); break; /* release HMA */
    case 0x03: case 0x05: g_xms.a20 = 1; X_SETAX(1); break;  /* enable A20 (global/local) */
    case 0x04: case 0x06: g_xms.a20 = 0; X_SETAX(1); break;  /* disable A20 */
    case 0x07: X_SETAX(g_xms.a20 ? 1 : 0); X_SETBL(0); break;/* query A20 */
    case 0x08:                                  /* query free extended memory */
        xms_query_free(&g_xms, &largest, &totfree);
        X_SETAX(largest > 0xFFFF ? 0xFFFF : largest);
        X_SETDX(totfree > 0xFFFF ? 0xFFFF : totfree);
        X_SETBL(largest ? 0 : XMSERR_NOMEM);
        break;
    case 0x09:                                  /* allocate EMB: DX=KB */
        if (xms_alloc(&g_xms, VDM_REG(tib, VTIB_EDX) & 0xFFFF, &nh, &err)) { X_SETAX(1); X_SETDX(nh); }
        else X_FAIL(err);
        break;
    case 0x0A:                                  /* free EMB: DX=handle */
        if (xms_free(&g_xms, (uint16_t)(VDM_REG(tib, VTIB_EDX) & 0xFFFF), &err)) X_SETAX(1);
        else X_FAIL(err);
        break;
    case 0x0B: {                                /* move EMB: DS:SI -> move struct */
        DWORD ds = VDM_REG(tib, VTIB_DS) & 0xFFFF, si = VDM_REG(tib, VTIB_ESI) & 0xFFFF;
        const volatile BYTE *s = (const volatile BYTE *)((ds << 4) + si);
        xms_move_t mv;
        mv.length     = (DWORD)s[0] | ((DWORD)s[1] << 8) | ((DWORD)s[2] << 16) | ((DWORD)s[3] << 24);
        mv.src_handle = (uint16_t)(s[4] | (s[5] << 8));
        mv.src_offset = (DWORD)s[6] | ((DWORD)s[7] << 8) | ((DWORD)s[8] << 16) | ((DWORD)s[9] << 24);
        mv.dst_handle = (uint16_t)(s[10] | (s[11] << 8));
        mv.dst_offset = (DWORD)s[12] | ((DWORD)s[13] << 8) | ((DWORD)s[14] << 16) | ((DWORD)s[15] << 24);
        if (xms_move(&g_xms, NULL, &mv, &err)) X_SETAX(1); else X_FAIL(err);
        break; }
    case 0x0C:                                  /* lock EMB: DX=handle -> DX:BX linear */
        handle = (uint16_t)(VDM_REG(tib, VTIB_EDX) & 0xFFFF);
        if (xms_lock(&g_xms, handle, &lin, &err)) { X_SETAX(1); X_SETDX(lin >> 16); X_SETBX(lin & 0xFFFF); }
        else X_FAIL(err);
        break;
    case 0x0D:                                  /* unlock EMB: DX=handle */
        if (xms_unlock(&g_xms, (uint16_t)(VDM_REG(tib, VTIB_EDX) & 0xFFFF), &err)) X_SETAX(1);
        else X_FAIL(err);
        break;
    case 0x0E:                                  /* get handle info: DX=handle */
        handle = (uint16_t)(VDM_REG(tib, VTIB_EDX) & 0xFFFF);
        if (xms_info(&g_xms, handle, &lock, &freeh, &size_kb, &err)) {
            X_SETAX(1); X_SETBH(lock); X_SETBL(freeh); X_SETDX(size_kb > 0xFFFF ? 0xFFFF : size_kb);
        } else X_FAIL(err);
        break;
    case 0x0F:                                  /* reallocate EMB: BX=new KB, DX=handle */
        handle = (uint16_t)(VDM_REG(tib, VTIB_EDX) & 0xFFFF);
        if (xms_realloc(&g_xms, handle, VDM_REG(tib, VTIB_EBX) & 0xFFFF, &err)) X_SETAX(1);
        else X_FAIL(err);
        break;
    case 0x10: X_SETAX(0); X_SETBL(0xB1); X_SETDX(0); break;  /* request UMB: none */
    case 0x11: X_FAIL(0xB2); break;                          /* release UMB */
    default:   X_FAIL(XMSERR_NOTIMPL); break;
    }
    #undef X_SETAX
    #undef X_SETBX
    #undef X_SETDX
    #undef X_SETBL
    #undef X_SETBH
    #undef X_FAIL
}

/* --- EMS (M4) -------------------------------------------------------------- *
 * Expanded memory lives on the host heap (pages * 16KB per handle); the 64KB
 * page frame at E000:0 is real V86 RAM (v86 Map 5). ems_map memcpys logical
 * pages in/out of the frame windows (page-frame shadowing). INT 67h carries the
 * function in AH and returns status in AH (0 = ok). */
static void *ems_host_alloc(void *ctx, uint32_t pages)
{
    (void)ctx;
    return VirtualAlloc(NULL, (SIZE_T)pages * EMS_PAGE_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}
static void ems_host_free(void *ctx, void *p, uint32_t pages)
{
    (void)ctx; (void)pages;
    if (p) VirtualFree(p, 0, MEM_RELEASE);
}

/* Service one INT 67h (EMM) call (function in AH; status back in AH). */
static void host_ems(volatile BYTE *tib)
{
    DWORD ah = (VDM_REG(tib, VTIB_EAX) >> 8) & 0xFF;
    uint8_t err = EMSERR_UNDEFFUNC;
    uint16_t handle = 0, p16 = 0, p16b = 0;

    #define E_SETAH(v) VDM_REG(tib, VTIB_EAX) = (VDM_REG(tib, VTIB_EAX) & 0xFFFF00FFu) | (((DWORD)(v) & 0xFF) << 8)
    #define E_SETAL(v) VDM_REG(tib, VTIB_EAX) = (VDM_REG(tib, VTIB_EAX) & 0xFFFFFF00u) | ((v) & 0xFF)
    #define E_SETBX(v) VDM_SET16(tib, VTIB_EBX, (v))
    #define E_SETDX(v) VDM_SET16(tib, VTIB_EDX, (v))

    switch (ah) {
    case 0x40: E_SETAH(EMS_OK); break;                  /* get manager status   */
    case 0x41: E_SETBX(g_ems.frame_seg); E_SETAH(EMS_OK); break;  /* page frame seg */
    case 0x42:                                          /* unallocated/total pages */
        ems_counts(&g_ems, &p16, &p16b);
        E_SETBX(p16); E_SETDX(p16b); E_SETAH(EMS_OK);
        break;
    case 0x43:                                          /* allocate BX pages -> DX handle */
        if (ems_alloc(&g_ems, VDM_REG(tib, VTIB_EBX) & 0xFFFF, &handle, &err)) { E_SETDX(handle); E_SETAH(EMS_OK); }
        else E_SETAH(err);
        break;
    case 0x44:                                          /* map: AL=phys BX=logical DX=handle */
        if (ems_map(&g_ems, (uint8_t)(VDM_REG(tib, VTIB_EAX) & 0xFF),
                    (uint16_t)(VDM_REG(tib, VTIB_EBX) & 0xFFFF),
                    (uint16_t)(VDM_REG(tib, VTIB_EDX) & 0xFFFF), &err)) E_SETAH(EMS_OK);
        else E_SETAH(err);
        break;
    case 0x45:                                          /* deallocate DX handle */
        if (ems_free(&g_ems, (uint16_t)(VDM_REG(tib, VTIB_EDX) & 0xFFFF), &err)) E_SETAH(EMS_OK);
        else E_SETAH(err);
        break;
    case 0x46: E_SETAL(EMS_VERSION); E_SETAH(EMS_OK); break;  /* EMM version 4.0 */
    case 0x47:                                          /* save page map: DX handle */
        if (ems_save_map(&g_ems, (uint16_t)(VDM_REG(tib, VTIB_EDX) & 0xFFFF), &err)) E_SETAH(EMS_OK);
        else E_SETAH(err);
        break;
    case 0x48:                                          /* restore page map: DX handle */
        if (ems_restore_map(&g_ems, (uint16_t)(VDM_REG(tib, VTIB_EDX) & 0xFFFF), &err)) E_SETAH(EMS_OK);
        else E_SETAH(err);
        break;
    case 0x4B: E_SETBX(ems_handle_count(&g_ems)); E_SETAH(EMS_OK); break;  /* # handles */
    case 0x4C:                                          /* pages owned by DX handle */
        if (ems_handle_pages(&g_ems, (uint16_t)(VDM_REG(tib, VTIB_EDX) & 0xFFFF), &p16, &err)) { E_SETBX(p16); E_SETAH(EMS_OK); }
        else E_SETAH(err);
        break;
    case 0x51:                                          /* reallocate: BX pages, DX handle */
        if (ems_realloc(&g_ems, (uint16_t)(VDM_REG(tib, VTIB_EDX) & 0xFFFF),
                        (uint16_t)(VDM_REG(tib, VTIB_EBX) & 0xFFFF), &err)) {
            ems_handle_pages(&g_ems, (uint16_t)(VDM_REG(tib, VTIB_EDX) & 0xFFFF), &p16, &err);
            E_SETBX(p16); E_SETAH(EMS_OK);
        } else E_SETAH(err);
        break;
    default: E_SETAH(EMSERR_UNDEFFUNC); break;
    }
    #undef E_SETAH
    #undef E_SETAL
    #undef E_SETBX
    #undef E_SETDX
}

/* --- menu + status bar (scaffold; most items are stubs for now) ------------ */
static char g_progname[64] = "(none)";      /* shown in the status bar          */
static volatile int g_title_dirty = 1;      /* UI thread re-applies the caption  */

/* Mouse state shared UI thread -> V86 thread (INT 33h). Position is in guest
   pixels (mapped from the window client); buttons: bit0 L, bit1 R, bit2 M. */
static volatile LONG g_ms_x = 320, g_ms_y = 240, g_ms_btn = 0;
static volatile LONG g_ms_hidden = 1;       /* INT 33h cursor hide-count; 0 => visible */

/* THE HOST ARROW over the video area, which is a SEPARATE thing from the INT 33h
   driver cursor above. The client used to hide it unconditionally, which is right
   for a game that has taken the mouse and wrong for everything else: with no
   pointer at all you cannot see where you are aiming a click, and you lose the
   pointer entirely the moment it crosses into the window. It is a user choice, so
   it is a toggle -- Input > Mouse > Show Host Cursor, or Ctrl+F8. Default OFF, i.e.
   exactly the behaviour that shipped, so no run changes unless it is asked for. */
static volatile LONG g_cursor_show = 0;

/* INT 33h mouse driver (functions DOS apps actually use). The host draws the
   cursor (overlay in the present path) when the hide-count is 0, so apps that
   rely on the driver cursor (the common case) get a visible pointer. */
static void mouse_int33(volatile BYTE *tib)
{
    static LONG last_x = 320, last_y = 240;             /* for AX=0B motion (V86 thread only) */
    DWORD ax = VDM_REG(tib, VTIB_EAX) & 0xFFFF;
    LONG x = g_ms_x, y = g_ms_y, b = g_ms_btn;
    switch (ax) {
    case 0x0000:                                        /* reset + get status      */
        VDM_SET16(tib, VTIB_EAX, 0xFFFF);               /* driver installed        */
        VDM_SET16(tib, VTIB_EBX, 0x0002);               /* 2 buttons               */
        InterlockedExchange(&g_ms_hidden, 1);           /* hidden until Show       */
        break;
    case 0x0001:                                        /* show cursor (dec count) */
        if (g_ms_hidden > 0) InterlockedDecrement(&g_ms_hidden);
        break;
    case 0x0002:                                        /* hide cursor (inc count) */
        InterlockedIncrement(&g_ms_hidden);
        break;
    case 0x0003:                                        /* get position + buttons  */
        VDM_SET16(tib, VTIB_ECX, (WORD)x);
        VDM_SET16(tib, VTIB_EDX, (WORD)y);
        VDM_SET16(tib, VTIB_EBX, (WORD)b);
        break;
    case 0x0004:                                        /* set cursor position     */
        InterlockedExchange(&g_ms_x, (LONG)(VDM_REG(tib, VTIB_ECX) & 0xFFFF));
        InterlockedExchange(&g_ms_y, (LONG)(VDM_REG(tib, VTIB_EDX) & 0xFFFF));
        break;
    case 0x0005: case 0x0006:                           /* button press/release info */
        VDM_SET16(tib, VTIB_EAX, (WORD)b);
        VDM_SET16(tib, VTIB_EBX, 0);                    /* 0 presses since last call */
        VDM_SET16(tib, VTIB_ECX, (WORD)x);
        VDM_SET16(tib, VTIB_EDX, (WORD)y);
        break;
    case 0x000B:                                        /* read relative motion    */
        VDM_SET16(tib, VTIB_ECX, (WORD)((x - last_x) * 8));   /* ~8 mickeys / pixel */
        VDM_SET16(tib, VTIB_EDX, (WORD)((y - last_y) * 8));
        last_x = x; last_y = y;
        break;
    default: break;                                     /* 07/08 range, 0C handler, ...: accept */
    }
}

/* Classic arrow cursor: 'o' = black outline (index 0), 'X' = white fill (15),
   ' ' = transparent; hotspot at the top-left tip. Drawn into the 8bpp frame each
   present -- the frame is re-rendered from VRAM every tick, so it leaves no trail. */
/* The INT 33h driver cursor. This was hand-drawn ASCII art until the demo sweep
   turned up its one cosmetic defect -- "the mouse cursor is not quite the right
   shape" -- so it is now DECODED FROM REAL ARTWORK and regenerated rather than
   remembered. 'o' = outline (palette index 0), 'X' = fill (index 15), ' ' =
   transparent; only the data changed, overlay_cursor() is untouched.
   Regenerate with:  python3 tools/mkcursor.py cursors/cursor-pointer.cur       */
/* Generated by tools/mkcursor.py from cursors/cursor-pointer.cur -- 16x16, hotspot (0,0).
   Do not hand-edit: regenerate from the artwork instead. */
static const char *const MS_CURSOR[16] = {
    "oo",
    "oXo",
    "oXXo",
    "oXXXo",
    "oXXXXo",
    "oXXXXXo",
    "oXXXXXXo",
    "oXXXXXXXo",
    "oXXXXXXXXo",
    "oXXXXXooooo",
    "oXXoXXo",
    "oXo oXXo",
    "oo  oXXo",
    "o    oXXo",
    "     oXXo",
    "      oo",
};
static void overlay_cursor(uint8_t *px, int W, int H, int stride, int mx, int my)
{
    int row;
    for (row = 0; row < 16; ++row) {
        const char *s = MS_CURSOR[row]; int col, y = my + row;
        if (y < 0 || y >= H) continue;
        for (col = 0; s[col]; ++col) {
            int x = mx + col; char c = s[col];
            if (c == ' ' || x < 0 || x >= W) continue;
            px[y * stride + x] = (c == 'o') ? 0 : 15;     /* black outline / white fill */
        }
    }
}

enum {                                       /* wired command IDs                */
    IDM_STUB = 1,                            /* every not-yet-wired item          */
    IDM_FILE_EXIT, IDM_FILE_CLOSEPROG,
    IDM_DISP_FULLSCREEN, IDM_DISP_SHOWMENU,
    IDM_INPUT_CURSOR,
    IDM_CAP_SHOT,
    IDM_HELP_ABOUT
};

static void mi (HMENU m, const char *s, UINT id) { AppendMenuA(m, MF_STRING, id, s); }
static void msep(HMENU m) { AppendMenuA(m, MF_SEPARATOR, 0, NULL); }
static void msub(HMENU p, const char *s, HMENU c) { AppendMenuA(p, MF_POPUP, (UINT_PTR)c, s); }
static HMENU mpop(void) { return CreatePopupMenu(); }

/* Build the full menu tree (the structure is the scaffold; only a few items are
   wired -- the rest carry IDM_STUB and no-op until they're implemented). */
static HMENU build_menu(void)
{
    HMENU bar = CreateMenu(), m, s;
    m = mpop();                                                   /* File         */
    mi(m, "Open Executable...\tCtrl+O", IDM_STUB);
    msub(m, "Open Recent", (s=mpop(), mi(s,"(empty)",IDM_STUB), s));
    msep(m);
    msub(m, "Save State", (s=mpop(), mi(s,"Quick-Save\tF5",IDM_STUB), mi(s,"Slot 1...9",IDM_STUB), s));
    msub(m, "Load State", (s=mpop(), mi(s,"Quick-Load\tF9",IDM_STUB), mi(s,"Slot 1...9",IDM_STUB), s));
    msep(m);
    msub(m, "Configuration", (s=mpop(), mi(s,"Edit Config File...",IDM_STUB),
        mi(s,"Reload Configuration",IDM_STUB), mi(s,"Save Current as Default",IDM_STUB),
        mi(s,"Open Config Folder",IDM_STUB), mi(s,"Configuration Tool...",IDM_STUB), s));
    msep(m);
    mi(m, "Restart Machine", IDM_STUB);
    mi(m, "Close Program", IDM_FILE_CLOSEPROG);
    mi(m, "Exit\tAlt+F4", IDM_FILE_EXIT);
    msub(bar, "File", m);

    m = mpop();                                                   /* Edit         */
    mi(m,"Mark / Select Region",IDM_STUB); mi(m,"Copy\tCtrl+C",IDM_STUB);
    mi(m,"Copy Whole Screen",IDM_STUB); mi(m,"Paste\tCtrl+V",IDM_STUB); mi(m,"Select All",IDM_STUB);
    msub(bar, "Edit", m);

    m = mpop();                                                   /* CPU          */
    msub(m,"CPU Type",(s=mpop(),mi(s,"8086",IDM_STUB),mi(s,"286",IDM_STUB),mi(s,"386",IDM_STUB),mi(s,"486",IDM_STUB),mi(s,"Pentium",IDM_STUB),s));
    msub(m,"Core",(s=mpop(),mi(s,"Auto",IDM_STUB),mi(s,"Normal",IDM_STUB),mi(s,"Dynamic",IDM_STUB),mi(s,"Simple",IDM_STUB),s));
    mi(m,"FPU",IDM_STUB); msep(m);
    msub(m,"Speed",(s=mpop(),mi(s,"Auto",IDM_STUB),mi(s,"Max",IDM_STUB),mi(s,"Fixed cycles...",IDM_STUB),s));
    mi(m,"Turbo",IDM_STUB); msep(m);
    msub(m,"Memory",(s=mpop(),mi(s,"Conventional",IDM_STUB),mi(s,"XMS",IDM_STUB),mi(s,"EMS",IDM_STUB),mi(s,"UMB",IDM_STUB),mi(s,"A20",IDM_STUB),s));
    msub(bar, "CPU", m);

    m = mpop();                                                   /* Display      */
    mi(m,"Fullscreen\tAlt+Enter",IDM_DISP_FULLSCREEN);
    msub(m,"Window Size",(s=mpop(),mi(s,"1x",IDM_STUB),mi(s,"2x",IDM_STUB),mi(s,"3x",IDM_STUB),mi(s,"Custom",IDM_STUB),s));
    msub(m,"Renderer",(s=mpop(),mi(s,"GDI",IDM_STUB),mi(s,"DirectDraw",IDM_STUB),mi(s,"Direct3D9",IDM_STUB),mi(s,"OpenGL",IDM_STUB),s));
    msub(m,"Scaler",(s=mpop(),mi(s,"None",IDM_STUB),mi(s,"Scale2x",IDM_STUB),mi(s,"hq2x",IDM_STUB),mi(s,"Scanlines",IDM_STUB),mi(s,"CRT",IDM_STUB),s));
    msub(m,"Filtering",(s=mpop(),mi(s,"Nearest",IDM_STUB),mi(s,"Bilinear",IDM_STUB),s));
    mi(m,"Aspect Ratio Correction",IDM_STUB);
    msub(m,"Frame Skip",(s=mpop(),mi(s,"0",IDM_STUB),mi(s,"1",IDM_STUB),mi(s,"2",IDM_STUB),s));
    mi(m,"VSync",IDM_STUB); msep(m);
    mi(m,"Show Menu Bar",IDM_DISP_SHOWMENU);
    msub(bar, "Display", m);

    m = mpop();                                                   /* Audio        */
    mi(m,"Master Volume / Mute",IDM_STUB); msep(m);
    msub(m,"Sound Blaster",(s=mpop(),mi(s,"SB16",IDM_STUB),mi(s,"AWE32",IDM_STUB),mi(s,"Address / IRQ / DMA",IDM_STUB),s));
    msub(m,"AdLib / OPL",(s=mpop(),mi(s,"OPL2",IDM_STUB),mi(s,"OPL3",IDM_STUB),s));
    msub(m,"MIDI",(s=mpop(),mi(s,"Host GM",IDM_STUB),mi(s,"MT-32",IDM_STUB),mi(s,"SoundFont...",IDM_STUB),s));
    mi(m,"PC Speaker",IDM_STUB); msub(m,"Gravis Ultrasound",(s=mpop(),mi(s,"Enable",IDM_STUB),s));
    msub(m,"Tandy / CMS",(s=mpop(),mi(s,"Enable",IDM_STUB),s)); msep(m);
    msub(m,"Sample Rate / Buffer",(s=mpop(),mi(s,"22050",IDM_STUB),mi(s,"44100",IDM_STUB),s));
    msub(bar, "Audio", m);

    m = mpop();                                                   /* Input        */
    msub(m,"Mouse",(s=mpop(),mi(s,"Capture\tCtrl+F10",IDM_STUB),
        mi(s,"Show Host Cursor\tCtrl+F8",IDM_INPUT_CURSOR),
        mi(s,"Seamless",IDM_STUB),mi(s,"Sensitivity",IDM_STUB),s));
    msub(m,"Keyboard",(s=mpop(),mi(s,"Layout",IDM_STUB),mi(s,"Typematic rate",IDM_STUB),mi(s,"Send Ctrl+Alt+Del",IDM_STUB),s));
    msub(m,"Joystick",(s=mpop(),mi(s,"Type",IDM_STUB),mi(s,"Map to gamepad",IDM_STUB),mi(s,"Calibrate",IDM_STUB),s));
    msep(m); mi(m,"Key Mapper...",IDM_STUB);
    msub(bar, "Input", m);

    m = mpop();                                                   /* Drive        */
    mi(m,"Mount Folder as Drive...",IDM_STUB); mi(m,"Mount Disk / CD Image...",IDM_STUB);
    mi(m,"Mount Physical Drive...",IDM_STUB); msub(m,"Unmount",(s=mpop(),mi(s,"(none)",IDM_STUB),s)); msep(m);
    mi(m,"Boot from Drive / Image...",IDM_STUB); mi(m,"Swap Disk\tCtrl+F4",IDM_STUB); msep(m);
    mi(m,"Drive Status...",IDM_STUB);
    msub(bar, "Drive", m);

    m = mpop();                                                   /* Capture      */
    mi(m,"Take Screenshot\tCtrl+F5",IDM_CAP_SHOT);
    msub(m,"Record Video (AVI)",(s=mpop(),mi(s,"Start / Stop",IDM_STUB),s));
    msub(m,"Record Audio (WAV)",(s=mpop(),mi(s,"Start / Stop",IDM_STUB),s));
    msub(m,"Record OPL / MIDI",(s=mpop(),mi(s,"Start / Stop",IDM_STUB),s)); msep(m);
    mi(m,"Open Capture Folder",IDM_STUB); mi(m,"Capture Settings...",IDM_STUB);
    msub(bar, "Capture", m);

    m = mpop();                                                   /* Debug        */
    mi(m,"Pause / Resume\tPause",IDM_STUB); mi(m,"Step Instruction",IDM_STUB);
    mi(m,"Debugger Console...",IDM_STUB); mi(m,"Registers / Memory / Disassembly",IDM_STUB); msep(m);
    msub(m,"Logging",(s=mpop(),mi(s,"Levels...",IDM_STUB),mi(s,"Log to file",IDM_STUB),s));
    mi(m,"Performance Overlay",IDM_STUB);
    msub(bar, "Debug", m);

    m = mpop();                                                   /* Help         */
    mi(m,"Quick Start",IDM_STUB); mi(m,"Keyboard Shortcuts...",IDM_STUB); msep(m);
    mi(m,"About",IDM_HELP_ABOUT);
    msub(bar, "Help", m);
    return bar;
}

static HWND g_status;                        /* the native comctl32 status bar    */

/* Create the native status bar child + set its text; record its height so the
   video blit reserves that strip. */
/* Window caption: "Windows XP Virtual DOS Machine" idle, "... - PROG.EXE" while a
   program runs. Safe to call from the V86 thread (SetWindowText posts WM_SETTEXT). */
#define VDM_WIN_TITLE "Windows XP Virtual DOS Machine"
static void set_window_title(void)
{
    char t[128]; char *p = t; const char *s = VDM_WIN_TITLE;
    if (!g_hwnd) return;
    while (*s) *p++ = *s++;
    if (g_progname[0] && g_progname[0] != '(') {       /* not "(none)" */
        const char *d = " - ", *b = g_progname;
        while (*d) *p++ = *d++;
        while (*b) *p++ = *b++;
    }
    *p = 0;
    SetWindowTextA(g_hwnd, t);
}

static void make_status(HWND parent, HINSTANCE hi)
{
    char line[96]; char *p = line; RECT sr;
    const char *a = "NTVDMEX  -  ";
    g_status = CreateWindowExA(0, STATUSCLASSNAME, NULL,
                               WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                               0, 0, 0, 0, parent, NULL, hi, NULL);
    if (!g_status) return;
    while (*a) *p++ = *a++;
    { const char *b = g_progname; while (*b) *p++ = *b++; } *p = 0;
    SendMessageA(g_status, SB_SETTEXTA, 0, (LPARAM)line);
    GetWindowRect(g_status, &sr);
    if (sr.bottom > sr.top) g_pd.status_h = sr.bottom - sr.top;
}

/* Tick/untick a menu item BY COMMAND, through whichever menu is live: "Show Menu
   Bar" DETACHES the bar into g_savedmenu, and a toggle pressed by hotkey while it
   is detached must still be recorded there or the tick is stale when it returns. */
static void menu_check(HWND h, UINT id, int on)
{
    HMENU m = GetMenu(h);
    if (!m) m = g_savedmenu;
    if (m) CheckMenuItem(m, id, MF_BYCOMMAND | (UINT)(on ? MF_CHECKED : MF_UNCHECKED));
}

/* Show/hide the host arrow over the video. The immediate SetCursor matters:
   WM_SETCURSOR only fires when the mouse next MOVES or re-enters the window, so
   without it the tick changes and the pointer does not until you jiggle it. Guard
   it on the pointer actually being over us -- SetCursor changes the shape there and
   then, and we have no business touching it while it is over someone else. */
static void host_cursor_set(HWND h, int on)
{
    POINT pt;
    InterlockedExchange(&g_cursor_show, on ? 1 : 0);
    menu_check(h, IDM_INPUT_CURSOR, on);
    if (GetCursorPos(&pt) && WindowFromPoint(pt) == h)
        SetCursor(on ? LoadCursorA(NULL, IDC_ARROW) : NULL);
}

/* --- the UI thread: window + present + frame timer ------------------------- */
static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_TIMER:
        if (g_title_dirty) { set_window_title(); g_title_dirty = 0; }  /* apply on UI thread */
        /* Drive the PIT from REAL elapsed time so the BIOS tick (0040:006C) and
           INT 1Ah track wall-clock regardless of WM_TIMER jitter; clamp after a
           stall so we don't flood a catch-up burst of ticks. */
        /* The frame loop no longer OWNS the PIT (that is host_pit_sync's shared clock), but
           it must still drive it: if only the exec thread advanced time, a guest spinning in
           its own code would wait forever for a clock that only ticks when it traps -- which
           is exactly the deadlock the first cut of this produced (async_bail=497,
           async_inj=0, guest frozen 25 s). Ticking here is also what lets the async injector
           preempt, since this thread is not the one stuck inside VdmStartExecution. */
        /* BEFORE ANY LOCK. If this gap is large while lk_wait stays small, the UI
           thread is not running at all and no amount of lock-splitting will help. */
        {   static LARGE_INTEGER s_prev;
            LARGE_INTEGER n;
            QueryPerformanceCounter(&n);
            if (s_prev.QuadPart) {
                uint32_t g = qpc_us(n.QuadPart - s_prev.QuadPart);
                if (g > g_ui_gap_us) g_ui_gap_us = g;
            }
            s_prev = n; }
        g_pit.frame_us = 0;
        host_pit_sync();
        host_key_typematic();       /* pumped from BOTH threads, like the PIT */
        HOST_LOCK();
        vdd_bus_frame(&g_bus);          /* tick PIT + render into g_vid.frame       */
        /* PRESENT IN PHASE WITH THE GUEST'S FRAME, not on our own timer.
           This tick used to run at 30 Hz and snapshot whenever it happened to fire.
           Once the guest was correctly paced to 60/70 Hz that meant sampling once
           per TWO of its frames at an arbitrary phase -- so a program that erases an
           object and redraws it one pixel along (BOUNCEBX) got caught between the
           two about half the time, and the object was simply missing from that
           frame. That is the residual tearing left after the 0x3DA fix, and it was
           ours, not the guest's: its frame rate measured 59.9 Hz.
           Now the timer runs fast and we snapshot only when the emulated CRT is in
           the tail of its active period, i.e. the guest has finished drawing and is
           parked in its retrace wait. The staleness fallback guarantees we still
           present if the phase window keeps being missed, so a guest that never
           touches 0x3DA is unaffected. */
        {   static DWORD s_last_present = 0;
            DWORD nowt = GetTickCount();
            int stale = (DWORD)(nowt - s_last_present) >= VID_PRESENT_STALE_MS;
            if (vdd_video_present_ready(&g_vid) || stale) {
                s_last_present = nowt;
                if (g_ms_hidden == 0 && g_vid.frame.bpp == 8 && g_vid.frame.pixels)
                    overlay_cursor((uint8_t *)g_vid.frame.pixels, g_vid.frame.w, g_vid.frame.h,
                                   (int)g_vid.frame.stride, g_ms_x, g_ms_y);  /* driver cursor */
                present_ddraw_snapshot(&g_pd, &g_vid.frame); /* consistent copy UNDER lock */
                HOST_UNLOCK();
                present_ddraw_present(&g_pd);   /* vsync'd blit OUTSIDE the lock         */
            } else {
                HOST_UNLOCK();  /* not our phase: keep the last frame up */
            }
        }
        /* Headless remote visual capture (session-9): the host screenshots ITSELF to
           C:\ntvdmex\shotNN.bmp every ~2s so a graphical run (Skyroads, the PM demos)
           is verifiable off the SMB share -- VNC capture is dead on the real box. The
           snapshot is owned by this UI thread, so no extra lock is needed; capped at
           40 frames so a long run never fills the disk. rt.bat copies shot*.bmp off. */
        if (g_capture) {
            static unsigned cap_tick = 0, cap_seq = 0;
            /* ► 2 s IS FAR TOO SLOW TO CATCH A MODE SWITCH. Doom runs about ten
                 seconds and sets mode 13h in the last fraction of it, so a 2 s cadence
                 caught exactly ONE frame -- blank text mode, two distinct colours. At
                 ~300 ms the 40-frame budget spans a whole run and straddles the switch,
                 which is the only way to SEE what the guest drew: the rig has no VNC and
                 `screendump` is QEMU-only, so these BMPs are the only eyes we have. */
            /* ► THE CADENCE IS A KNOB, BECAUSE 40 FRAMES x 300 ms ONLY SEES THE FIRST
                 TWELVE SECONDS. A scripted keypress run presses its first key at 14 s
                 -- deliberately, so the game has reached its demo -- and every shot had
                 already been taken by then. The run looked like "the keys did nothing"
                 when what actually happened is that nobody was looking. capture.flag's
                 CONTENTS are the period in milliseconds now; empty keeps the 300 ms
                 default, and 1100 spans a whole 45 s headless run. */
            if ((cap_tick++ % (g_capture_ms / VID_PRESENT_TICK_MS + 1)) == 0 && cap_seq < 40) {
                char path[] = "C:\\ntvdmex\\shot00.bmp";
                path[15] = (char)('0' + (cap_seq / 10) % 10);
                path[16] = (char)('0' + cap_seq % 10);
                if (present_ddraw_save_bmp(&g_pd, path) == 0) ++cap_seq;
            }
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;                       /* we own the whole client -> no white erase */
    case WM_SETCURSOR:
        /* Hide it over the VIDEO ONLY, and only while the toggle says so. wp is the
           window the hit-test belongs to, and testing the hit-test ALONE was wrong:
           the status bar is a child that forwards its own HTCLIENT here, so the
           cursor vanished over the status bar and its size grip as well. */
        if ((HWND)wp == h && LOWORD(lp) == HTCLIENT && !g_cursor_show) {
            SetCursor(NULL); return TRUE;
        }
        break;
    case WM_PAINT: {                     /* re-blit the last snapshot on expose/move   */
        PAINTSTRUCT ps; BeginPaint(h, &ps);
        present_ddraw_present(&g_pd);
        EndPaint(h, &ps);
        return 0; }
    case WM_SIZE:
        if (g_status) SendMessageA(g_status, WM_SIZE, 0, 0);  /* let it re-dock     */
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_FILE_EXIT: DestroyWindow(h); return 0;
        case IDM_DISP_FULLSCREEN: present_ddraw_set_fullscreen(&g_pd, !g_pd.fullscreen); return 0;
        case IDM_DISP_SHOWMENU: {
            HMENU cur = GetMenu(h);
            if (cur) { g_savedmenu = cur; SetMenu(h, NULL); } else SetMenu(h, g_savedmenu);
            return 0; }
        case IDM_INPUT_CURSOR: host_cursor_set(h, !g_cursor_show); return 0;
        case IDM_CAP_SHOT: host_screenshot(); return 0;
        case IDM_HELP_ABOUT:
            MessageBoxA(h, "NTVDMEX -- New Technology Virtual DOS Manager, Extended\n"
                          "A from-scratch ntvdm.exe for Windows XP (DOS on the real CPU in V86).",
                          "About NTVDMEX", MB_OK | MB_ICONINFORMATION); return 0;
        default: return 0;               /* IDM_STUB / not-yet-wired items: no-op      */
        }
    case WM_SYSKEYDOWN:                  /* Alt+Enter -> toggle fullscreen          */
        if (wp == VK_RETURN) { present_ddraw_set_fullscreen(&g_pd, !g_pd.fullscreen); return 0; }
        break;
    case WM_KEYDOWN:
        if (wp == VK_F11) { present_ddraw_set_fullscreen(&g_pd, !g_pd.fullscreen); return 0; }
        if (wp == VK_F5 && (GetKeyState(VK_CONTROL) & 0x8000)) { host_screenshot(); return 0; }
        /* Ctrl+F8 -> host cursor on/off. It needs a hotkey and not just the menu
           item because FULLSCREEN is exactly when you most want it and exactly when
           there is no menu bar to reach. Swallowed here, like Ctrl+F5, so the guest
           never sees the F-key. */
        if (wp == VK_F8 && (GetKeyState(VK_CONTROL) & 0x8000)) {
            host_cursor_set(h, !g_cursor_show); return 0;
        }
        /* Raw AT keyboard: push the MAKE scancode (lParam bits 16-23 = the OEM scan
           code) into the 0x60/0x64 FIFO and raise IRQ1, so action games that hook
           INT 09h or poll port 0x60 for real-time held-key state get input. This runs
           for every key, alongside the INT 16h ring below (which other games poll). */
        {
            uint8_t rawsc = (uint8_t)((lp >> 16) & 0xFF);
            int ext = (lp & 0x01000000) != 0;
            /* Bit 30 = the key was ALREADY down, i.e. this is OS auto-repeat. We
               generate typematic ourselves (see host_key_typematic), so swallow it:
               two sources would double the repeat rate. Counted, not silently
               dropped -- the count is how we tell "the OS stopped sending them"
               from "we stopped listening". */
            if (lp & 0x40000000) { g_ty_os_repeats++; break; }
            if (rawsc) {
                host_key_scancode(rawsc, ext, 0);
                host_key_typematic_press(rawsc, ext);
            }
        }
        /* NOTHING ELSE TO DO. The scancode above is the whole keystroke: INT 09h translates
           it and fills the BIOS ring in guest memory, which is the single buffer INT 16h and
           a BDA-reading program both look at. This used to ALSO push a keycode straight into
           a host-side ring -- a second, invisible buffer that made INT 16h appear to work
           while 0040:001E stayed empty forever, which is exactly why the Skyroads menu
           ignored every arrow. One key, one path. */
        break;
    case WM_KEYUP:                       /* raw AT keyboard BREAK code + IRQ1      */
        {
            uint8_t rawsc = (uint8_t)((lp >> 16) & 0xFF);
            int ext = (lp & 0x01000000) != 0;
            if (rawsc) {
                host_key_typematic_release(rawsc, ext);   /* stop repeating first */
                host_key_scancode(rawsc, ext, 1);
            }
        }
        break;
    case WM_CHAR:                        /* Windows' translation: not ours to use */
        /* WM_KEYDOWN already delivered this key as a scancode, and INT 09h turns that into
           the BIOS keycode. Pushing the WM_CHAR ascii as well would enter every printable
           key TWICE. (The scancode path is also the only one that can produce the AL=0
           extended codes an arrow needs, which WM_CHAR never generates at all.) */
        return 0;
    case WM_MOUSEMOVE:                   /* map client -> guest pixels, + buttons */
    case WM_LBUTTONDOWN: case WM_LBUTTONUP:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: {
        RECT rc; int cw, ch, fw, fh; LONG b = 0;
        fw = g_vid.frame.w ? (int)g_vid.frame.w : 640;
        fh = g_vid.frame.h ? (int)g_vid.frame.h : 480;
        GetClientRect(h, &rc);
        cw = rc.right; ch = rc.bottom - (g_pd.status_h ? g_pd.status_h : PRESENT_STATUS_H);
        if (cw < 1) cw = 1;
        if (ch < 1) ch = 1;
        { int fx = (short)LOWORD(lp) * fw / cw, fy = (short)HIWORD(lp) * fh / ch;
          if (fx < 0) fx = 0;
          else if (fx >= fw) fx = fw - 1;
          if (fy < 0) fy = 0;
          else if (fy >= fh) fy = fh - 1;
          InterlockedExchange(&g_ms_x, fx); InterlockedExchange(&g_ms_y, fy); }
        if (wp & MK_LBUTTON) b |= 1;
        if (wp & MK_RBUTTON) b |= 2;
        if (wp & MK_MBUTTON) b |= 4;
        InterlockedExchange(&g_ms_btn, b);
        return 0; }
    case WM_DESTROY:
        InterlockedExchange(&g_running, 0);
        /* PANIC-STOP THE SOUND, HERE, BEFORE ANYTHING ELSE UNWINDS.
           Closing the window used to leave notes sounding until the host was
           restarted (user, 2026-08-21). Nothing ever called audio_wave_stop -- it
           existed and had no caller -- so waveOut kept playing, and the mixer kept
           being asked for samples from an OPL whose voices were still keyed on. A
           sustaining voice (EGT=1) holds its level forever by design, so "forever"
           is exactly what you get: the chord under the cursor at the moment you
           clicked the X, indefinitely.
           ORDER MATTERS. Stop the device FIRST: that resets waveOut and ends the
           fill callbacks, so no audio thread is inside vdd_audio_mix when the OPL
           is torn down underneath it. Only then silence the chip. Doing it the
           other way round races the callback for the state it is reading. */
        audio_wave_stop(&g_wave);
        HOST_LOCK();
        vdd_opl_reset(&g_opl);                    /* all voices off, registers clear */
        HOST_UNLOCK();
        if (g_key_event) SetEvent(g_key_event);   /* unblock the V86 thread        */
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

static DWORD WINAPI ui_thread(LPVOID arg)
{
    WNDCLASSA wc; MSG msg; RECT rc;
    HINSTANCE hi = GetModuleHandleA(NULL);
    INITCOMMONCONTROLSEX icc;
    (void)arg;
    icc.dwSize = sizeof icc; icc.dwICC = ICC_WIN95_CLASSES;
    InitCommonControlsEx(&icc);                  /* activate the Luna (v6) context */
    ZeroMemory(&wc, sizeof wc);
    wc.lpfnWndProc = wnd_proc; wc.hInstance = hi;
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hIcon = LoadIconA(hi, MAKEINTRESOURCEA(101));    /* IDI_MAINICON: title bar + taskbar */
    wc.lpszClassName = "NtvdmexHostWindow";
    if (!RegisterClassA(&wc)) return 1;
    rc.left = 0; rc.top = 0; rc.right = VID_FB_W; rc.bottom = VID_FB_H + PRESENT_STATUS_H;
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, TRUE);   /* TRUE: window has a menu  */
    g_hwnd = CreateWindowA(wc.lpszClassName, VDM_WIN_TITLE, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                           CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
                           NULL, NULL, hi, NULL);
    if (!g_hwnd) return 1;
    SetMenu(g_hwnd, build_menu());
    menu_check(g_hwnd, IDM_INPUT_CURSOR, g_cursor_show);  /* tick reflects the state  */
    present_ddraw_init(&g_pd, g_hwnd);          /* GDI windowed; DDraw for fullscreen */
    make_status(g_hwnd, hi);                     /* native themed status bar          */
    ShowWindow(g_hwnd, SW_SHOW); UpdateWindow(g_hwnd);
    SetTimer(g_hwnd, 1, VID_PRESENT_TICK_MS, NULL);  /* fast tick; present is PHASE-gated */
    while (GetMessageA(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    present_ddraw_shutdown(&g_pd);
    return 0;
}

/* --- guest register view <-> VDM_TIB CONTEXT (for bus interrupt dispatch) --- */
static void regs_load(ntvdd_regs *r, volatile BYTE *tib)
{
    r->eax = VDM_REG(tib, VTIB_EAX); r->ebx = VDM_REG(tib, VTIB_EBX);
    r->ecx = VDM_REG(tib, VTIB_ECX); r->edx = VDM_REG(tib, VTIB_EDX);
    r->esi = VDM_REG(tib, VTIB_ESI); r->edi = VDM_REG(tib, VTIB_EDI);
    r->ebp = VDM_REG(tib, VTIB_EBP);
    r->ds = (uint16_t)VDM_REG(tib, VTIB_DS); r->es = (uint16_t)VDM_REG(tib, VTIB_ES);
    r->cf = 0;
}
/* STORE EVERYTHING LOAD READS. This wrote back only the four general registers, so any
   service whose ANSWER is a pointer silently threw that answer away: INT 10h AH=11h AL=30h
   returns the character generator in ES:BP, and the guest got back whatever ES:BP it
   happened to be holding -- so it drew its text out of an arbitrary chunk of memory. That
   is the "garbled text" in Skyroads, and it is why fixing the handler to SET ES:BP (and
   later replacing the font tables themselves) changed nothing: neither value ever reached
   the guest. Same silent loss applied to every ES:DI and DS:SI answer (VESA info blocks,
   INT 33h, INT 10h 1Bh). regs_load already reads all seven, so writing all seven back is
   symmetric: a handler that does not touch one stores the value it was given. */
static void regs_store(ntvdd_regs *r, volatile BYTE *tib)
{
    VDM_REG(tib, VTIB_EAX) = r->eax; VDM_REG(tib, VTIB_EBX) = r->ebx;
    VDM_REG(tib, VTIB_ECX) = r->ecx; VDM_REG(tib, VTIB_EDX) = r->edx;
    VDM_REG(tib, VTIB_ESI) = r->esi; VDM_REG(tib, VTIB_EDI) = r->edi;
    VDM_REG(tib, VTIB_EBP) = r->ebp;
    VDM_REG(tib, VTIB_DS)  = r->ds;  VDM_REG(tib, VTIB_ES)  = r->es;
}

/* Record the first few DISTINCT ports the guest touches that no VDD claims. A game
   hunting for hardware probes a fixed set of addresses, so this names the device it
   wants -- e.g. 0x220-0x22F is a Sound Blaster looking for its DSP. Distinct-only
   and budgeted, so it cannot flood a run. */
/* WHERE THE TIME GOES. 4.5M trapped port accesses in the first 6 s of a Skyroads run is
   ~750k/s, and at ~1.5 us of round trip each that is the entire CPU -- which is exactly what
   the game looks like on screen: everything correct, everything far too slow. Counting by
   PORT says which device is being hammered; capturing the guest CS:IP and the bytes there
   says which INSTRUCTION IDIOM it is, and that is what a fast path has to match. (The
   existing burst only collapses `IN/OUT` + `LOOP rel8`, and io_burst was 4828 of 4.5M, so
   Skyroads' delay loop is plainly a different shape.) */
static void io_hot_note(uint16_t port, DWORD cs, DWORD ip)
{
    int i;
    for (i = 0; i < g_io_hot_n; ++i)
        if (g_io_hot[i].port == port) { g_io_hot[i].n++; goto sited; }
    if (g_io_hot_n < IO_HOT_MAX) {
        g_io_hot[g_io_hot_n].port = port; g_io_hot[g_io_hot_n].n = 1; g_io_hot_n++;
    }
sited:
    if (g_io_site_logged < 6 && cs) {
        static DWORD s_seen[6];
        DWORD k, key = (cs << 16) ^ ip;
        for (k = 0; k < g_io_site_logged; ++k) if (s_seen[k] == key) return;
        s_seen[g_io_site_logged++] = key;
        { char b[192], *q = b;
          const volatile BYTE *cp = (const volatile BYTE *)((cs << 4) + ((ip - 6) & 0xFFFF));
          BYTE tmp[16]; unsigned j;
          for (j = 0; j < 16; ++j) tmp[j] = cp[j];
          q = zput(q, "IO-SITE port=0x"); q = zhex(q, port);
          q = zput(q, " cs:ip=0x");       q = zhex(q, cs);
          q = zput(q, ":0x");             q = zhex(q, ip);
          q = zput(q, " bytes[ip-6..]: "); q = zdump(q, tmp, 16);
          q = zput(q, "\r\n");
          log_append(LOG_PATH, b, q); serial_out(b, q); }
    }
}

static void io_unclaimed_note(uint16_t port, int is_in)
{
    int i;
    (void)is_in;
    for (i = 0; i < g_unclaimed_n; ++i) if (g_unclaimed[i] == port) return;
    if (g_unclaimed_n >= IO_UNCLAIMED_MAX) return;
    g_unclaimed[g_unclaimed_n++] = port;
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
     together than 10 ms because the UI tick is 5 ms. They are not. host_pit_sync
     takes g_lock, and a heavy I/O-trap loop starves the UI thread (that is what the
     comment above the exec-loop call at the bottom of this file is about) -- so gaps
     past 10 ms are ORDINARY, and the cap threw away real time on every one of them.
   ► THE LESSON, which is this project's own cardinal rule wearing a new hat: the old
     burst was at least CORRECT ON AVERAGE. Discarding time is not a smaller version
     of that error, it is a bigger and more constant one. Do not trade an occasional
     artefact for a permanent one.
   ► IF YOU PICK THIS UP: never discard. Keep the total exact and SMOOTH the
     delivery -- carry the backlog and release it at a bounded rate over the next few
     syncs, so the average tempo is preserved and only the lurch is removed. And
     measure the real gap distribution FIRST: that is what g_pit_gap_max and the
     counter below exist for. A second, independent cause is also in play -- with no
     CPU affinity set anywhere, a TSC-backed QueryPerformanceCounter can JUMP FORWARD
     when a thread migrates cores, which is indistinguishable from a stall in here. */
#define PIT_CATCHUP_MAX (PIT_INPUT_HZ / 100u)      /* 10 ms: the reporting threshold */
static uint32_t g_pit_catchup_clamped;             /* gaps past it (STAGE2)          */
static uint32_t g_pit_gap_max;                     /* the worst one, in 8254 clocks  */

static void host_pit_sync(void)
{
    static LARGE_INTEGER s_freq, s_last;
    LARGE_INTEGER now;
    ULONGLONG delta;
    /* Called from BOTH the exec thread (so a guest polling the counter reads real time) and
       the UI thread (so the clock keeps running while the guest is spinning and not trapping
       at all). It must be one shared clock or the two would double-count, hence the lock --
       g_lock is recursive for the exec thread, which already holds it inside host_io_do. */
    HOST_LOCK();
    if (!s_freq.QuadPart) {
        if (QueryPerformanceFrequency(&s_freq) && s_freq.QuadPart)
            QueryPerformanceCounter(&s_last);
        HOST_UNLOCK();
        return;
    }
    if (!QueryPerformanceCounter(&now) || now.QuadPart <= s_last.QuadPart) {
        HOST_UNLOCK();
        return;
    }
    g_async_tried_this_sync = 0;        /* a fresh burst of raises gets one attempt */
    ++g_pit_syncs;
    /* Before this sync's raises are added: how far behind had delivery fallen? */
    pm_owed_sample(g_pm_tick_owed);
    delta = (ULONGLONG)(now.QuadPart - s_last.QuadPart);
    /* clocks = delta * 1193182 / freq, without overflowing: delta is small (microseconds). */
    { ULONGLONG clocks = (delta * PIT_INPUT_HZ) / (ULONGLONG)s_freq.QuadPart;
      if (clocks) {
          /* Consume the WHOLE delta from the clock, then deliver only a bounded part of
             it: past the cap, time is DISCARDED rather than queued up and replayed. */
          s_last.QuadPart += (LONGLONG)((clocks * (ULONGLONG)s_freq.QuadPart) / PIT_INPUT_HZ);
          /* OBSERVE ONLY -- do not act on this. See PIT_CATCHUP_MAX. */
          if (clocks > PIT_CATCHUP_MAX) {
              g_pit_catchup_clamped++;
              if (clocks > g_pit_gap_max) g_pit_gap_max = (uint32_t)clocks;
          }
          if (clocks > PIT_INPUT_HZ) clocks = PIT_INPUT_HZ;   /* cap a long stall at 1 s */
          vdd_pit_add_clocks(&g_pit, (uint32_t)clocks);
      } }
    HOST_UNLOCK();
}

static DWORD g_sndio_logged = 0;    /* bounded SNDIO trace; see the note below */
static void host_io_do(volatile BYTE *tib, vdd_bus *bus, uint16_t port,
                       int is_in, int width)
{
    uint32_t val, eax = VDM_REG(tib, VTIB_EAX);
    g_io_last_port = port;              /* for the hot-port histogram */
    /* ► THE LAST FORK IN THE ECHO. 91% of DMX's 8237 count reads happen outside any
         COOPERATIVE injection -- but "outside cooperative" is two very different
         places: inside an ASYNC-delivered ISR (the async path does something the
         cooperative one does not, and making them equivalent is the fix), or in
         Doom's MAINLINE code (DMX polls at its own rate, the tick path is irrelevant,
         and the fix is somewhere else entirely). The async ISR cannot be bracketed
         the way the cooperative one was -- the guest runs it on its own thread -- but
         g_async_pm_active is set for exactly its duration, from the injection to the
         catcher. Read it here, where the port access is dispatched. */
    if (port == 0x03) { if (g_async_pm_active) ++g_dmapoll_in_async;
                        else                   ++g_dmapoll_mainline; }
    /* Sync the counter before the guest looks at it, so a poll always reads real time. */
    if (port >= 0x40 && port <= 0x43) host_pit_sync();
    /* Report the rate the guest programs. Skyroads divides its own fast timer down to the
       BIOS 18.2 Hz (519 injected IRQ0 vs 32 BIOS ticks last run => ~16:1), so the reload it
       writes is the tempo the music actually wants. */
    if (port == 0x40 && !is_in && g_pit_reload_log < 8) {
        static uint16_t s_prev = 0;
        if (g_pit.reload != s_prev) {
            char b[128], *q = b;
            s_prev = g_pit.reload; g_pit_reload_log++;
            q = zput(q, "PIT-RELOAD 0x"); q = zhex(q, (DWORD)g_pit.reload);
            q = zput(q, " (hz=0x");
            q = zhex(q, g_pit.reload ? (PIT_INPUT_HZ / g_pit.reload) : 18u);
            q = zput(q, ")\r\n");
            log_append(LOG_PATH, b, q); serial_out(b, q);
        }
    }
    if (is_in) {
        /* An unclaimed ISA port floats high: real hardware reads 0xFF, not 0x00.
           This matters for device detection -- a probe that reads 0x00 from an
           absent card can conclude it IS present and then wait forever for a
           response that will never come. */
        val = 0;
        if (!vdd_bus_io(bus, port, (uint8_t)width, 1, &val)) {
            val = 0xFFFFFFFFu;
            io_unclaimed_note(port, 1);
        }
        if (width == 1)      VDM_REG(tib, VTIB_EAX) = (eax & 0xFFFFFF00u) | (val & 0xFF);
        else if (width == 2) VDM_REG(tib, VTIB_EAX) = (eax & 0xFFFF0000u) | (val & 0xFFFF);
        else                 VDM_REG(tib, VTIB_EAX) = val;
    } else {
        val = (width == 1) ? (eax & 0xFF) : (width == 2) ? (eax & 0xFFFF) : eax;
        if (!vdd_bus_io(bus, port, (uint8_t)width, 0, &val)) io_unclaimed_note(port, 0);
    }
    /* ── THE SOUND-CARD HANDSHAKE, IN FULL, FOR AS LONG AS IT LASTS. ─────────────────
         "SB isn't responding at p=0x220, i=7, d=1" is Doom's verdict, not a
         measurement: it says the probe failed, not which step of it did. The DSP reset
         is a four-step conversation (write 1 to base+6, write 0, poll base+0xE for
         bit 7, read base+0xA for 0xAA) and any one of them can be the miss.
         Bounded to the first 300 accesses, which is far more than a probe needs and far
         less than a playing game produces -- the point is the OPENING of the
         conversation, and after that the per-block counters in STAGE2 take over.
         `io_hot_note` cannot serve here: it is only called on the V86 arm of the exec
         loop, so for a protected-mode client like Doom it records nothing at all --
         which is why STAGE2's "hot ports:" line came back empty from a run that had
         plainly done thousands of port accesses. */
    if (g_sndio_logged < 300
        && ((port >= 0x220 && port <= 0x22F)      /* Sound Blaster            */
         || (port >= 0x388 && port <= 0x389)      /* AdLib / OPL              */
         || (port >= 0x330 && port <= 0x331))) {  /* MPU-401 MIDI             */
        char sb2[128], *sq = sb2;
        ++g_sndio_logged;
        sq = zput(sq, "SNDIO "); sq = zput(sq, is_in ? "in  0x" : "out 0x");
        sq = zhex(sq, port);
        sq = zput(sq, is_in ? " -> 0x" : " <- 0x");
        sq = zhex(sq, val);
        sq = zput(sq, " w="); sq = zhexb(sq, (unsigned)width);
        sq = zput(sq, " ms="); sq = zhex(sq, GetTickCount());
        sq = zput(sq, "\r\n"); log_append(LOG_PATH, sb2, sq); serial_out(sb2, sq);
    }
}

/* Burst fast path for the `<I/O insn>; LOOP <back to it>` idiom -- the same
   trick as the mode-12h fill-loop interpreter below, applied to port I/O.

   Every guest IN/OUT is an IOPL-0 #GP reflected out to this user-mode monitor,
   which measures ~40x the cost of the ISA access it stands in for (iobench.com).
   DOS sound code leans on that idiom hard: an AdLib register write is `OUT` the
   address, 6 dummy reads, `OUT` the data, then `mov cx,35 / in al,dx / loop $-1`
   to satisfy the OPL's write-settling time -- 43 trapped accesses per register,
   which is what left Skyroads grinding in its OPL init instead of reaching video.
   Since the loop body is EXACTLY the one I/O instruction, we can run the rest of
   the iterations here, one bus call each, and pay a single trap for all of them.

   `io_start` is the offset of the I/O instruction and `ip_next` the offset of the
   insn after it; we only fire when a LOOP sits at `ip_next` and jumps back to
   precisely `io_start`, so the body cannot contain anything we would skip.

   Accounting is exact and needs NO EIP fixup: the guest is left about to execute
   the LOOP, so if it enters with CX=c the body still runs c-1 more times. We run
   n of those here and subtract n from CX, leaving c-n; the guest then runs
   (c-n)-1 itself, for n + (c-n-1) = c-1 total. Capping n (rather than draining
   the loop) also keeps IRQ latency bounded: a `mov cx,0xFFFF` delay loop still
   re-enters this monitor every IO_BURST_MAX accesses instead of once at the end.
   Returns the number of extra accesses performed (0 = idiom not present). */
#define IO_BURST_MAX 4096
static DWORD host_io_loop_burst(volatile BYTE *tib, vdd_bus *bus,
                                volatile const BYTE *seg, DWORD ip_next,
                                DWORD io_start, uint16_t port,
                                int is_in, int width, int is32)
{
    /* A 16-bit segment wraps offsets (and LOOP counts) at 64K; a 32-bit flat one
       does not, and there LOOP counts in ECX. One mask drives both. */
    DWORD cx, n, mask = is32 ? 0xFFFFFFFFu : 0xFFFFu;
    int disp;
    if (seg[ip_next & mask] != 0xE2) return 0;           /* LOOP rel8 only        */
    disp = (signed char)seg[(ip_next + 1) & mask];
    if (((ip_next + 2 + disp) & mask) != (io_start & mask)) return 0;
    cx = VDM_REG(tib, VTIB_ECX) & mask;
    n  = (cx - 1) & mask;                                /* body runs c-1 more    */
    if (n > IO_BURST_MAX) n = IO_BURST_MAX;
    if (!n) return 0;
    VDM_REG(tib, VTIB_ECX) = is32 ? (cx - n)
                                  : ((VDM_REG(tib, VTIB_ECX) & 0xFFFF0000u) |
                                     ((cx - n) & 0xFFFF));
    { DWORD i; for (i = 0; i < n; ++i) host_io_do(tib, bus, port, is_in, width); }
    g_io_extra += n;
    return n;
}

/* Decode + service a V86 IN/OUT that #GP-faulted (event 2), dispatch it to the
   bus, and advance EIP past the instruction so the guest resumes. Returns 1 if
   the faulting instruction was a (supported) I/O op we handled, 0 if it was a
   genuine GP fault the caller should stop on. No per-call logging -- I/O traps
   are hot (a palette set is ~768 OUTs); flushing the trace file each one stalls. */
static int host_try_io(volatile BYTE *tib, vdd_bus *bus)
{
    DWORD cs = VDM_REG(tib, VTIB_CS)  & 0xFFFF;
    DWORD ip = VDM_REG(tib, VTIB_EIP) & 0xFFFF;
    volatile BYTE *code = (volatile BYTE *)((cs << 4) + ip);   /* absolute V86 */
    int i = 0, opsize = 2, is_in, width, used_dx, len;
    BYTE op; uint16_t port;

    while (code[i] == 0x66 || code[i] == 0x67 ||
           code[i] == 0xF2 || code[i] == 0xF3) {        /* prefixes            */
        if (code[i] == 0x66) opsize = 4;
        if (++i > 4) return 0;
    }
    op = code[i];
    switch (op) {
    case 0xE4: is_in = 1; width = 1;      used_dx = 0; break;  /* IN  AL,ib    */
    case 0xE5: is_in = 1; width = opsize; used_dx = 0; break;  /* IN  eAX,ib   */
    case 0xE6: is_in = 0; width = 1;      used_dx = 0; break;  /* OUT ib,AL    */
    case 0xE7: is_in = 0; width = opsize; used_dx = 0; break;  /* OUT ib,eAX   */
    case 0xEC: is_in = 1; width = 1;      used_dx = 1; break;  /* IN  AL,DX    */
    case 0xED: is_in = 1; width = opsize; used_dx = 1; break;  /* IN  eAX,DX   */
    case 0xEE: is_in = 0; width = 1;      used_dx = 1; break;  /* OUT DX,AL    */
    case 0xEF: is_in = 0; width = opsize; used_dx = 1; break;  /* OUT DX,eAX   */
    default:   return 0;                       /* not an I/O op -> real fault  */
    }
    if (used_dx) { port = (uint16_t)VDM_REG(tib, VTIB_EDX); len = i + 1; }
    else         { port = code[i + 1];                      len = i + 2; }

    host_io_do(tib, bus, port, is_in, width);
    VDM_REG(tib, VTIB_EIP) = (ip + len) & 0xFFFF;          /* step past I/O    */
    /* ...and if a LOOP over this very instruction follows, drain it here. */
    host_io_loop_burst(tib, bus, (volatile const BYTE *)(cs << 4),
                       (ip + len) & 0xFFFF, ip, port, is_in, width, 0);
    return 1;
}

/* Retro I/O servicer for the real-hardware event-3 reflect. On this XP box an IOPL-0
   IN/OUT #GP is reflected as VTIB_EVENT=3 with CS:IP pointing at the instruction AFTER
   the faulting IN/OUT (EIP already advanced past it) -- so host_try_io, which decodes
   AT CS:IP, sees the next op (e.g. a MOV in Skyroads' vblank poll) and declines. Here
   we decode the IN/OUT that ENDS at CS:IP and service it WITHOUT advancing EIP: a DX-form
   (1 byte: EC/ED/EE/EF at IP-1, optional 66 prefix at IP-2) or an imm-form (2 bytes:
   E4-E7 at IP-2, port imm at IP-1). Returns 1 if serviced. */
static int host_try_io_retro(volatile BYTE *tib, vdd_bus *bus)
{
    DWORD cs = VDM_REG(tib, VTIB_CS) & 0xFFFF;
    DWORD ip = VDM_REG(tib, VTIB_EIP) & 0xFFFF;
    volatile BYTE *seg = (volatile BYTE *)(cs << 4);
    BYTE op; int is_in, width, opsize = 2; uint16_t port; DWORD io_start;
    if (ip < 1) return 0;
    op = seg[ip - 1];
    if (op == 0xEC || op == 0xED || op == 0xEE || op == 0xEF) {   /* DX-form (1 byte) */
        io_start = ip - 1;
        if (ip >= 2 && seg[ip - 2] == 0x66) { opsize = 4; io_start = ip - 2; }
        is_in = (op == 0xEC || op == 0xED);
        width = (op == 0xEC || op == 0xEE) ? 1 : opsize;
        port  = (uint16_t)VDM_REG(tib, VTIB_EDX);
    } else if (ip >= 2 && ((op = seg[ip - 2]) == 0xE4 || op == 0xE5 ||
                            op == 0xE6 || op == 0xE7)) {          /* imm-form (2 byte) */
        io_start = ip - 2;
        is_in = (op == 0xE4 || op == 0xE5);
        width = (op == 0xE4 || op == 0xE6) ? 1 : opsize;
        port  = seg[ip - 1];                                      /* imm8 port        */
    } else {
        return 0;                                                 /* no I/O ends here */
    }
    host_io_do(tib, bus, port, is_in, width);
    /* EIP is already past the I/O, so the guest is sitting on whatever follows --
       if that is a LOOP back to this same I/O (the OPL write-delay idiom), drain
       the iterations here rather than paying one #GP reflect per read. */
    host_io_loop_burst(tib, bus, seg, ip, io_start, port, is_in, width, 0);
    return 1;                              /* EIP already past the I/O -- do NOT advance */
}

/* Service a REP INS/OUTS reflected as event 1 (string I/O) -- how the VGA palette
   is loaded (`rep outsb` to 0x3C9) and how several sound drivers push blocks.
   The kernel hands us a decoded descriptor in the words after VTIB_EVENT, but we
   deliberately work from the guest's own SI/DI/CX/DF and the instruction bytes at
   CS:IP instead: those fields are already established, whereas the descriptor's
   layout is inferred from a single observation. (The two agreed on the run that
   found this: port 0x3C9, count 0x40, buffer 0100:0355.)

   Unlike the other servicers, CS:IP points AT the instruction here. We transfer at
   most IO_BURST_MAX units per reflect and only step past the instruction once CX
   drains -- leaving EIP on the REP otherwise, exactly as a real CPU resumes an
   interrupted string op, which keeps a 64K transfer from monopolising the monitor. */
static int host_try_io_string(volatile BYTE *tib, vdd_bus *bus)
{
    DWORD cs = VDM_REG(tib, VTIB_CS)  & 0xFFFF;
    DWORD ip = VDM_REG(tib, VTIB_EIP) & 0xFFFF;
    volatile BYTE *seg = (volatile BYTE *)(cs << 4);
    int i = 0, opsize = 2, is_in, width, rep = 0;
    DWORD sover = 0, count, n, step, k;
    uint16_t port;
    BYTE op;

    for (;;) {                                   /* prefixes                        */
        op = seg[(ip + i) & 0xFFFF];
        if      (op == 0x66) opsize = 4;
        else if (op == 0xF2 || op == 0xF3) rep = 1;
        else if (op == 0x26) sover = VTIB_ES;    /* segment overrides on the source */
        else if (op == 0x2E) sover = VTIB_CS;
        else if (op == 0x36) sover = VTIB_SS;
        else if (op == 0x3E) sover = VTIB_DS;
        else if (op != 0x67) break;
        if (++i > 4) return 0;
    }
    switch (op) {
    case 0x6C: is_in = 1; width = 1;      break;              /* INSB             */
    case 0x6D: is_in = 1; width = opsize; break;              /* INSW / INSD      */
    case 0x6E: is_in = 0; width = 1;      break;              /* OUTSB            */
    case 0x6F: is_in = 0; width = opsize; break;              /* OUTSW / OUTSD    */
    default:   return 0;                                      /* not a string I/O */
    }
    port  = (uint16_t)VDM_REG(tib, VTIB_EDX);
    count = rep ? (VDM_REG(tib, VTIB_ECX) & 0xFFFF) : 1;
    if (!count) {                                             /* REP with CX=0    */
        VDM_REG(tib, VTIB_EIP) = (ip + i + 1) & 0xFFFF;
        return 1;
    }
    step = (VDM_REG(tib, VTIB_EFLAGS) & 0x400) ? (DWORD)-width : (DWORD)width;  /* DF */
    n = (count > IO_BURST_MAX) ? IO_BURST_MAX : count;

    for (k = 0; k < n; ++k) {
        uint32_t val = 0;
        if (is_in) {                                          /* port -> ES:DI    */
            DWORD di = VDM_REG(tib, VTIB_EDI) & 0xFFFF;
            DWORD lin = ((VDM_REG(tib, VTIB_ES) & 0xFFFF) << 4) + di;
            vdd_bus_io(bus, port, (uint8_t)width, 1, &val);
            pokew_n(lin, val, width);
            VDM_SET16(tib, VTIB_EDI, (WORD)(di + step));
        } else {                                              /* DS:SI -> port    */
            DWORD si = VDM_REG(tib, VTIB_ESI) & 0xFFFF;
            DWORD sregoff = sover ? sover : VTIB_DS;
            DWORD lin = ((VDM_REG(tib, sregoff) & 0xFFFF) << 4) + si;
            val = peekw_n(lin, width);
            vdd_bus_io(bus, port, (uint8_t)width, 0, &val);
            VDM_SET16(tib, VTIB_ESI, (WORD)(si + step));
        }
    }
    if (rep) {
        VDM_SET16(tib, VTIB_ECX, (WORD)(count - n));
        if (count - n) return 1;                 /* more to go: resume ON the REP  */
    }
    VDM_REG(tib, VTIB_EIP) = (ip + i + 1) & 0xFFFF;
    return 1;
}

/* True if selector `sel`'s descriptor has the D/B (32-bit default) bit set. The bit
   lives in g_ldt[].flags bit 2 (descriptor byte-6 bit 6). All 16-bit DPMI clients leave
   it 0; a DOS/4GW-class 32-bit code selector sets it (via INT 31h 0009). */
static int dpmi_sel_is32(WORD sel)
{
    int idx = (sel & 0xFFFF) >> 3;
    if (idx < 1 || idx >= 512) return 0;
    return (g_ldt[idx].flags & 0x4) != 0;
}

/* PM variant of host_try_io (GH #18 run 72). A real-CPU PROTECTED-MODE IN/OUT is
   trapped by the kernel and reflected to us as VTIB_EVENT=0 -- the SAME I/O event as
   V86 (VM-confirmed by outprobe.com: a PM `OUT DX,AL` to 0x3C8 stops with event=0 on
   the instruction). Only the addressing differs: the faulting insn is at the code
   selector's LINEAR BASE + EIP, not V86 `CS<<4:IP`. Decode + dispatch through the same
   VDD bus, then step the guest past it, so PM port I/O (VGA/sound) reaches our VDDs.
   #3 (32-bit DPMI): the code selector's D/B bit sets the DEFAULT operand size -- 16-bit
   for a 16-bit segment (0x66 => 4), 32-bit for a DOS/4GW flat segment (0x66 => 2). We
   read it per-selector and offset EIP by its full 32-bit value when D=1, so this decoder
   serves both classes; for every existing D=0 client the behaviour is unchanged. */
#define DMAPOLL_MAX 8
static DWORD g_dmapoll_eip[DMAPOLL_MAX], g_dmapoll_hits[DMAPOLL_MAX];
static unsigned g_dmapoll_n = 0, g_dmapoll_overflow = 0;
/* ── WHO CALLS THE POLL? THE STACK KNOWS, AND THE IMAGE DOES NOT. ────────────────────
     DMX dispatches through a card-driver vtable -- four position routines of identical
     shape, one per sound card -- and in an LE image those entries are FIXUP RECORDS, so
     the pointer is simply not in the file. Searching for it statically returned nothing
     under every plausible encoding, which is why the caller chain is unknown.
     At the instant of the poll, though, the guest's own stack holds the return chain.
     Take the top of it and keep every word that looks like a code address, i.e. lands
     near the poll site itself (the whole code object is ~0x45000 bytes, so a +/-0x60000
     window cannot miss it and cannot admit heap or ring data). The union over a whole
     run is the set of call sites, and each converts to a file offset by subtracting
     0x03AEDFEC -- at which point DMX's refill path can be READ instead of inferred.
   ► WHAT THIS IS FOR. `getpos`'s caller computes `total - remaining` and is gated on an
     "is this transfer active" flag: that is the shape of a position REPORT, not
     necessarily the refill trigger. If DMX refills on the SB block IRQ instead -- which
     arrives at 99.5% -- then the 56/s poll rate has nothing to do with the 30% stale
     blocks and the 31%-vs-30% agreement is a coincidence. This decides that, and it is
     the difference between a cause and a pattern match. */
#define POLLSTK_MAX 48
static DWORD g_pollstk[POLLSTK_MAX], g_pollstk_hits[POLLSTK_MAX];
static DWORD g_pollgap[10], g_pollgap_max_us = 0;
static unsigned g_pollstk_n = 0, g_pollstk_overflow = 0;

static int host_try_io_pm(volatile BYTE *tib, vdd_bus *bus)
{
    DWORD csv = VDM_REG(tib, VTIB_CS)  & 0xFFFF;
    DWORD eip = VDM_REG(tib, VTIB_EIP);
    int is32 = dpmi_sel_is32((WORD)csv);
    DWORD eip_off = is32 ? eip : (eip & 0xFFFF);
    volatile BYTE *code = (volatile BYTE *)(ULONG_PTR)(dpmi_sel_base((WORD)csv) + eip_off);
    int i = 0, opsize = is32 ? 4 : 2, is_in, width, used_dx, len;
    BYTE op; uint16_t port;

    while (code[i] == 0x66 || code[i] == 0x67 ||
           code[i] == 0xF2 || code[i] == 0xF3) {        /* prefixes            */
        if (code[i] == 0x66) opsize = is32 ? 2 : 4;     /* 0x66 flips the segment default */
        if (++i > 4) return 0;
    }
    op = code[i];
    switch (op) {
    case 0xE4: is_in = 1; width = 1;      used_dx = 0; break;  /* IN  AL,ib    */
    case 0xE5: is_in = 1; width = opsize; used_dx = 0; break;  /* IN  eAX,ib   */
    case 0xE6: is_in = 0; width = 1;      used_dx = 0; break;  /* OUT ib,AL    */
    case 0xE7: is_in = 0; width = opsize; used_dx = 0; break;  /* OUT ib,eAX   */
    case 0xEC: is_in = 1; width = 1;      used_dx = 1; break;  /* IN  AL,DX    */
    case 0xED: is_in = 1; width = opsize; used_dx = 1; break;  /* IN  eAX,DX   */
    case 0xEE: is_in = 0; width = 1;      used_dx = 1; break;  /* OUT DX,AL    */
    case 0xEF: is_in = 0; width = opsize; used_dx = 1; break;  /* OUT DX,eAX   */
    default:   return 0;                       /* not an I/O op -> real fault  */
    }
    if (used_dx) { port = (uint16_t)VDM_REG(tib, VTIB_EDX); len = i + 1; }
    else         { port = code[i + 1];                      len = i + 2; }

    /* ── WHERE IN THE GUEST IS THE DMA POLL? A LOCATOR, NOT A HYPOTHESIS. ────────────
         DMX refills from its timer ISR and steers by the 8237's channel-1 count, and
         ~23 of the 135 ticks a second we deliver enter its handler and return without
         ever reading that count. The dispatcher is not what drops them -- it ALWAYS
         calls the registered handler (DOOM.EXE file 0x554f4, disassembled) -- so the
         decision is inside DMX's own routine, whose address is runtime data and cannot
         be read out of the image.
         The host, however, sees the instruction. Record the guest EIP of the reads of
         port 3, and the map `guest = file + 0x03AEDFEC` (verified on DMX's IRQ0 stub
         and Doom's keyboard ISR) turns it straight into a file offset to disassemble.
         The overflow is counted, so a too-small table cannot pass as a complete answer. */
    if (is_in && port == 0x03) {
        DWORD site = dpmi_sel_base((WORD)csv) + eip_off;
        unsigned s;
        for (s = 0; s < g_dmapoll_n; ++s) if (g_dmapoll_eip[s] == site) break;
        if (s < g_dmapoll_n) g_dmapoll_hits[s]++;
        else if (g_dmapoll_n < DMAPOLL_MAX) {
            g_dmapoll_eip[g_dmapoll_n] = site; g_dmapoll_hits[g_dmapoll_n] = 1;
            ++g_dmapoll_n;
        } else ++g_dmapoll_overflow;

        /* ── WHY IS THE MIXER RUN ONLY 56 TIMES A SECOND? TWO CAUSES, ONE SHAPE EACH.
             DMX's scheduler (DOOM.EXE file 0x57224) runs a task when the tick clock
             reaches its deadline, ABANDONS THE WHOLE PASS if the task is still busy,
             and recomputes the deadline from NOW so a missed run is never made up.
             (a) the mixer overruns the 7.4 ms tick period, so the next tick finds it
                 busy -> the gaps between polls are LONG and irregular.
             (b) we deliver ticks in bursts; each burst tick advances DMX's clock but
                 only one task run happens per ISR entry -> the gaps are SHORT and
                 regular, and the loss is in the bunching, not the duration.
             A rate cannot tell those apart -- both give 56/s -- so bucket the actual
             interval. QPC because at these scales GetTickCount's 10-16 ms granularity
             is the same size as the effect. */
        { static LARGE_INTEGER pf, prev;
          LARGE_INTEGER now;
          if (!pf.QuadPart) QueryPerformanceFrequency(&pf);
          if (pf.QuadPart && QueryPerformanceCounter(&now)) {
              if (prev.QuadPart) {
                  LONGLONG dus = ((now.QuadPart - prev.QuadPart) * 1000000) / pf.QuadPart;
                  unsigned b = 0;
                  while (b < 9 && dus >= (LONGLONG)1000 << b) ++b;   /* 1,2,4..256ms+ */
                  g_pollgap[b]++;
                  if (dus > (LONGLONG)g_pollgap_max_us) g_pollgap_max_us = (DWORD)dus;
              }
              prev = now;
          } }

        /* The return chain, straight off the guest's stack. */
        { DWORD ssb = dpmi_sel_base((WORD)(VDM_REG(tib, VTIB_SS) & 0xFFFF));
          DWORD esp = VDM_REG(tib, VTIB_ESP);
          const volatile DWORD *sp = (const volatile DWORD *)(ULONG_PTR)(ssb + esp);
          unsigned k;
          for (k = 0; k < 40; ++k) {
              DWORD w = sp[k], dlt = (w > site) ? (w - site) : (site - w);
              unsigned t;
              if (dlt > 0x60000u) continue;             /* not a code address */
              for (t = 0; t < g_pollstk_n; ++t) if (g_pollstk[t] == w) break;
              if (t < g_pollstk_n) g_pollstk_hits[t]++;
              else if (g_pollstk_n < POLLSTK_MAX) {
                  g_pollstk[g_pollstk_n] = w; g_pollstk_hits[g_pollstk_n] = 1;
                  ++g_pollstk_n;
              } else ++g_pollstk_overflow;
          } }
    }

    host_io_do(tib, bus, port, is_in, width);
    /* step past the I/O insn. 16-bit client (D=0): advance the low word, keep high.
       32-bit client (D=1): advance the full EIP. */
    VDM_REG(tib, VTIB_EIP) = is32 ? (eip + len) : ((eip & 0xFFFF0000u) | ((eip + len) & 0xFFFF));
    /* Same LOOP-drain as V86 (a PM sound driver runs the identical OPL idiom).
       With D/B=1 the LOOP counts in ECX, so the counter width follows the selector. */
    host_io_loop_burst(tib, bus, (volatile const BYTE *)(ULONG_PTR)dpmi_sel_base((WORD)csv),
                       eip_off + len, eip_off, port, is_in, width, is32);
    return 1;
}

/* ══ MODE-Y PLANE BACKING: POINT A0000 AT THE PLANE THE MASK SELECTS ═════════════════
 *
 *  Mode Y cannot be de-interleaved after the fact. The A0000 aperture is one flat
 *  buffer, so a guest write lands there with no record of which plane the map mask had
 *  selected, and six reconstruction rules were measured against captured frames without
 *  finding a good one -- every one trades horizontal resolution against stale content
 *  (see modey_flush() in vdd_video.c for the numbers). The information is simply not in
 *  the aperture.
 *
 *  So stop reconstructing it: give each plane its own memory and make A0000 BE the
 *  selected plane. A guest write then lands in the right plane by construction, and the
 *  renderer reads four planes that were never mixed.
 *
 *  ► THIS IS ONLY POSSIBLE BECAUSE A0000 IS ITS OWN ALLOCATION. Measured:
 *        A0000 region: alloc_base=0xa0000 size=0x20000 type=MEM_MAPPED
 *    AllocationBase IS 0xA0000 and it is already a section view, so it can be unmapped
 *    and replaced. A section cannot be mapped into the middle of a larger reservation,
 *    and that is what would have killed this idea.
 *
 *  ► THE WINDOW IS 128K AND ONLY THE FIRST HALF IS PLANAR. B0000-BFFFF is the text and
 *    mono window -- B8000 is the colour text page the VDD reads through `vmem+0x18000`
 *    -- so it gets its own section, mapped once and left alone. Unmapping the original
 *    view frees all 128K, so B0000 has to be re-established and its CONTENTS RESTORED
 *    before anything looks at them.
 *
 *  ► A MULTI-PLANE MASK CANNOT BE ONE MAPPING. Doom writes 0x0F 107 times a run, for
 *    its screen clear. Those windows get a scratch section, and its contents are fanned
 *    out to every plane the mask selected when the mask next moves.
 *
 *  ► REMAPPING HAPPENS ONLY ON THE CPU THREAD, inside the I/O trap that serviced the
 *    map-mask write, i.e. with the guest stopped. The renderer runs on the UI thread and
 *    only ever READS the host-side views, which stay mapped whatever is at A0000.
 */
#define MODEY_WIN   0x10000u                 /* 64K: A0000..AFFFF and B0000..BFFFF   */
#define MODEY_NSEC  6                        /* 0-3 planes, 4 chained/linear, 5 scratch */
static HANDLE g_ysec[MODEY_NSEC];
static void  *g_yview[MODEY_NSEC];           /* host-side views, always mapped       */
static HANDLE g_bsec;
static BYTE   g_yseed[MODEY_WIN];            /* scratch contents as it was seeded    */
static int    g_yremap      = 0;             /* the window is ours                   */
static int    g_ycur        = -1;            /* section index currently at A0000     */
static int    g_yprev_mask  = 0;             /* mask live while the scratch was up   */
static DWORD  g_yswaps = 0, g_yfanouts = 0, g_yfail = 0;
/* ── WHERE DOES A MAP-MASK WRITE GO IF IT DOES NOT MOVE THE WINDOW? ──────────────────
     The last run wrote the map mask 2,042,942 times and swapped 1,867,689 times: 175,253
     writes -- 8.6% -- did not move the window, and nothing says which of the four ways
     that can happen they took. Two of those ways are harmless (the guest rewrote the
     mask it already had; the window was already on that plane) and two would strand the
     window on the WRONG plane, which is exactly the shape a four-way collapse needs:
     a mask change that is dropped means the next store lands in the plane the PREVIOUS
     mask selected.
   ► SO CLOSE THE ARITHMETIC. Every map-mask write must land in exactly one bucket:
         mask_writes = sel_calls + mask_skip_same + mask_skip_chain4
         sel_calls   = swaps + sel_same + sel_zero + failed
     A residual in either line is a path nobody has accounted for. This is deliberately
     an IDENTITY rather than a rate: a rate cannot show a shape, and 8.6% has no shape. */
static DWORD  g_ysel_calls = 0;   /* modey_remap_select() entered with the remap live  */
static DWORD  g_ysel_same  = 0;   /* ...and the window was already where it wanted     */
static DWORD  g_ysel_zero  = 0;   /* ...and the mask selected no plane at all          */
/* ── DOES THE FAN-OUT ITSELF CREATE THE STATUS BAR'S FOUR-WAY COLLAPSE? ──────────────
     `bar_planes_equal` says ~1709 of 2560 bar offsets hold the SAME byte in all four
     planes, and the fan-out is the only path in this host that writes ONE byte to
     SEVERAL planes -- so it is the obvious suspect. Session 22 believed it had ruled
     that out: it disabled the fan-out entirely and the bar was "still 58% wrong".
   ⚠ THAT ELIMINATION DOES NOT HOLD, AND THE REASON IS THE METRIC. With the fan-out
     off, a multi-plane write reaches NO plane at all -- so the bar is wrong because it
     is UNWRITTEN rather than wrong because it is COLLAPSED, and a percentage of
     differing PIXELS scores those two identically. The experiment changed one wrong
     picture for a different wrong picture and the number could not tell them apart.
     (Same shape as `underruns=0` and `blocks_replayed`: a metric that cannot separate
     two failure modes is not evidence about which one is happening.)
   ► SO COUNT THE THING ITSELF, not a proxy: how many DISTINCT bar offsets does the
     fan-out write under a multi-bit mask, split by the two row bands session 23 showed
     behave differently (168-183, which no latch burst ever reaches, and 184-199, which
     they all do). If this lands near 1709 the fan-out IS the collapse; if it lands near
     zero the path is exonerated properly this time, by a number that could have said
     otherwise. One bit per (page, bar offset) = 960 bytes, set in a loop that already
     runs only over CHANGED bytes. */
#define YBAR_OFF_LO   (168u * 80u)      /* first bar byte within a page */
#define YBAR_OFF_MID  (184u * 80u)      /* band split: rows 184..199    */
#define YBAR_OFF_HI   (200u * 80u)
#define YBAR_PER_PAGE (YBAR_OFF_HI - YBAR_OFF_LO)          /* 2560 */
static BYTE  g_yfan_bar_seen[(3u * YBAR_PER_PAGE + 7u) / 8u];
static DWORD g_yfan_bar_writes[2];      /* fan-out writes to bar bytes, by band  */
static DWORD g_yfan_bar_distinct[2];    /* ...distinct offsets, by band          */
static DWORD g_yfan_bar_4way[2];        /* ...of which the mask was all four     */
/* ── IS THE GUEST WRITING THE SAME BYTES TO EVERY PLANE? ─────────────────────────────
     Everything else is now excluded by measurement: the fan-out writes 0 bar bytes, the
     latch bursts change the oracle by under a point when delivered, and the render is
     innocent (plane-vs-WAD matches screen-vs-WAD to the digit). What remains is the
     guest's own stores under single-plane masks -- and `bandprof.py` says all four
     planes hold PHASE-1 data at 54% (band A) to 80% (band B) of bar offsets against a
     reference uniformity of 12%.
     There are only two ways that happens. Either Doom writes four different byte
     streams and we misdirect them onto one plane's worth of content, or Doom writes the
     SAME stream four times because its per-plane source offset never advances. Those
     need completely different fixes and no measurement so far separates them.
   ► SO SAMPLE THE OUTGOING PLANE. A0000 maps exactly one plane at a time, so a store
     can only reach the plane that is mapped; take a fixed 32-byte window of the bar
     from a plane just before we swap away from it, and compare it with the last window
     taken from a DIFFERENT plane -- but only count comparisons where the plane's own
     content actually CHANGED since we last looked, or "identical" would mostly mean
     "nobody wrote anything".
       cross_same  two different planes were each written and received IDENTICAL bytes
       cross_diff  ...and received different bytes
     High cross_same means the fault is upstream of the planes entirely: the guest is
     writing one stream four times, and the question becomes whether a store lands under
     the mask Doom believes it set. Near-zero means the planes receive distinct data and
     the collapse is created somewhere we have not looked yet.
     Two windows, one per band, because the bands differ in intensity (54% vs 80%) and
     a single sample point cannot show that. Two 32-byte compares per swap.

   ⚠⚠ AND THAT INSTRUMENT COULD NOT HAVE SAID OTHERWISE -- IT IS THE SAME MISTAKE AS
     SESSION 23'S p0-vs-p2 CONTROL. `same` demanded that ALL 32 bytes match. The
     collapse this is testing for means roughly 67% per-byte agreement (that is what
     `bar_planes_equal` measures), and 0.668^32 = 2.5e-6 -- so under the hypothesis the
     window-level counter should read ~0 same out of 246, which is what it read
     (30/246). "cross_diff dominates" was therefore NOT evidence that the planes receive
     distinct data; it is what BOTH hypotheses predict, and the exclusion built on it is
     withdrawn.
   ► COUNT BYTES, NOT WINDOWS, and make the number DIRECTLY COMPARABLE to the two
     figures that already exist: bar_planes_equal (66.8% of bar offsets agree across all
     four planes) and bandprof.py's reference uniformity (~12% for an intact bar). An
     all-or-nothing predicate over a 256-byte window can only ever report "different";
     a per-byte rate lands between those two numbers and picks a side.
       cross_eqb/cross_totb  bytes agreeing between the outgoing plane and the last
                             window written by a DIFFERENT plane
       p1_eq/p1_tot[pl]      bytes of plane `pl`'s window agreeing with plane 1's last
                             window -- the hypothesis names plane 1 specifically, so ask
                             about plane 1 specifically rather than about "some other
                             plane"
     256 bytes rather than 32: the windows are sampled only when the plane's bar content
     actually CHANGED (247 times a run, measured), so the compare is free and a wider
     window is a tighter rate. */
#define YSMP_LEN 256u
#define YSMP_A   (176u * 80u)      /* band A, rows 168-183 (256B spans rows 176-179) */
#define YSMP_B   (192u * 80u)      /* band B, rows 184-199 (256B spans rows 192-195) */
static BYTE  g_ysmp[2][4][YSMP_LEN];      /* [band][plane] last seen                */
static int   g_ysmp_have[2][4];
static BYTE  g_ysmp_last[2][YSMP_LEN];    /* last CHANGED window, any plane         */
static int   g_ysmp_last_pl[2] = { -1, -1 };
static DWORD g_ysmp_cross_same[2], g_ysmp_cross_diff[2], g_ysmp_writes[2];
static DWORD g_ysmp_cross_eqb[2], g_ysmp_cross_totb[2];
static DWORD g_ysmp_p1_eq[2][4], g_ysmp_p1_tot[2][4];
/* ── ⚠ AND `cross_eqb` IS A STATE MEASUREMENT, NOT A DELIVERY ONE. ───────────────────
     It compares the WHOLE 256-byte window whenever ANY byte of it changed, so 255 of
     those bytes can be stale content left by an earlier event. That makes it very
     nearly `bar_planes_equal` computed a second way -- which is why it landed on 56/79%
     against that number's 66.8% -- and it is NOT independent evidence that the guest
     DELIVERED collapsed bytes. Same family of error as the all-or-nothing predicate it
     replaced: I read a number as answering a question it does not address.
   ► THE DELIVERY MEASUREMENT IS THE CHANGED BYTES ONLY. For each byte this pass
     actually wrote, was the value already present in the other plane? Stale bytes are
     excluded by construction, so a high rate means the guest HANDED us the same byte for
     two different planes -- which state cannot fake. */
static DWORD g_ysmp_dlv_eq[2], g_ysmp_dlv_tot[2];
static void ysmp_check(int band, unsigned off, int pl)
{
    const BYTE *v = (const BYTE *)g_yview[pl] + off;
    unsigned i;
    BYTE prev[YSMP_LEN];
    int had = g_ysmp_have[band][pl];
    int changed = !had;
    for (i = 0; i < YSMP_LEN && !changed; ++i)
        if (g_ysmp[band][pl][i] != v[i]) changed = 1;
    if (!changed) return;                       /* nobody wrote this window */
    for (i = 0; i < YSMP_LEN; ++i) prev[i] = g_ysmp[band][pl][i];
    /* Against plane 1 BEFORE this window is stored, so pl==1 compares with its own
       previous content (a self-consistency baseline) rather than with itself. */
    if (g_ysmp_have[band][1]) {
        for (i = 0; i < YSMP_LEN; ++i) {
            g_ysmp_p1_tot[band][pl]++;
            if (g_ysmp[band][1][i] == v[i]) g_ysmp_p1_eq[band][pl]++;
        }
    }
    for (i = 0; i < YSMP_LEN; ++i) g_ysmp[band][pl][i] = v[i];
    g_ysmp_have[band][pl] = 1;
    g_ysmp_writes[band]++;
    if (g_ysmp_last_pl[band] >= 0 && g_ysmp_last_pl[band] != pl) {
        int same = 1;
        for (i = 0; i < YSMP_LEN; ++i) {
            g_ysmp_cross_totb[band]++;
            if (g_ysmp_last[band][i] == v[i]) g_ysmp_cross_eqb[band]++; else same = 0;
            /* DELIVERY: only bytes this pass actually changed. `prev` was captured
               before the store above overwrote it. */
            if (had && prev[i] != v[i]) {
                g_ysmp_dlv_tot[band]++;
                if (g_ysmp_last[band][i] == v[i]) g_ysmp_dlv_eq[band]++;
            }
        }
        if (same) g_ysmp_cross_same[band]++; else g_ysmp_cross_diff[band]++;
    }
    for (i = 0; i < YSMP_LEN; ++i) g_ysmp_last[band][i] = v[i];
    g_ysmp_last_pl[band] = pl;
}
/* Record a fan-out write at plane offset k under `mask`. Returns nothing; cheap
   enough to sit in the fan-out's inner loop (3 compares for a non-bar byte). */
static void yfan_bar_note(unsigned k, int mask)
{
    unsigned pg;
    for (pg = 0; pg < 3; ++pg) {
        unsigned off = k - pg * 0x4000u;
        unsigned idx, band;
        if (k < pg * 0x4000u || off < YBAR_OFF_LO || off >= YBAR_OFF_HI) continue;
        band = (off < YBAR_OFF_MID) ? 0u : 1u;
        g_yfan_bar_writes[band]++;
        if ((mask & 0x0F) == 0x0F) g_yfan_bar_4way[band]++;
        idx = pg * YBAR_PER_PAGE + (off - YBAR_OFF_LO);
        if (!(g_yfan_bar_seen[idx >> 3] & (1u << (idx & 7)))) {
            g_yfan_bar_seen[idx >> 3] |= (BYTE)(1u << (idx & 7));
            g_yfan_bar_distinct[band]++;
        }
        return;
    }
}
static int    g_ywmode      = 0;             /* GC write mode, tracked for latch copies */
static int    g_ylatch      = 0;             /* write mode 1 seen in the current window */
static DWORD  g_ylatch_ok = 0, g_ylatch_unsolved = 0, g_ylatch_desc = 0;

/* VirtualQuery a probe address into the log -- the shape of the A0000 region after each
   step is the only thing that distinguishes "the range is reserved by the VDM" from
   "we asked for it wrongly", and those need completely different answers. */
static void modey_remap_probe(const char *when, DWORD addr)
{
    MEMORY_BASIC_INFORMATION mbi;
    char b[200], *q = b;
    q = zput(q, "MODEY-REMAP probe "); q = zput(q, when);
    q = zput(q, " @0x"); q = zhex(q, addr);
    if (VirtualQuery((LPCVOID)(ULONG_PTR)addr, &mbi, sizeof mbi) == sizeof mbi) {
        q = zput(q, " allocbase=0x"); q = zhex(q, (DWORD)(ULONG_PTR)mbi.AllocationBase);
        q = zput(q, " base=0x");      q = zhex(q, (DWORD)(ULONG_PTR)mbi.BaseAddress);
        q = zput(q, " size=0x");      q = zhex(q, (DWORD)mbi.RegionSize);
        q = zput(q, " state=0x");     q = zhex(q, mbi.State);
        q = zput(q, " type=0x");      q = zhex(q, mbi.Type);
        q = zput(q, mbi.State == MEM_FREE     ? " (FREE)"
                  : mbi.State == MEM_RESERVE  ? " (RESERVED)" : " (COMMIT)");
    } else q = zput(q, " <VirtualQuery failed>");
    q = zput(q, "\r\n"); log_append(LOG_PATH, b, q); serial_out(b, q);
}

/* ► THE REPORT IS BUFFERED, BECAUSE THIS RUNS BEFORE THE PREAMBLE. The remap has to
     happen before the UI thread exists -- the renderer dereferences the A0000 window
     every few milliseconds and there is an instant during the swap when it is unmapped
     -- and every log_write() before the preamble TRUNCATES the file. Two separate
     reports have already been lost to that. */
static char  g_remap_rep[2048];
static char *g_remap_repq = g_remap_rep;
static void modey_remap_emit(const char *b, const char *e)
{
    while (b < e && g_remap_repq < g_remap_rep + sizeof g_remap_rep - 1) *g_remap_repq++ = *b++;
    *g_remap_repq = 0;
}
static void modey_remap_flush_report(void)
{
    if (g_remap_repq == g_remap_rep) return;
    log_append(LOG_PATH, g_remap_rep, g_remap_repq);
    serial_out(g_remap_rep, g_remap_repq);
    g_remap_repq = g_remap_rep;
}
static void modey_remap_log(const char *what, DWORD err)
{
    char b[160], *q = b;
    q = zput(q, "MODEY-REMAP "); q = zput(q, what);
    if (err) { q = zput(q, " err=0x"); q = zhex(q, err); }
    q = zput(q, "\r\n"); modey_remap_emit(b, q);
}

/* Take ownership of the A0000 window. Returns 0 and leaves everything as it was if any
   step fails -- the heuristic path still works, so a failure here must not be fatal. */
static int modey_remap_init(void)
{
    static BYTE savea[MODEY_WIN], saveb[MODEY_WIN];
    unsigned i;
    for (i = 0; i < MODEY_WIN; ++i) savea[i] = ((volatile BYTE *)(ULONG_PTR)0xA0000)[i];
    for (i = 0; i < MODEY_WIN; ++i) saveb[i] = ((volatile BYTE *)(ULONG_PTR)0xB0000)[i];

    modey_remap_probe("before unmap", 0xA0000);
    if (!UnmapViewOfFile((LPVOID)(ULONG_PTR)0xA0000)) {
        modey_remap_log("unmap of the original A0000 view FAILED", GetLastError());
        return 0;
    }
    modey_remap_probe("after unmap", 0xA0000);
    /* Text memory first: it must be back before anything reads B8000. */
    g_bsec = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_EXECUTE_READWRITE,
                                0, MODEY_WIN, NULL);
    if (!g_bsec || !MapViewOfFileEx(g_bsec, FILE_MAP_ALL_ACCESS | FILE_MAP_EXECUTE,
                                    0, 0, MODEY_WIN, (LPVOID)(ULONG_PTR)0xB0000)) {
        modey_remap_log("could not re-establish B0000 -- text memory is GONE", GetLastError());
        return 0;
    }
    for (i = 0; i < MODEY_WIN; ++i) ((volatile BYTE *)(ULONG_PTR)0xB0000)[i] = saveb[i];

    /* ► CLAIM A0000 BEFORE ASKING FOR ANY FLOATING VIEW. Unmapping the original view
         makes A0000-AFFFF the LOWEST FREE 64K-aligned hole in the address space, and
         MapViewOfFile with no address hint takes the lowest hole -- so the first
         host-side view landed exactly on the address we were about to need, and the
         fixed map then failed with ERROR_INVALID_ADDRESS against our own mapping.
         Measured, and it reads as though the range were forbidden:
             after unmap      A0000 size=0x20000 (FREE)
             after failed map A0000 size=0x10000 (COMMIT, MEM_MAPPED)  <- ours
         Take the fixed address first; everything else can live anywhere. */
    for (i = 0; i < MODEY_NSEC; ++i) {
        g_ysec[i] = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_EXECUTE_READWRITE,
                                       0, MODEY_WIN, NULL);
        if (!g_ysec[i]) { modey_remap_log("plane section allocation FAILED", GetLastError()); return 0; }
    }
    if (!MapViewOfFileEx(g_ysec[4], FILE_MAP_ALL_ACCESS | FILE_MAP_EXECUTE,
                         0, 0, MODEY_WIN, (LPVOID)(ULONG_PTR)0xA0000)) {
        modey_remap_log("could not map a replacement at A0000", GetLastError());
        modey_remap_probe("after failed map", 0xA0000);
        return 0;
    }
    for (i = 0; i < MODEY_NSEC; ++i) {
        g_yview[i] = MapViewOfFile(g_ysec[i], FILE_MAP_ALL_ACCESS, 0, 0, MODEY_WIN);
        if (!g_yview[i]) { modey_remap_log("host-side plane view FAILED", GetLastError()); return 0; }
        /* ► SEED EVERY PLANE WITH A DISTINCT MARKER, NOT WITH THE APERTURE. Seeding all
             four from the same flat buffer makes them IDENTICAL, and any region the guest
             never writes per-plane then renders as four equal pixels -- indistinguishable
             from the plane-collapse bug this whole change exists to remove. A per-plane
             marker makes "never written" visible and attributable instead: the oracle
             sees index 0/1/2/3 rather than a plausible picture. */
        { unsigned k; BYTE *d = (BYTE *)g_yview[i];
          for (k = 0; k < MODEY_WIN; ++k) d[k] = (BYTE)(i < 4 ? i : savea[k]); }
    }
    g_ycur = 4; g_yremap = 1;
    modey_remap_log("A0000 is ours: 4 planes + linear + scratch", 0);
    return 1;
}

/* ── A LATCH COPY MOVES ALL FOUR PLANES AT ONCE, AND ONE MAPPING CANNOT DO THAT. ────
     VGA write mode 1: reading an address loads every plane into the chip's latches, and
     the next store writes them all back at the destination. It is the standard mode-Y
     way to move a region inside video memory, and Doom uses it to carry the STATUS BAR
     between its three pages rather than redraw it -- measured, 120 times a run, always
     as `mask := 0x0F` (write mode 0), `write mode := 1`, copy, `write mode := 0`.

     With A0000 pointing at one buffer the guest can only move the bytes it can see, so
     every plane ended up with the same data: the four-equal-pixel collapse the WAD
     oracle found in the bar (STBAR 60% wrong, identical in every frame) while the 3D
     view and the title screen -- plain write-mode-0 stores -- were pixel-exact.

   ► THE COPY IS RECOVERABLE, AND IT IS SOLVED RATHER THAN GUESSED. What the window
     leaves behind is `scratch[dst] = seed[src]` for every byte it moved. The offsets it
     changed are known exactly (scratch vs seed), so a single displacement `src = dst -
     delta` explains the whole window if it explains EVERY changed byte -- and that is
     checkable. Try the plausible page strides, VERIFY each against every changed byte,
     and only apply one that survives. A window nothing explains falls back to the plain
     fan-out and is counted, so "we could not solve it" can never masquerade as success. */
/* Does one displacement explain EVERY byte the burst changed? */
static int modey_latch_verify(long dl)
{
    const BYTE *sc = (const BYTE *)g_yview[5];
    unsigned i, n = 0;
    for (i = 0; i < MODEY_WIN; ++i) {
        long src;
        if (sc[i] == g_yseed[i]) continue;
        ++n;
        src = (long)i - dl;
        if (src < 0 || src >= (long)MODEY_WIN || g_yseed[src] != sc[i]) return 0;
    }
    return n != 0;
}

/* ► DERIVE THE DISPLACEMENT FROM THE DATA, DO NOT GUESS AT PAGE STRIDES. A candidate
     list of plausible strides solved 71 of 120 bursts and left 49 unexplained, which is
     the shape of a guess: it works where the guess was right. The burst itself says what
     the source was -- the longest run of changed bytes IS a verbatim copy of some run in
     the pre-burst image, so find that run in the seed and the displacement falls out.
     Every hit is then VERIFIED against every changed byte before it is used, so a wrong
     match cannot be applied; a burst nothing explains is counted, never guessed at. */
static long modey_latch_delta(void)
{
    const BYTE *sc = (const BYTE *)g_yview[5];
    unsigned i, run_lo = 0, run_len = 0, best_lo = 0, best_len = 0, cap;
    for (i = 0; i < MODEY_WIN; ++i) {
        if (sc[i] != g_yseed[i]) {
            if (!run_len) run_lo = i;
            ++run_len;
            if (run_len > best_len) { best_len = run_len; best_lo = run_lo; }
        } else run_len = 0;
    }
    if (best_len < 8) return 0;                  /* too little to identify a source */
    cap = best_len > 24 ? 24 : best_len;
    for (i = 0; i + cap <= MODEY_WIN; ++i) {
        unsigned j;
        if (g_yseed[i] != sc[best_lo]) continue;              /* cheap first-byte reject */
        for (j = 1; j < cap; ++j) if (g_yseed[i + j] != sc[best_lo + j]) break;
        if (j < cap) continue;
        { long dl = (long)best_lo - (long)i;
          if (dl && modey_latch_verify(dl)) return dl; }
    }
    return 0;
}

static void ygr4_close_run(void);       /* defined with the GR4 counters below */

static void modey_remap_select(void *ctx, int mask)
{
    int want, p, n = 0, sel[4];
    (void)ctx;
    if (!g_yremap) return;
    ++g_ysel_calls;
    ygr4_close_run();   /* the window is about to move: end the current read run */

    /* ── FAN OUT ONLY WHAT THE GUEST ACTUALLY WROTE. ────────────────────────────────
         A multi-plane mask means one store lands in several planes at once, so a
         scratch window's WRITES do belong to every selected plane -- but the bytes
         nobody touched do not. Copying the whole scratch pushed the plane it was
         seeded from over the top of all the others, making four identical planes and
         therefore duplicated columns.
         That is what the user could still see after the remap landed: Doom clears with
         mask 0x0F at level start (44 windows a run, measured), so every plane went
         identical across the page. The 3D view redraws completely every frame and
         healed itself; the STATUS BAR is only updated where it changes, so the
         corruption stayed -- "the game bar at the bottom still has quite a lot of
         pixelated graphics" -- and a screen wipe looked pixelated until the full redraw
         behind it cleaned up. Diffing against the seed is exact and costs one pass over
         64K, 44 times a run. */
    if (g_ycur == 5 && g_yprev_mask) {
        unsigned k;
        const BYTE *sc = (const BYTE *)g_yview[5];
        for (k = 0; k < MODEY_WIN; ++k) {
            BYTE b;
            if (sc[k] == g_yseed[k]) continue;   /* untouched: not this mask's business */
            b = sc[k];
            yfan_bar_note(k, g_yprev_mask);      /* is this how the bar collapses? */
            for (p = 0; p < 4; ++p)
                if (g_yprev_mask & (1 << p)) ((BYTE *)g_yview[p])[k] = b;
        }
        ++g_yfanouts;
    }
    if (mask < 0) { want = 4; g_yprev_mask = 0; }
    else {
        for (p = 0; p < 4; ++p) if (mask & (1 << p)) sel[n++] = p;
        if (!n) { ++g_ysel_zero; return; }                /* mask 0: nothing to point at */
        want = (n == 1) ? sel[0] : 5;
        g_yprev_mask = (n == 1) ? 0 : mask;
    }
    if (want == g_ycur) { ++g_ysel_same; return; }
    /* Before the mapping moves: what did the plane we are leaving actually receive? */
    if (g_ycur >= 0 && g_ycur < 4) { ysmp_check(0, YSMP_A, g_ycur); ysmp_check(1, YSMP_B, g_ycur); }
    if (!UnmapViewOfFile((LPVOID)(ULONG_PTR)0xA0000)) { ++g_yfail; return; }
    if (want == 5) {                                     /* seed the scratch so a
                                                            read-modify-write sees data,
                                                            and remember the seed so the
                                                            fan-out can tell writes from
                                                            bytes nobody touched */
        unsigned k; BYTE *d = (BYTE *)g_yview[5]; const BYTE *s2 = (const BYTE *)g_yview[sel[0]];
        for (k = 0; k < MODEY_WIN; ++k) { d[k] = s2[k]; g_yseed[k] = s2[k]; }
    }
    if (!MapViewOfFileEx(g_ysec[want], FILE_MAP_ALL_ACCESS | FILE_MAP_EXECUTE,
                         0, 0, MODEY_WIN, (LPVOID)(ULONG_PTR)0xA0000)) {
        ++g_yfail;
        /* Never leave the window unmapped: put SOMETHING back or the guest's next
           store faults into a hole. */
        MapViewOfFileEx(g_ysec[g_ycur < 0 ? 4 : g_ycur], FILE_MAP_ALL_ACCESS | FILE_MAP_EXECUTE,
                        0, 0, MODEY_WIN, (LPVOID)(ULONG_PTR)0xA0000);
        return;
    }
    g_ycur = want;
    ++g_yswaps;
}

static uint8_t *modey_remap_plane(void *ctx, int p)
{
    (void)ctx;
    return (uint8_t *)g_yview[p & 3];
}

/* ── DOES THE GUEST EVER READ A PLANE OTHER THAN THE ONE IT IS WRITING? ──────────────
     A0000 holds ONE section, and `modey_remap_select` positions it from the WRITE MASK.
     A guest read of A0000 is served by the mapping directly -- the VDD never sees it --
     so it returns the WRITE plane, whatever GR4 says. On the hardware those two are
     independent registers.
     Doom's `I_ReadScreen` reads all four planes into a linear buffer by cycling GR4
     alone; the write mask is irrelevant to it and is left wherever the last blit put it.
     Under this host every one of those four passes would read THE SAME PLANE, producing
     a linear buffer whose every four-pixel group holds one plane's byte -- which is the
     collapse, arriving via a path no write-side measurement could ever have seen.
   ► THE PAIR IS THE MEASUREMENT. `gr4_hist` says only that GR4 was written; the defect
     is GR4 disagreeing with the mapped plane, and only the host knows `g_ycur`. If
     `mismatch` is ~0 this candidate dies in one run, like the linear section did.
     Measure first: do NOT move the window here yet. Remapping on GR4 would change what
     the guest sees mid-run and there would be no clean before/after.

   ▶▶ **AND THE MEASUREMENT IS IN, AND SO IS DOOM'S OWN CODE.** `DOOM.EXE` disassembles
     (file offset 0x5c154, obj1 = the LE code object) to:

         I_ReadScreen(scr):
           mov edx,3CEh / mov al,4 / out dx,al      GC index := 4 (READ MAP SELECT)
         plane_loop:
           mov edx,3CFh / mov al,cl / out dx,al     GR4 := plane   <- THE ONLY PORT WRITE
         byte_loop:
           mov bl,[ebx+eax]                         read video memory
           mov [edx-4],bl                           scr[plane + 4*i] := byte
           cmp eax,3E80h / jl byte_loop             16000 = 64000/4
           inc ecx / cmp ecx,4 / jl plane_loop

     It cycles the READ plane four times and **never writes the map mask**. Under this
     host every one of those four passes is served by whatever section the WRITE mask
     last left at A0000, so the buffer comes back as `scr[p + 4i] = plane_M[i]` for all
     four p: every four-pixel group holding ONE plane's byte, replicated. That IS the
     collapse, and it arrives through a path no write-side instrument could see -- which
     is why every writer was excluded and the content was still there.
     The run-length histogram agrees to the count: 193 runs of 4 GR4 writes with no
     intervening mask change, plus 35 of 5, = 228 -- I_ReadScreen's call count. It is the
     SCREEN WIPE (wipe_StartScreen / wipe_EndScreen). The 3D view is redrawn every frame
     and heals; the status bar is repainted only where it CHANGES (ST_diffDraw), so its
     collapsed pixels are never rewritten and the damage is permanent. That is also the
     "the wipe looked pixelated until the full redraw cleaned up" note from session 22.

   ► THE FIX: FOLLOW GR4. Point the window at the read plane. Safe because Doom sets the
     map mask before every plane WRITE (2,029,794 mask writes against 39,975 GR4 writes)
     and the pairing matrix shows the order is GR4-then-mask, so a write is always
     preceded by a mask change that puts the window back. Not done when the scratch is up
     (`g_ycur == 5`): a multi-plane write window is mid-flight and I_ReadScreen never runs
     under one. */
static DWORD g_ygr4_calls = 0, g_ygr4_mismatch = 0, g_ygr4_pair[4][6];
/* ── AND THE MISMATCH ONLY BITES IF NO MASK CHANGE FOLLOWS. ──────────────────────────
     `mismatch` is sampled at the instant GR4 is written, and 74% of those instants have
     the window one plane behind -- but that is HARMLESS in the ordinary blit, where the
     guest sets GR4 = p and then immediately sets the mask to 1<<p, moving the window
     before any read happens. The pairing matrix cannot tell that apart from the case
     that matters.
   ► COUNT GR4 WRITES BETWEEN CONSECUTIVE MASK CHANGES. A pure-READ pass sets GR4 four
     times and never touches the mask, so the window sits still for all four and every
     read returns ONE plane -- filling the guest's buffer with one plane's bytes
     replicated across each four-pixel group. That is the collapse, and a run length of
     4 with no intervening select is its fingerprint. A run of 1 is the ordinary blit and
     is fine. This is the counter that can come out either way. */
static DWORD g_ygr4_since_sel = 0, g_ygr4_runs[10], g_ygr4_run_planes[4];
static void ygr4_close_run(void)
{
    if (g_ygr4_since_sel) {
        g_ygr4_runs[g_ygr4_since_sel < 9 ? g_ygr4_since_sel : 9]++;
        /* A run of 4+ is a read pass: record which plane the window was stranded on,
           because that plane's bytes are what the guest took away four times. */
        if (g_ygr4_since_sel >= 4 && g_ycur >= 0 && g_ycur < 4) g_ygr4_run_planes[g_ycur]++;
        g_ygr4_since_sel = 0;
    }
}
static DWORD g_ygr4_moves = 0;
static void modey_remap_readmap(void *ctx, int plane)
{
    (void)ctx;
    if (!g_yremap) return;
    ++g_ygr4_calls;
    ++g_ygr4_since_sel;
    if (g_ycur >= 0 && g_ycur < 6) g_ygr4_pair[plane & 3][g_ycur]++;
    if (plane != g_ycur) ++g_ygr4_mismatch;

    plane &= 3;
    if (g_ycur == 5 || g_ycur == plane) return;   /* scratch in flight, or already there */
    if (!UnmapViewOfFile((LPVOID)(ULONG_PTR)0xA0000)) { ++g_yfail; return; }
    if (!MapViewOfFileEx(g_ysec[plane], FILE_MAP_ALL_ACCESS | FILE_MAP_EXECUTE,
                         0, 0, MODEY_WIN, (LPVOID)(ULONG_PTR)0xA0000)) {
        ++g_yfail;
        MapViewOfFileEx(g_ysec[g_ycur < 0 ? 4 : g_ycur], FILE_MAP_ALL_ACCESS | FILE_MAP_EXECUTE,
                        0, 0, MODEY_WIN, (LPVOID)(ULONG_PTR)0xA0000);
        return;
    }
    g_ycur = plane;
    ++g_ygr4_moves;     /* NOT g_yswaps: the map-mask identity must keep balancing */
}

/* ► THE COPY'S BOUNDARY IS THE WRITE-MODE CHANGE, NOT THE MASK CHANGE. Solving over a
     whole mask window found nothing (44 windows, 0 solved): Doom sets `mask := 0x0F`
     once and then does MANY separate latch copies under it, so the changed bytes are the
     union of several moves and no single displacement explains them. Between `write mode
     := 1` and `write mode := 0` there is exactly one burst, which is the thing a single
     displacement CAN describe. Seed at the start of the burst, solve at its end. */
static void modey_remap_wmode(void *ctx, int wm)
{
    (void)ctx;
    if (!g_yremap) { g_ywmode = wm; return; }
    if (wm == 1 && g_ywmode != 1) {
        if (g_ycur == 5) {                          /* multi-plane: the scratch is the
                                                       only place the copy is visible */
            unsigned k; const BYTE *sc = (const BYTE *)g_yview[5];
            for (k = 0; k < MODEY_WIN; ++k) g_yseed[k] = sc[k];
            g_ylatch = 1;
        }
        /* A single-plane mask needs nothing: the guest is moving bytes inside the plane
           that IS mapped, which is exactly what the hardware would do. */
    } else if (wm != 1 && g_ywmode == 1 && g_ylatch) {
        long dl = modey_latch_delta();
        unsigned k;
        const BYTE *sc = (const BYTE *)g_yview[5];
        if (dl) {
            int p;
            for (k = 0; k < MODEY_WIN; ++k) {
                long src;
                if (sc[k] == g_yseed[k]) continue;
                src = (long)k - dl;
                for (p = 0; p < 4; ++p)
                    if (g_yprev_mask & (1 << p)) ((BYTE *)g_yview[p])[k] = ((BYTE *)g_yview[p])[src];
            }
            ++g_ylatch_ok;
        } else {
            /* ── ⚠⚠ THE UNSOLVED PATH USED TO DROP THE BURST ENTIRELY. ────────────────
                 The solver is 0-for-120: `dl` has never once been non-zero on a real
                 run, so this branch IS the burst path, not an edge case. It counted the
                 failure and did nothing -- and then the re-seed at the bottom of this
                 function overwrote g_yseed with the scratch, which is the very
                 comparison modey_remap_select()'s fan-out uses to decide what to
                 propagate. So by the time the mask changed, every byte matched the seed
                 and the fan-out copied NOTHING. Measured, and this is what sent me
                 looking: `fanout_bar distinct=0` over a whole run while the burst
                 instrument reported ~10,014 changed bytes.
                 The guest computed those bytes and we threw them away.
               ► WHAT WE CAN AND CANNOT PUT BACK. Write mode 1 gives each plane its OWN
                 latched byte, and the scratch holds ONE byte per offset -- it cannot
                 represent four latches, which is precisely why every inference scheme
                 over it has failed and why the solver never succeeds. Recovering the
                 true per-plane bytes needs the accesses themselves, and the A0000 page
                 trap that would provide them is MEASURED TWICE ON THIS BOX TO FREEZE
                 THE GUEST (see a000_protect) -- so that door is shut.
                 What we can do is stop discarding: propagate the scratch byte to the
                 planes the mask selected, exactly as the write-mode-0 fan-out does.
                 For the 6 CONSTANT bursts a run (fills of 0x00) that is EXACTLY right.
                 For the 114 copy bursts it is an approximation -- plane 0's source byte
                 reaching all four -- but it puts the guest's own data where stale
                 content sits today.
               ⚠ THAT TRADE IS NOT SELF-EVIDENT: it swaps "stale in four planes" for
                 "collapsed across four planes", and this project has been burned by
                 reasoning about which wrong picture is less wrong. So it is judged on
                 the WAD ORACLE (planejudge.py, plane-vs-STBAR), not on
                 bar_planes_equal, which by construction gets WORSE when we fan out.
               ⚠⚠ **AND IT WAS TRIED, MEASURED, AND NOT KEPT. DO NOT RE-APPLY IT.**
                 Propagating the scratch byte to the selected planes here delivers the
                 data -- `fanout_bar` band B went 0 -> 7352 writes over 192 distinct bar
                 offsets -- and the WAD oracle says it buys NOTHING:
                     plane 0  34.4% -> 34.9%      plane 2  29.8% -> 29.8%
                     plane 1  70.7% -> 70.2%      plane 3  27.8% -> 27.6%
                 Under a point either way, and it makes plane 1 -- the one plane that is
                 mostly CORRECT -- slightly worse, by smearing plane 0's bytes across it.
                 The informative part is what that implies: those 192 offsets were no
                 more wrong before than after, so **the bytes we discard are not what is
                 wrong with the status bar.** Session 23's refutation of the latch copy
                 stands, though not for the reason it gave (it argued from which rows the
                 bursts touch; the stronger argument is that delivering the data changes
                 nothing).
                 So keep DROPPING them, and keep COUNTING the drop, rather than shipping
                 an approximation that would read to the next session like the latch path
                 is handled. The real repair needs per-plane latch capture; see above for
                 why the page trap cannot provide it on this hardware, which leaves the
                 mode-12h-style interpreter as the only remaining candidate. */
            ++g_ylatch_unsolved;
        }
        /* ► WHAT IS A BURST, ACTUALLY? Two inference schemes have now failed on it --
             plausible page strides explained 71 of 120 (the shape of a lucky guess) and
             deriving the displacement from the copied run explained NONE. That second
             result is the informative one: if the burst were a verbatim region copy, its
             longest changed run would appear verbatim in the pre-burst image, and it does
             not. So describe the burst instead of guessing at it: how much changed, over
             what span, and whether the destination is a CONSTANT (a latched fill) rather
             than a copy. Bounded to the first few, because one example settles it. */
        /* ⚠⚠ THIS BOUND WAS 6, AND IT PRODUCED A WRONG ROOT CAUSE. Six descriptions of
             160 bursts, of which only TWO had any changed bytes -- both at 0x3a1c..0x3e7f
             -- were generalised into "the bursts only ever touch rows 186..199", and that
             sentence retired the latch copy as the status bar's cause. Two samples out of
             a hundred and sixty. A BOUND ON AN INSTRUMENT IS A CLAIM ABOUT WHAT IS
             REPRESENTATIVE, and this one was never checked. 160 lines is ~19 KB; describe
             them ALL, and let the row coverage be measured rather than extrapolated. */
        if (g_ylatch_desc < 4096) {
            unsigned k2, n = 0, lo = MODEY_WIN, hi = 0, uniq = 0, nbar = 0;
            BYTE first = 0;
            int constant = 1;
            for (k2 = 0; k2 < MODEY_WIN; ++k2) {
                unsigned row;
                if (sc[k2] == g_yseed[k2]) continue;
                if (!n) { lo = k2; first = sc[k2]; }
                if (sc[k2] != first) constant = 0;
                hi = k2; ++n;
                /* how much of this burst lands in the status bar at all */
                row = (k2 % 0x4000u) / 80u;
                if (row >= 168 && row < 200) ++nbar;
            }
            (void)uniq;
            { char lb2[224], *lq = lb2;
              ++g_ylatch_desc;
              lq = zput(lq, "MODEY-LATCH burst changed="); lq = zhex(lq, n);
              lq = zput(lq, " span=0x"); lq = zhex(lq, lo);
              lq = zput(lq, "..0x"); lq = zhex(lq, hi);
              /* ► IN THE UNITS OF THE CLAIM. A plane offset is row*80 + x/4, so a span
                   in hex says nothing about which rows a burst reaches -- and reading
                   0x3a1c..0x3e7f as "the status bar" without converting it is exactly
                   how the wrong cause survived. Print the rows next to the offsets. */
              lq = zput(lq, " rows="); lq = zhex(lq, n ? (lo % 0x4000u) / 80u : 0);
              lq = zput(lq, "..");     lq = zhex(lq, n ? (hi % 0x4000u) / 80u : 0);
              lq = zput(lq, " barbytes="); lq = zhex(lq, nbar);
              lq = zput(lq, constant ? " DEST IS CONSTANT 0x" : " dest varies, first=0x");
              lq = zhexb(lq, first);
              lq = zput(lq, " mask=0x"); lq = zhexb(lq, (unsigned)g_yprev_mask);
              lq = zput(lq, "\r\n"); log_append(LOG_PATH, lb2, lq); serial_out(lb2, lq); }
        }
        for (k = 0; k < MODEY_WIN; ++k) g_yseed[k] = sc[k];   /* re-seed for the byte diff */
        g_ylatch = 0;
    }
    g_ywmode = wm;
}

/* --- planar mode-12h: trap direct A0000 writes through the VGA write engine -- */
#define A000_LO 0xA0000u
#define A000_HI 0xB0000u
static int g_a000_prot = 0;

/* In mode 12h, mark the A0000 graphics window NOACCESS so direct guest writes
   fault to us; restore RW otherwise. */
static void a000_protect(int on)
{
    DWORD old;
    if (g_no_a000) on = 0;              /* diagnostic knob -- see NOA000_FLAG */
    if (on == g_a000_prot) return;
    if (VirtualProtect((LPVOID)A000_LO, 0x10000,
                       on ? PAGE_NOACCESS : PAGE_EXECUTE_READWRITE, &old))
        g_a000_prot = on;
}

/* ---- how mode 12h is intercepted (GH #55) --------------------------------- *
 * MEASURED, twice, on the physical box: arming the A0000 page trap FREEZES the
 * guest. `PAGE_NOACCESS` and `PAGE_READONLY` behave identically (so it is not
 * reads-vs-writes, it is protecting the range at all), io_events stops at 10
 * against 22,532,292 with the trap off, and the exec thread never comes back out
 * of VdmStartExecution -- the guest's CS:IP in the TIB stays frozen at whatever
 * the last event left it. The M3 planar trap was VM-confirmed on HVF and NEVER on
 * real hardware, and there is precedent for exactly this class of difference
 * (session 8: HVF reflects IOPL-0 I/O as event 0, real silicon as event 3).
 *
 * So do not protect the page. Instead, while a planar mode is current, run the
 * guest in the HOST INTERPRETER, whose A0000 accesses go through the planar write
 * engine by construction (imem_r8/imem_w8). The interpreter is the CPU for as long
 * as mode 12h is set; it yields whenever an IRQ is pending, and any opcode it does
 * not model drops that one instruction back to V86.                              */
static int g_p12_interp = 0;    /* planar mode is current -> interpret the guest  */

static void video_trap_sync(void)
{
    int planar = vdd_video_planar_active(&g_vid);
    if (planar && !g_p12_off) { a000_protect(0); g_p12_interp = 1; }
    else                      { a000_protect(planar); g_p12_interp = 0; }
}

/* ====================================================================== *
 *  Mode-12h fill-loop fast path: a small, bounded, flags-accurate 8086    *
 *  interpreter.                                                           *
 *                                                                         *
 *  In mode 12h the A0000 window is PAGE_NOACCESS, so every guest pixel    *
 *  touch faults to us. QuickBASIC's PAINT/LINE fills are tight per-pixel  *
 *  loops (e.g. `MOV AL,ES:[SI] / OR AL,AL / JNZ / DEC DI / JNZ`), so one  *
 *  fill = hundreds of thousands of V86 round-trips and never finishes.    *
 *                                                                         *
 *  Fix: when an A0000 access faults, run the *whole* inner loop here --   *
 *  loads/stores (planar engine for A0000, flat for normal RAM), the       *
 *  arithmetic/logic group (computing CF/PF/AF/ZF/SF/OF), INC/DEC, string  *
 *  ops (REP, honouring DF), MOV, TEST, the flag ops, and Jcc/JMP/LOOP --  *
 *  until we hit an opcode we don't model or an iteration cap. Then we     *
 *  write the architectural state back and return to V86. It NEVER         *
 *  derails: any unmodeled byte stops the interpreter with EIP exactly on  *
 *  that instruction, so V86 re-executes it. 16-bit only (0x66/0x67/LOCK   *
 *  bail). One fault now drives the entire fill instead of one-per-pixel.  *
 * ====================================================================== */

/* Flat/planar guest memory for the interpreter: A0000 goes through the VGA
   engine (a read loads the latches), everything else is the directly-mapped
   V86 address space. These are the host hooks v86interp.h requires. */
static uint8_t imem_r8(uint32_t lin)
{ return (lin >= A000_LO && lin < A000_HI) ? vga_planar_read(&g_vid, lin - A000_LO)
                                           : *(volatile BYTE *)lin; }
static void imem_w8(uint32_t lin, uint8_t v)
{ if (lin >= A000_LO && lin < A000_HI) vga_planar_write(&g_vid, lin - A000_LO, v);
  else *(volatile BYTE *)lin = v; }

/* Port I/O dispatched to the device bus (same path as host_try_io). The
   interpreter already runs under g_lock, which is what the bus needs. */
static uint32_t iio_in(uint16_t port, int width)
{ uint32_t v = 0; vdd_bus_io(&g_bus, port, (uint8_t)width, 1, &v); return v; }
static void iio_out(uint16_t port, int width, uint32_t val)
{ uint32_t v = val; vdd_bus_io(&g_bus, port, (uint8_t)width, 0, &v); }

#include "v86interp.h"

/* The mode-12h trap-storm escape hatch. By default V86 runs on the real CPU and
   each VGA access (memory OR port) is emulated one-at-a-time as a device access
   -- pure device virtualization. But QuickBasic plots pixels one at a time and
   reprograms a VGA register via OUT *between* pixels, so a fill is hundreds of
   thousands of fault round-trips (port faults AND memory faults) and crawls.

   host_interp() is the opt-in batching interpreter: load the V86 register file,
   run up to `cap` instructions in the host (the inner loop -- planar A0000
   access, IN/OUT through the bus, ALU, CALL/RET, branches), then write the
   architectural state back. The caller (the service loop) engages it ONLY on a
   detected trap-storm (the same tight PC window faulting repeatedly), so it's a
   measured fallback for proven pathological video loops, not a blanket policy.
   Returns the number of instructions executed (0 if the faulting instruction
   itself is unmodeled -> caller falls through). */
#define STORM_WINDOW   128       /* faults within this PC span count as "the same loop" */
#define STORM_GATE      8        /* consecutive in-window faults -> escalate to the interpreter */
#define TIER1_CAP  2000000L      /* interpreter iteration ceiling once escalated        */
#define P12_SLICE     20000L     /* planar mode: instructions per interpreter slice     */

static long host_interp(volatile BYTE *tib, long cap)
{
    icpu c; long iters;

    c.r[0] = (uint16_t)VDM_REG(tib, VTIB_EAX); c.r[1] = (uint16_t)VDM_REG(tib, VTIB_ECX);
    c.r[2] = (uint16_t)VDM_REG(tib, VTIB_EDX); c.r[3] = (uint16_t)VDM_REG(tib, VTIB_EBX);
    c.r[4] = (uint16_t)VDM_REG(tib, VTIB_ESP); c.r[5] = (uint16_t)VDM_REG(tib, VTIB_EBP);
    c.r[6] = (uint16_t)VDM_REG(tib, VTIB_ESI); c.r[7] = (uint16_t)VDM_REG(tib, VTIB_EDI);
    c.seg[0] = (uint16_t)VDM_REG(tib, VTIB_ES); c.seg[1] = (uint16_t)VDM_REG(tib, VTIB_CS);
    c.seg[2] = (uint16_t)VDM_REG(tib, VTIB_SS); c.seg[3] = (uint16_t)VDM_REG(tib, VTIB_DS);
    c.seg[4] = (uint16_t)VDM_REG(tib, VTIB_FS); c.seg[5] = (uint16_t)VDM_REG(tib, VTIB_GS);
    c.ip = (uint16_t)VDM_REG(tib, VTIB_EIP); c.flags = VDM_REG(tib, VTIB_EFLAGS);

    HOST_LOCK();
    for (iters = 0; iters < cap; ++iters) {
        if (!istep(&c)) break;
        /* YIELD WHEN AN INTERRUPT IS PENDING. A real CPU takes interrupts in the
           middle of a loop; the interpreter is standing in for that CPU and must
           do the same, or a guest whose loop can only END when an interrupt
           fires runs here forever.
           This is not hypothetical -- it is why mode 12h never worked. BLIT's
           outer loop is `DO WHILE INKEY$ = ""`, and QuickBASIC polls for the key
           in memory. Escalated to the interpreter, that loop burned the whole
           2,000,000-iteration cap with no way for a keystroke or a tick to ever
           reach it, returned, re-faulted, re-escalated: TEN I/O events in thirty
           seconds and a frozen screen, while the same program with the A0000
           trap disabled produced 22.5 MILLION.
           Checked every 256 instructions so the cost is negligible against the
           fill loops this batching exists to accelerate. */
        if ((iters & 0xFF) == 0xFF) {
            int q, pend = (g_irq0_pending != 0);
            for (q = 0; !pend && q < 8; ++q) pend = (g_irqn_pending[q] != 0);
            if (pend) { ++iters; break; }
        }
    }
    HOST_UNLOCK();

    if (iters == 0) return 0;                          /* first opcode unmodeled */

    VDM_SET16(tib, VTIB_EAX, c.r[0]); VDM_SET16(tib, VTIB_ECX, c.r[1]);
    VDM_SET16(tib, VTIB_EDX, c.r[2]); VDM_SET16(tib, VTIB_EBX, c.r[3]);
    VDM_SET16(tib, VTIB_ESP, c.r[4]); VDM_SET16(tib, VTIB_EBP, c.r[5]);
    VDM_SET16(tib, VTIB_ESI, c.r[6]); VDM_SET16(tib, VTIB_EDI, c.r[7]);
    VDM_SET16(tib, VTIB_EIP, c.ip);
    /* SEGMENTS TOO. They were loaded but never stored, so every segment load the
       interpreter modelled (POP ES / MOV DS,AX / far CALL / INT / IRET) was thrown
       away the moment we returned to V86 -- the guest carried on with the SEGMENT
       it had before the batch and the OFFSET the batch had reached. Harmless while
       batching was confined to a fill loop that never reloads a segment; fatal for
       continuous interpretation, where CS changes on every interrupt. */
    VDM_REG(tib, VTIB_ES) = c.seg[0]; VDM_REG(tib, VTIB_CS) = c.seg[1];
    VDM_REG(tib, VTIB_SS) = c.seg[2]; VDM_REG(tib, VTIB_DS) = c.seg[3];
    VDM_REG(tib, VTIB_FS) = c.seg[4]; VDM_REG(tib, VTIB_GS) = c.seg[5];
    /* update only the low 16 flag bits (arith + DF); keep VM/IOPL/IF etc. */
    VDM_REG(tib, VTIB_EFLAGS) = (VDM_REG(tib, VTIB_EFLAGS) & 0xFFFF0000u) | (c.flags & 0xFFFFu);
    return iters;
}

/* ── PROBE A GUEST POINTER WITHOUT FAULTING. Session 17, and it cost a run. ────────
   IsBadReadPtr does its job by TOUCHING the memory inside an SEH frame -- so on a bad
   pointer it raises an access violation, and a VECTORED handler sees that before the
   SEH frame swallows it. dpmi_crash_veh below is exactly such a handler, and while a
   PM client is running it treats any fault with a flat CS as a reflected guest INT
   31h: it rewrote our OWN thread's CONTEXT and resumed it. A diagnostic that guards
   itself with IsBadReadPtr therefore KILLS the run it is diagnosing, and the log ends
   one line before the thing you added it to see. That is what happened here.
   VirtualQuery answers the same question by asking the memory manager instead of the
   CPU, so it cannot raise. Committed + readable (any of the four read-capable
   protections) + the whole span inside one region is the test. */
static int host_readable(const void *addr, SIZE_T len)
{
    MEMORY_BASIC_INFORMATION mbi;
    ULONG_PTR a = (ULONG_PTR)addr;
    if (!a || len == 0) return 0;
    if (VirtualQuery((LPCVOID)a, &mbi, sizeof(mbi)) != sizeof(mbi)) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
    if (!(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                         PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
        return 0;
    /* the span must not run off the end of this region into an unmapped one */
    return (a + len) <= ((ULONG_PTR)mbi.BaseAddress + mbi.RegionSize);
}

/* Crash diagnostic (DPMI spike): the PM switch works but VdmStartExecution faults
   inside the monitor when it runs PM, crashing the host with no info. This VEH
   catches the fault, dumps the exception (code/addr/params) + the host CONTEXT +
   the guest PM CONTEXT to the log, then exits CLEANLY (no WER dialog) so the batch
   still prints the log. Only meaningful once g_dpmi_pm is set. */
static int s_veh_count = 0;
static LONG CALLBACK dpmi_crash_veh(EXCEPTION_POINTERS *ep)
{
    static char cb[1024]; char *p = cb;
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    CONTEXT *cx = ep->ContextRecord;
    if (!g_dpmi_pm) return EXCEPTION_CONTINUE_SEARCH;   /* only handle PM events */
    InterlockedIncrement(&g_veh_any);                   /* run 52: prove ANY PM fault reaches us */

    /* --- Reflected PM software interrupt = the DPMI INT 31h dispatch (run 26) ---------
       Under VdmStartExecution the kernel reflects a PM INT nn by advancing EIP past it and
       reloading FLAT CS/SS (0x1B/0x23), leaving the int OFFSET in EDX and the guest's other
       regs intact. So: CS flat + in PM => a reflected guest INT. Read the vector from the
       instruction bytes at [code_base+EDX] (the guest issues only INT 31h), SERVICE it as a
       DPMI call (returns in the CONTEXT, CF in EFlags), restore the LDT CS/SS, and resume the
       guest past the INT. The VEH IS the protected-mode DPMI handler, on the real CPU. */
    /* ► "FLAT CS" IS NOT ENOUGH TO IDENTIFY A GUEST REFLECT, and assuming it was cost
         session 17 a run. Our own host code ALSO runs with CS=0x1B, so a plain access
         violation anywhere in the host -- a bad pointer in a diagnostic, a library
         probe -- landed in this arm, got answered as "INT 31h, unsupported function",
         had EAX/EFLAGS/CS/SS rewritten, and was RESUMED. The host then continued with a
         corrupted context and the run ended with no explanation.
         The guest's PM address space is the low megabyte-and-a-bit (V86 window + our
         0x500 handler segment + the DPMI code base); no host code or system DLL lives
         below 2 MB. So require the faulting EIP to be down there. Anything else is OUR
         fault and must go to the fatal path, which says so and dumps it. */
    /* ► THE < 2 MB GUARD IS A 16-BIT CLIENT'S GUARD, AND DOOM IS NOT ONE. It exists
         to keep OUR OWN host faults (also CS=0x1B) out of this arm, using the fact
         that a 16-bit guest lives in the low megabyte. A 32-bit DOS/4GW client does
         not: Doom's code sits at ~0x03Bxxxxx, and its first PM fault under
         pmkernel.flag reported EIP=0x02620013 -- so the guard threw a SPURIOUS ENTRY
         FAULT onto the fatal path and killed the run at PM entry 1, two entries in.
         The EDX signature does not care about the address range: the kernel puts the
         ENTRY EIP in EDX (that fault carried EDX=0x5fd8, entry 1 exactly), and we
         only look while g_pm_entry_eip is set, i.e. inside VdmStartExecution. Accept
         either: the old low-memory case, or a match on EDX. */
    if (cx->SegCs == 0x1B && s_veh_count < 256
        && (cx->Eip < 0x00200000u
            || (g_pm_entry_eip >= 0 && (DWORD)cx->Edx == (DWORD)g_pm_entry_eip))) {
        DWORD site = g_dpmi_code_base + (cx->Edx & 0xFFFF);
        const BYTE *ib = (const BYTE *)(ULONG_PTR)site;
        DWORD func = cx->Eax & 0xFFFF;
        BYTE  vec  = (ib[0] == 0xCD) ? ib[1] : 0x31;    /* CD nn -> vector; default 31h    */
        ++s_veh_count;
        p = zput(p, "DPMI INT"); p = zhex(p, vec); p = zput(p, "h #"); p = zhex(p, (unsigned)s_veh_count);
        p = zput(p, ": AX=0x"); p = zhex(p, func);
        p = zput(p, " BX=0x"); p = zhex(p, cx->Ebx & 0xFFFF);
        p = zput(p, " CX=0x"); p = zhex(p, cx->Ecx & 0xFFFF);
        p = zput(p, " [site EDX=0x"); p = zhex(p, cx->Edx & 0xFFFF);
        p = zput(p, " EIP=0x"); p = zhex(p, cx->Eip);
        p = zput(p, " b@site="); p = zdump(p, ib, 4); p = zput(p, "]");
        { const BYTE *sent = (const BYTE *)(ULONG_PTR)0x1600;   /* guest sentinel DS:0x600 */
          p = zput(p, " sentinel@0x1600="); p = zdump(p, sent, 4); }
        /* ── WHAT ACTUALLY FAULTED. THIS ARM NEVER SAID, AND THAT IS THE WHOLE GAP. ──
             Everything above is an INTERPRETATION: it ASSUMES the fault is a kernel-
             reflected `INT nn`, reads the vector from [code_base+EDX] and answers it as
             a DPMI call -- then RESUMES the guest. If the assumption is wrong the guest
             carries on with EAX/EFLAGS/CS/SS rewritten and instructions silently skipped,
             and the log still reads like a serviced DPMI call. Under `pmkernel.flag` this
             arm fires once per PM entry and the client's `INT 31h 0301` then finds its
             RMCS half-written -- with no way, from this log, to tell whether that is a
             reflected INT we answered wrongly, our own `C4 C4` BOP raising #UD in PM, or
             a genuine access violation. So print the primitives, not the interpretation:
             the exception CODE, the address the KERNEL blames, its AV read/write
             parameters, the bytes at the faulting EIP, and the TIB's own idea of where
             the guest is -- because a CONTEXT that disagrees with the TIB is not the
             guest's CONTEXT at all. Cheap: one line, on a path that already logs. */
        p = zput(p, " exc=0x"); p = zhex(p, er->ExceptionCode);
        p = zput(p, " at=0x"); p = zhex(p, (DWORD)(ULONG_PTR)er->ExceptionAddress);
        if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
            p = zput(p, " av{op=0x");  p = zhex(p, (DWORD)er->ExceptionInformation[0]);
            p = zput(p, " addr=0x");   p = zhex(p, (DWORD)er->ExceptionInformation[1]);
            p = zput(p, "}");
        }
        { const BYTE *fb = (const BYTE *)(ULONG_PTR)(g_dpmi_code_base + (cx->Eip & 0xFFFF));
          p = zput(p, " b@eip="); p = zdump(p, fb, 6); }
        p = zput(p, " ctx{ss:esp=0x"); p = zhex(p, cx->SegSs); p = zput(p, ":0x"); p = zhex(p, cx->Esp);
        p = zput(p, " ds=0x"); p = zhex(p, cx->SegDs); p = zput(p, " es=0x"); p = zhex(p, cx->SegEs);
        p = zput(p, " edi=0x"); p = zhex(p, cx->Edi); p = zput(p, "}");
        if (g_tib_dbg) {
            volatile BYTE *t = g_tib_dbg;
            p = zput(p, " tib{cs:eip=0x"); p = zhex(p, VDM_REG(t, VTIB_CS) & 0xFFFF);
            p = zput(p, ":0x"); p = zhex(p, VDM_REG(t, VTIB_EIP));
            p = zput(p, " eax=0x"); p = zhex(p, VDM_REG(t, VTIB_EAX));
            p = zput(p, " edx=0x"); p = zhex(p, VDM_REG(t, VTIB_EDX)); p = zput(p, "}");
        }
        /* ── IS THIS A REFLECTED `INT nn` AT ALL? ASK THE INSTRUCTION, NOT THE ARM. ──
             Measured under `pmkernel.flag` (build/pmk5.log, 8 hits, one per PM entry):
             SIX carry exc=0xC0000005 and TWO exc=0xC000001E, and the bytes at the
             faulting EIP decode to plain stores -- `mov [0x600],ax`, `mov [0x604],dx`,
             `mov word [0x49a],0x0100` -- not to `CD nn`. They are FAULTS, and answering
             a fault as "INT 31h, unsupported function" rewrites EAX and CF, forces CS
             and SS, and resumes the guest corrupted, once per entry. That is why
             dpmitest.com's `INT 31h 0301` finds a half-written RMCS: the client's own
             stores are being interleaved with our damage.
             The store DOES land when re-executed -- the sentinel at 0x1600 goes 00->01
             across hit #2 -- so the correct response to a fault here is to put the
             guest's LDT selectors back (the CONTEXT arrives with flat CS=0x1B/SS=0x23,
             so resuming without that dies instantly) and resume, touching NOTHING else.
             Service ONLY what is provably an INT: `CD nn` at the faulting EIP. */
        { const BYTE *fi = (const BYTE *)(ULONG_PTR)(g_dpmi_code_base + (cx->Eip & 0xFFFF));
          if (fi[0] != 0xCD) {
              /* ► WE ARE A FIRST-CHANCE HANDLER. ARE WE STEALING A FAULT THE KERNEL
                   WOULD HANDLE BETTER? Under `pmkernel.flag` the guest runs INSIDE
                   VdmStartExecution, and the kernel's whole job for a VDM is to turn
                   a guest fault into an EVENT the monitor is handed back -- which the
                   outer loop already knows how to service (VDM_EVENT_GPFAULT and
                   friends). Swallowing the fault here means VdmStartExecution never
                   sees it. And swallowing it is not free: it resumes the guest at the
                   EIP the kernel reported, which for a 2-byte first instruction is
                   entry+3, one byte PAST the boundary -- measured twice, and fatal for
                   pmtick.com.
                   `pmvehpass.flag` runs the other arm of the experiment: let it fall
                   through and see whether VdmStartExecution returns an event instead.
                   Absent file = the behaviour above, unchanged. */
              if (g_pm_veh_pass) {
                  p = zput(p, " -> FAULT, not an INT: PASSING IT THROUGH (pmvehpass)");
                  p = zput(p, "\r\n");
                  log_append(LOG_PATH, cb, p); serial_out(cb, p);
                  return EXCEPTION_CONTINUE_SEARCH;
              }
              /* ── RESUME WHERE THE GUEST ACTUALLY IS, NOT WHERE THE RECORD SAYS. ──
                   The reported EIP is unreliable: E+0, E+1 and E+3 all measured, and
                   pmal.com and pmstep.com report DIFFERENT offsets for byte-identical
                   entry code, so it is not a function of the instruction stream.
                   Resuming at it is what actually breaks clients -- when it lands past
                   the entry the instruction there is SKIPPED (pmstep's `mov ax,0x4C00`
                   went missing, which is why its AH=4Ch never terminated; dpmitest
                   reaches INT 31h with the wrong AX and half-writes its RMCS), and when
                   it lands mid-instruction the guest dies outright (pmtick, pmal).
                   pmal.com settles where the guest really is by making AL a program
                   counter: AL=0 at the first fault and UNCHANGED at the second, so no
                   guest instruction had executed at either. The entry EIP is correct. */
              /* ── ONLY THE SPURIOUS ENTRY FAULT GETS REDIRECTED. ──────────────
                   Measured: the spurious one is exc=0xC0000005 whose AV record
                   carries NO address (0xFFFFFFFF, i.e. the kernel synthesised it)
                   or exc=0xC000001E. A genuine guest fault looks nothing like that
                   -- pmtick.com produced exc=0x80000003 (STATUS_BREAKPOINT) at
                   0x3dc, INSIDE ITS OWN MESSAGE DATA, from a guest that had already
                   run away. Redirecting THAT to the entry EIP is not a fix, it is a
                   loop: it keeps restarting an entry whose guest is long gone, and
                   the run ends at a nonsense cs:eip=0x3f:0x0080 with the real
                   failure never reported. Send anything unrecognised to the fatal
                   dump, which prints the code, the address and the bytes -- so a
                   runaway is VISIBLE instead of being silently "resumed". */
              /* ► THE DISCRIMINATOR IS EDX, NOT THE EXCEPTION CODE. Three codes have
                     now been seen for the SAME spurious entry event -- 0xC0000005,
                     0xC000001E and 0xC0000003/0x80000003 -- so keying on the code
                     mislabels one of them every time a new client turns up. pmtick
                     produced a STATUS_BREAKPOINT "at 0x3dc", and guest 0x3dc is the
                     middle of the string "PMTICK: mode switch FAILED (CF=1)": message
                     data, with no 0xCC in it. The guest was never there; the address
                     is as unreliable as the rest.
                     What IS constant, in every spurious fault since the first log, is
                     that the kernel puts THE ENTRY EIP IN EDX -- and it is demonstrably
                     not the guest's own EDX (here EDX=0x20b, the entry, while the
                     guest's real EDX was 0x2c2d). So ask that. */
              if ((DWORD)cx->Edx != (DWORD)g_pm_entry_eip || g_pm_entry_eip < 0) {
                  p = zput(p, " -> REAL fault (EDX is not the entry EIP): FATAL dump");
                  p = zput(p, "\r\n");
                  log_append(LOG_PATH, cb, p); serial_out(cb, p);
                  goto veh_fatal;
              }
              if (g_pm_entry_eip >= 0 && (DWORD)g_pm_entry_eip != cx->Eip) {
                  p = zput(p, " -> FAULT, not an INT: resuming at ENTRY 0x");
                  p = zhex(p, (DWORD)g_pm_entry_eip);
                  cx->Eip = (DWORD)g_pm_entry_eip;
              } else {
                  p = zput(p, " -> FAULT, not an INT: resuming untouched");
              }
              p = zput(p, "\r\n");
              log_append(LOG_PATH, cb, p); serial_out(cb, p);
              cx->SegCs = 0x0F; cx->SegSs = 0x17;       /* guest selectors back; regs intact */
              return EXCEPTION_CONTINUE_EXECUTION;
          } }
        cx->EFlags &= ~1u;                              /* default: CF=0 (success)          */
        switch (func) {
        case 0x0400:                                    /* get DPMI version                 */
            cx->Eax = (cx->Eax & 0xFFFF0000) | 0x005A;  /* AH=0 major, AL=0x5A (90) minor    */
            cx->Ebx = (cx->Ebx & 0xFFFF0000) | 0x0000;  /* BX flags: 16-bit host             */
            cx->Ecx = (cx->Ecx & 0xFFFF0000) | 0x0003;  /* CL=3 (80386)                      */
            cx->Edx = (cx->Edx & 0xFFFF0000) | 0x7008;  /* DH=slave 0x70, DL=master 0x08 PIC  */
            p = zput(p, " -> DPMI 0.90 (386)");
            break;
        case 0x0000:                                    /* allocate LDT descriptors (CX=count) */
            cx->Eax = (cx->Eax & 0xFFFF0000) | 0x001F;  /* base selector 0x1F (spike stub)    */
            p = zput(p, " -> alloc base sel 0x1F");
            break;
        default:
            cx->EFlags |= 1u;                           /* CF=1: unsupported function        */
            p = zput(p, " -> UNSUPPORTED (CF=1)");
            break;
        }
        p = zput(p, "\r\n");
        log_append(LOG_PATH, cb, p); serial_out(cb, p);
        cx->SegCs = 0x0F; cx->SegSs = 0x17;             /* restore the guest's LDT selectors  */
        return EXCEPTION_CONTINUE_EXECUTION;            /* resume the guest past the INT      */
    }

    /* --- genuine (non-reflected) PM fault: full dump + clean exit -------------------- */
veh_fatal:
    InterlockedIncrement(&g_veh_fatal);                 /* run 52: a real PM fault WAS delivered */
    p = zput(p, "\r\nDPMI FATAL: exception code=0x"); p = zhex(p, er->ExceptionCode);
    p = zput(p, " at 0x"); p = zhex(p, (unsigned)(ULONG_PTR)er->ExceptionAddress);
    p = zput(p, "\r\n  CS:EIP=0x"); p = zhex(p, cx->SegCs); p = zput(p, ":0x"); p = zhex(p, cx->Eip);
    p = zput(p, " SS:ESP=0x"); p = zhex(p, cx->SegSs); p = zput(p, ":0x"); p = zhex(p, cx->Esp);
    p = zput(p, " EFL=0x"); p = zhex(p, cx->EFlags); p = zput(p, "\r\n");
    p = zput(p, "  DS=0x"); p = zhex(p, cx->SegDs); p = zput(p, " ES=0x"); p = zhex(p, cx->SegEs);
    p = zput(p, " FS=0x"); p = zhex(p, cx->SegFs); p = zput(p, " GS=0x"); p = zhex(p, cx->SegGs);
    p = zput(p, "\r\n  EAX=0x"); p = zhex(p, cx->Eax); p = zput(p, " EBX=0x"); p = zhex(p, cx->Ebx);
    p = zput(p, " ECX=0x"); p = zhex(p, cx->Ecx); p = zput(p, " EDX=0x"); p = zhex(p, cx->Edx);
    p = zput(p, "\r\n");
    { const BYTE *fb = (const BYTE *)(ULONG_PTR)(er->ExceptionAddress);
      p = zput(p, "  bytes@fault: "); p = zdump(p, fb, 16); }
    log_append(LOG_PATH, cb, p);
    serial_out(cb, p);
    ExitProcess(0xDE0);                                 /* clean exit; batch dumps the log */
    return EXCEPTION_CONTINUE_SEARCH;                   /* not reached */
}

/* DPMI test watchdog: if the PM guest neither faults to the VEH nor exits within a few
   seconds (the kernel skip+resumes PM faults, so the guest spins), terminate cleanly so
   the batch dumps the log and locks release. Makes every DPMI run self-terminating. */
static DWORD WINAPI dpmi_watchdog(LPVOID param)
{
    static char wb[512]; char *q = wb; LONG prev = -1; (void)param;
    q = zput(q, "STAGE3-DPMI: watchdog started; sampling host PM-loop heartbeat (run 52 diag)\r\n");
    /* ► LOG, don't just serial. serial_out writes COM1, which exists on the QEMU dev VM
         and NOT on the bare-metal box -- so on the rig these lines went nowhere. That
         cost us a wrong conclusion about Doom (session 15): the absence of wd[] samples
         in result_doom.log was read as "it died before the first 250 ms sample", when in
         fact the samples were never written anywhere. Every diagnostic must reach the
         file log or it does not exist on the machine we actually test on. */
    log_append(LOG_PATH, wb, q); serial_out(wb, q); q = wb;
    /* Sample the host PM loop concurrently while the main thread is (possibly) blocked
       inside dpmi_enter_pm(). Each line answers the run-51 wall question:
         iter ADVANCING  -> the `for steps` loop is cycling; last ev/cs/eip/vec show WHICH
                            patched INT it keeps hitting (a busy-poll = a cheap missing
                            service, not the deep #GP wall).
         iter FROZEN     -> the main thread is wedged inside ONE dpmi_enter_pm at the guest
                            CS:EIP we last handed off; the guest bytes there tell a plain-
                            instruction #GP (the deep wall) from a jmp-self spin.
         veh any/fatal   -> whether a real Win32 exception was EVER delivered to us. fatal>0
                            means the PM #GP IS catchable via SEH (good news); both 0 while
                            frozen confirms the kernel swallowed it (runs 20-34 wall). */
    /* Only a SUSTAINED freeze is a wedge. A healthy client -- especially an animation
       loop -- keeps bumping g_dpmi_iter every frame; it must NOT be killed. So sample
       forever, reset on any progress, and terminate only after ~3s of NO progress.
       Stand down entirely once the client has exited cleanly (g_dpmi_done). Log the
       first 12 samples (the run-51/52 wedge diagnostic) + any frozen streak, and stay
       quiet while healthy so a long run doesn't flood COM1. */
    { unsigned n = 0, frozen = 0;
      for (;;) {
        LONG iter; DWORD en_cs, en_eip, base; const BYTE *ib;
        Sleep(250);
        if (g_dpmi_done) {                              /* client exited cleanly -> keep the window */
            q = zput(q, "STAGE3-DPMI: watchdog stand-down (client done)\r\n");
            log_append(LOG_PATH, wb, q); serial_out(wb, q);
            return 0;
        }
        iter   = g_dpmi_iter;
        frozen = (iter == prev) ? (frozen + 1) : 0;
        if (n < 12 || frozen) {                         /* diagnostic window + any freeze */
            en_cs  = g_dpmi_enter_cs;  en_eip = g_dpmi_enter_eip;
            q = zput(q, "  wd["); q = zhex(q, n);
            q = zput(q, "] iter="); q = zhex(q, (unsigned)iter);
            q = zput(q, frozen ? " FROZEN" : " advancing");
            q = zput(q, " enter="); q = zhex(q, en_cs); q = zput(q, ":"); q = zhex(q, en_eip);
            q = zput(q, " last{ev="); q = zhex(q, g_dpmi_last_ev);
            q = zput(q, " cs:eip="); q = zhex(q, g_dpmi_last_cs); q = zput(q, ":"); q = zhex(q, g_dpmi_last_eip);
            q = zput(q, " vec="); q = zhex(q, g_dpmi_last_vec); q = zput(q, "}");
            q = zput(q, " veh{any="); q = zhex(q, (unsigned)g_veh_any);
            q = zput(q, " fatal="); q = zhex(q, (unsigned)g_veh_fatal); q = zput(q, "}");
            if (frozen && g_dpmi_pm) {                   /* wedged: dump the guest bytes there */
                base = dpmi_sel_base((WORD)en_cs);
                ib = (const BYTE *)(ULONG_PTR)(base + (en_eip & 0xFFFF));
                q = zput(q, " b@enter="); q = zdump(q, ib, 8);
            }
            q = zput(q, "\r\n");
            log_append(LOG_PATH, wb, q); serial_out(wb, q); q = wb;   /* COM1-only = invisible on bare metal */
        }
        prev = iter; ++n;
        if (frozen >= 12) break;                         /* ~3s with NO progress -> real wedge */
      }
    }
    q = zput(q, "STAGE3-DPMI: watchdog terminating (wedged)\r\n");
    log_append(LOG_PATH, wb, q);
    serial_out(wb, q);
    /* ► FLUSH WHAT THE PROGRAM PRINTED BEFORE KILLING IT. Same text the clean
         wind-down emits, in the one path that used to lose it. Written in bounded
         slices because the accumulator is far larger than this thread's buffer. */
    if (g_mach && g_mach->out_len > 0) {
        DWORD off = 0, total = (DWORD)g_mach->out_len;
        q = wb; q = zput(q, "  ==> DOS OUTPUT (wedged): [\r\n");
        log_append(LOG_PATH, wb, q);
        while (off < total) {
            DWORD n2 = total - off; char sl[257];
            if (n2 > 256) n2 = 256;
            { DWORD k; for (k = 0; k < n2; ++k) sl[k] = g_mach->out[off + k]; }
            log_append(LOG_PATH, sl, sl + n2);
            off += n2;
        }
        q = wb; q = zput(q, "\r\n]\r\n");
        log_append(LOG_PATH, wb, q);
    }
    /* TerminateProcess (forceful) -- ExitProcess hangs trying to unwind the PM engine
       thread (un-terminable LDT context). */
    TerminateProcess(GetCurrentProcess(), 0xDD0);
    return 0;
}

static void dpmi_install(int idx);           /* defined just below; used by the helper */

/* A 16-bit CODE selector based on DOS_HDLR_SEG, so the host's own stubs (the 0306
   protected-to-real entry, the 0305 save/restore no-op) have a protected-mode address
   to hand the client. Allocated once, from the same LDT pool the client allocates from,
   and cached -- 0305 and 0306 both want it and a client may call either more than once. */
static WORD g_dpmi_hdlr_sel = 0;
static WORD dpmi_hdlr_code_sel(void)
{
    int idx;
    if (g_dpmi_hdlr_sel) return g_dpmi_hdlr_sel;
    if (g_ldt_next >= DPMI_LDT_MAX) return 0;
    idx = g_ldt_next++;
    g_ldt[idx].base   = (DWORD)DOS_HDLR_SEG << 4;
    g_ldt[idx].limit  = 0xFFFF;
    g_ldt[idx].access = 0xFA;                 /* present, DPL3, code, readable        */
    g_ldt[idx].flags  = 0;                    /* 16-bit: the stubs are 16-bit code    */
    dpmi_install(idx);
    g_dpmi_hdlr_sel = (WORD)((idx << 3) | 7);
    return g_dpmi_hdlr_sel;
}

/* Plant the default PM interrupt handlers and point every vector at them. See the
   note on g_pm_defsel. Called once, at the mode switch, before the client runs. */
static void dpmi_install_default_pm_handlers(dos_machine_t *mp)
{
    uint16_t seg = 0, max = 0;
    volatile BYTE *stub;
    int v, idx;
    char lb[160], *q = lb;
    /* 256 vectors x 3 bytes = 768; one 0x40-paragraph block covers it with room over. */
    if (dos_alloc(NULL, mp->first_mcb, 0x40, &seg, &max) || !seg) return;
    if (g_ldt_next >= DPMI_LDT_MAX) return;
    g_pm_defbase = (DWORD)seg << 4;
    stub = (volatile BYTE *)(ULONG_PTR)g_pm_defbase;
    idx = g_ldt_next++;
    g_ldt[idx].base   = g_pm_defbase;
    g_ldt[idx].limit  = 0x3FF;
    g_ldt[idx].access = 0xFA;                    /* present, DPL3, code, readable */
    g_ldt[idx].flags  = 0;                       /* 16-bit: the stubs are 16-bit  */
    dpmi_install(idx);
    g_pm_defsel = (WORD)((idx << 3) | 7);
    for (v = 0; v < 256; ++v) {
        DWORD off = (DWORD)v * DPMI_PMDEF_STRIDE;
        stub[off + 0] = 0xC4; stub[off + 1] = 0xC4;
        stub[off + 2] = 0xCF;                    /* BOP immediate AND the IRET */
        pmap_set(g_pm_defbase + off, (BYTE)v);   /* resolves the chained BOP -> vector v */
        g_pm_int[v].sel = g_pm_defsel;
        g_pm_int[v].off = off;
        g_pm_int[v].client = 0;
    }
    g_pm_defidx = idx;      /* so the width can be corrected once the client declares it */
    q = zput(q, "DPMI: default PM handlers at 0x"); q = zhex(q, g_pm_defsel);
    q = zput(q, ":0 (linear 0x"); q = zhex(q, g_pm_defbase);
    q = zput(q, ", 256 vectors)\r\n");
    log_append(LOG_PATH, lb, q); serial_out(lb, q);
}

/* (Re)build g_ldt[idx]'s descriptor and install it in the process LDT via svc 10. */
static void dpmi_install(int idx)
{
    DWORD lo, hi, lim = g_ldt[idx].limit; BYTE fl = g_ldt[idx].flags;
    BYTE acc = g_ldt[idx].access;
    WORD sel = (WORD)((idx << 3) | 7);
    if (lim > 0xFFFFF) { lim >>= 12; fl = (BYTE)(fl | 0x8); }   /* >1MB -> page granular */
    /* Run 69 (option C: avoid the kernel PM-fault reflect). On the REAL CPU a data/stack
       selector that a client retypes to CODE (or non-writable) faults the moment it is used
       -- exactly i310102's wall: its C runtime does INT 31h 0009 to set SS (sel 0x1F, idx 3)
       access = 0xFB (code), then the next stack write #GPs and the kernel cannot reflect it
       (runs 51/52), so the VDM silently terminates. The interpreter (runs 53+) survived this
       by NOT enforcing descriptor types. We do the same on the real CPU: the initial DATA (idx
       2 = DS) and STACK (idx 3 = SS) selectors are ALWAYS installed as present writable-data,
       so they stay usable no matter how the client retypes them. g_ldt[idx].access keeps the
       client's requested value, so LAR/LSL introspection (dpmi_sel_desc) still reports it. */
    if (idx == 2 || idx == 3) acc = 0xF2;                      /* present, DPL3, data R/W */
    dpmi_build_desc(g_ldt[idx].base, lim & 0xFFFFF, acc, fl, &lo, &hi);
    {
        /* #3 (DOS/4GW flat model): XP's LDT validator caps base+limit <= MmHighestUserAddress
           (~2GB); a base-0 ~2GB G=1 selector installs, a true 4GB one is REJECTED (Kernel RE
           session 7). Surface the NTSTATUS so a rejected flat/large descriptor is visible
           instead of silently leaving a stale selector that faults on first use (run 84). */
        LONG st = v86_set_ldt_entries(sel, lo, hi, sel, lo, hi); /* idempotent single-entry */
        if (st != 0) {
            /* ── CLAMP AND RETRY, don't just report ────────────────────────────────
               DOS/4GW (Doom) allocates a base-0 4GB G=1 FLAT selector and XP rejects it
               (0xC000011A): PspIsDescriptorValid requires
                   base + (G ? ((limit<<12)|0xFFF) : limit) <= MmHighestUserAddress.
               Merely logging the refusal leaves the client holding a selector that is
               not installed, so its first flat access faults -- which is a silent death,
               exactly the failure mode this project keeps being bitten by. XP cannot be
               talked into a true 4GB LDT selector (Kernel RE session 7: stock ntvdm is
               under the same cap), so the honest best effort is the largest descriptor
               the validator WILL take, which run 30 already showed installs: base 0,
               limit 0x7FFEF, G=1.
               The clamp is LOUD -- a client that then walks off the end of a segment it
               believes is 4GB must not look like a mystery. */
            DWORD cap = XP_LDT_MAX_LINEAR;
            DWORD clo = lo, chi = hi, want = g_ldt[idx].limit, cl = 0;
            LONG st2 = st;
            if (g_ldt[idx].base < cap) {
                DWORD room = cap - g_ldt[idx].base;
                if (fl & 0x8) cl = (room > 0xFFF) ? ((room - 0xFFF) >> 12) : 0;  /* G=1 */
                else          cl = room;                                        /* G=0 */
                if (cl > 0xFFFFF) cl = 0xFFFFF;
                dpmi_build_desc(g_ldt[idx].base, cl, acc, fl, &clo, &chi);
                st2 = v86_set_ldt_entries(sel, clo, chi, sel, clo, chi);
            }
            {
                char lb[256], *p = lb;
                p = zput(p, "DPMI-LDT: install REJECTED sel 0x"); p = zhex(p, sel);
                p = zput(p, " base 0x"); p = zhex(p, g_ldt[idx].base);
                p = zput(p, " limit 0x"); p = zhex(p, want);
                p = zput(p, " g="); p = zhex(p, (DWORD)((fl >> 3) & 1));
                p = zput(p, " status 0x"); p = zhex(p, (DWORD)st);
                if (st2 == 0) { p = zput(p, " -> CLAMPED to limit 0x"); p = zhex(p, cl);
                                p = zput(p, " (XP LDT cap) and installed"); }
                else          { p = zput(p, " -> clamp ALSO refused, status 0x");
                                p = zhex(p, (DWORD)st2); }
                p = zput(p, "\r\n");
                log_append(LOG_PATH, lb, p); serial_out(lb, p);
            }
        }
    }
}

/* GH #18 (run 67 corrected): install the PM-fault reflect machinery. Two LDT selectors:
   a writable-DATA stack selector (g_dpmi_fault_sel, based at DPMI_FAULT_STK_SEG<<4 so
   its :0x1000 is a valid scratch stack top) written to [TIB+0x638]; and a CODE selector
   (g_dpmi_flt_code_sel, based at DOS_HDLR_SEG<<4) with a BOP at DPMI_FAULT_COFF. The
   handler table g_flt_tbl[class]=({code_sel,COFF}) is what the kernel reads via
   [VDM_TIB+8] to set the reflected CS:EIP. Allocated once from the g_ldt[] pool. */
static void dpmi_install_fault_trampoline(void)
{
    int si, ci; unsigned i;
    volatile BYTE *hdlr = (volatile BYTE *)(ULONG_PTR)((DWORD)DOS_HDLR_SEG << 4);
    if (g_dpmi_fault_sel) return;                  /* already installed */
    if (g_ldt_next >= 510) return;
    /* the handler CODE selector + its BOP */
    ci = g_ldt_next++;
    g_ldt[ci].base   = (DWORD)DOS_HDLR_SEG << 4;   /* 0x500 -> code:COFF = linear 0x580 */
    g_ldt[ci].limit  = 0xFFFF;
    g_ldt[ci].access = 0xFA;                       /* code exec/read, DPL3, present     */
    g_ldt[ci].flags  = 0;
    dpmi_install(ci);
    g_dpmi_flt_code_sel = (WORD)((ci << 3) | 7);
    hdlr[DPMI_FAULT_COFF + 0] = VDM_BOP0;          /* plant C4 C4 57 at code:COFF (0x580) */
    hdlr[DPMI_FAULT_COFF + 1] = VDM_BOP1;
    hdlr[DPMI_FAULT_COFF + 2] = DPMI_FAULT_BOP;
    /* the handler STACK selector (writable-data) */
    si = g_ldt_next++;
    g_ldt[si].base   = (DWORD)DPMI_FAULT_STK_SEG << 4;  /* 0x2000 -> stack:0x1000 = 0x3000 */
    g_ldt[si].limit  = 0xFFFF;
    g_ldt[si].access = 0xF2;                       /* data read/write, DPL3, present      */
    g_ldt[si].flags  = 0;
    dpmi_install(si);
    g_dpmi_fault_sel = (WORD)((si << 3) | 7);
    /* the per-fault-class handler table: entry N (stride 0x10) = {CS@+0 word, EIP@+4 dword}.
       Fill the #GP class (6) with our code selector + BOP offset; zero the rest. */
    for (i = 0; i < sizeof g_flt_tbl; ++i) g_flt_tbl[i] = 0;
    *(WORD  *)(g_flt_tbl + DPMI_FLT_CLASS_GP * 0x10 + 0) = g_dpmi_flt_code_sel;
    *(DWORD *)(g_flt_tbl + DPMI_FLT_CLASS_GP * 0x10 + 4) = DPMI_FAULT_COFF;
}

/* GH #18 (run 67): arm the VDM_TIB PM-fault reflect state before each PM entry. The kernel
   takes the "first level, save" path only when the nest counter is 0 (then inc's it), so
   this runs before EVERY dpmi_enter_pm. Sets: nest=0, the 16/32 flag, the handler STACK
   selector at [TIB+0x638], and the handler-table pointer at [VDM_TIB+8]. */
static void dpmi_arm_fault_trampoline(volatile BYTE *tib, WORD flag)
{
    if (!g_dpmi_fault_sel) return;
    *(volatile WORD  *)(tib + VTIB_FLT_NEST)  = 0;
    *(volatile WORD  *)(tib + VTIB_FLT_FLAG)  = flag;
    *(volatile WORD  *)(tib + VTIB_FLT_HSEL)  = g_dpmi_fault_sel;         /* stack selector */
    *(volatile DWORD *)(tib + DPMI_TIB_FLTTBL) = (DWORD)(ULONG_PTR)g_flt_tbl;
}

/* Linear base of a selector, for INT 21h pointer thunks. */
static DWORD dpmi_sel_base(WORD sel)
{
    int idx = (sel & 0xFFFF) >> 3;
    /* Indices 1..3 are the switch's code/data/stack selectors (recorded in g_ldt[]
       from g_dpmi_seg_base); 3+ are client allocations. For a .COM all three bases
       equal g_dpmi_code_base; for a real .EXE (CS!=DS!=SS) they differ, so a per-
       selector lookup is required to translate DS:/ES: buffers correctly. */
    if (idx >= 1 && idx < DPMI_LDT_MAX) return g_ldt[idx].base;
    return g_dpmi_code_base;                                    /* null / unknown selector */
}

/* Descriptor introspection for the PM interpreter's LAR/LSL (run 55). Returns 1 if
   `sel` names a populated descriptor (access byte != 0) and fills the access-rights
   in LAR format (access byte at bits 8-15, the G/D/AVL flag nibble at 20-23) + the
   byte-granular limit. The null selector (idx 0) and unallocated slots are invalid. */
static int dpmi_sel_desc(uint16_t sel, uint32_t *ar, uint32_t *limit)
{
    int idx = (sel & 0xFFFF) >> 3;
    if (idx < 1 || idx >= 512 || g_ldt[idx].access == 0) return 0;
    if (ar)    *ar    = ((uint32_t)g_ldt[idx].access << 8) | ((uint32_t)(g_ldt[idx].flags & 0xF) << 20);
    if (limit) *limit = (uint32_t)g_ldt[idx].limit;
    return 1;
}

/* Resolve a PM BOP at CS:EIP back to the original interrupt vector.
   ► WHY THIS IS NOT JUST g_int_vec[eip]. The switch-time scan rewrote every `CD nn` in
     a 64K window at g_dpmi_code_base, and g_int_vec[] is keyed by OFFSET INTO THAT
     WINDOW. A client may reach the very same physical bytes through a DIFFERENT
     selector, and DOS/4GW does exactly that: it builds a second code selector (0x57,
     base 0xd9b0) for its protected-mode interrupt handlers, which overlaps the patched
     window. A BOP then fires at 0x57:0x558c -- linear 0xF33C, i.e. window offset
     0x969C -- and an EIP-keyed lookup asks for g_int_vec[0x558c], finds nothing, and
     the run dies as "unexpected PM stop". Resolve through the LINEAR address so any
     alias of the patched memory maps back to the right vector. */
static DWORD dpmi_bop_vec(DWORD csv, DWORD eip)
{
    return pmap_get(dpmi_sel_base((WORD)csv) + eip);
}

/* ── AN EIP IS ONLY 16 BITS WIDE WHEN ITS CODE SELECTOR IS. ───────────────────────
   Every PM stop used to read `VDM_REG(tib, VTIB_EIP) & 0xFFFF`, which is right for the
   16-bit selectors this host grew up on and WRONG the moment a client runs 32-bit code
   in a flat selector -- there, EIP is a full linear address and the top sixteen bits
   are the address, not junk to be discarded.
   It cost a whole diagnosis to see. Doom's first `int 21h` (AH=30h, get DOS version,
   at obj1+0x40be5) fired our BOP exactly as intended at linear 0x03b10be5, and the
   masked EIP turned the lookup into `pmap_get(0x0be5)` -- a different address, in low
   memory, with nothing recorded there. The run then died as "unexpected PM stop" AT THE
   VERY INSTRUCTION THAT PROVED THE PATCH WORKED, and the log said 0x187:0x0be7, which
   reads like a wild jump into the BIOS data area rather than what it was.
   This is the same rule the interrupt-frame width already follows: ask the descriptor,
   not the host's habits. dpmi_sel_is32() reads the D/B bit we store for the selector. */
static DWORD dpmi_pm_eip(volatile BYTE *tib)
{
    DWORD e = VDM_REG(tib, VTIB_EIP);
    return dpmi_sel_is32((WORD)(VDM_REG(tib, VTIB_CS) & 0xFFFF)) ? e : (e & 0xFFFF);
}

/* ── PATCH A REGION THE CLIENT HAS JUST DECLARED TO BE CODE. ──────────────────────
   Called from INT 31h 0009/000C when the resulting descriptor is a CODE type. The
   TIMING is the client's, not ours, and it is right: Doom's trace shows AH=48h
   allocate -> AH=3Fh read the module in -> 0009 retype to code -> far jump. So at the
   moment the client says "code", the bytes are already in memory and have not yet been
   executed -- the only window in which patching is both possible and safe.
   Scanning a data region would be the dangerous thing (a `CD 21` byte pair that is
   really data gets corrupted); scanning only what the client itself calls code is the
   narrowest rule that covers the case, and g_int_vec[] remains the revert map. */
static void dpmi_bp_arm(void);               /* fwd: a new region may hold a requested BP */
static void dpmi_bp_rearm_pending(DWORD cur_lin);   /* fwd: re-plant stepped-over breakpoints */
/* `d32` is the region's DEFAULT OPERAND SIZE -- the D/B bit of the descriptor that named
   it code -- and it is not optional: instruction lengths differ between the two, so
   decoding DOS/4GW's 16-bit modules as 32-bit rejects obvious real sites (`mov ax,4c00h
   / int 21h` scored zero votes, measured). Where the width is genuinely unknown the scan
   is idempotent and self-healing: a site rejected under the wrong width is not recorded
   in the patch map, so the next pass -- and there is always a next pass, because the
   client declares its regions repeatedly while it loads -- gets another chance at it. */
static void dpmi_patch_code_region(DWORD base, DWORD limit, int d32)
{
    volatile BYTE *mem;
    DWORD end, n = 0, rej = 0;
    char lb[192], *q = lb;
    /* ► NEVER PATCH THE IVT / BIOS DATA AREA, even when the client declares a base-0
         code selector -- and Doom does exactly that (sel 0x67, base 0, limit 0xFFFF),
         which made the first version of this scan rewrite 16 sites in linear 0..0xFFFF.
         An interrupt vector is a word pair, so `CD 21` is a perfectly ordinary VALUE
         down there: vector n = 0x21CD would be silently turned into 0xC4C4 and the
         guest would jump into hyperspace on the next INT n. Below 0x600 is IVT + BDA +
         our own handler segment prologue: definitionally data, never executed as the
         client's code. Start the scan above it. */
    end = base + limit;                              /* limit is the LAST valid byte */
    if (base < 0x600) base = 0x600;                  /* ...but the region END is unchanged */
    /* No upper bound any more: the regions that matter live in EXTENDED memory, which is
       where a working extender puts its modules. The size cap is a sanity bound rather
       than a policy -- a multi-megabyte "code" region is a flat alias, not a module. */
    if (end <= base) return;
    /* ► A FLAT CODE SELECTOR CANNOT BE SCANNED, BUT THE CLIENT'S MEMORY CAN. Doom's own
         32-bit code selector is `setaccess 0xC7FA` -- present, DPL3, code, G=1, D/B=1,
         base 0, limit 4 GB. Scanning that range is impossible and would mis-patch every
         byte of the address space, so we used to refuse it outright and the application's
         raw `CD 21` / `CD 31` were left unpatched -- which is the one fault XP will not
         reflect. The usable answer is that WE KNOW WHICH MEMORY IS THE CLIENT'S: it is
         what we handed out through 0501. So for an oversized region, scan those blocks
         (clipped to the region) instead of the range.
         Timing is on our side: the client declares its code selector once its image is
         loaded and long before it reads data files, so the blocks in play at that moment
         are code. The count is logged -- thousands of "INT sites" would mean we are
         patching data, and that is the number to look at if something later reads wrong. */
    if ((end - base) > 0x00400000u) {
        /* ► A FLAT CODE SELECTOR, AND WE DO NOT HAVE AN ANSWER FOR IT YET. Doom's own
             32-bit code selector is `setaccess 0xC7FA` -- present, DPL3, code, G=1,
             D/B=1, base 0, limit 4 GB. Scanning that range is impossible, so the
             application's raw `CD 21` / `CD 31` go unpatched, and a raw INT in protected
             mode is the one fault XP will not reflect.
             ▶ SCANNING THE CLIENT'S OWN 0501 BLOCKS INSTEAD WAS TRIED AND IS WORSE
               (measured, session 17): it patched three extra byte pairs inside the 1 MB
               block that already held the extender's modules -- i.e. DATA -- and the run
               ended EARLIER than without it. The client's memory is code and data mixed;
               "everything we handed out" is not a code region.
             ▶ What is needed is a way to know which parts of the client's memory are
               code. The client knows: it loads objects with a flag. We do not see that
               flag, but we DO see every window descriptor it builds over each object
               (0006 + 000C) while relocating -- that is the shape of the answer.
             g_dpmi_blk[] is recorded and ready for whoever takes this on. */
        q = zput(q, "DPMI: FLAT code region 0x"); q = zhex(q, base);
        q = zput(q, "..0x"); q = zhex(q, end);
        q = zput(q, " -- not scanned as a range (");
        q = zhex(q, (DWORD)g_dpmi_nblk); q = zput(q, " blocks known, ");
        q = zhex(q, (DWORD)g_le_ncode); q = zput(q, " LE code sizes)\r\n");
        log_append(LOG_PATH, lb, q); serial_out(lb, q);
        return;
    }
    /* ► WALK THE COMMITTED REGIONS, DO NOT WALK THE ADDRESS RANGE. A declared code
         region is the CLIENT's idea of what is code, and a client hands us ranges that
         span memory nobody has mapped -- Doom's own runtime declares a base-0 1 MB
         selector, and the low megabyte has holes. Scanning straight through faulted
         inside this very loop (caught by the VEH: `cmp al,0xcd` with EDX=0xd4000), i.e.
         our own scanner killing the run it exists to help. Probing every N bytes is not
         good enough either -- it guesses at a boundary the memory manager already knows.
         Ask it: VirtualQuery hands back exactly the committed, readable extents. */
    { DWORD a = base;
      while (a < end) {
          MEMORY_BASIC_INFORMATION mbi;
          DWORD rend, i;
          if (VirtualQuery((LPCVOID)(ULONG_PTR)a, &mbi, sizeof mbi) != sizeof mbi) break;
          rend = (DWORD)((ULONG_PTR)mbi.BaseAddress + mbi.RegionSize);
          if (rend <= a) break;                      /* no progress -> stop, never spin */
          if (rend > end) rend = end;
          if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
              mem = (volatile BYTE *)(ULONG_PTR)a;
              for (i = 0; i + 1 < (rend - a); ++i) {
                  /* ⚠ ADDING 0x32/0x34/0x35/0x36 HERE WAS TRIED AND IS NOT THE ANSWER.
                       Doom's Watcom thunk table at obj1+0x41e41 is `[cd nn][c3]` triples and
                       the log dump shows only INT 33h patched:
                           c3  cd 32 c3  c4 c4 c3  cd 34 c3  cd 35 c3  cd 36 c3
                       A raw `CD nn` in PM is the fault XP will not reflect, so those looked
                       like the silent teardown. Measured (session 19): patching them changes
                       NOTHING -- same tick count, same INT 31h total, and vectors 32/34/35/36
                       are NEVER SERVICED, i.e. the guest never calls those thunks. Reverted
                       rather than left in as unverified scan surface. The guest enters the
                       table only at the `c3` tail of the already-patched INT 31h thunk. */
                  if (mem[i] == 0xCD && (mem[i+1] == 0x31 || mem[i+1] == 0x21 || mem[i+1] == 0x10
                                         || mem[i+1] == 0x16 || mem[i+1] == 0x33
                                         || mem[i+1] == 0x1A || mem[i+1] == 0x08)) {
                      DWORD lin = a + i;
                      if (pmap_get(lin)) continue;   /* already patched (aliased region) */
                      /* ── ONLY IF IT IS AN INSTRUCTION. ───────────────────────────────
                           This scan used to take the byte pair as proof, and in Doom's
                           code object that is wrong in three places -- one of them
                           fatally. At obj1+0x3593f the stream is
                               39 fa  7e cd  31 c9    cmp edx,edi / jle -51 / xor ecx,ecx
                           and the `cd 31` is the jle's DISPLACEMENT followed by the xor's
                           opcode. Patching it made R_InitTextureMapping's loop-2 back edge
                           jump to obj1+0x35905 -- the middle of a `jl` -- where the guest
                           executed `cmp ecx,[ebx+0x034fe02d]` with an ANGLE in ebx, read
                           an unmapped address, and XP tore the VDM down with no VEH, no
                           watchdog line and no last log entry. Sessions 16-20 hunted that
                           as a fault in Doom. It was this line.
                           x86_is_insn_start() decodes forward from each of the preceding
                           48 bytes and asks how many streams land here; see x86len.h for
                           the measured separation (real sites 19-48 votes, false pairs
                           0-3) and why the threshold leans toward keeping. */
                      if (!x86_int_site_is_real((const unsigned char *)(ULONG_PTR)a, i,
                                                rend - a, d32)) {
                          if (rej++ < 16) {
                              char rb[128], *rq = rb;
                              rq = zput(rq, "DPMI: NOT an INT site (mid-instruction) 0x");
                              rq = zhex(rq, lin);
                              rq = zput(rq, " vec=0x"); rq = zhexb(rq, mem[i+1]);
                              rq = zput(rq, d32 ? " d32=1" : " d32=0");
                              rq = zput(rq, " ctx="); rq = zdump(rq, (const BYTE *)(ULONG_PTR)(lin - 4), 10);
                              rq = zput(rq, "\r\n");
                              log_append(LOG_PATH, rb, rq); serial_out(rb, rq);
                          }
                          continue;
                      }
                      pmap_set(lin, mem[i+1]);
                      mem[i] = 0xC4; mem[i+1] = 0xC4;
                      ++n;
                  }
              }
          }
          a = rend;
      } }
    q = zput(q, "DPMI: code region 0x"); q = zhex(q, base);
    q = zput(q, "..0x"); q = zhex(q, end);
    q = zput(q, " -> patched "); q = zhex(q, n); q = zput(q, " INT sites, rejected ");
    q = zhex(q, rej); q = zput(q, " mid-instruction byte pairs\r\n");
    log_append(LOG_PATH, lb, q); serial_out(lb, q);
    dpmi_bp_arm();          /* a module that has just appeared may hold a requested BP */
}

/* Learn the program's EXECUTABLE object sizes from its own LE header. See the commentary
   on g_le_code_sz. The image is already in `filebuf` -- the loader read it to run the
   MZ stub -- so this costs one pass over memory we are holding anyway and no file I/O.
   ► THE HEADER IS FOUND BY SEARCH, NOT BY e_lfanew. In a bound executable the MZ stub is
     the extender (DOS/4GW), and its e_lfanew is not a pointer to the LE at all -- on
     DOOM.EXE it reads 0x09b40000, i.e. off the end of a 0xad511-byte file. Every offset
     INSIDE the LE header is relative to the header, not the file, for the same reason.
   ► AND THE CANDIDATE IS VALIDATED, because "LE\0\0" is two ASCII letters and two zeroes
     and occurs in data by chance. Byte/word order little-endian, format level 0, a 386+
     CPU, a plausible OS and object count: five agreeing fields, which no accident of
     data passed on any binary tried here. */
static void dpmi_le_learn(const BYTE *buf, DWORD n)
{
    DWORD i;
    char lb[192], *q;
    if (!buf || n < 0x200) return;
#define LE32(o) ((DWORD)buf[(o)] | ((DWORD)buf[(o)+1] << 8) | ((DWORD)buf[(o)+2] << 16) | ((DWORD)buf[(o)+3] << 24))
#define LE16(o) ((DWORD)buf[(o)] | ((DWORD)buf[(o)+1] << 8))
    for (i = 0; i + 0x100 < n; ++i) {
        DWORD objoff, nobj, k;
        if (buf[i] != 'L' || buf[i+1] != 'E' || buf[i+2] || buf[i+3]) continue;
        if (LE32(i + 0x04) != 0) continue;                     /* format level      */
        { DWORD cpu = LE16(i + 0x08), os = LE16(i + 0x0A);
          if (cpu < 1 || cpu > 4 || os < 1 || os > 4) continue; }
        nobj   = LE32(i + 0x44);
        objoff = LE32(i + 0x40);
        if (nobj < 1 || nobj > 64) continue;
        if (objoff < 0x50 || i + objoff + nobj * 24 > n) continue;
        q = lb; q = zput(q, "DPMI: LE image at file 0x"); q = zhex(q, i);
        q = zput(q, ", "); q = zhex(q, nobj); q = zput(q, " objects\r\n");
        log_append(LOG_PATH, lb, q);
        for (k = 0; k < nobj; ++k) {
            DWORD o     = i + objoff + k * 24;
            DWORD vsize = LE32(o + 0x00), flags = LE32(o + 0x08);
            DWORD pr    = (vsize + 0xFFFu) & ~0xFFFu;          /* what 0501 will ask for */
            int   exec  = (flags & 0x0004u) != 0;
            q = lb; q = zput(q, "DPMI:   obj"); q = zhex(q, k + 1);
            q = zput(q, " vsize 0x"); q = zhex(q, vsize);
            q = zput(q, " flags 0x"); q = zhex(q, flags);
            q = zput(q, exec ? " EXEC" : " data");
            if (exec && pr >= 0x10000u && g_le_ncode < DPMI_LE_MAX) {
                g_le_code_sz[g_le_ncode++] = pr;
                q = zput(q, " -- code block size 0x"); q = zhex(q, pr);
            } else if (exec) {
                q = zput(q, " -- too small to key on (a based descriptor will reach it)");
            }
            q = zput(q, "\r\n");
            log_append(LOG_PATH, lb, q);
        }
        return;                                                /* first valid LE wins */
    }
#undef LE32
#undef LE16
}

/* Patch the INT sites in every block that holds an EXEC object. Called whenever the
   picture may have changed -- a new block, or the client naming a region code.
   ► WHY IT IS CALLED REPEATEDLY RATHER THAN ONCE. The block is allocated EMPTY and
     filled afterwards, and we never see the fill: DOS/4GW reads through a low-memory
     transfer buffer and copies up in its own code. On DOOM.EXE the code object's block
     is allocated ~1100 log lines before its last page arrives. There is no single event
     that means "loaded", so this is idempotent and cheap instead: sites already in the
     patch map are skipped, and the map drops entries whose bytes the guest has since
     overwritten, so a later pass re-patches what a copy undid. */
static void dpmi_scan_code_blocks(void)
{
    int i;
    for (i = 0; i < g_dpmi_nblk; ++i)
        if (g_dpmi_blk[i].code)
            dpmi_patch_code_region(g_dpmi_blk[i].base, g_dpmi_blk[i].size - 1,
                                   g_dpmi_client32);
}

/* A descriptor access byte names CODE iff it is a segment (S, bit 4) and executable
   (bit 3). 0xFB -- what DOS/4GW writes -- is present/DPL3/S/code/readable/accessed. */
#define DPMI_ACC_IS_CODE(a) (((a) & 0x18) == 0x18)

/* Read PMBP_PATH. One line per breakpoint:
       <hex linear addr to break on>  [hex linear addr to DUMP on hit]   # comment
   The optional second column is what makes this a debugger rather than a tracer:
   "stop here and show me that memory" is the question you actually have when a
   register holds a value you cannot explain -- as BX=0x8b17 did, read from a PSP
   field whose contents nothing in the log could show. '#' comments, blanks ignored.
   Absent file = no breakpoints and no cost, like every other knob here. */
static void dpmi_bp_load(void)
{
    HANDLE h = CreateFileA(PMBP_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
    char buf[1024]; DWORD rd = 0, i = 0;
    if (h == INVALID_HANDLE_VALUE) return;
    ReadFile(h, buf, sizeof buf - 1, &rd, NULL);
    CloseHandle(h);
    while (i < rd && g_bp_n < DPMI_BP_MAX) {
        DWORD v[5] = { 0, 0, 0, 0, 0 }; int col = 0;
        /* consume one LINE, taking up to two hex fields from it */
        while (i < rd && (buf[i] == '\r' || buf[i] == '\n')) ++i;   /* line breaks */
        if (i >= rd) break;
        if (buf[i] == '#') { while (i < rd && buf[i] != '\n') ++i; continue; }
        while (i < rd && buf[i] != '\r' && buf[i] != '\n') {
            int digits = 0;
            while (i < rd && (buf[i] == ' ' || buf[i] == '\t')) ++i;
            if (i >= rd || buf[i] == '\r' || buf[i] == '\n') break;
            if (buf[i] == '#') { while (i < rd && buf[i] != '\n') ++i; break; }
            while (i < rd) {
                char c = buf[i];
                int d = (c >= '0' && c <= '9') ? c - '0'
                      : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                      : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
                if (d < 0) break;
                if (col < 5) v[col] = (v[col] << 4) | (DWORD)d;
                ++digits; ++i;
            }
            if (digits) ++col;
            else ++i;                              /* junk byte: skip, never spin */
        }
        if (col >= 1) {
            g_bp_lin[g_bp_n] = v[0];
            g_bp_dump[g_bp_n] = (col >= 2) ? v[1] : 0;
            g_bp_skip[g_bp_n] = (col >= 3) ? v[2] : 0;
            g_bp_mode[g_bp_n] = (col >= 4) ? v[3] : 0;
            g_bp_rep[g_bp_n]  = (col >= 5) ? v[4] : 0;
            ++g_bp_n;
        }
    }
}

/* Arm any requested breakpoint whose address is now present in guest memory. Called
   after the up-front INT scan and after every code-region patch, because a module the
   client loads at runtime does not exist to be patched before then. */
static void dpmi_bp_arm(void)
{
    int k;
    for (k = 0; k < g_bp_n; ++k) {
        DWORD lin = g_bp_lin[k];
        volatile BYTE *b;
        char lb[128], *q = lb;
        if (lin < 0x600) continue;
        b = (volatile BYTE *)(ULONG_PTR)lin;
        /* ► THE TARGET NEED NOT EXIST YET, and reading it blindly faults in OUR OWN
             process. A breakpoint is usually placed on a module the client has not
             loaded at the time the list is read -- extended-memory addresses are not
             even allocated until the client asks for them -- so this arming pass runs
             repeatedly and must tolerate an address that is currently nothing. Same
             lesson as IsBadReadPtr: an instrument that faults kills the run it exists
             to observe. */
        if (!host_readable((const void *)(ULONG_PTR)lin, 2)) continue;
        /* ► RE-ARM IF THE CLIENT OVERWROTE US, and skip empty memory entirely. The
             first version armed at mode-switch time into memory the client had not
             loaded yet -- every breakpoint reported "was 00 00", the module was then
             READ IN OVER THE TOP, and not one of them fired. The instrument looked
             like it worked (twelve confident "armed at" lines) and measured nothing,
             which is this project's most familiar failure mode.
             So: an armed breakpoint whose bytes are no longer our BOP has been
             clobbered and must be re-armed against the NEW contents; and a site that
             is still 00 00 holds nothing to break on, so leave it unarmed and try
             again after the next region is loaded. */
        if (g_bp_armed[k]) {
            if (g_bp_mode[k] == 1 ? (b[0] == 0xCC)
                                  : (b[0] == 0xC4 && b[1] == 0xC4)) continue;  /* still planted */
            g_bp_armed[k] = 0; pmap_clear(lin);            /* clobbered -> re-arm below */
        }
        if (b[0] == 0x00 && b[1] == 0x00) continue;        /* nothing loaded here yet */
        if (pmap_get(lin)) continue;              /* an INT site already lives here */
        /* ► A BREAKPOINT HAS A TWO-BYTE FOOTPRINT, and that is not a detail. It
             displaces the byte AFTER the one you named, so a breakpoint on a ONE-BYTE
             instruction eats its neighbour. Session 17 put one on a `c3` (ret) at
             0x4a0b; the next byte, 0x4a0c, was the entry point of the routine being
             called two instructions earlier. `call 0x4a0c` therefore landed on the
             second half of our BOP, decoded as `LES DX,[BX+0x8b]`, read past the
             segment limit and killed the VDM -- a death the log presented as the
             client's, in the middle of a bisection hunting exactly that.
             We cannot tell where instructions start, so we cannot prevent this in
             general. What we CAN do is refuse the case that is definitely wrong -- two
             requested breakpoints whose footprints overlap -- and say the footprint out
             loud in every armed line, so the next reader places them knowing the rule:
             PUT A BREAKPOINT ON AN INSTRUCTION OF AT LEAST TWO BYTES, or make sure the
             following byte is not reachable before the breakpoint fires. */
        { int j, clash = 0;
          for (j = 0; j < g_bp_n; ++j)
              if (j != k && g_bp_armed[j] &&
                  (g_bp_lin[j] == lin + 1 || g_bp_lin[j] + 1 == lin)) clash = 1;
          if (clash) {
              q = zput(q, "DPMI-BP: REFUSED 0x"); q = zhex(q, lin);
              q = zput(q, " -- its 2-byte footprint overlaps another breakpoint\r\n");
              log_append(LOG_PATH, lb, q); serial_out(lb, q);
              continue;
          } }
        g_bp_orig[k][0] = b[0]; g_bp_orig[k][1] = b[1];
        if (g_bp_mode[k] == 1) {
            b[0] = 0xCC;                      /* INT3: one byte, fits over CLI/STI */
        } else {
            b[0] = 0xC4; b[1] = 0xC4;
            pmap_set(lin, DPMI_BP_VEC);       /* only a BOP is resolvable by vector  */
        }
        g_bp_armed[k] = 1; g_bp_pending[k] = 0;
        q = zput(q, "DPMI-BP: armed at linear 0x"); q = zhex(q, lin);
        q = zput(q, "..0x"); q = zhex(q, lin + 1);       /* say the 2-byte footprint */
        q = zput(q, " (displaced "); q = zdump(q, (const BYTE *)g_bp_orig[k], 2);
        q = zput(q, ")\r\n");
        log_append(LOG_PATH, lb, q); serial_out(lb, q);
    }
}

/* Re-plant any breakpoint that was stepped over, once the guest is no longer standing
   on its footprint. Called at every PM event, which is the first safe moment. */
static void dpmi_bp_rearm_pending(DWORD cur_lin)
{
    int k, any = 0;
    for (k = 0; k < g_bp_n; ++k)
        if (g_bp_pending[k] && cur_lin != g_bp_lin[k] && cur_lin != g_bp_lin[k] + 1) {
            g_bp_pending[k] = 0; any = 1;
        }
    if (any) dpmi_bp_arm();
}

/* Disarm the breakpoint at `lin` (restore its bytes). Returns its index, or -1. */
static int dpmi_bp_disarm(DWORD lin)
{
    int k;
    for (k = 0; k < g_bp_n; ++k)
        if (g_bp_armed[k] && g_bp_lin[k] == lin) {
            volatile BYTE *b = (volatile BYTE *)(ULONG_PTR)lin;
            b[0] = g_bp_orig[k][0];
            if (g_bp_mode[k] != 1) b[1] = g_bp_orig[k][1];
            pmap_clear(lin); g_bp_armed[k] = 0;
            return k;
        }
    return -1;
}

/* Un-patch / re-patch the shared code segment around a V86 excursion (INT 31h 0301/
   0303). The switch-time scan rewrote every `CD 31`/`CD 21` in the code segment to a
   BOP so PM software-ints reflect to us -- but that segment is ALSO the V86 view, so a
   real-mode INT inside a 0301 proc would hit a corrupted BOP instead of vectoring
   natively. g_int_vec[] is the revert map (offset -> original vector), so we restore
   the real `CD nn` bytes before running V86 (real-mode ints then vector through the
   IVT to our BOP stubs and are serviced normally) and re-apply the BOP patch before
   resuming the PM client. */
/* ── THE PATCH MAP MUST VERIFY BEFORE IT WRITES. ──────────────────────────────────
   These used to rewrite every recorded site unconditionally, and that CORRUPTS LIVE
   DATA. Doom found it: the client declares a base-0 64K code selector, so the scan
   records `CD nn` pairs all over low memory -- including addresses that later become
   DOS/4GW's FILE TRANSFER BUFFER. Every INT 31h 0301/0302 unpatches, runs the real-mode
   read (which fills that buffer with image bytes), then repatches -- stamping C4 C4 over
   two bytes of freshly-read program image. The client copied that up to extended memory
   and its relocation pass then read 0xC4C4 where an object index belonged:
       file    9a a4 59 80 | 00 83 | c4 08     lcall 0x0080:0x59a4 / add sp,8
       memory  9a a4 59 80 | c4 c4 | c4 08
   Verifying first makes the map SELF-CORRECTING: if the bytes are no longer what we put
   there, the guest has reused that memory, so the site is stale -- drop it and never
   touch those bytes again. That is strictly better than trying to predict which regions
   the guest will reuse, which is not knowable. */
static void dpmi_unpatch(void)
{
    DWORD sl;
    for (sl = 0; sl < DPMI_PMAP_SLOTS; ++sl) {
        DWORD a = g_pmap_lin[sl]; BYTE v = g_pmap_vec[sl];
        if (!a || !v || v == DPMI_BP_VEC) continue;           /* a BP is not an INT site */
        { volatile BYTE *b = (volatile BYTE *)(ULONG_PTR)a;
          if (b[0] == 0xC4 && b[1] == 0xC4) { b[0] = 0xCD; b[1] = v; }
          else g_pmap_vec[sl] = 0; }                          /* stale: guest reused it */
    }
}
static void dpmi_repatch(void)
{
    DWORD sl;
    for (sl = 0; sl < DPMI_PMAP_SLOTS; ++sl) {
        DWORD a = g_pmap_lin[sl]; BYTE v = g_pmap_vec[sl];
        if (!a || !v || v == DPMI_BP_VEC) continue;           /* a BP stays planted */
        { volatile BYTE *b = (volatile BYTE *)(ULONG_PTR)a;
          if (b[0] == 0xCD && b[1] == v) { b[0] = 0xC4; b[1] = 0xC4; }
          else g_pmap_vec[sl] = 0; }                          /* stale: guest reused it */
    }
}

/* Forward decl: the shared PM-interrupt dispatcher (defined after this fn). A callback
   handler that issues its own INT 31h/21h routes through it, same as the main PM loop. */
/* ── THE DEFAULT PM STUBS MUST BE AS WIDE AS THE CLIENT. ─────────────────────────────
     Each default vector is `C4 C4 CF`: a BOP we service, then an IRET that returns to
     whoever chained here. The selector was built 16-bit unconditionally ("the stubs are
     16-bit"), which is right for a 16-bit client and WRONG for DOS/4GW -- and Doom chains
     to it. Measured: Doom's timer ISR runs clean for five ticks, then on the sixth it
     chains to the previous vector-8 handler, which INT 31h 0204 reported as our default
     stub (`0x37:0x18`). We service the BOP, advance past it, and the guest is left at
     `0x37:0x1a` about to execute `CF` -- a SIXTEEN-BIT IRET popping the TWELVE-byte frame
     a 32-bit ISR pushed. It takes 6 bytes, lands on garbage, and the kernel tears the VDM
     down with no diagnostic. That is the whole "dies ~5 ticks in".
     One bit fixes it: with D/B set, the same `CF` is an IRETD. This is the third time this
     project has paid for "frame width and descriptor width are the same question" -- see
     the initial-selector and PM-return-catcher notes. The client's width is not known when
     the table is built, so sync it at use. */
static void dpmi_sync_defsel_width(void)
{
    BYTE want;
    if (g_pm_defidx < 0) return;
    want = (BYTE)(g_dpmi_client32 ? 0x4 : 0x0);      /* 0x4 = D/B, same idiom as the handler code sel */
    if (g_ldt[g_pm_defidx].flags == want) return;
    g_ldt[g_pm_defidx].flags = want;
    dpmi_install(g_pm_defidx);
}

static int dpmi_service_pm_int(dos_machine_t *mp, volatile BYTE *tib, DWORD vec, unsigned steps);
static void dpmi_ensure_pmret_sel(void);   /* fwd: shared PM-return catcher installer (#2b + 0303) */

/* Invoke a DPMI 0303 real-mode callback: the guest (running in V86 during a 0301
   excursion) far-called a planted callback BOP -- switch V86->PM, run the client's
   PM handler with the real-mode register state marshalled into its RMCS, then resume
   V86 at the far-call's return address. The inverse of 0301's PM->V86 direction.
   On entry the CONTEXT holds the V86 state at the far-call (segment un-patched). */
static void dpmi_invoke_callback(dos_machine_t *m, volatile BYTE *tib, int slot)
{
    char lb[256]; char *lp = lb;
    WORD rss = (WORD)VDM_REG(tib, VTIB_SS), rsp = (WORD)VDM_REG(tib, VTIB_ESP);
    DWORD rstk = ((DWORD)rss << 4) + rsp;
    WORD retIP = peekw(rstk), retCS = peekw(rstk + 2);     /* the far-call return frame */
    WORD newSP = (WORD)(rsp + 4);                          /* pop it */
    DWORD rmcs = dpmi_sel_base(g_cb[slot].rm_es) + g_cb[slot].rm_di;
    volatile BYTE *rc = (volatile BYTE *)(ULONG_PTR)rmcs;
    WORD vMsw = *(volatile WORD *)(tib + VTIB_MSW);
    unsigned ph; int cbdone = 0;
    /* fill the callback's RMCS with the real-mode register file + the return CS:IP:SS:SP */
    *(volatile WORD*)(rc+0x00)=VDM_REG(tib,VTIB_EDI); *(volatile WORD*)(rc+0x04)=VDM_REG(tib,VTIB_ESI);
    *(volatile WORD*)(rc+0x08)=VDM_REG(tib,VTIB_EBP); *(volatile WORD*)(rc+0x10)=VDM_REG(tib,VTIB_EBX);
    *(volatile WORD*)(rc+0x14)=VDM_REG(tib,VTIB_EDX); *(volatile WORD*)(rc+0x18)=VDM_REG(tib,VTIB_ECX);
    *(volatile WORD*)(rc+0x1C)=VDM_REG(tib,VTIB_EAX); *(volatile WORD*)(rc+0x22)=VDM_REG(tib,VTIB_ES);
    *(volatile WORD*)(rc+0x24)=VDM_REG(tib,VTIB_DS);  *(volatile WORD*)(rc+0x20)=(WORD)VDM_REG(tib,VTIB_EFLAGS);
    *(volatile WORD*)(rc+0x2A)=retIP; *(volatile WORD*)(rc+0x2C)=retCS;   /* CS:IP = far-call return */
    *(volatile WORD*)(rc+0x2E)=newSP; *(volatile WORD*)(rc+0x30)=rss;     /* SS:SP after popping it */
    lp = zput(lp, "  0303-cb slot "); lp = zhex(lp, slot); lp = zput(lp, " ret=0x"); lp = zhex(lp, retCS);
    lp = zput(lp, ":0x"); lp = zhex(lp, retIP); lp = zput(lp, " -> PM handler 0x"); lp = zhex(lp, g_cb[slot].pm_sel);
    lp = zput(lp, ":0x"); lp = zhex(lp, g_cb[slot].pm_off); lp = zput(lp, "\r\n");
    log_append(LOG_PATH, lb, lp); serial_out(lb, lp); lp = lb;
    /* re-arm the BOP patch (the PM handler is protected-mode code), enter PM */
    dpmi_repatch();
    *(volatile WORD *)(tib + VTIB_MSW) = (WORD)(vMsw | MSW_PE_BIT);
    /* PM handler stack (data selector 0x17, scratch SP) with an IRET frame -> PM-return catcher.
       The IRET operand size follows the HANDLER's CS D-bit: a 32-bit PM handler pops a dword
       FLAGS/CS/EIP frame, a 16-bit one pops a word frame (GH #18 run 83). */
    { WORD pss = 0x17, psp = 0xF400; DWORD b = dpmi_sel_base(pss);
      if (dpmi_sel_is32(g_cb[slot].pm_sel)) {
          psp -= 4; poked(b + psp, 0x00000202);        /* EFLAGS */
          psp -= 4; poked(b + psp, g_pmret_sel);       /* CS (dword; hi16=0) */
          psp -= 4; poked(b + psp, DPMI_PMRET_OFF);    /* EIP */
      } else {
          psp -= 2; pokew(b + psp, 0x0202);            /* FLAGS */
          psp -= 2; pokew(b + psp, g_pmret_sel);       /* CS */
          psp -= 2; pokew(b + psp, DPMI_PMRET_OFF);    /* IP */
      }
      VDM_SET16(tib, VTIB_SS, pss); VDM_REG(tib, VTIB_ESP) = psp; }
    VDM_REG(tib, VTIB_EFLAGS) = VTIB_EFLAGS_PM;
    VDM_SET16(tib, VTIB_CS, g_cb[slot].pm_sel); VDM_REG(tib, VTIB_EIP) = g_cb[slot].pm_off;
    VDM_SET16(tib, VTIB_ES, g_cb[slot].rm_es);  VDM_REG(tib, VTIB_EDI) = g_cb[slot].rm_di;  /* ES:DI = RMCS */
    VDM_SET16(tib, VTIB_DS, 0x17); VDM_REG(tib, VTIB_ESI) = 0;
    /* run the PM handler until it IRETs onto the PM-return catcher (g_pmret_sel:PMRET_OFF).
       A handler that itself issues INT 31h/21h now routes through the shared dispatcher
       dpmi_service_pm_int() -- the same full surface the main PM loop gets (GH #2), so a
       callback can allocate descriptors, print, sim-real-mode-int, etc. */
    for (ph = 0; ph < 64 && !cbdone; ++ph) {
        DWORD ev, eip, vec;
        dpmi_arm_fault_trampoline(tib, 0);   /* GH #18: re-arm the PM-fault reflect (no-op on interp path) */
        dpmi_enter_pm(tib);
        ev = VDM_REG(tib, VTIB_EVENT); eip = dpmi_pm_eip(tib);
        if (ev == VDM_EVENT_BOP && eip == DPMI_PMRET_OFF
            && (VDM_REG(tib, VTIB_CS) & 0xFFFF) == g_pmret_sel) { cbdone = 1; break; }
        if (ev == 3) continue;   /* dpmi_enter_pm reports "interrupt pending, not entered" -- retry */
        vec = (ev == VDM_EVENT_BOP) ? dpmi_bop_vec(VDM_REG(tib, VTIB_CS) & 0xFFFF, eip) : 0;   /* a patched INT the handler issued */
        if (vec == 0x31 || vec == 0x21) {
            if (dpmi_service_pm_int(m, tib, vec, ph) > 0) continue;   /* serviced -> resume handler */
            cbdone = 1; break;   /* client-exit or unexpected from inside a callback: end the loop */
        }
        lp = zput(lp, "  0303-cb: unexpected PM stop ev=0x"); lp = zhex(lp, ev);
        lp = zput(lp, " CS:IP=0x"); lp = zhex(lp, VDM_REG(tib, VTIB_CS) & 0xFFFF);
        lp = zput(lp, ":0x"); lp = zhex(lp, eip); lp = zput(lp, "\r\n");
        log_append(LOG_PATH, lb, lp); serial_out(lb, lp); lp = lb;
        break;
    }
    /* handler done: un-patch again and resume the RM proc in V86 at the RMCS CS:IP */
    dpmi_unpatch();
    *(volatile WORD *)(tib + VTIB_MSW) = vMsw;
    VDM_REG(tib, VTIB_EFLAGS) = 0x20202;
    VDM_REG(tib,VTIB_EDI)=*(volatile WORD*)(rc+0x00); VDM_REG(tib,VTIB_ESI)=*(volatile WORD*)(rc+0x04);
    VDM_REG(tib,VTIB_EBP)=*(volatile WORD*)(rc+0x08); VDM_REG(tib,VTIB_EBX)=*(volatile WORD*)(rc+0x10);
    VDM_REG(tib,VTIB_EDX)=*(volatile WORD*)(rc+0x14); VDM_REG(tib,VTIB_ECX)=*(volatile WORD*)(rc+0x18);
    VDM_REG(tib,VTIB_EAX)=*(volatile WORD*)(rc+0x1C);
    VDM_SET16(tib,VTIB_ES,*(volatile WORD*)(rc+0x22)); VDM_SET16(tib,VTIB_DS,*(volatile WORD*)(rc+0x24));
    VDM_SET16(tib,VTIB_CS,*(volatile WORD*)(rc+0x2C)); VDM_REG(tib,VTIB_EIP)=*(volatile WORD*)(rc+0x2A);
    VDM_SET16(tib,VTIB_SS,*(volatile WORD*)(rc+0x30)); VDM_REG(tib,VTIB_ESP)=*(volatile WORD*)(rc+0x2E);
    VDM_SET16(tib,VTIB_FS,*(volatile WORD*)(rc+0x30)); VDM_SET16(tib,VTIB_GS,*(volatile WORD*)(rc+0x30));
    lp = zput(lp, cbdone ? "  0303-cb: PM handler returned (OK)\r\n" : "  0303-cb: PM handler NO-RET\r\n");
    log_append(LOG_PATH, lb, lp); serial_out(lb, lp); lp = lb;
}

/* Service one protected-mode interrupt the DPMI client raised (a patched INT nn
   that reflected as a BOP). `vec` = the ORIGINAL vector (0x31 = DPMI, 0x21 = DOS).
   Updates the guest CONTEXT with the results (regs + CF) and advances EIP past the
   2-byte INT. Returns  1 = serviced, keep running;  0 = client terminated (INT 21h
   AH=4Ch);  -1 = unexpected / unserviceable stop (already logged).
   Extracted from the main PM loop so nested handlers -- a 0303 real-mode callback or
   a 0301 excursion proc that itself issues INT 31h/21h -- get the SAME full dispatch
   (GH #2). `steps` is only used for a log line. `mp` aliases the machine as `m` so the
   moved body is byte-for-byte the original (localized, #undef'd immediately). */
/* ── VECTOR A PROTECTED-MODE SOFTWARE INTERRUPT TO THE CLIENT'S OWN HANDLER. ──────
 *
 *  THE GAP THIS CLOSES, and it is architectural rather than a missing service.
 *  A DPMI client may install its own protected-mode handler for any interrupt (INT 31h
 *  0205), and a host that then services the interrupt ITSELF has taken the client's
 *  interrupt away from it. We did exactly that for every PM INT, and the old comment on
 *  g_pm_int[] admitted it: "We still service patched INT 21h/31h ourselves -- routing to
 *  a client-installed PM handler is a deeper item".
 *
 *  It is not deep, it is load-bearing. DOS/4GW installs a PM INT 21h handler at
 *  0x67:0x84 (inside the aliased code window at base 0xd9b0) and then calls its OWN
 *  private extender functions through it -- `mov ax,0xff80 / mov dx,0x1301 / mov es,sel
 *  / int 21h`, and on CF it prints "DOS/16M error: [34] DPMI host error (cannot lock
 *  stack)" and dies. AX=FF80h is not a DOS function and never was; it is DOS/4GW talking
 *  to itself, and every answer WE invent for it is wrong. Measured both ways: leaving the
 *  flags alone let it limp on to a later failure, returning CF=1 killed it here. The only
 *  right answer is to let its handler run.
 *
 *  Mechanics are the proven ones from dpmi_inject_pm_irq(): push an IRET frame on the
 *  client's own stack pointing at the PM-return catcher, vector to the handler, and run
 *  it through the SAME dispatcher the main loop uses so a handler that issues INT 31h,
 *  port I/O or a nested DOS call still works.
 *
 *  ► WHAT WE DELIBERATELY DO **NOT** DO IS RESTORE THE REGISTER FILE. An async IRQ is
 *    transparent, so dpmi_inject_pm_irq restores everything; a SOFTWARE interrupt is a
 *    call, and its whole purpose is to return AX/BX/CF to the caller. We keep what the
 *    handler produced -- including EFLAGS, which after its IRET holds whatever it wrote
 *    into the stack frame, which is precisely how a DOS handler returns CF.
 *
 *  ► RE-ENTRANCY: g_pm_disp[vec] guards the case where the handler issues the same INT
 *    again (a chain back to the host). We then service it ourselves, which is the
 *    correct meaning of "chain to the previous handler" when the previous one is us.
 */
static BYTE g_pm_disp[256];                 /* 1 while inside vec's client handler */

static int dpmi_dispatch_to_pm_handler(dos_machine_t *mp, volatile BYTE *tib,
                                       DWORD vec, unsigned steps)
{
    char lb[256], *lp = lb;
    DWORD sEIP = VDM_REG(tib, VTIB_EIP), sESP = VDM_REG(tib, VTIB_ESP);
    WORD  sCS  = (WORD)VDM_REG(tib, VTIB_CS), sSS = (WORD)VDM_REG(tib, VTIB_SS);
    DWORD sEFL = VDM_REG(tib, VTIB_EFLAGS);
    /* ── THE FRAME WIDTH FOLLOWS THE CLIENT'S MODE, NOT THE HANDLER SELECTOR'S D BIT.
         This is measured, and getting it wrong is invisible until the handler RETURNS.
         DOS/4GW's PM INT 21h handler lives in a 16-BIT code selector (0x67, D/B=0) --
         so dpmi_sel_is32() says "16-bit" and we pushed a 6-byte frame -- but it ends
         with `66 cf`, an operand-size-prefixed IRETD, which pops TWELVE bytes. It
         therefore returned to a wild address and the VDM died, with the last breakpoint
         sitting on the instruction before it.
         The client declared itself 32-bit at the mode switch (g_dpmi_client32), and an
         interrupt frame is a DPMI API width -- exactly the distinction the switch code
         already draws: the declared width is "the right input for DPMI API register
         widths, not for D/B". A 16-bit client's handler ends in a plain IRET and gets a
         6-byte frame, which is the same rule. */
    int h32 = g_dpmi_client32;
    unsigned ph; int done = 0;

    dpmi_ensure_pmret_sel();
    dpmi_sync_defsel_width();     /* the ISR may chain into the default stubs -- see the helper */
    if (g_pmret_sel == 0) return 0;                 /* caller falls back to servicing */

    /* ► FRAME WIDTH AND STACK-POINTER WIDTH ARE TWO DIFFERENT QUESTIONS, and conflating
         them faults in OUR OWN process. How many bytes the handler pops is the client's
         mode (h32, above). How the stack is ADDRESSED is the SS descriptor's B bit: with
         a 16-bit stack selector the CPU maintains SP only, and the top half of ESP holds
         whatever junk was last there -- 0xb350 in the run that caught this. Using that as
         an offset from the segment base walked straight off the end of guest memory and
         the VEH reported an access violation inside pokew(). A 32-bit frame on a 16-bit
         stack is perfectly ordinary and is exactly what DOS/4GW uses. */
    { DWORD b = dpmi_sel_base(sSS);
      int ss32 = dpmi_sel_is32(sSS);
      DWORD sp = ss32 ? sESP : (sESP & 0xFFFF);
      if (h32) {
          sp = ss32 ? sp - 4 : ((sp - 4) & 0xFFFF); poked(b + sp, sEFL);
          sp = ss32 ? sp - 4 : ((sp - 4) & 0xFFFF); poked(b + sp, g_pmret_sel);
          sp = ss32 ? sp - 4 : ((sp - 4) & 0xFFFF); poked(b + sp, DPMI_PMRET_OFF);
      } else {
          sp = ss32 ? sp - 2 : ((sp - 2) & 0xFFFF); pokew(b + sp, (WORD)sEFL);
          sp = ss32 ? sp - 2 : ((sp - 2) & 0xFFFF); pokew(b + sp, g_pmret_sel);
          sp = ss32 ? sp - 2 : ((sp - 2) & 0xFFFF); pokew(b + sp, DPMI_PMRET_OFF);
      }
      VDM_REG(tib, VTIB_ESP) = ss32 ? sp : ((sESP & 0xFFFF0000u) | sp); }

    VDM_SET16(tib, VTIB_CS, g_pm_int[vec].sel);
    VDM_REG(tib, VTIB_EIP) = h32 ? g_pm_int[vec].off : (g_pm_int[vec].off & 0xFFFF);

    lp = zput(lp, "PM INT 0x"); lp = zhex(lp, vec);
    lp = zput(lp, " -> CLIENT handler 0x"); lp = zhex(lp, g_pm_int[vec].sel);
    lp = zput(lp, ":0x"); lp = zhex(lp, g_pm_int[vec].off);
    lp = zput(lp, " AX=0x"); lp = zhex(lp, VDM_REG(tib, VTIB_EAX) & 0xFFFF);
    /* ► WHAT THE CALLER ACTUALLY PASSED. A DOS call that takes a pointer takes it in
         DS:(E)DX, and when one fails the FIRST question is whether the caller's pointer
         was good -- i.e. whether the fault is the client's or ours. Print the selector,
         the full 32-bit EDX (a flat client's offset does not fit in a word) and the
         bytes at the resulting linear address. */
    { DWORD dsv = VDM_REG(tib, VTIB_DS) & 0xFFFF;
      DWORD edx = VDM_REG(tib, VTIB_EDX);
      DWORD off = dpmi_sel_is32((WORD)dsv) ? edx : (edx & 0xFFFF);
      DWORD lin = dpmi_sel_base((WORD)dsv) + off;
      const BYTE *sb = (const BYTE *)(ULONG_PTR)lin;
      lp = zput(lp, " DS:EDX=0x"); lp = zhex(lp, dsv); lp = zput(lp, ":0x"); lp = zhex(lp, edx);
      lp = zput(lp, " lin=0x"); lp = zhex(lp, lin); lp = zput(lp, " @=");
      if (!host_readable(sb, 16)) lp = zput(lp, "<unreadable>");
      else                        lp = zdump(lp, sb, 16);
      /* ► AND THE CALLER'S STACK WIDTH, because that is what the client's dispatcher
           ASKS. DOS/4GW's common handler (mod:0x550) begins `LAR eax,SS` + `bt eax,22`
           -- it reads the D/B bit of the interrupted SS descriptor to decide whether the
           caller was 16- or 32-bit, and therefore whether a pointer argument is a word
           or a dword. If that bit is wrong, the extender truncates a flat pointer to its
           low 16 bits, which is exactly the failure being chased here. */
      { WORD ssv = (WORD)VDM_REG(tib, VTIB_SS);
        lp = zput(lp, " SS=0x"); lp = zhex(lp, ssv);
        lp = zput(lp, dpmi_sel_is32(ssv) ? " (SS D/B=1)" : " (SS D/B=0)");
        lp = zput(lp, " ESP=0x"); lp = zhex(lp, VDM_REG(tib, VTIB_ESP));
        lp = zput(lp, " CS=0x"); lp = zhex(lp, (WORD)VDM_REG(tib, VTIB_CS));
        lp = zput(lp, dpmi_sel_is32((WORD)VDM_REG(tib, VTIB_CS)) ? " (CS D/B=1)" : " (CS D/B=0)");
        lp = zput(lp, " h32="); lp = zhex(lp, (DWORD)h32); } }
    lp = zput(lp, "\r\n"); log_append(LOG_PATH, lb, lp); serial_out(lb, lp); lp = lb;

    g_pm_disp[vec] = 1;
    for (ph = 0; ph < 4096 && !done; ++ph) {
        DWORD ev, eip, nvec; int rc;
        /* ► CHECKPOINT INSIDE THE HANDLER TOO. The main loop's DPMI-CP lines stop at the
             moment we hand control to the client's handler, so without this a death in
             there is exactly the blind stretch this session spent hours removing -- one
             log line, then nothing. Bounded to the first 64 entries per dispatch so a
             handler that loops pays nothing. */
        if (ph < 64 && g_dpmi_cp_max > 8) {
            DWORD ccs = VDM_REG(tib, VTIB_CS) & 0xFFFF;
            DWORD cb  = dpmi_sel_base((WORD)ccs), cip = VDM_REG(tib, VTIB_EIP);
            const BYTE *ib = (const BYTE *)(ULONG_PTR)(cb + cip);
            lp = zput(lp, "  PMH["); lp = zhex(lp, ph);
            lp = zput(lp, "] cs:eip=0x"); lp = zhex(lp, ccs);
            lp = zput(lp, ":0x"); lp = zhex(lp, cip);
            lp = zput(lp, " ss:esp=0x"); lp = zhex(lp, VDM_REG(tib, VTIB_SS) & 0xFFFF);
            lp = zput(lp, ":0x"); lp = zhex(lp, VDM_REG(tib, VTIB_ESP));
            lp = zput(lp, " EAX=0x"); lp = zhex(lp, VDM_REG(tib, VTIB_EAX));
            lp = zput(lp, " DS=0x"); lp = zhex(lp, VDM_REG(tib, VTIB_DS) & 0xFFFF);
            lp = zput(lp, " ES=0x"); lp = zhex(lp, VDM_REG(tib, VTIB_ES) & 0xFFFF);
            lp = zput(lp, " b=");
            if (!host_readable(ib, 16)) lp = zput(lp, "<unreadable>");
            else                        lp = zdump(lp, ib, 16);
            lp = zput(lp, "\r\n");
            log_append(LOG_PATH, lb, lp); serial_out(lb, lp); lp = lb;
        }
        dpmi_arm_fault_trampoline(tib, 0);
        dpmi_enter_pm(tib);
        ev  = VDM_REG(tib, VTIB_EVENT);
        eip = dpmi_pm_eip(tib);
        if (ev == VDM_EVENT_BOP && eip == DPMI_PMRET_OFF
            && (VDM_REG(tib, VTIB_CS) & 0xFFFF) == g_pmret_sel) { done = 1; break; }
        if (ev == 3) continue;                      /* "interrupt pending" -> retry */
        if (ev == VDM_EVENT_IO || ev == VDM_EVENT_IO_HW || ev == VDM_EVENT_GPFAULT) {
            int io_h; HOST_LOCK(); io_h = host_try_io_pm(tib, &g_bus); HOST_UNLOCK();
            if (io_h) continue;
        }
        nvec = (ev == VDM_EVENT_BOP) ? dpmi_bop_vec(VDM_REG(tib, VTIB_CS) & 0xFFFF, eip) : 0;
        rc = dpmi_service_pm_int(mp, tib, nvec, steps);
        if (rc > 0) continue;
        g_pm_disp[vec] = 0;
        return rc;                                  /* 0 = client exited, -1 = stop */
    }
    g_pm_disp[vec] = 0;

    /* Resume the client past its INT. CS/SS and the stack pointer go back to what they
       were; the GPRs and EFLAGS are the handler's answer and are left alone. */
    VDM_SET16(tib, VTIB_CS, sCS);
    VDM_REG(tib, VTIB_EIP) = sEIP + 2;               /* past the 2-byte patched INT */
    VDM_SET16(tib, VTIB_SS, sSS);
    VDM_REG(tib, VTIB_ESP) = sESP;
    lp = zput(lp, "PM INT 0x"); lp = zhex(lp, vec);
    lp = zput(lp, done ? " <- handler IRET, AX=0x" : " <- handler NO-RET, AX=0x");
    lp = zhex(lp, VDM_REG(tib, VTIB_EAX) & 0xFFFF);
    lp = zput(lp, " CF="); lp = zhex(lp, VDM_REG(tib, VTIB_EFLAGS) & 1u);
    lp = zput(lp, "\r\n"); log_append(LOG_PATH, lb, lp); serial_out(lb, lp);
    return 1;
}

static int dpmi_service_pm_int(dos_machine_t *mp, volatile BYTE *tib, DWORD vec,
                               unsigned steps)
{
#define m (*mp)
    char report[2048]; char *base = report; char *p = report;
    DWORD ax = VDM_REG(tib, VTIB_EAX) & 0xFFFF;
    DWORD ev = VDM_REG(tib, VTIB_EVENT), eip = dpmi_pm_eip(tib);
    (void)steps;
    /* First safe moment to re-plant anything the skip mode stepped over. */
    if (g_bp_n) dpmi_bp_rearm_pending(dpmi_sel_base((WORD)(VDM_REG(tib, VTIB_CS) & 0xFFFF)) + eip);
                    /* ── THE CLIENT'S OWN PM HANDLER WINS, IF IT INSTALLED ONE. ──────
                       Scoped to INT 21h ON PURPOSE, for now. DOS/4GW also installs PM
                       handlers for 10h/16h/1Ah/... and by the spec those should route to
                       it too -- but our 0300 (simulate real-mode interrupt) currently only
                       implements INT 21h, so routing video and keyboard to a handler that
                       then asks us to simulate a real-mode INT 10h would trade a working
                       path for a broken one. Route the interrupt the evidence names,
                       measure, then widen. The order matters: this test comes before every
                       service arm below, so it cannot be shadowed by one of them. */
                    /* ── SNOOP `AH=25h` ON THE WAY PAST: IT IS HOW A GAME HOOKS ITS TIMER. ──
                       INT 31h 0205 is not the only way a client installs a protected-mode
                       handler, and for the case that matters it is not the way used at all.
                       Doom hooks its timer with DOS's own call:
                           AX=3508 int 21h          ; save the old vector
                           AX=2508 int 21h          ; DS:EDX = 0x187:0x03ae31f0
                       and DS is its OWN 32-bit code selector, with `60 1e 06 0f a0 0f a8`
                       (pushad; push ds/es/fs/gs) at the target -- an ISR prologue.
                       DOS/4GW's own PM INT 21h handler services that internally, in its own
                       IDT, and never tells us: no 0205 for vector 8 appears in a whole run.
                       So without this we do not know where the game's timer ISR is, and the
                       only INT 08h we can see is the extender's arming-pass stub -- which is
                       both the wrong target and, measured, fatal to enter asynchronously.
                       Record it and let the client's handler run as well; both want it, and
                       observing a call costs the client nothing. */
                    /* ── THE HOST OWNS THE HARDWARE-INTERRUPT VECTORS OF A 32-BIT CLIENT. ──
                       Doom hooks its timer with DOS's own call rather than INT 31h 0205:
                           AX=3508 int 21h ; AX=2508 int 21h, DS:EDX = 0x187:0x03ae31f0
                       with DS its own 32-bit code selector and `60 1e 06 0f a0 0f a8`
                       (pushad; push ds/es/fs/gs) at the target -- an ISR prologue.
                       Letting DOS/4GW service that does not work: it FAILS the hook (CF=1,
                       measured) and then reports "fatal error (1001): error in interrupt
                       chain", because the extender is trying to splice a handler into a
                       chain whose hardware end WE are, not it. It cannot see our injection
                       and we cannot see its internal IDT, and a chain with two owners is
                       exactly what error 1001 describes.
                       So for vectors 08h-0Fh -- the PIC lines, the ones this host actually
                       delivers -- answer 25h/35h ourselves against g_pm_int[], the table
                       injection reads. 21h and the rest still go to the client's handler,
                       untouched. Scoped to a 32-bit client so nothing 16-bit changes. */
                    if (vec == 0x21 && g_dpmi_client32) {
                        DWORD ah25 = (ax >> 8) & 0xFF, al25 = ax & 0xFF;
                        if ((ah25 == 0x25 || ah25 == 0x35) && al25 >= 0x08 && al25 <= 0x0F) {
                            if (ah25 == 0x25) {
                                WORD hs = (WORD)(VDM_REG(tib, VTIB_DS) & 0xFFFF);
                                g_pm_int[al25].sel = hs;
                                g_pm_int[al25].off = dpmi_sel_is32(hs) ? VDM_REG(tib, VTIB_EDX)
                                                                       : (VDM_REG(tib, VTIB_EDX) & 0xFFFF);
                                g_pm_int[al25].client = 1;
                                if (al25 == 0x08) { g_pm_app_hooked_timer = 1;
                                                    g_pm_app_timer_sel = hs;
                                                    g_pm_app_timer_off = g_pm_int[al25].off;
                                                    g_pm_vec8_armed_ms = GetTickCount(); }
                                p = zput(p, "PM INT 21h AH=25 (host-owned IRQ vector) 0x");
                                p = zhex(p, al25); p = zput(p, " = 0x"); p = zhex(p, hs);
                                p = zput(p, ":0x"); p = zhex(p, g_pm_int[al25].off);
                            } else {
                                /* ── WHAT THE CALLER CHAINS TO MUST BE SAFE TO CHAIN TO. ────
                                     A game saves the "old" handler here and far-jumps to it
                                     periodically -- Doom's timer ISR passes the tick down
                                     every Nth interrupt to keep the BIOS clock. If we hand
                                     back the extender's arming-pass stub, that chain lands in
                                     DOS/4GW's dispatcher, entered from a hardware interrupt
                                     WE synthesised rather than through its own IDT, on a
                                     stack it did not switch: measured, that kills the VDM
                                     outright, with no fault and no log. It is why delivery
                                     died after a handful of ticks however it was arranged --
                                     the deaths were never the injections, they were the Nth
                                     one, when the ISR chained.
                                     Once we own the line, the far end of the chain is OURS:
                                     hand back the host's own default stub for the vector
                                     (BOP; IRET), which accepts the chain and returns. */
                                WORD  osel = g_pm_int[al25].sel;
                                DWORD ooff = g_pm_int[al25].off;
                                if (!dpmi_sel_is32(osel) && g_pm_defsel) {
                                    osel = g_pm_defsel;
                                    ooff = (DWORD)al25 * DPMI_PMDEF_STRIDE;
                                }
                                VDM_SET16(tib, VTIB_ES, osel);
                                VDM_REG(tib, VTIB_EBX) = ooff;                 /* client is 32-bit */
                                p = zput(p, "PM INT 21h AH=35 (host-owned IRQ vector) 0x");
                                p = zhex(p, al25); p = zput(p, " -> 0x"); p = zhex(p, osel);
                                p = zput(p, ":0x"); p = zhex(p, ooff);
                            }
                            VDM_REG(tib, VTIB_EFLAGS) &= ~1u;          /* success */
                            p = zput(p, "\r\n");
                            log_append(LOG_PATH, base, p); serial_out(base, p); p = base;
                            VDM_REG(tib, VTIB_EIP) += 2;
                            return 1;
                        }
                    }
                    if (vec == 0x21 && g_pm_int[vec].client && !g_pm_disp[vec]) {
                        int rc = dpmi_dispatch_to_pm_handler(mp, tib, vec, steps);
                        if (rc != 0 || g_pm_int[vec].sel) return rc;
                        /* rc==0 with no handler means dispatch declined -> fall through */
                    }
                    if (vec == DPMI_BP_VEC) {                      /* guest breakpoint hit */
                        /* Report EVERYTHING -- the whole point is that the run may not
                           survive to the next line. Then restore the displaced bytes and
                           leave EIP WHERE IT IS, so the real instruction runs next and the
                           client carries on as if we had never been here. */
                        DWORD cs = VDM_REG(tib, VTIB_CS) & 0xFFFF;
                        DWORD cb = dpmi_sel_base((WORD)cs);
                        DWORD lin = cb + eip;
                        WORD  ss  = (WORD)(VDM_REG(tib, VTIB_SS) & 0xFFFF);
                        DWORD sb  = dpmi_sel_base(ss);
                        DWORD sp  = dpmi_sel_is32(ss) ? VDM_REG(tib, VTIB_ESP)
                                                      : (VDM_REG(tib, VTIB_ESP) & 0xFFFF);
                        p = zput(p, "DPMI-BP HIT linear 0x"); p = zhex(p, lin);
                        p = zput(p, " cs:eip=0x"); p = zhex(p, cs);
                        p = zput(p, ":0x"); p = zhex(p, eip);
                        p = zput(p, " after "); p = zhex(p, steps); p = zput(p, " svc");
                        /* ► A CLOCK, NOT JUST A SERVICE COUNT. Session 20 could bracket the
                             R_ExecuteSetViewSize death in INSTRUCTIONS but not in TIME, and
                             those two answers point at different culprits: a threshold in
                             instructions is the guest doing something illegal, a threshold in
                             milliseconds is the kernel's timer. `svc` counts host services,
                             which stop entirely inside a BOP-free stretch -- so it is exactly
                             the wrong unit for the question. GetTickCount is free. */
                        p = zput(p, " ms="); p = zhex(p, GetTickCount());
                        p = zput(p, " EAX=0x"); p = zhex(p, VDM_REG(tib, VTIB_EAX));
                        p = zput(p, " EBX=0x"); p = zhex(p, VDM_REG(tib, VTIB_EBX));
                        p = zput(p, " ECX=0x"); p = zhex(p, VDM_REG(tib, VTIB_ECX));
                        p = zput(p, " EDX=0x"); p = zhex(p, VDM_REG(tib, VTIB_EDX));
                        p = zput(p, " ESI=0x"); p = zhex(p, VDM_REG(tib, VTIB_ESI));
                        p = zput(p, " EDI=0x"); p = zhex(p, VDM_REG(tib, VTIB_EDI));
                        p = zput(p, " EBP=0x"); p = zhex(p, VDM_REG(tib, VTIB_EBP));
                        p = zput(p, " DS=0x"); p = zhex(p, VDM_REG(tib, VTIB_DS) & 0xFFFF);
                        p = zput(p, " ES=0x"); p = zhex(p, VDM_REG(tib, VTIB_ES) & 0xFFFF);
                        p = zput(p, " SS:SP=0x"); p = zhex(p, ss); p = zput(p, ":0x"); p = zhex(p, sp);
                        p = zput(p, " efl=0x"); p = zhex(p, VDM_REG(tib, VTIB_EFLAGS));
                        p = zput(p, " stack=");
                        /* 64 bytes, not 16: when a breakpoint sits inside a leaf routine the
                           question is almost always "who called this", and one frame is never
                           enough -- the whole return CHAIN is what names the decision. */
                        { const BYTE *sk = (const BYTE *)(ULONG_PTR)(sb + sp);
                          if (!host_readable(sk, 64)) p = zput(p, "<unreadable>");
                          else                        p = zdump(p, sk, 64); }
                        { int bk = dpmi_bp_disarm(lin);
                          if (bk >= 0 && g_bp_rep[bk]) g_bp_pending[bk] = 1;   /* repeating */
                          if (bk >= 0 && g_bp_skip[bk]) {
                              /* SKIP MODE: step over the instruction entirely. The bytes are
                                 already restored, so advancing EIP lands on whatever follows
                                 the skipped instruction -- the caller states its length. */
                              VDM_REG(tib, VTIB_EIP) += g_bp_skip[bk];
                              p = zput(p, " [SKIPPED "); p = zhex(p, g_bp_skip[bk]);
                              p = zput(p, " byte(s)]");
                              /* A skipped site is usually in a loop or a shared wrapper, so
                                 one-shot is useless there. We cannot re-plant NOW -- EIP is
                                 inside the two-byte footprint we just restored -- so mark it
                                 and let dpmi_bp_rearm_pending() do it once the guest has
                                 moved on. */
                              g_bp_pending[bk] = 1;
                          }
                          if (bk < 0) p = zput(p, " [WARN: no BP record here]");
                          else if (g_bp_dump[bk]) {
                              const BYTE *dm = (const BYTE *)(ULONG_PTR)g_bp_dump[bk];
                              p = zput(p, "\r\n  dump@0x"); p = zhex(p, g_bp_dump[bk]);
                              p = zput(p, "=");
                              if (!host_readable(dm, 64)) p = zput(p, "<unreadable from host>");
                              else                        p = zdump(p, dm, 64);
                          } }
                        p = zput(p, "\r\n");
                        log_append(LOG_PATH, base, p); serial_out(base, p); p = base;
                        return 1;                                  /* EIP unchanged on purpose */
                    }
                    if (vec == 0x10) {                             /* video BIOS in PM -> VDD */
                        ntvdd_regs r; regs_load(&r, tib);
                        HOST_LOCK();
                        vdd_bus_deliver_int(&g_bus, 0x10, &r);
                        HOST_UNLOCK();
                        regs_store(&r, tib);
                        video_trap_sync();          /* mode 12h: interpret (GH #55); no-op in 13h */
                        VDM_REG(tib, VTIB_EFLAGS) &= ~1u;
                        VDM_REG(tib, VTIB_EIP) += 2;              /* past the 2-byte PM BOP */
                        p = zput(p, "INT10h (PM) -> video VDD AX=0x"); p = zhex(p, ax);
                        p = zput(p, "\r\n"); log_append(LOG_PATH, base, p); serial_out(base, p); p = base;
                        return 1;
                    }
                    if (vec == 0x16) {                             /* keyboard BIOS in PM -> VDD */
                        ntvdd_regs r; uint8_t ah16; regs_load(&r, tib); ah16 = r_ah(&r);
                        for (;;) {                                 /* AH=00/10 block until a key */
                            HOST_LOCK();
                            vdd_bus_deliver_int(&g_bus, 0x16, &r);
                            HOST_UNLOCK();
                            if ((ah16 != 0x00 && ah16 != 0x10) || r.zf == 0 || !g_running) break;
                            InterlockedIncrement(&g_dpmi_iter);    /* keep the watchdog happy while blocked */
                            WaitForSingleObject(g_key_event, 50);
                        }
                        regs_store(&r, tib);
                        /* set CF/ZF directly in the PM eflags (no real-mode IRET frame in PM) */
                        if (r.cf) VDM_REG(tib, VTIB_EFLAGS) |= 1u;    else VDM_REG(tib, VTIB_EFLAGS) &= ~1u;
                        if (r.zf) VDM_REG(tib, VTIB_EFLAGS) |= 0x40u; else VDM_REG(tib, VTIB_EFLAGS) &= ~0x40u;
                        VDM_REG(tib, VTIB_EIP) += 2;               /* past the 2-byte PM BOP */
                        (void)base;                                 /* no per-poll logging (would flood) */
                        return 1;
                    }
                    if (vec == 0x33) {                             /* mouse in PM -> INT 33h */
                        mouse_int33(tib);
                        VDM_REG(tib, VTIB_EIP) += 2;
                        return 1;
                    }
                    if (vec == 0x1A || vec == 0x08) {              /* BIOS time / timer tick in PM */
                        ntvdd_regs r; regs_load(&r, tib);
                        HOST_LOCK();
                        vdd_bus_deliver_int(&g_bus, (uint8_t)vec, &r);   /* INT 1Ah get/set tick, or INT 08h increment */
                        HOST_UNLOCK();
                        regs_store(&r, tib);
                        VDM_REG(tib, VTIB_EIP) += 2;
                        return 1;
                    }
                    if (vec == 0x31) {                             /* DPMI INT 31h */
                        int need_scan = 0;   /* deferred: scanning logs, so do it after the flush */
                        p = zput(p, "INT31h AX=0x"); p = zhex(p, ax);
                        p = zput(p, " BX=0x"); p = zhex(p, VDM_REG(tib, VTIB_EBX) & 0xFFFF);
                        p = zput(p, " CX=0x"); p = zhex(p, VDM_REG(tib, VTIB_ECX) & 0xFFFF);
                        VDM_REG(tib, VTIB_EFLAGS) &= ~1u;          /* default CF=0 (success) */
                        switch (ax) {
                        case 0x0400:                               /* get DPMI version */
                            VDM_SET16(tib, VTIB_EAX, 0x005A);      /* 0.90 */
                            VDM_SET16(tib, VTIB_EBX, 0x0001);
                            VDM_SET16(tib, VTIB_ECX, 0x0003);      /* CL=3 (80386) */
                            VDM_SET16(tib, VTIB_EDX, 0x0870);
                            p = zput(p, " -> ver 0.90");
                            break;
                        case 0x0000: {                             /* allocate CX descriptors */
                            DWORD cx = VDM_REG(tib, VTIB_ECX) & 0xFFFF, i; WORD basesel;
                            if (cx == 0) cx = 1;
                            /* Recycle first, and only for CX==1: 0000 promises CONTIGUOUS
                               selectors (that is what 0003's increment is for), and a free
                               list cannot promise that. Single allocations are the whole of
                               what real clients ask for in bulk. */
                            if (cx == 1 && g_ldt_nfree > 0) {
                                int idx = g_ldt_free[--g_ldt_nfree];
                                g_ldt[idx].base = 0; g_ldt[idx].limit = 0;
                                g_ldt[idx].access = 0xF2; g_ldt[idx].flags = 0;
                                dpmi_install(idx);
                                VDM_SET16(tib, VTIB_EAX, (WORD)((idx << 3) | 7));
                                p = zput(p, " -> sel 0x"); p = zhex(p, (idx << 3) | 7);
                                p = zput(p, " (recycled)");
                                break;
                            }
                            if (g_ldt_next + (int)cx > DPMI_LDT_MAX) {      /* out of descriptors */
                                VDM_REG(tib, VTIB_EFLAGS) |= 1u; VDM_SET16(tib, VTIB_EAX, 0x8011);
                                p = zput(p, " -> ENOMEM"); break;
                            }
                            basesel = (WORD)((g_ldt_next << 3) | 7);
                            for (i = 0; i < cx; ++i) {
                                int idx = g_ldt_next++;
                                g_ldt[idx].base = 0; g_ldt[idx].limit = 0;
                                g_ldt[idx].access = 0xF2; g_ldt[idx].flags = 0;  /* data, RPL3 */
                                dpmi_install(idx);
                            }
                            VDM_SET16(tib, VTIB_EAX, basesel);
                            p = zput(p, " -> sel 0x"); p = zhex(p, basesel);
                            break; }
                        case 0x0001: {                             /* free descriptor BX */
                            WORD fsel = (WORD)(VDM_REG(tib, VTIB_EBX) & 0xFFFF);
                            int idx = fsel >> 3;
                            /* Never recycle OUR OWN selectors: the mode-switch trio, the PSP
                               and environment descriptors, and the stubs/trampoline. A client
                               is entitled to free anything it was given, but it was not given
                               these, and handing one back out later would pull the floor up. */
                            int reserved = (idx < 6)
                                || fsel == g_dpmi_hdlr_sel || fsel == g_pmret_sel
                                || fsel == g_dpmi_fault_sel || fsel == g_dpmi_flt_code_sel;
                            if (idx >= 1 && idx < DPMI_LDT_MAX && !reserved
                                && g_ldt[idx].access != 0 && g_ldt_nfree < DPMI_LDT_MAX) {
                                int d, dup = 0;
                                for (d = 0; d < g_ldt_nfree; ++d)   /* refuse a double free */
                                    if (g_ldt_free[d] == (WORD)idx) { dup = 1; break; }
                                if (!dup) {
                                    g_ldt[idx].base = g_ldt[idx].limit = 0;
                                    g_ldt[idx].access = 0;          /* not present */
                                    g_ldt[idx].flags = 0;
                                    dpmi_install(idx);
                                    g_ldt_free[g_ldt_nfree++] = (WORD)idx;
                                    p = zput(p, " -> freed sel 0x"); p = zhex(p, fsel);
                                    p = zput(p, " ("); p = zhex(p, (DWORD)g_ldt_nfree);
                                    p = zput(p, " on the free list)");
                                    break;
                                }
                            }
                            p = zput(p, " -> free (kept: reserved or not allocated)");
                            break; }
                        case 0x0100: {                             /* allocate DOS memory: BX paras -> AX=seg, DX=sel */
                            uint16_t want = (uint16_t)(VDM_REG(tib, VTIB_EBX) & 0xFFFF), seg = 0, max = 0;
                            int err = dos_alloc(NULL, m.first_mcb, want, &seg, &max);
                            if (err || g_ldt_next >= DPMI_LDT_MAX) {
                                VDM_REG(tib, VTIB_EFLAGS) |= 1u;
                                VDM_SET16(tib, VTIB_EAX, err ? err : 0x0008);   /* 8 = insufficient memory */
                                VDM_SET16(tib, VTIB_EBX, max);                  /* largest available (paras) */
                                p = zput(p, " -> DOSmem ENOMEM max=0x"); p = zhex(p, max);
                            } else {
                                int idx = g_ldt_next++;
                                g_ldt[idx].base = (DWORD)seg << 4;
                                g_ldt[idx].limit = want ? ((DWORD)want << 4) - 1 : 0;
                                g_ldt[idx].access = 0xF2; g_ldt[idx].flags = 0;  /* data, RPL3 */
                                dpmi_install(idx);
                                VDM_SET16(tib, VTIB_EAX, seg);
                                VDM_SET16(tib, VTIB_EDX, (WORD)((idx << 3) | 7));
                                p = zput(p, " -> DOSmem seg=0x"); p = zhex(p, seg);
                                p = zput(p, " sel=0x"); p = zhex(p, (idx << 3) | 7);
                            }
                            break; }
                        case 0x0101: {                             /* free DOS memory: DX = selector */
                            int idx = (VDM_REG(tib, VTIB_EDX) & 0xFFFF) >> 3;
                            if (idx >= 3 && idx < DPMI_LDT_MAX) {
                                DWORD seg = g_ldt[idx].base >> 4;
                                dos_free(NULL, (uint16_t)seg);
                                g_ldt[idx].base = g_ldt[idx].limit = 0; /* descriptor left reclaimable */
                            }
                            p = zput(p, " -> DOSfree");
                            break; }
                        case 0x0102: {                             /* resize DOS memory block: BX=new paras, DX=sel */
                            int idx = (VDM_REG(tib, VTIB_EDX) & 0xFFFF) >> 3;
                            uint16_t want = (uint16_t)(VDM_REG(tib, VTIB_EBX) & 0xFFFF), max = 0;
                            if (idx >= 1 && idx < DPMI_LDT_MAX && g_ldt[idx].base) {
                                uint16_t seg = (uint16_t)(g_ldt[idx].base >> 4);
                                int err = dos_resize(NULL, seg, want, &max);
                                if (err) {
                                    VDM_REG(tib, VTIB_EFLAGS) |= 1u;
                                    VDM_SET16(tib, VTIB_EAX, err);       /* DOS error (7/8/9) */
                                    VDM_SET16(tib, VTIB_EBX, max);       /* largest available (paras) */
                                    p = zput(p, " -> resize FAIL max=0x"); p = zhex(p, max);
                                } else {
                                    g_ldt[idx].limit = want ? ((DWORD)want << 4) - 1 : 0;
                                    dpmi_install(idx);
                                    p = zput(p, " -> resize seg=0x"); p = zhex(p, seg);
                                    p = zput(p, " to 0x"); p = zhex(p, want); p = zput(p, " paras");
                                }
                            } else {
                                VDM_REG(tib, VTIB_EFLAGS) |= 1u; VDM_SET16(tib, VTIB_EAX, 0x8022);  /* invalid sel */
                                p = zput(p, " -> resize bad sel");
                            }
                            break; }
                        case 0x0204: {                             /* get PM interrupt vector: BL -> CX:(E)DX */
                            DWORD bl = VDM_REG(tib, VTIB_EBX) & 0xFF;
                            VDM_SET16(tib, VTIB_ECX, g_pm_int[bl].sel);
                            /* ── THE RETURNED OFFSET IS AS WIDE AS THE CLIENT, NOT AS WIDE AS
                                 THE HANDLER'S SELECTOR. This tested only the handler selector,
                                 so a 16-bit handler (the extender's own stubs are all 16-bit)
                                 was reported with VDM_SET16 -- which preserves the top half of
                                 EDX. A 32-bit client reads all of EDX, so it got 0x????0020:
                                 the right offset with sixteen bits of whatever was there
                                 before glued on top.
                                 That is what broke Doom's timer. DOS/4GW services the game's
                                 `AH=25h` hook by first reading the vector back with 0204 to
                                 walk its chain; handed a garbage offset it does not recognise
                                 its own stub, fails the hook with CF=1, and later reports
                                     DOS/4GW Professional fatal error (1001):
                                     error in interrupt chain
                                 Zero-extend for a 32-bit client and the offset is just 0x20. */
                            if (dpmi_sel_is32(g_pm_int[bl].sel) || g_dpmi_client32)
                                VDM_REG(tib, VTIB_EDX) = g_pm_int[bl].off;
                            else
                                VDM_SET16(tib, VTIB_EDX, g_pm_int[bl].off & 0xFFFF);
                            p = zput(p, " -> getPMvec int 0x"); p = zhex(p, bl);
                            break; }
                        case 0x0205: {                             /* set PM interrupt vector: BL = CX:(E)DX */
                            DWORD bl = VDM_REG(tib, VTIB_EBX) & 0xFF;
                            WORD hsel = (WORD)VDM_REG(tib, VTIB_ECX);
                            g_pm_int[bl].sel = hsel;
                            /* 32-bit handler selector -> take the full 32-bit offset (GH #18 run 83);
                               a 16-bit handler keeps the word-masked offset as before */
                            g_pm_int[bl].off = dpmi_sel_is32(hsel) ? VDM_REG(tib, VTIB_EDX)
                                                                   : (VDM_REG(tib, VTIB_EDX) & 0xFFFF);
                            g_pm_int[bl].client = 1;         /* the client owns it now */
                            /* ── A HOOK IS NOT AN INVITATION TO INTERRUPT IMMEDIATELY. ──────
                                 We latch IRQ0 and deliver it as soon as the vector exists and
                                 virtual-IF is on, which in practice means THE INSTRUCTION AFTER
                                 the client installs it. Real hardware cannot do that: IRQ0 runs
                                 at 18.2 Hz, so the next tick is up to 55 ms -- millions of
                                 instructions -- away, and no DOS program is written to survive a
                                 timer interrupt arriving inside its own vector-arming loop.
                                 DOS/4GW is arming a TABLE when this bites: vectors 0..8 in one
                                 sequential pass, each a 4-byte default stub at 0x97:0x00, 0x04,
                                 ... 0x20. Injecting on the install of vector 8 vectors into a
                                 stub that is a placeholder, not the timer handler (Doom installs
                                 the real one much later, in I_StartupTimer), and the run ends
                                 there -- which is why `pmnoirq.flag` had to exist at all.
                                 Record when the vector was armed and let a tick period pass. */
                            if (bl == 0x08) g_pm_vec8_armed_ms = GetTickCount();
                            p = zput(p, " -> setPMvec int 0x"); p = zhex(p, bl);
                            p = zput(p, " = 0x"); p = zhex(p, g_pm_int[bl].sel);
                            p = zput(p, ":0x"); p = zhex(p, g_pm_int[bl].off);
                            break; }
                        /* ── 0202/0203 GET/SET PROTECTED-MODE EXCEPTION HANDLER ──────────
                           DPMI 0.9: BL = exception 00h..1Fh; CX:(E)DX = handler sel:off.
                           BL > 1Fh is the one documented error (8021h, "invalid value").
                           ► WHAT 0202 RETURNS BEFORE THE CLIENT HAS SET ANYTHING is a real
                             decision, not a detail. A client that CHAINS (the usual pattern:
                             get, save, set, and far-call the saved one for exceptions it
                             does not want) will store whatever we hand back and jump to it.
                             0000:0000 is therefore an unexecutable address dressed up as a
                             valid answer. We hand back the host's own handler segment and
                             the register-preserving RETF at DPMI_SSR_OFF, so a chain lands
                             on a real instruction inside a selector that exists.
                             This is NOT a working default exception handler -- the DPMI spec
                             says a host's default terminates the client, and ours cannot yet
                             do that from PM. It is the inert answer, and it is only ever
                             reached if an exception actually fires, at which point the run
                             is already lost. Recorded so the next reader does not mistake it
                             for a considered exception-delivery design: there isn't one yet. */
                        case 0x0202: {                             /* get PM exception handler: BL -> CX:(E)DX */
                            DWORD bl = VDM_REG(tib, VTIB_EBX) & 0xFF;
                            if (bl > 0x1F) {
                                VDM_REG(tib, VTIB_EFLAGS) |= 1u; VDM_SET16(tib, VTIB_EAX, 0x8021);
                                p = zput(p, " -> getEXC bad exception 0x"); p = zhex(p, bl); break;
                            }
                            if (!g_pm_exc[bl].set) {
                                g_pm_exc[bl].sel = dpmi_hdlr_code_sel();
                                g_pm_exc[bl].off = DPMI_SSR_OFF;
                            }
                            VDM_SET16(tib, VTIB_ECX, g_pm_exc[bl].sel);
                            if (dpmi_sel_is32(g_pm_exc[bl].sel)) VDM_REG(tib, VTIB_EDX) = g_pm_exc[bl].off;
                            else VDM_SET16(tib, VTIB_EDX, g_pm_exc[bl].off & 0xFFFF);
                            p = zput(p, " -> getEXC 0x"); p = zhex(p, bl);
                            p = zput(p, " = 0x"); p = zhex(p, g_pm_exc[bl].sel);
                            p = zput(p, ":0x"); p = zhex(p, g_pm_exc[bl].off);
                            break; }
                        case 0x0203: {                             /* set PM exception handler: BL = CX:(E)DX */
                            DWORD bl = VDM_REG(tib, VTIB_EBX) & 0xFF;
                            WORD hsel = (WORD)VDM_REG(tib, VTIB_ECX);
                            if (bl > 0x1F) {
                                VDM_REG(tib, VTIB_EFLAGS) |= 1u; VDM_SET16(tib, VTIB_EAX, 0x8021);
                                p = zput(p, " -> setEXC bad exception 0x"); p = zhex(p, bl); break;
                            }
                            g_pm_exc[bl].sel = hsel;
                            /* same 16/32 offset rule as 0205: a 32-bit handler selector means
                               the client passed a full EDX (GH #18 run 83). */
                            g_pm_exc[bl].off = dpmi_sel_is32(hsel) ? VDM_REG(tib, VTIB_EDX)
                                                                   : (VDM_REG(tib, VTIB_EDX) & 0xFFFF);
                            g_pm_exc[bl].set = 1;
                            p = zput(p, " -> setEXC 0x"); p = zhex(p, bl);
                            p = zput(p, " = 0x"); p = zhex(p, g_pm_exc[bl].sel);
                            p = zput(p, ":0x"); p = zhex(p, g_pm_exc[bl].off);
                            break; }
                        /* ── 06xx PAGE LOCKING / 07xx DEMAND-PAGING HINTS ────────────────
                           These are all trivially satisfiable here, and that is a FACT about
                           this host rather than a shortcut: our 0501 blocks are VirtualAlloc'd
                           MEM_COMMIT in our own process and are never paged out by us. There
                           is no virtual memory to lock, so "lock" is already true and "unlock"
                           costs nothing; the 07xx pair are explicitly advisory in the spec
                           ("the host may ignore this call"). Returning CF=0 is the correct
                           answer, not a stub. Doom asks 0702 six times.
                           0604 (get page size) is the one that carries information: 4096 on
                           every x86, which is not a guess -- it is architecture. */
                        case 0x0600:                               /* lock linear region      */
                        case 0x0601:                               /* unlock linear region    */
                        case 0x0602:                               /* unlock real-mode region */
                        case 0x0603:                               /* relock real-mode region */
                        case 0x0701:                               /* discard page contents (advisory) */
                        case 0x0702:                               /* mark pages demand-paging candidates (advisory) */
                            VDM_REG(tib, VTIB_EFLAGS) &= ~1u;
                            p = zput(p, " -> paging no-op (no virtual memory here)");
                            break;
                        case 0x0604:                               /* get page size -> BX:CX */
                            VDM_SET16(tib, VTIB_EBX, 0);
                            VDM_SET16(tib, VTIB_ECX, 0x1000);
                            VDM_REG(tib, VTIB_EFLAGS) &= ~1u;
                            p = zput(p, " -> page size 4096");
                            break;
                        case 0x0900:                               /* get + disable virtual interrupt state */
                            VDM_SET16(tib, VTIB_EAX, 0x0900 | (g_dpmi_vi & 1));
                            g_dpmi_vi = 0; p = zput(p, " -> cli");
                            break;
                        case 0x0901:                               /* get + enable virtual interrupt state */
                            VDM_SET16(tib, VTIB_EAX, 0x0900 | (g_dpmi_vi & 1));
                            g_dpmi_vi = 1; p = zput(p, " -> sti");
                            break;
                        case 0x0902:                               /* get virtual interrupt state -> AL */
                            VDM_SET16(tib, VTIB_EAX, 0x0900 | (g_dpmi_vi & 1));
                            p = zput(p, " -> getIF ");  p = zhex(p, g_dpmi_vi);
                            break;
                        case 0x0006: {                             /* get base of sel BX -> CX:DX */
                            DWORD b = dpmi_sel_base((WORD)(VDM_REG(tib, VTIB_EBX)));
                            VDM_SET16(tib, VTIB_ECX, b >> 16); VDM_SET16(tib, VTIB_EDX, b & 0xFFFF);
                            p = zput(p, " sel 0x"); p = zhex(p, VDM_REG(tib, VTIB_EBX) & 0xFFFF);
                            p = zput(p, " -> base 0x"); p = zhex(p, b);
                            break; }
                        /* 0007/0008/0009 now act on idx>=1: since run 48 records the switch's
                           code/data/stack selectors (0x0F/0x17/0x1F) in g_ldt[1..3], a client that
                           reconfigures its INITIAL selectors (e.g. a C runtime narrowing DS's limit)
                           must take effect -- else the change silently no-ops and the client faults. */
                        case 0x0007: {                             /* set base of sel BX = CX:DX */
                            int idx = (VDM_REG(tib, VTIB_EBX) & 0xFFFF) >> 3;
                            DWORD b = ((VDM_REG(tib, VTIB_ECX) & 0xFFFF) << 16) | (VDM_REG(tib, VTIB_EDX) & 0xFFFF);
                            if (idx >= 1 && idx < DPMI_LDT_MAX) { g_ldt[idx].base = b; dpmi_install(idx); }
                            p = zput(p, " sel 0x"); p = zhex(p, VDM_REG(tib, VTIB_EBX) & 0xFFFF);
                            p = zput(p, " -> setbase 0x"); p = zhex(p, b);
                            break; }
                        case 0x0008: {                             /* set limit of sel BX = CX:DX */
                            int idx = (VDM_REG(tib, VTIB_EBX) & 0xFFFF) >> 3;
                            DWORD l = ((VDM_REG(tib, VTIB_ECX) & 0xFFFF) << 16) | (VDM_REG(tib, VTIB_EDX) & 0xFFFF);
                            if (idx >= 1 && idx < DPMI_LDT_MAX) { g_ldt[idx].limit = l; dpmi_install(idx); }
                            p = zput(p, " sel 0x"); p = zhex(p, VDM_REG(tib, VTIB_EBX) & 0xFFFF);
                            p = zput(p, " -> setlimit 0x"); p = zhex(p, l);
                            break; }
                        case 0x0009: {                             /* set access rights of sel BX (CX) */
                            int idx = (VDM_REG(tib, VTIB_EBX) & 0xFFFF) >> 3;
                            if (idx >= 1 && idx < DPMI_LDT_MAX) {
                                /* CL = access byte (P|DPL|S|type). CH = descriptor byte 6
                                   (G|D/B|L|AVL|limit19:16); its HIGH nibble carries G/D/B/L/AVL,
                                   which maps 1:1 onto our flags nibble (see dpmi_build_desc).
                                   #3 (DOS/4GW): a 32-bit code selector arrives here with CH bit6
                                   (D/B) set -> flags bit2 -> dpmi_sel_is32() true. */
                                g_ldt[idx].access = VDM_REG(tib, VTIB_ECX) & 0xFF;
                                g_ldt[idx].flags  = (VDM_REG(tib, VTIB_ECX) >> 12) & 0xF;  /* CH high nibble */
                                dpmi_install(idx);
                                /* "This region is now code" is the client naming its own
                                   requirement -- and the only notice we get that a module
                                   it just loaded is about to be executed. Patch its INT
                                   sites now; see dpmi_patch_code_region(). */
                                if (DPMI_ACC_IS_CODE(g_ldt[idx].access)) {
                                    dpmi_patch_code_region(g_ldt[idx].base, g_ldt[idx].limit,
                                                           (g_ldt[idx].flags & 0x4) != 0);
                                    /* The client naming ANY region code means it has finished
                                       loading something -- a good moment to re-look at the
                                       blocks holding its EXEC objects. On DOOM.EXE this is the
                                       25-byte 16-bit alias object, declared code four log lines
                                       after the last page of the game's code object arrived. */
                                    need_scan = 1;
                                }
                            }
                            p = zput(p, " sel 0x"); p = zhex(p, VDM_REG(tib, VTIB_EBX) & 0xFFFF);
                            p = zput(p, " -> setaccess 0x"); p = zhex(p, VDM_REG(tib, VTIB_ECX) & 0xFFFF);
                            break; }
                        case 0x000A: {                             /* create data alias of sel BX */
                            int src = (VDM_REG(tib, VTIB_EBX) & 0xFFFF) >> 3, idx;
                            if (g_ldt_next >= DPMI_LDT_MAX) { VDM_REG(tib, VTIB_EFLAGS) |= 1u; p = zput(p, " -> ENOMEM"); break; }
                            idx = g_ldt_next++;
                            if (src >= 1 && src < DPMI_LDT_MAX) g_ldt[idx] = g_ldt[src];
                            else { g_ldt[idx].base = g_dpmi_code_base; g_ldt[idx].limit = 0xFFFF; g_ldt[idx].flags = 0; }
                            g_ldt[idx].access = 0xF2;              /* data alias */
                            dpmi_install(idx);
                            VDM_SET16(tib, VTIB_EAX, (WORD)((idx << 3) | 7));
                            p = zput(p, " -> alias sel 0x"); p = zhex(p, (idx << 3) | 7);
                            break; }
                        /* 000B/000C GET/SET DESCRIPTOR -- the raw 8-byte descriptor form of
                           0006-0009. DOS/4GW (Doom) is the client that needs them, and the
                           sequence it runs is worth recording because it names its intent:
                             mov bx,ds / mov di,sp / mov es,bx   ; 8-byte buffer on the stack
                             mov ax,000Bh / int 31h              ; read descriptor
                             and byte [es:di+6],0BFh             ; CLEAR the D/B bit
                             mov ax,000Ch / int 31h              ; write it back
                             add sp,8
                           i.e. the extender manages segment widths itself -- which is the same
                           conclusion dpmi_switch_to_pm() reaches from the other direction. */
                        case 0x0003:                               /* get selector increment value */
                            /* The amount to add to a selector to reach the next one in a block
                               allocated by 0000. Our selectors are LDT entries, so 8. */
                            VDM_SET16(tib, VTIB_EAX, 8);
                            VDM_REG(tib, VTIB_EFLAGS) &= ~1u;
                            p = zput(p, " -> sel increment 8");
                            break;
                        case 0x0A00:                               /* get vendor-specific API entry */
                            /* Correct answer is "no such vendor API": CF=1. The default arm
                               already does that, but naming it here stops it reading as a gap
                               in the Doom trace -- DOS/4GW asks, is refused, and carries on. */
                            VDM_REG(tib, VTIB_EFLAGS) |= 1u;
                            p = zput(p, " -> no vendor API (CF=1, correct)");
                            break;
                        case 0x000B: {                             /* get descriptor of sel BX -> ES:DI */
                            int idx = (VDM_REG(tib, VTIB_EBX) & 0xFFFF) >> 3;
                            DWORD esb = dpmi_sel_base((WORD)VDM_REG(tib, VTIB_ES));
                            volatile DWORD *d = (volatile DWORD *)(ULONG_PTR)
                                (esb + (VDM_REG(tib, VTIB_EDI) & 0xFFFF));
                            DWORD lo = 0, hi = 0;
                            if (idx >= 1 && idx < DPMI_LDT_MAX)
                                dpmi_build_desc(g_ldt[idx].base, g_ldt[idx].limit,
                                                g_ldt[idx].access, g_ldt[idx].flags, &lo, &hi);
                            else { VDM_REG(tib, VTIB_EFLAGS) |= 1u; p = zput(p, " -> bad sel"); break; }
                            d[0] = lo; d[1] = hi;
                            p = zput(p, " sel 0x"); p = zhex(p, VDM_REG(tib, VTIB_EBX) & 0xFFFF);
                            p = zput(p, " -> desc 0x"); p = zhex(p, lo);
                            p = zput(p, ":0x"); p = zhex(p, hi);
                            break; }
                        case 0x000C: {                             /* set descriptor of sel BX from ES:DI */
                            int idx = (VDM_REG(tib, VTIB_EBX) & 0xFFFF) >> 3;
                            DWORD esb = dpmi_sel_base((WORD)VDM_REG(tib, VTIB_ES));
                            volatile DWORD *d = (volatile DWORD *)(ULONG_PTR)
                                (esb + (VDM_REG(tib, VTIB_EDI) & 0xFFFF));
                            DWORD lo, hi;
                            if (idx < 1 || idx >= 512) { VDM_REG(tib, VTIB_EFLAGS) |= 1u;
                                                         p = zput(p, " -> bad sel"); break; }
                            lo = d[0]; hi = d[1];
                            /* Exact inverse of dpmi_build_desc(). */
                            g_ldt[idx].limit  = (lo & 0xFFFF) | (((hi >> 16) & 0xF) << 16);
                            g_ldt[idx].base   = ((lo >> 16) & 0xFFFF) | ((hi & 0xFF) << 16)
                                              | (((hi >> 24) & 0xFF) << 24);
                            g_ldt[idx].access = (BYTE)((hi >> 8) & 0xFF);
                            g_ldt[idx].flags  = (BYTE)((hi >> 20) & 0xF);
                            dpmi_install(idx);
                            if (DPMI_ACC_IS_CODE(g_ldt[idx].access)) { /* same rule as 0009 */
                                dpmi_patch_code_region(g_ldt[idx].base, g_ldt[idx].limit,
                                                       (g_ldt[idx].flags & 0x4) != 0);
                                need_scan = 1;
                            }
                            p = zput(p, " sel 0x"); p = zhex(p, VDM_REG(tib, VTIB_EBX) & 0xFFFF);
                            p = zput(p, " <- desc 0x"); p = zhex(p, lo);
                            p = zput(p, ":0x"); p = zhex(p, hi);
                            p = zput(p, " (base 0x"); p = zhex(p, g_ldt[idx].base);
                            p = zput(p, " limit 0x"); p = zhex(p, g_ldt[idx].limit);
                            p = zput(p, " acc 0x"); p = zhexb(p, g_ldt[idx].access);
                            p = zput(p, " flg 0x"); p = zhexb(p, g_ldt[idx].flags); p = zput(p, ")");
                            break; }
                        case 0x0305: {                             /* get state save/restore addresses */
                            /* AX = buffer size, BX:CX = real-mode routine, SI:(E)DI = PM routine.
                               Both routines are register-preserving no-ops here (DPMI_SSR_OFF).
                               The size is nominal rather than 0: nothing is written to the
                               buffer, but a client that allocates AX bytes should not be handed
                               a zero-size allocation. */
                            WORD sel = dpmi_hdlr_code_sel();
                            VDM_SET16(tib, VTIB_EAX, 0x0040);
                            VDM_SET16(tib, VTIB_EBX, DOS_HDLR_SEG);
                            VDM_SET16(tib, VTIB_ECX, DPMI_SSR_OFF);
                            VDM_SET16(tib, VTIB_ESI, sel);
                            VDM_REG  (tib, VTIB_EDI) = DPMI_SSR_OFF;
                            VDM_REG(tib, VTIB_EFLAGS) &= ~1u;      /* CF=0: always succeeds */
                            p = zput(p, " -> saverestore rm=0x"); p = zhex(p, DOS_HDLR_SEG);
                            p = zput(p, ":0x"); p = zhex(p, DPMI_SSR_OFF);
                            p = zput(p, " pm=0x"); p = zhex(p, sel);
                            p = zput(p, ":0x"); p = zhex(p, DPMI_SSR_OFF);
                            p = zput(p, " size=0x40");
                            break; }
                        case 0x0306: {                             /* get raw mode switch addresses */
                            /* THE CALL DOOM DIES ON. Its code right after this BOP is
                               `73 03 e9 44 02` = jnc +3 / jmp +0x244, so CF=1 sends DOS/4GW
                               straight to its abort path. Returning the two entries is what
                               lets it proceed. */
                            WORD sel = dpmi_hdlr_code_sel();
                            VDM_SET16(tib, VTIB_EBX, DOS_HDLR_SEG);        /* real->prot seg   */
                            VDM_SET16(tib, VTIB_ECX, DPMI_RAW2PM_OFF);     /* real->prot off   */
                            VDM_SET16(tib, VTIB_ESI, sel);                 /* prot->real sel   */
                            VDM_REG  (tib, VTIB_EDI) = DPMI_RAW2RM_OFF;    /* prot->real off   */
                            VDM_REG(tib, VTIB_EFLAGS) &= ~1u;              /* CF=0             */
                            p = zput(p, " -> rawswitch r2p=0x"); p = zhex(p, DOS_HDLR_SEG);
                            p = zput(p, ":0x"); p = zhex(p, DPMI_RAW2PM_OFF);
                            p = zput(p, " p2r=0x"); p = zhex(p, sel);
                            p = zput(p, ":0x"); p = zhex(p, DPMI_RAW2RM_OFF);
                            break; }
                        case 0x0500: {                             /* get free memory info -> ES:DI */
                            /* ── REPORT COHERENT NUMBERS, NOT A FIELD OF -1s. ───────────────
                               The 0.9 spec's 30h-byte block is
                                 00 largest available free block (BYTES)
                                 04 max unlocked page allocation   08 max locked page allocation
                                 0C total linear address space (PAGES, incl. already allocated)
                                 10 total unlocked pages           14 free pages
                                 18 total physical pages           1C free linear address space
                                 20 size of paging file/partition  24.. reserved
                               and says a host sets fields it does not support to -1. We used to
                               set ALL of them to -1 except the first, which is legal but tells a
                               client that sizes itself from the PAGE counts precisely nothing --
                               and 0xFFFFFFFF is a value a client may well arithmetic on. We do
                               know these numbers: 0501 is VirtualAlloc in our own process, so the
                               pool is what we say it is. Report it consistently in both units
                               rather than making the client guess. Reserved fields stay -1. */
                            DWORD esb = dpmi_sel_base((WORD)VDM_REG(tib, VTIB_ES));
                            volatile DWORD *info = (volatile DWORD *)(ULONG_PTR)
                                (esb + (VDM_REG(tib, VTIB_EDI) & 0xFFFF));
                            const DWORD pool_bytes = 0x04000000u;          /* 64 MB            */
                            const DWORD pool_pages = pool_bytes >> 12;     /* 0x4000 pages     */
                            int i; for (i = 0; i < 12; ++i) info[i] = 0xFFFFFFFFu;
                            info[0] = pool_bytes;                  /* largest free block, bytes */
                            info[1] = pool_pages;                  /* max unlocked page alloc   */
                            info[2] = pool_pages;                  /* max locked page alloc     */
                            info[3] = pool_pages;                  /* total linear address space*/
                            info[4] = pool_pages;                  /* total unlocked pages      */
                            info[5] = pool_pages;                  /* free pages                */
                            info[6] = pool_pages;                  /* total physical pages      */
                            info[7] = pool_pages;                  /* free linear address space */
                            info[8] = 0;                           /* no paging file            */
                            p = zput(p, " -> meminfo 64MB/0x4000 pages");
                            break; }
                        case 0x0501: {                             /* allocate memory block BX:CX bytes */
                            DWORD sz = ((VDM_REG(tib, VTIB_EBX) & 0xFFFF) << 16) | (VDM_REG(tib, VTIB_ECX) & 0xFFFF);
                            void *mem = VirtualAlloc(NULL, sz ? sz : 1, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
                            if (!mem) { VDM_REG(tib, VTIB_EFLAGS) |= 1u; VDM_SET16(tib, VTIB_EAX, 0x8013);
                                        p = zput(p, " -> ENOMEM"); break; }
                            if (g_dpmi_nblk < DPMI_MEMBLK_MAX) {   /* remember it: see the flat-selector case */
                                int c; BYTE iscode = 0;
                                /* Does this allocation match one of the program's EXEC objects?
                                   DOS/4GW asks for exactly the object's page-rounded virtual
                                   size, so the size IS the client telling us "this block is
                                   about to hold my code". See dpmi_le_learn(). */
                                for (c = 0; c < g_le_ncode; ++c)
                                    if (g_le_code_sz[c] == ((sz + 0xFFFu) & ~0xFFFu)) { iscode = 1; break; }
                                g_dpmi_blk[g_dpmi_nblk].base = (DWORD)(ULONG_PTR)mem;
                                g_dpmi_blk[g_dpmi_nblk].size = sz ? sz : 1;
                                g_dpmi_blk[g_dpmi_nblk].code = iscode;
                                ++g_dpmi_nblk;
                                if (iscode) { p = zput(p, " [LE CODE OBJECT]"); }
                            }
                            { DWORD lin = (DWORD)(ULONG_PTR)mem;   /* in-process: linear = host ptr */
                              VDM_SET16(tib, VTIB_EBX, lin >> 16); VDM_SET16(tib, VTIB_ECX, lin & 0xFFFF);
                              VDM_SET16(tib, VTIB_ESI, lin >> 16); VDM_SET16(tib, VTIB_EDI, lin & 0xFFFF); /* handle=addr */
                              p = zput(p, " -> mem 0x"); p = zhex(p, lin); }
                            /* A new block may be the one the image is being read into, and an
                               EARLIER code block may have finished filling since we last looked. */
                            need_scan = 1;
                            break; }
                        case 0x0502: {                             /* free memory block SI:DI = handle */
                            DWORD h = ((VDM_REG(tib, VTIB_ESI) & 0xFFFF) << 16) | (VDM_REG(tib, VTIB_EDI) & 0xFFFF);
                            if (h) VirtualFree((void *)(ULONG_PTR)h, 0, MEM_RELEASE);
                            p = zput(p, " -> freed");
                            break; }
                        case 0x0300: {                             /* simulate real-mode interrupt: BL=int, ES:DI=RMCS */
                            DWORD intno = VDM_REG(tib, VTIB_EBX) & 0xFF;
                            DWORD esb = dpmi_sel_base((WORD)VDM_REG(tib, VTIB_ES));
                            volatile BYTE *r = (volatile BYTE *)(ULONG_PTR)(esb + (VDM_REG(tib, VTIB_EDI) & 0xFFFF));
                            /* save the client's PM register file */
                            DWORD sA=VDM_REG(tib,VTIB_EAX),sB=VDM_REG(tib,VTIB_EBX),sC=VDM_REG(tib,VTIB_ECX),
                                  sD=VDM_REG(tib,VTIB_EDX),sS=VDM_REG(tib,VTIB_ESI),sDi=VDM_REG(tib,VTIB_EDI),
                                  sBp=VDM_REG(tib,VTIB_EBP),sDs=VDM_REG(tib,VTIB_DS),sEs=VDM_REG(tib,VTIB_ES),
                                  sSs=VDM_REG(tib,VTIB_SS),sSp=VDM_REG(tib,VTIB_ESP),sFl=VDM_REG(tib,VTIB_EFLAGS);
                            /* load the real-mode register block from the RMCS (WORD reads -> clean high) */
                            VDM_REG(tib,VTIB_EDI)=*(volatile WORD*)(r+0x00); VDM_REG(tib,VTIB_ESI)=*(volatile WORD*)(r+0x04);
                            VDM_REG(tib,VTIB_EBP)=*(volatile WORD*)(r+0x08); VDM_REG(tib,VTIB_EBX)=*(volatile WORD*)(r+0x10);
                            VDM_REG(tib,VTIB_EDX)=*(volatile WORD*)(r+0x14); VDM_REG(tib,VTIB_ECX)=*(volatile WORD*)(r+0x18);
                            VDM_REG(tib,VTIB_EAX)=*(volatile WORD*)(r+0x1C); VDM_REG(tib,VTIB_ES)=*(volatile WORD*)(r+0x22);
                            VDM_REG(tib,VTIB_DS)=*(volatile WORD*)(r+0x24);
                            VDM_REG(tib,VTIB_SS)=0x0100; VDM_REG(tib,VTIB_ESP)=0xFF00;  /* host scratch stack */
                            if (intno == 0x21) { m.tp = p; dos_int21(&m); p = m.tp; }
                            /* write results back into the RMCS */
                            *(volatile WORD*)(r+0x1C)=VDM_REG(tib,VTIB_EAX); *(volatile WORD*)(r+0x10)=VDM_REG(tib,VTIB_EBX);
                            *(volatile WORD*)(r+0x18)=VDM_REG(tib,VTIB_ECX); *(volatile WORD*)(r+0x14)=VDM_REG(tib,VTIB_EDX);
                            *(volatile WORD*)(r+0x00)=VDM_REG(tib,VTIB_EDI); *(volatile WORD*)(r+0x04)=VDM_REG(tib,VTIB_ESI);
                            *(volatile WORD*)(r+0x20)=(WORD)VDM_REG(tib,VTIB_EFLAGS);   /* FLAGS -- see 0301 */
                            /* restore the client's PM register file */
                            VDM_REG(tib,VTIB_EAX)=sA;VDM_REG(tib,VTIB_EBX)=sB;VDM_REG(tib,VTIB_ECX)=sC;VDM_REG(tib,VTIB_EDX)=sD;
                            VDM_REG(tib,VTIB_ESI)=sS;VDM_REG(tib,VTIB_EDI)=sDi;VDM_REG(tib,VTIB_EBP)=sBp;VDM_REG(tib,VTIB_DS)=sDs;
                            VDM_REG(tib,VTIB_ES)=sEs;VDM_REG(tib,VTIB_SS)=sSs;VDM_REG(tib,VTIB_ESP)=sSp;VDM_REG(tib,VTIB_EFLAGS)=sFl;
                            VDM_REG(tib,VTIB_EFLAGS) &= ~1u;       /* 0300 succeeds */
                            p = zput(p, " -> simInt 0x"); p = zhex(p, intno);
                            break; }
                        case 0x0302:                               /* ...with an IRET frame */
                        case 0x0301: {                             /* call real-mode FAR proc: ES:DI=RMCS, CX=stack words */
                            /* This is the first PM->V86->PM round-trip. Unlike 0300 (which fakes a
                               real-mode INT by calling dos_int21 host-side), 0301 must actually RUN
                               the client's real-mode procedure in V86: we rewrite the CONTEXT to
                               V86, push a far-return frame pointing at the DPMI_RMRET_BOP catcher,
                               run v86_run() until that BOP (servicing any INT 21h the proc makes),
                               copy the real-mode regs back into the RMCS, then restore PM.
                               ── 0302 IS THE SAME CALL WITH AN IRET FRAME. ─────────────────────
                               The ONLY difference is the frame pushed on the real-mode stack:
                               0301's procedure is entered as if FAR CALLed and ends in RETF, so
                               the frame is CS:IP; 0302's is entered as if by an INTERRUPT and ends
                               in IRET, so FLAGS is pushed underneath. Same catcher, same run loop,
                               same RMCS marshalling -- sharing the case is not a shortcut, it is
                               the actual relationship between the two services.
                               DOS/4GW's own protected-mode INT 21h handler needs 0302 the moment
                               it starts handling calls itself (session 17), because the real-mode
                               DOS entry it forwards to is an interrupt handler and returns by
                               IRET; giving it a RETF frame would leave FLAGS on the stack. */
                            DWORD esb = dpmi_sel_base((WORD)VDM_REG(tib, VTIB_ES));
                            volatile BYTE *r = (volatile BYTE *)(ULONG_PTR)(esb + (VDM_REG(tib, VTIB_EDI) & 0xFFFF));
                            /* --- save the client's PM CONTEXT (full register file + MSW) --- */
                            DWORD pA=VDM_REG(tib,VTIB_EAX),pB=VDM_REG(tib,VTIB_EBX),pC=VDM_REG(tib,VTIB_ECX),
                                  pD=VDM_REG(tib,VTIB_EDX),pSi=VDM_REG(tib,VTIB_ESI),pDi=VDM_REG(tib,VTIB_EDI),
                                  pBp=VDM_REG(tib,VTIB_EBP),pDs=VDM_REG(tib,VTIB_DS),pEs=VDM_REG(tib,VTIB_ES),
                                  pFs=VDM_REG(tib,VTIB_FS),pGs=VDM_REG(tib,VTIB_GS),pCs=VDM_REG(tib,VTIB_CS),
                                  pIp=VDM_REG(tib,VTIB_EIP),pSs=VDM_REG(tib,VTIB_SS),pSp=VDM_REG(tib,VTIB_ESP),
                                  pFl=VDM_REG(tib,VTIB_EFLAGS);
                            WORD pMsw = *(volatile WORD *)(tib + VTIB_MSW);
                            /* real-mode target + stack from the RMCS (default SS:SP to the code seg) */
                            WORD rcs = *(volatile WORD*)(r+0x2C), rip = *(volatile WORD*)(r+0x2A);
                            WORD rss = *(volatile WORD*)(r+0x30), rsp = *(volatile WORD*)(r+0x2E);
                            unsigned rt; int done = 0;
                            if (rss == 0) { rss = (WORD)(g_dpmi_code_base >> 4); rsp = 0xFF00; }
                            p = zput(p, (ax == 0x0302) ? " -> callRM(iret) 0x" : " -> callRM 0x");
                            p = zhex(p, rcs); p = zput(p, ":0x"); p = zhex(p, rip);
                            p = zput(p, " SS:SP=0x"); p = zhex(p, rss); p = zput(p, ":0x"); p = zhex(p, rsp);
                            /* ► THE POINTER ARGUMENT, BECAUSE THAT IS WHAT GOES WRONG HERE.
                                 Every pointer-taking DOS call arrives as DS:DX in the RMCS, and
                                 the client is responsible for having copied the string DOWN into
                                 real-mode-addressable memory first. When Doom's AH=3Dh open came
                                 through with an EMPTY name there was no way to tell whether the
                                 client had copied nothing or we were reading the wrong place.
                                 Print both the pointer and what is actually AT it. */
                            { WORD rds = *(volatile WORD*)(r+0x24), rdx = *(volatile WORD*)(r+0x14);
                              DWORD lin = ((DWORD)rds << 4) + rdx;
                              const BYTE *sb = (const BYTE *)(ULONG_PTR)lin;
                              p = zput(p, " AX=0x"); p = zhex(p, *(volatile WORD*)(r+0x1C));
                              p = zput(p, " BX=0x"); p = zhex(p, *(volatile WORD*)(r+0x10));
                              p = zput(p, " CX=0x"); p = zhex(p, *(volatile WORD*)(r+0x18));
                              p = zput(p, " DS:DX=0x"); p = zhex(p, rds); p = zput(p, ":0x"); p = zhex(p, rdx);
                              /* And WHERE we read the RMCS from -- ES:EDI, full width. The
                                 offset is masked to 16 bits below, which is right only while
                                 the caller is 16-bit code; print it so a garbage RMCS can be
                                 told apart from a correctly-read one that says something odd. */
                              p = zput(p, " [RMCS ES:EDI=0x"); p = zhex(p, VDM_REG(tib, VTIB_ES) & 0xFFFF);
                              p = zput(p, ":0x"); p = zhex(p, VDM_REG(tib, VTIB_EDI));
                              p = zput(p, " @0x"); p = zhex(p, (DWORD)(ULONG_PTR)r); p = zput(p, "]");
                              p = zput(p, " @=");
                              if (!host_readable(sb, 16)) p = zput(p, "<unreadable>");
                              else                        p = zdump(p, sb, 16); }
                            p = zput(p, "\r\n"); log_append(LOG_PATH, base, p); serial_out(base, p); p = base;
                            /* push the return frame on the RM stack: [FLAGS] CS IP, with FLAGS
                               present only for 0302 (the procedure will IRET, not RETF). */
                            if (ax == 0x0302) {
                                rsp -= 2; pokew(((DWORD)rss << 4) + rsp,
                                                *(volatile WORD*)(r+0x20));   /* RMCS.Flags */
                            }
                            rsp -= 2; pokew(((DWORD)rss << 4) + rsp, DOS_HDLR_SEG);   /* return CS */
                            rsp -= 2; pokew(((DWORD)rss << 4) + rsp, DPMI_RMRET_OFF);  /* return IP */
                            dpmi_unpatch();   /* restore real `CD nn` so RM ints in the proc vector natively */
                            /* --- rewrite the CONTEXT to V86 with the RMCS register file --- */
                            *(volatile WORD *)(tib + VTIB_MSW) = (WORD)(pMsw & ~MSW_PE_BIT);  /* leave PM */
                            VDM_REG(tib,VTIB_EFLAGS) = 0x20202;    /* VM + IF + reserved bit-1 */
                            VDM_REG(tib,VTIB_EDI)=*(volatile WORD*)(r+0x00); VDM_REG(tib,VTIB_ESI)=*(volatile WORD*)(r+0x04);
                            VDM_REG(tib,VTIB_EBP)=*(volatile WORD*)(r+0x08); VDM_REG(tib,VTIB_EBX)=*(volatile WORD*)(r+0x10);
                            VDM_REG(tib,VTIB_EDX)=*(volatile WORD*)(r+0x14); VDM_REG(tib,VTIB_ECX)=*(volatile WORD*)(r+0x18);
                            VDM_REG(tib,VTIB_EAX)=*(volatile WORD*)(r+0x1C);
                            VDM_SET16(tib,VTIB_ES,*(volatile WORD*)(r+0x22)); VDM_SET16(tib,VTIB_DS,*(volatile WORD*)(r+0x24));
                            VDM_SET16(tib,VTIB_FS,rss); VDM_SET16(tib,VTIB_GS,rss);
                            VDM_SET16(tib,VTIB_CS,rcs); VDM_REG(tib,VTIB_EIP)=rip;
                            VDM_SET16(tib,VTIB_SS,rss); VDM_REG(tib,VTIB_ESP)=rsp;
                            /* --- nested V86 run loop: run the proc until the return-BOP --- */
                            for (rt = 0; rt < 128 && !done; ++rt) {
                                LONG rst; DWORD rev = v86_run(tib, &rst);
                                DWORD info = VDM_REG(tib, VTIB_EVENT_INFO) & 0xFF;
                                if (rev == VDM_EVENT_BOP && info == DPMI_RMRET_BOP) {
                                    done = 1; break;                /* proc RETF'd -> finished */
                                }
                                if (rev == VDM_EVENT_BOP && info == 0x20) {   /* INT 21h from the proc */
                                    m.tp = p; dos_int21(&m); p = m.tp;
                                    VDM_REG(tib, VTIB_EIP) += 3;    /* past the BOP -> the stub IRET */
                                    log_append(LOG_PATH, base, p); serial_out(base, p); p = base;
                                    continue;
                                }
                                if (rev == VDM_EVENT_BOP && info == DPMI_CB_BOP) {  /* proc far-called a 0303 callback */
                                    int cbslot = (int)(((VDM_REG(tib,VTIB_EIP)&0xFFFF) - DPMI_CB_BASE_OFF) / 4);
                                    if (cbslot >= 0 && cbslot < DPMI_CB_SLOTS && g_cb[cbslot].used) {
                                        dpmi_invoke_callback(mp, tib, cbslot);   /* V86->PM handler->V86; sets CS:IP to the return */
                                        continue;
                                    }
                                    p = zput(p, "0301: bad cb slot\r\n"); log_append(LOG_PATH, base, p); serial_out(base, p); p = base;
                                    break;
                                }
                                if (rev == VDM_EVENT_IO || rev == VDM_EVENT_IO_HW || rev == VDM_EVENT_GPFAULT) {
                                    int h; HOST_LOCK();
                                    h = host_try_io(tib, &g_bus); HOST_UNLOCK();
                                    if (h) continue;
                                }
                                p = zput(p, "0301: unexpected RM event=0x"); p = zhex(p, rev);
                                p = zput(p, " info=0x"); p = zhex(p, info);
                                p = zput(p, " CS:IP=0x"); p = zhex(p, VDM_REG(tib,VTIB_CS)&0xFFFF);
                                p = zput(p, ":0x"); p = zhex(p, VDM_REG(tib,VTIB_EIP)&0xFFFF); p = zput(p, "\r\n");
                                log_append(LOG_PATH, base, p); serial_out(base, p); p = base;
                                break;
                            }
                            dpmi_repatch();   /* re-arm the BOP patch before the PM client resumes */
                            /* --- copy the real-mode register file back into the RMCS --- */
                            *(volatile WORD*)(r+0x1C)=VDM_REG(tib,VTIB_EAX); *(volatile WORD*)(r+0x10)=VDM_REG(tib,VTIB_EBX);
                            *(volatile WORD*)(r+0x18)=VDM_REG(tib,VTIB_ECX); *(volatile WORD*)(r+0x14)=VDM_REG(tib,VTIB_EDX);
                            *(volatile WORD*)(r+0x00)=VDM_REG(tib,VTIB_EDI); *(volatile WORD*)(r+0x04)=VDM_REG(tib,VTIB_ESI);
                            *(volatile WORD*)(r+0x08)=VDM_REG(tib,VTIB_EBP);
                            *(volatile WORD*)(r+0x22)=VDM_REG(tib,VTIB_ES);  *(volatile WORD*)(r+0x24)=VDM_REG(tib,VTIB_DS);
                            /* ── AND THE FLAGS, WHICH ARE THE ANSWER, NOT A DETAIL. ─────────────
                                 This block copied eight registers back and silently dropped the
                                 ninth. Every DOS service reports failure in CF, so discarding
                                 FLAGS told the client that every call SUCCEEDED -- and the client
                                 passes that verdict to the application, which believes it.
                                 Doom shows exactly how far a false success travels: `access()`
                                 on doom2f.wad returned "error 2, file not found" with CF=0, so
                                 the runtime concluded the French WAD existed, selected it, opened
                                 it (open failed, also as a "success"), and then read forever from
                                 the handle it never got -- 20969 iterations of `mov ah,3Fh; int
                                 21h` at obj1+0x40985 until the watchdog killed the run.
                                 Watcom's own idiom makes the point: `int 21h; rcl eax,1; ror
                                 eax,1` folds CF into the sign bit of EAX and branches on it, so
                                 CF is not one output among many -- it is the only one it reads.
                                 The DPMI 0.9 spec is explicit that 0300/0301/0302 return the
                                 real-mode register state in the RMCS, and FLAGS is part of it. */
                            *(volatile WORD*)(r+0x20)=(WORD)VDM_REG(tib,VTIB_EFLAGS);
                            /* --- restore the client's PM CONTEXT --- */
                            *(volatile WORD *)(tib + VTIB_MSW) = pMsw;       /* re-enter PM */
                            VDM_REG(tib,VTIB_EAX)=pA;VDM_REG(tib,VTIB_EBX)=pB;VDM_REG(tib,VTIB_ECX)=pC;VDM_REG(tib,VTIB_EDX)=pD;
                            VDM_REG(tib,VTIB_ESI)=pSi;VDM_REG(tib,VTIB_EDI)=pDi;VDM_REG(tib,VTIB_EBP)=pBp;
                            VDM_SET16(tib,VTIB_DS,pDs);VDM_SET16(tib,VTIB_ES,pEs);VDM_SET16(tib,VTIB_FS,pFs);VDM_SET16(tib,VTIB_GS,pGs);
                            VDM_SET16(tib,VTIB_CS,pCs);VDM_REG(tib,VTIB_EIP)=pIp;
                            VDM_SET16(tib,VTIB_SS,pSs);VDM_REG(tib,VTIB_ESP)=pSp;VDM_REG(tib,VTIB_EFLAGS)=pFl;
                            VDM_REG(tib,VTIB_EFLAGS) &= ~1u;        /* CF=0: success */
                            p = zput(p, "0301 -> RM proc returned after "); p = zhex(p, rt);
                            p = zput(p, done ? " steps (OK)" : " steps (NO-RET)");
                            break; }
                        case 0x0303: {                             /* allocate real-mode callback: DS:SI=handler, ES:DI=RMCS -> CX:DX */
                            int s;
                            dpmi_ensure_pmret_sel();   /* lazily install the PM-return catcher selector */
                            for (s = 0; s < DPMI_CB_SLOTS && g_cb[s].used; ++s) {}
                            if (s >= DPMI_CB_SLOTS || g_pmret_sel == 0) {
                                VDM_REG(tib, VTIB_EFLAGS) |= 1u; VDM_SET16(tib, VTIB_EAX, 0x8015);
                                p = zput(p, " -> cb ENOMEM"); break;
                            }
                            g_cb[s].used = 1;
                            g_cb[s].pm_sel = (WORD)VDM_REG(tib, VTIB_DS); g_cb[s].pm_off = VDM_REG(tib, VTIB_ESI) & 0xFFFF;
                            g_cb[s].rm_es  = (WORD)VDM_REG(tib, VTIB_ES); g_cb[s].rm_di  = VDM_REG(tib, VTIB_EDI) & 0xFFFF;
                            VDM_SET16(tib, VTIB_ECX, DOS_HDLR_SEG);
                            VDM_SET16(tib, VTIB_EDX, DPMI_CB_BASE_OFF + s*4);
                            p = zput(p, " -> cb slot "); p = zhex(p, s); p = zput(p, " = 0x");
                            p = zhex(p, DOS_HDLR_SEG); p = zput(p, ":0x"); p = zhex(p, DPMI_CB_BASE_OFF + s*4);
                            p = zput(p, " handler 0x"); p = zhex(p, g_cb[s].pm_sel); p = zput(p, ":0x"); p = zhex(p, g_cb[s].pm_off);
                            break; }
                        default:
                            VDM_REG(tib, VTIB_EFLAGS) |= 1u;       /* CF=1: unsupported */
                            p = zput(p, " -> UNSUP");
                            break;
                        }
                        p = zput(p, "\r\n"); log_append(LOG_PATH, base, p); serial_out(base, p); p = base;
                        if (need_scan) dpmi_scan_code_blocks();
                        VDM_REG(tib, VTIB_EIP) += 2;               /* past the 2-byte INT */
                        return 1;
                    }
                    if (vec == 0x21) {                             /* DOS INT 21h (in PM) */
                        DWORD ah = (ax >> 8) & 0xFF;
                        if (ah == 0x4C) {                          /* terminate */
                            DWORD ver = *(volatile WORD *)(ULONG_PTR)0x1600;
                            p = zput(p, "INT21h AH=4Ch -> client EXIT after "); p = zhex(p, steps);
                            p = zput(p, " svc. ver=0x"); p = zhex(p, ver);
                            p = zput(p, (ver == 0x005A) ? "  <<< DPMI client ran + exited cleanly >>>\r\n"
                                                        : "  <<< MISMATCH >>>\r\n");
                            log_append(LOG_PATH, base, p); serial_out(base, p); p = base;
                            return 0;
                        }
                        if (ah == 0x09) {                          /* print $-string DS:DX */
                            /* Resolve DS's linear base from the LDT so a client can print
                               through a descriptor it allocated + based itself. */
                            DWORD dsb = dpmi_sel_base((WORD)VDM_REG(tib, VTIB_DS));
                            const volatile BYTE *s = (const volatile BYTE *)(ULONG_PTR)
                                (dsb + (VDM_REG(tib, VTIB_EDX) & 0xFFFF));
                            char ob[256]; char *op = ob; int k;
                            op = zput(op, "INT21h AH=09 print: \"");
                            for (k = 0; k < 200 && *s != '$'; ++k, ++s) {
                                if (*s >= 0x20) *op++ = (char)*s;   /* printable -> serial echo */
                                if (m.conout) m.conout(m.conctx, *s);  /* -> the Luna console */
                                if (m.out_len < m.out_cap - 1) m.out[m.out_len++] = (char)*s;
                                else m.out_trunc = 1;
                            }
                            op = zput(op, "\"\r\n");
                            log_append(LOG_PATH, ob, op); serial_out(ob, op);
                            VDM_REG(tib, VTIB_EFLAGS) &= ~1u;
                            VDM_REG(tib, VTIB_EIP) += 2;
                            return 1;
                        }
                        if (ah == 0x02) {                          /* print char DL */
                            BYTE ch = VDM_REG(tib, VTIB_EDX) & 0xFF;
                            if (m.conout) m.conout(m.conctx, ch);
                            if (m.out_len < m.out_cap - 1) m.out[m.out_len++] = (char)ch;
                            else m.out_trunc = 1;
                            VDM_REG(tib, VTIB_EFLAGS) &= ~1u;
                            VDM_REG(tib, VTIB_EIP) += 2;
                            return 1;
                        }
                        if (ah == 0x40) {                          /* write BX=handle CX=cnt DS:DX=buf */
                            DWORD bh = VDM_REG(tib, VTIB_EBX) & 0xFFFF, cnt = VDM_REG(tib, VTIB_ECX) & 0xFFFF;
                            DWORD dsb = dpmi_sel_base((WORD)VDM_REG(tib, VTIB_DS));
                            const volatile BYTE *b = (const volatile BYTE *)(ULONG_PTR)
                                (dsb + (VDM_REG(tib, VTIB_EDX) & 0xFFFF));
                            VDM_REG(tib, VTIB_EFLAGS) &= ~1u;
                            if (bh == 1 || bh == 2) {              /* stdout / stderr */
                                char ob[300]; char *op = ob; DWORD k;
                                op = zput(op, "INT21h AH=40 write: \"");
                                for (k = 0; k < cnt && k < 250; ++k) {
                                    if (b[k] >= 0x20 && op < ob + 270) *op++ = (char)b[k];
                                    if (m.conout) m.conout(m.conctx, b[k]);
                                    if (m.out_len < m.out_cap - 1) m.out[m.out_len++] = (char)b[k];
                                    else m.out_trunc = 1;
                                }
                                op = zput(op, "\"\r\n");
                                log_append(LOG_PATH, ob, op); serial_out(ob, op);
                                VDM_SET16(tib, VTIB_EAX, cnt);     /* AX = bytes written */
                            } else if (bh < 24 && m.fh[bh]) {      /* file handle */
                                DWORD w = 0; WriteFile(m.fh[bh], (const void *)b, cnt, &w, NULL);
                                VDM_SET16(tib, VTIB_EAX, w);
                                p = zput(p, "INT21h AH=40 file write "); p = zhex(p, w); p = zput(p, "b\r\n");
                                log_append(LOG_PATH, base, p); serial_out(base, p); p = base;
                            } else { VDM_REG(tib, VTIB_EFLAGS) |= 1u; VDM_SET16(tib, VTIB_EAX, 6); }
                            VDM_REG(tib, VTIB_EIP) += 2;
                            return 1;
                        }
                        if (ah == 0x3C || ah == 0x3D) {            /* create / open: DS:DX = ASCIIZ name */
                            DWORD dsb = dpmi_sel_base((WORD)VDM_REG(tib, VTIB_DS));
                            const char *fn = (const char *)(ULONG_PTR)(dsb + (VDM_REG(tib, VTIB_EDX) & 0xFFFF));
                            DWORD acc = ((ax & 0xFF) == 0) ? GENERIC_READ
                                       : ((ax & 0xFF) == 1) ? GENERIC_WRITE : (GENERIC_READ | GENERIC_WRITE);
                            HANDLE f = (ah == 0x3C)
                                ? CreateFileA(fn, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL,
                                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL)
                                : CreateFileA(fn, acc, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                                              FILE_ATTRIBUTE_NORMAL, NULL);
                            VDM_REG(tib, VTIB_EFLAGS) &= ~1u;
                            if (f == INVALID_HANDLE_VALUE) { VDM_REG(tib, VTIB_EFLAGS) |= 1u; VDM_SET16(tib, VTIB_EAX, 2); }
                            else { int slot; for (slot = 5; slot < 24 && m.fh[slot]; ++slot) {}
                                   if (slot < 24) { m.fh[slot] = f; VDM_SET16(tib, VTIB_EAX, slot); }
                                   else { CloseHandle(f); VDM_REG(tib, VTIB_EFLAGS) |= 1u; VDM_SET16(tib, VTIB_EAX, 4); } }
                            p = zput(p, "INT21h AH="); p = zhex(p, ah); p = zput(p, " open \"");
                            p = zput(p, fn); p = zput(p, "\" -> AX=0x"); p = zhex(p, VDM_REG(tib, VTIB_EAX) & 0xFFFF);
                            p = zput(p, "\r\n"); log_append(LOG_PATH, base, p); serial_out(base, p); p = base;
                            VDM_REG(tib, VTIB_EIP) += 2; return 1;
                        }
                        if (ah == 0x3E) {                          /* close: BX=handle */
                            DWORD h = VDM_REG(tib, VTIB_EBX) & 0xFFFF;
                            if (h >= 5 && h < 24 && m.fh[h]) { CloseHandle(m.fh[h]); m.fh[h] = 0; }
                            VDM_REG(tib, VTIB_EFLAGS) &= ~1u;
                            VDM_REG(tib, VTIB_EIP) += 2; return 1;
                        }
                        if (ah == 0x3F) {                          /* read: BX=handle CX=cnt -> DS:DX */
                            DWORD h = VDM_REG(tib, VTIB_EBX) & 0xFFFF, cnt = VDM_REG(tib, VTIB_ECX) & 0xFFFF, rd = 0;
                            DWORD dsb = dpmi_sel_base((WORD)VDM_REG(tib, VTIB_DS));
                            void *b = (void *)(ULONG_PTR)(dsb + (VDM_REG(tib, VTIB_EDX) & 0xFFFF));
                            VDM_REG(tib, VTIB_EFLAGS) &= ~1u;
                            if (h < 24 && m.fh[h]) { ReadFile(m.fh[h], b, cnt, &rd, NULL); VDM_SET16(tib, VTIB_EAX, rd); }
                            else { VDM_REG(tib, VTIB_EFLAGS) |= 1u; VDM_SET16(tib, VTIB_EAX, 6); }
                            p = zput(p, "INT21h AH=3F read "); p = zhex(p, rd); p = zput(p, "b\r\n");
                            log_append(LOG_PATH, base, p); serial_out(base, p); p = base;
                            VDM_REG(tib, VTIB_EIP) += 2; return 1;
                        }
                        if (ah == 0x42) {                          /* lseek: AL=org BX=h CX:DX=off */
                            DWORD h = VDM_REG(tib, VTIB_EBX) & 0xFFFF, meth = ax & 0xFF;
                            LONG dist = (LONG)(((VDM_REG(tib, VTIB_ECX) & 0xFFFF) << 16) | (VDM_REG(tib, VTIB_EDX) & 0xFFFF));
                            VDM_REG(tib, VTIB_EFLAGS) &= ~1u;
                            if (h >= 5 && h < 24 && m.fh[h]) {
                                DWORD np = SetFilePointer(m.fh[h], dist, NULL, meth);
                                VDM_SET16(tib, VTIB_EDX, np >> 16); VDM_SET16(tib, VTIB_EAX, np & 0xFFFF);
                            } else VDM_REG(tib, VTIB_EFLAGS) |= 1u;
                            VDM_REG(tib, VTIB_EIP) += 2; return 1;
                        }
                        /* ── REGISTER-ONLY INT 21h: DELEGATE TO THE V86 DOS IMPLEMENTATION ──
                           dos_int21() reads and writes the same VDM_TIB register fields the PM
                           client left behind, so a service that takes NO pointer argument needs
                           no thunking at all -- there is nothing to translate.
                           ► THE WHITELIST IS DELIBERATE, NOT LAZINESS. A service that takes a
                             DS:DX / ES:BX pointer must NOT come through here: in protected mode
                             those registers hold SELECTORS, and handing a selector to code that
                             treats it as a real-mode segment reads or writes whatever happens to
                             live at selector<<4. That is a silent wrong-memory bug of exactly the
                             kind that cost this session a day (see the D/B fix). Pointer-taking
                             services are hand-rolled above with dpmi_sel_base(), or stay loud.
                           ► ES-as-SEGMENT services (49h free, 4Ah resize) are ALSO excluded from
                             the whitelist: the block address they want is a real-mode paragraph,
                             but in PM ES holds a selector. Session 16 said "if Doom calls 49h the
                             log will say so and we can settle the convention on evidence" -- IT
                             DID (twice), so 49h is hand-rolled below, resolving ES through the
                             LDT. 4Ah is still unevidenced and still stays loud.
                           ► 33h (Ctrl-Break / true version) joined the whitelist on the same
                             evidence: Doom calls it twice, and every subfunction dos_int21
                             implements (AL=00/01/05/06) reads and writes GPRs only. Leaving it
                             unhandled was NOT neutral -- the TODO arm returned with AX still
                             0x33xx and CF untouched, and the caller's very next instructions are
                             `xchg ax,cx / cbw / retn`, i.e. it propagates whatever we left.
                           Doom (DOS/4GW) needs 48h to allocate the memory it loads its LE image
                           into -- it is the first DOS call it makes from protected mode. */
                        if (ah == 0x48) {
                            /* ── DOS ALLOCATE, FROM PROTECTED MODE, RETURNS A SELECTOR ──────
                               A raw real-mode segment is useless to a PM client, and Doom
                               proves the convention in its own code: immediately after this
                               call it does
                                   jnc +3 / mov [0x0c4a],ax / mov [0x0980],dl / MOV ES,AX
                               Loading ES from AX only makes sense if AX is a SELECTOR -- with
                               the raw segment 0x151c it is GDT index 0x2A3, which #GPs and
                               silently kills the VDM. That `mov es,ax` IS the specification
                               here, the same way DOS/4GW clearing D/B itself settled the
                               initial-selector width.
                               So: do the real DOS allocation (dos_int21 owns the MCB chain),
                               then hand back a descriptor covering it, IN AX ONLY.

                               ► DO NOT ALSO PUT IT IN DX. Session 16 did, reasoning that it
                                 "matches INT 31h 0100's shape, which costs nothing and is
                                 what a client written against 0100 would expect". It cost
                                 Doom. Real DOS's AH=48h returns AX (and BX on failure) and
                                 PRESERVES EVERYTHING ELSE, so callers keep live values in
                                 the other registers across it -- and DOS/4GW keeps the
                                 request's BYTE SIZE in DX:
                                     mov dx,cx / add dx,0x27 / and dl,0xf0   ; DX = bytes
                                     mov bx,dx / ...shift...                 ; BX = paragraphs
                                     mov ah,48h / int 21h
                                     ...
                                     mov ax,dx                               ; DX still = bytes
                                     mov di,ax / add di,bx / dec di / dec di
                                     movw [di],0xfffe                        ; last word of block
                                 With DX clobbered to the selector (0xcf) instead of the size
                                 (0x40), DI became 0xcd against a 0x4f limit: a write past the
                                 segment end, #GP, and XP terminated the VDM with nothing in
                                 the log. Bisected to the instruction with the pmbp.txt
                                 breakpoints.
                               ► THE GENERAL RULE THIS EARNS: a service's register footprint
                                 is part of its contract. Writing a register the real service
                                 leaves alone is not a harmless bonus, it is a silent
                                 corruption of the caller's state. Return what DOS returns. */
                            DWORD want = VDM_REG(tib, VTIB_EBX) & 0xFFFF;
                            m.tp = p; dos_int21_set_pm(1); dos_int21(&m); dos_int21_set_pm(0); p = m.tp;
                            p = zput(p, "INT21h AH=48 (PM) alloc 0x"); p = zhex(p, want);
                            if (VDM_REG(tib, VTIB_EFLAGS) & 1u) {
                                p = zput(p, " -> FAILED, largest 0x");
                                p = zhex(p, VDM_REG(tib, VTIB_EBX) & 0xFFFF);
                            } else if (g_ldt_next >= DPMI_LDT_MAX) {
                                VDM_REG(tib, VTIB_EFLAGS) |= 1u;
                                VDM_SET16(tib, VTIB_EAX, 8);       /* insufficient memory */
                                p = zput(p, " -> no free LDT slot");
                            } else {
                                DWORD seg = VDM_REG(tib, VTIB_EAX) & 0xFFFF;
                                int idx = g_ldt_next++;
                                WORD sel;
                                g_ldt[idx].base   = seg << 4;
                                g_ldt[idx].limit  = want ? (want * 16u - 1u) : 0xFFFF;
                                g_ldt[idx].access = 0xF2;          /* present, DPL3, data R/W */
                                g_ldt[idx].flags  = 0;             /* 16-bit, byte granular   */
                                dpmi_install(idx);
                                sel = (WORD)((idx << 3) | 7);
                                VDM_SET16(tib, VTIB_EAX, sel);   /* AX only -- see above */
                                p = zput(p, " -> seg 0x"); p = zhex(p, seg);
                                p = zput(p, " as sel 0x"); p = zhex(p, sel);
                                p = zput(p, " limit 0x"); p = zhex(p, g_ldt[idx].limit);
                            }
                            p = zput(p, "\r\n");
                            log_append(LOG_PATH, base, p); serial_out(base, p); p = base;
                            VDM_REG(tib, VTIB_EIP) += 2;
                            return 1;
                        }
                        if (ah == 0x49) {
                            /* ── DOS FREE, FROM PROTECTED MODE: ES IS A SELECTOR ────────────
                               The mirror image of 48h above, and the convention is forced by
                               it rather than chosen: 48h handed the client a SELECTOR in AX,
                               the client did `mov es,ax`, and the only thing it can pass back
                               to 49h is that selector. So resolve ES through the LDT and free
                               the paragraph its base names. Treating ES as a raw segment here
                               would free whatever MCB happens to live at the selector VALUE --
                               a silent heap corruption, which is precisely the hazard the
                               whitelist comment below exists to prevent.
                               The LDT slot is zeroed rather than reused: g_ldt_next is a bump
                               allocator, so a freed slot is left reclaimable (same treatment
                               as INT 31h 0101) instead of pretending to a free list we do not
                               have. */
                            WORD sel = (WORD)(VDM_REG(tib, VTIB_ES) & 0xFFFF);
                            int idx = sel >> 3;
                            DWORD segbase = dpmi_sel_base(sel);
                            int err;
                            p = zput(p, "INT21h AH=49 (PM) free sel 0x"); p = zhex(p, sel);
                            p = zput(p, " base 0x"); p = zhex(p, segbase);
                            if ((segbase & 0xF) || segbase > 0xFFFFFu) {
                                /* Not a paragraph-aligned conventional-memory base: this is not
                                   a block DOS ever handed out, so refuse LOUDLY rather than
                                   corrupt the MCB chain guessing. */
                                VDM_REG(tib, VTIB_EFLAGS) |= 1u;
                                VDM_SET16(tib, VTIB_EAX, 9);       /* invalid memory block address */
                                p = zput(p, " -> REFUSED (not a DOS paragraph)");
                            } else {
                                err = dos_free(NULL, (uint16_t)(segbase >> 4));
                                if (err) { VDM_REG(tib, VTIB_EFLAGS) |= 1u; VDM_SET16(tib, VTIB_EAX, err);
                                           p = zput(p, " -> err 0x"); p = zhex(p, err); }
                                else { VDM_REG(tib, VTIB_EFLAGS) &= ~1u;
                                       if (idx >= 3 && idx < DPMI_LDT_MAX) { g_ldt[idx].base = g_ldt[idx].limit = 0; }
                                       p = zput(p, " -> freed seg 0x"); p = zhex(p, segbase >> 4); }
                            }
                            p = zput(p, "\r\n");
                            log_append(LOG_PATH, base, p); serial_out(base, p); p = base;
                            VDM_REG(tib, VTIB_EIP) += 2;
                            return 1;
                        }
                        if (ah == 0x25 || ah == 0x35) {
                            /* ── SET/GET INTERRUPT VECTOR FROM PROTECTED MODE ───────────────
                               These operate on the PROTECTED-MODE vector, i.e. they are
                               INT 31h 0205/0204 wearing a DOS hat, and must never reach
                               dos_int21() -- which writes DS:DX straight into the real-mode
                               IVT at linear (AL*4). In PM that would store a SELECTOR where
                               a segment belongs, in the first kilobyte of guest memory.

                               ► THE SPEC IS SILENT HERE. DPMI 0.9 defines INT 31h 0200-0206
                                 for vectors and says only that "DPMI defines a specific
                                 subset of DOS and BIOS calls that can be made by protected
                                 mode DOS programs" -- it does not say which side 25h/35h act
                                 on. Checked, not remembered.
                               ► WHAT SETTLES IT IS THE CLIENT, as usual. DOS/4GW does
                                     mov ax,0x3500 / int 21h        ; save the old vector
                                     mov ax,0x2500 / mov dx,0x2cf3 / int 21h
                                 with DS = 0x9F -- its own CODE SELECTOR -- and 0x2cf3 is a
                                 handler inside that selector. A selector:offset pair cannot
                                 be installed in the real-mode IVT, and a chain built from a
                                 35h that read the real vector and a 25h that wrote the PM one
                                 would be incoherent. So both act on the PM table.
                               ► OUTSTANDING VERIFICATION: confirm against stock ntvdm's own
                                 DPMI host with a text-mode probe (`stock <target>`). Until
                                 then this is forced-by-the-data, not oracle-confirmed. */
                            DWORD al = ax & 0xFF;
                            if (ah == 0x25) {
                                WORD hsel = (WORD)(VDM_REG(tib, VTIB_DS) & 0xFFFF);
                                g_pm_int[al].sel = hsel;
                                g_pm_int[al].off = dpmi_sel_is32(hsel) ? VDM_REG(tib, VTIB_EDX)
                                                                       : (VDM_REG(tib, VTIB_EDX) & 0xFFFF);
                                g_pm_int[al].client = 1;
                                p = zput(p, "INT21h AH=25 (PM) set PM vector 0x"); p = zhex(p, al);
                                p = zput(p, " = 0x"); p = zhex(p, g_pm_int[al].sel);
                                p = zput(p, ":0x"); p = zhex(p, g_pm_int[al].off);
                            } else {
                                VDM_SET16(tib, VTIB_ES, g_pm_int[al].sel);
                                if (dpmi_sel_is32(g_pm_int[al].sel)) VDM_REG(tib, VTIB_EBX) = g_pm_int[al].off;
                                else VDM_SET16(tib, VTIB_EBX, g_pm_int[al].off & 0xFFFF);
                                p = zput(p, "INT21h AH=35 (PM) get PM vector 0x"); p = zhex(p, al);
                                p = zput(p, " -> 0x"); p = zhex(p, g_pm_int[al].sel);
                                p = zput(p, ":0x"); p = zhex(p, g_pm_int[al].off);
                            }
                            VDM_REG(tib, VTIB_EFLAGS) &= ~1u;
                            p = zput(p, "\r\n");
                            log_append(LOG_PATH, base, p); serial_out(base, p); p = base;
                            VDM_REG(tib, VTIB_EIP) += 2;
                            return 1;
                        }
                        if (ah == 0x4A) {
                            /* ── DOS RESIZE, FROM PROTECTED MODE ────────────────────────────
                               ES is a selector, exactly as for 49h, and the evidence arrived
                               the same way: session 16 left 4Ah loud pending a client that
                               actually calls it, and DOS/4GW does -- immediately after the
                               48h whose block it is shrinking (ES=0xcf, BX=0x40 paras).
                               Resolve ES through the LDT, resize the real block, and then
                               UPDATE THE DESCRIPTOR: the client goes on using the selector it
                               already holds, so a limit left describing the old size is either
                               a spurious #GP (grown block) or a licence to run off the end of
                               the heap (shrunk one).
                               Register footprint is DOS's: nothing on success; AX = error and
                               BX = largest available on failure. See the AH=48h note above for
                               what happens when we improvise extra return values. */
                            WORD sel = (WORD)(VDM_REG(tib, VTIB_ES) & 0xFFFF);
                            int idx = sel >> 3;
                            DWORD segbase = dpmi_sel_base(sel);
                            DWORD want = VDM_REG(tib, VTIB_EBX) & 0xFFFF;
                            uint16_t max = 0;
                            p = zput(p, "INT21h AH=4A (PM) resize sel 0x"); p = zhex(p, sel);
                            p = zput(p, " base 0x"); p = zhex(p, segbase);
                            p = zput(p, " to 0x"); p = zhex(p, want); p = zput(p, " paras");
                            if ((segbase & 0xF) || segbase > 0xFFFFFu) {
                                VDM_REG(tib, VTIB_EFLAGS) |= 1u;
                                VDM_SET16(tib, VTIB_EAX, 9);   /* invalid memory block address */
                                p = zput(p, " -> REFUSED (not a DOS paragraph)");
                            } else {
                                int err = dos_resize(NULL, (uint16_t)(segbase >> 4),
                                                     (uint16_t)want, &max);
                                if (err) {
                                    VDM_REG(tib, VTIB_EFLAGS) |= 1u;
                                    VDM_SET16(tib, VTIB_EAX, err);
                                    if (err == 8) VDM_SET16(tib, VTIB_EBX, max);
                                    p = zput(p, " -> err 0x"); p = zhex(p, err);
                                    p = zput(p, " max 0x"); p = zhex(p, max);
                                } else {
                                    VDM_REG(tib, VTIB_EFLAGS) &= ~1u;
                                    if (idx >= 1 && idx < DPMI_LDT_MAX) {
                                        g_ldt[idx].limit = want ? (want * 16u - 1u) : 0;
                                        dpmi_install(idx);
                                        p = zput(p, " -> ok, sel limit now 0x");
                                        p = zhex(p, g_ldt[idx].limit);
                                    } else p = zput(p, " -> ok (no descriptor to update)");
                                }
                            }
                            p = zput(p, "\r\n");
                            log_append(LOG_PATH, base, p); serial_out(base, p); p = base;
                            VDM_REG(tib, VTIB_EIP) += 2;
                            return 1;
                        }
                        /* AH=44h IOCTL: only the register-only subfunctions. AL=00 (get
                           device info, BX in / DX out) is what a C runtime's isatty() uses
                           and what DOS/4GW calls for handles 0..4; AL=06/07 (input/output
                           status) likewise touch no memory. Everything else takes a DS:DX
                           buffer and stays loud -- the whitelist rule, applied within a
                           function rather than to it. */
                        if (ah == 0x44) {
                            DWORD al = ax & 0xFF;
                            if (al != 0x00 && al != 0x06 && al != 0x07) goto pm_int21_unhandled;
                        }
                        /* AH=06h direct console I/O is register-only in BOTH directions
                           (DL=char out, DL=FFh -> AL=char in, ZF), so it thunks with no
                           translation. It is also how DOS/4GW prints its FATAL ERRORS --
                           leaving it unimplemented is why "not enough memory for dispatcher
                           data" was invisible for a whole session and had to be
                           reconstructed character by character out of the TODO log lines. */
                        if (ah == 0x19 || ah == 0x2A || ah == 0x2C || ah == 0x30 ||
                            ah == 0x33 || ah == 0x58 || ah == 0x06 || ah == 0x44) {
                            m.tp = p; dos_int21_set_pm(1); dos_int21(&m); dos_int21_set_pm(0); p = m.tp;
                            p = zput(p, "INT21h AH=0x"); p = zhex(p, ah);
                            p = zput(p, " (PM, register-only -> V86 DOS) -> AX=0x");
                            p = zhex(p, VDM_REG(tib, VTIB_EAX) & 0xFFFF);
                            p = zput(p, " BX=0x"); p = zhex(p, VDM_REG(tib, VTIB_EBX) & 0xFFFF);
                            p = zput(p, " CF="); p = zhex(p, VDM_REG(tib, VTIB_EFLAGS) & 1u);
                            p = zput(p, "\r\n");
                            log_append(LOG_PATH, base, p); serial_out(base, p); p = base;
                            VDM_REG(tib, VTIB_EIP) += 2;
                            return 1;
                        }
                    pm_int21_unhandled:
                        /* ── UNHANDLED INT 21h FROM PM: SAY ENOUGH TO IDENTIFY IT ────────
                           The session-16 trace reported five calls with "AH=0xff", which is
                           not a DOS function at all -- so either the client really is passing
                           0xFF, or AH is inherited garbage from the caller, or the vector
                           resolution put us here wrongly. The old one-line log could not tell
                           those apart, and guessing between them is exactly the reasoning-
                           instead-of-measuring failure this project keeps paying for. Dump the
                           full register file and the caller's bytes so the NEXT run names it.
                           Note the caller's INT is 2 bytes back from EIP (the patched BOP). */
                        { DWORD cs = VDM_REG(tib, VTIB_CS) & 0xFFFF;
                          DWORD cb = dpmi_sel_base((WORD)cs);
                          DWORD ip = VDM_REG(tib, VTIB_EIP);
                          p = zput(p, "INT21h AH=0x"); p = zhex(p, ah); p = zput(p, " (PM thunk TODO)");
                          p = zput(p, " EAX=0x"); p = zhex(p, VDM_REG(tib, VTIB_EAX));
                          p = zput(p, " EBX=0x"); p = zhex(p, VDM_REG(tib, VTIB_EBX));
                          p = zput(p, " ECX=0x"); p = zhex(p, VDM_REG(tib, VTIB_ECX));
                          p = zput(p, " EDX=0x"); p = zhex(p, VDM_REG(tib, VTIB_EDX));
                          p = zput(p, " DS=0x");  p = zhex(p, VDM_REG(tib, VTIB_DS) & 0xFFFF);
                          p = zput(p, " ES=0x");  p = zhex(p, VDM_REG(tib, VTIB_ES) & 0xFFFF);
                          p = zput(p, " cs:eip=0x"); p = zhex(p, cs);
                          p = zput(p, ":0x"); p = zhex(p, ip);
                          p = zput(p, " bytes@int=");
                          /* Same guard as the checkpoint dump, and for the same reason it
                             must be host_readable() and never IsBadReadPtr: a probe that
                             faults on purpose is caught by our own VEH mid-PM-run. */
                          { const BYTE *ib = (const BYTE *)(ULONG_PTR)(cb + ip - 2);
                            if (!host_readable(ib, 16)) p = zput(p, "<unreadable from host>");
                            else                        p = zdump(p, ib, 16); }
                          p = zput(p, "\r\n"); }
                        /* ── AND ANSWER IT THE WAY OUR OWN DOS ANSWERS AN UNHANDLED
                              SERVICE: CF=1. ──────────────────────────────────────────
                           This arm used to return with the flags exactly as the client
                           left them, which in practice means CF=0 -- it told the client
                           its request SUCCEEDED. dos_int21()'s unhandled arm sets CF=1
                           and says why: "a quiet success would tell the program its
                           request worked when nothing happened". The protected-mode path
                           has no business disagreeing with the real-mode path about that.
                           It matters here: DOS/4GW routes these through a generic register-
                           block thunk, so whatever it is probing for, a false success sends
                           it down the branch for a feature we do not have. AX is left alone,
                           same as ERRCF() in dos_int21 -- CF is the answer, not a code we
                           would be inventing. */
                        VDM_REG(tib, VTIB_EFLAGS) |= 1u;
                        log_append(LOG_PATH, base, p); serial_out(base, p); p = base;
                        VDM_REG(tib, VTIB_EIP) += 2;
                        return 1;
                    }
                    /* ── THE LAST THING THE RUN SAYS SHOULD NAME THE WALL. ───────────
                       This printed only the event and a CS:EIP, and that is how the
                       32-bit EIP truncation hid: "0x187:0x0be7" looked like a wild jump
                       into low memory when it was really 0x03b10be7, two bytes past the
                       BOP we had just planted in Doom's own code. Dump the linear
                       address, the instruction bytes and the register file, so the run
                       that ends here identifies its own cause instead of needing a
                       breakpoint sweep to re-find it. */
                    { DWORD csv = VDM_REG(tib, VTIB_CS) & 0xFFFF;
                      DWORD lin = dpmi_sel_base((WORD)csv) + eip;
                      p = zput(p, "DPMI: unexpected PM stop event=0x"); p = zhex(p, ev);
                      p = zput(p, " CS:EIP=0x"); p = zhex(p, csv);
                      p = zput(p, ":0x"); p = zhex(p, eip);
                      p = zput(p, " linear=0x"); p = zhex(p, lin);
                      p = zput(p, (csv && dpmi_sel_is32((WORD)csv)) ? " (32-bit CS)" : " (16-bit CS)");
                      p = zput(p, " EAX=0x"); p = zhex(p, VDM_REG(tib, VTIB_EAX));
                      p = zput(p, " EBX=0x"); p = zhex(p, VDM_REG(tib, VTIB_EBX));
                      p = zput(p, " ECX=0x"); p = zhex(p, VDM_REG(tib, VTIB_ECX));
                      p = zput(p, " EDX=0x"); p = zhex(p, VDM_REG(tib, VTIB_EDX));
                      p = zput(p, " DS=0x");  p = zhex(p, VDM_REG(tib, VTIB_DS) & 0xFFFF);
                      p = zput(p, " SS:ESP=0x"); p = zhex(p, VDM_REG(tib, VTIB_SS) & 0xFFFF);
                      p = zput(p, ":0x"); p = zhex(p, VDM_REG(tib, VTIB_ESP));
                      /* Two bytes back is the BOP/INT itself, on the same +2 convention
                         the patched-INT path uses. host_readable(), never IsBadReadPtr:
                         a probe that faults on purpose kills the run it exists to watch. */
                      p = zput(p, " bytes@eip-2=");
                      { const BYTE *ib = (const BYTE *)(ULONG_PTR)(lin - 2);
                        if (lin < 2 || !host_readable(ib, 16)) p = zput(p, "<unreadable from host>");
                        else                                   p = zdump(p, ib, 16); }
                      p = zput(p, "\r\n"); }
                    log_append(LOG_PATH, base, p); serial_out(base, p); p = base;
                    return -1;
#undef m
}

/* Lazily install the PM-return catcher selector (g_pmret_sel): a code selector based
   at DOS_HDLR_SEG so a PM handler's IRET lands on the planted DPMI_PMRET BOP. Shared by
   the 0303 real-mode-callback path and the async-IRQ injector (#2b). */
static void dpmi_ensure_pmret_sel(void)
{
    if (g_pmret_sel == 0 && g_ldt_next < DPMI_LDT_MAX) {
        int idx = g_ldt_next++;
        g_ldt[idx].base = (DWORD)DOS_HDLR_SEG << 4; g_ldt[idx].limit = 0xFFFF;
        g_ldt[idx].access = 0xFA;                         /* code exec/read */
        /* ── THE CATCHER'S D/B BIT IS PART OF THE CALLER'S IDENTITY. ─────────────────
             We push this selector as the RETURN CS of the interrupt frame the client's
             handler runs on, so that its IRET lands back on our BOP. But a DOS extender
             READS that return CS: it is how the handler learns whether the code it
             interrupted was 16- or 32-bit, and therefore whether a pointer argument in
             (E)DX is a word or a dword. Leaving it 16-bit told DOS/4GW that every caller
             was 16-bit, and it TRUNCATED the application's flat pointers to their low
             word -- measured, on Doom's open of default.cfg:
                 app passed   DS:EDX = 0x18f:0x03b69b80  -> "default.cfg"
                 RMCS got     DS:DX  = 0x000:0x9b80      -> garbage, open failed with
                                                            "file not found"
             and its 16-bit stack frame was ALREADY correct (SS D/B=1), which is what
             made this hard to see: the frame width and the caller's advertised width are
             two different questions, and only one of them was being answered.
             Follow g_dpmi_client32, exactly as the frame width does (h32). A 16-bit
             client is unaffected: flags stay 0 and every existing test keeps its
             6-byte frame and 16-bit catcher. */
        g_ldt[idx].flags = g_dpmi_client32 ? 0x4 : 0x0;   /* 0x4 = D/B */
        dpmi_install(idx);
        g_pmret_sel = (WORD)((idx << 3) | 7);
    }
}

/* GH #18 #2b: asynchronously inject a hardware interrupt (IRQ0 -> INT `iv`) into the
   client's INSTALLED protected-mode vector (g_pm_int[iv], set via INT 31h 0205). This is
   the mechanism timer-hooking games (e.g. Doom) rely on: they hook INT 08h/1Ch and expect
   it to fire ~18.2x/s. We snapshot the interrupted PM context, build an IRET frame on the
   client's own PM stack pointing at the PM-return catcher, vector to the client's handler,
   run it through the SAME dispatcher the main loop uses (so a handler that itself does INT
   21h/31h or port I/O still works), and on its IRET restore the interrupted context verbatim.
   Faithful to a real INT: clears the virtual-IF (g_dpmi_vi) for the duration, restores it
   after. Returns 1 if the handler ran to its IRET, 0 otherwise. */
/* Build a protected-mode interrupt frame ON THE SUSPENDED THREAD'S OWN CONTEXT and vector
   it at the client's handler. Runs on the TIMER thread with the CPU thread suspended, so
   it may touch guest memory and the context but must not log, allocate an LDT entry, or
   block. Returns 1 if `cx` was rewritten and should be committed.
   ► THE RETURN PATH IS THE CATCHER, exactly as for the synchronous injector: the frame's
     return CS:EIP is g_pmret_sel:DPMI_PMRET_OFF, so the handler's IRET lands on a BOP and
     the main loop gets control back. What the main loop CANNOT recover from the IRET is
     where the guest actually was -- that return address was overwritten with the catcher's
     -- so the interrupted CS:EIP:SS:ESP:EFLAGS are saved here and restored there.
   ► ONE IN FLIGHT AT A TIME, claimed with an interlocked compare-exchange, because this
     runs on a different thread from the one that clears it. */
static int dpmi_async_inject_pm(unsigned irq, CONTEXT *cx)
{
    unsigned iv = 0x08 + irq;                    /* IRQ0-7 -> PM vectors 08h-0Fh */
    DWORD efl = cx->EFlags;
    WORD  ss;
    if (iv > 0x0F) { g_async_why = 1; return 0; }
    if (g_pm_noirq || g_in_pm_irq) { g_async_why = g_in_pm_irq ? 2 : 3; return 0; }     /* knob off, or a sync injection is running */
    if (g_pmret_sel == 0) { g_async_why = 4; return 0; }              /* no catcher yet -> no way back */
    if (!g_pm_int[iv].client) { g_async_why = 5; return 0; }          /* the client has not hooked this line */
    /* Not before the application has an ISR; see dpmi_inject_pm_irq(). */
    if (g_dpmi_client32 && iv == 0x08 && !g_pm_app_hooked_timer) { g_async_why = 6; return 0; }
    if (!g_dpmi_vi) { g_async_why = 7; return 0; }                    /* the client has interrupts masked */
    if (!(efl & (0x200u | EFLAGS_VIF_BIT))) { g_async_why = 8; return 0; }      /* ...and the CPU agrees */
    /* Same hold-off the cooperative path uses: a vector installed microseconds ago is an
       arming pass, and real IRQ0 could not have arrived yet. See INT 31h 0205. */
    if ((GetTickCount() - g_pm_vec8_armed_ms) < DPMI_IRQ0_ARM_QUIET_MS) { g_async_why = 9; return 0; }
    if (InterlockedCompareExchange(&g_async_pm_active, 1, 0) != 0) { g_async_why = 10; return 0; }

    ss = (WORD)(cx->SegSs & 0xFFFF);
    if (!(ss & 4)) { g_async_pm_active = 0; g_async_why = 11; return 0; }    /* not a client stack -> not safe */
    /* Same rule as the cooperative path: interrupt the APPLICATION, never the extender
       mid-service. See dpmi_inject_pm_irq() for what that cost to learn. */
    if (g_dpmi_client32 && !dpmi_sel_is32((WORD)(cx->SegCs & 0xFFFF))) {
        g_async_pm_active = 0; g_async_why = 12; return 0;
    }

    /* Save what we are interrupting; the catcher BOP is where it gets put back. */
    g_async_pm_cs  = (WORD)(cx->SegCs & 0xFFFF); g_async_pm_eip = cx->Eip;
    g_async_pm_ss  = ss;                          g_async_pm_esp = cx->Esp;
    g_async_pm_efl = efl;

    /* Frame width is the CLIENT's mode; stack addressing is the SS descriptor's B bit.
       Two different questions -- see dpmi_dispatch_to_pm_handler() for what conflating
       them costs. */
    { DWORD b = dpmi_sel_base(ss);
      int   ss32 = dpmi_sel_is32(ss), h32 = g_dpmi_client32;
      DWORD sp = ss32 ? cx->Esp : (cx->Esp & 0xFFFF);
      if (h32) {
          sp = ss32 ? sp - 4 : ((sp - 4) & 0xFFFF); poked(b + sp, efl);
          sp = ss32 ? sp - 4 : ((sp - 4) & 0xFFFF); poked(b + sp, g_pmret_sel);
          sp = ss32 ? sp - 4 : ((sp - 4) & 0xFFFF); poked(b + sp, DPMI_PMRET_OFF);
      } else {
          sp = ss32 ? sp - 2 : ((sp - 2) & 0xFFFF); pokew(b + sp, (WORD)efl);
          sp = ss32 ? sp - 2 : ((sp - 2) & 0xFFFF); pokew(b + sp, g_pmret_sel);
          sp = ss32 ? sp - 2 : ((sp - 2) & 0xFFFF); pokew(b + sp, DPMI_PMRET_OFF);
      }
      cx->Esp = ss32 ? sp : ((cx->Esp & 0xFFFF0000u) | sp); }

    cx->SegCs  = DPMI_IRQ_TARGET_SEL(iv);
    cx->Eip    = g_dpmi_client32 ? DPMI_IRQ_TARGET_OFF(iv) : (DPMI_IRQ_TARGET_OFF(iv) & 0xFFFF);
    /* ── CLEAR **VIP**, NOT JUST VIF — OR THE GUEST'S NEXT `STI` FAULTS. ─────────────
         The guest runs at CPL 3 with PVI, which is why CLI/STI are survivable there at
         all (measured: both SURVIVE, while INT3 and HLT kill the VDM). Under PVI, `STI`
         sets VIF -- but if **VIP (bit 20) is already set it raises #GP instead**, by
         design, so that the OS can deliver the interrupt it had pending. And a raw
         protected-mode #GP is the one fault XP will not reflect: run 71 watched it with
         a kernel debugger attached and the kernel just tears the VDM down, silently, no
         bugcheck and no break.
         That is exactly this failure. We deliver the tick OURSELVES, behind the
         kernel's back, so the kernel's own pending IRQ0 stays queued: it sets VIP and
         defers (see the EFLAGS_VIP_BIT note in ntvdm.h). Doom's timer ISR executes
         `sti` at obj1+0x1356d on EVERY tick, so the first tick that lands with VIP set
         dies there -- which is why it survived a handful of entries and then vanished
         with no fault, no watchdog line, and nothing after the log's last byte.
         We ARE the delivery, so the interrupt is no longer pending: clear VIP with VIF,
         and clear the kernel's own pending bits too, exactly as the event-3 guard in the
         main loop already does for the stale-pending case. */
    cx->EFlags = efl & ~(0x200u | EFLAGS_VIF_BIT | EFLAGS_VIP_BIT);
    *(volatile DWORD *)(ULONG_PTR)0x714 &= ~3u;      /* the kernel's pending-IRQ bits */
    g_dpmi_vi  = 0;                                  /* ...and our model of it */
    return 1;
}

static int dpmi_inject_pm_irq(dos_machine_t *mp, volatile BYTE *tib, unsigned iv, unsigned steps)
{
    char lb[256], *lp = lb;
    /* snapshot the interrupted PM register file */
    DWORD sEAX=VDM_REG(tib,VTIB_EAX), sEBX=VDM_REG(tib,VTIB_EBX), sECX=VDM_REG(tib,VTIB_ECX);
    DWORD sEDX=VDM_REG(tib,VTIB_EDX), sESI=VDM_REG(tib,VTIB_ESI), sEDI=VDM_REG(tib,VTIB_EDI);
    DWORD sEBP=VDM_REG(tib,VTIB_EBP), sEIP=VDM_REG(tib,VTIB_EIP), sESP=VDM_REG(tib,VTIB_ESP);
    DWORD sEFL=VDM_REG(tib,VTIB_EFLAGS);
    WORD  sCS=(WORD)VDM_REG(tib,VTIB_CS), sSS=(WORD)VDM_REG(tib,VTIB_SS);
    WORD  sDS=(WORD)VDM_REG(tib,VTIB_DS), sES=(WORD)VDM_REG(tib,VTIB_ES);
    WORD  sFS=(WORD)VDM_REG(tib,VTIB_FS), sGS=(WORD)VDM_REG(tib,VTIB_GS);
    int prev_vi = g_dpmi_vi; unsigned ph; int done = 0;
    DWORD t_isr0 = GetTickCount();
    DWORD wpre[DPMI_WATCH_MAX]; int wi;
    for (wi = 0; wi < g_pm_nwatch; ++wi) {
        const BYTE *wp0 = (const BYTE *)(ULONG_PTR)g_pm_watch[wi];
        wpre[wi] = host_readable(wp0, 4) ? *(const DWORD *)wp0 : 0xDEADDEADu;
    }

    dpmi_ensure_pmret_sel();
    if (g_pmret_sel == 0) return 0;
    /* ── DO NOT INTERRUPT THE EXTENDER, ONLY THE APPLICATION. ────────────────────────
         Measured: the first injection landed at mod:0x4b81 -- inside DOS/4GW's own INT
         21h thunk epilogue, on ITS internal 16-bit stack (SS=0xcf, SP=0x1a74) -- and the
         run ended with no further output. Its dispatcher (mod:0x550) immediately reads
         `LAR SS` and then walks an internal stack table at [0xa42], switching stacks and
         bounds-checking the result; arriving there on a stack it did not expect, in the
         middle of servicing a call it had not finished, is not a state it is written to
         survive.
         The case that MATTERS is the other one: the application spinning on its own timer
         counter, in its own 32-bit flat code, which is precisely where a tick has to land
         for the game to advance. So require the interrupted code to be 32-bit whenever
         the client is -- the extender's modules are all 16-bit selectors, so this
         separates "the game is running" from "the extender is mid-service" exactly.
         A 16-bit client keeps the previous behaviour unchanged. */
    if (g_dpmi_client32 && !dpmi_sel_is32(sCS)) return 0;
    /* ...and not before the application actually has an ISR. Delivery still goes through
       the extender's stub, because the extender owns the IDT and must do the dispatching
       (bypassing it produced "fatal error (1001): error in interrupt chain"). */
    if (g_dpmi_client32 && iv == 0x08 && !g_pm_app_hooked_timer) return 0;

    /* push an INT frame (FLAGS/CS/IP) on the client's current PM stack so the handler's IRET
       lands on the catcher; keep the client's own SS so the handler has a valid stack. Frame
       width + entry-offset mask follow the handler CS D-bit: a 32-bit PM INT handler wants a
       dword frame and a full 32-bit entry offset (GH #18 run 83). */
    /* Same rule as dpmi_dispatch_to_pm_handler(): the frame width is the CLIENT's, not
       the handler selector's. For every case confirmed so far the two agree (a 16-bit
       client with a 16-bit handler), so this is a consistency fix rather than a
       behaviour change -- but they diverge exactly where DOS/4GW lives. */
    int h32 = g_dpmi_client32;
    { WORD ss = sSS; DWORD b = dpmi_sel_base(ss);
      int ss32 = dpmi_sel_is32(ss);                /* see the note in the dispatch path */
      DWORD sp = ss32 ? sESP : (sESP & 0xFFFF);
      if (h32) {
          sp = ss32 ? sp - 4 : ((sp - 4) & 0xFFFF); poked(b + sp, sEFL);
          sp = ss32 ? sp - 4 : ((sp - 4) & 0xFFFF); poked(b + sp, g_pmret_sel);
          sp = ss32 ? sp - 4 : ((sp - 4) & 0xFFFF); poked(b + sp, DPMI_PMRET_OFF);
      } else {
          sp = ss32 ? sp - 2 : ((sp - 2) & 0xFFFF); pokew(b + sp, (WORD)sEFL);
          sp = ss32 ? sp - 2 : ((sp - 2) & 0xFFFF); pokew(b + sp, g_pmret_sel);
          sp = ss32 ? sp - 2 : ((sp - 2) & 0xFFFF); pokew(b + sp, DPMI_PMRET_OFF);
      }
      VDM_SET16(tib, VTIB_SS, ss);
      VDM_REG(tib, VTIB_ESP) = ss32 ? sp : ((sESP & 0xFFFF0000u) | sp); }
    g_dpmi_vi = 0;                                 /* mask further virtual interrupts     */
    /* An interrupt gate CLEARS IF and leaves everything else alone. This used to assign
       VTIB_EFLAGS_PM (0x202) outright, which both sets IF -- the opposite of what a gate
       does -- and discards the guest's own flags, DF included, so a handler that returned
       through a string operation would run it in the wrong direction. */
    /* VIP too -- see dpmi_async_inject_pm() for why an STI with VIP set is fatal here. */
    VDM_REG(tib, VTIB_EFLAGS) = (sEFL & ~(0x200u | EFLAGS_VIF_BIT | EFLAGS_VIP_BIT)) | 2u;
    *(volatile DWORD *)(ULONG_PTR)0x714 &= ~3u;
    VDM_SET16(tib, VTIB_CS, DPMI_IRQ_TARGET_SEL(iv));
    VDM_REG(tib, VTIB_EIP) = h32 ? DPMI_IRQ_TARGET_OFF(iv) : (DPMI_IRQ_TARGET_OFF(iv) & 0xFFFF);

    lp = zput(lp, "  IRQ0->PM INT 0x"); lp = zhex(lp, iv);
    lp = zput(lp, " handler 0x"); lp = zhex(lp, DPMI_IRQ_TARGET_SEL(iv));
    lp = zput(lp, ":0x"); lp = zhex(lp, DPMI_IRQ_TARGET_OFF(iv));
    /* What we are about to RUN, and the stack we are about to run it on. This path dies
       with no further output, so anything not printed here is unrecoverable afterwards. */
    { WORD hs = DPMI_IRQ_TARGET_SEL(iv);
      DWORD hl = dpmi_sel_base(hs) + (g_dpmi_client32 ? DPMI_IRQ_TARGET_OFF(iv) : (DPMI_IRQ_TARGET_OFF(iv) & 0xFFFF));
      const BYTE *hb = (const BYTE *)(ULONG_PTR)hl;
      lp = zput(lp, " lin=0x"); lp = zhex(lp, hl);
      lp = zput(lp, dpmi_sel_is32(hs) ? " (h CS D/B=1)" : " (h CS D/B=0)");
      lp = zput(lp, " h32="); lp = zhex(lp, (DWORD)h32);
      lp = zput(lp, " SS:ESP=0x"); lp = zhex(lp, VDM_REG(tib, VTIB_SS) & 0xFFFF);
      lp = zput(lp, ":0x"); lp = zhex(lp, VDM_REG(tib, VTIB_ESP));
      lp = zput(lp, dpmi_sel_is32(sSS) ? " (SS D/B=1)" : " (SS D/B=0)");
      lp = zput(lp, " from 0x"); lp = zhex(lp, sCS); lp = zput(lp, ":0x"); lp = zhex(lp, sEIP);
      lp = zput(lp, " bytes@handler=");
      if (!host_readable(hb, 16)) lp = zput(lp, "<unreadable>");
      else                        lp = zdump(lp, hb, 16); }
    lp = zput(lp, "\r\n");
    log_append(LOG_PATH, lb, lp); serial_out(lb, lp); lp = lb;

    /* ── GIVING UP HALFWAY THROUGH SOMEONE ELSE'S INTERRUPT HANDLER CORRUPTS THEM. ──
         This loop used to stop after 64 phases and then restore the interrupted context
         verbatim, abandoning the client's ISR wherever it had got to. That is not a
         timeout, it is a silent state corruption, and it is what stopped Doom's clock:
             IRQ0<-PM done=0 phases=0x40   [DMX depth]=3<-4   [DMX stack]=...4300<-...5300
         one abandoned dispatch leaked DMX's re-entrancy counter and one 4KB frame of its
         private interrupt stack, permanently. Doom's ticcount froze at 0x61 while 3,000
         more ticks were delivered into a dispatcher that would never call its service
         again, so I_GetTime() stopped, so TryRunTics() spun forever, so the title screen
         sat there for the rest of the run.
         A PHASE IS NOT A UNIT OF TIME -- it is one PM entry, and an ISR pays one for
         every trapped port access. The handler that blew the cap was the MIDI driver
         feeding the MPU-401: ~11 status polls per byte written, so a single music update
         is hundreds of phases. 64 was never a bound on anything real.
         So: bound it by WALL CLOCK, which is the thing actually at risk, keep the phase
         count only as a runaway backstop, and if we ever do stop early SAY SO -- it is a
         corruption event, not housekeeping. A genuinely wedged ISR is the watchdog's
         problem; it already terminates a VDM that stops making progress. */
    for (ph = 0; ph < DPMI_IRQ0_PHASE_MAX && !done; ++ph) {
        DWORD ev, eip, vec; int rc;
        if ((ph & 0x3F) == 0x3F && (GetTickCount() - t_isr0) > DPMI_IRQ0_MS_MAX) break;
        dpmi_arm_fault_trampoline(tib, 0);
        /* ► THE LAST THING BEFORE THE CLIFF. Doom takes five of these injections and
             dies inside the SIXTH: its "IRQ0->PM INT" entry line is the final line in
             the log, dpmi_enter_pm() never returns, and the VDM is gone. The entry line
             above is printed once per injection, so it cannot show what changed BETWEEN
             the fifth and the sixth -- and the five that work are byte-identical in
             every field it prints. Log the state at each PM entry instead, bounded, so
             the fatal one can be DIFFED against its five healthy predecessors. */
        if (g_pm_irq0_done < 12) {
            char eb[192], *eq = eb;
            eq = zput(eq, "   PMENT tick="); eq = zhex(eq, (DWORD)g_pm_irq0_done);
            eq = zput(eq, " ph="); eq = zhex(eq, (DWORD)ph);
            eq = zput(eq, " cs:eip=0x"); eq = zhex(eq, VDM_REG(tib, VTIB_CS) & 0xFFFF);
            eq = zput(eq, ":0x"); eq = zhex(eq, VDM_REG(tib, VTIB_EIP));
            eq = zput(eq, " ss:esp=0x"); eq = zhex(eq, VDM_REG(tib, VTIB_SS) & 0xFFFF);
            eq = zput(eq, ":0x"); eq = zhex(eq, VDM_REG(tib, VTIB_ESP));
            eq = zput(eq, " efl=0x"); eq = zhex(eq, VDM_REG(tib, VTIB_EFLAGS));
            eq = zput(eq, " [714]=0x"); eq = zhex(eq, *(volatile DWORD *)(ULONG_PTR)0x714);
            eq = zput(eq, " vi="); eq = zhex(eq, (DWORD)g_dpmi_vi);
            eq = zput(eq, " apa="); eq = zhex(eq, (DWORD)g_async_pm_active);
            eq = zput(eq, "\r\n"); log_append(LOG_PATH, eb, eq); serial_out(eb, eq);
        }
        dpmi_enter_pm(tib);
        ev  = VDM_REG(tib, VTIB_EVENT);
        eip = dpmi_pm_eip(tib);
        if (ev == VDM_EVENT_BOP && eip == DPMI_PMRET_OFF
            && (VDM_REG(tib, VTIB_CS) & 0xFFFF) == g_pmret_sel) { done = 1; break; }
        if (ev == 3) continue;                     /* "interrupt pending, not entered" -> retry */
        if (ev == VDM_EVENT_IO || ev == VDM_EVENT_IO_HW || ev == VDM_EVENT_GPFAULT) {
            int io_h;
            HOST_LOCK();
            io_h = host_try_io_pm(tib, &g_bus);
            HOST_UNLOCK();
            if (io_h) continue;
        }
        vec = (ev == VDM_EVENT_BOP) ? dpmi_bop_vec(VDM_REG(tib, VTIB_CS) & 0xFFFF, eip) : 0;
        rc = dpmi_service_pm_int(mp, tib, vec, steps);
        if (rc > 0) continue;
        break;                                     /* handler exited / unexpected stop */
    }

    /* ► DID THE HANDLER ACTUALLY FINISH? The entry line alone cannot distinguish "the
         ISR ran and returned" from "the ISR was entered and the run ended inside it",
         and those need completely different fixes. `done` is set only by the catcher
         BOP, i.e. by the handler's own IRET. */
    /* ► AN ABANDONED HANDLER IS A LOUD EVENT. It leaves the client's interrupt
         bookkeeping permanently wrong -- see the phase loop -- so it must never again
         be readable as a routine "done=0". */
    if (!done) {
        char ab2[192], *aq = ab2;
        aq = zput(aq, "DPMI: *** PM ISR ABANDONED after "); aq = zhex(aq, (DWORD)ph);
        aq = zput(aq, " phases / "); aq = zhex(aq, GetTickCount() - t_isr0);
        aq = zput(aq, " ms -- the client's interrupt state is now INCONSISTENT"
                      " (vec 0x"); aq = zhexb(aq, iv);
        aq = zput(aq, ", last cs:eip=0x"); aq = zhex(aq, VDM_REG(tib, VTIB_CS) & 0xFFFF);
        aq = zput(aq, ":0x"); aq = zhex(aq, dpmi_pm_eip(tib));
        aq = zput(aq, ")\r\n");
        log_append(LOG_PATH, ab2, aq); serial_out(ab2, aq);
    }
    /* ► NAME THE VECTOR. This line said "IRQ0" whatever it had just injected, and since
         session 23 gave the DEVICE lines a cooperative path it is used for IRQ5 too --
         so counting these lines to measure the TIMER's delivery over-counts by however
         much the Sound Blaster contributed. Same fault as ASYNC-PM's hardcoded
         "vec=0x08", one function over, and the same fix. */
    if (iv <= 0x0F) { g_pm_coop_line[iv & 7]++;
                      if (iv == 0x08) tick_delivered_note(); }
    { char cb2[128], *cq = cb2;
      cq = zput(cq, "  PMIRQ vec=0x"); cq = zhexb(cq, iv);
      cq = zput(cq, " done="); cq = zhex(cq, (DWORD)done);
      cq = zput(cq, " phases="); cq = zhex(cq, (DWORD)ph);
      cq = zput(cq, " ticks="); cq = zhex(cq, ++g_pm_irq0_done);
      for (wi = 0; wi < g_pm_nwatch; ++wi) {
          const BYTE *wp = (const BYTE *)(ULONG_PTR)g_pm_watch[wi];
          cq = zput(cq, " ["); cq = zhex(cq, g_pm_watch[wi]); cq = zput(cq, "]=0x");
          if (!host_readable(wp, 4)) cq = zput(cq, "????????");
          else cq = zhex(cq, *(const DWORD *)wp);
          cq = zput(cq, "<-0x"); cq = zhex(cq, wpre[wi]);
      }
      cq = zput(cq, "\r\n"); log_append(LOG_PATH, cb2, cq); serial_out(cb2, cq); }
    /* restore the interrupted PM context verbatim + unmask */
    VDM_REG(tib,VTIB_EAX)=sEAX; VDM_REG(tib,VTIB_EBX)=sEBX; VDM_REG(tib,VTIB_ECX)=sECX;
    VDM_REG(tib,VTIB_EDX)=sEDX; VDM_REG(tib,VTIB_ESI)=sESI; VDM_REG(tib,VTIB_EDI)=sEDI;
    VDM_REG(tib,VTIB_EBP)=sEBP; VDM_REG(tib,VTIB_EIP)=sEIP; VDM_REG(tib,VTIB_ESP)=sESP;
    VDM_REG(tib,VTIB_EFLAGS)=sEFL;
    VDM_SET16(tib,VTIB_CS,sCS); VDM_SET16(tib,VTIB_SS,sSS);
    VDM_SET16(tib,VTIB_DS,sDS); VDM_SET16(tib,VTIB_ES,sES);
    VDM_SET16(tib,VTIB_FS,sFS); VDM_SET16(tib,VTIB_GS,sGS);
    g_dpmi_vi = prev_vi;
    return done;
}

/* ================================================================================ *
 *  run 53 (GH #2): host-interpreted protected mode -- the emulation path.           *
 *                                                                                    *
 *  Run 52 proved the kernel DEADLOCKS (not skip-resumes) on a plain-instruction PM  *
 *  #GP, so we cannot let the kernel execute risky PM code. Instead run 16-bit PM in  *
 *  the v86interp core (already proven on the mode-12h fill loops) with g_seg2lin set *
 *  to an LDT-base resolver, so the SAME interpreter walks PM code -- descriptor      *
 *  bases instead of paragraph shifts -- and NEVER hands a faulting instruction to    *
 *  the kernel. An interpreter enforces no descriptor type, so the code-typed-SS      *
 *  write that #GP's the real CPU (run 51's I310102) simply succeeds here.            *
 *                                                                                    *
 *  istep() returns 0 on any opcode it doesn't model. The interpreter deliberately    *
 *  has no INT handler, so `CD nn` stops it cleanly -- we sync the icpu into the       *
 *  VDM_TIB, service the INT through the SAME dpmi_service_pm_int() the kernel path    *
 *  uses, reload, and continue. ANY OTHER unmodeled opcode is logged with its bytes    *
 *  and stops the run -- that report is the spike's to-do signal (the next opcode to   *
 *  add). No BOP patch is needed in this mode (the interpreter reads the raw CD nn).   *
 * ================================================================================ */
static int g_dpmi_use_interp = 0;             /* run 53 toggle (1 = interp fallback, 0 = kernel PM path).
                                                 run 59 (GH #18): 0 to exercise the real-CPU kernel path
                                                 WITH the +0x638 PM-fault trampoline. Flip to 1 to restore
                                                 the VM-confirmed interpreter runs (i310102/DPMIBACK). */

static uint32_t dpmi_seg2lin(uint16_t sel) { return dpmi_sel_base(sel); }

static void dpmi_icpu_load(icpu *c, volatile BYTE *tib)
{
    c->r[0]=(uint16_t)VDM_REG(tib,VTIB_EAX); c->r[1]=(uint16_t)VDM_REG(tib,VTIB_ECX);
    c->r[2]=(uint16_t)VDM_REG(tib,VTIB_EDX); c->r[3]=(uint16_t)VDM_REG(tib,VTIB_EBX);
    c->r[4]=(uint16_t)VDM_REG(tib,VTIB_ESP); c->r[5]=(uint16_t)VDM_REG(tib,VTIB_EBP);
    c->r[6]=(uint16_t)VDM_REG(tib,VTIB_ESI); c->r[7]=(uint16_t)VDM_REG(tib,VTIB_EDI);
    c->seg[0]=(uint16_t)VDM_REG(tib,VTIB_ES); c->seg[1]=(uint16_t)VDM_REG(tib,VTIB_CS);
    c->seg[2]=(uint16_t)VDM_REG(tib,VTIB_SS); c->seg[3]=(uint16_t)VDM_REG(tib,VTIB_DS);
    c->seg[4]=(uint16_t)VDM_REG(tib,VTIB_FS); c->seg[5]=(uint16_t)VDM_REG(tib,VTIB_GS);
    c->ip=(uint16_t)VDM_REG(tib,VTIB_EIP);
    c->flags=VDM_REG(tib,VTIB_EFLAGS);
}
static void dpmi_icpu_store(icpu *c, volatile BYTE *tib)
{
    VDM_SET16(tib,VTIB_EAX,c->r[0]); VDM_SET16(tib,VTIB_ECX,c->r[1]);
    VDM_SET16(tib,VTIB_EDX,c->r[2]); VDM_SET16(tib,VTIB_EBX,c->r[3]);
    VDM_SET16(tib,VTIB_ESP,c->r[4]); VDM_SET16(tib,VTIB_EBP,c->r[5]);
    VDM_SET16(tib,VTIB_ESI,c->r[6]); VDM_SET16(tib,VTIB_EDI,c->r[7]);
    VDM_SET16(tib,VTIB_ES,c->seg[0]); VDM_SET16(tib,VTIB_CS,c->seg[1]);
    VDM_SET16(tib,VTIB_SS,c->seg[2]); VDM_SET16(tib,VTIB_DS,c->seg[3]);
    VDM_SET16(tib,VTIB_FS,c->seg[4]); VDM_SET16(tib,VTIB_GS,c->seg[5]);
    VDM_SET16(tib,VTIB_EIP,c->ip);
    VDM_REG(tib,VTIB_EFLAGS) = (VDM_REG(tib,VTIB_EFLAGS) & 0xFFFF0000u) | (c->flags & 0xFFFFu);
}

/* Returns 0 = client exited (INT 21h AH=4Ch), -1 = stopped on an unmodeled/
   unserviceable opcode (already logged). Never touches the kernel PM path. */
static int dpmi_run_pm_interp(dos_machine_t *mp, volatile BYTE *tib)
{
    icpu c; long guard = 0; char rb[256]; char *r;
    g_seg2lin = dpmi_seg2lin;                 /* interpreter now resolves LDT bases */
    g_sel_desc = dpmi_sel_desc;               /* ...and answers LAR/LSL from g_ldt[] (run 55) */
    dpmi_icpu_load(&c, tib);
    r = rb;
    r = zput(r, "DPMI-INTERP: run 53 host PM begins CS:IP=0x"); r = zhex(r, c.seg[1]);
    r = zput(r, ":0x"); r = zhex(r, c.ip);
    r = zput(r, " DS=0x"); r = zhex(r, c.seg[3]); r = zput(r, " SS=0x"); r = zhex(r, c.seg[2]);
    r = zput(r, "\r\n"); log_append(LOG_PATH, rb, r); serial_out(rb, r);
    for (;;) {
        if (istep(&c)) { if (++guard > 20000000L) break; continue; }   /* modeled step */
        { uint32_t site = seg_base(c.seg[1]) + c.ip;
          uint8_t op = imem_r8(site), op1 = imem_r8(site+1);
          if (op == 0xCD) {                   /* INT nn -> shared DPMI/DOS dispatch */
              int rc;
              dpmi_icpu_store(&c, tib);
              VDM_REG(tib, VTIB_EVENT) = 4;   /* mimic a serviceable BOP for the dispatcher */
              rc = dpmi_service_pm_int(mp, tib, (DWORD)op1, (unsigned)guard);
              if (rc <= 0) return rc;         /* 0 = 4Ch exit, -1 = unserviceable */
              dpmi_icpu_load(&c, tib);        /* pick up results + any selector/mode change */
              continue;
          }
          r = rb;                             /* the spike's to-do signal */
          r = zput(r, "DPMI-INTERP: unmodeled opcode at CS:IP=0x"); r = zhex(r, c.seg[1]);
          r = zput(r, ":0x"); r = zhex(r, c.ip);
          r = zput(r, " bytes="); r = zdump(r, (const BYTE*)(ULONG_PTR)site, 8);
          r = zput(r, " (steps=0x"); r = zhex(r, (unsigned)guard); r = zput(r, ")\r\n");
          log_append(LOG_PATH, rb, r); serial_out(rb, r);
          dpmi_icpu_store(&c, tib);
          return -1;
        }
    }
    dpmi_icpu_store(&c, tib);                 /* guard cap hit (possible infinite loop) */
    r = rb; r = zput(r, "DPMI-INTERP: guard cap hit (spin?)\r\n");
    log_append(LOG_PATH, rb, r); serial_out(rb, r);
    return -1;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow)
{
    char report[8192]; char *p = report; char *base;
    volatile BYTE *tib, *hdlr;
    DWORD nread = 0, err = 0, ev; LONG st;
    dos_image_t img;
    dos_machine_t m;
    char dosout[16384];   /* M9 probe dumps run to several KB; 1024 truncated them */
    char progpath[768]; char args[256];
    unsigned i; int guard;
    static const BYTE bop[] = { VDM_BOP0, VDM_BOP1, 0x20, 0xCF };  /* BOP 0x20 ; iret */
    static const BYTE bop10[] = { VDM_BOP0, VDM_BOP1, 0x10, 0xCF }; /* BOP 0x10 ; iret */
    static const BYTE bop16[] = { VDM_BOP0, VDM_BOP1, 0x16, 0xCF }; /* BOP 0x16 ; iret */
    static const BYTE bop33[] = { VDM_BOP0, VDM_BOP1, 0x33, 0xCF }; /* BOP 0x33 ; iret */
    /* INT 08h (timer): tick via BOP, then chain INT 1Ch, then iret. INT 1Ch is a
       bare iret by default (the user-timer hook a program may repoint). INT 1Ah
       (BIOS time-of-day) is a plain BOP. */
    static const BYTE bop08[] = { VDM_BOP0, VDM_BOP1, 0x08, 0xCD, 0x1C, 0xCF };
    static const BYTE bop1c[] = { 0xCF };                           /* iret stub       */
    /* Default INT 09h = BOP 09 ; IRET. It must CONSUME the scancode, exactly as the BIOS
       handler does: a bare IRET left the byte in the controller forever, so with the 8042's
       proper one-byte-at-a-time pacing no further key could ever raise an interrupt (the
       whole keyboard died after one press). A game that installs its own INT 09h replaces
       this vector, so its handler still reads port 0x60 itself. */
    static const BYTE bop09[] = { VDM_BOP0, VDM_BOP1, 0x09, 0xCF };
    static const BYTE bop1a[] = { VDM_BOP0, VDM_BOP1, 0x1A, 0xCF }; /* BOP 0x1A ; iret */
    static const BYTE bop2f[] = { VDM_BOP0, VDM_BOP1, 0x2F, 0xCF }; /* INT 2Fh ; iret  */
    /* XMS API entry: reached by FAR CALL (INT 2Fh AX=4310 hands back ES:BX), so it
       ends in RETF (0xCB), not IRET. */
    static const BYTE bopxms[] = { VDM_BOP0, VDM_BOP1, 0x43, 0xCB };
    static const BYTE bop67[] = { VDM_BOP0, VDM_BOP1, 0x67, 0xCF }; /* INT 67h ; iret  */
    /* GH #43/#44/#45: the BIOS interrupts we had never planted at all. Until now
       these vectors were filled by the null-vector sweep with a bare IRET, so a
       guest asking for the equipment list or the memory size got silence and
       whatever was already in its registers. */
    /* {vector, BOP number}.  They match for all but INT 20h: BOP 0x20 is ALREADY
       the INT 21h handler's, and planting INT 20h with it made the BIOS dispatch
       intercept every INT 21h call as "terminate program" -- selftest exited at
       its first DOS call with no output. BOP numbers are a shared namespace with
       DPMI (0x50-0x57), XMS (0x43) and the rest; 0x30 is free. */
    static const BYTE bios_ints[][2] = {
        { 0x11, 0x11 }, { 0x12, 0x12 }, { 0x13, 0x13 }, { 0x14, 0x14 },
        { 0x15, 0x15 }, { 0x17, 0x17 }, { 0x25, 0x25 }, { 0x26, 0x26 },
        { 0x20, 0x30 },                                  /* GH #46: see above */
        { 0x27, 0x27 }, { 0x28, 0x28 }, { 0x29, 0x29 },
    };
    static const BYTE emmname[] = { 'E','M','M','X','X','X','X','0' };  /* EMS device header name */
    HANDLE ui = NULL;

    (void)hInst; (void)hPrev; (void)lpCmd; (void)nShow;
    progpath[0] = 0; args[0] = 0;

    p = zput(p, "NTVDMEX clean host\r\nSTAGE0: WinMain entered [build dpmi-harness-v180]\r\n");
    log_write(LOG_PATH, report, p);
    serial_init();                                      /* DPMI harness: COM1 log sink */
    serial_out(report, p);
    /* Headless test mode = the SMB watcher dropped the AUTOEXIT marker. In that mode the
       host must self-exit on guest exit AND bound any infinite run (a visual demo like
       pm32irq/animate never calls INT 21h 4Ch), else rt.bat's `start /wait` blocks forever
       and wedges the watcher (session-9). Latch it once here (the exit path deletes the marker). */
    g_headless = (GetFileAttributesA(AUTOEXIT_PATH) != INVALID_FILE_ATTRIBUTES);
    /* Self-screenshot only when explicitly requested (graphical tests) AND headless, so
       the common non-graphical tests never enter the capture path. Latched once here. */
    { HANDLE hm = CreateFileA(MODEY_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL, OPEN_EXISTING, 0, NULL);
      if (hm != INVALID_HANDLE_VALUE) {
          char mb[32]; DWORD rd4 = 0, v4 = 0, i4; int got = 0;
          ReadFile(hm, mb, sizeof mb - 1, &rd4, NULL); CloseHandle(hm);
          for (i4 = 0; i4 < rd4 && mb[i4] >= '0' && mb[i4] <= '9'; ++i4) { v4 = v4 * 10 + (DWORD)(mb[i4] - '0'); got = 1; }
          if (got && v4 <= 65536u) {
              char lb4[96], *lq = lb4;
              g_vid.modey_gap = v4;
              lq = zput(lq, "STAGE0: modey.txt -> gap="); lq = zhex(lq, v4);
              lq = zput(lq, " dwords\r\n"); log_append(LOG_PATH, lb4, lq); serial_out(lb4, lq);
          }
      } }
    g_capture  = g_headless && (GetFileAttributesA(CAPTURE_FLAG) != INVALID_FILE_ATTRIBUTES);
    if (g_capture) {                       /* its contents, if any, are the period in ms */
        HANDLE hc = CreateFileA(CAPTURE_FLAG, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                NULL, OPEN_EXISTING, 0, NULL);
        if (hc != INVALID_HANDLE_VALUE) {
            char cb3[32]; DWORD rd3 = 0, v3 = 0, i3;
            ReadFile(hc, cb3, sizeof cb3 - 1, &rd3, NULL); CloseHandle(hc);
            for (i3 = 0; i3 < rd3 && cb3[i3] >= '0' && cb3[i3] <= '9'; ++i3)
                v3 = v3 * 10 + (DWORD)(cb3[i3] - '0');
            if (v3 >= 50 && v3 <= 60000) g_capture_ms = v3;
        }
    }
    g_no_a000  = (GetFileAttributesA(NOA000_FLAG) != INVALID_FILE_ATTRIBUTES);
    g_interp12 = (GetFileAttributesA(INTERP12_FLAG) != INVALID_FILE_ATTRIBUTES);
    g_p12_off  = (GetFileAttributesA(P12OFF_FLAG)   != INVALID_FILE_ATTRIBUTES);
    g_opltrace_on = (GetFileAttributesA(OPLTRACE_FLAG) != INVALID_FILE_ATTRIBUTES);
    if (g_opltrace_on) g_opl.trace = opl_trace_write;
    if (g_interp12) g_no_a000 = 1;              /* interpreting instead of trapping */
    /* Headless cap override (decimal ms on the share). Read before the deadline thread
       starts, since that thread sleeps on it. Clamped: below the default a typo would
       kill runs early, above 10 min a typo would wedge the watcher for the whole time. */
    { HANDLE h = CreateFileA(HEADLESS_MS_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             NULL, OPEN_EXISTING, 0, NULL);
      if (h != INVALID_HANDLE_VALUE) {
          char c[16]; DWORD rd = 0, v = 0; int i;
          ReadFile(h, c, sizeof c, &rd, NULL);
          CloseHandle(h);
          for (i = 0; i < (int)rd; ++i) {
              if (c[i] < '0' || c[i] > '9') break;      /* stop at CR/LF/junk */
              v = v * 10 + (DWORD)(c[i] - '0');
          }
          if (v > PM_HEADLESS_MS_DEFAULT && v <= 600000) g_headless_ms = v;
      } }
    /* Async-preemption mode (session 11). Read once; a handle to THIS thread is what
       VdmQueueInterrupt takes, and this thread is the one that will be running the
       guest inside VdmStartExecution -- so duplicate it here, before the exec loop. */
    { HANDLE h = CreateFileA(QIMODE_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             NULL, OPEN_EXISTING, 0, NULL);
      if (h != INVALID_HANDLE_VALUE) {
          char c[2] = { 0, 0 }; DWORD rd = 0; int v = 0, i;
          ReadFile(h, c, 2, &rd, NULL);
          CloseHandle(h);
          for (i = 0; i < (int)rd; ++i) {          /* up to two hex digits */
              int d = -1;
              if (c[i] >= '0' && c[i] <= '9') d = c[i] - '0';
              else if (c[i] >= 'a' && c[i] <= 'f') d = c[i] - 'a' + 10;
              else if (c[i] >= 'A' && c[i] <= 'F') d = c[i] - 'A' + 10;
              if (d < 0) break;
              v = (v << 4) | d;
          }
          if (v > 0) { g_qi_bits  = (DWORD)v & 3;
                       g_qi_raise = (v & 0x04) != 0;
                       g_qi_vif   = (v & 0x08) != 0;
                       if (v & 0x40) g_qi_susp = 0;      /* bit 6 disables async delivery */
                       g_qi_keys  = (v & 0x20) != 0;
                       g_qi_keys_async = (v & 0x80) != 0; }
      } }
    if (g_qi_bits || g_qi_susp) {    /* async delivery needs a handle to the exec thread */
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                        &g_hcpu, 0, FALSE, DUPLICATE_SAME_ACCESS);
    }
    /* ── THE GUEST RAN AT NORMAL PRIORITY AGAINST A TIME_CRITICAL AUDIO THREAD. ──────
         audio_wave.c raises its pump to THREAD_PRIORITY_TIME_CRITICAL because refilling
         waveOut is a hard deadline. Nothing ever raised the thread that RUNS THE GUEST,
         so on this single-core box the mixer thread preempts guest code whenever it has
         work -- and Doom's DMX mixer is guest code that must finish inside one 7.4 ms
         timer tick or its scheduler abandons the pass, recomputes the deadline from NOW,
         and the block it would have filled replays the previous ring lap instead.
         Measured: the mixer NEVER runs on consecutive ticks (2.6% of gaps are one tick,
         49% two, 45% three or four) although we deliver 135 ticks/s against a 140 Hz
         reload -- so it is overrunning, not starved of ticks. And it is not lock
         contention: slicing host_audio_fill's hold into 64-frame pieces moved
         REPLAYED_LOUD by 2 blocks in 894. Preemption is what slicing cannot touch.
       ► ABOVE_NORMAL, not higher. The audio pump stays at 15 so it still wins every
         race it needs to -- starving it is what "a periodic tick or pulse in otherwise
         correct music" was, and that is a worse fault than the one being fixed. This
         only lifts the guest above the UI thread and the system's background work.
       ⚠ Knob, because it is a scheduling change on a box whose behaviour we have been
         wrong about before: execprio.txt absent or 1 = ABOVE_NORMAL (default),
         0 = leave at NORMAL (the old behaviour, for an A/B without a rebuild),
         2 = HIGHEST. */
    /* ── WHICH DSP VERSION WE CLAIM PICKS THE GUEST'S DRIVER PATH. See vdd_sb.h.
         dspver.txt holds "major minor" as two decimal numbers, e.g. "2 1" for a
         Sound Blaster 2.01, which makes DMX skip the mixer-0x82 interrupt gate and
         use the older 0x48/0x1C auto-init pair instead of the SB16 0xC6 command.
         Absent = 4.05, i.e. no change. */
    { HANDLE hv = CreateFileA(DSPVER_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL, OPEN_EXISTING, 0, NULL);
      if (hv != INVALID_HANDLE_VALUE) {
          char c[16]; DWORD rd = 0; int i = 0, mj = 0, mn = 0;
          ReadFile(hv, c, sizeof c, &rd, NULL);
          CloseHandle(hv);
          while (i < (int)rd && c[i] >= '0' && c[i] <= '9') mj = mj * 10 + (c[i++] - '0');
          while (i < (int)rd && (c[i] == ' ' || c[i] == '.')) ++i;
          while (i < (int)rd && c[i] >= '0' && c[i] <= '9') mn = mn * 10 + (c[i++] - '0');
          if (mj > 0 && mj < 256) { g_sb_ver_major = (uint8_t)mj; g_sb_ver_minor = (uint8_t)mn; }
      } }
    { HANDLE hg = CreateFileA(SBGATE_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL, OPEN_EXISTING, 0, NULL);
      if (hg != INVALID_HANDLE_VALUE) {
          char c[8]; DWORD rd = 0;
          ReadFile(hg, c, sizeof c, &rd, NULL); CloseHandle(hg);
          g_sb_gate = (rd && c[0] >= '0' && c[0] <= '9') ? (c[0] - '0') : 1;
      } }
    { DWORD prio = 1;
      HANDLE hp = CreateFileA(EXECPRIO_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL, OPEN_EXISTING, 0, NULL);
      if (hp != INVALID_HANDLE_VALUE) {
          char c[8]; DWORD rd = 0;
          ReadFile(hp, c, sizeof c, &rd, NULL);
          CloseHandle(hp);
          if (rd && c[0] >= '0' && c[0] <= '9') prio = (DWORD)(c[0] - '0');
      }
      g_exec_prio = prio;
      if (prio == 1) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
      else if (prio >= 2) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    }
    if (g_qi_bits) {
        /* Experiment mode: retarget the kernel's PIC so a KERNEL-dispatched IRQ 5 arrives
           as INT 65h while our own injection still arrives as INT 0Dh. Without this the
           two are the same vector and the qirq2 probe cannot attribute a delivery. */
        v86_ica_set_base(0x60);
    }
    p = zput(p, "STAGE0: qi_bits=0x"); p = zhex(p, g_qi_bits);
    p = zput(p, " qi_raise=0x");       p = zhex(p, (DWORD)g_qi_raise);
    p = zput(p, " qi_vif=0x");         p = zhex(p, (DWORD)g_qi_vif);
    p = zput(p, " qi_susp=0x");        p = zhex(p, (DWORD)g_qi_susp);
    p = zput(p, " hcpu=0x");           p = zhex(p, (DWORD)(ULONG_PTR)g_hcpu);
    p = zput(p, "\r\n");
    AddVectoredExceptionHandler(1, dpmi_crash_veh);     /* DPMI spike crash diagnostic */

    /* CSRSS command-info: receive buffers + first-command state + IFEO task id. */
    g_ci.CmdLine = g_cmd; g_ci.CmdLen = sizeof(g_cmd);
    g_ci.AppName = g_app; g_ci.AppLen = sizeof(g_app);
    g_ci.PifFile = g_pif; g_ci.PifLen = sizeof(g_pif);
    g_ci.CurDirectory = g_cur; g_ci.CurDirectoryLen = sizeof(g_cur);
    g_ci.Env = g_env; g_ci.EnvLen = sizeof(g_env);
    g_ci.Desktop = g_desk; g_ci.DesktopLen = sizeof(g_desk);
    g_ci.Title = g_title; g_ci.TitleLen = sizeof(g_title);
    g_ci.Reserved = g_rsv; g_ci.ReservedLen = sizeof(g_rsv);
    g_ci.StartupInfo.cb = sizeof(STARTUPINFOA);
    g_ci.VDMState = VDM_GET_FIRST_COMMAND;
    g_ci.TaskId   = csrss_parse_taskid(GetCommandLineA());

    /* V86 address space, then register as a VDM with the kernel (order matters). */
    v86_setup_memory();
    st = v86_init();
    p = zput(p, "STAGE1: v86_init NTSTATUS=0x"); p = zhex(p, (unsigned)st); p = zput(p, "\r\n");
    /* EMS page frame must be mapped AFTER VdmInitialize (see v86_map_ems_frame). */
    g_ems_frame_lin = v86_map_ems_frame();
    p = zput(p, "STAGE1: ems_frame lin=0x"); p = zhex(p, g_ems_frame_lin);
    p = zput(p, " seg=0x"); p = zhex(p, g_ems_frame_lin >> 4); p = zput(p, "\r\n");

    /* CSRSS: register as the console VDM, then fetch the program to run. */
    csrss_register_console();
    if (csrss_get_command(&g_ci, &err)) {
        p = zput(p, "STAGE1: program "); p = zput(p, g_cur);
        p = zput(p, "\\"); p = zput(p, g_title); p = zput(p, "\r\n");
    } else {
        p = zput(p, "STAGE1: GetNextVDMCommand FALSE err=0x"); p = zhex(p, err); p = zput(p, "\r\n");
    }
    log_write(LOG_PATH, report, p);

    tib = v86_get_tib();
    g_tib_dbg = tib;                                    /* let the crash VEH dump guest state */
    if (!tib) {
        p = zput(p, "STAGE1: no VDM_TIB -- abort\r\n"); log_append(LOG_PATH, report, p);
        return 1;
    }

    /* Load the program: C:\ntvdmex\target.txt override, else CurDir\Title, else a
       tiny exit stub. (target.txt decouples DOS-kernel tests from Title recovery.) */
    {
        HANDLE ht = CreateFileA(TARGET_PATH, GENERIC_READ, FILE_SHARE_READ, NULL,
                                OPEN_EXISTING, 0, NULL);
        if (ht != INVALID_HANDLE_VALUE) {
            char tpath[512]; DWORD tn = 0; char *q; char *a = 0;
            ReadFile(ht, tpath, sizeof(tpath) - 1, &tn, NULL); CloseHandle(ht);
            tpath[tn < sizeof(tpath) ? tn : sizeof(tpath) - 1] = 0;
            for (q = tpath; *q; ++q) {                  /* split "path [args]"; stop at CR/LF */
                if (*q == '\r' || *q == '\n') { *q = 0; break; }
                if (*q == ' ' && !a) { *q = 0; a = q + 1; }
            }
            if (tpath[0]) {
                HANDLE hf = CreateFileA(tpath, GENERIC_READ, FILE_SHARE_READ, NULL,
                                        OPEN_EXISTING, 0, NULL);
                if (hf != INVALID_HANDLE_VALUE) { ReadFile(hf, filebuf, sizeof(filebuf), &nread, NULL); CloseHandle(hf); }
                zput(progpath, tpath);                  /* env argv[0] */
                if (a) zput(args, a);                   /* PSP command tail */
                p = zput(p, "STAGE2: target.txt loaded 0x"); p = zhex(p, nread);
                p = zput(p, " from "); p = zput(p, tpath);
                if (a && a[0]) { p = zput(p, " args=["); p = zput(p, a); p = zput(p, "]"); }
                p = zput(p, "\r\n");
            }
        }
    }
    if (!nread && g_cur[0] && g_title[0]) {
        char path[768]; char *pp = path; HANDLE hf;
        pp = zput(pp, g_cur); pp = zput(pp, "\\"); pp = zput(pp, g_title);
        hf = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (hf != INVALID_HANDLE_VALUE) { ReadFile(hf, filebuf, sizeof(filebuf), &nread, NULL); CloseHandle(hf); }
        zput(progpath, path);                       /* env argv[0] */
        if (g_cmd[0]) zput(args, g_cmd);            /* best-effort: CmdLine if CSRSS populated it */
        p = zput(p, "STAGE2: loaded 0x"); p = zhex(p, nread);
        p = zput(p, " from "); p = zput(p, path); p = zput(p, "\r\n");
    }
    if (!nread) {
        static const BYTE stub[] = { 0xB4, 0x4C, 0xCD, 0x21 };   /* mov ah,4Ch; int 21h */
        for (i = 0; i < sizeof(stub); ++i) filebuf[i] = stub[i];
        nread = sizeof(stub);
        p = zput(p, "STAGE2: embedded fallback\r\n");
    }

    /* status-bar program name = basename of progpath (if any) */
    { const char *bn = progpath, *q; int k = 0;
      for (q = progpath; *q; ++q) if (*q == '\\' || *q == '/') bn = q + 1;
      if (*bn) { while (bn[k] && k < 63) { g_progname[k] = bn[k]; ++k; } g_progname[k] = 0; } }
    g_title_dirty = 1;                             /* UI thread sets the caption + prog name */

    /* If this is a bound linear executable (every DOS/4GW game is one), learn which of
       its objects are code before it starts asking us for memory to load them into. */
    dpmi_le_learn(filebuf, nread);

    /* Build the DOS process in conventional memory (base=NULL => absolute V86). */
    img = dos_load(NULL, filebuf, nread, DOS_PSP_SEG);

    hdlr = (volatile BYTE *)(DOS_HDLR_SEG << 4);            /* INT 21h BOP handler */
    for (i = 0; i < sizeof(bop); ++i) hdlr[i] = bop[i];
    *(volatile WORD *)0x84 = 0x0000;                        /* IVT[0x21].offset    */
    *(volatile WORD *)0x86 = DOS_HDLR_SEG;                  /* IVT[0x21].segment   */
    hdlr[DOS_DBCS_OFF] = 0; hdlr[DOS_DBCS_OFF + 1] = 0;     /* empty DBCS table    */
    for (i = 0; i < sizeof(bop10); ++i) hdlr[0x20 + i] = bop10[i];  /* INT 10h stub */
    *(volatile WORD *)0x40 = 0x0020;                        /* IVT[0x10].offset    */
    *(volatile WORD *)0x42 = DOS_HDLR_SEG;                  /* IVT[0x10].segment   */
    for (i = 0; i < sizeof(bop16); ++i) hdlr[0x28 + i] = bop16[i];  /* INT 16h stub */
    *(volatile WORD *)0x58 = 0x0028;                        /* IVT[0x16].offset    */
    *(volatile WORD *)0x5A = DOS_HDLR_SEG;                  /* IVT[0x16].segment   */
    for (i = 0; i < sizeof(bop33); ++i) hdlr[0x30 + i] = bop33[i];  /* INT 33h stub */
    *(volatile WORD *)(0x33 * 4)     = 0x0030;              /* IVT[0x33].offset    */
    *(volatile WORD *)(0x33 * 4 + 2) = DOS_HDLR_SEG;        /* IVT[0x33].segment   */
    for (i = 0; i < sizeof(bop08); ++i) hdlr[0x34 + i] = bop08[i];  /* INT 08h stub */
    *(volatile WORD *)(0x08 * 4)     = 0x0034;              /* IVT[0x08].offset    */
    *(volatile WORD *)(0x08 * 4 + 2) = DOS_HDLR_SEG;        /* IVT[0x08].segment   */
    for (i = 0; i < sizeof(bop1c); ++i) hdlr[0x3A + i] = bop1c[i];  /* INT 1Ch iret */
    *(volatile WORD *)(0x1C * 4)     = 0x003A;              /* IVT[0x1C].offset    */
    *(volatile WORD *)(0x1C * 4 + 2) = DOS_HDLR_SEG;        /* IVT[0x1C].segment   */
    for (i = 0; i < sizeof(bop09); ++i) hdlr[0x4C + i] = bop09[i];  /* INT 09h default iret (0x4C-0x4F) */
    /* DEFAULT DEVICE-IRQ HANDLERS. A real BIOS points the unused hardware vectors at a
       handler that just acknowledges and returns; we had them pointing at whatever junk was
       in the IVT, which on this box read F000:A390 -- unowned ROM. That was harmless only so
       long as we could not deliver a device IRQ asynchronously. Now that we can, injecting an
       IRQ the guest has not hooked jumps it into that junk and hangs it: measured, Skyroads
       (which never installs a Sound Blaster ISR at all) froze at F000:A390 the moment its DMA
       block completed. So give IRQ2-7 and IRQ8-15 a plain IRET, exactly as INT 09h has. */
    hdlr[DOS_IRET_STUB_OFF] = 0xCF;                                /* shared IRET stub    */
    hdlr[DOS_CASEMAP_OFF]   = 0xCB;                                /* AH=38h case map: RETF */
    { int k; for (k = 0; k < DOS_SDA_LEN; ++k) hdlr[DOS_SDA_OFF + k] = 0; }  /* AH=34h/5D06h */
    for (i = 0x0A; i <= 0x0F; ++i) {
        *(volatile WORD *)(i * 4)     = DOS_IRET_STUB_OFF;
        *(volatile WORD *)(i * 4 + 2) = DOS_HDLR_SEG;
    }
    for (i = 0x70; i <= 0x77; ++i) {
        *(volatile WORD *)(i * 4)     = DOS_IRET_STUB_OFF;
        *(volatile WORD *)(i * 4 + 2) = DOS_HDLR_SEG;
    }
    *(volatile WORD *)(0x09 * 4)     = 0x004C;              /* IVT[0x09].offset    */
    *(volatile WORD *)(0x09 * 4 + 2) = DOS_HDLR_SEG;        /* IVT[0x09].segment   */
    for (i = 0; i < sizeof(bop1a); ++i) hdlr[0x3C + i] = bop1a[i];  /* INT 1Ah stub */
    *(volatile WORD *)(0x1A * 4)     = 0x003C;              /* IVT[0x1A].offset    */
    *(volatile WORD *)(0x1A * 4 + 2) = DOS_HDLR_SEG;        /* IVT[0x1A].segment   */
    for (i = 0; i < sizeof(bop2f); ++i) hdlr[0x40 + i] = bop2f[i];  /* INT 2Fh stub */
    *(volatile WORD *)(0x2F * 4)     = 0x0040;              /* IVT[0x2F].offset    */
    *(volatile WORD *)(0x2F * 4 + 2) = DOS_HDLR_SEG;        /* IVT[0x2F].segment   */
    for (i = 0; i < sizeof(bopxms); ++i) hdlr[XMS_ENTRY_OFF + i] = bopxms[i];  /* XMS far-call entry */
    for (i = 0; i < sizeof(bop67); ++i) hdlr[0x48 + i] = bop67[i];  /* INT 67h (EMM) stub */
    *(volatile WORD *)(0x67 * 4)     = 0x0048;              /* IVT[0x67].offset    */
    *(volatile WORD *)(0x67 * 4 + 2) = DOS_HDLR_SEG;        /* IVT[0x67].segment   */
    /* DPMI mode-switch entry (far-called): BOP 0x50 ; RETF. The host services the
       BOP by switching to PM; the RETF only executes if the switch fails. */
    hdlr[DPMI_ENTRY_OFF + 0] = VDM_BOP0; hdlr[DPMI_ENTRY_OFF + 1] = VDM_BOP1;
    hdlr[DPMI_ENTRY_OFF + 2] = DPMI_BOP; hdlr[DPMI_ENTRY_OFF + 3] = 0xCB; /* RETF */
    /* DPMI 0301 real-mode-call return catcher: BOP 0x54 (no IRET/RETF -- the 0301
       handler detects it and returns to PM, it never resumes past it). */
    hdlr[DPMI_RMRET_OFF + 0] = VDM_BOP0; hdlr[DPMI_RMRET_OFF + 1] = VDM_BOP1;
    hdlr[DPMI_RMRET_OFF + 2] = DPMI_RMRET_BOP;
    /* DPMI 0303 real-mode callback entries (one per slot) + the PM-return catcher. */
    { int s; for (s = 0; s < DPMI_CB_SLOTS; ++s) {
        hdlr[DPMI_CB_BASE_OFF + s*4 + 0] = VDM_BOP0;
        hdlr[DPMI_CB_BASE_OFF + s*4 + 1] = VDM_BOP1;
        hdlr[DPMI_CB_BASE_OFF + s*4 + 2] = DPMI_CB_BOP;
    } }
    hdlr[DPMI_PMRET_OFF + 0] = VDM_BOP0; hdlr[DPMI_PMRET_OFF + 1] = VDM_BOP1;
    hdlr[DPMI_PMRET_OFF + 2] = DPMI_PMRET_BOP;
    /* 0306 raw mode-switch entries. Both are bare BOPs: the host completes the switch
       by rewriting the CONTEXT, so control never resumes past the BOP and no RETF/IRET
       tail is wanted (the same shape as DPMI_RMRET_OFF). The protected-to-real entry
       lives in this segment too and is reached through a code selector based here --
       see the 0306 handler. */
    hdlr[DPMI_RAW2PM_OFF + 0] = VDM_BOP0; hdlr[DPMI_RAW2PM_OFF + 1] = VDM_BOP1;
    hdlr[DPMI_RAW2PM_OFF + 2] = DPMI_RAW2PM_BOP;
    hdlr[DPMI_RAW2RM_OFF + 0] = VDM_BOP0; hdlr[DPMI_RAW2RM_OFF + 1] = VDM_BOP1;
    hdlr[DPMI_RAW2RM_OFF + 2] = DPMI_RAW2RM_BOP;
    /* 0305 save/restore: a register-preserving no-op (see the define). */
    hdlr[DPMI_SSR_OFF] = 0xCB;                               /* RETF */
    /* (GH #18 run 67: the PM-fault handler BOP is planted at the handler CODE selector's
       DPMI_FAULT_COFF by dpmi_install_fault_trampoline(), not here.) */
    /* EMS detection method 2: programs read the INT 67h vector's segment:000Ah for
       the device-driver name "EMMXXXX0". Park it in the handler segment. */
    for (i = 0; i < sizeof(emmname); ++i) hdlr[DOS_EMM_NAME_OFF + i] = emmname[i];

    { unsigned bi;
      volatile BYTE *bs = (volatile BYTE *)(DOS_CTAB_SEG << 4);
      for (bi = 0; bi < sizeof(bios_ints)/sizeof(bios_ints[0]); ++bi) {
          unsigned off = DOS_BIOS_STUBS + bi * 4;
          bs[off + 0] = VDM_BOP0; bs[off + 1] = VDM_BOP1;
          bs[off + 2] = bios_ints[bi][1]; bs[off + 3] = 0xCF;   /* IRET */
          *(volatile WORD *)(bios_ints[bi][0] * 4)     = (WORD)off;
          *(volatile WORD *)(bios_ints[bi][0] * 4 + 2) = DOS_CTAB_SEG;
      } }

    /* GH #27 -- THE NULL-VECTOR LANDMINE. A vector left at 0000:0000 sends a guest
       that INTs it to 0000:0000, where it executes the interrupt vector table
       itself as code. Point any such vector at the shared IRET stub.
       MEASURED BEFORE FIXING, and the measurement narrowed the fix: on the
       bare-metal rig most unclaimed vectors are NOT null -- they carry the VDM's
       own BIOS entries (INT 13h read F000:5595, INT 11h F000:F84D). Planting over
       those would swap a working handler for a bare IRET, i.e. a silent
       "success", which is the very failure mode this issue exists to remove. So
       fill only the genuinely null ones, and name them in the log. */
    { int v, n = 0, start = -1;
      p = zput(p, "STAGE0: null IVT vectors -> IRET stub:");
      for (v = 0; v <= 256; ++v) {                  /* 256 flushes a trailing run */
          int isnull = (v < 256) && (*(volatile DWORD *)(v * 4) == 0);
          if (isnull) {
              *(volatile WORD *)(v * 4)     = DOS_IRET_STUB_OFF;
              *(volatile WORD *)(v * 4 + 2) = DOS_HDLR_SEG;
              if (start < 0) start = v;
              ++n;
          } else if (start >= 0) {                  /* emit as ranges, not 133 items */
              p = zput(p, " 0x"); p = zhexb(p, (unsigned)start);
              if (v - 1 > start) { p = zput(p, "-0x"); p = zhexb(p, (unsigned)(v - 1)); }
              start = -1;
          }
      }
      if (!n) p = zput(p, " none");
      p = zput(p, "\r\n"); }

    dos_psp_build(NULL, DOS_PSP_SEG, DOS_ENV_SEG, DOS_MEM_TOP);
    dos_env_build(NULL, DOS_ENV_SEG, progpath[0] ? progpath : "C:\\PROGRAM.COM");  /* M2.5: env */
    dos_cmdtail_build(NULL, DOS_PSP_SEG, args);                                    /* M2.5: args */
    /* ► DUMP THE TAIL AS THE GUEST WILL SEE IT. Passing ANY argument makes DOS/4GW
         quit before printing a single character, with a DPMI/INT 21h trace identical
         to a working run for all 617 of its lines -- so the branch it takes is on
         MEMORY, and this is the memory. Length byte, the bytes, and the terminator. */
    { volatile BYTE *pspb = (volatile BYTE *)((DWORD)DOS_PSP_SEG << 4);
      unsigned ti;
      p = zput(p, "STAGE2: cmdtail len=0x"); p = zhexb(p, pspb[0x80]);
      p = zput(p, " [");
      for (ti = 0; ti < 16; ++ti) { p = zhexb(p, pspb[0x81 + ti]); p = zput(p, " "); }
      p = zput(p, "]\r\n"); }
    dos_int21_init(&m, dos_mcb_init(NULL));
    /* GH #38: plant the AH=65h character tables in the DOS-resident block. */
    { volatile BYTE *ct = (volatile BYTE *)(DOS_CTAB_SEG << 4); unsigned k;
      for (k = 0; k < sizeof(dos_tab_upper);   ++k) ct[DOS_CTAB_UPPER   + k] = dos_tab_upper[k];
      for (k = 0; k < sizeof(dos_tab_fnupper); ++k) ct[DOS_CTAB_FNUPPER + k] = dos_tab_fnupper[k];
      for (k = 0; k < sizeof(dos_tab_fnterm);  ++k) ct[DOS_CTAB_FNTERM  + k] = dos_tab_fnterm[k];
      for (k = 0; k < sizeof(dos_tab_collate); ++k) ct[DOS_CTAB_COLLATE + k] = dos_tab_collate[k];
      for (k = 0; k < sizeof(dos_tab_dbcs);    ++k) ct[DOS_CTAB_DBCS    + k] = dos_tab_dbcs[k]; }
    /* GH #35: plant SysVars for INT 21h AH=52h. Only the word at BX-2 (the first
       MCB segment) is real; the remaining fields are deliberately left zero --
       see the handler for why a null stub beats a plausible-looking one. */
    { int k; for (k = -2; k < 0x40; ++k) hdlr[DOS_SYSVARS_OFF + k] = 0; }
    *(volatile WORD *)((DOS_HDLR_SEG << 4) + DOS_SYSVARS_OFF - 2) = m.first_mcb;
    m.sysvars_seg = DOS_HDLR_SEG;
    m.sysvars_off = DOS_SYSVARS_OFF;
    xms_init(&g_xms, 16384, xms_host_alloc, xms_host_free, NULL);  /* M4: 16MB XMS pool */
    ems_init(&g_ems, (uint16_t)(g_ems_frame_lin >> 4), EMS_POOL_PAGES,
             (volatile BYTE *)g_ems_frame_lin,
             ems_host_alloc, ems_host_free, NULL);             /* M4: 8MB EMS pool   */

    /* Stand up the device bus (NULL base => absolute V86 addresses) with the PIT
       (ports 0x40-0x43, INT 08h/1Ah) and the video VDD (B8000 + INT 10h text +
       cell renderer). The present sink is DirectDraw via present_ddraw on the UI
       thread. I/O on claimed ports reflects as event 0 -> the bus; INT 10h comes
       in as a BOP routed below; DOS console output is routed via m.conout. */
    InitializeCriticalSection(&g_lock);
    QueryPerformanceFrequency(&g_qpf);      /* seeds qpc_us for the lock instrument */
    host_key_typematic_init();              /* typematic from XP's setting, not a guess */
    vdd_bus_init(&g_bus, NULL);
    vdd_bus_set_sinks(&g_bus, host_irq_sink, NULL, NULL, NULL);  /* host presents directly */
    g_pic_dev = vdd_pic_device(&g_pic);      /* before the PIT: it gates every IRQ */

    vdd_bus_add(&g_bus, &g_pic_dev);
    g_pit_dev = vdd_pit_device(&g_pit);
    vdd_bus_add(&g_bus, &g_pit_dev);
    g_vid.vmem = (uint8_t *)VID_APERTURE_BASE;  /* the mapped A0000 aperture (RAM) */
    /* (per-plane backing is taken later, once the preamble is on disk -- every
       log_write() before that point TRUNCATES the file and would eat its report.) */
    g_vid.time_us = host_time_us;               /* real CRT timebase for 0x3DA (#55) */
    g_vid_dev = vdd_video_device(&g_vid);
    vdd_bus_add(&g_bus, &g_vid_dev);
    /* AFTER the video VDD is on the bus (it needs st->bus to resolve a guest address). */
    vdd_video_install_fonts(&g_vid);            /* real glyph data behind INT 10h 1130h */
    /* The BIOS keyboard buffer belongs to the guest: point the VDD at 0040:0000 BEFORE the
       bus resets it, so the ring pointers it initialises land in guest memory where a DOS
       program reading 0040:001A can see them. V86 low memory is mapped in our address
       space, so the BDA is addressable directly. */
    g_in.bda = (uint8_t *)0x400;
    g_in_dev = vdd_input_device(&g_in);
    vdd_bus_add(&g_bus, &g_in_dev);             /* keyboard: claims INT 16h      */
    g_spk.pit = &g_pit;                         /* speaker tone <- PIT channel 2 */
    g_spk_dev = vdd_speaker_device(&g_spk);
    vdd_bus_add(&g_bus, &g_spk_dev);            /* PC speaker: claims port 0x61  */
    g_dma_dev = vdd_dma_device(&g_dma);
    vdd_bus_add(&g_bus, &g_dma_dev);            /* 8237 DMA: 0x00-0x0F/80-8F/C0-DF */
    g_opl.ext_clock = 1;                        /* exec loop pumps real elapsed us */
    g_opl_dev = vdd_opl_device(&g_opl);
    vdd_bus_add(&g_bus, &g_opl_dev);            /* AdLib/OPL2: ports 0x388/0x389 */
    g_sb.dma = &g_dma; g_sb.opl = &g_opl;       /* SB pulls PCM via DMA, mirrors FM */
    g_sb.base = SB_DEFAULT_BASE; g_sb.irq = SB_DEFAULT_IRQ;
    /* Opt-in raw PCM capture -- see sb_state.cap_buf. 4 MB is ~3 minutes of Doom's
       11025 Hz stereo, and it is a static buffer so the audio thread never allocates. */
    if (GetFileAttributesA(SBDUMP_FLAG) != INVALID_FILE_ATTRIBUTES) {
        static BYTE s_sbcap[4u * 1024u * 1024u];
        g_sb.cap_buf = s_sbcap; g_sb.cap_cap = sizeof s_sbcap; g_sb.cap_len = 0;
    }
    g_sb_dev = vdd_sb_device(&g_sb);
    vdd_bus_add(&g_bus, &g_sb_dev);             /* Sound Blaster 16: 0x220-0x22F  */
    g_mpu.sink = host_midi_sink;
    g_mpu_dev = vdd_mpu_device(&g_mpu);
    vdd_bus_add(&g_bus, &g_mpu_dev);            /* MPU-401 MIDI: 0x330/0x331      */
    /* ► SAY WHETHER EVERY DEVICE ACTUALLY GOT ON THE BUS. VDD_MAX_PORTS was 16 and
         exactly full; adding one range pushed the LAST device added -- the MPU-401 --
         off, its claim returned -1, nobody looked, and the guest's MIDI port read 0xFF
         like an empty slot. Doom reset it four times, got nothing, and played no music.
         A device that cannot get on the bus is not a detail to discover by diffing
         port traces against a working run. */
    /* (the bus health line is emitted after the preamble is written -- see below;
       every log_write() before then TRUNCATES the file.) */
    /* Start the mixer + audio thread. This is also the TRANSPORT: it is what
       walks the SB's DMA buffer and raises the block-completion IRQ, so it must
       run even if no sound device opens (audio_wave falls back to silent
       pumping) -- otherwise every SB game hangs on a machine without audio. */
    vdd_audio_init(&g_audio, &g_opl, &g_sb, AUDIO_OUT_HZ);
    /* ── THE AUDIO LEAD, AS A CONTROLLED VARIABLE (awbufs.txt). ──────────────────────
         Each queued waveOut buffer is ~11.6 ms that our DMA read pointer runs ahead of
         what is audible, and the guest must refill a block before we reach it. Doom's
         longest PM stretch with no host turn measured 62.8 ms against a 70 ms lead, so
         the lead is a suspect for the residual ECHO -- the capture is 46% identical to
         one ring lap (185.8 ms) earlier, against ~22% at every neighbouring lag.
         Setting this and reading back `sb replay:` in STAGE2 is the experiment: if
         replays move with the lead it is the race, if they do not, DMX is failing to
         refill for another reason and the lead is the wrong suspect. Absent = 6. */
    { HANDLE h = CreateFileA(AWBUFS_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             NULL, OPEN_EXISTING, 0, NULL);
      if (h != INVALID_HANDLE_VALUE) {
          char c[16]; DWORD rd = 0, v = 0; int i;
          ReadFile(h, c, sizeof c, &rd, NULL);
          CloseHandle(h);
          for (i = 0; i < (int)rd; ++i) {
              if (c[i] < '0' || c[i] > '9') break;
              v = v * 10 + (DWORD)(c[i] - '0');
          }
          g_wave.nbufs = v;                   /* audio_wave_start clamps to [2,AW_BUFFERS] */
      } }
    /* ── AND THE GRANULARITY, AS A SEPARATE CONTROLLED VARIABLE (awframes.txt). ──────
         nframes x nbufs is the LEAD; nframes alone is the STEP the guest's DMA read
         pointer moves in. They are different suspects and must be varied independently
         or a result cannot be attributed to either. `awbufs=2` already showed why this
         matters: it cut the lead, starved the transport, and the replay rate "improved"
         only because the non-flat block count collapsed 13x.
         To hold the lead constant while quartering the step: awframes=128, awbufs=24. */
    { HANDLE h = CreateFileA(AWFRAMES_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             NULL, OPEN_EXISTING, 0, NULL);
      if (h != INVALID_HANDLE_VALUE) {
          char c[16]; DWORD rd = 0, v = 0; int i;
          ReadFile(h, c, sizeof c, &rd, NULL);
          CloseHandle(h);
          for (i = 0; i < (int)rd; ++i) {
              if (c[i] < '0' || c[i] > '9') break;
              v = v * 10 + (DWORD)(c[i] - '0');
          }
          g_wave.nframes = v;                 /* clamped to [AW_MIN_FRAMES,AW_FRAMES] */
      } }
    { HANDLE hp2 = CreateFileA(PITPACE_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, 0, NULL);
      if (hp2 != INVALID_HANDLE_VALUE) {
          char c[8]; DWORD rd = 0;
          ReadFile(hp2, c, sizeof c, &rd, NULL); CloseHandle(hp2);
          if (rd && c[0] >= '0' && c[0] <= '9') g_pitpace_ms = c[0] - '0';
          g_pitpace_on = (g_pitpace_ms != 0);
      } }
    if (g_pitpace_on) {
        HMODULE mm = LoadLibraryA("winmm.dll");
        if (mm) { PFN_timeBeginPeriod tbp =
                      (PFN_timeBeginPeriod)GetProcAddress(mm, "timeBeginPeriod");
                  if (tbp) tbp(1); }
        g_pitpace_thread = CreateThread(NULL, 0, pit_pacer_thread, NULL, 0, NULL);
    }
    audio_wave_start(&g_wave, AUDIO_OUT_HZ, host_audio_fill, NULL);
    m.conout = host_conout; m.conctx = NULL;    /* DOS console out -> video      */
    m.conin  = host_conin;  m.cinctx = NULL;    /* DOS console in  <- keyboard   */
    m.coninnb = host_coninnb;                   /* AH=06 DL=FF non-blocking read */
    m.conpeek = host_conpeek;                   /* AH=0B/06 non-blocking status  */

    /* Hide the inherited console (CSRSS already bound the VDM); the Luna window
       is now the display. Then start the UI thread that owns it. */
    g_key_event = CreateEventA(NULL, FALSE, FALSE, NULL);   /* auto-reset        */
    { HWND con = GetConsoleWindow(); if (con) ShowWindow(con, SW_HIDE); }
    /* ► PER-PLANE BACKING BEFORE THE UI THREAD EXISTS. The remap unmaps the A0000
         window for an instant, and the renderer dereferences it every few milliseconds;
         doing this with that thread already running hung the host so early that no log
         reached disk at all. Its report is buffered and flushed after the preamble. */
    if (GetFileAttributesA(NOREMAP_FLAG) == INVALID_FILE_ATTRIBUTES && modey_remap_init()) {
        g_vid.ymap_ctx    = NULL;
        g_vid.ymap_select = modey_remap_select;
        g_vid.ymap_plane  = modey_remap_plane;
        g_vid.ymap_wmode  = modey_remap_wmode;
        g_vid.ymap_readmap = modey_remap_readmap;
    }
    ui = CreateThread(NULL, 0, ui_thread, NULL, 0, NULL);
    /* Headless: arm the deadline watchdog so a run that blocks on input (a "press any
       key" prompt, a game menu) still self-terminates instead of wedging the harness. */
    if (g_headless) { HANDLE hd = CreateThread(NULL, 0, headless_deadline_thread, NULL, 0, NULL);
                      if (hd) CloseHandle(hd);
                      /* appended to the preamble, which is flushed further down */
                      p = zput(p, "HEADLESS: cap=0x"); p = zhex(p, g_headless_ms);
                      p = zput(p, " ms\r\n"); }
    if (g_headless) { HANDLE hb = CreateThread(NULL, 0, heartbeat_thread, NULL, 0, NULL);
                      if (hb) CloseHandle(hb); }
    if (g_qi_keys) { HANDLE hk = CreateThread(NULL, 0, synthkey_thread, NULL, 0, NULL);
                     if (hk) CloseHandle(hk); }
    if (g_qi_raise) { HANDLE hq = CreateThread(NULL, 0, qirq_probe_thread, NULL, 0, NULL);
                      if (hq) CloseHandle(hq); }

    v86_set_entry(tib, img.cs, img.ip, img.ss, img.sp, DOS_PSP_SEG);
    /* ENTRY TRAMPOLINE: `STI` then a far jump to the program's real entry point.
       Under VME the CPU sets EFLAGS.VIF only when the guest EXECUTES sti -- and the
       kernel's whole notion of "this guest can take an interrupt" is VIF. Session 10
       correctly observed that DOS programs never issue sti (they are entered with
       interrupts already on) and fixed it by handing them IF=1 in the CONTEXT, but that
       sets the REAL IF, which the kernel does not consult, while VIF stays 0 forever.
       Setting VIF directly in the CONTEXT does not survive either -- the kernel sanitises
       it. Making the guest execute one real sti costs 6 bytes and gets VIF set the only
       way the CPU will accept, after which the hardware maintains it across the guest's
       own cli/sti/iret. Faithful, too: DOS's EXEC really does return into the program
       with interrupts enabled.
       RESULT: this does NOT unblock delivery -- and note qirq.com already executed its own
       sti before spinning, so the "guest never sets VIF" hypothesis was in truth already
       refuted by the earlier runs. Kept as an opt-in knob (it is the faithful entry
       sequence regardless) but it is not the missing piece. */
    if (g_qi_vif) {
        volatile BYTE *tr = (volatile BYTE *)(ULONG_PTR)(((DWORD)DOS_HDLR_SEG << 4) + 0x60);
        tr[0] = 0xFB;                                   /* sti                       */
        tr[1] = 0xEA;                                   /* jmp far cs:ip             */
        tr[2] = (BYTE)img.ip; tr[3] = (BYTE)(img.ip >> 8);
        tr[4] = (BYTE)img.cs; tr[5] = (BYTE)(img.cs >> 8);
        VDM_REG(tib, VTIB_CS)  = DOS_HDLR_SEG;
        VDM_REG(tib, VTIB_EIP) = 0x60;
    }
    /* Session 11: the kernel's deliverability test for a V86 frame on a VME CPU reads
       EFLAGS.VIF, not IF (VdmpCanDeliver, ntoskrnl 0x56dce0). Starting the guest with
       VIF clear makes every hardware interrupt undeliverable from the kernel's point of
       view -- it just sets VIP and defers. Opt-in until the rig confirms it. */
    if (g_qi_vif) VDM_REG(tib, VTIB_EFLAGS) |= EFLAGS_VIF_BIT;
    if (!img.is_exe)                                        /* .COM near-ret guard */
        *(volatile WORD *)(((DWORD)DOS_PSP_SEG << 4) + 0xFFFE) = 0;

    p = zput(p, img.is_exe ? "STAGE2: running .EXE (entry 0x"
                           : "STAGE2: running .COM (entry 0x");
    p = zhex(p, img.cs); p = zput(p, ":0x"); p = zhex(p, img.ip); p = zput(p, ")...\r\n");
    log_write(LOG_PATH, report, p);
    base = p;                       /* preamble is on disk; the loop appends from here */
    modey_remap_flush_report();     /* whatever the A0000 remap had to say, now it fits */
    /* ── CAN THE A0000 WINDOW BE REMAPPED? THE ONE FACT THE REAL VIDEO FIX NEEDS. ────
         Mode Y cannot be de-interleaved from a flat aperture: A0000 is one buffer, so
         a guest write lands there with no record of which plane the map mask selected,
         and six after-the-fact rules have now been measured against captured frames
         without finding a good one (see modey_flush()). The fix is to stop guessing --
         give each plane its own backing and point A0000 at the selected one on a mask
         change, with four pagefile-backed sections and MapViewOfFileEx at a fixed
         address: O(1) per change, exact, no copying.
         Whether that is possible at all turns on ONE thing: is A0000 its own
         allocation, or a slice of a larger reservation the VDM kernel made? A section
         cannot be mapped into the middle of an existing reservation, and MEM_RELEASE
         only takes a whole allocation. VirtualQuery answers it for the cost of one log
         line, and it is worth far more than another guess at a heuristic. */
    { MEMORY_BASIC_INFORMATION mbi;
      if (VirtualQuery((LPCVOID)(ULONG_PTR)0xA0000, &mbi, sizeof mbi) == sizeof mbi) {
          p = zput(p, "STAGE2: A0000 region: alloc_base=0x");
          p = zhex(p, (DWORD)(ULONG_PTR)mbi.AllocationBase);
          p = zput(p, " base=0x");   p = zhex(p, (DWORD)(ULONG_PTR)mbi.BaseAddress);
          p = zput(p, " size=0x");   p = zhex(p, (DWORD)mbi.RegionSize);
          p = zput(p, " state=0x");  p = zhex(p, mbi.State);
          p = zput(p, " type=0x");   p = zhex(p, mbi.Type);
          p = zput(p, " prot=0x");   p = zhex(p, mbi.Protect);
          p = zput(p, " allocprot=0x"); p = zhex(p, mbi.AllocationProtect);
          p = zput(p, (mbi.AllocationBase == (LPVOID)(ULONG_PTR)0xA0000)
                        ? "  -> OWN ALLOCATION: remappable\r\n"
                        : "  -> inside a larger reservation: NOT remappable in place\r\n");
          log_append(LOG_PATH, base, p); p = base;
      } }
    { char bb2[160], *bq = bb2;
      bq = zput(bq, g_bus.claim_fail ? "STAGE2: *** BUS CLAIMS REFUSED: " : "STAGE2: bus ok: ");
      bq = zhex(bq, (DWORD)g_bus.claim_fail);
      bq = zput(bq, " refused, ports="); bq = zhex(bq, (DWORD)g_bus.n_ports);
      bq = zput(bq, "/"); bq = zhex(bq, (DWORD)VDD_MAX_PORTS);
      bq = zput(bq, " mem="); bq = zhex(bq, (DWORD)g_bus.n_mem);
      bq = zput(bq, "/"); bq = zhex(bq, (DWORD)VDD_MAX_MEM);
      bq = zput(bq, " dev="); bq = zhex(bq, (DWORD)g_bus.n_dev);
      bq = zput(bq, "/"); bq = zhex(bq, (DWORD)VDD_MAX_DEV);
      bq = zput(bq, "\r\n"); log_append(LOG_PATH, bb2, bq); serial_out(bb2, bq); }

    SetCurrentDirectoryA(g_cur);    /* DOS relative paths resolve against CurDir */

    /* Service loop: run V86 until a BOP, dispatch INT 21h, step past the BOP, re-enter.
       Runs until the guest terminates, a hard stop, or the window closes (g_running);
       no iteration cap so interactive/animated programs keep going. */
    m.tib = tib; m.out = dosout; m.out_cap = sizeof(dosout); m.out_len = 0; m.out_trunc = 0;
    g_mach = &m;              /* the watchdog flushes this if the run wedges */
    (void)guard;
    { static uint32_t s_last_fault = 0; static int s_storm = 0;
    DWORD rm_start_tick = GetTickCount();   /* headless wall-clock cap origin (real-mode) */
    g_run_start_tick = rm_start_tick;       /* published for the STAGE2 vsync rate line */
    while (g_running) {
        /* Headless safety (session-9): the real-mode loop has no iteration cap so
           interactive/animated programs run free -- but under the SMB auto-exit harness
           a hung or infinite real-mode program (or a host bug) would run forever and
           wedge rt.bat's `start /wait`, and the box is not easily accessible to unwedge.
           So in headless mode bound it by wall clock: the host self-exits, rt.bat returns,
           the watcher survives. (The PM loop got this in v69; the real-mode loop needs it
           too -- a real-mode hang was the one path that could still permanently wedge the
           rig.) GetTickCount per iteration is cheap; the loop runs once per event/BOP. */
        if (g_headless && GetTickCount() - rm_start_tick > PM_HEADLESS_MS) {
            p = zput(p, "STAGE2: headless time cap (");
            p = zhex(p, PM_HEADLESS_MS); p = zput(p, " ms) reached -> exiting"
                     " (long/hung real-mode run; screenshots captured if graphical)\r\n");
            log_append(LOG_PATH, base, p); serial_out(base, p); p = base;
            break;
        }
        /* Pump the PIT tick from THIS thread's wall clock. Normally the UI thread
           raises IRQ0, but a heavy I/O-trap loop (e.g. Skyroads' OPL/timer delay poll
           that faults on every IN 388h) starves the UI thread, so the guest's timer
           would crawl (~100x too slow) and any tick-paced delay appears to hang. Set
           the pending flag at the ~18.2 Hz BIOS rate from here; it coalesces with the
           UI thread's flag (both just set it to 1) so there's no double-count. */
        /* Advance the PIT from the real clock every iteration. This is also what generates
           IRQ0 now, at whatever rate the GUEST programmed into channel 0 -- the old fixed
           55 ms pump hard-wired 18.2 Hz, so a game that reprograms the timer for its music
           (as this one does) had its sequencer clocked far too slowly no matter what. */
        host_pit_sync();
        opl_pump_time();            /* keep the OPL timers current for the guest */
        host_key_typematic();       /* the keyboard repeats even when the UI stalls */
        /* Deliver a pending PIT IRQ0 as INT 08h when the guest's main-line
           interrupts are enabled. We regain control at event boundaries, almost
           always inside a BOP stub (CS == DOS_HDLR_SEG) where the LIVE IF is the
           handler's (cleared by the CD nn that vectored in) -- the guest's real
           IF is the FLAGS the stub will IRET to, at SS:SP+4. Outside a stub
           (e.g. an I/O fault from main-line) the live EFLAGS IF applies. Skip if
           we're inside our own INT 08h stub, to avoid timer re-entrancy. */
        if (g_irq0_pending) {
            DWORD cs = VDM_REG(tib, VTIB_CS) & 0xFFFF, ip = VDM_REG(tib, VTIB_EIP) & 0xFFFF;
            DWORD fl;
            if (cs == DOS_HDLR_SEG) {
                DWORD ss = VDM_REG(tib, VTIB_SS) & 0xFFFF, sp = VDM_REG(tib, VTIB_ESP) & 0xFFFF;
                fl = peekw((ss << 4) + ((sp + 4) & 0xFFFF));   /* main-line FLAGS the stub returns to */
            } else {
                fl = VDM_REG(tib, VTIB_EFLAGS);
            }
            if (if_or_vif(fl) && vdd_pic_can_deliver(&g_pic, 0)
                && !(cs == DOS_HDLR_SEG && ip >= 0x34 && ip < 0x3A)) {
                InterlockedDecrement(&g_irq0_pending);
                vdd_pic_acknowledge(&g_pic, 0);
                vdd_pic_eoi(&g_pic, 0);         /* see async_inject_irq: timer is auto-EOI */
                g_irq0_inj++;
                inject_int(tib, 0x08);
            } else {
                static int s_bud_skip = 6;
                g_irq0_skip++;                  /* IF=0 or inside our own INT 08h */
                if (cs == DOS_HDLR_SEG && ip >= 0x34 && ip < 0x3A) g_irq0_skip_stub++;
                else if (!if_or_vif(fl))                           g_irq0_skip_if++;
                vdmstate_sample("irq0-skip", tib, &s_bud_skip);
            }
        }
        /* Deliver a pending keyboard IRQ1 as INT 09h, same IF-gating as IRQ0. The
           scancode is already queued for port 0x60; INT 09h vectors through IVT[9]
           to the game's own handler (or our IRET stub if it hasn't hooked one).
           Excludes re-entry into our INT 08h (0x34) and INT 09h (0x4C) stubs. */
        if (g_irq1_pending > 0) {
            DWORD cs = VDM_REG(tib, VTIB_CS) & 0xFFFF, ip = VDM_REG(tib, VTIB_EIP) & 0xFFFF;
            DWORD fl;
            if (cs == DOS_HDLR_SEG) {
                DWORD ss = VDM_REG(tib, VTIB_SS) & 0xFFFF, sp = VDM_REG(tib, VTIB_ESP) & 0xFFFF;
                fl = peekw((ss << 4) + ((sp + 4) & 0xFFFF));
            } else {
                fl = VDM_REG(tib, VTIB_EFLAGS);
            }
            if (if_or_vif(fl) && !(cs == DOS_HDLR_SEG &&
                                  ((ip >= 0x34 && ip < 0x3A) || (ip >= 0x4C && ip < 0x50)))) {
                InterlockedDecrement(&g_irq1_pending);   /* one INT 09h per queued scancode byte */
                vdd_pic_acknowledge(&g_pic, 1);
                if (async_vec_is_our_stub(1)) vdd_pic_eoi(&g_pic, 1);
                g_irq1_inj++;
                inject_int(tib, 0x09);
            }
        }
        /* Device IRQs (SB block completion on 5, etc.): same IF gating as IRQ0/1,
           vectored as INT 8+irq. Skip while inside our own timer/keyboard stubs. */
        { int q;
          DWORD qcs = VDM_REG(tib, VTIB_CS) & 0xFFFF, qip = VDM_REG(tib, VTIB_EIP) & 0xFFFF;
          /* Only the BOP itself is off-limits (we would re-enter mid-service);
             once past it the guest is running a handler with IF set, and a real
             PC delivers a device IRQ there quite happily. */
          int in_bop = (qcs == DOS_HDLR_SEG &&
                        ((qip >= 0x34 && qip < 0x37) || (qip >= 0x4C && qip < 0x4F)));
          if (!in_bop && guest_if_enabled(tib)) {
              for (q = 2; q < 8; ++q) {
                  if (peekw((8 + q) * 4 + 2) == DOS_HDLR_SEG
                      && peekw((8 + q) * 4) == DOS_IRET_STUB_OFF) {
                      InterlockedExchange(&g_irqn_pending[q], 0);   /* unhooked: drop it */
                      continue;
                  }
                  if (!vdd_pic_can_deliver(&g_pic, (uint8_t)q)) continue;
                  if (InterlockedExchange(&g_irqn_pending[q], 0)) {
                      vdd_pic_acknowledge(&g_pic, (uint8_t)q);
                      if (async_vec_is_our_stub((unsigned)q)) vdd_pic_eoi(&g_pic, (uint8_t)q);
                      g_irqn_inj++;
                      inject_int(tib, (unsigned)(8 + q));
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
              int pend = 0;
              for (q = 2; q < 8; ++q) if (g_irqn_pending[q]) { pend = q; break; }
              if (pend) g_irqn_refuse_total++;
              if (pend && g_irqn_refuse_log < 16) {
                  char rb[256], *rq = rb;
                  DWORD ss = VDM_REG(tib, VTIB_SS) & 0xFFFF, sp = VDM_REG(tib, VTIB_ESP) & 0xFFFF;
                  g_irqn_refuse_log++;
                  rq = zput(rq, "IRQN-REFUSE irq=0x");  rq = zhex(rq, (DWORD)pend);
                  rq = zput(rq, " cs:ip=0x");           rq = zhex(rq, qcs);
                  rq = zput(rq, ":0x");                 rq = zhex(rq, qip);
                  rq = zput(rq, " efl=0x");             rq = zhex(rq, VDM_REG(tib, VTIB_EFLAGS));
                  rq = zput(rq, " ss:sp=0x");           rq = zhex(rq, ss);
                  rq = zput(rq, ":0x");                 rq = zhex(rq, sp);
                  rq = zput(rq, " stkflags=0x");
                  rq = zhex(rq, peekw((ss << 4) + ((sp + 4) & 0xFFFF)));
                  rq = zput(rq, in_bop ? " why=in_bop" : " why=if_gate");
                  rq = zput(rq, "\r\n");
                  log_append(LOG_PATH, rb, rq); serial_out(rb, rq);
              }
          } }
        /* Mirror the guest's IF into EFLAGS.VIF before handing the context back. On VME
           hardware the kernel's deliverability test reads VIF, and VIF is lost every time
           we synthesise an interrupt frame ourselves -- so a guest that has interrupts
           enabled still looks disabled to the kernel, which then just sets VIP and defers.
           With VIP set and VIF clear the guest's next IRET faults into a dispatch that
           refuses to deliver and re-arms VIP: a livelock, measured on the rig as the guest
           frozen on the IRET at DOS_HDLR_SEG:0x0003. Keeping the two flags in step is what
           lets the kernel dispatch instead of deferring. */
        /* NOTE, measured: do NOT touch bit 9 (0x200) of FIXED_NTVDMSTATE. It is the VDM's
           virtual interrupt flag and the KERNEL already maintains it -- it read 0x...3230
           (bit set) from the first instruction. Mirroring our own IF into it only clobbered
           correct state (the word went 0x3230 -> 0x3030) and changed nothing else. */
        if (g_qi_vif) {
            if (VDM_REG(tib, VTIB_EFLAGS) & 0x200) VDM_REG(tib, VTIB_EFLAGS) |= EFLAGS_VIF_BIT;
            else                                   VDM_REG(tib, VTIB_EFLAGS) &= ~EFLAGS_VIF_BIT;
        }
        /* ---- MODE 12h: THE HOST IS THE CPU (GH #55) ------------------------- *
         * While a planar mode is current we do not hand the guest to V86 at all,
         * because on real hardware there is no way to see its A0000 writes there:
         * the page trap that would show them freezes the VDM (see video_trap_sync).
         * Run a slice in the interpreter instead -- its A0000 accesses go through
         * the planar write engine -- then loop, which re-runs the IRQ delivery gate
         * above so timer and keyboard interrupts reach the guest between slices.
         * A slice ends early the moment an IRQ is pending, so the slice size is a
         * ceiling on lock-hold time, not on responsiveness.
         * If the interpreter declines the instruction we are ON (a BOP, or an
         * opcode it does not model), ran == 0 and we fall through to V86 exactly as
         * before -- so a DOS call still reaches the kernel as a BOP event, and an
         * unmodeled opcode still executes on the real CPU. */
        if (g_p12_interp && !g_dpmi_pm) {
            long ran = host_interp(tib, P12_SLICE);
            if (ran > 0) { g_p12_batches++; g_p12_instrs += (DWORD)ran; continue; }
            /* NAME THE OPCODE. Every bail is guest execution we cannot see, so the
               list of declined opcodes IS the to-do list for this path (#27). */
            { static int s_bud_p12 = 12;
              if (s_bud_p12 > 0) {
                DWORD c3 = VDM_REG(tib, VTIB_CS) & 0xFFFF, i3 = VDM_REG(tib, VTIB_EIP) & 0xFFFF;
                const volatile BYTE *ip3 = (const volatile BYTE *)((c3 << 4) + i3);
                unsigned k5;
                --s_bud_p12;
                p = zput(p, "P12-BAIL at 0x"); p = zhex(p, c3);
                p = zput(p, ":0x"); p = zhex(p, i3); p = zput(p, " bytes:");
                for (k5 = 0; k5 < 8; ++k5) { p = zput(p, " "); p = zhexb(p, ip3[k5]); }
                p = zput(p, "\r\n");
                log_append(LOG_PATH, base, p); p = base;
              } }
            g_p12_bails++;
        }
        InterlockedExchange(&g_in_exec, 1);
        ev = v86_run(tib, &st);
        InterlockedExchange(&g_in_exec, 0);
        /* SPIKE: once in protected mode, stop at the FIRST PM event and dump the raw
           taxonomy (event/info/selectors) -- this is how the spike learns how the
           monitor reflects a PM INT 31h / fault. Later increments replace this with
           a real INT 31h dispatch. */
        if (g_dpmi_pm) {
            DWORD csv = VDM_REG(tib, VTIB_CS) & 0xFFFF, ipv = VDM_REG(tib, VTIB_EIP) & 0xFFFF;
            p = zput(p, "STAGE3-DPMI: PM stop event=0x"); p = zhex(p, ev);
            p = zput(p, " status=0x"); p = zhex(p, (unsigned)st);
            p = zput(p, " info=0x"); p = zhex(p, VDM_REG(tib, VTIB_EVENT_INFO));
            p = zput(p, " CS:IP=0x"); p = zhex(p, csv); p = zput(p, ":0x"); p = zhex(p, ipv);
            p = zput(p, " EFL=0x"); p = zhex(p, VDM_REG(tib, VTIB_EFLAGS));
            p = zput(p, " SS:SP=0x"); p = zhex(p, VDM_REG(tib, VTIB_SS) & 0xFFFF);
            p = zput(p, ":0x"); p = zhex(p, VDM_REG(tib, VTIB_ESP) & 0xFFFF);
            p = zput(p, "\r\n  VTIB[5A8..]: "); p = zdump(p, (const void *)(tib + 0x5A8), 0x20);
            p = zput(p, "\r\n");
            log_append(LOG_PATH, base, p); serial_out(base, p); p = base;
            break;
        }
        /* I/O port trap (event 0; VM-confirmed) or a generic GP fault (event 2):
           if the faulting instruction is an IN/OUT we can decode, service it via
           the VDD bus and resume; otherwise fall through to the stop dump. */
        if (ev == VDM_EVENT_IO || ev == VDM_EVENT_IO_HW ||
            ev == VDM_EVENT_GPFAULT || ev == VDM_EVENT_IO_STRING) {
            int handled;
            { static int s_bud_io = 6; vdmstate_sample("io-reflect", tib, &s_bud_io); }
            /* REP INS/OUTS arrives as its own event with CS:IP ON the instruction. */
            if (ev == VDM_EVENT_IO_STRING) {
                HOST_LOCK();
                handled = host_try_io_string(tib, &g_bus);
                HOST_UNLOCK();
                if (handled) { g_ev_iostr++; continue; }
            }
            /* Trap-storm detection over ALL faults (port + A0000 memory): the
               per-pixel VGA loop faults repeatedly in a tight PC window. Once a
               storm is established in mode 12h, escalate to the batching
               interpreter so the whole inner loop (OUTs + pixel writes) runs in
               one shot; otherwise emulate the single faulting access. */
            DWORD fcs = VDM_REG(tib, VTIB_CS) & 0xFFFF, fip = VDM_REG(tib, VTIB_EIP) & 0xFFFF;
            uint32_t cur = (fcs << 4) + fip;
            uint32_t d = (cur > s_last_fault) ? (cur - s_last_fault) : (s_last_fault - cur);
            s_storm = (d <= STORM_WINDOW) ? (s_storm + 1) : 0;
            s_last_fault = cur;
            if ((g_a000_prot || (g_interp12 && vdd_video_planar_active(&g_vid)))
                && s_storm >= STORM_GATE) {
                DWORD bc = VDM_REG(tib, VTIB_CS) & 0xFFFF, bi = VDM_REG(tib, VTIB_EIP) & 0xFFFF;
                long ran = host_interp(tib, TIER1_CAP);
                if (ran > 0) {
                    static int s_bud_bat = 10;
                    if (s_bud_bat > 0) {            /* is the batch ADVANCING the guest? */
                        --s_bud_bat;
                        p = zput(p, "BATCH ran="); p = zhex(p, (DWORD)ran);
                        p = zput(p, " from 0x"); p = zhex(p, bc); p = zput(p, ":0x"); p = zhex(p, bi);
                        p = zput(p, " to 0x"); p = zhex(p, VDM_REG(tib, VTIB_CS) & 0xFFFF);
                        p = zput(p, ":0x"); p = zhex(p, VDM_REG(tib, VTIB_EIP) & 0xFFFF);
                        p = zput(p, "\r\n");
                        log_append(LOG_PATH, base, p); p = base;
                    }
                    continue;                       /* batched the hot loop            */
                }
            }
            HOST_LOCK();
            handled = host_try_io(tib, &g_bus);     /* single port op (no logging)     */
            HOST_UNLOCK();
            if (handled) { g_ev_io++; io_hot_note(g_io_last_port, VDM_REG(tib, VTIB_CS) & 0xFFFF, VDM_REG(tib, VTIB_EIP) & 0xFFFF); continue; }
            /* real-HW event 3 reports CS:IP AFTER the faulting IN/OUT -> retro-decode the
               I/O instruction ending at CS:IP and service it (Skyroads' vblank IN AL,DX). */
            if (ev == VDM_EVENT_IO_HW) {
                HOST_LOCK();
                handled = host_try_io_retro(tib, &g_bus);
                HOST_UNLOCK();
                if (handled) { g_ev_io++; io_hot_note(g_io_last_port, VDM_REG(tib, VTIB_CS) & 0xFFFF, VDM_REG(tib, VTIB_EIP) & 0xFFFF); continue; }
            }
            if ((g_a000_prot || (g_interp12 && vdd_video_planar_active(&g_vid)))
                && host_interp(tib, 1) > 0) continue;   /* single A0000 access */
            /* The interpreter refused the very first opcode. With A0000 trapped that
               is a LIVELOCK, not a miss: we resume at the same EIP, the guest
               re-faults on the same store, forever. Name the opcode -- this is the
               "mode-12h MOV-store decoder gap" from the M3 notes, and it is why
               mode 12h has never rendered. Budgeted so it cannot flood the log. */
            if (g_a000_prot || g_interp12) {
                static int s_bud_dec = 8;
                if (s_bud_dec > 0) {
                    DWORD c2 = VDM_REG(tib, VTIB_CS) & 0xFFFF, i2 = VDM_REG(tib, VTIB_EIP) & 0xFFFF;
                    const volatile BYTE *ip2 = (const volatile BYTE *)((c2 << 4) + i2);
                    unsigned k4;
                    --s_bud_dec;
                    p = zput(p, "INTERP-REFUSED at 0x"); p = zhex(p, c2);
                    p = zput(p, ":0x"); p = zhex(p, i2); p = zput(p, " bytes:");
                    for (k4 = 0; k4 < 8; ++k4) { p = zput(p, " "); p = zhexb(p, ip2[k4]); }
                    p = zput(p, "\r\n");
                    log_append(LOG_PATH, base, p); p = base;
                }
                g_interp_refused++;
            }
            /* Not an I/O instruction and not an A0000 touch. On this hardware event 3
               is OVERLOADED: besides the I/O reflect, it is how the kernel says "a
               hardware interrupt is pending and the VDM has interrupts enabled" -- the
               interrupt assist we thought we lacked. It only ever fires now that the
               guest runs with IF=1; with IF=0 the kernel had nothing to notify us about
               and simply left VDM_INT_TIMER pending forever. Distinguish it from a
               genuine GP fault by the kernel's own pending bits in FIXED_NTVDMSTATE:
               clear them (so it stops re-notifying), latch the timer IRQ, and resume at
               the SAME EIP -- no instruction faulted, so nothing must be stepped over.
               Delivery itself is left to the loop-top gate, which owns the IF and
               re-entrancy checks. (Session 9 met this same event in protected mode and
               cleared the bits there; see dpmi_enter.S label 2.) */
            if (ev == VDM_EVENT_IO_HW) {
                volatile DWORD *vdmstate = (volatile DWORD *)(ULONG_PTR)0x714;
                DWORD pend = *vdmstate & 3u;
                if (pend) {
                    if (pend & 2u) irq0_latch();         /* VDM_INT_TIMER -> IRQ0 */
                    *vdmstate &= ~3u;
                    g_ev_intpend++;
                    continue;
                }
            }
        }
        if (ev != VDM_EVENT_BOP) {
            DWORD csv = VDM_REG(tib, VTIB_CS) & 0xFFFF;
            DWORD ipv = VDM_REG(tib, VTIB_EIP) & 0xFFFF;
            volatile BYTE *cp = (volatile BYTE *)((csv << 4) + ipv);
            BYTE ib[8], pb[8]; unsigned k;
            for (k = 0; k < 8; ++k) ib[k] = cp[k];
            for (k = 0; k < 8; ++k) pb[k] = (ipv >= 8) ? cp[(int)k - 8] : 0;  /* 8 bytes BEFORE CS:IP */
            p = zput(p, "STAGE2: stop event=0x"); p = zhex(p, ev);
            p = zput(p, " status=0x"); p = zhex(p, (unsigned)st);
            p = zput(p, " info=0x"); p = zhex(p, VDM_REG(tib, VTIB_EVENT_INFO));
            p = zput(p, " CS:IP=0x"); p = zhex(p, csv);
            p = zput(p, ":0x"); p = zhex(p, ipv); p = zput(p, "\r\n");
            p = zput(p, "  bytes[CS:IP-8]: "); p = zdump(p, pb, 8);
            p = zput(p, "  bytes@CS:IP: "); p = zdump(p, ib, 8);
            p = zput(p, "  VTIB[5A8..]: "); p = zdump(p, (const void *)(tib + 0x5A8), 0x20);
            log_append(LOG_PATH, base, p); p = base;
            break;
        }
        { static int s_bud_bop = 6; vdmstate_sample("bop", tib, &s_bud_bop); }
        /* Route the BOP by its number: 0x10 -> INT 10h, 0x16 -> INT 16h (both via
           the bus), else INT 21h. */
        if ((VDM_REG(tib, VTIB_EVENT_INFO) & 0xFF) == 0x10) {
            ntvdd_regs r; regs_load(&r, tib);
            HOST_LOCK();
            vdd_bus_deliver_int(&g_bus, 0x10, &r);
            HOST_UNLOCK();
            regs_store(&r, tib);
            video_trap_sync();     /* mode 12h: interpret the guest (GH #55) */
            VDM_REG(tib, VTIB_EIP) += 3;
            continue;
        }
        if ((VDM_REG(tib, VTIB_EVENT_INFO) & 0xFF) == 0x16) {
            ntvdd_regs r; uint8_t ah16; regs_load(&r, tib); ah16 = r_ah(&r);
            HOST_LOCK();
            vdd_bus_deliver_int(&g_bus, 0x16, &r);
            HOST_UNLOCK();
            /* A blocking BIOS read with no key must NOT park the exec thread -- doing that
               stops the guest dead, so its timer, its music and its screen freeze until a key
               arrives. (Same fault as INT 21h AH=01/07/08, fixed the same way.) Leave EIP on
               the BOP instead: the guest re-executes INT 16h and keeps taking timer
               interrupts while it waits, which is what a real BIOS spin does. */
            if ((ah16 == 0x00 || ah16 == 0x10) && r.zf != 0 && g_running) continue;
            regs_store(&r, tib);
            host_set_flags(tib, r.cf, r.zf);
            VDM_REG(tib, VTIB_EIP) += 3;
            continue;
        }
        if ((VDM_REG(tib, VTIB_EVENT_INFO) & 0xFF) == 0x33) {   /* INT 33h mouse  */
            mouse_int33(tib);
            VDM_REG(tib, VTIB_EIP) += 3;
            continue;
        }
        if ((VDM_REG(tib, VTIB_EVENT_INFO) & 0xFF) == 0x09) {   /* INT 09h: BIOS keyboard */
            HOST_LOCK();
            vdd_input_bios_consume(&g_in);      /* take the byte, re-arm if more queued */
            HOST_UNLOCK();
            VDM_REG(tib, VTIB_EIP) += 3;        /* -> the IRET */
            continue;
        }
        if ((VDM_REG(tib, VTIB_EVENT_INFO) & 0xFF) == 0x08) {   /* INT 08h timer tick */
            ntvdd_regs r; regs_load(&r, tib);
            HOST_LOCK();
            vdd_bus_deliver_int(&g_bus, 0x08, &r);  /* bump BIOS tick at 0040:006C */
            /* The real BIOS timer ISR ends with `mov al,20h; out 20h,al`. Ours is a BOP
               with nowhere to put one, so issue the EOI here -- without it the PIC's
               in-service bit for IRQ0 latches on the first tick and the timer stops dead
               (measured: exactly one tick delivered in a 30 s run). */
            vdd_pic_eoi(&g_pic, 0);
            HOST_UNLOCK();
            regs_store(&r, tib);
            VDM_REG(tib, VTIB_EIP) += 3;            /* -> CD 1C (chain user timer) */
            continue;
        }
        {   /* ---- BIOS services: INT 11h/12h/13h/14h/15h/17h/25h/26h ---------
               GH #43/#44/#45. Every one of these was previously an IRET that
               returned whatever the caller already had in its registers.
               NOTE ON EVIDENCE: the 6.22 oracle is NOT truth here -- QEMU runs
               SeaBIOS, so a BIOS answer from it is another reimplementation's
               opinion (epic #24). The equipment word and memory size are
               statements about OUR virtual machine's configuration, which is
               ours to declare; the rest report "not present" honestly and log,
               rather than pretending hardware exists. */
            unsigned bn = VDM_REG(tib, VTIB_EVENT_INFO) & 0xFF;
            int handled = 1;
            WORD *pflg = (WORD *)(((VDM_REG(tib, VTIB_SS) & 0xFFFF) << 4)
                                  + (((VDM_REG(tib, VTIB_ESP) & 0xFFFF) + 4) & 0xFFFF));
            #define BCF_SET() (*pflg |= 1)
            #define BCF_CLR() (*pflg &= (WORD)~1)
            #define BSETAX(v) (VDM_REG(tib, VTIB_EAX) = \
                (VDM_REG(tib, VTIB_EAX) & 0xFFFF0000u) | ((DWORD)(v) & 0xFFFF))
            if (bn == 0x11) {
                /* Equipment word. Our machine: one floppy, 80x25 colour video,
                   one parallel port, one serial port, no coprocessor.
                   bit0 floppy present, bits4-5 video (10b = 80x25 colour),
                   bits6-7 floppy count-1, bits9-11 serial, bits14-15 parallel. */
                BSETAX(0x4021);
                BCF_CLR();
            } else if (bn == 0x12) {
                BSETAX(640);                       /* KB of conventional memory */
                BCF_CLR();
            } else if (bn == 0x15) {
                unsigned ah15 = (VDM_REG(tib, VTIB_EAX) >> 8) & 0xFF;
                if (ah15 == 0x88) {                /* extended memory, KB */
                    BSETAX(0x3C00);                /* 15 MB, matching the XMS pool */
                    BCF_CLR();
                } else if (ah15 == 0x86) {         /* wait CX:DX microseconds */
                    BCF_CLR();                     /* the PIT already paces us */
                } else if (ah15 == 0xC0) {         /* get system config table */
                    BSETAX(0x8600); BCF_SET();     /* not provided -> unsupported */
                    g_bios_unimpl[0x15] = 1;
                } else {
                    /* ► LOG BEFORE BSETAX, NOT AFTER. The first cut printed AX *after*
                         this arm had already overwritten AH with 0x86, so the log said
                         "ax=0x86de" and only AL was the guest's -- the instrument
                         reporting its own write back as the guest's request. */
                    { char xb[96], *xq = xb;
                      xq = zput(xq, "  INT15 UNIMPL ax=0x");
                      xq = zhex(xq, VDM_REG(tib, VTIB_EAX) & 0xFFFF);
                      xq = zput(xq, " bx=0x"); xq = zhex(xq, VDM_REG(tib, VTIB_EBX) & 0xFFFF);
                      xq = zput(xq, " cx=0x"); xq = zhex(xq, VDM_REG(tib, VTIB_ECX) & 0xFFFF);
                      xq = zput(xq, " dx=0x"); xq = zhex(xq, VDM_REG(tib, VTIB_EDX) & 0xFFFF);
                      xq = zput(xq, " es=0x"); xq = zhex(xq, VDM_REG(tib, VTIB_ES) & 0xFFFF);
                      xq = zput(xq, "\r\n"); log_append(LOG_PATH, xb, xq); serial_out(xb, xq); }
                    BSETAX((WORD)((VDM_REG(tib, VTIB_EAX) & 0xFF) | 0x8600));
                    BCF_SET();                     /* AH=86h: unsupported fn */
                    g_bios_unimpl[0x15] = 1;
                    /* ► WHICH function, because "INT15" alone does not say. This arm is
                         the only difference between a Doom run that works and one given
                         a command-line argument: with an argument the trace is identical
                         until here, DOS/4GW takes this CF=1, and exits CLEANLY in 328 ms
                         (STAGE2: complete, run_ms=0x148) without printing a character. */
                }
            } else if (bn == 0x14) {               /* serial: no port fitted */
                BSETAX(0x0000);                    /* status: not ready       */
                BCF_CLR();
                g_bios_unimpl[0x14] = 1;
            } else if (bn == 0x17) {               /* printer: none fitted    */
                BSETAX((WORD)((VDM_REG(tib, VTIB_EAX) & 0x00FF) | 0x3000));
                BCF_CLR();                         /* AH bit4=selected, bit5=out of paper */
                g_bios_unimpl[0x17] = 1;
            } else if (bn == 0x13) {               /* disk services           */
                unsigned ah13 = (VDM_REG(tib, VTIB_EAX) >> 8) & 0xFF;
                if (ah13 == 0x00) { BSETAX(0); BCF_CLR(); }      /* reset: ok  */
                else if (ah13 == 0x01) { BSETAX(0); BCF_CLR(); } /* last status*/
                else {
                    /* We expose no raw sectors. Say so -- AH=01 "bad command"
                       with CF -- rather than returning success and no data,
                       which would look to the guest like an empty disk. */
                    BSETAX(0x0100); BCF_SET();
                    g_bios_unimpl[0x13] = 1;
                }
            } else if (bn == 0x30) {               /* INT 20h: terminate       */
                /* INT 20h is AH=4Ch with an exit code of 0. Routing it here
                   rather than leaving an IRET means a program that exits this
                   way actually exits, instead of returning into itself. */
                VDM_REG(tib, VTIB_EAX) &= 0xFFFF0000u;
                m.tp = p; dos_int21(&m); p = m.tp;   /* AH=00 -> terminate       */
                handled = 2;
            } else if (bn == 0x27) {               /* TSR, CP/M style          */
                p = zput(p, "  INT27 TSR: residency NOT honoured "
                            "(single-program host)\r\n");
                g_bios_unimpl[0x27] = 1;
                VDM_REG(tib, VTIB_EAX) &= 0xFFFF0000u;
                m.tp = p; dos_int21(&m); p = m.tp;
                handled = 2;
            } else if (bn == 0x28) {               /* DOS idle                 */
                BCF_CLR();                         /* nothing to yield to      */
            } else if (bn == 0x29) {               /* fast console output      */
                /* AL is the character. Programs that hook this expect it to
                   PRINT; leaving it as an IRET swallowed the output silently. */
                vdd_video_putc(&g_vid, (uint8_t)(VDM_REG(tib, VTIB_EAX) & 0xFF));
                BCF_CLR();
            } else if (bn == 0x25 || bn == 0x26) { /* absolute disk read/write */
                BSETAX(0x0207); BCF_SET();         /* AL=07 drive param error  */
                g_bios_unimpl[bn] = 1;
            } else handled = 0;
            #undef BCF_SET
            #undef BCF_CLR
            #undef BSETAX
            if (handled == 2) break;               /* terminate: leave the loop */
            if (handled) { VDM_REG(tib, VTIB_EIP) += 3; continue; }
        }
        if ((VDM_REG(tib, VTIB_EVENT_INFO) & 0xFF) == 0x1A) {   /* INT 1Ah BIOS time */
            ntvdd_regs r; regs_load(&r, tib);
            HOST_LOCK();
            vdd_bus_deliver_int(&g_bus, 0x1A, &r);
            HOST_UNLOCK();
            regs_store(&r, tib);
            host_set_flags(tib, r.cf, r.zf);
            VDM_REG(tib, VTIB_EIP) += 3;
            continue;
        }
        if ((VDM_REG(tib, VTIB_EVENT_INFO) & 0xFF) == 0x2F) {   /* INT 2Fh multiplex */
            DWORD ax = VDM_REG(tib, VTIB_EAX) & 0xFFFF;
            p = zput(p, "STAGE2: BOP2F ax=0x"); p = zhex(p, ax); p = zput(p, "\r\n");
            log_append(LOG_PATH, base, p); serial_out(base, p); p = base;
            if (ax == 0x4300) {                                 /* XMS installation check */
                VDM_SET16(tib, VTIB_EAX, (VDM_REG(tib, VTIB_EAX) & 0xFF00) | 0x80);  /* AL=80h installed */
            } else if (ax == 0x4310) {                          /* get XMS entry -> ES:BX */
                VDM_SET16(tib, VTIB_ES, DOS_HDLR_SEG);
                VDM_SET16(tib, VTIB_EBX, XMS_ENTRY_OFF);
            } else if (ax == 0x1687) {                           /* DPMI installation check (SPIKE) */
                /* AX=0 present; BX bit0=1 (32-bit programs supported, run 81); CL=3 (386);
                   DX=0.90; SI=0 private paras; ES:DI = mode-switch entry to FAR-CALL. A 16-bit
                   client ignores BX; a 32-bit client reads bit0 to decide to far-call with AX=1. */
                VDM_SET16(tib, VTIB_EAX, 0);
                VDM_SET16(tib, VTIB_EBX, 1);
                VDM_SET16(tib, VTIB_ECX, (VDM_REG(tib, VTIB_ECX) & 0xFF00) | 0x03);
                VDM_SET16(tib, VTIB_EDX, 0x005A);               /* DPMI 0.90        */
                VDM_SET16(tib, VTIB_ESI, 0);
                VDM_SET16(tib, VTIB_ES,  DOS_HDLR_SEG);
                VDM_SET16(tib, VTIB_EDI, DPMI_ENTRY_OFF);
                p = zput(p, "STAGE2: DPMI 1687 -> AX=0 ES:DI=0x"); p = zhex(p, DOS_HDLR_SEG);
                p = zput(p, ":0x"); p = zhex(p, DPMI_ENTRY_OFF); p = zput(p, " (guest must far-call this)\r\n");
                log_append(LOG_PATH, base, p); serial_out(base, p); p = base;
            }
            /* other INT 2Fh multiplex functions: left to the guest (no-op pass) */
            VDM_REG(tib, VTIB_EIP) += 3;                        /* -> the IRET (CF) */
            continue;
        }
        if ((VDM_REG(tib, VTIB_EVENT_INFO) & 0xFF) == 0x43) {   /* XMS API far-call entry */
            p = zput(p, " XMS AH=0x"); p = zhex(p, (VDM_REG(tib, VTIB_EAX) >> 8) & 0xFF); p = zput(p, "\r\n");
            log_append(LOG_PATH, base, p); p = base;
            host_xms(tib);
            VDM_REG(tib, VTIB_EIP) += 3;                        /* -> the RETF      */
            continue;
        }
        if ((VDM_REG(tib, VTIB_EVENT_INFO) & 0xFF) == 0x67) {   /* INT 67h EMM (EMS) */
            p = zput(p, " EMS AH=0x"); p = zhex(p, (VDM_REG(tib, VTIB_EAX) >> 8) & 0xFF); p = zput(p, "\r\n");
            log_append(LOG_PATH, base, p); p = base;
            HOST_LOCK();
            host_ems(tib);
            HOST_UNLOCK();
            VDM_REG(tib, VTIB_EIP) += 3;                        /* -> the IRET      */
            continue;
        }
        if ((VDM_REG(tib, VTIB_EVENT_INFO) & 0xFF) == DPMI_BOP) {  /* DPMI real->PM switch */
            DWORD csv = VDM_REG(tib, VTIB_CS) & 0xFFFF, ipv = VDM_REG(tib, VTIB_EIP) & 0xFFFF;
            LONG reg_st = 0, set_st = 0; int sw;
            /* AX bit0 = the client's declared width (0=16-bit, 1=32-bit e.g. DOS/4GW).
               Logged and recorded, but it does NOT set the initial selectors' D/B --
               see dpmi_switch_to_pm(); doing so ran DOS/4GW's 16-bit stub as 32-bit. */
            int is32 = (int)(VDM_REG(tib, VTIB_EAX) & 1);
            p = zput(p, "STAGE3: DPMI_BOP far-call LANDED @ 0x"); p = zhex(p, csv);
            p = zput(p, ":0x"); p = zhex(p, ipv);
            p = zput(p, is32 ? " -- switching to PM (32-bit client)\r\n"
                             : " -- switching to PM (16-bit client)\r\n");
            log_append(LOG_PATH, base, p); serial_out(base, p); p = base;
            sw = dpmi_switch_to_pm(tib, is32, &reg_st, &set_st);
            p = zput(p, " [svc11=0x"); p = zhex(p, (unsigned)reg_st);
            p = zput(p, " svc10=0x"); p = zhex(p, (unsigned)set_st); p = zput(p, "]");
            p = zput(p, " retcs=0x"); p = zhex(p, g_dpmi_dbg[0]);
            p = zput(p, " clo=0x"); p = zhex(p, g_dpmi_dbg[2]);
            p = zput(p, " chi=0x"); p = zhex(p, g_dpmi_dbg[3]);
            if (sw == 0) {
                unsigned steps;
                g_dpmi_pm = 1;
                g_dpmi_code_base = g_dpmi_seg_base[0];   /* CS base = the patch-scan target */
                /* Record the switch's code/data/stack selector bases (indices 1/2/3) so
                   dpmi_sel_base() translates DS:/ES:/SS: through the right base -- essential
                   once CS!=DS!=SS (a real .EXE); for a .COM all three are equal. */
                { int si; for (si = 0; si < 3; ++si) {
                    g_ldt[1 + si].base   = g_dpmi_seg_base[si];
                    g_ldt[1 + si].limit  = 0xFFFF;
                    g_ldt[1 + si].access = (si == 0) ? 0xFA : 0xF2;
                    /* Mirror the D/B width dpmi_switch_to_pm ACTUALLY installed, so
                       dpmi_sel_is32() (I/O decode + EIP-mask gating) agrees with the live
                       descriptor. That is now always 16-bit for these three: the client's
                       post-switch code must also be valid real-mode code on the failure
                       path, so it cannot be 32-bit. A 32-bit client far-jmps to its OWN
                       INT 31h-allocated 32-bit selectors, which are reported correctly. */
                    g_ldt[1 + si].flags  = 0;
                } }
                if (g_ldt_next < 4) g_ldt_next = 4;      /* client allocs start at index 4 now */
                /* ── DPMI INITIAL CLIENT STATE: ES = PSP SELECTOR, AND THE PSP'S
                      ENVIRONMENT POINTER CONVERTED TO A SELECTOR. ────────────────────
                   dpmi_switch_to_pm() sets ES = DS (a second copy of the data selector),
                   and that is simply wrong. DPMI 0.9, "entering protected mode", on the
                   register state at a successful return:
                       CS = 16-bit selector with base of real mode CS and a 64K limit
                       SS = Selector with base of real mode SS and a 64K limit
                       DS = Selector with base of real mode DS and a 64K limit
                       ES = Selector to program's PSP with a 100h byte limit
                   and, separately: "The environment pointer in the current program's PSP
                   will automatically be converted to a descriptor."

                   THIS IS NOT A SPEC DETAIL WE ARE HONOURING FOR TIDINESS -- it is what
                   killed Doom for four sessions. DOS/4GW's PM module does:
                       mov es,[saved DS] / mov bx,es:[0x2c] / mov es,bx
                   i.e. it reads the PSP's environment field and loads it as a SELECTOR.
                   With ES pointing at the data segment instead of the PSP, +0x2c is an
                   arbitrary code byte pair -- measured as 0x8b17, LDT index 4450 -- and
                   `mov es,bx` #GPs, which XP answers by terminating the whole VDM with no
                   exception we can catch. The client is thus its own second witness for
                   BOTH halves of the rule, independently of the spec text.

                   The environment field is left holding the SELECTOR from here on. The
                   spec makes restoring it the client's job before it terminates ("it must
                   restore it to the selector created by the DPMI host"), and nothing in
                   our DOS layer reads PSP+0x2C -- dos_psp.h writes it once at load and no
                   reader exists (checked). If one is ever added, it must not assume a
                   segment after a DPMI switch. */
                { WORD psp = m.psp_seg;
                  DWORD pspbase = (DWORD)psp << 4;
                  WORD psp_sel = 0, env_sel = 0;
                  if (g_ldt_next < DPMI_LDT_MAX) {
                      int pi = g_ldt_next++;
                      g_ldt[pi].base   = pspbase;
                      g_ldt[pi].limit  = 0xFF;        /* "a 100h byte limit", exactly */
                      g_ldt[pi].access = 0xF2;        /* present, DPL3, data R/W       */
                      g_ldt[pi].flags  = 0;
                      dpmi_install(pi);
                      psp_sel = (WORD)((pi << 3) | 7);
                      VDM_SET16(tib, VTIB_ES, psp_sel);
                  }
                  { volatile WORD *envf = (volatile WORD *)(ULONG_PTR)(pspbase + 0x2C);
                    WORD envseg = *envf;
                    /* envseg == 0 is legal and documented: a client may free its
                       environment and zero this word BEFORE switching, in which case
                       there is nothing to convert and we must not invent a descriptor. */
                    if (envseg && g_ldt_next < DPMI_LDT_MAX) {
                        int ei = g_ldt_next++;
                        g_ldt[ei].base   = (DWORD)envseg << 4;
                        g_ldt[ei].limit  = 0xFF;      /* dos_env_build fills a 0x10-para block */
                        g_ldt[ei].access = 0xF2;
                        g_ldt[ei].flags  = 0;
                        dpmi_install(ei);
                        env_sel = (WORD)((ei << 3) | 7);
                        *envf = env_sel;
                    }
                  }
                  dpmi_install_default_pm_handlers(&m);
                  p = zput(p, " PSP 0x"); p = zhex(p, psp);
                  p = zput(p, " -> ES=0x"); p = zhex(p, psp_sel);
                  p = zput(p, " env -> sel 0x"); p = zhex(p, env_sel);
                }
                p = zput(p, " segbase C=0x"); p = zhex(p, g_dpmi_seg_base[0]);
                p = zput(p, " D=0x"); p = zhex(p, g_dpmi_seg_base[1]);
                p = zput(p, " S=0x"); p = zhex(p, g_dpmi_seg_base[2]);
                p = zput(p, " -> PM ok (CS=0x"); p = zhex(p, VDM_REG(tib, VTIB_CS) & 0xFFFF);
                p = zput(p, ":0x"); p = zhex(p, VDM_REG(tib, VTIB_EIP) & 0xFFFF);
                p = zput(p, ") -> DPMI PM loop\r\n");
                log_append(LOG_PATH, base, p); serial_out(base, p); p = base;
                /* run 53: the emulation path -- execute PM in the host interpreter instead of
                   the kernel (which deadlocks on a PM #GP, run 52). No BOP patch: the interpreter
                   reads the raw CD nn and stops on it, and we service through the same dispatch.
                   No kernel watchdog here either -- the interpreter has its own guard cap, and the
                   watchdog's 3s TerminateProcess would guillotine a long (millions-of-insn) run. */
                if (g_dpmi_use_interp) {
                    p = zput(p, "DPMI: run 53 -- PM in host interpreter (no kernel, no BOP patch)\r\n");
                    log_append(LOG_PATH, base, p); serial_out(base, p); p = base;
                    dpmi_run_pm_interp(&m, tib);
                    break;
                }
                /* Safety watchdog (kernel PM path only): an un-terminable spin still self-kills
                   after ~3s so the batch dumps the log. */
                { HANDLE wd = CreateThread(NULL, 0, dpmi_watchdog, NULL, 0, NULL);
                  if (wd) CloseHandle(wd);
                  /* Prove creation FROM THIS THREAD. The watchdog's own first line is
                     written by the new thread, so its absence is ambiguous -- it cannot
                     distinguish "thread never created" from "created but the process was
                     killed before it was ever scheduled". Doom's log shows neither that
                     line nor any sample, so we need the difference. */
                  p = zput(p, "STAGE3-DPMI: watchdog thread created h="); p = zhex(p, (DWORD)(ULONG_PTR)wd);
                  p = zput(p, "\r\n");
                  log_append(LOG_PATH, base, p); serial_out(base, p); p = base; }
                /* Patch the client's PM `INT nn` (CD nn) -> BOP (C4 C4), recording the original
                   vector per CS offset. Same 2 bytes, so a real unmodified client's INT 31h/21h
                   now reflect to us as BOPs.

                   Why UP-FRONT (not lazy on first fault): a raw `INT 31h` in PM raises a #GP the
                   native kernel cannot reflect to us (runs 20-34) -- that unsolved reflect is the
                   whole reason we patch, so we cannot wait for the fault to catch it. So the scan
                   must find every INT site before the client runs.

                   Hardening (run 42): scan the FULL 64K code selector, not just the first 0x2000.
                   A real program's INT sites live well past 8 KB; the old bound silently missed
                   them (client would #GP-hang on the first unpatched INT 31h). The zeroed stack/BSS
                   tail can't match CD 31/CD 21, so scanning it is harmless. g_int_vec[] doubles as
                   an original-bytes map: g_int_vec[o]!=0 => offset o was `CD g_int_vec[o]` and is
                   now `C4 C4`, so a mis-patch (a CD 31/CD 21 byte-pair that was DATA, not code) is
                   detectable and revertible. Data mis-patch stays possible (x86 isn't
                   self-synchronising; without a disassembler we can't prove a byte is code) -- the
                   map is the mitigation, and an unexpected-BOP path below logs any surprise. */
                { volatile BYTE *cs = (volatile BYTE *)(ULONG_PTR)g_dpmi_code_base;
                  DWORD o, n = 0, last = 0;
                  for (o = 0; o < 0xFFFF; ++o) {
                      if (cs[o] == 0xCD && (cs[o+1] == 0x31 || cs[o+1] == 0x21 || cs[o+1] == 0x10
                                            || cs[o+1] == 0x16 || cs[o+1] == 0x33
                                            || cs[o+1] == 0x1A || cs[o+1] == 0x08)) {
                          DWORD lin = g_dpmi_code_base + o;      /* map is linear-keyed now */
                          pmap_set(lin, cs[o+1]); cs[o] = 0xC4; cs[o+1] = 0xC4; ++n; last = o;
                      }
                  }
                  p = zput(p, "DPMI: patched "); p = zhex(p, n);
                  p = zput(p, " INT sites -> BOP (full 64K scan, last off 0x"); p = zhex(p, last);
                  p = zput(p, ")\r\n");
                  log_append(LOG_PATH, base, p); serial_out(base, p); p = base;
                }
                /* Guest breakpoints (PMBP_PATH). Loaded here rather than at WinMain entry
                   so the list is read on the run that will use it, and armed both now and
                   after every code-region patch -- an address inside a module the client
                   has not loaded yet simply arms later. */
                dpmi_bp_load();
                dpmi_bp_arm();
                if (GetFileAttributesA(PMVERBOSE_PATH) != INVALID_FILE_ATTRIBUTES)
                    g_dpmi_cp_max = 0x100000;   /* verbose: trace a whole startup */
                { HANDLE hw = CreateFileA(PMWATCH_PATH, GENERIC_READ,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                          OPEN_EXISTING, 0, NULL);
                  if (hw != INVALID_HANDLE_VALUE) {
                      char wb2[128]; DWORD wn = 0, k2 = 0;
                      ReadFile(hw, wb2, sizeof wb2 - 1, &wn, NULL); CloseHandle(hw);
                      while (k2 < wn && g_pm_nwatch < DPMI_WATCH_MAX) {
                          DWORD v = 0; int dig = 0;
                          while (k2 < wn) {
                              char c = wb2[k2];
                              int d = (c >= '0' && c <= '9') ? c - '0'
                                    : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                                    : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
                              if (d < 0) break;
                              v = (v << 4) | (DWORD)d; ++dig; ++k2;
                          }
                          if (dig) g_pm_watch[g_pm_nwatch++] = v; else ++k2;
                      }
                      p = zput(p, "DPMI: pmwatch.txt -> ");
                      { int wi; for (wi = 0; wi < g_pm_nwatch; ++wi) {
                            p = zput(p, "0x"); p = zhex(p, g_pm_watch[wi]); p = zput(p, " "); } }
                      p = zput(p, "\r\n");
                      log_append(LOG_PATH, base, p); serial_out(base, p); p = base;
                  } }
                g_dpmi_use_kernel = (GetFileAttributesA(PMKERNEL_PATH) != INVALID_FILE_ATTRIBUTES);
                if (g_dpmi_use_kernel) {
                    p = zput(p, "DPMI: pmkernel.flag -- PM will run under VdmStartExecution\r\n");
                    log_append(LOG_PATH, base, p); serial_out(base, p); p = base;
                }
                g_sb_absent = (GetFileAttributesA(NOSB_PATH) != INVALID_FILE_ATTRIBUTES);
                g_opl_absent = g_sb_absent;      /* one knob, both devices unfitted */
                if (g_sb_absent) {
                    p = zput(p, "SB: nosb.flag -- DSP reset will NOT answer; no Sound Blaster fitted\r\n");
                    log_append(LOG_PATH, base, p); serial_out(base, p); p = base;
                }
                g_pm_veh_pass = (GetFileAttributesA(PMVEHPASS_PATH) != INVALID_FILE_ATTRIBUTES);
                if (g_pm_veh_pass) {
                    p = zput(p, "DPMI: pmvehpass.flag -- non-INT PM faults will NOT be swallowed by the VEH\r\n");
                    log_append(LOG_PATH, base, p); serial_out(base, p); p = base;
                }
                g_pm_noirq = (GetFileAttributesA(PMNOIRQ_PATH) != INVALID_FILE_ATTRIBUTES);
                if (g_pm_noirq) {
                    p = zput(p, "DPMI: pmnoirq.flag present -- IRQ0->PM injection SUPPRESSED\r\n");
                    log_append(LOG_PATH, base, p); serial_out(base, p); p = base;
                }
                /* GH #18 (run 67): install the PM-fault reflect machinery, so a RAW (non-BOP)
                   PM #GP -- an SS-retype, HLT, or privileged op the INT->BOP scan cannot
                   pre-patch -- is reflected by the kernel to our handler (code sel : BOP) on a
                   scratch stack, instead of silently terminating the VDM (runs 20-34). */
                dpmi_install_fault_trampoline();
                p = zput(p, "DPMI: PM-fault reflect stkSel=0x"); p = zhex(p, g_dpmi_fault_sel);
                p = zput(p, " codeSel=0x"); p = zhex(p, g_dpmi_flt_code_sel);
                p = zput(p, " bop@code:0x"); p = zhex(p, DPMI_FAULT_COFF);
                p = zput(p, " tbl@0x"); p = zhex(p, (DWORD)(ULONG_PTR)g_flt_tbl);
                p = zput(p, " class="); p = zhex(p, DPMI_FLT_CLASS_GP);
                p = zput(p, "\r\n");
                log_append(LOG_PATH, base, p); serial_out(base, p); p = base;

                /* --- DPMI protected-mode execution loop -----------------------------------
                   dpmi_enter_pm runs the client in PM until it stops. Two stop kinds:
                   (1) a patched INT nn BOP -- the kernel reflects C4 C4 as VTIB_EVENT=4
                       (run 32); we look up the original vector by fault EIP and dispatch.
                   (2) GH #18: a raw PM #GP the kernel reflects to our handler code selector
                       -- also VTIB_EVENT=4, but CS==g_dpmi_flt_code_sel and EIP==DPMI_FAULT_COFF.
                       We recover the saved faulting CS:EIP/SS:ESP from the VTIB_FLT_SAV* slots. */
                DWORD ev3_retries = 0;   /* GH#18: bounded event-3 (pending-int guard) re-entries */
                DWORD g_pmfault_dumps = 0;              /* rate-limit the PM-fault byte dump (anti-flood) */
                DWORD pm_start_tick = GetTickCount();   /* headless wall-clock cap origin */
                for (steps = 0; g_running && steps < 100000000; ++steps) {  /* run until window close (animation) */
                    DWORD ev, eip, csv, vec; int rc;
                    /* Headless safety (session-9): an infinite visual demo (pm32irq/animate)
                       never calls INT 21h 4Ch, so under the SMB auto-exit harness the PM loop
                       would run forever and wedge rt.bat's `start /wait`. Bound it by wall clock
                       so the host self-exits and the watcher survives. Sampled sparsely (every
                       4096 steps) to keep GetTickCount off the hot path. Interactive runs (no
                       marker) are unbounded, as before -- the user closes the window. */
                    if (g_headless && (steps & 0xFFF) == 0 && steps
                        && GetTickCount() - pm_start_tick > PM_HEADLESS_MS) {
                        p = zput(p, "STAGE3-DPMI: headless time cap (");
                        p = zhex(p, PM_HEADLESS_MS); p = zput(p, " ms) reached after 0x");
                        p = zhex(p, (unsigned)steps);
                        p = zput(p, " steps -> exiting (infinite/visual demo; watch it on the monitor)\r\n");
                        log_append(LOG_PATH, base, p); serial_out(base, p); p = base;
                        break;
                    }
                    /* run 52 heartbeat: publish where we're about to hand off + bump the
                       iteration counter BEFORE entering, so a watchdog sample taken while
                       we're blocked inside dpmi_enter_pm sees a FROZEN iter at this CS:EIP. */
                    g_dpmi_enter_cs  = VDM_REG(tib, VTIB_CS)  & 0xFFFF;
                    g_dpmi_enter_eip = dpmi_pm_eip(tib);
                    g_dpmi_iter      = (LONG)(steps + 1);
                    /* run 66 diagnostic (kept): log FIXED_NTVDMSTATE [0x714] once. Run 66 proved
                       bit3=0 already (classifier is NOT the blocker), so no forcing needed -- for
                       a #GP KiTrap0D pushes class 6, which reaches the generic reflect body. */
                    if (steps == 0) {
                        DWORD st714 = *(volatile DWORD *)(ULONG_PTR)0x714;
                        p = zput(p, "GH#18: [0x714]=0x"); p = zhex(p, st714);
                        p = zput(p, " bit3="); p = zhex(p, (st714 >> 3) & 1);
                        p = zput(p, " bit4="); p = zhex(p, (st714 >> 4) & 1);
                        p = zput(p, " bit14="); p = zhex(p, (st714 >> 14) & 1);
                        p = zput(p, " tib8=0x"); p = zhex(p, *(volatile DWORD *)(tib + DPMI_TIB_FLTTBL));
                        p = zput(p, "\r\n");
                        log_append(LOG_PATH, base, p); serial_out(base, p); p = base;
                    }
                    /* Timing in PM (polled): the host PIT (UI thread) raises IRQ0 at the 8254
                       rate; here we consume it and run the BIOS tick handler so 0040:006C and
                       INT 1Ah advance with wall-clock even though nothing injects INT 08h into
                       the PM client yet. (Async IRQ0 delivery to a client's PM INT 08h hook is
                       the remaining timing piece.) */
                    opl_pump_time();
                    if (InterlockedExchange(&g_irq0_pending, 0)) {
                        ntvdd_regs tr; regs_load(&tr, tib);
                        HOST_LOCK();
                        vdd_bus_deliver_int(&g_bus, 0x08, &tr);   /* pit_int08 -> ++0040:006C */
                        HOST_UNLOCK();
                        g_pm_irq0_latch = 1;    /* #2b: latch a virtual IRQ0 for the PM hook */
                    }
                    /* #2b async IRQ0 injection: when the client has hooked INT 08h in PM
                       (g_pm_int[8], via INT 31h 0205) and its virtual-IF is enabled, deliver
                       the latched IRQ0 to that handler -- how timer-hooking games get ticks.
                       The latch persists across CLI windows so a masked interrupt isn't lost. */
                    /* ► AND NOT WHILE AN ASYNC ONE IS STILL IN FLIGHT. The two injectors
                         guarded themselves but not each other: the async path can vector the
                         guest into its ISR and return, and if that ISR then leaves PM for a
                         DOS call, control arrives back HERE with the handler still live --
                         and a second tick would re-enter it on top of itself. Measured: the
                         run died on such an injection taken at obj1+0x153dc, i.e. inside the
                         very delay loop the tick exists to release. */
                    if (g_pm_irq0_latch && g_dpmi_vi && g_pm_int[0x08].client && !g_in_pm_irq
                        && !g_pm_noirq && !g_async_pm_active
                        && (GetTickCount() - g_pm_vec8_armed_ms) >= DPMI_IRQ0_ARM_QUIET_MS) {
                        uint32_t pre8 = g_dma.rd_count[1];
                        g_pm_irq0_latch = 0;
                        g_in_pm_irq = 1;
                        if (g_pm_tick_owed > 0 && dpmi_inject_pm_irq(&m, tib, 0x08, steps))
                            InterlockedDecrement(&g_pm_tick_owed);
                        g_in_pm_irq = 0;
                        g_coop_dma_polls += g_dma.rd_count[1] - pre8;
                    }
                    /* ── AND THE KEYBOARD, WHICH HAD NO COOPERATIVE PATH AT ALL. ─────────
                         IRQ0 has had one since #2b; IRQ1 had only the asynchronous
                         injector, and that gets ONE attempt per keystroke: the 8042 model
                         raises on the FIFO's empty->full edge and again as the guest
                         drains it, so if the one raise lands while the CPU thread is
                         inside the host rather than the guest, the attempt bails
                         (why=20, "not executing guest code") and nothing ever retries.
                         Measured, with a scripted key script and every gate open:
                             KEYIRQ raise gate=1 ok=0 pm=1 pmhook=1 in_exec=0 why=0x14
                         exactly once in a whole run, twelve scancodes pushed, p60=0 --
                         the client never read the keyboard port because it was never told
                         there was anything to read.
                         A pending interrupt is not a moment, it is a STATE: the 8259 holds
                         the request until it can be delivered. So hold it here too and
                         offer it at every pass round the loop, exactly as the timer latch
                         does. The guest reaches this point constantly (every INT 31h,
                         every trapped port access), so the latency is microseconds. */
                    /* ► AND ASK THE 8042, NOT ONLY THE COUNTER. On real hardware the
                         keyboard's request is a STATE -- OBF stays set until the byte is
                         read -- so a byte in the FIFO with no interrupt outstanding is a
                         condition that cannot arise. Deriving the arm from the FIFO as
                         well as the latch makes any future counter slip self-correcting
                         instead of silently swallowing a keystroke. */
                    if ((g_irq1_pending > 0 || vdd_input_sc_pending(&g_in))
                        && g_pm_int[0x09].client && !g_pm_noirq) {
                        if (g_irq1_pending <= 0) InterlockedIncrement(&g_irq1_pending);
                        int vi   = g_dpmi_vi;
                        int busy = g_in_pm_irq || g_async_pm_active;
                        int pic  = vdd_pic_can_deliver(&g_pic, 1);
                        int app  = dpmi_sel_is32((WORD)(VDM_REG(tib, VTIB_CS) & 0xFFFF));
                        int done1 = 0;
                        if (vi && !busy && pic) {
                            /* ── CLAIM THE PENDING INTERRUPT BEFORE RUNNING THE HANDLER,
                                 NOT AFTER. The client's ISR reads port 0x60 while we are
                                 inside this call, and the 8042 model RE-ASSERTS the line
                                 from in there whenever a byte is still queued. Decrementing
                                 afterwards therefore cancels the interrupt the ISR itself
                                 just raised, and the remaining byte sits in the FIFO with
                                 nothing left to announce it. Measured, with a scripted
                                 E0-50 (down arrow): three bytes delivered, `scleft=1`, and
                                 not one further raise for the rest of the run -- the
                                 keyboard simply stopped after the first arrow's prefix.
                                 Claim first, hand it back if the injection did not run. */
                            InterlockedDecrement(&g_irq1_pending);
                            g_in_pm_irq = 1;
                            done1 = dpmi_inject_pm_irq(&m, tib, 0x09, steps);
                            g_in_pm_irq = 0;
                            if (!done1) InterlockedIncrement(&g_irq1_pending);
                        }
                        /* ► WHY A HELD-BACK KEY IS HELD BACK. Five separate conditions
                             stand between a raised IRQ1 and the client's ISR, and a key
                             that never arrives looks the same whichever one said no.
                             Bounded, and only while something IS pending, so a run with
                             no keyboard activity pays nothing. */
                        if (g_keypm_logged < 64
                            && (!done1 || g_keypm_logged < 8)) {
                            char kb2[176], *kq = kb2;
                            ++g_keypm_logged;
                            kq = zput(kq, "KEYPM pend=");  kq = zhex(kq, (DWORD)g_irq1_pending);
                            kq = zput(kq, " vi=");         kq = zhex(kq, (DWORD)vi);
                            kq = zput(kq, " busy=");       kq = zhex(kq, (DWORD)busy);
                            kq = zput(kq, " pic=");        kq = zhex(kq, (DWORD)pic);
                            kq = zput(kq, " app32=");      kq = zhex(kq, (DWORD)app);
                            kq = zput(kq, " done=");       kq = zhex(kq, (DWORD)done1);
                            kq = zput(kq, " cs=0x");       kq = zhex(kq, VDM_REG(tib, VTIB_CS) & 0xFFFF);
                            kq = zput(kq, " scleft=");     kq = zhex(kq, (DWORD)vdd_input_sc_pending(&g_in));
                            kq = zput(kq, "\r\n"); log_append(LOG_PATH, kb2, kq); serial_out(kb2, kq);
                        }
                    }
                    /* ── AND THE DEVICE LINES, WHICH HAD NO COOPERATIVE PATH EITHER. ─────
                         This is the KEYBOARD BUG ABOVE, one line number over, and it is
                         the PCM click. A device IRQ gets exactly ONE delivery attempt --
                         the synchronous async_inject_irq() inside host_irq_sink(), made
                         from the AUDIO thread at the instant of the raise. If the CPU
                         thread happens to be inside the host rather than in guest code
                         that attempt bails at why=20 and NOTHING RETRIES: the V86 exec
                         loop's drain (the `for (q = 2; q < 8; ...)` above) needs a `tib`
                         from a trapping guest, and a 32-bit DPMI client never goes there.
                         MEASURED on a 45 s Doom run, and it is not marginal:
                             sb_blocks   0x0de6 = 3558 block completions raised
                             irq05 (PM)  0x0a37 = 2615 delivered
                             ASYNC-EARLY 1009 bails, EVERY ONE why=0x14 (g_in_exec == 0)
                         A dropped SB completion is not a dropped tick. Each one owns a
                         distinct 256-byte refill: no IRQ means DMX never rewrites that
                         block, so the 8237 laps the ring and we play the PREVIOUS lap's
                         audio verbatim. Proven in the capture -- seams 4096 bytes apart
                         share their preceding bytes exactly, and 96 of 182 seams are
                         preceded by a full 256-byte repeat. At 86 blocks/s that is the
                         buzz. A timer tick can be coalesced; this cannot.
                         So hold the request and offer it every pass, exactly as the
                         timer latch and the keyboard now do. The guest reaches this point
                         constantly (every INT 31h, every trapped port access), so the
                         added latency is microseconds and no new thread is involved. */
                    if (g_dpmi_vi && !g_pm_noirq && !g_in_pm_irq && !g_async_pm_active) {
                        int q;
                        for (q = 2; q < 8; ++q) {
                            if (!g_irqn_pending[q]) continue;
                            /* No PM handler: the client cannot want it. Drop it rather
                               than spin on it forever. */
                            if (!g_pm_int[0x08u + q].client) {
                                InterlockedExchange(&g_irqn_pending[q], 0);
                                ++g_pm_devirq_drop;
                                continue;
                            }
                            if (!vdd_pic_can_deliver(&g_pic, (uint8_t)q)) continue;
                            /* CLAIM BEFORE RUNNING, HAND BACK ON FAILURE -- the ISR runs
                               inside the call below and the device model re-raises from in
                               there, so clearing afterwards would cancel the interrupt the
                               handler itself just asked for. That is verbatim the mistake
                               the keyboard made. */
                            /* ► AND THE SAME BRACKET ON THE DEVICE LINES, BECAUSE THE
                                 TIMER MAY NOT BE THE HANDLER THAT POLLS AT ALL. DMX
                                 reads the 8237 count 55 times a second; that was matched
                                 against IRQ0's rate first only because IRQ0 was what the
                                 previous session was looking at. IRQ5 -- the block
                                 completion, which is when a refill is actually DUE -- is
                                 the more natural place for a driver to look, and nothing
                                 has excluded it. Same snapshot, same exactness. */
                            uint32_t pred = g_dma.rd_count[1];
                            InterlockedExchange(&g_irqn_pending[q], 0);
                            g_in_pm_irq = 1;
                            if (dpmi_inject_pm_irq(&m, tib, 0x08u + q, steps)) {
                                /* Same acknowledge/EOI rule the async path already uses for
                                   these lines (and which the 2615 delivered ones prove out):
                                   set in-service, and EOI ourselves only when the vector is
                                   still our own stub, because then no guest ISR will. */
                                vdd_pic_acknowledge(&g_pic, (uint8_t)q);
                                if (async_vec_is_our_stub((unsigned)q))
                                    vdd_pic_eoi(&g_pic, (uint8_t)q);
                                ++g_pm_devirq_inj;
                            } else {
                                InterlockedExchange(&g_irqn_pending[q], 1);
                                ++g_pm_devirq_fail;
                            }
                            g_in_pm_irq = 0;
                            g_coop_dma_polls_dev[q & 7] += g_dma.rd_count[1] - pred;
                            break;                  /* one per pass: let it IRET first */
                        }
                    }
                    /* ── BRACKET THE FIRST ENTRIES (session 16, Doom) ────────────────────
                       Doom's log ends at the steps==0 [0x714] dump above and the process is
                       gone, with no fault, no banner and no INT 21h. Between that line and
                       the next thing that logs there are FOUR things that can kill us, and
                       nothing said which: arming the trampoline, entering PM, the guest's
                       first instruction, or the return path. So checkpoint each side for the
                       first few iterations -- cheap, self-limiting, and it turns "died
                       somewhere in here" into a named step. Bounded to 4096 so a healthy client
                       (millions of iterations) pays nothing. */
                    if (steps < g_dpmi_cp_max) {
                        /* Dump the descriptor and the actual BYTES we are about to run. Doom
                           dies inside the FIRST dpmi_enter_pm and never returns, so the only
                           thing that can tell a bad mode switch from a specific offending
                           instruction is knowing which instruction it was. Cheap: 4 iterations. */
                        DWORD cbase = dpmi_sel_base((WORD)g_dpmi_enter_cs);
                        p = zput(p, "DPMI-CP["); p = zhex(p, (unsigned)steps);
                        p = zput(p, "] pre-arm cs:eip=0x"); p = zhex(p, g_dpmi_enter_cs);
                        p = zput(p, ":0x"); p = zhex(p, g_dpmi_enter_eip);
                        p = zput(p, " csbase=0x"); p = zhex(p, cbase);
                        p = zput(p, " ss:esp=0x"); p = zhex(p, VDM_REG(tib, VTIB_SS) & 0xFFFF);
                        p = zput(p, ":0x"); p = zhex(p, VDM_REG(tib, VTIB_ESP));
                        p = zput(p, " bytes@cs:eip=");
                        /* GUARD THE INSTRUMENT -- with host_readable(), NOT IsBadReadPtr.
                           csbase+eip need not be readable from the host's flat address
                           space, and an unguarded read would fault in our own diagnostic
                           and destroy the evidence we came for. IsBadReadPtr looks like
                           the guard for that but IS the same bug wearing a coat: it faults
                           on purpose, and dpmi_crash_veh sees the fault first. */
                        { const BYTE *ib = (const BYTE *)(ULONG_PTR)(cbase + g_dpmi_enter_eip);
                          if (!host_readable(ib, 16)) p = zput(p, "<unreadable from host>");
                          else                        p = zdump(p, ib, 16); }
                        p = zput(p, "\r\n");
                        log_append(LOG_PATH, base, p); serial_out(base, p); p = base;
                        /* ── WHERE DOES CONTROL GO NEXT? ────────────────────────────────
                           Session 17: Doom now clears every service it asks for and STILL
                           stops at the same instruction (0x0F:0x6644, a `xchg ax,cx / cbw /
                           retn` tail). The bytes at CS:EIP no longer explain anything --
                           they are three harmless instructions -- so the interesting address
                           is the one the RETN goes to, and the interesting state is what the
                           client is carrying into it. The old checkpoint could show neither.
                           Dump the register file and the top of the guest stack: the first
                           stack word IS the return address for the pending RETN, and that
                           turns "died somewhere after here" into a named next basic block.
                           ► THE STACK ADDRESS MUST FOLLOW THE SS D/B BIT. With a 16-bit
                             stack selector the CPU uses SP and leaves the top half of ESP
                             holding whatever junk was there -- the first run of this dump
                             read ESP=0xb3371474 against a base of 0x1100 and probed kernel
                             space. That is not a corrupt guest; it is the architecture, and
                             the same dpmi_sel_is32() rule 0204/0205 already use. */
                        { WORD ss = (WORD)(VDM_REG(tib, VTIB_SS) & 0xFFFF);
                          DWORD sb = dpmi_sel_base(ss);
                          DWORD sp = dpmi_sel_is32(ss) ? VDM_REG(tib, VTIB_ESP)
                                                       : (VDM_REG(tib, VTIB_ESP) & 0xFFFF);
                          const BYTE *sk = (const BYTE *)(ULONG_PTR)(sb + sp);
                          p = zput(p, "DPMI-CP["); p = zhex(p, (unsigned)steps);
                          p = zput(p, "] regs EAX=0x"); p = zhex(p, VDM_REG(tib, VTIB_EAX));
                          p = zput(p, " EBX=0x"); p = zhex(p, VDM_REG(tib, VTIB_EBX));
                          p = zput(p, " ECX=0x"); p = zhex(p, VDM_REG(tib, VTIB_ECX));
                          p = zput(p, " EDX=0x"); p = zhex(p, VDM_REG(tib, VTIB_EDX));
                          p = zput(p, " ESI=0x"); p = zhex(p, VDM_REG(tib, VTIB_ESI));
                          p = zput(p, " EDI=0x"); p = zhex(p, VDM_REG(tib, VTIB_EDI));
                          p = zput(p, " EBP=0x"); p = zhex(p, VDM_REG(tib, VTIB_EBP));
                          p = zput(p, " DS=0x"); p = zhex(p, VDM_REG(tib, VTIB_DS) & 0xFFFF);
                          p = zput(p, " ES=0x"); p = zhex(p, VDM_REG(tib, VTIB_ES) & 0xFFFF);
                          p = zput(p, " FS=0x"); p = zhex(p, VDM_REG(tib, VTIB_FS) & 0xFFFF);
                          p = zput(p, " GS=0x"); p = zhex(p, VDM_REG(tib, VTIB_GS) & 0xFFFF);
                          p = zput(p, " efl=0x"); p = zhex(p, VDM_REG(tib, VTIB_EFLAGS));
                          p = zput(p, " ssbase=0x"); p = zhex(p, sb);
                          p = zput(p, " sp=0x"); p = zhex(p, sp);
                          p = zput(p, " stack@ss:sp=");
                          if (!host_readable(sk, 32)) p = zput(p, "<unreadable from host>");
                          else                        p = zdump(p, sk, 32);
                          /* ── AND THE FRAME AT SS:BP ──────────────────────────────────
                             Static disassembly of DOOM.EXE (the DOS/4GW 16-bit half is
                             bound in at file offset 0x1DD0, so guest 0x0F:off = file
                             0x1DD0+off) says the client dies in its HANDOFF to the
                             application, twelve instructions after the last checkpoint:
                               72d8 mov es,[bp+2]   72db mov di,[bp+0xe]
                               72de/e2/e6 build an IRET frame from [bp+0x1e/0x22/0x26]
                               72ea mov bx,[bp+4]   72ef mov ss,ax   72f8 mov ds/es,bx
                               72fc iret            <- enters the app
                             EVERY operand is a word in the frame at SS:BP, and all of
                             them are selectors or a far entry point. Dumping the frame is
                             therefore the whole question: which descriptors it is about to
                             load, and where it is about to jump. Without it we would be
                             guessing which of the twelve faults; with it the answer is a
                             lookup against the descriptor calls already in this log. */
                          { const BYTE *fr = (const BYTE *)(ULONG_PTR)
                                (sb + (VDM_REG(tib, VTIB_EBP) & 0xFFFF));
                            p = zput(p, " frame@ss:bp=");
                            if (!host_readable(fr, 0x30)) p = zput(p, "<unreadable from host>");
                            else {
                                p = zdump(p, fr, 0x30);
                                /* ► AND THE CODE IT IS ABOUT TO JUMP TO. This half IS
                                     DOS/4GW-frame-specific and says so: [bp+0x22]:[bp+0x1e]
                                     is the far entry the IRET at 0x72fc consumes. The first
                                     frame dump proved every descriptor it loads is in range
                                     and correctly typed (SS=0xAF lim 0x7cff, SP=0x6F3E;
                                     DS/ES=0x17; CS=0x8F lim 0x5e3f, IP=0x2C63) -- so the
                                     fault is not the handoff, it is the FIRST INSTRUCTIONS
                                     OF THE MODULE, and those live in a block DOS/4GW read
                                     out of DOOM.EXE at runtime. They are not in any file we
                                     can disassemble offline; the only place they exist is
                                     guest memory, here, now. Hence the dump.
                                     Costs nothing when the frame is not a DOS/4GW one: the
                                     selector simply will not resolve to readable memory. */
                                { WORD fcs = *(const WORD *)(fr + 0x22);
                                  WORD fip = *(const WORD *)(fr + 0x1e);
                                  const BYTE *ep = (const BYTE *)(ULONG_PTR)
                                      (dpmi_sel_base(fcs) + fip);
                                  p = zput(p, " entry=0x"); p = zhex(p, fcs);
                                  p = zput(p, ":0x"); p = zhex(p, fip);
                                  p = zput(p, " base=0x"); p = zhex(p, dpmi_sel_base(fcs));
                                  p = zput(p, " code@entry=");
                                  if (!host_readable(ep, 64)) p = zput(p, "<unreadable from host>");
                                  else                        p = zdump(p, ep, 64); } } }
                          p = zput(p, "\r\n");
                          log_append(LOG_PATH, base, p); serial_out(base, p); p = base; }
                    }
                    dpmi_arm_fault_trampoline(tib, 0);   /* re-arm nest/flag/[0x638]/[TIB+8] */
                    if (steps < g_dpmi_cp_max) {
                        p = zput(p, "DPMI-CP["); p = zhex(p, (unsigned)steps);
                        p = zput(p, "] armed -> entering PM\r\n");
                        log_append(LOG_PATH, base, p); serial_out(base, p); p = base;
                    }
                    /* Give the watchdog a guaranteed turn before the FIRST entry only. If it
                       has logged a sample by the time we hand off, then its silence afterwards
                       means the whole process was killed at once (a kernel VDM terminate),
                       not that the thread never ran. 300 ms, once, on a diagnostic path. */
                    if (steps == 0) Sleep(300);
                    /* ── TELL THE ASYNC INJECTOR THE GUEST IS RUNNING. ───────────────────
                         g_in_exec means "the CPU thread is executing GUEST code, so its
                         context is the guest's and may be rewritten"; async_inject_irq()
                         refuses to touch the thread without it, precisely so it cannot race
                         the host manipulating the TIB. It was set only around v86_run(), so
                         for the whole of a protected-mode session the answer was "no" and
                         every asynchronous delivery bailed at the first line -- which is why
                         a PM guest could never be interrupted at all. Protected-mode
                         execution is execution too. */
                    /* ► NARROW ESP ON A 16-BIT STACK FOR **BOTH** PATHS. This was added
                         for the kernel path (it took the spike from 1 PM entry to 8) on the
                         reasoning that the far-jmp path reloads the junk harmlessly with
                         `lss`. Mostly true -- but measured, it is not always: give Doom a
                         command-line argument and the run dies right after the AH=30h
                         version check with
                             SS=0x00c7 (SS D/B=0)  ESP=0xb33b6f14
                             GH#18: PM-FAULT REFLECTED -- saved CS:EIP=0x00c7:0xb33b6f1e
                         and 0xb33b is a HOST THREAD-STACK address sitting in the top half
                         of ESP, because with a 16-bit SS the CPU maintains SP only and
                         whatever the host last had there stays.
                         ⚠ CLEARING IT DOES **NOT** FIX THAT BUG -- measured, the run is
                           identical (467 INT 31h calls either way). Kept anyway because
                           the junk is objectively wrong state that shows up in every dump
                           and there is no case where the high half of ESP is meaningful
                           while SS is 16-bit. Do not read this as the argument fix. */
                    if (!dpmi_sel_is32((WORD)(VDM_REG(tib, VTIB_SS) & 0xFFFF)))
                        VDM_REG(tib, VTIB_ESP) &= 0xFFFFu;
                    InterlockedExchange(&g_in_exec, 1);
                    if (g_dpmi_use_kernel) {
                        /* Hand the PM CONTEXT to the kernel monitor exactly as the V86
                           path does. Same TIB, same call; the only difference is that
                           EFLAGS.VM is clear and CS/SS/DS hold LDT selectors. */
                        LONG kst = 0; DWORD kev;
                        /* ► THE TOP HALF OF ESP IS JUNK ON A 16-BIT STACK, AND THE KERNEL
                             READS ALL OF IT. With a 16-bit SS the CPU maintains SP only, so
                             whatever was last in the high half stays there -- the far-jmp
                             path stores that junk into the TIB on exit and reloads it
                             harmlessly with `lss`, because the CPU ignores it. The kernel
                             does not: it takes the CONTEXT's ESP whole. Measured, and it is
                             the difference between the entry that works and the one that
                             never returns:
                                 entry 0  ss:esp=0x1f:0x0000fffe   -> returns ev=4
                                 entry 1  ss:esp=0x17:0xb33afffa   -> never returns
                             0xb33a is a host thread-stack address, and 0xb33afffa is far
                             outside a 0xFFFF-limit selector. Narrow it to what the
                             descriptor can actually address. */
                        if (!dpmi_sel_is32((WORD)(VDM_REG(tib, VTIB_SS) & 0xFFFF)))
                            VDM_REG(tib, VTIB_ESP) &= 0xFFFFu;
                        if (steps < 400) {      /* BEFORE the call: an entry that never
                                                  returns leaves no other trace at all */
                            p = zput(p, "PMKERNEL[");   p = zhex(p, (unsigned)steps);
                            p = zput(p, "] enter cs:eip=0x"); p = zhex(p, VDM_REG(tib, VTIB_CS) & 0xFFFF);
                            p = zput(p, ":0x");         p = zhex(p, VDM_REG(tib, VTIB_EIP));
                            p = zput(p, " ss:esp=0x");  p = zhex(p, VDM_REG(tib, VTIB_SS) & 0xFFFF);
                            p = zput(p, ":0x");         p = zhex(p, VDM_REG(tib, VTIB_ESP));
                            p = zput(p, " efl=0x");     p = zhex(p, VDM_REG(tib, VTIB_EFLAGS));
                            p = zput(p, " msw=0x");     p = zhex(p, *(volatile WORD *)(tib + VTIB_MSW));
                            p = zput(p, " [0x714]=0x"); p = zhex(p, *(volatile DWORD *)(ULONG_PTR)0x714);
                            p = zput(p, "\r\n");
                            log_append(LOG_PATH, base, p); serial_out(base, p); p = base;
                        }
                        g_pm_entry_eip = (LONG)VDM_REG(tib, VTIB_EIP);
                        kev = v86_run(tib, &kst);
                        g_pm_entry_eip = -1;
                        if (steps < 400) {
                            p = zput(p, "PMKERNEL[");     p = zhex(p, (unsigned)steps);
                            p = zput(p, "] VdmStartExecution -> st=0x"); p = zhex(p, (DWORD)kst);
                            p = zput(p, " ev=0x");        p = zhex(p, kev);
                            p = zput(p, " cs:eip=0x");    p = zhex(p, VDM_REG(tib, VTIB_CS) & 0xFFFF);
                            p = zput(p, ":0x");           p = zhex(p, VDM_REG(tib, VTIB_EIP));
                            p = zput(p, " efl=0x");       p = zhex(p, VDM_REG(tib, VTIB_EFLAGS));
                            p = zput(p, "\r\n");
                            log_append(LOG_PATH, base, p); serial_out(base, p); p = base;
                        }
                    } else {
                        /* ── HOW LONG DOES THE GUEST RUN WITHOUT GIVING US A TURN? ───────
                             Session 20's finding is that Doom dies inside a stretch of
                             protected-mode code that never BOPs, and that shortening the
                             stretch makes the VDM survive. That was measured in
                             INSTRUCTIONS, from a disassembly. Nothing has ever measured it
                             in TIME -- and time is what decides between "the guest
                             eventually does something illegal" and "a wall-clock deadline
                             in the kernel expires". One QueryPerformanceCounter pair per
                             PM entry is free next to the CreateFile-per-line logging this
                             host already does, and only stretches past the threshold say
                             anything, so a healthy run pays one comparison.
                             Report the ENTRY point as well as the exit: the entry is the
                             instruction after the BOP we last serviced, i.e. the name of
                             the stretch. */
                        LARGE_INTEGER t0, t1;
                        DWORD s_cs = VDM_REG(tib, VTIB_CS) & 0xFFFF, s_eip = dpmi_pm_eip(tib);
                        QueryPerformanceCounter(&t0);
                        dpmi_enter_pm(tib);
                        QueryPerformanceCounter(&t1);
                        {
                            DWORD us = qpc_us(t1.QuadPart - t0.QuadPart);
                            if (us > g_pm_stretch_max_us) {
                                g_pm_stretch_max_us = us;
                                if (us >= PM_STRETCH_LOG_US && g_pm_stretch_logged++ < 256) {
                                    p = zput(p, "PMSTRETCH us="); p = zhex(p, us);
                                    p = zput(p, " entry=0x"); p = zhex(p, s_cs);
                                    p = zput(p, ":0x"); p = zhex(p, s_eip);
                                    p = zput(p, " lin=0x");
                                    p = zhex(p, dpmi_sel_base((WORD)s_cs) + s_eip);
                                    p = zput(p, " exit=0x");
                                    p = zhex(p, VDM_REG(tib, VTIB_CS) & 0xFFFF);
                                    p = zput(p, ":0x"); p = zhex(p, dpmi_pm_eip(tib));
                                    p = zput(p, " ev=0x"); p = zhex(p, VDM_REG(tib, VTIB_EVENT));
                                    p = zput(p, " ms="); p = zhex(p, GetTickCount());
                                    p = zput(p, "\r\n");
                                    log_append(LOG_PATH, base, p); serial_out(base, p); p = base;
                                }
                            }
                        }
                    }
                    InterlockedExchange(&g_in_exec, 0);
                    if (steps < g_dpmi_cp_max) {
                        p = zput(p, "DPMI-CP["); p = zhex(p, (unsigned)steps);
                        p = zput(p, "] returned ev=0x"); p = zhex(p, VDM_REG(tib, VTIB_EVENT));
                        p = zput(p, " cs:eip=0x"); p = zhex(p, VDM_REG(tib, VTIB_CS) & 0xFFFF);
                        p = zput(p, ":0x"); p = zhex(p, VDM_REG(tib, VTIB_EIP) & 0xFFFF);
                        p = zput(p, "\r\n");
                        log_append(LOG_PATH, base, p); serial_out(base, p); p = base;
                    }
                    ev  = VDM_REG(tib, VTIB_EVENT);
                    eip = dpmi_pm_eip(tib);
                    csv = VDM_REG(tib, VTIB_CS) & 0xFFFF;
                    g_dpmi_last_ev  = ev;  g_dpmi_last_eip = eip;  g_dpmi_last_cs = csv;
                    /* ── AN ASYNC PM INTERRUPT HAS COME BACK. ────────────────────────────
                       dpmi_async_inject_pm() rewrote the running thread's context to vector
                       at the client's handler, with the frame's return CS:EIP pointing at
                       our catcher -- so the handler's IRET lands here as a BOP. The IRET
                       restored the FLAGS we pushed but NOT the guest's place in its own
                       code, because that return address was the catcher's. Put the saved
                       context back and let the guest carry on as though nothing happened,
                       which is precisely what "transparent" means for a hardware interrupt. */
                    if (ev == VDM_EVENT_BOP && g_async_pm_active
                        && csv == g_pmret_sel && eip == DPMI_PMRET_OFF) {
                        VDM_SET16(tib, VTIB_CS, g_async_pm_cs);
                        VDM_REG(tib, VTIB_EIP)    = g_async_pm_eip;
                        VDM_SET16(tib, VTIB_SS, g_async_pm_ss);
                        VDM_REG(tib, VTIB_ESP)    = g_async_pm_esp;
                        VDM_REG(tib, VTIB_EFLAGS) = g_async_pm_efl;
                        g_dpmi_vi = 1;                   /* unmask: the handler has finished */
                        g_async_pm_active = 0;
                        /* ── DRAIN THE BACKLOG WHILE WE STILL HAVE CONTROL. ──────────────
                             One asynchronous delivery costs a SuspendThread /
                             GetThreadContext / SetThreadContext round trip -- tens of
                             microseconds. Doom programs the PIT to reload 0x4a, i.e.
                             16124 Hz, and its millisecond delay waits `ms * scale / 1000`
                             ticks: about 480 for a 30 ms wait. One round trip per tick is
                             not a design at that rate, and the latch saturates at
                             IRQ0_PENDING_MAX=4 anyway, so chasing it delivered THREE ticks
                             against the hundreds owed and the guest never left its spin.
                             The asynchronous path's real job is to get us INTO a guest that
                             would otherwise never come out. Once here, the ISR can be run
                             directly and synchronously -- measured at phases=1, i.e. it
                             enters and IRETs with no excursion -- so one round trip serves a
                             whole batch. This is catch-up, the same shape the PIT already
                             uses when the host falls behind (pit_gaps), not an invention. */
                        /* ► SAY WHETHER THIS RAN, AND HOW FAR. Session 18 recorded
                             "coalescing the tick drain changed nothing (batch 64 -> 5
                             ticks, batch 3 -> 4)" and filed it as a dead end. But the
                             tick counter only ever advances ONE per async round trip,
                             which is what it would do if this batch never delivered --
                             and nothing here says which. Two numbers settle it: was the
                             gate taken, and what was k at exit. */
                        { int k = -1, gate;
                          gate = (g_dpmi_client32 && g_pm_app_hooked_timer && !g_pm_noirq
                                  && dpmi_sel_is32((WORD)(VDM_REG(tib, VTIB_CS) & 0xFFFF)));
                          uint32_t preb = g_dma.rd_count[1];
                          if (gate) {
                            g_in_pm_irq = 1;
                            /* ► DRAIN WHAT IS OWED, NOT A FIXED SIXTY-FOUR. This loop used to
                                 run the full DPMI_IRQ0_BATCH every time it was entered, with
                                 nothing tying it to elapsed time -- so the client's clock ran
                                 at the rate we happened to return from asynchronous
                                 injections rather than the rate it programmed into the 8254.
                                 Measured on Doom at 140 Hz: 169,032 ISR entries in 45 s
                                 against 6,300 owed, i.e. a game running 27x too fast. The
                                 batch is still worth having -- one SuspendThread round trip
                                 should repay a whole backlog -- but the backlog is a COUNT,
                                 and pm_tick_take() is where it lives. */
                            /* ► CONSUME A TICK ONLY IF IT WAS ACTUALLY DELIVERED.
                                 dpmi_inject_pm_irq() declines whenever the guest is in
                                 the extender's 16-bit code rather than the application
                                 -- a routine and correct refusal -- and taking the tick
                                 first threw it away every time that happened. Measured
                                 on Doom: 3,349 ISR entries in 45 s against the 6,300 it
                                 programmed at 140 Hz, i.e. a game clock running at half
                                 speed, which is most of "very laggy". The same mistake
                                 as the keyboard's, with the sign reversed: there,
                                 consuming late cancelled an interrupt the handler had
                                 raised; here, consuming early discarded one nobody ran. */
                            for (k = 0; k < DPMI_IRQ0_BATCH; ++k) {
                                if (g_pm_tick_owed <= 0) break;
                                if (!dpmi_inject_pm_irq(&m, tib, 0x08, steps)) break;
                                InterlockedDecrement(&g_pm_tick_owed);
                            }
                            g_in_pm_irq = 0;
                          }
                          g_coop_dma_polls += g_dma.rd_count[1] - preb;
                          { char bb[160], *bq = bb;
                            bq = zput(bq, "  BATCH gate="); bq = zhex(bq, (DWORD)gate);
                            bq = zput(bq, " k="); bq = zhex(bq, (DWORD)k);
                            bq = zput(bq, " c32="); bq = zhex(bq, (DWORD)g_dpmi_client32);
                            bq = zput(bq, " hooked="); bq = zhex(bq, (DWORD)g_pm_app_hooked_timer);
                            bq = zput(bq, " cs=0x"); bq = zhex(bq, VDM_REG(tib, VTIB_CS) & 0xFFFF);
                            bq = zput(bq, "\r\n"); log_append(LOG_PATH, bb, bq); serial_out(bb, bq); } }
                        continue;
                    }
                    /* GH#18 (bare-metal crack, 2026-08-18): dpmi_enter.S reports event 3 when it
                       DECLINES to enter PM -- guest IF=1 AND [0x714]&3 signals a pending hardware
                       interrupt (the FIXED_NTVDMSTATE pending bits; see dpmi_enter.S label 2). We
                       run PM IN-PROCESS (far-jmp), NOT via VdmStartExecution, so while PM executes
                       the kernel does not manage this VDM's interrupt assist -- those pending bits
                       are STALE real-mode state: a timer IRQ0 latched during the DOS INT 21h calls
                       before the mode switch. On real 3.3GHz silicon a tick is essentially always
                       pending at switch time (QEMU+HVF's dilated clock rarely had one), so the
                       monitor bailed with event 3 forever and the PM client never ran a single
                       instruction (EIP stuck at entry) -- THE session-8 "kernel won't run PM" wall.
                       Clear the stale pending bits and re-enter. Bounded so a genuinely re-arming
                       pending can't spin; the BIOS tick still advances via the IRQ0 path above. */
                    if (ev == 3) {
                        if (++ev3_retries <= 0x10000) {
                            if (ev3_retries <= 3) {
                                p = zput(p, "GH#18: event3 pending-int guard at CS:EIP=0x");
                                p = zhex(p, csv); p = zput(p, ":0x"); p = zhex(p, eip);
                                p = zput(p, " [0x714]=0x");
                                p = zhex(p, *(volatile DWORD *)(ULONG_PTR)0x714);
                                p = zput(p, " -> clear stale pending + re-enter\r\n");
                                log_append(LOG_PATH, base, p); serial_out(base, p); p = base;
                            }
                            *(volatile DWORD *)(ULONG_PTR)0x714 &= ~3u;
                            continue;
                        }
                        /* retry budget exhausted -> fall through and report an unexpected stop */
                    }
                    /* GH #18: the raw-PM-#GP reflect landed on our handler code selector. THIS
                       is the proof point: the kernel reflected a fault it used to swallow. */
                    if (ev == VDM_EVENT_BOP && csv == (g_dpmi_flt_code_sel & 0xFFFF)
                        && eip == DPMI_FAULT_COFF) {
                        DWORD fcs  = *(volatile WORD  *)(tib + VTIB_FLT_SAVCS);
                        DWORD feip = *(volatile DWORD *)(tib + VTIB_FLT_SAVEIP);
                        DWORD fss3 = *(volatile DWORD *)(tib + VTIB_FLT_SAV3);
                        /* ⚠ THESE TWO FIELDS ARE **NOT** CS:EIP, WHATEVER THEIR NAMES SAY.
                             Measured (session 19): the pair reads 0x00c7:0x...6f1e while the
                             guest's CS was 0x6f and its SS:ESP was 0x00c7:0x...6f14 -- i.e.
                             SS and ESP+0xa. Proved by accident and decisively: clearing the
                             junk top half of ESP changed this "EIP" from 0xb33b6f1e to
                             0x00006f1e. A field that tracks ESP is not EIP. The raw window
                             below shows the layout; `sav3` (tib+0x640, 0x36af here) is the
                             likelier faulting EIP. Do not resume anything on fcs:feip until
                             the slots are calibrated against a fault at a KNOWN address --
                             pmfault's HLT/INT3 cannot do it (they die without reflecting),
                             so that needs a new variant that loads a bad selector. */
                        p = zput(p, "GH#18: PM-FAULT REFLECTED to trampoline -- savSS:savESP=0x");
                        p = zhex(p, fcs); p = zput(p, ":0x"); p = zhex(p, feip);
                        p = zput(p, " (MISNAMED VTIB_FLT_SAVCS/SAVEIP) sav3=0x"); p = zhex(p, fss3);
                        p = zput(p, " nest=0x"); p = zhex(p, *(volatile WORD *)(tib + VTIB_FLT_NEST));
                        /* ► READ THE LAYOUT OFF THE TIB INSTEAD OF TRUSTING THE OFFSETS.
                             VTIB_FLT_SAVCS/SAVEIP were reverse-engineered in an earlier
                             session, and what they return does not look like a code
                             address: the argument-run fault reported CS=0x00c7 -- which is
                             the guest's SS, not its CS (0x6f) -- and EIP=0xb33b6f1e, which
                             is that run's ESP (0xb33b6f14) plus 0xa. Dump the window so the
                             real slots can be identified by looking for the KNOWN CS and a
                             plausible EIP, rather than by guessing a frame shape. */
                        { const BYTE *fw = (const BYTE *)(ULONG_PTR)(tib + 0x630);
                          p = zput(p, " tib[630..64f]=");
                          if (!host_readable(fw, 0x20)) p = zput(p, "<unreadable>");
                          else                          p = zdump(p, fw, 0x20); }
                        p = zput(p, " liveCS=0x"); p = zhex(p, VDM_REG(tib, VTIB_CS) & 0xFFFF);
                        p = zput(p, " liveSS=0x"); p = zhex(p, VDM_REG(tib, VTIB_SS) & 0xFFFF);
                        p = zput(p, " liveESP=0x"); p = zhex(p, VDM_REG(tib, VTIB_ESP));
                        p = zput(p, " -- REAL-CPU PM #GP reflect WORKS\r\n");
                        log_append(LOG_PATH, base, p); serial_out(base, p); p = base;
                        /* ⚠⚠ **THIS `break` IS WHY "DOS/4GW QUITS" ON A COMMAND-LINE ARGUMENT.**
                             It was right for run 59, whose deliverable was the reflect FIRING.
                             But it means ANY real PM fault ends the run with a tidy
                             `STAGE2: exec loop exited -> flushing` and `STAGE2: complete`,
                             which reads exactly like the client choosing to exit -- and that
                             is how the argument bug was described for two sessions. The guest
                             is not quitting; we are stopping it.
                             ▶ THE FIX IS DPMI-STANDARD AND THE PLUMBING EXISTS: a client
                               registers exception handlers with INT 31h 0203, and DOS/4GW
                               registers THIRTEEN of them (measured). A reflected fault should
                               be dispatched to g_pm_exc[n] for its exception number, exactly
                               as dpmi_dispatch_to_pm_handler() does for interrupts; only if
                               none is registered should the run end -- and then with a
                               message saying so, not a clean `complete`.
                               Blocked on identifying the exception number and the faulting
                               CS:EIP -- see the misnaming note above. */
                        break;
                    }
                    /* GH#18 run 72: a real-CPU PROTECTED-MODE I/O insn (IN/OUT) reflects as
                       event 0 -- the SAME VDD-trap event as V86 (VM-confirmed by outprobe.com,
                       a PM `OUT DX,AL` to 0x3C8). Service it through the device bus and resume,
                       so PM port I/O (VGA/sound) reaches our VDDs instead of the loop treating
                       event 0 as an "unexpected PM stop" and spinning. */
                    if (ev == VDM_EVENT_IO || ev == VDM_EVENT_IO_HW || ev == VDM_EVENT_GPFAULT) {
                        int io_h;
                        HOST_LOCK();
                        io_h = host_try_io_pm(tib, &g_bus);
                        HOST_UNLOCK();
                        if (io_h) continue;   /* serviced the port op -> keep running */
                        /* not a decodable I/O op -> fall through to the normal dispatch/stop */
                    }
                    /* BARE-METAL diagnostic (GH #18): dump the faulting PM instruction bytes for
                       any non-BOP stop, so we can identify what real hardware reflects as event 3
                       (raw PM #GP) vs the HVF silent-terminate. Rate-limited to the first 32 stops
                       so a client that repeatedly faults can't flood the log (session-9). */
                    if (ev != VDM_EVENT_BOP && g_pmfault_dumps < 32) {
                        DWORD fb = dpmi_sel_base((WORD)csv);
                        ++g_pmfault_dumps;
                        const volatile BYTE *fi = (const volatile BYTE *)(ULONG_PTR)(fb + eip);
                        uint32_t sel_ar = 0, sel_lim = 0; dpmi_sel_desc((WORD)csv, &sel_ar, &sel_lim);
                        p = zput(p, "GH#18 PM-FAULT ev=0x"); p = zhex(p, ev);
                        p = zput(p, " CS:EIP=0x"); p = zhex(p, csv); p = zput(p, ":0x"); p = zhex(p, eip);
                        p = zput(p, " base=0x"); p = zhex(p, fb); p = zput(p, " AR=0x"); p = zhex(p, sel_ar);
                        p = zput(p, " lim=0x"); p = zhex(p, sel_lim); p = zput(p, " bytes=");
                        p = zdump(p, (const void *)fi, 12); p = zput(p, "\r\n");
                        log_append(LOG_PATH, base, p); serial_out(base, p); p = base;
                    }
                    vec = (ev == VDM_EVENT_BOP) ? dpmi_bop_vec(csv, eip) : 0;
                    g_dpmi_last_vec = vec;
                    rc = dpmi_service_pm_int(&m, tib, vec, steps);
                    if (rc > 0) continue;   /* serviced -> keep running the PM client */
                    break;                  /* 0 = client exited, <0 = unexpected stop */
                }
                g_dpmi_done = 1;            /* PM run finished -> watchdog stands down, window persists */
                break;
            }
            p = zput(p, " -> SWITCH FAILED (staying real mode, CF=1)\r\n");
            log_append(LOG_PATH, base, p); serial_out(base, p); p = base;
            VDM_REG(tib, VTIB_EFLAGS) |= 1;         /* CF=1 signals failure to the client */
            VDM_REG(tib, VTIB_EIP) += 3;            /* -> the RETF, returns real mode     */
            continue;
        }
        m.tp = p;
        m.retry = 0;
        if (!dos_int21(&m)) {                       /* AH=4Ch -> terminate */
            p = m.tp;
            if (g_exec_depth > 0) {
                /* A CHILD terminated, not the program we were asked to run.
                   Put the parent's frame back and let it continue. */
                int d = --g_exec_depth;
                volatile WORD *pfl2;
                m.child_rc = (WORD)(m.exit_code & 0xFF);
                if (g_exec[d].child_seg) dos_free(NULL, g_exec[d].child_seg);
                VDM_REG(tib, VTIB_EAX) = g_exec[d].eax; VDM_REG(tib, VTIB_EBX) = g_exec[d].ebx;
                VDM_REG(tib, VTIB_ECX) = g_exec[d].ecx; VDM_REG(tib, VTIB_EDX) = g_exec[d].edx;
                VDM_REG(tib, VTIB_ESI) = g_exec[d].esi; VDM_REG(tib, VTIB_EDI) = g_exec[d].edi;
                VDM_REG(tib, VTIB_EBP) = g_exec[d].ebp; VDM_REG(tib, VTIB_ESP) = g_exec[d].esp;
                VDM_REG(tib, VTIB_EIP) = g_exec[d].eip; VDM_REG(tib, VTIB_EFLAGS) = g_exec[d].efl;
                VDM_REG(tib, VTIB_CS)  = g_exec[d].cs;  VDM_REG(tib, VTIB_SS) = g_exec[d].ss;
                VDM_REG(tib, VTIB_DS)  = g_exec[d].ds;  VDM_REG(tib, VTIB_ES) = g_exec[d].es;
                m.psp_seg = g_exec[d].psp;
                m.dta_seg = g_exec[d].dta_seg; m.dta_off = g_exec[d].dta_off;
                /* EXEC succeeded, so clear the carry the parent's IRET will
                   restore, and set AX=0 as DOS does. */
                pfl2 = (volatile WORD *)(((VDM_REG(tib, VTIB_SS) & 0xFFFF) << 4)
                        + (((VDM_REG(tib, VTIB_ESP) & 0xFFFF) + 4) & 0xFFFF));
                *pfl2 &= (WORD)~1;
                VDM_REG(tib, VTIB_EAX) &= 0xFFFF0000u;
                VDM_REG(tib, VTIB_EIP) += 3;        /* past the BOP -> the IRET */
                m.exit_code = 0;
                p = zput(p, "  EXEC: child exited rc=0x"); p = zhexb(p, m.child_rc);
                p = zput(p, ", parent resumed (depth="); p = zhexb(p, (unsigned)g_exec_depth);
                p = zput(p, ")\r\n");
                log_append(LOG_PATH, base, p); p = base;
                continue;
            }
            VDM_REG(tib, VTIB_EIP) += 3;
            log_append(LOG_PATH, base, p); p = base;
            break;
        }
        p = m.tp;
        if (m.exec_pending) {                       /* GH #30: AH=4Bh */
            m.exec_pending = 0;
            p = exec_begin(&m, tib, p);
            log_append(LOG_PATH, base, p); p = base;
            continue;                               /* CS:IP now points at the child */
        }
        /* A blocking read with nothing to return leaves EIP ON the BOP, so the guest
           re-executes the INT and keeps running -- and keeps taking timer interrupts, so
           its music and animation carry on while it waits for a key, as on real hardware. */
        if (!m.retry) VDM_REG(tib, VTIB_EIP) += 3;  /* past the 3-byte BOP -> the IRET */
        log_append(LOG_PATH, base, p); p = base;
    }
    }   /* storm-state block */

    /* Log the exec-loop exit before any flushing, so a hang or fault during
       shutdown is distinguishable from the loop never exiting at all. */
    p = zput(p, "STAGE2: exec loop exited -> flushing\r\n");
    log_append(LOG_PATH, base, p); serial_out(base, p); p = base;

    /* The exec loop is out: tell the headless backstop a clean shutdown is under
       way so it does not force-exit us mid-flush and lose the DOS output. */
    InterlockedExchange(&g_wound_down, 1);

    g_ci.ExitCode = (ULONG)m.exit_code;             /* M2.5: errorlevel (shell notify = best-effort TODO) */

    /* Flush captured DOS output to the console + the log. */
    {
        HANDLE hcon = CreateFileA("CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, NULL,
                                  OPEN_EXISTING, 0, NULL);
        if (m.out_len > 0) {
            m.out[m.out_len] = 0;
            if (hcon != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(hcon, m.out, m.out_len, &w, NULL); }
            p = zput(p, "  ==> DOS OUTPUT: ["); p = zput(p, m.out);
            if (m.out_trunc) p = zput(p, "\r\n<<<OUTPUT TRUNCATED>>>");
            p = zput(p, "]\r\n");
        }
        if (hcon != INVALID_HANDLE_VALUE) CloseHandle(hcon);
    }
    /* Exec-loop accounting: how much of the run went on port-I/O round trips, how
       much the burst fast path absorbed, and whether timer IRQs actually landed. */
    { int i; p = zput(p, "STAGE2: hot ports:");
      for (i = 0; i < g_io_hot_n; ++i) {
          p = zput(p, " 0x"); p = zhex(p, g_io_hot[i].port);
          p = zput(p, "=0x"); p = zhex(p, g_io_hot[i].n);
      }
      p = zput(p, "\r\nSTAGE2: pit_reload=0x"); p = zhex(p, (DWORD)g_pit.reload);
      p = zput(p, " skip_if=0x");   p = zhex(p, g_irq0_skip_if);
      p = zput(p, " skip_stub=0x"); p = zhex(p, g_irq0_skip_stub);
      p = zput(p, " async_inj=0x"); p = zhex(p, g_async_inj);
      p = zput(p, " async_pm=0x"); p = zhex(p, g_async_pm_inj);
      p = zput(p, " async_bail=0x"); p = zhex(p, g_async_bail);
      p = zput(p, " async_nest=0x"); p = zhex(p, g_async_nest_blocked);
      p = zput(p, " irq1_inj=0x");   p = zhex(p, g_irq1_inj);
      p = zput(p, " int16=[");
      { int k; for (k = 0; k < 4; ++k) { p = zput(p, "0x"); p = zhex(p, g_in.int16_calls[k]); p = zput(p, " "); } }
      p = zput(p, "] p60=0x");       p = zhex(p, g_in.p60_reads);
      p = zput(p, " sc_left=0x");    p = zhex(p, (DWORD)vdd_input_sc_pending(&g_in));
      p = zput(p, " sc_push=0x");    p = zhex(p, g_in.sc_pushed);
      p = zput(p, " sc_drop=0x");    p = zhex(p, g_in.sc_dropped);
      /* sc_hi is the deepest the 32-byte FIFO ever got; pit_clamp counts catch-up
         bursts the PIT refused to replay. Together these say whether a held key was
         starved of exec-loop turns and whether the guest's clock ever lurched. */
      p = zput(p, " sc_hi=0x");      p = zhex(p, g_in.sc_hiwater);
      /* pit_gaps = syncs more than 10 ms apart; pit_gapmax = the worst, in 8254
         clocks (1193182 = 1 s). NOTHING is clamped to these -- they exist so the
         catch-up burst can be fixed from a measured gap distribution instead of an
         assumed one, which is precisely the mistake that made it worse. */
      p = zput(p, " pit_gaps=0x");   p = zhex(p, g_pit_catchup_clamped);
      p = zput(p, " pit_gapmax=0x"); p = zhex(p, g_pit_gap_max);
      p = zput(p, "\r\nSTAGE2: pitpace=");  p = zhex(p, (DWORD)g_pitpace_ms);
      p = zput(p, " calls="); p = zhex(p, g_pitpace_calls);
      p = zput(p, "\r\nSTAGE2: TICKGAP us[<.5k,1k,2k,4k,8k,16k,32k,64k,128k,256k,512k,+]=");
      { unsigned tb; for (tb = 0; tb < 12; ++tb) { p = zput(p, tb ? "," : "");
                                                   p = zhex(p, g_tickgap[tb]); } }
      p = zput(p, " max_us="); p = zhex(p, g_tickgap_max_us);
      p = zput(p, " OVER_11600us="); p = zhex(p, g_tickgap_over);
      p = zput(p, "\r\nSTAGE2: DMXTASK: ok="); p = zhex(p, g_dmx_mixer_ok);
      p = zput(p, " samples="); p = zhex(p, g_dmx_samples);
      p = zput(p, " any_busy="); p = zhex(p, g_dmx_anybusy);
      p = zput(p, " mixer_OVERDUE="); p = zhex(p, g_dmx_overdue);
      p = zput(p, " max_late_ticks="); p = zhex(p, g_dmx_overdue_max);
      p = zput(p, " busy_by_task=");
      { unsigned dt; for (dt = 0; dt < 12; ++dt) { p = zput(p, dt ? "," : "");
                                                   p = zhex(p, g_dmx_busy[dt]); } }
      p = zput(p, "\r\nSTAGE2: dspver="); p = zhex(p, (DWORD)g_sb_ver_major);
      p = zput(p, "."); p = zhex(p, (DWORD)g_sb_ver_minor);
      p = zput(p, " execprio="); p = zhex(p, g_exec_prio);
      p = zput(p, "\r\nSTAGE2: lock: wait_us=0x");  p = zhex(p, g_lk_wait_us);
      p = zput(p, "@line ");                        p = zhex(p, (DWORD)g_lk_wait_site);
      p = zput(p, " hold_us=0x");                   p = zhex(p, g_lk_hold_us);
      p = zput(p, "@line ");                        p = zhex(p, (DWORD)g_lk_hold_site);
      p = zput(p, " ui_gap_us=0x");                 p = zhex(p, g_ui_gap_us);
      /* ty_sent = typematic repeats WE generated; ty_os = OS auto-repeats we
         suppressed because we generate our own. If ty_os is large and ty_sent is
         small, the pump is not running; if both are small while a key was held,
         the OS was not delivering repeats either -- which is what started this. */
      p = zput(p, " ty_sent=0x");                   p = zhex(p, g_ty_sent);
      p = zput(p, " ty_os=0x");                     p = zhex(p, g_ty_os_repeats);
      /* The XP setting we derived the rate from, raw and in microseconds, so a run
         says WHY it repeats at the speed it does. Verify against stock ntvdm with
         tools/dostest/tymat.asm: the target is delay 7 ticks / 102 repeats. */
      p = zput(p, " spi_delay=0x");                 p = zhex(p, g_ty_spi_delay);
      p = zput(p, " spi_speed=0x");                 p = zhex(p, g_ty_spi_speed);
      p = zput(p, " ty_delay_us=0x");               p = zhex(p, g_ty_delay_us);
      p = zput(p, " ty_period_us=0x");              p = zhex(p, g_ty_period_us);
      /* Does the guest set its OWN typematic rate? If it does, ours is a guess and
         should be taken from its 0xF3 byte instead (bits 0-4 rate, 5-6 delay). */
      p = zput(p, "\r\nSTAGE2: kbd 8042: writes=0x"); p = zhex(p, g_in.kbd_out_writes);
      p = zput(p, " typematic_set=0x");               p = zhexb(p, g_in.kbd_typematic_set);
      p = zput(p, " rate_byte=0x");                   p = zhexb(p, g_in.kbd_typematic_byte);
      p = zput(p, " seq=");
      { unsigned k; for (k = 0; k < g_in.kbd_out_n; ++k) {
            p = zput(p, k ? "," : ""); p = zhexb(p, g_in.kbd_out_log[k][0]);
            p = zput(p, ":");          p = zhexb(p, g_in.kbd_out_log[k][1]); } }
      p = zput(p, " int10_11=0x");   p = zhex(p, g_vid.int10_11_calls);
      /* Each font request, its answer, and the BYTES actually sitting at the address we
         handed back -- read from guest memory, so a wiped or misaligned table is visible
         rather than inferred. A glyph is mostly zeros with a few set rows; all-zero or
         all-FF here means the caller is drawing from the wrong place. */
      { int fi; for (fi = 0; fi < g_vid.font_qn; ++fi) {
          const volatile BYTE *fp;
          p = zput(p, "\r\n  font_q: AL=0x"); p = zhex(p, g_vid.font_q[fi].al);
          p = zput(p, " BH=0x");   p = zhex(p, g_vid.font_q[fi].bh);
          p = zput(p, " -> ES:BP=0x"); p = zhex(p, g_vid.font_q[fi].seg);
          p = zput(p, ":0x");      p = zhex(p, g_vid.font_q[fi].off);
          p = zput(p, " CX=0x");   p = zhex(p, g_vid.font_q[fi].cx);
          fp = (const volatile BYTE *)(((DWORD)g_vid.font_q[fi].seg << 4)
                                       + g_vid.font_q[fi].off);
          { BYTE fb[16]; unsigned k; for (k = 0; k < 16; ++k) fb[k] = fp[k];
            p = zput(p, " bytes: "); p = zdump(p, fb, 16); }
      } }
      p = zput(p, "\r\n");
      log_append(LOG_PATH, base, p); serial_out(base, p); p = base; }
    p = zput(p, "STAGE2: io_events=0x");  p = zhex(p, g_ev_io);
    p = zput(p, " io_burst=0x");          p = zhex(p, g_io_extra);
    p = zput(p, " irq0_inj=0x");          p = zhex(p, g_irq0_inj);
    p = zput(p, " irq0_skip=0x");         p = zhex(p, g_irq0_skip);
    p = zput(p, " intpend=0x");           p = zhex(p, g_ev_intpend);
    p = zput(p, " iostr=0x");             p = zhex(p, g_ev_iostr);
    p = zput(p, " bda_tick=0x");          p = zhex(p, ((DWORD)peekw(0x46E) << 16) | peekw(0x46C));
    p = zput(p, "\r\n");
    { int i; p = zput(p, "STAGE2: unclaimed ports touched:");
      for (i = 0; i < g_unclaimed_n; ++i) { p = zput(p, " 0x"); p = zhex(p, g_unclaimed[i]); }
      p = zput(p, "\r\n"); }
    /* GH #27: one line per class of unimplemented thing the run actually reached,
       so a run yields a to-do list instead of "the screen looked wrong". Empty
       lines are printed too -- "INT21 unimplemented:" with nothing after it is a
       positive statement that nothing was missing, which a suppressed line is not. */
    { int i, n;
      p = zput(p, "STAGE2: INT21 unimplemented:");
      for (i = 0, n = 0; i < 256; ++i)
          if ((m.unimpl21[i >> 3] >> (i & 7)) & 1u) { p = zput(p, " AH=0x"); p = zhexb(p, (unsigned)i); ++n; }
      if (!n) p = zput(p, " none");
      p = zput(p, "\r\n");
      p = zput(p, "STAGE2: INT21 undefined-on-6.22 (no-op, matches DOS):");
      for (i = 0, n = 0; i < 256; ++i)
          if ((m.noop21[i >> 3] >> (i & 7)) & 1u) { p = zput(p, " AH=0x"); p = zhexb(p, (unsigned)i); ++n; }
      if (!n) p = zput(p, " none");
      p = zput(p, "\r\n");
      p = zput(p, "STAGE2: BIOS partial/unimplemented:");
      for (i = 0, n = 0; i < 256; ++i)
          if (g_bios_unimpl[i]) { p = zput(p, " INT"); p = zhexb(p, (unsigned)i); ++n; }
      if (!n) p = zput(p, " none");
      p = zput(p, "\r\n");
      p = zput(p, "STAGE2: INT10 unimplemented:");
      for (i = 0, n = 0; i < 256; ++i)
          if (VID_UNIMPL_GET(g_vid.unimpl_fn, i)) { p = zput(p, " AH=0x"); p = zhexb(p, (unsigned)i); ++n; }
      if (!n) p = zput(p, " none");
      p = zput(p, "\r\n");
      { unsigned pl, nz[4]; 
        for (pl = 0; pl < 4; ++pl) { unsigned k2, c2 = 0;
            for (k2 = 0; k2 < VID_PLANE_SIZE; ++k2) if (g_vid.plane[pl][k2]) ++c2;
            nz[pl] = c2; }
        /* OPL PROFILE (GH #21): what the guest's music driver actually asks for.
           A gap the game never uses cannot be why the music sounds flat. */
        opl_trace_dump();
        /* ── THE SOUND STACK, END TO END, IN ONE LINE. ───────────────────────────────
             Every part of this was previously either unreported or spread across three
             places, and "sound works" was being inferred from the guest not complaining.
             It answers, in order: did the guest's PCM reach the DMA engine, did the
             mixer run, did MIDI messages leave the MPU-401, and did the HOST devices
             actually open -- because a silent run with a happy guest and a silent run
             with no wave device look identical from the guest's side. */
        if (g_sb.cap_buf && g_sb.cap_len) {
          HANDLE hc = CreateFileA(SBDUMP_PATH, GENERIC_WRITE, 0, NULL,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
          if (hc != INVALID_HANDLE_VALUE) {
              DWORD wr = 0; WriteFile(hc, g_sb.cap_buf, g_sb.cap_len, &wr, NULL); CloseHandle(hc);
          }
          p = zput(p, "STAGE2: sound: raw PCM capture -> sb.raw, "); p = zhex(p, g_sb.cap_len);
          p = zput(p, " bytes\r\n");
      }
      p = zput(p, "STAGE2: sound: sb_blocks="); p = zhex(p, g_sb.blocks);
        p = zput(p, " sb_rate=");                 p = zhex(p, g_sb.rate_hz);
        p = zput(p, " sb_mode=");                 p = zhex(p, (DWORD)g_sb.xfer_mode);
        p = zput(p, " sb_dspwr=");                p = zhex(p, g_sb.dsp_writes);
        p = zput(p, " midi_msgs=");               p = zhex(p, g_mpu.sent);
        p = zput(p, "\r\n");
        p = zput(p, "STAGE2: async per IRQ:");
        { unsigned li; for (li = 0; li < 8; ++li) {
            p = zput(p, " irq"); p = zhexb(p, li); p = zput(p, "=");
            p = zhex(p, g_async_inj_line[li]); } }
        /* The retry that did not exist before: every one of these is a block-completion
           IRQ that would previously have been dropped, and so a 256-byte refill DMX would
           never have made. Compare `devirq_inj` against the shortfall between sb_blocks
           and irq05 above -- that is the whole of the fix, in one subtraction. */
        /* ► THE ECHO, AS A NUMBER, WITH ITS CONTROLLED VARIABLE NEXT TO IT. `replayed`
             counts blocks >=90% identical to the same ring offsets one lap earlier --
             blocks DMX never refilled, played twice 186 ms apart. `lead` is the buffers
             actually queued, i.e. how far ahead of audible we read. Vary one, read the
             other; if they do not move together the lead is not the cause. */
        /* ⚠ FLUSH FIRST. `base` points PAST the preamble, so report[] has well under
             8 KB of headroom, and the 24-line sbblk ledger added above eats most of what
             was left. The first cut of this line was written into the overflow and simply
             never appeared -- while the line immediately AFTER it did, which reads exactly
             like "the code did not run" and cost a wasted pair of rig runs to tell apart
             from a stale binary. Two counters in the same basic block cannot disagree
             about whether they executed; when they appear to, suspect the transport. */
        p = zput(p, "\r\n");
        log_append(LOG_PATH, base, p); serial_out(base, p); p = base;
        p = zput(p, "STAGE2: sb replay: blocks_checked="); p = zhex(p, g_sb.blocks_checked);
        p = zput(p, " REPLAYED=");   p = zhex(p, g_sb.blocks_replayed);
        if (g_sb.blocks_checked) {
            p = zput(p, " (");
            p = zhex(p, g_sb.blocks_replayed * 100u / g_sb.blocks_checked);
            p = zput(p, "% of blocks, decimal-in-hex)");
        }
        /* ► ...AND HOW MANY OF THOSE BLOCKS CARRIED ANY AUDIO. A silent block is
             identical to the previous lap when the guest refills it CORRECTLY, so
             `REPLAYED` on its own cannot support "DMX never refilled it". Only
             REPLAYED_LOUD can, and its denominator is the non-flat blocks. */
        p = zput(p, " flat=");          p = zhex(p, g_sb.blocks_flat);
        p = zput(p, " REPLAYED_LOUD="); p = zhex(p, g_sb.blocks_replayed_loud);
        if (g_sb.blocks_checked > g_sb.blocks_flat) {
            p = zput(p, " (");
            p = zhex(p, g_sb.blocks_replayed_loud * 100u
                        / (g_sb.blocks_checked - g_sb.blocks_flat));
            p = zput(p, "% of NON-FLAT blocks, decimal-in-hex)");
        }
        p = zput(p, " runs[1,2,3,4-7,8-15,16-31,32-63,64+]=");
        { unsigned rb; for (rb = 0; rb < 8; ++rb)
            { p = zput(p, rb ? "," : ""); p = zhex(p, g_sb.replay_runs[rb]); } }
        p = zput(p, " run_max="); p = zhex(p, g_sb.replay_run_max);
        p = zput(p, " byte_lap_same="); p = zhex(p, g_sb.lap_same);
        p = zput(p, "/");              p = zhex(p, g_sb.lap_total);
        p = zput(p, " ring=");         p = zhex(p, g_sb.lap_len);
        p = zput(p, " toobig=");       p = zhex(p, g_sb.lap_toobig);
        p = zput(p, " lead_buffers=");  p = zhex(p, g_wave.nbufs);
        p = zput(p, "\r\nSTAGE2: pit budget: syncs="); p = zhex(p, g_pit_syncs);
        p = zput(p, " raises=");    p = zhex(p, g_irq_raised[0]);
        p = zput(p, " attempts=");  p = zhex(p, g_pit_async_attempts);
        p = zput(p, " delivered="); p = zhex(p, g_async_inj_line[0]);
        p = zput(p, " owed_max=");  p = zhex(p, (DWORD)g_pm_tick_owed_max);
        p = zput(p, " ui_gap_us="); p = zhex(p, g_ui_gap_us);
        p = zput(p, "\r\n");
        /* ► THE WHOLE DELIVERY ACCOUNT, IN ONE LINE, IN THE UNITS OF THE CLAIM. `raises`
             is what the 8254 generated; `async` is the SuspendThread arm; `coop` is the
             PM loop's own injections (the #2b latch plus the catch-up batch). Only the
             SUM can be compared with `raises`, and the pit budget line above shows only
             the first of the two -- which is how "we deliver 39% of the ticks Doom asked
             for" was read off an async-only counter. owed_now is the live depth, and the
             histogram behind it says how often the backlog was actually deep. */
        p = zput(p, "STAGE2: isr08 delivery: raises=");  p = zhex(p, g_irq_raised[0]);
        p = zput(p, " async=");   p = zhex(p, g_async_inj_line[0]);
        p = zput(p, " coop=");    p = zhex(p, g_pm_coop_line[0]);
        p = zput(p, " TOTAL=");   p = zhex(p, g_async_inj_line[0] + g_pm_coop_line[0]);
        if (g_irq_raised[0])
            { p = zput(p, " (");
              p = zhex(p, (g_async_inj_line[0] + g_pm_coop_line[0]) * 100u / g_irq_raised[0]);
              p = zput(p, "% of raises, decimal-in-hex)"); }
        p = zput(p, " owed_now="); p = zhex(p, (DWORD)g_pm_tick_owed);
        p = zput(p, " owed_depth_at_sync[0,1,2,3,4-7,8-15,16-31,32-63,64]=");
        { unsigned ob; for (ob = 0; ob < 9; ++ob)
            { p = zput(p, ob ? "," : ""); p = zhex(p, g_pm_owed_hist[ob]); } }
        p = zput(p, "\r\nSTAGE2: coop per IRQ:");
        { unsigned cl; for (cl = 0; cl < 8; ++cl)
            { if (!g_pm_coop_line[cl]) continue;
              p = zput(p, " irq"); p = zhexb(p, cl);
              p = zput(p, "=");    p = zhex(p, g_pm_coop_line[cl]); } }
        p = zput(p, "\r\n");
        /* ► WHICH CLAUSE SAID NO, PER LINE. `attempts` and `delivered` above give the
             shortfall as one subtraction and no reason for it; this names every refusal.
             Read bucket 14 (the CPU thread was in HOST code) against 10 (an injection
             still in flight) and 7/8 (the client has interrupts off) -- they need three
             completely different fixes, and session 23 spent a rig run on the one fix
             that could not have helped any of them. Flushed first, deliberately: `base`
             points past the preamble, so report[] has well under 8 KB of headroom and
             the sbblk ledger below eats most of what is left. */
        { unsigned wl, wc;
          static const char *const whyname[ASYNC_WHY_MAX] = {
            "DELIVERED","badvec","in_pm_irq","pm_noirq","no_catcher","unhooked_pm",
            "no_app_timer","vIF_off","IF_off","arm_quiet","IN_FLIGHT","host_stack",
            "not32","setctx_fail","HOST_CS","?f","?10","?11","?12","?13",
            "not_in_exec","pic_refuse","unhooked","suspend_fail","getctx_fail",
            "v86_IF_off","in_our_hdlr","observed","?1c","?1d","?1e","?1f" };
          log_append(LOG_PATH, base, p); serial_out(base, p); p = base;
          /* NO SILENT CAPS: say how many ASYNC-EARLY lines were written and how many
             were suppressed, so the log's thinness is never read as "it stopped
             happening". The histogram below is the complete account either way. */
          p = zput(p, "STAGE2: async early-bail lines logged=");
          p = zhex(p, g_async_early_bail_logged < ASYNC_EARLY_BAIL_LOG_MAX
                        ? g_async_early_bail_logged : ASYNC_EARLY_BAIL_LOG_MAX);
          p = zput(p, " of ");   p = zhex(p, g_async_early_bail_logged);
          p = zput(p, " (cap "); p = zhex(p, ASYNC_EARLY_BAIL_LOG_MAX);
          p = zput(p, " -- these are FILE I/O UNDER g_lock; see async_early_bail)\r\n");
          for (wl = 0; wl < 8; ++wl) {
              unsigned tot = 0;
              for (wc = 0; wc < ASYNC_WHY_MAX; ++wc) tot += g_async_why_hist[wl][wc];
              if (!tot) continue;
              p = zput(p, "STAGE2: async why irq"); p = zhexb(p, wl);
              p = zput(p, " total=");               p = zhex(p, tot);
              for (wc = 0; wc < ASYNC_WHY_MAX; ++wc) {
                  if (!g_async_why_hist[wl][wc]) continue;
                  p = zput(p, " "); p = zput(p, whyname[wc]);
                  p = zput(p, "="); p = zhex(p, g_async_why_hist[wl][wc]);
              }
              p = zput(p, "\r\n");
              /* Bound against report[] itself, not p - base: see the sbblk loop below. */
              if ((size_t)(p - report) > sizeof report - 512) {
                  log_append(LOG_PATH, base, p); serial_out(base, p); p = base;
              }
          }
          log_append(LOG_PATH, base, p); serial_out(base, p); p = base; }
        /* ► DOES DMX ASK US WHERE THE PLAY HEAD IS? See the note in vdd_dma.h. A
             nonzero rd_addr on the SB's channel means every refill decision the guest
             makes is downstream of our cur_addr, which advances on the audio thread in
             whatever chunk size waveOut asked for. Zero means that whole family of
             causes is dead and the refill is driven by the IRQ count alone. */
        p = zput(p, "STAGE2: 8237 guest reads: ch1_addr="); p = zhex(p, g_dma.rd_addr[1]);
        p = zput(p, " ch1_count=");  p = zhex(p, g_dma.rd_count[1]);
        p = zput(p, " ch5_addr=");   p = zhex(p, g_dma.rd_addr[5]);
        p = zput(p, " ch5_count=");  p = zhex(p, g_dma.rd_count[5]);
        p = zput(p, " status0=");    p = zhex(p, g_dma.rd_status[0]);
        p = zput(p, " status1=");    p = zhex(p, g_dma.rd_status[1]);
        /* ► THE OUTPUT SIDE. See vdd_sb.h: everything else here measures the RING.
             `idle` is silence WE inserted because the DSP was un-armed; the run-length
             buckets say whether that is a scatter of single samples or a gap once per
             block. `cmd` names which transfer command the guest used -- 0x14 single
             (stops every block) vs 0x1C/0xBx auto-init (streams). */
        p = zput(p, "\r\nSTAGE2: sb OUTPUT: active="); p = zhex(p, g_sb.out_active);
        p = zput(p, " idle=");   p = zhex(p, g_sb.out_idle);
        p = zput(p, " paused="); p = zhex(p, g_sb.out_paused);
        { uint32_t tot = g_sb.out_active + g_sb.out_idle + g_sb.out_paused;
          if (tot) { p = zput(p, " (");
                     p = zhex(p, (g_sb.out_idle + g_sb.out_paused) * 100u / tot);
                     p = zput(p, "% of output is inserted silence)"); } }
        p = zput(p, " gap_runs[1,2,4,8,16,32,64,128+]=");
        { int gb; for (gb = 0; gb < 8; ++gb) { p = zput(p, gb ? "," : "");
                                               p = zhex(p, g_sb.idle_runs[gb]); } }
        /* ► ISOLATED silent blocks are the dropouts; long runs are real silence. */
        p = zput(p, " flat_runs[1,2,3,4-7,8-15,16-31,32-63,64+]=");
        { int fb; for (fb = 0; fb < 8; ++fb) { p = zput(p, fb ? "," : "");
                                               p = zhex(p, g_sb.flat_runs[fb]); } }
        /* ► THE QUEUE, WHICH IS WHAT THE SPEAKER ACTUALLY SEES. STARVED>0 means the
             driver ran out of data and played silence -- an audible gap that no
             ring-side counter can show. `drain` is the margin: its mass sitting at
             nbufs-1 is one buffer from silence even when starved reads 0. */
        p = zput(p, " QUEUE: starved="); p = zhex(p, g_wave.starved);
        p = zput(p, " drain_max=");      p = zhex(p, g_wave.drain_max);
        p = zput(p, " wr_fail=");        p = zhex(p, g_wave.underruns);
        p = zput(p, " drain_hist=");
        { uint32_t db; for (db = 0; db <= g_wave.nbufs && db <= AW_BUFFERS; ++db) {
              p = zput(p, db ? "," : ""); p = zhex(p, g_wave.drain_hist[db]); } }
        p = zput(p, " geom: nbufs="); p = zhex(p, g_wave.nbufs);
        p = zput(p, " nframes=");     p = zhex(p, g_wave.nframes);
        p = zput(p, " GATE: on="); p = zhex(p, (DWORD)g_sb.gate_on);
        p = zput(p, " stalled_samples="); p = zhex(p, g_sb.gate_stalled);
        p = zput(p, " FORCED="); p = zhex(p, g_sb.gate_forced);
        if (g_sb.out_active) { p = zput(p, "(stall=");
            p = zhex(p, g_sb.gate_stalled * 1000u / g_sb.out_active);
            p = zput(p, " per mille of output)"); }
        p = zput(p, " mix82=");   p = zhex(p, g_sb.mix82_reads);
        p = zput(p, " ANSWERED_NO="); p = zhex(p, g_sb.mix82_zero);
        if (g_sb.mix82_reads) { p = zput(p, "(");
            p = zhex(p, g_sb.mix82_zero * 100u / g_sb.mix82_reads);
            p = zput(p, "% turned away)"); }
        p = zput(p, " rate_hz="); p = zhex(p, g_sb.rate_hz);
        p = zput(p, " blk_len="); p = zhex(p, g_sb.block_len);
        p = zput(p, " dsp_cmds:");
        { unsigned cc; for (cc = 0; cc < 256; ++cc)
            if (g_sb.cmd_hist[cc]) { p = zput(p, " "); p = zhexb(p, cc);
                                     p = zput(p, "x"); p = zhex(p, g_sb.cmd_hist[cc]); } }
        p = zput(p, "\r\nSTAGE2: sb ");
        /* ► THE GUEST ADDRESS OF EVERY DMA-COUNT POLL. Subtract 0x03AEDFEC for the
             DOOM.EXE file offset and disassemble it. */
        p = zput(p, " dma_poll_sites=");
        { unsigned s; for (s = 0; s < g_dmapoll_n; ++s) {
              p = zput(p, s ? " " : ""); p = zput(p, "0x"); p = zhex(p, g_dmapoll_eip[s]);
              p = zput(p, "x"); p = zhex(p, g_dmapoll_hits[s]); } }
        p = zput(p, " overflow="); p = zhex(p, (DWORD)g_dmapoll_overflow);
        /* ► THE CALL CHAIN. Subtract 0x03AEDFEC for the DOOM.EXE file offset. */
        p = zput(p, " poll_stack=");
        { unsigned s; for (s = 0; s < g_pollstk_n; ++s) {
              p = zput(p, s ? " " : ""); p = zput(p, "0x"); p = zhex(p, g_pollstk[s]);
              p = zput(p, "x"); p = zhex(p, g_pollstk_hits[s]); } }
        p = zput(p, " stkovf="); p = zhex(p, (DWORD)g_pollstk_overflow);
        /* ► WHY ONLY 56 MIXER RUNS/s: long overruns (a) or bunching (b)? */
        p = zput(p, " poll_gap_us[<1k,2k,4k,8k,16k,32k,64k,128k,256k,+]=");
        { unsigned gb; for (gb = 0; gb < 10; ++gb) { p = zput(p, gb ? "," : "");
                                                     p = zhex(p, g_pollgap[gb]); } }
        p = zput(p, " gap_max_us="); p = zhex(p, g_pollgap_max_us);
        /* ── READ DMX'S TASK PERIOD OUT OF THE LIVE GUEST. ──────────────────────────
             Everything about the refill rate is inferred from how often the mixer polls:
             "period 1 tick, overrunning" and "period 2 ticks, working as designed" both
             fit 58 runs/s and need opposite fixes. The scheduler (DOOM.EXE 0x57224)
             walks 32-byte task entries -- +0x00 handler, +0x08 PERIOD, +0x14 next_due,
             +0x1c busy, +0x1e enabled -- so the period is simply there to be read.
           ⚠ DO NOT COMPUTE THE ADDRESS. Composing virtual->guest through the LE object
             table produced a table of noise, and the structural scan written to find it
             instead read unmapped memory and killed the run before STAGE2 finished.
           ► SEARCH FOR THE HANDLER. The mixer's entry is DOOM.EXE file 0x56884, and the
             file->guest delta 0x03AEDFEC is verified twice over (DMX's IRQ0 stub and
             Doom's keyboard ISR, and DMXCHK re-checks it in-run), so the task entry is
             whatever 32 bytes begin with that pointer. No data-address arithmetic at
             all, and a hit is self-verifying.
             Walk only COMMITTED, READABLE regions and stop 32 bytes short of each one's
             end -- that is what the previous attempt got wrong. */
        { const DWORD mixer = 0x57224u + 0x03AEDFECu;   /* DMX IRQ0 handler = table[0] */
          const volatile BYTE *stub = (const volatile BYTE *)(ULONG_PTR)0x03b431f0;
          MEMORY_BASIC_INFORMATION mb;
          ULONG_PTR a = 0x00010000u, lim = 0x7ff00000u;   /* whole user space */
          unsigned found = 0, t;
          p = zput(p, " DMXCHK=");
          for (t = 0; t < 5; ++t) p = zhexb(p, stub[t]);       /* expect 601e060fa0 */
          p = zput(p, " mixer=0x"); p = zhex(p, mixer);
          while (a < lim && found < 3) {
              ULONG_PTR base, end, q;
              int readable;
              if (VirtualQuery((LPCVOID)a, &mb, sizeof mb) != sizeof mb) break;
              base = (ULONG_PTR)mb.BaseAddress; end = base + mb.RegionSize;
              readable = (mb.State == MEM_COMMIT)
                       && (mb.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY
                                       | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE
                                       | PAGE_EXECUTE_WRITECOPY))
                       && !(mb.Protect & PAGE_GUARD);
              if (readable) {
                  for (q = base; q + 16u*36u <= end && found < 3; q += 4) {
                      const volatile DWORD *e = (const volatile DWORD *)q;
                      if (e[0] != mixer) continue;
                      /* ⚠ DUMP RAW, DO NOT INTERPRET. The first attempt read
                           [+8] as the period and got 140 -- which at a ~135/s clock
                           means one run a SECOND against 58 observed, so either the
                           match is spurious or the field offsets are wrong. Print the
                           bytes and decide offline; a guessed layout is how this
                           session has already produced two counters that could not
                           have contradicted themselves. 64 bytes = two table entries,
                           so a real table shows a second handler pointer at +32. */
                      /* ── THE DISPATCHER'S HANDLER TABLE. ───────────────────────
                           DOOM.EXE 0x554f4 calls [eax*4 + 0x281ac] with eax = irq*9,
                           i.e. a 36-byte stride from a base whose ADDRESS is an LE
                           fixup and therefore absent from the image. But entry 0 is
                           DMX's IRQ0 handler and that address IS known -- so the base
                           is wherever that pointer lies, and every other IRQ's handler
                           follows at +36. No address arithmetic, self-verifying.
                           IRQ5 is the Sound Blaster: 81 block IRQs a second are
                           delivered and only 58 reach the mixer, so its handler is
                           what decides the missing 28%. Subtract 0x03AEDFEC from the
                           printed value for the file offset to disassemble. */
                      unsigned iq;
                      if (q + 16u * 36u > end) continue;   /* table must fit */
                      ++found;
                      p = zput(p, " IRQTAB@0x"); p = zhex(p, (DWORD)q);
                      for (iq = 0; iq < 16; ++iq) {
                          DWORD h = *(const volatile DWORD *)(q + iq * 36u);
                          if (!h) continue;
                          p = zput(p, " i"); p = zhexb(p, iq);
                          p = zput(p, "=0x"); p = zhex(p, h);
                      }
                  }
              }
              if (end <= base) break;
              a = end;
          }
          if (!found) p = zput(p, " IRQTAB-NOT-FOUND");
          /* ── THE CALLBACK BETWEEN THE SB ISR AND THE MIXER. ─────────────────────
               DMX's SB handler ends with `call dword [0x584]` (DOOM.EXE 0x53298) and
               that callback is where the 28% goes: the ISR runs 86/s (mix82 reads ==
               blocks) but the mixer is entered 58/s. Its address is runtime data, and
               the code virtual->file map is the one map still unknown.
               But the IRQ table settles the addressing: the pointers IN it are LINEAR
               (entry 0 read back as 0x03b45210, exactly file 0x57224 + 0x03AEDFEC),
               while data operands like 0x281ac needed +0x03BA0000 to be found. So CS
               is flat at base 0 and DS is not -- and a code pointer STORED in data is
               therefore directly a linear address. Read it and subtract 0x03AEDFEC.
               Also read DMX's own state word at data 0x26370, which its ISR compares
               against 1 before doing anything at all. */
          { const volatile DWORD *cb  = (const volatile DWORD *)(ULONG_PTR)(0x584u   + 0x03BA0000u);
            const volatile DWORD *stt = (const volatile DWORD *)(ULONG_PTR)(0x26370u + 0x03BA0000u);
            MEMORY_BASIC_INFORMATION mq;
            if (VirtualQuery((LPCVOID)cb, &mq, sizeof mq) == sizeof mq && mq.State == MEM_COMMIT) {
                DWORD v = *cb;
                p = zput(p, " CB[0x584]=0x"); p = zhex(p, v);
                if (v > 0x03AEDFECu && v < 0x03AEDFECu + 0x45000u) {
                    p = zput(p, "=file0x"); p = zhex(p, v - 0x03AEDFECu);
                } else p = zput(p, "(not a linear code addr)");
            }
            if (VirtualQuery((LPCVOID)stt, &mq, sizeof mq) == sizeof mq && mq.State == MEM_COMMIT) {
                p = zput(p, " STATE[0x26370]="); p = zhex(p, *stt);
            } } }
        p = zput(p, " count_rd_by_width w1="); p = zhex(p, g_dma.rd_w1);
        p = zput(p, " w2=");                   p = zhex(p, g_dma.rd_w2);
        p = zput(p, " w4=");                   p = zhex(p, g_dma.rd_w4);
        /* ...and how many of those reads DMX made from inside a COOPERATIVE tick. */
        p = zput(p, " from_coop_isr08=");      p = zhex(p, g_coop_dma_polls);
        { unsigned dq; for (dq = 2; dq < 8; ++dq)
            { if (!g_coop_dma_polls_dev[dq]) continue;
              p = zput(p, " from_coop_irq"); p = zhexb(p, dq);
              p = zput(p, "=");              p = zhex(p, g_coop_dma_polls_dev[dq]); } }
        p = zput(p, " in_async_isr=");  p = zhex(p, g_dmapoll_in_async);
        p = zput(p, " mainline=");      p = zhex(p, g_dmapoll_mainline);
        p = zput(p, "\r\nSTAGE2: devirq (cooperative PM retry): inj=");
        p = zhex(p, g_pm_devirq_inj);
        p = zput(p, " fail=");  p = zhex(p, g_pm_devirq_fail);
        p = zput(p, " dropped_unhooked="); p = zhex(p, g_pm_devirq_drop);
        p = zput(p, "\r\nSTAGE2: sound2: ");
        p = zput(p, " mpu_uart=");                p = zhex(p, (DWORD)g_mpu.uart_mode);
        p = zput(p, " host_wave=");               p = zput(p, g_wave.silent ? "SILENT" : "open");
        p = zput(p, " host_midi=");               p = zput(p, g_wave.hmidi ? "open" : "NONE");
        p = zput(p, " underruns=");               p = zhex(p, g_wave.underruns);
        p = zput(p, "\r\n");
        /* ► THE BLOCK-BOUNDARY LEDGER (see the sb_blkrec comment in vdd_sb.h). One line
             per completed block for the first few: where the capture stood, what the
             8237 held, and whether it had wrapped. cap_off is the load-bearing column --
             it turns sbref.py's INFERRED 128-frame grid into measured boundaries, so
             "the jump is two frames in" can be checked against fact instead of against
             our own guess at where a block starts. */
        if (g_sb.blklog_n) {
            uint32_t bi;
            p = zput(p, "STAGE2: sound blocks: n="); p = zhex(p, g_sb.blocks);
            p = zput(p, " logged="); p = zhex(p, g_sb.blklog_n);
            p = zput(p, " (cap_off block_len phys count base_addr base_count page mode)\r\n");
            for (bi = 0; bi < g_sb.blklog_n && bi < SB_BLKLOG_MAX; ++bi) {
                const struct sb_blkrec *b = &g_sb.blklog[bi];
                p = zput(p, "STAGE2: sbblk "); p = zhexb(p, bi);
                p = zput(p, " cap_off=");    p = zhex(p, b->cap_off);
                p = zput(p, " blk_len=");    p = zhex(p, b->block_len);
                p = zput(p, " phys=");       p = zhex(p, b->phys);
                p = zput(p, " count=");      p = zhex(p, b->cur_count);
                p = zput(p, " base=");       p = zhex(p, b->base_addr);
                p = zput(p, "/");            p = zhex(p, b->base_count);
                p = zput(p, " page=");       p = zhexb(p, b->page);
                p = zput(p, " mode=");       p = zhexb(p, b->mode);
                p = zput(p, b->reloaded ? " WRAPPED" : " mid-ring");
                if (b->ended) p = zput(p, " ENDED");
                p = zput(p, "\r\n");
                /* `base` points PAST the preamble, not at report[0], so bound against
                   the array itself -- p - base would let this overrun by the preamble's
                   length. */
                if (p > report + sizeof report - 512) {
                    log_append(LOG_PATH, base, p); serial_out(base, p); p = base;
                }
            }
            /* Hand the rest of STAGE2 an empty buffer: the ledger is the biggest single
               block in this report and everything after it was running on fumes. */
            log_append(LOG_PATH, base, p); serial_out(base, p); p = base;
        }
        p = zput(p, "STAGE2: opl: trace=");   p = zhex(p, g_opltrace_n);
        p = zput(p, " tdrop=");               p = zhex(p, g_opltrace_drop);
        p = zput(p, "\r\n");
        p = zput(p, "STAGE2: opl: writes=");  p = zhex(p, g_opl.prof_writes);
        p = zput(p, " keyons=");              p = zhex(p, g_opl.prof_keyons);
        p = zput(p, " bd_writes=");           p = zhex(p, g_opl.prof_bd_writes);
        p = zput(p, " bd_or=");               p = zhexb(p, g_opl.prof_bd_or);
        p = zput(p, " keyon_am=");            p = zhex(p, g_opl.prof_keyon_am);
        p = zput(p, " keyon_vib=");           p = zhex(p, g_opl.prof_keyon_vib);
        p = zput(p, " am_ops=");              p = zhex(p, g_opl.prof_am_ops);
        p = zput(p, " vib_ops=");             p = zhex(p, g_opl.prof_vib_ops);
        p = zput(p, " waves=");               p = zhexb(p, g_opl.prof_wave_mask);
        p = zput(p, " wse=");                 p = zhexb(p, g_opl.prof_wse);
        p = zput(p, "\r\n");
        /* PERCUSSION, BY EDGE COUNT. bd_or above is an OR over the whole run and
           cannot tell "set once at init" from "drums play throughout" -- it once
           produced a confident wrong answer about exactly this register. These are
           key-on edges per voice, and the last three are NOT SYNTHESISED YET, so
           this doubles as their loud-failure report: the run says how many hits it
           could not play rather than going quietly silent. */
        p = zput(p, "STAGE2: opl rhythm: bassdrum="); p = zhex(p, g_opl.prof_rhythm_hits[4]);
        p = zput(p, " tomtom=");                      p = zhex(p, g_opl.prof_rhythm_hits[2]);
        p = zput(p, "  NOT SYNTHESISED hihat=");      p = zhex(p, g_opl.prof_rhythm_hits[0]);
        p = zput(p, " snare=");                       p = zhex(p, g_opl.prof_rhythm_hits[3]);
        p = zput(p, " cymbal=");                      p = zhex(p, g_opl.prof_rhythm_hits[1]);
        p = zput(p, "\r\n");
        p = zput(p, "STAGE2: vsync: vbl_edges="); p = zhex(p, g_vid.vbl_edges);
        p = zput(p, " p3da_reads=");             p = zhex(p, g_vid.p3da_reads);
        p = zput(p, " run_ms=");                 p = zhex(p, GetTickCount() - g_run_start_tick);
        p = zput(p, "\r\n");
        /* ► Is mode Y actually in use? The whole unchained theory rests on Doom
             clearing Sequencer reg 4 bit 3, which was INFERRED from a pixel pattern
             (80-px period, 50 rows) and never observed directly. Print the register. */
        p = zput(p, "STAGE2: video now: chain4="); p = zhexb(p, g_vid.chain4);
        p = zput(p, " ymask="); p = zhexb(p, g_vid.y_mask);
        p = zput(p, " mkind="); p = zhexb(p, g_vid.mkind);
        p = zput(p, " gw="); p = zhex(p, g_vid.gw); p = zput(p, " gh="); p = zhex(p, g_vid.gh);
        p = zput(p, " mapmask="); p = zhexb(p, g_vid.map_mask);
        p = zput(p, " setreset="); p = zhexb(p, g_vid.set_reset);
        p = zput(p, " ensr="); p = zhexb(p, g_vid.enable_sr);
        p = zput(p, " wmode="); p = zhexb(p, g_vid.write_mode);
        p = zput(p, " plane-nonzero=");
        for (pl = 0; pl < 4; ++pl) { p = zhex(p, nz[pl]); p = zput(p, pl<3?"/":""); }
        p = zput(p, "\r\n"); }
      { const volatile BYTE *z0 = (const volatile BYTE *)0;
        DWORD cs2 = VDM_REG(tib, VTIB_CS) & 0xFFFF, ip2 = VDM_REG(tib, VTIB_EIP) & 0xFFFF;
        const volatile BYTE *cd = (const volatile BYTE *)((cs2 << 4) + ip2);
        unsigned k3;
        p = zput(p, "STAGE2: ivt08="); p = zhex(p, (DWORD)z0[0x22] | ((DWORD)z0[0x23] << 8));
        p = zput(p, ":"); p = zhex(p, (DWORD)z0[0x20] | ((DWORD)z0[0x21] << 8));
        p = zput(p, " ivt1C="); p = zhex(p, (DWORD)z0[0x72] | ((DWORD)z0[0x73] << 8));
        p = zput(p, ":"); p = zhex(p, (DWORD)z0[0x70] | ((DWORD)z0[0x71] << 8));
        p = zput(p, " at-cs:ip=");
        for (k3 = 0; k3 < 8; ++k3) { p = zhexb(p, cd[k3]); p = zput(p, " "); }
        p = zput(p, " asyncinj="); p = zhex(p, g_async_inj);
        p = zput(p, " interp-refused="); p = zhex(p, g_interp_refused);
        p = zput(p, " p12-batches="); p = zhex(p, g_p12_batches);
        p = zput(p, " p12-instrs=");  p = zhex(p, g_p12_instrs);
        p = zput(p, " p12-bails=");   p = zhex(p, g_p12_bails);
        p = zput(p, " bail="); p = zhex(p, g_async_bail);
        p = zput(p, " nestblk="); p = zhex(p, g_async_nest_blocked);
        p = zput(p, " asyncsites="); p = zhex(p, (DWORD)g_async_nsite);
        p = zput(p, (g_async_site_full ? "(FULL)" : ""));
        p = zput(p, " pmstretch_max_us="); p = zhex(p, g_pm_stretch_max_us);
        p = zput(p, "\r\n"); }
      p = zput(p, "STAGE2: mode sets:");
      for (i = 0; i < g_vid.mode_qn; ++i) {
          p = zput(p, " mode=0x"); p = zhexb(p, g_vid.mode_q[i].mode);
          p = zput(p, "/kind="); p = zhexb(p, g_vid.mode_q[i].kind);
          p = zput(p, "/"); p = zhex(p, g_vid.mode_q[i].w);
          p = zput(p, "x"); p = zhex(p, g_vid.mode_q[i].h);
      }
      if (!g_vid.mode_qn) p = zput(p, " none");
      p = zput(p, "\r\n");
      /* ► THE MODE-Y ARRAYS, NOT THE PLANAR ONES. "plane-nonzero" above counts
           g_vid.plane[] -- the 16-colour planar buffer, which an unchained 256-colour
           mode never touches -- so it has reported four zeroes for every mode-Y run
           ever made and told us nothing. These are the arrays a mode-Y frame is
           actually built from, plus the map-mask values the program really used. */
      p = zput(p, "STAGE2: modeY remap="); p = zhex(p, (DWORD)g_yremap);
      p = zput(p, " swaps="); p = zhex(p, g_yswaps);
      p = zput(p, " fanouts="); p = zhex(p, g_yfanouts);
      p = zput(p, " failed="); p = zhex(p, g_yfail);
      /* ► ARE THE PLANES COLLAPSED, OR DOES THE RENDER COLLAPSE THEM? The oracle says
           62% of the status bar's four-pixel groups hold one value where the reference
           holds four. That can only come from the four PLANES agreeing, or from the
           render reading one plane four times. Ask the planes directly, over the bar's
           own offsets in the page the CRTC is displaying. If they disagree here and the
           screen shows agreement, the fault is downstream of the planes. */
      if (g_yremap) {
          /* ► PER PAGE, because "we are displaying the wrong buffer" and "the buffer is
               wrong" look identical from one page. Doom triple-buffers at 0, 0x4000 and
               0x8000; if one page's bar is intact and the one the CRTC points at is not,
               the fault is in following the page flip, not in the writes. */
          uint32_t pg;
          p = zput(p, " bar_planes_equal_per_page:");
          for (pg = 0; pg < 3; ++pg) {
              uint32_t o, eq = 0, tot = 0, base = pg * 0x4000u;
              for (o = base + 168u * 80u; o < base + 200u * 80u; ++o) {
                  uint32_t m = o & (MODEY_WIN - 1u);
                  BYTE a = ((BYTE *)g_yview[0])[m], b = ((BYTE *)g_yview[1])[m];
                  BYTE c = ((BYTE *)g_yview[2])[m], d2 = ((BYTE *)g_yview[3])[m];
                  ++tot; if (a == b && b == c && c == d2) ++eq;
              }
              p = zput(p, " p"); p = zhexb(p, pg); p = zput(p, "=");
              p = zhex(p, eq); p = zput(p, "/"); p = zhex(p, tot);
          }
      }
      /* ► THE FAN-OUT'S OWN CONTRIBUTION TO THE COLLAPSE, by row band. Compare
           `distinct` against bar_planes_equal above: near it means this path IS the
           four-way collapse; near zero exonerates it properly. Band A is rows 168-183,
           which no write-mode-1 burst ever reaches and which session 23 measured as the
           WORSE half; band B is 184-199, where every burst lands. */
      p = zput(p, " fanout_bar[A=rows168-183,B=184-199]: writes=");
      p = zhex(p, g_yfan_bar_writes[0]); p = zput(p, "/"); p = zhex(p, g_yfan_bar_writes[1]);
      p = zput(p, " distinct=");
      p = zhex(p, g_yfan_bar_distinct[0]); p = zput(p, "/"); p = zhex(p, g_yfan_bar_distinct[1]);
      p = zput(p, " of "); p = zhex(p, YBAR_OFF_MID - YBAR_OFF_LO);
      p = zput(p, "/");    p = zhex(p, YBAR_OFF_HI - YBAR_OFF_MID);
      p = zput(p, " per page, 4way=");
      p = zhex(p, g_yfan_bar_4way[0]); p = zput(p, "/"); p = zhex(p, g_yfan_bar_4way[1]);
      /* ► DOES THE GUEST WRITE THE SAME BYTES TO DIFFERENT PLANES? See ysmp_check().
           Read `eqb` (PER-BYTE agreement), not `cross_same` (per-window, kept only so
           the old number stays comparable and visibly useless). Compare eqb against the
           two figures printed above and by bandprof.py:
             ~67%  matches bar_planes_equal  => the planes are RECEIVING collapsed data
                                                and the fault is upstream of them
             ~12%  matches an intact bar     => they receive distinct data and something
                                                downstream collapses it
           p1eq is the same rate against PLANE 1's last window specifically, per plane,
           because the hypothesis names plane 1. p1eq[1] is the self-baseline: how much
           plane 1 agrees with its own previous content, i.e. how much of this window is
           static anyway. A p1eq[0/2/3] near p1eq[1] is the collapse; well below it is
           not. All rates are percent, printed in hex. */
      { int bnd; for (bnd = 0; bnd < 2; ++bnd) {
          int pq;
          p = zput(p, bnd ? " ysmpB[184-199]:" : " ysmpA[168-183]:");
          p = zput(p, " writes="); p = zhex(p, g_ysmp_writes[bnd]);
          p = zput(p, " cross_same="); p = zhex(p, g_ysmp_cross_same[bnd]);
          p = zput(p, " cross_diff="); p = zhex(p, g_ysmp_cross_diff[bnd]);
          p = zput(p, " cross_eqb=");
          p = zhex(p, g_ysmp_cross_eqb[bnd]); p = zput(p, "/");
          p = zhex(p, g_ysmp_cross_totb[bnd]);
          if (g_ysmp_cross_totb[bnd]) {
              p = zput(p, "(");
              p = zhex(p, g_ysmp_cross_eqb[bnd] * 100u / g_ysmp_cross_totb[bnd]);
              p = zput(p, "% STATE-not-delivery)");
          }
          /* ► THE DELIVERY RATE -- CHANGED BYTES ONLY. This is the one to read:
               high => the guest handed the same byte to two different planes. */
          p = zput(p, " delivered_eq=");
          p = zhex(p, g_ysmp_dlv_eq[bnd]); p = zput(p, "/");
          p = zhex(p, g_ysmp_dlv_tot[bnd]);
          if (g_ysmp_dlv_tot[bnd]) {
              p = zput(p, "(");
              p = zhex(p, g_ysmp_dlv_eq[bnd] * 100u / g_ysmp_dlv_tot[bnd]);
              p = zput(p, "%)");
          }
          p = zput(p, " p1eq=");
          for (pq = 0; pq < 4; ++pq) {
              p = zput(p, pq ? "/" : "");
              if (g_ysmp_p1_tot[bnd][pq])
                  p = zhex(p, g_ysmp_p1_eq[bnd][pq] * 100u / g_ysmp_p1_tot[bnd][pq]);
              else p = zput(p, "-");
          }
          p = zput(p, "% n=");
          for (pq = 0; pq < 4; ++pq) {
              p = zput(p, pq ? "/" : "");
              p = zhex(p, g_ysmp_p1_tot[bnd][pq] / YSMP_LEN);
          } } }
      /* ► THE MAP-MASK IDENTITY. See g_ysel_calls. Both lines must balance exactly;
           a residual is a path nobody has accounted for. */
      { DWORD mw = 0, resid;
        for (i = 0; i < 16; ++i) mw += g_vid.mask_hist[i];
        /* ⚠ `skip_same` LEFT THIS IDENTITY WHEN THE GR4 FIX LANDED. A map-mask write
             whose value is unchanged now still calls select -- it has to, because a read
             may have moved the window since -- so it is no longer a bucket that
             ACCOUNTS for a write, just a note about how many writes were redundant.
             Leaving it in the sum printed a **UNACCOUNTED** residual of exactly
             -skip_same, which is a counter describing the code as it used to be. */
        p = zput(p, " maskacct: writes="); p = zhex(p, mw);
        p = zput(p, " = sel_calls="); p = zhex(p, g_ysel_calls);
        p = zput(p, " - c4sel="); p = zhex(p, g_vid.chain4_sel);
        p = zput(p, " + skip_chain4="); p = zhex(p, g_vid.mask_skip_chain4);
        p = zput(p, " [redundant_same="); p = zhex(p, g_vid.mask_skip_same);
        p = zput(p, ", informational]");
        resid = mw - (g_ysel_calls - g_vid.chain4_sel) - g_vid.mask_skip_chain4;
        p = zput(p, " residual="); p = zhex(p, resid);
        p = zput(p, resid ? " **UNACCOUNTED**" : " (balanced)");
        p = zput(p, " | sel_calls = swaps="); p = zhex(p, g_yswaps);
        p = zput(p, " + sel_same="); p = zhex(p, g_ysel_same);
        p = zput(p, " + sel_zero="); p = zhex(p, g_ysel_zero);
        p = zput(p, " + failed="); p = zhex(p, g_yfail);
        resid = g_ysel_calls - g_yswaps - g_ysel_same - g_ysel_zero - g_yfail;
        p = zput(p, " residual="); p = zhex(p, resid);
        p = zput(p, resid ? " **UNACCOUNTED**" : " (balanced)"); }
      /* ► THE READ PLANE. See modey_remap_readmap(). `mismatch` counts GR4 writes that
           named a plane other than the one mapped at A0000 -- every guest read between
           such a write and the next mask change returns the WRONG PLANE'S BYTES, and no
           write-side instrument can see it. `pair` is the (GR4, mapped) matrix, so a
           mismatch can be attributed rather than just counted: a column concentrated on
           one mapped plane means the guest cycled GR4 while the window sat still, which
           is exactly the I_ReadScreen shape. Section 4 is linear, 5 is the scratch. */
      p = zput(p, " gr4: writes="); p = zhex(p, g_ygr4_calls);
      p = zput(p, " mismatch="); p = zhex(p, g_ygr4_mismatch);
      p = zput(p, " WINDOW_MOVES="); p = zhex(p, g_ygr4_moves);
      p = zput(p, " hist=");
      for (i = 0; i < 4; ++i) { p = zput(p, i ? "/" : ""); p = zhex(p, g_vid.gr4_hist[i]); }
      p = zput(p, " pair[gr4->mapped]:");
      { unsigned a2, b2;
        for (a2 = 0; a2 < 4; ++a2)
          for (b2 = 0; b2 < 6; ++b2)
            if (g_ygr4_pair[a2][b2]) {
                p = zput(p, " r"); p = zhexb(p, a2);
                p = zput(p, "->m"); p = zhexb(p, b2);
                p = zput(p, "="); p = zhex(p, g_ygr4_pair[a2][b2]); } }
      /* ► THE ONE THAT DECIDES IT. GR4 writes between consecutive mask changes:
           1 = the ordinary blit (window moves before any read -- harmless)
           4 = a PURE READ PASS with the window stranded (the collapse)          */
      p = zput(p, " gr4_runs[n GR4 per select]:");
      { unsigned r2; for (r2 = 1; r2 < 10; ++r2)
          if (g_ygr4_runs[r2]) { p = zput(p, " "); p = zhexb(p, r2);
                                 p = zput(p, "x"); p = zhex(p, g_ygr4_runs[r2]); } }
      p = zput(p, " stranded_on_plane=");
      for (i = 0; i < 4; ++i) { p = zput(p, i ? "/" : ""); p = zhex(p, g_ygr4_run_planes[i]); }
      /* ► IS THE LINEAR SECTION EVEN OCCUPIED? One number, ahead of the dump: how many
           of the bar region's 10240 linear bytes are non-zero. Zero means nothing was
           ever written to A0000 while the window pointed at the linear section, and the
           candidate dies here without parsing anything. */
      { uint32_t o2, nz = 0; const BYTE *lv = (const BYTE *)g_yview[4];
        for (o2 = 168u * 320u; o2 < 200u * 320u; ++o2)
            if (lv[o2 & (MODEY_WIN - 1u)]) ++nz;
        p = zput(p, " linear_bar_nonzero="); p = zhex(p, nz);
        p = zput(p, "/"); p = zhex(p, 32u * 320u); }
      p = zput(p, " latch_solved="); p = zhex(p, g_ylatch_ok);
      p = zput(p, " latch_UNSOLVED="); p = zhex(p, g_ylatch_unsolved);
      p = zput(p, " gap="); p = zhex(p, g_vid.modey_gap);
      p = zput(p, " attributed="); p = zhex(p, g_vid.ynz[0]);
      p = zput(p, " crtc_seen="); p = zhexb(p, g_vid.crtc_seen);
      p = zput(p, " crtc_start=0x"); p = zhex(p, g_vid.crtc_start);
      p = zput(p, "\r\n");
      p = zput(p, "STAGE2: modeY snaps:");
      for (i = 0; i < 4; ++i) {
          p = zput(p, " p"); p = zhexb(p, (unsigned)i);
          p = zput(p, "="); p = zhex(p, g_vid.ysnap[i]);
          p = zput(p, "/nz="); p = zhex(p, g_vid.ynz[i]);
      }
      p = zput(p, " wmode hist:");
      for (i = 0; i < 4; ++i) { p = zput(p, " "); p = zhexb(p, (unsigned)i);
                                p = zput(p, "x"); p = zhex(p, g_vid.wmode_hist[i]); }
      p = zput(p, "\r\nSTAGE2: modeY (wmode,mask) pairs:");
      { unsigned wm, mk;
        for (wm = 0; wm < 4; ++wm)
          for (mk = 0; mk < 16; ++mk)
            if (g_vid.mw_hist[wm * 16 + mk]) {
                p = zput(p, " w"); p = zhexb(p, wm);
                p = zput(p, "/m"); p = zhexb(p, mk);
                p = zput(p, "="); p = zhex(p, g_vid.mw_hist[wm * 16 + mk]); } }
      p = zput(p, "\r\nSTAGE2: modeY mapmask hist:");
      for (i = 0; i < 16; ++i)
          if (g_vid.mask_hist[i]) { p = zput(p, " 0x"); p = zhexb(p, (unsigned)i);
                                    p = zput(p, "x"); p = zhex(p, g_vid.mask_hist[i]); }
      p = zput(p, "\r\n");
      p = zput(p, "STAGE2: video modes unsupported:");
      for (i = 0, n = 0; i < 256; ++i)
          if (VID_UNIMPL_GET(g_vid.unimpl_mode, i)) { p = zput(p, " 0x"); p = zhexb(p, (unsigned)i); ++n; }
      if (!n) p = zput(p, " none");
      p = zput(p, "\r\n"); }
    /* ── DUMP THE BAR'S FOUR PLANES SO THE WAD CAN JUDGE THEM. ──────────────────────
         Everything measured so far describes the SCREEN, and the screen is planes plus
         a render. The oracle can only say "this pixel is wrong"; it cannot say which
         plane holds the right byte, because by then the four have been interleaved.
       ► WHAT THIS SETTLES. Measured against the IWAD on the last run's captures:
         plane 1's columns are 70.3% correct and planes 0/2/3 are 33.6/29.3/27.3%, and
         "plane 1 replicated across the group" explains 63.0% of bar pixels against a
         40.1%-correct baseline. So plane 1's content is reaching the other three. What
         no capture can distinguish is whether 0/2/3 hold a LITERAL COPY of plane 1
         (one writer smearing) or their own damaged content that merely resembles it
         (a per-plane fault). Comparing the planes to each other answers that, and the
         answer picks between two completely different fixes.
       ► ALSO REFUTED, AND WHY THIS IS NOT THE LATCH DUMP IT LOOKS LIKE: the
         write-mode-1 bursts only ever touch plane offsets 0x3a1c..0x3e7f, i.e. rows
         186..199. Rows 168..185, which no burst reaches, are MORE wrong (62.2% against
         56.9%). The latch copy is not the status bar's cause; do not rebuild the A0000
         trap on the strength of session 22's note. See build/barprof.py.
         All three pages, because the pages have been equal to the digit before and
         that is itself a fact worth re-checking. Mode-Y runs only; ~67 KB, one shot. */
    if (g_yremap && g_vid.mkind == VID_KIND_LINEAR8 && !g_vid.chain4) {
        uint32_t pg, pl, row;
        char lb[220], *lq;
        log_append(LOG_PATH, base, p); serial_out(base, p); p = base;  /* keep the log in order */
        lq = lb; lq = zput(lq, "MODEYBAR dump: 3 pages x 4 planes x rows 168..199, "
                               "80 bytes/row (plane offset = row*80 + x/4)\r\n");
        log_append(LOG_PATH, lb, lq); serial_out(lb, lq);
        for (pg = 0; pg < 3; ++pg)
            for (pl = 0; pl < 4; ++pl)
                for (row = 168; row < 200; ++row) {
                    uint32_t o = (pg * 0x4000u + row * 80u) & (MODEY_WIN - 1u), x;
                    const BYTE *src = (const BYTE *)g_yview[pl];
                    lq = lb;
                    lq = zput(lq, "MODEYBAR pg"); lq = zhexb(lq, pg);
                    lq = zput(lq, " pl");         lq = zhexb(lq, pl);
                    lq = zput(lq, " y");          lq = zhexb(lq, row);
                    lq = zput(lq, " ");
                    for (x = 0; x < 80; ++x) lq = zhexb(lq, src[(o + x) & (MODEY_WIN - 1u)]);
                    lq = zput(lq, "\r\n");
                    /* File only: 67 KB down a 115200 COM1 is ~6 s of wind-down for a
                       dump nobody reads off the serial line. */
                    log_append(LOG_PATH, lb, lq);
                }
        /* ── AND THE LINEAR SECTION, WHICH IS THE ONE PLACE NOBODY HAS LOOKED. ────────
             modey_remap_init() sets g_ycur = 4 and a chain4 change selects 4, so A0000
             maps g_ysec[4] -- NOT any plane -- both before the first map-mask write and
             for as long as the guest stays chained. Anything the guest writes to A0000
             in either window lands here and is invisible to all four planes, for good.
             That is the exact shape the evidence demands: the bar is wrong from the
             FIRST frame and flat afterwards, the fully-redrawn surfaces (title screen
             0-of-64000, the 3D view) are perfect, and every "what corrupts it during
             play" candidate has come back excluded. Write-once damage needs a
             write-once mechanism, and this is one.
             In chained mode the byte at offset o IS pixel (o%320, o/320), so the bar
             region is rows 168..199 at 320 bytes a row -- no plane stride. Score it
             against STBAR directly: a good score means Doom drew the bar while the
             window pointed here and the planes never received it. */
        lq = lb; lq = zput(lq, "MODEYLIN dump: linear section, rows 168..199, "
                               "320 bytes/row in 4 chunks (offset = row*320 + x)\r\n");
        log_append(LOG_PATH, lb, lq); serial_out(lb, lq);
        for (row = 168; row < 200; ++row) {
            uint32_t q4, x;
            for (q4 = 0; q4 < 4; ++q4) {
                uint32_t o = (row * 320u + q4 * 80u) & (MODEY_WIN - 1u);
                const BYTE *src = (const BYTE *)g_yview[4];
                lq = lb;
                lq = zput(lq, "MODEYLIN y"); lq = zhexb(lq, row);
                lq = zput(lq, " q");         lq = zhexb(lq, q4);
                lq = zput(lq, " ");
                for (x = 0; x < 80; ++x) lq = zhexb(lq, src[(o + x) & (MODEY_WIN - 1u)]);
                lq = zput(lq, "\r\n");
                log_append(LOG_PATH, lb, lq);
            }
        }
    }
    p = zput(p, "STAGE2: complete\r\n");
    log_append(LOG_PATH, base, p); serial_out(base, p); p = base;   /* headless: mirror the DOS-output flush + completion to COM1 */

    /* Headless test mode: the guest has just terminated (or the PM loop hit its
       headless time cap), so exit immediately (no window-close needed) -- this lets a
       test harness's `start /wait` return and the log be collected. Consume the marker
       so it's one-shot; interactive runs (no marker) keep the window open. */
    if (g_headless) {
        DeleteFileA(AUTOEXIT_PATH);
        ExitProcess(0);
    }
    /* Keep the Luna window open so the guest's final screen stays visible until
       the user closes it; then the UI thread's message loop returns. */
    if (ui) { WaitForSingleObject(ui, INFINITE); CloseHandle(ui); }
    return 0;
}
