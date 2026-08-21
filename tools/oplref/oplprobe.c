/* oplprobe.c -- the SINGLE-NOTE experiment rig for the OPL synth.  DEV TOOL ONLY.
 *
 * WHY THIS EXISTS, and why it is not just `oplcmp` with a smaller file.
 * `oplcmp` replays a game trace and scores it. That proves the timbre is wrong; it
 * cannot say WHICH parameter is wrong, because every note moves every variable at
 * once. This rig does the opposite: it programs ONE channel, holds everything
 * still, moves ONE register, and measures the SAME quantity out of both cores. A
 * sweep that used to be an afternoon of listening is a for-loop that prints a
 * number.
 *
 * ORACLE DISCIPLINE (see return-ntvdm.md). The reference core is LGPL-2.1 and our
 * synth is deliberately clean-room MIT, so the reference is used STRICTLY as a
 * BLACK BOX: controlled register writes in, samples out, constants derived from
 * the measurement. Its source is not read for values. That is why every experiment
 * here reports a DERIVED PHYSICAL QUANTITY (a dB slope, a modulation index) rather
 * than a code constant -- the physical quantity is what the datasheet describes and
 * what the silicon does, and it is what we are entitled to match.
 *
 * The measurements are spectral, not sample-by-sample, because that is what
 * "timbre" means. All test notes are tuned so that one cycle is EXACTLY 128 samples
 * at the chip's native rate, so a DFT over 64 cycles has no leakage and a harmonic
 * magnitude is exact rather than approximate.
 *
 *   Build:  tools/oplref/build.sh        Run:  build/oplref/oplprobe <experiment>
 *   Experiments: validate tl mod fb wave env ksl mult all
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#include "../../src/vdd/vdd_opl.h"
#include "opl3.h"

#define RATE  49716
#define PERIOD  128             /* samples per cycle of the test note (exact)     */
#define CYCLES   64
#define NWIN   (PERIOD * CYCLES)
#define NSETTLE  4096           /* let the attack finish before measuring         */
#define NMAX   (NSETTLE + NWIN)

/* The test note: fnum 0x200, block 4, MULT x1 -> phase advances 8 index steps per
   sample, so 1024 steps = 128 samples = one cycle, exactly. */
#define TEST_FNUM  0x200
#define TEST_BLOCK 4

typedef struct { uint8_t reg, val; } rv;

/* The table the synth ships, so exp_kslrom can print the difference rather than
   leave the reader to subtract 16 numbers by hand. */
static const uint8_t opl_kslrom_probe[16] =
    { 0, 32, 40, 45, 48, 51, 53, 55, 56, 58, 59, 60, 61, 62, 63, 64 };

static int16_t g_a[NMAX], g_b[NMAX];
static opl_state g_ours;
static opl3_chip g_ref;

/* --- driving both cores ----------------------------------------------------- */
static void both_reset(void)
{
    memset(&g_ours, 0, sizeof g_ours);
    g_ours.sample_hz = RATE;
    g_ours.ext_clock = 1;
    vdd_opl_reset(&g_ours);
    OPL3_Reset(&g_ref, RATE);
}

static void both_write(uint8_t reg, uint8_t val)
{
    vdd_opl_write_reg(&g_ours, reg, val);
    OPL3_WriteReg(&g_ref, reg, val);
}

static void both_prog(const rv *p, int n)
{
    int i;
    for (i = 0; i < n; i++) both_write(p[i].reg, p[i].val);
}

static void both_render(size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        int16_t s = 0, buf[2] = { 0, 0 };
        vdd_opl_render(&g_ours, &s, 1);
        OPL3_GenerateResampled(&g_ref, buf);
        g_a[i] = s;
        g_b[i] = buf[0];
    }
}

/* --- measurement ------------------------------------------------------------ */
/* Magnitude of harmonic k over the measurement window. `k` cycles-per-fundamental
   times CYCLES gives an integer bin, so this is a clean DFT coefficient. */
static double harm(const int16_t *s, int k)
{
    double re = 0, im = 0;
    int i, bin = k * CYCLES;
    for (i = 0; i < NWIN; i++) {
        double ang = 2.0 * M_PI * bin * i / NWIN;
        re += s[NSETTLE + i] * cos(ang);
        im -= s[NSETTLE + i] * sin(ang);
    }
    return 2.0 * sqrt(re * re + im * im) / NWIN;
}

static double rms_of(const int16_t *s, size_t from, size_t n)
{
    double e = 0; size_t i;
    for (i = 0; i < n; i++) e += (double)s[from + i] * s[from + i];
    return sqrt(e / n);
}

static double peak_of(const int16_t *s, size_t from, size_t n)
{
    double p = 0; size_t i;
    for (i = 0; i < n; i++) { double v = fabs((double)s[from + i]); if (v > p) p = v; }
    return p;
}

/* J_n by numeric integration of its integral form -- no libm bessel needed and
   accurate to well past what we can measure. */
static double besselj(int n, double x)
{
    const int N = 2048;
    double s = 0; int i;
    for (i = 0; i <= N; i++) {
        double th = M_PI * i / N;
        double w = (i == 0 || i == N) ? 0.5 : 1.0;
        s += w * cos(n * th - x * sin(th));
    }
    return s * (M_PI / N) / M_PI;
}

/* Phase modulation of a sine carrier by a sine modulator at the SAME frequency
   puts harmonic k at J_{k-1}(b) + (-1)^k J_{k+1}(b) -- the second term is the
   negative-order sideband folding back through zero. Fit b to the measured
   harmonic magnitudes: that single number IS the modulation index, and it is the
   quantity the two cores must agree on. */
#define NHARM 12
#define NB     20001            /* index i == modulation index i*BSTEP            */
#define BSTEP  0.002

/* The model shape for every candidate index, built once. Without this the fit
   dominates the runtime and a sweep takes minutes instead of seconds. */
static double (*g_jt)[NHARM + 1];

