/* host_diag.c -- crash handling and diagnostics: the fatal dump, the PM-fault handler, the
 *   watchdog, the end-of-run reports and the probe loaders.
 *
 * Its own translation unit (#335): declared in host_diag.h. */
#include "host_state.h"
#include "log.h"
#include "ne.h"
#include "wow32.h"
#include "wowanchors.h"
#include "wowsched.h"
#include "wowcall.h"
#include "wowmsg.h"
#include "wowres.h"
#include "wowwin.h"
#include "wowgdi.h"
#include "wowuser.h"
#include "host_diag.h"
#include "main.h"
#include "host_dpmi_int.h"
#include "host_bios.h"
#include "host_dpmi.h"
#include "host_irq.h"
#include "host_video.h"
#include "host_window.h"


/* ── ★ THE WATCHDOG WRITES HERE, AND NOWHERE ELSE. ────────────────────────────────
     It is the one instrument whose whole job is to speak when the main thread cannot,
     so sharing a file with the main thread's firehose leaves a shared-resource
     explanation alive for every silence it reports. On its own path, an empty file
     means the THREAD did not run and a populated one means it did -- which is the
     distinction session 32 could not make and spent four rig runs failing to settle.
     LogAppend opens with FILE_SHARE_READ|FILE_SHARE_WRITE and appends, so this was
     never likely; "never likely" is not the same as ruled out. */
#define WDLOG_PATH    OUT_("wdprobe.log")
/* ── ★ dsprobe.txt: GUEST DATA WORDS TO DUMP AT EVERY #GP. ────────────────────
     Whitespace-separated hex offsets; four bytes of DS: are printed for each.
     A guest's branch decisions live in its own data segment, and reading them off
     a disassembly is inference -- the whole ZAR investigation (GH #23) turned on
     ds:0x2e, ds:0x34 and the function pointer at ds:0xaa4, none of which any
     register dump can show. This is the generic form of the one-off ds:0000 dump:
     name the offsets in a file and they come back in the fault report, with no
     guest-specific code in the host and nothing to rebuild between guesses. */
#define DSPROBE_PATH     CFG_("dsprobe.txt")
/* csprobe.txt is the same knob against CS: the guest's CODE. It exists because a
   guest's dispatch can be PATCHED AT RUNTIME -- ZAR's DOS/16M reaches a routine the
   file on disk has no call to, so what matters is the bytes in memory, not the bytes
   in the image. Comparing the two is the whole point. */
#define CSPROBE_PATH     CFG_("csprobe.txt")
static VOID ProbeLoadInto(PCSTR path, WORD *out, INT *outCount)
{
    CHAR buffer[256]; DWORD bytesRead = 0; INT index = 0;
    HANDLE handle = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
    *outCount = 0;
    if (handle == INVALID_HANDLE_VALUE) return;
    ReadFile(handle, buffer, sizeof buffer - 1, &bytesRead, NULL);
    CloseHandle(handle);
    buffer[bytesRead < sizeof buffer ? bytesRead : sizeof buffer - 1] = 0;
    while (buffer[index] && *outCount < DSPROBE_MAX) {
        UINT value = 0; INT got = 0;
        while (buffer[index] == ' ' || buffer[index] == '\t' || buffer[index] == '\r' || buffer[index] == '\n' || buffer[index] == ',') ++index;
        while (buffer[index]) {
            CHAR character = buffer[index];
            if      (character >= '0' && character <= '9') value = (value << NIBBLE_SHIFT) | (UINT)(character - '0');
            else if (character >= 'a' && character <= 'f') value = (value << NIBBLE_SHIFT) | (UINT)(character - 'a' + HEX_DIGIT_A_VALUE);
            else if (character >= 'A' && character <= 'F') value = (value << NIBBLE_SHIFT) | (UINT)(character - 'A' + HEX_DIGIT_A_VALUE);
            else break;
            ++got; ++index;
        }
        if (got) out[(*outCount)++] = (WORD)value; else if (buffer[index]) ++index;
    }
}

VOID DsProbeLoad(VOID)
{
    CHAR buffer[256]; DWORD bytesRead = 0; INT index = 0;
    HANDLE handle;
    ProbeLoadInto(CSPROBE_PATH, g_CsProbe, &g_CsProbeCount);
    handle = CreateFileA(DSPROBE_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, 0, NULL);
    g_DsProbeCount = 0;
    if (handle == INVALID_HANDLE_VALUE) return;
    ReadFile(handle, buffer, sizeof buffer - 1, &bytesRead, NULL);
    CloseHandle(handle);
    buffer[bytesRead < sizeof buffer ? bytesRead : sizeof buffer - 1] = 0;
    while (buffer[index] && g_DsProbeCount < DSPROBE_MAX) {
        UINT value = 0; INT got = 0;
        while (buffer[index] == ' ' || buffer[index] == '\t' || buffer[index] == '\r' || buffer[index] == '\n' || buffer[index] == ',') ++index;
        while (buffer[index]) {
            CHAR character = buffer[index];
            if      (character >= '0' && character <= '9') value = (value << NIBBLE_SHIFT) | (UINT)(character - '0');
            else if (character >= 'a' && character <= 'f') value = (value << NIBBLE_SHIFT) | (UINT)(character - 'a' + HEX_DIGIT_A_VALUE);
            else if (character >= 'A' && character <= 'F') value = (value << NIBBLE_SHIFT) | (UINT)(character - 'A' + HEX_DIGIT_A_VALUE);
            else break;
            ++got; ++index;
        }
        if (got) g_DsProbe[g_DsProbeCount++] = (WORD)value; else if (buffer[index]) ++index;
    }
}
INT g_PmVehPass = 0;   /* pmvehpass.flag: let a non-INT PM fault fall THROUGH the VEH */
/* ⛔ A CLIENT THAT RETURNS TO ITS PARENT NEVER SET g_DpmiDone -- the run is not over --
     so its watchdog stayed armed over the shell. An idle prompt does not bump
     g_DpmiIteration and V86 code never counts as "moving", so quitting Doom to the prompt
     got the host TerminateProcess'd 3 s later (found s81 testing #152). Each watchdog
     now owns a generation and stands down when the client it watched is gone. */
volatile LONG  g_DpmiWatchdogGeneration   = 0;
volatile DWORD g_DpmiEnterCs = 0;  /* guest CS handed to the last DpmiEnterProtectedMode    */
volatile DWORD g_DpmiEnterEip= 0;  /* guest EIP handed to the last DpmiEnterProtectedMode   */
volatile DWORD g_DpmiLastEvent  = 0;  /* VTIB_EVENT reported by the last return        */
volatile DWORD g_DpmiLastVector = 0;  /* vector serviced on the last iteration         */
static volatile LONG  g_VehAny       = 0;  /* # PM-context exceptions delivered to the VEH  */
static volatile LONG  g_VehFatal     = 0;  /* # of those that took the non-reflect fatal path */
LONG g_IfvTraceCount;
DWORD g_IfvReenter[PIC_LINES];
DWORD g_AsyncEarlyBailLogged = 0;
/* ► WHICH CLAUSE SAID NO, PER LINE. `attempts` and `delivered` give the shortfall as one
     subtraction and no reason for it; this names every refusal. Read bucket 14 (the CPU
     thread was in HOST code) against 10 (an injection still in flight) and 7/8 (the client
     has interrupts off) -- they need three completely different fixes, and session 23
     spent a rig run on the one fix that could not have helped any of them. On its own
     (s85) so the headless forced exit prints it too: 3DBench's runs end that way, and
     #238 was diagnosed without it. */
