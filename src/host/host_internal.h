/* host_internal.h -- what the host_*.c files share: the types, macros and state
 * used by more than one of them, and a prototype for every function called across
 * files. Included once, by main.c, after its prelude. */
#ifndef NTVDMEX_HOST_INTERNAL_H
#define NTVDMEX_HOST_INTERNAL_H

/* ── ★ THE FOUR IMPORTS WINDOWS 2000 DOES NOT HAVE. (2026-09-22, user on the 2000 box) ──
     The host would not START on Windows 2000: "The procedure entry point
     AddVectoredExceptionHandler could not be located in KERNEL32.dll". The loader
     resolves every static import before WinMain runs, so ONE missing export is a
     refusal to load, not a degraded feature. The import table of the built exe was
     dumped and compared: of 417 imports exactly four are XP-only --
       kernel32  AddVectoredExceptionHandler   (the PM-fault VEH)
       kernel32  AttachConsole                 (stdout to the parent's console)
       user32    RegisterRawInputDevices       (raw mouse deltas while captured)
       user32    GetRawInputData
     Everything else is NT 5.0. So these four are bound at run time, and each has a
     fallback that already existed or is the same mechanism one layer down:
       no VEH        -> the unhandled-exception filter runs the SAME handler. With no
                        CRT there is no SEH frame in this host to claim a fault first,
                        so the filter is the next thing after the VEH would have been,
                        and a filter may return EXCEPTION_CONTINUE_EXECUTION with a
                        modified context exactly as the VEH does.
       no Attach     -> the other stdout routes (inherited handle, the parent's handle)
       no raw input  -> g_MouseRawOk stays 0 and the absolute-derived delta path runs,
                        which is what the code already did when registration failed.
   ⚠ THIS IS "LOADS ON 2000", NOT "RUNS ON 2000". The NtVdmControl contract (the
     VdmInitialize block, VDM_TIB offsets, TEB+0xF18, [0x714]) was taken from XP's
     ntvdm and kernel; what 2000's do differently is unmeasured, and the first run's
     log is the instrument. The Win16 half is pinned to XP's krnl386 and is a
     separate effort. On XP nothing changes: the same four functions are found and
     used as before. `STAGE0: os=` names the version and which of the four resolved. */
typedef PVOID (WINAPI *PFN_ADD_VECTORED_EXCEPTION_HANDLER)(ULONG, PVECTORED_EXCEPTION_HANDLER);
typedef BOOL  (WINAPI *PFN_ATTACH_CONSOLE)(DWORD);
typedef BOOL  (WINAPI *PFN_REGISTER_RAW_INPUT_DEVICES)(PCRAWINPUTDEVICE, UINT, UINT);
typedef UINT  (WINAPI *PFN_GET_RAW_INPUT_DATA)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
/* ── ★ HOW "JUST OPEN NTVDMEX" WORKS, AND WHY IT NEEDS A FOUR-BYTE DOS PROGRAM. ──────
     Run with no arguments -- double-clicked, or from a shortcut -- this process CANNOT
     become a VDM. Measured on the rig, 2026-09-25:

         STAGE0: cmdline=["...\bin\ntvdmhost.exe" ]
         STAGE1: VdmRegisterWithKernel NTSTATUS=0xc0000022        <- STATUS_ACCESS_DENIED
         STAGE1: GetNextVDMCommand FALSE err=0x57

     ...and then it exits, silently, with no window. VDM privilege is not something a
     process can ask for: NT grants it to a process CSRSS created for a 16-bit image.
   ⇒ So the launcher does not try. It writes a four-byte DOS program -- `mov ah,4Ch;
     int 21h`, the same stub the harness has used for years -- and CreateProcess's it.
     Windows sees a DOS image, CSRSS builds a real VDM, the IFEO Debugger key routes
     `ntvdm.exe` to US, and THAT instance has the privilege. The stub itself never
     runs: we recognise its name and load a shell instead.
   ⚠ The name is the whole signal, so it must be one nothing else uses, and it must
     differ from `dosstub.com` -- the harness stub means "target.txt names the
     program", this one means "the user asked for a shell, ignore target.txt". A
     stale target.txt on someone's machine must not hijack a double-click.
   ⛔⛔ AND IT MUST BE 8.3, WHICH THE FIRST CUT WAS NOT. `ntvdmex-shell.com` came back
     from CSRSS as **`NTVDME~1.COM`**:

         STAGE1: program C:\DOCUME~1\Matthew\LOCALS~1\Temp\NTVDME~1.COM

     so the basename never matched, the branch never fired, and the launch fell through
     to running the four-byte stub for real -- a VDM that came up correctly and exited
     immediately, with nothing on screen. **The DOS side of this system sees 8.3 names
     and only 8.3 names**; the same rule already caught `p_dpmiins.com` in the probe
     harness and krnl386 in the Win16 one. Seven characters, no hyphen, no mangling. */
#define LAUNCH_STUB_NAME "ntvdmex.com"
static VOID HostProfileDump(VOID);
/* s84: open Settings at startup on the tab numbered in this file (0 = MS-DOS), so the
   rig can photograph each page without clicking at coordinates. Test-only. */
#define SETSHOT_PATH CFG_("setshot.txt")
/* ── ⚠ `/B` BOOT LOGGING WAS TRIED AND DOES NOT WORK HERE. (GH #128, session 36) ──
     krnl386 accepts the stock `WIN /B` switch in its PSP command tail; documented
     behaviour is a BOOTLOG.TXT in the Windows directory listing every `LoadStart = ` /
     `LoadSuccess = ` / `LoadFail = ` line, failure CODE included.
   ▶ MEASURED AND REMOVED. `/B` was placed in the tail (confirmed in the LDT log) and
     NO BOOTLOG.TXT appeared at any of four candidate paths. The likely reason is that
     the Windows directory is not yet known this early, so the one open fails -- and
     after a failed open krnl386 logs nothing more for the rest of the run, SILENTLY.
   ★ Forcing the logging on by hand was not pursued for the same reason: without a
     filename it has nothing to open.
   ⇒ A breakpoint on the loader's per-module return answers the same question and DOES
     work -- it reads the loader's return code per module directly. See the session-36
     log. */
/* A log NOTHING truncates. WinMain has three LogWrite calls and each wipes the file;
   diagnostics that need to survive the whole run belong here instead. */
#define LDTLOG_PATH   OUT_("ldtprobe.log")
#define DSPROBE_MAX 12
/* Offset, within DOS_HDLR_SEG, of the XMS API far-call entry stub (BOP 0x43; RETF).
   Lives just past the INT 1Ah stub (which ends at 0x40) and the INT 2Fh stub (4 bytes
   at 0x40). INT 2Fh AX=4310 hands the guest DOS_HDLR_SEG:XMS_ENTRY_OFF to far-call. */
#define XMS_ENTRY_OFF 0x0044
/* ── ★ THE CPU CLASS `INT 2Fh AX=1687h` REPORTS IN CL, AND IT IS NOT COSMETIC. ────────
     `krnl386.exe` asks `INT 2Fh AX=1687h` at start-up, and the CPU bits of the Win16
     `GetWinFlags` word (KERNEL.132) follow the CL it gets back (observed): CL == 3
     gives WF_CPU386 (0x0004), a larger CL gives WF_CPU486 (0x0008), and no DPMI host
     at all stops Windows from starting.

   ⇒ CL is an ORDINAL CPU class whose lowest accepted value is 3. That reading is
     behavioural, not off a spec sheet -- there is no DPMI document in this repo, and
     `tests/probes/dos/p_dpmins.com` confirms neither software oracle can be asked:
     **MS-DOS 6.22 and DOSBox-X both leave every register untouched** (no DPMI host),
     so only stock ntvdm can answer and that needs the IFEO bracket.

   ⛔ WE HARDCODED 3 AND IT PRODUCED A MEASURED MISMATCH. `tests/probes/win16` measured
     `GetWinFlags` = **4C25 from us**, **4C29 from stock**, reproduced twice. The single
     differing bit is 0x0004 vs 0x0008 -- WF_CPU386 vs WF_CPU486 -- so stock's DPMI
     host returns CL>3 and ours returned 3. **4 is the smallest value consistent with
     the measurement**, so it is what we report: matching the oracle without claiming
     anything the measurement does not support.
   ⚠ "4 means 80486" is the obvious reading and is NOT established by anything here.
     What IS established: stock returns >3, krnl386 treats >3 as WF_CPU486, and any
     machine that can run NTVDMEX is past a 386 several times over.
   ⚠ OBSERVABLE FOR EVERY DPMI GUEST, not just Win16 -- an extender may branch on it.
     Changed once, with an interleaved before/after on the rig (`runs/s79_cl_ab/`).
   ⛔ #248: AND ONLY ONE OF ITS TWO SITES WAS CHANGED. INT 31h 0400h reports the same CL,
     and kept the hardcoded 3 this note replaced -- one machine, described two ways to the
     same client. The define now lives in dpmi_svc.h with the rest of 0400h's answer, and
     every site (1687h, 0400h on both PM paths) reads it from there. */

/* DPMI (M4 slice 3, spike): the mode-switch entry far-called by a client after it
   detects DPMI via INT 2Fh AX=1687h. Lives past the INT 67h stub (0x48..0x4B).
   The stub is `BOP 0x50 ; RETF`: the host services the BOP by switching the VDM to
   protected mode (rewriting CS:IP), so the RETF only runs if the switch FAILS (it
   returns to the client in real mode with CF=1). See src/vdm/dpmi.c. */
#define DPMI_ENTRY_OFF 0x0050
/* DPMI 0301 (call real-mode procedure): the far-return catcher. To run a client's
   real-mode proc we switch the VDM back to V86 and push a far-return frame pointing
   here; when the proc RETFs it lands on this BOP, which the 0301 handler recognises
   as "the real-mode call finished" and switches back to protected mode. Lives just
   past the mode-switch entry (0x50..0x53). */
#define DPMI_RMRET_OFF 0x0054
#define DPMI_RMRET_BOP 0x54
/* NTVDM's own BOPs, as issued by Microsoft's 16-bit components. These are the GUEST's
   numbers -- ours above happen to overlap and are told apart by origin, not by value. */
#define NTVDM_BOP_CMD  0x54   /* XP's COMMAND.COM: 15 sites, each + a sub-function byte */
/* DPMI 0303 (allocate real-mode callback): planted real-mode BOP entries (one per
   callback slot) that a client's real-mode code far-calls; the host switches V86->PM
   and runs the client's PM handler. DPMI_PMRET is the PM-side return catcher the
   handler IRETs to (reached via g_PmReturnSelector, a code selector based at DOS_HDLR_SEG).
   All within the 0x500..0x5FF handler segment (0x600 up is the device headers, #207).
   #248: DPMI_CB_SLOTS (16, the spec's minimum -- was 4) lives in dpmi_svc.h, and the
   slots moved to 0x90..0xCF to make room; see the segment map below. */
#define DPMI_CB_BOP      0x55
#define DPMI_CB_BASE_OFF 0x0090      /* slot i entry at DOS_HDLR_SEG:(base + i*4), 0x90..0xCF */
#define DPMI_PMRET_OFF   0x0070
#define DPMI_RAW2PM_OFF  0x005C
#define DPMI_RAW2RM_OFF  0x0074
/* INT 31h 0305 state save/restore. Both procedures are FAR CALLed with AL=0 save /
   AL=1 restore, ES:(E)DI = buffer, and MUST PRESERVE ALL REGISTERS. We keep the whole
   guest register file in the VDM_TIB across every mode switch we perform, so there is
   no host-side state the client has to hand back to us -- a bare RETF is a correct,
   register-preserving implementation. Planted as data (0xCB), not a BOP: it never
   needs to reach the host at all. */
