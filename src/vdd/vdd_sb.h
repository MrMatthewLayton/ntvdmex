/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The Sound Blaster 16 VDD: DSP, mixer, and DMA playback.  (GH #20)
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
 * tests/unit/sb_test.c with no host audio anywhere near it.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_VDD_SB_H
#define NTVDMEX_VDD_SB_H

/* SbStartBlock: a DMA block's mode. */
#define SB_SINGLE_CYCLE     0
#define SB_AUTO_INIT        1
#include "audio_format.h"   /* defines only: AUDIO_MONO / AUDIO_STEREO for SbRender */

#include "vdd_bus.h"
#include "vdd_dma.h"
#include "vdd_opl.h"

#define SB_DEFAULT_BASE             0x220
#define SB_DEFAULT_IRQ              5
#define SB_DEFAULT_DMA8             1
#define SB_DEFAULT_DMA16            5
#define SB_PORT_LAST                0x0F
#define SB_BASE_STEP                0x20    /* The Audio page's base choices: 220h, 240h, ... */
#define SB_BASE_CHOICE_MASK         3
#define SB_IRQ_CHOICE_MASK          3
#define SB_EMU8K_PORT_OFFSET        0x400   /* An AWE32's EMU8000 at base + 400h (A220 -> E620) */
#define SB_BLASTER_TYPE_SBPRO       4       /* BLASTER's T */
#define SB_BLASTER_TYPE_SB16        6

/* DSP version we report to command 0xE1. 4.05 = a Sound Blaster 16, which is what
 * a game needs to see before it will use the 16-bit and auto-init commands.
 */
/* #231 (docs/EMULATION.md): three cards, in the Settings list's order. The SB Pro's
 * DSP is 3.02 and has none of the SB16 commands; the AWE32 carries the SB16 DSP (4.12)
 * and, with #233, the EMU8000.
 */
#define SB_MODEL_SB16               0
#define SB_MODEL_AWE32              1
#define SB_MODEL_SBPRO              2
#define SB_MODEL_LAST               SB_MODEL_SBPRO
#define SB_DSP_VERSION_MAJOR        4
#define SB_DSP_VERSION_MINOR        5
#define SB_DSP_VERSION_SBPRO_MAJOR  3       /* 3.02 */
#define SB_DSP_VERSION_SBPRO_MINOR  2
#define SB_DSP_VERSION_AWE32_MINOR  12      /* 4.12 */

/* ...AND THAT CHOICE SELECTS THE GUEST'S ENTIRE DRIVER PATH:
 * DMX branches on it in two places that matter. Its SB interrupt handler
 * (DOOM.EXE 0x53024) tests `version >= 4.00` and, if so, asks MIXER REGISTER 0x82
 * whether the interrupt was really the card's before refilling; below 4.00 it
 * skips that check entirely. And 4.xx is what makes it use the SB16 programmed
 * transfer commands (0xC6 = 8-bit auto-init, measured, issued once) instead of the
 * older 0x48 + 0x1C pair.
 * So the version is not cosmetic -- it picks which of two quite different guest
 * code paths runs against this VDD, and only one of them has ever been exercised.
 * Runtime override so both can be heard without a rebuild; the default is
 * unchanged.
 */
extern BYTE g_SbVersionMajor;
extern BYTE g_SbVersionMinor;
extern INT g_SbGate;    /* ACK gate, opt-in: deviates from the hardware. See vdd_sb.c */

#define SB_DEVICE_NAME          "sb16"
#define SB_STEREO_CHANNELS      2u
#define SB_RUN_BUCKETS          8   /* Run-length histograms */
#define SB_DSP_OPCODES          256
#define SB_MIXER_REGISTERS      256

/* GateMode: the ACK/POLL gate (vdd_sb.c), opt-in through g_SbGate. */
#define SB_GATE_OFF             0
#define SB_GATE_ACK             1   /* VDMSound's: hold until the block IRQ is acked */
#define SB_GATE_POLL            2   /* Hold until the guest polls the DMA position */
#define SB_OUTPUT_QUEUE_MAX     8   /* Bytes the DSP can have waiting to be read */
#define SB_ARGUMENTS_MAX        4   /* Longest command argument list we accept */

/* Playback state of the DSP's transfer engine. */
enum
{
    SB_TRANSFER_IDLE = 0,
    SB_TRANSFER_SINGLE,        /* one block, then IRQ and stop */
    SB_TRANSFER_AUTO           /* ring: IRQ per block, keep going */
};

/* One entry of the block-boundary ledger (SB_STATE.BlockLog -- see the note there). */
typedef struct _SB_BLOCK_RECORD
{
    UINT32 CaptureOffset;      /* bytes captured when the block completed */
    UINT32 BlockLength;        /* what the DSP was told a block is */
    UINT32 Physical;           /* 8237 current physical address, post-fetch */
    WORD CurrentCount;         /* 8237 count remaining (reloaded already on TC) */
    WORD BaseAddress;
    WORD BaseCount;
    BYTE Page;
    BYTE Mode;
    BYTE Ended;
    BYTE Reloaded;
} SB_BLOCK_RECORD, *PSB_BLOCK_RECORD;

typedef struct _SB_STATE
{
    PVDD_BUS   Bus;
    PDMA_STATE Dma;            /* where playback data comes from */
    POPL_STATE Opl;            /* FM mirrored at 2x0-2x3 and 2x8-2x9 */

    WORD BasePort;             /* 0x220 by default */
    BYTE Irq;
    BYTE Dma8;
    BYTE Dma16;
    BYTE  Model;                /* #231: SB_MODEL_* -- which card this DSP is */

    /* DSP command state machine */
    BYTE  Command;             /* command awaiting arguments (0 = none) */
    BYTE  Arguments[SB_ARGUMENTS_MAX];
    BYTE ArgumentCount;
    BYTE ArgumentsWanted;
    BYTE  OutputQueue[SB_OUTPUT_QUEUE_MAX];
    BYTE OutputQueueHead;
    BYTE OutputQueueLength;
    BYTE  IsResetAsserted;     /* 1 = reset asserted, waiting for the 0 write */
    BYTE  IsSpeakerOn;         /* DSP speaker on/off (does not gate DMA) */

    /* transfer engine */
    BYTE  TransferMode;        /* SB_XFER_* */
    BYTE  Is16Bit;       /* 16-bit samples (DMA channel Dma16) */
    BYTE  IsStereo;
    BYTE  IsLegacyTransfer;    /* #189: a DSP 1.x-3.x command started it (rate = BYTE rate) */
    BYTE  IsSigned;
    BYTE  IsPaused;
    UINT32 BlockLength;        /* bytes per block, from the length the game set */
    UINT32 BlockRemaining;     /* bytes still to fetch in this block */
    UINT32 RateHz;             /* sample rate, from time constant or 0x41 */
    BYTE  IsIrqPending;        /* block done: IRQ raised, awaiting ack */

    UINT32 DspWrites;          /* DSP command bytes accepted (diagnostics) */
    UINT32 Blocks;             /* blocks completed -> IRQs raised */
    /* -- [CAUTION] EVERY AUDIO METRIC IN THIS PROJECT MEASURES THE RING. THE USER HEARS THE
     * OUTPUT. --------------------------------------------------------------------
     * `REPLAYED`, `flat`, `byte_lap_same` all compare the guest's DMA buffer against
     * itself a lap earlier. A defect introduced BETWEEN the ring and the IsSpeakerOn is
     * invisible to all of them -- and the symptom reported is exactly that shape:
     * "I hear the gun fire but the sample parts have gaps between them",
     * |- - - - - -| rather than |------|. Gaps are MISSING output, not REPEATED
     * content, so the replay counters could never have shown them.
     * `VddSbRender` emits a zero for every output sample taken while the DSP is
     * un-armed (`SB_TRANSFER_IDLE`) or IsPaused -- and a SINGLE-CYCLE transfer goes IDLE at
     * the end of EVERY block, until the guest re-arms it. If that is what Doom uses,
     * the output is silence-padded once per block by construction.
     * - So count the OUTPUT: how many samples were real, how many were zeros we
     *   inserted, and -- because a rate cannot show a shape -- the LENGTH of each run
     *   of inserted zeros. A few scattered samples and "half of every block" are the
     *   same percentage and completely different sounds.
     */
    BYTE  GateMode;            /* 0=off 1=ACK gate (VDMSound) 2=POLL gate */
    UINT32 GateMark;        /* Dma->CountReads as of the last block IRQ */
    INT16  LastSample;         /* held while the gate is closed */
    /* #189: the same, as the pair the stereo render holds */
    INT16 LastLeft;
    INT16 LastRight;
    UINT32 GateWait;           /* samples the gate has held THIS time */
    UINT32 GateStalled;        /* total samples held */
    UINT32 GateForced;         /* times the safety yielded -- must be ~0 */
    UINT32 Mixer82Reads;       /* guest asks 'was that IRQ yours?'  -- see vdd_sb.c */
    UINT32 Mixer82Zero;        /* ...and we answered NO, so it did not refill */
    UINT32 OutputActive;       /* output samples actually fetched from the ring */
    UINT32 OutputIdle;         /* ...zeros emitted because the DSP was un-armed */
    UINT32 OutputPaused;       /* ...zeros emitted because the guest paused it */
    UINT32 OutputNoDack;       /* ...because the 8237 would not serve the channel
                                  (masked, or its controller disabled) -- #176     */
    UINT32 IdleRun;            /* current run of consecutive inserted zeros */
    UINT32 IdleRuns[SB_RUN_BUCKETS];        /* run lengths, log2 buckets: 1,2,4,8,...,128+ */
    UINT32 CommandHistogram[SB_DSP_OPCODES];    /* DSP commands the guest issued, by opcode */

    /* mixer */
    BYTE  MixerIndex;
    BYTE  Mixer[SB_MIXER_REGISTERS];
    /* RAW CAPTURE OF WHAT WE ACTUALLY PLAY:
     * The DMA ring is the only place the guest's PCM exists, and VddSbRender() is
     * the only thing that reads it -- so a byte-for-byte record of what came out is
     * the audio equivalent of a screenshot. Doom's sound effects are DS* lumps in the
     * IWAD, 8-bit unsigned at 11025 Hz, which makes them an exact oracle: correlate
     * the capture against the lump and any deviation is ours, located in time.
     * Host-owned buffer, filled without I/O so the audio thread never Blocks; the
     * host writes it out at wind-down.
     */
    BYTE *CaptureBuffer;
    UINT32 CaptureLength;
    UINT32 CaptureCapacity;

    /* THE BLOCK-BOUNDARY LEDGER:
     * The click is not a rate fault (41.5 s of audio from a 45 s run) nor a framing
     * fault (corr(L,R) = 0.978). It is a DISCONTINUITY 12.8x over-represented at
     * offset 2 of every 128-frame block -- a defect at the boundary, 86 times a
     * second, which the ear hears as a buzz rather than as clicks.
     * Three things happen at that boundary and only one of them can be two frames
     * out: the completion IRQ we raise, the 8237's auto-init reload of
     * CurrentAddress/CurrentCount, and the guest's refill arriving afterwards. Nothing
     * recorded so far distinguishes them.
     * - CaptureOffset IS THE FIELD THAT MATTERS. sbref.py currently INFERS the 128-frame
     *   period from the capture and then measures against its own inference. Writing
     *   down where each block actually ended, as an offset into the very buffer being
     *   analysed, replaces that inference with a fact -- so "the jump is two frames
     *   into the block" stops being a property of a guessed grid.
     *   Bounded and allocation-free: the first few Blocks settle it, and the audio
     *   thread must never do I/O, so the host prints this at wind-down.
     */
#define SB_BLOCK_LOG_MAX    24
    SB_BLOCK_RECORD BlockLog[SB_BLOCK_LOG_MAX];
    UINT32 BlockLogCount;      /* blocks seen; entries kept = min(n, MAX) */

    /* THE REPLAY DETECTOR:
     * The residual click is an ECHO, and the user heard it as one before it was
     * measured: the capture is 46.0% identical to the byte ONE RING LAP earlier
     * (4096 bytes = 185.8 ms at 11025 Hz stereo) against ~22% at 2048, 8192, and
     * at 4095/4097 -- a spike that collapses one byte either side is a literal
     * repeat, not a correlation. We are re-reading ring content DMX has not
     * refilled yet, so the same audio is played twice 186 ms apart.
     * - WHY THIS LIVES IN THE HOST AND NOT IN THE ANALYSIS SCRIPT. Post-hoc capture
     *   analysis needs sbdump.flag, a copy off the box, and a matching anchor, and it
     *   cannot be correlated with anything happening inside the run. As a live counter
     *   the replay rate becomes a number in STAGE2 -- which makes the AUDIO LEAD a
     *   CONTROLLED VARIABLE: vary aw->nbufs and see whether replays move with it. If
     *   they do it is the lead-vs-refill race; if they do not, DMX is failing to refill
     *   for some other reason and the lead is the wrong suspect.
     *   Compare each fetched byte against what we fetched at the same RING OFFSET one
     *   lap ago. Ring-sized shadow, no allocation, no I/O -- this runs on the audio
     *   thread. Rings larger than the shadow simply disable the check (counted).
     */
#define SB_LAP_MAX      8192

/* Peak-to-peak span, in raw DMA bytes, at or below which a block carries no audio. */
#define SB_FLAT_RANGE   2
    BYTE  LapBuffer[SB_LAP_MAX];    /* what we read at each ring offset last lap */
    UINT32 LapLength;               /* ring size currently being tracked (0 = off) */
    UINT32 LapSeen;                 /* bytes fetched since the ring was programmed */
    /* accumulators for the block in progress */
    UINT32 BlockSame;
    UINT32 BlockBytes;
    UINT32 BlocksChecked;           /* blocks that had a full previous lap to compare */
    UINT32 BlocksReplayed;          /* ...of which >=90% identical: DMX never refilled */
    /* [CAUTION]: ...AND THAT LAST COMMENT IS A CONCLUSION, NOT A MEASUREMENT. "identical to one
     * lap earlier" is what a MISSING REFILL looks like -- and it is also what a
     * CORRECT refill looks like whenever the guest writes the same bytes again, which
     * for 8-bit PCM means every stretch of silence (0x80) and every sustained flat
     * tone. Doom's attract demo is quiet for long stretches, so a large fraction of
     * this counter may be the game being silent rather than the host losing data, and
     * the two need completely different work. The metric cannot separate them and
     * session 23 flagged the risk in prose without instrumenting it.
     * So classify each block by its own DYNAMIC RANGE as well: a block whose bytes
     * span almost no range carries no audio, and a "replay" verdict on it says
     * nothing. `replayed_loud` -- replayed AND carrying signal -- is the only one of
     * these numbers the defect claim can rest on. One min/max per byte, in a loop
     * that already runs.
     */
    UINT32 BlocksFlat;              /* blocks with essentially no dynamic range */
    UINT32 BlocksReplayedLoud;      /* replayed AND not flat: the number that counts */
    /* AND THE SHAPE OF THE FAILURE, WHICH A RATE CANNOT SHOW:
     * "32% of audible Blocks are lap repeats" is the same number whether every
     * third block is stale (a race at the margin: our read head and DMX's write
     * head are too close, and the fix is the LEAD) or whether the run is fine
     * for a second and then replays forty Blocks in a row (a STALL: something
     * stops DMX refilling at all for ~half a second, and the lead is irrelevant).
     * Those are different bugs. The run-length distribution separates them for
     * one comparison and one counter per block.
     */
    UINT32 ReplayRun;               /* consecutive replayed-loud blocks, in progress */
    UINT32 ReplayRuns[SB_RUN_BUCKETS];           /* run lengths 1,2,3,4-7,8-15,16-31,32-63,64+ */
    UINT32 ReplayRunMax;
    UINT32 FlatRun;                 /* consecutive FLAT blocks, in progress */
    UINT32 FlatRuns[SB_RUN_BUCKETS];             /* ...run lengths, same buckets. runs of 1 = the
                                       audible DROPOUTS; long runs = real silence.   */
    /* range accumulators for the block in progress */
    UINT32 BlockMin;
    UINT32 BlockMax;
    /* byte-level rate, the live form of the 46% */
    UINT32 LapSame;
    UINT32 LapTotal;
    UINT32 LapTooBig;               /* rings larger than SB_LAP_MAX: check skipped */
    UINT32 LapOffset;               /* ring offset of the fetch in progress */
} SB_STATE, *PSB_STATE;
typedef const SB_STATE *PCSB_STATE;

/* Build the device descriptor to hand to VddBusAdd(). Set base/irq/dma and the
 * dma/opl back-pointers before adding.
 */
INT  VddSbInitialize(_In_ PVDD_BUS bus, _In_ PVOID context);
VOID VddSbReset(_In_ PVOID context);
static inline NTVDD_DEVICE VddSbDevice(_In_ PSB_STATE state)
{ NTVDD_DEVICE device;
device.Name = SB_DEVICE_NAME;
device.Initialize = VddSbInitialize;
device.Reset = VddSbReset;
  device.Shutdown = 0;
  device.Context = state;
  return device; }

/* Pull up to `frames` samples of playback into `output` (mono 16-bit at the card's
 * current rate), fetching through the DMA controller and raising the completion
 * IRQ when a block ends. Returns frames produced; silence (zeros) when idle, so
 * the mixer can always call it.
 */
UINT32 VddSbRender(_Inout_ PSB_STATE state, _Out_writes_(frames) INT16 *output, _In_ UINT32 frames);
/* #189: the same transport, rendering interleaved L/R pairs (2*frames samples). An
 * 8-bit or 16-bit MONO transfer gives L = R.
 */
UINT32 VddSbRenderStereo(
    _Inout_ PSB_STATE state,
    _Out_writes_(SB_STEREO_CHANNELS * frames) INT16 *output,
    _In_ UINT32 frames);
/* #189: frames per second of the transfer in progress -- the programmed rate, halved
 * for an SB Pro stereo transfer (its time constant counts both channels).
 */
UINT32 VddSbFrameHz(_In_ PCSB_STATE state);

/* True while a programmed transfer is running (test/mixer convenience). */
static inline INT VddSbIsActive(_In_ PCSB_STATE state)
{
    return state->TransferMode != SB_TRANSFER_IDLE && !state->IsPaused;
}

/* nosb.flag: when set, the DSP reset handshake withholds its 0xAA so a detect
 * fails, i.e. the machine reports no Sound Blaster fitted. See vdd_sb.c.
 */
extern INT g_SbAbsent;

#endif /* NTVDMEX_VDD_SB_H */