VOID AsyncWhyReport(VOID)
{
    static PCSTR const whyNames[ASYNC_WHY_MAX] = {
        "DELIVERED","badvec","in_pm_irq","pm_noirq","no_catcher","unhooked_pm",
        "no_app_timer","vIF_off","IF_off","arm_quiet","IN_FLIGHT","host_stack",
        "not32","setctx_fail","HOST_CS","?f","?10","?11","?12","?13",
        "not_in_exec","pic_refuse","unhooked","suspend_fail","getctx_fail",
        "v86_IF_off","in_our_hdlr","observed","ctx_busy","left_exec","simint_rm","nested_tick" };
    CHAR base[1024], *cursor = base;
    UINT line, reason;
    /* NO SILENT CAPS: say how many ASYNC-EARLY lines were written and how many were
       suppressed, so the log's thinness is never read as "it stopped happening". The
       histogram below is the complete account either way. */
    cursor = LogPut(cursor, "STAGE2: async early-bail lines logged=");
    cursor = LogHex(cursor, g_AsyncEarlyBailLogged < ASYNC_EARLY_BAIL_LOG_MAX
                  ? g_AsyncEarlyBailLogged : ASYNC_EARLY_BAIL_LOG_MAX);
    cursor = LogPut(cursor, " of ");   cursor = LogHex(cursor, g_AsyncEarlyBailLogged);
    cursor = LogPut(cursor, " (cap "); cursor = LogHex(cursor, ASYNC_EARLY_BAIL_LOG_MAX);
    cursor = LogPut(cursor, " -- these are FILE I/O UNDER g_lock; see async_early_bail)\r\n");
    LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
    for (line = 0; line < 8; ++line) {     /* one line per IRQ, flushed each: <= 32 x 24 chars */
        UINT total = 0;
        for (reason = 0; reason < ASYNC_WHY_MAX; ++reason) total += g_AsyncWhyHistogram[line][reason];
        if (!total) continue;
        cursor = LogPut(cursor, "STAGE2: async why irq"); cursor = LogHexByte(cursor, (BYTE)line);
        cursor = LogPut(cursor, " total=");               cursor = LogHex(cursor, total);
        for (reason = 0; reason < ASYNC_WHY_MAX; ++reason) {
            if (!g_AsyncWhyHistogram[line][reason]) continue;
            cursor = LogPut(cursor, " "); cursor = LogPut(cursor, whyNames[reason]);
            cursor = LogPut(cursor, "="); cursor = LogHex(cursor, g_AsyncWhyHistogram[line][reason]);
        }
        cursor = LogPut(cursor, "\r\n");
        LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
    }
}

/* The IF/VIF census (see IfvNote), on its own so the headless forced exit -- which
   skips the main report, and is how ZAR's runs end -- can print it too. */
VOID IfvReport(VOID)
{
    CHAR base[4096], *cursor = base;
  { INT path, state, line; DWORD starveMaximumMs = g_IfvStarveMaximumMs;
    static PCSTR const stateNames[8] = { "000","001","010","011","100","101","110","111" };
    if (g_IfvStarveOpen && GetTickCount() - g_IfvStarveT0 > starveMaximumMs)
        starveMaximumMs = GetTickCount() - g_IfvStarveT0;
    cursor = LogPut(cursor, "STAGE2: IFV census (IF,VIF,S714)");
    for (path = 0; path < IFV_PATHS; ++path) {
        cursor = LogPut(cursor, path == 0 ? " live{" : path == 1 ? " vtib01{" : " vtibdev{");
        for (state = 0; state < 8; ++state) {
            if (!g_IfvCensus[path][state]) continue;
            cursor = LogPut(cursor, " "); cursor = LogPut(cursor, stateNames[state]); cursor = LogPut(cursor, "=");
            cursor = LogHex(cursor, g_IfvCensus[path][state]);
        }
        cursor = LogPut(cursor, " }");
    }
    cursor = LogPut(cursor, " starve_max_ms="); cursor = LogHex(cursor, starveMaximumMs);
    cursor = LogPut(cursor, " stretches=");     cursor = LogHex(cursor, g_IfvStarveCount);
    cursor = LogPut(cursor, " shadow{");
    for (line = 0; line < 16; ++line) {
        if (!g_IfvShadow[line]) continue;
        cursor = LogPut(cursor, " irq"); cursor = LogHexByte(cursor, (BYTE)line);
        cursor = LogPut(cursor, "=");    cursor = LogHex(cursor, g_IfvShadow[line]);
    }
    cursor = LogPut(cursor, " } reenter{");
    for (line = 0; line < 16; ++line) {
        if (!g_IfvReenter[line]) continue;
        cursor = LogPut(cursor, " irq"); cursor = LogHexByte(cursor, (BYTE)line);
        cursor = LogPut(cursor, "=");    cursor = LogHex(cursor, g_IfvReenter[line]);
    }
    cursor = LogPut(cursor, " }\r\n");
    LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
    { LONG index, count = g_IfvTraceCount < IFV_TRACE_MAX ? g_IfvTraceCount : IFV_TRACE_MAX;
      for (index = 0; index < count; ++index) {
          cursor = LogPut(cursor, "STAGE2: IFV irq"); cursor = LogHexByte(cursor, g_IfvTrace[index].Irq);
          cursor = LogPut(cursor, g_IfvTrace[index].Path == 0 ? " async" : " coop");
          cursor = LogPut(cursor, " st=");  cursor = LogPut(cursor, stateNames[g_IfvTrace[index].State & 7]);
          cursor = LogPut(cursor, " fl=0x"); cursor = LogHex(cursor, g_IfvTrace[index].Flags);
          cursor = LogPut(cursor, " at ");   cursor = LogHex(cursor, g_IfvTrace[index].Cs);
          cursor = LogPut(cursor, ":");      cursor = LogHex(cursor, g_IfvTrace[index].Ip);
          cursor = LogPut(cursor, "\r\n");
      }
      LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base; } }
}
#define FATAL_DUMP_EXIT_CODE 0xDE0   /* HostFatalDump: a clean exit after the dump */
/* Crash diagnostic (DPMI spike): the PM switch works but VdmStartExecution faults
   inside the monitor when it runs PM, crashing the host with no info. This VEH
   catches the fault, dumps the exception (code/addr/params) + the host CONTEXT +
   the guest PM CONTEXT to the log, then exits CLEANLY (no WER dialog) so the batch
   still prints the log. Only meaningful once g_DpmiPm is set. */
static INT g_VehCount = 0;