#define DPMI_SSR_OFF     0x0078
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
 *   0x58       >>> this stub <<<  0x59 AH=38h case map      0x5C-0x5E  DPMI raw RM->PM
 *   0x60-0x65  opt-in entry trampoline (qimode VIF; empty since the CB slots moved)
 *   0x70-0x72  DPMI PMRET         0x74-0x76  DPMI raw PM->RM     0x78  DPMI 0305 RETF
 *   0x80-0x82  DPMI fault BOP (code selector)
 *   0x90-0xCF  DPMI CB slots (16 x 4; #248 -- were 4 slots at 0x60-0x6F)
 *   0xE0-0xE3  INT 33h event-handler return (MS_CB_RET_OFF)
 *   free:      0x83-0x8F, 0xD0-0xDF, 0xE4-0xFF
 */
#define DOS_IRET_STUB_OFF 0x0058
/* DOS_SYSVARS_OFF lives in dos_layout.h with the rest of this segment map -- the
   MCB head at -2 and MEM.EXE's UMB word at 0x8C are both measured against it. */

/* GH #18 real-CPU PM-fault trampoline. When a raw (non-BOP) protected-mode #GP faults,
   the kernel reflect path jumps the guest to [VDM_TIB+0x638]:0x1000 (see ntvdm.h
   VTIB_FLT_*). We install a code selector H (g_DpmiFaultSelector) based at DOS_HDLR_SEG<<4
   and plant a BOP (C4 C4 57) at offset 0x1000 within it -- linear (DOS_HDLR_SEG<<4)+0x1000
   = 0x1500, in the mapped V86 window, in the unused gap below the env block (0x6000) and
   the program (PSP 0x10000). The reflect therefore surfaces to the host as VTIB_EVENT=4
   with CS==H and EIP==0x1000, and the saved faulting CS:EIP/SS:ESP sit in VTIB_FLT_SAV*.
   This routes a raw PM #GP (SS-retype, HLT, privileged op) into the same host loop that
   services the INT->BOP path -- ntvdm's mechanism (session 6), no ntvdm globals. */
/* Run 67's corrected mechanism -- the layout the kernel accepts, after runs 65/66 failed:
   - [TIB+0x638] is the fault handler's STACK selector (writable-DATA); the reflect sets
     new SS:ESP = [TIB+0x638]:0x1000 and builds an IRET frame there. A CODE selector there
     is REJECTED -- which is why runs 65/66 failed.
   - The handler CS:EIP comes from a table pointed to by [VDM_TIB+8], indexed by fault
     CLASS (6 for a #GP, observed), stride 0x10: entry = {CS@+0 word, EIP@+4 dword}. The
     CS must be a code selector and the EIP within its limit, or the reflect is refused.
   So we need TWO selectors + a table: a data stack selector and a code selector with a BOP,
   and the table[6] = {code_sel, bop_off}, with [VDM_TIB+8] pointing at the table. */
/* ── #205: THE FAULT STACK WAS IN THE PROGRAM. (s85) ──────────────────────────────────
     It was based at linear 0x2000 (segment 0x200), "below the program" when programs
     loaded at segment 0x1000. They load at ~0x243 now, so every reflected PM fault
     wrote its frame at 0x2FC0..0x2FDF -- inside the running program's own image. Typed
     at 6.22's COMMAND.COM, DOS/4GW's data segment starts at 0x2530 and its selector-table
     pointer ([0AA2h]) is 0x2FD2: the reflect of its first raw INT 21h zeroed it, and its
     next allocation stored through ES=0 (#GP, exit FFh). Under XP's larger shell the
     program sits 448 bytes higher and the frame missed. Watched on the rig: the byte
     went 0xAF -> 0x00 across exactly that reflect.
   ► The stack now lives in the HOST image (g_FaultStack), where no guest can own it --
     like the class table g_FaultTable beside it. 64 KB because the selector's limit is
     0xFFFF and it is a 16-bit stack: even a wrapped SP stays inside the buffer. The top
     is still :0x1000, the 4 KB a DPMI 0.9 exception handler is promised. */
#define DPMI_FAULT_STK_SIZE 0x10000
#define DPMI_FAULT_COFF    0x0080    /* BOP offset within the handler code selector (linear 0x580) */
#define DPMI_TIB_FLTTBL    0x08      /* VDM_TIB offset holding the handler-table pointer    */
/* Offset, WITHIN g_DpmiFaultCodeSelector (base DOS_HDLR_SEG<<4), of fault class i's own BOP
   site. The sites themselves live at DOS_CTAB_SEG:DOS_FLTSITE_OFF -- see dos_layout.h for
   why the class has to be distinguishable at all. The handler segment cannot host them:
   it is 256 bytes with 16 free, and eight 3-byte BOPs do not fit. */
#define DPMI_FAULT_SITE(i) (((DOS_CTAB_SEG << PARAGRAPH_SHIFT) - (DOS_HDLR_SEG << PARAGRAPH_SHIFT)) \
                            + DOS_FLTSITE_OFF + (i) * DOS_FLTSITE_SIZE)
#define DPMI_FLTRET_COFF   (((DOS_CTAB_SEG << PARAGRAPH_SHIFT) - (DOS_HDLR_SEG << PARAGRAPH_SHIFT)) + DOS_FLTRET_OFF)

enum { WOW_SHIMS = 2, SHIM_NOT_TRIED = 0, SHIM_LOADED = 1, SHIM_NO_LOAD = 2, SHIM_INIT_REFUSED = 3 };   /* WOW32.DLL and NTVDM.EXE's shims, and how each went */
enum { WOWFOLD_MUTE_NONE = 0, WOWFOLD_MUTE_FOLD = 1, WOWFOLD_MUTE_DROP = 2 };   /* g_WowFoldMute: log in full, fold to the verdict, drop */
static VOID WowIcaDeliver(DOS_MACHINE *machine, volatile BYTE *tib, UINT steps);
/* Floor on how often the WM_TIMER body may do its frame work, in ms. 15 is the ~64 Hz
   this always actually ran at under XP's default granularity. Knob: uitick.txt.
   ★ 0 = AUTO (s73, the default): the present is RAISED BY THE GUEST'S FRAME -- the
     video VDD fires present_hook from the guest's own 0x3DA poll when the beam enters
     the present window, WM_APP_PRESENT runs the frame body at once, and the timer is
     only a fallback (floor = 90% of the mode's frame period) for a guest that never
     looks at the retrace. Two clocks became one; see present_hook in vdd_video.h. */
#define UITICK_AUTO 0
enum { KEYIRQ_RETRY_OFF = 0, KEYIRQ_RETRY_ON = 1, KEYIRQ_RETRY_CLOCK_ON_SCHEDULE = 2, KEYIRQ_RETRY_ONE_YIELD = 3 };   /* keyirq.txt; 2 is refuted (see HostPitDeliver) */
static UINT32 QpcMicroseconds(LONGLONG ticks);          /* fwd: defined with the lock instruments */
static VOID ModeYTimelineReport(VOID);          /* fwd: north star 1, with the mode-Y remap */
static VOID GusReport(VOID);               /* fwd: north star 2, with the GUS globals */
static INT  ModeYInterpServes(VOID);      /* fwd: north star 1, design C */
static VOID ModeYRingDump(PCSTR why);  /* fwd: north star 1, design C */
static VOID ModeYRingNoteIrq(UINT vector, WORD cs, WORD ip, WORD ss, WORD sp);
static INT HostReadable(PCVOID pointer, SIZE_T length);  /* fwd: defined with the VEH */
/* __LINE__ gives every site its own identity without touching 28 call sites by hand. */
#define HOST_LOCK()   HostLockEnter(__LINE__)
#define HOST_UNLOCK() HostLockLeave()
/* ── THE INT->BOP PATCH MAP, KEYED BY LINEAR ADDRESS. ─────────────────────────────
   It used to be keyed by OFFSET INTO A SINGLE 64K WINDOW at g_DpmiCodeBase, and that
   is exactly as much of the guest as the one up-front scan could reach. Session 17
   measured what that misses: DOS/4GW allocates conventional memory with INT 21h AH=48h,
   READS A PROTECTED-MODE MODULE INTO IT from DOOM.EXE, retypes the descriptor to code
   (INT 31h 0009, access 0xFB) and far-jumps in. That module's first instruction is
   `mov ah,0x30 / CD 21` -- a RAW int, outside the window, never patched. A raw INT in
   PM raises a #GP the kernel will not reflect, so the VDM died silently, at the same
   address, every run since session 15.
   Keying by linear address lets the map cover every region the client later declares to
   be code, and makes DpmiBopVector's alias handling fall out for free. Sized to the V86
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
enum { BREAKPOINT_MODE_INT3 = 1, BREAKPOINT_MODE_WOW_SEGMENT = 2, BREAKPOINT_MODE_DUMP_DS = 4, BREAKPOINT_MODE_LE_CODE = 8 };   /* pmbreak.txt column 4: 2 takes the segment number in bits 4-7 */
#define DPMI_BP_VEC  0xEE               /* sentinel in g_int_vec[]: not a real vector */
#define DPMI_BP_MAX  32
/* And the same for every segment: krnl386 copies each one to a block of its own and
   commits a code selector over it. Indexed by segment number - 1; 0 = not seen yet. */
#define WOW_PMBASE_MAX 8
enum { CAPTURE_MS_DEFAULT = 300 };   /* capture.flag empty: a shot this often */
/* ── ★ IS THE GUEST'S CLOCK EVEN? A TOTAL CANNOT ANSWER THAT. ────────────────────
 *  The user reports Skyroads "slow down / speed up" -- a RATE THAT VARIES -- and
 *  every instrument this host has for IRQ0 is a COUNT. `irq0_inj 4485` is the same
 *  number whether the ticks arrived evenly or in a clump followed by a stall, and
 *  session 22 already had to reconstruct the distribution by hand to find the audio
 *  defect at the DMA block boundary. Counting harder cannot see unevenness; only
 *  the distribution can.
 *  ⇒ Two shapes, because they answer different questions:
 *      TIMELINE   deliveries in each whole second of the run. A slow-down/speed-up
 *                 is visible directly as the sequence moving -- 180,180,61,204,90 --
 *                 and no other instrument here would show it at all.
 *      GAPS       the inter-delivery interval histogram. Says whether unevenness is
 *                 a few long stalls or continuous jitter, which need different fixes.
 *  ⚠ RATE, NOT COST: ~180 deliveries a second, so one QueryPerformanceCounter each
 *    is free -- and unlike the reflected-INT trace this cannot be driven faster by
 *    the guest, because it is bounded by the PIT rate the guest itself programmed.
 *    (See [[guest-trace-can-outrun-the-guest]] for why that distinction matters.)
 *  ⚠ FIXED WINDOW, so a long run cannot grow it. Past IRQ0TL_SECS the timeline
 *    simply stops recording; the gap histogram keeps going. */
#define IRQ0TL_SECS 90
static INT InterpreterMemoryPageOk(UINT32 linear);        /* fwd: page-validity guard, defined with V86HostRead8 */
#define IO_HOT_MAX 48   /* 12 filled up before the hottest port was even seen */
#define EV_HIST_MAX 16
/* ...and the same, second by second, so a run with two phases (3DBench: a title wait
   spinning on INT 16h, then the benchmark) is not read as one average. Cumulative
   snapshots at the first VdmRunGuest return of each second; the report prints deltas. */
#define XS_SECS 40
enum { XS_RAISE, XS_ASYNC, XS_COOP, XS_NIE, XS_BOP, XS_IO, XS_HOSTMS, XS_PACE, XS_N };
/* The cooperative IRQ0 gate's IF refusals inside one of our stubs, by the caller's
   return CS:IP (the INT's frame) and the stub offset. */
#define SKIPIF_SITES 6
static struct { WORD Cs, Ip, Stub; DWORD Count; } g_SkipIfSite[SKIPIF_SITES];
static struct { WORD Port; DWORD Count; } g_IoHot[IO_HOT_MAX];
#define IO_UNCLAIMED_MAX 24
#define PM_HEADLESS_MS_DEFAULT 30000             /* headless exec-loop wall-clock cap (an infinite visual demo self-exits) */
#define PM_HEADLESS_MS g_HeadlessMs
static DWORD DpmiSelectorBase(WORD selector);       /* fwd: watchdog resolves the frozen selector base */
/* DPMI PM interrupt-vector table (INT 31h 0204/0205). A client installs its own PM
   handlers here; we store them so a get/set/restore round-trips faithfully. (We still
   service patched INT 21h/31h ourselves -- routing to a client-installed PM handler is
   a deeper item; storing the vectors is what real extenders' save/restore needs.) */
/* `client` distinguishes a vector the CLIENT installed (0205 / INT 21h AH=25h) from
   the host default we pre-load into every entry at mode-switch time. The difference is
   load-bearing in two places: we only ROUTE an interrupt to a handler the client chose,
   and we only INJECT IRQ0 into an INT 08h the client actually hooked -- injecting into
   our own default would be a very confusing way to talk to ourselves. */
static struct { WORD Selector; DWORD Offset; BYTE Client; } g_PmInt[IVT_VECTORS];
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
   address is registered in the patch map against its vector, so DpmiBopVector() resolves
   a chained call straight back to the service the client was asking for. */
/* Every block we have handed the client through INT 31h 0501. We know exactly which
   memory is the client's because we allocated it -- and that is the only usable answer
   when the client declares a FLAT code selector. See DpmiPatchCodeRegion().
   `code` marks a block that holds one of the program's EXECUTABLE objects, matched by
   size against the LE object table -- see DpmiLeLearn(). */
#define DPMI_MEMBLK_MAX 64
static struct { DWORD Base, Size; BYTE Code; } g_DpmiBlock[DPMI_MEMBLK_MAX];
/* ── WHAT THE CLIENT OWNS, SO IT CAN BE GIVEN BACK WHEN IT EXITS. (s80) ─────────────
     g_DpmiBlock[] is the patcher's list: capped at 64 and never told about 0502, so it
     cannot say what is still live. These two can. DpmiClientTeardown() releases
     whatever is left in them, which is what a DPMI host does when its client ends --
     and what lets the NEXT client (the user runs Doom twice) find memory at all.
   #248: g_DpmiOwned[] is now also THE HANDLE RECORD -- 0502h and 0503h accept only a
     handle that is in it (8023h otherwise), because a handle here is a host address and
     VirtualFree() on an arbitrary one would release OUR memory. So it may not silently
     overflow any more: 0501h refuses (8016h) when it is full. 4096 live blocks is ~36x
     the most any shelf guest has held (ZAR, 114 allocations in a whole run). */
#define DPMI_OWNED_MAX 4096
#define DPMI_DOSBLK_MAX 64
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
/* When the client last installed a PM INT 08h handler, and how long IRQ0 injection must
   then hold off. One 18.2 Hz tick period is 54.9 ms -- the shortest gap real hardware can
   put between "the vector exists" and "the timer fires". See INT 31h 0205. */
