/* host_wow.h -- Win16: the WOW glue -- module loading and anchors, the scheduler, WOW32 calls,
 *   the shims, 16-bit callbacks and owner-draw.
 *
 * Declarations only (#335): defined in host_wow.c. */
#ifndef NTVDMEX_HOST_WOW_H
#define NTVDMEX_HOST_WOW_H
#include "host_state.h"

PVOID ShimMapFlat(WORD segment, DWORD offset, INT isProtectedMode);
VOID WowShimsLoad(VOID);

/* Defined in host_wow.c (#335). */
INT WowDlgIsSelectorAbsent(WORD selector);
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
INT WowCall16SyncEx(DWORD proc, WORD ds, const WORD *args, INT argumentCount, WORD hwnd, WORD message, WORD *result, BYTE *blob, INT blobLength, INT blobArgument, const INT *fix, INT fixupCount);
VOID Wow32CurrentDirectorySet(PCSTR directory);
extern INT g_WowFoldMute;
extern DWORD g_WowFoldDropped;
extern DWORD g_IcaRaised;
extern DWORD g_IcaNoHandler;
extern DWORD g_IcaDelivered;
extern DWORD g_ShimState[WOW_SHIMS];
extern DWORD g_ShimError[WOW_SHIMS];
VOID WowLogFlush(PSTR base, PSTR *logCursor);
INT LaunchIsWow(PCSTR command);
extern CHAR g_WowName[WOW_MAX_MOD][16];
INT WowModuleOfSelector(WORD selector);
INT WowUserAnchor(WORD thunkId, WORD argumentBytes, WORD returnStub);
INT WowShellAnchor(WORD thunkId, WORD argumentBytes, WORD returnStub);
INT WowCommonDialogAnchor(WORD thunkId, WORD argumentBytes, WORD returnStub);
INT WowKeyboardAnchor(WORD thunkId, WORD argumentBytes, WORD returnStub);
INT WowSoundAnchor(WORD thunkId, WORD argumentBytes, WORD returnStub);
INT WowGdiAnchor(WORD thunkId, WORD argumentBytes, WORD returnStub);
INT WowKernel2Stub(WORD thunkId, WORD returnStub);
WORD WowHostAllocate(WORD paras);
extern WORD g_WowEntryCx;
extern WORD g_WowPspSegment;
extern WORD g_WowPathSegment;
extern WORD g_WowEnvironmentSegment;
extern DWORD g_WowCallbackLinear;
PSTR WowPspEnvironmentCheck(PSTR cursor, PCSTR where);
VOID WowProbeLoad(PCSTR command);
INT WowRefuse(PCSTR command);
WORD WowCallbackSelector(VOID);
VOID WowProbeLdtMatrix(PCSTR tag);
INT WowPlaceV86(DOS_MACHINE *machine, WORD *entryCs, WORD *eip, WORD *entryDs, WORD *entrySs, WORD *esp);
VOID WowProbeSelectors(VOID);
DWORD Wow32ReturnOverride(WORD thunkId);
INT Wow32ModeOverride(WORD thunkId);
VOID Wow32ModeLoad(VOID);
VOID Wow32ReturnLoad(VOID);
extern WORD g_WowDgroupSelector;
INT WowSchedFree(VOID);
INT WowSchedPick(WORD current);
INT WowSchedRunnable(WORD current, INT depth);
INT WowSchedTopLevel(const WOWSCHED_SLOT *slot);
WORD WowSchedCurrentTask(VOID);
VOID WowTaskDirectoryHere(WORD task);
VOID WowTaskChdir(WORD task, PSTR *logCursor);
VOID WowSchedSetCurrent(WORD task);
INT WowSchedRetarget(WORD hwnd, WORD *stackSegment, WORD *stackPointer, DWORD *stackSegmentBase, WORD *prev);
VOID WowSchedUntarget(WORD prev);
INT WowSchedInterTaskLive(VOID);
VOID WowQuietLoad(VOID);
VOID WowSchedLoad(VOID);
extern INT g_WowCallOn;
VOID WowCallLoad(VOID);
DWORD Wow32HostSelectorToLinear(WORD selector, PVOID context);
VOID WowShadowPut(INT index);
INT WowShadowSync(PSTR *logCursor);
INT WowVendorApiEntry(DOS_MACHINE *machine, WORD *selector, WORD *offset);
INT WowCall16Sync(DWORD proc, WORD ds, const WORD *args, INT argumentCount, WORD hwnd, WORD message, WORD *result);
DWORD ShimGlobal16(INT operation, DWORD firstArgument, DWORD secondArgument);
LRESULT WowControlColour(HWND window, WORD window16, UINT message, WPARAM wParam, LPARAM lParam, INT *handled);
LRESULT WowOwnerDraw(HWND window, WORD window16, UINT message, WPARAM wParam, LPARAM lParam, INT *handled);
INT WowSend16Blob(WORD window16, WORD message, WORD wParam, BYTE *blob, INT blobLength, const INT *fix, INT fixupCount, WORD *result);
INT WowSend16Now(WORD window16, WORD message, WORD wParam, DWORD lParam, WORD *result);
VOID WowIcaDeliver(DOS_MACHINE *machine, volatile BYTE *tib, UINT steps);
#endif
