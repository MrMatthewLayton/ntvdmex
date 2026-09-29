/* vdd_emu8k.c -- the EMU8000 (Sound Blaster AWE32 wavetable). See vdd_emu8k.h and
 * docs/inventory/emu8k.md.
 *
 * Built from the AWE32/EMU8000 Programmer's Guide rev 1.00 (§n, p.n below), in our own words:
 * a device is in because it is in the period hardware contract, not because a guest asked.
 * Where the guide gives only the ENDS of a scale (attack 11.88 s .. 6 ms, decay 470 ms/dB ..
 * 240 us/dB) the curve between them is OURS -- logarithmic interpolation -- and says so.
 *
 * No C library (the host links -nostdlib): every exponential below comes from one 257-entry
 * table of 2^(i/256), built once by repeated multiplication.
 */
#include "vdd_emu8k.h"

/* ---- fixed-point exponentials --------------------------------------------------------- */

static uint32_t s_exp2[257];         /* 2^(i/256), Q16: 65536 .. 131072                    */
static uint32_t s_atk_inc[128];      /* attack: Q16 of full level gained per engine tick   */
static int32_t  s_dcy_db[128];       /* decay/release: Q16 dB per engine tick              */
static int      s_tables_built;

/* 2^(x / 65536), Q16. Good to about 1 part in 10^5 -- far below anything audible. */
static uint32_t emu_exp2(int32_t x)
{
    int32_t  i = x >> 16;                                  /* arithmetic: floor           */
    uint32_t f = (uint32_t)x & 0xFFFFu, k = f >> 8, m;
    m = s_exp2[k] + (uint32_t)(((uint64_t)(s_exp2[k + 1] - s_exp2[k]) * (f & 0xFF)) >> 8);
    if (i >= 15) return 0xFFFFFFFFu;
    if (i >= 0)  return m << i;
    if (i <= -32) return 0;
    return m >> -i;
}

/* log2(v / 65536), Q16, for v > 0: the inverse of the above, by search of the same table. */
static int32_t emu_log2(uint32_t v)
{
    int32_t n = 0; uint32_t lo = 0, hi = 256, k;
    if (!v) return -(32 << 16);
    while (v >= 131072u) { v >>= 1; ++n; }
    while (v <  65536u)  { v <<= 1; --n; }
    while (hi - lo > 1) { k = (lo + hi) >> 1; if (s_exp2[k] <= v) lo = k; else hi = k; }
    k = lo;
    return n * 65536 + (int32_t)(k << 8)
         + (int32_t)(((uint64_t)(v - s_exp2[k]) << 8) / (s_exp2[k + 1] - s_exp2[k]));
}

/* Q16 dB of attenuation -> Q16 linear gain. 20·log10(2) = 6.0206 dB per octave, so
   octaves = dB × 0.166096 = dB × 10885 / 65536. */
static uint32_t emu_db_gain(int32_t att_q16)
{
    if (att_q16 <= 0) return 65536u;
    if (att_q16 >= (int32_t)(100u << 16)) return 0;
    return emu_exp2(-(int32_t)(((int64_t)att_q16 * 10885) >> 16));
}

#define TICK_US_X10 7256u                                  /* 32 / 44100 s = 725.6 us      */
/* The slewing current volume is held as CV << 14: CV is 16 bits, so << 16 would reach the
   sign bit of an int32 at full volume. */
#define EMU8K_CV_SH 14

uint32_t vdd_emu8k_attack_us(uint8_t code)
{
    /* p.16: 0x01 = 11.88 s, 0x7F = 6 ms, 0 = never. log2(6 ms / 11.88 s) = -10.9513. */
    code &= 0x7F;
    if (!code) return 0;
    return (uint32_t)(((uint64_t)11880000u
                     * emu_exp2(-(int32_t)((717709ll * (code - 1)) / 126))) >> 16);
}

uint32_t vdd_emu8k_decay_us_per_db(uint8_t code)
{
    /* p.15: 0x01 = 470 ms/dB, 0x7F = 240 us/dB, 0 = no decay. log2(240 us / 470 ms)
       = -10.9354. The guide's own example -- 0x5C "for a release rate of 100 msec" (§7) --
       lands at 1.97 ms/dB here: about 50 dB of fall in 100 ms. */
    code &= 0x7F;
    if (!code) return 0;
    return (uint32_t)(((uint64_t)470000u
                     * emu_exp2(-(int32_t)((716669ll * (code - 1)) / 126))) >> 16);
}

static void emu_build_tables(void)
{
    /* 2^(1/256) = 1.0027112750502025. Plain double arithmetic -- no libm needed. */
    double v = 1.0;
    unsigned i;
    if (s_tables_built) return;
    for (i = 0; i <= 256; ++i) { s_exp2[i] = (uint32_t)(v * 65536.0 + 0.5); v *= 1.0027112750502025; }
    s_exp2[256] = 131072u;
    s_atk_inc[0] = 0; s_dcy_db[0] = 0;
    for (i = 1; i < 128; ++i) {
        uint32_t us = vdd_emu8k_attack_us((uint8_t)i);
        uint32_t dp = vdd_emu8k_decay_us_per_db((uint8_t)i);
        s_atk_inc[i] = (uint32_t)(((uint64_t)65536u * TICK_US_X10) / ((uint64_t)us * 10u));
        if (!s_atk_inc[i]) s_atk_inc[i] = 1;
        s_dcy_db[i]  = (int32_t)(((uint64_t)65536u * TICK_US_X10) / ((uint64_t)dp * 10u));
        if (!s_dcy_db[i]) s_dcy_db[i] = 1;
    }
    s_tables_built = 1;
}

