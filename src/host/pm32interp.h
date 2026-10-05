/* pm32interp.h -- a small, flags-accurate interpreter for FLAT 32-bit protected-mode code.
 *
 * WHY IT EXISTS (s80, north star 1, design C for Doom). Mode Y's multi-plane map masks
 * cannot be served by any mapping of A0000 -- one virtual page cannot store into two
 * planes -- so while one is live the guest's stores must go through the VGA address
 * generator. v86interp.h does that for real-mode guests (Wolf3D, Mario). Doom's renderer
 * is 32-bit flat protected-mode code, and v86interp is 16-bit to its core (16-bit IP and
 * address size). This is the 32-bit counterpart: the host runs Doom's column and span
 * drawers here, from the trapped OUT that sets the mask until the drawer returns.
 *
 * SCOPE. Flat 32-bit code: 32-bit address size (0x67 declines), 32-bit default operand
 * size with the 0x66 prefix for 16-bit, 32-bit stack. The integer instruction set a C
 * compiler (Watcom) and id's drawer assembly emit. Everything else -- FPU, far control
 * transfers, segment loads, INT/IRET, CLI/STI, privileged instructions -- DECLINES.
 *
 * DECLINE IS EXACT. p32_step() either executes one whole instruction and returns 1, or
 * returns 0 having changed NOTHING (registers, flags, EIP, memory), so the caller can hand
 * the instruction to the real CPU. Memory writes are therefore deferred until the
 * instruction is known to complete (the few instructions that write memory write it last).
 *
 * The includer MUST provide, before #include'ing this header:
 *   - fixed-width int types (uint8_t .. uint64_t, int8_t .. int64_t)
 *   - uint8_t  p32_rd8(uint32_t lin);            guest byte read  (A0000 -> the VGA engine)
 *   - void     p32_wr8(uint32_t lin, uint8_t v); guest byte write (A0000 -> the VGA engine)
 *   - int      p32_ok(uint32_t lin, int w, int wr);  may this access proceed? (0 = decline)
 *   - uint32_t p32_in(uint16_t port, int w);
 *   - void     p32_out(uint16_t port, int w, uint32_t v);
 * Kept host-agnostic so it is unit-tested off-VM (tests/unit/pm32interp_test.c).
 */
#ifndef PM32INTERP_H
#define PM32INTERP_H

#define P32_CF 0x0001u
#define P32_PF 0x0004u
#define P32_AF 0x0010u
#define P32_ZF 0x0040u
#define P32_SF 0x0080u
#define P32_DF 0x0400u
#define P32_OF 0x0800u
#define P32_ARITH (P32_CF | P32_PF | P32_AF | P32_ZF | P32_SF | P32_OF)

typedef struct {
    uint32_t r[8];        /* EAX ECX EDX EBX ESP EBP ESI EDI                            */
    uint32_t eip;         /* offset in CS                                               */
    uint32_t flags;
    uint32_t base[6];     /* segment BASES, x86 sreg order: ES CS SS DS FS GS            */
} p32cpu;

/* ---- register views ------------------------------------------------------------- */
static uint32_t p32_msk(int w) { return w == 1 ? 0xFFu : w == 2 ? 0xFFFFu : 0xFFFFFFFFu; }
static uint32_t p32_sgn(int w) { return w == 1 ? 0x80u : w == 2 ? 0x8000u : 0x80000000u; }
static uint32_t p32_greg(const p32cpu *c, int e, int w)
{
    if (w == 1) return (e < 4) ? (c->r[e] & 0xFF) : ((c->r[e - 4] >> 8) & 0xFF);
    return c->r[e & 7] & p32_msk(w);
}
static void p32_sreg(p32cpu *c, int e, int w, uint32_t v)
{
    if (w == 1) {
        if (e < 4) c->r[e] = (c->r[e] & 0xFFFFFF00u) | (v & 0xFF);
        else       c->r[e - 4] = (c->r[e - 4] & 0xFFFF00FFu) | ((v & 0xFF) << 8);
    } else if (w == 2) c->r[e & 7] = (c->r[e & 7] & 0xFFFF0000u) | (v & 0xFFFF);
    else c->r[e & 7] = v;
}

/* ---- memory --------------------------------------------------------------------- */
static uint32_t p32_rdm(uint32_t lin, int w)
{
    uint32_t v = p32_rd8(lin);
    if (w >= 2) v |= (uint32_t)p32_rd8(lin + 1) << 8;
    if (w == 4) { v |= (uint32_t)p32_rd8(lin + 2) << 16; v |= (uint32_t)p32_rd8(lin + 3) << 24; }
    return v;
}
static void p32_wrm(uint32_t lin, int w, uint32_t v)
{
    p32_wr8(lin, (uint8_t)v);
    if (w >= 2) p32_wr8(lin + 1, (uint8_t)(v >> 8));
    if (w == 4) { p32_wr8(lin + 2, (uint8_t)(v >> 16)); p32_wr8(lin + 3, (uint8_t)(v >> 24)); }
}

