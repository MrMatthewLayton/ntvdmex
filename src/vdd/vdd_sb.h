/*
 * vdd_sb.h -- the Sound Blaster 16 VDD: DSP, mixer, and DMA playback.  (GH #20)
 *
 * This is the device DOS games use for SAMPLED audio -- speech, sound effects,
 * streaming music. It does not push samples through ports: the game programs the
 * 8237 (vdd_dma.c) with a buffer, tells the DSP "play N bytes at rate R", and the
 * card fetches the data itself, raising an IRQ when the block completes. Games
 * then either hand over the next block or, far more commonly, use auto-init mode
 * and treat the buffer as a ring they refill from the IRQ handler.
 *
 * The detection handshake is what everything hinges on, and it is unforgiving:
 *   write 1 to 2x6 (reset), wait >=3us, write 0, then read 2xA -- which MUST
 *   return 0xAA, with 2xE bit 7 set to say a byte is waiting.
 * A game that does not see 0xAA concludes there is no card; a game that sees a
 * half-implemented card can wait forever for an IRQ that never arrives. That is
 * exactly where Skyroads sits today: it has its .SND file open and is spinning
 * with the BIOS tick running, waiting on sound hardware that is not there yet.
 *
 * Base address is configurable because games probe a set of them (0x220 is the
 * near-universal default, 0x240 the usual alternative); IRQ 5 and DMA 1 are the
 * defaults every DOS game's autodetect expects.
 *
 * Pure C, no <windows.h>: the card pulls audio through the DMA VDD and raises
 * IRQs through the bus, so the whole thing is exercised off-VM by
 * tools/dostest/sb_test.c with no host audio anywhere near it.
 */
#ifndef NTVDMEX_VDD_SB_H
#define NTVDMEX_VDD_SB_H

#include "vdd_bus.h"
#include "vdd_dma.h"
#include "vdd_opl.h"

#define SB_DEFAULT_BASE  0x220
#define SB_DEFAULT_IRQ   5
#define SB_DEFAULT_DMA8  1
#define SB_DEFAULT_DMA16 5

/* DSP version we report to command 0xE1. 4.05 = a Sound Blaster 16, which is what
   a game needs to see before it will use the 16-bit and auto-init commands. */
#define SB_DSP_VER_MAJOR 4
#define SB_DSP_VER_MINOR 5

#define SB_OUTQ_MAX 8          /* bytes the DSP can have waiting to be read      */
#define SB_ARG_MAX  4          /* longest command argument list we accept        */

/* Playback state of the DSP's transfer engine. */
enum {
    SB_XFER_IDLE = 0,
    SB_XFER_SINGLE,            /* one block, then IRQ and stop                   */
    SB_XFER_AUTO               /* ring: IRQ per block, keep going                */
};

