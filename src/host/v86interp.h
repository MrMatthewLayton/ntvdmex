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
 * instruction so V86 re-executes it. Address size is 16-bit only (0x67 and
 * LOCK bail); the 0x66 operand-size forms are modelled (run 54, and #194 for
 * the stack, string and control-transfer forms -- see the #194 note below).
 *
 * The includer MUST, before #include'ing this header, provide:
 *   - the fixed-width int types (uint8_t/uint16_t/uint32_t/int8_t/int16_t) + BYTE
 *   - uint8_t V86HostRead8(uint32_t lin);  void V86HostWrite8(uint32_t lin, uint8_t v);
 *     (guest byte read/write; A0000 reads must load the VGA latches.)
 *   - uint32_t V86HostIn(uint16_t port, int width);
 *     void     V86HostOut(uint16_t port, int width, uint32_t val);
 *     (port I/O dispatched to the device bus, for VGA-register-per-pixel loops.)
 * This keeps the interpreter host-agnostic so it can be unit-tested off-VM
 * against a flat memory array (see tests/unit/interp_test.c).
 */
#ifndef V86INTERP_H
#define V86INTERP_H
#include "../ntvdmex_types.h"
/* `int`, not INT, in this file: the spelling moved code in V86Step (#333). */

#define V86_GUEST_LIMIT 0x110000u                            /* 1MB + HMA: guest linear range */
#define V86_CF 0x0001u
#define V86_PF 0x0004u
#define V86_AF 0x0010u
#define V86_ZF 0x0040u
#define V86_SF 0x0080u
#define V86_DF 0x0400u
#define V86_OF 0x0800u

typedef struct _V86_CPU {
    UINT32 Registers[8];      /* E-AX/CX/DX/BX/SP/BP/SI/DI (full 32-bit; 16-/8-bit views mask) */
    WORD Segments[6];    /* ES CS SS DS FS GS       (x86 sreg encoding)  */
    WORD Ip;        /* address size stays 16-bit (the 0x67 prefix still bails)      */
    UINT32 Flags;
} V86_CPU, *PV86_CPU; typedef const V86_CPU *PCV86_CPU;

/* ── #183: THE HOT HELPERS ARE FORCED INLINE. (s87) ─────────────────────────────────
     A rig profile of mybench.com (CPU-bound, multi-plane mask) found V86Add, V86Subtract,
     V86Logic, V86Alu, grw, srw, V86ReadMemory, V86WriteMemory and V86DecodeModrm all compiled as SEPARATE
     functions: istep() is so large that GCC's large-function-growth limit refuses to
     inline anything more into it, so every interpreted `add si,3` paid about ten cdecl
     calls (stack args, i686). IINL overrides that for the small leaf helpers only. */
#if defined(__GNUC__)
#define V86_INLINE static inline __attribute__((always_inline))
#else
#define V86_INLINE static inline
#endif

/* PF = even parity of the low byte. 0x6996 is the 16-entry odd-parity table as a bit
   string: one fold to a nibble and a shift, instead of three folds. Same answer. */
V86_INLINE int V86Parity(BYTE value) { value ^= value >> 4; return !((0x6996u >> (value & 0xFu)) & 1u); }

/* Operand-width helpers: w is 1/2/4 bytes. The 4-byte path exists for 16-bit code
   that uses the 0x66 operand-size prefix (386 32-bit register math -- e.g. a C
   runtime's MOVZX ESI,SI / SHL ESI,4). run 54. */
V86_INLINE UINT32 V86Mask(int width) { return (width == 1) ? 0xFFu : (width == 2) ? 0xFFFFu : 0xFFFFFFFFu; }
V86_INLINE UINT32 V86SignBit(int width) { return (width == 1) ? 0x80u : (width == 2) ? 0x8000u : 0x80000000u; }

/* Segment-register -> linear-base resolver. NULL = V86 semantics (base = seg<<4).
   For protected mode (DPMI, GH #2) the host sets this to an LDT-selector->base
   lookup, so the SAME 16-bit interpreter core runs PM code -- descriptor bases
   instead of paragraph shifts. This is the one change that lets us execute PM in
   the host and never hand a faulting instruction to the kernel's VDM-fault path
   (which deadlocks on a PM #GP -- run 52). An interpreter enforces no descriptor
   type/limit, so e.g. a write through a code-typed SS (what #GP's the real CPU in
   run 51's I310102) simply succeeds here. */
static UINT32 (*g_V86SegmentToLinear)(WORD segment) = 0;
V86_INLINE UINT32 V86SegmentBase(WORD segment)
{ return g_V86SegmentToLinear ? g_V86SegmentToLinear(segment) : ((UINT32)segment << 4); }

/* Descriptor-introspection hook for PM (LAR/LSL, run 55). Given a selector, returns
   1 if it names a valid, accessible descriptor and fills *ar (access-rights in LAR
   format: access byte at bits 8-15, G/D/AVL nibble at 20-23) and *limit (byte-
   granular). 0 = invalid selector (the op then clears ZF, leaving the dest reg
   unchanged). NULL in V86 mode -> LAR/LSL bail (they don't occur there). The DPMI
   host sets this to read its g_ldt[] table. */
static int (*g_V86SelectorDescriptor)(WORD selector, UINT32 *accessRights, UINT32 *limit) = 0;

V86_INLINE UINT32 V86ReadMemory(UINT32 linear, int width)
{ UINT32 value = V86HostRead8(linear);
  if (width >= 2) value |= (UINT32)V86HostRead8(linear + 1) << 8;
  if (width == 4) value |= ((UINT32)V86HostRead8(linear + 2) << 16) | ((UINT32)V86HostRead8(linear + 3) << 24);
  return value; }
V86_INLINE VOID V86WriteMemory(UINT32 linear, int width, UINT32 value)
{ V86HostWrite8(linear, (BYTE)value);
  if (width >= 2) V86HostWrite8(linear + 1, (BYTE)(value >> 8));
  if (width == 4) { V86HostWrite8(linear + 2, (BYTE)(value >> 16)); V86HostWrite8(linear + 3, (BYTE)(value >> 24)); } }

/* ── #194: THE 0x66 FORMS THAT USED TO BAIL, AND THE TWO THINGS THE MANUAL LEAVES OPEN.
     PUSHFD/POPFD, 32-bit PUSH/POP of a segment register, the 32-bit string ops, CALL/
     JMP/RET/RETF/LEAVE/IRETD with 0x66, and IRET in protected mode all declined, each
     one a hand-back to the real CPU -- and in a planar mode a hand-back is not one
     instruction, it is everything up to the next event with A0000 unprotected (the
     s68 scasb lesson). Address size stays 16-bit throughout: SP/SI/DI/CX, never ESP.
   ► WHAT THE MANUAL LEAVES OPEN WAS MEASURED, NOT RECALLED -- tests/probes/dos/p_o32.com
     on the rig's own CPU under XP's V86 monitor, the machine this interpreter stands in
     for (runs/s87_dpmi; the same bytes are replayed through this file off-VM by
     interp_test.c against tests/unit/p_o32.ref.txt):
       - `66 PUSH sreg` writes the selector as a WORD: the slot's upper half is left as
         it was (sentinel DEAD survived). Intel allows either; this CPU keeps it.
       - but a 32-bit FAR CALL writes its CS slot as a DWORD, zero-extended (item 28:
         the 0x1234 left in that slot was cleared). Different instructions, different
         answers -- so they are two code paths here, not one helper.
       - `66 MOV r32,sreg` ZERO-EXTENDS into the register (EAX[31:16] = 0).
       - PUSHFD/POPFD and IRETD are not the CPU's answer at all in V86 below IOPL 3: they
         #GP and the NT kernel emulates them. Its PUSHFD image has VM (17) and RF (16)
         set and VIF (19) following the virtual IF; POPFD toggles AC (18) and ID (21) and
         they stick. See V86FlagsImage(). IRETD works (p_iretd.com).
   ► A TARGET PAST THE 64 KB LIMIT BAILS. With 0x66 a near transfer carries a 32-bit
     EIP; a 16-bit code segment's limit is FFFFh, so EIP > FFFFh is a #GP on the
     hardware. We hand that instruction back untouched and let the CPU raise it. */
#define V86I_EFL_AC   0x00040000u
#define V86I_EFL_ID   0x00200000u
#define V86I_EFL_HI   (V86I_EFL_AC | V86I_EFL_ID)  /* EFLAGS[31:16] bits POPFD may change */

/* PUSH/POP with an explicit width, SS:SP-relative (16-bit stack address size). */
static VOID V86Push(V86_CPU *cpu, int width, UINT32 value)
{
    WORD stackPointer = (WORD)(cpu->Registers[4] - width);
    V86WriteMemory(V86SegmentBase(cpu->Segments[2]) + stackPointer, width, value);
    cpu->Registers[4] = (cpu->Registers[4] & 0xFFFF0000u) | stackPointer;
}
static UINT32 V86Pop(V86_CPU *cpu, int width)
{
    WORD stackPointer = (WORD)cpu->Registers[4];
    UINT32 value = V86ReadMemory(V86SegmentBase(cpu->Segments[2]) + stackPointer, width);
    cpu->Registers[4] = (cpu->Registers[4] & 0xFFFF0000u) | (WORD)(stackPointer + width);
    return value;
}
/* PUSH of a segment register in a W-wide slot. W=4: a WORD store, SP -= 4, the slot's
   upper half untouched (measured -- see above). Not for a far CALL's CS: that one is a
   zero-extended dword (ipush(c, 4, cs)). */
static VOID V86PushSegment(V86_CPU *cpu, int width, WORD value)
{
    WORD stackPointer = (WORD)(cpu->Registers[4] - width);
    V86WriteMemory(V86SegmentBase(cpu->Segments[2]) + stackPointer, 2, value);
    cpu->Registers[4] = (cpu->Registers[4] & 0xFFFF0000u) | stackPointer;
}
/* The FLAGS image PUSHF(D) writes, and what POPF(D) may load. IF is carried (an
   interpreted handler's POPF/IRET must restore it); TF/IOPL/NT are not modelled.
   PUSHFD's upper half is the NT V86 monitor's (measured): AC/ID as they stand, and in
   V86 mode (no PM resolver) VM and RF always, VIF = the guest's IF. */
static UINT32 V86FlagsImage(const V86_CPU *cpu, int width)
{
    UINT32 value = (cpu->Flags & 0x0ED5u) | 0x0002u;
    if (width == 4) {
        value |= cpu->Flags & V86I_EFL_HI;
        if (!g_V86SegmentToLinear) value |= 0x00030000u | ((cpu->Flags & 0x0200u) ? 0x00080000u : 0u);
    }
    return value;
}
static VOID V86FlagsLoad(V86_CPU *cpu, UINT32 value, int width)
{
    UINT32 low = (value & 0x0ED5u) | 0x0002u;
    /* The 16-bit form REPLACES the whole register (upper half -> 0), as POPF and IRET
       always have here -- kept bit for bit, the fuzz pins it. The 32-bit form loads
       only the modelled upper bits and leaves the rest (VM, VIF ...) as they were. */
    if (width == 2) cpu->Flags = low;
    else        cpu->Flags = (cpu->Flags & ~(0xFFFFu | V86I_EFL_HI)) | (value & V86I_EFL_HI) | low;
}

/* A protected-mode far transfer the interpreter can FOLLOW: a present, 16-bit code
   segment at the caller's own privilege, with the offset inside its limit. Anything
   else -- a 32-bit target (this core decodes 16-bit code only), a ring change (which
   would also switch stacks), a bad selector -- is the real CPU's to take. */
static int V86IsFarTargetValid(const V86_CPU *cpu, WORD selector, UINT32 offset)
{
    UINT32 accessRights, limit;
    if (!g_V86SelectorDescriptor || !g_V86SelectorDescriptor(selector, &accessRights, &limit)) return 0;
    if (!(accessRights & 0x8000u) || !(accessRights & 0x0800u)) return 0;   /* present, code         */
    if (accessRights & (1u << 22)) return 0;                      /* D=1: a 32-bit segment */
    if ((selector & 3) != (cpu->Segments[1] & 3)) return 0;         /* privilege change      */
    return offset <= limit;
}

/* CPU register file access by x86 encoding. Sub-register writes preserve the bits
   they don't touch: a 16-bit write keeps E-reg[31:16]; an 8-bit write keeps the
   other 24 bits (x86 partial-register semantics). */
V86_INLINE WORD V86Get16(V86_CPU *cpu, int registerIndex) { return (WORD)cpu->Registers[registerIndex & 7]; }
V86_INLINE VOID     V86Set16(V86_CPU *cpu, int registerIndex, WORD value) { cpu->Registers[registerIndex & 7] = (cpu->Registers[registerIndex & 7] & 0xFFFF0000u) | value; }
V86_INLINE BYTE  V86Get8(V86_CPU *cpu, int registerIndex)
{ return (registerIndex < 4) ? (BYTE)cpu->Registers[registerIndex] : (BYTE)(cpu->Registers[registerIndex - 4] >> 8); }
V86_INLINE VOID     V86Set8(V86_CPU *cpu, int registerIndex, BYTE value)
{ if (registerIndex < 4) cpu->Registers[registerIndex] = (cpu->Registers[registerIndex] & 0xFFFFFF00u) | value;
  else cpu->Registers[registerIndex - 4] = (cpu->Registers[registerIndex - 4] & 0xFFFF00FFu) | ((UINT32)value << 8); }
/* read/write a register by operand width (1/2/4). */
V86_INLINE UINT32 V86GetRegister(V86_CPU *cpu, int registerIndex, int width)
{ return (width == 1) ? V86Get8(cpu, registerIndex) : (width == 2) ? (UINT32)(WORD)cpu->Registers[registerIndex & 7] : cpu->Registers[registerIndex & 7]; }
V86_INLINE VOID V86SetRegister(V86_CPU *cpu, int registerIndex, int width, UINT32 value)
{ if (width == 1) V86Set8(cpu, registerIndex, (BYTE)value); else if (width == 2) V86Set16(cpu, registerIndex, (WORD)value); else cpu->Registers[registerIndex & 7] = value; }

/* Flag-computing ALU primitives (result masked to operand width w). */
/* Carry without 64-bit arithmetic (i686 pays for every uint64_t): below 32 bits the
   sum cannot wrap a uint32_t, so CF is "the sum exceeds the mask"; at 32 bits it wrapped
   iff the result is below an operand (or equal to it with a carry in). SUB borrows iff
   a < b + cin, i.e. a <= b with a borrow in, a < b without. Flags are built in a local
   and stored once. Same answers as before -- scripts/interpfuzz.sh holds the digest. */
V86_INLINE UINT32 V86Add(V86_CPU *cpu, UINT32 first, UINT32 second, int carryIn, int width)
{
    UINT32 mask = V86Mask(width), signBit = V86SignBit(width);
    UINT32 maskedFirst = first & mask, maskedSecond = second & mask;
    UINT32 full = maskedFirst + maskedSecond + (UINT32)carryIn;
    UINT32 result = full & mask, newFlags = cpu->Flags & ~(V86_CF | V86_PF | V86_AF | V86_ZF | V86_SF | V86_OF);
    if (width == 4 ? (carryIn ? full <= maskedFirst : full < maskedFirst) : (full > mask)) newFlags |= V86_CF;
    if ((maskedFirst ^ maskedSecond ^ result) & 0x10u)                newFlags |= V86_AF;
    if (!result)                                   newFlags |= V86_ZF;
    if (result & signBit)                               newFlags |= V86_SF;
    if (V86Parity((BYTE)result))                  newFlags |= V86_PF;
    if ((~(maskedFirst ^ maskedSecond) & (maskedFirst ^ result)) & signBit)         newFlags |= V86_OF;
    cpu->Flags = newFlags;
    return result;
}
V86_INLINE UINT32 V86Subtract(V86_CPU *cpu, UINT32 first, UINT32 second, int borrowIn, int width)
{
    UINT32 mask = V86Mask(width), signBit = V86SignBit(width);
    UINT32 maskedFirst = first & mask, maskedSecond = second & mask, result = (maskedFirst - maskedSecond - (UINT32)borrowIn) & mask;
    UINT32 newFlags = cpu->Flags & ~(V86_CF | V86_PF | V86_AF | V86_ZF | V86_SF | V86_OF);
    if (borrowIn ? maskedFirst <= maskedSecond : maskedFirst < maskedSecond)               newFlags |= V86_CF;
    if ((maskedFirst ^ maskedSecond ^ result) & 0x10u)                newFlags |= V86_AF;
    if (!result)                                   newFlags |= V86_ZF;
    if (result & signBit)                               newFlags |= V86_SF;
    if (V86Parity((BYTE)result))                  newFlags |= V86_PF;
    if (((maskedFirst ^ maskedSecond) & (maskedFirst ^ result)) & signBit)          newFlags |= V86_OF;
    cpu->Flags = newFlags;
    return result;
}
V86_INLINE VOID V86Logic(V86_CPU *cpu, UINT32 result, int width)
{
    UINT32 mask = V86Mask(width), signBit = V86SignBit(width);
    UINT32 newFlags = cpu->Flags & ~(V86_CF | V86_PF | V86_AF | V86_ZF | V86_SF | V86_OF);   /* CF=OF=0 */
    result &= mask;
    if (!result)                  newFlags |= V86_ZF;
    if (result & signBit)              newFlags |= V86_SF;
    if (V86Parity((BYTE)result)) newFlags |= V86_PF;
    cpu->Flags = newFlags;
}

/* aluop encoding 0..7 = ADD OR ADC SBB AND SUB XOR CMP. Returns result;
   CMP (7) computes flags only. */
V86_INLINE UINT32 V86Alu(V86_CPU *cpu, int operation, UINT32 first, UINT32 second, int width)
{
    switch (operation) {
    case 0: return V86Add(cpu, first, second, 0, width);
    case 1: { UINT32 result = first | second; V86Logic(cpu, result, width); return result; }
    case 2: return V86Add(cpu, first, second, (cpu->Flags & V86_CF) ? 1 : 0, width);
    case 3: return V86Subtract(cpu, first, second, (cpu->Flags & V86_CF) ? 1 : 0, width);
    case 4: { UINT32 result = first & second; V86Logic(cpu, result, width); return result; }
    case 5: return V86Subtract(cpu, first, second, 0, width);
    case 6: { UINT32 result = first ^ second; V86Logic(cpu, result, width); return result; }
    default: V86Subtract(cpu, first, second, 0, width); return 0;             /* CMP: no store */
    }
}

/* Shift/rotate group-2 (sub 0..7 = ROL ROR RCL RCR SHL SHR SAL SAR), done one
   bit at a time so CF tracks correctly. Rotates touch only CF/OF; shifts also
   set SF/ZF/PF. OF is defined only for count 1. QuickBasic builds the mode-12h
   bit-mask with SHR (0x80 >> x) / SHL, so this is on the hot pixel path. */
static UINT32 V86ShiftRotate(V86_CPU *cpu, int operation, UINT32 value, int count, int width)
{
    UINT32 mask = V86Mask(width), signBit = V86SignBit(width), original;
    int carry = (cpu->Flags & V86_CF) ? 1 : 0, oldCarry, index;
    count &= 0x1F; value &= mask; original = value;
    if (count == 0) return value;                            /* x86: flags unchanged */
    for (index = 0; index < count; ++index) switch (operation) {
        case 0: carry = (value & signBit) ? 1 : 0; value = ((value << 1) | (UINT32)carry) & mask; break;          /* ROL */
        case 1: carry = value & 1; value = ((value >> 1) | (carry ? signBit : 0)) & mask; break;                     /* ROR */
        case 2: oldCarry = carry; carry = (value & signBit) ? 1 : 0; value = ((value << 1) | (UINT32)oldCarry) & mask; break; /* RCL */
        case 3: oldCarry = carry; carry = value & 1; value = ((value >> 1) | (oldCarry ? signBit : 0)) & mask; break;      /* RCR */
        case 4: case 6: carry = (value & signBit) ? 1 : 0; value = (value << 1) & mask; break;                    /* SHL/SAL */
        case 5: carry = value & 1; value = (value >> 1) & mask; break;                                       /* SHR */
        default: carry = value & 1; value = ((value >> 1) | (value & signBit)) & mask; break;                         /* SAR */
    }
    cpu->Flags = (cpu->Flags & ~V86_CF) | (carry ? V86_CF : 0);
    if (count == 1) {                                    /* OF only defined for count 1 */
        int overflow;
        switch (operation) {
        case 5:  overflow = (original & signBit) ? 1 : 0; break;                      /* SHR: MSB of orig */
        case 7:  overflow = 0; break;                                        /* SAR */
        case 1: case 3: overflow = (((value & signBit) ? 1 : 0) ^ ((value & (signBit >> 1)) ? 1 : 0)); break; /* ROR/RCR */
        default: overflow = (((value & signBit) ? 1 : 0) ^ carry); break;                /* ROL/RCL/SHL */
        }
        cpu->Flags = (cpu->Flags & ~V86_OF) | (overflow ? V86_OF : 0);
    }
    if (operation >= 4) {                                    /* shifts set SF/ZF/PF (not rotates) */
        cpu->Flags &= ~(V86_SF | V86_ZF | V86_PF);
        if (!value) cpu->Flags |= V86_ZF;
        if (value & signBit) cpu->Flags |= V86_SF;
        if (V86Parity((BYTE)value)) cpu->Flags |= V86_PF;
    }
    return value;
}

V86_INLINE int V86Condition(V86_CPU *cpu, int condition)
{
    int carry = !!(cpu->Flags & V86_CF), zero = !!(cpu->Flags & V86_ZF), sign = !!(cpu->Flags & V86_SF),
        overflow = !!(cpu->Flags & V86_OF), parity = !!(cpu->Flags & V86_PF);
    switch (condition & 0xF) {
    case 0x0: return overflow;            case 0x1: return !overflow;
    case 0x2: return carry;            case 0x3: return !carry;
    case 0x4: return zero;            case 0x5: return !zero;
    case 0x6: return carry || zero;      case 0x7: return !(carry || zero);
    case 0x8: return sign;            case 0x9: return !sign;
    case 0xA: return parity;            case 0xB: return !parity;
    case 0xC: return sign != overflow;      case 0xD: return sign == overflow;
    case 0xE: return zero || (sign != overflow);
    default:  return !(zero || (sign != overflow));
    }
}

/* Code-stream byte fetch: routed through V86HostRead8 (NOT a raw pointer) so the
   interpreter is fully memory-abstracted -- identical on the V86 host (code is
   never in the A0000 window, so V86HostRead8 returns the mapped byte) and testable
   off-VM against a flat array. `cb` is the linear address of CS:IP.
   ── #183 (s87): ...AND, WHEN THE HOST SAYS IT IS SAFE, THROUGH A POINTER. ──────────
     A rig profile put V86HostRead8's own range/page checks at ~12% of the interpreter: they
     ran once per code BYTE. An includer that defines V86I_CODE_PTR provides
       const volatile BYTE *V86HostCodePointer(uint32_t lin);
     = a pointer good for the 16 bytes at `lin` (one plain-RAM page, never the A0000
     aperture -- a read there loads the VGA latches, so it must stay a call), or NULL.
     istep asks once per instruction; NULL falls back to V86HostRead8 byte by byte, exactly
     as before. Without the macro `cp` is a constant NULL and the compiler drops it. */
#ifdef V86I_CODE_PTR
#define V86_CODE(lin) V86HostCodePointer(lin)
#else
#define V86_CODE(lin) ((const volatile BYTE *)0)
#endif
#define V86_CODE_BYTE(position) (codePointer ? codePointer[(position)] : V86HostRead8(codeLinear + (UINT32)(position)))
/* An immediate of `width` bytes at V86_CODE_BYTE(position): little-endian, the same bytes as V86ReadMemory(codeLinear + position, width). */
#define V86_IMMEDIATE(position, width) (codePointer ? (UINT32)codePointer[(position)] \
                          | ((width) >= 2 ? (UINT32)codePointer[(position) + 1] << 8 : 0u) \
                          | ((width) == 4 ? ((UINT32)codePointer[(position) + 2] << 16) | ((UINT32)codePointer[(position) + 3] << 24) : 0u) \
                        : V86ReadMemory(codeLinear + (UINT32)(position), (width)))

