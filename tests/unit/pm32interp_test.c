/* pm32interp_test.c -- off-VM battery for the flat 32-bit interpreter (src/host/pm32interp.h).
 *
 * s80, north star 1: Doom's low-detail drawers run here while a multi-plane map mask is
 * live. What matters: exact results, exact flags (the drawers branch on them), exact
 * EIP, and an EXACT DECLINE -- an unmodelled instruction must change nothing, because
 * the real CPU re-executes it. Flat memory, every segment base 0.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "ntvdmex_types.h"   /* the host hooks below are written before pm32interp.h brings it */

#define MEMSZ 0x200000u
static BYTE g_Memory[MEMSZ];
static UINT32 g_OutPort, g_OutValue, g_OutCount;
static BYTE  Pm32HostRead8(UINT32 linear) { return linear < MEMSZ ? g_Memory[linear] : 0xFF; }
static VOID     Pm32HostWrite8(UINT32 linear, BYTE value) { if (linear < MEMSZ) g_Memory[linear] = value; }
static INT      Pm32HostCanAccess(UINT32 linear, INT width, INT isWrite) { (VOID)isWrite; return linear < MEMSZ && linear + (UINT32)width <= MEMSZ; }
static UINT32 Pm32HostIn(WORD port, INT width) { (VOID)width; return port == 0x3DA ? 0x08 : 0; }
static VOID     Pm32HostOut(WORD port, INT width, UINT32 value) { (VOID)width; g_OutPort = port; g_OutValue = value; g_OutCount++; }

#include "../../src/host/pm32interp.h"

static INT g_Total = 0, g_Failures = 0;
#define CHECK(condition,message) do{ g_Total++; if(condition){printf("  PASS  %s\n",(message));} \
    else{printf("  FAIL  %s\n",(message)); g_Failures++;} }while(0)

#define CODE 0x1000u
static PM32_CPU Pm32InterpTestMakeCpu(PCBYTE code, INT length)
{
    PM32_CPU cpu; memset(&cpu, 0, sizeof cpu);
    memcpy(g_Memory + CODE, code, (size_t)length);
    cpu.Eip = CODE; cpu.Flags = 0x202; cpu.Registers[4] = 0x100000;       /* ESP */
    return cpu;
}