static void fit_init(void)
{
    int i, k;
    if (g_jt) return;
    g_jt = malloc(sizeof(*g_jt) * NB);
    for (i = 0; i < NB; i++) {
        double b = i * BSTEP;
        for (k = 1; k <= NHARM; k++)
            g_jt[i][k] = fabs(besselj(k - 1, b) + ((k & 1) ? -1.0 : 1.0) * besselj(k + 1, b));
    }
}

static double fit_index(const double *h, double *out_scale)
{
    double best = 0, bestErr = 1e300, bs = 0;
    int i, k;
    fit_init();
    for (i = 0; i < NB; i++) {
        const double *m = g_jt[i];
        double num = 0, den = 0, err = 0, s;
        for (k = 1; k <= NHARM; k++) { num += h[k] * m[k]; den += m[k] * m[k]; }
        if (den <= 0) continue;
        s = num / den;
        for (k = 1; k <= NHARM; k++) { double d = h[k] - s * m[k]; err += d * d; }
        if (err < bestErr) { bestErr = err; best = i * BSTEP; bs = s; }
    }
    if (out_scale) *out_scale = bs;
    return best;
}

/* --- the standard test note ------------------------------------------------- *
 * Channel 0, operators at register offsets 0x00 (modulator) and 0x03 (carrier).
 * EGT=1 so the note SUSTAINS (otherwise it decays under the measurement window),
 * AR=15 so the attack is over before it, DR=0/SL=0 so nothing moves afterwards.  */
/* PARKING AN OPERATOR. There is no way to key one operator of a channel off, so an
   experiment that wants to hear only ONE of them has to silence the other -- and
   TL=63 is only -47 dB, not silence. A residual sine 47 dB down is 0.4% of H1,
   which is fine for a level reading and fatal for a distortion reading. So the
   unwanted operator also gets MULT=12: its residual then lands on harmonic 12 and
   cannot contaminate the harmonics being measured at all. */
#define PARK_MULT 12
#define PARK_TL   0x3F

static void note_setup2(uint8_t mod_tl, uint8_t car_tl, uint8_t fb, uint8_t cnt,
                        uint8_t mod_wave, uint8_t car_wave,
                        uint8_t mod_mult, uint8_t car_mult)
{
    rv p[] = {
        { 0x01, 0x20 },                 /* waveform select enable                 */
        { 0x20, (uint8_t)(0x20 | mod_mult) },   /* EGT=1, KSR=0, no AM/VIB        */
        { 0x23, (uint8_t)(0x20 | car_mult) },
        { 0x40, mod_tl }, { 0x43, car_tl },
        { 0x60, 0xF0 }, { 0x63, 0xF0 }, /* AR=15 DR=0                             */
        { 0x80, 0x0F }, { 0x83, 0x0F }, /* SL=0  RR=15                            */
        { 0xE0, mod_wave }, { 0xE3, car_wave },
        { 0xC0, (uint8_t)((fb << 1) | cnt) },
        { 0xA0, TEST_FNUM & 0xFF },
        { 0xB0, (uint8_t)(0x20 | (TEST_BLOCK << 2) | ((TEST_FNUM >> 8) & 3)) },
    };
    both_reset();
    both_prog(p, (int)(sizeof p / sizeof p[0]));
}

static void note_setup(uint8_t mod_tl, uint8_t car_tl, uint8_t fb, uint8_t cnt,
                       uint8_t mod_wave, uint8_t car_wave)
{
    note_setup2(mod_tl, car_tl, fb, cnt, mod_wave, car_wave, 1, 1);
}

static void harmonics(const int16_t *s, double *h)
{
    int k;
    for (k = 1; k <= NHARM; k++) h[k] = harm(s, k);
}

/* ============================================================================ *
 * EXPERIMENT 0 -- VALIDATE THE INSTRUMENT.
 * Do not bisect against an unverified instrument: this harness has already
 * produced one artefact of its own (a write-latency queue on the reference cost a
 * quarter of the apparent defect). Three cheap invariants catch that class.
 * ============================================================================ */
static int exp_validate(void)
{
    int fails = 0;
    size_t i;

    /* 1. silence in -> silence out, both cores. */
    both_reset();
    both_render(NMAX);
    { double pa = peak_of(g_a, 0, NMAX), pb = peak_of(g_b, 0, NMAX);
      printf("  silence:      ours peak %.0f   ref peak %.0f    %s\n",
             pa, pb, (pa == 0 && pb == 0) ? "ok" : "FAIL");
      if (pa != 0 || pb != 0) fails++; }

    /* 2. determinism: the same program twice must be bit-identical, or every
          later comparison is measuring noise. */
    note_setup2(PARK_TL, 0x00, 0, 1, 0, 0, PARK_MULT, 1);
    both_render(NMAX);
    { static int16_t sa[NMAX], sb[NMAX];
      int same_a, same_b;
      memcpy(sa, g_a, sizeof sa); memcpy(sb, g_b, sizeof sb);
      note_setup2(PARK_TL, 0x00, 0, 1, 0, 0, PARK_MULT, 1);
      both_render(NMAX);
      same_a = memcmp(sa, g_a, sizeof sa) == 0;
      same_b = memcmp(sb, g_b, sizeof sb) == 0;
      printf("  determinism:  ours %s   ref %s\n",
             same_a ? "ok" : "FAIL", same_b ? "ok" : "FAIL");
      if (!same_a || !same_b) fails++; }

    /* 3. a bare note is a SINE in both: harmonic 1 must dominate. This is the
          check that says "the two cores are playing the same note at all". */
    { double ha[NHARM + 1], hb[NHARM + 1];
      double thd_a, thd_b, ea = 0, eb = 0;
      int k;
      harmonics(g_a, ha); harmonics(g_b, hb);
      /* stop short of PARK_MULT: harmonic 12 is where the parked operator sits */
      for (k = 2; k < PARK_MULT; k++) { ea += ha[k] * ha[k]; eb += hb[k] * hb[k]; }
      thd_a = ha[1] > 0 ? 100.0 * sqrt(ea) / ha[1] : 0;
      thd_b = hb[1] > 0 ? 100.0 * sqrt(eb) / hb[1] : 0;
      printf("  pure tone:    ours H1 %8.1f (THD %.2f%%)   ref H1 %8.1f (THD %.2f%%)\n",
             ha[1], thd_a, hb[1], thd_b);
      if (thd_a > 5.0 || thd_b > 5.0) { printf("    THD too high -- not a sine\n"); fails++; }
    }

    /* 4. pitch: zero crossings must agree, or every spectral bin is misaligned. */
    { long za = 0, zb = 0;
      for (i = NSETTLE + 1; i < NMAX; i++) {
          if ((g_a[i - 1] < 0) != (g_a[i] < 0)) za++;
          if ((g_b[i - 1] < 0) != (g_b[i] < 0)) zb++;
      }
      printf("  pitch:        ours %ld zero-crossings   ref %ld   (expect %d)   %s\n",
             za, zb, 2 * CYCLES, (za == zb) ? "ok" : "FAIL");
      if (za != zb) fails++; }

    return fails;
}

