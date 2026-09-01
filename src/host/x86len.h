/* x86len.h -- x86 instruction LENGTH decoding, and the instruction-boundary test the
 * INT-site patcher needs.  Header-only, no CRT, no <windows.h> (so the off-VM battery
 * can compile it with the native cc).
 *
 * ═══════════════════════════════════════════════════════════════════════════════════
 *  WHY THIS EXISTS -- IT IS THE BUG THAT KILLED DOOM FOR FIVE SESSIONS.
 * ═══════════════════════════════════════════════════════════════════════════════════
 *  Protected-mode `INT nn` is the one fault XP will not reflect, so dpmi_patch_code_region()
 *  rewrites every `CD nn` in the client's code to a BOP (`C4 C4`) and services it in the
 *  host.  It did that by scanning for the BYTE PAIR, with no idea where instructions start.
 *
 *  In Doom's code object that is wrong in exactly three places, and one of them is fatal:
 *
 *      obj1+0x3593f   39 fa  7e cd  31 c9        cmp edx,edi / jle -51 / xor ecx,ecx
 *                            ^^ ^^^^^^^
 *      The `cd` is the DISPLACEMENT of `jle`, and the `31` is the opcode of the `xor`.
 *      Patching turned that into
 *                            7e c4  c4 c9        jle -60 / (garbage)
 *      i.e. R_InitTextureMapping's loop-2 back edge now jumps to obj1+0x35905, which is
 *      the middle of a `jl` -- and the guest executes `cmp ecx,[ebx+0x034fe02d]` off a
 *      register holding an angle.  Wild read, #PF at CPL 3, and XP tears the VDM down
 *      silently: no VEH, no watchdog line, no last log entry.  Sessions 16-20 chased that
 *      as a mystery in Doom.  It was ours.
 *      (The other two: obj1+0x0ae0f is the displacement of a `call rel32`, obj1+0x0512d
 *      is a word in a data table.  Both were being corrupted too.)
 *
 *  ► THE RULE: PATCH ONLY WHAT IS AN INSTRUCTION.  x86 is self-synchronising -- decode
 *    forward from a few dozen earlier offsets and the streams converge on the real
 *    boundaries within a handful of instructions.  So for each candidate site, decode
 *    from each of the preceding `span` bytes and count how many land exactly on it.
 *    Measured against objdump over Doom's 32-bit code object and DOS/4GW's two 16-bit
 *    modules (242 real INT sites, 7 false byte pairs):
 *
 *        real sites   19..48 votes out of 48        -> all 242 kept
 *        false pairs   0.. 3 votes out of 48        -> all   7 rejected
 *
 *    A 25% threshold sits in the middle of that gap with room on both sides.  It is
 *    deliberately biased toward KEEPING: a missed real site is an unpatched `CD nn` that
 *    kills the guest, while a rejected false one only costs a service we never needed.
 *
 *  ► SCOPE.  This decodes LENGTHS, not semantics.  It has no notion of what an
 *    instruction does and never needs one.  Undecodable opcodes return 0, which the
 *    boundary test reads as "this stream is not code" -- a vote against, which is the
 *    conservative direction for a stream that started mid-instruction.
 */
#ifndef HOST_X86LEN_H
#define HOST_X86LEN_H

/* imm kinds. `z` = 2 bytes with a 16-bit operand size, 4 with a 32-bit one. */
#define XL_NONE  0
#define XL_IB    1      /* imm8                                  */
#define XL_IZ    2      /* imm16/imm32 by operand size           */
#define XL_IW    3      /* imm16 always (ret imm16)              */
#define XL_MOFF  4      /* moffs: 2/4 by ADDRESS size            */
#define XL_ENTER 5      /* imm16 + imm8                          */
#define XL_FAR   6      /* ptr16:16/32 -> z + 2                  */
#define XL_G3B   7      /* group 3 /0,/1 take imm8               */
#define XL_G3Z   8      /* group 3 /0,/1 take immz               */

#define XL_MR    0x10   /* has a modrm byte                      */

