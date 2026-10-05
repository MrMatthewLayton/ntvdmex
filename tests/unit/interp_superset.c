/* interp_superset.c -- "the new interpreter does everything the old one did, the same
 * way, and only ADDS". GH #194.
 *
 * WHY A SECOND MODE. interpfuzz.sh's digest compares HEAD and the working tree over
 * everything, which is exactly right for a change that must not alter behaviour -- and
 * useless for #194, whose whole point is that instructions which used to BAIL now run:
 * the digest differs on the first `66 9C`, and says nothing about whether anything ELSE
 * moved. This runs both interpreters in lockstep on the same random programs and state:
 *   - wherever the OLD one executed a step, the NEW one must execute it too, with an
 *     identical register file, IP, flags, memory writes and port traffic;
 *   - wherever the old one declined, the new one may run -- that step is counted by
 *     opcode (the coverage the new forms got) and rolled back, and the program ends.
 * A mismatch is printed with the bytes and both states. ALLOW="66:8C 66:E5" (opcode
 * keys, `66:` = under the operand-size prefix) names INTENDED changes, which are counted
 * but do not fail the run.
 *
 *   interp_superset [programs] [steps] [seed] [pm]       (pm=1: the fake PM table)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "interp_xcpu.h"

void ref_init(const uint8_t *img, int pm);  void new_init(const uint8_t *img, int pm);
void ref_poke(uint32_t lin, uint8_t v);     void new_poke(uint32_t lin, uint8_t v);
const uint8_t *ref_mem(void);               void new_sync(const uint8_t *img);
int  ref_step(xcpu *x, uint64_t *fx);       int  new_step(xcpu *x, uint64_t *fx);
void new_undo_begin(void);  int new_undo_rollback(void);  void new_undo_end(void);

static uint8_t IMG[XMEM_SIZE];
static uint64_t g_rng;
static uint32_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return (uint32_t)(g_rng >> 11); }

/* Opcode key: 0x10000 if 0x66 was among the prefixes, 0x0F00|op2 for the 0F map. */
static unsigned keyof(const uint8_t *m, uint32_t lin)
{
    unsigned osz = 0, k;
    for (k = 0; k < 5; ++k) {
        uint8_t b = m[(lin + k) % XMEM_SIZE];
        if (b == 0x66) { osz = 1; continue; }
        if (b == 0x26 || b == 0x2E || b == 0x36 || b == 0x3E || b == 0x64 || b == 0x65 ||
            b == 0xF2 || b == 0xF3 || b == 0x67 || b == 0xF0) continue;
        if (b == 0x0F) return (osz << 16) | 0x0F00 | m[(lin + k + 1) % XMEM_SIZE];
        return (osz << 16) | b;
    }
    return 0xFFFFF;
}
static void keystr(unsigned key, char *o)
{
    if ((key & 0xFF00) == 0x0F00) sprintf(o, "%s0F%02X", (key >> 16) ? "66:" : "", key & 0xFF);
    else sprintf(o, "%s%02X", (key >> 16) ? "66:" : "", key & 0xFF);
}

static unsigned g_extra[0x20000], g_mism[0x20000];

/* Whole-token match in a space-separated list: "8C" must not match "66:8C". */
static int allowed_key(const char *allow, const char *ks)
{
    size_t n = strlen(ks);
    const char *q = allow;
    if (!allow) return 0;
    while ((q = strstr(q, ks)) != NULL) {
        if ((q == allow || q[-1] == ' ') && (q[n] == 0 || q[n] == ' ')) return 1;
        q += n;
    }
    return 0;
}

static uint32_t lin_of(const xcpu *x, int pm)
{
    /* Only for printing the bytes: real-mode CS:IP, or the fake PM base. */
    uint32_t base = pm ? (((uint32_t)(x->seg[1] >> 3) * 0x1230u) & 0xFFFF0u) : ((uint32_t)x->seg[1] << 4);
    return (base + x->ip) % XMEM_SIZE;
}

static void dump(const char *tag, const xcpu *x)
{
    int i;
    printf("    %s ip=%04X cs=%04X fl=%08X", tag, x->ip, x->seg[1], x->flags);
    for (i = 0; i < 8; ++i) printf(" r%d=%08X", i, x->r[i]);
    printf(" seg=%04X/%04X/%04X/%04X/%04X/%04X\n", x->seg[0], x->seg[1], x->seg[2], x->seg[3], x->seg[4], x->seg[5]);
}