/* ── THE FATAL DUMP, SHARED. (session 62) ────────────────────────────────────────
     Factored out of the VEH's fatalDump arm so the LAST-CHANCE filter below can
     produce the same dump for a real-mode run -- until now `!g_DpmiPm` bailed at
     the top of the VEH, so a host fault under a V86-only guest fell through to WER
     and the log just STOPPED. Mario and Heretic both died blind that way (10-game
     pass, 2026-09-10). Never returns: writes the dump, drops the tray icon, exits
     cleanly so the batch still collects the log. Runs on a broken process: no
     allocation, no locks, statics only. Own 2 KB buffer -- the frames + @esp lines
     alone approach 1 KB, and appending them after an arm's ~400 chars overflowed
     the old shared cb[1024] silently. */
static VOID HostFatalDump(EXCEPTION_RECORD *record, CONTEXT *context)
{
    static CHAR lineBuffer[2048]; PSTR cursor = lineBuffer;
    InterlockedIncrement(&g_VehFatal);                 /* run 52: a real fault WAS delivered */
    cursor = LogPut(cursor, g_DpmiPm ? "\r\nDPMI FATAL: exception code=0x"
                          : "\r\nHOST FATAL (real-mode guest): exception code=0x");
    cursor = LogHex(cursor, record->ExceptionCode);
    cursor = LogPut(cursor, " at 0x"); cursor = LogHex(cursor, (UINT)(ULONG_PTR)record->ExceptionAddress);
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= EXCEPTION_AV_PARAMETERS) {
        cursor = LogPut(cursor, " av{op=0x");  cursor = LogHex(cursor, (DWORD)record->ExceptionInformation[EXCEPTION_AV_OPERATION]);
        cursor = LogPut(cursor, " addr=0x");   cursor = LogHex(cursor, (DWORD)record->ExceptionInformation[EXCEPTION_AV_ADDRESS]);
        cursor = LogPut(cursor, "}");
    }
    cursor = LogPut(cursor, "\r\n  CS:EIP=0x"); cursor = LogHex(cursor, context->SegCs); cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, context->Eip);
    cursor = LogPut(cursor, " SS:ESP=0x"); cursor = LogHex(cursor, context->SegSs); cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, context->Esp);
    cursor = LogPut(cursor, " EFL=0x"); cursor = LogHex(cursor, context->EFlags); cursor = LogPut(cursor, "\r\n");
    cursor = LogPut(cursor, "  DS=0x"); cursor = LogHex(cursor, context->SegDs); cursor = LogPut(cursor, " ES=0x"); cursor = LogHex(cursor, context->SegEs);
    cursor = LogPut(cursor, " FS=0x"); cursor = LogHex(cursor, context->SegFs); cursor = LogPut(cursor, " GS=0x"); cursor = LogHex(cursor, context->SegGs);
    cursor = LogPut(cursor, "\r\n  EAX=0x"); cursor = LogHex(cursor, context->Eax); cursor = LogPut(cursor, " EBX=0x"); cursor = LogHex(cursor, context->Ebx);
    cursor = LogPut(cursor, " ECX=0x"); cursor = LogHex(cursor, context->Ecx); cursor = LogPut(cursor, " EDX=0x"); cursor = LogHex(cursor, context->Edx);
    cursor = LogPut(cursor, "\r\n");
    { const BYTE *faultBytes = (const BYTE *)(ULONG_PTR)(record->ExceptionAddress);
      cursor = LogPut(cursor, "  bytes@fault: ");
      if (HostReadable(faultBytes, 16)) cursor = LogDump(cursor, faultBytes, 16); else cursor = LogPut(cursor, "<unreadable>");
    }
    /* ★ THE INTERPRETER'S LIVE REGISTERS. When the fault is inside istep (a stray
         guest pointer, s69), the VDM context is a whole slice stale; THIS is the es/di
         the effective address was actually built from. es_base+ea that lands in an
         unmapped hole names the bug: a wrong SEGMENT vs an unmasked OFFSET. */
    if (g_InterpreterCpu) {
        const V86_CPU *cpu = g_InterpreterCpu;
        cursor = LogPut(cursor, "\r\n  interp cs:ip=0x"); cursor = LogHex(cursor, cpu->Segments[1]); cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, cpu->Ip);
        cursor = LogPut(cursor, " es=0x"); cursor = LogHex(cursor, cpu->Segments[0]); cursor = LogPut(cursor, " ds=0x"); cursor = LogHex(cursor, cpu->Segments[3]);
        cursor = LogPut(cursor, " ss=0x"); cursor = LogHex(cursor, cpu->Segments[2]);
        cursor = LogPut(cursor, "\r\n  interp di=0x"); cursor = LogHex(cursor, cpu->Registers[7]); cursor = LogPut(cursor, " si=0x"); cursor = LogHex(cursor, cpu->Registers[6]);
        cursor = LogPut(cursor, " bx=0x"); cursor = LogHex(cursor, cpu->Registers[3]); cursor = LogPut(cursor, " bp=0x"); cursor = LogHex(cursor, cpu->Registers[5]);
        cursor = LogPut(cursor, " ax=0x"); cursor = LogHex(cursor, cpu->Registers[0]); cursor = LogPut(cursor, " cx=0x"); cursor = LogHex(cursor, cpu->Registers[1]);
        cursor = LogPut(cursor, "\r\n");
    }
    /* Where the GUEST was when the host died. For a real-mode crash this is the
       line that names the suspect -- e.g. a CLI poll of an unclaimed port. */
    if (g_TibDebug) {
        volatile BYTE *tibDebug = g_TibDebug;
        cursor = LogPut(cursor, "\r\n  guest tib{cs:ip=0x"); cursor = LogHex(cursor, VDM_REG16(tibDebug, VTIB_CS));
        cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, VDM_REG(tibDebug, VTIB_EIP));
        cursor = LogPut(cursor, " eax=0x"); cursor = LogHex(cursor, VDM_REG(tibDebug, VTIB_EAX));
        cursor = LogPut(cursor, " edx=0x"); cursor = LogHex(cursor, VDM_REG(tibDebug, VTIB_EDX)); cursor = LogPut(cursor, "}");
    }
    cursor = LogPut(cursor, "\r\n");
    /* ── ★★ WHO CALLED INTO THIS? (GH #128, session 38) ───────────────────────────
         A fault inside ntdll or kernel32 is OUR bug at one remove -- some host call
         passed a bad pointer -- and the registers name the *instruction* while saying
         nothing about the caller. "An access violation in ntdll at 0x7c912c16" is not
         an actionable line; the first return address inside our own image is.
       ⚠ Guarded at every step and bounded: this runs inside a VEH on a process that is
         already broken, so it may not allocate, may not lock, and must not fault. A
         frame chain that does not ascend is not a frame chain, and we stop rather than
         printing plausible-looking rubbish -- an instrument that invents its own frame
         is this project's most expensive recurring mistake. */
    cursor = LogPut(cursor, "  last WOW32 call ENTERED: id=0x"); cursor = LogHex(cursor, g_WowLastId);
    { PCSTR name = Wow32Name(g_WowLastId);
      if (name) { cursor = LogPut(cursor, " "); cursor = LogPut(cursor, name); } }
    cursor = LogPut(cursor, " from=0x"); cursor = LogHex(cursor, g_WowLastFrom);
    cursor = LogPut(cursor, " (it may have COMPLETED -- the log line is only written with the result,\r\n"
                "    so a crash inside a service loses the header and the log ends one call short)\r\n");
    cursor = LogPut(cursor, "  tid=0x"); cursor = LogHex(cursor, GetCurrentThreadId());
    cursor = LogPut(cursor, (GetCurrentThreadId() == g_GuestThreadId)
              ? " (THE GUEST THREAD)" : " (a WORKER thread, NOT the guest)");
    cursor = LogPut(cursor, " ourbase=0x");
    cursor = LogHex(cursor, (DWORD)(ULONG_PTR)GetModuleHandleA(NULL));
    cursor = LogPut(cursor, "\r\n  frames:");
    {   DWORD framePointer = context->Ebp; INT index;
        for (index = 0; index < 12 && framePointer; ++index) {
            const DWORD *frame = (const DWORD *)(ULONG_PTR)framePointer;
            if (!HostReadable(frame, 8)) { cursor = LogPut(cursor, " <unreadable>"); break; }
            cursor = LogPut(cursor, " 0x"); cursor = LogHex(cursor, frame[1]);
            if (frame[0] <= framePointer) { cursor = LogPut(cursor, " <chain ends>"); break; }
            framePointer = frame[0];
        }
        cursor = LogPut(cursor, "\r\n");
    }
    /* ★ AND THE RAW STACK, because the frame chain is only as good as EBP. A leaf
         function that has not built a frame, or one compiled without one, breaks the
         walk above and the walk cannot tell you that it did. Twenty-four words from
         ESP will contain the return address whether EBP is trustworthy or not -- the
         reader looks for one near `ourbase`. */
    {   const DWORD *stack = (const DWORD *)(ULONG_PTR)context->Esp; INT index;
        cursor = LogPut(cursor, "  @esp:");
        for (index = 0; index < 24; ++index) {
            if (!HostReadable(stack + index, 4)) { cursor = LogPut(cursor, " <unreadable>"); break; }
            cursor = LogPut(cursor, " 0x"); cursor = LogHex(cursor, stack[index]);
        }
        cursor = LogPut(cursor, "\r\n");
    }
    LogAppend(LOG_PATH, lineBuffer, cursor);
    SerialOut(lineBuffer, cursor);
    TrayRemove(g_Window);      /* the VEH exits without unwinding the UI thread */
    HostRecordFinish();
    ExitProcess(FATAL_DUMP_EXIT_CODE);                                 /* clean exit; batch dumps the log */
}
#define VEH_LOW_MEMORY_EIP_LIMIT_U 0x00200000u
/* First-chance sightings of real-mode/host faults -- see the arm in the VEH below. */
static LONG g_RmFaultSeen = 0;
#define DPMI_SPIKE_BASE_SELECTOR 0x001F   /* the spike's 0000h answer */
LONG CALLBACK DpmiCrashVeh(EXCEPTION_POINTERS *pointers)
{
    static CHAR lineBuffer[1024]; PSTR cursor = lineBuffer;
    EXCEPTION_RECORD *record = pointers->ExceptionRecord;
    CONTEXT *context = pointers->ContextRecord;
    if (!g_DpmiPm) {
        /* ── A REAL-MODE GUEST'S HOST CRASH USED TO BE INVISIBLE. (session 62) ────
             This handler bailed here unconditionally, so every host-side fault
             under a V86-only guest went to WER with nothing in the log. FIRST
             chance we only LOG -- an SEH frame somewhere below may legitimately
             claim the fault (nothing in src raises on purpose, but system DLLs
             may), and exiting here would kill a healthy run. If nothing claims
             it, HostUnhandledFilter writes the full dump at LAST chance.
             Error severity (0xC........) -- and BREAKPOINT/SINGLE-STEP too,
             measured the hard way: Mario's host dies with EXIT CODE 0x80000003
             (STATUS_BREAKPOINT, from the launcher's %ERRORLEVEL%), and the
             first cut of this arm filtered to error severity only, so the one
             exception that names the killer was exactly the one not logged.
             Guard pages and debug prints stay excluded. */
        if ((record->ExceptionCode & NT_STATUS_SEVERITY_MASK) == NT_STATUS_SEVERITY_ERROR
            || record->ExceptionCode == EXCEPTION_BREAKPOINT
            || record->ExceptionCode == EXCEPTION_SINGLE_STEP) {
            LONG faultNumber = InterlockedIncrement(&g_RmFaultSeen);
            if (faultNumber <= 32) {
                cursor = LogPut(cursor, "HOSTFAULT #"); cursor = LogHex(cursor, (UINT)faultNumber);
                cursor = LogPut(cursor, ": exc=0x"); cursor = LogHex(cursor, record->ExceptionCode);
                cursor = LogPut(cursor, " at=0x"); cursor = LogHex(cursor, (DWORD)(ULONG_PTR)record->ExceptionAddress);
                if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= EXCEPTION_AV_PARAMETERS) {
                    cursor = LogPut(cursor, " av{op=0x"); cursor = LogHex(cursor, (DWORD)record->ExceptionInformation[EXCEPTION_AV_OPERATION]);
                    cursor = LogPut(cursor, " addr=0x"); cursor = LogHex(cursor, (DWORD)record->ExceptionInformation[EXCEPTION_AV_ADDRESS]);
                    cursor = LogPut(cursor, "}");
                }
                cursor = LogPut(cursor, " cs:eip=0x"); cursor = LogHex(cursor, context->SegCs);
                cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, context->Eip);
                cursor = LogPut(cursor, " tid=0x"); cursor = LogHex(cursor, GetCurrentThreadId());
                cursor = LogPut(cursor, (GetCurrentThreadId() == g_GuestThreadId) ? " (guest thread)" : " (worker)");
                if (g_TibDebug) {
                    volatile BYTE *tibDebug = g_TibDebug;
                    cursor = LogPut(cursor, " tib{cs:ip=0x"); cursor = LogHex(cursor, VDM_REG16(tibDebug, VTIB_CS));
                    cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, VDM_REG(tibDebug, VTIB_EIP)); cursor = LogPut(cursor, "}");
                }
                cursor = LogPut(cursor, "\r\n");
                LogAppend(LOG_PATH, lineBuffer, cursor); SerialOut(lineBuffer, cursor);
            }
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
    InterlockedIncrement(&g_VehAny);                   /* run 52: prove ANY PM fault reaches us */

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
         only look while g_PmEntryEip is set, i.e. inside VdmStartExecution. Accept
         either: the old low-memory case, or a match on EDX. */
    if (context->SegCs == NT_USER_CODE_SELECTOR && g_VehCount < 256
        && (context->Eip < VEH_LOW_MEMORY_EIP_LIMIT_U
            || (g_PmEntryEip >= 0 && (DWORD)context->Edx == (DWORD)g_PmEntryEip))) {
        DWORD site = g_DpmiCodeBase + (context->Edx & WORD_MASK);
        const BYTE *siteBytes = (const BYTE *)(ULONG_PTR)site;
        DWORD functionNumber = context->Eax & WORD_MASK;
        BYTE  vector  = (siteBytes[0] == X86_OP_INT) ? siteBytes[1] : VECTOR_DPMI;    /* CD nn -> vector; default 31h    */
        ++g_VehCount;
        cursor = LogPut(cursor, "DPMI INT"); cursor = LogHex(cursor, vector); cursor = LogPut(cursor, "h #"); cursor = LogHex(cursor, (UINT)g_VehCount);
        cursor = LogPut(cursor, ": AX=0x"); cursor = LogHex(cursor, functionNumber);
        cursor = LogPut(cursor, " BX=0x"); cursor = LogHex(cursor, context->Ebx & WORD_MASK);
        cursor = LogPut(cursor, " CX=0x"); cursor = LogHex(cursor, context->Ecx & WORD_MASK);
        cursor = LogPut(cursor, " [site EDX=0x"); cursor = LogHex(cursor, context->Edx & WORD_MASK);
        cursor = LogPut(cursor, " EIP=0x"); cursor = LogHex(cursor, context->Eip);
        cursor = LogPut(cursor, " b@site="); cursor = LogDump(cursor, siteBytes, 4); cursor = LogPut(cursor, "]");
        { const BYTE *sent = (const BYTE *)(ULONG_PTR)0x1600;   /* guest sentinel DS:0x600 */
          cursor = LogPut(cursor, " sentinel@0x1600="); cursor = LogDump(cursor, sent, 4); }
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
        cursor = LogPut(cursor, " exc=0x"); cursor = LogHex(cursor, record->ExceptionCode);
        cursor = LogPut(cursor, " at=0x"); cursor = LogHex(cursor, (DWORD)(ULONG_PTR)record->ExceptionAddress);
        if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= EXCEPTION_AV_PARAMETERS) {
            cursor = LogPut(cursor, " av{op=0x");  cursor = LogHex(cursor, (DWORD)record->ExceptionInformation[EXCEPTION_AV_OPERATION]);
            cursor = LogPut(cursor, " addr=0x");   cursor = LogHex(cursor, (DWORD)record->ExceptionInformation[EXCEPTION_AV_ADDRESS]);
            cursor = LogPut(cursor, "}");
        }
        { const BYTE *faultBytes = (const BYTE *)(ULONG_PTR)(g_DpmiCodeBase + (context->Eip & WORD_MASK));
          cursor = LogPut(cursor, " b@eip="); cursor = LogDump(cursor, faultBytes, 6); }
        cursor = LogPut(cursor, " ctx{ss:esp=0x"); cursor = LogHex(cursor, context->SegSs); cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, context->Esp);
        cursor = LogPut(cursor, " ds=0x"); cursor = LogHex(cursor, context->SegDs); cursor = LogPut(cursor, " es=0x"); cursor = LogHex(cursor, context->SegEs);
        cursor = LogPut(cursor, " edi=0x"); cursor = LogHex(cursor, context->Edi); cursor = LogPut(cursor, "}");
        if (g_TibDebug) {
            volatile BYTE *tibDebug = g_TibDebug;
            cursor = LogPut(cursor, " tib{cs:eip=0x"); cursor = LogHex(cursor, VDM_REG16(tibDebug, VTIB_CS));
            cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, VDM_REG(tibDebug, VTIB_EIP));
            cursor = LogPut(cursor, " eax=0x"); cursor = LogHex(cursor, VDM_REG(tibDebug, VTIB_EAX));
            cursor = LogPut(cursor, " edx=0x"); cursor = LogHex(cursor, VDM_REG(tibDebug, VTIB_EDX)); cursor = LogPut(cursor, "}");
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
        { const BYTE *faultInstruction = (const BYTE *)(ULONG_PTR)(g_DpmiCodeBase + (context->Eip & WORD_MASK));
          if (faultInstruction[0] != X86_OP_INT) {
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
              if (g_PmVehPass) {
                  cursor = LogPut(cursor, " -> FAULT, not an INT: PASSING IT THROUGH (pmvehpass)");
                  cursor = LogPut(cursor, "\r\n");
                  LogAppend(LOG_PATH, lineBuffer, cursor); SerialOut(lineBuffer, cursor);
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
              if ((DWORD)context->Edx != (DWORD)g_PmEntryEip || g_PmEntryEip < 0) {
                  cursor = LogPut(cursor, " -> REAL fault (EDX is not the entry EIP): FATAL dump");
                  cursor = LogPut(cursor, "\r\n");
                  goto fatalDump;                       /* the label flushes cb */
              }
              if (g_PmEntryEip >= 0 && (DWORD)g_PmEntryEip != context->Eip) {
                  cursor = LogPut(cursor, " -> FAULT, not an INT: resuming at ENTRY 0x");
                  cursor = LogHex(cursor, (DWORD)g_PmEntryEip);
                  context->Eip = (DWORD)g_PmEntryEip;
              } else {
                  cursor = LogPut(cursor, " -> FAULT, not an INT: resuming untouched");
              }
              cursor = LogPut(cursor, "\r\n");
              LogAppend(LOG_PATH, lineBuffer, cursor); SerialOut(lineBuffer, cursor);
              context->SegCs = DPMI_INITIAL_CODE_SELECTOR; context->SegSs = DPMI_INITIAL_DATA_SELECTOR;       /* guest selectors back; regs intact */
              return EXCEPTION_CONTINUE_EXECUTION;
          } }
        context->EFlags &= ~EFLAGS_CF_U;                              /* default: CF=0 (success)          */
        switch (functionNumber) {
        case DPMI_FN_GET_VERSION:                                    /* get DPMI version                 */
            /* #248: the same answer as the main 0400h arm (dpmi_svc.h) -- this spike path
               said CL=3 and swapped the PIC bases (DH is the MASTER base). */
            context->Eax = (context->Eax & HIGH_WORD_MASK_U) | DPMI_VERSION_090;
            context->Ebx = (context->Ebx & HIGH_WORD_MASK_U) | DPMI_VER_BX;
            context->Ecx = (context->Ecx & HIGH_WORD_MASK_U) | DPMI_CPU_CLASS;
            context->Edx = (context->Edx & HIGH_WORD_MASK_U) | DPMI_VER_DX;
            cursor = LogPut(cursor, " -> DPMI 0.90");
            break;
        case DPMI_FN_ALLOCATE_DESCRIPTORS:                                    /* allocate LDT descriptors (CX=count) */
            context->Eax = (context->Eax & HIGH_WORD_MASK_U) | DPMI_SPIKE_BASE_SELECTOR;  /* base selector 0x1F (spike stub)    */
            cursor = LogPut(cursor, " -> alloc base sel 0x1F");
            break;
        default:
            context->EFlags |= EFLAGS_CF_U;                           /* CF=1: unsupported function        */
            cursor = LogPut(cursor, " -> UNSUPPORTED (CF=1)");
            break;
        }
        cursor = LogPut(cursor, "\r\n");
        LogAppend(LOG_PATH, lineBuffer, cursor); SerialOut(lineBuffer, cursor);
        context->SegCs = DPMI_INITIAL_CODE_SELECTOR; context->SegSs = DPMI_INITIAL_DATA_SELECTOR;             /* restore the guest's LDT selectors  */
        return EXCEPTION_CONTINUE_EXECUTION;            /* resume the guest past the INT      */
    }

    /* --- genuine (non-reflected) PM fault: full dump + clean exit -------------------- */
