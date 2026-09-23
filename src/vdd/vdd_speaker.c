/* vdd_speaker.c -- see vdd_speaker.h.  PC-speaker control port 0x61 on the VDD
 * bus; the tone frequency comes from PIT channel 2.  Pure C, no <windows.h>. */
#include "vdd_speaker.h"

/* Port 0x61 (PPI port B): bit 0 = timer-2 gate, bit 1 = speaker data, bit 4 =
   DRAM-refresh toggle (programs poll it to time short delays). We store the
   written control bits and toggle bit 4 on each read so those delay loops run. */
/* ── BIT 0 IS COUNTER 2'S GATE, AND IT IS AN OUTPUT OF THIS PORT. ────────────
     Storing the byte and going home leaves counter 2 running whatever software
     asked for, so the gate-and-poll idiom -- program a count, drop the gate,
     raise it, time the result -- measures nothing. Push it through. */
static void spk_out(void *self, uint16_t port, uint8_t w, uint32_t v)
{ speaker_state *st = (speaker_state *)self; (void)port; (void)w;
  st->port61 = (uint8_t)v;
  if (st->pit) vdd_pit_ch2_gate(st->pit, st->port61 & 0x01); }

/* ── BIT 5 IS COUNTER 2'S OUT PIN, NOT A BIT THE GUEST WROTE. ────────────────
     This used to hand back whatever bit 5 had been written, so the classic
     "measure elapsed time without interrupts" loop -- poll 61h bit 5 and count
     the iterations -- saw a constant and either fell straight through or span
     forever. All three oracles agree it must move (p_pit pit.61h.bit5.toggles).
   ⚠ Bit 4 is still SYNTHESISED, deliberately: it is the DRAM-refresh toggle and
     we flip it on every read so refresh-poll delay loops terminate. A guest that
     CALIBRATES against it gets a number with no relation to time -- a known,
     recorded approximation, not an oversight. */
static void spk_in(void *self, uint16_t port, uint8_t w, uint32_t *v)
{ speaker_state *st = (speaker_state *)self; (void)port; (void)w;
  st->refresh ^= 0x10;
  *v = (uint8_t)((st->port61 & ~0x30) | st->refresh
                 | ((st->pit && vdd_pit_ch2_out(st->pit)) ? 0x20 : 0)); }

void vdd_speaker_reset(void *self)
{ speaker_state *st = (speaker_state *)self; st->port61 = 0; st->refresh = 0; }  /* keep bus + pit */

int vdd_speaker_init(vdd_bus *b, void *self)
{ speaker_state *st = (speaker_state *)self; st->bus = b;
  return vdd_claim_ports(b, 0x61, 0x61, spk_in, spk_out, st); }
