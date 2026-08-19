/*
 * vdd_dma.h -- the ISA DMA controller VDD: a pair of Intel 8237As.  (sound epic)
 *
 * Sound Blaster playback is DMA, not port writes: the DSP is told "play N bytes"
 * and the 8237 feeds it from guest memory autonomously. So the SB VDD cannot be
 * built until a DMA controller exists, and this device lands first.
 *
 * Two cascaded controllers, as on every PC/AT:
 *   - controller 1, channels 0-3, 8-bit transfers, ports 0x00-0x0F
 *   - controller 2, channels 4-7, 16-bit transfers, ports 0xC0-0xDF (2x spacing)
 * plus the page registers at 0x80-0x8F, which supply the high address bits the
 * 8237's own 16-bit address register cannot reach.
 *
 * Address arithmetic differs per controller and is the classic place to get this
 * wrong: an 8-bit channel addresses BYTES as (page << 16) | addr, while a 16-bit
 * channel addresses WORDS as ((page & 0xFE) << 16) | (addr << 1), and its count
 * is in words too. Both count "transfers - 1", so a 100-byte block programs 99.
 *
 * A sound device does not read guest memory itself; it pulls through this VDD via
 * vdd_dma_read(), which walks the current address, honours the decrement and
 * auto-init mode bits, and raises terminal count -- so auto-init ring buffers
 * (how every DOS game streams continuous audio) work without the caller knowing.
 *
 * Pure C, no <windows.h>: the only outside effect is vdd_map_lin(), so the whole
 * controller is exercised off-VM by tools/dostest/dma_test.c.
 */
#ifndef NTVDMEX_VDD_DMA_H
#define NTVDMEX_VDD_DMA_H

#include "vdd_bus.h"

/* mode register (0x0B / 0xD6) bit fields */
#define DMA_MODE_CHAN      0x03    /* which channel this mode byte programs      */
#define DMA_MODE_XFER      0x0C    /* 00 verify, 01 write(dev->mem), 10 read(mem->dev) */
#define DMA_MODE_XFER_VERIFY 0x00
#define DMA_MODE_XFER_WRITE  0x04
#define DMA_MODE_XFER_READ   0x08
#define DMA_MODE_AUTOINIT  0x10    /* reload base addr/count at terminal count   */
#define DMA_MODE_DECREMENT 0x20    /* walk the address downwards                 */
#define DMA_MODE_SELECT    0xC0    /* 00 demand, 01 single, 10 block, 11 cascade */

typedef struct dma_chan {
    uint16_t base_addr, cur_addr;   /* byte offset (ch0-3) or word offset (ch4-7) */
    uint16_t base_count, cur_count; /* transfers-1, as the guest programmed it     */
    uint8_t  page;                  /* high address bits, from ports 0x80-0x8F     */
    uint8_t  mode;                  /* last mode byte written for this channel     */
    uint8_t  masked;                /* 1 = channel disabled (mask register)        */
    uint8_t  tc;                    /* terminal count reached; cleared on status rd */
} dma_chan;

typedef struct dma_state {
    vdd_bus *bus;
    dma_chan ch[8];
    uint8_t  ff[2];                 /* per-controller lo/hi byte-pointer flip-flop */
    uint8_t  cmd[2];                /* per-controller command register             */
} dma_state;

/* Build the device descriptor to hand to vdd_bus_add(). */
int  vdd_dma_init(vdd_bus *b, void *self);
void vdd_dma_reset(void *self);
static inline ntvdd vdd_dma_device(dma_state *st)
{ ntvdd d; d.name = "dma"; d.init = vdd_dma_init; d.reset = vdd_dma_reset;
  d.shutdown = 0; d.self = st; return d; }

/* The physical address the next transfer on `ch` will touch. */
uint32_t vdd_dma_cur_phys(const dma_state *st, uint8_t ch);

/* Bytes still to transfer before terminal count (count+1 units, scaled to bytes). */
uint32_t vdd_dma_remaining(const dma_state *st, uint8_t ch);

/* Pull up to `n` bytes from guest memory into `dst` (memory -> device: playback).
   Stops early at terminal count on a non-auto-init channel (and masks it, as the
   8237 does); an auto-init channel reloads and keeps going, so a ring buffer
   streams forever. Returns bytes actually transferred; 0 if the channel is
   masked. `tc_out` (optional) is set non-zero if terminal count was reached. */
uint32_t vdd_dma_read(dma_state *st, uint8_t ch, uint8_t *dst, uint32_t n, int *tc_out);

/* Push `n` bytes into guest memory (device -> memory: recording). Same rules. */
uint32_t vdd_dma_write(dma_state *st, uint8_t ch, const uint8_t *src, uint32_t n, int *tc_out);

#endif /* NTVDMEX_VDD_DMA_H */
