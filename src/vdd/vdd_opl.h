/*
 * vdd_opl.h -- the AdLib / OPL2 (Yamaha YM3812) and OPL3 (YMF262) VDD.
 *                                                   (sound epic, GH #21; OPL3 #232)
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
 *   T1 (register 0x02) counts 80us steps, T2 (register 0x03) counts 320us steps.
 * Each counts UP from its preset to 256, overflows, sets its status flag, and
 * reloads. The canonical detect writes 0xFF to T1, starts it, delays ~80us, and
 * expects status 0xC0 (IRQ + T1 expired); anything else means "no AdLib".
 *
 * Time is injected, not read: vdd_opl_add_us() advances the timers, so the device
 * stays pure C with no clock of its own and the whole thing is exercised off-VM by
 * tools/dostest/opl_test.c. The host pumps it from real elapsed time; the mixer
 * pumps it from the sample clock, which is what keeps music in tempo.
 *
 * FM synthesis lives in vdd_opl_synth.c behind vdd_opl_render(); this file owns
 * the programmer-visible device.
 *
 * ── TWO CHIPS, ONE MODEL (GH #232). `opl_state.opl3` says which is fitted:
 *   0  YM3812 (OPL2) -- an AdLib. Ports 0x388/0x389 only; 0x38A/0x38B are not
 *      decoded (writes vanish, reads float 0xFF), and status bits 1-2 read 1.
 *   1  YMF262 (OPL3) -- an SB16/AWE32. A SECOND REGISTER ARRAY at 0x100-0x1FF,
 *      reached by writing the address to 0x38A instead of 0x388 (there is ONE
 *      9-bit address latch; A1 on the address write is its top bit, and the data
 *      port writes wherever it points). Status bits 1-2 read 0 -- which is the
 *      whole of how software tells the two chips apart (see vdd_opl_read_status).
 *   The OPL3 powers up OPL2-COMPATIBLE: until register 0x105 bit 0 (NEW) is set,
 *   array 1 latches but does nothing, waveforms are 2 bits, and output is mono.
 *   With NEW set it is 18 channels, pairable into 4-operator voices (0x104),
 *   8 waveforms, and each channel is routed left/right by C0-C8 bits 4/5.
 *   The chip type survives vdd_opl_reset(): it is a fact about the CARD, set by
 *   the host from the `Opl` setting, not guest-visible state.
 */
#ifndef NTVDMEX_VDD_OPL_H
#define NTVDMEX_VDD_OPL_H

#include "vdd_bus.h"

#define OPL_NUM_CH   9         /* OPL2 (and OPL3 array 0): 9 two-operator channels */
#define OPL_NUM_OP  18
#define OPL3_NUM_CH 18         /* OPL3: two arrays of 9                          */
#define OPL3_NUM_OP 36
#define OPL3_NUM_REG 0x200     /* array 0 at 0x000-0x0FF, array 1 at 0x100-0x1FF */

/* OPL3-only registers, in array 1 (so 9-bit numbers). */
#define OPL3_REG_4OP  0x104    /* bits 0-5: pair ch 0+3, 1+4, 2+5, 9+12, 10+13, 11+14 */
#define OPL3_REG_NEW  0x105    /* bit 0: NEW -- the OPL3 extensions are live       */

/* C0-C8 (and 0x1C0-0x1C8) output-routing bits, OPL3 with NEW set. The chip has
   FOUR outputs, A-D. A Sound Blaster 16 / AWE32 wires A to the left DAC and B to
   the right; C and D are brought out of the chip and routed NOWHERE on those cards,
   so a voice sent only to C/D is silent there -- and is here, deliberately. */
#define OPL_C0_CHA  0x10       /* output A -> left                               */
#define OPL_C0_CHB  0x20       /* output B -> right                              */
#define OPL_C0_CHC  0x40       /* output C -- not connected on an SB16           */
#define OPL_C0_CHD  0x80       /* output D -- not connected on an SB16           */

