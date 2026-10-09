/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The clean DOS VDM host (windowed). Orchestrates the pipeline the
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
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include <windows.h>
#include <commctrl.h>
#include "ntvdm.h"
#include "host_core.h"          /* the folder and its paths, the host lock, guest-memory access */
#include "v86.h"
#include "dpmi.h"
#include "csrss.h"
#include "log.h"
#include "settings.h"           /* registry-backed knobs + the Settings dialog */
#include "mgrproto.h"           /* GH #281: the NTVDMEX manager -- one tray icon for every program */
#include "pcspeaker.h"          /* the OTHER speaker: the one on the motherboard */
#include "install.h"            /* GH #13: becoming the machine's VDM, reversibly */
#include "x86len.h"             /* which `CD nn` byte pairs are really INT instructions */
#include "dpmi_rmcs.h"          /* GH #247: the real-mode call structure, and 0300h's routing */
#include "dpmi_svc.h"           /* GH #248: INT 31h's spec-decided answers (selectors, callbacks, 0503h) */
#include "i33_driver.h"         /* GH #264/#265: INT 33h cursor masks, profiles, alternate handlers */
#include "pif.h"                /* a .PIF's program, directory and parameters */
#include "../wow/ne.h"          /* GH #128: 16-bit New Executable loader (WOW bootstrap) */
#include "../wow/wow32.h"       /* GH #128: the 32-bit half -- krnl386's calls out to Win32 */
#include "../wow/wowanchors.h"  /* GH #128: ...and how a thunk module's segment is RECOGNISED */
#include "../wow/wowsched.h"    /* GH #128: ...and the Win16 task scheduler, which is also ours */
#include "../wow/wowcall.h"     /* GH #128: ...and the OTHER direction -- calling 16-bit code */
#include "../wow/wowmsg.h"      /* GH #128: ...and the MESSAGE QUEUE the loop turns on */
#include "../wow/wowres.h"      /* GH #128: ...and the guest's OWN menu and icons */
#include "../wow/wowwin.h"      /* GH #128: ...and a Win16 window IS a real Win32 window */
/* [CAUTION]: wowgdi.h COMES BEFORE wowuser.h, and the order is load-bearing: USER's
 * GetDC/GetWindowDC issue a GDI token, so the object map has to be in scope by
 * the time USER's dispatcher is compiled. It used to be last only because it
 * borrowed USER's note helpers, and those now live in wow32.h.
 */
#include "../wow/wowgdi.h"      /* GH #128: GDI.EXE's id space -- where MS Paint begins */
#include "../wow/wowuser.h"     /* GH #128: USER.EXE's id space -- a DIFFERENT one; see the file */
/* [CAUTION]: AFTER wowuser.h, and that order is load-bearing too: the modal loop reads
 * the window table and the procedure rule that file owns. USER's DialogBox and
 * EndDialog arms reach it through the three prototypes declared there.
 */
#include "../wow/wowdlg.h"      /* GH #128: ...and the MODAL loop -- why DialogBox does not return */
#include "../wow/wowenum.h"     /* GH #128: ...and one callback per item -- EnumWindows, LineDDA */
#include "../wow/wowshell.h"    /* GH #128: ...and SHELL.DLL's, which is a THIRD one again */
#include "../wow/wowcommdlg.h"  /* GH #128: ...and COMMDLG.DLL's -- File > Open */
#include "../wow/wowkbd.h"      /* GH #128: ...and KEYBOARD.DRV's -- ANSI/OEM conversion */
#include "../wow/wowsound.h"    /* GH #299: ...and SOUND.DRV's -- stock answers 0 */
#include "../wow/wowmmedia.h"   /* GH #278: ...and MMSYSTEM's two -- mmCallProc32 */
#include "dos_mcb.h"
#include "bios_bda.h"           /* GH #253: 0040:000E/0010/0013 and the EBDA, from one source */
#include "dos_loader.h"
#include "dos_psp.h"
#include "dos_env.h"
#include "dos_int21.h"
#include "dos_auxprn.h"         /* #251: AUX/PRN driver code planted at DOS_CTAB_SEG */
typedef CHAR DOS_AUXPRN_FITS[(sizeof(g_DosAuxPrnCode) <= DOS_AUXPRN_LEN) ? 1 : -1];
#include "bios_kbdact.h"    /* #254: INT 09h side-calls planted at DOS_CTAB_SEG */
#include "bios_prtsc.h"     /* #274: the default INT 05h's byte sequencer */
typedef CHAR BIOS_KBDACT_FITS[(sizeof(g_BiosKeyboardActionCode) <= DOS_KBDACT_LEN
                               && DOS_AUXPRN_OFF + DOS_AUXPRN_LEN <= DOS_KBDACT_OFF
                               && DOS_KBDACT_OFF + DOS_KBDACT_LEN <= DOS_GENSTUB_OFF
                               && DOS_GENSTUB_OFF + DOS_GENSTUB_N * DOS_GENSTUB_SIZE <= DOS_CTAB_END) ? 1 : -1];
#include "dos_layout.h"
#include "dos_disk.h"       /* GH #44: image geometry + CHS<->LBA */
#include <tlhelp32.h>
#include "dos_recovery.h"   /* GH #132: what to do when we will not start */
#include "dos_err.h"        /* #34: the INT 24h contract (dos_crit_*) */
#include "dos_sysvars.h"    /* GH #48: the List of Lists, built to the measured 6.22 layout */
#include "dos_ctab.h"
#include "dos_xms.h"
#include "dos_extmem.h"     /* GH #54: INT 15h AH=87h address resolution */
#include "dos_ems.h"
#include "vdd_bus.h"
#include "vdd_pit.h"
#include "vdd_cmos.h"
#include "vdd_fdc.h"
#include "vdd_ide.h"
#include "vdd_pic.h"
#include "vdd_video.h"
#include "sysfont.h"        /* #322: the VGA tables from the system fonts */
/* #321: what the last font build did (Settings shows it) and the TextFont it was for. */
SYSFONT_REPORT g_SysFontReport;
CHAR             g_TextFontLive[NTVDMEX_PATH_MAX];
#include "vdd_input.h"
#include "vdd_speaker.h"
#include "vdd_joy.h"
#include "vdd_dma.h"
#include "vdd_opl.h"
#include "vdd_sb.h"
#include "vdd_gus.h"
#include "vdd_emu8k.h"      /* #233: the AWE32's wavetable chip */
#include "vdd_mpu.h"
#include "vdd_comm.h"
#include "vdd_net.h"        /* GH #8 (s91): NetBIOS via INT 5Ch */
#include <nb30.h>
#include "../../sdk/include/ntvdmex-vdd.h"
#include "vdd_audio.h"
#include "audio_wave.h"
#include "midi_route.h"     /* #136: Settings > Audio > MIDI -> a host device, by name */
#include "present_ddraw.h"

/* The host's other modules: what each one offers main.c (#335). */
#include "host_types.h"
#include "host_state.h"
#include "host_dpmi_client.h"
#include "host_report.h"
#include "host_dpmi_int.h"
#include "host_dos.h"
#include "host_irq.h"
#include "host_video.h"
#include "host_diag.h"
#include "host_io.h"
#include "host_bios.h"
#include "host_dpmi.h"
#include "host_wow.h"
#include "host_input.h"
#include "host_mouse.h"
#include "host_window.h"
#include "host_settings.h"
#include "host_audio.h"
#include "main.h"
#include "host_timing.h"
#include "host_install.h"

static BYTE g_TrampolineSave[DOS_HDLR_TRAMPOLINE_SIZE];
static INT  g_TrampolineSaved;

WORD g_DsProbe[DSPROBE_MAX];
INT  g_DsProbeCount = 0;
WORD g_CsProbe[DSPROBE_MAX];
INT  g_CsProbeCount = 0;

/* Set per BOP in the exec loop: did this `C4 C4 nn` execute in GUEST code rather than at
 * one of the addresses we plant ours at? See the note where it is assigned.
 */
