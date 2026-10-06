/*
 * vdd_speaker.h -- the PC-speaker VDD.  (M3, ADR-0008)
 *
 * The third device on the bus (after the PIT timer and the video VDD), included
 * mainly to prove the VDD ABI generalises to a new device class. The PC speaker
 * is PIT channel 2 (the tone frequency) gated by I/O port 0x61 bits 0 (timer-2
 * gate) and 1 (speaker-data enable). This VDD claims port 0x61, tracks the gate
 * state, and reports the active tone (frequency from PIT channel 2). It does NOT
 * synthesise audio -- producing actual sound is M7 (peripheral VDDs); this is the
 * device-model stub. No Windows calls, only Windows types; exercised off-VM by speaker_test.c.
 */
#ifndef NTVDMEX_VDD_SPEAKER_H
#define NTVDMEX_VDD_SPEAKER_H

#include "vdd_bus.h"
#include "vdd_pit.h"

#define SPEAKER_PORT            0x61   /* PPI port B                                    */
#define SPEAKER_GATE_BIT        0x01   /* bit 0: counter 2's gate                       */
#define SPEAKER_TONE_BITS       0x03   /* bits 0-1: gate AND speaker data = a tone      */
#define SPEAKER_DEVICE_NAME     "speaker"

typedef struct _SPEAKER_STATE {
    PVDD_BUS        Bus;
    pit_state      *Pit;         /* channel 2: the tone, and the GATE/OUT pair    */
    BYTE            Port61;      /* last value written to port 0x61              */
    BYTE            RefreshToggle; /* toggling bit 4 so refresh-poll delay loops run */
} SPEAKER_STATE, *PSPEAKER_STATE;

typedef const SPEAKER_STATE *PCSPEAKER_STATE;

#define SPEAKER_NO_TONE         0

/* The speaker emits a tone iff the timer-2 gate (bit 0) AND speaker-data (bit 1)
   are both set; the tone is PIT channel 2's output frequency. */
static inline INT    VddSpeakerIsActive(_In_ PCSPEAKER_STATE state) { return (state->Port61 & SPEAKER_TONE_BITS) == SPEAKER_TONE_BITS; }
static inline UINT32 VddSpeakerHz(_In_ PCSPEAKER_STATE state)
{ return state->Pit ? pit_ch2_hz(state->Pit) : SPEAKER_NO_TONE; }

INT  VddSpeakerInitialize(_In_ PVDD_BUS bus, _In_ PVOID context);
VOID VddSpeakerReset(_In_ PVOID context);
static inline NTVDD_DEVICE VddSpeakerDevice(_In_ PSPEAKER_STATE state)
{ NTVDD_DEVICE device; device.Name = SPEAKER_DEVICE_NAME; device.Initialize = VddSpeakerInitialize; device.Reset = VddSpeakerReset;
  device.Shutdown = 0; device.Context = state; return device; }

#endif /* NTVDMEX_VDD_SPEAKER_H */
