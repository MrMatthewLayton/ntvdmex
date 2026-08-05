/* interp_test.c -- off-VM unit battery for the mode-12h fill-loop interpreter
 * (src/host/v86interp.h).
 *
 * The interpreter is the fast path that runs QuickBASIC's per-pixel PAINT/LINE
 * loops entirely in the host instead of taking one V86 round-trip per pixel.
 * Correctness here = "never derails": exact registers, exact flags, exact bail.
 * We exercise it over a flat 1MB+ memory array (no VGA planar engine -- that's
 * host-specific; here every address is plain RAM), so the decode, the flag
 * maths, the string ops, and the control flow are all checkable natively.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

typedef unsigned char BYTE;

static BYTE MEM[0x110000];
/* The host hooks v86interp.h requires (flat RAM, range-guarded). */
static uint8_t imem_r8(uint32_t lin) { return (lin < sizeof MEM) ? MEM[lin] : 0; }
static void    imem_w8(uint32_t lin, uint8_t v) { if (lin < sizeof MEM) MEM[lin] = v; }

/* Port-I/O hooks: a tiny model so the IN/OUT opcodes are exercised. Port 0x60
   returns a canned byte; writes to 0x3C5 are recorded for the OUT test. */
static uint8_t g_port3c5 = 0;
static uint32_t iio_in(uint16_t port, int width) { (void)width; return (port == 0x60) ? 0xA5 : 0; }
static void iio_out(uint16_t port, int width, uint32_t val) { (void)width; if (port == 0x3C5) g_port3c5 = (uint8_t)val; }

#include "../../src/host/v86interp.h"

static int total = 0, fails = 0;
#define CHECK(c,m) do{ total++; if(c){printf("  PASS  %s\n",(m));} \
    else{printf("  FAIL  %s\n",(m)); fails++;} }while(0)

/* Load code bytes at the linear address CS:IP and point the cpu there. */
static void load(icpu *c, uint16_t cs, uint16_t ip, const BYTE *b, int n)
{
    uint32_t lin = ((uint32_t)cs << 4) + ip; int i;
    c->seg[1] = cs; c->ip = ip;
    for (i = 0; i < n; i++) MEM[lin + i] = b[i];
}
/* Run until istep bails (unmodeled) or a generous cap; return steps taken. */
static int run(icpu *c)
{
    int n = 0;
    while (n < 10000000 && istep(c)) n++;
    return n;
}
/* Single-step exactly once. */
static int step1(icpu *c) { return istep(c); }

static icpu mkcpu(void)
{
    icpu c; memset(&c, 0, sizeof c);
    c.flags = 0x0002;                                 /* the always-set bit 1 */
    return c;
}

/* Tiny descriptor table for the LAR/LSL battery (run 55): sel 0x08 = code 0xFA
   limit 0xFFFF; sel 0x10 = data 0xF3 (G/D nibble 0x4) limit 0x25CF; else invalid. */
static int test_sel_desc(uint16_t sel, uint32_t *ar, uint32_t *limit)
{
    switch (sel & 0xFFF8) {
    case 0x08: if (ar) *ar = (0xFAu << 8);                        if (limit) *limit = 0xFFFF; return 1;
    case 0x10: if (ar) *ar = (0xF3u << 8) | (0x4u << 20);         if (limit) *limit = 0x25CF; return 1;
    default:   return 0;
    }
}

