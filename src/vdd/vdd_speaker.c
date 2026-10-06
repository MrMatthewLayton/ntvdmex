/* vdd_speaker.c -- see vdd_speaker.h.  PC-speaker control port 0x61 on the VDD
 * bus; the tone frequency comes from PIT channel 2.  No Windows calls, only Windows types. */
#include "vdd_speaker.h"

/* Port 0x61 (PPI port B): bit 0 = timer-2 gate, bit 1 = speaker data, bit 4 =
   DRAM-refresh toggle (programs poll it to time short delays). We store the
   written control bits and toggle bit 4 on each read so those delay loops run. */
/* ── BIT 0 IS COUNTER 2'S GATE, AND IT IS AN OUTPUT OF THIS PORT. ────────────
     Storing the byte and going home leaves counter 2 running whatever software
     asked for, so the gate-and-poll idiom -- program a count, drop the gate,
     raise it, time the result -- measures nothing. Push it through. */
static VOID VddSpeakerPortOut(PVOID context, WORD port, BYTE width, UINT32 value)
{ PSPEAKER_STATE state = (PSPEAKER_STATE)context; (VOID)port; (VOID)width;
  state->Port61 = (BYTE)value;
  if (state->Pit) VddPitCounter2Gate(state->Pit, state->Port61 & SPEAKER_GATE_BIT); }

/* ── BIT 5 IS COUNTER 2'S OUT PIN, NOT A BIT THE GUEST WROTE. ────────────────
     This used to hand back whatever bit 5 had been written, so the classic
     "measure elapsed time without interrupts" loop -- poll 61h bit 5 and count
     the iterations -- saw a constant and either fell straight through or span
     forever. All three oracles agree it must move (p_pit pit.61h.bit5.toggles).
   ⚠ Bit 4 is still SYNTHESISED, deliberately: it is the DRAM-refresh toggle and
     we flip it on every read so refresh-poll delay loops terminate. A guest that
     CALIBRATES against it gets a number with no relation to time -- a known,
     recorded approximation, not an oversight. */
#define SPEAKER_REFRESH_BIT     0x10   /* bit 4: the DRAM-refresh toggle               */
#define SPEAKER_OUT_BIT         0x20   /* bit 5: counter 2's OUT pin                    */
#define SPEAKER_READ_BACK_MASK  0x30   /* the bits a read computes rather than echoes   */
#define SPEAKER_OUT_LOW         0

static VOID VddSpeakerPortIn(PVOID context, WORD port, BYTE width, UINT32 *value)
{ PSPEAKER_STATE state = (PSPEAKER_STATE)context; (VOID)port; (VOID)width;
  state->RefreshToggle ^= SPEAKER_REFRESH_BIT;
  *value = (BYTE)((state->Port61 & ~SPEAKER_READ_BACK_MASK) | state->RefreshToggle
                 | ((state->Pit && VddPitCounter2Out(state->Pit)) ? SPEAKER_OUT_BIT : SPEAKER_OUT_LOW)); }

VOID VddSpeakerReset(PVOID context)
{ PSPEAKER_STATE state = (PSPEAKER_STATE)context; state->Port61 = 0; state->RefreshToggle = 0; }  /* keep bus + pit */

INT VddSpeakerInitialize(PVDD_BUS bus, PVOID context)
{ PSPEAKER_STATE state = (PSPEAKER_STATE)context; state->Bus = bus;
  return VddClaimPorts(bus, SPEAKER_PORT, SPEAKER_PORT, VddSpeakerPortIn, VddSpeakerPortOut, state); }
