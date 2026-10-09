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
#include "host_core.h"     /* the folder and its paths, the host lock, guest-memory access */
#include "v86.h"
#include "dpmi.h"
#include "csrss.h"
#include "log.h"
#include "settings.h"   /* registry-backed knobs + the Settings dialog */
#include "mgrproto.h"   /* GH #281: the NTVDMEX manager -- one tray icon for every program */
#include "pcspeaker.h"  /* the OTHER speaker: the one on the motherboard */
#include "install.h"    /* GH #13: becoming the machine's VDM, reversibly */
#include "x86len.h"     /* which `CD nn` byte pairs are really INT instructions */
#include "dpmi_rmcs.h"  /* GH #247: the real-mode call structure, and 0300h's routing */
#include "dpmi_svc.h"   /* GH #248: INT 31h's spec-decided answers (selectors, callbacks, 0503h) */
#include "i33_driver.h" /* GH #264/#265: INT 33h cursor masks, profiles, alternate handlers */
#include "pif.h"        /* a .PIF's program, directory and parameters */
#include "../wow/ne.h"  /* GH #128: 16-bit New Executable loader (WOW bootstrap) */
#include "../wow/wow32.h" /* GH #128: the 32-bit half -- krnl386's calls out to Win32 */
#include "../wow/wow32.c"
#include "../wow/wowanchors.h" /* GH #128: ...and how a thunk module's segment is RECOGNISED */
#include "../wow/wowanchors.c"
#include "../wow/wowsched.h" /* GH #128: ...and the Win16 task scheduler, which is also ours */
#include "../wow/wowcall.h" /* GH #128: ...and the OTHER direction -- calling 16-bit code */
#include "../wow/wowcall.c"
#include "../wow/wowmsg.h" /* GH #128: ...and the MESSAGE QUEUE the loop turns on */
#include "../wow/wowmsg.c"
#include "../wow/wowres.h" /* GH #128: ...and the guest's OWN menu and icons */
#include "../wow/wowres.c"
#include "../wow/wowwin.h" /* GH #128: ...and a Win16 window IS a real Win32 window */
#include "../wow/wowwin.c"
/* ⚠ wowgdi.h COMES BEFORE wowuser.h, and the order is load-bearing: USER's
     GetDC/GetWindowDC issue a GDI token, so the object map has to be in scope by
     the time USER's dispatcher is compiled. It used to be last only because it
     borrowed USER's note helpers, and those now live in wow32.h. */
#include "../wow/wowgdi.h" /* GH #128: GDI.EXE's id space -- where MS Paint begins */
#include "../wow/wowgdi.c"
#include "../wow/wowuser.h" /* GH #128: USER.EXE's id space -- a DIFFERENT one; see the file */
/* ⚠ AFTER wowuser.h, and that order is load-bearing too: the modal loop reads
     the window table and the procedure rule that file owns. USER's DialogBox and
     EndDialog arms reach it through the three prototypes declared there. */
#include "../wow/wowdlg.h" /* GH #128: ...and the MODAL loop -- why DialogBox does not return */
#include "../wow/wowenum.h" /* GH #128: ...and one callback per item -- EnumWindows, LineDDA */
#include "../wow/wowshell.h" /* GH #128: ...and SHELL.DLL's, which is a THIRD one again */
#include "../wow/wowcommdlg.h" /* GH #128: ...and COMMDLG.DLL's -- File > Open */
#include "../wow/wowkbd.h" /* GH #128: ...and KEYBOARD.DRV's -- ANSI/OEM conversion */
#include "../wow/wowsound.h" /* GH #299: ...and SOUND.DRV's -- stock answers 0 */
#include "../wow/wowmmedia.h" /* GH #278: ...and MMSYSTEM's two -- mmCallProc32 */
#include "dos_mcb.h"
#include "bios_bda.h"       /* GH #253: 0040:000E/0010/0013 and the EBDA, from one source */
#include "dos_loader.h"
#include "dos_psp.h"
#include "dos_env.h"
#include "dos_int21.h"
#include "dos_auxprn.h"   /* #251: AUX/PRN driver code planted at DOS_CTAB_SEG */
typedef CHAR DOS_AUXPRN_FITS[(sizeof(g_DosAuxPrnCode) <= DOS_AUXPRN_LEN) ? 1 : -1];
#include "bios_kbdact.h"  /* #254: INT 09h side-calls planted at DOS_CTAB_SEG */
#include "bios_prtsc.h"   /* #274: the default INT 05h's byte sequencer */
typedef CHAR BIOS_KBDACT_FITS[(sizeof(g_BiosKeyboardActionCode) <= DOS_KBDACT_LEN
                               && DOS_AUXPRN_OFF + DOS_AUXPRN_LEN <= DOS_KBDACT_OFF
                               && DOS_KBDACT_OFF + DOS_KBDACT_LEN <= DOS_GENSTUB_OFF
                               && DOS_GENSTUB_OFF + DOS_GENSTUB_N * DOS_GENSTUB_SIZE <= DOS_CTAB_END) ? 1 : -1];
#include "dos_layout.h"
#include "dos_disk.h"       /* GH #44: image geometry + CHS<->LBA */
#include <tlhelp32.h>
#include "dos_recovery.h"   /* GH #132: what to do when we will not start */
#include "dos_err.h"       /* #34: the INT 24h contract (dos_crit_*) */
#include "dos_sysvars.h"   /* GH #48: the List of Lists, built to the measured 6.22 layout */
#include "dos_ctab.h"
#include "dos_xms.h"
#include "dos_extmem.h"      /* GH #54: INT 15h AH=87h address resolution */
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
#include "vdd_emu8k.h"   /* #233: the AWE32's wavetable chip */
#include "vdd_mpu.h"
#include "vdd_comm.h"
#include "vdd_net.h"     /* GH #8 (s91): NetBIOS via INT 5Ch */
#include <nb30.h>
#include "../../sdk/include/ntvdmex-vdd.h"
#include "vdd_audio.h"
#include "audio_wave.h"
#include "midi_route.h"       /* #136: Settings > Audio > MIDI -> a host device, by name */
#include "present_ddraw.h"

#include "host_internal.h"
#include "host_diag.c"
#include "host_dpmi.c"
#include "host_dpmi_int.c"
#include "host_wow.c"
#include "host_window.c"


/* LOG_PATH now lives in log.h -- see the note there. */
/* Both are written by the runner and READ by us, so they are cfg, not out. */
#define TARGET_PATH CFG_("target.txt")
/* A DOS shell to run when NOTHING named a program. NOT a target: target.txt names THE
   test and is consulted first; this is the last resort. See the STAGE2 block. */
#define SHELL_PATH  CFG_("shell.txt")
/* #208: present = load a program started from Windows directly (the pre-#208 way)
   instead of handing it to XP's COMMAND.COM. An A/B switch, not a setting. */
#define DIRECTLAUNCH_FLAG CFG_("directlaunch.flag")
#define AUTOEXIT_PATH CFG_("autoexit")   /* marker: headless test mode -> exit when the guest exits */
/* Opt-in screenshot flag. Lives on the SMB SHARE folder so the remote driver can
   toggle it (create it before a GRAPHICAL test, remove it otherwise). Non-graphical
   tests (selftest/dpmitest) then never touch the self-capture path -- keeping the
   common case off the capture code entirely. */
#define CAPTURE_FLAG CFG_("capture.flag")
/* Mode-Y de-interleave tuning; see modey_flush() in vdd_video.c. Contents = the run
   coalescing slack in dwords. Absent = the built-in default. */
#define MODEY_PATH CFG_("modey.txt")
/* A hex VRAM byte offset. Every planar write to it is recorded with the registers
   that produced it and the guest CS:IP -- see the watchpoint in vdd_video.c. */
#define VWATCH_PATH CFG_("vwatch.txt")
/* Per-plane backing for mode Y is ON by default -- see the MODE-Y PLANE BACKING block.
   This file DISABLES it and falls back to the de-interleave heuristic, which is worth
   keeping only because it is what a machine that refuses the remap will use. */
#define SBDUMP_FLAG  CFG_("sbdump.flag")
/* North star 2: present = no Gravis UltraSound (no device, no ULTRASND= in the env). */
#define NOGUS_FLAG   CFG_(KNOB_FILE_NOGUS)
#define SBDUMP_PATH  OUT_("sb.raw")
/* s81: record the audio output (audio_rec.h via AudioWaveRecord*). The flag is the harness's
   switch; Tools > Capture > Record Audio will drive the same recorder. */
#define WAVREC_FLAG  CFG_("wavrec.flag")
#define WAVREC_PATH  OUT_("capture_audio.wav")
#define NOREMAP_FLAG CFG_("noremap.flag")
/* Diagnostic knob: disable the mode-12h A0000 NOACCESS trap. With it off, planar
   writes land in the raw aperture instead of the VGA engine, so the PICTURE will
   be wrong -- the question it answers is whether the guest EXECUTES AT ALL.
   Absent = normal behaviour, so ordinary runs are untouched. Delete after use. */
#define NOA000_FLAG  CFG_("noa000.flag")
/* ── TURN THE PM INT-SITE PATCHER OFF. (s74, diagnostic) ─────────────────────────
   The patcher rewrites `CD nn` -> `C4 C4` in a client's declared code region so a
   protected-mode INT becomes a BOP we can service; a raw `CD nn` in PM is the one
   fault XP will not reflect. But the region is whatever the CLIENT calls code, and
   in a guest that GENERATES tables or code at runtime a stray 0xCD is just data --
   which is how this has now broken guests five times (Doom's jump table four, then
   heaven7). Present = scan nothing, so a silent death can be A/B'd against the
   patcher in one run instead of being argued about. Absent = normal behaviour.
   ⚠ It is a DIAGNOSTIC, not a fix: with it on, a guest that really does execute
     `CD nn` in PM dies differently. Judge it on whether the guest gets FURTHER. */
#define NOPMPATCH_FLAG CFG_("nopmpatch.flag")
/* ── DUMP A GUEST LINEAR RANGE AT THE HEADLESS DEADLINE. (s74b, diagnostic) ──────
   Contents: two hex numbers, "<linear> <size>". Written to debug\out\memdump.bin while
   the guest is still mapped -- the way to READ A PACKED GUEST (heaven7 unpacks itself
   into its 0501 block, so its strings and tables exist only in memory). Absent = off. */
#define MEMDUMP_FLAG   CFG_("memdump.flag")
/* Optional CONTENTS of nopmpatch.flag: a hex byte count. Regions at least that big
   are not scanned; smaller ones are patched as usual. Empty = skip every region.
   Why a SIZE: DOS/4GW's own PM code region is ~0x5000 bytes of dense, real
   `mov ah,N / int 21h`, and it NEEDS patching (a raw CD in PM is the one fault XP
   will not reflect). heaven7's is the whole 0x3a000 LE allocation -- code object
   AND data object AND everything it generates into them -- and needs not to be.
   Size separates the two without naming an address, which changes run to run. */
/* Mode 12h WITHOUT the A0000 page trap (GH #55). Arming that trap stops the V86
   guest running at all -- 10 I/O events in 30s against 22.5 MILLION with it off.
   But we do not actually need it: in mode 12h QuickBASIC reprograms a VGA
   register via OUT between pixels, so the PORT traps alone hand us control
   constantly, and the batching interpreter can then run the pixel loop with its
   A0000 stores going through the planar engine. This knob keys the interpreter
   off "planar mode is active" instead of "the page is protected". */
#define INTERP12_FLAG CFG_("interp12.flag")
/* Escape hatch for the planar policy (GH #55): present = go back to the A0000
   page trap. Interpreting the guest for the whole time a planar mode is set is
   the DEFAULT because the page trap demonstrably freezes the guest on real
   hardware; this knob exists so the old path is still one file away. */
#define P12OFF_FLAG   CFG_("p12off.flag")
/* North star 1 (s80): present = do NOT interpret mode-Y multi-plane / latch windows,
   i.e. go back to the scratch + fan-out approximation. The A/B and rollback lever. */
#define MYINTERP_OFF_FLAG CFG_("modeyinterp_off.flag")
/* Present = record the last 64 mode-Y interpreted instructions (s80's crash finder). */
#define MYRING_FLAG CFG_("myring.flag")
static BYTE g_TrampolineSave[DOS_HDLR_TRAMPOLINE_SIZE];
static INT  g_TrampolineSaved;
/* North star 1 for PROTECTED-mode guests (Doom): present = do not interpret its drawers. */
#define MYPM_OFF_FLAG    CFG_("modeypm_off.flag")
/* Present = keep the scratch window while a PM guest runs NATIVELY under a multi-plane
   mask, so any store its own code makes there is counted (fanN) instead of assumed away. */
#define MYPM_DETECT_FLAG CFG_("modeypm_detect.flag")
/* GH #128: opt into the EXPERIMENTAL WOW load probe on a Win16 launch. Absent (the
   default) the host still refuses Win16 loudly -- an experiment must never become the
   shipped behaviour by accident. */
#define WOWTRY_FLAG   CFG_("wowtry.flag")
/* Dev-only: capture the exact OPL register stream a game sends, with timestamps,
   so it can be replayed offline through BOTH our synth and a reference core and
   the audio diffed. Counting register writes cannot say WHY an instrument sounds
   wrong; comparing waveforms from identical input can. Absent = no cost at all. */
#define OPLTRACE_FLAG CFG_("opltrace.flag")
#define QIMODE_PATH CFG_("qimode.txt")
/* FIXED_NTVDMSTATE ([0x714]) initial value override, hex, up to 8 digits. Absent = 0.
   Exists so the rig can try a different starting word without a rebuild -- see the
   note at the VdmRegisterWithKernel call for why the word must be INITIALISED at all. */
#define VDMSTATE_PATH CFG_("vdmstate.txt")
/* Headless wall-clock cap override, decimal milliseconds, also on the share. The 30 s
   default is right for an unattended test that must not wedge the watcher, but an
   INTERACTIVE test on the box -- keylog, where a human walks over and presses every key
   -- needs minutes, and the default would kill the guest mid-typing. Absent = the
   default. */
#define HEADLESS_MS_PATH CFG_("headless_ms.txt")
#define AWBUFS_PATH      CFG_("awbufs.txt")
#define AWFRAMES_PATH    CFG_("awframes.txt")
#define EXECPRIO_PATH    CFG_("execprio.txt")
#define DSPVER_PATH      CFG_("dspver.txt")
#define SBGATE_PATH      CFG_("sbgate.txt")
#define PITPACE_PATH     CFG_(KNOB_FILE_PITPACE)
#define PITPRIO_PATH     CFG_("pitprio.txt")
#define PITINJ_PATH      CFG_("pitinj.txt")
#define UITICK_PATH      CFG_(KNOB_FILE_UITICK)
/* courier.txt = 0 turns the tick courier off (see TickCourierThread). 1 = as shipped. */
#define COURIER_PATH     CFG_("courier.txt")
/* llkbd.txt = 1 re-enables the SYSTEM-WIDE low-level keyboard hook. OFF by default --
   see InputCaptureSet for why it is the single most dangerous thing this host does. */
#define LLKBD_PATH       CFG_("llkbd.txt")
/* ── ★ HOW LONG A BLOCKED Win16 TASK WAITS. (GH #128, session 43) ────────────────
     Milliseconds, decimal; **0 means FOREVER**, which is what a real Win16 task
     does and what an INTERACTIVE session needs -- a program sitting in GetMessage
     with its window on the desktop is not stuck, it is waiting for the user, and
     a host that quits it after six seconds makes it impossible to type into.
     Absent = WOWMSG_WAIT_MS, the bound a harness run needs so that a guest which
     will never receive anything still lets the run finish. */
#define WOWIDLE_PATH     CFG_("wowidle.txt")
#define KEYIRQ_PATH      CFG_("keyirq.txt")
#define MSENS_PATH       CFG_(KNOB_FILE_MSENS)
/* ── ★ THE CPU-SPEED CALIBRATION, AS A FILE. (GH #56) ────────────────────────────
     Decimal MHz: how fast an UNTHROTTLED host looks to a DOS program on THIS box.
     Every speed on the menu is a fraction of it, so it is the one number that makes
     "33 MHz" mean 33 MHz here, and it is wrong on somebody else's machine by
     construction -- a faster box presents more. Measured by cpubench.asm; absent =
     CPUSPEED_REF_MHZ_DEFAULT. A knob, so re-calibrating is a run rather than a build. */
#define CPUREF_PATH      CFG_("cpuref.txt")
/* Decimal index into g_CpuSpeedMhz, overriding the registry for one run. This is how
   the rig sweeps every speed in a single batch without touching HKCU. */
#define CPUSPD_PATH      CFG_(KNOB_FILE_CPUSPD)
/* Throttle granularity (target run/hold period, ms). 0/absent = auto-detect. */
#define CPUGRAN_PATH     CFG_("cpugran.txt")
/* cpuaff.txt = 1 -> pin the guest to a core of its own (see CpuAffinityApply). */
#define CPUAFF_PATH      CFG_("cpuaff.txt")
#define DOSVER_PATH      CFG_(KNOB_FILE_DOSVER)
/* INT 21h AH=53h's private AL sub-functions. A knob because the measured values are
   measured IN A CONTEXT (a probe whose stdout was redirected) and at least AL=5 is
   suspected of depending on it -- see dos_int21.c. One row per line:
       <AL hex> <AX hex> <CF 0|1>        e.g.  05 5300 0
   `;` or `#` starts a comment; absent rows keep the built-in measured default. */
#define INT53_PATH       CFG_("int53.txt")
/* Extra guest environment variables, one NAME=VALUE per line; '#' comments a line.
   See DosEnvBuildWithCard for why this exists -- a DOS program configured through its
   environment could not be configured at all before it. */
#define DOSENV_PATH      CFG_("dosenv.txt")
static WORD g_DsProbe[DSPROBE_MAX];
static INT  g_DsProbeCount = 0;
static WORD g_CsProbe[DSPROBE_MAX];
static INT  g_CsProbeCount = 0;

#define DOSTRACE_FLAG    CFG_("dostrace.flag")
/* The XMS pool, in KB. Named because SysVars+0x45 must report the SAME
   number to MEM.EXE (GH #47) -- two literals would drift. */
/* ── #48: THE POOL IS THE MACHINE'S EXTENDED MEMORY LESS THE HMA, AS HIMEM'S IS. ──────
     This was 16384 on a machine whose INT 15h AH=88h, CMOS 17h/18h and 30h/31h and
     AH=87h address space (dos_extmem.h, 1..16 MB) all say 15360 KB of extended memory:
     XMS handed out more memory than the machine has, and SysVars+0x45 reported the
     pool, so MEM and AH=88h disagreed (#48). HIMEM on 6.22 reports the extended memory
     minus the 64 KB HMA it keeps for itself -- the oracle's MEM: total 15,232K, XMS free
     15,168K (runs/s81_mem/oracle_memd.txt). Derived from CMOS_EXTENDED_KB so the four views
     (88h, CMOS, SysVars+0x45, XMS) are one number and cannot drift again.
   ⚠ AN OBSERVABLE CHANGE: XMS AH=08h now says 15296 KB, 1088 KB less than before. No
     oracle pins the old figure (xms-ems.md: 08h's size is per machine, abstained), and
     DPMI memory does not come from this pool -- but every XMS client sees it. */
#define XMS_HMA_KB    64
#define XMS_POOL_KB   (CMOS_EXTENDED_KB - XMS_HMA_KB)       /* 15296 */

#define DPMI_BOP       0x50
/* Set per BOP in the exec loop: did this `C4 C4 nn` execute in GUEST code rather than at
   one of the addresses we plant ours at? See the note where it is assigned. */
static INT g_BopFromGuest = 0;
static DWORD g_NtvdmBopCount = 0;   /* how many guest-issued NTVDM BOPs this run serviced */
/* s79: set at load time, from the IMAGE, not the path -- see the scan in STAGE2.
   g_GuestNtAware means "we loaded this as a shell AND it talks to NTVDM", which is
   what earns it DOS 5.00 and the private AH=53h answers. */
static DWORD g_GuestNtvdmBops = 0;
static INT   g_GuestNtAware    = 0;
static INT   g_ShellGetNextCount  = 0;
static CHAR  g_ShellPath[300];         /* the shell we loaded, for its COMSPEC (s81) */   /* BOP 54 sub 01 calls this session -- see its arm (s81) */
static CHAR  g_FirstProgram[300];         /* its 8.3 path -- the sub 01 NAME field        */
static CHAR  g_FirstTail[128];         /* its arguments -- the sub 01 command TAIL     */
#define NTVDM_BOP_DOS  0x50   /* XP's COMMAND.COM: 1 site, in its version-refusal path  */
/* How to answer an NTVDM BOP we have not implemented yet: contents of cfg\bop54.txt.
   A knob because the right answer is UNKNOWN and is being measured -- see the handler. */
#define BOP54_PATH     CFG_("bop54.txt")
/* A command line to hand the shell ONCE through BOP 0x54 sub 01, so the path can be
   tested end-to-end rather than only 'it accepted an empty answer'. */
#define BOPCMD_PATH    CFG_("bopcmd.txt")
/* The startup batch file BOP 0x54 sub 0x0D hands the shell. Stock NTVDM names
   AUTOEXEC.NT here; ours defaults to the DOS-native AUTOEXEC.BAT. See the handler. */
#define BOPAUTO_PATH   CFG_("autoexec.txt")
#define DPMI_PMRET_BOP   0x56
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
#define DPMI_RAW2RM_BOP  0x59        /* protected -> real (entered in PM)           */
#define DPMI_FLT_CLASS_GP  6         /* the kernel's fault class for a #GP (observed: 6)    */
/* EMS (M4): the LIM page frame is a 64KB RAM window in the UMA. VdmMapEmsFrame
   scans the conventional page-frame segments AFTER VdmInitialize for a free 64KB
   hole and maps it there; g_EmsFrameLinear holds the linear base actually chosen
   (0 => none found, EMS unavailable). The guest learns the segment via AH=41. */
#define EMS_POOL_PAGES 512                           /* 512 * 16KB = 8MB of EMS */
static DWORD g_EmsFrameLinear;                        /* set by VdmMapEmsFrame */

/* CSRSS receive buffers + program image (no CRT heap; static = zero-init). */
static CHAR g_CommandLine[1024], g_Application[1024], g_CurrentDirectory[512], g_PifPath[512];
static WORD g_CdsSegment;                       /* the CDS array's reserved block, 0 = none */
static WORD g_SftSegment;                       /* the SFT block for a DOS guest, 0 = none  */

static CHAR g_Environment[8192], g_Desktop[512], g_Title[512], g_Reserved[512];
VDM_COMMAND_INFO g_CommandInfo;
/* The SECOND fetch (s72): what CSRSS actually queued -- AppName is the program's
   full path, CmdLine its argument tail (CR LF terminated), Env the launcher's Win32
   environment block. See csrss_fetch_command(). */
static CHAR g_Application2[1024], g_CommandLine2[1024], g_CurrentDirectory2[512], g_Environment2[8192];
static INT  g_Fetch2Ok = 0;

static volatile LONG g_ReportGotNext = 0;   /* the report call returned TRUE: a command was queued to us */
static DWORD WINAPI CsrssReportThread(LPVOID parameter)
{
    DWORD exitCode = 0;
    BOOL isOk = CsrssTaskDone(g_CommandInfo.TaskId, (ULONG)(ULONG_PTR)parameter, &exitCode, NULL);
    if (isOk) InterlockedExchange(&g_ReportGotNext, 1);
    return exitCode;
}
static BYTE g_FileBuffer[0x80000];   /* 512KB: hold a real game's MZ image (DOS/4GW stub etc.), run 85 */

static FDC_STATE    g_Fdc;       static NTVDD_DEVICE g_FdcDevice;
static IDE_STATE    g_Ide;       static NTVDD_DEVICE g_IdeDevice;
static DMA_STATE    g_Dma;       static NTVDD_DEVICE g_DmaDevice;
static BYTE      g_GusDram[GUS_DRAM_SIZE];
INT          g_GusOn = 0;
static WORD     g_Emu8KDram[EMU8K_DRAM_WORDS];
PCSTR g_DosVersionWhy = 0;
static INT          g_DosVersionShell = 0;      /* #208: an XP shell is present, told 5.00 itself */
UINT32 g_PitAsyncAttempts;
static DWORD g_KeyPmLogged  = 0;           /* bounded KEYPM account; see the PM exec loop */
static INT g_PmIrq0Latch = 0;             /* #2b: a virtual IRQ0 awaiting injection into the PM hook */
/* Suppress the asynchronous IRQ0 -> PM INT 08h injection. A knob, not a feature: when a
   client dies the instant we deliver a timer tick, the first question is whether the
   DELIVERY is wrong or merely BADLY TIMED, and the cheapest way to ask it is to stop
   delivering and see how much further the client gets. Absent file = normal behaviour. */
#define PMNOIRQ_PATH CFG_("pmnoirq.flag")
#define PMVEHPASS_PATH CFG_("pmvehpass.flag")
#define NOSB_PATH      CFG_("nosb.flag")
/* ── "THERE IS NO MOUSE ON THIS MACHINE". ───────────────────────────────────────
     INT 33h AX=0000h answers AX=0 and every other function is left alone, which is
     what a DOS program sees with no driver loaded. It exists to take the mouse --
     and with it Doom's twice-a-frame DPMI real-mode simulation -- OUT of a run, so
     "is that path involved in this crash at all" becomes one measurement instead of
     a series of guesses. A configuration, not a debug hack: a machine without a
     mouse is a machine a guest has to cope with. */
#define NOMOUSE_PATH   CFG_("nomouse.flag")
#define TEXTDUMP_PATH  CFG_("textdump.flag")   /* shotNN.txt beside shotNN.bmp */
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
#define PMWATCH_PATH CFG_("pmwatch.txt")
/* ── ★ WHO WROTE THIS BYTE? (GH #128, session 37) ─────────────────────────────
     pmbp.txt answers "what is there when I stop here", which needs you to know
     where to stop. The expensive question is the other one, and it has no
     instrument: krnl386's per-drive flag table at DGROUP 0x2a2 is READ from two
     places in its own code and WRITTEN from none, and it is wrong anyway.
     One line: `<hex offset> [segment]`, segment defaulting to 4 (DGROUP), because
     the addresses worth watching are data whose base moves every run. Sampled at
     every PM event, so it cannot name the instruction -- it brackets the write
     between two events that name themselves, which is a bisect's first step for
     one run instead of five. Absent file = no watch and no cost. */
#define PMCHG_PATH   CFG_("pmchg.txt")
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
#define PMKERNEL_PATH CFG_("pmkernel.flag")
/* Per-event checkpoint verbosity. The full dump -- registers, stack, frame, entry code
   -- was built for the era when the client died inside the FIRST DpmiEnterProtectedMode and the
   only question was "did we get there at all". Now that a client runs for thousands of
   events it is the thing stopping it: a Doom run hit the 4 MB log cap at event 0xdb1
   with the game still loading. So: a handful of checkpoints always (they still catch a
   death at the switch), and the full firehose only when asked for. */
#define PMVERBOSE_PATH CFG_("pmverbose.flag")
/* ── ★ WHAT IS THE GUEST DOING DURING A LONG TIMER GAP? (Skyroads wobble, s61) ────
     The gaps that wobble the game are the long ones, and the question is whether
     they are the guest's music ISR overrunning on our slow (trapped) OPL port I/O,
     or something else (a compute stretch, our own delivery latency). So split the
     port-I/O count by gap size. `dio` is the port events serviced since the last
     delivery -- g_EventIo is the running total. Also keep the single worst gap's I/O
     and where the guest was when we finally delivered it.

   ⚠⚠ TWO DEFECTS IN THE FIRST CUT OF THIS, AND THE SECOND ONE PRODUCED A REFUTATION
      THAT DOES NOT HOLD. Both are measured, on the s61 in-game run:

      1. "LONG" WAS A FIXED 8 ms, AND THE GUEST CHANGES THE TICK RATE UNDER IT.
         Skyroads' intro runs at the BIOS 18.2 Hz -- a 55 ms period -- and only
         switches to 180 Hz (5.56 ms) in game. So every NORMAL intro tick scored as
         a "big gap": 187 of the 319 were the intro ticking correctly. The threshold
         has to be RELATIVE TO THE PERIOD THE GUEST PROGRAMMED, or it measures the
         guest's own choice of rate and calls it a fault.
      2. I/O PER *GAP* IS CONFOUNDED BY GAP LENGTH. A gap ten times longer collects
         ten times the I/O whatever the guest is doing, so "big gaps carry more I/O"
         is true by construction and says nothing about the music. It has to be a
         RATE -- I/O per millisecond -- before the two populations are comparable.
         The s60 note recorded the opposite sign of this ratio and read it as
         REFUTING the music-overrun hypothesis; on this run the raw ratio comes out
         25x the other way. Neither number means anything until it is a rate.
      ⇒ Both are the [[instrument-must-not-infer-its-own-frame]] shape: an instrument
        that quietly assumes a constant the system is free to change. */
DWORD          g_Irq0IoPrevious, g_Irq0WorstGapMs, g_Irq0WorstGapIo;
DWORD          g_Irq0WorstCs, g_Irq0WorstIp;    /* where IF re-opened     */
DWORD          g_Irq0NormalCount, g_Irq0NormalMicroseconds, g_Irq0NormalIo;
DWORD          g_Irq0WorstRaise, g_Irq0WorstAttempts, g_Irq0WorstNie, g_Irq0WorstYield;
DWORD          g_Irq0WorstPerMicroseconds;   /* the period in force at the worst gap  */
/* V86 single-run stretch durations (Skyroads wobble, s61). See the exec loop. */
static DWORD          g_V86StringHistogram[8], g_V86StringCount8;
static DWORD          g_V86StringMaximumMs, g_V86StringMaximumCs, g_V86StringMaximumIp, g_V86StringMaximumEvent;
DWORD          g_HeartbeatDs = 0;            /* guest DS sampled by the heartbeat (s69 fade dump) */
DWORD          g_EventIoString      = 0;  /* REP INS/OUTS (event 1) reflects serviced */
static HANDLE         g_OnceMutex    = NULL; /* the single-instance mutex (WinMain); handed
                                                 over before a relaunch, see task-done */
/* ── ⚠ AN INTERMITTENT I/O STORM, NOT YET EXPLAINED (session 53). ────────────
     Some runs of tests/probes/dos/spktest.com report 1.87 MILLION serviced I/O
     events for a program that issues EIGHTY-SIX, take 8.7 s where a clean run
     takes 4.6, and log the SAME SIX guest CS:IP sites either way -- so the same
     handful of instructions are being serviced ~21,700 times each while the
     guest still completes correctly. Four consecutive runs on the build that
     followed would not reproduce it, so it is not the binary and not the
     settings; it is a race with something.
   ▶ THESE THREE COUNTERS ARE THE DIAGNOSIS, PRE-PLACED. The two service paths
     differ in exactly the way that matters: HostTryIo() ADVANCES EIP past the
     instruction, HostTryIoRetro() deliberately DOES NOT (the real-hardware
     event reports CS:IP already past the I/O). A storm that is all `retro` is a
     guest that is not being stepped; a storm that is all `direct` is the kernel
     re-reporting an event we already retired. The event histogram says which
     kernel event is arriving. One dirty run now answers the question. */
static DWORD g_IoViaDirect = 0, g_IoViaRetro = 0;
static LONGLONG g_HostTimeLast;           /* QPC of the last VdmRunGuest return; 0 = none */
static INT  g_HostEventLast;
INT            g_NoA000       = 0;  /* NOA000_FLAG present: leave A0000 mapped (diagnostic) */
static INT            g_NoPmPatch    = 0;  /* NOPMPATCH_FLAG present: scan no code regions (diagnostic) */
static INT            g_Fault32Warned  = 0;  /* said once: NT's 16-bit frame cannot locate a flat client's INT */
static DWORD          g_NoPmPatchMinimum = 0; /* ...or only regions >= this many bytes */
DWORD          g_MemoryDumpLinear = 0, g_MemoryDumpLength = 0;   /* MEMDUMP_FLAG */
static INT            g_Interp12      = 0;  /* INTERP12_FLAG: interpret mode 12h, no page trap */
static DWORD          g_RunStartTick= 0;  /* exec-loop start, so STAGE2 can report a RATE    */
/* Planar-mode interpretation, measured. `batches` is how many times we drove the
   guest from the host, `instrs` how many instructions that came to, and `bails`
   how often the interpreter declined the very first opcode and had to let V86 run
   (each bail is a stretch of guest execution whose A0000 writes we do NOT see). */
static DWORD          g_P12Batches   = 0;
static DWORD          g_P12Instructions    = 0;
static DWORD          g_P12Bails     = 0;
/* ── EVERY DISTINCT BAIL SITE, NOT THE FIRST TWELVE LINES. (s68) ──────────────────
     In a planar mode a bail is not one instruction: VdmRunGuest keeps the guest until the
     next EVENT with A0000 unprotected, so every VRAM write in that stretch is lost to
     st->plane[]. The list of bail sites IS the to-do list for the interpreter, and a
     12-line budget spent on one site hid the `repne scasb` that cost Lemmings its
     sprite erase for four sessions. Linear table, BOP stubs excluded, printed at
     exit with the bytes so each entry can be decoded without a dump. */
#define P12_SITE_MAX 24
static struct { DWORD Cs, Ip, Count; BYTE Bytes[8]; } g_P12Site[P12_SITE_MAX];
static UINT g_P12SiteCount = 0, g_P12SiteLost = 0;
DWORD g_HeadlessMs = PM_HEADLESS_MS_DEFAULT;   /* overridable via HEADLESS_MS_PATH */
static INT   g_LdtClientMark = 0;          /* g_LdtNext when the client switched in */
/* Ticks run per asynchronous entry -- see the drain in the main loop. */
/* ► A BATCH IS CATCH-UP, NOT A LICENCE TO COMPRESS TIME. At Doom's 140 Hz, draining
     64 ticks back to back hands the guest 0.45 SECONDS of game time in microseconds --
     and its timer ISR is where DMX writes PCM into the DMA ring, so the ring is filled
     in bursts while the mixer reads it smoothly. That is a chk-a-chk-a in the sampled
     audio no amount of mixer accuracy can undo. The exec loop gets a turn thousands of
     times a second, so a small batch still clears any real backlog. */
#define DPMI_IRQ0_BATCH 4
/* Set when the APPLICATION (not the extender's arming pass) installs a timer ISR. Until
   then vector 8 holds a placeholder stub and delivering to it is both pointless and, on
   DOS/4GW, fatal. Learned by snooping INT 21h AH=25h -- see the routing path. */
static INT   g_DpmiUseKernel = 0;         /* pmkernel.flag: run PM via VdmStartExecution */
static INT  g_CloseForced;           /* the exit in progress is ours, not the guest's */
/* ── #244: IS INT 15h STILL OURS? ─────────────────────────────────────────────────────
     The BIOS INT 09h calls INT 15h AH=4Fh for every byte (bios_kbdact.asm k4f). Our own
     INT 15h answers 4Fh with CF=1 and AL untouched -- "process it as it is" -- so while
     IVT[15h] still points at our stub the call cannot change anything, and making it
     would cost two VM exits per scancode on the path Doom and Skyroads are fragile on.
     So the call is made only once something has hooked the vector: exactly the case
     the intercept exists for (KEYB-style remappers, hot-key TSRs, p_kbd3).
     g_Int15StubOffset is recorded where the stub is planted (bios_ints[]). */
WORD  g_Int15StubOffset;
static DWORD g_Kb4FCalls, g_Kb4FTranslate;         /* k4f entries / bytes it handed back */
DWORD      g_PrintScreenJobs, g_PrintScreenErrors;
BYTE       g_PrintScreenStatus = BIOS_PRINT_SCREEN_STATUS_OK;   /* what 0050:0000 would hold */
static DWORD          g_ExecPriority     = 0;   /* guest thread priority class; see EXECPRIO_PATH */
static INT            g_QiRaise      = 0;
static INT            g_QiVif        = 0;   /* start the guest with EFLAGS.VIF set */
static INT            g_QiKeys       = 0;   /* synthesise keypresses (repro the hang) */
static DWORD          g_InterpRefused = 0;  /* interpreter declined the faulting opcode */
/* ── ★★★★ A TIMER RE-ARMED FROM INSIDE ITS OWN HANDLER DISCARDS THE TICK QUEUED BEHIND IT. ──
     s70, the user's by-hand Lemmings run: the fade completed, then "it stalled when the
     trapdoors opened" -- every heartbeat from then on inside the ISR's retrace spin,
     irq0/s == edges/s == 71, p3da 3.1M reads/s, and the new counters read blocks=796,
     timeouts=1. Not nesting this time (the in-service guard held) but a LOCKSTEP: one
     long tick (250 ms in service -- the trapdoor moment) left a tick queued behind the
     handler; the queued tick re-entered the handler AT THE EOI, that instance spun to the
     next retrace and re-armed the PIT, and its own tick then fired during the NEXT
     instance's spin -- queued again, forever. The main loop never got a cycle. A real
     8259 latches one IRR bit and would do exactly the same after such a stall; Lemmings'
     design is fragile and a 386 simply never stalled that long.
   ► The guest's own act names the fix. A `out 43h` + count on counter 0 from INSIDE the
     IRQ0 handler is the guest saying "the next tick is N clocks from NOW" -- it has just
     resynchronised. A tick that was queued before that instant belongs to the period it
     just discarded, so drop it: clear the V86 backlog and the IRR bit. For a guest that
     programs its timer once at start-up (Skyroads, Doom's DMX, the BIOS rate) the
     condition never holds -- no restart while in service -- so nothing changes for them.
     The deviation from the hardware is exactly one tick, at exactly the moment the
     guest asked for a new period. Counted: STAGE2 irq0_isr[...,resync_drop]. */
DWORD    g_Irq0ResyncDrop = 0;
enum { PM_GATE_NO_LATCH = 0, PM_GATE_VIF_OFF = 1, PM_GATE_NO_HOOK = 2, PM_GATE_IN_PM_IRQ = 3, PM_GATE_NO_IRQ = 4, PM_GATE_ASYNC_IN_FLIGHT = 5, PM_GATE_ARMED = 6, PM_GATE_TRIED = 7, PM_GATE_CLAIM_REFUSED = 8, PM_GATE_DECLINED = 9, PM_GATES = 10 };   /* g_PmCooperativeGate's columns */
/* ── #172: WHY THE PER-PASS TIMER LATCH LEFT A BACKLOG STANDING. ─────────────────
     Doom's quit wait (I_WaitVBL, a PM 3DAh poll) drops IRQ0 to ~25/s, and s81 filed it
     as ticks never RAISED. The same run's IRQ0WHY says otherwise -- gen=0 del=0x9e: every
     long gap had the ticks raised and not delivered -- and the async arm is expected to
     miss during a trap storm (why=20, the CPU thread is in host code). Every trap returns
     to the PM loop, so the per-pass latch should catch up within microseconds. It does
     not, and this names the gate. Counted per PASS with >= 2 ticks owed (the `del`
     signature), first failing gate only:
       0 no latch   1 vIF=0   2 no INT 08h hook   3 in a PM IRQ   4 noirq
       5 async injection in flight   6 vec 8 armed < 55 ms ago
       7 all gates open (the pass tried)   8 claim refused (IRQ0 masked/in service)
       9 the injector declined (guest in the extender's 16-bit code)
     7 counts the pass; 8/9 are that pass's failures, so 7 - 8 - 9 = delivered. */
static DWORD g_PmCooperativeGate[PM_GATES];
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
     DpmiInjectPmIrq(), so a before/after snapshot of the counter brackets it exactly,
     with no new plumbing into the device model. Near zero here confirms it. */
static UINT32 g_CooperativeDmaPolls;
static UINT32 g_CooperativeDmaPollsDevice[PIC_LINES_PER_CHIP];   /* ...and the same, per DEVICE line */
/* Cooperative delivery of DEVICE lines (2-7) to a PM client -- the retry the async
   path never had. `inj` is the interrupts that would previously have been LOST. */
static DWORD g_PmDeviceIrqInjected  = 0;
static DWORD g_PmDeviceIrqFail = 0;
static DWORD g_PmDeviceIrqDrop = 0;           /* pending on a line the client never hooked */
/* ── WHERE WAS THE GUEST WHEN THE CLOCK ASKED FOR A TURN? ────────────────────────────
     The asynchronous injector is the ONLY thing that can touch a protected-mode guest
     inside a BOP-free stretch, and session 20 localised Doom's death to exactly such a
     stretch (R_InitTextureMapping's ~3.5M-instruction loop 2). The commentary above
     AsyncInjectIrq() already names the open question: every LOGGED injection lands at
     0x03ae53dc -- the millisecond-delay spin, the safe case -- and "whether a later one
     lands somewhere else" was never measured.
     It could not be measured with a per-attempt line: the guest's timer runs at 16124 Hz
     (PIT-RELOAD 0x4a, measured), so an uncapped line per attempt is ~700k lines a run and
     a capped one goes quiet long before the interesting part. Session 20 raised the cap
     and still saw nothing near the death, and concluded "zero async attempts" -- but the
     V86 arm of AsyncInjectIrq() returns SILENTLY, so that was an unread instrument, not
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
/* The longest single DpmiEnterProtectedMode() -- i.e. the longest run of guest protected-mode
   code that gave the host no turn at all -- and the record-breakers past the threshold.
   Only NEW maxima log, so a run reports a growth curve of a few dozen lines instead of
   one line per entry. */
#define PM_STRETCH_LOG_US 300u
static DWORD g_PmStretchMaximumMicroseconds = 0;
static DWORD g_PmStretchLogged = 0;
static DOS_START_MODE g_StartMode = DOS_START_NORMAL;

/* ⚠ Reported at EXIT, not at init. The early-startup log line was written
   before a later LogWrite(LOG_PATH,...) TRUNCATES the file, so it never
   survived to be read -- which looked exactly like the code not running. */
PCSTR g_StdioHow = "(not initialised)";
PCSTR g_StdioSource = "";   /* which channel it came down, if any */
DWORD g_StdioParentProcessId;   /* GH #131: whose child we turned out to be */
DWORD g_DmxSamples, g_DmxBusy[12], g_DmxMixerOk;
DWORD g_DmxOverdue, g_DmxOverdueMaximum, g_DmxAnyBusy;
/* ── PACE THE PIT. ──────────────────────────────────────────────────────────────────
     HostPitSync() advances the emulated 8254 by however much wall-clock has elapsed
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
   ⚠ It takes g_Lock like every other caller, so it is a knob (pitpace.txt = 0 to
     disable) and the lock figures must be read on the first run with it on. */
typedef MMRESULT (WINAPI *PFN_TIME_BEGIN_PERIOD)(UINT);
static HANDLE g_PitPaceThread;
/* #256: 1 while the TOP-LEVEL PM loop is dispatching -- the one place a PM BIOS wait may
   re-execute its BOP (every nested loop counts its passes). See the PM INT 15h 86h arm. */
static INT g_PmTopDispatch;   /* set by the top-level loop before it dispatches  */
static HANDLE g_CourierThread;
UINT g_CpuSpeedReferenceMhz = CPUSPEED_REF_MHZ_DEFAULT;  /* cpuref.txt       */
static HANDLE g_CpuSpeedThread;
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
     the same box with the same keyboard (tests/probes/dos/tymat.asm, run under both
     hosts via rt_stock.bat):
         stock ntvdm   delay 7 ticks ~385 ms   102 repeats -> 22.1/s
         us            delay 9 ticks ~495 ms    49 repeats -> 10.9/s
     Half the rate and a third again the delay. Hardcoding 385/22.1 would repeat
     the same mistake with a fresher number, because that is THIS box's XP setting,
     not a universal truth -- the user can change it in Control Panel and a real DOS
     box would follow. So take it from the system, which is the thing stock ntvdm is
     effectively passing through, and keep the measured pair above as the
     VERIFICATION target rather than the source. */
UINT32 g_TypematicDelayMicroseconds  = TYPEMATIC_DEFAULT_DELAY_US;   /* replaced at startup from XP's setting */
DWORD    g_TypematicSpiDelay, g_TypematicSpiSpeed;     /* raw, so STAGE2 can show them */
/* ── ★ REFLECT DPMI 0300 TO THE GUEST'S OWN REAL-MODE HANDLER -- ON BY DEFAULT (s81).
     It was off (simintrefl.flag to enable) because it wedged ZAR waiting on an SB
     completion the nested V86 call never delivered. s81 fixed that, and the spec says
     0300 runs the real-mode handler, so it is on; simintrefl_off.flag is the opt-out.
   ► #247: "on" now means EVERY vector -- our stubs too -- runs from the IVT (RmcsSimIntRoute
     in dpmi_rmcs.h). The flag restores the pre-#247 ROUTING: 21h/33h/10h host-side,
     everything else not run. (Not the pre-#247 marshalling: the full register write-back
     and the INT 21h carry stay fixed either way.) The rig's rollback lever for the routing. */
#define SIMINTREFL_OFF_FLAG CFG_("simintrefl_off.flag")
/* ── WHAT DID THE GUEST DO AFTER THE CALLBACK WAS INJECTED? (s71) ──────────────────
     QB's first callback was injected (inj=1) and never came back (done=0, no STRAY),
     while the guest carried on ticking -- so every later event was refused as
     "in flight" and a click opened nothing. Nothing in the log said where the handler
     went: the heartbeat only ever shows the last TRAP, which for an idle text-mode
     guest is the INT 08h stub. So for the first few injections, log the next few VM
     events verbatim (event, BOP number, cs:ip, ss:sp, the code and stack bytes). The
     first event after a good injection is the handler's own CLI/STI reflection or its
     RETF landing on the return BOP; anything else names the wrong turn. Bounded. */
INT    g_MouseCallbackTrace;            /* VM events still to log after an injection  */
static WORD      g_WowEntryDs = 0;   /* krnl386's autodata paragraph      */
static INT       g_WowEntering = 0;   /* the guest is krnl386, not DOS     */
/* ── #153: FILE > OPEN EXECUTABLE / OPEN RECENT. ─────────────────────────────────────
     User decision (s81): if this window is sitting at the top-level shell's prompt,
     TYPE the program into it -- drive, `CD`, name -- so it runs here and the prompt
     comes back afterwards. Otherwise start it in a new window, as a double-click does.
   ► "At the prompt" is read off the DOS kernel, not guessed: the top-level program is a
     shell, nothing is EXEC'd under it, and it is inside its AH=0Ah line read. Anything
     already typed on that line is rubbed out first (our AH=0Ah honours backspace).
   ⚠ A NEW WINDOW NEEDS CREATE_NEW_CONSOLE. This process IS a VDM, and a DOS program
     started from its console is queued by CSRSS to THIS VDM -- the relaunch case in the
     task-done path -- rather than getting a machine of its own.
   ⚠ Only a DOS image is typed: a Win16/Win32 one at a DOS prompt just says it needs
     Windows. And the directory must fit DOS's 64-character current-directory limit. */
/* ── #211: TWO BUSY GUESTS MUST NOT STARVE THE MACHINE. ──────────────────────────────
     The thread that runs the guest is ABOVE_NORMAL (execprio), and a DOS program that
     polls its keyboard keeps it busy for ever -- QBasic idling in its editor is a whole
     core. Measured on the 2-core rig (s81): QBasic in one host and Skyroads in another
     took both cores, and every NORMAL process -- cmd, tasklist, the rig's own harness
     -- stopped dead until the hosts were killed.
   ► So while ANOTHER NTVDMEX is running, a host whose window is not in the foreground
     drops its guest to BELOW_NORMAL, and gets its own priority back when it is brought
     forward. A host running alone is never touched: its priority is exactly what it was,
     which is what the Skyroads timing guard and every rig measurement assume. */
HANDLE g_ExecThread;                        /* the exec (guest) thread               */
INT    g_ExecPriorityForeground = THREAD_PRIORITY_NORMAL;
NTVDMEX_SETTINGS g_SettingsDisk;

INT g_DspVersionForced;                /* cfg\dspver.txt beat the model's version */

/* ── #136: CONVENTIONAL MEMORY, AND WHERE IT ENDS. ────────────────────────────────
     g_ConventionalKbWant is the setting (memory FITTED, 64..640 KB); g_DosMemoryTop is the
     paragraph DOS's arena ends at and the EBDA starts at, decided ONCE at start-up from
     it (BiosConventionalTopParagraph -- 640 KB gives exactly DOS_MEM_TOP, 9FC0h, so the default
     machine is the one every build so far has run). Every consumer reads the variable:
     INT 12h, 0040:0013, 0040:000E, INT 15h C1h, CMOS 15h/16h, the first PSP's +02h and
     the MCB chain. ⚠ START-UP ONLY: SettingsApply runs again on a dialog OK, but moving
     the top of an arena a program is already running in is not something any machine
     does; the new value is the next program's. */
UINT g_ConventionalKbWant = BIOS_CONV_KB_MAX;
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
DWORD  g_ModeYSelectorCalls = 0;   /* ModeYRemapSelect() entered with the remap live  */
DWORD  g_ModeYSelectorSame  = 0;   /* ...and the window was already where it wanted     */
DWORD  g_ModeYSelectorZero  = 0;   /* ...and the mask selected no plane at all          */
DWORD  g_ModeYTimelineIns[YTL_SECS];   /* instructions interpreted for mode Y (s80, design C) */
UINT64 g_ModeYTimelineInterpreterCycles[YTL_SECS];   /* ...and the host cycles that took */
DWORD g_ModeYFanoutBarWrites[2];      /* fan-out writes to bar bytes, by band  */
DWORD g_ModeYFanoutBarDistinct[2];    /* ...distinct offsets, by band          */
DWORD g_ModeYFanoutBar4Way[2];        /* ...of which the mask was all four     */
DWORD  g_ModeYLatchOk = 0, g_ModeYLatchUnsolved = 0, g_ModeYLatchDescriptor = 0;

/* --- planar mode-12h: trap direct A0000 writes through the VGA write engine -- */


INT g_A000Protection = 0;

/* The mode-12h trap-storm escape hatch. By default V86 runs on the real CPU and
   each VGA access (memory OR port) is emulated one-at-a-time as a device access
   -- pure device virtualization. But QuickBasic plots pixels one at a time and
   reprograms a VGA register via OUT *between* pixels, so a fill is hundreds of
   thousands of fault round-trips (port faults AND memory faults) and crawls.

   HostInterp() is the opt-in batching interpreter: load the V86 register file,
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

#define LIVEHB_FLAG CFG_("livehb.flag")
#define PITLATCH_FLAG CFG_("pitlatch.flag")
enum { GUEST_EIP_FROM_FRAME = 0, GUEST_EIP_FROM_TIB_SLOT = 1, GUEST_EIP_FROM_BLOCKS = 2, CSRSS_REPORT_GRACE_MS = 50 };   /* WinMain: a PM fault's EIP source, the TDB's hInstance, ExitVDM's wait */
#define UNIMPLEMENTED_BOP_EXIT_CODE 0xBD   /* a guest killed by an unanswered BOP: never a clean 0 */
enum { ENVIRONMENT_SCAN_MAX = 900, PATH_VALUE_MAX = 250 };   /* sub 0Fh's environment snapshot */
enum { PAUSE_POLL_MS = 20, PMWATCH_COLUMNS = 2, PM_HEADLESS_CHECK_MASK = 0xFFF, PM_STEPS_MAX = 100000000 };   /* WinMain's exec loops */
#define CPU_REFERENCE_MHZ_MAX_U 100000u
#define CPU_REFERENCE_MHZ_MIN_U 1u
enum { EXTENDER_ARGV0_SAFE_LENGTH = 62, ENVIRONMENT_DUMP_BYTES = 0xC0, ENVIRONMENT_DUMP_LINE = 264, ENVIRONMENT_DUMP_NULS = 2, ENVIRONMENT_DUMP_SKIP = 8, COMMAND_TAIL_DUMP_BYTES = 16, UI_TICK_MS_MAX = 100, MOUSE_SENSITIVITY_MIN = 10, MOUSE_SENSITIVITY_MAX = 1000 };   /* WinMain: argv[0], the guest dumps, knob ranges */
#define WOW_KRNL386_ENTRY_AX 0x4B4F   /* 'OK': what krnl386 expects in AX at entry */
#define FILE_TYPE_NOT_ASKED_U 0xFFFFFFFFu   /* the handle report: no handle to ask about */
enum { NT_AWARE_SHELL_BOPS_MIN = 8, ROUTED_PATH_MAX = 120, WOW_COMMAND_DIRECTORY_MAX = 0x10C, SHELL_HEADER_READ = 0x44, COMMAND_COM_LENGTH = 11, STD_HANDLE_REPORTS = 5 };   /* WinMain's launch path */
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
 * ================================================================================ */
static INT g_DpmiUseInterp = 0;             /* run 53 toggle (1 = interp fallback, 0 = kernel PM path).
                                                 run 59 (GH #18): 0 to exercise the real-CPU kernel path
                                                 WITH the +0x638 PM-fault trampoline. Flip to 1 to restore
                                                 the VM-confirmed interpreter runs (i310102/DPMIBACK). */
#define MODEY_GAP_MAX_U     65536u
#define OS_VERSION_NOT_NT_U 0x80000000u   /* GetVersion: the high bit is set on Windows 9x */
enum { EXECPRIO_ABOVE_NORMAL = 1, EXECPRIO_HIGHEST = 2 };   /* execprio.txt: the exec thread's priority (0 = left alone) */
enum { QIMODE_DIGITS = 2, QIMODE_RAISE = 0x04, QIMODE_VIF = 0x08, QIMODE_KEYS = 0x20, QIMODE_NO_SUSPEND = 0x40, QIMODE_KEYS_ASYNC = 0x80, QIMODE_PIC_BASE = 0x60 };   /* qimode.txt bits (QIMODE_PATH) */
enum { INT53_FIELD_AL = 0, INT53_FIELD_AX = 1, INT53_FIELD_CARRY = 2, INT53_FIELDS = 3, MEMDUMP_FIELDS = 2 };   /* int53.txt: "<AL> <AX> <CF>"; memdump.flag: "<linear> <size>" */
/* ── ★★★★★ A TITLE IS "PROGRAM [ARGUMENTS]", AND WE OPENED THE WHOLE THING AS A
     FILENAME. (session 59) `target.txt` has split `path [args]` since M2.5, but the
     CSRSS path -- which is EVERY REAL LAUNCH, because the IFEO hook is how a program
     reaches us on the user's machine -- never did. `ZAR.EXE -Help` therefore tried to
     open a file literally called `C:\game\ZAR.EXE -Help`, read ZERO bytes, fell through
     to the four-byte `mov ah,4Ch / int 21h` embedded stub and exited in 78 ms having
     printed nothing. **No DOS program could be given an argument at all** -- not
     `EDIT FOO.TXT`, not `DOOM -warp 1 1` -- and every one of them would have looked
     like "the program runs and does nothing", which is the same symptom GH #131 chased
     for a session. The tell is `loaded 0x00000000` followed by `embedded fallback`.
   ► TRY THE WHOLE STRING FIRST, THEN SPLIT LEFT TO RIGHT, AND LET THE FILE SYSTEM
     ARBITRATE. Opening the untouched string first is what keeps a real path CONTAINING
     a space working; only if that fails is a space treated as the separator, and then
     the FIRST split that names a file which actually EXISTS wins -- so
     `C:\Program Files\x\y.exe -a` finds `y.exe` and not `C:\Program`. Guessing where
     the arguments start is exactly the thing not to do here.
   ⚠ `path` is modified in place (NUL-terminated at the end of the program name) and
     *pargs is left pointing at the remaining arguments within it, leading whitespace
     skipped, or NULL when there were none. Returns INVALID_HANDLE_VALUE if no split
     names a real file, leaving `path` as it was found. */
static HANDLE CsrssOpenSplit(PSTR path, PSTR *argumentsOut)
{
    HANDLE fileHandle;
    INT index;
    *argumentsOut = NULL;
    fileHandle = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (fileHandle != INVALID_HANDLE_VALUE) return fileHandle;
    for (index = 0; path[index]; ++index) {
        CHAR save;
        if (path[index] != ' ' && path[index] != '\t') continue;
        save = path[index];
        path[index] = 0;
        fileHandle = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (fileHandle != INVALID_HANDLE_VALUE) {
            PSTR arguments = path + index + 1;
            while (*arguments == ' ' || *arguments == '\t') ++arguments;
            if (*arguments) *argumentsOut = arguments;
            return fileHandle;
        }
        path[index] = save;
    }
    return INVALID_HANDLE_VALUE;
}

































static PSTR TaskRelaunchQueuedCommand(PSTR cursor, PSTR const base)
{
    /* ── A COMMAND ARRIVED IN THE WINDOW. The launcher queued its next DOS program
         to THIS console's VDM (us) between the report and ExitVDM; CSRSS handed it
         to the blocked report call. We are not going to run it in this process, and
         CSRSS has already forgotten it: relaunch it here, in the same console, so it
         runs in a fresh host. (Measured before this: the program silently did not
         run and the launcher saw exit code 0.) */
    if (g_ReportGotNext && g_CsrssNextApp[0]) {
        CHAR commandLine[2300]; PSTR scan = commandLine; STARTUPINFOA si; PROCESS_INFORMATION processInfo;
        scan = LogPut(scan, "\""); scan = LogPut(scan, g_CsrssNextApp); scan = LogPut(scan, "\"");
        if (g_CsrssNextCommand[0]) { scan = LogPut(scan, " "); scan = LogPut(scan, g_CsrssNextCommand); }
        *scan = 0;
        ZeroMemory(&si, sizeof si); si.cb = sizeof si; ZeroMemory(&processInfo, sizeof processInfo);
        /* ⛔ HAND OVER THE SINGLE-INSTANCE MUTEX FIRST. (s73) The relaunched host is a
           second ntvdmhost, and the guard in WinMain refuses a second instance while
           the first OWNS the mutex -- which this one did, for as long as it waited
           on the child. Measured: `BC x.BAS` then `LINK x.OBJ` from one cmd window --
           BC ran, LINK was queued to this VDM, the relaunched host wrote "REFUSED:
           another ntvdmhost is LIVE" and exited, the launcher saw rc=0, and no EXE
           was ever written. That was "QBasic cannot build EXEs" from a batch file.
           The guest is gone (the exec loop is out), so the system-wide things the
           guard protects are released here too; the child takes them over. */
        HostPanicRelease();
        if (g_OnceMutex) { ReleaseMutex(g_OnceMutex); CloseHandle(g_OnceMutex); g_OnceMutex = NULL; }
        /* ── AND ITS REDIRECT. (s73) The command's StdIn/Out/Err came back from the
             report call as handles CSRSS placed in THIS process (the launcher's
             `LINK > file`). The child's StdioInitialize takes an inherited disk/pipe
             standard handle first, so hand them over inheritable; a console handle
             is left alone (the child finds its console the way it always has).
             Measured before this: the relaunched LINK's log said "stdout -> none". */
        {   INT item, any = 0; HANDLE handles[CSRSS_STANDARD_HANDLES] = { NULL, NULL, NULL };
            for (item = 0; item < CSRSS_STANDARD_HANDLES; ++item) {
                HANDLE handle = g_CsrssNextStandardHandles[item]; DWORD valueType;
                if (!handle || handle == INVALID_HANDLE_VALUE) continue;
                valueType = GetFileType(handle);
                if (valueType != FILE_TYPE_DISK && valueType != FILE_TYPE_PIPE) continue;
                if (DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(), &handles[item],
                                    0, TRUE, DUPLICATE_SAME_ACCESS)) any = 1;
            }
            if (any) {
                /* ⚠ STARTUPINFO NEVER REACHES THE CHILD: a DOS .EXE goes through
                   BaseSrv, which creates the ntvdm (us again, via IFEO) itself.
                   What the child DOES read is its PARENT'S PEB standard handles
                   (StdioFromParent, GH #131) -- and its parent is this process.
                   SetStdHandle writes exactly those PEB fields. Measured: with
                   STARTUPINFO alone the child still said "stdout -> none". */
                if (handles[CSRSS_STD_IN]) SetStdHandle(STD_INPUT_HANDLE,  handles[CSRSS_STD_IN]);
                if (handles[CSRSS_STD_OUT]) SetStdHandle(STD_OUTPUT_HANDLE, handles[CSRSS_STD_OUT]);
                if (handles[CSRSS_STD_ERR] || handles[CSRSS_STD_OUT]) SetStdHandle(STD_ERROR_HANDLE, handles[CSRSS_STD_ERR] ? handles[CSRSS_STD_ERR] : handles[CSRSS_STD_OUT]);
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
                           g_CsrssNextDirectory[0] ? g_CsrssNextDirectory : NULL, &si, &processInfo)) {
            cursor = LogPut(cursor, " -> pid 0x"); cursor = LogHex(cursor, processInfo.dwProcessId); cursor = LogPut(cursor, ", waiting\r\n");
            LogAppend(LOG_PATH, base, cursor); cursor = base;
            WaitForSingleObject(processInfo.hProcess, INFINITE);
            CloseHandle(processInfo.hProcess); CloseHandle(processInfo.hThread);
            cursor = LogPut(cursor, "STAGE2: task done -> relaunched command finished\r\n");
        } else { cursor = LogPut(cursor, " -> CreateProcess FAILED 0x"); cursor = LogHex(cursor, GetLastError()); cursor = LogPut(cursor, "\r\n"); }
        LogAppend(LOG_PATH, base, cursor); cursor = base;
    }
    return cursor;
}


static PSTR TaskReportExitToCsrss(PSTR cursor, PSTR const base, DOS_MACHINE *machine)
{
    /* ── ★ REPORT THE ERRORLEVEL TO CSRSS AND LEAVE THE CONSOLE. (s72) ──────────────
         Only when CSRSS queued this task to us (the harness stub / target.txt shapes
         and a WOW launch are untouched). AFTER the flush, so the launcher -- released
         the instant CSRSS takes the report -- finds the program's whole output.
       ► MEASURED: GetNextVDMCommand with the exit code releases the launcher and then
         BLOCKS, DONT_WAIT or not, until the console's next DOS command arrives: stock
         ntvdm stays resident per console and that wait is its idle state. We do not
         stay, so the report goes in a helper thread and the main thread carries on to
         ExitVDM, which is what releases the console's VDM record (without it the next
         DOS program typed into the same window was queued to a host that had gone). */
    if (g_Fetch2Ok) {
        HANDLE thread2; DWORD threadId = 0, waitResult;
        cursor = LogPut(cursor, "STAGE2: task done -> reporting exit code 0x"); cursor = LogHex(cursor, (DWORD)machine->ExitCode);
        cursor = LogPut(cursor, " to CSRSS (helper thread)...\r\n");
        LogAppend(LOG_PATH, base, cursor); cursor = base;
        thread2 = CreateThread(NULL, 0, CsrssReportThread, (LPVOID)(ULONG_PTR)machine->ExitCode, 0, &threadId);
        /* A short grace so the report's LPC is processed (launcher released) before
           ExitVDM's is. The wait normally times out: the call is blocked for the
           console's next command, which is stock ntvdm's idle state, not ours. */
        waitResult = thread2 ? WaitForSingleObject(thread2, CSRSS_REPORT_GRACE_MS) : WAIT_FAILED;
        if (thread2) CloseHandle(thread2);
        cursor = LogPut(cursor, "STAGE2: task done -> report thread ");
        cursor = LogPut(cursor, waitResult == WAIT_OBJECT_0 ? "returned" : waitResult == WAIT_TIMEOUT ? "blocked for the console's next command (expected)" : "could not start");
        cursor = LogPut(cursor, "; ExitVDM...\r\n");
        LogAppend(LOG_PATH, base, cursor); cursor = base;
        {   BOOL isExitOk = CsrssExitVdm(); DWORD exitError = GetLastError();
            cursor = LogPut(cursor, "STAGE2: task done -> ExitVDM = "); cursor = LogPut(cursor, isExitOk ? "TRUE" : "FALSE");
            cursor = LogPut(cursor, " err=0x"); cursor = LogHex(cursor, exitError); cursor = LogPut(cursor, " got_next="); cursor = LogHex(cursor, (DWORD)g_ReportGotNext);
            cursor = LogPut(cursor, " next_app=["); cursor = LogPut(cursor, g_CsrssNextApp); cursor = LogPut(cursor, "]\r\n");
            LogAppend(LOG_PATH, base, cursor); cursor = base; }
        cursor = TaskRelaunchQueuedCommand(cursor, base);
    }
    return cursor;
}





/* A #GP through an IDT gate is an unserviced INT nn the client issued, not an exception it asked for: reflect it as the interrupt, and patch the site so the next pass takes the BOP path. */
static INT DpmiServiceIdtGateFault(PSTR *cursorIo, PSTR const base, volatile WORD * const frame, const DWORD faultEsp, const DWORD faultSs, const DWORD faultEip, volatile BYTE * const tib, DOS_MACHINE *machine, const UINT steps)
{
    PSTR cursor = *cursorIo;
    if ((frame[DPMI_FRAME_ERROR] & X86_ERROR_CODE_IDT) && HostReadable((const VOID *)frame, DPMI_FRAME16_SIZE)) {
        DWORD gateVector = (DWORD)(DPMI_SELECTOR_INDEX(frame[DPMI_FRAME_ERROR])) & BYTE_MASK;
        DWORD guestCodeBase  = DpmiSelectorBase(frame[DPMI_FRAME_CS]);
        volatile BYTE *guestInstruction = (volatile BYTE *)(ULONG_PTR)(guestCodeBase + frame[DPMI_FRAME_IP]);
        /* ── ⚠⚠⚠ WHAT MAKES THE FRAME'S IP TRUSTWORTHY IS THE
             SELECTOR'S BASE, NOT ITS WIDTH. (s74)
             This began as `if (guestCodeBase && ...)`. DpmiSelectorBase() returns
             g_Ldt[idx].base, so a base of 0 -- exactly what a FLAT
             32-bit client runs on (`desc 0x0000ffff:0x00cffa00`, base 0,
             limit 4 GB, D/B=1) -- was being read as "no selector" and
             those faults declined by accident.
             ⚠ The first attempt at this replaced the test with "decline
               any 32-bit CS", and THAT WAS A REGRESSION, measured: the
               serviced count on heaven7 fell 62 -> 43, because a 32-bit
               CS with a NON-ZERO base (0x287, base ~0x48000) has a
               perfectly good IP and had been serviced all along. Width
               was never the issue.
             The real issue is narrow and it is this: NT hands us a
             16-BIT exception frame whatever the client is, so when the
             base is 0 the EIP *is* the linear address and arrives with
             its top 16 bits gone -- heaven7 reports 0x231c for an
             instruction living at ~0x0433231c. Then `guestCodeBase + fr[3]` names
             LOW MEMORY, and a chance `CD nn` match there would make us
             write C4 C4 into an innocent page: the eager patcher's own
             failure mode, relocated.
           ⇒ So: trust `guestCodeBase + fr[3]` whenever the base is non-zero (as
             before), and for the flat base-0 case RECONSTRUCT the
             address from the client's own 0501 blocks, requiring a
             UNIQUE hit that actually holds `CD <vec>`. Unique-or-decline
             is evidence; picking the first match would be a guess. */
        UINT32 guestAccessRights = 0;
        INT guestPresent = DpmiSelectorDescriptor(frame[DPMI_FRAME_CS], &guestAccessRights, NULL);
        INT guestIs32    = guestPresent && (((guestAccessRights >> X86_DESCRIPTOR_FLAGS_SHIFT) & DPMI_DESCRIPTOR_FLAGS_MASK) & DPMI_DESCRIPTOR_FLAG_BIG);
        INT guestTruncated   = guestIs32 && guestCodeBase == 0;   /* EIP *is* the linear addr */
        INT guestCandidates    = 0;
        DWORD guestLinear   = guestCodeBase + frame[DPMI_FRAME_IP];
        DWORD guestRecovered   = 0;
        INT   guestSource   = GUEST_EIP_FROM_FRAME;                   /* 0 frame, 1 TIB slot, 2 blocks */
        INT   guestSsIs32  = DpmiSelectorIs32(frame[DPMI_FRAME_SS]);
        /* the slot must agree with the frame's low halves, or it is not
           the slot we calibrated -- then we do not resume on it */
        INT   guestEspOk = !guestSsIs32 || ((faultEsp & WORD_MASK_U) == frame[DPMI_FRAME_SP] && (faultSs & WORD_MASK_U) == frame[DPMI_FRAME_SS]);
        /* ── ★★ THE FULL-WIDTH REGISTERS ARE IN THE TIB; THE FRAME IS
             THE TRUNCATED COPY. (s74, second pass) ─────────────────────
             The kernel saves the faulting SS:ESP and EIP at full width
             BEFORE it builds the 16-bit DPMI frame: `faultSs:faultEsp` (from the misnamed
             VTIB_FLT_SAVCS/SAVEIP) is SS:ESP and `faultEip` (VTIB_FLT_SAV3) is EIP. That is
             not a reading of the layout, it is three faults with every
             field known independently, from one heaven7 run:
                 based CS 0x287:0x0119   sav3=0x00000119  savSS:ESP=0x297:0x5874
                 flat  CS 0x347 -> lin 0x04332335 (blocks, unique)
                                         sav3=0x04332335  savSS:ESP=0x34f:0x8610
               and the frame's fr[7]:fr[6] equalled the low halves each time.
             So the flat base-0 EIP need not be REBUILT from the client's
             0501 blocks; it is in the slot. The block walk stays as a
             CROSS-CHECK (logged: agree / disagree / ambiguous) and as the
             fallback if the slot's address does not hold `CD vec`.
           ⚠ AND THE SAME TRUNCATION APPLIES TO ESP, which this arm had
             been restoring from fr[6] -- 16 bits -- while its own comment
             claimed a flat-SS client was "declined above". Nothing declined
             it. heaven7 survived only because its SS is BASED with SP <
             64 KB; a Watcom flat-model client (Doom: DS=SS=flat, ESP ~
             0x0043xxxx) resumed through here would have lost the top half
             of its stack pointer on the first lazily-serviced INT. Restore
             ESP from the slot whenever SS is 32-bit, and refuse to resume
             if the slot and the frame disagree in the low half -- that is
             the one check that would catch a mis-identified slot. */
        if (guestTruncated) {
            guestRecovered = DpmiRecoverFlatEip((DWORD)frame[DPMI_FRAME_IP], (BYTE)gateVector, &guestCandidates);
            if ((faultEip & WORD_MASK_U) == frame[DPMI_FRAME_IP]
                && HostReadable((const VOID *)(ULONG_PTR)faultEip, X86_INT_LENGTH)
                && ((const volatile BYTE *)(ULONG_PTR)faultEip)[0] == X86_OP_INT
                && ((const volatile BYTE *)(ULONG_PTR)faultEip)[1] == (BYTE)gateVector) {
                guestLinear = faultEip; guestSource = GUEST_EIP_FROM_TIB_SLOT;
            } else {
                guestLinear = guestRecovered; guestSource = GUEST_EIP_FROM_BLOCKS;      /* 0 = ambiguous or absent */
            }
            guestInstruction   = (volatile BYTE *)(ULONG_PTR)guestLinear;
            if (!guestLinear && !g_Fault32Warned) {
                CHAR wowLine3[288], *wowCursor3 = wowLine3;
                g_Fault32Warned = 1;
                wowCursor3 = LogPut(wowCursor3, "  EXC: #GP(IDT) vec=0x"); wowCursor3 = LogHex(wowCursor3, gateVector);
                wowCursor3 = LogPut(wowCursor3, " in a FLAT base-0 32-bit CS 0x");
                wowCursor3 = LogHex(wowCursor3, frame[DPMI_FRAME_CS]);
                wowCursor3 = LogPut(wowCursor3, ": NT's frame is 16-bit so the EIP arrived"
                              " truncated (0x"); wowCursor3 = LogHex(wowCursor3, frame[DPMI_FRAME_IP]);
                wowCursor3 = LogPut(wowCursor3, "); TIB sav3=0x"); wowCursor3 = LogHex(wowCursor3, faultEip);
                wowCursor3 = LogPut(wowCursor3, " does not hold CD "); wowCursor3 = LogHexByte(wowCursor3, (UINT)gateVector);
                wowCursor3 = LogPut(wowCursor3, ", and reconstruction from the client's"
                              " 0501 blocks found "); wowCursor3 = LogHex(wowCursor3, (DWORD)guestCandidates);
                wowCursor3 = LogPut(wowCursor3, " candidates -- need exactly 1. Reflecting instead.\r\n");
                LogAppend(LOG_PATH, wowLine3, wowCursor3); SerialOut(wowLine3, wowCursor3);
            }
        }
        if (guestSsIs32 && !guestEspOk && !g_Fault32Warned) {
            CHAR wowLine4[224], *wowCursor4 = wowLine4;
            g_Fault32Warned = 1;
            wowCursor4 = LogPut(wowCursor4, "  EXC: #GP(IDT) vec=0x"); wowCursor4 = LogHex(wowCursor4, gateVector);
            wowCursor4 = LogPut(wowCursor4, " with a 32-bit SS 0x"); wowCursor4 = LogHex(wowCursor4, frame[DPMI_FRAME_SS]);
            wowCursor4 = LogPut(wowCursor4, ": TIB savSS:savESP=0x"); wowCursor4 = LogHex(wowCursor4, faultSs);
            wowCursor4 = LogPut(wowCursor4, ":0x"); wowCursor4 = LogHex(wowCursor4, faultEsp);
            wowCursor4 = LogPut(wowCursor4, " does not match the frame's 0x"); wowCursor4 = LogHex(wowCursor4, frame[DPMI_FRAME_SS]);
            wowCursor4 = LogPut(wowCursor4, ":0x"); wowCursor4 = LogHex(wowCursor4, frame[DPMI_FRAME_SP]);
            wowCursor4 = LogPut(wowCursor4, " -- cannot restore a full ESP. Reflecting instead.\r\n");
            LogAppend(LOG_PATH, wowLine4, wowCursor4); SerialOut(wowLine4, wowCursor4);
        }
        if (guestPresent && guestLinear && guestEspOk
            && HostReadable((const VOID *)guestInstruction, X86_INT_LENGTH)
            && guestInstruction[0] == X86_OP_INT && guestInstruction[1] == (BYTE)gateVector) {
            /* ── ★★★★★ THIS IS THE PASS THAT RE-PATCHED CALC'S FP SITE.
                 (session 56 -- the question session 55 left open.)
                 Session 55 put a 34h..3Fh guard in the SCANNER and the
                 guard was right, but it was in the wrong place: the
                 scanner never touched CALC's site. THIS did. Measured,
                 from the session-55 CALC log:

                   EXC: #GP(IDT) is a RAW INT 0x39 at 0x0b77:0x05c6
                        lin=0x03d15b66 -- servicing + patching     x120,673

                 The comment below is right that the CPU's error code
                 names the vector and the address, and that this is the
                 strongest evidence a byte pair is really an INT. It is
                 still not evidence that the byte pair is an INTERRUPT.
                 `CD 39` in FP-emulator code IS the x87 instruction, and
                 the fault is how the emulator is ENTERED, not a failure.
                 Rewriting it to a BOP destroys the guest's own encoding
                 -- which is exactly what the scanner was stopped from
                 doing, by a guard this path did not have. */
            INT guestResult;
            INT isFloatingPointVector = (gateVector >= VECTOR_FLOATING_POINT_FIRST && gateVector <= VECTOR_FLOATING_POINT_LAST);
            INT floatingPointHooked = isFloatingPointVector && g_PmInt[gateVector].Client;
            cursor = LogPut(cursor, "  EXC: #GP(IDT) is a RAW INT 0x"); cursor = LogHex(cursor, gateVector);
            cursor = LogPut(cursor, " at 0x"); cursor = LogHex(cursor, frame[DPMI_FRAME_CS]);
            cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, frame[DPMI_FRAME_IP]);
            cursor = LogPut(cursor, " lin=0x"); cursor = LogHex(cursor, guestLinear);
            if (guestTruncated) {
                cursor = LogPut(cursor, guestSource == GUEST_EIP_FROM_TIB_SLOT ? " (flat base-0 CS: EIP from TIB sav3,"
                                        " blocks cross-check "
                                      : " (flat base-0 CS: EIP RECONSTRUCTED"
                                        " from blocks, TIB sav3=0x");
                if (guestSource == GUEST_EIP_FROM_TIB_SLOT) {
                    if (!guestRecovered)             { cursor = LogPut(cursor, "ambiguous n="); cursor = LogHex(cursor, (DWORD)guestCandidates); }
                    else if (guestRecovered == guestLinear)   cursor = LogPut(cursor, "AGREE");
                    else                   { cursor = LogPut(cursor, "DISAGREE 0x"); cursor = LogHex(cursor, guestRecovered); }
                } else cursor = LogHex(cursor, faultEip);
                cursor = LogPut(cursor, ")");
            }
            if (guestSsIs32) {
                cursor = LogPut(cursor, " SS32 esp=0x"); cursor = LogHex(cursor, faultEsp);
                if ((faultEsp & WORD_MASK_U) != frame[DPMI_FRAME_SP]) cursor = LogPut(cursor, " ⚠ slot/frame DISAGREE");
            }
            if (floatingPointHooked) {
                cursor = LogPut(cursor, " -- FP EMULATOR RANGE: reflecting to the"
                            " handler the guest installed, 0x");
                cursor = LogHex(cursor, g_PmInt[gateVector].Selector);
                cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, g_PmInt[gateVector].Offset);
                cursor = LogPut(cursor, " (not patched, not serviced)\r\n");
            } else {
                cursor = LogPut(cursor, isFloatingPointVector ? " -- FP EMULATOR RANGE but NO handler"
                                  " installed; servicing without patching\r\n"
                                : " -- servicing + patching\r\n");
            }
            LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
            /* put the guest back where it faulted, EIP ON the INT.
               ⚠ For the flat base-0 case the frame's IP is TRUNCATED, so
                 resume on the full linear address (TIB slot, or the block
                 reconstruction) -- writing fr[3] back there would be a
                 wild jump into low memory. The frame's SP is truncated the
                 same way: for a 32-bit SS take ESP from the TIB slot, which
                 `guestEspOk` has just checked against the frame's low half. */
            VDM_SET16(tib, VTIB_SS, frame[DPMI_FRAME_SS]);
            VDM_REG(tib, VTIB_ESP) = guestSsIs32 ? faultEsp : (DWORD)frame[DPMI_FRAME_SP];
            VDM_SET16(tib, VTIB_CS, frame[DPMI_FRAME_CS]);
            VDM_REG(tib, VTIB_EIP) = guestTruncated ? (guestLinear - guestCodeBase) : (DWORD)frame[DPMI_FRAME_IP];
            VDM_SET16(tib, VTIB_EFLAGS, frame[DPMI_FRAME_FLAGS]);
            /* ── ★★★★ REFLECT IT, DO NOT SERVICE IT. ─────────────────
                 An FP `CD nn` is not a request to the host; it is the
                 guest's own emulator being entered, and the ONLY thing
                 that knows where to resume is that emulator. It reads
                 the modrm and displacement that FOLLOW the two bytes,
                 then REWRITES THE RETURN IP ON THE STACK to step over
                 them. So build the frame the CPU would have built --
                 flags, CS, IP-past-the-INT, on the GUEST's stack -- and
                 let its IRET decide where it goes. Every host-side
                 mechanism we have (BOP + trampoline, or a service arm
                 that IRETs) throws that adjustment away.
               ⚠ IF stays as the guest had it. An INT gate would clear
                 it, but the emulator is not an ISR: it runs as part of
                 the guest's own instruction stream, and this host's
                 V86/VME rules make silently clearing IF a real hazard
                 (see [[vme-vif-interrupt-gating]]). TF is cleared,
                 which is what a gate does and costs nothing. */
            if (floatingPointHooked) {
                DWORD stackBase   = DpmiSelectorBase(frame[DPMI_FRAME_SS]);
                INT   ss32 = guestSsIs32;
                DWORD stackPointer   = ss32 ? faultEsp : (DWORD)frame[DPMI_FRAME_SP];   /* full width, see guestEspOk */
                stackPointer = ss32 ? stackPointer - X86_WORD_SIZE : ((stackPointer - X86_WORD_SIZE) & WORD_MASK);
                PokeWord(stackBase + stackPointer, frame[DPMI_FRAME_FLAGS]);                    /* FLAGS      */
                stackPointer = ss32 ? stackPointer - X86_WORD_SIZE : ((stackPointer - X86_WORD_SIZE) & WORD_MASK);
                PokeWord(stackBase + stackPointer, frame[DPMI_FRAME_CS]);                    /* return CS  */
                stackPointer = ss32 ? stackPointer - X86_WORD_SIZE : ((stackPointer - X86_WORD_SIZE) & WORD_MASK);
                PokeWord(stackBase + stackPointer, (WORD)(frame[DPMI_FRAME_IP] + X86_INT_LENGTH));        /* return IP  */
                VDM_REG(tib, VTIB_ESP) = stackPointer;
                VDM_SET16(tib, VTIB_CS, g_PmInt[gateVector].Selector);
                VDM_REG(tib, VTIB_EIP) = g_PmInt[gateVector].Offset & WORD_MASK;
                VDM_REG(tib, VTIB_EFLAGS) &= ~EFLAGS_TF_U;     /* TF */
                { *cursorIo = cursor; return HOST_FLOW_CONTINUE; }
            }
            if (!isFloatingPointVector && HostWritable((VOID *)(ULONG_PTR)guestInstruction, X86_INT_LENGTH)) {
                guestInstruction[0] = VDM_BOP0; guestInstruction[1] = VDM_BOP1;
                PatchMapSet(guestLinear, (BYTE)gateVector);   /* the REAL site (s74) */
            }
            guestResult = DpmiServicePmInt(machine, tib, gateVector, steps);
            if (guestResult > 0) { *cursorIo = cursor; return HOST_FLOW_CONTINUE; }      /* serviced -> keep running */
            if (guestResult == 0) { g_DpmiDone = 1; }
            { *cursorIo = cursor; return HOST_FLOW_BREAK; }
        }
    }
    *cursorIo = cursor; return HOST_FLOW_NEXT;
}


/* Deliver a PM fault to the exception handler the client registered (INT 31h 0203h): build its exception frame -- in the client's width, not the handler selector's -- and re-enter the client there. */
static INT DpmiDeliverToClientHandler(PSTR *cursorIo, PSTR const base, const DWORD esp, const DWORD stackBase, volatile WORD * const frame, volatile BYTE * const tib, const INT exception)
{
    PSTR cursor = *cursorIo;
    /* ── ★★★★★ THE EXCEPTION FRAME'S WIDTH FOLLOWS THE CLIENT'S
         MODE, NOT THE HANDLER SELECTOR'S D BIT. This is the same
         rule DpmiDispatchToPmHandler() already documents for
         INTERRUPT frames, and it was never applied here -- so a
         32-bit client got a 16-bit exception frame and read its
         fields off the end of it.
       ► MEASURED, ZAR (GH #23). NT builds a SIXTEEN-BIT frame on
         the fault stack (8 words; the bytes are only coherent read
         that way). DOS/4GW declares itself 32-bit at the mode
         switch, so its #GP handler (entered at 0x0f:0x6abb) reads
         the faulting EIP and CS at frame +0x0C and +0x10 -- the DPMI
         32-BIT frame's slots (observed in its fault registers). In
         our 16-byte frame there is nothing at +0x10, so DS loaded
         ZERO and the handler faulted on its own first memory read --
         which re-entered it, for ever, until the log capped.
       ► AND THE RETURN CONFIRMS IT INDEPENDENTLY: the handler leaves
         with a 32-bit far return, popping EIGHT bytes -- the same
         tell (as with IRETD) that identified the interrupt-frame
         case.
       ► WHY THIS IS A REBUILD AND NOT A WIDER READ: the frame is the
         KERNEL'S, and it is 16-bit whatever the client is. So the
         32-bit frame is built BELOW NT's (which is left intact, so
         the logging above still reads the kernel's own values) and
         ESP is moved onto it.
       ⚠ NT's fields ARE 16-BIT, so a fault in 32-bit code with an
         EIP above 0xFFFF would arrive here already truncated by the
         kernel -- this widens the frame, it cannot recover bits that
         were never handed to us. Both of ZAR's faults are in 16-bit
         DOS/4GW selectors (0x1a7 and 0x0f, both D/B=0) where the
         question does not arise; a 32-bit-CS fault that resumes
         wrongly should suspect this line first. */
    if (g_DpmiIsClient32) {
        DWORD newSp = (esp - DPMI_FRAME32_SIZE) & WORD_MASK;
        volatile DWORD *d32 =
            (volatile DWORD *)(ULONG_PTR)(stackBase + newSp);
        if (HostReadable((const VOID *)d32, DPMI_FRAME32_SIZE)) {
            d32[DPMI_FRAME_RETURN_IP] = (DWORD)DPMI_FLTRET_COFF;  /* return EIP */
            d32[DPMI_FRAME_RETURN_CS] = g_DpmiFaultCodeSelector;      /* return CS  */
            d32[DPMI_FRAME_ERROR] = frame[DPMI_FRAME_ERROR];                    /* error code */
            d32[DPMI_FRAME_IP] = frame[DPMI_FRAME_IP];                    /* fault EIP  */
            d32[DPMI_FRAME_CS] = frame[DPMI_FRAME_CS];                    /* fault CS   */
            d32[DPMI_FRAME_FLAGS] = frame[DPMI_FRAME_FLAGS];                    /* EFLAGS     */
            d32[DPMI_FRAME_SP] = frame[DPMI_FRAME_SP];                    /* fault ESP  */
            d32[DPMI_FRAME_SS] = frame[DPMI_FRAME_SS];                    /* fault SS   */
            VDM_REG(tib, VTIB_ESP) =
                (VDM_REG(tib, VTIB_ESP) & HIGH_WORD_MASK_U) | newSp;
        } else {
            cursor = LogPut(cursor, "  EXC: !! 32-bit frame site unreadable at 0x");
            cursor = LogHex(cursor, stackBase + newSp);
            cursor = LogPut(cursor, " -- delivering the KERNEL'S 16-bit frame, which"
                        " a 32-bit client will misread\r\n");
            frame[DPMI_FRAME_RETURN_IP] = (WORD)DPMI_FLTRET_COFF;
            frame[DPMI_FRAME_RETURN_CS] = g_DpmiFaultCodeSelector;
        }
    } else {
        frame[DPMI_FRAME_RETURN_IP] = (WORD)DPMI_FLTRET_COFF;      /* return IP */
        frame[DPMI_FRAME_RETURN_CS] = g_DpmiFaultCodeSelector;         /* return CS */
    }
    VDM_SET16(tib, VTIB_CS,  g_PmException[exception].Selector);
    VDM_REG(tib, VTIB_EIP) = g_PmException[exception].Offset;
    cursor = LogPut(cursor, "  EXC: -> client handler for 0x"); cursor = LogHex(cursor, (DWORD)exception);
    cursor = LogPut(cursor, " at 0x"); cursor = LogHex(cursor, g_PmException[exception].Selector);
    cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, g_PmException[exception].Offset);
    cursor = LogPut(cursor, " frame{err=0x"); cursor = LogHex(cursor, frame[DPMI_FRAME_ERROR]);
    cursor = LogPut(cursor, " cs:ip=0x"); cursor = LogHex(cursor, frame[DPMI_FRAME_CS]);
    cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, frame[DPMI_FRAME_IP]);
    cursor = LogPut(cursor, " fl=0x"); cursor = LogHex(cursor, frame[DPMI_FRAME_FLAGS]);
    cursor = LogPut(cursor, " ss:sp=0x"); cursor = LogHex(cursor, frame[DPMI_FRAME_SS]);
    cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, frame[DPMI_FRAME_SP]);
    cursor = LogPut(cursor, "} retf-> 0x"); cursor = LogHex(cursor, g_DpmiFaultCodeSelector);
    cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, (DWORD)DPMI_FLTRET_COFF);
    /* ── ★ WHAT THE FAULTING INSTRUCTION WAS LOOKING AT. ─────
         The frame says WHERE it faulted; on a #GP that is half
         the question, because the other half is always "which
         selector, and did the offset fit inside it". Session 34
         spent a run on a krnl386 #GP through ES unable to
         say whether ES was the wrong selector or the right one
         with too small a limit -- from a log that had already
         printed the address. The reflect leaves the guest's GPRs
         and DS/ES alone (only CS/SS/ESP move), so this is free.
         Print the bytes at the fault too: a fault you can decode
         is a fault you can attribute without a second run. */
    cursor = LogPut(cursor, "\r\n       fault regs: eax=0x"); cursor = LogHex(cursor, VDM_REG(tib, VTIB_EAX));
    cursor = LogPut(cursor, " ebx=0x"); cursor = LogHex(cursor, VDM_REG(tib, VTIB_EBX));
    cursor = LogPut(cursor, " ecx=0x"); cursor = LogHex(cursor, VDM_REG(tib, VTIB_ECX));
    cursor = LogPut(cursor, " edx=0x"); cursor = LogHex(cursor, VDM_REG(tib, VTIB_EDX));
    cursor = LogPut(cursor, " esi=0x"); cursor = LogHex(cursor, VDM_REG(tib, VTIB_ESI));
    cursor = LogPut(cursor, " edi=0x"); cursor = LogHex(cursor, VDM_REG(tib, VTIB_EDI));
    cursor = LogPut(cursor, " ebp=0x"); cursor = LogHex(cursor, VDM_REG(tib, VTIB_EBP));
    { WORD selectors[2]; PCSTR registerNames[2] = { " ds", " es" }; INT selectorIndex;
      selectors[0] = (WORD)VDM_REG16(tib, VTIB_DS);
      selectors[1] = (WORD)VDM_REG16(tib, VTIB_ES);
      for (selectorIndex = 0; selectorIndex < 2; ++selectorIndex) {
          UINT32 accessRights = 0, descriptorLimit = 0;
          cursor = LogPut(cursor, registerNames[selectorIndex]); cursor = LogPut(cursor, "=0x"); cursor = LogHex(cursor, selectors[selectorIndex]);
          if (DpmiSelectorDescriptor(selectors[selectorIndex], &accessRights, &descriptorLimit)) {
              cursor = LogPut(cursor, "{base=0x"); cursor = LogHex(cursor, DpmiSelectorBase(selectors[selectorIndex]));
              cursor = LogPut(cursor, " lim=0x"); cursor = LogHex(cursor, descriptorLimit);
              cursor = LogPut(cursor, " ar=0x"); cursor = LogHex(cursor, accessRights); cursor = LogPut(cursor, "}");
          } else cursor = LogPut(cursor, "{NO DESCRIPTOR}");
      } }
    /* ── AND WHAT ES POINTS AT. On a #GP through a selector,
         the object is the evidence. Session 34's fault reads
         `es:[0x28]` -- the in-memory module database's
         ne_modtab -- and got 0x38a where the layout says 0x7c;
         whether that database is malformed or simply is not at
         ES cannot be decided from a register dump, only from
         the bytes. 0x40 of them is the whole NE header. */
    { DWORD extraBase = DpmiSelectorBase((WORD)VDM_REG16(tib, VTIB_ES));
      const volatile BYTE *extraOrigin = (const volatile BYTE *)(ULONG_PTR)extraBase;
      cursor = LogPut(cursor, "\r\n       @es:0000 = ");
      if (extraBase && HostReadable((const VOID *)extraOrigin, 0x40))
           cursor = LogDump(cursor, (const VOID *)extraOrigin, 0x40);
      else cursor = LogPut(cursor, "<unreadable>"); }
    /* ── ★ AND THE SAME FOR DS, for the same reason. ES is dumped
         because a #GP is usually ABOUT a selector; DS is dumped
         because the bottom of a guest's data segment is where its
         own state lives, and "which branch did it take, and on
         what" is answerable from those bytes when it is not
         answerable from the registers. (ZAR, GH #23: DOS/16M keeps
         its memory-manager INTERRUPT VECTOR at ds:0x34 -- 0x15
         means "size memory with INT 15h AH=88h", anything else
         means "use my own manager", and the second path is the one
         that faults. One dump says which we are on.) */
    { DWORD dataBase = DpmiSelectorBase((WORD)VDM_REG16(tib, VTIB_DS));
      const volatile BYTE *dataOrigin = (const volatile BYTE *)(ULONG_PTR)dataBase;
      cursor = LogPut(cursor, "\r\n       @ds:0000 = ");
      if (dataBase && HostReadable((const VOID *)dataOrigin, 0x40))
           cursor = LogDump(cursor, (const VOID *)dataOrigin, 0x40);
      else cursor = LogPut(cursor, "<unreadable>");
      /* The named offsets from dsprobe.txt -- see the knob's
         note. Four bytes each, so a WORD and the word after it
         (a far pointer's two halves) both read in one line. */
      if (g_DsProbeCount && dataBase) {
          INT dataProbeIndex;
          cursor = LogPut(cursor, "\r\n       dsprobe:");
          for (dataProbeIndex = 0; dataProbeIndex < g_DsProbeCount; ++dataProbeIndex) {
              const volatile BYTE *dataProbeBytes = dataOrigin + g_DsProbe[dataProbeIndex];
              cursor = LogPut(cursor, " ds[0x"); cursor = LogHex(cursor, g_DsProbe[dataProbeIndex]);
              cursor = LogPut(cursor, "]=");
              if (HostReadable((const VOID *)dataProbeBytes, 4))
                   cursor = LogDump(cursor, (const VOID *)dataProbeBytes, 4);
              else cursor = LogPut(cursor, "?? ");
          }
      }
      /* csprobe: the same against the FAULTING CODE SEGMENT, six
         bytes each -- enough for `e8 rel16` plus what follows, which
         is the shape being checked against the file on disk. */
      if (g_CsProbeCount) {
          DWORD codeBase2 = DpmiSelectorBase(frame[DPMI_FRAME_CS]);
          const volatile BYTE *codeOrigin =
              (const volatile BYTE *)(ULONG_PTR)codeBase2;
          INT codeProbeIndex;
          cursor = LogPut(cursor, "\r\n       csprobe:");
          for (codeProbeIndex = 0; codeProbeIndex < g_CsProbeCount; ++codeProbeIndex) {
              const volatile BYTE *codeProbeBytes = codeOrigin + g_CsProbe[codeProbeIndex];
              cursor = LogPut(cursor, " cs[0x"); cursor = LogHex(cursor, g_CsProbe[codeProbeIndex]);
              cursor = LogPut(cursor, "]=");
              if (codeBase2 && HostReadable((const VOID *)codeProbeBytes, 6))
                   cursor = LogDump(cursor, (const VOID *)codeProbeBytes, 6);
              else cursor = LogPut(cursor, "?? ");
          }
      } }
    { DWORD frameCodeBase = DpmiSelectorBase(frame[DPMI_FRAME_CS]);
      const volatile BYTE *fi2 =
          (const volatile BYTE *)(ULONG_PTR)(frameCodeBase + frame[DPMI_FRAME_IP]);
      cursor = LogPut(cursor, " bytes@fault=");
      if (HostReadable((const VOID *)fi2, 8)) cursor = LogDump(cursor, (const VOID *)fi2, 8);
      else                                     cursor = LogPut(cursor, "<unreadable>");
      /* ── ★ THE CODE AROUND THE FAULT, AND THE SELECTOR'S BASE.
           Eight bytes AT the fault identify the instruction; they
           do not identify WHERE IN THE GUEST'S IMAGE it came from,
           and that is the question as soon as you start matching a
           fault against a file on disk. A window either side can be
           searched for in the binary, which either confirms the
           file<->guest mapping or refutes it -- and this
           investigation (GH #23) has now had TWO conclusions rest
           on a mapping derived from a single 8-byte match.
           The base is printed for the same reason: it is what turns
           a selector:offset into the linear address pmbp.txt wants. */
      cursor = LogPut(cursor, "\r\n       csbase=0x"); cursor = LogHex(cursor, frameCodeBase);
      cursor = LogPut(cursor, " code[ip-0x20..ip+0x20]=");
      { const volatile BYTE *codeWindow =
            (const volatile BYTE *)(ULONG_PTR)(frameCodeBase + ((frame[DPMI_FRAME_IP] - 0x20) & WORD_MASK));
        if (frame[DPMI_FRAME_IP] >= 0x20 && HostReadable((const VOID *)codeWindow, 0x40))
             cursor = LogDump(cursor, (const VOID *)codeWindow, 0x40);
        else cursor = LogPut(cursor, "<unreadable>"); } }
    /* ── ★ AND WHO CALLED. The frame says WHERE it faulted; on a
         #GP inside a subroutine that is only half the question,
         because the other half is always "how did the guest get
         here" -- and a routine that faults on its FIRST memory
         access has usually been entered in the wrong state rather
         than gone wrong on its own. The return address is sitting
         on the faulting stack, a few words up from SS:SP, and it
         costs one dump to have it instead of a second run and a
         breakpoint. (ZAR, GH #23: `push 8 / pop es` in DOS/16M.) */
    { DWORD sb2 = DpmiSelectorBase(frame[DPMI_FRAME_SS]);
      const volatile BYTE *stackBytes2 =
          (const volatile BYTE *)(ULONG_PTR)(sb2 + frame[DPMI_FRAME_SP]);
      cursor = LogPut(cursor, "\r\n       @ss:sp = ");
      if (sb2 && HostReadable((const VOID *)stackBytes2, 0x20))
           cursor = LogDump(cursor, (const VOID *)stackBytes2, 0x20);
      else cursor = LogPut(cursor, "<unreadable>"); }
    cursor = LogPut(cursor, "\r\n");
    LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
    { *cursorIo = cursor; return HOST_FLOW_CONTINUE; }                 /* re-arm + re-enter, now in the handler */
    *cursorIo = cursor; return HOST_FLOW_NEXT;
}


/* The client's exception handler returned through our fault-return address: take IP, CS, FLAGS, SP and SS from the frame it left, drop the error code, and resume the client there. */
static INT DpmiResumeAfterClientHandler(PSTR *cursorIo, PSTR const base, const DWORD event, const DWORD currentCs, const DWORD eip, volatile BYTE * const tib)
{
    PSTR cursor = *cursorIo;
    /* ── The client's exception handler has finished: its `retf` landed here. ──
         DPMI 0.9: the handler returns through the CS:IP at the bottom of the
         frame, having optionally rewritten the CS:IP / FLAGS / SS:SP above it
         to say where execution should resume. krnl386's handler does exactly
         that (observed) -- it points the resume elsewhere rather than back at its
         own invalid opcode, which is the whole reason it raised one. After the
         `retf` popped two words, SS:SP is at the error code, so what is left is
             +0x00 error code   +0x02 IP   +0x04 CS
             +0x06 FLAGS        +0x08 SP   +0x0a SS
         and the resume is: take those, drop the error code, run.
       ⚠ FLAGS IS SIXTEEN BITS. Assigning it to EFLAGS whole would clear the
         high half -- VM, IOPL's neighbours, the lot -- so merge it. */
    if (event == VDM_EVENT_BOP && currentCs == (g_DpmiFaultCodeSelector & WORD_MASK)
        && eip == DPMI_FLTRET_COFF) {
        DWORD stackBase  = DpmiSelectorBase(g_DpmiFaultSelector);
        DWORD esp = VDM_REG16(tib, VTIB_ESP);
        volatile WORD *frame = (volatile WORD *)(ULONG_PTR)(stackBase + esp);
        if (!HostReadable((const VOID *)frame, DPMI_RETURNED16_SIZE)) {
            cursor = LogPut(cursor, "GH#128: EXC RETURN but the frame at SS:SP is unreadable "
                        "-- stopping\r\n");
            LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
            { *cursorIo = cursor; return HOST_FLOW_BREAK; }
        }
        /* ── ★ AND THE RETURN IS THE SAME QUESTION, SO IT GETS THE SAME
             ANSWER. A 32-bit client's handler leaves by RETFD, which pops
             EIGHT bytes, so ESP lands on the error code of a DWORD frame:
                 +0x00 err  +0x04 EIP  +0x08 CS
                 +0x0c EFLAGS  +0x10 ESP  +0x14 SS
             Reading that as words would take the resume address from the
             top half of the error code.
           ► THE FRAME IS READ BACK, NOT REMEMBERED, because rewriting it is
             the handler's documented lever: DOS/4GW's #GP handler resumes AT
             THE FAULTING INSTRUCTION having stored 0 over the bad selector on
             the faulting stack (it takes that stack from +0x18/+0x1c, which
             only exist in the 32-bit frame), so the `pop es` re-executes and
             succeeds. Resuming from anywhere we cached would defeat it. */
        if (g_DpmiIsClient32) {
            volatile DWORD *d32 = (volatile DWORD *)(ULONG_PTR)(stackBase + esp);
            if (!HostReadable((const VOID *)d32, DPMI_RETURNED32_SIZE)) {
                cursor = LogPut(cursor, "GH#128: EXC RETURN (32) but the frame at SS:ESP is "
                            "unreadable -- stopping\r\n");
                LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
                { *cursorIo = cursor; return HOST_FLOW_BREAK; }
            }
            cursor = LogPut(cursor, "GH#128: EXC RETURN(32) -> resume 0x"); cursor = LogHex(cursor, d32[DPMI_RETURNED_CS]);
            cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, d32[DPMI_RETURNED_IP]);
            cursor = LogPut(cursor, " fl=0x"); cursor = LogHex(cursor, d32[DPMI_RETURNED_FLAGS]);
            cursor = LogPut(cursor, " ss:esp=0x"); cursor = LogHex(cursor, d32[DPMI_RETURNED_SS]);
            cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, d32[DPMI_RETURNED_SP]);
            cursor = LogPut(cursor, " err=0x"); cursor = LogHex(cursor, d32[DPMI_RETURNED_ERROR]);
            cursor = LogPut(cursor, "\r\n");
            LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
            VDM_SET16(tib, VTIB_SS,  (WORD)d32[DPMI_RETURNED_SS]);
            VDM_REG(tib, VTIB_ESP) = d32[DPMI_RETURNED_SP];
            VDM_SET16(tib, VTIB_CS,  (WORD)d32[DPMI_RETURNED_CS]);
            VDM_REG(tib, VTIB_EIP) = d32[DPMI_RETURNED_IP];
            /* ⚠ FLAGS: still merged as sixteen bits. The high half carries VM
                 and IOPL's neighbours, and this frame's EFLAGS came from a
                 kernel frame that only ever held a word. */
            VDM_SET16(tib, VTIB_EFLAGS, (WORD)d32[DPMI_RETURNED_FLAGS]);
            { *cursorIo = cursor; return HOST_FLOW_CONTINUE; }
        }
        cursor = LogPut(cursor, "GH#128: EXC RETURN -> resume 0x"); cursor = LogHex(cursor, frame[DPMI_RETURNED_CS]);
        cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, frame[DPMI_RETURNED_IP]);
        cursor = LogPut(cursor, " fl=0x"); cursor = LogHex(cursor, frame[DPMI_RETURNED_FLAGS]);
        cursor = LogPut(cursor, " ss:sp=0x"); cursor = LogHex(cursor, frame[DPMI_RETURNED_SS]);
        cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, frame[DPMI_RETURNED_SP]);
        cursor = LogPut(cursor, " err=0x"); cursor = LogHex(cursor, frame[DPMI_RETURNED_ERROR]);
        cursor = LogPut(cursor, "\r\n");
        LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
        VDM_SET16(tib, VTIB_SS,  frame[DPMI_RETURNED_SS]); VDM_REG(tib, VTIB_ESP) = frame[DPMI_RETURNED_SP];
        VDM_SET16(tib, VTIB_CS,  frame[DPMI_RETURNED_CS]); VDM_REG(tib, VTIB_EIP) = frame[DPMI_RETURNED_IP];
        VDM_SET16(tib, VTIB_EFLAGS, frame[DPMI_RETURNED_FLAGS]);
        { *cursorIo = cursor; return HOST_FLOW_CONTINUE; }
    }
    *cursorIo = cursor; return HOST_FLOW_NEXT;
}


/* A PM fault the kernel reflected to our handler code selector (GH #18): decode it and report it, then service an unserviced INT nn, deliver the fault to the client's own handler, or end the run. */
static INT DpmiHandleReflectedFault(PSTR *cursorIo, PSTR const base, const DWORD event, const DWORD currentCs, const DWORD eip, volatile BYTE * const tib, DOS_MACHINE *machine, const UINT steps)
{
    PSTR cursor = *cursorIo;
    /* GH #18: the raw-PM-#GP reflect landed on our handler code selector. THIS
       is the proof point: the kernel reflected a fault it used to swallow. */
    if (event == VDM_EVENT_BOP && currentCs == (g_DpmiFaultCodeSelector & WORD_MASK)
        && (eip == DPMI_FAULT_COFF
            || (eip >= DPMI_FAULT_SITE(0)
                && eip <  DPMI_FAULT_SITE(DOS_FLTSITE_N)
                && ((eip - DPMI_FAULT_SITE(0)) & (DOS_FLTSITE_SIZE - 1)) == 0))) {
        /* Which class the kernel dispatched through -- the site it landed on
           names it. -1 = the legacy shared site, which now means "a class we
           did not fill", not "we do not know". */
        INT faultClass = (eip == DPMI_FAULT_COFF)
                   ? -1 : (INT)((eip - DPMI_FAULT_SITE(0)) / DOS_FLTSITE_SIZE);
        DWORD faultSs  = *(volatile WORD  *)(tib + VTIB_FLT_SAVCS);
        DWORD faultEsp = *(volatile DWORD *)(tib + VTIB_FLT_SAVEIP);
        DWORD faultEip = *(volatile DWORD *)(tib + VTIB_FLT_SAV3);
        /* ⚠ THESE TWO FIELDS ARE **NOT** CS:EIP, WHATEVER THEIR NAMES SAY.
             Measured (session 19): the pair reads 0x00c7:0x...6f1e while the
             guest's CS was 0x6f and its SS:ESP was 0x00c7:0x...6f14 -- i.e.
             SS and ESP+0xa. Proved by accident and decisively: clearing the
             junk top half of ESP changed this "EIP" from 0xb33b6f1e to
             0x00006f1e. A field that tracks ESP is not EIP. The raw window
             below shows the layout; `sav3` (tib+0x640, 0x36af here) is the
             likelier faulting EIP. Do not resume anything on faultSs:faultEsp until
             the slots are calibrated against a fault at a KNOWN address --
             pmfault's HLT/INT3 cannot do it (they die without reflecting),
             so that needs a new variant that loads a bad selector. */
        cursor = LogPut(cursor, "GH#18: PM-FAULT REFLECTED class=");
        if (faultClass < 0) cursor = LogPut(cursor, "SHARED-SITE");
        else            cursor = LogHex(cursor, (DWORD)faultClass);
        cursor = LogPut(cursor, " -- savSS:savESP=0x");
        cursor = LogHex(cursor, faultSs); cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, faultEsp);
        cursor = LogPut(cursor, " (MISNAMED VTIB_FLT_SAVCS/SAVEIP) sav3=0x"); cursor = LogHex(cursor, faultEip);
        cursor = LogPut(cursor, " nest=0x"); cursor = LogHex(cursor, *(volatile WORD *)(tib + VTIB_FLT_NEST));
        /* ── ★ THE KERNEL BUILDS A FRAME AND WE HAVE NEVER LOOKED AT IT. ──
             The reflect sets SS:ESP = [TIB+0x638]:0x1000 and then PUSHES:
             session 33's run came out of it with liveESP = ...0x0fd0, i.e.
             0x30 bytes below the top. That frame is the only place the
             faulting CS, the flags and the trap/error code can be -- the
             VTIB_FLT_SAV* slots hold three values and we need six.
           ⚠ THIS IS A DUMP, NOT A DECODE. Nothing here claims to know the
             layout. It is printed against a fault whose every field is
             already known independently -- krnl386's deliberate `0f ff`
             (UD0) at CS:IP 0x01cf:0xc5f0, SS:SP=0x001f:0x0fea, #UD =
             DPMI exception 6 -- so each slot can be identified by the value
             in it rather than by a guess about the shape. Offsets are
             printed with the dwords for exactly that reason: a dump whose
             columns you have to count is half an instrument.
             Read it, THEN write the decode. */
        { DWORD faultStackBase = (DWORD)(ULONG_PTR)g_FaultStack;
          const volatile DWORD *frame = (const volatile DWORD *)(ULONG_PTR)(faultStackBase + 0x0FC0);
          INT frameIndex;
          cursor = LogPut(cursor, "\r\n  FLTSTK sel=0x"); cursor = LogHex(cursor, g_DpmiFaultSelector);
          cursor = LogPut(cursor, " lin=0x"); cursor = LogHex(cursor, faultStackBase);
          cursor = LogPut(cursor, " top=0x1000 espNOW=0x");
          cursor = LogHex(cursor, VDM_REG(tib, VTIB_ESP));
          if (!HostReadable((const VOID *)frame, 0x40)) cursor = LogPut(cursor, " <unreadable>");
          else for (frameIndex = 0; frameIndex < 16; ++frameIndex) {
              cursor = LogPut(cursor, "\r\n    +0x"); cursor = LogHex(cursor, 0x0FC0 + frameIndex * 4);
              cursor = LogPut(cursor, " = 0x"); cursor = LogHex(cursor, frame[frameIndex]);
          }
          cursor = LogPut(cursor, "\r\n "); }
        /* ► READ THE LAYOUT OFF THE TIB INSTEAD OF TRUSTING THE OFFSETS.
             VTIB_FLT_SAVCS/SAVEIP were reverse-engineered in an earlier
             session, and what they return does not look like a code
             address: the argument-run fault reported CS=0x00c7 -- which is
             the guest's SS, not its CS (0x6f) -- and EIP=0xb33b6f1e, which
             is that run's ESP (0xb33b6f14) plus 0xa. Dump the window so the
             real slots can be identified by looking for the KNOWN CS and a
             plausible EIP, rather than by guessing a frame shape. */
        { const BYTE *frameWords = (const BYTE *)(ULONG_PTR)(tib + 0x630);
          cursor = LogPut(cursor, " tib[630..64f]=");
          if (!HostReadable(frameWords, 0x20)) cursor = LogPut(cursor, "<unreadable>");
          else                          cursor = LogDump(cursor, frameWords, 0x20); }
        cursor = LogPut(cursor, " liveCS=0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_CS));
        cursor = LogPut(cursor, " liveSS=0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_SS));
        cursor = LogPut(cursor, " liveESP=0x"); cursor = LogHex(cursor, VDM_REG(tib, VTIB_ESP));
        cursor = LogPut(cursor, " -- REAL-CPU PM fault reflect WORKS\r\n");
        LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
        /* ── ★★ DELIVER IT TO THE CLIENT'S INT 31h 0203 HANDLER. ────────────
             This `break` used to end the run here, which is why "DOS/4GW quits
             on a command-line argument": ANY real PM fault produced a tidy
             `exec loop exited -> flushing` that read exactly like the client
             choosing to exit. It was not quitting; we were stopping it.

           ★ THE FRAME IS ALREADY BUILT, AND NOT BY US. Measured on the rig
             (session 34, against krnl386's deliberate `0f ff` at 0x01cf:0xc5f0
             whose every field was known in advance): the kernel switches to
             [TIB+0x638]:0x1000, pushes 0x10 bytes, and what it pushes IS the
             DPMI 0.9 16-bit exception frame --

               SS:SP+0x00  return IP   } LEFT ZERO for the host to fill
               SS:SP+0x02  return CS   }
               SS:SP+0x04  error code        0000
               SS:SP+0x06  faulting IP       c5f0   <- the UD0
               SS:SP+0x08  faulting CS       01cf
               SS:SP+0x0a  FLAGS             3246
               SS:SP+0x0c  faulting SP       0fea
               SS:SP+0x0e  faulting SS       001f

             -- which is not a coincidence: this machinery exists in NT FOR
             ntvdm's DPMI, so it emits the shape DPMI specifies. krnl386's own
             handler confirms the layout independently (observed): it rewrites
             exactly the faulting IP and CS slots with a resume address and
             leaves by `retf`, as DPMI 0.9 prescribes.
             ⇒ So delivery is: fill the two return words with a BOP of ours,
               point CS:EIP at the registered handler, and leave the kernel's
               SS:ESP alone. The handler runs on the host stack the kernel
               chose, and its `retf` comes back to us at DPMI_FLTRET_COFF.

           ⚠ THE EXCEPTION NUMBER IS THE TABLE INDEX, AND THAT IS A READING,
             NOT A MEASUREMENT. The frame carries no trap number; the only
             channel is which entry of g_FaultTable the kernel dispatched
             through. #UD (vector 6) arrived at index 6 -- consistent with
             "index == vector", and equally consistent with "index == an NT
             fault class that happens to cover #UD as well as the #GP this
             table was first built for". Hence the guard below: deliver ONLY
             to an exception the client has actually registered a handler
             for. krnl386 registers one at a time and faults immediately, so
             a wrong reading stops the run with a line that says which index
             had no handler -- it does not call the wrong handler. */
        {
            INT exception = faultClass;
            DWORD stackBase  = DpmiSelectorBase(g_DpmiFaultSelector);
            DWORD esp = VDM_REG16(tib, VTIB_ESP);
            volatile WORD *frame = (volatile WORD *)(ULONG_PTR)(stackBase + esp);
            /* ── ★★ A #GP THROUGH AN IDT GATE IS AN UNSERVICED `INT nn`. ────
                 Not an exception the client asked for -- a software interrupt
                 the host failed to intercept. The #GP error code says so
                 exactly: bit 1 (IDT) set and bits 3..15 the vector.

               ⚠ WHY PATCHING ON COMMIT CANNOT COVER THIS. Our INT->BOP scan
                 runs when the client declares a region CODE, and krnl386
                 declares it BEFORE it copies the code in: measured, `04F2 ...
                 installed idx 1 base=0x03b30000 acc=0xfb` is preceded by
                 `code region 0x03b30000..0x03b3045f -> patched 00000000 INT
                 sites` -- the region was empty at declaration time. It then
                 copies SYSTEM.DRV in and executes a raw `cd 21` at
                 0x000f:0x01f7. No commit-time hook can see content that has
                 not arrived, and session 33 hit the mirror image of this
                 (krnl386 copying code we HAD patched, carrying the `C4 C4`
                 but leaving the address-keyed vector behind).

               ⇒ So service it HERE, where the CPU has just told us both the
                 vector and the address, and confirm against the instruction
                 bytes before believing the error code. This retires the whole
                 class: any `CD nn` in protected mode, in any code we never
                 scanned, becomes serviceable instead of fatal. It is also what
                 a DPMI host is supposed to do -- reflect the interrupt -- and
                 it is strictly better than a scan, which can only ever cover
                 memory it was pointed at while the content was there.
                 The site is patched on the way past, so the second execution
                 takes the fast BOP path: `C4 C4` in place (same length, as
                 always) plus the vector in the address-keyed map. That patch
                 needs no length heuristic -- the CPU just executed these two
                 bytes AS an interrupt, which is the strongest evidence the
                 x86len vote was ever trying to approximate. */
            {
                INT flow = DpmiServiceIdtGateFault(&cursor, base, frame, faultEsp, faultSs, faultEip, tib, machine, steps);
                if (flow == HOST_FLOW_BREAK) { *cursorIo = cursor; return HOST_FLOW_BREAK; }
                if (flow == HOST_FLOW_CONTINUE) { *cursorIo = cursor; return HOST_FLOW_CONTINUE; }
            }
            /* ── ★★★ (C) THE MACHINE FELL IDLE: START THE PARKED TASK.
                 krnl386's creating task ends itself (observed: its
                 record goes, the current-task word at DGROUP 0x228
                 becomes 0, and it moves to a private kernel stack) --
                 and from then on the machine belongs to the scheduler.
                 The first code to touch the current task then loads
                 that 0 as a selector and reads through it -- a #GP
                 with err=0. That fault is not a defect,
                 it is the cue: nobody is running and somebody is
                 waiting. Resume them instead of reflecting.
               ⚠ THIS TRUNCATES THE CREATOR. It had already retired, but
                 it was still freeing selectors when we took the machine
                 away, and those leak. Logged as the truncation it is,
                 because the honest fix is to park the creator too and
                 give it the rest of its turn later. */
            /* ★ Every PSP we built, and what its environment field
                 holds NOW. Two faults in this epic were that field;
                 printing it at the fault turns "who wrote 1 there"
                 from a reading of candidate code into a reading of
                 the log. */
            if (g_WowPspCount) {
                INT probeIndex3;
                cursor = LogPut(cursor, "  PSPENV:");
                for (probeIndex3 = 0; probeIndex3 < g_WowPspCount; ++probeIndex3) {
                    const volatile BYTE *pspBytes = (const volatile BYTE *)
                        (ULONG_PTR)DpmiSelectorBase(g_WowPspSelector[probeIndex3]);
                    cursor = LogPut(cursor, " sel 0x"); cursor = LogHex(cursor, g_WowPspSelector[probeIndex3]);
                    cursor = LogPut(cursor, "->+0x2c=0x");
                    if (HostReadable((const VOID *)pspBytes, DOS_PSP_ENVIRONMENT + sizeof(WORD)))
                        cursor = LogHex(cursor, (DWORD)(pspBytes[DOS_PSP_ENVIRONMENT] | (pspBytes[DOS_PSP_ENVIRONMENT + 1] << BYTE_SHIFT)));
                    else cursor = LogPut(cursor, "??unreadable");
                }
                cursor = LogPut(cursor, "\r\n");
            }
            INT schedSlot = g_WowSchedOn ? WowSchedPick(0) : -1;
            if (schedSlot >= 0 && WowSchedCurrentTask() == 0) {
                cursor = LogPut(cursor, "  WOWSCHED: [0x228]==0 -- the creator retired "
                            "and task 0x");
                cursor = LogHex(cursor, g_WowSchedSlots[schedSlot].Task);
                cursor = LogPut(cursor, " is parked. Resuming it INSTEAD of reflecting this "
                            "fault; the creator's remaining teardown is "
                            "ABANDONED (selectors leak).\r\n");
                /* ★ VERIFY THE LAUNCH RESULT WE INVENTED AT (A). By now
                     InitTask has NOT run for this task (it runs after we
                     resume), so TDB+0x1c is still zero -- but the moment
                     it is not, this line proves or breaks the SS&~1
                     invariant, and a wrong hInstance is exactly the kind
                     of thing that would otherwise fail three walls later
                     with no trace back to here. */
                {   DWORD taskBase = DpmiSelectorBase(g_WowSchedSlots[schedSlot].Task);
                    if (taskBase) {
                        const volatile BYTE *taskBytes =
                            (const volatile BYTE *)(ULONG_PTR)taskBase;
                        WORD handle = (WORD)(taskBytes[WOW_TDB_INSTANCE] | (taskBytes[WOW_TDB_INSTANCE + 1] << BYTE_SHIFT));
                        cursor = LogPut(cursor, "  WOWSCHED: TDB+0x1c now reads 0x");
                        cursor = LogHex(cursor, handle);
                        cursor = LogPut(cursor, " (0 = InitTask has not run yet)\r\n");
                    }
                }
                LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
                WowSchedPoke(g_WowSchedSlots[schedSlot].ModeLinear, WOW32_MODE_ORDINARY);
                {   WORD toTask = g_WowSchedSlots[schedSlot].Task;
                    INT isTopLevel = WowSchedTopLevel(&g_WowSchedSlots[schedSlot]);
                    if (!g_WowSchedShell) g_WowSchedShell = toTask;   /* s92: WOWEXEC */
                    WowSchedRestore(&g_WowSchedSlots[schedSlot], tib);
                    if (isTopLevel) g_WowSchedCurrentBase = g_WowCallDepth;
                    /* ★ AND PUT THE CURRENT-TASK WORD BACK WITH IT.
                         The creator zeroed it on its way out; the
                         frame we are resuming was parked when it
                         held this task. Restoring registers alone
                         resumed the new task's code with krnl386
                         still believing nobody is current -- see
                         WowSchedSetCurrent for why that is part of the
                         context and not the ruled-out "write
                         [0x228] to yield". */
                    WowSchedSetCurrent(toTask);
                    WowTaskChdir(toTask, &cursor);   /* #164 */
                }
                ++g_WowSchedSwitches;
                { *cursorIo = cursor; return HOST_FLOW_CONTINUE; }
            }
            if (exception < 0 || exception > X86_EXCEPTIONS - 1) {
                cursor = LogPut(cursor, "  EXC: no class (shared site) -- cannot name the "
                            "exception, stopping\r\n");
            } else if (!HostReadable((const VOID *)frame, DPMI_FRAME16_SIZE)) {
                cursor = LogPut(cursor, "  EXC: frame at SS:SP is not readable, stopping\r\n");
            } else if (!g_PmException[exception].IsSet) {
                cursor = LogPut(cursor, "  EXC: exception 0x"); cursor = LogHex(cursor, (DWORD)exception);
                cursor = LogPut(cursor, " has NO client handler (INT 31h 0203 never called "
                            "for it) -- stopping rather than guessing\r\n");
            } else {
                {
                    INT flow = DpmiDeliverToClientHandler(&cursor, base, esp, stackBase, frame, tib, exception);
                    if (flow == HOST_FLOW_CONTINUE) { *cursorIo = cursor; return HOST_FLOW_CONTINUE; }
                }
            }
            LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
        }
        { *cursorIo = cursor; return HOST_FLOW_BREAK; }
    }
    *cursorIo = cursor; return HOST_FLOW_NEXT;
}


/* An async PM interrupt has come back: the client's handler IRETed into our catcher, so put the client back at the place in its own code that the injection interrupted. */
static INT DpmiFinishAsyncPmInterrupt(const DWORD event, const DWORD currentCs, const DWORD eip, volatile BYTE * const tib, DOS_MACHINE *machine, const UINT steps)
{
    /* ── AN ASYNC PM INTERRUPT HAS COME BACK. ────────────────────────────
       DpmiAsyncInjectPm() rewrote the running thread's context to vector
       at the client's handler, with the frame's return CS:EIP pointing at
       our catcher -- so the handler's IRET lands here as a BOP. The IRET
       restored the FLAGS we pushed but NOT the guest's place in its own
       code, because that return address was the catcher's. Put the saved
       context back and let the guest carry on as though nothing happened,
       which is precisely what "transparent" means for a hardware interrupt. */
    if (event == VDM_EVENT_BOP && g_AsyncPmActive
        && currentCs == g_PmReturnSelector && eip == DPMI_PMRET_OFF) {
        VDM_SET16(tib, VTIB_CS, g_AsyncPmCs);
        VDM_REG(tib, VTIB_EIP)    = g_AsyncPmEip;
        VDM_SET16(tib, VTIB_SS, g_AsyncPmSs);
        VDM_REG(tib, VTIB_ESP)    = g_AsyncPmEsp;
        VDM_REG(tib, VTIB_EFLAGS) = g_AsyncPmEflags;
        g_DpmiVi = 1;                   /* unmask: the handler has finished */
        g_AsyncPmActive = 0;
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
        { INT item = -1, gate;
          gate = (g_DpmiIsClient32 && g_PmAppHookedTimer && !g_PmNoIrq
                  && DpmiSelectorIs32((WORD)VDM_REG16(tib, VTIB_CS)));
          UINT32 dmaReadsBeforeBlock = g_Dma.ChannelCountReads[1];
          if (gate) {
            g_InPmIrq = 1;
            /* ► DRAIN WHAT IS OWED, NOT A FIXED SIXTY-FOUR. This loop used to
                 run the full DPMI_IRQ0_BATCH every time it was entered, with
                 nothing tying it to elapsed time -- so the client's clock ran
                 at the rate we happened to return from asynchronous
                 injections rather than the rate it programmed into the 8254.
                 Measured on Doom at 140 Hz: 169,032 ISR entries in 45 s
                 against 6,300 owed, i.e. a game running 27x too fast. The
                 batch is still worth having -- one SuspendThread round trip
                 should repay a whole backlog -- but the backlog is a COUNT,
                 and PmTickTake() is where it lives. */
            /* ► CONSUME A TICK ONLY IF IT WAS ACTUALLY DELIVERED.
                 DpmiInjectPmIrq() declines whenever the guest is in
                 the extender's 16-bit code rather than the application
                 -- a routine and correct refusal -- and taking the tick
                 first threw it away every time that happened. Measured
                 on Doom: 3,349 ISR entries in 45 s against the 6,300 it
                 programmed at 140 Hz, i.e. a game clock running at half
                 speed, which is most of "very laggy". The same mistake
                 as the keyboard's, with the sign reversed: there,
                 consuming late cancelled an interrupt the handler had
                 raised; here, consuming early discarded one nobody ran. */
            for (item = 0; item < DPMI_IRQ0_BATCH; ++item) {
                if (g_PmTickOwed <= 0) break;
                if (!Irq0PmClaim()) break;     /* last tick not EOI'd (#173) */
                if (!DpmiInjectPmIrq(machine, tib, VECTOR_TIMER, steps)) { Irq0PmUnclaim(); break; }
                InterlockedDecrement(&g_PmTickOwed);
            }
            g_InPmIrq = 0;
          }
          g_CooperativeDmaPolls += g_Dma.ChannelCountReads[1] - dmaReadsBeforeBlock;
          { CHAR breakLine[160], *breakCursor = breakLine;
            breakCursor = LogPut(breakCursor, "  BATCH gate="); breakCursor = LogHex(breakCursor, (DWORD)gate);
            breakCursor = LogPut(breakCursor, " k="); breakCursor = LogHex(breakCursor, (DWORD)item);
            breakCursor = LogPut(breakCursor, " c32="); breakCursor = LogHex(breakCursor, (DWORD)g_DpmiIsClient32);
            breakCursor = LogPut(breakCursor, " hooked="); breakCursor = LogHex(breakCursor, (DWORD)g_PmAppHookedTimer);
            breakCursor = LogPut(breakCursor, " cs=0x"); breakCursor = LogHex(breakCursor, VDM_REG16(tib, VTIB_CS));
            breakCursor = LogPut(breakCursor, "\r\n"); LogAppend(LOG_PATH, breakLine, breakCursor); SerialOut(breakLine, breakCursor); } }
        { return HOST_FLOW_CONTINUE; }
    }
    return HOST_FLOW_NEXT;
}


/* Run the client until its next event: under the kernel's VDM monitor (pmkernel.flag), or on the
   real CPU through the host's own protected-mode entry (DpmiEnterProtectedMode), timing the stretch. */
static INT DpmiRunClientSlice(PSTR *cursorIo, PSTR const base, volatile BYTE * const tib, const UINT steps)
{
    PSTR cursor = *cursorIo;
    if (g_DpmiUseKernel) {
        /* Hand the PM CONTEXT to the kernel monitor exactly as the V86
           path does. Same TIB, same call; the only difference is that
           EFLAGS.VM is clear and CS/SS/DS hold LDT selectors. */
        LONG runStatus = 0; DWORD runEvent;
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
        if (!DpmiSelectorIs32((WORD)VDM_REG16(tib, VTIB_SS)))
            VDM_REG(tib, VTIB_ESP) &= WORD_MASK_U;
        if (steps < 400) {      /* BEFORE the call: an entry that never
                                  returns leaves no other trace at all */
            cursor = LogPut(cursor, "PMKERNEL[");   cursor = LogHex(cursor, (UINT)steps);
            cursor = LogPut(cursor, "] enter cs:eip=0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_CS));
            cursor = LogPut(cursor, ":0x");         cursor = LogHex(cursor, VDM_REG(tib, VTIB_EIP));
            cursor = LogPut(cursor, " ss:esp=0x");  cursor = LogHex(cursor, VDM_REG16(tib, VTIB_SS));
            cursor = LogPut(cursor, ":0x");         cursor = LogHex(cursor, VDM_REG(tib, VTIB_ESP));
            cursor = LogPut(cursor, " efl=0x");     cursor = LogHex(cursor, VDM_REG(tib, VTIB_EFLAGS));
            cursor = LogPut(cursor, " msw=0x");     cursor = LogHex(cursor, *(volatile WORD *)(tib + VTIB_MSW));
            cursor = LogPut(cursor, " [0x714]=0x"); cursor = LogHex(cursor, *(volatile DWORD *)(ULONG_PTR)FIXED_NTVDMSTATE_LINEAR);
            cursor = LogPut(cursor, "\r\n");
            LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
        }
        g_PmEntryEip = (LONG)VDM_REG(tib, VTIB_EIP);
        runEvent = VdmRunGuest(tib, &runStatus);
        g_PmEntryEip = -1;
        if (steps < 400) {
            cursor = LogPut(cursor, "PMKERNEL[");     cursor = LogHex(cursor, (UINT)steps);
            cursor = LogPut(cursor, "] VdmStartExecution -> st=0x"); cursor = LogHex(cursor, (DWORD)runStatus);
            cursor = LogPut(cursor, " ev=0x");        cursor = LogHex(cursor, runEvent);
            cursor = LogPut(cursor, " cs:eip=0x");    cursor = LogHex(cursor, VDM_REG16(tib, VTIB_CS));
            cursor = LogPut(cursor, ":0x");           cursor = LogHex(cursor, VDM_REG(tib, VTIB_EIP));
            cursor = LogPut(cursor, " efl=0x");       cursor = LogHex(cursor, VDM_REG(tib, VTIB_EFLAGS));
            cursor = LogPut(cursor, "\r\n");
            LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
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
        LARGE_INTEGER qpcStart, qpcEnd;
        DWORD stormCs = VDM_REG16(tib, VTIB_CS), stormEip = DpmiPmEip(tib);
        /* ── ★★ RE-ARM BEFORE EVERY ENTRY, BECAUSE THE TARGET APPEARS LATE. ──
             DpmiBreakpointArm() skips a site that still reads 00 00 -- correctly,
             since arming into memory the client has not loaded yet was a
             previous session's silent no-op. It is then only retried when a
             code region is patched, and that was enough while the CLIENT's
             own extender declared its modules.
           ⚠ It is NOT enough now. krnl386 LOADS ITS OWN SEGMENTS: it copies
             segment 1 to linear 0x20760 and installs the descriptor through
             04F2, and the copy may land after the last patch pass -- so a
             breakpoint inside it is skipped at setup (still zeroes) and never
             looked at again. Two breakpoints armed inside krnl386's segment 1
             produced no "armed" line and no hit at all, which reads exactly
             like "the guest never got there" and means nothing of the sort.
           Cheap: a handful of entries, and only when a list was loaded. The
             guest is the ONLY thing still running when the process is killed,
             so this is the last instrument standing -- it has to actually arm. */
        if (g_BreakpointCount) DpmiBreakpointArm();
        /* The injections above run the client's own code; if it exited in
           there, there is nothing left to enter. See g_PmClientExited. */
        if (g_PmClientExited) { *cursorIo = cursor; return HOST_FLOW_BREAK; }
        QueryPerformanceCounter(&qpcStart);
        DpmiEnterProtectedMode(tib);
        QueryPerformanceCounter(&qpcEnd);
        {
            DWORD microseconds = QpcMicroseconds(qpcEnd.QuadPart - qpcStart.QuadPart);
            if (microseconds > g_PmStretchMaximumMicroseconds) {
                g_PmStretchMaximumMicroseconds = microseconds;
                if (microseconds >= PM_STRETCH_LOG_US && g_PmStretchLogged++ < 256) {
                    cursor = LogPut(cursor, "PMSTRETCH us="); cursor = LogHex(cursor, microseconds);
                    cursor = LogPut(cursor, " entry=0x"); cursor = LogHex(cursor, stormCs);
                    cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, stormEip);
                    cursor = LogPut(cursor, " lin=0x");
                    cursor = LogHex(cursor, DpmiSelectorBase((WORD)stormCs) + stormEip);
                    cursor = LogPut(cursor, " exit=0x");
                    cursor = LogHex(cursor, VDM_REG16(tib, VTIB_CS));
                    cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, DpmiPmEip(tib));
                    cursor = LogPut(cursor, " ev=0x"); cursor = LogHex(cursor, VDM_REG(tib, VTIB_EVENT));
                    cursor = LogPut(cursor, " ms="); cursor = LogHex(cursor, GetTickCount());
                    cursor = LogPut(cursor, "\r\n");
                    LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
                }
            }
        }
    }
    *cursorIo = cursor; return HOST_FLOW_NEXT;
}


/* Bracket the client's first entries with checkpoints (session 16, Doom): arming the trampoline, entering PM, the first instruction and the return path, so a silent death names its step. */
static PSTR DpmiCheckpointFirstEntries(PSTR cursor, PSTR const base, const UINT steps, volatile BYTE * const tib)
{
    /* ── BRACKET THE FIRST ENTRIES (session 16, Doom) ────────────────────
       Doom's log ends at the steps==0 [0x714] dump above and the process is
       gone, with no fault, no banner and no INT 21h. Between that line and
       the next thing that logs there are FOUR things that can kill us, and
       nothing said which: arming the trampoline, entering PM, the guest's
       first instruction, or the return path. So checkpoint each side for the
       first few iterations -- cheap, self-limiting, and it turns "died
       somewhere in here" into a named step. Bounded to 4096 so a healthy client
       (millions of iterations) pays nothing. */
    if (steps < g_DpmiCpMaximum) {
        /* Dump the descriptor and the actual BYTES we are about to run. Doom
           dies inside the FIRST DpmiEnterProtectedMode and never returns, so the only
           thing that can tell a bad mode switch from a specific offending
           instruction is knowing which instruction it was. Cheap: 4 iterations. */
        DWORD currentCodeBase = DpmiSelectorBase((WORD)g_DpmiEnterCs);
        cursor = LogPut(cursor, "DPMI-CP["); cursor = LogHex(cursor, (UINT)steps);
        cursor = LogPut(cursor, "] pre-arm cs:eip=0x"); cursor = LogHex(cursor, g_DpmiEnterCs);
        cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, g_DpmiEnterEip);
        cursor = LogPut(cursor, " csbase=0x"); cursor = LogHex(cursor, currentCodeBase);
        cursor = LogPut(cursor, " ss:esp=0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_SS));
        cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, VDM_REG(tib, VTIB_ESP));
        cursor = LogPut(cursor, " bytes@cs:eip=");
        /* GUARD THE INSTRUMENT -- with HostReadable(), NOT IsBadReadPtr.
           csbase+eip need not be readable from the host's flat address
           space, and an unguarded read would fault in our own diagnostic
           and destroy the evidence we came for. IsBadReadPtr looks like
           the guard for that but IS the same bug wearing a coat: it faults
           on purpose, and DpmiCrashVeh sees the fault first. */
        { const BYTE *enterBytes = (const BYTE *)(ULONG_PTR)(currentCodeBase + g_DpmiEnterEip);
          if (!HostReadable(enterBytes, 16)) cursor = LogPut(cursor, "<unreadable from host>");
          else                        cursor = LogDump(cursor, enterBytes, 16); }
        cursor = LogPut(cursor, "\r\n");
        LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
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
             the same DpmiSelectorIs32() rule 0204/0205 already use. */
        { WORD ss = (WORD)VDM_REG16(tib, VTIB_SS);
          DWORD stackBase = DpmiSelectorBase(ss);
          DWORD stackPointer = DpmiSelectorIs32(ss) ? VDM_REG(tib, VTIB_ESP)
                                       : VDM_REG16(tib, VTIB_ESP);
          const BYTE *stackDump = (const BYTE *)(ULONG_PTR)(stackBase + stackPointer);
          cursor = LogPut(cursor, "DPMI-CP["); cursor = LogHex(cursor, (UINT)steps);
          cursor = LogPut(cursor, "] regs EAX=0x"); cursor = LogHex(cursor, VDM_REG(tib, VTIB_EAX));
          cursor = LogPut(cursor, " EBX=0x"); cursor = LogHex(cursor, VDM_REG(tib, VTIB_EBX));
          cursor = LogPut(cursor, " ECX=0x"); cursor = LogHex(cursor, VDM_REG(tib, VTIB_ECX));
          cursor = LogPut(cursor, " EDX=0x"); cursor = LogHex(cursor, VDM_REG(tib, VTIB_EDX));
          cursor = LogPut(cursor, " ESI=0x"); cursor = LogHex(cursor, VDM_REG(tib, VTIB_ESI));
          cursor = LogPut(cursor, " EDI=0x"); cursor = LogHex(cursor, VDM_REG(tib, VTIB_EDI));
          cursor = LogPut(cursor, " EBP=0x"); cursor = LogHex(cursor, VDM_REG(tib, VTIB_EBP));
          cursor = LogPut(cursor, " DS=0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_DS));
          cursor = LogPut(cursor, " ES=0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_ES));
          cursor = LogPut(cursor, " FS=0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_FS));
          cursor = LogPut(cursor, " GS=0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_GS));
          cursor = LogPut(cursor, " efl=0x"); cursor = LogHex(cursor, VDM_REG(tib, VTIB_EFLAGS));
          cursor = LogPut(cursor, " ssbase=0x"); cursor = LogHex(cursor, stackBase);
          cursor = LogPut(cursor, " sp=0x"); cursor = LogHex(cursor, stackPointer);
          cursor = LogPut(cursor, " stack@ss:sp=");
          if (!HostReadable(stackDump, 32)) cursor = LogPut(cursor, "<unreadable from host>");
          else                        cursor = LogDump(cursor, stackDump, 32);
          /* ── AND THE FRAME AT SS:BP ──────────────────────────────────
             The client dies in DOS/4GW's HANDOFF to the application,
             just past the last checkpoint: it loads its segment registers
             and an IRET frame from the frame at SS:BP and enters the app
             (observed: every value it loads is a word of that frame, all
             of them selectors or a far entry point). Dumping the frame is
             therefore the whole question: which descriptors it is about to
             load, and where it is about to jump. Without it we would be
             guessing which load faults; with it the answer is a lookup
             against the descriptor calls already in this log. */
          { const BYTE *frame = (const BYTE *)(ULONG_PTR)
                (stackBase + VDM_REG16(tib, VTIB_EBP));
            cursor = LogPut(cursor, " frame@ss:bp=");
            if (!HostReadable(frame, 0x30)) cursor = LogPut(cursor, "<unreadable from host>");
            else {
                cursor = LogDump(cursor, frame, 0x30);
                /* ► AND THE CODE IT IS ABOUT TO JUMP TO. This half IS
                     DOS/4GW-frame-specific and says so: [bp+0x22]:[bp+0x1e]
                     is the far entry the handoff's IRET takes (as observed
                     in the frame dump). The first
                     frame dump proved every descriptor it loads is in range
                     and correctly typed (SS=0xAF lim 0x7cff, SP=0x6F3E;
                     DS/ES=0x17; CS=0x8F lim 0x5e3f, IP=0x2C63) -- so the
                     fault is not the handoff, it is the FIRST INSTRUCTIONS
                     OF THE MODULE, and those live in a block DOS/4GW read
                     out of DOOM.EXE at runtime. They are not at any fixed
                     file offset we can read offline; the only place they
                     exist is guest memory, here, now. Hence the dump.
                     Costs nothing when the frame is not a DOS/4GW one: the
                     selector simply will not resolve to readable memory. */
                { WORD frameCs = *(const WORD *)(frame + 0x22);
                  WORD frameIp = *(const WORD *)(frame + 0x1e);
                  const BYTE *entryBytes = (const BYTE *)(ULONG_PTR)
                      (DpmiSelectorBase(frameCs) + frameIp);
                  cursor = LogPut(cursor, " entry=0x"); cursor = LogHex(cursor, frameCs);
                  cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, frameIp);
                  cursor = LogPut(cursor, " base=0x"); cursor = LogHex(cursor, DpmiSelectorBase(frameCs));
                  cursor = LogPut(cursor, " code@entry=");
                  if (!HostReadable(entryBytes, 64)) cursor = LogPut(cursor, "<unreadable from host>");
                  else                        cursor = LogDump(cursor, entryBytes, 64); } } }
          cursor = LogPut(cursor, "\r\n");
          LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base; }
    }
    return cursor;
}


/* Deliver pending hardware IRQs to the client's PM vectors -- 08h-0Fh for the master PIC, 70h-77h for
   the slave -- in g_IrqOrder's priority order; an IRQ whose vector the client never hooked is dropped. */
static VOID DpmiDeliverPendingIrqs(DOS_MACHINE *machine, volatile BYTE * const tib, const UINT steps)
{
    if (g_DpmiVi && !g_PmNoIrq && !g_InPmIrq && !g_AsyncPmActive) {
        INT scan, item;
        for (item = 0; item < (INT)sizeof g_IrqOrder; ++item) {
            /* DPMI 0.9: hardware interrupts arrive at the PM vectors that
               match the PIC's -- 08h-0Fh for the master, 70h-77h for the
               slave -- which is where a client hooks its IRQ 8-15 handler. */
            UINT interruptVector;
            scan  = g_IrqOrder[item];
            interruptVector = (scan < PIC_LINES_PER_CHIP) ? PIC_MASTER_VECTOR_BASE + (UINT)scan : PIC_SLAVE_VECTOR_BASE + (UINT)(scan - PIC_LINES_PER_CHIP);
            if (!g_IrqNPending[scan]) continue;
            /* No PM handler: the client cannot want it. Drop it rather
               than spin on it forever. */
            if (!g_PmInt[interruptVector].Client) {
                InterlockedExchange(&g_IrqNPending[scan], 0);
                ++g_PmDeviceIrqDrop;
                continue;
            }
            if (!VddPicCanDeliver(&g_Pic, (BYTE)scan)) continue;
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
            /* ── AND THE PIC IS TOLD BEFORE THE HANDLER RUNS, NOT AFTER (#213).
                 The ISR runs inside DpmiInjectPmIrq() and sends its EOI
                 from in there; acknowledging afterwards set the in-service bit
                 AFTER that EOI, so the line stayed in service for good. Until
                 #173 the next timer tick's non-specific EOI happened to clear
                 it (IRQ0 was never in service to take it); once IRQ0 was held
                 properly, ZAR's IRQ5 went dead after its first SB block.
                 Same acknowledge/EOI rule as the async path: in service, and
                 released at once when the vector is still our own stub. */
            UINT32 dmaReadsBeforeDevice = g_Dma.ChannelCountReads[SB_DEFAULT_DMA8];
            InterlockedExchange(&g_IrqNPending[scan], 0);
            if (AsyncVectorIsOurStub((UINT)scan))
                VddPicAcknowledgeAutoEoi(&g_Pic, (BYTE)scan);
            else VddPicAcknowledge(&g_Pic, (BYTE)scan);
            g_InPmIrq = 1;
            if (DpmiInjectPmIrq(machine, tib, interruptVector, steps)) {
                ++g_PmDeviceIrqInjected;
            } else {
                /* No handler ran, so nothing will EOI: hand the line back. */
                VddPicEndOfInterrupt(&g_Pic, (BYTE)scan);
                VddPicRaise(&g_Pic, (BYTE)scan);
                InterlockedExchange(&g_IrqNPending[scan], 1);
                ++g_PmDeviceIrqFail;
            }
            g_InPmIrq = 0;
            g_CooperativeDmaPollsDevice[scan & 7] += g_Dma.ChannelCountReads[SB_DEFAULT_DMA8] - dmaReadsBeforeDevice;
            break;                  /* one per pass: let it IRET first */
        }
    }
}


/* The keyboard's cooperative path, like IRQ0's: a pending IRQ1 (or a scan code waiting in the 8042) is
   delivered between PM steps when the client has hooked it and can take an interrupt now. */
static VOID DpmiDeliverKeyboardIrq(volatile BYTE * const tib, DOS_MACHINE *machine, const UINT steps)
{
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
    if ((g_Irq1Pending > 0 || VddInputScanCodePending(&g_Input))
        && g_PmInt[VECTOR_KEYBOARD].Client && !g_PmNoIrq) {
        if (g_Irq1Pending <= 0) InterlockedIncrement(&g_Irq1Pending);
        INT virtualIf   = g_DpmiVi;
        INT busy = g_InPmIrq || g_AsyncPmActive;
        INT picCanDeliver  = VddPicCanDeliver(&g_Pic, PIC_IRQ_KEYBOARD);
        INT isCode32  = DpmiSelectorIs32((WORD)VDM_REG16(tib, VTIB_CS));
        INT done1 = 0;
        if (virtualIf && !busy && picCanDeliver) {
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
            InterlockedDecrement(&g_Irq1Pending);
            g_InPmIrq = 1;
            done1 = DpmiInjectPmIrq(machine, tib, VECTOR_KEYBOARD, steps);
            g_InPmIrq = 0;
            if (!done1) InterlockedIncrement(&g_Irq1Pending);
        }
        /* ► WHY A HELD-BACK KEY IS HELD BACK. Five separate conditions
             stand between a raised IRQ1 and the client's ISR, and a key
             that never arrives looks the same whichever one said no.
             Bounded, and only while something IS pending, so a run with
             no keyboard activity pays nothing. */
        if (g_KeyPmLogged < 64
            && (!done1 || g_KeyPmLogged < 8)) {
            CHAR keyLine[176], *keyCursor = keyLine;
            ++g_KeyPmLogged;
            keyCursor = LogPut(keyCursor, "KEYPM pend=");  keyCursor = LogHex(keyCursor, (DWORD)g_Irq1Pending);
            keyCursor = LogPut(keyCursor, " vi=");         keyCursor = LogHex(keyCursor, (DWORD)virtualIf);
            keyCursor = LogPut(keyCursor, " busy=");       keyCursor = LogHex(keyCursor, (DWORD)busy);
            keyCursor = LogPut(keyCursor, " pic=");        keyCursor = LogHex(keyCursor, (DWORD)picCanDeliver);
            keyCursor = LogPut(keyCursor, " app32=");      keyCursor = LogHex(keyCursor, (DWORD)isCode32);
            keyCursor = LogPut(keyCursor, " done=");       keyCursor = LogHex(keyCursor, (DWORD)done1);
            keyCursor = LogPut(keyCursor, " cs=0x");       keyCursor = LogHex(keyCursor, VDM_REG16(tib, VTIB_CS));
            keyCursor = LogPut(keyCursor, " scleft=");     keyCursor = LogHex(keyCursor, (DWORD)VddInputScanCodesQueued(&g_Input));
            keyCursor = LogPut(keyCursor, "\r\n"); LogAppend(LOG_PATH, keyLine, keyCursor); SerialOut(keyLine, keyCursor);
        }
    }
}


/* A heartbeat from the thread that is demonstrably alive (GH #128): where the client is, for each of
   the first 255 steps and then every 4096th. */
static PSTR DpmiLogHeartbeat(PSTR cursor, PSTR const base, const UINT steps, volatile BYTE * const tib)
{
    /* ── HEARTBEAT FROM THE THREAD THAT IS DEMONSTRABLY ALIVE. (GH #128)
         The watchdog thread is supposed to answer "where is the guest
         stuck", and on the WOW runs it logs its FIRST sample and then
         nothing -- twelve were asked for. So it is not a usable
         instrument here, whatever is wrong with it, and building on it
         would be building on something that has already been caught
         lying once. This line comes from the PM loop itself, which is
         provably still running because everything else in the log does.
       Sparse (every 4096 steps) so it cannot flood, and it prints the
         guest position and the last event -- which is the whole question
         when a run stops producing output but does not die. */
    /* Every 16 steps, not every 4096: a WOW run stops after only a few
       hundred PM entries, so a sparse heartbeat prints nothing at all --
       which is what the first attempt did. The LAST line before the log
       ends is the answer this exists for: the CS:EIP we handed to
       DpmiEnterProtectedMode and never came back from. */
    /* EVERY step for the first 256, then sparsely. A WOW run wedges
       after a few dozen PM entries, so anything sparser prints the
       position before the interesting one and not the interesting one
       -- which is what both earlier attempts did. Doom does millions of
       entries, hence the fallback rate rather than "always". */
    if (steps && (steps < 256 || (steps & 0xFFF) == 0)) {
        /* ⚠ A TIMELINE OF ITS OWN. Session 32 had to borrow wall-clock from
             the async thread's `ms=` stamps to discover that the whole WOW
             run is 282 MILLISECONDS -- which is the fact that reframed "it
             loads and then stops" and cleared the watchdog of a bug it never
             had. A heartbeat that cannot say WHEN forces that reconstruction
             every time, and only works while some other instrument happens
             to be printing timestamps. */
        cursor = LogPut(cursor, "PMHB ms=0x");     cursor = LogHex(cursor, GetTickCount());
        cursor = LogPut(cursor, " steps=0x");      cursor = LogHex(cursor, (DWORD)steps);
        cursor = LogPut(cursor, " cs:eip=0x");     cursor = LogHex(cursor, VDM_REG16(tib, VTIB_CS));
        cursor = LogPut(cursor, ":0x");            cursor = LogHex(cursor, DpmiPmEip(tib));
        cursor = LogPut(cursor, " lastev=0x");     cursor = LogHex(cursor, g_DpmiLastEvent);
        cursor = LogPut(cursor, " lastvec=0x");    cursor = LogHex(cursor, g_DpmiLastVector);
        cursor = LogPut(cursor, " wow32{ok=0x");   cursor = LogHex(cursor, g_Wow32Serviced);
        cursor = LogPut(cursor, " decl=0x");       cursor = LogHex(cursor, g_Wow32Declined);
        cursor = LogPut(cursor, " unimpl=0x");     cursor = LogHex(cursor, g_Wow32Unimplemented);
        cursor = LogPut(cursor, "}");
        /* ── ★ THE COUNTERS THAT MATTER TO A CRASH MUST NOT LIVE ONLY IN THE
             EXIT REPORT. (s72) The async why-histogram was printed at exit
             and nowhere else -- so for a guest that is KILLED, which is the
             only kind of run that needs it, the number was unreachable. I
             asked the user for a run whose whole purpose was to produce a
             counter the run could not produce. Emit the three that bear on
             the DPMI mode-switch race on every PM heartbeat instead. */
        cursor = LogPut(cursor, " why{simint_rm=0x"); cursor = LogHex(cursor, g_AsyncWhyHistogram[0][ASYNC_WHY_SIMINT_RM]);
        cursor = LogPut(cursor, " hostcs=0x");        cursor = LogHex(cursor, g_AsyncWhyHistogram[0][ASYNC_WHY_HOST_CS]);
        cursor = LogPut(cursor, " inflight=0x");      cursor = LogHex(cursor, g_AsyncWhyHistogram[0][ASYNC_WHY_IN_FLIGHT]);
        cursor = LogPut(cursor, "}");
        /* ★ WHERE IS IT ABOUT TO GO, AND WHAT IS IT ABOUT TO RUN.
             The resume point alone is not enough. When we hand the
             guest back at one of our default PM stubs it is sitting on
             a `CF`, and the interesting address is the one that IRET
             pops -- so print the stack top as well, and the bytes at
             the resume point. When a run's last line is a PM entry that
             never returns, this turns "it died somewhere after the IRET"
             into an address to disassemble.
           ⚠ Both reads are guarded. An instrument that faults while
             reporting a wedge reports nothing, which is how the
             watchdog thread came to log one sample and stop. */
        {   DWORD currentCodeBase = DpmiSelectorBase((WORD)VDM_REG16(tib, VTIB_CS));
            DWORD currentStackBase = DpmiSelectorBase((WORD)VDM_REG16(tib, VTIB_SS));
            DWORD spValue   = VDM_REG16(tib, VTIB_ESP);
            ULONG_PTR codeLinear = (ULONG_PTR)(currentCodeBase + DpmiPmEip(tib));
            ULONG_PTR stackLinear = (ULONG_PTR)(currentStackBase + spValue);
            cursor = LogPut(cursor, " ss:sp=0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_SS));
            cursor = LogPut(cursor, ":0x");       cursor = LogHex(cursor, spValue);
            /* ⚠ THE SELECTOR BASE, because a breakpoint is a LINEAR
                 address and guessing which copy of a module is the
                 executing one is how three runs got spent. The bind
                 stage loads the WOW module set and logs its bases;
                 those are not necessarily the bases the PM run uses,
                 and breakpoints armed against the wrong copy arm
                 SUCCESSFULLY (same bytes, real memory) and never fire.
                 Print it rather than infer it. */
            cursor = LogPut(cursor, " csbase=0x"); cursor = LogHex(cursor, currentCodeBase);
            if (currentCodeBase && MemoryReadable(codeLinear, 8)) {
                cursor = LogPut(cursor, " code="); cursor = LogDump(cursor, (const BYTE *)codeLinear, 8);
            }
            if (currentStackBase && MemoryReadable(stackLinear, 24)) {
                const BYTE *stackBytes = (const BYTE *)stackLinear;
                INT wordIndex;
                cursor = LogPut(cursor, " iret->0x");
                cursor = LogHex(cursor, (DWORD)(stackBytes[X86_FRAME16_CS] | (stackBytes[X86_FRAME16_CS + 1] << BYTE_SHIFT)));   /* CS  */
                cursor = LogPut(cursor, ":0x");
                cursor = LogHex(cursor, (DWORD)(stackBytes[0] | (stackBytes[1] << BYTE_SHIFT)));   /* IP  */
                /* ...and the frames ABOVE it. The IRET target turned out
                   to be a bare `ret` (krnl386 chains INT 31h to us through
                   an interrupt-style far call from a small helper, so the
                   interesting address is one frame further up). Print the whole
                   top of the
                   stack rather than coming back for it a third time. */
                cursor = LogPut(cursor, " stk");
                for (wordIndex = 0; wordIndex < 12; ++wordIndex) {
                    cursor = LogPut(cursor, " "); cursor = LogHex(cursor, (DWORD)(stackBytes[wordIndex*2] | (stackBytes[wordIndex*2+1] << BYTE_SHIFT)));
                }
            }
        }
        cursor = LogPut(cursor, "\r\n");
        LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
    }
    return cursor;
}


/* The PM run is over. A client that exited with a parent waiting in real mode (AH=4Ch from an EXEC'd child) is torn down and terminated there, and the exec loop carries on in V86; otherwise the PM run is marked done and the real-mode loop ends. */
static INT DpmiEndClientSession(PSTR *cursorIo, PSTR const base, volatile BYTE * const tib, DOS_MACHINE *machine)
{
    PSTR cursor = *cursorIo;
    /* ── ★★★ A CHILD WITH A PARENT GOES BACK TO ITS PARENT. (s80) ─────────
         The client said AH=4Ch in protected mode. If something EXEC'd it
         (COMMAND.COM, a SETUP program) that parent is parked in real mode
         at its own INT 21h, and ending the VDM here is what made "run Doom
         from the shell" and "save and launch" come back to nothing. Release
         the client, leave PM, and let the real-mode terminate do what it
         does for any child: free its PSP block, unwind its vectors, restore
         the parent's frame. The exec loop then simply carries on in V86.
       ⚠ ONLY with a parent. A top-level client's exit still ends the run,
         unchanged -- nothing is waiting for it. */
    if (g_PmClientExited && g_ExecDepth > 0 && g_Running) {
        InterlockedIncrement(&g_DpmiWatchdogGeneration);   /* its watchdog stands down */
        DpmiClientTeardown();
        *(volatile WORD *)(tib + VTIB_MSW) &= (WORD)~MSW_PE_BIT;   /* leave PM */
        VDM_SET16(tib, VTIB_FS, 0); VDM_SET16(tib, VTIB_GS, 0);
        machine->ExitCode = g_PmExitCode;
        cursor = LogPut(cursor, "DPMI: client exited with a parent waiting (depth=");
        cursor = LogHexByte(cursor, (UINT)g_ExecDepth);
        cursor = LogPut(cursor, ") -- back to real mode, terminating the child there\r\n");
        LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
        if (g_CloseForced) {           /* #152: it did not unhook itself */
            g_CloseForced = 0;
            if (g_Routed && g_ExecDepth == 1) g_BackToPrompt = 1;   /* #208 */
            ExecMachineRestore(g_ExecDepth - 1, &cursor);
            machine->IsTsrPending = 0;
        }
        if (DosTerminate(machine, tib, &cursor, base)) { *cursorIo = cursor; return HOST_FLOW_CONTINUE; }   /* parent resumed */
    }
    g_CloseForced = 0;
    g_DpmiDone = 1;            /* PM run finished -> watchdog stands down, window persists */
    { *cursorIo = cursor; return HOST_FLOW_BREAK; }
    *cursorIo = cursor; return HOST_FLOW_NEXT;
}

#include "host_report.c"   /* the end-of-run report: ReportEndOfRun and its sections */
enum { INSTALL_EXIT_OK = 0, INSTALL_EXIT_FAILED = 1, INSTALL_STATUS_EXIT_OURS = 0, INSTALL_STATUS_EXIT_NONE = 1, INSTALL_STATUS_EXIT_OTHER = 2 };   /* the install verbs' exit codes */
enum { PENDING_INT_RETRIES_MAX = 0x10000 };   /* event 3 ("interrupt pending, not entered"): retries before giving up */
enum { EXEC_HANDLED_RUN_OVER = 2, EXEC_HANDLED_CHILD_EXITED = 3 };   /* WinMain's DosTerminate outcomes: the run ends, or a child returned to its parent */

/* Run the DPMI client in protected mode until it stops for good: each step delivers pending interrupts, runs the client to its next event, and services what stopped it -- a patched INT nn BOP, a fault the kernel reflected, an async interrupt's return. */
static VOID DpmiRunClient(PSTR *cursorIo, PSTR const base, volatile BYTE * const tib, DOS_MACHINE *machine)
{
    PSTR cursor = *cursorIo;
    UINT steps;
    /* --- DPMI protected-mode execution loop -----------------------------------
       DpmiEnterProtectedMode runs the client in PM until it stops. Two stop kinds:
       (1) a patched INT nn BOP -- the kernel reflects C4 C4 as VTIB_EVENT=4
           (run 32); we look up the original vector by fault EIP and dispatch.
       (2) GH #18: a raw PM #GP the kernel reflects to our handler code selector
           -- also VTIB_EVENT=4, but CS==g_DpmiFaultCodeSelector and EIP==DPMI_FAULT_COFF.
           We recover the saved faulting CS:EIP/SS:ESP from the VTIB_FLT_SAV* slots. */
    DWORD event3Retries = 0;   /* GH#18: bounded event-3 (pending-int guard) re-entries */
    DWORD pmFaultDumps = 0;              /* rate-limit the PM-fault byte dump (anti-flood) */
    DWORD pmStartTick = GetTickCount();   /* headless wall-clock cap origin */
    for (steps = 0; g_Running && steps < PM_STEPS_MAX; ++steps) {  /* run until window close (animation) */
        DWORD event, eip, currentCs, vector; INT status;
        /* #152: Close Program ends a PM client exactly as its own AH=4Ch
           would; the child-with-a-parent path below does the rest. */
        if (g_CloseRequest && !g_WowLaunch) {
            InterlockedExchange(&g_CloseRequest, 0);
            g_CloseForced = 1;
            g_PmClientExited = 1; g_PmExitCode = 0;
            cursor = LogPut(cursor, "CLOSEPROG: ending the DPMI client (File > Close Program)\r\n");
            LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
        }
        if (g_PmClientExited) break;   /* exited inside a nested run -- see the flag */
        /* Headless safety (session-9): an infinite visual demo (pm32irq/animate)
           never calls INT 21h 4Ch, so under the SMB auto-exit harness the PM loop
           would run forever and wedge rt.bat's `start /wait`. Bound it by wall clock
           so the host self-exits and the watcher survives. Sampled sparsely (every
           4096 steps) to keep GetTickCount off the hot path. Interactive runs (no
           marker) are unbounded, as before -- the user closes the window. */
        if (g_Headless && (steps & PM_HEADLESS_CHECK_MASK) == 0 && steps
            && GetTickCount() - pmStartTick > PM_HEADLESS_MS) {
            cursor = LogPut(cursor, "STAGE3-DPMI: headless time cap (");
            cursor = LogHex(cursor, PM_HEADLESS_MS); cursor = LogPut(cursor, " ms) reached after 0x");
            cursor = LogHex(cursor, (UINT)steps);
            cursor = LogPut(cursor, " steps -> exiting (infinite/visual demo; watch it on the monitor)\r\n");
            LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
            break;
        }
        cursor = DpmiLogHeartbeat(cursor, base, steps, tib);
        /* run 52 heartbeat: publish where we're about to hand off + bump the
           iteration counter BEFORE entering, so a watchdog sample taken while
           we're blocked inside DpmiEnterProtectedMode sees a FROZEN iter at this CS:EIP. */
        g_DpmiEnterCs  = VDM_REG16(tib, VTIB_CS);
        g_DpmiEnterEip = DpmiPmEip(tib);
        g_DpmiIteration      = (LONG)(steps + 1);
        /* run 66 diagnostic (kept): log FIXED_NTVDMSTATE [0x714] once. Run 66 proved
           bit3=0 already (classifier is NOT the blocker), so no forcing needed -- for
           a #GP the kernel uses class 6, which reaches the generic reflect body. */
        if (steps == 0) {
            DWORD vdmStateAt714 = *(volatile DWORD *)(ULONG_PTR)FIXED_NTVDMSTATE_LINEAR;
            cursor = LogPut(cursor, "GH#18: [0x714]=0x"); cursor = LogHex(cursor, vdmStateAt714);
            cursor = LogPut(cursor, " bit3="); cursor = LogHex(cursor, (vdmStateAt714 >> 3) & 1);
            cursor = LogPut(cursor, " bit4="); cursor = LogHex(cursor, (vdmStateAt714 >> 4) & 1);
            cursor = LogPut(cursor, " bit14="); cursor = LogHex(cursor, (vdmStateAt714 >> 14) & 1);
            cursor = LogPut(cursor, " tib8=0x"); cursor = LogHex(cursor, *(volatile DWORD *)(tib + DPMI_TIB_FLTTBL));
            cursor = LogPut(cursor, "\r\n");
            LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
        }
        /* Timing in PM (polled): the host PIT (UI thread) raises IRQ0 at the 8254
           rate; here we consume it and run the BIOS tick handler so 0040:006C and
           INT 1Ah advance with wall-clock even though nothing injects INT 08h into
           the PM client yet. (Async IRQ0 delivery to a client's PM INT 08h hook is
           the remaining timing piece.) */
        OplPumpTime();
        if (InterlockedExchange(&g_Irq0Pending, 0)) {
            NTVDD_REGISTERS trapRegisters; RegistersLoad(&trapRegisters, tib);
            HOST_LOCK();
            VddBusDeliverInterrupt(&g_Bus, VECTOR_TIMER, &trapRegisters);   /* PitInt08 -> ++0040:006C */
            HOST_UNLOCK();
            g_PmIrq0Latch = 1;    /* #2b: latch a virtual IRQ0 for the PM hook */
        }
        /* #2b async IRQ0 injection: when the client has hooked INT 08h in PM
           (g_PmInt[8], via INT 31h 0205) and its virtual-IF is enabled, deliver
           the latched IRQ0 to that handler -- how timer-hooking games get ticks.
           The latch persists across CLI windows so a masked interrupt isn't lost. */
        /* ► AND NOT WHILE AN ASYNC ONE IS STILL IN FLIGHT. The two injectors
             guarded themselves but not each other: the async path can vector the
             guest into its ISR and return, and if that ISR then leaves PM for a
             DOS call, control arrives back HERE with the handler still live --
             and a second tick would re-enter it on top of itself. Measured: the
             run died on such an injection taken at obj1+0x153dc, i.e. inside the
             very delay loop the tick exists to release. */
        /* #172: which gate turned an OWED tick away. See g_PmCooperativeGate. */
        INT twoTicksOwed = (g_PmTickOwed >= 2);
        if (twoTicksOwed) {
            UINT gateIndex = !(g_PmIrq0Latch || g_PmTickOwed > 0) ? PM_GATE_NO_LATCH : !g_DpmiVi ? PM_GATE_VIF_OFF
                        : !g_PmInt[VECTOR_TIMER].Client ? PM_GATE_NO_HOOK : g_InPmIrq ? PM_GATE_IN_PM_IRQ
                        : g_PmNoIrq ? PM_GATE_NO_IRQ : g_AsyncPmActive ? PM_GATE_ASYNC_IN_FLIGHT
                        : (GetTickCount() - g_PmVector8ArmedMs) < DPMI_IRQ0_ARM_QUIET_MS ? PM_GATE_ARMED
                        : PM_GATE_TRIED;
            g_PmCooperativeGate[gateIndex]++;
        }
        /* ── #172: THE LATCH OPENS THIS, NOT THE OWED COUNT. (s85) ────────────
             s84 opened the arm on `g_PmTickOwed > 0` as well, on the theory
             that the quit wait's backlog was stuck here. The real cause was
             ModeYPmRun holding g_Lock (stop reason `irq`), and isolated on the
             menu-quit route (runs/s85/owed/, interleaved x3) the owed-count arm
             bought nothing: quit window 139/s either way, REPLAYED_LOUD 48-50
             with it against 44-45 without, plus ~5,800 injections declined per
             run in DOS/4GW's 16-bit start-up. Latch only. */
        if (g_PmIrq0Latch
            && g_DpmiVi && g_PmInt[VECTOR_TIMER].Client && !g_InPmIrq
            && !g_PmNoIrq && !g_AsyncPmActive
            && (GetTickCount() - g_PmVector8ArmedMs) >= DPMI_IRQ0_ARM_QUIET_MS) {
            UINT32 dmaReadsBefore = g_Dma.ChannelCountReads[SB_DEFAULT_DMA8];
            g_PmIrq0Latch = 0;
            g_InPmIrq = 1;
            if (g_PmTickOwed > 0 && Irq0PmClaim()) {
                if (DpmiInjectPmIrq(machine, tib, VECTOR_TIMER, steps))
                    InterlockedDecrement(&g_PmTickOwed);
                else { Irq0PmUnclaim(); if (twoTicksOwed) g_PmCooperativeGate[PM_GATE_DECLINED]++; }
            } else if (twoTicksOwed) g_PmCooperativeGate[PM_GATE_CLAIM_REFUSED]++;
            g_InPmIrq = 0;
            g_CooperativeDmaPolls += g_Dma.ChannelCountReads[SB_DEFAULT_DMA8] - dmaReadsBefore;
        }
        /* ── s90 (#278): IRQs RAISED BY A 32-BIT COMPONENT (call_ica_hw_interrupt,
             through bin\wowshim\NTVDM.EXE) -- winmm raises IRQ 10 to tell
             MMSYSTEM a callback is queued in their shared buffer, and
             MMSYSTEM's handler (PM vector 72h) drains it and EOIs both PICs.
             Delivered here, by the guest thread, on the same gates as IRQ0;
             a line nobody hooked is counted and dropped (real hardware would
             reach the default handler, which only EOIs). WOW only. */
        if (g_WowLaunch && g_IcaPending && g_DpmiVi)
            WowIcaDeliver(machine, tib, steps);
        DpmiDeliverKeyboardIrq(tib, machine, steps);
        /* ── AND THE DEVICE LINES, WHICH HAD NO COOPERATIVE PATH EITHER. ─────
             This is the KEYBOARD BUG ABOVE, one line number over, and it is
             the PCM click. A device IRQ gets exactly ONE delivery attempt --
             the synchronous AsyncInjectIrq() inside HostIrqSink(), made
             from the AUDIO thread at the instant of the raise. If the CPU
             thread happens to be inside the host rather than in guest code
             that attempt bails at why=20 and NOTHING RETRIES: the V86 exec
             loop's drain (the `for (q = 2; q < 8; ...)` above) needs a `tib`
             from a trapping guest, and a 32-bit DPMI client never goes there.
             MEASURED on a 45 s Doom run, and it is not marginal:
                 sb_blocks   0x0de6 = 3558 block completions raised
                 irq05 (PM)  0x0a37 = 2615 delivered
                 ASYNC-EARLY 1009 bails, EVERY ONE why=0x14 (g_InExec == 0)
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
        /* ── AND THE MOUSE DRIVER'S OWN CALLBACK (INT 33h 0Ch), same gate.
             (s74c) ZAR's buttons travel only through this. */
        if (g_MouseEventPend && MouseAnyHandler()        /* 0Ch's or 18h's (#265) */
            && g_DpmiVi && !g_PmNoIrq && !g_InPmIrq && !g_AsyncPmActive) {
            g_InPmIrq = 1;
            DpmiInjectPmMouseCallback(machine, tib, steps);
            g_InPmIrq = 0;
        }
        DpmiDeliverPendingIrqs(machine, tib, steps);
        cursor = DpmiCheckpointFirstEntries(cursor, base, steps, tib);
        DpmiArmFaultTrampoline(tib, 0);   /* re-arm nest/flag/[0x638]/[TIB+8] */
        if (steps < g_DpmiCpMaximum) {
            cursor = LogPut(cursor, "DPMI-CP["); cursor = LogHex(cursor, (UINT)steps);
            cursor = LogPut(cursor, "] armed -> entering PM\r\n");
            LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
        }
        /* Give the watchdog a guaranteed turn before the FIRST entry only. If it
           has logged a sample by the time we hand off, then its silence afterwards
           means the whole process was killed at once (a kernel VDM terminate),
           not that the thread never ran. 300 ms, once, on a diagnostic path. */
        if (steps == 0) Sleep(300);
        /* ── TELL THE ASYNC INJECTOR THE GUEST IS RUNNING. ───────────────────
             g_InExec means "the CPU thread is executing GUEST code, so its
             context is the guest's and may be rewritten"; AsyncInjectIrq()
             refuses to touch the thread without it, precisely so it cannot race
             the host manipulating the TIB. It was set only around VdmRunGuest(), so
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
        if (!DpmiSelectorIs32((WORD)VDM_REG16(tib, VTIB_SS)))
            VDM_REG(tib, VTIB_ESP) &= WORD_MASK_U;
        while (g_PauseWant && g_Running) { ++g_PauseCooperative; Sleep(PAUSE_POLL_MS); }   /* #219 */
        CpuSpeedCooperativePark();                                                  /* #225 */
        InterlockedExchange(&g_InExec, 1);
        ExecEnterMark();   /* guest-execution clock starts (throttle) */
        {
            INT flow = DpmiRunClientSlice(&cursor, base, tib, steps);
            if (flow == HOST_FLOW_BREAK) break;
        }
        ExecLeaveMark();   /* ...and stops. Our servicing is not its   */
        InterlockedExchange(&g_InExec, 0);
        if (steps < g_DpmiCpMaximum) {
            cursor = LogPut(cursor, "DPMI-CP["); cursor = LogHex(cursor, (UINT)steps);
            cursor = LogPut(cursor, "] returned ev=0x"); cursor = LogHex(cursor, VDM_REG(tib, VTIB_EVENT));
            cursor = LogPut(cursor, " cs:eip=0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_CS));
            cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_EIP));
            cursor = LogPut(cursor, "\r\n");
            LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
        }
        event  = VDM_REG(tib, VTIB_EVENT);
        eip = DpmiPmEip(tib);
        currentCs = VDM_REG16(tib, VTIB_CS);
        g_DpmiLastEvent  = event;  g_DpmiLastEip = eip;  g_DpmiLastCs = currentCs;
        {
            INT flow = DpmiFinishAsyncPmInterrupt(event, currentCs, eip, tib, machine, steps);
            if (flow == HOST_FLOW_CONTINUE) continue;
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
        if (event == 3) {
            if (++event3Retries <= PENDING_INT_RETRIES_MAX) {
                if (event3Retries <= 3) {
                    cursor = LogPut(cursor, "GH#18: event3 pending-int guard at CS:EIP=0x");
                    cursor = LogHex(cursor, currentCs); cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, eip);
                    cursor = LogPut(cursor, " [0x714]=0x");
                    cursor = LogHex(cursor, *(volatile DWORD *)(ULONG_PTR)FIXED_NTVDMSTATE_LINEAR);
                    cursor = LogPut(cursor, " -> clear stale pending + re-enter\r\n");
                    LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
                }
                *(volatile DWORD *)(ULONG_PTR)FIXED_NTVDMSTATE_LINEAR &= ~VDM_INT_PENDING;
                continue;
            }
            /* retry budget exhausted -> fall through and report an unexpected stop */
        }
        {
            INT flow = DpmiHandleReflectedFault(&cursor, base, event, currentCs, eip, tib, machine, steps);
            if (flow == HOST_FLOW_BREAK) break;
            if (flow == HOST_FLOW_CONTINUE) continue;
        }
        {
            INT flow = DpmiResumeAfterClientHandler(&cursor, base, event, currentCs, eip, tib);
            if (flow == HOST_FLOW_BREAK) break;
            if (flow == HOST_FLOW_CONTINUE) continue;
        }
        /* GH#18 run 72: a real-CPU PROTECTED-MODE I/O insn (IN/OUT) reflects as
           event 0 -- the SAME VDD-trap event as V86 (VM-confirmed by outprobe.com,
           a PM `OUT DX,AL` to 0x3C8). Service it through the device bus and resume,
           so PM port I/O (VGA/sound) reaches our VDDs instead of the loop treating
           event 0 as an "unexpected PM stop" and spinning. */
        if (event == VDM_EVENT_IO || event == VDM_EVENT_IO_HW || event == VDM_EVENT_GPFAULT) {
            INT isIoHandled;
            HOST_LOCK();
            isIoHandled = HostTryIoPm(tib, &g_Bus);
            HOST_UNLOCK();
            if (isIoHandled) {
                /* North star 1: the OUT that just trapped may have opened a
                   multi-plane window -- Doom's drawers start exactly so. */
                if (ModeYPmNeedsInterp()) ModeYPmRun(tib);
                continue;         /* serviced the port op -> keep running */
            }
            /* not a decodable I/O op -> fall through to the normal dispatch/stop */
        }
        /* BARE-METAL diagnostic (GH #18): dump the faulting PM instruction bytes for
           any non-BOP stop, so we can identify what real hardware reflects as event 3
           (raw PM #GP) vs the HVF silent-terminate. Rate-limited to the first 32 stops
           so a client that repeatedly faults can't flood the log (session-9). */
        if (event != VDM_EVENT_BOP && pmFaultDumps < 32) {
            DWORD faultBase = DpmiSelectorBase((WORD)currentCs);
            ++pmFaultDumps;
            const volatile BYTE *faultInstruction = (const volatile BYTE *)(ULONG_PTR)(faultBase + eip);
            UINT32 selectorAr = 0, selectorLim = 0; DpmiSelectorDescriptor((WORD)currentCs, &selectorAr, &selectorLim);
            cursor = LogPut(cursor, "GH#18 PM-FAULT ev=0x"); cursor = LogHex(cursor, event);
            cursor = LogPut(cursor, " CS:EIP=0x"); cursor = LogHex(cursor, currentCs); cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, eip);
            cursor = LogPut(cursor, " base=0x"); cursor = LogHex(cursor, faultBase); cursor = LogPut(cursor, " AR=0x"); cursor = LogHex(cursor, selectorAr);
            cursor = LogPut(cursor, " lim=0x"); cursor = LogHex(cursor, selectorLim); cursor = LogPut(cursor, " bytes=");
            cursor = LogDump(cursor, (const VOID *)faultInstruction, 12); cursor = LogPut(cursor, "\r\n");
            LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
        }
        vector = (event == VDM_EVENT_BOP) ? DpmiBopVector(currentCs, eip) : 0;
        g_DpmiLastVector = vector;
        g_PmTopDispatch = 1;          /* #256: consumed by this dispatch */
        status = DpmiServicePmInt(machine, tib, vector, steps);
        if (status > 0) continue;   /* serviced -> keep running the PM client */
        break;                  /* 0 = client exited, <0 = unexpected stop */
    }
    *cursorIo = cursor;
}


static PSTR DpmiInstallFaultReflect(PSTR cursor, PSTR const base)
{
    /* GH #18 (run 67): install the PM-fault reflect machinery, so a RAW (non-BOP)
       PM #GP -- an SS-retype, HLT, or privileged op the INT->BOP scan cannot
       pre-patch -- is reflected by the kernel to our handler (code sel : BOP) on a
       scratch stack, instead of silently terminating the VDM (runs 20-34). */
    DpmiInstallFaultTrampoline();
    cursor = LogPut(cursor, "DPMI: PM-fault reflect stkSel=0x"); cursor = LogHex(cursor, g_DpmiFaultSelector);
    cursor = LogPut(cursor, " codeSel=0x"); cursor = LogHex(cursor, g_DpmiFaultCodeSelector);
    cursor = LogPut(cursor, " bop@code:0x"); cursor = LogHex(cursor, DPMI_FAULT_COFF);
    cursor = LogPut(cursor, " tbl@0x"); cursor = LogHex(cursor, (DWORD)(ULONG_PTR)g_FaultTable);
    cursor = LogPut(cursor, " class="); cursor = LogHex(cursor, DPMI_FLT_CLASS_GP);
    cursor = LogPut(cursor, "\r\n");
    LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
    return cursor;
}


/* Read what this PM session is configured to do, on the run that will use it: guest breakpoints, the WOW32 and scheduler switches, the PM watch addresses, and the cfg\ flags (pmkernel, nomouse, nosb, pmvehpass, pmnoirq). */
static PSTR DpmiLoadSessionKnobs(PSTR cursor, PSTR const base)
{
    /* Guest breakpoints (PMBP_PATH). Loaded here rather than at WinMain entry
       so the list is read on the run that will use it, and armed both now and
       after every code-region patch -- an address inside a module the client
       has not loaded yet simply arms later. */
    DpmiBreakpointLoad();
    DpmiBreakpointArm();
    Wow32ReturnLoad();
    Wow32ModeLoad();
    WowSchedLoad();
    WowCallLoad();
    /* LAST of the switches, so everything above still gets to say
       what it armed before the trace goes quiet. */
    WowQuietLoad();
    /* pmchg.txt: `<hex offset> [segment]`, segment defaulting to 4
       (krnl386's DGROUP). Absent file = no watch and no cost. */
    { HANDLE configHandle = CreateFileA(PMCHG_PATH, GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                              OPEN_EXISTING, 0, NULL);
      if (configHandle != INVALID_HANDLE_VALUE) {
          CHAR pmChangeText[128]; DWORD commandLength = 0, pmTextIndex = 0, values[PMWATCH_COLUMNS] = { 0, 0 }; INT column = 0;
          ReadFile(configHandle, pmChangeText, sizeof pmChangeText - 1, &commandLength, NULL); CloseHandle(configHandle);
          while (pmTextIndex < commandLength && column < PMWATCH_COLUMNS) {
              INT digits = 0;
              while (pmTextIndex < commandLength && (pmChangeText[pmTextIndex] == ' ' || pmChangeText[pmTextIndex] == '\t')) ++pmTextIndex;
              while (pmTextIndex < commandLength) {
                  CHAR character = pmChangeText[pmTextIndex];
                  INT pmDigit = (character >= '0' && character <= '9') ? character - '0'
                        : (character >= 'a' && character <= 'f') ? character - 'a' + HEX_DIGIT_A_VALUE
                        : (character >= 'A' && character <= 'F') ? character - 'A' + HEX_DIGIT_A_VALUE : -1;
                  if (pmDigit < 0) break;
                  values[column] = (values[column] << NIBBLE_SHIFT) | (DWORD)pmDigit; ++digits; ++pmTextIndex;
              }
              if (digits) ++column; else break;
          }
          if (column >= 1) {
              g_PmWatchOffset = values[0];
              if (column >= PMWATCH_COLUMNS && values[1] <= WOW_PMBASE_MAX)
                  g_PmWatchSegment = (UINT)values[1];   /* 0 = already linear */
              cursor = LogPut(cursor, "PMWATCH: watching seg "); cursor = LogHex(cursor, g_PmWatchSegment);
              cursor = LogPut(cursor, " + 0x"); cursor = LogHex(cursor, g_PmWatchOffset);
              cursor = LogPut(cursor, " for changes\r\n");
              LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
          }
      }
    }
    if (GetFileAttributesA(PMVERBOSE_PATH) != INVALID_FILE_ATTRIBUTES)
        g_DpmiCpMaximum = 0x100000;   /* verbose: trace a whole startup */
    { HANDLE watchHandle = CreateFileA(PMWATCH_PATH, GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                              OPEN_EXISTING, 0, NULL);
      if (watchHandle != INVALID_HANDLE_VALUE) {
          CHAR watchBuffer[128]; DWORD watchLength = 0, index2b = 0;
          ReadFile(watchHandle, watchBuffer, sizeof watchBuffer - 1, &watchLength, NULL); CloseHandle(watchHandle);
          while (index2b < watchLength && g_PmWatchCount < DPMI_WATCH_MAX) {
              DWORD number = 0; INT dig = 0, rel = 0;
              if (index2b < watchLength && watchBuffer[index2b] == '+') { rel = 1; ++index2b; }
              while (index2b < watchLength) {
                  CHAR character = watchBuffer[index2b];
                  INT digit = (character >= '0' && character <= '9') ? character - '0'
                        : (character >= 'a' && character <= 'f') ? character - 'a' + HEX_DIGIT_A_VALUE
                        : (character >= 'A' && character <= 'F') ? character - 'A' + HEX_DIGIT_A_VALUE : -1;
                  if (digit < 0) break;
                  number = (number << NIBBLE_SHIFT) | (DWORD)digit; ++dig; ++index2b;
              }
              if (dig) { g_PmWatchRel[g_PmWatchCount] = (BYTE)rel;
                         g_PmWatch[g_PmWatchCount++] = number; }
              else ++index2b;
          }
          cursor = LogPut(cursor, "DPMI: pmwatch.txt -> ");
          { INT watchIndex2; for (watchIndex2 = 0; watchIndex2 < g_PmWatchCount; ++watchIndex2) {
                cursor = LogPut(cursor, g_PmWatchRel[watchIndex2] ? "codebase+0x" : "0x");
                cursor = LogHex(cursor, g_PmWatch[watchIndex2]); cursor = LogPut(cursor, " "); } }
          cursor = LogPut(cursor, "\r\n");
          LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
      } }
    g_DpmiUseKernel = (GetFileAttributesA(PMKERNEL_PATH) != INVALID_FILE_ATTRIBUTES);
    if (g_DpmiUseKernel) {
        cursor = LogPut(cursor, "DPMI: pmkernel.flag -- PM will run under VdmStartExecution\r\n");
        LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
    }
    if (g_MouseAbsent) {
        cursor = LogPut(cursor, "MOUSE: nomouse.flag -- INT 33h reports NO driver installed\r\n");
        LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
    }
    g_SbAbsent = (GetFileAttributesA(NOSB_PATH) != INVALID_FILE_ATTRIBUTES);
    g_OplAbsent = g_SbAbsent;      /* one knob, both devices unfitted */
    if (g_SbAbsent) {
        cursor = LogPut(cursor, "SB: nosb.flag -- DSP reset will NOT answer; no Sound Blaster fitted\r\n");
        LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
    }
    g_PmVehPass = (GetFileAttributesA(PMVEHPASS_PATH) != INVALID_FILE_ATTRIBUTES);
    if (g_PmVehPass) {
        cursor = LogPut(cursor, "DPMI: pmvehpass.flag -- non-INT PM faults will NOT be swallowed by the VEH\r\n");
        LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
    }
    g_PmNoIrq = (GetFileAttributesA(PMNOIRQ_PATH) != INVALID_FILE_ATTRIBUTES);
    if (g_PmNoIrq) {
        cursor = LogPut(cursor, "DPMI: pmnoirq.flag present -- IRQ0->PM injection SUPPRESSED\r\n");
        LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
    }
    return cursor;
}


static PSTR DpmiPatchClientIntSitesUpFront(PSTR cursor, PSTR const base)
{
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
    { volatile BYTE *cs = (volatile BYTE *)(ULONG_PTR)g_DpmiCodeBase;
      DWORD position, count = 0, last = 0, votedCount = 0;
      for (position = 0; position < X86_SEGMENT_LIMIT_64K; ++position) {
          /* ⚠ 0x2F IS HERE BECAUSE krnl386 DIED WITHOUT IT (GH #128).
               A PM guest cannot reach the IVT, so an INT this list does not
               name stays a raw `CD nn`, and executing it in protected mode
               goes to the kernel's #GP reflect -- which does not reflect,
               it SILENTLY TERMINATES THE VDM. That is the #18 signature:
               no exception, no log line, the process simply gone.
               krnl386 issues INT 2Fh from PM (168A, the "MS-DOS" vendor
               query -- its very next interrupt after the 000A alias, per
               the log) and it died there. No DOS/4GW-class client
               ever did that, which is why the list never needed 0x2F.
             ⚠ Every number added here widens the false-positive surface of
               what is a NAIVE byte-pair scan -- the same shape that once
               rewrote a `jle` displacement in Doom and cost five sessions.
               The narrow list is the mitigation. Add a vector only with a
               guest that provably needs it, and only with a service arm to
               receive it (see DpmiServicePmInt). */
          /* ── ★★★ AND EVERY OTHER VECTOR, IF THE DECODER VOUCHES FOR IT. ──
               The list above is the vectors we EXPECTED, and the note beside
               it is right that widening a naive byte-pair scan is dangerous.
               But "naive" is the part that changed: x86len.h's vote arrived in
               session 21 and is already trusted to gate every site in
               DpmiPatchCodeRegion(). It was simply never wired in here.

             ⚠ MEASURED, session 32. krnl386 executes an `INT 2` (`cd 02`) in
               protected mode. A breakpoint armed on that site reported `displaced
               cd 02`, which
               is proof it was RAW -- DpmiBreakpointArm() refuses a site that is already
               an INT site, so it could not have armed otherwise. 0x02 is not on
               the list, so it was never even a candidate.

             ⚠⚠ AND THE FIRST DIAGNOSIS WAS WRONG: this was written up as "the
               boundary vote produces false negatives on real instructions". The
               vote never ran. The filter in front of it was the defect, and it
               is a different one in each of the two scanners.

             ⚠⚠⚠ AND "EVERY VECTOR, GATED BY THE VOTE" WAS TRIED AND IS WRONG.
               Measured: it patched 0x7d sites instead of 0x6c -- 17 unlisted ones
               the vote vouched for -- and the WOW run went BACKWARDS, dying at PM
               step 0x27 instead of 0x46. Residuals fell from 31 to 14 and the
               INT 2 was correctly claimed, so the widening did what it said; it
               also broke the guest earlier, which means at least one of those 17
               is data or mid-instruction that the vote waved through. The vote is
               good (real sites 19-48 votes, false pairs 0-3) but it is NOT good
               enough to underwrite all 256 vectors on this binary.

             ⇒ SO: EVIDENCE ONLY, which is what the note above already prescribed
               -- "add a vector only with a guest that provably needs it". The one
               vector with proof is 0x02, and it is still gated by the vote. A
               listed vector is patched exactly as before, unvoted, so no existing
               guest can change behaviour. `votedCount` is counted separately so the
               next widening is measurable rather than asserted. */
          if (cs[position] == X86_OP_INT) {
              BYTE siteVector = cs[position+1];
              INT listed = (siteVector == VECTOR_DPMI || siteVector == VECTOR_DOS || siteVector == VECTOR_VIDEO
                            || siteVector == VECTOR_KEYBOARD_SERVICES || siteVector == VECTOR_MOUSE
                            || siteVector == VECTOR_MULTIPLEX
                            || siteVector == VECTOR_EQUIPMENT || siteVector == VECTOR_KERNEL_DEBUGGER
                            || siteVector == VECTOR_TIME || siteVector == VECTOR_TIMER);
              DWORD linear = g_DpmiCodeBase + position;      /* map is linear-keyed now */
              if (!listed && siteVector != VECTOR_NMI) continue;    /* evidence only -- see above */
              if (!listed) {
                  /* Initial mode-switch selectors are 16-bit even for a 32-bit
                     client (the RETF-on-failure proof, session 16), so d32=0. */
                  if (!X86IsIntSiteReal((const BYTE *)(ULONG_PTR)cs,
                                            position, X86_SEGMENT_LIMIT_64K, X86_OPERAND_16)) continue;
                  ++votedCount;
              }
              PatchMapSet(linear, siteVector); cs[position] = VDM_BOP0; cs[position+1] = VDM_BOP1; ++count; last = position;
          }
      }
      cursor = LogPut(cursor, "DPMI: patched "); cursor = LogHex(cursor, count);
      cursor = LogPut(cursor, " INT sites -> BOP (full 64K scan, last off 0x"); cursor = LogHex(cursor, last);
      cursor = LogPut(cursor, "), of which "); cursor = LogHex(cursor, votedCount);
      cursor = LogPut(cursor, " were UNLISTED vectors vouched for by the x86len vote\r\n");
      LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
      /* ── WHAT DID THE PATCH LEAVE BEHIND? ────────────────────────────
           Every `CD nn` still in this region is a vector we did not claim,
           and a PM guest cannot reach the IVT -- so if the guest executes
           one, the kernel #GP reflect silently terminates the VDM. No
           exception, no log line, the process simply gone. That is how
           krnl386 died on INT 2Fh, and finding it meant reading this
           function's constant list by hand afterwards.
         So say it up front, as a histogram by vector. It is an UPPER
         BOUND -- a linear byte-pair count over a region that contains data
         as well as code, so some of these are not instructions at all --
         but a vector that is ABSENT here cannot kill the guest, which
         makes the list a genuine shortlist of suspects rather than a
         guess. Cheap, and it turns the next silent death into a lookup. */
      {   DWORD hist[BYTE_VALUES], offset2, total = 0; INT number;
          for (number = 0; number < BYTE_VALUES; ++number) hist[number] = 0;
          for (offset2 = 0; offset2 < X86_SEGMENT_LIMIT_64K; ++offset2)
              if (cs[offset2] == X86_OP_INT) { ++hist[cs[offset2 + 1]]; ++total; }
          /* ── ★★ AND THE OFFSETS, NOT JUST THE HISTOGRAM. ──────────────────
               A histogram says a vector is a suspect; it does not say WHERE,
               so acting on it still means reading the binary by hand. Session
               32 needed exactly that: a `cd 02` in krnl386 turned out to be
               REAL CODE the guest executes, left RAW by the boundary vote --
               proved by arming a breakpoint on it and reading back
               `displaced cd 02` (a patched site is an INT site, and
               DpmiBreakpointArm refuses those, so it could not have armed at all).
             A raw `CD nn` in protected mode is not a warning, it is a silent
               VDM teardown waiting for the guest to take that branch. Print the
               addresses so the next one is a breakpoint away instead of a
               disassembly session. Bounded so a data-heavy region cannot flood. */
          {   DWORD shown = 0;
              cursor = LogPut(cursor, "DPMI: residual CD nn SITES (linear, first 24):");
              for (offset2 = 0; offset2 < X86_SEGMENT_LIMIT_64K && shown < 24; ++offset2)
                  if (cs[offset2] == X86_OP_INT) {
                      cursor = LogPut(cursor, " 0x"); cursor = LogHex(cursor, (DWORD)(ULONG_PTR)(cs + offset2));
                      cursor = LogPut(cursor, "="); cursor = LogHexByte(cursor, cs[offset2 + 1]);
                      ++shown;
                  }
              cursor = LogPut(cursor, "\r\n");
              LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
          }
          cursor = LogPut(cursor, "DPMI: residual CD nn in the region: "); cursor = LogHex(cursor, total);
          cursor = LogPut(cursor, " (unclaimed vectors, upper bound)");
          for (number = 0; number < BYTE_VALUES; ++number) if (hist[number]) {
              cursor = LogPut(cursor, " "); cursor = LogHexByte(cursor, (BYTE)number);
              cursor = LogPut(cursor, "h x"); cursor = LogHex(cursor, hist[number]);
          }
          cursor = LogPut(cursor, "\r\n");
          LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
      }
    }
    return cursor;
}


/* The client asked to switch to protected mode (the DPMI entry BOP): build its initial selectors and PSP selector, start the watchdog, patch its INT sites, load the session's switches, run it in PM until it stops for good, and end the session -- or report that the switch failed. */
static INT DpmiStartClientSession(PSTR *cursorIo, PSTR const base, volatile BYTE * const tib, DOS_MACHINE *machine)
{
    PSTR cursor = *cursorIo;
    DWORD currentCs = VDM_REG16(tib, VTIB_CS), currentIp = VDM_REG16(tib, VTIB_EIP);
    LONG registerStatus = 0, setStatus = 0; INT switched;
    /* AX bit0 = the client's declared width (0=16-bit, 1=32-bit e.g. DOS/4GW).
       Logged and recorded, but it does NOT set the initial selectors' D/B --
       see DpmiSwitchToProtectedMode(); doing so ran DOS/4GW's 16-bit stub as 32-bit. */
    INT is32 = (INT)(VDM_REG(tib, VTIB_EAX) & 1);
    cursor = LogPut(cursor, "STAGE3: DPMI_BOP far-call LANDED @ 0x"); cursor = LogHex(cursor, currentCs);
    cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, currentIp);
    cursor = LogPut(cursor, is32 ? " -- switching to PM (32-bit client)\r\n"
                     : " -- switching to PM (16-bit client)\r\n");
    LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
    switched = DpmiSwitchToProtectedMode(tib, is32, &registerStatus, &setStatus);
    cursor = LogPut(cursor, " [svc11=0x"); cursor = LogHex(cursor, (UINT)registerStatus);
    cursor = LogPut(cursor, " svc10=0x"); cursor = LogHex(cursor, (UINT)setStatus); cursor = LogPut(cursor, "]");
    cursor = LogPut(cursor, " retcs=0x"); cursor = LogHex(cursor, g_DpmiDebug[0]);
    cursor = LogPut(cursor, " clo=0x"); cursor = LogHex(cursor, g_DpmiDebug[2]);
    cursor = LogPut(cursor, " chi=0x"); cursor = LogHex(cursor, g_DpmiDebug[3]);
    if (switched == 0) {
        g_DpmiPm = 1;
        g_DpmiCodeBase = g_DpmiSegmentBase[0];   /* CS base = the patch-scan target */
        /* Record the switch's code/data/stack selector bases (indices 1/2/3) so
           DpmiSelectorBase() translates DS:/ES:/SS: through the right base -- essential
           once CS!=DS!=SS (a real .EXE); for a .COM all three are equal. */
        { INT si; for (si = 0; si < DPMI_INITIAL_SELECTOR_COUNT; ++si) {
            g_Ldt[DPMI_INITIAL_FIRST_INDEX + si].Base   = g_DpmiSegmentBase[si];
            g_Ldt[DPMI_INITIAL_FIRST_INDEX + si].Limit  = X86_SEGMENT_LIMIT_64K;
            g_Ldt[DPMI_INITIAL_FIRST_INDEX + si].Access = (si == 0) ? DPMI_ACCESS_CODE : DPMI_ACCESS_DATA;
            /* Mirror the D/B width DpmiSwitchToProtectedMode ACTUALLY installed, so
               DpmiSelectorIs32() (I/O decode + EIP-mask gating) agrees with the live
               descriptor. That is now always 16-bit for these three: the client's
               post-switch code must also be valid real-mode code on the failure
               path, so it cannot be 32-bit. A 32-bit client far-jmps to its OWN
               INT 31h-allocated 32-bit selectors, which are reported correctly. */
            g_Ldt[DPMI_INITIAL_FIRST_INDEX + si].Flags  = 0;
        } }
        if (g_LdtNext < DPMI_FIRST_CLIENT_INDEX) g_LdtNext = DPMI_FIRST_CLIENT_INDEX;      /* client allocs start at index 4 now */
        g_LdtClientMark = g_LdtNext;          /* teardown gives back everything above */
        /* ── DPMI INITIAL CLIENT STATE: ES = PSP SELECTOR, AND THE PSP'S
              ENVIRONMENT POINTER CONVERTED TO A SELECTOR. ────────────────────
           DpmiSwitchToProtectedMode() sets ES = DS (a second copy of the data selector),
           and that is simply wrong. DPMI 0.9, "entering protected mode", on the
           register state at a successful return:
               CS = 16-bit selector with base of real mode CS and a 64K limit
               SS = Selector with base of real mode SS and a 64K limit
               DS = Selector with base of real mode DS and a 64K limit
               ES = Selector to program's PSP with a 100h byte limit
           and, separately: "The environment pointer in the current program's PSP
           will automatically be converted to a descriptor."

           THIS IS NOT A SPEC DETAIL WE ARE HONOURING FOR TIDINESS -- it is what
           killed Doom for four sessions. DOS/4GW's PM module reads the
           environment field at +0x2c of whatever the initial ES selects and
           loads it as a SELECTOR (observed). With ES pointing at the data segment
           instead of the PSP, +0x2c is an arbitrary code byte pair -- measured as
           0x8b17, LDT index 4450 -- and that segment load #GPs, which XP answers by
           terminating the whole VDM with no
           exception we can catch. The client is thus its own second witness for
           BOTH halves of the rule, independently of the spec text.

           The environment field is left holding the SELECTOR from here on. The
           spec makes restoring it the client's job before it terminates ("it must
           restore it to the selector created by the DPMI host"), and nothing in
           our DOS layer reads PSP+0x2C -- dos_psp.h writes it once at load and no
           reader exists (checked). If one is ever added, it must not assume a
           segment after a DPMI switch. */
        { WORD psp = machine->PspSegment;
          DWORD pspBase = (DWORD)psp << PARAGRAPH_SHIFT;
          WORD pspSelector = 0, environmentSelector = 0;
          if (g_LdtNext < DPMI_LDT_MAX) {
              INT pspIndex = g_LdtNext++;
              g_Ldt[pspIndex].Base   = pspBase;
              g_Ldt[pspIndex].Limit  = DOS_PSP_SIZE - 1;        /* "a 100h byte limit", exactly */
              g_Ldt[pspIndex].Access = DPMI_ACCESS_DATA;        /* present, DPL3, data R/W       */
              g_Ldt[pspIndex].Flags  = 0;
              DpmiInstall(pspIndex);
              pspSelector = (WORD)DPMI_LDT_SELECTOR(pspIndex);
              VDM_SET16(tib, VTIB_ES, pspSelector);
          }
          { volatile WORD *environmentField = (volatile WORD *)(ULONG_PTR)(pspBase + DOS_PSP_ENVIRONMENT);
            WORD environmentSegment = *environmentField;
            /* environmentSegment == 0 is legal and documented: a client may free its
               environment and zero this word BEFORE switching, in which case
               there is nothing to convert and we must not invent a descriptor. */
            if (environmentSegment && g_LdtNext < DPMI_LDT_MAX) {
                INT entryIndex = g_LdtNext++;
                g_Ldt[entryIndex].Base   = (DWORD)environmentSegment << PARAGRAPH_SHIFT;
                g_Ldt[entryIndex].Limit  = DOS_PSP_SIZE - 1;      /* DosEnvBuild fills a 0x10-para block */
                g_Ldt[entryIndex].Access = DPMI_ACCESS_DATA;
                g_Ldt[entryIndex].Flags  = 0;
                DpmiInstall(entryIndex);
                environmentSelector = (WORD)DPMI_LDT_SELECTOR(entryIndex);
                *environmentField = environmentSelector;
            }
          }
          DpmiInstallDefaultPmHandlers(machine);
          cursor = LogPut(cursor, " PSP 0x"); cursor = LogHex(cursor, psp);
          cursor = LogPut(cursor, " -> ES=0x"); cursor = LogHex(cursor, pspSelector);
          cursor = LogPut(cursor, " env -> sel 0x"); cursor = LogHex(cursor, environmentSelector);
        }
        cursor = LogPut(cursor, " segbase C=0x"); cursor = LogHex(cursor, g_DpmiSegmentBase[0]);
        cursor = LogPut(cursor, " D=0x"); cursor = LogHex(cursor, g_DpmiSegmentBase[1]);
        cursor = LogPut(cursor, " S=0x"); cursor = LogHex(cursor, g_DpmiSegmentBase[2]);
        cursor = LogPut(cursor, " -> PM ok (CS=0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_CS));
        cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_EIP));
        cursor = LogPut(cursor, ") -> DPMI PM loop\r\n");
        LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
        /* run 53: the emulation path -- execute PM in the host interpreter instead of
           the kernel (which deadlocks on a PM #GP, run 52). No BOP patch: the interpreter
           reads the raw CD nn and stops on it, and we service through the same dispatch.
           No kernel watchdog here either -- the interpreter has its own guard cap, and the
           watchdog's 3s TerminateProcess would guillotine a long (millions-of-insn) run. */
        if (g_DpmiUseInterp) {
            cursor = LogPut(cursor, "DPMI: run 53 -- PM in host interpreter (no kernel, no BOP patch)\r\n");
            LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
            DpmiRunPmInterp(machine, tib);
            { *cursorIo = cursor; return HOST_FLOW_BREAK; }
        }
        /* Safety watchdog (kernel PM path only): an un-terminable spin still self-kills
           after ~3s so the batch dumps the log. */
        { HANDLE watchdogThread = CreateThread(NULL, 0, DpmiWatchdog,
                                   (LPVOID)(ULONG_PTR)g_DpmiWatchdogGeneration, 0, NULL);
          if (watchdogThread) CloseHandle(watchdogThread);
          /* Prove creation FROM THIS THREAD. The watchdog's own first line is
             written by the new thread, so its absence is ambiguous -- it cannot
             distinguish "thread never created" from "created but the process was
             killed before it was ever scheduled". Doom's log shows neither that
             line nor any sample, so we need the difference. */
          cursor = LogPut(cursor, "STAGE3-DPMI: watchdog thread created h="); cursor = LogHex(cursor, (DWORD)(ULONG_PTR)watchdogThread);
          cursor = LogPut(cursor, "\r\n");
          LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base; }
        cursor = DpmiPatchClientIntSitesUpFront(cursor, base);
        cursor = DpmiLoadSessionKnobs(cursor, base);
        cursor = DpmiInstallFaultReflect(cursor, base);

        DpmiRunClient(&cursor, base, tib, machine);
        {
            INT flow = DpmiEndClientSession(&cursor, base, tib, machine);
            if (flow == HOST_FLOW_BREAK) { *cursorIo = cursor; return HOST_FLOW_BREAK; }
            if (flow == HOST_FLOW_CONTINUE) { *cursorIo = cursor; return HOST_FLOW_CONTINUE; }
        }
    }
    cursor = LogPut(cursor, " -> SWITCH FAILED (staying real mode, CF=1)\r\n");
    LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
    VDM_REG(tib, VTIB_EFLAGS) |= EFLAGS_CF;         /* CF=1 signals failure to the client */
    VDM_REG(tib, VTIB_EIP) += VDM_BOP_LENGTH;            /* -> the RETF, returns real mode     */
    { *cursorIo = cursor; return HOST_FLOW_CONTINUE; }
    *cursorIo = cursor; return HOST_FLOW_NEXT;
}


static INT NtvdmCommandStartupBatch(PSTR *cursorIo, PSTR const base, const DWORD bopNumber, const DWORD sub, volatile BYTE * const tib, const INT quiet)
{
    PSTR cursor = *cursorIo;
    /* ── ★ sub 0D = "GIVE ME A PATH TO OPEN" -- THE STARTUP BATCH FILE. ─────
         The guest named this gap itself. With `/p` on its command line COMMAND.COM
         allocates a 7-paragraph block (`AH=48h -> 0x0D6D`), issues this BOP with
         `DS:DX` pointing into it, and its very next DOS call is `AH=3Dh` (open)
         on that buffer (our INT 21h log). We wrote nothing, so it opened "" and
         our DOS answered "path not found".
       ▸ Stock answers with a path, as an OEM string at DS:DX, of at most
         **0x40** bytes.
       ▸ Which path: stock's permanent shell runs AUTOEXEC.NT at start-up, and
         this is the `/p` (permanent shell) startup path. ⚠ That last step is an
         INFERENCE from context -- but it is verifiable by behaviour, because
         whatever we write here is the path the guest opens next.
       ⛔ WE DO NOT DEFAULT TO XP's AUTOEXEC.NT, deliberately. The real one loads
         `mscdexnt.exe`, `redir` and **`dosx`** -- NT's DPMI host, which we provide
         ourselves and which has no business being loaded into our VDM. The
         DOS-native `C:\AUTOEXEC.BAT` is the honest default for a DOS that is
         ours; `cfg\autoexec.txt` points it anywhere, including at NT's.
       ⚠ A path that does not exist is FINE and is the normal case -- a DOS with no
         AUTOEXEC.BAT simply has none. What was broken was the empty string. */
    if (bopNumber == NTVDM_BOP_CMD && sub == NTVDM_CMD_STARTUP_BATCH) {
        DWORD nameBase = (VDM_REG16(tib, VTIB_DS) << PARAGRAPH_SHIFT)
                 + VDM_REG16(tib, VTIB_EDX);
        volatile BYTE *nameBytes = (volatile BYTE *)(ULONG_PTR)nameBase;
        CHAR autoText[80]; DWORD autoLength = 0, item;
        HANDLE autoHandle = CreateFileA(BOPAUTO_PATH, GENERIC_READ, FILE_SHARE_READ,
                                NULL, OPEN_EXISTING, 0, NULL);
        if (autoHandle != INVALID_HANDLE_VALUE) {
            ReadFile(autoHandle, autoText, sizeof(autoText) - 1, &autoLength, NULL); CloseHandle(autoHandle);
            while (autoLength && (autoText[autoLength-1] == '\r' || autoText[autoLength-1] == '\n'
                          || autoText[autoLength-1] == ' ' || autoText[autoLength-1] == '\t')) --autoLength;
        }
        /* ── s92 (#316): AND IT RUNS WITH ECHO OFF, AS STOCK's DOES. XP leaves an
             EMPTY C:\AUTOEXEC.BAT on every machine; the shell runs it with echo ON,
             and the end of a batch with echo on is a blank line and the PROMPT --
             which `prog > file` from cmd captured ahead of the program's own output
             (dospair LM6: ours "\r\nC:\...\DOS>\n\r\n" first; stock nothing). Stock's
             AUTOEXEC.NT begins `@echo off`. So the default is now a two-line batch
             the host writes -- `@echo off` and a CALL of C:\AUTOEXEC.BAT if there
             is one -- so the user's batch still runs, silently. Its short path must
             fit XP's 0x3F cap; if it cannot be made, the old answer stands. */
        if (!autoLength) {
            static CHAR wrap[MAX_PATH];
            if (!wrap[0]) {
                CHAR tempDirectory[MAX_PATH], full[MAX_PATH], shortPath[MAX_PATH];
                DWORD tempLength = GetTempPathA(sizeof tempDirectory, tempDirectory), shortTempLength;
                HANDLE writeHandle;
                if (tempLength && tempLength < sizeof tempDirectory - 16) {
                    wsprintfA(full, HOST_STARTUP_BATCH_FORMAT, tempDirectory);
                    writeHandle = CreateFileA(full, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                    if (writeHandle != INVALID_HANDLE_VALUE) {
                        static const CHAR body[] =
                            HOST_STARTUP_BATCH_BODY;
                        DWORD bytesWritten = 0;
                        WriteFile(writeHandle, body, sizeof body - 1, &bytesWritten, NULL);
                        CloseHandle(writeHandle);
                        shortTempLength = GetShortPathNameA(full, shortPath, sizeof shortPath);
                        if (shortTempLength && shortTempLength <= NTVDM_CMD_STARTUP_PATH_MAX && bytesWritten == sizeof body - 1)
                            lstrcpynA(wrap, shortPath, sizeof wrap);
                    }
                }
                if (!wrap[0]) lstrcpynA(wrap, HOST_AUTOEXEC_PATH, sizeof wrap);
            }
            for (autoLength = 0; wrap[autoLength] && autoLength < sizeof autoText - 1; ++autoLength) autoText[autoLength] = wrap[autoLength];
        }
        if (autoLength > NTVDM_CMD_STARTUP_PATH_MAX) autoLength = NTVDM_CMD_STARTUP_PATH_MAX;          /* XP's own cap */
        for (item = 0; item < autoLength; ++item) nameBytes[item] = (BYTE)autoText[item];
        nameBytes[autoLength] = 0;
        if (!quiet) {
            cursor = LogPut(cursor, "         sub 0D answered: startup batch [");
            for (item = 0; item < autoLength; ++item) { CHAR piece[2]; piece[0]=autoText[item]; piece[1]=0; cursor = LogPut(cursor, piece); }
            cursor = LogPut(cursor, "] at 0x"); cursor = LogHex(cursor, nameBase); cursor = LogPut(cursor, "\r\n");
            LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
        }
        VDM_REG(tib, VTIB_EFLAGS) &= ~EFLAGS_CF_U;
        VDM_REG(tib, VTIB_EIP) += VDM_BOP_SUBFUNCTION_LENGTH;
        { *cursorIo = cursor; return HOST_FLOW_CONTINUE; }
    }
    *cursorIo = cursor; return HOST_FLOW_NEXT;
}


static INT NtvdmCommandPrompt(PSTR *cursorIo, PSTR const base, const DWORD bopNumber, const DWORD sub, volatile BYTE * const tib, const INT quiet)
{
    PSTR cursor = *cursorIo;
    /* ── ★ sub 0F = THE HOST'S `PROMPT`, AND IT ANSWERS IN BX. ──────────────
         Stock passes the HOST's `PROMPT` environment variable through to the DOS
         shell here, and reports in BX; with nothing to pass it answers BX = 0.
       ▸ BX = 0, which is exactly stock's answer when it has nothing to pass. We were
       setting no register at all, so the guest read whatever
         BX happened to hold -- an unimplemented call answering at random again. */
    /* ── ★ sub 0F = "GIVE ME THE INITIAL ENVIRONMENT". (s81: the `C>` prompt) ──
         Under /P, XP's COMMAND.COM builds a FRESH environment, as DOS's primary
         shell does -- `PATH=` and a COMSPEC, nothing else -- and asks NTVDM for the
         rest here (stock hands it the Win32 environment, PROMPT included). We said
         "none", so the user's shell came up `C>`: DOS's default, with no PROMPT.
         The protocol, as the shell drives it (observed, two calls):
           call 1: BX=0 in  -> BX out = EXTRA paragraphs needed (0 = keep the old
                   environment); the shell grows its block by that much
           call 2: ES:0 = the new block, BX = its size in paragraphs
                   -> the variables, double-NUL ended; BX out = paragraphs used,
                   which must not exceed what came in (else it gives up)
         We answer with the environment we built for it (seg DOS_ENV_SEG: PROMPT,
         PATH, BLASTER, ULTRASND...), snapshotted on call 1 while it is intact, with
         COMSPEC pointed at the real shell. */
    if (bopNumber == NTVDM_BOP_CMD && sub == NTVDM_CMD_PROMPT) {
        static CHAR environmentSnapshot[1024]; static DWORD environmentSnapshotLength;
        DWORD initialBx = VDM_REG16(tib, VTIB_EBX);
        if (initialBx == 0) {
            const volatile BYTE *environment0 = (const volatile BYTE *)(ULONG_PTR)((DWORD)DOS_ENV_SEG << PARAGRAPH_SHIFT);
            DWORD inputIndex = 0, outputIndex = 0;
            while (inputIndex < ENVIRONMENT_SCAN_MAX && !(environment0[inputIndex] == 0 && environment0[inputIndex + 1] == 0)) {
                DWORD environmentStart = inputIndex;
                while (environment0[inputIndex] && inputIndex < ENVIRONMENT_SCAN_MAX) ++inputIndex;
                /* ── PATH IS WINDOWS' PATH, IN 8.3. (s81, user: "mem" -> "Bad command
                     or file name") We handed the shell `PATH=C:\`, so nothing in
                     SYSTEM32 -- MEM, EDIT, DEBUG, every XP DOS tool -- could be run by
                     name. Stock passes the Win32 environment; this passes its PATH,
                     each entry shortened (a DOS program cannot open a long name) and
                     the whole kept under 250 characters, dropping entries past that
                     rather than cutting one in half. */
                if ((environment0[environmentStart] | ASCII_CASE_BIT) == 'p' && (environment0[environmentStart + 1] | ASCII_CASE_BIT) == 'a' &&
                    (environment0[environmentStart + 2] | ASCII_CASE_BIT) == 't' && (environment0[environmentStart + 3] | ASCII_CASE_BIT) == 'h' &&
                    environment0[environmentStart + 4] == '=') {
                    CHAR writeBuffer[2048], shellPath[MAX_PATH]; DWORD writeLength, start = outputIndex, length0 = 0;
                    PSTR start0 = writeBuffer, limit;
                    writeLength = GetEnvironmentVariableA(HOST_ENV_PATH, writeBuffer, sizeof writeBuffer);
                    outputIndex = (DWORD)(LogPut(environmentSnapshot + outputIndex, HOST_ENV_PATH_ASSIGN) - environmentSnapshot);
                    if (writeLength && writeLength < sizeof writeBuffer) {
                        while (*start0) {
                            DWORD shortLength;
                            limit = start0; while (*limit && *limit != ';') ++limit;
                            if (*limit) *limit++ = 0; else limit = start0 + lstrlenA(start0);
                            shortLength = *start0 ? GetShortPathNameA(start0, shellPath, sizeof shellPath) : 0;
                            if (shortLength && shortLength < sizeof shellPath && (outputIndex - start) + shortLength + 1 < PATH_VALUE_MAX) {
                                if (length0++) environmentSnapshot[outputIndex++] = ';';
                                outputIndex = (DWORD)(LogPut(environmentSnapshot + outputIndex, shellPath) - environmentSnapshot);
                            }
                            start0 = limit;
                        }
                    }
                    if (!length0) outputIndex = (DWORD)(LogPut(environmentSnapshot + outputIndex, HOST_DEFAULT_DRIVE_ROOT) - environmentSnapshot);
                } else
                if ((environment0[environmentStart] | ASCII_CASE_BIT) == 'c' && environment0[environmentStart + 7] == '=' &&
                    (environment0[environmentStart + 1] | ASCII_CASE_BIT) == 'o' && (environment0[environmentStart + 2] | ASCII_CASE_BIT) == 'm') {
                    outputIndex = (DWORD)(LogPut(environmentSnapshot + outputIndex, HOST_ENV_COMSPEC_ASSIGN) - environmentSnapshot);
                    outputIndex = (DWORD)(LogPut(environmentSnapshot + outputIndex, g_ShellPath[0] ? g_ShellPath
                                                   : HOST_DEFAULT_SHELL_PATH) - environmentSnapshot);
                } else {
                    DWORD snapshotIndex; for (snapshotIndex = environmentStart; snapshotIndex < inputIndex; ++snapshotIndex) environmentSnapshot[outputIndex++] = (CHAR)environment0[snapshotIndex];
                }
                environmentSnapshot[outputIndex++] = 0;
                ++inputIndex;
            }
            environmentSnapshot[outputIndex++] = 0;
            environmentSnapshotLength = outputIndex;
            VDM_SET16(tib, VTIB_EBX, (WORD)((environmentSnapshotLength + PARAGRAPH_LAST_BYTE) / PARAGRAPH_SIZE + 1));
        } else {
            volatile BYTE *environment1 = (volatile BYTE *)(ULONG_PTR)(VDM_REG16(tib, VTIB_ES) << PARAGRAPH_SHIFT);
            DWORD paragraph, used = (environmentSnapshotLength + PARAGRAPH_LAST_BYTE) / PARAGRAPH_SIZE;
            if (environmentSnapshotLength && used <= initialBx) {
                for (paragraph = 0; paragraph < environmentSnapshotLength; ++paragraph) environment1[paragraph] = (BYTE)environmentSnapshot[paragraph];
                VDM_SET16(tib, VTIB_EBX, (WORD)used);
            } else VDM_SET16(tib, VTIB_EBX, 0);
        }
        if (!quiet) {
            cursor = LogPut(cursor, initialBx ? "         sub 0F (2/2): environment written, paras=0x"
                            : "         sub 0F (1/2): environment needs extra paras=0x");
            cursor = LogHex(cursor, VDM_REG16(tib, VTIB_EBX)); cursor = LogPut(cursor, "\r\n");
            LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
        }
        VDM_REG(tib, VTIB_EFLAGS) &= ~EFLAGS_CF_U;
        VDM_REG(tib, VTIB_EIP) += VDM_BOP_SUBFUNCTION_LENGTH;
        { *cursorIo = cursor; return HOST_FLOW_CONTINUE; }
    }
    *cursorIo = cursor; return HOST_FLOW_NEXT;
}


/* An NTVDM BOP from the guest's own code (XP's COMMAND.COM is NTVDM-aware): service its command-shell
   sub-functions -- the next command to run, termination, the startup batch file, the prompt, the
   query bits and the keyboard configuration. */
static INT NtvdmServiceGuestBop(PSTR *cursorIo, PSTR const base, volatile BYTE * const tib, DOS_MACHINE *machine, CHAR *programPathBuffer)
{
    PSTR cursor = *cursorIo;
        DWORD bopNumber  = VDM_REG(tib, VTIB_EVENT_INFO) & BYTE_MASK;
        DWORD codeSegment = VDM_REG16(tib, VTIB_CS), instructionPointer = VDM_REG16(tib, VTIB_EIP);
        const volatile BYTE *isvBopBytes = (const volatile BYTE *)(ULONG_PTR)((codeSegment << PARAGRAPH_SHIFT) + instructionPointer);
        DWORD sub = isvBopBytes[VDM_BOP_LENGTH];
        static INT  carryPolicy = -1;                 /* -1 = not yet read */
        INT quiet = 0;                           /* rate-limit the LOG, never the answer */
        if (carryPolicy < 0) {
            CHAR text[16]; DWORD bytesRead = 0;
            HANDLE handle = CreateFileA(BOP54_PATH, GENERIC_READ, FILE_SHARE_READ,
                                   NULL, OPEN_EXISTING, 0, NULL);
            /* DEFAULT CF=0, and that is measured rather than chosen: with CF=1 the
               sub-01 site's `jnc` falls into a retry and COMMAND.COM POLLS the call
               forever (first run: 268,435,180 bytes of log, one line per spin). With
               CF=0 it proceeds to a second, different call -- sub 0x0E at 0x5E6.
               Neither is known to be RIGHT; one is known to be a dead end. */
            carryPolicy = 0;
            if (handle != INVALID_HANDLE_VALUE) {
                ReadFile(handle, text, sizeof text - 1, &bytesRead, NULL); CloseHandle(handle);
                if (bytesRead >= 3 && text[0] == 'c' && text[1] == 'f' && text[2] == '1') carryPolicy = 1;
            }
        }
        /* ── ⛔ RATE-LIMITED, AND IT COST 256 MB TO LEARN. ───────────────────────
             The first run of this instrument answered CF=1, COMMAND.COM POLLED the
             call, and the unlimited log reached 268,435,180 bytes -- one line per
             iteration of a loop that never ended. An instrument that scales with a
             guest's spin rate is a denial-of-service on the thing you are trying to
             read. 16 in full, then count only; the total goes in the summary.
           ⚠ The 17th call is not less interesting than the 16th -- if the values
             ever CHANGE after the cap this will not show it. It logs a resumed line
             when the register signature differs from the last one printed, so a
             state change still surfaces while a spin does not. */
        {   DWORD signature = VDM_REG16(tib, VTIB_EAX)
                      ^ (VDM_REG16(tib, VTIB_EBX) << PARAGRAPH_SHIFT)
                      ^ (VDM_REG16(tib, VTIB_ECX) << BYTE_SHIFT)
                      ^ (VDM_REG16(tib, VTIB_EDX) << 12) ^ (sub << 20);
            static DWORD lastSignature = 0xFFFFFFFFu;
            INT novel = (signature != lastSignature);
            lastSignature = signature;
            ++g_NtvdmBopCount;
            /* ── ⛔⛔ A NOVELTY FILTER IS NOT A CAP, AND IT COST A SECOND 256 MB.
                 The first rate-limit logged 16 in full and then only when the
                 register signature CHANGED. That is the right shape for a spin on
                 identical values -- and no defence at all against a LOOP, where
                 every pass differs slightly: once COMMAND.COM reached its command
                 loop the log hit 268,435,219 bytes again, 285,698 BOPs.
               ⇒ A hard ceiling as well as a novelty test. Past it, count only. */
            /* ⛔⛔ AND IT MUST GATE THE LOG ONLY, NEVER THE HANDLING. The first cut
                 `continue`d out of the rate-limit arm, which SKIPPED the sub 01 and
                 sub 0E handlers below and answered with the generic CF instead --
                 so past the 17th call the guest was being told something different
                 from what the first sixteen were told, silently. A quiet instrument
                 that also changes behaviour is not an instrument. */
            quiet = (g_NtvdmBopCount > 64) || (g_NtvdmBopCount > 16 && !novel);
        }
        if (quiet) goto ntvdmBopDispatch;
        cursor = LogPut(cursor, "STAGE2: NTVDM BOP from guest: bop=0x"); cursor = LogHexByte(cursor, bopNumber);
        cursor = LogPut(cursor, " sub=0x"); cursor = LogHexByte(cursor, sub);
        cursor = LogPut(cursor, " at 0x"); cursor = LogHex(cursor, codeSegment); cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, instructionPointer);
        cursor = LogPut(cursor, " ax=0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_EAX));
        cursor = LogPut(cursor, " bx=0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_EBX));
        cursor = LogPut(cursor, " cx=0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_ECX));
        cursor = LogPut(cursor, " dx=0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_EDX));
        cursor = LogPut(cursor, " si=0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_ESI));
        cursor = LogPut(cursor, " di=0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_EDI));
        cursor = LogPut(cursor, " ds=0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_DS));
        cursor = LogPut(cursor, " es=0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_ES));
        cursor = LogPut(cursor, " ss:sp=0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_SS));
        cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_ESP));
        cursor = LogPut(cursor, " -> UNIMPLEMENTED, answering CF="); cursor = LogDecimal(cursor, (UINT)carryPolicy);
        cursor = LogPut(cursor, "\r\n");
        /* And the bytes it is about to run either way -- the branch is right there,
           and which way it goes is the whole question. */
        cursor = LogPut(cursor, "         next="); cursor = LogDump(cursor, (const VOID *)(isvBopBytes + VDM_BOP_SUBFUNCTION_LENGTH), 12);
        /* ── ★ COMMAND.COM's STATE BLOCK, WHOLE, RATHER THAN ONE BYTE AT A TIME.
             Its decisions about being a shell follow a handful of bytes in its
             RESIDENT data -- around 0x2B0 and 0x320..0x333 of the
             resident segment, 0x0100 for a .COM: the banner, "ask for a command"
             vs "prompt", and the keyboard read are all gated in that block.
           ⇒ Printing all of them at once turns "find the next gate, answer it,
             re-run" into one reading. Five turns of that pattern produced one
             caveat; this is the instrument that should have come first.
           ⚠ The segment is ASSUMED to be 0x0100 (a .COM's PSP). If COMMAND.COM is
             ever loaded elsewhere these rows are somebody else's memory -- the
             `@0100:` in the trace's call sites is the check that it is not. */
        { const volatile BYTE *lowPspBytes = (const volatile BYTE *)(ULONG_PTR)(0x0100u << PARAGRAPH_SHIFT);
          cursor = LogPut(cursor, "\r\n         cc[0x2B0..0x2BF]="); cursor = LogDump(cursor, (const VOID *)(lowPspBytes + 0x2B0), 16);
          cursor = LogPut(cursor, "\r\n         cc[0x320..0x333]="); cursor = LogDump(cursor, (const VOID *)(lowPspBytes + 0x320), 20);
          /* s81: the environment the shell ACTUALLY has (PSP:2Ch), as text -- the
             prompt came up `C>` under /P, i.e. without the PROMPT we passed. */
          { WORD pspEnvironmentSegment = *(const volatile WORD *)(lowPspBytes + DOS_PSP_ENVIRONMENT); INT scan;
            const volatile BYTE *environment2 = (const volatile BYTE *)(ULONG_PTR)((DWORD)pspEnvironmentSegment << PARAGRAPH_SHIFT);
            cursor = LogPut(cursor, "\r\n         shell env seg=0x"); cursor = LogHex(cursor, pspEnvironmentSegment); cursor = LogPut(cursor, " [");
            for (scan = 0; scan < 160 && !(environment2[scan] == 0 && environment2[scan + 1] == 0); ++scan) {
                CHAR character1[2]; character1[0] = environment2[scan] ? (CHAR)environment2[scan] : '|'; character1[1] = 0;
                if (character1[0] < 0x20 || character1[0] > 0x7e) character1[0] = '.';
                cursor = LogPut(cursor, character1); }
            cursor = LogPut(cursor, "]"); } }
        cursor = LogPut(cursor, "\r\n");
        LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
    ntvdmBopDispatch:
        /* ── ★ sub 01 = "WHAT SHOULD I RUN NEXT?" -- ANSWER "NOTHING". ───────────
             On XP this is answered from CSRSS's GetNextVDMCommand (VDM_COMMAND_INFO).
             The guest's request block is at DS:DX -- (DS<<4)+DX, exactly as we
             compute it here -- and the answer comes back in the same block.
           ▸ The fields (see docs/inventory/bop.md):
               +0x10 w   OUT  flags; zero is "nothing to do" (observed: the shell
                              then goes to its prompt).
               +0x12 dw  IN+OUT a cookie. COMMAND.COM fills it before the call and
                              takes back whatever is there afterwards -- so it must
                              ROUND-TRIP.
               +0x1A w   OUT  COMMAND.COM keeps the low byte.
               +0x02 +0x04 +0x06 +0x16 +0x20   OUT
               +0x22 w   OUT  status; stock writes 4, 8 or 9 here.
           ⚠ WHAT WE WRITE IS A DEFINED "NO COMMAND", NOT A DECODED ONE. That is a
             real claim and it may be wrong -- but the alternative is not neutral:
             leaving the block ALONE hands COMMAND.COM whatever was in its own
             memory, which is how it got a garbage answer and spun. A defined answer
             is falsifiable; an uninitialised one is not.
           ⛔ The cookie is preserved rather than zeroed, because the guest reloads
             it into its own state unconditionally -- zeroing it would destroy
             something we were only asked to carry. */
        if (bopNumber == NTVDM_BOP_CMD && sub == NTVDM_CMD_NEXT_COMMAND) {
            DWORD block = (VDM_REG16(tib, VTIB_DS) << PARAGRAPH_SHIFT)
                      + VDM_REG16(tib, VTIB_EDX);
            volatile BYTE *blockBytes = (volatile BYTE *)(ULONG_PTR)block;
            /* ── ★ THE SECOND "WHAT NEXT?" IS THE SHELL LEAVING. (s81 sweep: `exit`) ──
                 The block is NT's CMDINFO -- +04 CurDrive, +0C CmdLineSize, +0E
                 ReturnCode, +18 fTSRExit, +1C:1E/+20 ExecPath -- every measured value
                 lines up. Sub 01 is GetNextVDMCommand: the DOS side has finished and
                 asks the Win32 side for work. XP's permanent shell asks once at start-
                 up and again when the user types EXIT (never after an internal or an
                 EXEC'd command -- measured: `dir` does not call it). A bare-launched
                 session has no Win32 side to hand anything back, so the second ask
                 means "we are done": end the VDM with the shell's ReturnCode, as stock
                 ends a command.com window. */
            /* #208: the routed program was ENDED BY CLOSE PROGRAM -- the user asked for the
                 prompt, not for the window to close. Go interactive from here (AH=53h
                 AL=2 CF=1, the shell's own prompt path) and answer "nothing", once; the
                 shell's NEXT sub 01 is then an ordinary EXIT. */
            if (g_GuestNtAware && g_BackToPrompt && g_ShellGetNextCount >= 1) {
                g_BackToPrompt = 0;
                g_DosInt53Answers[DOS_INT53_SHELL_LOOP].IsCarry = 1;
                cursor = LogPut(cursor, "         sub 01 after Close Program: back to the prompt (#208)\r\n");
                LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
            } else
            if (g_GuestNtAware && ++g_ShellGetNextCount > 1) {
                cursor = LogPut(cursor, "         sub 01 again: the shell is handing back control (EXIT)"
                            " -- ending the VDM, rc=0x");
                cursor = LogHex(cursor, *(volatile WORD *)(blockBytes + NTVDM_CMD_BLOCK_EXIT_CODE)); cursor = LogPut(cursor, "\r\n");
                LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
                machine->ExitCode = *(volatile WORD *)(blockBytes + NTVDM_CMD_BLOCK_EXIT_CODE) & BYTE_MASK;
                { *cursorIo = cursor; return HOST_FLOW_BREAK; }
            }
            #define BW(o, v) (*(volatile WORD *)(blockBytes + (o)) = (WORD)(v))
            /* ⚠ DUMPED BEFORE WE WRITE ANYTHING. Logging it after the BW()s below
                 would show our own zeros back and read as the guest's input --
                 which is the whole class of mistake this file keeps catching. */
            if (quiet) goto blockWritten;
            cursor = LogPut(cursor, "         blk in="); cursor = LogDump(cursor, (const VOID *)blockBytes, 0x28);
            cursor = LogPut(cursor, "\r\n         +1C:1E=0x");
            cursor = LogHex(cursor, *(volatile WORD *)(blockBytes + NTVDM_CMD_BLOCK_NAME_SEGMENT)); cursor = LogPut(cursor, ":0x");
            cursor = LogHex(cursor, *(volatile WORD *)(blockBytes + NTVDM_CMD_BLOCK_NAME_OFFSET));
            cursor = LogPut(cursor, " +20=0x"); cursor = LogHex(cursor, *(volatile WORD *)(blockBytes + NTVDM_CMD_BLOCK_NAME_CAPACITY));
            cursor = LogPut(cursor, "\r\n");
            blockWritten: ;
            /* ── ★★ TOUCH AS LITTLE AS POSSIBLE. ────────────────────────────────
                 The first cut zeroed every field that XP's handler writes. One of
                 them, `+0x04`, is the DEFAULT DRIVE -- COMMAND.COM selects it with
                 `AH=0Eh` immediately after the call (our INT 21h log shows the DL it
                 passes) -- so a zero meant drive 0 = A:, and our own handler
                 said so in the same log ("drive A: exists but is not ready ...
                 selected as the DOS current drive anyway") while the shell went
                 quiet.
               ★ Dumping the block BEFORE writing it settled the design: the guest
                 already supplies `+0x04 = 02` (C:). It was never ours to fill in.
                 Measured, one run:
                   blk in = 26 02 | 00 01 | 02 00 | 00 00 | 42 93 | 27 93 | 80 00 ...
                 ⇒ `+0x08:+0x0A = 0x9342:0x9327` -- and `0x9327` is EXACTLY the
                   buffer COMMAND.COM reads its command line from (it is also the
                   DS:DX it later hands to INT 21h AH=0Ah, in our log). Two
                   independent sources, same address.
               ⇒ So we now write only what a "no command" answer really is: the
                 empty command tail, and the flags word. Everything else is left as
                 the guest set it, because a field we cannot name is not ours. */
            {   DWORD codeBase = ((DWORD)(*(volatile WORD *)(blockBytes + NTVDM_CMD_BLOCK_TAIL_SEGMENT)) << PARAGRAPH_SHIFT)
                         + *(volatile WORD *)(blockBytes + NTVDM_CMD_BLOCK_TAIL_OFFSET);
                volatile BYTE *codeBlock = (volatile BYTE *)(ULONG_PTR)codeBase;
                /* ── AN EMPTY DOS COMMAND TAIL, AND THE CR IS THE POINT. ────────
                     COMMAND.COM scans this buffer for the terminating CR (observed).
                     With no CR in the buffer the scan walks the WHOLE 64K segment and
                     never leaves -- which is precisely the "spinning in
                     V86 with no traps" the headless deadline was killing.

                   ⛔⛔⛔ AND `[0]` IS NOT OURS. IT IS DOS'S AH=0Ah MAXIMUM, AND
                     WRITING IT COST THE INTERACTIVE PROMPT. (s79)
                     This used to write `c[0] = length`, on the reading that the
                     shell copies [0]+3 bytes of it. That was not wrong about the copy
                     and was completely wrong about the buffer, because **the same
                     buffer is handed to INT 21h AH=0Ah** (DX=0x9327 in our log), and
                     the shell sets its [0] to 0x80 ONCE, at start-up (observed in the
                     block before our first answer). 0x80 is the buffered-input
                     MAXIMUM, set a single time and never re-set. Our "empty tail" answer
                     zeroed it on the first BOP, so
                     every later AH=0Ah saw a zero-capacity buffer, returned an empty
                     line immediately, and COMMAND.COM printed its prompt again --
                     991 prompts in one 30-second run, with `INT21 AH=0A line max=00`
                     in the log the whole time. The shell was AT the keyboard read;
                     we were answering it with EOF.
                     ⚠ The `+0x0C` field of the request block is 0x0080 -- the guest
                       tells us the capacity there. Two independent sources, and the
                       one we overwrote was the same number.
                   ⇒ WRITE THE LENGTH AT [1], WHERE DOS PUTS IT, AND NEVER TOUCH [0].
                     Nothing downstream needs the length: the shell copies [0]+3 =
                     0x83 bytes (a superset) and finds the end of the text by its CR
                     (observed), so the CR is the only load-bearing byte.
                     Layout: [0] = max (the GUEST's, leave alone), [1] = length,
                             [2..] = text, then CR. */
                /* ── ONE COMMAND, ONCE, FROM cfg\bopcmd.txt. ──────────────────
                     An empty answer proves only that the guest accepted one. This
                     hands it a REAL command line exactly once and empties every
                     reply after -- so the log shows whether the whole path works
                     (does it execute and PRINT?) without the command repeating for
                     ever in a loop that already runs 1.25M times a run.
                   ⚠ One-shot on purpose: a guest that asks again must not be given
                     the same command again. That is the difference between testing
                     the mechanism and building a fork bomb. */
                static INT commandDone = 0;
                CHAR commandBuffer[128]; DWORD commandLength = 0;
                if (!commandDone && g_Routed) {
                    /* #208: the program's arguments, as the shell's tail -- it passes
                       them on as the program's own PSP command tail. */
                    /* ⚠ THE TAIL IS THE WHOLE COMMAND LINE, VERB FIRST; the NAME field
                         is only the already-resolved path to run it with. Measured two
                         ways: tail "hello" + name COMMAND.COM EXEC'd COMMAND.COM, and
                         tail " " + name HELLO.COM ran NOTHING (a blank line is "no
                         command" and the shell went to its prompt). */
                    commandDone = 1;
                    {   PSTR writeCursor = commandBuffer, limit = commandBuffer + sizeof(commandBuffer) - 2;
                        PCSTR firstProgram = g_FirstProgram;
                        while (*firstProgram && writeCursor < limit) *writeCursor++ = *firstProgram++;
                        if (g_FirstTail[0] && writeCursor < limit) {
                            PCSTR firstTail = g_FirstTail;
                            if (*firstTail != ' ') *writeCursor++ = ' ';
                            while (*firstTail && writeCursor < limit) *writeCursor++ = *firstTail++;
                        }
                        commandLength = (DWORD)(writeCursor - commandBuffer); }
                }
                if (!commandDone) {
                    HANDLE bopCommandFile = CreateFileA(BOPCMD_PATH, GENERIC_READ, FILE_SHARE_READ,
                                             NULL, OPEN_EXISTING, 0, NULL);
                    commandDone = 1;
                    if (bopCommandFile != INVALID_HANDLE_VALUE) {
                        ReadFile(bopCommandFile, commandBuffer, sizeof(commandBuffer) - 1, &commandLength, NULL);
                        CloseHandle(bopCommandFile);
                        while (commandLength && (commandBuffer[commandLength-1] == '\r' || commandBuffer[commandLength-1] == '\n')) --commandLength;
                    }
                }
                if (commandLength) {
                    DWORD item;
                    codeBlock[DOS_LINE_INPUT_LENGTH] = (BYTE)commandLength;                /* [0] is the guest's AH=0Ah max */
                    for (item = 0; item < commandLength; ++item) codeBlock[DOS_LINE_INPUT_TEXT + item] = (BYTE)commandBuffer[item];
                    codeBlock[DOS_LINE_INPUT_TEXT + commandLength] = ASCII_CR;
                } else {
                    codeBlock[DOS_LINE_INPUT_LENGTH] = 0; codeBlock[DOS_LINE_INPUT_TEXT] = ASCII_CR;          /* ditto: [0] is NOT ours */
                }
                if (!quiet) { cursor = LogPut(cursor, "         cmdline buf 0x"); cursor = LogHex(cursor, codeBase);
                              if (commandLength) { cursor = LogPut(cursor, " <- ["); 
                                        { DWORD item; for (item = 0; item < commandLength; ++item)
                                              { CHAR piece[2]; piece[0]=commandBuffer[item]; piece[1]=0; cursor = LogPut(cursor, piece); } }
                                        cursor = LogPut(cursor, "] ONE SHOT"); }
                              else      cursor = LogPut(cursor, " <- empty tail (len=0, CR)");
                              cursor = LogPut(cursor, "\r\n"); }
            }
            BW(NTVDM_CMD_BLOCK_REDIRECTION, 0);                       /* flags: nothing was redirected */
            /* ── ★ AND THE OTHER TWO HALVES OF THE ANSWER. ──────────────────────
                 sub 01 returns THREE things, not one: a command TAIL (+0x08:+0x0A,
                 written above), a program NAME (+0x1C:+0x1E, capacity +0x20), and
                 that program's TYPE at +0x22. Stock picks the type from the name's
                 extension (observed per program type):
                   `.EXE` -> 4   `.COM` -> 8   `.BAT` -> 2   shorter than 7 -> 9
                 -- a file-extension dispatch and not a status word. ⛔ I had guessed
                 "status enumeration"; it is not.
               ▸ A zero-length name therefore means type 9, and the guest agrees:
                 on the no-program path it writes a 0 to the FIRST BYTE of the name
                 buffer at 0x9473 (observed) -- which is exactly the +0x1C:+0x1E we
                 are handed (`+1C:1E=0x9342:0x9473`).
               ⚠ We had been leaving both alone, i.e. handing the shell whatever was
                 in its own memory. `ver` and `dir` worked anyway -- a builtin needs
                 only the tail -- but that was luck, not an answer. */
            {   DWORD nameBase = ((DWORD)(*(volatile WORD *)(blockBytes + NTVDM_CMD_BLOCK_NAME_SEGMENT)) << PARAGRAPH_SHIFT)
                         + *(volatile WORD *)(blockBytes + NTVDM_CMD_BLOCK_NAME_OFFSET);
                DWORD capacity = *(volatile WORD *)(blockBytes + NTVDM_CMD_BLOCK_NAME_CAPACITY);
                volatile BYTE *nameBytes = (volatile BYTE *)(ULONG_PTR)nameBase;
                /* ── ★ HAND BACK WHAT CSRSS NAMED -- THE SAME SOURCE XP USES. ────
                     On XP this buffer is filled from the VDM_COMMAND_INFO that
                     GetNextVDMCommand returned, and our STAGE1 already made that
                     exact call: `STAGE1: command fetch ... app=[...]` lands in
                     g_Application2. Returning it is not a guess about what the shell wants;
                     it is the same answer from the same place.
                   ▸ ONCE. The first call is the VDM's reason for existing; after
                     that there is nothing more to run and the name is empty. A
                     shell that is handed the same program every time it asks would
                     exec it for ever.
                   ⚠ The type is stock's own rule (see above): the last four
                     characters, `.EXE`->4 `.COM`->8 `.BAT`->2, and anything
                     shorter than 7 characters -> 9. Mirrored, not invented. */
                static INT namedOnce = 0;
                /* ⚠ programPathBuffer FIRST, not g_Application2. On the rig CSRSS names
                   `dosstub.com` -- the harness stub -- and `target.txt` names the
                   real program, so g_Application2 would hand the shell the stub. programPathBuffer
                   is what we actually LOADED, which is the program either way. */
                PCSTR programPath = g_Routed ? g_FirstProgram      /* #208: the program */
                               : programPathBuffer[0] ? programPathBuffer : (g_Application2[0] ? g_Application2 : "");
                DWORD al = 0, valueType = NTVDM_CMD_TYPE_OTHER;
                if (!namedOnce && programPath[0]) {
                    while (programPath[al] && al + 1 < capacity && al < MAX_PATH) ++al;
                    namedOnce = 1;
                }
                if (al > NTVDM_CMD_SHORT_NAME_MAX) {
                    CHAR extension[DOS_DOT_EXTENSION_LENGTH + 1]; INT item;
                    for (item = 0; item < DOS_DOT_EXTENSION_LENGTH; ++item) {
                        CHAR character = programPath[al - DOS_DOT_EXTENSION_LENGTH + item];
                        extension[item] = (character >= 'a' && character <= 'z') ? (CHAR)(character - ASCII_CASE_BIT) : character;
                    }
                    extension[DOS_DOT_EXTENSION_LENGTH] = 0;
                    if      (extension[1]=='E' && extension[2]=='X' && extension[3]=='E' && extension[0]=='.') valueType = NTVDM_CMD_TYPE_EXE;
                    else if (extension[1]=='C' && extension[2]=='O' && extension[3]=='M' && extension[0]=='.') valueType = NTVDM_CMD_TYPE_COM;
                    else if (extension[1]=='B' && extension[2]=='A' && extension[3]=='T' && extension[0]=='.') valueType = NTVDM_CMD_TYPE_BAT;
                }
                { DWORD item; for (item = 0; item < al; ++item) nameBytes[item] = (BYTE)programPath[item]; nameBytes[al] = 0; }
                BW(NTVDM_CMD_BLOCK_PROGRAM_TYPE, valueType);
            /* ── +0x1A GATES THE INTERACTIVE PATH, so it is not a field we may
                 leave alone. COMMAND.COM keeps its low byte, and a non-zero value
                 takes it AWAY from the prompt. Zero. */
            BW(NTVDM_CMD_BLOCK_KEYBOARD_GATE, 0);
                if (!quiet) {
                    cursor = LogPut(cursor, "         prog name <- ["); 
                    { DWORD item; for (item = 0; item < al; ++item) { CHAR piece[2]; piece[0]=programPath[item]; piece[1]=0; cursor = LogPut(cursor, piece); } }
                    cursor = LogPut(cursor, "] type="); cursor = LogDecimal(cursor, valueType); cursor = LogPut(cursor, "\r\n");
                }
            }
            #undef BW
            /* ── ★★★ +0x12 IS NOT A COOKIE. IT IS THE INTERACTIVE SWITCH. ───────
                 I called it a cookie because COMMAND.COM fills it from its own
                 state before the call and takes it straight back after -- which
                 looks exactly like carrying an opaque handle. It is not: with it
                 zero the shell goes to its keyboard prompt (AH=0Ah), and with it
                 non-zero it does not (observed).
                 ⇒ a ZERO dword means "nothing is driving me: read from the keyboard".
               ★ And stock answers zero there in exactly this case -- when nothing is
                 redirected (flags at +0x10 zero).
               ⛔ PRESERVING IT WAS THE BUG. "Round-trip the value you were only
                 asked to carry" is a good instinct and it was wrong here: the guest
                 re-loads its own non-zero state, we hand it straight back, and it
                 concludes it is being driven -- for ever. 1.25M calls a run.
               ⇒ Zero, and only because the flags are zero: the two move together
                 in stock's answers and must stay tied together here. */
            *(volatile DWORD *)(blockBytes + NTVDM_CMD_BLOCK_INTERACTIVE) = 0;
            /* +0x14 is the high half of that dword and is covered by the store above.
               +0x02 +0x04 +0x06 +0x16 +0x1A +0x20 +0x22 likewise: XP writes them,
               but we cannot yet say WHAT, and a named wrong value is worse than an
               unchanged right one. Revisit each as its meaning is earned. */
            VDM_REG(tib, VTIB_EAX) &= HIGH_WORD_MASK_U;   /* AX = 0 */
            if (!quiet) {
                cursor = LogPut(cursor, "         sub 01 answered: no command (flags=0, cookie and "
                            "the guest's own fields left alone), blk=0x"); cursor = LogHex(cursor, block);
                cursor = LogPut(cursor, "\r\n");
                LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
            }
            VDM_REG(tib, VTIB_EFLAGS) &= ~EFLAGS_CF_U;  /* success */
            VDM_REG(tib, VTIB_EIP) += VDM_BOP_SUBFUNCTION_LENGTH;
            { *cursorIo = cursor; return HOST_FLOW_CONTINUE; }
        }
        {
            INT flow = NtvdmCommandPrompt(&cursor, base, bopNumber, sub, tib, quiet);
            if (flow == HOST_FLOW_CONTINUE) { *cursorIo = cursor; return HOST_FLOW_CONTINUE; }
        }
        {
            INT flow = NtvdmCommandStartupBatch(&cursor, base, bopNumber, sub, tib, quiet);
            if (flow == HOST_FLOW_CONTINUE) { *cursorIo = cursor; return HOST_FLOW_CONTINUE; }
        }
        /* ── ★ sub 10 = A ONE-BIT QUERY, AND IT GATES THE PROMPT. ───────────────
             The answer is AL, a yes/no flag and nothing more.
           ★ It sits ON the interactive path. COMMAND.COM issues it just before its
             prompt, and a non-zero AL takes it AWAY from the prompt: it
             means "do not read the keyboard". We were not setting AL at all, leaving
             whatever the guest happened to have there -- which is how an
             unimplemented call still ANSWERS, at random.
           ▸ AL = 0. Whatever the flag tracks, it is not set in a plain VDM that
             has been asked to run a shell, and 0 is the value that lets the shell
             be a shell. ⚠ Recorded as a reading of ONE flag we have not named,
             not as a decode of what it means. */
        if (bopNumber == NTVDM_BOP_CMD && sub == NTVDM_CMD_QUERY_BIT) {
            VDM_REG(tib, VTIB_EAX) &= ~BYTE_MASK_U;   /* AL = 0 */
            if (!quiet) {
                cursor = LogPut(cursor, "         sub 10 answered: AL=0\r\n");
                LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
            }
            VDM_REG(tib, VTIB_EFLAGS) &= ~EFLAGS_CF_U;
            VDM_REG(tib, VTIB_EIP) += VDM_BOP_SUBFUNCTION_LENGTH;
            { *cursorIo = cursor; return HOST_FLOW_CONTINUE; }
        }
        /* ── ★ sub 0E = THE KEYBOARD / CODE-PAGE CONFIGURATION. ─────────────────
             On stock this describes the keyboard / code-page setup for a KEYB
             command line, written into the guest's buffer.
           ⇒ DS:SI is a buffer of CX bytes; DX is the ANSWER -- non-zero = "there is
             a keyboard driver to set up" (the shell goes on to set one up), zero =
             skip it (observed).
           ⛔ I HAD THIS WRONG ONCE. Reading only the guest side, DX looked like a
             leftover from the `INT 2Fh AX=AD80h` KEYB check just before it, and I
             wrote that the BOP "probably does not touch DX". Stock sets DX on every
             answer. **A register set by the callee is not distinguishable from a
             leftover by looking at the caller alone.**
           ▸ We answer 0 = no keyboard driver, which is TRUE of us: we do not load
             KB16.COM or KEYBOARD.SYS. Explicitly, rather than by leaving DX alone
             and getting 0 because that is what happened to be in it. */
        /* ── sub 00 = VDDTerminateVDM: THE PERMANENT SHELL'S EXIT. (s81 sweep) ──────
             XP's COMMAND.COM ends the VDM through here on EXIT when it is the
             permanent shell (started with /P) -- observed.
             It had no arm, so it fell to the generic "skip the BOP" below and EXIT
             did nothing. End the run exactly as a top-level AH=4Ch does. */
        if (bopNumber == NTVDM_BOP_CMD && sub == NTVDM_CMD_TERMINATE) {
            cursor = LogPut(cursor, "         sub 00: the shell asked to END THE VDM (EXIT) -- ending the run\r\n");
            LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
            machine->ExitCode = 0;
            { *cursorIo = cursor; return HOST_FLOW_BREAK; }
        }
        if (bopNumber == NTVDM_BOP_CMD && sub == NTVDM_CMD_KEYBOARD_CONFIG) {
            VDM_REG(tib, VTIB_EDX) &= HIGH_WORD_MASK_U;   /* DX = 0: no KEYB to run */
            if (!quiet) {
                cursor = LogPut(cursor, "         sub 0E answered: no keyboard driver (DX=0)\r\n");
                LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
            }
            VDM_REG(tib, VTIB_EFLAGS) &= ~EFLAGS_CF_U;
            VDM_REG(tib, VTIB_EIP) += VDM_BOP_SUBFUNCTION_LENGTH;
            { *cursorIo = cursor; return HOST_FLOW_CONTINUE; }
        }
        if (carryPolicy) VDM_REG(tib, VTIB_EFLAGS) |=  EFLAGS_CF_U;
        else        VDM_REG(tib, VTIB_EFLAGS) &= ~EFLAGS_CF_U;
        VDM_REG(tib, VTIB_EIP) += VDM_BOP_SUBFUNCTION_LENGTH;             /* C4 C4 <bop> <sub> -- see above */
        { *cursorIo = cursor; return HOST_FLOW_CONTINUE; }
    *cursorIo = cursor; return HOST_FLOW_NEXT;
}


/* INT 20h, 22h (BOP 30h) and INT 27h end the program: terminate it -- or, with a parent waiting, return to the parent. */
static INT DosServiceTerminateBop(PSTR *cursorIo, PSTR const base, volatile BYTE * const tib, DOS_MACHINE *machine)
{
    PSTR cursor = *cursorIo;
    {   /* ---- INT 20h / 22h (BOP 30h) and INT 27h: they END the program, so they stay
           here -- only the exec loop can terminate a run or return to a parent.
           The rest of the BIOS block moved to V86BiosBop() (GH #247). */
        UINT bopNumber = VDM_REG(tib, VTIB_EVENT_INFO) & BYTE_MASK;
        INT handled = 1;
        if (bopNumber == DOS_BOP_INT20) {               /* INT 20h: terminate       */
            /* INT 20h is AH=4Ch with an exit code of 0. Routing it here
               rather than leaving an IRET means a program that exits this
               way actually exits, instead of returning into itself. */
            VDM_REG(tib, VTIB_EAX) &= HIGH_WORD_MASK_U;
            machine->TraceCursor = cursor; DosInt21(machine); cursor = machine->TraceCursor;   /* AH=00 -> terminate       */
            /* ★ AND IF THIS WAS A CHILD, GO BACK TO ITS PARENT. (GH #134)
                 This used to set handled = 2, which `break`s the exec loop
                 and ends the whole VDM -- so a .COM child exiting the normal
                 .COM way took the host down with it. */
            handled = DosTerminate(machine, tib, &cursor, base) ? EXEC_HANDLED_CHILD_EXITED : EXEC_HANDLED_RUN_OVER;
        } else if (bopNumber == DOS_BOP_FOR_VECTOR(VECTOR_TERMINATE_RESIDENT)) {               /* TSR, CP/M style          */
            /* ── THE OLD FORM OF AH=31h, AND IT KEEPS MEMORY TOO. (GH #49) ─
                 DX is a BYTE OFFSET past the PSP here, not a paragraph
                 count -- that is the one thing this call does differently
                 and the easy thing to get wrong. Round UP to paragraphs so
                 the last partial one is kept rather than cut off.
                 CS is the resident program's PSP for an INT 27h caller. */
            DWORD int27Dx = VDM_REG16(tib, VTIB_EDX);
            machine->TsrKeep = (WORD)((int27Dx + PARAGRAPH_LAST_BYTE) >> PARAGRAPH_SHIFT);
            machine->IsTsrPending = 1;
            cursor = LogPut(cursor, "  INT27 TSR: keep 0x"); cursor = LogHex(cursor, machine->TsrKeep);
            cursor = LogPut(cursor, " paras (DX=0x"); cursor = LogHex(cursor, int27Dx);
            cursor = LogPut(cursor, " bytes), vectors LEFT INSTALLED\r\n");
            VDM_REG(tib, VTIB_EAX) &= HIGH_WORD_MASK_U;
            machine->TraceCursor = cursor; DosInt21(machine); cursor = machine->TraceCursor;
            handled = DosTerminate(machine, tib, &cursor, base) ? EXEC_HANDLED_CHILD_EXITED : EXEC_HANDLED_RUN_OVER;
        } else handled = 0;
        if (handled == EXEC_HANDLED_RUN_OVER) { *cursorIo = cursor; return HOST_FLOW_BREAK; }               /* terminate: the run is over  */
        if (handled == EXEC_HANDLED_CHILD_EXITED) { *cursorIo = cursor; return HOST_FLOW_CONTINUE; }            /* a child exited: parent is back */
        if (handled) { VDM_REG(tib, VTIB_EIP) += VDM_BOP_LENGTH; { *cursorIo = cursor; return HOST_FLOW_CONTINUE; } }
    }
    *cursorIo = cursor; return HOST_FLOW_NEXT;
}


/* The BIOS INT 08h timer tick, serviced in the host. */
static INT V86ServiceTimerBop(volatile BYTE * const tib)
{
    if ((VDM_REG(tib, VTIB_EVENT_INFO) & BYTE_MASK) == VECTOR_TIMER) {   /* INT 08h timer tick */
        NTVDD_REGISTERS registers; RegistersLoad(&registers, tib);
        HOST_LOCK();
        VddBusDeliverInterrupt(&g_Bus, VECTOR_TIMER, &registers);  /* bump BIOS tick at 0040:006C */
        /* The real BIOS timer ISR ends with `mov al,20h; out 20h,al`. Ours is a BOP
           with nowhere to put one, so issue the EOI here -- without it the PIC's
           in-service bit for IRQ0 latches on the first tick and the timer stops dead
           (measured: exactly one tick delivered in a 30 s run). */
        VddPicEndOfInterrupt(&g_Pic, PIC_IRQ_TIMER);
        HOST_UNLOCK();
        RegistersStore(&registers, tib);
        VDM_REG(tib, VTIB_EIP) += VDM_BOP_LENGTH;            /* -> CD 1C (chain user timer) */
        { return HOST_FLOW_CONTINUE; }
    }
    return HOST_FLOW_NEXT;
}


/* The BIOS INT 09h keyboard handler, serviced in the host. */
static INT V86ServiceKeyboardBop(volatile BYTE * const tib)
{
    if ((VDM_REG(tib, VTIB_EVENT_INFO) & BYTE_MASK) == VECTOR_KEYBOARD) {   /* INT 09h: BIOS keyboard */
        INT keyAction;
        /* ── #244: THE SECOND HALF, AFTER INT 15h AH=4Fh SAID "PROCESS IT" (CF=1).
             We are at bios_kbdact.asm k4f's BOP: AL is the scancode as the hook left
             it (possibly changed), the interrupted code's AX is on the stack above
             the INT 09h frame. Translate AL, EOI, pop that AX ourselves, and resume
             where the byte's action says -- a side-call, or brk's bare IRET. */
        if (VDM_REG16(tib, VTIB_CS) == DOS_CTAB_SEG
            && VDM_REG16(tib, VTIB_EIP) == DOS_KBDACT_OFF + BIOS_KEYBOARD_ACTION_INTERCEPT_BOP) {
            DWORD ss = VDM_REG16(tib, VTIB_SS), stackPointer = VDM_REG16(tib, VTIB_ESP);
            INT keyboardAction;
            HOST_LOCK();
            keyAction = VddInputBiosTranslate(&g_Input, (BYTE)VDM_REG(tib, VTIB_EAX));
            VddPicEndOfInterrupt(&g_Pic, PIC_IRQ_KEYBOARD);
            if (keyAction == INPUT_ACTION_PAUSE && KeyboardActionEntry(keyAction) < 0) VddInputPauseCancel(&g_Input);
            HOST_UNLOCK();
            ++g_Kb4FTranslate;
            VDM_SET16(tib, VTIB_EAX, PeekWord((ss << PARAGRAPH_SHIFT) + stackPointer));          /* pop ax */
            VDM_REG(tib, VTIB_ESP) = (VDM_REG(tib, VTIB_ESP) & HIGH_WORD_MASK_U) | ((stackPointer + X86_WORD_SIZE) & WORD_MASK);
            keyboardAction = KeyboardActionEntry(keyAction);
            VDM_REG(tib, VTIB_EIP) = (DWORD)(DOS_KBDACT_OFF + (keyboardAction >= 0 ? keyboardAction : BIOS_KEYBOARD_ACTION_IRET));
            { return HOST_FLOW_CONTINUE; }
        }
        /* ── #244: THE FIRST HALF, WHEN SOMETHING HAS HOOKED INT 15h. Take the byte
             out of the controller now (it is the BIOS's `in al,60h`), push the
             interrupted code's AX, load AX = 4F00h | byte, and run k4f: `stc / int
             15h` in the guest, then back to the arm above -- or, on CF=0, k4f's own
             EOI and IRET (swallowed). The EOI waits until then, as the BIOS's does.
             Not hooked (the normal case, every game included): straight on below,
             exactly as before -- not one extra instruction on the default path. */
        if (Int15Hooked()) {
            INT scanCode;
            HOST_LOCK();
            scanCode = VddInputBiosFetch(&g_Input);
            HOST_UNLOCK();
            if (scanCode >= 0) {
                DWORD ss = VDM_REG16(tib, VTIB_SS);
                WORD  stackPointer = (WORD)((VDM_REG(tib, VTIB_ESP) - X86_WORD_SIZE) & WORD_MASK);
                PokeWord((ss << PARAGRAPH_SHIFT) + stackPointer, (WORD)VDM_REG(tib, VTIB_EAX));     /* push ax */
                VDM_REG(tib, VTIB_ESP) = (VDM_REG(tib, VTIB_ESP) & HIGH_WORD_MASK_U) | stackPointer;
                VDM_SET16(tib, VTIB_EAX, (WORD)((BIOS_SYSTEM_KEYBOARD_INTERCEPT << BYTE_SHIFT) | (UINT)scanCode));
                VDM_SET16(tib, VTIB_CS, DOS_CTAB_SEG);
                VDM_REG(tib, VTIB_EIP) = (DWORD)(DOS_KBDACT_OFF + BIOS_KEYBOARD_ACTION_INTERCEPT);
                ++g_Kb4FCalls;
                { return HOST_FLOW_CONTINUE; }
            }
            /* nothing presented: a spurious IRQ1 -- EOI and IRET, as below */
            HOST_LOCK();
            VddPicEndOfInterrupt(&g_Pic, PIC_IRQ_KEYBOARD);
            HOST_UNLOCK();
            VDM_REG(tib, VTIB_EIP) += VDM_BOP_LENGTH;
            { return HOST_FLOW_CONTINUE; }
        }
        HOST_LOCK();
        keyAction = VddInputBiosConsume(&g_Input);   /* take the byte, re-arm if more queued */
        /* ── ★ THE BIOS INT 09h ENDS WITH AN EOI, AND SO MUST THIS. ──────────────────
             A guest that hooks INT 09h keeps IRQ1 in service until it EOIs (strict
             acknowledge above). QB.EXE's hook EOIs only the keys it swallows; for
             every ordinary key it CHAINS to the BIOS handler (`int 0EFh`) and leaves
             the EOI to it -- as the real one does with `mov al,20h / out 20h,al`.
             This arm never sent one, so IRQ1's in-service bit stayed set after the
             first key and no keyboard interrupt was ever delivered again: measured
             keyirq=1 for a whole QBasic run of ~19 key presses. Same shape as the
             INT 08h arm's EOI below, for the same reason. Harmless when the guest
             EOI'd before chaining: the bit is already clear. */
        VddPicEndOfInterrupt(&g_Pic, PIC_IRQ_KEYBOARD);
        HOST_UNLOCK();
        /* ── #254: AND WHAT THE BIOS CALLS FROM IT. Ctrl-Break -> INT 1Bh, Print
             Screen -> INT 05h, SysReq -> INT 15h AH=85h, Pause -> the spin loop:
             resume the guest in bios_kbdact.asm's routine (after the EOI, as the
             BIOS does), which IRETs to the interrupted code. Only guest-side
             calls -- nothing here reaches the host. */
        {   INT keyboardAction = KeyboardActionEntry(keyAction);
            if (keyboardAction >= 0) {
                VDM_SET16(tib, VTIB_CS, DOS_CTAB_SEG);
                VDM_REG(tib, VTIB_EIP) = (DWORD)(DOS_KBDACT_OFF + keyboardAction);
                { return HOST_FLOW_CONTINUE; }
            }
            if (keyAction == INPUT_ACTION_PAUSE) { HOST_LOCK(); VddInputPauseCancel(&g_Input); HOST_UNLOCK(); }
        }
        VDM_REG(tib, VTIB_EIP) += VDM_BOP_LENGTH;        /* -> the IRET */
        { return HOST_FLOW_CONTINUE; }
    }
    return HOST_FLOW_NEXT;
}


/* The guest stopped for something that is neither I/O nor a BOP: log where, with the bytes before and at CS:IP. */
static INT V86ReportUnexpectedStop(PSTR *cursorIo, PSTR const base, const DWORD event, volatile BYTE * const tib, const LONG vdmStatus)
{
    PSTR cursor = *cursorIo;
    if (event != VDM_EVENT_BOP) {
        DWORD currentCs = VDM_REG16(tib, VTIB_CS);
        DWORD currentIp = VDM_REG16(tib, VTIB_EIP);
        volatile BYTE *codeBytes = (volatile BYTE *)((currentCs << PARAGRAPH_SHIFT) + currentIp);
        BYTE instructionBytes[8], pageBytes[8]; UINT item;
        for (item = 0; item < 8; ++item) instructionBytes[item] = codeBytes[item];
        for (item = 0; item < 8; ++item) pageBytes[item] = (currentIp >= 8) ? codeBytes[(INT)item - 8] : 0;  /* 8 bytes BEFORE CS:IP */
        cursor = LogPut(cursor, "STAGE2: stop event=0x"); cursor = LogHex(cursor, event);
        cursor = LogPut(cursor, " status=0x"); cursor = LogHex(cursor, (UINT)vdmStatus);
        cursor = LogPut(cursor, " info=0x"); cursor = LogHex(cursor, VDM_REG(tib, VTIB_EVENT_INFO));
        cursor = LogPut(cursor, " CS:IP=0x"); cursor = LogHex(cursor, currentCs);
        cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, currentIp); cursor = LogPut(cursor, "\r\n");
        cursor = LogPut(cursor, "  bytes[CS:IP-8]: "); cursor = LogDump(cursor, pageBytes, 8);
        cursor = LogPut(cursor, "  bytes@CS:IP: "); cursor = LogDump(cursor, instructionBytes, 8);
        cursor = LogPut(cursor, "  VTIB[5A8..]: "); cursor = LogDump(cursor, (const VOID *)(tib + 0x5A8), 0x20);
        LogAppend(LOG_PATH, base, cursor); cursor = base;
        { *cursorIo = cursor; return HOST_FLOW_BREAK; }
    }
    *cursorIo = cursor; return HOST_FLOW_NEXT;
}


/* A port access (or REP INS/OUTS, or a #GP on one) the kernel reflected: emulate it on the device bus and resume the guest after the instruction. */
static INT V86ServiceIoEvent(PSTR *cursorIo, PSTR const base, const DWORD event, volatile BYTE * const tib)
{
    PSTR cursor = *cursorIo;
    static UINT32 lastFault = 0; static INT stormCount = 0;   /* the I/O-fault storm detector's memory, across events */
    /* I/O port trap (event 0; VM-confirmed) or a generic GP fault (event 2):
       if the faulting instruction is an IN/OUT we can decode, service it via
       the VDD bus and resume; otherwise fall through to the stop dump. */
    if (event == VDM_EVENT_IO || event == VDM_EVENT_IO_HW ||
        event == VDM_EVENT_GPFAULT || event == VDM_EVENT_IO_STRING) {
        INT handled;
        { static INT ioBudget = 6; VdmStateSample("io-reflect", tib, &ioBudget); }
        /* REP INS/OUTS arrives as its own event with CS:IP ON the instruction. */
        if (event == VDM_EVENT_IO_STRING) {
            HOST_LOCK();
            handled = HostTryIoString(tib, &g_Bus);
            HOST_UNLOCK();
            if (handled) { g_EventIoString++; { *cursorIo = cursor; return HOST_FLOW_CONTINUE; } }
        }
        /* Trap-storm detection over ALL faults (port + A0000 memory): the
           per-pixel VGA loop faults repeatedly in a tight PC window. Once a
           storm is established in mode 12h, escalate to the batching
           interpreter so the whole inner loop (OUTs + pixel writes) runs in
           one shot; otherwise emulate the single faulting access. */
        DWORD currentCs2 = VDM_REG16(tib, VTIB_CS), currentIp2 = VDM_REG16(tib, VTIB_EIP);
        UINT32 current = (currentCs2 << PARAGRAPH_SHIFT) + currentIp2;
        UINT32 distance = (current > lastFault) ? (current - lastFault) : (lastFault - current);
        stormCount = (distance <= STORM_WINDOW) ? (stormCount + 1) : 0;
        lastFault = current;
        if ((g_A000Protection || (g_Interp12 && VddVideoIsPlanarActive(&g_Video)))
            && stormCount >= STORM_GATE) {
            DWORD breakCs = VDM_REG16(tib, VTIB_CS), breakIp = VDM_REG16(tib, VTIB_EIP);
            INT32 ran = HostInterpPaced(tib, TIER1_CAP);
            if (ran > 0) {
                static INT batchBudget = 10;
                if (batchBudget > 0) {            /* is the batch ADVANCING the guest? */
                    --batchBudget;
                    cursor = LogPut(cursor, "BATCH ran="); cursor = LogHex(cursor, (DWORD)ran);
                    cursor = LogPut(cursor, " from 0x"); cursor = LogHex(cursor, breakCs); cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, breakIp);
                    cursor = LogPut(cursor, " to 0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_CS));
                    cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_EIP));
                    cursor = LogPut(cursor, "\r\n");
                    LogAppend(LOG_PATH, base, cursor); cursor = base;
                }
                { *cursorIo = cursor; return HOST_FLOW_CONTINUE; }                       /* batched the hot loop            */
            }
        }
        HOST_LOCK();
        handled = HostTryIo(tib, &g_Bus);     /* single port op (no logging)     */
        HOST_UNLOCK();
        RetraceIdle();                              /* #183: outside the lock */
        if (handled) { g_EventIo++; g_IoViaDirect++; IoHotNote(g_IoLastPort, VDM_REG16(tib, VTIB_CS), VDM_REG16(tib, VTIB_EIP)); { *cursorIo = cursor; return HOST_FLOW_CONTINUE; } }
        /* real-HW event 3 reports CS:IP AFTER the faulting IN/OUT -> retro-decode the
           I/O instruction ending at CS:IP and service it (Skyroads' vblank IN AL,DX). */
        if (event == VDM_EVENT_IO_HW) {
            HOST_LOCK();
            handled = HostTryIoRetro(tib, &g_Bus);
            HOST_UNLOCK();
            RetraceIdle();                          /* #183: outside the lock */
            if (handled) { g_EventIo++; g_IoViaRetro++; IoHotNote(g_IoLastPort, VDM_REG16(tib, VTIB_CS), VDM_REG16(tib, VTIB_EIP)); { *cursorIo = cursor; return HOST_FLOW_CONTINUE; } }
        }
        if ((g_A000Protection || (g_Interp12 && VddVideoIsPlanarActive(&g_Video)))
            && HostInterpPaced(tib, 1) > 0) { *cursorIo = cursor; return HOST_FLOW_CONTINUE; }   /* single A0000 access */
        /* The interpreter refused the very first opcode. With A0000 trapped that
           is a LIVELOCK, not a miss: we resume at the same EIP, the guest
           re-faults on the same store, forever. Name the opcode -- this is the
           "mode-12h MOV-store decoder gap" from the M3 notes, and it is why
           mode 12h has never rendered. Budgeted so it cannot flood the log. */
        if (g_A000Protection || g_Interp12) {
            static INT declineBudget = 8;
            if (declineBudget > 0) {
                DWORD codeSegment2 = VDM_REG16(tib, VTIB_CS), eip2 = VDM_REG16(tib, VTIB_EIP);
                const volatile BYTE *ip2 = (const volatile BYTE *)((codeSegment2 << PARAGRAPH_SHIFT) + eip2);
                UINT index4;
                --declineBudget;
                cursor = LogPut(cursor, "INTERP-REFUSED at 0x"); cursor = LogHex(cursor, codeSegment2);
                cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, eip2); cursor = LogPut(cursor, " bytes:");
                for (index4 = 0; index4 < 8; ++index4) { cursor = LogPut(cursor, " "); cursor = LogHexByte(cursor, ip2[index4]); }
                cursor = LogPut(cursor, "\r\n");
                LogAppend(LOG_PATH, base, cursor); cursor = base;
            }
            g_InterpRefused++;
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
        if (event == VDM_EVENT_IO_HW) {
            volatile DWORD *vdmStateWord = (volatile DWORD *)(ULONG_PTR)FIXED_NTVDMSTATE_LINEAR;
            DWORD pend = *vdmStateWord & VDM_INT_PENDING;
            if (pend) {
                if (pend & VDM_INT_TIMER) Irq0Latch();         /* VDM_INT_TIMER -> IRQ0 */
                *vdmStateWord &= ~VDM_INT_PENDING;
                g_EventIntPending++;
                { *cursorIo = cursor; return HOST_FLOW_CONTINUE; }
            }
        }
    }
    *cursorIo = cursor; return HOST_FLOW_NEXT;
}


/* Run the guest in V86 until its next event, and time the stretch (#238). */
static VOID V86RunGuestTimed(volatile BYTE * const tib, DWORD *eventIo, LONG *vdmStatusIo)
{
    DWORD event = *eventIo;
    LONG vdmStatus = *vdmStatusIo;
    {   LARGE_INTEGER runStart, runEnd; DWORD runMilliseconds;
        DWORD eventCs = VDM_REG16(tib, VTIB_CS), eip = VDM_REG16(tib, VTIB_EIP);
        QueryPerformanceCounter(&runStart);
        if (g_HostTimeLast)                                   /* #238: see g_HostMicrosecondsEvent */
            g_HostMicrosecondsEvent[g_HostEventLast] += QpcMicroseconds(runStart.QuadPart - g_HostTimeLast);
        event = VdmRunGuest(tib, &vdmStatus);
        QueryPerformanceCounter(&runEnd);
        g_V86MicrosecondsTotal += QpcMicroseconds(runEnd.QuadPart - runStart.QuadPart);
        g_HostTimeLast = runEnd.QuadPart;
        g_HostEventLast = event < EV_HIST_MAX ? event : EV_HIST_MAX - 1;
        {   DWORD now = GetTickCount();                     /* #238: g_XsSnapshot */
            if (!g_XsStart) g_XsStart = now;
            while (g_XsSeconds < XS_SECS && now - g_XsStart >= g_XsSeconds * MILLISECONDS_PER_SECOND_U) {
                DWORD *snapshot = g_XsSnapshot[g_XsSeconds++], hostUs = 0; INT eventIndex;
                for (eventIndex = 0; eventIndex < EV_HIST_MAX; ++eventIndex) hostUs += g_HostMicrosecondsEvent[eventIndex] / MICROSECONDS_PER_MILLISECOND_U;
                snapshot[XS_RAISE] = g_IrqRaised[0]; snapshot[XS_ASYNC] = g_AsyncInjected;
                snapshot[XS_COOP] = g_Irq0Injected;       snapshot[XS_NIE] = g_AsyncWhyHistogram[0][ASYNC_WHY_NOT_IN_EXEC];
                snapshot[XS_BOP] = g_EventHistogram[VDM_EVENT_BOP];      snapshot[XS_IO] = g_EventHistogram[VDM_EVENT_IO];
                snapshot[XS_HOSTMS] = hostUs;     snapshot[XS_PACE] = g_PitPaceCalls;
            } }
        /* s82: the entry trampoline borrowed DPMI callback slot 0/1's bytes (#248: the
           slots have since moved to 0x90; the bytes are spare now). The first
           exit that is not inside it hands them back -- long before any client can
           have allocated, let alone called, a callback. */
        if (g_TrampolineSaved) {
            DWORD trampolineCs = VDM_REG16(tib, VTIB_CS), trampolineIp = VDM_REG16(tib, VTIB_EIP);
            if (!(trampolineCs == DOS_HDLR_SEG && trampolineIp >= DOS_HDLR_TRAMPOLINE_OFF && trampolineIp < DOS_HDLR_TRAMPOLINE_OFF + DOS_HDLR_TRAMPOLINE_SIZE)) {
                volatile BYTE *trampoline = (volatile BYTE *)(ULONG_PTR)(((DWORD)DOS_HDLR_SEG << PARAGRAPH_SHIFT) + DOS_HDLR_TRAMPOLINE_OFF);
                INT item;
                for (item = 0; item < DOS_HDLR_TRAMPOLINE_SIZE; ++item) trampoline[item] = g_TrampolineSave[item];
                g_TrampolineSaved = 0;
                {   CHAR trampolineLine[200], *trampolineCursor = LogPut(trampolineLine, "STAGE2: entry trampoline done -- first exit ev=0x");
                    trampolineCursor = LogHex(trampolineCursor, (DWORD)event); trampolineCursor = LogPut(trampolineCursor, " at 0x"); trampolineCursor = LogHex(trampolineCursor, trampolineCs);
                    trampolineCursor = LogPut(trampolineCursor, ":0x"); trampolineCursor = LogHex(trampolineCursor, trampolineIp); trampolineCursor = LogPut(trampolineCursor, " VTIB EFLAGS=0x");
                    trampolineCursor = LogHex(trampolineCursor, VDM_REG(tib, VTIB_EFLAGS)); trampolineCursor = LogPut(trampolineCursor, " (IF=bit 9, VIF=bit 19, VIP=bit 20)\r\n");
                    LogAppend(LOG_PATH, trampolineLine, trampolineCursor); }
            }
        }
        /* ── ★ HOW LONG DID ONE V86 RUN LAST WITHOUT GIVING US A TURN? (Skyroads
             wobble, s61.) The big timer gaps are low-I/O, so the guest is not
             hammering ports -- it is inside ONE long VdmRunGuest stretch (a spin, a
             cli section, or slow interpreted VGA). This is the PM stretch
             instrument for the V86 path: bucket the durations and keep the worst,
             with the ENTRY cs:ip (where the stretch began) and the exit event. A
             stretch >8 ms at native speed is 30M+ instructions -- so it is a WAIT
             or a slow op, and its entry names the routine. */
        {   UINT bucket = 0;
            runMilliseconds = QpcMicroseconds(runEnd.QuadPart - runStart.QuadPart) / MICROSECONDS_PER_MILLISECOND_U;   /* ms */
            while (bucket < 7 && runMilliseconds >= (1u << bucket)) ++bucket;
            g_V86StringHistogram[bucket]++;
            if (runMilliseconds >= 8u) ++g_V86StringCount8;
            if (runMilliseconds > g_V86StringMaximumMs) {
                g_V86StringMaximumMs = runMilliseconds; g_V86StringMaximumCs = eventCs;
                g_V86StringMaximumIp = eip; g_V86StringMaximumEvent = event;
            } } }
    *eventIo = event; *vdmStatusIo = vdmStatus;
}


/* While Mode Y needs it, run a slice of the guest in the interpreter rather than in V86. */
static INT V86RunModeYSlice(volatile BYTE * const tib)
{
    if (!g_P12Interp && ModeYNeedsInterp()) {
        INT32 ran;
        UINT64 cyclesStart = ModeYTimelineRdtsc();
        g_ModeYInterp = 1;
        ran = HostInterpPaced(tib, P12_SLICE);
        g_ModeYInterp = 0;
        if (g_ModeYTimelineT0) { DWORD second = (GetTickCount() - g_ModeYTimelineT0) / MILLISECONDS_PER_SECOND_U;
                        if (second < YTL_SECS) { if (ran > 0) g_ModeYTimelineIns[second] += (DWORD)ran;
                                              g_ModeYTimelineInterpreterCycles[second] += ModeYTimelineRdtsc() - cyclesStart; } }
        if (ran > 0) { g_ModeYSlices++; g_ModeYInstructions += (DWORD)ran; { return HOST_FLOW_CONTINUE; } }
        if (VDM_REG16(tib, VTIB_CS) == 0) ModeYRingDump("guest at CS=0 after a slice");
        /* Declined (a BOP, or an opcode it does not model): that ONE instruction
           runs natively. Under a multi-plane mask a native A0000 store reaches only
           the first selected plane -- so count these separately; they are the
           remaining way this window can be wrong. */
        { DWORD codeSegment3 = VDM_REG16(tib, VTIB_CS), eip3 = VDM_REG16(tib, VTIB_EIP);
          const volatile BYTE *ip3 = (const volatile BYTE *)((codeSegment3 << PARAGRAPH_SHIFT) + eip3);
          if (!(ip3[0] == VDM_BOP0 && ip3[1] == VDM_BOP1)) {
              BYTE mapMask = (BYTE)(g_Video.MapMask & VIDEO_ALL_PLANES);
              ++g_ModeYBails;
              if (mapMask & (BYTE)(mapMask - 1)) ++g_ModeYBailMp;
              ModeYBailNote(codeSegment3, eip3, ip3);
          } }
    }
    return HOST_FLOW_NEXT;
}


/* Mode 12h (GH #55): while a planar mode is current, the host is the CPU -- run a slice in the interpreter, whose A0000 accesses go through the planar write engine. */
static INT V86RunPlanarSlice(volatile BYTE * const tib)
{
    /* ---- MODE 12h: THE HOST IS THE CPU (GH #55) ------------------------- *
     * While a planar mode is current we do not hand the guest to V86 at all,
     * because on real hardware there is no way to see its A0000 writes there:
     * the page trap that would show them freezes the VDM (see VideoTrapSync).
     * Run a slice in the interpreter instead -- its A0000 accesses go through
     * the planar write engine -- then loop, which re-runs the IRQ delivery gate
     * above so timer and keyboard interrupts reach the guest between slices.
     * A slice ends early the moment an IRQ is pending, so the slice size is a
     * ceiling on lock-hold time, not on responsiveness.
     * If the interpreter declines the instruction we are ON (a BOP, or an
     * opcode it does not model), ran == 0 and we fall through to V86 exactly as
     * before -- so a DOS call still reaches the kernel as a BOP event, and an
     * unmodeled opcode still executes on the real CPU. */
    if (g_P12Interp && !g_DpmiPm) {
        INT32 ran = HostInterpPaced(tib, P12_SLICE);
        if (ran > 0) { g_P12Batches++; g_P12Instructions += (DWORD)ran; { return HOST_FLOW_CONTINUE; } }
        /* NAME THE OPCODE. Every bail is guest execution we cannot see, so the
           list of declined opcodes IS the to-do list for this path (#27).
           ⚠ NOT ON A BOP: our own `C4 C4 nn` stubs are the overwhelming majority
           of bails and are not unmodelled opcodes. Per-site table, reported at
           exit -- see g_P12Site. */
        { DWORD codeSegment3 = VDM_REG16(tib, VTIB_CS), eip3 = VDM_REG16(tib, VTIB_EIP);
          const volatile BYTE *ip3 = (const volatile BYTE *)((codeSegment3 << PARAGRAPH_SHIFT) + eip3);
          if (!(ip3[0] == VDM_BOP0 && ip3[1] == VDM_BOP1)) {
            UINT index5;
            for (index5 = 0; index5 < g_P12SiteCount; ++index5)
                if (g_P12Site[index5].Cs == codeSegment3 && g_P12Site[index5].Ip == eip3) break;
            if (index5 < g_P12SiteCount) g_P12Site[index5].Count++;
            else if (g_P12SiteCount < P12_SITE_MAX) {
                UINT index6;
                g_P12Site[index5].Cs = codeSegment3; g_P12Site[index5].Ip = eip3; g_P12Site[index5].Count = 1;
                for (index6 = 0; index6 < 8; ++index6) g_P12Site[index5].Bytes[index6] = ip3[index6];
                ++g_P12SiteCount;
            } else ++g_P12SiteLost;
          } }
        g_P12Bails++;
    }
    return HOST_FLOW_NEXT;
}


/* Deliver a pending IRQ1 (the keyboard) to the guest when it can take an interrupt. */
static VOID V86DeliverKeyboardIrq(volatile BYTE * const tib)
{
    /* Deliver a pending keyboard IRQ1 as INT 09h, same IF-gating as IRQ0. The
       scancode is already queued for port 0x60; INT 09h vectors through IVT[9]
       to the game's own handler (or our IRET stub if it hasn't hooked one).
       Excludes re-entry into our INT 08h (0x34) and INT 09h (0x4C) stubs. */
    if (g_Irq1Pending > 0) {
        DWORD cs = VDM_REG16(tib, VTIB_CS), ip = VDM_REG16(tib, VTIB_EIP);
        DWORD flags2;
        if (IsOurStubCsIp(cs, ip)) {   /* #206: BIOS stubs too */
            DWORD ss = VDM_REG16(tib, VTIB_SS), stackPointer = VDM_REG16(tib, VTIB_ESP);
            flags2 = PeekWord((ss << PARAGRAPH_SHIFT) + ((stackPointer + X86_FRAME16_FLAGS) & WORD_MASK));
        } else {
            flags2 = VDM_REG(tib, VTIB_EFLAGS);
            IfvNote(IFV_PATH_VTIB_IRQ01, flags2);
        }
        /* ── WHY IS THIS REFUSED? MEASURED, NOT ASSUMED. ─────────────────────────
             KEYLAT says 90% of keystrokes take >64 ms to reach INT 09h (max 9.6 s)
             while the UI thread hands them over in 0 ms, so the refusal is here and
             it is not rare -- it is the normal case. There are exactly three ways
             out of this gate, and guessing which one has already cost four rounds. */
        ++g_Irq1Checks;
        if (!IfOrVif(flags2))                                          ++g_Irq1NoIf;
        else if (cs == DOS_HDLR_SEG && ip >= DOS_HDLR_INT08_STUB_OFF && ip < DOS_HDLR_INT08_STUB_END)      ++g_Irq1In08;
        else if (cs == DOS_HDLR_SEG && ip >= DOS_HDLR_INT09_STUB_OFF && ip < DOS_HDLR_INT09_STUB_END)      ++g_Irq1In09;
        if (IfOrVif(flags2) && !(cs == DOS_HDLR_SEG &&
                              ((ip >= DOS_HDLR_INT08_STUB_OFF && ip < DOS_HDLR_INT08_STUB_END) || (ip >= DOS_HDLR_INT09_STUB_OFF && ip < DOS_HDLR_INT09_STUB_END)))) {
            InterlockedDecrement(&g_Irq1Pending);   /* one INT 09h per queued scancode byte */
            VddPicAcknowledge(&g_Pic, 1);
            if (AsyncVectorIsOurStub(PIC_IRQ_KEYBOARD)) VddPicEndOfInterrupt(&g_Pic, PIC_IRQ_KEYBOARD);
            g_Irq1Injected++;
            InjectInt(tib, VECTOR_KEYBOARD);
            KeyLatencyPop();               /* the guest is now IN its INT 09h */
        }
    }
}


/* Deliver a pending IRQ0 (the timer) to the guest when it can take an interrupt. */
static VOID V86DeliverTimerIrq(volatile BYTE * const tib)
{
    /* Deliver a pending PIT IRQ0 as INT 08h when the guest's main-line
       interrupts are enabled. We regain control at event boundaries, almost
       always inside a BOP stub (CS == DOS_HDLR_SEG) where the LIVE IF is the
       handler's (cleared by the CD nn that vectored in) -- the guest's real
       IF is the FLAGS the stub will IRET to, at SS:SP+4. Outside a stub
       (e.g. an I/O fault from main-line) the live EFLAGS IF applies. Skip if
       we're inside our own INT 08h stub, to avoid timer re-entrancy. */
    if (g_Irq0Pending) {
        DWORD cs = VDM_REG16(tib, VTIB_CS), ip = VDM_REG16(tib, VTIB_EIP);
        DWORD flags2;
        if (IsOurStubCsIp(cs, ip)) {   /* #206: BIOS stubs too */
            DWORD ss = VDM_REG16(tib, VTIB_SS), stackPointer = VDM_REG16(tib, VTIB_ESP);
            flags2 = PeekWord((ss << PARAGRAPH_SHIFT) + ((stackPointer + X86_FRAME16_FLAGS) & WORD_MASK));   /* main-line FLAGS the stub returns to */
        } else {
            flags2 = VDM_REG(tib, VTIB_EFLAGS);
            IfvNote(IFV_PATH_VTIB_IRQ01, flags2);
        }
        if (IfOrVif(flags2) && Irq0CanDeliver()
            && !(cs == DOS_HDLR_SEG && ip >= DOS_HDLR_INT08_STUB_OFF && ip < DOS_HDLR_INT08_STUB_END)) {
            InterlockedDecrement(&g_Irq0Pending);
            Irq0Ack();                     /* in service until the guest EOIs (s70) */
            g_Irq0Injected++;
            g_Irq0NoteCs = cs; g_Irq0NoteIp = ip;   /* where IF re-opened */
            Irq0DeliveredNote();          /* the guest's clock, as a timeline */
            InjectInt(tib, VECTOR_TIMER);
        } else {
            static INT skipBudget = 6;
            g_Irq0Skip++;                  /* IF=0 or inside our own INT 08h */
            if (cs == DOS_HDLR_SEG && ip >= DOS_HDLR_INT08_STUB_OFF && ip < DOS_HDLR_INT08_STUB_END) g_Irq0SkipStub++;
            else if (!IfOrVif(flags2)) {
                g_Irq0SkipIf++;
                if (IsOurStubCsIp(cs, ip)) {   /* #238: who called our stub with IF off */
                    DWORD ss = VDM_REG16(tib, VTIB_SS), stackPointer = VDM_REG16(tib, VTIB_ESP);
                    SkipIfSiteNote(PeekWord((ss << PARAGRAPH_SHIFT) + ((stackPointer + X86_FRAME16_CS) & WORD_MASK)),
                                     PeekWord((ss << PARAGRAPH_SHIFT) + (stackPointer & WORD_MASK)), ip);
                }
            }
            VdmStateSample("irq0-skip", tib, &skipBudget);
        }
    }
}


/* The exec loop: run the guest until it terminates, a hard stop, or the window closes -- deliver pending IRQs, run it (in V86 or, for planar and Mode Y video, in the interpreter), and service what stopped it: port I/O, the DPMI switch, BIOS and DOS BOPs, the guest's own NTVDM BOPs, INT 21h. */
static VOID HostRunExecLoop(PSTR *cursorIo, PSTR const base, DOS_MACHINE *machine, volatile BYTE * const tib, LONG *vdmStatusIo, CHAR *programPathBuffer)
{
    PSTR cursor = *cursorIo;
    DWORD event;
    LONG vdmStatus = *vdmStatusIo;
    DWORD rmStartTick = GetTickCount();   /* headless wall-clock cap origin (real-mode) */
    g_RunStartTick = rmStartTick;       /* published for the STAGE2 vsync rate line */
    while (g_Running) {
        /* Headless safety (session-9): the real-mode loop has no iteration cap so
           interactive/animated programs run free -- but under the SMB auto-exit harness
           a hung or infinite real-mode program (or a host bug) would run forever and
           wedge rt.bat's `start /wait`, and the box is not easily accessible to unwedge.
           So in headless mode bound it by wall clock: the host self-exits, rt.bat returns,
           the watcher survives. (The PM loop got this in v69; the real-mode loop needs it
           too -- a real-mode hang was the one path that could still permanently wedge the
           rig.) GetTickCount per iteration is cheap; the loop runs once per event/BOP. */
        if (g_Headless && GetTickCount() - rmStartTick > PM_HEADLESS_MS) {
            cursor = LogPut(cursor, "STAGE2: headless time cap (");
            cursor = LogHex(cursor, PM_HEADLESS_MS); cursor = LogPut(cursor, " ms) reached -> exiting"
                     " (long/hung real-mode run; screenshots captured if graphical)\r\n");
            LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
            break;
        }
        /* File > Close Program (#152). Here, not mid-dispatch: the guest is stopped at
           an event boundary, which is also where every injected IRQ is delivered. */
        if (g_CloseRequest && !g_WowLaunch) {
            InterlockedExchange(&g_CloseRequest, 0);
            if (g_ExecDepth > 0 || !g_TopIsShell) {
                if (CloseProgramNow(machine, (VOID *)tib, &cursor, base)) continue;
                break;
            }
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
        HostPitSync();
        OplPumpTime();            /* keep the OPL timers current for the guest */
        HostKeyTypematic();       /* the keyboard repeats even when the UI stalls */
        HostKeyPresent();         /* ...and presents the next byte after its transfer time */
        V86DeliverTimerIrq(tib);
        V86DeliverKeyboardIrq(tib);
        V86DeliverDeviceIrq(tib);   /* see the helper: shared with the nested 0301/0302 loop */
        MouseCallbackTry(tib);          /* INT 33h 0Ch events, under the same gate as an IRQ */
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
        if (g_QiVif) {
            if (VDM_REG(tib, VTIB_EFLAGS) & EFLAGS_IF) VDM_REG(tib, VTIB_EFLAGS) |= EFLAGS_VIF;
            else                                   VDM_REG(tib, VTIB_EFLAGS) &= ~EFLAGS_VIF;
        }
        {
            INT flow = V86RunPlanarSlice(tib);
            if (flow == HOST_FLOW_CONTINUE) continue;
        }
        {
            INT flow = V86RunModeYSlice(tib);
            if (flow == HOST_FLOW_CONTINUE) continue;
        }
        while (g_PauseWant && g_Running) { ++g_PauseCooperative; Sleep(PAUSE_POLL_MS); }   /* #219 */
        CpuSpeedCooperativePark();                                                  /* #225 */
        InterlockedExchange(&g_InExec, 1);
        ExecEnterMark();               /* guest-execution clock starts (throttle) */
        V86RunGuestTimed(tib, &event, &vdmStatus);
        g_EventHistogram[event < EV_HIST_MAX ? event : EV_HIST_MAX - 1]++;
        ExecLeaveMark();               /* ...and stops. Our servicing is not its  */
        InterlockedExchange(&g_InExec, 0);
        /* ── The VM events that follow a mouse-callback injection, verbatim. See
             g_MouseCallbackTrace. Logged before any arm below acts on the event, so what is
             recorded is what the kernel handed back, not what we made of it. */
        if (g_MouseCallbackTrace > 0) {
            CHAR tibLine2[384], *tibCursor2 = tibLine2;
            DWORD trampolineCs = VDM_REG16(tib, VTIB_CS), trampolineIp = VDM_REG16(tib, VTIB_EIP);
            DWORD tibSs = VDM_REG16(tib, VTIB_SS), tibSp = VDM_REG16(tib, VTIB_ESP);
            --g_MouseCallbackTrace;
            tibCursor2 = LogPut(tibCursor2, "MOUSECB-TRACE ev=0x"); tibCursor2 = LogHex(tibCursor2, event);
            tibCursor2 = LogPut(tibCursor2, " info=0x"); tibCursor2 = LogHex(tibCursor2, VDM_REG(tib, VTIB_EVENT_INFO));
            tibCursor2 = LogPut(tibCursor2, " st=0x");   tibCursor2 = LogHex(tibCursor2, (DWORD)vdmStatus);
            tibCursor2 = LogPut(tibCursor2, " cs:ip=0x"); tibCursor2 = LogHex(tibCursor2, trampolineCs); tibCursor2 = LogPut(tibCursor2, ":0x"); tibCursor2 = LogHex(tibCursor2, trampolineIp);
            tibCursor2 = LogPut(tibCursor2, " efl=0x");  tibCursor2 = LogHex(tibCursor2, VDM_REG(tib, VTIB_EFLAGS));
            tibCursor2 = LogPut(tibCursor2, " ss:sp=0x"); tibCursor2 = LogHex(tibCursor2, tibSs); tibCursor2 = LogPut(tibCursor2, ":0x"); tibCursor2 = LogHex(tibCursor2, tibSp);
            tibCursor2 = LogPut(tibCursor2, " ax=0x");   tibCursor2 = LogHex(tibCursor2, VDM_REG16(tib, VTIB_EAX));
            tibCursor2 = LogPut(tibCursor2, " [714]=0x"); tibCursor2 = LogHex(tibCursor2, *(volatile DWORD *)(ULONG_PTR)FIXED_NTVDMSTATE_LINEAR);
            tibCursor2 = LogPut(tibCursor2, " active="); tibCursor2 = LogHex(tibCursor2, (DWORD)g_MouseCallbackActive);
            tibCursor2 = LogPut(tibCursor2, " code=");   tibCursor2 = LogDump(tibCursor2, (const VOID *)(ULONG_PTR)((trampolineCs << PARAGRAPH_SHIFT) + trampolineIp), 8);
            tibCursor2 = LogPut(tibCursor2, " stack=");  tibCursor2 = LogDump(tibCursor2, (const VOID *)(ULONG_PTR)((tibSs << PARAGRAPH_SHIFT) + tibSp), 12);
            tibCursor2 = LogPut(tibCursor2, "\r\n");
            LogAppend(LOG_PATH, tibLine2, tibCursor2);
        }
        /* ── WHEN VdmStartExecution RETURNS A FAILURE STATUS, SAY SO. (session 62) ──
             The V86 path ignored `st` entirely -- only the DPMI branch below ever
             read it -- so a guest that executes an INT3 (or any fault the kernel
             turns into a returned NTSTATUS rather than a serviceable event) died
             with the host process exit code equal to that status and NOTHING in the
             log. That is exactly Mario's flaky ~2s death: exit 0x80000003
             (STATUS_BREAKPOINT), reached in the MAIN LOOP well past the joystick
             poll, with the last heartbeat the only witness. Name the guest cs:ip
             and the bytes there so the INT3's origin is visible. Bounded; the top
             bit of an NTSTATUS is set for both warning (0x8...) and error (0xC...). */
        if ((UINT)vdmStatus & NT_STATUS_NOT_SUCCESS_BIT_U) {
            static INT stateBudget = 16;
            if (stateBudget > 0) {
                DWORD stateCs = VDM_REG16(tib, VTIB_CS), si = VDM_REG16(tib, VTIB_EIP);
                const volatile BYTE *sp3 = (const volatile BYTE *)((stateCs << PARAGRAPH_SHIFT) + si);
                UINT index7;
                --stateBudget;
                cursor = LogPut(cursor, "V86-STATUS 0x"); cursor = LogHex(cursor, (UINT)vdmStatus);
                cursor = LogPut(cursor, " ev=0x"); cursor = LogHex(cursor, event);
                cursor = LogPut(cursor, " at 0x"); cursor = LogHex(cursor, stateCs); cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, si);
                cursor = LogPut(cursor, " bytes:");
                for (index7 = 0; index7 < 8; ++index7) { cursor = LogPut(cursor, " "); cursor = LogHexByte(cursor, sp3[index7]); }
                cursor = LogPut(cursor, "\r\n");
                LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
            }
        }
        /* SPIKE: once in protected mode, stop at the FIRST PM event and dump the raw
           taxonomy (event/info/selectors) -- this is how the spike learns how the
           monitor reflects a PM INT 31h / fault. Later increments replace this with
           a real INT 31h dispatch. */
        if (g_DpmiPm) {
            DWORD currentCs = VDM_REG16(tib, VTIB_CS), currentIp = VDM_REG16(tib, VTIB_EIP);
            cursor = LogPut(cursor, "STAGE3-DPMI: PM stop event=0x"); cursor = LogHex(cursor, event);
            cursor = LogPut(cursor, " status=0x"); cursor = LogHex(cursor, (UINT)vdmStatus);
            cursor = LogPut(cursor, " info=0x"); cursor = LogHex(cursor, VDM_REG(tib, VTIB_EVENT_INFO));
            cursor = LogPut(cursor, " CS:IP=0x"); cursor = LogHex(cursor, currentCs); cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, currentIp);
            cursor = LogPut(cursor, " EFL=0x"); cursor = LogHex(cursor, VDM_REG(tib, VTIB_EFLAGS));
            cursor = LogPut(cursor, " SS:SP=0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_SS));
            cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_ESP));
            cursor = LogPut(cursor, "\r\n  VTIB[5A8..]: "); cursor = LogDump(cursor, (const VOID *)(tib + 0x5A8), 0x20);
            cursor = LogPut(cursor, "\r\n");
            LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
            break;
        }
        {
            INT flow = V86ServiceIoEvent(&cursor, base, event, tib);
            if (flow == HOST_FLOW_CONTINUE) continue;
        }
        {
            INT flow = V86ReportUnexpectedStop(&cursor, base, event, tib, vdmStatus);
            if (flow == HOST_FLOW_BREAK) break;
        }
        { static INT bopBudget = 6; VdmStateSample("bop", tib, &bopBudget); }
        /* ── ★★★ WHOSE BOP IS THIS? THE NUMBER DOES NOT SAY. (s78) ──────────────────
             `C4 C4 nn` is NTVDM's call-out instruction and the number space is NTVDM's,
             not ours. Ours were assigned freely and two of them are already taken by a
             guest that ships with the OS: XP's COMMAND.COM issues `BOP 0x54` fifteen
             times and `BOP 0x50` once, while ours are DPMI_RMRET and the DPMI entry.
           ★ THE DISCRIMINATOR IS THE ADDRESS, NOT THE NUMBER. Every BOP we plant, we
             plant at an address we own -- DOS_HDLR_SEG for the INT stubs, the DPMI
             entry/return catchers and the callback slots, DOS_CTAB_SEG for the BIOS
             stubs. A BOP executing anywhere else is the GUEST's own code calling
             NTVDM, and must not be answered as if it were one of ours.
           ⚠ This is cheaper AND safer than renumbering ours out of the way: the numbers
             we would move to are equally NTVDM's, so renumbering only relocates the
             collision, while the origin check is exact. See docs/inventory/bop.md. */
        {
            DWORD bopCs = VDM_REG16(tib, VTIB_CS);
            g_BopFromGuest = (bopCs != DOS_HDLR_SEG && bopCs != DOS_CTAB_SEG);
            ++g_BopHistogram[VDM_REG(tib, VTIB_EVENT_INFO) & BYTE_MASK];     /* #238 */
        }
        /* Route the BOP by its number.
           ── The BOP numbers our stubs share with the nested DPMI loop: INT 10h/16h/33h,
             the BIOS block (11h-17h, 25h/26h, 28h/29h), 1Ah, 2Fh, the XMS entry and
             INT 67h. One copy, in V86BiosBop() (GH #247). */
        {   INT biosResult = V86BiosBop(tib, VDM_REG(tib, VTIB_EVENT_INFO) & BYTE_MASK, &cursor, base);
            if (biosResult != V86BOP_NONE) continue;          /* DONE or RERUN: both resume the guest */
        }
        if ((VDM_REG(tib, VTIB_EVENT_INFO) & BYTE_MASK) == MS_CB_BOP) {   /* INT 33h handler returned */
            MouseCallbackReturn(tib);
            continue;
        }
        {
            INT flow = V86ServiceKeyboardBop(tib);
            if (flow == HOST_FLOW_CONTINUE) continue;
        }
        {
            INT flow = V86ServiceTimerBop(tib);
            if (flow == HOST_FLOW_CONTINUE) continue;
        }
        {
            INT flow = DosServiceTerminateBop(&cursor, base, tib, machine);
            if (flow == HOST_FLOW_BREAK) break;
            if (flow == HOST_FLOW_CONTINUE) continue;
        }
        /* ⚠ `!g_BopFromGuest`: XP's COMMAND.COM issues a `BOP 0x50` of its own (one
             site, in its "Incorrect DOS version" path). Without the origin test that
             would be serviced as a DPMI real-to-protected mode switch. */
        if (!g_BopFromGuest
            && (VDM_REG(tib, VTIB_EVENT_INFO) & BYTE_MASK) == DPMI_BOP) {  /* DPMI real->PM switch */
            {
                INT flow = DpmiStartClientSession(&cursor, base, tib, machine);
                if (flow == HOST_FLOW_BREAK) break;
                if (flow == HOST_FLOW_CONTINUE) continue;
            }
        }
        /* ── ★★ THIS IS THE FALL-THROUGH, AND IT ANSWERS FOR EVERY BOP IT WAS NEVER
               GIVEN. (s78) ──────────────────────────────────────────────────────
             ev is already known to be VDM_EVENT_BOP here, and every arm above matches
             an EXACT code -- INT 21h's own stub is `C4 C4 20` (see the bop[] table at
             the install). So anything that reaches this line is a BOP we do not
             implement, and we hand it to the DOS INT 21h handler anyway, where the
             guest's AH decides what it "asked" for.
           ★ XP's COMMAND.COM is the guest that made this visible. It is NTVDM-AWARE:
             its image contains sixteen `C4 C4` sites -- fifteen of them BOP 0x54 with
             a sub-function byte after it, one BOP 0x50. Our 0x54 is DPMI_RMRET_BOP and
             our 0x50 is the DPMI entry, so the NUMBERS COLLIDE with the ones a real
             NTVDM guest uses. The one at 0x9342:0x03ce arrives with AX=0x0002, falls
             through here, is read as INT 21h AH=00, and terminates the guest -- which
             we then report as a CLEAN EXIT, CODE 0. Four sessions read that as
             "COMMAND.COM gives up"; it never called DOS at all.
           ⚠ THE GATE IS MEASURED, NOT ASSUMED. The arms above are long and one of them
             -- the BIOS block -- deliberately sets `handled = 0` and falls through, so
             "require 0x20" needed evidence rather than a reading of the control flow.
             The diagnostic below shipped first (92e2136) and the battery was run with
             it: 17 DOS guests (Doom, Hexen, Duke3D, Heretic, heaven7, Skyroads, Wolf3D,
             Mario, Lemmings, Chasm, Gothica, Radiance, Fusion, Skyxmas, vesacube, MEM,
             6.22's COMMAND.COM) and 3 Win16 apps (Notepad, Paint, WinMine) -- ALL of
             them confirmed loaded, **zero** fall-throughs. Only XP's COMMAND.COM
             reaches here. (⚠ Lemmings' first row was a NO SUCH TARGET and its zero was
             not evidence; re-run under its real entry, `lemvga.com`.)
           ⚠ WE STOP, we do not skip. Skipping needs the BOP's encoded LENGTH, and that
             is per-call: `0x54` carries a sub-function byte, so `EIP += 3` would leave
             that byte to execute as an instruction. A refusal we cannot encode is
             better reported than faked -- see the standing note that an unimplemented
             call which still ANSWERS is worse than one that does not. */
        /* ── ★★★ AN NTVDM BOP FROM THE GUEST'S OWN CODE. (s78) ──────────────────────
             XP's COMMAND.COM is NTVDM-aware and this is how it talks to the 32-bit side.
             Both of its numbers carry a SUB-FUNCTION BYTE after the BOP, so the
             instruction is FOUR bytes, not three -- read off the guest's bytes at the
             BOP site (logged), not assumed: what follows decodes as a sensible
             instruction at +4 and as junk at +3, for both 0x54 and 0x50.
             ⚠ That is a claim about THESE TWO numbers, from this one guest. It is not a
               general rule about BOP encoding, and must be re-derived for any other
               number that turns up here.
           ▶ WHAT TO ANSWER IS NOT KNOWN YET, so it is a knob rather than a guess:
             cfg\bop54.txt = "cf1" (default) or "cf0". What sub 01 does next depends on
             carry, so the two settings take COMMAND.COM down different paths and the
             difference is the measurement. Every call is logged with full
             registers so the two runs can be diffed.
           ⛔ A BOP IS NOT AN INT: nothing was pushed, so CF goes in the live EFLAGS. */
        /* s91 (#11): a guest's own `C4 C4 58 nn` is the third-party BOP -- see IsvBop.
             Four bytes (the sub-function follows), like 50h/54h below. */
        if (g_BopFromGuest && (VDM_REG(tib, VTIB_EVENT_INFO) & BYTE_MASK) == NTVDM_BOP_ISV) {
            DWORD codeSegment = VDM_REG16(tib, VTIB_CS), instructionPointer = VDM_REG16(tib, VTIB_EIP);
            const volatile BYTE *isvBopBytes = (const volatile BYTE *)(ULONG_PTR)((codeSegment << PARAGRAPH_SHIFT) + instructionPointer);
            IsvBop(tib, isvBopBytes[VDM_BOP_LENGTH], &cursor);
            VDM_REG(tib, VTIB_EIP) += VDM_BOP_SUBFUNCTION_LENGTH;
            continue;
        }
        if (g_BopFromGuest
            && ((VDM_REG(tib, VTIB_EVENT_INFO) & BYTE_MASK) == NTVDM_BOP_CMD
                || (VDM_REG(tib, VTIB_EVENT_INFO) & BYTE_MASK) == NTVDM_BOP_DOS)) {
            {
                INT flow = NtvdmServiceGuestBop(&cursor, base, tib, machine, programPathBuffer);
                if (flow == HOST_FLOW_BREAK) break;
                if (flow == HOST_FLOW_CONTINUE) continue;
            }
        }
        if ((VDM_REG(tib, VTIB_EVENT_INFO) & BYTE_MASK) != DOS_BOP_INT21) {
            DWORD codeSegment = VDM_REG16(tib, VTIB_CS), instructionPointer = VDM_REG16(tib, VTIB_EIP);
            const volatile BYTE *isvBopBytes = (const volatile BYTE *)(ULONG_PTR)((codeSegment << PARAGRAPH_SHIFT) + instructionPointer);
            cursor = LogPut(cursor, "STAGE2: UNIMPLEMENTED BOP -- refusing (NOT an INT 21h call): bop=0x");
            cursor = LogHexByte(cursor, VDM_REG(tib, VTIB_EVENT_INFO) & BYTE_MASK);
            cursor = LogPut(cursor, " next=0x"); cursor = LogHexByte(cursor, isvBopBytes[VDM_BOP_LENGTH]);   /* the sub-function byte */
            cursor = LogPut(cursor, " at 0x");  cursor = LogHex(cursor, codeSegment); cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, instructionPointer);
            cursor = LogPut(cursor, " ax=0x");  cursor = LogHex(cursor, VDM_REG16(tib, VTIB_EAX));
            cursor = LogPut(cursor, " bx=0x");  cursor = LogHex(cursor, VDM_REG16(tib, VTIB_EBX));
            cursor = LogPut(cursor, " dx=0x");  cursor = LogHex(cursor, VDM_REG16(tib, VTIB_EDX));
            cursor = LogPut(cursor, "\r\n         see docs/inventory/bop.md -- the C4 C4 number space is NTVDM's\r\n");
            LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
            /* A DISTINCT, NON-ZERO exit code. The whole defect this closes was a guest
               killed by an unimplemented call and reported as a clean exit 0; reusing 0
               here would leave the lie in place with better logging on top of it. */
            machine->ExitCode = UNIMPLEMENTED_BOP_EXIT_CODE;
            break;
        }
        /* ── #34: THE GUEST'S INT 24h HAS ANSWERED. Recognised by ADDRESS: BOP 20h is
             also the INT 21h BOP, and this one sits at DOS_CRIT_RETURN, where only
             CriticalRaise ever sends the guest. */
        if (!g_BopFromGuest && VDM_REG16(tib, VTIB_CS) == DOS_CTAB_SEG
            && VDM_REG16(tib, VTIB_EIP) == DOS_CRIT_RETURN) {
            INT criticalAction = CriticalReturn(machine, tib, &cursor);
            LogAppend(LOG_PATH, base, cursor); cursor = base;
            if (criticalAction) {                                /* ABORT: end the program, as 4Ch */
                if (DosTerminate(machine, tib, &cursor, base)) continue;
                break;
            }
            continue;                                /* RETRY re-runs it; FAIL/IGNORE resume */
        }
        if (!machine->IsCritActive) CriticalSnapshot(tib);      /* #34: the call's INPUT registers (not the handler's own calls) */
        machine->TraceCursor = cursor;
        machine->IsRetry = 0;
        machine->CanTrampoline = 1;                         /* #251: we can resume elsewhere */
        machine->CanRaiseCrit = 1;                        /* #275: ...and raise INT 24h (below) */
        if (!DosInt21(machine)) {                       /* AH=4Ch -> terminate */
            machine->CanTrampoline = 0;
            machine->CanRaiseCrit = 0;
            cursor = machine->TraceCursor;
            if (DosTerminate(machine, tib, &cursor, base)) continue;
            break;
        }
        machine->CanTrampoline = 0;
        machine->CanRaiseCrit = 0;
        cursor = machine->TraceCursor;
        if (machine->Trampoline) {                          /* #251: into DOS's AUX/PRN driver code */
            VDM_SET16(tib, VTIB_CS, DOS_CTAB_SEG);
            VDM_REG(tib, VTIB_EIP) = machine->Trampoline;
            machine->Trampoline = 0;
            LogAppend(LOG_PATH, base, cursor); cursor = base;
            continue;
        }
        if (machine->IsCritPending) {                       /* #34: call the guest's INT 24h */
            CriticalRaise(machine, tib, &cursor);
            LogAppend(LOG_PATH, base, cursor); cursor = base;
            continue;                               /* CS:IP is now the INT 24h site */
        }
        if (machine->IsExecPending) {                       /* GH #30: AH=4Bh */
            machine->IsExecPending = 0;
            cursor = ExecBegin(machine, tib, cursor);
            LogAppend(LOG_PATH, base, cursor); cursor = base;
            continue;                               /* CS:IP now points at the child */
        }
        /* A blocking read with nothing to return leaves EIP ON the BOP, so the guest
           re-executes the INT and keeps running -- and keeps taking timer interrupts, so
           its music and animation carry on while it waits for a key, as on real hardware. */
        if (!machine->IsRetry) VDM_REG(tib, VTIB_EIP) += VDM_BOP_LENGTH;  /* past the 3-byte BOP -> the IRET */
        LogAppend(LOG_PATH, base, cursor); cursor = base;
    }
    *cursorIo = cursor; *vdmStatusIo = vdmStatus;
}


/* Interrupt vectors a third-party VDD claimed (claim_int): a user vector gets one of the generic stubs;
   a system vector, or one past the last stub, is not delivered -- and the log says which. */
static VOID StartupReportVectorWiring(VOID)
{
    /* ── s91 (#315): EVERY CLAIMED VECTOR GETS A WAY IN. A claim only reached its device
         where the host had wired a stub by number (10h 14h 16h 1Ah 08h 2Ah 5Ch), so a
         third-party driver's claim_int on any other vector -- the SDK promises it --
         was never delivered. Each such claim now gets a generic stub, and the IVT
         points at it. ⚠ Only the vectors nobody else owns: the user vectors 60h-66h,
         68h-6Fh and 78h-FEh. A claim on a DOS, BIOS or IRQ vector is logged and
         refused rather than silently stealing it from the system. */
    {   UINT number, count = 0;
        volatile BYTE *controlBytes = (volatile BYTE *)(ULONG_PTR)((DWORD)DOS_CTAB_SEG << PARAGRAPH_SHIFT);
        for (number = 0; number < IVT_VECTORS; ++number) {
            INT wired = (number == VECTOR_TIMER || number == VECTOR_VIDEO || number == VECTOR_SERIAL || number == VECTOR_KEYBOARD_SERVICES || number == VECTOR_TIME
                         || number == VECTOR_NETWORK || number == VECTOR_NETBIOS);
            INT user  = (number >= VECTOR_USER_RANGE1_FIRST && number <= VECTOR_USER_RANGE1_LAST) || (number >= VECTOR_USER_RANGE2_FIRST && number <= VECTOR_USER_RANGE2_LAST)
                        || (number >= VECTOR_USER_RANGE3_FIRST && number <= VECTOR_USER_RANGE3_LAST);
            CHAR gateLine[120], *gateCursor = gateLine;
            if (!g_Bus.Interrupts[number].Service || wired) continue;
            gateCursor = LogPut(gateCursor, "  VDD: claim_int 0x"); gateCursor = LogHex(gateCursor, number);
            if (!user || count >= DOS_GENSTUB_N) {
                gateCursor = LogPut(gateCursor, user ? " -- no generic stub left; NOT delivered\r\n"
                                   : " -- a system vector; NOT delivered (user vectors only)\r\n");
            } else {
                UINT offset = DOS_GENSTUB_OFF + count * DOS_GENSTUB_SIZE;
                controlBytes[offset + 0] = VDM_BOP0; controlBytes[offset + 1] = VDM_BOP1;
                controlBytes[offset + VDM_BOP_NUMBER_OFFSET] = DOS_GENSTUB_BOP; controlBytes[offset + VDM_BOP_LENGTH] = X86_OP_IRET;            /* IRET */
                g_GenericStubVector[count] = (BYTE)number;
                *(volatile WORD *)(ULONG_PTR)(IVT_OFFSET_ADDRESS(number))     = (WORD)offset;
                *(volatile WORD *)(ULONG_PTR)(IVT_SEGMENT_ADDRESS(number)) = DOS_CTAB_SEG;
                ++count;
                gateCursor = LogPut(gateCursor, " -> generic stub 0090:0x"); gateCursor = LogHex(gateCursor, offset); gateCursor = LogPut(gateCursor, "\r\n");
            }
            LogAppend(LOG_PATH, gateLine, gateCursor);
        }
    }
}


/* krnl386's entry: VdmSetEntry set a DOS program's registers; krnl386 wants DS = its data segment and AX = 4B4Fh ('OK'). */
static PSTR StartupPrepareWowEntry(PSTR cursor, volatile BYTE * const tib, DOS_IMAGE *image)
{
    if (g_WowEntering) {
        /* VdmSetEntry points DS/ES/FS/GS at the PSP and zeroes AX, which is right
           for a DOS program and wrong for this one. krnl386 wants DS = its automatic
           data segment, and it expects AX = 0x4b4f -- 'OK' -- at entry; with anything
           else it returns at once with AX=0. Get AX wrong and it returns instantly,
           which would read as "the entry did nothing" rather than "we failed a
           handshake". Measured at the entry breakpoint; see session 30 part 5. */
        VDM_SET16(tib, VTIB_DS, g_WowEntryDs);
        /* ★ AND ES, WHICH IS NOT COSMETIC: krnl386 takes ES+0x10 as the base of the
             DPMI host's private data and carves every later allocation upward from
             there without asking DOS. VdmSetEntry points ES at DOS_PSP_SEG, whose
             +0x10 is where the (discarded) DOS image sat and where DosMcbAllocate had
             already placed krnl386's own code. Point it at the arena block instead. */
        if (g_WowPspSegment) VDM_SET16(tib, VTIB_ES, g_WowPspSegment);
        /* ★ CX = HOW MUCH MEMORY IS AVAILABLE ABOVE THE STACK, IN BYTES.
             krnl386 takes CX at entry, as a byte count, for the size of the block its
             selector over base(SS)+SP describes (observed: the arena it then uses is
             CX >> 4 paragraphs). Measured at three breakpoints, CX was 0 all the way
             from entry, so krnl386 believed it had ZERO paragraphs and every
             allocation out of that arena failed -- including one inside LoadSegment,
             which is why it could not load its own segment 1 and exited.
           The selector has a 64 KB limit, so this is the whole of it minus the header
           image we place at its base. Nothing else names this quantity to the guest. */
        VDM_SET16(tib, VTIB_ECX, (WORD)g_WowEntryCx);
        VDM_REG(tib, VTIB_EAX) = WOW_KRNL386_ENTRY_AX;
        cursor = LogPut(cursor, "STAGE2: WOW entry -- krnl386 in V86 at 0x");
        cursor = LogHex(cursor, image->CodeSegment); cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, image->InstructionPointer);
        cursor = LogPut(cursor, " DS=0x"); cursor = LogHex(cursor, g_WowEntryDs);
        cursor = LogPut(cursor, " ES=0x"); cursor = LogHex(cursor, g_WowPspSegment);
        cursor = LogPut(cursor, " CX=0x"); cursor = LogHex(cursor, g_WowEntryCx);
        cursor = LogPut(cursor, " (it will carve from 0x"); cursor = LogHex(cursor, (DWORD)(g_WowPspSegment + DOS_PSP_PARAGRAPHS));
        cursor = LogPut(cursor, ") AX=0x4b4f\r\n");
    }
    return cursor;
}


/* cfg\cpuspeed.txt: the CPU-speed setting (a decimal index into the speed list), overriding Settings. */
static VOID StartupLoadCpuSpeedKnob(VOID)
{
    { HANDLE cpuSpeedFile = CreateFileA(CPUSPD_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, 0, NULL);
      if (cpuSpeedFile != INVALID_HANDLE_VALUE) {
          /* ── ⚠⚠ THIS READ ONE CHARACTER, SO HALF THE LADDER WAS UNREACHABLE. ────
               `c[0] - '0'` cannot express an index above 9, and the ladder went to
               17 in session 54 when it grew from 7 entries to 18. So every speed
               from 75 MHz down -- 75, 66, 50, 33, 25, 16, 12, 8, which is ALL of
               the period-hardware settings and every one a person would actually
               reach for -- silently selected the FIRST DIGIT instead: `13` (33 MHz)
               ran as index 1, i.e. 3300 MHz, and reported itself as doing so.
             ⚠ THE RANGE CHECK HID IT rather than catching it. `c[0] < '0' +
               CPUSPEED_COUNT` with COUNT=18 accepts characters up to 'A', so a
               two-digit value passed the guard on its first digit and was accepted
               as a valid index -- a bounds test that admits exactly the input it
               should have rejected.
             ★ MEASURED 2026-09-09: `echo 13 > cpuspd.txt` came back
               `STAGE2: cpuspeed idx=00000001 mhz=00000ce4` -- 3300 MHz. The rig
               sweep this knob exists to drive therefore never tested the slow half
               of the ladder even once, and cpuswp.bat only ever swept 0-6.
             ► The CPUREF reader four lines above already does it correctly. Same
               loop here; there is no reason for two adjacent knobs to disagree.
             ⚠ This is the FILE knob only. The menu and the Settings dialog set the
               index directly and were never affected, so it is a testability defect
               and not the cause of any speed a user has seen. */
          CHAR text[8]; DWORD bytesRead = 0; UINT value9 = 0; INT index9;
          ReadFile(cpuSpeedFile, text, sizeof text, &bytesRead, NULL); CloseHandle(cpuSpeedFile);
          for (index9 = 0; index9 < (INT)bytesRead; ++index9) { if (text[index9] < '0' || text[index9] > '9') break;
                                             value9 = value9 * DECIMAL_RADIX_U + (UINT)(text[index9] - '0'); }
          if (index9 > 0 && value9 < (UINT)CPUSPEED_COUNT) {
              g_CpuSpeedIndex = (INT)value9;
              SettingsNoteOverride(SET_SPEEDMODE, CFG_TEXT(KNOB_FILE_CPUSPD), value9);
          }
      } }
}


/* cfg\vwatch.txt: a planar watchpoint at a video RAM offset, when the file is present. */
static VOID StartupLoadVideoWatch(VOID)
{
    /* ⚠ AFTER VddBusAdd, NOT BEFORE. VddBusAdd calls VddVideoInitialize, which
       disarms the watchpoint -- setting it first looked right and was silently
       undone, and the run came back with no trace and no error. */
    /* cfg/vwatch.txt: a hex VRAM byte offset to record every planar write to. Off
       unless the file is there -- see the watchpoint in vdd_video.c. */
    { HANDLE watchHandle = CreateFileA(VWATCH_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL, OPEN_EXISTING, 0, NULL);
      if (watchHandle != INVALID_HANDLE_VALUE) {
          CHAR watchText[32]; DWORD watchBytesRead = 0, watchValue = 0, watchIndex; INT gotWatch = 0;
          ReadFile(watchHandle, watchText, sizeof watchText - 1, &watchBytesRead, NULL); CloseHandle(watchHandle);
          for (watchIndex = 0; watchIndex < watchBytesRead; ++watchIndex) {
              INT digit = -1; CHAR digitCharacter = watchText[watchIndex];
              if (digitCharacter >= '0' && digitCharacter <= '9') digit = digitCharacter - '0';
              else if (digitCharacter >= 'a' && digitCharacter <= 'f') digit = digitCharacter - 'a' + HEX_DIGIT_A_VALUE;
              else if (digitCharacter >= 'A' && digitCharacter <= 'F') digit = digitCharacter - 'A' + HEX_DIGIT_A_VALUE;
              if (digit < 0) break;
              watchValue = (watchValue << NIBBLE_SHIFT) | (DWORD)digit; gotWatch = 1;
          }
          if (gotWatch) {
              CHAR watchLine[96], *watchCursor = watchLine;
              g_Video.WatchOffset = watchValue;
              watchCursor = LogPut(watchCursor, "STAGE0: vwatch.txt -> planar watchpoint at VRAM offset 0x");
              watchCursor = LogHex(watchCursor, watchValue); watchCursor = LogPut(watchCursor, "\r\n");
              LogAppend(LOG_PATH, watchLine, watchCursor); SerialOut(watchLine, watchCursor);
          }
      } }
}


/* Build DOS's drive tables -- the DPBs and the CDS -- for the drive letters that exist (GetLogicalDrives),
   capped at what the reserved space holds, and the device chain from the NUL device. */
static VOID StartupBuildDriveTables(CHAR *report, volatile BYTE * const sysVars, DOS_MACHINE *machine)
{
    /* ── ★ THE REAL CHAINS: DPB, CDS AND THE DEVICE HEADER. (GH #48) ────────────
         Until now everything above +0x20 was deliberately zero, and that choice was
         right while there was nothing truthful to put there: a walker that follows
         a garbage DPB pointer wanders into nonsense, whereas a null one stops.
         But it caps what memory- and disk-aware software can do, and #47 names it
         as a likely reason MEM.EXE lies.
       ▸ EVERY OFFSET AND EVERY STRUCTURE SIZE HERE WAS DUMPED OFF MS-DOS 6.22 by
         tests/probes/dos/p_sysvar.asm and decoded in src/dos/dos_sysvars.h -- which is
         what #48 asks for in as many words, because "the layout dumps have twice
         caught errors that a plausible reading would have missed". The DPB being 33
         bytes, for instance, is not recalled: 6.22's first DPB is at 0116:136A and
         its own `next` pointer says 0116:138B, and 0x138B - 0x136A = 0x21.
       ▸ The structures live in a block taken from the MCB chain rather than in the
         resident filler, which has under 800 bytes free and cannot hold a
         LASTDRIVE-long CDS array. Real DOS's are resident too, so the memory it
         costs is honest rather than an accounting trick. */
    {   volatile BYTE *controlTable = (volatile BYTE *)(DOS_CTAB_SEG << PARAGRAPH_SHIFT);
        DWORD drives = GetLogicalDrives();
        UINT drive, count = 0, slot[DOS_DPBCHAIN_MAX], driveCount = 0;
        PSTR scan = report;
        /* Which drive letters exist, capped at what the reserved space holds.
           Counted FIRST, because each DPB's `next` pointer has to name the one
           after it and the last must terminate -- and a chain that does not
           terminate is not a cosmetic fault. The same mistake on the SFT chain
           had krnl386 reading the IVT as an SFT header and looping through
           117 MB of DPMI calls (see DOS_SFT_* in dos_layout.h). */
        /* ⚠ SUPPRESS THE HARDWARE-ERROR DIALOG FIRST, AND SKIP REMOVABLES.
             GetDiskFreeSpaceA("A:\\") on a machine whose floppy drive is empty
             raises XP's "There is no disk in drive A:" box and BLOCKS on it --
             at host startup, with no window up and nothing in the log, so it
             presents as a hang and not as an error. It wedged the rig on the
             first run of this code. SEM_FAILCRITICALERRORS makes the call fail
             instead of asking, and DRIVE_REMOVABLE is skipped outright: a DPB
             for a drive whose media can vanish is not worth the risk here. */
        UINT previousErrorMode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
        UINT driveTypes[DOS_DRIVE_LETTERS];
        /* ── s71: EVERY DRIVE THE MACHINE HAS, CLASSIFIED ONCE. ──────────────────────
             Fixed and RAM disks get a DPB measured off the volume; a REMOVABLE drive
             (A:) gets a DPB with floppy defaults and its media is NEVER touched here
             (the "no disk in drive A:" box is modal and wedged the rig once); CD-ROM
             and network drives are redirector drives -- a CDS entry with the network
             flag and no DPB, which is how MSCDEX and a redirector present them. */
        for (drive = 0; drive < DOS_DRIVE_LETTERS; ++drive) {
            CHAR rootText[4];
            driveTypes[drive] = DRIVE_NO_ROOT_DIR;
            if (!(drives & (1u << drive))) continue;
            rootText[0] = (CHAR)('A' + drive); rootText[1] = ':'; rootText[2] = '\\'; rootText[3] = 0;
            driveTypes[drive] = GetDriveTypeA(rootText);
            if (driveTypes[drive] != DRIVE_FIXED && driveTypes[drive] != DRIVE_RAMDISK
                && driveTypes[drive] != DRIVE_REMOVABLE) continue;
            if (driveCount < DOS_DPBCHAIN_MAX) slot[driveCount++] = drive;
        }
        for (count = 0; count < driveCount; ++count) {
            DWORD sectorsPerCluster = 0, bytesPerSector = 0, freeClusters = 0, totalClusters = 0;
            CHAR root[4]; UINT item; BYTE driveParameterBlock[DOS_DPB_LEN];
            INT last = (count + 1 == driveCount);
            root[0] = (CHAR)('A' + slot[count]); root[1] = ':'; root[2] = '\\'; root[3] = 0;
            /* #48: a removable drive gets 6.22's OWN 1.44M floppy DPB -- 224 root entries,
                 media F0h -- which DosDpbBuild now reproduces byte for byte (sysvars_test).
                 It had 512 entries and F8h (the fixed-disk values) on a floppy. */
            INT isRemovable = (driveTypes[slot[count]] == DRIVE_REMOVABLE);
            if (isRemovable)                                           /* 1.44M defaults, no probe */
                { sectorsPerCluster = 1; bytesPerSector = DOS_DPB_DEFAULT_SECTOR_SIZE; totalClusters = DOS_FLOPPY_144_CLUSTERS; }
            else if (!GetDiskFreeSpaceA(root, &sectorsPerCluster, &bytesPerSector, &freeClusters, &totalClusters))
                { sectorsPerCluster = DOS_UNMEASURED_SECTORS_PER_CLUSTER; bytesPerSector = DOS_DPB_DEFAULT_SECTOR_SIZE; totalClusters = DOS_UNMEASURED_CLUSTERS; }
            DosDpbBuild(driveParameterBlock, slot[count], bytesPerSector ? bytesPerSector : DOS_DPB_DEFAULT_SECTOR_SIZE, sectorsPerCluster ? sectorsPerCluster : 1, isRemovable ? DOS_FLOPPY_144_ROOT_ENTRIES : DOS_FIXED_ROOT_ENTRIES,
                          (totalClusters > DOS_DPB_CLUSTER_LIMIT) ? DOS_DPB_CLUSTER_LIMIT : totalClusters + 1, isRemovable ? DOS_MEDIA_FLOPPY_144 : DOS_MEDIA_FIXED,
                          DOS_DEV_SEG, DOS_DEVICE_OFFSET(DOS_DEVICE_BLOCK),       /* #48: the block driver */
                          last ? DOS_CHAIN_END : DOS_CTAB_SEG,
                          last ? DOS_CHAIN_END : (WORD)(DOS_DPBCHAIN_OFF + (count + 1) * DOS_DPB_LEN));
            for (item = 0; item < DOS_DPB_LEN; ++item)
                controlTable[DOS_DPBCHAIN_OFF + count * DOS_DPB_LEN + item] = driveParameterBlock[item];
        }
        if (driveCount) {
            *(volatile WORD *)(sysVars + DOS_SYSVARS_OFF + DOS_SYSVARS_DPB)     = DOS_DPBCHAIN_OFF;
            *(volatile WORD *)(sysVars + DOS_SYSVARS_OFF + DOS_SYSVARS_DPB + X86_FAR_POINTER_SEGMENT) = DOS_CTAB_SEG;
        } else {                                  /* no chain is better than a bad one */
            *(volatile WORD *)(sysVars + DOS_SYSVARS_OFF + DOS_SYSVARS_DPB)     = DOS_CHAIN_END;
            *(volatile WORD *)(sysVars + DOS_SYSVARS_OFF + DOS_SYSVARS_DPB + X86_FAR_POINTER_SEGMENT) = DOS_CHAIN_END;
        }
        *(volatile WORD *)(sysVars + DOS_SYSVARS_OFF + DOS_SYSVARS_MAX_SECTOR) = DOS_DPB_DEFAULT_SECTOR_SIZE;
        /* ── ★ SYSVARS+0x45: EXTENDED MEMORY, IN KB. THE ANSWER TO GH #47. ────
             MEM.EXE does not get this from the XMS driver. It reads it straight
             out of SysVars and skips its entire extended-memory report when the
             word is zero:
                 07B5  les bx,[..] / cmp word [es:bx+0x45],0 / jz 0x907
             Measured on 6.22: SysVars+0x45 = 0x3B80 = 15232, and MEM prints
             "Extended (XMS) 15,232K" -- the same number, which is what
             identifies the field. Ours read zero because the swappable data
             area used to be planted at SysVars+0x44, so the InDOS byte WAS
             this field. The SDA has moved; see DOS_SDA_OFF.
           The value is the XMS pool, so the two cannot disagree. */
        /* ── ★ #48: IT IS THE MACHINE'S EXTENDED MEMORY, NOT THE XMS POOL. ─────────
             The field is what SYSINIT read from INT 15h AH=88h at boot, before
             HIMEM loaded (RBIL: "extended memory size in K"). 6.22 says so itself:
             MEM /D there prints Extended 15,597,568 = 65,536 used + 15,532,032 free
             (runs/s81_mem/oracle_memd.txt) -- the total is this field (15232K), the
             free is HIMEM's AH=08h, and the 64K "used" is the HMA, which XMS never
             counts. Ours said 16384 (the pool) while AH=88h and CMOS 17h/30h said
             15360 -- one machine with two sizes. Now all three are CMOS_EXTENDED_KB and the
             pool is that LESS the HMA (XMS_POOL_KB), so MEM's Total - Free = Used
             comes out 64K, 6.22's shape, and can never go negative. */
        *(volatile WORD *)(sysVars + DOS_SYSVARS_OFF + DOS_SYSVARS_EXTENDED_KB) = (WORD)CMOS_EXTENDED_KB;
        /* ⚠ GH #47: SysVars +0x43 = 0x0103, +0x49 = 0xFFFF and +0x4B = 0x0001
           (6.22's values, where ours are zero) were planted together as a
           diagnostic and REFUTED -- the phantom "Upper 1,663K" did not move.
           +0x49 was the prime suspect on the theory that zero reads as "the UMB
           chain starts at segment 0". It does not. Seventh refutation. */
        sysVars[DOS_SYSVARS_OFF + DOS_SYSVARS_BLOCK_DEVICES] = (BYTE)driveCount;
        /* ---- the device chain. The NUL header is INLINE at +0x22, not a pointer
               to one (measured on 6.22). #48: it no longer terminates -- it links to
               IO.SYS's twelve (CON .. COM4) at DOS_DEV_SEG, in 6.22's order, and the
               last of those terminates. See DosDeviceChainBuild for what is measured
               (the order, the stride, which pointers name which) and what is not (the
               attribute words). SysVars+0x08/+0x0C name CLOCK$ and CON, as on 6.22.
             ⚠ Linear 0x600..0x6E7 had been the environment block's until #207 moved
               it to DOS_ENV_SEG 0x7F; nothing else writes there (dos_layout.h). */
        {   BYTE nulDevice[DOS_SYSVARS_NUL_LEN], device[DOS_DEVICE_AREA_LEN], nulStub[DOS_NULSTUB_LEN]; UINT item;
            volatile BYTE *deviceArea = (volatile BYTE *)(ULONG_PTR)((DWORD)DOS_DEV_SEG << PARAGRAPH_SHIFT);
            DosDeviceChainBuild(device, DOS_DEV_SEG, driveCount);
            for (item = 0; item < DOS_DEVICE_AREA_LEN; ++item) deviceArea[item] = device[item];
            DosNulStubBuild(nulStub);
            for (item = 0; item < DOS_NULSTUB_LEN; ++item) sysVars[DOS_NULSTUB_OFF + item] = nulStub[item];
            DosNulHeaderBuild(nulDevice, DOS_DEV_SEG, DOS_DEVICE_OFFSET(DOS_DEVICE_CON),
                          DOS_NULSTUB_OFF + DOS_NULSTUB_STRAT, DOS_NULSTUB_OFF + DOS_NULSTUB_INTR);
            for (item = 0; item < DOS_SYSVARS_NUL_LEN; ++item)
                sysVars[DOS_SYSVARS_OFF + DOS_SYSVARS_NUL + item] = nulDevice[item];
            *(volatile WORD *)(sysVars + DOS_SYSVARS_OFF + DOS_SYSVARS_CLOCK)     = DOS_DEVICE_OFFSET(DOS_DEVICE_CLOCK);
            *(volatile WORD *)(sysVars + DOS_SYSVARS_OFF + DOS_SYSVARS_CLOCK + X86_FAR_POINTER_SEGMENT) = DOS_DEV_SEG;
            *(volatile WORD *)(sysVars + DOS_SYSVARS_OFF + DOS_SYSVARS_CON)       = DOS_DEVICE_OFFSET(DOS_DEVICE_CON);
            *(volatile WORD *)(sysVars + DOS_SYSVARS_OFF + DOS_SYSVARS_CON + X86_FAR_POINTER_SEGMENT)   = DOS_DEV_SEG; }
        /* ---- the CDS array. ONE ENTRY PER DRIVE LETTER, LASTDRIVE of them,
               because it is INDEXED by drive and a walker reads all of them
               whatever we populate. Entries for drives that exist carry flags
               0x4000 (physical) and a pointer to that drive's DPB; the rest are
               zeroed with a terminated DPB pointer rather than one dangling at
               the array's base. Measured layout: path at +0, flags at +0x43,
               DPB far pointer at +0x45, backslash offset at +0x4F, stride 88. */
        /* In its own reserved block now (g_CdsSegment), 26 entries -- see DOS_LASTDRIVE. */
        if (g_CdsSegment) {
            volatile BYTE *cdsBytes = (volatile BYTE *)((DWORD)g_CdsSegment << PARAGRAPH_SHIFT);
            UINT di2, seen2 = 0;
            for (di2 = 0; di2 < DOS_LASTDRIVE; ++di2) {
                BYTE cds[DOS_CDS_LEN]; UINT item, flags = 0;
                INT have = 0, index2;
                for (index2 = 0; index2 < (INT)driveCount; ++index2) if (slot[index2] == di2) have = 1;
                if (have) flags = DOS_CDS_FLAG_PHYSICAL;
                else if (driveTypes[di2] == DRIVE_CDROM || driveTypes[di2] == DRIVE_REMOTE)
                    flags = DOS_CDS_FLAG_PHYSICAL | DOS_CDS_FLAG_NETWORK;
                DosCdsBuild(cds, di2, flags, DOS_CTAB_SEG,
                              (WORD)(DOS_DPBCHAIN_OFF + seen2 * DOS_DPB_LEN));
                if (have) ++seen2;
                for (item = 0; item < DOS_CDS_LEN; ++item)
                    cdsBytes[di2 * DOS_CDS_LEN + item] = cds[item];
            }
            *(volatile WORD *)(sysVars + DOS_SYSVARS_OFF + DOS_SYSVARS_CDS)     = 0x0000;
            *(volatile WORD *)(sysVars + DOS_SYSVARS_OFF + DOS_SYSVARS_CDS + X86_FAR_POINTER_SEGMENT) = g_CdsSegment;
        } else {                                  /* no block: no array, say so */
            *(volatile WORD *)(sysVars + DOS_SYSVARS_OFF + DOS_SYSVARS_CDS)     = DOS_CDS_ARRAY_NONE;
            *(volatile WORD *)(sysVars + DOS_SYSVARS_OFF + DOS_SYSVARS_CDS + X86_FAR_POINTER_SEGMENT) = DOS_CDS_ARRAY_NONE;
        }
        /* ---- the SYSTEM FILE TABLE: one block, terminated, entries = what our
               INT 21h layer can really open (DOS_MACHINE::fh[]). Same shape the
               WOW path plants -- see the reserve above for why a DOS guest needs
               one and why 0000:0000 was worse than useless. The entries are left
               zeroed, which is not a stub: that is what a free SFT entry looks
               like, and nothing has been opened this early.
             ⚠ THE TERMINATOR IS THE POINT AT LEAST AS MUCH AS THE COUNT IS, so
               the no-block path below writes FFFF:FFFF rather than falling back to
               the zero this change exists to remove. */
        if (g_SftSegment) {
            volatile BYTE *sft = (volatile BYTE *)((DWORD)g_SftSegment << PARAGRAPH_SHIFT);
            UINT item;
            for (item = 0; item < (UINT)DOS_SFT_BYTES; ++item) sft[item] = 0;
            *(volatile WORD *)(sft + DOS_SFT_NEXT_OFFSET) = DOS_SFT_LAST;             /* next offset: last block */
            *(volatile WORD *)(sft + DOS_SFT_NEXT_SEGMENT) = DOS_SFT_LAST;             /* next segment            */
            *(volatile WORD *)(sft + DOS_SFT_COUNT) = DOS_SFT_ENTRIES;    /* entries in this block   */
            *(volatile WORD *)(sysVars + DOS_SYSVARS_OFF + DOS_SYSVARS_SFT)     = 0x0000;
            *(volatile WORD *)(sysVars + DOS_SYSVARS_OFF + DOS_SYSVARS_SFT + X86_FAR_POINTER_SEGMENT) = g_SftSegment;
        } else {
            *(volatile WORD *)(sysVars + DOS_SYSVARS_OFF + DOS_SYSVARS_SFT)     = DOS_SFT_LAST;
            *(volatile WORD *)(sysVars + DOS_SYSVARS_OFF + DOS_SYSVARS_SFT + X86_FAR_POINTER_SEGMENT) = DOS_SFT_LAST;
        }
        scan = LogPut(scan, "DOS: SysVars ");     scan = LogHex(scan, driveCount);
        scan = LogPut(scan, " DPBs at 0x");       scan = LogHex(scan, DOS_CTAB_SEG);
        scan = LogPut(scan, ":");                 scan = LogHex(scan, DOS_DPBCHAIN_OFF);
        scan = LogPut(scan, " (terminated), NUL inline -> CON..COM4 at 0x"); scan = LogHex(scan, DOS_DEV_SEG);
        scan = LogPut(scan, ":0 (terminated), first MCB 0x"); scan = LogHex(scan, machine->FirstMcb);
        scan = LogPut(scan, " above SysVars 0x"); scan = LogHex(scan, DOS_SYSVARS_SEG);
        scan = LogPut(scan, " (#207), "); scan = LogHex(scan, DOS_LASTDRIVE);
        scan = LogPut(scan, " CDS entries at 0x"); scan = LogHex(scan, g_CdsSegment);
        scan = LogPut(scan, ":0, SFT ");
        if (g_SftSegment) { scan = LogPut(scan, "at 0x"); scan = LogHex(scan, g_SftSegment);
                         scan = LogPut(scan, ":0 x"); scan = LogHex(scan, (DWORD)DOS_SFT_ENTRIES);
                         scan = LogPut(scan, " entries (terminated)"); }
        else             scan = LogPut(scan, "ABSENT -- chain head terminated FFFF:FFFF");
        scan = LogPut(scan, ", drives=");
        for (drive = 0; drive < DOS_DRIVE_LETTERS; ++drive) {
            if (!(drives & (1u << drive))) continue;
            scan = LogPut(scan, driveTypes[drive] == DRIVE_FIXED ? " " : driveTypes[drive] == DRIVE_REMOVABLE ? " ~"
                       : driveTypes[drive] == DRIVE_CDROM ? " cd:" : driveTypes[drive] == DRIVE_REMOTE ? " net:"
                       : driveTypes[drive] == DRIVE_RAMDISK ? " ram:" : " ?");
            { CHAR driveLetter[2]; driveLetter[0] = (CHAR)('A' + drive); driveLetter[1] = 0; scan = LogPut(scan, driveLetter); }
        }
        scan = LogPut(scan, " (GH #48)\r\n");
        SetErrorMode(previousErrorMode);
        LogAppend(LOG_PATH, report, scan); SerialOut(report, scan);
    }
}


/* Plant the country tables in DOS memory: upper-case, file-name upper-case, file-name terminators and collation. */
static VOID StartupPlantCountryTables(VOID)
{
    /* GH #38: plant the AH=65h character tables in the DOS-resident block. */
    { volatile BYTE *controlTable = (volatile BYTE *)(DOS_CTAB_SEG << PARAGRAPH_SHIFT); UINT item;
      for (item = 0; item < sizeof(g_DosCtabUpper);   ++item) controlTable[DOS_CTAB_UPPER   + item] = g_DosCtabUpper[item];
      for (item = 0; item < sizeof(g_DosCtabFileNameUpper); ++item) controlTable[DOS_CTAB_FNUPPER + item] = g_DosCtabFileNameUpper[item];
      for (item = 0; item < sizeof(g_DosCtabFileNameTerminators);  ++item) controlTable[DOS_CTAB_FNTERM  + item] = g_DosCtabFileNameTerminators[item];
      for (item = 0; item < sizeof(g_DosCtabCollate); ++item) controlTable[DOS_CTAB_COLLATE + item] = g_DosCtabCollate[item];
      for (item = 0; item < sizeof(g_DosCtabDbcs);    ++item) controlTable[DOS_CTAB_DBCS    + item] = g_DosCtabDbcs[item];
      /* ── THE INT 2Fh AX=122Eh TABLES, ZEROED EXPLICITLY. ──────────────────
           The block is MCB-reserved and in practice arrives zeroed, but a guest
           that reads a table we never wrote is reading whatever the last run
           left there -- and this is exactly the region where a stale pointer had
           krnl386 writing into our own handler code. Cheap to be certain.
         ⚠ 192 bytes covers all three tables (A, B, C at 0x4E0/0x520/0x560). */
      for (item = 0; item < DOS_INT2F_TBLS_LEN; ++item) controlTable[DOS_INT2F_TBL_A + item] = 0;
      /* ── INT 15h AH=C0h: THE SYSTEM CONFIGURATION TABLE. (GH #54) ──────────────
           Measured (tests/probes/dos/p_int15.asm): PCem's real AMI 486 says FC 01 00
           70 00; SeaBIOS FC 00 01 74 40; dosbox-x FC 00 01 70 40. The model triple
           is the AMI's -- the period machine. The FEATURE BITS ARE NOT COPIED from
           anyone: each one is a claim about THIS machine, and a claim a guest can
           act on, so only the true ones are set.
             f1 bit 6  second 8259 present ............ yes (vdd_pic)
             f1 bit 5  real-time clock present ........ yes (vdd_cmos)
             f1 bit 4  INT 09h calls INT 15h AH=4Fh ... YES (#244) -- bios_kbdact.asm
                       k4f, made whenever IVT[15h] is not our own stub (our default
                       would answer "process it" unchanged, so skipping it then is
                       indistinguishable). Was NO (64h); AMI, SeaBIOS and dosbox-x set it
             f1 bit 2  EBDA allocated ................. YES (#253) -- 1 KB at 9FC0h,
                       which is what INT 12h's 639 KB always implied; AH=C1h and
                       0040:000E now say so too. Was NO (60h) while C1h refused --
                       see bios_bda.h for why the EBDA, not 640 KB, is the answer
             f2 bit 6  INT 16h AH=09h supported ....... yes (vdd_input) */
      {   static const BYTE systemConfiguration[10] = { 0x08, 0x00, 0xFC, 0x01, 0x00,
                                            0x74, 0x40, 0x00, 0x00, 0x00 };
          for (item = 0; item < sizeof systemConfiguration; ++item) controlTable[DOS_SYSCONF_OFF + item] = systemConfiguration[item]; } }
}


/* How INT 21h AH=53h answers, and where that came from: the built-in model, or a cfg\ override. */
static PSTR StartupConfigureAh53Answers(PSTR cursor)
{
    /* ── INT 21h AH=53h's PRIVATE SUB-FUNCTIONS, AS A KNOB. (s79) ────────────────────
         XP's COMMAND.COM asks AH=53h with AL as a selector, and AL=5's answer decides
         whether it ever reads the keyboard: answered 1, it never does (observed: no
         AH=0Ah, the shell goes past its prompt). Stock IS interactive,
         so stock must answer AL=0 there -- while our probe measured AL=1 with its
         output redirected to a file. Until that is re-measured un-redirected, the
         answers are a table a run can change, not a constant a rebuild can.
       ⚠ THE DEFAULT IS THE MEASURED VALUE, so a run with no file behaves exactly as
         before. This is deliberately NOT a fix.
       ⚠ And it prints unconditionally, with its source -- the dosver knob had two
         sources and logged a line only when one of them won, which is how the rig
         reported DOS 5.00 to every guest for an unknown number of sessions. */
    { PCSTR int53Source = "built-in (measured vs stock ntvdm, 2026-09-25)";
      HANDLE handle;
      /* ── ★★ THE CONTEXT-DEPENDENCE, MODELLED RATHER THAN OVERRIDDEN. (s79) ─────────
           The measured stock answers (AL=2 -> CF=0, AL=5 -> AL=1) do not let XP's
           COMMAND.COM read a key: with AL=5 answered 1 it never reaches its keyboard
           read (see above). Stock IS interactive, so stock answers
           differently WHEN THE SHELL ASKS -- the call is context-dependent, and the
           context we measured in was a standalone probe with its stdout redirected.
         ⇒ So model the context instead of claiming a new universal value: an
           NTVDM-AWARE SHELL gets the answers that make it a shell; every other guest,
           and every probe, still gets the measured ones. That is narrower than the
           `cfg\int53.txt` knob it replaces, and it cannot affect anything else.
         ⚠ STILL PROVISIONAL, BUT NARROWER (s84, #142). `p_int53f.com` under stock, with
           and without redirection, answers IDENTICALLY (5305 -> AL=1 both ways), so
           redirection is NOT the context that flips it. What is left is the caller:
           a probe is always a CHILD of stock's shell, and only the shell itself can be
           asked for the AL=0 answer this branch gives it. Kept as the model until
           that can be measured. It is marked here so it cannot quietly become folklore. */
      if (g_GuestNtAware) {
          g_DosInt53Answers[DOS_INT53_SHELL_LOOP].Ax = DOS_FN_BPB_TO_DPB << BYTE_SHIFT; g_DosInt53Answers[DOS_INT53_SHELL_LOOP].IsCarry = 1;   /* top of its main loop */
          /* #208: a ROUTED program is the shell's work, not the keyboard's. CF=0 here sends
             its loop to BOP 54 sub 01 ("what next?") instead of the prompt -- so when the
             program ends the shell ASKS, and we decide: done (close the window) or, after
             Close Program, the prompt. See the sub 01 arm. */
          if (g_Routed) g_DosInt53Answers[DOS_INT53_SHELL_LOOP].IsCarry = 0;
          g_DosInt53Answers[DOS_INT53_STARTUP].Ax = DOS_FN_BPB_TO_DPB << BYTE_SHIFT; g_DosInt53Answers[DOS_INT53_STARTUP].IsCarry = 0;   /* -> [0x327] = 0       */
          int53Source = "NTVDM-aware shell (PROVISIONAL -- see p_int53f)";
      }
      handle = CreateFileA(INT53_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                      NULL, OPEN_EXISTING, 0, NULL);
      if (handle != INVALID_HANDLE_VALUE) {
          CHAR text[512]; DWORD bytesRead = 0, index = 0;
          ReadFile(handle, text, sizeof text - 1, &bytesRead, NULL); CloseHandle(handle);
          while (index < bytesRead) {
              UINT versions[INT53_FIELDS]; INT fieldCount = 0;
              /* one line */
              while (index < bytesRead && (text[index] == ' ' || text[index] == '\t')) ++index;
              if (index < bytesRead && (text[index] == ';' || text[index] == '#')) { while (index < bytesRead && text[index] != '\n') ++index; }
              while (index < bytesRead && text[index] != '\n' && fieldCount < INT53_FIELDS) {
                  UINT value = 0; INT got = 0;
                  while (index < bytesRead && (text[index] == ' ' || text[index] == '\t')) ++index;
                  while (index < bytesRead) {
                      CHAR digitCharacter = text[index];
                      INT digit = (digitCharacter >= '0' && digitCharacter <= '9') ? digitCharacter - '0'
                            : (digitCharacter >= 'a' && digitCharacter <= 'f') ? digitCharacter - 'a' + HEX_DIGIT_A_VALUE
                            : (digitCharacter >= 'A' && digitCharacter <= 'F') ? digitCharacter - 'A' + HEX_DIGIT_A_VALUE : -1;
                      if (digit < 0) break;
                      value = value * HEX_RADIX_U + (UINT)digit; got = 1; ++index;
                  }
                  if (!got) break;
                  versions[fieldCount++] = value;
              }
              if (fieldCount == INT53_FIELDS && versions[INT53_FIELD_AL] < DOS_INT53_COUNT) {
                  g_DosInt53Answers[versions[INT53_FIELD_AL]].Ax = (WORD)versions[INT53_FIELD_AX];
                  g_DosInt53Answers[versions[INT53_FIELD_AL]].IsCarry = (BYTE)(versions[INT53_FIELD_CARRY] ? 1 : 0);
                  int53Source = "cfg\\int53.txt";
              }
              while (index < bytesRead && text[index] != '\n') ++index;
              if (index < bytesRead) ++index;
          }
      }
      cursor = LogPut(cursor, "STAGE2: INT 21h AH=53h answers (source: ");
      cursor = LogPut(cursor, int53Source); cursor = LogPut(cursor, ")");
      { UINT item;
        for (item = 0; item < DOS_INT53_COUNT; ++item) {
            cursor = LogPut(cursor, " "); cursor = LogHexByte(cursor, (BYTE)item); cursor = LogPut(cursor, "=");
            cursor = LogHex(cursor, g_DosInt53Answers[item].Ax);
            cursor = LogPut(cursor, g_DosInt53Answers[item].IsCarry ? "/C" : "/c");
        } }
      cursor = LogPut(cursor, "\r\n"); }
    return cursor;
}


/* cfg\dosver.txt: the DOS version to report, when the file is present. */
static PCSTR StartupLoadDosVersionKnob(PCSTR dosVersionSource, DOS_MACHINE *machine)
{
    { HANDLE handle = CreateFileA(DOSVER_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             NULL, OPEN_EXISTING, 0, NULL);
      if (handle != INVALID_HANDLE_VALUE) {
          CHAR text[16]; DWORD bytesRead = 0; UINT index = 0, major = 0, minor = 0;
          ReadFile(handle, text, sizeof text - 1, &bytesRead, NULL);
          CloseHandle(handle);
          while (index < bytesRead && text[index] >= '0' && text[index] <= '9') { major = major*DECIMAL_RADIX + (UINT)(text[index]-'0'); ++index; }
          if (index < bytesRead && text[index] == '.') {
              ++index;
              while (index < bytesRead && text[index] >= '0' && text[index] <= '9') { minor = minor*DECIMAL_RADIX + (UINT)(text[index]-'0'); ++index; }
          }
          if (major && major < BYTE_VALUES && minor < BYTE_VALUES) {
              DosInt21SetVersion(machine, (BYTE)major, (BYTE)minor);
              SettingsNoteOverride(SET_DOSMAJ, CFG_TEXT(KNOB_FILE_DOSVER), major);
              SettingsNoteOverride(SET_DOSMIN, CFG_TEXT(KNOB_FILE_DOSVER), minor);
              dosVersionSource = CFG_TEXT(KNOB_FILE_DOSVER);
              g_DosVersionForced = 1;
              g_DosVersionWhy = SETTINGS_DOS_VERSION_WHY;
          }
      } }
    return dosVersionSource;
}


/* Lay DOS's memory-control-block chain, initialise INT 21h on it, and name the program's block as DOS 4+ does. */
static VOID StartupBuildMemoryChain(DOS_MACHINE *machine, CHAR *programPathBuffer)
{
    {   WORD firstMcb = DosMcbInitializeWithTop(NULL, g_DosMemoryTop);   /* #136 */
        DosInt21Initialize(machine, firstMcb);
        /* The program's name in its MCB, as DOS 4+ writes it (#47: MEM /D). After
           DosMcbInitialize, which lays the chain and clears the name byte. */
        DosMcbSetOwnerName(NULL, DOS_PSP_SEG, programPathBuffer);
        /* The CDS array's block, at the top of the chain (see DOS_LASTDRIVE). The
           PSP was built with DOS_MEM_TOP as its memory top; the program's block now
           ends one paragraph below the reserved block's data, and PSP+2 must say so
           or a program that resizes itself to "PSP+2 - PSP" fails with error 8. */
        /* ⚠⚠ ONE RESERVATION, CARVED -- NOT TWO CALLS. DosMcbReserveTop() splits
             the LAST 'Z' block, and its first act is to make the block it split an
             'M' and put the new 'Z' on top. So a SECOND call finds the block the
             FIRST one just reserved and tries to split THAT: 143 paragraphs, which
             cannot hold the SFT's 473, so it returned 0 and the SFT silently came
             out ABSENT. Measured, first run -- the log said "SFT ABSENT" while the
             memory it needed was sitting free below. Reserve the pair in one go and
             carve it: CDS at the bottom, SFT immediately above. One MCB owned by
             DOS (8) covering both is what it is -- resident DOS data. */
        {   WORD reservedSegment = DosMcbReserveTop(NULL, firstMcb,
                                            (WORD)(DOS_CDS_PARAS + DOS_SFT_PARAS));
            if (reservedSegment) { g_CdsSegment = reservedSegment; g_SftSegment = (WORD)(reservedSegment + DOS_CDS_PARAS); }
            else        g_CdsSegment = DosMcbReserveTop(NULL, firstMcb, DOS_CDS_PARAS);
        }
        /* ── ★ AND THE SFT, FOR A **DOS** GUEST. (s72) ────────────────────────────
             The SFT chain was planted only on the WOW path, and the note there said
             so in as many words -- "a DOS guest still gets SysVars+4 = 0 ... when a
             DOS program needs the SFT, this moves". A DOS program now has: p_sysvar
             walks the List of Lists and reads the chain head as absent.
           ⚠⚠ AND ABSENT IS THE DANGEROUS PART, NOT THE MISSING PART. SysVars+4 = 0
             does not mean "no SFT", it means "an SFT at segment 0" -- so a program
             that walks the chain reads the IVT as SFT headers. That is exactly what
             krnl386 did: 0x00000000 -> 0x000fa357 -> 0x000bc370 -> back, forever,
             117 MB of DPMI calls in one run (see DOS_SFT_* in dos_layout.h). The
             only reason no DOS guest had hit it is that none had looked.
           ▸ THE MEMORY IS AFFORDABLE, AND THAT WAS MEASURED, NOT ASSUMED. The block
             is 473 paragraphs (7.4 KB). p_tsr puts the largest free block at 0x962F
             here against 0x9302 on genuine MS-DOS 6.22 -- we hand out 12.7 KB MORE
             than real DOS -- so after this we are still 5.3 KB ahead of it, and a
             guest that fits on 6.22 still fits here.
           ▸ Reserved at the top like the CDS, owned by DOS (8), so it is resident
             data a memory walker can see and account for rather than a hole. It is
             taken AFTER the CDS, so it lands just below it and the program's block
             now ends below THIS one -- hence PSP+2 comes from the lower of the two. */
        if (g_CdsSegment)
            *(volatile WORD *)(((DWORD)DOS_PSP_SEG << PARAGRAPH_SHIFT) + DOS_PSP_MEMORY_TOP) = (WORD)(g_CdsSegment - 1);
    }
}


/* Build the guest's DOS environment block, with cfg\dosenv.txt's extra variables, the launcher compiler
   variables, and argv[0] -- shortened for the guest when it has to be. */
static PSTR StartupBuildEnvironment(PSTR cursor, CHAR *programPathBuffer)
{
    { static CHAR dosEnvironment[192];
      HANDLE handle = CreateFileA(DOSENV_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             NULL, OPEN_EXISTING, 0, NULL);
      dosEnvironment[0] = 0;
      if (handle != INVALID_HANDLE_VALUE) {
          DWORD bytesRead = 0;
          ReadFile(handle, dosEnvironment, sizeof dosEnvironment - 1, &bytesRead, NULL);
          CloseHandle(handle);
          dosEnvironment[bytesRead < sizeof dosEnvironment ? bytesRead : sizeof dosEnvironment - 1] = 0;
          if (dosEnvironment[0]) {
              cursor = LogPut(cursor, "STAGE2: dosenv.txt -> extra guest environment [");
              cursor = LogPut(cursor, dosEnvironment); cursor = LogPut(cursor, "]\r\n");
          }
      }
      /* ── ★★★★ argv[0] MUST BE 8.3, BECAUSE A DOS EXTENDER RE-OPENS IT. (s73) ─────
           DOS/4GW loads its own protected-mode half by re-opening the program named in
           the environment's program-path field, and it tokenises that name -- so a path
           with spaces is torn at the first one and the open fails. It then prints its
           banner and stops, with no error and no video mode set.
         ▸ THAT IS THE "HEADLESS-ONLY DOS/4GW BLOCKER", and it was never headless-only:
           it is PATH-specific, exactly as the note said. A by-hand launch works because
           CSRSS hands us the SHORT name already (C:\DOCUME~1\...\DOOM.EXE); the
           target.txt path handed over the long one (C:\Documents and Settings\...),
           and only the harness used that. Any user whose games live under a path with
           a space had the same broken launch.
         ▸ Shortened here, once, where argv[0] is built -- so every launch shape agrees
           with the one that was already working. WowShorten leaves the path alone if
           GetShortPathNameA cannot answer, so a path that has no 8.3 form is unchanged. */
      if (programPathBuffer[0]) {
          CHAR before[768]; lstrcpynA(before, programPathBuffer, sizeof before);
          WowShorten(programPathBuffer, sizeof programPathBuffer);
          if (lstrcmpA(before, programPathBuffer) != 0) {
              cursor = LogPut(cursor, "STAGE2: argv[0] shortened for the guest: ["); cursor = LogPut(cursor, before);
              cursor = LogPut(cursor, "] -> ["); cursor = LogPut(cursor, programPathBuffer); cursor = LogPut(cursor, "]\r\n");
          }
          /* ── ★★★★★ AND IF IT IS STILL TOO LONG, HAND OVER THE BARE NAME. ──────────
               DOS/4GW 1.97 copies argv[0] into a **64-BYTE BUFFER** and does not bound
               it. MEASURED, three ways, from this one share:
                 doom\DOOM.EXE        62 chars -> loads and plays
                 hexen\HEXEN.EXE      64 chars -> "fatal error (1007): can't find file
                                      ...\HEXEN\HEXEN.EXE< to load"  (no room for the
                                      NUL, so it reads one byte of garbage)
                 heretic\HERETIC.EXE  68 chars -> truncated at 64: "...\HERETICD"
               Copying HEXEN.EXE alone to a 61-char path fixed it outright -- DOS/4GW
               loaded and Hexen got as far as looking for its WAD.
             ⚠ THIS IS WHY "HEXEN WORKED BEFORE AND DOES NOT NOW", and it is not a code
               regression: s73 moved the games from `games\Hexen\` (59) to
               `demo\msdos\hexen\` (64) and crossed the limit. A user installing a game
               under a deep path of their own hits exactly the same wall.
             ▸ The bare filename is safe because the guest's current directory IS the
               program's own directory (it is how every one of these games finds its
               WAD), so the extender's open resolves to the same file -- and 8.3 name
               plus NUL can never approach 64. Only done when it must be. */
          if (lstrlenA(programPathBuffer) > EXTENDER_ARGV0_SAFE_LENGTH) {
              PCSTR baseName = programPathBuffer, scan;
              for (scan = programPathBuffer; *scan; ++scan) if (*scan == '\\' || *scan == '/') baseName = scan + 1;
              if (baseName != programPathBuffer && *baseName) {
                  cursor = LogPut(cursor, "STAGE2: argv[0] is "); cursor = LogHex(cursor, (DWORD)lstrlenA(programPathBuffer));
                  cursor = LogPut(cursor, " chars -- past DOS/4GW's 64-byte buffer; handing over the bare name [");
                  cursor = LogPut(cursor, baseName); cursor = LogPut(cursor, "] (cwd is the program's own directory)\r\n");
                  { CHAR baseNameCopy[64]; lstrcpynA(baseNameCopy, baseName, sizeof baseNameCopy); LogPut(programPathBuffer, baseNameCopy); }
              }
          }
      }
      /* ── THE ENVIRONMENT: the four defaults + dosenv.txt + the launcher's LIB/INCLUDE,
           all in the ONE fixed 256-byte block at 0x60. The memory map does not move --
           see LauncherCompilerVariables for why that matters (a relocated block #GP'd every
           DOS extender). `extra` is dosenv.txt followed by the compiler vars. */
      { static CHAR environmentExtra[512]; DWORD extraOffset = 0;
        if (dosEnvironment[0]) { extraOffset = (DWORD)wsprintfA(environmentExtra, "%s", dosEnvironment);
                         if (extraOffset && environmentExtra[extraOffset-1] != '\n') environmentExtra[extraOffset++] = '\n'; }
        /* ── ULTRASND= IS HOW A GUS PROGRAM FINDS THE CARD, AND IT LOOKS BEFORE IT PROBES.
             heaven7 never touched a port without it. <base hex>,<DRAM DMA>,<record DMA>,
             <GF1 IRQ>,<MIDI IRQ> (docs/ref/gus.md §1) -- the numbers the device was built
             with, so the string and the card cannot disagree. A dosenv.txt ULTRASND wins. */
        if (g_GusOn && !StrStrNoCase(environmentExtra, HOST_ENV_ULTRASND_ASSIGN)) {
            extraOffset += (DWORD)wsprintfA(environmentExtra + extraOffset, HOST_ENV_ULTRASND_FORMAT,
                                   (UINT)g_Gus.BasePort, (UINT)g_Gus.DmaChannel, (UINT)g_Gus.DmaChannel,
                                   (UINT)g_Gus.Irq, (UINT)g_Gus.Irq);
        }
        if (g_Fetch2Ok) {
            UINT variableCount = LauncherCompilerVariables(g_Environment2, sizeof g_Environment2, environmentExtra + extraOffset,
                                                 (DWORD)sizeof environmentExtra - extraOffset);
            if (variableCount) { cursor = LogPut(cursor, "STAGE2: launcher compiler vars -> "); cursor = LogHex(cursor, variableCount);
                      cursor = LogPut(cursor, " (LIB/INCLUDE into the 0x60 block; memory map unmoved)\r\n"); }
        }
        DosEnvBuildWithCard(NULL, DOS_ENV_SEG, programPathBuffer[0] ? programPathBuffer : HOST_DEFAULT_PROGRAM_PATH,
                           HOST_DEFAULT_DRIVE_ROOT, &g_SbConfig, environmentExtra[0] ? environmentExtra : NULL);        /* M2.5: env */
      }
      /* ── ★ READ THE BLOCK BACK OUT OF GUEST MEMORY AND PRINT IT. Not the string we
           passed in -- the bytes the guest will actually walk, which is a different
           claim and the only one worth logging. A DOS environment is a run of
           NUL-terminated strings ended by a double NUL, then a count WORD, then the
           program path; every one of those can be got wrong, and "the variable is
           set" cannot be told from "the variable is set but the block is malformed"
           from the host side. NULs print as '.' so the string boundaries are visible.
         ⚠ This is what makes dosenv.txt a MEASURED feature rather than an asserted
           one -- see `an unimplemented call still answers`. */
      /* ⚠ ITS OWN BUFFER, NOT THE RUNNING REPORT. The first cut appended into `p` and
           bounded the printable characters with `p < base + 3800` -- by this point in
           the STAGE2 report that bound was already passed, so every readable byte was
           silently dropped and the dump came back as `[......]`, which reads exactly
           like an EMPTY ENVIRONMENT. An instrument that fails by printing a plausible
           wrong answer is worse than one that fails loudly. */
      { const volatile BYTE *environmentBytes = (const volatile BYTE *)((DWORD)DOS_ENV_SEG << PARAGRAPH_SHIFT);
        CHAR environmentLine[288], *environmentCursor = environmentLine;
        UINT environmentIndex, zeros = 0;
        environmentCursor = LogPut(environmentCursor, "STAGE2: guest environment block @0x");
        environmentCursor = LogHex(environmentCursor, (DWORD)DOS_ENV_SEG << PARAGRAPH_SHIFT); environmentCursor = LogPut(environmentCursor, " = [");
        for (environmentIndex = 0; environmentIndex < ENVIRONMENT_DUMP_BYTES && environmentCursor < environmentLine + ENVIRONMENT_DUMP_LINE; ++environmentIndex) {
            BYTE character = environmentBytes[environmentIndex];
            if (character == 0) { *environmentCursor++ = '.'; *environmentCursor = 0;
                          if (++zeros >= ENVIRONMENT_DUMP_NULS && environmentIndex > ENVIRONMENT_DUMP_SKIP) break;
                          continue; }
            zeros = 0;
            *environmentCursor++ = (CHAR)((character >= ASCII_SPACE && character < ASCII_DELETE) ? character : '?'); *environmentCursor = 0;
        }
        environmentCursor = LogPut(environmentCursor, "]\r\n");
        (VOID)environmentCursor;
        /* Into the running report, the same way every neighbouring line goes: a
           direct LogAppend here produced NOTHING in the file while the zput three
           statements above appeared, and an instrument that silently writes nowhere
           is not worth debugging twice. */
        cursor = LogPut(cursor, environmentLine); }
    }
    return cursor;
}


/* Plant a BOP stub for each BIOS interrupt we service (biosInts) in DOS memory. */
static VOID StartupPlantBiosStubs(VOID)
{
    {
      /* GH #43/#44/#45: the BIOS interrupts we had never planted at all. Until now
         these vectors were filled by the null-vector sweep with a bare IRET, so a
         guest asking for the equipment list or the memory size got silence and
         whatever was already in its registers. */
      /* {vector, BOP number}.  They match for all but INT 20h: BOP 0x20 is ALREADY
         the INT 21h handler's, and planting INT 20h with it made the BIOS dispatch
         intercept every INT 21h call as "terminate program" -- selftest exited at
         its first DOS call with no output. BOP numbers are a shared namespace with
         DPMI (0x50-0x57), XMS (0x43) and the rest; 0x30 is free. */
      static const BYTE biosInts[][2] = {
          { VECTOR_EQUIPMENT, DOS_BOP_FOR_VECTOR(VECTOR_EQUIPMENT) }, { VECTOR_MEMORY_SIZE, DOS_BOP_FOR_VECTOR(VECTOR_MEMORY_SIZE) }, { VECTOR_DISK, DOS_BOP_FOR_VECTOR(VECTOR_DISK) }, { VECTOR_SERIAL, DOS_BOP_FOR_VECTOR(VECTOR_SERIAL) },
          { VECTOR_SYSTEM, DOS_BOP_FOR_VECTOR(VECTOR_SYSTEM) }, { VECTOR_PRINTER, DOS_BOP_FOR_VECTOR(VECTOR_PRINTER) }, { VECTOR_ABSOLUTE_DISK_READ, DOS_BOP_FOR_VECTOR(VECTOR_ABSOLUTE_DISK_READ) }, { VECTOR_ABSOLUTE_DISK_WRITE, DOS_BOP_FOR_VECTOR(VECTOR_ABSOLUTE_DISK_WRITE) },
          { VECTOR_TERMINATE, DOS_BOP_INT20 },                                  /* GH #46: see above */
          { VECTOR_TERMINATE_RESIDENT, DOS_BOP_FOR_VECTOR(VECTOR_TERMINATE_RESIDENT) }, { VECTOR_DOS_IDLE, DOS_BOP_FOR_VECTOR(VECTOR_DOS_IDLE) }, { VECTOR_FAST_CONSOLE_OUTPUT, DOS_BOP_FOR_VECTOR(VECTOR_FAST_CONSOLE_OUTPUT) },
          { VECTOR_NETWORK, DOS_BOP_FOR_VECTOR(VECTOR_NETWORK) }, { VECTOR_NETBIOS, DOS_BOP_FOR_VECTOR(VECTOR_NETBIOS) },                  /* GH #8 (s91): NetBIOS, see V86BiosBop */
      };
      UINT byteIndex;
      volatile BYTE *controlBytes = (volatile BYTE *)(DOS_CTAB_SEG << PARAGRAPH_SHIFT);
      for (byteIndex = 0; byteIndex < sizeof(biosInts)/sizeof(biosInts[0]); ++byteIndex) {
          UINT offset = DOS_BIOS_STUBS + byteIndex * DOS_BIOS_STUB_SIZE;
          controlBytes[offset + 0] = VDM_BOP0; controlBytes[offset + 1] = VDM_BOP1;
          controlBytes[offset + VDM_BOP_NUMBER_OFFSET] = biosInts[byteIndex][1];
          /* ── INT 25h/26h RETURN WITH THE CALLER'S FLAGS STILL PUSHED. ───────
               Every other vector here ends in IRET. DOS's absolute disk read and
               write do NOT: they return by RETF, deliberately leaving the FLAGS
               word the INT pushed on the caller's stack, which the caller then
               discards itself (`add sp,2`). Ending them with IRET pops that word
               and the caller's `add sp,2` then eats its own return address --
               corruption that surfaces later, somewhere else. (GH #44) */
          controlBytes[offset + VDM_BOP_LENGTH] = (biosInts[byteIndex][0] == VECTOR_ABSOLUTE_DISK_READ || biosInts[byteIndex][0] == VECTOR_ABSOLUTE_DISK_WRITE)
                        ? X86_OP_RETF   /* RETF */
                        : X86_OP_IRET;  /* IRET */
          *(volatile WORD *)(IVT_OFFSET_ADDRESS(biosInts[byteIndex][0]))     = (WORD)offset;
          *(volatile WORD *)(IVT_SEGMENT_ADDRESS(biosInts[byteIndex][0])) = DOS_CTAB_SEG;
          if (biosInts[byteIndex][0] == VECTOR_SYSTEM) g_Int15StubOffset = (WORD)offset;   /* #244 */
      } }
}


/* Apply the Settings conventional-memory size (#136), unless the program does not fit under it. */
static PSTR StartupApplyConventionalKb(PSTR cursor, const DWORD readCount)
{
    /* ── #136: HOW MUCH CONVENTIONAL MEMORY THIS MACHINE HAS. Decided here, once, before
         the image is laid down: the loader writes the program above the PSP with no
         bound of its own, and the EBDA (BiosBdaInitializeWithTop, later) is zeroed at the new
         top -- so a program that does not fit under a small setting would be loaded and
         then have its own code wiped. A real DOS says "Program too big to fit in memory"
         at that point; the host cannot say that to a program it was launched to RUN, so
         it refuses the SETTING instead, loudly, and the machine stays at 640 KB.
         640 (the default) never enters this block and logs nothing new. */
    if (g_ConventionalKbWant != BIOS_CONV_KB_MAX) {
        WORD top = BiosConventionalTopParagraph(g_ConventionalKbWant), alloc = 0;
        WORD avail = (WORD)(top - DOS_PSP_SEG);
        INT high = 0, fits;
        if (readCount >= DOS_EXE_SIGNATURE_SIZE && g_FileBuffer[0] == 'M' && g_FileBuffer[1] == 'Z')
            fits = DosExecSize(g_FileBuffer, readCount, avail, &alloc, &high) == 0;
        else                                /* .COM: PSP + the image + a 256-byte stack */
            fits = (UINT32)DOS_PSP_PARAGRAPHS + ((readCount + DOS_PSP_SIZE + PARAGRAPH_LAST_BYTE_U) >> PARAGRAPH_SHIFT) <= (UINT32)avail;
        cursor = LogPut(cursor, "STAGE2: ConventionalKB=");  cursor = LogDecimal(cursor, g_ConventionalKbWant);
        if (fits) {
            g_DosMemoryTop = top;
            cursor = LogPut(cursor, " -> INT 12h ");       cursor = LogDecimal(cursor, BiosBaseKbOfTop(top));
            cursor = LogPut(cursor, " KB, EBDA + MCB top 0x"); cursor = LogHex(cursor, top);
            cursor = LogPut(cursor, " (#136)\r\n");
        } else {
            cursor = LogPut(cursor, " REFUSED: this program does not fit under it -- the machine stays at "
                        "640 KB (#136)\r\n");
            SettingsNoteOverride(SET_CONVKB, SETTINGS_SOURCE_CONV_KB, BIOS_CONV_KB_MAX);
        }
    }
    return cursor;
}


/* Nothing named a program: load a shell -- the one cfg\shell.txt names, or the fallback. */
static VOID StartupLoadShell(PSTR *cursorIo, DWORD *readCountIo, CHAR *shellConfig, PCSTR const shellSource, CHAR *programPathBuffer, INT *wasShellIo)
{
    PSTR cursor = *cursorIo;
    DWORD readCount = *readCountIo;
    INT wasShell = *wasShellIo;
    if (!readCount) {
        CHAR shell[512]; HANDLE shellHandle = INVALID_HANDLE_VALUE;
        PCSTR why = 0;
        static CHAR whyBuffer[96];
        if (shellConfig[0]) {
            lstrcpynA(shell, shellConfig, sizeof shell);
            shellHandle = CreateFileA(shell, GENERIC_READ, FILE_SHARE_READ, NULL,
                             OPEN_EXISTING, 0, NULL);
            why = shellSource;
            if (shellHandle == INVALID_HANDLE_VALUE) {
                LogPut(LogPut(whyBuffer, shellSource), " NAMES A FILE THAT WILL NOT OPEN");
                why = whyBuffer;
            } else {
                /* ⛔ A Windows program is not a DOS shell. The Browse filter allows *.exe
                     (a DOS shell can be one), so a user can pick cmd.exe; loaded as a DOS
                     guest a PE image just runs its stub or worse. Refuse it and fall back. */
                BYTE header[SHELL_HEADER_READ]; DWORD headerRead = 0;
                ReadFile(shellHandle, header, sizeof header, &headerRead, NULL);
                if (headerRead == sizeof header && header[0] == 'M' && header[1] == 'Z') {
                    DWORD newHeaderOffset = *(const DWORD *)(header + DOS_MZ_NEW_HEADER); BYTE signatureBytes[DOS_EXE_SIGNATURE_SIZE]; DWORD signatureRead = 0;
                    if (newHeaderOffset >= DOS_MZ_NEW_HEADER_MIN && SetFilePointer(shellHandle, (LONG)newHeaderOffset, NULL, FILE_BEGIN) == newHeaderOffset
                        && ReadFile(shellHandle, signatureBytes, DOS_EXE_SIGNATURE_SIZE, &signatureRead, NULL) && signatureRead == DOS_EXE_SIGNATURE_SIZE
                        && ((signatureBytes[0] == 'N' && signatureBytes[1] == 'E') || (signatureBytes[0] == 'P' && signatureBytes[1] == 'E'))) {
                        CloseHandle(shellHandle); shellHandle = INVALID_HANDLE_VALUE;
                        LogPut(LogPut(whyBuffer, shellSource), " NAMES A WINDOWS PROGRAM, NOT A DOS SHELL");
                        why = whyBuffer;
                    }
                }
                if (shellHandle != INVALID_HANDLE_VALUE) SetFilePointer(shellHandle, 0, NULL, FILE_BEGIN);
            }
        }
        if (shellHandle == INVALID_HANDLE_VALUE) {
            LogPut(shell, HOST_DEFAULT_SHELL_PATH);
            shellHandle = CreateFileA(shell, GENERIC_READ, FILE_SHARE_READ, NULL,
                             OPEN_EXISTING, 0, NULL);
            if (shellHandle != INVALID_HANDLE_VALUE && !why) why = "the system's own COMMAND.COM";
        }
        if (shellHandle != INVALID_HANDLE_VALUE) {
            ReadFile(shellHandle, g_FileBuffer, sizeof(g_FileBuffer), &readCount, NULL); CloseHandle(shellHandle);
            LogPut(programPathBuffer, shell);
            wasShell = 1;
            cursor = LogPut(cursor, "STAGE2: no program was named -> loading a SHELL, 0x");
            cursor = LogHex(cursor, readCount); cursor = LogPut(cursor, " from "); cursor = LogPut(cursor, shell);
            cursor = LogPut(cursor, " ("); cursor = LogPut(cursor, why ? why : "default"); cursor = LogPut(cursor, ")\r\n");
        }
    }
    *cursorIo = cursor; *readCountIo = readCount; *wasShellIo = wasShell;
}


/* #208: hand a program started from Windows to XP's COMMAND.COM, unless directlaunch.flag asks for the old way. */
static VOID StartupRouteToCommandCom(PSTR *cursorIo, DWORD *readCountIo, CHAR *shellConfig, CHAR *programPathBuffer, CHAR *args)
{
    PSTR cursor = *cursorIo;
    DWORD readCount = *readCountIo;
    if (readCount && !g_WowLaunch && !shellConfig[0]
        && GetFileAttributesA(DIRECTLAUNCH_FLAG) == INVALID_FILE_ATTRIBUTES) {
        INT pathLength = lstrlenA(programPathBuffer), isCommand = 0, isNewExe = 0;
        CHAR shortPath[300]; DWORD shortLength;
        isCommand = (pathLength >= COMMAND_COM_LENGTH && !lstrcmpiA(programPathBuffer + pathLength - COMMAND_COM_LENGTH, HOST_COMMAND_COM));
        if (readCount > DOS_MZ_NEW_HEADER_MIN && g_FileBuffer[0] == 'M' && g_FileBuffer[1] == 'Z') {
            DWORD newHeaderOffset = *(const DWORD *)(g_FileBuffer + DOS_MZ_NEW_HEADER);
            if (newHeaderOffset > DOS_MZ_NEW_HEADER_MIN && newHeaderOffset + DOS_EXE_SIGNATURE_SIZE < readCount
                && ((g_FileBuffer[newHeaderOffset] == 'N' && g_FileBuffer[newHeaderOffset + 1] == 'E')
                    || (g_FileBuffer[newHeaderOffset] == 'P' && g_FileBuffer[newHeaderOffset + 1] == 'E'))) isNewExe = 1;
        }
        shortLength = GetShortPathNameA(programPathBuffer, shortPath, sizeof shortPath);
        if (!isCommand && !isNewExe && shortLength && shortLength < ROUTED_PATH_MAX && shortLength + 1 + (DWORD)lstrlenA(args) < DOS_PSP_COMMAND_TAIL_MAX) {
            lstrcpynA(g_FirstProgram, shortPath, sizeof g_FirstProgram);
            lstrcpynA(g_FirstTail, args, sizeof g_FirstTail);
            g_Routed = 1;
            readCount = 0;                               /* -> the shell block below */
            args[0] = 0;                             /* they are the program's, not the shell's */
            cursor = LogPut(cursor, "STAGE2: #208 routing [");  cursor = LogPut(cursor, g_FirstProgram);
            cursor = LogPut(cursor, "] args=[");                cursor = LogPut(cursor, g_FirstTail);
            cursor = LogPut(cursor, "] through XP's COMMAND.COM (BOP 54 sub 01), as stock does\r\n");
        } else {
            cursor = LogPut(cursor, "STAGE2: #208 NOT routed (");
            cursor = LogPut(cursor, isCommand ? "it is a COMMAND.COM" : isNewExe ? "not a DOS image"
                              : "8.3 path + arguments too long for a DOS command line");
            cursor = LogPut(cursor, ") -- loading it directly\r\n");
        }
    }
    *cursorIo = cursor; *readCountIo = readCount;
}


/* A .PIF: parse it and take its program, start directory and parameters. */
static VOID StartupApplyPif(PSTR *cursorIo, DWORD *readCountIo, CHAR *programPathBuffer, CHAR *args)
{
    PSTR cursor = *cursorIo;
    DWORD readCount = *readCountIo;
    /* ── A .PIF NAMES A PROGRAM; IT IS NOT ONE. (s85) See pif.h. ─────────────────────
         Explorer queues the PIF itself as the program, and we handed its bytes to
         COMMAND.COM to execute. Stock NTVDM reads it: the program, its parameters (the
         WINDOWS 386 section's copy when there is one) and its start directory. A
         relative program is looked for in the start directory, then beside the PIF,
         then on the PATH. Arguments typed after the PIF follow the PIF's own. */
    if (readCount && !g_WowLaunch && programPathBuffer[0]) {
        INT pathLength = lstrlenA(programPathBuffer);
        PIF_INFO pif;
        if (pathLength > DOS_DOT_EXTENSION_LENGTH && !lstrcmpiA(programPathBuffer + pathLength - DOS_DOT_EXTENSION_LENGTH, HOST_EXTENSION_PIF)
            && PifParse(g_FileBuffer, readCount, &pif)) {
            CHAR program[MAX_PATH], directory[MAX_PATH], pifDirectory[MAX_PATH], pifCandidate[MAX_PATH], extra[256];
            HANDLE pifHandle = INVALID_HANDLE_VALUE;
            INT item;
            ExpandEnvironmentStringsA(pif.Program, program, sizeof program);
            ExpandEnvironmentStringsA(pif.Directory, directory, sizeof directory);
            lstrcpynA(pifDirectory, programPathBuffer, sizeof pifDirectory);
            for (item = lstrlenA(pifDirectory); item > 0 && pifDirectory[item - 1] != '\\'; --item) ;
            pifDirectory[item > 0 ? item - 1 : 0] = 0;
            if (directory[0] && GetFileAttributesA(directory) == INVALID_FILE_ATTRIBUTES) {
                cursor = LogPut(cursor, "STAGE2: PIF start directory ["); cursor = LogPut(cursor, directory);
                cursor = LogPut(cursor, "] does not exist -- ignored\r\n");
                directory[0] = 0;
            }
            if (program[0] == '\\' || (program[0] && program[1] == ':')) {
                lstrcpynA(pifCandidate, program, sizeof pifCandidate);
                pifHandle = CreateFileA(pifCandidate, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
            } else {
                PCSTR roots[2]; INT rootIndex;
                roots[0] = directory; roots[1] = pifDirectory;
                for (rootIndex = 0; rootIndex < 2 && pifHandle == INVALID_HANDLE_VALUE; ++rootIndex) {
                    PSTR wordStart;
                    if (!roots[rootIndex][0]) continue;
                    wordStart = LogPut(pifCandidate, roots[rootIndex]); wordStart = LogPut(wordStart, HOST_PATH_SEPARATOR); LogPut(wordStart, program);
                    pifHandle = CreateFileA(pifCandidate, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
                }
                if (pifHandle == INVALID_HANDLE_VALUE && SearchPathA(NULL, program, NULL, sizeof pifCandidate, pifCandidate, NULL))
                    pifHandle = CreateFileA(pifCandidate, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
            }
            lstrcpynA(extra, args, sizeof extra);
            cursor = LogPut(cursor, "STAGE2: PIF ["); cursor = LogPut(cursor, programPathBuffer);
            cursor = LogPut(cursor, "] -> program ["); cursor = LogPut(cursor, program);
            cursor = LogPut(cursor, "] dir=["); cursor = LogPut(cursor, directory);
            cursor = LogPut(cursor, "] params=["); cursor = LogPut(cursor, pif.Parameters);
            cursor = LogPut(cursor, pif.IsParametersFrom386 ? "] (WINDOWS 386 section)" : "] (basic section)");
            if (pifHandle == INVALID_HANDLE_VALUE) {
                /* Not found: do NOT run the PIF's bytes. A shell is the honest answer. */
                readCount = 0; programPathBuffer[0] = 0; args[0] = 0;
                cursor = LogPut(cursor, " -- PROGRAM NOT FOUND, starting a shell instead\r\n");
            } else {
                PSTR wordStart;
                readCount = 0;
                ReadFile(pifHandle, g_FileBuffer, sizeof(g_FileBuffer), &readCount, NULL); CloseHandle(pifHandle);
                lstrcpynA(programPathBuffer, pifCandidate, sizeof programPathBuffer);
                /* "?" asks Windows to prompt for parameters; there is no one to ask here. */
                wordStart = LogPut(args, (pif.Parameters[0] == '?' && !pif.Parameters[1]) ? "" : pif.Parameters);
                if (extra[0]) { if (args[0]) wordStart = LogPut(wordStart, " "); LogPut(wordStart, extra); }
                LogPut(g_WowCommandProgram, programPathBuffer); WowShorten(g_WowCommandProgram, sizeof g_WowCommandProgram);
                LogPut(g_WowCommandArguments, args);
                if (directory[0]) { lstrcpynA(g_CurrentDirectory, directory, sizeof g_CurrentDirectory); SetCurrentDirectoryA(g_CurrentDirectory); }
                cursor = LogPut(cursor, " -- loaded 0x"); cursor = LogHex(cursor, readCount);
                cursor = LogPut(cursor, " from ["); cursor = LogPut(cursor, programPathBuffer);
                cursor = LogPut(cursor, "] args=["); cursor = LogPut(cursor, args); cursor = LogPut(cursor, "]\r\n");
            }
        }
    }
    *cursorIo = cursor; *readCountIo = readCount;
}


/* Load the program from the command title when it is an absolute path (the title arrives with a trailing space). */
static VOID StartupLoadTitlePath(PSTR *cursorIo, DWORD *readCountIo, CHAR *programPathBuffer, CHAR *args)
{
    PSTR cursor = *cursorIo;
    DWORD readCount = *readCountIo;
    /* ── ★★★★★ AN ABSOLUTE TITLE IS NOT JOINED TO THE DIRECTORY. (GH #131,
         session 57) This composed `g_CurrentDirectory + "\" + g_Title` unconditionally, and
         CSRSS's title for a program named with a full path at a cmd prompt IS
         that full path -- so `hello.com > out.txt` run in C:\test produced

             STAGE1: program C:\test\C:\test\hello.com
             STAGE2: loaded 0x00000000 from C:\test\C:\test\hello.com
             STAGE2: embedded fallback

         -- a path that cannot exist, a load of zero bytes, and then the four-byte
         `mov ah,4Ch / int 21h` stub running INSTEAD OF THE PROGRAM. The run then
         completes cleanly and writes NOTHING, which is indistinguishable from
         the redirect defect this issue is about: the output file is empty either
         way. It is not the same bug, and it was hiding behind it -- the log
         reported ZERO INT 21h calls in the whole run, which is the tell.
       ⚠ TWO FORMS OF ABSOLUTE, both real: `C:\...` (a drive) and `\...` (rooted
         on the current drive). A relative title still gets the directory, which
         is what the join was for and is still right. */
    if (!readCount && g_Title[0]
        && (g_Title[0] == '\\' || (g_Title[1] == ':' && g_Title[2] == '\\'))) {
        CHAR path[768]; HANDLE fileHandle; int targetLength; PSTR targetArguments = NULL; /* stays int: INT here moves the compiled code */
        LogPut(path, g_Title);
        /* ⚠ AND THE TITLE ARRIVES WITH A TRAILING SPACE -- measured:
             `title=[C:\test\hello.com ]`. CreateFileA's treatment of one is not
             something to rely on, and a path is not a place to leave whitespace
             the guest never typed. */
        for (targetLength = 0; path[targetLength]; ++targetLength) ;
        while (targetLength > 0 && (path[targetLength - 1] == ' ' || path[targetLength - 1] == '\t')) path[--targetLength] = 0;
        fileHandle = CsrssOpenSplit(path, &targetArguments);        /* "program [args]" -- see the helper */
        if (fileHandle != INVALID_HANDLE_VALUE) { ReadFile(fileHandle, g_FileBuffer, sizeof(g_FileBuffer), &readCount, NULL); CloseHandle(fileHandle); }
        LogPut(programPathBuffer, path);
        /* The title's own arguments beat CSRSS's CmdLine field, which arrives as
           junk on this path (measured: `cmd=[\]` for a `ZAR.EXE -Help` launch) and
           was only ever a best-effort guess. */
        if (targetArguments)         LogPut(args, targetArguments);
        else if (g_CommandLine[0]) LogPut(args, g_CommandLine);
        cursor = LogPut(cursor, "STAGE2: loaded 0x"); cursor = LogHex(cursor, readCount);
        cursor = LogPut(cursor, " from "); cursor = LogPut(cursor, path);
        if (targetArguments) { cursor = LogPut(cursor, " args=["); cursor = LogPut(cursor, targetArguments); cursor = LogPut(cursor, "]"); }
        cursor = LogPut(cursor, " [the title is ALREADY ABSOLUTE -- not joined to the"
                    " current directory]\r\n");
    }
    *cursorIo = cursor; *readCountIo = readCount;
}


/* Load the application CSRSS named in the second fetch, recognising the harness's stub. */
static VOID StartupLoadCsrssApplication(PSTR *cursorIo, INT *wantShellIo, DWORD *readCountIo, CHAR *programPathBuffer, CHAR *args)
{
    PSTR cursor = *cursorIo;
    INT wantShell = *wantShellIo;
    DWORD readCount = *readCountIo;
    /* Load the program: what CSRSS asked for, else C:\ntvdmex\target.txt, else a
       tiny exit stub.
     ⚠⚠ THAT ORDER USED TO BE THE OTHER WAY ROUND, AND IT IS A BLOCKER ON #130.
       target.txt won UNCONDITIONALLY, so with NTVDMEX installed as the machine's
       VDM *every* DOS and Win16 launch ran whatever that file happened to name,
       whatever the user double-clicked. It also silently corrupted our own
       measurements: the launch matrix's stock column reported DPMI output under
       `p_ver.com` because a run had reused a stale target.
     ► THE TEST HARNESS IS UNAFFECTED, and that is measured rather than hoped:
       on the rig CSRSS hands back `title=[]` with no program name at all
       (STAGE1: program C:\test\ -- an empty tail), so the override still
       applies there. It stops applying exactly when someone actually asked for
       a program, which is the only case that was ever wrong. */
    /* ── ★★★★★ THE COMMAND CSRSS QUEUED WINS OVER EVERYTHING BELOW. (s72) ───────
         AppName is the program's full path and CmdLine its tail, straight from the
         launcher's CreateProcess -- no title heuristics, no quote-splitting, no join
         with the current directory, and target.txt is not consulted. The title and
         target.txt paths below remain for the shapes where this fetch does not answer
         (the rig harness's dosstub.com + target.txt, where the queued command IS the
         stub and the file names the real target -- kept by a stub-named check). */
    if (g_Fetch2Ok) {
        HANDLE fileHandle = CreateFileA(g_Application2, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        INT isStub = 0;
        {   PCSTR baseName = g_Application2, scan;
            for (scan = g_Application2; *scan; ++scan) if (*scan == '\\' || *scan == '/') baseName = scan + 1;
            isStub   = (lstrcmpiA(baseName, HOST_HARNESS_STUB_NAME) == 0);
            /* ★ OUR OWN LAUNCHER'S STUB -- see LAUNCH_STUB_NAME. Same mechanism, a
                 DIFFERENT meaning: the harness stub says "target.txt names the
                 program", this one says "the user opened NTVDMEX, give them a shell".
                 Keeping them apart is what stops a stale target.txt from hijacking a
                 double-click on someone's machine. */
            if (lstrcmpiA(baseName, LAUNCH_STUB_NAME) == 0) { isStub = 1; wantShell = 1; } }
        if (isStub) {
            if (fileHandle != INVALID_HANDLE_VALUE) CloseHandle(fileHandle);
            cursor = LogPut(cursor, wantShell
                     ? "STAGE2: launched with no program -- the user opened NTVDMEX; going straight to a SHELL\r\n"
                     : "STAGE2: CSRSS queued dosstub.com -- the harness stub; target.txt names the program\r\n");
            g_Title[0] = 0;                              /* and the title must not either */
        } else if (fileHandle != INVALID_HANDLE_VALUE) {
            ReadFile(fileHandle, g_FileBuffer, sizeof(g_FileBuffer), &readCount, NULL); CloseHandle(fileHandle);
            LogPut(programPathBuffer, g_Application2);
            LogPut(args, g_CommandLine2);
            LogPut(g_WowCommandProgram, g_Application2); WowShorten(g_WowCommandProgram, sizeof g_WowCommandProgram);
            LogPut(g_WowCommandArguments, g_CommandLine2);
            cursor = LogPut(cursor, "STAGE2: loaded 0x"); cursor = LogHex(cursor, readCount);
            cursor = LogPut(cursor, " from "); cursor = LogPut(cursor, g_Application2);
            cursor = LogPut(cursor, " args=["); cursor = LogPut(cursor, args); cursor = LogPut(cursor, "] (CSRSS AppName + CmdLine; target.txt and the title NOT consulted)\r\n");
            g_Title[0] = 0;                              /* the title chain below must not re-load */
        } else {
            cursor = LogPut(cursor, "STAGE2: CSRSS AppName ["); cursor = LogPut(cursor, g_Application2);
            cursor = LogPut(cursor, "] cannot be opened (0x"); cursor = LogHex(cursor, GetLastError());
            cursor = LogPut(cursor, ") -- falling back to the title / target.txt\r\n");
        }
    }
    *cursorIo = cursor; *wantShellIo = wantShell; *readCountIo = readCount;
}


/* CSRSS handed us a command: record it, report the path we will actually use, and set up stdio from
   the handles it came with. */
static VOID StartupTakeCsrssCommand(PSTR *cursorIo, DWORD *errorIo)
{
    PSTR cursor = *cursorIo;
    DWORD error = *errorIo;
    if (CsrssGetCommand(&g_CommandInfo, &error)) {
        /* ⚠ PRINT THE PATH WE WILL ACTUALLY USE. This joined the directory and
             the title unconditionally and so reported `C:\test\C:\test\hello.com`
             for a title that was already absolute -- a path that cannot exist,
             in the one line a reader checks first. An instrument that composes a
             string the loader does not use is an instrument that lies. */
        cursor = LogPut(cursor, "STAGE1: program ");
        if (g_Title[0] == '\\' || (g_Title[1] == ':' && g_Title[2] == '\\'))
            cursor = LogPut(cursor, g_Title);
        else { cursor = LogPut(cursor, g_CurrentDirectory); cursor = LogPut(cursor, HOST_PATH_SEPARATOR); cursor = LogPut(cursor, g_Title); } cursor = LogPut(cursor, "\r\n");
        /* #129: the OTHER half of the launch shape. CSRSS hands back the app name,
           the command tail, the PIF and a set of flags -- any of which may be what
           actually distinguishes a WOW launch from a DOS one. Print them all rather
           than guessing which one matters; a trace that prints the request but not
           the answer is half an instrument. */
        cursor = LogPut(cursor, "STAGE1: vdm app=[");   cursor = LogPut(cursor, g_Application);
        cursor = LogPut(cursor, "] cmd=[");             cursor = LogPut(cursor, g_CommandLine);
        cursor = LogPut(cursor, "] pif=[");             cursor = LogPut(cursor, g_PifPath);
        cursor = LogPut(cursor, "] title=[");           cursor = LogPut(cursor, g_Title);
        cursor = LogPut(cursor, "]\r\n");
        cursor = LogPut(cursor, "STAGE1: vdm flags=0x");   cursor = LogHex(cursor, g_CommandInfo.CreationFlags);
        cursor = LogPut(cursor, " state=0x");              cursor = LogHex(cursor, g_CommandInfo.VDMState);
        cursor = LogPut(cursor, " taskid=0x");             cursor = LogHex(cursor, g_CommandInfo.TaskId);
        cursor = LogPut(cursor, " codepage=0x");           cursor = LogHex(cursor, g_CommandInfo.CodePage);
        cursor = LogPut(cursor, "\r\n");
        /* ── ★ THE VDM'S STANDARD HANDLES COME FROM CSRSS, NOT FROM INHERITANCE.
             (GH #131) Measured on the rig: an IFEO-substituted process gets NO
             inherited handles at all -- GetStdHandle reports FILE_TYPE_UNKNOWN
             and AttachConsole(ATTACH_PARENT_PROCESS) fails -- so the obvious
             route ("we are cmd's child, use its stdout") simply does not work
             and reported `none (no console, no redirect)`.
             CSRSS hands them over here instead, in the STARTUPINFO it fills in
             for GetNextVDMCommand, already duplicated into this process. That is
             how stock ntvdm gets them, and it is the only channel that carries
             a redirect the user typed at cmd. */
        /* ► PRINT THE RAW HANDLES AND THEIR TYPES **BEFORE** DECIDING ANYTHING.
             Four routes have been eliminated here already, and each cost a run
             because the log said which route was CHOSEN and never what the
             candidates actually WERE. A handle value with a file type beside it
             settles "is there a redirect on this VDM at all" in one line, for
             every one of the three streams, whether or not we end up using it. */
        {   PCSTR handleNames[STD_HANDLE_REPORTS]; HANDLE handles[STD_HANDLE_REPORTS]; INT item;
            handleNames[0] = "vdm.StdIn";   handles[0] = g_CommandInfo.StdIn;
            handleNames[1] = "vdm.StdOut";  handles[1] = g_CommandInfo.StdOut;
            handleNames[2] = "vdm.StdErr";  handles[2] = g_CommandInfo.StdErr;
            handleNames[3] = "si.hStdOut";  handles[3] = g_CommandInfo.StartupInfo.hStdOutput;
            handleNames[4] = "si.hStdIn";   handles[4] = g_CommandInfo.StartupInfo.hStdInput;
            cursor = LogPut(cursor, "STAGE1: vdm handles");
            for (item = 0; item < STD_HANDLE_REPORTS; ++item) {
                DWORD fileType = (handles[item] && handles[item] != INVALID_HANDLE_VALUE)
                            ? GetFileType(handles[item]) : FILE_TYPE_NOT_ASKED_U;
                cursor = LogPut(cursor, " "); cursor = LogPut(cursor, handleNames[item]);
                cursor = LogPut(cursor, "=0x"); cursor = LogHex(cursor, (DWORD)(ULONG_PTR)handles[item]);
                cursor = LogPut(cursor, "/t"); cursor = LogHex(cursor, fileType);
            }
            cursor = LogPut(cursor, " sf=0x"); cursor = LogHex(cursor, g_CommandInfo.StartupInfo.dwFlags);
            cursor = LogPut(cursor, "\r\n"); }
        g_StdioHow = StdioInitializeVdm();
        cursor = LogPut(cursor, "STAGE1: stdout -> ");
        if (g_StdioSource[0]) { cursor = LogPut(cursor, g_StdioSource); cursor = LogPut(cursor, " -> "); }
        cursor = LogPut(cursor, g_StdioHow);
        cursor = LogPut(cursor, "\r\n");
    } else {
        cursor = LogPut(cursor, "STAGE1: GetNextVDMCommand FALSE err=0x"); cursor = LogHex(cursor, error); cursor = LogPut(cursor, "\r\n");
    }
    *cursorIo = cursor; *errorIo = error;
}


/* GH #128, on a Win16 launch: log the TIB the kernel handed us, then probe the LDT and the selectors. */
static VOID StartupWowSelectorStage(VOID)
{
    if (g_WowModuleCount) {                        /* GH #128: WOW selector stage */
        /* ⛔ NO FLUSH HERE, AND NEVER THROUGH `base`. (s73) This read
             `LogAppend(LOG_PATH, base, p); p = base;` from e595c91 (s68), which turned
             every `report` flush into a `base` flush mechanically -- right for the exit
             report, where base marks the end of the preamble, and WRONG here, 1,500
             lines before `base = p` is ever executed. So: LogAppend(NULL, p) -- a bad
             range -- then p = NULL, then every STAGE1 line was zput FROM ADDRESS 0, which
             in a VDM process is the guest's IVT and BDA. krnl386 then ran on a trashed
             interrupt table and the VDM died silently at PMHB 0x85. Win16 was dead from
             s68 to s73 and nothing noticed: no Win16 program was launched after the wipe.
           There is nothing to flush anyway -- the probes below log through LDTLOG_PATH,
             and report[] has ~5 KB of headroom at this point; the preamble goes to disk
             as one piece at the LogWrite after the command fetch. */
        /* The DOS bisection puts the flip between CsrssGetCommand() and
           VdmGetTib(). The latter is one call and costs nothing to try here, so
           try it BEFORE concluding the blocker is the command fetch. */
        {   PVOID tib2 = VdmGetTib();
            CHAR tibLine[120], *tibCursor = tibLine;
            tibCursor = LogPut(tibCursor, "WOWTRY: v86_get_tib -> 0x"); tibCursor = LogHex(tibCursor, (DWORD)(ULONG_PTR)tib2);
            tibCursor = LogPut(tibCursor, "\r\n"); LogAppend(LDTLOG_PATH, tibLine, tibCursor); }
        WowProbeLdtMatrix("wow-after-get-tib");
        WowProbeSelectors();
        /* ⚠ NO EARLY RETURN ANY MORE. This used to `return WowRefuse(...)` here,
             which was correct while the plan was "enter in protected mode" -- there was
             nothing further to do. It is not, krnl386 is entered in V86, and a V86
             entry needs the whole DOS machine underneath it: conventional memory, an
             INT 21h that answers AH=52h, the IVT, INT 2Fh. All of that is built a few
             hundred lines below. So fall through and let it be built.
           Everything WOW-specific past this point is gated on g_WowModuleCount, which is 0
           on a DOS launch -- so the DOS path, which is the half that WORKS, sees no
           change at all. That gating is deliberate and worth preserving. */
    }
}


/* Give the VDM state word at [0x714] a defined starting value -- zero, or cfg\vdmstate.txt -- instead of
   whatever the machine's boot left there, and report what was inherited. */
static PSTR StartupCheckInheritedVdmState(PSTR cursor)
{
    /* ── ⛔⛔⛔ FIXED_NTVDMSTATE ([0x714]) IS INHERITED GARBAGE UNTIL SOMEONE WRITES IT.
         (2026-09-12: "no DOS app runs on the rig" after a REBOOT, host gone in <1 s.)
         VdmInitialize does not define this word; it holds whatever the machine's
         real-mode boot left in physical low memory, so it changes PER BOOT. Since the
         Sep 9 boot it read 0xc0003232 and everything worked; after this morning's
         reboot it read 0xc0002979 -- bit 0 (VDM_INT_HARDWARE pending) SET -- and the
         kernel dutifully raised VIP in the guest's very first EFLAGS (0x00130002).
         The first STI with VIP set is a raw #GP, and XP tears the VDM down silently
         (run 71 watched it under a kernel debugger; see DpmiAsyncInjectPm). So the
         host died after its first IRQ0 check on EVERY launch, the three-strikes
         counter then removed the IFEO key, and every later launch was stock ntvdm.
       ► Stock ntvdm's live dump (build/stockdumps/130913, +0x714) reads 0x00300200:
         no 0xc000 high bits, nothing pending -- it starts from a DEFINED word. So do
         we, now: zero, or cfg\vdmstate.txt. The kernel sets bits 0-1 when it queues
         and dpmi_enter.S sets bit 9 on PM entry; nothing else needs to be pre-set. */
    {   volatile DWORD *vdmState = (volatile DWORD *)(ULONG_PTR)FIXED_NTVDMSTATE_LINEAR;
        DWORD inherited = *vdmState, want = 0;
        HANDLE handle = CreateFileA(VDMSTATE_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, 0, NULL);
        if (handle != INVALID_HANDLE_VALUE) {
            CHAR text[9]; DWORD bytesRead = 0; INT index;
            ReadFile(handle, text, DWORD_HEX_DIGITS, &bytesRead, NULL);
            CloseHandle(handle);
            for (index = 0; index < (INT)bytesRead; ++index) {
                INT digit = -1;
                if (text[index] >= '0' && text[index] <= '9') digit = text[index] - '0';
                else if (text[index] >= 'a' && text[index] <= 'f') digit = text[index] - 'a' + HEX_DIGIT_A_VALUE;
                else if (text[index] >= 'A' && text[index] <= 'F') digit = text[index] - 'A' + HEX_DIGIT_A_VALUE;
                if (digit < 0) break;
                want = (want << NIBBLE_SHIFT) | (DWORD)digit;
            }
        }
        *vdmState = want;
        cursor = LogPut(cursor, "STAGE1: FIXED_NTVDMSTATE [0x714] inherited=0x"); cursor = LogHex(cursor, inherited);
        cursor = LogPut(cursor, " -> set 0x"); cursor = LogHex(cursor, want);
        cursor = LogPut(cursor, handle != INVALID_HANDLE_VALUE ? " (cfg\\vdmstate.txt)\r\n" : "\r\n");
    }
    return cursor;
}


/* cfg\qimode.txt: up to two hex digits of interrupt-delivery switches -- the pending bit, raise, VIF,
   keyboard, no-suspend and async keyboard (QIMODE_*). */
static VOID StartupLoadQiMode(VOID)
{
    /* Async-preemption mode (session 11). Read once; a handle to THIS thread is what
       VdmQueueInterrupt takes, and this thread is the one that will be running the
       guest inside VdmStartExecution -- so duplicate it here, before the exec loop. */
    { HANDLE handle = CreateFileA(QIMODE_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             NULL, OPEN_EXISTING, 0, NULL);
      if (handle != INVALID_HANDLE_VALUE) {
          CHAR text[2] = { 0, 0 }; DWORD bytesRead = 0; INT number = 0, index;
          ReadFile(handle, text, QIMODE_DIGITS, &bytesRead, NULL);
          CloseHandle(handle);
          for (index = 0; index < (INT)bytesRead; ++index) {          /* up to two hex digits */
              INT digit = -1;
              if (text[index] >= '0' && text[index] <= '9') digit = text[index] - '0';
              else if (text[index] >= 'a' && text[index] <= 'f') digit = text[index] - 'a' + HEX_DIGIT_A_VALUE;
              else if (text[index] >= 'A' && text[index] <= 'F') digit = text[index] - 'A' + HEX_DIGIT_A_VALUE;
              if (digit < 0) break;
              number = (number << NIBBLE_SHIFT) | digit;
          }
          if (number > 0) { g_QiBits  = (DWORD)number & VDM_INT_PENDING;
                       g_QiRaise = (number & QIMODE_RAISE) != 0;
                       g_QiVif   = (number & QIMODE_VIF) != 0;
                       if (number & QIMODE_NO_SUSPEND) g_QiSuspended = 0;      /* bit 6 disables async delivery */
                       g_QiKeys  = (number & QIMODE_KEYS) != 0;
                       g_QiKeysAsync = (number & QIMODE_KEYS_ASYNC) != 0; }
      } }
}


/* A Win16 launch is latched here, where the answer is known, before the UI thread that decides on a window
   starts. It logs the STAGE0 lines and is refused through WowRefuse -- unless wowtry.flag asks for the
   WOW probe instead. */
static INT StartupLatchWowLaunch(PSTR *cursorIo, CHAR *report, INT *exitCodeOut)
{
    PSTR cursor = *cursorIo;
    /* ── IS THIS A WIN16 (WOW) LAUNCH? IF SO, HAND IT STRAIGHT BACK. (GH #129) ──
         Windows runs 16-bit WINDOWS programs inside the SAME ntvdm.exe it uses for
         DOS, so our IFEO Debugger hook catches both -- and we implement only the DOS
         half. Left unhandled, installing NTVDMEX breaks every Win16 program on the
         machine. This is the guard that makes "leave it installed" safe.
       ► MEASURED, not assumed (both captured on the rig, 2026-08-26):
           DOS : ntvdmhost.exe "…\ntvdm.exe" -f -i20
           WOW : ntvdmhost.exe "…\ntvdm.exe" -f -i1 -w -a …\krnl386.exe
         `-w` is the discriminator and `-a <krnl386>` is the WOW bootstrap. A second,
         independent tell: GetNextVDMCommand returns FALSE err=0x57 on a WOW launch,
         because a WOW VDM does not receive its program that way.
       ► WE CANNOT HAND IT BACK. Three routes measured and eliminated -- see
         WowRefuse() below. So this refuses loudly instead, which is at least an
         accurate, actionable failure rather than a DOS host chewing on an NE file.
       ► NOT a throwaway. When the WOW epic (#128) lands, this same detection becomes
         the dispatch point -- the `-w` arm routes to our WOW layer. */
    if (LaunchIsWow(GetCommandLineA())) {
        /* ★ Latched HERE because this is where the answer is known, and the UI
             thread -- which decides whether to show a window -- is started later.
             See the note by TrayAdd for why a Win16 guest gets no window. */
        g_WowLaunch = 1;
        cursor = LogPut(cursor, "STAGE0: WIN16/WOW launch detected -> refusing (see GH #129)\r\n");
        cursor = LogPut(cursor, "STAGE0: root=["); cursor = LogPut(cursor, NTVDMEX_DIR); cursor = LogPut(cursor, "] (derived from the host's own path)\r\n");
        HmaTry();
        cursor = LogPut(cursor, "STAGE0: HMA ");
        if (g_Hma) cursor = LogPut(cursor, "committed at 0x100000 -- FFFF:0010 is real");
        else { cursor = LogPut(cursor, "UNAVAILABLE err=0x"); cursor = LogHex(cursor, g_HmaError);
       cursor = LogPut(cursor, " state=0x"); cursor = LogHex(cursor, g_HmaState);
       cursor = LogPut(cursor, " prot=0x");  cursor = LogHex(cursor, g_HmaProtection); }
        cursor = LogPut(cursor, "\r\n");
        cursor = LogPut(cursor, "STAGE0: cmdline=["); cursor = LogPut(cursor, GetCommandLineA()); cursor = LogPut(cursor, "]\r\n");
        LogAppend(LOG_PATH, report, cursor); SerialOut(report, cursor);
        if (GetFileAttributesA(WOWTRY_FLAG) == INVALID_FILE_ATTRIBUTES)
            { *cursorIo = cursor; *exitCodeOut = WowRefuse(GetCommandLineA()); return HOST_FLOW_RETURN; }
        /* Experiment opted in: load now, then fall through so the selector stage can
           run once the VDM is registered. Still refuses at the end -- nothing here
           executes guest code yet. */
        WowProbeLoad(GetCommandLineA());
    }
    *cursorIo = cursor; return HOST_FLOW_NEXT;
}


/* Claim an instance slot (#211): the first host writes to debug\out\, the Nth to debug\out\N\; refuse once all are taken. */
static INT StartupClaimInstance(INT *exitCodeOut)
{
    /* ── ⛔⛔ ONE HOST AT A TIME. ─────────────────────────────────────────────────
         Nothing stopped a second instance, and two of them fight over things that
         are SYSTEM-WIDE, not per-process: the low-level keyboard hook, the cursor
         clip, exclusive-fullscreen DirectDraw, and the guest's suspend count. The
         user hit it directly -- "I closed your Skyroads run, and reran it. That
         basically crashed Windows" -- because closing does not necessarily finish
         (a suspended guest thread keeps the process alive; see HostPanicRelease),
         so the rerun landed ON TOP of a zombie that still owned the keyboard.
       ⚠ Bail SILENTLY and with success. This is launched by the IFEO Debugger key
         on every 16-bit start, so a message box here would be a modal dialog on a
         machine that is already confused -- and a failure exit code would make the
         launch look broken rather than declined. */
    /* ⚠⚠ GetLastError() IS ONLY MEANINGFUL IMMEDIATELY AFTER THE CALL, AND THE FIRST
         CUT OF THIS GUARD REFUSED EVERY LAUNCH. CreateDirectoryA above fails with
         ERROR_ALREADY_EXISTS whenever cfg\ exists -- i.e. always, after the first run
         -- and CreateMutexA does NOT clear the last-error value when it succeeds. So
         the guard read the DIRECTORY's error, concluded another host was running, and
         exited: the rig went silent, no log at all, and the run timed out.
         Clear it first and latch it immediately. Same do-nothing-and-look-fine shape
         as everything else this file warns about. */
    /* ⚠ AND A HELD MUTEX IS NOT PROOF A HOST IS ALIVE. (s63) The value of the guard
         is entirely in NOT refusing a launch when the "other" instance is a corpse.
         The named object exists as long as ANY handle to it is open -- including one
         held by a wedged zombie -- so ERROR_ALREADY_EXISTS on its own said "refuse"
         even when the previous host had left the machine (that is the flash-and-vanish
         the user hit). So do not decide on the flag: TRY TO ACQUIRE it. A host that is
         really running owns it (created with bInitialOwner) and the wait TIMES OUT ->
         refuse, correctly, one-host-at-a-time. If the owner released it or its thread
         died the wait returns signalled or ABANDONED -> we take it and run. The
         window-close path now TerminateProcess()es (see UiThread), so the common
         zombie is gone at the source; this makes the guard safe against any that slip
         through rather than turning them into a permanent lockout. */
    /* ── #211: MORE THAN ONE HOST AT A TIME. (s81) ───────────────────────────────
         Stock XP runs one ntvdm.exe per DOS window, and so do we now. The mutex below
         was a one-host-only guard (s63); it is now a CLAIM ON AN INSTANCE NUMBER. The
         first host takes instance 1 under the original name and changes nothing --
         same log, same debug\out\, so the rig harness is untouched. A host started
         while it runs takes the lowest free number N and writes to debug\out\N\.
       ► What the old guard protected is covered elsewhere now: the keyboard hook and
         the cursor clip are held only while a window has captured the mouse (and only
         one can), and a closed window's process is terminated, so the half-dead host
         that "basically crashed Windows" no longer outlives its window.
       ⚠ The acquire rule is unchanged, per name: TRY to take it. A live host never
         yields its mutex (the wait times out -> next number); a dead one's is released
         or ABANDONED (-> we take that number). */
    {   INT attempt; CHAR name[48];
        for (attempt = 1; attempt <= HOST_INSTANCES_MAX && !g_OnceMutex; ++attempt) {
            HANDLE once; DWORD lastError, waitResult = WAIT_OBJECT_0;
            PSTR scan = LogPut(name, HOST_INSTANCE_MUTEX);
            if (attempt > 1) { *scan++ = '_'; scan = LogDecimal(scan, (UINT)attempt); }
            /* ⚠⚠ GetLastError() IS ONLY MEANINGFUL IMMEDIATELY AFTER THE CALL -- clear
                 it first; CreateMutexA does not clear it on success. (The first cut of
                 the guard read CreateDirectoryA's ERROR_ALREADY_EXISTS and refused.) */
            SetLastError(0);
            once = CreateMutexA(NULL, TRUE, name);
            lastError  = GetLastError();
            if (!once) continue;
            if (lastError == ERROR_ALREADY_EXISTS) {
                waitResult = WaitForSingleObject(once, HOST_INSTANCE_WAIT_MS);   /* brief: a live host never yields it */
                if (waitResult == WAIT_TIMEOUT) { CloseHandle(once); continue; }
            }
            g_OnceMutex = once;
            if (attempt > 1) {
                PSTR position = LogPut(g_OutSubdirectory, HOST_OUT_SUBDIRECTORY);
                position = LogDecimal(position, (UINT)attempt); LogPut(position, HOST_PATH_SEPARATOR);
                CreateDirectoryA(NTVDMEX_OUT, NULL);
            }
            g_Instance = attempt; g_InstanceAbandoned = (waitResult == WAIT_ABANDONED);   /* reported at STAGE0 */
        }
        if (!g_OnceMutex) {
            static const CHAR message[] = "REFUSED: 16 NTVDMEX hosts are already running -- this instance is exiting\r\n";
            LogAppend(LOG_PATH, message, message + sizeof(message) - 1);
            { *exitCodeOut = 0; return HOST_FLOW_RETURN; }
        } }
    return HOST_FLOW_NEXT;
}


/* The install verbs (/install, /uninstall, /status), run before anything else exists, and exit. */
static INT StartupRunInstallVerb(INT *exitCodeOut)
{
    /* ── ★ THE INSTALL VERBS, BEFORE ANYTHING ELSE EXISTS. (GH #13) ─────────────
         `ntvdmhost.exe /install`, `/uninstall`, `/status`. They run and exit without
         touching the VDM, the log, COM1 or the recovery counter -- none of which
         should move because somebody asked whether we are installed.
       ⛔⛔ AND THIS BLOCK USED TO SIT BELOW THE SINGLE-INSTANCE GUARD, WHICH MADE
         `install.bat` LIE. (2026-09-22) The guard returns 0 -- success, silently, no
         output -- when another host owns the mutex. A verb arriving while ANY guest
         was on screen therefore printed NOTHING and exited 0, and install.bat, which
         branches on the exit code alone, announced "Installed. Every MS-DOS and
         16-bit Windows program now runs under NTVDMEX" having written nothing to the
         registry at all. smoke.bat's `/status` gate passed for the same reason and
         then failed with "no log was written" -- the exact report from the user's
         Windows 2000 box ("installed, apparently; smoke does not run; no logs").
         A verb is a command-line utility invocation, not a VDM launch: it must never
         be subject to a guard about how many VDMs are running.
       ⚠ THE VERB MUST BE THE FIRST ARGUMENT, and that is what makes this safe to put
         ahead of every other launch shape. Windows hands an IFEO-substituted VDM the
         ORIGINAL command line, whose first argument is always the path to ntvdm.exe,
         so a real VDM launch can never look like a verb -- while a token matched
         anywhere on the line could be, one day, a DOS program's argument.
       ⚠ Output goes to stdout when there is one and a message box when there is not,
         because this is the one part of the host that is run BOTH from a prompt and
         by double-clicking. Reporting into a console nobody can see is how an
         installer becomes "it did nothing". */
    {   CHAR installStatus[2048];
        INT verb = InstallVerb(GetCommandLineA());
        if (verb > INSTALL_VERB_NONE) {
            /* ⚠ `verb == 0`, NOT `verb != 2`. The first cut wrote the latter, which
                 makes INSTALL and UNINSTALL both ask to be installed -- and it
                 reported "INSTALLED" cheerfully while doing it, because the message
                 is composed from the same wrong flag. Caught on the rig by the
                 BEHAVIOURAL half of the gate, not by the registry read. */
            INT isOk, want = (verb == INSTALL_VERB_INSTALL);
            installStatus[0] = 0;
            if (verb == INSTALL_VERB_STATUS) {
                /* ── /status ANSWERS IN ITS EXIT CODE, not only in English. ──────
                     0 = NTVDMEX is the machine's VDM, 1 = nobody is, 2 = another
                     program is. A script can branch on that without matching a
                     sentence -- which is exactly what package/smoke.bat was doing
                     wrongly, grepping for text only /install ever prints. */
                INSTALL_STATE installState = InstallStatusText(installStatus, sizeof installStatus);
                InstallReport(installStatus, TRUE);
                { *exitCodeOut = installState == INSTALL_OURS ? INSTALL_STATUS_EXIT_OURS : (installState == INSTALL_OTHER ? INSTALL_STATUS_EXIT_OTHER : INSTALL_STATUS_EXIT_NONE); return HOST_FLOW_RETURN; }
            }
            isOk = InstallPerform(want, CommandLineHasForce(GetCommandLineA()), installStatus, sizeof installStatus);
            InstallReport(installStatus, isOk);
            { *exitCodeOut = isOk ? INSTALL_EXIT_OK : INSTALL_EXIT_FAILED; return HOST_FLOW_RETURN; }
        } }
    return HOST_FLOW_NEXT;
}


/* cfg\target.txt: the harness's way of naming the program -- consulted only when neither CSRSS nor the user (asking for a shell) named one (GH #130). */
static VOID StartupLoadTarget(PSTR *cursorIo, DWORD *readCountIo, const INT wowCommandFromCsrss, const INT wantShell, CHAR *programPathBuffer, CHAR *args)
{
    PSTR cursor = *cursorIo;
    DWORD readCount = *readCountIo;
    {
        INT csrssNamed = (g_CurrentDirectory[0] && g_Title[0]) || (readCount != 0) || wowCommandFromCsrss;
        /* ⚠ `want_shell` skips target.txt ENTIRELY. A user who opened NTVDMEX asked for
             a shell, not for whatever the last test run happened to leave in cfg\. */
        HANDLE thread = (csrssNamed || wantShell)
                  ? INVALID_HANDLE_VALUE
                  : CreateFileA(TARGET_PATH, GENERIC_READ, FILE_SHARE_READ, NULL,
                                OPEN_EXISTING, 0, NULL);
        if (csrssNamed && !readCount) {
            cursor = LogPut(cursor, "STAGE2: CSRSS named a program -- target.txt NOT consulted "
                        "(GH #130)\r\n");
        }
        if (thread != INVALID_HANDLE_VALUE) {
            CHAR tempPath[512]; DWORD tempLength = 0; PSTR scan; PSTR cursorA = 0;
            ReadFile(thread, tempPath, sizeof(tempPath) - 1, &tempLength, NULL); CloseHandle(thread);
            tempPath[tempLength < sizeof(tempPath) ? tempLength : sizeof(tempPath) - 1] = 0;
            /* ── ⚠⚠ A PROGRAM PATH MAY CONTAIN SPACES, AND THIS SPLIT ON THE FIRST ONE.
                 Every path the rig used to hand us was C:\game\X.EXE or C:\test\X.COM,
                 so "first space starts the arguments" was never wrong -- until the rig
                 moved into the share folder, whose path contains "Documents and
                 Settings". Measured, first run after the move:

                   target.txt loaded 0x0 from C:\Documents
                     args=[and Settings\All Users\...\games\Skyroads\Skyroads.EXE]

                 A zero-byte load, then the embedded four-byte `mov ah,4Ch / int 21h`
                 stub runs INSTEAD of the game and the run completes cleanly -- the
                 same silent-success shape as GH #131 below, and it reports `mode
                 sets: none` rather than any kind of error.
               ⇒ So honour QUOTES, and treat an unquoted line as a bare path with no
                 arguments when what it names actually exists. A quoted first token is
                 unambiguous and is what every Windows caller already writes. */
            scan = tempPath;
            if (*scan == '"') {                            /* "path with spaces" [args] */
                PSTR word2 = scan; ++scan;
                while (*scan && *scan != '"') *word2++ = *scan++;
                if (*scan == '"') ++scan;
                *word2 = 0;
                while (*scan == ' ') ++scan;
                { PSTR limit; for (limit = scan; *limit; ++limit) if (*limit == '\r' || *limit == '\n') { *limit = 0; break; } }
                if (*scan) cursorA = scan;
            } else {
                for (scan = tempPath; *scan; ++scan)                 /* trim EOL first */
                    if (*scan == '\r' || *scan == '\n') { *scan = 0; break; }
                /* Unquoted: only split on a space if the WHOLE line is not itself a
                   file. That keeps `C:\test\x.com >out.txt` working and stops a path
                   with spaces being torn in half. */
                { HANDLE probe = CreateFileA(tempPath, GENERIC_READ, FILE_SHARE_READ, NULL,
                                             OPEN_EXISTING, 0, NULL);
                  if (probe != INVALID_HANDLE_VALUE) CloseHandle(probe);
                  else for (scan = tempPath; *scan; ++scan)
                           if (*scan == ' ') { *scan = 0; cursorA = scan + 1; break; } }
            }
            if (tempPath[0]) {
                HANDLE fileHandle = CreateFileA(tempPath, GENERIC_READ, FILE_SHARE_READ, NULL,
                                        OPEN_EXISTING, 0, NULL);
                if (fileHandle != INVALID_HANDLE_VALUE) { ReadFile(fileHandle, g_FileBuffer, sizeof(g_FileBuffer), &readCount, NULL); CloseHandle(fileHandle); }
                LogPut(programPathBuffer, tempPath);                  /* env argv[0] */
                if (cursorA) LogPut(args, cursorA);                   /* PSP command tail */
                /* ★ GH #128: on a WOW launch this same path is the WIN16 program,
                     and WOW32 0x70 is how WOWEXEC asks for it. The DOS image
                     loaded just below is discarded there (see WowPlaceV86), but
                     the NAME is the one thing the WOW path still needs -- Windows
                     does not put it on the VDM's command line. */
                LogPut(g_WowCommandProgram, tempPath); WowShorten(g_WowCommandProgram, sizeof g_WowCommandProgram);
                if (cursorA) LogPut(g_WowCommandArguments, cursorA);
                cursor = LogPut(cursor, "STAGE2: target.txt loaded 0x"); cursor = LogHex(cursor, readCount);
                cursor = LogPut(cursor, " from "); cursor = LogPut(cursor, tempPath);
                if (cursorA && cursorA[0]) { cursor = LogPut(cursor, " args=["); cursor = LogPut(cursor, cursorA); cursor = LogPut(cursor, "]"); }
                cursor = LogPut(cursor, "\r\n");
            }
        }
    }
    *cursorIo = cursor; *readCountIo = readCount;
}


/* The Win16 program, from CSRSS (s73): a WOW launch never carried its program on its command line, so ask CSRSS for it -- the second fetch, which names the program and its arguments. */
static VOID StartupFetchWowCommand(PSTR *cursorIo, INT *wowCommandFromCsrssIo, CHAR *args, CHAR *programPathBuffer, DWORD *readCountIo)
{
    PSTR cursor = *cursorIo;
    INT wowCommandFromCsrss = *wowCommandFromCsrssIo;
    DWORD readCount = *readCountIo;
    /* ── ★★★★ THE WIN16 PROGRAM, FROM CSRSS. (s73) ──────────────────────────
         A WOW launch never carried its program: the VDM starts as
         `ntvdm -f -i<n> -w -a krnl386.exe`, the first fetch above returns FALSE
         err=0x57 (measured, s3x), and until today the name came ONLY from
         cfg\target.txt -- the harness's channel -- so on any machine without
         that file a double-clicked Win16 program ran nothing, and on the rig it
         ran whatever the file happened to name last. Stock WOW gets it exactly
         the way stock DOS does: WOWEXEC's WowGetNextVDMCommand (WOW32 0x70) is
         wow32.dll calling GetNextVDMCommand with VDM_FLAG_WOW, and CSRSS answers
         with the AppName + CmdLine the launcher queued. We ask here, before
         krnl386 runs, so that 0x70 can answer from g_WowCommandProgram as it already
         does. DONT_WAIT: a misunderstanding is a FALSE, never a hang. */
    VDM_COMMAND_INFO commandInfo2;
    DWORD error2 = 0; BOOL ok2; INT item;
    static CHAR pifFile2[512], desktop2[512], title2[512], reservedBuffer2[512];
    ZeroMemory(&commandInfo2, sizeof commandInfo2);
    commandInfo2.CmdLine = g_CommandLine2; commandInfo2.CmdLen = sizeof g_CommandLine2;  commandInfo2.AppName = g_Application2; commandInfo2.AppLen = sizeof g_Application2;
    commandInfo2.PifFile = pifFile2; commandInfo2.PifLen = sizeof pifFile2; commandInfo2.CurDirectory = g_CurrentDirectory2; commandInfo2.CurDirectoryLen = sizeof g_CurrentDirectory2;
    commandInfo2.Env = g_Environment2; commandInfo2.EnvLen = sizeof g_Environment2; commandInfo2.Desktop = desktop2; commandInfo2.DesktopLen = sizeof desktop2;
    commandInfo2.Title = title2; commandInfo2.TitleLen = sizeof title2; commandInfo2.Reserved = reservedBuffer2; commandInfo2.ReservedLen = sizeof reservedBuffer2;
    commandInfo2.StartupInfo.cb = sizeof(STARTUPINFOA);
    /* ► THE SHAPE IS MEASURED, NOT ASSUMED (rig, s73). WOW|FIRST|DONT_WAIT alone
         answers FALSE err=0x490 (ERROR_NOT_FOUND), with either task id. What
         answers TRUE is GET_FIRST_COMMAND|WOW -- the same handshake stock ntvdm's
         cmdGetStartInfo makes for DOS, and like the DOS one it fills Title/CurDir
         and leaves AppName as capture-buffer junk ("[5??]"). So, as on the DOS
         path: the GET_FIRST call first, then the FIRST_TASK fetch for the command
         itself. Every call is DONT_WAIT and every answer is logged; a "name" is
         only believed when it is drive-qualified or UNC -- TRUE with junk is not
         a program. */
    {   static const struct { DWORD VdmState; INT IsOwnTask; INT IsHandshake; PCSTR Description; } vdmStates[] = {
            { VDM_GET_FIRST_COMMAND | VDM_FLAG_WOW | VDM_FLAG_DONT_WAIT, 1, 1, "GET_FIRST|WOW|DONT_WAIT taskid=-i (handshake)" },
            { VDM_FLAG_WOW | VDM_FLAG_FIRST_TASK | VDM_FLAG_DONT_WAIT, 1, 0, "WOW|FIRST|DONT_WAIT taskid=-i" },
            { VDM_FLAG_WOW | VDM_FLAG_FIRST_TASK | VDM_FLAG_DONT_WAIT, 0, 0, "WOW|FIRST|DONT_WAIT taskid=0"  },
            { VDM_FLAG_WOW | VDM_FLAG_DONT_WAIT, 1, 0, "WOW|DONT_WAIT taskid=-i"       },
            { VDM_FLAG_WOW | VDM_FLAG_FIRST_TASK | VDM_FLAG_RETRY | VDM_FLAG_DONT_WAIT, 1, 0, "WOW|FIRST|RETRY|DONT_WAIT taskid=-i" },
        };
        UINT si; INT named = 0;
        for (si = 0; si < sizeof vdmStates / sizeof vdmStates[0] && !named; ++si) {
            g_Application2[0] = 0; g_CommandLine2[0] = 0; g_CurrentDirectory2[0] = 0; error2 = 0;
            commandInfo2.AppLen = sizeof g_Application2; commandInfo2.CmdLen = sizeof g_CommandLine2; commandInfo2.CurDirectoryLen = sizeof g_CurrentDirectory2;
            commandInfo2.EnvLen = sizeof g_Environment2; commandInfo2.PifLen = sizeof pifFile2; commandInfo2.DesktopLen = sizeof desktop2;
            commandInfo2.TitleLen = sizeof title2; commandInfo2.ReservedLen = sizeof reservedBuffer2;
            commandInfo2.VDMState = vdmStates[si].VdmState;
            commandInfo2.TaskId = vdmStates[si].IsOwnTask ? g_CommandInfo.TaskId : 0;
            ok2 = CsrssGetCommand(&commandInfo2, &error2);
            g_Application2[sizeof g_Application2 - 1] = 0; g_CommandLine2[sizeof g_CommandLine2 - 1] = 0; g_CurrentDirectory2[sizeof g_CurrentDirectory2 - 1] = 0;
            named = ok2 && !vdmStates[si].IsHandshake
                    && ((g_Application2[0] >= 'A' && (g_Application2[0] | ASCII_CASE_BIT) <= 'z' && g_Application2[1] == ':' && g_Application2[2] == '\\')
                        || (g_Application2[0] == '\\' && g_Application2[1] == '\\'));
            cursor = LogPut(cursor, "STAGE1: WOW command fetch ["); cursor = LogPut(cursor, vdmStates[si].Description); cursor = LogPut(cursor, "] -> ");
            cursor = LogPut(cursor, ok2 ? "TRUE" : "FALSE");
            cursor = LogPut(cursor, " err=0x"); cursor = LogHex(cursor, error2);
            cursor = LogPut(cursor, " app=["); if (named) cursor = LogPut(cursor, g_Application2); else if (g_Application2[0]) cursor = LogPut(cursor, "<not a path>");
            cursor = LogPut(cursor, "] args=["); if (named) cursor = LogPut(cursor, g_CommandLine2);
            cursor = LogPut(cursor, "] cur=["); cursor = LogPut(cursor, g_CurrentDirectory2); cursor = LogPut(cursor, "] show=0x"); cursor = LogHex(cursor, commandInfo2.StartupInfo.wShowWindow);
            cursor = LogPut(cursor, " taskid=0x"); cursor = LogHex(cursor, commandInfo2.TaskId);
            cursor = LogPut(cursor, "\r\n");
        }
        ok2 = named;
    }
    for (item = 0; g_CommandLine2[item]; ++item) if (g_CommandLine2[item] == '\r' || g_CommandLine2[item] == '\n') { g_CommandLine2[item] = 0; break; }
    wowCommandFromCsrss = ok2 && g_Application2[0];
    if (wowCommandFromCsrss) {
        /* Exactly what the target.txt path does with a name, or the stage below
           builds the V86 world for the embedded four-byte stub instead ("STAGE2:
           embedded fallback"), krnl386 gets no program path in its environment,
           and it dies in its own init: "NTVDM KERNEL: Unable to initialize heap".
           Measured, first cut. The image read here is discarded by WowPlaceV86;
           the NAME and the byte count are what the stage keys on. */
        HANDLE wowHandle;
        LogPut(g_WowCommandProgram, g_Application2); WowShorten(g_WowCommandProgram, sizeof g_WowCommandProgram);
        /* the tail arrives with its leading space, as a DOS tail does; 0x70 adds its own */
        { PCSTR cursorA = g_CommandLine2; while (*cursorA == ' ') ++cursorA; LogPut(g_WowCommandArguments, cursorA); LogPut(args, cursorA); }
        LogPut(programPathBuffer, g_WowCommandProgram);
        wowHandle = CreateFileA(g_WowCommandProgram, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (wowHandle != INVALID_HANDLE_VALUE) { ReadFile(wowHandle, g_FileBuffer, sizeof(g_FileBuffer), &readCount, NULL); CloseHandle(wowHandle); }
        if (g_CurrentDirectory2[0]) {
            SetCurrentDirectoryA(g_CurrentDirectory2);
            /* #164: WOWEXEC changes to this before LoadModule, so the task starts
               in the folder it was launched from -- 8.3, as krnl386 sees paths. */
            if (!GetShortPathNameA(g_CurrentDirectory2, g_WowCommandDirectory, sizeof g_WowCommandDirectory)
                || lstrlenA(g_WowCommandDirectory) >= WOW_COMMAND_DIRECTORY_MAX)
                g_WowCommandDirectory[0] = 0;
        }
        cursor = LogPut(cursor, "STAGE2: Win16 program from CSRSS -- LAUNCH ["); cursor = LogPut(cursor, g_WowCommandProgram);
        cursor = LogPut(cursor, "] loaded 0x"); cursor = LogHex(cursor, readCount); cursor = LogPut(cursor, " (target.txt NOT consulted)\r\n");
        if (!readCount) wowCommandFromCsrss = 0;          /* unreadable: fall back as before */
    }
    *cursorIo = cursor; *wowCommandFromCsrssIo = wowCommandFromCsrss; *readCountIo = readCount;
}



/* The first fetch gave a directory or a title: fetch the command again, in full, for its application name, command line and the rest. */
static PSTR StartupFetchCommandDetails(PSTR cursor)
{
    VDM_COMMAND_INFO commandInfo2;
    DWORD error2 = 0; BOOL ok2; INT item;
    static CHAR pifFile2[512], desktop2[512], title2[512], reservedBuffer2[512];
    ZeroMemory(&commandInfo2, sizeof commandInfo2);
    commandInfo2.CmdLine = g_CommandLine2; commandInfo2.CmdLen = sizeof g_CommandLine2;  commandInfo2.AppName = g_Application2; commandInfo2.AppLen = sizeof g_Application2;
    commandInfo2.PifFile = pifFile2; commandInfo2.PifLen = sizeof pifFile2; commandInfo2.CurDirectory = g_CurrentDirectory2; commandInfo2.CurDirectoryLen = sizeof g_CurrentDirectory2;
    commandInfo2.Env = g_Environment2; commandInfo2.EnvLen = sizeof g_Environment2; commandInfo2.Desktop = desktop2; commandInfo2.DesktopLen = sizeof desktop2;
    commandInfo2.Title = title2; commandInfo2.TitleLen = sizeof title2; commandInfo2.Reserved = reservedBuffer2; commandInfo2.ReservedLen = sizeof reservedBuffer2;
    commandInfo2.StartupInfo.cb = sizeof(STARTUPINFOA);
    commandInfo2.VDMState = VDM_FLAG_DOS | VDM_FLAG_FIRST_TASK | VDM_FLAG_DONT_WAIT;              /* VDM_FLAG_DOS | FIRST_TASK | DONT_WAIT */
    commandInfo2.TaskId = g_CommandInfo.TaskId;
    ok2 = CsrssGetCommand(&commandInfo2, &error2);
    g_Application2[sizeof g_Application2 - 1] = 0; g_CommandLine2[sizeof g_CommandLine2 - 1] = 0; g_CurrentDirectory2[sizeof g_CurrentDirectory2 - 1] = 0;
    /* the tail ends in CR LF; the PSP wants neither */
    for (item = 0; g_CommandLine2[item]; ++item) if (g_CommandLine2[item] == '\r' || g_CommandLine2[item] == '\n') { g_CommandLine2[item] = 0; break; }
    g_Fetch2Ok = ok2 && commandInfo2.AppLen > 1 && g_Application2[0];
    cursor = LogPut(cursor, "STAGE1: command fetch (DOS|FIRST|DONT_WAIT) -> "); cursor = LogPut(cursor, ok2 ? "TRUE" : "FALSE");
    cursor = LogPut(cursor, " err=0x"); cursor = LogHex(cursor, error2);
    cursor = LogPut(cursor, " app=["); cursor = LogPut(cursor, g_Application2); cursor = LogPut(cursor, "] args=["); cursor = LogPut(cursor, g_CommandLine2);
    cursor = LogPut(cursor, "] cur=["); cursor = LogPut(cursor, g_CurrentDirectory2); cursor = LogPut(cursor, "] bat=0x"); cursor = LogHex(cursor, commandInfo2.ComingFromBat);
    cursor = LogPut(cursor, " drive=0x"); cursor = LogHex(cursor, commandInfo2.CurrentDrive); cursor = LogPut(cursor, " envlen=0x"); cursor = LogHex(cursor, commandInfo2.EnvLen);
    cursor = LogPut(cursor, " flags=0x"); cursor = LogHex(cursor, commandInfo2.CreationFlags);
    cursor = LogPut(cursor, " std=0x"); cursor = LogHex(cursor, (DWORD)(ULONG_PTR)commandInfo2.StdIn); cursor = LogPut(cursor, "/0x"); cursor = LogHex(cursor, (DWORD)(ULONG_PTR)commandInfo2.StdOut);
    cursor = LogPut(cursor, "/0x"); cursor = LogHex(cursor, (DWORD)(ULONG_PTR)commandInfo2.StdErr);
    cursor = LogPut(cursor, "\r\n");
    return cursor;
}


/* Get the guest ready to run: hide the inherited console, back Mode Y's planes before the UI thread exists, start the UI thread and the headless and probe threads, set the entry state (krnl386's for a Win16 launch), check the INT 21h stub, report the start, the VDD vector claims, the A0000 region and the bus, and enter the program's directory. */
static VOID StartupStartGuest(PSTR *cursorIo, PSTR *baseIo, HANDLE *uiThreadIo, DOS_MACHINE *machine, DOS_IMAGE *image, volatile BYTE * const tib, CHAR *report)
{
    PSTR cursor = *cursorIo;
    PSTR base = *baseIo;
    HANDLE uiThread = *uiThreadIo;
    /* Hide the inherited console (CSRSS already bound the VDM); the Luna window
       is now the display. Then start the UI thread that owns it. */
    g_KeyEvent = CreateEventA(NULL, FALSE, FALSE, NULL);   /* auto-reset        */
    { HWND consoleWindow = GetConsoleWindow(); if (consoleWindow) ShowWindow(consoleWindow, SW_HIDE); }
    /* ► PER-PLANE BACKING BEFORE THE UI THREAD EXISTS. The remap unmaps the A0000
         window for an instant, and the renderer dereferences it every few milliseconds;
         doing this with that thread already running hung the host so early that no log
         reached disk at all. Its report is buffered and flushed after the preamble. */
    if (GetFileAttributesA(NOREMAP_FLAG) == INVALID_FILE_ATTRIBUTES && ModeYRemapInitialize()) {
        g_Video.YMapContext    = NULL;
        g_Video.YMapSelect = ModeYRemapSelect;
        g_Video.YMapPlane  = ModeYRemapPlane;
        g_Video.YMapWriteMode  = ModeYRemapWriteMode;
        g_Video.YMapReadMap = ModeYRemapReadMap;
    }
    uiThread = CreateThread(NULL, 0, UiThread, NULL, 0, NULL);
    /* Headless: arm the deadline watchdog so a run that blocks on input (a "press any
       key" prompt, a game menu) still self-terminates instead of wedging the harness. */
    if (g_Headless) { HANDLE deadlineThread = CreateThread(NULL, 0, HeadlessDeadlineThread, NULL, 0, NULL);
                      if (deadlineThread) CloseHandle(deadlineThread);
                      /* appended to the preamble, which is flushed further down */
                      cursor = LogPut(cursor, "HEADLESS: cap=0x"); cursor = LogHex(cursor, g_HeadlessMs);
                      cursor = LogPut(cursor, " ms\r\n"); }
    /* cfg\livehb.flag: the heartbeat on a LIVE (by-hand) run too. A host that dies
       with no exit report leaves nothing else that says where the guest was. (s68) */
    if (g_Headless || GetFileAttributesA(LIVEHB_FLAG) != INVALID_FILE_ATTRIBUTES) {
                      HANDLE heartbeatThread = CreateThread(NULL, 0, HeartbeatThread, NULL, 0, NULL);
                      if (heartbeatThread) CloseHandle(heartbeatThread); }
    if (g_QiKeys) { HANDLE keyThread = CreateThread(NULL, 0, SynthKeyThread, NULL, 0, NULL);
                     if (keyThread) CloseHandle(keyThread); }
    if (g_QiRaise) { HANDLE irqThread = CreateThread(NULL, 0, QueueIrqProbeThread, NULL, 0, NULL);
                      if (irqThread) CloseHandle(irqThread); }

    /* ── GH #128: on a WOW launch, the guest is krnl386, not a DOS program. ─────────
         Placed HERE because this is the one point where the DOS machine is fully
         built (conventional memory, INT 21h, the IVT, INT 2Fh) and the guest entry
         has not yet been committed. krnl386 needs all of it: AH=52h almost at
         once, then 2F/1687 to find our DPMI host and switch itself.
         The DOS program load above still ran and is simply discarded -- it is
         tolerant of a missing target and costs one wasted image. Overriding here
         rather than short-circuiting there keeps the DOS path's spine untouched. */
    {   WORD wowCs = 0, wowIp = 0, wowDs = 0, wowSs = 0, wowSp = 0;
        if (g_WowModuleCount && WowPlaceV86(machine, &wowCs, &wowIp, &wowDs, &wowSs, &wowSp) == 0) {
            image->CodeSegment = wowCs; image->InstructionPointer = wowIp; image->StackSegment = wowSs; image->StackPointer = wowSp;
            g_WowEntryDs = wowDs;
            g_WowEntering = 1;
        }
    }
    VdmSetEntry(tib, image->CodeSegment, image->InstructionPointer, image->StackSegment, image->StackPointer, DOS_PSP_SEG);
    cursor = StartupPrepareWowEntry(cursor, tib, image);
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
       sequence regardless) but it is not the missing piece.
     ⚠ s82: THE ORACLES DISAGREE WITH US, AND THIS DOES NOT FIX IT. p_ifst.com asks FLAGS
       at a program's first instruction: MS-DOS 6.22, DOSBox-X and PCem all answer IF=1; we
       answer IF=0, and keep answering it after INT 10h and INT 21h. A program that never
       executes STI (mybench.com) then has every timer tick refused by our own gate, which
       reads that virtual IF, and 0040:006C stands still. Defaulting this trampoline was
       tried: the guest's STI did not stick (first exit still VTIB EFLAGS=0x30002), with or
       without VIP cleared first -- the wall s11 recorded. Still opt-in. See GH issue.
     ✅ #212, s84: FIXED ELSEWHERE, AND THIS WAS NEVER THE PLACE. Every program is started
       by a shell's EXEC, and the EXEC path handed the child the parent's flags from
       INSIDE our INT 21h stub, where IF is already clear -- so the trampoline's STI ran
       for the shell, not the program. ExecBegin() now enters the child with IF set;
       p_ifst agrees with all three oracles. */
    if (g_QiVif) {
        volatile BYTE *trampoline = (volatile BYTE *)(ULONG_PTR)(((DWORD)DOS_HDLR_SEG << PARAGRAPH_SHIFT) + DOS_HDLR_TRAMPOLINE_OFF);
        /* ⚠ s82: 0x60-0x65 is DPMI callback slot 0 and half of slot 1, planted ABOVE
             (dos_layout.h). Keep what was there; the exec loop puts it back at the first
             exit outside the trampoline. Opt-in, this never mattered; default, it would
             send a client's first callback into `sti; jmp far <program entry>`.
           (#248: the callback slots moved to 0x90, so these bytes are now unused; the
             save/restore stays because it costs nothing and keeps the area as found.) */
        { INT item; for (item = 0; item < DOS_HDLR_TRAMPOLINE_SIZE; ++item) g_TrampolineSave[item] = trampoline[item]; g_TrampolineSaved = 1; }
        trampoline[0] = X86_OP_STI;                                   /* sti                       */
        trampoline[1] = X86_OP_JMP_FAR;                                   /* jmp far cs:ip             */
        trampoline[2] = (BYTE)image->InstructionPointer; trampoline[3] = (BYTE)(image->InstructionPointer >> BYTE_SHIFT);
        trampoline[4] = (BYTE)image->CodeSegment; trampoline[5] = (BYTE)(image->CodeSegment >> BYTE_SHIFT);
        VDM_REG(tib, VTIB_CS)  = DOS_HDLR_SEG;
        VDM_REG(tib, VTIB_EIP) = DOS_HDLR_TRAMPOLINE_OFF;
    }
    /* Session 11: the kernel's deliverability test for a V86 frame on a VME CPU follows
       EFLAGS.VIF, not IF (observed: VIP set and delivery deferred). Starting the guest with
       VIF clear makes every hardware interrupt undeliverable from the kernel's point of
       view -- it just sets VIP and defers. Opt-in until the rig confirms it. */
    if (g_QiVif) VDM_REG(tib, VTIB_EFLAGS) |= EFLAGS_VIF;
    if (!image->IsExe)                                        /* .COM near-ret guard */
        *(volatile WORD *)(((DWORD)DOS_PSP_SEG << PARAGRAPH_SHIFT) + DOS_COM_STACK_TOP) = 0;

    /* Every stub the host plants in DOS_HDLR_SEG is planted by a different piece of
       start-up code with its own idea of a free offset. Verify the ones a guest can
       RETURN INTO after all planting is done -- an overwritten stub is a guest jumping
       into another service's BOP, and the symptom (QBasic: "DOS terminate" on the first
       mouse move) names nothing. */
    {   volatile BYTE *handlerSegment = (volatile BYTE *)(DOS_HDLR_SEG << PARAGRAPH_SHIFT);
        if (handlerSegment[MS_CB_RET_OFF] != VDM_BOP0 || handlerSegment[MS_CB_RET_OFF + 1] != VDM_BOP1
            || handlerSegment[MS_CB_RET_OFF + VDM_BOP_NUMBER_OFFSET] != MS_CB_BOP) {
            cursor = LogPut(cursor, "STAGE2: *** STUB OVERWRITTEN at DOS_HDLR_SEG:0x");
            cursor = LogHex(cursor, MS_CB_RET_OFF); cursor = LogPut(cursor, " (mouse callback return): bytes ");
            cursor = LogDump(cursor, (const VOID *)(handlerSegment + MS_CB_RET_OFF), VDM_BOP_STUB_SIZE); cursor = LogPut(cursor, "\r\n");
        }
    }
    cursor = LogPut(cursor, image->IsExe ? "STAGE2: running .EXE (entry 0x"
                           : "STAGE2: running .COM (entry 0x");
    cursor = LogHex(cursor, image->CodeSegment); cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, image->InstructionPointer); cursor = LogPut(cursor, ")...\r\n");
    LogWrite(LOG_PATH, report, cursor);
    base = cursor;                       /* preamble is on disk; the loop appends from here */
    {   /* #211. After the last truncating write, for the same reason as #144 below. */
        CHAR instanceLine[200], *instanceCursor = LogPut(instanceLine, "STAGE2: instance "); instanceCursor = LogDecimal(instanceCursor, (UINT)g_Instance);
        if (g_Instance > 1) { instanceCursor = LogPut(instanceCursor, " (another NTVDMEX was running) -> output in "); instanceCursor = LogPut(instanceCursor, g_OutSubdirectory); }
        if (g_InstanceAbandoned) instanceCursor = LogPut(instanceCursor, " -- prior holder ABANDONED (zombie thread died)");
        instanceCursor = LogPut(instanceCursor, " (#211)\r\n");
        LogAppend(LOG_PATH, instanceLine, instanceCursor); }
    /* #144. HERE, not with the other STAGE0 lines: this is the last truncating write
       (an append before it is wiped -- the first cut of this was), and the ~7 KB table
       would overflow the 8 KB preamble buffer. Every file override has been read. */
    SettingsLogSources();
    /* ── THIRD-PARTY VDDs LOAD HERE, AND THE PLACE IS THE POINT. ─────────────
         Two constraints, and only this line satisfies both:
         (1) AFTER every built-in device is on the bus, so a third-party claim
             that collides with our own video or UART is REFUSED honestly rather
             than silently shadowing it;
         (2) AFTER THE LAST LogWrite, which TRUNCATES. The first cut put this
             beside the built-in devices, ~350 lines up -- the driver may well
             have loaded and every line it logged was erased before anyone could
             read it, which reads exactly like "the code never ran". The warning
             immediately above this one says so in as many words, and I still
             walked into it. Third time this trap has been paid for.
         The guest's image is loaded but not yet running, so claims made here are
         in place before its first instruction. */
    VddLoadThirdParty();
    StartupReportVectorWiring();
    /* ⚠⚠ AFTER THE **LAST** LogWrite. There are THREE of them in WinMain and every one
         TRUNCATES. This probe was placed after the first, then after the second, and
         both times its output was silently erased by the next one -- which reads
         exactly like "the code never ran", and cost three rounds of looking in the
         wrong place. If you add diagnostics to WinMain, append AFTER line ~9941 or put
         them in `p` so a LogWrite carries them. */
    {   DWORD attributes = GetFileAttributesA(WOWTRY_FLAG);
        CHAR wowLine[200], *wowCursor = wowLine;
        wowCursor = LogPut(wowCursor, "LDTARM: wow_mods="); wowCursor = LogHex(wowCursor, (DWORD)g_WowModuleCount);
        wowCursor = LogPut(wowCursor, " flag_attr=0x");      wowCursor = LogHex(wowCursor, attributes);
        wowCursor = LogPut(wowCursor, "\r\n"); LogAppend(LOG_PATH, wowLine, wowCursor);
        if (!g_WowModuleCount && attributes != INVALID_FILE_ATTRIBUTES)
            WowProbeLdtMatrix("dos-late");  /* the other corner of the 2x2 */
    }
    ModeYRemapFlushReport();     /* whatever the A0000 remap had to say, now it fits */
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
    { MEMORY_BASIC_INFORMATION regionInfo;
      if (VirtualQuery((LPCVOID)(ULONG_PTR)VIDEO_APERTURE_BASE, &regionInfo, sizeof regionInfo) == sizeof regionInfo) {
          cursor = LogPut(cursor, "STAGE2: A0000 region: alloc_base=0x");
          cursor = LogHex(cursor, (DWORD)(ULONG_PTR)regionInfo.AllocationBase);
          cursor = LogPut(cursor, " base=0x");   cursor = LogHex(cursor, (DWORD)(ULONG_PTR)regionInfo.BaseAddress);
          cursor = LogPut(cursor, " size=0x");   cursor = LogHex(cursor, (DWORD)regionInfo.RegionSize);
          cursor = LogPut(cursor, " state=0x");  cursor = LogHex(cursor, regionInfo.State);
          cursor = LogPut(cursor, " type=0x");   cursor = LogHex(cursor, regionInfo.Type);
          cursor = LogPut(cursor, " prot=0x");   cursor = LogHex(cursor, regionInfo.Protect);
          cursor = LogPut(cursor, " allocprot=0x"); cursor = LogHex(cursor, regionInfo.AllocationProtect);
          cursor = LogPut(cursor, (regionInfo.AllocationBase == (LPVOID)(ULONG_PTR)VIDEO_APERTURE_BASE)
                        ? "  -> OWN ALLOCATION: remappable\r\n"
                        : "  -> inside a larger reservation: NOT remappable in place\r\n");
          LogAppend(LOG_PATH, base, cursor); cursor = base;
      } }
    { CHAR busLine[160], *busCursor = busLine;
      busCursor = LogPut(busCursor, g_Bus.ClaimFailures ? "STAGE2: *** BUS CLAIMS REFUSED: " : "STAGE2: bus ok: ");
      busCursor = LogHex(busCursor, (DWORD)g_Bus.ClaimFailures);
      busCursor = LogPut(busCursor, " refused, ports="); busCursor = LogHex(busCursor, (DWORD)g_Bus.PortCount);
      busCursor = LogPut(busCursor, "/"); busCursor = LogHex(busCursor, (DWORD)VDD_MAX_PORT_RANGES);
      busCursor = LogPut(busCursor, " mem="); busCursor = LogHex(busCursor, (DWORD)g_Bus.MemoryCount);
      busCursor = LogPut(busCursor, "/"); busCursor = LogHex(busCursor, (DWORD)VDD_MAX_MEMORY_WINDOWS);
      busCursor = LogPut(busCursor, " dev="); busCursor = LogHex(busCursor, (DWORD)g_Bus.DeviceCount);
      busCursor = LogPut(busCursor, "/"); busCursor = LogHex(busCursor, (DWORD)VDD_MAX_DEVICES);
      busCursor = LogPut(busCursor, "\r\n"); LogAppend(LOG_PATH, busLine, busCursor); SerialOut(busLine, busCursor); }

    SetCurrentDirectoryA(g_CurrentDirectory);    /* DOS relative paths resolve against CurDir */
    *cursorIo = cursor; *baseIo = base; *uiThreadIo = uiThread;
}


/* Connect DOS to the host: console output to the video VDD, console input from the keyboard, the tick and printer hooks, and the DOS-trace and simulated-interrupt switches. */
static VOID StartupConnectDosToHost(DOS_MACHINE *machine)
{
    machine->ConsoleOut = HostConsoleOut; machine->ConsoleOutContext = NULL;    /* DOS console out -> video      */
    machine->ConsoleIn  = HostConsoleIn;  machine->ConsoleInContext = NULL;    /* DOS console in  <- keyboard   */
    /* Full INT 21h call trace, opt-in per run: it is a differential instrument, not a
       default. See the trace at the top of DosInt21(). */
    machine->IsTraceAll = (GetFileAttributesA(DOSTRACE_FLAG) != INVALID_FILE_ATTRIBUTES);
    /* DPMI 0300 reflects to the guest's own real-mode handler -- ON by default since s81,
       when the wedge that kept it off was found and fixed (see g_NestedRm and the BIOS
       tick in AsyncInjectIrq). simintrefl_off.flag turns it off for diagnosis; say so,
       because a guest-owned vector then silently does nothing again. */
    g_SimIntReflect = (GetFileAttributesA(SIMINTREFL_OFF_FLAG) == INVALID_FILE_ATTRIBUTES);
    if (!g_SimIntReflect) {
        CHAR stageLine[224], *stageCursor = stageLine;
        stageCursor = LogPut(stageCursor, "STAGE1: simintrefl_off.flag -- DPMI 0300 is the pre-#247 shape: INT 21h/"
                      "33h/10h host-side, EVERY other vector NOT RUN (ZAR: silent). See "
                      "simint_route().\r\n");
        LogAppend(LOG_PATH, stageLine, stageCursor); SerialOut(stageLine, stageCursor);
    }
    machine->ConsoleInNoWait = HostConsoleInNoBlock;                   /* AH=06 DL=FF non-blocking read */
    machine->ConsolePeek = HostConsolePeek;                   /* AH=0B/06 non-blocking status  */
    machine->SetTicks = HostSetTicks;               /* AH=2Dh reloads 0040:006C (#250) */
    machine->TicksContext = NULL;
    machine->PrinterOut  = DosPrnOut;                     /* #251: PRN/AUX when not in V86 */
    machine->AuxOut  = DosAuxOut;  machine->DeviceContext = NULL;
}


/* Start the host's services: the PIT pacer (or the tick it replaces), the capture watchdog, the CPU-speed governor, the audio output and its MIDI route, and wavrec.flag's recording. */
static PSTR StartupStartServices(PSTR cursor)
{
    if (g_PitPaceOn) {
        HMODULE winmmModule = LoadLibraryA(HOST_MODULE_WINMM);
        if (winmmModule) { PFN_TIME_BEGIN_PERIOD beginPeriod =
                      (PFN_TIME_BEGIN_PERIOD)GetProcAddress(winmmModule, HOST_EXPORT_TIME_BEGIN_PERIOD);
                  if (beginPeriod) beginPeriod(1); }
        PitPacerTimerStart(winmmModule);                /* #238: a true 1 ms wake */
        g_PitPaceThread = CreateThread(NULL, 0, PitPacerThread, NULL, 0, NULL);
    }
    /* The capture watchdog runs for every guest, throttled or not, headless or not:
       it is the only thing standing between a wedge and a hard reset. */
    { HANDLE captureWatchdog = CreateThread(NULL, 0, CaptureWatchdogThread, NULL, 0, NULL);
      if (captureWatchdog) CloseHandle(captureWatchdog); }
    /* ── ★ THE TICK COURIER. Auto-reset: one signal wakes exactly one pass, and a
         signal arriving while it is already awake is not lost -- the pass re-checks
         g_Irq0Pending anyway. Created even when the courier is knobbed off, so the
         raise site's SetEvent never has to test two things. */
    if (g_QiSuspended) {
        g_CourierEvent = CreateEventA(NULL, FALSE, FALSE, NULL);
        if (g_CourierEvent)
            g_CourierThread = CreateThread(NULL, 0, TickCourierThread, NULL, 0, NULL);
    }
    /* ── ★ THE CPU-SPEED THROTTLE. (GH #56) ──────────────────────────────────────
         Started unconditionally, even at Unlimited: the setting is live, and a
         thread that has to be created before it can bite would make the menu work
         only on machines that started throttled. It idles in 4 ms sleeps when
         there is nothing to do.
       ⚠ IT NEEDS THE 1 ms TIMER RESOLUTION, and the block above only raises it when
         the PIT pacer is on. Without timeBeginPeriod(1) a Sleep(1) is XP's default
         ~15.6 ms, which would turn every held millisecond into fifteen and make the
         guest fifteen times slower than the label on the menu. So raise it here too
         -- the call nests, and pitpace=0 must not silently change what "33 MHz"
         means. Bound by name, like every other winmm use, so the import allowlist
         is unaffected. */
    if (!g_PitPaceOn) {
        HMODULE winmmModule2 = LoadLibraryA(HOST_MODULE_WINMM);
        if (winmmModule2) { PFN_TIME_BEGIN_PERIOD beginPeriod2 =
                       (PFN_TIME_BEGIN_PERIOD)GetProcAddress(winmmModule2, HOST_EXPORT_TIME_BEGIN_PERIOD);
                   if (beginPeriod2) beginPeriod2(1); }
    }
    CpuSpeedRecompute();
    g_CpuSpeedRelease = CreateEventA(NULL, FALSE, FALSE, NULL);   /* #225, auto-reset */
    g_CpuSpeedThread = CreateThread(NULL, 0, CpuSpeedThread, NULL, 0, NULL);
    /* ⚠ THE SAME RATE THE MIXER WAS BUILT AT. Opening the device at one rate and
         mixing at another silently resamples everything to a clock nothing runs
         on -- audible as a pitch error, not as an error message. */
    g_Wave.WantsDirectSound = (g_Settings.Values[SET_AUDIOAPI] == 1);          /* #234 */
    g_Wave.IsForcedSilent = g_Safe.AudioOut;                 /* s90 #132: SAFE MODE */
    g_Wave.MidiChoice = (INT)(g_Settings.Values[SET_MIDI] < MIDI_ROUTE_COUNT ? g_Settings.Values[SET_MIDI] : 0);  /* #136 */
    AudioWaveStart(&g_Wave, SettingsOutputHz(&g_Settings), HostAudioFill, NULL);
    /* ── #136: SAY WHICH SYNTH THE MPU-401 PLAYS THROUGH, but only when it was chosen.
         Host GM (the default) opens device 0 as it always did and logs nothing new. */
    if (g_Wave.MidiChoice != MIDI_ROUTE_GM) {
        CHAR midiLine[200], *midiCursor = LogPut(midiLine, "STAGE2: MIDI = ");
        midiCursor = LogPut(midiCursor, g_Wave.MidiChoice == MIDI_ROUTE_MT32 ? "MT-32" : "SoundFont");
        if (g_Wave.IsMidiExternal) {
            midiCursor = LogPut(midiCursor, " -> device "); midiCursor = LogDecimal(midiCursor, (UINT)g_Wave.MidiDevice);
            midiCursor = LogPut(midiCursor, " \""); midiCursor = LogPut(midiCursor, g_Wave.MidiName); midiCursor = LogPut(midiCursor, "\", SysEx passed through");
            g_Mpu.SysExSink = HostMidiSysEx;     /* the MPU is on the bus already; no */
            g_GusMidi.SysExSink = HostMidiSysEx; /* guest code has run yet            */
        } else {
            midiCursor = LogPut(midiCursor, " asked for, NO such device among "); midiCursor = LogDecimal(midiCursor, g_Wave.MidiDeviceCount);
            midiCursor = LogPut(midiCursor, " -> Host GM (device 0");
            if (g_Wave.MidiName[0]) { midiCursor = LogPut(midiCursor, " \""); midiCursor = LogPut(midiCursor, g_Wave.MidiName); midiCursor = LogPut(midiCursor, "\""); }
            midiCursor = LogPut(midiCursor, g_Wave.MidiDevice < 0 ? ", would not open)" : ")");
        }
        if (g_Wave.MidiChoice == MIDI_ROUTE_SF2)
            midiCursor = LogPut(midiCursor, "; SoundFontPath is not passed on -- the driver keeps its own list");
        midiCursor = LogPut(midiCursor, " (#136)\r\n"); LogAppend(LOG_PATH, midiLine, midiCursor);
    }
    {   CHAR audioLine[128], *audioCursor = LogPut(audioLine, "STAGE2: audio output = ");
        audioCursor = LogPut(audioCursor, g_Wave.IsUsingDirectSound ? "DirectSound" : g_Wave.IsSilent ? "none (silent pump)" : "WinMM");
        if (g_Wave.WantsDirectSound && !g_Wave.IsUsingDirectSound) audioCursor = LogPut(audioCursor, " (DirectSound asked for, would not open)");
        audioCursor = LogPut(audioCursor, "\r\n"); LogAppend(LOG_PATH, audioLine, audioCursor); }
    /* cfg\wavrec.flag: record the whole run's audio -- see HostRecordFinish. */
    if (GetFileAttributesA(WAVREC_FLAG) != INVALID_FILE_ATTRIBUTES) {
        INT recordResult = AudioWaveRecordStart(WAVREC_PATH, g_Wave.SampleHz);
        cursor = LogPut(cursor, recordResult == 0 ? "STAGE1: wavrec.flag -- recording the audio output to debug\\out\\capture_audio.wav at "
                            : "STAGE1: wavrec.flag -- COULD NOT start the recording at ");
        cursor = LogDecimal(cursor, g_Wave.SampleHz); cursor = LogPut(cursor, " Hz\r\n");
    }
    return cursor;
}


/* Read the cfg\ tuning files: the audio lead (awbufs, awframes), the PIT pacer, the keyboard hook and IRQ, the tick courier, the UI tick, WOW idling, mouse sensitivity, and the CPU reference, speed, granularity and affinity. */
static VOID StartupLoadTuningKnobs(VOID)
{
    /* ── THE AUDIO LEAD, AS A CONTROLLED VARIABLE (awbufs.txt). ──────────────────────
         Each queued waveOut buffer is ~11.6 ms that our DMA read pointer runs ahead of
         what is audible, and the guest must refill a block before we reach it. Doom's
         longest PM stretch with no host turn measured 62.8 ms against a 70 ms lead, so
         the lead is a suspect for the residual ECHO -- the capture is 46% identical to
         one ring lap (185.8 ms) earlier, against ~22% at every neighbouring lag.
         Setting this and reading back `sb replay:` in STAGE2 is the experiment: if
         replays move with the lead it is the race, if they do not, DMX is failing to
         refill for another reason and the lead is the wrong suspect. Absent = 6. */
    { HANDLE handle = CreateFileA(AWBUFS_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             NULL, OPEN_EXISTING, 0, NULL);
      if (handle != INVALID_HANDLE_VALUE) {
          CHAR text[16]; DWORD bytesRead = 0, number = 0; INT index;
          ReadFile(handle, text, sizeof text, &bytesRead, NULL);
          CloseHandle(handle);
          for (index = 0; index < (INT)bytesRead; ++index) {
              if (text[index] < '0' || text[index] > '9') break;
              number = number * DECIMAL_RADIX + (DWORD)(text[index] - '0');
          }
          g_Wave.BufferCount = number;                   /* AudioWaveStart clamps to [2,AUDIO_WAVE_BUFFERS] */
      } }
    /* ── AND THE GRANULARITY, AS A SEPARATE CONTROLLED VARIABLE (awframes.txt). ──────
         nframes x nbufs is the LEAD; nframes alone is the STEP the guest's DMA read
         pointer moves in. They are different suspects and must be varied independently
         or a result cannot be attributed to either. `awbufs=2` already showed why this
         matters: it cut the lead, starved the transport, and the replay rate "improved"
         only because the non-flat block count collapsed 13x.
         To hold the lead constant while quartering the step: awframes=128, awbufs=24. */
    { HANDLE handle = CreateFileA(AWFRAMES_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             NULL, OPEN_EXISTING, 0, NULL);
      if (handle != INVALID_HANDLE_VALUE) {
          CHAR text[16]; DWORD bytesRead = 0, number = 0; INT index;
          ReadFile(handle, text, sizeof text, &bytesRead, NULL);
          CloseHandle(handle);
          for (index = 0; index < (INT)bytesRead; ++index) {
              if (text[index] < '0' || text[index] > '9') break;
              number = number * DECIMAL_RADIX + (DWORD)(text[index] - '0');
          }
          g_Wave.FrameCount = number;                 /* clamped to [AUDIO_WAVE_MIN_FRAMES,AUDIO_WAVE_FRAMES] */
      } }
    { HANDLE pitPaceFile = CreateFileA(PITPACE_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, 0, NULL);
      if (pitPaceFile != INVALID_HANDLE_VALUE) {
          CHAR text[8]; DWORD bytesRead = 0;
          ReadFile(pitPaceFile, text, sizeof text, &bytesRead, NULL); CloseHandle(pitPaceFile);
          if (bytesRead && text[0] >= '0' && text[0] <= '9') g_PitPaceMs = text[0] - '0';
          g_PitPaceOn = (g_PitPaceMs != 0);
          SettingsNoteOverride(SET_PITPACE, CFG_TEXT(KNOB_FILE_PITPACE) SETTINGS_SOURCE_MS, (DWORD)g_PitPaceMs);
      } }
    /* The pacer's two OTHER levers -- see PitPacerThread. Absent file = as shipped. */
    { HANDLE pitPriorityFile = CreateFileA(PITPRIO_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, 0, NULL);
      if (pitPriorityFile != INVALID_HANDLE_VALUE) {
          CHAR text[8]; DWORD bytesRead = 0;
          ReadFile(pitPriorityFile, text, sizeof text, &bytesRead, NULL); CloseHandle(pitPriorityFile);
          if (bytesRead && text[0] >= '0' && text[0] <= '4') {
              static const INT priorities[5] = { THREAD_PRIORITY_IDLE, THREAD_PRIORITY_BELOW_NORMAL,
                                          THREAD_PRIORITY_NORMAL, THREAD_PRIORITY_ABOVE_NORMAL,
                                          THREAD_PRIORITY_HIGHEST };
              g_PitPacePriority = priorities[text[0] - '0'];
          }
      } }
    { HANDLE pitInjectFile = CreateFileA(PITINJ_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, 0, NULL);
      if (pitInjectFile != INVALID_HANDLE_VALUE) {
          CHAR text[8]; DWORD bytesRead = 0;
          ReadFile(pitInjectFile, text, sizeof text, &bytesRead, NULL); CloseHandle(pitInjectFile);
          if (bytesRead && (text[0] == '0' || text[0] == '1')) g_PitPaceInject = text[0] - '0';
      } }
    /* llkbd.txt = 1 re-enables the system-wide keyboard hook. See InputCaptureSet. */
    { HANDLE keyHandle = CreateFileA(LLKBD_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL, OPEN_EXISTING, 0, NULL);
      if (keyHandle != INVALID_HANDLE_VALUE) {
          CHAR text[8]; DWORD bytesRead = 0;
          ReadFile(keyHandle, text, sizeof text, &bytesRead, NULL); CloseHandle(keyHandle);
          if (bytesRead && (text[0] == '0' || text[0] == '1')) g_LowLevelKeyboardOn = text[0] - '0';
      } }
    /* courier.txt -- the tick courier (see TickCourierThread). 0 = as shipped. */
    { HANDLE configHandle = CreateFileA(COURIER_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL, OPEN_EXISTING, 0, NULL);
      if (configHandle != INVALID_HANDLE_VALUE) {
          CHAR text[8]; DWORD bytesRead = 0;
          ReadFile(configHandle, text, sizeof text, &bytesRead, NULL); CloseHandle(configHandle);
          /* 0 = off, 1 = REFUTED (progressive collapse), 2 = gentle. See the thread. */
          if (bytesRead && text[0] >= '0' && text[0] <= '2') g_CourierOn = text[0] - '0';
      } }
    { HANDLE uiTickFile = CreateFileA(UITICK_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, 0, NULL);
      if (uiTickFile != INVALID_HANDLE_VALUE) {
          CHAR text[8]; DWORD bytesRead = 0; INT number = 0, index;
          ReadFile(uiTickFile, text, sizeof text, &bytesRead, NULL); CloseHandle(uiTickFile);
          for (index = 0; index < (INT)bytesRead; ++index) {
              if (text[index] < '0' || text[index] > '9') break;
              number = number * DECIMAL_RADIX + (text[index] - '0');
          }
          if (bytesRead && text[0] >= '0' && text[0] <= '9' && number <= UI_TICK_MS_MAX) {
              g_UiTickMinimumMs = number;
              SettingsNoteOverride(SET_UITICK, CFG_TEXT(KNOB_FILE_UITICK) SETTINGS_SOURCE_MS, (DWORD)number);
          }
      } }
    /* wowidle.txt -- how long a Win16 task blocked in GetMessage waits. 0 = forever. */
    { HANDLE wowIdleFile = CreateFileA(WOWIDLE_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, 0, NULL);
      if (wowIdleFile != INVALID_HANDLE_VALUE) {
          CHAR text[12]; DWORD bytesRead = 0, number = 0; INT index;
          ReadFile(wowIdleFile, text, sizeof text, &bytesRead, NULL); CloseHandle(wowIdleFile);
          for (index = 0; index < (INT)bytesRead; ++index) {
              if (text[index] < '0' || text[index] > '9') break;
              number = number * DECIMAL_RADIX + (DWORD)(text[index] - '0');
          }
          if (bytesRead && text[0] >= '0' && text[0] <= '9') {
              CHAR wowLine2[160], *wowCursor2 = wowLine2;
              g_WowMsgWaitMs = number;
              wowCursor2 = LogPut(wowCursor2, "WOWMSG: GetMessage idle wait = ");
              if (number) { wowCursor2 = LogHex(wowCursor2, number); wowCursor2 = LogPut(wowCursor2, " ms"); }
              else     wowCursor2 = LogPut(wowCursor2, "FOREVER (interactive: the guest is waiting "
                                     "for the user, not stuck)");
              wowCursor2 = LogPut(wowCursor2, "\r\n"); LogAppend(LOG_PATH, wowLine2, wowCursor2); SerialOut(wowLine2, wowCursor2);
          }
      } }
    /* keyirq.txt -- the knob 5b6a4a6's message promises. It was lost in that session's
       revert, so the escape hatch documented at the retry site did not actually exist. */
    { HANDLE keyIrqFile = CreateFileA(KEYIRQ_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, 0, NULL);
      if (keyIrqFile != INVALID_HANDLE_VALUE) {
          CHAR text[8]; DWORD bytesRead = 0;
          ReadFile(keyIrqFile, text, sizeof text, &bytesRead, NULL); CloseHandle(keyIrqFile);
          /* 0 = never yield, 1 = always (old default), 2 = only while the clock is on
             schedule. See the yield branch in HostIrqSink. */
          if (bytesRead && text[0] >= '0' && text[0] <= '3') g_KeyIrqRetry = text[0] - '0';
      } }
    /* Mouse feel is per-guest and per-hand, and every test of it costs a play session,
       so it is a knob from the start: percent, 100 = the device's own counts. */
    { HANDLE mouseSensitivityFile = CreateFileA(MSENS_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, 0, NULL);
      if (mouseSensitivityFile != INVALID_HANDLE_VALUE) {
          CHAR text[8]; DWORD bytesRead = 0; INT value2 = 0, index2;
          ReadFile(mouseSensitivityFile, text, sizeof text, &bytesRead, NULL); CloseHandle(mouseSensitivityFile);
          for (index2 = 0; index2 < (INT)bytesRead; ++index2) { if (text[index2] < '0' || text[index2] > '9') break;
                                          value2 = value2 * DECIMAL_RADIX + (text[index2] - '0'); }
          if (value2 >= MOUSE_SENSITIVITY_MIN && value2 <= MOUSE_SENSITIVITY_MAX) {
              g_MouseSensitivity = value2;
              SettingsNoteOverride(SET_MSENS, CFG_TEXT(KNOB_FILE_MSENS), (DWORD)value2);
          }
      } }
    /* ── GH #56: the calibration and a one-run speed override, both from the share.
         ⚠ THESE RUN BEFORE CpuSpeedRecompute() BELOW, which is the whole point: the
           duty is computed once from whatever the registry and these two agree on,
           and re-computed only when the setting changes. Reading them after would
           leave the thread running on the registry's answer for the whole session,
           which is exactly the shape of a knob that silently does nothing. */
    { HANDLE cpuReferenceFile = CreateFileA(CPUREF_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, 0, NULL);
      if (cpuReferenceFile != INVALID_HANDLE_VALUE) {
          CHAR text[12]; DWORD bytesRead = 0; UINT sbValue = 0; INT index2;
          ReadFile(cpuReferenceFile, text, sizeof text, &bytesRead, NULL); CloseHandle(cpuReferenceFile);
          for (index2 = 0; index2 < (INT)bytesRead; ++index2) { if (text[index2] < '0' || text[index2] > '9') break;
                                          sbValue = sbValue * DECIMAL_RADIX_U + (UINT)(text[index2] - '0'); }
          if (sbValue >= CPU_REFERENCE_MHZ_MIN_U && sbValue <= CPU_REFERENCE_MHZ_MAX_U) g_CpuSpeedReferenceMhz = sbValue;
      } }
    StartupLoadCpuSpeedKnob();
    /* ── THE GRANULARITY SLIDER AS A FILE KNOB. cpugran.txt = target period in ms,
         0 or absent = AUTO (measure the suspend round trip and pick the finest
         period this box can sustain). See the long note in cpuspeed.h -- this is
         the lever that decides whether a slow setting is playable or a slideshow,
         and it is a file knob first so the sweep can find the right default without
         a rebuild per value. */
    { HANDLE cpuGranularityFile = CreateFileA(CPUGRAN_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, 0, NULL);
      if (cpuGranularityFile != INVALID_HANDLE_VALUE) {
          CHAR text[12]; DWORD bytesRead = 0; UINT valueG = 0; INT indexG;
          ReadFile(cpuGranularityFile, text, sizeof text, &bytesRead, NULL); CloseHandle(cpuGranularityFile);
          for (indexG = 0; indexG < (INT)bytesRead; ++indexG) { if (text[indexG] < '0' || text[indexG] > '9') break;
                                             valueG = valueG * DECIMAL_RADIX_U + (UINT)(text[indexG] - '0'); }
          if (indexG > 0 && valueG <= CPUSPEED_GRAN_MAX_MS) g_CpuSpeedGranularityMs = valueG;
      } }
    /* cpuaff.txt = 1 -> give the guest a core of its own. See CpuAffinityApply. */
    { HANDLE cpuAffinityFile = CreateFileA(CPUAFF_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, 0, NULL);
      if (cpuAffinityFile != INVALID_HANDLE_VALUE) {
          CHAR text[8]; DWORD bytesRead = 0;
          ReadFile(cpuAffinityFile, text, sizeof text, &bytesRead, NULL); CloseHandle(cpuAffinityFile);
          if (bytesRead && (text[0] == '0' || text[0] == '1')) g_CpuAffinityOn = (text[0] == '1');
      } }
}


/* Put the machine on the bus: the host lock; the PIC and the PIT with its clock hooks; CMOS, FDC, IDE and video; the system font; the keyboard; the serial ports, declared in the BIOS data area, which is then initialised; NetBIOS, the speaker, the gameport, DMA, OPL, Sound Blaster, MPU-401, GUS and AWE32; the WOW callbacks; then the mixer, which also drives the SB's DMA. */
static PSTR StartupAttachDevices(PSTR cursor)
{
    /* Stand up the device bus (NULL base => absolute V86 addresses) with the PIT
       (ports 0x40-0x43, INT 08h/1Ah) and the video VDD (B8000 + INT 10h text +
       cell renderer). The present sink is DirectDraw via present_ddraw on the UI
       thread. I/O on claimed ports reflects as event 0 -> the bus; INT 10h comes
       in as a BOP routed below; DOS console output is routed via m.conout. */
    InitializeCriticalSection(&g_Lock);
    InitializeCriticalSection(&g_PitCs);       /* the crystal's own lock; see its decl */
    g_Pit.Guard = HostPitGuard;               /* port handlers serialize with the pacer */
    g_Pit.GuardContext = NULL;
    g_Pit.RtcNow = HostRtcNow;               /* INT 1Ah AH=02h/04h -- see the hook */
    g_Pit.RtcContext = NULL;
    g_Pit.RtcSet = HostRtcSet;               /* INT 1Ah AH=03h/05h -- the VDM's RTC (#250) */
    g_Pit.TicksSet = HostTicksSet;           /* INT 1Ah AH=01h -> DOS's clock (#262) */
    g_DosTickTake = HostTickTake;           /* a raw 006C store -> DOS's clock (#262 B) */
    QueryPerformanceFrequency(&g_QpcFrequency);      /* seeds QpcMicroseconds for the lock instrument */
    HostKeyTypematicInitialize();              /* typematic from XP's setting, not a guess */
    VddBusInitialize(&g_Bus, NULL);
    VddBusSetSinks(&g_Bus, HostIrqSink, NULL, NULL, NULL);  /* host presents directly */
    g_PicDevice = VddPicDevice(&g_Pic);      /* before the PIT: it gates every IRQ */

    VddBusAdd(&g_Bus, &g_PicDevice);
    g_PitDevice = VddPitDevice(&g_Pit);
    VddBusAdd(&g_Bus, &g_PitDevice);
    /* ── ★ 0040:006C IS TICKS SINCE MIDNIGHT, SO SET IT TO THAT. (GH #253) ──
         POST does this from the RTC; nothing here did, so every launch began at
         00:00:00 by the BIOS's clock while INT 1Ah AH=02h read the real time. Seeded
         from the SAME hook AH=02h answers from (HostRtcNow), after the PIT is on
         the bus and before anything can take IRQ0. See VddPitSeedTimeOfDay. */
    VddPitSeedTimeOfDay(&g_Pit);
    /* ── THE RTC/CMOS TAKES THE SAME CLOCK INT 1Ah DOES. ─────────────────────
         Registers 00h-09h and INT 1Ah AH=02h/04h are two doors onto ONE clock,
         and a guest may use either -- so they are given the same hook and their
         agreement is structural rather than something to keep in step by hand.
         The same principle as A20's three doors; p_rtc.asm's rtc.agree.hours is
         the case that checks it, and it read 0000 against 0101 on all three
         oracles before this device existed. */
    g_Cmos.RtcNow = HostRtcNow;
    g_Cmos.RtcContext = NULL;
    g_Cmos.RtcSet = HostRtcSet;              /* GH #261: CMOS 00h-09h + 32h writes */
    g_WowWinCtlColor  = WowControlColour;              /* s89: WM_CTLCOLOR via the nested run */
    g_WowUserSend16    = WowSend16Now;            /* s89 #305: WM_DESTROY sent, not posted */
    g_WowWinSend16    = WowSend16Now;            /* s89 #300: WM_H/VSCROLL sent from the tracking loop */
    g_WowUserCall16    = WowCall16Sync;           /* s91 #308: a subclassed control's messages */
    g_WowWinOwnerDraw = WowOwnerDraw;             /* s89 #302: owner-draw via the nested run */
    g_WowWinGlobal16  = ShimGlobal16;             /* s92 #305 M12: a Win16 HDROP is a krnl386 block */
    g_WowUserSend16Blob   = WowSend16Blob;           /* s89 #302: WM_CREATE to template controls */
    g_WowWinSend16Blob   = WowSend16Blob;           /* s91 #305 M9: WM_GETMINMAXINFO */
    g_Cmos.BaseKb = (WORD)(BiosBaseKbOfTop(g_DosMemoryTop) + BIOS_EBDA_KB);  /* #136 */
    g_CmosDevice = VddCmosDevice(&g_Cmos);
    VddBusAdd(&g_Bus, &g_CmosDevice);           /* MC146818: ports 0x70/0x71    */
    /* ── THE FLOPPY CONTROLLER, WHOSE ABSENCE WAS A HANG. ────────────────────
         3F0h-3F7h were claimed by nothing, so the Main Status Register read FFh
         -- RQM=1 with DIO=1 -- and the datasheet's own command-write loop
         (`and al,0C0h / cmp al,80h / jne`) never matched and never exited.
         MEASURED on the rig before this existed: fdc.cmdwait = 01C0 here against
         0080 on 6.22/QEMU and on PCem's real AMI BIOS alike. Same shape as the
         MC146818's UIP bit two devices above. IRQ6 stays dormant unless a guest
         both gates it through DOR bit 3 and unmasks it at the PIC, which starts
         at 0xFC. See src/vdd/vdd_fdc.h. */
    g_FdcDevice = VddFdcDevice(&g_Fdc);
    VddBusAdd(&g_Bus, &g_FdcDevice);            /* 82077AA: 3F2h-3F5h, 3F7h     */
    /* ── THE IDE ADAPTER, FITTED, BOTH CHANNELS EMPTY. (GH #179) ─────────────
         Same shape as the FDC above, one surface later: nothing claimed 1F0h-1F7h
         or 3F6h, FFh there is BSY=1, and the ATA's own "wait until BSY clears"
         never exited. An adapter with no drives reads 00h everywhere (DD7 pulled
         down by the ATA document; DD6:0 a recorded choice) and latches nothing,
         so a detection routine finds the controller, finds no device, and moves
         on. See src/vdd/vdd_ide.h for why 00h and not 7Fh. */
    g_IdeDevice = VddIdeDevice(&g_Ide);
    VddBusAdd(&g_Bus, &g_IdeDevice);            /* ATA: 1F0h-1F7h, 3F6h, 170h-177h, 376h-377h */
    /* ...and the BIOS says the same thing: 0040:0075, the number of fixed disks a
       program reads before it calls INT 13h DL=80h, is WRITTEN 0 rather than left
       to whatever the page held (docs/inventory/bda.md 3). One fact, three doors:
       the BDA, INT 13h, and the adapter's empty channels. */
    *(volatile BYTE *)(ULONG_PTR)(BIOS_BDA_BASE + BIOS_BDA_FIXED_DISK_COUNT) = 0;
    g_Video.VideoMemory = (BYTE *)VIDEO_APERTURE_BASE;  /* the mapped A0000 aperture (RAM) */
    /* (per-plane backing is taken later, once the preamble is on disk -- every
       LogWrite() before that point TRUNCATES the file and would eat its report.) */
    g_Video.TimeUs = HostTimeMicroseconds;               /* real CRT timebase for 0x3DA (#55) */
    g_Video.PresentHook = HostPresentHook;     /* Auto: the guest's frame raises the present (s73) */
    /* Opt-in only: the ring costs two stores on the hottest path in the program and
       the dump does file I/O under g_Lock. See the note in vdd_video.h. */
    g_Video.IsPort3DaRingOn =
        (GetFileAttributesA(PITLATCH_FLAG) != INVALID_FILE_ATTRIBUTES);
    g_Video.GuestPc = HostGuestPc;             /* so a VRAM watchpoint names a routine */
    g_Video.BiosData = (BYTE *)BIOS_BDA_BASE;               /* the display's BDA fields (0449..0489) */
    g_VideoDevice = VddVideoDevice(&g_Video);
    VddBusAdd(&g_Bus, &g_VideoDevice);
    StartupLoadVideoWatch();
    /* AFTER the video VDD is on the bus (it needs st->bus to resolve a guest address). */
    /* #322: no font data ships -- the tables come from the system's fonts. Into the
       report buffer: a LogAppend here would be erased when the report is rewritten. */
    /* #321: the TextFont setting is laid over the default; g_Settings is loaded by now. */
    cursor = LogPut(cursor, "STAGE1: ");
    cursor = LogPut(cursor, SysFontBuild(g_Settings.Strings[SET_STR_TEXTFONT], &g_SysFontReport));
    cursor = LogPut(cursor, "\r\n");
    if (SysFontIsDefaultDegraded(&g_SysFontReport))
        cursor = LogPut(cursor, "STAGE1: sysfont: THE DEFAULT IS DEGRADED -- a code page 437 font file "
                    "is missing, so box drawing may show accented letters (#321)\r\n");
    lstrcpynA(g_TextFontLive, g_Settings.Strings[SET_STR_TEXTFONT], sizeof g_TextFontLive);
    VddVideoInstallFonts(&g_Video);            /* real glyph data behind INT 10h 1130h */
    /* The BIOS keyboard buffer belongs to the guest: point the VDD at 0040:0000 BEFORE the
       bus resets it, so the ring pointers it initialises land in guest memory where a DOS
       program reading 0040:001A can see them. V86 low memory is mapped in our address
       space, so the BDA is addressable directly. */
    g_Input.BiosData = (BYTE *)BIOS_BDA_BASE;
    g_Input.TimeMicroseconds = HostTimeMicroseconds;                /* the keyboard's transfer time is real time */
    g_InputDevice = VddInputDevice(&g_Input);
    VddBusAdd(&g_Bus, &g_InputDevice);             /* keyboard: claims INT 16h      */
    /* ── ★★ THE BDA's PORT BASE-ADDRESS TABLE, WHICH WE HAD LEFT AT ZERO. (GH #128) ──
         0040:0000..0007 are the COM1..COM4 I/O bases and 0040:0008..000F the LPT1..LPT4
         bases. Nothing ever wrote them, so every one read as 0 -- while our INT 11h
         equipment word (0x4021) says bits 14-15 = 01 = ONE PARALLEL PORT. Our own BIOS
         was contradicting itself: a port declared present in the equipment word whose
         base address is 0.
       ★ THAT INCONSISTENCY IS WHY COMM.DRV FAILS TO LOAD, measured end to end this
         session. Its LibMain returns the word at 0040:0008 -- the LPT1 base address,
         read through the BDA selector (`__0040H`) -- as its result. Zero means "DLL
         initialisation failed"
         (documented LibMain contract), LoadModule returns 0, and the boot dies with "NTVDM
         KERNEL: Missing 16-bit system module
         ... COMM.DRV". Five drivers whose LibMain returns 1 load; this one does not.
       ⚠ SO WRITE ONLY WHAT THE EQUIPMENT WORD ALREADY CLAIMS -- one parallel port at
         the standard LPT1 base, and NO serial ports. Filling in COM1..COM4 as well
         would be inventing hardware nothing answers for, which is the "runs but lies"
         class this project treats as its most expensive kind of bug. The equipment
         word is the declaration; this table just stops disagreeing with it.
       ⚠ AFTER the input VDD is on the bus: it initialises the keyboard ring through
         the same 0040:0000 pointer, and doing this first would be overwritten. */
    /* ── ★★★ AND NOW THERE ARE SERIAL PORTS, SO SAY SO. (GH #9, session 56) ──
         The note above is right that filling in COM1..COM4 while nothing
         answered for them would be inventing hardware -- that was the correct
         call when there was no UART. There is one now: vdd_comm.c claims
         0x3F8..0x3FF and 0x2F8..0x2FF and answers every register, including the
         local-loopback self-test every serial driver runs before it believes a
         port exists. So the declaration is no longer a lie -- and the equipment
         word is updated in the same breath, because the whole point of the
         original note is that the two must not disagree. */
    { INT callbackIndex;
      for (callbackIndex = 0; callbackIndex < COMM_MAX_PORTS; ++callbackIndex) {
          g_ComSpool[callbackIndex] = INVALID_HANDLE_VALUE; g_ComFailed[callbackIndex] = 0; }
      g_Comm.Ports[0].BasePort = COMM_COM1_BASE; g_Comm.Ports[0].Irq = COMM_COM1_IRQ; g_Comm.Ports[0].IsFitted = 1;
      g_Comm.Ports[1].BasePort = COMM_COM2_BASE; g_Comm.Ports[1].Irq = COMM_COM2_IRQ; g_Comm.Ports[1].IsFitted = 1;
      /* #245 (s90): COM3 AND COM4 ARE FITTED BECAUSE STOCK DECLARES THEM. Measured
         with tests/probes/dos/p_com34 under XP's own NTVDM on the rig: INT 11h
         AX=C823 (FOUR serial ports, bits 9-11) and BDA 0040:0000 = 03F8 02F8 03E8
         02E8. The question #181 left open is answered by the oracle that defines
         "ntvdm superset", and the device has had the slots since s85. */
      g_Comm.Ports[2].BasePort = COMM_COM3_BASE; g_Comm.Ports[2].Irq = COMM_COM1_IRQ; g_Comm.Ports[2].IsFitted = 1;
      g_Comm.Ports[3].BasePort = COMM_COM4_BASE; g_Comm.Ports[3].Irq = COMM_COM2_IRQ; g_Comm.Ports[3].IsFitted = 1;
      g_Comm.Printers[0].BasePort = LPT_DEFAULT_BASE; g_Comm.Printers[0].IsFitted = 1;   /* LPT1 data/strobe */
      g_Comm.Sink = ComTransmitSink; g_Comm.SinkContext = NULL;
      g_Comm.PrinterSink = LptTransmitSink; g_Comm.PrinterSinkContext = NULL;
      g_CommDevice = VddCommDevice(&g_Comm);
      VddBusAdd(&g_Bus, &g_CommDevice); }        /* 8250/16550A + INT 14h        */
    VddNetBiosSetBackend(&g_Net, NetSubmit, NULL);
    g_NetDevice = VddNetBiosDevice(&g_Net);
    VddBusAdd(&g_Bus, &g_NetDevice);             /* GH #8: NetBIOS, INT 5Ch       */
    { volatile WORD *biosDataArea = (volatile WORD *)(ULONG_PTR)BIOS_BDA_BASE;
      /* Declare exactly what the VDD actually CLAIMED. A port whose claim was
         refused for want of a bus table slot is not fitted, and writing its base
         here anyway would recreate the very inconsistency this block exists to
         fix, one layer down. */
      /* ── ★ ONE LOOP OVER THE VDD'S SLOTS, NOT FOUR LITERALS. (GH #181) ──
           The table has room for COM1..COM4 and the VDD now has four slots,
           so the row for each comes from the slot's own base and fitted flag
           -- the same source BiosEquipmentWord() counts. COM3/COM4 are not
           fitted above (that is an oracle question: whether a period machine
           of the kind we model declares four ports, #181), so they still read
           0 here; the point is that fitting one is now a single line above
           and this table and INT 11h follow it without being edited. */
      { INT callbackIndex;
        for (callbackIndex = 0; callbackIndex < BIOS_BDA_COM_PORTS; ++callbackIndex)
            biosDataArea[callbackIndex] = (WORD)(VddCommIsFitted(&g_Comm, callbackIndex) ? g_Comm.Ports[callbackIndex].BasePort : 0); }
      biosDataArea[BIOS_BDA_LPT_BASES / X86_WORD_SIZE] = (WORD)(VddLptIsFitted(&g_Comm, 0) ? LPT_DEFAULT_BASE : 0);   /* LPT1          */
      biosDataArea[BIOS_BDA_LPT_BASES / X86_WORD_SIZE + 1] = 0; biosDataArea[BIOS_BDA_LPT_BASES / X86_WORD_SIZE + 2] = 0; }                         /* LPT2..LPT3: none fitted   */
    /* ── ★★ 000E, 0010, 0013 AND THE EBDA, FROM THE FUNCTIONS INT 11h/12h CALL. (#253)
         0040:000E is LPT4 on a PC and the EBDA segment on an AT and later; this block
         used to zero it as "LPT4: none", which on an AT reads as "no EBDA" -- while
         INT 12h said 639 KB, i.e. that one exists. bios_bda.h settles it: there is a
         1 KB EBDA at 9FC0h, and 000E, INT 15h C1h and the C0h table all say so.
         0010 and 0013 were never written at all; they now hold BiosEquipmentWord()
         and BIOS_BASE_MEM_KB, the same expressions INT 11h and INT 12h return, so a
         guest that reads the BDA and one that calls the interrupt see one machine.
       ⚠ AFTER the COM/LPT slots are fitted (the word counts them) and after
         SettingsApply() has set the joystick type (bit 12). g_BdaReady lets a later
         settings change re-write 0010 -- see BiosBdaRefreshEquipment. */
    BiosBdaInitializeWithTop(NULL, BiosEquipmentWord(), g_DosMemoryTop);   /* #136 */
    g_BdaReady = 1;
    g_Speaker.Pit = &g_Pit;                         /* speaker tone <- PIT channel 2 */
    g_SpeakerDevice = VddSpeakerDevice(&g_Speaker);
    VddBusAdd(&g_Bus, &g_SpeakerDevice);            /* PC speaker: claims port 0x61  */
    g_Joystick.NowMicroseconds = JoystickNowMicroseconds;
    g_JoystickDevice = VddJoystickDevice(&g_Joystick);
    VddBusAdd(&g_Bus, &g_JoystickDevice);            /* gameport: 0x200-0x207         */
    /* ⛔ THE POLL THREAD IS NOT SPAWNED HERE. See JoystickPollEnsure: it is created
         ONLY when a joystick is actually configured, so the default play config --
         which is EVERY game that does not use a gamepad, Skyroads included -- runs
         with the exact s61 thread landscape and cannot regress on its account. */
    g_DmaDevice = VddDmaDevice(&g_Dma);
    VddBusAdd(&g_Bus, &g_DmaDevice);            /* 8237 DMA: 0x00-0x0F/80-8F/C0-DF */
    g_Opl.IsExternalClock = 1;                        /* exec loop pumps real elapsed us */
    g_OplDevice = VddOplDevice(&g_Opl);
    VddBusAdd(&g_Bus, &g_OplDevice);            /* AdLib/OPL2: ports 0x388/0x389 */
    g_Sb.Dma = &g_Dma; g_Sb.Opl = &g_Opl;       /* SB pulls PCM via DMA, mirrors FM */
    /* ⚠ THE SAME NUMBERS THAT GO INTO BLASTER (dos_env.h). If these two ever come
         from different places, a driver is told one port and finds another. */
    g_Sb.BasePort = g_SbConfig.IoBase; g_Sb.Irq = g_SbConfig.Irq;
    g_Sb.Dma8 = g_SbConfig.Dma8Channel;
    if (g_SbConfig.Dma16Channel) g_Sb.Dma16 = g_SbConfig.Dma16Channel;   /* 0 = keep vdd_sb's default */
    /* Opt-in raw PCM capture -- see SB_STATE.CaptureBuffer. 4 MB is ~3 minutes of Doom's
       11025 Hz stereo, and it is a static buffer so the audio thread never allocates. */
    if (GetFileAttributesA(SBDUMP_FLAG) != INVALID_FILE_ATTRIBUTES) {
        static BYTE sbCapture[4u * 1024u * 1024u];
        g_Sb.CaptureBuffer = sbCapture; g_Sb.CaptureCapacity = sizeof sbCapture; g_Sb.CaptureLength = 0;
    }
    g_SbDevice = VddSbDevice(&g_Sb);
    VddBusAdd(&g_Bus, &g_SbDevice);             /* Sound Blaster 16: 0x220-0x22F  */
    g_Mpu.Sink = HostMidiSink;
    {   static const WORD mpuBases[5] = { 0x300, 0x310, 0x320, 0x330, 0x340 };   /* #235 */
        g_Mpu.BasePort = mpuBases[g_Settings.Values[SET_MPUADDR] <= ARRAYSIZE(mpuBases) - 1 ? g_Settings.Values[SET_MPUADDR] : MPU_DEFAULT_BASE_CHOICE]; }
    g_MpuDevice = VddMpuDevice(&g_Mpu);
    VddBusAdd(&g_Bus, &g_MpuDevice);            /* MPU-401 MIDI: 0x330/0x331      */
    /* The Gravis UltraSound: 240h-24Fh and 340h-347h, IRQ 11, DMA 3 (docs/ref/gus.md).
       ⚠ THE SAME NUMBERS GO INTO ULTRASND= -- see the environment build. */
    if (g_GusOn) {                             /* decided at startup: see NOGUS_FLAG's read */
        g_Gus.Dma = &g_Dma; g_Gus.Dram = g_GusDram;
        g_GusMidi.Sink = HostMidiSink;            /* #190: the 6850 UART -> the synth */
        g_Gus.MidiSink = GusMidiToSynth;
        g_GusDevice = VddGusDevice(&g_Gus);
        VddBusAdd(&g_Bus, &g_GusDevice);
    }
    /* #233: the AWE32's EMU8000 -- at the SB's base + 400h / 800h / C00h (620h, A20h,
       E20h for a card at 220h), 512 KB of sample DRAM, and BLASTER's E says where. */
    g_AweOn = (g_Settings.Values[SET_SBMODEL] == SB_MODEL_AWE32);
    if (g_AweOn) {
        g_Emu8K.BasePort = (WORD)(g_SbConfig.IoBase + SB_EMU8K_PORT_OFFSET);
        g_Emu8K.Dram = g_Emu8KDram; g_Emu8K.DramWords = EMU8K_DRAM_WORDS;
        g_Emu8KDevice = VddEmu8kDevice(&g_Emu8K);
        VddBusAdd(&g_Bus, &g_Emu8KDevice);
    }
    /* ► SAY WHETHER EVERY DEVICE ACTUALLY GOT ON THE BUS. VDD_MAX_PORT_RANGES was 16 and
         exactly full; adding one range pushed the LAST device added -- the MPU-401 --
         off, its claim returned -1, nobody looked, and the guest's MIDI port read 0xFF
         like an empty slot. Doom reset it four times, got nothing, and played no music.
         A device that cannot get on the bus is not a detail to discover by diffing
         port traces against a working run. */
    /* (the bus health line is emitted after the preamble is written -- see below;
       every LogWrite() before then TRUNCATES the file.) */
    /* Start the mixer + audio thread. This is also the TRANSPORT: it is what
       walks the SB's DMA buffer and raises the block-completion IRQ, so it must
       run even if no sound device opens (AUDIO_WAVE falls back to silent
       pumping) -- otherwise every SB game hangs on a machine without audio. */
    VddAudioInitialize(&g_Audio, &g_Opl, &g_Sb, SettingsOutputHz(&g_Settings));
    VddAudioSetGus(&g_Audio, g_GusOn ? &g_Gus : NULL);
    VddAudioSetEmu8k(&g_Audio, g_AweOn ? &g_Emu8K : NULL);   /* #233 */
    SettingsApplyDevices(&g_Settings);   /* master volume, mute, speaker -- the mixer
                                         zeroes its own struct, so not one line earlier */
    return cursor;
}


/* Build DOS in conventional memory: load the program, plant the INT 21h and BIOS stubs, fill the IVT, build the environment, the command tail, the MCB chain and the List of Lists, settle the version, plant the country and drive tables, and start XMS and EMS. */
static VOID StartupBuildDos(PSTR *cursorIo, const DWORD readCount, DOS_IMAGE *image, CHAR *programPathBuffer, CHAR *args, DOS_MACHINE *machine, CHAR *report)
{
    PSTR cursor = *cursorIo;
    volatile BYTE * handlerArea;
    UINT index;
    /* If this is a bound linear executable (every DOS/4GW game is one), learn which of
       its objects are code before it starts asking us for memory to load them into. */
    DpmiLeLearn(g_FileBuffer, readCount);

    cursor = StartupApplyConventionalKb(cursor, readCount);
    /* Build the DOS process in conventional memory (base=NULL => absolute V86). */
    (*image) = DosLoadImage(NULL, g_FileBuffer, readCount, DOS_PSP_SEG);

    static const BYTE bop[] = { VDM_BOP0, VDM_BOP1, DOS_BOP_INT21, X86_OP_IRET };  /* BOP 0x20 ; iret */
    static const BYTE bop10[] = { VDM_BOP0, VDM_BOP1, DOS_BOP_FOR_VECTOR(VECTOR_VIDEO), X86_OP_IRET }; /* BOP 0x10 ; iret */
    static const BYTE bop16[] = { VDM_BOP0, VDM_BOP1, DOS_BOP_FOR_VECTOR(VECTOR_KEYBOARD_SERVICES), X86_OP_IRET }; /* BOP 0x16 ; iret */
    static const BYTE bop33[] = { VDM_BOP0, VDM_BOP1, DOS_BOP_FOR_VECTOR(VECTOR_MOUSE), X86_OP_IRET }; /* BOP 0x33 ; iret */
    /* INT 08h (timer): tick via BOP, then chain INT 1Ch, then iret. INT 1Ch is a
       bare iret by default (the user-timer hook a program may repoint). INT 1Ah
       (BIOS time-of-day) is a plain BOP. */
    static const BYTE bop08[] = { VDM_BOP0, VDM_BOP1, DOS_BOP_FOR_VECTOR(VECTOR_TIMER), X86_OP_INT, VECTOR_USER_TICK, X86_OP_IRET };
    static const BYTE bop1c[] = { X86_OP_IRET };                           /* iret stub       */
    /* Default INT 09h = BOP 09 ; IRET. It must CONSUME the scancode, exactly as the BIOS
       handler does: a bare IRET left the byte in the controller forever, so with the 8042's
       proper one-byte-at-a-time pacing no further key could ever raise an interrupt (the
       whole keyboard died after one press). A game that installs its own INT 09h replaces
       this vector, so its handler still reads port 0x60 itself. */
    static const BYTE bop09[] = { VDM_BOP0, VDM_BOP1, DOS_BOP_FOR_VECTOR(VECTOR_KEYBOARD), X86_OP_IRET };
    static const BYTE bop1a[] = { VDM_BOP0, VDM_BOP1, DOS_BOP_FOR_VECTOR(VECTOR_TIME), X86_OP_IRET }; /* BOP 0x1A ; iret */
    static const BYTE bop2f[] = { VDM_BOP0, VDM_BOP1, DOS_BOP_FOR_VECTOR(VECTOR_MULTIPLEX), X86_OP_IRET }; /* INT 2Fh ; iret  */
    /* XMS API entry: reached by FAR CALL (INT 2Fh AX=4310 hands back ES:BX), so it
       ends in RETF (0xCB), not IRET. */
    static const BYTE xmsBopStub[] = { VDM_BOP0, VDM_BOP1, DOS_BOP_XMS_ENTRY, X86_OP_RETF };
    static const BYTE bop67[] = { VDM_BOP0, VDM_BOP1, DOS_BOP_FOR_VECTOR(VECTOR_EMS), X86_OP_IRET }; /* INT 67h ; iret  */
    static const BYTE emmDeviceName[] = { 'E','M','M','X','X','X','X','0' };  /* EMS device header name */
    handlerArea = (volatile BYTE *)(DOS_HDLR_SEG << PARAGRAPH_SHIFT);            /* INT 21h BOP handler */
    for (index = 0; index < sizeof(bop); ++index) handlerArea[DOS_HDLR_INT21_STUB_OFF + index] = bop[index];
    *(volatile WORD *)IVT_OFFSET_ADDRESS(VECTOR_DOS) = DOS_HDLR_INT21_STUB_OFF;                        /* IVT[0x21].offset    */
    *(volatile WORD *)IVT_SEGMENT_ADDRESS(VECTOR_DOS) = DOS_HDLR_SEG;                  /* IVT[0x21].segment   */
    handlerArea[DOS_DBCS_OFF] = 0; handlerArea[DOS_DBCS_OFF + 1] = 0;     /* empty DBCS table    */
    for (index = 0; index < sizeof(bop10); ++index) handlerArea[DOS_HDLR_INT10_STUB_OFF + index] = bop10[index];  /* INT 10h stub */
    *(volatile WORD *)IVT_OFFSET_ADDRESS(VECTOR_VIDEO) = DOS_HDLR_INT10_STUB_OFF;                        /* IVT[0x10].offset    */
    *(volatile WORD *)IVT_SEGMENT_ADDRESS(VECTOR_VIDEO) = DOS_HDLR_SEG;                  /* IVT[0x10].segment   */
    for (index = 0; index < sizeof(bop16); ++index) handlerArea[DOS_HDLR_INT16_STUB_OFF + index] = bop16[index];  /* INT 16h stub */
    *(volatile WORD *)IVT_OFFSET_ADDRESS(VECTOR_KEYBOARD_SERVICES) = DOS_HDLR_INT16_STUB_OFF;                        /* IVT[0x16].offset    */
    *(volatile WORD *)IVT_SEGMENT_ADDRESS(VECTOR_KEYBOARD_SERVICES) = DOS_HDLR_SEG;                  /* IVT[0x16].segment   */
    for (index = 0; index < sizeof(bop33); ++index) handlerArea[DOS_HDLR_INT33_STUB_OFF + index] = bop33[index];  /* INT 33h stub */
    *(volatile WORD *)(IVT_OFFSET_ADDRESS(VECTOR_MOUSE))     = DOS_HDLR_INT33_STUB_OFF;              /* IVT[0x33].offset    */
    *(volatile WORD *)(IVT_SEGMENT_ADDRESS(VECTOR_MOUSE)) = DOS_HDLR_SEG;        /* IVT[0x33].segment   */
    for (index = 0; index < sizeof(bop08); ++index) handlerArea[DOS_HDLR_INT08_STUB_OFF + index] = bop08[index];  /* INT 08h stub */
    *(volatile WORD *)(IVT_OFFSET_ADDRESS(VECTOR_TIMER))     = DOS_HDLR_INT08_STUB_OFF;              /* IVT[0x08].offset    */
    *(volatile WORD *)(IVT_SEGMENT_ADDRESS(VECTOR_TIMER)) = DOS_HDLR_SEG;        /* IVT[0x08].segment   */
    for (index = 0; index < sizeof(bop1c); ++index) handlerArea[DOS_HDLR_INT1C_STUB_OFF + index] = bop1c[index];  /* INT 1Ch iret */
    *(volatile WORD *)(IVT_OFFSET_ADDRESS(VECTOR_USER_TICK))     = DOS_HDLR_INT1C_STUB_OFF;              /* IVT[0x1C].offset    */
    *(volatile WORD *)(IVT_SEGMENT_ADDRESS(VECTOR_USER_TICK)) = DOS_HDLR_SEG;        /* IVT[0x1C].segment   */
    for (index = 0; index < sizeof(bop09); ++index) handlerArea[DOS_HDLR_INT09_STUB_OFF + index] = bop09[index];  /* INT 09h default iret (0x4C-0x4F) */
    /* INT 33h event-handler return: the guest's handler RETFs here (see MouseCallbackTry). */
    handlerArea[MS_CB_RET_OFF + 0] = VDM_BOP0; handlerArea[MS_CB_RET_OFF + 1] = VDM_BOP1;
    handlerArea[MS_CB_RET_OFF + VDM_BOP_NUMBER_OFFSET] = MS_CB_BOP; handlerArea[MS_CB_RET_OFF + VDM_BOP_LENGTH] = X86_OP_IRET;
    /* DEFAULT DEVICE-IRQ HANDLERS. A real BIOS points the unused hardware vectors at a
       handler that just acknowledges and returns; we had them pointing at whatever junk was
       in the IVT, which on this box read F000:A390 -- unowned ROM. That was harmless only so
       long as we could not deliver a device IRQ asynchronously. Now that we can, injecting an
       IRQ the guest has not hooked jumps it into that junk and hangs it: measured, Skyroads
       (which never installs a Sound Blaster ISR at all) froze at F000:A390 the moment its DMA
       block completed. So give IRQ2-7 and IRQ8-15 a plain IRET, exactly as INT 09h has. */
    handlerArea[DOS_IRET_STUB_OFF] = X86_OP_IRET;                                /* shared IRET stub    */
    handlerArea[DOS_CASEMAP_OFF]   = X86_OP_RETF;                                /* AH=38h case map: RETF */
    { volatile BYTE *swappableDataArea = (volatile BYTE *)(ULONG_PTR)((DWORD)DOS_SDA_SEG << PARAGRAPH_SHIFT);   /* AH=34h/5D06h */
      INT item; for (item = 0; item < DOS_SDA_LEN; ++item) swappableDataArea[DOS_SDA_OFF + item] = 0; }
    for (index = VECTOR_IRQ2; index <= VECTOR_IRQ7; ++index) {
        *(volatile WORD *)(IVT_OFFSET_ADDRESS(index))     = DOS_IRET_STUB_OFF;
        *(volatile WORD *)(IVT_SEGMENT_ADDRESS(index)) = DOS_HDLR_SEG;
    }
    for (index = VECTOR_IRQ8; index <= VECTOR_IRQ15; ++index) {
        *(volatile WORD *)(IVT_OFFSET_ADDRESS(index))     = DOS_IRET_STUB_OFF;
        *(volatile WORD *)(IVT_SEGMENT_ADDRESS(index)) = DOS_HDLR_SEG;
    }
    *(volatile WORD *)(IVT_OFFSET_ADDRESS(VECTOR_KEYBOARD))     = DOS_HDLR_INT09_STUB_OFF;              /* IVT[0x09].offset    */
    *(volatile WORD *)(IVT_SEGMENT_ADDRESS(VECTOR_KEYBOARD)) = DOS_HDLR_SEG;        /* IVT[0x09].segment   */
    for (index = 0; index < sizeof(bop1a); ++index) handlerArea[DOS_HDLR_INT1A_STUB_OFF + index] = bop1a[index];  /* INT 1Ah stub */
    *(volatile WORD *)(IVT_OFFSET_ADDRESS(VECTOR_TIME))     = DOS_HDLR_INT1A_STUB_OFF;              /* IVT[0x1A].offset    */
    *(volatile WORD *)(IVT_SEGMENT_ADDRESS(VECTOR_TIME)) = DOS_HDLR_SEG;        /* IVT[0x1A].segment   */
    for (index = 0; index < sizeof(bop2f); ++index) handlerArea[DOS_HDLR_INT2F_STUB_OFF + index] = bop2f[index];  /* INT 2Fh stub */
    *(volatile WORD *)(IVT_OFFSET_ADDRESS(VECTOR_MULTIPLEX))     = DOS_HDLR_INT2F_STUB_OFF;              /* IVT[0x2F].offset    */
    *(volatile WORD *)(IVT_SEGMENT_ADDRESS(VECTOR_MULTIPLEX)) = DOS_HDLR_SEG;        /* IVT[0x2F].segment   */
    for (index = 0; index < sizeof(xmsBopStub); ++index) handlerArea[XMS_ENTRY_OFF + index] = xmsBopStub[index];  /* XMS far-call entry */
    /* ⚠ GH #47: a non-zero word at XMS_ENTRY_OFF+0x45 WAS TRIED AND REFUTED.
       MEM.EXE skips its whole extended-memory report on a zero word at +0x45 of
       some structure, and that structure looked like the XMS entry. It is not:
       planting HIMEM's own bytes (EB 50) at the entry changed nothing. Sixth
       refutation. */
    for (index = 0; index < sizeof(bop67); ++index) handlerArea[DOS_HDLR_INT67_STUB_OFF + index] = bop67[index];  /* INT 67h (EMM) stub */
    /* ⚠ NO EMS MEANS NO INT 67h VECTOR AND NO DEVICE NAME. Both halves, because
         a program detects EMM by either following the vector to the "EMMXXXX0"
         header OR by opening the device; leaving one of them behind is a manager
         that half-exists, which is worse for a guest than one that does not. */
    if (g_EmsOn) {
        *(volatile WORD *)(IVT_OFFSET_ADDRESS(VECTOR_EMS))     = DOS_HDLR_INT67_STUB_OFF;          /* IVT[0x67].offset    */
        *(volatile WORD *)(IVT_SEGMENT_ADDRESS(VECTOR_EMS)) = DOS_HDLR_SEG;    /* IVT[0x67].segment   */
    } else {
        *(volatile WORD *)(IVT_OFFSET_ADDRESS(VECTOR_EMS))     = 0;
        *(volatile WORD *)(IVT_SEGMENT_ADDRESS(VECTOR_EMS)) = 0;
    }
    /* DPMI mode-switch entry (far-called): BOP 0x50 ; RETF. The host services the
       BOP by switching to PM; the RETF only executes if the switch fails. */
    handlerArea[DPMI_ENTRY_OFF + 0] = VDM_BOP0; handlerArea[DPMI_ENTRY_OFF + 1] = VDM_BOP1;
    handlerArea[DPMI_ENTRY_OFF + VDM_BOP_NUMBER_OFFSET] = DPMI_BOP; handlerArea[DPMI_ENTRY_OFF + VDM_BOP_LENGTH] = X86_OP_RETF; /* RETF */
    /* DPMI 0301 real-mode-call return catcher: BOP 0x54 (no IRET/RETF -- the 0301
       handler detects it and returns to PM, it never resumes past it). */
    handlerArea[DPMI_RMRET_OFF + 0] = VDM_BOP0; handlerArea[DPMI_RMRET_OFF + 1] = VDM_BOP1;
    handlerArea[DPMI_RMRET_OFF + VDM_BOP_NUMBER_OFFSET] = DPMI_RMRET_BOP;
    /* DPMI 0303 real-mode callback entries (one per slot) + the PM-return catcher. */
    { INT callbackSlot; for (callbackSlot = 0; callbackSlot < DPMI_CB_SLOTS; ++callbackSlot) {
        WORD entry = DpmiCallbackEntry(DPMI_CB_BASE_OFF, callbackSlot);
        handlerArea[entry + 0] = VDM_BOP0;
        handlerArea[entry + 1] = VDM_BOP1;
        handlerArea[entry + VDM_BOP_NUMBER_OFFSET] = DPMI_CB_BOP;
    } }
    handlerArea[DPMI_PMRET_OFF + 0] = VDM_BOP0; handlerArea[DPMI_PMRET_OFF + 1] = VDM_BOP1;
    handlerArea[DPMI_PMRET_OFF + VDM_BOP_NUMBER_OFFSET] = DPMI_PMRET_BOP;
    /* 0306 raw mode-switch entries. Both are bare BOPs: the host completes the switch
       by rewriting the CONTEXT, so control never resumes past the BOP and no RETF/IRET
       tail is wanted (the same shape as DPMI_RMRET_OFF). The protected-to-real entry
       lives in this segment too and is reached through a code selector based here --
       see the 0306 handler. */
    handlerArea[DPMI_RAW2PM_OFF + 0] = VDM_BOP0; handlerArea[DPMI_RAW2PM_OFF + 1] = VDM_BOP1;
    handlerArea[DPMI_RAW2PM_OFF + VDM_BOP_NUMBER_OFFSET] = DPMI_RAW2PM_BOP;
    handlerArea[DPMI_RAW2RM_OFF + 0] = VDM_BOP0; handlerArea[DPMI_RAW2RM_OFF + 1] = VDM_BOP1;
    handlerArea[DPMI_RAW2RM_OFF + VDM_BOP_NUMBER_OFFSET] = DPMI_RAW2RM_BOP;
    /* 0305 save/restore: a register-preserving no-op (see the define). */
    handlerArea[DPMI_SSR_OFF] = X86_OP_RETF;                               /* RETF */
    /* (GH #18 run 67: the PM-fault handler BOP is planted at the handler CODE selector's
       DPMI_FAULT_COFF by DpmiInstallFaultTrampoline(), not here.) */
    /* EMS detection method 2: programs read the INT 67h vector's segment:000Ah for
       the device-driver name "EMMXXXX0". Park it in the handler segment. */
    if (g_EmsOn)
        for (index = 0; index < sizeof(emmDeviceName); ++index) handlerArea[DOS_EMM_NAME_OFF + index] = emmDeviceName[index];

    StartupPlantBiosStubs();

    /* ── INT 22h / 23h / 24h: REAL VECTORS, SO THE PSP CAN SAVE SOMETHING. (#34) ──
         Every PSP stores the live copies of these three and restores them at exit.
         They were whatever the IVT happened to hold, and the PSP fields were zero
         -- which passes "the saved copy matches the live vector" trivially when
         both are 0000:0000, so the gap could not be seen from that test alone.
       ▸ INT 24h returns AL=3, FAIL THE CALL. Real DOS's default lives in
         COMMAND.COM and prompts Abort/Retry/Ignore/Fail; there is no shell here to
         prompt with, and of the four answers FAIL is the only one that hands the
         error back to the program that can report it. IGNORE would corrupt data
         and RETRY would spin forever. Documented rather than chosen silently.
       ▸ INT 23h (Ctrl-Break) is a bare IRET: returning with CF clear means
         "carry on", which is what a host with no shell to return to should do.
       ▸ INT 22h (terminate address) routes to the same BOP as INT 20h, so a guest
         that jumps there actually exits instead of falling through the IVT. */
    {   volatile BYTE *controlBytes = (volatile BYTE *)(DOS_CTAB_SEG << PARAGRAPH_SHIFT);
        UINT position = DOS_CRIT_STUBS;
        controlBytes[position+0] = VDM_BOP0; controlBytes[position+1] = VDM_BOP1; controlBytes[position+VDM_BOP_NUMBER_OFFSET] = DOS_BOP_INT20; controlBytes[position+VDM_BOP_LENGTH] = X86_OP_IRET;
        *(volatile WORD *)(IVT_OFFSET_ADDRESS(VECTOR_TERMINATE_ADDRESS))     = (WORD)position;              /* INT 22h */
        *(volatile WORD *)(IVT_SEGMENT_ADDRESS(VECTOR_TERMINATE_ADDRESS)) = DOS_CTAB_SEG;
        controlBytes[position+DOS_CRIT_STUB_INT23] = X86_OP_IRET;                                          /* INT 23h: IRET */
        *(volatile WORD *)(IVT_OFFSET_ADDRESS(VECTOR_CTRL_C))     = (WORD)(position + DOS_CRIT_STUB_INT23);
        *(volatile WORD *)(IVT_SEGMENT_ADDRESS(VECTOR_CTRL_C)) = DOS_CTAB_SEG;
        controlBytes[position+DOS_CRIT_STUB_INT24] = X86_OP_MOV_IMM_BYTE_FIRST; controlBytes[position+DOS_CRIT_STUB_INT24+1] = DOS_CRIT_ACTION_FAIL; controlBytes[position+DOS_CRIT_STUB_INT24+2] = X86_OP_IRET;         /* mov al,3 ; iret */
        *(volatile WORD *)(IVT_OFFSET_ADDRESS(VECTOR_CRITICAL_ERROR))     = (WORD)(position + DOS_CRIT_STUB_INT24);        /* INT 24h */
        *(volatile WORD *)(IVT_SEGMENT_ADDRESS(VECTOR_CRITICAL_ERROR)) = DOS_CTAB_SEG;
        /* #34: the site DOS calls the guest's INT 24h from -- see CriticalRaise. */
        controlBytes[DOS_CRIT_RAISE + 0] = X86_OP_INT; controlBytes[DOS_CRIT_RAISE + 1] = VECTOR_CRITICAL_ERROR;   /* int 24h  */
        controlBytes[DOS_CRIT_RETURN + 0] = VDM_BOP0; controlBytes[DOS_CRIT_RETURN + 1] = VDM_BOP1;
        controlBytes[DOS_CRIT_RETURN + VDM_BOP_NUMBER_OFFSET] = DOS_BOP_INT21;                                 /* bop 20h  */
    }
    /* #251: DOS's AUX/PRN driver code, which INT 21h resumes the guest in -- see
       dos_auxprn.asm for why it is guest code and what it was measured against. */
    {   volatile BYTE *controlBytes = (volatile BYTE *)(DOS_CTAB_SEG << PARAGRAPH_SHIFT);
        UINT item;
        for (item = 0; item < sizeof(g_DosAuxPrnCode); ++item) controlBytes[DOS_AUXPRN_OFF + item] = g_DosAuxPrnCode[item];
        /* #254: the BIOS INT 09h's side-calls -- see bios_kbdact.asm. */
        for (item = 0; item < sizeof(g_BiosKeyboardActionCode); ++item) controlBytes[DOS_KBDACT_OFF + item] = g_BiosKeyboardActionCode[item];
        /* ── #274: INT 05h IS OURS NOW -- THE BIOS PRINT-SCREEN ROUTINE (p5). ─────────────
             A fresh VDM left IVT[05h] at F000:FF54, a jump deeper into the VDM's own ROM
             that KeyboardActionEntry refuses to enter (p_ivtkbd), so Print Screen called nothing
             and a program's own `int 5` went somewhere we cannot vouch for. Every BIOS
             since the PC has a routine here; ours prints the screen through INT 17h and
             keeps its status HOST-side -- see PrintScreenBop for why not at 0050:0000. */
        *(volatile WORD *)(ULONG_PTR)(IVT_OFFSET_ADDRESS(VECTOR_PRINT_SCREEN))     = (WORD)(DOS_KBDACT_OFF + BIOS_KEYBOARD_ACTION_DEFAULT_INT05);
        *(volatile WORD *)(ULONG_PTR)(IVT_SEGMENT_ADDRESS(VECTOR_PRINT_SCREEN)) = DOS_CTAB_SEG;
    }

    /* GH #27 -- THE NULL-VECTOR LANDMINE. A vector left at 0000:0000 sends a guest
       that INTs it to 0000:0000, where it executes the interrupt vector table
       itself as code. Point any such vector at the shared IRET stub.
       MEASURED BEFORE FIXING, and the measurement narrowed the fix: on the
       bare-metal rig most unclaimed vectors are NOT null -- they carry the VDM's
       own BIOS entries (INT 13h read F000:5595, INT 11h F000:F84D). Planting over
       those would swap a working handler for a bare IRET, i.e. a silent
       "success", which is the very failure mode this issue exists to remove. So
       fill only the genuinely null ones, and name them in the log. */
    { INT number, count = 0, start = -1;
      cursor = LogPut(cursor, "STAGE0: null IVT vectors -> IRET stub:");
      for (number = 0; number <= IVT_VECTORS; ++number) {                  /* 256 flushes a trailing run */
          INT isNullVector = (number < IVT_VECTORS) && (*(volatile DWORD *)(IVT_OFFSET_ADDRESS(number)) == 0);
          if (isNullVector) {
              *(volatile WORD *)(IVT_OFFSET_ADDRESS(number))     = DOS_IRET_STUB_OFF;
              *(volatile WORD *)(IVT_SEGMENT_ADDRESS(number)) = DOS_HDLR_SEG;
              if (start < 0) start = number;
              ++count;
          } else if (start >= 0) {                  /* emit as ranges, not 133 items */
              cursor = LogPut(cursor, " 0x"); cursor = LogHexByte(cursor, (UINT)start);
              if (number - 1 > start) { cursor = LogPut(cursor, "-0x"); cursor = LogHexByte(cursor, (UINT)(number - 1)); }
              start = -1;
          }
      }
      if (!count) cursor = LogPut(cursor, " none");
      cursor = LogPut(cursor, "\r\n"); }

    DosPspBuild(NULL, DOS_PSP_SEG, DOS_ENV_SEG, g_DosMemoryTop);   /* #136 */
    /* AFTER the vectors above are planted, never before: saving a vector that is
       still 0000:0000 stores a null the program restores on the way out. Parent
       PSP = our own, since nothing launched us from inside the VDM. (GH #34) */
    DosPspSaveVectors(NULL, DOS_PSP_SEG, DOS_PSP_SEG);
    /* ── ★ EXTRA ENVIRONMENT VARIABLES FROM dosenv.txt. Read here, next to the block
         being built, so a knob that is absent costs exactly one failed open and the
         environment is byte-identical to what it has always been. */
    DsProbeLoad();          /* the #GP fault report's named guest data words */
    cursor = StartupBuildEnvironment(cursor, programPathBuffer);
    DosPspBuildCommandTail(NULL, DOS_PSP_SEG, args);                                    /* M2.5: args */
    /* ► DUMP THE TAIL AS THE GUEST WILL SEE IT. Passing ANY argument makes DOS/4GW
         quit before printing a single character, with a DPMI/INT 21h trace identical
         to a working run for all 617 of its lines -- so the branch it takes is on
         MEMORY, and this is the memory. Length byte, the bytes, and the terminator. */
    { volatile BYTE *pspView = (volatile BYTE *)((DWORD)DOS_PSP_SEG << PARAGRAPH_SHIFT);
      UINT textIndex;
      cursor = LogPut(cursor, "STAGE2: cmdtail len=0x"); cursor = LogHexByte(cursor, pspView[DOS_PSP_COMMAND_TAIL_LENGTH]);
      cursor = LogPut(cursor, " [");
      for (textIndex = 0; textIndex < COMMAND_TAIL_DUMP_BYTES; ++textIndex) { cursor = LogHexByte(cursor, pspView[DOS_PSP_COMMAND_TAIL + textIndex]); cursor = LogPut(cursor, " "); }
      cursor = LogPut(cursor, "]\r\n"); }
    StartupBuildMemoryChain(machine, programPathBuffer);
    /* Published so the Settings dialog can change the reported DOS version while a
       guest is running -- it is read per INT 21h AH=30h, so it takes effect at the
       guest's next version check with no restart. */
    g_DosMachine = machine;
    DosInt21SetVersion(machine, (BYTE)g_Settings.Values[SET_DOSMAJ], (BYTE)g_Settings.Values[SET_DOSMIN]);
    /* Two sources, and the second one silently wins -- see the note below the file read. */
    PCSTR dosVersionSource = "HKCU\\Software\\NTVDMEX (Settings dialog)";
    /* ── THE REPORTED DOS VERSION IS A KNOB, BECAUSE IT IS A LIE THE GUEST CHOOSES.
         Real DOS ships SETVER for precisely this, and the number is not a fact about
         us: it is what a particular guest will accept. We default to 6.22 to match the
         M9 oracle, and XP's OWN COMMAND.COM refuses that outright -- "Incorrect DOS
         version", INT 21h AH=00h, terminated before it printed a prompt. NT's DOS has
         always reported 5.00 and its shell is built to match.
         `dosver.txt` on the share: "5.0", "6.22", "3.31" -- major.minor decimal. */
    /* ── ★ AN NTVDM-AWARE SHELL GETS 5.00 WITHOUT ANYONE HAVING TO ASK. (s79) ────────
         XP's COMMAND.COM accepts only AX = 5 exactly (5.00 -- observed: 6.22 is
         refused) and prints "Incorrect DOS version" otherwise, so on the default 6.22 a
         double-click would die before it
         printed anything. This is not a global policy change: it applies only to a
         guest we loaded AS THE SHELL that carries NTVDM's own BOPs (see the scan), and
         `cfg\dosver.txt` below still overrides it. 6.22's COMMAND.COM has no BOPs and
         is untouched -- it keeps 6.22, which is the version it expects. */
    /* ⇒ s81 (#208), the user's choice: SETVER, NOT A SESSION-WIDE 5.00. Every program
         started from Windows now runs UNDER this shell, so forcing the whole session to
         5.00 would have changed the version every program sees. Only the SHELL'S OWN
         PROCESS is told 5.00 (DosInt21SetShellPsp); what it runs gets the setting. */
    if (g_GuestNtAware) {
        DosInt21SetShellPsp(machine, DOS_PSP_SEG, TRUE);
        dosVersionSource = "the setting -- the NTVDM-aware shell ITSELF is told 5.00 (per process, SETVER-style)";
        g_DosVersionShell = 1;
    } else if (g_GuestNtvdmBops >= NT_AWARE_SHELL_BOPS_MIN && g_TopIsShell) {
        /* s91: XP's COMMAND.COM launched AS THE PROGRAM -- `command.com /c prog > file`
             from cmd.exe, or a user typing `command` there. It is not "the shell we
             chose", so the rule above did not apply, and on the default 6.22 it said
             "Incorrect DOS version" and quit where stock runs it (launch matrix row 5,
             runs/s91/chain18b). Same image test and same per-process 5.00 as the
             EXEC path gives a second XP shell; the name check is the second factor
             that keeps an innocent guest from being told DOS 5. */
        DosInt21SetShellPsp(machine, DOS_PSP_SEG, TRUE);
        dosVersionSource = "the setting -- XP's COMMAND.COM run as the program is told 5.00 (per process, SETVER-style)";
        g_DosVersionShell = 1;
    }
    dosVersionSource = StartupLoadDosVersionKnob(dosVersionSource, machine);
    /* ── ★ SAY WHICH VERSION IS IN FORCE, AND WHERE IT CAME FROM. EVERY RUN. ──────
         This printed a line ONLY when `dosver.txt` overrode, so the persistent source
         -- HKCU\Software\NTVDMEX\DosVersionMajor/Minor, written by the Settings dialog
         -- was completely silent. The rig was found reporting **5.00 to every DOS
         guest** from that registry value, with no `dosver.txt` anywhere, on a project
         whose entire parity method diffs against a 6.22 oracle (`p_ver.com`:
         `int21.30 AX=0005`). Deleting the file, which is what every note about this
         knob says to do afterwards, does NOT restore the default -- and nothing
         anywhere reported the discrepancy.
       ⚠ The standing rule this breaks is "a status line nobody reads is not a check";
         this was worse, because there was no line at all. Unconditional now, and it
         names the SOURCE, because the number alone would not have caught it either.
       ⚠ DECIMAL, and it took a wrong reading to notice. The first cut used zhexb and
         printed "6.22" as **06.16**, which a human reads as version 6.16 -- a number in
         the wrong units is not a measurement, and this line exists precisely so nobody
         has to decode it. Minor is zero-padded to two digits: "6.2" and "6.20" are
         different DOS versions. */
    cursor = LogPut(cursor, "STAGE2: DOS version reported = ");
    cursor = LogDecimal(cursor, machine->VersionMajor); cursor = LogPut(cursor, ".");
    if (machine->VersionMinor < 10) cursor = LogPut(cursor, "0");
    cursor = LogDecimal(cursor, machine->VersionMinor);
    cursor = LogPut(cursor, " (source: "); cursor = LogPut(cursor, dosVersionSource); cursor = LogPut(cursor, ")\r\n");
    cursor = StartupConfigureAh53Answers(cursor);
    StartupPlantCountryTables();
    /* GH #35: plant SysVars for INT 21h AH=52h. Most fields are deliberately
       left zero -- see the handler for why a null stub beats a plausible-looking
       one. Only fields with a caller that demonstrably reads them are filled:
         BX-2   the first MCB segment (GH #35)
         +0x21  LASTDRIVE -- krnl386 takes a pointer to this byte through the
                SysVars+0x6A table below, so zero here means it believes there
                are no drives at all.
       ★ s81: SysVars has its OWN segment now (DOS_SYSVARS_SEG, see dos_layout.h), so
         the whole of it is ours to clear -- the old "+0x40 only, the SDA follows"
         limit was a symptom of it sharing DOS_HDLR_SEG. */
    volatile BYTE *sysVars = (volatile BYTE *)(ULONG_PTR)((DWORD)DOS_SYSVARS_SEG << PARAGRAPH_SHIFT);
    { INT item; for (item = DOS_SYSVARS_MCB_HEAD; item < DOS_SYSVARS_LEN; ++item) sysVars[DOS_SYSVARS_OFF + item] = 0; }
    *(volatile WORD *)(sysVars + DOS_SYSVARS_OFF + DOS_SYSVARS_MCB_HEAD) = machine->FirstMcb;
    /* ⚠⚠ SysVars+0x66 = "first MCB in upper memory" (= absolute SEG:0x008C, which MEM
         also reads directly -- see DOS_UMBHEAD_OFF). 0xFFFF means "none", the truth
         on a machine that refuses AH=5803. Zero here is what MEM /C walked as a UMB
         chain starting at segment 0. SysVars+0x68 holds the first MCB again, as it
         does on 6.22 and PCem (p_sysvar). */
    *(volatile WORD *)(sysVars + DOS_UMBHEAD_OFF) = DOS_UMBHEAD_NONE;
    *(volatile WORD *)(sysVars + DOS_SYSVARS_OFF + DOS_SYSVARS_FIRST_MCB_COPY) = machine->FirstMcb;
    sysVars[DOS_SYSVARS_OFF + DOS_SYSVARS_BLOCK_DEVICES] = 1;                      /* block devices       */
    sysVars[DOS_SYSVARS_OFF + DOS_SYSVARS_LASTDRIVE] = DOS_LASTDRIVE;          /* LASTDRIVE           */
    machine->SysvarsSegment = DOS_SYSVARS_SEG;
    machine->SysvarsOffset = DOS_SYSVARS_OFF;
    StartupBuildDriveTables(report, sysVars, machine);
    /* GH #128: and the WOW extension krnl386 reads before it does anything else. */
    DosWowPublish(handlerArea, (volatile BYTE *)(DOS_CTAB_SEG << PARAGRAPH_SHIFT), DOS_DRIVE_C);
    DosXmsInitialize(&g_Xms, XMS_POOL_KB, XmsHostAllocate, XmsHostFree, NULL);  /* M4: XMS pool */
    /* ── ★ THE HMA: 64KB-16 AT LINEAR 0x100000, REACHED AS FFFF:0010. (s72) ───────
         p_xms measured us refusing it TWICE over -- AH=00h answered DX=0 ("no HMA")
         and AH=01h answered BL=0x90 ("HMA does not exist") -- against an oracle that
         has one. That is an unimplemented FEATURE, not a wrong number.
       ▸ In this design a guest linear address IS a host virtual address (every
         `(seg<<4)+off` deref in this file depends on it), so the HMA is exactly one
         committed 64KB page range at 0x100000. Whether NT lets us have that address
         in a VDM process is an empirical question, so it is ASKED and LOGGED rather
         than assumed: a guest is told DX=1 only if the memory is really there.
       ▸ No A20 aliasing, and that is a decision already recorded in dos_xms.h: "an
         NT VDM does not wrap at 1 MB -- the line is effectively always open". We
         model the A20 FLAG (AH=03h..07h) but not the address wrap. A program that
         disables A20 and then expects FFFF:0010 to alias 0000:0000 would see the
         HMA instead; none of the panel does, and inventing a wrap would mean
         remapping views on every A20 toggle. */
    /* (the attempt itself is made early, in HmaTry(), and reported in the STAGE0
       preamble -- ONE buffered flush, which survives the log-handle race that
       swallowed this line entirely when it was appended separately here.) */

    DosEmsInitialize(&g_Ems, (WORD)(g_EmsFrameLinear >> PARAGRAPH_SHIFT), EMS_POOL_PAGES,
             (volatile BYTE *)g_EmsFrameLinear,
             EmsHostAllocate, EmsHostFree, NULL);             /* M4: 8MB EMS pool   */
    *cursorIo = cursor;
}


/* Find and load the program: CSRSS's application, cfg\target.txt, the title path, a PIF, XP's COMMAND.COM (#208) or a shell; then count the NTVDM BOP sites an NTVDM-aware shell carries. */
static VOID StartupLoadProgram(PSTR *cursorIo, DWORD *readCountIo, CHAR *programPathBuffer, CHAR *args, const INT wowCommandFromCsrss)
{
    PSTR cursor = *cursorIo;
    INT wantShell = 0;                 /* s79: our own launcher stub asked for a SHELL (see LAUNCH_STUB_NAME) */
    DWORD readCount = *readCountIo;
    INT wasShell  = 0;                 /* s79: we actually loaded a shell, not a named program */
    UINT index;
    StartupLoadCsrssApplication(&cursor, &wantShell, &readCount, programPathBuffer, args);
    StartupLoadTarget(&cursor, &readCount, wowCommandFromCsrss, wantShell, programPathBuffer, args);
    StartupLoadTitlePath(&cursor, &readCount, programPathBuffer, args);
    if (!readCount && g_CurrentDirectory[0] && g_Title[0]) {
        CHAR path[768]; PSTR pathCursor = path; HANDLE fileHandle; int targetLength; PSTR targetArguments = NULL; /* stays int: INT here moves the compiled code */
        pathCursor = LogPut(pathCursor, g_CurrentDirectory); pathCursor = LogPut(pathCursor, HOST_PATH_SEPARATOR); pathCursor = LogPut(pathCursor, g_Title);
        for (targetLength = 0; path[targetLength]; ++targetLength) ;              /* same trailing-space trim as above */
        while (targetLength > 0 && (path[targetLength - 1] == ' ' || path[targetLength - 1] == '\t')) path[--targetLength] = 0;
        fileHandle = CsrssOpenSplit(path, &targetArguments);        /* a RELATIVE title carries args too */
        if (fileHandle != INVALID_HANDLE_VALUE) { ReadFile(fileHandle, g_FileBuffer, sizeof(g_FileBuffer), &readCount, NULL); CloseHandle(fileHandle); }
        LogPut(programPathBuffer, path);                       /* env argv[0] */
        if (targetArguments)         LogPut(args, targetArguments);
        else if (g_CommandLine[0]) LogPut(args, g_CommandLine);       /* best-effort: CmdLine if CSRSS populated it */
        cursor = LogPut(cursor, "STAGE2: loaded 0x"); cursor = LogHex(cursor, readCount);
        cursor = LogPut(cursor, " from "); cursor = LogPut(cursor, path);
        if (targetArguments) { cursor = LogPut(cursor, " args=["); cursor = LogPut(cursor, targetArguments); cursor = LogPut(cursor, "]"); }
        cursor = LogPut(cursor, "\r\n");
    }
    /* ── ★★★★★ NOTHING NAMED A PROGRAM ⇒ RUN A SHELL. (s78) ──────────────────────
         The user's question was *"do you actually need to build a shell, or simply load
         Windows NT's COMMAND.COM when ntvdmex is loaded with no guest EXE?"* -- and the
         answer is the second one. `COMMAND.COM` IS the shell; it becomes the guest like
         any other DOS program, and MS-DOS 6.22's copy already works here: banner,
         prompt, `ver`, and a real `dir` listing with volume serial and free space
         (measured on the rig, s78).
       ⚠ STRICTLY BELOW EVERYTHING ELSE, and that placement is the whole design. CSRSS's
         AppName, `target.txt`, an absolute title and a relative title have all been
         tried above and all failed. Put this any higher and the headless harness -- which
         names its program in `target.txt` -- silently runs a shell instead of the test.
       ▸ WHICH shell is a configuration question, not a guess:
           1. `cfg\shell.txt`  -- a path the user chooses. A 6.22 COMMAND.COM goes here.
           2. `C:\WINDOWS\SYSTEM32\COMMAND.COM` -- present on every XP box.
         ⚠ (2) is XP's own, which is NTVDM-aware and stops at `BOP 0x54` -- see
           docs/inventory/bop.md. That is not a reason to leave it out: it fails with a
           log line naming exactly what is missing, where the old fallback was a 4-byte
           `mov ah,4Ch / int 21h` that exited cleanly and said nothing at all.
         ⛔ COMSPEC is deliberately NOT consulted: under a Windows session it names
           `cmd.exe`, a 32-bit PE that must never be loaded as a DOS guest. */
    /* ── #208: HAND A DOS PROGRAM TO XP's SHELL INSTEAD OF LOADING IT. See g_Routed. ──
         Only when all of these hold, and otherwise exactly as before:
           - a DOS program was found (an MZ/COM image, not NE/PE -- a Win16 or Win32
             image under COMMAND.COM just says "requires Microsoft Windows")
           - this is not a WOW launch, and the program is not itself a COMMAND.COM
           - the shell would be XP's own (no cfg\shell.txt, no Settings choice -- #203): only that shell asks
             BOP 54 sub 01, so only it can be handed a program
           - its 8.3 path and arguments fit a DOS command line
           - cfg\directlaunch.flag is absent (the A/B switch back to direct loading) */
    /* ── #203: WHICH SHELL, DECIDED ONCE. default (XP's own) < Settings' "DOS prompt"
         (HKCU DosPrompt) < cfg\shell.txt -- the file wins, as every file knob does, so
         the harness is never overridden by whatever was last picked in the dialog. The
         #208 routing below and the shell load after it both read this one answer; they
         used to test the file separately, which a registry setting would have split. */
    CHAR shellConfig[512]; PCSTR shellSource = 0;
    shellConfig[0] = 0;
    {   HANDLE configHandle = CreateFileA(SHELL_PATH, GENERIC_READ, FILE_SHARE_READ,
                                NULL, OPEN_EXISTING, 0, NULL);
        if (configHandle != INVALID_HANDLE_VALUE) {
            DWORD commandLength = 0; INT shellLength;
            ReadFile(configHandle, shellConfig, sizeof(shellConfig) - 1, &commandLength, NULL); CloseHandle(configHandle);
            shellConfig[commandLength < sizeof(shellConfig) ? commandLength : sizeof(shellConfig) - 1] = 0;
            /* Trim the newline the file almost certainly ends with, and any spaces --
               the same trap target.txt's reader already documents. */
            for (shellLength = 0; shellConfig[shellLength]; ++shellLength) ;
            while (shellLength > 0 && (shellConfig[shellLength-1] == '\r' || shellConfig[shellLength-1] == '\n'
                              || shellConfig[shellLength-1] == ' ' || shellConfig[shellLength-1] == '\t'))
                shellConfig[--shellLength] = 0;
            if (shellLength) {
                shellSource = "cfg\\shell.txt";
                if (g_Settings.Strings[SET_STR_SHELL][0]) g_ShellOverride = "cfg\\shell.txt";
            }
        }
        if (!shellConfig[0] && g_Settings.Strings[SET_STR_SHELL][0]) {
            lstrcpynA(shellConfig, g_Settings.Strings[SET_STR_SHELL], sizeof shellConfig);
            shellSource = "Settings > General > DOS prompt";
        }
    }
    StartupApplyPif(&cursor, &readCount, programPathBuffer, args);
    /* #153: File > Open Recent lists every program this host was started with. */
    if (readCount && !g_WowLaunch && programPathBuffer[0]) MruAdd(programPathBuffer);
    StartupRouteToCommandCom(&cursor, &readCount, shellConfig, programPathBuffer, args);
    StartupLoadShell(&cursor, &readCount, shellConfig, shellSource, programPathBuffer, &wasShell);
    if (!readCount) {
        static const BYTE stub[] = { X86_OP_MOV_AH_IMM, DOS_FN_EXIT, X86_OP_INT, VECTOR_DOS };   /* mov ah,4Ch; int 21h */
        for (index = 0; index < sizeof(stub); ++index) g_FileBuffer[index] = stub[index];
        readCount = sizeof(stub);
        /* ⚠ SAY WHY, not just that. This printed "embedded fallback" and nothing else,
             and it is reached when a path was wrong as well as when nothing was named --
             GH #131 spent a session on a run that looked clean and wrote nothing. */
        cursor = LogPut(cursor, "STAGE2: embedded fallback -- nothing named a program AND no shell "
                    "could be opened (cfg\\shell.txt, C:\\WINDOWS\\SYSTEM32\\COMMAND.COM)"
                    "\r\n");
    }

    /* ── ★★★ IS THIS GUEST NTVDM-AWARE? ASK THE IMAGE, NOT THE PATH. (s79) ───────────
         XP's own COMMAND.COM needs two things no ordinary DOS guest does: it refuses
         any DOS version but 5.00, and it reads `INT 21h AH=53h`'s private AL
         sub-functions to decide whether it is an interactive shell at all. Both were
         `cfg\` knobs, which is the right shape for an experiment and the wrong one for
         a product -- "double-click NTVDMEX and get a prompt" cannot require two files.

       ⇒ The discriminator is a MEASURED PROPERTY OF THE IMAGE, not a filename: an
         NTVDM-aware guest talks to the 32-bit side through `C4 C4 54 <sub>` BOPs. XP's
         COMMAND.COM has FIFTEEN of them. A path check would be a guess (a user may put
         XP's shell in `cfg\shell.txt`, or ours somewhere else); the BOPs are what
         actually make it NT-aware.
       ⚠ THE THRESHOLD IS THE GUARD. `C4 C4` is a legal, if odd, instruction pair, so
         one or two sites prove nothing -- a false positive would silently change the
         DOS version reported to an innocent guest. Requiring EIGHT distinct sites is
         far beyond coincidence and still well under XP's fifteen, so a future build of
         the shell with a few fewer would still be recognised. The count is logged, so
         a guest that lands near the line says so instead of being decided silently.
       ⚠ It is only consulted for a program we loaded as THE SHELL. A DOS game that
         somehow tripped the count must not be told it is running on DOS 5. */
    g_GuestNtvdmBops = 0;
    if (readCount > VDM_BOP_SUBFUNCTION_LENGTH) {
        DWORD item;
        for (item = 0; item + VDM_BOP_LENGTH < readCount; ++item)
            if (g_FileBuffer[item] == VDM_BOP0 && g_FileBuffer[item+1] == VDM_BOP1 && g_FileBuffer[item+2] == NTVDM_BOP_CMD)
                ++g_GuestNtvdmBops;
    }
    g_GuestNtAware = (wasShell && g_GuestNtvdmBops >= NT_AWARE_SHELL_BOPS_MIN);
    /* #152: Close Program has nothing to close at a shell's own prompt -- whether we
       chose the shell or something named COMMAND.COM explicitly. */
    {   INT pathLength = lstrlenA(programPathBuffer);
        g_TopIsShell = wasShell
            || (pathLength >= COMMAND_COM_LENGTH && !lstrcmpiA(programPathBuffer + pathLength - COMMAND_COM_LENGTH, HOST_COMMAND_COM)); }
    /* ── ★ THE NTVDM-AWARE SHELL IS LAUNCHED `/P <its own directory>`, AS STOCK DOES. ──
         ntvdm.exe carries `%s=%s%s /p %s\system32`; s79 found /P mattered and the bare
         launch later dropped every argument. Without /P, PERMCOM ([0x2B0]) stays 0, so
         XP's EXIT takes DOS's ordinary return-to-parent path -- which for a top-level
         shell is ITSELF, and the prompt just comes back (the sweep's "exit does not
         work"). The directory argument is COMMAND.COM's own COMSPEC location, which
         also replaces the C:\COMMAND.COM the environment otherwise names. */
    if (g_GuestNtAware && !args[0]) {
        CHAR directory[300], shortDirectory[300]; INT directoryLength = 0, cut = 0;
        for (directoryLength = 0; programPathBuffer[directoryLength] && directoryLength < (INT)sizeof directory - 1; ++directoryLength) {
            directory[directoryLength] = programPathBuffer[directoryLength]; if (programPathBuffer[directoryLength] == '\\') cut = directoryLength; }
        directory[cut ? cut : directoryLength] = 0;
        if (!GetShortPathNameA(directory, shortDirectory, sizeof shortDirectory)) LogPut(shortDirectory, directory);
        wsprintfA(args, HOST_SHELL_ARGUMENTS_FORMAT, shortDirectory);
        if (!GetShortPathNameA(programPathBuffer, g_ShellPath, sizeof g_ShellPath)) LogPut(g_ShellPath, programPathBuffer);
        cursor = LogPut(cursor, "STAGE2: NTVDM-aware shell -> command tail [");
        cursor = LogPut(cursor, args); cursor = LogPut(cursor, "] (permanent, as stock launches it)\r\n");
    }
    cursor = LogPut(cursor, "STAGE2: guest NTVDM BOP sites (C4 C4 54) = ");
    cursor = LogDecimal(cursor, g_GuestNtvdmBops);
    cursor = LogPut(cursor, g_GuestNtAware
             ? " -> NTVDM-AWARE SHELL: DOS 5.00 and the private AH=53h answers apply\r\n"
             : (wasShell ? " -> an ordinary DOS shell\r\n" : " (not loaded as a shell)\r\n"));

    /* status-bar program name = basename of programPathBuffer (if any) */
    { PCSTR baseName = programPathBuffer, scan; INT item = 0;
      for (scan = programPathBuffer; *scan; ++scan) if (*scan == '\\' || *scan == '/') baseName = scan + 1;
      if (*baseName) { while (baseName[item] && item < PROGRAM_NAME_SIZE - 1) { g_ProgramName[item] = baseName[item]; ++item; } g_ProgramName[item] = 0; } }
    *cursorIo = cursor; *readCountIo = readCount;
}


/* Become the VDM: set up its memory, register with the kernel's VDM support, give [0x714] a defined value, fetch the command from CSRSS (DOS or Win16), and take the VDM_TIB -- or stop if there is none. */
static INT StartupRegisterVdm(PSTR *cursorIo, LONG *vdmStatusIo, INT *wowCommandFromCsrssIo, CHAR *args, CHAR *programPathBuffer, DWORD *readCountIo, CHAR *report, volatile BYTE * *tibIo, INT *exitCodeOut)
{
    PSTR cursor = *cursorIo;
    LONG vdmStatus = *vdmStatusIo;
    DWORD error = 0;
    INT wowCommandFromCsrss = *wowCommandFromCsrssIo;
    DWORD readCount = *readCountIo;
    volatile BYTE * tib = *tibIo;
    /* V86 address space, then register as a VDM with the kernel (order matters). */
    VdmSetupMemory();
    vdmStatus = VdmRegisterWithKernel();
    cursor = LogPut(cursor, "STAGE1: v86_init NTSTATUS=0x"); cursor = LogHex(cursor, (UINT)vdmStatus); cursor = LogPut(cursor, "\r\n");
    cursor = StartupCheckInheritedVdmState(cursor);
    /* ── THE CLEAN 2x2. ─────────────────────────────────────────────────────────────
         The first differential compared a WOW probe HERE against a DOS probe placed
         ~500 lines later, after CSRSS and the whole DOS machine were built. That is two
         variables, not one, so "WOW is refused, DOS succeeds" did not actually follow.
         Probe BOTH launch types at BOTH points and let the 2x2 say whether it is the
         launch type or the amount of VDM setup that matters. */
    if (GetFileAttributesA(WOWTRY_FLAG) != INVALID_FILE_ATTRIBUTES)
        WowProbeLdtMatrix(g_WowModuleCount ? "wow-early" : "dos-early");
    StartupWowSelectorStage();
    /* EMS page frame must be mapped AFTER VdmInitialize (see VdmMapEmsFrame). */
    g_EmsFrameLinear = VdmMapEmsFrame();
    if (GetFileAttributesA(WOWTRY_FLAG) != INVALID_FILE_ATTRIBUTES) WowProbeLdtMatrix("B-after-emsframe");
    cursor = LogPut(cursor, "STAGE1: ems_frame lin=0x"); cursor = LogHex(cursor, g_EmsFrameLinear);
    cursor = LogPut(cursor, " seg=0x"); cursor = LogHex(cursor, g_EmsFrameLinear >> PARAGRAPH_SHIFT); cursor = LogPut(cursor, "\r\n");

    /* CSRSS: register as the console VDM, then fetch the program to run. */
    CsrssRegisterConsole();
    if (GetFileAttributesA(WOWTRY_FLAG) != INVALID_FILE_ATTRIBUTES) WowProbeLdtMatrix("C-after-csrss-register");
    StartupTakeCsrssCommand(&cursor, &error);
    /* ── ★★★★★ THE SECOND FETCH: THE COMMAND ITSELF. (s72, the package smoke test) ──
         The call above is stock ntvdm's `cmdGetStartInfo` shape (VDM_GET_FIRST_COMMAND):
         it fills Title, CurDirectory and the PIF, and nothing else -- AppName/CmdLine
         come back as capture-buffer scaffolding (`app=[5??] cmd=[\]`). Explorer puts the
         program's path in the console TITLE, which is the only reason a double-click has
         ever run the right program. A launch from cmd.exe or a batch file -- smoke.bat,
         a prompt, the friend's machine on the 18th -- has a title of "" (start) or the
         typed command WITH ITS ARGUMENTS (direct), and we ran the embedded four-byte stub
         and reported a clean exit: the package smoke test passed without running the
         self-test. Stock ntvdm consumes the real command in its exec-BOP path with a
         second GetNextVDMCommand, VDM_FLAG_DOS | VDM_FLAG_FIRST_TASK.
       ► MEASURED on the rig, both launch shapes (STAGE1: fetch2 lines, s72):
           AppName = C:\DOCUME~1\...\bm\selftest.com   (full short path, AppLen incl. NUL)
           CmdLine = "hello world\r\n"                     (the tail ONLY; "\r\n" when none)
           CurDirectory = the launcher's cwd; Env = its Win32 environment block (0x46c);
           ComingFromBat = 1 from a batch file; TaskId 0 without -i is fine.
         DONT_WAIT so a protocol misunderstanding is a FALSE with an error, never a hang.
         Only after a successful first fetch: on a WOW launch the first returns FALSE
         (err 0x57) and that tell is left exactly as it was. */
    if (g_CurrentDirectory[0] || g_Title[0]) {
        cursor = StartupFetchCommandDetails(cursor);
    } else if (g_WowLaunch) {
        StartupFetchWowCommand(&cursor, &wowCommandFromCsrss, args, programPathBuffer, &readCount);
    }
    LogWrite(LOG_PATH, report, cursor);
    /* ⚠ AFTER the LogWrite, not before: LogWrite TRUNCATES. The first cut of this
         ran the probe earlier and its output was silently erased by this very line,
         which looked exactly like "the probe never ran". Same stale/truncated-artefact
         trap this project keeps paying for, in a new costume. */
    if (GetFileAttributesA(WOWTRY_FLAG) != INVALID_FILE_ATTRIBUTES)
        WowProbeLdtMatrix("D-after-getcommand");   /* before VdmGetTib */
    tib = VdmGetTib();
    if (GetFileAttributesA(WOWTRY_FLAG) != INVALID_FILE_ATTRIBUTES) WowProbeLdtMatrix("E-after-get-tib");
    g_TibDebug = tib;                                    /* let the crash VEH dump guest state */
    if (!tib) {
        cursor = LogPut(cursor, "STAGE1: no VDM_TIB -- abort\r\n"); LogAppend(LOG_PATH, report, cursor);
        { *cursorIo = cursor; *vdmStatusIo = vdmStatus; *wowCommandFromCsrssIo = wowCommandFromCsrss; *readCountIo = readCount; *tibIo = tib; *exitCodeOut = 1; return HOST_FLOW_RETURN; }
    }
    *cursorIo = cursor; *vdmStatusIo = vdmStatus; *wowCommandFromCsrssIo = wowCommandFromCsrss; *readCountIo = readCount; *tibIo = tib; return HOST_FLOW_NEXT;
}


/* Start the log and its COM1 mirror, take stdio, run the recovery counter (safe mode after repeated failed starts), read Settings and the cfg\ knobs, install the fault handlers, and log the STAGE0 state. */
static INT StartupConfigure(PSTR *cursorIo, CHAR *report, INT *exitCodeOut)
{
    PSTR cursor = *cursorIo;
    /* ── NO MODAL HARDWARE-ERROR BOXES, EVER, FOR THE WHOLE PROCESS. ───────────────
         Every Win32 call that touches a drive with no media -- A: with the door
         open, an ejected CD -- raises XP's "There is no disk in drive" box unless
         told not to, and that box has wedged the rig from inside host start-up
         once already. Now that the guest can select and search those drives
         (INT 21h AH=0Eh, 47h, 4Eh...), the mode must cover every call, not just
         the two sites that wrapped it. The errors still come back as errors. */
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    cursor = LogPut(cursor, "NTVDMEX clean host\r\nSTAGE0: WinMain entered [build dpmi-harness-v180]\r\n");
    LogWrite(LOG_PATH, report, cursor);
    SerialInitialize();                                      /* DPMI harness: COM1 log sink */
    g_StdioHow = StdioInitialize();                         /* GH #131; reported at exit */
    {   UINT fails = RecoveryRead();               /* GH #132 */
        g_StartMode = DosRecoveryDecideStartMode(fails);
        RecoveryWrite(fails + 1);                      /* cleared only on a clean exit */
        cursor = LogPut(cursor, "STAGE0: consecutive failed starts = "); cursor = LogHexByte(cursor, fails);
        cursor = LogPut(cursor, g_StartMode == DOS_START_UNINSTALL ? " -> UNINSTALL\r\n"
                  : g_StartMode == DOS_START_SAFE      ? " -> SAFE MODE\r\n"
                                                        : " -> normal\r\n");
        if (g_StartMode == DOS_START_UNINSTALL) RecoveryUninstall(&cursor);
        g_Safe = DosRecoveryGetSafeSkips(g_StartMode);
        if (g_StartMode == DOS_START_SAFE)
            cursor = LogPut(cursor, "STAGE0: SAFE MODE skips: third-party VDDs, audio output (silent"
                        " pump), the real PC speaker, the joystick thread, the WOW"
                        " shims, fullscreen -- the next clean exit clears it\r\n");
        /* s90: NOT `p = report` -- the next LogWrite TRUNCATES the file and re-writes
           the report buffer, so a line dropped from the buffer here was lost from
           EVERY log: "consecutive failed starts" never survived since #132 landed. */
        LogAppend(LOG_PATH, report, cursor); SerialOut(report, cursor); }
    SerialOut(report, cursor);

    {
        INT exitCode, flow = StartupLatchWowLaunch(&cursor, report, &exitCode);
        if (flow == HOST_FLOW_RETURN) { *cursorIo = cursor; *exitCodeOut = exitCode; return HOST_FLOW_RETURN; }
    }
    /* Headless test mode = the SMB watcher dropped the AUTOEXIT marker. In that mode the
       host must self-exit on guest exit AND bound any infinite run (a visual demo like
       pm32irq/animate never calls INT 21h 4Ch), else rt.bat's `start /wait` blocks forever
       and wedges the watcher (session-9). Latch it once here (the exit path deletes the marker). */
    /* ── SETTINGS FIRST, TEST FILES SECOND. ─────────────────────────────────────
         Load the stored configuration here, at the TOP of the knob block, so every
         file knob below it still overrides. That ordering is the whole contract (see
         settings.h): the rig configures this host by writing files and re-launching,
         and a setting clicked in a dialog on that machine must never silently change
         what a headless measurement is measuring. */
    SettingsLoad(&g_Settings);
    g_SettingsDisk = g_Settings;          /* nothing has overridden anything yet */
    SettingsApply(NULL, &g_Settings, SETTINGS_APPLY_STARTUP);
    /* Log only the LIVE settings -- the ones the SettingsApply* functions actually
       push into the machine. The stored-but-not-yet-honoured ones would make this
       line four times longer and every value in it would be a claim the run cannot
       support. Two lines because there are now enough of them to wrap. */
    cursor = LogPut(cursor, "STAGE0: settings hidecur=retired(#218)");
    cursor = LogPut(cursor, " blink=");   cursor = LogHex(cursor, g_Settings.Values[SET_BLINKCURSOR]);
    cursor = LogPut(cursor, " msens=");   cursor = LogHex(cursor, g_Settings.Values[SET_MSENS]);
    cursor = LogPut(cursor, " dosver=");  cursor = LogHex(cursor, g_Settings.Values[SET_DOSMAJ]);
    cursor = LogPut(cursor, ".");         cursor = LogHex(cursor, g_Settings.Values[SET_DOSMIN]);
    cursor = LogPut(cursor, " pitpace="); cursor = LogHex(cursor, g_Settings.Values[SET_PITPACE]);
    cursor = LogPut(cursor, " uitick=");  cursor = LogHex(cursor, g_Settings.Values[SET_UITICK]);
    cursor = LogPut(cursor, "\r\n");
    cursor = LogPut(cursor, "STAGE0: settings vol=");  cursor = LogHex(cursor, g_Settings.Values[SET_VOLUME]);
    cursor = LogPut(cursor, " mute=");      cursor = LogHex(cursor, g_Settings.Values[SET_MUTE]);
    cursor = LogPut(cursor, " spk=");       cursor = LogHex(cursor, g_Settings.Values[SET_SPEAKER]);
    cursor = LogPut(cursor, " outhz=");     cursor = LogHex(cursor, SettingsOutputHz(&g_Settings));
    cursor = LogPut(cursor, " sb=A");       cursor = LogHex(cursor, g_SbConfig.IoBase);
    cursor = LogPut(cursor, " I");          cursor = LogHex(cursor, g_SbConfig.Irq);
    cursor = LogPut(cursor, " D");          cursor = LogHex(cursor, g_SbConfig.Dma8Channel);
    cursor = LogPut(cursor, " H");          cursor = LogHex(cursor, g_SbConfig.Dma16Channel);
    cursor = LogPut(cursor, " xms=");       cursor = LogHex(cursor, (DWORD)g_XmsOn);
    cursor = LogPut(cursor, " ems=");       cursor = LogHex(cursor, (DWORD)g_EmsOn);
    cursor = LogPut(cursor, " winsize=");   cursor = LogHex(cursor, g_Settings.Values[SET_WINSIZE]);
    cursor = LogPut(cursor, " scaler=");    cursor = LogHex(cursor, g_Settings.Values[SET_SCALER]);
    cursor = LogPut(cursor, " aspect=");    cursor = LogHex(cursor, g_Settings.Values[SET_ASPECT]);
    cursor = LogPut(cursor, " filter=");    cursor = LogHex(cursor, g_Settings.Values[SET_FILTER]);
    cursor = LogPut(cursor, " vsync=");     cursor = LogHex(cursor, g_Settings.Values[SET_VSYNC]);
    cursor = LogPut(cursor, " frameskip="); cursor = LogHex(cursor, g_Settings.Values[SET_FRAMESKIP]);
    cursor = LogPut(cursor, "\r\n");
    /* ── ★★★ THE TIMING LANDSCAPE, IN EVERY LOG. (s63) ──────────────────────────────
         Skyroads' frame pacing is fragile on a 2-core box and has regressed THREE
         times, each time because a change quietly added background work or moved a
         priority and nobody re-checked. So make the invariants AUDITABLE: the pacer
         priority MUST read 0x0 (THREAD_PRIORITY_NORMAL); the s61 regression was it
         sitting at HIGHEST (0x2), which let the pacer preempt the guest. joy_thread
         MUST read 0x0 whenever JoystickType is None, because a non-joystick game
         must run with no poll thread at all. A regression in either now shows up in
         the first twenty lines of every run, not two months later on a user's
         screen. (Priority constants: NORMAL=0, ABOVE_NORMAL=1, HIGHEST=2,
         BELOW_NORMAL=-1=0xffffffff, LOWEST=-2.) */
    cursor = LogPut(cursor, "STAGE0: timing: pacer_prio="); cursor = LogHex(cursor, (DWORD)g_PitPacePriority);
    cursor = LogPut(cursor, " (want 0x0=NORMAL) joytype=");  cursor = LogHex(cursor, (DWORD)g_Joystick.Type);
    cursor = LogPut(cursor, " joy_thread=");  cursor = LogHex(cursor, (DWORD)g_JoystickThreadStarted);
    cursor = LogPut(cursor, " (want 0x0 when joytype=0x0) pit_split=1\r\n");

    g_Headless = (GetFileAttributesA(AUTOEXIT_PATH) != INVALID_FILE_ATTRIBUTES);
    /* Self-screenshot only when explicitly requested (graphical tests) AND headless, so
       the common non-graphical tests never enter the capture path. Latched once here. */
    { HANDLE modeHandle = CreateFileA(MODEY_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL, OPEN_EXISTING, 0, NULL);
      if (modeHandle != INVALID_HANDLE_VALUE) {
          CHAR modeYText[32]; DWORD modeYRead = 0, modeYValue = 0, modeYIndex; INT got = 0;
          ReadFile(modeHandle, modeYText, sizeof modeYText - 1, &modeYRead, NULL); CloseHandle(modeHandle);
          for (modeYIndex = 0; modeYIndex < modeYRead && modeYText[modeYIndex] >= '0' && modeYText[modeYIndex] <= '9'; ++modeYIndex) { modeYValue = modeYValue * DECIMAL_RADIX + (DWORD)(modeYText[modeYIndex] - '0'); got = 1; }
          if (got && modeYValue <= MODEY_GAP_MAX_U) {
              CHAR dwordsLine[96], *lineCursor = dwordsLine;
              g_Video.ModeYGap = modeYValue;
              lineCursor = LogPut(lineCursor, "STAGE0: modey.txt -> gap="); lineCursor = LogHex(lineCursor, modeYValue);
              lineCursor = LogPut(lineCursor, " dwords\r\n"); LogAppend(LOG_PATH, dwordsLine, lineCursor); SerialOut(dwordsLine, lineCursor);
          }
      } }
    g_Capture  = g_Headless && (GetFileAttributesA(CAPTURE_FLAG) != INVALID_FILE_ATTRIBUTES);
    if (g_Capture) {                       /* its contents, if any, are the period in ms */
        HANDLE configHandle = CreateFileA(CAPTURE_FLAG, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                NULL, OPEN_EXISTING, 0, NULL);
        if (configHandle != INVALID_HANDLE_VALUE) {
            CHAR captureText[32]; DWORD captureRead = 0, captureValue = 0, captureIndex;
            ReadFile(configHandle, captureText, sizeof captureText - 1, &captureRead, NULL); CloseHandle(configHandle);
            for (captureIndex = 0; captureIndex < captureRead && captureText[captureIndex] >= '0' && captureText[captureIndex] <= '9'; ++captureIndex)
                captureValue = captureValue * DECIMAL_RADIX + (DWORD)(captureText[captureIndex] - '0');
            if (captureValue >= CAPTURE_MS_MIN && captureValue <= CAPTURE_MS_MAX) g_CaptureMs = captureValue;
            {   DWORD periodDelay = 0;                                 /* #58: "period delay" */
                while (captureIndex < captureRead && captureText[captureIndex] == ' ') ++captureIndex;
                for (; captureIndex < captureRead && captureText[captureIndex] >= '0' && captureText[captureIndex] <= '9'; ++captureIndex)
                    periodDelay = periodDelay * DECIMAL_RADIX + (DWORD)(captureText[captureIndex] - '0');
                if (periodDelay <= CAPTURE_DELAY_MS_MAX) g_CaptureDelayMs = periodDelay; }
            g_CaptureStart = GetTickCount();
        }
    }
    /* ⚠ THESE BELONG WITH THE OTHER STARTUP FLAGS, NOT IN THE DPMI BLOCK. Read from
         inside the protected-mode setup they applied to Doom and not to QBasic --
         nomouse worked for one guest and silently did nothing for the other, and
         textdump wrote no files at all for a real-mode run. A knob that only some
         launches honour is worse than no knob. */
    g_TextDump = (GetFileAttributesA(TEXTDUMP_PATH) != INVALID_FILE_ATTRIBUTES);
    g_MouseAbsent = (GetFileAttributesA(NOMOUSE_PATH) != INVALID_FILE_ATTRIBUTES);
    g_NoA000  = (GetFileAttributesA(NOA000_FLAG) != INVALID_FILE_ATTRIBUTES);
    g_NoPmPatch = (GetFileAttributesA(NOPMPATCH_FLAG) != INVALID_FILE_ATTRIBUTES);
    if (g_NoPmPatch) {                    /* contents, if any, = minimum region size */
        HANDLE noPatchHandle = CreateFileA(NOPMPATCH_FLAG, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                NULL, OPEN_EXISTING, 0, NULL);
        if (noPatchHandle != INVALID_HANDLE_VALUE) {
            CHAR noPatchText[32]; DWORD noPatchBytesRead = 0, noPatchValue = 0, noPatchIndex;
            ReadFile(noPatchHandle, noPatchText, sizeof noPatchText - 1, &noPatchBytesRead, NULL); CloseHandle(noPatchHandle);
            for (noPatchIndex = 0; noPatchIndex < noPatchBytesRead; ++noPatchIndex) {
                INT hexDigit = (noPatchText[noPatchIndex] >= '0' && noPatchText[noPatchIndex] <= '9') ? noPatchText[noPatchIndex] - '0'
                       : (noPatchText[noPatchIndex] >= 'a' && noPatchText[noPatchIndex] <= 'f') ? noPatchText[noPatchIndex] - 'a' + HEX_DIGIT_A_VALUE
                       : (noPatchText[noPatchIndex] >= 'A' && noPatchText[noPatchIndex] <= 'F') ? noPatchText[noPatchIndex] - 'A' + HEX_DIGIT_A_VALUE : -1;
                if (hexDigit < 0) break;
                noPatchValue = (noPatchValue << NIBBLE_SHIFT) | (DWORD)hexDigit;
            }
            g_NoPmPatchMinimum = noPatchValue;
        }
    }
    {   HANDLE modeHandle = CreateFileA(MEMDUMP_FLAG, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                NULL, OPEN_EXISTING, 0, NULL);
        if (modeHandle != INVALID_HANDLE_VALUE) {         /* "<linear> <size>" in hex */
            CHAR memoryDumpText[48]; DWORD memoryDumpBytesRead = 0, memoryDumpIndex, values[MEMDUMP_FIELDS] = {0, 0}, width = 0; INT isIn = 0;
            ReadFile(modeHandle, memoryDumpText, sizeof memoryDumpText - 1, &memoryDumpBytesRead, NULL); CloseHandle(modeHandle);
            for (memoryDumpIndex = 0; memoryDumpIndex < memoryDumpBytesRead && width < MEMDUMP_FIELDS; ++memoryDumpIndex) {
                INT hexDigit = (memoryDumpText[memoryDumpIndex] >= '0' && memoryDumpText[memoryDumpIndex] <= '9') ? memoryDumpText[memoryDumpIndex] - '0'
                       : (memoryDumpText[memoryDumpIndex] >= 'a' && memoryDumpText[memoryDumpIndex] <= 'f') ? memoryDumpText[memoryDumpIndex] - 'a' + HEX_DIGIT_A_VALUE
                       : (memoryDumpText[memoryDumpIndex] >= 'A' && memoryDumpText[memoryDumpIndex] <= 'F') ? memoryDumpText[memoryDumpIndex] - 'A' + HEX_DIGIT_A_VALUE : -1;
                if (hexDigit < 0) { if (isIn) { ++width; isIn = 0; } continue; }
                values[width] = (values[width] << NIBBLE_SHIFT) | (DWORD)hexDigit; isIn = 1;
            }
            g_MemoryDumpLinear = values[0]; g_MemoryDumpLength = values[1];
        }
    }
    g_Interp12 = (GetFileAttributesA(INTERP12_FLAG) != INVALID_FILE_ATTRIBUTES);
    g_ModeYInterpOffset = (GetFileAttributesA(MYINTERP_OFF_FLAG) != INVALID_FILE_ATTRIBUTES);
    g_ModeYRingOn    = (GetFileAttributesA(MYRING_FLAG) != INVALID_FILE_ATTRIBUTES);
    /* The GUS is decided HERE, with its resources, because the environment block is
       built before the devices are added -- and ULTRASND= has to say what the card
       will be. Deciding it at device setup left the first heaven7 run with no ULTRASND
       and a card nothing looked for. */
    g_GusOn = g_Settings.Values[SET_GUS] && (GetFileAttributesA(NOGUS_FLAG) == INVALID_FILE_ATTRIBUTES);
    if (g_Settings.Values[SET_GUS] && !g_GusOn) SettingsNoteOverride(SET_GUS, CFG_TEXT(KNOB_FILE_NOGUS), 0);
    if (GetFileAttributesA(DDRAWFS_FLAG) != INVALID_FILE_ATTRIBUTES)   /* read again at fullscreen */
        SettingsNoteOverride(SET_RENDERER, CFG_TEXT(KNOB_FILE_DDRAWFS), 1);
    /* #235: the card as the Audio page's jumpers set it (defaults = the card as built). */
    {   static const BYTE gusIrqs[7] = { 2, 3, 5, 7, 11, 12, 15 };
        static const BYTE gusDmaChannels[5] = { 1, 3, 5, 6, 7 };
        g_Gus.BasePort   = (WORD)(GUS_BASE_FIRST + GUS_BASE_STEP * (g_Settings.Values[SET_GUSADDR] <= GUS_BASE_LAST_CHOICE ? g_Settings.Values[SET_GUSADDR] : GUS_DEFAULT_BASE_CHOICE));
        g_Gus.Irq    = gusIrqs[g_Settings.Values[SET_GUSIRQ] <= ARRAYSIZE(gusIrqs) - 1 ? g_Settings.Values[SET_GUSIRQ] : GUS_DEFAULT_IRQ_CHOICE];
        g_Gus.DmaChannel = gusDmaChannels[g_Settings.Values[SET_GUSDMA] <= ARRAYSIZE(gusDmaChannels) - 1 ? g_Settings.Values[SET_GUSDMA] : GUS_DEFAULT_DMA_CHOICE]; }
    /* ...and OFF THE SOUND BLASTER'S RESOURCES. The SB's own choices in the dialog
       include 240h, IRQ 11 and DMA 3 -- each of them the GUS default -- and two cards on
       one line is a machine nobody could have built. Step aside to the next period
       choice (ref/gus.md §5 lists what the latches can select). */
    if (g_SbConfig.IoBase == g_Gus.BasePort) g_Gus.BasePort = GUS_FALLBACK_BASE;
    if (g_SbConfig.Irq == g_Gus.Irq)   g_Gus.Irq = GUS_FALLBACK_IRQ;
    if (g_SbConfig.Dma8Channel == g_Gus.DmaChannel || g_SbConfig.Dma16Channel == g_Gus.DmaChannel) g_Gus.DmaChannel = GUS_FALLBACK_DMA;
    if (g_SbConfig.Dma8Channel == g_Gus.DmaChannel || g_SbConfig.Dma16Channel == g_Gus.DmaChannel) g_Gus.DmaChannel = GUS_SECOND_FALLBACK_DMA;
    g_ModeYPmOffset     = (GetFileAttributesA(MYPM_OFF_FLAG) != INVALID_FILE_ATTRIBUTES);
    g_ModeYPmDetect  = (GetFileAttributesA(MYPM_DETECT_FLAG) != INVALID_FILE_ATTRIBUTES);
    g_P12Offset  = (GetFileAttributesA(P12OFF_FLAG)   != INVALID_FILE_ATTRIBUTES);
    g_OplTraceOn = (GetFileAttributesA(OPLTRACE_FLAG) != INVALID_FILE_ATTRIBUTES);
    if (g_OplTraceOn) g_Opl.Trace = OplTraceWrite;
    if (g_Interp12) g_NoA000 = 1;              /* interpreting instead of trapping */
    /* Headless cap override (decimal ms on the share). Read before the deadline thread
       starts, since that thread sleeps on it. Clamped: below the default a typo would
       kill runs early, above 10 min a typo would wedge the watcher for the whole time. */
    { HANDLE handle = CreateFileA(HEADLESS_MS_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             NULL, OPEN_EXISTING, 0, NULL);
      if (handle != INVALID_HANDLE_VALUE) {
          CHAR text[16]; DWORD bytesRead = 0, number = 0; INT index;
          ReadFile(handle, text, sizeof text, &bytesRead, NULL);
          CloseHandle(handle);
          for (index = 0; index < (INT)bytesRead; ++index) {
              if (text[index] < '0' || text[index] > '9') break;      /* stop at CR/LF/junk */
              number = number * DECIMAL_RADIX + (DWORD)(text[index] - '0');
          }
          if (number > PM_HEADLESS_MS_DEFAULT && number <= HEADLESS_MS_MAX) g_HeadlessMs = number;   /* s84: an hour, for slow-rung timedemos */
      } }
    StartupLoadQiMode();
    if (g_QiBits || g_QiSuspended) {    /* async delivery needs a handle to the exec thread */
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                        &g_HostCpu, 0, FALSE, DUPLICATE_SAME_ACCESS);
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
         contention: slicing HostAudioFill's hold into 64-frame pieces moved
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
    { HANDLE versionHandle = CreateFileA(DSPVER_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL, OPEN_EXISTING, 0, NULL);
      if (versionHandle != INVALID_HANDLE_VALUE) {
          CHAR text[16]; DWORD bytesRead = 0; INT index = 0, major = 0, minor = 0;
          ReadFile(versionHandle, text, sizeof text, &bytesRead, NULL);
          CloseHandle(versionHandle);
          while (index < (INT)bytesRead && text[index] >= '0' && text[index] <= '9') major = major * DECIMAL_RADIX + (text[index++] - '0');
          while (index < (INT)bytesRead && (text[index] == ' ' || text[index] == '.')) ++index;
          while (index < (INT)bytesRead && text[index] >= '0' && text[index] <= '9') minor = minor * DECIMAL_RADIX + (text[index++] - '0');
          if (major > 0 && major < BYTE_VALUES) { g_SbVersionMajor = (BYTE)major; g_SbVersionMinor = (BYTE)minor;
                                    g_DspVersionForced = 1; }
      } }
    { HANDLE gateHandle = CreateFileA(SBGATE_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL, OPEN_EXISTING, 0, NULL);
      if (gateHandle != INVALID_HANDLE_VALUE) {
          CHAR text[8]; DWORD bytesRead = 0;
          ReadFile(gateHandle, text, sizeof text, &bytesRead, NULL); CloseHandle(gateHandle);
          g_SbGate = (bytesRead && text[0] >= '0' && text[0] <= '9') ? (text[0] - '0') : 1;
      } }
    { DWORD priority = EXECPRIO_ABOVE_NORMAL;
      HANDLE priorityHandle = CreateFileA(EXECPRIO_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL, OPEN_EXISTING, 0, NULL);
      if (priorityHandle != INVALID_HANDLE_VALUE) {
          CHAR text[8]; DWORD bytesRead = 0;
          ReadFile(priorityHandle, text, sizeof text, &bytesRead, NULL);
          CloseHandle(priorityHandle);
          if (bytesRead && text[0] >= '0' && text[0] <= '9') priority = (DWORD)(text[0] - '0');
      }
      g_ExecPriority = priority;
      if (priority == EXECPRIO_ABOVE_NORMAL) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
      else if (priority >= EXECPRIO_HIGHEST) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
      g_ExecPriorityForeground = GetThreadPriority(GetCurrentThread());
      DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                      &g_ExecThread, 0, FALSE, DUPLICATE_SAME_ACCESS);   /* #211: BackgroundPriorityTick */
    }
    HostProfileStart();                                 /* #183: cfg\hostprof.flag */
    if (g_QiBits) {
        /* Experiment mode: retarget the kernel's PIC so a KERNEL-dispatched IRQ 5 arrives
           as INT 65h while our own injection still arrives as INT 0Dh. Without this the
           two are the same vector and the qirq2 probe cannot attribute a delivery. */
        VdmIcaSetBase(QIMODE_PIC_BASE);
    }
    cursor = LogPut(cursor, "STAGE0: qi_bits=0x"); cursor = LogHex(cursor, g_QiBits);
    cursor = LogPut(cursor, " qi_raise=0x");       cursor = LogHex(cursor, (DWORD)g_QiRaise);
    cursor = LogPut(cursor, " qi_vif=0x");         cursor = LogHex(cursor, (DWORD)g_QiVif);
    cursor = LogPut(cursor, " qi_susp=0x");        cursor = LogHex(cursor, (DWORD)g_QiSuspended);
    cursor = LogPut(cursor, " hcpu=0x");           cursor = LogHex(cursor, (DWORD)(ULONG_PTR)g_HostCpu);
    cursor = LogPut(cursor, "\r\n");
    cursor = LogPut(cursor, "STAGE0: os=0x"); cursor = LogHex(cursor, ((g_OsVersion & BYTE_MASK) << BYTE_SHIFT) | ((g_OsVersion >> BYTE_SHIFT) & BYTE_MASK));
    cursor = LogPut(cursor, " build="); cursor = LogDecimal(cursor, (g_OsVersion < OS_VERSION_NOT_NT_U) ? (g_OsVersion >> WORD_SHIFT) : 0);
    cursor = LogPut(cursor, " veh="); cursor = LogDecimal(cursor, g_PfnAddVeh != 0);
    cursor = LogPut(cursor, " attachconsole="); cursor = LogDecimal(cursor, g_PfnAttachConsole != 0);
    cursor = LogPut(cursor, " rawinput="); cursor = LogDecimal(cursor, g_PfnRegisterRawInput && g_PfnGetRawInput);
    cursor = LogPut(cursor, g_PfnAddVeh ? "\r\n" : "  (no VEH: the unhandled filter runs the PM-fault arms)\r\n");
    if (g_PfnAddVeh) g_PfnAddVeh(1, DpmiCrashVeh);  /* DPMI spike crash diagnostic; XP+ */
    SetUnhandledExceptionFilter(HostUnhandledFilter); /* real-mode runs: full dump, not WER */

    /* CSRSS command-info: receive buffers + first-command state + IFEO task id. */
    g_CommandInfo.CmdLine = g_CommandLine; g_CommandInfo.CmdLen = sizeof(g_CommandLine);
    g_CommandInfo.AppName = g_Application; g_CommandInfo.AppLen = sizeof(g_Application);
    g_CommandInfo.PifFile = g_PifPath; g_CommandInfo.PifLen = sizeof(g_PifPath);
    g_CommandInfo.CurDirectory = g_CurrentDirectory; g_CommandInfo.CurDirectoryLen = sizeof(g_CurrentDirectory);
    g_CommandInfo.Env = g_Environment; g_CommandInfo.EnvLen = sizeof(g_Environment);
    g_CommandInfo.Desktop = g_Desktop; g_CommandInfo.DesktopLen = sizeof(g_Desktop);
    g_CommandInfo.Title = g_Title; g_CommandInfo.TitleLen = sizeof(g_Title);
    g_CommandInfo.Reserved = g_Reserved; g_CommandInfo.ReservedLen = sizeof(g_Reserved);
    g_CommandInfo.StartupInfo.cb = sizeof(STARTUPINFOA);
    g_CommandInfo.VDMState = VDM_GET_FIRST_COMMAND;
    g_CommandInfo.TaskId   = CsrssParseTaskId(GetCommandLineA());

    /* ── WHAT SHAPE OF LAUNCH IS THIS? (GH #129) ────────────────────────────────
         Windows launches ntvdm.exe for BOTH a DOS program and a 16-bit WINDOWS
         program -- WOW runs inside the same VDM binary. Our IFEO Debugger hook
         therefore intercepts both, and we implement only the DOS half, so a Win16
         launch currently lands in a host that cannot load an NE file at all.
       ► Before deciding anything from the command line, RECORD IT. The flags that
         distinguish the two are described in various places and this project has
         been bitten repeatedly by building on a documented claim instead of a
         measured one. Log the raw string; diff a DOS launch against a Win16 launch
         on the rig; write the detector against what the diff actually shows. */
    cursor = LogPut(cursor, "STAGE0: root=["); cursor = LogPut(cursor, NTVDMEX_DIR); cursor = LogPut(cursor, "] (derived from the host's own path)\r\n");
    HmaTry();
    cursor = LogPut(cursor, "STAGE0: HMA ");
    if (g_Hma) cursor = LogPut(cursor, "committed at 0x100000 -- FFFF:0010 is real");
    else { cursor = LogPut(cursor, "UNAVAILABLE err=0x"); cursor = LogHex(cursor, g_HmaError);
       cursor = LogPut(cursor, " state=0x"); cursor = LogHex(cursor, g_HmaState);
       cursor = LogPut(cursor, " prot=0x");  cursor = LogHex(cursor, g_HmaProtection); }
    cursor = LogPut(cursor, "\r\n");
        cursor = LogPut(cursor, "STAGE0: cmdline=["); cursor = LogPut(cursor, GetCommandLineA()); cursor = LogPut(cursor, "]\r\n");
    *cursorIo = cursor; return HOST_FLOW_NEXT;
}

INT WINAPI WinMain(HINSTANCE instance, HINSTANCE previousInstance, LPSTR commandLineText, INT showCommand)
{
    CHAR report[8192]; PSTR cursor = report; PSTR base;
    PCSTR const reportEnd = report + sizeof report;   /* the guards below keep a line's worth short of it */
    INT wowCommandFromCsrss = 0;         /* s73: the Win16 program came from CSRSS, not target.txt */
    volatile BYTE *tib;
    DWORD readCount = 0; LONG vdmStatus;
    DOS_IMAGE image;
    DOS_MACHINE machine;
    CHAR dosOutput[16384];   /* M9 probe dumps run to several KB; 1024 truncated them */
    CHAR programPathBuffer[768]; CHAR args[256];
    INT guard;
    g_GuestThreadId = GetCurrentThreadId();
    OsCompatBind();                    /* the four XP-only imports, or their absence */
    /* cfg\ and debug\out\ before ANYTHING logs. A missing out\ makes every LogAppend
       fail silently, and the log is what explains every other failure. Idempotent;
       debug\ first because CreateDirectoryA does not create intermediate levels. */
    CreateDirectoryA(NTVDMEX_CFG, NULL);
    CreateDirectoryA(NTVDMEX_DEBUG, NULL);
    CreateDirectoryA(NTVDMEX_OUT, NULL);
    /* s90 (#278): THE WOW32.DLL / NTVDM.EXE STAND-INS GO IN FIRST, before anything in
         this process can touch winmm. winmm asks "am I under WOW?" ONCE and caches the
         answer (0x76b616ec), and the host's own audio and timer code loads winmm early:
         loaded at the WOW branch below, the shims arrived after the question had been
         answered "no", and NotifyCallbackData kept returning 0 (runs/s90/sr4). */
    if (LaunchIsWow(GetCommandLineA())) WowShimsLoad();

    {
        INT exitCode, flow = StartupRunInstallVerb(&exitCode);
        if (flow == HOST_FLOW_RETURN) return exitCode;
    }

    /* ── ★★★★★ NOTHING ON THE COMMAND LINE = THE USER OPENED NTVDMEX. (s79) ──────────
         Sits here for the same reason the verbs do: above every launch guard, and safe
         because a real VDM launch always has arguments. Before this, a double-click
         reached STAGE1, was refused VDM privilege (`NtVdmControl` -> 0xC0000022) and
         vanished without a window or a message -- the worst possible answer to "open it
         and see". */
    if (CommandLineBare(GetCommandLineA())) return LaunchShellVdm();

    {
        INT exitCode, flow = StartupClaimInstance(&exitCode);
        if (flow == HOST_FLOW_RETURN) return exitCode;
    }
    HANDLE uiThread = NULL;

    (VOID)instance; (VOID)previousInstance; (VOID)commandLineText; (VOID)showCommand;
    programPathBuffer[0] = 0; args[0] = 0;

    /* The install verbs ran far above, ahead of the single-instance guard -- see the
       block after the CreateDirectory calls, and the defect note there. */

    {
        INT exitCode, flow = StartupConfigure(&cursor, report, &exitCode);
        if (flow == HOST_FLOW_RETURN) return exitCode;
    }

    {
        INT exitCode, flow = StartupRegisterVdm(&cursor, &vdmStatus, &wowCommandFromCsrss, args, programPathBuffer, &readCount, report, &tib, &exitCode);
        if (flow == HOST_FLOW_RETURN) return exitCode;
    }

    StartupLoadProgram(&cursor, &readCount, programPathBuffer, args, wowCommandFromCsrss);
    /* No flag to raise: the UI tick polls g_ProgramName and repaints the strip when it
       changes. See StatusUpdate. */

    StartupBuildDos(&cursor, readCount, &image, programPathBuffer, args, &machine, report);

    cursor = StartupAttachDevices(cursor);
    StartupLoadTuningKnobs();
    cursor = StartupStartServices(cursor);
    StartupConnectDosToHost(&machine);

    StartupStartGuest(&cursor, &base, &uiThread, &machine, &image, tib, report);

    /* Service loop: run V86 until a BOP, dispatch INT 21h, step past the BOP, re-enter.
       Runs until the guest terminates, a hard stop, or the window closes (g_Running);
       no iteration cap so interactive/animated programs keep going. */
    machine.Tib = tib; machine.Output = dosOutput; machine.OutputCapacity = sizeof(dosOutput); machine.OutputLength = 0; machine.IsOutputTruncated = 0;
    g_Machine = &machine;              /* the watchdog flushes this if the run wedges */
    (VOID)guard;
    HostRunExecLoop(&cursor, base, &machine, tib, &vdmStatus, programPathBuffer);

    /* Log the exec-loop exit before any flushing, so a hang or fault during
       shutdown is distinguishable from the loop never exiting at all. */
    cursor = LogPut(cursor, "STAGE2: exec loop exited -> flushing\r\n");
    LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;

    /* The exec loop is out: tell the headless backstop a clean shutdown is under
       way so it does not force-exit us mid-flush and lose the DOS output. */
    InterlockedExchange(&g_WoundDown, 1);

    g_CommandInfo.ExitCode = (ULONG)machine.ExitCode;
    /* Flush captured DOS output to the console + the log.
     ► IF WE STREAMED IT LIVE, DO NOT PRINT IT AGAIN. g_Stdio carries the output
       as the guest produces it now (GH #131), so the historical bulk write to
       CONOUT$ would DOUBLE every line -- and would do it into the redirect
       target, where it is not merely ugly but wrong. Flush whatever is still in
       the line buffer instead. The log copy below is unconditional either way:
       it is a different sink and the one the rig harness reads. */
    StdioFlush();
    cursor = TaskReportExitToCsrss(cursor, base, &machine);
    cursor = ReportStartMode(cursor);
    PcSpeakerClose(&g_PcSpeaker);               /* ⚠ a headless run never sees WM_DESTROY,
                                            and Beep.sys outlives the process */
    RecoveryOk();                       /* GH #132: this run ended cleanly */
    cursor = ReportStdoutAndDosOutput(cursor, &machine);
    cursor = ReportEndOfRun(cursor, base, reportEnd, &machine, tib);

    /* Headless test mode: the guest has just terminated (or the PM loop hit its
       headless time cap), so exit immediately (no window-close needed) -- this lets a
       test harness's `start /wait` return and the log be collected. Consume the marker
       so it's one-shot; interactive runs (no marker) keep the window open. */
    if (g_Headless) {
        DeleteFileA(AUTOEXIT_PATH);
        TrayRemove(g_Window);     /* ExitProcess runs no window cleanup -- see below */
        HostRecordFinish();
        ExitProcess(0);
    }
    /* ── ★★★★ A WIN16 HOST MUST NOT OUTLIVE ITS GUEST. (session 56) ────────────
         REPORTED BY THE USER: "when a WoW16 window exits, it leaves its tray icon
         behind. They are stacking up in the tray."
       ⚠⚠ I FIRST WROTE THAT THESE WERE NOT GHOSTS BUT LIVE PROCESSES. THE USER
         REFUTED IT WITH ONE OBSERVATION: "they all disappear when the mouse
         hovers over them", which is the textbook signature of a GHOST -- Explorer
         reaps a tray icon only after its owner is dead, and only lazily, when the
         mouse passes over it. A live process's icon does not do that.
         Measured afterwards, and it is not close: FIVE icons in the tray with
         `tasklist` reporting ZERO ntvdmhost.exe. They are ghosts.
       ★ BOTH FACTS ARE REAL AND THEY COMPOSE. The lingering host is measured too
         (launch CALC, click its X, the window is gone and PID 1488 is still
         there) -- that is the bug this arm and wowuser.h's WM_DESTROY fix. But a
         LINGERING host is what the NEXT `taskkill /f /im ntvdmhost.exe` then
         kills, and every launch script runs one, against ALL instances. An
         externally terminated process cannot run NIM_DELETE, so each one becomes
         a ghost. Demonstrated end to end: 1 host + 6 icons -> taskkill -> 0 hosts
         and still 6 icons.
       ⇒ So the icons were ghosts, the lingering hosts were what got ghosted, and
         the fix for both is the same: a host that exits WITH its guest never
         needs killing.
       ⚠ The cause is the line below this one, and it was right for the case it
         was written for and wrong for this one. "Keep the window open so the
         guest's final screen stays visible until the user closes it" assumes
         there IS a window and a final screen. A Win16 guest has NEITHER -- the
         note by TrayAdd says so in as many words: it gets no VDM window, only a
         tray icon. So there was nothing to look at and nothing to close, and the
         wait never ended. The user could only reach it through the tray menu's
         Exit, which is exactly the step they were never going to take twenty
         times a session.
       ⇒ For a Win16 host the guest exiting IS the end of the run. Ask the UI
         thread to close, and let it leave through its OWN path -- WM_CLOSE ->
         DestroyWindow -> WM_DESTROY -> PostQuitMessage -> the message loop
         returns -> TrayRemove + PresentDdrawShutdown. That is strictly better
         than ExitProcess here, because WM_DESTROY is also what stops the OPL and
         Beep.sys, and both of those have outlived a host before. */
    /* ── ★ AUTO-CLOSE ON GUEST TERMINATION, FOR EVERY GUEST. (s63, user ask) ────────
         This was WOW-only; a DOS guest that exited left its window open showing a
         dead final frame until the user closed it by hand. But reaching here means
         the guest we were asked to run has TERMINATED -- for a game, quitting it
         exits the program and lands exactly here -- so the run is over and there is
         nothing to interact with. Closing through WM_CLOSE -> WM_DESTROY runs
         HostPanicRelease(), which is also THE FIX FOR THE CAPTURE-EXIT TRAP: a
         guest that exited while it held the mouse used to leave the pointer clipped
         to a dead window with no way to escape but the keyboard chord. Same path for
         DOS and Win16 now; the s63 UiThread TerminateProcess then guarantees the
         process is gone. */
    if (g_Window) PostMessageA(g_Window, WM_CLOSE, 0, 0);
    if (uiThread) { WaitForSingleObject(uiThread, INFINITE); CloseHandle(uiThread); }
    return 0;
}
