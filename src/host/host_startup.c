/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Start-up: WinMain's steps -- configuring, registering the VDM, loading the program, building DOS, attaching the devices, starting the guest.
 *
 * Part of the host's single translation unit: #included by main.c.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

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

/* LOG_PATH now lives in log.h -- see the note there. */
/* Both are written by the runner and READ by us, so they are cfg, not out. */
#define TARGET_PATH         CFG_("target.txt")

/* A DOS shell to run when NOTHING named a program. NOT a target: target.txt names THE
 * test and is consulted first; this is the last resort. See the STAGE2 block.
 */
#define SHELL_PATH          CFG_("shell.txt")

/* #208: present = load a program started from Windows directly (the pre-#208 way)
 * instead of handing it to XP's COMMAND.COM. An A/B switch, not a setting.
 */
#define DIRECTLAUNCH_FLAG   CFG_("directlaunch.flag")

/* Opt-in screenshot flag. Lives on the SMB SHARE folder so the remote driver can
 * toggle it (create it before a GRAPHICAL test, remove it otherwise). Non-graphical
 * tests (selftest/dpmitest) then never touch the self-capture path -- keeping the
 * common case off the capture code entirely.
 */
#define CAPTURE_FLAG        CFG_("capture.flag")

/* Mode-Y de-interleave tuning; see modey_flush() in vdd_video.c. Contents = the run
 * coalescing slack in dwords. Absent = the built-in default.
 */
#define MODEY_PATH          CFG_("modey.txt")

/* A hex VRAM byte offset. Every planar write to it is recorded with the registers
 * that produced it and the guest CS:IP -- see the watchpoint in vdd_video.c.
 */
#define VWATCH_PATH         CFG_("vwatch.txt")

/* Per-plane backing for mode Y is ON by default -- see the MODE-Y PLANE BACKING block.
 * This file DISABLES it and falls back to the de-interleave heuristic, which is worth
 * keeping only because it is what a machine that refuses the remap will use.
 */
#define SBDUMP_FLAG         CFG_("sbdump.flag")

/* North star 2: present = no Gravis UltraSound (no device, no ULTRASND= in the env). */
#define NOGUS_FLAG          CFG_(KNOB_FILE_NOGUS)

/* s81: record the audio output (audio_rec.h via AudioWaveRecord*). The flag is the harness's
 * switch; Tools > Capture > Record Audio will drive the same recorder.
 */
#define WAVREC_FLAG         CFG_("wavrec.flag")

#define WAVREC_PATH         OUT_("capture_audio.wav")

#define NOREMAP_FLAG        CFG_("noremap.flag")

/* Diagnostic knob: disable the mode-12h A0000 NOACCESS trap. With it off, planar
 * writes land in the raw aperture instead of the VGA engine, so the PICTURE will
 * be wrong -- the question it answers is whether the guest EXECUTES AT ALL.
 * Absent = normal behaviour, so ordinary runs are untouched. Delete after use.
 */
#define NOA000_FLAG         CFG_("noa000.flag")

/* TURN THE PM INT-SITE PATCHER OFF. (s74, diagnostic):
 * The patcher rewrites `CD nn` -> `C4 C4` in a client's declared code region so a
 * protected-mode INT becomes a BOP we can service; a raw `CD nn` in PM is the one
 * fault XP will not reflect. But the region is whatever the CLIENT calls code, and
 * in a guest that GENERATES tables or code at runtime a stray 0xCD is just data --
 * which is how this has now broken guests five times (Doom's jump table four, then
 * heaven7). Present = scan nothing, so a silent death can be A/B'd against the
 * patcher in one run instead of being argued about. Absent = normal behaviour.
 *
 * [CAUTION]: It is a DIAGNOSTIC, not a fix: with it on, a guest that really does execute
 * `CD nn` in PM dies differently. Judge it on whether the guest gets FURTHER.
 */
#define NOPMPATCH_FLAG      CFG_("nopmpatch.flag")

/* DUMP A GUEST LINEAR RANGE AT THE HEADLESS DEADLINE. (s74b, diagnostic):
 * Contents: two hex numbers, "<linear> <size>". Written to debug\out\memdump.bin while
 * the guest is still mapped -- the way to READ A PACKED GUEST (heaven7 unpacks itself
 * into its 0501 block, so its strings and tables exist only in memory). Absent = off.
 */
#define MEMDUMP_FLAG        CFG_("memdump.flag")

/* Optional CONTENTS of nopmpatch.flag: a hex byte count. Regions at least that big
 * are not scanned; smaller ones are patched as usual. Empty = skip every region.
 * Why a SIZE: DOS/4GW's own PM code region is ~0x5000 bytes of dense, real
 * `mov ah,N / int 21h`, and it NEEDS patching (a raw CD in PM is the one fault XP
 * will not reflect). heaven7's is the whole 0x3a000 LE allocation -- code object
 * AND data object AND everything it generates into them -- and needs not to be.
 * Size separates the two without naming an address, which changes run to run.
 */
/* Mode 12h WITHOUT the A0000 page trap (GH #55). Arming that trap stops the V86
 * guest running at all -- 10 I/O events in 30s against 22.5 MILLION with it off.
 * But we do not actually need it: in mode 12h QuickBASIC reprograms a VGA
 * register via OUT between pixels, so the PORT traps alone hand us control
 * constantly, and the batching interpreter can then run the pixel loop with its
 * A0000 stores going through the planar engine. This knob keys the interpreter
 * off "planar mode is active" instead of "the page is protected".
 */
#define INTERP12_FLAG       CFG_("interp12.flag")

/* Escape hatch for the planar policy (GH #55): present = go back to the A0000
 * page trap. Interpreting the guest for the whole time a planar mode is set is
 * the DEFAULT because the page trap demonstrably freezes the guest on real
 * hardware; this knob exists so the old path is still one file away.
 */
#define P12OFF_FLAG         CFG_("p12off.flag")

/* North star 1 (s80): present = do NOT interpret mode-Y multi-plane / latch windows,
 * i.e. go back to the scratch + fan-out approximation. The A/B and rollback lever.
 */
#define MYINTERP_OFF_FLAG   CFG_("modeyinterp_off.flag")

/* Present = record the last 64 mode-Y interpreted instructions (s80's crash finder). */
#define MYRING_FLAG         CFG_("myring.flag")

/* North star 1 for PROTECTED-mode guests (Doom): present = do not interpret its drawers. */
#define MYPM_OFF_FLAG       CFG_("modeypm_off.flag")

/* Present = keep the scratch window while a PM guest runs NATIVELY under a multi-plane
 * mask, so any store its own code makes there is counted (fanN) instead of assumed away.
 */
#define MYPM_DETECT_FLAG    CFG_("modeypm_detect.flag")

/* GH #128: opt into the EXPERIMENTAL WOW load probe on a Win16 launch. Absent (the
 * default) the host still refuses Win16 loudly -- an experiment must never become the
 * shipped behaviour by accident.
 */
#define WOWTRY_FLAG         CFG_("wowtry.flag")

/* Dev-only: capture the exact OPL register stream a game sends, with timestamps,
 * so it can be replayed offline through BOTH our synth and a reference core and
 * the audio diffed. Counting register writes cannot say WHY an instrument sounds
 * wrong; comparing waveforms from identical input can. Absent = no cost at all.
 */
#define OPLTRACE_FLAG       CFG_("opltrace.flag")

#define QIMODE_PATH         CFG_("qimode.txt")

/* FIXED_NTVDMSTATE ([0x714]) initial value override, hex, up to 8 digits. Absent = 0.
 * Exists so the rig can try a different starting word without a rebuild -- see the
 * note at the VdmRegisterWithKernel call for why the word must be INITIALISED at all.
 */
#define VDMSTATE_PATH       CFG_("vdmstate.txt")

/* Headless wall-clock cap override, decimal milliseconds, also on the share. The 30 s
 * default is right for an unattended test that must not wedge the watcher, but an
 * INTERACTIVE test on the box -- keylog, where a human walks over and presses every key
 * -- needs minutes, and the default would kill the guest mid-typing. Absent = the
 * default.
 */
#define HEADLESS_MS_PATH    CFG_("headless_ms.txt")

#define AWBUFS_PATH         CFG_("awbufs.txt")

#define AWFRAMES_PATH       CFG_("awframes.txt")

#define EXECPRIO_PATH       CFG_("execprio.txt")

#define DSPVER_PATH         CFG_("dspver.txt")

#define SBGATE_PATH         CFG_("sbgate.txt")

#define PITPACE_PATH        CFG_(KNOB_FILE_PITPACE)

#define PITPRIO_PATH        CFG_("pitprio.txt")

#define PITINJ_PATH         CFG_("pitinj.txt")

#define UITICK_PATH         CFG_(KNOB_FILE_UITICK)

/* courier.txt = 0 turns the tick courier off (see TickCourierThread). 1 = as shipped. */
#define COURIER_PATH        CFG_("courier.txt")

/* llkbd.txt = 1 re-enables the SYSTEM-WIDE low-level keyboard hook. OFF by default --
 * see InputCaptureSet for why it is the single most dangerous thing this host does.
 */
#define LLKBD_PATH          CFG_("llkbd.txt")

/* HOW LONG A BLOCKED Win16 TASK WAITS. (GH #128, session 43) (Importance = 1):
 * Milliseconds, decimal; **0 means FOREVER**, which is what a real Win16 task
 * does and what an INTERACTIVE session needs -- a program sitting in GetMessage
 * with its window on the desktop is not stuck, it is waiting for the user, and
 * a host that quits it after six seconds makes it impossible to type into.
 * Absent = WOWMSG_WAIT_MS, the bound a harness run needs so that a guest which
 * will never receive anything still lets the run finish.
 */
#define WOWIDLE_PATH        CFG_("wowidle.txt")

#define KEYIRQ_PATH         CFG_("keyirq.txt")

#define MSENS_PATH          CFG_(KNOB_FILE_MSENS)

/* THE CPU-SPEED CALIBRATION, AS A FILE. (GH #56) (Importance = 1):
 * Decimal MHz: how fast an UNTHROTTLED host looks to a DOS program on THIS box.
 * Every speed on the menu is a fraction of it, so it is the one number that makes
 * "33 MHz" mean 33 MHz here, and it is wrong on somebody else's machine by
 * construction -- a faster box presents more. Measured by cpubench.asm; absent =
 * CPUSPEED_REF_MHZ_DEFAULT. A knob, so re-calibrating is a run rather than a build.
 */
#define CPUREF_PATH         CFG_("cpuref.txt")

/* Decimal index into g_CpuSpeedMhz, overriding the registry for one run. This is how
 * the rig sweeps every speed in a single batch without touching HKCU.
 */
#define CPUSPD_PATH         CFG_(KNOB_FILE_CPUSPD)

/* Throttle granularity (target run/hold period, ms). 0/absent = auto-detect. */
#define CPUGRAN_PATH        CFG_("cpugran.txt")

/* cpuaff.txt = 1 -> pin the guest to a core of its own (see CpuAffinityApply). */
#define CPUAFF_PATH         CFG_("cpuaff.txt")

#define DOSVER_PATH         CFG_(KNOB_FILE_DOSVER)

/* INT 21h AH=53h's private AL sub-functions. A knob because the measured values are
 * measured IN A CONTEXT (a probe whose stdout was redirected) and at least AL=5 is
 * suspected of depending on it -- see dos_int21.c. One row per line:
 *     <AL hex> <AX hex> <CF 0|1>        e.g.  05 5300 0
 * `;` or `#` starts a comment; absent rows keep the built-in measured default.
 */
#define INT53_PATH          CFG_("int53.txt")

/* Extra guest environment variables, one NAME=VALUE per line; '#' comments a line.
 * See DosEnvBuildWithCard for why this exists -- a DOS program configured through its
 * environment could not be configured at all before it.
 */
#define DOSENV_PATH         CFG_("dosenv.txt")

#define DOSTRACE_FLAG   CFG_("dostrace.flag")

/* The XMS pool, in KB. Named because SysVars+0x45 must report the SAME
 * number to MEM.EXE (GH #47) -- two literals would drift.
 */
/* #48: THE POOL IS THE MACHINE'S EXTENDED MEMORY LESS THE HMA, AS HIMEM'S IS:
 * This was 16384 on a machine whose INT 15h AH=88h, CMOS 17h/18h and 30h/31h and
 * AH=87h address space (dos_extmem.h, 1..16 MB) all say 15360 KB of extended memory:
 * XMS handed out more memory than the machine has, and SysVars+0x45 reported the
 * pool, so MEM and AH=88h disagreed (#48). HIMEM on 6.22 reports the extended memory
 * minus the 64 KB HMA it keeps for itself -- the oracle's MEM: total 15,232K, XMS free
 * 15,168K (runs/s81_mem/oracle_memd.txt). Derived from CMOS_EXTENDED_KB so the four views
 * (88h, CMOS, SysVars+0x45, XMS) are one number and cannot drift again.
 *
 * [CAUTION]: AN OBSERVABLE CHANGE: XMS AH=08h now says 15296 KB, 1088 KB less than before. No
 * oracle pins the old figure (xms-ems.md: 08h's size is per machine, abstained), and
 * DPMI memory does not come from this pool -- but every XMS client sees it.
 */
#define XMS_HMA_KB      64

#define XMS_POOL_KB     (CMOS_EXTENDED_KB - XMS_HMA_KB)     /* 15296 */

#define DPMI_PMRET_BOP      0x56

/* INT 31h 0306 RAW MODE SWITCH (Doom/DOS/4GW needs it -- it tests CF from 0306 and
 * `jmp`s to its abort path when the call fails, which is exactly where it died).
 * Spec (DPMI 1.0 0306): returns BX:CX = real-to-protected entry, SI:(E)DI =
 * protected-to-real entry. Both are entered by FAR JMP -- not call -- with
 *   AX = new DS, CX = new ES, DX = new SS, (E)BX = new (E)SP,
 *   SI = new CS, (E)DI = new (E)IP
 * (E)BP is preserved across the switch; FS/GS read 0 afterwards; the other GPRs are
 * undefined. So each entry is just a BOP the host traps and completes by rewriting
 * the CONTEXT -- there is no return address to honour, which is why a FAR JMP is
 * safe. NB offset 0x58 is DOS_IRET_STUB_OFF and BOP 0x57 is DPMI_FAULT_BOP; these
 * take the next free slots in both namespaces.
 */
#define DPMI_RAW2PM_BOP     0x58    /* Real -> protected (entered in V86) */

#define DPMI_RAW2RM_BOP     0x59    /* Protected -> real (entered in PM) */

/* EMS (M4): the LIM page frame is a 64KB RAM window in the UMA. VdmMapEmsFrame
 * scans the conventional page-frame segments AFTER VdmInitialize for a free 64KB
 * hole and maps it there; g_EmsFrameLinear holds the linear base actually chosen
 * (0 => none found, EMS unavailable). The guest learns the segment via AH=41.
 */
#define EMS_POOL_PAGES      512     /* 512 * 16KB = 8MB of EMS */

/* "THERE IS NO MOUSE ON THIS MACHINE":
 * INT 33h AX=0000h answers AX=0 and every other function is left alone, which is
 * what a DOS program sees with no driver loaded. It exists to take the mouse --
 * and with it Doom's twice-a-frame DPMI real-mode simulation -- OUT of a run, so
 * "is that path involved in this crash at all" becomes one measurement instead of
 * a series of guesses. A configuration, not a debug hack: a machine without a
 * mouse is a machine a guest has to cope with.
 */
#define NOMOUSE_PATH    CFG_("nomouse.flag")

#define TEXTDUMP_PATH   CFG_("textdump.flag")   /* shotNN.txt beside shotNN.bmp */

/* -- REFLECT DPMI 0300 TO THE GUEST'S OWN REAL-MODE HANDLER -- ON BY DEFAULT (s81).
 * It was off (simintrefl.flag to enable) because it wedged ZAR waiting on an SB
 * completion the nested V86 call never delivered. s81 fixed that, and the spec says
 * 0300 runs the real-mode handler, so it is on; simintrefl_off.flag is the opt-out.
 * - #247: "on" now means EVERY vector -- our stubs too -- runs from the IVT (RmcsSimIntRoute
 *   in dpmi_rmcs.h). The flag restores the pre-#247 ROUTING: 21h/33h/10h host-side,
 *   everything else not run. (Not the pre-#247 marshalling: the full register write-back
 *   and the INT 21h carry stay fixed either way.) The rig's rollback lever for the routing.
 */
#define SIMINTREFL_OFF_FLAG     CFG_("simintrefl_off.flag")

#define LIVEHB_FLAG     CFG_("livehb.flag")

#define PITLATCH_FLAG   CFG_("pitlatch.flag")

#define CPU_REFERENCE_MHZ_MAX_U     100000u

#define CPU_REFERENCE_MHZ_MIN_U     1u

enum
{
    EXTENDER_ARGV0_SAFE_LENGTH = 62, ENVIRONMENT_DUMP_BYTES = 0xC0, ENVIRONMENT_DUMP_LINE = 264, ENVIRONMENT_DUMP_NULS = 2, ENVIRONMENT_DUMP_SKIP = 8, COMMAND_TAIL_DUMP_BYTES = 16, UI_TICK_MS_MAX = 100, MOUSE_SENSITIVITY_MIN = 10, MOUSE_SENSITIVITY_MAX = 1000
};   /* WinMain: argv[0], the guest dumps, knob ranges */

#define WOW_KRNL386_ENTRY_AX    0x4B4F          /* 'OK': what krnl386 expects in AX at entry */

#define FILE_TYPE_NOT_ASKED_U   0xFFFFFFFFu     /* The handle report: no handle to ask about */

enum
{
    NT_AWARE_SHELL_BOPS_MIN = 8, ROUTED_PATH_MAX = 120, WOW_COMMAND_DIRECTORY_MAX = 0x10C, SHELL_HEADER_READ = 0x44, COMMAND_COM_LENGTH = 11, STD_HANDLE_REPORTS = 5
};   /* WinMain's launch path */

#define MODEY_GAP_MAX_U         65536u

#define OS_VERSION_NOT_NT_U     0x80000000u     /* GetVersion: the high bit is set on Windows 9x */

enum
{
    EXECPRIO_ABOVE_NORMAL = 1, EXECPRIO_HIGHEST = 2
};   /* execprio.txt: the exec thread's priority (0 = left alone) */

enum
{
    QIMODE_DIGITS = 2, QIMODE_RAISE = 0x04, QIMODE_VIF = 0x08, QIMODE_KEYS = 0x20, QIMODE_NO_SUSPEND = 0x40, QIMODE_KEYS_ASYNC = 0x80, QIMODE_PIC_BASE = 0x60
};   /* qimode.txt bits (QIMODE_PATH) */

enum
{
    INT53_FIELD_AL = 0, INT53_FIELD_AX = 1, INT53_FIELD_CARRY = 2, INT53_FIELDS = 3, MEMDUMP_FIELDS = 2
};   /* int53.txt: "<AL> <AX> <CF>"; memdump.flag: "<linear> <size>" */

/* Interrupt vectors a third-party VDD claimed (claim_int): a user vector gets one of the generic stubs;
 * a system vector, or one past the last stub, is not delivered -- and the log says which.
 */
