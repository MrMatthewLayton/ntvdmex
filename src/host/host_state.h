/* host_state.h -- the machine's state that the whole host shares: the virtual devices, the guest CPU context and the run-wide flags.
 *
 * Declarations only (#335): every variable is defined, with its comment, in host_state.c. */
#ifndef NTVDMEX_HOST_STATE_H
#define NTVDMEX_HOST_STATE_H
#include <windows.h>
#include "ntvdm.h"
#include "host_core.h"        /* the PFN_* types of the run-time imports */
#include "../ntvdmex_x86.h"
#include "../ntvdmex_units.h"
#include "settings.h"
#include "pcspeaker.h"
#include "../wow/ne.h"
#include "../wow/wowsched.h"
#include "dos_int21.h"
#include "dos_env.h"
#include "dos_xms.h"
#include "dos_recovery.h"
#include "vdd_bus.h"
#include "vdd_pit.h"
#include "vdd_pic.h"
#include "vdd_video.h"
#include "vdd_input.h"
#include "vdd_speaker.h"
#include "vdd_joy.h"
#include "vdd_opl.h"
#include "vdd_sb.h"
#include "vdd_gus.h"
#include "vdd_comm.h"
#include "vdd_audio.h"
#include "audio_wave.h"
#include "present_ddraw.h"
#include "host_types.h"


