/* host_dpmi_int.h -- what host_dpmi_int.c offers the host's other files.
 *
 * Declarations only (#335): defined in host_dpmi_int.c. */
#ifndef NTVDMEX_HOST_DPMI_INT_H
#define NTVDMEX_HOST_DPMI_INT_H
#include "host_state.h"

extern WORD g_WowLastId;
extern WORD g_WowLastFrom;

/* Defined in host_dpmi_int.c (#335). */
extern DWORD g_WowPspLinear[WOW_PSP_TRACK];
extern WORD g_WowPspEnvironment[WOW_PSP_TRACK];
extern DWORD g_WowSyncWrites;
extern DWORD g_WowPmBase[WOW_PMBASE_MAX];
extern BYTE g_BreakpointDone[DPMI_BP_MAX];
extern WORD g_DpmiDosBlock[DPMI_DOSBLK_MAX];
extern INT g_DpmiDosBlockCount;
extern DWORD g_LeCodeSize[DPMI_LE_MAX];
extern WORD g_PmAppTimerSelector;
extern DWORD g_PmAppTimerOffset;
extern INT g_PmDispatchTop;
INT DpmiServicePmIntBody(DOS_MACHINE *machine, volatile BYTE *tib, DWORD vector, UINT steps);
#endif
