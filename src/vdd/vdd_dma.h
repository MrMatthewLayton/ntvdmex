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

/* ── THE COMMAND REGISTER (0x08 / 0xD0), per the Intel 8237A datasheet. ───────────
     Written per controller, cleared by master clear. ONE bit is honoured; the rest
     are STORED and nothing reads them -- see docs/inventory/dma.md for why each is
     safe to leave that way on a PC, and what honouring it would take.
       bit 0  memory-to-memory (ch0 -> ch1 through the temporary register)  stored
       bit 1  channel 0 address hold (only meaningful with bit 0)           stored
       bit 2  CONTROLLER DISABLE -- no DACK is given on ANY of its four     HONOURED
              channels, so no byte moves and no TC is reached; DREQs stay
              pending (status bits 7:4) until the guest re-enables it
       bit 3  compressed timing (a bus-cycle length: nothing to model)      stored
       bit 4  rotating priority (we serve one channel per call: no arbiter) stored
       bit 5  extended write (a strobe width: nothing to model)             stored
       bit 6  DREQ sense active-LOW                                         stored
       bit 7  DACK sense active-HIGH                                        stored
   ⚠ Bits 6 and 7 are the board's WIRING contract, not a mode: a PC's cards drive
     DREQ active-high, so a guest that flips bit 6 has told the chip every idle line
     is a request. Nothing in the period software we know does it, and modelling it
     would mean inventing transfers no device asked for. */
#define DMA_CMD_MEM2MEM    0x01
#define DMA_CMD_ADDRHOLD   0x02
#define DMA_CMD_DISABLE    0x04
#define DMA_CMD_COMPRESSED 0x08
#define DMA_CMD_ROTATE     0x10
#define DMA_CMD_EXTWRITE   0x20
#define DMA_CMD_DREQ_LOW   0x40
#define DMA_CMD_DACK_HIGH  0x80

/* status register (read 0x08 / 0xD0) */
#define DMA_STATUS_TC      0x0F    /* terminal count, ch 0-3 of this controller; clear on read */
#define DMA_STATUS_DRQ     0xF0    /* request pending, ch 0-3 of this controller; DERIVED      */

/* ── WHO IS ASSERTING DREQ? THE DEVICE KNOWS; THE 8237 ONLY SEES THE PIN. ─────────
     Status bits 7:4 report each channel's DREQ input. On the bus that is a wire the
     card drives; here the card's own state already says whether it wants the bus (an
     SB with a transfer armed, a GUS whose 41h "go" is waiting on the 8237), so the
     bits are DERIVED at the moment of the status read rather than latched -- a second
     copy of the same fact would only be a second thing to get out of step.
     A device that can request DMA registers one function at init; it returns a mask
     of the channels 0-7 whose DREQ it is asserting right now.
   ★ Independent of the MASK and of the CONTROLLER-DISABLE bit, by design: those decide
     whether the 8237 ANSWERS a request, not whether one is being made. A request that
     the controller is refusing is exactly the one a status read must show.
   ⚠ It is the wiring of the machine, so it SURVIVES vdd_dma_reset -- a guest's master
     clear does not unplug the sound card. */