static INT g_BopFromGuest = 0;
DWORD g_NtvdmBopCount = 0;   /* how many guest-issued NTVDM BOPs this run serviced */
/* s79: set at load time, from the IMAGE, not the path -- see the scan in STAGE2.
 * g_GuestNtAware means "we loaded this as a shell AND it talks to NTVDM", which is
 * what earns it DOS 5.00 and the private AH=53h answers.
 */
static DWORD g_GuestNtvdmBops = 0;
static INT   g_GuestNtAware    = 0;
static INT   g_ShellGetNextCount  = 0;
static CHAR  g_ShellPath[300];         /* the shell we loaded, for its COMSPEC (s81) */   /* BOP 54 sub 01 calls this session -- see its arm (s81) */
static CHAR  g_FirstProgram[300];         /* its 8.3 path -- the sub 01 NAME field */
static CHAR  g_FirstTail[128];         /* its arguments -- the sub 01 command TAIL */

static DWORD g_EmsFrameLinear;                        /* set by VdmMapEmsFrame */

/* CSRSS receive buffers + program image (no CRT heap; static = zero-init). */
static CHAR g_CommandLine[1024];
static CHAR g_Application[1024];
static CHAR g_CurrentDirectory[512];
static CHAR g_PifPath[512];
static WORD g_CdsSegment;                       /* the CDS array's reserved block, 0 = none */
static WORD g_SftSegment;                       /* the SFT block for a DOS guest, 0 = none */

static CHAR g_Environment[8192];
static CHAR g_Desktop[512];
static CHAR g_Title[512];
static CHAR g_Reserved[512];
VDM_COMMAND_INFO g_CommandInfo;
/* The SECOND fetch (s72): what CSRSS actually queued -- AppName is the program's
 * full path, CmdLine its argument tail (CR LF terminated), Env the launcher's Win32
 * environment block. See csrss_fetch_command().
 */
static CHAR g_Application2[1024];
static CHAR g_CommandLine2[1024];
static CHAR g_CurrentDirectory2[512];
static CHAR g_Environment2[8192];
static INT  g_Fetch2Ok = 0;

static volatile LONG g_ReportGotNext = 0;   /* the report call returned TRUE: a command was queued to us */
static DWORD WINAPI CsrssReportThread(LPVOID parameter)
{
    DWORD exitCode = 0;
    BOOL isOk = CsrssTaskDone(g_CommandInfo.TaskId, (ULONG)(ULONG_PTR)parameter, &exitCode, NULL);

    if (isOk)
        InterlockedExchange(&g_ReportGotNext, 1);

    return exitCode;
}

static BYTE g_FileBuffer[0x80000];   /* 512KB: hold a real game's MZ image (DOS/4GW stub etc.), run 85 */

static FDC_STATE    g_Fdc;       static NTVDD_DEVICE g_FdcDevice;
static IDE_STATE    g_Ide;       static NTVDD_DEVICE g_IdeDevice;
DMA_STATE    g_Dma;       static NTVDD_DEVICE g_DmaDevice;
static BYTE      g_GusDram[GUS_DRAM_SIZE];
INT          g_GusOn = 0;
static WORD     g_Emu8KDram[EMU8K_DRAM_WORDS];
PCSTR g_DosVersionWhy = 0;
static INT          g_DosVersionShell = 0;      /* #208: an XP shell is present, told 5.00 itself */
UINT32 g_PitAsyncAttempts;
DWORD g_KeyPmLogged  = 0;           /* bounded KEYPM account; see the PM exec loop */
INT g_PmIrq0Latch = 0;             /* #2b: a virtual IRQ0 awaiting injection into the PM hook */

/* WHAT IS THE GUEST DOING DURING A LONG TIMER GAP? (Skyroads wobble, s61) (Importance = 1):
 * The gaps that wobble the game are the long ones, and the question is whether
 * they are the guest's music ISR overrunning on our slow (trapped) OPL port I/O,
 * or something else (a compute stretch, our own delivery latency). So split the
 * port-I/O count by gap size. `dio` is the port events serviced since the last
 * delivery -- g_EventIo is the running total. Also keep the single worst gap's I/O
 * and where the guest was when we finally delivered it.
 *
 * [CAUTION]: TWO DEFECTS IN THE FIRST CUT OF THIS, AND THE SECOND ONE PRODUCED A REFUTATION
 *  THAT DOES NOT HOLD. Both are measured, on the s61 in-game run:
 *
 *  1. "LONG" WAS A FIXED 8 ms, AND THE GUEST CHANGES THE TICK RATE UNDER IT.
 *     Skyroads' intro runs at the BIOS 18.2 Hz -- a 55 ms period -- and only
 *     switches to 180 Hz (5.56 ms) in game. So every NORMAL intro tick scored as
 *     a "big gap": 187 of the 319 were the intro ticking correctly. The threshold
 *     has to be RELATIVE TO THE PERIOD THE GUEST PROGRAMMED, or it measures the
 *     guest's own choice of rate and calls it a fault.
 *  2. I/O PER *GAP* IS CONFOUNDED BY GAP LENGTH. A gap ten times longer collects
 *     ten times the I/O whatever the guest is doing, so "big gaps carry more I/O"
 *     is true by construction and says nothing about the music. It has to be a
 *     RATE -- I/O per millisecond -- before the two populations are comparable.
 *     The s60 note recorded the opposite sign of this ratio and read it as
 *     REFUTING the music-overrun hypothesis; on this run the raw ratio comes out
 *     25x the other way. Neither number means anything until it is a rate.
 *
 *  Both are the [[instrument-must-not-infer-its-own-frame]] shape: an instrument
 *  that quietly assumes a constant the system is free to change.
 */
DWORD g_Irq0IoPrevious;
DWORD g_Irq0WorstGapMs;
DWORD g_Irq0WorstGapIo;
/* where IF re-opened */
DWORD g_Irq0WorstCs;
DWORD g_Irq0WorstIp;
DWORD g_Irq0NormalCount;
DWORD g_Irq0NormalMicroseconds;
DWORD g_Irq0NormalIo;
DWORD g_Irq0WorstRaise;
DWORD g_Irq0WorstAttempts;
DWORD g_Irq0WorstNie;
DWORD g_Irq0WorstYield;
DWORD          g_Irq0WorstPerMicroseconds;   /* the period in force at the worst gap */
/* V86 single-run stretch durations (Skyroads wobble, s61). See the exec loop. */
DWORD g_V86StringHistogram[8];
DWORD g_V86StringCount8;
DWORD g_V86StringMaximumMs;
DWORD g_V86StringMaximumCs;
DWORD g_V86StringMaximumIp;
DWORD g_V86StringMaximumEvent;
DWORD          g_HeartbeatDs = 0;            /* guest DS sampled by the heartbeat (s69 fade dump) */
DWORD          g_EventIoString      = 0;  /* REP INS/OUTS (event 1) reflects serviced */
static HANDLE         g_OnceMutex    = NULL; /* the single-instance mutex (WinMain); handed
                                                 over before a relaunch, see task-done */
/* [CAUTION]: AN INTERMITTENT I/O STORM, NOT YET EXPLAINED (session 53) (Importance = 1):
 * Some runs of tests/probes/dos/spktest.com report 1.87 MILLION serviced I/O
 * events for a program that issues EIGHTY-SIX, take 8.7 s where a clean run
 * takes 4.6, and log the SAME SIX guest CS:IP sites either way -- so the same
 * handful of instructions are being serviced ~21,700 times each while the
 * guest still completes correctly. Four consecutive runs on the build that
 * followed would not reproduce it, so it is not the binary and not the
 * settings; it is a race with something.
 * - THESE THREE COUNTERS ARE THE DIAGNOSIS, PRE-PLACED. The two service paths
 *   differ in exactly the way that matters: HostTryIo() ADVANCES EIP past the
 *   instruction, HostTryIoRetro() deliberately DOES NOT (the real-hardware
 *   event reports CS:IP already past the I/O). A storm that is all `retro` is a
 *   guest that is not being stepped; a storm that is all `direct` is the kernel
 *   re-reporting an event we already retired. The event histogram says which
 *   kernel event is arriving. One dirty run now answers the question.
 */
