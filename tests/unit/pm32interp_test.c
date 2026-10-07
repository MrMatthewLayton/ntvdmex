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

#define MEMSZ 0x200000u
static uint8_t MEM[MEMSZ];
static uint32_t g_out_port, g_out_val, g_out_n;
static uint8_t  Pm32HostRead8(uint32_t lin) { return lin < MEMSZ ? MEM[lin] : 0xFF; }
static void     Pm32HostWrite8(uint32_t lin, uint8_t v) { if (lin < MEMSZ) MEM[lin] = v; }
static int      Pm32HostCanAccess(uint32_t lin, int w, int wr) { (void)wr; return lin < MEMSZ && lin + (uint32_t)w <= MEMSZ; }
static uint32_t Pm32HostIn(uint16_t port, int w) { (void)w; return port == 0x3DA ? 0x08 : 0; }
static void     Pm32HostOut(uint16_t port, int w, uint32_t v) { (void)w; g_out_port = port; g_out_val = v; g_out_n++; }

#include "../../src/host/pm32interp.h"

static int total = 0, fails = 0;
#define CHECK(c,m) do{ total++; if(c){printf("  PASS  %s\n",(m));} \
    else{printf("  FAIL  %s\n",(m)); fails++;} }while(0)

#define CODE 0x1000u
static PM32_CPU mk(const uint8_t *b, int n)
{
    PM32_CPU c; memset(&c, 0, sizeof c);
    memcpy(MEM + CODE, b, (size_t)n);
    c.Eip = CODE; c.Flags = 0x202; c.Registers[4] = 0x100000;       /* ESP */
    return c;
}

