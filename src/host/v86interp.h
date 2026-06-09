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
    uint16_t r[8];      /* AX CX DX BX SP BP SI DI (x86 reg encoding)   */
    uint16_t seg[6];    /* ES CS SS DS FS GS       (x86 sreg encoding)  */
    uint16_t ip;
    uint32_t flags;
} icpu;

static int iparity(uint8_t v) { v ^= v >> 4; v ^= v >> 2; v ^= v >> 1; return !(v & 1); }

static uint32_t rd_mem(uint32_t lin, int w)
{ return (w == 1) ? imem_r8(lin) : (uint32_t)(imem_r8(lin) | (imem_r8(lin + 1) << 8)); }
static void wr_mem(uint32_t lin, int w, uint32_t v)
{ imem_w8(lin, (uint8_t)v); if (w == 2) imem_w8(lin + 1, (uint8_t)(v >> 8)); }

/* CPU register file access by x86 encoding. */
static uint16_t g16(icpu *c, int e) { return c->r[e & 7]; }
static void     s16(icpu *c, int e, uint16_t v) { c->r[e & 7] = v; }
static uint8_t  g8(icpu *c, int e)
{ return (e < 4) ? (uint8_t)c->r[e] : (uint8_t)(c->r[e - 4] >> 8); }
static void     s8(icpu *c, int e, uint8_t v)
{ if (e < 4) c->r[e] = (uint16_t)((c->r[e] & 0xFF00) | v);
  else c->r[e - 4] = (uint16_t)((c->r[e - 4] & 0x00FF) | ((uint16_t)v << 8)); }

