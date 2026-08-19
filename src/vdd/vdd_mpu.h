/*
 * vdd_mpu.h -- MPU-401 MIDI interface VDD (UART mode).  (sound epic, GH #20/#21)
 *
 * The third way a DOS game makes sound: instead of synthesising FM itself or
 * streaming samples, it sends MIDI events to an external synthesiser. On XP there
 * IS one -- the Microsoft GS Wavetable synth -- so the honest implementation is to
 * forward events to the host rather than synthesise them ourselves, which is both
 * far less work and far better sounding than an FM approximation of General MIDI.
 *
 * Ports 0x330 (data) and 0x331 (status read / command write). Two things about
 * the status register catch everyone out: both flags are ACTIVE LOW (a CLEAR bit
 * means ready), and a game will not proceed until it sees the 0xFE acknowledge
 * after the 0xFF reset and 0x3F "enter UART mode" commands.
 *
 * Intelligent mode (the MPU's own sequencer) is deliberately NOT implemented:
 * essentially every DOS game uses UART mode, and a half-built intelligent mode
 * would be worse than none -- a game that detects it would then rely on it.
 *
 * Pure C, no <windows.h>: assembled MIDI messages go to an injected sink, so the
 * device is exercised off-VM by tools/dostest/mpu_test.c with no host MIDI at all.
 */
#ifndef NTVDMEX_VDD_MPU_H
#define NTVDMEX_VDD_MPU_H

#include "vdd_bus.h"

#define MPU_DEFAULT_BASE 0x330

/* status register bits, both ACTIVE LOW */
#define MPU_ST_DRR  0x40    /* clear => the port can accept a byte from the guest */
#define MPU_ST_DSR  0x80    /* clear => a byte is waiting for the guest to read   */

#define MPU_ACK     0xFE    /* what a command must answer with                    */
#define MPU_INQ_MAX 8

/* A complete MIDI message, ready for the host synth. `msg` is packed as
   status | data1<<8 | data2<<16 (the layout midiOutShortMsg wants). */
typedef void (*mpu_midi_sink)(void *ctx, uint32_t msg);

typedef struct mpu_state {
    vdd_bus *bus;
    uint16_t base;
    uint8_t  uart_mode;             /* 0x3F received: pass bytes straight through */

    uint8_t  inq[MPU_INQ_MAX];      /* bytes waiting for the guest (mostly ACKs)  */
    uint8_t  inq_head, inq_len;

    /* running MIDI message assembly */
    uint8_t  status;                /* current status byte (running status)       */
    uint8_t  data[2];
    uint8_t  ndata, want_data;
    uint8_t  in_sysex;

    mpu_midi_sink sink; void *sink_ctx;
    uint32_t sent;                  /* messages forwarded (tests + diagnostics)   */
} mpu_state;

int  vdd_mpu_init(vdd_bus *b, void *self);
void vdd_mpu_reset(void *self);
static inline ntvdd vdd_mpu_device(mpu_state *st)
{ ntvdd d; d.name = "mpu401"; d.init = vdd_mpu_init; d.reset = vdd_mpu_reset;
  d.shutdown = 0; d.self = st; return d; }

#endif /* NTVDMEX_VDD_MPU_H */