#define DPMI_IRQ0_ARM_QUIET_MS 55
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
#define DPMI_IRQ_TARGET_SEL(iv) (g_PmInt[iv].Selector)
#define DPMI_IRQ_TARGET_OFF(iv) (g_PmInt[iv].Offset)

#define DPMI_PMDEF_STRIDE 3
/* DPMI PM EXCEPTION-handler table (INT 31h 0202/0203). Separate from g_PmInt on
   purpose: 0202/0203 address CPU exceptions 00h-1Fh, which are a different namespace
   from the interrupt vectors 0204/0205 addresses -- a client may legitimately install
   a #GP (0Dh) exception handler and an INT 0Dh (IRQ5) interrupt handler at once, and
   collapsing them into one table would make each silently overwrite the other.
   Doom's DOS/4GW makes 45 of these calls -- the biggest single block of UNSUP in the
   session-16 trace -- installing its own fault handlers before it runs the game. */
static struct { WORD Selector; DWORD Offset; INT IsSet; } g_PmException[X86_EXCEPTIONS];
/* DPMI 0303 real-mode callbacks: each slot records the client's PM handler (sel:off)
   and the RMCS buffer (sel:off) to marshal register state through. g_PmReturnSelector is a
   code selector based at DOS_HDLR_SEG (0x500) so the PM handler's IRET lands on the
   planted DPMI_PMRET catcher; allocated lazily on the first 0303. */
static struct { WORD PmSelector; DWORD PmOffset; WORD RmEs; DWORD RmDi; INT IsUsed; } g_Callbacks[DPMI_CB_SLOTS];
#define PROGRAM_NAME_SIZE 64
static VOID ExecMachineSave(INT depth);    /* fwd: defined with CloseProgramNow */
enum { DPMI_FAULT_TABLE_ENTRY = 0x10, DPMI_FAULT_TABLE_OFFSET = 4 };   /* g_FaultTable: per class, the code selector then the offset DWORD */
static BYTE  g_FaultTable[DOS_FLTSITE_N * DPMI_FAULT_TABLE_ENTRY] __attribute__((aligned(16)));
static BYTE  g_FaultStack[DPMI_FAULT_STK_SIZE] __attribute__((aligned(16)));   /* #205 */
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
/* Indices below this belong to the host: 0 null, 1/2 the initial CS/DS, and 3 which
   DpmiInstall() force-types to writable data. INT 31h 0001 refuses to free them and
   the WOW selector stage refuses to allocate them -- one constant so the two agree. */
#define DPMI_LDT_RESERVED 6
#define DPMI_HOSTPOOL_HI  0x2b
#define DPMI_LDT_FIRSTFREE (DPMI_HOSTPOOL_HI + 1)
static struct _DPMI_DESCRIPTOR { DWORD Base, Limit; BYTE Access, Flags; } g_Ldt[DPMI_LDT_MAX];
enum { SETTINGS_APPLY_STARTUP = 0, SETTINGS_APPLY_LIVE = 1 };   /* SettingsApply: before anything is built, or on a running VM */
static INT AsyncVectorIsOurStub(UINT irq);

enum { IFV_PATH_LIVE = 0, IFV_PATH_VTIB_IRQ01 = 1, IFV_PATH_VTIB_DEVICE = 2, IFV_PATHS = 3 };   /* the IF/VIF census's paths: live (async), IRQ 0/1 via the VTIB, a device IRQ via the VTIB */
enum { ASYNC_WHY_NOT_IN_EXEC = 20, ASYNC_WHY_PIC_REFUSE = 21, ASYNC_WHY_UNHOOKED = 22, ASYNC_WHY_SUSPEND_FAIL = 23, ASYNC_WHY_GETCTX_FAIL = 24, ASYNC_WHY_V86_IF_OFF = 25, ASYNC_WHY_IN_OUR_HANDLER = 26, ASYNC_WHY_OBSERVED = 27, ASYNC_WHY_CTX_BUSY = 28, ASYNC_WHY_LEFT_EXEC = 29, ASYNC_WHY_SIMINT_RM = 30, ASYNC_WHY_NESTED_TICK = 31, ASYNC_WHY_PM_ONLY_LINE = 32, ASYNC_WHY_BAD_IRQ = 40 };   /* 20+: AsyncInjectIrq's early exits; 32 and 40 are past the histogram */
static VOID PokeWord(DWORD linear, WORD value);        /* fwd: guest-memory helpers, defined below */
static WORD PeekWord(DWORD linear);
static VOID HostPitSync(VOID);             /* fwd: the guest's clock, driven by both threads */
static VOID HostPitGenerate(VOID);         /* fwd: the crystal half (g_PitCs only)  */
static VOID HostPitDeliver(VOID);          /* fwd: the attempt half (g_Lock, by TRY) */
static INT  V86DeliverDeviceIrq(volatile BYTE *tib);  /* fwd: shared by the main and nested V86 loops */
enum { ASYNC_DELIVERED = 0, ASYNC_WHY_BAD_VECTOR = 1, ASYNC_WHY_IN_PM_IRQ = 2, ASYNC_WHY_PM_NO_IRQ = 3, ASYNC_WHY_NO_CATCHER = 4, ASYNC_WHY_UNHOOKED_PM = 5, ASYNC_WHY_NO_APP_TIMER = 6, ASYNC_WHY_VIF_OFF = 7, ASYNC_WHY_IF_OFF = 8, ASYNC_WHY_ARM_QUIET = 9, ASYNC_WHY_IN_FLIGHT = 10, ASYNC_WHY_HOST_STACK = 11, ASYNC_WHY_NOT_32 = 12, ASYNC_WHY_SETCTX_FAIL = 13, ASYNC_WHY_HOST_CS = 14 };   /* g_AsyncWhy: AsyncWhyReport's whyNames, 0-14 */
static INT  DpmiAsyncInjectPm(UINT irq, CONTEXT *context);
/* ── ...AND `g_AsyncWhy` ALONE STILL CANNOT ANSWER THE QUESTION THAT MATTERS. ───────
     It holds the LAST refusal, so a run can say "62 attempts, 56 delivered" and not say
     which clause consumed the other six -- nor, far more importantly, what the refusal
     PROFILE looks like when the sync rate itself is the binding constraint. Session 23
     measured that tripling the attempt budget moved delivery by one tick per second:
     the ceiling is not how often we ask, it is how often the guest is in an injectable
     state, and "injectable" is a dozen different conditions wearing one number.
     So keep the whole distribution, per LINE -- the timer and the Sound Blaster fail for
     different reasons and averaging them together hides both. Three buckets need
     OPPOSITE fixes and only a histogram tells them apart:
       10  g_AsyncPmActive   an injection is still in flight -> the guest's ISR is slow
                               to IRET, and MORE attempts can never help
       21  VddPicCanDeliver IRQ0's in-service bit is still set -> we are not seeing the
                               guest's EOI, which would be OUR bug, not a rate one
       7/8 virtual-IF clear    the client has interrupts off -> only the cooperative path
                               can ever deliver
       14  host CS             the CPU thread was inside the HOST, not the client, when
                               the clock asked -- the g_Lock starvation showing up here
     Bucket 0 is delivery. Two DWORDs per line per code is 1 KB of BSS, no lock (the
     timer/UI thread is the only writer; a torn count would cost a unit, not a wrong
     conclusion) and no I/O, so it costs nothing at the PIT's rate. */
#define ASYNC_WHY_MAX 32
/* Deliveries, with the gate's view of them: enough to see a handler re-entered and what
   the flags said when it was. A delivery that lands within 0x60 bytes past its OWN
   vector's entry point is, to a first approximation, the handler being re-entered:
   counted per line and always traced (the first eight of any kind are traced too, for
   context). This is the instrument that found irq8.nested's path; it stays as a detector. */
#define IFV_TRACE_MAX 40
static struct { BYTE Irq, Path, State; WORD Cs, Ip; DWORD Flags; } g_IfvTrace[IFV_TRACE_MAX];
#define DPMI_WATCH_MAX 4
/* ...and when `decl` dominates (it did once the latch was fixed: tried=0x171c decl=0x169d),
   WHICH refusal inside DpmiInjectPmIrq: [0] the interrupted CS is 16-bit (the
   extender's code), [1] the application has no timer hook. Per second on IRQ0TL's clock,
   and the 16-bit sites by CS:EIP, so the refusals can be laid against the quit wait. */
#define PMINJ_SITES 6
static struct { WORD Cs; DWORD Eip, Count; } g_PmInjectSite[PMINJ_SITES];
/* Record and (boundedly) report an async attempt that gave up BEFORE the guest context
   was ever inspected. Bounded for the same reason the PM bail log is: these fire at the
   PIT's rate, so an uncapped line per tick would bury the run it exists to explain. The
   cap is generous enough to span a whole 45s headless run. */
/* Enough to show WHEN the bails start and what the first ones are; the totals live in
   g_AsyncWhyHistogram, which costs nothing. See AsyncEarlyBail() for what 4000 cost. */
#define ASYNC_EARLY_BAIL_LOG_MAX 32
/* Paired with the InterlockedCompareExchange below; every path that resumed the guest
   must drop ownership. See g_AsyncContextWrite. */
#define ASYNC_CTX_RELEASE() InterlockedExchange(&g_AsyncContextWrite, 0)
static VOID MouseChildExited(VOID);          /* fwd: see g_MouseWantRelease */
/* ── #153: FILE > OPEN RECENT. Every program this host was started with, and every one
     opened from the menu, newest first, `Recent1`..`Recent8` beside the settings.
     (A Win16 program started from Windows is not recorded: its path arrives inside
     WOW, not here.) */
#define MRU_MAX 8
enum { INSTALL_VERB_NONE = -1, INSTALL_VERB_INSTALL = 0, INSTALL_VERB_UNINSTALL = 1, INSTALL_VERB_STATUS = 2, INSTALL_VERBS = 3 };   /* InstallVerb: the verbs[] index */
static VOID InstallReport(PCSTR message, INT isOk);

static HANDLE StdioPebHandle(HANDLE proc, UINT offset);

enum { INPUT_KEY_WAIT_MS = 50 };   /* a blocking key read's wait for the next key */
static VOID HostRecordFinish(VOID);        /* below: patches the header, logs */
#define TYPEMATIC_DEFAULT_PERIOD_US 92000u
#define TYPEMATIC_DEFAULT_DELAY_US  500000u   /* until XP's own setting is read at startup */
static VOID HostMouseButton(INT button, INT down);
enum { I33_FALLBACK_X = 320, I33_FALLBACK_Y = 240, I33_ABSOLUTE_DELTA_SCALE = 8 };   /* I33TakeMotion's absolute-derived fallback */
/* --- menu + status bar (scaffold; most items are stubs for now) ------------ */
static CHAR g_ProgramName[64] = HOST_PROGRAM_NONE;      /* first part of the status strip    */
/* ── `i33oth=1079` IS NOT A MEASUREMENT, IT IS A BUCKET. ──────────────────────────
     A thousand INT 33h calls arrive with AX >= 0x10 and the histogram above lumps
     every one of them together, so it cannot tell the two live explanations apart --
     and they need OPPOSITE fixes:
       (a) Doom calls driver functions we do not implement. `MouseInt33`'s
           `default: break;` accepts them silently and returns nothing: the same
           "does nothing, reports success" shape as the 0300 bug below.
       (b) A MIS-PATCHED `CD 33` SITE. The DPMI host rewrites `CD nn` into BOPs, so a
           false positive inside data or mid-instruction calls us with ARBITRARY EAX --
           which is exactly what "a thousand calls with AX >= 0x10" looks like. That
           is the class of bug that killed Doom for five sessions (see x86len.h).
     The two have different SHAPES and the shape is the discriminator: a real function
     set is a handful of plausible values from a handful of sites; a mis-patch is
     scattered values, and its site is not a `CD 33` in DOOM.EXE.
   ★★ ANSWERED 2026-09-09, AND IT IS NEITHER OF THE TWO. The histogram this note asked
     for was read off a real Doom run and it is small and entirely sensible:
         0000 x1   0015 x1   53c1 x1   0003 x1338   000b x1338
     Doom probes ONCE (00 reset, 15 get-storage-size, 53c1) and then polls position
     and relative motion every frame. `AX=53c1` -- the value that made "AX >= 0x10"
     look like a thousand scattered calls -- is LOGITECH CYBERMAN SWIFT DETECTION, a
     real and documented probe, and Doom announces the result itself in the same log:
         CyberMan: Wrong mouse driver - no SWIFT support (AX=53c1).
     So the bucket was hiding one legitimate call made once, not a mis-patch and not
     a set of unimplemented functions. ⇒ NO FIX IS NEEDED HERE. The lesson is the one
     this note already carries -- the bucket, not the thing bucketed, was the defect.
   ► AND THE HISTOGRAM IS NOW LOAD-BEARING: the auto-capture trigger keys on exactly
     the USE functions (01/03/05/06/0B) and deliberately not on 00/15/53c1, which is
     only defensible because this measurement says which is which. See
     g_MouseWantCapture.
   ⚠ SO RECORD BOTH, AND DO NOT GUESS BETWEEN THEM. The AX values as they actually
     are (sparse, not bucketed), and WHERE the caller was -- linear address, which
     entry path, and the bytes around the site so it can be diffed against the file
     on disk without another run. That diff is the session-21 method and it found the
     last mis-patch inside an hour. */
