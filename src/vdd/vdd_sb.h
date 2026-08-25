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
/* ── ...AND THAT CHOICE SELECTS THE GUEST'S ENTIRE DRIVER PATH. ──────────────────
     DMX branches on it in two places that matter. Its SB interrupt handler
     (DOOM.EXE 0x53024) tests `version >= 4.00` and, if so, asks MIXER REGISTER 0x82
     whether the interrupt was really the card's before refilling; below 4.00 it
     skips that check entirely. And 4.xx is what makes it use the SB16 programmed
     transfer commands (0xC6 = 8-bit auto-init, measured, issued once) instead of the
     older 0x48 + 0x1C pair.
     So the version is not cosmetic -- it picks which of two quite different guest
     code paths runs against this VDD, and only one of them has ever been exercised.
     Runtime override so both can be heard without a rebuild; the default is
     unchanged. */
extern uint8_t g_sb_ver_major, g_sb_ver_minor;
extern int g_sb_gate;   /* ACK gate, opt-in: deviates from the hardware. See vdd_sb.c */

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
    /* ── ⚠ EVERY AUDIO METRIC IN THIS PROJECT MEASURES THE RING. THE USER HEARS THE
         OUTPUT. ────────────────────────────────────────────────────────────────────
         `REPLAYED`, `flat`, `byte_lap_same` all compare the guest's DMA buffer against
         itself a lap earlier. A defect introduced BETWEEN the ring and the speaker is
         invisible to all of them -- and the symptom reported is exactly that shape:
         "I hear the gun fire but the sample parts have gaps between them",
         |- - - - - -| rather than |------|. Gaps are MISSING output, not REPEATED
         content, so the replay counters could never have shown them.
         `vdd_sb_render` emits a zero for every output sample taken while the DSP is
         un-armed (`SB_XFER_IDLE`) or paused -- and a SINGLE-CYCLE transfer goes IDLE at
         the end of EVERY block, until the guest re-arms it. If that is what Doom uses,
         the output is silence-padded once per block by construction.
       ► So count the OUTPUT: how many samples were real, how many were zeros we
         inserted, and -- because a rate cannot show a shape -- the LENGTH of each run
         of inserted zeros. A few scattered samples and "half of every block" are the
         same percentage and completely different sounds. */
    uint8_t  gate_on;          /* 0=off 1=ACK gate (VDMSound) 2=POLL gate           */
    uint32_t gate_mark;        /* dma->count_reads as of the last block IRQ         */
    int16_t  last_sample;      /* held while the gate is closed                     */
    uint32_t gate_wait;        /* samples the gate has held THIS time               */
    uint32_t gate_stalled;     /* total samples held                                */
    uint32_t gate_forced;      /* times the safety yielded -- must be ~0            */
    uint32_t mix82_reads;      /* guest asks 'was that IRQ yours?'  -- see vdd_sb.c */
    uint32_t mix82_zero;       /* ...and we answered NO, so it did not refill      */
    uint32_t out_active;       /* output samples actually fetched from the ring   */
    uint32_t out_idle;         /* ...zeros emitted because the DSP was un-armed   */
    uint32_t out_paused;       /* ...zeros emitted because the guest paused it    */
    uint32_t idle_run;         /* current run of consecutive inserted zeros       */
    uint32_t idle_runs[8];     /* run lengths, log2 buckets: 1,2,4,8,...,128+     */
    uint32_t cmd_hist[256];    /* DSP commands the guest issued, by opcode        */

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

    /* ── THE REPLAY DETECTOR. ────────────────────────────────────────────────────
         The residual click is an ECHO, and the user heard it as one before it was
         measured: the capture is 46.0% identical to the byte ONE RING LAP earlier
         (4096 bytes = 185.8 ms at 11025 Hz stereo) against ~22% at 2048, 8192, and
         at 4095/4097 -- a spike that collapses one byte either side is a literal
         repeat, not a correlation. We are re-reading ring content DMX has not
         refilled yet, so the same audio is played twice 186 ms apart.
       ► WHY THIS LIVES IN THE HOST AND NOT IN THE ANALYSIS SCRIPT. Post-hoc capture
         analysis needs sbdump.flag, a copy off the box, and a matching anchor, and it
         cannot be correlated with anything happening inside the run. As a live counter
         the replay rate becomes a number in STAGE2 -- which makes the AUDIO LEAD a
         CONTROLLED VARIABLE: vary aw->nbufs and see whether replays move with it. If
         they do it is the lead-vs-refill race; if they do not, DMX is failing to refill
         for some other reason and the lead is the wrong suspect.
         Compare each fetched byte against what we fetched at the same RING OFFSET one
         lap ago. Ring-sized shadow, no allocation, no I/O -- this runs on the audio
         thread. Rings larger than the shadow simply disable the check (counted). */
