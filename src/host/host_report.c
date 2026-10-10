/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The end-of-run report: what the run did, section by section, in the
 * order the log has always had them (ReportEndOfRun), plus the start mode and the DOS
 * output that WinMain reports just before it.
 *
 * Its own translation unit (#335): declared in host_report.h.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include "host_report.h"
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
#include "main.h"
#include "host_audio.h"
#include "host_bios.h"
#include "host_diag.h"
#include "host_dos.h"
#include "host_dpmi.h"
#include "host_dpmi_int.h"
#include "host_input.h"
#include "host_io.h"
#include "host_irq.h"
#include "host_timing.h"
#include "host_video.h"
#include "host_window.h"
#include "host_wow.h"

PSTR ReportStartMode(PSTR cursor)
{
    /* [CAUTION]: REPORTED AT EXIT, not at startup. The startup line is written before a
     * later LogWrite(LOG_PATH,...) TRUNCATES the file, so it never survived to
     * be read -- exactly as #131's stdout line did not. Same trap, same day.
     */
    cursor = LogPut(cursor, "STAGE2: start mode was ");
    cursor = LogPut(cursor, g_StartMode == DOS_START_UNINSTALL ? "UNINSTALL"
             : g_StartMode == DOS_START_SAFE       ? "SAFE (skipped: VDD plugins, audio"
                                                       " output, real speaker, joystick, WOW"
                                                       " shims, fullscreen)" : "normal");
    cursor = LogPut(cursor, g_Wave.IsUsingDirectSound ? " [audio: DirectSound]" : g_Wave.IsSilent ? " [audio: no device, silent pump]"
                                                                          : " [audio: WinMM]");
    cursor = LogPut(cursor, "; clean exit -> failure counter cleared (GH #132)\r\n");
    return cursor;
}

/* End of run: where stdout went, and the DOS output -- echoed to the console when nothing live carried it. */
PSTR ReportStdoutAndDosOutput(PSTR cursor, DOS_MACHINE *machine)
{
    cursor = LogPut(cursor, "STAGE2: stdout -> ");

    if (g_StdioSource[0])
    {
        cursor = LogPut(cursor, g_StdioSource);
        cursor = LogPut(cursor, " -> ");
    }

    cursor = LogPut(cursor, g_StdioHow);
    cursor = LogPut(cursor, g_Stdio != INVALID_HANDLE_VALUE ? " [LIVE]" : " [buffered only]");
    cursor = LogPut(cursor, " ppid=0x"); cursor = LogHex(cursor, g_StdioParentProcessId); cursor = LogPut(cursor, "\r\n");
    {
        HANDLE consoleHandle = (g_Stdio == INVALID_HANDLE_VALUE)
            ? CreateFileA(HOST_DEVICE_CONSOLE_OUTPUT, GENERIC_WRITE, FILE_SHARE_WRITE, NULL,
                          OPEN_EXISTING, 0, NULL)
            : INVALID_HANDLE_VALUE;

        if (machine->OutputLength > 0)
        {
            machine->Output[machine->OutputLength] = 0;

            if (consoleHandle != INVALID_HANDLE_VALUE)
            {
                DWORD consoleWritten;
                WriteFile(consoleHandle, machine->Output, machine->OutputLength, &consoleWritten, NULL);
            }

            cursor = LogPut(cursor, "  ==> DOS OUTPUT: ["); cursor = LogPut(cursor, machine->Output);

            if (machine->IsOutputTruncated)
                cursor = LogPut(cursor, "\r\n<<<OUTPUT TRUNCATED>>>");

            cursor = LogPut(cursor, "]\r\n");
        }

        if (consoleHandle != INVALID_HANDLE_VALUE)
            CloseHandle(consoleHandle);
    }
    return cursor;
}

/* End of run: how many NTVDM BOPs the guest issued -- the count the log's rate limit hides. */
static PSTR ReportNtvdmBops(PSTR cursor)
{
    /* Exec-loop accounting: how much of the run went on port-I/O round trips, how
     * much the burst fast path absorbed, and whether timer IRQs actually landed.
     */
    /* The count the rate-limit above hides. An absence here means no guest issued one;
     * a large number means a guest is POLLING a call we have not implemented.
     */
    if (g_NtvdmBopCount)
    {
        cursor = LogPut(cursor, "STAGE2: NTVDM BOPs from guest = "); cursor = LogDecimal(cursor, g_NtvdmBopCount);
        cursor = LogPut(cursor, " (see docs/inventory/bop.md)\r\n");
    }

    return cursor;
}

/* End of run: the hottest I/O ports, then the PIT reload, IRQ0 in-service accounting and the async-injection counters. */
static PSTR ReportHotPortsAndTimerCounters(PSTR cursor)
{
    INT index;

    cursor = LogPut(cursor, "STAGE2: hot ports:");

    for (index = 0; index < g_IoHotCount; ++index)
    {
        cursor = LogPut(cursor, " 0x"); cursor = LogHex(cursor, g_IoHot[index].Port);
        cursor = LogPut(cursor, "=0x"); cursor = LogHex(cursor, g_IoHot[index].Count);
    }

    cursor = LogPut(cursor, "\r\nSTAGE2: pit_reload=0x"); cursor = LogHex(cursor, (DWORD)g_Pit.Reload);
    cursor = LogPut(cursor, " oneshot_loads=0x"); cursor = LogHex(cursor, g_Pit.OneShotLoads);   /* #175 */
    cursor = LogPut(cursor, " skip_if=0x");   cursor = LogHex(cursor, g_Irq0SkipIf);
    cursor = LogPut(cursor, " skip_stub=0x"); cursor = LogHex(cursor, g_Irq0SkipStub);
    cursor = LogPut(cursor, " async_inj=0x"); cursor = LogHex(cursor, g_AsyncInjected);
    cursor = LogPut(cursor, " async_pm=0x"); cursor = LogHex(cursor, g_AsyncPmInjected);
    cursor = LogPut(cursor, " async_bail=0x"); cursor = LogHex(cursor, g_AsyncBail);
    cursor = LogPut(cursor, " async_nest=0x"); cursor = LogHex(cursor, g_AsyncNestBlocked);
    /* s70: IRQ0 in-service accounting (see Irq0Ack). strict = acknowledges that held
     * the line, auto = stub/fallback, blocks = deliveries refused while in service,
     * timeouts = releases by the safety net, fallback = the auto-EOI regime engaged.
     */
    cursor = LogPut(cursor, " irq0_isr[strict,auto,blocks,timeouts,fallback,resync_drop]=0x"); cursor = LogHex(cursor, g_Irq0IsrStrict);
    cursor = LogPut(cursor, ",0x"); cursor = LogHex(cursor, g_Irq0IsrAuto);
    cursor = LogPut(cursor, ",0x"); cursor = LogHex(cursor, g_Irq0IsrBlocks);
    cursor = LogPut(cursor, ",0x"); cursor = LogHex(cursor, g_Irq0IsrTimeouts);
    cursor = LogPut(cursor, ",0x"); cursor = LogHex(cursor, (DWORD)g_Irq0AutoEoi);
    cursor = LogPut(cursor, ",0x"); cursor = LogHex(cursor, g_Irq0ResyncDrop);
    cursor = LogPut(cursor, " irq1_inj=0x");   cursor = LogHex(cursor, g_Irq1Injected);
    cursor = LogPut(cursor, " int16=[");
    {
        INT item;

        for (item = 0; item < 4; ++item)
        {
            cursor = LogPut(cursor, "0x");
            cursor = LogHex(cursor, g_Input.Int16Calls[item]);
            cursor = LogPut(cursor, " ");
        }
    }
    cursor = LogPut(cursor, "] p60=0x");       cursor = LogHex(cursor, g_Input.Port60Reads);
    cursor = LogPut(cursor, " owed=0x");       cursor = LogHex(cursor, g_Input.OwedScanCodesServed);   /* keys the BIOS arm served after a hook's port read */
    cursor = LogPut(cursor, " sc_left=0x");    cursor = LogHex(cursor, (DWORD)VddInputScanCodesQueued(&g_Input));
    cursor = LogPut(cursor, " sc_held=0x");    cursor = LogHex(cursor, g_Input.ScanCodeHeldReads);   /* re-reads inside the transfer hold */
    cursor = LogPut(cursor, " sc_push=0x");    cursor = LogHex(cursor, g_Input.ScanCodesPushed);
    cursor = LogPut(cursor, " sc_drop=0x");    cursor = LogHex(cursor, g_Input.ScanCodesDropped);
    /* #244/#274: INT 15h AH=4Fh calls made / bytes handed back (the difference is what
     * a hook swallowed); default INT 05h jobs / printer errors / last status.
     */
    cursor = LogPut(cursor, " kb4f=0x");       cursor = LogHex(cursor, g_Kb4FCalls);
    cursor = LogPut(cursor, "/0x");            cursor = LogHex(cursor, g_Kb4FTranslate);
    cursor = LogPut(cursor, " prtsc=0x");      cursor = LogHex(cursor, g_PrintScreenJobs);
    cursor = LogPut(cursor, "/0x");            cursor = LogHex(cursor, g_PrintScreenErrors);
    cursor = LogPut(cursor, "/0x");            cursor = LogHex(cursor, g_PrintScreenStatus);
    /* sc_hi is the deepest the 32-byte FIFO ever got; pit_clamp counts catch-up
     * bursts the PIT refused to replay. Together these say whether a held key was
     * starved of exec-loop turns and whether the guest's clock ever lurched.
     */
    cursor = LogPut(cursor, " sc_hi=0x");      cursor = LogHex(cursor, g_Input.ScanCodeHighWater);
    /* pit_gaps = syncs more than 10 ms apart; pit_gapmax = the worst, in 8254
     * clocks (1193182 = 1 s). NOTHING is clamped to these -- they exist so the
     * catch-up burst can be fixed from a measured gap distribution instead of an
     * assumed one, which is precisely the mistake that made it worse.
     */
    cursor = LogPut(cursor, " pit_gaps=0x");   cursor = LogHex(cursor, g_PitCatchupClamped);
    cursor = LogPut(cursor, " pit_gapmax=0x"); cursor = LogHex(cursor, g_PitGapMaximum);
    return cursor;
}

/* End of run: how long presenting a frame took, in the window and fullscreen. */
static PSTR ReportPresentTiming(PSTR cursor)
{
    /* s84 (user): the cost of drawing the picture, per present, by path -- the number
     * that decides whether a windowed DirectDraw renderer is worth building. Decimal
     * microseconds: mean and worst. `win` = GDI (window or borderless fullscreen).
     */
    cursor = LogPut(cursor, "\r\nSTAGE2: present us win n="); cursor = LogDecimal(cursor, (DWORD)g_PresentDdraw.PresentWindowCount);
    cursor = LogPut(cursor, " mean="); cursor = LogDecimal(cursor, g_PresentDdraw.PresentWindowCount ? (DWORD)(g_PresentDdraw.PresentWindowUs / g_PresentDdraw.PresentWindowCount) : 0);
    cursor = LogPut(cursor, " max=");  cursor = LogDecimal(cursor, (DWORD)g_PresentDdraw.PresentWindowMax);
    cursor = LogPut(cursor, " | fs(ddraw) n="); cursor = LogDecimal(cursor, (DWORD)g_PresentDdraw.PresentFullscreenCount);
    cursor = LogPut(cursor, " mean="); cursor = LogDecimal(cursor, g_PresentDdraw.PresentFullscreenCount ? (DWORD)(g_PresentDdraw.PresentFullscreenUs / g_PresentDdraw.PresentFullscreenCount) : 0);
    cursor = LogPut(cursor, " max=");  cursor = LogDecimal(cursor, (DWORD)g_PresentDdraw.PresentFullscreenMax);
    cursor = LogPut(cursor, " flips{done="); cursor = LogDecimal(cursor, (DWORD)g_PresentDdraw.FlipDone);   /* s86 */
    cursor = LogPut(cursor, " pend=");       cursor = LogDecimal(cursor, (DWORD)g_PresentDdraw.FlipPending);
    cursor = LogPut(cursor, " mid=");        cursor = LogDecimal(cursor, (DWORD)g_PresentDdraw.FlipMidScreen);
    cursor = LogPut(cursor, " drop=");       cursor = LogDecimal(cursor, (DWORD)g_PresentDdraw.FlipDropped);
    cursor = LogPut(cursor, " bufs=");       cursor = LogDecimal(cursor, (DWORD)g_PresentDdraw.FlipBuffers);
    cursor = LogPut(cursor, " ourwait=");    cursor = LogDecimal(cursor, (DWORD)g_PresentDdraw.IsFlipOurWait);
    cursor = LogPut(cursor, g_PresentDdraw.IsFlipDriverTimed ? " path=driverflag}" : " path=triple}");
    cursor = LogPut(cursor, " winsize="); cursor = LogDecimal(cursor, g_Settings.Values[SET_WINSIZE] + 1);
    cursor = LogPut(cursor, "x scaler="); cursor = LogDecimal(cursor, g_Settings.Values[SET_SCALER]);
    return cursor;
}

/* End of run: the PIT pacer, the retrace-wait idles, and the host CPU time the process used. */
static PSTR ReportPitPacingAndHostCpuTime(PSTR cursor)
{
    cursor = LogPut(cursor, "\r\nSTAGE2: pitpace=");  cursor = LogHex(cursor, (DWORD)g_PitPaceMs);
    cursor = LogPut(cursor, " calls="); cursor = LogHex(cursor, g_PitPaceCalls);
    cursor = LogPut(cursor, " prio="); cursor = LogHex(cursor, (DWORD)g_PitPacePriority);
    cursor = LogPut(cursor, " inject="); cursor = LogHex(cursor, (DWORD)g_PitPaceInject);
    cursor = LogPut(cursor, " uitick_min_ms="); cursor = LogHex(cursor, (DWORD)g_UiTickMinimumMs);
    cursor = LogPut(cursor, g_UiTickMinimumMs == UITICK_AUTO ? " (AUTO)" : " (fixed)");
    cursor = LogPut(cursor, " presents{hook="); cursor = LogHex(cursor, g_UiHookPresents);
    cursor = LogPut(cursor, " timer="); cursor = LogHex(cursor, g_UiTimerPresents);
    cursor = LogPut(cursor, " hook_fires="); cursor = LogHex(cursor, g_Video.PresentHookFires);
    cursor = LogPut(cursor, " of_which_after_draw="); cursor = LogHex(cursor, g_Video.PresentHookGap); cursor = LogPut(cursor, "}");
    cursor = LogPut(cursor, " uitick_skipped="); cursor = LogHex(cursor, g_UiTickSkips);
    /* GH #56: DID THE THROTTLE ACTUALLY BITE?:
     * run/held are the milliseconds the Bresenham handed out, and held/(run+
     * held) must come out at 1 - duty or the mechanism is not doing what the
     * setting says. `missed` is the one to watch: a held millisecond where the
     * guest was NOT inside VdmStartExecution, which we cannot hold. A large
     * missed against a small held means the guest spends its time in host code
     * -- our own service calls -- and the throttle is reaching a fraction of
     * its execution. That is a real limit of the mechanism and it should be
     * legible in the log rather than inferred from a stopwatch.
     */
    cursor = LogPut(cursor, "\r\nSTAGE2: retrace-wait idles (1 ms sleeps)="); cursor = LogDecimal(cursor, g_RetraceIdles);  /* #183 */
    /* #183 (user, 2026-09-28: the concern is host CPU AND frame rate): what the host
     * actually spent, from Windows' own accounting -- the process, and the thread that
     * runs the guest -- so a cheaper wait shows up as a number, not an impression.
     */
    {   FILETIME creationTime, exitTime, kernelTime, userTime;
    ULONGLONG previousKernel = 0;
    ULONGLONG previousUser = 0;
    ULONGLONG totalKernel = 0;
    ULONGLONG totalUser = 0;

        if (GetProcessTimes(GetCurrentProcess(), &creationTime, &exitTime, &kernelTime, &userTime))
        {
            previousKernel = ((ULONGLONG)kernelTime.dwHighDateTime << DWORD_SHIFT | kernelTime.dwLowDateTime) / FILETIME_TICKS_PER_MILLISECOND_U;
            previousUser = ((ULONGLONG)userTime.dwHighDateTime << DWORD_SHIFT | userTime.dwLowDateTime) / FILETIME_TICKS_PER_MILLISECOND_U; }

        if (g_HostCpu && GetThreadTimes(g_HostCpu, &creationTime, &exitTime, &kernelTime, &userTime))
        {
            totalKernel = ((ULONGLONG)kernelTime.dwHighDateTime << DWORD_SHIFT | kernelTime.dwLowDateTime) / FILETIME_TICKS_PER_MILLISECOND_U;
            totalUser = ((ULONGLONG)userTime.dwHighDateTime << DWORD_SHIFT | userTime.dwLowDateTime) / FILETIME_TICKS_PER_MILLISECOND_U; }

        cursor = LogPut(cursor, "\r\nSTAGE2: host cpu ms: process user="); cursor = LogDecimal(cursor, (DWORD)previousUser);
        cursor = LogPut(cursor, " kernel="); cursor = LogDecimal(cursor, (DWORD)previousKernel);
        cursor = LogPut(cursor, " | guest thread user="); cursor = LogDecimal(cursor, (DWORD)totalUser);
        cursor = LogPut(cursor, " kernel="); cursor = LogDecimal(cursor, (DWORD)totalKernel);
        cursor = LogPut(cursor, " | run_ms=");
        cursor = LogDecimal(cursor, GetTickCount() - g_RunStartTick); }
    return cursor;
}

/* End of run: VBE 4F07h retrace waits and the guest's busiest 3DAh polling sites. */
static PSTR ReportRetracePolling(PSTR cursor)
{
    cursor = LogPut(cursor, "\r\nSTAGE2: VBE 4F07h retrace waits="); cursor = LogDecimal(cursor, g_VbeWaits);           /* #226 */
    {   INT item;

        for (item = 0; item < RT_SITES && g_RetraceSite[item].Count; ++item)
        {
            cursor = LogPut(cursor, "\r\nSTAGE2: 3DAh site "); cursor = LogHex(cursor, g_RetraceSite[item].Cs); cursor = LogPut(cursor, ":");
            cursor = LogHex(cursor, g_RetraceSite[item].Ip); cursor = LogPut(cursor, " n="); cursor = LogDecimal(cursor, g_RetraceSite[item].Count);
            cursor = LogPut(cursor, " next="); cursor = LogDump(cursor, (const VOID *)g_RetraceSite[item].Bytes, 10);
        } }

    return cursor;
}

/* End of run: the host CPU's clock and the CPU-speed governor -- duty, delivered share, holds, debt, burst granularity and affinity. */
static PSTR ReportCpuSpeedGovernor(PSTR cursor)
{
    cursor = LogPut(cursor, "\r\nSTAGE2: host cpu ~MHz="); cursor = LogDecimal(cursor, HostCpuMhz());   /* #224 */
    cursor = LogPut(cursor, "\r\nSTAGE2: cpuspeed idx="); cursor = LogHex(cursor, (DWORD)g_CpuSpeedIndex);
    cursor = LogPut(cursor, " mhz="); cursor = LogHex(cursor, g_CpuSpeedIndex < CPUSPEED_COUNT
                                      ? g_CpuSpeedMhz[g_CpuSpeedIndex] : 0u);
    cursor = LogPut(cursor, " ref_mhz="); cursor = LogHex(cursor, g_CpuSpeedReferenceMhz);
    /* REQUESTED vs DELIVERED, BOTH MEASURED, AND NOW THEY AGREE BY DESIGN.
     * The throttle's contract is that guest execution is `duty` of wall time.
     * `delivered_bp` is that ratio as it actually came out -- lifetime guest
     * EXECUTION over lifetime WALL -- and it equals `duty_bp` whenever the
     * setting is reachable (below the port-trap ceiling and inside the hold cap),
     * disagreeing in the open when it is not. The control law that ties them
     * together is CpuSpeedStep, proven by a DETERMINISTIC test off-hardware
     * (tests/unit/cpuspeed_test.c) rather than argued from a test-machine number read
     * through the guest's own throttled clock.
     *
     * [INFO]: run_ms IS TRUE GUEST EXECUTION now: dexec is sampled resume-to-suspend, so
     * holds (before the resume) and host-servicing (outside VdmRunGuest) are already
     * out of it. That is why the old execnet/held-subtraction dance is gone --
     * the number is clean at the source instead of patched at the report.
     */
    cursor = LogPut(cursor, " duty_bp="); cursor = LogHex(cursor, (DWORD)g_CpuSpeedDuty);
    cursor = LogPut(cursor, " duty_rm_bp="); cursor = LogHex(cursor, (DWORD)g_CpuSpeedDutyRm);   /* #225 */
    { DWORD wallMs = GetTickCount() - g_StartMs;
      /* Units cancel in the ratio, so ms goes straight in. */
      cursor = LogPut(cursor, " delivered_bp=");
      cursor = LogHex(cursor, CpuSpeedDeliveredBp(g_CpuSpeedRunMs, wallMs));
      cursor = LogPut(cursor, " exec_ms="); cursor = LogHex(cursor, g_CpuSpeedRunMs);
      cursor = LogPut(cursor, " wall_ms=");
      cursor = LogHex(cursor, wallMs); }
    cursor = LogPut(cursor, " held_ms="); cursor = LogHex(cursor, g_CpuSpeedHeldMs);
    cursor = LogPut(cursor, " hold_max_us="); cursor = LogHex(cursor, g_CpuSpeedHoldMaximumMicroseconds);   /* #225 */
    cursor = LogPut(cursor, " debt_max_us="); cursor = LogHex(cursor, g_CpuSpeedDebtMaximumMicroseconds);
    cursor = LogPut(cursor, " coop="); cursor = LogHex(cursor, g_CpuSpeedCooperativeCatches);        /* #225 */
    cursor = LogPut(cursor, " coop_to="); cursor = LogHex(cursor, g_CpuSpeedCooperativeTimeouts);
    cursor = LogPut(cursor, " ran_us="); cursor = LogHex(cursor, g_CpuSpeedRanMicroseconds);
    cursor = LogPut(cursor, " win_wall_us="); cursor = LogHex(cursor, g_CpuSpeedWallMicroseconds);
    cursor = LogPut(cursor, " missed="); cursor = LogHex(cursor, g_CpuSpeedMissed);
    /* GRANULARITY = BURST SIZE = playable vs slideshow. `periods` over the run's
     * seconds is how many bursts a second the guest advanced in; seven was the
     * "still unplayable" number. gran=0 means auto chose period_ms from rt_us.
     */
    cursor = LogPut(cursor, " gran_ms="); cursor = LogHex(cursor, g_CpuSpeedGranularityMs);
    cursor = LogPut(cursor, " period_ms="); cursor = LogHex(cursor, g_CpuSpeedPeriodMs);
    cursor = LogPut(cursor, " rt_us="); cursor = LogHex(cursor, g_CpuSpeedRoundTripMicroseconds);
    cursor = LogPut(cursor, " periods="); cursor = LogHex(cursor, g_CpuSpeedPeriods);
    cursor = LogPut(cursor, " aff="); cursor = LogHex(cursor, (DWORD)g_CpuAffinityOn);
    cursor = LogPut(cursor, " ncpu="); cursor = LogHex(cursor, g_CpuAffinityCpuCount);
    return cursor;
}

/* End of run: delivery latencies -- INT 33h callbacks, keyboard messages to the guest, and the gaps between timer ticks. */
static PSTR ReportDeliveryLatencies(PSTR cursor)
{
    /* The INT 33h callback path, at EXIT (the heartbeat's copy is a snapshot). */
    cursor = LogPut(cursor, "\r\nSTAGE2: MOUSECB inj="); cursor = LogHex(cursor, g_MouseCallbackInjected);
    cursor = LogPut(cursor, " done=");  cursor = LogHex(cursor, g_MouseCallbackDone);
    cursor = LogPut(cursor, " lost=");  cursor = LogHex(cursor, g_MouseCallbackLost);
    cursor = LogPut(cursor, " stray="); cursor = LogHex(cursor, g_MouseCallbackStray);
    cursor = LogPut(cursor, " pm=");    cursor = LogHex(cursor, g_MouseCallbackPm);
    cursor = LogPut(cursor, " active="); cursor = LogHex(cursor, (DWORD)g_MouseCallbackActive);
    cursor = LogPut(cursor, " raised="); cursor = LogHex(cursor, g_MouseEventRaised);
    cursor = LogPut(cursor, " why[fly,none,nohdl,stub,if]=");
    {
        INT reason;

        for (reason = 0; reason < MOUSE_CB_WHY_COUNT; ++reason)
        {
            cursor = LogHex(cursor, g_MouseCallbackWhy[reason]);
            cursor = LogPut(cursor, reason < MOUSE_CB_WHY_COUNT - 1 ? "," : "");
        }
    }
    cursor = LogPut(cursor, " mask=0x"); cursor = LogHex(cursor, (DWORD)g_MouseEventMask);
    cursor = LogPut(cursor, " hdl=0x");  cursor = LogHex(cursor, (DWORD)g_MouseEventSegment);
    cursor = LogPut(cursor, ":0x");      cursor = LogHex(cursor, (DWORD)g_MouseEventOffset);
    /* THE KEYSTROKE ITSELF, both halves. ms buckets [0,1,2,4,8,16,32,64+]. */
    cursor = LogPut(cursor, "\r\nSTAGE2: KEYLAT msgq_ms[0,1,2,4,8,16,32,64+]=");
    { UINT bucket5;

    for (bucket5 = 0; bucket5 < 8; ++bucket5) { cursor = LogPut(cursor, bucket5 ? "," : "");
                                                cursor = LogHex(cursor, g_KeyMessageHistogram[bucket5]); } }

    cursor = LogPut(cursor, " n="); cursor = LogHex(cursor, g_KeyMessageCount);
    cursor = LogPut(cursor, " max_ms="); cursor = LogHex(cursor, g_KeyMessageMaximumMs);
    cursor = LogPut(cursor, " || deliver_ms[0,1,2,4,8,16,32,64+]=");
    { UINT bucket5;

    for (bucket5 = 0; bucket5 < 8; ++bucket5) { cursor = LogPut(cursor, bucket5 ? "," : "");
                                                cursor = LogHex(cursor, g_KeyDeliveryHistogram[bucket5]); } }

    cursor = LogPut(cursor, " n="); cursor = LogHex(cursor, g_KeyDeliveryCount);
    cursor = LogPut(cursor, " max_ms="); cursor = LogHex(cursor, g_KeyDeliveryMaximumMs);
    cursor = LogPut(cursor, "\r\nSTAGE2: TICKGAP us[<.5k,1k,2k,4k,8k,16k,32k,64k,128k,256k,512k,+]=");
    { UINT bucket7;

    for (bucket7 = 0; bucket7 < 12; ++bucket7) { cursor = LogPut(cursor, bucket7 ? "," : "");
                                                 cursor = LogHex(cursor, g_TickGap[bucket7]); } }

    cursor = LogPut(cursor, " max_us="); cursor = LogHex(cursor, g_TickGapMaximumMicroseconds);
    cursor = LogPut(cursor, " OVER_11600us="); cursor = LogHex(cursor, g_TickGapOver);
    return cursor;
}

/* End of run: the DMX mixer task, IRQs raised by the WOW shims, and the SB DSP version the guest asked for. */
static PSTR ReportDmxTaskAndShimIrqs(PSTR cursor)
{
    cursor = LogPut(cursor, "\r\nSTAGE2: DMXTASK: ok="); cursor = LogHex(cursor, g_DmxMixerOk);
    cursor = LogPut(cursor, " samples="); cursor = LogHex(cursor, g_DmxSamples);
    cursor = LogPut(cursor, " any_busy="); cursor = LogHex(cursor, g_DmxAnyBusy);
    cursor = LogPut(cursor, " mixer_OVERDUE="); cursor = LogHex(cursor, g_DmxOverdue);
    cursor = LogPut(cursor, " max_late_ticks="); cursor = LogHex(cursor, g_DmxOverdueMaximum);
    cursor = LogPut(cursor, " busy_by_task=");
    { UINT timelineIndex;

    for (timelineIndex = 0; timelineIndex < 12; ++timelineIndex) { cursor = LogPut(cursor, timelineIndex ? "," : "");
                                                 cursor = LogHex(cursor, g_DmxBusy[timelineIndex]); } }

    cursor = LogPut(cursor, "\r\nSTAGE2: ica (shim-raised IRQs, #278) raised="); cursor = LogHex(cursor, g_IcaRaised);
    cursor = LogPut(cursor, " delivered="); cursor = LogHex(cursor, g_IcaDelivered);
    cursor = LogPut(cursor, " nohandler="); cursor = LogHex(cursor, g_IcaNoHandler);
    cursor = LogPut(cursor, " idlewaits(#306)="); cursor = LogHex(cursor, g_WowIdleWaits);
    cursor = LogPut(cursor, " shims[WOW32.DLL,NTVDM.EXE]=["); cursor = LogHex(cursor, g_ShimState[0]);
    cursor = LogPut(cursor, ","); cursor = LogHex(cursor, g_ShimState[1]); cursor = LogPut(cursor, "] err=[");
    cursor = LogHex(cursor, g_ShimError[0]); cursor = LogPut(cursor, ","); cursor = LogHex(cursor, g_ShimError[1]); cursor = LogPut(cursor, "]");
    cursor = LogPut(cursor, " (1 loaded, 2 not found, 3 init refused)");
    cursor = LogPut(cursor, "\r\nSTAGE2: dspver="); cursor = LogHex(cursor, (DWORD)g_SbVersionMajor);
    cursor = LogPut(cursor, "."); cursor = LogHex(cursor, (DWORD)g_SbVersionMinor);
    cursor = LogPut(cursor, " execprio="); cursor = LogHex(cursor, g_ExecPriority);
    return cursor;
}

/* End of run: the host lock -- waits, holds and the sites that held it longest. */
static PSTR ReportHostLock(PSTR cursor)
{
    cursor = LogPut(cursor, "\r\nSTAGE2: lock: wait_us=0x");  cursor = LogHex(cursor, g_LockWaitMicroseconds);
    cursor = LogPut(cursor, "@line ");                        cursor = LogHex(cursor, (DWORD)g_LockWaitSite);
    cursor = LogPut(cursor, " hold_us=0x");                   cursor = LogHex(cursor, g_LockHoldMicroseconds);
    cursor = LogPut(cursor, "@line ");                        cursor = LogHex(cursor, (DWORD)g_LockHoldSite);
    cursor = LogPut(cursor, " ui_gap_us=0x");                 cursor = LogHex(cursor, g_UiGapMicroseconds);
    /* ty_sent = typematic repeats WE generated; ty_os = OS auto-repeats we
     * suppressed because we generate our own. If ty_os is large and ty_sent is
     * small, the pump is not running; if both are small while a key was held,
     * the OS was not delivering repeats either -- which is what started this.
     */
    cursor = LogPut(cursor, " ty_sent=0x");                   cursor = LogHex(cursor, g_TypematicSent);
    cursor = LogPut(cursor, " ty_os=0x");                     cursor = LogHex(cursor, g_TypematicOsRepeats);
    /* The XP setting we derived the rate from, raw and in microseconds, so a run
     * says WHY it repeats at the speed it does. Verify against stock ntvdm with
     * tests/probes/dos/tymat.asm: the target is delay 7 ticks / 102 repeats.
     */
    cursor = LogPut(cursor, " spi_delay=0x");                 cursor = LogHex(cursor, g_TypematicSpiDelay);
    cursor = LogPut(cursor, " spi_speed=0x");                 cursor = LogHex(cursor, g_TypematicSpiSpeed);
    cursor = LogPut(cursor, " ty_delay_us=0x");               cursor = LogHex(cursor, g_TypematicDelayMicroseconds);
    cursor = LogPut(cursor, " ty_period_us=0x");              cursor = LogHex(cursor, g_TypematicPeriodMicroseconds);
    return cursor;
}

/* End of run: the 8042's port traffic, and every font the guest asked INT 10h for, with its first bytes. */
static PSTR ReportKeyboardControllerAndFontQueries(PSTR cursor, PSTR const base)
{
    /* Does the guest set its OWN typematic rate? If it does, ours is a guess and
     * should be taken from its 0xF3 byte instead (bits 0-4 rate, 5-6 delay).
     */
    cursor = LogPut(cursor, "\r\nSTAGE2: kbd 8042: writes=0x"); cursor = LogHex(cursor, g_Input.KeyboardPortWrites);
    cursor = LogPut(cursor, " typematic_set=0x");               cursor = LogHexByte(cursor, g_Input.IsTypematicSet);
    cursor = LogPut(cursor, " rate_byte=0x");                   cursor = LogHexByte(cursor, g_Input.TypematicByte);
    cursor = LogPut(cursor, " seq=");
    { UINT item;

    for (item = 0; item < g_Input.KeyboardPortLogCount; ++item)
    {
          cursor = LogPut(cursor, item ? "," : ""); cursor = LogHexByte(cursor, g_Input.KeyboardPortLog[item][0]);
          cursor = LogPut(cursor, ":");
          cursor = LogHexByte(cursor, g_Input.KeyboardPortLog[item][1]); } }

    cursor = LogPut(cursor, " int10_11=0x");   cursor = LogHex(cursor, g_Video.Int10Ah11Calls);
    /* Each font request, its answer, and the BYTES actually sitting at the address we
     * handed back -- read from guest memory, so a wiped or misaligned table is visible
     * rather than inferred. A glyph is mostly zeros with a few set rows; all-zero or
     * all-FF here means the caller is drawing from the wrong place.
     */
    { INT fontIndex;

    for (fontIndex = 0; fontIndex < g_Video.FontQueryCount; ++fontIndex)
    {
        const volatile BYTE *fontPointer;
        cursor = LogPut(cursor, "\r\n  font_q: AL=0x"); cursor = LogHex(cursor, g_Video.FontQueries[fontIndex].Al);
        cursor = LogPut(cursor, " BH=0x");   cursor = LogHex(cursor, g_Video.FontQueries[fontIndex].Bh);
        cursor = LogPut(cursor, " -> ES:BP=0x"); cursor = LogHex(cursor, g_Video.FontQueries[fontIndex].Segment);
        cursor = LogPut(cursor, ":0x");      cursor = LogHex(cursor, g_Video.FontQueries[fontIndex].Offset);
        cursor = LogPut(cursor, " CX=0x");   cursor = LogHex(cursor, g_Video.FontQueries[fontIndex].Cx);
        fontPointer = (const volatile BYTE *)(((DWORD)g_Video.FontQueries[fontIndex].Segment << PARAGRAPH_SHIFT)
                                     + g_Video.FontQueries[fontIndex].Offset);
        { BYTE fontBytes[16];
        UINT item;

        for (item = 0; item < 16; ++item)
            fontBytes[item] = fontPointer[item];

          cursor = LogPut(cursor, " bytes: ");
          cursor = LogDump(cursor, fontBytes, 16); }
    } }

    cursor = LogPut(cursor, "\r\n");
    LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
    return cursor;
}

/* End of run: the exec loop's event counts, then the guest's clock as a shape -- IRQ0 per second, the gaps between ticks and why ticks were held, PM cooperation and the tick courier. */
static PSTR ReportExecEventsAndIrq0Timeline(PSTR cursor, PSTR const base)
{
    cursor = LogPut(cursor, "STAGE2: io_events=0x");  cursor = LogHex(cursor, g_EventIo);
    cursor = LogPut(cursor, " io_burst=0x");          cursor = LogHex(cursor, g_IoExtra);
    cursor = LogPut(cursor, " irq0_inj=0x");          cursor = LogHex(cursor, g_Irq0Injected);
    /* THE GUEST'S CLOCK AS A SHAPE, NOT A TOTAL. See Irq0DeliveredNote.
     * IRQ0TL is deliveries in each whole second: read it as a sequence and a
     * slow-down/speed-up is the sequence moving. IRQ0GAP is the inter-delivery
     * interval histogram, which says whether that is stalls or jitter. Both are
     * DECIMAL-in-hex like every other counter here.
     */
    {   UINT timelineSecond;
    DWORD last = 0;

        for (timelineSecond = 0; timelineSecond < IRQ0TL_SECS; ++timelineSecond)
            if (g_Irq0TimeLast[timelineSecond])
                last = timelineSecond;

        cursor = LogPut(cursor, "\r\nSTAGE2: IRQ0TL persec=");

        for (timelineSecond = 0; timelineSecond <= last && timelineSecond < IRQ0TL_SECS; ++timelineSecond)
        {
            cursor = LogPut(cursor, timelineSecond ? "," : ""); cursor = LogHex(cursor, g_Irq0TimeLast[timelineSecond]);

            if (cursor - base > 3600)
            {
                LogAppend(LOG_PATH, base, cursor);
                cursor = base;
            }
        }

        cursor = LogPut(cursor, "\r\nSTAGE2: IRQ0GAP ms[<1,1,2,4,8,16,32,64+]=");

        for (timelineSecond = 0; timelineSecond < 8; ++timelineSecond) { cursor = LogPut(cursor, timelineSecond ? "," : "");
                                  cursor = LogHex(cursor, g_Irq0GapHistogram[timelineSecond]); }

        cursor = LogPut(cursor, " n="); cursor = LogHex(cursor, g_Irq0GapCount);
        cursor = LogPut(cursor, " max_ms="); cursor = LogHex(cursor, g_Irq0GapMaximumMs);
        /* IS THE MUSIC ISR THE CULPRIT? io-per-MILLISECOND, anomalous vs normal.
         * A RATE, because a longer gap collects more I/O whatever the guest is
         * doing -- comparing io-per-GAP across populations of different length is
         * the confound that produced a refutation this run reverses. If anom_io_pms
         * is many times norm_io_pms the long gaps really are the guest hammering
         * ports (a trapped-I/O overrun) and the fix is the port cost; if they are
         * similar the search moves on. worst = the single biggest gap, its I/O, and
         * the CS:IP where the guest re-opened interrupts (cs=0xffff means async).
         */
        cursor = LogPut(cursor, " anom_n="); cursor = LogHex(cursor, g_Irq0AnomalyCount);
        cursor = LogPut(cursor, " anom_ms="); cursor = LogHex(cursor, g_Irq0AnomalyMicroseconds / MICROSECONDS_PER_MILLISECOND_U);
        cursor = LogPut(cursor, " anom_io_pms=");
        cursor = LogHex(cursor, g_Irq0AnomalyMicroseconds ? g_Irq0AnomalyIo / (g_Irq0AnomalyMicroseconds / MICROSECONDS_PER_MILLISECOND_U ? g_Irq0AnomalyMicroseconds / MICROSECONDS_PER_MILLISECOND_U : 1u) : 0u);
        cursor = LogPut(cursor, " norm_n="); cursor = LogHex(cursor, g_Irq0NormalCount);
        cursor = LogPut(cursor, " norm_ms="); cursor = LogHex(cursor, g_Irq0NormalMicroseconds / MICROSECONDS_PER_MILLISECOND_U);
        cursor = LogPut(cursor, " norm_io_pms=");
        cursor = LogHex(cursor, g_Irq0NormalMicroseconds ? g_Irq0NormalIo / (g_Irq0NormalMicroseconds / MICROSECONDS_PER_MILLISECOND_U ? g_Irq0NormalMicroseconds / MICROSECONDS_PER_MILLISECOND_U : 1u) : 0u);
        cursor = LogPut(cursor, " worst_ms="); cursor = LogHex(cursor, g_Irq0WorstGapMs);
        cursor = LogPut(cursor, " worst_io="); cursor = LogHex(cursor, g_Irq0WorstGapIo);
        cursor = LogPut(cursor, " worst_per_us="); cursor = LogHex(cursor, g_Irq0WorstPerMicroseconds);
        cursor = LogPut(cursor, " worst_cs="); cursor = LogHex(cursor, g_Irq0WorstCs);
        cursor = LogPut(cursor, " worst_ip="); cursor = LogHex(cursor, g_Irq0WorstIp);
        /* THE A/B/C DISCRIMINATOR. See Irq0DeliveredNote for how to read it.
         * Deltas summed over ANOMALOUS gaps only (>= 2 of the period the guest
         * itself programmed), so they are meaningful divided by anom_n above.
         * raise  IRQ0s the 8254 generated inside the gap
         * att    async attempts actually made        (raise-att = attempts stolen)
         * nie    attempts that bailed not_in_exec    (high => A, delivery)
         * yld    attempts handed to a pending KEY    (high => C, the yield)
         *
         * [INFO]: gen/del is the answer with no threshold in it: `del` counts anomalous
         * gaps that contained >= 2 raises (the ticks existed and we lost them --
         * A), `gen` counts those that did not (the clock itself stalled -- B).
         * Whichever dominates names the fix. wst_* are the same four for the single
         * worst gap, beside the period that was in force for it.
         */
        cursor = LogPut(cursor, "\r\nSTAGE2: IRQ0WHY gen="); cursor = LogHex(cursor, g_Irq0AnomalyGeneration);
        cursor = LogPut(cursor, " del="); cursor = LogHex(cursor, g_Irq0AnomalyDelete);
        cursor = LogPut(cursor, " anom[raise,att,nie,yld]=");
        cursor = LogHex(cursor, g_Irq0AnomalyRaise); cursor = LogPut(cursor, ",");
        cursor = LogHex(cursor, g_Irq0AnomalyAttempts);   cursor = LogPut(cursor, ",");
        cursor = LogHex(cursor, g_Irq0AnomalyNie);   cursor = LogPut(cursor, ",");
        cursor = LogHex(cursor, g_Irq0AnomalyYield);
        cursor = LogPut(cursor, " wst[raise,att,nie,yld]=");
        cursor = LogHex(cursor, g_Irq0WorstRaise); cursor = LogPut(cursor, ",");
        cursor = LogHex(cursor, g_Irq0WorstAttempts);   cursor = LogPut(cursor, ",");
        cursor = LogHex(cursor, g_Irq0WorstNie);   cursor = LogPut(cursor, ",");
        cursor = LogHex(cursor, g_Irq0WorstYield);
        /* #172: see g_PmCooperativeGate for the ten columns. */
        cursor = LogPut(cursor, "\r\nSTAGE2: PMCOOP owed>=2 gate[latch,vif,hook,inirq,noirq,async,armed,tried,claim,decl]=");
        {
            INT gateIndex;

            for (gateIndex = 0; gateIndex < PM_GATES; ++gateIndex)
            {
                cursor = LogPut(cursor, gateIndex ? "," : "");
                cursor = LogHex(cursor, g_PmCooperativeGate[gateIndex]);
            }
        }
        cursor = LogPut(cursor, "\r\nSTAGE2: PMINJ decl[cs16,nohook]="); cursor = LogHex(cursor, g_PmInjectDecl[0]);
        cursor = LogPut(cursor, ","); cursor = LogHex(cursor, g_PmInjectDecl[1]);
        { INT timelineSecond, last = -1;

          for (timelineSecond = 0; timelineSecond < IRQ0TL_SECS; ++timelineSecond)
              if (g_PmInjectDeclTl[timelineSecond])
                  last = timelineSecond;

          cursor = LogPut(cursor, " persec=");

          for (timelineSecond = 0; timelineSecond <= last; ++timelineSecond)
          {
              cursor = LogPut(cursor, timelineSecond ? "," : "");
              cursor = LogHex(cursor, g_PmInjectDeclTl[timelineSecond]);
          }
          }
        { INT item;
        cursor = LogPut(cursor, " cs16 sites:");

          for (item = 0; item < PMINJ_SITES && g_PmInjectSite[item].Count; ++item)
          {
              cursor = LogPut(cursor, " "); cursor = LogHex(cursor, g_PmInjectSite[item].Cs); cursor = LogPut(cursor, ":");
              cursor = LogHex(cursor, g_PmInjectSite[item].Eip);
              cursor = LogPut(cursor, "x");
              cursor = LogHex(cursor, g_PmInjectSite[item].Count); } }

        /* WHAT THE TICK COURIER DID. `inj` is the whole point: ticks placed that
         * the raise site had already given away to a key. Read it against IRQ0WHY's
         * yld -- if inj is a large fraction of yld the courier is collecting exactly
         * what the yield spends. `tries` counts suspend round trips (the cost),
         * `giveup` the passes that spent their whole budget without placing, which
         * is the guest legitimately holding interrupts off. courier=0 means the
         * knob turned it off, and every other field here must then read zero.
         */
        cursor = LogPut(cursor, "\r\nSTAGE2: COURIER on="); cursor = LogHex(cursor, (DWORD)g_CourierOn);
        cursor = LogPut(cursor, " wakes="); cursor = LogHex(cursor, g_CourierWakes);
        cursor = LogPut(cursor, " inj=");   cursor = LogHex(cursor, g_CourierInjected);
        cursor = LogPut(cursor, " tries="); cursor = LogHex(cursor, g_CourierTries);
        cursor = LogPut(cursor, " giveup="); cursor = LogHex(cursor, g_CourierGiveUp);
    }
    return cursor;
}

/* End of run: how long V86 string instructions kept the guest, and the loop's remaining event counters. */
static PSTR ReportV86StringTiming(PSTR cursor)
{
    /* V86 STRETCHES: the duration of single VdmRunGuest calls, ms buckets. A big
     * timer gap IS a big stretch here; str_max names where it started (cs:ip) and
     * how it ended (ev). ev 2=I/O, others per the event taxonomy.
     */
    cursor = LogPut(cursor, "\r\nSTAGE2: V86STR ms[<1,1,2,4,8,16,32,64+]=");
    { UINT bucket6;

    for (bucket6 = 0; bucket6 < 8; ++bucket6) { cursor = LogPut(cursor, bucket6 ? "," : "");
                                            cursor = LogHex(cursor, g_V86StringHistogram[bucket6]); } }

    cursor = LogPut(cursor, " n8="); cursor = LogHex(cursor, g_V86StringCount8);
    cursor = LogPut(cursor, " max_ms="); cursor = LogHex(cursor, g_V86StringMaximumMs);
    cursor = LogPut(cursor, " max_cs="); cursor = LogHex(cursor, g_V86StringMaximumCs);
    cursor = LogPut(cursor, " max_ip="); cursor = LogHex(cursor, g_V86StringMaximumIp);
    cursor = LogPut(cursor, " max_ev="); cursor = LogHex(cursor, g_V86StringMaximumEvent);
    cursor = LogPut(cursor, " irq0_skip=0x");         cursor = LogHex(cursor, g_Irq0Skip);
    cursor = LogPut(cursor, " intpend=0x");           cursor = LogHex(cursor, g_EventIntPending);
    cursor = LogPut(cursor, " iostr=0x");             cursor = LogHex(cursor, g_EventIoString);
    cursor = LogPut(cursor, " bda_tick=0x");          cursor = LogHex(cursor, ((DWORD)PeekWord(BIOS_BDA_BASE + BIOS_BDA_TICK_COUNT_HIGH) << WORD_SHIFT) | PeekWord(BIOS_BDA_BASE + BIOS_BDA_TICK_COUNT));
    cursor = LogPut(cursor, "\r\n");
    return cursor;
}

/* End of run: every I/O port the guest touched that no device claimed. */
static PSTR ReportUnclaimedPorts(PSTR cursor)
{
    { INT index;
    cursor = LogPut(cursor, "STAGE2: unclaimed ports touched:");

      for (index = 0; index < g_UnclaimedCount; ++index)
      {
          cursor = LogPut(cursor, " 0x");
          cursor = LogHex(cursor, g_Unclaimed[index]);
      }

      cursor = LogPut(cursor, "\r\n"); }
    return cursor;
}

/* GH #27: one line per class of unimplemented thing the run actually reached,
 * so a run yields a to-do list instead of "the screen looked wrong". Empty
 * lines are printed too -- "INT21 unimplemented:" with nothing after it is a
 * positive statement that nothing was missing, which a suppressed line is not.
 */
static PSTR ReportUnimplemented(PSTR cursor, DOS_MACHINE *machine)
{
    INT index;
    INT count;

    cursor = LogPut(cursor, "STAGE2: INT21 unimplemented:");

    for (index = 0, count = 0; index < BYTE_VALUES; ++index)
        if ((machine->Unimplemented[index >> BITMAP_BYTE_SHIFT] >> (index & BITMAP_BIT_MASK)) & 1u)
        {
            cursor = LogPut(cursor, " AH=0x");
            cursor = LogHexByte(cursor, (UINT)index);
            ++count;
        }

    if (!count)
        cursor = LogPut(cursor, " none");

    cursor = LogPut(cursor, "\r\n");
    cursor = LogPut(cursor, "STAGE2: INT21 undefined-on-6.22 (no-op, matches DOS):");

    for (index = 0, count = 0; index < BYTE_VALUES; ++index)
        if ((machine->Undefined[index >> BITMAP_BYTE_SHIFT] >> (index & BITMAP_BIT_MASK)) & 1u)
        {
            cursor = LogPut(cursor, " AH=0x");
            cursor = LogHexByte(cursor, (UINT)index);
            ++count;
        }

    if (!count)
        cursor = LogPut(cursor, " none");

    cursor = LogPut(cursor, "\r\n");
    cursor = LogPut(cursor, "STAGE2: BIOS partial/unimplemented:");

    for (index = 0, count = 0; index < BYTE_VALUES; ++index)
        if (g_BiosUnimplemented[index])
        {
            cursor = LogPut(cursor, " INT");
            cursor = LogHexByte(cursor, (UINT)index);
            ++count;
        }

    if (!count)
        cursor = LogPut(cursor, " none");

    cursor = LogPut(cursor, "\r\n");
    cursor = LogPut(cursor, "STAGE2: INT10 unimplemented:");

    for (index = 0, count = 0; index < BYTE_VALUES; ++index)
        if (VIDEO_UNIMPLEMENTED_GET(g_Video.UnimplementedFunctions, index))
        {
            cursor = LogPut(cursor, " AH=0x");
            cursor = LogHexByte(cursor, (UINT)index);
            ++count;
        }

    if (!count)
        cursor = LogPut(cursor, " none");

    cursor = LogPut(cursor, "\r\n");
    return cursor;
}

/* End of run: the sound stack end to end -- the OPL profile, the SB capture, the GUS, one summary line, and async injections per IRQ. */
static PSTR ReportSoundStack(PSTR cursor)
{
      /* OPL PROFILE (GH #21): what the guest's music driver actually asks for.
       * A gap the game never uses cannot be why the music sounds flat.
       */
      OplTraceDump();
      /* THE SOUND STACK, END TO END, IN ONE LINE:
       * Every part of this was previously either unreported or spread across three
       * places, and "sound works" was being inferred from the guest not complaining.
       * It answers, in order: did the guest's PCM reach the DMA engine, did the
       * mixer run, did MIDI messages leave the MPU-401, and did the HOST devices
       * actually open -- because a silent run with a happy guest and a silent run
       * with no wave device look identical from the guest's side.
       */
      if (g_Sb.CaptureBuffer && g_Sb.CaptureLength)
      {
        HANDLE configHandle = CreateFileA(SBDUMP_PATH, GENERIC_WRITE, 0, NULL,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

        if (configHandle != INVALID_HANDLE_VALUE)
        {
            DWORD bytesWritten = 0;
            WriteFile(configHandle, g_Sb.CaptureBuffer, g_Sb.CaptureLength, &bytesWritten, NULL);
            CloseHandle(configHandle);
        }

        cursor = LogPut(cursor, "STAGE2: sound: raw PCM capture -> sb.raw, "); cursor = LogHex(cursor, g_Sb.CaptureLength);
        cursor = LogPut(cursor, " bytes\r\n");
    }

    GusReport();
    cursor = LogPut(cursor, "STAGE2: sound: sb_blocks="); cursor = LogHex(cursor, g_Sb.Blocks);
      cursor = LogPut(cursor, " sb_rate=");                 cursor = LogHex(cursor, g_Sb.RateHz);
      cursor = LogPut(cursor, " sb_mode=");                 cursor = LogHex(cursor, (DWORD)g_Sb.TransferMode);
      cursor = LogPut(cursor, " sb_dspwr=");                cursor = LogHex(cursor, g_Sb.DspWrites);
      cursor = LogPut(cursor, " midi_msgs=");               cursor = LogHex(cursor, g_Mpu.MessagesSent);
      cursor = LogPut(cursor, "\r\n");
      cursor = LogPut(cursor, "STAGE2: async per IRQ:");
      { UINT lineIndex;

      for (lineIndex = 0; lineIndex < 8; ++lineIndex)
      {
          cursor = LogPut(cursor, " irq"); cursor = LogHexByte(cursor, lineIndex); cursor = LogPut(cursor, "=");
          cursor = LogHex(cursor, g_AsyncInjectedLine[lineIndex]); } }

    return cursor;
}

/* End of run: the Sound Blaster replay check -- blocks DMX never refilled, played twice -- with the buffer lead beside it. */
static PSTR ReportSbReplay(PSTR cursor, PSTR const base)
{
    /* The retry that did not exist before: every one of these is a block-completion
     * IRQ that would previously have been dropped, and so a 256-byte refill DMX would
     * never have made. Compare `devirq_inj` against the shortfall between sb_blocks
     * and irq05 above -- that is the whole of the fix, in one subtraction.
     */
    /* - THE ECHO, AS A NUMBER, WITH ITS CONTROLLED VARIABLE NEXT TO IT. `replayed`
     * counts blocks >=90% identical to the same ring offsets one lap earlier --
     * blocks DMX never refilled, played twice 186 ms apart. `lead` is the buffers
     * actually queued, i.e. how far ahead of audible we read. Vary one, read the
     * other; if they do not move together the lead is not the cause.
     */
    /* [CAUTION]: FLUSH FIRST. `base` points PAST the preamble, so report[] has well under
     * 8 KB of headroom, and the 24-line sbblk ledger added above eats most of what
     * was left. The first cut of this line was written into the overflow and simply
     * never appeared -- while the line immediately AFTER it did, which reads exactly
     * like "the code did not run" and cost a wasted pair of test-machine runs to tell apart
     * from a stale binary. Two counters in the same basic block cannot disagree
     * about whether they executed; when they appear to, suspect the transport.
     */
    cursor = LogPut(cursor, "\r\n");
    LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
    cursor = LogPut(cursor, "STAGE2: sb replay: blocks_checked="); cursor = LogHex(cursor, g_Sb.BlocksChecked);
    cursor = LogPut(cursor, " REPLAYED=");   cursor = LogHex(cursor, g_Sb.BlocksReplayed);

    if (g_Sb.BlocksChecked)
    {
        cursor = LogPut(cursor, " (");
        cursor = LogHex(cursor, g_Sb.BlocksReplayed * 100u / g_Sb.BlocksChecked);
        cursor = LogPut(cursor, "% of blocks, decimal-in-hex)");
    }

    /* - ...AND HOW MANY OF THOSE BLOCKS CARRIED ANY AUDIO. A silent block is
     * identical to the previous lap when the guest refills it CORRECTLY, so
     * `REPLAYED` on its own cannot support "DMX never refilled it". Only
     * REPLAYED_LOUD can, and its denominator is the non-flat blocks.
     */
    cursor = LogPut(cursor, " flat=");          cursor = LogHex(cursor, g_Sb.BlocksFlat);
    cursor = LogPut(cursor, " REPLAYED_LOUD="); cursor = LogHex(cursor, g_Sb.BlocksReplayedLoud);

    if (g_Sb.BlocksChecked > g_Sb.BlocksFlat)
    {
        cursor = LogPut(cursor, " (");
        cursor = LogHex(cursor, g_Sb.BlocksReplayedLoud * 100u
                    / (g_Sb.BlocksChecked - g_Sb.BlocksFlat));
        cursor = LogPut(cursor, "% of NON-FLAT blocks, decimal-in-hex)");
    }

    cursor = LogPut(cursor, " runs[1,2,3,4-7,8-15,16-31,32-63,64+]=");
    { UINT rateBucket;

    for (rateBucket = 0; rateBucket < 8; ++rateBucket)
        {
            cursor = LogPut(cursor, rateBucket ? "," : "");
            cursor = LogHex(cursor, g_Sb.ReplayRuns[rateBucket]);
        }
        }
    cursor = LogPut(cursor, " run_max="); cursor = LogHex(cursor, g_Sb.ReplayRunMax);
    cursor = LogPut(cursor, " byte_lap_same="); cursor = LogHex(cursor, g_Sb.LapSame);
    cursor = LogPut(cursor, "/");              cursor = LogHex(cursor, g_Sb.LapTotal);
    cursor = LogPut(cursor, " ring=");         cursor = LogHex(cursor, g_Sb.LapLength);
    cursor = LogPut(cursor, " toobig=");       cursor = LogHex(cursor, g_Sb.LapTooBig);
    cursor = LogPut(cursor, " lead_buffers=");  cursor = LogHex(cursor, g_Wave.BufferCount);
    return cursor;
}

/* End of run: simulated interrupts (DPMI 0300h) nobody serviced, by vector. */
static PSTR ReportDpmiSimulatedInterrupts(PSTR cursor, PSTR const base)
{
    /* - WHAT THE BOUNDED REFLECTED-DISPATCH TRACE STOPPED WRITING DOWN. Every INT
     * reflected to a client's own PM handler is counted per (vector, AH) even once
     * the per-pair line budget is spent, and this is where the counts come back.
     * It is also the cheapest answer to "what is this guest actually DOING?" --
     * ZAR's answer is 21/2c x ~170000, i.e. it polls its own clock hook, which no
     * amount of steady-state single-stepping had made obvious. Sorted by nothing:
     * a vector/AH pair is its own name and the numbers are what matter.
     */
    /* Flush first: up to 24 pairs is ~500 bytes and `report` is shared with everything
     * above, which has no bound of its own.
     */
    /* - WHAT THE GUEST ASKED 0300 FOR AND WE DID NOT DO. These counters existed
     * already, but only in the periodic KEYLOG block -- which a headless run never
     * reaches, so they had never once been printed in the logs this project
     * actually reads. `simInt 0x10 x7` was sitting in ZAR's every run for four
     * sessions with nothing to show it. A service that silently does nothing and
     * reports success is this project's most expensive bug shape; the counter for
     * it belongs where the evidence is read.
     */
    cursor = LogPut(cursor, "\r\nSTAGE2: simInt (DPMI 0300) UNHANDLED: total=");
    cursor = LogHex(cursor, g_SimIntUnhandled);
    { UINT sbIndex, any = 0;

      for (sbIndex = 0; sbIndex < IVT_VECTORS; ++sbIndex) if (g_SimIntVector[sbIndex])
      {
          cursor = LogPut(cursor, " int"); cursor = LogHexByte(cursor, (BYTE)sbIndex);
          cursor = LogPut(cursor, "h x");
          cursor = LogHex(cursor, g_SimIntVector[sbIndex]);
          any = 1; }

      if (!any)
          cursor = LogPut(cursor, " (none -- every simulated interrupt was serviced)"); }

    cursor = LogPut(cursor, "\r\n"); LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
    return cursor;
}

/* End of run: PM interrupts reflected to real mode -- the default IRQ handler's reflections and the dispatches by vector and AH. */
static PSTR ReportPmReflectedInterrupts(PSTR cursor, PSTR const base)
{
    cursor = LogPut(cursor, "STAGE2: PM default IRQ handler reflected to the BIOS: ");
    cursor = LogHex(cursor, g_PmIrqReflects);
    cursor = LogPut(cursor, "  -> the guest's real-mode ISR: "); cursor = LogHex(cursor, g_PmIrqRmReflects);
    cursor = LogPut(cursor, " (failed "); cursor = LogHex(cursor, g_PmIrqRmFail); cursor = LogPut(cursor, ")");
    cursor = LogPut(cursor, "\r\n"); LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
    cursor = LogPut(cursor, "STAGE2: PM reflected dispatches (vec/AH=count):");
    { UINT dacValue, dacIndex, shown = 0;

      for (dacValue = 0; dacValue < 256 && shown < 24; ++dacValue)
        for (dacIndex = 0; dacIndex < 256 && shown < 24; ++dacIndex)
        {
            if (!g_PmDispatchCount[dacValue][dacIndex])
                continue;

            cursor = LogPut(cursor, " "); cursor = LogHexByte(cursor, (BYTE)dacValue);
            cursor = LogPut(cursor, "/");  cursor = LogHexByte(cursor, (BYTE)dacIndex);
            cursor = LogPut(cursor, "=");  cursor = LogHex(cursor, g_PmDispatchCount[dacValue][dacIndex]);
            ++shown;
        }

      if (!shown)
          cursor = LogPut(cursor, " none"); }

    return cursor;
}

/* End of run: the PIT budget, IRQ0 delivery and cooperative ticks per IRQ, then the async-injection, CPU-share and IF/VIF reports. */
static PSTR ReportTimerAndInterruptDelivery(PSTR cursor, PSTR const base)
{
    cursor = LogPut(cursor, "\r\nSTAGE2: pit budget: syncs="); cursor = LogHex(cursor, g_PitSyncs);
    cursor = LogPut(cursor, " raises=");    cursor = LogHex(cursor, g_IrqRaised[0]);
    cursor = LogPut(cursor, " attempts=");  cursor = LogHex(cursor, g_PitAsyncAttempts);
    cursor = LogPut(cursor, " delivered="); cursor = LogHex(cursor, g_AsyncInjectedLine[0]);
    cursor = LogPut(cursor, " owed_max=");  cursor = LogHex(cursor, (DWORD)g_PmTickOwedMaximum);
    cursor = LogPut(cursor, " ui_gap_us="); cursor = LogHex(cursor, g_UiGapMicroseconds);
    cursor = LogPut(cursor, "\r\n");
    /* - THE WHOLE DELIVERY ACCOUNT, IN ONE LINE, IN THE UNITS OF THE CLAIM. `raises`
     * is what the 8254 generated; `async` is the SuspendThread arm; `coop` is the
     * PM loop's own injections (the #2b latch plus the catch-up batch). Only the
     * SUM can be compared with `raises`, and the pit budget line above shows only
     * the first of the two -- which is how "we deliver 39% of the ticks Doom asked
     * for" was read off an async-only counter. owed_now is the live depth, and the
     * histogram behind it says how often the backlog was actually deep.
     */
    cursor = LogPut(cursor, "STAGE2: isr08 delivery: raises=");  cursor = LogHex(cursor, g_IrqRaised[0]);
    cursor = LogPut(cursor, " async=");   cursor = LogHex(cursor, g_AsyncInjectedLine[0]);
    cursor = LogPut(cursor, " coop=");    cursor = LogHex(cursor, g_PmCooperativeLine[0]);
    cursor = LogPut(cursor, " TOTAL=");   cursor = LogHex(cursor, g_AsyncInjectedLine[0] + g_PmCooperativeLine[0]);

    if (g_IrqRaised[0])
        { cursor = LogPut(cursor, " (");
          cursor = LogHex(cursor, (g_AsyncInjectedLine[0] + g_PmCooperativeLine[0]) * 100u / g_IrqRaised[0]);
          cursor = LogPut(cursor, "% of raises, decimal-in-hex)"); }

    cursor = LogPut(cursor, " owed_now="); cursor = LogHex(cursor, (DWORD)g_PmTickOwed);
    cursor = LogPut(cursor, " owed_depth_at_sync[0,1,2,3,4-7,8-15,16-31,32-63,64]=");
    { UINT outputBucket;

    for (outputBucket = 0; outputBucket < 9; ++outputBucket)
        {
            cursor = LogPut(cursor, outputBucket ? "," : "");
            cursor = LogHex(cursor, g_PmOwedHistogram[outputBucket]);
        }
        }
    cursor = LogPut(cursor, "\r\nSTAGE2: coop per IRQ:");
    { UINT columnIndex;

    for (columnIndex = 0; columnIndex < 8; ++columnIndex)
        { if (!g_PmCooperativeLine[columnIndex])
            continue;
          cursor = LogPut(cursor, " irq"); cursor = LogHexByte(cursor, columnIndex);
          cursor = LogPut(cursor, "=");
          cursor = LogHex(cursor, g_PmCooperativeLine[columnIndex]); } }

    cursor = LogPut(cursor, "\r\n");
    /* Flushed first, deliberately: `base` points past the preamble, so report[] has
     * well under 8 KB of headroom and the sbblk ledger below eats most of what is
     * left. Then which clause said no, per line (AsyncWhyReport).
     */
    LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
    AsyncWhyReport();
    ExecShareReport();
    IfvReport();   /* IF/VIF census -- see IfvNote */
    return cursor;
}

/* End of run: what the guest read back from the 8237 and what the Sound Blaster output did. */
static PSTR ReportDmaAndSoundBlaster(PSTR cursor)
{
    /* - DOES DMX ASK US WHERE THE PLAY HEAD IS? See the note in vdd_dma.h. A
     * nonzero rd_addr on the SB's channel means every refill decision the guest
     * makes is downstream of our cur_addr, which advances on the audio thread in
     * whatever chunk size waveOut asked for. Zero means that whole family of
     * causes is dead and the refill is driven by the IRQ count alone.
     */
    cursor = LogPut(cursor, "STAGE2: 8237 guest reads: ch1_addr="); cursor = LogHex(cursor, g_Dma.AddressReads[1]);
    cursor = LogPut(cursor, " ch1_count=");  cursor = LogHex(cursor, g_Dma.ChannelCountReads[1]);
    cursor = LogPut(cursor, " ch5_addr=");   cursor = LogHex(cursor, g_Dma.AddressReads[5]);
    cursor = LogPut(cursor, " ch5_count=");  cursor = LogHex(cursor, g_Dma.ChannelCountReads[5]);
    cursor = LogPut(cursor, " status0=");    cursor = LogHex(cursor, g_Dma.StatusReads[0]);
    cursor = LogPut(cursor, " status1=");    cursor = LogHex(cursor, g_Dma.StatusReads[1]);
    /* - THE OUTPUT SIDE. See vdd_sb.h: everything else here measures the RING.
     * `idle` is silence WE inserted because the DSP was un-armed; the run-length
     * buckets say whether that is a scatter of single samples or a gap once per
     * block. `cmd` names which transfer command the guest used -- 0x14 single
     * (stops every block) vs 0x1C/0xBx auto-init (streams).
     */
    cursor = LogPut(cursor, "\r\nSTAGE2: sb OUTPUT: active="); cursor = LogHex(cursor, g_Sb.OutputActive);
    cursor = LogPut(cursor, " idle=");   cursor = LogHex(cursor, g_Sb.OutputIdle);
    cursor = LogPut(cursor, " paused="); cursor = LogHex(cursor, g_Sb.OutputPaused);
    { UINT32 total = g_Sb.OutputActive + g_Sb.OutputIdle + g_Sb.OutputPaused;

      if (total) { cursor = LogPut(cursor, " (");
                 cursor = LogHex(cursor, (g_Sb.OutputIdle + g_Sb.OutputPaused) * 100u / total);
                 cursor = LogPut(cursor, "% of output is inserted silence)"); } }

    cursor = LogPut(cursor, " gap_runs[1,2,4,8,16,32,64,128+]=");
    { INT bucket3;

    for (bucket3 = 0; bucket3 < 8; ++bucket3) { cursor = LogPut(cursor, bucket3 ? "," : "");
                                           cursor = LogHex(cursor, g_Sb.IdleRuns[bucket3]); } }

    /* - ISOLATED silent blocks are the dropouts; long runs are real silence. */
    cursor = LogPut(cursor, " flat_runs[1,2,3,4-7,8-15,16-31,32-63,64+]=");
    { INT bucket2;

    for (bucket2 = 0; bucket2 < 8; ++bucket2) { cursor = LogPut(cursor, bucket2 ? "," : "");
                                           cursor = LogHex(cursor, g_Sb.FlatRuns[bucket2]); } }

    /* - THE QUEUE, WHICH IS WHAT THE SPEAKER ACTUALLY SEES. STARVED>0 means the
     * driver ran out of data and played silence -- an audible gap that no
     * ring-side counter can show. `drain` is the margin: its mass sitting at
     * nbufs-1 is one buffer from silence even when starved reads 0.
     */
    cursor = LogPut(cursor, " QUEUE: starved="); cursor = LogHex(cursor, g_Wave.Starved);
    cursor = LogPut(cursor, " drain_max=");      cursor = LogHex(cursor, g_Wave.DrainMax);
    cursor = LogPut(cursor, " wr_fail=");        cursor = LogHex(cursor, g_Wave.Underruns);
    cursor = LogPut(cursor, " drain_hist=");
    { UINT32 bufferIndex;

    for (bufferIndex = 0; bufferIndex <= g_Wave.BufferCount && bufferIndex <= AUDIO_WAVE_BUFFERS; ++bufferIndex)
    {
          cursor = LogPut(cursor, bufferIndex ? "," : "");
          cursor = LogHex(cursor, g_Wave.DrainHistogram[bufferIndex]); } }

    cursor = LogPut(cursor, " geom: nbufs="); cursor = LogHex(cursor, g_Wave.BufferCount);
    cursor = LogPut(cursor, " nframes=");     cursor = LogHex(cursor, g_Wave.FrameCount);
    cursor = LogPut(cursor, " GATE: on="); cursor = LogHex(cursor, (DWORD)g_Sb.GateMode);
    cursor = LogPut(cursor, " stalled_samples="); cursor = LogHex(cursor, g_Sb.GateStalled);
    cursor = LogPut(cursor, " FORCED="); cursor = LogHex(cursor, g_Sb.GateForced);

    if (g_Sb.OutputActive) { cursor = LogPut(cursor, "(stall=");
        cursor = LogHex(cursor, g_Sb.GateStalled * PER_MILLE_U / g_Sb.OutputActive);
        cursor = LogPut(cursor, " per mille of output)"); }

    cursor = LogPut(cursor, " mix82=");   cursor = LogHex(cursor, g_Sb.Mixer82Reads);
    cursor = LogPut(cursor, " ANSWERED_NO="); cursor = LogHex(cursor, g_Sb.Mixer82Zero);

    if (g_Sb.Mixer82Reads) { cursor = LogPut(cursor, "(");
        cursor = LogHex(cursor, g_Sb.Mixer82Zero * 100u / g_Sb.Mixer82Reads);
        cursor = LogPut(cursor, "% turned away)"); }

    cursor = LogPut(cursor, " rate_hz="); cursor = LogHex(cursor, g_Sb.RateHz);
    cursor = LogPut(cursor, " blk_len="); cursor = LogHex(cursor, g_Sb.BlockLength);
    cursor = LogPut(cursor, " dsp_cmds:");
    { UINT vectorIndex;

    for (vectorIndex = 0; vectorIndex < IVT_VECTORS; ++vectorIndex)
        if (g_Sb.CommandHistogram[vectorIndex]) { cursor = LogPut(cursor, " "); cursor = LogHexByte(cursor, vectorIndex);
                                 cursor = LogPut(cursor, "x");
                                 cursor = LogHex(cursor, g_Sb.CommandHistogram[vectorIndex]); } }

    cursor = LogPut(cursor, "\r\nSTAGE2: sb ");
    /* - THE GUEST ADDRESS OF EVERY DMA-COUNT POLL. Subtract 0x03AEDFEC for the
     * DOOM.EXE file offset and disassemble it.
     */
    cursor = LogPut(cursor, " dma_poll_sites=");
    { UINT pollIndex;

    for (pollIndex = 0; pollIndex < g_DmaPollCount; ++pollIndex)
    {
          cursor = LogPut(cursor, pollIndex ? " " : ""); cursor = LogPut(cursor, "0x"); cursor = LogHex(cursor, g_DmaPollEip[pollIndex]);
          cursor = LogPut(cursor, "x");
          cursor = LogHex(cursor, g_DmaPollHits[pollIndex]); } }

    cursor = LogPut(cursor, " overflow="); cursor = LogHex(cursor, (DWORD)g_DmaPollOverflow);
    /* - THE CALL CHAIN. Subtract 0x03AEDFEC for the DOOM.EXE file offset. */
    cursor = LogPut(cursor, " poll_stack=");
    { UINT pollIndex;

    for (pollIndex = 0; pollIndex < g_PollStackCount; ++pollIndex)
    {
          cursor = LogPut(cursor, pollIndex ? " " : ""); cursor = LogPut(cursor, "0x"); cursor = LogHex(cursor, g_PollStack[pollIndex]);
          cursor = LogPut(cursor, "x");
          cursor = LogHex(cursor, g_PollStackHits[pollIndex]); } }

    cursor = LogPut(cursor, " stkovf="); cursor = LogHex(cursor, (DWORD)g_PollStackOverflow);
    /* - WHY ONLY 56 MIXER RUNS/s: long overruns (a) or bunching (b)? */
    cursor = LogPut(cursor, " poll_gap_us[<1k,2k,4k,8k,16k,32k,64k,128k,256k,+]=");
    { UINT bucket4;

    for (bucket4 = 0; bucket4 < 10; ++bucket4) { cursor = LogPut(cursor, bucket4 ? "," : "");
                                                 cursor = LogHex(cursor, g_PollGap[bucket4]); } }

    cursor = LogPut(cursor, " gap_max_us="); cursor = LogHex(cursor, g_PollGapMaximumMicroseconds);
    return cursor;
}

/* End of run: DMX's mixer task as found in guest memory, and who polled the DMA count and from where. */
static PSTR ReportDmxTaskAndDmaPolls(PSTR cursor)
{
    /* READ DMX'S TASK PERIOD OUT OF THE LIVE GUEST:
     * Everything about the refill rate is inferred from how often the mixer polls:
     * "period 1 tick, overrunning" and "period 2 ticks, working as designed" both
     * fit 58 runs/s and need opposite fixes. The scheduler (DOOM.EXE 0x57224)
     * walks 32-byte task entries -- +0x00 handler, +0x08 PERIOD, +0x14 next_due,
     * +0x1c busy, +0x1e enabled -- so the period is simply there to be read.
     *
     * [CAUTION]: DO NOT COMPUTE THE ADDRESS. Composing virtual->guest through the LE object
     * table produced a table of noise, and the structural scan written to find it
     * instead read unmapped memory and killed the run before STAGE2 finished.
     * - SEARCH FOR THE HANDLER. The mixer's entry is DOOM.EXE file 0x56884, and the
     *   file->guest delta 0x03AEDFEC is verified twice over (DMX's IRQ0 stub and
     *   Doom's keyboard ISR, and DMXCHK re-checks it in-run), so the task entry is
     *   whatever 32 bytes begin with that pointer. No data-address arithmetic at
     *   all, and a hit is self-verifying.
     *   Walk only COMMITTED, READABLE regions and stop 32 bytes short of each one's
     *   end -- that is what the previous attempt got wrong.
     */
    { const DWORD mixer = 0x57224u + 0x03AEDFECu;   /* DMX IRQ0 handler = table[0] */
      const volatile BYTE *stub = (const volatile BYTE *)(ULONG_PTR)0x03b431f0;
      MEMORY_BASIC_INFORMATION memoryInfo;
      /* whole user space */
      ULONG_PTR address = 0x00010000u;
      ULONG_PTR lim = 0x7ff00000u;
      UINT found = 0;
      UINT tries;
      /* [CAUTION]: 0x03b431f0 IS DOOM'S ADDRESS, AND ONLY DOOM'S. Reading it unguarded is
       * the very mistake the note above says the scan got wrong -- made again,
       * one line below the warning. Under Doom the page is mapped and the probe
       * is free; under ANY OTHER GUEST it is not, and the access violation
       * killed the host mid-summary: Skyroads crashed on exit four times over
       * and truncated its own STAGE2 log at `async why irq00`. A Doom-shaped
       * instrument must be inert everywhere else, so ASK FIRST.
       */
      cursor = LogPut(cursor, " DMXCHK=");

      if (MemoryReadable((ULONG_PTR)stub, 5))
          for (tries = 0; tries < 5; ++tries)
              cursor = LogHexByte(cursor, stub[tries]);                                   /* expect 601e060fa0 */
      else
          cursor = LogPut(cursor, "unmapped");   /* not Doom: the probe has nothing to say */

      cursor = LogPut(cursor, " mixer=0x"); cursor = LogHex(cursor, mixer);

      while (address < lim && found < 3)
      {
          ULONG_PTR base;
          ULONG_PTR end;
          ULONG_PTR scan;
          INT readable;

          if (VirtualQuery((LPCVOID)address, &memoryInfo, sizeof memoryInfo) != sizeof memoryInfo)
              break;

          base = (ULONG_PTR)memoryInfo.BaseAddress;
          end = base + memoryInfo.RegionSize;
          readable = (memoryInfo.State == MEM_COMMIT)
                   && (memoryInfo.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY
                                   | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE
                                   | PAGE_EXECUTE_WRITECOPY))
                   && !(memoryInfo.Protect & PAGE_GUARD);

          if (readable)
          {
              for (scan = base; scan + 16u*36u <= end && found < 3; scan += 4)
              {
                  const volatile DWORD *entryWords = (const volatile DWORD *)scan;

                  if (entryWords[0] != mixer)
                      continue;

                  /* [CAUTION]: DUMP RAW, DO NOT INTERPRET. The first attempt read
                   * [+8] as the period and got 140 -- which at a ~135/s clock
                   * means one run a SECOND against 58 observed, so either the
                   * match is spurious or the field offsets are wrong. Print the
                   * bytes and decide offline; a guessed layout is how this
                   * session has already produced two counters that could not
                   * have contradicted themselves. 64 bytes = two table entries,
                   * so a real table shows a second handler pointer at +32.
                   */
                  /* THE DISPATCHER'S HANDLER TABLE:
                   * DOOM.EXE 0x554f4 calls [eax*4 + 0x281ac] with eax = irq*9,
                   * i.e. a 36-byte stride from a base whose ADDRESS is an LE
                   * fixup and therefore absent from the image. But entry 0 is
                   * DMX's IRQ0 handler and that address IS known -- so the base
                   * is wherever that pointer lies, and every other IRQ's handler
                   * follows at +36. No address arithmetic, self-verifying.
                   * IRQ5 is the Sound Blaster: 81 block IRQs a second are
                   * delivered and only 58 reach the mixer, so its handler is
                   * what decides the missing 28%. Subtract 0x03AEDFEC from the
                   * printed value for the file offset to disassemble.
                   */
                  UINT irqIndex;

                  if (scan + 16u * 36u > end)
                      continue;                           /* table must fit */

                  ++found;
                  cursor = LogPut(cursor, " IRQTAB@0x"); cursor = LogHex(cursor, (DWORD)scan);

                  for (irqIndex = 0; irqIndex < 16; ++irqIndex)
                  {
                      DWORD handle = *(const volatile DWORD *)(scan + irqIndex * 36u);

                      if (!handle)
                          continue;

                      cursor = LogPut(cursor, " i"); cursor = LogHexByte(cursor, irqIndex);
                      cursor = LogPut(cursor, "=0x"); cursor = LogHex(cursor, handle);
                  }
              }
          }

          if (end <= base)
              break;

          address = end;
      }

      if (!found)
          cursor = LogPut(cursor, " IRQTAB-NOT-FOUND");

      /* THE CALLBACK BETWEEN THE SB ISR AND THE MIXER:
       * DMX's SB handler ends by calling through a pointer stored at data 0x584
       * (the value read back there is a code address), and that callback is
       * where the 28% goes: the ISR runs 86/s (mix82 reads ==
       * blocks) but the mixer is entered 58/s. Its address is runtime data, and
       * the code virtual->file map is the one map still unknown.
       * But the IRQ table settles the addressing: the pointers IN it are LINEAR
       * (entry 0 read back as 0x03b45210, exactly file 0x57224 + 0x03AEDFEC),
       * while data operands like 0x281ac needed +0x03BA0000 to be found. So CS
       * is flat at base 0 and DS is not -- and a code pointer STORED in data is
       * therefore directly a linear address. Read it and subtract 0x03AEDFEC.
       * Also read DMX's own state word at data 0x26370, which its ISR compares
       * against 1 before doing anything at all.
       */
      { const volatile DWORD *counterBytes  = (const volatile DWORD *)(ULONG_PTR)(0x584u   + 0x03BA0000u);
        const volatile DWORD *stateWord = (const volatile DWORD *)(ULONG_PTR)(0x26370u + 0x03BA0000u);
        MEMORY_BASIC_INFORMATION stateMemoryInfo;

        if (VirtualQuery((LPCVOID)counterBytes, &stateMemoryInfo, sizeof stateMemoryInfo) == sizeof stateMemoryInfo && stateMemoryInfo.State == MEM_COMMIT)
        {
            DWORD number = *counterBytes;
            cursor = LogPut(cursor, " CB[0x584]=0x"); cursor = LogHex(cursor, number);

            if (number > 0x03AEDFECu && number < 0x03AEDFECu + 0x45000u)
            {
                cursor = LogPut(cursor, "=file0x"); cursor = LogHex(cursor, number - 0x03AEDFECu);
            }
            else
                cursor = LogPut(cursor, "(not a linear code addr)");
        }

        if (VirtualQuery((LPCVOID)stateWord, &stateMemoryInfo, sizeof stateMemoryInfo) == sizeof stateMemoryInfo && stateMemoryInfo.State == MEM_COMMIT)
        {
            cursor = LogPut(cursor, " STATE[0x26370]="); cursor = LogHex(cursor, *stateWord);
        } } }

    cursor = LogPut(cursor, " count_rd_by_width w1="); cursor = LogHex(cursor, g_Dma.CountReadsByte);
    cursor = LogPut(cursor, " w2=");                   cursor = LogHex(cursor, g_Dma.CountReadsWord);
    cursor = LogPut(cursor, " w4=");                   cursor = LogHex(cursor, g_Dma.CountReadsDword);
    /* ...and how many of those reads DMX made from inside a COOPERATIVE tick. */
    cursor = LogPut(cursor, " from_coop_isr08=");      cursor = LogHex(cursor, g_CooperativeDmaPolls);
    { UINT quadrant;

    for (quadrant = 2; quadrant < 8; ++quadrant)
        { if (!g_CooperativeDmaPollsDevice[quadrant])
            continue;
          cursor = LogPut(cursor, " from_coop_irq"); cursor = LogHexByte(cursor, quadrant);
          cursor = LogPut(cursor, "=");
          cursor = LogHex(cursor, g_CooperativeDmaPollsDevice[quadrant]); } }

    cursor = LogPut(cursor, " in_async_isr=");  cursor = LogHex(cursor, g_DmaPollInAsync);
    cursor = LogPut(cursor, " mainline=");      cursor = LogHex(cursor, g_DmaPollMainline);
    return cursor;
}

/* End of run: device-IRQ retries, the second sound line and the per-block SB ledger. */
static PSTR ReportDeviceIrqRetriesAndSoundBlocks(
    PSTR cursor,
    PSTR const base,
    PCSTR const reportEnd)
{
    cursor = LogPut(cursor, "\r\nSTAGE2: devirq async retry (one per sync): try=");
    cursor = LogHex(cursor, g_IrqNRetryTry);
    cursor = LogPut(cursor, " ok="); cursor = LogHex(cursor, g_IrqNRetryOk);
    cursor = LogPut(cursor, " (0/0 = every device IRQ landed at its raise instant)");
    cursor = LogPut(cursor, "\r\nSTAGE2: devirq (cooperative PM retry): inj=");
    cursor = LogHex(cursor, g_PmDeviceIrqInjected);
    cursor = LogPut(cursor, " fail=");  cursor = LogHex(cursor, g_PmDeviceIrqFail);
    cursor = LogPut(cursor, " dropped_unhooked="); cursor = LogHex(cursor, g_PmDeviceIrqDrop);
    cursor = LogPut(cursor, "\r\nSTAGE2: sound2: ");
    cursor = LogPut(cursor, " mpu_uart=");                cursor = LogHex(cursor, (DWORD)g_Mpu.IsUartMode);
    cursor = LogPut(cursor, " host_wave=");               cursor = LogPut(cursor, g_Wave.IsSilent ? "SILENT" : "open");
    cursor = LogPut(cursor, " host_midi=");               cursor = LogPut(cursor, g_Wave.MidiOut ? "open" : "NONE");
    cursor = LogPut(cursor, " underruns=");               cursor = LogHex(cursor, g_Wave.Underruns);
    cursor = LogPut(cursor, "\r\n");
    /* - THE BLOCK-BOUNDARY LEDGER (see the SB_BLOCK_RECORD comment in vdd_sb.h). One line
     * per completed block for the first few: where the capture stood, what the
     * 8237 held, and whether it had wrapped. cap_off is the load-bearing column --
     * it turns sbref.py's INFERRED 128-frame grid into measured boundaries, so
     * "the jump is two frames in" can be checked against fact instead of against
     * our own guess at where a block starts.
     */
    if (g_Sb.BlockLogCount)
    {
        UINT32 blockIndex;
        cursor = LogPut(cursor, "STAGE2: sound blocks: n="); cursor = LogHex(cursor, g_Sb.Blocks);
        cursor = LogPut(cursor, " logged="); cursor = LogHex(cursor, g_Sb.BlockLogCount);
        cursor = LogPut(cursor, " (cap_off block_len phys count base_addr base_count page mode)\r\n");

        for (blockIndex = 0; blockIndex < g_Sb.BlockLogCount && blockIndex < SB_BLOCK_LOG_MAX; ++blockIndex)
        {
            const SB_BLOCK_RECORD *blockRecord = &g_Sb.BlockLog[blockIndex];
            cursor = LogPut(cursor, "STAGE2: sbblk "); cursor = LogHexByte(cursor, blockIndex);
            cursor = LogPut(cursor, " cap_off=");    cursor = LogHex(cursor, blockRecord->CaptureOffset);
            cursor = LogPut(cursor, " blk_len=");    cursor = LogHex(cursor, blockRecord->BlockLength);
            cursor = LogPut(cursor, " phys=");       cursor = LogHex(cursor, blockRecord->Physical);
            cursor = LogPut(cursor, " count=");      cursor = LogHex(cursor, blockRecord->CurrentCount);
            cursor = LogPut(cursor, " base=");       cursor = LogHex(cursor, blockRecord->BaseAddress);
            cursor = LogPut(cursor, "/");            cursor = LogHex(cursor, blockRecord->BaseCount);
            cursor = LogPut(cursor, " page=");       cursor = LogHexByte(cursor, blockRecord->Page);
            cursor = LogPut(cursor, " mode=");       cursor = LogHexByte(cursor, blockRecord->Mode);
            cursor = LogPut(cursor, blockRecord->Reloaded ? " WRAPPED" : " mid-ring");

            if (blockRecord->Ended)
                cursor = LogPut(cursor, " ENDED");

            cursor = LogPut(cursor, "\r\n");
            /* `base` points PAST the preamble, not at report[0], so bound against
             * the array itself -- p - base would let this overrun by the preamble's
             * length.
             */
            if (cursor > reportEnd - 512)
            {
                LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
            }
        }

        /* Hand the rest of STAGE2 an empty buffer: the ledger is the biggest single
         * block in this report and everything after it was running on fumes.
         */
        LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
    }

    return cursor;
}

/* End of run: the OPL trace and profile, device events, the PC speaker, the wave output and the OPL rhythm section. */
static PSTR ReportAudioDevices(PSTR cursor)
{
        cursor = LogPut(cursor, "STAGE2: opl: trace=");   cursor = LogHex(cursor, g_OplTraceCount);
        cursor = LogPut(cursor, " tdrop=");               cursor = LogHex(cursor, g_OplTraceDrop);
        cursor = LogPut(cursor, "\r\n");
        /* THE SPEAKER PATH, END TO END, IN ONE LINE. "I heard nothing" has four
         * causes and until now no log told them apart: the guest's port writes not
         * reaching the VDD, the mixer not fitted to it, a frequency it refuses, or
         * a fault downstream of the mixer entirely. Each counter is taken where the
         * decision is made, so the first zero names the stage.
         */
    { INT eventIndex;
    cursor = LogPut(cursor, "STAGE2: events: direct_io=");
    cursor = LogHex(cursor, g_IoViaDirect);
      cursor = LogPut(cursor, " retro_io=");  cursor = LogHex(cursor, g_IoViaRetro);
      cursor = LogPut(cursor, " by_event=");

      for (eventIndex = 0; eventIndex < EV_HIST_MAX; ++eventIndex)
      {
          if (!g_EventHistogram[eventIndex])
              continue;

          cursor = LogPut(cursor, " "); cursor = LogHex(cursor, (DWORD)eventIndex); cursor = LogPut(cursor, ":"); cursor = LogHex(cursor, g_EventHistogram[eventIndex]);
      }

      cursor = LogPut(cursor, "\r\n"); }
    cursor = LogPut(cursor, "STAGE2: pcspk: want=");   cursor = LogHex(cursor, (DWORD)g_SpeakerReal);
    cursor = LogPut(cursor, " opened=");               cursor = LogHex(cursor, (DWORD)g_PcSpeaker.OpenState);
    cursor = LogPut(cursor, " open_err=");             cursor = LogHex(cursor, g_PcSpeaker.OpenError);
    cursor = LogPut(cursor, " via=");                  cursor = LogHex(cursor, (DWORD)g_PcSpeaker.OpenPath);
    cursor = LogPut(cursor, " ioctls=");               cursor = LogHex(cursor, g_PcSpeaker.IoctlCount);
    cursor = LogPut(cursor, " refused=");              cursor = LogHex(cursor, g_PcSpeaker.FailedCount);
    cursor = LogPut(cursor, " last_hz=");              cursor = LogHex(cursor, g_PcSpeaker.CurrentHz);
    cursor = LogPut(cursor, "\r\n");
    cursor = LogPut(cursor, "STAGE2: wave: dev_volume_ok="); cursor = LogHex(cursor, (DWORD)g_Wave.IsDeviceVolumeKnown);
    cursor = LogPut(cursor, " dev_volume=0x");               cursor = LogHex(cursor, g_Wave.DeviceVolume);
    cursor = LogPut(cursor, " silent=");                     cursor = LogHex(cursor, (DWORD)g_Wave.IsSilent);
    cursor = LogPut(cursor, "\r\n");
    cursor = LogPut(cursor, "STAGE2: spk: fitted=");    cursor = LogHex(cursor, (DWORD)(g_Audio.Speaker ? 1 : 0));
    cursor = LogPut(cursor, " level=");                 cursor = LogHex(cursor, (DWORD)g_Audio.SpeakerLevel);
    cursor = LogPut(cursor, " port61=0x");              cursor = LogHex(cursor, (DWORD)g_Speaker.Port61);
    cursor = LogPut(cursor, " ch2_reload=");            cursor = LogHex(cursor, (DWORD)g_Pit.Counter2Reload);
    cursor = LogPut(cursor, " gated_frames=");          cursor = LogHex(cursor, g_Audio.SpeakerGated);
    cursor = LogPut(cursor, " emitted_frames=");        cursor = LogHex(cursor, g_Audio.SpeakerFrames);
    cursor = LogPut(cursor, " last_hz=");               cursor = LogHex(cursor, g_Audio.SpeakerHz);
    cursor = LogPut(cursor, " mixed_frames=");          cursor = LogHex(cursor, g_Audio.FramesMixed);
    cursor = LogPut(cursor, " master=");                cursor = LogHex(cursor, g_Audio.Master);
    cursor = LogPut(cursor, " muted=");                 cursor = LogHex(cursor, (DWORD)g_Audio.IsMuted);
    cursor = LogPut(cursor, "\r\n");
    cursor = LogPut(cursor, "STAGE2: opl: writes=");  cursor = LogHex(cursor, g_Opl.ProfileWrites);
        cursor = LogPut(cursor, " keyons=");              cursor = LogHex(cursor, g_Opl.ProfileKeyOns);
        cursor = LogPut(cursor, " bd_writes=");           cursor = LogHex(cursor, g_Opl.ProfileBdWrites);
        cursor = LogPut(cursor, " bd_or=");               cursor = LogHexByte(cursor, g_Opl.ProfileBdOr);
        cursor = LogPut(cursor, " keyon_am=");            cursor = LogHex(cursor, g_Opl.ProfileKeyOnAm);
        cursor = LogPut(cursor, " keyon_vib=");           cursor = LogHex(cursor, g_Opl.ProfileKeyOnVibrato);
        cursor = LogPut(cursor, " am_ops=");              cursor = LogHex(cursor, g_Opl.ProfileAmOperators);
        cursor = LogPut(cursor, " vib_ops=");             cursor = LogHex(cursor, g_Opl.ProfileVibratoOperators);
        cursor = LogPut(cursor, " waves=");               cursor = LogHexByte(cursor, g_Opl.ProfileWaveMask);
        cursor = LogPut(cursor, " wse=");                 cursor = LogHexByte(cursor, g_Opl.ProfileWaveformSelect);
        cursor = LogPut(cursor, "\r\n");
        /* PERCUSSION, BY EDGE COUNT. bd_or above is an OR over the whole run and
         * cannot tell "set once at init" from "drums play throughout" -- it once
         * produced a confident wrong answer about exactly this register. These are
         * key-on edges per voice. All five are synthesised since #139 (hi-hat,
         * snare and cymbal were silent before it and this line said so); the counts
         * stay because they say how much percussion a game actually uses.
         */
        cursor = LogPut(cursor, "STAGE2: opl rhythm: bassdrum="); cursor = LogHex(cursor, g_Opl.ProfileRhythmHits[4]);
        cursor = LogPut(cursor, " tomtom=");                      cursor = LogHex(cursor, g_Opl.ProfileRhythmHits[2]);
        cursor = LogPut(cursor, " hihat=");                       cursor = LogHex(cursor, g_Opl.ProfileRhythmHits[0]);
        cursor = LogPut(cursor, " snare=");                       cursor = LogHex(cursor, g_Opl.ProfileRhythmHits[3]);
        cursor = LogPut(cursor, " cymbal=");                      cursor = LogHex(cursor, g_Opl.ProfileRhythmHits[1]);
        cursor = LogPut(cursor, "\r\n");
    return cursor;
}

/* End of run: vertical sync, out-of-range guest memory, the planar write and attribute state, the palette signature, the fade and watch dumps, the planar high-water mark. */
static PSTR ReportPlanarVideoState(PSTR cursor, PCSTR const reportEnd)
{
    cursor = LogPut(cursor, "STAGE2: vsync: vbl_edges="); cursor = LogHex(cursor, g_Video.VblEdges);
    cursor = LogPut(cursor, " p3da_reads=");             cursor = LogHex(cursor, g_Video.Port3DaReads);
    cursor = LogPut(cursor, " hbl_owed=");               cursor = LogHex(cursor, g_Video.Port3DaHblOwed);
    cursor = LogPut(cursor, " vbl_owed=");               cursor = LogHex(cursor, g_Video.Port3DaVblOwed);   /* #225 */
    cursor = LogPut(cursor, " run_ms=");                 cursor = LogHex(cursor, GetTickCount() - g_RunStartTick);
    cursor = LogPut(cursor, "\r\n");
    cursor = LogPut(cursor, "STAGE2: imem out-of-range (guarded, would have CRASHED the host): bad_reads=");
    cursor = LogHex(cursor, g_InterpreterMemoryBadReads); cursor = LogPut(cursor, " bad_writes="); cursor = LogHex(cursor, g_InterpreterMemoryBadWrites);
    cursor = LogPut(cursor, " logged="); cursor = LogHex(cursor, g_InterpreterMemoryBadLogged); cursor = LogPut(cursor, "\r\n");
    /* - Is mode Y actually in use? The whole unchained theory rests on Doom
     * clearing Sequencer reg 4 bit 3, which was INFERRED from a pixel pattern
     * (80-px period, 50 rows) and never observed directly. Print the register.
     */
    /* THE CRTC AS THE GUEST PROGRAMMED IT. Added s64 after two speculative
     * fixes in a row -- one right, one wrong. render_planar now depends on all
     * of these, so a wrong picture has to be able to name which register is
     * responsible instead of being guessed at.
     */
    {   UINT index;
        cursor = LogPut(cursor, "STAGE2: planar w0: ensr@write");

        for (index = 0; index < 16; ++index) if (g_Video.WriteEnableSetResetHistogram[index])
        {
            cursor = LogPut(cursor, " 0x"); cursor = LogHexByte(cursor, index);
            cursor = LogPut(cursor, "=");
            cursor = LogDecimal(cursor, g_Video.WriteEnableSetResetHistogram[index]); }

        cursor = LogPut(cursor, " | alu");

        for (index = 0; index < 4; ++index) if (g_Video.WriteAluHistogram[index])
        {
            cursor = LogPut(cursor, " "); cursor = LogDecimal(cursor, index);
            cursor = LogPut(cursor, "=");
            cursor = LogDecimal(cursor, g_Video.WriteAluHistogram[index]); }

        cursor = LogPut(cursor, " | p3_from_sr=");  cursor = LogDecimal(cursor, g_Video.WritePlane3SetReset);
        cursor = LogPut(cursor, " of_which_nonzero="); cursor = LogDecimal(cursor, g_Video.WritePlane3NonZero);
        cursor = LogPut(cursor, " p3_from_cpu=");   cursor = LogDecimal(cursor, g_Video.WritePlane3Data);
        cursor = LogPut(cursor, "\r\n"); }
    {   UINT index;
        cursor = LogPut(cursor, "STAGE2: attr: acport="); cursor = LogDecimal(cursor, g_Video.AcPortWrites);
        cursor = LogPut(cursor, " acbios=");              cursor = LogDecimal(cursor, g_Video.AcBiosWrites);
        cursor = LogPut(cursor, " dacw=");                cursor = LogDecimal(cursor, g_Video.DacWrites);
        cursor = LogPut(cursor, " palresets="); cursor = LogDecimal(cursor, g_Video.PaletteResets);
        cursor = LogPut(cursor, " hi_since_reset="); cursor = LogDecimal(cursor, g_Video.DacHighSinceReset);
        /* The one that can actually be non-zero: this report is written after
         * the guest has exited through a mode set back to text.
         */
        cursor = LogPut(cursor, " hi_max="); cursor = LogDecimal(cursor, g_Video.DacHighMax);
        cursor = LogPut(cursor, " dacblk[16s]=");

        for (index = 0; index < 16; ++index)
        {
            cursor = LogDecimal(cursor, g_Video.DacBlock[index]);
            cursor = LogPut(cursor, ",");
        }

        cursor = LogPut(cursor, " vpal=[");

        for (index = 0; index < 16; ++index)
        {
            cursor = LogHexByte(cursor, g_Video.PaletteRegisters[index]);
            cursor = LogPut(cursor, " ");
        }

        cursor = LogPut(cursor, "] dac@vpal=[");
        /* The DAC entries the DEFAULT AC palette actually points at. If these are
         * the seeded EGA64 values the guest never wrote them; if they hold its
         * colours, the write landed and the fault is downstream.
         */
        for (index = 0; index < 16; ++index)
        {
            UINT32 colour = g_Video.Dac[g_Video.PaletteRegisters[index] & 0x3F];
            cursor = LogHexByte(cursor, (colour >> WORD_SHIFT) & BYTE_MASK); cursor = LogHexByte(cursor, (colour >> BYTE_SHIFT) & BYTE_MASK);
            cursor = LogHexByte(cursor, colour & BYTE_MASK);
            cursor = LogPut(cursor, " "); }

        cursor = LogPut(cursor, "]\r\n"); }
    /* s69: IS THE LEVEL PALETTE EVEN LOADED?:
     * The gameplay screen renders black because dac@vpal is all-zero. That is
     * either "the game never loaded its colours" or "the fade multiplied them
     * to zero and is stuck". The oracle's per-frame palette buffer holds a
     * known green run (00 2a 00 15 3f 15 15 15 15 2a 00 00); scan our guest's
     * conventional memory for it. Found => the colours ARE in memory and the
     * fault is the fade/DAC path (ours); absent => the palette was never loaded
     * (a file-read/decode fault, earlier). Bounded, one pass, exit only.
     */
    {   static const BYTE signature[12] = {0x00,0x2a,0x00,0x15,0x3f,0x15,0x15,0x15,0x15,0x2a,0x00,0x00};
        UINT32 candidate;
        UINT32 found = 0xFFFFFFFFu;
        UINT32 hits = 0;

        for (candidate = 0x400; candidate + 12 <= 0xA0000u; ++candidate)
        {
            if ((candidate & 0xFFF) == 0 && !InterpreterMemoryPageOk(candidate))
            {
                candidate += 0xFFF;
                continue;
            }

            if (*(volatile BYTE *)candidate == 0x00 && *(volatile BYTE *)(candidate+1) == 0x2a)
            {
                UINT32 item;
                INT isOk = 1;

                for (item = 0; item < 12; ++item) if (*(volatile BYTE *)(candidate+item) != signature[item])
                {
                    isOk = 0;
                    break;
                }

                if (isOk)
                {
                    if (found == 0xFFFFFFFFu)
                        found = candidate;

                    hits++;
                }
            }
        }

        cursor = LogPut(cursor, "STAGE2: level-palette signature: ");

        if (found != 0xFFFFFFFFu) { cursor = LogPut(cursor, "FOUND at lin=0x"); cursor = LogHex(cursor, found);
            cursor = LogPut(cursor, " (hits="); cursor = LogDecimal(cursor, hits);
            cursor = LogPut(cursor, ") -> colours ARE loaded; fade/DAC path is the fault\r\n"); }
        else
            cursor = LogPut(cursor, "ABSENT -> the level palette was never loaded into guest RAM\r\n");
    }
    /* s69: THE FADED PALETTE BUFFER + THE FADE STATE, from the guest DS the
     * heartbeat last sampled. The per-frame palette routine feeds the DAC from
     * ds:0x2668; if that is black while the raw palette (above) is present, the
     * fade multiplied it to zero. [0x1f7c] is the palette-state selector
     * (oracle=4). Reads go through InterpreterMemoryPageOk so an unmapped DS cannot fault.
     */
    if (g_HeartbeatDs)
    {
        UINT32 dataSegmentBase = (UINT32)g_HeartbeatDs << PARAGRAPH_SHIFT;
        UINT32 index;
        cursor = LogPut(cursor, "STAGE2: fade dump ds=0x"); cursor = LogHex(cursor, g_HeartbeatDs);

        if (InterpreterMemoryPageOk(dataSegmentBase + 0x1f7c))
        {
            cursor = LogPut(cursor, " [1f7c]=0x"); cursor = LogHexByte(cursor, *(volatile BYTE *)(dataSegmentBase + 0x1f7c));
        }

        cursor = LogPut(cursor, " buf@2668=[");

        if (InterpreterMemoryPageOk(dataSegmentBase + 0x2668))
        {
            for (index = 0; index < 24; ++index)
            {
                cursor = LogHexByte(cursor, *(volatile BYTE *)(dataSegmentBase + 0x2668 + index));
                cursor = LogPut(cursor, " ");
            }
        }
        else
            cursor = LogPut(cursor, "<ds:2668 unmapped>");

        cursor = LogPut(cursor, "]\r\n");
    }

    /* The VRAM watchpoint. Silent unless cfg/vwatch.txt armed it. */
    if (g_Video.WatchOffset != VIDEO_OFFSET_NONE)
    {
        UINT watchIndex2;
        UINT watchCount = g_Video.WatchCount < VIDEO_WATCH_MAX ? g_Video.WatchCount : VIDEO_WATCH_MAX;
        cursor = LogPut(cursor, "STAGE2: vwatch off=0x"); cursor = LogHex(cursor, g_Video.WatchOffset);
        cursor = LogPut(cursor, " writes="); cursor = LogDecimal(cursor, g_Video.WatchCount); cursor = LogPut(cursor, "\r\n");

        for (watchIndex2 = 0; watchIndex2 <= watchCount; ++watchIndex2)
        {
            const VIDEO_WATCH_RECORD *watch = (watchIndex2 < watchCount) ? &g_Video.Watch[watchIndex2] : &g_Video.WatchLast;
            INT item;

            if (watchIndex2 == watchCount)
            {
                if (!g_Video.WatchCount)
                    break;

                cursor = LogPut(cursor, "  LAST ");
            }
            else
            {
                cursor = LogPut(cursor, "  #");
                cursor = LogDecimal(cursor, watchIndex2);
                cursor = LogPut(cursor, " ");
            }

            cursor = LogPut(cursor, "pc="); cursor = LogHex(cursor, watch->Pc);
            cursor = LogPut(cursor, " wm="); cursor = LogDecimal(cursor, watch->WriteMode);
            cursor = LogPut(cursor, " mm="); cursor = LogHexByte(cursor, watch->MapMask);
            cursor = LogPut(cursor, " ensr="); cursor = LogHexByte(cursor, watch->EnableSetReset);
            cursor = LogPut(cursor, " sr="); cursor = LogHexByte(cursor, watch->SetReset);
            cursor = LogPut(cursor, " frot="); cursor = LogHexByte(cursor, watch->FunctionRotate);
            cursor = LogPut(cursor, " bm="); cursor = LogHexByte(cursor, watch->BitMask);
            cursor = LogPut(cursor, " cpu="); cursor = LogHexByte(cursor, watch->Cpu);
            cursor = LogPut(cursor, " lat=");

            for (item = 0; item < 4; ++item)
                cursor = LogHexByte(cursor, watch->Latch[item]);

            cursor = LogPut(cursor, " after=");

            for (item = 0; item < 4; ++item)
                cursor = LogHexByte(cursor, watch->After[item]);

            cursor = LogPut(cursor, "\r\n");

            if (cursor > reportEnd - 256)
                break;
        }
    }

    /* - Does this guest use OFF-SCREEN VRAM? Above 38400 used to read back as
     * 0xFF, so the answer used to be invisible.
     */
    cursor = LogPut(cursor, "STAGE2: planar hi_water=0x"); cursor = LogHex(cursor, g_Video.PlanarHighWater);
    cursor = LogPut(cursor, " plane_size=0x"); cursor = LogHex(cursor, (DWORD)VIDEO_PLANE_SIZE);
    cursor = LogPut(cursor, " wsite_lost="); cursor = LogDecimal(cursor, g_Video.WriteSitesLost);
    /* - DOES THIS GUEST USE COLOUR COMPARE? Read mode 1 is pixel-perfect terrain
     * collision in one instruction; until it was implemented, every such read
     * returned a raw plane byte and the guest acted on it.
     */
    cursor = LogPut(cursor, " reads[mode0/mode1]="); cursor = LogDecimal(cursor, g_Video.ReadModeHistogram[0]);
    cursor = LogPut(cursor, "/"); cursor = LogDecimal(cursor, g_Video.ReadModeHistogram[1]);
    cursor = LogPut(cursor, " cc="); cursor = LogHexByte(cursor, g_Video.ColorCompare);
    cursor = LogPut(cursor, " cdc="); cursor = LogHexByte(cursor, g_Video.ColorDontCare);
    cursor = LogPut(cursor, "\r\n");
    return cursor;
}

/* End of run: the planar access sites -- off-screen writers, the off-screen cache, colour-compare reads and read sites. */
static PSTR ReportPlanarSites(PSTR cursor, PSTR const base, PCSTR const reportEnd)
{
            {   UINT item;
                /* - And, whatever their rank, every site that touched DEEP off-screen
                 * VRAM. That region was unreachable until a plane became 64KB, so
                 * "who is up there" is the question worth answering unprompted -- and
                 * a one-shot blit of a status panel will never be in a top-N list.
                 */
            {
                LogAppend(LOG_PATH, base, cursor);
                SerialOut(base, cursor);
                cursor = base;
            }
                cursor = LogPut(cursor, "STAGE2: planar sites touching off-screen (>=0xC000):\r\n");

                for (item = 0; item < VIDEO_SITES; ++item)
                {
                    INT which;

                    for (which = 0; which < 2; ++which)
                    {
                        const VIDEO_SITE *videoSite = which ? &g_Video.ReadSites[item] : &g_Video.WriteSites[item];

                        if (!videoSite->Count || videoSite->High < 0xC000u)
                            continue;

                        cursor = LogPut(cursor, which ? "  READ  pc=" : "  WRITE pc=");
                        cursor = LogHex(cursor, videoSite->Pc);
                        cursor = LogPut(cursor, " n=");     cursor = LogDecimal(cursor, videoSite->Count);
                        cursor = LogPut(cursor, " off=0x"); cursor = LogHex(cursor, videoSite->Low);
                        cursor = LogPut(cursor, "..0x");    cursor = LogHex(cursor, videoSite->High);
                        cursor = LogPut(cursor, "\r\n");
                    }

                    if (cursor > reportEnd - 256)
                        break;
                }         }

            /* - THE OFF-SCREEN SPRITE CACHE, WITHOUT THE COLLISION CAVEAT. The report
             * above is drawn from 256-slot hashes that lost 249,630 reads on the run
             * this was written for, so a site missing from it may simply have collided
             * -- and the open toolbar question is precisely whether a routine RAN.
             * This table is linear and cannot lose an entry to another pc; only to
             * being full, which `lost` states. `seq` is a tick per cache access, so
             * first/last order the compositor against the blitter and answer "was the
             * cache filled BEFORE it was copied to the screen" directly.
             */
            /* [CAUTION]: FLUSH FIRST. This block is near the END of a report that shares one
             * char[8192], and the off-screen list above it can run to dozens of lines.
             * On its first test-machine run the buffer guard below fired after the FIRST entry
             * and silently dropped the other nine -- the table had the answer and the
             * log did not. It was caught only because `seq` is printed beside the per
             * entry counts and 12800 did not add up to 19984; without that cross-check
             * the truncated list reads as a complete one, which is the worst shape a
             * report can fail in. A fixed buffer is a silent budget: spend it here.
             */
            {
                LogAppend(LOG_PATH, base, cursor);
                SerialOut(base, cursor);
                cursor = base;
            }
            {   UINT item;
                cursor = LogPut(cursor, "STAGE2: off-screen cache sites (>=0x"); cursor = LogHex(cursor, VIDEO_CACHE_LOW);
                cursor = LogPut(cursor, "), lost="); cursor = LogDecimal(cursor, g_Video.CacheSitesLost);
                cursor = LogPut(cursor, " seq=");    cursor = LogDecimal(cursor, g_Video.CacheSequence);
                cursor = LogPut(cursor, "\r\n");
                {   UINT32 accounted = 0;

                    for (item = 0; item < VIDEO_CACHE_SITES; ++item)
                    {
                        const VIDEO_CACHE_SITE *cacheSite = &g_Video.CacheSites[item];

                        if (!cacheSite->Count)
                            continue;

                        accounted += cacheSite->Count;
                        cursor = LogPut(cursor, cacheSite->IsWrite ? "  cache WRITE pc=" : "  cache READ  pc=");
                        cursor = LogHex(cursor, cacheSite->Pc);
                        cursor = LogPut(cursor, " n=");      cursor = LogDecimal(cursor, cacheSite->Count);
                        cursor = LogPut(cursor, " off=0x");  cursor = LogHex(cursor, cacheSite->Low);
                        cursor = LogPut(cursor, "..0x");     cursor = LogHex(cursor, cacheSite->High);
                        cursor = LogPut(cursor, " first=");  cursor = LogDecimal(cursor, cacheSite->First);
                        cursor = LogPut(cursor, " last=");   cursor = LogDecimal(cursor, cacheSite->Last);
                        cursor = LogPut(cursor, "\r\n");

                        if (cursor > reportEnd - 512)
                            break;
                    }

                    /* - THE LIST MUST ACCOUNT FOR EVERY ACCESS IT COUNTED. n's + lost has
                     * to equal seq; if it does not, lines are MISSING and the absence of
                     * a pc above means nothing. Said out loud so it cannot be read past.
                     */
                    cursor = LogPut(cursor, "  cache accounted="); cursor = LogDecimal(cursor, accounted + g_Video.CacheSitesLost);
                    cursor = LogPut(cursor, " of seq=");           cursor = LogDecimal(cursor, g_Video.CacheSequence);
                    cursor = LogPut(cursor, (accounted + g_Video.CacheSitesLost == g_Video.CacheSequence)
                                  ? " COMPLETE\r\n" : " !! TRUNCATED -- absence proves NOTHING\r\n");
                }
            }
            /* The write sites, busiest first -- who drew the screen, and where. */
            {   UINT item, shown;

                for (shown = 0; shown < 14; ++shown)
                {
                    UINT best = VIDEO_SITES;
                    UINT32 bestCount = 0;

                    for (item = 0; item < VIDEO_SITES; ++item)
                        if (g_Video.WriteSites[item].Count > bestCount)
                        {
                            bestCount = g_Video.WriteSites[item].Count;
                            best = item;
                        }

                    if (best == VIDEO_SITES)
                        break;

                    cursor = LogPut(cursor, "  wsite pc="); cursor = LogHex(cursor, g_Video.WriteSites[best].Pc);
                    cursor = LogPut(cursor, " n=");   cursor = LogDecimal(cursor, g_Video.WriteSites[best].Count);
                    cursor = LogPut(cursor, " off=0x"); cursor = LogHex(cursor, g_Video.WriteSites[best].Low);
                    cursor = LogPut(cursor, "..0x");    cursor = LogHex(cursor, g_Video.WriteSites[best].High);
                    cursor = LogPut(cursor, "\r\n");
                    g_Video.WriteSites[best].Count = 0;            /* report is the last use of it */

                    if (cursor > reportEnd - 512)
                        break;
                }

                /* - THE COLOUR-COMPARE READ SITES: a guest asking "where is the ground". */
            {
                LogAppend(LOG_PATH, base, cursor);
                SerialOut(base, cursor);
                cursor = base;
            }
                cursor = LogPut(cursor, "STAGE2: planar COLOUR-COMPARE read sites, lost=");
                cursor = LogDecimal(cursor, g_Video.CompareSitesLost); cursor = LogPut(cursor, "\r\n");

                for (shown = 0; shown < 10; ++shown)
                {
                    UINT best = VIDEO_SITES;
                    UINT32 bestCount = 0;

                    for (item = 0; item < VIDEO_SITES; ++item)
                        if (g_Video.CompareSites[item].Count > bestCount)
                        {
                            bestCount = g_Video.CompareSites[item].Count;
                            best = item;
                        }

                    if (best == VIDEO_SITES)
                        break;

                    cursor = LogPut(cursor, "  cc-read pc="); cursor = LogHex(cursor, g_Video.CompareSites[best].Pc);
                    cursor = LogPut(cursor, " n=");   cursor = LogDecimal(cursor, g_Video.CompareSites[best].Count);
                    cursor = LogPut(cursor, " off=0x"); cursor = LogHex(cursor, g_Video.CompareSites[best].Low);
                    cursor = LogPut(cursor, "..0x");    cursor = LogHex(cursor, g_Video.CompareSites[best].High);
                    /* - AND WHAT IT ANSWERED. all-zero means the guest saw no terrain. */
                    cursor = LogPut(cursor, " zero="); cursor = LogDecimal(cursor, g_Video.CompareSitesZero[best]);
                    cursor = LogPut(cursor, " ones="); cursor = LogDecimal(cursor, g_Video.CompareSitesOnes[best]);
                    cursor = LogPut(cursor, "\r\n");
                    g_Video.CompareSites[best].Count = 0;

                    if (cursor > reportEnd - 512)
                        break;
                }

                /* - HOW MANY SITES THIS TOP-N LEFT OUT. Without it a truncated list
                 * reads as a complete enumeration of who touches VRAM, and "pc X is not
                 * here" becomes an argument it cannot support.
                 */
                {   UINT more = 0;

                    for (item = 0; item < VIDEO_SITES; ++item)
                        if (g_Video.CompareSites[item].Count)
                            ++more;

                    cursor = LogPut(cursor, "  (cc-read: "); cursor = LogDecimal(cursor, more);
                    cursor = LogPut(cursor, " further sites not shown)\r\n");
                }
            {
                LogAppend(LOG_PATH, base, cursor);
                SerialOut(base, cursor);
                cursor = base;
            }
                cursor = LogPut(cursor, "STAGE2: planar read sites, rsite_lost=");
                cursor = LogDecimal(cursor, g_Video.ReadSitesLost); cursor = LogPut(cursor, "\r\n");

                for (shown = 0; shown < 14; ++shown)
                {
                    UINT best = VIDEO_SITES;
                    UINT32 bestCount = 0;

                    for (item = 0; item < VIDEO_SITES; ++item)
                        if (g_Video.ReadSites[item].Count > bestCount)
                        {
                            bestCount = g_Video.ReadSites[item].Count;
                            best = item;
                        }

                    if (best == VIDEO_SITES)
                        break;

                    cursor = LogPut(cursor, "  rsite pc="); cursor = LogHex(cursor, g_Video.ReadSites[best].Pc);
                    cursor = LogPut(cursor, " n=");   cursor = LogDecimal(cursor, g_Video.ReadSites[best].Count);
                    cursor = LogPut(cursor, " off=0x"); cursor = LogHex(cursor, g_Video.ReadSites[best].Low);
                    cursor = LogPut(cursor, "..0x");    cursor = LogHex(cursor, g_Video.ReadSites[best].High);
                    cursor = LogPut(cursor, "\r\n");
                    g_Video.ReadSites[best].Count = 0;

                    if (cursor > reportEnd - 512)
                        break;
                }

                /* - HOW MANY SITES THIS TOP-N LEFT OUT. Without it a truncated list
                 * reads as a complete enumeration of who touches VRAM, and "pc X is not
                 * here" becomes an argument it cannot support.
                 */
                {   UINT more = 0;

                    for (item = 0; item < VIDEO_SITES; ++item)
                        if (g_Video.ReadSites[item].Count)
                            ++more;

                    cursor = LogPut(cursor, "  (rsite: "); cursor = LogDecimal(cursor, more);
                    cursor = LogPut(cursor, " further sites not shown)\r\n");
                }
    }
    return cursor;
}

/* End of run: the 3DAh clock, the CRTC start and registers, and the video state as the run ended, with each plane's non-zero count. */
static PSTR ReportCrtcAndVideoNow(PSTR cursor, UINT *nonZero)
{
    UINT plane;

    /* - Does this guest scroll or page-flip, and how often would a frame have
     * been built from a half-written start address?
     */
    /* - THE RETRACE CLOCK, IN ITS OWN UNITS. span_ms is how much MODEL time passed
     * between the guest's first and last 0x3DA poll. Against the run's real
     * length it says whether the timebase is wall-clock: if span_ms is a
     * twentieth of the run, the guest's frame rate is low because the clock is
     * slow, and the vblank geometry is not on trial. dtmax is the longest the
     * guest went between polls -- it can only miss a vblank if that exceeds the
     * blanking interval.
     */
    cursor = LogPut(cursor, "STAGE2: 3da clock: span_ms="); cursor = LogDecimal(cursor, (UINT32)((g_Video.Time3DaLast - g_Video.Time3DaFirst) / MICROSECONDS_PER_MILLISECOND_U));
    cursor = LogPut(cursor, " reads=");  cursor = LogDecimal(cursor, g_Video.Port3DaReads);
    cursor = LogPut(cursor, " edges=");  cursor = LogDecimal(cursor, g_Video.VblEdges);
    cursor = LogPut(cursor, " pal_splits="); cursor = LogDecimal(cursor, g_Video.PaletteSplitNotes);
    cursor = LogPut(cursor, " dacrow[0-1,2-159,160+,vbl]="); cursor = LogDecimal(cursor, g_Video.DacRowHistogram[0]);
    cursor = LogPut(cursor, "/"); cursor = LogDecimal(cursor, g_Video.DacRowHistogram[1]);
    cursor = LogPut(cursor, "/"); cursor = LogDecimal(cursor, g_Video.DacRowHistogram[2]);
    cursor = LogPut(cursor, "/"); cursor = LogDecimal(cursor, g_Video.DacRowHistogram[3]);
    cursor = LogPut(cursor, " dtmax_us="); cursor = LogDecimal(cursor, g_Video.Dt3DaMax);
    cursor = LogPut(cursor, " dtzero=");   cursor = LogDecimal(cursor, g_Video.Dt3DaZero);
    cursor = LogPut(cursor, " dthist[1,4,16,64,256,1k,4k,16k+us]=");
    {
        UINT byteIndex;

        for (byteIndex = 0; byteIndex < 8; ++byteIndex)
        {
            if (byteIndex)
                cursor = LogPut(cursor, "/");

            cursor = LogDecimal(cursor, g_Video.Dt3DaHistogram[byteIndex]);
        }
    }
    cursor = LogPut(cursor, "\r\n");
    cursor = LogPut(cursor, "STAGE2: crtc start: pairs="); cursor = LogDecimal(cursor, g_Video.CrtcStartWrites);
    cursor = LogPut(cursor, " torn_avoided="); cursor = LogDecimal(cursor, g_Video.CrtcStartHalf);
    cursor = LogPut(cursor, " live="); cursor = LogDecimal(cursor, g_Video.CrtcStartLive);
    cursor = LogPut(cursor, " gap_frames[0,1,2,3,4+]=");         /* #221: guest pacing */
    {
        INT gateIndex;

        for (gateIndex = 0; gateIndex < 5; ++gateIndex)
        {
            if (gateIndex)
                cursor = LogPut(cursor, "/");

            cursor = LogDecimal(cursor, g_Video.StartGapHistogram[gateIndex]);
        }
    }
    cursor = LogPut(cursor, "\r\n");
    cursor = LogPut(cursor, "STAGE2: crtc: start=");   cursor = LogDecimal(cursor, g_Video.CrtcStart);
    cursor = LogPut(cursor, " offset=");               cursor = LogDecimal(cursor, g_Video.CrtcOffset);
    cursor = LogPut(cursor, " off_seen=");             cursor = LogDecimal(cursor, g_Video.IsCrtcOffsetSeen);
    cursor = LogPut(cursor, " linecmp=");              cursor = LogDecimal(cursor, g_Video.CrtcLineCompare);
    cursor = LogPut(cursor, " (0x18=");                cursor = LogDecimal(cursor, g_Video.CrtcLineCompareLow);
    cursor = LogPut(cursor, " ovf=");                  cursor = LogDecimal(cursor, g_Video.CrtcOverflow);
    cursor = LogPut(cursor, " maxscan=");              cursor = LogDecimal(cursor, g_Video.CrtcMaxScan);
    cursor = LogPut(cursor, ")\r\n");
    /* THE VGA REGISTER FILE. (docs/inventory/vga.md, step 1):
     * Its own buffer and its own flush: the block is ~1.2 KB and `report` is
     * shared with everything else in this summary.
     */
    {   static CHAR videoRegisters[2048];
        INT dumpLength = VddVideoRegistersDump(&g_Video, videoRegisters, (INT)sizeof videoRegisters);

        if (dumpLength > 0)
        {
            LogAppend(LOG_PATH, videoRegisters, videoRegisters + dumpLength);
            SerialOut(videoRegisters, videoRegisters + dumpLength);
        }
        }
    cursor = LogPut(cursor, "STAGE2: video now: chain4="); cursor = LogHexByte(cursor, g_Video.IsChain4);
    cursor = LogPut(cursor, " ymask="); cursor = LogHexByte(cursor, g_Video.YMask);
    cursor = LogPut(cursor, " mkind="); cursor = LogHexByte(cursor, g_Video.ModeKind);
    cursor = LogPut(cursor, " gw="); cursor = LogHex(cursor, g_Video.GraphicsWidth); cursor = LogPut(cursor, " gh="); cursor = LogHex(cursor, g_Video.GraphicsHeight);
    cursor = LogPut(cursor, " mapmask="); cursor = LogHexByte(cursor, g_Video.MapMask);
    cursor = LogPut(cursor, " setreset="); cursor = LogHexByte(cursor, g_Video.SetReset);
    cursor = LogPut(cursor, " ensr="); cursor = LogHexByte(cursor, g_Video.EnableSetReset);
    cursor = LogPut(cursor, " wmode="); cursor = LogHexByte(cursor, g_Video.WriteMode);
    cursor = LogPut(cursor, " plane-nonzero=");

    for (plane = 0; plane < VIDEO_PLANES; ++plane)
    {
        cursor = LogHex(cursor, nonZero[plane]);
        cursor = LogPut(cursor, plane<VIDEO_PLANES - 1?"/":"");
    }

    cursor = LogPut(cursor, "\r\n");
    return cursor;
}

/* End of run: the guest as it was left -- the timer and tick vectors, the bytes at CS:IP, and the async-injection and interpreter counters. */
static PSTR ReportGuestStateAtExit(PSTR cursor, PSTR const base, volatile BYTE * const tib)
{
    /* [CAUTION]: FLUSH FIRST. The rsite/crtc/video-now block above can fill the 8 KB report
     * on its own, and LogAppend writes [buf,end) UNCLAMPED: the ivt08 line was
     * cut at `bail=000`, the next two blocks vanished, and the overrun went onto
     * the stack. (s68 -- the third truncation of this report in two sessions.)
     */
    LogAppend(LOG_PATH, base, cursor); cursor = base;
    { const volatile BYTE *zeroPage = (const volatile BYTE *)0;
      DWORD cs2 = VDM_REG16(tib, VTIB_CS);
      DWORD ip2 = VDM_REG16(tib, VTIB_EIP);
      const volatile BYTE *codeView = (const volatile BYTE *)((cs2 << PARAGRAPH_SHIFT) + ip2);
      UINT index3;
      cursor = LogPut(cursor, "STAGE2: ivt08="); cursor = LogHex(cursor, (DWORD)zeroPage[0x22] | ((DWORD)zeroPage[0x23] << BYTE_SHIFT));
      cursor = LogPut(cursor, ":"); cursor = LogHex(cursor, (DWORD)zeroPage[0x20] | ((DWORD)zeroPage[0x21] << BYTE_SHIFT));
      cursor = LogPut(cursor, " ivt1C="); cursor = LogHex(cursor, (DWORD)zeroPage[0x72] | ((DWORD)zeroPage[0x73] << BYTE_SHIFT));
      cursor = LogPut(cursor, ":"); cursor = LogHex(cursor, (DWORD)zeroPage[0x70] | ((DWORD)zeroPage[0x71] << BYTE_SHIFT));
      cursor = LogPut(cursor, " at-cs:ip=");

      for (index3 = 0; index3 < 8; ++index3)
      {
          cursor = LogHexByte(cursor, codeView[index3]);
          cursor = LogPut(cursor, " ");
      }

      cursor = LogPut(cursor, " asyncinj="); cursor = LogHex(cursor, g_AsyncInjected);
      cursor = LogPut(cursor, " interp-refused="); cursor = LogHex(cursor, g_InterpRefused);
      cursor = LogPut(cursor, " p12-batches="); cursor = LogHex(cursor, g_P12Batches);
      cursor = LogPut(cursor, " p12-instrs=");  cursor = LogHex(cursor, g_P12Instructions);
      cursor = LogPut(cursor, " p12-bails=");   cursor = LogHex(cursor, g_P12Bails);
      cursor = LogPut(cursor, " bail="); cursor = LogHex(cursor, g_AsyncBail);
      cursor = LogPut(cursor, " nestblk="); cursor = LogHex(cursor, g_AsyncNestBlocked);
      cursor = LogPut(cursor, " asyncsites="); cursor = LogHex(cursor, (DWORD)g_AsyncSiteCount);
      cursor = LogPut(cursor, (g_AsyncSiteFull ? "(FULL)" : ""));
      cursor = LogPut(cursor, " pmstretch_max_us="); cursor = LogHex(cursor, g_PmStretchMaximumMicroseconds);
      cursor = LogPut(cursor, "\r\n"); }
    return cursor;
}

/* End of run: the V86 interpreter's to-do list -- every non-BOP site it declined, with the bytes there. */
static PSTR ReportInterpreterBailSites(PSTR cursor, PSTR const base, PCSTR const reportEnd)
{
    /* The interpreter's to-do list: every non-BOP site it declined, with the bytes. */
    { UINT index4;
      LogAppend(LOG_PATH, base, cursor); cursor = base;      /* flush: the table may be long */
      cursor = LogPut(cursor, "STAGE2: P12 non-BOP bail sites="); cursor = LogDecimal(cursor, g_P12SiteCount);
      cursor = LogPut(cursor, " lost="); cursor = LogDecimal(cursor, g_P12SiteLost); cursor = LogPut(cursor, "\r\n");

      for (index4 = 0; index4 < g_P12SiteCount; ++index4)
      {
          UINT index5;
          cursor = LogPut(cursor, "  bail cs:ip="); cursor = LogHex(cursor, g_P12Site[index4].Cs);
          cursor = LogPut(cursor, ":"); cursor = LogHex(cursor, g_P12Site[index4].Ip);
          cursor = LogPut(cursor, " n="); cursor = LogDecimal(cursor, g_P12Site[index4].Count); cursor = LogPut(cursor, " bytes:");

          for (index5 = 0; index5 < 8; ++index5)
          {
              cursor = LogPut(cursor, " ");
              cursor = LogHexByte(cursor, g_P12Site[index4].Bytes[index5]);
          }

          cursor = LogPut(cursor, "\r\n");

          if (cursor > reportEnd - 256)
          {
              LogAppend(LOG_PATH, base, cursor);
              cursor = base;
          }
      } }

    LogAppend(LOG_PATH, base, cursor); cursor = base;
    return cursor;
}

/* End of run: every video mode the guest set or asked about. */
static PSTR ReportModeSets(PSTR cursor)
{
    INT index;

    cursor = LogPut(cursor, "STAGE2: mode sets:");

    for (index = 0; index < g_Video.ModeQueryCount; ++index)
    {
        cursor = LogPut(cursor, " mode=0x"); cursor = LogHexByte(cursor, g_Video.ModeQueries[index].Mode);
        cursor = LogPut(cursor, "/kind="); cursor = LogHexByte(cursor, g_Video.ModeQueries[index].Kind);
        cursor = LogPut(cursor, "/"); cursor = LogHex(cursor, g_Video.ModeQueries[index].Width);
        cursor = LogPut(cursor, "x"); cursor = LogHex(cursor, g_Video.ModeQueries[index].Height);
    }

    if (!g_Video.ModeQueryCount)
        cursor = LogPut(cursor, " none");

    cursor = LogPut(cursor, "\r\n");
    return cursor;
}

/* End of run: Mode Y -- the remap's counters, the fan-out bar, GR4 and the latches, the plane snapshots and the write-mode and map-mask histograms. */
static PSTR ReportModeY(PSTR cursor)
{
    INT index;

    /* - THE MODE-Y ARRAYS, NOT THE PLANAR ONES. "plane-nonzero" above counts
     * g_Video.plane[] -- the 16-colour planar buffer, which an unchained 256-colour
     * mode never touches -- so it has reported four zeroes for every mode-Y run
     * ever made and told us nothing. These are the arrays a mode-Y frame is
     * actually built from, plus the map-mask values the program really used.
     */
    ModeYTimelineReport();   /* NORTH STAR 1's measurement -- see g_ytl_* */
    cursor = LogPut(cursor, "STAGE2: modeY remap="); cursor = LogHex(cursor, (DWORD)g_ModeYRemap);
    cursor = LogPut(cursor, " swaps="); cursor = LogHex(cursor, g_ModeYSwaps);
    cursor = LogPut(cursor, " fanouts="); cursor = LogHex(cursor, g_ModeYFanouts);
    cursor = LogPut(cursor, " failed="); cursor = LogHex(cursor, g_ModeYFail);
    /* - ARE THE PLANES COLLAPSED, OR DOES THE RENDER COLLAPSE THEM? The oracle says
     * 62% of the status bar's four-pixel groups hold one value where the reference
     * holds four. That can only come from the four PLANES agreeing, or from the
     * render reading one plane four times. Ask the planes directly, over the bar's
     * own offsets in the page the CRTC is displaying. If they disagree here and the
     * screen shows agreement, the fault is downstream of the planes.
     */
    if (g_ModeYRemap)
    {
        /* - PER PAGE, because "we are displaying the wrong buffer" and "the buffer is
         * wrong" look identical from one page. Doom triple-buffers at 0, 0x4000 and
         * 0x8000; if one page's bar is intact and the one the CRTC points at is not,
         * the fault is in following the page flip, not in the writes.
         */
        UINT32 page;
        cursor = LogPut(cursor, " bar_planes_equal_per_page:");

        for (page = 0; page < 3; ++page)
        {
            UINT32 position;
            UINT32 equalCount = 0;
            UINT32 total = 0;
            UINT32 base = page * 0x4000u;

            for (position = base + 168u * 80u; position < base + 200u * 80u; ++position)
            {
                UINT32 windowOffset = position & (MODEY_WIN - 1u);
                BYTE plane0Byte = ((BYTE *)g_ModeYView[0])[windowOffset];
                BYTE plane1Byte = ((BYTE *)g_ModeYView[1])[windowOffset];
                BYTE plane2Byte = ((BYTE *)g_ModeYView[2])[windowOffset];
                BYTE plane3Byte = ((BYTE *)g_ModeYView[3])[windowOffset];
                ++total;

                if (plane0Byte == plane1Byte && plane1Byte == plane2Byte && plane2Byte == plane3Byte)
                    ++equalCount;
            }

            cursor = LogPut(cursor, " p"); cursor = LogHexByte(cursor, page); cursor = LogPut(cursor, "=");
            cursor = LogHex(cursor, equalCount); cursor = LogPut(cursor, "/"); cursor = LogHex(cursor, total);
        }
    }

    /* - THE FAN-OUT'S OWN CONTRIBUTION TO THE COLLAPSE, by row band. Compare
     * `distinct` against bar_planes_equal above: near it means this path IS the
     * four-way collapse; near zero exonerates it properly. Band A is rows 168-183,
     * which no write-mode-1 burst ever reaches and which session 23 measured as the
     * WORSE half; band B is 184-199, where every burst lands.
     */
    cursor = LogPut(cursor, " fanout_bar[A=rows168-183,B=184-199]: writes=");
    cursor = LogHex(cursor, g_ModeYFanoutBarWrites[0]); cursor = LogPut(cursor, "/"); cursor = LogHex(cursor, g_ModeYFanoutBarWrites[1]);
    cursor = LogPut(cursor, " distinct=");
    cursor = LogHex(cursor, g_ModeYFanoutBarDistinct[0]); cursor = LogPut(cursor, "/"); cursor = LogHex(cursor, g_ModeYFanoutBarDistinct[1]);
    cursor = LogPut(cursor, " of "); cursor = LogHex(cursor, YBAR_OFF_MID - YBAR_OFF_LO);
    cursor = LogPut(cursor, "/");    cursor = LogHex(cursor, YBAR_OFF_HI - YBAR_OFF_MID);
    cursor = LogPut(cursor, " per page, 4way=");
    cursor = LogHex(cursor, g_ModeYFanoutBar4Way[0]); cursor = LogPut(cursor, "/"); cursor = LogHex(cursor, g_ModeYFanoutBar4Way[1]);
    /* - DOES THE GUEST WRITE THE SAME BYTES TO DIFFERENT PLANES? See ModeYSampleCheck().
     * Read `eqb` (PER-BYTE agreement), not `cross_same` (per-window, kept only so
     * the old number stays comparable and visibly useless). Compare eqb against the
     * two figures printed above and by bandprof.py:
     * ~67%  matches bar_planes_equal  => the planes are RECEIVING collapsed data
     *                                    and the fault is upstream of them
     * ~12%  matches an intact bar     => they receive distinct data and something
     *                                    downstream collapses it
     * p1eq is the same rate against PLANE 1's last window specifically, per plane,
     * because the hypothesis names plane 1. p1eq[1] is the self-baseline: how much
     * plane 1 agrees with its own previous content, i.e. how much of this window is
     * static anyway. A p1eq[0/2/3] near p1eq[1] is the collapse; well below it is
     * not. All rates are percent, printed in hex.
     */
    { INT band;

    for (band = 0; band < 2; ++band)
    {
        INT probeIndex2;
        cursor = LogPut(cursor, band ? " ysmpB[184-199]:" : " ysmpA[168-183]:");
        cursor = LogPut(cursor, " writes="); cursor = LogHex(cursor, g_ModeYSampleWrites[band]);
        cursor = LogPut(cursor, " cross_same="); cursor = LogHex(cursor, g_ModeYSampleCrossSame[band]);
        cursor = LogPut(cursor, " cross_diff="); cursor = LogHex(cursor, g_ModeYSampleCrossDiff[band]);
        cursor = LogPut(cursor, " cross_eqb=");
        cursor = LogHex(cursor, g_ModeYSampleCrossEqualBytes[band]); cursor = LogPut(cursor, "/");
        cursor = LogHex(cursor, g_ModeYSampleCrossTotalBytes[band]);

        if (g_ModeYSampleCrossTotalBytes[band])
        {
            cursor = LogPut(cursor, "(");
            cursor = LogHex(cursor, g_ModeYSampleCrossEqualBytes[band] * 100u / g_ModeYSampleCrossTotalBytes[band]);
            cursor = LogPut(cursor, "% STATE-not-delivery)");
        }

        /* - THE DELIVERY RATE -- CHANGED BYTES ONLY. This is the one to read:
         * high => the guest handed the same byte to two different planes.
         */
        cursor = LogPut(cursor, " delivered_eq=");
        cursor = LogHex(cursor, g_ModeYSampleDeliveredEqual[band]); cursor = LogPut(cursor, "/");
        cursor = LogHex(cursor, g_ModeYSampleDeliveredTotal[band]);

        if (g_ModeYSampleDeliveredTotal[band])
        {
            cursor = LogPut(cursor, "(");
            cursor = LogHex(cursor, g_ModeYSampleDeliveredEqual[band] * 100u / g_ModeYSampleDeliveredTotal[band]);
            cursor = LogPut(cursor, "%)");
        }

        cursor = LogPut(cursor, " p1eq=");

        for (probeIndex2 = 0; probeIndex2 < 4; ++probeIndex2)
        {
            cursor = LogPut(cursor, probeIndex2 ? "/" : "");

            if (g_ModeYSampleP1Total[band][probeIndex2])
                cursor = LogHex(cursor, g_ModeYSampleP1Equal[band][probeIndex2] * 100u / g_ModeYSampleP1Total[band][probeIndex2]);
            else
                cursor = LogPut(cursor, "-");
        }

        cursor = LogPut(cursor, "% n=");

        for (probeIndex2 = 0; probeIndex2 < 4; ++probeIndex2)
        {
            cursor = LogPut(cursor, probeIndex2 ? "/" : "");
            cursor = LogHex(cursor, g_ModeYSampleP1Total[band][probeIndex2] / YSMP_LEN);
        } } }

    /* - THE MAP-MASK IDENTITY. See g_ModeYSelectorCalls. Both lines must balance exactly;
     * a residual is a path nobody has accounted for.
     */
    { DWORD maskWrites = 0, residual;

      for (index = 0; index < 16; ++index)
          maskWrites += g_Video.MaskHistogram[index];

      /* [CAUTION]: `skip_same` LEFT THIS IDENTITY WHEN THE GR4 FIX LANDED. A map-mask write
       * whose value is unchanged now still calls select -- it has to, because a read
       * may have moved the window since -- so it is no longer a bucket that
       * ACCOUNTS for a write, just a note about how many writes were redundant.
       * Leaving it in the sum printed a **UNACCOUNTED** residual of exactly
       * -skip_same, which is a counter describing the code as it used to be.
       */
      cursor = LogPut(cursor, " maskacct: writes="); cursor = LogHex(cursor, maskWrites);
      cursor = LogPut(cursor, " = sel_calls="); cursor = LogHex(cursor, g_ModeYSelectorCalls);
      cursor = LogPut(cursor, " - c4sel="); cursor = LogHex(cursor, g_Video.Chain4Selects);
      cursor = LogPut(cursor, " c4xfer="); cursor = LogHex(cursor, g_Video.Chain4Transfers);
      cursor = LogPut(cursor, " + skip_chain4="); cursor = LogHex(cursor, g_Video.MaskSkipChain4);
      cursor = LogPut(cursor, " [redundant_same="); cursor = LogHex(cursor, g_Video.MaskSkipSame);
      cursor = LogPut(cursor, ", informational]");
      residual = maskWrites - (g_ModeYSelectorCalls - g_Video.Chain4Selects) - g_Video.MaskSkipChain4;
      cursor = LogPut(cursor, " residual="); cursor = LogHex(cursor, residual);
      cursor = LogPut(cursor, residual ? " **UNACCOUNTED**" : " (balanced)");
      cursor = LogPut(cursor, " | sel_calls = swaps="); cursor = LogHex(cursor, g_ModeYSwaps);
      cursor = LogPut(cursor, " + sel_same="); cursor = LogHex(cursor, g_ModeYSelectorSame);
      cursor = LogPut(cursor, " + sel_zero="); cursor = LogHex(cursor, g_ModeYSelectorZero);
      cursor = LogPut(cursor, " + failed="); cursor = LogHex(cursor, g_ModeYFail);
      residual = g_ModeYSelectorCalls - g_ModeYSwaps - g_ModeYSelectorSame - g_ModeYSelectorZero - g_ModeYFail;
      cursor = LogPut(cursor, " residual="); cursor = LogHex(cursor, residual);
      cursor = LogPut(cursor, residual ? " **UNACCOUNTED**" : " (balanced)"); }
    /* - THE READ PLANE. See ModeYRemapReadMap(). `mismatch` counts GR4 writes that
     * named a plane other than the one mapped at A0000 -- every guest read between
     * such a write and the next mask change returns the WRONG PLANE'S BYTES, and no
     * write-side instrument can see it. `pair` is the (GR4, mapped) matrix, so a
     * mismatch can be attributed rather than just counted: a column concentrated on
     * one mapped plane means the guest cycled GR4 while the window sat still, which
     * is exactly the I_ReadScreen shape. Section 4 is linear, 5 is the scratch.
     */
    cursor = LogPut(cursor, " gr4: writes="); cursor = LogHex(cursor, g_ModeYGr4Calls);
    cursor = LogPut(cursor, " mismatch="); cursor = LogHex(cursor, g_ModeYGr4Mismatch);
    cursor = LogPut(cursor, " WINDOW_MOVES="); cursor = LogHex(cursor, g_ModeYGr4Moves);
    cursor = LogPut(cursor, " hist=");

    for (index = 0; index < 4; ++index)
    {
        cursor = LogPut(cursor, index ? "/" : "");
        cursor = LogHex(cursor, g_Video.Gr4Histogram[index]);
    }

    cursor = LogPut(cursor, " pair[gr4->mapped]:");
    { UINT pairA, pairB;

      for (pairA = 0; pairA < 4; ++pairA)
        for (pairB = 0; pairB < 6; ++pairB)
          if (g_ModeYGr4Pair[pairA][pairB])
          {
              cursor = LogPut(cursor, " r"); cursor = LogHexByte(cursor, pairA);
              cursor = LogPut(cursor, "->m"); cursor = LogHexByte(cursor, pairB);
              cursor = LogPut(cursor, "=");
              cursor = LogHex(cursor, g_ModeYGr4Pair[pairA][pairB]); } }

    /* - THE ONE THAT DECIDES IT. GR4 writes between consecutive mask changes:
     * 1 = the ordinary blit (window moves before any read -- harmless)
     * 4 = a PURE READ PASS with the window stranded (the collapse)
     */
    cursor = LogPut(cursor, " gr4_runs[n GR4 per select]:");
    { UINT row2;

    for (row2 = 1; row2 < 10; ++row2)
        if (g_ModeYGr4Runs[row2]) { cursor = LogPut(cursor, " "); cursor = LogHexByte(cursor, row2);
                               cursor = LogPut(cursor, "x");
                               cursor = LogHex(cursor, g_ModeYGr4Runs[row2]); } }

    cursor = LogPut(cursor, " stranded_on_plane=");

    for (index = 0; index < 4; ++index)
    {
        cursor = LogPut(cursor, index ? "/" : "");
        cursor = LogHex(cursor, g_ModeYGr4RunPlanes[index]);
    }

    /* - IS THE LINEAR SECTION EVEN OCCUPIED? One number, ahead of the dump: how many
     * of the bar region's 10240 linear bytes are non-zero. Zero means nothing was
     * ever written to A0000 while the window pointed at the linear section, and the
     * candidate dies here without parsing anything.
     */
    { UINT32 offset2, nonZero = 0;
    const BYTE *latchView = (const BYTE *)g_ModeYView[4];

      for (offset2 = 168u * 320u; offset2 < 200u * 320u; ++offset2)
          if (latchView[offset2 & (MODEY_WIN - 1u)])
              ++nonZero;

      cursor = LogPut(cursor, " linear_bar_nonzero="); cursor = LogHex(cursor, nonZero);
      cursor = LogPut(cursor, "/");
      cursor = LogHex(cursor, 32u * 320u); }
    cursor = LogPut(cursor, " latch_solved="); cursor = LogHex(cursor, g_ModeYLatchOk);
    cursor = LogPut(cursor, " latch_UNSOLVED="); cursor = LogHex(cursor, g_ModeYLatchUnsolved);
    cursor = LogPut(cursor, " gap="); cursor = LogHex(cursor, g_Video.ModeYGap);
    cursor = LogPut(cursor, " attributed="); cursor = LogHex(cursor, g_Video.YNonZero[0]);
    cursor = LogPut(cursor, " crtc_seen="); cursor = LogHexByte(cursor, g_Video.IsCrtcSeen);
    cursor = LogPut(cursor, " crtc_start=0x"); cursor = LogHex(cursor, g_Video.CrtcStart);
    cursor = LogPut(cursor, "\r\n");
    cursor = LogPut(cursor, "STAGE2: modeY snaps:");

    for (index = 0; index < 4; ++index)
    {
        cursor = LogPut(cursor, " p"); cursor = LogHexByte(cursor, (UINT)index);
        cursor = LogPut(cursor, "="); cursor = LogHex(cursor, g_Video.YSnapshots[index]);
        cursor = LogPut(cursor, "/nz="); cursor = LogHex(cursor, g_Video.YNonZero[index]);
    }

    cursor = LogPut(cursor, " wmode hist:");

    for (index = 0; index < 4; ++index) { cursor = LogPut(cursor, " ");
    cursor = LogHexByte(cursor, (UINT)index);
                              cursor = LogPut(cursor, "x");
                              cursor = LogHex(cursor, g_Video.WriteModeHistogram[index]); }

    cursor = LogPut(cursor, "\r\nSTAGE2: modeY (wmode,mask) pairs:");
    { UINT writeMode, mask;

      for (writeMode = 0; writeMode < 4; ++writeMode)
        for (mask = 0; mask < 16; ++mask)
          if (g_Video.ModeMaskHistogram[writeMode * 16 + mask])
          {
              cursor = LogPut(cursor, " w"); cursor = LogHexByte(cursor, writeMode);
              cursor = LogPut(cursor, "/m"); cursor = LogHexByte(cursor, mask);
              cursor = LogPut(cursor, "=");
              cursor = LogHex(cursor, g_Video.ModeMaskHistogram[writeMode * 16 + mask]); } }

    cursor = LogPut(cursor, "\r\nSTAGE2: modeY mapmask hist:");

    for (index = 0; index < 16; ++index)
        if (g_Video.MaskHistogram[index]) { cursor = LogPut(cursor, " 0x"); cursor = LogHexByte(cursor, (UINT)index);
                                  cursor = LogPut(cursor, "x");
                                  cursor = LogHex(cursor, g_Video.MaskHistogram[index]); }

    cursor = LogPut(cursor, "\r\n");
    return cursor;
}

/* End of run: what the guest did with VESA -- the framebuffer it wrote, the mode it set, the calls and queries it made, and the modes nobody supports. */
static PSTR ReportVesa(PSTR cursor, PSTR const base)
{
    INT index;
    INT count;

    if (g_Video.VramNonZero)
    {
        UINT32 pitch = g_Video.VesaStride ? g_Video.VesaStride : 1;
        cursor = LogPut(cursor, "STAGE2: VESA framebuffer WRITTEN: lo=0x"); cursor = LogHex(cursor, g_Video.VramLow);
        cursor = LogPut(cursor, " hi=0x"); cursor = LogHex(cursor, g_Video.VramHigh);
        cursor = LogPut(cursor, " nonzero=0x"); cursor = LogHex(cursor, g_Video.VramNonZero);
        cursor = LogPut(cursor, "  => row "); cursor = LogHex(cursor, g_Video.VramLow / pitch);
        cursor = LogPut(cursor, " col "); cursor = LogHex(cursor, (g_Video.VramLow % pitch) / (pitch / (g_Video.VesaWidth ? g_Video.VesaWidth : 1)));
        cursor = LogPut(cursor, " .. row "); cursor = LogHex(cursor, g_Video.VramHigh / pitch);
        cursor = LogPut(cursor, "\r\n");
    }

    cursor = LogPut(cursor, "STAGE2: VESA mode SET (4F02): ");

    if (!g_Video.IsVesaSetSeen)
        cursor = LogPut(cursor, "never called");
    else { cursor = LogPut(cursor, "BX=0x"); cursor = LogHex(cursor, (DWORD)g_Video.VesaSetBx);
           cursor = LogPut(cursor, g_Video.IsVesaSetOk ? " ACCEPTED" : " REFUSED");
           cursor = LogPut(cursor, (g_Video.VesaSetBx & 0x4000) ? " [LFB]" : " [banked]");
           cursor = LogPut(cursor, " -> "); cursor = LogHex(cursor, (DWORD)g_Video.VesaWidth);
           cursor = LogPut(cursor, "x"); cursor = LogHex(cursor, (DWORD)g_Video.VesaHeight);
           cursor = LogPut(cursor, "x"); cursor = LogHex(cursor, (DWORD)g_Video.VesaBpp);
           cursor = LogPut(cursor, " stride=0x");
           cursor = LogHex(cursor, g_Video.VesaStride); }

    cursor = LogPut(cursor, "\r\n");
    cursor = LogPut(cursor, "STAGE2: VESA calls by sub-function:");
    { INT any = 0;

      for (index = 0; index < 0x16; ++index) if (g_Video.VesaCalls[index])
      {
          UINT barIndex;
          any = 1;
          cursor = LogPut(cursor, " 4F"); cursor = LogHexByte(cursor, (UINT)index); cursor = LogPut(cursor, "x"); cursor = LogHex(cursor, g_Video.VesaCalls[index]);

          if (g_Video.VesaBl[index])
          {
              cursor = LogPut(cursor, "(bl:");

              for (barIndex = 0; barIndex < 16; ++barIndex) if (g_Video.VesaBl[index] & (1u << barIndex))
              {
                  cursor = LogHexByte(cursor, barIndex == 15 ? 0x80u : barIndex);
                  cursor = LogPut(cursor, ",");
              }

              cursor = LogPut(cursor, ")");
          }
      }

      if (!any)
          cursor = LogPut(cursor, " none");

      if (g_Video.VesaCalls[7])
      {
          cursor = LogPut(cursor, " | 4F07 max start=("); cursor = LogHex(cursor, (DWORD)g_Video.Vesa07MaxX);
          cursor = LogPut(cursor, ","); cursor = LogHex(cursor, (DWORD)g_Video.Vesa07MaxY);
          cursor = LogPut(cursor, ") refused="); cursor = LogHex(cursor, g_Video.Vesa07Rejected);
      }

      /* #53: the 4F0Ah block's port writes -- a client switching banks without INT 10h
       * shows here and NOT in the 4F05 count above.
       */
      if (g_Video.VbePmBankCount | g_Video.VbePmStartCount | g_Video.VbePmRejected)
      {
          cursor = LogPut(cursor, " | 4F0A-block banks=0x"); cursor = LogHex(cursor, g_Video.VbePmBankCount);
          cursor = LogPut(cursor, " starts=0x"); cursor = LogHex(cursor, g_Video.VbePmStartCount);
          cursor = LogPut(cursor, " refused=0x"); cursor = LogHex(cursor, g_Video.VbePmRejected);
      }

      cursor = LogPut(cursor, "\r\n");
      LogAppend(LOG_PATH, base, cursor);
      SerialOut(base, cursor);
      cursor = base; }
    cursor = LogPut(cursor, "STAGE2: VESA mode queries (4F01/4F02):");

    if (!g_Video.VesaQueryCount)
        cursor = LogPut(cursor, " none");
    else for (index = 0; index < g_Video.VesaQueryCount; ++index)
    {
        cursor = LogPut(cursor, " 4F"); cursor = LogHexByte(cursor, (UINT)g_Video.VesaQueryFunction[index]);
        cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, (DWORD)g_Video.VesaQueries[index]);
        cursor = LogPut(cursor, g_Video.VesaQueryOk[index] ? "=OK" : "=UNSUPPORTED");
    }

    cursor = LogPut(cursor, "\r\n");
    cursor = LogPut(cursor, "STAGE2: video modes unsupported:");

    for (index = 0, count = 0; index < 256; ++index)
        if (VIDEO_UNIMPLEMENTED_GET(g_Video.UnimplementedModes, index))
        {
            cursor = LogPut(cursor, " 0x");
            cursor = LogHexByte(cursor, (UINT)index);
            ++count;
        }

    if (!count)
        cursor = LogPut(cursor, " none");

    cursor = LogPut(cursor, "\r\n");
    return cursor;
}