typedef struct sb_state {
    vdd_bus   *bus;
    dma_state *dma;            /* where playback data comes from                 */
    opl_state *opl;            /* FM mirrored at 2x0-2x3 and 2x8-2x9             */

    uint16_t base;             /* 0x220 by default                               */
    uint8_t  irq, dma8, dma16;

    /* DSP command state machine */
    uint8_t  cmd;              /* command awaiting arguments (0 = none)          */
    uint8_t  args[SB_ARG_MAX];
    uint8_t  nargs, want_args;
    uint8_t  outq[SB_OUTQ_MAX];
    uint8_t  outq_head, outq_len;
    uint8_t  reset_state;      /* 1 = reset asserted, waiting for the 0 write     */
    uint8_t  speaker;          /* DSP speaker on/off (does not gate DMA)          */

    /* transfer engine */
    uint8_t  xfer_mode;        /* SB_XFER_*                                       */
    uint8_t  xfer_16bit;       /* 16-bit samples (DMA channel dma16)              */
    uint8_t  xfer_stereo;
    uint8_t  xfer_signed;
    uint8_t  paused;
    uint32_t block_len;        /* bytes per block, from the length the game set   */
    uint32_t block_left;       /* bytes still to fetch in this block              */
    uint32_t rate_hz;          /* sample rate, from time constant or 0x41         */
    uint8_t  irq_pending;      /* block done: IRQ raised, awaiting ack            */

    uint32_t dsp_writes;       /* DSP command bytes accepted (diagnostics)       */
    uint32_t blocks;           /* blocks completed -> IRQs raised                */

    /* mixer */
    uint8_t  mix_index;
    uint8_t  mix[256];
    /* ── RAW CAPTURE OF WHAT WE ACTUALLY PLAY. ───────────────────────────────────
         The DMA ring is the only place the guest's PCM exists, and vdd_sb_render() is
         the only thing that reads it -- so a byte-for-byte record of what came out is
         the audio equivalent of a screenshot. Doom's sound effects are DS* lumps in the
         IWAD, 8-bit unsigned at 11025 Hz, which makes them an exact oracle: correlate
         the capture against the lump and any deviation is ours, located in time.
         Host-owned buffer, filled without I/O so the audio thread never blocks; the
         host writes it out at wind-down. */
    uint8_t *cap_buf;
    uint32_t cap_len, cap_cap;

    /* ── THE BLOCK-BOUNDARY LEDGER. ──────────────────────────────────────────────
         The click is not a rate fault (41.5 s of audio from a 45 s run) nor a framing
         fault (corr(L,R) = 0.978). It is a DISCONTINUITY 12.8x over-represented at
         offset 2 of every 128-frame block -- a defect at the boundary, 86 times a
         second, which the ear hears as a buzz rather than as clicks.
         Three things happen at that boundary and only one of them can be two frames
         out: the completion IRQ we raise, the 8237's auto-init reload of
         cur_addr/cur_count, and the guest's refill arriving afterwards. Nothing
         recorded so far distinguishes them.
       ► cap_off IS THE FIELD THAT MATTERS. sbref.py currently INFERS the 128-frame
         period from the capture and then measures against its own inference. Writing
         down where each block actually ended, as an offset into the very buffer being
         analysed, replaces that inference with a fact -- so "the jump is two frames
         into the block" stops being a property of a guessed grid.
         Bounded and allocation-free: the first few blocks settle it, and the audio
         thread must never do I/O, so the host prints this at wind-down. */
#define SB_BLKLOG_MAX 24
    struct sb_blkrec {
        uint32_t cap_off;      /* bytes captured when the block completed          */
        uint32_t block_len;    /* what the DSP was told a block is                 */
        uint32_t phys;         /* 8237 current physical address, post-fetch        */
        uint16_t cur_count;    /* 8237 count remaining (reloaded already on TC)    */
        uint16_t base_addr, base_count;
        uint8_t  page, mode, ended, reloaded;
    } blklog[SB_BLKLOG_MAX];
    uint32_t blklog_n;         /* blocks seen; entries kept = min(n, MAX)          */
} sb_state;

/* Build the device descriptor to hand to vdd_bus_add(). Set base/irq/dma and the
   dma/opl back-pointers before adding. */
int  vdd_sb_init(vdd_bus *b, void *self);
void vdd_sb_reset(void *self);
static inline ntvdd vdd_sb_device(sb_state *st)
{ ntvdd d; d.name = "sb16"; d.init = vdd_sb_init; d.reset = vdd_sb_reset;
  d.shutdown = 0; d.self = st; return d; }

/* Pull up to `frames` samples of playback into `out` (mono 16-bit at the card's
   current rate), fetching through the DMA controller and raising the completion
   IRQ when a block ends. Returns frames produced; silence (zeros) when idle, so
   the mixer can always call it. */
uint32_t vdd_sb_render(sb_state *st, int16_t *out, uint32_t frames);

/* True while a programmed transfer is running (test/mixer convenience). */
static inline int vdd_sb_active(const sb_state *st)
{ return st->xfer_mode != SB_XFER_IDLE && !st->paused; }

/* nosb.flag: when set, the DSP reset handshake withholds its 0xAA so a detect
   fails, i.e. the machine reports no Sound Blaster fitted. See vdd_sb.c. */
extern int g_sb_absent;

#endif /* NTVDMEX_VDD_SB_H */
