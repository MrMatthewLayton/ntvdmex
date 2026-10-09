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

#include "v86cpu.h"           /* the CPU state and the constants its users share */

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
V86_INLINE int V86Parity(BYTE value) { value ^= value >> NIBBLE_SHIFT; return !((V86_PARITY_TABLE >> (value & NIBBLE_MASK_U)) & 1u); }

/* Operand-width helpers: w is 1/2/4 bytes. The 4-byte path exists for 16-bit code
   that uses the 0x66 operand-size prefix (386 32-bit register math -- e.g. a C
   runtime's MOVZX ESI,SI / SHL ESI,4). run 54. */
V86_INLINE UINT32 V86Mask(int width) { return (width == 1) ? BYTE_MASK_U : (width == X86_WORD_SIZE) ? WORD_MASK_U : DWORD_MASK_U; }
V86_INLINE UINT32 V86SignBit(int width) { return (width == 1) ? X86_BYTE_SIGN_U : (width == X86_WORD_SIZE) ? X86_WORD_SIGN_U : X86_DWORD_SIGN_U; }

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
{ return g_V86SegmentToLinear ? g_V86SegmentToLinear(segment) : ((UINT32)segment << PARAGRAPH_SHIFT); }

/* Descriptor-introspection hook for PM (LAR/LSL, run 55). Given a selector, returns
   1 if it names a valid, accessible descriptor and fills *ar (access-rights in LAR
   format: access byte at bits 8-15, G/D/AVL nibble at 20-23) and *limit (byte-
   granular). 0 = invalid selector (the op then clears ZF, leaving the dest reg
   unchanged). NULL in V86 mode -> LAR/LSL bail (they don't occur there). The DPMI
   host sets this to read its g_ldt[] table. */
static int (*g_V86SelectorDescriptor)(WORD selector, UINT32 *accessRights, UINT32 *limit) = 0;

V86_INLINE UINT32 V86ReadMemory(UINT32 linear, int width)
{ UINT32 value = V86HostRead8(linear);
  if (width >= X86_WORD_SIZE) value |= (UINT32)V86HostRead8(linear + 1) << BYTE_SHIFT;
  if (width == X86_DWORD_SIZE) value |= ((UINT32)V86HostRead8(linear + V86_THIRD_BYTE) << WORD_SHIFT) | ((UINT32)V86HostRead8(linear + V86_FOURTH_BYTE) << TOP_BYTE_SHIFT);
  return value; }
V86_INLINE VOID V86WriteMemory(UINT32 linear, int width, UINT32 value)
{ V86HostWrite8(linear, (BYTE)value);
  if (width >= X86_WORD_SIZE) V86HostWrite8(linear + 1, (BYTE)(value >> BYTE_SHIFT));
  if (width == X86_DWORD_SIZE) { V86HostWrite8(linear + V86_THIRD_BYTE, (BYTE)(value >> WORD_SHIFT)); V86HostWrite8(linear + V86_FOURTH_BYTE, (BYTE)(value >> TOP_BYTE_SHIFT)); } }

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
    WORD stackPointer = (WORD)(cpu->Registers[X86_REG_SP] - width);
    V86WriteMemory(V86SegmentBase(cpu->Segments[X86_SREG_SS]) + stackPointer, width, value);
    cpu->Registers[X86_REG_SP] = (cpu->Registers[X86_REG_SP] & HIGH_WORD_MASK_U) | stackPointer;
}
static UINT32 V86Pop(V86_CPU *cpu, int width)
{
    WORD stackPointer = (WORD)cpu->Registers[X86_REG_SP];
    UINT32 value = V86ReadMemory(V86SegmentBase(cpu->Segments[X86_SREG_SS]) + stackPointer, width);
    cpu->Registers[X86_REG_SP] = (cpu->Registers[X86_REG_SP] & HIGH_WORD_MASK_U) | (WORD)(stackPointer + width);
    return value;
}
/* PUSH of a segment register in a W-wide slot. W=4: a WORD store, SP -= 4, the slot's
   upper half untouched (measured -- see above). Not for a far CALL's CS: that one is a
   zero-extended dword (ipush(c, 4, cs)). */
static VOID V86PushSegment(V86_CPU *cpu, int width, WORD value)
{
    WORD stackPointer = (WORD)(cpu->Registers[X86_REG_SP] - width);
    V86WriteMemory(V86SegmentBase(cpu->Segments[X86_SREG_SS]) + stackPointer, X86_WORD_SIZE, value);
    cpu->Registers[X86_REG_SP] = (cpu->Registers[X86_REG_SP] & HIGH_WORD_MASK_U) | stackPointer;
}
/* The FLAGS image PUSHF(D) writes, and what POPF(D) may load. IF is carried (an
   interpreted handler's POPF/IRET must restore it); TF/IOPL/NT are not modelled.
   PUSHFD's upper half is the NT V86 monitor's (measured): AC/ID as they stand, and in
   V86 mode (no PM resolver) VM and RF always, VIF = the guest's IF. */
static UINT32 V86FlagsImage(const V86_CPU *cpu, int width)
{
    UINT32 value = (cpu->Flags & V86_FLAGS_MODELLED) | EFLAGS_RESERVED_ONE_U;
    if (width == X86_DWORD_SIZE) {
        value |= cpu->Flags & V86I_EFL_HI;
        if (!g_V86SegmentToLinear) value |= (EFLAGS_RF_U | EFLAGS_VM_U) | ((cpu->Flags & EFLAGS_IF_U) ? EFLAGS_VIF_U : 0u);
    }
    return value;
}
static VOID V86FlagsLoad(V86_CPU *cpu, UINT32 value, int width)
{
    UINT32 low = (value & V86_FLAGS_MODELLED) | EFLAGS_RESERVED_ONE_U;
    /* The 16-bit form REPLACES the whole register (upper half -> 0), as POPF and IRET
       always have here -- kept bit for bit, the fuzz pins it. The 32-bit form loads
       only the modelled upper bits and leaves the rest (VM, VIF ...) as they were. */
    if (width == X86_WORD_SIZE) cpu->Flags = low;
    else        cpu->Flags = (cpu->Flags & ~(WORD_MASK_U | V86I_EFL_HI)) | (value & V86I_EFL_HI) | low;
}

/* A protected-mode far transfer the interpreter can FOLLOW: a present, 16-bit code
   segment at the caller's own privilege, with the offset inside its limit. Anything
   else -- a 32-bit target (this core decodes 16-bit code only), a ring change (which
   would also switch stacks), a bad selector -- is the real CPU's to take. */
static int V86IsFarTargetValid(const V86_CPU *cpu, WORD selector, UINT32 offset)
{
    UINT32 accessRights, limit;
    if (!g_V86SelectorDescriptor || !g_V86SelectorDescriptor(selector, &accessRights, &limit)) return 0;
    if (!(accessRights & V86_ACCESS_PRESENT) || !(accessRights & V86_ACCESS_CODE)) return 0;   /* present, code         */
    if (accessRights & V86_ACCESS_DEFAULT_BIG) return 0;                      /* D=1: a 32-bit segment */
    if ((selector & V86_SELECTOR_RPL_MASK) != (cpu->Segments[X86_SREG_CS] & V86_SELECTOR_RPL_MASK)) return 0;         /* privilege change      */
    return offset <= limit;
}

/* CPU register file access by x86 encoding. Sub-register writes preserve the bits
   they don't touch: a 16-bit write keeps E-reg[31:16]; an 8-bit write keeps the
   other 24 bits (x86 partial-register semantics). */
V86_INLINE WORD V86Get16(V86_CPU *cpu, int registerIndex) { return (WORD)cpu->Registers[registerIndex & X86_MODRM_REGISTER_MASK]; }
V86_INLINE VOID     V86Set16(V86_CPU *cpu, int registerIndex, WORD value) { cpu->Registers[registerIndex & X86_MODRM_REGISTER_MASK] = (cpu->Registers[registerIndex & X86_MODRM_REGISTER_MASK] & HIGH_WORD_MASK_U) | value; }
V86_INLINE BYTE  V86Get8(V86_CPU *cpu, int registerIndex)
{ return (registerIndex < X86_BYTE_REGISTERS) ? (BYTE)cpu->Registers[registerIndex] : (BYTE)(cpu->Registers[registerIndex - X86_BYTE_REGISTERS] >> BYTE_SHIFT); }
V86_INLINE VOID     V86Set8(V86_CPU *cpu, int registerIndex, BYTE value)
{ if (registerIndex < X86_BYTE_REGISTERS) cpu->Registers[registerIndex] = (cpu->Registers[registerIndex] & (~BYTE_MASK_U)) | value;
  else cpu->Registers[registerIndex - X86_BYTE_REGISTERS] = (cpu->Registers[registerIndex - X86_BYTE_REGISTERS] & (~HIGH_BYTE_MASK_U)) | ((UINT32)value << BYTE_SHIFT); }
/* read/write a register by operand width (1/2/4). */
V86_INLINE UINT32 V86GetRegister(V86_CPU *cpu, int registerIndex, int width)
{ return (width == 1) ? V86Get8(cpu, registerIndex) : (width == X86_WORD_SIZE) ? (UINT32)(WORD)cpu->Registers[registerIndex & X86_MODRM_REGISTER_MASK] : cpu->Registers[registerIndex & X86_MODRM_REGISTER_MASK]; }
V86_INLINE VOID V86SetRegister(V86_CPU *cpu, int registerIndex, int width, UINT32 value)
{ if (width == 1) V86Set8(cpu, registerIndex, (BYTE)value); else if (width == X86_WORD_SIZE) V86Set16(cpu, registerIndex, (WORD)value); else cpu->Registers[registerIndex & X86_MODRM_REGISTER_MASK] = value; }

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
    UINT32 result = full & mask, newFlags = cpu->Flags & ~(EFLAGS_CF_U | EFLAGS_PF_U | EFLAGS_AF_U | EFLAGS_ZF_U | EFLAGS_SF_U | EFLAGS_OF_U);
    if (width == X86_DWORD_SIZE ? (carryIn ? full <= maskedFirst : full < maskedFirst) : (full > mask)) newFlags |= EFLAGS_CF_U;
    if ((maskedFirst ^ maskedSecond ^ result) & EFLAGS_AF_U)                newFlags |= EFLAGS_AF_U;
    if (!result)                                   newFlags |= EFLAGS_ZF_U;
    if (result & signBit)                               newFlags |= EFLAGS_SF_U;
    if (V86Parity((BYTE)result))                  newFlags |= EFLAGS_PF_U;
    if ((~(maskedFirst ^ maskedSecond) & (maskedFirst ^ result)) & signBit)         newFlags |= EFLAGS_OF_U;
    cpu->Flags = newFlags;
    return result;
}
V86_INLINE UINT32 V86Subtract(V86_CPU *cpu, UINT32 first, UINT32 second, int borrowIn, int width)
{
    UINT32 mask = V86Mask(width), signBit = V86SignBit(width);
    UINT32 maskedFirst = first & mask, maskedSecond = second & mask, result = (maskedFirst - maskedSecond - (UINT32)borrowIn) & mask;
    UINT32 newFlags = cpu->Flags & ~(EFLAGS_CF_U | EFLAGS_PF_U | EFLAGS_AF_U | EFLAGS_ZF_U | EFLAGS_SF_U | EFLAGS_OF_U);
    if (borrowIn ? maskedFirst <= maskedSecond : maskedFirst < maskedSecond)               newFlags |= EFLAGS_CF_U;
    if ((maskedFirst ^ maskedSecond ^ result) & EFLAGS_AF_U)                newFlags |= EFLAGS_AF_U;
    if (!result)                                   newFlags |= EFLAGS_ZF_U;
    if (result & signBit)                               newFlags |= EFLAGS_SF_U;
    if (V86Parity((BYTE)result))                  newFlags |= EFLAGS_PF_U;
    if (((maskedFirst ^ maskedSecond) & (maskedFirst ^ result)) & signBit)          newFlags |= EFLAGS_OF_U;
    cpu->Flags = newFlags;
    return result;
}
V86_INLINE VOID V86Logic(V86_CPU *cpu, UINT32 result, int width)
{
    UINT32 mask = V86Mask(width), signBit = V86SignBit(width);
    UINT32 newFlags = cpu->Flags & ~(EFLAGS_CF_U | EFLAGS_PF_U | EFLAGS_AF_U | EFLAGS_ZF_U | EFLAGS_SF_U | EFLAGS_OF_U);   /* CF=OF=0 */
    result &= mask;
    if (!result)                  newFlags |= EFLAGS_ZF_U;
    if (result & signBit)              newFlags |= EFLAGS_SF_U;
    if (V86Parity((BYTE)result)) newFlags |= EFLAGS_PF_U;
    cpu->Flags = newFlags;
}

/* aluop encoding 0..7 = ADD OR ADC SBB AND SUB XOR CMP. Returns result;
   CMP (7) computes flags only. */
