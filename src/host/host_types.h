/* host_types.h -- the types, macros and constants the host's files share. Declarations only:
 * any of the host's translation units may include it (#335). */
#ifndef NTVDMEX_HOST_TYPES_H
#define NTVDMEX_HOST_TYPES_H

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
/* Floor on how often the WM_TIMER body may do its frame work, in ms. 15 is the ~64 Hz
   this always actually ran at under XP's default granularity. Knob: uitick.txt.
   ★ 0 = AUTO (s73, the default): the present is RAISED BY THE GUEST'S FRAME -- the
     video VDD fires present_hook from the guest's own 0x3DA poll when the beam enters
     the present window, WM_APP_PRESENT runs the frame body at once, and the timer is
     only a fallback (floor = 90% of the mode's frame period) for a guest that never
     looks at the retrace. Two clocks became one; see present_hook in vdd_video.h. */
#define UITICK_AUTO 0
enum { KEYIRQ_RETRY_OFF = 0, KEYIRQ_RETRY_ON = 1, KEYIRQ_RETRY_CLOCK_ON_SCHEDULE = 2, KEYIRQ_RETRY_ONE_YIELD = 3 };   /* keyirq.txt; 2 is refuted (see HostPitDeliver) */
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
#define IO_UNCLAIMED_MAX 24
#define PM_HEADLESS_MS_DEFAULT 30000             /* headless exec-loop wall-clock cap (an infinite visual demo self-exits) */
#define PM_HEADLESS_MS g_HeadlessMs
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
#define PROGRAM_NAME_SIZE 64
enum { DPMI_FAULT_TABLE_ENTRY = 0x10, DPMI_FAULT_TABLE_OFFSET = 4 };   /* g_FaultTable: per class, the code selector then the offset DWORD */
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
enum { SETTINGS_APPLY_STARTUP = 0, SETTINGS_APPLY_LIVE = 1 };   /* SettingsApply: before anything is built, or on a running VM */
enum { IFV_PATH_LIVE = 0, IFV_PATH_VTIB_IRQ01 = 1, IFV_PATH_VTIB_DEVICE = 2, IFV_PATHS = 3 };   /* the IF/VIF census's paths: live (async), IRQ 0/1 via the VTIB, a device IRQ via the VTIB */
enum { ASYNC_WHY_NOT_IN_EXEC = 20, ASYNC_WHY_PIC_REFUSE = 21, ASYNC_WHY_UNHOOKED = 22, ASYNC_WHY_SUSPEND_FAIL = 23, ASYNC_WHY_GETCTX_FAIL = 24, ASYNC_WHY_V86_IF_OFF = 25, ASYNC_WHY_IN_OUR_HANDLER = 26, ASYNC_WHY_OBSERVED = 27, ASYNC_WHY_CTX_BUSY = 28, ASYNC_WHY_LEFT_EXEC = 29, ASYNC_WHY_SIMINT_RM = 30, ASYNC_WHY_NESTED_TICK = 31, ASYNC_WHY_PM_ONLY_LINE = 32, ASYNC_WHY_BAD_IRQ = 40 };   /* 20+: AsyncInjectIrq's early exits; 32 and 40 are past the histogram */
enum { ASYNC_DELIVERED = 0, ASYNC_WHY_BAD_VECTOR = 1, ASYNC_WHY_IN_PM_IRQ = 2, ASYNC_WHY_PM_NO_IRQ = 3, ASYNC_WHY_NO_CATCHER = 4, ASYNC_WHY_UNHOOKED_PM = 5, ASYNC_WHY_NO_APP_TIMER = 6, ASYNC_WHY_VIF_OFF = 7, ASYNC_WHY_IF_OFF = 8, ASYNC_WHY_ARM_QUIET = 9, ASYNC_WHY_IN_FLIGHT = 10, ASYNC_WHY_HOST_STACK = 11, ASYNC_WHY_NOT_32 = 12, ASYNC_WHY_SETCTX_FAIL = 13, ASYNC_WHY_HOST_CS = 14 };   /* g_AsyncWhy: AsyncWhyReport's whyNames, 0-14 */
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
#define DPMI_WATCH_MAX 4
/* ...and when `decl` dominates (it did once the latch was fixed: tried=0x171c decl=0x169d),
   WHICH refusal inside DpmiInjectPmIrq: [0] the interrupted CS is 16-bit (the
   extender's code), [1] the application has no timer hook. Per second on IRQ0TL's clock,
   and the 16-bit sites by CS:EIP, so the refusals can be laid against the quit wait. */
