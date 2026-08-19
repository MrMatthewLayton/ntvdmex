/* opl_synth_test.c -- off-VM battery for the OPL2 FM core (vdd_opl_synth.c).
 *
 * These are PROPERTY tests, not golden-sample comparisons. The core is written
 * from the documented YM3812 behaviour rather than ported, so bit-exactness with
 * real silicon is not the goal and asserting it would only encode my own errors.
 * What must be true for music to be usable is testable directly:
 *
 *   - pitch is CORRECT (measured by counting zero crossings, then compared with
 *     the chip's published formula f = fnum * 49716 / 2^(20-block))
 *   - a key-on produces sound and a key-off eventually produces silence
 *   - louder settings really are louder (total level attenuates monotonically)
 *   - FM actually modulates: a modulator at non-zero level changes the carrier's
 *     waveform rather than being ignored
 *   - nothing ever exceeds int16, whatever the register soup
 */
#include <stdio.h>
#include <string.h>
#include "vdd_opl.h"

void vdd_opl_render(opl_state *st, int16_t *out, uint32_t frames);

static int total = 0, fails = 0;
#define CHECK(c,m) do{ total++; if(c){printf("  PASS  %s\n",(m));} \
    else{printf("  FAIL  %s\n",(m)); fails++;} }while(0)

#define OPL_NATIVE_HZ 49716
static int16_t buf[OPL_NATIVE_HZ];              /* one second                     */

static opl_state opl;

/* Operator index -> register offset. The banks skip 0x06/0x07 and 0x0E/0x0F. */
static uint8_t opreg(int op) { return (uint8_t)(op + 2 * (op / 6)); }

/* Give an operator an instant attack that holds at full volume, so a test hears a
   steady tone rather than a transient, at the requested total level. */
static void set_op(int op, uint8_t tl)
{
    vdd_opl_write_reg(&opl, (uint8_t)(0x20 + opreg(op)), 0x21);   /* EGT=1 MULT=1  */
    vdd_opl_write_reg(&opl, (uint8_t)(0x40 + opreg(op)), tl);
    vdd_opl_write_reg(&opl, (uint8_t)(0x60 + opreg(op)), 0xF0);   /* AR=15 DR=0    */
    vdd_opl_write_reg(&opl, (uint8_t)(0x80 + opreg(op)), 0x0F);   /* SL=0  RR=15   */
}

/* Program a sustained tone on channel `c` and key it on. The MODULATOR is muted
   by default (TL max): without that it defaults to full volume and every test
   measures a two-operator blend instead of the thing it means to measure. */
static void note_on(int c, uint16_t fnum, uint8_t block, uint8_t tl)
{
    set_op(vdd_opl_op_index(c, 0), 0x3F);                          /* silent mod   */
    set_op(vdd_opl_op_index(c, 1), tl);                            /* carrier      */
    vdd_opl_write_reg(&opl, (uint8_t)(0xC0 + c), 0x01);            /* additive     */
    vdd_opl_write_reg(&opl, (uint8_t)(0xA0 + c), (uint8_t)(fnum & 0xFF));
    vdd_opl_write_reg(&opl, (uint8_t)(0xB0 + c),
                      (uint8_t)(0x20 | (block << 2) | ((fnum >> 8) & 3)));
}

/* Count positive-going zero crossings -> cycles -> Hz. */
static double measure_hz(const int16_t *s, int n)
{
    int i, cross = 0, prev = 0;
    for (i = 0; i < n; ++i) {
        int cur = s[i] > 0;
        if (cur && !prev) cross++;
        prev = cur;
    }
    return (double)cross * OPL_NATIVE_HZ / (double)n;
}
static long rms(const int16_t *s, int n)
{
    long long acc = 0; int i;
    for (i = 0; i < n; ++i) acc += (long long)s[i] * s[i];
    return (long)(acc / (n ? n : 1));
}

