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

    /* ---- T19: bail on 32-bit prefix (0x66) ----------------------------- */
    { icpu c = mkcpu(); BYTE p[] = { 0x66, 0x40 };
      load(&c, 0x1000, 0, p, sizeof p);
      CHECK(step1(&c) == 0 && c.ip == 0, "bail: 0x66 operand-size prefix"); }

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

    printf("\n%d checks, %d failed\n", total, fails);
    return fails ? 1 : 0;
}