DWORD g_IoViaDirect = 0;
DWORD g_IoViaRetro = 0;
static LONGLONG g_HostTimeLast;           /* QPC of the last VdmRunGuest return; 0 = none */
static INT  g_HostEventLast;
INT            g_NoA000       = 0;  /* NOA000_FLAG present: leave A0000 mapped (diagnostic) */
INT            g_NoPmPatch    = 0;  /* NOPMPATCH_FLAG present: scan no code regions (diagnostic) */
INT            g_Fault32Warned  = 0;  /* said once: NT's 16-bit frame cannot locate a flat client's INT */
DWORD          g_NoPmPatchMinimum = 0; /* ...or only regions >= this many bytes */
/* MEMDUMP_FLAG */
DWORD g_MemoryDumpLinear = 0;
DWORD g_MemoryDumpLength = 0;
static INT            g_Interp12      = 0;  /* INTERP12_FLAG: interpret mode 12h, no page trap */
DWORD          g_RunStartTick= 0;  /* exec-loop start, so STAGE2 can report a RATE */
/* Planar-mode interpretation, measured. `batches` is how many times we drove the
 * guest from the host, `instrs` how many instructions that came to, and `bails`
 * how often the interpreter declined the very first opcode and had to let V86 run
 * (each bail is a stretch of guest execution whose A0000 writes we do NOT see).
 */
DWORD          g_P12Batches   = 0;
DWORD          g_P12Instructions    = 0;
DWORD          g_P12Bails     = 0;
/* EVERY DISTINCT BAIL SITE, NOT THE FIRST TWELVE LINES. (s68):
 * In a planar mode a bail is not one instruction: VdmRunGuest keeps the guest until the
 * next EVENT with A0000 unprotected, so every VRAM write in that stretch is lost to
 * st->plane[]. The list of bail sites IS the to-do list for the interpreter, and a
 * 12-line budget spent on one site hid the `repne scasb` that cost Lemmings its
 * sprite erase for four sessions. Linear table, BOP stubs excluded, printed at
 * exit with the bytes so each entry can be decoded without a dump.
 */
P12_SITE g_P12Site[P12_SITE_MAX];
UINT g_P12SiteCount = 0;
UINT g_P12SiteLost = 0;
DWORD g_HeadlessMs = PM_HEADLESS_MS_DEFAULT;   /* overridable via HEADLESS_MS_PATH */
INT   g_LdtClientMark = 0;          /* g_LdtNext when the client switched in */

/* Set when the APPLICATION (not the extender's arming pass) installs a timer ISR. Until
 * then vector 8 holds a placeholder stub and delivering to it is both pointless and, on
 * DOS/4GW, fatal. Learned by snooping INT 21h AH=25h -- see the routing path.
 */
INT   g_DpmiUseKernel = 0;         /* pmkernel.flag: run PM via VdmStartExecution */
INT  g_CloseForced;           /* the exit in progress is ours, not the guest's */
/* #244: IS INT 15h STILL OURS?:
 * The BIOS INT 09h calls INT 15h AH=4Fh for every byte (bios_kbdact.asm k4f). Our own
 * INT 15h answers 4Fh with CF=1 and AL untouched -- "process it as it is" -- so while
 * IVT[15h] still points at our stub the call cannot change anything, and making it
 * would cost two VM exits per scancode on the path Doom and Skyroads are fragile on.
 * So the call is made only once something has hooked the vector: exactly the case
 * the intercept exists for (KEYB-style remappers, hot-key TSRs, p_kbd3).
 * g_Int15StubOffset is recorded where the stub is planted (bios_ints[]).
 */
WORD  g_Int15StubOffset;
/* k4f entries / bytes it handed back */
DWORD g_Kb4FCalls;
DWORD g_Kb4FTranslate;
DWORD g_PrintScreenJobs;
DWORD g_PrintScreenErrors;
BYTE       g_PrintScreenStatus = BIOS_PRINT_SCREEN_STATUS_OK;   /* what 0050:0000 would hold */
DWORD          g_ExecPriority     = 0;   /* guest thread priority class; see EXECPRIO_PATH */
static INT            g_QiRaise      = 0;
static INT            g_QiVif        = 0;   /* start the guest with EFLAGS.VIF set */
static INT            g_QiKeys       = 0;   /* synthesise keypresses (repro the hang) */
DWORD          g_InterpRefused = 0;  /* interpreter declined the faulting opcode */
/* A TIMER RE-ARMED FROM INSIDE ITS OWN HANDLER DISCARDS THE TICK QUEUED BEHIND IT (Importance = 4):
 * s70, the user's by-hand Lemmings run: the fade completed, then "it stalled when the
 * trapdoors opened" -- every heartbeat from then on inside the ISR's retrace spin,
 * irq0/s == edges/s == 71, p3da 3.1M reads/s, and the new counters read blocks=796,
 * timeouts=1. Not nesting this time (the in-service guard held) but a LOCKSTEP: one
 * long tick (250 ms in service -- the trapdoor moment) left a tick queued behind the
 * handler; the queued tick re-entered the handler AT THE EOI, that instance spun to the
 * next retrace and re-armed the PIT, and its own tick then fired during the NEXT
 * instance's spin -- queued again, forever. The main loop never got a cycle. A real
 * 8259 latches one IRR bit and would do exactly the same after such a stall; Lemmings'
 * design is fragile and a 386 simply never stalled that long.
 * - The guest's own act names the fix. A `out 43h` + count on counter 0 from INSIDE the
 *   IRQ0 handler is the guest saying "the next tick is N clocks from NOW" -- it has just
 *   resynchronised. A tick that was queued before that instant belongs to the period it
 *   just discarded, so drop it: clear the V86 backlog and the IRR bit. For a guest that
 *   programs its timer once at start-up (Skyroads, Doom's DMX, the BIOS rate) the
 *   condition never holds -- no restart while in service -- so nothing changes for them.
 *   The deviation from the hardware is exactly one tick, at exactly the moment the
 *   guest asked for a new period. Counted: STAGE2 irq0_isr[...,resync_drop].
 */
DWORD    g_Irq0ResyncDrop = 0;
/* #172: WHY THE PER-PASS TIMER LATCH LEFT A BACKLOG STANDING:
 * Doom's quit wait (I_WaitVBL, a PM 3DAh poll) drops IRQ0 to ~25/s, and s81 filed it
 * as ticks never RAISED. The same run's IRQ0WHY says otherwise -- gen=0 del=0x9e: every
 * long gap had the ticks raised and not delivered -- and the async arm is expected to
 * miss during a trap storm (why=20, the CPU thread is in host code). Every trap returns
 * to the PM loop, so the per-pass latch should catch up within microseconds. It does
 * not, and this names the gate. Counted per PASS with >= 2 ticks owed (the `del`
 * signature), first failing gate only:
 * 0 no latch   1 vIF=0   2 no INT 08h hook   3 in a PM IRQ   4 noirq
 * 5 async injection in flight   6 vec 8 armed < 55 ms ago
 * 7 all gates open (the pass tried)   8 claim refused (IRQ0 masked/in service)
 * 9 the injector declined (guest in the extender's 16-bit code)
 * 7 counts the pass; 8/9 are that pass's failures, so 7 - 8 - 9 = delivered.
 */