/* ---- sound memory (§5) ------------------------------------------------------------- */

/* A word of sound memory. The ROM space reads the ROM image if the host fitted one and
   zero otherwise (we have no GM ROM -- see the inventory); DRAM above what is fitted
   reads zero. Nothing aliases. */
static int16_t emu_mem(const emu8k_state *st, uint32_t a)
{
    a &= EMU8K_ADDR_MASK;
    if (a < EMU8K_DRAM_BASE) return (st->rom && a < st->rom_words) ? (int16_t)st->rom[a] : 0;
    a -= EMU8K_DRAM_BASE;
    return (st->dram && a < st->dram_words) ? (int16_t)st->dram[a] : 0;
}
static void emu_mem_write(emu8k_state *st, uint32_t a, uint16_t w)
{
    a &= EMU8K_ADDR_MASK;
    if (a < EMU8K_DRAM_BASE) { st->sm_rom_writes++; return; }   /* ROM: the write goes nowhere */
    a -= EMU8K_DRAM_BASE;
    if (st->dram && a < st->dram_words) st->dram[a] = w;
}

/* Is any channel allocated to DMA stream `s` (0 LR, 1 RR, 2 LW, 3 RW)? CCCA bit 26 = DMA,
   bit 25 = write, bit 24 = right (p.9-10) -- so bits 25-24 ARE the stream number. */
static int emu_stream_alloc(const emu8k_state *st, unsigned s)
{
    unsigned i;
    for (i = 0; i < EMU8K_VOICES; ++i)
        if ((st->v[i].ccca & 0x04000000u) && ((st->v[i].ccca >> 24) & 3) == s) return 1;
    return 0;
}

/* The transfers are instantaneous here, so FULL and EMPTY are set only while a stream has
   NO channel allocated to carry it (§5: the transfer then waits "until ... aborted because
   no channels are currently programmed"). Allocating one completes the waiting transfer. */
static void emu_stream_service(emu8k_state *st)
{
    unsigned lr;
    for (lr = 0; lr < 2; ++lr) {
        if (st->sm_full[lr] && emu_stream_alloc(st, 2 + lr)) {
            emu_mem_write(st, st->sma[2 + lr], st->sm_wr[lr]);
            st->sma[2 + lr] = (st->sma[2 + lr] + 1) & EMU8K_ADDR_MASK;
            st->sm_full[lr] = 0; st->sm_words_written++;
        }
        if (st->sm_empty[lr] && emu_stream_alloc(st, lr)) {
            st->sm_rd[lr] = (uint16_t)emu_mem(st, st->sma[lr]);
            st->sma[lr] = (st->sma[lr] + 1) & EMU8K_ADDR_MASK;
            st->sm_empty[lr] = 0; st->sm_words_read++;
        }
    }
}

/* SMLD/SMRD write (p.12): the word goes to SMAxW, which then increments. */
static void emu_sm_write(emu8k_state *st, unsigned lr, uint16_t w)
{
    st->sm_wr[lr] = w;
    if (emu_stream_alloc(st, 2 + lr)) {
        emu_mem_write(st, st->sma[2 + lr], w);
        st->sma[2 + lr] = (st->sma[2 + lr] + 1) & EMU8K_ADDR_MASK;
        st->sm_full[lr] = 0; st->sm_words_written++;
    } else { st->sm_full[lr] = 1; st->sm_held++; }
}

/* SMLD/SMRD read (p.12): hand over what the read register holds, then PREFETCH the word at
   SMAxR into it. So the first read after setting SMAxR returns STALE data -- the guide
   tells a driver to read and discard one word first (§5), and this is why. */
static uint16_t emu_sm_read(emu8k_state *st, unsigned lr)
{
    uint16_t r = st->sm_rd[lr];
    if (emu_stream_alloc(st, lr)) {
        st->sm_rd[lr] = (uint16_t)emu_mem(st, st->sma[lr]);
        st->sma[lr] = (st->sma[lr] + 1) & EMU8K_ADDR_MASK;
        st->sm_empty[lr] = 0; st->sm_words_read++;
    } else st->sm_empty[lr] = 1;
    return r;
}

static uint16_t emu_wc(const emu8k_state *st)
{
    /* p.13: "continuously incrementing at the sample rate ... no mechanism to reset". */
    if (st->clock) return (uint16_t)((st->clock(st->clock_ctx) * 441u) / 10000u);
    return (uint16_t)st->wc;
}

/* ---- the envelope engine (p.14-18, the diagram on p.19) ---------------------------------- */

static void env_start(emu8k_env *e) { e->phase = EMU8K_ENV_DELAY; e->count = 0; e->amp = 0; e->att = 0; }