/* ============================================================================ *
 * EXPERIMENT A -- TOTAL LEVEL.
 * One operator, no modulation, sweep TL. Gives the absolute full-scale amplitude
 * of an operator in each core and the dB-per-TL-step slope. Should settle the
 * level ratio on its own.
 * ============================================================================ */
static void exp_tl(void)
{
    int tl;
    double a0 = 0, b0 = 0;
    printf("  TL   ours H1     ref H1    ratio   ours dB   ref dB   (0.75 dB/step expected)\n");
    for (tl = 0; tl <= 63; tl++) {
        double ha[NHARM + 1], hb[NHARM + 1];
        /* additive, modulator parked off harmonic 1: only the carrier is measured */
        note_setup2(PARK_TL, (uint8_t)tl, 0, 1, 0, 0, PARK_MULT, 1);
        both_render(NMAX);
        harmonics(g_a, ha); harmonics(g_b, hb);
        if (tl == 0) { a0 = ha[1]; b0 = hb[1]; }
        if (tl % 4 == 0 || tl >= 60)
            printf("  %2d  %9.2f  %9.2f  %7.3f  %8.2f %8.2f\n", tl, ha[1], hb[1],
                   hb[1] > 0 ? ha[1] / hb[1] : 0.0,
                   ha[1] > 0 ? 20 * log10(ha[1] / a0) : -99,
                   hb[1] > 0 ? 20 * log10(hb[1] / b0) : -99);
    }
    printf("\n  FULL SCALE (TL=0):  ours %.2f   ref %.2f   ratio %.4f\n",
           a0, b0, b0 > 0 ? a0 / b0 : 0.0);
}

/* ============================================================================ *
 * EXPERIMENT B -- MODULATION INDEX.  THE PRIME SUSPECT.
 * Two operators, carrier at full volume, sweep the MODULATOR's TL. The fitted
 * index b must fall 0.75 dB per TL step in both cores; what matters is b at TL=0,
 * which is the modulation depth scaling constant expressed physically.
 * ============================================================================ */
static void exp_mod(void)
{
    int tl;
    printf("  modTL   ours b     ref b    b ratio    ours H1/H2/H3      ref H1/H2/H3\n");
    for (tl = 0; tl <= 40; tl += 2) {
        double ha[NHARM + 1], hb[NHARM + 1], ba, bb;
        note_setup((uint8_t)tl, 0x00, 0, 0, 0, 0);
        both_render(NMAX);
        harmonics(g_a, ha); harmonics(g_b, hb);
        ba = fit_index(ha, NULL);
        bb = fit_index(hb, NULL);
        printf("  %3d   %7.3f   %7.3f   %7.3f   %6.0f %6.0f %6.0f   %6.0f %6.0f %6.0f\n",
               tl, ba, bb, bb > 0 ? ba / bb : 0.0,
               ha[1], ha[2], ha[3], hb[1], hb[2], hb[3]);
    }
    /* The headline number: index at zero attenuation. */
    { double ha[NHARM + 1], hb[NHARM + 1], ba, bb;
      note_setup(0x00, 0x00, 0, 0, 0, 0);
      both_render(NMAX);
      harmonics(g_a, ha); harmonics(g_b, hb);
      ba = fit_index(ha, NULL); bb = fit_index(hb, NULL);
      printf("\n  MODULATION INDEX AT modTL=0:  ours %.4f rad   ref %.4f rad   ratio %.4f\n",
             ba, bb, bb > 0 ? ba / bb : 0.0);
      printf("  (in cycles: ours %.4f   ref %.4f)\n", ba / (2 * M_PI), bb / (2 * M_PI)); }
}

/* ============================================================================ *
 * EXPERIMENT C -- FEEDBACK.
 * One self-modulating operator heard alone (carrier silenced, additive connection),
 * sweep FB 0..7. Feedback is not pure PM so the fitted index is only indicative --
 * the harmonic magnitudes are the real comparison.
 * ============================================================================ */
static void exp_fb(void)
{
    int fb;
    printf("  FB   ours H1   ref H1   ours H2   ref H2   ours H3   ref H3   ours b   ref b\n");
    for (fb = 0; fb <= 7; fb++) {
        double ha[NHARM + 1], hb[NHARM + 1];
        /* additive, carrier parked off the harmonics we measure */
        note_setup2(0x00, PARK_TL, (uint8_t)fb, 1, 0, 0, 1, PARK_MULT);
        both_render(NMAX);
        harmonics(g_a, ha); harmonics(g_b, hb);
        printf("  %d  %8.0f %8.0f  %8.0f %8.0f  %8.0f %8.0f  %7.3f %7.3f\n",
               fb, ha[1], hb[1], ha[2], hb[2], ha[3], hb[3],
               fit_index(ha, NULL), fit_index(hb, NULL));
    }
}

/* ============================================================================ *
 * EXPERIMENT D -- WAVEFORMS.  The four OPL2 shapes, unmodulated.
 * ============================================================================ */