DWORD g_PmCooperativeGate[PM_GATES];
/* WHICH INJECTION PATH ACTUALLY PRODUCES A REFILL?:
 * Measured: DMX polls the 8237's channel-1 CURRENT COUNT ~55 times a second (all
 * 8-bit reads, so two per poll) while 82 DMA blocks complete -- and 32% of audible
 * blocks are lap repeats, which is the same fraction as the blocks that complete
 * without DMX ever having looked. 55/s is also, to within 1.5% in two separate runs,
 * the ASYNCHRONOUS arm's delivery rate -- while the cooperative arm delivers 79/s
 * more on top and the total is 135/s.
 * If that is a coincidence, DMX's poll rate is its own and the refill headroom is the
 * thing to attack. If it is not, then the two injection paths are NOT equivalent from
 * the guest's side -- a cooperative tick enters the same handler and does not produce
 * the same work -- and making them equivalent would take refills from 55/s to 135/s.
 * Two ratios agreeing is not a mechanism, so measure it directly: count the count-reads
 * that happen INSIDE a cooperative INT 08h. The handler runs synchronously within
 * DpmiInjectPmIrq(), so a before/after snapshot of the counter brackets it exactly,
 * with no new plumbing into the device model. Near zero here confirms it.
 */
UINT32 g_CooperativeDmaPolls;
UINT32 g_CooperativeDmaPollsDevice[PIC_LINES_PER_CHIP];   /* ...and the same, per DEVICE line */
/* Cooperative delivery of DEVICE lines (2-7) to a PM client -- the retry the async
 * path never had. `inj` is the interrupts that would previously have been LOST.
 */
DWORD g_PmDeviceIrqInjected  = 0;
DWORD g_PmDeviceIrqFail = 0;
DWORD g_PmDeviceIrqDrop = 0;           /* pending on a line the client never hooked */
DWORD g_PmStretchMaximumMicroseconds = 0;
DWORD g_PmStretchLogged = 0;
DOS_START_MODE g_StartMode = DOS_START_NORMAL;

/* [CAUTION]: Reported at EXIT, not at init. The early-startup log line was written
 * before a later LogWrite(LOG_PATH,...) TRUNCATES the file, so it never
 * survived to be read -- which looked exactly like the code not running.
 */
PCSTR g_StdioHow = "(not initialised)";
PCSTR g_StdioSource = "";   /* which channel it came down, if any */
DWORD g_StdioParentProcessId;   /* GH #131: whose child we turned out to be */
DWORD g_DmxSamples;
DWORD g_DmxBusy[12];
DWORD g_DmxMixerOk;
DWORD g_DmxOverdue;
DWORD g_DmxOverdueMaximum;
DWORD g_DmxAnyBusy;
/* PACE THE PIT:
 * HostPitSync() advances the emulated 8254 by however much wall-clock has elapsed
 * since the last call, raising one IRQ0 per reload period -- so its CALL RATE sets
 * how evenly the guest's ticks land. It is driven by the UI thread and by guest I/O
 * traps, and measured it runs 65 times a second against a 140 Hz timer: each call
 * therefore raises about two ticks, which go out back-to-back.
 * Measured interval between DELIVERED IRQ0s (n=6069, 135/s, so 7.4 ms if even):
 *     <0.5ms  52.7%     8-16ms  22.9%     16-32ms  18.6%     max 48 ms
 * 53% arrive in BURSTS and 28% of gaps exceed 11.6 ms -- one DMA block. That is the
 * whole audio defect: DMX's mixer is armed by the SB block IRQ (next_due = NOW) and
 * serviced on the next timer tick, so a gap longer than a block lets a second block
 * arm before the first is serviced. The two arms COLLAPSE into one refill and a block
 * is never filled -- 32.8% measured, against 28% of gaps being over-length.
 * - So call it far more often. At ~1 kHz each sync raises at most one tick and the
 *   ticks come out evenly, WITHOUT changing the rate: the 8254 still advances by real
 *   elapsed time and the guest still gets the 140 Hz it programmed. This is a pacing
 *   change, not a rate change -- which matters, because session 22 proved that
 *   delivering MORE ticks per opportunity (DPMI_IRQ0_BATCH) compresses game time and
 *   is catastrophic.
 *
 * [CAUTION]: 1 ms Sleep needs the multimedia timer resolution raised; without timeBeginPeriod
 * XP's default granularity is ~15.6 ms and this thread would run slower than the UI
 * one it is meant to replace. Loaded dynamically, as audio_wave.c already does for
 * waveOut, so the import allowlist is unaffected.
 *
 * [CAUTION]: It takes g_Lock like every other caller, so it is a knob (pitpace.txt = 0 to
 * disable) and the lock figures must be read on the first run with it on.
 */
typedef MMRESULT (WINAPI *PFN_TIME_BEGIN_PERIOD)(UINT);
static HANDLE g_PitPaceThread;
/* #256: 1 while the TOP-LEVEL PM loop is dispatching -- the one place a PM BIOS wait may
 * re-execute its BOP (every nested loop counts its passes). See the PM INT 15h 86h arm.
 */
INT g_PmTopDispatch;   /* set by the top-level loop before it dispatches */
static HANDLE g_CourierThread;
UINT g_CpuSpeedReferenceMhz = CPUSPEED_REF_MHZ_DEFAULT;  /* cpuref.txt */
static HANDLE g_CpuSpeedThread;
/* TYPEMATIC REPEAT: WE ARE THE KEYBOARD, SO WE MUST DO ITS REPEATING:
 * THE BUG (user, reported twice): crash the ship in Skyroads while holding the up
 * arrow and, on real DOS or stock ntvdm, the restarted level accelerates
 * immediately. Under NTVDMEX you must release the key and press it again, and it
 * works about one time in ten.
 * - WHY. The game hooks INT 09h and tracks key state from make/break codes. Its
 *   restart path clears that state, so the key has to be RE-ASSERTED -- and on real
 *   hardware it is, because an AT keyboard repeats the held key by itself, roughly
 *   every 92 ms after a ~500 ms delay. The guest sees a fresh make and carries on.
 * - WHAT WE WERE DOING. Outsourcing that to Windows: one make per WM_KEYDOWN, OS
 *   auto-repeat included. But auto-repeat arrives as WINDOW MESSAGES, and the UI
 *   thread was measured stalling up to 857 ms at a time -- so the repeats simply do
 *   not arrive. MEASURED: 3205 scancodes across a whole session, when a single
 *   60-second hold should exceed that on its own. The one-in-ten is the race
 *   between the restart and a repeat that mostly is not coming.
 * - THE FIX IS A MODELLING FIX, not a tuning one. A real keyboard's repeat does not
 *   depend on how busy the host is, so ours must not either: the deadline is driven
 *   from QueryPerformanceCounter and pumped from BOTH the exec loop and the UI
 *   timer, exactly as the PIT already is. When one thread stalls the other covers.
 * - ONE KEY, ONE PATH. OS auto-repeat is now SUPPRESSED (WM_KEYDOWN bit 30) because
 *   we generate our own; letting both through would double the rate and make typing
 *   stutter. That is the same principle that fixed the earlier keyboard bug, where
 *   a parallel host-side ring meant INT 16h appeared to work while the BIOS ring
 *   stayed empty forever.
 * Only the most recently pressed key repeats, which is what AT hardware does.
 */
/* - THE RATE IS NOT A CONSTANT AND MUST NOT BE ONE HERE. These started as 500 ms
 * and 10.9/s, remembered AT hardware defaults. MEASURED against stock ntvdm on
 * the same box with the same keyboard (tests/probes/dos/tymat.asm, run under both
 * hosts via rt_stock.bat):
 *   stock ntvdm   delay 7 ticks ~385 ms   102 repeats -> 22.1/s
 *   us            delay 9 ticks ~495 ms    49 repeats -> 10.9/s
 * Half the rate and a third again the delay. Hardcoding 385/22.1 would repeat
 * the same mistake with a fresher number, because that is THIS box's XP setting,
 * not a universal truth -- the user can change it in Control Panel and a real DOS
 * box would follow. So take it from the system, which is the thing stock ntvdm is
 * effectively passing through, and keep the measured pair above as the
 * VERIFICATION target rather than the source.
 */
