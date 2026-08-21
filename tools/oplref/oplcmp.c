/* oplcmp.c -- replay a captured OPL register trace through BOTH our synth and the
 * reference core, write two WAVs, and report where they differ.  DEV TOOL ONLY.
 *
 * WHY THIS EXISTS. "The music sounds a bit flat" is not a bug report you can act on,
 * and counting register writes cannot answer it either -- an earlier attempt did
 * exactly that and drew a confident wrong conclusion from an OR-over-the-run counter.
 * The only honest instrument is: feed the SAME register stream, with the SAME
 * timing, to our synth and to a core known to be right, and compare the samples.
 * Then "flat" becomes a specific parameter that is wrong, and every future change to
 * the synth has a regression target instead of an opinion.
 *
 * The reference is Nuked OPL3 (cycle-accurate YMF262, reconstructed from a die shot).
 * It is LGPL-2.1 and our synth is deliberately clean-room MIT, so it lives OUT OF
 * TREE in build/oplref/ (see fetch.sh) and is never linked into the host.
 *
 * Build:  tools/oplref/build.sh          Run:  build/oplref/oplcmp <trace.txt> [outdir]
 *
 * Trace format (written by the host under opltrace.flag): one `us reg val` per line,
 * hex, comments with '#'. The timestamps are the guest's REAL write times, so the
 * replay reproduces its phrasing -- most of what makes music sound like itself.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#include "../../src/vdd/vdd_opl.h"
#include "opl3.h"

#define RATE 49716              /* both cores render at the chip's native rate */
#define MAXEV 300000

typedef struct { uint32_t us; uint8_t reg, val; } ev_t;
static ev_t  g_ev[MAXEV];
static int   g_nev;

/* --- tiny WAV writer (mono 16-bit) ------------------------------------------ */
static void wav_write(const char *path, const int16_t *s, size_t n, int rate)
{
    FILE *f = fopen(path, "wb");
    uint32_t data = (uint32_t)(n * 2), riff = 36 + data, brate = (uint32_t)rate * 2;
    uint16_t one = 1, bits = 16, align = 2;
    uint32_t fmtsz = 16; uint32_t r = (uint32_t)rate;
    if (!f) { fprintf(stderr, "cannot write %s\n", path); return; }
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmtsz, 4, 1, f); fwrite(&one, 2, 1, f); fwrite(&one, 2, 1, f);
    fwrite(&r, 4, 1, f); fwrite(&brate, 4, 1, f); fwrite(&align, 2, 1, f);
    fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    fwrite(s, 2, n, f); fclose(f);
}

static int load_trace(const char *path)
{
    char line[256];
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return -1; }
    while (fgets(line, sizeof line, f)) {
        unsigned us, reg, val;
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        if (sscanf(line, "%x %x %x", &us, &reg, &val) != 3) continue;
        if (g_nev >= MAXEV) break;
        g_ev[g_nev].us = us; g_ev[g_nev].reg = (uint8_t)reg;
        g_ev[g_nev].val = (uint8_t)val; g_nev++;
    }
    fclose(f);
    return g_nev;
}

