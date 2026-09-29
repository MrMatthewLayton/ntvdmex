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
 *   Experiments: validate tl mod fb wave env ksl mult lfo rhythm all, and for the
 *   snare / hi-hat / cymbal (#139): rphase restart keyor noise drums
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
 * EXPERIMENT M -- RHYTHM MODE, MAPPED FROM THE OUTSIDE.
 *
 * With 0xBD bit 5 set, channels 6-8 stop being melodic voices and become five
 * percussion ones. WHICH operator belongs to which drum, and whether each is heard
 * directly or through FM, is exactly the sort of thing it would be easy to write
 * down from half-memory and get subtly wrong -- so this does not write it down at
 * all. It silences one operator at a time and watches which drum goes quiet. The
 * mapping falls out of the measurement.
 *
 * It also asks of each voice: is it TONAL or NOISE? Energy that lands on harmonics
 * of the channel's own frequency is tonal and can be reproduced by the ordinary
 * phase generator; energy spread off that grid cannot, and marks the voices that
 * need the chip's special phase logic.
 *
 * MEASURED NEED (edge counts from the Skyroads trace, the trustworthy instrument):
 *   hi-hat 548, bass drum 142, cymbal 138, snare 70, tom-tom 0, over 89.8 s
 *   starting at 19.8 s -- against 545 melodic notes on channels 0-3 only.
 * And the 10-second segment scores put the residual error exactly there: 0.96-0.97
 * before the drums come in, 0.26 in the quiet percussion-led passage after.
 * ============================================================================ */
static const char *g_drum[5]     = { "hi-hat", "cymbal", "tom-tom", "snare", "bass drum" };
static const uint8_t g_drumbit[5] = { 0x01, 0x02, 0x04, 0x08, 0x10 };
/* operators 12..17 live at register offsets 0x10..0x15 */
static const uint8_t g_rop[6]    = { 0x10, 0x11, 0x12, 0x13, 0x14, 0x15 };

static uint8_t g_rmult[6] = { 1, 1, 1, 1, 1, 1 };         /* per-operator MULT    */

static void rhythm_setup(uint8_t mute_off, uint8_t drumbits)
{
    int i;
    both_reset();
    both_write(0x01, 0x20);
    for (i = 0; i < 6; i++) {
        uint8_t o = g_rop[i];
        both_write((uint8_t)(0x20 + o), (uint8_t)(0x20 | g_rmult[i]));  /* EGT=1  */
        both_write((uint8_t)(0x40 + o), (uint8_t)(o == mute_off ? 0x3F : 0x00));
        both_write((uint8_t)(0x60 + o), 0xF0);            /* AR=15, DR=0          */
        both_write((uint8_t)(0x80 + o), 0x0F);            /* SL=0,  RR=15         */
        both_write((uint8_t)(0xE0 + o), 0x00);
    }
    for (i = 6; i < 9; i++) {
        both_write((uint8_t)(0xC0 + i), 0x00);            /* FM, no feedback      */
        both_write((uint8_t)(0xA0 + i), TEST_FNUM & 0xFF);
        /* NO key-on bit: in rhythm mode these voices are keyed from 0xBD */
        both_write((uint8_t)(0xB0 + i),
                   (uint8_t)((TEST_BLOCK << 2) | ((TEST_FNUM >> 8) & 3)));
    }
    both_write(0xBD, (uint8_t)(OPL_BD_RHY | drumbits));
}

/* Fraction of the signal's energy sitting on harmonics of the test note. Near 1 is
   tonal; near 0 is noise. */
static double tonality(const int16_t *s)
{
    double tot = 0, tone = 0;
    int i, k;
    for (i = 0; i < NWIN; i++) tot += (double)s[NSETTLE + i] * s[NSETTLE + i];
    tot /= NWIN;
    for (k = 1; k <= 60; k++) { double h = harm(s, k); tone += h * h / 2; }
    return tot > 0 ? tone / tot : 0;
}

static void exp_rhythm(void)
{
    int d, i;
    printf("  WHICH OPERATOR DRIVES WHICH DRUM (reference RMS with that operator at TL=63,\n"
           "  as a fraction of the RMS with all six at full volume)\n");
    printf("  drum        all on");
    for (i = 0; i < 6; i++) printf("   op%d", 12 + i);
    printf("\n");
    for (d = 0; d < 5; d++) {
        double base;
        rhythm_setup(0xFF, g_drumbit[d]);
        both_render(NMAX);
        base = rms_of(g_b, NSETTLE, NWIN);
        printf("  %-10s %7.0f", g_drum[d], base);
        for (i = 0; i < 6; i++) {
            rhythm_setup(g_rop[i], g_drumbit[d]);
            both_render(NMAX);
            printf("  %.2f", base > 0 ? rms_of(g_b, NSETTLE, NWIN) / base : 0.0);
        }
        printf("\n");
    }

    printf("\n  CHARACTER OF EACH VOICE (reference)\n");
    printf("  drum        RMS    peak   tonality   H1     H2     H3     H4\n");
    for (d = 0; d < 5; d++) {
        double h[5];
        int k;
        rhythm_setup(0xFF, g_drumbit[d]);
        both_render(NMAX);
        for (k = 1; k <= 4; k++) h[k] = harm(g_b, k);
        printf("  %-10s %6.0f %6.0f     %5.3f  %6.0f %6.0f %6.0f %6.0f\n",
               g_drum[d], rms_of(g_b, NSETTLE, NWIN), peak_of(g_b, NSETTLE, NWIN),
               tonality(g_b), h[1], h[2], h[3], h[4]);
    }

    /* WHICH PHASE ACCUMULATOR FEEDS WHICH VOICE. The mute test says which operator
       supplies a voice's ENVELOPE; it cannot say whose PHASE it runs on, and for
       the percussion voices those are not the same thing. Doubling one operator's
       MULT doubles its phase rate and nothing else, so whichever voices shift
       spectrally are the ones reading that accumulator. */
    printf("\n  WHOSE PHASE DOES EACH VOICE RUN ON (peak harmonic bin in the reference,\n"
           "  first with every MULT at 1, then doubling one operator's MULT at a time)\n");
    printf("  drum        base");
    for (i = 0; i < 6; i++) printf("   op%d", 12 + i);
    printf("\n");
    for (d = 0; d < 5; d++) {
        int basebin = 0, k;
        double v = 0;
        rhythm_setup(0xFF, g_drumbit[d]);
        both_render(NMAX);
        for (k = 1; k <= 60; k++) { double x = harm(g_b, k); if (x > v) { v = x; basebin = k; } }
        printf("  %-10s %4d", g_drum[d], basebin);
        for (i = 0; i < 6; i++) {
            int bin = 0;
            g_rmult[i] = 2;
            rhythm_setup(0xFF, g_drumbit[d]);
            both_render(NMAX);
            v = 0;
            for (k = 1; k <= 60; k++) { double x = harm(g_b, k); if (x > v) { v = x; bin = k; } }
            g_rmult[i] = 1;
            printf("  %4s", bin == basebin ? "." : "");
            if (bin != basebin) printf("\b\b\b\b%4d", bin);
        }
        printf("\n");
    }
    printf("  ('.' = unchanged, so that operator's phase does not reach this voice)\n");

    printf("\n  OURS vs REFERENCE, one voice at a time\n");
    /* Correlation is also reported at the BEST SMALL LAG. A waveform that is right
       but a sample or two early scores near zero at lag 0, which reads exactly like
       a wrong waveform -- and the fix for the two is completely different. */
    printf("  drum         ours RMS   ref RMS    ratio    corr    best corr @ lag\n");
    for (d = 0; d < 5; d++) {
        double ra, rb, xa = 0, xb = 0, xc = 0, best = -2;
        int j, lag, bestlag = 0;
        rhythm_setup(0xFF, g_drumbit[d]);
        both_render(NMAX);
        ra = rms_of(g_a, NSETTLE, NWIN); rb = rms_of(g_b, NSETTLE, NWIN);
        for (j = 0; j < NWIN; j++) {
            double x = g_a[NSETTLE + j], y = g_b[NSETTLE + j];
            xa += x * x; xb += y * y; xc += x * y;
        }
        for (lag = -16; lag <= 16; lag++) {
            double ca = 0, cb = 0, cc = 0, r;
            for (j = 32; j < NWIN - 32; j++) {
                double x = g_a[NSETTLE + j], y = g_b[NSETTLE + j + lag];
                ca += x * x; cb += y * y; cc += x * y;
            }
            r = (ca > 0 && cb > 0) ? cc / sqrt(ca * cb) : 0;
            if (r > best) { best = r; bestlag = lag; }
        }
        printf("  %-10s %9.0f %9.0f  %7.3f  %+6.3f    %+6.3f @ %+d\n", g_drum[d], ra, rb,
               rb > 0 ? ra / rb : 0.0,
               (xa > 0 && xb > 0) ? xc / sqrt(xa * xb) : 0.0, best, bestlag);
    }
}

/* ============================================================================ *
 * EXPERIMENT L -- THE TREMOLO AND VIBRATO LFOs.
 *
 * Both are currently no-ops: 0xBD is stored and never acted on. They are not
 * speculative gaps -- the per-note edge counters say 103 of Skyroads' 982 notes
 * start with tremolo and 149 with vibrato, and those counters count EDGES, unlike
 * the OR-over-the-run field that once produced a confident wrong answer about
 * rhythm mode.
 *
 * Three things have to come out of the oracle for each: the RATE, the DEPTH, and
 * the SHAPE. Shape matters as much as the other two -- a sine and a triangle of
 * equal rate and depth do not sound alike -- so both experiments print the raw
 * curve rather than only its summary statistics.
 * ============================================================================ */
#define LFO_STEP 128            /* one reading per cycle of the test note        */
#define LFO_LEN  (RATE * 8)     /* ~30 LFO cycles: enough to time one to 0.03%   */
#define VIB_STEP 1024           /* one vibrato step, so readings do not smear     */

/* Rate and peak-to-peak of a slow periodic curve, by counting how often it crosses
   its own mean going upwards. Robust to shape, which matters because we do not yet
   know whether these are sines or triangles. */
static void lfo_stats_step(const double *v, int n, int stride, double *rate, double *depth)
{
    double mn = 1e300, mx = -1e300, mean = 0;
    int i, first = -1, last = -1, cross = 0;
    for (i = 0; i < n; i++) { if (v[i] < mn) mn = v[i]; if (v[i] > mx) mx = v[i]; mean += v[i]; }
    mean /= n;
    for (i = 1; i < n; i++)
        if (v[i - 1] <= mean && v[i] > mean) {
            if (first < 0) first = i; else { last = i; cross++; }
        }
    *depth = mx - mn;
    *rate = (cross > 0 && last > first)
          ? (double)cross * RATE / ((double)(last - first) * stride) : 0.0;
}

static void lfo_stats(const double *v, int n, double *rate, double *depth)
{ lfo_stats_step(v, n, LFO_STEP, rate, depth); }

static void lfo_dump(const char *what, const double *v, int n)
{
    int i;
    printf("    %s over one cycle:", what);
    for (i = 0; i < n && i < 26; i++) {
        if (i % 13 == 0) printf("\n      ");
        printf("%8.3f", v[i]);
    }
    printf("\n");
}

static void exp_lfo(void)
{
    static double va[LFO_LEN / LFO_STEP], vb[LFO_LEN / LFO_STEP];
    int n = LFO_LEN / LFO_STEP, d, i;
    printf("  (test note is %d Hz; one reading per %d samples)\n",
           RATE / PERIOD, LFO_STEP);

    /* ---- TREMOLO: AM=1 on the carrier, depth bit DAM = 0xBD bit 7 ------------ */
    for (d = 0; d <= 1; d++) {
        rv p[] = {
            { 0x01, 0x20 }, { 0x20, (uint8_t)(0x20 | PARK_MULT) }, { 0x23, 0xA1 },
            { 0x40, PARK_TL }, { 0x43, 0x00 },      /* carrier AM=1, EGT=1, MULT=1 */
            { 0x60, 0xF0 }, { 0x63, 0xF0 },
            { 0x80, 0x0F }, { 0x83, 0x0F },
            { 0xE0, 0x00 }, { 0xE3, 0x00 }, { 0xC0, 0x01 },       /* additive      */
            { 0xA0, TEST_FNUM & 0xFF },
            { 0xB0, (uint8_t)(0x20 | (TEST_BLOCK << 2) | ((TEST_FNUM >> 8) & 3)) },
        };
        double ra, rb, da, db;
        both_reset();
        both_prog(p, (int)(sizeof p / sizeof p[0]));
        both_write(0xBD, (uint8_t)(d ? 0x80 : 0x00));
        env_render(g_ea, g_eb, LFO_LEN);
        for (i = 0; i < n; i++) {
            double ea = env_pk(g_ea, i * LFO_STEP, LFO_STEP);
            double eb = env_pk(g_eb, i * LFO_STEP, LFO_STEP);
            va[i] = ea > 0 ? 20 * log10(ea / 4096.0) : -99;
            vb[i] = eb > 0 ? 20 * log10(eb / 4085.0) : -99;
        }
        lfo_stats(va, n, &ra, &da);
        lfo_stats(vb, n, &rb, &db);
        printf("  TREMOLO DAM=%d   ours %6.3f Hz / %5.2f dB     ref %6.3f Hz / %5.2f dB\n",
               d, ra, da, rb, db);
        lfo_dump("ref amplitude, dB", vb, n);
        if (d) {
            /* COUNT THE STAIRCASE. The rate alone cannot separate a 52-step
               triangle from a 54-step one -- 4% apart, inside the measurement --
               but the steps are individually visible, so count them instead. */
            int steps = 0; double prev = vb[0];
            for (i = 1; i < n; i++) {
                if (vb[i] != prev) { steps++; prev = vb[i]; }
                if (steps && vb[i] == vb[0] && vb[i - 1] != vb[0]) break;
            }
            printf("    distinct levels before returning to the top: %d"
                   "  (one step per %d samples)\n", steps, LFO_STEP * i / (steps ? steps : 1));
        }
    }

    /* ---- VIBRATO: VIB=1 on the carrier, depth bit DVB = 0xBD bit 6 ----------- *
     * Swept over F-num as well as depth. The offset looks like a shift of the
     * F-number, and a shift QUANTISES: if the law really is fnum >> 9 then an
     * F-num below 512 gets no vibrato at all at DVB=0. That is a sharp, falsifiable
     * prediction, and it is the kind of edge a depth measured at one pitch would
     * sail straight past.                                                         */
    /* 0x100 is deliberately absent: at that F-num the test note is 256 samples a
       cycle, so a reading window holds too few zero crossings to resolve a 7-cent
       shift. An unresolvable reading is not evidence, and the earlier run of this
       sweep printed 121 cents there -- which is my instrument, not the chip. */
    { static const uint16_t vfn[2] = { 0x200, 0x3C0 };
      int fi;
      for (fi = 0; fi < 2; fi++)
    for (d = 0; d <= 1; d++) {
        rv p[] = {
            { 0x01, 0x20 }, { 0x20, (uint8_t)(0x20 | PARK_MULT) }, { 0x23, 0x61 },
            { 0x40, PARK_TL }, { 0x43, 0x00 },      /* carrier VIB=1, EGT=1        */
            { 0x60, 0xF0 }, { 0x63, 0xF0 },
            { 0x80, 0x0F }, { 0x83, 0x0F },
            { 0xE0, 0x00 }, { 0xE3, 0x00 }, { 0xC0, 0x01 },
            { 0xA0, (uint8_t)(vfn[fi] & 0xFF) },
            { 0xB0, (uint8_t)(0x20 | (TEST_BLOCK << 2) | ((vfn[fi] >> 8) & 3)) },
        };
        double ra, rb, da, db;
        both_reset();
        both_prog(p, (int)(sizeof p / sizeof p[0]));
        both_write(0xBD, (uint8_t)(d ? 0x40 : 0x00));
        env_render(g_ea, g_eb, LFO_LEN);
        /* Instantaneous pitch from interpolated upward zero crossings, averaged
           over each reading window; expressed in cents against the mean. */
        { int c;
          for (c = 0; c < 2; c++) {
            const int16_t *s = c ? g_eb : g_ea;
            double *out = c ? vb : va, mean = 0;
            int k;
            for (k = 0; k < LFO_LEN / VIB_STEP; k++) {
                int j, base = k * VIB_STEP, nx = 0; double f0 = -1, f1 = -1;
                for (j = base + 1; j < base + VIB_STEP && j < LFO_LEN; j++)
                    if (s[j - 1] < 0 && s[j] >= 0) {
                        double frac = (double)(-s[j - 1]) / (double)(s[j] - s[j - 1]);
                        double t = (j - 1) + frac;
                        if (f0 < 0) f0 = t; else { f1 = t; nx++; }
                    }
                out[k] = (nx > 0 && f1 > f0) ? (double)nx * RATE / (f1 - f0) : 0;
                mean += out[k];
            }
            mean /= (LFO_LEN / VIB_STEP);
            for (k = 0; k < LFO_LEN / VIB_STEP; k++)
                out[k] = out[k] > 0 ? 1200.0 * log2(out[k] / mean) : 0;
          } }
        lfo_stats_step(va, LFO_LEN / VIB_STEP, VIB_STEP, &ra, &da);
        lfo_stats_step(vb, LFO_LEN / VIB_STEP, VIB_STEP, &rb, &db);
        printf("  VIBRATO fnum=%03X DVB=%d   ours %6.3f Hz / %6.2f cents   "
               "ref %6.3f Hz / %6.2f cents  (fnum>>9 = %d, fnum>>8 = %d)\n",
               vfn[fi], d, ra, da, rb, db, vfn[fi] >> 9, vfn[fi] >> 8);
    } }
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

/* ============================================================================ *
 * EXPERIMENTS N, N2, O, P, Q -- SNARE, HI-HAT AND CYMBAL (#139).
 *
 * Experiment M says WHICH operators these three use; these say WHAT they do with
 * them, still strictly from the outside.
 *
 * THE INSTRUMENT: A PHASE READ-BACK. An operator at full level with a held
 * envelope outputs a known function of its 10-bit phase index, one per waveform.
 * Measure that function on the tom-tom -- an ordinary operator, driven at exactly
 * one index step per sample -- for all eight waveforms (OPL3, NEW set). Then run
 * a drum eight times, identically but for its waveform, and at every sample pick
 * the index whose eight tabulated outputs are nearest the eight observed ones.
 * That is the phase the drum USED, sample by sample. Tolerant (nearest, not
 * equal) because the idle operators add a few LSBs of offset; a sample whose best
 * match is still far off is reported as undecodable rather than guessed.
 * ⚠ Near a sine peak several indices give the same eight outputs (the table is
 *   flat there), so 0x300 can read back as 0x301-0x304. The rules below are
 *   checked with that one known ambiguity allowed and nothing else.
 * ============================================================================ */
typedef struct { uint8_t reg, val; int at; } tev_t;      /* a write at sample `at` */

typedef struct {
    uint8_t  bd;                    /* 0xBD drum bits                              */
    uint16_t f7, f8;                /* channel 7 / 8 F-number                      */
    uint8_t  b7, b8;                /* ... and block                               */
    uint8_t  m13, m17;              /* op13 / op17 MULT                            */
    uint8_t  ws;                    /* waveform for all six rhythm operators       */
    uint8_t  newm;                  /* OPL3 with NEW set (waveforms 4-7)           */
    uint8_t  tlmask;                /* bit i: operator 12+i at TL=63               */
    uint8_t  ad, sr, egt;           /* 0x60 / 0x80 values, EGT                     */
    uint8_t  bdhi;                  /* DAM/DVB bits for 0xBD                       */
    uint8_t  amvib;                 /* 0x80/0x40 bits for all six 0x20 registers   */
    int      key_at;                /* sample at which the 0xBD drum bits go on    */
} dcfg_t;

static const dcfg_t DCFG0 = { 0, 0x200, 0x200, 4, 4, 1, 1, 0, 0, 0, 0xF0, 0x0F, 1, 0, 0, 0 };

static void both_write9(uint16_t reg, uint8_t val)
{
    vdd_opl_write_reg(&g_ours, reg, val);
    OPL3_WriteReg(&g_ref, reg, val);
}

static const uint8_t g_mult2[16] = { 1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 20, 24, 24, 30, 30 };
static tev_t g_dev[64];
static int   g_ndev;

/* Program a drum setup into both cores and render n samples of it, plus any
   extra timed writes queued in g_dev. */
static void drum_run(const dcfg_t *c, int n)
{
    int i, e = 0;
    memset(&g_ours, 0, sizeof g_ours);
    g_ours.sample_hz = RATE; g_ours.ext_clock = 1; g_ours.opl3 = c->newm;
    vdd_opl_reset(&g_ours);
    OPL3_Reset(&g_ref, RATE);
    if (c->newm) both_write9(0x105, 1); else both_write(0x01, 0x20);
    for (i = 0; i < 6; i++) {
        uint8_t o = g_rop[i];
        uint8_t mult = (12 + i == 13) ? c->m13 : (12 + i == 17) ? c->m17 : 1;
        both_write((uint8_t)(0x20 + o), (uint8_t)(c->amvib | (c->egt ? 0x20 : 0) | mult));
        both_write((uint8_t)(0x40 + o), (uint8_t)((c->tlmask >> i) & 1 ? 0x3F : 0x00));
        both_write((uint8_t)(0x60 + o), c->ad);
        both_write((uint8_t)(0x80 + o), c->sr);
        both_write((uint8_t)(0xE0 + o), c->ws);
    }
    for (i = 6; i < 9; i++) {
        uint16_t f = i == 6 ? TEST_FNUM : i == 7 ? c->f7 : c->f8;
        uint8_t  b = i == 6 ? TEST_BLOCK : i == 7 ? c->b7 : c->b8;
        both_write((uint8_t)(0xC0 + i), c->newm ? 0x30 : 0x00);
        both_write((uint8_t)(0xA0 + i), (uint8_t)(f & 0xFF));
        both_write((uint8_t)(0xB0 + i), (uint8_t)((b << 2) | ((f >> 8) & 3)));
    }
    both_write(0xBD, (uint8_t)(c->bdhi | OPL_BD_RHY | (c->key_at ? 0 : c->bd)));
    for (i = 0; i < n; i++) {
        int16_t s = 0, buf[2] = { 0, 0 };
        if (c->key_at && i == c->key_at) both_write(0xBD, (uint8_t)(c->bdhi | OPL_BD_RHY | c->bd));
        while (e < g_ndev && g_dev[e].at == i) { both_write(g_dev[e].reg, g_dev[e].val); e++; }
        vdd_opl_render(&g_ours, &s, 1);
        OPL3_GenerateResampled(&g_ref, buf);
        g_a[i] = s; g_b[i] = buf[0];
    }
}

/* Best-lag correlation of ours against the reference over [from, from+n). */
static double lagcorr(size_t from, size_t n, int *lag_out, double *lag0)
{
    double best = -2; int lag, bestlag = 0;
    for (lag = -8; lag <= 8; lag++) {
        double ca = 0, cb = 0, cc = 0, r; size_t j;
        for (j = 16; j < n - 16; j++) {
            double x = g_a[from + j], y = g_b[from + j + lag];
            ca += x * x; cb += y * y; cc += x * y;
        }
        r = (ca > 0 && cb > 0) ? cc / sqrt(ca * cb) : (ca == 0 && cb == 0 ? 1.0 : 0.0);
        if (lag == 0 && lag0) *lag0 = r;
        if (r > best) { best = r; bestlag = lag; }
    }
    if (lag_out) *lag_out = bestlag;
    return best;
}

/* --- the phase read-back ------------------------------------------------------ */
#define RB_N 12000
static int16_t g_rbtab[2][8][1024];             /* [core][waveform][index]         */
static int16_t g_rbrun[2][8][RB_N];             /* [core][waveform][sample]        */
static int     g_rbph[2][RB_N];                 /* decoded index, -1 = undecodable */

static void readback_calibrate(void)
{
    dcfg_t c = DCFG0;
    int w, n, core, L[2];
    c.bd = 0x04; c.newm = 1; c.f8 = 0x200; c.b8 = 1;  /* tom: one index per sample */
    /* Align each core's table on its own: waveform 1 goes silent at index 512. */
    c.ws = 1; drum_run(&c, 4096);
    for (core = 0; core < 2; core++) {
        const int16_t *s = core ? g_b : g_a;
        for (n = 101; n < 4096; n++) if (s[n - 1] != 0 && s[n] == 0) break;
        L[core] = n - 512;
    }
    for (w = 0; w < 8; w++) {
        c.ws = (uint8_t)w; drum_run(&c, 4096);
        for (core = 0; core < 2; core++)
            for (n = 2048; n < 3072; n++)
                g_rbtab[core][w][(n - L[core]) & 1023] = (core ? g_b : g_a)[n];
    }
}

static void readback(const dcfg_t *base, int n)
{
    int w, i, k, core;
    dcfg_t c = *base;
    c.newm = 1;
    for (w = 0; w < 8; w++) {
        c.ws = (uint8_t)w; drum_run(&c, n);
        memcpy(g_rbrun[0][w], g_a, n * sizeof(int16_t));
        memcpy(g_rbrun[1][w], g_b, n * sizeof(int16_t));
    }
    for (core = 0; core < 2; core++)
        for (i = 0; i < n; i++) {
            int best = 1 << 30, bk = -1;
            for (k = 0; k < 1024; k++) {
                int s = 0;
                for (w = 0; w < 8 && s < best; w++) s += abs(g_rbtab[core][w][k] - g_rbrun[core][w][i]);
                if (s < best) { best = s; bk = k; }
            }
            g_rbph[core][i] = best > 400 ? -1 : bk;
        }
}

/* Our model of the index each voice uses, for checking the reference's against:
   the rules in vdd_opl_synth.c, restated here from the measurement so that the
   probe checks the synth rather than trusting it. `n` is the REFERENCE's sample. */
static uint32_t ref_index(const dcfg_t *c, int op, long n)
{
    uint32_t inc = op == 13 ? ((uint32_t)(c->f7 << c->b7) * g_mult2[c->m13]) >> 1
                            : ((uint32_t)(c->f8 << c->b8) * g_mult2[c->m17]) >> 1;
    return n < 0 ? 0 : (((uint32_t)n * inc) >> 10) & 1023;
}
static uint32_t rbit(uint32_t p13, uint32_t p17)
{
    return (((p13 >> 2) ^ (p13 >> 7)) | ((p13 >> 3) ^ (p17 >> 5)) | ((p17 >> 3) ^ (p17 >> 5))) & 1;
}
static uint32_t nstep(uint32_t w, int k)
{
    while (k-- > 0) w = (w >> 1) | ((((w >> 14) ^ w) & 1) << 22);
    return w;
}

/* EXPERIMENT N -- what phase each voice runs on, and the rule behind it. */
static void exp_rphase(void)
{
    static const char *nm[3] = { "hi-hat", "snare", "cymbal" };
    static const uint8_t bit[3] = { 0x01, 0x08, 0x02 };
    /* The FIT setup, and a HELD-OUT one the rules are then checked on. */
    dcfg_t fit = DCFG0, held = DCFG0;
    int v, i, n = RB_N, d13, d17;
    fit.f7 = 0x1A3; fit.b7 = 5; fit.m13 = 3; fit.f8 = 0x2F1; fit.b8 = 4; fit.m17 = 5;
    held.f7 = 0x0B7; held.b7 = 6; held.m13 = 1; held.f8 = 0x3E5; held.b8 = 2; held.m17 = 7;

    readback_calibrate();
    printf("  PHASE INDICES EACH VOICE USES (read back from the output; ref | ours)\n");
    for (v = 0; v < 3; v++) {
        int core;
        dcfg_t c = fit; c.bd = bit[v];
        readback(&c, n);
        for (core = 1; core >= 0; core--) {
            static int hist[1025];
            int k, shown = 0;
            memset(hist, 0, sizeof hist);
            for (i = 64; i < n; i++) hist[g_rbph[core][i] < 0 ? 1024 : g_rbph[core][i]]++;
            printf("  %-7s %-4s", core ? nm[v] : "", core ? "ref" : "ours");
            for (k = 0; k < 1025; k++)
                if (hist[k] && shown++ < 8) {
                    if (k == 1024) printf(" undecodable:%d", hist[k]);
                    else printf(" %03x:%d", k, hist[k]);
                }
            printf("\n");
        }
    }

    /* THE PHASE BIT. For the cymbal (no noise in it), which subset of the twenty
       accumulator bits -- op13's index bits 0-9, op17's 0-9 -- determines bit 9 of
       its phase, and at which sample offsets? Every subset of up to five bits is
       tried; "determines" means no two samples with equal inputs disagree. */
    printf("\n  WHICH ACCUMULATOR BITS DETERMINE THE CYMBAL'S PHASE BIT (fit setup)\n");
    {
        dcfg_t c = fit; c.bd = 0x02;
        static int tgt[RB_N];
        int found = 0;
        readback(&c, n);
        for (i = 0; i < n; i++) tgt[i] = g_rbph[1][i] < 0 ? -1 : (g_rbph[1][i] >> 9) & 1;
        for (d13 = 2; d13 <= 3; d13++) for (d17 = 2; d17 <= 3; d17++) {
            uint32_t a, b, cc, dd, ee;
            for (a = 0; a < 20; a++) for (b = a; b < 20; b++) for (cc = b; cc < 20; cc++)
            for (dd = cc; dd < 20; dd++) for (ee = dd; ee < 20; ee++) {
                uint32_t bits[5] = { a, b, cc, dd, ee };
                signed char tab[32];
                int bad = 0, j;
                memset(tab, -1, sizeof tab);
                for (i = 64; i < n && !bad; i++) {
                    uint32_t full, key = 0;
                    if (tgt[i] < 0) continue;
                    full = ref_index(&c, 13, i - d13) | (ref_index(&c, 17, i - d17) << 10);
                    for (j = 0; j < 5; j++) key |= ((full >> bits[j]) & 1) << j;
                    if (tab[key] < 0) tab[key] = (signed char)tgt[i];
                    else if (tab[key] != tgt[i]) bad = 1;
                }
                if (!bad) {
                    uint32_t m = 0;
                    for (j = 0; j < 5; j++) m |= 1u << bits[j];
                    printf("    offsets op13 -%d op17 -%d: op13 bits", d13, d17);
                    for (j = 0; j < 10; j++) if (m >> j & 1) printf(" %d", j);
                    printf(", op17 bits");
                    for (j = 10; j < 20; j++) if (m >> j & 1) printf(" %d", j - 10);
                    printf("\n");
                    found++;
                    if (found == 1) {
                        int k;
                        printf("      truth table, zero cells (p17.5 p17.3 p13.7 p13.3 p13.2):");
                        for (k = 0; k < 32; k++) {
                            uint32_t p13 = ((k & 1) << 2) | ((k >> 1 & 1) << 3) | ((k >> 2 & 1) << 7);
                            uint32_t p17 = ((k >> 3 & 1) << 3) | ((k >> 4 & 1) << 5);
                            if (!rbit(p13, p17)) printf(" %d%d%d%d%d", k >> 4 & 1, k >> 3 & 1,
                                                        k >> 2 & 1, k >> 1 & 1, k & 1);
                        }
                        printf("\n");
                    }
                }
            }
        }
        printf("    %d determining subset(s) of <= 5 bits\n", found);
    }

    /* THE RULES, checked on the HELD-OUT setup against the reference's read-back,
       with the noise predicted from the LFSR model (experiment O). Reference
       sample n <-> the model's window after 72*(n-2) steps from OPL_NOISE_SEED --
       ours is one output sample ahead of the reference for these voices. */
    printf("\n  RULES vs REFERENCE READ-BACK, held-out setup (f7=%03x b7=%d m13=%d, f8=%03x b8=%d m17=%d)\n",
           held.f7, held.b7, held.m13, held.f8, held.b8, held.m17);
    for (v = 0; v < 3; v++) {
        dcfg_t c = held; c.bd = bit[v];
        int bad = 0, tot = 0, undec = 0;
        uint32_t wprev;
        readback(&c, n);
        {
            static uint32_t W[RB_N];
            W[2] = OPL_NOISE_SEED;                /* = reference sample 2's window  */
            for (i = 3; i < n; i++) W[i] = nstep(W[i - 1], 72);
            for (i = 64; i < n; i++) {
                uint32_t b, pred, got = (uint32_t)g_rbph[1][i];
                if (g_rbph[1][i] < 0) { undec++; continue; }
                wprev = W[i - 1];
                if (v == 0) {
                    b = rbit(ref_index(&c, 13, i - 2), ref_index(&c, 17, i - 3));
                    pred = (b << 9) | ((b ^ W[i]) & 1 ? 0x0D0 : 0x034);
                } else if (v == 1) {
                    b = (ref_index(&c, 13, i - 3) >> 8) & 1;
                    pred = (b << 9) | (((b ^ (wprev >> 6)) & 1) << 8);
                } else {
                    pred = (rbit(ref_index(&c, 13, i - 3), ref_index(&c, 17, i - 3)) << 9) | 0x080;
                }
                tot++;
                if (got != pred && !(pred == 0x300 && got > 0x300 && got <= 0x30A)) bad++;
            }
        }
        printf("    %-7s %d of %d samples mispredicted (%d undecodable)\n", nm[v], bad, tot, undec);
    }
}

/* EXPERIMENT N2 -- WHERE A KEY-ON RESTARTS THE ACCUMULATOR. The hi-hat and cymbal
   read two accumulators at once, so a restart one step early or late is not a
   harmless lag: it changes which bits co-occur. Key each at sample K (the other
   accumulator running since reset) and fit, from the read-back, the offset each
   accumulator is at: index = (n - K - keyed_offset)*inc for the keyed one,
   (n - free_offset)*inc for the other. Then show what is left over sample by
   sample once the synth models it (`drums` has the correlations). */
static void exp_restart(void)
{
    static const int Ks[5] = { 0, 1, 2, 37, 3001 };
    int v, ki, d13, d17, i, n = 8000;
    readback_calibrate();
    for (v = 0; v < 2; v++) for (ki = 0; ki < 5; ki++) {
        dcfg_t c = DCFG0;
        int K = Ks[ki];
        c.f7 = 0x1A3; c.b7 = 5; c.m13 = 3; c.f8 = 0x2F1; c.b8 = 4; c.m17 = 5;
        c.bd = v ? 0x02 : 0x01; c.key_at = K;
        readback(&c, n);
        for (d13 = -1; d13 <= 5; d13++) for (d17 = -1; d17 <= 5; d17++) {
            int bad = 0, tot = 0;
            static uint32_t W[RB_N];
            W[2] = OPL_NOISE_SEED;
            for (i = 3; i < n; i++) W[i] = nstep(W[i - 1], 72);
            for (i = K + 64; i < n; i++) {
                uint32_t p13, p17, b, pred;
                if (g_rbph[1][i] < 0) continue;
                p13 = v == 0 ? ref_index(&c, 13, i - K - d13) : ref_index(&c, 13, i - d13);
                p17 = v == 1 ? ref_index(&c, 17, i - K - d17) : ref_index(&c, 17, i - d17);
                b = rbit(p13, p17);
                pred = v == 0 ? ((b << 9) | ((b ^ W[i]) & 1 ? 0x0D0 : 0x034)) : ((b << 9) | 0x80);
                tot++; bad += pred != (uint32_t)g_rbph[1][i];
            }
            if (bad == 0) printf("  %-7s keyed at %4d: keyed accumulator offset %d, free one %d  (0 of %d wrong)\n",
                                 v ? "cymbal" : "hi-hat", K, v ? d17 : d13, v ? d13 : d17, tot);
        }
    }
    for (v = 0; v < 2; v++) {
        dcfg_t c = DCFG0; int bad = 0;
        c.key_at = 3001; c.f7 = 0x1A3; c.b7 = 5; c.f8 = 0x2F1; c.b8 = 4; c.bd = v ? 2 : 1;
        drum_run(&c, 8000);
        printf("  %-7s keyed at 3001, samples where |ours[t] - ref[t%+d]| > 40:", v ? "cymbal" : "hi-hat", 3 + v);
        for (i = 0; i < 7990; i++)
            if (abs(g_a[i] - g_b[i + 3 + v]) > 40 && bad++ < 8) printf(" t=%d (%d vs %d)", i, g_a[i], g_b[i + 3 + v]);
        printf("  [%d in all]\n", bad);
    }
}

/* EXPERIMENT Q -- A DRUM BIT AND ITS CHANNEL'S KEY BIT. Is an operator of channels
   6-8 keyed by its 0xBD drum bit, by its channel's B0 key bit, or by either?
   Decaying envelopes (DR=4) make a restart visible as a jump in level. */
static void exp_keyor(void)
{
    static const char *what[5] = {
        "B8 key on, rhythm on, TT bit set at 6000",
        "B8 key on, rhythm on, nothing else",
        "TT bit at 0, off at 5990, on again at 6000",
        "TT bit held from 0, B8 key set at 6000",
        "B7 key on, rhythm on, HH+SD bits set at 6000" };
    int mode, i;
    printf("  RMS over sample windows, reference | ours; then best-lag correlation\n");
    printf("  %-46s %-14s %-14s %-14s %-14s\n", "scenario", "100-600", "5400-5900", "6100-6600", "11000-11500");
    for (mode = 0; mode < 5; mode++) {
        static const int win[4] = { 100, 5400, 6100, 11000 };
        int k, lag;
        double r;
        memset(&g_ours, 0, sizeof g_ours);
        g_ours.sample_hz = RATE; g_ours.ext_clock = 1;
        vdd_opl_reset(&g_ours);
        OPL3_Reset(&g_ref, RATE);
        both_write(0x01, 0x20);
        for (i = 0; i < 6; i++) {
            uint8_t o = g_rop[i];
            both_write((uint8_t)(0x20 + o), 0x01); both_write((uint8_t)(0x40 + o), 0x00);
            both_write((uint8_t)(0x60 + o), 0xF4); both_write((uint8_t)(0x80 + o), 0xF4);
            both_write((uint8_t)(0xE0 + o), 0x00);
        }
        for (i = 6; i < 9; i++) {
            both_write((uint8_t)(0xC0 + i), 0x00);
            both_write((uint8_t)(0xA0 + i), TEST_FNUM & 0xFF);
            both_write((uint8_t)(0xB0 + i), (uint8_t)(((mode <= 1 && i == 8) || (mode == 4 && i == 7) ? 0x20 : 0) |
                                                      (TEST_BLOCK << 2) | ((TEST_FNUM >> 8) & 3)));
        }
        both_write(0xBD, (uint8_t)(OPL_BD_RHY | (mode == 2 || mode == 3 ? 0x04 : 0)));
        for (i = 0; i < 12000; i++) {
            int16_t s = 0, buf[2] = { 0, 0 };
            if (i == 5990 && mode == 2) both_write(0xBD, OPL_BD_RHY);
            if (i == 6000 && (mode == 0 || mode == 2)) both_write(0xBD, OPL_BD_RHY | 0x04);
            if (i == 6000 && mode == 3) both_write(0xB8, (uint8_t)(0x20 | (TEST_BLOCK << 2) | ((TEST_FNUM >> 8) & 3)));
            if (i == 6000 && mode == 4) both_write(0xBD, OPL_BD_RHY | 0x09);
            vdd_opl_render(&g_ours, &s, 1);
            OPL3_GenerateResampled(&g_ref, buf);
            g_a[i] = s; g_b[i] = buf[0];
        }
        printf("  %-46s", what[mode]);
        for (k = 0; k < 4; k++)
            printf(" %5.0f|%-5.0f   ", rms_of(g_b, win[k], 500), rms_of(g_a, win[k], 500));
        r = lagcorr(64, 11900, &lag, NULL);
        printf(" %+.4f @%+d\n", r, lag);
    }
    printf("  (rows 1 and 2 of the reference are identical: the TT bit does nothing to an\n"
           "   operator its channel key already holds; row 4: a B0 key written in rhythm\n"
           "   mode still keys ch8's operators, the tom-tom's restart and the cymbal.\n"
           "   Row 5's lower correlation is the MIX, not either voice: the reference\n"
           "   delivers the snare one sample after the hi-hat (experiment M's lags,\n"
           "   +4 vs +3) and two noise signals one sample apart do not correlate --\n"
           "   `drums` scores each voice alone)\n");
}

/* EXPERIMENT O -- the noise source. op13/op17 frozen (F-num 0) makes the phase
   bit 0, so the hi-hat's and snare's phases carry only the noise. */
static int bm_complexity(const int *s, int n, int *taps, int *ntaps)
{
    static int C[512], B[512], T[512];
    int L = 0, m = 1, i, j;
    memset(C, 0, sizeof C); memset(B, 0, sizeof B);
    C[0] = B[0] = 1;
    for (i = 0; i < n; i++) {
        int d = s[i];
        for (j = 1; j <= L; j++) d ^= C[j] & s[i - j];
        if (!d) { m++; continue; }
        memcpy(T, C, sizeof T);
        for (j = 0; j + m < 512; j++) C[j + m] ^= B[j];
        if (2 * L <= i) { L = i + 1 - L; memcpy(B, T, sizeof B); m = 1; } else m++;
    }
    *ntaps = 0;
    for (j = 1; j <= L; j++) if (C[j]) taps[(*ntaps)++] = j;
    return L;
}

static void exp_noise(void)
{
    static int hh[RB_N], sd[RB_N];
    dcfg_t c = DCFG0;
    int i, n = 4000, taps[64], nt, L, k, from = 16;
    c.f7 = 0; c.f8 = 0;
    readback_calibrate();
    c.bd = 0x01; readback(&c, n);
    for (i = 0; i < n; i++) hh[i] = g_rbph[1][i] == 0x0D0 ? 1 : g_rbph[1][i] == 0x034 ? 0 : -1;
    c.bd = 0x08; readback(&c, n);
    for (i = 0; i < n; i++) sd[i] = g_rbph[1][i] == 0x100 ? 1 : g_rbph[1][i] == 0x000 ? 0 : -1;
    for (i = from; i < n; i++) if (hh[i] < 0 || sd[i] < 0) { printf("  undecodable sample %d\n", i); return; }
    printf("  hi-hat noise, reference samples 0-79: ");
    for (i = 0; i < 80; i++) putchar(hh[i] < 0 ? '?' : '0' + hh[i]);
    printf("\n  snare  noise, reference samples 0-79: ");
    for (i = 0; i < 80; i++) putchar(sd[i] < 0 ? '?' : '0' + sd[i]);
    printf("\n");
    L = bm_complexity(hh + from, n - from, taps, &nt);
    printf("  hi-hat: linear complexity %d, recurrence s[n] = XOR of s[n-k], k =", L);
    for (k = 0; k < nt; k++) printf(" %d", taps[k]);
    L = bm_complexity(sd + from, n - from, taps, &nt);
    printf("\n  snare:  linear complexity %d, recurrence s[n] = XOR of s[n-k], k =", L);
    for (k = 0; k < nt; k++) printf(" %d", taps[k]);
    printf("\n");

    /* Which plain trinomial LFSR, stepped how many times per sample, has that
       recurrence? Decimate each candidate and Berlekamp-Massey it. */
    printf("  trinomial u[i] = u[i-a] ^ u[i-23], stepped K times per sample, giving the same recurrence:\n");
    {
        static int u[23 + 150 * 200], dsq[150];
        int a, K, ok, tt[64], ntt;
        int want[64], nwant = nt;
        memcpy(want, taps, sizeof want);
        for (a = 1; a < 23; a++) {
            for (i = 0; i < 23; i++) u[i] = i == 0;
            for (i = 23; i < (int)(sizeof u / sizeof u[0]); i++) u[i] = u[i - a] ^ u[i - 23];
            for (K = 1; K < 200; K++) {
                for (i = 0; i < 150; i++) dsq[i] = u[i * K];
                bm_complexity(dsq, 150, tt, &ntt);
                ok = ntt == nwant;
                for (k = 0; ok && k < ntt; k++) ok = tt[k] == want[k];
                if (ok) printf("    a=%d K=%d\n", a, K);
            }
        }
    }

    /* Every K above fits EACH stream alone. What tells them apart is whether ONE
       register explains BOTH drums: solve (GF(2) elimination) for the 23-bit state
       that yields the hi-hat's stream at K steps per sample, then look for the
       snare's stream anywhere within +-3 samples' worth of steps of it. */
    {
        static const int Ks[5] = { 9, 18, 36, 72, 144 };
        static uint32_t mask[144 * 1200 + 1024];
        static uint8_t  u[144 * 1200 + 1024];
        int nk = 1000, ki;
        for (ki = 0; ki < 5; ki++) {
            int K = Ks[ki], j, cc, found = 0, rank = 0;
            uint32_t piv[23], pb[23], x = 0;
            int lim = K * nk + 4 * K;
            memset(piv, 0, sizeof piv);
            for (j = 0; j < lim; j++) mask[j] = j < 23 ? 1u << j : mask[j - 9] ^ mask[j - 23];
            for (i = from; i < nk; i++) {           /* hh[i] = u[K*i]                  */
                uint32_t m = mask[K * i], b = (uint32_t)hh[i];
                int p;
                for (p = 22; p >= 0; p--) if ((m >> p & 1) && piv[p]) { m ^= piv[p]; b ^= pb[p]; }
                if (!m) continue;
                for (p = 22; !(m >> p & 1); p--) ;
                piv[p] = m; pb[p] = b; rank++;
            }
            if (rank < 23) { printf("    K=%3d: underdetermined\n", K); continue; }
            for (j = 0; j < 23; j++) {              /* back-substitute                 */
                uint32_t m = piv[j], b = pb[j]; int q;
                for (q = 0; q < j; q++) if (m >> q & 1) b ^= (x >> q) & 1;
                if (b) x |= 1u << j;
            }
            for (j = 0; j < lim; j++) u[j] = j < 23 ? (uint8_t)(x >> j & 1) : u[j - 9] ^ u[j - 23];
            for (i = from; i < nk && (int)u[K * i] == hh[i]; i++) ;
            if (i < nk) { printf("    K=%3d: no single state explains the hi-hat\n", K); continue; }
            for (cc = -3 * K; cc <= 3 * K; cc++) {
                int ok = 1;
                for (i = 64; i < nk && ok; i++) ok = (int)u[K * i + cc] == sd[i];
                if (ok) { printf("    K=%3d: snare = the hi-hat's register %+d steps (%+.3f samples)\n",
                                 K, cc, (double)cc / K); found = 1; }
            }
            if (!found) printf("    K=%3d: the snare is NOT on the hi-hat's register (within +-3 samples)\n", K);
        }
    }

    /* And the seed. Straight out of reset our renderer holds OPL_NOISE_SEED, one
       set bit, and steps it 72 times before each sample; ours leads the reference
       by one sample on these voices, so reference sample 2's window IS that seed.
       Check both drums from there: hi-hat bit 0 of this sample's window, snare
       bit 6 of the previous sample's (66 steps behind). */
    {
        static uint32_t W[RB_N];
        int okh = 1, oks = 1;
        W[2] = OPL_NOISE_SEED;
        for (i = 3; i < n; i++) W[i] = nstep(W[i - 1], 72);
        for (i = 64; i < n; i++) {
            if ((int)(W[i] & 1) != hh[i]) okh = 0;
            if ((int)((W[i - 1] >> 6) & 1) != sd[i]) oks = 0;
        }
        printf("  seed %06X, 72 steps/sample: hi-hat %s, snare %s (reference samples 64-%d)\n",
               OPL_NOISE_SEED, okh ? "MATCHES every sample" : "does NOT match",
               oks ? "MATCHES every sample" : "does NOT match", n - 1);
    }
}

/* EXPERIMENT P -- the three voices against the reference, isolated and in harder
   company: other pitches, waveforms, real envelopes with key-off, LFOs, keying
   after a delay (so the noise and the accumulators have to have been running),
   and all five drums at once with the others muted by TL (so every interaction
   stays and only the output is isolated). */
static void exp_drums(void)
{
    /* The tom-tom rides along as a CONTROL: an ordinary operator, already matched,
       so whatever residual it shows in a scenario is not the phase generator's. */
    static const char *nm[4] = { "hi-hat", "snare", "cymbal", "tom-tom (control)" };
    static const uint8_t bit[4] = { 0x01, 0x08, 0x02, 0x04 };
    static const uint8_t opbit[4] = { 1 << 1, 1 << 4, 1 << 5, 1 << 2 };   /* op13 16 17 14 */
    struct { const char *what; dcfg_t c; int keyoff; } sc[12];
    int ns = 0, s, v, n = NMAX;
    dcfg_t c;

    c = DCFG0; sc[ns].what = "default (experiment M's setup)"; sc[ns].c = c; sc[ns++].keyoff = 0;
    c = DCFG0; c.f7 = 0x1A3; c.b7 = 5; c.m13 = 3; c.f8 = 0x2F1; c.b8 = 4; c.m17 = 5;
    sc[ns].what = "other pitches and MULTs"; sc[ns].c = c; sc[ns++].keyoff = 0;
    c = DCFG0; c.f7 = 0x0B7; c.b7 = 6; c.f8 = 0x3E5; c.b8 = 2; c.m17 = 7;
    sc[ns].what = "held-out pitches"; sc[ns].c = c; sc[ns++].keyoff = 0;
    c = DCFG0; c.ws = 2; sc[ns].what = "waveform 2 (OPL2, WSE)"; sc[ns].c = c; sc[ns++].keyoff = 0;
    c = DCFG0; c.ws = 3; sc[ns].what = "waveform 3 (OPL2, WSE)"; sc[ns].c = c; sc[ns++].keyoff = 0;
    c = DCFG0; c.ws = 5; c.newm = 1; sc[ns].what = "waveform 5 (OPL3, NEW)"; sc[ns].c = c; sc[ns++].keyoff = 0;
    c = DCFG0; c.ad = 0xA5; c.sr = 0x46; c.egt = 0; sc[ns].what = "AR10 DR5 SL4 RR6, key-off at 6000"; sc[ns].c = c; sc[ns++].keyoff = 6000;
    c = DCFG0; c.bdhi = 0xC0; c.amvib = 0xC0; sc[ns].what = "deep tremolo + vibrato"; sc[ns].c = c; sc[ns++].keyoff = 0;
    c = DCFG0; c.key_at = 3001; c.f7 = 0x1A3; c.b7 = 5; c.f8 = 0x2F1; c.b8 = 4;
    sc[ns].what = "keyed 3001 samples into rhythm mode"; sc[ns].c = c; sc[ns++].keyoff = 0;
    c = DCFG0; c.tlmask = 0; c.f7 = 0x1A3; c.b7 = 5; c.f8 = 0x2F1; c.b8 = 4;
    sc[ns].what = "all five drums keyed, others muted by TL"; sc[ns].c = c; sc[ns++].keyoff = -1;

    printf("  best-lag correlation, ours vs reference, @ best lag, then the largest |ours - ref|\n"
           "  at the voice's own lag (0 = sample-exact; the 16-bit output's LSB noise is ~10)\n");
    printf("  %-42s %-21s %-21s %-21s %-21s\n", "scenario", nm[0], nm[1], nm[2], nm[3]);
    for (s = 0; s < ns; s++) {
        printf("  %-42s", sc[s].what);
        for (v = 0; v < 4; v++) {
            double r, ra, rb; int lag;
            c = sc[s].c;
            if (sc[s].keyoff == -1) { c.bd = 0x1F; c.tlmask = (uint8_t)(0x3F & ~opbit[v]); }
            else c.bd = bit[v];
            g_ndev = 0;
            if (sc[s].keyoff > 0) { g_dev[0].at = sc[s].keyoff; g_dev[0].reg = 0xBD;
                                    g_dev[0].val = (uint8_t)(c.bdhi | OPL_BD_RHY); g_ndev = 1; }
            drum_run(&c, n);
            g_ndev = 0;
            r = lagcorr(64, (size_t)n - 80, &lag, NULL);
            ra = rms_of(g_a, 64, (size_t)n - 80); rb = rms_of(g_b, 64, (size_t)n - 80);
            {   /* the largest sample difference at the voice's own lag (+3 hi-hat,
                   +4 snare/cymbal, from experiment M) -- 0 is sample-exact. A
                   constant or silent signal correlates at ANY lag, so this column
                   is the one that decides there. Skips 4 samples at a key edge. */
                int j, lg = (v == 0 || v == 3) ? 3 : 4, md = 0, ko = sc[s].keyoff > 0 ? sc[s].keyoff : -99;
                for (j = 64; j < n - 16; j++) {
                    int dd = abs(g_a[j] - g_b[j + lg]);
                    if (j >= ko - 4 && j <= ko + 4) continue;
                    if (c.key_at && j >= c.key_at - 4 && j <= c.key_at + 4) continue;
                    if (dd > md) md = dd;
                }
                if (ra < 16 && rb < 16) printf(" silent in both %5d  ", md);
                else printf(" %+.4f @%+d %5d  ", r, lag, md);
            }
        }
        printf("\n");
    }

    /* OPL3 routing: which channel's C0 sends each drum left or right. Route the
       channel under test LEFT and the other two rhythm channels RIGHT. */
    printf("\n  OPL3 ROUTING: RMS left/right with only the named channel routed left\n");
    {
        static const char *dn[5] = { "hi-hat", "cymbal", "tom-tom", "snare", "bass drum" };
        int rc, d, i;
        for (d = 0; d < 5; d++) {
            printf("    %-9s", dn[d]);
            for (rc = 6; rc <= 8; rc++) {
                double el = 0, er = 0, ol = 0, orr = 0;
                memset(&g_ours, 0, sizeof g_ours);
                g_ours.sample_hz = RATE; g_ours.ext_clock = 1; g_ours.opl3 = 1;
                vdd_opl_reset(&g_ours);
                OPL3_Reset(&g_ref, RATE);
                both_write9(0x105, 1);
                for (i = 0; i < 6; i++) {
                    uint8_t o = g_rop[i];
                    both_write((uint8_t)(0x20 + o), 0x21); both_write((uint8_t)(0x40 + o), 0x00);
                    both_write((uint8_t)(0x60 + o), 0xF0); both_write((uint8_t)(0x80 + o), 0x0F);
                    both_write((uint8_t)(0xE0 + o), 0x00);
                }
                for (i = 6; i < 9; i++) {
                    both_write((uint8_t)(0xC0 + i), i == rc ? 0x10 : 0x20);
                    both_write((uint8_t)(0xA0 + i), TEST_FNUM & 0xFF);
                    both_write((uint8_t)(0xB0 + i), (uint8_t)((TEST_BLOCK << 2) | ((TEST_FNUM >> 8) & 3)));
                }
                both_write(0xBD, (uint8_t)(OPL_BD_RHY | (1 << d)));
                for (i = 0; i < 8000; i++) {
                    int16_t b[2], s2[2];
                    OPL3_GenerateResampled(&g_ref, b);
                    vdd_opl_render_st(&g_ours, s2, 1);
                    if (i < 100) continue;
                    el += (double)b[0] * b[0];   er  += (double)b[1] * b[1];
                    ol += (double)s2[0] * s2[0]; orr += (double)s2[1] * s2[1];
                }
                printf("  ch%d left: ref %4.0f/%-4.0f ours %4.0f/%-4.0f", rc,
                       sqrt(el / 7900), sqrt(er / 7900), sqrt(ol / 7900), sqrt(orr / 7900));
            }
            printf("\n");
        }
    }

    /* Character, in our own output now: the tonality experiment M measured on the
       reference (0.509 / 0.003 / 0.749). */
    printf("\n  tonality (fraction of energy on harmonics of the test note), default setup\n");
    for (v = 0; v < 3; v++) {
        c = DCFG0; c.bd = bit[v];
        drum_run(&c, NMAX);
        printf("    %-7s ours %.3f  ref %.3f\n", nm[v], tonality(g_a), tonality(g_b));
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
    if (all || !strcmp(what, "lfo"))  { printf("== L. TREMOLO / VIBRATO LFOs ==\n"); exp_lfo(); printf("\n"); }
    if (all || !strcmp(what, "rhythm")) { printf("== M. RHYTHM MODE ==\n"); exp_rhythm(); printf("\n"); }
    if (all || !strcmp(what, "rphase")) { printf("== N. RHYTHM PHASE READ-BACK ==\n"); exp_rphase(); printf("\n"); }
    if (all || !strcmp(what, "restart")) { printf("== N2. RHYTHM KEY-ON RESTART POINT ==\n"); exp_restart(); printf("\n"); }
    if (all || !strcmp(what, "keyor"))  { printf("== Q. DRUM BIT OR CHANNEL KEY ==\n"); exp_keyor(); printf("\n"); }
    if (all || !strcmp(what, "noise"))  { printf("== O. RHYTHM NOISE ==\n"); exp_noise(); printf("\n"); }
    if (all || !strcmp(what, "drums"))  { printf("== P. SNARE / HI-HAT / CYMBAL vs REFERENCE ==\n"); exp_drums(); printf("\n"); }
    if (all || !strcmp(what, "env"))  { printf("== E. ENVELOPE RATES ==\n");    exp_env();  printf("\n"); }
    if (all || !strcmp(what, "egrate")) { printf("== H. ENVELOPE RATE LAW ==\n"); exp_egrate(); printf("\n"); }
    if (all || !strcmp(what, "retrig")) { printf("== I. RETRIGGER ==\n");         exp_retrig(); printf("\n"); }
    if (all || !strcmp(what, "attack")) { printf("== J. ATTACK CURVE ==\n");       exp_attack(); printf("\n"); }
    return 0;
}