UINT32 g_TypematicDelayMicroseconds  = TYPEMATIC_DEFAULT_DELAY_US;   /* replaced at startup from XP's setting */
/* raw, so STAGE2 can show them */
DWORD g_TypematicSpiDelay;
DWORD g_TypematicSpiSpeed;

/* WHAT DID THE GUEST DO AFTER THE CALLBACK WAS INJECTED? (s71):
 * QB's first callback was injected (inj=1) and never came back (done=0, no STRAY),
 * while the guest carried on ticking -- so every later event was refused as
 * "in flight" and a click opened nothing. Nothing in the log said where the handler
 * went: the heartbeat only ever shows the last TRAP, which for an idle text-mode
 * guest is the INT 08h stub. So for the first few injections, log the next few VM
 * events verbatim (event, BOP number, cs:ip, ss:sp, the code and stack bytes). The
 * first event after a good injection is the handler's own CLI/STI reflection or its
 * RETF landing on the return BOP; anything else names the wrong turn. Bounded.
 */
INT    g_MouseCallbackTrace;            /* VM events still to log after an injection */
static WORD      g_WowEntryDs = 0;   /* krnl386's autodata paragraph */
static INT       g_WowEntering = 0;   /* the guest is krnl386, not DOS */
/* #153: FILE > OPEN EXECUTABLE / OPEN RECENT:
 * User decision (s81): if this window is sitting at the top-level shell's prompt,
 * TYPE the program into it -- drive, `CD`, name -- so it runs here and the prompt
 * comes back afterwards. Otherwise start it in a new window, as a double-click does.
 * - "At the prompt" is read off the DOS kernel, not guessed: the top-level program is a
 *   shell, nothing is EXEC'd under it, and it is inside its AH=0Ah line read. Anything
 *   already typed on that line is rubbed out first (our AH=0Ah honours backspace).
 *
 * [CAUTION]: A NEW WINDOW NEEDS CREATE_NEW_CONSOLE. This process IS a VDM, and a DOS program
 * started from its console is queued by CSRSS to THIS VDM -- the relaunch case in the
 * task-done path -- rather than getting a machine of its own.
 *
 * [CAUTION]: Only a DOS image is typed: a Win16/Win32 one at a DOS prompt just says it needs
 * Windows. And the directory must fit DOS's 64-character current-directory limit.
 */
/* #211: TWO BUSY GUESTS MUST NOT STARVE THE MACHINE:
 * The thread that runs the guest is ABOVE_NORMAL (execprio), and a DOS program that
 * polls its keyboard keeps it busy for ever -- QBasic idling in its editor is a whole
 * core. Measured on the 2-core rig (s81): QBasic in one host and Skyroads in another
 * took both cores, and every NORMAL process -- cmd, tasklist, the rig's own harness
 * -- stopped dead until the hosts were killed.
 * - So while ANOTHER NTVDMEX is running, a host whose window is not in the foreground
 *   drops its guest to BELOW_NORMAL, and gets its own priority back when it is brought
 *   forward. A host running alone is never touched: its priority is exactly what it was,
 *   which is what the Skyroads timing guard and every rig measurement assume.
 */
HANDLE g_ExecThread;                        /* the exec (guest) thread */
INT    g_ExecPriorityForeground = THREAD_PRIORITY_NORMAL;
NTVDMEX_SETTINGS g_SettingsDisk;

INT g_DspVersionForced;                /* cfg\dspver.txt beat the model's version */

/* #136: CONVENTIONAL MEMORY, AND WHERE IT ENDS:
 * g_ConventionalKbWant is the setting (memory FITTED, 64..640 KB); g_DosMemoryTop is the
 * paragraph DOS's arena ends at and the EBDA starts at, decided ONCE at start-up from
 * it (BiosConventionalTopParagraph -- 640 KB gives exactly DOS_MEM_TOP, 9FC0h, so the default
 * machine is the one every build so far has run). Every consumer reads the variable:
 * INT 12h, 0040:0013, 0040:000E, INT 15h C1h, CMOS 15h/16h, the first PSP's +02h and
 * the MCB chain. [CAUTION] START-UP ONLY: SettingsApply runs again on a dialog OK, but moving
 * the top of an arena a program is already running in is not something any machine
 * does; the new value is the next program's.
 */
UINT g_ConventionalKbWant = BIOS_CONV_KB_MAX;
/* WHERE DOES A MAP-MASK WRITE GO IF IT DOES NOT MOVE THE WINDOW?:
 * The last run wrote the map mask 2,042,942 times and swapped 1,867,689 times: 175,253
 * writes -- 8.6% -- did not move the window, and nothing says which of the four ways
 * that can happen they took. Two of those ways are harmless (the guest rewrote the
 * mask it already had; the window was already on that plane) and two would strand the
 * window on the WRONG plane, which is exactly the shape a four-way collapse needs:
 * a mask change that is dropped means the next store lands in the plane the PREVIOUS
 * mask selected.
 * - SO CLOSE THE ARITHMETIC. Every map-mask write must land in exactly one bucket:
 *     mask_writes = sel_calls + mask_skip_same + mask_skip_chain4
 *     sel_calls   = swaps + sel_same + sel_zero + failed
 *   A residual in either line is a path nobody has accounted for. This is deliberately
 *   an IDENTITY rather than a rate: a rate cannot show a shape, and 8.6% has no shape.
 */
DWORD  g_ModeYSelectorCalls = 0;   /* ModeYRemapSelect() entered with the remap live */
DWORD  g_ModeYSelectorSame  = 0;   /* ...and the window was already where it wanted */
DWORD  g_ModeYSelectorZero  = 0;   /* ...and the mask selected no plane at all */
DWORD  g_ModeYTimelineIns[YTL_SECS];   /* instructions interpreted for mode Y (s80, design C) */
UINT64 g_ModeYTimelineInterpreterCycles[YTL_SECS];   /* ...and the host cycles that took */
DWORD g_ModeYFanoutBarWrites[2];      /* fan-out writes to bar bytes, by band */
DWORD g_ModeYFanoutBarDistinct[2];    /* ...distinct offsets, by band */
DWORD g_ModeYFanoutBar4Way[2];        /* ...of which the mask was all four */
DWORD g_ModeYLatchOk = 0;
DWORD g_ModeYLatchUnsolved = 0;
DWORD g_ModeYLatchDescriptor = 0;

/* --- planar mode-12h: trap direct A0000 writes through the VGA write engine -- */

INT g_A000Protection = 0;

/* ================================================================================ *
 *  run 53 (GH #2): host-interpreted protected mode -- the emulation path.           *
 *                                                                                    *
 *  Run 52 proved the kernel DEADLOCKS (not skip-resumes) on a plain-instruction PM  *
 *  #GP, so we cannot let the kernel execute risky PM code. Instead run 16-bit PM in  *
 *  the v86interp core (already proven on the mode-12h fill loops) with g_V86SegmentToLinear set *
 *  to an LDT-base resolver, so the SAME interpreter walks PM code -- descriptor      *
 *  bases instead of paragraph shifts -- and NEVER hands a faulting instruction to    *
 *  the kernel. An interpreter enforces no descriptor type, so the code-typed-SS      *
 *  write that #GP's the real CPU (run 51's I310102) simply succeeds here.            *
 *                                                                                    *
 *  istep() returns 0 on any opcode it doesn't model. The interpreter deliberately    *
 *  has no INT handler, so `CD nn` stops it cleanly -- we sync the icpu into the       *
 *  VDM_TIB, service the INT through the SAME DpmiServicePmInt() the kernel path    *
 *  uses, reload, and continue. ANY OTHER unmodeled opcode is logged with its bytes    *
 *  and stops the run -- that report is the spike's to-do signal (the next opcode to   *
 *  add). No BOP patch is needed in this mode (the interpreter reads the raw CD nn).   *
 * ================================================================================
 */
INT g_DpmiUseInterp = 0;             /* run 53 toggle (1 = interp fallback, 0 = kernel PM path).
                                                 run 59 (GH #18): 0 to exercise the real-CPU kernel path
                                                 WITH the +0x638 PM-fault trampoline. Flip to 1 to restore
                                                 the VM-confirmed interpreter runs (i310102/DPMIBACK). */