fatalDump:
    LogAppend(LOG_PATH, lineBuffer, cursor); SerialOut(lineBuffer, cursor);     /* flush the arm's context first;   */
    HostFatalDump(record, context);                            /* the dump has its own buffer. The */
    return EXCEPTION_CONTINUE_SEARCH;                   /* old inline dump overflowed cb.   */
}

/* ── LAST CHANCE: the dump a real-mode run never had. (session 62) ───────────────
     With g_DpmiPm set the VEH above already turns an unmatched fault into the
     fatal dump at FIRST chance. Real-mode runs only log there and pass the fault
     on -- so if no SEH frame claims it, it arrives here, where WER used to eat it
     and the log just stopped. Same dump, same clean exit, so the batch and the
     rig watcher collect the evidence either way. */
LONG WINAPI HostUnhandledFilter(EXCEPTION_POINTERS *pointers)
{
    static CHAR lineBuffer[128]; PSTR cursor = lineBuffer;
    /* ── NO VEH ON THIS OS (Windows 2000): the filter IS the VEH. ─────────────────
         With no vectored handler installed, a fault reaches here after the (empty)
         SEH chain, one dispatch step later than the VEH saw it on XP but with the same
         record and context. Run the same arms; if one resumes the guest, resume. */
    if (!g_PfnAddVeh) {
        if (DpmiCrashVeh(pointers) == EXCEPTION_CONTINUE_EXECUTION)
            return EXCEPTION_CONTINUE_EXECUTION;
        /* DpmiCrashVeh's fatal arm has already dumped and exited when it applies;
           anything that falls out of it is the real-mode/host case below. */
    }
    cursor = LogPut(cursor, "\r\nHOST UNHANDLED EXCEPTION (last chance; no SEH claimed it):\r\n");
    LogAppend(LOG_PATH, lineBuffer, cursor); SerialOut(lineBuffer, cursor);
    HostFatalDump(pointers->ExceptionRecord, pointers->ContextRecord);
    return EXCEPTION_EXECUTE_HANDLER;                   /* not reached */
}
enum { DPMI_WATCHDOG_TICK_MS = 250, DPMI_WATCHDOG_EXIT_CODE = 0xDD0, DPMI_WATCHDOG_FROZEN_TICKS = 12, DPMI_WATCHDOG_FROZEN_TICKS_WOW = 600 };   /* DpmiWatchdog: frozen 3 s, or 150 s on WOW (krnl386 is watched BECAUSE it stops) */
/* DPMI test watchdog: if the PM guest neither faults to the VEH nor exits within a few
   seconds (the kernel skip+resumes PM faults, so the guest spins), terminate cleanly so
   the batch dumps the log and locks release. Makes every DPMI run self-terminating. */
