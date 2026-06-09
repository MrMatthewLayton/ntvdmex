/* vdd_speaker.c -- see vdd_speaker.h.  PC-speaker control port 0x61 on the VDD
 * bus; the tone frequency comes from PIT channel 2.  Pure C, no <windows.h>. */
#include "vdd_speaker.h"

/* Port 0x61 (PPI port B): bit 0 = timer-2 gate, bit 1 = speaker data, bit 4 =
   DRAM-refresh toggle (programs poll it to time short delays). We store the
   written control bits and toggle bit 4 on each read so those delay loops run. */
static void spk_out(void *self, uint16_t port, uint8_t w, uint32_t v)
{ speaker_state *st = (speaker_state *)self; (void)port; (void)w; st->port61 = (uint8_t)v; }

static void spk_in(void *self, uint16_t port, uint8_t w, uint32_t *v)
{ speaker_state *st = (speaker_state *)self; (void)port; (void)w;
  st->refresh ^= 0x10;
  *v = (uint8_t)((st->port61 & ~0x10) | st->refresh); }

void vdd_speaker_reset(void *self)
{ speaker_state *st = (speaker_state *)self; st->port61 = 0; st->refresh = 0; }  /* keep bus + pit */

int vdd_speaker_init(vdd_bus *b, void *self)
{ speaker_state *st = (speaker_state *)self; st->bus = b;
  return vdd_claim_ports(b, 0x61, 0x61, spk_in, spk_out, st); }