/* -- A TITLE IS "PROGRAM [ARGUMENTS]", AND WE OPENED THE WHOLE THING AS A
 * FILENAME. (session 59) `target.txt` has split `path [args]` since M2.5, but the
 * CSRSS path -- which is EVERY REAL LAUNCH, because the IFEO hook is how a program
 * reaches us on the user's machine -- never did. `ZAR.EXE -Help` therefore tried to
 * open a file literally called `C:\game\ZAR.EXE -Help`, read ZERO bytes, fell through
 * to the four-byte `mov ah,4Ch / int 21h` embedded stub and exited in 78 ms having
 * printed nothing. **No DOS program could be given an argument at all** -- not
 * `EDIT FOO.TXT`, not `DOOM -warp 1 1` -- and every one of them would have looked
 * like "the program runs and does nothing", which is the same symptom GH #131 chased
 * for a session. The tell is `loaded 0x00000000` followed by `embedded fallback`.
 * - TRY THE WHOLE STRING FIRST, THEN SPLIT LEFT TO RIGHT, AND LET THE FILE SYSTEM
 *   ARBITRATE. Opening the untouched string first is what keeps a real path CONTAINING
 *   a space working; only if that fails is a space treated as the separator, and then
 *   the FIRST split that names a file which actually EXISTS wins -- so
 *   `C:\Program Files\x\y.exe -a` finds `y.exe` and not `C:\Program`. Guessing where
 *   the arguments start is exactly the thing not to do here.
 *
 * [CAUTION]: `path` is modified in place (NUL-terminated at the end of the program name) and
 * *pargs is left pointing at the remaining arguments within it, leading whitespace
 * skipped, or NULL when there were none. Returns INVALID_HANDLE_VALUE if no split
 * names a real file, leaving `path` as it was found.
 */
static HANDLE CsrssOpenSplit(PSTR path, PSTR *argumentsOut)
{
    HANDLE fileHandle;
    INT index;

    *argumentsOut = NULL;
    fileHandle = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);

    if (fileHandle != INVALID_HANDLE_VALUE)
        return fileHandle;

    for (index = 0; path[index]; ++index)
    {
        CHAR save;

        if (path[index] != ' ' && path[index] != '\t')
            continue;

        save = path[index];
        path[index] = 0;
        fileHandle = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);

        if (fileHandle != INVALID_HANDLE_VALUE)
        {
            PSTR arguments = path + index + 1;

            while (*arguments == ' ' || *arguments == '\t')
                ++arguments;

            if (*arguments)
                *argumentsOut = arguments;

            return fileHandle;
        }

        path[index] = save;
    }

    return INVALID_HANDLE_VALUE;
}

static PSTR TaskRelaunchQueuedCommand(PSTR cursor, PSTR const base)
{
    /* -- A COMMAND ARRIVED IN THE WINDOW. The launcher queued its next DOS program
     * to THIS console's VDM (us) between the report and ExitVDM; CSRSS handed it
     * to the blocked report call. We are not going to run it in this process, and
     * CSRSS has already forgotten it: relaunch it here, in the same console, so it
     * runs in a fresh host. (Measured before this: the program silently did not
     * run and the launcher saw exit code 0.)
     */
    if (g_ReportGotNext && g_CsrssNextApp[0])
    {
        CHAR commandLine[2300];
        PSTR scan = commandLine;
        STARTUPINFOA si;
        PROCESS_INFORMATION processInfo;
        scan = LogPut(scan, "\""); scan = LogPut(scan, g_CsrssNextApp); scan = LogPut(scan, "\"");

        if (g_CsrssNextCommand[0])
        {
            scan = LogPut(scan, " ");
            scan = LogPut(scan, g_CsrssNextCommand);
        }

        *scan = 0;
        ZeroMemory(&si, sizeof si);
        si.cb = sizeof si;
        ZeroMemory(&processInfo, sizeof processInfo);
        /* [WARNING]: HAND OVER THE SINGLE-INSTANCE MUTEX FIRST. (s73) The relaunched host is a
         * second ntvdmhost, and the guard in WinMain refuses a second instance while
         * the first OWNS the mutex -- which this one did, for as long as it waited
         * on the child. Measured: `BC x.BAS` then `LINK x.OBJ` from one cmd window --
         * BC ran, LINK was queued to this VDM, the relaunched host wrote "REFUSED:
         * another ntvdmhost is LIVE" and exited, the launcher saw rc=0, and no EXE
         * was ever written. That was "QBasic cannot build EXEs" from a batch file.
         * The guest is gone (the exec loop is out), so the system-wide things the
         * guard protects are released here too; the child takes them over.
         */
        HostPanicRelease();

        if (g_OnceMutex)
        {
            ReleaseMutex(g_OnceMutex);
            CloseHandle(g_OnceMutex);
            g_OnceMutex = NULL;
        }

        /* -- AND ITS REDIRECT. (s73) The command's StdIn/Out/Err came back from the
         * report call as handles CSRSS placed in THIS process (the launcher's
         * `LINK > file`). The child's StdioInitialize takes an inherited disk/pipe
         * standard handle first, so hand them over inheritable; a console handle
         * is left alone (the child finds its console the way it always has).
         * Measured before this: the relaunched LINK's log said "stdout -> none".
         */
        {   INT item, any = 0;
        HANDLE handles[CSRSS_STANDARD_HANDLES] = { NULL, NULL, NULL };

            for (item = 0; item < CSRSS_STANDARD_HANDLES; ++item)
            {
                HANDLE handle = g_CsrssNextStandardHandles[item];
                DWORD valueType;

                if (!handle || handle == INVALID_HANDLE_VALUE)
                    continue;

                valueType = GetFileType(handle);

                if (valueType != FILE_TYPE_DISK && valueType != FILE_TYPE_PIPE)
                    continue;

                if (DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(), &handles[item],
                                    0, TRUE, DUPLICATE_SAME_ACCESS))
                    any = 1;
            }

            if (any)
            {
                /* [CAUTION]: STARTUPINFO NEVER REACHES THE CHILD: a DOS .EXE goes through
                 * BaseSrv, which creates the ntvdm (us again, via IFEO) itself.
                 * What the child DOES read is its PARENT'S PEB standard handles
                 * (StdioFromParent, GH #131) -- and its parent is this process.
                 * SetStdHandle writes exactly those PEB fields. Measured: with
                 * STARTUPINFO alone the child still said "stdout -> none".
                 */
                if (handles[CSRSS_STD_IN])
                    SetStdHandle(STD_INPUT_HANDLE,  handles[CSRSS_STD_IN]);

                if (handles[CSRSS_STD_OUT])
                    SetStdHandle(STD_OUTPUT_HANDLE, handles[CSRSS_STD_OUT]);

                if (handles[CSRSS_STD_ERR] || handles[CSRSS_STD_OUT])
                    SetStdHandle(STD_ERROR_HANDLE, handles[CSRSS_STD_ERR] ? handles[CSRSS_STD_ERR] : handles[CSRSS_STD_OUT]);

                si.dwFlags |= STARTF_USESTDHANDLES;
                si.hStdInput  = handles[CSRSS_STD_IN] ? handles[CSRSS_STD_IN] : GetStdHandle(STD_INPUT_HANDLE);
                si.hStdOutput = handles[CSRSS_STD_OUT] ? handles[CSRSS_STD_OUT] : GetStdHandle(STD_OUTPUT_HANDLE);
                si.hStdError  = handles[CSRSS_STD_ERR] ? handles[CSRSS_STD_ERR] : (handles[CSRSS_STD_OUT] ? handles[CSRSS_STD_OUT] : GetStdHandle(STD_ERROR_HANDLE));
            }

            cursor = LogPut(cursor, "STAGE2: task done -> next command's std handles in=0x"); cursor = LogHex(cursor, (DWORD)(ULONG_PTR)g_CsrssNextStandardHandles[0]);
            cursor = LogPut(cursor, " out=0x"); cursor = LogHex(cursor, (DWORD)(ULONG_PTR)g_CsrssNextStandardHandles[1]);
            cursor = LogPut(cursor, " err=0x"); cursor = LogHex(cursor, (DWORD)(ULONG_PTR)g_CsrssNextStandardHandles[2]);
            cursor = LogPut(cursor, any ? " -> handed to the child (redirect)\r\n" : " -> not a redirect, child finds its own\r\n"); }
        cursor = LogPut(cursor, "STAGE2: task done -> a command was queued to this VDM in the window: relaunching [");
        cursor = LogPut(cursor, commandLine); cursor = LogPut(cursor, "] in [");  cursor = LogPut(cursor, g_CsrssNextDirectory); cursor = LogPut(cursor, "] (single-instance mutex released)");

        if (CreateProcessA(NULL, commandLine, NULL, NULL, TRUE, 0, NULL,
                           g_CsrssNextDirectory[0] ? g_CsrssNextDirectory : NULL, &si, &processInfo))
        {
            cursor = LogPut(cursor, " -> pid 0x"); cursor = LogHex(cursor, processInfo.dwProcessId); cursor = LogPut(cursor, ", waiting\r\n");
            LogAppend(LOG_PATH, base, cursor); cursor = base;
            WaitForSingleObject(processInfo.hProcess, INFINITE);
            CloseHandle(processInfo.hProcess);
            CloseHandle(processInfo.hThread);
            cursor = LogPut(cursor, "STAGE2: task done -> relaunched command finished\r\n");
        }
        else
        {
            cursor = LogPut(cursor, " -> CreateProcess FAILED 0x");
            cursor = LogHex(cursor, GetLastError());
            cursor = LogPut(cursor, "\r\n");
        }

        LogAppend(LOG_PATH, base, cursor); cursor = base;
    }

    return cursor;
}