INT main(VOID)
{
    printf("== flat 32-bit interpreter battery ==\n");

    /* ---- arithmetic and flags ---- */
    { BYTE code[] = { 0x01, 0xD8 };                         /* add eax,ebx */
      PM32_CPU cpu = Pm32InterpTestMakeCpu(code, 2); cpu.Registers[0] = 0x7FFFFFFF; cpu.Registers[3] = 1;
      CHECK(Pm32Step(&cpu) && cpu.Registers[0] == 0x80000000u && cpu.Eip == CODE + 2, "add eax,ebx: 7FFFFFFF+1");
      CHECK((cpu.Flags & EFLAGS_OF_U) && (cpu.Flags & EFLAGS_SF_U) && !(cpu.Flags & EFLAGS_CF_U) && !(cpu.Flags & EFLAGS_ZF_U),
            "add: signed overflow sets OF+SF, not CF/ZF"); }
    { BYTE code[] = { 0x2B, 0xC5 };                         /* sub eax,ebp */
      PM32_CPU cpu = Pm32InterpTestMakeCpu(code, 2); cpu.Registers[0] = 1; cpu.Registers[5] = 2; Pm32Step(&cpu);
      CHECK(cpu.Registers[0] == 0xFFFFFFFFu && (cpu.Flags & EFLAGS_CF_U) && (cpu.Flags & EFLAGS_SF_U), "sub eax,ebp: 1-2 borrows"); }
    { BYTE code[] = { 0x3B, 0x05, 0x10, 0x20, 0x00, 0x00 }; /* cmp eax,[2010h] */
      PM32_CPU cpu = Pm32InterpTestMakeCpu(code, 6); cpu.Registers[0] = 5; g_Memory[0x2010] = 5; Pm32Step(&cpu);
      CHECK((cpu.Flags & EFLAGS_ZF_U) && cpu.Registers[0] == 5 && cpu.Eip == CODE + 6, "cmp eax,[disp32]: equal -> ZF, no store"); }
    { BYTE code[] = { 0x83, 0xE1, 0x01 };                   /* and ecx,1 */
      PM32_CPU cpu = Pm32InterpTestMakeCpu(code, 3); cpu.Registers[1] = 0x12345677; cpu.Flags |= EFLAGS_CF_U | EFLAGS_OF_U; Pm32Step(&cpu);
      CHECK(cpu.Registers[1] == 1 && !(cpu.Flags & (EFLAGS_CF_U | EFLAGS_OF_U | EFLAGS_ZF_U)), "and ecx,1: CF=OF=0"); }
    { BYTE code[] = { 0x81, 0xE3, 0x00, 0x00, 0xFF, 0xFF }; /* and ebx,FFFF0000h */
      PM32_CPU cpu = Pm32InterpTestMakeCpu(code, 6); cpu.Registers[3] = 0x12345678; Pm32Step(&cpu);
      CHECK(cpu.Registers[3] == 0x12340000u && cpu.Eip == CODE + 6, "and ebx,imm32"); }
    { BYTE code[] = { 0x48 };                               /* dec eax: CF preserved */
      PM32_CPU cpu = Pm32InterpTestMakeCpu(code, 1); cpu.Registers[0] = 1; cpu.Flags |= EFLAGS_CF_U; Pm32Step(&cpu);
      CHECK(cpu.Registers[0] == 0 && (cpu.Flags & EFLAGS_ZF_U) && (cpu.Flags & EFLAGS_CF_U), "dec eax: 1->0 sets ZF, keeps CF"); }
    { BYTE code[] = { 0xFF, 0x0D, 0x58, 0x1A, 0x00, 0x00 }; /* dec dword [1A58h] */
      PM32_CPU cpu = Pm32InterpTestMakeCpu(code, 6); g_Memory[0x1A58] = 1; g_Memory[0x1A59] = 0; g_Memory[0x1A5A] = 0; g_Memory[0x1A5B] = 0; Pm32Step(&cpu);
      CHECK(g_Memory[0x1A58] == 0 && (cpu.Flags & EFLAGS_ZF_U) && cpu.Eip == CODE + 6, "dec dword [disp32]: -> 0, ZF"); }
    { BYTE code[] = { 0xF7, 0x05, 0x5C, 0x1A, 0x00, 0x00, 0xFE, 0xFF, 0xFF, 0xFF };  /* test dword [1A5Ch],FFFFFFFEh */
      PM32_CPU cpu = Pm32InterpTestMakeCpu(code, 10); g_Memory[0x1A5C] = 1; g_Memory[0x1A5D] = g_Memory[0x1A5E] = g_Memory[0x1A5F] = 0; Pm32Step(&cpu);
      CHECK((cpu.Flags & EFLAGS_ZF_U) && cpu.Eip == CODE + 10, "test dword [disp32],imm32: 1 & FFFFFFFE = 0 -> ZF"); }
    { BYTE code[] = { 0xF7, 0xE9 };                         /* imul ecx */
      PM32_CPU cpu = Pm32InterpTestMakeCpu(code, 2); cpu.Registers[0] = 0xFFFFFFFE; cpu.Registers[1] = 3; Pm32Step(&cpu);
      CHECK(cpu.Registers[0] == 0xFFFFFFFAu && cpu.Registers[2] == 0xFFFFFFFFu && !(cpu.Flags & EFLAGS_CF_U), "imul ecx: -2*3 = -6 in EDX:EAX"); }
    { BYTE code[] = { 0x0F, 0xAF, 0xC3 };                   /* imul eax,ebx */
      PM32_CPU cpu = Pm32InterpTestMakeCpu(code, 3); cpu.Registers[0] = 0x10000; cpu.Registers[3] = 0x10000; Pm32Step(&cpu);
      CHECK(cpu.Registers[0] == 0 && (cpu.Flags & EFLAGS_CF_U) && (cpu.Flags & EFLAGS_OF_U), "imul eax,ebx: overflow -> CF=OF=1"); }
    { BYTE code[] = { 0x99, 0xF7, 0x7E, 0x0A };             /* cdq ; idiv dword [esi+0Ah] */
      PM32_CPU cpu = Pm32InterpTestMakeCpu(code, 4); cpu.Registers[0] = (UINT32)-100; cpu.Registers[6] = 0x3000;
      g_Memory[0x300A] = 7; g_Memory[0x300B] = g_Memory[0x300C] = g_Memory[0x300D] = 0;
      Pm32Step(&cpu); Pm32Step(&cpu);
      CHECK(cpu.Registers[0] == (UINT32)-14 && cpu.Registers[2] == (UINT32)-2, "cdq; idiv: -100/7 = -14 r -2 (truncation)"); }
    { BYTE code[] = { 0xF7, 0xF9 };                         /* idiv ecx, ecx=0 -> #DE */
      PM32_CPU cpu = Pm32InterpTestMakeCpu(code, 2); PM32_CPU before; cpu.Registers[1] = 0; cpu.Registers[0] = 5; before = cpu;
      CHECK(!Pm32Step(&cpu) && !memcmp(&cpu, &before, sizeof cpu), "idiv by 0: DECLINES with nothing changed"); }

    /* ---- shifts ---- */
    { BYTE code[] = { 0xC1, 0xEB, 0x02 };                   /* shr ebx,2 */
      PM32_CPU cpu = Pm32InterpTestMakeCpu(code, 3); cpu.Registers[3] = 0x0B; Pm32Step(&cpu);
      CHECK(cpu.Registers[3] == 2 && (cpu.Flags & EFLAGS_CF_U), "shr ebx,2: 0Bh -> 2, CF = last bit out (1)"); }
    { BYTE code[] = { 0xD3, 0xE0 };                         /* shl eax,cl */
      PM32_CPU cpu = Pm32InterpTestMakeCpu(code, 2); cpu.Registers[0] = 3; cpu.Registers[1] = 2; Pm32Step(&cpu);
      CHECK(cpu.Registers[0] == 0x0C, "shl eax,cl: 3<<2 = 0Ch (Doom's two-plane mask)"); }
    { BYTE code[] = { 0xD1, 0xE8 };                         /* shr eax,1 */
      PM32_CPU cpu = Pm32InterpTestMakeCpu(code, 2); cpu.Registers[0] = 1; Pm32Step(&cpu);
      CHECK(cpu.Registers[0] == 0 && (cpu.Flags & EFLAGS_ZF_U) && (cpu.Flags & EFLAGS_CF_U), "shr eax,1: 1 -> 0, ZF+CF"); }
    { BYTE code[] = { 0x0F, 0xA4, 0xE9, 0x16 };             /* shld ecx,ebp,16h */
      PM32_CPU cpu = Pm32InterpTestMakeCpu(code, 4); cpu.Registers[1] = 0; cpu.Registers[5] = 0xABCD1234u; Pm32Step(&cpu);
      CHECK(cpu.Registers[1] == (0xABCD1234u >> 10) && cpu.Eip == CODE + 4, "shld ecx,ebp,22: brings in ebp's top 22 bits"); }
    { BYTE code[] = { 0xD1, 0xD0 };                         /* rcl eax,1 -> decline */
      PM32_CPU cpu = Pm32InterpTestMakeCpu(code, 2); PM32_CPU before = cpu;
      CHECK(!Pm32Step(&cpu) && !memcmp(&cpu, &before, sizeof cpu), "rcl: declines, state untouched"); }

    /* ---- addressing ---- */
    { BYTE code[] = { 0x8D, 0x7C, 0xED, 0x00 };             /* lea edi,[ebp+ebp*8+0] */
      PM32_CPU cpu = Pm32InterpTestMakeCpu(code, 4); cpu.Registers[5] = 10; Pm32Step(&cpu);
      CHECK(cpu.Registers[7] == 90 && cpu.Eip == CODE + 4, "lea edi,[ebp+ebp*8]: 10*9 = 90"); }
    { BYTE code[] = { 0x8A, 0x04, 0x0E };                   /* mov al,[esi+ecx] */
      PM32_CPU cpu = Pm32InterpTestMakeCpu(code, 3); cpu.Registers[0] = 0x11223344; cpu.Registers[6] = 0x4000; cpu.Registers[1] = 5; g_Memory[0x4005] = 0x99; Pm32Step(&cpu);
      CHECK(cpu.Registers[0] == 0x11223399u, "mov al,[esi+ecx]: SIB, high bytes of EAX kept"); }
    { BYTE code[] = { 0x88, 0x5F, 0x50 };                   /* mov [edi+50h],bl */
      PM32_CPU cpu = Pm32InterpTestMakeCpu(code, 3); cpu.Registers[7] = 0x5000; cpu.Registers[3] = 0x77; Pm32Step(&cpu);
      CHECK(g_Memory[0x5050] == 0x77, "mov [edi+disp8],bl"); }
    { BYTE code[] = { 0x66, 0x89, 0x17 };                   /* mov word [edi],dx */
      PM32_CPU cpu = Pm32InterpTestMakeCpu(code, 3); cpu.Registers[7] = 0x5100; cpu.Registers[2] = 0xBEEF1234u; g_Memory[0x5102] = 0x5A; Pm32Step(&cpu);
      CHECK(g_Memory[0x5100] == 0x34 && g_Memory[0x5101] == 0x12 && g_Memory[0x5102] == 0x5A && cpu.Eip == CODE + 3,
            "66 mov [edi],dx: two bytes, no more"); }
    { BYTE code[] = { 0x0F, 0xB6, 0x06, 0x0F, 0xBE, 0x06 };  /* movzx eax,byte[esi] ; movsx eax,byte[esi] */
      PM32_CPU cpu = Pm32InterpTestMakeCpu(code, 6); cpu.Registers[6] = 0x5200; g_Memory[0x5200] = 0x80; Pm32Step(&cpu);
      CHECK(cpu.Registers[0] == 0x80, "movzx eax,byte [esi]");
      Pm32Step(&cpu);
      CHECK(cpu.Registers[0] == 0xFFFFFF80u, "movsx eax,byte [esi]"); }
    { BYTE code[] = { 0xA1, 0x00, 0x60, 0x00, 0x00, 0xA3, 0x04, 0x60, 0x00, 0x00 };  /* mov eax,[6000h]; mov [6004h],eax */
      PM32_CPU cpu = Pm32InterpTestMakeCpu(code, 10); g_Memory[0x6000] = 0x78; g_Memory[0x6001] = 0x56; g_Memory[0x6002] = 0x34; g_Memory[0x6003] = 0x12;
      Pm32Step(&cpu); Pm32Step(&cpu);
      CHECK(cpu.Registers[0] == 0x12345678u && g_Memory[0x6004] == 0x78 && g_Memory[0x6007] == 0x12, "mov eax,moffs / mov moffs,eax"); }

    /* ---- control flow ---- */
    { BYTE code[] = { 0x75, 0x10 };                         /* jne +10h, ZF=0 -> taken */
      PM32_CPU cpu = Pm32InterpTestMakeCpu(code, 2); Pm32Step(&cpu);
      CHECK(cpu.Eip == CODE + 0x12, "jne rel8 taken"); }
    { BYTE code[] = { 0x0F, 0x8F, 0x00, 0x01, 0x00, 0x00 };  /* jg rel32, ZF=0 SF=OF -> taken */
      PM32_CPU cpu = Pm32InterpTestMakeCpu(code, 6); Pm32Step(&cpu);
      CHECK(cpu.Eip == CODE + 6 + 0x100, "jg rel32 taken"); }
    { BYTE code[] = { 0x0F, 0x88, 0x00, 0x01, 0x00, 0x00 };  /* js rel32, SF=0 -> not taken */
      PM32_CPU cpu = Pm32InterpTestMakeCpu(code, 6); Pm32Step(&cpu);
      CHECK(cpu.Eip == CODE + 6, "js rel32 not taken"); }
    { BYTE code[] = { 0xE8, 0x05, 0x00, 0x00, 0x00, 0x90, 0x90, 0x90, 0x90, 0x90, 0xC3 };  /* call +5 ; ... ; ret */
      PM32_CPU cpu = Pm32InterpTestMakeCpu(code, 11); UINT32 stackPointer = cpu.Registers[4];
      Pm32Step(&cpu);
      CHECK(cpu.Eip == CODE + 10 && cpu.Registers[4] == stackPointer - 4, "call rel32: pushes return, jumps");
      Pm32Step(&cpu);
      CHECK(cpu.Eip == CODE + 5 && cpu.Registers[4] == stackPointer, "ret: back to the instruction after the call"); }

    /* ---- stack ---- */
    { BYTE code[] = { 0x60, 0x61 };                         /* pushad ; popad */
      PM32_CPU cpu = Pm32InterpTestMakeCpu(code, 2); INT index; UINT32 stackPointer = cpu.Registers[4];
      for (index = 0; index < 8; ++index) if (index != 4) cpu.Registers[index] = 0x1000 + (UINT32)index;
      Pm32Step(&cpu);
      CHECK(cpu.Registers[4] == stackPointer - 32, "pushad: 32 bytes");
      for (index = 0; index < 8; ++index) if (index != 4) cpu.Registers[index] = 0;
      Pm32Step(&cpu);
      CHECK(cpu.Registers[4] == stackPointer && cpu.Registers[0] == 0x1000 && cpu.Registers[7] == 0x1007 && cpu.Registers[5] == 0x1005, "popad: restores all, ESP back"); }

    /* ---- port I/O ---- */
    { BYTE code[] = { 0xEE };                               /* out dx,al */
      PM32_CPU cpu = Pm32InterpTestMakeCpu(code, 1); cpu.Registers[2] = 0x3C5; cpu.Registers[0] = 0x0C; g_OutCount = 0; Pm32Step(&cpu);
      CHECK(g_OutCount == 1 && g_OutPort == 0x3C5 && g_OutValue == 0x0C, "out dx,al: map mask 0Ch reaches the port"); }

    /* ---- exact decline ---- */
    { BYTE code[] = { 0xD9, 0xE8 };                         /* fld1: FPU */
      PM32_CPU cpu = Pm32InterpTestMakeCpu(code, 2); PM32_CPU before = cpu;
      CHECK(!Pm32Step(&cpu) && !memcmp(&cpu, &before, sizeof cpu), "FPU: declines, nothing changed"); }
    { BYTE code[] = { 0xCD, 0x31 };                         /* int 31h */
      PM32_CPU cpu = Pm32InterpTestMakeCpu(code, 2); PM32_CPU before = cpu;
      CHECK(!Pm32Step(&cpu) && !memcmp(&cpu, &before, sizeof cpu), "INT: declines"); }
    { BYTE code[] = { 0x67, 0x8B, 0x07 };                   /* 16-bit addressing */
      PM32_CPU cpu = Pm32InterpTestMakeCpu(code, 3); PM32_CPU before = cpu;
      CHECK(!Pm32Step(&cpu) && !memcmp(&cpu, &before, sizeof cpu), "0x67 prefix: declines"); }

    /* ---- a drawer, end to end: the shape of Doom's R_DrawColumnLow ------------------
       pushad; mov edx,3C5h; mov eax,3; out dx,al; mov edi,5400h; mov ecx,4;
       L: mov [edi],bl; add edi,50h; dec ecx; jne L; popad; ret                        */
    { BYTE code[] = { 0x60, 0xBA, 0xC5, 0x03, 0x00, 0x00, 0xB8, 0x03, 0x00, 0x00, 0x00, 0xEE,
                      0xBF, 0x00, 0x54, 0x00, 0x00, 0xB9, 0x04, 0x00, 0x00, 0x00,
                      0x88, 0x1F, 0x83, 0xC7, 0x50, 0x49, 0x75, 0xF8, 0x61, 0xC3 };
      PM32_CPU cpu = Pm32InterpTestMakeCpu(code, (INT)sizeof code); UINT32 stackPointer, count = 0; INT index;
      /* a return address on the stack, as if called */
      cpu.Registers[4] -= 4; g_Memory[cpu.Registers[4]] = 0x00; g_Memory[cpu.Registers[4] + 1] = 0x20; g_Memory[cpu.Registers[4] + 2] = 0; g_Memory[cpu.Registers[4] + 3] = 0;
      stackPointer = cpu.Registers[4]; cpu.Registers[3] = 0xAB; cpu.Registers[7] = 0x1234; g_OutCount = 0;
      while (cpu.Eip != 0x2000 && count < 1000 && Pm32Step(&cpu)) ++count;
      for (index = 0; index < 4; ++index) if (g_Memory[0x5400 + 0x50 * index] != 0xAB) break;
      CHECK(cpu.Eip == 0x2000 && cpu.Registers[4] == stackPointer + 4, "drawer: runs to its ret and returns to the caller");
      CHECK(index == 4, "drawer: all four column bytes stored");
      CHECK(g_OutCount == 1 && g_OutValue == 3 && cpu.Registers[7] == 0x1234, "drawer: mask OUT seen once, registers restored by popad"); }

    printf("\n%d checks, %d failed\n", g_Total, g_Failures);
    return g_Failures ? 1 : 0;
}