static void exp_wave(void)
{
    int w, k;
    for (w = 0; w < 4; w++) {
        double ha[NHARM + 1], hb[NHARM + 1];
        note_setup2(PARK_TL, 0x00, 0, 1, 0, (uint8_t)w, PARK_MULT, 1);
        both_render(NMAX);
        harmonics(g_a, ha); harmonics(g_b, hb);
        printf("  wave %d   ours:", w);
        for (k = 1; k <= 6; k++) printf(" %7.0f", ha[k]);
        printf("\n           ref :");
        for (k = 1; k <= 6; k++) printf(" %7.0f", hb[k]);
        printf("\n           rms ours %.1f  ref %.1f  ratio %.3f  dc ours %.1f ref %.1f\n",
               rms_of(g_a, NSETTLE, NWIN), rms_of(g_b, NSETTLE, NWIN),
               rms_of(g_b, NSETTLE, NWIN) > 0
                   ? rms_of(g_a, NSETTLE, NWIN) / rms_of(g_b, NSETTLE, NWIN) : 0.0,
               harm(g_a, 0), harm(g_b, 0));
    }
}

/* ============================================================================ *
 * EXPERIMENT E -- ENVELOPE RATES.
 * The synth's own header nominates these as "the first thing to refine", and the
 * game trace says ours goes fully silent 14.4% of the time against the reference's
 * 0.1% -- which is an ENVELOPE statement, not a timbre one. Measure attack time
 * (key-on to 90% of peak) and release time (key-off to -40 dB) at every rate.
 * ============================================================================ */
#define ENVLEN (RATE * 8)               /* 8 s outlasts every rate we can measure  */
static int16_t g_ea[ENVLEN], g_eb[ENVLEN];

static void env_render(int16_t *dst_a, int16_t *dst_b, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        int16_t s = 0, buf[2] = { 0, 0 };
        vdd_opl_render(&g_ours, &s, 1);
        OPL3_GenerateResampled(&g_ref, buf);
        dst_a[i] = s; dst_b[i] = buf[0];
    }
}

/* Envelope follower: peak magnitude in each 128-sample (one cycle) window. */
static double env_at(const int16_t *s, int i)
{
    int j; double p = 0;
    for (j = 0; j < PERIOD && i + j < ENVLEN; j++) {
        double v = fabs((double)s[i + j]);
        if (v > p) p = v;
    }
    return p;
}

static double time_to_rise(const int16_t *s, int n, double frac)
{
    double pk = 0; int i;
    for (i = 0; i + PERIOD < n; i += PERIOD) { double e = env_at(s, i); if (e > pk) pk = e; }
    if (pk <= 0) return -1;
    for (i = 0; i + PERIOD < n; i += PERIOD)
        if (env_at(s, i) >= frac * pk) return 1000.0 * i / RATE;
    return -1;
}

static double time_to_fall(const int16_t *s, int n, double db)
{
    double e0 = env_at(s, 0), thr; int i;
    if (e0 <= 0) return -1;
    thr = e0 * pow(10.0, db / 20.0);
    for (i = 0; i + PERIOD < n; i += PERIOD)
        if (env_at(s, i) <= thr) return 1000.0 * i / RATE;
    return -1;
}

