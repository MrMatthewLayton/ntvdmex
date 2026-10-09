/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The AdLib / OPL2 (Yamaha YM3812) and OPL3 (YMF262) VDD.
 *                                                 (sound epic, GH #21; OPL3 #232)
 *
 * Ports 0x388 (address latch + status read) and 0x389 (data); on an OPL3 also
 * 0x38A (array-1 address) and 0x38B (data). This replaces the
 * detection stub that was bolted onto the video VDD: that stub answered detection
 * by toggling the status bits on every read, which made games believe an OPL was
 * present and then commit to a music path nothing implemented. A real register
 * model plus REAL timers is what a game actually needs -- AdLib detection is a
 * timer measurement, and games pace their music on timer overflow.
 *
 * Two timers, and they are the whole reason detection works:
 * T1 (register 0x02) counts 80us steps, T2 (register 0x03) counts 320us steps.
 * Each counts UP from its preset to 256, overflows, sets its status flag, and
 * reloads. The canonical detect writes 0xFF to T1, starts it, delays ~80us, and
 * expects status 0xC0 (IRQ + T1 expired); anything else means "no AdLib".
 *
 * Time is injected, not read: VddOplAddMicroseconds() advances the timers, so the device
 * stays pure C with no clock of its own and the whole thing is exercised off-VM by
 * tests/unit/opl_test.c. The host pumps it from real elapsed time; the mixer
 * pumps it from the sample clock, which is what keeps music in tempo.
 *
 * FM synthesis lives in vdd_opl_synth.c behind VddOplRender(); this file owns
 * the programmer-visible device.
 *
 * -- TWO CHIPS, ONE MODEL (GH #232). `OPL_STATE.IsOpl3` says which is fitted:
 * 0  YM3812 (OPL2) -- an AdLib. Ports 0x388/0x389 only; 0x38A/0x38B are not
 *    decoded (writes vanish, reads float 0xFF), and status bits 1-2 read 1.
 * 1  YMF262 (OPL3) -- an SB16/AWE32. A SECOND REGISTER ARRAY at 0x100-0x1FF,
 *    reached by writing the address to 0x38A instead of 0x388 (there is ONE
 *    9-bit address latch; A1 on the address write is its top bit, and the data
 *    port writes wherever it points). Status bits 1-2 read 0 -- which is the
 *    whole of how software tells the two chips apart (see VddOplReadStatus).
 * The OPL3 powers up OPL2-COMPATIBLE: until register 0x105 bit 0 (NEW) is set,
 * array 1 latches but does nothing, waveforms are 2 bits, and output is mono.
 * With NEW set it is 18 channels, pairable into 4-operator voices (0x104),
 * 8 waveforms, and each channel is routed left/right by C0-C8 bits 4/5.
 * The chip type survives VddOplReset(): it is a fact about the CARD, set by
 * the host from the `Opl` setting, not guest-visible state.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_VDD_OPL_H
#define NTVDMEX_VDD_OPL_H

#include "vdd_bus.h"

/* VddOplOperatorIndex: which operator of a channel. */
#define OPL_MODULATOR                       0
#define OPL_CARRIER                         1

#define OPL_PORT_FIRST                      0x388

#define OPL_CHANNELS                        9       /* OPL2 (and OPL3 array 0): 9 two-operator channels */
#define OPL_OPERATORS                       18
#define OPL3_CHANNELS                       18      /* OPL3: two arrays of 9 */
#define OPL3_OPERATORS                      36
#define OPL3_REGISTERS                      0x200   /* Array 0 at 0x000-0x0FF, array 1 at 0x100-0x1FF */

/* OPL3-only registers, in array 1 (so 9-bit numbers). */
#define OPL3_REGISTER_FOUR_OPERATOR         0x104   /* Bits 0-5: pair ch 0+3, 1+4, 2+5, 9+12, 10+13, 11+14 */
#define OPL3_REGISTER_NEW                   0x105   /* Bit 0: NEW -- the OPL3 extensions are live */

/* The register file's layout. Per-operator registers are a base plus an operator
 * OFFSET (three banks of six slots, 0x00-0x05, 0x08-0x0D, 0x10-0x15); per-channel
 * registers a base plus the channel (0-8). Array 1 is the same at +0x100.
 */
#define OPL_REGISTER_TEST                   0x01
#define OPL_REGISTER_TIMER1                 0x02
#define OPL_REGISTER_TIMER2                 0x03
#define OPL_REGISTER_TIMER_CONTROL          0x04
#define OPL_REGISTER_AM_VIB                 0x20    /* AM/VIB/EGT/KSR/MULT */
#define OPL_REGISTER_AM_VIB_LAST            0x35
#define OPL_REGISTER_KSL_TL                 0x40    /* Key-scale level, total level */
#define OPL_REGISTER_KSL_TL_LAST            0x55
#define OPL_REGISTER_AR_DR                  0x60    /* Attack, decay rates */
#define OPL_REGISTER_AR_DR_LAST             0x75
#define OPL_REGISTER_SL_RR                  0x80    /* Sustain level, release rate */
#define OPL_REGISTER_SL_RR_LAST             0x95
#define OPL_REGISTER_FNUMBER_LOW            0xA0
#define OPL_REGISTER_FNUMBER_LOW_LAST       0xA8
#define OPL_REGISTER_KEY_BLOCK              0xB0    /* Key-on, block, F-number high */
#define OPL_REGISTER_KEY_BLOCK_LAST         0xB8
#define OPL_REGISTER_RHYTHM                 0xBD
#define OPL_REGISTER_FEEDBACK               0xC0    /* Feedback, connection (and OPL3 routing) */
#define OPL_REGISTER_FEEDBACK_LAST          0xC8
#define OPL_REGISTER_WAVEFORM               0xE0
#define OPL_REGISTER_WAVEFORM_LAST          0xF5
#define OPL_ARRAY_SHIFT                     8       /* Bit 8 of a register number: the array */
#define OPL_ARRAY1_BASE                     0x100
#define OPL_OPERATOR_OFFSET_MASK            0x1F
#define OPL_OPERATOR_BANK_SHIFT             3
#define OPL_OPERATOR_SLOT_MASK              7
#define OPL_OPERATOR_BANKS                  3
#define OPL_OPERATOR_BANK_SLOTS             6
#define OPL_CHANNELS_PER_BANK               3
#define OPL_CARRIER_OFFSET                  3       /* A channel's carrier: three slots on */
#define OPL_NO_OPERATOR                     (-1)

/* Register fields. */
#define OPL_AM_VIB_AM                       0x80
#define OPL_AM_VIB_VIB                      0x40
#define OPL_AM_VIB_AM_SHIFT                 7
#define OPL_AM_VIB_VIB_SHIFT                6
#define OPL_AM_VIB_EGT_SHIFT                5
#define OPL_AM_VIB_KSR_SHIFT                4
#define OPL_AM_VIB_MULT_MASK                0x0F
#define OPL_KSL_SHIFT                       6
#define OPL_KSL_MASK                        3
#define OPL_TL_MASK                         0x3F
#define OPL_RATE_HIGH_SHIFT                 4       /* AR and SL in the high nibble */
#define OPL_RATE_MASK                       0x0F
#define OPL_WAVEFORM_MASK                   7
#define OPL_FNUMBER_LOW_BITS                0xFF
#define OPL_FNUMBER_HIGH_BITS               0x300
#define OPL_FNUMBER_HIGH_MASK               3
#define OPL_FNUMBER_HIGH_SHIFT              8
#define OPL_KEY_ON                          0x20
#define OPL_KEY_ON_SHIFT                    5
#define OPL_BLOCK_SHIFT                     2
#define OPL_BLOCK_MASK                      7
#define OPL_FEEDBACK_SHIFT                  1
#define OPL_FEEDBACK_MASK                   7
#define OPL_TEST_WSE                        0x20    /* Register 0x01: waveform select enable */

/* 4-operator pairs (OPL3_REGISTER_FOUR_OPERATOR). */
#define OPL_FOUR_OPERATOR_FIRST             1       /* OplFourOperatorRole: leads a pair */
#define OPL_FOUR_OPERATOR_SECOND            2       /* ...is the pair's second channel */
#define OPL_FOUR_OPERATOR_PARTNER           3       /* The second channel is the first + 3 */
#define OPL_FOUR_OPERATOR_CHANNELS          6       /* Channels 0-5 of an array can pair */
#define OPL_FOUR_OPERATOR_PAIR_CHANNELS     2
#define OPL_FOUR_OPERATOR_ARRAY1_BITS       3       /* Bits 3-5: array 1's pairs */

/* Rhythm mode: operators 12-17 on channels 6-8. */
#define OPL_RHYTHM_FIRST_OPERATOR           12
#define OPL_RHYTHM_OPERATORS                6
#define OPL_RHYTHM_CHANNELS                 3
#define OPL_RHYTHM_CHANNEL_BASS_DRUM        6
#define OPL_RHYTHM_CHANNEL_HIHAT_SNARE      7
#define OPL_RHYTHM_CHANNEL_TOM_CYMBAL       8
#define OPL_OPERATOR_BASS_DRUM_MODULATOR    12
#define OPL_OPERATOR_HIHAT                  13
#define OPL_OPERATOR_TOM_TOM                14
#define OPL_OPERATOR_BASS_DRUM_CARRIER      15
#define OPL_OPERATOR_SNARE                  16
#define OPL_OPERATOR_CYMBAL                 17
#define OPL_RHYTHM_RESTART_HIHAT            1       /* RhythmRestart bits */
#define OPL_RHYTHM_RESTART_CYMBAL           2
#define OPL_RHYTHM_VOICES                   5       /* Hi-hat, cymbal, tom-tom, snare, bass */
#define OPL_BD_DRUMS                        0x1F    /* 0xBD bits 0-4: the drums' keys */
#define OPL_STEREO_CHANNELS                 2

/* C0-C8 (and 0x1C0-0x1C8) output-routing bits, OPL3 with NEW set. The chip has
 * FOUR outputs, A-D. A Sound Blaster 16 / AWE32 wires A to the left DAC and B to
 * the right; C and D are brought out of the chip and routed NOWHERE on those cards,
 * so a voice sent only to C/D is silent there -- and is here, deliberately.
 */
#define OPL_C0_OUTPUT_A                     0x10    /* output A -> left */
#define OPL_C0_OUTPUT_B                     0x20    /* output B -> right */
#define OPL_C0_OUTPUT_C                     0x40    /* output C -- not connected on an SB16 */
#define OPL_C0_OUTPUT_D                     0x80    /* output D -- not connected on an SB16 */

#define OPL_TIMER1_US                       80      /* Timer 1 resolution, microseconds */
#define OPL_TIMER2_US                       320     /* Timer 2 resolution */
#define OPL_DEFAULT_HZ                      44100u  /* Render rate */
#define OPL_DEFAULT_FRAME_US                16667u  /* ~60 Hz bus frame tick */

/* status register bits (read from port 0x388) */
#define OPL_STATUS_IRQ                      0x80
#define OPL_STATUS_TIMER1                   0x40
#define OPL_STATUS_TIMER2                   0x20

/* Status bits 1 and 2 are the chip's ID. A YM3812 reads them as 1, a YMF262 as 0,
 * and "(status & 0x06) == 0 means OPL3" is the test drivers use (Creative's SB16
 * documentation, and every OPL3 detect after it). The timer detect masks with
 * 0xE0, so an AdLib detect passes on both chips.
 */
#define OPL_STATUS_OPL2_ID                  0x06

/* register 0xBD: LFO depths, and rhythm mode. The two depth bits scale the chip's
 * single shared tremolo and vibrato oscillators; the AM/VIB bits in 0x20-0x35 say
 * which operators listen to them.
 */
#define OPL_BD_TREMOLO_DEPTH                0x80    /* Tremolo depth: 0 = 1.2 dB, 1 = 4.9 dB */
#define OPL_BD_VIBRATO_DEPTH                0x40    /* Vibrato depth: 1 = double */
#define OPL_BD_RHYTHM                       0x20    /* Rhythm mode enable */

/* The noise LFSR's power-on contents: ONE set bit. MEASURED, not chosen
 * (`oplprobe noise`): the reference's hi-hat and snare noise, read out of its
 * output, solves to exactly this state at the point our renderer starts from
 * after reset (it is stepped before each sample). See vdd_opl_synth.c.
 */
#define OPL_NOISE_SEED                      0x100000u

/* register 0x04 (timer control) bits */
#define OPL_TIMER_CONTROL_T1_START          0x01
#define OPL_TIMER_CONTROL_T2_START          0x02
#define OPL_TIMER_CONTROL_T2_MASK           0x20
#define OPL_TIMER_CONTROL_T1_MASK           0x40
#define OPL_TIMER_CONTROL_IRQ_RESET         0x80

/* The envelope counts ATTENUATION, so 0 is full volume and OPL_ENVELOPE_MAX is silence
 * -- the opposite of the intuitive reading, and the direction that matters when
 * initialising it. Carried in fixed point because the slowest rate advances only
 * one unit per 4096 samples; 511 << 20 still fits an int32. See vdd_opl_synth.c.
 */
#define OPL_ENVELOPE_MAX                    511     /* Fully attenuated */
#define OPL_ENVELOPE_SHIFT                  20      /* Fractional bits */
#define OPL_ENVELOPE_FULL                   ((INT32)OPL_ENVELOPE_MAX << OPL_ENVELOPE_SHIFT)

/* Render `frames` samples at the chip's NATIVE 49716 Hz (vdd_opl_synth.c); the
 * mixer resamples to the host rate. Rendering at the native rate keeps the phase
 * arithmetic exact, which is what makes the pitch correct.
 *   VddOplRender     mono. OPL2 (or OPL3 with NEW clear): exactly the chip's
 *                      one output. OPL3 with NEW set: (left + right) / 2.
 *   VddOplRenderStereo  interleaved L/R, 2*frames samples. Without NEW both sides
 *                      carry the mono signal, identical to VddOplRender.
 */
#define OPL_NATIVE_HZ                       49716u

/* envelope generator phase */
enum
{
    OPL_ENVELOPE_OFF = 0, OPL_ENVELOPE_ATTACK, OPL_ENVELOPE_DECAY, OPL_ENVELOPE_SUSTAIN, OPL_ENVELOPE_RELEASE
};

typedef struct _OPL_OPERATOR
{
    /* programmed by the register file */
    /* 0x20-0x35 */
    BYTE AmplitudeModulation;
    BYTE Vibrato;
    BYTE EnvelopeType;
    BYTE KeyScaleRate;
    BYTE Multiplier;
    /* 0x40-0x55: key-scale level, total level */
    BYTE KeyScaleLevel;
    BYTE TotalLevel;
    /* 0x60-0x75: attack, decay rates */
    BYTE AttackRate;
    BYTE DecayRate;
    /* 0x80-0x95: sustain level, release rate */
    BYTE SustainLevel;
    BYTE ReleaseRate;
    /* 0xE0-0xF5: waveform select, the 3 bits AS WRITTEN. How many of them count is
     * decided at render time (OplEffectiveWaveform in vdd_opl_synth.c), because it depends
     * on registers written later -- NEW, and on an OPL2 the WSE bit in 0x01.
     */
    BYTE Waveform;
    /* synthesis state */
    UINT32 Phase;                       /* phase accumulator, 10.10 fixed point */
    INT32  Envelope;                       /* attenuation, OPL_ENVELOPE_SHIFT fixed point */
    BYTE  EnvelopeState;
    /* last two outputs, for feedback */
    INT32 Output1;
    INT32 Output2;
} OPL_OPERATOR, *POPL_OPERATOR;
typedef const OPL_OPERATOR *PCOPL_OPERATOR;

typedef struct _OPL_CHANNEL
{
    WORD FNumber;                       /* 10-bit frequency number */
    BYTE  Block;                        /* 3-bit octave */
    BYTE  IsKeyOn;
    BYTE  Feedback;                     /* feedback level */
    BYTE  Connection;                   /* 0 = FM (op1 modulates op2), 1 = additive */
} OPL_CHANNEL, *POPL_CHANNEL;
typedef const OPL_CHANNEL *PCOPL_CHANNEL;

typedef struct _OPL_STATE
{
    PVDD_BUS Bus;
    /* Raw register file as written, both arrays: Registers[0x0B0] is array 0's 0xB0,
     * Registers[0x1B0] array 1's. On an OPL2 the top half is never written.
     */
    BYTE  Registers[OPL3_REGISTERS];
    WORD AddressLatch;                  /* 9-bit address latch (0x388, or 0x38A
                                           for array 1 on an OPL3)                */
    BYTE  IsOpl3;                       /* chip fitted: 0 = YM3812, 1 = YMF262.
                                           Set by the host; survives reset.       */
    OPL_OPERATOR   Operators[OPL3_OPERATORS];           /* 0-17 array 0, 18-35 array 1 */
    OPL_CHANNEL   Channels[OPL3_CHANNELS];           /* 0-8 array 0, 9-17 array 1 */

    /* timers */
    BYTE Timer1Preset;
    BYTE Timer2Preset;
    BYTE IsTimer1Running;
    BYTE IsTimer2Running;
    BYTE IsTimer1Masked;
    BYTE IsTimer2Masked;
    /* current up-counters (preset..256) */
    WORD Timer1Count;
    WORD Timer2Count;
    BYTE  Status;                       /* timer/IRQ flags (bits 5-7). A read of
                                           0x388 is this plus the chip ID bits --
                                           use VddOplReadStatus(), not this.   */
    /* microseconds not yet turned into steps */
    UINT32 Timer1FractionUs;
    UINT32 Timer2FractionUs;

    /* Free-running sample counter driving BOTH low-frequency oscillators. They are
     * properties of the chip, not of a note: one tremolo and one vibrato shared by
     * all 18 operators, never restarted by key-on. Two notes struck a beat apart
     * are therefore at different points in the sweep, which is most of what makes
     * the effect sound like an instrument rather than a wobble.
     */
    UINT32 LfoCount;

    /* The chip's NOISE generator: a 23-bit LFSR, also free-running from power-on
     * and never restarted by key-on. Only the hi-hat and snare read it. Held as
     * the last 23 bits it produced (bit 0 oldest); VddOplReset() seeds it with
     * OPL_NOISE_SEED. See OplNoiseStep() in vdd_opl_synth.c for how, and how
     * much of that was measured.
     */
    UINT32 Noise;
    /* Bit 0 op13, bit 1 op17: keyed on in rhythm mode since the last sample, so
     * the accumulator restarts one step further on (see OplRhythmSample).
     */
    BYTE  RhythmRestart;

    UINT32 SampleHz;                    /* render rate (0 => OPL_DEFAULT_HZ) */
    UINT32 FrameUs;                     /* microseconds per bus frame tick */
    BYTE  IsExternalClock;                 /* 1 = host drives time via VddOplAddMicroseconds,
                                           so the coarse frame tick must NOT also
                                           advance the timers (that would double-
                                           count and run music at 2x tempo)       */

    /* Register-write trace hook (dev only). Set by the host to capture the exact
     * stream a game sends, so it can be replayed offline through BOTH this synth
     * and a reference core (Nuked OPL3) and the outputs diffed. Counting register
     * writes cannot answer "why does this instrument sound wrong"; comparing
     * waveforms from identical input can. NULL in normal runs and in the battery.
     * ARRAY 0 ONLY: the hook's register is 8 bits wide and the Trace format has
     * no bank column, so an OPL3 program's array-1 writes are not captured.
     */
    VOID (*Trace)(BYTE registerIndex, BYTE value);

    /* WHAT THE GAME ACTUALLY ASKS FOR (GH #21):
     * The synth has three declared gaps -- tremolo/vibrato depth (0xBD), rhythm
     * mode (0xBD), and envelope rates anchored only to within ~2x at the extremes
     * -- and "the music sounds a bit flat" could be any of them. Rather than rank
     * them by ear, record what the guest's music driver REALLY writes: a feature
     * the game never touches cannot be the cause, so this turns three plausible
     * stories into one by elimination. Reported in the STAGE2 block.
     */
    UINT32 ProfileWrites;               /* register writes seen */
    UINT32 ProfileKeyOns;               /* key-on edges (notes started) */
    UINT32 ProfileBdWrites;             /* writes to 0xBD specifically */
    BYTE  ProfileBdOr;                  /* OR of every value written to 0xBD */
    BYTE  ProfileWaveformSelect;        /* reg 0x01 bit 5 (waveform select enable) */
    BYTE  ProfileWaveMask;              /* OR of 1<<wave (3 bits) over every op */
    /* Bit n = operator slot n of EITHER array: array 1's 18 slots fold onto the
     * same 18 bits, which keeps the STAGE2 line one 32-bit number.
     */
    UINT32 ProfileAmOperators;          /* bitmask: operators that ever set AM */
    UINT32 ProfileVibratoOperators;     /* bitmask: operators that ever set VIB */
    UINT32 ProfileKeyOnAm;              /* notes started with AM on either operator */
    UINT32 ProfileKeyOnVibrato;         /* notes started with VIB on either op */
    /* Percussion hits by voice: hi-hat, cymbal, tom-tom, snare, bass drum. EDGES,
     * not an OR over the run -- a counter that only says a feature was TOUCHED
     * once produced a confident wrong answer about this very register. (Until
     * #139 three of the five were silent and this was their loud-failure report;
     * all five are synthesised now.)
     */
    UINT32 ProfileRhythmHits[OPL_RHYTHM_VOICES];
} OPL_STATE, *POPL_STATE;
typedef const OPL_STATE *PCOPL_STATE;

/* nosb.flag: when set, the status port floats (0xFF) so an AdLib detect fails. */
extern INT g_OplAbsent;

/* Build the device descriptor to hand to VddBusAdd(). */
INT  VddOplInitialize(_In_ PVDD_BUS bus, _In_ PVOID context);
VOID VddOplReset(_In_ PVOID context);

/* Advance the timers by `microseconds`, raising status flags on overflow.
 * Exposed rather than driven by a clock inside the device so tests can step it
 * exactly and the mixer can drive it from the sample clock.
 */
VOID VddOplAddMicroseconds(_Inout_ POPL_STATE state, _In_ UINT32 microseconds);

/* Direct register write, `registerNumber` 9 bits (0x1xx = OPL3 array 1; ignored on an
 * OPL2). Used by the data port, tests, and the host.
 */
VOID VddOplWriteRegister(_Inout_ POPL_STATE state, _In_ WORD registerNumber, _In_ BYTE value);

/* The port-level interface, for anything that decodes the chip's A0/A1 lines
 * itself -- the Sound Blaster's mirrors at 2x0-2x3 and 2x8/2x9 (vdd_sb.c).
 * `arrayIndex` 1 is A1 high (0x38A / 2x2): on an OPL2 it is not decoded and the write
 * is dropped. The data write goes to whatever the latch holds.
 */
VOID    VddOplWriteAddress(_Inout_ POPL_STATE state, _In_ INT arrayIndex, _In_ BYTE value);
VOID    VddOplWriteData(_Inout_ POPL_STATE state, _In_ BYTE value);
/* A status read (0x388; on an OPL3 also 0x38A): the timer flags plus the chip ID
 * bits, or 0xFF when no chip is fitted (nosb.flag).
 */
BYTE VddOplReadStatus(_In_ PCOPL_STATE state);

/* 1 while the chip is an OPL3 with NEW set: 18 channels, stereo routing live. */
INT  VddOplIsNewMode(_In_ PCOPL_STATE state);

/* Key every sounding voice off, both arrays and the rhythm drums, each through its
 * own release (F-number kept). For the host's "program ended" path.
 */
VOID VddOplAllNotesOff(_Inout_ POPL_STATE state);

/* Operator index for (channel, isCarrier): isCarrier=0 modulator, 1 carrier. The OPL's
 * operator-to-register mapping is famously non-contiguous. Channels 9-17 are the
 * OPL3's array 1 and map to operators 18-35 the same way.
 */
INT  VddOplOperatorIndex(_In_ INT channel, _In_ INT isCarrier);

/* Internal, shared by vdd_opl.c and vdd_opl_synth.c: 1 if channel `channel` leads a
 * live 4-operator pair (OPL3, NEW set, its 0x104 bit set), 2 if it is the pair's
 * second channel, 0 for an ordinary two-operator channel.
 */
INT  OplFourOperatorRole(_In_ PCOPL_STATE state, _In_ INT channel);
VOID VddOplRender(_Inout_ POPL_STATE state, _Out_writes_(frames) INT16 *output, _In_ UINT32 frames);
VOID VddOplRenderStereo(
    _Inout_ POPL_STATE state,
    _Out_writes_(OPL_STEREO_CHANNELS * frames) INT16 *output,
    _In_ UINT32 frames);

static inline NTVDD_DEVICE VddOplDevice(_In_ POPL_STATE state)
{ NTVDD_DEVICE device;
device.Name = "opl2";
device.Initialize = VddOplInitialize;
device.Reset = VddOplReset;
  device.Shutdown = 0;
  device.Context = state;
  return device; }

#endif /* NTVDMEX_VDD_OPL_H */