/* Release from wherever the envelope is. Mid-attack, the linear level becomes dB. */
static void env_release(emu8k_env *e)
{
    if (e->phase == EMU8K_ENV_OFF || e->phase == EMU8K_ENV_DONE) return;
    if (e->phase == EMU8K_ENV_DELAY) { e->phase = EMU8K_ENV_DONE; return; }
    if (e->phase == EMU8K_ENV_ATTACK) {
        if (e->amp <= 0) { e->phase = EMU8K_ENV_DONE; return; }
        /* dB = -20·log10(amp) = -6.0206·log2(amp) */
        e->att = (int32_t)(-((int64_t)emu_log2((uint32_t)e->amp) * 394566) >> 16);
        if (e->att < 0) e->att = 0;
    }
    e->phase = EMU8K_ENV_RELEASE;
}

/* One engine tick. `delay` = ENVVOL/ENVVAL, `ah` = ATKHLDV/ATKHLD, `ds` = DCYSUSV/DCYSUS.
   Returns the envelope's level, Q16 linear (0..65536). */
static uint32_t env_tick(emu8k_env *e, uint16_t delay, uint16_t ah, uint16_t ds)
{
    switch (e->phase) {
    case EMU8K_ENV_DELAY: {
        /* p.14: 8000h = no delay; below that, 725 us units -- one engine tick each. */
        uint32_t d = delay >= 0x8000 ? 0 : 0x8000u - delay;
        if (e->count < d) { e->count++; return 0; }
        e->phase = EMU8K_ENV_ATTACK; e->count = 0; e->amp = 0;
    }   /* fall through */
    case EMU8K_ENV_ATTACK:
        /* p.16: bits 6-0, 0 = never attack. The rise is linear in amplitude over the time. */
        e->amp += (int32_t)s_atk_inc[ah & 0x7F];
        if (e->amp < 65536) return (uint32_t)e->amp;
        e->amp = 65536; e->att = 0; e->phase = EMU8K_ENV_HOLD; e->count = 0;
        /* fall through */
    case EMU8K_ENV_HOLD: {
        /* p.16: bits 14-8 in 92 ms steps, 7Fh = no hold, 00h = 11.68 s. */
        uint32_t h = ((0x7Fu - ((ah >> 8) & 0x7F)) * 920000u) / TICK_US_X10;
        if (e->count < h) { e->count++; return 65536u; }
        e->phase = EMU8K_ENV_DECAY;
    }   /* fall through */
    case EMU8K_ENV_DECAY: {
        /* p.14-15: sustain bits 14-8 in 0.75 dB steps below peak (7Fh = 0 dB, 0 = silence);
           decay bits 6-0 as a time per dB, 0 = no decay. dB-linear: the fall is exponential. */
        uint32_t s = (ds >> 8) & 0x7F;
        int32_t sus = s ? (int32_t)((0x7Fu - s) * 49152u) : (int32_t)(100u << 16);  /* 0.75 dB = 49152 */
        if (e->att < sus) { e->att += s_dcy_db[ds & 0x7F]; if (e->att > sus) e->att = sus; }
        return emu_db_gain(e->att); }
    case EMU8K_ENV_RELEASE:
        e->att += s_dcy_db[ds & 0x7F];
        if (e->att >= (int32_t)(96u << 16)) { e->phase = EMU8K_ENV_DONE; return 0; }
        return emu_db_gain(e->att);
    default:
        return 0;
    }
}

/* A triangle LFO (p.18: FRQ in 0.042 Hz steps, FFh = 10.72 Hz). Starts at zero going up.
   Returns -65536..65536. */
static int32_t lfo_tick(uint32_t *ph, uint32_t *wait, uint8_t frq)
{
    uint32_t p;
    if (*wait) { (*wait)--; return 0; }
    p = *ph >> 16;
    /* phase step per tick = 2^32 × (frq × 10.72/255 Hz) × 725.6 us = frq × 131008 */
    *ph += (uint32_t)frq * 131008u;
    if (p < 16384u) return (int32_t)(p * 4u);
    if (p < 49152u) return 131072 - (int32_t)(p * 4u);
    return (int32_t)(p * 4u) - 262144;
}
static uint32_t lfo_delay_ticks(uint16_t r) { return r >= 0x8000 ? 0 : 0x8000u - r; }

/* ---- the low-pass filter (CCCA Q, IFATN cutoff: p.9, p.17) --------------------------- */

/* sin/cos by series, in double: only ever called when a channel's cutoff or Q CHANGES. */
static double emu_sin(double x) { double x2 = x * x;
    return x * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42 * (1 - x2 / 72 * (1 - x2 / 110))))); }
static double emu_cos(double x) { double x2 = x * x;
    return 1 - x2 / 2 * (1 - x2 / 12 * (1 - x2 / 30 * (1 - x2 / 56 * (1 - x2 / 90 * (1 - x2 / 132))))); }

/* The cutoff scale. p.17 puts IFATN's cutoff byte at "quarter semitones, 00h = 125 Hz,
   FFh = 8 kHz" -- but 255 quarter-semitones is 5.3 octaves and 125 Hz -> 8 kHz is 6. The
   END POINTS are what a program sets, so they win: six octaves across 0000h..FF00h of the
   16-bit cutoff, i.e. one octave = FF00h / 6 = 2A80h units. */
#define EMU8K_CUT_OCT 0x2A80

