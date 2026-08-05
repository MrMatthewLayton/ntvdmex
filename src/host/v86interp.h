/* v86interp.h -- bounded, flags-accurate 8086 interpreter for the mode-12h
 * fill-loop fast path.
 *
 * In mode 12h the A0000 window is PAGE_NOACCESS, so every guest pixel touch
 * faults to us. QuickBASIC's PAINT/LINE fills are tight per-pixel loops (e.g.
 * `MOV AL,ES:[SI] / OR AL,AL / JNZ / DEC DI / JNZ`), so one fill = hundreds of
 * thousands of V86 round-trips and never finishes. The fix is to run the whole
 * inner loop here -- loads/stores (planar engine for A0000, flat for normal
 * RAM), the arithmetic/logic group (computing CF/PF/AF/ZF/SF/OF), INC/DEC,
 * string ops (REP, honouring DF), MOV, TEST, the flag ops, and Jcc/JMP/LOOP --
 * until we hit an opcode we don't model or an iteration cap, then return to V86.
 * It NEVER derails: any unmodeled byte stops with IP exactly on that
 * instruction so V86 re-executes it. 16-bit only (0x66/0x67/LOCK bail).
 *
 * The includer MUST, before #include'ing this header, provide:
 *   - the fixed-width int types (uint8_t/uint16_t/uint32_t/int8_t/int16_t) + BYTE
 *   - uint8_t imem_r8(uint32_t lin);  void imem_w8(uint32_t lin, uint8_t v);
 *     (guest byte read/write; A0000 reads must load the VGA latches.)
 *   - uint32_t iio_in(uint16_t port, int width);
 *     void     iio_out(uint16_t port, int width, uint32_t val);
 *     (port I/O dispatched to the device bus, for VGA-register-per-pixel loops.)
 * This keeps the interpreter host-agnostic so it can be unit-tested off-VM
 * against a flat memory array (see tools/dostest/interp_test.c).
 */
#ifndef V86INTERP_H
#define V86INTERP_H

#define GUEST_HI 0x110000u                            /* 1MB + HMA: guest linear range */
#define F_CF 0x0001u
#define F_PF 0x0004u
#define F_AF 0x0010u
#define F_ZF 0x0040u
#define F_SF 0x0080u
#define F_DF 0x0400u
#define F_OF 0x0800u

typedef struct {
    uint32_t r[8];      /* E-AX/CX/DX/BX/SP/BP/SI/DI (full 32-bit; 16-/8-bit views mask) */
    uint16_t seg[6];    /* ES CS SS DS FS GS       (x86 sreg encoding)  */
    uint16_t ip;        /* address size stays 16-bit (the 0x67 prefix still bails)      */
    uint32_t flags;
} icpu;

static int iparity(uint8_t v) { v ^= v >> 4; v ^= v >> 2; v ^= v >> 1; return !(v & 1); }

/* Operand-width helpers: w is 1/2/4 bytes. The 4-byte path exists for 16-bit code
   that uses the 0x66 operand-size prefix (386 32-bit register math -- e.g. a C
   runtime's MOVZX ESI,SI / SHL ESI,4). run 54. */
static uint32_t wmask(int w) { return (w == 1) ? 0xFFu : (w == 2) ? 0xFFFFu : 0xFFFFFFFFu; }
static uint32_t wsign(int w) { return (w == 1) ? 0x80u : (w == 2) ? 0x8000u : 0x80000000u; }

/* Segment-register -> linear-base resolver. NULL = V86 semantics (base = seg<<4).
   For protected mode (DPMI, GH #2) the host sets this to an LDT-selector->base
   lookup, so the SAME 16-bit interpreter core runs PM code -- descriptor bases
   instead of paragraph shifts. This is the one change that lets us execute PM in
   the host and never hand a faulting instruction to the kernel's VDM-fault path
   (which deadlocks on a PM #GP -- run 52). An interpreter enforces no descriptor
   type/limit, so e.g. a write through a code-typed SS (what #GP's the real CPU in
   run 51's I310102) simply succeeds here. */
static uint32_t (*g_seg2lin)(uint16_t seg) = 0;
static uint32_t seg_base(uint16_t seg)
{ return g_seg2lin ? g_seg2lin(seg) : ((uint32_t)seg << 4); }

/* Descriptor-introspection hook for PM (LAR/LSL, run 55). Given a selector, returns
   1 if it names a valid, accessible descriptor and fills *ar (access-rights in LAR
   format: access byte at bits 8-15, G/D/AVL nibble at 20-23) and *limit (byte-
   granular). 0 = invalid selector (the op then clears ZF, leaving the dest reg
   unchanged). NULL in V86 mode -> LAR/LSL bail (they don't occur there). The DPMI
   host sets this to read its g_ldt[] table. */
static int (*g_sel_desc)(uint16_t sel, uint32_t *ar, uint32_t *limit) = 0;

static uint32_t rd_mem(uint32_t lin, int w)
{ uint32_t v = imem_r8(lin);
  if (w >= 2) v |= (uint32_t)imem_r8(lin + 1) << 8;
  if (w == 4) v |= ((uint32_t)imem_r8(lin + 2) << 16) | ((uint32_t)imem_r8(lin + 3) << 24);
  return v; }
static void wr_mem(uint32_t lin, int w, uint32_t v)
{ imem_w8(lin, (uint8_t)v);
  if (w >= 2) imem_w8(lin + 1, (uint8_t)(v >> 8));
  if (w == 4) { imem_w8(lin + 2, (uint8_t)(v >> 16)); imem_w8(lin + 3, (uint8_t)(v >> 24)); } }

/* CPU register file access by x86 encoding. Sub-register writes preserve the bits
   they don't touch: a 16-bit write keeps E-reg[31:16]; an 8-bit write keeps the
   other 24 bits (x86 partial-register semantics). */
static uint16_t g16(icpu *c, int e) { return (uint16_t)c->r[e & 7]; }
static void     s16(icpu *c, int e, uint16_t v) { c->r[e & 7] = (c->r[e & 7] & 0xFFFF0000u) | v; }
static uint8_t  g8(icpu *c, int e)
{ return (e < 4) ? (uint8_t)c->r[e] : (uint8_t)(c->r[e - 4] >> 8); }
static void     s8(icpu *c, int e, uint8_t v)
{ if (e < 4) c->r[e] = (c->r[e] & 0xFFFFFF00u) | v;
  else c->r[e - 4] = (c->r[e - 4] & 0xFFFF00FFu) | ((uint32_t)v << 8); }