#define OPL_T1_US   80         /* timer 1 resolution, microseconds               */
#define OPL_T2_US  320         /* timer 2 resolution                             */
#define OPL_DEFAULT_HZ       44100u   /* render rate                             */
#define OPL_DEFAULT_FRAME_US 16667u   /* ~60 Hz bus frame tick                   */

/* status register bits (read from port 0x388) */
#define OPL_ST_IRQ  0x80
#define OPL_ST_T1   0x40
#define OPL_ST_T2   0x20
/* Status bits 1 and 2 are the chip's ID. A YM3812 reads them as 1, a YMF262 as 0,
   and "(status & 0x06) == 0 means OPL3" is the test drivers use (Creative's SB16
   documentation, and every OPL3 detect after it). The timer detect masks with
   0xE0, so an AdLib detect passes on both chips. */
#define OPL_ST_OPL2_ID 0x06

/* register 0xBD: LFO depths, and rhythm mode. The two depth bits scale the chip's
   single shared tremolo and vibrato oscillators; the AM/VIB bits in 0x20-0x35 say
   which operators listen to them. */
#define OPL_BD_DAM  0x80        /* tremolo depth: 0 = 1.2 dB, 1 = 4.9 dB          */
#define OPL_BD_DVB  0x40        /* vibrato depth: 1 = double                      */
#define OPL_BD_RHY  0x20        /* rhythm mode enable                             */

/* register 0x04 (timer control) bits */
#define OPL_TC_T1_START 0x01
#define OPL_TC_T2_START 0x02
#define OPL_TC_T2_MASK  0x20
#define OPL_TC_T1_MASK  0x40
#define OPL_TC_IRQ_RST  0x80

/* envelope generator phase */
enum { OPL_EG_OFF = 0, OPL_EG_ATTACK, OPL_EG_DECAY, OPL_EG_SUSTAIN, OPL_EG_RELEASE };

/* The envelope counts ATTENUATION, so 0 is full volume and OPL_ENV_MAX is silence
   -- the opposite of the intuitive reading, and the direction that matters when
   initialising it. Carried in fixed point because the slowest rate advances only
   one unit per 4096 samples; 511 << 20 still fits an int32. See vdd_opl_synth.c. */
#define OPL_ENV_MAX    511                                  /* fully attenuated   */
#define OPL_ENV_SHIFT   20                                  /* fractional bits    */
#define OPL_ENV_FULL   ((int32_t)OPL_ENV_MAX << OPL_ENV_SHIFT)

typedef struct opl_op {
    /* programmed by the register file */
    uint8_t am, vib, egt, ksr, mult;    /* 0x20-0x35                              */
    uint8_t ksl, tl;                    /* 0x40-0x55: key-scale level, total level */
    uint8_t ar, dr;                     /* 0x60-0x75: attack, decay rates          */
    uint8_t sl, rr;                     /* 0x80-0x95: sustain level, release rate  */
    /* 0xE0-0xF5: waveform select, the 3 bits AS WRITTEN. How many of them count is
       decided at render time (opl_eff_wave in vdd_opl_synth.c), because it depends
       on registers written later -- NEW, and on an OPL2 the WSE bit in 0x01. */
    uint8_t wave;
    /* synthesis state */
    uint32_t phase;                     /* phase accumulator, 10.10 fixed point    */
    int32_t  env;                       /* attenuation, OPL_ENV_SHIFT fixed point  */
    uint8_t  eg_state;
    int32_t  out1, out2;                /* last two outputs, for feedback          */
} opl_op;

typedef struct opl_ch {
    uint16_t fnum;                      /* 10-bit frequency number                 */
    uint8_t  block;                     /* 3-bit octave                            */
    uint8_t  keyon;
    uint8_t  fb;                        /* feedback level                          */
    uint8_t  cnt;                       /* 0 = FM (op1 modulates op2), 1 = additive */
} opl_ch;