#define I33_AXN   24                        /* distinct AX values kept                */
#define I33_SITEN 12                        /* distinct caller sites kept             */
static struct { WORD Ax; DWORD Count; }  g_MouseI33Ax[I33_AXN];
static struct { DWORD Linear, Eip, Count; WORD Cs, Ax; BYTE Source; BYTE Context[12]; } g_MouseI33Site[I33_SITEN];
/* Which arm called us -- a mis-patch can only arrive through the PM BOP, and 0300's
   site is the INT 31h thunk rather than a `CD 33` at all, so the path is evidence. */
#define I33_SRC_V86  1                      /* V86 BOP (patched `CD 33` in real mode) */
#define I33_SRC_PM   2                      /* PM BOP  (patched `CD 33` in PM code)   */
static INT DpmiSelectorIs32(WORD selector);
/* ── ★★ A CLICK IS AN EVENT, AND WE WERE ONLY EVER REPORTING A LEVEL. ────────────────
     `g_MouseButtons` is a sample of the MK_* bits taken whenever a mouse message happens to
     arrive, and INT 33h 05h/06h -- "how many times has this button been pressed /
     released SINCE YOU LAST ASKED, and WHERE" -- was hardcoded to answer zero: the
     old arm set EBX to a literal 0 with the comment "0 presses since last call"
     beside it. That is the whole reason a guest which detects clicks the ordinary way
     sees NONE. It is not a partial implementation, it is a confident wrong answer.
     A level sample cannot substitute either: press and release between two of the
     guest's polls and the click never existed. ZAR is exactly this shape.
   ► So COUNT THE EDGES on the UI thread, where the transitions actually arrive, and
     record the POSITION AT THE TRANSITION -- 05h/06h report where the button went
     down, not where the pointer has drifted to since. Drained by the read, because
     "since the last call" is the contract.
   ⚠ ONE SOURCE OF EDGES ONLY. Raw input (WM_INPUT) also carries button transitions in
     usButtonFlags, and we deliberately do NOT read them: RegisterRawInputDevices is
     called with dwFlags = 0, so the legacy WM_?BUTTON* messages still arrive as well,
     and counting both would double every click. Legacy is the single writer. */
#define MS_BTNS 3                           /* left, right, middle                     */
enum { MOUSE_CB_WHY_IN_FLIGHT = 0, MOUSE_CB_WHY_NO_EVENTS = 1, MOUSE_CB_WHY_NO_HANDLER = 2, MOUSE_CB_WHY_IN_STUB = 3, MOUSE_CB_WHY_IF_OFF = 4, MOUSE_CB_WHY_STUB_CLOBBERED = 5, MOUSE_CB_WHY_COUNT = 6 };   /* g_MouseCallbackWhy */
/* ── ★ ...AND NOW IT IS CALLED (s71). ──────────────────────────────────────────────
     QB.EXE, edit.com and every other Microsoft text-mode UI take their mouse THROUGH
     THIS HANDLER: they install it with 0Ch and then poll their own flags, calling 03h
     only a handful of times per run (measured: 20 calls in a whole QBasic session).
     With the handler stored and never invoked, the pointer moved and no click ever
     reached the program -- "none of the menus worked".
     The driver calls the handler from its own interrupt context with AX = the event
     bits that fired (bit 0 motion, 1/2 left down/up, 3/4 right, 5/6 middle), BX = the
     button state, CX/DX = the virtual position, SI/DI = the mickey counts, DS = the
     driver's data segment; the handler returns with RETF. We do it at the same place
     and under the same gate as an injected IRQ: at an exec-loop boundary, interrupts
     enabled, not inside our own timer/keyboard stubs. The interrupted context is saved
     HOST-SIDE in full, a far return to DOS_HDLR_SEG:MS_CB_RET_OFF is pushed, and the
     BOP there restores the context -- so the guest's stack carries only the return
     address and nothing we save can be corrupted by the handler. One in flight at a
     time; a handler that never returns is timed out and counted, not waited for.
   ⚠ V86 only. A protected-mode client's handler lives at a selector:offset and needs
     the DPMI callback path; those are counted (cb_pm) so the gap stays visible. */
/* ⚠ 0x5C WAS THE FIRST CHOICE AND IT IS DPMI_RAW2PM_OFF -- planted LATER in start-up,
     so the mouse handler's RETF landed on a raw mode-switch BOP, which for a program
     that is not a DPMI client falls through to INT 21h with AH = the event bits = 0:
     "DOS terminate". QBasic died the instant the mouse moved (s71, twice). The
     handler segment has no single map of its slots; the check before "running .EXE"
     now verifies this stub survived every later planting.
   ⚠⚠ 0x12 WAS THE SECOND CHOICE, AND THE GUEST OVERWROTE IT. Segment 0050 is not
     ours: linear 0500..05FF is the DOS/BIOS communication area, and 0050:0010..0021
     are BASIC's documented slots -- 0010 its DS, 0012 the saved INT 1Ch vector, 0016
     INT 23h, 001A INT 24h. QBasic IS BASIC: measured on the rig (s71, headless
     qbclick.bat) the bytes at 0050:0012 read `3a 00 50 00` = 0050:003A, our INT 1Ch
     stub, stored there by QB at start-up. The handler's RETF then executed data, the
     return BOP was never reached, the callback stayed "in flight" for the whole run
     and every click after it was refused -- "the mouse opens nothing".
   ⚠⚠⚠ 0x100 WAS THE THIRD CHOICE AND IT IS THE ENVIRONMENT BLOCK. DOS_ENV_SEG is
     0x0060, i.e. linear 0x600 = 0050:0100 -- the segment overlaps its own env one
     paragraph on. (#207: the env has since moved to 0x7F and 0x600 holds DOS_DEV_SEG's
     device headers -- the same rule, a different tenant.) The re-check (added the same session) caught it at once: the bytes
     read `43 4f 4d 53` = "COMS" (COMSPEC). So segment 0x50 is usable ONLY for offsets
     0x00..0xFF, and inside that BASIC scribbles the low slots and DOS owns the stubs.
     0xE0 is the gap: past the sysvars block (0x8E..~0xD0) and below the env at 0x100,
     touched by no planting of ours (the next is DOS_FLTSITE_OFF at 0x260, in DPMI
     mode) and by nothing a DOS program documents. AND the bytes are re-verified at
     every injection (MouseCallbackTry), because segment 0x50 is guest-writable and a
     fixed offset is a hope, not a guarantee -- the check turns a crash into a
     counted, named refusal. */
#define MS_CB_RET_OFF   0x00E0          /* DOS_HDLR_SEG:00E0 = BOP MS_CB_BOP ; iret   */
#define MS_CB_BOP       0x35
typedef struct { LONG Bits, Buttons, X, Y; } MOUSE_EVENT_ENTRY;
static VOID VideoTrapSync(VOID);             /* fwd */
static WORD DpmiSegmentToDescriptor(WORD segment);
/* ── WE CANNOT HAND A WIN16 LAUNCH BACK. MEASURED, THREE WAYS. ───────────────────
     This function used to try. It does not any more, because relaunching stock
     ntvdm is not merely unimplemented -- it is impossible through this mechanism,
     and leaving hopeful code here would be the "does nothing, reports success"
     shape the rest of this project bans. What was eliminated, on the rig
     (2026-08-26, GH #129):

     1. SPAWN `System32\ntvdm.exe` DIRECTLY -- cannot: the IFEO Debugger value is
        keyed on the image NAME and is evaluated inside CreateProcess, so this
        re-enters US immediately. A fork bomb on every Win16 launch.

     2. SPAWN A RENAMED COPY -- tried; child exits rc=0xFF at once and the program
        never appears. Not a STARTUPINFO problem: passing our real one through
        (console handles, window station, desktop) gave the identical result.

     3. ⇒ THE ROOT CAUSE. A renamed ntvdm CANNOT BE A VDM AT ALL. Pointing
        `Control\WOW\wowcmdline` straight at a byte-identical copy under a different
        name -- no IFEO involved, Windows launching it itself -- makes Windows
        refuse the Win16 program outright:
            "C:\WINDOWS\system32\sysedit.exe is not a valid Win32 application."
        So Windows validates the VDM image's identity, and rc=0xFF in (2) was that
        same rejection seen from the other side.

     Both exits are therefore closed: the real name re-hooks us, and any other name
     is not accepted as a VDM.

   ► ONE CANDIDATE REMAINS, UNTESTED: delete the IFEO value, spawn the real
     ntvdm.exe, restore the value. It is a race -- a DOS launch landing in that
     window would silently get stock -- which is why it has not been done casually.
   ► THE REAL ANSWER IS #128: implement WOW, and this becomes the dispatch point
     rather than a dead end. The detection above is what that will hang off. */
/* The loaded modules, at file scope: selector allocation happens in a LATER stage of
   WinMain -- DpmiInstall and the LDT pool are defined further down, and entering
   protected mode needs the VDM registered first -- so the result of the load has to
   outlive this call.

   ⚠ TWO PHASES, AND THE ORDER IS FORCED. An import is patched as
     target-selector:offset, so every module's selectors must be final before ANY
     module is relocated. The earlier version of this code relocated at load time with
     placeholder segment values and relocated AGAIN once selectors existed. That is
     broken and tests/unit/ne_test.c now proves it: a chained record finds its next
     site by reading the word AT the current site, and the first pass has overwritten
     exactly those words with addresses. The second pass follows garbage. So:
        wow_load_modules()   -- parse, allocate, copy bytes.  NO relocation.
        wow_bind_modules()   -- selectors for everything, then relocate ONCE. */
#define WOW_MAX_MOD 16       /* krnl386 + the ten siblings, with headroom */
#define WOW_PATH_PARAS      0x20               /* the path buffer: one paragraph-run, one purpose */
#define WOW_ENV_PARAS  0x100                 /* 4 KB -- a DOS environment and then some */
/* ── ★ EVERY PSP THIS HOST BUILDS, SO ITS ENVIRONMENT FIELD CAN BE RE-READ LATER.
     `PSP+0x2c` is the field two separate faults turned on, and the question that
     could not be answered from a fault dump is not "what is it now" but "who
     changed it after we wrote it". One selector and one linear address per task is
     enough to print the answer at every fault, which is the difference between
     watching the field and inferring it from the code that might write it. */
#define WOW_PSP_TRACK 4
static INT HostReadable(PCVOID pointer, SIZE_T length);   /* fwd: defined with the
                                                             other memory probes */

#define HOST_KEY_DOWN_BIT       0x8000 /* GetKeyState / GetAsyncKeyState: held now   */
enum { HOST_INSTANCES_MAX = 16, HOST_INSTANCE_WAIT_MS = 200, CAPTURE_MS_MIN = 50, CAPTURE_MS_MAX = 60000, CAPTURE_DELAY_MS_MAX = 600000, HEADLESS_MS_MAX = 3600000 };   /* startup limits: instance numbers, knob ranges */
static VOID HostFullscreenToggle(HWND window);
/* The display half. Separate because the UI thread builds its presenter long after
   WinMain reads the registry, and PresentDdrawInitialize() zeroes its own struct. */
/* ── ★ ddrawfs.flag -- GO BACK TO EXCLUSIVE DIRECTDRAW FULLSCREEN. ───────────────────
     OFF by default, and the default is the whole point: the exclusive path's stretch
     blt is filtered by the driver and cannot be told not to be, which is what made
     fullscreen blurry when the identically-scaled WINDOW was sharp. Borderless-window
     fullscreen through the GDI path has none of that.
     Kept as a knob rather than deleted because "no tearing" was the exclusive path's
     original argument and a file is enough to get it back for a comparison. */
#define DDRAWFS_FLAG CFG_(KNOB_FILE_DDRAWFS)

#define WINDOW_SETTING_UNSET_U 0xFFFFFFFFu   /* g_WindowSizeLive / g_AspectLive: nothing applied yet */
static VOID ModifierTrack(BYTE rawScancode, INT extended, INT down);
/* What follows the guests' 3DAh reads, site by site: the loops RetraceIdle() must recognise
   are the ones real programs use, so they are MEASURED here, not assumed (STAGE2). */
#define RT_SITES 8
static struct { DWORD Cs, Ip, Count; BYTE Bytes[10]; } g_RetraceSite[RT_SITES];
/* PM variant of HostTryIo (GH #18 run 72). A real-CPU PROTECTED-MODE IN/OUT is
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
#define MODEY_ROW_BYTES 80u   /* a mode-Y row: 320 pixels across four planes */
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
/* ── ★ NORTH STAR 1: WHAT DOES MODE Y COST, PER SECOND? (s80) ─────────────────────────
     The mode-Y fix was parked on a performance judgement -- "arming the A0000 trap makes
     the interpreter the CPU" -- that nobody had measured, and the user's bar is "measure
     first, then decide". Every map-mask write ALREADY traps (it is an OUT to 3C5h) and
     already remaps the window here, so the question is not "trap per mask change or per
     write" in the abstract; it is these numbers, per second of play:
         sel    map-mask writes that reached us        (= SR2 write rate)
         swap   the ones that actually moved the window (Unmap + MapViewOfFileEx)
         fan    multi-plane windows closed               (the case one mapping cannot do)
         fanB   bytes those windows CHANGED -- a LOWER bound on the guest's stores under a
                multi-plane mask, since a same-value store is invisible (which is the
                whole defect); a trap-per-write design would pay per store
         us     host time spent inside this function     (the current design's own cost)
         flip   CRTC 0Ch (start address high) writes      (guest frames, for page-flippers)
         ins    instructions interpreted for mode Y        (design C, ModeYNeedsInterp)
         ius    host time spent interpreting them          (compare with `us`)
     Timed with RDTSC, not QPC: on XP QPC can be the ACPI PM timer at ~1 us a read, and
     this runs ~10^5 times a second in Doom's low detail; two QPCs a call would perturb
     the thing measured. Cycles are converted to us once, at report time, against QPC. */
