/*
 * vdd_speaker.h -- the PC-speaker VDD.  (M3, ADR-0008)
 *
 * The third device on the bus (after the PIT timer and the video VDD), included
 * mainly to prove the VDD ABI generalises to a new device class. The PC speaker
 * is PIT channel 2 (the tone frequency) gated by I/O port 0x61 bits 0 (timer-2
 * gate) and 1 (speaker-data enable). This VDD claims port 0x61, tracks the gate
 * state, and reports the active tone (frequency from PIT channel 2). It does NOT
 * synthesise audio -- producing actual sound is M7 (peripheral VDDs); this is the
 * device-model stub. Pure C, no <windows.h>; exercised off-VM by speaker_test.c.
 */
#ifndef NTVDMEX_VDD_SPEAKER_H
#define NTVDMEX_VDD_SPEAKER_H

#include "vdd_bus.h"
#include "vdd_pit.h"

typedef struct speaker_state {
    vdd_bus         *bus;
    const pit_state *pit;       /* channel 2 supplies the tone frequency        */
    uint8_t          port61;    /* last value written to port 0x61              */
    uint8_t          refresh;   /* toggling bit 4 so refresh-poll delay loops run */
} speaker_state;

/* The speaker emits a tone iff the timer-2 gate (bit 0) AND speaker-data (bit 1)
   are both set; the tone is PIT channel 2's output frequency. */
static inline int      vdd_speaker_active(const speaker_state *st) { return (st->port61 & 0x03) == 0x03; }
static inline uint32_t vdd_speaker_hz(const speaker_state *st)
{ return st->pit ? pit_ch2_hz(st->pit) : 0; }

int  vdd_speaker_init(vdd_bus *b, void *self);
void vdd_speaker_reset(void *self);
static inline ntvdd vdd_speaker_device(speaker_state *st)
{ ntvdd d; d.name = "speaker"; d.init = vdd_speaker_init; d.reset = vdd_speaker_reset;
  d.shutdown = 0; d.self = st; return d; }

#endif /* NTVDMEX_VDD_SPEAKER_H */