int main(void)
{
    double hz, want;
    long r_loud, r_quiet, r_off;

    printf("== sound epic: OPL2 FM synthesis battery ==\n");

    /* T1: pitch accuracy against the chip's published formula ---------------- */
    /* fnum=0x200, block=4 -> 512 * 49716 / 2^16 = 388.4 Hz                     */
    memset(&opl, 0, sizeof opl); vdd_opl_reset(&opl);
    note_on(0, 0x200, 4, 0);
    vdd_opl_render(&opl, buf, OPL_NATIVE_HZ / 4);
    hz   = measure_hz(buf, OPL_NATIVE_HZ / 4);
    want = 512.0 * OPL_NATIVE_HZ / 65536.0;
    printf("        measured %.1f Hz, expected %.1f Hz\n", hz, want);
    CHECK(hz > want * 0.98 && hz < want * 1.02, "pitch: fnum=0x200 block=4 within 2%");

    /* an octave up must double the frequency                                   */
    memset(&opl, 0, sizeof opl); vdd_opl_reset(&opl);
    note_on(0, 0x200, 5, 0);
    vdd_opl_render(&opl, buf, OPL_NATIVE_HZ / 4);
    hz = measure_hz(buf, OPL_NATIVE_HZ / 4);
    printf("        measured %.1f Hz, expected %.1f Hz\n", hz, want * 2.0);
    CHECK(hz > want * 2.0 * 0.98 && hz < want * 2.0 * 1.02, "pitch: block+1 is one octave up");

    /* a different fnum scales linearly                                         */
    memset(&opl, 0, sizeof opl); vdd_opl_reset(&opl);
    note_on(0, 0x100, 4, 0);
    vdd_opl_render(&opl, buf, OPL_NATIVE_HZ / 4);
    hz = measure_hz(buf, OPL_NATIVE_HZ / 4);
    CHECK(hz > want * 0.5 * 0.97 && hz < want * 0.5 * 1.03, "pitch: half the F-number is half the pitch");

    /* T2: key-on makes sound, key-off eventually silences -------------------- */
    memset(&opl, 0, sizeof opl); vdd_opl_reset(&opl);
    note_on(0, 0x200, 4, 0);
    vdd_opl_render(&opl, buf, 4096);
    r_loud = rms(buf, 4096);
    CHECK(r_loud > 10000, "key-on: channel produces signal");

    vdd_opl_write_reg(&opl, 0xB0, 0x10);                    /* key-off           */
    vdd_opl_render(&opl, buf, OPL_NATIVE_HZ / 2);           /* let release finish */
    vdd_opl_render(&opl, buf, 4096);
    r_off = rms(buf, 4096);
    CHECK(r_off < r_loud / 100, "key-off: decays to silence");

    /* T3: total level attenuates ------------------------------------------- */
    memset(&opl, 0, sizeof opl); vdd_opl_reset(&opl);
    note_on(0, 0x200, 4, 0x20);                             /* -24 dB            */
    vdd_opl_render(&opl, buf, 4096);
    r_quiet = rms(buf, 4096);
    CHECK(r_quiet < r_loud / 4 && r_quiet > 0, "total level: higher TL is quieter but audible");

    /* T4: FM actually modulates -------------------------------------------- */
    {
        long r_plain, r_fm;
        memset(&opl, 0, sizeof opl); vdd_opl_reset(&opl);
        note_on(0, 0x200, 4, 0);
        vdd_opl_write_reg(&opl, 0xC0, 0x00);                /* FM, modulator muted */
        vdd_opl_render(&opl, buf, 8192);
        r_plain = rms(buf, 8192);

        memset(&opl, 0, sizeof opl); vdd_opl_reset(&opl);
        note_on(0, 0x200, 4, 0);
        vdd_opl_write_reg(&opl, 0xC0, 0x00);
        set_op(vdd_opl_op_index(0, 0), 0x00);               /* modulator at full   */
        vdd_opl_write_reg(&opl, 0xB0, 0x00);                /* re-key so it starts */
        vdd_opl_write_reg(&opl, 0xB0, (uint8_t)(0x20 | (4 << 2) | 0x02));
        vdd_opl_render(&opl, buf, 8192);
        r_fm = rms(buf, 8192);
        printf("        plain rms=%ld, modulated rms=%ld\n", r_plain, r_fm);
        CHECK(r_fm != r_plain, "FM: a live modulator changes the carrier output");
    }

    /* T5: output never leaves int16, even with everything blaring ----------- */
    {
        int c, i, clipped = 0;
        memset(&opl, 0, sizeof opl); vdd_opl_reset(&opl);
        for (c = 0; c < OPL_NUM_CH; ++c) note_on(c, (uint16_t)(0x180 + c * 16), 5, 0);
        for (c = 0; c < OPL_NUM_CH; ++c) vdd_opl_write_reg(&opl, (uint8_t)(0xC0 + c), 0x0F);
        vdd_opl_render(&opl, buf, OPL_NATIVE_HZ / 8);
        for (i = 0; i < OPL_NATIVE_HZ / 8; ++i)
            if (buf[i] == 32767 || buf[i] == -32768) clipped++;
        CHECK(rms(buf, OPL_NATIVE_HZ / 8) > 0, "9 channels at once: still produces signal");
        printf("        %d of %d samples at the clip rail\n", clipped, OPL_NATIVE_HZ / 8);
        CHECK(clipped < OPL_NATIVE_HZ / 80, "9 channels at once: not permanently clipped");
    }

    /* T6: an untouched chip is silent --------------------------------------- */
    memset(&opl, 0, sizeof opl); vdd_opl_reset(&opl);
    vdd_opl_render(&opl, buf, 4096);
    CHECK(rms(buf, 4096) == 0, "reset: silent until a note is keyed on");

    printf("-- %d checks, %d failures --\n", total, fails);
    return fails ? 1 : 0;
}
