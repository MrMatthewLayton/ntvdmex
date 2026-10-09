/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Differential fuzz + throughput bench for src/host/v86interp.h (#183).
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
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include "../../src/ntvdmex_types.h"

static BYTE g_Memory[0x110000];
static UINT64 g_Hash = 1469598103934665603ULL;           /* FNV-1a over everything seen */
static VOID InterpFuzzMix(UINT64 value)
{
    INT index;

    for (index = 0; index < 8; ++index)
    {
        g_Hash ^= (BYTE)(value >> (index * 8));
        g_Hash *= 1099511628211ULL;
    }
}

static INT g_IsHashingWrites = 1;
static BYTE V86HostRead8(UINT32 linear)
{
    return (linear < sizeof g_Memory) ? g_Memory[linear] : 0xFF;
}

static VOID    V86HostWrite8(UINT32 linear, BYTE value)
{
    if (g_IsHashingWrites)
        InterpFuzzMix(((UINT64)linear << 8) | value);
    if (linear < sizeof g_Memory)
        g_Memory[linear] = value;
}

static UINT32 V86HostIn(WORD port, INT width)
{
    InterpFuzzMix(0x1000000ULL | port | ((UINT64)width << 16));
    return (UINT32)port * 2654435761u;
}

static VOID V86HostOut(WORD port, INT width, UINT32 value)
{
    InterpFuzzMix(0x2000000ULL | port | ((UINT64)width << 16) | ((UINT64)value << 32));
}

/* #183 (s87): the code-pointer fetch path. Half the address space answers NULL so both
 * paths (pointer and V86HostRead8) run in every fuzz; an older header ignores the hook.
 */
#define V86I_CODE_PTR   1
static const volatile BYTE *V86HostCodePointer(UINT32 linear)
{
    return (linear + 16 <= sizeof g_Memory && !(linear & 0x100)) ? (const volatile BYTE *)&g_Memory[linear] : 0;
}

#ifndef INTERP_H
#define INTERP_H    "../../src/host/v86interp.h"
#endif
#include INTERP_H

static UINT64 g_Random;
static UINT32 InterpFuzzRandom(VOID)
{
    g_Random ^= g_Random << 13;
    g_Random ^= g_Random >> 7;
    g_Random ^= g_Random << 17;
    return (UINT32)(g_Random >> 11);
}

static VOID InterpFuzzHashState(PCV86_CPU cpu, INT isOk)
{
    INT index;

    for (index = 0; index < 8; ++index)
        InterpFuzzMix(cpu->Registers[index]);
    for (index = 0; index < 6; ++index)
        InterpFuzzMix(cpu->Segments[index]);
    InterpFuzzMix(cpu->Ip);
    InterpFuzzMix(cpu->Flags & 0x0FD5u);
    InterpFuzzMix((UINT64)isOk);
}

INT main(INT argc, PSTR *argv)
{
    long programCount = argc > 1 ? atol(argv[1]) : 200000;
    INT  stepLimit = argc > 2 ? atoi(argv[2]) : 64;
    UINT64 seed = argc > 3 ? strtoull(argv[3], 0, 0) : 0x5EEDF00DULL;
    long benchCount = argc > 4 ? atol(argv[4]) : 3000000;
    long program, executed = 0, bails = 0;
    clock_t startClock, endClock;
    UINT32 index;

    /* ---- differential part: random bytes as code, random everything else ---- */
    g_Random = seed ? seed : 1;
    for (index = 0; index < sizeof g_Memory; ++index)
        g_Memory[index] = (BYTE)InterpFuzzRandom();
    for (program = 0; program < programCount; ++program)
    {
        V86_CPU cpu;
        INT position;
        memset(&cpu, 0, sizeof cpu);
        for (position = 0; position < 8; ++position)
            cpu.Registers[position] = (InterpFuzzRandom() & 3) ? (InterpFuzzRandom() & 0xFFFF) : InterpFuzzRandom();
        for (position = 0; position < 6; ++position)
            cpu.Segments[position] = (WORD)(InterpFuzzRandom() & 0xFFFF);
        cpu.Registers[1] &= 0xFFFF;                                /* REP counts CX; see the REP fix */
        cpu.Segments[1] = (WORD)(InterpFuzzRandom() % 0xF000);          /* keep CS:IP in plain RAM */
        cpu.Ip = (WORD)InterpFuzzRandom();
        cpu.Flags = 0x0002 | (InterpFuzzRandom() & 0x0ED5u);
        /* Re-seed a window of code so every program starts on fresh random bytes. */
        { UINT32 linear = ((UINT32)cpu.Segments[1] << 4) + cpu.Ip, offset;
          for (offset = 0; offset < 64 && linear + offset < sizeof g_Memory; ++offset)
              g_Memory[linear + offset] = (BYTE)InterpFuzzRandom(); }
        for (position = 0; position < stepLimit; ++position)
        {
            INT isOk = V86Step(&cpu);
            InterpFuzzHashState(&cpu, isOk);
            if (!isOk)
            {
                ++bails;
                break;
            }
            ++executed;
        }
    }
    printf("fuzz: %ld programs x <=%d steps, seed 0x%llx: %ld executed, %ld bailed\n",
           programCount, stepLimit, (unsigned long long)seed, executed, bails);
    printf("digest %016llx\n", (unsigned long long)g_Hash);

    /* ---- throughput: a Wolf3D-shaped inner loop (compiled scaler + game logic) ---- */
    if (benchCount > 0)
    {
        static const BYTE loopCode[] = {
            0x8A, 0x04,             /* mov al,[si] */
            0x26, 0x88, 0x05,       /* mov es:[di],al */
            0x83, 0xC7, 0x50,       /* add di,80 */
            0x03, 0xF3,             /* add si,bx */
            0x8B, 0x46, 0x06,       /* mov ax,[bp+6] */
            0x3D, 0x34, 0x12,       /* cmp ax,1234h */
            0x74, 0x01,             /* je +1 */
            0x40,                   /* inc ax */
            0xAA,                   /* stosb */
            0x49,                   /* dec cx */
            0x75, 0xE9              /* jnz loop */
        };
        V86_CPU cpu;
        long count = 0;
        memset(&cpu, 0, sizeof cpu);
        memcpy(g_Memory + 0x10000, loopCode, sizeof loopCode);
        cpu.Segments[1] = 0x1000;
        cpu.Segments[0] = 0x3000;
        cpu.Segments[2] = 0x4000;
        cpu.Segments[3] = 0x5000;
        cpu.Flags = 0x0202;
        cpu.Registers[5] = 0x100;
        cpu.Registers[3] = 3;
        g_IsHashingWrites = 0;
        startClock = clock();
        while (count < benchCount)
        {
            cpu.Ip = 0;
            cpu.Registers[1] = 1000;
            while (V86Step(&cpu))
            {
                if (++count >= benchCount)
                    break;
                if (cpu.Ip == 0)
                    break;
            }
            if (cpu.Ip != 0 && cpu.Registers[1] != 0 && count < benchCount)
            {
                printf("bench: bailed at ip %04x\n", cpu.Ip);
                return 1;
            }
        }
        endClock = clock();
        printf("bench: %ld instructions in %.3f s = %.1f M/s\n", count,
               (double)(endClock - startClock) / CLOCKS_PER_SEC, count / 1e6 / ((double)(endClock - startClock) / CLOCKS_PER_SEC));
    }
    return 0;
}
