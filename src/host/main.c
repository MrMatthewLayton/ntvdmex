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
static void irq0_latch(void)
{
    if (g_irq0_pending < IRQ0_PENDING_MAX) InterlockedIncrement(&g_irq0_pending);
}
static volatile LONG g_irq1_pending = 0;    /* count of un-delivered keyboard IRQ1s (one per scancode byte) */
static int g_pm_irq0_latch = 0;             /* #2b: a virtual IRQ0 awaiting injection into the PM hook */
static int g_in_pm_irq     = 0;             /* #2b: re-entrancy guard while inside an injected PM ISR   */
static CRITICAL_SECTION g_lock;             /* serialises all bus dispatch       */
static HWND         g_hwnd;
static HANDLE       g_key_event;            /* signalled when a key is pushed     */
static volatile LONG g_running = 1;         /* 0 once the window is closed         */
static int g_dpmi_pm = 0;                   /* set once the guest is switched to PM (spike) */
static DWORD g_dpmi_code_base = 0;          /* linear base of the guest PM code seg (retcs<<4) */
static BYTE  g_int_vec[0x10000];            /* per-CS-offset: original INT vector we patched to a BOP */

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

static int            g_capture       = 0;  /* CAPTURE_FLAG present: opt-in self-screenshot for graphical tests */
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
static struct { WORD sel; DWORD off; } g_pm_int[256];
static int   g_dpmi_vi = 1;                 /* DPMI virtual interrupt flag (INT 31h 0900/0901/0902) */
/* DPMI 0303 real-mode callbacks: each slot records the client's PM handler (sel:off)
   and the RMCS buffer (sel:off) to marshal register state through. g_pmret_sel is a
   code selector based at DOS_HDLR_SEG (0x500) so the PM handler's IRET lands on the
   planted DPMI_PMRET catcher; allocated lazily on the first 0303. */