/* One-byte opcode map: XL_MR | <imm kind>. */
static const unsigned char xl_map1[256] = {
/*00*/ 0x10,0x10,0x10,0x10,   1,   2,   0,   0,
/*08*/ 0x10,0x10,0x10,0x10,   1,   2,   0,   0,   /* 0F is handled before the table */
/*10*/ 0x10,0x10,0x10,0x10,   1,   2,   0,   0,
/*18*/ 0x10,0x10,0x10,0x10,   1,   2,   0,   0,
/*20*/ 0x10,0x10,0x10,0x10,   1,   2,   0,   0,
/*28*/ 0x10,0x10,0x10,0x10,   1,   2,   0,   0,
/*30*/ 0x10,0x10,0x10,0x10,   1,   2,   0,   0,
/*38*/ 0x10,0x10,0x10,0x10,   1,   2,   0,   0,
/*40*/    0,   0,   0,   0,   0,   0,   0,   0,
/*48*/    0,   0,   0,   0,   0,   0,   0,   0,
/*50*/    0,   0,   0,   0,   0,   0,   0,   0,
/*58*/    0,   0,   0,   0,   0,   0,   0,   0,
/*60*/    0,   0,0x10,0x10,   0,   0,   0,   0,
/*68*/    2,0x12,   1,0x11,   0,   0,   0,   0,
/*70*/    1,   1,   1,   1,   1,   1,   1,   1,
/*78*/    1,   1,   1,   1,   1,   1,   1,   1,
/*80*/ 0x11,0x12,0x11,0x11,0x10,0x10,0x10,0x10,
/*88*/ 0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,
/*90*/    0,   0,   0,   0,   0,   0,   0,   0,
/*98*/    0,   0,   6,   0,   0,   0,   0,   0,
/*A0*/    4,   4,   4,   4,   0,   0,   0,   0,
/*A8*/    1,   2,   0,   0,   0,   0,   0,   0,
/*B0*/    1,   1,   1,   1,   1,   1,   1,   1,
/*B8*/    2,   2,   2,   2,   2,   2,   2,   2,
/*C0*/ 0x11,0x11,   3,   0,0x10,0x10,0x11,0x12,
/*C8*/    5,   0,   3,   0,   0,   1,   0,   0,
/*D0*/ 0x10,0x10,0x10,0x10,   1,   1,   0,   0,
/*D8*/ 0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,   /* x87 */
/*E0*/    1,   1,   1,   1,   1,   1,   1,   1,
/*E8*/    2,   2,   6,   1,   0,   0,   0,   0,
/*F0*/    0,   0,   0,   0,   0,   0,0x17,0x18,
/*F8*/    0,   0,   0,   0,   0,   0,0x10,0x10
};

/* Is `op` a prefix?  (Segment overrides, operand/address size, lock, rep.) */
static int xl_is_prefix(unsigned char op)
{
    return op == 0x26 || op == 0x2E || op == 0x36 || op == 0x3E
        || op == 0x64 || op == 0x65 || op == 0x66 || op == 0x67
        || op == 0xF0 || op == 0xF2 || op == 0xF3;
}

/* 0F-escaped opcodes.  Most take a modrm and no immediate; these are the exceptions. */
static unsigned xl_map2(unsigned char o2)
{
    switch (o2) {
    case 0x05: case 0x06: case 0x07: case 0x08: case 0x09: case 0x0B: case 0x0E:
    case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x77:
    case 0xA0: case 0xA1: case 0xA2: case 0xA8: case 0xA9: case 0xAA:
        return XL_NONE;
    case 0xC8: case 0xC9: case 0xCA: case 0xCB:                 /* bswap */
    case 0xCC: case 0xCD: case 0xCE: case 0xCF:
        return XL_NONE;
    case 0x70: case 0x71: case 0x72: case 0x73:                 /* modrm + imm8 */
    case 0xA4: case 0xAC: case 0xBA:
    case 0xC2: case 0xC4: case 0xC5: case 0xC6:
        return XL_MR | XL_IB;
    default:
        if (o2 >= 0x80 && o2 <= 0x8F) return XL_IZ;             /* jcc rel16/32  */
        return XL_MR | XL_NONE;
    }
}