/* read/write a register by operand width (1/2/4). */
static uint32_t grw(icpu *c, int e, int w)
{ return (w == 1) ? g8(c, e) : (w == 2) ? (uint32_t)(uint16_t)c->r[e & 7] : c->r[e & 7]; }
static void srw(icpu *c, int e, int w, uint32_t v)
{ if (w == 1) s8(c, e, (uint8_t)v); else if (w == 2) s16(c, e, (uint16_t)v); else c->r[e & 7] = v; }

/* Flag-computing ALU primitives (result masked to operand width w). */
static uint32_t do_add(icpu *c, uint32_t a, uint32_t b, int cin, int w)
{
    uint32_t m = wmask(w), sb = wsign(w);
    uint32_t fa = a & m, fb = b & m;
    uint64_t full = (uint64_t)fa + fb + (uint32_t)cin;   /* 64-bit: carry-safe for w==4 */
    uint32_t res = (uint32_t)(full & m);
    c->flags &= ~(F_CF | F_PF | F_AF | F_ZF | F_SF | F_OF);
    if (full & ((uint64_t)m + 1))               c->flags |= F_CF;
    if ((fa ^ fb ^ res) & 0x10u)                c->flags |= F_AF;
    if (!res)                                   c->flags |= F_ZF;
    if (res & sb)                               c->flags |= F_SF;
    if (iparity((uint8_t)res))                  c->flags |= F_PF;
    if ((~(fa ^ fb) & (fa ^ res)) & sb)         c->flags |= F_OF;
    return res;
}
static uint32_t do_sub(icpu *c, uint32_t a, uint32_t b, int cin, int w)
{
    uint32_t m = wmask(w), sb = wsign(w);
    uint32_t fa = a & m, fb = b & m, res = (fa - fb - (uint32_t)cin) & m;
    c->flags &= ~(F_CF | F_PF | F_AF | F_ZF | F_SF | F_OF);
    if ((uint64_t)fa < (uint64_t)fb + (uint32_t)cin)  c->flags |= F_CF;
    if ((fa ^ fb ^ res) & 0x10u)                c->flags |= F_AF;
    if (!res)                                   c->flags |= F_ZF;
    if (res & sb)                               c->flags |= F_SF;
    if (iparity((uint8_t)res))                  c->flags |= F_PF;
    if (((fa ^ fb) & (fa ^ res)) & sb)          c->flags |= F_OF;
    return res;
}
static void do_logic(icpu *c, uint32_t res, int w)
{
    uint32_t m = wmask(w), sb = wsign(w);
    res &= m;
    c->flags &= ~(F_CF | F_PF | F_AF | F_ZF | F_SF | F_OF);   /* CF=OF=0 */
    if (!res)                  c->flags |= F_ZF;
    if (res & sb)              c->flags |= F_SF;
    if (iparity((uint8_t)res)) c->flags |= F_PF;
}

/* aluop encoding 0..7 = ADD OR ADC SBB AND SUB XOR CMP. Returns result;
   CMP (7) computes flags only. */
static uint32_t do_alu(icpu *c, int aluop, uint32_t a, uint32_t b, int w)
{
    switch (aluop) {
    case 0: return do_add(c, a, b, 0, w);
    case 1: { uint32_t r = a | b; do_logic(c, r, w); return r; }
    case 2: return do_add(c, a, b, (c->flags & F_CF) ? 1 : 0, w);
    case 3: return do_sub(c, a, b, (c->flags & F_CF) ? 1 : 0, w);
    case 4: { uint32_t r = a & b; do_logic(c, r, w); return r; }
    case 5: return do_sub(c, a, b, 0, w);
    case 6: { uint32_t r = a ^ b; do_logic(c, r, w); return r; }
    default: do_sub(c, a, b, 0, w); return 0;             /* CMP: no store */
    }
}

/* Shift/rotate group-2 (sub 0..7 = ROL ROR RCL RCR SHL SHR SAL SAR), done one
   bit at a time so CF tracks correctly. Rotates touch only CF/OF; shifts also
   set SF/ZF/PF. OF is defined only for count 1. QuickBasic builds the mode-12h
   bit-mask with SHR (0x80 >> x) / SHL, so this is on the hot pixel path. */
static uint32_t do_shrot(icpu *c, int sub, uint32_t v, int cnt, int w)
{
    uint32_t m = wmask(w), sb = wsign(w), orig;
    int cf = (c->flags & F_CF) ? 1 : 0, oldcf, i;
    cnt &= 0x1F; v &= m; orig = v;
    if (cnt == 0) return v;                            /* x86: flags unchanged */
    for (i = 0; i < cnt; ++i) switch (sub) {
        case 0: cf = (v & sb) ? 1 : 0; v = ((v << 1) | (uint32_t)cf) & m; break;          /* ROL */
        case 1: cf = v & 1; v = ((v >> 1) | (cf ? sb : 0)) & m; break;                     /* ROR */
        case 2: oldcf = cf; cf = (v & sb) ? 1 : 0; v = ((v << 1) | (uint32_t)oldcf) & m; break; /* RCL */
        case 3: oldcf = cf; cf = v & 1; v = ((v >> 1) | (oldcf ? sb : 0)) & m; break;      /* RCR */
        case 4: case 6: cf = (v & sb) ? 1 : 0; v = (v << 1) & m; break;                    /* SHL/SAL */
        case 5: cf = v & 1; v = (v >> 1) & m; break;                                       /* SHR */
        default: cf = v & 1; v = ((v >> 1) | (v & sb)) & m; break;                         /* SAR */
    }
    c->flags = (c->flags & ~F_CF) | (cf ? F_CF : 0);
    if (cnt == 1) {                                    /* OF only defined for count 1 */
        int of;
        switch (sub) {
        case 5:  of = (orig & sb) ? 1 : 0; break;                      /* SHR: MSB of orig */
        case 7:  of = 0; break;                                        /* SAR */
        case 1: case 3: of = (((v & sb) ? 1 : 0) ^ ((v & (sb >> 1)) ? 1 : 0)); break; /* ROR/RCR */
        default: of = (((v & sb) ? 1 : 0) ^ cf); break;                /* ROL/RCL/SHL */
        }
        c->flags = (c->flags & ~F_OF) | (of ? F_OF : 0);
    }
    if (sub >= 4) {                                    /* shifts set SF/ZF/PF (not rotates) */
        c->flags &= ~(F_SF | F_ZF | F_PF);
        if (!v) c->flags |= F_ZF;
        if (v & sb) c->flags |= F_SF;
        if (iparity((uint8_t)v)) c->flags |= F_PF;
    }
    return v;
}

