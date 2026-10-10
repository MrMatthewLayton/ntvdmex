/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Off-VM unit battery for the mode-12h fill-loop interpreter
 * (src/host/v86interp.h).
 *
 * The interpreter is the fast path that runs QuickBASIC's per-pixel PAINT/LINE
 * loops entirely in the host instead of taking one V86 round-trip per pixel.
 * Correctness here = "never derails": exact registers, exact flags, exact bail.
 * We exercise it over a flat 1MB+ memory array (no VGA planar engine -- that's
 * host-specific; here every address is plain RAM), so the decode, the flag
 * maths, the string ops, and the control flow are all checkable natively.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "ntvdmex_types.h"  /* the host hooks below are written before v86interp.h brings it */

static BYTE g_Memory[0x110000];
/* The host hooks v86interp.h requires (flat RAM, range-guarded). */
static BYTE V86HostRead8(UINT32 linear)
{
    return (linear < sizeof g_Memory) ? g_Memory[linear] : 0;
}

static VOID    V86HostWrite8(UINT32 linear, BYTE value)
{
    if (linear < sizeof g_Memory)
        g_Memory[linear] = value;
}

/* Port-I/O hooks: a tiny model so the IN/OUT opcodes are exercised. Port 0x60
 * returns a canned byte; writes to 0x3C5 are recorded for the OUT test.
 */
static BYTE g_Port3C5 = 0;
static UINT32 V86HostIn(WORD port, INT width)
{
    (VOID)width;
    return (port == 0x60) ? 0xA5 : 0;
}

static VOID V86HostOut(WORD port, INT width, UINT32 value)
{
    (VOID)width;

    if (port == 0x3C5)
        g_Port3C5 = (BYTE)value;
}

#include "../../src/host/v86interp.h"

static INT g_Total = 0;
static INT g_Failures = 0;
#define CHECK(cpu,message) do{ g_Total++; if(cpu){printf("  PASS  %s\n",(message));} \
    else{printf("  FAIL  %s\n",(message)); g_Failures++;} }while(0)

/* Load code bytes at the linear address CS:IP and point the cpu there. */
static VOID InterpTestLoad(
    PV86_CPU cpu,
    WORD codeSegment,
    WORD instructionPointer,
    PCBYTE bytes,
    INT count)
{
    UINT32 linear = ((UINT32)codeSegment << 4) + instructionPointer;
    INT index;

    cpu->Segments[1] = codeSegment;
    cpu->Ip = instructionPointer;

    for (index = 0; index < count; index++)
        g_Memory[linear + index] = bytes[index];
}

/* Run until istep bails (unmodeled) or a generous cap; return steps taken. */
static INT InterpTestRun(PV86_CPU cpu)
{
    INT steps = 0;

    while (steps < 10000000 && V86Step(cpu))
        steps++;

    return steps;
}

/* Single-step exactly once. */
static INT InterpTestStepOnce(PV86_CPU cpu)
{
    return V86Step(cpu);
}

static V86_CPU InterpTestMakeCpu(VOID)
{
    V86_CPU cpu;

    memset(&cpu, 0, sizeof cpu);
    cpu.Flags = 0x0002;                                 /* the always-set bit 1 */
    return cpu;
}

/* Tiny descriptor table for the LAR/LSL battery (run 55): sel 0x08 = code 0xFA
 * limit 0xFFFF; sel 0x10 = data 0xF3 (G/D nibble 0x4) limit 0x25CF; else invalid.
 */
static INT InterpTestLarLslDescriptor(WORD selector, PUINT32 accessRights, PUINT32 limit)
{
    switch (selector & 0xFFF8)
    {
    case 0x08:
        if (accessRights)
            *accessRights = (0xFAu << 8);

    if (limit)
        *limit = 0xFFFF;

    return 1;

    case 0x10:
        if (accessRights)
            *accessRights = (0xF3u << 8) | (0x4u << 20);

    if (limit)
        *limit = 0x25CF;

    return 1;

    default:
        return 0;
    }
}

/* #194: THE 0x66 STACK / STRING / CONTROL-TRANSFER FORMS, AND IRET IN PM:
 * Expectations are the SDM's where it decides, and the test machine's (tests/probes/dos/p_o32.com,
 * s87) where it does not: a 32-bit PUSH sreg keeps the slot's upper half, a
 * 32-bit far CALL zero-extends its CS slot, MOV r32,sreg zero-extends, and PUSHFD's
 * upper half is the NT V86 monitor's (VM|RF, VIF = IF, AC/ID as they stand).
 */
static UINT32 InterpTestSegmentToLinear(WORD selector)
{
    return 0x30000u + (UINT32)(selector & 0xFFF8u) * 0x100u;
}

/* 0x08: 16-bit code, limit 0x7FFF. 0x10: data. 0x18: 32-bit code. Anything else invalid. */
static INT InterpTestSelectorDescriptor(WORD selector, PUINT32 accessRights, PUINT32 limit)
{
    switch (selector & 0xFFF8)
    {
    case 0x08:
        *accessRights = 0xFAu << 8;
    *limit = 0x7FFF;
    return 1;

    case 0x10:
        *accessRights = 0xF2u << 8;
    *limit = 0xFFFF;
    return 1;

    case 0x18:
        *accessRights = (0xFAu << 8) | (0x4u << 20);
    *limit = 0xFFFF;
    return 1;

    default:
        return 0;
    }
}

static VOID InterpTestPut32(UINT32 linear, UINT32 value)
{
    g_Memory[linear] = (BYTE)value;
    g_Memory[linear+1] = (BYTE)(value >> 8);
    g_Memory[linear+2] = (BYTE)(value >> 16);
    g_Memory[linear+3] = (BYTE)(value >> 24);
}