#define PMINJ_SITES 6
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
/* ── #153: FILE > OPEN RECENT. Every program this host was started with, and every one
     opened from the menu, newest first, `Recent1`..`Recent8` beside the settings.
     (A Win16 program started from Windows is not recorded: its path arrives inside
     WOW, not here.) */
#define MRU_MAX 8
enum { INSTALL_VERB_NONE = -1, INSTALL_VERB_INSTALL = 0, INSTALL_VERB_UNINSTALL = 1, INSTALL_VERB_STATUS = 2, INSTALL_VERBS = 3 };   /* InstallVerb: the verbs[] index */
enum { INPUT_KEY_WAIT_MS = 50 };   /* a blocking key read's wait for the next key */
#define TYPEMATIC_DEFAULT_PERIOD_US 92000u
#define TYPEMATIC_DEFAULT_DELAY_US  500000u   /* until XP's own setting is read at startup */
enum { I33_FALLBACK_X = 320, I33_FALLBACK_Y = 240, I33_ABSOLUTE_DELTA_SCALE = 8 };   /* I33TakeMotion's absolute-derived fallback */
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
/* Which arm called us -- a mis-patch can only arrive through the PM BOP, and 0300's
   site is the INT 31h thunk rather than a `CD 33` at all, so the path is evidence. */
#define I33_SRC_V86  1                      /* V86 BOP (patched `CD 33` in real mode) */
#define I33_SRC_PM   2                      /* PM BOP  (patched `CD 33` in PM code)   */
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
#define HOST_KEY_DOWN_BIT       0x8000 /* GetKeyState / GetAsyncKeyState: held now   */
enum { HOST_INSTANCES_MAX = 16, HOST_INSTANCE_WAIT_MS = 200, CAPTURE_MS_MIN = 50, CAPTURE_MS_MAX = 60000, CAPTURE_DELAY_MS_MAX = 600000, HEADLESS_MS_MAX = 3600000 };   /* startup limits: instance numbers, knob ranges */
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
/* What follows the guests' 3DAh reads, site by site: the loops RetraceIdle() must recognise
   are the ones real programs use, so they are MEASURED here, not assumed (STAGE2). */
#define RT_SITES 8
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
/* The interpreter's per-instruction code pointer (v86interp.h, V86I_CODE_PTR): the 16
   bytes at `lin` sit in one already-probed page of plain RAM below 1 MB, outside the
   aperture -- exactly the bytes V86HostRead8's fast path would have read one at a time. */
#define V86I_CODE_PTR 1
#define WOW_ID_NONE 0xFFFF   /* g_WowLastId: no WOW32 call entered yet */
#define WOW_SHADOW_ENTRIES DPMI_LDT_MAX
enum { NESTED_V86_ROUNDS_MAX = 128, PM_INT21_HANDLE_LIMIT = 24, PM_INT21_LOCK_REGION = 0xFF80 };   /* DpmiServicePmIntBody: 0301h's nested run, PM INT 21h */
/* Our BIOS/driver stub BOPs, serviced in one place for the exec loop AND the nested DPMI
   real-mode loop -- defined just above WinMain, where its arms used to live. (GH #247) */
#define V86BOP_NONE  0      /* not one of V86BiosBop's numbers                         */
#define INT15_WAIT_SLEEP_MS     3       /* INT 15h AH=86h: sleep only while more than this is left */
#include <wownt32.h>   /* declarations only: WOW_TYPE_*, the handle types WOWHandle32/16 are given */
#include "../shim/shim_api.h"   /* defines only: the host/shim contract (SHIM_API_VERSION, SHIM_GLOBAL_*) */
typedef struct { PVOID InByte, InWord, InStringByte, InStringWord, OutByte, OutWord, OutStringByte, OutStringWord; } ISV_IO_HANDLERS;
#define ISV_MAX_HOOKS 16
/* What a step of a long function tells its caller to do next (#335): carry on, or leave the
   enclosing loop, start its next pass, or return from the enclosing function. */