static int icond(icpu *c, int t)
{
    int cf = !!(c->flags & F_CF), zf = !!(c->flags & F_ZF), sf = !!(c->flags & F_SF),
        of = !!(c->flags & F_OF), pf = !!(c->flags & F_PF);
    switch (t & 0xF) {
    case 0x0: return of;            case 0x1: return !of;
    case 0x2: return cf;            case 0x3: return !cf;
    case 0x4: return zf;            case 0x5: return !zf;
    case 0x6: return cf || zf;      case 0x7: return !(cf || zf);
    case 0x8: return sf;            case 0x9: return !sf;
    case 0xA: return pf;            case 0xB: return !pf;
    case 0xC: return sf != of;      case 0xD: return sf == of;
    case 0xE: return zf || (sf != of);
    default:  return !(zf || (sf != of));
    }
}

/* Code-stream byte fetch: routed through imem_r8 (NOT a raw pointer) so the
   interpreter is fully memory-abstracted -- identical on the V86 host (code is
   never in the A0000 window, so imem_r8 returns the mapped byte) and testable
   off-VM against a flat array. `cb` is the linear address of CS:IP. */
#define CB(off) imem_r8(cb + (uint32_t)(off))

/* Decode a 16-bit ModRM byte at CB(idx). Fills *o (is_mem + linear addr or rm
   register, plus the reg field g). Returns bytes consumed (ModRM + disp). */
typedef struct { int is_mem; uint32_t lin; uint16_t ea; int g; int rm_reg; } modrm_t;
static int decode_modrm(icpu *c, uint32_t cb, int idx, int segov, modrm_t *o)
{
    BYTE mr = CB(idx); int mod = mr >> 6, rm = mr & 7, len = 1, bp = 0;
    uint16_t ea = 0, BX = c->r[3], BP = c->r[5], SI = c->r[6], DI = c->r[7];
    o->g = (mr >> 3) & 7;
    if (mod == 3) { o->is_mem = 0; o->rm_reg = rm; o->ea = 0; return 1; }
    switch (rm) {
    case 0: ea = (uint16_t)(BX + SI); break;  case 1: ea = (uint16_t)(BX + DI); break;
    case 2: ea = (uint16_t)(BP + SI); bp = 1; break;
    case 3: ea = (uint16_t)(BP + DI); bp = 1; break;
    case 4: ea = SI; break;                   case 5: ea = DI; break;
    case 6: if (mod == 0) { ea = (uint16_t)(CB(idx + 1) | (CB(idx + 2) << 8)); len += 2; }
            else { ea = BP; bp = 1; } break;
    default: ea = BX; break;
    }
    if (mod == 1)      { ea = (uint16_t)(ea + (int16_t)(signed char)CB(idx + len)); len += 1; }
    else if (mod == 2) { ea = (uint16_t)(ea + (CB(idx + len) | (CB(idx + len + 1) << 8))); len += 2; }
    o->is_mem = 1; o->ea = ea;
    o->lin = (seg_base(c->seg[(segov >= 0) ? segov : (bp ? 2 : 3)])) + ea;  /* SS if BP else DS */
    return len;
}

/* Execute one instruction. Returns 1 if modeled (state + IP advanced/jumped),
   0 to bail (state untouched at the current instruction). */