/* ---- flags ---------------------------------------------------------------------- */
static int p32_par(uint32_t v) { v &= 0xFF; v ^= v >> 4; v ^= v >> 2; v ^= v >> 1; return !(v & 1); }
static uint32_t p32_szp(uint32_t r, int w)
{
    uint32_t f = 0;
    r &= p32_msk(w);
    if (!r) f |= P32_ZF;
    if (r & p32_sgn(w)) f |= P32_SF;
    if (p32_par(r)) f |= P32_PF;
    return f;
}
/* ADD/ADC and SUB/SBB/CMP: result + the six arithmetic flags. */
static uint32_t p32_add(uint32_t *fl, uint32_t a, uint32_t b, uint32_t ci, int w)
{
    uint32_t m = p32_msk(w), s = p32_sgn(w);
    uint64_t full = (uint64_t)(a & m) + (uint64_t)(b & m) + ci;
    uint32_t r = (uint32_t)full & m, f = p32_szp(r, w);
    if (full >> (w * 8)) f |= P32_CF;
    if (((a ^ b ^ r) & 0x10)) f |= P32_AF;
    if ((~(a ^ b) & (a ^ r)) & s) f |= P32_OF;
    *fl = (*fl & ~P32_ARITH) | f;
    return r;
}
static uint32_t p32_sub(uint32_t *fl, uint32_t a, uint32_t b, uint32_t bi, int w)
{
    uint32_t m = p32_msk(w), s = p32_sgn(w);
    uint32_t r = (a - b - bi) & m, f = p32_szp(r, w);
    if ((uint64_t)(a & m) < (uint64_t)(b & m) + bi) f |= P32_CF;
    if (((a ^ b ^ r) & 0x10)) f |= P32_AF;
    if (((a ^ b) & (a ^ r)) & s) f |= P32_OF;
    *fl = (*fl & ~P32_ARITH) | f;
    return r;
}
static uint32_t p32_logic(uint32_t *fl, uint32_t r, int w)
{   /* AND/OR/XOR/TEST: CF=OF=0, AF undefined (left clear, as hardware does in practice) */
    r &= p32_msk(w);
    *fl = (*fl & ~P32_ARITH) | p32_szp(r, w);
    return r;
}
/* group-1 op: 0 ADD 1 OR 2 ADC 3 SBB 4 AND 5 SUB 6 XOR 7 CMP. Returns the result; *store
   says whether it is written back (CMP is not). */
static uint32_t p32_alu(uint32_t *fl, int op, uint32_t a, uint32_t b, int w, int *store)
{
    uint32_t cf = *fl & P32_CF;
    *store = (op != 7);
    switch (op) {
    case 0: return p32_add(fl, a, b, 0, w);
    case 1: return p32_logic(fl, a | b, w);
    case 2: return p32_add(fl, a, b, cf, w);
    case 3: return p32_sub(fl, a, b, cf, w);
    case 4: return p32_logic(fl, a & b, w);
    case 5: return p32_sub(fl, a, b, 0, w);
    case 6: return p32_logic(fl, a ^ b, w);
    default: return p32_sub(fl, a, b, 0, w);
    }
}
static int p32_cond(uint32_t f, int cc)
{
    int r;
    switch (cc >> 1) {
    case 0: r = (f & P32_OF) != 0; break;                              /* O  */
    case 1: r = (f & P32_CF) != 0; break;                              /* B  */
    case 2: r = (f & P32_ZF) != 0; break;                              /* Z  */
    case 3: r = (f & (P32_CF | P32_ZF)) != 0; break;                   /* BE */
    case 4: r = (f & P32_SF) != 0; break;                              /* S  */
    case 5: r = (f & P32_PF) != 0; break;                              /* P  */
    case 6: r = ((f & P32_SF) != 0) != ((f & P32_OF) != 0); break;     /* L  */
    default: r = (f & P32_ZF) || (((f & P32_SF) != 0) != ((f & P32_OF) != 0)); /* LE */
    }
    return (cc & 1) ? !r : r;
}

/* ---- decode ---------------------------------------------------------------------- */
typedef struct {
    int      is_mem;     /* 1 = memory operand at lin                                  */
    uint32_t lin;
    int      reg;        /* the /r field                                               */
    int      rm;         /* register number when !is_mem                               */
    int      len;        /* bytes consumed by ModRM + SIB + displacement              */
} p32_modrm;

/* Code fetch: CS base + EIP + i. */
static uint8_t p32_cb(const p32cpu *c, uint32_t i) { return p32_rd8(c->base[1] + c->eip + i); }
static uint32_t p32_ci(const p32cpu *c, uint32_t i, int w)
{
    uint32_t v = p32_cb(c, i);
    if (w >= 2) v |= (uint32_t)p32_cb(c, i + 1) << 8;
    if (w == 4) { v |= (uint32_t)p32_cb(c, i + 2) << 16; v |= (uint32_t)p32_cb(c, i + 3) << 24; }
    return v;
}

/* 32-bit ModRM/SIB. `seg` is an override (0..5) or -1: default DS, or SS for an EBP/ESP base. */
static void p32_decode(const p32cpu *c, uint32_t at, int seg, p32_modrm *m)
{
    uint8_t mr = p32_cb(c, at);
    int mod = mr >> 6, rm = mr & 7, n = 1, s = 3;
    uint32_t ea = 0;
    m->reg = (mr >> 3) & 7;
    if (mod == 3) { m->is_mem = 0; m->rm = rm; m->len = 1; return; }
    m->is_mem = 1;
    if (rm == 4) {                                  /* SIB */
        uint8_t sib = p32_cb(c, at + 1);
        int sc = sib >> 6, ix = (sib >> 3) & 7, bs = sib & 7;
        n = 2;
        if (bs == 5 && mod == 0) { ea = p32_ci(c, at + n, 4); n += 4; }
        else { ea = c->r[bs]; if (bs == 4 || bs == 5) s = 2; }
        if (ix != 4) ea += c->r[ix] << sc;
    } else if (rm == 5 && mod == 0) {               /* disp32 */
        ea = p32_ci(c, at + 1, 4); n = 5;
    } else {
        ea = c->r[rm];
        if (rm == 5) s = 2;                         /* [EBP+d] defaults to SS */
    }
    if (mod == 1) { ea += (uint32_t)(int32_t)(int8_t)p32_cb(c, at + n); n += 1; }
    else if (mod == 2) { ea += p32_ci(c, at + n, 4); n += 4; }
    m->lin = c->base[seg >= 0 ? seg : s] + ea;
    m->len = n;
}
static uint32_t p32_rmv(const p32cpu *c, const p32_modrm *m, int w)
{ return m->is_mem ? p32_rdm(m->lin, w) : p32_greg(c, m->rm, w); }

