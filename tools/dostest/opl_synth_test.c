/* opl_synth_test.c -- off-VM battery for the OPL2/OPL3 FM core (vdd_opl_synth.c).
 *
 * T7-T12 are the OPL3 (#232). T7 is the one GOLDEN test in this file, and it is
 * the exception that proves the rule below: it does not claim the OPL2 output is
 * RIGHT, only that it is UNCHANGED -- that adding a second chip moved nothing in
 * the first. The rest are properties again: NEW gates array 1, C0 bits 4/5 route
 * left/right, a 4-op voice is heard only through its algorithm's carriers, and
 * waveforms 4-7 exist only with NEW set.
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

/* ── OPL3 helpers (#232) ──────────────────────────────────────────────────── */
static int16_t st_buf[2 * 8192];                /* interleaved L/R                */

/* Operator 0-35 -> 9-bit register offset: array 1 is 0x100 + the same layout.  */
static uint16_t opreg9(int op)
{
    int a = op / 18, s = op % 18;
    return (uint16_t)(a * 0x100 + s + 2 * (s / 6));
}
static void w9(uint16_t reg, uint8_t val) { vdd_opl_write_reg(&opl, reg, val); }
/* A steady full-level operator, or (live=0) one that never attacks: AR=0 holds
   it at full attenuation, so it outputs exactly 0 -- and so modulates nothing. */
static void op9(int op, int live, uint8_t wave)
{
    w9((uint16_t)(0x20 + opreg9(op)), 0x21);
    w9((uint16_t)(0x40 + opreg9(op)), 0x00);
    w9((uint16_t)(0x60 + opreg9(op)), live ? 0xF0 : 0x00);
    w9((uint16_t)(0x80 + opreg9(op)), 0x0F);
    w9((uint16_t)(0xE0 + opreg9(op)), wave);
}
/* Program channel c (0-17) and key it on: `c0` is the whole C0 byte. */
static void ch9(int c, uint8_t c0, uint16_t fnum, uint8_t block)
{
    uint16_t a = (uint16_t)((c / 9) * 0x100 + c % 9);
    w9((uint16_t)(0xC0 + a), c0);
    w9((uint16_t)(0xA0 + a), (uint8_t)(fnum & 0xFF));
    w9((uint16_t)(0xB0 + a), (uint8_t)(0x20 | (block << 2) | ((fnum >> 8) & 3)));
}
static void opl3_fresh(int newm)
{
    memset(&opl, 0, sizeof opl); opl.opl3 = 1; vdd_opl_reset(&opl);
    if (newm) w9(0x105, 0x01);
}
static long rms_side(const int16_t *s, int n, int side)
{
    long long acc = 0; int i;
    for (i = 0; i < n; ++i) acc += (long long)s[2 * i + side] * s[2 * i + side];
    return (long)(acc / (n ? n : 1));
}
static uint32_t fnv16(uint32_t h, const int16_t *s, int n)
{
    int i;
    for (i = 0; i < n; ++i) {
        h ^= (uint8_t)s[i];                    h *= 16777619u;
        h ^= (uint8_t)((uint16_t)s[i] >> 8);   h *= 16777619u;
    }
    return h;
}

/* ── THE OPL2 GOLDEN. A fixed register sequence touching every OPL2 feature the
     synth models -- 9 voices, feedback, both connections, the four waveforms
     (with WSE), KSL, AM/VIB at full depth, key-offs, rhythm bass drum and
     tom-tom -- rendered and hashed. The checksum was taken from the build BEFORE
     the OPL3 existed (42a9029); an OPL2 must still produce it sample for sample,
     and so must an OPL3 with NEW clear. `which` selects the render path:
     0 mono, 1 the left of the stereo render, 2 its right. */