extern PFN_ADD_VECTORED_EXCEPTION_HANDLER g_PfnAddVeh;
extern PFN_REGISTER_RAW_INPUT_DEVICES g_PfnRegisterRawInput;
extern PFN_GET_RAW_INPUT_DATA g_PfnGetRawInput;
extern DOS_SAFE_SKIPS g_Safe;
extern DOS_MACHINE *g_Machine;
extern VDD_BUS g_Bus;
extern PIT_STATE g_Pit;
extern NTVDD_DEVICE g_PitDevice;
extern PIC_STATE g_Pic;
extern NTVDD_DEVICE g_PicDevice;
extern VIDEO_STATE g_Video;
extern NTVDD_DEVICE g_VideoDevice;
extern INPUT_STATE g_Input;
extern NTVDD_DEVICE g_InputDevice;
extern SPEAKER_STATE g_Speaker;
extern NTVDD_DEVICE g_SpeakerDevice;
extern PCSPEAKER g_PcSpeaker;
extern INT g_SpeakerReal;
extern OPL_STATE g_Opl;
extern NTVDD_DEVICE g_OplDevice;
extern SB_STATE g_Sb;
extern NTVDD_DEVICE g_SbDevice;
extern GUS_STATE g_Gus;
extern NTVDD_DEVICE g_GusDevice;
extern COMM_STATE g_Comm;
extern NTVDD_DEVICE g_CommDevice;
extern JOYSTICK_STATE g_Joystick;
extern NTVDD_DEVICE g_JoystickDevice;
extern AUDIO_STATE g_Audio;
extern AUDIO_WAVE g_Wave;
extern PRESENT_DDRAW g_PresentDdraw;
extern DOS_XMS_STATE g_Xms;
extern volatile LONG g_Irq0Pending;
extern volatile LONG g_IcaPending;
extern volatile LONG g_PmTickOwed;
extern volatile LONG g_Irq1Pending;
extern INT g_InPmIrq;
extern LARGE_INTEGER g_QpcFrequency;
extern INT g_LockSite, g_LockHoldSite, g_LockWaitSite;
extern UINT32 g_LockHoldMicroseconds, g_LockWaitMicroseconds, g_UiGapMicroseconds;
extern INT g_UiTickMinimumMs;
extern DWORD g_KeyMessageHistogram[8];
extern DWORD g_KeyMessageMaximumMs, g_KeyMessageCount;
extern DWORD g_KeyDeliveryHistogram[8];
extern DWORD g_KeyDeliveryMaximumMs, g_KeyDeliveryCount;
extern INT g_KeyIrqRetry;
extern HWND g_Window;
extern HANDLE g_KeyEvent;
extern volatile LONG g_Running;
extern INT g_DpmiPm;
extern INT g_PmClientExited;
extern DWORD g_DpmiCodeBase;
extern DWORD g_PatchMapLinear[DPMI_PMAP_SLOTS];
extern BYTE g_PatchMapVector[DPMI_PMAP_SLOTS];
extern INT g_PmNoIrq;
extern volatile LONG g_PmEntryEip;
extern INT g_BreakpointCount;
extern volatile LONG g_DpmiIteration;
extern volatile LONG g_DpmiDone;
extern DWORD g_IoExtra;
extern DWORD g_Irq0Injected;
extern DWORD g_Irq0TimeLast[IRQ0TL_SECS];
extern DWORD g_Irq0GapHistogram[8];
extern DWORD g_Irq0GapMaximumMs, g_Irq0GapCount;
extern DWORD g_Irq0AnomalyCount, g_Irq0AnomalyMicroseconds, g_Irq0AnomalyIo;
extern DWORD g_Irq0AnomalyRaise, g_Irq0AnomalyAttempts, g_Irq0AnomalyNie, g_Irq0AnomalyYield;
extern DWORD g_Irq0AnomalyGeneration, g_Irq0AnomalyDelete;
extern volatile LONG g_WoundDown;
extern INT g_IoHotCount;
extern WORD g_Unclaimed[IO_UNCLAIMED_MAX];
extern INT g_UnclaimedCount;
extern volatile DWORD g_DpmiLastCs;
extern volatile DWORD g_DpmiLastEip;
extern DWORD g_PmVector8ArmedMs;
extern INT g_PmAppHookedTimer;
extern INT g_DpmiVi;
extern BYTE g_BiosUnimplemented[BYTE_VALUES];
extern INT g_ExecDepth;
extern CHAR g_ProgramName[PROGRAM_NAME_SIZE];
extern WORD g_PmReturnSelector;
extern WORD g_DpmiFaultSelector;
extern WORD g_DpmiFaultCodeSelector;
extern INT g_LdtNext;
extern volatile BYTE *g_TibDebug;
extern volatile LONG g_IrqNPending[PIC_LINES];
extern const BYTE g_IrqOrder[14];
extern DWORD g_IrqRaised[PIC_LINES];
extern HANDLE g_HostCpu;
extern INT g_QiSuspended;
extern volatile LONG g_InExec;
extern volatile LONG g_PauseWant;
extern HANDLE g_CourierEvent;
extern DWORD g_AsyncInjected;
extern DWORD g_AsyncBail;
extern LONG g_AsyncWhy;
extern DWORD g_AsyncWhyHistogram[PIC_LINES_PER_CHIP][ASYNC_WHY_MAX];
extern volatile LONG g_AsyncPmActive;
extern DWORD g_AsyncPmEip, g_AsyncPmEsp, g_AsyncPmEflags;
extern WORD g_AsyncPmCs, g_AsyncPmSs;
extern DWORD g_LeLoadBase;
extern volatile LONG g_SimIntBusy;
extern volatile LONG g_NestedRm;
extern INT g_PitPaceOn, g_PitPaceMs;
extern volatile LONGLONG g_Int15EventEnd;
extern DWORD g_Int15Waits, g_Int15Events, g_Int15Posted, g_Int15Busy;
extern INT g_CourierOn;
extern DWORD g_CourierWakes, g_CourierInjected, g_CourierTries, g_CourierGiveUp;
extern INT g_CpuSpeedIndex;
extern volatile LONG g_CpuSpeedDuty;
extern volatile LONG g_CpuSpeedDutyRm;
extern DWORD g_CpuSpeedRunMs, g_CpuSpeedHeldMs;
extern DWORD g_CpuSpeedMissed;
extern DWORD g_CpuSpeedHoldMaximumMicroseconds;
extern DWORD g_CpuSpeedPeriods;
extern DWORD g_StartMs;
extern DWORD g_CpuSpeedCooperativeCatches, g_CpuSpeedCooperativeTimeouts;
extern UINT32 g_TypematicPeriodMicroseconds;
extern INT g_MouseSensitivity;
extern DWORD g_SimIntUnhandled, g_SimIntVector[IVT_VECTORS];
extern volatile LONG g_MouseEventMask, g_MouseEventSegment, g_MouseEventOffset;
extern INT g_MouseCallbackActive;
extern DWORD g_MouseCallbackInjected, g_MouseCallbackDone, g_MouseCallbackLost, g_MouseCallbackPm, g_MouseCallbackStray;
extern DWORD g_MouseCallbackWhy[MOUSE_CB_WHY_COUNT];
extern DWORD g_MouseEventRaised;
extern volatile LONG g_Captured;
extern volatile LONG g_MouseWantCapture;
extern volatile LONG g_MouseSeamless;
extern NE_MODULE g_WowModule[WOW_MAX_MOD];
extern BYTE *g_WowImage[WOW_MAX_MOD];
extern INT g_WowModuleCount;
extern WORD g_PmTransferSegment;
extern WORD g_WowPspSelector[WOW_PSP_TRACK];
extern INT g_WowPspCount;
extern INT g_WowLaunch;
extern NTVDMEX_SETTINGS g_Settings;
extern DOS_MACHINE *g_DosMachine;
extern DOS_SB_CONFIG g_SbConfig;
extern INT g_XmsOn, g_EmsOn;
extern DWORD g_GuestThreadId;
extern INT g_WowSchedOn;
extern WOWSCHED_SLOT g_WowSchedSlots[WOWSCHED_MAX];
extern INT g_WowWindowNested;
extern BYTE *g_WowShadow;
extern DWORD g_PmIrqRmReflects, g_PmIrqRmFail;

#endif