#define YTL_SECS 90
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
#define YBAR_OFF_LO   (168u * MODEY_ROW_BYTES)      /* first bar byte within a page */
#define YBAR_OFF_MID  (184u * MODEY_ROW_BYTES)      /* band split: rows 184..199    */
#define YBAR_OFF_HI   (200u * MODEY_ROW_BYTES)
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
static VOID ModeYGr4CloseRun(VOID);       /* defined with the GR4 counters below */

static VOID ModeYRemapSelectBody(PVOID context, INT mask);
static VOID InterpreterMemoryBadNote(UINT32 linear, INT write);   /* defined after v86interp.h (needs icpu) */
/* The interpreter's per-instruction code pointer (v86interp.h, V86I_CODE_PTR): the 16
   bytes at `lin` sit in one already-probed page of plain RAM below 1 MB, outside the
   aperture -- exactly the bytes V86HostRead8's fast path would have read one at a time. */
#define V86I_CODE_PTR 1
static VOID HostPitGenerate(VOID);
#define WOW_ID_NONE 0xFFFF   /* g_WowLastId: no WOW32 call entered yet */
static VOID DpmiInstall(INT index);           /* defined just below; used by the helper */
static VOID WowShadowPut(INT index);         /* GH #128: keep the descriptor shadow in step */

static VOID DpmiBreakpointArm(VOID);               /* fwd: a new region may hold a requested BP */
static VOID DpmiBreakpointRearmPending(DWORD currentLinear);   /* fwd: re-plant stepped-over breakpoints */
static INT DpmiServicePmInt(DOS_MACHINE *machine, volatile BYTE *tib, DWORD vector, UINT steps);
static VOID DpmiEnsurePmReturnSelector(VOID);   /* fwd: shared PM-return catcher installer (#2b + 0303) */
#define WOW_SHADOW_ENTRIES DPMI_LDT_MAX
enum { NESTED_V86_ROUNDS_MAX = 128, PM_INT21_HANDLE_LIMIT = 24, PM_INT21_LOCK_REGION = 0xFF80 };   /* DpmiServicePmIntBody: 0301h's nested run, PM INT 21h */
/* Our BIOS/driver stub BOPs, serviced in one place for the exec loop AND the nested DPMI
   real-mode loop -- defined just above WinMain, where its arms used to live. (GH #247) */
#define V86BOP_NONE  0      /* not one of V86BiosBop's numbers                         */
static INT V86BiosBop(volatile BYTE *tib, UINT bopNumber, PSTR *logCursor, PSTR base);
#define INT15_WAIT_SLEEP_MS     3       /* INT 15h AH=86h: sleep only while more than this is left */
/* ── ★★★ THE NESTED RUN: CALL 16-BIT CODE AND WAIT FOR THE ANSWER. (s89, #162) ─────
     Every call into Win16 code so far was ARRANGED from a BOP and taken on the way out
     (wowcall.h): fine for anything the guest asked us to do, impossible for a question
     WINDOWS asks mid-way through its own work -- WM_CTLCOLOR comes from inside a
     control's paint and needs a brush before the paint can go on. This runs the call
     to completion right here: WowCallEnter parks the current context and enters the
     procedure exactly as a deferred callback would (unloaded segments included), and
     this loop drives the guest -- servicing every BOP, USER and GDI calls included,
     the way the PM IRQ injector does -- until the procedure's return stub pops that
     frame (WowCallLeave restores the parked context), then hands the result back.
   ⚠ Only on the guest thread, in protected mode, a Win16 session, below the callback
     depth limit. A run that stops without returning unwinds its frame and says so. */
static INT g_WowWindowNested = 0;
static INT WowCall16SyncEx(DWORD proc, WORD ds, const WORD *args, INT argumentCount,
                              WORD hwnd, WORD message, WORD *result,
                              BYTE *blob, INT blobLength, INT blobArgument,
                              const INT *fix, INT fixupCount);
#include <wownt32.h>   /* declarations only: WOW_TYPE_*, the handle types WOWHandle32/16 are given */
#include "../shim/shim_api.h"   /* defines only: the host/shim contract (SHIM_API_VERSION, SHIM_GLOBAL_*) */
typedef struct { PVOID InByte, InWord, InStringByte, InStringWord, OutByte, OutWord, OutStringByte, OutStringWord; } ISV_IO_HANDLERS;
#define ISV_MAX_HOOKS 16
static struct { HANDLE VddHandle; WORD FirstPort, LastPort; ISV_IO_HANDLERS Handlers; INT IsLive; } g_IsvHooks[ISV_MAX_HOOKS];

/* The interpreter templates' host callbacks, then the templates themselves. */
static inline __attribute__((always_inline)) BYTE V86HostRead8(UINT32 linear);
static inline __attribute__((always_inline)) VOID V86HostWrite8(UINT32 linear, BYTE value);
static inline __attribute__((always_inline)) const volatile BYTE *V86HostCodePointer(UINT32 linear);
static UINT32 V86HostIn(WORD port, INT width);
static VOID V86HostOut(WORD port, INT width, UINT32 byteValue);
static BYTE Pm32HostRead8(UINT32 linear);
static VOID Pm32HostWrite8(UINT32 linear, BYTE value);
static INT Pm32HostCanAccess(UINT32 linear, INT width, INT isWrite);
static UINT32 Pm32HostIn(WORD port, INT width);
static VOID Pm32HostOut(WORD port, INT width, UINT32 value);

#include "v86interp.h"
#include "pm32interp.h"