static VOID StartupReportVectorWiring(VOID)
{
    /* -- s91 (#315): EVERY CLAIMED VECTOR GETS A WAY IN. A claim only reached its device
     * where the host had wired a stub by number (10h 14h 16h 1Ah 08h 2Ah 5Ch), so a
     * third-party driver's claim_int on any other vector -- the SDK promises it --
     * was never delivered. Each such claim now gets a generic stub, and the IVT
     * points at it. [CAUTION] Only the vectors nobody else owns: the user vectors 60h-66h,
     * 68h-6Fh and 78h-FEh. A claim on a DOS, BIOS or IRQ vector is logged and
     * refused rather than silently stealing it from the system.
     */
    {   UINT number, count = 0;
        volatile BYTE *controlBytes = (volatile BYTE *)(ULONG_PTR)((DWORD)DOS_CTAB_SEG << PARAGRAPH_SHIFT);

        for (number = 0; number < IVT_VECTORS; ++number)
        {
            INT wired = (number == VECTOR_TIMER || number == VECTOR_VIDEO || number == VECTOR_SERIAL || number == VECTOR_KEYBOARD_SERVICES || number == VECTOR_TIME
                         || number == VECTOR_NETWORK || number == VECTOR_NETBIOS);
            INT user  = (number >= VECTOR_USER_RANGE1_FIRST && number <= VECTOR_USER_RANGE1_LAST) || (number >= VECTOR_USER_RANGE2_FIRST && number <= VECTOR_USER_RANGE2_LAST)
                        || (number >= VECTOR_USER_RANGE3_FIRST && number <= VECTOR_USER_RANGE3_LAST);
            CHAR gateLine[120];
            CHAR *gateCursor = gateLine;

            if (!g_Bus.Interrupts[number].Service || wired)
                continue;

            gateCursor = LogPut(gateCursor, "  VDD: claim_int 0x"); gateCursor = LogHex(gateCursor, number);

            if (!user || count >= DOS_GENSTUB_N)
            {
                gateCursor = LogPut(gateCursor, user ? " -- no generic stub left; NOT delivered\r\n"
                                   : " -- a system vector; NOT delivered (user vectors only)\r\n");
            }
            else
            {
                UINT offset = DOS_GENSTUB_OFF + count * DOS_GENSTUB_SIZE;
                controlBytes[offset + 0] = VDM_BOP0;
                controlBytes[offset + 1] = VDM_BOP1;
                controlBytes[offset + VDM_BOP_NUMBER_OFFSET] = DOS_GENSTUB_BOP;
                controlBytes[offset + VDM_BOP_LENGTH] = X86_OP_IRET;            /* IRET */
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
    if (g_WowEntering)
    {
        /* VdmSetEntry points DS/ES/FS/GS at the PSP and zeroes AX, which is right
         * for a DOS program and wrong for this one. krnl386 wants DS = its automatic
         * data segment, and it expects AX = 0x4b4f -- 'OK' -- at entry; with anything
         * else it returns at once with AX=0. Get AX wrong and it returns instantly,
         * which would read as "the entry did nothing" rather than "we failed a
         * handshake". Measured at the entry breakpoint; see session 30 part 5.
         */
        VDM_SET16(tib, VTIB_DS, g_WowEntryDs);
        /* [INFO]: AND ES, WHICH IS NOT COSMETIC: krnl386 takes ES+0x10 as the base of the
         * DPMI host's private data and carves every later allocation upward from
         * there without asking DOS. VdmSetEntry points ES at DOS_PSP_SEG, whose
         * +0x10 is where the (discarded) DOS image sat and where DosMcbAllocate had
         * already placed krnl386's own code. Point it at the arena block instead.
         */
        if (g_WowPspSegment)
            VDM_SET16(tib, VTIB_ES, g_WowPspSegment);

        /* [INFO]: CX = HOW MUCH MEMORY IS AVAILABLE ABOVE THE STACK, IN BYTES.
         * krnl386 takes CX at entry, as a byte count, for the size of the block its
         * selector over base(SS)+SP describes (observed: the arena it then uses is
         * CX >> 4 paragraphs). Measured at three breakpoints, CX was 0 all the way
         * from entry, so krnl386 believed it had ZERO paragraphs and every
         * allocation out of that arena failed -- including one inside LoadSegment,
         * which is why it could not load its own segment 1 and exited.
         * The selector has a 64 KB limit, so this is the whole of it minus the header
         * image we place at its base. Nothing else names this quantity to the guest.
         */
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

      if (cpuSpeedFile != INVALID_HANDLE_VALUE)
      {
          /* [CAUTION]: THIS READ ONE CHARACTER, SO HALF THE LADDER WAS UNREACHABLE (Importance =
           * 2): `c[0] - '0'` cannot express an index above 9, and the ladder went to 17 in session
           * 54 when it grew from 7 entries to 18. So every speed from 75 MHz down -- 75, 66, 50,
           * 33, 25, 16, 12, 8, which is ALL of the period-hardware settings and every one a person
           * would actually reach for -- silently selected the FIRST DIGIT instead: `13` (33 MHz)
           * ran as index 1, i.e. 3300 MHz, and reported itself as doing so.
           *
           * [CAUTION]: THE RANGE CHECK HID IT rather than catching it. `c[0] < '0' +
           * CPUSPEED_COUNT` with COUNT=18 accepts characters up to 'A', so a
           * two-digit value passed the guard on its first digit and was accepted
           * as a valid index -- a bounds test that admits exactly the input it
           * should have rejected.
           *
           * [INFO]: MEASURED 2026-09-09: `echo 13 > cpuspd.txt` came back
           * `STAGE2: cpuspeed idx=00000001 mhz=00000ce4` -- 3300 MHz. The rig
           * sweep this knob exists to drive therefore never tested the slow half
           * of the ladder even once, and cpuswp.bat only ever swept 0-6.
           * - The CPUREF reader four lines above already does it correctly. Same
           *   loop here; there is no reason for two adjacent knobs to disagree.
           *
           * [CAUTION]: This is the FILE knob only. The menu and the Settings dialog set the
           * index directly and were never affected, so it is a testability defect
           * and not the cause of any speed a user has seen.
           */
          CHAR text[8];
          DWORD bytesRead = 0;
          UINT value9 = 0;
          INT index9;
          ReadFile(cpuSpeedFile, text, sizeof text, &bytesRead, NULL);
          CloseHandle(cpuSpeedFile);

          for (index9 = 0; index9 < (INT)bytesRead; ++index9) { if (text[index9] < '0' || text[index9] > '9')
              break;
                                             value9 = value9 * DECIMAL_RADIX_U + (UINT)(text[index9] - '0'); }

          if (index9 > 0 && value9 < (UINT)CPUSPEED_COUNT)
          {
              g_CpuSpeedIndex = (INT)value9;
              SettingsNoteOverride(SET_SPEEDMODE, CFG_TEXT(KNOB_FILE_CPUSPD), value9);
          }
      } }
}

/* cfg\vwatch.txt: a planar watchpoint at a video RAM offset, when the file is present. */
static VOID StartupLoadVideoWatch(VOID)
{
    /* [CAUTION]: AFTER VddBusAdd, NOT BEFORE. VddBusAdd calls VddVideoInitialize, which
     * disarms the watchpoint -- setting it first looked right and was silently
     * undone, and the run came back with no trace and no error.
     */
    /* cfg/vwatch.txt: a hex VRAM byte offset to record every planar write to. Off
     * unless the file is there -- see the watchpoint in vdd_video.c.
     */
    { HANDLE watchHandle = CreateFileA(VWATCH_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL, OPEN_EXISTING, 0, NULL);

      if (watchHandle != INVALID_HANDLE_VALUE)
      {
          CHAR watchText[32];
          DWORD watchBytesRead = 0;
          DWORD watchValue = 0;
          DWORD watchIndex;
          INT gotWatch = 0;
          ReadFile(watchHandle, watchText, sizeof watchText - 1, &watchBytesRead, NULL);
          CloseHandle(watchHandle);

          for (watchIndex = 0; watchIndex < watchBytesRead; ++watchIndex)
          {
              INT digit = -1;
              CHAR digitCharacter = watchText[watchIndex];

              if (digitCharacter >= '0' && digitCharacter <= '9')
                  digit = digitCharacter - '0';
              else if (digitCharacter >= 'a' && digitCharacter <= 'f')
                  digit = digitCharacter - 'a' + HEX_DIGIT_A_VALUE;
              else if (digitCharacter >= 'A' && digitCharacter <= 'F')
                  digit = digitCharacter - 'A' + HEX_DIGIT_A_VALUE;

              if (digit < 0)
                  break;

              watchValue = (watchValue << NIBBLE_SHIFT) | (DWORD)digit;
              gotWatch = 1;
          }

          if (gotWatch)
          {
              CHAR watchLine[96];
              CHAR *watchCursor = watchLine;
              g_Video.WatchOffset = watchValue;
              watchCursor = LogPut(watchCursor, "STAGE0: vwatch.txt -> planar watchpoint at VRAM offset 0x");
              watchCursor = LogHex(watchCursor, watchValue); watchCursor = LogPut(watchCursor, "\r\n");
              LogAppend(LOG_PATH, watchLine, watchCursor); SerialOut(watchLine, watchCursor);
          }
      } }
}

/* Build DOS's drive tables -- the DPBs and the CDS -- for the drive letters that exist (GetLogicalDrives),
 * capped at what the reserved space holds, and the device chain from the NUL device.
 */
static VOID StartupBuildDriveTables(
    CHAR *report,
    volatile BYTE * const sysVars,
    DOS_MACHINE *machine)
{
    /* THE REAL CHAINS: DPB, CDS AND THE DEVICE HEADER. (GH #48) (Importance = 1):
     * Until now everything above +0x20 was deliberately zero, and that choice was
     * right while there was nothing truthful to put there: a walker that follows
     * a garbage DPB pointer wanders into nonsense, whereas a null one stops.
     * But it caps what memory- and disk-aware software can do, and #47 names it
     * as a likely reason MEM.EXE lies.
     * - EVERY OFFSET AND EVERY STRUCTURE SIZE HERE WAS DUMPED OFF MS-DOS 6.22 by
     *   tests/probes/dos/p_sysvar.asm and decoded in src/dos/dos_sysvars.h -- which is
     *   what #48 asks for in as many words, because "the layout dumps have twice
     *   caught errors that a plausible reading would have missed". The DPB being 33
     *   bytes, for instance, is not recalled: 6.22's first DPB is at 0116:136A and
     *   its own `next` pointer says 0116:138B, and 0x138B - 0x136A = 0x21.
     * - The structures live in a block taken from the MCB chain rather than in the
     *   resident filler, which has under 800 bytes free and cannot hold a
     *   LASTDRIVE-long CDS array. Real DOS's are resident too, so the memory it
     *   costs is honest rather than an accounting trick.
     */
    {   volatile BYTE *controlTable = (volatile BYTE *)(DOS_CTAB_SEG << PARAGRAPH_SHIFT);
        DWORD drives = GetLogicalDrives();
        UINT drive;
        UINT count = 0;
        UINT slot[DOS_DPBCHAIN_MAX];
        UINT driveCount = 0;
        PSTR scan = report;
        /* Which drive letters exist, capped at what the reserved space holds.
         * Counted FIRST, because each DPB's `next` pointer has to name the one
         * after it and the last must terminate -- and a chain that does not
         * terminate is not a cosmetic fault. The same mistake on the SFT chain
         * had krnl386 reading the IVT as an SFT header and looping through
         * 117 MB of DPMI calls (see DOS_SFT_* in dos_layout.h).
         */
        /* [CAUTION]: SUPPRESS THE HARDWARE-ERROR DIALOG FIRST, AND SKIP REMOVABLES.
         * GetDiskFreeSpaceA("A:\\") on a machine whose floppy drive is empty
         * raises XP's "There is no disk in drive A:" box and BLOCKS on it --
         * at host startup, with no window up and nothing in the log, so it
         * presents as a hang and not as an error. It wedged the rig on the
         * first run of this code. SEM_FAILCRITICALERRORS makes the call fail
         * instead of asking, and DRIVE_REMOVABLE is skipped outright: a DPB
         * for a drive whose media can vanish is not worth the risk here.
         */
        UINT previousErrorMode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
        UINT driveTypes[DOS_DRIVE_LETTERS];
        /* s71: EVERY DRIVE THE MACHINE HAS, CLASSIFIED ONCE:
         * Fixed and RAM disks get a DPB measured off the volume; a REMOVABLE drive
         * (A:) gets a DPB with floppy defaults and its media is NEVER touched here
         * (the "no disk in drive A:" box is modal and wedged the rig once); CD-ROM
         * and network drives are redirector drives -- a CDS entry with the network
         * flag and no DPB, which is how MSCDEX and a redirector present them.
         */
        for (drive = 0; drive < DOS_DRIVE_LETTERS; ++drive)
        {
            CHAR rootText[4];
            driveTypes[drive] = DRIVE_NO_ROOT_DIR;

            if (!(drives & (1u << drive)))
                continue;

            rootText[0] = (CHAR)('A' + drive);
            rootText[1] = ':';
            rootText[2] = '\\';
            rootText[3] = 0;
            driveTypes[drive] = GetDriveTypeA(rootText);

            if (driveTypes[drive] != DRIVE_FIXED && driveTypes[drive] != DRIVE_RAMDISK
                && driveTypes[drive] != DRIVE_REMOVABLE)
                continue;

            if (driveCount < DOS_DPBCHAIN_MAX)
                slot[driveCount++] = drive;
        }

        for (count = 0; count < driveCount; ++count)
        {
            DWORD sectorsPerCluster = 0;
            DWORD bytesPerSector = 0;
            DWORD freeClusters = 0;
            DWORD totalClusters = 0;
            CHAR root[4];
            UINT item;
            BYTE driveParameterBlock[DOS_DPB_LEN];
            INT last = (count + 1 == driveCount);
            root[0] = (CHAR)('A' + slot[count]);
            root[1] = ':';
            root[2] = '\\';
            root[3] = 0;
            /* #48: a removable drive gets 6.22's OWN 1.44M floppy DPB -- 224 root entries,
             * media F0h -- which DosDpbBuild now reproduces byte for byte (sysvars_test).
             * It had 512 entries and F8h (the fixed-disk values) on a floppy.
             */
            INT isRemovable = (driveTypes[slot[count]] == DRIVE_REMOVABLE);

            if (isRemovable)                                           /* 1.44M defaults, no probe */
                {
                    sectorsPerCluster = 1;
                    bytesPerSector = DOS_DPB_DEFAULT_SECTOR_SIZE;
                    totalClusters = DOS_FLOPPY_144_CLUSTERS;
                }
            else if (!GetDiskFreeSpaceA(root, &sectorsPerCluster, &bytesPerSector, &freeClusters, &totalClusters))
                {
                    sectorsPerCluster = DOS_UNMEASURED_SECTORS_PER_CLUSTER;
                    bytesPerSector = DOS_DPB_DEFAULT_SECTOR_SIZE;
                    totalClusters = DOS_UNMEASURED_CLUSTERS;
                }

            DosDpbBuild(driveParameterBlock, slot[count], bytesPerSector ? bytesPerSector : DOS_DPB_DEFAULT_SECTOR_SIZE, sectorsPerCluster ? sectorsPerCluster : 1, isRemovable ? DOS_FLOPPY_144_ROOT_ENTRIES : DOS_FIXED_ROOT_ENTRIES,
                          (totalClusters > DOS_DPB_CLUSTER_LIMIT) ? DOS_DPB_CLUSTER_LIMIT : totalClusters + 1, isRemovable ? DOS_MEDIA_FLOPPY_144 : DOS_MEDIA_FIXED,
                          DOS_DEV_SEG, DOS_DEVICE_OFFSET(DOS_DEVICE_BLOCK),       /* #48: the block driver */
                          last ? DOS_CHAIN_END : DOS_CTAB_SEG,
                          last ? DOS_CHAIN_END : (WORD)(DOS_DPBCHAIN_OFF + (count + 1) * DOS_DPB_LEN));

            for (item = 0; item < DOS_DPB_LEN; ++item)
                controlTable[DOS_DPBCHAIN_OFF + count * DOS_DPB_LEN + item] = driveParameterBlock[item];
        }

        if (driveCount)
        {
            *(volatile WORD *)(sysVars + DOS_SYSVARS_OFF + DOS_SYSVARS_DPB)     = DOS_DPBCHAIN_OFF;
            *(volatile WORD *)(sysVars + DOS_SYSVARS_OFF + DOS_SYSVARS_DPB + X86_FAR_POINTER_SEGMENT) = DOS_CTAB_SEG;
        }
        else                                    /* no chain is better than a bad one */
        {
            *(volatile WORD *)(sysVars + DOS_SYSVARS_OFF + DOS_SYSVARS_DPB)     = DOS_CHAIN_END;
            *(volatile WORD *)(sysVars + DOS_SYSVARS_OFF + DOS_SYSVARS_DPB + X86_FAR_POINTER_SEGMENT) = DOS_CHAIN_END;
        }

        *(volatile WORD *)(sysVars + DOS_SYSVARS_OFF + DOS_SYSVARS_MAX_SECTOR) = DOS_DPB_DEFAULT_SECTOR_SIZE;
        /* SYSVARS+0x45: EXTENDED MEMORY, IN KB. THE ANSWER TO GH #47 (Importance = 1):
         * MEM.EXE does not get this from the XMS driver. It reads it straight
         * out of SysVars and skips its entire extended-memory report when the
         * word is zero:
         *     07B5  les bx,[..] / cmp word [es:bx+0x45],0 / jz 0x907
         * Measured on 6.22: SysVars+0x45 = 0x3B80 = 15232, and MEM prints
         * "Extended (XMS) 15,232K" -- the same number, which is what
         * identifies the field. Ours read zero because the swappable data
         * area used to be planted at SysVars+0x44, so the InDOS byte WAS
         * this field. The SDA has moved; see DOS_SDA_OFF.
         * The value is the XMS pool, so the two cannot disagree.
         */
        /* #48: IT IS THE MACHINE'S EXTENDED MEMORY, NOT THE XMS POOL (Importance = 1):
         * The field is what SYSINIT read from INT 15h AH=88h at boot, before
         * HIMEM loaded (RBIL: "extended memory size in K"). 6.22 says so itself:
         * MEM /D there prints Extended 15,597,568 = 65,536 used + 15,532,032 free
         * (runs/s81_mem/oracle_memd.txt) -- the total is this field (15232K), the
         * free is HIMEM's AH=08h, and the 64K "used" is the HMA, which XMS never
         * counts. Ours said 16384 (the pool) while AH=88h and CMOS 17h/30h said
         * 15360 -- one machine with two sizes. Now all three are CMOS_EXTENDED_KB and the
         * pool is that LESS the HMA (XMS_POOL_KB), so MEM's Total - Free = Used
         * comes out 64K, 6.22's shape, and can never go negative.
         */
        *(volatile WORD *)(sysVars + DOS_SYSVARS_OFF + DOS_SYSVARS_EXTENDED_KB) = (WORD)CMOS_EXTENDED_KB;
        /* [CAUTION]: GH #47: SysVars +0x43 = 0x0103, +0x49 = 0xFFFF and +0x4B = 0x0001
         * (6.22's values, where ours are zero) were planted together as a
         * diagnostic and REFUTED -- the phantom "Upper 1,663K" did not move.
         * +0x49 was the prime suspect on the theory that zero reads as "the UMB
         * chain starts at segment 0". It does not. Seventh refutation.
         */
        sysVars[DOS_SYSVARS_OFF + DOS_SYSVARS_BLOCK_DEVICES] = (BYTE)driveCount;
        /* ---- the device chain. The NUL header is INLINE at +0x22, not a pointer
         * to one (measured on 6.22). #48: it no longer terminates -- it links to
         * IO.SYS's twelve (CON .. COM4) at DOS_DEV_SEG, in 6.22's order, and the
         * last of those terminates. See DosDeviceChainBuild for what is measured
         * (the order, the stride, which pointers name which) and what is not (the
         * attribute words). SysVars+0x08/+0x0C name CLOCK$ and CON, as on 6.22.
         *
         * [CAUTION]: Linear 0x600..0x6E7 had been the environment block's until #207 moved
         * it to DOS_ENV_SEG 0x7F; nothing else writes there (dos_layout.h).
         */
        {   BYTE nulDevice[DOS_SYSVARS_NUL_LEN], device[DOS_DEVICE_AREA_LEN], nulStub[DOS_NULSTUB_LEN];
        UINT item;
            volatile BYTE *deviceArea = (volatile BYTE *)(ULONG_PTR)((DWORD)DOS_DEV_SEG << PARAGRAPH_SHIFT);
            DosDeviceChainBuild(device, DOS_DEV_SEG, driveCount);

            for (item = 0; item < DOS_DEVICE_AREA_LEN; ++item)
                deviceArea[item] = device[item];

            DosNulStubBuild(nulStub);

            for (item = 0; item < DOS_NULSTUB_LEN; ++item)
                sysVars[DOS_NULSTUB_OFF + item] = nulStub[item];

            DosNulHeaderBuild(nulDevice, DOS_DEV_SEG, DOS_DEVICE_OFFSET(DOS_DEVICE_CON),
                          DOS_NULSTUB_OFF + DOS_NULSTUB_STRAT, DOS_NULSTUB_OFF + DOS_NULSTUB_INTR);

            for (item = 0; item < DOS_SYSVARS_NUL_LEN; ++item)
                sysVars[DOS_SYSVARS_OFF + DOS_SYSVARS_NUL + item] = nulDevice[item];

            *(volatile WORD *)(sysVars + DOS_SYSVARS_OFF + DOS_SYSVARS_CLOCK)     = DOS_DEVICE_OFFSET(DOS_DEVICE_CLOCK);
            *(volatile WORD *)(sysVars + DOS_SYSVARS_OFF + DOS_SYSVARS_CLOCK + X86_FAR_POINTER_SEGMENT) = DOS_DEV_SEG;
            *(volatile WORD *)(sysVars + DOS_SYSVARS_OFF + DOS_SYSVARS_CON)       = DOS_DEVICE_OFFSET(DOS_DEVICE_CON);
            *(volatile WORD *)(sysVars + DOS_SYSVARS_OFF + DOS_SYSVARS_CON + X86_FAR_POINTER_SEGMENT)   = DOS_DEV_SEG; }
        /* ---- the CDS array. ONE ENTRY PER DRIVE LETTER, LASTDRIVE of them,
         * because it is INDEXED by drive and a walker reads all of them
         * whatever we populate. Entries for drives that exist carry flags
         * 0x4000 (physical) and a pointer to that drive's DPB; the rest are
         * zeroed with a terminated DPB pointer rather than one dangling at
         * the array's base. Measured layout: path at +0, flags at +0x43,
         * DPB far pointer at +0x45, backslash offset at +0x4F, stride 88.
         */
        /* In its own reserved block now (g_CdsSegment), 26 entries -- see DOS_LASTDRIVE. */
        if (g_CdsSegment)
        {
            volatile BYTE *cdsBytes = (volatile BYTE *)((DWORD)g_CdsSegment << PARAGRAPH_SHIFT);
            UINT di2;
            UINT seen2 = 0;

            for (di2 = 0; di2 < DOS_LASTDRIVE; ++di2)
            {
                BYTE cds[DOS_CDS_LEN];
                UINT item;
                UINT flags = 0;
                INT have = 0;
                INT index2;

                for (index2 = 0; index2 < (INT)driveCount; ++index2)
                    if (slot[index2] == di2)
                        have = 1;

                if (have)
                    flags = DOS_CDS_FLAG_PHYSICAL;
                else if (driveTypes[di2] == DRIVE_CDROM || driveTypes[di2] == DRIVE_REMOTE)
                    flags = DOS_CDS_FLAG_PHYSICAL | DOS_CDS_FLAG_NETWORK;

                DosCdsBuild(cds, di2, flags, DOS_CTAB_SEG,
                              (WORD)(DOS_DPBCHAIN_OFF + seen2 * DOS_DPB_LEN));

                if (have)
                    ++seen2;

                for (item = 0; item < DOS_CDS_LEN; ++item)
                    cdsBytes[di2 * DOS_CDS_LEN + item] = cds[item];
            }

            *(volatile WORD *)(sysVars + DOS_SYSVARS_OFF + DOS_SYSVARS_CDS)     = 0x0000;
            *(volatile WORD *)(sysVars + DOS_SYSVARS_OFF + DOS_SYSVARS_CDS + X86_FAR_POINTER_SEGMENT) = g_CdsSegment;
        }
        else                                    /* no block: no array, say so */
        {
            *(volatile WORD *)(sysVars + DOS_SYSVARS_OFF + DOS_SYSVARS_CDS)     = DOS_CDS_ARRAY_NONE;
            *(volatile WORD *)(sysVars + DOS_SYSVARS_OFF + DOS_SYSVARS_CDS + X86_FAR_POINTER_SEGMENT) = DOS_CDS_ARRAY_NONE;
        }

        /* ---- the SYSTEM FILE TABLE: one block, terminated, entries = what our
         * INT 21h layer can really open (DOS_MACHINE::fh[]). Same shape the
         * WOW path plants -- see the reserve above for why a DOS guest needs
         * one and why 0000:0000 was worse than useless. The entries are left
         * zeroed, which is not a stub: that is what a free SFT entry looks
         * like, and nothing has been opened this early.
         *
         * [CAUTION]: THE TERMINATOR IS THE POINT AT LEAST AS MUCH AS THE COUNT IS, so
         * the no-block path below writes FFFF:FFFF rather than falling back to
         * the zero this change exists to remove.
         */
        if (g_SftSegment)
        {
            volatile BYTE *sft = (volatile BYTE *)((DWORD)g_SftSegment << PARAGRAPH_SHIFT);
            UINT item;

            for (item = 0; item < (UINT)DOS_SFT_BYTES; ++item)
                sft[item] = 0;

            *(volatile WORD *)(sft + DOS_SFT_NEXT_OFFSET) = DOS_SFT_LAST;             /* next offset: last block */
            *(volatile WORD *)(sft + DOS_SFT_NEXT_SEGMENT) = DOS_SFT_LAST;             /* next segment */
            *(volatile WORD *)(sft + DOS_SFT_COUNT) = DOS_SFT_ENTRIES;    /* entries in this block */
            *(volatile WORD *)(sysVars + DOS_SYSVARS_OFF + DOS_SYSVARS_SFT)     = 0x0000;
            *(volatile WORD *)(sysVars + DOS_SYSVARS_OFF + DOS_SYSVARS_SFT + X86_FAR_POINTER_SEGMENT) = g_SftSegment;
        }
        else
        {
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
        else
            scan = LogPut(scan, "ABSENT -- chain head terminated FFFF:FFFF");

        scan = LogPut(scan, ", drives=");

        for (drive = 0; drive < DOS_DRIVE_LETTERS; ++drive)
        {
            if (!(drives & (1u << drive)))
                continue;

            scan = LogPut(scan, driveTypes[drive] == DRIVE_FIXED ? " " : driveTypes[drive] == DRIVE_REMOVABLE ? " ~"
                       : driveTypes[drive] == DRIVE_CDROM ? " cd:" : driveTypes[drive] == DRIVE_REMOTE ? " net:"
                       : driveTypes[drive] == DRIVE_RAMDISK ? " ram:" : " ?");
            {
                CHAR driveLetter[2];
                driveLetter[0] = (CHAR)('A' + drive);
                driveLetter[1] = 0;
                scan = LogPut(scan, driveLetter);
            }
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
    { volatile BYTE *controlTable = (volatile BYTE *)(DOS_CTAB_SEG << PARAGRAPH_SHIFT);
    UINT item;

      for (item = 0; item < sizeof(g_DosCtabUpper);   ++item)
          controlTable[DOS_CTAB_UPPER   + item] = g_DosCtabUpper[item];

      for (item = 0; item < sizeof(g_DosCtabFileNameUpper); ++item)
          controlTable[DOS_CTAB_FNUPPER + item] = g_DosCtabFileNameUpper[item];

      for (item = 0; item < sizeof(g_DosCtabFileNameTerminators);  ++item)
          controlTable[DOS_CTAB_FNTERM  + item] = g_DosCtabFileNameTerminators[item];

      for (item = 0; item < sizeof(g_DosCtabCollate); ++item)
          controlTable[DOS_CTAB_COLLATE + item] = g_DosCtabCollate[item];

      for (item = 0; item < sizeof(g_DosCtabDbcs);    ++item)
          controlTable[DOS_CTAB_DBCS    + item] = g_DosCtabDbcs[item];

      /* THE INT 2Fh AX=122Eh TABLES, ZEROED EXPLICITLY:
       * The block is MCB-reserved and in practice arrives zeroed, but a guest
       * that reads a table we never wrote is reading whatever the last run
       * left there -- and this is exactly the region where a stale pointer had
       * krnl386 writing into our own handler code. Cheap to be certain.
       *
       * [CAUTION]: 192 bytes covers all three tables (A, B, C at 0x4E0/0x520/0x560).
       */
      for (item = 0; item < DOS_INT2F_TBLS_LEN; ++item)
          controlTable[DOS_INT2F_TBL_A + item] = 0;

      /* INT 15h AH=C0h: THE SYSTEM CONFIGURATION TABLE. (GH #54):
       * Measured (tests/probes/dos/p_int15.asm): PCem's real AMI 486 says FC 01 00
       * 70 00; SeaBIOS FC 00 01 74 40; dosbox-x FC 00 01 70 40. The model triple
       * is the AMI's -- the period machine. The FEATURE BITS ARE NOT COPIED from
       * anyone: each one is a claim about THIS machine, and a claim a guest can
       * act on, so only the true ones are set.
       * f1 bit 6  second 8259 present ............ yes (vdd_pic)
       * f1 bit 5  real-time clock present ........ yes (vdd_cmos)
       * f1 bit 4  INT 09h calls INT 15h AH=4Fh ... YES (#244) -- bios_kbdact.asm
       *           k4f, made whenever IVT[15h] is not our own stub (our default
       *           would answer "process it" unchanged, so skipping it then is
       *           indistinguishable). Was NO (64h); AMI, SeaBIOS and dosbox-x set it
       * f1 bit 2  EBDA allocated ................. YES (#253) -- 1 KB at 9FC0h,
       *           which is what INT 12h's 639 KB always implied; AH=C1h and
       *           0040:000E now say so too. Was NO (60h) while C1h refused --
       *           see bios_bda.h for why the EBDA, not 640 KB, is the answer
       * f2 bit 6  INT 16h AH=09h supported ....... yes (vdd_input)
       */
      {   static const BYTE systemConfiguration[10] = { 0x08, 0x00, 0xFC, 0x01, 0x00,
                                            0x74, 0x40, 0x00, 0x00, 0x00 };

          for (item = 0; item < sizeof systemConfiguration; ++item)
              controlTable[DOS_SYSCONF_OFF + item] = systemConfiguration[item]; } }
}

/* How INT 21h AH=53h answers, and where that came from: the built-in model, or a cfg\ override. */
static PSTR StartupConfigureAh53Answers(PSTR cursor)
{
    /* INT 21h AH=53h's PRIVATE SUB-FUNCTIONS, AS A KNOB. (s79):
     * XP's COMMAND.COM asks AH=53h with AL as a selector, and AL=5's answer decides
     * whether it ever reads the keyboard: answered 1, it never does (observed: no
     * AH=0Ah, the shell goes past its prompt). Stock IS interactive,
     * so stock must answer AL=0 there -- while our probe measured AL=1 with its
     * output redirected to a file. Until that is re-measured un-redirected, the
     * answers are a table a run can change, not a constant a rebuild can.
     *
     * [CAUTION]: THE DEFAULT IS THE MEASURED VALUE, so a run with no file behaves exactly as
     * before. This is deliberately NOT a fix.
     *
     * [CAUTION]: And it prints unconditionally, with its source -- the dosver knob had two
     * sources and logged a line only when one of them won, which is how the rig
     * reported DOS 5.00 to every guest for an unknown number of sessions.
     */
    { PCSTR int53Source = "built-in (measured vs stock ntvdm, 2026-09-25)";
      HANDLE handle;
      /* THE CONTEXT-DEPENDENCE, MODELLED RATHER THAN OVERRIDDEN. (s79) (Importance = 2):
       * The measured stock answers (AL=2 -> CF=0, AL=5 -> AL=1) do not let XP's
       * COMMAND.COM read a key: with AL=5 answered 1 it never reaches its keyboard
       * read (see above). Stock IS interactive, so stock answers
       * differently WHEN THE SHELL ASKS -- the call is context-dependent, and the
       * context we measured in was a standalone probe with its stdout redirected.
       *
       * So model the context instead of claiming a new universal value: an
       * NTVDM-AWARE SHELL gets the answers that make it a shell; every other guest,
       * and every probe, still gets the measured ones. That is narrower than the
       * `cfg\int53.txt` knob it replaces, and it cannot affect anything else.
       *
       * [CAUTION]: STILL PROVISIONAL, BUT NARROWER (s84, #142). `p_int53f.com` under stock, with
       * and without redirection, answers IDENTICALLY (5305 -> AL=1 both ways), so
       * redirection is NOT the context that flips it. What is left is the caller:
       * a probe is always a CHILD of stock's shell, and only the shell itself can be
       * asked for the AL=0 answer this branch gives it. Kept as the model until
       * that can be measured. It is marked here so it cannot quietly become folklore.
       */
      if (g_GuestNtAware)
      {
          g_DosInt53Answers[DOS_INT53_SHELL_LOOP].Ax = DOS_FN_BPB_TO_DPB << BYTE_SHIFT;
          g_DosInt53Answers[DOS_INT53_SHELL_LOOP].IsCarry = 1;   /* top of its main loop */
          /* #208: a ROUTED program is the shell's work, not the keyboard's. CF=0 here sends
           * its loop to BOP 54 sub 01 ("what next?") instead of the prompt -- so when the
           * program ends the shell ASKS, and we decide: done (close the window) or, after
           * Close Program, the prompt. See the sub 01 arm.
           */
          if (g_Routed)
              g_DosInt53Answers[DOS_INT53_SHELL_LOOP].IsCarry = 0;

          g_DosInt53Answers[DOS_INT53_STARTUP].Ax = DOS_FN_BPB_TO_DPB << BYTE_SHIFT;
          g_DosInt53Answers[DOS_INT53_STARTUP].IsCarry = 0;   /* -> [0x327] = 0 */
          int53Source = "NTVDM-aware shell (PROVISIONAL -- see p_int53f)";
      }

      handle = CreateFileA(INT53_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                      NULL, OPEN_EXISTING, 0, NULL);

      if (handle != INVALID_HANDLE_VALUE)
      {
          CHAR text[512];
          DWORD bytesRead = 0;
          DWORD index = 0;
          ReadFile(handle, text, sizeof text - 1, &bytesRead, NULL);
          CloseHandle(handle);

          while (index < bytesRead)
          {
              UINT versions[INT53_FIELDS];
              INT fieldCount = 0;
              /* one line */
              while (index < bytesRead && (text[index] == ' ' || text[index] == '\t'))
                  ++index;

              if (index < bytesRead && (text[index] == ';' || text[index] == '#'))
              {
                  while (index < bytesRead && text[index] != '\n')
                      ++index;
              }

              while (index < bytesRead && text[index] != '\n' && fieldCount < INT53_FIELDS)
              {
                  UINT value = 0;
                  INT got = 0;

                  while (index < bytesRead && (text[index] == ' ' || text[index] == '\t'))
                      ++index;

                  while (index < bytesRead)
                  {
                      CHAR digitCharacter = text[index];
                      INT digit = (digitCharacter >= '0' && digitCharacter <= '9') ? digitCharacter - '0'
                            : (digitCharacter >= 'a' && digitCharacter <= 'f') ? digitCharacter - 'a' + HEX_DIGIT_A_VALUE
                            : (digitCharacter >= 'A' && digitCharacter <= 'F') ? digitCharacter - 'A' + HEX_DIGIT_A_VALUE : -1;

                      if (digit < 0)
                          break;

                      value = value * HEX_RADIX_U + (UINT)digit;
                      got = 1;
                      ++index;
                  }

                  if (!got)
                      break;

                  versions[fieldCount++] = value;
              }

              if (fieldCount == INT53_FIELDS && versions[INT53_FIELD_AL] < DOS_INT53_COUNT)
              {
                  g_DosInt53Answers[versions[INT53_FIELD_AL]].Ax = (WORD)versions[INT53_FIELD_AX];
                  g_DosInt53Answers[versions[INT53_FIELD_AL]].IsCarry = (BYTE)(versions[INT53_FIELD_CARRY] ? 1 : 0);
                  int53Source = "cfg\\int53.txt";
              }

              while (index < bytesRead && text[index] != '\n') ++index;

              if (index < bytesRead)
                  ++index;
          }
      }

      cursor = LogPut(cursor, "STAGE2: INT 21h AH=53h answers (source: ");
      cursor = LogPut(cursor, int53Source); cursor = LogPut(cursor, ")");
      { UINT item;

        for (item = 0; item < DOS_INT53_COUNT; ++item)
        {
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

      if (handle != INVALID_HANDLE_VALUE)
      {
          CHAR text[16];
          DWORD bytesRead = 0;
          UINT index = 0;
          UINT major = 0;
          UINT minor = 0;
          ReadFile(handle, text, sizeof text - 1, &bytesRead, NULL);
          CloseHandle(handle);

          while (index < bytesRead && text[index] >= '0' && text[index] <= '9')
          {
              major = major*DECIMAL_RADIX + (UINT)(text[index]-'0');
              ++index;
          }

          if (index < bytesRead && text[index] == '.')
          {
              ++index;

              while (index < bytesRead && text[index] >= '0' && text[index] <= '9')
              {
                  minor = minor*DECIMAL_RADIX + (UINT)(text[index]-'0');
                  ++index;
              }
          }

          if (major && major < BYTE_VALUES && minor < BYTE_VALUES)
          {
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
         * DosMcbInitialize, which lays the chain and clears the name byte.
         */
        DosMcbSetOwnerName(NULL, DOS_PSP_SEG, programPathBuffer);
        /* The CDS array's block, at the top of the chain (see DOS_LASTDRIVE). The
         * PSP was built with DOS_MEM_TOP as its memory top; the program's block now
         * ends one paragraph below the reserved block's data, and PSP+2 must say so
         * or a program that resizes itself to "PSP+2 - PSP" fails with error 8.
         */
        /* [CAUTION]: ONE RESERVATION, CARVED -- NOT TWO CALLS. DosMcbReserveTop() splits
         * the LAST 'Z' block, and its first act is to make the block it split an
         * 'M' and put the new 'Z' on top. So a SECOND call finds the block the
         * FIRST one just reserved and tries to split THAT: 143 paragraphs, which
         * cannot hold the SFT's 473, so it returned 0 and the SFT silently came
         * out ABSENT. Measured, first run -- the log said "SFT ABSENT" while the
         * memory it needed was sitting free below. Reserve the pair in one go and
         * carve it: CDS at the bottom, SFT immediately above. One MCB owned by
         * DOS (8) covering both is what it is -- resident DOS data.
         */
        {   WORD reservedSegment = DosMcbReserveTop(NULL, firstMcb,
                                            (WORD)(DOS_CDS_PARAS + DOS_SFT_PARAS));

            if (reservedSegment)
            {
                g_CdsSegment = reservedSegment;
                g_SftSegment = (WORD)(reservedSegment + DOS_CDS_PARAS);
            }
            else
                g_CdsSegment = DosMcbReserveTop(NULL, firstMcb, DOS_CDS_PARAS);
        }
        /* AND THE SFT, FOR A **DOS** GUEST. (s72) (Importance = 1):
         * The SFT chain was planted only on the WOW path, and the note there said
         * so in as many words -- "a DOS guest still gets SysVars+4 = 0 ... when a
         * DOS program needs the SFT, this moves". A DOS program now has: p_sysvar
         * walks the List of Lists and reads the chain head as absent.
         *
         * [CAUTION]: AND ABSENT IS THE DANGEROUS PART, NOT THE MISSING PART. SysVars+4 = 0
         * does not mean "no SFT", it means "an SFT at segment 0" -- so a program
         * that walks the chain reads the IVT as SFT headers. That is exactly what
         * krnl386 did: 0x00000000 -> 0x000fa357 -> 0x000bc370 -> back, forever,
         * 117 MB of DPMI calls in one run (see DOS_SFT_* in dos_layout.h). The
         * only reason no DOS guest had hit it is that none had looked.
         * - THE MEMORY IS AFFORDABLE, AND THAT WAS MEASURED, NOT ASSUMED. The block
         *   is 473 paragraphs (7.4 KB). p_tsr puts the largest free block at 0x962F
         *   here against 0x9302 on genuine MS-DOS 6.22 -- we hand out 12.7 KB MORE
         *   than real DOS -- so after this we are still 5.3 KB ahead of it, and a
         *   guest that fits on 6.22 still fits here.
         * - Reserved at the top like the CDS, owned by DOS (8), so it is resident
         *   data a memory walker can see and account for rather than a hole. It is
         *   taken AFTER the CDS, so it lands just below it and the program's block
         *   now ends below THIS one -- hence PSP+2 comes from the lower of the two.
         */
        if (g_CdsSegment)
            *(volatile WORD *)(((DWORD)DOS_PSP_SEG << PARAGRAPH_SHIFT) + DOS_PSP_MEMORY_TOP) = (WORD)(g_CdsSegment - 1);
    }
}

/* Build the guest's DOS environment block, with cfg\dosenv.txt's extra variables, the launcher compiler
 * variables, and argv[0] -- shortened for the guest when it has to be.
 */
static PSTR StartupBuildEnvironment(PSTR cursor, CHAR *programPathBuffer)
{
    { static CHAR dosEnvironment[192];
      HANDLE handle = CreateFileA(DOSENV_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             NULL, OPEN_EXISTING, 0, NULL);
      dosEnvironment[0] = 0;

      if (handle != INVALID_HANDLE_VALUE)
      {
          DWORD bytesRead = 0;
          ReadFile(handle, dosEnvironment, sizeof dosEnvironment - 1, &bytesRead, NULL);
          CloseHandle(handle);
          dosEnvironment[bytesRead < sizeof dosEnvironment ? bytesRead : sizeof dosEnvironment - 1] = 0;

          if (dosEnvironment[0])
          {
              cursor = LogPut(cursor, "STAGE2: dosenv.txt -> extra guest environment [");
              cursor = LogPut(cursor, dosEnvironment); cursor = LogPut(cursor, "]\r\n");
          }
      }

      /* argv[0] MUST BE 8.3, BECAUSE A DOS EXTENDER RE-OPENS IT. (s73) (Importance = 4):
       * DOS/4GW loads its own protected-mode half by re-opening the program named in
       * the environment's program-path field, and it tokenises that name -- so a path
       * with spaces is torn at the first one and the open fails. It then prints its
       * banner and stops, with no error and no video mode set.
       * - THAT IS THE "HEADLESS-ONLY DOS/4GW BLOCKER", and it was never headless-only:
       *   it is PATH-specific, exactly as the note said. A by-hand launch works because
       *   CSRSS hands us the SHORT name already (C:\DOCUME~1\...\DOOM.EXE); the
       *   target.txt path handed over the long one (C:\Documents and Settings\...),
       *   and only the harness used that. Any user whose games live under a path with
       *   a space had the same broken launch.
       * - Shortened here, once, where argv[0] is built -- so every launch shape agrees
       *   with the one that was already working. WowShorten leaves the path alone if
       *   GetShortPathNameA cannot answer, so a path that has no 8.3 form is unchanged.
       */
      if (programPathBuffer[0])
      {
          CHAR before[768];
          lstrcpynA(before, programPathBuffer, sizeof before);
          WowShorten(programPathBuffer, sizeof programPathBuffer);

          if (lstrcmpA(before, programPathBuffer) != 0)
          {
              cursor = LogPut(cursor, "STAGE2: argv[0] shortened for the guest: ["); cursor = LogPut(cursor, before);
              cursor = LogPut(cursor, "] -> ["); cursor = LogPut(cursor, programPathBuffer); cursor = LogPut(cursor, "]\r\n");
          }

          /* AND IF IT IS STILL TOO LONG, HAND OVER THE BARE NAME (Importance = 5):
           * DOS/4GW 1.97 copies argv[0] into a **64-BYTE BUFFER** and does not bound
           * it. MEASURED, three ways, from this one share:
           *   doom\DOOM.EXE        62 chars -> loads and plays
           *   hexen\HEXEN.EXE      64 chars -> "fatal error (1007): can't find file
           *                        ...\HEXEN\HEXEN.EXE< to load"  (no room for the
           *                        NUL, so it reads one byte of garbage)
           *   heretic\HERETIC.EXE  68 chars -> truncated at 64: "...\HERETICD"
           * Copying HEXEN.EXE alone to a 61-char path fixed it outright -- DOS/4GW
           * loaded and Hexen got as far as looking for its WAD.
           *
           * [CAUTION]: THIS IS WHY "HEXEN WORKED BEFORE AND DOES NOT NOW", and it is not a code
           * regression: s73 moved the games from `games\Hexen\` (59) to
           * `demo\msdos\hexen\` (64) and crossed the limit. A user installing a game
           * under a deep path of their own hits exactly the same wall.
           * - The bare filename is safe because the guest's current directory IS the
           *   program's own directory (it is how every one of these games finds its
           *   WAD), so the extender's open resolves to the same file -- and 8.3 name
           *   plus NUL can never approach 64. Only done when it must be.
           */
          if (lstrlenA(programPathBuffer) > EXTENDER_ARGV0_SAFE_LENGTH)
          {
              PCSTR baseName = programPathBuffer;
              PCSTR scan;

              for (scan = programPathBuffer; *scan; ++scan)
                  if (*scan == '\\' || *scan == '/')
                      baseName = scan + 1;

              if (baseName != programPathBuffer && *baseName)
              {
                  cursor = LogPut(cursor, "STAGE2: argv[0] is "); cursor = LogHex(cursor, (DWORD)lstrlenA(programPathBuffer));
                  cursor = LogPut(cursor, " chars -- past DOS/4GW's 64-byte buffer; handing over the bare name [");
                  cursor = LogPut(cursor, baseName); cursor = LogPut(cursor, "] (cwd is the program's own directory)\r\n");
                  {
                      CHAR baseNameCopy[64];
                      lstrcpynA(baseNameCopy, baseName, sizeof baseNameCopy);
                      LogPut(programPathBuffer, baseNameCopy);
                  }
              }
          }
      }

      /* -- THE ENVIRONMENT: the four defaults + dosenv.txt + the launcher's LIB/INCLUDE,
       * all in the ONE fixed 256-byte block at 0x60. The memory map does not move --
       * see LauncherCompilerVariables for why that matters (a relocated block #GP'd every
       * DOS extender). `extra` is dosenv.txt followed by the compiler vars.
       */
      { static CHAR environmentExtra[512];
      DWORD extraOffset = 0;

        if (dosEnvironment[0]) { extraOffset = (DWORD)wsprintfA(environmentExtra, "%s", dosEnvironment);

                         if (extraOffset && environmentExtra[extraOffset-1] != '\n')
                             environmentExtra[extraOffset++] = '\n'; }

        /* -- ULTRASND= IS HOW A GUS PROGRAM FINDS THE CARD, AND IT LOOKS BEFORE IT PROBES.
         * heaven7 never touched a port without it. <base hex>,<DRAM DMA>,<record DMA>,
         * <GF1 IRQ>,<MIDI IRQ> (docs/ref/gus.md section 1) -- the numbers the device was built
         * with, so the string and the card cannot disagree. A dosenv.txt ULTRASND wins.
         */
        if (g_GusOn && !StrStrNoCase(environmentExtra, HOST_ENV_ULTRASND_ASSIGN))
        {
            extraOffset += (DWORD)wsprintfA(environmentExtra + extraOffset, HOST_ENV_ULTRASND_FORMAT,
                                   (UINT)g_Gus.BasePort, (UINT)g_Gus.DmaChannel, (UINT)g_Gus.DmaChannel,
                                   (UINT)g_Gus.Irq, (UINT)g_Gus.Irq);
        }

        if (g_Fetch2Ok)
        {
            UINT variableCount = LauncherCompilerVariables(g_Environment2, sizeof g_Environment2, environmentExtra + extraOffset,
                                                 (DWORD)sizeof environmentExtra - extraOffset);

            if (variableCount) { cursor = LogPut(cursor, "STAGE2: launcher compiler vars -> "); cursor = LogHex(cursor, variableCount);
                      cursor = LogPut(cursor, " (LIB/INCLUDE into the 0x60 block; memory map unmoved)\r\n"); }
        }

        DosEnvBuildWithCard(NULL, DOS_ENV_SEG, programPathBuffer[0] ? programPathBuffer : HOST_DEFAULT_PROGRAM_PATH,
                           HOST_DEFAULT_DRIVE_ROOT, &g_SbConfig, environmentExtra[0] ? environmentExtra : NULL);        /* M2.5: env */
      }
      /* -- READ THE BLOCK BACK OUT OF GUEST MEMORY AND PRINT IT. Not the string we
       * passed in -- the bytes the guest will actually walk, which is a different
       * claim and the only one worth logging. A DOS environment is a run of
       * NUL-terminated strings ended by a double NUL, then a count WORD, then the
       * program path; every one of those can be got wrong, and "the variable is
       * set" cannot be told from "the variable is set but the block is malformed"
       * from the host side. NULs print as '.' so the string boundaries are visible.
       *
       * [CAUTION]: This is what makes dosenv.txt a MEASURED feature rather than an asserted
       * one -- see `an unimplemented call still answers`.
       */
      /* [CAUTION]: ITS OWN BUFFER, NOT THE RUNNING REPORT. The first cut appended into `p` and
       * bounded the printable characters with `p < base + 3800` -- by this point in
       * the STAGE2 report that bound was already passed, so every readable byte was
       * silently dropped and the dump came back as `[......]`, which reads exactly
       * like an EMPTY ENVIRONMENT. An instrument that fails by printing a plausible
       * wrong answer is worse than one that fails loudly.
       */
      { const volatile BYTE *environmentBytes = (const volatile BYTE *)((DWORD)DOS_ENV_SEG << PARAGRAPH_SHIFT);
        CHAR environmentLine[288];
        CHAR *environmentCursor = environmentLine;
        UINT environmentIndex;
        UINT zeros = 0;
        environmentCursor = LogPut(environmentCursor, "STAGE2: guest environment block @0x");
        environmentCursor = LogHex(environmentCursor, (DWORD)DOS_ENV_SEG << PARAGRAPH_SHIFT); environmentCursor = LogPut(environmentCursor, " = [");

        for (environmentIndex = 0; environmentIndex < ENVIRONMENT_DUMP_BYTES && environmentCursor < environmentLine + ENVIRONMENT_DUMP_LINE; ++environmentIndex)
        {
            BYTE character = environmentBytes[environmentIndex];

            if (character == 0) { *environmentCursor++ = '.';
            *environmentCursor = 0;

                          if (++zeros >= ENVIRONMENT_DUMP_NULS && environmentIndex > ENVIRONMENT_DUMP_SKIP)
                              break;

                          continue; }

            zeros = 0;
            *environmentCursor++ = (CHAR)((character >= ASCII_SPACE && character < ASCII_DELETE) ? character : '?');
            *environmentCursor = 0;
        }

        environmentCursor = LogPut(environmentCursor, "]\r\n");
        (VOID)environmentCursor;
        /* Into the running report, the same way every neighbouring line goes: a
         * direct LogAppend here produced NOTHING in the file while the zput three
         * statements above appeared, and an instrument that silently writes nowhere
         * is not worth debugging twice.
         */
        cursor = LogPut(cursor, environmentLine); }
    }
    return cursor;
}

/* Plant a BOP stub for each BIOS interrupt we service (biosInts) in DOS memory. */
static VOID StartupPlantBiosStubs(VOID)
{
    {
      /* GH #43/#44/#45: the BIOS interrupts we had never planted at all. Until now
       * these vectors were filled by the null-vector sweep with a bare IRET, so a
       * guest asking for the equipment list or the memory size got silence and
       * whatever was already in its registers.
       */
      /* {vector, BOP number}.  They match for all but INT 20h: BOP 0x20 is ALREADY
       * the INT 21h handler's, and planting INT 20h with it made the BIOS dispatch
       * intercept every INT 21h call as "terminate program" -- selftest exited at
       * its first DOS call with no output. BOP numbers are a shared namespace with
       * DPMI (0x50-0x57), XMS (0x43) and the rest; 0x30 is free.
       */
      static const BYTE biosInts[][2] = {
          { VECTOR_EQUIPMENT, DOS_BOP_FOR_VECTOR(VECTOR_EQUIPMENT) }, { VECTOR_MEMORY_SIZE, DOS_BOP_FOR_VECTOR(VECTOR_MEMORY_SIZE) }, { VECTOR_DISK, DOS_BOP_FOR_VECTOR(VECTOR_DISK) }, { VECTOR_SERIAL, DOS_BOP_FOR_VECTOR(VECTOR_SERIAL) },
          { VECTOR_SYSTEM, DOS_BOP_FOR_VECTOR(VECTOR_SYSTEM) }, { VECTOR_PRINTER, DOS_BOP_FOR_VECTOR(VECTOR_PRINTER) }, { VECTOR_ABSOLUTE_DISK_READ, DOS_BOP_FOR_VECTOR(VECTOR_ABSOLUTE_DISK_READ) }, { VECTOR_ABSOLUTE_DISK_WRITE, DOS_BOP_FOR_VECTOR(VECTOR_ABSOLUTE_DISK_WRITE) },
          { VECTOR_TERMINATE, DOS_BOP_INT20 },                                  /* GH #46: see above */
          { VECTOR_TERMINATE_RESIDENT, DOS_BOP_FOR_VECTOR(VECTOR_TERMINATE_RESIDENT) }, { VECTOR_DOS_IDLE, DOS_BOP_FOR_VECTOR(VECTOR_DOS_IDLE) }, { VECTOR_FAST_CONSOLE_OUTPUT, DOS_BOP_FOR_VECTOR(VECTOR_FAST_CONSOLE_OUTPUT) },
          { VECTOR_NETWORK, DOS_BOP_FOR_VECTOR(VECTOR_NETWORK) }, { VECTOR_NETBIOS, DOS_BOP_FOR_VECTOR(VECTOR_NETBIOS) },                  /* GH #8 (s91): NetBIOS, see V86BiosBop */
      };
      UINT byteIndex;
      volatile BYTE *controlBytes = (volatile BYTE *)(DOS_CTAB_SEG << PARAGRAPH_SHIFT);

      for (byteIndex = 0; byteIndex < sizeof(biosInts)/sizeof(biosInts[0]); ++byteIndex)
      {
          UINT offset = DOS_BIOS_STUBS + byteIndex * DOS_BIOS_STUB_SIZE;
          controlBytes[offset + 0] = VDM_BOP0;
          controlBytes[offset + 1] = VDM_BOP1;
          controlBytes[offset + VDM_BOP_NUMBER_OFFSET] = biosInts[byteIndex][1];
          /* INT 25h/26h RETURN WITH THE CALLER'S FLAGS STILL PUSHED:
           * Every other vector here ends in IRET. DOS's absolute disk read and
           * write do NOT: they return by RETF, deliberately leaving the FLAGS
           * word the INT pushed on the caller's stack, which the caller then
           * discards itself (`add sp,2`). Ending them with IRET pops that word
           * and the caller's `add sp,2` then eats its own return address --
           * corruption that surfaces later, somewhere else. (GH #44)
           */
          controlBytes[offset + VDM_BOP_LENGTH] = (biosInts[byteIndex][0] == VECTOR_ABSOLUTE_DISK_READ || biosInts[byteIndex][0] == VECTOR_ABSOLUTE_DISK_WRITE)
                        ? X86_OP_RETF   /* RETF */
                        : X86_OP_IRET;  /* IRET */
          *(volatile WORD *)(IVT_OFFSET_ADDRESS(biosInts[byteIndex][0]))     = (WORD)offset;
          *(volatile WORD *)(IVT_SEGMENT_ADDRESS(biosInts[byteIndex][0])) = DOS_CTAB_SEG;

          if (biosInts[byteIndex][0] == VECTOR_SYSTEM)
              g_Int15StubOffset = (WORD)offset;                                            /* #244 */
      } }
}

/* Apply the Settings conventional-memory size (#136), unless the program does not fit under it. */
static PSTR StartupApplyConventionalKb(PSTR cursor, const DWORD readCount)
{
    /* -- #136: HOW MUCH CONVENTIONAL MEMORY THIS MACHINE HAS. Decided here, once, before
     * the image is laid down: the loader writes the program above the PSP with no
     * bound of its own, and the EBDA (BiosBdaInitializeWithTop, later) is zeroed at the new
     * top -- so a program that does not fit under a small setting would be loaded and
     * then have its own code wiped. A real DOS says "Program too big to fit in memory"
     * at that point; the host cannot say that to a program it was launched to RUN, so
     * it refuses the SETTING instead, loudly, and the machine stays at 640 KB.
     * 640 (the default) never enters this block and logs nothing new.
     */
    if (g_ConventionalKbWant != BIOS_CONV_KB_MAX)
    {
        WORD top = BiosConventionalTopParagraph(g_ConventionalKbWant);
        WORD alloc = 0;
        WORD avail = (WORD)(top - DOS_PSP_SEG);
        INT high = 0;
        INT fits;

        if (readCount >= DOS_EXE_SIGNATURE_SIZE && g_FileBuffer[0] == 'M' && g_FileBuffer[1] == 'Z')
            fits = DosExecSize(g_FileBuffer, readCount, avail, &alloc, &high) == 0;
        else                                /* .COM: PSP + the image + a 256-byte stack */
            fits = (UINT32)DOS_PSP_PARAGRAPHS + ((readCount + DOS_PSP_SIZE + PARAGRAPH_LAST_BYTE_U) >> PARAGRAPH_SHIFT) <= (UINT32)avail;

        cursor = LogPut(cursor, "STAGE2: ConventionalKB=");  cursor = LogDecimal(cursor, g_ConventionalKbWant);

        if (fits)
        {
            g_DosMemoryTop = top;
            cursor = LogPut(cursor, " -> INT 12h ");       cursor = LogDecimal(cursor, BiosBaseKbOfTop(top));
            cursor = LogPut(cursor, " KB, EBDA + MCB top 0x"); cursor = LogHex(cursor, top);
            cursor = LogPut(cursor, " (#136)\r\n");
        }
        else
        {
            cursor = LogPut(cursor, " REFUSED: this program does not fit under it -- the machine stays at "
                        "640 KB (#136)\r\n");
            SettingsNoteOverride(SET_CONVKB, SETTINGS_SOURCE_CONV_KB, BIOS_CONV_KB_MAX);
        }
    }

    return cursor;
}

/* Nothing named a program: load a shell -- the one cfg\shell.txt names, or the fallback. */
static VOID StartupLoadShell(
    PSTR *cursorIo,
    DWORD *readCountIo,
    CHAR *shellConfig,
    PCSTR const shellSource,
    CHAR *programPathBuffer,
    INT *wasShellIo)
{
    PSTR cursor = *cursorIo;
    DWORD readCount = *readCountIo;
    INT wasShell = *wasShellIo;

    if (!readCount)
    {
        CHAR shell[512];
        HANDLE shellHandle = INVALID_HANDLE_VALUE;
        PCSTR why = 0;
        static CHAR whyBuffer[96];

        if (shellConfig[0])
        {
            lstrcpynA(shell, shellConfig, sizeof shell);
            shellHandle = CreateFileA(shell, GENERIC_READ, FILE_SHARE_READ, NULL,
                             OPEN_EXISTING, 0, NULL);
            why = shellSource;

            if (shellHandle == INVALID_HANDLE_VALUE)
            {
                LogPut(LogPut(whyBuffer, shellSource), " NAMES A FILE THAT WILL NOT OPEN");
                why = whyBuffer;
            }
            else
            {
                /* [WARNING]: A Windows program is not a DOS shell. The Browse filter allows *.exe
                 * (a DOS shell can be one), so a user can pick cmd.exe; loaded as a DOS
                 * guest a PE image just runs its stub or worse. Refuse it and fall back.
                 */
                BYTE header[SHELL_HEADER_READ];
                DWORD headerRead = 0;
                ReadFile(shellHandle, header, sizeof header, &headerRead, NULL);

                if (headerRead == sizeof header && header[0] == 'M' && header[1] == 'Z')
                {
                    DWORD newHeaderOffset = *(const DWORD *)(header + DOS_MZ_NEW_HEADER);
                    BYTE signatureBytes[DOS_EXE_SIGNATURE_SIZE];
                    DWORD signatureRead = 0;

                    if (newHeaderOffset >= DOS_MZ_NEW_HEADER_MIN && SetFilePointer(shellHandle, (LONG)newHeaderOffset, NULL, FILE_BEGIN) == newHeaderOffset
                        && ReadFile(shellHandle, signatureBytes, DOS_EXE_SIGNATURE_SIZE, &signatureRead, NULL) && signatureRead == DOS_EXE_SIGNATURE_SIZE
                        && ((signatureBytes[0] == 'N' && signatureBytes[1] == 'E') || (signatureBytes[0] == 'P' && signatureBytes[1] == 'E')))
                    {
                        CloseHandle(shellHandle);
                        shellHandle = INVALID_HANDLE_VALUE;
                        LogPut(LogPut(whyBuffer, shellSource), " NAMES A WINDOWS PROGRAM, NOT A DOS SHELL");
                        why = whyBuffer;
                    }
                }

                if (shellHandle != INVALID_HANDLE_VALUE)
                    SetFilePointer(shellHandle, 0, NULL, FILE_BEGIN);
            }
        }

        if (shellHandle == INVALID_HANDLE_VALUE)
        {
            LogPut(shell, HOST_DEFAULT_SHELL_PATH);
            shellHandle = CreateFileA(shell, GENERIC_READ, FILE_SHARE_READ, NULL,
                             OPEN_EXISTING, 0, NULL);

            if (shellHandle != INVALID_HANDLE_VALUE && !why)
                why = "the system's own COMMAND.COM";
        }

        if (shellHandle != INVALID_HANDLE_VALUE)
        {
            ReadFile(shellHandle, g_FileBuffer, sizeof(g_FileBuffer), &readCount, NULL);
            CloseHandle(shellHandle);
            LogPut(programPathBuffer, shell);
            wasShell = 1;
            cursor = LogPut(cursor, "STAGE2: no program was named -> loading a SHELL, 0x");
            cursor = LogHex(cursor, readCount); cursor = LogPut(cursor, " from "); cursor = LogPut(cursor, shell);
            cursor = LogPut(cursor, " ("); cursor = LogPut(cursor, why ? why : "default"); cursor = LogPut(cursor, ")\r\n");
        }
    }

    *cursorIo = cursor;
    *readCountIo = readCount;
    *wasShellIo = wasShell;
}

/* #208: hand a program started from Windows to XP's COMMAND.COM, unless directlaunch.flag asks for the old way. */
static VOID StartupRouteToCommandCom(
    PSTR *cursorIo,
    DWORD *readCountIo,
    CHAR *shellConfig,
    CHAR *programPathBuffer,
    CHAR *args)
{
    PSTR cursor = *cursorIo;
    DWORD readCount = *readCountIo;

    if (readCount && !g_WowLaunch && !shellConfig[0]
        && GetFileAttributesA(DIRECTLAUNCH_FLAG) == INVALID_FILE_ATTRIBUTES)
    {
        INT pathLength = lstrlenA(programPathBuffer);
        INT isCommand = 0;
        INT isNewExe = 0;
        CHAR shortPath[300];
        DWORD shortLength;
        isCommand = (pathLength >= COMMAND_COM_LENGTH && !lstrcmpiA(programPathBuffer + pathLength - COMMAND_COM_LENGTH, HOST_COMMAND_COM));

        if (readCount > DOS_MZ_NEW_HEADER_MIN && g_FileBuffer[0] == 'M' && g_FileBuffer[1] == 'Z')
        {
            DWORD newHeaderOffset = *(const DWORD *)(g_FileBuffer + DOS_MZ_NEW_HEADER);

            if (newHeaderOffset > DOS_MZ_NEW_HEADER_MIN && newHeaderOffset + DOS_EXE_SIGNATURE_SIZE < readCount
                && ((g_FileBuffer[newHeaderOffset] == 'N' && g_FileBuffer[newHeaderOffset + 1] == 'E')
                    || (g_FileBuffer[newHeaderOffset] == 'P' && g_FileBuffer[newHeaderOffset + 1] == 'E')))
                isNewExe = 1;
        }

        shortLength = GetShortPathNameA(programPathBuffer, shortPath, sizeof shortPath);

        if (!isCommand && !isNewExe && shortLength && shortLength < ROUTED_PATH_MAX && shortLength + 1 + (DWORD)lstrlenA(args) < DOS_PSP_COMMAND_TAIL_MAX)
        {
            lstrcpynA(g_FirstProgram, shortPath, sizeof g_FirstProgram);
            lstrcpynA(g_FirstTail, args, sizeof g_FirstTail);
            g_Routed = 1;
            readCount = 0;                               /* -> the shell block below */
            args[0] = 0;                             /* they are the program's, not the shell's */
            cursor = LogPut(cursor, "STAGE2: #208 routing [");  cursor = LogPut(cursor, g_FirstProgram);
            cursor = LogPut(cursor, "] args=[");                cursor = LogPut(cursor, g_FirstTail);
            cursor = LogPut(cursor, "] through XP's COMMAND.COM (BOP 54 sub 01), as stock does\r\n");
        }
        else
        {
            cursor = LogPut(cursor, "STAGE2: #208 NOT routed (");
            cursor = LogPut(cursor, isCommand ? "it is a COMMAND.COM" : isNewExe ? "not a DOS image"
                              : "8.3 path + arguments too long for a DOS command line");
            cursor = LogPut(cursor, ") -- loading it directly\r\n");
        }
    }

    *cursorIo = cursor;
    *readCountIo = readCount;
}

/* A .PIF: parse it and take its program, start directory and parameters. */
static VOID StartupApplyPif(PSTR *cursorIo, DWORD *readCountIo, CHAR *programPathBuffer, CHAR *args)
{
    PSTR cursor = *cursorIo;
    DWORD readCount = *readCountIo;

    /* A .PIF NAMES A PROGRAM; IT IS NOT ONE. (s85) See pif.h:
     * Explorer queues the PIF itself as the program, and we handed its bytes to
     * COMMAND.COM to execute. Stock NTVDM reads it: the program, its parameters (the
     * WINDOWS 386 section's copy when there is one) and its start directory. A
     * relative program is looked for in the start directory, then beside the PIF,
     * then on the PATH. Arguments typed after the PIF follow the PIF's own.
     */
    if (readCount && !g_WowLaunch && programPathBuffer[0])
    {
        INT pathLength = lstrlenA(programPathBuffer);
        PIF_INFO pif;

        if (pathLength > DOS_DOT_EXTENSION_LENGTH && !lstrcmpiA(programPathBuffer + pathLength - DOS_DOT_EXTENSION_LENGTH, HOST_EXTENSION_PIF)
            && PifParse(g_FileBuffer, readCount, &pif))
        {
            CHAR program[MAX_PATH];
            CHAR directory[MAX_PATH];
            CHAR pifDirectory[MAX_PATH];
            CHAR pifCandidate[MAX_PATH];
            CHAR extra[256];
            HANDLE pifHandle = INVALID_HANDLE_VALUE;
            INT item;
            ExpandEnvironmentStringsA(pif.Program, program, sizeof program);
            ExpandEnvironmentStringsA(pif.Directory, directory, sizeof directory);
            lstrcpynA(pifDirectory, programPathBuffer, sizeof pifDirectory);

            for (item = lstrlenA(pifDirectory); item > 0 && pifDirectory[item - 1] != '\\'; --item) ;

            pifDirectory[item > 0 ? item - 1 : 0] = 0;

            if (directory[0] && GetFileAttributesA(directory) == INVALID_FILE_ATTRIBUTES)
            {
                cursor = LogPut(cursor, "STAGE2: PIF start directory ["); cursor = LogPut(cursor, directory);
                cursor = LogPut(cursor, "] does not exist -- ignored\r\n");
                directory[0] = 0;
            }

            if (program[0] == '\\' || (program[0] && program[1] == ':'))
            {
                lstrcpynA(pifCandidate, program, sizeof pifCandidate);
                pifHandle = CreateFileA(pifCandidate, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
            }
            else
            {
                PCSTR roots[2];
                INT rootIndex;
                roots[0] = directory;
                roots[1] = pifDirectory;

                for (rootIndex = 0; rootIndex < 2 && pifHandle == INVALID_HANDLE_VALUE; ++rootIndex)
                {
                    PSTR wordStart;

                    if (!roots[rootIndex][0])
                        continue;

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

            if (pifHandle == INVALID_HANDLE_VALUE)
            {
                /* Not found: do NOT run the PIF's bytes. A shell is the honest answer. */
                readCount = 0;
                programPathBuffer[0] = 0;
                args[0] = 0;
                cursor = LogPut(cursor, " -- PROGRAM NOT FOUND, starting a shell instead\r\n");
            }
            else
            {
                PSTR wordStart;
                readCount = 0;
                ReadFile(pifHandle, g_FileBuffer, sizeof(g_FileBuffer), &readCount, NULL);
                CloseHandle(pifHandle);
                lstrcpynA(programPathBuffer, pifCandidate, sizeof programPathBuffer);
                /* "?" asks Windows to prompt for parameters; there is no one to ask here. */
                wordStart = LogPut(args, (pif.Parameters[0] == '?' && !pif.Parameters[1]) ? "" : pif.Parameters);

                if (extra[0])
                {
                    if (args[0])
                        wordStart = LogPut(wordStart, " ");

                    LogPut(wordStart, extra);
                }

                LogPut(g_WowCommandProgram, programPathBuffer);
                WowShorten(g_WowCommandProgram, sizeof g_WowCommandProgram);
                LogPut(g_WowCommandArguments, args);

                if (directory[0])
                {
                    lstrcpynA(g_CurrentDirectory, directory, sizeof g_CurrentDirectory);
                    SetCurrentDirectoryA(g_CurrentDirectory);
                }

                cursor = LogPut(cursor, " -- loaded 0x"); cursor = LogHex(cursor, readCount);
                cursor = LogPut(cursor, " from ["); cursor = LogPut(cursor, programPathBuffer);
                cursor = LogPut(cursor, "] args=["); cursor = LogPut(cursor, args); cursor = LogPut(cursor, "]\r\n");
            }
        }
    }

    *cursorIo = cursor;
    *readCountIo = readCount;
}

/* Load the program from the command title when it is an absolute path (the title arrives with a trailing space). */
static VOID StartupLoadTitlePath(
    PSTR *cursorIo,
    DWORD *readCountIo,
    CHAR *programPathBuffer,
    CHAR *args)
{
    PSTR cursor = *cursorIo;
    DWORD readCount = *readCountIo;

    /* -- AN ABSOLUTE TITLE IS NOT JOINED TO THE DIRECTORY. (GH #131,
     * session 57) This composed `g_CurrentDirectory + "\" + g_Title` unconditionally, and
     * CSRSS's title for a program named with a full path at a cmd prompt IS
     * that full path -- so `hello.com > out.txt` run in C:\test produced
     *
     *     STAGE1: program C:\test\C:\test\hello.com
     *     STAGE2: loaded 0x00000000 from C:\test\C:\test\hello.com
     *     STAGE2: embedded fallback
     *
     * -- a path that cannot exist, a load of zero bytes, and then the four-byte
     * `mov ah,4Ch / int 21h` stub running INSTEAD OF THE PROGRAM. The run then
     * completes cleanly and writes NOTHING, which is indistinguishable from
     * the redirect defect this issue is about: the output file is empty either
     * way. It is not the same bug, and it was hiding behind it -- the log
     * reported ZERO INT 21h calls in the whole run, which is the tell.
     *
     * [CAUTION]: TWO FORMS OF ABSOLUTE, both real: `C:\...` (a drive) and `\...` (rooted
     * on the current drive). A relative title still gets the directory, which
     * is what the join was for and is still right.
     */
    if (!readCount && g_Title[0]
        && (g_Title[0] == '\\' || (g_Title[1] == ':' && g_Title[2] == '\\')))
    {
        CHAR path[768];
        HANDLE fileHandle;
        int targetLength;
        PSTR targetArguments = NULL; /* stays int: INT here moves the compiled code */
        LogPut(path, g_Title);
        /* [CAUTION]: AND THE TITLE ARRIVES WITH A TRAILING SPACE -- measured:
         * `title=[C:\test\hello.com ]`. CreateFileA's treatment of one is not
         * something to rely on, and a path is not a place to leave whitespace
         * the guest never typed.
         */
        for (targetLength = 0; path[targetLength]; ++targetLength) ;

        while (targetLength > 0 && (path[targetLength - 1] == ' ' || path[targetLength - 1] == '\t'))
            path[--targetLength] = 0;

        fileHandle = CsrssOpenSplit(path, &targetArguments);        /* "program [args]" -- see the helper */

        if (fileHandle != INVALID_HANDLE_VALUE)
        {
            ReadFile(fileHandle, g_FileBuffer, sizeof(g_FileBuffer), &readCount, NULL);
            CloseHandle(fileHandle);
        }

        LogPut(programPathBuffer, path);
        /* The title's own arguments beat CSRSS's CmdLine field, which arrives as
         * junk on this path (measured: `cmd=[\]` for a `ZAR.EXE -Help` launch) and
         * was only ever a best-effort guess.
         */
        if (targetArguments)
            LogPut(args, targetArguments);
        else if (g_CommandLine[0])
            LogPut(args, g_CommandLine);

        cursor = LogPut(cursor, "STAGE2: loaded 0x"); cursor = LogHex(cursor, readCount);
        cursor = LogPut(cursor, " from "); cursor = LogPut(cursor, path);

        if (targetArguments)
        {
            cursor = LogPut(cursor, " args=[");
            cursor = LogPut(cursor, targetArguments);
            cursor = LogPut(cursor, "]");
        }

        cursor = LogPut(cursor, " [the title is ALREADY ABSOLUTE -- not joined to the"
                    " current directory]\r\n");
    }

    *cursorIo = cursor;
    *readCountIo = readCount;
}

/* Load the application CSRSS named in the second fetch, recognising the harness's stub. */
static VOID StartupLoadCsrssApplication(
    PSTR *cursorIo,
    INT *wantShellIo,
    DWORD *readCountIo,
    CHAR *programPathBuffer,
    CHAR *args)
{
    PSTR cursor = *cursorIo;
    INT wantShell = *wantShellIo;
    DWORD readCount = *readCountIo;

    /* Load the program: what CSRSS asked for, else C:\ntvdmex\target.txt, else a
     * tiny exit stub.
     *
     * [CAUTION]: THAT ORDER USED TO BE THE OTHER WAY ROUND, AND IT IS A BLOCKER ON #130.
     * target.txt won UNCONDITIONALLY, so with NTVDMEX installed as the machine's
     * VDM *every* DOS and Win16 launch ran whatever that file happened to name,
     * whatever the user double-clicked. It also silently corrupted our own
     * measurements: the launch matrix's stock column reported DPMI output under
     * `p_ver.com` because a run had reused a stale target.
     * - THE TEST HARNESS IS UNAFFECTED, and that is measured rather than hoped:
     *   on the rig CSRSS hands back `title=[]` with no program name at all
     *   (STAGE1: program C:\test\ -- an empty tail), so the override still
     *   applies there. It stops applying exactly when someone actually asked for
     *   a program, which is the only case that was ever wrong.
     */
    /* THE COMMAND CSRSS QUEUED WINS OVER EVERYTHING BELOW. (s72) (Importance = 5):
     * AppName is the program's full path and CmdLine its tail, straight from the
     * launcher's CreateProcess -- no title heuristics, no quote-splitting, no join
     * with the current directory, and target.txt is not consulted. The title and
     * target.txt paths below remain for the shapes where this fetch does not answer
     * (the rig harness's dosstub.com + target.txt, where the queued command IS the
     * stub and the file names the real target -- kept by a stub-named check).
     */
    if (g_Fetch2Ok)
    {
        HANDLE fileHandle = CreateFileA(g_Application2, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        INT isStub = 0;
        {   PCSTR baseName = g_Application2, scan;

            for (scan = g_Application2; *scan; ++scan)
                if (*scan == '\\' || *scan == '/')
                    baseName = scan + 1;

            isStub   = (lstrcmpiA(baseName, HOST_HARNESS_STUB_NAME) == 0);
            /* [INFO]: OUR OWN LAUNCHER'S STUB -- see LAUNCH_STUB_NAME. Same mechanism, a
             * DIFFERENT meaning: the harness stub says "target.txt names the
             * program", this one says "the user opened NTVDMEX, give them a shell".
             * Keeping them apart is what stops a stale target.txt from hijacking a
             * double-click on someone's machine.
             */
            if (lstrcmpiA(baseName, LAUNCH_STUB_NAME) == 0)
            {
                isStub = 1;
                wantShell = 1;
            }
            }

        if (isStub)
        {
            if (fileHandle != INVALID_HANDLE_VALUE)
                CloseHandle(fileHandle);

            cursor = LogPut(cursor, wantShell
                     ? "STAGE2: launched with no program -- the user opened NTVDMEX; going straight to a SHELL\r\n"
                     : "STAGE2: CSRSS queued dosstub.com -- the harness stub; target.txt names the program\r\n");
            g_Title[0] = 0;                              /* and the title must not either */
        }
        else if (fileHandle != INVALID_HANDLE_VALUE)
        {
            ReadFile(fileHandle, g_FileBuffer, sizeof(g_FileBuffer), &readCount, NULL);
            CloseHandle(fileHandle);
            LogPut(programPathBuffer, g_Application2);
            LogPut(args, g_CommandLine2);
            LogPut(g_WowCommandProgram, g_Application2);
            WowShorten(g_WowCommandProgram, sizeof g_WowCommandProgram);
            LogPut(g_WowCommandArguments, g_CommandLine2);
            cursor = LogPut(cursor, "STAGE2: loaded 0x"); cursor = LogHex(cursor, readCount);
            cursor = LogPut(cursor, " from "); cursor = LogPut(cursor, g_Application2);
            cursor = LogPut(cursor, " args=[");
            cursor = LogPut(cursor, args);
            cursor = LogPut(cursor, "] (CSRSS AppName + CmdLine; target.txt and the title NOT consulted)\r\n");
            g_Title[0] = 0;                              /* the title chain below must not re-load */
        }
        else
        {
            cursor = LogPut(cursor, "STAGE2: CSRSS AppName ["); cursor = LogPut(cursor, g_Application2);
            cursor = LogPut(cursor, "] cannot be opened (0x"); cursor = LogHex(cursor, GetLastError());
            cursor = LogPut(cursor, ") -- falling back to the title / target.txt\r\n");
        }
    }

    *cursorIo = cursor;
    *wantShellIo = wantShell;
    *readCountIo = readCount;
}

/* CSRSS handed us a command: record it, report the path we will actually use, and set up stdio from
 * the handles it came with.
 */
static VOID StartupTakeCsrssCommand(PSTR *cursorIo, DWORD *errorIo)
{
    PSTR cursor = *cursorIo;
    DWORD error = *errorIo;

    if (CsrssGetCommand(&g_CommandInfo, &error))
    {
        /* [CAUTION]: PRINT THE PATH WE WILL ACTUALLY USE. This joined the directory and
         * the title unconditionally and so reported `C:\test\C:\test\hello.com`
         * for a title that was already absolute -- a path that cannot exist,
         * in the one line a reader checks first. An instrument that composes a
         * string the loader does not use is an instrument that lies.
         */
        cursor = LogPut(cursor, "STAGE1: program ");

        if (g_Title[0] == '\\' || (g_Title[1] == ':' && g_Title[2] == '\\'))
            cursor = LogPut(cursor, g_Title);
        else
        {
            cursor = LogPut(cursor, g_CurrentDirectory);
            cursor = LogPut(cursor, HOST_PATH_SEPARATOR);
            cursor = LogPut(cursor, g_Title);
        }

        cursor = LogPut(cursor, "\r\n");
        /* #129: the OTHER half of the launch shape. CSRSS hands back the app name,
         * the command tail, the PIF and a set of flags -- any of which may be what
         * actually distinguishes a WOW launch from a DOS one. Print them all rather
         * than guessing which one matters; a trace that prints the request but not
         * the answer is half an instrument.
         */
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
        /* -- THE VDM'S STANDARD HANDLES COME FROM CSRSS, NOT FROM INHERITANCE.
         * (GH #131) Measured on the rig: an IFEO-substituted process gets NO
         * inherited handles at all -- GetStdHandle reports FILE_TYPE_UNKNOWN
         * and AttachConsole(ATTACH_PARENT_PROCESS) fails -- so the obvious
         * route ("we are cmd's child, use its stdout") simply does not work
         * and reported `none (no console, no redirect)`.
         * CSRSS hands them over here instead, in the STARTUPINFO it fills in
         * for GetNextVDMCommand, already duplicated into this process. That is
         * how stock ntvdm gets them, and it is the only channel that carries
         * a redirect the user typed at cmd.
         */
        /* - PRINT THE RAW HANDLES AND THEIR TYPES **BEFORE** DECIDING ANYTHING.
         * Four routes have been eliminated here already, and each cost a run
         * because the log said which route was CHOSEN and never what the
         * candidates actually WERE. A handle value with a file type beside it
         * settles "is there a redirect on this VDM at all" in one line, for
         * every one of the three streams, whether or not we end up using it.
         */
        {   PCSTR handleNames[STD_HANDLE_REPORTS];
        HANDLE handles[STD_HANDLE_REPORTS];
        INT item;
            handleNames[0] = "vdm.StdIn";
            handles[0] = g_CommandInfo.StdIn;
            handleNames[1] = "vdm.StdOut";
            handles[1] = g_CommandInfo.StdOut;
            handleNames[2] = "vdm.StdErr";
            handles[2] = g_CommandInfo.StdErr;
            handleNames[3] = "si.hStdOut";
            handles[3] = g_CommandInfo.StartupInfo.hStdOutput;
            handleNames[4] = "si.hStdIn";
            handles[4] = g_CommandInfo.StartupInfo.hStdInput;
            cursor = LogPut(cursor, "STAGE1: vdm handles");

            for (item = 0; item < STD_HANDLE_REPORTS; ++item)
            {
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

        if (g_StdioSource[0])
        {
            cursor = LogPut(cursor, g_StdioSource);
            cursor = LogPut(cursor, " -> ");
        }

        cursor = LogPut(cursor, g_StdioHow);
        cursor = LogPut(cursor, "\r\n");
    }
    else
    {
        cursor = LogPut(cursor, "STAGE1: GetNextVDMCommand FALSE err=0x"); cursor = LogHex(cursor, error); cursor = LogPut(cursor, "\r\n");
    }

    *cursorIo = cursor;
    *errorIo = error;
}

/* GH #128, on a Win16 launch: log the TIB the kernel handed us, then probe the LDT and the selectors. */
static VOID StartupWowSelectorStage(VOID)
{
    if (g_WowModuleCount)                          /* GH #128: WOW selector stage */
    {
        /* [WARNING]: NO FLUSH HERE, AND NEVER THROUGH `base`. (s73) This read
         * `LogAppend(LOG_PATH, base, p); p = base;` from e595c91 (s68), which turned
         * every `report` flush into a `base` flush mechanically -- right for the exit
         * report, where base marks the end of the preamble, and WRONG here, 1,500
         * lines before `base = p` is ever executed. So: LogAppend(NULL, p) -- a bad
         * range -- then p = NULL, then every STAGE1 line was zput FROM ADDRESS 0, which
         * in a VDM process is the guest's IVT and BDA. krnl386 then ran on a trashed
         * interrupt table and the VDM died silently at PMHB 0x85. Win16 was dead from
         * s68 to s73 and nothing noticed: no Win16 program was launched after the wipe.
         * There is nothing to flush anyway -- the probes below log through LDTLOG_PATH,
         * and report[] has ~5 KB of headroom at this point; the preamble goes to disk
         * as one piece at the LogWrite after the command fetch.
         */
        /* The DOS bisection puts the flip between CsrssGetCommand() and
         * VdmGetTib(). The latter is one call and costs nothing to try here, so
         * try it BEFORE concluding the blocker is the command fetch.
         */
        {   PVOID tib2 = VdmGetTib();
            CHAR tibLine[120];
            CHAR *tibCursor = tibLine;
            tibCursor = LogPut(tibCursor, "WOWTRY: v86_get_tib -> 0x"); tibCursor = LogHex(tibCursor, (DWORD)(ULONG_PTR)tib2);
            tibCursor = LogPut(tibCursor, "\r\n");
            LogAppend(LDTLOG_PATH, tibLine, tibCursor); }
        WowProbeLdtMatrix("wow-after-get-tib");
        WowProbeSelectors();
        /* [CAUTION]: NO EARLY RETURN ANY MORE. This used to `return WowRefuse(...)` here,
         * which was correct while the plan was "enter in protected mode" -- there was
         * nothing further to do. It is not, krnl386 is entered in V86, and a V86
         * entry needs the whole DOS machine underneath it: conventional memory, an
         * INT 21h that answers AH=52h, the IVT, INT 2Fh. All of that is built a few
         * hundred lines below. So fall through and let it be built.
         * Everything WOW-specific past this point is gated on g_WowModuleCount, which is 0
         * on a DOS launch -- so the DOS path, which is the half that WORKS, sees no
         * change at all. That gating is deliberate and worth preserving.
         */
    }
}

/* Give the VDM state word at [0x714] a defined starting value -- zero, or cfg\vdmstate.txt -- instead of
 * whatever the machine's boot left there, and report what was inherited.
 */
static PSTR StartupCheckInheritedVdmState(PSTR cursor)
{
    /* -- [WARNING] FIXED_NTVDMSTATE ([0x714]) IS INHERITED GARBAGE UNTIL SOMEONE WRITES IT.
     * (2026-09-12: "no DOS app runs on the rig" after a REBOOT, host gone in <1 s.)
     * VdmInitialize does not define this word; it holds whatever the machine's
     * real-mode boot left in physical low memory, so it changes PER BOOT. Since the
     * Sep 9 boot it read 0xc0003232 and everything worked; after this morning's
     * reboot it read 0xc0002979 -- bit 0 (VDM_INT_HARDWARE pending) SET -- and the
     * kernel dutifully raised VIP in the guest's very first EFLAGS (0x00130002).
     * The first STI with VIP set is a raw #GP, and XP tears the VDM down silently
     * (run 71 watched it under a kernel debugger; see DpmiAsyncInjectPm). So the
     * host died after its first IRQ0 check on EVERY launch, the three-strikes
     * counter then removed the IFEO key, and every later launch was stock ntvdm.
     * - Stock ntvdm's live dump (build/stockdumps/130913, +0x714) reads 0x00300200:
     *   no 0xc000 high bits, nothing pending -- it starts from a DEFINED word. So do
     *   we, now: zero, or cfg\vdmstate.txt. The kernel sets bits 0-1 when it queues
     *   and dpmi_enter.S sets bit 9 on PM entry; nothing else needs to be pre-set.
     */
    {   volatile DWORD *vdmState = (volatile DWORD *)(ULONG_PTR)FIXED_NTVDMSTATE_LINEAR;
        DWORD inherited = *vdmState;
        DWORD want = 0;
        HANDLE handle = CreateFileA(VDMSTATE_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, 0, NULL);

        if (handle != INVALID_HANDLE_VALUE)
        {
            CHAR text[9];
            DWORD bytesRead = 0;
            INT index;
            ReadFile(handle, text, DWORD_HEX_DIGITS, &bytesRead, NULL);
            CloseHandle(handle);

            for (index = 0; index < (INT)bytesRead; ++index)
            {
                INT digit = -1;

                if (text[index] >= '0' && text[index] <= '9')
                    digit = text[index] - '0';
                else if (text[index] >= 'a' && text[index] <= 'f')
                    digit = text[index] - 'a' + HEX_DIGIT_A_VALUE;
                else if (text[index] >= 'A' && text[index] <= 'F')
                    digit = text[index] - 'A' + HEX_DIGIT_A_VALUE;

                if (digit < 0)
                    break;

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
 * keyboard, no-suspend and async keyboard (QIMODE_*).
 */
static VOID StartupLoadQiMode(VOID)
{
    /* Async-preemption mode (session 11). Read once; a handle to THIS thread is what
     * VdmQueueInterrupt takes, and this thread is the one that will be running the
     * guest inside VdmStartExecution -- so duplicate it here, before the exec loop.
     */
    { HANDLE handle = CreateFileA(QIMODE_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             NULL, OPEN_EXISTING, 0, NULL);

      if (handle != INVALID_HANDLE_VALUE)
      {
          CHAR text[2] = { 0, 0 };
          DWORD bytesRead = 0;
          INT number = 0;
          INT index;
          ReadFile(handle, text, QIMODE_DIGITS, &bytesRead, NULL);
          CloseHandle(handle);

          for (index = 0; index < (INT)bytesRead; ++index)            /* up to two hex digits */
          {
              INT digit = -1;

              if (text[index] >= '0' && text[index] <= '9')
                  digit = text[index] - '0';
              else if (text[index] >= 'a' && text[index] <= 'f')
                  digit = text[index] - 'a' + HEX_DIGIT_A_VALUE;
              else if (text[index] >= 'A' && text[index] <= 'F')
                  digit = text[index] - 'A' + HEX_DIGIT_A_VALUE;

              if (digit < 0)
                  break;

              number = (number << NIBBLE_SHIFT) | digit;
          }

          if (number > 0) { g_QiBits  = (DWORD)number & VDM_INT_PENDING;
                       g_QiRaise = (number & QIMODE_RAISE) != 0;
                       g_QiVif   = (number & QIMODE_VIF) != 0;

                       if (number & QIMODE_NO_SUSPEND)
                           g_QiSuspended = 0;                                  /* bit 6 disables async delivery */

                       g_QiKeys  = (number & QIMODE_KEYS) != 0;
                       g_QiKeysAsync = (number & QIMODE_KEYS_ASYNC) != 0; }
      } }
}

/* A Win16 launch is latched here, where the answer is known, before the UI thread that decides on a window
 * starts. It logs the STAGE0 lines and is refused through WowRefuse -- unless wowtry.flag asks for the
 * WOW probe instead.
 */
static INT StartupLatchWowLaunch(PSTR *cursorIo, CHAR *report, INT *exitCodeOut)
{
    PSTR cursor = *cursorIo;

    /* IS THIS A WIN16 (WOW) LAUNCH? IF SO, HAND IT STRAIGHT BACK. (GH #129):
     * Windows runs 16-bit WINDOWS programs inside the SAME ntvdm.exe it uses for
     * DOS, so our IFEO Debugger hook catches both -- and we implement only the DOS
     * half. Left unhandled, installing NTVDMEX breaks every Win16 program on the
     * machine. This is the guard that makes "leave it installed" safe.
     * - MEASURED, not assumed (both captured on the rig, 2026-08-26):
     *   DOS : ntvdmhost.exe "...\ntvdm.exe" -f -i20
     *   WOW : ntvdmhost.exe "...\ntvdm.exe" -f -i1 -w -a ...\krnl386.exe
     *   `-w` is the discriminator and `-a <krnl386>` is the WOW bootstrap. A second,
     *   independent tell: GetNextVDMCommand returns FALSE err=0x57 on a WOW launch,
     *   because a WOW VDM does not receive its program that way.
     * - WE CANNOT HAND IT BACK. Three routes measured and eliminated -- see
     *   WowRefuse() below. So this refuses loudly instead, which is at least an
     *   accurate, actionable failure rather than a DOS host chewing on an NE file.
     * - NOT a throwaway. When the WOW epic (#128) lands, this same detection becomes
     *   the dispatch point -- the `-w` arm routes to our WOW layer.
     */
    if (LaunchIsWow(GetCommandLineA()))
    {
        /* [INFO]: Latched HERE because this is where the answer is known, and the UI
         * thread -- which decides whether to show a window -- is started later.
         * See the note by TrayAdd for why a Win16 guest gets no window.
         */
        g_WowLaunch = 1;
        cursor = LogPut(cursor, "STAGE0: WIN16/WOW launch detected -> refusing (see GH #129)\r\n");
        cursor = LogPut(cursor, "STAGE0: root=["); cursor = LogPut(cursor, NTVDMEX_DIR); cursor = LogPut(cursor, "] (derived from the host's own path)\r\n");
        HmaTry();
        cursor = LogPut(cursor, "STAGE0: HMA ");

        if (g_Hma)
            cursor = LogPut(cursor, "committed at 0x100000 -- FFFF:0010 is real");
        else { cursor = LogPut(cursor, "UNAVAILABLE err=0x"); cursor = LogHex(cursor, g_HmaError);
       cursor = LogPut(cursor, " state=0x"); cursor = LogHex(cursor, g_HmaState);
       cursor = LogPut(cursor, " prot=0x");
       cursor = LogHex(cursor, g_HmaProtection); }

        cursor = LogPut(cursor, "\r\n");
        cursor = LogPut(cursor, "STAGE0: cmdline=["); cursor = LogPut(cursor, GetCommandLineA()); cursor = LogPut(cursor, "]\r\n");
        LogAppend(LOG_PATH, report, cursor); SerialOut(report, cursor);

        if (GetFileAttributesA(WOWTRY_FLAG) == INVALID_FILE_ATTRIBUTES)
            {
                *cursorIo = cursor;
                *exitCodeOut = WowRefuse(GetCommandLineA());
                return HOST_FLOW_RETURN;
            }

        /* Experiment opted in: load now, then fall through so the selector stage can
         * run once the VDM is registered. Still refuses at the end -- nothing here
         * executes guest code yet.
         */
        WowProbeLoad(GetCommandLineA());
    }

    *cursorIo = cursor;
    return HOST_FLOW_NEXT;
}

/* Claim an instance slot (#211): the first host writes to debug\out\, the Nth to debug\out\N\; refuse once all are taken. */
static INT StartupClaimInstance(INT *exitCodeOut)
{
    /* [WARNING]: ONE HOST AT A TIME (Importance = 2):
     * Nothing stopped a second instance, and two of them fight over things that
     * are SYSTEM-WIDE, not per-process: the low-level keyboard hook, the cursor
     * clip, exclusive-fullscreen DirectDraw, and the guest's suspend count. The
     * user hit it directly -- "I closed your Skyroads run, and reran it. That
     * basically crashed Windows" -- because closing does not necessarily finish
     * (a suspended guest thread keeps the process alive; see HostPanicRelease),
     * so the rerun landed ON TOP of a zombie that still owned the keyboard.
     *
     * [CAUTION]: Bail SILENTLY and with success. This is launched by the IFEO Debugger key
     * on every 16-bit start, so a message box here would be a modal dialog on a
     * machine that is already confused -- and a failure exit code would make the
     * launch look broken rather than declined.
     */
    /* [CAUTION]: GetLastError() IS ONLY MEANINGFUL IMMEDIATELY AFTER THE CALL, AND THE FIRST
     * CUT OF THIS GUARD REFUSED EVERY LAUNCH. CreateDirectoryA above fails with
     * ERROR_ALREADY_EXISTS whenever cfg\ exists -- i.e. always, after the first run
     * -- and CreateMutexA does NOT clear the last-error value when it succeeds. So
     * the guard read the DIRECTORY's error, concluded another host was running, and
     * exited: the rig went silent, no log at all, and the run timed out.
     * Clear it first and latch it immediately. Same do-nothing-and-look-fine shape
     * as everything else this file warns about.
     */
    /* [CAUTION]: AND A HELD MUTEX IS NOT PROOF A HOST IS ALIVE. (s63) The value of the guard
     * is entirely in NOT refusing a launch when the "other" instance is a corpse.
     * The named object exists as long as ANY handle to it is open -- including one
     * held by a wedged zombie -- so ERROR_ALREADY_EXISTS on its own said "refuse"
     * even when the previous host had left the machine (that is the flash-and-vanish
     * the user hit). So do not decide on the flag: TRY TO ACQUIRE it. A host that is
     * really running owns it (created with bInitialOwner) and the wait TIMES OUT ->
     * refuse, correctly, one-host-at-a-time. If the owner released it or its thread
     * died the wait returns signalled or ABANDONED -> we take it and run. The
     * window-close path now TerminateProcess()es (see UiThread), so the common
     * zombie is gone at the source; this makes the guard safe against any that slip
     * through rather than turning them into a permanent lockout.
     */
    /* #211: MORE THAN ONE HOST AT A TIME. (s81):
     * Stock XP runs one ntvdm.exe per DOS window, and so do we now. The mutex below
     * was a one-host-only guard (s63); it is now a CLAIM ON AN INSTANCE NUMBER. The
     * first host takes instance 1 under the original name and changes nothing --
     * same log, same debug\out\, so the rig harness is untouched. A host started
     * while it runs takes the lowest free number N and writes to debug\out\N\.
     * - What the old guard protected is covered elsewhere now: the keyboard hook and
     *   the cursor clip are held only while a window has captured the mouse (and only
     *   one can), and a closed window's process is terminated, so the half-dead host
     *   that "basically crashed Windows" no longer outlives its window.
     *
     * [CAUTION]: The acquire rule is unchanged, per name: TRY to take it. A live host never
     * yields its mutex (the wait times out -> next number); a dead one's is released
     * or ABANDONED (-> we take that number).
     */
    {   INT attempt;
    CHAR name[48];

        for (attempt = 1; attempt <= HOST_INSTANCES_MAX && !g_OnceMutex; ++attempt)
        {
            HANDLE once;
            DWORD lastError;
            DWORD waitResult = WAIT_OBJECT_0;
            PSTR scan = LogPut(name, HOST_INSTANCE_MUTEX);

            if (attempt > 1)
            {
                *scan++ = '_';
                scan = LogDecimal(scan, (UINT)attempt);
            }

            /* [CAUTION]: GetLastError() IS ONLY MEANINGFUL IMMEDIATELY AFTER THE CALL -- clear
             * it first; CreateMutexA does not clear it on success. (The first cut of
             * the guard read CreateDirectoryA's ERROR_ALREADY_EXISTS and refused.)
             */
            SetLastError(0);
            once = CreateMutexA(NULL, TRUE, name);
            lastError  = GetLastError();

            if (!once)
                continue;

            if (lastError == ERROR_ALREADY_EXISTS)
            {
                waitResult = WaitForSingleObject(once, HOST_INSTANCE_WAIT_MS);   /* brief: a live host never yields it */

                if (waitResult == WAIT_TIMEOUT)
                {
                    CloseHandle(once);
                    continue;
                }
            }

            g_OnceMutex = once;

            if (attempt > 1)
            {
                PSTR position = LogPut(g_OutSubdirectory, HOST_OUT_SUBDIRECTORY);
                position = LogDecimal(position, (UINT)attempt); LogPut(position, HOST_PATH_SEPARATOR);
                CreateDirectoryA(NTVDMEX_OUT, NULL);
            }

            g_Instance = attempt;
            g_InstanceAbandoned = (waitResult == WAIT_ABANDONED);   /* reported at STAGE0 */
        }

        if (!g_OnceMutex)
        {
            static const CHAR message[] = "REFUSED: 16 NTVDMEX hosts are already running -- this instance is exiting\r\n";
            LogAppend(LOG_PATH, message, message + sizeof(message) - 1);
            {
                *exitCodeOut = 0;
                return HOST_FLOW_RETURN;
            }
        } }

    return HOST_FLOW_NEXT;
}

/* The install verbs (/install, /uninstall, /status), run before anything else exists, and exit. */
static INT StartupRunInstallVerb(INT *exitCodeOut)
{
    /* THE INSTALL VERBS, BEFORE ANYTHING ELSE EXISTS. (GH #13) (Importance = 1):
     * `ntvdmhost.exe /install`, `/uninstall`, `/status`. They run and exit without
     * touching the VDM, the log, COM1 or the recovery counter -- none of which
     * should move because somebody asked whether we are installed.
     *
     * [WARNING]: AND THIS BLOCK USED TO SIT BELOW THE SINGLE-INSTANCE GUARD, WHICH MADE
     * `install.bat` LIE. (2026-09-22) The guard returns 0 -- success, silently, no
     * output -- when another host owns the mutex. A verb arriving while ANY guest
     * was on screen therefore printed NOTHING and exited 0, and install.bat, which
     * branches on the exit code alone, announced "Installed. Every MS-DOS and
     * 16-bit Windows program now runs under NTVDMEX" having written nothing to the
     * registry at all. smoke.bat's `/status` gate passed for the same reason and
     * then failed with "no log was written" -- the exact report from the user's
     * Windows 2000 box ("installed, apparently; smoke does not run; no logs").
     * A verb is a command-line utility invocation, not a VDM launch: it must never
     * be subject to a guard about how many VDMs are running.
     *
     * [CAUTION]: THE VERB MUST BE THE FIRST ARGUMENT, and that is what makes this safe to put
     * ahead of every other launch shape. Windows hands an IFEO-substituted VDM the
     * ORIGINAL command line, whose first argument is always the path to ntvdm.exe,
     * so a real VDM launch can never look like a verb -- while a token matched
     * anywhere on the line could be, one day, a DOS program's argument.
     *
     * [CAUTION]: Output goes to stdout when there is one and a message box when there is not,
     * because this is the one part of the host that is run BOTH from a prompt and
     * by double-clicking. Reporting into a console nobody can see is how an
     * installer becomes "it did nothing".
     */
    {   CHAR installStatus[2048];
        INT verb = InstallVerb(GetCommandLineA());

        if (verb > INSTALL_VERB_NONE)
        {
            /* [CAUTION]: `verb == 0`, NOT `verb != 2`. The first cut wrote the latter, which
             * makes INSTALL and UNINSTALL both ask to be installed -- and it
             * reported "INSTALLED" cheerfully while doing it, because the message
             * is composed from the same wrong flag. Caught on the rig by the
             * BEHAVIOURAL half of the gate, not by the registry read.
             */
            INT isOk;
            INT want = (verb == INSTALL_VERB_INSTALL);
            installStatus[0] = 0;

            if (verb == INSTALL_VERB_STATUS)
            {
                /* /status ANSWERS IN ITS EXIT CODE, not only in English:
                 * 0 = NTVDMEX is the machine's VDM, 1 = nobody is, 2 = another
                 * program is. A script can branch on that without matching a
                 * sentence -- which is exactly what package/smoke.bat was doing
                 * wrongly, grepping for text only /install ever prints.
                 */
                INSTALL_STATE installState = InstallStatusText(installStatus, sizeof installStatus);
                InstallReport(installStatus, TRUE);
                {
                    *exitCodeOut = installState == INSTALL_OURS ? INSTALL_STATUS_EXIT_OURS : (installState == INSTALL_OTHER ? INSTALL_STATUS_EXIT_OTHER : INSTALL_STATUS_EXIT_NONE);
                    return HOST_FLOW_RETURN;
                }
            }

            isOk = InstallPerform(want, CommandLineHasForce(GetCommandLineA()), installStatus, sizeof installStatus);
            InstallReport(installStatus, isOk);
            {
                *exitCodeOut = isOk ? INSTALL_EXIT_OK : INSTALL_EXIT_FAILED;
                return HOST_FLOW_RETURN;
            }
        } }

    return HOST_FLOW_NEXT;
}

/* cfg\target.txt: the harness's way of naming the program -- consulted only when neither CSRSS nor the user (asking for a shell) named one (GH #130). */
static VOID StartupLoadTarget(
    PSTR *cursorIo,
    DWORD *readCountIo,
    const INT wowCommandFromCsrss,
    const INT wantShell,
    CHAR *programPathBuffer,
    CHAR *args)
{
    PSTR cursor = *cursorIo;
    DWORD readCount = *readCountIo;

    {
        INT csrssNamed = (g_CurrentDirectory[0] && g_Title[0]) || (readCount != 0) || wowCommandFromCsrss;
        /* [CAUTION]: `want_shell` skips target.txt ENTIRELY. A user who opened NTVDMEX asked for
         * a shell, not for whatever the last test run happened to leave in cfg\.
         */
        HANDLE thread = (csrssNamed || wantShell)
                  ? INVALID_HANDLE_VALUE
                  : CreateFileA(TARGET_PATH, GENERIC_READ, FILE_SHARE_READ, NULL,
                                OPEN_EXISTING, 0, NULL);

        if (csrssNamed && !readCount)
        {
            cursor = LogPut(cursor, "STAGE2: CSRSS named a program -- target.txt NOT consulted "
                        "(GH #130)\r\n");
        }

        if (thread != INVALID_HANDLE_VALUE)
        {
            CHAR tempPath[512];
            DWORD tempLength = 0;
            PSTR scan;
            PSTR cursorA = 0;
            ReadFile(thread, tempPath, sizeof(tempPath) - 1, &tempLength, NULL);
            CloseHandle(thread);
            tempPath[tempLength < sizeof(tempPath) ? tempLength : sizeof(tempPath) - 1] = 0;
            /* -- [CAUTION] A PROGRAM PATH MAY CONTAIN SPACES, AND THIS SPLIT ON THE FIRST ONE.
             * Every path the rig used to hand us was C:\game\X.EXE or C:\test\X.COM,
             * so "first space starts the arguments" was never wrong -- until the rig
             * moved into the share folder, whose path contains "Documents and
             * Settings". Measured, first run after the move:
             *
             *   target.txt loaded 0x0 from C:\Documents
             *     args=[and Settings\All Users\...\games\Skyroads\Skyroads.EXE]
             *
             * A zero-byte load, then the embedded four-byte `mov ah,4Ch / int 21h`
             * stub runs INSTEAD of the game and the run completes cleanly -- the
             * same silent-success shape as GH #131 below, and it reports `mode
             * sets: none` rather than any kind of error.
             *
             * So honour QUOTES, and treat an unquoted line as a bare path with no
             * arguments when what it names actually exists. A quoted first token is
             * unambiguous and is what every Windows caller already writes.
             */
            scan = tempPath;

            if (*scan == '"')                              /* "path with spaces" [args] */
            {
                PSTR word2 = scan;
                ++scan;

                while (*scan && *scan != '"')
                    *word2++ = *scan++;

                if (*scan == '"')
                    ++scan;

                *word2 = 0;

                while (*scan == ' ')
                    ++scan;

                {
                    PSTR limit;

                    for (limit = scan; *limit; ++limit) if (*limit == '\r' || *limit == '\n')
                    {
                        *limit = 0;
                        break;
                    }
                }

                if (*scan)
                    cursorA = scan;
            }
            else
            {
                for (scan = tempPath; *scan; ++scan)                 /* trim EOL first */
                    if (*scan == '\r' || *scan == '\n')
                    {
                        *scan = 0;
                        break;
                    }

                /* Unquoted: only split on a space if the WHOLE line is not itself a
                 * file. That keeps `C:\test\x.com >out.txt` working and stops a path
                 * with spaces being torn in half.
                 */
                { HANDLE probe = CreateFileA(tempPath, GENERIC_READ, FILE_SHARE_READ, NULL,
                                             OPEN_EXISTING, 0, NULL);

                  if (probe != INVALID_HANDLE_VALUE)
                      CloseHandle(probe);
                  else for (scan = tempPath; *scan; ++scan)
                           if (*scan == ' ')
                           {
                               *scan = 0;
                               cursorA = scan + 1;
                               break;
                           }
                           }
            }

            if (tempPath[0])
            {
                HANDLE fileHandle = CreateFileA(tempPath, GENERIC_READ, FILE_SHARE_READ, NULL,
                                        OPEN_EXISTING, 0, NULL);

                if (fileHandle != INVALID_HANDLE_VALUE)
                {
                    ReadFile(fileHandle, g_FileBuffer, sizeof(g_FileBuffer), &readCount, NULL);
                    CloseHandle(fileHandle);
                }

                LogPut(programPathBuffer, tempPath);                  /* env argv[0] */

                if (cursorA)
                    LogPut(args, cursorA);                            /* PSP command tail */

                /* [INFO]: GH #128: on a WOW launch this same path is the WIN16 program,
                 * and WOW32 0x70 is how WOWEXEC asks for it. The DOS image
                 * loaded just below is discarded there (see WowPlaceV86), but
                 * the NAME is the one thing the WOW path still needs -- Windows
                 * does not put it on the VDM's command line.
                 */
                LogPut(g_WowCommandProgram, tempPath);
                WowShorten(g_WowCommandProgram, sizeof g_WowCommandProgram);

                if (cursorA)
                    LogPut(g_WowCommandArguments, cursorA);

                cursor = LogPut(cursor, "STAGE2: target.txt loaded 0x"); cursor = LogHex(cursor, readCount);
                cursor = LogPut(cursor, " from "); cursor = LogPut(cursor, tempPath);

                if (cursorA && cursorA[0])
                {
                    cursor = LogPut(cursor, " args=[");
                    cursor = LogPut(cursor, cursorA);
                    cursor = LogPut(cursor, "]");
                }

                cursor = LogPut(cursor, "\r\n");
            }
        }
    }
    *cursorIo = cursor;
    *readCountIo = readCount;
}

/* The Win16 program, from CSRSS (s73): a WOW launch never carried its program on its command line, so ask CSRSS for it -- the second fetch, which names the program and its arguments. */
static VOID StartupFetchWowCommand(
    PSTR *cursorIo,
    INT *wowCommandFromCsrssIo,
    CHAR *args,
    CHAR *programPathBuffer,
    DWORD *readCountIo)
{
    PSTR cursor = *cursorIo;
    INT wowCommandFromCsrss = *wowCommandFromCsrssIo;
    DWORD readCount = *readCountIo;
    /* THE WIN16 PROGRAM, FROM CSRSS. (s73) (Importance = 4):
     * A WOW launch never carried its program: the VDM starts as
     * `ntvdm -f -i<n> -w -a krnl386.exe`, the first fetch above returns FALSE
     * err=0x57 (measured, s3x), and until today the name came ONLY from
     * cfg\target.txt -- the harness's channel -- so on any machine without
     * that file a double-clicked Win16 program ran nothing, and on the rig it
     * ran whatever the file happened to name last. Stock WOW gets it exactly
     * the way stock DOS does: WOWEXEC's WowGetNextVDMCommand (WOW32 0x70) is
     * wow32.dll calling GetNextVDMCommand with VDM_FLAG_WOW, and CSRSS answers
     * with the AppName + CmdLine the launcher queued. We ask here, before
     * krnl386 runs, so that 0x70 can answer from g_WowCommandProgram as it already
     * does. DONT_WAIT: a misunderstanding is a FALSE, never a hang.
     */
    VDM_COMMAND_INFO commandInfo2;
    DWORD error2 = 0;
    BOOL ok2;
    INT item;
    static CHAR pifFile2[512];
    static CHAR desktop2[512];
    static CHAR title2[512];
    static CHAR reservedBuffer2[512];

    ZeroMemory(&commandInfo2, sizeof commandInfo2);
    commandInfo2.CmdLine = g_CommandLine2;
    commandInfo2.CmdLen = sizeof g_CommandLine2;
    commandInfo2.AppName = g_Application2;
    commandInfo2.AppLen = sizeof g_Application2;
    commandInfo2.PifFile = pifFile2;
    commandInfo2.PifLen = sizeof pifFile2;
    commandInfo2.CurDirectory = g_CurrentDirectory2;
    commandInfo2.CurDirectoryLen = sizeof g_CurrentDirectory2;
    commandInfo2.Env = g_Environment2;
    commandInfo2.EnvLen = sizeof g_Environment2;
    commandInfo2.Desktop = desktop2;
    commandInfo2.DesktopLen = sizeof desktop2;
    commandInfo2.Title = title2;
    commandInfo2.TitleLen = sizeof title2;
    commandInfo2.Reserved = reservedBuffer2;
    commandInfo2.ReservedLen = sizeof reservedBuffer2;
    commandInfo2.StartupInfo.cb = sizeof(STARTUPINFOA);
    /* - THE SHAPE IS MEASURED, NOT ASSUMED (rig, s73). WOW|FIRST|DONT_WAIT alone
     * answers FALSE err=0x490 (ERROR_NOT_FOUND), with either task id. What
     * answers TRUE is GET_FIRST_COMMAND|WOW -- the same handshake stock ntvdm's
     * cmdGetStartInfo makes for DOS, and like the DOS one it fills Title/CurDir
     * and leaves AppName as capture-buffer junk ("[5??]"). So, as on the DOS
     * path: the GET_FIRST call first, then the FIRST_TASK fetch for the command
     * itself. Every call is DONT_WAIT and every answer is logged; a "name" is
     * only believed when it is drive-qualified or UNC -- TRUE with junk is not
     * a program.
     */
    {   static const struct
    {
        DWORD VdmState;
        INT IsOwnTask;
        INT IsHandshake;
        PCSTR Description;
    }
    vdmStates[] = {
            { VDM_GET_FIRST_COMMAND | VDM_FLAG_WOW | VDM_FLAG_DONT_WAIT, 1, 1, "GET_FIRST|WOW|DONT_WAIT taskid=-i (handshake)" },
            { VDM_FLAG_WOW | VDM_FLAG_FIRST_TASK | VDM_FLAG_DONT_WAIT, 1, 0, "WOW|FIRST|DONT_WAIT taskid=-i" },
            { VDM_FLAG_WOW | VDM_FLAG_FIRST_TASK | VDM_FLAG_DONT_WAIT, 0, 0, "WOW|FIRST|DONT_WAIT taskid=0"  },
            { VDM_FLAG_WOW | VDM_FLAG_DONT_WAIT, 1, 0, "WOW|DONT_WAIT taskid=-i"       },
            { VDM_FLAG_WOW | VDM_FLAG_FIRST_TASK | VDM_FLAG_RETRY | VDM_FLAG_DONT_WAIT, 1, 0, "WOW|FIRST|RETRY|DONT_WAIT taskid=-i" },
        };
        UINT si;
        INT named = 0;

        for (si = 0; si < sizeof vdmStates / sizeof vdmStates[0] && !named; ++si)
        {
            g_Application2[0] = 0;
            g_CommandLine2[0] = 0;
            g_CurrentDirectory2[0] = 0;
            error2 = 0;
            commandInfo2.AppLen = sizeof g_Application2;
            commandInfo2.CmdLen = sizeof g_CommandLine2;
            commandInfo2.CurDirectoryLen = sizeof g_CurrentDirectory2;
            commandInfo2.EnvLen = sizeof g_Environment2;
            commandInfo2.PifLen = sizeof pifFile2;
            commandInfo2.DesktopLen = sizeof desktop2;
            commandInfo2.TitleLen = sizeof title2;
            commandInfo2.ReservedLen = sizeof reservedBuffer2;
            commandInfo2.VDMState = vdmStates[si].VdmState;
            commandInfo2.TaskId = vdmStates[si].IsOwnTask ? g_CommandInfo.TaskId : 0;
            ok2 = CsrssGetCommand(&commandInfo2, &error2);
            g_Application2[sizeof g_Application2 - 1] = 0;
            g_CommandLine2[sizeof g_CommandLine2 - 1] = 0;
            g_CurrentDirectory2[sizeof g_CurrentDirectory2 - 1] = 0;
            named = ok2 && !vdmStates[si].IsHandshake
                    && ((g_Application2[0] >= 'A' && (g_Application2[0] | ASCII_CASE_BIT) <= 'z' && g_Application2[1] == ':' && g_Application2[2] == '\\')
                        || (g_Application2[0] == '\\' && g_Application2[1] == '\\'));
            cursor = LogPut(cursor, "STAGE1: WOW command fetch ["); cursor = LogPut(cursor, vdmStates[si].Description); cursor = LogPut(cursor, "] -> ");
            cursor = LogPut(cursor, ok2 ? "TRUE" : "FALSE");
            cursor = LogPut(cursor, " err=0x"); cursor = LogHex(cursor, error2);
            cursor = LogPut(cursor, " app=[");

            if (named)
                cursor = LogPut(cursor, g_Application2);
                else if (g_Application2[0])
                    cursor = LogPut(cursor, "<not a path>");

            cursor = LogPut(cursor, "] args=[");

            if (named)
                cursor = LogPut(cursor, g_CommandLine2);

            cursor = LogPut(cursor, "] cur=["); cursor = LogPut(cursor, g_CurrentDirectory2); cursor = LogPut(cursor, "] show=0x"); cursor = LogHex(cursor, commandInfo2.StartupInfo.wShowWindow);
            cursor = LogPut(cursor, " taskid=0x"); cursor = LogHex(cursor, commandInfo2.TaskId);
            cursor = LogPut(cursor, "\r\n");
        }

        ok2 = named;
    }

    for (item = 0; g_CommandLine2[item]; ++item) if (g_CommandLine2[item] == '\r' || g_CommandLine2[item] == '\n')
    {
        g_CommandLine2[item] = 0;
        break;
    }

    wowCommandFromCsrss = ok2 && g_Application2[0];

    if (wowCommandFromCsrss)
    {
        /* Exactly what the target.txt path does with a name, or the stage below
         * builds the V86 world for the embedded four-byte stub instead ("STAGE2:
         * embedded fallback"), krnl386 gets no program path in its environment,
         * and it dies in its own init: "NTVDM KERNEL: Unable to initialize heap".
         * Measured, first cut. The image read here is discarded by WowPlaceV86;
         * the NAME and the byte count are what the stage keys on.
         */
        HANDLE wowHandle;
        LogPut(g_WowCommandProgram, g_Application2);
        WowShorten(g_WowCommandProgram, sizeof g_WowCommandProgram);
        /* the tail arrives with its leading space, as a DOS tail does; 0x70 adds its own */
        {
            PCSTR cursorA = g_CommandLine2;

            while (*cursorA == ' ')
                ++cursorA;

            LogPut(g_WowCommandArguments, cursorA);
            LogPut(args, cursorA);
        }
        LogPut(programPathBuffer, g_WowCommandProgram);
        wowHandle = CreateFileA(g_WowCommandProgram, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);

        if (wowHandle != INVALID_HANDLE_VALUE)
        {
            ReadFile(wowHandle, g_FileBuffer, sizeof(g_FileBuffer), &readCount, NULL);
            CloseHandle(wowHandle);
        }

        if (g_CurrentDirectory2[0])
        {
            SetCurrentDirectoryA(g_CurrentDirectory2);
            /* #164: WOWEXEC changes to this before LoadModule, so the task starts
             * in the folder it was launched from -- 8.3, as krnl386 sees paths.
             */
            if (!GetShortPathNameA(g_CurrentDirectory2, g_WowCommandDirectory, sizeof g_WowCommandDirectory)
                || lstrlenA(g_WowCommandDirectory) >= WOW_COMMAND_DIRECTORY_MAX)
                g_WowCommandDirectory[0] = 0;
        }

        cursor = LogPut(cursor, "STAGE2: Win16 program from CSRSS -- LAUNCH ["); cursor = LogPut(cursor, g_WowCommandProgram);
        cursor = LogPut(cursor, "] loaded 0x"); cursor = LogHex(cursor, readCount); cursor = LogPut(cursor, " (target.txt NOT consulted)\r\n");

        if (!readCount)
            wowCommandFromCsrss = 0;                      /* unreadable: fall back as before */
    }

    *cursorIo = cursor;
    *wowCommandFromCsrssIo = wowCommandFromCsrss;
    *readCountIo = readCount;
}

/* The first fetch gave a directory or a title: fetch the command again, in full, for its application name, command line and the rest. */
static PSTR StartupFetchCommandDetails(PSTR cursor)
{
    VDM_COMMAND_INFO commandInfo2;
    DWORD error2 = 0;
    BOOL ok2;
    INT item;
    static CHAR pifFile2[512];
    static CHAR desktop2[512];
    static CHAR title2[512];
    static CHAR reservedBuffer2[512];

    ZeroMemory(&commandInfo2, sizeof commandInfo2);
    commandInfo2.CmdLine = g_CommandLine2;
    commandInfo2.CmdLen = sizeof g_CommandLine2;
    commandInfo2.AppName = g_Application2;
    commandInfo2.AppLen = sizeof g_Application2;
    commandInfo2.PifFile = pifFile2;
    commandInfo2.PifLen = sizeof pifFile2;
    commandInfo2.CurDirectory = g_CurrentDirectory2;
    commandInfo2.CurDirectoryLen = sizeof g_CurrentDirectory2;
    commandInfo2.Env = g_Environment2;
    commandInfo2.EnvLen = sizeof g_Environment2;
    commandInfo2.Desktop = desktop2;
    commandInfo2.DesktopLen = sizeof desktop2;
    commandInfo2.Title = title2;
    commandInfo2.TitleLen = sizeof title2;
    commandInfo2.Reserved = reservedBuffer2;
    commandInfo2.ReservedLen = sizeof reservedBuffer2;
    commandInfo2.StartupInfo.cb = sizeof(STARTUPINFOA);
    commandInfo2.VDMState = VDM_FLAG_DOS | VDM_FLAG_FIRST_TASK | VDM_FLAG_DONT_WAIT;              /* VDM_FLAG_DOS | FIRST_TASK | DONT_WAIT */
    commandInfo2.TaskId = g_CommandInfo.TaskId;
    ok2 = CsrssGetCommand(&commandInfo2, &error2);
    g_Application2[sizeof g_Application2 - 1] = 0;
    g_CommandLine2[sizeof g_CommandLine2 - 1] = 0;
    g_CurrentDirectory2[sizeof g_CurrentDirectory2 - 1] = 0;
    /* the tail ends in CR LF; the PSP wants neither */
    for (item = 0; g_CommandLine2[item]; ++item) if (g_CommandLine2[item] == '\r' || g_CommandLine2[item] == '\n')
    {
        g_CommandLine2[item] = 0;
        break;
    }

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
static VOID StartupStartGuest(
    PSTR *cursorIo,
    PSTR *baseIo,
    HANDLE *uiThreadIo,
    DOS_MACHINE *machine,
    DOS_IMAGE *image,
    volatile BYTE * const tib,
    CHAR *report)
{
    PSTR cursor = *cursorIo;
    PSTR base = *baseIo;
    HANDLE uiThread = *uiThreadIo;

    /* Hide the inherited console (CSRSS already bound the VDM); the Luna window
     * is now the display. Then start the UI thread that owns it.
     */
    g_KeyEvent = CreateEventA(NULL, FALSE, FALSE, NULL);   /* auto-reset */
    {
        HWND consoleWindow = GetConsoleWindow();

        if (consoleWindow)
            ShowWindow(consoleWindow, SW_HIDE);
    }
    /* - PER-PLANE BACKING BEFORE THE UI THREAD EXISTS. The remap unmaps the A0000
     * window for an instant, and the renderer dereferences it every few milliseconds;
     * doing this with that thread already running hung the host so early that no log
     * reached disk at all. Its report is buffered and flushed after the preamble.
     */
    if (GetFileAttributesA(NOREMAP_FLAG) == INVALID_FILE_ATTRIBUTES && ModeYRemapInitialize())
    {
        g_Video.YMapContext    = NULL;
        g_Video.YMapSelect = ModeYRemapSelect;
        g_Video.YMapPlane  = ModeYRemapPlane;
        g_Video.YMapWriteMode  = ModeYRemapWriteMode;
        g_Video.YMapReadMap = ModeYRemapReadMap;
    }

    uiThread = CreateThread(NULL, 0, UiThread, NULL, 0, NULL);
    /* Headless: arm the deadline watchdog so a run that blocks on input (a "press any
     * key" prompt, a game menu) still self-terminates instead of wedging the harness.
     */
    if (g_Headless) { HANDLE deadlineThread = CreateThread(NULL, 0, HeadlessDeadlineThread, NULL, 0, NULL);

                      if (deadlineThread)
                          CloseHandle(deadlineThread);

                      /* appended to the preamble, which is flushed further down */
                      cursor = LogPut(cursor, "HEADLESS: cap=0x"); cursor = LogHex(cursor, g_HeadlessMs);
                      cursor = LogPut(cursor, " ms\r\n"); }

    /* cfg\livehb.flag: the heartbeat on a LIVE (by-hand) run too. A host that dies
     * with no exit report leaves nothing else that says where the guest was. (s68)
     */
    if (g_Headless || GetFileAttributesA(LIVEHB_FLAG) != INVALID_FILE_ATTRIBUTES)
    {
                      HANDLE heartbeatThread = CreateThread(NULL, 0, HeartbeatThread, NULL, 0, NULL);

                      if (heartbeatThread)
                          CloseHandle(heartbeatThread); }

    if (g_QiKeys) { HANDLE keyThread = CreateThread(NULL, 0, SynthKeyThread, NULL, 0, NULL);

                     if (keyThread)
                         CloseHandle(keyThread); }

    if (g_QiRaise) { HANDLE irqThread = CreateThread(NULL, 0, QueueIrqProbeThread, NULL, 0, NULL);

                      if (irqThread)
                          CloseHandle(irqThread); }

    /* GH #128: on a WOW launch, the guest is krnl386, not a DOS program:
     * Placed HERE because this is the one point where the DOS machine is fully
     * built (conventional memory, INT 21h, the IVT, INT 2Fh) and the guest entry
     * has not yet been committed. krnl386 needs all of it: AH=52h almost at
     * once, then 2F/1687 to find our DPMI host and switch itself.
     * The DOS program load above still ran and is simply discarded -- it is
     * tolerant of a missing target and costs one wasted image. Overriding here
     * rather than short-circuiting there keeps the DOS path's spine untouched.
     */
    {   WORD wowCs = 0, wowIp = 0, wowDs = 0, wowSs = 0, wowSp = 0;

        if (g_WowModuleCount && WowPlaceV86(machine, &wowCs, &wowIp, &wowDs, &wowSs, &wowSp) == 0)
        {
            image->CodeSegment = wowCs;
            image->InstructionPointer = wowIp;
            image->StackSegment = wowSs;
            image->StackPointer = wowSp;
            g_WowEntryDs = wowDs;
            g_WowEntering = 1;
        }
    }
    VdmSetEntry(tib, image->CodeSegment, image->InstructionPointer, image->StackSegment, image->StackPointer, DOS_PSP_SEG);
    cursor = StartupPrepareWowEntry(cursor, tib, image);
    /* ENTRY TRAMPOLINE: `STI` then a far jump to the program's real entry point.
     * Under VME the CPU sets EFLAGS.VIF only when the guest EXECUTES sti -- and the
     * kernel's whole notion of "this guest can take an interrupt" is VIF. Session 10
     * correctly observed that DOS programs never issue sti (they are entered with
     * interrupts already on) and fixed it by handing them IF=1 in the CONTEXT, but that
     * sets the REAL IF, which the kernel does not consult, while VIF stays 0 forever.
     * Setting VIF directly in the CONTEXT does not survive either -- the kernel sanitises
     * it. Making the guest execute one real sti costs 6 bytes and gets VIF set the only
     * way the CPU will accept, after which the hardware maintains it across the guest's
     * own cli/sti/iret. Faithful, too: DOS's EXEC really does return into the program
     * with interrupts enabled.
     * RESULT: this does NOT unblock delivery -- and note qirq.com already executed its own
     * sti before spinning, so the "guest never sets VIF" hypothesis was in truth already
     * refuted by the earlier runs. Kept as an opt-in knob (it is the faithful entry
     * sequence regardless) but it is not the missing piece.
     *
     * [CAUTION]: s82: THE ORACLES DISAGREE WITH US, AND THIS DOES NOT FIX IT. p_ifst.com asks FLAGS
     * at a program's first instruction: MS-DOS 6.22, DOSBox-X and PCem all answer IF=1; we
     * answer IF=0, and keep answering it after INT 10h and INT 21h. A program that never
     * executes STI (mybench.com) then has every timer tick refused by our own gate, which
     * reads that virtual IF, and 0040:006C stands still. Defaulting this trampoline was
     * tried: the guest's STI did not stick (first exit still VTIB EFLAGS=0x30002), with or
     * without VIP cleared first -- the wall s11 recorded. Still opt-in. See GH issue.
     * OK #212, s84: FIXED ELSEWHERE, AND THIS WAS NEVER THE PLACE. Every program is started
     * by a shell's EXEC, and the EXEC path handed the child the parent's flags from
     * INSIDE our INT 21h stub, where IF is already clear -- so the trampoline's STI ran
     * for the shell, not the program. ExecBegin() now enters the child with IF set;
     * p_ifst agrees with all three oracles.
     */
    if (g_QiVif)
    {
        volatile BYTE *trampoline = (volatile BYTE *)(ULONG_PTR)(((DWORD)DOS_HDLR_SEG << PARAGRAPH_SHIFT) + DOS_HDLR_TRAMPOLINE_OFF);
        /* [CAUTION]: s82: 0x60-0x65 is DPMI callback slot 0 and half of slot 1, planted ABOVE
         * (dos_layout.h). Keep what was there; the exec loop puts it back at the first
         * exit outside the trampoline. Opt-in, this never mattered; default, it would
         * send a client's first callback into `sti; jmp far <program entry>`.
         * (#248: the callback slots moved to 0x90, so these bytes are now unused; the
         * save/restore stays because it costs nothing and keeps the area as found.)
         */
        {
            INT item;

            for (item = 0; item < DOS_HDLR_TRAMPOLINE_SIZE; ++item)
                g_TrampolineSave[item] = trampoline[item];

            g_TrampolineSaved = 1;
        }
        trampoline[0] = X86_OP_STI;                                   /* sti */
        trampoline[1] = X86_OP_JMP_FAR;                                   /* jmp far cs:ip */
        trampoline[2] = (BYTE)image->InstructionPointer;
        trampoline[3] = (BYTE)(image->InstructionPointer >> BYTE_SHIFT);
        trampoline[4] = (BYTE)image->CodeSegment;
        trampoline[5] = (BYTE)(image->CodeSegment >> BYTE_SHIFT);
        VDM_REG(tib, VTIB_CS)  = DOS_HDLR_SEG;
        VDM_REG(tib, VTIB_EIP) = DOS_HDLR_TRAMPOLINE_OFF;
    }

    /* Session 11: the kernel's deliverability test for a V86 frame on a VME CPU follows
     * EFLAGS.VIF, not IF (observed: VIP set and delivery deferred). Starting the guest with
     * VIF clear makes every hardware interrupt undeliverable from the kernel's point of
     * view -- it just sets VIP and defers. Opt-in until the rig confirms it.
     */
    if (g_QiVif)
        VDM_REG(tib, VTIB_EFLAGS) |= EFLAGS_VIF;

    if (!image->IsExe)                                        /* .COM near-ret guard */
        *(volatile WORD *)(((DWORD)DOS_PSP_SEG << PARAGRAPH_SHIFT) + DOS_COM_STACK_TOP) = 0;

    /* Every stub the host plants in DOS_HDLR_SEG is planted by a different piece of
     * start-up code with its own idea of a free offset. Verify the ones a guest can
     * RETURN INTO after all planting is done -- an overwritten stub is a guest jumping
     * into another service's BOP, and the symptom (QBasic: "DOS terminate" on the first
     * mouse move) names nothing.
     */
    {   volatile BYTE *handlerSegment = (volatile BYTE *)(DOS_HDLR_SEG << PARAGRAPH_SHIFT);

        if (handlerSegment[MS_CB_RET_OFF] != VDM_BOP0 || handlerSegment[MS_CB_RET_OFF + 1] != VDM_BOP1
            || handlerSegment[MS_CB_RET_OFF + VDM_BOP_NUMBER_OFFSET] != MS_CB_BOP)
        {
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

        if (g_Instance > 1)
        {
            instanceCursor = LogPut(instanceCursor, " (another NTVDMEX was running) -> output in ");
            instanceCursor = LogPut(instanceCursor, g_OutSubdirectory);
        }

        if (g_InstanceAbandoned)
            instanceCursor = LogPut(instanceCursor, " -- prior holder ABANDONED (zombie thread died)");

        instanceCursor = LogPut(instanceCursor, " (#211)\r\n");
        LogAppend(LOG_PATH, instanceLine, instanceCursor); }
    /* #144. HERE, not with the other STAGE0 lines: this is the last truncating write
     * (an append before it is wiped -- the first cut of this was), and the ~7 KB table
     * would overflow the 8 KB preamble buffer. Every file override has been read.
     */
    SettingsLogSources();
    /* THIRD-PARTY VDDs LOAD HERE, AND THE PLACE IS THE POINT:
     * Two constraints, and only this line satisfies both:
     * (1) AFTER every built-in device is on the bus, so a third-party claim
     *   that collides with our own video or UART is REFUSED honestly rather
     *   than silently shadowing it;
     * (2) AFTER THE LAST LogWrite, which TRUNCATES. The first cut put this
     *   beside the built-in devices, ~350 lines up -- the driver may well
     *   have loaded and every line it logged was erased before anyone could
     *   read it, which reads exactly like "the code never ran". The warning
     *   immediately above this one says so in as many words, and I still
     *   walked into it. Third time this trap has been paid for.
     * The guest's image is loaded but not yet running, so claims made here are
     * in place before its first instruction.
     */
    VddLoadThirdParty();
    StartupReportVectorWiring();
    /* [CAUTION]: AFTER THE **LAST** LogWrite. There are THREE of them in WinMain and every one
     * TRUNCATES. This probe was placed after the first, then after the second, and
     * both times its output was silently erased by the next one -- which reads
     * exactly like "the code never ran", and cost three rounds of looking in the
     * wrong place. If you add diagnostics to WinMain, append AFTER line ~9941 or put
     * them in `p` so a LogWrite carries them.
     */
    {   DWORD attributes = GetFileAttributesA(WOWTRY_FLAG);
        CHAR wowLine[200];
        CHAR *wowCursor = wowLine;
        wowCursor = LogPut(wowCursor, "LDTARM: wow_mods="); wowCursor = LogHex(wowCursor, (DWORD)g_WowModuleCount);
        wowCursor = LogPut(wowCursor, " flag_attr=0x");      wowCursor = LogHex(wowCursor, attributes);
        wowCursor = LogPut(wowCursor, "\r\n"); LogAppend(LOG_PATH, wowLine, wowCursor);

        if (!g_WowModuleCount && attributes != INVALID_FILE_ATTRIBUTES)
            WowProbeLdtMatrix("dos-late");  /* the other corner of the 2x2 */
    }
    ModeYRemapFlushReport();     /* whatever the A0000 remap had to say, now it fits */
    /* CAN THE A0000 WINDOW BE REMAPPED? THE ONE FACT THE REAL VIDEO FIX NEEDS:
     * Mode Y cannot be de-interleaved from a flat aperture: A0000 is one buffer, so
     * a guest write lands there with no record of which plane the map mask selected,
     * and six after-the-fact rules have now been measured against captured frames
     * without finding a good one (see modey_flush()). The fix is to stop guessing --
     * give each plane its own backing and point A0000 at the selected one on a mask
     * change, with four pagefile-backed sections and MapViewOfFileEx at a fixed
     * address: O(1) per change, exact, no copying.
     * Whether that is possible at all turns on ONE thing: is A0000 its own
     * allocation, or a slice of a larger reservation the VDM kernel made? A section
     * cannot be mapped into the middle of an existing reservation, and MEM_RELEASE
     * only takes a whole allocation. VirtualQuery answers it for the cost of one log
     * line, and it is worth far more than another guess at a heuristic.
     */
    { MEMORY_BASIC_INFORMATION regionInfo;

      if (VirtualQuery((LPCVOID)(ULONG_PTR)VIDEO_APERTURE_BASE, &regionInfo, sizeof regionInfo) == sizeof regionInfo)
      {
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
      busCursor = LogPut(busCursor, "\r\n");
      LogAppend(LOG_PATH, busLine, busCursor);
      SerialOut(busLine, busCursor); }

    SetCurrentDirectoryA(g_CurrentDirectory);    /* DOS relative paths resolve against CurDir */
    *cursorIo = cursor;
    *baseIo = base;
    *uiThreadIo = uiThread;
}

/* Connect DOS to the host: console output to the video VDD, console input from the keyboard, the tick and printer hooks, and the DOS-trace and simulated-interrupt switches. */
static VOID StartupConnectDosToHost(DOS_MACHINE *machine)
{
    machine->ConsoleOut = HostConsoleOut;
    machine->ConsoleOutContext = NULL;    /* DOS console out -> video */
    machine->ConsoleIn  = HostConsoleIn;
    machine->ConsoleInContext = NULL;    /* DOS console in  <- keyboard */
    /* Full INT 21h call trace, opt-in per run: it is a differential instrument, not a
     * default. See the trace at the top of DosInt21().
     */
    machine->IsTraceAll = (GetFileAttributesA(DOSTRACE_FLAG) != INVALID_FILE_ATTRIBUTES);
    /* DPMI 0300 reflects to the guest's own real-mode handler -- ON by default since s81,
     * when the wedge that kept it off was found and fixed (see g_NestedRm and the BIOS
     * tick in AsyncInjectIrq). simintrefl_off.flag turns it off for diagnosis; say so,
     * because a guest-owned vector then silently does nothing again.
     */
    g_SimIntReflect = (GetFileAttributesA(SIMINTREFL_OFF_FLAG) == INVALID_FILE_ATTRIBUTES);

    if (!g_SimIntReflect)
    {
        CHAR stageLine[224];
        CHAR *stageCursor = stageLine;
        stageCursor = LogPut(stageCursor, "STAGE1: simintrefl_off.flag -- DPMI 0300 is the pre-#247 shape: INT 21h/"
                      "33h/10h host-side, EVERY other vector NOT RUN (ZAR: silent). See "
                      "simint_route().\r\n");
        LogAppend(LOG_PATH, stageLine, stageCursor); SerialOut(stageLine, stageCursor);
    }

    machine->ConsoleInNoWait = HostConsoleInNoBlock;                   /* AH=06 DL=FF non-blocking read */
    machine->ConsolePeek = HostConsolePeek;                   /* AH=0B/06 non-blocking status */
    machine->SetTicks = HostSetTicks;               /* AH=2Dh reloads 0040:006C (#250) */
    machine->TicksContext = NULL;
    machine->PrinterOut  = DosPrnOut;                     /* #251: PRN/AUX when not in V86 */
    machine->AuxOut  = DosAuxOut;
    machine->DeviceContext = NULL;
}

/* Start the host's services: the PIT pacer (or the tick it replaces), the capture watchdog, the CPU-speed governor, the audio output and its MIDI route, and wavrec.flag's recording. */
static PSTR StartupStartServices(PSTR cursor)
{
    if (g_PitPaceOn)
    {
        HMODULE winmmModule = LoadLibraryA(HOST_MODULE_WINMM);

        if (winmmModule) { PFN_TIME_BEGIN_PERIOD beginPeriod =
                      (PFN_TIME_BEGIN_PERIOD)GetProcAddress(winmmModule, HOST_EXPORT_TIME_BEGIN_PERIOD);

                  if (beginPeriod)
                      beginPeriod(1); }

        PitPacerTimerStart(winmmModule);                /* #238: a true 1 ms wake */
        g_PitPaceThread = CreateThread(NULL, 0, PitPacerThread, NULL, 0, NULL);
    }

    /* The capture watchdog runs for every guest, throttled or not, headless or not:
     * it is the only thing standing between a wedge and a hard reset.
     */
    { HANDLE captureWatchdog = CreateThread(NULL, 0, CaptureWatchdogThread, NULL, 0, NULL);

      if (captureWatchdog)
          CloseHandle(captureWatchdog); }

    /* -- THE TICK COURIER. Auto-reset: one signal wakes exactly one pass, and a
     * signal arriving while it is already awake is not lost -- the pass re-checks
     * g_Irq0Pending anyway. Created even when the courier is knobbed off, so the
     * raise site's SetEvent never has to test two things.
     */
    if (g_QiSuspended)
    {
        g_CourierEvent = CreateEventA(NULL, FALSE, FALSE, NULL);

        if (g_CourierEvent)
            g_CourierThread = CreateThread(NULL, 0, TickCourierThread, NULL, 0, NULL);
    }

    /* THE CPU-SPEED THROTTLE. (GH #56) (Importance = 1):
     * Started unconditionally, even at Unlimited: the setting is live, and a
     * thread that has to be created before it can bite would make the menu work
     * only on machines that started throttled. It idles in 4 ms sleeps when
     * there is nothing to do.
     *
     * [CAUTION]: IT NEEDS THE 1 ms TIMER RESOLUTION, and the block above only raises it when
     * the PIT pacer is on. Without timeBeginPeriod(1) a Sleep(1) is XP's default
     * ~15.6 ms, which would turn every held millisecond into fifteen and make the
     * guest fifteen times slower than the label on the menu. So raise it here too
     * -- the call nests, and pitpace=0 must not silently change what "33 MHz"
     * means. Bound by name, like every other winmm use, so the import allowlist
     * is unaffected.
     */
    if (!g_PitPaceOn)
    {
        HMODULE winmmModule2 = LoadLibraryA(HOST_MODULE_WINMM);

        if (winmmModule2) { PFN_TIME_BEGIN_PERIOD beginPeriod2 =
                       (PFN_TIME_BEGIN_PERIOD)GetProcAddress(winmmModule2, HOST_EXPORT_TIME_BEGIN_PERIOD);

                   if (beginPeriod2)
                       beginPeriod2(1); }
    }

    CpuSpeedRecompute();
    g_CpuSpeedRelease = CreateEventA(NULL, FALSE, FALSE, NULL);   /* #225, auto-reset */
    g_CpuSpeedThread = CreateThread(NULL, 0, CpuSpeedThread, NULL, 0, NULL);
    /* [CAUTION]: THE SAME RATE THE MIXER WAS BUILT AT. Opening the device at one rate and
     * mixing at another silently resamples everything to a clock nothing runs
     * on -- audible as a pitch error, not as an error message.
     */
    g_Wave.WantsDirectSound = (g_Settings.Values[SET_AUDIOAPI] == 1);          /* #234 */
    g_Wave.IsForcedSilent = g_Safe.AudioOut;                 /* s90 #132: SAFE MODE */
    g_Wave.MidiChoice = (INT)(g_Settings.Values[SET_MIDI] < MIDI_ROUTE_COUNT ? g_Settings.Values[SET_MIDI] : 0);  /* #136 */
    AudioWaveStart(&g_Wave, SettingsOutputHz(&g_Settings), HostAudioFill, NULL);
    /* -- #136: SAY WHICH SYNTH THE MPU-401 PLAYS THROUGH, but only when it was chosen.
     * Host GM (the default) opens device 0 as it always did and logs nothing new.
     */
    if (g_Wave.MidiChoice != MIDI_ROUTE_GM)
    {
        CHAR midiLine[200];
        CHAR *midiCursor = LogPut(midiLine, "STAGE2: MIDI = ");
        midiCursor = LogPut(midiCursor, g_Wave.MidiChoice == MIDI_ROUTE_MT32 ? "MT-32" : "SoundFont");

        if (g_Wave.IsMidiExternal)
        {
            midiCursor = LogPut(midiCursor, " -> device "); midiCursor = LogDecimal(midiCursor, (UINT)g_Wave.MidiDevice);
            midiCursor = LogPut(midiCursor, " \""); midiCursor = LogPut(midiCursor, g_Wave.MidiName); midiCursor = LogPut(midiCursor, "\", SysEx passed through");
            g_Mpu.SysExSink = HostMidiSysEx;     /* the MPU is on the bus already; no */
            g_GusMidi.SysExSink = HostMidiSysEx; /* guest code has run yet */
        }
        else
        {
            midiCursor = LogPut(midiCursor, " asked for, NO such device among "); midiCursor = LogDecimal(midiCursor, g_Wave.MidiDeviceCount);
            midiCursor = LogPut(midiCursor, " -> Host GM (device 0");

            if (g_Wave.MidiName[0])
            {
                midiCursor = LogPut(midiCursor, " \"");
                midiCursor = LogPut(midiCursor, g_Wave.MidiName);
                midiCursor = LogPut(midiCursor, "\"");
            }

            midiCursor = LogPut(midiCursor, g_Wave.MidiDevice < 0 ? ", would not open)" : ")");
        }

        if (g_Wave.MidiChoice == MIDI_ROUTE_SF2)
            midiCursor = LogPut(midiCursor, "; SoundFontPath is not passed on -- the driver keeps its own list");

        midiCursor = LogPut(midiCursor, " (#136)\r\n"); LogAppend(LOG_PATH, midiLine, midiCursor);
    }

    {   CHAR audioLine[128], *audioCursor = LogPut(audioLine, "STAGE2: audio output = ");
        audioCursor = LogPut(audioCursor, g_Wave.IsUsingDirectSound ? "DirectSound" : g_Wave.IsSilent ? "none (silent pump)" : "WinMM");

        if (g_Wave.WantsDirectSound && !g_Wave.IsUsingDirectSound)
            audioCursor = LogPut(audioCursor, " (DirectSound asked for, would not open)");

        audioCursor = LogPut(audioCursor, "\r\n");
        LogAppend(LOG_PATH, audioLine, audioCursor); }
    /* cfg\wavrec.flag: record the whole run's audio -- see HostRecordFinish. */
    if (GetFileAttributesA(WAVREC_FLAG) != INVALID_FILE_ATTRIBUTES)
    {
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
    /* THE AUDIO LEAD, AS A CONTROLLED VARIABLE (awbufs.txt):
     * Each queued waveOut buffer is ~11.6 ms that our DMA read pointer runs ahead of
     * what is audible, and the guest must refill a block before we reach it. Doom's
     * longest PM stretch with no host turn measured 62.8 ms against a 70 ms lead, so
     * the lead is a suspect for the residual ECHO -- the capture is 46% identical to
     * one ring lap (185.8 ms) earlier, against ~22% at every neighbouring lag.
     * Setting this and reading back `sb replay:` in STAGE2 is the experiment: if
     * replays move with the lead it is the race, if they do not, DMX is failing to
     * refill for another reason and the lead is the wrong suspect. Absent = 6.
     */
    { HANDLE handle = CreateFileA(AWBUFS_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             NULL, OPEN_EXISTING, 0, NULL);

      if (handle != INVALID_HANDLE_VALUE)
      {
          CHAR text[16];
          DWORD bytesRead = 0;
          DWORD number = 0;
          INT index;
          ReadFile(handle, text, sizeof text, &bytesRead, NULL);
          CloseHandle(handle);

          for (index = 0; index < (INT)bytesRead; ++index)
          {
              if (text[index] < '0' || text[index] > '9')
                  break;

              number = number * DECIMAL_RADIX + (DWORD)(text[index] - '0');
          }

          g_Wave.BufferCount = number;                   /* AudioWaveStart clamps to [2,AUDIO_WAVE_BUFFERS] */
      } }

    /* AND THE GRANULARITY, AS A SEPARATE CONTROLLED VARIABLE (awframes.txt):
     * nframes x nbufs is the LEAD; nframes alone is the STEP the guest's DMA read
     * pointer moves in. They are different suspects and must be varied independently
     * or a result cannot be attributed to either. `awbufs=2` already showed why this
     * matters: it cut the lead, starved the transport, and the replay rate "improved"
     * only because the non-flat block count collapsed 13x.
     * To hold the lead constant while quartering the step: awframes=128, awbufs=24.
     */
    { HANDLE handle = CreateFileA(AWFRAMES_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             NULL, OPEN_EXISTING, 0, NULL);

      if (handle != INVALID_HANDLE_VALUE)
      {
          CHAR text[16];
          DWORD bytesRead = 0;
          DWORD number = 0;
          INT index;
          ReadFile(handle, text, sizeof text, &bytesRead, NULL);
          CloseHandle(handle);

          for (index = 0; index < (INT)bytesRead; ++index)
          {
              if (text[index] < '0' || text[index] > '9')
                  break;

              number = number * DECIMAL_RADIX + (DWORD)(text[index] - '0');
          }

          g_Wave.FrameCount = number;                 /* clamped to [AUDIO_WAVE_MIN_FRAMES,AUDIO_WAVE_FRAMES] */
      } }

    { HANDLE pitPaceFile = CreateFileA(PITPACE_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, 0, NULL);

      if (pitPaceFile != INVALID_HANDLE_VALUE)
      {
          CHAR text[8];
          DWORD bytesRead = 0;
          ReadFile(pitPaceFile, text, sizeof text, &bytesRead, NULL);
          CloseHandle(pitPaceFile);

          if (bytesRead && text[0] >= '0' && text[0] <= '9')
              g_PitPaceMs = text[0] - '0';

          g_PitPaceOn = (g_PitPaceMs != 0);
          SettingsNoteOverride(SET_PITPACE, CFG_TEXT(KNOB_FILE_PITPACE) SETTINGS_SOURCE_MS, (DWORD)g_PitPaceMs);
      } }

    /* The pacer's two OTHER levers -- see PitPacerThread. Absent file = as shipped. */
    { HANDLE pitPriorityFile = CreateFileA(PITPRIO_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, 0, NULL);

      if (pitPriorityFile != INVALID_HANDLE_VALUE)
      {
          CHAR text[8];
          DWORD bytesRead = 0;
          ReadFile(pitPriorityFile, text, sizeof text, &bytesRead, NULL);
          CloseHandle(pitPriorityFile);

          if (bytesRead && text[0] >= '0' && text[0] <= '4')
          {
              static const INT priorities[5] = { THREAD_PRIORITY_IDLE, THREAD_PRIORITY_BELOW_NORMAL,
                                          THREAD_PRIORITY_NORMAL, THREAD_PRIORITY_ABOVE_NORMAL,
                                          THREAD_PRIORITY_HIGHEST };
              g_PitPacePriority = priorities[text[0] - '0'];
          }
      } }

    { HANDLE pitInjectFile = CreateFileA(PITINJ_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, 0, NULL);

      if (pitInjectFile != INVALID_HANDLE_VALUE)
      {
          CHAR text[8];
          DWORD bytesRead = 0;
          ReadFile(pitInjectFile, text, sizeof text, &bytesRead, NULL);
          CloseHandle(pitInjectFile);

          if (bytesRead && (text[0] == '0' || text[0] == '1'))
              g_PitPaceInject = text[0] - '0';
      } }

    /* llkbd.txt = 1 re-enables the system-wide keyboard hook. See InputCaptureSet. */
    { HANDLE keyHandle = CreateFileA(LLKBD_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL, OPEN_EXISTING, 0, NULL);

      if (keyHandle != INVALID_HANDLE_VALUE)
      {
          CHAR text[8];
          DWORD bytesRead = 0;
          ReadFile(keyHandle, text, sizeof text, &bytesRead, NULL);
          CloseHandle(keyHandle);

          if (bytesRead && (text[0] == '0' || text[0] == '1'))
              g_LowLevelKeyboardOn = text[0] - '0';
      } }

    /* courier.txt -- the tick courier (see TickCourierThread). 0 = as shipped. */
    { HANDLE configHandle = CreateFileA(COURIER_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL, OPEN_EXISTING, 0, NULL);

      if (configHandle != INVALID_HANDLE_VALUE)
      {
          CHAR text[8];
          DWORD bytesRead = 0;
          ReadFile(configHandle, text, sizeof text, &bytesRead, NULL);
          CloseHandle(configHandle);
          /* 0 = off, 1 = REFUTED (progressive collapse), 2 = gentle. See the thread. */
          if (bytesRead && text[0] >= '0' && text[0] <= '2')
              g_CourierOn = text[0] - '0';
      } }

    { HANDLE uiTickFile = CreateFileA(UITICK_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, 0, NULL);

      if (uiTickFile != INVALID_HANDLE_VALUE)
      {
          CHAR text[8];
          DWORD bytesRead = 0;
          INT number = 0;
          INT index;
          ReadFile(uiTickFile, text, sizeof text, &bytesRead, NULL);
          CloseHandle(uiTickFile);

          for (index = 0; index < (INT)bytesRead; ++index)
          {
              if (text[index] < '0' || text[index] > '9')
                  break;

              number = number * DECIMAL_RADIX + (text[index] - '0');
          }

          if (bytesRead && text[0] >= '0' && text[0] <= '9' && number <= UI_TICK_MS_MAX)
          {
              g_UiTickMinimumMs = number;
              SettingsNoteOverride(SET_UITICK, CFG_TEXT(KNOB_FILE_UITICK) SETTINGS_SOURCE_MS, (DWORD)number);
          }
      } }

    /* wowidle.txt -- how long a Win16 task blocked in GetMessage waits. 0 = forever. */
    { HANDLE wowIdleFile = CreateFileA(WOWIDLE_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, 0, NULL);

      if (wowIdleFile != INVALID_HANDLE_VALUE)
      {
          CHAR text[12];
          DWORD bytesRead = 0;
          DWORD number = 0;
          INT index;
          ReadFile(wowIdleFile, text, sizeof text, &bytesRead, NULL);
          CloseHandle(wowIdleFile);

          for (index = 0; index < (INT)bytesRead; ++index)
          {
              if (text[index] < '0' || text[index] > '9')
                  break;

              number = number * DECIMAL_RADIX + (DWORD)(text[index] - '0');
          }

          if (bytesRead && text[0] >= '0' && text[0] <= '9')
          {
              CHAR wowLine2[160];
              CHAR *wowCursor2 = wowLine2;
              g_WowMsgWaitMs = number;
              wowCursor2 = LogPut(wowCursor2, "WOWMSG: GetMessage idle wait = ");

              if (number)
              {
                  wowCursor2 = LogHex(wowCursor2, number);
                  wowCursor2 = LogPut(wowCursor2, " ms");
              }
              else     wowCursor2 = LogPut(wowCursor2, "FOREVER (interactive: the guest is waiting "
                                     "for the user, not stuck)");

              wowCursor2 = LogPut(wowCursor2, "\r\n"); LogAppend(LOG_PATH, wowLine2, wowCursor2); SerialOut(wowLine2, wowCursor2);
          }
      } }

    /* keyirq.txt -- the knob 5b6a4a6's message promises. It was lost in that session's
     * revert, so the escape hatch documented at the retry site did not actually exist.
     */
    { HANDLE keyIrqFile = CreateFileA(KEYIRQ_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, 0, NULL);

      if (keyIrqFile != INVALID_HANDLE_VALUE)
      {
          CHAR text[8];
          DWORD bytesRead = 0;
          ReadFile(keyIrqFile, text, sizeof text, &bytesRead, NULL);
          CloseHandle(keyIrqFile);
          /* 0 = never yield, 1 = always (old default), 2 = only while the clock is on
           * schedule. See the yield branch in HostIrqSink.
           */
          if (bytesRead && text[0] >= '0' && text[0] <= '3')
              g_KeyIrqRetry = text[0] - '0';
      } }

    /* Mouse feel is per-guest and per-hand, and every test of it costs a play session,
     * so it is a knob from the start: percent, 100 = the device's own counts.
     */
    { HANDLE mouseSensitivityFile = CreateFileA(MSENS_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, 0, NULL);

      if (mouseSensitivityFile != INVALID_HANDLE_VALUE)
      {
          CHAR text[8];
          DWORD bytesRead = 0;
          INT value2 = 0;
          INT index2;
          ReadFile(mouseSensitivityFile, text, sizeof text, &bytesRead, NULL);
          CloseHandle(mouseSensitivityFile);

          for (index2 = 0; index2 < (INT)bytesRead; ++index2) { if (text[index2] < '0' || text[index2] > '9')
              break;
                                          value2 = value2 * DECIMAL_RADIX + (text[index2] - '0'); }

          if (value2 >= MOUSE_SENSITIVITY_MIN && value2 <= MOUSE_SENSITIVITY_MAX)
          {
              g_MouseSensitivity = value2;
              SettingsNoteOverride(SET_MSENS, CFG_TEXT(KNOB_FILE_MSENS), (DWORD)value2);
          }
      } }

    /* -- GH #56: the calibration and a one-run speed override, both from the share.
     *
     * [CAUTION]: THESE RUN BEFORE CpuSpeedRecompute() BELOW, which is the whole point: the
     * duty is computed once from whatever the registry and these two agree on,
     * and re-computed only when the setting changes. Reading them after would
     * leave the thread running on the registry's answer for the whole session,
     * which is exactly the shape of a knob that silently does nothing.
     */
    { HANDLE cpuReferenceFile = CreateFileA(CPUREF_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, 0, NULL);

      if (cpuReferenceFile != INVALID_HANDLE_VALUE)
      {
          CHAR text[12];
          DWORD bytesRead = 0;
          UINT sbValue = 0;
          INT index2;
          ReadFile(cpuReferenceFile, text, sizeof text, &bytesRead, NULL);
          CloseHandle(cpuReferenceFile);

          for (index2 = 0; index2 < (INT)bytesRead; ++index2) { if (text[index2] < '0' || text[index2] > '9')
              break;
                                          sbValue = sbValue * DECIMAL_RADIX_U + (UINT)(text[index2] - '0'); }

          if (sbValue >= CPU_REFERENCE_MHZ_MIN_U && sbValue <= CPU_REFERENCE_MHZ_MAX_U)
              g_CpuSpeedReferenceMhz = sbValue;
      } }

    StartupLoadCpuSpeedKnob();
    /* -- THE GRANULARITY SLIDER AS A FILE KNOB. cpugran.txt = target period in ms,
     * 0 or absent = AUTO (measure the suspend round trip and pick the finest
     * period this box can sustain). See the long note in cpuspeed.h -- this is
     * the lever that decides whether a slow setting is playable or a slideshow,
     * and it is a file knob first so the sweep can find the right default without
     * a rebuild per value.
     */
    { HANDLE cpuGranularityFile = CreateFileA(CPUGRAN_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, 0, NULL);

      if (cpuGranularityFile != INVALID_HANDLE_VALUE)
      {
          CHAR text[12];
          DWORD bytesRead = 0;
          UINT valueG = 0;
          INT indexG;
          ReadFile(cpuGranularityFile, text, sizeof text, &bytesRead, NULL);
          CloseHandle(cpuGranularityFile);

          for (indexG = 0; indexG < (INT)bytesRead; ++indexG) { if (text[indexG] < '0' || text[indexG] > '9')
              break;
                                             valueG = valueG * DECIMAL_RADIX_U + (UINT)(text[indexG] - '0'); }

          if (indexG > 0 && valueG <= CPUSPEED_GRAN_MAX_MS)
              g_CpuSpeedGranularityMs = valueG;
      } }

    /* cpuaff.txt = 1 -> give the guest a core of its own. See CpuAffinityApply. */
    { HANDLE cpuAffinityFile = CreateFileA(CPUAFF_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, 0, NULL);

      if (cpuAffinityFile != INVALID_HANDLE_VALUE)
      {
          CHAR text[8];
          DWORD bytesRead = 0;
          ReadFile(cpuAffinityFile, text, sizeof text, &bytesRead, NULL);
          CloseHandle(cpuAffinityFile);

          if (bytesRead && (text[0] == '0' || text[0] == '1'))
              g_CpuAffinityOn = (text[0] == '1');
      } }
}

/* Put the machine on the bus: the host lock; the PIC and the PIT with its clock hooks; CMOS, FDC, IDE and video; the system font; the keyboard; the serial ports, declared in the BIOS data area, which is then initialised; NetBIOS, the speaker, the gameport, DMA, OPL, Sound Blaster, MPU-401, GUS and AWE32; the WOW callbacks; then the mixer, which also drives the SB's DMA. */
static PSTR StartupAttachDevices(PSTR cursor)
{
    /* Stand up the device bus (NULL base => absolute V86 addresses) with the PIT
     * (ports 0x40-0x43, INT 08h/1Ah) and the video VDD (B8000 + INT 10h text +
     * cell renderer). The present sink is DirectDraw via present_ddraw on the UI
     * thread. I/O on claimed ports reflects as event 0 -> the bus; INT 10h comes
     * in as a BOP routed below; DOS console output is routed via m.conout.
     */
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
    /* 0040:006C IS TICKS SINCE MIDNIGHT, SO SET IT TO THAT. (GH #253) (Importance = 1):
     * POST does this from the RTC; nothing here did, so every launch began at
     * 00:00:00 by the BIOS's clock while INT 1Ah AH=02h read the real time. Seeded
     * from the SAME hook AH=02h answers from (HostRtcNow), after the PIT is on
     * the bus and before anything can take IRQ0. See VddPitSeedTimeOfDay.
     */
    VddPitSeedTimeOfDay(&g_Pit);
    /* THE RTC/CMOS TAKES THE SAME CLOCK INT 1Ah DOES:
     * Registers 00h-09h and INT 1Ah AH=02h/04h are two doors onto ONE clock,
     * and a guest may use either -- so they are given the same hook and their
     * agreement is structural rather than something to keep in step by hand.
     * The same principle as A20's three doors; p_rtc.asm's rtc.agree.hours is
     * the case that checks it, and it read 0000 against 0101 on all three
     * oracles before this device existed.
     */
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
    VddBusAdd(&g_Bus, &g_CmosDevice);           /* MC146818: ports 0x70/0x71 */
    /* THE FLOPPY CONTROLLER, WHOSE ABSENCE WAS A HANG:
     * 3F0h-3F7h were claimed by nothing, so the Main Status Register read FFh
     * -- RQM=1 with DIO=1 -- and the datasheet's own command-write loop
     * (`and al,0C0h / cmp al,80h / jne`) never matched and never exited.
     * MEASURED on the rig before this existed: fdc.cmdwait = 01C0 here against
     * 0080 on 6.22/QEMU and on PCem's real AMI BIOS alike. Same shape as the
     * MC146818's UIP bit two devices above. IRQ6 stays dormant unless a guest
     * both gates it through DOR bit 3 and unmasks it at the PIC, which starts
     * at 0xFC. See src/vdd/vdd_fdc.h.
     */
    g_FdcDevice = VddFdcDevice(&g_Fdc);
    VddBusAdd(&g_Bus, &g_FdcDevice);            /* 82077AA: 3F2h-3F5h, 3F7h */
    /* THE IDE ADAPTER, FITTED, BOTH CHANNELS EMPTY. (GH #179):
     * Same shape as the FDC above, one surface later: nothing claimed 1F0h-1F7h
     * or 3F6h, FFh there is BSY=1, and the ATA's own "wait until BSY clears"
     * never exited. An adapter with no drives reads 00h everywhere (DD7 pulled
     * down by the ATA document; DD6:0 a recorded choice) and latches nothing,
     * so a detection routine finds the controller, finds no device, and moves
     * on. See src/vdd/vdd_ide.h for why 00h and not 7Fh.
     */
    g_IdeDevice = VddIdeDevice(&g_Ide);
    VddBusAdd(&g_Bus, &g_IdeDevice);            /* ATA: 1F0h-1F7h, 3F6h, 170h-177h, 376h-377h */
    /* ...and the BIOS says the same thing: 0040:0075, the number of fixed disks a
     * program reads before it calls INT 13h DL=80h, is WRITTEN 0 rather than left
     * to whatever the page held (docs/inventory/bda.md 3). One fact, three doors:
     * the BDA, INT 13h, and the adapter's empty channels.
     */
    *(volatile BYTE *)(ULONG_PTR)(BIOS_BDA_BASE + BIOS_BDA_FIXED_DISK_COUNT) = 0;
    g_Video.VideoMemory = (BYTE *)VIDEO_APERTURE_BASE;  /* the mapped A0000 aperture (RAM) */
    /* (per-plane backing is taken later, once the preamble is on disk -- every
     * LogWrite() before that point TRUNCATES the file and would eat its report.)
     */
    g_Video.TimeUs = HostTimeMicroseconds;               /* real CRT timebase for 0x3DA (#55) */
    g_Video.PresentHook = HostPresentHook;     /* Auto: the guest's frame raises the present (s73) */
    /* Opt-in only: the ring costs two stores on the hottest path in the program and
     * the dump does file I/O under g_Lock. See the note in vdd_video.h.
     */
    g_Video.IsPort3DaRingOn =
        (GetFileAttributesA(PITLATCH_FLAG) != INVALID_FILE_ATTRIBUTES);
    g_Video.GuestPc = HostGuestPc;             /* so a VRAM watchpoint names a routine */
    g_Video.BiosData = (BYTE *)BIOS_BDA_BASE;               /* the display's BDA fields (0449..0489) */
    g_VideoDevice = VddVideoDevice(&g_Video);
    VddBusAdd(&g_Bus, &g_VideoDevice);
    StartupLoadVideoWatch();
    /* AFTER the video VDD is on the bus (it needs st->bus to resolve a guest address). */
    /* #322: no font data ships -- the tables come from the system's fonts. Into the
     * report buffer: a LogAppend here would be erased when the report is rewritten.
     */
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
     * bus resets it, so the ring pointers it initialises land in guest memory where a DOS
     * program reading 0040:001A can see them. V86 low memory is mapped in our address
     * space, so the BDA is addressable directly.
     */
    g_Input.BiosData = (BYTE *)BIOS_BDA_BASE;
    g_Input.TimeMicroseconds = HostTimeMicroseconds;                /* the keyboard's transfer time is real time */
    g_InputDevice = VddInputDevice(&g_Input);
    VddBusAdd(&g_Bus, &g_InputDevice);             /* keyboard: claims INT 16h */
    /* THE BDA's PORT BASE-ADDRESS TABLE, WHICH WE HAD LEFT AT ZERO. (GH #128) (Importance = 2):
     * 0040:0000..0007 are the COM1..COM4 I/O bases and 0040:0008..000F the LPT1..LPT4
     * bases. Nothing ever wrote them, so every one read as 0 -- while our INT 11h
     * equipment word (0x4021) says bits 14-15 = 01 = ONE PARALLEL PORT. Our own BIOS
     * was contradicting itself: a port declared present in the equipment word whose
     * base address is 0.
     *
     * [INFO]: THAT INCONSISTENCY IS WHY COMM.DRV FAILS TO LOAD, measured end to end this
     * session. Its LibMain returns the word at 0040:0008 -- the LPT1 base address,
     * read through the BDA selector (`__0040H`) -- as its result. Zero means "DLL
     * initialisation failed"
     * (documented LibMain contract), LoadModule returns 0, and the boot dies with "NTVDM
     * KERNEL: Missing 16-bit system module
     * ... COMM.DRV". Five drivers whose LibMain returns 1 load; this one does not.
     *
     * [CAUTION]: SO WRITE ONLY WHAT THE EQUIPMENT WORD ALREADY CLAIMS -- one parallel port at
     * the standard LPT1 base, and NO serial ports. Filling in COM1..COM4 as well
     * would be inventing hardware nothing answers for, which is the "runs but lies"
     * class this project treats as its most expensive kind of bug. The equipment
     * word is the declaration; this table just stops disagreeing with it.
     *
     * [CAUTION]: AFTER the input VDD is on the bus: it initialises the keyboard ring through
     * the same 0040:0000 pointer, and doing this first would be overwritten.
     */
    /* AND NOW THERE ARE SERIAL PORTS, SO SAY SO. (GH #9, session 56) (Importance = 3):
     * The note above is right that filling in COM1..COM4 while nothing
     * answered for them would be inventing hardware -- that was the correct
     * call when there was no UART. There is one now: vdd_comm.c claims
     * 0x3F8..0x3FF and 0x2F8..0x2FF and answers every register, including the
     * local-loopback self-test every serial driver runs before it believes a
     * port exists. So the declaration is no longer a lie -- and the equipment
     * word is updated in the same breath, because the whole point of the
     * original note is that the two must not disagree.
     */
    { INT callbackIndex;

      for (callbackIndex = 0; callbackIndex < COMM_MAX_PORTS; ++callbackIndex)
      {
          g_ComSpool[callbackIndex] = INVALID_HANDLE_VALUE;
          g_ComFailed[callbackIndex] = 0; }

      g_Comm.Ports[0].BasePort = COMM_COM1_BASE;
      g_Comm.Ports[0].Irq = COMM_COM1_IRQ;
      g_Comm.Ports[0].IsFitted = 1;
      g_Comm.Ports[1].BasePort = COMM_COM2_BASE;
      g_Comm.Ports[1].Irq = COMM_COM2_IRQ;
      g_Comm.Ports[1].IsFitted = 1;
      /* #245 (s90): COM3 AND COM4 ARE FITTED BECAUSE STOCK DECLARES THEM. Measured
       * with tests/probes/dos/p_com34 under XP's own NTVDM on the rig: INT 11h
       * AX=C823 (FOUR serial ports, bits 9-11) and BDA 0040:0000 = 03F8 02F8 03E8
       * 02E8. The question #181 left open is answered by the oracle that defines
       * "ntvdm superset", and the device has had the slots since s85.
       */
      g_Comm.Ports[2].BasePort = COMM_COM3_BASE;
      g_Comm.Ports[2].Irq = COMM_COM1_IRQ;
      g_Comm.Ports[2].IsFitted = 1;
      g_Comm.Ports[3].BasePort = COMM_COM4_BASE;
      g_Comm.Ports[3].Irq = COMM_COM2_IRQ;
      g_Comm.Ports[3].IsFitted = 1;
      g_Comm.Printers[0].BasePort = LPT_DEFAULT_BASE;
      g_Comm.Printers[0].IsFitted = 1;   /* LPT1 data/strobe */
      g_Comm.Sink = ComTransmitSink;
      g_Comm.SinkContext = NULL;
      g_Comm.PrinterSink = LptTransmitSink;
      g_Comm.PrinterSinkContext = NULL;
      g_CommDevice = VddCommDevice(&g_Comm);
      VddBusAdd(&g_Bus, &g_CommDevice); }        /* 8250/16550A + INT 14h */
    VddNetBiosSetBackend(&g_Net, NetSubmit, NULL);
    g_NetDevice = VddNetBiosDevice(&g_Net);
    VddBusAdd(&g_Bus, &g_NetDevice);             /* GH #8: NetBIOS, INT 5Ch */
    { volatile WORD *biosDataArea = (volatile WORD *)(ULONG_PTR)BIOS_BDA_BASE;
      /* Declare exactly what the VDD actually CLAIMED. A port whose claim was
       * refused for want of a bus table slot is not fitted, and writing its base
       * here anyway would recreate the very inconsistency this block exists to
       * fix, one layer down.
       */
      /* ONE LOOP OVER THE VDD'S SLOTS, NOT FOUR LITERALS. (GH #181) (Importance = 1):
       * The table has room for COM1..COM4 and the VDD now has four slots,
       * so the row for each comes from the slot's own base and fitted flag
       * -- the same source BiosEquipmentWord() counts. COM3/COM4 are not
       * fitted above (that is an oracle question: whether a period machine
       * of the kind we model declares four ports, #181), so they still read
       * 0 here; the point is that fitting one is now a single line above
       * and this table and INT 11h follow it without being edited.
       */
      { INT callbackIndex;

        for (callbackIndex = 0; callbackIndex < BIOS_BDA_COM_PORTS; ++callbackIndex)
            biosDataArea[callbackIndex] = (WORD)(VddCommIsFitted(&g_Comm, callbackIndex) ? g_Comm.Ports[callbackIndex].BasePort : 0); }

      biosDataArea[BIOS_BDA_LPT_BASES / X86_WORD_SIZE] = (WORD)(VddLptIsFitted(&g_Comm, 0) ? LPT_DEFAULT_BASE : 0);   /* LPT1 */
      biosDataArea[BIOS_BDA_LPT_BASES / X86_WORD_SIZE + 1] = 0;
      biosDataArea[BIOS_BDA_LPT_BASES / X86_WORD_SIZE + 2] = 0; }                         /* LPT2..LPT3: none fitted */
    /* -- 000E, 0010, 0013 AND THE EBDA, FROM THE FUNCTIONS INT 11h/12h CALL. (#253)
     * 0040:000E is LPT4 on a PC and the EBDA segment on an AT and later; this block
     * used to zero it as "LPT4: none", which on an AT reads as "no EBDA" -- while
     * INT 12h said 639 KB, i.e. that one exists. bios_bda.h settles it: there is a
     * 1 KB EBDA at 9FC0h, and 000E, INT 15h C1h and the C0h table all say so.
     * 0010 and 0013 were never written at all; they now hold BiosEquipmentWord()
     * and BIOS_BASE_MEM_KB, the same expressions INT 11h and INT 12h return, so a
     * guest that reads the BDA and one that calls the interrupt see one machine.
     *
     * [CAUTION]: AFTER the COM/LPT slots are fitted (the word counts them) and after
     * SettingsApply() has set the joystick type (bit 12). g_BdaReady lets a later
     * settings change re-write 0010 -- see BiosBdaRefreshEquipment.
     */
    BiosBdaInitializeWithTop(NULL, BiosEquipmentWord(), g_DosMemoryTop);   /* #136 */
    g_BdaReady = 1;
    g_Speaker.Pit = &g_Pit;                         /* speaker tone <- PIT channel 2 */
    g_SpeakerDevice = VddSpeakerDevice(&g_Speaker);
    VddBusAdd(&g_Bus, &g_SpeakerDevice);            /* PC speaker: claims port 0x61 */
    g_Joystick.NowMicroseconds = JoystickNowMicroseconds;
    g_JoystickDevice = VddJoystickDevice(&g_Joystick);
    VddBusAdd(&g_Bus, &g_JoystickDevice);            /* gameport: 0x200-0x207 */
    /* [WARNING]: THE POLL THREAD IS NOT SPAWNED HERE. See JoystickPollEnsure: it is created
     * ONLY when a joystick is actually configured, so the default play config --
     * which is EVERY game that does not use a gamepad, Skyroads included -- runs
     * with the exact s61 thread landscape and cannot regress on its account.
     */
    g_DmaDevice = VddDmaDevice(&g_Dma);
    VddBusAdd(&g_Bus, &g_DmaDevice);            /* 8237 DMA: 0x00-0x0F/80-8F/C0-DF */
    g_Opl.IsExternalClock = 1;                        /* exec loop pumps real elapsed us */
    g_OplDevice = VddOplDevice(&g_Opl);
    VddBusAdd(&g_Bus, &g_OplDevice);            /* AdLib/OPL2: ports 0x388/0x389 */
    g_Sb.Dma = &g_Dma;
    g_Sb.Opl = &g_Opl;       /* SB pulls PCM via DMA, mirrors FM */
    /* [CAUTION]: THE SAME NUMBERS THAT GO INTO BLASTER (dos_env.h). If these two ever come
     * from different places, a driver is told one port and finds another.
     */
    g_Sb.BasePort = g_SbConfig.IoBase;
    g_Sb.Irq = g_SbConfig.Irq;
    g_Sb.Dma8 = g_SbConfig.Dma8Channel;

    if (g_SbConfig.Dma16Channel)
        g_Sb.Dma16 = g_SbConfig.Dma16Channel;                            /* 0 = keep vdd_sb's default */

    /* Opt-in raw PCM capture -- see SB_STATE.CaptureBuffer. 4 MB is ~3 minutes of Doom's
     * 11025 Hz stereo, and it is a static buffer so the audio thread never allocates.
     */
    if (GetFileAttributesA(SBDUMP_FLAG) != INVALID_FILE_ATTRIBUTES)
    {
        static BYTE sbCapture[4u * 1024u * 1024u];
        g_Sb.CaptureBuffer = sbCapture;
        g_Sb.CaptureCapacity = sizeof sbCapture;
        g_Sb.CaptureLength = 0;
    }

    g_SbDevice = VddSbDevice(&g_Sb);
    VddBusAdd(&g_Bus, &g_SbDevice);             /* Sound Blaster 16: 0x220-0x22F */
    g_Mpu.Sink = HostMidiSink;
    {   static const WORD mpuBases[5] = { 0x300, 0x310, 0x320, 0x330, 0x340 };   /* #235 */
        g_Mpu.BasePort = mpuBases[g_Settings.Values[SET_MPUADDR] <= ARRAYSIZE(mpuBases) - 1 ? g_Settings.Values[SET_MPUADDR] : MPU_DEFAULT_BASE_CHOICE]; }
    g_MpuDevice = VddMpuDevice(&g_Mpu);
    VddBusAdd(&g_Bus, &g_MpuDevice);            /* MPU-401 MIDI: 0x330/0x331 */
    /* The Gravis UltraSound: 240h-24Fh and 340h-347h, IRQ 11, DMA 3 (docs/ref/gus.md).
     *
     * [CAUTION]: THE SAME NUMBERS GO INTO ULTRASND= -- see the environment build.
     */
    if (g_GusOn)                               /* decided at startup: see NOGUS_FLAG's read */
    {
        g_Gus.Dma = &g_Dma;
        g_Gus.Dram = g_GusDram;
        g_GusMidi.Sink = HostMidiSink;            /* #190: the 6850 UART -> the synth */
        g_Gus.MidiSink = GusMidiToSynth;
        g_GusDevice = VddGusDevice(&g_Gus);
        VddBusAdd(&g_Bus, &g_GusDevice);
    }

    /* #233: the AWE32's EMU8000 -- at the SB's base + 400h / 800h / C00h (620h, A20h,
     * E20h for a card at 220h), 512 KB of sample DRAM, and BLASTER's E says where.
     */
    g_AweOn = (g_Settings.Values[SET_SBMODEL] == SB_MODEL_AWE32);

    if (g_AweOn)
    {
        g_Emu8K.BasePort = (WORD)(g_SbConfig.IoBase + SB_EMU8K_PORT_OFFSET);
        g_Emu8K.Dram = g_Emu8KDram;
        g_Emu8K.DramWords = EMU8K_DRAM_WORDS;
        g_Emu8KDevice = VddEmu8kDevice(&g_Emu8K);
        VddBusAdd(&g_Bus, &g_Emu8KDevice);
    }

    /* - SAY WHETHER EVERY DEVICE ACTUALLY GOT ON THE BUS. VDD_MAX_PORT_RANGES was 16 and
     * exactly full; adding one range pushed the LAST device added -- the MPU-401 --
     * off, its claim returned -1, nobody looked, and the guest's MIDI port read 0xFF
     * like an empty slot. Doom reset it four times, got nothing, and played no music.
     * A device that cannot get on the bus is not a detail to discover by diffing
     * port traces against a working run.
     */
    /* (the bus health line is emitted after the preamble is written -- see below;
     * every LogWrite() before then TRUNCATES the file.)
     */
    /* Start the mixer + audio thread. This is also the TRANSPORT: it is what
     * walks the SB's DMA buffer and raises the block-completion IRQ, so it must
     * run even if no sound device opens (AUDIO_WAVE falls back to silent
     * pumping) -- otherwise every SB game hangs on a machine without audio.
     */
    VddAudioInitialize(&g_Audio, &g_Opl, &g_Sb, SettingsOutputHz(&g_Settings));
    VddAudioSetGus(&g_Audio, g_GusOn ? &g_Gus : NULL);
    VddAudioSetEmu8k(&g_Audio, g_AweOn ? &g_Emu8K : NULL);   /* #233 */
    SettingsApplyDevices(&g_Settings);   /* master volume, mute, speaker -- the mixer
                                         zeroes its own struct, so not one line earlier */
    return cursor;
}

/* Build DOS in conventional memory: load the program, plant the INT 21h and BIOS stubs, fill the IVT, build the environment, the command tail, the MCB chain and the List of Lists, settle the version, plant the country and drive tables, and start XMS and EMS. */
static VOID StartupBuildDos(
    PSTR *cursorIo,
    const DWORD readCount,
    DOS_IMAGE *image,
    CHAR *programPathBuffer,
    CHAR *args,
    DOS_MACHINE *machine,
    CHAR *report)
{
    PSTR cursor = *cursorIo;
    volatile BYTE * handlerArea;
    UINT index;

    /* If this is a bound linear executable (every DOS/4GW game is one), learn which of
     * its objects are code before it starts asking us for memory to load them into.
     */
    DpmiLeLearn(g_FileBuffer, readCount);

    cursor = StartupApplyConventionalKb(cursor, readCount);
    /* Build the DOS process in conventional memory (base=NULL => absolute V86). */
    (*image) = DosLoadImage(NULL, g_FileBuffer, readCount, DOS_PSP_SEG);

    static const BYTE bop[] = { VDM_BOP0, VDM_BOP1, DOS_BOP_INT21, X86_OP_IRET };  /* BOP 0x20 ; iret */
    static const BYTE bop10[] = { VDM_BOP0, VDM_BOP1, DOS_BOP_FOR_VECTOR(VECTOR_VIDEO), X86_OP_IRET }; /* BOP 0x10 ; iret */
    static const BYTE bop16[] = { VDM_BOP0, VDM_BOP1, DOS_BOP_FOR_VECTOR(VECTOR_KEYBOARD_SERVICES), X86_OP_IRET }; /* BOP 0x16 ; iret */
    static const BYTE bop33[] = { VDM_BOP0, VDM_BOP1, DOS_BOP_FOR_VECTOR(VECTOR_MOUSE), X86_OP_IRET }; /* BOP 0x33 ; iret */
    /* INT 08h (timer): tick via BOP, then chain INT 1Ch, then iret. INT 1Ch is a
     * bare iret by default (the user-timer hook a program may repoint). INT 1Ah
     * (BIOS time-of-day) is a plain BOP.
     */
    static const BYTE bop08[] = { VDM_BOP0, VDM_BOP1, DOS_BOP_FOR_VECTOR(VECTOR_TIMER), X86_OP_INT, VECTOR_USER_TICK, X86_OP_IRET };
    static const BYTE bop1c[] = { X86_OP_IRET };                           /* iret stub */
    /* Default INT 09h = BOP 09 ; IRET. It must CONSUME the scancode, exactly as the BIOS
     * handler does: a bare IRET left the byte in the controller forever, so with the 8042's
     * proper one-byte-at-a-time pacing no further key could ever raise an interrupt (the
     * whole keyboard died after one press). A game that installs its own INT 09h replaces
     * this vector, so its handler still reads port 0x60 itself.
     */
    static const BYTE bop09[] = { VDM_BOP0, VDM_BOP1, DOS_BOP_FOR_VECTOR(VECTOR_KEYBOARD), X86_OP_IRET };
    static const BYTE bop1a[] = { VDM_BOP0, VDM_BOP1, DOS_BOP_FOR_VECTOR(VECTOR_TIME), X86_OP_IRET }; /* BOP 0x1A ; iret */
    static const BYTE bop2f[] = { VDM_BOP0, VDM_BOP1, DOS_BOP_FOR_VECTOR(VECTOR_MULTIPLEX), X86_OP_IRET }; /* INT 2Fh ; iret */
    /* XMS API entry: reached by FAR CALL (INT 2Fh AX=4310 hands back ES:BX), so it
     * ends in RETF (0xCB), not IRET.
     */
    static const BYTE xmsBopStub[] = { VDM_BOP0, VDM_BOP1, DOS_BOP_XMS_ENTRY, X86_OP_RETF };
    static const BYTE bop67[] = { VDM_BOP0, VDM_BOP1, DOS_BOP_FOR_VECTOR(VECTOR_EMS), X86_OP_IRET }; /* INT 67h ; iret */
    static const BYTE emmDeviceName[] = { 'E','M','M','X','X','X','X','0' };  /* EMS device header name */
    handlerArea = (volatile BYTE *)(DOS_HDLR_SEG << PARAGRAPH_SHIFT);            /* INT 21h BOP handler */

    for (index = 0; index < sizeof(bop); ++index)
        handlerArea[DOS_HDLR_INT21_STUB_OFF + index] = bop[index];

    *(volatile WORD *)IVT_OFFSET_ADDRESS(VECTOR_DOS) = DOS_HDLR_INT21_STUB_OFF;                        /* IVT[0x21].offset */
    *(volatile WORD *)IVT_SEGMENT_ADDRESS(VECTOR_DOS) = DOS_HDLR_SEG;                  /* IVT[0x21].segment */
    handlerArea[DOS_DBCS_OFF] = 0;
    handlerArea[DOS_DBCS_OFF + 1] = 0;     /* empty DBCS table */

    for (index = 0; index < sizeof(bop10); ++index)
        handlerArea[DOS_HDLR_INT10_STUB_OFF + index] = bop10[index];                                              /* INT 10h stub */

    *(volatile WORD *)IVT_OFFSET_ADDRESS(VECTOR_VIDEO) = DOS_HDLR_INT10_STUB_OFF;                        /* IVT[0x10].offset */
    *(volatile WORD *)IVT_SEGMENT_ADDRESS(VECTOR_VIDEO) = DOS_HDLR_SEG;                  /* IVT[0x10].segment */

    for (index = 0; index < sizeof(bop16); ++index)
        handlerArea[DOS_HDLR_INT16_STUB_OFF + index] = bop16[index];                                              /* INT 16h stub */

    *(volatile WORD *)IVT_OFFSET_ADDRESS(VECTOR_KEYBOARD_SERVICES) = DOS_HDLR_INT16_STUB_OFF;                        /* IVT[0x16].offset */
    *(volatile WORD *)IVT_SEGMENT_ADDRESS(VECTOR_KEYBOARD_SERVICES) = DOS_HDLR_SEG;                  /* IVT[0x16].segment */

    for (index = 0; index < sizeof(bop33); ++index)
        handlerArea[DOS_HDLR_INT33_STUB_OFF + index] = bop33[index];                                              /* INT 33h stub */

    *(volatile WORD *)(IVT_OFFSET_ADDRESS(VECTOR_MOUSE))     = DOS_HDLR_INT33_STUB_OFF;              /* IVT[0x33].offset */
    *(volatile WORD *)(IVT_SEGMENT_ADDRESS(VECTOR_MOUSE)) = DOS_HDLR_SEG;        /* IVT[0x33].segment */

    for (index = 0; index < sizeof(bop08); ++index)
        handlerArea[DOS_HDLR_INT08_STUB_OFF + index] = bop08[index];                                              /* INT 08h stub */

    *(volatile WORD *)(IVT_OFFSET_ADDRESS(VECTOR_TIMER))     = DOS_HDLR_INT08_STUB_OFF;              /* IVT[0x08].offset */
    *(volatile WORD *)(IVT_SEGMENT_ADDRESS(VECTOR_TIMER)) = DOS_HDLR_SEG;        /* IVT[0x08].segment */

    for (index = 0; index < sizeof(bop1c); ++index)
        handlerArea[DOS_HDLR_INT1C_STUB_OFF + index] = bop1c[index];                                              /* INT 1Ch iret */

    *(volatile WORD *)(IVT_OFFSET_ADDRESS(VECTOR_USER_TICK))     = DOS_HDLR_INT1C_STUB_OFF;              /* IVT[0x1C].offset */
    *(volatile WORD *)(IVT_SEGMENT_ADDRESS(VECTOR_USER_TICK)) = DOS_HDLR_SEG;        /* IVT[0x1C].segment */

    for (index = 0; index < sizeof(bop09); ++index)
        handlerArea[DOS_HDLR_INT09_STUB_OFF + index] = bop09[index];                                              /* INT 09h default iret (0x4C-0x4F) */

    /* INT 33h event-handler return: the guest's handler RETFs here (see MouseCallbackTry). */
    handlerArea[MS_CB_RET_OFF + 0] = VDM_BOP0;
    handlerArea[MS_CB_RET_OFF + 1] = VDM_BOP1;
    handlerArea[MS_CB_RET_OFF + VDM_BOP_NUMBER_OFFSET] = MS_CB_BOP;
    handlerArea[MS_CB_RET_OFF + VDM_BOP_LENGTH] = X86_OP_IRET;
    /* DEFAULT DEVICE-IRQ HANDLERS. A real BIOS points the unused hardware vectors at a
     * handler that just acknowledges and returns; we had them pointing at whatever junk was
     * in the IVT, which on this box read F000:A390 -- unowned ROM. That was harmless only so
     * long as we could not deliver a device IRQ asynchronously. Now that we can, injecting an
     * IRQ the guest has not hooked jumps it into that junk and hangs it: measured, Skyroads
     * (which never installs a Sound Blaster ISR at all) froze at F000:A390 the moment its DMA
     * block completed. So give IRQ2-7 and IRQ8-15 a plain IRET, exactly as INT 09h has.
     */
    handlerArea[DOS_IRET_STUB_OFF] = X86_OP_IRET;                                /* shared IRET stub */
    handlerArea[DOS_CASEMAP_OFF]   = X86_OP_RETF;                                /* AH=38h case map: RETF */
    { volatile BYTE *swappableDataArea = (volatile BYTE *)(ULONG_PTR)((DWORD)DOS_SDA_SEG << PARAGRAPH_SHIFT);   /* AH=34h/5D06h */
      INT item;

      for (item = 0; item < DOS_SDA_LEN; ++item)
          swappableDataArea[DOS_SDA_OFF + item] = 0; }

    for (index = VECTOR_IRQ2; index <= VECTOR_IRQ7; ++index)
    {
        *(volatile WORD *)(IVT_OFFSET_ADDRESS(index))     = DOS_IRET_STUB_OFF;
        *(volatile WORD *)(IVT_SEGMENT_ADDRESS(index)) = DOS_HDLR_SEG;
    }

    for (index = VECTOR_IRQ8; index <= VECTOR_IRQ15; ++index)
    {
        *(volatile WORD *)(IVT_OFFSET_ADDRESS(index))     = DOS_IRET_STUB_OFF;
        *(volatile WORD *)(IVT_SEGMENT_ADDRESS(index)) = DOS_HDLR_SEG;
    }

    *(volatile WORD *)(IVT_OFFSET_ADDRESS(VECTOR_KEYBOARD))     = DOS_HDLR_INT09_STUB_OFF;              /* IVT[0x09].offset */
    *(volatile WORD *)(IVT_SEGMENT_ADDRESS(VECTOR_KEYBOARD)) = DOS_HDLR_SEG;        /* IVT[0x09].segment */

    for (index = 0; index < sizeof(bop1a); ++index)
        handlerArea[DOS_HDLR_INT1A_STUB_OFF + index] = bop1a[index];                                              /* INT 1Ah stub */

    *(volatile WORD *)(IVT_OFFSET_ADDRESS(VECTOR_TIME))     = DOS_HDLR_INT1A_STUB_OFF;              /* IVT[0x1A].offset */
    *(volatile WORD *)(IVT_SEGMENT_ADDRESS(VECTOR_TIME)) = DOS_HDLR_SEG;        /* IVT[0x1A].segment */

    for (index = 0; index < sizeof(bop2f); ++index)
        handlerArea[DOS_HDLR_INT2F_STUB_OFF + index] = bop2f[index];                                              /* INT 2Fh stub */

    *(volatile WORD *)(IVT_OFFSET_ADDRESS(VECTOR_MULTIPLEX))     = DOS_HDLR_INT2F_STUB_OFF;              /* IVT[0x2F].offset */
    *(volatile WORD *)(IVT_SEGMENT_ADDRESS(VECTOR_MULTIPLEX)) = DOS_HDLR_SEG;        /* IVT[0x2F].segment */

    for (index = 0; index < sizeof(xmsBopStub); ++index)
        handlerArea[XMS_ENTRY_OFF + index] = xmsBopStub[index];                                                   /* XMS far-call entry */

    /* [CAUTION]: GH #47: a non-zero word at XMS_ENTRY_OFF+0x45 WAS TRIED AND REFUTED.
     * MEM.EXE skips its whole extended-memory report on a zero word at +0x45 of
     * some structure, and that structure looked like the XMS entry. It is not:
     * planting HIMEM's own bytes (EB 50) at the entry changed nothing. Sixth
     * refutation.
     */
    for (index = 0; index < sizeof(bop67); ++index)
        handlerArea[DOS_HDLR_INT67_STUB_OFF + index] = bop67[index];                                              /* INT 67h (EMM) stub */

    /* [CAUTION]: NO EMS MEANS NO INT 67h VECTOR AND NO DEVICE NAME. Both halves, because
     * a program detects EMM by either following the vector to the "EMMXXXX0"
     * header OR by opening the device; leaving one of them behind is a manager
     * that half-exists, which is worse for a guest than one that does not.
     */
    if (g_EmsOn)
    {
        *(volatile WORD *)(IVT_OFFSET_ADDRESS(VECTOR_EMS))     = DOS_HDLR_INT67_STUB_OFF;          /* IVT[0x67].offset */
        *(volatile WORD *)(IVT_SEGMENT_ADDRESS(VECTOR_EMS)) = DOS_HDLR_SEG;    /* IVT[0x67].segment */
    }
    else
    {
        *(volatile WORD *)(IVT_OFFSET_ADDRESS(VECTOR_EMS))     = 0;
        *(volatile WORD *)(IVT_SEGMENT_ADDRESS(VECTOR_EMS)) = 0;
    }

    /* DPMI mode-switch entry (far-called): BOP 0x50 ; RETF. The host services the
     * BOP by switching to PM; the RETF only executes if the switch fails.
     */
    handlerArea[DPMI_ENTRY_OFF + 0] = VDM_BOP0;
    handlerArea[DPMI_ENTRY_OFF + 1] = VDM_BOP1;
    handlerArea[DPMI_ENTRY_OFF + VDM_BOP_NUMBER_OFFSET] = DPMI_BOP;
    handlerArea[DPMI_ENTRY_OFF + VDM_BOP_LENGTH] = X86_OP_RETF; /* RETF */
    /* DPMI 0301 real-mode-call return catcher: BOP 0x54 (no IRET/RETF -- the 0301
     * handler detects it and returns to PM, it never resumes past it).
     */
    handlerArea[DPMI_RMRET_OFF + 0] = VDM_BOP0;
    handlerArea[DPMI_RMRET_OFF + 1] = VDM_BOP1;
    handlerArea[DPMI_RMRET_OFF + VDM_BOP_NUMBER_OFFSET] = DPMI_RMRET_BOP;
    /* DPMI 0303 real-mode callback entries (one per slot) + the PM-return catcher. */
    { INT callbackSlot;

    for (callbackSlot = 0; callbackSlot < DPMI_CB_SLOTS; ++callbackSlot)
    {
        WORD entry = DpmiCallbackEntry(DPMI_CB_BASE_OFF, callbackSlot);
        handlerArea[entry + 0] = VDM_BOP0;
        handlerArea[entry + 1] = VDM_BOP1;
        handlerArea[entry + VDM_BOP_NUMBER_OFFSET] = DPMI_CB_BOP;
    } }

    handlerArea[DPMI_PMRET_OFF + 0] = VDM_BOP0;
    handlerArea[DPMI_PMRET_OFF + 1] = VDM_BOP1;
    handlerArea[DPMI_PMRET_OFF + VDM_BOP_NUMBER_OFFSET] = DPMI_PMRET_BOP;
    /* 0306 raw mode-switch entries. Both are bare BOPs: the host completes the switch
     * by rewriting the CONTEXT, so control never resumes past the BOP and no RETF/IRET
     * tail is wanted (the same shape as DPMI_RMRET_OFF). The protected-to-real entry
     * lives in this segment too and is reached through a code selector based here --
     * see the 0306 handler.
     */
    handlerArea[DPMI_RAW2PM_OFF + 0] = VDM_BOP0;
    handlerArea[DPMI_RAW2PM_OFF + 1] = VDM_BOP1;
    handlerArea[DPMI_RAW2PM_OFF + VDM_BOP_NUMBER_OFFSET] = DPMI_RAW2PM_BOP;
    handlerArea[DPMI_RAW2RM_OFF + 0] = VDM_BOP0;
    handlerArea[DPMI_RAW2RM_OFF + 1] = VDM_BOP1;
    handlerArea[DPMI_RAW2RM_OFF + VDM_BOP_NUMBER_OFFSET] = DPMI_RAW2RM_BOP;
    /* 0305 save/restore: a register-preserving no-op (see the define). */
    handlerArea[DPMI_SSR_OFF] = X86_OP_RETF;                               /* RETF */
    /* (GH #18 run 67: the PM-fault handler BOP is planted at the handler CODE selector's
     * DPMI_FAULT_COFF by DpmiInstallFaultTrampoline(), not here.)
     */
    /* EMS detection method 2: programs read the INT 67h vector's segment:000Ah for
     * the device-driver name "EMMXXXX0". Park it in the handler segment.
     */
    if (g_EmsOn)
        for (index = 0; index < sizeof(emmDeviceName); ++index)
            handlerArea[DOS_EMM_NAME_OFF + index] = emmDeviceName[index];

    StartupPlantBiosStubs();

    /* INT 22h / 23h / 24h: REAL VECTORS, SO THE PSP CAN SAVE SOMETHING. (#34):
     * Every PSP stores the live copies of these three and restores them at exit.
     * They were whatever the IVT happened to hold, and the PSP fields were zero
     * -- which passes "the saved copy matches the live vector" trivially when
     * both are 0000:0000, so the gap could not be seen from that test alone.
     * - INT 24h returns AL=3, FAIL THE CALL. Real DOS's default lives in
     *   COMMAND.COM and prompts Abort/Retry/Ignore/Fail; there is no shell here to
     *   prompt with, and of the four answers FAIL is the only one that hands the
     *   error back to the program that can report it. IGNORE would corrupt data
     *   and RETRY would spin forever. Documented rather than chosen silently.
     * - INT 23h (Ctrl-Break) is a bare IRET: returning with CF clear means
     *   "carry on", which is what a host with no shell to return to should do.
     * - INT 22h (terminate address) routes to the same BOP as INT 20h, so a guest
     *   that jumps there actually exits instead of falling through the IVT.
     */
    {   volatile BYTE *controlBytes = (volatile BYTE *)(DOS_CTAB_SEG << PARAGRAPH_SHIFT);
        UINT position = DOS_CRIT_STUBS;
        controlBytes[position+0] = VDM_BOP0;
        controlBytes[position+1] = VDM_BOP1;
        controlBytes[position+VDM_BOP_NUMBER_OFFSET] = DOS_BOP_INT20;
        controlBytes[position+VDM_BOP_LENGTH] = X86_OP_IRET;
        *(volatile WORD *)(IVT_OFFSET_ADDRESS(VECTOR_TERMINATE_ADDRESS))     = (WORD)position;              /* INT 22h */
        *(volatile WORD *)(IVT_SEGMENT_ADDRESS(VECTOR_TERMINATE_ADDRESS)) = DOS_CTAB_SEG;
        controlBytes[position+DOS_CRIT_STUB_INT23] = X86_OP_IRET;                                          /* INT 23h: IRET */
        *(volatile WORD *)(IVT_OFFSET_ADDRESS(VECTOR_CTRL_C))     = (WORD)(position + DOS_CRIT_STUB_INT23);
        *(volatile WORD *)(IVT_SEGMENT_ADDRESS(VECTOR_CTRL_C)) = DOS_CTAB_SEG;
        controlBytes[position+DOS_CRIT_STUB_INT24] = X86_OP_MOV_IMM_BYTE_FIRST;
        controlBytes[position+DOS_CRIT_STUB_INT24+1] = DOS_CRIT_ACTION_FAIL;
        controlBytes[position+DOS_CRIT_STUB_INT24+2] = X86_OP_IRET;         /* mov al,3 ; iret */
        *(volatile WORD *)(IVT_OFFSET_ADDRESS(VECTOR_CRITICAL_ERROR))     = (WORD)(position + DOS_CRIT_STUB_INT24);        /* INT 24h */
        *(volatile WORD *)(IVT_SEGMENT_ADDRESS(VECTOR_CRITICAL_ERROR)) = DOS_CTAB_SEG;
        /* #34: the site DOS calls the guest's INT 24h from -- see CriticalRaise. */
        controlBytes[DOS_CRIT_RAISE + 0] = X86_OP_INT;
        controlBytes[DOS_CRIT_RAISE + 1] = VECTOR_CRITICAL_ERROR;   /* int 24h */
        controlBytes[DOS_CRIT_RETURN + 0] = VDM_BOP0;
        controlBytes[DOS_CRIT_RETURN + 1] = VDM_BOP1;
        controlBytes[DOS_CRIT_RETURN + VDM_BOP_NUMBER_OFFSET] = DOS_BOP_INT21;                                 /* bop 20h */
    }
    /* #251: DOS's AUX/PRN driver code, which INT 21h resumes the guest in -- see
     * dos_auxprn.asm for why it is guest code and what it was measured against.
     */
    {   volatile BYTE *controlBytes = (volatile BYTE *)(DOS_CTAB_SEG << PARAGRAPH_SHIFT);
        UINT item;

        for (item = 0; item < sizeof(g_DosAuxPrnCode); ++item)
            controlBytes[DOS_AUXPRN_OFF + item] = g_DosAuxPrnCode[item];

        /* #254: the BIOS INT 09h's side-calls -- see bios_kbdact.asm. */
        for (item = 0; item < sizeof(g_BiosKeyboardActionCode); ++item)
            controlBytes[DOS_KBDACT_OFF + item] = g_BiosKeyboardActionCode[item];

        /* #274: INT 05h IS OURS NOW -- THE BIOS PRINT-SCREEN ROUTINE (p5):
         * A fresh VDM left IVT[05h] at F000:FF54, a jump deeper into the VDM's own ROM
         * that KeyboardActionEntry refuses to enter (p_ivtkbd), so Print Screen called nothing
         * and a program's own `int 5` went somewhere we cannot vouch for. Every BIOS
         * since the PC has a routine here; ours prints the screen through INT 17h and
         * keeps its status HOST-side -- see PrintScreenBop for why not at 0050:0000.
         */
        *(volatile WORD *)(ULONG_PTR)(IVT_OFFSET_ADDRESS(VECTOR_PRINT_SCREEN))     = (WORD)(DOS_KBDACT_OFF + BIOS_KEYBOARD_ACTION_DEFAULT_INT05);
        *(volatile WORD *)(ULONG_PTR)(IVT_SEGMENT_ADDRESS(VECTOR_PRINT_SCREEN)) = DOS_CTAB_SEG;
    }

    /* GH #27 -- THE NULL-VECTOR LANDMINE. A vector left at 0000:0000 sends a guest
     * that INTs it to 0000:0000, where it executes the interrupt vector table
     * itself as code. Point any such vector at the shared IRET stub.
     * MEASURED BEFORE FIXING, and the measurement narrowed the fix: on the
     * bare-metal rig most unclaimed vectors are NOT null -- they carry the VDM's
     * own BIOS entries (INT 13h read F000:5595, INT 11h F000:F84D). Planting over
     * those would swap a working handler for a bare IRET, i.e. a silent
     * "success", which is the very failure mode this issue exists to remove. So
     * fill only the genuinely null ones, and name them in the log.
     */
    { INT number, count = 0, start = -1;
      cursor = LogPut(cursor, "STAGE0: null IVT vectors -> IRET stub:");

      for (number = 0; number <= IVT_VECTORS; ++number)                    /* 256 flushes a trailing run */
      {
          INT isNullVector = (number < IVT_VECTORS) && (*(volatile DWORD *)(IVT_OFFSET_ADDRESS(number)) == 0);

          if (isNullVector)
          {
              *(volatile WORD *)(IVT_OFFSET_ADDRESS(number))     = DOS_IRET_STUB_OFF;
              *(volatile WORD *)(IVT_SEGMENT_ADDRESS(number)) = DOS_HDLR_SEG;

              if (start < 0)
                  start = number;

              ++count;
          }
          else if (start >= 0)                    /* emit as ranges, not 133 items */
          {
              cursor = LogPut(cursor, " 0x"); cursor = LogHexByte(cursor, (UINT)start);

              if (number - 1 > start)
              {
                  cursor = LogPut(cursor, "-0x");
                  cursor = LogHexByte(cursor, (UINT)(number - 1));
              }

              start = -1;
          }
      }

      if (!count)
          cursor = LogPut(cursor, " none");

      cursor = LogPut(cursor, "\r\n"); }

    DosPspBuild(NULL, DOS_PSP_SEG, DOS_ENV_SEG, g_DosMemoryTop);   /* #136 */
    /* AFTER the vectors above are planted, never before: saving a vector that is
     * still 0000:0000 stores a null the program restores on the way out. Parent
     * PSP = our own, since nothing launched us from inside the VDM. (GH #34)
     */
    DosPspSaveVectors(NULL, DOS_PSP_SEG, DOS_PSP_SEG);
    /* -- EXTRA ENVIRONMENT VARIABLES FROM dosenv.txt. Read here, next to the block
     * being built, so a knob that is absent costs exactly one failed open and the
     * environment is byte-identical to what it has always been.
     */
    DsProbeLoad();          /* the #GP fault report's named guest data words */
    cursor = StartupBuildEnvironment(cursor, programPathBuffer);
    DosPspBuildCommandTail(NULL, DOS_PSP_SEG, args);                                    /* M2.5: args */
    /* - DUMP THE TAIL AS THE GUEST WILL SEE IT. Passing ANY argument makes DOS/4GW
     * quit before printing a single character, with a DPMI/INT 21h trace identical
     * to a working run for all 617 of its lines -- so the branch it takes is on
     * MEMORY, and this is the memory. Length byte, the bytes, and the terminator.
     */
    { volatile BYTE *pspView = (volatile BYTE *)((DWORD)DOS_PSP_SEG << PARAGRAPH_SHIFT);
      UINT textIndex;
      cursor = LogPut(cursor, "STAGE2: cmdtail len=0x"); cursor = LogHexByte(cursor, pspView[DOS_PSP_COMMAND_TAIL_LENGTH]);
      cursor = LogPut(cursor, " [");

      for (textIndex = 0; textIndex < COMMAND_TAIL_DUMP_BYTES; ++textIndex)
      {
          cursor = LogHexByte(cursor, pspView[DOS_PSP_COMMAND_TAIL + textIndex]);
          cursor = LogPut(cursor, " ");
      }

      cursor = LogPut(cursor, "]\r\n"); }
    StartupBuildMemoryChain(machine, programPathBuffer);
    /* Published so the Settings dialog can change the reported DOS version while a
     * guest is running -- it is read per INT 21h AH=30h, so it takes effect at the
     * guest's next version check with no restart.
     */
    g_DosMachine = machine;
    DosInt21SetVersion(machine, (BYTE)g_Settings.Values[SET_DOSMAJ], (BYTE)g_Settings.Values[SET_DOSMIN]);
    /* Two sources, and the second one silently wins -- see the note below the file read. */
    PCSTR dosVersionSource = "HKCU\\Software\\NTVDMEX (Settings dialog)";
    /* -- THE REPORTED DOS VERSION IS A KNOB, BECAUSE IT IS A LIE THE GUEST CHOOSES.
     * Real DOS ships SETVER for precisely this, and the number is not a fact about
     * us: it is what a particular guest will accept. We default to 6.22 to match the
     * M9 oracle, and XP's OWN COMMAND.COM refuses that outright -- "Incorrect DOS
     * version", INT 21h AH=00h, terminated before it printed a prompt. NT's DOS has
     * always reported 5.00 and its shell is built to match.
     * `dosver.txt` on the share: "5.0", "6.22", "3.31" -- major.minor decimal.
     */
    /* AN NTVDM-AWARE SHELL GETS 5.00 WITHOUT ANYONE HAVING TO ASK. (s79) (Importance = 1):
     * XP's COMMAND.COM accepts only AX = 5 exactly (5.00 -- observed: 6.22 is
     * refused) and prints "Incorrect DOS version" otherwise, so on the default 6.22 a
     * double-click would die before it
     * printed anything. This is not a global policy change: it applies only to a
     * guest we loaded AS THE SHELL that carries NTVDM's own BOPs (see the scan), and
     * `cfg\dosver.txt` below still overrides it. 6.22's COMMAND.COM has no BOPs and
     * is untouched -- it keeps 6.22, which is the version it expects.
     */
    /* s81 (#208), the user's choice: SETVER, NOT A SESSION-WIDE 5.00. Every program
     * started from Windows now runs UNDER this shell, so forcing the whole session to
     * 5.00 would have changed the version every program sees. Only the SHELL'S OWN
     * PROCESS is told 5.00 (DosInt21SetShellPsp); what it runs gets the setting.
     */
    if (g_GuestNtAware)
    {
        DosInt21SetShellPsp(machine, DOS_PSP_SEG, TRUE);
        dosVersionSource = "the setting -- the NTVDM-aware shell ITSELF is told 5.00 (per process, SETVER-style)";
        g_DosVersionShell = 1;
    }
    else if (g_GuestNtvdmBops >= NT_AWARE_SHELL_BOPS_MIN && g_TopIsShell)
    {
        /* s91: XP's COMMAND.COM launched AS THE PROGRAM -- `command.com /c prog > file`
         * from cmd.exe, or a user typing `command` there. It is not "the shell we
         * chose", so the rule above did not apply, and on the default 6.22 it said
         * "Incorrect DOS version" and quit where stock runs it (launch matrix row 5,
         * runs/s91/chain18b). Same image test and same per-process 5.00 as the
         * EXEC path gives a second XP shell; the name check is the second factor
         * that keeps an innocent guest from being told DOS 5.
         */
        DosInt21SetShellPsp(machine, DOS_PSP_SEG, TRUE);
        dosVersionSource = "the setting -- XP's COMMAND.COM run as the program is told 5.00 (per process, SETVER-style)";
        g_DosVersionShell = 1;
    }

    dosVersionSource = StartupLoadDosVersionKnob(dosVersionSource, machine);
    /* SAY WHICH VERSION IS IN FORCE, AND WHERE IT CAME FROM. EVERY RUN (Importance = 1):
     * This printed a line ONLY when `dosver.txt` overrode, so the persistent source
     * -- HKCU\Software\NTVDMEX\DosVersionMajor/Minor, written by the Settings dialog
     * -- was completely silent. The rig was found reporting **5.00 to every DOS
     * guest** from that registry value, with no `dosver.txt` anywhere, on a project
     * whose entire parity method diffs against a 6.22 oracle (`p_ver.com`:
     * `int21.30 AX=0005`). Deleting the file, which is what every note about this
     * knob says to do afterwards, does NOT restore the default -- and nothing
     * anywhere reported the discrepancy.
     *
     * [CAUTION]: The standing rule this breaks is "a status line nobody reads is not a check";
     * this was worse, because there was no line at all. Unconditional now, and it
     * names the SOURCE, because the number alone would not have caught it either.
     *
     * [CAUTION]: DECIMAL, and it took a wrong reading to notice. The first cut used zhexb and
     * printed "6.22" as **06.16**, which a human reads as version 6.16 -- a number in
     * the wrong units is not a measurement, and this line exists precisely so nobody
     * has to decode it. Minor is zero-padded to two digits: "6.2" and "6.20" are
     * different DOS versions.
     */
    cursor = LogPut(cursor, "STAGE2: DOS version reported = ");
    cursor = LogDecimal(cursor, machine->VersionMajor); cursor = LogPut(cursor, ".");

    if (machine->VersionMinor < 10)
        cursor = LogPut(cursor, "0");

    cursor = LogDecimal(cursor, machine->VersionMinor);
    cursor = LogPut(cursor, " (source: "); cursor = LogPut(cursor, dosVersionSource); cursor = LogPut(cursor, ")\r\n");
    cursor = StartupConfigureAh53Answers(cursor);
    StartupPlantCountryTables();
    /* GH #35: plant SysVars for INT 21h AH=52h. Most fields are deliberately
     * left zero -- see the handler for why a null stub beats a plausible-looking
     * one. Only fields with a caller that demonstrably reads them are filled:
     * BX-2   the first MCB segment (GH #35)
     * +0x21  LASTDRIVE -- krnl386 takes a pointer to this byte through the
     *        SysVars+0x6A table below, so zero here means it believes there
     *        are no drives at all.
     *
     * [INFO]: s81: SysVars has its OWN segment now (DOS_SYSVARS_SEG, see dos_layout.h), so
     * the whole of it is ours to clear -- the old "+0x40 only, the SDA follows"
     * limit was a symptom of it sharing DOS_HDLR_SEG.
     */
    volatile BYTE *sysVars = (volatile BYTE *)(ULONG_PTR)((DWORD)DOS_SYSVARS_SEG << PARAGRAPH_SHIFT);
    {
        INT item;

        for (item = DOS_SYSVARS_MCB_HEAD; item < DOS_SYSVARS_LEN; ++item)
            sysVars[DOS_SYSVARS_OFF + item] = 0;
    }
    *(volatile WORD *)(sysVars + DOS_SYSVARS_OFF + DOS_SYSVARS_MCB_HEAD) = machine->FirstMcb;
    /* [CAUTION]: SysVars+0x66 = "first MCB in upper memory" (= absolute SEG:0x008C, which MEM
     * also reads directly -- see DOS_UMBHEAD_OFF). 0xFFFF means "none", the truth
     * on a machine that refuses AH=5803. Zero here is what MEM /C walked as a UMB
     * chain starting at segment 0. SysVars+0x68 holds the first MCB again, as it
     * does on 6.22 and PCem (p_sysvar).
     */
    *(volatile WORD *)(sysVars + DOS_UMBHEAD_OFF) = DOS_UMBHEAD_NONE;
    *(volatile WORD *)(sysVars + DOS_SYSVARS_OFF + DOS_SYSVARS_FIRST_MCB_COPY) = machine->FirstMcb;
    sysVars[DOS_SYSVARS_OFF + DOS_SYSVARS_BLOCK_DEVICES] = 1;                      /* block devices */
    sysVars[DOS_SYSVARS_OFF + DOS_SYSVARS_LASTDRIVE] = DOS_LASTDRIVE;          /* LASTDRIVE */
    machine->SysvarsSegment = DOS_SYSVARS_SEG;
    machine->SysvarsOffset = DOS_SYSVARS_OFF;
    StartupBuildDriveTables(report, sysVars, machine);
    /* GH #128: and the WOW extension krnl386 reads before it does anything else. */
    DosWowPublish(handlerArea, (volatile BYTE *)(DOS_CTAB_SEG << PARAGRAPH_SHIFT), DOS_DRIVE_C);
    DosXmsInitialize(&g_Xms, XMS_POOL_KB, XmsHostAllocate, XmsHostFree, NULL);  /* M4: XMS pool */
    /* THE HMA: 64KB-16 AT LINEAR 0x100000, REACHED AS FFFF:0010. (s72) (Importance = 1):
     * p_xms measured us refusing it TWICE over -- AH=00h answered DX=0 ("no HMA")
     * and AH=01h answered BL=0x90 ("HMA does not exist") -- against an oracle that
     * has one. That is an unimplemented FEATURE, not a wrong number.
     * - In this design a guest linear address IS a host virtual address (every
     *   `(seg<<4)+off` deref in this file depends on it), so the HMA is exactly one
     *   committed 64KB page range at 0x100000. Whether NT lets us have that address
     *   in a VDM process is an empirical question, so it is ASKED and LOGGED rather
     *   than assumed: a guest is told DX=1 only if the memory is really there.
     * - No A20 aliasing, and that is a decision already recorded in dos_xms.h: "an
     *   NT VDM does not wrap at 1 MB -- the line is effectively always open". We
     *   model the A20 FLAG (AH=03h..07h) but not the address wrap. A program that
     *   disables A20 and then expects FFFF:0010 to alias 0000:0000 would see the
     *   HMA instead; none of the panel does, and inventing a wrap would mean
     *   remapping views on every A20 toggle.
     */
    /* (the attempt itself is made early, in HmaTry(), and reported in the STAGE0
     * preamble -- ONE buffered flush, which survives the log-handle race that
     * swallowed this line entirely when it was appended separately here.)
     */

    DosEmsInitialize(&g_Ems, (WORD)(g_EmsFrameLinear >> PARAGRAPH_SHIFT), EMS_POOL_PAGES,
             (volatile BYTE *)g_EmsFrameLinear,
             EmsHostAllocate, EmsHostFree, NULL);             /* M4: 8MB EMS pool */
    *cursorIo = cursor;
}

/* Find and load the program: CSRSS's application, cfg\target.txt, the title path, a PIF, XP's COMMAND.COM (#208) or a shell; then count the NTVDM BOP sites an NTVDM-aware shell carries. */
static VOID StartupLoadProgram(
    PSTR *cursorIo,
    DWORD *readCountIo,
    CHAR *programPathBuffer,
    CHAR *args,
    const INT wowCommandFromCsrss)
{
    PSTR cursor = *cursorIo;
    INT wantShell = 0;                 /* s79: our own launcher stub asked for a SHELL (see LAUNCH_STUB_NAME) */
    DWORD readCount = *readCountIo;
    INT wasShell  = 0;                 /* s79: we actually loaded a shell, not a named program */
    UINT index;

    StartupLoadCsrssApplication(&cursor, &wantShell, &readCount, programPathBuffer, args);
    StartupLoadTarget(&cursor, &readCount, wowCommandFromCsrss, wantShell, programPathBuffer, args);
    StartupLoadTitlePath(&cursor, &readCount, programPathBuffer, args);

    if (!readCount && g_CurrentDirectory[0] && g_Title[0])
    {
        CHAR path[768];
        PSTR pathCursor = path;
        HANDLE fileHandle;
        int targetLength;
        PSTR targetArguments = NULL; /* stays int: INT here moves the compiled code */
        pathCursor = LogPut(pathCursor, g_CurrentDirectory); pathCursor = LogPut(pathCursor, HOST_PATH_SEPARATOR); pathCursor = LogPut(pathCursor, g_Title);

        for (targetLength = 0; path[targetLength]; ++targetLength) ;              /* same trailing-space trim as above */

        while (targetLength > 0 && (path[targetLength - 1] == ' ' || path[targetLength - 1] == '\t'))
            path[--targetLength] = 0;

        fileHandle = CsrssOpenSplit(path, &targetArguments);        /* a RELATIVE title carries args too */

        if (fileHandle != INVALID_HANDLE_VALUE)
        {
            ReadFile(fileHandle, g_FileBuffer, sizeof(g_FileBuffer), &readCount, NULL);
            CloseHandle(fileHandle);
        }

        LogPut(programPathBuffer, path);                       /* env argv[0] */

        if (targetArguments)
            LogPut(args, targetArguments);
        else if (g_CommandLine[0])
            LogPut(args, g_CommandLine);                              /* best-effort: CmdLine if CSRSS populated it */

        cursor = LogPut(cursor, "STAGE2: loaded 0x"); cursor = LogHex(cursor, readCount);
        cursor = LogPut(cursor, " from "); cursor = LogPut(cursor, path);

        if (targetArguments)
        {
            cursor = LogPut(cursor, " args=[");
            cursor = LogPut(cursor, targetArguments);
            cursor = LogPut(cursor, "]");
        }

        cursor = LogPut(cursor, "\r\n");
    }

    /* NOTHING NAMED A PROGRAM => RUN A SHELL. (s78) (Importance = 5):
     * The user's question was *"do you actually need to build a shell, or simply load
     * Windows NT's COMMAND.COM when ntvdmex is loaded with no guest EXE?"* -- and the
     * answer is the second one. `COMMAND.COM` IS the shell; it becomes the guest like
     * any other DOS program, and MS-DOS 6.22's copy already works here: banner,
     * prompt, `ver`, and a real `dir` listing with volume serial and free space
     * (measured on the rig, s78).
     *
     * [CAUTION]: STRICTLY BELOW EVERYTHING ELSE, and that placement is the whole design. CSRSS's
     * AppName, `target.txt`, an absolute title and a relative title have all been
     * tried above and all failed. Put this any higher and the headless harness -- which
     * names its program in `target.txt` -- silently runs a shell instead of the test.
     * - WHICH shell is a configuration question, not a guess:
     *   1. `cfg\shell.txt`  -- a path the user chooses. A 6.22 COMMAND.COM goes here.
     *   2. `C:\WINDOWS\SYSTEM32\COMMAND.COM` -- present on every XP box.
     *
     * [CAUTION]: (2) is XP's own, which is NTVDM-aware and stops at `BOP 0x54` -- see
     * docs/inventory/bop.md. That is not a reason to leave it out: it fails with a
     * log line naming exactly what is missing, where the old fallback was a 4-byte
     * `mov ah,4Ch / int 21h` that exited cleanly and said nothing at all.
     *
     * [WARNING]: COMSPEC is deliberately NOT consulted: under a Windows session it names
     * `cmd.exe`, a 32-bit PE that must never be loaded as a DOS guest.
     */
    /* #208: HAND A DOS PROGRAM TO XP's SHELL INSTEAD OF LOADING IT. See g_Routed:
     * Only when all of these hold, and otherwise exactly as before:
     * - a DOS program was found (an MZ/COM image, not NE/PE -- a Win16 or Win32
     *   image under COMMAND.COM just says "requires Microsoft Windows")
     * - this is not a WOW launch, and the program is not itself a COMMAND.COM
     * - the shell would be XP's own (no cfg\shell.txt, no Settings choice -- #203): only that shell asks
     *   BOP 54 sub 01, so only it can be handed a program
     * - its 8.3 path and arguments fit a DOS command line
     * - cfg\directlaunch.flag is absent (the A/B switch back to direct loading)
     */
    /* -- #203: WHICH SHELL, DECIDED ONCE. default (XP's own) < Settings' "DOS prompt"
     * (HKCU DosPrompt) < cfg\shell.txt -- the file wins, as every file knob does, so
     * the harness is never overridden by whatever was last picked in the dialog. The
     * #208 routing below and the shell load after it both read this one answer; they
     * used to test the file separately, which a registry setting would have split.
     */
    CHAR shellConfig[512];
    PCSTR shellSource = 0;
    shellConfig[0] = 0;
    {   HANDLE configHandle = CreateFileA(SHELL_PATH, GENERIC_READ, FILE_SHARE_READ,
                                NULL, OPEN_EXISTING, 0, NULL);

        if (configHandle != INVALID_HANDLE_VALUE)
        {
            DWORD commandLength = 0;
            INT shellLength;
            ReadFile(configHandle, shellConfig, sizeof(shellConfig) - 1, &commandLength, NULL);
            CloseHandle(configHandle);
            shellConfig[commandLength < sizeof(shellConfig) ? commandLength : sizeof(shellConfig) - 1] = 0;
            /* Trim the newline the file almost certainly ends with, and any spaces --
             * the same trap target.txt's reader already documents.
             */
            for (shellLength = 0; shellConfig[shellLength]; ++shellLength) ;

            while (shellLength > 0 && (shellConfig[shellLength-1] == '\r' || shellConfig[shellLength-1] == '\n'
                              || shellConfig[shellLength-1] == ' ' || shellConfig[shellLength-1] == '\t'))
                shellConfig[--shellLength] = 0;

            if (shellLength)
            {
                shellSource = "cfg\\shell.txt";

                if (g_Settings.Strings[SET_STR_SHELL][0])
                    g_ShellOverride = "cfg\\shell.txt";
            }
        }

        if (!shellConfig[0] && g_Settings.Strings[SET_STR_SHELL][0])
        {
            lstrcpynA(shellConfig, g_Settings.Strings[SET_STR_SHELL], sizeof shellConfig);
            shellSource = "Settings > General > DOS prompt";
        }
    }
    StartupApplyPif(&cursor, &readCount, programPathBuffer, args);
    /* #153: File > Open Recent lists every program this host was started with. */
    if (readCount && !g_WowLaunch && programPathBuffer[0])
        MruAdd(programPathBuffer);

    StartupRouteToCommandCom(&cursor, &readCount, shellConfig, programPathBuffer, args);
    StartupLoadShell(&cursor, &readCount, shellConfig, shellSource, programPathBuffer, &wasShell);

    if (!readCount)
    {
        static const BYTE stub[] = { X86_OP_MOV_AH_IMM, DOS_FN_EXIT, X86_OP_INT, VECTOR_DOS };   /* mov ah,4Ch; int 21h */

        for (index = 0; index < sizeof(stub); ++index)
            g_FileBuffer[index] = stub[index];

        readCount = sizeof(stub);
        /* [CAUTION]: SAY WHY, not just that. This printed "embedded fallback" and nothing else,
         * and it is reached when a path was wrong as well as when nothing was named --
         * GH #131 spent a session on a run that looked clean and wrote nothing.
         */
        cursor = LogPut(cursor, "STAGE2: embedded fallback -- nothing named a program AND no shell "
                    "could be opened (cfg\\shell.txt, C:\\WINDOWS\\SYSTEM32\\COMMAND.COM)"
                    "\r\n");
    }

    /* IS THIS GUEST NTVDM-AWARE? ASK THE IMAGE, NOT THE PATH. (s79) (Importance = 3):
     * XP's own COMMAND.COM needs two things no ordinary DOS guest does: it refuses
     * any DOS version but 5.00, and it reads `INT 21h AH=53h`'s private AL
     * sub-functions to decide whether it is an interactive shell at all. Both were
     * `cfg\` knobs, which is the right shape for an experiment and the wrong one for
     * a product -- "double-click NTVDMEX and get a prompt" cannot require two files.
     *
     * The discriminator is a MEASURED PROPERTY OF THE IMAGE, not a filename: an
     * NTVDM-aware guest talks to the 32-bit side through `C4 C4 54 <sub>` BOPs. XP's
     * COMMAND.COM has FIFTEEN of them. A path check would be a guess (a user may put
     * XP's shell in `cfg\shell.txt`, or ours somewhere else); the BOPs are what
     * actually make it NT-aware.
     *
     * [CAUTION]: THE THRESHOLD IS THE GUARD. `C4 C4` is a legal, if odd, instruction pair, so
     * one or two sites prove nothing -- a false positive would silently change the
     * DOS version reported to an innocent guest. Requiring EIGHT distinct sites is
     * far beyond coincidence and still well under XP's fifteen, so a future build of
     * the shell with a few fewer would still be recognised. The count is logged, so
     * a guest that lands near the line says so instead of being decided silently.
     *
     * [CAUTION]: It is only consulted for a program we loaded as THE SHELL. A DOS game that
     * somehow tripped the count must not be told it is running on DOS 5.
     */
    g_GuestNtvdmBops = 0;

    if (readCount > VDM_BOP_SUBFUNCTION_LENGTH)
    {
        DWORD item;

        for (item = 0; item + VDM_BOP_LENGTH < readCount; ++item)
            if (g_FileBuffer[item] == VDM_BOP0 && g_FileBuffer[item+1] == VDM_BOP1 && g_FileBuffer[item+2] == NTVDM_BOP_CMD)
                ++g_GuestNtvdmBops;
    }

    g_GuestNtAware = (wasShell && g_GuestNtvdmBops >= NT_AWARE_SHELL_BOPS_MIN);
    /* #152: Close Program has nothing to close at a shell's own prompt -- whether we
     * chose the shell or something named COMMAND.COM explicitly.
     */
    {   INT pathLength = lstrlenA(programPathBuffer);
        g_TopIsShell = wasShell
            || (pathLength >= COMMAND_COM_LENGTH && !lstrcmpiA(programPathBuffer + pathLength - COMMAND_COM_LENGTH, HOST_COMMAND_COM)); }
    /* THE NTVDM-AWARE SHELL IS LAUNCHED `/P <its own directory>`, AS STOCK DOES (Importance = 1):
     * ntvdm.exe carries `%s=%s%s /p %s\system32`; s79 found /P mattered and the bare
     * launch later dropped every argument. Without /P, PERMCOM ([0x2B0]) stays 0, so
     * XP's EXIT takes DOS's ordinary return-to-parent path -- which for a top-level
     * shell is ITSELF, and the prompt just comes back (the sweep's "exit does not
     * work"). The directory argument is COMMAND.COM's own COMSPEC location, which
     * also replaces the C:\COMMAND.COM the environment otherwise names.
     */
    if (g_GuestNtAware && !args[0])
    {
        CHAR directory[300];
        CHAR shortDirectory[300];
        INT directoryLength = 0;
        INT cut = 0;

        for (directoryLength = 0; programPathBuffer[directoryLength] && directoryLength < (INT)sizeof directory - 1; ++directoryLength)
        {
            directory[directoryLength] = programPathBuffer[directoryLength];

            if (programPathBuffer[directoryLength] == '\\')
                cut = directoryLength; }

        directory[cut ? cut : directoryLength] = 0;

        if (!GetShortPathNameA(directory, shortDirectory, sizeof shortDirectory))
            LogPut(shortDirectory, directory);

        wsprintfA(args, HOST_SHELL_ARGUMENTS_FORMAT, shortDirectory);

        if (!GetShortPathNameA(programPathBuffer, g_ShellPath, sizeof g_ShellPath))
            LogPut(g_ShellPath, programPathBuffer);

        cursor = LogPut(cursor, "STAGE2: NTVDM-aware shell -> command tail [");
        cursor = LogPut(cursor, args); cursor = LogPut(cursor, "] (permanent, as stock launches it)\r\n");
    }

    cursor = LogPut(cursor, "STAGE2: guest NTVDM BOP sites (C4 C4 54) = ");
    cursor = LogDecimal(cursor, g_GuestNtvdmBops);
    cursor = LogPut(cursor, g_GuestNtAware
             ? " -> NTVDM-AWARE SHELL: DOS 5.00 and the private AH=53h answers apply\r\n"
             : (wasShell ? " -> an ordinary DOS shell\r\n" : " (not loaded as a shell)\r\n"));

    /* status-bar program name = basename of programPathBuffer (if any) */
    { PCSTR baseName = programPathBuffer, scan;
    INT item = 0;

      for (scan = programPathBuffer; *scan; ++scan)
          if (*scan == '\\' || *scan == '/')
              baseName = scan + 1;

      if (*baseName)
      {
          while (baseName[item] && item < PROGRAM_NAME_SIZE - 1)
          {
              g_ProgramName[item] = baseName[item];
              ++item;
          }

          g_ProgramName[item] = 0;
      }
      }
    *cursorIo = cursor;
    *readCountIo = readCount;
}

/* Become the VDM: set up its memory, register with the kernel's VDM support, give [0x714] a defined value, fetch the command from CSRSS (DOS or Win16), and take the VDM_TIB -- or stop if there is none. */
static INT StartupRegisterVdm(
    PSTR *cursorIo,
    LONG *vdmStatusIo,
    INT *wowCommandFromCsrssIo,
    CHAR *args,
    CHAR *programPathBuffer,
    DWORD *readCountIo,
    CHAR *report,
    volatile BYTE * *tibIo,
    INT *exitCodeOut)
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
    /* THE CLEAN 2x2:
     * The first differential compared a WOW probe HERE against a DOS probe placed
     * ~500 lines later, after CSRSS and the whole DOS machine were built. That is two
     * variables, not one, so "WOW is refused, DOS succeeds" did not actually follow.
     * Probe BOTH launch types at BOTH points and let the 2x2 say whether it is the
     * launch type or the amount of VDM setup that matters.
     */
    if (GetFileAttributesA(WOWTRY_FLAG) != INVALID_FILE_ATTRIBUTES)
        WowProbeLdtMatrix(g_WowModuleCount ? "wow-early" : "dos-early");

    StartupWowSelectorStage();
    /* EMS page frame must be mapped AFTER VdmInitialize (see VdmMapEmsFrame). */
    g_EmsFrameLinear = VdmMapEmsFrame();

    if (GetFileAttributesA(WOWTRY_FLAG) != INVALID_FILE_ATTRIBUTES)
        WowProbeLdtMatrix("B-after-emsframe");

    cursor = LogPut(cursor, "STAGE1: ems_frame lin=0x"); cursor = LogHex(cursor, g_EmsFrameLinear);
    cursor = LogPut(cursor, " seg=0x"); cursor = LogHex(cursor, g_EmsFrameLinear >> PARAGRAPH_SHIFT); cursor = LogPut(cursor, "\r\n");

    /* CSRSS: register as the console VDM, then fetch the program to run. */
    CsrssRegisterConsole();

    if (GetFileAttributesA(WOWTRY_FLAG) != INVALID_FILE_ATTRIBUTES)
        WowProbeLdtMatrix("C-after-csrss-register");

    StartupTakeCsrssCommand(&cursor, &error);
    /* THE SECOND FETCH: THE COMMAND ITSELF. (s72, the package smoke test) (Importance = 5):
     * The call above is stock ntvdm's `cmdGetStartInfo` shape (VDM_GET_FIRST_COMMAND):
     * it fills Title, CurDirectory and the PIF, and nothing else -- AppName/CmdLine
     * come back as capture-buffer scaffolding (`app=[5??] cmd=[\]`). Explorer puts the
     * program's path in the console TITLE, which is the only reason a double-click has
     * ever run the right program. A launch from cmd.exe or a batch file -- smoke.bat,
     * a prompt, the friend's machine on the 18th -- has a title of "" (start) or the
     * typed command WITH ITS ARGUMENTS (direct), and we ran the embedded four-byte stub
     * and reported a clean exit: the package smoke test passed without running the
     * self-test. Stock ntvdm consumes the real command in its exec-BOP path with a
     * second GetNextVDMCommand, VDM_FLAG_DOS | VDM_FLAG_FIRST_TASK.
     * - MEASURED on the rig, both launch shapes (STAGE1: fetch2 lines, s72):
     *   AppName = C:\DOCUME~1\...\bm\selftest.com   (full short path, AppLen incl. NUL)
     *   CmdLine = "hello world\r\n"                     (the tail ONLY; "\r\n" when none)
     *   CurDirectory = the launcher's cwd; Env = its Win32 environment block (0x46c);
     *   ComingFromBat = 1 from a batch file; TaskId 0 without -i is fine.
     *   DONT_WAIT so a protocol misunderstanding is a FALSE with an error, never a hang.
     *   Only after a successful first fetch: on a WOW launch the first returns FALSE
     *   (err 0x57) and that tell is left exactly as it was.
     */
    if (g_CurrentDirectory[0] || g_Title[0])
    {
        cursor = StartupFetchCommandDetails(cursor);
    }
    else if (g_WowLaunch)
    {
        StartupFetchWowCommand(&cursor, &wowCommandFromCsrss, args, programPathBuffer, &readCount);
    }

    LogWrite(LOG_PATH, report, cursor);
    /* [CAUTION]: AFTER the LogWrite, not before: LogWrite TRUNCATES. The first cut of this
     * ran the probe earlier and its output was silently erased by this very line,
     * which looked exactly like "the probe never ran". Same stale/truncated-artefact
     * trap this project keeps paying for, in a new costume.
     */
    if (GetFileAttributesA(WOWTRY_FLAG) != INVALID_FILE_ATTRIBUTES)
        WowProbeLdtMatrix("D-after-getcommand");   /* before VdmGetTib */

    tib = VdmGetTib();

    if (GetFileAttributesA(WOWTRY_FLAG) != INVALID_FILE_ATTRIBUTES)
        WowProbeLdtMatrix("E-after-get-tib");

    g_TibDebug = tib;                                    /* let the crash VEH dump guest state */

    if (!tib)
    {
        cursor = LogPut(cursor, "STAGE1: no VDM_TIB -- abort\r\n"); LogAppend(LOG_PATH, report, cursor);
        {
            *cursorIo = cursor;
            *vdmStatusIo = vdmStatus;
            *wowCommandFromCsrssIo = wowCommandFromCsrss;
            *readCountIo = readCount;
            *tibIo = tib;
            *exitCodeOut = 1;
            return HOST_FLOW_RETURN;
        }
    }

    *cursorIo = cursor;
    *vdmStatusIo = vdmStatus;
    *wowCommandFromCsrssIo = wowCommandFromCsrss;
    *readCountIo = readCount;
    *tibIo = tib;
    return HOST_FLOW_NEXT;
}

/* Start the log and its COM1 mirror, take stdio, run the recovery counter (safe mode after repeated failed starts), read Settings and the cfg\ knobs, install the fault handlers, and log the STAGE0 state. */
static INT StartupConfigure(PSTR *cursorIo, CHAR *report, INT *exitCodeOut)
{
    PSTR cursor = *cursorIo;

    /* NO MODAL HARDWARE-ERROR BOXES, EVER, FOR THE WHOLE PROCESS:
     * Every Win32 call that touches a drive with no media -- A: with the door
     * open, an ejected CD -- raises XP's "There is no disk in drive" box unless
     * told not to, and that box has wedged the rig from inside host start-up
     * once already. Now that the guest can select and search those drives
     * (INT 21h AH=0Eh, 47h, 4Eh...), the mode must cover every call, not just
     * the two sites that wrapped it. The errors still come back as errors.
     */
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

        if (g_StartMode == DOS_START_UNINSTALL)
            RecoveryUninstall(&cursor);

        g_Safe = DosRecoveryGetSafeSkips(g_StartMode);

        if (g_StartMode == DOS_START_SAFE)
            cursor = LogPut(cursor, "STAGE0: SAFE MODE skips: third-party VDDs, audio output (silent"
                        " pump), the real PC speaker, the joystick thread, the WOW"
                        " shims, fullscreen -- the next clean exit clears it\r\n");

        /* s90: NOT `p = report` -- the next LogWrite TRUNCATES the file and re-writes
         * the report buffer, so a line dropped from the buffer here was lost from
         * EVERY log: "consecutive failed starts" never survived since #132 landed.
         */
        LogAppend(LOG_PATH, report, cursor);
        SerialOut(report, cursor); }
    SerialOut(report, cursor);

    {
        INT exitCode;
        INT flow = StartupLatchWowLaunch(&cursor, report, &exitCode);

        if (flow == HOST_FLOW_RETURN)
        {
            *cursorIo = cursor;
            *exitCodeOut = exitCode;
            return HOST_FLOW_RETURN;
        }
    }
    /* Headless test mode = the SMB watcher dropped the AUTOEXIT marker. In that mode the
     * host must self-exit on guest exit AND bound any infinite run (a visual demo like
     * pm32irq/animate never calls INT 21h 4Ch), else rt.bat's `start /wait` blocks forever
     * and wedges the watcher (session-9). Latch it once here (the exit path deletes the marker).
     */
    /* SETTINGS FIRST, TEST FILES SECOND:
     * Load the stored configuration here, at the TOP of the knob block, so every
     * file knob below it still overrides. That ordering is the whole contract (see
     * settings.h): the rig configures this host by writing files and re-launching,
     * and a setting clicked in a dialog on that machine must never silently change
     * what a headless measurement is measuring.
     */
    SettingsLoad(&g_Settings);
    g_SettingsDisk = g_Settings;          /* nothing has overridden anything yet */
    SettingsApply(NULL, &g_Settings, SETTINGS_APPLY_STARTUP);
    /* Log only the LIVE settings -- the ones the SettingsApply* functions actually
     * push into the machine. The stored-but-not-yet-honoured ones would make this
     * line four times longer and every value in it would be a claim the run cannot
     * support. Two lines because there are now enough of them to wrap.
     */
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
    /* THE TIMING LANDSCAPE, IN EVERY LOG. (s63) (Importance = 3):
     * Skyroads' frame pacing is fragile on a 2-core box and has regressed THREE
     * times, each time because a change quietly added background work or moved a
     * priority and nobody re-checked. So make the invariants AUDITABLE: the pacer
     * priority MUST read 0x0 (THREAD_PRIORITY_NORMAL); the s61 regression was it
     * sitting at HIGHEST (0x2), which let the pacer preempt the guest. joy_thread
     * MUST read 0x0 whenever JoystickType is None, because a non-joystick game
     * must run with no poll thread at all. A regression in either now shows up in
     * the first twenty lines of every run, not two months later on a user's
     * screen. (Priority constants: NORMAL=0, ABOVE_NORMAL=1, HIGHEST=2,
     * BELOW_NORMAL=-1=0xffffffff, LOWEST=-2.)
     */
    cursor = LogPut(cursor, "STAGE0: timing: pacer_prio="); cursor = LogHex(cursor, (DWORD)g_PitPacePriority);
    cursor = LogPut(cursor, " (want 0x0=NORMAL) joytype=");  cursor = LogHex(cursor, (DWORD)g_Joystick.Type);
    cursor = LogPut(cursor, " joy_thread=");  cursor = LogHex(cursor, (DWORD)g_JoystickThreadStarted);
    cursor = LogPut(cursor, " (want 0x0 when joytype=0x0) pit_split=1\r\n");

    g_Headless = (GetFileAttributesA(AUTOEXIT_PATH) != INVALID_FILE_ATTRIBUTES);
    /* Self-screenshot only when explicitly requested (graphical tests) AND headless, so
     * the common non-graphical tests never enter the capture path. Latched once here.
     */
    { HANDLE modeHandle = CreateFileA(MODEY_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL, OPEN_EXISTING, 0, NULL);

      if (modeHandle != INVALID_HANDLE_VALUE)
      {
          CHAR modeYText[32];
          DWORD modeYRead = 0;
          DWORD modeYValue = 0;
          DWORD modeYIndex;
          INT got = 0;
          ReadFile(modeHandle, modeYText, sizeof modeYText - 1, &modeYRead, NULL);
          CloseHandle(modeHandle);

          for (modeYIndex = 0; modeYIndex < modeYRead && modeYText[modeYIndex] >= '0' && modeYText[modeYIndex] <= '9'; ++modeYIndex)
          {
              modeYValue = modeYValue * DECIMAL_RADIX + (DWORD)(modeYText[modeYIndex] - '0');
              got = 1;
          }

          if (got && modeYValue <= MODEY_GAP_MAX_U)
          {
              CHAR dwordsLine[96];
              CHAR *lineCursor = dwordsLine;
              g_Video.ModeYGap = modeYValue;
              lineCursor = LogPut(lineCursor, "STAGE0: modey.txt -> gap="); lineCursor = LogHex(lineCursor, modeYValue);
              lineCursor = LogPut(lineCursor, " dwords\r\n"); LogAppend(LOG_PATH, dwordsLine, lineCursor); SerialOut(dwordsLine, lineCursor);
          }
      } }

    g_Capture  = g_Headless && (GetFileAttributesA(CAPTURE_FLAG) != INVALID_FILE_ATTRIBUTES);

    if (g_Capture)                         /* its contents, if any, are the period in ms */
    {
        HANDLE configHandle = CreateFileA(CAPTURE_FLAG, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                NULL, OPEN_EXISTING, 0, NULL);

        if (configHandle != INVALID_HANDLE_VALUE)
        {
            CHAR captureText[32];
            DWORD captureRead = 0;
            DWORD captureValue = 0;
            DWORD captureIndex;
            ReadFile(configHandle, captureText, sizeof captureText - 1, &captureRead, NULL);
            CloseHandle(configHandle);

            for (captureIndex = 0; captureIndex < captureRead && captureText[captureIndex] >= '0' && captureText[captureIndex] <= '9'; ++captureIndex)
                captureValue = captureValue * DECIMAL_RADIX + (DWORD)(captureText[captureIndex] - '0');

            if (captureValue >= CAPTURE_MS_MIN && captureValue <= CAPTURE_MS_MAX)
                g_CaptureMs = captureValue;

            {   DWORD periodDelay = 0;                                 /* #58: "period delay" */

                while (captureIndex < captureRead && captureText[captureIndex] == ' ')
                    ++captureIndex;

                for (; captureIndex < captureRead && captureText[captureIndex] >= '0' && captureText[captureIndex] <= '9'; ++captureIndex)
                    periodDelay = periodDelay * DECIMAL_RADIX + (DWORD)(captureText[captureIndex] - '0');

                if (periodDelay <= CAPTURE_DELAY_MS_MAX)
                    g_CaptureDelayMs = periodDelay; }

            g_CaptureStart = GetTickCount();
        }
    }

    /* [CAUTION]: THESE BELONG WITH THE OTHER STARTUP FLAGS, NOT IN THE DPMI BLOCK. Read from
     * inside the protected-mode setup they applied to Doom and not to QBasic --
     * nomouse worked for one guest and silently did nothing for the other, and
     * textdump wrote no files at all for a real-mode run. A knob that only some
     * launches honour is worse than no knob.
     */
    g_TextDump = (GetFileAttributesA(TEXTDUMP_PATH) != INVALID_FILE_ATTRIBUTES);
    g_MouseAbsent = (GetFileAttributesA(NOMOUSE_PATH) != INVALID_FILE_ATTRIBUTES);
    g_NoA000  = (GetFileAttributesA(NOA000_FLAG) != INVALID_FILE_ATTRIBUTES);
    g_NoPmPatch = (GetFileAttributesA(NOPMPATCH_FLAG) != INVALID_FILE_ATTRIBUTES);

    if (g_NoPmPatch)                      /* contents, if any, = minimum region size */
    {
        HANDLE noPatchHandle = CreateFileA(NOPMPATCH_FLAG, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                NULL, OPEN_EXISTING, 0, NULL);

        if (noPatchHandle != INVALID_HANDLE_VALUE)
        {
            CHAR noPatchText[32];
            DWORD noPatchBytesRead = 0;
            DWORD noPatchValue = 0;
            DWORD noPatchIndex;
            ReadFile(noPatchHandle, noPatchText, sizeof noPatchText - 1, &noPatchBytesRead, NULL);
            CloseHandle(noPatchHandle);

            for (noPatchIndex = 0; noPatchIndex < noPatchBytesRead; ++noPatchIndex)
            {
                INT hexDigit = (noPatchText[noPatchIndex] >= '0' && noPatchText[noPatchIndex] <= '9') ? noPatchText[noPatchIndex] - '0'
                       : (noPatchText[noPatchIndex] >= 'a' && noPatchText[noPatchIndex] <= 'f') ? noPatchText[noPatchIndex] - 'a' + HEX_DIGIT_A_VALUE
                       : (noPatchText[noPatchIndex] >= 'A' && noPatchText[noPatchIndex] <= 'F') ? noPatchText[noPatchIndex] - 'A' + HEX_DIGIT_A_VALUE : -1;

                if (hexDigit < 0)
                    break;

                noPatchValue = (noPatchValue << NIBBLE_SHIFT) | (DWORD)hexDigit;
            }

            g_NoPmPatchMinimum = noPatchValue;
        }
    }

    {   HANDLE modeHandle = CreateFileA(MEMDUMP_FLAG, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                NULL, OPEN_EXISTING, 0, NULL);

        if (modeHandle != INVALID_HANDLE_VALUE)           /* "<linear> <size>" in hex */
        {
            CHAR memoryDumpText[48];
            DWORD memoryDumpBytesRead = 0, memoryDumpIndex, values[MEMDUMP_FIELDS] = {0, 0}, width = 0;
            INT isIn = 0;
            ReadFile(modeHandle, memoryDumpText, sizeof memoryDumpText - 1, &memoryDumpBytesRead, NULL);
            CloseHandle(modeHandle);

            for (memoryDumpIndex = 0; memoryDumpIndex < memoryDumpBytesRead && width < MEMDUMP_FIELDS; ++memoryDumpIndex)
            {
                INT hexDigit = (memoryDumpText[memoryDumpIndex] >= '0' && memoryDumpText[memoryDumpIndex] <= '9') ? memoryDumpText[memoryDumpIndex] - '0'
                       : (memoryDumpText[memoryDumpIndex] >= 'a' && memoryDumpText[memoryDumpIndex] <= 'f') ? memoryDumpText[memoryDumpIndex] - 'a' + HEX_DIGIT_A_VALUE
                       : (memoryDumpText[memoryDumpIndex] >= 'A' && memoryDumpText[memoryDumpIndex] <= 'F') ? memoryDumpText[memoryDumpIndex] - 'A' + HEX_DIGIT_A_VALUE : -1;

                if (hexDigit < 0)
                {
                    if (isIn)
                    {
                        ++width;
                        isIn = 0;
                    }

                    continue;
                }

                values[width] = (values[width] << NIBBLE_SHIFT) | (DWORD)hexDigit;
                isIn = 1;
            }

            g_MemoryDumpLinear = values[0];
            g_MemoryDumpLength = values[1];
        }
    }
    g_Interp12 = (GetFileAttributesA(INTERP12_FLAG) != INVALID_FILE_ATTRIBUTES);
    g_ModeYInterpOffset = (GetFileAttributesA(MYINTERP_OFF_FLAG) != INVALID_FILE_ATTRIBUTES);
    g_ModeYRingOn    = (GetFileAttributesA(MYRING_FLAG) != INVALID_FILE_ATTRIBUTES);
    /* The GUS is decided HERE, with its resources, because the environment block is
     * built before the devices are added -- and ULTRASND= has to say what the card
     * will be. Deciding it at device setup left the first heaven7 run with no ULTRASND
     * and a card nothing looked for.
     */
    g_GusOn = g_Settings.Values[SET_GUS] && (GetFileAttributesA(NOGUS_FLAG) == INVALID_FILE_ATTRIBUTES);

    if (g_Settings.Values[SET_GUS] && !g_GusOn)
        SettingsNoteOverride(SET_GUS, CFG_TEXT(KNOB_FILE_NOGUS), 0);

    if (GetFileAttributesA(DDRAWFS_FLAG) != INVALID_FILE_ATTRIBUTES)   /* read again at fullscreen */
        SettingsNoteOverride(SET_RENDERER, CFG_TEXT(KNOB_FILE_DDRAWFS), 1);

    /* #235: the card as the Audio page's jumpers set it (defaults = the card as built). */
    {   static const BYTE gusIrqs[7] = { 2, 3, 5, 7, 11, 12, 15 };
        static const BYTE gusDmaChannels[5] = { 1, 3, 5, 6, 7 };
        g_Gus.BasePort   = (WORD)(GUS_BASE_FIRST + GUS_BASE_STEP * (g_Settings.Values[SET_GUSADDR] <= GUS_BASE_LAST_CHOICE ? g_Settings.Values[SET_GUSADDR] : GUS_DEFAULT_BASE_CHOICE));
        g_Gus.Irq    = gusIrqs[g_Settings.Values[SET_GUSIRQ] <= ARRAYSIZE(gusIrqs) - 1 ? g_Settings.Values[SET_GUSIRQ] : GUS_DEFAULT_IRQ_CHOICE];
        g_Gus.DmaChannel = gusDmaChannels[g_Settings.Values[SET_GUSDMA] <= ARRAYSIZE(gusDmaChannels) - 1 ? g_Settings.Values[SET_GUSDMA] : GUS_DEFAULT_DMA_CHOICE]; }
    /* ...and OFF THE SOUND BLASTER'S RESOURCES. The SB's own choices in the dialog
     * include 240h, IRQ 11 and DMA 3 -- each of them the GUS default -- and two cards on
     * one line is a machine nobody could have built. Step aside to the next period
     * choice (ref/gus.md section 5 lists what the latches can select).
     */
    if (g_SbConfig.IoBase == g_Gus.BasePort)
        g_Gus.BasePort = GUS_FALLBACK_BASE;

    if (g_SbConfig.Irq == g_Gus.Irq)
        g_Gus.Irq = GUS_FALLBACK_IRQ;

    if (g_SbConfig.Dma8Channel == g_Gus.DmaChannel || g_SbConfig.Dma16Channel == g_Gus.DmaChannel)
        g_Gus.DmaChannel = GUS_FALLBACK_DMA;

    if (g_SbConfig.Dma8Channel == g_Gus.DmaChannel || g_SbConfig.Dma16Channel == g_Gus.DmaChannel)
        g_Gus.DmaChannel = GUS_SECOND_FALLBACK_DMA;

    g_ModeYPmOffset     = (GetFileAttributesA(MYPM_OFF_FLAG) != INVALID_FILE_ATTRIBUTES);
    g_ModeYPmDetect  = (GetFileAttributesA(MYPM_DETECT_FLAG) != INVALID_FILE_ATTRIBUTES);
    g_P12Offset  = (GetFileAttributesA(P12OFF_FLAG)   != INVALID_FILE_ATTRIBUTES);
    g_OplTraceOn = (GetFileAttributesA(OPLTRACE_FLAG) != INVALID_FILE_ATTRIBUTES);

    if (g_OplTraceOn)
        g_Opl.Trace = OplTraceWrite;

    if (g_Interp12)
        g_NoA000 = 1;                          /* interpreting instead of trapping */

    /* Headless cap override (decimal ms on the share). Read before the deadline thread
     * starts, since that thread sleeps on it. Clamped: below the default a typo would
     * kill runs early, above 10 min a typo would wedge the watcher for the whole time.
     */
    { HANDLE handle = CreateFileA(HEADLESS_MS_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             NULL, OPEN_EXISTING, 0, NULL);

      if (handle != INVALID_HANDLE_VALUE)
      {
          CHAR text[16];
          DWORD bytesRead = 0;
          DWORD number = 0;
          INT index;
          ReadFile(handle, text, sizeof text, &bytesRead, NULL);
          CloseHandle(handle);

          for (index = 0; index < (INT)bytesRead; ++index)
          {
              if (text[index] < '0' || text[index] > '9')
                  break;                                              /* stop at CR/LF/junk */

              number = number * DECIMAL_RADIX + (DWORD)(text[index] - '0');
          }

          if (number > PM_HEADLESS_MS_DEFAULT && number <= HEADLESS_MS_MAX)
              g_HeadlessMs = number;                                                                 /* s84: an hour, for slow-rung timedemos */
      } }

    StartupLoadQiMode();

    if (g_QiBits || g_QiSuspended)      /* async delivery needs a handle to the exec thread */
    {
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                        &g_HostCpu, 0, FALSE, DUPLICATE_SAME_ACCESS);
    }

    /* THE GUEST RAN AT NORMAL PRIORITY AGAINST A TIME_CRITICAL AUDIO THREAD:
     * audio_wave.c raises its pump to THREAD_PRIORITY_TIME_CRITICAL because refilling
     * waveOut is a hard deadline. Nothing ever raised the thread that RUNS THE GUEST,
     * so on this single-core box the mixer thread preempts guest code whenever it has
     * work -- and Doom's DMX mixer is guest code that must finish inside one 7.4 ms
     * timer tick or its scheduler abandons the pass, recomputes the deadline from NOW,
     * and the block it would have filled replays the previous ring lap instead.
     * Measured: the mixer NEVER runs on consecutive ticks (2.6% of gaps are one tick,
     * 49% two, 45% three or four) although we deliver 135 ticks/s against a 140 Hz
     * reload -- so it is overrunning, not starved of ticks. And it is not lock
     * contention: slicing HostAudioFill's hold into 64-frame pieces moved
     * REPLAYED_LOUD by 2 blocks in 894. Preemption is what slicing cannot touch.
     * - ABOVE_NORMAL, not higher. The audio pump stays at 15 so it still wins every
     *   race it needs to -- starving it is what "a periodic tick or pulse in otherwise
     *   correct music" was, and that is a worse fault than the one being fixed. This
     *   only lifts the guest above the UI thread and the system's background work.
     *
     * [CAUTION]: Knob, because it is a scheduling change on a box whose behaviour we have been
     * wrong about before: execprio.txt absent or 1 = ABOVE_NORMAL (default),
     * 0 = leave at NORMAL (the old behaviour, for an A/B without a rebuild),
     * 2 = HIGHEST.
     */
    /* -- WHICH DSP VERSION WE CLAIM PICKS THE GUEST'S DRIVER PATH. See vdd_sb.h.
     * dspver.txt holds "major minor" as two decimal numbers, e.g. "2 1" for a
     * Sound Blaster 2.01, which makes DMX skip the mixer-0x82 interrupt gate and
     * use the older 0x48/0x1C auto-init pair instead of the SB16 0xC6 command.
     * Absent = 4.05, i.e. no change.
     */
    { HANDLE versionHandle = CreateFileA(DSPVER_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL, OPEN_EXISTING, 0, NULL);

      if (versionHandle != INVALID_HANDLE_VALUE)
      {
          CHAR text[16];
          DWORD bytesRead = 0;
          INT index = 0;
          INT major = 0;
          INT minor = 0;
          ReadFile(versionHandle, text, sizeof text, &bytesRead, NULL);
          CloseHandle(versionHandle);

          while (index < (INT)bytesRead && text[index] >= '0' && text[index] <= '9')
              major = major * DECIMAL_RADIX + (text[index++] - '0');

          while (index < (INT)bytesRead && (text[index] == ' ' || text[index] == '.'))
              ++index;

          while (index < (INT)bytesRead && text[index] >= '0' && text[index] <= '9')
              minor = minor * DECIMAL_RADIX + (text[index++] - '0');

          if (major > 0 && major < BYTE_VALUES) { g_SbVersionMajor = (BYTE)major;
          g_SbVersionMinor = (BYTE)minor;
                                    g_DspVersionForced = 1; }
      } }

    { HANDLE gateHandle = CreateFileA(SBGATE_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL, OPEN_EXISTING, 0, NULL);

      if (gateHandle != INVALID_HANDLE_VALUE)
      {
          CHAR text[8];
          DWORD bytesRead = 0;
          ReadFile(gateHandle, text, sizeof text, &bytesRead, NULL);
          CloseHandle(gateHandle);
          g_SbGate = (bytesRead && text[0] >= '0' && text[0] <= '9') ? (text[0] - '0') : 1;
      } }

    { DWORD priority = EXECPRIO_ABOVE_NORMAL;
      HANDLE priorityHandle = CreateFileA(EXECPRIO_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL, OPEN_EXISTING, 0, NULL);

      if (priorityHandle != INVALID_HANDLE_VALUE)
      {
          CHAR text[8];
          DWORD bytesRead = 0;
          ReadFile(priorityHandle, text, sizeof text, &bytesRead, NULL);
          CloseHandle(priorityHandle);

          if (bytesRead && text[0] >= '0' && text[0] <= '9')
              priority = (DWORD)(text[0] - '0');
      }

      g_ExecPriority = priority;

      if (priority == EXECPRIO_ABOVE_NORMAL)
          SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
      else if (priority >= EXECPRIO_HIGHEST)
          SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

      g_ExecPriorityForeground = GetThreadPriority(GetCurrentThread());
      DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                      &g_ExecThread, 0, FALSE, DUPLICATE_SAME_ACCESS);   /* #211: BackgroundPriorityTick */
    }
    HostProfileStart();                                 /* #183: cfg\hostprof.flag */

    if (g_QiBits)
    {
        /* Experiment mode: retarget the kernel's PIC so a KERNEL-dispatched IRQ 5 arrives
         * as INT 65h while our own injection still arrives as INT 0Dh. Without this the
         * two are the same vector and the qirq2 probe cannot attribute a delivery.
         */
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

    if (g_PfnAddVeh)
        g_PfnAddVeh(1, DpmiCrashVeh);               /* DPMI spike crash diagnostic; XP+ */

    SetUnhandledExceptionFilter(HostUnhandledFilter); /* real-mode runs: full dump, not WER */

    /* CSRSS command-info: receive buffers + first-command state + IFEO task id. */
    g_CommandInfo.CmdLine = g_CommandLine;
    g_CommandInfo.CmdLen = sizeof(g_CommandLine);
    g_CommandInfo.AppName = g_Application;
    g_CommandInfo.AppLen = sizeof(g_Application);
    g_CommandInfo.PifFile = g_PifPath;
    g_CommandInfo.PifLen = sizeof(g_PifPath);
    g_CommandInfo.CurDirectory = g_CurrentDirectory;
    g_CommandInfo.CurDirectoryLen = sizeof(g_CurrentDirectory);
    g_CommandInfo.Env = g_Environment;
    g_CommandInfo.EnvLen = sizeof(g_Environment);
    g_CommandInfo.Desktop = g_Desktop;
    g_CommandInfo.DesktopLen = sizeof(g_Desktop);
    g_CommandInfo.Title = g_Title;
    g_CommandInfo.TitleLen = sizeof(g_Title);
    g_CommandInfo.Reserved = g_Reserved;
    g_CommandInfo.ReservedLen = sizeof(g_Reserved);
    g_CommandInfo.StartupInfo.cb = sizeof(STARTUPINFOA);
    g_CommandInfo.VDMState = VDM_GET_FIRST_COMMAND;
    g_CommandInfo.TaskId   = CsrssParseTaskId(GetCommandLineA());

    /* WHAT SHAPE OF LAUNCH IS THIS? (GH #129):
     * Windows launches ntvdm.exe for BOTH a DOS program and a 16-bit WINDOWS
     * program -- WOW runs inside the same VDM binary. Our IFEO Debugger hook
     * therefore intercepts both, and we implement only the DOS half, so a Win16
     * launch currently lands in a host that cannot load an NE file at all.
     * - Before deciding anything from the command line, RECORD IT. The flags that
     *   distinguish the two are described in various places and this project has
     *   been bitten repeatedly by building on a documented claim instead of a
     *   measured one. Log the raw string; diff a DOS launch against a Win16 launch
     *   on the rig; write the detector against what the diff actually shows.
     */
    cursor = LogPut(cursor, "STAGE0: root=["); cursor = LogPut(cursor, NTVDMEX_DIR); cursor = LogPut(cursor, "] (derived from the host's own path)\r\n");
    HmaTry();
    cursor = LogPut(cursor, "STAGE0: HMA ");

    if (g_Hma)
        cursor = LogPut(cursor, "committed at 0x100000 -- FFFF:0010 is real");
    else { cursor = LogPut(cursor, "UNAVAILABLE err=0x"); cursor = LogHex(cursor, g_HmaError);
       cursor = LogPut(cursor, " state=0x"); cursor = LogHex(cursor, g_HmaState);
       cursor = LogPut(cursor, " prot=0x");
       cursor = LogHex(cursor, g_HmaProtection); }

    cursor = LogPut(cursor, "\r\n");
        cursor = LogPut(cursor, "STAGE0: cmdline=["); cursor = LogPut(cursor, GetCommandLineA()); cursor = LogPut(cursor, "]\r\n");
    *cursorIo = cursor;
    return HOST_FLOW_NEXT;
}
