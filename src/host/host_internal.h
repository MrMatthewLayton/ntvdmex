/* host_internal.h -- what the host_*.c files share: the types, macros and state
 * used by more than one of them, and a prototype for every function called across
 * files. Included once, by main.c, after its prelude. */
#ifndef NTVDMEX_HOST_INTERNAL_H
#define NTVDMEX_HOST_INTERNAL_H

#include "host_types.h"
#include "host_state.h"
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

static VOID WowIcaDeliver(DOS_MACHINE *machine, volatile BYTE *tib, UINT steps);
static VOID DpmiInstall(INT index);           /* defined just below; used by the helper */
static VOID WowShadowPut(INT index);         /* GH #128: keep the descriptor shadow in step */
static VOID DpmiBreakpointArm(VOID);               /* fwd: a new region may hold a requested BP */
static VOID DpmiBreakpointRearmPending(DWORD currentLinear);   /* fwd: re-plant stepped-over breakpoints */
static VOID DpmiEnsurePmReturnSelector(VOID);   /* fwd: shared PM-return catcher installer (#2b + 0303) */
/* State used from a file other than its owner's (tentative definitions). */
static INT g_WowFoldMute;
static DWORD g_WowFoldDropped;
static DWORD g_IcaRaised, g_IcaDelivered, g_IcaNoHandler;
static DWORD g_WowIdleWaits;
static DWORD g_ShimState[WOW_SHIMS], g_ShimError[WOW_SHIMS];
static INT g_PmIrq0Latch;
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
static DWORD g_PmIrqReflects;
static INT g_NoPmPatch;
static DWORD g_NoPmPatchMinimum;
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
static WORD g_LdtFree[DPMI_LDT_MAX];
static INT g_LdtFreeCount;
static INT g_PmWatchCount;
static DWORD g_PmCooperativeLine[PIC_LINES_PER_CHIP];
static INT g_PmTopDispatch;
static INT g_PmDispatchTop;
static INT g_SimIntReflect;
static CHAR g_WowName[WOW_MAX_MOD][16];
static WORD g_WowEntryCx;
static WORD g_WowPspSegment;
static WORD g_WowPathSegment;
static WORD g_WowEnvironmentSegment;
static DWORD g_WowCallbackLinear;
static DWORD g_WowPspLinear[WOW_PSP_TRACK];
static WORD g_WowPspEnvironment[WOW_PSP_TRACK];
static WORD g_PmTransferParagraphs;
static UINT g_DmaPollOverflow;
static UINT g_PollStackOverflow;
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
static VOID WowLogFlush(PSTR base, PSTR *logCursor);
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
static INT DpmiHostIndex(VOID);
static WORD DpmiHandlerCodeSelector(VOID);
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
static VOID DpmiEnsurePmReturnSelector(VOID);
static INT WowCall16Sync(DWORD proc, WORD ds, const WORD *args, INT argumentCount, WORD hwnd, WORD message, WORD *result);
static DWORD ShimGlobal16(INT operation, DWORD firstArgument, DWORD secondArgument);
static INT DpmiNestedFault(volatile BYTE *tib, DWORD event, DWORD eip);
static LRESULT WowControlColour(HWND window, WORD window16, UINT message, WPARAM wParam, LPARAM lParam, INT *handled);
static LRESULT WowOwnerDraw(HWND window, WORD window16, UINT message, WPARAM wParam, LPARAM lParam, INT *handled);
static INT WowSend16Blob(WORD window16, WORD message, WORD wParam, BYTE *blob, INT blobLength, const INT *fix, INT fixupCount, WORD *result);
static INT WowSend16Now(WORD window16, WORD message, WORD wParam, DWORD lParam, WORD *result);
static INT DpmiInjectPmIrq(DOS_MACHINE *machine, volatile BYTE *tib, UINT interruptVector, UINT steps);
static VOID WowIcaDeliver(DOS_MACHINE *machine, volatile BYTE *tib, UINT steps);
static INT DpmiInjectPmMouseCallback(DOS_MACHINE *machine, volatile BYTE *tib, UINT steps);
static VOID DpmiClientTeardown(VOID);

#endif