/* Decode a 16-bit ModRM byte at CB(idx). Fills *o (is_mem + linear addr or rm
   register, plus the reg field g). Returns bytes consumed (ModRM + disp). */
typedef struct _V86_MODRM { int IsMemory; UINT32 Linear; WORD EffectiveAddress; int Register; int RegisterMemory; } V86_MODRM;
static int V86DecodeModrm(V86_CPU *cpu, UINT32 codeLinear, const volatile BYTE *codePointer, int offset, int segmentOverride, V86_MODRM *out)
{
    BYTE modrmByte = V86_CODE_BYTE(offset); int mode = modrmByte >> 6, registerMemory = modrmByte & 7, length = 1, isStackBased = 0;
    WORD effectiveAddress = 0, bxValue = cpu->Registers[3], bpValue = cpu->Registers[5], siValue = cpu->Registers[6], diValue = cpu->Registers[7];
    out->Register = (modrmByte >> 3) & 7;
    if (mode == 3) { out->IsMemory = 0; out->RegisterMemory = registerMemory; out->EffectiveAddress = 0; return 1; }
    switch (registerMemory) {
    case 0: effectiveAddress = (WORD)(bxValue + siValue); break;  case 1: effectiveAddress = (WORD)(bxValue + diValue); break;
    case 2: effectiveAddress = (WORD)(bpValue + siValue); isStackBased = 1; break;
    case 3: effectiveAddress = (WORD)(bpValue + diValue); isStackBased = 1; break;
    case 4: effectiveAddress = siValue; break;                   case 5: effectiveAddress = diValue; break;
    case 6: if (mode == 0) { effectiveAddress = (WORD)(V86_CODE_BYTE(offset + 1) | (V86_CODE_BYTE(offset + 2) << 8)); length += 2; }
            else { effectiveAddress = bpValue; isStackBased = 1; } break;
    default: effectiveAddress = bxValue; break;
    }
    if (mode == 1)      { effectiveAddress = (WORD)(effectiveAddress + (INT16)(signed char)V86_CODE_BYTE(offset + length)); length += 1; }
    else if (mode == 2) { effectiveAddress = (WORD)(effectiveAddress + (V86_CODE_BYTE(offset + length) | (V86_CODE_BYTE(offset + length + 1) << 8))); length += 2; }
    out->IsMemory = 1; out->EffectiveAddress = effectiveAddress;
    out->Linear = (V86SegmentBase(cpu->Segments[(segmentOverride >= 0) ? segmentOverride : (isStackBased ? 2 : 3)])) + effectiveAddress;  /* SS if BP else DS */
    return length;
}