static void exp_env(void)
{
    int r;
    /* --- attack: key on with AR=r, DR=0 (nothing follows), measure the rise. --- */
    printf("  ATTACK (ms from key-on to 90%% of peak; AR sweep, DR=0)\n");
    printf("  AR    ours ms    ref ms     ratio\n");
    for (r = 1; r <= 15; r++) {
        rv p[] = {
            { 0x01, 0x20 }, { 0x20, 0x21 }, { 0x23, 0x21 },
            { 0x40, 0x3F }, { 0x43, 0x00 },
            { 0x60, 0x00 }, { 0x63, (uint8_t)(r << 4) },
            { 0x80, 0x0F }, { 0x83, 0x0F },
            { 0xE0, 0x00 }, { 0xE3, 0x00 }, { 0xC0, 0x00 },
            { 0xA0, TEST_FNUM & 0xFF },
            { 0xB0, (uint8_t)(0x20 | (TEST_BLOCK << 2) | ((TEST_FNUM >> 8) & 3)) },
        };
        double ta, tb;
        both_reset();
        both_prog(p, (int)(sizeof p / sizeof p[0]));
        env_render(g_ea, g_eb, ENVLEN);
        ta = time_to_rise(g_ea, ENVLEN, 0.9);
        tb = time_to_rise(g_eb, ENVLEN, 0.9);
        printf("  %2d  %9.2f %9.2f  %8.3f\n", r, ta, tb, tb > 0 ? ta / tb : 0.0);
    }

    /* --- decay: AR=15 (instant), DR=r, SL=15 (decay all the way), EGT=0. ------ */
    printf("\n  DECAY (ms from full to -40 dB; AR=15, DR sweep, SL=15)\n");
    printf("  DR    ours ms    ref ms     ratio\n");
    for (r = 1; r <= 15; r++) {
        rv p[] = {
            { 0x01, 0x20 }, { 0x20, 0x01 }, { 0x23, 0x01 },   /* EGT=0 percussive */
            { 0x40, 0x3F }, { 0x43, 0x00 },
            { 0x60, 0x00 }, { 0x63, (uint8_t)(0xF0 | r) },
            { 0x80, 0x0F }, { 0x83, 0xFF },                   /* SL=15 RR=15      */
            { 0xE0, 0x00 }, { 0xE3, 0x00 }, { 0xC0, 0x00 },
            { 0xA0, TEST_FNUM & 0xFF },
            { 0xB0, (uint8_t)(0x20 | (TEST_BLOCK << 2) | ((TEST_FNUM >> 8) & 3)) },
        };
        double ta, tb;
        both_reset();
        both_prog(p, (int)(sizeof p / sizeof p[0]));
        env_render(g_ea, g_eb, ENVLEN);
        ta = time_to_fall(g_ea, ENVLEN, -40.0);
        tb = time_to_fall(g_eb, ENVLEN, -40.0);
        printf("  %2d  %9.2f %9.2f  %8.3f\n", r, ta, tb, tb > 0 ? ta / tb : 0.0);
    }

    /* --- release: sustain, then key off with RR=r. --------------------------- */
    printf("\n  RELEASE (ms from key-off to -40 dB; RR sweep)\n");
    printf("  RR    ours ms    ref ms     ratio\n");
    for (r = 1; r <= 15; r++) {
        rv p[] = {
            { 0x01, 0x20 }, { 0x20, 0x21 }, { 0x23, 0x21 },
            { 0x40, 0x3F }, { 0x43, 0x00 },
            { 0x60, 0x00 }, { 0x63, 0xF0 },
            { 0x80, 0x0F }, { 0x83, (uint8_t)r },
            { 0xE0, 0x00 }, { 0xE3, 0x00 }, { 0xC0, 0x00 },
            { 0xA0, TEST_FNUM & 0xFF },
            { 0xB0, (uint8_t)(0x20 | (TEST_BLOCK << 2) | ((TEST_FNUM >> 8) & 3)) },
        };
        double ta, tb;
        both_reset();
        both_prog(p, (int)(sizeof p / sizeof p[0]));
        env_render(g_ea, g_eb, RATE / 10);              /* settle 100 ms          */
        both_write(0xB0, (uint8_t)((TEST_BLOCK << 2) | ((TEST_FNUM >> 8) & 3)));
        env_render(g_ea, g_eb, ENVLEN);
        ta = time_to_fall(g_ea, ENVLEN, -40.0);
        tb = time_to_fall(g_eb, ENVLEN, -40.0);
        printf("  %2d  %9.2f %9.2f  %8.3f\n", r, ta, tb, tb > 0 ? ta / tb : 0.0);
    }

    /* --- AR=0 is a documented special case and an easy thing to get backwards:
           does the operator stay silent, or jump to full volume? ---------------- */
    {
        rv p[] = {
            { 0x01, 0x20 }, { 0x20, 0x21 }, { 0x23, 0x21 },
            { 0x40, 0x3F }, { 0x43, 0x00 },
            { 0x60, 0x00 }, { 0x63, 0x00 },             /* AR=0 DR=0              */
            { 0x80, 0x0F }, { 0x83, 0x0F },
            { 0xE0, 0x00 }, { 0xE3, 0x00 }, { 0xC0, 0x00 },
            { 0xA0, TEST_FNUM & 0xFF },
            { 0xB0, (uint8_t)(0x20 | (TEST_BLOCK << 2) | ((TEST_FNUM >> 8) & 3)) },
        };
        both_reset();
        both_prog(p, (int)(sizeof p / sizeof p[0]));
        env_render(g_ea, g_eb, ENVLEN);
        printf("\n  AR=0 special case:  ours peak %.0f   ref peak %.0f   %s\n",
               peak_of(g_ea, 0, ENVLEN), peak_of(g_eb, 0, ENVLEN),
               peak_of(g_eb, 0, ENVLEN) < 16 ? "(ref stays SILENT)" : "(ref sounds)");
    }
}

/* ============================================================================ *
 * EXPERIMENT H -- THE ENVELOPE RATE LAW, DERIVED.
 *
 * `env` above shows decay is 1.5x too slow, but a ratio is not a law and nudging a
 * constant until the ratio reads 1.0 would be fitting noise. What the hardware
 * actually defines is a DECAY SLOPE per effective 6-bit rate, and that is a
 * physical quantity we can measure directly: time the reference's attenuation
 * between two known levels and divide.
 *
 * The effective rate is 4*R + rof, where rof (0..3) comes from where the note sits
 * on the keyboard. To reach ADJACENT rates -- which is the only way to see the
 * sub-step structure inside a group of four -- this sweeps rof via the BLOCK, and
 * compensates the resulting pitch change with MULT so the envelope follower's
 * window is valid in every case.
 *
 * Reported as SAMPLES PER ENVELOPE UNIT (one unit = 0.1875 dB) and as the implied
 * ANCHOR, rate + 4*log2(samples-per-unit), which is constant if and only if the
 * law really is "speed doubles every 4 rate steps".
 * ============================================================================ */
#define EG_DB_LO   -3.0                 /* fit between these two levels           */
#define EG_DB_HI  -48.0
#define EG_WIN    160                   /* >= one cycle in every (block,mult) case */
#define EG_STEP    20                   /* follower hop: 8 readings per window     */
#define EG_MINPTS   8                   /* fewer than this and the fit is a guess  */
/* THE FLOOR MATTERS. Below roughly this amplitude the 16-bit output quantises, the
   measured envelope stops falling, and a regression that includes those points
   reads a slope that is far too shallow -- which is exactly how an earlier run of
   this experiment reported a decay constant 8% too slow AND a perfectly constant
   anchor column that made it look right. Stay well above it. */
#define EG_FLOOR   24.0

static double env_pk(const int16_t *s, int i, int w)
{
    int j; double p = 0;
    for (j = 0; j < w && i + j < ENVLEN; j++) {
        double v = fabs((double)s[i + j]);
        if (v > p) p = v;
    }
    return p;
}

/* Samples per envelope unit (one unit = 0.1875 dB), by LEAST-SQUARES over the
   whole decay rather than the time between two crossings. Two crossings quantise
   to the follower's window and throw away every sample in between; a regression
   over the straight part of the dB curve uses all of it and is what makes adjacent
   rates -- which differ by only 19% -- distinguishable at all. */