static VOID InterpTestO32Battery(VOID)
{
    printf("== #194: 0x66 stack/string/transfer forms, PM IRET ==\n");
    /* PUSHFD: low word as PUSHF; upper = VM|RF, VIF since IF=1 (measured image 000B0297). */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x66, 0x9C };
      cpu.Flags = 0x0297;
      cpu.Segments[2] = 0x2000;
      cpu.Registers[4] = 0x0100;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      CHECK(InterpTestStepOnce(&cpu) && (cpu.Registers[4] & 0xFFFF) == 0xFC && V86ReadMemory(0x200FC, 4) == 0x000B0297u,
            "66 9C pushfd: 4 bytes, image 000B0297 (the test machine's, IF=1)"); }
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x66, 0x9C };
      cpu.Flags = 0x0097;
      cpu.Segments[2] = 0x2000;
      cpu.Registers[4] = 0x0100;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      CHECK(InterpTestStepOnce(&cpu) && V86ReadMemory(0x200FC, 4) == 0x00030097u, "66 9C pushfd: IF=0 -> no VIF"); }
    /* POPFD: loads AC and ID (they stick on the test machine), the arithmetic flags, IF. */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x66, 0x9D, 0x66, 0x9C };
      cpu.Flags = 0x0202;
      cpu.Segments[2] = 0x2000;
      cpu.Registers[4] = 0x00FC;
      InterpTestPut32(0x200FC, 0x002400C1u);
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      CHECK(InterpTestStepOnce(&cpu) && (cpu.Registers[4] & 0xFFFF) == 0x100 && (cpu.Flags & 0xFFFF) == 0x00C3
            && (cpu.Flags & 0x00240000u) == 0x00240000u, "66 9D popfd: CF ZF SF, IF cleared, AC+ID set, SP+4");
      CHECK(InterpTestStepOnce(&cpu) && (V86ReadMemory(0x200FC, 4) & 0x00240000u) == 0x00240000u,
            "pushfd after popfd: AC/ID read back (the 486/CPUID toggle tests pass)"); }
    /* 66 PUSH ES / 66 POP DS: a WORD store into a 4-byte slot (upper kept), SP -/+ 4. */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x66, 0x06, 0x66, 0x1F };
      cpu.Segments[0] = 0x1234;
      cpu.Segments[2] = 0x2000;
      cpu.Registers[4] = 0x0100;
      InterpTestPut32(0x200FC, 0xDEADBEEFu);
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      CHECK(InterpTestStepOnce(&cpu) && (cpu.Registers[4] & 0xFFFF) == 0xFC && V86ReadMemory(0x200FC, 4) == 0xDEAD1234u,
            "66 06: SP-4, low word = ES, upper half of the slot untouched (measured)");
      CHECK(InterpTestStepOnce(&cpu) && cpu.Segments[3] == 0x1234 && (cpu.Registers[4] & 0xFFFF) == 0x100, "66 1F: DS = low word, SP+4"); }
    /* PUSH FS / POP GS: the 0F map, both widths. */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x0F, 0xA0, 0x66, 0x0F, 0xA8, 0x66, 0x0F, 0xA9, 0x0F, 0xA1 };
      cpu.Segments[4] = 0x4444;
      cpu.Segments[5] = 0x5555;
      cpu.Segments[2] = 0x2000;
      cpu.Registers[4] = 0x0100;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      CHECK(InterpTestStepOnce(&cpu) && (cpu.Registers[4] & 0xFFFF) == 0xFE && V86ReadMemory(0x200FE, 2) == 0x4444, "0F A0 push fs: 2 bytes");
      CHECK(InterpTestStepOnce(&cpu) && (cpu.Registers[4] & 0xFFFF) == 0xFA && V86ReadMemory(0x200FA, 2) == 0x5555, "66 0F A8 push gs: 4 bytes");
      cpu.Segments[5] = 0;
      CHECK(InterpTestStepOnce(&cpu) && cpu.Segments[5] == 0x5555 && (cpu.Registers[4] & 0xFFFF) == 0xFE, "66 0F A9 pop gs: SP+4");
      cpu.Segments[4] = 0;
      CHECK(InterpTestStepOnce(&cpu) && cpu.Segments[4] == 0x4444 && (cpu.Registers[4] & 0xFFFF) == 0x100 && cpu.Ip == 10, "0F A1 pop fs: SP+2"); }
    /* REP MOVSD forward, then MOVSD backward. */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0xF3, 0x66, 0xA5, 0xFD, 0x66, 0xA5 };
      cpu.Segments[3] = 0x2000;
      cpu.Segments[0] = 0x3000;
      cpu.Registers[6] = 0x10;
      cpu.Registers[7] = 0x20;
      cpu.Registers[1] = 2;
      InterpTestPut32(0x20010, 0x11223344u);
      InterpTestPut32(0x20014, 0x55667788u);
      InterpTestPut32(0x30020, 0);
      InterpTestPut32(0x30024, 0);
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      CHECK(InterpTestStepOnce(&cpu) && V86ReadMemory(0x30020, 4) == 0x11223344u && V86ReadMemory(0x30024, 4) == 0x55667788u
            && (cpu.Registers[6] & 0xFFFF) == 0x18 && (cpu.Registers[7] & 0xFFFF) == 0x28 && (cpu.Registers[1] & 0xFFFF) == 0,
            "rep movsd: 2 dwords, SI/DI +8, CX 0");
      InterpTestStepOnce(&cpu);                                         /* STD */
      cpu.Registers[6] = 0x10;
      cpu.Registers[7] = 0x40;
      CHECK(InterpTestStepOnce(&cpu) && V86ReadMemory(0x30040, 4) == 0x11223344u && (cpu.Registers[6] & 0xFFFF) == 0x0C
            && (cpu.Registers[7] & 0xFFFF) == 0x3C, "movsd DF=1: SI/DI -4"); }
    /* STOSD / LODSD / REPE CMPSD / REPNE SCASD. */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x66, 0xAB, 0x66, 0xAD };
      cpu.Segments[0] = 0x3000;
      cpu.Segments[3] = 0x3000;
      cpu.Registers[7] = 0x50;
      cpu.Registers[6] = 0x50;
      cpu.Registers[0] = 0xCAFEBABEu;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      CHECK(InterpTestStepOnce(&cpu) && V86ReadMemory(0x30050, 4) == 0xCAFEBABEu && (cpu.Registers[7] & 0xFFFF) == 0x54, "stosd: EAX stored, DI+4");
      cpu.Registers[0] = 0;
      CHECK(InterpTestStepOnce(&cpu) && cpu.Registers[0] == 0xCAFEBABEu && (cpu.Registers[6] & 0xFFFF) == 0x54, "lodsd: EAX loaded, SI+4"); }
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0xF3, 0x66, 0xA7 };       /* repe cmpsd -- p_o32 items 16/17 */
      cpu.Segments[3] = 0x2000;
      cpu.Segments[0] = 0x3000;
      cpu.Registers[6] = 0;
      cpu.Registers[7] = 0;
      cpu.Registers[1] = 3;
      InterpTestPut32(0x20000, 0xA1A2A3A4u);
      InterpTestPut32(0x20004, 0xB1B2B3B4u);
      InterpTestPut32(0x20008, 0xC1C2C3C4u);
      InterpTestPut32(0x30000, 0xA1A2A3A4u);
      InterpTestPut32(0x30004, 0xB1B2B300u);
      InterpTestPut32(0x30008, 0xC1C2C3C4u);
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      CHECK(InterpTestStepOnce(&cpu) && (cpu.Registers[1] & 0xFFFF) == 1 && (cpu.Registers[6] & 0xFFFF) == 8 && (cpu.Flags & 0x08D5) == 0x0004,
            "repe cmpsd: stops at dword 1, CX=1, SI+8, flags = PF only (the test machine's)"); }
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0xF2, 0x66, 0xAF };       /* repne scasd -- p_o32 items 18/19 */
      cpu.Segments[0] = 0x2000;
      cpu.Registers[7] = 0;
      cpu.Registers[1] = 5;
      cpu.Registers[0] = 0xC1C2C3C4u;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      CHECK(InterpTestStepOnce(&cpu) && (cpu.Registers[1] & 0xFFFF) == 2 && (cpu.Registers[7] & 0xFFFF) == 12 && (cpu.Flags & 0x08D5) == 0x0044,
            "repne scasd: found at dword 2, CX=2, DI+12, ZF+PF"); }
    /* CALL rel32 / RET / RET imm16 / bail past 64 KB. */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x66, 0xE8, 0x10, 0x00, 0x00, 0x00 };
      cpu.Segments[2] = 0x2000;
      cpu.Registers[4] = 0x100;
      InterpTestLoad(&cpu, 0x1000, 0x20, bytes, sizeof bytes);
      CHECK(InterpTestStepOnce(&cpu) && cpu.Ip == 0x36 && (cpu.Registers[4] & 0xFFFF) == 0xFC && V86ReadMemory(0x200FC, 4) == 0x26,
            "66 E8: target next+rel32, 4-byte return EIP"); }
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x66, 0xE8, 0x00, 0x00, 0x01, 0x00 };   /* rel32 = 10000h */
      cpu.Segments[2] = 0x2000;
      cpu.Registers[4] = 0x100;
      InterpTestLoad(&cpu, 0x1000, 0x20, bytes, sizeof bytes);
      CHECK(InterpTestStepOnce(&cpu) == 0 && cpu.Ip == 0x20 && (cpu.Registers[4] & 0xFFFF) == 0x100,
            "66 E8 past 64 KB: bails, nothing moved (the CPU's #GP)"); }
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x66, 0xE9, 0xF0, 0xFF, 0xFF, 0xFF };   /* jmp -16 */
      InterpTestLoad(&cpu, 0x1000, 0x40, bytes, sizeof bytes);
      CHECK(InterpTestStepOnce(&cpu) && cpu.Ip == 0x36, "66 E9: jmp rel32 backwards"); }
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x66, 0xC3 };
      cpu.Segments[2] = 0x2000;
      cpu.Registers[4] = 0xF8;
      InterpTestPut32(0x200F8, 0x1234);
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      CHECK(InterpTestStepOnce(&cpu) && cpu.Ip == 0x1234 && (cpu.Registers[4] & 0xFFFF) == 0xFC, "66 C3: pops a 4-byte EIP"); }
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x66, 0xC2, 0x04, 0x00 };
      cpu.Segments[2] = 0x2000;
      cpu.Registers[4] = 0xF8;
      InterpTestPut32(0x200F8, 0x1234);
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      CHECK(InterpTestStepOnce(&cpu) && cpu.Ip == 0x1234 && (cpu.Registers[4] & 0xFFFF) == 0x100, "66 C2 4: EIP + 4 bytes of arguments"); }
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x66, 0xC3 };
      cpu.Segments[2] = 0x2000;
      cpu.Registers[4] = 0xF8;
      InterpTestPut32(0x200F8, 0x00011234u);
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      CHECK(InterpTestStepOnce(&cpu) == 0 && cpu.Ip == 0 && (cpu.Registers[4] & 0xFFFF) == 0xF8, "66 C3 to EIP > FFFFh: bails untouched"); }
    /* RETF dword (real mode): EIP then a CS dword; RETF 8. */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x66, 0xCA, 0x08, 0x00 };
      cpu.Segments[2] = 0x2000;
      cpu.Registers[4] = 0xE0;
      InterpTestPut32(0x200E0, 0x0456);
      InterpTestPut32(0x200E4, 0xABCD2345u);
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      CHECK(InterpTestStepOnce(&cpu) && cpu.Ip == 0x0456 && cpu.Segments[1] == 0x2345 && (cpu.Registers[4] & 0xFFFF) == 0xF0,
            "66 CA 8: CS = low word of its dword, SP += 8 + 8"); }
    /* FF /2 /4 /6 /3 under 66. */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x66, 0xFF, 0x16, 0x00, 0x05 };       /* call dword [0500h] */
      cpu.Segments[3] = 0x2000;
      cpu.Segments[2] = 0x2000;
      cpu.Registers[4] = 0x100;
      InterpTestPut32(0x20500, 0x0777);
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      CHECK(InterpTestStepOnce(&cpu) && cpu.Ip == 0x0777 && V86ReadMemory(0x200FC, 4) == 5, "66 FF /2: near call [m32], 4-byte return"); }
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x66, 0xFF, 0xE0 };                   /* jmp eax */
      cpu.Registers[0] = 0x0000ABCDu;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      CHECK(InterpTestStepOnce(&cpu) && cpu.Ip == 0xABCD, "66 FF /4: jmp eax"); }
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x66, 0xFF, 0x36, 0x00, 0x05 };       /* push dword [0500h] */
      cpu.Segments[3] = 0x2000;
      cpu.Segments[2] = 0x2000;
      cpu.Registers[4] = 0x100;
      InterpTestPut32(0x20500, 0x12345678u);
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      CHECK(InterpTestStepOnce(&cpu) && V86ReadMemory(0x200FC, 4) == 0x12345678u && (cpu.Registers[4] & 0xFFFF) == 0xFC, "66 FF /6: push dword [m]"); }
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x66, 0xFF, 0x1E, 0x00, 0x05 };       /* call far [0500h] m16:32 */
      cpu.Segments[3] = 0x2000;
      cpu.Segments[2] = 0x2000;
      cpu.Registers[4] = 0x100;
      InterpTestPut32(0x20500, 0x0321);
      g_Memory[0x20504] = 0x34;
      g_Memory[0x20505] = 0x12;
      InterpTestPut32(0x200FC, 0xFACE0000u);                                          /* sentinel in the CS slot */
      InterpTestLoad(&cpu, 0x1000, 0x10, bytes, sizeof bytes);
      CHECK(InterpTestStepOnce(&cpu) && cpu.Segments[1] == 0x1234 && cpu.Ip == 0x0321 && (cpu.Registers[4] & 0xFFFF) == 0xF8
            && V86ReadMemory(0x200FC, 4) == 0x00001000u && V86ReadMemory(0x200F8, 4) == 0x15,
            "66 FF /3: far call m16:32, CS slot ZERO-extended (measured), EIP slot"); }
    /* 66 9A / 66 EA ptr16:32. */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x66, 0x9A, 0x21, 0x03, 0x00, 0x00, 0x34, 0x12 };
      cpu.Segments[2] = 0x2000;
      cpu.Registers[4] = 0x100;
      InterpTestPut32(0x200FC, 0xFACE0000u);
      InterpTestLoad(&cpu, 0x1000, 0x10, bytes, sizeof bytes);
      CHECK(InterpTestStepOnce(&cpu) && cpu.Segments[1] == 0x1234 && cpu.Ip == 0x0321 && V86ReadMemory(0x200FC, 4) == 0x1000
            && V86ReadMemory(0x200F8, 4) == 0x18, "66 9A: far call ptr16:32, CS dword, EIP dword"); }
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x66, 0xEA, 0x21, 0x03, 0x00, 0x00, 0x34, 0x12 };
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      CHECK(InterpTestStepOnce(&cpu) && cpu.Segments[1] == 0x1234 && cpu.Ip == 0x0321, "66 EA: far jmp ptr16:32"); }
    /* 66 8F /0, 66 C4, 66 C9. */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x66, 0x8F, 0x06, 0x00, 0x05 };
      cpu.Segments[3] = 0x2000;
      cpu.Segments[2] = 0x2000;
      cpu.Registers[4] = 0xFC;
      InterpTestPut32(0x200FC, 0xCAFEBABEu);
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      CHECK(InterpTestStepOnce(&cpu) && V86ReadMemory(0x20500, 4) == 0xCAFEBABEu && (cpu.Registers[4] & 0xFFFF) == 0x100, "66 8F: pop dword [m]"); }
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x66, 0xC4, 0x0E, 0x00, 0x05 };       /* les ecx,[0500h] */
      cpu.Segments[3] = 0x2000;
      InterpTestPut32(0x20500, 0x87654321u);
      g_Memory[0x20504] = 0x99;
      g_Memory[0x20505] = 0x88;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      CHECK(InterpTestStepOnce(&cpu) && cpu.Registers[1] == 0x87654321u && cpu.Segments[0] == 0x8899, "66 C4: les ecx, m16:32"); }
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x66, 0xC9 };                         /* p_o32 items 36/37 */
      cpu.Segments[2] = 0x2000;
      cpu.Registers[4] = 0xABCD00F2u;
      cpu.Registers[5] = 0x777700FCu;
      InterpTestPut32(0x200FC, 0x11223344u);
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      CHECK(InterpTestStepOnce(&cpu) && cpu.Registers[5] == 0x11223344u && cpu.Registers[4] == 0xABCD0100u,
            "66 C9 leave: SP = BP (16-bit stack, ESP high kept), EBP = pop32"); }
    /* IRETD, real mode. */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x66, 0xCF };
      cpu.Segments[2] = 0x2000;
      cpu.Registers[4] = 0xF0;
      InterpTestPut32(0x200F0, 0x0456);
      InterpTestPut32(0x200F4, 0x2345);
      InterpTestPut32(0x200F8, 0x00240203u);
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      CHECK(InterpTestStepOnce(&cpu) && cpu.Ip == 0x0456 && cpu.Segments[1] == 0x2345 && (cpu.Registers[4] & 0xFFFF) == 0xFC
            && (cpu.Flags & 0xFFFF) == 0x0203 && (cpu.Flags & 0x00240000u) == 0x00240000u,
            "66 CF iretd: EIP, CS, EFLAGS dwords (12 bytes), as POPFD loads them"); }
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x66, 0xCF };
      cpu.Segments[2] = 0x2000;
      cpu.Registers[4] = 0xF0;
      InterpTestPut32(0x200F0, 0x00010456u);
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      CHECK(InterpTestStepOnce(&cpu) == 0 && (cpu.Registers[4] & 0xFFFF) == 0xF0, "66 CF to EIP > FFFFh: bails"); }
    /* 66 8C: register zero-extends (measured), memory is a word. */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x66, 0x8C, 0xD8, 0x66, 0x8C, 0x1E, 0x00, 0x05 };
      cpu.Registers[0] = 0xDEADBEEFu;
      cpu.Segments[3] = 0x2000;
      InterpTestPut32(0x20500, 0xFFFFFFFFu);
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      CHECK(InterpTestStepOnce(&cpu) && cpu.Registers[0] == 0x2000, "66 8C D8 mov eax,ds: zero-extended (measured)");
      CHECK(InterpTestStepOnce(&cpu) && V86ReadMemory(0x20500, 4) == 0xFFFF2000u, "66 8C to memory: a word store"); }
    /* IN EAX: a 4-byte port access now. */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x66, 0xE5, 0x60 };
      cpu.Registers[0] = 0xDEADBEEFu;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      CHECK(InterpTestStepOnce(&cpu) && cpu.Registers[0] == 0xA5, "66 E5 in eax,60h: all 32 bits written"); }

    /* IRET and RETF dword in 16-bit PROTECTED mode: */
    g_V86SegmentToLinear = InterpTestSegmentToLinear;
    g_V86SelectorDescriptor = InterpTestSelectorDescriptor;
    { V86_CPU cpu = InterpTestMakeCpu();
    UINT32 stackBase = InterpTestSegmentToLinear(0x17);
    UINT32 linear = InterpTestSegmentToLinear(0x0F) + 0x20;
      cpu.Segments[1] = 0x000F;
      cpu.Segments[2] = 0x0017;
      cpu.Registers[4] = 0x100;
      cpu.Flags = 0x0002;
      cpu.Ip = 0x20;
      g_Memory[linear] = 0xCF;
      g_Memory[stackBase + 0x100] = 0x34;
      g_Memory[stackBase + 0x101] = 0x12;
      g_Memory[stackBase + 0x102] = 0x0F;
      g_Memory[stackBase + 0x103] = 0x00;
      g_Memory[stackBase + 0x104] = 0x03;
      g_Memory[stackBase + 0x105] = 0x02;                       /* FLAGS 0203 */
      CHECK(InterpTestStepOnce(&cpu) && cpu.Ip == 0x1234 && cpu.Segments[1] == 0x000F && (cpu.Registers[4] & 0xFFFF) == 0x106
            && (cpu.Flags & 0xFFFF) == 0x0203, "PM iret: same ring, 16-bit code target, FLAGS loaded"); }
    { V86_CPU cpu = InterpTestMakeCpu();
    UINT32 stackBase = InterpTestSegmentToLinear(0x17);
    UINT32 linear = InterpTestSegmentToLinear(0x0F) + 0x20;
      cpu.Segments[1] = 0x000F;
      cpu.Segments[2] = 0x0017;
      cpu.Registers[4] = 0x100;
      cpu.Ip = 0x20;
      g_Memory[linear] = 0xCF;
      g_Memory[stackBase + 0x100] = 0x34;
      g_Memory[stackBase + 0x101] = 0x12;
      g_Memory[stackBase + 0x102] = 0x17;
      g_Memory[stackBase + 0x103] = 0x00;
      CHECK(InterpTestStepOnce(&cpu) == 0 && cpu.Ip == 0x20 && (cpu.Registers[4] & 0xFFFF) == 0x100, "PM iret to a DATA selector: bails");
      g_Memory[stackBase + 0x102] = 0x1F;
      CHECK(InterpTestStepOnce(&cpu) == 0, "PM iret to a 32-bit code segment: bails (not decodable here)");
      g_Memory[stackBase + 0x102] = 0x0C;
      CHECK(InterpTestStepOnce(&cpu) == 0, "PM iret to RPL 0 (a ring change, SS:SP too): bails");
      g_Memory[stackBase + 0x102] = 0x0F;
      g_Memory[stackBase + 0x101] = 0x90;                       /* IP 9034 > limit 7FFF */
      CHECK(InterpTestStepOnce(&cpu) == 0, "PM iret past the target's limit: bails");
      g_Memory[stackBase + 0x102] = 0x27;
      g_Memory[stackBase + 0x101] = 0x12;
      CHECK(InterpTestStepOnce(&cpu) == 0, "PM iret to an invalid selector: bails"); }
    { V86_CPU cpu = InterpTestMakeCpu();
    UINT32 stackBase = InterpTestSegmentToLinear(0x17);
    UINT32 linear = InterpTestSegmentToLinear(0x0F) + 0x20;
      cpu.Segments[1] = 0x000F;
      cpu.Segments[2] = 0x0017;
      cpu.Registers[4] = 0x100;
      cpu.Ip = 0x20;
      g_Memory[linear] = 0x66;
      g_Memory[linear + 1] = 0xCB;
      InterpTestPut32(stackBase + 0x100, 0x00000456u);
      InterpTestPut32(stackBase + 0x104, 0x0000000Fu);
      CHECK(InterpTestStepOnce(&cpu) && cpu.Ip == 0x456 && cpu.Segments[1] == 0x0F && (cpu.Registers[4] & 0xFFFF) == 0x108,
            "PM 66 CB retfd: to a 16-bit code segment, SP+8");
      cpu.Ip = 0x20;
      cpu.Registers[4] = 0x100;
      InterpTestPut32(stackBase + 0x104, 0x1F);
      CHECK(InterpTestStepOnce(&cpu) == 0, "PM 66 CB retfd to a 32-bit segment: bails"); }
    { V86_CPU cpu = InterpTestMakeCpu();
    UINT32 linear = InterpTestSegmentToLinear(0x0F) + 0x20;
      cpu.Segments[1] = 0x000F;
      cpu.Segments[2] = 0x0017;
      cpu.Registers[4] = 0x100;
      cpu.Ip = 0x20;
      g_Memory[linear] = 0x66;
      g_Memory[linear + 1] = 0xCF;
      CHECK(InterpTestStepOnce(&cpu) == 0, "PM 66 CF iretd: still the CPU's (bails)"); }
    g_V86SegmentToLinear = 0;
    g_V86SelectorDescriptor = 0;
}

