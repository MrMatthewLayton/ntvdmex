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

#endif