static double eg_slope(const int16_t *s)
{
    double pk = 0, lo, hi, sx = 0, sy = 0, sxx = 0, sxy = 0, slope;
    int i, n = 0;
    for (i = 0; i + EG_WIN < ENVLEN; i += EG_STEP) { double e = env_pk(s, i, EG_WIN); if (e > pk) pk = e; }
    if (pk <= 0) return -1;
    hi = pk * pow(10.0, EG_DB_LO / 20.0);
    lo = pk * pow(10.0, EG_DB_HI / 20.0);
    for (i = 0; i + EG_WIN < ENVLEN; i += EG_STEP) {
        double e = env_pk(s, i, EG_WIN), y;
        if (e > hi) continue;
        if (e < lo || e < EG_FLOOR) break;
        y = 20.0 * log10(e / pk);
        sx += i; sy += y; sxx += (double)i * i; sxy += (double)i * y; n++;
    }
    if (n < EG_MINPTS) return -1;
    slope = (n * sxy - sx * sy) / (n * sxx - sx * sx);     /* dB per sample        */
    if (slope >= 0) return -1;
    return 0.1875 / -slope;
}

static void exp_egrate(void)
{
    /* rof is set by BLOCK (with KSR=0, rof = ((block<<1)|fnum9) >> 2); MULT pulls
       the pitch back into the follower's window. */
    static const uint8_t blk[4] = { 0, 2, 4, 6 }, mlt[4] = { 15, 4, 1, 0 };
    int R, rof;
    printf("  rate   ours smp/unit   ref smp/unit    ratio    ours anchor   ref anchor\n");
    for (R = 1; R <= 15; R++) {
        for (rof = 0; rof < 4; rof++) {
            int rate = R * 4 + rof;
            rv p[] = {
                { 0x01, 0x20 },
                { 0x20, mlt[rof] }, { 0x23, mlt[rof] },   /* EGT=0 percussive     */
                { 0x40, 0x3F }, { 0x43, 0x00 },
                /* the parked modulator gets DR=15 too: at TL=63 it is only -47 dB,
                   which would otherwise sit ABOVE the tail of the decay we are
                   trying to measure and hide it completely. */
                { 0x60, 0xFF }, { 0x63, (uint8_t)(0xF0 | R) },
                { 0x80, 0xFF }, { 0x83, 0xFF },           /* SL=15: decay all way */
                { 0xE0, 0x00 }, { 0xE3, 0x00 }, { 0xC0, 0x01 },   /* additive     */
                { 0xA0, TEST_FNUM & 0xFF },
                { 0xB0, (uint8_t)(0x20 | (blk[rof] << 2) | ((TEST_FNUM >> 8) & 3)) },
            };
            double sa, sb;
            both_reset();
            both_prog(p, (int)(sizeof p / sizeof p[0]));
            env_render(g_ea, g_eb, ENVLEN);
            sa = eg_slope(g_ea); sb = eg_slope(g_eb);
            printf("  %2d  ", rate);
            if (sa > 0) printf("  %11.3f", sa); else printf("        too slow");
            if (sb > 0) printf("   %11.3f", sb); else printf("     too slow");
            if (sa > 0 && sb > 0) printf("  %7.3f", sa / sb); else printf("         ");
            if (sa > 0) printf("     %8.3f", rate + 4 * log2(sa)); else printf("             ");
            if (sb > 0) printf("     %8.3f", rate + 4 * log2(sb));
            printf("\n");
        }
    }
    printf("\n  A CONSTANT 'ref anchor' column means speed doubles every 4 rate steps;\n"
           "  its value is the rate at which the envelope moves one unit per sample.\n");
}

/* ============================================================================ *
 * EXPERIMENT J -- THE ATTACK CURVE.
 * Decay is linear in the attenuation domain, so one number (a slope) describes it.
 * Attack is NOT: it slows as it approaches full volume. Printing the inferred
 * attenuation against time says what the curve actually is, instead of tuning a
 * constant until one arbitrary checkpoint (the 90% time) happens to line up while
 * the shape stays wrong.
 * ============================================================================ */
/* Fitted exponential rate constant of the ATTENUATION during attack: env(t) decays
   like env0 * exp(-lambda t). Regressed over the middle of the curve, away from the
   start transient and away from the floor where the follower quantises. */
static double attack_lambda(const int16_t *s)
{
    double pk = 0, sx = 0, sy = 0, sxx = 0, sxy = 0, slope;
    int i, n = 0;
    for (i = 0; i + EG_WIN < ENVLEN; i += EG_WIN) { double e = env_pk(s, i, EG_WIN); if (e > pk) pk = e; }
    if (pk <= 0) return -1;
    for (i = 0; i + EG_WIN < ENVLEN; i += EG_WIN) {
        double e = env_pk(s, i, EG_WIN), env, y;
        if (e <= 0) continue;
        env = -20.0 * log10(e / pk) / 0.1875;       /* inferred attenuation, units */
        if (e < EG_FLOOR) continue;                 /* quantisation floor: unusable */
        if (env > 200.0) continue;                  /* still in the start transient */
        if (env < 20.0) break;
        y = log(env);
        sx += i; sy += y; sxx += (double)i * i; sxy += (double)i * y; n++;
    }
    if (n < EG_MINPTS) return -1;
    slope = (n * sxy - sx * sy) / (n * sxx - sx * sx);
    return slope < 0 ? -slope : -1;
}

/* The decay law derived by `egrate`, so attack can be expressed RELATIVE to it:
   the two share the same rate input, and what we need is the one number that says
   how much faster the attack runs than the decay at the same rate. */
#define EG_DIV_MEASURED 32768.0
static double decay_units_per_sample(int rate)
{
    return (4 + (rate & 3)) * (double)(1u << (rate >> 2)) / EG_DIV_MEASURED;
}