/* State used from a file other than its owner's (tentative definitions). */
static PFN_ADD_VECTORED_EXCEPTION_HANDLER g_PfnAddVeh;
static PFN_ATTACH_CONSOLE g_PfnAttachConsole;
static PFN_REGISTER_RAW_INPUT_DEVICES g_PfnRegisterRawInput;
static PFN_GET_RAW_INPUT_DATA g_PfnGetRawInput;
static DWORD g_OsVersion;
static DOS_SAFE_SKIPS g_Safe;
static WORD g_DsProbe[DSPROBE_MAX];
static INT g_DsProbeCount;
static WORD g_CsProbe[DSPROBE_MAX];
static INT g_CsProbeCount;
static INT g_Routed;
static INT g_BackToPrompt;
static VDM_COMMAND_INFO g_CommandInfo;
static DOS_MACHINE *g_Machine;
static VDD_BUS g_Bus;
static PIT_STATE g_Pit; static NTVDD_DEVICE g_PitDevice;
static CMOS_STATE g_Cmos; static NTVDD_DEVICE g_CmosDevice;
static PIC_STATE g_Pic; static NTVDD_DEVICE g_PicDevice;
static VIDEO_STATE g_Video; static NTVDD_DEVICE g_VideoDevice;
static INPUT_STATE g_Input; static NTVDD_DEVICE g_InputDevice;
static SPEAKER_STATE g_Speaker; static NTVDD_DEVICE g_SpeakerDevice;
static PCSPEAKER g_PcSpeaker;
static INT g_SpeakerReal;
static OPL_STATE g_Opl; static NTVDD_DEVICE g_OplDevice;
static SB_STATE g_Sb; static NTVDD_DEVICE g_SbDevice;
static GUS_STATE g_Gus; static NTVDD_DEVICE g_GusDevice;
static INT g_GusOn;
static EMU8K_STATE g_Emu8K; static NTVDD_DEVICE g_Emu8KDevice;
static INT g_AweOn;
static INT g_DosVersionForced;
static PCSTR g_DosVersionWhy;
static MPU_STATE g_Mpu; static NTVDD_DEVICE g_MpuDevice;
static COMM_STATE g_Comm; static NTVDD_DEVICE g_CommDevice;
static NETBIOS_STATE g_Net; static NTVDD_DEVICE g_NetDevice;
static BYTE g_GenericStubVector[DOS_GENSTUB_N];
static JOYSTICK_STATE g_Joystick; static NTVDD_DEVICE g_JoystickDevice;
static INT g_JoystickPovMap;
static INT g_WowFoldMute;
static DWORD g_WowFoldDropped;
static AUDIO_STATE g_Audio; static AUDIO_WAVE g_Wave;
static PRESENT_DDRAW g_PresentDdraw;
static DOS_XMS_STATE g_Xms;
static PVOID g_Hma;
static DWORD g_HmaError;
static DWORD g_HmaState, g_HmaProtection;
static DOS_EMS_STATE g_Ems;
static volatile LONG g_Irq0Pending;
static volatile LONG g_IcaPending;
static DWORD g_IcaRaised, g_IcaDelivered, g_IcaNoHandler;
static DWORD g_WowIdleWaits;
static DWORD g_ShimState[WOW_SHIMS], g_ShimError[WOW_SHIMS];
static UINT32 g_PitSyncs;
static UINT32 g_PitAsyncAttempts;
static DWORD g_PitDeliverSkipped;
static LONG g_PmTickOwedMaximum;
static UINT32 g_PmOwedHistogram[9];
static volatile LONG g_PmTickOwed;
static DWORD g_TickGap[12], g_TickGapMaximumMicroseconds, g_TickGapOver;
static volatile LONG g_Irq1Pending;
static INT g_PmIrq0Latch;
static INT g_InPmIrq;
static CRITICAL_SECTION g_Lock;
static LARGE_INTEGER g_QpcFrequency;
static INT g_LockSite, g_LockHoldSite, g_LockWaitSite;
static UINT32 g_LockHoldMicroseconds, g_LockWaitMicroseconds, g_UiGapMicroseconds;
static INT g_UiTickMinimumMs;
static DWORD g_UiTickSkips;
static DWORD g_UiHookPresents, g_UiTimerPresents;
static DWORD g_KeyMessageHistogram[8];
static DWORD g_KeyMessageMaximumMs, g_KeyMessageCount;
static DWORD g_KeyDeliveryHistogram[8];
static DWORD g_KeyDeliveryMaximumMs, g_KeyDeliveryCount;
static DWORD g_Irq1Checks, g_Irq1NoIf, g_Irq1In08, g_Irq1In09;
static DWORD g_Irq1AsyncInjected;
static INT g_KeyIrqRetry;
static DWORD g_Irq1AsyncRetry;
static CRITICAL_SECTION g_PitCs;
static HWND g_Window;
static HANDLE g_KeyEvent;
static volatile LONG g_Running;
static INT g_DpmiPm;
static INT g_PmClientExited;
static DWORD g_DpmiCodeBase;
static DWORD g_PatchMapLinear[DPMI_PMAP_SLOTS];
static BYTE g_PatchMapVector[DPMI_PMAP_SLOTS];
static DWORD g_PatchMapCount;
static INT g_PmNoIrq;
static INT g_PmVehPass;
static volatile LONG g_PmEntryEip;
static UINT g_DpmiCpMaximum;
static DWORD g_WowPmBase[WOW_PMBASE_MAX];
static DWORD g_PmWatchOffset;
static UINT g_PmWatchSegment;
static DWORD g_BreakpointDump[DPMI_BP_MAX];
static DWORD g_BreakpointSkip[DPMI_BP_MAX];
static DWORD g_BreakpointMode[DPMI_BP_MAX];
static BYTE g_BreakpointPending[DPMI_BP_MAX];
static DWORD g_BreakpointReport[DPMI_BP_MAX];
static BYTE g_BreakpointDone[DPMI_BP_MAX];
static INT g_BreakpointCount;
static volatile LONG g_DpmiIteration;
static volatile LONG g_DpmiDone;
static volatile LONG g_DpmiWatchdogGeneration;
static INT g_Headless;
static DWORD g_EventIo;
static DWORD g_IoExtra;
static DWORD g_Irq0Injected;
static DWORD g_Irq0TimeLast[IRQ0TL_SECS];
static DWORD g_Irq0GapHistogram[8];
static DWORD g_Irq0GapMaximumMs, g_Irq0GapCount;
static LONGLONG g_Irq0Start, g_Irq0TimePrevious;
static DWORD g_Irq0IoPrevious, g_Irq0WorstGapMs, g_Irq0WorstGapIo;
static DWORD g_Irq0WorstCs, g_Irq0WorstIp;
static DWORD g_Irq0NoteCs, g_Irq0NoteIp;
static DWORD g_Irq0RaiseCount, g_Irq0AttemptsCount, g_Irq0NieCount, g_Irq0YieldCount;
static DWORD g_Irq0AnomalyCount, g_Irq0AnomalyMicroseconds, g_Irq0AnomalyIo;
static DWORD g_Irq0AnomalyRaise, g_Irq0AnomalyAttempts, g_Irq0AnomalyNie, g_Irq0AnomalyYield;
static DWORD g_Irq0AnomalyGeneration, g_Irq0AnomalyDelete;
static DWORD g_Irq0NormalCount, g_Irq0NormalMicroseconds, g_Irq0NormalIo;
static DWORD g_Irq0WorstRaise, g_Irq0WorstAttempts, g_Irq0WorstNie, g_Irq0WorstYield;
static DWORD g_Irq0WorstPerMicroseconds;
static DWORD g_Irq0Skip;
static DWORD g_Irq0SkipIf;
static DWORD g_Irq0SkipStub;
static DWORD g_HeartbeatDs;
static DWORD g_EventIntPending;
static DWORD g_EventIoString;
static DWORD g_Irq1Injected;
static DWORD g_PmIrqReflects;
static DWORD g_IrqNInjected;
static DWORD g_IrqNRefuseTotal;
static volatile LONG g_WoundDown;
static WORD g_IoLastPort;
static DWORD g_EventHistogram[EV_HIST_MAX];
static DWORD g_V86MicrosecondsTotal, g_HostMicrosecondsEvent[EV_HIST_MAX];
static DWORD g_BopHistogram[BYTE_VALUES];
static DWORD g_PitPaceCalls;
static DWORD g_XsStart, g_XsSeconds, g_XsSnapshot[XS_SECS][XS_N];
static INT g_IoHotCount;
static WORD g_Unclaimed[IO_UNCLAIMED_MAX];
static INT g_UnclaimedCount;
static UINT g_CaptureMs;
static DWORD g_CaptureDelayMs, g_CaptureStart;
static INT g_Capture;
static INT g_NoA000;
static INT g_NoPmPatch;
static DWORD g_NoPmPatchMinimum;
static DWORD g_MemoryDumpLinear, g_MemoryDumpLength;
static INT g_P12Offset;
static DWORD g_OplTraceCount;
static DWORD g_OplTraceDrop;
static INT g_OplTraceOn;
static INT g_ModeYInterpOffset;
static INT g_ModeYInterp;
static DWORD g_ModeYSlices, g_ModeYInstructions, g_ModeYBails, g_ModeYBailMp;
static INT g_ModeYRingOn;
static INT g_ModeYPmOffset;
static INT g_ModeYPmDetect;
static DWORD g_HeadlessMs;
static volatile DWORD g_DpmiEnterCs;
static volatile DWORD g_DpmiEnterEip;
static volatile DWORD g_DpmiLastEvent;
static volatile DWORD g_DpmiLastCs;
static volatile DWORD g_DpmiLastEip;
static volatile DWORD g_DpmiLastVector;
static INT g_DpmiBlockCount;
static DWORD g_DpmiOwned[DPMI_OWNED_MAX];
static INT g_DpmiOwnedCount;
static WORD g_DpmiDosBlock[DPMI_DOSBLK_MAX];
static INT g_DpmiDosBlockCount;
static INT g_LdtClientMark;
static INT g_PmExitCode;
static DWORD g_LeCodeSize[DPMI_LE_MAX];
static INT g_LeCodeCount;
static DWORD g_PmVector8ArmedMs;
static INT g_PmAppHookedTimer;
static WORD g_PmAppTimerSelector;
static DWORD g_PmAppTimerOffset;
static WORD g_PmDefaultSelector;
static INT g_DpmiVi;
static BYTE g_BiosUnimplemented[BYTE_VALUES];
static INT g_ExecDepth;
static volatile LONG g_CloseRequest;
static INT g_TopIsShell;
static CHAR g_ProgramName[PROGRAM_NAME_SIZE];
static WORD g_PmReturnSelector;
static WORD g_DpmiFaultSelector;
static WORD g_DpmiFaultCodeSelector;
static INT g_LdtNext;
static WORD g_LdtFree[DPMI_LDT_MAX];
static INT g_LdtFreeCount;
static volatile BYTE *g_TibDebug;
static HANDLE g_ComSpool[COMM_MAX_PORTS];
static INT g_ComFailed[COMM_MAX_PORTS];
static INT g_BdaReady;
static WORD g_Int15StubOffset;
static DWORD g_PrintScreenJobs, g_PrintScreenErrors;
static BYTE g_PrintScreenStatus;
static volatile LONG g_IrqNPending[PIC_LINES];
static const BYTE g_IrqOrder[14];
static DWORD g_IrqNRetryTry, g_IrqNRetryOk, g_IrqNRetryWhy;
static DWORD g_IrqRaised[PIC_LINES];
static DWORD g_IrqRaisedAny;
static HANDLE g_HostCpu;
static DWORD g_QiBits;
static INT g_QiSuspended;
static INT g_QiKeysAsync;
static volatile LONG g_InExec;
static volatile LONG g_AsyncContextWrite;
static volatile LONG g_PauseWant;
static INT g_BehaveDos622;
static DWORD g_PauseCount, g_PauseCooperative, g_PauseMs;
static HANDLE g_CourierEvent;
static DWORD g_AsyncInjected;
static DWORD g_AsyncBail;
static DWORD g_QiCalls;
static volatile LONG g_QiStatus;
static DWORD g_AsyncNestBlocked;
static DWORD g_Irq0IsrSince;
static DWORD g_Irq0IsrBlocks;
static DWORD g_Irq0IsrTimeouts;
static DWORD g_Irq0IsrStrict;
static DWORD g_Irq0IsrAuto;
static INT g_Irq0AutoEoi;
static DWORD g_Irq0ResyncDrop;
static LONG g_AsyncWhy;
static DWORD g_AsyncWhyHistogram[PIC_LINES_PER_CHIP][ASYNC_WHY_MAX];
static DWORD g_IfvCensus[IFV_PATHS][8];
static DWORD g_IfvShadow[PIC_LINES];
static DWORD g_IfvStarveT0, g_IfvStarveMaximumMs, g_IfvStarveCount;
static INT g_IfvStarveOpen;
static LONG g_IfvTraceCount;
static DWORD g_IfvReenter[PIC_LINES];
static volatile LONG g_AsyncPmActive;
static DWORD g_AsyncPmEip, g_AsyncPmEsp, g_AsyncPmEflags;
static WORD g_AsyncPmCs, g_AsyncPmSs;
static DWORD g_AsyncPmInjected;
static DWORD g_AsyncInjectedLine[PIC_LINES];
static DWORD g_PmWatch[DPMI_WATCH_MAX];
static INT g_PmWatchCount;
static BYTE g_PmWatchRel[DPMI_WATCH_MAX];
static DWORD g_LeLoadBase;
static DWORD g_PmCooperativeLine[PIC_LINES_PER_CHIP];
static DWORD g_PmInjectDecl[2], g_PmInjectDeclTl[IRQ0TL_SECS];
static UINT32 g_DmaPollInAsync, g_DmaPollMainline;
static DWORD g_AsyncEarlyBailLogged;
static INT g_AsyncSiteCount;
static INT g_AsyncSiteFull;
static volatile LONG g_SimIntBusy;
static volatile LONG g_NestedRm;
static PCSTR g_FloppyImage;
static HANDLE g_Stdio;
static PCSTR g_StdioHow;
static PCSTR g_StdioSource;
static DWORD g_StdioParentProcessId;
static DWORD g_DmxSamples, g_DmxBusy[12], g_DmxMixerOk;
static DWORD g_DmxOverdue, g_DmxOverdueMaximum, g_DmxAnyBusy;
static INT g_PitPaceOn, g_PitPaceMs;
static INT g_PitPacePriority;
static INT g_PitPaceInject;
static volatile LONGLONG g_Int15WaitEnd;
static volatile LONGLONG g_Int15EventEnd;
static volatile DWORD g_Int15EventLinear;
static DWORD g_Int15Waits, g_Int15Events, g_Int15Posted, g_Int15Busy;
static INT g_PmTopDispatch;
static INT g_PmDispatchTop;
static INT g_CourierOn;
static DWORD g_CourierWakes, g_CourierInjected, g_CourierTries, g_CourierGiveUp;
static INT g_CpuSpeedIndex;
static UINT g_CpuSpeedReferenceMhz;
static volatile LONG g_CpuSpeedDuty;
static volatile LONG g_CpuSpeedDutyRm;
static DWORD g_CpuSpeedRunMs, g_CpuSpeedHeldMs;
static DWORD g_CpuSpeedMissed;
static DWORD g_CpuSpeedHoldMaximumMicroseconds;
static DWORD g_CpuSpeedDebtMaximumMicroseconds;
static DWORD g_CpuSpeedRanMicroseconds;
static DWORD g_CpuSpeedWallMicroseconds;
static UINT g_CpuSpeedGranularityMs;
static DWORD g_CpuSpeedRoundTripMicroseconds;
static DWORD g_CpuSpeedPeriodMs;
static DWORD g_CpuSpeedPeriods;
static DWORD g_StartMs;
static HANDLE g_CpuSpeedRelease;
static DWORD g_CpuSpeedCooperativeCatches, g_CpuSpeedCooperativeTimeouts;
static INT g_CpuAffinityOn;
static DWORD g_CpuAffinityGuest, g_CpuAffinityRest, g_CpuAffinityCpuCount;
static MPU_STATE g_GusMidi;
static UINT32 g_TypematicDelayMicroseconds;
static UINT32 g_TypematicPeriodMicroseconds;
static DWORD g_TypematicSpiDelay, g_TypematicSpiSpeed;
static UINT32 g_TypematicSent, g_TypematicOsRepeats;
static volatile LONG g_MouseX, g_MouseY, g_MouseButtons;
static volatile LONG g_MouseDx, g_MouseDy;
static INT g_MouseRawOk;
static INT g_MouseSensitivity;
static DWORD g_MouseWmInput, g_MouseRawAbsolute, g_MouseI33[16], g_MouseI33Other;
static DWORD g_MouseI33AxOverflow, g_MouseI33SiteCount, g_MouseI33SiteOverflow;
static DWORD g_SimIntUnhandled, g_SimIntVector[IVT_VECTORS];
static INT g_SimIntReflect;
static INT g_MouseAbsent;
static INT g_TextDump;
static volatile LONG g_MouseHidden;
static volatile LONG g_MousePressCount[MS_BTNS], g_MouseReleaseCount[MS_BTNS];
static DWORD g_MouseEdges;
static volatile LONG g_MouseEventMask, g_MouseEventSegment, g_MouseEventOffset;
static DWORD g_MouseEventInstalls;
static volatile LONG g_MouseEventPend;
static INT g_MouseCallbackActive;
static DWORD g_MouseCallbackInjected, g_MouseCallbackDone, g_MouseCallbackLost, g_MouseCallbackPm, g_MouseCallbackStray;
static INT g_MouseCallbackTrace;
static DWORD g_MouseCallbackWhy[MOUSE_CB_WHY_COUNT];
static DWORD g_MouseEventRaised;
static INT g_HostCursorMode;
static volatile LONG g_Captured;
static volatile LONG g_MouseWantCapture;
static volatile LONG g_MouseAutoCaptureDone;
static volatile LONG g_MouseWantRelease;
static volatile LONG g_MouseSeamless;
static DWORD g_MouseShapeSets;
static volatile LONG g_MouseGraphicsCursorDefined;
static DWORD g_MouseGraphicsCursorBadPointer;
static DWORD g_MouseAccelerationCalls;
static DWORD g_MouseAltCalls;
static volatile LONG g_MouseTextCursorAnd, g_MouseTextCursorXor;
static DWORD g_MouseStateBadPointer;
static DWORD g_MouseI33Unimplemented;
static NE_MODULE g_WowModule[WOW_MAX_MOD];
static BYTE *g_WowImage[WOW_MAX_MOD];
static CHAR g_WowName[WOW_MAX_MOD][16];
static INT g_WowModuleCount;
static WORD g_WowEntryCx;
static WORD g_WowPspSegment;
static WORD g_PmTransferSegment;
static WORD g_WowPathSegment;
static WORD g_WowEnvironmentSegment;
static DWORD g_WowCallbackLinear;
static WORD g_WowPspSelector[WOW_PSP_TRACK];
static DWORD g_WowPspLinear[WOW_PSP_TRACK];
static WORD g_WowPspEnvironment[WOW_PSP_TRACK];
static INT g_WowPspCount;
static WORD g_PmTransferParagraphs;
static INT g_WowLaunch;
static HHOOK g_LowLevelKeyboard;
static INT g_LowLevelKeyboardOn;
static HANDLE g_ExecThread;
static INT g_ExecPriorityForeground;
static LONG g_JoystickThreadStarted;
static NTVDMEX_SETTINGS g_Settings;
static NTVDMEX_SETTINGS g_SettingsDisk;
static PCSTR g_ShellOverride;
static DOS_MACHINE *g_DosMachine;
static INT g_FrameSkip;
static DOS_SB_CONFIG g_SbConfig;
static INT g_DspVersionForced;
static INT g_XmsOn, g_EmsOn;
static UINT g_ConventionalKbWant;
static WORD g_DosMemoryTop;
static DWORD g_WindowSizeLive;
static DWORD g_AspectLive;
static const INT g_SettingsPages[NTVDMEX_PAGE_COUNT];
static UINT32 g_PitCatchupClamped;
static UINT32 g_PitGapMaximum;
static DWORD g_VbeWaits;
static volatile DWORD g_RetracePending, g_RetraceCs, g_RetraceIp, g_RetraceAl, g_RetraceCx, g_RetraceIdles;
static DWORD g_DmaPollEip[DMAPOLL_MAX], g_DmaPollHits[DMAPOLL_MAX];
static UINT g_DmaPollCount, g_DmaPollOverflow;
static DWORD g_PollStack[POLLSTK_MAX], g_PollStackHits[POLLSTK_MAX];
static DWORD g_PollGap[10], g_PollGapMaximumMicroseconds;
static UINT g_PollStackCount, g_PollStackOverflow;
static PVOID g_ModeYView[MODEY_NSEC];
static INT g_ModeYRemap;
static DWORD g_ModeYSwaps, g_ModeYFanouts, g_ModeYFail;
static DWORD g_ModeYSelectorCalls;
static DWORD g_ModeYSelectorSame;
static DWORD g_ModeYSelectorZero;
static DWORD g_ModeYTimelineT0;
static DWORD g_ModeYTimelineIns[YTL_SECS];
static UINT64 g_ModeYTimelineInterpreterCycles[YTL_SECS];
static DWORD g_ModeYFanoutBarWrites[2];
static DWORD g_ModeYFanoutBarDistinct[2];
static DWORD g_ModeYFanoutBar4Way[2];
static DWORD g_ModeYSampleCrossSame[2], g_ModeYSampleCrossDiff[2], g_ModeYSampleWrites[2];
static DWORD g_ModeYSampleCrossEqualBytes[2], g_ModeYSampleCrossTotalBytes[2];
static DWORD g_ModeYSampleP1Equal[2][4], g_ModeYSampleP1Total[2][4];
static DWORD g_ModeYSampleDeliveredEqual[2], g_ModeYSampleDeliveredTotal[2];
static DWORD g_ModeYLatchOk, g_ModeYLatchUnsolved, g_ModeYLatchDescriptor;
static DWORD g_ModeYGr4Calls, g_ModeYGr4Mismatch, g_ModeYGr4Pair[4][6];
static DWORD g_ModeYGr4SinceSelector, g_ModeYGr4Runs[10], g_ModeYGr4RunPlanes[VIDEO_PLANES];
static DWORD g_ModeYGr4Moves;
static INT g_A000Protection;
static INT g_P12Interp;
static DWORD g_InterpreterMemoryBadReads, g_InterpreterMemoryBadWrites, g_InterpreterMemoryBadLogged;
static const V86_CPU *g_InterpreterCpu;
static DWORD g_GuestThreadId;
static WORD g_WowLastId;
static WORD g_WowLastFrom;
static INT g_HostPoolSpill;
static WORD g_DpmiHandlerSelector;
static INT g_WowSchedOn;
static WORD g_WowDgroupSelector;
static WOWSCHED_SLOT g_WowSchedSlots[WOWSCHED_MAX];
static INT g_WowWindowNested;
static DWORD g_WowSchedSwitches;
static WORD g_WowSchedLaunchChild, g_WowSchedShell;
static INT g_WowCallOn;
static BYTE g_PmDispatch[IVT_VECTORS];
static DWORD g_PmDispatchCount[IVT_VECTORS][BYTE_VALUES];
static DWORD g_Wow32Serviced, g_Wow32Unimplemented, g_Wow32Declined;
static BYTE *g_WowShadow;
static DWORD g_WowSyncWrites;
static DWORD g_PmIrqRmReflects, g_PmIrqRmFail;