typedef uint8_t (*dma_dreq_fn)(const void *ctx);
#define DMA_DREQ_MAX 4

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
    /* ── THE NINE PAGE PORTS THAT MAP TO NO CHANNEL ARE STILL REAL LATCHES. ──────
         Seven of the sixteen ports at 80h-8Fh carry a DMA channel's high address
         bits. The other nine -- 80h, 84h-86h, 88h, 8Ch-8Fh -- are read/write
         storage on a PC anyway, because the address decoder does not bother to
         leave them out, and 80h doubles as the POST diagnostic port.
       ★ MEASURED (p_dma.asm dma.page.spare80, 2026-09-23): dosbox-x AND PCem, on
         a real AMI BIOS, both read back a written 0x5A. Only 6.22-under-QEMU
         answers 0xFF. ⚠ That is NOT "QEMU is the outlier" as a general rule --
         counted over a session it is not even true; see
         docs/research/oracle-disagreements.md.
       ⚠ We answered 0xFF, which describes an EMPTY BUS rather than a machine.
         Indexed by the low nibble of the port; the mapped ports never reach it. */
    uint8_t  page_spare[16];
    uint8_t  cmd[2];                /* per-controller command register; DMA_CMD_*  */
    dma_dreq_fn dreq_fn[DMA_DREQ_MAX];   /* who drives DREQ -- host wiring, see above */
    const void *dreq_ctx[DMA_DREQ_MAX];
    uint8_t  dreq_n;
    /* ── DOES THE GUEST ASK US WHERE THE PLAY HEAD IS? ───────────────────────────
         A double-buffering sound driver has two ways to decide which half of the
         DMA ring is safe to write: count the block-completion IRQs, or READ THE
         8237's CURRENT ADDRESS. If DMX does the latter then the fidelity of
         cur_addr -- which advances on the AUDIO thread here, in whatever chunks
         waveOut happens to ask for -- is load-bearing for every refill decision,
         and a ring replay would be the guest writing where we told it to write.
         If it never reads them, that whole family of causes is dead and the
         refill must be driven by the IRQ count alone. Nothing distinguishes the
         two today: SNDIO traces only the card's own ports, and the hot-port
         histogram is empty for a protected-mode client. Three counters settle it. */
    uint32_t rd_addr[8], rd_count[8];  /* guest reads of cur_addr / cur_count      */
    /* ── A COUNT READ IS THE GUEST'S MIXER SAYING 'I AM RUNNING NOW'. ────────────
         DMX's refill routine polls the 8237's current count as its FIRST action
         (DOOM.EXE 0x56884), before it decides which block to fill. So a read of the
         count register is the one externally visible moment at which the guest is
         known to be mid-refill -- which is exactly the phase signal the ACK cannot
         give, because DMX acknowledges the interrupt BEFORE it refills (0x53024's
         status read runs ahead of the `call [0x584]` that does the work). */
    uint32_t count_reads;              /* monotonic: any guest read of a count reg  */
    uint32_t rd_status[2];             /* ...and of the status register (TC bits)  */
    /* ⚠ A COUNT OF PORT READS IS NOT A COUNT OF POLLS. The 8237's count register is
         16 bits behind an 8-bit port with a lo/hi flip-flop, so one poll is TWO
         reads -- unless the guest issues a 16-bit IN, which `dma_in` currently
         serves by ignoring the width and returning a single half. Dividing reads by
         two to get a poll rate is an assumption about which of those is happening,
         and the whole "DMX looks less often than blocks complete" reading rests on
         it. Count the widths and let the run say. */
    uint32_t rd_w1, rd_w2, rd_w4;      /* count-register reads by operand width    */
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

/* ── WOULD THE 8237 ANSWER A DREQ ON `ch` RIGHT NOW? ──────────────────────────────
     The ONE question every DMA-moving path asks, so the conditions live in one place:
     the channel's mask bit is clear AND its controller is not disabled (command bit 2).
     vdd_dma_read/_write ask it themselves; a device that must decide whether to HOLD
     its own state machine (an SB that must not end its block, a GUS whose upload must
     wait) asks it before it pulls, rather than reading `masked` or `cmd[]` itself.
   ⚠ Not modelled: the AT cascade. Controller 1 reaches the bus through channel 4, so
     on a real AT masking channel 4 or disabling controller 2 also starves channels
     0-3. We do not run the BIOS that unmasks channel 4 (master clear leaves it set),
     so honouring it would silence every 8-bit transfer. See docs/inventory/dma.md. */
int vdd_dma_grants(const dma_state *st, uint8_t ch);

/* The DREQ lines, channels 0-7 as bits 0-7, from every registered device. Channel 4
   is controller 1's HRQ (the cascade): asserted while controller 1 has a request it
   would serve. */
uint8_t vdd_dma_dreq(const dma_state *st);

/* A device that can request DMA calls this once, from its init. Returns -1 if the
   table is full; registering the same (fn, ctx) twice is harmless. */
int vdd_dma_add_dreq(dma_state *st, dma_dreq_fn fn, const void *ctx);

/* Pull up to `n` bytes from guest memory into `dst` (memory -> device: playback).
   Stops early at terminal count on a non-auto-init channel (and masks it, as the
   8237 does); an auto-init channel reloads and keeps going, so a ring buffer
   streams forever. Returns bytes actually transferred; 0 if the 8237 would not
   serve the channel (vdd_dma_grants: masked, or its controller disabled) -- no
   byte moves, the address and count stand still, and no TC is raised.
   `tc_out` (optional) is set non-zero if terminal count was reached. */
uint32_t vdd_dma_read(dma_state *st, uint8_t ch, uint8_t *dst, uint32_t n, int *tc_out);

/* Push `n` bytes into guest memory (device -> memory: recording). Same rules. */
uint32_t vdd_dma_write(dma_state *st, uint8_t ch, const uint8_t *src, uint32_t n, int *tc_out);

#endif /* NTVDMEX_VDD_DMA_H */
