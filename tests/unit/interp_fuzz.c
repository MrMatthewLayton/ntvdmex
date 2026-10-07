/* interp_fuzz.c -- differential fuzz + throughput bench for src/host/v86interp.h (#183).
 *
 * WHY. The interpreter is being made faster, and "faster" must mean "the same answers,
 * sooner". A unit battery checks the cases someone thought of; this checks the ones
 * nobody did. It runs millions of SEEDED random programs over random state and folds
 * EVERY observable -- the register file, flags, IP and segments after each step, whether
 * the step bailed, every memory write and every port access -- into one digest.
 *
 * HOW TO USE IT. Build it twice, against two interpreters, and compare the digests:
 *
 *   ./scripts/interpfuzz.sh            # HEAD's interpreter vs the working tree's
 *
 * Same seed, same digest = identical behaviour on every program tried. The rate line
 * is the benchmark (instructions per second on this Mac: relative, not the rig's).
 *
 *   interp_fuzz [programs] [steps] [seed] [bench-iters]
 *
 * INTERP_H (a -D) names the interpreter header to include; default the real one.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

typedef unsigned char BYTE;

static BYTE MEM[0x110000];
static uint64_t g_h = 1469598103934665603ULL;           /* FNV-1a over everything seen */
static void hmix(uint64_t v) { int i; for (i = 0; i < 8; ++i) { g_h ^= (BYTE)(v >> (i * 8)); g_h *= 1099511628211ULL; } }

static int g_hash_writes = 1;
static uint8_t V86HostRead8(uint32_t lin) { return (lin < sizeof MEM) ? MEM[lin] : 0xFF; }
static void    V86HostWrite8(uint32_t lin, uint8_t v)
{ if (g_hash_writes) hmix(((uint64_t)lin << 8) | v); if (lin < sizeof MEM) MEM[lin] = v; }
static uint32_t V86HostIn(uint16_t port, int width) { hmix(0x1000000ULL | port | ((uint64_t)width << 16)); return (uint32_t)port * 2654435761u; }
static void V86HostOut(uint16_t port, int width, uint32_t val)
{ hmix(0x2000000ULL | port | ((uint64_t)width << 16) | ((uint64_t)val << 32)); }

/* #183 (s87): the code-pointer fetch path. Half the address space answers NULL so both
   paths (pointer and V86HostRead8) run in every fuzz; an older header ignores the hook. */
#define V86I_CODE_PTR 1
static const volatile BYTE *V86HostCodePointer(uint32_t lin)
{ return (lin + 16 <= sizeof MEM && !(lin & 0x100)) ? (const volatile BYTE *)&MEM[lin] : 0; }

#ifndef INTERP_H
#define INTERP_H "../../src/host/v86interp.h"
#endif
#include INTERP_H

static uint64_t g_rng;
static uint32_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return (uint32_t)(g_rng >> 11); }

static void hstate(const V86_CPU *c, int ok)
{
    int i;
    for (i = 0; i < 8; ++i) hmix(c->Registers[i]);
    for (i = 0; i < 6; ++i) hmix(c->Segments[i]);
    hmix(c->Ip); hmix(c->Flags & 0x0FD5u); hmix((uint64_t)ok);
}

int main(int argc, char **argv)
{
    long progs = argc > 1 ? atol(argv[1]) : 200000;
    int  steps = argc > 2 ? atoi(argv[2]) : 64;
    uint64_t seed = argc > 3 ? strtoull(argv[3], 0, 0) : 0x5EEDF00DULL;
    long bench = argc > 4 ? atol(argv[4]) : 3000000;
    long p, executed = 0, bails = 0;
    clock_t t0, t1;
    uint32_t i;

    /* ---- differential part: random bytes as code, random everything else ---- */
    g_rng = seed ? seed : 1;
    for (i = 0; i < sizeof MEM; ++i) MEM[i] = (BYTE)rnd();
    for (p = 0; p < progs; ++p) {
        V86_CPU c; int s;
        memset(&c, 0, sizeof c);
        for (s = 0; s < 8; ++s) c.Registers[s] = (rnd() & 3) ? (rnd() & 0xFFFF) : rnd();
        for (s = 0; s < 6; ++s) c.Segments[s] = (uint16_t)(rnd() & 0xFFFF);
        c.Registers[1] &= 0xFFFF;                                /* REP counts CX; see the REP fix */
        c.Segments[1] = (uint16_t)(rnd() % 0xF000);          /* keep CS:IP in plain RAM */
        c.Ip = (uint16_t)rnd();
        c.Flags = 0x0002 | (rnd() & 0x0ED5u);
        /* Re-seed a window of code so every program starts on fresh random bytes. */
        { uint32_t lin = ((uint32_t)c.Segments[1] << 4) + c.Ip, k;
          for (k = 0; k < 64 && lin + k < sizeof MEM; ++k) MEM[lin + k] = (BYTE)rnd(); }
        for (s = 0; s < steps; ++s) {
            int ok = V86Step(&c);
            hstate(&c, ok);
            if (!ok) { ++bails; break; }
            ++executed;
        }
    }
    printf("fuzz: %ld programs x <=%d steps, seed 0x%llx: %ld executed, %ld bailed\n",
           progs, steps, (unsigned long long)seed, executed, bails);
    printf("digest %016llx\n", (unsigned long long)g_h);

    /* ---- throughput: a Wolf3D-shaped inner loop (compiled scaler + game logic) ---- */
    if (bench > 0) {
        static const BYTE loop[] = {
            0x8A, 0x04,             /* mov al,[si]       */
            0x26, 0x88, 0x05,       /* mov es:[di],al    */
            0x83, 0xC7, 0x50,       /* add di,80         */
            0x03, 0xF3,             /* add si,bx         */
            0x8B, 0x46, 0x06,       /* mov ax,[bp+6]     */
            0x3D, 0x34, 0x12,       /* cmp ax,1234h      */
            0x74, 0x01,             /* je +1             */
            0x40,                   /* inc ax            */
            0xAA,                   /* stosb             */
            0x49,                   /* dec cx            */
            0x75, 0xE9              /* jnz loop          */
        };
        V86_CPU c; long n = 0;
        memset(&c, 0, sizeof c);
        memcpy(MEM + 0x10000, loop, sizeof loop);
        c.Segments[1] = 0x1000; c.Segments[0] = 0x3000; c.Segments[2] = 0x4000; c.Segments[3] = 0x5000;
        c.Flags = 0x0202; c.Registers[5] = 0x100; c.Registers[3] = 3;
        g_hash_writes = 0;
        t0 = clock();
        while (n < bench) {
            c.Ip = 0; c.Registers[1] = 1000;
            while (V86Step(&c)) { if (++n >= bench) break; if (c.Ip == 0) break; }
            if (c.Ip != 0 && c.Registers[1] != 0 && n < bench) { printf("bench: bailed at ip %04x\n", c.Ip); return 1; }
        }
        t1 = clock();
        printf("bench: %ld instructions in %.3f s = %.1f M/s\n", n,
               (double)(t1 - t0) / CLOCKS_PER_SEC, n / 1e6 / ((double)(t1 - t0) / CLOCKS_PER_SEC));
    }
    return 0;
}