#define SB_LAP_MAX 8192
/* Peak-to-peak span, in raw DMA bytes, at or below which a block carries no audio. */
#define SB_FLAT_RANGE 2
    uint8_t  lap_buf[SB_LAP_MAX];   /* what we read at each ring offset last lap    */
    uint32_t lap_len;               /* ring size currently being tracked (0 = off)  */
    uint32_t lap_seen;              /* bytes fetched since the ring was programmed  */
    uint32_t blk_same, blk_bytes;   /* accumulators for the block in progress       */
    uint32_t blocks_checked;        /* blocks that had a full previous lap to compare */
    uint32_t blocks_replayed;       /* ...of which >=90% identical: DMX never refilled */
    /* ⚠ ...AND THAT LAST COMMENT IS A CONCLUSION, NOT A MEASUREMENT. "identical to one
         lap earlier" is what a MISSING REFILL looks like -- and it is also what a
         CORRECT refill looks like whenever the guest writes the same bytes again, which
         for 8-bit PCM means every stretch of silence (0x80) and every sustained flat
         tone. Doom's attract demo is quiet for long stretches, so a large fraction of
         this counter may be the game being silent rather than the host losing data, and
         the two need completely different work. The metric cannot separate them and
         session 23 flagged the risk in prose without instrumenting it.
         So classify each block by its own DYNAMIC RANGE as well: a block whose bytes
         span almost no range carries no audio, and a "replay" verdict on it says
         nothing. `replayed_loud` -- replayed AND carrying signal -- is the only one of
         these numbers the defect claim can rest on. One min/max per byte, in a loop
         that already runs. */
    uint32_t blocks_flat;           /* blocks with essentially no dynamic range      */
    uint32_t blocks_replayed_loud;  /* replayed AND not flat: the number that counts */
    /* ── AND THE SHAPE OF THE FAILURE, WHICH A RATE CANNOT SHOW. ─────────────────
         "32% of audible blocks are lap repeats" is the same number whether every
         third block is stale (a race at the margin: our read head and DMX's write
         head are too close, and the fix is the LEAD) or whether the run is fine
         for a second and then replays forty blocks in a row (a STALL: something
         stops DMX refilling at all for ~half a second, and the lead is irrelevant).
         Those are different bugs. The run-length distribution separates them for
         one comparison and one counter per block. */
    uint32_t replay_run;            /* consecutive replayed-loud blocks, in progress */
    uint32_t replay_runs[8];        /* run lengths 1,2,3,4-7,8-15,16-31,32-63,64+    */
    uint32_t replay_run_max;
    uint32_t flat_run;              /* consecutive FLAT blocks, in progress          */
    uint32_t flat_runs[8];          /* ...run lengths, same buckets. runs of 1 = the
                                       audible DROPOUTS; long runs = real silence.   */
    uint32_t blk_min, blk_max;      /* range accumulators for the block in progress  */
    uint32_t lap_same, lap_total;   /* byte-level rate, the live form of the 46%    */
    uint32_t lap_toobig;            /* rings larger than SB_LAP_MAX: check skipped  */
    uint32_t lap_off;               /* ring offset of the fetch in progress         */
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