int main(void)
{
    printf("== mode-12h fill-loop interpreter battery ==\n");
    memset(MEM, 0, sizeof MEM);

    /* ---- T1: ADD AL,imm8 -> 0x80+0x80 = 0 (CF,ZF,OF,PF; not SF,AF) -------- */
    { icpu c = mkcpu(); BYTE p[] = { 0x04, 0x80 };    /* ADD AL,80h */
      c.r[0] = 0x0080; load(&c, 0x1000, 0, p, sizeof p); step1(&c);
      CHECK((c.r[0] & 0xFF) == 0x00, "add: 80+80 result = 0");
      CHECK((c.flags & F_CF) && (c.flags & F_ZF) && (c.flags & F_OF) && (c.flags & F_PF),
            "add: 80+80 sets CF+ZF+OF+PF");
      CHECK(!(c.flags & F_SF) && !(c.flags & F_AF), "add: 80+80 clears SF+AF"); }

    /* ---- T2: ADD AL,1 -> 0x7F+1 = 0x80 (OF,SF,AF; not CF,ZF,PF) ---------- */
    { icpu c = mkcpu(); BYTE p[] = { 0x04, 0x01 };
      c.r[0] = 0x007F; load(&c, 0x1000, 0, p, sizeof p); step1(&c);
      CHECK((c.r[0] & 0xFF) == 0x80, "add: 7F+1 result = 80");
      CHECK((c.flags & F_OF) && (c.flags & F_SF) && (c.flags & F_AF),
            "add: 7F+1 sets OF+SF+AF");
      CHECK(!(c.flags & F_CF) && !(c.flags & F_ZF) && !(c.flags & F_PF),
            "add: 7F+1 clears CF+ZF+PF"); }

    /* ---- T3: SUB AX,imm16 -> 1-2 = 0xFFFF (CF,SF,AF,PF; not OF,ZF) ------- */
    { icpu c = mkcpu(); BYTE p[] = { 0x2D, 0x02, 0x00 };
      c.r[0] = 0x0001; load(&c, 0x1000, 0, p, sizeof p); step1(&c);
      CHECK(c.r[0] == 0xFFFF, "sub: 1-2 result = FFFF");
      CHECK((c.flags & F_CF) && (c.flags & F_SF) && (c.flags & F_AF) && (c.flags & F_PF),
            "sub: 1-2 sets CF+SF+AF+PF");
      CHECK(!(c.flags & F_OF) && !(c.flags & F_ZF), "sub: 1-2 clears OF+ZF"); }

    /* ---- T4: CMP computes flags but does not store ---------------------- */
    { icpu c = mkcpu(); BYTE p[] = { 0x3D, 0x01, 0x00 };  /* CMP AX,1 */
      c.r[0] = 0x0001; load(&c, 0x1000, 0, p, sizeof p); step1(&c);
      CHECK(c.r[0] == 0x0001, "cmp: AX unchanged");
      CHECK((c.flags & F_ZF) && !(c.flags & F_CF), "cmp: 1==1 -> ZF, no CF"); }

    /* ---- T5: OR AL,AL on zero -> ZF+PF, CF/OF cleared ------------------- */
    { icpu c = mkcpu(); BYTE p[] = { 0x08, 0xC0 };   /* OR AL,AL */
      c.r[0] = 0x0000; c.flags |= F_CF | F_OF; load(&c, 0x1000, 0, p, sizeof p); step1(&c);
      CHECK((c.flags & F_ZF) && (c.flags & F_PF), "or: 0|0 sets ZF+PF");
      CHECK(!(c.flags & F_CF) && !(c.flags & F_OF), "or: clears CF+OF"); }

    /* ---- T6: INC/DEC preserve CF; INC 0xFFFF -> 0 with ZF, CF kept ------ */
    { icpu c = mkcpu(); BYTE p[] = { 0x40 };          /* INC AX */
      c.r[0] = 0xFFFF; c.flags |= F_CF; load(&c, 0x1000, 0, p, sizeof p); step1(&c);
      CHECK(c.r[0] == 0x0000 && (c.flags & F_ZF), "inc: FFFF+1 = 0, ZF");
      CHECK(c.flags & F_CF, "inc: preserves CF"); }

    /* ---- T7: ADC adds the carry-in -------------------------------------- */
    { icpu c = mkcpu(); BYTE p[] = { 0x14, 0x00 };    /* ADC AL,0 */
      c.r[0] = 0x0005; c.flags |= F_CF; load(&c, 0x1000, 0, p, sizeof p); step1(&c);
      CHECK((c.r[0] & 0xFF) == 0x06, "adc: 5+0+CF = 6"); }

    /* ---- T8: MOV reg<->mem + ModRM disp + segment override -------------- */
    { icpu c = mkcpu();
      /* MOV AL, ES:[BX+SI+2]  =  26 8A 40 02 */
      BYTE p[] = { 0x26, 0x8A, 0x40, 0x02 };
      c.seg[0] = 0x2000; c.r[3] = 0x0010; c.r[6] = 0x0004;   /* ES, BX, SI */
      MEM[((uint32_t)0x2000 << 4) + 0x16] = 0x9C;            /* ES:(0x10+4+2)=0x16 */
      load(&c, 0x1000, 0, p, sizeof p); step1(&c);
      CHECK((c.r[0] & 0xFF) == 0x9C, "mov: AL <- ES:[BX+SI+2]");
      CHECK(c.ip == 4, "mov: ip advanced by 4 (prefix+modrm+disp8)"); }

    /* ---- T9: [BP] defaults to SS --------------------------------------- */
    { icpu c = mkcpu(); BYTE p[] = { 0x8A, 0x46, 0x00 };     /* MOV AL,[BP+0] */
      c.seg[2] = 0x3000; c.r[5] = 0x0020;                    /* SS, BP */
      MEM[((uint32_t)0x3000 << 4) + 0x20] = 0x77;
      load(&c, 0x1000, 0, p, sizeof p); step1(&c);
      CHECK((c.r[0] & 0xFF) == 0x77, "mov: [BP] uses SS by default"); }

    /* ---- T10: MOV r/m,imm (C7) into RAM via [BX] ------------------------ */
    { icpu c = mkcpu(); BYTE p[] = { 0xC7, 0x07, 0x34, 0x12 };  /* MOV WORD [BX],1234h */
      c.seg[3] = 0x4000; c.r[3] = 0x0008;                    /* DS, BX */
      load(&c, 0x1000, 0, p, sizeof p); step1(&c);
      CHECK(MEM[((uint32_t)0x4000<<4)+8] == 0x34 && MEM[((uint32_t)0x4000<<4)+9] == 0x12,
            "mov: WORD [BX] = 1234h (little-endian)"); }

    /* ---- T11: REP STOSB fill (forward, DF=0) --------------------------- */
    { icpu c = mkcpu(); BYTE p[] = { 0xF3, 0xAA };    /* REP STOSB */
      c.seg[0] = 0x2000; c.r[7] = 0x0000; c.r[1] = 4; c.r[0] = 0x5A;  /* ES,DI,CX,AL */
      load(&c, 0x1000, 0, p, sizeof p); step1(&c);
      CHECK(MEM[0x20000]==0x5A && MEM[0x20001]==0x5A && MEM[0x20002]==0x5A && MEM[0x20003]==0x5A,
            "stos: REP fills 4 bytes");
      CHECK(c.r[1] == 0 && c.r[7] == 4, "stos: CX=0, DI advanced to 4"); }

    /* ---- T12: REP STOSW backward (DF=1) -------------------------------- */
    { icpu c = mkcpu(); BYTE p[] = { 0xFD, 0xF3, 0xAB };   /* STD; REP STOSW */
      c.seg[0] = 0x3000; c.r[7] = 0x0010; c.r[1] = 2; c.r[0] = 0x1234;
      load(&c, 0x1000, 0, p, sizeof p); step1(&c); step1(&c);   /* STD, then REP STOSW */
      CHECK(MEM[0x30010]==0x34 && MEM[0x30011]==0x12, "stosw/STD: word at DI");
      CHECK(MEM[0x3000E]==0x34 && MEM[0x3000F]==0x12, "stosw/STD: word at DI-2");
      CHECK(c.r[7] == 0x000C && c.r[1] == 0, "stosw/STD: DI=-4, CX=0"); }

    /* ---- T13: REP MOVSB copy ------------------------------------------- */
    { icpu c = mkcpu(); BYTE p[] = { 0xFC, 0xF3, 0xA4 };   /* CLD; REP MOVSB */
      c.seg[3] = 0x5000; c.seg[0] = 0x6000; c.r[6] = 0; c.r[7] = 0; c.r[1] = 3;
      MEM[0x50000]=0xDE; MEM[0x50001]=0xAD; MEM[0x50002]=0xBE;
      load(&c, 0x1000, 0, p, sizeof p); step1(&c); step1(&c);
      CHECK(MEM[0x60000]==0xDE && MEM[0x60001]==0xAD && MEM[0x60002]==0xBE,
            "movsb: DS:SI -> ES:DI x3"); }

    /* ---- T14: Jcc taken/not on ZF -------------------------------------- */
    { icpu c = mkcpu(); BYTE p[] = { 0x74, 0x10 };    /* JZ +0x10 */
      c.flags |= F_ZF; load(&c, 0x1000, 0, p, sizeof p); step1(&c);
      CHECK(c.ip == 0x12, "jz: taken -> ip = 2 + 0x10");
      c = mkcpu(); load(&c, 0x1000, 0, p, sizeof p); step1(&c);
      CHECK(c.ip == 0x02, "jz: not taken -> ip = 2"); }

    /* ---- T15: JMP short backward / NOP --------------------------------- */
    { icpu c = mkcpu(); BYTE p[] = { 0x90, 0xEB, 0xFD };  /* NOP; JMP -3 */
      load(&c, 0x1000, 0, p, sizeof p);
      step1(&c); CHECK(c.ip == 1, "nop: ip=1");
      step1(&c); CHECK(c.ip == 0, "jmp short: 3 + (-3) = 0"); }

    /* ---- T16: LOOP countdown (CX=5 -> runs body 5x, CX=0) -------------- */
    { icpu c = mkcpu();
      /* MOV CX,5 ; loop: NOP ; LOOP loop */
      BYTE p[] = { 0xB9, 0x05, 0x00, 0x90, 0xE2, 0xFD };
      load(&c, 0x1000, 0, p, sizeof p);
      step1(&c);                                      /* MOV CX,5 */
      { int guard = 0; while (c.ip != 6 && guard++ < 100) step1(&c); }
      CHECK(c.r[1] == 0, "loop: CX decremented to 0");
      CHECK(c.ip == 6, "loop: fell through after CX hit 0"); }

    /* ---- T17: TEST sets flags, no store -------------------------------- */
    { icpu c = mkcpu(); BYTE p[] = { 0xA8, 0x01 };    /* TEST AL,1 */
      c.r[0] = 0x00F0; load(&c, 0x1000, 0, p, sizeof p); step1(&c);
      CHECK(c.r[0] == 0x00F0 && (c.flags & F_ZF), "test: AL&1==0 -> ZF, AL kept"); }

    /* ---- T18: bail on unmodeled opcode (INT 20h) leaves state at it ----- */
    { icpu c = mkcpu(); BYTE p[] = { 0x90, 0xCD, 0x20 };   /* NOP; INT 20h */
      load(&c, 0x1000, 0, p, sizeof p);
      CHECK(step1(&c) == 1 && c.ip == 1, "bail: NOP runs");
      CHECK(step1(&c) == 0 && c.ip == 1, "bail: INT 20h returns 0, ip unchanged"); }

    /* ---- T19: 32-bit operand-size (0x66) -- run 54 --------------------- *
     * A C runtime under DPMI does 32-bit register math in a 16-bit segment  *
     * via the 0x66 prefix (run 53's I310102 stopped on MOVZX ESI,SI). These *
     * exercise the widened register file + width-aware helpers.             */
    { icpu c = mkcpu(); BYTE p[] = { 0x66, 0x0F, 0xB7, 0xF6 };   /* MOVZX ESI,SI */
      load(&c, 0x1000, 0, p, sizeof p); c.r[6] = 0x1234ABCD;
      CHECK(step1(&c) == 1 && c.ip == 4, "66 0F B7: MOVZX ESI,SI runs, ip += 4");
      CHECK(c.r[6] == 0x0000ABCD, "movzx: ESI = zero-extended SI"); }

    { icpu c = mkcpu(); BYTE p[] = { 0x66, 0xC1, 0xE6, 0x04 };   /* SHL ESI,4 */
      load(&c, 0x1000, 0, p, sizeof p); c.r[6] = 0x0000ABCD;
      CHECK(step1(&c) == 1 && c.r[6] == 0x000ABCD0, "66 C1 /4: SHL ESI,4 (32-bit)"); }

    { icpu c = mkcpu(); BYTE p[] = { 0x66, 0xB8, 0x78, 0x56, 0x34, 0x12 };  /* MOV EAX,imm32 */
      load(&c, 0x1000, 0, p, sizeof p);
      CHECK(step1(&c) == 1 && c.ip == 6, "66 B8: MOV EAX,imm32 runs, ip += 6");
      CHECK(c.r[0] == 0x12345678, "mov: EAX = imm32"); }

    { icpu c = mkcpu(); BYTE p[] = { 0x66, 0x01, 0xC0 };         /* ADD EAX,EAX */
      load(&c, 0x1000, 0, p, sizeof p); c.r[0] = 0x80000000u;
      CHECK(step1(&c) == 1 && c.r[0] == 0, "66 01: ADD EAX,EAX = 0 (32-bit wrap)");
      CHECK((c.flags & F_CF) && (c.flags & F_ZF), "add32: CF+ZF at 32-bit boundary"); }

    { icpu c = mkcpu(); BYTE p[] = { 0x66, 0x0F, 0xBE, 0xC0 };   /* MOVSX EAX,AL */
      load(&c, 0x1000, 0, p, sizeof p); c.r[0] = 0x00000080u;
      CHECK(step1(&c) == 1 && c.r[0] == 0xFFFFFF80u, "66 0F BE: MOVSX EAX,AL sign-extends"); }

    /* partial-register semantics: a 16-bit write preserves E-reg[31:16] */
    { icpu c = mkcpu(); BYTE p[] = { 0xB8, 0xCD, 0xAB };         /* MOV AX,0xABCD (no 0x66) */
      load(&c, 0x1000, 0, p, sizeof p); c.r[0] = 0x12345678u;
      CHECK(step1(&c) == 1 && c.r[0] == 0x1234ABCDu, "mov ax preserves high EAX"); }

    /* PUSH/POP r32 with 0x66: 4-byte stack slot, SP +/- 4 */
    { icpu c = mkcpu(); BYTE p[] = { 0x66, 0x53, 0x66, 0x5B };   /* PUSH EBX; POP EBX */
      load(&c, 0x1000, 0, p, sizeof p);
      c.seg[2] = 0x0000; c.r[4] = 0x0100; c.r[3] = 0xCAFEF00Du;
      CHECK(step1(&c) == 1 && (c.r[4] & 0xFFFF) == 0x00FC, "66 push ebx: SP -= 4");
      CHECK(rd_mem(0xFC, 4) == 0xCAFEF00Du, "66 push ebx: 4 bytes on stack");
      c.r[3] = 0;
      CHECK(step1(&c) == 1 && c.r[3] == 0xCAFEF00Du && (c.r[4] & 0xFFFF) == 0x0100,
            "66 pop ebx: value + SP restored"); }

    /* a bare 0x66 before a byte op is ignored (operand size irrelevant) */
    { icpu c = mkcpu(); BYTE p[] = { 0x66, 0x04, 0x01 };         /* ADD AL,1 (66 ignored) */
      load(&c, 0x1000, 0, p, sizeof p); c.r[0] = 0x05;
      CHECK(step1(&c) == 1 && (c.r[0] & 0xFF) == 0x06, "66 before byte-op: width stays 1"); }

    /* ---- T20: LAR/LSL descriptor introspection -- run 55 --------------- *
     * A DPMI C runtime reads a descriptor's access byte with LAR;CX / SHR.  *
     * In V86 (g_sel_desc==NULL) these bail; with the hook they consult it.  */
    g_sel_desc = test_sel_desc;
    { icpu c = mkcpu(); BYTE p[] = { 0x66, 0x0F, 0x02, 0xC9 };   /* LAR ECX,CX */
      load(&c, 0x1000, 0, p, sizeof p); c.r[1] = 0x0008;         /* CX = sel 0x08 (code 0xFA) */
      CHECK(step1(&c) == 1 && c.ip == 4, "66 0F 02: LAR ECX,CX runs, ip += 4");
      CHECK(c.r[1] == 0x0000FA00 && (c.flags & F_ZF), "lar: ECX = access<<8, ZF set (valid sel)"); }

    { icpu c = mkcpu(); BYTE p[] = { 0x66, 0x0F, 0x02, 0xC9, 0x66, 0xC1, 0xE9, 0x08 }; /* LAR;SHR ECX,8 */
      load(&c, 0x1000, 0, p, sizeof p); c.r[1] = 0x0010;         /* CX = sel 0x10 (data 0xF3) */
      step1(&c); step1(&c);                                      /* LAR then SHR ECX,8 */
      CHECK((c.r[1] & 0xFF) == 0xF3, "lar+shr: CL = descriptor access byte (the C-runtime idiom)"); }

    { icpu c = mkcpu(); BYTE p[] = { 0x66, 0x0F, 0x02, 0xC9 };   /* LAR ECX,CX -- invalid sel */
      load(&c, 0x1000, 0, p, sizeof p); c.r[1] = 0x0000; c.flags |= F_ZF; /* ZF preset */
      CHECK(step1(&c) == 1 && !(c.flags & F_ZF), "lar: invalid selector clears ZF");
      CHECK(c.r[1] == 0x0000, "lar: dest unchanged on invalid selector"); }

    { icpu c = mkcpu(); BYTE p[] = { 0x66, 0x0F, 0x03, 0xC9 };   /* LSL ECX,CX */
      load(&c, 0x1000, 0, p, sizeof p); c.r[1] = 0x0010;         /* CX = sel 0x10 (limit 0x25CF) */
      CHECK(step1(&c) == 1 && c.r[1] == 0x000025CF && (c.flags & F_ZF),
            "66 0F 03: LSL ECX,CX = byte limit, ZF set"); }
    g_sel_desc = 0;

    /* ---- T20: full read-scan fill loop, exit by counter ---------------- *
     * MOV AL,ES:[SI] / OR AL,AL / JNZ found / INC SI / DEC DI / JNZ loop    *
     * with all-zero pixels and DI=4: runs 4 iterations, then falls to a     *
     * bail opcode -- exactly the BUBBLES PAINT pattern, all in the host.    */
    { icpu c = mkcpu();
      BYTE p[] = {
        /*00*/ 0x26, 0x8A, 0x04,    /* MOV AL, ES:[SI] */
        /*03*/ 0x0A, 0xC0,          /* OR  AL, AL      */
        /*05*/ 0x75, 0x06,          /* JNZ found(+6 -> 0x0D) */
        /*07*/ 0x46,                /* INC SI          */
        /*08*/ 0x4F,                /* DEC DI          */
        /*09*/ 0x75, 0xF5,          /* JNZ loop(-11 -> 0x00) */
        /*0B*/ 0x90, 0x90,          /* (pad)           */
        /*0D*/ 0xF4                 /* HLT (unmodeled -> bail) */
      };
      c.seg[0] = 0xA000; c.r[6] = 0; c.r[7] = 4;       /* ES, SI, DI */
      /* pixels all zero already (MEM is zeroed) */
      load(&c, 0x4000, 0, p, sizeof p);
      run(&c);
      CHECK(c.r[7] == 0 && c.r[6] == 4, "scan: counter loop exits at DI=0, SI=4");
      CHECK(c.ip == 0x0D, "scan: bailed exactly on the HLT"); }

    /* ---- T21: same scan, early exit on a nonzero pixel ----------------- */
    { icpu c = mkcpu();
      BYTE p[] = {
        0x26, 0x8A, 0x04, 0x0A, 0xC0, 0x75, 0x06,
        0x46, 0x4F, 0x75, 0xF5, 0x90, 0x90, 0xF4
      };
      c.seg[0] = 0xA000; c.r[6] = 0; c.r[7] = 8;
      MEM[((uint32_t)0xA000 << 4) + 2] = 0x77;         /* nonzero at offset 2 */
      load(&c, 0x4000, 0, p, sizeof p);
      run(&c);
      CHECK((c.r[0] & 0xFF) == 0x77 && c.r[6] == 2 && c.r[7] == 6,
            "scan: stops on nonzero pixel (AL=77, SI=2, DI=6)");
      CHECK(c.ip == 0x0D, "scan: nonzero exit bails on the HLT"); }

    /* ---- T22: XCHG r/m8,r8 with memory (QB pixel plot) ----------------- */
    { icpu c = mkcpu(); BYTE p[] = { 0x26, 0x86, 0x05 };   /* XCHG ES:[DI],AL */
      c.seg[0] = 0x7000; c.r[7] = 0x0004; c.r[0] = 0x00C3;     /* ES, DI, AL */
      MEM[((uint32_t)0x7000 << 4) + 4] = 0x2A;
      load(&c, 0x1000, 0, p, sizeof p); step1(&c);
      CHECK(MEM[((uint32_t)0x7000<<4)+4] == 0xC3, "xchg: memory got AL");
      CHECK((c.r[0] & 0xFF) == 0x2A, "xchg: AL got old memory value"); }

    /* ---- T23: XCHG r16,r16 (reg-reg) ----------------------------------- */
    { icpu c = mkcpu(); BYTE p[] = { 0x87, 0xD8 };    /* XCHG AX,BX */
      c.r[0] = 0x1111; c.r[3] = 0x2222; load(&c, 0x1000, 0, p, sizeof p); step1(&c);
      CHECK(c.r[0] == 0x2222 && c.r[3] == 0x1111, "xchg: AX<->BX"); }

    /* ---- T24: PUSH then POP round-trips through SS:SP ------------------- */
    { icpu c = mkcpu(); BYTE p[] = { 0x51, 0x5A };    /* PUSH CX ; POP DX */
      c.seg[2] = 0x8000; c.r[4] = 0x0100; c.r[1] = 0xBEEF;    /* SS, SP, CX */
      load(&c, 0x1000, 0, p, sizeof p);
      step1(&c); CHECK(c.r[4] == 0x00FE, "push: SP -= 2");
      CHECK(MEM[((uint32_t)0x8000<<4)+0xFE]==0xEF && MEM[((uint32_t)0x8000<<4)+0xFF]==0xBE,
            "push: word written at SS:SP");
      step1(&c); CHECK(c.r[2] == 0xBEEF && c.r[4] == 0x0100, "pop: DX=CX, SP restored"); }

    /* ---- T25: OUT imm8 + IN DX dispatched to the port hooks ------------- */
    { icpu c = mkcpu(); BYTE p[] = { 0xE6, 0x3C };    /* OUT 3Ch... no: imm port 0x3C */
      /* use OUT DX,AL to port 0x3C5, then IN AL,0x60 */
      BYTE q[] = { 0xEE, 0xE4, 0x60 };                /* OUT DX,AL ; IN AL,60h */
      (void)p;
      c.r[2] = 0x3C5; c.r[0] = 0x0042;                /* DX=3C5, AL=0x42 */
      load(&c, 0x1000, 0, q, sizeof q);
      step1(&c); CHECK(g_port3c5 == 0x42, "out: DX(3C5) <- AL via bus");
      step1(&c); CHECK((c.r[0] & 0xFF) == 0xA5, "in: AL <- port 0x60 via bus"); }

    /* ---- T26: CALL near relative + RET round-trip ---------------------- *
     * 00 MOV AX,1234 / 03 CALL +4 / 06 INC AX / 07 HLT / 0A INC BX / 0B RET */
    { icpu c = mkcpu();
      BYTE p[] = { 0xB8,0x34,0x12, 0xE8,0x04,0x00, 0x40, 0xF4, 0x90,0x90, 0x43, 0xC3 };
      c.seg[2] = 0x9000; c.r[4] = 0x0200;             /* SS, SP */
      load(&c, 0x1000, 0, p, sizeof p); run(&c);
      CHECK(c.r[0] == 0x1235, "call/ret: AX=1235 (INC AX after return)");
      CHECK(c.r[3] == 0x0001, "call/ret: BX=1 (subroutine ran)");
      CHECK(c.r[4] == 0x0200, "call/ret: SP restored");
      CHECK(c.ip == 0x07, "call/ret: bailed on HLT after return"); }

    /* ---- T27: CALL near indirect via register (FF /2) ------------------ *
     * 00 CALL SI(=06) / 02 INC AX / 03 HLT / 06 RET                        */
    { icpu c = mkcpu();
      BYTE p[] = { 0xFF,0xD6, 0x40, 0xF4, 0x90,0x90, 0xC3 };
      c.seg[2] = 0x9000; c.r[4] = 0x0200; c.r[6] = 0x0006;   /* SS, SP, SI */
      load(&c, 0x1000, 0, p, sizeof p); run(&c);
      CHECK(c.r[0] == 0x0001 && c.r[4] == 0x0200 && c.ip == 0x03,
            "call indirect: ran subroutine via SI, SP restored"); }

    /* ---- T28: SHR builds a bit-mask (QB's 0x80 >> x) -------------------- */
    { icpu c = mkcpu(); BYTE p[] = { 0xD2, 0xE8 };   /* SHR AL, CL */
      c.r[0] = 0x0080; c.r[1] = 0x0003;              /* AL=0x80, CL=3 */
      load(&c, 0x1000, 0, p, sizeof p); step1(&c);
      CHECK((c.r[0] & 0xFF) == 0x10, "shr: 0x80 >> 3 = 0x10"); }

    /* ---- T29: SHL by 1 sets CF from the bit shifted out ----------------- */
    { icpu c = mkcpu(); BYTE p[] = { 0xD0, 0xE0 };   /* SHL AL, 1 */
      c.r[0] = 0x00C0; load(&c, 0x1000, 0, p, sizeof p); step1(&c);
      CHECK((c.r[0] & 0xFF) == 0x80, "shl: 0xC0 << 1 = 0x80");
      CHECK((c.flags & F_CF) != 0, "shl: CF = bit shifted out"); }

    /* ---- T30: ROR by 1, CF = rotated bit; result wraps ------------------ */
    { icpu c = mkcpu(); BYTE p[] = { 0xD0, 0xC8 };   /* ROR AL, 1 */
      c.r[0] = 0x0001; load(&c, 0x1000, 0, p, sizeof p); step1(&c);
      CHECK((c.r[0] & 0xFF) == 0x80 && (c.flags & F_CF), "ror: 0x01 ror 1 = 0x80, CF=1"); }

    /* ---- T31: SHR imm8 (C0 /5) with flags ------------------------------- */
    { icpu c = mkcpu(); BYTE p[] = { 0xC0, 0xE8, 0x04 };   /* SHR AL, 4 */
      c.r[0] = 0x00A5; load(&c, 0x1000, 0, p, sizeof p); step1(&c);
      CHECK((c.r[0] & 0xFF) == 0x0A, "shr: 0xA5 >> 4 = 0x0A"); }

    /* ---- T32: PUSH/POP ES round-trips (the per-pixel bail) -------------- */
    { icpu c = mkcpu(); BYTE p[] = { 0x06, 0x1F };   /* PUSH ES ; POP DS */
      c.seg[2] = 0x9000; c.r[4] = 0x0100; c.seg[0] = 0xA000;  /* SS,SP,ES */
      load(&c, 0x1000, 0, p, sizeof p);
      step1(&c); CHECK(c.r[4] == 0x00FE, "push ES: SP -= 2");
      step1(&c); CHECK(c.seg[3] == 0xA000 && c.r[4] == 0x0100, "pop DS = pushed ES"); }

    /* ---- T33: MOV Sreg,r/m and MOV r/m,Sreg --------------------------- */
    { icpu c = mkcpu(); BYTE p[] = { 0x8E, 0xC0, 0x8C, 0xC3 };  /* MOV ES,AX ; MOV BX,ES */
      c.r[0] = 0xB800; load(&c, 0x1000, 0, p, sizeof p);
      step1(&c); CHECK(c.seg[0] == 0xB800, "mov ES,AX");
      step1(&c); CHECK(c.r[3] == 0xB800, "mov BX,ES"); }

    /* ---- T34: LEA loads the offset, not the memory contents ------------ */
    { icpu c = mkcpu(); BYTE p[] = { 0x8D, 0x41, 0x06 };   /* LEA AX,[BX+DI+6] */
      c.r[3] = 0x0010; c.r[7] = 0x0004;              /* BX, DI */
      MEM[0x1A] = 0xFF;                              /* would be wrong to load */
      load(&c, 0x1000, 0, p, sizeof p); step1(&c);
      CHECK(c.r[0] == 0x001A, "lea: AX = BX+DI+6 = 0x1A (offset, not [0x1A])"); }

    /* ---- T35: PUSH imm16 (68) writes a W-wide slot; SP -= 2 -- run 56 ---- *
     * The exact opcode run 55 stopped on: 68 3a 02 = PUSH 0x023A.           */
    { icpu c = mkcpu(); BYTE p[] = { 0x68, 0x3A, 0x02 };   /* PUSH 0x023A */
      c.seg[2] = 0x8000; c.r[4] = 0x0100;                  /* SS, SP */
      load(&c, 0x1000, 0, p, sizeof p);
      CHECK(step1(&c) == 1 && c.ip == 3 && c.r[4] == 0x00FE, "68: PUSH imm16, ip+=3, SP-=2");
      CHECK(MEM[((uint32_t)0x8000<<4)+0xFE]==0x3A && MEM[((uint32_t)0x8000<<4)+0xFF]==0x02,
            "push imm16: word 0x023A written at SS:SP"); }

    /* ---- T36: PUSH imm8 (6A) sign-extends to the 16-bit slot ------------ */
    { icpu c = mkcpu(); BYTE p[] = { 0x6A, 0xFF };         /* PUSH -1 (byte) */
      c.seg[2] = 0x8000; c.r[4] = 0x0100;
      load(&c, 0x1000, 0, p, sizeof p);
      CHECK(step1(&c) == 1 && c.ip == 2 && c.r[4] == 0x00FE, "6A: PUSH imm8, ip+=2, SP-=2");
      CHECK(MEM[((uint32_t)0x8000<<4)+0xFE]==0xFF && MEM[((uint32_t)0x8000<<4)+0xFF]==0xFF,
            "push imm8: -1 sign-extended to 0xFFFF"); }

    /* ---- T37: PUSH imm round-trips through POP (value + flags intact) --- */
    { icpu c = mkcpu(); BYTE p[] = { 0x6A, 0x7F, 0x58 };   /* PUSH 0x7F ; POP AX */
      c.seg[2] = 0x8000; c.r[4] = 0x0100;
      load(&c, 0x1000, 0, p, sizeof p);
      step1(&c); step1(&c);
      CHECK(c.r[0] == 0x007F && c.r[4] == 0x0100, "push imm8/pop: AX=0x7F, SP restored"); }

    /* ---- T38: 32-bit PUSH imm32 (66 68) -- a 32-bit C runtime arg ------- */
    { icpu c = mkcpu(); BYTE p[] = { 0x66, 0x68, 0x78, 0x56, 0x34, 0x12 }; /* PUSH 0x12345678 */
      c.seg[2] = 0x8000; c.r[4] = 0x0100;
      load(&c, 0x1000, 0, p, sizeof p);
      CHECK(step1(&c) == 1 && c.ip == 6 && c.r[4] == 0x00FC, "66 68: PUSH imm32, ip+=6, SP-=4");
      CHECK(MEM[((uint32_t)0x8000<<4)+0xFC]==0x78 && MEM[((uint32_t)0x8000<<4)+0xFF]==0x12,
            "push imm32: dword 0x12345678 written at SS:SP"); }

    /* ---- T39: RETF (CB) pops offset then a 2-byte selector into CS -- run 57 - *
     * The far-return that follows run 56's `PUSH seg; PUSH off; RETF` idiom.    */
    { icpu c = mkcpu(); BYTE p[] = { 0xCB };               /* RETF */
      c.seg[2] = 0x8000; c.r[4] = 0x0100;                  /* SS, SP */
      MEM[((uint32_t)0x8000<<4)+0x100] = 0x34;             /* [SP]   = offset 0x1234 */
      MEM[((uint32_t)0x8000<<4)+0x101] = 0x12;
      MEM[((uint32_t)0x8000<<4)+0x102] = 0x78;             /* [SP+2] = selector 0x5678 */
      MEM[((uint32_t)0x8000<<4)+0x103] = 0x56;
      load(&c, 0x1000, 0, p, sizeof p); step1(&c);
      CHECK(c.ip == 0x1234 && c.seg[1] == 0x5678, "CB: RETF sets IP=off, CS=selector");
      CHECK(c.r[4] == 0x0104, "retf: SP += 4 (offset + selector)"); }

    /* ---- T40: RETF imm16 (CA) also releases imm16 stack bytes ---------------- */
    { icpu c = mkcpu(); BYTE p[] = { 0xCA, 0x08, 0x00 };   /* RETF 8 */
      c.seg[2] = 0x8000; c.r[4] = 0x0100;
      MEM[((uint32_t)0x8000<<4)+0x100] = 0x00; MEM[((uint32_t)0x8000<<4)+0x101] = 0x02; /* off 0x0200 */
      MEM[((uint32_t)0x8000<<4)+0x102] = 0x0F; MEM[((uint32_t)0x8000<<4)+0x103] = 0x00; /* sel 0x000F */
      load(&c, 0x1000, 0, p, sizeof p); step1(&c);
      CHECK(c.ip == 0x0200 && c.seg[1] == 0x000F, "CA: RETF imm16 sets CS:IP");
      CHECK(c.r[4] == 0x010C, "retf imm16: SP += 4 + 8"); }

    /* ---- T41: the full idiom -- PUSH seg; PUSH off; RETF far-transfers -------- *
     * seg_base = seg<<4 here (g_seg2lin NULL), so CS=0x0800 lands code at 0x8000. */
    { icpu c = mkcpu();
      BYTE code[] = { 0x68, 0x00, 0x08,   /* PUSH 0x0800 (target segment) */
                      0x68, 0x00, 0x01,   /* PUSH 0x0100 (target offset)  */
                      0xCB };             /* RETF -> 0x0800:0x0100        */
      c.seg[2] = 0x9000; c.r[4] = 0x0200;                 /* SS, SP */
      load(&c, 0x1000, 0, code, sizeof code);
      MEM[((uint32_t)0x0800<<4)+0x100] = 0xF4;            /* HLT at the target -> run() bails */
      run(&c);
      CHECK(c.seg[1] == 0x0800 && c.ip == 0x0100, "push seg/off + RETF: transferred to 0800:0100");
      CHECK(c.r[4] == 0x0200, "far-transfer: SP back to start (2 pushes + retf pop 4)"); }

    /* ---- T42: LEAVE (C9) -- MOV SP,BP; POP BP, the callee epilogue -- run 58 - *
     * SP starts below BP (locals allocated); LEAVE discards them (SP<-BP) then   *
     * pops the caller's BP. Paired with ENTER / `PUSH BP; MOV BP,SP`.            */
    { icpu c = mkcpu(); BYTE p[] = { 0xC9 };               /* LEAVE */
      c.seg[2] = 0x8000; c.r[4] = 0x00F8; c.r[5] = 0x0100;  /* SS, SP (locals), BP */
      MEM[((uint32_t)0x8000<<4)+0x100] = 0xBC;             /* [BP] = caller's BP 0x0ABC */
      MEM[((uint32_t)0x8000<<4)+0x101] = 0x0A;
      load(&c, 0x1000, 0, p, sizeof p); step1(&c);
      CHECK(c.ip == 1, "C9: LEAVE, ip += 1");
      CHECK(c.r[5] == 0x0ABC, "leave: BP <- caller's BP popped from [old BP]");
      CHECK(c.r[4] == 0x0102, "leave: SP <- BP then +2 (locals discarded, BP popped)"); }

    /* ---- T43: LEAVE preserves the high 16 bits of ESP/EBP (partial-reg) ------ */
    { icpu c = mkcpu(); BYTE p[] = { 0xC9 };               /* LEAVE */
      c.seg[2] = 0x8000; c.r[4] = 0xDEAD00F8; c.r[5] = 0xBEEF0100;
      MEM[((uint32_t)0x8000<<4)+0x100] = 0xBC; MEM[((uint32_t)0x8000<<4)+0x101] = 0x0A;
      load(&c, 0x1000, 0, p, sizeof p); step1(&c);
      CHECK(c.r[5] == 0xBEEF0ABC && c.r[4] == 0xDEAD0102,
            "leave: E-reg high halves of SP/BP preserved (16-bit LEAVE)"); }

    /* ---- T44: PUSHF (9C) -- push the modeled FLAGS + reserved bit 1 -- run 59 - *
     * SP -= 2; [SP] = (flags & modeled-mask) | 0x0002. IF/TF/IOPL/NT not modeled. */
    { icpu c = mkcpu(); BYTE p[] = { 0x9C };               /* PUSHF */
      c.seg[2] = 0x8000; c.r[4] = 0x0100;
      c.flags = F_CF | F_ZF | F_SF | F_DF;                /* 0x04C1 + reserved */
      load(&c, 0x1000, 0, p, sizeof p); step1(&c);
      CHECK(c.ip == 1, "9C: PUSHF, ip += 1");
      CHECK(c.r[4] == 0x00FE, "pushf: SP -= 2");
      { uint16_t w = MEM[((uint32_t)0x8000<<4)+0xFE] | (MEM[((uint32_t)0x8000<<4)+0xFF]<<8);
        CHECK(w == ((F_CF|F_ZF|F_SF|F_DF) | 0x0002u), "pushf: pushed FLAGS = modeled bits + reserved bit 1"); } }

    /* ---- T45: POPF (9D) -- load FLAGS from stack (modeled bits only) ---------- */
    { icpu c = mkcpu(); BYTE p[] = { 0x9D };               /* POPF */
      c.seg[2] = 0x8000; c.r[4] = 0x00FE;
      MEM[((uint32_t)0x8000<<4)+0xFE] = (F_CF|F_OF|F_PF) & 0xFF; /* low byte 0x05 */
      MEM[((uint32_t)0x8000<<4)+0xFF] = ((F_OF) >> 8) & 0xFF;    /* high byte 0x08 (OF) */
      load(&c, 0x1000, 0, p, sizeof p); step1(&c);
      CHECK(c.r[4] == 0x0100, "popf: SP += 2");
      CHECK((c.flags & (F_CF|F_PF|F_OF)) == (F_CF|F_PF|F_OF), "popf: CF/PF/OF restored from stack");
      CHECK((c.flags & 0x0002u) && !(c.flags & F_ZF), "popf: reserved bit set, ZF cleared (not on stack)"); }

    /* ---- T46: PUSHF/POPF round-trip is exact for modeled flags; SP high half kept */
    { icpu c = mkcpu(); BYTE p[] = { 0x9C, 0x31, 0xC0, 0x9D };  /* PUSHF; XOR AX,AX; POPF */
      c.seg[2] = 0x8000; c.r[4] = 0xCAFE0100;
      c.flags = F_AF | F_SF | F_DF;                       /* clobbered by XOR, restored by POPF */
      load(&c, 0x1000, 0, p, sizeof p); step1(&c); step1(&c); step1(&c);
      CHECK((c.flags & (F_AF|F_SF|F_DF)) == (F_AF|F_SF|F_DF), "pushf/popf: modeled flags round-trip exactly");
      CHECK(c.r[4] == 0xCAFE0100, "pushf/popf: SP back to start, E-reg high half preserved"); }

    printf("\n%d checks, %d failed\n", total, fails);
    return fails ? 1 : 0;
}
