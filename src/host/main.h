/* main.h -- what main.c offers the host's other files.
 *
 * Declarations only (#335): defined in main.c. */
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
#endif
