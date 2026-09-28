/* vdd_gus.h -- the Gravis UltraSound (GF1), north star 2.
 *
 * The hardware reference is docs/ref/gus.md (from the UltraSound SDK v2.22); what this
 * model does and does not do is docs/inventory/gus.md. Section numbers below (§n) are
 * the reference's.
 *
 * The GUS is a synthesizer with its own memory, not a DAC: the guest uploads samples
 * into on-card DRAM (programmed I/O at 3X7, or DMA), then points up to 32 voices at
 * them. This model renders the voices at the GF1's own service rate -- one output sample
 * per pass over the active voices -- and the audio mixer resamples that to the output.
 */
#ifndef NTVDMEX_VDD_GUS_H
#define NTVDMEX_VDD_GUS_H
#include "vdd_bus.h"
#include "vdd_dma.h"

#define GUS_DEFAULT_BASE 0x240      /* 220h is the Sound Blaster's (ref §1)          */
#define GUS_DEFAULT_IRQ  11         /* user decision 2026-09-25; slave delivery s80  */
#define GUS_DEFAULT_DMA  3
#define GUS_DRAM_SIZE    (1024u * 1024u)   /* 20 address bits (ref §3)               */
#define GUS_VOICES       32

typedef struct gus_voice {
    uint8_t  ctrl;          /* 00h: 0 stopped 1 stop 2 16-bit 3 loop 4 bidi 5 IRQ en 6 dir 7 IRQ pend */
    uint16_t fc;            /* 01h: frequency control, 6.9 fixed point in bits 15-1  */
    uint32_t start, end;    /* 02h-05h, in 1/512-sample units (address << 9 | frac)  */
    uint32_t pos;           /* 0Ah/0Bh: current position, same units                 */
    uint8_t  ramp_rate;     /* 06h: bits 5-0 step, 7-6 update rate                   */
    uint8_t  ramp_start;    /* 07h: EEEEMMMM                                         */
    uint8_t  ramp_end;      /* 08h                                                   */
    uint16_t vol;           /* 09h: EEEE MMMMMMMM xxxx (the 12-bit volume in 15-4)   */
    uint8_t  pan;           /* 0Ch                                                   */
    uint8_t  vctrl;         /* 0Dh: 0 stopped 1 stop 2 rollover 3 loop 4 bidi 5 IRQ en 6 dir 7 IRQ pend */
    uint32_t ramp_div;      /* services since the last ramp step                     */
} gus_voice;

typedef struct gus_state {
    vdd_bus   *bus;
    dma_state *dma;
    uint16_t   base;
    uint8_t    irq, dma_ch;
    uint8_t   *dram;                 /* GUS_DRAM_SIZE bytes, owned by the host      */

    /* the indirect register file (ref §2) */
    uint8_t    page, sel;
    uint16_t   lo_latch;             /* 3X4 byte written ahead of 3X5 for a 16-bit reg */

    /* globals (ref §2.1) */
    uint8_t    reset;                /* 4Ch: bit 0 run, 1 DAC, 2 master IRQ         */
    uint8_t    active;               /* 14..32                                       */
    uint8_t    dma_ctrl;             /* 41h as written                               */
    uint8_t    dma_tc;               /* 41h bit 6 on read: TC pending                */
    uint16_t   dma_addr;             /* 42h: DRAM address bits 19-4                  */
    uint8_t    dma_waiting;          /* 41h go written, 8237 not ready yet           */
    uint32_t   dram_io;              /* 43h/44h                                      */
    uint8_t    timer_ctrl;           /* 45h                                          */
    uint8_t    t1_load, t2_load;     /* 46h/47h                                      */
    uint8_t    samp_freq, samp_ctrl, samp_tc;   /* 48h/49h: recording (answered, silent) */
    uint8_t    jtrim;                /* 4Bh                                          */

    /* board (ref §5, §9) */
    uint8_t    mix;                  /* 2X0                                          */
    uint8_t    latch_armed;          /* 2X0 was the last write: 2XB may be written   */
    uint8_t    irq_latch, dma_latch; /* 2XB                                          */
    uint8_t    regctl;               /* 2XF                                          */
    uint8_t    adlib_idx;            /* 2X8 write                                    */
    uint8_t    adlib_mask;           /* 2X9 bits 5/6                                 */
    uint8_t    t1_run, t2_run;
    uint8_t    t1_val, t2_val;
    uint8_t    t1_exp, t2_exp;       /* expired flags (2X8 read, 2X6 bits 2/3)       */
    uint32_t   t1_acc_ns, t2_acc_ns;
    uint8_t    midi_ctrl;            /* 3X0 write                                    */
    uint8_t    line_up;              /* the IRQ line is asserted (edge detection)    */

    gus_voice  v[GUS_VOICES];

    /* diagnostics */
    uint32_t   io_writes, io_reads, dram_pokes, dram_peeks, dma_uploads, dma_bytes;
    uint32_t   voice_starts, irqs_raised, fifo_reads, renders, samples_out;
    uint32_t   latch_locked_out;     /* 2XB writes refused by the lock-out           */
    uint32_t   out_nonzero;          /* output samples that were not silence         */
    uint32_t   out_peak;             /* largest |sample| produced                    */
} gus_state;

int  vdd_gus_init(vdd_bus *b, void *self);
void vdd_gus_reset(void *self);
static inline ntvdd vdd_gus_device(gus_state *st)
{ ntvdd d; d.name = "gus"; d.init = vdd_gus_init; d.reset = vdd_gus_reset;
  d.shutdown = 0; d.self = st; return d; }

/* The GF1's output rate for the current active-voice count (ref §4), in Hz. */
uint32_t vdd_gus_rate_hz(const gus_state *st);

/* Render `n` mono samples at vdd_gus_rate_hz(), advancing every voice, volume ramp and
   timer by that much GF1 time, and raising the interrupts that time produces. */
void vdd_gus_render(gus_state *st, int16_t *out, uint32_t n);
/* #189: the same, as panned interleaved L/R pairs (2*n samples). */
void vdd_gus_render_st(gus_state *st, int16_t *out, uint32_t n);

/* The linear gain (Q16) of a 12-bit GF1 volume (ref §7). Exposed for the test. */
uint32_t vdd_gus_vol_gain(uint16_t vol12);

#endif /* NTVDMEX_VDD_GUS_H */