/* #194: REPLAY p_o32.com THROUGH THE INTERPRETER AND COMPARE WITH THE TEST MACHINE'S CPU:
 * p_o32's `measure` section is pure computation into `res` (no INT, no I/O, no absolute
 * segment value stored), so the very bytes the test machine ran under XP's V86 monitor can be run
 * here. p_o32.ref.txt is the test machine's `BUF=res` line, recorded by
 * `dosdiff.py tests/probes/dos/p_o32.com --host ntvdmex` (s87). Equal buffers =
 * the interpreter answers every case as the machine it stands in for does -- including
 * the parts the manual leaves to the implementation.
 *
 * [CAUTION]: A missing .COM or reference is a FAILURE, not a skip (offvm.sh's rule).
 */
static INT InterpTestHexValue(INT character) { return (character >= '0' && character <= '9') ? character - '0' : (character >= 'A' && character <= 'F') ? character - 'A' + 10
                                   : (character >= 'a' && character <= 'f') ? character - 'a' + 10 : -1; }
static VOID InterpTestO32Replay(VOID)
{
    FILE *file;
    static BYTE comImage[0x10000];
    size_t size;
    CHAR line[4096];
    BYTE reference[1024];
    INT referenceCount = 0;
    WORD measureOffset;
    WORD resultOffset;
    WORD resultLength;
    V86_CPU cpu;
    long steps = 0;
    INT index;
    INT first = -1;

    printf("== #194: p_o32.com replayed through the interpreter vs the test machine ==\n");
    file = fopen("p_o32.com", "rb");
    CHECK(file != NULL, "p_o32.com present (run from tests/unit)");

    if (!file)
        return;

    size = fread(comImage, 1, sizeof comImage, file);
    fclose(file);
    file = fopen("p_o32.ref.txt", "r");
    CHECK(file != NULL, "p_o32.ref.txt present (the test machine's dump)");

    if (!file)
        return;

    while (fgets(line, sizeof line, file))
    {
        PSTR cursor = strstr(line, "BUF=res ");

        if (!cursor)
            continue;

        for (cursor += 8; InterpTestHexValue(cursor[0]) >= 0 && InterpTestHexValue(cursor[1]) >= 0 && referenceCount < (INT)sizeof reference; cursor += 2)
            reference[referenceCount++] = (BYTE)(InterpTestHexValue(cursor[0]) * 16 + InterpTestHexValue(cursor[1]));
    }

    fclose(file);
    measureOffset = (WORD)(comImage[3] | (comImage[4] << 8));
    resultOffset     = (WORD)(comImage[7] | (comImage[8] << 8));
    resultLength  = (WORD)(comImage[9] | (comImage[10] << 8));
    CHECK(referenceCount == resultLength, "reference length = the probe's RES_LEN");
    memset(g_Memory + 0x10000, 0, 0x10000);
    memcpy(g_Memory + 0x10100, comImage, size);
    cpu = InterpTestMakeCpu();

    for (index = 0; index < 4; ++index)
        cpu.Segments[index] = 0x1000;

    cpu.Flags = 0x0202;                                  /* IF=1, as the test machine ran it */
    cpu.Registers[4] = 0xFFFC;
    g_Memory[0x1FFFC] = 0xF0;
    g_Memory[0x1FFFD] = 0xFF;   /* return to FFF0: the end */
    cpu.Ip = measureOffset;

    while (cpu.Ip != 0xFFF0 && steps < 200000 && V86Step(&cpu))
        ++steps;

    if (cpu.Ip != 0xFFF0)
    {
        UINT32 linear = ((UINT32)cpu.Segments[1] << 4) + cpu.Ip;
        printf("  bailed at %04X:%04X bytes %02X %02X %02X %02X after %ld steps\n",
               cpu.Segments[1], cpu.Ip, g_Memory[linear], g_Memory[linear + 1], g_Memory[linear + 2], g_Memory[linear + 3], steps);
    }

    CHECK(cpu.Ip == 0xFFF0, "the whole measure section runs in the interpreter (no bail)");

    for (index = 0; index < referenceCount && index < resultLength; ++index)
        if (g_Memory[0x10000 + resultOffset + index] != reference[index])
        {
            first = index;
            break;
        }

    if (first >= 0)
    {
        INT item = first / 4;
        printf("  first difference in item %d: interp %02X%02X%02X%02X real %02X%02X%02X%02X (bytes, LE)\n", item,
               g_Memory[0x10000 + resultOffset + 4*item], g_Memory[0x10000 + resultOffset + 4*item + 1], g_Memory[0x10000 + resultOffset + 4*item + 2], g_Memory[0x10000 + resultOffset + 4*item + 3],
               reference[4*item], reference[4*item + 1], reference[4*item + 2], reference[4*item + 3]);
    }

    CHECK(first < 0 && referenceCount == resultLength, "res buffer identical to the test machine's, byte for byte");
}