static PSTR ReportModeYBarDump(PSTR cursor, PSTR const base)
{
    /* DUMP THE BAR'S FOUR PLANES SO THE WAD CAN JUDGE THEM:
     * Everything measured so far describes the SCREEN, and the screen is planes plus
     * a render. The oracle can only say "this pixel is wrong"; it cannot say which
     * plane holds the right byte, because by then the four have been interleaved.
     * - WHAT THIS SETTLES. Measured against the IWAD on the last run's captures:
     *   plane 1's columns are 70.3% correct and planes 0/2/3 are 33.6/29.3/27.3%, and
     *   "plane 1 replicated across the group" explains 63.0% of bar pixels against a
     *   40.1%-correct baseline. So plane 1's content is reaching the other three. What
     *   no capture can distinguish is whether 0/2/3 hold a LITERAL COPY of plane 1
     *   (one writer smearing) or their own damaged content that merely resembles it
     *   (a per-plane fault). Comparing the planes to each other answers that, and the
     *   answer picks between two completely different fixes.
     * - ALSO REFUTED, AND WHY THIS IS NOT THE LATCH DUMP IT LOOKS LIKE: the
     *   write-mode-1 bursts only ever touch plane offsets 0x3a1c..0x3e7f, i.e. rows
     *   186..199. Rows 168..185, which no burst reaches, are MORE wrong (62.2% against
     *   56.9%). The latch copy is not the status bar's cause; do not rebuild the A0000
     *   trap on the strength of session 22's note. See build/barprof.py.
     *   All three pages, because the pages have been equal to the digit before and
     *   that is itself a fact worth re-checking. Mode-Y runs only; ~67 KB, one shot.
     */
    if (g_ModeYRemap && g_Video.ModeKind == VIDEO_KIND_LINEAR8 && !g_Video.IsChain4)
    {
        UINT32 page;
        UINT32 plane;
        UINT32 row;
        CHAR lineBuffer[220];
        CHAR *lineCursor;
        LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;  /* keep the log in order */
        lineCursor = lineBuffer; lineCursor = LogPut(lineCursor, "MODEYBAR dump: 3 pages x 4 planes x rows 168..199, "
                               "80 bytes/row (plane offset = row*80 + x/4)\r\n");
        LogAppend(LOG_PATH, lineBuffer, lineCursor); SerialOut(lineBuffer, lineCursor);

        for (page = 0; page < 3; ++page)
            for (plane = 0; plane < VIDEO_PLANES; ++plane)
                for (row = 168; row < 200; ++row)
                {
                    UINT32 position = (page * 0x4000u + row * 80u) & (MODEY_WIN - 1u);
                    UINT32 pixelX;
                    const BYTE *source = (const BYTE *)g_ModeYView[plane];
                    lineCursor = lineBuffer;
                    lineCursor = LogPut(lineCursor, "MODEYBAR pg"); lineCursor = LogHexByte(lineCursor, page);
                    lineCursor = LogPut(lineCursor, " pl");         lineCursor = LogHexByte(lineCursor, plane);
                    lineCursor = LogPut(lineCursor, " y");          lineCursor = LogHexByte(lineCursor, row);
                    lineCursor = LogPut(lineCursor, " ");

                    for (pixelX = 0; pixelX < 80; ++pixelX)
                        lineCursor = LogHexByte(lineCursor, source[(position + pixelX) & (MODEY_WIN - 1u)]);

                    lineCursor = LogPut(lineCursor, "\r\n");
                    /* File only: 67 KB down a 115200 COM1 is ~6 s of wind-down for a
                     * dump nobody reads off the serial line.
                     */
                    LogAppend(LOG_PATH, lineBuffer, lineCursor);
                }

        /* AND THE LINEAR SECTION, WHICH IS THE ONE PLACE NOBODY HAS LOOKED:
         * ModeYRemapInitialize() sets g_ModeYCurrent = 4 and a chain4 change selects 4, so A0000
         * maps g_ModeYSeconds[4] -- NOT any plane -- both before the first map-mask write and
         * for as long as the guest stays chained. Anything the guest writes to A0000
         * in either window lands here and is invisible to all four planes, for good.
         * That is the exact shape the evidence demands: the bar is wrong from the
         * FIRST frame and flat afterwards, the fully-redrawn surfaces (title screen
         * 0-of-64000, the 3D view) are perfect, and every "what corrupts it during
         * play" candidate has come back excluded. Write-once damage needs a
         * write-once mechanism, and this is one.
         * In chained mode the byte at offset o IS pixel (o%320, o/320), so the bar
         * region is rows 168..199 at 320 bytes a row -- no plane stride. Score it
         * against STBAR directly: a good score means Doom drew the bar while the
         * window pointed here and the planes never received it.
         */
        lineCursor = lineBuffer; lineCursor = LogPut(lineCursor, "MODEYLIN dump: linear section, rows 168..199, "
                               "320 bytes/row in 4 chunks (offset = row*320 + x)\r\n");
        LogAppend(LOG_PATH, lineBuffer, lineCursor); SerialOut(lineBuffer, lineCursor);

        for (row = 168; row < 200; ++row)
        {
            UINT32 quarter;
            UINT32 pixelX;

            for (quarter = 0; quarter < 4; ++quarter)
            {
                UINT32 position = (row * 320u + quarter * 80u) & (MODEY_WIN - 1u);
                const BYTE *source = (const BYTE *)g_ModeYView[4];
                lineCursor = lineBuffer;
                lineCursor = LogPut(lineCursor, "MODEYLIN y"); lineCursor = LogHexByte(lineCursor, row);
                lineCursor = LogPut(lineCursor, " q");         lineCursor = LogHexByte(lineCursor, quarter);
                lineCursor = LogPut(lineCursor, " ");

                for (pixelX = 0; pixelX < 80; ++pixelX)
                    lineCursor = LogHexByte(lineCursor, source[(position + pixelX) & (MODEY_WIN - 1u)]);

                lineCursor = LogPut(lineCursor, "\r\n");
                LogAppend(LOG_PATH, lineBuffer, lineCursor);
            }
        }
    }

    return cursor;
}