static void exp_attack(void)
{
    static const uint8_t blk[4] = { 0, 2, 4, 6 }, mlt[4] = { 15, 4, 1, 0 };
    int ar, rof;
    printf("  AR  rate    ours lambda    ref lambda   ref lambda / decay-slope\n");
    for (ar = 2; ar <= 7; ar++) for (rof = 0; rof < 4; rof++) {
        rv p[] = {
            { 0x01, 0x20 },
            { 0x20, (uint8_t)(0x20 | mlt[rof]) }, { 0x23, (uint8_t)(0x20 | mlt[rof]) },
            { 0x40, 0x3F }, { 0x43, 0x00 },
            { 0x60, 0xF0 }, { 0x63, (uint8_t)(ar << 4) },   /* AR=ar, DR=0        */
            { 0x80, 0x0F }, { 0x83, 0x0F },
            { 0xE0, 0x00 }, { 0xE3, 0x00 }, { 0xC0, 0x00 },
            { 0xA0, TEST_FNUM & 0xFF },
            { 0xB0, (uint8_t)(0x20 | (blk[rof] << 2) | ((TEST_FNUM >> 8) & 3)) },
        };
        int rate = ar * 4 + rof;
        double la, lb;
        both_reset();
        both_prog(p, (int)(sizeof p / sizeof p[0]));
        env_render(g_ea, g_eb, ENVLEN);
        la = attack_lambda(g_ea);
        lb = attack_lambda(g_eb);
        printf("  %2d  %3d  ", ar, rate);
        if (la > 0) printf("  %.6e", la); else printf("        (instant)");
        if (lb > 0) printf("   %.6e", lb); else printf("        (instant)");
        if (lb > 0) printf("        %8.4f", lb / decay_units_per_sample(rate));
        printf("\n");
    }
    printf("\n  A CONSTANT last column is the attack law: each sample, the attenuation\n"
           "  loses that fraction of ITSELF times the rate's decay step.\n");
}

/* ============================================================================ *
 * EXPERIMENT I -- RETRIGGER.
 * Does key-on force the attenuation back to silence, or does the attack resume
 * from wherever the note already was? The two sound completely different on
 * repeated notes, and it is the kind of thing that is easy to assume and wrong.
 * ============================================================================ */
static void exp_retrig(void)
{
    /* Slow attack (AR=6), key on, interrupt it partway, key on again. */
    rv p[] = {
        { 0x01, 0x20 }, { 0x20, 0x21 }, { 0x23, 0x21 },
        { 0x40, 0x3F }, { 0x43, 0x00 },
        { 0x60, 0xF0 }, { 0x63, 0x60 },           /* carrier AR=6, DR=0           */
        { 0x80, 0x0F }, { 0x83, 0x00 },           /* SL=0, RR=0 (hold on release) */
        { 0xE0, 0x00 }, { 0xE3, 0x00 }, { 0xC0, 0x00 },
        { 0xA0, TEST_FNUM & 0xFF },
    };
    const uint8_t kon  = (uint8_t)(0x20 | (TEST_BLOCK << 2) | ((TEST_FNUM >> 8) & 3));
    const uint8_t koff = (uint8_t)(       (TEST_BLOCK << 2) | ((TEST_FNUM >> 8) & 3));
    int half = RATE / 100;                        /* ~10 ms: partway up an AR=6   */
    both_reset();
    both_prog(p, (int)(sizeof p / sizeof p[0]));
    both_write(0xB0, kon);
    env_render(g_ea, g_eb, half);
    printf("  partway up:      ours %6.0f   ref %6.0f\n",
           env_pk(g_ea, half - EG_WIN, EG_WIN), env_pk(g_eb, half - EG_WIN, EG_WIN));
    both_write(0xB0, koff);
    both_write(0xB0, kon);
    env_render(g_ea, g_eb, EG_WIN * 2);
    printf("  right after re-key-on:  ours %6.0f   ref %6.0f\n",
           env_pk(g_ea, 0, EG_WIN), env_pk(g_eb, 0, EG_WIN));
    printf("  -> if 'ref' collapses to ~0, key-on RESETS the envelope to silence;\n"
           "     if it holds, the attack RESUMES from the current level.\n");
}

/* ============================================================================ *
 * EXPERIMENT F -- KEY SCALE LEVEL.  Sweep block with KSL on; the attenuation
 * added per octave is the derived quantity.
 * ============================================================================ */
static void exp_ksl(void)
{
    /* KSL depends only on BLOCK and the top four bits of F-num, never on MULT --
       so MULT is free to pull the low blocks back up to a frequency whose period
       fits inside the measurement window. Without that, block 0 is 3 Hz and the
       peak reading is taken over a fraction of one cycle. */
    static const uint16_t fn[4] = { 0x040, 0x100, 0x200, 0x3C0 };
    int ksl, blk, f;
    printf("  attenuation in dB below full scale; ERR is ours minus ref\n");
    for (ksl = 0; ksl < 4; ksl++) {
        printf("  KSL=%d      fnum:", ksl);
        for (f = 0; f < 4; f++) printf("      %03X        ", fn[f]);
        printf("\n");
        for (blk = 0; blk < 8; blk++) {
            printf("    block %d  ", blk);
            for (f = 0; f < 4; f++) {
                uint8_t m = (blk <= 2) ? 15 : 1;
                rv p[] = {
                    { 0x01, 0x20 }, { 0x20, (uint8_t)(0x20 | m) }, { 0x23, (uint8_t)(0x20 | m) },
                    { 0x40, 0x3F }, { 0x43, (uint8_t)(ksl << 6) },
                    { 0x60, 0xF0 }, { 0x63, 0xF0 },
                    { 0x80, 0x0F }, { 0x83, 0x0F },
                    { 0xE0, 0x00 }, { 0xE3, 0x00 }, { 0xC0, 0x00 },
                    { 0xA0, (uint8_t)(fn[f] & 0xFF) },
                    { 0xB0, (uint8_t)(0x20 | (blk << 2) | ((fn[f] >> 8) & 3)) },
                };
                double pa, pb, da, db;
                both_reset();
                both_prog(p, (int)(sizeof p / sizeof p[0]));
                both_render(NMAX);
                pa = peak_of(g_a, NSETTLE, NWIN); pb = peak_of(g_b, NSETTLE, NWIN);
                da = pa > 0 ? -20 * log10(pa / 4096.0) : 99;
                db = pb > 0 ? -20 * log10(pb / 4085.0) : 99;
                printf("%6.2f %6.2f %+6.2f  ", da, db, da - db);
            }
            printf("\n");
        }
    }
}