static PSTR TaskReportExitToCsrss(PSTR cursor, PSTR const base, DOS_MACHINE *machine)
{
    /* REPORT THE ERRORLEVEL TO CSRSS AND LEAVE THE CONSOLE. (s72) (Importance = 1):
     * Only when CSRSS queued this task to us (the harness stub / target.txt shapes
     * and a WOW launch are untouched). AFTER the flush, so the launcher -- released
     * the instant CSRSS takes the report -- finds the program's whole output.
     * - MEASURED: GetNextVDMCommand with the exit code releases the launcher and then
     *   BLOCKS, DONT_WAIT or not, until the console's next DOS command arrives: stock
     *   ntvdm stays resident per console and that wait is its idle state. We do not
     *   stay, so the report goes in a helper thread and the main thread carries on to
     *   ExitVDM, which is what releases the console's VDM record (without it the next
     *   DOS program typed into the same window was queued to a host that had gone).
     */
    if (g_Fetch2Ok)
    {
        HANDLE thread2;
        DWORD threadId = 0;
        DWORD waitResult;
        cursor = LogPut(cursor, "STAGE2: task done -> reporting exit code 0x"); cursor = LogHex(cursor, (DWORD)machine->ExitCode);
        cursor = LogPut(cursor, " to CSRSS (helper thread)...\r\n");
        LogAppend(LOG_PATH, base, cursor); cursor = base;
        thread2 = CreateThread(NULL, 0, CsrssReportThread, (LPVOID)(ULONG_PTR)machine->ExitCode, 0, &threadId);
        /* A short grace so the report's LPC is processed (launcher released) before
         * ExitVDM's is. The wait normally times out: the call is blocked for the
         * console's next command, which is stock ntvdm's idle state, not ours.
         */
        waitResult = thread2 ? WaitForSingleObject(thread2, CSRSS_REPORT_GRACE_MS) : WAIT_FAILED;

        if (thread2)
            CloseHandle(thread2);

        cursor = LogPut(cursor, "STAGE2: task done -> report thread ");
        cursor = LogPut(cursor, waitResult == WAIT_OBJECT_0 ? "returned" : waitResult == WAIT_TIMEOUT ? "blocked for the console's next command (expected)" : "could not start");
        cursor = LogPut(cursor, "; ExitVDM...\r\n");
        LogAppend(LOG_PATH, base, cursor); cursor = base;
        {   BOOL isExitOk = CsrssExitVdm();
        DWORD exitError = GetLastError();
            cursor = LogPut(cursor, "STAGE2: task done -> ExitVDM = "); cursor = LogPut(cursor, isExitOk ? "TRUE" : "FALSE");
            cursor = LogPut(cursor, " err=0x"); cursor = LogHex(cursor, exitError); cursor = LogPut(cursor, " got_next="); cursor = LogHex(cursor, (DWORD)g_ReportGotNext);
            cursor = LogPut(cursor, " next_app=["); cursor = LogPut(cursor, g_CsrssNextApp); cursor = LogPut(cursor, "]\r\n");
            LogAppend(LOG_PATH, base, cursor);
            cursor = base; }
        cursor = TaskRelaunchQueuedCommand(cursor, base);
    }

    return cursor;
}


#include "host_exec.c"

#include "host_startup.c"