enum { HOST_FLOW_NEXT = 0, HOST_FLOW_BREAK, HOST_FLOW_CONTINUE, HOST_FLOW_RETURN };

/* The shared tables' entry types (#335: named so the tables can be declared extern; the
   fields are as they were). */
typedef struct _SKIP_IF_SITE { WORD Cs, Ip, Stub; DWORD Count; } SKIP_IF_SITE, *PSKIP_IF_SITE;
typedef const SKIP_IF_SITE *PCSKIP_IF_SITE;
typedef struct _IO_HOT_PORT { WORD Port; DWORD Count; } IO_HOT_PORT, *PIO_HOT_PORT;
typedef const IO_HOT_PORT *PCIO_HOT_PORT;
typedef struct _PM_INTERRUPT_VECTOR { WORD Selector; DWORD Offset; BYTE Client; } PM_INTERRUPT_VECTOR, *PPM_INTERRUPT_VECTOR;
typedef const PM_INTERRUPT_VECTOR *PCPM_INTERRUPT_VECTOR;
typedef struct _DPMI_MEMORY_BLOCK { DWORD Base, Size; BYTE Code; } DPMI_MEMORY_BLOCK, *PDPMI_MEMORY_BLOCK;
typedef const DPMI_MEMORY_BLOCK *PCDPMI_MEMORY_BLOCK;
typedef struct _PM_EXCEPTION_VECTOR { WORD Selector; DWORD Offset; INT IsSet; } PM_EXCEPTION_VECTOR, *PPM_EXCEPTION_VECTOR;
typedef const PM_EXCEPTION_VECTOR *PCPM_EXCEPTION_VECTOR;
typedef struct _DPMI_CALLBACK { WORD PmSelector; DWORD PmOffset; WORD RmEs; DWORD RmDi; INT IsUsed; } DPMI_CALLBACK, *PDPMI_CALLBACK;
typedef const DPMI_CALLBACK *PCDPMI_CALLBACK;
typedef struct _IFV_TRACE_ENTRY { BYTE Irq, Path, State; WORD Cs, Ip; DWORD Flags; } IFV_TRACE_ENTRY, *PIFV_TRACE_ENTRY;
typedef const IFV_TRACE_ENTRY *PCIFV_TRACE_ENTRY;
typedef struct _PM_INJECT_SITE { WORD Cs; DWORD Eip, Count; } PM_INJECT_SITE, *PPM_INJECT_SITE;
typedef const PM_INJECT_SITE *PCPM_INJECT_SITE;
typedef struct _I33_FUNCTION_COUNT { WORD Ax; DWORD Count; } I33_FUNCTION_COUNT, *PI33_FUNCTION_COUNT;
typedef const I33_FUNCTION_COUNT *PCI33_FUNCTION_COUNT;
typedef struct _I33_CALL_SITE { DWORD Linear, Eip, Count; WORD Cs, Ax; BYTE Source; BYTE Context[12]; } I33_CALL_SITE, *PI33_CALL_SITE;
typedef const I33_CALL_SITE *PCI33_CALL_SITE;
typedef struct _RETRACE_SITE { DWORD Cs, Ip, Count; BYTE Bytes[10]; } RETRACE_SITE, *PRETRACE_SITE;
typedef const RETRACE_SITE *PCRETRACE_SITE;
typedef struct _ISV_IO_HOOK { HANDLE VddHandle; WORD FirstPort, LastPort; ISV_IO_HANDLERS Handlers; INT IsLive; } ISV_IO_HOOK, *PISV_IO_HOOK;
typedef const ISV_IO_HOOK *PCISV_IO_HOOK;
/* A descriptor in the DPMI host's LDT shadow (g_Ldt). */
typedef struct _DPMI_DESCRIPTOR { DWORD Base, Limit; BYTE Access, Flags; } DPMI_DESCRIPTOR, *PDPMI_DESCRIPTOR;
#endif