/* ---- stack (32-bit) ---------------------------------------------------------------- */
static int p32_push_ok(const p32cpu *c, int w) { return p32_ok(c->base[2] + c->r[4] - (uint32_t)w, w, 1); }
static void p32_push(p32cpu *c, uint32_t v, int w) { c->r[4] -= (uint32_t)w; p32_wrm(c->base[2] + c->r[4], w, v); }
static uint32_t p32_pop(p32cpu *c, int w) { uint32_t v = p32_rdm(c->base[2] + c->r[4], w); c->r[4] += (uint32_t)w; return v; }

/* ---- shifts (C0/C1/D0-D3). Returns 0 to decline (RCL/RCR). ------------------------ */
static int p32_shift(uint32_t *fl, int op, uint32_t v, unsigned n, int w, uint32_t *out)
{
    uint32_t m = p32_msk(w), s = p32_sgn(w), r = v & m, f;
    unsigned bits = (unsigned)w * 8;
    n &= 0x1F;
    if (!n) { *out = r; return 1; }                 /* count 0: nothing, flags untouched */
    f = *fl;
    switch (op) {
    case 0: case 1: {                               /* ROL / ROR */
        unsigned k = n % bits;
        if (k) r = (op == 0) ? ((r << k) | (r >> (bits - k))) & m : ((r >> k) | (r << (bits - k))) & m;
        f &= ~(P32_CF | P32_OF);
        if (op == 0) { if (r & 1) f |= P32_CF; if (((r & s) != 0) != ((f & P32_CF) != 0)) f |= P32_OF; }
        else { if (r & s) f |= P32_CF; if (((r & s) != 0) != ((r & (s >> 1)) != 0)) f |= P32_OF; }
        *fl = f; *out = r; return 1; }
    case 2: case 3: return 0;                       /* RCL / RCR: decline */
    case 4: case 6: {                               /* SHL / SAL */
        uint32_t cf = (n <= bits) ? ((r >> (bits - n)) & 1) : 0;
        r = (n < 32) ? (r << n) & m : 0;
        f = (f & ~P32_ARITH) | p32_szp(r, w);
        if (cf) f |= P32_CF;
        if (((r & s) != 0) != (cf != 0)) f |= P32_OF;
        *fl = f; *out = r; return 1; }
    case 5: {                                       /* SHR */
        uint32_t cf = (r >> (n - 1)) & 1;
        uint32_t o = (r & s) != 0;
        r = (n < 32) ? r >> n : 0;
        f = (f & ~P32_ARITH) | p32_szp(r, w);
        if (cf) f |= P32_CF;
        if (o) f |= P32_OF;
        *fl = f; *out = r; return 1; }
    default: {                                      /* SAR */
        int32_t sv = (w == 1) ? (int8_t)r : (w == 2) ? (int16_t)r : (int32_t)r;
        uint32_t cf = (uint32_t)(sv >> (n - 1)) & 1;
        r = (uint32_t)(sv >> (n > 31 ? 31 : n)) & m;
        f = (f & ~P32_ARITH) | p32_szp(r, w);
        if (cf) f |= P32_CF;
        *fl = f; *out = r; return 1; }
    }
}