/* Where the guest is, for instruments downstream of a memory access. Every planar
   VRAM write reaches the video VDD through this interpreter, so a watchpoint there
   can name the guest routine responsible -- which is the difference between "some
   idiom wrote 0xFF" and an address to disassemble. One store per instruction. */
static UINT32 g_V86InstructionPointer;

/* Execute one instruction. Returns 1 if modeled (state + IP advanced/jumped),
   0 to bail (state untouched at the current instruction). */
static int V86Step(V86_CPU *cpu)
{
    UINT32 codeLinear = (V86SegmentBase(cpu->Segments[1])) + cpu->Ip;   /* linear CS:IP */
    const volatile BYTE *codePointer = V86_CODE(codeLinear);              /* NULL = fetch through V86HostRead8 */
    g_V86InstructionPointer = ((UINT32)cpu->Segments[1] << 16) | cpu->Ip;
    int offset = 0, segmentOverride = -1, repeat = 0, isOperand32 = 0;
    int operandSize;                                            /* word operand width: 4 if 0x66 else 2 */
    BYTE opcode;
    for (;;) {                                        /* prefixes */
        BYTE prefix = V86_CODE_BYTE(offset);
        if      (prefix == 0x26) { segmentOverride = 0; offset++; }     /* ES */
        else if (prefix == 0x2E) { segmentOverride = 1; offset++; }     /* CS */
        else if (prefix == 0x36) { segmentOverride = 2; offset++; }     /* SS */
        else if (prefix == 0x3E) { segmentOverride = 3; offset++; }     /* DS */
        else if (prefix == 0x64) { segmentOverride = 4; offset++; }     /* FS */
        else if (prefix == 0x65) { segmentOverride = 5; offset++; }     /* GS */
        else if (prefix == 0xF3) { repeat = 1; offset++; }
        else if (prefix == 0xF2) { repeat = 2; offset++; }
        else if (prefix == 0x66) { isOperand32 = 1; offset++; }       /* run 54: operand-size -> 32-bit */
        else if (prefix == 0x67 || prefix == 0xF0) return 0;    /* addr-size / LOCK: still bail */
        else break;
        if (offset > 4) return 0;
    }
    opcode = V86_CODE_BYTE(offset++);
    operandSize = isOperand32 ? 4 : 2;                                  /* run 54: word operand width (0x66 -> 4) */

    /* ---- 0F two-byte map (run 54): MOVZX/MOVSX r, r/m -------------------- *
     * B6/B7 = zero-extend byte/word, BE/BF = sign-extend. Dest width = W, so *
     * `66 0F B7` gives MOVZX ESI,SI. Other 0F ops bail (the to-do signal).   */
    if (opcode == 0x0F) {
        BYTE opcode2 = V86_CODE_BYTE(offset++);
        if (opcode2 == 0xB6 || opcode2 == 0xB7 || opcode2 == 0xBE || opcode2 == 0xBF) {
            int sourceWidth = (opcode2 & 1) ? 2 : 1;               /* source width */
            int isSignExtend = (opcode2 >= 0xBE);                   /* sign- vs zero-extend */
            V86_MODRM modrm; offset += V86DecodeModrm(cpu, codeLinear, codePointer, offset, segmentOverride, &modrm);
            if (modrm.IsMemory && modrm.Linear >= V86_GUEST_LIMIT) return 0;
            { UINT32 value = modrm.IsMemory ? V86ReadMemory(modrm.Linear, sourceWidth)
                                    : (sourceWidth == 1 ? V86Get8(cpu, modrm.RegisterMemory) : (UINT32)V86Get16(cpu, modrm.RegisterMemory));
              if (isSignExtend && (value & V86SignBit(sourceWidth))) value |= ~V86Mask(sourceWidth);   /* sign-extend to 32 */
              V86SetRegister(cpu, modrm.Register, operandSize, value & V86Mask(operandSize)); }
            cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
        }
        if (opcode2 == 0x02 || opcode2 == 0x03) {             /* LAR / LSL r, r/m16 (run 55) */
            V86_MODRM modrm; WORD selector; UINT32 accessRights, limit;
            if (!g_V86SelectorDescriptor) return 0;                /* V86: no descriptor table -> bail */
            offset += V86DecodeModrm(cpu, codeLinear, codePointer, offset, segmentOverride, &modrm);
            if (modrm.IsMemory && modrm.Linear >= V86_GUEST_LIMIT) return 0;
            selector = modrm.IsMemory ? (WORD)V86ReadMemory(modrm.Linear, 2) : V86Get16(cpu, modrm.RegisterMemory);  /* selector is 16-bit */
            if (g_V86SelectorDescriptor(selector, &accessRights, &limit)) {         /* valid -> load rights/limit, set ZF */
                V86SetRegister(cpu, modrm.Register, operandSize, (opcode2 == 0x02 ? accessRights : limit) & V86Mask(operandSize));
                cpu->Flags |= V86_ZF;
            } else cpu->Flags &= ~V86_ZF;                 /* invalid -> clear ZF, dest unchanged */
            cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
        }
        /* PUSH/POP FS and GS (0F A0/A1/A8/A9), either width -- #194. The 16-bit form
           was unmodelled too: the 386 encodings live only in the 0F map. */
        if (opcode2 == 0xA0 || opcode2 == 0xA8) {
            V86PushSegment(cpu, operandSize, cpu->Segments[opcode2 == 0xA0 ? 4 : 5]);
            cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
        }
        if (opcode2 == 0xA1 || opcode2 == 0xA9) {
            cpu->Segments[opcode2 == 0xA1 ? 4 : 5] = (WORD)V86Pop(cpu, operandSize);
            cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
        }
        /* #269: Jcc rel16 (0F 80-8F) -- the near form 386-targeted 16-bit code uses for
           any branch past 127 bytes. 16-bit operand size: IP = next + rel16, wrapping
           in the segment as IP does. With 0x66 it is rel32 and EIP = next + rel32, bailed
           if it leaves the 64 KB segment (the CPU's #GP, not ours) -- the E9 rule. */
        if (opcode2 >= 0x80 && opcode2 <= 0x8F) {
            int isTaken = V86Condition(cpu, opcode2 & 0xF);
            if (isOperand32) {
                UINT32 nextIp = (UINT32)(WORD)(cpu->Ip + offset + 4);
                UINT32 target = nextIp + V86ReadMemory(codeLinear + offset, 4);
                if (isTaken && target > 0xFFFFu) return 0;
                cpu->Ip = (WORD)(isTaken ? target : nextIp); return 1;
            } else {
                INT16 relative = (INT16)(V86_CODE_BYTE(offset) | (V86_CODE_BYTE(offset + 1) << 8)); offset += 2;
                cpu->Ip = (WORD)(cpu->Ip + offset + (isTaken ? relative : 0)); return 1;
            }
        }
        /* #269: SETcc r/m8 (0F 90-9F) -- 1 if the condition holds, else 0. The reg
           field of the ModR/M is not used; no flags change. */
        if (opcode2 >= 0x90 && opcode2 <= 0x9F) {
            V86_MODRM modrm; BYTE value = (BYTE)(V86Condition(cpu, opcode2 & 0xF) ? 1 : 0);
            offset += V86DecodeModrm(cpu, codeLinear, codePointer, offset, segmentOverride, &modrm);
            if (modrm.IsMemory && modrm.Linear >= V86_GUEST_LIMIT) return 0;
            if (modrm.IsMemory) V86WriteMemory(modrm.Linear, 1, value);
            else          V86Set8(cpu, modrm.RegisterMemory, value);
            cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
        }
        return 0;                                     /* other 0F ops: bail */
    }

    /* ---- arithmetic/logic group: ADD..CMP, reg/mem forms ------------------ */
    if (opcode < 0x40 && (opcode & 7) < 6) {
        int aluOperation = (opcode >> 3) & 7, form = opcode & 7;
        int width = (form == 0 || form == 2 || form == 4) ? 1 : operandSize;
        UINT32 first, second, result; int isDestinationMemory = 0, destinationRegister = 0; UINT32 destinationLinear = 0;
        if (form <= 3) {
            V86_MODRM modrm; offset += V86DecodeModrm(cpu, codeLinear, codePointer, offset, segmentOverride, &modrm);
            if (modrm.IsMemory && modrm.Linear >= V86_GUEST_LIMIT) return 0;
            UINT32 rmValue = modrm.IsMemory ? V86ReadMemory(modrm.Linear, width) : V86GetRegister(cpu, modrm.RegisterMemory, width);
            UINT32 registerValue = V86GetRegister(cpu, modrm.Register, width);
            if (form <= 1) { first = rmValue; second = registerValue; if (modrm.IsMemory) { isDestinationMemory = 1; destinationLinear = modrm.Linear; } else destinationRegister = modrm.RegisterMemory; }
            else           { first = registerValue; second = rmValue; destinationRegister = modrm.Register; }
        } else if (form == 4) { first = V86Get8(cpu, 0);  second = V86_CODE_BYTE(offset++); destinationRegister = 0; }
        else { first = V86GetRegister(cpu, 0, width); second = V86_IMMEDIATE(offset, width); offset += width; destinationRegister = 0; }
        result = V86Alu(cpu, aluOperation, first, second, width);
        if (aluOperation != 7) {
            if (isDestinationMemory) V86WriteMemory(destinationLinear, width, result);
            else      V86SetRegister(cpu, destinationRegister, width, result);
        }
        cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
    }

    /* ---- group1: ADD..CMP r/m, imm (80/81/83) ----------------------------- */
    if (opcode == 0x80 || opcode == 0x81 || opcode == 0x83) {
        int width = (opcode == 0x80) ? 1 : operandSize; UINT32 first, second, result;
        V86_MODRM modrm; offset += V86DecodeModrm(cpu, codeLinear, codePointer, offset, segmentOverride, &modrm);
        if (modrm.IsMemory && modrm.Linear >= V86_GUEST_LIMIT) return 0;
        first = modrm.IsMemory ? V86ReadMemory(modrm.Linear, width) : V86GetRegister(cpu, modrm.RegisterMemory, width);
        if (opcode == 0x81) { second = V86_IMMEDIATE(offset, width); offset += width; }
        else { second = (UINT32)(INT32)(INT8)V86_CODE_BYTE(offset++); second &= V86Mask(width); }
        result = V86Alu(cpu, modrm.Register, first, second, width);
        if (modrm.Register != 7) {
            if (modrm.IsMemory) V86WriteMemory(modrm.Linear, width, result);
            else          V86SetRegister(cpu, modrm.RegisterMemory, width, result);
        }
        cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
    }

    /* ---- INC/DEC r16/r32 (40-4F) ------------------------------------------ */
    if (opcode >= 0x40 && opcode <= 0x4F) {
        int registerIndex = opcode & 7; UINT32 cf = cpu->Flags & V86_CF;
        UINT32 result = (opcode >= 0x48) ? V86Subtract(cpu, V86GetRegister(cpu, registerIndex, operandSize), 1, 0, operandSize)
                                    : V86Add(cpu, V86GetRegister(cpu, registerIndex, operandSize), 1, 0, operandSize);
        cpu->Flags = (cpu->Flags & ~V86_CF) | cf;           /* INC/DEC preserve CF */
        V86SetRegister(cpu, registerIndex, operandSize, result);
        cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
    }

    /* ---- group FE/FF: INC/DEC r/m, and (FF only) near indirect CALL/JMP +
            PUSH r/m. Far call/jmp (g=3/5) bail. ---------------------------- */
    if (opcode == 0xFE || opcode == 0xFF) {
        int width = (opcode == 0xFE) ? 1 : operandSize;
        V86_MODRM modrm; offset += V86DecodeModrm(cpu, codeLinear, codePointer, offset, segmentOverride, &modrm);
        if (modrm.IsMemory && modrm.Linear >= V86_GUEST_LIMIT) return 0;
        if (modrm.Register == 0 || modrm.Register == 1) {                   /* INC/DEC r/m */
            UINT32 cf = cpu->Flags & V86_CF;
            UINT32 first = modrm.IsMemory ? V86ReadMemory(modrm.Linear, width) : V86GetRegister(cpu, modrm.RegisterMemory, width);
            UINT32 result = (modrm.Register == 1) ? V86Subtract(cpu, first, 1, 0, width) : V86Add(cpu, first, 1, 0, width);
            cpu->Flags = (cpu->Flags & ~V86_CF) | cf;
            if (modrm.IsMemory) V86WriteMemory(modrm.Linear, width, result);
            else          V86SetRegister(cpu, modrm.RegisterMemory, width, result);
            cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
        }
        if (opcode != 0xFF) return 0;                      /* FE has nothing past INC/DEC */
        if (isOperand32) {
            /* #194: the 32-bit forms. CALL/JMP near take a 32-bit EIP (bail past the
               64 KB limit -- see the #194 note at the top); PUSH r/m32; and CALL/JMP
               FAR m16:32 -- offset dword, then the selector -- real mode only, as the
               16-bit far forms below are. */
            UINT32 value32 = modrm.IsMemory ? V86ReadMemory(modrm.Linear, 4) : cpu->Registers[modrm.RegisterMemory & 7];
            UINT32 nextIp = (UINT32)(WORD)(cpu->Ip + offset);
            if (modrm.Register == 2 || modrm.Register == 4) {
                if (value32 > 0xFFFFu) return 0;
                if (modrm.Register == 2) V86Push(cpu, 4, nextIp);
                cpu->Ip = (WORD)value32; return 1;
            }
            if (modrm.Register == 6) { V86Push(cpu, 4, value32); cpu->Ip = (WORD)nextIp; return 1; }
            if ((modrm.Register == 3 || modrm.Register == 5) && modrm.IsMemory && !g_V86SegmentToLinear) {
                WORD segment = (WORD)V86ReadMemory(modrm.Linear + 4, 2);
                if (value32 > 0xFFFFu) return 0;
                if (modrm.Register == 3) { V86Push(cpu, 4, cpu->Segments[1]); V86Push(cpu, 4, nextIp); }  /* CS slot: dword */
                cpu->Segments[1] = segment; cpu->Ip = (WORD)value32; return 1;
            }
            return 0;
        }
        { WORD value16 = modrm.IsMemory ? (WORD)V86ReadMemory(modrm.Linear, 2) : V86Get16(cpu, modrm.RegisterMemory);
          WORD nextIp = (WORD)(cpu->Ip + offset);
          if (modrm.Register == 2) {                              /* CALL near indirect */
              WORD stackPointer = (WORD)(cpu->Registers[4] - 2);
              V86WriteMemory((V86SegmentBase(cpu->Segments[2])) + stackPointer, 2, nextIp);
              cpu->Registers[4] = stackPointer; cpu->Ip = value16; return 1;
          }
          if (modrm.Register == 4) { cpu->Ip = value16; return 1; }     /* JMP near indirect */
          if (modrm.Register == 6) {                              /* PUSH r/m16 */
              WORD stackPointer = (WORD)(cpu->Registers[4] - 2);
              V86WriteMemory((V86SegmentBase(cpu->Segments[2])) + stackPointer, 2, value16);
              cpu->Registers[4] = stackPointer; cpu->Ip = nextIp; return 1;
          }
          /* CALL FAR m16:16 (/3) and JMP FAR m16:16 (/5): the seg:off is IN MEMORY.
             (s68) Lemmings dispatches through `jmp far [0x1fbe]` on every frame and
             this bailed to V86 -- and in a planar mode a bail is not one instruction,
             it is everything up to the next event, with A0000 unprotected. Same shape
             as the scasb leak; same fix. Real mode only, like EA/9A above. */
          if ((modrm.Register == 3 || modrm.Register == 5) && modrm.IsMemory && !g_V86SegmentToLinear) {
              WORD segment = (WORD)V86ReadMemory(modrm.Linear + 2, 2);
              if (modrm.Register == 3) {
                  UINT32 stackBase = V86SegmentBase(cpu->Segments[2]);
                  WORD stackPointer = (WORD)(cpu->Registers[4] - 2); V86WriteMemory(stackBase + stackPointer, 2, cpu->Segments[1]);
                  stackPointer = (WORD)(stackPointer - 2);               V86WriteMemory(stackBase + stackPointer, 2, nextIp);
                  cpu->Registers[4] = (cpu->Registers[4] & 0xFFFF0000u) | stackPointer;
              }
              cpu->Segments[1] = segment; cpu->Ip = value16; return 1;
          } }
        return 0;                                      /* g=7 / reg-form far: bail */
    }

    /* ---- TEST r/m,r (84/85); TEST AL/AX,imm (A8/A9) ----------------------- */
    if (opcode == 0x84 || opcode == 0x85) {
        int width = (opcode == 0x84) ? 1 : operandSize;
        V86_MODRM modrm; offset += V86DecodeModrm(cpu, codeLinear, codePointer, offset, segmentOverride, &modrm);
        if (modrm.IsMemory && modrm.Linear >= V86_GUEST_LIMIT) return 0;
        { UINT32 operand = modrm.IsMemory ? V86ReadMemory(modrm.Linear, width) : V86GetRegister(cpu, modrm.RegisterMemory, width);
          UINT32 registerValue = V86GetRegister(cpu, modrm.Register, width);
          V86Logic(cpu, operand & registerValue, width); }
        cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
    }
    if (opcode == 0xA8) { V86Logic(cpu, (UINT32)V86Get8(cpu, 0) & V86_CODE_BYTE(offset), 1); offset++;
                      cpu->Ip = (WORD)(cpu->Ip + offset); return 1; }
    if (opcode == 0xA9) { UINT32 second = V86_IMMEDIATE(offset, operandSize); offset += operandSize;
                      V86Logic(cpu, V86GetRegister(cpu, 0, operandSize) & second, operandSize);
                      cpu->Ip = (WORD)(cpu->Ip + offset); return 1; }
    /* ---- group3 (F6/F7): TEST r/m,imm (reg 0/1); NOT/NEG (2/3); MUL/IMUL     *
     * (4/5) -> [E]DX:[E]AX; DIV/IDIV (6/7) <- [E]DX:[E]AX. run 59's I310102     *
     * reached `66 F7 /6` = DIV EDI (printf's hex-digit divide loop). No #DE     *
     * trap path here, so a zero divisor or quotient overflow BAILS to V86       *
     * rather than emit UB (correct code never hits it). run 61. */
    if (opcode == 0xF6 || opcode == 0xF7) {
        int width = (opcode == 0xF6) ? 1 : operandSize;
        V86_MODRM modrm; offset += V86DecodeModrm(cpu, codeLinear, codePointer, offset, segmentOverride, &modrm);
        if (modrm.IsMemory && modrm.Linear >= V86_GUEST_LIMIT) return 0;
        if (modrm.Register == 0 || modrm.Register == 1) {                    /* TEST r/m,imm */
            UINT32 operand = modrm.IsMemory ? V86ReadMemory(modrm.Linear, width) : V86GetRegister(cpu, modrm.RegisterMemory, width);
            UINT32 second = V86_IMMEDIATE(offset, width); offset += width;
            V86Logic(cpu, operand & second, width);
            cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
        }
        { UINT32 operand = modrm.IsMemory ? V86ReadMemory(modrm.Linear, width) : V86GetRegister(cpu, modrm.RegisterMemory, width);
          if (modrm.Register == 2) {                              /* NOT: no flags */
              UINT32 result = (~operand) & V86Mask(width);
              if (modrm.IsMemory) V86WriteMemory(modrm.Linear, width, result); else V86SetRegister(cpu, modrm.RegisterMemory, width, result);
          } else if (modrm.Register == 3) {                       /* NEG: 0 - e, flags like SUB */
              UINT32 result = V86Subtract(cpu, 0, operand, 0, width);
              if (modrm.IsMemory) V86WriteMemory(modrm.Linear, width, result); else V86SetRegister(cpu, modrm.RegisterMemory, width, result);
          } else if (modrm.Register == 4 || modrm.Register == 5) {           /* MUL (4) / IMUL (5) */
              int isOverflow;
              if (width == 1) {
                  UINT32 product = (modrm.Register == 4)
                      ? (cpu->Registers[0] & 0xFFu) * (operand & 0xFFu)
                      : (UINT32)(INT32)((INT8)(cpu->Registers[0] & 0xFF) * (INT8)operand) & 0xFFFFu;
                  cpu->Registers[0] = (cpu->Registers[0] & 0xFFFF0000u) | (product & 0xFFFFu);
                  isOverflow = (modrm.Register == 4) ? ((product >> 8) != 0)
                                   : ((INT16)product != (INT8)(product & 0xFF));
              } else if (width == 2) {
                  UINT32 product = (modrm.Register == 4)
                      ? (cpu->Registers[0] & 0xFFFFu) * (operand & 0xFFFFu)
                      : (UINT32)(INT32)((INT16)(cpu->Registers[0] & 0xFFFF) * (INT16)operand);
                  cpu->Registers[0] = (cpu->Registers[0] & 0xFFFF0000u) | (product & 0xFFFFu);
                  cpu->Registers[2] = (cpu->Registers[2] & 0xFFFF0000u) | ((product >> 16) & 0xFFFFu);
                  isOverflow = (modrm.Register == 4) ? ((product >> 16) != 0)
                                   : ((INT32)product != (INT16)(product & 0xFFFF));
              } else {                                  /* w == 4 */
                  UINT64 product = (modrm.Register == 4)
                      ? (UINT64)cpu->Registers[0] * (UINT64)operand
                      : (UINT64)((INT64)(INT32)cpu->Registers[0] * (INT64)(INT32)operand);
                  cpu->Registers[0] = (UINT32)product;
                  cpu->Registers[2] = (UINT32)(product >> 32);
                  isOverflow = (modrm.Register == 4) ? ((product >> 32) != 0)
                                   : ((INT64)product != (INT32)product);
              }
              cpu->Flags = (cpu->Flags & ~(V86_CF | V86_OF)) | (isOverflow ? (V86_CF | V86_OF) : 0);
          } else {                                     /* m.g == 6 DIV / 7 IDIV */
              if (operand == 0) return 0;                     /* #DE (div by zero): bail */
              if (width == 1) {
                  if (modrm.Register == 6) { UINT32 dividend = cpu->Registers[0] & 0xFFFFu, quotient = dividend / (operand & 0xFFu), remainder = dividend % (operand & 0xFFu);
                      if (quotient > 0xFF) return 0;            /* #DE quotient overflow */
                      cpu->Registers[0] = (cpu->Registers[0] & 0xFFFF0000u) | (quotient & 0xFF) | ((remainder & 0xFF) << 8); }
                  else { INT16 dividend = (INT16)(cpu->Registers[0] & 0xFFFF); INT8 divisor = (INT8)operand;
                      INT32 quotient = dividend / divisor, remainder = dividend % divisor; if (quotient > 127 || quotient < -128) return 0;
                      cpu->Registers[0] = (cpu->Registers[0] & 0xFFFF0000u) | (quotient & 0xFF) | ((remainder & 0xFF) << 8); }
              } else if (width == 2) {
                  UINT32 dividend = ((cpu->Registers[2] & 0xFFFFu) << 16) | (cpu->Registers[0] & 0xFFFFu);
                  if (modrm.Register == 6) { UINT32 quotient = dividend / (operand & 0xFFFFu), remainder = dividend % (operand & 0xFFFFu);
                      if (quotient > 0xFFFF) return 0;
                      cpu->Registers[0] = (cpu->Registers[0] & 0xFFFF0000u) | (quotient & 0xFFFF);
                      cpu->Registers[2] = (cpu->Registers[2] & 0xFFFF0000u) | (remainder & 0xFFFF); }
                  else { INT32 signedDividend = (INT32)dividend, divisor = (INT16)operand, quotient = signedDividend / divisor, remainder = signedDividend % divisor;
                      if (quotient > 32767 || quotient < -32768) return 0;
                      cpu->Registers[0] = (cpu->Registers[0] & 0xFFFF0000u) | (quotient & 0xFFFF);
                      cpu->Registers[2] = (cpu->Registers[2] & 0xFFFF0000u) | (remainder & 0xFFFF); }
              } else {                                  /* w == 4 */
                  UINT64 dividend = ((UINT64)cpu->Registers[2] << 32) | (UINT64)cpu->Registers[0];
                  if (modrm.Register == 6) { UINT64 quotient = dividend / operand, remainder = dividend % operand;
                      if (quotient > 0xFFFFFFFFu) return 0;
                      cpu->Registers[0] = (UINT32)quotient; cpu->Registers[2] = (UINT32)remainder; }
                  else { INT64 signedDividend = (INT64)dividend, divisor = (INT32)operand, quotient = signedDividend / divisor, remainder = signedDividend % divisor;
                      if (quotient > 2147483647LL || quotient < -2147483648LL) return 0;
                      cpu->Registers[0] = (UINT32)quotient; cpu->Registers[2] = (UINT32)remainder; }
              }
          }
        }
        cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
    }

    /* ---- MOV r/m<->reg (88-8B); MOV r/m,imm (C6/C7) ----------------------- */
    if (opcode == 0x88 || opcode == 0x89 || opcode == 0x8A || opcode == 0x8B) {
        int width = (opcode & 1) ? operandSize : 1, isLoad = (opcode == 0x8A || opcode == 0x8B);
        V86_MODRM modrm; offset += V86DecodeModrm(cpu, codeLinear, codePointer, offset, segmentOverride, &modrm);
        if (modrm.IsMemory && modrm.Linear >= V86_GUEST_LIMIT) return 0;
        if (isLoad) { UINT32 value = modrm.IsMemory ? V86ReadMemory(modrm.Linear, width) : V86GetRegister(cpu, modrm.RegisterMemory, width);
                    V86SetRegister(cpu, modrm.Register, width, value); }
        else { UINT32 value = V86GetRegister(cpu, modrm.Register, width);
               if (modrm.IsMemory) V86WriteMemory(modrm.Linear, width, value);
               else          V86SetRegister(cpu, modrm.RegisterMemory, width, value); }
        cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
    }
    if (opcode == 0xC6 || opcode == 0xC7) {
        int width = (opcode == 0xC7) ? operandSize : 1;
        V86_MODRM modrm; offset += V86DecodeModrm(cpu, codeLinear, codePointer, offset, segmentOverride, &modrm);
        if (modrm.Register != 0) return 0;
        if (modrm.IsMemory && modrm.Linear >= V86_GUEST_LIMIT) return 0;
        { UINT32 value = V86_IMMEDIATE(offset, width); offset += width;
          if (modrm.IsMemory) V86WriteMemory(modrm.Linear, width, value);
          else          V86SetRegister(cpu, modrm.RegisterMemory, width, value); }
        cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
    }

    /* ---- XCHG r/m,r (86/87): swap; an A0000 read loads latches; no flags --- *
     * QuickBASIC plots mode-12h pixels with `XCHG ES:[DI],AL` (read-modify the *
     * VGA latches + write in one op), so this is the hot pixel-store path.     */
    if (opcode == 0x86 || opcode == 0x87) {
        int width = (opcode == 0x87) ? operandSize : 1;
        V86_MODRM modrm; offset += V86DecodeModrm(cpu, codeLinear, codePointer, offset, segmentOverride, &modrm);
        if (modrm.IsMemory && modrm.Linear >= V86_GUEST_LIMIT) return 0;
        { UINT32 registerValue = V86GetRegister(cpu, modrm.Register, width);
          UINT32 rmValue = modrm.IsMemory ? V86ReadMemory(modrm.Linear, width) : V86GetRegister(cpu, modrm.RegisterMemory, width);
          if (modrm.IsMemory) V86WriteMemory(modrm.Linear, width, registerValue);
          else          V86SetRegister(cpu, modrm.RegisterMemory, width, registerValue);
          V86SetRegister(cpu, modrm.Register, width, rmValue); }
        cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
    }

    /* ---- PUSH/POP r16/r32 (50-5F): SS:SP-relative, via imem (W-wide slot) --- */
    if (opcode >= 0x50 && opcode <= 0x57) {
        WORD stackPointer = (WORD)(cpu->Registers[4] - operandSize);
        V86WriteMemory((V86SegmentBase(cpu->Segments[2])) + stackPointer, operandSize, V86GetRegister(cpu, opcode & 7, operandSize));
        cpu->Registers[4] = (cpu->Registers[4] & 0xFFFF0000u) | stackPointer; cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
    }
    if (opcode >= 0x58 && opcode <= 0x5F) {
        WORD stackPointer = (WORD)cpu->Registers[4];
        V86SetRegister(cpu, opcode & 7, operandSize, V86ReadMemory((V86SegmentBase(cpu->Segments[2])) + stackPointer, operandSize));
        cpu->Registers[4] = (cpu->Registers[4] & 0xFFFF0000u) | (WORD)(stackPointer + operandSize); cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
    }

    /* ---- PUSH imm (68 = imm16/imm32, 6A = imm8 sign-extended to W): a C runtime *
     * pushes call args and far-jump targets this way (run 55's I310102 stopped on *
     * `68 3a 02` = PUSH 0x023A). Slot + immediate are W-wide (0x66 -> 32-bit).     *
     * run 56. */
    if (opcode == 0x68 || opcode == 0x6A) {
        WORD stackPointer = (WORD)(cpu->Registers[4] - operandSize);
        UINT32 value;
        if (opcode == 0x68) { value = V86_IMMEDIATE(offset, operandSize); offset += operandSize; }     /* imm is W bytes */
        else { value = (UINT32)(INT32)(INT8)V86_CODE_BYTE(offset++); value &= V86Mask(operandSize); }  /* sign-ext imm8 */
        V86WriteMemory((V86SegmentBase(cpu->Segments[2])) + stackPointer, operandSize, value);
        cpu->Registers[4] = (cpu->Registers[4] & 0xFFFF0000u) | stackPointer; cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
    }

    /* ---- PUSHF/POPF (9C/9D): the FLAGS stack pair. A C runtime saves/restores  *
     * FLAGS around a code sequence (run 58's I310102 stopped on `9c ...` heading  *
     * a PUSHF; PUSH EDX; ... register-save). We push only the flags we model      *
     * (arithmetic + DF) plus the always-set reserved bit 1; POPF keeps the same   *
     * mask so the round-trip is exact (IF/TF/IOPL/NT are not modeled -> dropped,  *
     * so c->flags never accumulates junk). 16-bit only; the 0x66 PUSHFD/POPFD     *
     * 32-bit-EFLAGS form bails as TODO, matching the neighbouring stack ops.      *
     * run 59. */
    if (opcode == 0x9C) {                                  /* PUSHF */
        WORD stackPointer;
        if (isOperand32) {                                     /* PUSHFD (#194): see V86FlagsImage */
            V86Push(cpu, 4, V86FlagsImage(cpu, 4));
            cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
        }
        stackPointer = (WORD)(cpu->Registers[4] - 2);
        V86WriteMemory((V86SegmentBase(cpu->Segments[2])) + stackPointer, 2, (WORD)((cpu->Flags & 0x0ED5u) | 0x0002u));
        cpu->Registers[4] = (cpu->Registers[4] & 0xFFFF0000u) | stackPointer; cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
    }
    if (opcode == 0x9D) {                                  /* POPF */
        WORD stackPointer;
        if (isOperand32) {                                     /* POPFD (#194): see V86FlagsLoad */
            V86FlagsLoad(cpu, V86Pop(cpu, 4), 4);
            cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
        }
        stackPointer = (WORD)cpu->Registers[4];
        /* IF (0x200) is part of the mask: dropping it made every interpreted POPF
           silently disable the guest's interrupts. */
        cpu->Flags = (V86ReadMemory((V86SegmentBase(cpu->Segments[2])) + stackPointer, 2) & 0x0ED5u) | 0x0002u;
        cpu->Registers[4] = (cpu->Registers[4] & 0xFFFF0000u) | (WORD)(stackPointer + 2); cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
    }

    /* ---- PUSHA/POPA (60/61): push/pop the whole GP file. A callee saves the    *
     * register file on entry this way (run 61's I310102 stopped on `66 60` =      *
     * PUSHAD). Push order AX,CX,DX,BX,SP,BP,SI,DI (indices 0..7) with the pushed  *
     * SP being its value BEFORE the push; POPA restores DI..AX and DISCARDS the   *
     * saved-SP slot. Values are W-wide (0x66 -> PUSHAD/POPAD); the stack offset   *
     * stays 16-bit (address size). run 62. */
    if (opcode == 0x60) {                                  /* PUSHA / PUSHAD */
        WORD stackPointer = (WORD)cpu->Registers[4];
        UINT32 originalSp = V86GetRegister(cpu, 4, operandSize); int registerIndex;           /* SP/ESP before any push */
        for (registerIndex = 0; registerIndex < 8; registerIndex++) {
            UINT32 value = (registerIndex == 4) ? originalSp : V86GetRegister(cpu, registerIndex, operandSize);
            stackPointer = (WORD)(stackPointer - operandSize);
            V86WriteMemory((V86SegmentBase(cpu->Segments[2])) + stackPointer, operandSize, value);
        }
        cpu->Registers[4] = (cpu->Registers[4] & 0xFFFF0000u) | stackPointer; cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
    }
    if (opcode == 0x61) {                                  /* POPA / POPAD */
        WORD stackPointer = (WORD)cpu->Registers[4]; int registerIndex;
        for (registerIndex = 7; registerIndex >= 0; registerIndex--) {
            if (registerIndex == 4) { stackPointer = (WORD)(stackPointer + operandSize); continue; }   /* discard saved SP */
            V86SetRegister(cpu, registerIndex, operandSize, V86ReadMemory((V86SegmentBase(cpu->Segments[2])) + stackPointer, operandSize));
            stackPointer = (WORD)(stackPointer + operandSize);
        }
        cpu->Registers[4] = (cpu->Registers[4] & 0xFFFF0000u) | stackPointer; cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
    }

    /* ---- PUSH/POP segment regs (06/0E/16/1E push ES/CS/SS/DS, 07/17/1F pop  *
     * ES/SS/DS). QB saves/restores ES (and DS) around each pixel -- this was  *
     * the per-pixel bail that capped batching at ~1 pixel. POP CS (0F) is not *
     * modeled (it would change the code segment mid-interpret). ------------- */
    if (opcode == 0x06 || opcode == 0x0E || opcode == 0x16 || opcode == 0x1E) {        /* PUSH sreg */
        int segmentRegister = (opcode == 0x06) ? 0 : (opcode == 0x0E) ? 1 : (opcode == 0x16) ? 2 : 3;
        WORD stackPointer;
        if (isOperand32) {                                     /* #194: 4-byte slot */
            V86PushSegment(cpu, 4, cpu->Segments[segmentRegister]);
            cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
        }
        stackPointer = (WORD)(cpu->Registers[4] - 2);
        V86WriteMemory((V86SegmentBase(cpu->Segments[2])) + stackPointer, 2, cpu->Segments[segmentRegister]);
        cpu->Registers[4] = (cpu->Registers[4] & 0xFFFF0000u) | stackPointer; cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
    }
    if (opcode == 0x07 || opcode == 0x17 || opcode == 0x1F) {                      /* POP sreg */
        int segmentRegister = (opcode == 0x07) ? 0 : (opcode == 0x17) ? 2 : 3;
        WORD stackPointer;
        if (isOperand32) {                                     /* #194: 4-byte slot, low word */
            cpu->Segments[segmentRegister] = (WORD)V86Pop(cpu, 4);
            cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
        }
        stackPointer = (WORD)cpu->Registers[4];
        cpu->Segments[segmentRegister] = (WORD)V86ReadMemory((V86SegmentBase(cpu->Segments[2])) + stackPointer, 2);
        cpu->Registers[4] = (cpu->Registers[4] & 0xFFFF0000u) | (WORD)(stackPointer + 2); cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
    }

    /* ---- MOV r/m16,Sreg (8C) / MOV Sreg,r/m16 (8E) ------------------------- */
    if (opcode == 0x8C || opcode == 0x8E) {
        V86_MODRM modrm; offset += V86DecodeModrm(cpu, codeLinear, codePointer, offset, segmentOverride, &modrm);
        if (modrm.IsMemory && modrm.Linear >= V86_GUEST_LIMIT) return 0;
        if ((modrm.Register & 7) > 5) return 0;
        if (opcode == 0x8C) {                              /* store Sreg -> r/m16 */
            WORD value = cpu->Segments[modrm.Register & 7];
            /* #194: `66 8C` to a REGISTER zero-extends into all 32 bits (measured, p_o32
               item 4); to memory it is a word store either way. */
            if (modrm.IsMemory) V86WriteMemory(modrm.Linear, 2, value);
            else if (isOperand32) cpu->Registers[modrm.RegisterMemory & 7] = value;
            else V86Set16(cpu, modrm.RegisterMemory, value);
        } else {                                       /* load Sreg <- r/m16 */
            if ((modrm.Register & 7) == 1) return 0;              /* MOV CS,x is illegal */
            cpu->Segments[modrm.Register & 7] = modrm.IsMemory ? (WORD)V86ReadMemory(modrm.Linear, 2) : V86Get16(cpu, modrm.RegisterMemory);
        }
        cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
    }

    /* ---- LEA r16/r32, m (8D): load the effective-address offset (not memory) */
    if (opcode == 0x8D) {
        V86_MODRM modrm; offset += V86DecodeModrm(cpu, codeLinear, codePointer, offset, segmentOverride, &modrm);
        if (!modrm.IsMemory) return 0;                       /* LEA with reg operand is illegal */
        V86SetRegister(cpu, modrm.Register, operandSize, modrm.EffectiveAddress);                          /* addr size is 16-bit -> EffectiveAddress zero-ext to W */
        cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
    }

    /* ---- shift/rotate group-2: D0/D1 (by 1), D2/D3 (by CL), C0/C1 (imm8) --- */
    if (opcode == 0xD0 || opcode == 0xD1 || opcode == 0xD2 || opcode == 0xD3 || opcode == 0xC0 || opcode == 0xC1) {
        int width = (opcode & 1) ? operandSize : 1, count;
        V86_MODRM modrm; offset += V86DecodeModrm(cpu, codeLinear, codePointer, offset, segmentOverride, &modrm);
        if (modrm.IsMemory && modrm.Linear >= V86_GUEST_LIMIT) return 0;
        if (opcode == 0xD0 || opcode == 0xD1) count = 1;
        else if (opcode == 0xD2 || opcode == 0xD3) count = V86Get8(cpu, 1);   /* CL */
        else count = V86_CODE_BYTE(offset++);                                /* imm8 */
        { UINT32 value = modrm.IsMemory ? V86ReadMemory(modrm.Linear, width) : V86GetRegister(cpu, modrm.RegisterMemory, width);
          UINT32 result = V86ShiftRotate(cpu, modrm.Register, value, count, width);
          if (modrm.IsMemory) V86WriteMemory(modrm.Linear, width, result);
          else          V86SetRegister(cpu, modrm.RegisterMemory, width, result); }
        cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
    }

    /* ---- IN/OUT via the device bus (E4-E7, EC-EF) -------------------------- *
     * Lets the interpreter run VGA-register-per-pixel plot loops in-host (QB    *
     * reprograms the Graphics Controller bit mask via OUT between pixels).      */
    /* #194: the word forms follow 0x66 -- IN EAX / OUT EAX are 4-byte port accesses.
       They used to be executed as 16-bit ones: EAX[31:16] left stale on IN, and a
       device that decodes 32-bit accesses (none is assumed) handed half a write. */
    if (opcode == 0xE4 || opcode == 0xE5 || opcode == 0xEC || opcode == 0xED) {          /* IN  */
        int width = (opcode & 1) ? operandSize : 1;
        WORD port = (opcode <= 0xE5) ? (WORD)V86_CODE_BYTE(offset++) : cpu->Registers[2];    /* imm8/DX */
        UINT32 value = V86HostIn(port, width);
        V86SetRegister(cpu, 0, width, value & V86Mask(width));
        cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
    }
    if (opcode == 0xE6 || opcode == 0xE7 || opcode == 0xEE || opcode == 0xEF) {          /* OUT */
        int width = (opcode & 1) ? operandSize : 1;
        WORD port = (opcode <= 0xE7) ? (WORD)V86_CODE_BYTE(offset++) : cpu->Registers[2];    /* imm8/DX */
        V86HostOut(port, width, V86GetRegister(cpu, 0, width));
        cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
    }

    /* ---- MOV r,imm (B0-BF); MOV AL/AX,moffs / moffs,AL/AX (A0-A3) --------- */
    if (opcode >= 0xB0 && opcode <= 0xB7) { V86Set8(cpu, opcode & 7, V86_CODE_BYTE(offset)); offset++;
                                    cpu->Ip = (WORD)(cpu->Ip + offset); return 1; }
    if (opcode >= 0xB8 && opcode <= 0xBF) { UINT32 value = V86_IMMEDIATE(offset, operandSize); offset += operandSize;
                                    V86SetRegister(cpu, opcode & 7, operandSize, value); cpu->Ip = (WORD)(cpu->Ip + offset); return 1; }
    if (opcode >= 0xA0 && opcode <= 0xA3) {
        WORD address = (WORD)(V86_CODE_BYTE(offset) | (V86_CODE_BYTE(offset + 1) << 8)); offset += 2;
        UINT32 linear = (V86SegmentBase(cpu->Segments[(segmentOverride >= 0) ? segmentOverride : 3])) + address;
        int width = (opcode & 1) ? operandSize : 1;
        if (linear >= V86_GUEST_LIMIT) return 0;
        if (opcode <= 0xA1) V86SetRegister(cpu, 0, width, V86ReadMemory(linear, width));
        else            V86WriteMemory(linear, width, V86GetRegister(cpu, 0, width));
        cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
    }

    /* ---- string ops: STOS (AA/AB), MOVS (A4/A5), LODS (AC/AD) -------------- */
    /* #194: the dword forms (66 AB/A5/A7/AF/AD) are the same loops with w = 4. Each element
       is still moved a byte at a time, in the order the word forms always used (low
       byte first; destination before source for CMPS), each byte's offset wrapping at
       64 KB -- so an A0000 latch read happens exactly where it did. */
    if (opcode == 0xAA || opcode == 0xAB) {                   /* STOS ES:DI <- AL/AX/EAX */
        int width = (opcode == 0xAB) ? operandSize : 1, direction = (cpu->Flags & V86_DF) ? -width : width, index;
        UINT32 count = repeat ? (UINT32)(WORD)cpu->Registers[1] : 1, extraSegment = cpu->Segments[0], accumulator = cpu->Registers[0]; WORD destinationIndex = cpu->Registers[7];
        while (count) { for (index = 0; index < width; ++index)
                          V86HostWrite8((V86SegmentBase(extraSegment)) + (WORD)(destinationIndex + index), (BYTE)(accumulator >> (8 * index)));
                      destinationIndex = (WORD)(destinationIndex + direction); count--; }
        V86Set16(cpu, 7, destinationIndex); if (repeat) V86Set16(cpu, 1, (WORD)count);
        cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
    }
    if (opcode == 0xA4 || opcode == 0xA5) {                   /* MOVS ES:DI <- DS:SI */
        int width = (opcode == 0xA5) ? operandSize : 1, direction = (cpu->Flags & V86_DF) ? -width : width, index;
        UINT32 count = repeat ? (UINT32)(WORD)cpu->Registers[1] : 1, sourceSegment = cpu->Segments[(segmentOverride >= 0) ? segmentOverride : 3], extraSegment = cpu->Segments[0];
        WORD sourceIndex = cpu->Registers[6], destinationIndex = cpu->Registers[7];
        while (count) { for (index = 0; index < width; ++index)
                          V86HostWrite8((V86SegmentBase(extraSegment)) + (WORD)(destinationIndex + index),
                                  V86HostRead8((V86SegmentBase(sourceSegment)) + (WORD)(sourceIndex + index)));
                      sourceIndex = (WORD)(sourceIndex + direction); destinationIndex = (WORD)(destinationIndex + direction); count--; }
        V86Set16(cpu, 6, sourceIndex); V86Set16(cpu, 7, destinationIndex); if (repeat) V86Set16(cpu, 1, (WORD)count);
        cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
    }
    /* ── CMPS (A6/A7) and SCAS (AE/AF), with REPE (F3) / REPNE (F2). ─────────────
         ⛔ THESE WERE UNMODELLED, AND THE BAIL LEAKED A WHOLE FRAME OF VRAM WRITES.
         (s68, Lemmings.) In a planar mode the host is the CPU and A0000 is left
         UNPROTECTED in V86 (the page trap freezes the rig -- see video_trap_sync).
         Lemmings erases its sprites by `repne scasb` over an 800-byte dirty map and
         a `rep movsb` latch copy master->page for every cell it finds. The scasb
         bailed here, VdmRunGuest then kept the guest on the real CPU until the NEXT
         EVENT, and every one of those latch copies landed in the live mapping,
         invisible to st->plane[]. Measured: the erase engine's whole-run total was
         exactly 2 x 7040 = the two full-window redraws (no scasb on that path) and
         ZERO dirty-cell restores -- the permanent trail of un-erased lemmings.
       ► Flags are CMP's (V86Subtract, no store). REP termination is on ZF after each
         element: REPE stops on ZF=0, REPNE on ZF=1. CX=0 with a prefix = no-op and
         the flags are left alone, as on the hardware. */
    if (opcode == 0xA6 || opcode == 0xA7 || opcode == 0xAE || opcode == 0xAF) {
        int width = (opcode & 1) ? operandSize : 1, direction = (cpu->Flags & V86_DF) ? -width : width, index;
        int isScas = (opcode >= 0xAE);
        UINT32 count = repeat ? (UINT32)(WORD)cpu->Registers[1] : 1, extraSegment = cpu->Segments[0];
        UINT32 sourceSegment = cpu->Segments[(segmentOverride >= 0) ? segmentOverride : 3];
        WORD sourceIndex = cpu->Registers[6], destinationIndex = cpu->Registers[7];
        while (count) {
            UINT32 first = 0, second = 0;
            for (index = 0; index < width; ++index)
                second |= (UINT32)V86HostRead8((V86SegmentBase(extraSegment)) + (WORD)(destinationIndex + index)) << (8 * index);
            if (isScas) first = V86GetRegister(cpu, 0, width);                              /* AL/AX/EAX */
            else {
                for (index = 0; index < width; ++index)
                    first |= (UINT32)V86HostRead8((V86SegmentBase(sourceSegment)) + (WORD)(sourceIndex + index)) << (8 * index);
                sourceIndex = (WORD)(sourceIndex + direction);
            }
            V86Subtract(cpu, first, second, 0, width);                                   /* CMP a,b */
            destinationIndex = (WORD)(destinationIndex + direction); count--;
            if (repeat == 1 && !(cpu->Flags & V86_ZF)) break;               /* REPE  */
            if (repeat == 2 &&  (cpu->Flags & V86_ZF)) break;               /* REPNE */
        }
        if (!isScas) V86Set16(cpu, 6, sourceIndex);
        V86Set16(cpu, 7, destinationIndex); if (repeat) V86Set16(cpu, 1, (WORD)count);
        cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
    }
    if (opcode == 0xAC || opcode == 0xAD) {                   /* LODS AL/AX/EAX <- DS:SI */
        int width = (opcode == 0xAD) ? operandSize : 1, direction = (cpu->Flags & V86_DF) ? -width : width, index;
        UINT32 count = repeat ? (UINT32)(WORD)cpu->Registers[1] : 1, sourceSegment = cpu->Segments[(segmentOverride >= 0) ? segmentOverride : 3]; WORD sourceIndex = cpu->Registers[6];
        while (count) { UINT32 value = 0;
                      for (index = 0; index < width; ++index)
                          value |= (UINT32)V86HostRead8((V86SegmentBase(sourceSegment)) + (WORD)(sourceIndex + index)) << (8 * index);
                      V86SetRegister(cpu, 0, width, value);
                      sourceIndex = (WORD)(sourceIndex + direction); count--; }
        V86Set16(cpu, 6, sourceIndex); if (repeat) V86Set16(cpu, 1, (WORD)count);
        cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
    }

    /* ---- control flow: Jcc (70-7F), JMP short (EB) / near (E9),
            CALL near (E8) + RET near (C3/C2), LOOP/LOOPE/LOOPNE/JCXZ (E0-E3) -- *
     * CALL/RET let the interpreter follow QuickBasic's per-pixel runtime call,  *
     * so a whole scanline batches in one fault instead of ~5 instr per pixel.   */
    if (opcode >= 0x70 && opcode <= 0x7F) {
        INT8 relative = (INT8)V86_CODE_BYTE(offset++); int isTaken = V86Condition(cpu, opcode & 0xF);
        cpu->Ip = (WORD)(cpu->Ip + offset + (isTaken ? relative : 0)); return 1;
    }
    if (opcode == 0xEB) { INT8 relative = (INT8)V86_CODE_BYTE(offset++); cpu->Ip = (WORD)(cpu->Ip + offset + relative); return 1; }
    /* #194: JMP/CALL rel32 (66 E9/E8): EIP = next + rel32, a 4-byte return slot; bail if
       the target leaves the 64 KB segment (the CPU's #GP, not ours to fake). */
    if ((opcode == 0xE9 || opcode == 0xE8) && isOperand32) {
        UINT32 nextIp = (UINT32)(WORD)(cpu->Ip + offset + 4);
        UINT32 target = nextIp + V86ReadMemory(codeLinear + offset, 4);
        if (target > 0xFFFFu) return 0;
        if (opcode == 0xE8) V86Push(cpu, 4, nextIp);
        cpu->Ip = (WORD)target; return 1;
    }
    if (opcode == 0xE9) { INT16 relative; relative = (INT16)(V86_CODE_BYTE(offset) | (V86_CODE_BYTE(offset + 1) << 8)); offset += 2;
                      cpu->Ip = (WORD)(cpu->Ip + offset + relative); return 1; }
    if (opcode == 0xE8) {                                  /* CALL near relative */
        INT16 relative; WORD nextIp, stackPointer;
        relative = (INT16)(V86_CODE_BYTE(offset) | (V86_CODE_BYTE(offset + 1) << 8)); offset += 2;
        nextIp = (WORD)(cpu->Ip + offset);                 /* return address */
        stackPointer = (WORD)(cpu->Registers[4] - 2);
        V86WriteMemory((V86SegmentBase(cpu->Segments[2])) + stackPointer, 2, nextIp);
        cpu->Registers[4] = (cpu->Registers[4] & 0xFFFF0000u) | stackPointer; cpu->Ip = (WORD)(nextIp + relative); return 1;
    }
    if (opcode == 0xC3 || opcode == 0xC2) {                    /* RET near (+ imm16 pop) */
        WORD stackPointer, returnIp, extraPop;
        if (isOperand32) {                                     /* #194: a 4-byte EIP slot */
            UINT32 operand;
            stackPointer = (WORD)cpu->Registers[4];
            operand = V86ReadMemory(V86SegmentBase(cpu->Segments[2]) + stackPointer, 4);
            if (operand > 0xFFFFu) return 0;
            extraPop = (opcode == 0xC2) ? (WORD)(V86_CODE_BYTE(offset) | (V86_CODE_BYTE(offset + 1) << 8)) : 0;
            cpu->Registers[4] = (cpu->Registers[4] & 0xFFFF0000u) | (WORD)(stackPointer + 4 + extraPop);
            cpu->Ip = (WORD)operand; return 1;
        }
        stackPointer = (WORD)cpu->Registers[4];
        returnIp = (WORD)V86ReadMemory((V86SegmentBase(cpu->Segments[2])) + stackPointer, 2);
        extraPop = (opcode == 0xC2) ? (WORD)(V86_CODE_BYTE(offset) | (V86_CODE_BYTE(offset + 1) << 8)) : 0;
        cpu->Registers[4] = (cpu->Registers[4] & 0xFFFF0000u) | (WORD)(stackPointer + 2 + extraPop); cpu->Ip = returnIp; return 1;
    }

    /* ---- RETF (CB) / RETF imm16 (CA): far return -- pop offset then a 2-byte  *
     * SELECTOR into CS. In PM `V86SegmentBase` resolves that selector via the LDT (the *
     * same machinery LAR/LSL use), so the client's `PUSH seg; PUSH off; RETF`    *
     * far-transfer idiom (run 56's wall) just follows through. run 57. */
    if (opcode == 0xCB || opcode == 0xCA) {
        WORD stackPointer, returnOffset, selector, extraPop;
        if (isOperand32) {                                     /* #194: EIP dword, then a CS dword */
            UINT32 operand; UINT32 stackBase = V86SegmentBase(cpu->Segments[2]);
            stackPointer  = (WORD)cpu->Registers[4];
            operand   = V86ReadMemory(stackBase + stackPointer, 4);
            selector = (WORD)V86ReadMemory(stackBase + (WORD)(stackPointer + 4), 2);
            if (g_V86SegmentToLinear ? !V86IsFarTargetValid(cpu, selector, operand) : (operand > 0xFFFFu)) return 0;
            extraPop = (opcode == 0xCA) ? (WORD)(V86_CODE_BYTE(offset) | (V86_CODE_BYTE(offset + 1) << 8)) : 0;
            cpu->Registers[4] = (cpu->Registers[4] & 0xFFFF0000u) | (WORD)(stackPointer + 8 + extraPop);
            cpu->Segments[1] = selector; cpu->Ip = (WORD)operand; return 1;
        }
        stackPointer  = (WORD)cpu->Registers[4];
        returnOffset = (WORD)V86ReadMemory((V86SegmentBase(cpu->Segments[2])) + stackPointer, 2);
        selector = (WORD)V86ReadMemory((V86SegmentBase(cpu->Segments[2])) + (WORD)(stackPointer + 2), 2);
        extraPop = (opcode == 0xCA) ? (WORD)(V86_CODE_BYTE(offset) | (V86_CODE_BYTE(offset + 1) << 8)) : 0;
        cpu->Registers[4] = (cpu->Registers[4] & 0xFFFF0000u) | (WORD)(stackPointer + 4 + extraPop);
        cpu->Segments[1] = selector; cpu->Ip = returnOffset; return 1;
    }

    /* ---- LEAVE (C9): frame teardown -- MOV SP,BP; POP BP. Paired with the C  *
     * runtime's function-prologue ENTER/`PUSH BP; MOV BP,SP`, so it lands on    *
     * every callee return (run 57's far RET reaches main(), whose epilogue is   *
     * this). SP first snaps to BP (discarding locals), then the caller's BP is  *
     * popped. run 58. */
    /* #269: ENTER imm16, 0 (C8) -- the prologue LEAVE below undoes: push BP, BP <- SP,
       SP -= imm16, all 16-bit (this interpreter's stack is SS:SP). A NESTING LEVEL
       (copying the caller's frame pointers) and the 0x66 form, whose final BP/EBP
       write the manual ties to the stack size, still bail. */
    if (opcode == 0xC8 && !isOperand32) {
        WORD frameSize = (WORD)(V86_CODE_BYTE(offset) | (V86_CODE_BYTE(offset + 1) << 8));
        WORD stackPointer;
        if (V86_CODE_BYTE(offset + 2) & 0x1F) return 0;
        offset += 3;
        stackPointer = (WORD)(cpu->Registers[4] - 2);
        V86WriteMemory(V86SegmentBase(cpu->Segments[2]) + stackPointer, 2, (WORD)cpu->Registers[5]);
        cpu->Registers[5] = (cpu->Registers[5] & 0xFFFF0000u) | stackPointer;        /* BP <- SP */
        cpu->Registers[4] = (cpu->Registers[4] & 0xFFFF0000u) | (WORD)(stackPointer - frameSize);
        cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
    }
    if (opcode == 0xC9) {
        WORD stackPointer, framePointer;
        if (isOperand32) {                                     /* #194: SP <- BP (16-bit stack), EBP <- pop32 */
            stackPointer = (WORD)cpu->Registers[5];
            cpu->Registers[5] = V86ReadMemory(V86SegmentBase(cpu->Segments[2]) + stackPointer, 4);
            cpu->Registers[4] = (cpu->Registers[4] & 0xFFFF0000u) | (WORD)(stackPointer + 4);
            cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
        }
        stackPointer = (WORD)cpu->Registers[5];                        /* SP <- BP */
        framePointer = (WORD)V86ReadMemory((V86SegmentBase(cpu->Segments[2])) + stackPointer, 2);
        cpu->Registers[5] = (cpu->Registers[5] & 0xFFFF0000u) | framePointer;        /* BP <- pop */
        cpu->Registers[4] = (cpu->Registers[4] & 0xFFFF0000u) | (WORD)(stackPointer + 2);
        cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
    }
    /* ── CBW (98) / CWD (99), XLAT (D7), LES/LDS (C4/C5, MEMORY form). (s68) ────────
         Named by the P12 bail table on Lemmings once the scasb was modelled: 270
         bails a run on CBW alone, each one a stretch of V86 with A0000 unprotected.
       ► C4 is ALSO the VDM BOP -- but only as `C4 C4 nn`, i.e. the modrm byte 0xC4
         (mod=3, the REGISTER form, which LES cannot take). A memory-form C4/C5 is a
         real LES/LDS and cannot be a BOP; the register form still bails, so every
         service call still reaches the kernel as before. */
    /* POP r/m16 (8F /0) and WAIT (9B). Named by the bail table on Bubbles (QBasic,
       mode 12h): `pop [bx+7]` alone bailed 1,158,586 times in one run -- the QBasic
       runtime's calling convention -- and each bail is a stretch of lost pixels. */
    if (opcode == 0x8F) {
        V86_MODRM modrm; WORD stackPointer; UINT32 value;
        if (g_V86SegmentToLinear) return 0;
        offset += V86DecodeModrm(cpu, codeLinear, codePointer, offset, segmentOverride, &modrm);
        if (modrm.Register != 0 || (modrm.IsMemory && modrm.Linear + operandSize > V86_GUEST_LIMIT)) return 0;
        stackPointer = (WORD)cpu->Registers[4];
        value = V86ReadMemory((V86SegmentBase(cpu->Segments[2])) + stackPointer, operandSize);    /* #194: W = 4 under 0x66 */
        cpu->Registers[4] = (cpu->Registers[4] & 0xFFFF0000u) | (WORD)(stackPointer + operandSize);   /* SP moves FIRST:
                                                    `pop [sp-relative]` sees the new SP */
        if (modrm.IsMemory) V86WriteMemory(modrm.Linear, operandSize, value); else V86SetRegister(cpu, modrm.RegisterMemory, operandSize, value);
        cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
    }
    if (opcode == 0x9B) { cpu->Ip = (WORD)(cpu->Ip + offset); return 1; }   /* WAIT: no FPU here */
    /* LAHF (9F) / SAHF (9E): AH <-> SF ZF AF PF CF (bit 1 reads as 1). Bubbles: 16,586. */
    if (opcode == 0x9F) { V86Set8(cpu, 4, (BYTE)((cpu->Flags & 0xD5u) | 0x02u)); cpu->Ip = (WORD)(cpu->Ip + offset); return 1; }
    if (opcode == 0x9E) { cpu->Flags = (cpu->Flags & ~0xD5u) | (V86Get8(cpu, 4) & 0xD5u); cpu->Ip = (WORD)(cpu->Ip + offset); return 1; }
    if (opcode == 0x98) {                                  /* CBW: AX <- sign(AL); 66: CWDE */
        if (isOperand32) cpu->Registers[0] = (UINT32)(INT32)(INT16)(cpu->Registers[0] & 0xFFFF);
        else     V86Set16(cpu, 0, (WORD)(INT16)(INT8)(cpu->Registers[0] & 0xFF));
        cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
    }
    if (opcode == 0x99) {                                  /* CWD: DX <- sign(AX); 66: CDQ */
        /* CDQ is s80's: Wolf3D's FixedByFrac (`cdq / idiv dword`) declined it 1,074
           times under a multi-plane mask, handing its renderer to the real CPU. */
        if (isOperand32) cpu->Registers[2] = (cpu->Registers[0] & 0x80000000u) ? 0xFFFFFFFFu : 0u;
        else     V86Set16(cpu, 2, (cpu->Registers[0] & 0x8000u) ? 0xFFFFu : 0u);
        cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
    }
    /* ---- IMUL reg, r/m, imm (69: imm16/32, 6B: sign-extended imm8). s80: Mario's
     * `6b f8 0a` = imul di,ax,10 was 7,978 of its declines under a multi-plane mask.
     * Destination is the reg field; CF=OF=1 when the signed product does not fit the
     * destination width. SF/ZF/AF/PF are undefined by the spec and left alone. */
    if (opcode == 0x69 || opcode == 0x6B) {
        int width = operandSize, isOverflow;
        INT64 first, second, product;
        UINT32 result;
        V86_MODRM modrm; offset += V86DecodeModrm(cpu, codeLinear, codePointer, offset, segmentOverride, &modrm);
        if (modrm.IsMemory && modrm.Linear >= V86_GUEST_LIMIT) return 0;
        { UINT32 operand = modrm.IsMemory ? V86ReadMemory(modrm.Linear, width) : V86GetRegister(cpu, modrm.RegisterMemory, width);
          first = (width == 4) ? (INT64)(INT32)operand : (INT64)(INT16)operand; }
        if (opcode == 0x6B) { second = (INT8)V86_IMMEDIATE(offset, 1); offset += 1; }
        else { UINT32 immediate = V86_IMMEDIATE(offset, width); offset += width;
               second = (width == 4) ? (INT64)(INT32)immediate : (INT64)(INT16)immediate; }
        product = first * second;
        result = (UINT32)product & V86Mask(width);
        isOverflow = (width == 4) ? (product != (INT64)(INT32)result) : (product != (INT64)(INT16)result);
        V86SetRegister(cpu, modrm.Register, width, result);
        cpu->Flags = (cpu->Flags & ~(V86_CF | V86_OF)) | (isOverflow ? (V86_CF | V86_OF) : 0);
        cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
    }
    if (opcode == 0xD7) {                                  /* XLAT: AL <- [DS:BX+AL] */
        UINT32 sourceSegment = cpu->Segments[(segmentOverride >= 0) ? segmentOverride : 3];
        UINT32 linear = (V86SegmentBase(sourceSegment)) + (WORD)((cpu->Registers[3] & 0xFFFF) + (cpu->Registers[0] & 0xFF));
        if (linear >= V86_GUEST_LIMIT) return 0;
        V86Set8(cpu, 0, V86HostRead8(linear));
        cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
    }
    if ((opcode == 0xC4 || opcode == 0xC5) && V86_CODE_BYTE(offset) != 0xC4 && (V86_CODE_BYTE(offset) >> 6) != 3) {
        V86_MODRM modrm;
        if (g_V86SegmentToLinear) return 0;                       /* PM: TODO */
        offset += V86DecodeModrm(cpu, codeLinear, codePointer, offset, segmentOverride, &modrm);
        if (!modrm.IsMemory || modrm.Linear + operandSize + 2 > V86_GUEST_LIMIT) return 0;
        /* #194: under 0x66 the pointer is m16:32 -- a dword offset, then the segment. */
        V86SetRegister(cpu, modrm.Register, operandSize, V86ReadMemory(modrm.Linear, operandSize));
        cpu->Segments[opcode == 0xC4 ? 0 : 3] = (WORD)V86ReadMemory(modrm.Linear + operandSize, 2);
        cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
    }
    if (opcode >= 0xE0 && opcode <= 0xE3) {
        INT8 relative = (INT8)V86_CODE_BYTE(offset++); int isTaken;
        /* #183: CX, not ECX -- 16-bit address size counts in CX and leaves the high
           half alone, and since s80 carried all 32 bits in, the high half is real. */
        if (opcode == 0xE3) isTaken = ((WORD)cpu->Registers[1] == 0);        /* JCXZ */
        else { V86Set16(cpu, 1, (WORD)(cpu->Registers[1] - 1));
               int isCountNonZero = ((WORD)cpu->Registers[1] != 0);
               isTaken = (opcode == 0xE2) ? isCountNonZero                                   /* LOOP   */
                    : (opcode == 0xE1) ? (isCountNonZero && (cpu->Flags & V86_ZF))            /* LOOPE  */
                                   : (isCountNonZero && !(cpu->Flags & V86_ZF)); }        /* LOOPNE */
        cpu->Ip = (WORD)(cpu->Ip + offset + (isTaken ? relative : 0)); return 1;
    }

    /* ---- XCHG AX,r16 (91-97): the accumulator short-form -- swap AX with the  *
     * indexed reg (0x90 = XCHG AX,AX = NOP, handled just below). No flags. A C   *
     * runtime uses it as cheap register glue (run 59's I310102 stopped on `96` = *
     * XCHG AX,SI). W-wide (0x66 -> XCHG EAX,r32). run 60. */
    if (opcode >= 0x91 && opcode <= 0x97) {
        int registerIndex = opcode & 7;
        UINT32 first = V86GetRegister(cpu, 0, operandSize), second = V86GetRegister(cpu, registerIndex, operandSize);
        V86SetRegister(cpu, 0, operandSize, second); V86SetRegister(cpu, registerIndex, operandSize, first);
        cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
    }

    /* ---- flag ops + NOP --------------------------------------------------- */
    if (opcode == 0x90) { cpu->Ip = (WORD)(cpu->Ip + offset); return 1; }              /* NOP */
    if (opcode == 0xF8) { cpu->Flags &= ~V86_CF; cpu->Ip = (WORD)(cpu->Ip + offset); return 1; }  /* CLC */
    if (opcode == 0xF9) { cpu->Flags |=  V86_CF; cpu->Ip = (WORD)(cpu->Ip + offset); return 1; }  /* STC */
    if (opcode == 0xF5) { cpu->Flags ^=  V86_CF; cpu->Ip = (WORD)(cpu->Ip + offset); return 1; }  /* CMC */
    if (opcode == 0xFC) { cpu->Flags &= ~V86_DF; cpu->Ip = (WORD)(cpu->Ip + offset); return 1; }  /* CLD */
    if (opcode == 0xFD) { cpu->Flags |=  V86_DF; cpu->Ip = (WORD)(cpu->Ip + offset); return 1; }  /* STD */

    /* ---- control transfer through the IVT: int nn (CD) / INT3 (CC) / IRET (CF) *
     * Needed by CONTINUOUS interpretation (mode 12h, GH #55). Without them the    *
     * interpreter stopped at the first DOS/BIOS call and handed the guest back to *
     * the real CPU -- where its A0000 writes bypass the planar engine and the      *
     * picture is lost until the next port trap. With them the guest keeps running  *
     * here across its own interrupt handlers.                                      *
     * LES/LDS (C4/C5) are deliberately still unmodeled: the VDM BOP is `C4 C4 nn`, *
     * so bailing on C4 is exactly how a DOS/BIOS call reaches the kernel as a BOP  *
     * event. Modeling LES would swallow every service call the host provides.      */
    if (opcode == 0xCD || opcode == 0xCC) {
        UINT32 stackBase; WORD stackPointer, nextIp, vector;
        if (g_V86SegmentToLinear) return 0;                       /* PM: no real-mode IVT -> bail   */
        vector = (opcode == 0xCC) ? 3 : V86_CODE_BYTE(offset++);
        nextIp = (WORD)(cpu->Ip + offset);                 /* return address = past the int  */
        stackBase = V86SegmentBase(cpu->Segments[2]);
        stackPointer = (WORD)(cpu->Registers[4] - 2); V86WriteMemory(stackBase + stackPointer, 2, (cpu->Flags & 0x0ED5u) | 0x0002u);
        stackPointer = (WORD)(stackPointer - 2);      V86WriteMemory(stackBase + stackPointer, 2, cpu->Segments[1]);
        stackPointer = (WORD)(stackPointer - 2);      V86WriteMemory(stackBase + stackPointer, 2, nextIp);
        cpu->Registers[4]   = (cpu->Registers[4] & 0xFFFF0000u) | stackPointer;
        cpu->Flags &= ~0x0300u;                          /* IF/TF cleared on entry         */
        cpu->Ip     = (WORD)V86ReadMemory((UINT32)vector * 4, 2);
        cpu->Segments[1] = (WORD)V86ReadMemory((UINT32)vector * 4 + 2, 2);
        return 1;
    }
    if (opcode == 0xCF) {                                  /* IRET */
        UINT32 stackBase; WORD stackPointer;
        if (isOperand32 && !g_V86SegmentToLinear) {
            /* #194: IRETD in real/V86 mode -- EIP, CS and EFLAGS as dwords, 12 bytes. The
               EIP must fit the 64 KB segment (bail otherwise); EFLAGS loads as POPFD does.
               PM IRETD still bails: it may return to a 32-bit segment or another ring. */
            UINT32 operand, newFlags; WORD codeSegment;
            stackPointer  = (WORD)cpu->Registers[4]; stackBase = V86SegmentBase(cpu->Segments[2]);
            operand   = V86ReadMemory(stackBase + stackPointer, 4);
            codeSegment  = (WORD)V86ReadMemory(stackBase + (WORD)(stackPointer + 4), 2);
            newFlags  = V86ReadMemory(stackBase + (WORD)(stackPointer + 8), 4);
            if (operand > 0xFFFFu) return 0;
            cpu->Ip = (WORD)operand; cpu->Segments[1] = codeSegment;
            V86FlagsLoad(cpu, newFlags, 4);
            cpu->Registers[4] = (cpu->Registers[4] & 0xFFFF0000u) | (WORD)(stackPointer + 12);
            return 1;
        }
        if (isOperand32) return 0;                             /* PM IRETD: the CPU's            */
        if (g_V86SegmentToLinear) {
            /* #194: IRET in 16-bit PROTECTED mode, same privilege -- the return from an
               interrupt handler the DPMI interpreter path ran (dpmi_run_pm_interp). It
               follows only a frame V86IsFarTargetValid() accepts: a present 16-bit code segment at
               the current ring, IP inside its limit. A ring change would also pop SS:SP,
               and a 32-bit target is code this core cannot decode -- both the CPU's.
               FLAGS load exactly as the real-mode IRET's (IF included, as POPF does on
               this path: the virtual IF is the DPMI host's to keep, see 0900h-0902h). */
            WORD nextIp, newCodeSegment; UINT32 newFlags;
            stackPointer  = (WORD)cpu->Registers[4]; stackBase = V86SegmentBase(cpu->Segments[2]);
            nextIp = (WORD)V86ReadMemory(stackBase + stackPointer, 2);
            newCodeSegment = (WORD)V86ReadMemory(stackBase + (WORD)(stackPointer + 2), 2);
            newFlags  = V86ReadMemory(stackBase + (WORD)(stackPointer + 4), 2);
            if (!V86IsFarTargetValid(cpu, newCodeSegment, nextIp)) return 0;
            cpu->Ip = nextIp; cpu->Segments[1] = newCodeSegment;
            V86FlagsLoad(cpu, newFlags, 2);
            cpu->Registers[4] = (cpu->Registers[4] & 0xFFFF0000u) | (WORD)(stackPointer + 6);
            return 1;
        }
        stackPointer  = (WORD)cpu->Registers[4]; stackBase = V86SegmentBase(cpu->Segments[2]);
        cpu->Ip     = (WORD)V86ReadMemory(stackBase + stackPointer, 2);
        cpu->Segments[1] = (WORD)V86ReadMemory(stackBase + (WORD)(stackPointer + 2), 2);
        cpu->Flags  = (V86ReadMemory(stackBase + (WORD)(stackPointer + 4), 2) & 0x0ED5u) | 0x0002u;
        cpu->Registers[4]   = (cpu->Registers[4] & 0xFFFF0000u) | (WORD)(stackPointer + 6);
        return 1;
    }
    /* ---- far JMP (EA) / far CALL (9A) to a real-mode seg:off ---------------- */
    if ((opcode == 0xEA || opcode == 0x9A) && isOperand32 && !g_V86SegmentToLinear) {
        /* #194: ptr16:32 -- a dword offset, then the segment; CALL pushes CS and EIP as
           dwords. Real mode only, like the 16-bit form below. */
        UINT32 farOffset32 = V86ReadMemory(codeLinear + offset, 4);
        WORD segment = (WORD)(V86_CODE_BYTE(offset + 4) | (V86_CODE_BYTE(offset + 5) << 8));
        offset += 6;
        if (farOffset32 > 0xFFFFu) return 0;
        if (opcode == 0x9A) { V86Push(cpu, 4, cpu->Segments[1]); V86Push(cpu, 4, (WORD)(cpu->Ip + offset)); }  /* CS: dword */
        cpu->Segments[1] = segment; cpu->Ip = (WORD)farOffset32; return 1;
    }
    if (opcode == 0xEA || opcode == 0x9A) {
        WORD farOffset, segment;
        if (isOperand32 || g_V86SegmentToLinear) return 0;
        farOffset = (WORD)(V86_CODE_BYTE(offset) | (V86_CODE_BYTE(offset + 1) << 8)); offset += 2;
        segment = (WORD)(V86_CODE_BYTE(offset) | (V86_CODE_BYTE(offset + 1) << 8)); offset += 2;
        if (opcode == 0x9A) {                              /* CALL FAR: push CS then IP      */
            UINT32 stackBase = V86SegmentBase(cpu->Segments[2]);
            WORD nextIp = (WORD)(cpu->Ip + offset), stackPointer;
            stackPointer = (WORD)(cpu->Registers[4] - 2); V86WriteMemory(stackBase + stackPointer, 2, cpu->Segments[1]);
            stackPointer = (WORD)(stackPointer - 2);      V86WriteMemory(stackBase + stackPointer, 2, nextIp);
            cpu->Registers[4] = (cpu->Registers[4] & 0xFFFF0000u) | stackPointer;
        }
        cpu->Segments[1] = segment; cpu->Ip = farOffset; return 1;
    }
    /* ---- CLI/STI (FA/FB). IF is carried in the flag image so an interpreted   *
     * handler's IRET restores it and the loop-top delivery gate sees the truth. */
    if (opcode == 0xFA) { cpu->Flags &= ~0x0200u; cpu->Ip = (WORD)(cpu->Ip + offset); return 1; }
    if (opcode == 0xFB) { cpu->Flags |=  0x0200u; cpu->Ip = (WORD)(cpu->Ip + offset); return 1; }

    return 0;                                          /* unmodeled: bail to V86 */
}

#endif /* V86INTERP_H */