/* The end-of-run report, section by section, in the order the log has always had them. */
PSTR ReportEndOfRun(
    PSTR cursor,
    PSTR const base,
    PCSTR const reportEnd,
    DOS_MACHINE *machine,
    volatile BYTE * const tib)
{
    cursor = ReportNtvdmBops(cursor);
    cursor = ReportHotPortsAndTimerCounters(cursor);
    cursor = ReportPresentTiming(cursor);
    cursor = ReportPitPacingAndHostCpuTime(cursor);
    cursor = ReportRetracePolling(cursor);
    cursor = ReportCpuSpeedGovernor(cursor);
    cursor = ReportDeliveryLatencies(cursor);
    cursor = ReportDmxTaskAndShimIrqs(cursor);
    cursor = ReportHostLock(cursor);
    cursor = ReportKeyboardControllerAndFontQueries(cursor, base);
    cursor = ReportExecEventsAndIrq0Timeline(cursor, base);
    cursor = ReportV86StringTiming(cursor);
    cursor = ReportUnclaimedPorts(cursor);
    cursor = ReportUnimplemented(cursor, machine);
    { UINT plane, nonZero[VIDEO_PLANES];

      for (plane = 0; plane < VIDEO_PLANES; ++plane) { UINT byteIndex2, changed = 0;
          { const BYTE *planeBytes = g_Video.YMapPlane ? g_Video.YMapPlane(g_Video.YMapContext, plane) : g_Video.Planes[plane];

            for (byteIndex2 = 0; byteIndex2 < VIDEO_PLANE_SIZE; ++byteIndex2)
                if (planeBytes[byteIndex2])
                    ++changed; }

          nonZero[plane] = changed; }

      cursor = ReportSoundStack(cursor);
      cursor = ReportSbReplay(cursor, base);
      cursor = ReportDpmiSimulatedInterrupts(cursor, base);
      cursor = ReportPmReflectedInterrupts(cursor, base);
      cursor = ReportTimerAndInterruptDelivery(cursor, base);
      cursor = ReportDmaAndSoundBlaster(cursor);
      cursor = ReportDmxTaskAndDmaPolls(cursor);
      cursor = ReportDeviceIrqRetriesAndSoundBlocks(cursor, base, reportEnd);
      cursor = ReportAudioDevices(cursor);
      cursor = ReportPlanarVideoState(cursor, reportEnd);
      cursor = ReportPlanarSites(cursor, base, reportEnd);
      cursor = ReportCrtcAndVideoNow(cursor, nonZero);
    }
    cursor = ReportGuestStateAtExit(cursor, base, tib);
    cursor = ReportInterpreterBailSites(cursor, base, reportEnd);
    cursor = ReportModeSets(cursor);
    cursor = ReportModeY(cursor);
    cursor = ReportVesa(cursor, base);
    cursor = ReportModeYBarDump(cursor, base);

    if (g_CpuSpeedPeriods)
        CpuSpeedTimelineDump("STAGE2: CTL");                                    /* #225 */

    cursor = LogPut(cursor, "STAGE2: complete\r\n");
    LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;   /* headless: mirror the DOS-output flush + completion to COM1 */
    return cursor;
}
