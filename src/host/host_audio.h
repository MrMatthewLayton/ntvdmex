/* host_audio.h -- the host side of the sound devices: OPL tracing and pumping, the audio fill, MIDI sinks and the GUS report.
 *
 * Declarations only (#335): defined in host_audio.c. */
#ifndef NTVDMEX_HOST_AUDIO_H
#define NTVDMEX_HOST_AUDIO_H
#include "host_state.h"

VOID GusReport(VOID);
extern DWORD g_OplTraceCount;
extern DWORD g_OplTraceDrop;
extern INT g_OplTraceOn;
VOID OplTraceWrite(BYTE registerIndex, BYTE value);
VOID OplTraceDump(VOID);
VOID OplPumpTime(VOID);
VOID HostAudioFill(PVOID context, INT16 *out, UINT32 frames);
VOID HostMidiSink(PVOID context, UINT32 message);
VOID HostMidiSysEx(PVOID context, const BYTE *message, UINT32 length);
extern MPU_STATE g_GusMidi;
VOID GusMidiToSynth(PVOID context, BYTE byteValue);

#endif