static void filt_setup(emu8k_voice *v, uint16_t cf, uint8_t q)
{
    double fc, w0, sn, cs, alpha, qlin, a0;
    if (v->f_valid && v->f_cf == cf && v->f_q == q) return;
    v->f_cf = cf; v->f_q = q; v->f_valid = 1;
    /* p.17: Q 0 and cutoff FFh = "the filter does not alter the signal". Exactly that. */
    v->f_bypass = (uint8_t)(q == 0 && cf >= 0xFF00);
    if (v->f_bypass) { v->x1 = v->x2 = v->y1 = v->y2 = 0; return; }
    fc = 125.0 * (double)emu_exp2((int32_t)(((uint32_t)cf << 16) / EMU8K_CUT_OCT)) / 65536.0;
    if (fc > 0.45 * EMU8K_RATE_HZ) fc = 0.45 * EMU8K_RATE_HZ;
    w0 = 2.0 * 3.14159265358979 * fc / EMU8K_RATE_HZ;
    sn = emu_sin(w0); cs = emu_cos(w0);
    /* p.9: Q 0 = no resonance, 15 = "about 24 dB". A resonant 2-pole's peak is ≈ its Q, so
       Q 0 is the flat Butterworth 0.707 and each step adds 1.6 dB above it (+3 dB for the
       0.707 itself): 15 -> 0.707 × 10^(27/20) ≈ 15.8 ≈ 24 dB. */
    qlin = 0.70710678 * (double)emu_exp2((int32_t)(((uint64_t)q * 117965u * 10885u) >> 16)) / 65536.0;
    alpha = sn / (2.0 * qlin);
    a0 = 1.0 + alpha;
    v->b0 = (int32_t)(((1.0 - cs) / 2.0) / a0 * 268435456.0);
    v->b1 = (int32_t)((1.0 - cs) / a0 * 268435456.0);
    v->b2 = v->b0;
    v->a1 = (int32_t)((-2.0 * cs) / a0 * 268435456.0);
    v->a2 = (int32_t)((1.0 - alpha) / a0 * 268435456.0);
}

static int32_t filt_run(emu8k_voice *v, int32_t x)
{
    int64_t acc;
    int32_t y;
    if (v->f_bypass) return x;
    acc = (int64_t)v->b0 * x + (int64_t)v->b1 * v->x1 + (int64_t)v->b2 * v->x2
        - (int64_t)v->a1 * v->y1 - (int64_t)v->a2 * v->y2;
    y = (int32_t)(acc >> 28);
    if (y > 262143) y = 262143; else if (y < -262144) y = -262144;   /* resonance headroom */
    v->x2 = v->x1; v->x1 = x; v->y2 = v->y1; v->y1 = y;
    return y;
}

/* The per-channel engine tick: the envelope generator (unless DCYSUSV bit 7 has turned it
   off) recomputes the TARGETS, then the sound generator takes them up. */
static void emu_voice_tick(emu8k_voice *v)
{
    int32_t cv, vt;
    uint32_t pt, ft;
    if (!(v->dcysusv & 0x80) && !(v->ccca & 0x04000000u)) {
        uint32_t vol = env_tick(&v->venv, v->envvol, v->atkhldv, v->dcysusv);
        int32_t  mod = (int32_t)env_tick(&v->menv, v->envval, v->atkhld, v->dcysus);
        int32_t  l1  = lfo_tick(&v->lfo1_ph, &v->lfo1_wait, (uint8_t)v->tremfrq);
        int32_t  l2  = lfo_tick(&v->lfo2_ph, &v->lfo2_wait, (uint8_t)v->fm2frq2);
        int32_t  oct, cut, att;
        uint32_t g;
        /* pitch: IP E000h = unity, 1000h per octave (p.16) -> Q16 octaves; then ENV1 ×
           PEFE hi (±1 oct), LFO1 × FMMOD hi (±1 oct), LFO2 × FM2FRQ2 hi (±1 oct) */
        oct  = ((int32_t)v->ip - 0xE000) * 16;
        oct += (int32_t)(((int64_t)mod * (int8_t)(v->pefe >> 8)) / 127);
        oct += (int32_t)(((int64_t)l1  * (int8_t)(v->fmmod >> 8)) / 127);
        oct += (int32_t)(((int64_t)l2  * (int8_t)(v->fm2frq2 >> 8)) / 127);
        pt = (uint32_t)(((uint64_t)emu_exp2(oct) * 0x4000u) >> 16); /* 4000h = unity (p.7) */
        if (oct >= (2 << 16) || pt > 0xFFFF) pt = 0xFFFF;
        /* cutoff: IFATN hi, ENV1 × PEFE lo (±6 oct), LFO1 × FMMOD lo (±3 oct) */
        cut  = (int32_t)(v->ifatn & 0xFF00);
        cut += (int32_t)(((int64_t)mod * (int8_t)v->pefe  * 6 * EMU8K_CUT_OCT / 127) >> 16);
        cut += (int32_t)(((int64_t)l1  * (int8_t)v->fmmod * 3 * EMU8K_CUT_OCT / 127) >> 16);
        ft = cut < 0 ? 0 : cut > 0xFFFF ? 0xFFFF : (uint32_t)cut;
        /* volume: ENV2 × IFATN lo (0.375 dB steps) × LFO1 tremolo (TREMFRQ hi, ±12 dB) */
        att  = (int32_t)(v->ifatn & 0xFF) * 24576;              /* 0.375 dB = 24576 Q16    */
        att -= (int32_t)(((int64_t)l1 * (int8_t)(v->tremfrq >> 8) * 12) / 127);
        g = (uint32_t)(((uint64_t)vol * emu_db_gain(att)) >> 16);
        if (att < 0) g = (uint32_t)(((uint64_t)vol * emu_exp2((int32_t)(((int64_t)-att * 10885) >> 16))) >> 16);
        vt = g >= 65536u ? 0xFFFF : (int32_t)g;
        v->ptrx = (pt << 16) | (v->ptrx & 0xFFFFu);
        v->vtft = ((uint32_t)vt << 16) | ft;
    }
    /* The sound generator: current pitch and cutoff take their targets at once; current
       volume slews to its target across the tick, so an envelope step is not a click. */
    v->cpf  = (v->ptrx & 0xFFFF0000u) | (v->cpf & 0xFFFFu);
    v->cvcf = (v->cvcf & 0xFFFF0000u) | (v->vtft & 0xFFFFu);
    vt = (int32_t)(v->vtft >> 16);
    cv = v->cv_acc;
    /* rounded AWAY from zero, so the slew ARRIVES: a truncated step settles short of the
       target (FFFEh for FFFFh) and never moves again. The render clamps the overshoot. */
    { int32_t d = (vt << EMU8K_CV_SH) - cv;
      v->cv_step = (d + (d > 0 ? (int32_t)EMU8K_TICK - 1 : d < 0 ? -((int32_t)EMU8K_TICK - 1) : 0))
                 / (int32_t)EMU8K_TICK; }
    /* p.9: PSST bits 31-24, 0 = extreme RIGHT, FFh = extreme LEFT. The diagram (p.19) sends
       PAN to one side and its LOGICAL NOT to the other: a linear crossfade. */
    { uint32_t pan = v->psst >> 24;
      v->gl = (int32_t)((pan * 256u) / 255u);
      v->gr = (int32_t)(((255u - pan) * 256u) / 255u); }
}

