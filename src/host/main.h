/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * What main.c offers the host's other files.
 *
 * Declarations only (#335): defined in main.c.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_MAIN_H
#define NTVDMEX_MAIN_H

#include "host_state.h"

extern INT g_GusOn;
extern DWORD g_DmxSamples;
extern DWORD g_DmxBusy[12];
extern DWORD g_DmxMixerOk;
extern DWORD g_DmxOverdue;
extern DWORD g_DmxOverdueMaximum;
extern DWORD g_DmxAnyBusy;

extern UINT32 g_TypematicDelayMicroseconds;
extern DWORD g_TypematicSpiDelay;
extern DWORD g_TypematicSpiSpeed;
extern UINT32 g_PitAsyncAttempts;
extern DWORD g_Irq0WorstGapIo;
extern DWORD g_Irq0IoPrevious;
extern DWORD g_Irq0WorstGapMs;
extern DWORD g_Irq0WorstIp;
extern DWORD g_Irq0WorstCs;
extern DWORD g_Irq0NormalCount;
extern DWORD g_Irq0NormalIo;
extern DWORD g_Irq0NormalMicroseconds;
extern DWORD g_Irq0WorstRaise;
extern DWORD g_Irq0WorstAttempts;
extern DWORD g_Irq0WorstNie;
extern DWORD g_Irq0WorstYield;
extern DWORD g_Irq0WorstPerMicroseconds;
extern DWORD g_HeartbeatDs;
extern DWORD g_EventIoString;
extern DWORD g_MemoryDumpLinear;
extern DWORD g_MemoryDumpLength;
extern DWORD g_HeadlessMs;
extern DWORD g_Irq0ResyncDrop;
extern UINT g_CpuSpeedReferenceMhz;
extern HANDLE g_ExecThread;
extern INT g_ExecPriorityForeground;
extern INT g_MouseCallbackTrace;
extern WORD g_Int15StubOffset;
extern DWORD g_PrintScreenErrors;
extern DWORD g_PrintScreenJobs;
extern BYTE g_PrintScreenStatus;
extern VDM_COMMAND_INFO g_CommandInfo;
extern PCSTR g_StdioHow;
extern PCSTR g_StdioSource;
extern DWORD g_StdioParentProcessId;
extern SYSFONT_REPORT g_SysFontReport;
extern CHAR g_TextFontLive[NTVDMEX_PATH_MAX];
extern PCSTR g_DosVersionWhy;
extern NTVDMEX_SETTINGS g_SettingsDisk;
extern INT g_DspVersionForced;
extern UINT g_ConventionalKbWant;
extern INT g_NoA000;
extern DWORD g_ModeYSelectorCalls;
extern DWORD g_ModeYSelectorSame;
extern DWORD g_ModeYSelectorZero;
extern DWORD g_ModeYTimelineIns[YTL_SECS];
extern UINT64 g_ModeYTimelineInterpreterCycles[YTL_SECS];
extern DWORD g_ModeYFanoutBarWrites[2];
extern DWORD g_ModeYFanoutBarDistinct[2];
extern DWORD g_ModeYFanoutBar4Way[2];
extern DWORD g_ModeYLatchDescriptor;
extern DWORD g_ModeYLatchUnsolved;
extern DWORD g_ModeYLatchOk;
extern INT g_A000Protection;
/* Defined in main.c (#335). */
extern WORD g_DsProbe[DSPROBE_MAX];
extern INT g_DsProbeCount;
extern WORD g_CsProbe[DSPROBE_MAX];
extern INT g_CsProbeCount;
extern INT g_PmIrq0Latch;
extern INT g_NoPmPatch;
extern DWORD g_NoPmPatchMinimum;
extern INT g_LdtClientMark;
extern INT g_PmTopDispatch;
extern DWORD g_NtvdmBopCount;
extern DMA_STATE g_Dma;
extern DWORD g_V86StringHistogram[8];
extern DWORD g_V86StringCount8;
extern DWORD g_V86StringMaximumEvent;
extern DWORD g_V86StringMaximumMs;
extern DWORD g_V86StringMaximumCs;
extern DWORD g_V86StringMaximumIp;
extern DWORD g_IoViaDirect;
extern DWORD g_IoViaRetro;
extern DWORD g_RunStartTick;
extern DWORD g_P12Batches;
extern DWORD g_P12Instructions;
extern DWORD g_P12Bails;
extern P12_SITE g_P12Site[P12_SITE_MAX];
extern UINT g_P12SiteCount;
extern UINT g_P12SiteLost;
extern DWORD g_Kb4FCalls;
extern DWORD g_Kb4FTranslate;
extern DWORD g_ExecPriority;
extern DWORD g_InterpRefused;
extern DWORD g_PmCooperativeGate[PM_GATES];
extern UINT32 g_CooperativeDmaPolls;
extern UINT32 g_CooperativeDmaPollsDevice[PIC_LINES_PER_CHIP];
extern DWORD g_PmDeviceIrqInjected;
extern DWORD g_PmDeviceIrqFail;
extern DWORD g_PmDeviceIrqDrop;
extern DWORD g_PmStretchMaximumMicroseconds;
extern DOS_START_MODE g_StartMode;

extern DWORD g_KeyPmLogged;
extern INT g_Fault32Warned;
extern INT g_DpmiUseKernel;
extern INT g_CloseForced;
extern DWORD g_PmStretchLogged;
extern INT g_DpmiUseInterp;
#endif