DWORD WINAPI DpmiWatchdog(LPVOID param)
{
    static CHAR lineBuffer[512]; PSTR cursor = lineBuffer; LONG prev = -1;
    LONG modeYGeneration = (LONG)(ULONG_PTR)param;          /* see g_DpmiWatchdogGeneration */
    /* ── ★★ AN INSTRUMENT MUST BE ABLE TO PREEMPT WHAT IT INSTRUMENTS. ─────────────
         The thread that RUNS THE GUEST is raised to THREAD_PRIORITY_ABOVE_NORMAL (or
         HIGHEST with execprio>=2) so the audio pump cannot preempt guest code. This
         thread was left at NORMAL -- and the rig is a SINGLE-CORE box. So the moment
         the guest stops yielding, which is the only moment this thread exists for, it
         is the lowest-priority runnable thread in the process and gets nothing.
       ⚠ That is the shape of the one-sample bug: wd[0] lands during bring-up while the
         guest is still returning to the host between PM events; from the first real
         spin onward there are no more samples, no stand-down line and no wedge line --
         the thread is not dead, it is starved. Session 31 read the same silence as the
         thread dying and guarded a dereference that was never reached.
       HIGHEST beats the guest at either execprio setting. Safe because this thread
         sleeps 250 ms out of every 250 ms; it can never starve the guest back. */
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    cursor = LogPut(cursor, "STAGE3-DPMI: watchdog started at THREAD_PRIORITY_HIGHEST; sampling host PM-loop heartbeat\r\n");
    /* ► LOG, don't just serial. SerialOut writes COM1, which exists on the QEMU dev VM
         and NOT on the bare-metal box -- so on the rig these lines went nowhere. That
         cost us a wrong conclusion about Doom (session 15): the absence of wd[] samples
         in result_doom.log was read as "it died before the first 250 ms sample", when in
         fact the samples were never written anywhere. Every diagnostic must reach the
         file log or it does not exist on the machine we actually test on. */
    LogAppend(WDLOG_PATH, lineBuffer, cursor); SerialOut(lineBuffer, cursor); cursor = lineBuffer;
    /* Sample the host PM loop concurrently while the main thread is (possibly) blocked
       inside DpmiEnterProtectedMode(). Each line answers the run-51 wall question:
         iter ADVANCING  -> the `for steps` loop is cycling; last ev/cs/eip/vec show WHICH
                            patched INT it keeps hitting (a busy-poll = a cheap missing
                            service, not the deep #GP wall).
         iter FROZEN     -> the main thread is wedged inside ONE DpmiEnterProtectedMode at the guest
                            CS:EIP we last handed off; the guest bytes there tell a plain-
                            instruction #GP (the deep wall) from a jmp-self spin.
         veh any/fatal   -> whether a real Win32 exception was EVER delivered to us. fatal>0
                            means the PM #GP IS catchable via SEH (good news); both 0 while
                            frozen confirms the kernel swallowed it (runs 20-34 wall). */
    /* Only a SUSTAINED freeze is a wedge. A healthy client -- especially an animation
       loop -- keeps bumping g_DpmiIteration every frame; it must NOT be killed. So sample
       forever, reset on any progress, and terminate only after ~3s of NO progress.
       Stand down entirely once the client has exited cleanly (g_DpmiDone). Log the
       first 12 samples (the run-51/52 wedge diagnostic) + any frozen streak, and stay
       quiet while healthy so a long run doesn't flood COM1. */
    { UINT tick = 0, frozen = 0; INT watchdogSaidWait = 0;
      for (;;) {
        LONG iter; DWORD enCs, enEip, base; const BYTE *entryBytes;
        Sleep(DPMI_WATCHDOG_TICK_MS);
        if (g_DpmiDone) {                              /* client exited cleanly -> keep the window */
            cursor = LogPut(cursor, "STAGE3-DPMI: watchdog stand-down (client done)\r\n");
            LogAppend(WDLOG_PATH, lineBuffer, cursor); SerialOut(lineBuffer, cursor);
            return 0;
        }
        if (g_DpmiWatchdogGeneration != modeYGeneration) {                  /* its client went back to a parent */
            cursor = LogPut(cursor, "STAGE3-DPMI: watchdog stand-down (client returned to its parent)\r\n");
            LogAppend(WDLOG_PATH, lineBuffer, cursor); LogAppend(LOG_PATH, lineBuffer, cursor);
            return 0;
        }
        /* ⚠ A TICK MARKER, so "no samples" can be told apart from "never woke up".
             Printed AFTER Sleep and BEFORE anything else, for the first 24 iterations.
             If wd[0] is followed by tick 1 but no sample, the fault is below; if there
             is no tick 1 at all, the thread never got the CPU back -- which is a
             different bug with a different fix, and the two were indistinguishable in
             every log so far. Cheap and self-retiring. */
        if (tick < 24) {
            cursor = LogPut(cursor, "  wdtick "); cursor = LogHex(cursor, tick); cursor = LogPut(cursor, "\r\n");
            LogAppend(WDLOG_PATH, lineBuffer, cursor); cursor = lineBuffer;
        }
        iter   = g_DpmiIteration;
        frozen = (iter == prev) ? (frozen + 1) : 0;
        /* ── ★★★ A Win16 TASK WAITING FOR THE USER IS NOT WEDGED. ─────────────
             The exec thread parks the guest inside Win16 `GetMessage` when its
             queue is empty, which is where a Win16 task is SUPPOSED to wait --
             and while it is parked `g_DpmiIteration` does not advance, so this
             watchdog counted it as frozen and TerminateProcess'd the host 150 s
             later. That is what killed an idle Notepad the user had been left to
             click around in, and the only record went to THIS log rather than the
             main one, so `ntvdmhost.log` just stopped.
           ⇒ The host knows the difference and says so; sampling never could.
             See g_WowMsgInWait in wowmsg.h. The streak is RESET rather than the
             sample skipped, so a guest that wakes, wedges and is not in the wait
             still gets the full 150 s from the moment it stopped advancing. */
        if (g_PauseWant && frozen) frozen = 0;   /* #219: paused, not wedged */
        if (g_WowMsgInWait && frozen) {
            if (!watchdogSaidWait) {
                watchdogSaidWait = 1;
                cursor = LogPut(cursor, "  wd: the guest is PARKED IN Win16 GetMessage -- waiting"
                            " for input, not wedged; the freeze counter is held at 0"
                            " for as long as it waits\r\n");
                LogAppend(WDLOG_PATH, lineBuffer, cursor); SerialOut(lineBuffer, cursor); cursor = lineBuffer;
            }
            frozen = 0;
        }
        if (tick < 12 || frozen) {                         /* diagnostic window + any freeze */
            enCs  = g_DpmiEnterCs;  enEip = g_DpmiEnterEip;
            cursor = LogPut(cursor, "  wd["); cursor = LogHex(cursor, tick);
            cursor = LogPut(cursor, "] iter="); cursor = LogHex(cursor, (UINT)iter);
            cursor = LogPut(cursor, frozen ? " FROZEN" : " advancing");
            cursor = LogPut(cursor, " enter="); cursor = LogHex(cursor, enCs); cursor = LogPut(cursor, ":"); cursor = LogHex(cursor, enEip);
            cursor = LogPut(cursor, " last{ev="); cursor = LogHex(cursor, g_DpmiLastEvent);
            cursor = LogPut(cursor, " cs:eip="); cursor = LogHex(cursor, g_DpmiLastCs); cursor = LogPut(cursor, ":"); cursor = LogHex(cursor, g_DpmiLastEip);
            cursor = LogPut(cursor, " vec="); cursor = LogHex(cursor, g_DpmiLastVector); cursor = LogPut(cursor, "}");
            cursor = LogPut(cursor, " veh{any="); cursor = LogHex(cursor, (UINT)g_VehAny);
            cursor = LogPut(cursor, " fatal="); cursor = LogHex(cursor, (UINT)g_VehFatal); cursor = LogPut(cursor, "}");

            /* ── ★ FLUSH THE CHEAP READING BEFORE ATTEMPTING THE EXPENSIVE ONE. ────
                 Everything above is loads of our own globals and cannot fault. What
                 follows resolves a guest selector, dereferences guest memory and
                 suspends another thread -- any of which can fault or block, and if it
                 does on the SAME line, the basic sample is lost with it.
               ⚠ THIS IS THE ACTUAL SHAPE OF THE ONE-SAMPLE BUG. wd[0] has frozen==0
                 and skips the enrichment entirely; wd[1] is the first sample to attempt
                 it -- and on every WOW run the log has exactly wd[0] and then silence,
                 with no stand-down line and no wedge line, i.e. the thread does not
                 come back from iteration 1. Session 31 guarded the dereference and the
                 symptom survived, so the guard was not the whole story. Splitting the
                 line is what makes the NEXT run diagnostic instead of ambiguous: basic
                 samples appearing without enrichment localises the fault to the
                 enrichment; basic samples stopping too localises it to Sleep/log. */
            cursor = LogPut(cursor, "\r\n");
            LogAppend(WDLOG_PATH, lineBuffer, cursor); cursor = lineBuffer;

            /* ⚠ AND NO SerialOut IN THIS LOOP. FlushFileBuffers on a comm handle has
                 no timeout (only WriteFile does), so it is the one unbounded call on
                 this path -- and COM1 does not exist on the bare-metal rig, so these
                 lines never went anywhere useful anyway. A debug sink that can block
                 the thread it instruments is worse than no sink. */

            if (frozen) {
              cursor = LogPut(cursor, "  wd["); cursor = LogHex(cursor, tick); cursor = LogPut(cursor, "]+");
              if (g_DpmiPm) {                           /* wedged: dump the guest bytes there */
                base = DpmiSelectorBase((WORD)enCs);
                entryBytes = (const BYTE *)(ULONG_PTR)(base + (enEip & WORD_MASK));
                /* ⚠ GUARD THE DEREFERENCE. This read is unguarded no longer, and the
                     bug it caused is the exact shape this project keeps hitting: an
                     instrument that dies at the moment it becomes useful. wd[0] has
                     frozen==0 and skips this branch; wd[1] is the FIRST sample that
                     takes it -- so if the selector does not resolve, the watchdog
                     thread faults on its first frozen sample and is never heard from
                     again. On the WOW runs that is precisely what the log showed:
                     one wd[] line, then silence, then no wedge report at all, so the
                     one question the watchdog exists to answer ("where is the guest
                     stuck?") went unanswered while looking like "nothing was wrong".
                   MemoryReadable() is already in this file for this reason. */
                if (base && MemoryReadable((ULONG_PTR)entryBytes, 8)) {
                    cursor = LogPut(cursor, " b@enter="); cursor = LogDump(cursor, entryBytes, 8);
                } else {
                    cursor = LogPut(cursor, " b@enter=<cs 0x"); cursor = LogHex(cursor, enCs);
                    cursor = LogPut(cursor, " does not resolve>");
                }
              }
            /* ── ★ AND WHERE IT ACTUALLY IS, NOT WHERE IT WENT IN. ─────────────────
                 `enter=` is the CS:EIP we HANDED to DpmiEnterProtectedMode. The PM heartbeat
                 prints the same thing, so when a WOW run's log stops, all it licenses
                 is "it went in there and did not come back" -- NOT "it stopped there".
                 Those are different claims and only the first is measured. Session 31
                 ended on exactly that ambiguity, inside krnl386.
               A spinning PM guest never leaves PM, so the only way to see it is to
                 freeze the thread running it and read the context -- the same
                 SuspendThread / GetThreadContext round trip the async IRQ injector
                 already does on g_HostCpu.
               ⚠ SAMPLE HELD, LOG RELEASED. Nothing is written to the log between
                 SuspendThread and ResumeThread: LogAppend opens a file, and blocking
                 there with the guest frozen is how a diagnostic becomes a deadlock. */
              if (g_HostCpu) {
                CONTEXT context;
                DWORD guestCs = 0, guestEip = 0, guestSs = 0, guestEsp = 0, guestFlags = 0;
                INT got = 0;
                context.ContextFlags = CONTEXT_CONTROL;
                if (SuspendThread(g_HostCpu) != (DWORD)-1) {
                    if (GetThreadContext(g_HostCpu, &context)) {
                        guestCs = context.SegCs; guestEip = context.Eip; guestSs = context.SegSs;
                        guestEsp = context.Esp;  guestFlags  = context.EFlags; got = 1;
                    }
                    ResumeThread(g_HostCpu);
                }
                if (!got) {
                    cursor = LogPut(cursor, " LIVE=<thread sample failed>");
                } else {
                    DWORD liveBase = DpmiSelectorBase((WORD)guestCs);
                    const BYTE *liveBytes = (const BYTE *)(ULONG_PTR)(liveBase + (guestEip & WORD_MASK));
                    cursor = LogPut(cursor, " LIVE cs:eip=");  cursor = LogHex(cursor, guestCs);
                    cursor = LogPut(cursor, ":");              cursor = LogHex(cursor, guestEip);
                    cursor = LogPut(cursor, " ss:esp=");       cursor = LogHex(cursor, guestSs);
                    cursor = LogPut(cursor, ":");              cursor = LogHex(cursor, guestEsp);
                    cursor = LogPut(cursor, " efl=");          cursor = LogHex(cursor, guestFlags);
                    cursor = LogPut(cursor, " csbase=");       cursor = LogHex(cursor, liveBase);
                    if (liveBase && MemoryReadable((ULONG_PTR)liveBytes, 8)) {
                        cursor = LogPut(cursor, " b@live="); cursor = LogDump(cursor, liveBytes, 8);
                    }
                    /* ── ★ A GUEST THAT IS MOVING IS NOT WEDGED. (s74c, Duke3D's SETUP)
                         `iter` counts PM ENTRIES, so a flat client that runs natively
                         without trapping -- SETMAIN hooks the keyboard, not the timer,
                         draws its menu and then polls its own key buffer -- never bumps
                         it, and this watchdog TerminateProcess'd a perfectly healthy
                         program 3 s after it finished loading. By hand that is "setup
                         crashes". The live sample above already tells the two apart:
                         a wedge sits on one EIP (a jmp-self, or the kernel never
                         returning, in which case the sample is host code), a running
                         guest is somewhere else every 250 ms. Client code (TI=1) at a
                         new EIP resets the streak, exactly as the Win16 wait does. */
                    { static DWORD watchdogLivePrevious = 0;
                      if ((guestCs & DPMI_SELECTOR_TI) && guestEip != watchdogLivePrevious) {
                          frozen = 0;
                          cursor = LogPut(cursor, " (moving -> not wedged)");
                      }
                      watchdogLivePrevious = guestEip; }
                }
              }
              cursor = LogPut(cursor, "\r\n");
              LogAppend(WDLOG_PATH, lineBuffer, cursor); cursor = lineBuffer;    /* the enrichment, as its own line */
            }
        }
        prev = iter; ++tick;
        /* ⚠ ON A WOW RUN A FREEZE IS THE MEASUREMENT, NOT A FAULT TO BE CLEANED UP.
             12 frozen samples is 3 seconds, after which this thread TerminateProcess()es
             the host -- fine for a DOS client that should never stall, and exactly wrong
             for krnl386, which is being watched precisely BECAUSE it stops. Killing at
             3s throws away every later sample, i.e. the whole trace of where it sits.
             wowrun.bat already bounds the run (75s, then taskkill) and the headless
             deadline bounds the rest, so nothing here is unbounded. */
        if (frozen >= (g_WowModuleCount ? DPMI_WATCHDOG_FROZEN_TICKS_WOW : DPMI_WATCHDOG_FROZEN_TICKS)          /* 3s normally; 150s on a WOW run */
            && g_DpmiWatchdogGeneration == modeYGeneration) break;           /* ...and only while its client lives */
      }
    }
    cursor = LogPut(cursor, "STAGE3-DPMI: watchdog terminating (wedged)\r\n");
    LogAppend(WDLOG_PATH, lineBuffer, cursor);
    /* ⚠ AND SAY IT IN THE MAIN LOG TOO. This used to go only to wdprobe.log, so a
         host killed here left `ntvdmhost.log` ending mid-sentence with no reason --
         indistinguishable from a crash, and it cost a wrong diagnosis. Whoever
         reads the log the run was writing must be told what stopped it. */
    LogAppend(LOG_PATH, lineBuffer, cursor);
    SerialOut(lineBuffer, cursor);
    /* ► FLUSH WHAT THE PROGRAM PRINTED BEFORE KILLING IT. Same text the clean
         wind-down emits, in the one path that used to lose it. Written in bounded
         slices because the accumulator is far larger than this thread's buffer. */
    if (g_Machine && g_Machine->OutputLength > 0) {
        DWORD offset = 0, total = (DWORD)g_Machine->OutputLength;
        cursor = lineBuffer; cursor = LogPut(cursor, "  ==> DOS OUTPUT (wedged): [\r\n");
        LogAppend(WDLOG_PATH, lineBuffer, cursor);
        while (offset < total) {
            DWORD length = total - offset; CHAR slice[257];
            if (length > 256) length = 256;
            { DWORD index; for (index = 0; index < length; ++index) slice[index] = g_Machine->Output[offset + index]; }
            LogAppend(WDLOG_PATH, slice, slice + length);
            offset += length;
        }
        cursor = lineBuffer; cursor = LogPut(cursor, "\r\n]\r\n");
        LogAppend(WDLOG_PATH, lineBuffer, cursor);
    }
    /* ⚠ TAKE THE TRAY ICON WITH US. TerminateProcess runs NO cleanup at all, so
         an icon left installed here becomes a genuine GHOST -- it sits in the
         tray pointing at a dead process until the user happens to mouse over it
         and Explorer reaps it. That is the second half of the user's "they are
         stacking up" report, and it is the half that survives the process. */
    TrayRemove(g_Window);
    /* TerminateProcess (forceful) -- ExitProcess hangs trying to unwind the PM engine
       thread (un-terminable LDT context). */
    HostRecordFinish();
    TerminateProcess(GetCurrentProcess(), DPMI_WATCHDOG_EXIT_CODE);
    return 0;
}