static struct { WORD pm_sel; DWORD pm_off; WORD rm_es; DWORD rm_di; int used; } g_cb[DPMI_CB_SLOTS];
static BYTE g_bios_unimpl[256];   /* GH #27: BIOS services a run actually wanted */
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
static struct dpmi_desc { DWORD base, limit; BYTE access, flags; } g_ldt[512];
static int   g_ldt_next = 3;
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
static int async_inject_irq(unsigned irq)
{
    CONTEXT cx;
    DWORD efl, ss, sp, cs, ip;
    WORD fl;
    int ok = 0;

    if (!g_hcpu || g_in_exec == 0) { g_async_bail++; return 0; }
    /* Ask the PIC, exactly as the hardware would: is this line unmasked, and is nothing of
       equal or higher priority still in service? That is what stops us re-entering a handler
       that has not EOI'd yet -- the fault behind "press a key and everything hangs". */
    if (!vdd_pic_can_deliver(&g_pic, (uint8_t)irq)) { g_async_bail++; g_async_nest_blocked++; return 0; }
    /* Never deliver a line the guest has not hooked. Its vector still points at our default
       IRET stub, which means no ISR is installed -- and on a real PC an unused line sits
       masked in the PIC, so nothing would arrive at all. Delivering anyway is not harmless:
       it perturbs the guest's stack and control flow for no benefit, and it demonstrably
       derailed Skyroads (which never installs a Sound Blaster ISR) into executing junk in
       our own handler segment at 0050:006c, where it "terminated" via a garbage INT 21h. */
    { unsigned v0 = vdd_pic_vector(&g_pic, (uint8_t)irq);
      if (irq >= 2 && peekw(v0 * 4 + 2) == DOS_HDLR_SEG
                   && peekw(v0 * 4) == DOS_IRET_STUB_OFF) { g_async_bail++; return 0; } }
    if (SuspendThread(g_hcpu) == (DWORD)-1) { g_async_bail++; return 0; }
    { unsigned i; char *z = (char *)&cx; for (i = 0; i < sizeof cx; ++i) z[i] = 0; }
    cx.ContextFlags = CONTEXT_CONTROL | CONTEXT_SEGMENTS;
    if (!GetThreadContext(g_hcpu, &cx)) { ResumeThread(g_hcpu); g_async_bail++; return 0; }

    efl = cx.EFlags;
    cs  = cx.SegCs & 0xFFFF;
    if (!(efl & EFLAGS_VM_BIT) || !(efl & (0x200u | EFLAGS_VIF_BIT)) || cs == DOS_HDLR_SEG) {
        ResumeThread(g_hcpu); g_async_bail++; return 0;
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
        if (g_qi_susp && async_inject_irq(0)) InterlockedDecrement(&g_irq0_pending);
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
           The TIMER keeps async delivery, which is what makes the game playable at all. */
        if (g_qi_keys_async && async_inject_irq(1)) InterlockedDecrement(&g_irq1_pending);
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
    EnterCriticalSection(&g_lock);
    vdd_video_putc(&g_vid, ch);
    LeaveCriticalSection(&g_lock);
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
        EnterCriticalSection(&g_lock);
        got = vdd_input_pop(&g_in, &k);
        LeaveCriticalSection(&g_lock);
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
    EnterCriticalSection(&g_lock);
    vdd_opl_add_us(&g_opl, us);
    LeaveCriticalSection(&g_lock);
}

/* The audio thread's fill callback. Mixing touches the DMA controller, guest
   memory and the IRQ path, so it takes the same lock the exec thread uses. */
static void host_audio_fill(void *ctx, int16_t *out, uint32_t frames)
{
    (void)ctx;
    EnterCriticalSection(&g_lock);
    vdd_audio_mix(&g_audio, out, frames);
    LeaveCriticalSection(&g_lock);
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

    EnterCriticalSection(&g_lock);
    w = g_vid.frame.w; h = g_vid.frame.h;
    src = g_vid.frame.pixels;
    if (!src || !w || !h || g_vid.frame.bpp != 8) { LeaveCriticalSection(&g_lock); return; }
    stride = (w + 3) & ~3u;                  /* DIB rows are 4-byte aligned          */
    pal_n  = 256;
    img_sz = stride * h;
    dib_sz = sizeof(BITMAPINFOHEADER) + pal_n * 4 + img_sz;
    hmem = GlobalAlloc(GMEM_MOVEABLE, dib_sz);
    if (!hmem) { LeaveCriticalSection(&g_lock); return; }
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
    LeaveCriticalSection(&g_lock);

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
    EnterCriticalSection(&g_lock);
    if (ext) vdd_input_push_scancode(&g_in, 0xE0);
    vdd_input_push_scancode(&g_in, is_break ? (uint8_t)(rawsc | 0x80) : rawsc);
    LeaveCriticalSection(&g_lock);
    /* The VDD raises IRQ1 itself now, on the 8042's empty->full transition and again as the
       guest drains the FIFO -- so the host must NOT latch one per byte here as well, or the
       interrupts run ahead of the bytes again. */
    if (g_key_event) SetEvent(g_key_event);
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
    EnterCriticalSection(&g_lock);
    got = vdd_input_pop(&g_in, &k);
    LeaveCriticalSection(&g_lock);
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
    EnterCriticalSection(&g_lock);
    got = vdd_input_peek(&g_in, &k);
    LeaveCriticalSection(&g_lock);
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
static const char *const MS_CURSOR[16] = {
    "o",          "oo",         "oXo",        "oXXo",
    "oXXXo",      "oXXXXo",     "oXXXXXo",    "oXXXXXXo",
    "oXXXXXXXo",  "oXXXXoooo",  "oXXoXo",     "oXo oXo",
    "ooo  oXo",   "oo    oXo",  "      oXo",  "       oo",
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
    msub(m,"Mouse",(s=mpop(),mi(s,"Capture\tCtrl+F10",IDM_STUB),mi(s,"Seamless",IDM_STUB),mi(s,"Sensitivity",IDM_STUB),s));
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
        g_pit.frame_us = 0;
        host_pit_sync();
        EnterCriticalSection(&g_lock);
        vdd_bus_frame(&g_bus);          /* tick PIT + render into g_vid.frame       */
        if (g_ms_hidden == 0 && g_vid.frame.bpp == 8 && g_vid.frame.pixels)
            overlay_cursor((uint8_t *)g_vid.frame.pixels, g_vid.frame.w, g_vid.frame.h,
                           (int)g_vid.frame.stride, g_ms_x, g_ms_y);   /* driver mouse cursor */
        present_ddraw_snapshot(&g_pd, &g_vid.frame);  /* consistent copy UNDER lock  */
        LeaveCriticalSection(&g_lock);
        present_ddraw_present(&g_pd);   /* vsync'd blit OUTSIDE the lock             */
        /* Headless remote visual capture (session-9): the host screenshots ITSELF to
           C:\ntvdmex\shotNN.bmp every ~2s so a graphical run (Skyroads, the PM demos)
           is verifiable off the SMB share -- VNC capture is dead on the real box. The
           snapshot is owned by this UI thread, so no extra lock is needed; capped at
           40 frames so a long run never fills the disk. rt.bat copies shot*.bmp off. */
        if (g_capture) {
            static unsigned cap_tick = 0, cap_seq = 0;
            if ((cap_tick++ % 60) == 0 && cap_seq < 40) {
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
        if (LOWORD(lp) == HTCLIENT) { SetCursor(NULL); return TRUE; }  /* hide over video */
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
        /* Raw AT keyboard: push the MAKE scancode (lParam bits 16-23 = the OEM scan
           code) into the 0x60/0x64 FIFO and raise IRQ1, so action games that hook
           INT 09h or poll port 0x60 for real-time held-key state get input. This runs
           for every key, alongside the INT 16h ring below (which other games poll). */
        {
            uint8_t rawsc = (uint8_t)((lp >> 16) & 0xFF);
            if (rawsc) host_key_scancode(rawsc, (lp & 0x01000000) != 0, 0);
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
            if (rawsc) host_key_scancode(rawsc, (lp & 0x01000000) != 0, 1);
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
    present_ddraw_init(&g_pd, g_hwnd);          /* GDI windowed; DDraw for fullscreen */
    make_status(g_hwnd, hi);                     /* native themed status bar          */
    ShowWindow(g_hwnd, SW_SHOW); UpdateWindow(g_hwnd);
    SetTimer(g_hwnd, 1, 33, NULL);              /* ~30 Hz present/PIT tick         */
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
static void host_pit_sync(void)
{
    static LARGE_INTEGER s_freq, s_last;
    LARGE_INTEGER now;
    ULONGLONG delta;
    /* Called from BOTH the exec thread (so a guest polling the counter reads real time) and
       the UI thread (so the clock keeps running while the guest is spinning and not trapping
       at all). It must be one shared clock or the two would double-count, hence the lock --
       g_lock is recursive for the exec thread, which already holds it inside host_io_do. */
    EnterCriticalSection(&g_lock);
    if (!s_freq.QuadPart) {
        if (QueryPerformanceFrequency(&s_freq) && s_freq.QuadPart)
            QueryPerformanceCounter(&s_last);
        LeaveCriticalSection(&g_lock);
        return;
    }
    if (!QueryPerformanceCounter(&now) || now.QuadPart <= s_last.QuadPart) {
        LeaveCriticalSection(&g_lock);
        return;
    }
    delta = (ULONGLONG)(now.QuadPart - s_last.QuadPart);
    /* clocks = delta * 1193182 / freq, without overflowing: delta is small (microseconds). */
    { ULONGLONG clocks = (delta * PIT_INPUT_HZ) / (ULONGLONG)s_freq.QuadPart;
      if (clocks) {
          s_last.QuadPart += (LONGLONG)((clocks * (ULONGLONG)s_freq.QuadPart) / PIT_INPUT_HZ);
          if (clocks > PIT_INPUT_HZ) clocks = PIT_INPUT_HZ;   /* cap a long stall at 1 s */
          vdd_pit_add_clocks(&g_pit, (uint32_t)clocks);
      } }
    LeaveCriticalSection(&g_lock);
}

static void host_io_do(volatile BYTE *tib, vdd_bus *bus, uint16_t port,
                       int is_in, int width)
{
    uint32_t val, eax = VDM_REG(tib, VTIB_EAX);
    g_io_last_port = port;              /* for the hot-port histogram */
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

/* --- planar mode-12h: trap direct A0000 writes through the VGA write engine -- */
#define A000_LO 0xA0000u
#define A000_HI 0xB0000u
static int g_a000_prot = 0;

/* In mode 12h, mark the A0000 graphics window NOACCESS so direct guest writes
   fault to us; restore RW otherwise. */
static void a000_protect(int on)
{
    DWORD old;
    if (on == g_a000_prot) return;
    if (VirtualProtect((LPVOID)A000_LO, 0x10000,
                       on ? PAGE_NOACCESS : PAGE_EXECUTE_READWRITE, &old))
        g_a000_prot = on;
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

    EnterCriticalSection(&g_lock);
    for (iters = 0; iters < cap; ++iters)
        if (!istep(&c)) break;
    LeaveCriticalSection(&g_lock);

    if (iters == 0) return 0;                          /* first opcode unmodeled */

    VDM_SET16(tib, VTIB_EAX, c.r[0]); VDM_SET16(tib, VTIB_ECX, c.r[1]);
    VDM_SET16(tib, VTIB_EDX, c.r[2]); VDM_SET16(tib, VTIB_EBX, c.r[3]);
    VDM_SET16(tib, VTIB_ESP, c.r[4]); VDM_SET16(tib, VTIB_EBP, c.r[5]);
    VDM_SET16(tib, VTIB_ESI, c.r[6]); VDM_SET16(tib, VTIB_EDI, c.r[7]);
    VDM_SET16(tib, VTIB_EIP, c.ip);
    /* update only the low 16 flag bits (arith + DF); keep VM/IOPL/IF etc. */
    VDM_REG(tib, VTIB_EFLAGS) = (VDM_REG(tib, VTIB_EFLAGS) & 0xFFFF0000u) | (c.flags & 0xFFFFu);
    return iters;
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
    if (cx->SegCs == 0x1B && s_veh_count < 256) {
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
    serial_out(wb, q); q = wb;
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
            serial_out(wb, q); q = wb;
        }
        prev = iter; ++n;
        if (frozen >= 12) break;                         /* ~3s with NO progress -> real wedge */
      }
    }
    q = zput(q, "STAGE3-DPMI: watchdog terminating (wedged)\r\n");
    log_append(LOG_PATH, wb, q);
    serial_out(wb, q);
    /* TerminateProcess (forceful) -- ExitProcess hangs trying to unwind the PM engine
       thread (un-terminable LDT context). */
    TerminateProcess(GetCurrentProcess(), 0xDD0);
    return 0;
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
            char lb[160], *p = lb;
            p = zput(p, "DPMI-LDT: install REJECTED sel 0x"); p = zhex(p, sel);
            p = zput(p, " base 0x"); p = zhex(p, g_ldt[idx].base);
            p = zput(p, " limit 0x"); p = zhex(p, g_ldt[idx].limit);
            p = zput(p, " g="); p = zhex(p, (DWORD)((fl >> 3) & 1));
            p = zput(p, " status 0x"); p = zhex(p, (DWORD)st); p = zput(p, "\r\n");
            log_append(LOG_PATH, lb, p); serial_out(lb, p);
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
    if (idx >= 1 && idx < 512) return g_ldt[idx].base;
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

/* Un-patch / re-patch the shared code segment around a V86 excursion (INT 31h 0301/
   0303). The switch-time scan rewrote every `CD 31`/`CD 21` in the code segment to a
   BOP so PM software-ints reflect to us -- but that segment is ALSO the V86 view, so a
   real-mode INT inside a 0301 proc would hit a corrupted BOP instead of vectoring
   natively. g_int_vec[] is the revert map (offset -> original vector), so we restore
   the real `CD nn` bytes before running V86 (real-mode ints then vector through the
   IVT to our BOP stubs and are serviced normally) and re-apply the BOP patch before
   resuming the PM client. */
static void dpmi_unpatch(void)
{
    volatile BYTE *cs = (volatile BYTE *)(ULONG_PTR)g_dpmi_code_base;
    DWORD o;
    for (o = 0; o < 0xFFFF; ++o)
        if (g_int_vec[o]) { cs[o] = 0xCD; cs[o + 1] = g_int_vec[o]; }
}
static void dpmi_repatch(void)
{
    volatile BYTE *cs = (volatile BYTE *)(ULONG_PTR)g_dpmi_code_base;
    DWORD o;
    for (o = 0; o < 0xFFFF; ++o)
        if (g_int_vec[o]) { cs[o] = 0xC4; cs[o + 1] = 0xC4; }
}

/* Forward decl: the shared PM-interrupt dispatcher (defined after this fn). A callback
   handler that issues its own INT 31h/21h routes through it, same as the main PM loop. */
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
        ev = VDM_REG(tib, VTIB_EVENT); eip = VDM_REG(tib, VTIB_EIP) & 0xFFFF;
        if (ev == VDM_EVENT_BOP && eip == DPMI_PMRET_OFF
            && (VDM_REG(tib, VTIB_CS) & 0xFFFF) == g_pmret_sel) { cbdone = 1; break; }
        if (ev == 3) continue;   /* dpmi_enter_pm reports "interrupt pending, not entered" -- retry */
        vec = (ev == VDM_EVENT_BOP) ? g_int_vec[eip] : 0;   /* a patched INT the handler issued */
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
static int dpmi_service_pm_int(dos_machine_t *mp, volatile BYTE *tib, DWORD vec,
                               unsigned steps)
{
#define m (*mp)
    char report[2048]; char *base = report; char *p = report;
    DWORD ax = VDM_REG(tib, VTIB_EAX) & 0xFFFF;
    DWORD ev = VDM_REG(tib, VTIB_EVENT), eip = VDM_REG(tib, VTIB_EIP) & 0xFFFF;
    (void)steps;
                    if (vec == 0x10) {                             /* video BIOS in PM -> VDD */
                        ntvdd_regs r; regs_load(&r, tib);
                        EnterCriticalSection(&g_lock);
                        vdd_bus_deliver_int(&g_bus, 0x10, &r);
                        LeaveCriticalSection(&g_lock);
                        regs_store(&r, tib);
                        a000_protect(vdd_video_planar_active(&g_vid));  /* mode 12h A0000 trap; no-op in 13h */
                        VDM_REG(tib, VTIB_EFLAGS) &= ~1u;
                        VDM_REG(tib, VTIB_EIP) += 2;              /* past the 2-byte PM BOP */
                        p = zput(p, "INT10h (PM) -> video VDD AX=0x"); p = zhex(p, ax);
                        p = zput(p, "\r\n"); log_append(LOG_PATH, base, p); serial_out(base, p); p = base;
                        return 1;
                    }
                    if (vec == 0x16) {                             /* keyboard BIOS in PM -> VDD */
                        ntvdd_regs r; uint8_t ah16; regs_load(&r, tib); ah16 = r_ah(&r);
                        for (;;) {                                 /* AH=00/10 block until a key */
                            EnterCriticalSection(&g_lock);
                            vdd_bus_deliver_int(&g_bus, 0x16, &r);
                            LeaveCriticalSection(&g_lock);
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
                        EnterCriticalSection(&g_lock);
                        vdd_bus_deliver_int(&g_bus, (uint8_t)vec, &r);   /* INT 1Ah get/set tick, or INT 08h increment */
                        LeaveCriticalSection(&g_lock);
                        regs_store(&r, tib);
                        VDM_REG(tib, VTIB_EIP) += 2;
                        return 1;
                    }
                    if (vec == 0x31) {                             /* DPMI INT 31h */
                        p = zput(p, "INT31h AX=0x"); p = zhex(p, ax);
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
                            if (g_ldt_next + (int)cx > 512) {      /* out of descriptors */
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
                        case 0x0001:                               /* free descriptor BX (no-op reclaim) */
                            p = zput(p, " -> free");
                            break;
                        case 0x0100: {                             /* allocate DOS memory: BX paras -> AX=seg, DX=sel */
                            uint16_t want = (uint16_t)(VDM_REG(tib, VTIB_EBX) & 0xFFFF), seg = 0, max = 0;
                            int err = dos_alloc(NULL, m.first_mcb, want, &seg, &max);
                            if (err || g_ldt_next >= 512) {
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
                            if (idx >= 3 && idx < 512) {
                                DWORD seg = g_ldt[idx].base >> 4;
                                dos_free(NULL, (uint16_t)seg);
                                g_ldt[idx].base = g_ldt[idx].limit = 0; /* descriptor left reclaimable */
                            }
                            p = zput(p, " -> DOSfree");
                            break; }
                        case 0x0102: {                             /* resize DOS memory block: BX=new paras, DX=sel */
                            int idx = (VDM_REG(tib, VTIB_EDX) & 0xFFFF) >> 3;
                            uint16_t want = (uint16_t)(VDM_REG(tib, VTIB_EBX) & 0xFFFF), max = 0;
                            if (idx >= 1 && idx < 512 && g_ldt[idx].base) {
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
                            /* 32-bit handler selector -> return the full 32-bit offset (GH #18 run 83) */
                            if (dpmi_sel_is32(g_pm_int[bl].sel)) VDM_REG(tib, VTIB_EDX) = g_pm_int[bl].off;
                            else VDM_SET16(tib, VTIB_EDX, g_pm_int[bl].off & 0xFFFF);
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
                            p = zput(p, " -> setPMvec int 0x"); p = zhex(p, bl);
                            p = zput(p, " = 0x"); p = zhex(p, g_pm_int[bl].sel);
                            p = zput(p, ":0x"); p = zhex(p, g_pm_int[bl].off);
                            break; }
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
                            if (idx >= 1 && idx < 512) { g_ldt[idx].base = b; dpmi_install(idx); }
                            p = zput(p, " sel 0x"); p = zhex(p, VDM_REG(tib, VTIB_EBX) & 0xFFFF);
                            p = zput(p, " -> setbase 0x"); p = zhex(p, b);
                            break; }
                        case 0x0008: {                             /* set limit of sel BX = CX:DX */
                            int idx = (VDM_REG(tib, VTIB_EBX) & 0xFFFF) >> 3;
                            DWORD l = ((VDM_REG(tib, VTIB_ECX) & 0xFFFF) << 16) | (VDM_REG(tib, VTIB_EDX) & 0xFFFF);
                            if (idx >= 1 && idx < 512) { g_ldt[idx].limit = l; dpmi_install(idx); }
                            p = zput(p, " sel 0x"); p = zhex(p, VDM_REG(tib, VTIB_EBX) & 0xFFFF);
                            p = zput(p, " -> setlimit 0x"); p = zhex(p, l);
                            break; }
                        case 0x0009: {                             /* set access rights of sel BX (CX) */
                            int idx = (VDM_REG(tib, VTIB_EBX) & 0xFFFF) >> 3;
                            if (idx >= 1 && idx < 512) {
                                /* CL = access byte (P|DPL|S|type). CH = descriptor byte 6
                                   (G|D/B|L|AVL|limit19:16); its HIGH nibble carries G/D/B/L/AVL,
                                   which maps 1:1 onto our flags nibble (see dpmi_build_desc).
                                   #3 (DOS/4GW): a 32-bit code selector arrives here with CH bit6
                                   (D/B) set -> flags bit2 -> dpmi_sel_is32() true. */
                                g_ldt[idx].access = VDM_REG(tib, VTIB_ECX) & 0xFF;
                                g_ldt[idx].flags  = (VDM_REG(tib, VTIB_ECX) >> 12) & 0xF;  /* CH high nibble */
                                dpmi_install(idx);
                            }
                            p = zput(p, " sel 0x"); p = zhex(p, VDM_REG(tib, VTIB_EBX) & 0xFFFF);
                            p = zput(p, " -> setaccess 0x"); p = zhex(p, VDM_REG(tib, VTIB_ECX) & 0xFFFF);
                            break; }
                        case 0x000A: {                             /* create data alias of sel BX */
                            int src = (VDM_REG(tib, VTIB_EBX) & 0xFFFF) >> 3, idx;
                            if (g_ldt_next >= 512) { VDM_REG(tib, VTIB_EFLAGS) |= 1u; p = zput(p, " -> ENOMEM"); break; }
                            idx = g_ldt_next++;
                            if (src >= 1 && src < 512) g_ldt[idx] = g_ldt[src];
                            else { g_ldt[idx].base = g_dpmi_code_base; g_ldt[idx].limit = 0xFFFF; g_ldt[idx].flags = 0; }
                            g_ldt[idx].access = 0xF2;              /* data alias */
                            dpmi_install(idx);
                            VDM_SET16(tib, VTIB_EAX, (WORD)((idx << 3) | 7));
                            p = zput(p, " -> alias sel 0x"); p = zhex(p, (idx << 3) | 7);
                            break; }
                        case 0x0500: {                             /* get free memory info -> ES:DI */
                            DWORD esb = dpmi_sel_base((WORD)VDM_REG(tib, VTIB_ES));
                            volatile DWORD *info = (volatile DWORD *)(ULONG_PTR)
                                (esb + (VDM_REG(tib, VTIB_EDI) & 0xFFFF));
                            int i; for (i = 0; i < 12; ++i) info[i] = 0xFFFFFFFFu;
                            info[0] = 0x04000000u;                 /* largest free block = 64MB */
                            p = zput(p, " -> meminfo");
                            break; }
                        case 0x0501: {                             /* allocate memory block BX:CX bytes */
                            DWORD sz = ((VDM_REG(tib, VTIB_EBX) & 0xFFFF) << 16) | (VDM_REG(tib, VTIB_ECX) & 0xFFFF);
                            void *mem = VirtualAlloc(NULL, sz ? sz : 1, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
                            if (!mem) { VDM_REG(tib, VTIB_EFLAGS) |= 1u; VDM_SET16(tib, VTIB_EAX, 0x8013);
                                        p = zput(p, " -> ENOMEM"); break; }
                            { DWORD lin = (DWORD)(ULONG_PTR)mem;   /* in-process: linear = host ptr */
                              VDM_SET16(tib, VTIB_EBX, lin >> 16); VDM_SET16(tib, VTIB_ECX, lin & 0xFFFF);
                              VDM_SET16(tib, VTIB_ESI, lin >> 16); VDM_SET16(tib, VTIB_EDI, lin & 0xFFFF); /* handle=addr */
                              p = zput(p, " -> mem 0x"); p = zhex(p, lin); }
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
                            /* restore the client's PM register file */
                            VDM_REG(tib,VTIB_EAX)=sA;VDM_REG(tib,VTIB_EBX)=sB;VDM_REG(tib,VTIB_ECX)=sC;VDM_REG(tib,VTIB_EDX)=sD;
                            VDM_REG(tib,VTIB_ESI)=sS;VDM_REG(tib,VTIB_EDI)=sDi;VDM_REG(tib,VTIB_EBP)=sBp;VDM_REG(tib,VTIB_DS)=sDs;
                            VDM_REG(tib,VTIB_ES)=sEs;VDM_REG(tib,VTIB_SS)=sSs;VDM_REG(tib,VTIB_ESP)=sSp;VDM_REG(tib,VTIB_EFLAGS)=sFl;
                            VDM_REG(tib,VTIB_EFLAGS) &= ~1u;       /* 0300 succeeds */
                            p = zput(p, " -> simInt 0x"); p = zhex(p, intno);
                            break; }
                        case 0x0301: {                             /* call real-mode FAR proc: ES:DI=RMCS, CX=stack words */
                            /* This is the first PM->V86->PM round-trip. Unlike 0300 (which fakes a
                               real-mode INT by calling dos_int21 host-side), 0301 must actually RUN
                               the client's real-mode procedure in V86: we rewrite the CONTEXT to
                               V86, push a far-return frame pointing at the DPMI_RMRET_BOP catcher,
                               run v86_run() until that BOP (servicing any INT 21h the proc makes),
                               copy the real-mode regs back into the RMCS, then restore PM. */
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
                            p = zput(p, " -> callRM 0x"); p = zhex(p, rcs); p = zput(p, ":0x"); p = zhex(p, rip);
                            p = zput(p, " SS:SP=0x"); p = zhex(p, rss); p = zput(p, ":0x"); p = zhex(p, rsp);
                            p = zput(p, "\r\n"); log_append(LOG_PATH, base, p); serial_out(base, p); p = base;
                            /* push the far-return frame [IP=RMRET, CS=DOS_HDLR_SEG] on the RM stack */
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
                                    int h; EnterCriticalSection(&g_lock);
                                    h = host_try_io(tib, &g_bus); LeaveCriticalSection(&g_lock);
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
                        p = zput(p, "INT21h AH=0x"); p = zhex(p, ah); p = zput(p, " (PM thunk TODO)\r\n");
                        log_append(LOG_PATH, base, p); serial_out(base, p); p = base;
                        VDM_REG(tib, VTIB_EIP) += 2;
                        return 1;
                    }
                    p = zput(p, "DPMI: unexpected PM stop event=0x"); p = zhex(p, ev);
                    p = zput(p, " CS:EIP=0x"); p = zhex(p, VDM_REG(tib, VTIB_CS) & 0xFFFF);
                    p = zput(p, ":0x"); p = zhex(p, eip); p = zput(p, "\r\n");
                    log_append(LOG_PATH, base, p); serial_out(base, p); p = base;
                    return -1;
#undef m
}

/* Lazily install the PM-return catcher selector (g_pmret_sel): a code selector based
   at DOS_HDLR_SEG so a PM handler's IRET lands on the planted DPMI_PMRET BOP. Shared by
   the 0303 real-mode-callback path and the async-IRQ injector (#2b). */
static void dpmi_ensure_pmret_sel(void)
{
    if (g_pmret_sel == 0 && g_ldt_next < 512) {
        int idx = g_ldt_next++;
        g_ldt[idx].base = (DWORD)DOS_HDLR_SEG << 4; g_ldt[idx].limit = 0xFFFF;
        g_ldt[idx].access = 0xFA; g_ldt[idx].flags = 0;   /* code exec/read */
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

    dpmi_ensure_pmret_sel();
    if (g_pmret_sel == 0) return 0;

    /* push an INT frame (FLAGS/CS/IP) on the client's current PM stack so the handler's IRET
       lands on the catcher; keep the client's own SS so the handler has a valid stack. Frame
       width + entry-offset mask follow the handler CS D-bit: a 32-bit PM INT handler wants a
       dword frame and a full 32-bit entry offset (GH #18 run 83). */
    int h32 = dpmi_sel_is32(g_pm_int[iv].sel);
    { WORD ss = sSS; DWORD b = dpmi_sel_base(ss);
      if (h32) {
          DWORD sp = sESP;
          sp -= 4; poked(b + sp, sEFL);            /* EFLAGS (restored below regardless) */
          sp -= 4; poked(b + sp, g_pmret_sel);     /* CS  = catcher (dword; hi16=0)       */
          sp -= 4; poked(b + sp, DPMI_PMRET_OFF);  /* EIP = catcher                       */
          VDM_SET16(tib, VTIB_SS, ss); VDM_REG(tib, VTIB_ESP) = sp;
      } else {
          WORD sp = (WORD)sESP;
          sp -= 2; pokew(b + sp, (WORD)sEFL);      /* FLAGS (restored below regardless) */
          sp -= 2; pokew(b + sp, g_pmret_sel);     /* CS  = catcher                      */
          sp -= 2; pokew(b + sp, DPMI_PMRET_OFF);  /* IP  = catcher                      */
          VDM_SET16(tib, VTIB_SS, ss); VDM_REG(tib, VTIB_ESP) = sp;
      } }
    g_dpmi_vi = 0;                                 /* mask further virtual interrupts     */
    VDM_REG(tib, VTIB_EFLAGS) = VTIB_EFLAGS_PM;
    VDM_SET16(tib, VTIB_CS, g_pm_int[iv].sel);
    VDM_REG(tib, VTIB_EIP) = h32 ? g_pm_int[iv].off : (g_pm_int[iv].off & 0xFFFF);

    lp = zput(lp, "  IRQ0->PM INT 0x"); lp = zhex(lp, iv);
    lp = zput(lp, " handler 0x"); lp = zhex(lp, g_pm_int[iv].sel);
    lp = zput(lp, ":0x"); lp = zhex(lp, g_pm_int[iv].off & 0xFFFF); lp = zput(lp, "\r\n");
    log_append(LOG_PATH, lb, lp); serial_out(lb, lp); lp = lb;

    for (ph = 0; ph < 64 && !done; ++ph) {
        DWORD ev, eip, vec; int rc;
        dpmi_arm_fault_trampoline(tib, 0);
        dpmi_enter_pm(tib);
        ev  = VDM_REG(tib, VTIB_EVENT);
        eip = VDM_REG(tib, VTIB_EIP) & 0xFFFF;
        if (ev == VDM_EVENT_BOP && eip == DPMI_PMRET_OFF
            && (VDM_REG(tib, VTIB_CS) & 0xFFFF) == g_pmret_sel) { done = 1; break; }
        if (ev == 3) continue;                     /* "interrupt pending, not entered" -> retry */
        if (ev == VDM_EVENT_IO || ev == VDM_EVENT_IO_HW || ev == VDM_EVENT_GPFAULT) {
            int io_h;
            EnterCriticalSection(&g_lock);
            io_h = host_try_io_pm(tib, &g_bus);
            LeaveCriticalSection(&g_lock);
            if (io_h) continue;
        }
        vec = (ev == VDM_EVENT_BOP) ? g_int_vec[eip] : 0;
        rc = dpmi_service_pm_int(mp, tib, vec, steps);
        if (rc > 0) continue;
        break;                                     /* handler exited / unexpected stop */
    }

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
    static const BYTE bios_ints[] = { 0x11, 0x12, 0x13, 0x14, 0x15, 0x17, 0x25, 0x26 };
    static const BYTE emmname[] = { 'E','M','M','X','X','X','X','0' };  /* EMS device header name */
    HANDLE ui = NULL;

    (void)hInst; (void)hPrev; (void)lpCmd; (void)nShow;
    progpath[0] = 0; args[0] = 0;

    p = zput(p, "NTVDMEX clean host\r\nSTAGE0: WinMain entered [build dpmi-harness-v164]\r\n");
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
    g_capture  = g_headless && (GetFileAttributesA(CAPTURE_FLAG) != INVALID_FILE_ATTRIBUTES);
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
    /* (GH #18 run 67: the PM-fault handler BOP is planted at the handler CODE selector's
       DPMI_FAULT_COFF by dpmi_install_fault_trampoline(), not here.) */
    /* EMS detection method 2: programs read the INT 67h vector's segment:000Ah for
       the device-driver name "EMMXXXX0". Park it in the handler segment. */
    for (i = 0; i < sizeof(emmname); ++i) hdlr[DOS_EMM_NAME_OFF + i] = emmname[i];

    { unsigned bi;
      volatile BYTE *bs = (volatile BYTE *)(DOS_CTAB_SEG << 4);
      for (bi = 0; bi < sizeof(bios_ints); ++bi) {
          unsigned off = DOS_BIOS_STUBS + bi * 4;
          bs[off + 0] = VDM_BOP0; bs[off + 1] = VDM_BOP1;
          bs[off + 2] = bios_ints[bi]; bs[off + 3] = 0xCF;   /* IRET */
          *(volatile WORD *)(bios_ints[bi] * 4)     = (WORD)off;
          *(volatile WORD *)(bios_ints[bi] * 4 + 2) = DOS_CTAB_SEG;
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
    vdd_bus_init(&g_bus, NULL);
    vdd_bus_set_sinks(&g_bus, host_irq_sink, NULL, NULL, NULL);  /* host presents directly */
    g_pic_dev = vdd_pic_device(&g_pic);      /* before the PIT: it gates every IRQ */

    vdd_bus_add(&g_bus, &g_pic_dev);
    g_pit_dev = vdd_pit_device(&g_pit);
    vdd_bus_add(&g_bus, &g_pit_dev);
    g_vid.vmem = (uint8_t *)VID_APERTURE_BASE;  /* the mapped A0000 aperture (RAM) */
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
    g_sb_dev = vdd_sb_device(&g_sb);
    vdd_bus_add(&g_bus, &g_sb_dev);             /* Sound Blaster 16: 0x220-0x22F  */
    g_mpu.sink = host_midi_sink;
    g_mpu_dev = vdd_mpu_device(&g_mpu);
    vdd_bus_add(&g_bus, &g_mpu_dev);            /* MPU-401 MIDI: 0x330/0x331      */
    /* Start the mixer + audio thread. This is also the TRANSPORT: it is what
       walks the SB's DMA buffer and raises the block-completion IRQ, so it must
       run even if no sound device opens (audio_wave falls back to silent
       pumping) -- otherwise every SB game hangs on a machine without audio. */
    vdd_audio_init(&g_audio, &g_opl, &g_sb, AUDIO_OUT_HZ);
    audio_wave_start(&g_wave, AUDIO_OUT_HZ, host_audio_fill, NULL);
    m.conout = host_conout; m.conctx = NULL;    /* DOS console out -> video      */
    m.conin  = host_conin;  m.cinctx = NULL;    /* DOS console in  <- keyboard   */
    m.coninnb = host_coninnb;                   /* AH=06 DL=FF non-blocking read */
    m.conpeek = host_conpeek;                   /* AH=0B/06 non-blocking status  */

    /* Hide the inherited console (CSRSS already bound the VDM); the Luna window
       is now the display. Then start the UI thread that owns it. */
    g_key_event = CreateEventA(NULL, FALSE, FALSE, NULL);   /* auto-reset        */
    { HWND con = GetConsoleWindow(); if (con) ShowWindow(con, SW_HIDE); }
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

    SetCurrentDirectoryA(g_cur);    /* DOS relative paths resolve against CurDir */

    /* Service loop: run V86 until a BOP, dispatch INT 21h, step past the BOP, re-enter.
       Runs until the guest terminates, a hard stop, or the window closes (g_running);
       no iteration cap so interactive/animated programs keep going. */
    m.tib = tib; m.out = dosout; m.out_cap = sizeof(dosout); m.out_len = 0; m.out_trunc = 0;
    (void)guard;
    { static uint32_t s_last_fault = 0; static int s_storm = 0;
    DWORD rm_start_tick = GetTickCount();   /* headless wall-clock cap origin (real-mode) */
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
                EnterCriticalSection(&g_lock);
                handled = host_try_io_string(tib, &g_bus);
                LeaveCriticalSection(&g_lock);
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
            if (g_a000_prot && s_storm >= STORM_GATE && host_interp(tib, TIER1_CAP) > 0)
                continue;                           /* batched the hot loop            */
            EnterCriticalSection(&g_lock);
            handled = host_try_io(tib, &g_bus);     /* single port op (no logging)     */
            LeaveCriticalSection(&g_lock);
            if (handled) { g_ev_io++; io_hot_note(g_io_last_port, VDM_REG(tib, VTIB_CS) & 0xFFFF, VDM_REG(tib, VTIB_EIP) & 0xFFFF); continue; }
            /* real-HW event 3 reports CS:IP AFTER the faulting IN/OUT -> retro-decode the
               I/O instruction ending at CS:IP and service it (Skyroads' vblank IN AL,DX). */
            if (ev == VDM_EVENT_IO_HW) {
                EnterCriticalSection(&g_lock);
                handled = host_try_io_retro(tib, &g_bus);
                LeaveCriticalSection(&g_lock);
                if (handled) { g_ev_io++; io_hot_note(g_io_last_port, VDM_REG(tib, VTIB_CS) & 0xFFFF, VDM_REG(tib, VTIB_EIP) & 0xFFFF); continue; }
            }
            if (g_a000_prot && host_interp(tib, 1) > 0) continue;  /* single A0000 access */
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
            EnterCriticalSection(&g_lock);
            vdd_bus_deliver_int(&g_bus, 0x10, &r);
            LeaveCriticalSection(&g_lock);
            regs_store(&r, tib);
            a000_protect(vdd_video_planar_active(&g_vid));   /* trap A0000 in mode 12h */
            VDM_REG(tib, VTIB_EIP) += 3;
            continue;
        }
        if ((VDM_REG(tib, VTIB_EVENT_INFO) & 0xFF) == 0x16) {
            ntvdd_regs r; uint8_t ah16; regs_load(&r, tib); ah16 = r_ah(&r);
            EnterCriticalSection(&g_lock);
            vdd_bus_deliver_int(&g_bus, 0x16, &r);
            LeaveCriticalSection(&g_lock);
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
            EnterCriticalSection(&g_lock);
            vdd_input_bios_consume(&g_in);      /* take the byte, re-arm if more queued */
            LeaveCriticalSection(&g_lock);
            VDM_REG(tib, VTIB_EIP) += 3;        /* -> the IRET */
            continue;
        }
        if ((VDM_REG(tib, VTIB_EVENT_INFO) & 0xFF) == 0x08) {   /* INT 08h timer tick */
            ntvdd_regs r; regs_load(&r, tib);
            EnterCriticalSection(&g_lock);
            vdd_bus_deliver_int(&g_bus, 0x08, &r);  /* bump BIOS tick at 0040:006C */
            /* The real BIOS timer ISR ends with `mov al,20h; out 20h,al`. Ours is a BOP
               with nowhere to put one, so issue the EOI here -- without it the PIC's
               in-service bit for IRQ0 latches on the first tick and the timer stops dead
               (measured: exactly one tick delivered in a 30 s run). */
            vdd_pic_eoi(&g_pic, 0);
            LeaveCriticalSection(&g_lock);
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
                    BSETAX((WORD)((VDM_REG(tib, VTIB_EAX) & 0xFF) | 0x8600));
                    BCF_SET();                     /* AH=86h: unsupported fn */
                    g_bios_unimpl[0x15] = 1;
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
            } else if (bn == 0x25 || bn == 0x26) { /* absolute disk read/write */
                BSETAX(0x0207); BCF_SET();         /* AL=07 drive param error  */
                g_bios_unimpl[bn] = 1;
            } else handled = 0;
            #undef BCF_SET
            #undef BCF_CLR
            #undef BSETAX
            if (handled) { VDM_REG(tib, VTIB_EIP) += 3; continue; }
        }
        if ((VDM_REG(tib, VTIB_EVENT_INFO) & 0xFF) == 0x1A) {   /* INT 1Ah BIOS time */
            ntvdd_regs r; regs_load(&r, tib);
            EnterCriticalSection(&g_lock);
            vdd_bus_deliver_int(&g_bus, 0x1A, &r);
            LeaveCriticalSection(&g_lock);
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
            EnterCriticalSection(&g_lock);
            host_ems(tib);
            LeaveCriticalSection(&g_lock);
            VDM_REG(tib, VTIB_EIP) += 3;                        /* -> the IRET      */
            continue;
        }
        if ((VDM_REG(tib, VTIB_EVENT_INFO) & 0xFF) == DPMI_BOP) {  /* DPMI real->PM switch */
            DWORD csv = VDM_REG(tib, VTIB_CS) & 0xFFFF, ipv = VDM_REG(tib, VTIB_EIP) & 0xFFFF;
            LONG reg_st = 0, set_st = 0; int sw;
            /* #3 (run 81): AX bit0 selects the client width -- 0=16-bit, 1=32-bit (DOS/4GW).
               A 32-bit client gets D/B=1 initial CS/DS/SS so its post-switch code runs 32-bit. */
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
                    /* mirror the D/B width dpmi_switch_to_pm installed, so dpmi_sel_is32()
                       (I/O decode + EIP-mask gating) agrees with the live descriptor (run 81). */
                    g_ldt[1 + si].flags  = is32 ? 0x4 : 0;
                } }
                if (g_ldt_next < 4) g_ldt_next = 4;      /* client allocs start at index 4 now */
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
                  if (wd) CloseHandle(wd); }
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
                          g_int_vec[o] = cs[o+1]; cs[o] = 0xC4; cs[o+1] = 0xC4; ++n; last = o;
                      }
                  }
                  p = zput(p, "DPMI: patched "); p = zhex(p, n);
                  p = zput(p, " INT sites -> BOP (full 64K scan, last off 0x"); p = zhex(p, last);
                  p = zput(p, ")\r\n");
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
                    g_dpmi_enter_eip = VDM_REG(tib, VTIB_EIP) & 0xFFFF;
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
                        EnterCriticalSection(&g_lock);
                        vdd_bus_deliver_int(&g_bus, 0x08, &tr);   /* pit_int08 -> ++0040:006C */
                        LeaveCriticalSection(&g_lock);
                        g_pm_irq0_latch = 1;    /* #2b: latch a virtual IRQ0 for the PM hook */
                    }
                    /* #2b async IRQ0 injection: when the client has hooked INT 08h in PM
                       (g_pm_int[8], via INT 31h 0205) and its virtual-IF is enabled, deliver
                       the latched IRQ0 to that handler -- how timer-hooking games get ticks.
                       The latch persists across CLI windows so a masked interrupt isn't lost. */
                    if (g_pm_irq0_latch && g_dpmi_vi && g_pm_int[0x08].sel && !g_in_pm_irq) {
                        g_pm_irq0_latch = 0;
                        g_in_pm_irq = 1;
                        dpmi_inject_pm_irq(&m, tib, 0x08, steps);
                        g_in_pm_irq = 0;
                    }
                    dpmi_arm_fault_trampoline(tib, 0);   /* re-arm nest/flag/[0x638]/[TIB+8] */
                    dpmi_enter_pm(tib);
                    ev  = VDM_REG(tib, VTIB_EVENT);
                    eip = VDM_REG(tib, VTIB_EIP) & 0xFFFF;
                    csv = VDM_REG(tib, VTIB_CS) & 0xFFFF;
                    g_dpmi_last_ev  = ev;  g_dpmi_last_eip = eip;  g_dpmi_last_cs = csv;
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
                        p = zput(p, "GH#18: PM-FAULT REFLECTED to trampoline -- saved CS:EIP=0x");
                        p = zhex(p, fcs); p = zput(p, ":0x"); p = zhex(p, feip);
                        p = zput(p, " sav3=0x"); p = zhex(p, fss3);
                        p = zput(p, " nest=0x"); p = zhex(p, *(volatile WORD *)(tib + VTIB_FLT_NEST));
                        p = zput(p, " -- REAL-CPU PM #GP reflect WORKS\r\n");
                        log_append(LOG_PATH, base, p); serial_out(base, p); p = base;
                        /* Servicing a real fault (emulate/skip the instruction, then resume at
                           the saved CS:EIP) is the follow-on; for run 59 the reflect firing IS
                           the deliverable, so stop the client cleanly here. */
                        break;
                    }
                    /* GH#18 run 72: a real-CPU PROTECTED-MODE I/O insn (IN/OUT) reflects as
                       event 0 -- the SAME VDD-trap event as V86 (VM-confirmed by outprobe.com,
                       a PM `OUT DX,AL` to 0x3C8). Service it through the device bus and resume,
                       so PM port I/O (VGA/sound) reaches our VDDs instead of the loop treating
                       event 0 as an "unexpected PM stop" and spinning. */
                    if (ev == VDM_EVENT_IO || ev == VDM_EVENT_IO_HW || ev == VDM_EVENT_GPFAULT) {
                        int io_h;
                        EnterCriticalSection(&g_lock);
                        io_h = host_try_io_pm(tib, &g_bus);
                        LeaveCriticalSection(&g_lock);
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
                    vec = (ev == VDM_EVENT_BOP) ? g_int_vec[eip] : 0;
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
            p = m.tp; VDM_REG(tib, VTIB_EIP) += 3;
            log_append(LOG_PATH, base, p); p = base;
            break;
        }
        p = m.tp;
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
      p = zput(p, " async_bail=0x"); p = zhex(p, g_async_bail);
      p = zput(p, " async_nest=0x"); p = zhex(p, g_async_nest_blocked);
      p = zput(p, " irq1_inj=0x");   p = zhex(p, g_irq1_inj);
      p = zput(p, " int16=[");
      { int k; for (k = 0; k < 4; ++k) { p = zput(p, "0x"); p = zhex(p, g_in.int16_calls[k]); p = zput(p, " "); } }
      p = zput(p, "] p60=0x");       p = zhex(p, g_in.p60_reads);
      p = zput(p, " sc_left=0x");    p = zhex(p, (DWORD)vdd_input_sc_pending(&g_in));
      p = zput(p, " sc_push=0x");    p = zhex(p, g_in.sc_pushed);
      p = zput(p, " sc_drop=0x");    p = zhex(p, g_in.sc_dropped);
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
      p = zput(p, "STAGE2: video modes unsupported:");
      for (i = 0, n = 0; i < 256; ++i)
          if (VID_UNIMPL_GET(g_vid.unimpl_mode, i)) { p = zput(p, " 0x"); p = zhexb(p, (unsigned)i); ++n; }
      if (!n) p = zput(p, " none");
      p = zput(p, "\r\n"); }
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
