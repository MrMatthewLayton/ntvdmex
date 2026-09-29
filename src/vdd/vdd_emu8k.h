/* vdd_emu8k.h -- the E-mu EMU8000, the wavetable synthesizer on the Sound Blaster AWE32 (#233).
 *
 * The specification is Creative's *AWE32/EMU8000 Programmer's Guide*, revision 1.00, Dave
 * Rossum, E-mu/Creative 1994-1996 (the "AWE32 Developer Information Pack"; see
 * docs/ref/SOURCES.md). Section and page references below (§n, p.n) are that guide's. What
 * this model does and does not do, register by register, is docs/inventory/emu8k.md.
 * Clean-room: written from the guide's prose, never from another emulator's EMU8000.
 *
 * The chip in one paragraph. 32 channels, each a sample-playback oscillator reading 16-bit
 * words out of "sound memory" (a 1 MB General MIDI ROM at word address 000000h and, from
 * 200000h up, DRAM the guest uploads into), with an interpolator, a resonant low-pass filter,
 * a volume stage and a pan/effects-send stage (p.19). Each channel's SOUND GENERATOR holds
 * CURRENT values (pitch CPF, volume and cutoff CVCF, address CCCA) that slew towards TARGET
 * values (PTRX, VTFT). An ENVELOPE GENERATOR per channel -- two DAHDSR envelopes and two LFOs,
 * from the "initial" registers IP/IFATN/PEFE/FMMOD/TREMFRQ/FM2FRQ2 -- overwrites those
 * targets every cycle unless it has been turned off (DCYSUSV bit 7). There is no "voice
 * done": a channel plays for ever and must always loop (§5); a note ends when its volume
 * envelope reaches zero.
 *
 * Ports (§2). Three groups, relative to the AWE's base E (BLASTER's `E`, 620h on a legacy
 * card at A220 -- i.e. SB base + 400h):
 *      E+000h  Data0   doubleword (LS word here, MS word at E+002h)
 *      E+400h  Data1   word or doubleword (MS word at E+402h)
 *      E+402h  Data2   word -- the SAME port as Data1's MS word: which one it is depends
 *                      on the register selected (see emu8k_data1_is_dw in the .c)
 *      E+800h  Data3   word
 *      E+802h  Pointer word: bits 4-0 channel, bits 7-5 register number
 * The guide allows only word and doubleword transfers; a doubleword is the LS word to the
 * port and the MS word to the port two higher, in that order.
 *
 * Pure C, no <windows.h>, and NO C LIBRARY: the host links -nostdlib, so there is no
 * libm here either -- every exponential is a table built from one multiplication.
 */
#ifndef NTVDMEX_VDD_EMU8K_H
#define NTVDMEX_VDD_EMU8K_H
#include "vdd_bus.h"

#define EMU8K_DEFAULT_BASE  0x620          /* SB 220h + 400h (§2); BLASTER "E620"            */
#define EMU8K_VOICES        32
#define EMU8K_RATE_HZ       44100u         /* WC: 65536 counts per 1.486 s (p.13)            */
#define EMU8K_ADDR_MASK     0xFFFFFFu      /* sound memory addresses are 24 bits (p.9)       */
#define EMU8K_DRAM_BASE     0x200000u      /* ROM 000000h-1FFFFFh, DRAM from 200000h (§5)    */
#define EMU8K_DRAM_WORDS    0x40000u       /* the stock AWE32's 512 KB, in 16-bit words       */
#define EMU8K_ROM_WORDS_MAX 0x200000u      /* the space the ROM may occupy (§5)               */
/* The envelope engine's update period. The guide's time unit for every delay register is
   725 us (p.14-16); 32 sample periods at 44.1 kHz is 725.6 us. So the engine here runs
   once per 32 output samples and a delay count is simply a count of engine ticks. */
#define EMU8K_TICK          32u

/* One of the two DAHDSR envelopes (p.19: ENVELOPE 1 = modulation, ENVELOPE 2 = volume). */
typedef struct emu8k_env {
    uint8_t  phase;          /* EMU8K_ENV_*                                                  */
    uint32_t count;          /* ticks spent in the current timed phase (delay, hold)         */
    int32_t  amp;            /* attack level, Q16 linear (0..65536)                          */
    int32_t  att;            /* attenuation below peak after the attack, Q16 dB              */
} emu8k_env;
enum { EMU8K_ENV_OFF = 0, EMU8K_ENV_DELAY, EMU8K_ENV_ATTACK, EMU8K_ENV_HOLD,
       EMU8K_ENV_DECAY, EMU8K_ENV_RELEASE, EMU8K_ENV_DONE };