/* ============================================================================ *
 * EXPERIMENT K -- READ THE KSL ROM OUT OF THE ORACLE.
 * `ksl` above only sampled four F-num nibbles and they all happened to land on
 * whole ROM steps, which validates the LAW but says nothing about the 16-entry
 * table itself -- the entries between the octaves are exactly where a table copied
 * from documentation could be wrong without any of the other experiments noticing.
 * At KSL=3 and block 7 the attenuation is the ROM entry directly, so this reads the
 * table back one entry at a time.
 * ============================================================================ */
static void exp_kslrom(void)
{
    int n;
    printf("  nibble   ref dB    implied ROM   our ROM   delta\n");
    for (n = 0; n < 16; n++) {
        uint16_t f = (uint16_t)(n * 64 + 32);
        rv p[] = {
            { 0x01, 0x20 }, { 0x20, 0x21 }, { 0x23, 0x21 },
            { 0x40, 0x3F }, { 0x43, 0xC0 },               /* KSL=3, TL=0          */
            { 0x60, 0xF0 }, { 0x63, 0xF0 },
            { 0x80, 0x0F }, { 0x83, 0x0F },
            { 0xE0, 0x00 }, { 0xE3, 0x00 }, { 0xC0, 0x00 },
            { 0xA0, (uint8_t)(f & 0xFF) },
            { 0xB0, (uint8_t)(0x20 | (7 << 2) | ((f >> 8) & 3)) },
        };
        double pb, db, rom;
        both_reset();
        both_prog(p, (int)(sizeof p / sizeof p[0]));
        both_render(NMAX);
        pb = peak_of(g_b, NSETTLE, NWIN);
        db = pb > 0 ? -20 * log10(pb / 4085.0) : 99;
        /* block 7 leaves raw = rom - 8, and one ROM unit is 0.7526 dB */
        rom = db / (20 * log10(2.0) / 8) + 8;
        /* Entry 0 is UNOBSERVABLE: block 7 subtracts 8, so any ROM value at or
           below 8 clamps to zero attenuation and reads back the same. The +8
           printed there is the clamp, not a discrepancy. */
        printf("  %4d   %7.2f      %8.2f  %8d   %+6.2f%s\n", n, db, rom,
               opl_kslrom_probe[n], rom - opl_kslrom_probe[n],
               n == 0 ? "   (clamped: unobservable)" : "");
    }
}

/* ============================================================================ *
 * EXPERIMENT G -- MULT.  Each multiplier must land the fundamental on the same
 * harmonic in both cores; a mismatch detunes an instrument without changing
 * anything else, which is exactly the "sounds wrong but plays the right tune"
 * symptom.
 * ============================================================================ */
static void exp_mult(void)
{
    int m;
    printf("  MULT   ours peak-bin   ref peak-bin   (bin = harmonic of the test note)\n");
    for (m = 0; m < 16; m++) {
        rv p[] = {
            { 0x01, 0x20 }, { 0x20, 0x21 }, { 0x23, (uint8_t)(0x20 | m) },
            { 0x40, 0x3F }, { 0x43, 0x00 },
            { 0x60, 0xF0 }, { 0x63, 0xF0 },
            { 0x80, 0x0F }, { 0x83, 0x0F },
            { 0xE0, 0x00 }, { 0xE3, 0x00 }, { 0xC0, 0x00 },
            { 0xA0, TEST_FNUM & 0xFF },
            { 0xB0, (uint8_t)(0x20 | (TEST_BLOCK << 2) | ((TEST_FNUM >> 8) & 3)) },
        };
        int k, ka = 0, kb = 0; double va = 0, vb = 0;
        both_reset();
        both_prog(p, (int)(sizeof p / sizeof p[0]));
        both_render(NMAX);
        for (k = 1; k <= 32; k++) {
            double x = harm(g_a, k), y = harm(g_b, k);
            if (x > va) { va = x; ka = k; }
            if (y > vb) { vb = y; kb = k; }
        }
        printf("  %2d    %5d (%.0f)      %5d (%.0f)   %s\n", m, ka, va, kb, vb,
               ka == kb ? "" : "  <-- MISMATCH");
    }
}

int main(int argc, char **argv)
{
    const char *what = (argc > 1) ? argv[1] : "all";
    int all = strcmp(what, "all") == 0;

    if (all || !strcmp(what, "validate")) {
        printf("== VALIDATE THE INSTRUMENT ==\n");
        if (exp_validate()) printf("  *** VALIDATION FAILED -- do not trust anything below\n");
        printf("\n");
    }
    if (all || !strcmp(what, "tl"))   { printf("== A. TOTAL LEVEL ==\n");       exp_tl();   printf("\n"); }
    if (all || !strcmp(what, "mod"))  { printf("== B. MODULATION INDEX ==\n");  exp_mod();  printf("\n"); }
    if (all || !strcmp(what, "fb"))   { printf("== C. FEEDBACK ==\n");          exp_fb();   printf("\n"); }
    if (all || !strcmp(what, "wave")) { printf("== D. WAVEFORMS ==\n");         exp_wave(); printf("\n"); }
    if (all || !strcmp(what, "mult")) { printf("== G. MULT ==\n");              exp_mult(); printf("\n"); }
    if (all || !strcmp(what, "ksl"))  { printf("== F. KEY SCALE LEVEL ==\n");   exp_ksl();  printf("\n"); }
    if (all || !strcmp(what, "kslrom")) { printf("== K. KSL ROM READ-OUT ==\n"); exp_kslrom(); printf("\n"); }
    if (all || !strcmp(what, "env"))  { printf("== E. ENVELOPE RATES ==\n");    exp_env();  printf("\n"); }
    if (all || !strcmp(what, "egrate")) { printf("== H. ENVELOPE RATE LAW ==\n"); exp_egrate(); printf("\n"); }
    if (all || !strcmp(what, "retrig")) { printf("== I. RETRIGGER ==\n");         exp_retrig(); printf("\n"); }
    if (all || !strcmp(what, "attack")) { printf("== J. ATTACK CURVE ==\n");       exp_attack(); printf("\n"); }
    return 0;
}