V86_INLINE UINT32 V86Alu(V86_CPU *cpu, int operation, UINT32 first, UINT32 second, int width)
{
    switch (operation) {
    case X86_ALU_ADD: return V86Add(cpu, first, second, 0, width);
    case X86_ALU_OR: { UINT32 result = first | second; V86Logic(cpu, result, width); return result; }
    case X86_ALU_ADC: return V86Add(cpu, first, second, (cpu->Flags & EFLAGS_CF_U) ? 1 : 0, width);
    case X86_ALU_SBB: return V86Subtract(cpu, first, second, (cpu->Flags & EFLAGS_CF_U) ? 1 : 0, width);
    case X86_ALU_AND: { UINT32 result = first & second; V86Logic(cpu, result, width); return result; }
    case X86_ALU_SUB: return V86Subtract(cpu, first, second, 0, width);
    case X86_ALU_XOR: { UINT32 result = first ^ second; V86Logic(cpu, result, width); return result; }
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
    int carry = (cpu->Flags & EFLAGS_CF_U) ? 1 : 0, oldCarry, index;
    count &= X86_SHIFT_COUNT_MASK; value &= mask; original = value;
    if (count == 0) return value;                            /* x86: flags unchanged */
    for (index = 0; index < count; ++index) switch (operation) {
        case X86_SHIFT_ROL: carry = (value & signBit) ? 1 : 0; value = ((value << 1) | (UINT32)carry) & mask; break;          /* ROL */
        case X86_SHIFT_ROR: carry = value & 1; value = ((value >> 1) | (carry ? signBit : 0)) & mask; break;                     /* ROR */
        case X86_SHIFT_RCL: oldCarry = carry; carry = (value & signBit) ? 1 : 0; value = ((value << 1) | (UINT32)oldCarry) & mask; break; /* RCL */
        case X86_SHIFT_RCR: oldCarry = carry; carry = value & 1; value = ((value >> 1) | (oldCarry ? signBit : 0)) & mask; break;      /* RCR */
        case X86_SHIFT_SHL: case X86_SHIFT_SAL: carry = (value & signBit) ? 1 : 0; value = (value << 1) & mask; break;                    /* SHL/SAL */
        case X86_SHIFT_SHR: carry = value & 1; value = (value >> 1) & mask; break;                                       /* SHR */
        default: carry = value & 1; value = ((value >> 1) | (value & signBit)) & mask; break;                         /* SAR */
    }
    cpu->Flags = (cpu->Flags & ~EFLAGS_CF_U) | (carry ? EFLAGS_CF_U : 0);
    if (count == 1) {                                    /* OF only defined for count 1 */
        int overflow;
        switch (operation) {
        case X86_SHIFT_SHR:  overflow = (original & signBit) ? 1 : 0; break;                      /* SHR: MSB of orig */
        case X86_SHIFT_SAR:  overflow = 0; break;                                        /* SAR */
        case X86_SHIFT_ROR: case X86_SHIFT_RCR: overflow = (((value & signBit) ? 1 : 0) ^ ((value & (signBit >> 1)) ? 1 : 0)); break; /* ROR/RCR */
        default: overflow = (((value & signBit) ? 1 : 0) ^ carry); break;                /* ROL/RCL/SHL */
        }
        cpu->Flags = (cpu->Flags & ~EFLAGS_OF_U) | (overflow ? EFLAGS_OF_U : 0);
    }
    if (operation >= X86_SHIFT_SHL) {                                    /* shifts set SF/ZF/PF (not rotates) */
        cpu->Flags &= ~(EFLAGS_SF_U | EFLAGS_ZF_U | EFLAGS_PF_U);
        if (!value) cpu->Flags |= EFLAGS_ZF_U;
        if (value & signBit) cpu->Flags |= EFLAGS_SF_U;
        if (V86Parity((BYTE)value)) cpu->Flags |= EFLAGS_PF_U;
    }
    return value;
}

V86_INLINE int V86Condition(V86_CPU *cpu, int condition)
{
    int carry = !!(cpu->Flags & EFLAGS_CF_U), zero = !!(cpu->Flags & EFLAGS_ZF_U), sign = !!(cpu->Flags & EFLAGS_SF_U),
        overflow = !!(cpu->Flags & EFLAGS_OF_U), parity = !!(cpu->Flags & EFLAGS_PF_U);
    switch (condition & X86_CONDITION_MASK) {
    case V86_CC_OVERFLOW: return overflow;            case V86_CC_NOT_OVERFLOW: return !overflow;
    case V86_CC_BELOW: return carry;            case V86_CC_NOT_BELOW: return !carry;
    case V86_CC_ZERO: return zero;            case V86_CC_NOT_ZERO: return !zero;
    case V86_CC_BELOW_OR_EQUAL: return carry || zero;      case V86_CC_ABOVE: return !(carry || zero);
    case V86_CC_SIGN: return sign;            case V86_CC_NOT_SIGN: return !sign;
    case V86_CC_PARITY: return parity;            case V86_CC_NOT_PARITY: return !parity;
    case V86_CC_LESS: return sign != overflow;      case V86_CC_NOT_LESS: return sign == overflow;
    case V86_CC_LESS_OR_EQUAL: return zero || (sign != overflow);
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
                          | ((width) >= X86_WORD_SIZE ? (UINT32)codePointer[(position) + 1] << BYTE_SHIFT : 0u) \
                          | ((width) == X86_DWORD_SIZE ? ((UINT32)codePointer[(position) + V86_THIRD_BYTE] << WORD_SHIFT) | ((UINT32)codePointer[(position) + V86_FOURTH_BYTE] << TOP_BYTE_SHIFT) : 0u) \
                        : V86ReadMemory(codeLinear + (UINT32)(position), (width)))

/* Decode a 16-bit ModRM byte at CB(idx). Fills *o (is_mem + linear addr or rm
   register, plus the reg field g). Returns bytes consumed (ModRM + disp). */
typedef struct _V86_MODRM { int IsMemory; UINT32 Linear; WORD EffectiveAddress; int Register; int RegisterMemory; } V86_MODRM;
static int V86DecodeModrm(V86_CPU *cpu, UINT32 codeLinear, const volatile BYTE *codePointer, int offset, int segmentOverride, V86_MODRM *out)
{
    BYTE modrmByte = V86_CODE_BYTE(offset); int mode = modrmByte >> X86_MODRM_MODE_SHIFT, registerMemory = modrmByte & X86_MODRM_REGISTER_MASK, length = 1, isStackBased = 0;
    WORD effectiveAddress = 0, bxValue = cpu->Registers[X86_REG_BX], bpValue = cpu->Registers[X86_REG_BP], siValue = cpu->Registers[X86_REG_SI], diValue = cpu->Registers[X86_REG_DI];
    out->Register = (modrmByte >> X86_MODRM_REG_SHIFT) & X86_MODRM_REGISTER_MASK;
    if (mode == X86_MODE_REGISTER) { out->IsMemory = 0; out->RegisterMemory = registerMemory; out->EffectiveAddress = 0; return 1; }
    switch (registerMemory) {
    case V86_RM_BX_SI: effectiveAddress = (WORD)(bxValue + siValue); break;  case V86_RM_BX_DI: effectiveAddress = (WORD)(bxValue + diValue); break;
    case V86_RM_BP_SI: effectiveAddress = (WORD)(bpValue + siValue); isStackBased = 1; break;
    case V86_RM_BP_DI: effectiveAddress = (WORD)(bpValue + diValue); isStackBased = 1; break;
    case V86_RM_SI: effectiveAddress = siValue; break;                   case V86_RM_DI: effectiveAddress = diValue; break;
    case V86_RM_BP: if (mode == V86_MODE_NO_DISP) { effectiveAddress = (WORD)(V86_CODE_BYTE(offset + 1) | (V86_CODE_BYTE(offset + X86_WORD_SIZE) << BYTE_SHIFT)); length += X86_WORD_SIZE; }
            else { effectiveAddress = bpValue; isStackBased = 1; } break;
    default: effectiveAddress = bxValue; break;
    }
    if (mode == X86_MODE_DISP8)      { effectiveAddress = (WORD)(effectiveAddress + (INT16)(signed char)V86_CODE_BYTE(offset + length)); length += 1; }
    else if (mode == V86_MODE_DISP16) { effectiveAddress = (WORD)(effectiveAddress + (V86_CODE_BYTE(offset + length) | (V86_CODE_BYTE(offset + length + 1) << BYTE_SHIFT))); length += X86_WORD_SIZE; }
    out->IsMemory = 1; out->EffectiveAddress = effectiveAddress;
    out->Linear = (V86SegmentBase(cpu->Segments[(segmentOverride >= 0) ? segmentOverride : (isStackBased ? X86_SREG_SS : X86_SREG_DS)])) + effectiveAddress;  /* SS if BP else DS */
    return length;
}

/* Where the guest is, for instruments downstream of a memory access. Every planar
   VRAM write reaches the video VDD through this interpreter, so a watchpoint there
   can name the guest routine responsible -- which is the difference between "some
   idiom wrote 0xFF" and an address to disassemble. One store per instruction. */
static UINT32 g_V86InstructionPointer;

/* Execute one instruction. Returns 1 if modeled (state + IP advanced/jumped),
   0 to bail (state untouched at the current instruction). */
/* V86_STEP_LINKAGE: `static` unless the includer says otherwise -- the host's one copy of the
   interpreter (host_video.c) makes V86Step external so host_dpmi.c's loop can call it (#335). */
#ifndef V86_STEP_LINKAGE
#define V86_STEP_LINKAGE static
#endif
V86_STEP_LINKAGE int V86Step(V86_CPU *cpu)
{
    UINT32 codeLinear = (V86SegmentBase(cpu->Segments[X86_SREG_CS])) + cpu->Ip;   /* linear CS:IP */
    const volatile BYTE *codePointer = V86_CODE(codeLinear);              /* NULL = fetch through V86HostRead8 */
    g_V86InstructionPointer = ((UINT32)cpu->Segments[X86_SREG_CS] << WORD_SHIFT) | cpu->Ip;
    int offset = 0, segmentOverride = -1, repeat = 0, isOperand32 = 0;
    int operandSize;                                            /* word operand width: 4 if 0x66 else 2 */
    BYTE opcode;
    for (;;) {                                        /* prefixes */
        BYTE prefix = V86_CODE_BYTE(offset);
        if      (prefix == X86_PREFIX_ES) { segmentOverride = X86_SREG_ES; offset++; }     /* ES */
        else if (prefix == X86_PREFIX_CS) { segmentOverride = X86_SREG_CS; offset++; }     /* CS */
        else if (prefix == X86_PREFIX_SS) { segmentOverride = X86_SREG_SS; offset++; }     /* SS */
        else if (prefix == X86_PREFIX_DS) { segmentOverride = X86_SREG_DS; offset++; }     /* DS */
        else if (prefix == X86_PREFIX_FS) { segmentOverride = X86_SREG_FS; offset++; }     /* FS */
        else if (prefix == X86_PREFIX_GS) { segmentOverride = X86_SREG_GS; offset++; }     /* GS */
        else if (prefix == X86_PREFIX_REP) { repeat = X86_REPEAT_REP; offset++; }
        else if (prefix == X86_PREFIX_REPNE) { repeat = X86_REPEAT_REPNE; offset++; }
        else if (prefix == X86_PREFIX_OPERAND_SIZE) { isOperand32 = 1; offset++; }       /* run 54: operand-size -> 32-bit */
        else if (prefix == X86_PREFIX_ADDRESS_SIZE || prefix == X86_PREFIX_LOCK) return 0;    /* addr-size / LOCK: still bail */
        else break;
        if (offset > X86_PREFIXES_MAX) return 0;
    }
    opcode = V86_CODE_BYTE(offset++);
    operandSize = isOperand32 ? X86_DWORD_SIZE : X86_WORD_SIZE;                                  /* run 54: word operand width (0x66 -> 4) */

    /* ---- 0F two-byte map (run 54): MOVZX/MOVSX r, r/m -------------------- *
     * B6/B7 = zero-extend byte/word, BE/BF = sign-extend. Dest width = W, so *
     * `66 0F B7` gives MOVZX ESI,SI. Other 0F ops bail (the to-do signal).   */
    if (opcode == X86_ESCAPE) {
        BYTE opcode2 = V86_CODE_BYTE(offset++);
        if (opcode2 == X86_OP2_MOVZX_BYTE || opcode2 == X86_OP2_MOVZX_WORD || opcode2 == X86_OP2_MOVSX_BYTE || opcode2 == X86_OP2_MOVSX_WORD) {
            int sourceWidth = (opcode2 & 1) ? X86_WORD_SIZE : 1;               /* source width */
            int isSignExtend = (opcode2 >= X86_OP2_MOVSX_BYTE);                   /* sign- vs zero-extend */
            V86_MODRM modrm; offset += V86DecodeModrm(cpu, codeLinear, codePointer, offset, segmentOverride, &modrm);
            if (modrm.IsMemory && modrm.Linear >= V86_GUEST_LIMIT) return 0;
            { UINT32 value = modrm.IsMemory ? V86ReadMemory(modrm.Linear, sourceWidth)
                                    : (sourceWidth == 1 ? V86Get8(cpu, modrm.RegisterMemory) : (UINT32)V86Get16(cpu, modrm.RegisterMemory));
              if (isSignExtend && (value & V86SignBit(sourceWidth))) value |= ~V86Mask(sourceWidth);   /* sign-extend to 32 */
              V86SetRegister(cpu, modrm.Register, operandSize, value & V86Mask(operandSize)); }
            cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
        }
        if (opcode2 == V86_OP2_LAR || opcode2 == V86_OP2_LSL) {             /* LAR / LSL r, r/m16 (run 55) */
            V86_MODRM modrm; WORD selector; UINT32 accessRights, limit;
            if (!g_V86SelectorDescriptor) return 0;                /* V86: no descriptor table -> bail */
            offset += V86DecodeModrm(cpu, codeLinear, codePointer, offset, segmentOverride, &modrm);
            if (modrm.IsMemory && modrm.Linear >= V86_GUEST_LIMIT) return 0;
            selector = modrm.IsMemory ? (WORD)V86ReadMemory(modrm.Linear, X86_WORD_SIZE) : V86Get16(cpu, modrm.RegisterMemory);  /* selector is 16-bit */
            if (g_V86SelectorDescriptor(selector, &accessRights, &limit)) {         /* valid -> load rights/limit, set ZF */
                V86SetRegister(cpu, modrm.Register, operandSize, (opcode2 == V86_OP2_LAR ? accessRights : limit) & V86Mask(operandSize));
                cpu->Flags |= EFLAGS_ZF_U;
            } else cpu->Flags &= ~EFLAGS_ZF_U;                 /* invalid -> clear ZF, dest unchanged */
            cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
        }
        /* PUSH/POP FS and GS (0F A0/A1/A8/A9), either width -- #194. The 16-bit form
           was unmodelled too: the 386 encodings live only in the 0F map. */
        if (opcode2 == V86_OP2_PUSH_FS || opcode2 == V86_OP2_PUSH_GS) {
            V86PushSegment(cpu, operandSize, cpu->Segments[opcode2 == V86_OP2_PUSH_FS ? X86_SREG_FS : X86_SREG_GS]);
            cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
        }
        if (opcode2 == V86_OP2_POP_FS || opcode2 == V86_OP2_POP_GS) {
            cpu->Segments[opcode2 == V86_OP2_POP_FS ? X86_SREG_FS : X86_SREG_GS] = (WORD)V86Pop(cpu, operandSize);
            cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
        }
        /* #269: Jcc rel16 (0F 80-8F) -- the near form 386-targeted 16-bit code uses for
           any branch past 127 bytes. 16-bit operand size: IP = next + rel16, wrapping
           in the segment as IP does. With 0x66 it is rel32 and EIP = next + rel32, bailed
           if it leaves the 64 KB segment (the CPU's #GP, not ours) -- the E9 rule. */
        if (opcode2 >= X86_JCC_NEAR_FIRST && opcode2 <= X86_JCC_NEAR_LAST) {
            int isTaken = V86Condition(cpu, opcode2 & X86_CONDITION_MASK);
            if (isOperand32) {
                UINT32 nextIp = (UINT32)(WORD)(cpu->Ip + offset + X86_DWORD_SIZE);
                UINT32 target = nextIp + V86ReadMemory(codeLinear + offset, X86_DWORD_SIZE);
                if (isTaken && target > V86_OFFSET_MAX) return 0;
                cpu->Ip = (WORD)(isTaken ? target : nextIp); return 1;
            } else {
                INT16 relative = (INT16)(V86_CODE_BYTE(offset) | (V86_CODE_BYTE(offset + 1) << BYTE_SHIFT)); offset += X86_WORD_SIZE;
                cpu->Ip = (WORD)(cpu->Ip + offset + (isTaken ? relative : 0)); return 1;
            }
        }
        /* #269: SETcc r/m8 (0F 90-9F) -- 1 if the condition holds, else 0. The reg
           field of the ModR/M is not used; no flags change. */
        if (opcode2 >= X86_OP2_SETCC_FIRST && opcode2 <= X86_OP2_SETCC_LAST) {
            V86_MODRM modrm; BYTE value = (BYTE)(V86Condition(cpu, opcode2 & X86_CONDITION_MASK) ? 1 : 0);
            offset += V86DecodeModrm(cpu, codeLinear, codePointer, offset, segmentOverride, &modrm);
            if (modrm.IsMemory && modrm.Linear >= V86_GUEST_LIMIT) return 0;
            if (modrm.IsMemory) V86WriteMemory(modrm.Linear, 1, value);
            else          V86Set8(cpu, modrm.RegisterMemory, value);
            cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
        }
        return 0;                                     /* other 0F ops: bail */
    }

    /* ---- arithmetic/logic group: ADD..CMP, reg/mem forms ------------------ */
    if (opcode < X86_OP_INC_FIRST && (opcode & X86_MODRM_REGISTER_MASK) < V86_ALU_FORM_COUNT) {
        int aluOperation = (opcode >> X86_MODRM_REG_SHIFT) & X86_MODRM_REGISTER_MASK, form = opcode & X86_MODRM_REGISTER_MASK;
        int width = (form == V86_ALU_FORM_RM_BYTE || form == V86_ALU_FORM_REG_BYTE || form == V86_ALU_FORM_AL_IMM) ? 1 : operandSize;
        UINT32 first, second, result; int isDestinationMemory = 0, destinationRegister = 0; UINT32 destinationLinear = 0;
        if (form <= V86_ALU_FORM_REG) {
            V86_MODRM modrm; offset += V86DecodeModrm(cpu, codeLinear, codePointer, offset, segmentOverride, &modrm);
            if (modrm.IsMemory && modrm.Linear >= V86_GUEST_LIMIT) return 0;
            UINT32 rmValue = modrm.IsMemory ? V86ReadMemory(modrm.Linear, width) : V86GetRegister(cpu, modrm.RegisterMemory, width);
            UINT32 registerValue = V86GetRegister(cpu, modrm.Register, width);
            if (form <= V86_ALU_FORM_RM) { first = rmValue; second = registerValue; if (modrm.IsMemory) { isDestinationMemory = 1; destinationLinear = modrm.Linear; } else destinationRegister = modrm.RegisterMemory; }
            else           { first = registerValue; second = rmValue; destinationRegister = modrm.Register; }
        } else if (form == V86_ALU_FORM_AL_IMM) { first = V86Get8(cpu, X86_REG_AX);  second = V86_CODE_BYTE(offset++); destinationRegister = 0; }
        else { first = V86GetRegister(cpu, X86_REG_AX, width); second = V86_IMMEDIATE(offset, width); offset += width; destinationRegister = 0; }
        result = V86Alu(cpu, aluOperation, first, second, width);
        if (aluOperation != X86_ALU_CMP) {
            if (isDestinationMemory) V86WriteMemory(destinationLinear, width, result);
            else      V86SetRegister(cpu, destinationRegister, width, result);
        }
        cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
    }

    /* ---- group1: ADD..CMP r/m, imm (80/81/83) ----------------------------- */
    if (opcode == X86_OP_GROUP1_IMM8 || opcode == X86_OP_GROUP1_IMM || opcode == X86_OP_GROUP1_SIMM8) {
        int width = (opcode == X86_OP_GROUP1_IMM8) ? 1 : operandSize; UINT32 first, second, result;
        V86_MODRM modrm; offset += V86DecodeModrm(cpu, codeLinear, codePointer, offset, segmentOverride, &modrm);
        if (modrm.IsMemory && modrm.Linear >= V86_GUEST_LIMIT) return 0;
        first = modrm.IsMemory ? V86ReadMemory(modrm.Linear, width) : V86GetRegister(cpu, modrm.RegisterMemory, width);
        if (opcode == X86_OP_GROUP1_IMM) { second = V86_IMMEDIATE(offset, width); offset += width; }
        else { second = (UINT32)(INT32)(INT8)V86_CODE_BYTE(offset++); second &= V86Mask(width); }
        result = V86Alu(cpu, modrm.Register, first, second, width);
        if (modrm.Register != X86_ALU_CMP) {
            if (modrm.IsMemory) V86WriteMemory(modrm.Linear, width, result);
            else          V86SetRegister(cpu, modrm.RegisterMemory, width, result);
        }
        cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
    }

    /* ---- INC/DEC r16/r32 (40-4F) ------------------------------------------ */
    if (opcode >= X86_OP_INC_FIRST && opcode <= X86_OP_DEC_LAST) {
        int registerIndex = opcode & X86_MODRM_REGISTER_MASK; UINT32 cf = cpu->Flags & EFLAGS_CF_U;
        UINT32 result = (opcode >= X86_OP_DEC_FIRST) ? V86Subtract(cpu, V86GetRegister(cpu, registerIndex, operandSize), 1, 0, operandSize)
                                    : V86Add(cpu, V86GetRegister(cpu, registerIndex, operandSize), 1, 0, operandSize);
        cpu->Flags = (cpu->Flags & ~EFLAGS_CF_U) | cf;           /* INC/DEC preserve CF */
        V86SetRegister(cpu, registerIndex, operandSize, result);
        cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
    }

    /* ---- group FE/FF: INC/DEC r/m, and (FF only) near indirect CALL/JMP +
            PUSH r/m. Far call/jmp (g=3/5) bail. ---------------------------- */
    if (opcode == X86_OP_GROUP4 || opcode == X86_OP_GROUP5) {
        int width = (opcode == X86_OP_GROUP4) ? 1 : operandSize;
        V86_MODRM modrm; offset += V86DecodeModrm(cpu, codeLinear, codePointer, offset, segmentOverride, &modrm);
        if (modrm.IsMemory && modrm.Linear >= V86_GUEST_LIMIT) return 0;
        if (modrm.Register == 0 || modrm.Register == 1) {                   /* INC/DEC r/m */
            UINT32 cf = cpu->Flags & EFLAGS_CF_U;
            UINT32 first = modrm.IsMemory ? V86ReadMemory(modrm.Linear, width) : V86GetRegister(cpu, modrm.RegisterMemory, width);
            UINT32 result = (modrm.Register == 1) ? V86Subtract(cpu, first, 1, 0, width) : V86Add(cpu, first, 1, 0, width);
            cpu->Flags = (cpu->Flags & ~EFLAGS_CF_U) | cf;
            if (modrm.IsMemory) V86WriteMemory(modrm.Linear, width, result);
            else          V86SetRegister(cpu, modrm.RegisterMemory, width, result);
            cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
        }
        if (opcode != X86_OP_GROUP5) return 0;                      /* FE has nothing past INC/DEC */
        if (isOperand32) {
            /* #194: the 32-bit forms. CALL/JMP near take a 32-bit EIP (bail past the
               64 KB limit -- see the #194 note at the top); PUSH r/m32; and CALL/JMP
               FAR m16:32 -- offset dword, then the selector -- real mode only, as the
               16-bit far forms below are. */
            UINT32 value32 = modrm.IsMemory ? V86ReadMemory(modrm.Linear, X86_DWORD_SIZE) : cpu->Registers[modrm.RegisterMemory & X86_MODRM_REGISTER_MASK];
            UINT32 nextIp = (UINT32)(WORD)(cpu->Ip + offset);
            if (modrm.Register == X86_GROUP5_CALL || modrm.Register == X86_GROUP5_JMP) {
                if (value32 > V86_OFFSET_MAX) return 0;
                if (modrm.Register == X86_GROUP5_CALL) V86Push(cpu, X86_DWORD_SIZE, nextIp);
                cpu->Ip = (WORD)value32; return 1;
            }
            if (modrm.Register == X86_GROUP5_PUSH) { V86Push(cpu, X86_DWORD_SIZE, value32); cpu->Ip = (WORD)nextIp; return 1; }
            if ((modrm.Register == V86_GROUP5_CALL_FAR || modrm.Register == V86_GROUP5_JMP_FAR) && modrm.IsMemory && !g_V86SegmentToLinear) {
                WORD segment = (WORD)V86ReadMemory(modrm.Linear + X86_DWORD_SIZE, X86_WORD_SIZE);
                if (value32 > V86_OFFSET_MAX) return 0;
                if (modrm.Register == V86_GROUP5_CALL_FAR) { V86Push(cpu, X86_DWORD_SIZE, cpu->Segments[X86_SREG_CS]); V86Push(cpu, X86_DWORD_SIZE, nextIp); }  /* CS slot: dword */
                cpu->Segments[X86_SREG_CS] = segment; cpu->Ip = (WORD)value32; return 1;
            }
            return 0;
        }
        { WORD value16 = modrm.IsMemory ? (WORD)V86ReadMemory(modrm.Linear, X86_WORD_SIZE) : V86Get16(cpu, modrm.RegisterMemory);
          WORD nextIp = (WORD)(cpu->Ip + offset);
          if (modrm.Register == X86_GROUP5_CALL) {                              /* CALL near indirect */
              WORD stackPointer = (WORD)(cpu->Registers[X86_REG_SP] - X86_WORD_SIZE);
              V86WriteMemory((V86SegmentBase(cpu->Segments[X86_SREG_SS])) + stackPointer, X86_WORD_SIZE, nextIp);
              cpu->Registers[X86_REG_SP] = stackPointer; cpu->Ip = value16; return 1;
          }
          if (modrm.Register == X86_GROUP5_JMP) { cpu->Ip = value16; return 1; }     /* JMP near indirect */
          if (modrm.Register == X86_GROUP5_PUSH) {                              /* PUSH r/m16 */
              WORD stackPointer = (WORD)(cpu->Registers[X86_REG_SP] - X86_WORD_SIZE);
              V86WriteMemory((V86SegmentBase(cpu->Segments[X86_SREG_SS])) + stackPointer, X86_WORD_SIZE, value16);
              cpu->Registers[X86_REG_SP] = stackPointer; cpu->Ip = nextIp; return 1;
          }
          /* CALL FAR m16:16 (/3) and JMP FAR m16:16 (/5): the seg:off is IN MEMORY.
             (s68) Lemmings dispatches through `jmp far [0x1fbe]` on every frame and
             this bailed to V86 -- and in a planar mode a bail is not one instruction,
             it is everything up to the next event, with A0000 unprotected. Same shape
             as the scasb leak; same fix. Real mode only, like EA/9A above. */
          if ((modrm.Register == V86_GROUP5_CALL_FAR || modrm.Register == V86_GROUP5_JMP_FAR) && modrm.IsMemory && !g_V86SegmentToLinear) {
              WORD segment = (WORD)V86ReadMemory(modrm.Linear + X86_WORD_SIZE, X86_WORD_SIZE);
              if (modrm.Register == V86_GROUP5_CALL_FAR) {
                  UINT32 stackBase = V86SegmentBase(cpu->Segments[X86_SREG_SS]);
                  WORD stackPointer = (WORD)(cpu->Registers[X86_REG_SP] - X86_WORD_SIZE); V86WriteMemory(stackBase + stackPointer, X86_WORD_SIZE, cpu->Segments[X86_SREG_CS]);
                  stackPointer = (WORD)(stackPointer - X86_WORD_SIZE);               V86WriteMemory(stackBase + stackPointer, X86_WORD_SIZE, nextIp);
                  cpu->Registers[X86_REG_SP] = (cpu->Registers[X86_REG_SP] & HIGH_WORD_MASK_U) | stackPointer;
              }
              cpu->Segments[X86_SREG_CS] = segment; cpu->Ip = value16; return 1;
          } }
        return 0;                                      /* g=7 / reg-form far: bail */
    }

    /* ---- TEST r/m,r (84/85); TEST AL/AX,imm (A8/A9) ----------------------- */
    if (opcode == X86_OP_TEST_BYTE || opcode == X86_OP_TEST) {
        int width = (opcode == X86_OP_TEST_BYTE) ? 1 : operandSize;
        V86_MODRM modrm; offset += V86DecodeModrm(cpu, codeLinear, codePointer, offset, segmentOverride, &modrm);
        if (modrm.IsMemory && modrm.Linear >= V86_GUEST_LIMIT) return 0;
        { UINT32 operand = modrm.IsMemory ? V86ReadMemory(modrm.Linear, width) : V86GetRegister(cpu, modrm.RegisterMemory, width);
          UINT32 registerValue = V86GetRegister(cpu, modrm.Register, width);
          V86Logic(cpu, operand & registerValue, width); }
        cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
    }
    if (opcode == X86_OP_TEST_IMM_BYTE) { V86Logic(cpu, (UINT32)V86Get8(cpu, X86_REG_AX) & V86_CODE_BYTE(offset), 1); offset++;
                      cpu->Ip = (WORD)(cpu->Ip + offset); return 1; }
    if (opcode == X86_OP_TEST_IMM) { UINT32 second = V86_IMMEDIATE(offset, operandSize); offset += operandSize;
                      V86Logic(cpu, V86GetRegister(cpu, X86_REG_AX, operandSize) & second, operandSize);
                      cpu->Ip = (WORD)(cpu->Ip + offset); return 1; }
    /* ---- group3 (F6/F7): TEST r/m,imm (reg 0/1); NOT/NEG (2/3); MUL/IMUL     *
     * (4/5) -> [E]DX:[E]AX; DIV/IDIV (6/7) <- [E]DX:[E]AX. run 59's I310102     *
     * reached `66 F7 /6` = DIV EDI (printf's hex-digit divide loop). No #DE     *
     * trap path here, so a zero divisor or quotient overflow BAILS to V86       *
     * rather than emit UB (correct code never hits it). run 61. */
    if (opcode == X86_OP_GROUP3_BYTE || opcode == X86_OP_GROUP3) {
        int width = (opcode == X86_OP_GROUP3_BYTE) ? 1 : operandSize;
        V86_MODRM modrm; offset += V86DecodeModrm(cpu, codeLinear, codePointer, offset, segmentOverride, &modrm);
        if (modrm.IsMemory && modrm.Linear >= V86_GUEST_LIMIT) return 0;
        if (modrm.Register == 0 || modrm.Register == 1) {                    /* TEST r/m,imm */
            UINT32 operand = modrm.IsMemory ? V86ReadMemory(modrm.Linear, width) : V86GetRegister(cpu, modrm.RegisterMemory, width);
            UINT32 second = V86_IMMEDIATE(offset, width); offset += width;
            V86Logic(cpu, operand & second, width);
            cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
        }
        { UINT32 operand = modrm.IsMemory ? V86ReadMemory(modrm.Linear, width) : V86GetRegister(cpu, modrm.RegisterMemory, width);
          if (modrm.Register == X86_GROUP3_NOT) {                              /* NOT: no flags */
              UINT32 result = (~operand) & V86Mask(width);
              if (modrm.IsMemory) V86WriteMemory(modrm.Linear, width, result); else V86SetRegister(cpu, modrm.RegisterMemory, width, result);
          } else if (modrm.Register == X86_GROUP3_NEG) {                       /* NEG: 0 - e, flags like SUB */
              UINT32 result = V86Subtract(cpu, 0, operand, 0, width);
              if (modrm.IsMemory) V86WriteMemory(modrm.Linear, width, result); else V86SetRegister(cpu, modrm.RegisterMemory, width, result);
          } else if (modrm.Register == X86_GROUP3_MUL || modrm.Register == X86_GROUP3_IMUL) {           /* MUL (4) / IMUL (5) */
              int isOverflow;
              if (width == 1) {
                  UINT32 product = (modrm.Register == X86_GROUP3_MUL)
                      ? (cpu->Registers[X86_REG_AX] & BYTE_MASK_U) * (operand & BYTE_MASK_U)
                      : (UINT32)(INT32)((INT8)(cpu->Registers[X86_REG_AX] & BYTE_MASK) * (INT8)operand) & WORD_MASK_U;
                  cpu->Registers[X86_REG_AX] = (cpu->Registers[X86_REG_AX] & HIGH_WORD_MASK_U) | (product & WORD_MASK_U);
                  isOverflow = (modrm.Register == X86_GROUP3_MUL) ? ((product >> BYTE_SHIFT) != 0)
                                   : ((INT16)product != (INT8)(product & BYTE_MASK));
              } else if (width == X86_WORD_SIZE) {
                  UINT32 product = (modrm.Register == X86_GROUP3_MUL)
                      ? (cpu->Registers[X86_REG_AX] & WORD_MASK_U) * (operand & WORD_MASK_U)
                      : (UINT32)(INT32)((INT16)(cpu->Registers[X86_REG_AX] & WORD_MASK) * (INT16)operand);
                  cpu->Registers[X86_REG_AX] = (cpu->Registers[X86_REG_AX] & HIGH_WORD_MASK_U) | (product & WORD_MASK_U);
                  cpu->Registers[X86_REG_DX] = (cpu->Registers[X86_REG_DX] & HIGH_WORD_MASK_U) | ((product >> WORD_SHIFT) & WORD_MASK_U);
                  isOverflow = (modrm.Register == X86_GROUP3_MUL) ? ((product >> WORD_SHIFT) != 0)
                                   : ((INT32)product != (INT16)(product & WORD_MASK));
              } else {                                  /* w == 4 */
                  UINT64 product = (modrm.Register == X86_GROUP3_MUL)
                      ? (UINT64)cpu->Registers[X86_REG_AX] * (UINT64)operand
                      : (UINT64)((INT64)(INT32)cpu->Registers[X86_REG_AX] * (INT64)(INT32)operand);
                  cpu->Registers[X86_REG_AX] = (UINT32)product;
                  cpu->Registers[X86_REG_DX] = (UINT32)(product >> DWORD_SHIFT);
                  isOverflow = (modrm.Register == X86_GROUP3_MUL) ? ((product >> DWORD_SHIFT) != 0)
                                   : ((INT64)product != (INT32)product);
              }
              cpu->Flags = (cpu->Flags & ~(EFLAGS_CF_U | EFLAGS_OF_U)) | (isOverflow ? (EFLAGS_CF_U | EFLAGS_OF_U) : 0);
          } else {                                     /* m.g == 6 DIV / 7 IDIV */
              if (operand == 0) return 0;                     /* #DE (div by zero): bail */
              if (width == 1) {
                  if (modrm.Register == X86_GROUP3_DIV) { UINT32 dividend = cpu->Registers[X86_REG_AX] & WORD_MASK_U, quotient = dividend / (operand & BYTE_MASK_U), remainder = dividend % (operand & BYTE_MASK_U);
                      if (quotient > BYTE_MASK) return 0;            /* #DE quotient overflow */
                      cpu->Registers[X86_REG_AX] = (cpu->Registers[X86_REG_AX] & HIGH_WORD_MASK_U) | (quotient & BYTE_MASK) | ((remainder & BYTE_MASK) << BYTE_SHIFT); }
                  else { INT16 dividend = (INT16)(cpu->Registers[X86_REG_AX] & WORD_MASK); INT8 divisor = (INT8)operand;
                      INT32 quotient = dividend / divisor, remainder = dividend % divisor; if (quotient > INT8_MAX_VALUE || quotient < INT8_MIN_VALUE) return 0;
                      cpu->Registers[X86_REG_AX] = (cpu->Registers[X86_REG_AX] & HIGH_WORD_MASK_U) | (quotient & BYTE_MASK) | ((remainder & BYTE_MASK) << BYTE_SHIFT); }
              } else if (width == X86_WORD_SIZE) {
                  UINT32 dividend = ((cpu->Registers[X86_REG_DX] & WORD_MASK_U) << WORD_SHIFT) | (cpu->Registers[X86_REG_AX] & WORD_MASK_U);
                  if (modrm.Register == X86_GROUP3_DIV) { UINT32 quotient = dividend / (operand & WORD_MASK_U), remainder = dividend % (operand & WORD_MASK_U);
                      if (quotient > WORD_MASK) return 0;
                      cpu->Registers[X86_REG_AX] = (cpu->Registers[X86_REG_AX] & HIGH_WORD_MASK_U) | (quotient & WORD_MASK);
                      cpu->Registers[X86_REG_DX] = (cpu->Registers[X86_REG_DX] & HIGH_WORD_MASK_U) | (remainder & WORD_MASK); }
                  else { INT32 signedDividend = (INT32)dividend, divisor = (INT16)operand, quotient = signedDividend / divisor, remainder = signedDividend % divisor;
                      if (quotient > INT16_MAX_VALUE || quotient < INT16_MIN_VALUE) return 0;
                      cpu->Registers[X86_REG_AX] = (cpu->Registers[X86_REG_AX] & HIGH_WORD_MASK_U) | (quotient & WORD_MASK);
                      cpu->Registers[X86_REG_DX] = (cpu->Registers[X86_REG_DX] & HIGH_WORD_MASK_U) | (remainder & WORD_MASK); }
              } else {                                  /* w == 4 */
                  UINT64 dividend = ((UINT64)cpu->Registers[X86_REG_DX] << DWORD_SHIFT) | (UINT64)cpu->Registers[X86_REG_AX];
                  if (modrm.Register == X86_GROUP3_DIV) { UINT64 quotient = dividend / operand, remainder = dividend % operand;
                      if (quotient > DWORD_MASK_U) return 0;
                      cpu->Registers[X86_REG_AX] = (UINT32)quotient; cpu->Registers[X86_REG_DX] = (UINT32)remainder; }
                  else { INT64 signedDividend = (INT64)dividend, divisor = (INT32)operand, quotient = signedDividend / divisor, remainder = signedDividend % divisor;
                      if (quotient > V86_INT32_MAX || quotient < V86_INT32_MIN) return 0;
                      cpu->Registers[X86_REG_AX] = (UINT32)quotient; cpu->Registers[X86_REG_DX] = (UINT32)remainder; }
              }
          }
        }
        cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
    }

    /* ---- MOV r/m<->reg (88-8B); MOV r/m,imm (C6/C7) ----------------------- */
    if (opcode == X86_OP_MOV_TO_RM_BYTE || opcode == X86_OP_MOV_TO_RM || opcode == X86_OP_MOV_FROM_RM_BYTE || opcode == X86_OP_MOV_FROM_RM) {
        int width = (opcode & 1) ? operandSize : 1, isLoad = (opcode == X86_OP_MOV_FROM_RM_BYTE || opcode == X86_OP_MOV_FROM_RM);
        V86_MODRM modrm; offset += V86DecodeModrm(cpu, codeLinear, codePointer, offset, segmentOverride, &modrm);
        if (modrm.IsMemory && modrm.Linear >= V86_GUEST_LIMIT) return 0;
        if (isLoad) { UINT32 value = modrm.IsMemory ? V86ReadMemory(modrm.Linear, width) : V86GetRegister(cpu, modrm.RegisterMemory, width);
                    V86SetRegister(cpu, modrm.Register, width, value); }
        else { UINT32 value = V86GetRegister(cpu, modrm.Register, width);
               if (modrm.IsMemory) V86WriteMemory(modrm.Linear, width, value);
               else          V86SetRegister(cpu, modrm.RegisterMemory, width, value); }
        cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
    }
    if (opcode == X86_OP_MOV_IMM_RM_BYTE || opcode == X86_OP_MOV_IMM_RM) {
        int width = (opcode == X86_OP_MOV_IMM_RM) ? operandSize : 1;
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
    if (opcode == X86_OP_XCHG_BYTE || opcode == X86_OP_XCHG) {
        int width = (opcode == X86_OP_XCHG) ? operandSize : 1;
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
    if (opcode >= X86_OP_PUSH_FIRST && opcode <= X86_OP_PUSH_LAST) {
        WORD stackPointer = (WORD)(cpu->Registers[X86_REG_SP] - operandSize);
        V86WriteMemory((V86SegmentBase(cpu->Segments[X86_SREG_SS])) + stackPointer, operandSize, V86GetRegister(cpu, opcode & X86_MODRM_REGISTER_MASK, operandSize));
        cpu->Registers[X86_REG_SP] = (cpu->Registers[X86_REG_SP] & HIGH_WORD_MASK_U) | stackPointer; cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
    }
    if (opcode >= X86_OP_POP_FIRST && opcode <= X86_OP_POP_LAST) {
        WORD stackPointer = (WORD)cpu->Registers[X86_REG_SP];
        V86SetRegister(cpu, opcode & X86_MODRM_REGISTER_MASK, operandSize, V86ReadMemory((V86SegmentBase(cpu->Segments[X86_SREG_SS])) + stackPointer, operandSize));
        cpu->Registers[X86_REG_SP] = (cpu->Registers[X86_REG_SP] & HIGH_WORD_MASK_U) | (WORD)(stackPointer + operandSize); cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
    }

    /* ---- PUSH imm (68 = imm16/imm32, 6A = imm8 sign-extended to W): a C runtime *
     * pushes call args and far-jump targets this way (run 55's I310102 stopped on *
     * `68 3a 02` = PUSH 0x023A). Slot + immediate are W-wide (0x66 -> 32-bit).     *
     * run 56. */
    if (opcode == X86_OP_PUSH_IMM || opcode == X86_OP_PUSH_IMM8) {
        WORD stackPointer = (WORD)(cpu->Registers[X86_REG_SP] - operandSize);
        UINT32 value;
        if (opcode == X86_OP_PUSH_IMM) { value = V86_IMMEDIATE(offset, operandSize); offset += operandSize; }     /* imm is W bytes */
        else { value = (UINT32)(INT32)(INT8)V86_CODE_BYTE(offset++); value &= V86Mask(operandSize); }  /* sign-ext imm8 */
        V86WriteMemory((V86SegmentBase(cpu->Segments[X86_SREG_SS])) + stackPointer, operandSize, value);
        cpu->Registers[X86_REG_SP] = (cpu->Registers[X86_REG_SP] & HIGH_WORD_MASK_U) | stackPointer; cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
    }

    /* ---- PUSHF/POPF (9C/9D): the FLAGS stack pair. A C runtime saves/restores  *
     * FLAGS around a code sequence (run 58's I310102 stopped on `9c ...` heading  *
     * a PUSHF; PUSH EDX; ... register-save). We push only the flags we model      *
     * (arithmetic + DF) plus the always-set reserved bit 1; POPF keeps the same   *
     * mask so the round-trip is exact (IF/TF/IOPL/NT are not modeled -> dropped,  *
     * so c->flags never accumulates junk). 16-bit only; the 0x66 PUSHFD/POPFD     *
     * 32-bit-EFLAGS form bails as TODO, matching the neighbouring stack ops.      *
     * run 59. */
    if (opcode == X86_OP_PUSHF) {                                  /* PUSHF */
        WORD stackPointer;
        if (isOperand32) {                                     /* PUSHFD (#194): see V86FlagsImage */
            V86Push(cpu, X86_DWORD_SIZE, V86FlagsImage(cpu, X86_DWORD_SIZE));
            cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
        }
        stackPointer = (WORD)(cpu->Registers[X86_REG_SP] - X86_WORD_SIZE);
        V86WriteMemory((V86SegmentBase(cpu->Segments[X86_SREG_SS])) + stackPointer, X86_WORD_SIZE, (WORD)((cpu->Flags & V86_FLAGS_MODELLED) | EFLAGS_RESERVED_ONE_U));
        cpu->Registers[X86_REG_SP] = (cpu->Registers[X86_REG_SP] & HIGH_WORD_MASK_U) | stackPointer; cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
    }
    if (opcode == X86_OP_POPF) {                                  /* POPF */
        WORD stackPointer;
        if (isOperand32) {                                     /* POPFD (#194): see V86FlagsLoad */
            V86FlagsLoad(cpu, V86Pop(cpu, X86_DWORD_SIZE), X86_DWORD_SIZE);
            cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
        }
        stackPointer = (WORD)cpu->Registers[X86_REG_SP];
        /* IF (0x200) is part of the mask: dropping it made every interpreted POPF
           silently disable the guest's interrupts. */
        cpu->Flags = (V86ReadMemory((V86SegmentBase(cpu->Segments[X86_SREG_SS])) + stackPointer, X86_WORD_SIZE) & V86_FLAGS_MODELLED) | EFLAGS_RESERVED_ONE_U;
        cpu->Registers[X86_REG_SP] = (cpu->Registers[X86_REG_SP] & HIGH_WORD_MASK_U) | (WORD)(stackPointer + X86_WORD_SIZE); cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
    }

    /* ---- PUSHA/POPA (60/61): push/pop the whole GP file. A callee saves the    *
     * register file on entry this way (run 61's I310102 stopped on `66 60` =      *
     * PUSHAD). Push order AX,CX,DX,BX,SP,BP,SI,DI (indices 0..7) with the pushed  *
     * SP being its value BEFORE the push; POPA restores DI..AX and DISCARDS the   *
     * saved-SP slot. Values are W-wide (0x66 -> PUSHAD/POPAD); the stack offset   *
     * stays 16-bit (address size). run 62. */
    if (opcode == X86_OP_PUSHA) {                                  /* PUSHA / PUSHAD */
        WORD stackPointer = (WORD)cpu->Registers[X86_REG_SP];
        UINT32 originalSp = V86GetRegister(cpu, X86_REG_SP, operandSize); int registerIndex;           /* SP/ESP before any push */
        for (registerIndex = 0; registerIndex < X86_GENERAL_REGISTERS; registerIndex++) {
            UINT32 value = (registerIndex == X86_REG_SP) ? originalSp : V86GetRegister(cpu, registerIndex, operandSize);
            stackPointer = (WORD)(stackPointer - operandSize);
            V86WriteMemory((V86SegmentBase(cpu->Segments[X86_SREG_SS])) + stackPointer, operandSize, value);
        }
        cpu->Registers[X86_REG_SP] = (cpu->Registers[X86_REG_SP] & HIGH_WORD_MASK_U) | stackPointer; cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
    }
    if (opcode == X86_OP_POPA) {                                  /* POPA / POPAD */
        WORD stackPointer = (WORD)cpu->Registers[X86_REG_SP]; int registerIndex;
        for (registerIndex = X86_REG_DI; registerIndex >= 0; registerIndex--) {
            if (registerIndex == X86_REG_SP) { stackPointer = (WORD)(stackPointer + operandSize); continue; }   /* discard saved SP */
            V86SetRegister(cpu, registerIndex, operandSize, V86ReadMemory((V86SegmentBase(cpu->Segments[X86_SREG_SS])) + stackPointer, operandSize));
            stackPointer = (WORD)(stackPointer + operandSize);
        }
        cpu->Registers[X86_REG_SP] = (cpu->Registers[X86_REG_SP] & HIGH_WORD_MASK_U) | stackPointer; cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
    }

    /* ---- PUSH/POP segment regs (06/0E/16/1E push ES/CS/SS/DS, 07/17/1F pop  *
     * ES/SS/DS). QB saves/restores ES (and DS) around each pixel -- this was  *
     * the per-pixel bail that capped batching at ~1 pixel. POP CS (0F) is not *
     * modeled (it would change the code segment mid-interpret). ------------- */
    if (opcode == X86_OP_PUSH_ES || opcode == X86_OP_PUSH_CS || opcode == X86_OP_PUSH_SS || opcode == X86_OP_PUSH_DS) {        /* PUSH sreg */
        int segmentRegister = (opcode == X86_OP_PUSH_ES) ? X86_SREG_ES : (opcode == X86_OP_PUSH_CS) ? X86_SREG_CS : (opcode == X86_OP_PUSH_SS) ? X86_SREG_SS : X86_SREG_DS;
        WORD stackPointer;
        if (isOperand32) {                                     /* #194: 4-byte slot */
            V86PushSegment(cpu, X86_DWORD_SIZE, cpu->Segments[segmentRegister]);
            cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
        }
        stackPointer = (WORD)(cpu->Registers[X86_REG_SP] - X86_WORD_SIZE);
        V86WriteMemory((V86SegmentBase(cpu->Segments[X86_SREG_SS])) + stackPointer, X86_WORD_SIZE, cpu->Segments[segmentRegister]);
        cpu->Registers[X86_REG_SP] = (cpu->Registers[X86_REG_SP] & HIGH_WORD_MASK_U) | stackPointer; cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
    }
    if (opcode == X86_OP_POP_ES || opcode == X86_OP_POP_SS || opcode == X86_OP_POP_DS) {                      /* POP sreg */
        int segmentRegister = (opcode == X86_OP_POP_ES) ? X86_SREG_ES : (opcode == X86_OP_POP_SS) ? X86_SREG_SS : X86_SREG_DS;
        WORD stackPointer;
        if (isOperand32) {                                     /* #194: 4-byte slot, low word */
            cpu->Segments[segmentRegister] = (WORD)V86Pop(cpu, X86_DWORD_SIZE);
            cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
        }
        stackPointer = (WORD)cpu->Registers[X86_REG_SP];
        cpu->Segments[segmentRegister] = (WORD)V86ReadMemory((V86SegmentBase(cpu->Segments[X86_SREG_SS])) + stackPointer, X86_WORD_SIZE);
        cpu->Registers[X86_REG_SP] = (cpu->Registers[X86_REG_SP] & HIGH_WORD_MASK_U) | (WORD)(stackPointer + X86_WORD_SIZE); cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
    }

    /* ---- MOV r/m16,Sreg (8C) / MOV Sreg,r/m16 (8E) ------------------------- */
    if (opcode == X86_OP_MOV_FROM_SREG || opcode == X86_OP_MOV_TO_SREG) {
        V86_MODRM modrm; offset += V86DecodeModrm(cpu, codeLinear, codePointer, offset, segmentOverride, &modrm);
        if (modrm.IsMemory && modrm.Linear >= V86_GUEST_LIMIT) return 0;
        if ((modrm.Register & X86_MODRM_REGISTER_MASK) > X86_SREG_GS) return 0;
        if (opcode == X86_OP_MOV_FROM_SREG) {                              /* store Sreg -> r/m16 */
            WORD value = cpu->Segments[modrm.Register & X86_MODRM_REGISTER_MASK];
            /* #194: `66 8C` to a REGISTER zero-extends into all 32 bits (measured, p_o32
               item 4); to memory it is a word store either way. */
            if (modrm.IsMemory) V86WriteMemory(modrm.Linear, X86_WORD_SIZE, value);
            else if (isOperand32) cpu->Registers[modrm.RegisterMemory & X86_MODRM_REGISTER_MASK] = value;
            else V86Set16(cpu, modrm.RegisterMemory, value);
        } else {                                       /* load Sreg <- r/m16 */
            if ((modrm.Register & X86_MODRM_REGISTER_MASK) == X86_SREG_CS) return 0;              /* MOV CS,x is illegal */
            cpu->Segments[modrm.Register & X86_MODRM_REGISTER_MASK] = modrm.IsMemory ? (WORD)V86ReadMemory(modrm.Linear, X86_WORD_SIZE) : V86Get16(cpu, modrm.RegisterMemory);
        }
        cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
    }

    /* ---- LEA r16/r32, m (8D): load the effective-address offset (not memory) */
    if (opcode == X86_OP_LEA) {
        V86_MODRM modrm; offset += V86DecodeModrm(cpu, codeLinear, codePointer, offset, segmentOverride, &modrm);
        if (!modrm.IsMemory) return 0;                       /* LEA with reg operand is illegal */
        V86SetRegister(cpu, modrm.Register, operandSize, modrm.EffectiveAddress);                          /* addr size is 16-bit -> EffectiveAddress zero-ext to W */
        cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
    }

    /* ---- shift/rotate group-2: D0/D1 (by 1), D2/D3 (by CL), C0/C1 (imm8) --- */
    if (opcode == X86_OP_SHIFT_ONE_BYTE || opcode == X86_OP_SHIFT_ONE || opcode == X86_OP_SHIFT_CL_BYTE || opcode == X86_OP_SHIFT_CL || opcode == X86_OP_SHIFT_IMM_BYTE || opcode == X86_OP_SHIFT_IMM) {
        int width = (opcode & 1) ? operandSize : 1, count;
        V86_MODRM modrm; offset += V86DecodeModrm(cpu, codeLinear, codePointer, offset, segmentOverride, &modrm);
        if (modrm.IsMemory && modrm.Linear >= V86_GUEST_LIMIT) return 0;
        if (opcode == X86_OP_SHIFT_ONE_BYTE || opcode == X86_OP_SHIFT_ONE) count = 1;
        else if (opcode == X86_OP_SHIFT_CL_BYTE || opcode == X86_OP_SHIFT_CL) count = V86Get8(cpu, X86_REG_CX);   /* CL */
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
    if (opcode == X86_OP_IN_IMM_BYTE || opcode == X86_OP_IN_IMM || opcode == X86_OP_IN_DX_BYTE || opcode == X86_OP_IN_DX) {          /* IN  */
        int width = (opcode & 1) ? operandSize : 1;
        WORD port = (opcode <= X86_OP_IN_IMM) ? (WORD)V86_CODE_BYTE(offset++) : cpu->Registers[X86_REG_DX];    /* imm8/DX */
        UINT32 value = V86HostIn(port, width);
        V86SetRegister(cpu, X86_REG_AX, width, value & V86Mask(width));
        cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
    }
    if (opcode == X86_OP_OUT_IMM_BYTE || opcode == X86_OP_OUT_IMM || opcode == X86_OP_OUT_DX_BYTE || opcode == X86_OP_OUT_DX) {          /* OUT */
        int width = (opcode & 1) ? operandSize : 1;
        WORD port = (opcode <= X86_OP_OUT_IMM) ? (WORD)V86_CODE_BYTE(offset++) : cpu->Registers[X86_REG_DX];    /* imm8/DX */
        V86HostOut(port, width, V86GetRegister(cpu, X86_REG_AX, width));
        cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
    }

    /* ---- MOV r,imm (B0-BF); MOV AL/AX,moffs / moffs,AL/AX (A0-A3) --------- */
    if (opcode >= X86_OP_MOV_IMM_BYTE_FIRST && opcode <= X86_OP_MOV_IMM_BYTE_LAST) { V86Set8(cpu, opcode & X86_MODRM_REGISTER_MASK, V86_CODE_BYTE(offset)); offset++;
                                    cpu->Ip = (WORD)(cpu->Ip + offset); return 1; }
    if (opcode >= X86_OP_MOV_IMM_FIRST && opcode <= X86_OP_MOV_IMM_LAST) { UINT32 value = V86_IMMEDIATE(offset, operandSize); offset += operandSize;
                                    V86SetRegister(cpu, opcode & X86_MODRM_REGISTER_MASK, operandSize, value); cpu->Ip = (WORD)(cpu->Ip + offset); return 1; }
    if (opcode >= X86_OP_MOV_FROM_MOFFS_BYTE && opcode <= X86_OP_MOV_TO_MOFFS) {
        WORD address = (WORD)(V86_CODE_BYTE(offset) | (V86_CODE_BYTE(offset + 1) << BYTE_SHIFT)); offset += X86_WORD_SIZE;
        UINT32 linear = (V86SegmentBase(cpu->Segments[(segmentOverride >= 0) ? segmentOverride : X86_SREG_DS])) + address;
        int width = (opcode & 1) ? operandSize : 1;
        if (linear >= V86_GUEST_LIMIT) return 0;
        if (opcode <= X86_OP_MOV_FROM_MOFFS) V86SetRegister(cpu, X86_REG_AX, width, V86ReadMemory(linear, width));
        else            V86WriteMemory(linear, width, V86GetRegister(cpu, X86_REG_AX, width));
        cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
    }

    /* ---- string ops: STOS (AA/AB), MOVS (A4/A5), LODS (AC/AD) -------------- */
    /* #194: the dword forms (66 AB/A5/A7/AF/AD) are the same loops with w = 4. Each element
       is still moved a byte at a time, in the order the word forms always used (low
       byte first; destination before source for CMPS), each byte's offset wrapping at
       64 KB -- so an A0000 latch read happens exactly where it did. */
    if (opcode == X86_OP_STOSB || opcode == X86_OP_STOS) {                   /* STOS ES:DI <- AL/AX/EAX */
        int width = (opcode == X86_OP_STOS) ? operandSize : 1, direction = (cpu->Flags & EFLAGS_DF_U) ? -width : width, index;
        UINT32 count = repeat ? (UINT32)(WORD)cpu->Registers[X86_REG_CX] : 1, extraSegment = cpu->Segments[X86_SREG_ES], accumulator = cpu->Registers[X86_REG_AX]; WORD destinationIndex = cpu->Registers[X86_REG_DI];
        while (count) { for (index = 0; index < width; ++index)
                          V86HostWrite8((V86SegmentBase(extraSegment)) + (WORD)(destinationIndex + index), (BYTE)(accumulator >> (BYTE_SHIFT * index)));
                      destinationIndex = (WORD)(destinationIndex + direction); count--; }
        V86Set16(cpu, X86_REG_DI, destinationIndex); if (repeat) V86Set16(cpu, X86_REG_CX, (WORD)count);
        cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
    }
    if (opcode == X86_OP_MOVSB || opcode == X86_OP_MOVS) {                   /* MOVS ES:DI <- DS:SI */
        int width = (opcode == X86_OP_MOVS) ? operandSize : 1, direction = (cpu->Flags & EFLAGS_DF_U) ? -width : width, index;
        UINT32 count = repeat ? (UINT32)(WORD)cpu->Registers[X86_REG_CX] : 1, sourceSegment = cpu->Segments[(segmentOverride >= 0) ? segmentOverride : X86_SREG_DS], extraSegment = cpu->Segments[X86_SREG_ES];
        WORD sourceIndex = cpu->Registers[X86_REG_SI], destinationIndex = cpu->Registers[X86_REG_DI];
        while (count) { for (index = 0; index < width; ++index)
                          V86HostWrite8((V86SegmentBase(extraSegment)) + (WORD)(destinationIndex + index),
                                  V86HostRead8((V86SegmentBase(sourceSegment)) + (WORD)(sourceIndex + index)));
                      sourceIndex = (WORD)(sourceIndex + direction); destinationIndex = (WORD)(destinationIndex + direction); count--; }
        V86Set16(cpu, X86_REG_SI, sourceIndex); V86Set16(cpu, X86_REG_DI, destinationIndex); if (repeat) V86Set16(cpu, X86_REG_CX, (WORD)count);
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
    if (opcode == X86_OP_CMPSB || opcode == X86_OP_CMPS || opcode == X86_OP_SCASB || opcode == X86_OP_SCAS) {
        int width = (opcode & 1) ? operandSize : 1, direction = (cpu->Flags & EFLAGS_DF_U) ? -width : width, index;
        int isScas = (opcode >= X86_OP_SCASB);
        UINT32 count = repeat ? (UINT32)(WORD)cpu->Registers[X86_REG_CX] : 1, extraSegment = cpu->Segments[X86_SREG_ES];
        UINT32 sourceSegment = cpu->Segments[(segmentOverride >= 0) ? segmentOverride : X86_SREG_DS];
        WORD sourceIndex = cpu->Registers[X86_REG_SI], destinationIndex = cpu->Registers[X86_REG_DI];
        while (count) {
            UINT32 first = 0, second = 0;
            for (index = 0; index < width; ++index)
                second |= (UINT32)V86HostRead8((V86SegmentBase(extraSegment)) + (WORD)(destinationIndex + index)) << (BYTE_SHIFT * index);
            if (isScas) first = V86GetRegister(cpu, X86_REG_AX, width);                              /* AL/AX/EAX */
            else {
                for (index = 0; index < width; ++index)
                    first |= (UINT32)V86HostRead8((V86SegmentBase(sourceSegment)) + (WORD)(sourceIndex + index)) << (BYTE_SHIFT * index);
                sourceIndex = (WORD)(sourceIndex + direction);
            }
            V86Subtract(cpu, first, second, 0, width);                                   /* CMP a,b */
            destinationIndex = (WORD)(destinationIndex + direction); count--;
            if (repeat == 1 && !(cpu->Flags & EFLAGS_ZF_U)) break;               /* REPE  */
            if (repeat == X86_REPEAT_REPNE &&  (cpu->Flags & EFLAGS_ZF_U)) break;               /* REPNE */
        }
        if (!isScas) V86Set16(cpu, X86_REG_SI, sourceIndex);
        V86Set16(cpu, X86_REG_DI, destinationIndex); if (repeat) V86Set16(cpu, X86_REG_CX, (WORD)count);
        cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
    }
    if (opcode == X86_OP_LODSB || opcode == X86_OP_LODS) {                   /* LODS AL/AX/EAX <- DS:SI */
        int width = (opcode == X86_OP_LODS) ? operandSize : 1, direction = (cpu->Flags & EFLAGS_DF_U) ? -width : width, index;
        UINT32 count = repeat ? (UINT32)(WORD)cpu->Registers[X86_REG_CX] : 1, sourceSegment = cpu->Segments[(segmentOverride >= 0) ? segmentOverride : X86_SREG_DS]; WORD sourceIndex = cpu->Registers[X86_REG_SI];
        while (count) { UINT32 value = 0;
                      for (index = 0; index < width; ++index)
                          value |= (UINT32)V86HostRead8((V86SegmentBase(sourceSegment)) + (WORD)(sourceIndex + index)) << (BYTE_SHIFT * index);
                      V86SetRegister(cpu, X86_REG_AX, width, value);
                      sourceIndex = (WORD)(sourceIndex + direction); count--; }
        V86Set16(cpu, X86_REG_SI, sourceIndex); if (repeat) V86Set16(cpu, X86_REG_CX, (WORD)count);
        cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
    }

    /* ---- control flow: Jcc (70-7F), JMP short (EB) / near (E9),
            CALL near (E8) + RET near (C3/C2), LOOP/LOOPE/LOOPNE/JCXZ (E0-E3) -- *
     * CALL/RET let the interpreter follow QuickBasic's per-pixel runtime call,  *
     * so a whole scanline batches in one fault instead of ~5 instr per pixel.   */
    if (opcode >= X86_OP_JCC_SHORT_FIRST && opcode <= X86_OP_JCC_SHORT_LAST) {
        INT8 relative = (INT8)V86_CODE_BYTE(offset++); int isTaken = V86Condition(cpu, opcode & X86_CONDITION_MASK);
        cpu->Ip = (WORD)(cpu->Ip + offset + (isTaken ? relative : 0)); return 1;
    }
    if (opcode == X86_OP_JMP_SHORT) { INT8 relative = (INT8)V86_CODE_BYTE(offset++); cpu->Ip = (WORD)(cpu->Ip + offset + relative); return 1; }
    /* #194: JMP/CALL rel32 (66 E9/E8): EIP = next + rel32, a 4-byte return slot; bail if
       the target leaves the 64 KB segment (the CPU's #GP, not ours to fake). */
    if ((opcode == X86_OP_JMP || opcode == X86_OP_CALL) && isOperand32) {
        UINT32 nextIp = (UINT32)(WORD)(cpu->Ip + offset + X86_DWORD_SIZE);
        UINT32 target = nextIp + V86ReadMemory(codeLinear + offset, X86_DWORD_SIZE);
        if (target > V86_OFFSET_MAX) return 0;
        if (opcode == X86_OP_CALL) V86Push(cpu, X86_DWORD_SIZE, nextIp);
        cpu->Ip = (WORD)target; return 1;
    }
    if (opcode == X86_OP_JMP) { INT16 relative; relative = (INT16)(V86_CODE_BYTE(offset) | (V86_CODE_BYTE(offset + 1) << BYTE_SHIFT)); offset += X86_WORD_SIZE;
                      cpu->Ip = (WORD)(cpu->Ip + offset + relative); return 1; }
    if (opcode == X86_OP_CALL) {                                  /* CALL near relative */
        INT16 relative; WORD nextIp, stackPointer;
        relative = (INT16)(V86_CODE_BYTE(offset) | (V86_CODE_BYTE(offset + 1) << BYTE_SHIFT)); offset += X86_WORD_SIZE;
        nextIp = (WORD)(cpu->Ip + offset);                 /* return address */
        stackPointer = (WORD)(cpu->Registers[X86_REG_SP] - X86_WORD_SIZE);
        V86WriteMemory((V86SegmentBase(cpu->Segments[X86_SREG_SS])) + stackPointer, X86_WORD_SIZE, nextIp);
        cpu->Registers[X86_REG_SP] = (cpu->Registers[X86_REG_SP] & HIGH_WORD_MASK_U) | stackPointer; cpu->Ip = (WORD)(nextIp + relative); return 1;
    }
    if (opcode == X86_OP_RET || opcode == X86_OP_RET_IMM) {                    /* RET near (+ imm16 pop) */
        WORD stackPointer, returnIp, extraPop;
        if (isOperand32) {                                     /* #194: a 4-byte EIP slot */
            UINT32 operand;
            stackPointer = (WORD)cpu->Registers[X86_REG_SP];
            operand = V86ReadMemory(V86SegmentBase(cpu->Segments[X86_SREG_SS]) + stackPointer, X86_DWORD_SIZE);
            if (operand > V86_OFFSET_MAX) return 0;
            extraPop = (opcode == X86_OP_RET_IMM) ? (WORD)(V86_CODE_BYTE(offset) | (V86_CODE_BYTE(offset + 1) << BYTE_SHIFT)) : 0;
            cpu->Registers[X86_REG_SP] = (cpu->Registers[X86_REG_SP] & HIGH_WORD_MASK_U) | (WORD)(stackPointer + X86_DWORD_SIZE + extraPop);
            cpu->Ip = (WORD)operand; return 1;
        }
        stackPointer = (WORD)cpu->Registers[X86_REG_SP];
        returnIp = (WORD)V86ReadMemory((V86SegmentBase(cpu->Segments[X86_SREG_SS])) + stackPointer, X86_WORD_SIZE);
        extraPop = (opcode == X86_OP_RET_IMM) ? (WORD)(V86_CODE_BYTE(offset) | (V86_CODE_BYTE(offset + 1) << BYTE_SHIFT)) : 0;
        cpu->Registers[X86_REG_SP] = (cpu->Registers[X86_REG_SP] & HIGH_WORD_MASK_U) | (WORD)(stackPointer + X86_WORD_SIZE + extraPop); cpu->Ip = returnIp; return 1;
    }

    /* ---- RETF (CB) / RETF imm16 (CA): far return -- pop offset then a 2-byte  *
     * SELECTOR into CS. In PM `V86SegmentBase` resolves that selector via the LDT (the *
     * same machinery LAR/LSL use), so the client's `PUSH seg; PUSH off; RETF`    *
     * far-transfer idiom (run 56's wall) just follows through. run 57. */
    if (opcode == X86_OP_RETF || opcode == X86_OP_RETF_IMM) {
        WORD stackPointer, returnOffset, selector, extraPop;
        if (isOperand32) {                                     /* #194: EIP dword, then a CS dword */
            UINT32 operand; UINT32 stackBase = V86SegmentBase(cpu->Segments[X86_SREG_SS]);
            stackPointer  = (WORD)cpu->Registers[X86_REG_SP];
            operand   = V86ReadMemory(stackBase + stackPointer, X86_DWORD_SIZE);
            selector = (WORD)V86ReadMemory(stackBase + (WORD)(stackPointer + X86_DWORD_SIZE), X86_WORD_SIZE);
            if (g_V86SegmentToLinear ? !V86IsFarTargetValid(cpu, selector, operand) : (operand > V86_OFFSET_MAX)) return 0;
            extraPop = (opcode == X86_OP_RETF_IMM) ? (WORD)(V86_CODE_BYTE(offset) | (V86_CODE_BYTE(offset + 1) << BYTE_SHIFT)) : 0;
            cpu->Registers[X86_REG_SP] = (cpu->Registers[X86_REG_SP] & HIGH_WORD_MASK_U) | (WORD)(stackPointer + V86_FAR_FRAME32 + extraPop);
            cpu->Segments[X86_SREG_CS] = selector; cpu->Ip = (WORD)operand; return 1;
        }
        stackPointer  = (WORD)cpu->Registers[X86_REG_SP];
        returnOffset = (WORD)V86ReadMemory((V86SegmentBase(cpu->Segments[X86_SREG_SS])) + stackPointer, X86_WORD_SIZE);
        selector = (WORD)V86ReadMemory((V86SegmentBase(cpu->Segments[X86_SREG_SS])) + (WORD)(stackPointer + X86_WORD_SIZE), X86_WORD_SIZE);
        extraPop = (opcode == X86_OP_RETF_IMM) ? (WORD)(V86_CODE_BYTE(offset) | (V86_CODE_BYTE(offset + 1) << BYTE_SHIFT)) : 0;
        cpu->Registers[X86_REG_SP] = (cpu->Registers[X86_REG_SP] & HIGH_WORD_MASK_U) | (WORD)(stackPointer + V86_FAR_FRAME16 + extraPop);
        cpu->Segments[X86_SREG_CS] = selector; cpu->Ip = returnOffset; return 1;
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
    if (opcode == X86_OP_ENTER && !isOperand32) {
        WORD frameSize = (WORD)(V86_CODE_BYTE(offset) | (V86_CODE_BYTE(offset + 1) << BYTE_SHIFT));
        WORD stackPointer;
        if (V86_CODE_BYTE(offset + X86_WORD_SIZE) & V86_ENTER_LEVEL_MASK) return 0;
        offset += V86_ENTER_OPERAND_LENGTH;
        stackPointer = (WORD)(cpu->Registers[X86_REG_SP] - X86_WORD_SIZE);
        V86WriteMemory(V86SegmentBase(cpu->Segments[X86_SREG_SS]) + stackPointer, X86_WORD_SIZE, (WORD)cpu->Registers[X86_REG_BP]);
        cpu->Registers[X86_REG_BP] = (cpu->Registers[X86_REG_BP] & HIGH_WORD_MASK_U) | stackPointer;        /* BP <- SP */
        cpu->Registers[X86_REG_SP] = (cpu->Registers[X86_REG_SP] & HIGH_WORD_MASK_U) | (WORD)(stackPointer - frameSize);
        cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
    }
    if (opcode == X86_OP_LEAVE) {
        WORD stackPointer, framePointer;
        if (isOperand32) {                                     /* #194: SP <- BP (16-bit stack), EBP <- pop32 */
            stackPointer = (WORD)cpu->Registers[X86_REG_BP];
            cpu->Registers[X86_REG_BP] = V86ReadMemory(V86SegmentBase(cpu->Segments[X86_SREG_SS]) + stackPointer, X86_DWORD_SIZE);
            cpu->Registers[X86_REG_SP] = (cpu->Registers[X86_REG_SP] & HIGH_WORD_MASK_U) | (WORD)(stackPointer + X86_DWORD_SIZE);
            cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
        }
        stackPointer = (WORD)cpu->Registers[X86_REG_BP];                        /* SP <- BP */
        framePointer = (WORD)V86ReadMemory((V86SegmentBase(cpu->Segments[X86_SREG_SS])) + stackPointer, X86_WORD_SIZE);
        cpu->Registers[X86_REG_BP] = (cpu->Registers[X86_REG_BP] & HIGH_WORD_MASK_U) | framePointer;        /* BP <- pop */
        cpu->Registers[X86_REG_SP] = (cpu->Registers[X86_REG_SP] & HIGH_WORD_MASK_U) | (WORD)(stackPointer + X86_WORD_SIZE);
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
    if (opcode == X86_OP_POP_RM) {
        V86_MODRM modrm; WORD stackPointer; UINT32 value;
        if (g_V86SegmentToLinear) return 0;
        offset += V86DecodeModrm(cpu, codeLinear, codePointer, offset, segmentOverride, &modrm);
        if (modrm.Register != 0 || (modrm.IsMemory && modrm.Linear + operandSize > V86_GUEST_LIMIT)) return 0;
        stackPointer = (WORD)cpu->Registers[X86_REG_SP];
        value = V86ReadMemory((V86SegmentBase(cpu->Segments[X86_SREG_SS])) + stackPointer, operandSize);    /* #194: W = 4 under 0x66 */
        cpu->Registers[X86_REG_SP] = (cpu->Registers[X86_REG_SP] & HIGH_WORD_MASK_U) | (WORD)(stackPointer + operandSize);   /* SP moves FIRST:
                                                    `pop [sp-relative]` sees the new SP */
        if (modrm.IsMemory) V86WriteMemory(modrm.Linear, operandSize, value); else V86SetRegister(cpu, modrm.RegisterMemory, operandSize, value);
        cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
    }
    if (opcode == X86_OP_WAIT) { cpu->Ip = (WORD)(cpu->Ip + offset); return 1; }   /* WAIT: no FPU here */
    /* LAHF (9F) / SAHF (9E): AH <-> SF ZF AF PF CF (bit 1 reads as 1). Bubbles: 16,586. */
    if (opcode == X86_OP_LAHF) { V86Set8(cpu, X86_REG_SP, (BYTE)((cpu->Flags & V86_LAHF_FLAGS) | EFLAGS_RESERVED_ONE_U)); cpu->Ip = (WORD)(cpu->Ip + offset); return 1; }
    if (opcode == X86_OP_SAHF) { cpu->Flags = (cpu->Flags & ~V86_LAHF_FLAGS) | (V86Get8(cpu, X86_REG_SP) & V86_LAHF_FLAGS); cpu->Ip = (WORD)(cpu->Ip + offset); return 1; }
    if (opcode == X86_OP_CBW) {                                  /* CBW: AX <- sign(AL); 66: CWDE */
        if (isOperand32) cpu->Registers[X86_REG_AX] = (UINT32)(INT32)(INT16)(cpu->Registers[X86_REG_AX] & WORD_MASK);
        else     V86Set16(cpu, X86_REG_AX, (WORD)(INT16)(INT8)(cpu->Registers[X86_REG_AX] & BYTE_MASK));
        cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
    }
    if (opcode == X86_OP_CWD) {                                  /* CWD: DX <- sign(AX); 66: CDQ */
        /* CDQ is s80's: Wolf3D's FixedByFrac (`cdq / idiv dword`) declined it 1,074
           times under a multi-plane mask, handing its renderer to the real CPU. */
        if (isOperand32) cpu->Registers[X86_REG_DX] = (cpu->Registers[X86_REG_AX] & X86_DWORD_SIGN_U) ? DWORD_MASK_U : 0u;
        else     V86Set16(cpu, X86_REG_DX, (cpu->Registers[X86_REG_AX] & X86_WORD_SIGN_U) ? WORD_MASK_U : 0u);
        cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
    }
    /* ---- IMUL reg, r/m, imm (69: imm16/32, 6B: sign-extended imm8). s80: Mario's
     * `6b f8 0a` = imul di,ax,10 was 7,978 of its declines under a multi-plane mask.
     * Destination is the reg field; CF=OF=1 when the signed product does not fit the
     * destination width. SF/ZF/AF/PF are undefined by the spec and left alone. */
    if (opcode == X86_OP_IMUL_IMM || opcode == X86_OP_IMUL_IMM8) {
        int width = operandSize, isOverflow;
        INT64 first, second, product;
        UINT32 result;
        V86_MODRM modrm; offset += V86DecodeModrm(cpu, codeLinear, codePointer, offset, segmentOverride, &modrm);
        if (modrm.IsMemory && modrm.Linear >= V86_GUEST_LIMIT) return 0;
        { UINT32 operand = modrm.IsMemory ? V86ReadMemory(modrm.Linear, width) : V86GetRegister(cpu, modrm.RegisterMemory, width);
          first = (width == X86_DWORD_SIZE) ? (INT64)(INT32)operand : (INT64)(INT16)operand; }
        if (opcode == X86_OP_IMUL_IMM8) { second = (INT8)V86_IMMEDIATE(offset, 1); offset += 1; }
        else { UINT32 immediate = V86_IMMEDIATE(offset, width); offset += width;
               second = (width == X86_DWORD_SIZE) ? (INT64)(INT32)immediate : (INT64)(INT16)immediate; }
        product = first * second;
        result = (UINT32)product & V86Mask(width);
        isOverflow = (width == X86_DWORD_SIZE) ? (product != (INT64)(INT32)result) : (product != (INT64)(INT16)result);
        V86SetRegister(cpu, modrm.Register, width, result);
        cpu->Flags = (cpu->Flags & ~(EFLAGS_CF_U | EFLAGS_OF_U)) | (isOverflow ? (EFLAGS_CF_U | EFLAGS_OF_U) : 0);
        cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
    }
    if (opcode == X86_OP_XLAT) {                                  /* XLAT: AL <- [DS:BX+AL] */
        UINT32 sourceSegment = cpu->Segments[(segmentOverride >= 0) ? segmentOverride : X86_SREG_DS];
        UINT32 linear = (V86SegmentBase(sourceSegment)) + (WORD)((cpu->Registers[X86_REG_BX] & WORD_MASK) + (cpu->Registers[X86_REG_AX] & BYTE_MASK));
        if (linear >= V86_GUEST_LIMIT) return 0;
        V86Set8(cpu, X86_REG_AX, V86HostRead8(linear));
        cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
    }
    if ((opcode == X86_OP_LES || opcode == X86_OP_LDS) && V86_CODE_BYTE(offset) != VDM_BOP1 && (V86_CODE_BYTE(offset) >> X86_MODRM_MODE_SHIFT) != X86_MODE_REGISTER) {
        V86_MODRM modrm;
        if (g_V86SegmentToLinear) return 0;                       /* PM: TODO */
        offset += V86DecodeModrm(cpu, codeLinear, codePointer, offset, segmentOverride, &modrm);
        if (!modrm.IsMemory || modrm.Linear + operandSize + X86_WORD_SIZE > V86_GUEST_LIMIT) return 0;
        /* #194: under 0x66 the pointer is m16:32 -- a dword offset, then the segment. */
        V86SetRegister(cpu, modrm.Register, operandSize, V86ReadMemory(modrm.Linear, operandSize));
        cpu->Segments[opcode == X86_OP_LES ? X86_SREG_ES : X86_SREG_DS] = (WORD)V86ReadMemory(modrm.Linear + operandSize, X86_WORD_SIZE);
        cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
    }
    if (opcode >= X86_OP_LOOPNE && opcode <= X86_OP_JCXZ) {
        INT8 relative = (INT8)V86_CODE_BYTE(offset++); int isTaken;
        /* #183: CX, not ECX -- 16-bit address size counts in CX and leaves the high
           half alone, and since s80 carried all 32 bits in, the high half is real. */
        if (opcode == X86_OP_JCXZ) isTaken = ((WORD)cpu->Registers[X86_REG_CX] == 0);        /* JCXZ */
        else { V86Set16(cpu, X86_REG_CX, (WORD)(cpu->Registers[X86_REG_CX] - 1));
               int isCountNonZero = ((WORD)cpu->Registers[X86_REG_CX] != 0);
               isTaken = (opcode == X86_OP_LOOP) ? isCountNonZero                                   /* LOOP   */
                    : (opcode == X86_OP_LOOPE) ? (isCountNonZero && (cpu->Flags & EFLAGS_ZF_U))            /* LOOPE  */
                                   : (isCountNonZero && !(cpu->Flags & EFLAGS_ZF_U)); }        /* LOOPNE */
        cpu->Ip = (WORD)(cpu->Ip + offset + (isTaken ? relative : 0)); return 1;
    }

    /* ---- XCHG AX,r16 (91-97): the accumulator short-form -- swap AX with the  *
     * indexed reg (0x90 = XCHG AX,AX = NOP, handled just below). No flags. A C   *
     * runtime uses it as cheap register glue (run 59's I310102 stopped on `96` = *
     * XCHG AX,SI). W-wide (0x66 -> XCHG EAX,r32). run 60. */
    if (opcode >= X86_OP_XCHG_FIRST && opcode <= X86_OP_XCHG_LAST) {
        int registerIndex = opcode & X86_MODRM_REGISTER_MASK;
        UINT32 first = V86GetRegister(cpu, X86_REG_AX, operandSize), second = V86GetRegister(cpu, registerIndex, operandSize);
        V86SetRegister(cpu, X86_REG_AX, operandSize, second); V86SetRegister(cpu, registerIndex, operandSize, first);
        cpu->Ip = (WORD)(cpu->Ip + offset); return 1;
    }

    /* ---- flag ops + NOP --------------------------------------------------- */
    if (opcode == X86_OP_NOP) { cpu->Ip = (WORD)(cpu->Ip + offset); return 1; }              /* NOP */
    if (opcode == X86_OP_CLC) { cpu->Flags &= ~EFLAGS_CF_U; cpu->Ip = (WORD)(cpu->Ip + offset); return 1; }  /* CLC */
    if (opcode == X86_OP_STC) { cpu->Flags |=  EFLAGS_CF_U; cpu->Ip = (WORD)(cpu->Ip + offset); return 1; }  /* STC */
    if (opcode == X86_OP_CMC) { cpu->Flags ^=  EFLAGS_CF_U; cpu->Ip = (WORD)(cpu->Ip + offset); return 1; }  /* CMC */
    if (opcode == X86_OP_CLD) { cpu->Flags &= ~EFLAGS_DF_U; cpu->Ip = (WORD)(cpu->Ip + offset); return 1; }  /* CLD */
    if (opcode == X86_OP_STD) { cpu->Flags |=  EFLAGS_DF_U; cpu->Ip = (WORD)(cpu->Ip + offset); return 1; }  /* STD */

    /* ---- control transfer through the IVT: int nn (CD) / INT3 (CC) / IRET (CF) *
     * Needed by CONTINUOUS interpretation (mode 12h, GH #55). Without them the    *
     * interpreter stopped at the first DOS/BIOS call and handed the guest back to *
     * the real CPU -- where its A0000 writes bypass the planar engine and the      *
     * picture is lost until the next port trap. With them the guest keeps running  *
     * here across its own interrupt handlers.                                      *
     * LES/LDS (C4/C5) are deliberately still unmodeled: the VDM BOP is `C4 C4 nn`, *
     * so bailing on C4 is exactly how a DOS/BIOS call reaches the kernel as a BOP  *
     * event. Modeling LES would swallow every service call the host provides.      */
    if (opcode == X86_OP_INT || opcode == X86_OP_INT3) {
        UINT32 stackBase; WORD stackPointer, nextIp, vector;
        if (g_V86SegmentToLinear) return 0;                       /* PM: no real-mode IVT -> bail   */
        vector = (opcode == X86_OP_INT3) ? V86_INT3_VECTOR : V86_CODE_BYTE(offset++);
        nextIp = (WORD)(cpu->Ip + offset);                 /* return address = past the int  */
        stackBase = V86SegmentBase(cpu->Segments[X86_SREG_SS]);
        stackPointer = (WORD)(cpu->Registers[X86_REG_SP] - X86_WORD_SIZE); V86WriteMemory(stackBase + stackPointer, X86_WORD_SIZE, (cpu->Flags & V86_FLAGS_MODELLED) | EFLAGS_RESERVED_ONE_U);
        stackPointer = (WORD)(stackPointer - X86_WORD_SIZE);      V86WriteMemory(stackBase + stackPointer, X86_WORD_SIZE, cpu->Segments[X86_SREG_CS]);
        stackPointer = (WORD)(stackPointer - X86_WORD_SIZE);      V86WriteMemory(stackBase + stackPointer, X86_WORD_SIZE, nextIp);
        cpu->Registers[X86_REG_SP]   = (cpu->Registers[X86_REG_SP] & HIGH_WORD_MASK_U) | stackPointer;
        cpu->Flags &= ~(EFLAGS_IF_U | EFLAGS_TF_U);                          /* IF/TF cleared on entry         */
        cpu->Ip     = (WORD)V86ReadMemory((UINT32)vector * IVT_ENTRY_SIZE, X86_WORD_SIZE);
        cpu->Segments[X86_SREG_CS] = (WORD)V86ReadMemory((UINT32)vector * IVT_ENTRY_SIZE + X86_WORD_SIZE, X86_WORD_SIZE);
        return 1;
    }
    if (opcode == X86_OP_IRET) {                                  /* IRET */
        UINT32 stackBase; WORD stackPointer;
        if (isOperand32 && !g_V86SegmentToLinear) {
            /* #194: IRETD in real/V86 mode -- EIP, CS and EFLAGS as dwords, 12 bytes. The
               EIP must fit the 64 KB segment (bail otherwise); EFLAGS loads as POPFD does.
               PM IRETD still bails: it may return to a 32-bit segment or another ring. */
            UINT32 operand, newFlags; WORD codeSegment;
            stackPointer  = (WORD)cpu->Registers[X86_REG_SP]; stackBase = V86SegmentBase(cpu->Segments[X86_SREG_SS]);
            operand   = V86ReadMemory(stackBase + stackPointer, X86_DWORD_SIZE);
            codeSegment  = (WORD)V86ReadMemory(stackBase + (WORD)(stackPointer + X86_DWORD_SIZE), X86_WORD_SIZE);
            newFlags  = V86ReadMemory(stackBase + (WORD)(stackPointer + V86_FAR_FRAME32), X86_DWORD_SIZE);
            if (operand > V86_OFFSET_MAX) return 0;
            cpu->Ip = (WORD)operand; cpu->Segments[X86_SREG_CS] = codeSegment;
            V86FlagsLoad(cpu, newFlags, X86_DWORD_SIZE);
            cpu->Registers[X86_REG_SP] = (cpu->Registers[X86_REG_SP] & HIGH_WORD_MASK_U) | (WORD)(stackPointer + V86_IRET_FRAME32);
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
            stackPointer  = (WORD)cpu->Registers[X86_REG_SP]; stackBase = V86SegmentBase(cpu->Segments[X86_SREG_SS]);
            nextIp = (WORD)V86ReadMemory(stackBase + stackPointer, X86_WORD_SIZE);
            newCodeSegment = (WORD)V86ReadMemory(stackBase + (WORD)(stackPointer + X86_WORD_SIZE), X86_WORD_SIZE);
            newFlags  = V86ReadMemory(stackBase + (WORD)(stackPointer + V86_FAR_FRAME16), X86_WORD_SIZE);
            if (!V86IsFarTargetValid(cpu, newCodeSegment, nextIp)) return 0;
            cpu->Ip = nextIp; cpu->Segments[X86_SREG_CS] = newCodeSegment;
            V86FlagsLoad(cpu, newFlags, X86_WORD_SIZE);
            cpu->Registers[X86_REG_SP] = (cpu->Registers[X86_REG_SP] & HIGH_WORD_MASK_U) | (WORD)(stackPointer + V86_IRET_FRAME16);
            return 1;
        }
        stackPointer  = (WORD)cpu->Registers[X86_REG_SP]; stackBase = V86SegmentBase(cpu->Segments[X86_SREG_SS]);
        cpu->Ip     = (WORD)V86ReadMemory(stackBase + stackPointer, X86_WORD_SIZE);
        cpu->Segments[X86_SREG_CS] = (WORD)V86ReadMemory(stackBase + (WORD)(stackPointer + X86_WORD_SIZE), X86_WORD_SIZE);
        cpu->Flags  = (V86ReadMemory(stackBase + (WORD)(stackPointer + V86_FAR_FRAME16), X86_WORD_SIZE) & V86_FLAGS_MODELLED) | EFLAGS_RESERVED_ONE_U;
        cpu->Registers[X86_REG_SP]   = (cpu->Registers[X86_REG_SP] & HIGH_WORD_MASK_U) | (WORD)(stackPointer + V86_IRET_FRAME16);
        return 1;
    }
    /* ---- far JMP (EA) / far CALL (9A) to a real-mode seg:off ---------------- */
    if ((opcode == X86_OP_JMP_FAR || opcode == X86_OP_CALL_FAR) && isOperand32 && !g_V86SegmentToLinear) {
        /* #194: ptr16:32 -- a dword offset, then the segment; CALL pushes CS and EIP as
           dwords. Real mode only, like the 16-bit form below. */
        UINT32 farOffset32 = V86ReadMemory(codeLinear + offset, X86_DWORD_SIZE);
        WORD segment = (WORD)(V86_CODE_BYTE(offset + X86_DWORD_SIZE) | (V86_CODE_BYTE(offset + X86_DWORD_SIZE + 1) << BYTE_SHIFT));
        offset += V86_FAR_POINTER32;
        if (farOffset32 > V86_OFFSET_MAX) return 0;
        if (opcode == X86_OP_CALL_FAR) { V86Push(cpu, X86_DWORD_SIZE, cpu->Segments[X86_SREG_CS]); V86Push(cpu, X86_DWORD_SIZE, (WORD)(cpu->Ip + offset)); }  /* CS: dword */
        cpu->Segments[X86_SREG_CS] = segment; cpu->Ip = (WORD)farOffset32; return 1;
    }
    if (opcode == X86_OP_JMP_FAR || opcode == X86_OP_CALL_FAR) {
        WORD farOffset, segment;
        if (isOperand32 || g_V86SegmentToLinear) return 0;
        farOffset = (WORD)(V86_CODE_BYTE(offset) | (V86_CODE_BYTE(offset + 1) << BYTE_SHIFT)); offset += X86_WORD_SIZE;
        segment = (WORD)(V86_CODE_BYTE(offset) | (V86_CODE_BYTE(offset + 1) << BYTE_SHIFT)); offset += X86_WORD_SIZE;
        if (opcode == X86_OP_CALL_FAR) {                              /* CALL FAR: push CS then IP      */
            UINT32 stackBase = V86SegmentBase(cpu->Segments[X86_SREG_SS]);
            WORD nextIp = (WORD)(cpu->Ip + offset), stackPointer;
            stackPointer = (WORD)(cpu->Registers[X86_REG_SP] - X86_WORD_SIZE); V86WriteMemory(stackBase + stackPointer, X86_WORD_SIZE, cpu->Segments[X86_SREG_CS]);
            stackPointer = (WORD)(stackPointer - X86_WORD_SIZE);      V86WriteMemory(stackBase + stackPointer, X86_WORD_SIZE, nextIp);
            cpu->Registers[X86_REG_SP] = (cpu->Registers[X86_REG_SP] & HIGH_WORD_MASK_U) | stackPointer;
        }
        cpu->Segments[X86_SREG_CS] = segment; cpu->Ip = farOffset; return 1;
    }
    /* ---- CLI/STI (FA/FB). IF is carried in the flag image so an interpreted   *
     * handler's IRET restores it and the loop-top delivery gate sees the truth. */
    if (opcode == X86_OP_CLI) { cpu->Flags &= ~EFLAGS_IF_U; cpu->Ip = (WORD)(cpu->Ip + offset); return 1; }
    if (opcode == X86_OP_STI) { cpu->Flags |=  EFLAGS_IF_U; cpu->Ip = (WORD)(cpu->Ip + offset); return 1; }

    return 0;                                          /* unmodeled: bail to V86 */
}

#endif /* V86INTERP_H */