static int istep(icpu *c)
{
    uint32_t cb = (seg_base(c->seg[1])) + c->ip;   /* linear CS:IP */
    int idx = 0, segov = -1, rep = 0, osz = 0;
    int W;                                            /* word operand width: 4 if 0x66 else 2 */
    BYTE op;
    for (;;) {                                        /* prefixes */
        BYTE b = CB(idx);
        if      (b == 0x26) { segov = 0; idx++; }     /* ES */
        else if (b == 0x2E) { segov = 1; idx++; }     /* CS */
        else if (b == 0x36) { segov = 2; idx++; }     /* SS */
        else if (b == 0x3E) { segov = 3; idx++; }     /* DS */
        else if (b == 0x64) { segov = 4; idx++; }     /* FS */
        else if (b == 0x65) { segov = 5; idx++; }     /* GS */
        else if (b == 0xF3) { rep = 1; idx++; }
        else if (b == 0xF2) { rep = 2; idx++; }
        else if (b == 0x66) { osz = 1; idx++; }       /* run 54: operand-size -> 32-bit */
        else if (b == 0x67 || b == 0xF0) return 0;    /* addr-size / LOCK: still bail */
        else break;
        if (idx > 4) return 0;
    }
    op = CB(idx++);
    W = osz ? 4 : 2;                                  /* run 54: word operand width (0x66 -> 4) */

    /* ---- 0F two-byte map (run 54): MOVZX/MOVSX r, r/m -------------------- *
     * B6/B7 = zero-extend byte/word, BE/BF = sign-extend. Dest width = W, so *
     * `66 0F B7` gives MOVZX ESI,SI. Other 0F ops bail (the to-do signal).   */
    if (op == 0x0F) {
        BYTE op2 = CB(idx++);
        if (op2 == 0xB6 || op2 == 0xB7 || op2 == 0xBE || op2 == 0xBF) {
            int sw = (op2 & 1) ? 2 : 1;               /* source width */
            int sx = (op2 >= 0xBE);                   /* sign- vs zero-extend */
            modrm_t m; idx += decode_modrm(c, cb, idx, segov, &m);
            if (m.is_mem && m.lin >= GUEST_HI) return 0;
            { uint32_t v = m.is_mem ? rd_mem(m.lin, sw)
                                    : (sw == 1 ? g8(c, m.rm_reg) : (uint32_t)g16(c, m.rm_reg));
              if (sx && (v & wsign(sw))) v |= ~wmask(sw);   /* sign-extend to 32 */
              srw(c, m.g, W, v & wmask(W)); }
            c->ip = (uint16_t)(c->ip + idx); return 1;
        }
        if (op2 == 0x02 || op2 == 0x03) {             /* LAR / LSL r, r/m16 (run 55) */
            modrm_t m; uint16_t sel; uint32_t ar, lim;
            if (!g_sel_desc) return 0;                /* V86: no descriptor table -> bail */
            idx += decode_modrm(c, cb, idx, segov, &m);
            if (m.is_mem && m.lin >= GUEST_HI) return 0;
            sel = m.is_mem ? (uint16_t)rd_mem(m.lin, 2) : g16(c, m.rm_reg);  /* selector is 16-bit */
            if (g_sel_desc(sel, &ar, &lim)) {         /* valid -> load rights/limit, set ZF */
                srw(c, m.g, W, (op2 == 0x02 ? ar : lim) & wmask(W));
                c->flags |= F_ZF;
            } else c->flags &= ~F_ZF;                 /* invalid -> clear ZF, dest unchanged */
            c->ip = (uint16_t)(c->ip + idx); return 1;
        }
        return 0;                                     /* other 0F ops: bail */
    }

    /* ---- arithmetic/logic group: ADD..CMP, reg/mem forms ------------------ */
    if (op < 0x40 && (op & 7) < 6) {
        int aluop = (op >> 3) & 7, form = op & 7;
        int w = (form == 0 || form == 2 || form == 4) ? 1 : W;
        uint32_t a, b, res; int dmem = 0, dreg = 0; uint32_t dlin = 0;
        if (form <= 3) {
            modrm_t m; idx += decode_modrm(c, cb, idx, segov, &m);
            if (m.is_mem && m.lin >= GUEST_HI) return 0;
            uint32_t ev = m.is_mem ? rd_mem(m.lin, w) : grw(c, m.rm_reg, w);
            uint32_t gv = grw(c, m.g, w);
            if (form <= 1) { a = ev; b = gv; if (m.is_mem) { dmem = 1; dlin = m.lin; } else dreg = m.rm_reg; }
            else           { a = gv; b = ev; dreg = m.g; }
        } else if (form == 4) { a = g8(c, 0);  b = CB(idx++); dreg = 0; }
        else { a = grw(c, 0, w); b = rd_mem(cb + idx, w); idx += w; dreg = 0; }
        res = do_alu(c, aluop, a, b, w);
        if (aluop != 7) {
            if (dmem) wr_mem(dlin, w, res);
            else      srw(c, dreg, w, res);
        }
        c->ip = (uint16_t)(c->ip + idx); return 1;
    }

    /* ---- group1: ADD..CMP r/m, imm (80/81/83) ----------------------------- */
    if (op == 0x80 || op == 0x81 || op == 0x83) {
        int w = (op == 0x80) ? 1 : W; uint32_t a, b, res;
        modrm_t m; idx += decode_modrm(c, cb, idx, segov, &m);
        if (m.is_mem && m.lin >= GUEST_HI) return 0;
        a = m.is_mem ? rd_mem(m.lin, w) : grw(c, m.rm_reg, w);
        if (op == 0x81) { b = rd_mem(cb + idx, w); idx += w; }
        else { b = (uint32_t)(int32_t)(int8_t)CB(idx++); b &= wmask(w); }
        res = do_alu(c, m.g, a, b, w);
        if (m.g != 7) {
            if (m.is_mem) wr_mem(m.lin, w, res);
            else          srw(c, m.rm_reg, w, res);
        }
        c->ip = (uint16_t)(c->ip + idx); return 1;
    }

    /* ---- INC/DEC r16/r32 (40-4F) ------------------------------------------ */
    if (op >= 0x40 && op <= 0x4F) {
        int reg = op & 7; uint32_t cf = c->flags & F_CF;
        uint32_t res = (op >= 0x48) ? do_sub(c, grw(c, reg, W), 1, 0, W)
                                    : do_add(c, grw(c, reg, W), 1, 0, W);
        c->flags = (c->flags & ~F_CF) | cf;           /* INC/DEC preserve CF */
        srw(c, reg, W, res);
        c->ip = (uint16_t)(c->ip + idx); return 1;
    }

    /* ---- group FE/FF: INC/DEC r/m, and (FF only) near indirect CALL/JMP +
            PUSH r/m. Far call/jmp (g=3/5) bail. ---------------------------- */
    if (op == 0xFE || op == 0xFF) {
        int w = (op == 0xFE) ? 1 : W;
        modrm_t m; idx += decode_modrm(c, cb, idx, segov, &m);
        if (m.is_mem && m.lin >= GUEST_HI) return 0;
        if (m.g == 0 || m.g == 1) {                   /* INC/DEC r/m */
            uint32_t cf = c->flags & F_CF;
            uint32_t a = m.is_mem ? rd_mem(m.lin, w) : grw(c, m.rm_reg, w);
            uint32_t res = (m.g == 1) ? do_sub(c, a, 1, 0, w) : do_add(c, a, 1, 0, w);
            c->flags = (c->flags & ~F_CF) | cf;
            if (m.is_mem) wr_mem(m.lin, w, res);
            else          srw(c, m.rm_reg, w, res);
            c->ip = (uint16_t)(c->ip + idx); return 1;
        }
        if (op != 0xFF || osz) return 0;               /* FE none; osz push/call/jmp: TODO */
        { uint16_t val = m.is_mem ? (uint16_t)rd_mem(m.lin, 2) : g16(c, m.rm_reg);
          uint16_t nip = (uint16_t)(c->ip + idx);
          if (m.g == 2) {                              /* CALL near indirect */
              uint16_t sp = (uint16_t)(c->r[4] - 2);
              wr_mem((seg_base(c->seg[2])) + sp, 2, nip);
              c->r[4] = sp; c->ip = val; return 1;
          }
          if (m.g == 4) { c->ip = val; return 1; }     /* JMP near indirect */
          if (m.g == 6) {                              /* PUSH r/m16 */
              uint16_t sp = (uint16_t)(c->r[4] - 2);
              wr_mem((seg_base(c->seg[2])) + sp, 2, val);
              c->r[4] = sp; c->ip = nip; return 1;
          } }
        return 0;                                      /* g=3/5/7: far/illegal -> bail */
    }

    /* ---- TEST r/m,r (84/85); TEST AL/AX,imm (A8/A9) ----------------------- */
    if (op == 0x84 || op == 0x85) {
        int w = (op == 0x84) ? 1 : W;
        modrm_t m; idx += decode_modrm(c, cb, idx, segov, &m);
        if (m.is_mem && m.lin >= GUEST_HI) return 0;
        { uint32_t e = m.is_mem ? rd_mem(m.lin, w) : grw(c, m.rm_reg, w);
          uint32_t g = grw(c, m.g, w);
          do_logic(c, e & g, w); }
        c->ip = (uint16_t)(c->ip + idx); return 1;
    }
    if (op == 0xA8) { do_logic(c, (uint32_t)g8(c, 0) & CB(idx), 1); idx++;
                      c->ip = (uint16_t)(c->ip + idx); return 1; }
    if (op == 0xA9) { uint32_t b = rd_mem(cb + idx, W); idx += W;
                      do_logic(c, grw(c, 0, W) & b, W);
                      c->ip = (uint16_t)(c->ip + idx); return 1; }
    /* ---- group3 (F6/F7): only TEST r/m,imm (reg=0/1) here; rest bail ------ */
    if (op == 0xF6 || op == 0xF7) {
        int w = (op == 0xF6) ? 1 : W;
        modrm_t m; idx += decode_modrm(c, cb, idx, segov, &m);
        if (m.g != 0 && m.g != 1) return 0;           /* NOT/NEG/MUL/DIV: bail */
        if (m.is_mem && m.lin >= GUEST_HI) return 0;
        { uint32_t e = m.is_mem ? rd_mem(m.lin, w) : grw(c, m.rm_reg, w);
          uint32_t b = rd_mem(cb + idx, w); idx += w;
          do_logic(c, e & b, w); }
        c->ip = (uint16_t)(c->ip + idx); return 1;
    }

    /* ---- MOV r/m<->reg (88-8B); MOV r/m,imm (C6/C7) ----------------------- */
    if (op == 0x88 || op == 0x89 || op == 0x8A || op == 0x8B) {
        int w = (op & 1) ? W : 1, load = (op == 0x8A || op == 0x8B);
        modrm_t m; idx += decode_modrm(c, cb, idx, segov, &m);
        if (m.is_mem && m.lin >= GUEST_HI) return 0;
        if (load) { uint32_t v = m.is_mem ? rd_mem(m.lin, w) : grw(c, m.rm_reg, w);
                    srw(c, m.g, w, v); }
        else { uint32_t v = grw(c, m.g, w);
               if (m.is_mem) wr_mem(m.lin, w, v);
               else          srw(c, m.rm_reg, w, v); }
        c->ip = (uint16_t)(c->ip + idx); return 1;
    }
    if (op == 0xC6 || op == 0xC7) {
        int w = (op == 0xC7) ? W : 1;
        modrm_t m; idx += decode_modrm(c, cb, idx, segov, &m);
        if (m.g != 0) return 0;
        if (m.is_mem && m.lin >= GUEST_HI) return 0;
        { uint32_t v = rd_mem(cb + idx, w); idx += w;
          if (m.is_mem) wr_mem(m.lin, w, v);
          else          srw(c, m.rm_reg, w, v); }
        c->ip = (uint16_t)(c->ip + idx); return 1;
    }

    /* ---- XCHG r/m,r (86/87): swap; an A0000 read loads latches; no flags --- *
     * QuickBASIC plots mode-12h pixels with `XCHG ES:[DI],AL` (read-modify the *
     * VGA latches + write in one op), so this is the hot pixel-store path.     */
    if (op == 0x86 || op == 0x87) {
        int w = (op == 0x87) ? W : 1;
        modrm_t m; idx += decode_modrm(c, cb, idx, segov, &m);
        if (m.is_mem && m.lin >= GUEST_HI) return 0;
        { uint32_t rv = grw(c, m.g, w);
          uint32_t ev = m.is_mem ? rd_mem(m.lin, w) : grw(c, m.rm_reg, w);
          if (m.is_mem) wr_mem(m.lin, w, rv);
          else          srw(c, m.rm_reg, w, rv);
          srw(c, m.g, w, ev); }
        c->ip = (uint16_t)(c->ip + idx); return 1;
    }

    /* ---- PUSH/POP r16/r32 (50-5F): SS:SP-relative, via imem (W-wide slot) --- */
    if (op >= 0x50 && op <= 0x57) {
        uint16_t sp = (uint16_t)(c->r[4] - W);
        wr_mem((seg_base(c->seg[2])) + sp, W, grw(c, op & 7, W));
        c->r[4] = (c->r[4] & 0xFFFF0000u) | sp; c->ip = (uint16_t)(c->ip + idx); return 1;
    }
    if (op >= 0x58 && op <= 0x5F) {
        uint16_t sp = (uint16_t)c->r[4];
        srw(c, op & 7, W, rd_mem((seg_base(c->seg[2])) + sp, W));
        c->r[4] = (c->r[4] & 0xFFFF0000u) | (uint16_t)(sp + W); c->ip = (uint16_t)(c->ip + idx); return 1;
    }

    /* ---- PUSH imm (68 = imm16/imm32, 6A = imm8 sign-extended to W): a C runtime *
     * pushes call args and far-jump targets this way (run 55's I310102 stopped on *
     * `68 3a 02` = PUSH 0x023A). Slot + immediate are W-wide (0x66 -> 32-bit).     *
     * run 56. */
    if (op == 0x68 || op == 0x6A) {
        uint16_t sp = (uint16_t)(c->r[4] - W);
        uint32_t v;
        if (op == 0x68) { v = rd_mem(cb + idx, W); idx += W; }     /* imm is W bytes */
        else { v = (uint32_t)(int32_t)(int8_t)CB(idx++); v &= wmask(W); }  /* sign-ext imm8 */
        wr_mem((seg_base(c->seg[2])) + sp, W, v);
        c->r[4] = (c->r[4] & 0xFFFF0000u) | sp; c->ip = (uint16_t)(c->ip + idx); return 1;
    }

    /* ---- PUSHF/POPF (9C/9D): the FLAGS stack pair. A C runtime saves/restores  *
     * FLAGS around a code sequence (run 58's I310102 stopped on `9c ...` heading  *
     * a PUSHF; PUSH EDX; ... register-save). We push only the flags we model      *
     * (arithmetic + DF) plus the always-set reserved bit 1; POPF keeps the same   *
     * mask so the round-trip is exact (IF/TF/IOPL/NT are not modeled -> dropped,  *
     * so c->flags never accumulates junk). 16-bit only; the 0x66 PUSHFD/POPFD     *
     * 32-bit-EFLAGS form bails as TODO, matching the neighbouring stack ops.      *
     * run 59. */
    if (op == 0x9C) {                                  /* PUSHF */
        uint16_t sp;
        if (osz) return 0;                             /* PUSHFD (32-bit EFLAGS): TODO */
        sp = (uint16_t)(c->r[4] - 2);
        wr_mem((seg_base(c->seg[2])) + sp, 2, (uint16_t)((c->flags & 0x0CD5u) | 0x0002u));
        c->r[4] = (c->r[4] & 0xFFFF0000u) | sp; c->ip = (uint16_t)(c->ip + idx); return 1;
    }
    if (op == 0x9D) {                                  /* POPF */
        uint16_t sp;
        if (osz) return 0;                             /* POPFD (32-bit EFLAGS): TODO */
        sp = (uint16_t)c->r[4];
        c->flags = (rd_mem((seg_base(c->seg[2])) + sp, 2) & 0x0CD5u) | 0x0002u;
        c->r[4] = (c->r[4] & 0xFFFF0000u) | (uint16_t)(sp + 2); c->ip = (uint16_t)(c->ip + idx); return 1;
    }

    /* ---- PUSH/POP segment regs (06/0E/16/1E push ES/CS/SS/DS, 07/17/1F pop  *
     * ES/SS/DS). QB saves/restores ES (and DS) around each pixel -- this was  *
     * the per-pixel bail that capped batching at ~1 pixel. POP CS (0F) is not *
     * modeled (it would change the code segment mid-interpret). ------------- */
    if (op == 0x06 || op == 0x0E || op == 0x16 || op == 0x1E) {        /* PUSH sreg */
        int sr = (op == 0x06) ? 0 : (op == 0x0E) ? 1 : (op == 0x16) ? 2 : 3;
        uint16_t sp;
        if (osz) return 0;                             /* 32-bit seg push: TODO */
        sp = (uint16_t)(c->r[4] - 2);
        wr_mem((seg_base(c->seg[2])) + sp, 2, c->seg[sr]);
        c->r[4] = (c->r[4] & 0xFFFF0000u) | sp; c->ip = (uint16_t)(c->ip + idx); return 1;
    }
    if (op == 0x07 || op == 0x17 || op == 0x1F) {                      /* POP sreg */
        int sr = (op == 0x07) ? 0 : (op == 0x17) ? 2 : 3;
        uint16_t sp;
        if (osz) return 0;                             /* 32-bit seg pop: TODO */
        sp = (uint16_t)c->r[4];
        c->seg[sr] = (uint16_t)rd_mem((seg_base(c->seg[2])) + sp, 2);
        c->r[4] = (c->r[4] & 0xFFFF0000u) | (uint16_t)(sp + 2); c->ip = (uint16_t)(c->ip + idx); return 1;
    }

    /* ---- MOV r/m16,Sreg (8C) / MOV Sreg,r/m16 (8E) ------------------------- */
    if (op == 0x8C || op == 0x8E) {
        modrm_t m; idx += decode_modrm(c, cb, idx, segov, &m);
        if (m.is_mem && m.lin >= GUEST_HI) return 0;
        if ((m.g & 7) > 5) return 0;
        if (op == 0x8C) {                              /* store Sreg -> r/m16 */
            uint16_t v = c->seg[m.g & 7];
            if (m.is_mem) wr_mem(m.lin, 2, v); else s16(c, m.rm_reg, v);
        } else {                                       /* load Sreg <- r/m16 */
            if ((m.g & 7) == 1) return 0;              /* MOV CS,x is illegal */
            c->seg[m.g & 7] = m.is_mem ? (uint16_t)rd_mem(m.lin, 2) : g16(c, m.rm_reg);
        }
        c->ip = (uint16_t)(c->ip + idx); return 1;
    }

    /* ---- LEA r16/r32, m (8D): load the effective-address offset (not memory) */
    if (op == 0x8D) {
        modrm_t m; idx += decode_modrm(c, cb, idx, segov, &m);
        if (!m.is_mem) return 0;                       /* LEA with reg operand is illegal */
        srw(c, m.g, W, m.ea);                          /* addr size is 16-bit -> ea zero-ext to W */
        c->ip = (uint16_t)(c->ip + idx); return 1;
    }

    /* ---- shift/rotate group-2: D0/D1 (by 1), D2/D3 (by CL), C0/C1 (imm8) --- */
    if (op == 0xD0 || op == 0xD1 || op == 0xD2 || op == 0xD3 || op == 0xC0 || op == 0xC1) {
        int w = (op & 1) ? W : 1, cnt;
        modrm_t m; idx += decode_modrm(c, cb, idx, segov, &m);
        if (m.is_mem && m.lin >= GUEST_HI) return 0;
        if (op == 0xD0 || op == 0xD1) cnt = 1;
        else if (op == 0xD2 || op == 0xD3) cnt = g8(c, 1);   /* CL */
        else cnt = CB(idx++);                                /* imm8 */
        { uint32_t v = m.is_mem ? rd_mem(m.lin, w) : grw(c, m.rm_reg, w);
          uint32_t res = do_shrot(c, m.g, v, cnt, w);
          if (m.is_mem) wr_mem(m.lin, w, res);
          else          srw(c, m.rm_reg, w, res); }
        c->ip = (uint16_t)(c->ip + idx); return 1;
    }

    /* ---- IN/OUT via the device bus (E4-E7, EC-EF) -------------------------- *
     * Lets the interpreter run VGA-register-per-pixel plot loops in-host (QB    *
     * reprograms the Graphics Controller bit mask via OUT between pixels).      */
    if (op == 0xE4 || op == 0xE5 || op == 0xEC || op == 0xED) {          /* IN  */
        int w = (op & 1) ? 2 : 1;
        uint16_t port = (op <= 0xE5) ? (uint16_t)CB(idx++) : c->r[2];    /* imm8/DX */
        uint32_t v = iio_in(port, w);
        if (w == 1) s8(c, 0, (uint8_t)v); else s16(c, 0, (uint16_t)v);
        c->ip = (uint16_t)(c->ip + idx); return 1;
    }
    if (op == 0xE6 || op == 0xE7 || op == 0xEE || op == 0xEF) {          /* OUT */
        int w = (op & 1) ? 2 : 1;
        uint16_t port = (op <= 0xE7) ? (uint16_t)CB(idx++) : c->r[2];    /* imm8/DX */
        iio_out(port, w, (w == 1) ? g8(c, 0) : g16(c, 0));
        c->ip = (uint16_t)(c->ip + idx); return 1;
    }

    /* ---- MOV r,imm (B0-BF); MOV AL/AX,moffs / moffs,AL/AX (A0-A3) --------- */
    if (op >= 0xB0 && op <= 0xB7) { s8(c, op & 7, CB(idx)); idx++;
                                    c->ip = (uint16_t)(c->ip + idx); return 1; }
    if (op >= 0xB8 && op <= 0xBF) { uint32_t v = rd_mem(cb + idx, W); idx += W;
                                    srw(c, op & 7, W, v); c->ip = (uint16_t)(c->ip + idx); return 1; }
    if (op >= 0xA0 && op <= 0xA3) {
        uint16_t off = (uint16_t)(CB(idx) | (CB(idx + 1) << 8)); idx += 2;
        uint32_t lin = (seg_base(c->seg[(segov >= 0) ? segov : 3])) + off;
        int w = (op & 1) ? W : 1;
        if (lin >= GUEST_HI) return 0;
        if (op <= 0xA1) srw(c, 0, w, rd_mem(lin, w));
        else            wr_mem(lin, w, grw(c, 0, w));
        c->ip = (uint16_t)(c->ip + idx); return 1;
    }

    /* ---- string ops: STOS (AA/AB), MOVS (A4/A5), LODS (AC/AD) -------------- */
    if (op == 0xAA || op == 0xAB) {                   /* STOS ES:DI <- AL/AX */
        int w = (op == 0xAB) ? 2 : 1, dir = (c->flags & F_DF) ? -w : w;
        if (osz) return 0;                            /* 32-bit string op: TODO */
        uint32_t cnt = rep ? c->r[1] : 1, es = c->seg[0], al = c->r[0]; uint16_t di = c->r[7];
        while (cnt) { uint32_t lin = (seg_base(es)) + di;
                      imem_w8(lin, (uint8_t)al);
                      if (w == 2) imem_w8((seg_base(es)) + (uint16_t)(di + 1), (uint8_t)(al >> 8));
                      di = (uint16_t)(di + dir); cnt--; }
        c->r[7] = di; if (rep) c->r[1] = (uint16_t)cnt;
        c->ip = (uint16_t)(c->ip + idx); return 1;
    }
    if (op == 0xA4 || op == 0xA5) {                   /* MOVS ES:DI <- DS:SI */
        int w = (op == 0xA5) ? 2 : 1, dir = (c->flags & F_DF) ? -w : w;
        if (osz) return 0;                            /* 32-bit string op: TODO */
        uint32_t cnt = rep ? c->r[1] : 1, ss = c->seg[(segov >= 0) ? segov : 3], es = c->seg[0];
        uint16_t si = c->r[6], di = c->r[7];
        while (cnt) { uint32_t sl = (seg_base(ss)) + si, dl = (seg_base(es)) + di;
                      imem_w8(dl, imem_r8(sl));
                      if (w == 2) imem_w8((seg_base(es)) + (uint16_t)(di + 1),
                                          imem_r8((seg_base(ss)) + (uint16_t)(si + 1)));
                      si = (uint16_t)(si + dir); di = (uint16_t)(di + dir); cnt--; }
        c->r[6] = si; c->r[7] = di; if (rep) c->r[1] = (uint16_t)cnt;
        c->ip = (uint16_t)(c->ip + idx); return 1;
    }
    if (op == 0xAC || op == 0xAD) {                   /* LODS AL/AX <- DS:SI */
        int w = (op == 0xAD) ? 2 : 1, dir = (c->flags & F_DF) ? -w : w;
        if (osz) return 0;                            /* 32-bit string op: TODO */
        uint32_t cnt = rep ? c->r[1] : 1, ss = c->seg[(segov >= 0) ? segov : 3]; uint16_t si = c->r[6];
        while (cnt) { uint32_t sl = (seg_base(ss)) + si, v = imem_r8(sl);
                      if (w == 2) v |= (uint32_t)imem_r8((seg_base(ss)) + (uint16_t)(si + 1)) << 8;
                      if (w == 1) s8(c, 0, (uint8_t)v); else s16(c, 0, (uint16_t)v);
                      si = (uint16_t)(si + dir); cnt--; }
        c->r[6] = si; if (rep) c->r[1] = (uint16_t)cnt;
        c->ip = (uint16_t)(c->ip + idx); return 1;
    }

    /* ---- control flow: Jcc (70-7F), JMP short (EB) / near (E9),
            CALL near (E8) + RET near (C3/C2), LOOP/LOOPE/LOOPNE/JCXZ (E0-E3) -- *
     * CALL/RET let the interpreter follow QuickBasic's per-pixel runtime call,  *
     * so a whole scanline batches in one fault instead of ~5 instr per pixel.   */
    if (op >= 0x70 && op <= 0x7F) {
        int8_t rel = (int8_t)CB(idx++); int take = icond(c, op & 0xF);
        c->ip = (uint16_t)(c->ip + idx + (take ? rel : 0)); return 1;
    }
    if (op == 0xEB) { int8_t rel = (int8_t)CB(idx++); c->ip = (uint16_t)(c->ip + idx + rel); return 1; }
    if (op == 0xE9) { int16_t rel; if (osz) return 0; rel = (int16_t)(CB(idx) | (CB(idx + 1) << 8)); idx += 2;
                      c->ip = (uint16_t)(c->ip + idx + rel); return 1; }
    if (op == 0xE8) {                                  /* CALL near relative */
        int16_t rel; uint16_t nip, sp;
        if (osz) return 0;                            /* 32-bit near call: TODO */
        rel = (int16_t)(CB(idx) | (CB(idx + 1) << 8)); idx += 2;
        nip = (uint16_t)(c->ip + idx);                 /* return address */
        sp = (uint16_t)(c->r[4] - 2);
        wr_mem((seg_base(c->seg[2])) + sp, 2, nip);
        c->r[4] = (c->r[4] & 0xFFFF0000u) | sp; c->ip = (uint16_t)(nip + rel); return 1;
    }
    if (op == 0xC3 || op == 0xC2) {                    /* RET near (+ imm16 pop) */
        uint16_t sp, ret, extra;
        if (osz) return 0;                            /* 32-bit near ret: TODO */
        sp = (uint16_t)c->r[4];
        ret = (uint16_t)rd_mem((seg_base(c->seg[2])) + sp, 2);
        extra = (op == 0xC2) ? (uint16_t)(CB(idx) | (CB(idx + 1) << 8)) : 0;
        c->r[4] = (c->r[4] & 0xFFFF0000u) | (uint16_t)(sp + 2 + extra); c->ip = ret; return 1;
    }

    /* ---- RETF (CB) / RETF imm16 (CA): far return -- pop offset then a 2-byte  *
     * SELECTOR into CS. In PM `seg_base` resolves that selector via the LDT (the *
     * same machinery LAR/LSL use), so the client's `PUSH seg; PUSH off; RETF`    *
     * far-transfer idiom (run 56's wall) just follows through. run 57. */
    if (op == 0xCB || op == 0xCA) {
        uint16_t sp, off, sel, extra;
        if (osz) return 0;                            /* 32-bit far ret: TODO */
        sp  = (uint16_t)c->r[4];
        off = (uint16_t)rd_mem((seg_base(c->seg[2])) + sp, 2);
        sel = (uint16_t)rd_mem((seg_base(c->seg[2])) + (uint16_t)(sp + 2), 2);
        extra = (op == 0xCA) ? (uint16_t)(CB(idx) | (CB(idx + 1) << 8)) : 0;
        c->r[4] = (c->r[4] & 0xFFFF0000u) | (uint16_t)(sp + 4 + extra);
        c->seg[1] = sel; c->ip = off; return 1;
    }

    /* ---- LEAVE (C9): frame teardown -- MOV SP,BP; POP BP. Paired with the C  *
     * runtime's function-prologue ENTER/`PUSH BP; MOV BP,SP`, so it lands on    *
     * every callee return (run 57's far RET reaches main(), whose epilogue is   *
     * this). SP first snaps to BP (discarding locals), then the caller's BP is  *
     * popped. run 58. */
    if (op == 0xC9) {
        uint16_t sp, bp;
        if (osz) return 0;                            /* 32-bit LEAVE (ESP/EBP): TODO */
        sp = (uint16_t)c->r[5];                        /* SP <- BP */
        bp = (uint16_t)rd_mem((seg_base(c->seg[2])) + sp, 2);
        c->r[5] = (c->r[5] & 0xFFFF0000u) | bp;        /* BP <- pop */
        c->r[4] = (c->r[4] & 0xFFFF0000u) | (uint16_t)(sp + 2);
        c->ip = (uint16_t)(c->ip + idx); return 1;
    }
    if (op >= 0xE0 && op <= 0xE3) {
        int8_t rel = (int8_t)CB(idx++); int take;
        if (op == 0xE3) take = (c->r[1] == 0);        /* JCXZ */
        else { c->r[1] = (uint16_t)(c->r[1] - 1);
               int cx = (c->r[1] != 0);
               take = (op == 0xE2) ? cx                                   /* LOOP   */
                    : (op == 0xE1) ? (cx && (c->flags & F_ZF))            /* LOOPE  */
                                   : (cx && !(c->flags & F_ZF)); }        /* LOOPNE */
        c->ip = (uint16_t)(c->ip + idx + (take ? rel : 0)); return 1;
    }

    /* ---- XCHG AX,r16 (91-97): the accumulator short-form -- swap AX with the  *
     * indexed reg (0x90 = XCHG AX,AX = NOP, handled just below). No flags. A C   *
     * runtime uses it as cheap register glue (run 59's I310102 stopped on `96` = *
     * XCHG AX,SI). W-wide (0x66 -> XCHG EAX,r32). run 60. */
    if (op >= 0x91 && op <= 0x97) {
        int reg = op & 7;
        uint32_t a = grw(c, 0, W), b = grw(c, reg, W);
        srw(c, 0, W, b); srw(c, reg, W, a);
        c->ip = (uint16_t)(c->ip + idx); return 1;
    }

    /* ---- flag ops + NOP --------------------------------------------------- */
    if (op == 0x90) { c->ip = (uint16_t)(c->ip + idx); return 1; }              /* NOP */
    if (op == 0xF8) { c->flags &= ~F_CF; c->ip = (uint16_t)(c->ip + idx); return 1; }  /* CLC */
    if (op == 0xF9) { c->flags |=  F_CF; c->ip = (uint16_t)(c->ip + idx); return 1; }  /* STC */
    if (op == 0xF5) { c->flags ^=  F_CF; c->ip = (uint16_t)(c->ip + idx); return 1; }  /* CMC */
    if (op == 0xFC) { c->flags &= ~F_DF; c->ip = (uint16_t)(c->ip + idx); return 1; }  /* CLD */
    if (op == 0xFD) { c->flags |=  F_DF; c->ip = (uint16_t)(c->ip + idx); return 1; }  /* STD */

    return 0;                                          /* unmodeled: bail to V86 */
}

#endif /* V86INTERP_H */