/* ---- the register file (§3) ------------------------------------------------------------- */

/* Data1's MS word and Data2 share E+402h. It is Data1's MS word exactly when the register
   selected is a Data1 DOUBLEWORD (p.6-7): CCCA (r0, every channel) and, in r1, HWCF4/5/6
   (ch 9, 10, 13) and the four sound-memory addresses (ch 20-23). Everything else there is
   the Data2 word register. */
static int emu8k_data1_is_dw(uint8_t reg, uint8_t ch)
{
    if (reg == 0) return 1;
    if (reg != 1) return 0;
    return ch == 9 || ch == 10 || ch == 13 || (ch >= 20 && ch <= 23);
}

static uint32_t half_set(uint32_t r, int hi, uint16_t w)
{ return hi ? (r & 0x0000FFFFu) | ((uint32_t)w << 16) : (r & 0xFFFF0000u) | w; }
static uint16_t half_get(uint32_t r, int hi) { return (uint16_t)(hi ? r >> 16 : r); }

/* DCYSUSV (p.14-15): bit 7 turns the engine off; bit 15 = these are RELEASE values. Turning
   the engine ON with a decay (bit 15 clear) is what starts a note -- §6 writes it last but
   one, after the envelope parameters and the silent CVCF/VTFT. */
static void emu_write_dcysusv(emu8k_voice *v, uint16_t w, emu8k_state *st)
{
    int was_off = (v->dcysusv & 0x80) != 0;
    v->dcysusv = w;
    if (w & 0x80) return;                            /* engine off: nothing more moves   */
    if (w & 0x8000) { env_release(&v->venv); st->releases++; return; }
    if (was_off || v->venv.phase == EMU8K_ENV_OFF || v->venv.phase == EMU8K_ENV_DONE) {
        env_start(&v->venv); env_start(&v->menv);
        v->lfo1_ph = v->lfo2_ph = 0;
        v->lfo1_wait = lfo_delay_ticks(v->lfo1val);
        v->lfo2_wait = lfo_delay_ticks(v->lfo2val);
        st->notes_started++;
    }
}

