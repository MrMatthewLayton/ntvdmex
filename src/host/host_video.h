/* host_video.h -- video: Mode Y, the A000 trap, the instruction interpreters' host callbacks
 *   (v86interp.h, pm32interp.h) and the profiler.
 *
 * Declarations only (#335): defined in host_video.c. */
#ifndef NTVDMEX_HOST_VIDEO_H
#define NTVDMEX_HOST_VIDEO_H
#include "host_state.h"

VOID ModeYRingNoteIrq(UINT vector, WORD cs, WORD ip, WORD ss, WORD sp);

VOID ModeYTimelineReport(VOID);
VOID VideoTrapSync(VOID);
extern INT g_P12Offset;
extern INT g_ModeYInterpOffset;
extern INT g_ModeYInterp;
extern DWORD g_ModeYSlices;
extern DWORD g_ModeYBails;
extern DWORD g_ModeYBailMp;
extern DWORD g_ModeYInstructions;
extern INT g_ModeYRingOn;
extern INT g_ModeYPmOffset;
extern INT g_ModeYPmDetect;
VOID PlanesDumpBeside(PCSTR bitmapPath);
extern PVOID g_ModeYView[MODEY_NSEC];
extern INT g_ModeYRemap;
extern DWORD g_ModeYFanouts;
extern DWORD g_ModeYFail;
extern DWORD g_ModeYSwaps;
extern DWORD g_ModeYTimelineT0;
UINT64 ModeYTimelineRdtsc(VOID);
extern DWORD g_ModeYSampleWrites[2];
extern DWORD g_ModeYSampleCrossSame[2];
extern DWORD g_ModeYSampleCrossDiff[2];
extern DWORD g_ModeYSampleCrossEqualBytes[2];
extern DWORD g_ModeYSampleCrossTotalBytes[2];
extern DWORD g_ModeYSampleP1Equal[2][4];
extern DWORD g_ModeYSampleP1Total[2][4];
extern DWORD g_ModeYSampleDeliveredEqual[2];
extern DWORD g_ModeYSampleDeliveredTotal[2];
VOID ModeYRemapFlushReport(VOID);
INT ModeYRemapInitialize(VOID);
VOID ModeYRemapSelect(PVOID context, INT mask);
VOID ModeYBailNote(DWORD cs, DWORD ip, const volatile BYTE *bytes);
BYTE *ModeYRemapPlane(PVOID context, INT plane);
extern DWORD g_ModeYGr4Pair[4][6];
extern DWORD g_ModeYGr4Mismatch;
extern DWORD g_ModeYGr4Calls;
extern DWORD g_ModeYGr4RunPlanes[VIDEO_PLANES];
extern DWORD g_ModeYGr4Runs[10];
extern DWORD g_ModeYGr4SinceSelector;
extern DWORD g_ModeYGr4Moves;
VOID ModeYRemapReadMap(PVOID context, INT plane);
VOID ModeYRemapWriteMode(PVOID context, INT writeMode);
extern INT g_P12Interp;
INT ModeYNeedsInterp(VOID);
extern DWORD g_InterpreterMemoryBadWrites;
extern DWORD g_InterpreterMemoryBadLogged;
extern DWORD g_InterpreterMemoryBadReads;
INT InterpreterMemoryPageOk(UINT32 linear);
INT ModeYPmNeedsInterp(VOID);
VOID ModeYPmRun(volatile BYTE *tib);
extern const V86_CPU *g_InterpreterCpu;
UINT32 HostGuestPc(VOID);
VOID ModeYRingDump(PCSTR why);
VOID HostProfileStart(VOID);
INT32 HostInterpPaced(volatile BYTE *tib, INT32 cap);
INT DpmiRunPmInterp(DOS_MACHINE *machine, volatile BYTE *tib);
#endif