typedef struct opl_state {
    vdd_bus *bus;
    /* Raw register file as written, both arrays: reg[0x0B0] is array 0's 0xB0,
       reg[0x1B0] array 1's. On an OPL2 the top half is never written. */
    uint8_t  reg[OPL3_NUM_REG];
    uint16_t index;                     /* 9-bit address latch (0x388, or 0x38A
                                           for array 1 on an OPL3)                */
    uint8_t  opl3;                      /* chip fitted: 0 = YM3812, 1 = YMF262.
                                           Set by the host; survives reset.       */
    opl_op   op[OPL3_NUM_OP];           /* 0-17 array 0, 18-35 array 1            */
    opl_ch   ch[OPL3_NUM_CH];           /* 0-8 array 0, 9-17 array 1              */

    /* timers */
    uint8_t  t1_preset, t2_preset;
    uint8_t  t1_run, t2_run;
    uint8_t  t1_mask, t2_mask;
    uint16_t t1_count, t2_count;        /* current up-counters (preset..256)       */
    uint8_t  status;                    /* timer/IRQ flags (bits 5-7). A read of
                                           0x388 is this plus the chip ID bits --
                                           use vdd_opl_read_status(), not this.   */
    uint32_t t1_frac_us, t2_frac_us;    /* microseconds not yet turned into steps  */

    /* Free-running sample counter driving BOTH low-frequency oscillators. They are
       properties of the chip, not of a note: one tremolo and one vibrato shared by
       all 18 operators, never restarted by key-on. Two notes struck a beat apart
       are therefore at different points in the sweep, which is most of what makes
       the effect sound like an instrument rather than a wobble. */
    uint32_t lfo_count;

    uint32_t sample_hz;                 /* render rate (0 => OPL_DEFAULT_HZ)       */
    uint32_t frame_us;                  /* microseconds per bus frame tick         */
    uint8_t  ext_clock;                 /* 1 = host drives time via vdd_opl_add_us,
                                           so the coarse frame tick must NOT also
                                           advance the timers (that would double-
                                           count and run music at 2x tempo)       */

    /* Register-write trace hook (dev only). Set by the host to capture the exact
       stream a game sends, so it can be replayed offline through BOTH this synth
       and a reference core (Nuked OPL3) and the outputs diffed. Counting register
       writes cannot answer "why does this instrument sound wrong"; comparing
       waveforms from identical input can. NULL in normal runs and in the battery.
       ARRAY 0 ONLY: the hook's register is 8 bits wide and the trace format has
       no bank column, so an OPL3 program's array-1 writes are not captured. */
    void (*trace)(uint8_t reg, uint8_t val);

    /* ---- WHAT THE GAME ACTUALLY ASKS FOR (GH #21) ------------------------- *
     * The synth has three declared gaps -- tremolo/vibrato depth (0xBD), rhythm
     * mode (0xBD), and envelope rates anchored only to within ~2x at the extremes
     * -- and "the music sounds a bit flat" could be any of them. Rather than rank
     * them by ear, record what the guest's music driver REALLY writes: a feature
     * the game never touches cannot be the cause, so this turns three plausible
     * stories into one by elimination. Reported in the STAGE2 block.            */
    uint32_t prof_writes;               /* register writes seen                    */
    uint32_t prof_keyons;               /* key-on edges (notes started)            */
    uint32_t prof_bd_writes;            /* writes to 0xBD specifically             */
    uint8_t  prof_bd_or;                /* OR of every value written to 0xBD       */
    uint8_t  prof_wse;                  /* reg 0x01 bit 5 (waveform select enable) */
    uint8_t  prof_wave_mask;            /* OR of 1<<wave (3 bits) over every op    */
    /* Bit n = operator slot n of EITHER array: array 1's 18 slots fold onto the
       same 18 bits, which keeps the STAGE2 line one 32-bit number. */
    uint32_t prof_am_ops;               /* bitmask: operators that ever set AM     */
    uint32_t prof_vib_ops;              /* bitmask: operators that ever set VIB    */
    uint32_t prof_keyon_am;             /* notes started with AM on either operator */
    uint32_t prof_keyon_vib;            /* notes started with VIB on either op      */
    /* Percussion hits by voice: hi-hat, cymbal, tom-tom, snare, bass drum. EDGES,
       not an OR over the run -- a counter that only says a feature was TOUCHED
       once produced a confident wrong answer about this very register. Three of
       these five voices are not synthesised yet, so this is also the loud-failure
       report for them: a run says how much percussion it could not play. */
    uint32_t prof_rhythm_hits[5];
} opl_state;