INT WINAPI WinMain(
    HINSTANCE instance,
    HINSTANCE previousInstance,
    LPSTR commandLineText,
    INT showCommand)
{
    CHAR report[8192];
    PSTR cursor = report;
    PSTR base;
    PCSTR const reportEnd = report + sizeof report;   /* the guards below keep a line's worth short of it */
    INT wowCommandFromCsrss = 0;         /* s73: the Win16 program came from CSRSS, not target.txt */
    volatile BYTE *tib;
    DWORD readCount = 0;
    LONG vdmStatus;
    DOS_IMAGE image;
    DOS_MACHINE machine;
    CHAR dosOutput[16384];   /* M9 probe dumps run to several KB; 1024 truncated them */
    CHAR programPathBuffer[768];
    CHAR args[256];
    INT guard;

    g_GuestThreadId = GetCurrentThreadId();
    OsCompatBind();                    /* the four XP-only imports, or their absence */
    /* cfg\ and debug\out\ before ANYTHING logs. A missing out\ makes every LogAppend
     * fail silently, and the log is what explains every other failure. Idempotent;
     * debug\ first because CreateDirectoryA does not create intermediate levels.
     */
    CreateDirectoryA(NTVDMEX_CFG, NULL);
    CreateDirectoryA(NTVDMEX_DEBUG, NULL);
    CreateDirectoryA(NTVDMEX_OUT, NULL);
    /* s90 (#278): THE WOW32.DLL / NTVDM.EXE STAND-INS GO IN FIRST, before anything in
     * this process can touch winmm. winmm asks "am I under WOW?" ONCE and caches the
     * answer (0x76b616ec), and the host's own audio and timer code loads winmm early:
     * loaded at the WOW branch below, the shims arrived after the question had been
     * answered "no", and NotifyCallbackData kept returning 0 (runs/s90/sr4).
     */
    if (LaunchIsWow(GetCommandLineA()))
        WowShimsLoad();

    {
        INT exitCode;
        INT flow = StartupRunInstallVerb(&exitCode);

        if (flow == HOST_FLOW_RETURN)
            return exitCode;
    }

    /* NOTHING ON THE COMMAND LINE = THE USER OPENED NTVDMEX. (s79) (Importance = 5):
     * Sits here for the same reason the verbs do: above every launch guard, and safe
     * because a real VDM launch always has arguments. Before this, a double-click
     * reached STAGE1, was refused VDM privilege (`NtVdmControl` -> 0xC0000022) and
     * vanished without a window or a message -- the worst possible answer to "open it
     * and see".
     */
    if (CommandLineBare(GetCommandLineA()))
        return LaunchShellVdm();

    {
        INT exitCode;
        INT flow = StartupClaimInstance(&exitCode);

        if (flow == HOST_FLOW_RETURN)
            return exitCode;
    }
    HANDLE uiThread = NULL;

    (VOID)instance;
    (VOID)previousInstance;
    (VOID)commandLineText;
    (VOID)showCommand;
    programPathBuffer[0] = 0;
    args[0] = 0;

    /* The install verbs ran far above, ahead of the single-instance guard -- see the
     * block after the CreateDirectory calls, and the defect note there.
     */

    {
        INT exitCode;
        INT flow = StartupConfigure(&cursor, report, &exitCode);

        if (flow == HOST_FLOW_RETURN)
            return exitCode;
    }

    {
        INT exitCode;
        INT flow = StartupRegisterVdm(&cursor, &vdmStatus, &wowCommandFromCsrss, args, programPathBuffer, &readCount, report, &tib, &exitCode);

        if (flow == HOST_FLOW_RETURN)
            return exitCode;
    }

    StartupLoadProgram(&cursor, &readCount, programPathBuffer, args, wowCommandFromCsrss);
    /* No flag to raise: the UI tick polls g_ProgramName and repaints the strip when it
     * changes. See StatusUpdate.
     */

    StartupBuildDos(&cursor, readCount, &image, programPathBuffer, args, &machine, report);

    cursor = StartupAttachDevices(cursor);
    StartupLoadTuningKnobs();
    cursor = StartupStartServices(cursor);
    StartupConnectDosToHost(&machine);

    StartupStartGuest(&cursor, &base, &uiThread, &machine, &image, tib, report);

    /* Service loop: run V86 until a BOP, dispatch INT 21h, step past the BOP, re-enter.
     * Runs until the guest terminates, a hard stop, or the window closes (g_Running);
     * no iteration cap so interactive/animated programs keep going.
     */
    machine.Tib = tib;
    machine.Output = dosOutput;
    machine.OutputCapacity = sizeof(dosOutput);
    machine.OutputLength = 0;
    machine.IsOutputTruncated = 0;
    g_Machine = &machine;              /* the watchdog flushes this if the run wedges */
    (VOID)guard;
    HostRunExecLoop(&cursor, base, &machine, tib, &vdmStatus, programPathBuffer);

    /* Log the exec-loop exit before any flushing, so a hang or fault during
     * shutdown is distinguishable from the loop never exiting at all.
     */
    cursor = LogPut(cursor, "STAGE2: exec loop exited -> flushing\r\n");
    LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;

    /* The exec loop is out: tell the headless backstop a clean shutdown is under
     * way so it does not force-exit us mid-flush and lose the DOS output.
     */
    InterlockedExchange(&g_WoundDown, 1);

    g_CommandInfo.ExitCode = (ULONG)machine.ExitCode;
    /* Flush captured DOS output to the console + the log.
     * - IF WE STREAMED IT LIVE, DO NOT PRINT IT AGAIN. g_Stdio carries the output
     *   as the guest produces it now (GH #131), so the historical bulk write to
     *   CONOUT$ would DOUBLE every line -- and would do it into the redirect
     *   target, where it is not merely ugly but wrong. Flush whatever is still in
     *   the line buffer instead. The log copy below is unconditional either way:
     *   it is a different sink and the one the rig harness reads.
     */
    StdioFlush();
    cursor = TaskReportExitToCsrss(cursor, base, &machine);
    cursor = ReportStartMode(cursor);
    PcSpeakerClose(&g_PcSpeaker);               /* [CAUTION] a headless run never sees WM_DESTROY,
                                            and Beep.sys outlives the process */
    RecoveryOk();                       /* GH #132: this run ended cleanly */
    cursor = ReportStdoutAndDosOutput(cursor, &machine);
    cursor = ReportEndOfRun(cursor, base, reportEnd, &machine, tib);

    /* Headless test mode: the guest has just terminated (or the PM loop hit its
     * headless time cap), so exit immediately (no window-close needed) -- this lets a
     * test harness's `start /wait` return and the log be collected. Consume the marker
     * so it's one-shot; interactive runs (no marker) keep the window open.
     */
    if (g_Headless)
    {
        DeleteFileA(AUTOEXIT_PATH);
        TrayRemove(g_Window);     /* ExitProcess runs no window cleanup -- see below */
        HostRecordFinish();
        ExitProcess(0);
    }

    /* A WIN16 HOST MUST NOT OUTLIVE ITS GUEST. (session 56) (Importance = 4):
     * REPORTED BY THE USER: "when a WoW16 window exits, it leaves its tray icon
     * behind. They are stacking up in the tray."
     *
     * [CAUTION]: I FIRST WROTE THAT THESE WERE NOT GHOSTS BUT LIVE PROCESSES. THE USER
     * REFUTED IT WITH ONE OBSERVATION: "they all disappear when the mouse
     * hovers over them", which is the textbook signature of a GHOST -- Explorer
     * reaps a tray icon only after its owner is dead, and only lazily, when the
     * mouse passes over it. A live process's icon does not do that.
     * Measured afterwards, and it is not close: FIVE icons in the tray with
     * `tasklist` reporting ZERO ntvdmhost.exe. They are ghosts.
     *
     * [INFO]: BOTH FACTS ARE REAL AND THEY COMPOSE. The lingering host is measured too
     * (launch CALC, click its X, the window is gone and PID 1488 is still
     * there) -- that is the bug this arm and wowuser.h's WM_DESTROY fix. But a
     * LINGERING host is what the NEXT `taskkill /f /im ntvdmhost.exe` then
     * kills, and every launch script runs one, against ALL instances. An
     * externally terminated process cannot run NIM_DELETE, so each one becomes
     * a ghost. Demonstrated end to end: 1 host + 6 icons -> taskkill -> 0 hosts
     * and still 6 icons.
     *
     * So the icons were ghosts, the lingering hosts were what got ghosted, and
     * the fix for both is the same: a host that exits WITH its guest never
     * needs killing.
     *
     * [CAUTION]: The cause is the line below this one, and it was right for the case it
     * was written for and wrong for this one. "Keep the window open so the
     * guest's final screen stays visible until the user closes it" assumes
     * there IS a window and a final screen. A Win16 guest has NEITHER -- the
     * note by TrayAdd says so in as many words: it gets no VDM window, only a
     * tray icon. So there was nothing to look at and nothing to close, and the
     * wait never ended. The user could only reach it through the tray menu's
     * Exit, which is exactly the step they were never going to take twenty
     * times a session.
     *
     * For a Win16 host the guest exiting IS the end of the run. Ask the UI
     * thread to close, and let it leave through its OWN path -- WM_CLOSE ->
     * DestroyWindow -> WM_DESTROY -> PostQuitMessage -> the message loop
     * returns -> TrayRemove + PresentDdrawShutdown. That is strictly better
     * than ExitProcess here, because WM_DESTROY is also what stops the OPL and
     * Beep.sys, and both of those have outlived a host before.
     */
    /* AUTO-CLOSE ON GUEST TERMINATION, FOR EVERY GUEST. (s63, user ask) (Importance = 1):
     * This was WOW-only; a DOS guest that exited left its window open showing a
     * dead final frame until the user closed it by hand. But reaching here means
     * the guest we were asked to run has TERMINATED -- for a game, quitting it
     * exits the program and lands exactly here -- so the run is over and there is
     * nothing to interact with. Closing through WM_CLOSE -> WM_DESTROY runs
     * HostPanicRelease(), which is also THE FIX FOR THE CAPTURE-EXIT TRAP: a
     * guest that exited while it held the mouse used to leave the pointer clipped
     * to a dead window with no way to escape but the keyboard chord. Same path for
     * DOS and Win16 now; the s63 UiThread TerminateProcess then guarantees the
     * process is gone.
     */
    if (g_Window)
        PostMessageA(g_Window, WM_CLOSE, 0, 0);

    if (uiThread)
    {
        WaitForSingleObject(uiThread, INFINITE);
        CloseHandle(uiThread);
    }

    return 0;
}