int main(int argc, char **argv)
{
    long progs = argc > 1 ? atol(argv[1]) : 200000;
    int  steps = argc > 2 ? atoi(argv[2]) : 64;
    uint64_t seed = argc > 3 ? strtoull(argv[3], 0, 0) : 0x5EEDF00DULL;
    int pm = argc > 4 ? atoi(argv[4]) : 0;
    const char *allow = getenv("ALLOW");
    long p, same = 0, extra = 0, bad = 0, allowed = 0, printed = 0;
    unsigned k;
    uint32_t i;

    g_rng = seed ? seed : 1;
    for (i = 0; i < XMEM_SIZE; ++i) IMG[i] = (uint8_t)rnd();
    ref_init(IMG, pm); new_init(IMG, pm);
    for (p = 0; p < progs; ++p) {
        xcpu x; int s;
        memset(&x, 0, sizeof x);
        for (s = 0; s < 8; ++s) x.r[s] = (rnd() & 3) ? (rnd() & 0xFFFF) : rnd();
        for (s = 0; s < 6; ++s) x.seg[s] = (uint16_t)(rnd() & 0xFFFF);
        x.r[1] &= 0xFFFF;
        x.seg[1] = (uint16_t)(rnd() % 0xF000);
        if (pm) x.seg[1] = (uint16_t)((x.seg[1] & ~7u) | 7u);      /* an LDT selector, RPL 3 */
        x.ip = (uint16_t)rnd();
        x.flags = 0x0002 | (rnd() & 0x0ED5u);
        { uint32_t lin = lin_of(&x, pm), q;
          for (q = 0; q < 64; ++q) { uint8_t b = (uint8_t)rnd(); ref_poke((lin + q) % XMEM_SIZE, b); new_poke((lin + q) % XMEM_SIZE, b); } }
        for (s = 0; s < steps; ++s) {
            xcpu xr = x, xn = x; uint64_t fr = 0, fn = 0; int rok, nok;
            unsigned key = keyof(ref_mem(), lin_of(&x, pm));
            new_undo_begin();
            rok = ref_step(&xr, &fr);
            nok = new_step(&xn, &fn);
            if (rok) {
                new_undo_end();
                if (!nok || memcmp(&xr, &xn, sizeof xr) || fr != fn) {
                    char ks[16]; keystr(key, ks);
                    g_mism[key & 0x1FFFF]++;
                    if (allowed_key(allow, ks)) ++allowed;
                    else {
                        ++bad;
                        if (printed++ < 12) {
                            const uint8_t *m = ref_mem(); uint32_t lin = lin_of(&x, pm), q;
                            printf("MISMATCH %s new_ok=%d fx %s  bytes:", ks, nok, fr == fn ? "same" : "DIFFER");
                            for (q = 0; q < 8; ++q) printf(" %02X", m[(lin + q) % XMEM_SIZE]);
                            printf("\n"); dump("in ", &x); dump("old", &xr); dump("new", &xn);
                        }
                    }
                    new_sync(ref_mem());          /* realign memory, end this program */
                    break;
                }
                ++same; x = xr;
                continue;
            }
            /* The old interpreter declined. */
            if (nok) {
                ++extra; g_extra[key & 0x1FFFF]++;
                if (!new_undo_rollback()) new_sync(ref_mem());
            } else new_undo_end();
            break;
        }
    }
    printf("superset: %ld programs, %ld steps identical, %ld steps newly executed, "
           "%ld mismatches (%ld allowed)\n", progs, same, extra, bad + allowed, allowed);
    printf("newly executed, by opcode (top 40):\n");
    { int shown;
      for (shown = 0; shown < 40; ++shown) {
          unsigned best = 0, bk = 0; char ks[16];
          for (k = 0; k < 0x20000; ++k) if (g_extra[k] > best) { best = g_extra[k]; bk = k; }
          if (!best) break;
          keystr(bk, ks); printf("  %-8s %u\n", ks, best); g_extra[bk] = 0;
      } }
    if (allowed) {
        printf("allowed (intended) changes, by opcode:\n");
        for (k = 0; k < 0x20000; ++k) if (g_mism[k]) { char ks[16]; keystr(k, ks); if (allowed_key(allow, ks)) printf("  %-8s %u\n", ks, g_mism[k]); }
    }
    if (bad) { printf("⛔ NOT A SUPERSET\n"); return 1; }
    printf("SUPERSET OK\n");
    return 0;
}