int main(void)
{
    printf("== flat 32-bit interpreter battery ==\n");

    /* ---- arithmetic and flags ---- */
    { uint8_t p[] = { 0x01, 0xD8 };                         /* add eax,ebx */
      PM32_CPU c = mk(p, 2); c.Registers[0] = 0x7FFFFFFF; c.Registers[3] = 1;
      CHECK(Pm32Step(&c) && c.Registers[0] == 0x80000000u && c.Eip == CODE + 2, "add eax,ebx: 7FFFFFFF+1");
      CHECK((c.Flags & P32_OF) && (c.Flags & P32_SF) && !(c.Flags & P32_CF) && !(c.Flags & P32_ZF),
            "add: signed overflow sets OF+SF, not CF/ZF"); }
    { uint8_t p[] = { 0x2B, 0xC5 };                         /* sub eax,ebp */
      PM32_CPU c = mk(p, 2); c.Registers[0] = 1; c.Registers[5] = 2; Pm32Step(&c);
      CHECK(c.Registers[0] == 0xFFFFFFFFu && (c.Flags & P32_CF) && (c.Flags & P32_SF), "sub eax,ebp: 1-2 borrows"); }
    { uint8_t p[] = { 0x3B, 0x05, 0x10, 0x20, 0x00, 0x00 }; /* cmp eax,[2010h] */
      PM32_CPU c = mk(p, 6); c.Registers[0] = 5; MEM[0x2010] = 5; Pm32Step(&c);
      CHECK((c.Flags & P32_ZF) && c.Registers[0] == 5 && c.Eip == CODE + 6, "cmp eax,[disp32]: equal -> ZF, no store"); }
    { uint8_t p[] = { 0x83, 0xE1, 0x01 };                   /* and ecx,1 */
      PM32_CPU c = mk(p, 3); c.Registers[1] = 0x12345677; c.Flags |= P32_CF | P32_OF; Pm32Step(&c);
      CHECK(c.Registers[1] == 1 && !(c.Flags & (P32_CF | P32_OF | P32_ZF)), "and ecx,1: CF=OF=0"); }
    { uint8_t p[] = { 0x81, 0xE3, 0x00, 0x00, 0xFF, 0xFF }; /* and ebx,FFFF0000h */
      PM32_CPU c = mk(p, 6); c.Registers[3] = 0x12345678; Pm32Step(&c);
      CHECK(c.Registers[3] == 0x12340000u && c.Eip == CODE + 6, "and ebx,imm32"); }
    { uint8_t p[] = { 0x48 };                               /* dec eax: CF preserved */
      PM32_CPU c = mk(p, 1); c.Registers[0] = 1; c.Flags |= P32_CF; Pm32Step(&c);
      CHECK(c.Registers[0] == 0 && (c.Flags & P32_ZF) && (c.Flags & P32_CF), "dec eax: 1->0 sets ZF, keeps CF"); }
    { uint8_t p[] = { 0xFF, 0x0D, 0x58, 0x1A, 0x00, 0x00 }; /* dec dword [1A58h] */
      PM32_CPU c = mk(p, 6); MEM[0x1A58] = 1; MEM[0x1A59] = 0; MEM[0x1A5A] = 0; MEM[0x1A5B] = 0; Pm32Step(&c);
      CHECK(MEM[0x1A58] == 0 && (c.Flags & P32_ZF) && c.Eip == CODE + 6, "dec dword [disp32]: -> 0, ZF"); }
    { uint8_t p[] = { 0xF7, 0x05, 0x5C, 0x1A, 0x00, 0x00, 0xFE, 0xFF, 0xFF, 0xFF };  /* test dword [1A5Ch],FFFFFFFEh */
      PM32_CPU c = mk(p, 10); MEM[0x1A5C] = 1; MEM[0x1A5D] = MEM[0x1A5E] = MEM[0x1A5F] = 0; Pm32Step(&c);
      CHECK((c.Flags & P32_ZF) && c.Eip == CODE + 10, "test dword [disp32],imm32: 1 & FFFFFFFE = 0 -> ZF"); }
    { uint8_t p[] = { 0xF7, 0xE9 };                         /* imul ecx */
      PM32_CPU c = mk(p, 2); c.Registers[0] = 0xFFFFFFFE; c.Registers[1] = 3; Pm32Step(&c);
      CHECK(c.Registers[0] == 0xFFFFFFFAu && c.Registers[2] == 0xFFFFFFFFu && !(c.Flags & P32_CF), "imul ecx: -2*3 = -6 in EDX:EAX"); }
    { uint8_t p[] = { 0x0F, 0xAF, 0xC3 };                   /* imul eax,ebx */
      PM32_CPU c = mk(p, 3); c.Registers[0] = 0x10000; c.Registers[3] = 0x10000; Pm32Step(&c);
      CHECK(c.Registers[0] == 0 && (c.Flags & P32_CF) && (c.Flags & P32_OF), "imul eax,ebx: overflow -> CF=OF=1"); }
    { uint8_t p[] = { 0x99, 0xF7, 0x7E, 0x0A };             /* cdq ; idiv dword [esi+0Ah] */
      PM32_CPU c = mk(p, 4); c.Registers[0] = (uint32_t)-100; c.Registers[6] = 0x3000;
      MEM[0x300A] = 7; MEM[0x300B] = MEM[0x300C] = MEM[0x300D] = 0;
      Pm32Step(&c); Pm32Step(&c);
      CHECK(c.Registers[0] == (uint32_t)-14 && c.Registers[2] == (uint32_t)-2, "cdq; idiv: -100/7 = -14 r -2 (truncation)"); }
    { uint8_t p[] = { 0xF7, 0xF9 };                         /* idiv ecx, ecx=0 -> #DE */
      PM32_CPU c = mk(p, 2); PM32_CPU c0; c.Registers[1] = 0; c.Registers[0] = 5; c0 = c;
      CHECK(!Pm32Step(&c) && !memcmp(&c, &c0, sizeof c), "idiv by 0: DECLINES with nothing changed"); }

    /* ---- shifts ---- */
    { uint8_t p[] = { 0xC1, 0xEB, 0x02 };                   /* shr ebx,2 */
      PM32_CPU c = mk(p, 3); c.Registers[3] = 0x0B; Pm32Step(&c);
      CHECK(c.Registers[3] == 2 && (c.Flags & P32_CF), "shr ebx,2: 0Bh -> 2, CF = last bit out (1)"); }
    { uint8_t p[] = { 0xD3, 0xE0 };                         /* shl eax,cl */
      PM32_CPU c = mk(p, 2); c.Registers[0] = 3; c.Registers[1] = 2; Pm32Step(&c);
      CHECK(c.Registers[0] == 0x0C, "shl eax,cl: 3<<2 = 0Ch (Doom's two-plane mask)"); }
    { uint8_t p[] = { 0xD1, 0xE8 };                         /* shr eax,1 */
      PM32_CPU c = mk(p, 2); c.Registers[0] = 1; Pm32Step(&c);
      CHECK(c.Registers[0] == 0 && (c.Flags & P32_ZF) && (c.Flags & P32_CF), "shr eax,1: 1 -> 0, ZF+CF"); }
    { uint8_t p[] = { 0x0F, 0xA4, 0xE9, 0x16 };             /* shld ecx,ebp,16h */
      PM32_CPU c = mk(p, 4); c.Registers[1] = 0; c.Registers[5] = 0xABCD1234u; Pm32Step(&c);
      CHECK(c.Registers[1] == (0xABCD1234u >> 10) && c.Eip == CODE + 4, "shld ecx,ebp,22: brings in ebp's top 22 bits"); }
    { uint8_t p[] = { 0xD1, 0xD0 };                         /* rcl eax,1 -> decline */
      PM32_CPU c = mk(p, 2); PM32_CPU c0 = c;
      CHECK(!Pm32Step(&c) && !memcmp(&c, &c0, sizeof c), "rcl: declines, state untouched"); }

    /* ---- addressing ---- */
    { uint8_t p[] = { 0x8D, 0x7C, 0xED, 0x00 };             /* lea edi,[ebp+ebp*8+0] */
      PM32_CPU c = mk(p, 4); c.Registers[5] = 10; Pm32Step(&c);
      CHECK(c.Registers[7] == 90 && c.Eip == CODE + 4, "lea edi,[ebp+ebp*8]: 10*9 = 90"); }
    { uint8_t p[] = { 0x8A, 0x04, 0x0E };                   /* mov al,[esi+ecx] */
      PM32_CPU c = mk(p, 3); c.Registers[0] = 0x11223344; c.Registers[6] = 0x4000; c.Registers[1] = 5; MEM[0x4005] = 0x99; Pm32Step(&c);
      CHECK(c.Registers[0] == 0x11223399u, "mov al,[esi+ecx]: SIB, high bytes of EAX kept"); }
    { uint8_t p[] = { 0x88, 0x5F, 0x50 };                   /* mov [edi+50h],bl */
      PM32_CPU c = mk(p, 3); c.Registers[7] = 0x5000; c.Registers[3] = 0x77; Pm32Step(&c);
      CHECK(MEM[0x5050] == 0x77, "mov [edi+disp8],bl"); }
    { uint8_t p[] = { 0x66, 0x89, 0x17 };                   /* mov word [edi],dx */
      PM32_CPU c = mk(p, 3); c.Registers[7] = 0x5100; c.Registers[2] = 0xBEEF1234u; MEM[0x5102] = 0x5A; Pm32Step(&c);
      CHECK(MEM[0x5100] == 0x34 && MEM[0x5101] == 0x12 && MEM[0x5102] == 0x5A && c.Eip == CODE + 3,
            "66 mov [edi],dx: two bytes, no more"); }
    { uint8_t p[] = { 0x0F, 0xB6, 0x06, 0x0F, 0xBE, 0x06 };  /* movzx eax,byte[esi] ; movsx eax,byte[esi] */
      PM32_CPU c = mk(p, 6); c.Registers[6] = 0x5200; MEM[0x5200] = 0x80; Pm32Step(&c);
      CHECK(c.Registers[0] == 0x80, "movzx eax,byte [esi]");
      Pm32Step(&c);
      CHECK(c.Registers[0] == 0xFFFFFF80u, "movsx eax,byte [esi]"); }
    { uint8_t p[] = { 0xA1, 0x00, 0x60, 0x00, 0x00, 0xA3, 0x04, 0x60, 0x00, 0x00 };  /* mov eax,[6000h]; mov [6004h],eax */
      PM32_CPU c = mk(p, 10); MEM[0x6000] = 0x78; MEM[0x6001] = 0x56; MEM[0x6002] = 0x34; MEM[0x6003] = 0x12;
      Pm32Step(&c); Pm32Step(&c);
      CHECK(c.Registers[0] == 0x12345678u && MEM[0x6004] == 0x78 && MEM[0x6007] == 0x12, "mov eax,moffs / mov moffs,eax"); }

    /* ---- control flow ---- */
    { uint8_t p[] = { 0x75, 0x10 };                         /* jne +10h, ZF=0 -> taken */
      PM32_CPU c = mk(p, 2); Pm32Step(&c);
      CHECK(c.Eip == CODE + 0x12, "jne rel8 taken"); }
    { uint8_t p[] = { 0x0F, 0x8F, 0x00, 0x01, 0x00, 0x00 };  /* jg rel32, ZF=0 SF=OF -> taken */
      PM32_CPU c = mk(p, 6); Pm32Step(&c);
      CHECK(c.Eip == CODE + 6 + 0x100, "jg rel32 taken"); }
    { uint8_t p[] = { 0x0F, 0x88, 0x00, 0x01, 0x00, 0x00 };  /* js rel32, SF=0 -> not taken */
      PM32_CPU c = mk(p, 6); Pm32Step(&c);
      CHECK(c.Eip == CODE + 6, "js rel32 not taken"); }
    { uint8_t p[] = { 0xE8, 0x05, 0x00, 0x00, 0x00, 0x90, 0x90, 0x90, 0x90, 0x90, 0xC3 };  /* call +5 ; ... ; ret */
      PM32_CPU c = mk(p, 11); uint32_t sp = c.Registers[4];
      Pm32Step(&c);
      CHECK(c.Eip == CODE + 10 && c.Registers[4] == sp - 4, "call rel32: pushes return, jumps");
      Pm32Step(&c);
      CHECK(c.Eip == CODE + 5 && c.Registers[4] == sp, "ret: back to the instruction after the call"); }

    /* ---- stack ---- */
    { uint8_t p[] = { 0x60, 0x61 };                         /* pushad ; popad */
      PM32_CPU c = mk(p, 2); int k; uint32_t sp = c.Registers[4];
      for (k = 0; k < 8; ++k) if (k != 4) c.Registers[k] = 0x1000 + (uint32_t)k;
      Pm32Step(&c);
      CHECK(c.Registers[4] == sp - 32, "pushad: 32 bytes");
      for (k = 0; k < 8; ++k) if (k != 4) c.Registers[k] = 0;
      Pm32Step(&c);
      CHECK(c.Registers[4] == sp && c.Registers[0] == 0x1000 && c.Registers[7] == 0x1007 && c.Registers[5] == 0x1005, "popad: restores all, ESP back"); }

    /* ---- port I/O ---- */
    { uint8_t p[] = { 0xEE };                               /* out dx,al */
      PM32_CPU c = mk(p, 1); c.Registers[2] = 0x3C5; c.Registers[0] = 0x0C; g_out_n = 0; Pm32Step(&c);
      CHECK(g_out_n == 1 && g_out_port == 0x3C5 && g_out_val == 0x0C, "out dx,al: map mask 0Ch reaches the port"); }

    /* ---- exact decline ---- */
    { uint8_t p[] = { 0xD9, 0xE8 };                         /* fld1: FPU */
      PM32_CPU c = mk(p, 2); PM32_CPU c0 = c;
      CHECK(!Pm32Step(&c) && !memcmp(&c, &c0, sizeof c), "FPU: declines, nothing changed"); }
    { uint8_t p[] = { 0xCD, 0x31 };                         /* int 31h */
      PM32_CPU c = mk(p, 2); PM32_CPU c0 = c;
      CHECK(!Pm32Step(&c) && !memcmp(&c, &c0, sizeof c), "INT: declines"); }
    { uint8_t p[] = { 0x67, 0x8B, 0x07 };                   /* 16-bit addressing */
      PM32_CPU c = mk(p, 3); PM32_CPU c0 = c;
      CHECK(!Pm32Step(&c) && !memcmp(&c, &c0, sizeof c), "0x67 prefix: declines"); }

    /* ---- a drawer, end to end: the shape of Doom's R_DrawColumnLow ------------------
       pushad; mov edx,3C5h; mov eax,3; out dx,al; mov edi,5400h; mov ecx,4;
       L: mov [edi],bl; add edi,50h; dec ecx; jne L; popad; ret                        */
    { uint8_t p[] = { 0x60, 0xBA, 0xC5, 0x03, 0x00, 0x00, 0xB8, 0x03, 0x00, 0x00, 0x00, 0xEE,
                      0xBF, 0x00, 0x54, 0x00, 0x00, 0xB9, 0x04, 0x00, 0x00, 0x00,
                      0x88, 0x1F, 0x83, 0xC7, 0x50, 0x49, 0x75, 0xF8, 0x61, 0xC3 };
      PM32_CPU c = mk(p, (int)sizeof p); uint32_t sp, n = 0; int k;
      /* a return address on the stack, as if called */
      c.Registers[4] -= 4; MEM[c.Registers[4]] = 0x00; MEM[c.Registers[4] + 1] = 0x20; MEM[c.Registers[4] + 2] = 0; MEM[c.Registers[4] + 3] = 0;
      sp = c.Registers[4]; c.Registers[3] = 0xAB; c.Registers[7] = 0x1234; g_out_n = 0;
      while (c.Eip != 0x2000 && n < 1000 && Pm32Step(&c)) ++n;
      for (k = 0; k < 4; ++k) if (MEM[0x5400 + 0x50 * k] != 0xAB) break;
      CHECK(c.Eip == 0x2000 && c.Registers[4] == sp + 4, "drawer: runs to its ret and returns to the caller");
      CHECK(k == 4, "drawer: all four column bytes stored");
      CHECK(g_out_n == 1 && g_out_val == 3 && c.Registers[7] == 0x1234, "drawer: mask OUT seen once, registers restored by popad"); }

    printf("\n%d checks, %d failed\n", total, fails);
    return fails ? 1 : 0;
}