static void emu_data_write(emu8k_state *st, int port, int hi, uint16_t w)
{
    uint8_t reg = (uint8_t)((st->ptr >> 5) & 7), ch = (uint8_t)(st->ptr & 0x1F);
    emu8k_voice *v = &st->v[ch];
    if (port == 0) {                                 /* Data0: all doublewords            */
        switch (reg) {
        case 0: v->cpf  = half_set(v->cpf, hi, w); break;
        case 1: v->ptrx = half_set(v->ptrx, hi, w); break;
        case 2: v->cvcf = half_set(v->cvcf, hi, w);
                if (hi) { v->cv_acc = (int32_t)w << EMU8K_CV_SH; v->cv_step = 0; }
                break;
        case 3: v->vtft = half_set(v->vtft, hi, w); break;
        case 4: v->d0r4 = half_set(v->d0r4, hi, w); break;
        case 5: v->d0r5 = half_set(v->d0r5, hi, w); break;
        case 6: v->psst = half_set(v->psst, hi, w); break;
        case 7: v->csl  = half_set(v->csl, hi, w); break;
        }
        return;
    }
    if (port == 1) {                                 /* Data1                             */
        switch (reg) {
        case 0:
            /* p.9: bit 27 "should always be zero" -- stored as written */
            v->ccca = half_set(v->ccca, hi, w);
            emu_stream_service(st);                  /* a channel may now carry a stream  */
            break;
        case 1:
            switch (ch) {
            case 9:  st->hwcf4 = half_set(st->hwcf4, hi, w); break;
            case 10: st->hwcf5 = half_set(st->hwcf5, hi, w); break;
            case 13: st->hwcf6 = half_set(st->hwcf6, hi, w); break;
            case 20: case 21: case 22: case 23:
                /* bits 31-24 "Don't Care on write" (p.10-11) */
                st->sma[ch - 20] = half_set(st->sma[ch - 20], hi, w) & EMU8K_ADDR_MASK;
                break;
            case 26: emu_sm_write(st, 0, w); break;  /* SMLD                             */
            case 29: st->hwcf1 = w; break;
            case 30: st->hwcf2 = w; break;
            case 31: st->hwcf3 = w; break;
            default: st->d1r1[ch] = half_set(st->d1r1[ch], hi, w); break;
            }
            break;
        case 2: st->init[0][ch] = w; break;
        case 3: st->init[2][ch] = w; break;
        case 4: v->envvol = w; break;
        case 5: emu_write_dcysusv(v, w, st); break;
        case 6: v->envval = w; break;
        case 7:                                      /* DCYSUS: bit 7 is always zero     */
            v->dcysus = (uint16_t)(w & ~0x80u);
            if (w & 0x8000) env_release(&v->menv);
            break;
        }
        return;
    }
    if (port == 2) {                                 /* Data2: all words                  */
        switch (reg) {
        case 1:
            if (ch == 26) emu_sm_write(st, 1, w);    /* SMRD                             */
            else if (ch != 27) st->d2r1[ch] = w;     /* WC is read-only                  */
            break;
        case 2: st->init[1][ch] = w; break;
        case 3: st->init[3][ch] = w; break;
        case 4: v->atkhldv = (uint16_t)(w & ~0x80u); break;
        case 5: v->lfo1val = w; break;
        case 6: v->atkhld  = (uint16_t)(w & ~0x80u); break;
        case 7: v->lfo2val = w; break;
        default: break;
        }
        return;
    }
    switch (reg) {                                   /* Data3: all words                  */
    case 0: v->ip = w; break;
    case 1: v->ifatn = w; break;
    case 2: v->pefe = w; break;
    case 3: v->fmmod = w; break;
    case 4: v->tremfrq = w; break;
    case 5: v->fm2frq2 = w; break;
    case 6: v->d3r6 = w; break;
    case 7: v->d3r7 = w; break;
    }
}

static uint16_t emu_data_read(emu8k_state *st, int port, int hi)
{
    uint8_t reg = (uint8_t)((st->ptr >> 5) & 7), ch = (uint8_t)(st->ptr & 0x1F);
    emu8k_voice *v = &st->v[ch];
    if (port == 0) {
        switch (reg) {
        case 0: return half_get(v->cpf, hi);
        case 1: return half_get(v->ptrx, hi);
        case 2: return half_get((v->cvcf & 0xFFFFu) | ((uint32_t)(v->cv_acc >> EMU8K_CV_SH) << 16), hi);
        case 3: return half_get(v->vtft, hi);
        case 4: return half_get(v->d0r4, hi);
        case 5: return half_get(v->d0r5, hi);
        case 6: return half_get(v->psst, hi);
        default: return half_get(v->csl, hi);
        }
    }
    if (port == 1) {
        switch (reg) {
        case 0: return half_get(v->ccca, hi);
        case 1:
            switch (ch) {
            case 9:  return half_get(st->hwcf4, hi);
            case 10: return half_get(st->hwcf5, hi);
            case 13: return half_get(st->hwcf6, hi);
            case 20: case 21:                        /* bit 31 = EMPTY, 30-24 read zero   */
                return half_get(st->sma[ch - 20] | (st->sm_empty[ch - 20] ? 0x80000000u : 0), hi);
            case 22: case 23:                        /* bit 31 = FULL                     */
                return half_get(st->sma[ch - 20] | (st->sm_full[ch - 22] ? 0x80000000u : 0), hi);
            case 26: return emu_sm_read(st, 0);
            /* p.13: HWCF1-3 "will not be correctly read by the processor" (a VLSI error).
               What the error LOOKS like is not in the guide; the period drivers probe for
               it -- HWCF1 read back masked with 7Eh must be 58h after 59h was written, and
               HWCF2's low two bits must be set -- so that is the shape modelled. */
            case 29: return (uint16_t)(st->hwcf1 & 0x7E);
            case 30: return (uint16_t)(st->hwcf2 | 0x0003);
            case 31: return st->hwcf3;
            default: return half_get(st->d1r1[ch], hi);
            }
        case 2: return st->init[0][ch];
        case 3: return st->init[2][ch];
        case 4: return v->envvol;
        case 5: return v->dcysusv;
        case 6: return v->envval;
        default: return v->dcysus;
        }
    }
    if (port == 2) {
        switch (reg) {
        case 1:
            if (ch == 26) return emu_sm_read(st, 1);
            if (ch == 27) return emu_wc(st);
            return st->d2r1[ch];
        case 2: return st->init[1][ch];
        case 3: return st->init[3][ch];
        case 4: return v->atkhldv;
        case 5: return v->lfo1val;
        case 6: return v->atkhld;
        case 7: return v->lfo2val;
        default: return 0;
        }
    }
    switch (reg) {
    case 0: return v->ip;
    case 1: return v->ifatn;
    case 2: return v->pefe;
    case 3: return v->fmmod;
    case 4: return v->tremfrq;
    case 5: return v->fm2frq2;
    case 6: return v->d3r6;
    default: return v->d3r7;
    }
}

