/*
 * vdd_opl.h -- the AdLib / OPL2 (Yamaha YM3812) VDD.  (sound epic, GH #21)
 *
 * Ports 0x388 (address latch + status read) and 0x389 (data). This replaces the
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
 */
#ifndef NTVDMEX_VDD_OPL_H
#define NTVDMEX_VDD_OPL_H

#include "vdd_bus.h"

#define OPL_NUM_CH   9         /* OPL2: 9 two-operator channels                  */
#define OPL_NUM_OP  18

#define OPL_T1_US   80         /* timer 1 resolution, microseconds               */
#define OPL_T2_US  320         /* timer 2 resolution                             */
#define OPL_DEFAULT_HZ       44100u   /* render rate                             */
#define OPL_DEFAULT_FRAME_US 16667u   /* ~60 Hz bus frame tick                   */

/* status register bits (read from port 0x388) */
#define OPL_ST_IRQ  0x80
#define OPL_ST_T1   0x40
#define OPL_ST_T2   0x20

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
    uint8_t wave;                       /* 0xE0-0xF5: waveform select (OPL2: 0-3)  */
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
    uint8_t  reg[256];                  /* raw register file as written            */
    uint8_t  index;                     /* address latch (last write to 0x388)     */
    opl_op   op[OPL_NUM_OP];
    opl_ch   ch[OPL_NUM_CH];

    /* timers */
    uint8_t  t1_preset, t2_preset;
    uint8_t  t1_run, t2_run;
    uint8_t  t1_mask, t2_mask;
    uint16_t t1_count, t2_count;        /* current up-counters (preset..256)       */
    uint8_t  status;                    /* what a read of 0x388 returns            */
    uint32_t t1_frac_us, t2_frac_us;    /* microseconds not yet turned into steps  */

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
       waveforms from identical input can. NULL in normal runs and in the battery. */
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
    uint8_t  prof_wave_mask;            /* OR of 1<<wave over every operator       */
    uint32_t prof_am_ops;               /* bitmask: operators that ever set AM     */
    uint32_t prof_vib_ops;              /* bitmask: operators that ever set VIB    */
    uint32_t prof_keyon_am;             /* notes started with AM on either operator */
    uint32_t prof_keyon_vib;            /* notes started with VIB on either op      */
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

/* Direct register write (port 0x389 path), also used by tests and the SB's
   mirrored FM ports at 2x8/2x9. */
void vdd_opl_write_reg(opl_state *st, uint8_t reg, uint8_t val);

/* Operator index for (channel, which): which=0 modulator, 1 carrier. The OPL's
   operator-to-register mapping is famously non-contiguous. */
int  vdd_opl_op_index(int ch, int which);

/* Render `frames` mono samples at the chip's NATIVE 49716 Hz (vdd_opl_synth.c);
   the mixer resamples to the host rate. Rendering at the native rate keeps the
   phase arithmetic exact, which is what makes the pitch correct. */
#define OPL_NATIVE_HZ 49716u
void vdd_opl_render(opl_state *st, int16_t *out, uint32_t frames);

#endif /* NTVDMEX_VDD_OPL_H */
