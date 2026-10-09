/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Time: the PIT and its pacer, BIOS ticks, IRQ0 delivery, the CPU-speed governor,
 *   the RTC and vertical retrace.
 *
 * Declarations only (#335): defined in host_timing.c.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_HOST_TIMING_H
#define NTVDMEX_HOST_TIMING_H

#include "host_state.h"

extern LONGLONG g_Irq0TimePrevious;
extern LONGLONG g_Irq0Start;
extern DWORD g_Irq0AttemptsCount;
extern DWORD g_Irq0NieCount;
extern DWORD g_Irq0YieldCount;
extern DWORD g_Irq0RaiseCount;
extern DWORD g_IrqNInjected;
extern DWORD g_IrqNRefuseTotal;
extern CMOS_STATE g_Cmos;
extern NTVDD_DEVICE g_CmosDevice;
extern UINT32 g_PitSyncs;
extern LONG g_PmTickOwedMaximum;
extern UINT32 g_PmOwedHistogram[9];
extern DWORD g_TickGapMaximumMicroseconds;
extern DWORD g_TickGap[12];
extern DWORD g_TickGapOver;
extern CRITICAL_SECTION g_PitCs;
extern DWORD g_EventIo;
extern DWORD g_Irq0NoteCs;
extern DWORD g_Irq0NoteIp;
extern DWORD g_Irq0Skip;
extern DWORD g_Irq0SkipIf;
extern DWORD g_Irq0SkipStub;
extern DWORD g_EventIntPending;
extern DWORD g_EventHistogram[EV_HIST_MAX];
extern DWORD g_V86MicrosecondsTotal;
extern DWORD g_HostMicrosecondsEvent[EV_HIST_MAX];
extern DWORD g_BopHistogram[BYTE_VALUES];
extern DWORD g_PitPaceCalls;
extern DWORD g_XsStart;
extern DWORD g_XsSnapshot[XS_SECS][XS_N];
extern DWORD g_XsSeconds;
extern DWORD g_IrqNRetryTry;
extern DWORD g_IrqNRetryWhy;
extern DWORD g_IrqNRetryOk;
extern DWORD g_Irq0IsrSince;
extern DWORD g_Irq0IsrBlocks;
extern DWORD g_Irq0IsrTimeouts;
extern DWORD g_Irq0IsrStrict;
extern DWORD g_Irq0IsrAuto;
extern INT g_Irq0AutoEoi;
extern INT g_PitPacePriority;
extern INT g_PitPaceInject;
extern DWORD g_CpuSpeedDebtMaximumMicroseconds;
extern DWORD g_CpuSpeedRanMicroseconds;
extern DWORD g_CpuSpeedWallMicroseconds;
extern UINT g_CpuSpeedGranularityMs;
extern DWORD g_CpuSpeedRoundTripMicroseconds;
extern DWORD g_CpuSpeedPeriodMs;
extern HANDLE g_CpuSpeedRelease;
extern INT g_CpuAffinityOn;
extern DWORD g_CpuAffinityRest;
extern DWORD g_CpuAffinityCpuCount;
extern DWORD g_CpuAffinityGuest;
extern UINT32 g_PitCatchupClamped;
extern UINT32 g_PitGapMaximum;
extern DWORD g_VbeWaits;
extern volatile DWORD g_RetraceCs;
extern volatile DWORD g_RetraceIp;
extern volatile DWORD g_RetraceAl;
extern volatile DWORD g_RetracePending;
extern volatile DWORD g_RetraceIdles;
extern volatile DWORD g_RetraceCx;

UINT64 HostTimeMicroseconds(VOID);

VOID HostPitResyncCheck(VOID);
VOID HostPitSync(VOID);
VOID PitLatchNote(BYTE command);
VOID RetraceNote(volatile BYTE *tib, WORD port, INT isIn, DWORD cs, DWORD ipAfter);
VOID Irq0Latch(VOID);
INT PmTickTake(VOID);
VOID TickDeliveredNote(VOID);
INT Irq0CanDeliver(VOID);
VOID Irq0Ack(VOID);
VOID Irq0DeliveredNote(VOID);
INT Irq0PmClaim(VOID);
VOID Irq0PmUnclaim(VOID);
LONGLONG Int15QpcAfterMicroseconds(DWORD microseconds);
VOID PitPacerTimerStart(HMODULE winmmModule);
DWORD WINAPI PitPacerThread(LPVOID param);
DWORD WINAPI TickCourierThread(LPVOID parameter);
UINT HostCpuMhz(VOID);
VOID ExecEnterMark(VOID);
VOID ExecLeaveMark(VOID);
VOID CpuSpeedCooperativePark(VOID);
VOID CpuSpeedTimelineDump(PCSTR tag);
VOID CpuSpeedRecompute(VOID);
DWORD WINAPI CpuSpeedThread(LPVOID param);
DWORD WINAPI HeartbeatThread(LPVOID parameter);
VOID ExecShareReport(VOID);
DWORD WINAPI HeadlessDeadlineThread(LPVOID parameter);
VOID BackgroundPriorityTick(HWND window);
VOID HostRtcNow(PVOID context, PIT_RTC_READING *out);
INT HostRtcSet(PVOID context, const PIT_RTC_READING *reading, INT what);
INT HostTickTake(UINT32 *ticks, UINT32 *wraps, UINT32 *since);
VOID HostTicksSet(PVOID context, UINT32 ticks);
VOID HostSetTicks(PVOID context, UINT32 ticks);
VOID HostPitGuard(PVOID context, INT enter);
VOID HostPitGenerate(VOID);
VOID Int10WaitAfter(VOID);
VOID RetraceIdle(VOID);

#endif