typedef struct emu8k_voice {
    /* ---- the register file, as the guest sees it (p.6-7) ---- */
    uint32_t cpf;            /* Data0 r0: current pitch (31-16), fractional address (15-0) */
    uint32_t ptrx;           /* Data0 r1: pitch target, reverb send, aux byte              */
    uint32_t cvcf;           /* Data0 r2: current volume (31-16), current cutoff (15-0)    */
    uint32_t vtft;           /* Data0 r3: volume target, cutoff target                     */
    uint32_t d0r4, d0r5;     /* Data0 r4/r5: not in the guide's map -- stored, read back   */
    uint32_t psst;           /* Data0 r6: pan (31-24), loop start (23-0)                   */
    uint32_t csl;            /* Data0 r7: chorus send (31-24), loop end (23-0)             */
    uint32_t ccca;           /* Data1 r0: Q (31-28), DMA/WR/RIGHT (26-24), address (23-0)  */
    uint16_t envvol, dcysusv, envval, dcysus;          /* Data1 r4-r7                      */
    uint16_t atkhldv, lfo1val, atkhld, lfo2val;        /* Data2 r4-r7                      */
    uint16_t ip, ifatn, pefe, fmmod, tremfrq, fm2frq2; /* Data3 r0-r5                      */
    uint16_t d3r6, d3r7;     /* Data3 r6/r7: not in the map -- stored, read back           */

    /* ---- the engine's own state ---- */
    emu8k_env venv, menv;    /* volume (ENV2) and modulation (ENV1) envelopes              */
    uint32_t lfo1_ph, lfo2_ph;         /* triangle phase, a full cycle = 2^32              */
    uint32_t lfo1_wait, lfo2_wait;     /* ticks of LFO delay still to run                  */
    int32_t  cv_acc;         /* current volume as CV << 14: slews to VT within a tick      */
    int32_t  cv_step;
    int32_t  gl, gr;         /* pan gains, Q8 (0..256), latched at each tick               */
    /* the low-pass filter: direct form I, Q28 coefficients, cached for (cutoff, Q) */
    uint16_t f_cf; uint8_t f_q, f_valid, f_bypass;
    int32_t  b0, b1, b2, a1, a2;
    int32_t  x1, x2, y1, y2;
} emu8k_voice;

/* Optional host clock for WC, in microseconds (monotonic). NULL: WC counts rendered samples. */
typedef uint64_t (*emu8k_clock_fn)(void *ctx);

typedef struct emu8k_state {
    vdd_bus  *bus;
    uint16_t  base;                  /* E: Data0 at E, Data1/2 at E+400h, Data3/Ptr at E+800h */
    uint16_t *dram;                  /* dram_words 16-bit words, owned by the host          */
    uint32_t  dram_words;            /* 0 = none fitted                                     */
    const uint16_t *rom;             /* the GM ROM image, rom_words long; NULL = reads zero */
    uint32_t  rom_words;
    emu8k_clock_fn clock; void *clock_ctx;

    uint16_t  ptr;                   /* the Pointer register as written                     */
    uint8_t   blo[3];                /* byte-access latches for the three port groups       */
    uint16_t  hwcf1, hwcf2, hwcf3;   /* Data1 r1 ch 29/30/31                                */
    uint32_t  hwcf4, hwcf5, hwcf6;   /* Data1 r1 ch 9/10/13                                 */
    uint16_t  init[4][32];           /* INIT1-4 (Data1/Data2 r2/r3): the effects programs   */
    uint32_t  d1r1[32];              /* Data1 r1, channels the map does not name            */
    uint16_t  d2r1[32];              /* Data2 r1, likewise                                  */

    /* the four sound-memory DMA streams (§5): 0 left read, 1 right read, 2 left write,
       3 right write -- the same code CCCA bits 25-24 give a channel allocated to one */
    uint32_t  sma[4];                /* SMALR, SMARR, SMALW, SMARW: bits 23-0               */
    uint16_t  sm_rd[2];              /* SMLD/SMRD read registers (the prefetched word)      */
    uint16_t  sm_wr[2];              /* SMLD/SMRD write registers                           */
    uint8_t   sm_empty[2];           /* SMALR/SMARR bit 31                                  */
    uint8_t   sm_full[2];            /* SMALW/SMARW bit 31                                  */

    uint32_t  wc;                    /* sample counter (render-driven)                       */
    uint32_t  tick_pos;              /* 0..EMU8K_TICK-1: where in the engine period we are  */
    emu8k_voice v[EMU8K_VOICES];

    /* diagnostics */
    uint32_t  io_writes, io_reads, byte_io;
    uint32_t  sm_words_written, sm_words_read, sm_rom_writes, sm_held;
    uint32_t  notes_started, releases, renders, samples_out, out_nonzero, out_peak;
} emu8k_state;

int  vdd_emu8k_init(vdd_bus *b, void *self);
void vdd_emu8k_reset(void *self);
static inline ntvdd vdd_emu8k_device(emu8k_state *st)
{ ntvdd d; d.name = "emu8k"; d.init = vdd_emu8k_init; d.reset = vdd_emu8k_reset;
  d.shutdown = 0; d.self = st; return d; }

/* The chip's output rate: fixed, 44.1 kHz (the WC period, p.13). */
static inline uint32_t vdd_emu8k_rate_hz(const emu8k_state *st) { (void)st; return EMU8K_RATE_HZ; }

/* Render `n` stereo frames (2*n interleaved L/R samples) at vdd_emu8k_rate_hz(), advancing
   every channel, envelope, LFO and the sample counter by that much chip time. */
void vdd_emu8k_render_st(emu8k_state *st, int16_t *out, uint32_t n);

/* Exposed for the test: the attack time (us) of ATKHLDV/ATKHLD bits 6-0, and the decay /
   release time per dB (us) of DCYSUSV/DCYSUS bits 6-0. 0 = never / no decay. */
uint32_t vdd_emu8k_attack_us(uint8_t code);
uint32_t vdd_emu8k_decay_us_per_db(uint8_t code);

#endif /* NTVDMEX_VDD_EMU8K_H */