INT main(VOID)
{
    printf("== mode-12h fill-loop interpreter battery ==\n");
    memset(g_Memory, 0, sizeof g_Memory);

    /* T1: ADD AL,imm8 -> 0x80+0x80 = 0 (CF,ZF,OF,PF; not SF,AF): */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x04, 0x80 };    /* ADD AL,80h */
      cpu.Registers[0] = 0x0080;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      InterpTestStepOnce(&cpu);
      CHECK((cpu.Registers[0] & 0xFF) == 0x00, "add: 80+80 result = 0");
      CHECK((cpu.Flags & EFLAGS_CF_U) && (cpu.Flags & EFLAGS_ZF_U) && (cpu.Flags & EFLAGS_OF_U) && (cpu.Flags & EFLAGS_PF_U),
            "add: 80+80 sets CF+ZF+OF+PF");
      CHECK(!(cpu.Flags & EFLAGS_SF_U) && !(cpu.Flags & EFLAGS_AF_U), "add: 80+80 clears SF+AF"); }

    /* T2: ADD AL,1 -> 0x7F+1 = 0x80 (OF,SF,AF; not CF,ZF,PF): */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x04, 0x01 };
      cpu.Registers[0] = 0x007F;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      InterpTestStepOnce(&cpu);
      CHECK((cpu.Registers[0] & 0xFF) == 0x80, "add: 7F+1 result = 80");
      CHECK((cpu.Flags & EFLAGS_OF_U) && (cpu.Flags & EFLAGS_SF_U) && (cpu.Flags & EFLAGS_AF_U),
            "add: 7F+1 sets OF+SF+AF");
      CHECK(!(cpu.Flags & EFLAGS_CF_U) && !(cpu.Flags & EFLAGS_ZF_U) && !(cpu.Flags & EFLAGS_PF_U),
            "add: 7F+1 clears CF+ZF+PF"); }

    /* T3: SUB AX,imm16 -> 1-2 = 0xFFFF (CF,SF,AF,PF; not OF,ZF): */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x2D, 0x02, 0x00 };
      cpu.Registers[0] = 0x0001;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      InterpTestStepOnce(&cpu);
      CHECK(cpu.Registers[0] == 0xFFFF, "sub: 1-2 result = FFFF");
      CHECK((cpu.Flags & EFLAGS_CF_U) && (cpu.Flags & EFLAGS_SF_U) && (cpu.Flags & EFLAGS_AF_U) && (cpu.Flags & EFLAGS_PF_U),
            "sub: 1-2 sets CF+SF+AF+PF");
      CHECK(!(cpu.Flags & EFLAGS_OF_U) && !(cpu.Flags & EFLAGS_ZF_U), "sub: 1-2 clears OF+ZF"); }

    /* T4: CMP computes flags but does not store: */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x3D, 0x01, 0x00 };  /* CMP AX,1 */
      cpu.Registers[0] = 0x0001;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      InterpTestStepOnce(&cpu);
      CHECK(cpu.Registers[0] == 0x0001, "cmp: AX unchanged");
      CHECK((cpu.Flags & EFLAGS_ZF_U) && !(cpu.Flags & EFLAGS_CF_U), "cmp: 1==1 -> ZF, no CF"); }

    /* T5: OR AL,AL on zero -> ZF+PF, CF/OF cleared: */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x08, 0xC0 };   /* OR AL,AL */
      cpu.Registers[0] = 0x0000;
      cpu.Flags |= EFLAGS_CF_U | EFLAGS_OF_U;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      InterpTestStepOnce(&cpu);
      CHECK((cpu.Flags & EFLAGS_ZF_U) && (cpu.Flags & EFLAGS_PF_U), "or: 0|0 sets ZF+PF");
      CHECK(!(cpu.Flags & EFLAGS_CF_U) && !(cpu.Flags & EFLAGS_OF_U), "or: clears CF+OF"); }

    /* T6: INC/DEC preserve CF; INC 0xFFFF -> 0 with ZF, CF kept: */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x40 };          /* INC AX */
      cpu.Registers[0] = 0xFFFF;
      cpu.Flags |= EFLAGS_CF_U;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      InterpTestStepOnce(&cpu);
      CHECK(cpu.Registers[0] == 0x0000 && (cpu.Flags & EFLAGS_ZF_U), "inc: FFFF+1 = 0, ZF");
      CHECK(cpu.Flags & EFLAGS_CF_U, "inc: preserves CF"); }

    /* T7: ADC adds the carry-in: */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x14, 0x00 };    /* ADC AL,0 */
      cpu.Registers[0] = 0x0005;
      cpu.Flags |= EFLAGS_CF_U;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      InterpTestStepOnce(&cpu);
      CHECK((cpu.Registers[0] & 0xFF) == 0x06, "adc: 5+0+CF = 6"); }

    /* T8: MOV reg<->mem + ModRM disp + segment override: */
    { V86_CPU cpu = InterpTestMakeCpu();
      /* MOV AL, ES:[BX+SI+2]  =  26 8A 40 02 */
      BYTE bytes[] = { 0x26, 0x8A, 0x40, 0x02 };
      cpu.Segments[0] = 0x2000;
      cpu.Registers[3] = 0x0010;
      cpu.Registers[6] = 0x0004;   /* ES, BX, SI */
      g_Memory[((UINT32)0x2000 << 4) + 0x16] = 0x9C;            /* ES:(0x10+4+2)=0x16 */
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      InterpTestStepOnce(&cpu);
      CHECK((cpu.Registers[0] & 0xFF) == 0x9C, "mov: AL <- ES:[BX+SI+2]");
      CHECK(cpu.Ip == 4, "mov: ip advanced by 4 (prefix+modrm+disp8)"); }

    /* T9: [BP] defaults to SS: */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x8A, 0x46, 0x00 };     /* MOV AL,[BP+0] */
      cpu.Segments[2] = 0x3000;
      cpu.Registers[5] = 0x0020;                    /* SS, BP */
      g_Memory[((UINT32)0x3000 << 4) + 0x20] = 0x77;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      InterpTestStepOnce(&cpu);
      CHECK((cpu.Registers[0] & 0xFF) == 0x77, "mov: [BP] uses SS by default"); }

    /* T10: MOV r/m,imm (C7) into RAM via [BX]: */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0xC7, 0x07, 0x34, 0x12 };  /* MOV WORD [BX],1234h */
      cpu.Segments[3] = 0x4000;
      cpu.Registers[3] = 0x0008;                    /* DS, BX */
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      InterpTestStepOnce(&cpu);
      CHECK(g_Memory[((UINT32)0x4000<<4)+8] == 0x34 && g_Memory[((UINT32)0x4000<<4)+9] == 0x12,
            "mov: WORD [BX] = 1234h (little-endian)"); }

    /* T11: REP STOSB fill (forward, DF=0): */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0xF3, 0xAA };    /* REP STOSB */
      cpu.Segments[0] = 0x2000;
      cpu.Registers[7] = 0x0000;
      cpu.Registers[1] = 4;
      cpu.Registers[0] = 0x5A;  /* ES,DI,CX,AL */
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      InterpTestStepOnce(&cpu);
      CHECK(g_Memory[0x20000]==0x5A && g_Memory[0x20001]==0x5A && g_Memory[0x20002]==0x5A && g_Memory[0x20003]==0x5A,
            "stos: REP fills 4 bytes");
      CHECK(cpu.Registers[1] == 0 && cpu.Registers[7] == 4, "stos: CX=0, DI advanced to 4"); }

    /* T12: REP STOSW backward (DF=1): */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0xFD, 0xF3, 0xAB };   /* STD; REP STOSW */
      cpu.Segments[0] = 0x3000;
      cpu.Registers[7] = 0x0010;
      cpu.Registers[1] = 2;
      cpu.Registers[0] = 0x1234;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      InterpTestStepOnce(&cpu);
      InterpTestStepOnce(&cpu);   /* STD, then REP STOSW */
      CHECK(g_Memory[0x30010]==0x34 && g_Memory[0x30011]==0x12, "stosw/STD: word at DI");
      CHECK(g_Memory[0x3000E]==0x34 && g_Memory[0x3000F]==0x12, "stosw/STD: word at DI-2");
      CHECK(cpu.Registers[7] == 0x000C && cpu.Registers[1] == 0, "stosw/STD: DI=-4, CX=0"); }

    /* T13: REP MOVSB copy: */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0xFC, 0xF3, 0xA4 };   /* CLD; REP MOVSB */
      cpu.Segments[3] = 0x5000;
      cpu.Segments[0] = 0x6000;
      cpu.Registers[6] = 0;
      cpu.Registers[7] = 0;
      cpu.Registers[1] = 3;
      g_Memory[0x50000]=0xDE;
      g_Memory[0x50001]=0xAD;
      g_Memory[0x50002]=0xBE;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      InterpTestStepOnce(&cpu);
      InterpTestStepOnce(&cpu);
      CHECK(g_Memory[0x60000]==0xDE && g_Memory[0x60001]==0xAD && g_Memory[0x60002]==0xBE,
            "movsb: DS:SI -> ES:DI x3"); }

    /* T14: Jcc taken/not on ZF: */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x74, 0x10 };    /* JZ +0x10 */
      cpu.Flags |= EFLAGS_ZF_U;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      InterpTestStepOnce(&cpu);
      CHECK(cpu.Ip == 0x12, "jz: taken -> ip = 2 + 0x10");
      cpu = InterpTestMakeCpu();
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      InterpTestStepOnce(&cpu);
      CHECK(cpu.Ip == 0x02, "jz: not taken -> ip = 2"); }

    /* T15: JMP short backward / NOP: */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x90, 0xEB, 0xFD };  /* NOP; JMP -3 */
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      InterpTestStepOnce(&cpu);
      CHECK(cpu.Ip == 1, "nop: ip=1");
      InterpTestStepOnce(&cpu);
      CHECK(cpu.Ip == 0, "jmp short: 3 + (-3) = 0"); }

    /* T16: LOOP countdown (CX=5 -> runs body 5x, CX=0): */
    { V86_CPU cpu = InterpTestMakeCpu();
      /* MOV CX,5 ; loop: NOP ; LOOP loop */
      BYTE bytes[] = { 0xB9, 0x05, 0x00, 0x90, 0xE2, 0xFD };
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      InterpTestStepOnce(&cpu);                                      /* MOV CX,5 */
      {
          INT guard = 0;

          while (cpu.Ip != 6 && guard++ < 100)
              InterpTestStepOnce(&cpu);
      }
      CHECK(cpu.Registers[1] == 0, "loop: CX decremented to 0");
      CHECK(cpu.Ip == 6, "loop: fell through after CX hit 0"); }

    /* T17: TEST sets flags, no store: */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0xA8, 0x01 };    /* TEST AL,1 */
      cpu.Registers[0] = 0x00F0;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      InterpTestStepOnce(&cpu);
      CHECK(cpu.Registers[0] == 0x00F0 && (cpu.Flags & EFLAGS_ZF_U), "test: AL&1==0 -> ZF, AL kept"); }

    /* T18: bail on an unmodeled opcode leaves state exactly at it:
     * The load-bearing case is the VDM BOP, `C4 C4 nn`. C4 is LES, and LES is  *
     * deliberately NOT modeled: bailing on it is how a DOS/BIOS call reaches   *
     * the kernel as a BOP event instead of being swallowed here.
     */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x90, 0xC4, 0xC4, 0x21 };   /* NOP; BOP 21h */
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      CHECK(InterpTestStepOnce(&cpu) == 1 && cpu.Ip == 1, "bail: NOP runs");
      CHECK(InterpTestStepOnce(&cpu) == 0 && cpu.Ip == 1, "bail: BOP (C4 C4 nn) returns 0, ip unchanged"); }

    /* T18b: INT nn / IRET / CLI / STI (GH #55):
     * Continuous interpretation in mode 12h means the interpreter has to run
     * the guest THROUGH its own interrupt handlers; before this it stopped at
     * the first INT and handed the guest back to V86, where its A0000 writes
     * are invisible to us. Vectoring goes through the real IVT at linear 0.
     */
    { V86_CPU cpu = InterpTestMakeCpu();
      BYTE bytes[] = { 0xFB, 0xCD, 0x21 };                  /* STI; INT 21h */
      BYTE handler[] = { 0xCF };                              /* handler: IRET */
      g_Memory[0x21 * 4] = 0x34;
      g_Memory[0x21 * 4 + 1] = 0x12;   /* IVT[21h] = 5000:1234 */
      g_Memory[0x21 * 4 + 2] = 0x00;
      g_Memory[0x21 * 4 + 3] = 0x50;
      InterpTestLoad(&cpu, 0x5000, 0x1234, handler, sizeof handler);
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      cpu.Segments[2] = 0x2000;
      cpu.Registers[4] = 0x0100;               /* SS:SP = 2000:0100 */
      CHECK(InterpTestStepOnce(&cpu) == 1 && (cpu.Flags & 0x200u), "sti: IF set in the flag image");
      CHECK(InterpTestStepOnce(&cpu) == 1, "CD 21: INT is modeled");
      CHECK(cpu.Segments[1] == 0x5000 && cpu.Ip == 0x1234, "int: vectored via IVT[21h]");
      CHECK(cpu.Registers[4] == 0x00FA, "int: pushed FLAGS/CS/IP (SP -= 6)");
      CHECK(!(cpu.Flags & 0x200u), "int: IF cleared on entry");
      CHECK(g_Memory[0x20000 + 0xFA] == 0x03 && g_Memory[0x20000 + 0xFB] == 0x00,
            "int: return IP points past the 2-byte INT");
      CHECK(g_Memory[0x20000 + 0xFC] == 0x00 && g_Memory[0x20000 + 0xFD] == 0x10,
            "int: return CS is the interrupted segment");
      CHECK(InterpTestStepOnce(&cpu) == 1, "CF: IRET is modeled");
      CHECK(cpu.Segments[1] == 0x1000 && cpu.Ip == 3, "iret: back past the INT");
      CHECK(cpu.Registers[4] == 0x0100, "iret: stack unwound");
      CHECK(cpu.Flags & 0x200u, "iret: IF restored from the pushed FLAGS"); }

    /* T18c: far JMP / far CALL: */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x9A, 0x11, 0x22, 0x00, 0x30 };  /* CALL 3000:2211 */
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      cpu.Segments[2] = 0x2000;
      cpu.Registers[4] = 0x0100;
      CHECK(InterpTestStepOnce(&cpu) == 1 && cpu.Segments[1] == 0x3000 && cpu.Ip == 0x2211, "9A: far CALL transfers");
      CHECK(cpu.Registers[4] == 0x00FC, "far call: pushed CS:IP (SP -= 4)");
      CHECK(g_Memory[0x20000 + 0xFE] == 0x00 && g_Memory[0x20000 + 0xFF] == 0x10, "far call: pushed CS");
      { BYTE moreBytes[] = { 0xEA, 0x00, 0x01, 0x00, 0x40 };    /* JMP 4000:0100 */
        InterpTestLoad(&cpu, 0x3000, 0x2211, moreBytes, sizeof moreBytes);
        CHECK(InterpTestStepOnce(&cpu) == 1 && cpu.Segments[1] == 0x4000 && cpu.Ip == 0x0100, "EA: far JMP transfers"); } }

    /* T19: 32-bit operand-size (0x66) -- run 54:
     * A C runtime under DPMI does 32-bit register math in a 16-bit segment  *
     * via the 0x66 prefix (run 53's I310102 stopped on MOVZX ESI,SI). These *
     * exercise the widened register file + width-aware helpers.
     */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x66, 0x0F, 0xB7, 0xF6 };   /* MOVZX ESI,SI */
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      cpu.Registers[6] = 0x1234ABCD;
      CHECK(InterpTestStepOnce(&cpu) == 1 && cpu.Ip == 4, "66 0F B7: MOVZX ESI,SI runs, ip += 4");
      CHECK(cpu.Registers[6] == 0x0000ABCD, "movzx: ESI = zero-extended SI"); }

    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x66, 0xC1, 0xE6, 0x04 };   /* SHL ESI,4 */
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      cpu.Registers[6] = 0x0000ABCD;
      CHECK(InterpTestStepOnce(&cpu) == 1 && cpu.Registers[6] == 0x000ABCD0, "66 C1 /4: SHL ESI,4 (32-bit)"); }

    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x66, 0xB8, 0x78, 0x56, 0x34, 0x12 };  /* MOV EAX,imm32 */
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      CHECK(InterpTestStepOnce(&cpu) == 1 && cpu.Ip == 6, "66 B8: MOV EAX,imm32 runs, ip += 6");
      CHECK(cpu.Registers[0] == 0x12345678, "mov: EAX = imm32"); }

    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x66, 0x01, 0xC0 };         /* ADD EAX,EAX */
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      cpu.Registers[0] = 0x80000000u;
      CHECK(InterpTestStepOnce(&cpu) == 1 && cpu.Registers[0] == 0, "66 01: ADD EAX,EAX = 0 (32-bit wrap)");
      CHECK((cpu.Flags & EFLAGS_CF_U) && (cpu.Flags & EFLAGS_ZF_U), "add32: CF+ZF at 32-bit boundary"); }

    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x66, 0x0F, 0xBE, 0xC0 };   /* MOVSX EAX,AL */
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      cpu.Registers[0] = 0x00000080u;
      CHECK(InterpTestStepOnce(&cpu) == 1 && cpu.Registers[0] == 0xFFFFFF80u, "66 0F BE: MOVSX EAX,AL sign-extends"); }

    /* partial-register semantics: a 16-bit write preserves E-reg[31:16] */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0xB8, 0xCD, 0xAB };         /* MOV AX,0xABCD (no 0x66) */
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      cpu.Registers[0] = 0x12345678u;
      CHECK(InterpTestStepOnce(&cpu) == 1 && cpu.Registers[0] == 0x1234ABCDu, "mov ax preserves high EAX"); }

    /* PUSH/POP r32 with 0x66: 4-byte stack slot, SP +/- 4 */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x66, 0x53, 0x66, 0x5B };   /* PUSH EBX; POP EBX */
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      cpu.Segments[2] = 0x0000;
      cpu.Registers[4] = 0x0100;
      cpu.Registers[3] = 0xCAFEF00Du;
      CHECK(InterpTestStepOnce(&cpu) == 1 && (cpu.Registers[4] & 0xFFFF) == 0x00FC, "66 push ebx: SP -= 4");
      CHECK(V86ReadMemory(0xFC, 4) == 0xCAFEF00Du, "66 push ebx: 4 bytes on stack");
      cpu.Registers[3] = 0;
      CHECK(InterpTestStepOnce(&cpu) == 1 && cpu.Registers[3] == 0xCAFEF00Du && (cpu.Registers[4] & 0xFFFF) == 0x0100,
            "66 pop ebx: value + SP restored"); }

    /* a bare 0x66 before a byte op is ignored (operand size irrelevant) */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x66, 0x04, 0x01 };         /* ADD AL,1 (66 ignored) */
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      cpu.Registers[0] = 0x05;
      CHECK(InterpTestStepOnce(&cpu) == 1 && (cpu.Registers[0] & 0xFF) == 0x06, "66 before byte-op: width stays 1"); }

    /* #269: Jcc rel16/rel32 (0F 8x), SETcc (0F 9x), ENTER n,0 (C8): */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x0F, 0x84, 0x00, 0x01 };            /* jz +0x100 */
      cpu.Flags |= EFLAGS_ZF_U;
      InterpTestLoad(&cpu, 0x1000, 0x10, bytes, sizeof bytes);
      CHECK(InterpTestStepOnce(&cpu) == 1 && cpu.Ip == 0x0114, "0F 84 jz rel16 taken: ip = next + 0x100"); }
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x0F, 0x84, 0x00, 0x01 };
      InterpTestLoad(&cpu, 0x1000, 0x10, bytes, sizeof bytes);
      CHECK(InterpTestStepOnce(&cpu) == 1 && cpu.Ip == 0x0014, "0F 84 jz rel16 not taken: ip = next"); }
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x0F, 0x85, 0xF0, 0xFF };            /* jnz -0x10 */
      InterpTestLoad(&cpu, 0x1000, 0x0004, bytes, sizeof bytes);
      CHECK(InterpTestStepOnce(&cpu) == 1 && cpu.Ip == 0xFFF8, "0F 85 jnz rel16 backwards wraps in IP"); }
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x66, 0x0F, 0x82, 0x10, 0x00, 0x00, 0x00 }; /* jc rel32 */
      cpu.Flags |= EFLAGS_CF_U;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      CHECK(InterpTestStepOnce(&cpu) == 1 && cpu.Ip == 0x0017, "66 0F 82 jc rel32 taken: ip = next + 0x10"); }
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x66, 0x0F, 0x82, 0x00, 0x00, 0x01, 0x00 }; /* out of segment */
      cpu.Flags |= EFLAGS_CF_U;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      CHECK(InterpTestStepOnce(&cpu) == 0, "66 0F 82 jc rel32 past 64 KB: bail (the CPU's #GP)"); }
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x0F, 0x9C, 0xC1 };                  /* setl cl */
      cpu.Registers[1] = 0xFFFF;
      cpu.Flags |= EFLAGS_SF_U;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      CHECK(InterpTestStepOnce(&cpu) == 1 && cpu.Registers[1] == 0xFF01 && cpu.Ip == 3, "0F 9C setl cl: SF!=OF -> CL=1, CH kept"); }
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x0F, 0x94, 0x06, 0x00, 0x05 };      /* sete [0500h] */
      cpu.Segments[3] = 0x2000;
      g_Memory[0x20500] = 0x77;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      CHECK(InterpTestStepOnce(&cpu) == 1 && g_Memory[0x20500] == 0x00 && cpu.Ip == 5, "0F 94 sete [m]: ZF=0 -> 0"); }
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0xC8, 0x10, 0x00, 0x00 };            /* enter 0x10,0 */
      cpu.Segments[2] = 0x2000;
      cpu.Registers[4] = 0xABCD0100u;
      cpu.Registers[5] = 0x1234BEEFu;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      CHECK(InterpTestStepOnce(&cpu) == 1 && V86ReadMemory(0x200FE, 2) == 0xBEEF && cpu.Registers[5] == 0x123400FEu
            && cpu.Registers[4] == 0xABCD00EEu && cpu.Ip == 4,
            "C8 enter 0x10,0: push BP, BP = SP, SP -= 0x10 (high halves kept)"); }
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0xC8, 0x10, 0x00, 0x01 };            /* enter 0x10,1 */
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      CHECK(InterpTestStepOnce(&cpu) == 0, "C8 enter with a nesting level: bail"); }

    /* T20: LAR/LSL descriptor introspection -- run 55:
     * A DPMI C runtime reads a descriptor's access byte with LAR;CX / SHR.  *
     * In V86 (g_V86SelectorDescriptor==NULL) these bail; with the hook they consult it.
     */
    g_V86SelectorDescriptor = InterpTestLarLslDescriptor;
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x66, 0x0F, 0x02, 0xC9 };   /* LAR ECX,CX */
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      cpu.Registers[1] = 0x0008;         /* CX = sel 0x08 (code 0xFA) */
      CHECK(InterpTestStepOnce(&cpu) == 1 && cpu.Ip == 4, "66 0F 02: LAR ECX,CX runs, ip += 4");
      CHECK(cpu.Registers[1] == 0x0000FA00 && (cpu.Flags & EFLAGS_ZF_U), "lar: ECX = access<<8, ZF set (valid sel)"); }

    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x66, 0x0F, 0x02, 0xC9, 0x66, 0xC1, 0xE9, 0x08 }; /* LAR;SHR ECX,8 */
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      cpu.Registers[1] = 0x0010;         /* CX = sel 0x10 (data 0xF3) */
      InterpTestStepOnce(&cpu);
      InterpTestStepOnce(&cpu);                                      /* LAR then SHR ECX,8 */
      CHECK((cpu.Registers[1] & 0xFF) == 0xF3, "lar+shr: CL = descriptor access byte (the C-runtime idiom)"); }

    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x66, 0x0F, 0x02, 0xC9 };   /* LAR ECX,CX -- invalid sel */
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      cpu.Registers[1] = 0x0000;
      cpu.Flags |= EFLAGS_ZF_U; /* ZF preset */
      CHECK(InterpTestStepOnce(&cpu) == 1 && !(cpu.Flags & EFLAGS_ZF_U), "lar: invalid selector clears ZF");
      CHECK(cpu.Registers[1] == 0x0000, "lar: dest unchanged on invalid selector"); }

    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x66, 0x0F, 0x03, 0xC9 };   /* LSL ECX,CX */
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      cpu.Registers[1] = 0x0010;         /* CX = sel 0x10 (limit 0x25CF) */
      CHECK(InterpTestStepOnce(&cpu) == 1 && cpu.Registers[1] == 0x000025CF && (cpu.Flags & EFLAGS_ZF_U),
            "66 0F 03: LSL ECX,CX = byte limit, ZF set"); }
    g_V86SelectorDescriptor = 0;

    /* T20: full read-scan fill loop, exit by counter:
     * MOV AL,ES:[SI] / OR AL,AL / JNZ found / INC SI / DEC DI / JNZ loop    *
     * with all-zero pixels and DI=4: runs 4 iterations, then falls to a     *
     * bail opcode -- exactly the BUBBLES PAINT pattern, all in the host.
     */
    { V86_CPU cpu = InterpTestMakeCpu();
      BYTE bytes[] = {
        /*00 */ 0x26, 0x8A, 0x04,    /* MOV AL, ES:[SI] */
        /*03 */ 0x0A, 0xC0,          /* OR  AL, AL */
        /*05 */ 0x75, 0x06,          /* JNZ found(+6 -> 0x0D) */
        /*07 */ 0x46,                /* INC SI */
        /*08 */ 0x4F,                /* DEC DI */
        /*09 */ 0x75, 0xF5,          /* JNZ loop(-11 -> 0x00) */
        /*0B */ 0x90, 0x90,          /* (pad) */
        /*0D */ 0xF4                 /* HLT (unmodeled -> bail) */
      };
      cpu.Segments[0] = 0xA000;
      cpu.Registers[6] = 0;
      cpu.Registers[7] = 4;       /* ES, SI, DI */
      /* pixels all zero already (MEM is zeroed) */
      InterpTestLoad(&cpu, 0x4000, 0, bytes, sizeof bytes);
      InterpTestRun(&cpu);
      CHECK(cpu.Registers[7] == 0 && cpu.Registers[6] == 4, "scan: counter loop exits at DI=0, SI=4");
      CHECK(cpu.Ip == 0x0D, "scan: bailed exactly on the HLT"); }

    /* T21: same scan, early exit on a nonzero pixel: */
    { V86_CPU cpu = InterpTestMakeCpu();
      BYTE bytes[] = {
        0x26, 0x8A, 0x04, 0x0A, 0xC0, 0x75, 0x06,
        0x46, 0x4F, 0x75, 0xF5, 0x90, 0x90, 0xF4
      };
      cpu.Segments[0] = 0xA000;
      cpu.Registers[6] = 0;
      cpu.Registers[7] = 8;
      g_Memory[((UINT32)0xA000 << 4) + 2] = 0x77;         /* nonzero at offset 2 */
      InterpTestLoad(&cpu, 0x4000, 0, bytes, sizeof bytes);
      InterpTestRun(&cpu);
      CHECK((cpu.Registers[0] & 0xFF) == 0x77 && cpu.Registers[6] == 2 && cpu.Registers[7] == 6,
            "scan: stops on nonzero pixel (AL=77, SI=2, DI=6)");
      CHECK(cpu.Ip == 0x0D, "scan: nonzero exit bails on the HLT"); }

    /* T22: XCHG r/m8,r8 with memory (QB pixel plot): */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x26, 0x86, 0x05 };   /* XCHG ES:[DI],AL */
      cpu.Segments[0] = 0x7000;
      cpu.Registers[7] = 0x0004;
      cpu.Registers[0] = 0x00C3;     /* ES, DI, AL */
      g_Memory[((UINT32)0x7000 << 4) + 4] = 0x2A;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      InterpTestStepOnce(&cpu);
      CHECK(g_Memory[((UINT32)0x7000<<4)+4] == 0xC3, "xchg: memory got AL");
      CHECK((cpu.Registers[0] & 0xFF) == 0x2A, "xchg: AL got old memory value"); }

    /* T23: XCHG r16,r16 (reg-reg): */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x87, 0xD8 };    /* XCHG AX,BX */
      cpu.Registers[0] = 0x1111;
      cpu.Registers[3] = 0x2222;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      InterpTestStepOnce(&cpu);
      CHECK(cpu.Registers[0] == 0x2222 && cpu.Registers[3] == 0x1111, "xchg: AX<->BX"); }

    /* T24: PUSH then POP round-trips through SS:SP: */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x51, 0x5A };    /* PUSH CX ; POP DX */
      cpu.Segments[2] = 0x8000;
      cpu.Registers[4] = 0x0100;
      cpu.Registers[1] = 0xBEEF;    /* SS, SP, CX */
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      InterpTestStepOnce(&cpu);
      CHECK(cpu.Registers[4] == 0x00FE, "push: SP -= 2");
      CHECK(g_Memory[((UINT32)0x8000<<4)+0xFE]==0xEF && g_Memory[((UINT32)0x8000<<4)+0xFF]==0xBE,
            "push: word written at SS:SP");
      InterpTestStepOnce(&cpu);
      CHECK(cpu.Registers[2] == 0xBEEF && cpu.Registers[4] == 0x0100, "pop: DX=CX, SP restored"); }

    /* T25: OUT imm8 + IN DX dispatched to the port hooks: */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0xE6, 0x3C };    /* OUT 3Ch... no: imm port 0x3C */
      /* use OUT DX,AL to port 0x3C5, then IN AL,0x60 */
      BYTE moreBytes[] = { 0xEE, 0xE4, 0x60 };                /* OUT DX,AL ; IN AL,60h */
      (VOID)bytes;
      cpu.Registers[2] = 0x3C5;
      cpu.Registers[0] = 0x0042;                /* DX=3C5, AL=0x42 */
      InterpTestLoad(&cpu, 0x1000, 0, moreBytes, sizeof moreBytes);
      InterpTestStepOnce(&cpu);
      CHECK(g_Port3C5 == 0x42, "out: DX(3C5) <- AL via bus");
      InterpTestStepOnce(&cpu);
      CHECK((cpu.Registers[0] & 0xFF) == 0xA5, "in: AL <- port 0x60 via bus"); }

    /* T26: CALL near relative + RET round-trip:
     * 00 MOV AX,1234 / 03 CALL +4 / 06 INC AX / 07 HLT / 0A INC BX / 0B RET
     */
    { V86_CPU cpu = InterpTestMakeCpu();
      BYTE bytes[] = { 0xB8,0x34,0x12, 0xE8,0x04,0x00, 0x40, 0xF4, 0x90,0x90, 0x43, 0xC3 };
      cpu.Segments[2] = 0x9000;
      cpu.Registers[4] = 0x0200;             /* SS, SP */
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      InterpTestRun(&cpu);
      CHECK(cpu.Registers[0] == 0x1235, "call/ret: AX=1235 (INC AX after return)");
      CHECK(cpu.Registers[3] == 0x0001, "call/ret: BX=1 (subroutine ran)");
      CHECK(cpu.Registers[4] == 0x0200, "call/ret: SP restored");
      CHECK(cpu.Ip == 0x07, "call/ret: bailed on HLT after return"); }

    /* T27: CALL near indirect via register (FF /2):
     * 00 CALL SI(=06) / 02 INC AX / 03 HLT / 06 RET
     */
    { V86_CPU cpu = InterpTestMakeCpu();
      BYTE bytes[] = { 0xFF,0xD6, 0x40, 0xF4, 0x90,0x90, 0xC3 };
      cpu.Segments[2] = 0x9000;
      cpu.Registers[4] = 0x0200;
      cpu.Registers[6] = 0x0006;   /* SS, SP, SI */
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      InterpTestRun(&cpu);
      CHECK(cpu.Registers[0] == 0x0001 && cpu.Registers[4] == 0x0200 && cpu.Ip == 0x03,
            "call indirect: ran subroutine via SI, SP restored"); }

    /* T28: SHR builds a bit-mask (QB's 0x80 >> x): */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0xD2, 0xE8 };   /* SHR AL, CL */
      cpu.Registers[0] = 0x0080;
      cpu.Registers[1] = 0x0003;              /* AL=0x80, CL=3 */
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      InterpTestStepOnce(&cpu);
      CHECK((cpu.Registers[0] & 0xFF) == 0x10, "shr: 0x80 >> 3 = 0x10"); }

    /* T29: SHL by 1 sets CF from the bit shifted out: */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0xD0, 0xE0 };   /* SHL AL, 1 */
      cpu.Registers[0] = 0x00C0;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      InterpTestStepOnce(&cpu);
      CHECK((cpu.Registers[0] & 0xFF) == 0x80, "shl: 0xC0 << 1 = 0x80");
      CHECK((cpu.Flags & EFLAGS_CF_U) != 0, "shl: CF = bit shifted out"); }

    /* T30: ROR by 1, CF = rotated bit; result wraps: */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0xD0, 0xC8 };   /* ROR AL, 1 */
      cpu.Registers[0] = 0x0001;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      InterpTestStepOnce(&cpu);
      CHECK((cpu.Registers[0] & 0xFF) == 0x80 && (cpu.Flags & EFLAGS_CF_U), "ror: 0x01 ror 1 = 0x80, CF=1"); }

    /* T31: SHR imm8 (C0 /5) with flags: */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0xC0, 0xE8, 0x04 };   /* SHR AL, 4 */
      cpu.Registers[0] = 0x00A5;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      InterpTestStepOnce(&cpu);
      CHECK((cpu.Registers[0] & 0xFF) == 0x0A, "shr: 0xA5 >> 4 = 0x0A"); }

    /* T32: PUSH/POP ES round-trips (the per-pixel bail): */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x06, 0x1F };   /* PUSH ES ; POP DS */
      cpu.Segments[2] = 0x9000;
      cpu.Registers[4] = 0x0100;
      cpu.Segments[0] = 0xA000;  /* SS,SP,ES */
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      InterpTestStepOnce(&cpu);
      CHECK(cpu.Registers[4] == 0x00FE, "push ES: SP -= 2");
      InterpTestStepOnce(&cpu);
      CHECK(cpu.Segments[3] == 0xA000 && cpu.Registers[4] == 0x0100, "pop DS = pushed ES"); }

    /* T33: MOV Sreg,r/m and MOV r/m,Sreg: */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x8E, 0xC0, 0x8C, 0xC3 };  /* MOV ES,AX ; MOV BX,ES */
      cpu.Registers[0] = 0xB800;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      InterpTestStepOnce(&cpu);
      CHECK(cpu.Segments[0] == 0xB800, "mov ES,AX");
      InterpTestStepOnce(&cpu);
      CHECK(cpu.Registers[3] == 0xB800, "mov BX,ES"); }

    /* T34: LEA loads the offset, not the memory contents: */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x8D, 0x41, 0x06 };   /* LEA AX,[BX+DI+6] */
      cpu.Registers[3] = 0x0010;
      cpu.Registers[7] = 0x0004;              /* BX, DI */
      g_Memory[0x1A] = 0xFF;                              /* would be wrong to load */
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      InterpTestStepOnce(&cpu);
      CHECK(cpu.Registers[0] == 0x001A, "lea: AX = BX+DI+6 = 0x1A (offset, not [0x1A])"); }

    /* T35: PUSH imm16 (68) writes a W-wide slot; SP -= 2 -- run 56:
     * The exact opcode run 55 stopped on: 68 3a 02 = PUSH 0x023A.
     */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x68, 0x3A, 0x02 };   /* PUSH 0x023A */
      cpu.Segments[2] = 0x8000;
      cpu.Registers[4] = 0x0100;                  /* SS, SP */
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      CHECK(InterpTestStepOnce(&cpu) == 1 && cpu.Ip == 3 && cpu.Registers[4] == 0x00FE, "68: PUSH imm16, ip+=3, SP-=2");
      CHECK(g_Memory[((UINT32)0x8000<<4)+0xFE]==0x3A && g_Memory[((UINT32)0x8000<<4)+0xFF]==0x02,
            "push imm16: word 0x023A written at SS:SP"); }

    /* T36: PUSH imm8 (6A) sign-extends to the 16-bit slot: */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x6A, 0xFF };         /* PUSH -1 (byte) */
      cpu.Segments[2] = 0x8000;
      cpu.Registers[4] = 0x0100;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      CHECK(InterpTestStepOnce(&cpu) == 1 && cpu.Ip == 2 && cpu.Registers[4] == 0x00FE, "6A: PUSH imm8, ip+=2, SP-=2");
      CHECK(g_Memory[((UINT32)0x8000<<4)+0xFE]==0xFF && g_Memory[((UINT32)0x8000<<4)+0xFF]==0xFF,
            "push imm8: -1 sign-extended to 0xFFFF"); }

    /* T37: PUSH imm round-trips through POP (value + flags intact): */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x6A, 0x7F, 0x58 };   /* PUSH 0x7F ; POP AX */
      cpu.Segments[2] = 0x8000;
      cpu.Registers[4] = 0x0100;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      InterpTestStepOnce(&cpu);
      InterpTestStepOnce(&cpu);
      CHECK(cpu.Registers[0] == 0x007F && cpu.Registers[4] == 0x0100, "push imm8/pop: AX=0x7F, SP restored"); }

    /* T38: 32-bit PUSH imm32 (66 68) -- a 32-bit C runtime arg: */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x66, 0x68, 0x78, 0x56, 0x34, 0x12 }; /* PUSH 0x12345678 */
      cpu.Segments[2] = 0x8000;
      cpu.Registers[4] = 0x0100;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      CHECK(InterpTestStepOnce(&cpu) == 1 && cpu.Ip == 6 && cpu.Registers[4] == 0x00FC, "66 68: PUSH imm32, ip+=6, SP-=4");
      CHECK(g_Memory[((UINT32)0x8000<<4)+0xFC]==0x78 && g_Memory[((UINT32)0x8000<<4)+0xFF]==0x12,
            "push imm32: dword 0x12345678 written at SS:SP"); }

    /* T39: RETF (CB) pops offset then a 2-byte selector into CS -- run 57:
     * The far-return that follows run 56's `PUSH seg; PUSH off; RETF` idiom.
     */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0xCB };               /* RETF */
      cpu.Segments[2] = 0x8000;
      cpu.Registers[4] = 0x0100;                  /* SS, SP */
      g_Memory[((UINT32)0x8000<<4)+0x100] = 0x34;             /* [SP]   = offset 0x1234 */
      g_Memory[((UINT32)0x8000<<4)+0x101] = 0x12;
      g_Memory[((UINT32)0x8000<<4)+0x102] = 0x78;             /* [SP+2] = selector 0x5678 */
      g_Memory[((UINT32)0x8000<<4)+0x103] = 0x56;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      InterpTestStepOnce(&cpu);
      CHECK(cpu.Ip == 0x1234 && cpu.Segments[1] == 0x5678, "CB: RETF sets IP=off, CS=selector");
      CHECK(cpu.Registers[4] == 0x0104, "retf: SP += 4 (offset + selector)"); }

    /* T40: RETF imm16 (CA) also releases imm16 stack bytes: */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0xCA, 0x08, 0x00 };   /* RETF 8 */
      cpu.Segments[2] = 0x8000;
      cpu.Registers[4] = 0x0100;
      g_Memory[((UINT32)0x8000<<4)+0x100] = 0x00;
      g_Memory[((UINT32)0x8000<<4)+0x101] = 0x02; /* off 0x0200 */
      g_Memory[((UINT32)0x8000<<4)+0x102] = 0x0F;
      g_Memory[((UINT32)0x8000<<4)+0x103] = 0x00; /* sel 0x000F */
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      InterpTestStepOnce(&cpu);
      CHECK(cpu.Ip == 0x0200 && cpu.Segments[1] == 0x000F, "CA: RETF imm16 sets CS:IP");
      CHECK(cpu.Registers[4] == 0x010C, "retf imm16: SP += 4 + 8"); }

    /* T41: the full idiom -- PUSH seg; PUSH off; RETF far-transfers:
     * V86SegmentBase = seg<<4 here (g_V86SegmentToLinear NULL), so CS=0x0800 lands code at 0x8000.
     */
    { V86_CPU cpu = InterpTestMakeCpu();
      BYTE code[] = { 0x68, 0x00, 0x08,   /* PUSH 0x0800 (target segment) */
                      0x68, 0x00, 0x01,   /* PUSH 0x0100 (target offset) */
                      0xCB };             /* RETF -> 0x0800:0x0100 */
      cpu.Segments[2] = 0x9000;
      cpu.Registers[4] = 0x0200;                 /* SS, SP */
      InterpTestLoad(&cpu, 0x1000, 0, code, sizeof code);
      g_Memory[((UINT32)0x0800<<4)+0x100] = 0xF4;            /* HLT at the target -> run() bails */
      InterpTestRun(&cpu);
      CHECK(cpu.Segments[1] == 0x0800 && cpu.Ip == 0x0100, "push seg/off + RETF: transferred to 0800:0100");
      CHECK(cpu.Registers[4] == 0x0200, "far-transfer: SP back to start (2 pushes + retf pop 4)"); }

    /* T42: LEAVE (C9) -- MOV SP,BP; POP BP, the callee epilogue -- run 58:
     * SP starts below BP (locals allocated); LEAVE discards them (SP<-BP) then   *
     * pops the caller's BP. Paired with ENTER / `PUSH BP; MOV BP,SP`.
     */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0xC9 };               /* LEAVE */
      cpu.Segments[2] = 0x8000;
      cpu.Registers[4] = 0x00F8;
      cpu.Registers[5] = 0x0100;  /* SS, SP (locals), BP */
      g_Memory[((UINT32)0x8000<<4)+0x100] = 0xBC;             /* [BP] = caller's BP 0x0ABC */
      g_Memory[((UINT32)0x8000<<4)+0x101] = 0x0A;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      InterpTestStepOnce(&cpu);
      CHECK(cpu.Ip == 1, "C9: LEAVE, ip += 1");
      CHECK(cpu.Registers[5] == 0x0ABC, "leave: BP <- caller's BP popped from [old BP]");
      CHECK(cpu.Registers[4] == 0x0102, "leave: SP <- BP then +2 (locals discarded, BP popped)"); }

    /* T43: LEAVE preserves the high 16 bits of ESP/EBP (partial-reg): */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0xC9 };               /* LEAVE */
      cpu.Segments[2] = 0x8000;
      cpu.Registers[4] = 0xDEAD00F8;
      cpu.Registers[5] = 0xBEEF0100;
      g_Memory[((UINT32)0x8000<<4)+0x100] = 0xBC;
      g_Memory[((UINT32)0x8000<<4)+0x101] = 0x0A;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      InterpTestStepOnce(&cpu);
      CHECK(cpu.Registers[5] == 0xBEEF0ABC && cpu.Registers[4] == 0xDEAD0102,
            "leave: E-reg high halves of SP/BP preserved (16-bit LEAVE)"); }

    /* T44: PUSHF (9C) -- push the modeled FLAGS + reserved bit 1 -- run 59:
     * SP -= 2; [SP] = (flags & modeled-mask) | 0x0002. IF/TF/IOPL/NT not modeled.
     */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x9C };               /* PUSHF */
      cpu.Segments[2] = 0x8000;
      cpu.Registers[4] = 0x0100;
      cpu.Flags = EFLAGS_CF_U | EFLAGS_ZF_U | EFLAGS_SF_U | EFLAGS_DF_U;                /* 0x04C1 + reserved */
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      InterpTestStepOnce(&cpu);
      CHECK(cpu.Ip == 1, "9C: PUSHF, ip += 1");
      CHECK(cpu.Registers[4] == 0x00FE, "pushf: SP -= 2");
      { WORD word = g_Memory[((UINT32)0x8000<<4)+0xFE] | (g_Memory[((UINT32)0x8000<<4)+0xFF]<<8);
        CHECK(word == ((EFLAGS_CF_U|EFLAGS_ZF_U|EFLAGS_SF_U|EFLAGS_DF_U) | 0x0002u), "pushf: pushed FLAGS = modeled bits + reserved bit 1"); } }

    /* T45: POPF (9D) -- load FLAGS from stack (modeled bits only): */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x9D };               /* POPF */
      cpu.Segments[2] = 0x8000;
      cpu.Registers[4] = 0x00FE;
      g_Memory[((UINT32)0x8000<<4)+0xFE] = (EFLAGS_CF_U|EFLAGS_OF_U|EFLAGS_PF_U) & 0xFF; /* low byte 0x05 */
      g_Memory[((UINT32)0x8000<<4)+0xFF] = ((EFLAGS_OF_U) >> 8) & 0xFF;    /* high byte 0x08 (OF) */
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      InterpTestStepOnce(&cpu);
      CHECK(cpu.Registers[4] == 0x0100, "popf: SP += 2");
      CHECK((cpu.Flags & (EFLAGS_CF_U|EFLAGS_PF_U|EFLAGS_OF_U)) == (EFLAGS_CF_U|EFLAGS_PF_U|EFLAGS_OF_U), "popf: CF/PF/OF restored from stack");
      CHECK((cpu.Flags & 0x0002u) && !(cpu.Flags & EFLAGS_ZF_U), "popf: reserved bit set, ZF cleared (not on stack)"); }

    /* T46: PUSHF/POPF round-trip is exact for modeled flags; SP high half kept */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x9C, 0x31, 0xC0, 0x9D };  /* PUSHF; XOR AX,AX; POPF */
      cpu.Segments[2] = 0x8000;
      cpu.Registers[4] = 0xCAFE0100;
      cpu.Flags = EFLAGS_AF_U | EFLAGS_SF_U | EFLAGS_DF_U;                       /* clobbered by XOR, restored by POPF */
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      InterpTestStepOnce(&cpu);
      InterpTestStepOnce(&cpu);
      InterpTestStepOnce(&cpu);
      CHECK((cpu.Flags & (EFLAGS_AF_U|EFLAGS_SF_U|EFLAGS_DF_U)) == (EFLAGS_AF_U|EFLAGS_SF_U|EFLAGS_DF_U), "pushf/popf: modeled flags round-trip exactly");
      CHECK(cpu.Registers[4] == 0xCAFE0100, "pushf/popf: SP back to start, E-reg high half preserved"); }

    /* T47: XCHG AX,SI (96) -- accumulator short-form swap, no flags -- run 60 */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x96 };               /* XCHG AX,SI */
      cpu.Registers[0] = 0x1234;
      cpu.Registers[6] = 0xABCD;
      cpu.Flags = EFLAGS_CF_U | EFLAGS_ZF_U;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      InterpTestStepOnce(&cpu);
      CHECK(cpu.Ip == 1, "96: XCHG AX,SI, ip += 1");
      CHECK(cpu.Registers[0] == 0xABCD && cpu.Registers[6] == 0x1234, "xchg ax,si: AX<->SI swapped");
      CHECK((cpu.Flags & (EFLAGS_CF_U|EFLAGS_ZF_U)) == (EFLAGS_CF_U|EFLAGS_ZF_U), "xchg: flags untouched"); }

    /* T48: XCHG AX,AX (90) is a NOP; XCHG preserves E-reg high halves (16-bit) */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x90, 0x91 };         /* NOP; XCHG AX,CX */
      cpu.Registers[0] = 0xDEAD1234;
      cpu.Registers[1] = 0xBEEFABCD;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      InterpTestStepOnce(&cpu);
      CHECK(cpu.Registers[0] == 0xDEAD1234, "90: NOP leaves AX unchanged");
      InterpTestStepOnce(&cpu);
      CHECK(cpu.Registers[0] == 0xDEADABCD && cpu.Registers[1] == 0xBEEF1234,
            "xchg ax,cx: 16-bit views swap, E-reg high halves preserved"); }

    /* T49: 0x66 XCHG EAX,r32 -- full 32-bit swap: */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x66, 0x93 };         /* XCHG EAX,EBX */
      cpu.Registers[0] = 0x11223344;
      cpu.Registers[3] = 0x55667788;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      InterpTestStepOnce(&cpu);
      CHECK(cpu.Registers[0] == 0x55667788 && cpu.Registers[3] == 0x11223344, "66 93: XCHG EAX,EBX full 32-bit swap"); }

    /* T50: F7 group -- NOT (reg 2), no flags -- run 61: */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0xF7, 0xD6 };        /* NOT SI */
      cpu.Registers[6] = 0x1234;
      cpu.Flags = EFLAGS_CF_U | 0x0002;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      InterpTestStepOnce(&cpu);
      CHECK((cpu.Registers[6] & 0xFFFF) == 0xEDCB, "F7 /2: NOT SI = ~0x1234");
      CHECK(cpu.Flags & EFLAGS_CF_U, "not: flags untouched"); }

    /* T51: NEG (reg 3) -- 0 - e, flags like SUB: */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0xF7, 0xD8 };        /* NEG AX */
      cpu.Registers[0] = 0x0005;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      InterpTestStepOnce(&cpu);
      CHECK((cpu.Registers[0] & 0xFFFF) == 0xFFFB, "F7 /3: NEG AX(5) = 0xFFFB");
      CHECK((cpu.Flags & EFLAGS_CF_U) && (cpu.Flags & EFLAGS_SF_U), "neg 5: CF set (nonzero) + SF"); }
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0xF7, 0xD8 };        /* NEG AX, AX=0 */
      cpu.Registers[0] = 0x0000;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      InterpTestStepOnce(&cpu);
      CHECK((cpu.Registers[0] & 0xFFFF) == 0 && (cpu.Flags & EFLAGS_ZF_U) && !(cpu.Flags & EFLAGS_CF_U), "neg 0 = 0, ZF, no CF"); }

    /* T52: MUL (reg 4) -- unsigned, DX:AX <- AX*r/m: */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0xF7, 0xE1 };        /* MUL CX */
      cpu.Registers[0] = 0x1000;
      cpu.Registers[1] = 0x0010;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      InterpTestStepOnce(&cpu);
      CHECK((cpu.Registers[0] & 0xFFFF) == 0x0000 && (cpu.Registers[2] & 0xFFFF) == 0x0001, "F7 /4: MUL CX -> DX:AX=0001:0000");
      CHECK((cpu.Flags & EFLAGS_CF_U) && (cpu.Flags & EFLAGS_OF_U), "mul: CF/OF set (DX nonzero)"); }
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0xF7, 0xE1 };        /* MUL CX, small */
      cpu.Registers[0] = 3;
      cpu.Registers[1] = 4;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      InterpTestStepOnce(&cpu);
      CHECK((cpu.Registers[0] & 0xFFFF) == 12 && (cpu.Registers[2] & 0xFFFF) == 0, "mul 3*4=12, DX=0");
      CHECK(!(cpu.Flags & EFLAGS_CF_U) && !(cpu.Flags & EFLAGS_OF_U), "mul: CF/OF clear (fits)"); }

    /* T53: IMUL (reg 5) -- signed: */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0xF7, 0xE9 };        /* IMUL CX */
      cpu.Registers[0] = 0xFFFE;
      cpu.Registers[1] = 0x0003;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      InterpTestStepOnce(&cpu);   /* -2 * 3 */
      CHECK((cpu.Registers[0] & 0xFFFF) == 0xFFFA && (cpu.Registers[2] & 0xFFFF) == 0xFFFF, "F7 /5: IMUL -2*3 = -6 (FFFF:FFFA)");
      CHECK(!(cpu.Flags & EFLAGS_CF_U) && !(cpu.Flags & EFLAGS_OF_U), "imul: CF/OF clear (fits 16-bit signed)"); }

    /* T54: DIV (reg 6) -- unsigned, quotient AX, remainder DX: */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0xF7, 0xF1 };        /* DIV CX */
      cpu.Registers[0] = 0x0000;
      cpu.Registers[2] = 0x0001;
      cpu.Registers[1] = 0x0010;  /* DX:AX = 0x10000 / 0x10 */
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      InterpTestStepOnce(&cpu);
      CHECK((cpu.Registers[0] & 0xFFFF) == 0x1000 && (cpu.Registers[2] & 0xFFFF) == 0x0000, "F7 /6: DIV CX 0x10000/0x10 -> AX=0x1000 DX=0"); }

    /* T55: 66 F7 /6 = DIV EDI -- the exact i310102 hex-format opcode: */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x66, 0xF7, 0xF7 };  /* DIV EDI */
      cpu.Registers[0] = 100;
      cpu.Registers[2] = 0;
      cpu.Registers[7] = 7;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      InterpTestStepOnce(&cpu);
      CHECK(cpu.Registers[0] == 14 && cpu.Registers[2] == 2, "66 F7 /6: DIV EDI 100/7 -> EAX=14 EDX=2 (the run-60 wall op)"); }

    /* T56: IDIV (reg 7) -- signed: */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0xF7, 0xF9 };        /* IDIV CX */
      cpu.Registers[0] = 0xFF9C;
      cpu.Registers[2] = 0xFFFF;
      cpu.Registers[1] = 0x0007;  /* DX:AX = -100 / 7 */
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      InterpTestStepOnce(&cpu);
      CHECK((cpu.Registers[0] & 0xFFFF) == 0xFFF2 && (cpu.Registers[2] & 0xFFFF) == 0xFFFE, "F7 /7: IDIV -100/7 -> AX=-14 DX=-2"); }

    /* T57: F6 (byte) DIV -- AX / r8 -> AL quotient, AH remainder: */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0xF6, 0xF3 };        /* DIV BL */
      cpu.Registers[0] = 100;
      cpu.Registers[3] = 0x07;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      InterpTestStepOnce(&cpu);
      CHECK((cpu.Registers[0] & 0xFF) == 14 && ((cpu.Registers[0] >> 8) & 0xFF) == 2, "F6 /6: DIV BL 100/7 -> AL=14 AH=2"); }

    /* T58: DIV by zero bails (return 0 -> InterpTestStepOnce no-op, IP unchanged): */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0xF7, 0xF1 };        /* DIV CX, CX=0 */
      cpu.Registers[0] = 5;
      cpu.Registers[2] = 0;
      cpu.Registers[1] = 0;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      {
          INT advanced = InterpTestStepOnce(&cpu);
          CHECK(advanced == 0 && cpu.Ip == 0, "div by zero: interp bails (no UB), IP unchanged");
      }
      }

    /* T59: PUSHA (60) -- push AX,CX,DX,BX,SP,BP,SI,DI; saved SP = pre-push: */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x60 };              /* PUSHA */
      cpu.Segments[2] = 0x8000;
      cpu.Registers[4] = 0x0100;
      cpu.Registers[0]=0x1111;
      cpu.Registers[1]=0x2222;
      cpu.Registers[2]=0x3333;
      cpu.Registers[3]=0x4444;
      cpu.Registers[5]=0x6666;
      cpu.Registers[6]=0x7777;
      cpu.Registers[7]=0x8888;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      InterpTestStepOnce(&cpu);
      CHECK((cpu.Registers[4] & 0xFFFF) == 0x00F0, "60: PUSHA, SP -= 16");
      { UINT32 base = (UINT32)0x8000 << 4;
        CHECK((g_Memory[base+0xFE] | (g_Memory[base+0xFF]<<8)) == 0x1111, "pusha: AX at top slot [SP+14]");
        CHECK((g_Memory[base+0xF6] | (g_Memory[base+0xF7]<<8)) == 0x0100, "pusha: saved-SP slot = pre-push SP");
        CHECK((g_Memory[base+0xF0] | (g_Memory[base+0xF1]<<8)) == 0x8888, "pusha: DI at bottom slot [SP]"); } }

    /* T60: PUSHA/POPA round-trip -- POPA restores AX..DI, discards saved SP */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x60, 0x61 };        /* PUSHA; POPA */
      cpu.Segments[2] = 0x8000;
      cpu.Registers[4] = 0x0100;
      cpu.Registers[0]=0x1111;
      cpu.Registers[1]=0x2222;
      cpu.Registers[2]=0x3333;
      cpu.Registers[3]=0x4444;
      cpu.Registers[5]=0x6666;
      cpu.Registers[6]=0x7777;
      cpu.Registers[7]=0x8888;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      InterpTestStepOnce(&cpu);        /* PUSHA */
      cpu.Registers[0]=cpu.Registers[1]=cpu.Registers[2]=cpu.Registers[3]=cpu.Registers[5]=cpu.Registers[6]=cpu.Registers[7]=0; /* clobber all GP */
      InterpTestStepOnce(&cpu);                                          /* POPA */
      CHECK(cpu.Registers[0]==0x1111 && cpu.Registers[3]==0x4444 && cpu.Registers[7]==0x8888 && cpu.Registers[6]==0x7777,
            "61: POPA restores AX/BX/DI/SI from stack");
      CHECK((cpu.Registers[4] & 0xFFFF) == 0x0100, "popa: SP back to start (saved-SP slot discarded)"); }

    /* T61: PUSHAD/POPAD (66 60 / 66 61) -- full 32-bit register file: */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x66, 0x60, 0x66, 0x61 };  /* PUSHAD; POPAD */
      cpu.Segments[2] = 0x8000;
      cpu.Registers[4] = 0x0200;
      cpu.Registers[0]=0xAAAA1111;
      cpu.Registers[3]=0xBBBB4444;
      cpu.Registers[7]=0xCCCC8888;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      InterpTestStepOnce(&cpu);        /* PUSHAD */
      CHECK((cpu.Registers[4] & 0xFFFF) == 0x01E0, "66 60: PUSHAD, SP -= 32");
      cpu.Registers[0]=cpu.Registers[3]=cpu.Registers[7]=0;
      InterpTestStepOnce(&cpu);                                          /* POPAD */
      CHECK(cpu.Registers[0]==0xAAAA1111 && cpu.Registers[3]==0xBBBB4444 && cpu.Registers[7]==0xCCCC8888,
            "66 61: POPAD restores full 32-bit EAX/EBX/EDI");
      CHECK((cpu.Registers[4] & 0xFFFF) == 0x0200, "popad: SP back to 0x0200"); }

    /* T62: LEMMINGS' DIRTY-MAP SCAN -- `repne scasb` (F2 AE) MUST BE MODELLED.
     * (s68) The erase engine is: mov al,1 / mov cx,0x28 / repne scasb / jz found.
     * Unmodelled, the scasb bailed to V86 and the latch copies that followed it
     * landed in the unprotected A0000 window, so no sprite was ever erased.
     * Pinned as the guest's own idiom: ES:DI over a 40-byte row with a 1 at [5].
     */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0xF2, 0xAE, 0x74, 0x02, 0xB0, 0x55, 0xB0, 0xAA };
      /* repne scasb; jz +2; mov al,55h; mov al,AAh */
      UINT32 base = (UINT32)0x2000 << 4;
      memset(g_Memory + base, 0, 0x28);
      g_Memory[base + 5] = 1;
      cpu.Segments[0] = 0x2000;
      cpu.Registers[7] = 0;
      cpu.Registers[0] = 0x01;
      cpu.Registers[1] = 0x28;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      CHECK(InterpTestStepOnce(&cpu) == 1, "F2 AE: repne scasb is modelled (no bail)");
      CHECK((cpu.Flags & EFLAGS_ZF_U) && (cpu.Registers[7] & 0xFFFF) == 6 && (cpu.Registers[1] & 0xFFFF) == 0x28 - 6,
            "repne scasb: stops ONE PAST the match with ZF=1, CX=0x22");
      InterpTestStepOnce(&cpu);
      InterpTestStepOnce(&cpu);
      CHECK((cpu.Registers[0] & 0xFF) == 0xAA, "repne scasb + jz: the `found` branch is taken"); }

    /* T63: repne scasb with NO match runs CX to 0 and leaves ZF=0: */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0xF2, 0xAE };
      UINT32 base = (UINT32)0x2000 << 4;
      memset(g_Memory + base, 0, 0x28);
      cpu.Segments[0] = 0x2000;
      cpu.Registers[7] = 0;
      cpu.Registers[0] = 0x01;
      cpu.Registers[1] = 0x28;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      InterpTestStepOnce(&cpu);
      CHECK(!(cpu.Flags & EFLAGS_ZF_U) && (cpu.Registers[7] & 0xFFFF) == 0x28 && (cpu.Registers[1] & 0xFFFF) == 0,
            "repne scasb: no match -> DI+=CX, CX=0, ZF=0"); }

    /* T64: repe scasb (F3 AE) finds the END of a run of matches: */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0xF3, 0xAE };
      UINT32 base = (UINT32)0x2000 << 4;
      memset(g_Memory + base, 1, 0x28);
      g_Memory[base + 3] = 0;
      cpu.Segments[0] = 0x2000;
      cpu.Registers[7] = 0;
      cpu.Registers[0] = 0x01;
      cpu.Registers[1] = 0x28;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      InterpTestStepOnce(&cpu);
      CHECK(!(cpu.Flags & EFLAGS_ZF_U) && (cpu.Registers[7] & 0xFFFF) == 4 && (cpu.Registers[1] & 0xFFFF) == 0x28 - 4,
            "repe scasb: stops one past the first mismatch, ZF=0"); }

    /* T65: rep with CX=0 is a no-op that leaves the flags alone: */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0xF2, 0xAE };
      cpu.Segments[0] = 0x2000;
      cpu.Registers[7] = 7;
      cpu.Registers[0] = 0x01;
      cpu.Registers[1] = 0;
      cpu.Flags |= EFLAGS_ZF_U | EFLAGS_CF_U;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      InterpTestStepOnce(&cpu);
      CHECK((cpu.Flags & (EFLAGS_ZF_U | EFLAGS_CF_U)) == (EFLAGS_ZF_U | EFLAGS_CF_U) && (cpu.Registers[7] & 0xFFFF) == 7,
            "repne scasb CX=0: nothing happens, flags untouched"); }

    /* T66: cmpsb (A6) compares DS:SI with ES:DI, CMP flags, both advance: */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0xA6 };
      UINT32 sourceBase = (UINT32)0x3000 << 4;
      UINT32 destinationBase = (UINT32)0x4000 << 4;
      g_Memory[sourceBase] = 0x10;
      g_Memory[destinationBase] = 0x20;
      cpu.Segments[3] = 0x3000;
      cpu.Segments[0] = 0x4000;
      cpu.Registers[6] = 0;
      cpu.Registers[7] = 0;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      InterpTestStepOnce(&cpu);
      CHECK((cpu.Flags & EFLAGS_CF_U) && !(cpu.Flags & EFLAGS_ZF_U) && (cpu.Registers[6] & 0xFFFF) == 1 && (cpu.Registers[7] & 0xFFFF) == 1,
            "cmpsb: 10h-20h -> CF=1 ZF=0, SI and DI advance"); }

    /* T67: repe cmpsw (F3 A7) with DF=1 walks DOWN by words: */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0xF3, 0xA7 };
      UINT32 sourceBase = (UINT32)0x3000 << 4;
      UINT32 destinationBase = (UINT32)0x4000 << 4;
      g_Memory[sourceBase+8]=0x11;
      g_Memory[sourceBase+9]=0x22;
      g_Memory[destinationBase+8]=0x11;
      g_Memory[destinationBase+9]=0x22;   /* equal */
      g_Memory[sourceBase+6]=0x33;
      g_Memory[sourceBase+7]=0x44;
      g_Memory[destinationBase+6]=0x33;
      g_Memory[destinationBase+7]=0x45;   /* differ */
      cpu.Segments[3] = 0x3000;
      cpu.Segments[0] = 0x4000;
      cpu.Registers[6] = 8;
      cpu.Registers[7] = 8;
      cpu.Registers[1] = 4;
      cpu.Flags |= EFLAGS_DF_U;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      InterpTestStepOnce(&cpu);
      CHECK(!(cpu.Flags & EFLAGS_ZF_U) && (cpu.Registers[6] & 0xFFFF) == 4 && (cpu.Registers[7] & 0xFFFF) == 4 && (cpu.Registers[1] & 0xFFFF) == 2,
            "repe cmpsw DF=1: two words compared, stops after the mismatch, SI/DI -= 4"); }

    /* T68-T71: the rest of Lemmings' bail table -- CBW, CWD, XLAT, LES mem: */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x98, 0x99 };        /* CBW; CWD */
      cpu.Registers[0] = 0x1280;
      cpu.Registers[2] = 0x1234;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      InterpTestStepOnce(&cpu);
      CHECK((cpu.Registers[0] & 0xFFFF) == 0xFF80, "98: CBW sign-extends AL=80h to AX=FF80h");
      InterpTestStepOnce(&cpu);
      CHECK((cpu.Registers[2] & 0xFFFF) == 0xFFFF, "99: CWD sign-extends AX into DX"); }
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0xD7 };              /* XLAT */
      UINT32 base = (UINT32)0x5000 << 4;
      g_Memory[base + 0x100 + 7] = 0x5A;
      cpu.Segments[3] = 0x5000;
      cpu.Registers[3] = 0x100;
      cpu.Registers[0] = 0x1107;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      InterpTestStepOnce(&cpu);
      CHECK((cpu.Registers[0] & 0xFFFF) == 0x115A, "D7: XLAT AL <- [DS:BX+AL], AH kept"); }
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0xC4, 0x1E, 0xBE, 0x1F };   /* LES BX,[1FBEh] */
      UINT32 base = (UINT32)0x5000 << 4;
      g_Memory[base+0x1FBE]=0x34;
      g_Memory[base+0x1FBF]=0x12;
      g_Memory[base+0x1FC0]=0x00;
      g_Memory[base+0x1FC1]=0xA0;
      cpu.Segments[3] = 0x5000;
      cpu.Segments[0] = 0;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      CHECK(InterpTestStepOnce(&cpu) == 1, "C4 1E: LES (memory form) is modelled");
      CHECK((cpu.Registers[3] & 0xFFFF) == 0x1234 && cpu.Segments[0] == 0xA000, "LES BX,[m]: BX=off, ES=seg"); }
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0xC4, 0xC4, 0x21 };  /* the VDM BOP: must STILL bail */
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      CHECK(InterpTestStepOnce(&cpu) == 0 && cpu.Ip == 0, "C4 C4 nn: the BOP still bails, IP untouched"); }

    /* T72: POP r/m16 (8F /0) -- Bubbles' `pop [bx+7]`, 1.16M bails a run: */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x8F, 0x47, 0x07, 0x9B };   /* POP [BX+7]; WAIT */
      UINT32 stackBase = (UINT32)0x8000 << 4;
      UINT32 dataBase = (UINT32)0x5000 << 4;
      cpu.Segments[2] = 0x8000;
      cpu.Registers[4] = 0x0100;
      g_Memory[stackBase+0x100] = 0xCD;
      g_Memory[stackBase+0x101] = 0xAB;
      cpu.Segments[3] = 0x5000;
      cpu.Registers[3] = 0x20;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      InterpTestStepOnce(&cpu);
      CHECK((g_Memory[dataBase+0x27] | (g_Memory[dataBase+0x28] << 8)) == 0xABCD && (cpu.Registers[4] & 0xFFFF) == 0x0102,
            "8F /0: POP [BX+7] stores the word and SP += 2");
      CHECK(InterpTestStepOnce(&cpu) == 1 && cpu.Ip == 4, "9B: WAIT is a no-op"); }

    /* T73: LAHF/SAHF round-trip (9F/9E): */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x9F, 0x9E };
      cpu.Flags = 0x0002 | EFLAGS_CF_U | EFLAGS_ZF_U | EFLAGS_SF_U;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      InterpTestStepOnce(&cpu);
      CHECK(((cpu.Registers[0] >> 8) & 0xFF) == (0x02 | EFLAGS_CF_U | EFLAGS_ZF_U | EFLAGS_SF_U), "9F: LAHF copies SF/ZF/CF + bit1 into AH");
      cpu.Flags = 0x0002;
      cpu.Registers[0] = (cpu.Registers[0] & 0xFF) | ((EFLAGS_PF_U | EFLAGS_AF_U) << 8);
      InterpTestStepOnce(&cpu);
      CHECK((cpu.Flags & 0xD5) == (EFLAGS_PF_U | EFLAGS_AF_U), "9E: SAHF loads PF/AF from AH, clears the rest"); }

    /* s80, north star 1: what Wolf3D and Mario declined under a multi-plane mask.
     * A declined instruction runs natively UNTIL THE NEXT TRAP, and any A0000 store in
     * that stretch reaches one plane only -- so these are correctness, not speed. ----
     */
    /* CDQ (66 99): Wolf3D's FixedByFrac `cdq / idiv dword [bp+0Ah]`, 1,074 declines. */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x66, 0x99 };
      cpu.Registers[0] = 0x80000000u;
      cpu.Registers[2] = 0x12345678u;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      CHECK(InterpTestStepOnce(&cpu) && cpu.Registers[2] == 0xFFFFFFFFu && cpu.Registers[0] == 0x80000000u, "cdq: EAX<0 -> EDX=FFFFFFFF, EAX kept");
      CHECK(cpu.Ip == 2, "cdq: IP advances past the prefix and opcode"); }
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x66, 0x99 };
      cpu.Registers[0] = 0x7FFFFFFFu;
      cpu.Registers[2] = 0xFFFFFFFFu;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      InterpTestStepOnce(&cpu);
      CHECK(cpu.Registers[2] == 0, "cdq: EAX>=0 -> EDX=0"); }
    /* CWDE (66 98): EAX <- sign-extend AX, whatever the high half held. */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x66, 0x98 };
      cpu.Registers[0] = 0x12348001u;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      CHECK(InterpTestStepOnce(&cpu) && cpu.Registers[0] == 0xFFFF8001u, "cwde: AX=8001 -> EAX=FFFF8001"); }
    /* IMUL r16, r/m16, imm8 (6B): Mario's `6b f8 0a` = imul di,ax,10 (7,978 declines). */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x6B, 0xF8, 0x0A };
      cpu.Registers[0] = 0x0007;
      cpu.Registers[7] = 0xABCD0000u;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      CHECK(InterpTestStepOnce(&cpu) && cpu.Registers[7] == 0xABCD0046u, "imul 6B: di = ax*10 = 70, high half of EDI kept");
      CHECK(!(cpu.Flags & (EFLAGS_CF_U | EFLAGS_OF_U)) && cpu.Ip == 3, "imul 6B: no overflow -> CF=OF=0, IP+3"); }
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x6B, 0xC3, 0xFE };          /* imul ax,bx,-2 */
      cpu.Registers[3] = 0x0003;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      InterpTestStepOnce(&cpu);
      CHECK((cpu.Registers[0] & 0xFFFF) == 0xFFFA, "imul 6B: imm8 is SIGN-extended (3 * -2 = -6)"); }
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x6B, 0xC3, 0x04 };          /* imul ax,bx,4 */
      cpu.Registers[3] = 0x4000;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      InterpTestStepOnce(&cpu);
      CHECK((cpu.Registers[0] & 0xFFFF) == 0 && (cpu.Flags & EFLAGS_CF_U) && (cpu.Flags & EFLAGS_OF_U),
            "imul 6B: 4000h*4 overflows 16 bits -> AX=0, CF=OF=1"); }
    /* Mario's memory form: `6b 06 cc 00 5a` = imul ax, [00CCh], 5Ah (DS-relative). */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x6B, 0x06, 0xCC, 0x00, 0x5A };
      cpu.Segments[3] = 0x2000;
      g_Memory[0x200CC] = 0x03;
      g_Memory[0x200CD] = 0x00;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      CHECK(InterpTestStepOnce(&cpu) && (cpu.Registers[0] & 0xFFFF) == 0x010E && cpu.Ip == 5, "imul 6B: [disp16] * 5Ah = 3*90 = 10Eh, IP+5"); }
    /* IMUL r16, r/m16, imm16 (69): imul bx,cx,1234h. */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x69, 0xD9, 0x34, 0x12 };
      cpu.Registers[1] = 0x0002;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      CHECK(InterpTestStepOnce(&cpu) && (cpu.Registers[3] & 0xFFFF) == 0x2468 && cpu.Ip == 4, "imul 69: bx = cx*1234h = 2468h, IP+4"); }
    /* 66 6B: imul eax,eax,10 -- 32-bit, signed overflow. */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0x66, 0x6B, 0xC0, 0x0A };
      cpu.Registers[0] = 0x10000000u;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      CHECK(InterpTestStepOnce(&cpu) && cpu.Registers[0] == 0xA0000000u && (cpu.Flags & EFLAGS_CF_U) && (cpu.Flags & EFLAGS_OF_U),
            "imul 66 6B: eax*10 wraps to A0000000h, CF=OF=1"); }

    /* #183: 16-bit address size counts in CX and walks SI/DI, and the HIGH halves
     * of ECX/ESI/EDI are the guest's (s80 carries all 32 bits). The interpreter took
     * the REP count from ECX -- up to 4G stores -- and zeroed the high halves after.
     */
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0xF3, 0xAA };     /* REP STOSB */
      UINT32 index;

      for (index = 0; index < 16; ++index)
          g_Memory[0x20000 + index] = 0;

      cpu.Segments[0] = 0x2000;
      cpu.Registers[7] = 0xABCD0000u;
      cpu.Registers[1] = 0x00010003u;
      cpu.Registers[0] = 0x5A;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      InterpTestStepOnce(&cpu);
      CHECK(g_Memory[0x20000] == 0x5A && g_Memory[0x20002] == 0x5A && g_Memory[0x20003] == 0,
            "rep stosb: CX=3 stores exactly 3 bytes although ECX=0x00010003");
      CHECK(cpu.Registers[1] == 0x00010000u, "rep stosb: ECX high half kept, CX counted to 0");
      CHECK(cpu.Registers[7] == 0xABCD0003u, "rep stosb: EDI high half kept, DI advanced by 3"); }
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0xE2, 0xFE };     /* LOOP $ */
      cpu.Registers[1] = 0x00050002u;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      InterpTestStepOnce(&cpu);
      CHECK(cpu.Ip == 0 && cpu.Registers[1] == 0x00050001u, "loop: CX 2->1, jumps, ECX high kept");
      InterpTestStepOnce(&cpu);
      CHECK(cpu.Ip == 2 && cpu.Registers[1] == 0x00050000u, "loop: CX 1->0 falls through (ECX high ignored)"); }
    { V86_CPU cpu = InterpTestMakeCpu();
    BYTE bytes[] = { 0xE3, 0x10 };     /* JCXZ +16 */
      cpu.Registers[1] = 0x00070000u;
      InterpTestLoad(&cpu, 0x1000, 0, bytes, sizeof bytes);
      InterpTestStepOnce(&cpu);
      CHECK(cpu.Ip == 0x12, "jcxz: taken when CX=0 although ECX!=0"); }

    InterpTestO32Battery();
    InterpTestO32Replay();

    printf("\n%d checks, %d failed\n", g_Total, g_Failures);
    return g_Failures ? 1 : 0;
}