/* Build the device descriptor to hand to vdd_bus_add(). */
int  vdd_opl_init(vdd_bus *b, void *self);
void vdd_opl_reset(void *self);
static inline ntvdd vdd_opl_device(opl_state *st)
{ ntvdd d; d.name = "opl2"; d.init = vdd_opl_init; d.reset = vdd_opl_reset;
  d.shutdown = 0; d.self = st; return d; }

/* Advance the timers by `us` microseconds, raising status flags on overflow.
   Exposed rather than driven by a clock inside the device so tests can step it
   exactly and the mixer can drive it from the sample clock. */
void vdd_opl_add_us(opl_state *st, uint32_t us);

/* Direct register write, `reg` 9 bits (0x1xx = OPL3 array 1; ignored on an
   OPL2). Used by the data port, tests, and the host. */
void vdd_opl_write_reg(opl_state *st, uint16_t reg, uint8_t val);

/* The port-level interface, for anything that decodes the chip's A0/A1 lines
   itself -- the Sound Blaster's mirrors at 2x0-2x3 and 2x8/2x9 (vdd_sb.c).
   `array` 1 is A1 high (0x38A / 2x2): on an OPL2 it is not decoded and the write
   is dropped. The data write goes to whatever the latch holds. */
void    vdd_opl_write_addr(opl_state *st, int array, uint8_t val);
void    vdd_opl_write_data(opl_state *st, uint8_t val);
/* A status read (0x388; on an OPL3 also 0x38A): the timer flags plus the chip ID
   bits, or 0xFF when no chip is fitted (nosb.flag). */
uint8_t vdd_opl_read_status(const opl_state *st);

/* 1 while the chip is an OPL3 with NEW set: 18 channels, stereo routing live. */
int  vdd_opl_new_mode(const opl_state *st);

/* Key every sounding voice off, both arrays and the rhythm drums, each through its
   own release (F-number kept). For the host's "program ended" path. */
void vdd_opl_all_notes_off(opl_state *st);

/* Operator index for (channel, which): which=0 modulator, 1 carrier. The OPL's
   operator-to-register mapping is famously non-contiguous. Channels 9-17 are the
   OPL3's array 1 and map to operators 18-35 the same way. */
int  vdd_opl_op_index(int ch, int which);

/* Internal, shared by vdd_opl.c and vdd_opl_synth.c: 1 if channel `c` leads a
   live 4-operator pair (OPL3, NEW set, its 0x104 bit set), 2 if it is the pair's
   second channel, 0 for an ordinary two-operator channel. */
int  opl_4op_role(const opl_state *st, int c);

/* Render `frames` samples at the chip's NATIVE 49716 Hz (vdd_opl_synth.c); the
   mixer resamples to the host rate. Rendering at the native rate keeps the phase
   arithmetic exact, which is what makes the pitch correct.
     vdd_opl_render     mono. OPL2 (or OPL3 with NEW clear): exactly the chip's
                        one output. OPL3 with NEW set: (left + right) / 2.
     vdd_opl_render_st  interleaved L/R, 2*frames samples. Without NEW both sides
                        carry the mono signal, identical to vdd_opl_render. */
#define OPL_NATIVE_HZ 49716u
void vdd_opl_render(opl_state *st, int16_t *out, uint32_t frames);
void vdd_opl_render_st(opl_state *st, int16_t *out, uint32_t frames);

/* nosb.flag: when set, the status port floats (0xFF) so an AdLib detect fails. */
extern int g_opl_absent;

#endif /* NTVDMEX_VDD_OPL_H */