/* ---- one instruction ---------------------------------------------------------------- */
/* Returns 1 = executed, 0 = declined with no state changed. */
static int p32_step(p32cpu *c)
{
    uint32_t i = 0;                 /* bytes consumed so far                                */
    int seg = -1, osz = 0, rep = 0;
    uint8_t op;
    int W;
    for (;;) {                      /* prefixes */
        op = p32_cb(c, i);
        if (op == 0x66) { osz = 1; ++i; }
        else if (op == 0x26) { seg = 0; ++i; } else if (op == 0x2E) { seg = 1; ++i; }
        else if (op == 0x36) { seg = 2; ++i; } else if (op == 0x3E) { seg = 3; ++i; }
        else if (op == 0x64) { seg = 4; ++i; } else if (op == 0x65) { seg = 5; ++i; }
        else if (op == 0xF3) { rep = 1; ++i; } else if (op == 0xF2) { rep = 2; ++i; }
        else if (op == 0x67 || op == 0xF0) return 0;      /* 16-bit addressing / LOCK */
        else break;
        if (i > 4) return 0;
    }
    ++i;
    W = osz ? 2 : 4;

#define P32_DONE(n) do { c->eip += (uint32_t)(n); return 1; } while (0)

    /* ---- ALU r/m,r / r,r/m / acc,imm : 00-3F except the segment/BCD holes ---- */
    if (op < 0x40 && (op & 7) < 6) {
        int alu = op >> 3, form = op & 7, w = (form & 1) ? W : 1, store;
        uint32_t r;
        if (form == 4 || form == 5) {                        /* AL/eAX, imm */
            uint32_t b = p32_ci(c, i, w), fl = c->flags;
            r = p32_alu(&fl, alu, p32_greg(c, 0, w), b, w, &store);
            if (store) p32_sreg(c, 0, w, r);
            c->flags = fl;
            P32_DONE(i + (uint32_t)w);
        } else {
            p32_modrm m; uint32_t a, b, fl = c->flags;
            p32_decode(c, i, seg, &m);
            if (m.is_mem && !p32_ok(m.lin, w, form < 2 && alu != 7)) return 0;
            if (form < 2) { a = p32_rmv(c, &m, w); b = p32_greg(c, m.reg, w); }
            else          { a = p32_greg(c, m.reg, w); b = p32_rmv(c, &m, w); }
            r = p32_alu(&fl, alu, a, b, w, &store);
            if (store) {
                if (form < 2) { if (m.is_mem) p32_wrm(m.lin, w, r); else p32_sreg(c, m.rm, w, r); }
                else p32_sreg(c, m.reg, w, r);
            }
            c->flags = fl;
            P32_DONE(i + (uint32_t)m.len);
        }
    }
    /* ---- INC/DEC r32 (40-4F): CF preserved ---- */
    if (op >= 0x40 && op <= 0x4F) {
        int e = op & 7; uint32_t cf = c->flags & P32_CF, fl = c->flags, r;
        r = (op < 0x48) ? p32_add(&fl, p32_greg(c, e, W), 1, 0, W) : p32_sub(&fl, p32_greg(c, e, W), 1, 0, W);
        c->flags = (fl & ~P32_CF) | cf;
        p32_sreg(c, e, W, r);
        P32_DONE(i);
    }
    /* ---- PUSH/POP r (50-5F) ---- */
    if (op >= 0x50 && op <= 0x57) {
        uint32_t v = p32_greg(c, op & 7, W);
        if (!p32_push_ok(c, W)) return 0;
        p32_push(c, v, W); P32_DONE(i);
    }
    if (op >= 0x58 && op <= 0x5F) {
        uint32_t v;
        if (!p32_ok(c->base[2] + c->r[4], W, 0)) return 0;
        v = p32_pop(c, W);
        p32_sreg(c, op & 7, W, v);
        P32_DONE(i);
    }
    /* ---- PUSHAD / POPAD (60/61) ---- */
    if (op == 0x60) {
        uint32_t sp0 = c->r[4]; int k;
        if (osz || !p32_ok(c->base[2] + c->r[4] - 32, 32, 1)) return 0;
        for (k = 0; k < 8; ++k) p32_push(c, k == 4 ? sp0 : c->r[k], 4);
        P32_DONE(i);
    }
    if (op == 0x61) {
        int k; uint32_t v[8];
        if (osz || !p32_ok(c->base[2] + c->r[4], 32, 0)) return 0;
        for (k = 7; k >= 0; --k) v[k] = p32_pop(c, 4);
        for (k = 0; k < 8; ++k) if (k != 4) c->r[k] = v[k];   /* the saved ESP is discarded */
        P32_DONE(i);
    }
    /* ---- PUSH imm (68/6A) ---- */
    if (op == 0x68 || op == 0x6A) {
        uint32_t v = (op == 0x68) ? p32_ci(c, i, W) : (uint32_t)(int32_t)(int8_t)p32_cb(c, i);
        if (!p32_push_ok(c, W)) return 0;
        p32_push(c, v, W);
        P32_DONE(i + (op == 0x68 ? (uint32_t)W : 1u));
    }
    /* ---- IMUL r, r/m, imm (69/6B) ---- */
    if (op == 0x69 || op == 0x6B) {
        p32_modrm m; int64_t a, b, prod; uint32_t r, n;
        p32_decode(c, i, seg, &m);
        if (m.is_mem && !p32_ok(m.lin, W, 0)) return 0;
        a = (W == 4) ? (int64_t)(int32_t)p32_rmv(c, &m, 4) : (int64_t)(int16_t)p32_rmv(c, &m, 2);
        n = i + (uint32_t)m.len;
        if (op == 0x6B) { b = (int8_t)p32_cb(c, n); n += 1; }
        else { uint32_t iv = p32_ci(c, n, W); n += (uint32_t)W; b = (W == 4) ? (int64_t)(int32_t)iv : (int64_t)(int16_t)iv; }
        prod = a * b; r = (uint32_t)prod & p32_msk(W);
        p32_sreg(c, m.reg, W, r);
        c->flags &= ~(P32_CF | P32_OF);
        if (prod != ((W == 4) ? (int64_t)(int32_t)r : (int64_t)(int16_t)r)) c->flags |= P32_CF | P32_OF;
        P32_DONE(n);
    }
    /* ---- Jcc rel8 (70-7F) ---- */
    if (op >= 0x70 && op <= 0x7F) {
        int32_t d = (int8_t)p32_cb(c, i);
        c->eip += i + 1;
        if (p32_cond(c->flags, op & 0x0F)) c->eip += (uint32_t)d;
        return 1;
    }
    /* ---- group 1 (80/81/83) ---- */
    if (op == 0x80 || op == 0x81 || op == 0x83) {
        int w = (op == 0x80) ? 1 : W, store; p32_modrm m; uint32_t a, b, r, n, fl = c->flags;
        p32_decode(c, i, seg, &m);
        n = i + (uint32_t)m.len;
        if (op == 0x81) { b = p32_ci(c, n, w); n += (uint32_t)w; }
        else { b = (uint32_t)(int32_t)(int8_t)p32_cb(c, n); n += 1; }
        if (m.is_mem && !p32_ok(m.lin, w, m.reg != 7)) return 0;
        a = p32_rmv(c, &m, w);
        r = p32_alu(&fl, m.reg, a, b, w, &store);
        if (store) { if (m.is_mem) p32_wrm(m.lin, w, r); else p32_sreg(c, m.rm, w, r); }
        c->flags = fl;
        P32_DONE(n);
    }
    /* ---- TEST r/m,r (84/85) ---- */
    if (op == 0x84 || op == 0x85) {
        int w = (op & 1) ? W : 1; p32_modrm m;
        p32_decode(c, i, seg, &m);
        if (m.is_mem && !p32_ok(m.lin, w, 0)) return 0;
        p32_logic(&c->flags, p32_rmv(c, &m, w) & p32_greg(c, m.reg, w), w);
        P32_DONE(i + (uint32_t)m.len);
    }
    /* ---- XCHG r/m,r (86/87) ---- */
    if (op == 0x86 || op == 0x87) {
        int w = (op & 1) ? W : 1; p32_modrm m; uint32_t a, b;
        p32_decode(c, i, seg, &m);
        if (m.is_mem && !p32_ok(m.lin, w, 1)) return 0;
        a = p32_rmv(c, &m, w); b = p32_greg(c, m.reg, w);
        if (m.is_mem) p32_wrm(m.lin, w, b); else p32_sreg(c, m.rm, w, b);
        p32_sreg(c, m.reg, w, a);
        P32_DONE(i + (uint32_t)m.len);
    }
    /* ---- MOV (88-8B) ---- */
    if (op >= 0x88 && op <= 0x8B) {
        int w = (op & 1) ? W : 1; p32_modrm m;
        p32_decode(c, i, seg, &m);
        if (m.is_mem && !p32_ok(m.lin, w, op < 0x8A)) return 0;
        if (op < 0x8A) { uint32_t v = p32_greg(c, m.reg, w);
                         if (m.is_mem) p32_wrm(m.lin, w, v); else p32_sreg(c, m.rm, w, v); }
        else p32_sreg(c, m.reg, w, p32_rmv(c, &m, w));
        P32_DONE(i + (uint32_t)m.len);
    }
    /* ---- LEA (8D): the offset, not the linear address ---- */
    if (op == 0x8D) {
        p32_modrm m; uint32_t sb;
        p32_decode(c, i, -1, &m);
        if (!m.is_mem) return 0;
        /* p32_decode added a segment base; LEA wants the effective address alone. The
           default segment for the form is DS or SS; subtract the one it used. */
        sb = c->base[3];
        { uint8_t mr = p32_cb(c, i); int mod = mr >> 6, rm = mr & 7;
          if (rm == 5 && mod != 0) sb = c->base[2];
          if (rm == 4) { uint8_t sib = p32_cb(c, i + 1); int bs = sib & 7;
                         if (bs == 4 || (bs == 5 && mod != 0)) sb = c->base[2]; } }
        p32_sreg(c, m.reg, W, m.lin - sb);
        P32_DONE(i + (uint32_t)m.len);
    }
    /* ---- NOP / XCHG eAX,r (90-97) ---- */
    if (op == 0x90) { if (rep) return 0; P32_DONE(i); }               /* F3 90 = PAUSE: decline */
    if (op > 0x90 && op <= 0x97) {
        uint32_t a = p32_greg(c, 0, W), b = p32_greg(c, op & 7, W);
        p32_sreg(c, 0, W, b); p32_sreg(c, op & 7, W, a); P32_DONE(i);
    }
    /* ---- CWDE/CBW (98), CDQ/CWD (99) ---- */
    if (op == 0x98) {
        if (W == 4) c->r[0] = (uint32_t)(int32_t)(int16_t)(c->r[0] & 0xFFFF);
        else p32_sreg(c, 0, 2, (uint32_t)(int16_t)(int8_t)(c->r[0] & 0xFF));
        P32_DONE(i);
    }
    if (op == 0x99) {
        if (W == 4) c->r[2] = (c->r[0] & 0x80000000u) ? 0xFFFFFFFFu : 0;
        else p32_sreg(c, 2, 2, (c->r[0] & 0x8000u) ? 0xFFFFu : 0);
        P32_DONE(i);
    }
    /* ---- MOV moffs (A0-A3) ---- */
    if (op >= 0xA0 && op <= 0xA3) {
        int w = (op & 1) ? W : 1; uint32_t lin = c->base[seg >= 0 ? seg : 3] + p32_ci(c, i, 4);
        if (!p32_ok(lin, w, op >= 0xA2)) return 0;
        if (op < 0xA2) p32_sreg(c, 0, w, p32_rdm(lin, w)); else p32_wrm(lin, w, p32_greg(c, 0, w));
        P32_DONE(i + 4);
    }
    /* ---- TEST acc,imm (A8/A9) ---- */
    if (op == 0xA8 || op == 0xA9) {
        int w = (op & 1) ? W : 1;
        p32_logic(&c->flags, p32_greg(c, 0, w) & p32_ci(c, i, w), w);
        P32_DONE(i + (uint32_t)w);
    }
    /* ---- string ops: MOVS (A4/A5), STOS (AA/AB), LODS (AC/AD), with REP ---- */
    if (op == 0xA4 || op == 0xA5 || op == 0xAA || op == 0xAB || op == 0xAC || op == 0xAD) {
        int w = (op & 1) ? W : 1;
        int32_t d = (c->flags & P32_DF) ? -w : w;
        uint32_t n = rep ? c->r[1] : 1, k;
        uint32_t sbase = c->base[seg >= 0 ? seg : 3], dbase = c->base[0];
        if (rep == 2) return 0;                                  /* REPNE on MOVS/STOS: odd */
        if (n > 0x10000u) return 0;                              /* bounded; the CPU can have it */
        for (k = 0; k < n; ++k) {                                /* probe the whole range first */
            uint32_t so = c->r[6] + (uint32_t)(d * (int32_t)k), dof = c->r[7] + (uint32_t)(d * (int32_t)k);
            if (op <= 0xA5 || op >= 0xAC) if (!p32_ok(sbase + so, w, 0)) return 0;
            if (op <= 0xAB)               if (!p32_ok(dbase + dof, w, 1)) return 0;
        }
        for (k = 0; k < n; ++k) {
            if (op == 0xA4 || op == 0xA5) p32_wrm(dbase + c->r[7], w, p32_rdm(sbase + c->r[6], w));
            else if (op == 0xAA || op == 0xAB) p32_wrm(dbase + c->r[7], w, p32_greg(c, 0, w));
            else p32_sreg(c, 0, w, p32_rdm(sbase + c->r[6], w));
            if (op <= 0xA5 || op >= 0xAC) c->r[6] += (uint32_t)d;
            if (op <= 0xAB) c->r[7] += (uint32_t)d;
        }
        if (rep) c->r[1] = 0;
        P32_DONE(i);
    }
    /* ---- MOV r,imm (B0-BF) ---- */
    if (op >= 0xB0 && op <= 0xB7) { p32_sreg(c, op & 7, 1, p32_cb(c, i)); P32_DONE(i + 1); }
    if (op >= 0xB8 && op <= 0xBF) { p32_sreg(c, op & 7, W, p32_ci(c, i, W)); P32_DONE(i + (uint32_t)W); }
    /* ---- shifts: C0/C1 imm8, D0/D1 by 1, D2/D3 by CL ---- */
    if (op == 0xC0 || op == 0xC1 || (op >= 0xD0 && op <= 0xD3)) {
        int w = (op & 1) ? W : 1; p32_modrm m; uint32_t n, cnt, r, fl = c->flags;
        p32_decode(c, i, seg, &m);
        n = i + (uint32_t)m.len;
        if (op <= 0xC1) { cnt = p32_cb(c, n); n += 1; }
        else cnt = (op <= 0xD1) ? 1 : (c->r[1] & 0xFF);
        if (m.is_mem && !p32_ok(m.lin, w, 1)) return 0;
        if (!p32_shift(&fl, m.reg, p32_rmv(c, &m, w), cnt, w, &r)) return 0;
        if (m.is_mem) p32_wrm(m.lin, w, r); else p32_sreg(c, m.rm, w, r);
        c->flags = fl;
        P32_DONE(n);
    }
    /* ---- RET / RET imm16 (C3/C2) ---- */
    if (op == 0xC3 || op == 0xC2) {
        uint32_t t, extra = (op == 0xC2) ? (p32_cb(c, i) | ((uint32_t)p32_cb(c, i + 1) << 8)) : 0;
        if (osz || !p32_ok(c->base[2] + c->r[4], 4, 0)) return 0;
        t = p32_pop(c, 4);
        c->r[4] += extra;
        c->eip = t;
        return 1;
    }
    /* ---- MOV r/m,imm (C6/C7) ---- */
    if (op == 0xC6 || op == 0xC7) {
        int w = (op & 1) ? W : 1; p32_modrm m; uint32_t v;
        p32_decode(c, i, seg, &m);
        if (m.reg != 0) return 0;
        v = p32_ci(c, i + (uint32_t)m.len, w);
        if (m.is_mem && !p32_ok(m.lin, w, 1)) return 0;
        if (m.is_mem) p32_wrm(m.lin, w, v); else p32_sreg(c, m.rm, w, v);
        P32_DONE(i + (uint32_t)m.len + (uint32_t)w);
    }
    /* ---- IN/OUT (E4-E7 imm8, EC-EF DX) ---- */
    if ((op >= 0xE4 && op <= 0xE7) || (op >= 0xEC && op <= 0xEF)) {
        int w = (op & 1) ? W : 1, isout = (op & 2) != 0;
        uint16_t port = (op >= 0xEC) ? (uint16_t)(c->r[2] & 0xFFFF) : (uint16_t)p32_cb(c, i);
        uint32_t n = (op >= 0xEC) ? i : i + 1;
        if (isout) p32_out(port, w, p32_greg(c, 0, w));
        else p32_sreg(c, 0, w, p32_in(port, w));
        P32_DONE(n);
    }
    /* ---- CALL rel32 (E8), JMP rel32 (E9), JMP rel8 (EB) ---- */
    if (op == 0xE8) {
        uint32_t d = p32_ci(c, i, 4);
        if (osz || !p32_push_ok(c, 4)) return 0;
        p32_push(c, c->eip + i + 4, 4);
        c->eip += i + 4 + d; return 1;
    }
    if (op == 0xE9) { if (osz) return 0; c->eip += i + 4 + p32_ci(c, i, 4); return 1; }
    if (op == 0xEB) { c->eip += i + 1 + (uint32_t)(int32_t)(int8_t)p32_cb(c, i); return 1; }
    /* ---- flag ops ---- */
    if (op == 0xF5) { c->flags ^= P32_CF; P32_DONE(i); }
    if (op == 0xF8) { c->flags &= ~P32_CF; P32_DONE(i); }
    if (op == 0xF9) { c->flags |= P32_CF; P32_DONE(i); }
    if (op == 0xFC) { c->flags &= ~P32_DF; P32_DONE(i); }
    if (op == 0xFD) { c->flags |= P32_DF; P32_DONE(i); }
    /* ---- group 3 (F6/F7) ---- */
    if (op == 0xF6 || op == 0xF7) {
        int w = (op == 0xF6) ? 1 : W; p32_modrm m; uint32_t e, n;
        p32_decode(c, i, seg, &m);
        n = i + (uint32_t)m.len;
        if (m.is_mem && !p32_ok(m.lin, w, m.reg == 2 || m.reg == 3)) return 0;
        e = p32_rmv(c, &m, w);
        switch (m.reg) {
        case 0: case 1: p32_logic(&c->flags, e & p32_ci(c, n, w), w); n += (uint32_t)w; break;
        case 2: { uint32_t r = ~e & p32_msk(w); if (m.is_mem) p32_wrm(m.lin, w, r); else p32_sreg(c, m.rm, w, r); break; }
        case 3: { uint32_t fl = c->flags, r = p32_sub(&fl, 0, e, 0, w);
                  if (e & p32_msk(w)) fl |= P32_CF; else fl &= ~P32_CF;
                  if (m.is_mem) p32_wrm(m.lin, w, r); else p32_sreg(c, m.rm, w, r);
                  c->flags = fl; break; }
        case 4: case 5: {                                        /* MUL / IMUL acc */
            int ovf;
            if (w == 1) {
                uint32_t p = (m.reg == 4) ? (c->r[0] & 0xFF) * e
                                          : (uint32_t)((int32_t)(int8_t)c->r[0] * (int32_t)(int8_t)e) & 0xFFFF;
                p32_sreg(c, 0, 2, p);
                ovf = (m.reg == 4) ? (p >> 8) != 0 : (int16_t)p != (int8_t)p;
            } else if (w == 2) {
                uint32_t p = (m.reg == 4) ? (c->r[0] & 0xFFFF) * e
                                          : (uint32_t)((int32_t)(int16_t)c->r[0] * (int32_t)(int16_t)e);
                p32_sreg(c, 0, 2, p); p32_sreg(c, 2, 2, p >> 16);
                ovf = (m.reg == 4) ? (p >> 16) != 0 : (int32_t)p != (int16_t)p;
            } else {
                uint64_t p = (m.reg == 4) ? (uint64_t)c->r[0] * e
                                          : (uint64_t)((int64_t)(int32_t)c->r[0] * (int64_t)(int32_t)e);
                c->r[0] = (uint32_t)p; c->r[2] = (uint32_t)(p >> 32);
                ovf = (m.reg == 4) ? (p >> 32) != 0 : (int64_t)p != (int32_t)p;
            }
            c->flags &= ~(P32_CF | P32_OF);
            if (ovf) c->flags |= P32_CF | P32_OF;
            break; }
        default: {                                               /* DIV / IDIV: #DE declines */
            if (!(e & p32_msk(w))) return 0;
            if (w == 1) {
                uint32_t dv = c->r[0] & 0xFFFF;
                if (m.reg == 6) { uint32_t q = dv / e, r = dv % e; if (q > 0xFF) return 0;
                                  p32_sreg(c, 0, 2, q | (r << 8)); }
                else { int32_t sd = (int16_t)dv, d = (int8_t)e, q = sd / d, r = sd % d;
                       if (q > 127 || q < -128) return 0; p32_sreg(c, 0, 2, (q & 0xFF) | ((r & 0xFF) << 8)); }
            } else if (w == 2) {
                uint32_t dv = ((c->r[2] & 0xFFFF) << 16) | (c->r[0] & 0xFFFF);
                if (m.reg == 6) { uint32_t q = dv / e, r = dv % e; if (q > 0xFFFF) return 0;
                                  p32_sreg(c, 0, 2, q); p32_sreg(c, 2, 2, r); }
                else { int32_t sd = (int32_t)dv, d = (int16_t)e, q = sd / d, r = sd % d;
                       if (q > 32767 || q < -32768) return 0; p32_sreg(c, 0, 2, (uint32_t)q); p32_sreg(c, 2, 2, (uint32_t)r); }
            } else {
                uint64_t dv = ((uint64_t)c->r[2] << 32) | c->r[0];
                if (m.reg == 6) { uint64_t q = dv / e, r = dv % e; if (q > 0xFFFFFFFFu) return 0;
                                  c->r[0] = (uint32_t)q; c->r[2] = (uint32_t)r; }
                else { int64_t sd = (int64_t)dv, d = (int32_t)e, q, r;
                       if (d == -1 && sd == (int64_t)0x8000000000000000ULL) return 0;
                       q = sd / d; r = sd % d;
                       if (q > 2147483647LL || q < -2147483648LL) return 0;
                       c->r[0] = (uint32_t)q; c->r[2] = (uint32_t)r; }
            }
            break; }
        }
        P32_DONE(n);
    }
    /* ---- group 4/5 (FE/FF): INC/DEC r/m; FF: CALL/JMP near r/m, PUSH r/m ---- */
    if (op == 0xFE || op == 0xFF) {
        int w = (op == 0xFE) ? 1 : W; p32_modrm m;
        p32_decode(c, i, seg, &m);
        if (m.reg <= 1) {
            uint32_t fl = c->flags, cf = c->flags & P32_CF, r;
            if (m.is_mem && !p32_ok(m.lin, w, 1)) return 0;
            r = (m.reg == 0) ? p32_add(&fl, p32_rmv(c, &m, w), 1, 0, w) : p32_sub(&fl, p32_rmv(c, &m, w), 1, 0, w);
            if (m.is_mem) p32_wrm(m.lin, w, r); else p32_sreg(c, m.rm, w, r);
            c->flags = (fl & ~P32_CF) | cf;
            P32_DONE(i + (uint32_t)m.len);
        }
        if (op == 0xFF && (m.reg == 2 || m.reg == 4) && !osz) {
            uint32_t t;
            if (m.is_mem && !p32_ok(m.lin, 4, 0)) return 0;
            t = p32_rmv(c, &m, 4);
            if (m.reg == 2) { if (!p32_push_ok(c, 4)) return 0; p32_push(c, c->eip + i + (uint32_t)m.len, 4); }
            c->eip = t; return 1;
        }
        if (op == 0xFF && m.reg == 6) {
            uint32_t v;
            if (m.is_mem && !p32_ok(m.lin, W, 0)) return 0;
            if (!p32_push_ok(c, W)) return 0;
            v = p32_rmv(c, &m, W);
            p32_push(c, v, W);
            P32_DONE(i + (uint32_t)m.len);
        }
        return 0;
    }
    /* ---- 0F xx ---- */
    if (op == 0x0F) {
        uint8_t o2 = p32_cb(c, i);
        ++i;
        if (o2 >= 0x80 && o2 <= 0x8F) {                          /* Jcc rel32 */
            uint32_t d = p32_ci(c, i, 4);
            if (osz) return 0;
            c->eip += i + 4;
            if (p32_cond(c->flags, o2 & 0x0F)) c->eip += d;
            return 1;
        }
        if (o2 >= 0x90 && o2 <= 0x9F) {                          /* SETcc r/m8 */
            p32_modrm m; uint32_t v = (uint32_t)p32_cond(c->flags, o2 & 0x0F);
            p32_decode(c, i, seg, &m);
            if (m.is_mem && !p32_ok(m.lin, 1, 1)) return 0;
            if (m.is_mem) p32_wrm(m.lin, 1, v); else p32_sreg(c, m.rm, 1, v);
            P32_DONE(i + (uint32_t)m.len);
        }
        if (o2 == 0xB6 || o2 == 0xB7 || o2 == 0xBE || o2 == 0xBF) {  /* MOVZX / MOVSX */
            int sw = (o2 & 1) ? 2 : 1; p32_modrm m; uint32_t v;
            p32_decode(c, i, seg, &m);
            if (m.is_mem && !p32_ok(m.lin, sw, 0)) return 0;
            v = p32_rmv(c, &m, sw);
            if (o2 >= 0xBE) v = (sw == 1) ? (uint32_t)(int32_t)(int8_t)v : (uint32_t)(int32_t)(int16_t)v;
            p32_sreg(c, m.reg, W, v);
            P32_DONE(i + (uint32_t)m.len);
        }
        if (o2 == 0xAF) {                                        /* IMUL r, r/m */
            p32_modrm m; int64_t prod; uint32_t r;
            p32_decode(c, i, seg, &m);
            if (m.is_mem && !p32_ok(m.lin, W, 0)) return 0;
            if (W == 4) prod = (int64_t)(int32_t)c->r[m.reg] * (int64_t)(int32_t)p32_rmv(c, &m, 4);
            else        prod = (int64_t)(int16_t)c->r[m.reg] * (int64_t)(int16_t)p32_rmv(c, &m, 2);
            r = (uint32_t)prod & p32_msk(W);
            p32_sreg(c, m.reg, W, r);
            c->flags &= ~(P32_CF | P32_OF);
            if (prod != ((W == 4) ? (int64_t)(int32_t)r : (int64_t)(int16_t)r)) c->flags |= P32_CF | P32_OF;
            P32_DONE(i + (uint32_t)m.len);
        }
        if (o2 == 0xA4 || o2 == 0xA5 || o2 == 0xAC || o2 == 0xAD) {  /* SHLD / SHRD */
            p32_modrm m; uint32_t n, cnt, dst, src, r, fl, bits = (uint32_t)W * 8, msk = p32_msk(W);
            p32_decode(c, i, seg, &m);
            n = i + (uint32_t)m.len;
            if (o2 == 0xA4 || o2 == 0xAC) { cnt = p32_cb(c, n); n += 1; } else cnt = c->r[1] & 0xFF;
            cnt &= 0x1F;
            if (m.is_mem && !p32_ok(m.lin, W, 1)) return 0;
            if (!cnt) P32_DONE(n);
            if (cnt > bits) return 0;                            /* undefined: let the CPU */
            dst = p32_rmv(c, &m, W); src = p32_greg(c, m.reg, W);
            fl = c->flags;
            if (o2 <= 0xA5) {                                    /* SHLD */
                uint64_t cat = ((uint64_t)dst << bits) | src;
                r = (uint32_t)((cat << cnt) >> bits) & msk;
                fl = (fl & ~P32_ARITH) | p32_szp(r, W);
                if ((dst >> (bits - cnt)) & 1) fl |= P32_CF;
                if (((r ^ dst) & p32_sgn(W))) fl |= P32_OF;
            } else {                                             /* SHRD */
                uint64_t cat = ((uint64_t)src << bits) | dst;
                r = (uint32_t)(cat >> cnt) & msk;
                fl = (fl & ~P32_ARITH) | p32_szp(r, W);
                if ((dst >> (cnt - 1)) & 1) fl |= P32_CF;
                if (((r ^ dst) & p32_sgn(W))) fl |= P32_OF;
            }
            if (m.is_mem) p32_wrm(m.lin, W, r); else p32_sreg(c, m.rm, W, r);
            c->flags = fl;
            P32_DONE(n);
        }
        return 0;
    }
#undef P32_DONE
    return 0;
}

#endif /* PM32INTERP_H */
