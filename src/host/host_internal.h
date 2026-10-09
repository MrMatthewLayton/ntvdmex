/* host_internal.h -- what the host_*.c files share: the types, macros and state
 * used by more than one of them, and a prototype for every function called across
 * files. Included once, by main.c, after its prelude. */
#ifndef NTVDMEX_HOST_INTERNAL_H
#define NTVDMEX_HOST_INTERNAL_H

#include "host_types.h"
#include "host_state.h"
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

static VOID HostProfileDump(VOID);
static VOID WowIcaDeliver(DOS_MACHINE *machine, volatile BYTE *tib, UINT steps);
static VOID ModeYTimelineReport(VOID);          /* fwd: north star 1, with the mode-Y remap */
static INT  ModeYInterpServes(VOID);      /* fwd: north star 1, design C */
static VOID ModeYRingDump(PCSTR why);  /* fwd: north star 1, design C */
static VOID ModeYRingNoteIrq(UINT vector, WORD cs, WORD ip, WORD ss, WORD sp);
static INT InterpreterMemoryPageOk(UINT32 linear);        /* fwd: page-validity guard, defined with V86HostRead8 */
static VOID ExecMachineSave(INT depth);    /* fwd: defined with CloseProgramNow */
static BYTE  g_FaultTable[DOS_FLTSITE_N * DPMI_FAULT_TABLE_ENTRY] __attribute__((aligned(16)));
static BYTE  g_FaultStack[DPMI_FAULT_STK_SIZE] __attribute__((aligned(16)));   /* #205 */
static struct _DPMI_DESCRIPTOR { DWORD Base, Limit; BYTE Access, Flags; } g_Ldt[DPMI_LDT_MAX];
static INT AsyncVectorIsOurStub(UINT irq);
static VOID HostPitGenerate(VOID);         /* fwd: the crystal half (g_PitCs only)  */
static VOID HostPitDeliver(VOID);          /* fwd: the attempt half (g_Lock, by TRY) */
static INT  V86DeliverDeviceIrq(volatile BYTE *tib);  /* fwd: shared by the main and nested V86 loops */
static INT  DpmiAsyncInjectPm(UINT irq, CONTEXT *context);
static VOID MouseChildExited(VOID);          /* fwd: see g_MouseWantRelease */
static HANDLE StdioPebHandle(HANDLE proc, UINT offset);
static VOID HostRecordFinish(VOID);        /* below: patches the header, logs */
static VOID VideoTrapSync(VOID);             /* fwd */
static WORD DpmiSegmentToDescriptor(WORD segment);
static VOID HostFullscreenToggle(HWND window);
static VOID ModeYGr4CloseRun(VOID);       /* defined with the GR4 counters below */
static VOID ModeYRemapSelectBody(PVOID context, INT mask);
static VOID InterpreterMemoryBadNote(UINT32 linear, INT write);   /* defined after v86interp.h (needs icpu) */
static VOID HostPitGenerate(VOID);
static VOID DpmiInstall(INT index);           /* defined just below; used by the helper */
static VOID WowShadowPut(INT index);         /* GH #128: keep the descriptor shadow in step */
static VOID DpmiBreakpointArm(VOID);               /* fwd: a new region may hold a requested BP */
static VOID DpmiBreakpointRearmPending(DWORD currentLinear);   /* fwd: re-plant stepped-over breakpoints */
static INT DpmiServicePmInt(DOS_MACHINE *machine, volatile BYTE *tib, DWORD vector, UINT steps);
static VOID DpmiEnsurePmReturnSelector(VOID);   /* fwd: shared PM-return catcher installer (#2b + 0303) */
static INT V86BiosBop(volatile BYTE *tib, UINT bopNumber, PSTR *logCursor, PSTR base);
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
static INT WowCall16SyncEx(DWORD proc, WORD ds, const WORD *args, INT argumentCount,
                              WORD hwnd, WORD message, WORD *result,
                              BYTE *blob, INT blobLength, INT blobArgument,
                              const INT *fix, INT fixupCount);
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
static WORD g_DsProbe[DSPROBE_MAX];
static INT g_DsProbeCount;
static WORD g_CsProbe[DSPROBE_MAX];
static INT g_CsProbeCount;
static INT g_Routed;
static INT g_BackToPrompt;
static VDM_COMMAND_INFO g_CommandInfo;
static CMOS_STATE g_Cmos; static NTVDD_DEVICE g_CmosDevice;
static EMU8K_STATE g_Emu8K; static NTVDD_DEVICE g_Emu8KDevice;
static INT g_AweOn;
static INT g_DosVersionForced;
static PCSTR g_DosVersionWhy;
static MPU_STATE g_Mpu; static NTVDD_DEVICE g_MpuDevice;
static NETBIOS_STATE g_Net; static NTVDD_DEVICE g_NetDevice;
static BYTE g_GenericStubVector[DOS_GENSTUB_N];
static INT g_WowFoldMute;
static DWORD g_WowFoldDropped;
static PVOID g_Hma;
static DWORD g_HmaError;
static DWORD g_HmaState, g_HmaProtection;
static DOS_EMS_STATE g_Ems;
static DWORD g_IcaRaised, g_IcaDelivered, g_IcaNoHandler;
static DWORD g_WowIdleWaits;
static DWORD g_ShimState[WOW_SHIMS], g_ShimError[WOW_SHIMS];
static UINT32 g_PitSyncs;
static UINT32 g_PitAsyncAttempts;
static DWORD g_PitDeliverSkipped;
static LONG g_PmTickOwedMaximum;
static UINT32 g_PmOwedHistogram[9];
static DWORD g_TickGap[12], g_TickGapMaximumMicroseconds, g_TickGapOver;
static INT g_PmIrq0Latch;
static DWORD g_UiTickSkips;
static DWORD g_UiHookPresents, g_UiTimerPresents;
static DWORD g_Irq1Checks, g_Irq1NoIf, g_Irq1In08, g_Irq1In09;
static DWORD g_Irq1AsyncInjected;
static DWORD g_Irq1AsyncRetry;
static CRITICAL_SECTION g_PitCs;
static INT g_PmVehPass;
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
static volatile LONG g_DpmiWatchdogGeneration;
static INT g_Headless;
static DWORD g_EventIo;
static LONGLONG g_Irq0Start, g_Irq0TimePrevious;
static DWORD g_Irq0IoPrevious, g_Irq0WorstGapMs, g_Irq0WorstGapIo;
static DWORD g_Irq0WorstCs, g_Irq0WorstIp;
static DWORD g_Irq0NoteCs, g_Irq0NoteIp;
static DWORD g_Irq0RaiseCount, g_Irq0AttemptsCount, g_Irq0NieCount, g_Irq0YieldCount;
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
static DWORD g_EventHistogram[EV_HIST_MAX];
static DWORD g_V86MicrosecondsTotal, g_HostMicrosecondsEvent[EV_HIST_MAX];
static DWORD g_BopHistogram[BYTE_VALUES];
static DWORD g_PitPaceCalls;
static DWORD g_XsStart, g_XsSeconds, g_XsSnapshot[XS_SECS][XS_N];
static UINT g_CaptureMs;
static DWORD g_CaptureDelayMs, g_CaptureStart;
static INT g_Capture;
static INT g_NoA000;
static INT g_NoPmPatch;
static DWORD g_NoPmPatchMinimum;
static DWORD g_MemoryDumpLinear, g_MemoryDumpLength;
static INT g_P12Offset;
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
static WORD g_PmAppTimerSelector;
static DWORD g_PmAppTimerOffset;
static WORD g_PmDefaultSelector;
static volatile LONG g_CloseRequest;
static INT g_TopIsShell;
static WORD g_LdtFree[DPMI_LDT_MAX];
static INT g_LdtFreeCount;
static HANDLE g_ComSpool[COMM_MAX_PORTS];
static INT g_ComFailed[COMM_MAX_PORTS];
static INT g_BdaReady;
static WORD g_Int15StubOffset;
static DWORD g_PrintScreenJobs, g_PrintScreenErrors;
static BYTE g_PrintScreenStatus;
static DWORD g_IrqNRetryTry, g_IrqNRetryOk, g_IrqNRetryWhy;
static DWORD g_IrqRaisedAny;
static DWORD g_QiBits;
static INT g_QiKeysAsync;
static volatile LONG g_AsyncContextWrite;
static INT g_BehaveDos622;
static DWORD g_PauseCount, g_PauseCooperative, g_PauseMs;
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
static DWORD g_IfvCensus[IFV_PATHS][8];
static DWORD g_IfvShadow[PIC_LINES];
static DWORD g_IfvStarveT0, g_IfvStarveMaximumMs, g_IfvStarveCount;
static INT g_IfvStarveOpen;
static LONG g_IfvTraceCount;
static DWORD g_IfvReenter[PIC_LINES];
static DWORD g_AsyncPmInjected;
static DWORD g_AsyncInjectedLine[PIC_LINES];
static DWORD g_PmWatch[DPMI_WATCH_MAX];
static INT g_PmWatchCount;
static BYTE g_PmWatchRel[DPMI_WATCH_MAX];
static DWORD g_PmCooperativeLine[PIC_LINES_PER_CHIP];
static DWORD g_PmInjectDecl[2], g_PmInjectDeclTl[IRQ0TL_SECS];
static DWORD g_AsyncEarlyBailLogged;
static INT g_AsyncSiteCount;
static INT g_AsyncSiteFull;
static PCSTR g_FloppyImage;
static HANDLE g_Stdio;
static PCSTR g_StdioHow;
static PCSTR g_StdioSource;
static DWORD g_StdioParentProcessId;
static INT g_PitPacePriority;
static INT g_PitPaceInject;
static volatile LONGLONG g_Int15WaitEnd;
static volatile DWORD g_Int15EventLinear;
static INT g_PmTopDispatch;
static INT g_PmDispatchTop;
static UINT g_CpuSpeedReferenceMhz;
static DWORD g_CpuSpeedDebtMaximumMicroseconds;
static DWORD g_CpuSpeedRanMicroseconds;
static DWORD g_CpuSpeedWallMicroseconds;
static UINT g_CpuSpeedGranularityMs;
static DWORD g_CpuSpeedRoundTripMicroseconds;
static DWORD g_CpuSpeedPeriodMs;
static HANDLE g_CpuSpeedRelease;
static INT g_CpuAffinityOn;
static DWORD g_CpuAffinityGuest, g_CpuAffinityRest, g_CpuAffinityCpuCount;
static volatile LONG g_MouseX, g_MouseY, g_MouseButtons;
static volatile LONG g_MouseDx, g_MouseDy;
static INT g_MouseRawOk;
static DWORD g_MouseWmInput, g_MouseRawAbsolute, g_MouseI33[16], g_MouseI33Other;
static DWORD g_MouseI33AxOverflow, g_MouseI33SiteCount, g_MouseI33SiteOverflow;
static INT g_SimIntReflect;
static INT g_MouseAbsent;
static INT g_TextDump;
static volatile LONG g_MouseHidden;
static volatile LONG g_MousePressCount[MS_BTNS], g_MouseReleaseCount[MS_BTNS];
static DWORD g_MouseEdges;
static DWORD g_MouseEventInstalls;
static volatile LONG g_MouseEventPend;
static INT g_MouseCallbackTrace;
static INT g_HostCursorMode;
static volatile LONG g_MouseAutoCaptureDone;
static volatile LONG g_MouseWantRelease;
static DWORD g_MouseShapeSets;
static volatile LONG g_MouseGraphicsCursorDefined;
static DWORD g_MouseGraphicsCursorBadPointer;
static DWORD g_MouseAccelerationCalls;
static DWORD g_MouseAltCalls;
static volatile LONG g_MouseTextCursorAnd, g_MouseTextCursorXor;
static DWORD g_MouseStateBadPointer;
static DWORD g_MouseI33Unimplemented;
static CHAR g_WowName[WOW_MAX_MOD][16];
static WORD g_WowEntryCx;
static WORD g_WowPspSegment;
static WORD g_WowPathSegment;
static WORD g_WowEnvironmentSegment;
static DWORD g_WowCallbackLinear;
static DWORD g_WowPspLinear[WOW_PSP_TRACK];
static WORD g_WowPspEnvironment[WOW_PSP_TRACK];
static WORD g_PmTransferParagraphs;
static INT g_LowLevelKeyboardOn;
static HANDLE g_ExecThread;
static INT g_ExecPriorityForeground;
static NTVDMEX_SETTINGS g_SettingsDisk;
static PCSTR g_ShellOverride;
static INT g_FrameSkip;
static INT g_DspVersionForced;
static UINT g_ConventionalKbWant;
static WORD g_DosMemoryTop;
static DWORD g_WindowSizeLive;
static DWORD g_AspectLive;
static const INT g_SettingsPages[NTVDMEX_PAGE_COUNT];
static UINT32 g_PitCatchupClamped;
static UINT32 g_PitGapMaximum;
static DWORD g_VbeWaits;
static volatile DWORD g_RetracePending, g_RetraceCs, g_RetraceIp, g_RetraceAl, g_RetraceCx, g_RetraceIdles;
static UINT g_DmaPollOverflow;
static UINT g_PollStackOverflow;
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
static WORD g_WowLastId;
static WORD g_WowLastFrom;
static INT g_HostPoolSpill;
static WORD g_DpmiHandlerSelector;
static WORD g_WowDgroupSelector;
static DWORD g_WowSchedSwitches;
static WORD g_WowSchedLaunchChild, g_WowSchedShell;
static INT g_WowCallOn;
static BYTE g_PmDispatch[IVT_VECTORS];
static DWORD g_PmDispatchCount[IVT_VECTORS][BYTE_VALUES];
static DWORD g_Wow32Serviced, g_Wow32Unimplemented, g_Wow32Declined;
static DWORD g_WowSyncWrites;
/* Functions called from a file other than their own. */
static VOID DsProbeLoad(VOID);
static UINT LauncherCompilerVariables(PCSTR environment, DWORD environmentCapacity, PSTR out, DWORD outCapacity);
static VOID HmaTry(VOID);
static VOID Irq0Latch(VOID);
static INT PmTickTake(VOID);
static VOID TickDeliveredNote(VOID);
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
static VOID WowLogFlush(PSTR base, PSTR *logCursor);
static INT LptSpoolPut(BYTE character);
static VOID LptTransmitSink(PVOID context, INT port, BYTE byteValue);
static INT DosPrnOut(PVOID context, BYTE character);
static INT KeyboardActionEntry(INT keyboardAction);
static INT Int15Hooked(VOID);
static VOID DosAuxOut(PVOID context, BYTE character);
static UINT IrqPmVector(UINT irq);
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
static INT IfOrVif(DWORD flags);
static INT IsOurStubCsIp(DWORD cs, DWORD ip);
static VOID VdmStateSample(PCSTR label, volatile BYTE *tib, INT *budget);
static VOID InjectInt(volatile BYTE *tib, UINT vector);
static INT DosTerminate(DOS_MACHINE *machine, PVOID tib, PSTR *logCursor, PSTR base);
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
static VOID PlanesDumpBeside(PCSTR bitmapPath);
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
static VOID InputCaptureSet(HWND window, INT isOn);
static INT OtherHostsRunning(VOID);
static VOID BackgroundPriorityTick(HWND window);
static VOID HostPanicRelease(VOID);
static DWORD WINAPI CaptureWatchdogThread(LPVOID parameter);
static VOID HostFullscreenToggle(HWND window);
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
static DWORD WINAPI UiThread(LPVOID argument);
static VOID RegistersLoad(NTVDD_REGISTERS *registers, volatile BYTE *tib);
static VOID RegistersStore(NTVDD_REGISTERS *registers, volatile BYTE *tib);
static VOID HostRtcNow(PVOID context, PIT_RTC_READING *out);
static INT HostRtcSet(PVOID context, const PIT_RTC_READING *reading, INT what);
static INT HostTickTake(UINT32 *ticks, UINT32 *wraps, UINT32 *since);
static VOID HostTicksSet(PVOID context, UINT32 ticks);
static VOID HostSetTicks(PVOID context, UINT32 ticks);
static VOID HostPitGuard(PVOID context, INT enter);
static VOID HostPitGenerate(VOID);
static VOID HostPitDeliver(VOID);
static VOID Int10WaitAfter(VOID);
static VOID RetraceIdle(VOID);
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