/* ---- ports (§2) ------------------------------------------------------------------- */

/* A word transfer at offset `off` from the base (000h/002h, 400h/402h, 800h/802h). */
static void emu_word_out(emu8k_state *st, uint16_t off, uint16_t w)
{
    uint8_t reg = (uint8_t)((st->ptr >> 5) & 7), ch = (uint8_t)(st->ptr & 0x1F);
    switch (off) {
    case 0x000: emu_data_write(st, 0, 0, w); break;
    case 0x002: emu_data_write(st, 0, 1, w); break;
    case 0x400: emu_data_write(st, 1, 0, w); break;
    case 0x402: if (emu8k_data1_is_dw(reg, ch)) emu_data_write(st, 1, 1, w);
                else                             emu_data_write(st, 2, 0, w);
                break;
    case 0x800: emu_data_write(st, 3, 0, w); break;
    case 0x802: st->ptr = w; break;
    default: break;
    }
}
static uint16_t emu_word_in(emu8k_state *st, uint16_t off)
{
    uint8_t reg = (uint8_t)((st->ptr >> 5) & 7), ch = (uint8_t)(st->ptr & 0x1F);
    switch (off) {
    case 0x000: return emu_data_read(st, 0, 0);
    case 0x002: return emu_data_read(st, 0, 1);
    case 0x400: return emu_data_read(st, 1, 0);
    case 0x402: return emu8k_data1_is_dw(reg, ch) ? emu_data_read(st, 1, 1) : emu_data_read(st, 2, 0);
    case 0x800: return emu_data_read(st, 3, 0);
    /* p.5: the MS 8 bits of a Pointer read are "random (actually a VLSI test register)" --
       we answer zero there, deterministically. */
    case 0x802: return (uint16_t)(st->ptr & 0x00FF);
    default:    return 0xFFFF;
    }
}

static void emu_out(void *self, uint16_t port, uint8_t w, uint32_t val)
{
    emu8k_state *st = (emu8k_state *)self;
    uint16_t off = (uint16_t)(port - st->base);
    st->io_writes++;
    if (w >= 4) {                                    /* a doubleword: LS word, then MS    */
        emu_word_out(st, off, (uint16_t)val);
        emu_word_out(st, (uint16_t)(off + 2), (uint16_t)(val >> 16));
    } else if (w == 2) {
        emu_word_out(st, off, (uint16_t)val);
    } else {
        /* §2: "no byte I/O transactions are allowed". The guide does not say what a byte
           does; we latch the even byte and complete the word on the odd one, so a program
           that splits a word into two OUTs still lands it. */
        st->byte_io++;
        if (off & 1) emu_word_out(st, (uint16_t)(off - 1),
                                  (uint16_t)(st->blo[off >> 10] | ((val & 0xFF) << 8)));
        else st->blo[off >> 10] = (uint8_t)val;
    }
}

static void emu_in(void *self, uint16_t port, uint8_t w, uint32_t *val)
{
    emu8k_state *st = (emu8k_state *)self;
    uint16_t off = (uint16_t)(port - st->base);
    st->io_reads++;
    if (w >= 4) {
        uint32_t lo = emu_word_in(st, off);
        *val = lo | ((uint32_t)emu_word_in(st, (uint16_t)(off + 2)) << 16);
    } else if (w == 2) {
        *val = emu_word_in(st, off);
    } else {
        /* a byte read of the even port reads the word; the odd port gives its high half */
        st->byte_io++;
        if (off & 1) *val = st->blo[off >> 10];
        else { uint16_t x = emu_word_in(st, off); st->blo[off >> 10] = (uint8_t)(x >> 8); *val = x & 0xFF; }
    }
}

/* ---- the render ------------------------------------------------------------------------- */