/* Bytes consumed by a modrm (+sib +disp).  0 = ran off the end. */
static unsigned xl_modrm(const unsigned char *b, unsigned i, unsigned n, int addr32)
{
    unsigned char m;
    unsigned mod, rm, len = 1;
    if (i >= n) return 0;
    m = b[i]; mod = (unsigned)(m >> 6); rm = (unsigned)(m & 7);
    if (mod == 3) return 1;
    if (addr32) {
        if (rm == 4) {                                   /* sib */
            if (i + 1 >= n) return 0;
            if (mod == 0 && (b[i + 1] & 7) == 5) len += 4;
            len += 1;
        } else if (mod == 0 && rm == 5) {
            len += 4;
        }
        if      (mod == 1) len += 1;
        else if (mod == 2) len += 4;
    } else {
        if (mod == 0 && rm == 6) len += 2;
        if      (mod == 1) len += 1;
        else if (mod == 2) len += 2;
    }
    return len;
}

/* Length in bytes of the instruction at b[i], or 0 if it cannot be decoded / runs off
   the end.  `d32` is the code segment's D/B bit (1 = 32-bit default operand+address). */
static unsigned x86_insn_len(const unsigned char *b, unsigned i, unsigned n, int d32)
{
    unsigned start = i, npfx = 0, ent, z;
    int op32 = d32, ad32 = d32, reg = -1;
    unsigned char op;

    while (i < n && xl_is_prefix(b[i])) {
        if      (b[i] == 0x66) op32 = !d32;
        else if (b[i] == 0x67) ad32 = !d32;
        ++i;
        if (++npfx > 8) return 0;                        /* prefix soup: not code   */
    }
    if (i >= n) return 0;
    op = b[i++];
    if (op == 0x0F) {
        unsigned char o2;
        if (i >= n) return 0;
        o2 = b[i++];
        if (o2 == 0x38 || o2 == 0x3A) {                  /* 3-byte escapes          */
            unsigned r;
            if (i >= n) return 0;
            ++i;
            r = xl_modrm(b, i, n, ad32);
            if (!r) return 0;
            i += r;
            if (o2 == 0x3A) ++i;
            return (i <= n) ? i - start : 0;
        }
        ent = xl_map2(o2);
    } else {
        ent = xl_map1[op];
    }
    if (ent & XL_MR) {
        unsigned r;
        if (i >= n) return 0;
        reg = (int)((b[i] >> 3) & 7);
        r = xl_modrm(b, i, n, ad32);
        if (!r) return 0;
        i += r;
    }
    z = op32 ? 4u : 2u;
    switch (ent & 0x0F) {
    case XL_NONE:                     break;
    case XL_IB:    i += 1;            break;
    case XL_IZ:    i += z;            break;
    case XL_IW:    i += 2;            break;
    case XL_MOFF:  i += ad32 ? 4u : 2u; break;
    case XL_ENTER: i += 3;            break;
    case XL_FAR:   i += z + 2;        break;
    case XL_G3B:   if (reg >= 0 && reg < 2) i += 1; break;
    case XL_G3Z:   if (reg >= 0 && reg < 2) i += z; break;
    default:       return 0;
    }
    return (i <= n) ? i - start : 0;
}

/* Does an instruction START at b[off]?  Decodes forward from each of the preceding
   `span` bytes and counts how many streams land exactly on `off`.  See the header
   commentary for the measured separation and why the threshold is a quarter. */
#define X86_BOUNDARY_SPAN 48u

static int x86_is_insn_start(const unsigned char *b, unsigned off, unsigned n, int d32)
{
    unsigned span = (off < X86_BOUNDARY_SPAN) ? off : X86_BOUNDARY_SPAN;
    unsigned s, tries = 0, votes = 0;
    if (off >= n) return 0;
    for (s = off - span; s < off; ++s) {
        unsigned i = s;
        ++tries;
        while (i < off) {
            unsigned len = x86_insn_len(b, i, n, d32);
            if (!len) break;                             /* not a decodable stream  */
            i += len;
        }
        if (i == off) ++votes;
    }
    if (!tries) return 1;                                /* at the very start: trust it */
    return votes * 4 >= tries;
}