int main(int argc, char **argv)
{
    const char *outdir = (argc > 2) ? argv[2] : "build/oplref";
    char p1[512], p2[512];
    static opl_state ours;
    static opl3_chip ref;
    int16_t *a, *b;
    size_t total, i;
    uint32_t t0;
    int ie;
    double se = 0, ea = 0, eb = 0, cross = 0;
    long clipped = 0, silent_a = 0, silent_b = 0;

    if (argc < 2) { fprintf(stderr, "usage: oplcmp <trace.txt> [outdir]\n"); return 2; }
    if (load_trace(argv[1]) <= 0) { fprintf(stderr, "no events\n"); return 1; }

    t0 = g_ev[0].us;
    total = (size_t)(((double)(g_ev[g_nev - 1].us - t0) / 1e6) * RATE) + RATE / 2;
    a = calloc(total, sizeof *a);
    b = calloc(total, sizeof *b);
    if (!a || !b) { fprintf(stderr, "oom\n"); return 1; }

    memset(&ours, 0, sizeof ours);
    ours.sample_hz = RATE;
    ours.ext_clock = 1;                 /* we drive time; no bus frame tick here */
    vdd_opl_reset(&ours);
    OPL3_Reset(&ref, RATE);

    /* Replay: advance both cores to each event's timestamp, then apply the write to
       both. Identical input, identical timing -- any difference in the output is
       ours. */
    ie = 0;
    for (i = 0; i < total; i++) {
        uint32_t now_us = (uint32_t)(((double)i / RATE) * 1e6);
        while (ie < g_nev && (g_ev[ie].us - t0) <= now_us) {
            vdd_opl_write_reg(&ours, g_ev[ie].reg, g_ev[ie].val);
            OPL3_WriteReg(&ref, g_ev[ie].reg, g_ev[ie].val);   /* unbuffered: exact replay */
            ie++;
        }
        { int16_t s = 0; vdd_opl_render(&ours, &s, 1); a[i] = s; }
        { int16_t buf[2] = { 0, 0 }; OPL3_GenerateResampled(&ref, buf); b[i] = buf[0]; }
    }

    for (i = 0; i < total; i++) {
        double x = a[i], y = b[i];
        se += (x - y) * (x - y); ea += x * x; eb += y * y; cross += x * y;
        if (a[i] == 32767 || a[i] == -32768) clipped++;
        if (!a[i]) silent_a++;
        if (!b[i]) silent_b++;
    }

    /* ---- ENVELOPE COMPARISON (phase-insensitive) --------------------------- *
     * Raw waveform correlation collapses on a one-sample skew even when the sound
     * is identical, so it cannot tell "wrong timbre" from "right timbre, shifted".
     * Short-window RMS discards phase entirely and measures the thing "flat"
     * actually describes: how notes swell and decay. Read the two together --
     *   envelope HIGH + waveform LOW  -> dynamics right, HARMONICS wrong
     *   envelope LOW                  -> notes start/stop/decay differently        */
    {
#define WIN_MS 10
        size_t w = (size_t)(RATE * WIN_MS / 1000), nw = total / w, k;
        double *ra = calloc(nw, sizeof *ra), *rb = calloc(nw, sizeof *rb);
        double ma = 0, mb = 0, ca = 0, cb = 0, cc = 0;
        FILE *csv;
        char pc[512];
        for (k = 0; k < nw; k++) {
            double sa = 0, sb = 0; size_t j;
            for (j = 0; j < w; j++) { sa += (double)a[k*w+j]*a[k*w+j]; sb += (double)b[k*w+j]*b[k*w+j]; }
            ra[k] = sqrt(sa / w); rb[k] = sqrt(sb / w);
            ma += ra[k]; mb += rb[k];
        }
        ma /= nw; mb /= nw;
        for (k = 0; k < nw; k++) {
            double da = ra[k] - ma, db = rb[k] - mb;
            ca += da*da; cb += db*db; cc += da*db;
        }
        printf("envelope corr     %.4f  (%d ms windows, phase-insensitive)\n",
               (ca > 0 && cb > 0) ? cc / sqrt(ca * cb) : 0.0, WIN_MS);
        snprintf(pc, sizeof pc, "%s/envelope.csv", outdir);
        csv = fopen(pc, "w");
        if (csv) {
            fprintf(csv, "window_ms,ours_rms,ref_rms\n");
            for (k = 0; k < nw; k++)
                fprintf(csv, "%zu,%.1f,%.1f\n", k * WIN_MS, ra[k], rb[k]);
            fclose(csv);
            printf("wrote %s\n", pc);
        }
        free(ra); free(rb);
    }

    snprintf(p1, sizeof p1, "%s/ours.wav", outdir);
    snprintf(p2, sizeof p2, "%s/ref.wav", outdir);
    wav_write(p1, a, total, RATE);
    wav_write(p2, b, total, RATE);

    printf("events            %d\n", g_nev);
    printf("duration          %.2f s (%zu frames @ %d Hz)\n", (double)total / RATE, total, RATE);
    printf("RMS ours          %.1f\n", sqrt(ea / total));
    printf("RMS ref           %.1f\n", sqrt(eb / total));
    printf("level ratio       %.3f  (1.0 = same loudness)\n",
           eb > 0 ? sqrt(ea / eb) : 0.0);
    printf("correlation       %.4f  (1.0 = same waveform)\n",
           (ea > 0 && eb > 0) ? cross / sqrt(ea * eb) : 0.0);
    printf("RMS error         %.1f  (%.1f%% of reference)\n",
           sqrt(se / total), eb > 0 ? 100.0 * sqrt(se / eb) : 0.0);
    /* "Silent frames" counts EXACT zeros, and that is a trap: the reference carries
       a DC offset of a couple of LSBs, so it is almost never exactly zero even when
       it is inaudible. Counting near-silence too says whether a gap between the two
       is a real difference in what is sounding or just that offset. */
    { long near_a = 0, near_b = 0;
      for (i = 0; i < total; i++) {
          if (a[i] > -3 && a[i] < 3) near_a++;
          if (b[i] > -3 && b[i] < 3) near_b++;
      }
      printf("silent frames     ours %.1f%%  ref %.1f%%   (exactly zero)\n",
             100.0 * silent_a / total, 100.0 * silent_b / total);
      printf("near-silent       ours %.1f%%  ref %.1f%%   (|sample| < 3)\n",
             100.0 * near_a / total, 100.0 * near_b / total); }

    /* WHERE the error lives, not just how big it is. One number over 90 seconds
       averages a passage we reproduce well together with one we do not, and hides
       both. A per-segment breakdown points at the minute of music to go and look at. */
    {
        size_t seg = (size_t)RATE * 10, k, nseg = (total + seg - 1) / seg;
        printf("\nper-10s segment:\n  from   corr    level   ref RMS\n");
        for (k = 0; k < nseg; k++) {
            size_t s0 = k * seg, s1 = s0 + seg, j;
            double xa = 0, xb = 0, xc = 0;
            if (s1 > total) s1 = total;
            for (j = s0; j < s1; j++) {
                double x = a[j], y = b[j];
                xa += x * x; xb += y * y; xc += x * y;
            }
            printf("  %4zus  %6.4f  %6.3f  %8.1f\n", k * 10,
                   (xa > 0 && xb > 0) ? xc / sqrt(xa * xb) : 0.0,
                   xb > 0 ? sqrt(xa / xb) : 0.0,
                   sqrt(xb / (double)(s1 - s0)));
        }
    }
    if (clipped) printf("CLIPPED (ours)    %ld frames\n", clipped);
    printf("\nwrote %s and %s\n", p1, p2);
    return 0;
}