void vdd_emu8k_render_st(emu8k_state *st, int16_t *out, uint32_t n)
{
    uint32_t i, k;
    int audible = (st->hwcf3 & 0x0004) != 0;          /* §4: HWCF3 enables audio output  */
    st->renders++;
    for (i = 0; i < n; ++i) {
        int32_t accl = 0, accr = 0, l, r;
        if (st->tick_pos == 0)
            for (k = 0; k < EMU8K_VOICES; ++k) emu_voice_tick(&st->v[k]);
        st->tick_pos = (st->tick_pos + 1) % EMU8K_TICK;
        for (k = 0; k < EMU8K_VOICES; ++k) {
            emu8k_voice *v = &st->v[k];
            uint32_t ca, f, ls, le, step;
            int32_t g;
            if (v->ccca & 0x04000000u) continue;     /* a DMA channel makes no sound       */
            /* current volume slews to its target (clamped: the step is a rounded division) */
            if (v->cv_step) {
                int32_t tgt = (int32_t)(v->vtft >> 16) << EMU8K_CV_SH;
                v->cv_acc += v->cv_step;
                if ((v->cv_step > 0 && v->cv_acc > tgt) || (v->cv_step < 0 && v->cv_acc < tgt)) {
                    v->cv_acc = tgt; v->cv_step = 0;
                }
            }
            ca = v->ccca & EMU8K_ADDR_MASK;
            f  = v->cpf & 0xFFFFu;
            g  = v->cv_acc >> EMU8K_CV_SH;
            if (g > 0 && audible) {
                /* CA is "one word lower than the actual audio location" (p.10): the
                   interpolator reads CA+1 and CA+2, weighted by the fraction. */
                int32_t s0 = emu_mem(st, ca + 1), s1 = emu_mem(st, ca + 2), s;
                filt_setup(v, (uint16_t)v->cvcf, (uint8_t)(v->ccca >> 28));
                s = s0 + (int32_t)(((int64_t)(s1 - s0) * (int32_t)f) >> 16);
                s = filt_run(v, s);
                s = (int32_t)(((int64_t)s * g) >> 16);
                accl += (s * v->gl) >> 8;
                accr += (s * v->gr) >> 8;
            }
            /* advance: CP 4000h = one word per sample (p.7), so the step in 1/65536 words
               is CP × 4. Then ALWAYS loop (§5): passing CSL returns to PSST. */
            step = f + ((v->cpf >> 16) << 2);
            ca = (ca + (step >> 16)) & EMU8K_ADDR_MASK;
            ls = v->psst & EMU8K_ADDR_MASK;
            le = v->csl & EMU8K_ADDR_MASK;
            if (le > ls && ca >= le) ca = ls + (ca - le) % (le - ls);
            v->ccca = (v->ccca & 0xFF000000u) | ca;
            v->cpf  = (v->cpf & 0xFFFF0000u) | (step & 0xFFFFu);
        }
        l = accl > 32767 ? 32767 : accl < -32768 ? -32768 : accl;
        r = accr > 32767 ? 32767 : accr < -32768 ? -32768 : accr;
        out[2 * i] = (int16_t)l; out[2 * i + 1] = (int16_t)r;
        if (l || r) {
            uint32_t a = (uint32_t)(l < 0 ? -l : l), b = (uint32_t)(r < 0 ? -r : r);
            st->out_nonzero++;
            if (a > st->out_peak) st->out_peak = a;
            if (b > st->out_peak) st->out_peak = b;
        }
        st->wc++;
    }
    st->samples_out += n;
}

/* ---- the bus ------------------------------------------------------------------------------ */

/* ── THE RESET STATE IS THE INITIALISED CHIP, NOT POWER-UP NOISE. §4: at power-up "most
     registers contain random data" and HWCF3's audio-enable is clear; a real machine ran
     AWEUTIL /S from AUTOEXEC.BAT to run the §4 procedure before any game started, and most
     games rely on that having happened. We have no AUTOEXEC to run it from, so reset leaves
     the chip exactly as §4 leaves it: every channel's engine off and silent, the sound-memory
     addresses zero, HWCF1/2/3 = 0059h/0020h/0004h and HWCF4/5/6 = 0/83h/8000h. The INIT
     arrays hold zero -- they only program the reverb/chorus/EQ effects, which are not
     modelled. A guest that runs the §4 procedure itself gets the same state again. */
void vdd_emu8k_reset(void *self)
{
    emu8k_state *st = (emu8k_state *)self;
    unsigned i;
    uint8_t *p;
    emu_build_tables();
    p = (uint8_t *)st->v;
    for (i = 0; i < sizeof st->v; ++i) p[i] = 0;
    for (i = 0; i < EMU8K_VOICES; ++i) {
        st->v[i].dcysusv = 0x0080;                   /* §4 step 1: engine off             */
        st->v[i].gl = st->v[i].gr = 128;
    }
    p = (uint8_t *)st->init;
    for (i = 0; i < sizeof st->init; ++i) p[i] = 0;
    for (i = 0; i < 32; ++i) { st->d1r1[i] = 0; st->d2r1[i] = 0; }
    for (i = 0; i < 4; ++i) st->sma[i] = 0;
    st->sm_rd[0] = st->sm_rd[1] = st->sm_wr[0] = st->sm_wr[1] = 0;
    st->sm_empty[0] = st->sm_empty[1] = st->sm_full[0] = st->sm_full[1] = 0;
    st->hwcf1 = 0x0059; st->hwcf2 = 0x0020; st->hwcf3 = 0x0004;
    st->hwcf4 = 0; st->hwcf5 = 0x83; st->hwcf6 = 0x8000;
    st->ptr = 0; st->blo[0] = st->blo[1] = st->blo[2] = 0;
    st->tick_pos = 0;
    /* WC is NOT reset: p.13 "there is no mechanism to reset this counter". */
}

int vdd_emu8k_init(vdd_bus *b, void *self)
{
    emu8k_state *st = (emu8k_state *)self;
    st->bus = b;
    if (!st->base) st->base = EMU8K_DEFAULT_BASE;
    vdd_emu8k_reset(st);
    /* §2: three groups of four ports. */
    if (vdd_claim_ports(b, st->base, (uint16_t)(st->base + 3), emu_in, emu_out, st)) return -1;
    if (vdd_claim_ports(b, (uint16_t)(st->base + 0x400), (uint16_t)(st->base + 0x403), emu_in, emu_out, st)) return -1;
    if (vdd_claim_ports(b, (uint16_t)(st->base + 0x800), (uint16_t)(st->base + 0x803), emu_in, emu_out, st)) return -1;
    return 0;
}