/* Functions called from a file other than their own. */
static VOID OsCompatBind(VOID);
static BOOL OsCompatAttachConsole(DWORD processId);
static VOID DsProbeLoad(VOID);
static PCSTR NtvdmexRoot(VOID);
static PCSTR NtvdmexPath(PCSTR subdirectory, PCSTR name);
static UINT LauncherCompilerVariables(PCSTR environment, DWORD environmentCapacity, PSTR out, DWORD outCapacity);
static VOID GusReport(VOID);
static INT StrStrNoCase(PCSTR block, PCSTR name);
static BYTE NetSubmit(PVOID context, NETBIOS_REQUEST *request);
static VOID HmaTry(VOID);
static VOID Irq0Latch(VOID);
static INT PmTickTake(VOID);
static VOID TickDeliveredNote(VOID);
static VOID KeyLatencyPop(VOID);
static UINT32 QpcMicroseconds(LONGLONG ticks);
static UINT64 QpcMicroseconds64(LONGLONG ticks);
static VOID HostLockEnter(INT site);
static VOID HostLockLeave(VOID);
static INT HostLockTry(INT site);
static BYTE PatchMapGet(DWORD linear);
static VOID PatchMapSet(DWORD linear, BYTE vector);
static VOID PatchMapClear(DWORD linear);
static VOID Irq0DeliveredNote(VOID);
static VOID SkipIfSiteNote(DWORD codeSegment, DWORD instructionPointer, DWORD stub);
static PSTR ExecBegin(DOS_MACHINE *machine, volatile BYTE *tib, PSTR cursor);
static VOID CriticalSnapshot(volatile BYTE *tib);
static VOID CriticalRaise(DOS_MACHINE *machine, volatile BYTE *tib, PSTR *logCursor);
static INT CriticalReturn(DOS_MACHINE *machine, volatile BYTE *tib, PSTR *logCursor);
static INT PmRwHardwareFail(DOS_MACHINE *machine, volatile BYTE *tib, BYTE function, DWORD win32Error, PSTR *logCursor);
static VOID ComTransmitSink(PVOID context, INT port, BYTE byteValue);
static WORD BiosEquipmentWord(VOID);
static VOID BiosBdaRefreshEquipment(VOID);
static VOID SerialInitialize(VOID);
static VOID SerialOut(PCSTR buffer, PCSTR end);
static VOID VddLoadThirdParty(VOID);
static VOID WowLogFlush(PSTR base, PSTR *logCursor);
static INT LptSpoolPut(BYTE character);
static VOID LptTransmitSink(PVOID context, INT port, BYTE byteValue);
static INT DosPrnOut(PVOID context, BYTE character);
static INT KeyboardActionEntry(INT keyboardAction);
static INT Int15Hooked(VOID);
static VOID DosAuxOut(PVOID context, BYTE character);
static INT MemoryReadable(ULONG_PTR address, SIZE_T length);
static UINT IrqPmVector(UINT irq);
static VOID HostPitResyncCheck(VOID);
static INT Irq0CanDeliver(VOID);
static VOID Irq0Ack(VOID);
static INT Irq0PmClaim(VOID);
static VOID Irq0PmUnclaim(VOID);
static VOID IfvNote(INT path, DWORD flags);
static DWORD PmWatchAddress(INT index);
static VOID PmInjectDeclineNote(INT why, WORD cs, DWORD eip);
static INT AsyncInjectIrq(UINT irq);
static INT AsyncVectorIsOurStub(UINT irq);
static VOID HostIrqSink(PVOID context, BYTE irq);
static VOID PokeWord(DWORD linear, WORD value);
static VOID PokeDword(DWORD linear, DWORD value);
static WORD PeekWord(DWORD linear);
static INT IfOrVif(DWORD flags);
static INT IsOurStubCsIp(DWORD cs, DWORD ip);
static DWORD PeekWidth(DWORD linear, INT width);
static VOID PokeWidth(DWORD linear, DWORD value, INT width);
static VOID VdmStateSample(PCSTR label, volatile BYTE *tib, INT *budget);
static VOID InjectInt(volatile BYTE *tib, UINT vector);
static INT DosTerminate(DOS_MACHINE *machine, PVOID tib, PSTR *logCursor, PSTR base);
static UINT RecoveryRead(VOID);
static VOID RecoveryWrite(UINT value);
static VOID RecoveryOk(VOID);
static INT MruLoad(CHAR out[MRU_MAX][MAX_PATH]);
static VOID MruAdd(PCSTR path);
static INT InstallPerform(INT want, INT force, PSTR message, DWORD cap);
static INSTALL_STATE InstallStatusText(PSTR message, DWORD cap);
static INT InstallVerb(PCSTR command);
static INT CommandLineHasForce(PCSTR command);
static INT CommandLineBare(PCSTR command);
static INT LaunchShellVdm(VOID);
static VOID InstallReport(PCSTR message, INT isOk);
static VOID RecoveryUninstall(PSTR *logCursor);
static INT HostHasFloppy(VOID);
static INT HostHasCdrom(VOID);
static PDOS_DISK_GEOMETRY DiskFor(UINT drive);
static INT DiskIo(UINT drive, UINT32 lba, UINT count, BYTE *guest, INT write);
static VOID StdioFlush(VOID);
static HANDLE StdioPebHandle(HANDLE proc, UINT offset);
static PCSTR StdioInitialize(VOID);
static PCSTR StdioInitializeVdm(VOID);
static VOID HostConsoleOut(PVOID context, BYTE ch);
static INT HostConsoleIn(PVOID context);
static UINT64 HostTimeMicroseconds(VOID);
static VOID OplTraceWrite(BYTE registerIndex, BYTE value);
static VOID OplTraceDump(VOID);
static VOID OplPumpTime(VOID);
static LONGLONG Int15QpcAfterMicroseconds(DWORD microseconds);
static VOID PitPacerTimerStart(HMODULE winmmModule);
static DWORD WINAPI PitPacerThread(LPVOID param);
static DWORD WINAPI TickCourierThread(LPVOID parameter);
static UINT HostCpuMhz(VOID);
static VOID ExecEnterMark(VOID);
static VOID ExecLeaveMark(VOID);
static VOID CpuSpeedCooperativePark(VOID);
static VOID CpuSpeedTimelineDump(PCSTR tag);
static VOID CpuSpeedRecompute(VOID);
static DWORD WINAPI CpuSpeedThread(LPVOID param);
static VOID HostAudioFill(PVOID context, INT16 *out, UINT32 frames);
static VOID HostMidiSink(PVOID context, UINT32 message);
static VOID HostMidiSysEx(PVOID context, const BYTE *message, UINT32 length);
static VOID GusMidiToSynth(PVOID context, BYTE byteValue);
static VOID PlanesDumpBeside(PCSTR bitmapPath);
static VOID HostKeyScancode(BYTE rawScancode, INT extended, INT isBreak);
static VOID HostKeyTypematicInitialize(VOID);
static LONGLONG QpcTicks(UINT32 microseconds);
static VOID HostKeyPresent(VOID);
static VOID HostKeyTypematic(VOID);
static DWORD WINAPI SynthKeyThread(LPVOID parameter);
static DWORD WINAPI QueueIrqProbeThread(LPVOID parameter);
static DWORD WINAPI HeartbeatThread(LPVOID parameter);
static VOID HostRecordFinish(VOID);
static VOID AsyncWhyReport(VOID);
static VOID ExecShareReport(VOID);
static VOID IfvReport(VOID);
static DWORD WINAPI HeadlessDeadlineThread(LPVOID parameter);
static INT TypeInPush(PCSTR text);
static INT HostConsoleInNoBlock(PVOID context);
static INT HostConsolePeek(PVOID context);
static VOID HostSetFlags(volatile BYTE *tib, BYTE carryFlag, BYTE zeroFlag);
static PVOID XmsHostAllocate(PVOID context, DWORD kilobytes);
static VOID XmsHostFree(PVOID context, PVOID memory, DWORD kilobytes);
static VOID HostXms(volatile BYTE *tib);
static PVOID EmsHostAllocate(PVOID context, DWORD pages);
static VOID EmsHostFree(PVOID context, PVOID memory, DWORD pages);
static VOID HostEms(volatile BYTE *tib);
static UINT I33Width(VOID);
static UINT I33Height(VOID);
static INT I33XShift(VOID);
static LONG I33VirtualX(LONG pixelX);
static INT I33Text(VOID);
static LONG I33VirtualY(LONG pixelY);
static LONG I33VirtualMaximumY(VOID);
static VOID MouseEventRaise(LONG bits);
static VOID MouseButtonEdges(LONG prev, LONG now);
static VOID HostMouseButton(INT button, INT down);
static VOID MouseChildExited(VOID);
static INT CaptureAllowed(VOID);
static INT MouseGoesToGuest(VOID);
static LONG I33ClampX(LONG virtualX);
static LONG I33ClampY(LONG virtualY);
static VOID I33ResetState(VOID);
static UINT Int15MoveBlockAt(volatile BYTE *tib, DWORD gdtLinear);
static VOID ExecMachineSave(INT depth);
static VOID ExecMachineRestore(INT depth, PSTR *logCursor);
static INT CloseProgramNow(DOS_MACHINE *machine, PVOID tib, PSTR *logCursor, PSTR base);
static VOID MouseInt33(volatile BYTE *tib, INT source);
static INT MouseAnyHandler(VOID);
static INT MouseEventQueueTake(MOUSE_EVENT_ENTRY *event, LONG *outAx, WORD *segment, DWORD *offset);
static VOID MouseCallbackTry(volatile BYTE *tib);
static VOID MouseCallbackReturn(volatile BYTE *tib);
static VOID MouseDrawGraphicsCursor(BYTE *pixels, INT width, INT height, INT stride);
static PCSTR CommandLineAfterArgv0(PCSTR cursor);
static INT LaunchIsWow(PCSTR command);
static INT WowModuleOfSelector(WORD selector);
static INT WowUserAnchor(WORD thunkId, WORD argumentBytes, WORD returnStub);
static INT WowShellAnchor(WORD thunkId, WORD argumentBytes, WORD returnStub);
static INT WowCommonDialogAnchor(WORD thunkId, WORD argumentBytes, WORD returnStub);
static INT WowKeyboardAnchor(WORD thunkId, WORD argumentBytes, WORD returnStub);
static INT WowSoundAnchor(WORD thunkId, WORD argumentBytes, WORD returnStub);
static INT WowGdiAnchor(WORD thunkId, WORD argumentBytes, WORD returnStub);
static INT WowKernel2Stub(WORD thunkId, WORD returnStub);
static WORD WowHostAllocate(WORD paras);
static PSTR WowPspEnvironmentCheck(PSTR cursor, PCSTR where);
static VOID WowProbeLoad(PCSTR command);
static INT WowRefuse(PCSTR command);
static VOID HostPresentHook(PVOID context);
static VOID TrayRemove(HWND window);
static INT StringsEqual(PCSTR first, PCSTR second);
static LRESULT CALLBACK LowLevelKeyboardProcedure(INT code, WPARAM wParam, LPARAM lParam);
static VOID InputCaptureSet(HWND window, INT isOn);
static INT OtherHostsRunning(VOID);
static VOID BackgroundPriorityTick(HWND window);
static VOID HostPanicRelease(VOID);
static DWORD WINAPI CaptureWatchdogThread(LPVOID parameter);
static VOID HostFullscreenToggle(HWND window);
static UINT64 JoystickNowMicroseconds(PVOID context);
static VOID JoystickPollEnsure(VOID);
static VOID SettingsNoteOverride(INT settingId, PCSTR source, DWORD value);
static VOID SettingsLogSources(VOID);
static VOID SettingsApply(HWND window, const NTVDMEX_SETTINGS *settings, INT live);
static VOID SettingsApplyPresent(PRESENT_DDRAW *present, const NTVDMEX_SETTINGS *settings);
static VOID SettingsApplyDevices(const NTVDMEX_SETTINGS *settings);
static UINT32 SettingsOutputHz(const NTVDMEX_SETTINGS *settings);
static VOID MenuViewSync(HWND window);
static VOID HostApplyWindowSize(HWND window, DWORD index);
static VOID SettingsApplyLive(HWND window);
static INT_PTR CALLBACK SettingsPageProcedure(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam);
static INT_PTR CALLBACK SettingsDialogProcedure(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam);
static VOID KeyMessageNote(VOID);
static VOID KeyPushMake(LPARAM lParam);
static VOID KeyPushBreak(LPARAM lParam);
static VOID ModifierTrack(BYTE rawScancode, INT extended, INT down);
static VOID HostReleaseModifiers(VOID);
static DWORD WINAPI UiThread(LPVOID argument);
static VOID RegistersLoad(NTVDD_REGISTERS *registers, volatile BYTE *tib);
static VOID RegistersStore(NTVDD_REGISTERS *registers, volatile BYTE *tib);
static VOID IoHotNote(WORD port, DWORD cs, DWORD ip);
static VOID HostRtcNow(PVOID context, PIT_RTC_READING *out);
static INT HostRtcSet(PVOID context, const PIT_RTC_READING *reading, INT what);
static INT HostTickTake(UINT32 *ticks, UINT32 *wraps, UINT32 *since);
static VOID HostTicksSet(PVOID context, UINT32 ticks);
static VOID HostSetTicks(PVOID context, UINT32 ticks);
static VOID HostPitGuard(PVOID context, INT enter);
static VOID HostPitGenerate(VOID);
static VOID HostPitDeliver(VOID);
static VOID HostPitSync(VOID);
static VOID PitLatchNote(BYTE command);
static VOID Int10WaitAfter(VOID);
static VOID RetraceNote(volatile BYTE *tib, WORD port, INT isIn, DWORD cs, DWORD ipAfter);
static VOID RetraceIdle(VOID);
static INT HostTryIo(volatile BYTE *tib, VDD_BUS *bus);
static INT HostTryIoRetro(volatile BYTE *tib, VDD_BUS *bus);
static INT HostTryIoString(volatile BYTE *tib, VDD_BUS *bus);
static INT DpmiSelectorIs32(WORD selector);
static INT HostTryIoPm(volatile BYTE *tib, VDD_BUS *bus);
static UINT64 ModeYTimelineRdtsc(VOID);
static VOID ModeYRemapFlushReport(VOID);
static INT ModeYRemapInitialize(VOID);
static VOID ModeYRemapSelect(PVOID context, INT mask);
static VOID ModeYBailNote(DWORD cs, DWORD ip, const volatile BYTE *bytes);
static VOID ModeYTimelineReport(VOID);
static VOID ModeYRemapSelectBody(PVOID context, INT mask);
static BYTE *ModeYRemapPlane(PVOID context, INT plane);
static VOID ModeYGr4CloseRun(VOID);
static VOID ModeYRemapReadMap(PVOID context, INT plane);
static VOID ModeYRemapWriteMode(PVOID context, INT writeMode);
static VOID VideoTrapSync(VOID);
static INT ModeYInterpServes(VOID);
static INT ModeYNeedsInterp(VOID);
static INT InterpreterMemoryPageOk(UINT32 linear);
static INT ModeYPmNeedsInterp(VOID);
static VOID ModeYPmRun(volatile BYTE *tib);
static VOID InterpreterMemoryBadNote(UINT32 linear, INT write);
static UINT32 HostGuestPc(VOID);
static VOID ModeYRingDump(PCSTR why);
static VOID ModeYRingNoteIrq(UINT vector, WORD cs, WORD ip, WORD ss, WORD sp);
static VOID HostProfileStart(VOID);
static VOID HostProfileDump(VOID);
static INT32 HostInterpPaced(volatile BYTE *tib, INT32 cap);
static INT HostReadable(PCVOID pointer, SIZE_T length);
static INT HostWritable(PVOID pointer, SIZE_T length);
static LONG CALLBACK DpmiCrashVeh(EXCEPTION_POINTERS *pointers);
static LONG WINAPI HostUnhandledFilter(EXCEPTION_POINTERS *pointers);
static DWORD WINAPI DpmiWatchdog(LPVOID param);
static INT DpmiHostIndex(VOID);
static WORD DpmiHandlerCodeSelector(VOID);
static WORD DpmiSegmentToDescriptor(WORD segment);
static WORD WowCallbackSelector(VOID);
static VOID DpmiSegmentToDescriptorForget(WORD selector);
static VOID WowProbeLdtMatrix(PCSTR tag);
static INT WowPlaceV86(DOS_MACHINE *machine, WORD *entryCs, WORD *eip, WORD *entryDs, WORD *entrySs, WORD *esp);
static VOID WowProbeSelectors(VOID);
static VOID DpmiInstallDefaultPmHandlers(DOS_MACHINE *machine);
static VOID DpmiInstall(INT index);
static VOID DpmiInstallFaultTrampoline(VOID);
static VOID DpmiArmFaultTrampoline(volatile BYTE *tib, WORD flag);
static DWORD DpmiSelectorBase(WORD selector);
static DWORD DpmiRecoverFlatEip(DWORD lo16, BYTE vector, INT *candidateCount);
static INT DpmiSelectorDescriptor(WORD selector, UINT32 *accessRights, UINT32 *limit);
static DWORD DpmiBopVector(DWORD csValue, DWORD eip);
static DWORD DpmiPmEip(volatile BYTE *tib);
static VOID DpmiPatchCodeRegion(DWORD base, DWORD limit, INT is32BitRegion);
static VOID DpmiLeLearn(const BYTE *buffer, DWORD length);
static VOID DpmiScanCodeBlocks(VOID);
static VOID DpmiBreakpointLoad(VOID);
static VOID DpmiBreakpointResolveCodeBase(DWORD base);
static VOID DpmiBreakpointResolveSegment(UINT segmentNumber, DWORD base);
static VOID DpmiBreakpointArm(VOID);
static VOID DpmiBreakpointRearmPending(DWORD currentLinear);
static INT DpmiBreakpointDisarm(DWORD linear);
static VOID DpmiUnpatch(VOID);
static VOID DpmiRepatch(VOID);
static DWORD Wow32ReturnOverride(WORD thunkId);
static INT Wow32ModeOverride(WORD thunkId);
static VOID Wow32ModeLoad(VOID);
static VOID Wow32ReturnLoad(VOID);
static INT WowSchedFree(VOID);
static INT WowSchedPick(WORD current);
static INT WowSchedRunnable(WORD current, INT depth);
static INT WowSchedTopLevel(const WOWSCHED_SLOT *slot);
static WORD WowSchedCurrentTask(VOID);
static VOID WowTaskDirectoryHere(WORD task);
static VOID WowTaskChdir(WORD task, PSTR *logCursor);
static VOID WowSchedSetCurrent(WORD task);
static INT WowSchedRetarget(WORD hwnd, WORD *stackSegment, WORD *stackPointer, DWORD *stackSegmentBase, WORD *prev);
static VOID WowSchedUntarget(WORD prev);
static INT WowSchedInterTaskLive(VOID);
static VOID WowQuietLoad(VOID);
static VOID WowSchedLoad(VOID);
static VOID WowCallLoad(VOID);
static VOID DpmiInvokeCallback(DOS_MACHINE *machine, volatile BYTE *tib, INT slot);
static INT DpmiDispatchToPmHandler(DOS_MACHINE *machine, volatile BYTE *tib, DWORD vector, UINT steps);
static DWORD DpmiCallerOffset(volatile BYTE *tib, DWORD offset);
static DWORD DpmiRmcsPointer(volatile BYTE *tib, DWORD esBase);
static VOID RmcsToTib(volatile BYTE *tib, const RMCS_REGS *registers);
static VOID TibToRmcs(volatile BYTE *tib, RMCS_REGS *registers, WORD flags);
static VOID DpmiRmcsProbe(volatile BYTE *tib, DWORD esBase, UINT slot, DWORD interruptNumber);
static DWORD Wow32HostSelectorToLinear(WORD selector, PVOID context);
static INT DpmiOwnedFind(DWORD handle);
static VOID DpmiLdtRelease(INT index);
static INT DpmiLdtTake(VOID);
static INT DpmiClientSelectorOk(WORD selector);
static VOID WowShadowPut(INT index);
static INT WowShadowSync(PSTR *logCursor);
static INT WowVendorApiEntry(DOS_MACHINE *machine, WORD *selector, WORD *offset);
static PSTR PmInt21Transfer(DOS_MACHINE *machine, volatile BYTE *tib, DWORD ah, PSTR cursor);
static PSTR PmInt21Lfn(DOS_MACHINE *machine, volatile BYTE *tib, PSTR cursor);
static INT DpmiReflectIrqToRm(DOS_MACHINE *machine, volatile BYTE *tib, UINT vector);
static INT DpmiServicePmIntBody(DOS_MACHINE *machine, volatile BYTE *tib, DWORD vector, UINT steps);
static INT DpmiServicePmInt(DOS_MACHINE *machine, volatile BYTE *tib, DWORD vector, UINT steps);
static VOID DpmiEnsurePmReturnSelector(VOID);
static INT DpmiAsyncInjectPm(UINT irq, CONTEXT *context);
static INT WowCall16Sync(DWORD proc, WORD ds, const WORD *args, INT argumentCount, WORD hwnd, WORD message, WORD *result);
static DWORD ShimGlobal16(INT operation, DWORD firstArgument, DWORD secondArgument);
static PVOID ShimMapFlat(WORD segment, DWORD offset, INT isProtectedMode);
static VOID IsvIoIn(PVOID self, WORD port, BYTE width, UINT32 *value);
static VOID IsvIoOut(PVOID self, WORD port, BYTE width, UINT32 value);
static VOID WowShimsLoad(VOID);
static VOID IsvBop(volatile BYTE *tib, DWORD subfunction, PSTR *logCursor);
static INT DpmiNestedFault(volatile BYTE *tib, DWORD event, DWORD eip);
static INT WowCall16SyncEx(DWORD proc, WORD ds, const WORD *args, INT argumentCount, WORD hwnd, WORD message, WORD *result, BYTE *blob, INT blobLength, INT blobArgument, const INT *fix, INT fixupCount);
static LRESULT WowControlColour(HWND window, WORD window16, UINT message, WPARAM wParam, LPARAM lParam, INT *handled);
static LRESULT WowOwnerDraw(HWND window, WORD window16, UINT message, WPARAM wParam, LPARAM lParam, INT *handled);
static INT WowSend16Blob(WORD window16, WORD message, WORD wParam, BYTE *blob, INT blobLength, const INT *fix, INT fixupCount, WORD *result);
static INT WowSend16Now(WORD window16, WORD message, WORD wParam, DWORD lParam, WORD *result);
static INT DpmiInjectPmIrq(DOS_MACHINE *machine, volatile BYTE *tib, UINT interruptVector, UINT steps);
static VOID WowIcaDeliver(DOS_MACHINE *machine, volatile BYTE *tib, UINT steps);
static INT DpmiInjectPmMouseCallback(DOS_MACHINE *machine, volatile BYTE *tib, UINT steps);
static VOID DpmiClientTeardown(VOID);
static INT DpmiRunPmInterp(DOS_MACHINE *machine, volatile BYTE *tib);
static VOID DosWowPublish(volatile BYTE *handlerArea, volatile BYTE *controlTable, UINT currentDrive);
static INT V86DeliverDeviceIrq(volatile BYTE *tib);
static INT V86BiosBop(volatile BYTE *tib, UINT bopNumber, PSTR *logCursor, PSTR base);

#endif