/* Flag-computing ALU primitives (result masked to operand width w). */
static uint32_t do_add(icpu *c, uint32_t a, uint32_t b, int cin, int w)
{
    uint32_t m = (w == 1) ? 0xFFu : 0xFFFFu, sb = (w == 1) ? 0x80u : 0x8000u;
    uint32_t fa = a & m, fb = b & m, full = fa + fb + (uint32_t)cin, res = full & m;
    c->flags &= ~(F_CF | F_PF | F_AF | F_ZF | F_SF | F_OF);
    if (full & (m + 1))                         c->flags |= F_CF;
    if ((fa ^ fb ^ res) & 0x10u)                c->flags |= F_AF;
    if (!res)                                   c->flags |= F_ZF;
    if (res & sb)                               c->flags |= F_SF;
    if (iparity((uint8_t)res))                  c->flags |= F_PF;
    if ((~(fa ^ fb) & (fa ^ res)) & sb)         c->flags |= F_OF;
    return res;
}
static uint32_t do_sub(icpu *c, uint32_t a, uint32_t b, int cin, int w)
{
    uint32_t m = (w == 1) ? 0xFFu : 0xFFFFu, sb = (w == 1) ? 0x80u : 0x8000u;
    uint32_t fa = a & m, fb = b & m, res = (fa - fb - (uint32_t)cin) & m;
    c->flags &= ~(F_CF | F_PF | F_AF | F_ZF | F_SF | F_OF);
    if (fa < fb + (uint32_t)cin)                c->flags |= F_CF;
    if ((fa ^ fb ^ res) & 0x10u)                c->flags |= F_AF;
    if (!res)                                   c->flags |= F_ZF;
    if (res & sb)                               c->flags |= F_SF;
    if (iparity((uint8_t)res))                  c->flags |= F_PF;
    if (((fa ^ fb) & (fa ^ res)) & sb)          c->flags |= F_OF;
    return res;
}
static void do_logic(icpu *c, uint32_t res, int w)
{
    uint32_t m = (w == 1) ? 0xFFu : 0xFFFFu, sb = (w == 1) ? 0x80u : 0x8000u;
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
typedef struct { int is_mem; uint32_t lin; int g; int rm_reg; } modrm_t;
static int decode_modrm(icpu *c, uint32_t cb, int idx, int segov, modrm_t *o)
{
    BYTE mr = CB(idx); int mod = mr >> 6, rm = mr & 7, len = 1, bp = 0;
    uint16_t ea = 0, BX = c->r[3], BP = c->r[5], SI = c->r[6], DI = c->r[7];
    o->g = (mr >> 3) & 7;
    if (mod == 3) { o->is_mem = 0; o->rm_reg = rm; return 1; }
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
    o->is_mem = 1;
    o->lin = ((uint32_t)c->seg[(segov >= 0) ? segov : (bp ? 2 : 3)] << 4) + ea;  /* SS if BP else DS */
    return len;
}

/* Execute one instruction. Returns 1 if modeled (state + IP advanced/jumped),
   0 to bail (state untouched at the current instruction). */
static int istep(icpu *c)
{
    uint32_t cb = ((uint32_t)c->seg[1] << 4) + c->ip;   /* linear CS:IP */
    int idx = 0, segov = -1, rep = 0;
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
        else if (b == 0x66 || b == 0x67 || b == 0xF0) return 0;   /* 32-bit/LOCK: bail */
        else break;
        if (idx > 4) return 0;
    }
    op = CB(idx++);

    /* ---- arithmetic/logic group: ADD..CMP, reg/mem forms ------------------ */
    if (op < 0x40 && (op & 7) < 6) {
        int aluop = (op >> 3) & 7, form = op & 7;
        int w = (form == 0 || form == 2 || form == 4) ? 1 : 2;
        uint32_t a, b, res; int dmem = 0, dreg = 0; uint32_t dlin = 0;
        if (form <= 3) {
            modrm_t m; idx += decode_modrm(c, cb, idx, segov, &m);
            if (m.is_mem && m.lin >= GUEST_HI) return 0;
            uint32_t ev = m.is_mem ? rd_mem(m.lin, w) : (w == 1 ? g8(c, m.rm_reg) : g16(c, m.rm_reg));
            uint32_t gv = (w == 1) ? g8(c, m.g) : g16(c, m.g);
            if (form <= 1) { a = ev; b = gv; if (m.is_mem) { dmem = 1; dlin = m.lin; } else dreg = m.rm_reg; }
            else           { a = gv; b = ev; dreg = m.g; }
        } else if (form == 4) { a = g8(c, 0);  b = CB(idx++); dreg = 0; }
        else { a = g16(c, 0); b = (uint32_t)(CB(idx) | (CB(idx + 1) << 8)); idx += 2; dreg = 0; }
        res = do_alu(c, aluop, a, b, w);
        if (aluop != 7) {
            if (dmem)         wr_mem(dlin, w, res);
            else if (w == 1)  s8(c, dreg, (uint8_t)res);
            else              s16(c, dreg, (uint16_t)res);
        }
        c->ip = (uint16_t)(c->ip + idx); return 1;
    }

    /* ---- group1: ADD..CMP r/m, imm (80/81/83) ----------------------------- */
    if (op == 0x80 || op == 0x81 || op == 0x83) {
        int w = (op == 0x80) ? 1 : 2; uint32_t a, b, res;
        modrm_t m; idx += decode_modrm(c, cb, idx, segov, &m);
        if (m.is_mem && m.lin >= GUEST_HI) return 0;
        a = m.is_mem ? rd_mem(m.lin, w) : (w == 1 ? g8(c, m.rm_reg) : g16(c, m.rm_reg));
        if (op == 0x81) { b = (uint32_t)(CB(idx) | (CB(idx + 1) << 8)); idx += 2; }
        else { b = (uint32_t)(int32_t)(int8_t)CB(idx++); b &= (w == 1) ? 0xFFu : 0xFFFFu; }
        res = do_alu(c, m.g, a, b, w);
        if (m.g != 7) {
            if (m.is_mem)    wr_mem(m.lin, w, res);
            else if (w == 1) s8(c, m.rm_reg, (uint8_t)res);
            else             s16(c, m.rm_reg, (uint16_t)res);
        }
        c->ip = (uint16_t)(c->ip + idx); return 1;
    }

    /* ---- INC/DEC r16 (40-4F) ---------------------------------------------- */
    if (op >= 0x40 && op <= 0x4F) {
        int reg = op & 7; uint32_t cf = c->flags & F_CF;
        uint16_t res = (op >= 0x48) ? (uint16_t)do_sub(c, g16(c, reg), 1, 0, 2)
                                    : (uint16_t)do_add(c, g16(c, reg), 1, 0, 2);
        c->flags = (c->flags & ~F_CF) | cf;           /* INC/DEC preserve CF */
        s16(c, reg, res);
        c->ip = (uint16_t)(c->ip + idx); return 1;
    }

    /* ---- group FE/FF: INC/DEC r/m, and (FF only) near indirect CALL/JMP +
            PUSH r/m. Far call/jmp (g=3/5) bail. ---------------------------- */
    if (op == 0xFE || op == 0xFF) {
        int w = (op == 0xFE) ? 1 : 2;
        modrm_t m; idx += decode_modrm(c, cb, idx, segov, &m);
        if (m.is_mem && m.lin >= GUEST_HI) return 0;
        if (m.g == 0 || m.g == 1) {                   /* INC/DEC r/m */
            uint32_t cf = c->flags & F_CF;
            uint32_t a = m.is_mem ? rd_mem(m.lin, w) : (w == 1 ? g8(c, m.rm_reg) : g16(c, m.rm_reg));
            uint32_t res = (m.g == 1) ? do_sub(c, a, 1, 0, w) : do_add(c, a, 1, 0, w);
            c->flags = (c->flags & ~F_CF) | cf;
            if (m.is_mem)    wr_mem(m.lin, w, res);
            else if (w == 1) s8(c, m.rm_reg, (uint8_t)res);
            else             s16(c, m.rm_reg, (uint16_t)res);
            c->ip = (uint16_t)(c->ip + idx); return 1;
        }
        if (op != 0xFF) return 0;                      /* FE has no other forms */
        { uint16_t val = m.is_mem ? (uint16_t)rd_mem(m.lin, 2) : g16(c, m.rm_reg);
          uint16_t nip = (uint16_t)(c->ip + idx);
          if (m.g == 2) {                              /* CALL near indirect */
              uint16_t sp = (uint16_t)(c->r[4] - 2);
              wr_mem(((uint32_t)c->seg[2] << 4) + sp, 2, nip);
              c->r[4] = sp; c->ip = val; return 1;
          }
          if (m.g == 4) { c->ip = val; return 1; }     /* JMP near indirect */
          if (m.g == 6) {                              /* PUSH r/m16 */
              uint16_t sp = (uint16_t)(c->r[4] - 2);
              wr_mem(((uint32_t)c->seg[2] << 4) + sp, 2, val);
              c->r[4] = sp; c->ip = nip; return 1;
          } }
        return 0;                                      /* g=3/5/7: far/illegal -> bail */
    }

    /* ---- TEST r/m,r (84/85); TEST AL/AX,imm (A8/A9) ----------------------- */
    if (op == 0x84 || op == 0x85) {
        int w = (op == 0x84) ? 1 : 2;
        modrm_t m; idx += decode_modrm(c, cb, idx, segov, &m);
        if (m.is_mem && m.lin >= GUEST_HI) return 0;
        { uint32_t e = m.is_mem ? rd_mem(m.lin, w) : (w == 1 ? g8(c, m.rm_reg) : g16(c, m.rm_reg));
          uint32_t g = (w == 1) ? g8(c, m.g) : g16(c, m.g);
          do_logic(c, e & g, w); }
        c->ip = (uint16_t)(c->ip + idx); return 1;
    }
    if (op == 0xA8) { do_logic(c, (uint32_t)g8(c, 0) & CB(idx), 1); idx++;
                      c->ip = (uint16_t)(c->ip + idx); return 1; }
    if (op == 0xA9) { uint32_t b = (uint32_t)(CB(idx) | (CB(idx + 1) << 8)); idx += 2;
                      do_logic(c, (uint32_t)g16(c, 0) & b, 2);
                      c->ip = (uint16_t)(c->ip + idx); return 1; }

    /* ---- group3 (F6/F7): only TEST r/m,imm (reg=0/1) here; rest bail ------ */
    if (op == 0xF6 || op == 0xF7) {
        int w = (op == 0xF6) ? 1 : 2;
        modrm_t m; idx += decode_modrm(c, cb, idx, segov, &m);
        if (m.g != 0 && m.g != 1) return 0;           /* NOT/NEG/MUL/DIV: bail */
        if (m.is_mem && m.lin >= GUEST_HI) return 0;
        { uint32_t e = m.is_mem ? rd_mem(m.lin, w) : (w == 1 ? g8(c, m.rm_reg) : g16(c, m.rm_reg));
          uint32_t b; if (w == 1) { b = CB(idx++); } else { b = (uint32_t)(CB(idx) | (CB(idx + 1) << 8)); idx += 2; }
          do_logic(c, e & b, w); }
        c->ip = (uint16_t)(c->ip + idx); return 1;
    }

    /* ---- MOV r/m<->reg (88-8B); MOV r/m,imm (C6/C7) ----------------------- */
    if (op == 0x88 || op == 0x89 || op == 0x8A || op == 0x8B) {
        int w = (op & 1) ? 2 : 1, load = (op == 0x8A || op == 0x8B);
        modrm_t m; idx += decode_modrm(c, cb, idx, segov, &m);
        if (m.is_mem && m.lin >= GUEST_HI) return 0;
        if (load) { uint32_t v = m.is_mem ? rd_mem(m.lin, w) : (w == 1 ? g8(c, m.rm_reg) : g16(c, m.rm_reg));
                    if (w == 1) s8(c, m.g, (uint8_t)v); else s16(c, m.g, (uint16_t)v); }
        else { uint32_t v = (w == 1) ? g8(c, m.g) : g16(c, m.g);
               if (m.is_mem) wr_mem(m.lin, w, v);
               else if (w == 1) s8(c, m.rm_reg, (uint8_t)v); else s16(c, m.rm_reg, (uint16_t)v); }
        c->ip = (uint16_t)(c->ip + idx); return 1;
    }
    if (op == 0xC6 || op == 0xC7) {
        int w = (op == 0xC7) ? 2 : 1;
        modrm_t m; idx += decode_modrm(c, cb, idx, segov, &m);
        if (m.g != 0) return 0;
        if (m.is_mem && m.lin >= GUEST_HI) return 0;
        { uint32_t v; if (w == 1) v = CB(idx++); else { v = (uint32_t)(CB(idx) | (CB(idx + 1) << 8)); idx += 2; }
          if (m.is_mem) wr_mem(m.lin, w, v);
          else if (w == 1) s8(c, m.rm_reg, (uint8_t)v); else s16(c, m.rm_reg, (uint16_t)v); }
        c->ip = (uint16_t)(c->ip + idx); return 1;
    }

    /* ---- XCHG r/m,r (86/87): swap; an A0000 read loads latches; no flags --- *
     * QuickBASIC plots mode-12h pixels with `XCHG ES:[DI],AL` (read-modify the *
     * VGA latches + write in one op), so this is the hot pixel-store path.     */
    if (op == 0x86 || op == 0x87) {
        int w = (op == 0x87) ? 2 : 1;
        modrm_t m; idx += decode_modrm(c, cb, idx, segov, &m);
        if (m.is_mem && m.lin >= GUEST_HI) return 0;
        { uint32_t rv = (w == 1) ? g8(c, m.g) : g16(c, m.g);
          uint32_t ev = m.is_mem ? rd_mem(m.lin, w) : (w == 1 ? g8(c, m.rm_reg) : g16(c, m.rm_reg));
          if (m.is_mem) wr_mem(m.lin, w, rv);
          else if (w == 1) s8(c, m.rm_reg, (uint8_t)rv); else s16(c, m.rm_reg, (uint16_t)rv);
          if (w == 1) s8(c, m.g, (uint8_t)ev); else s16(c, m.g, (uint16_t)ev); }
        c->ip = (uint16_t)(c->ip + idx); return 1;
    }

    /* ---- PUSH/POP r16 (50-5F): SS:SP-relative, via imem -------------------- */
    if (op >= 0x50 && op <= 0x57) {
        uint16_t sp = (uint16_t)(c->r[4] - 2);
        wr_mem(((uint32_t)c->seg[2] << 4) + sp, 2, g16(c, op & 7));
        c->r[4] = sp; c->ip = (uint16_t)(c->ip + idx); return 1;
    }
    if (op >= 0x58 && op <= 0x5F) {
        uint16_t sp = c->r[4];
        s16(c, op & 7, (uint16_t)rd_mem(((uint32_t)c->seg[2] << 4) + sp, 2));
        c->r[4] = (uint16_t)(sp + 2); c->ip = (uint16_t)(c->ip + idx); return 1;
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
    if (op >= 0xB8 && op <= 0xBF) { uint32_t v = (uint32_t)(CB(idx) | (CB(idx + 1) << 8)); idx += 2;
                                    s16(c, op & 7, (uint16_t)v); c->ip = (uint16_t)(c->ip + idx); return 1; }
    if (op >= 0xA0 && op <= 0xA3) {
        uint16_t off = (uint16_t)(CB(idx) | (CB(idx + 1) << 8)); idx += 2;
        uint32_t lin = ((uint32_t)c->seg[(segov >= 0) ? segov : 3] << 4) + off;
        int w = (op & 1) ? 2 : 1;
        if (lin >= GUEST_HI) return 0;
        if (op <= 0xA1) { uint32_t v = rd_mem(lin, w); if (w == 1) s8(c, 0, (uint8_t)v); else s16(c, 0, (uint16_t)v); }
        else            { uint32_t v = (w == 1) ? g8(c, 0) : g16(c, 0); wr_mem(lin, w, v); }
        c->ip = (uint16_t)(c->ip + idx); return 1;
    }

    /* ---- string ops: STOS (AA/AB), MOVS (A4/A5), LODS (AC/AD) -------------- */
    if (op == 0xAA || op == 0xAB) {                   /* STOS ES:DI <- AL/AX */
        int w = (op == 0xAB) ? 2 : 1, dir = (c->flags & F_DF) ? -w : w;
        uint32_t cnt = rep ? c->r[1] : 1, es = c->seg[0], al = c->r[0]; uint16_t di = c->r[7];
        while (cnt) { uint32_t lin = ((uint32_t)es << 4) + di;
                      imem_w8(lin, (uint8_t)al);
                      if (w == 2) imem_w8(((uint32_t)es << 4) + (uint16_t)(di + 1), (uint8_t)(al >> 8));
                      di = (uint16_t)(di + dir); cnt--; }
        c->r[7] = di; if (rep) c->r[1] = (uint16_t)cnt;
        c->ip = (uint16_t)(c->ip + idx); return 1;
    }
    if (op == 0xA4 || op == 0xA5) {                   /* MOVS ES:DI <- DS:SI */
        int w = (op == 0xA5) ? 2 : 1, dir = (c->flags & F_DF) ? -w : w;
        uint32_t cnt = rep ? c->r[1] : 1, ss = c->seg[(segov >= 0) ? segov : 3], es = c->seg[0];
        uint16_t si = c->r[6], di = c->r[7];
        while (cnt) { uint32_t sl = ((uint32_t)ss << 4) + si, dl = ((uint32_t)es << 4) + di;
                      imem_w8(dl, imem_r8(sl));
                      if (w == 2) imem_w8(((uint32_t)es << 4) + (uint16_t)(di + 1),
                                          imem_r8(((uint32_t)ss << 4) + (uint16_t)(si + 1)));
                      si = (uint16_t)(si + dir); di = (uint16_t)(di + dir); cnt--; }
        c->r[6] = si; c->r[7] = di; if (rep) c->r[1] = (uint16_t)cnt;
        c->ip = (uint16_t)(c->ip + idx); return 1;
    }
    if (op == 0xAC || op == 0xAD) {                   /* LODS AL/AX <- DS:SI */
        int w = (op == 0xAD) ? 2 : 1, dir = (c->flags & F_DF) ? -w : w;
        uint32_t cnt = rep ? c->r[1] : 1, ss = c->seg[(segov >= 0) ? segov : 3]; uint16_t si = c->r[6];
        while (cnt) { uint32_t sl = ((uint32_t)ss << 4) + si, v = imem_r8(sl);
                      if (w == 2) v |= (uint32_t)imem_r8(((uint32_t)ss << 4) + (uint16_t)(si + 1)) << 8;
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
    if (op == 0xE9) { int16_t rel = (int16_t)(CB(idx) | (CB(idx + 1) << 8)); idx += 2;
                      c->ip = (uint16_t)(c->ip + idx + rel); return 1; }
    if (op == 0xE8) {                                  /* CALL near relative */
        int16_t rel = (int16_t)(CB(idx) | (CB(idx + 1) << 8)); idx += 2;
        uint16_t nip = (uint16_t)(c->ip + idx);        /* return address */
        uint16_t sp = (uint16_t)(c->r[4] - 2);
        wr_mem(((uint32_t)c->seg[2] << 4) + sp, 2, nip);
        c->r[4] = sp; c->ip = (uint16_t)(nip + rel); return 1;
    }
    if (op == 0xC3 || op == 0xC2) {                    /* RET near (+ imm16 pop) */
        uint16_t sp = c->r[4];
        uint16_t ret = (uint16_t)rd_mem(((uint32_t)c->seg[2] << 4) + sp, 2);
        uint16_t extra = (op == 0xC2) ? (uint16_t)(CB(idx) | (CB(idx + 1) << 8)) : 0;
        c->r[4] = (uint16_t)(sp + 2 + extra); c->ip = ret; return 1;
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