/* May the `CD nn` at b[off] be rewritten to a BOP?
 *
 * ► THE TWO ERRORS USED TO BE SYMMETRIC. THEY ARE NOT ANY MORE, AND THAT IS THE
 *   WHOLE OF THIS RULE.  Both failures were measured on the rig, one after the other:
 *       accepted a false one  -> Doom died in R_InitTextureMapping (five sessions lost)
 *       refused a real one    -> the run died inside DOS/4GW's own startup, at its
 *                                `mov ah,30h / int 21h` DOS-version check, 54,000 log
 *                                lines earlier
 *   Because refusing was ALSO fatal, this rule was deliberately narrow: it rejected a
 *   site only when it could name the owning instruction AND that instruction was a
 *   RELATIVE BRANCH -- the class that provably rewrites a jump target.  Everything
 *   else was kept.  "Reject anything a confirmed instruction covers" was tried and
 *   reverted, because DOS/4GW's version check is preceded by the string
 *   "requires DOS/16M\n\r$": every backward anchor decodes ASCII, the real site scores
 *   1 vote in 48, and the ASCII stream's `30 cd` (xor ch,cl) "covers" it with 47.  By
 *   coverage alone that was indistinguishable from Doom's `jle`.
 *
 * ► ★★★ SESSION 39: THE SECOND HALF OF THAT PREMISE IS NO LONGER TRUE.  A raw `CD nn`
 *   in protected mode is not fatal any more.  Since session 34 a #GP whose error code
 *   has the IDT bit set IS that interrupt: the host takes the vector out of the error
 *   code, CONFIRMS it against the two bytes at the faulting address, services it, and
 *   patches the site on the way past (see the `#GP(IDT) is a RAW INT` arm in main.c).
 *   That patch needs no heuristic at all -- the CPU has just executed those two bytes
 *   AS an interrupt, which is the strongest possible evidence, and it is exactly what
 *   this vote has only ever been trying to approximate.
 *
 *   ⇒ So the costs have INVERTED:
 *       false accept -> silent code corruption, fatal, and hard to find
 *       false reject -> one extra #GP, serviced, then patched correctly and for good
 *   and the rule must follow the premise: WHEN IN DOUBT, REJECT.
 *
 * ► WHAT FORCED IT (session 39, GH #128).  krnl386 seg1 has, at 0x2051:
 *       3a cd     cmp cl,ch
 *       75 50     jne +0x50
 *   The `cd 75` spanning those two instructions is not an instruction; the vote for
 *   starting at 0x2052 failed, the owner was correctly named as the `cmp` -- and the
 *   `cmp` is not a relative branch, so the old rule KEPT it.  `cd 75` became `c4 c4`,
 *   krnl386's `jne` became `les dx,[bx+si+0x0b]`, and WOWEXEC died with
 *   "General Protection Fault in module KRNL386.EXE at 0001:2053" the moment it tried
 *   to launch an application.  Found by the method that found Doom's: when a guest
 *   dies at an address, diff the bytes there against the file on disk -- one byte
 *   differed, and the scan's own log line already named the offset it had patched.
 *
 * ► THE DOS/4GW SITE IS NOW REJECTED TOO, AND THAT IS THE INTENDED OUTCOME rather
 *   than a regression this rule tolerates: it is a real `int 21h`, it is left raw, the
 *   first execution faults, and the #GP(IDT) arm services and patches it.  One fault,
 *   once.
 * ⚠ THE ONE CASE THAT STILL NEEDS THE SCAN is a code selector whose base is 0: the
 *   #GP(IDT) arm guards on `gcb &&`, so it declines to service there.  A flat base-0
 *   code region is not scanned as a range either, so those sites were already raw
 *   before this change -- it makes nothing worse -- but that is the gap to close if a
 *   guest is ever seen dying on a raw INT after this.
 */
static int x86_int_site_is_real(const unsigned char *b, unsigned off, unsigned n, int d32)
{
    unsigned j;
    if (x86_is_insn_start(b, off, n, d32)) return 1;
    for (j = 1; j < 16u && j <= off; ++j) {
        unsigned len = x86_insn_len(b, off - j, n, d32);
        if (len > j && x86_is_insn_start(b, off - j, n, d32))
            return 0;               /* an instruction owns it -> it is an OPERAND */
    }
    return 1;                       /* nothing owns it -> keep */
}

#endif /* HOST_X86LEN_H */