#define OPL2_GOLDEN 0xA60B79B9u
static uint32_t g_hash;
static void g_eat(int which, int n)
{
    int i;
    if (which == 0) { vdd_opl_render(&opl, buf, (uint32_t)n); g_hash = fnv16(g_hash, buf, n); return; }
    vdd_opl_render_st(&opl, st_buf, (uint32_t)n);
    for (i = 0; i < n; ++i) buf[i] = st_buf[2 * i + (which - 1)];
    g_hash = fnv16(g_hash, buf, n);
}
static void g_w(unsigned r, unsigned v) { vdd_opl_write_reg(&opl, (uint8_t)r, (uint8_t)v); }
static uint32_t golden_run(int chip_opl3, int which)
{
    int c, k;
    memset(&opl, 0, sizeof opl); opl.opl3 = (uint8_t)chip_opl3; vdd_opl_reset(&opl);
    g_hash = 2166136261u;
    g_w(0x01, 0x20);                                 /* WSE                       */
    g_w(0xBD, 0xC0);                                 /* deep AM + VIB             */
    for (c = 0; c < 9; ++c) {
        int m = vdd_opl_op_index(c, 0), cr = vdd_opl_op_index(c, 1);
        unsigned mo = (unsigned)(m + 2 * (m / 6)), co = (unsigned)(cr + 2 * (cr / 6));
        g_w(0x20 + mo, 0x21 | ((c & 1) << 7) | ((c & 2) << 5) | (c & 4 ? 0x10 : 0) | (c % 5));
        g_w(0x20 + co, 0x21 | ((c & 2) << 6));
        g_w(0x40 + mo, (unsigned)(0x10 + c * 3) | ((c % 4) << 6));
        g_w(0x40 + co, (unsigned)(c * 2) | (((c + 1) % 4) << 6));
        g_w(0x60 + mo, 0xF0 - (unsigned)c * 0x11 + 3);
        g_w(0x60 + co, 0xD2 + (unsigned)c);
        g_w(0x80 + mo, 0x35 + (unsigned)c * 0x10);
        g_w(0x80 + co, 0x24 + (unsigned)c);
        g_w(0xE0 + mo, (unsigned)c & 3);
        g_w(0xE0 + co, (unsigned)(c + 1) & 3);
        g_w(0xC0 + c, (unsigned)((c * 3) & 0x0E) | (unsigned)(c & 1));
        g_w(0xA0 + c, 0x40 + (unsigned)c * 23);
        g_w(0xB0 + c, 0x20 | (unsigned)((2 + c % 5) << 2) | (unsigned)(c & 3));
        g_eat(which, 700);
    }
    g_eat(which, 6000);
    for (c = 0; c < 9; c += 2) { g_w(0xB0 + c, opl.reg[0xB0 + c] & ~0x20); g_eat(which, 900); }
    g_w(0xBD, 0xE0 | 0x10 | 0x04);                   /* rhythm: bass drum + tom   */
    g_eat(which, 4000);
    g_w(0xBD, 0xE0 | 0x0B);
    g_eat(which, 3000);
    g_w(0xBD, 0x00);
    for (k = 0; k < 8; ++k) g_eat(which, 4000);
    return g_hash;
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

    /* ══ OPL3 (YMF262), GH #232 ═══════════════════════════════════════════════ */

    /* T7: the OPL2 golden -- nothing the OPL3 added leaks into OPL2 output ---- */
    { uint32_t h;
      h = golden_run(0, 0); printf("        OPL2 mono  fnv=0x%08X (golden 0x%08X)\n", h, OPL2_GOLDEN);
      CHECK(h == OPL2_GOLDEN, "golden: OPL2 mono render bit-identical to the pre-OPL3 build");
      CHECK(golden_run(0, 1) == OPL2_GOLDEN && golden_run(0, 2) == OPL2_GOLDEN,
            "golden: OPL2 stereo render -- left AND right are that same signal");
      CHECK(golden_run(1, 0) == OPL2_GOLDEN && golden_run(1, 1) == OPL2_GOLDEN &&
            golden_run(1, 2) == OPL2_GOLDEN,
            "golden: an OPL3 with NEW clear is the OPL2, sample for sample"); }

    /* T8: NEW gates array 1 ------------------------------------------------- */
    {   long r_off, r_on;
        opl3_fresh(0);
        op9(vdd_opl_op_index(9, 0), 0, 0); op9(vdd_opl_op_index(9, 1), 1, 0);
        ch9(9, 0x31, 0x200, 4);                     /* array 1 ch 0: L+R, additive */
        vdd_opl_render_st(&opl, st_buf, 4096);
        r_off = rms_side(st_buf, 4096, 0) + rms_side(st_buf, 4096, 1);
        CHECK(opl.ch[9].keyon == 1 && r_off == 0,
              "NEW clear: an array-1 voice latches (keyed) but is SILENT");
        w9(0x105, 0x01);
        vdd_opl_render_st(&opl, st_buf, 4096);
        r_on = rms_side(st_buf, 4096, 0);
        CHECK(r_on > 10000 && rms_side(st_buf, 4096, 1) == r_on,
              "NEW set: the latched array-1 voice sounds (both sides, C0=0x31)");
        /* an OPL2 has no array 1 at all: the same writes produce nothing       */
        memset(&opl, 0, sizeof opl); vdd_opl_reset(&opl);
        op9(vdd_opl_op_index(9, 0), 0, 0); op9(vdd_opl_op_index(9, 1), 1, 0);
        ch9(9, 0x31, 0x200, 4);
        w9(0x105, 0x01);
        vdd_opl_render_st(&opl, st_buf, 4096);
        CHECK(rms_side(st_buf, 4096, 0) == 0 && !vdd_opl_new_mode(&opl),
              "OPL2: array-1 writes (and NEW) do nothing");
    }

    /* T9: stereo routing, C0 bits 4 (A = left) and 5 (B = right) ------------ */
    {   static const struct { uint8_t c0; int l, r; const char *m; } rt[] = {
            { 0x11, 1, 0, "stereo: C0 bit 4 (CHA) -> LEFT only" },
            { 0x21, 0, 1, "stereo: C0 bit 5 (CHB) -> RIGHT only" },
            { 0x31, 1, 1, "stereo: bits 4+5 -> both sides, equal" },
            { 0xC1, 0, 0, "stereo: CHC/CHD only (bits 6-7) -> silent: not wired on an SB16" },
            { 0x01, 0, 0, "stereo: NEW set, no routing bits -> silent" } };
        int k;
        for (k = 0; k < 5; ++k) {
            long l, r;
            opl3_fresh(1);
            op9(vdd_opl_op_index(2, 0), 0, 0); op9(vdd_opl_op_index(2, 1), 1, 0);
            ch9(2, rt[k].c0, 0x200, 4);
            vdd_opl_render_st(&opl, st_buf, 4096);
            l = rms_side(st_buf, 4096, 0); r = rms_side(st_buf, 4096, 1);
            CHECK((rt[k].l ? l > 10000 : l == 0) && (rt[k].r ? r > 10000 : r == 0) &&
                  (!(rt[k].l && rt[k].r) || l == r), rt[k].m);
        }
        /* NEW clear: routing bits are not looked at -- mono to both, as an OPL2 */
        opl3_fresh(0);
        op9(vdd_opl_op_index(2, 0), 0, 0); op9(vdd_opl_op_index(2, 1), 1, 0);
        ch9(2, 0x11, 0x200, 4);
        vdd_opl_render_st(&opl, st_buf, 4096);
        CHECK(rms_side(st_buf, 4096, 0) > 10000 &&
              rms_side(st_buf, 4096, 1) == rms_side(st_buf, 4096, 0),
              "stereo: NEW clear ignores C0 bits 4-5 -- mono to both sides");
        /* mono render of a left-only voice: the (L+R)/2 fold                     */
        opl3_fresh(1);
        op9(vdd_opl_op_index(2, 0), 0, 0); op9(vdd_opl_op_index(2, 1), 1, 0);
        ch9(2, 0x11, 0x200, 4);
        vdd_opl_render(&opl, buf, 4096);
        { long rm = rms(buf, 4096);
          CHECK(rm > 10000 / 4 && rm < 30000000, "mono render with NEW set: folds (L+R)/2"); }
    }

    /* T10: 4-operator voices -- the carrier set per algorithm --------------- *
     * Pair 0+3. One operator is made live at a time (the others never attack,
     * so they output 0 and modulate nothing): the voice is audible exactly when
     * that operator is one the algorithm SUMS, and silent when it only
     * modulates -- a modulator into a silent operator is silence.             */
    {   /* audible[alg][op]: alg = CNT(ch0)<<1 | CNT(ch3)                         */
        static const int audible[4][4] = {
            { 0, 0, 0, 1 },         /* 0,0  1->2->3->4                            */
            { 0, 1, 0, 1 },         /* 0,1  (1->2) + (3->4)                       */
            { 1, 0, 0, 1 },         /* 1,0  1 + (2->3->4)                         */
            { 1, 0, 1, 1 } };       /* 1,1  1 + (2->3) + 4                        */
        static const char *algname[4] = { "FM-FM", "FM-AM", "AM-FM", "AM-AM" };
        int alg, k, ops[4];
        ops[0] = vdd_opl_op_index(0, 0); ops[1] = vdd_opl_op_index(0, 1);
        ops[2] = vdd_opl_op_index(3, 0); ops[3] = vdd_opl_op_index(3, 1);
        for (alg = 0; alg < 4; ++alg) {
            int ok = 1;
            char msg[96];
            for (k = 0; k < 4; ++k) {
                int j; long l;
                opl3_fresh(1);
                w9(0x104, 0x01);
                for (j = 0; j < 4; ++j) op9(ops[j], j == k, 0);
                w9(0xC3, (uint8_t)(0x30 | (alg & 1)));   /* ch3: CNT2 (routing ignored) */
                ch9(0, (uint8_t)(0x30 | (alg >> 1)), 0x200, 4);
                vdd_opl_render_st(&opl, st_buf, 4096);
                l = rms_side(st_buf, 4096, 0);
                if (audible[alg][k] ? !(l > 10000) : (l != 0)) {
                    ok = 0;
                    printf("        %s: operator %d live -> rms %ld (want %s)\n",
                           algname[alg], k + 1, l, audible[alg][k] ? "sound" : "silence");
                }
            }
            snprintf(msg, sizeof msg, "4-op %s: heard only through its carrier set", algname[alg]);
            CHECK(ok, msg);
        }
        /* FM really chains: 1 -> 2 -> 3 -> 4 with all live differs from 4 alone */
        {   uint32_t hs;
            opl3_fresh(1); w9(0x104, 0x01);
            op9(ops[0], 0, 0); op9(ops[1], 0, 0); op9(ops[2], 0, 0); op9(ops[3], 1, 0);
            w9(0xC3, 0x30); ch9(0, 0x30, 0x200, 4);
            vdd_opl_render_st(&opl, st_buf, 8192);
            hs = fnv16(2166136261u, st_buf, 2 * 8192);
            opl3_fresh(1); w9(0x104, 0x01);
            op9(ops[0], 1, 0); op9(ops[1], 1, 0); op9(ops[2], 1, 0); op9(ops[3], 1, 0);
            w9(0xC3, 0x30); ch9(0, 0x30, 0x200, 4);
            vdd_opl_render_st(&opl, st_buf, 8192);
            CHECK(rms_side(st_buf, 8192, 0) > 0 && fnv16(2166136261u, st_buf, 2 * 8192) != hs,
                  "4-op FM-FM: live modulators 1-3 change operator 4's output");
        }
        /* The pairing needs NEW: same registers, NEW clear -> ch0 is a 2-op voice
           and only its own carrier (operator 2) is heard.                       */
        {   long l2;
            opl3_fresh(0); w9(0x104, 0x01);
            op9(ops[0], 0, 0); op9(ops[1], 0, 0); op9(ops[2], 0, 0); op9(ops[3], 1, 0);
            w9(0xC3, 0x30); ch9(0, 0x30, 0x200, 4);
            vdd_opl_render_st(&opl, st_buf, 4096);
            l2 = rms_side(st_buf, 4096, 0);
            CHECK(l2 == 0, "4-op needs NEW: with NEW clear, ch3's carrier is not keyed by ch0");
        }
        /* the second channel's own key-on does nothing while paired             */
        {   opl3_fresh(1); w9(0x104, 0x01);
            op9(ops[2], 0, 0); op9(ops[3], 1, 0);
            ch9(3, 0x31, 0x200, 4);
            vdd_opl_render_st(&opl, st_buf, 4096);
            CHECK(rms_side(st_buf, 4096, 0) == 0, "4-op: keying the SECOND channel sounds nothing");
        }
    }

    /* T11: waveforms 4-7 exist only with NEW set --------------------------- */
    {   uint32_t h_sine, h_w, h_w_new, h_sine_new;
        int w, zeros, i, flat;
        /* reference: a plain sine carrier, NEW clear, then NEW set              */
        opl3_fresh(0);
        op9(vdd_opl_op_index(1, 0), 0, 0); op9(vdd_opl_op_index(1, 1), 1, 0);
        ch9(1, 0x31, 0x200, 4);
        vdd_opl_render_st(&opl, st_buf, 4096); h_sine = fnv16(2166136261u, st_buf, 8192);
        opl3_fresh(1);
        op9(vdd_opl_op_index(1, 0), 0, 0); op9(vdd_opl_op_index(1, 1), 1, 0);
        ch9(1, 0x31, 0x200, 4);
        vdd_opl_render_st(&opl, st_buf, 4096); h_sine_new = fnv16(2166136261u, st_buf, 8192);
        for (w = 4; w < 8; ++w) {
            char msg[96];
            opl3_fresh(0);
            op9(vdd_opl_op_index(1, 0), 0, 0); op9(vdd_opl_op_index(1, 1), 1, (uint8_t)w);
            ch9(1, 0x31, 0x200, 4);
            vdd_opl_render_st(&opl, st_buf, 4096); h_w = fnv16(2166136261u, st_buf, 8192);
            opl3_fresh(1);
            op9(vdd_opl_op_index(1, 0), 0, 0); op9(vdd_opl_op_index(1, 1), 1, (uint8_t)w);
            ch9(1, 0x31, 0x200, 4);
            vdd_opl_render_st(&opl, st_buf, 4096); h_w_new = fnv16(2166136261u, st_buf, 8192);
            snprintf(msg, sizeof msg, "waveform %d: NEW clear plays wave %d (2 bits), NEW set plays %d",
                     w, w & 3, w);
            /* w & 3 == 0 for 4, so NEW clear == sine; 5-7 fold onto 1-3 (checked below) */
            CHECK((w != 4 || h_w == h_sine) && h_w_new != h_sine_new && h_w_new != h_w, msg);
        }
        /* waveform 5 with NEW clear is waveform 1                                */
        opl3_fresh(0);
        op9(vdd_opl_op_index(1, 0), 0, 0); op9(vdd_opl_op_index(1, 1), 1, 5);
        ch9(1, 0x31, 0x200, 4);
        vdd_opl_render_st(&opl, st_buf, 4096); h_w = fnv16(2166136261u, st_buf, 8192);
        opl3_fresh(0);
        op9(vdd_opl_op_index(1, 0), 0, 0); op9(vdd_opl_op_index(1, 1), 1, 1);
        ch9(1, 0x31, 0x200, 4);
        vdd_opl_render_st(&opl, st_buf, 4096);
        CHECK(h_w == fnv16(2166136261u, st_buf, 8192), "waveform 5 with NEW clear == waveform 1");
        /* shapes: 4 is silent for half of each cycle; 6 is a square, flat-topped */
        opl3_fresh(1);
        op9(vdd_opl_op_index(1, 0), 0, 0); op9(vdd_opl_op_index(1, 1), 1, 4);
        ch9(1, 0x31, 0x200, 4);                     /* 128 samples per cycle      */
        vdd_opl_render_st(&opl, st_buf, 4096);
        for (zeros = 0, i = 0; i < 4096; ++i) if (st_buf[2 * i] == 0) zeros++;
        printf("        wave 4: %d of 4096 samples silent\n", zeros);
        CHECK(zeros > 1900 && zeros < 2300, "waveform 4: silent through the second half-cycle");
        opl3_fresh(1);
        op9(vdd_opl_op_index(1, 0), 0, 0); op9(vdd_opl_op_index(1, 1), 1, 6);
        ch9(1, 0x31, 0x200, 4);
        vdd_opl_render_st(&opl, st_buf, 4096);
        /* from sample 8: sample 0 precedes the AR=15 attack's first tick        */
        for (flat = 0, i = 8; i < 4096; ++i)
            if (st_buf[2 * i] == st_buf[16] || st_buf[2 * i] == -st_buf[16]) flat++;
        printf("        wave 6: level %d, %d of 4088 samples at +-level\n", st_buf[16], flat);
        CHECK(st_buf[16] > 3000 && flat == 4088, "waveform 6: a square (one magnitude, both signs)");
        /* OPL2: WSE (0x01 bit 5) gates waveform select; the OPL3 has no WSE     */
        memset(&opl, 0, sizeof opl); vdd_opl_reset(&opl);
        op9(vdd_opl_op_index(1, 0), 0, 0); op9(vdd_opl_op_index(1, 1), 1, 2);
        ch9(1, 0x01, 0x200, 4);
        vdd_opl_render(&opl, buf, 4096);
        { int neg = 0; for (i = 0; i < 4096; ++i) if (buf[i] < 0) neg++;
          CHECK(neg > 1500, "OPL2, WSE clear: 0xE0=2 still plays a sine (negative half present)"); }
        memset(&opl, 0, sizeof opl); vdd_opl_reset(&opl);
        w9(0x01, 0x20);
        op9(vdd_opl_op_index(1, 0), 0, 0); op9(vdd_opl_op_index(1, 1), 1, 2);
        ch9(1, 0x01, 0x200, 4);
        vdd_opl_render(&opl, buf, 4096);
        { int neg = 0; for (i = 0; i < 4096; ++i) if (buf[i] < 0) neg++;
          CHECK(neg == 0, "OPL2, WSE set: waveform 2 (|sine|) -- no negative samples"); }
        opl3_fresh(0);
        op9(vdd_opl_op_index(1, 0), 0, 0); op9(vdd_opl_op_index(1, 1), 1, 2);
        ch9(1, 0x01, 0x200, 4);
        vdd_opl_render(&opl, buf, 4096);
        { int neg = 0; for (i = 0; i < 4096; ++i) if (buf[i] < 0) neg++;
          CHECK(neg == 0, "OPL3, NEW clear, no WSE: waveform 2 plays (no WSE on a YMF262)"); }
    }

    /* T12: 18 voices, full blast, stereo -- nothing leaves int16 ------------- */
    {   int c, i, clipped = 0;
        opl3_fresh(1);
        for (c = 0; c < OPL3_NUM_CH; ++c) {
            op9(vdd_opl_op_index(c, 0), 1, (uint8_t)(c & 7));
            op9(vdd_opl_op_index(c, 1), 1, (uint8_t)((c + 3) & 7));
            ch9(c, (uint8_t)(0x3F), (uint16_t)(0x180 + c * 16), 5);
        }
        vdd_opl_render_st(&opl, st_buf, 8192);
        for (i = 0; i < 2 * 8192; ++i) if (st_buf[i] == 32767 || st_buf[i] == -32768) clipped++;
        CHECK(rms_side(st_buf, 8192, 0) > 0 && rms_side(st_buf, 8192, 1) > 0,
              "18 channels at once: both sides carry signal");
        printf("        %d of %d stereo samples at the clip rail\n", clipped, 2 * 8192);
    }

    printf("-- %d checks, %d failures --\n", total, fails);
    return fails ? 1 : 0;
}
