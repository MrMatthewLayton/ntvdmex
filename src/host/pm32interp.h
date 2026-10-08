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
 * DECLINE IS EXACT. Pm32Step() either executes one whole instruction and returns 1, or
 * returns 0 having changed NOTHING (registers, flags, EIP, memory), so the caller can hand
 * the instruction to the real CPU. Memory writes are therefore deferred until the
 * instruction is known to complete (the few instructions that write memory write it last).
 *
 * The includer MUST provide, before #include'ing this header:
 *   - fixed-width int types (uint8_t .. uint64_t, int8_t .. int64_t)
 *   - uint8_t  Pm32HostRead8(uint32_t lin);            guest byte read  (A0000 -> the VGA engine)
 *   - void     Pm32HostWrite8(uint32_t lin, uint8_t v); guest byte write (A0000 -> the VGA engine)
 *   - int      Pm32HostCanAccess(uint32_t lin, int w, int wr);  may this access proceed? (0 = decline)
 *   - uint32_t Pm32HostIn(uint16_t port, int w);
 *   - void     Pm32HostOut(uint16_t port, int w, uint32_t v);
 * Kept host-agnostic so it is unit-tested off-VM (tests/unit/pm32interp_test.c).
 */
#ifndef PM32INTERP_H
#define PM32INTERP_H
#include "../ntvdmex_types.h"

#define PM32_ARITH (EFLAGS_CF_U | EFLAGS_PF_U | EFLAGS_AF_U | EFLAGS_ZF_U | EFLAGS_SF_U | EFLAGS_OF_U)

/* Operand widths, masks and signs. */
#define PM32_PUSHAD_BYTES 32      /* eight DWORD registers                      */
/* Flags. */
#define PM32_AUXILIARY_BIT   0x10   /* the carry out of bit 3                       */
/* ModRM and SIB. */
#define PM32_MODE_DISP_FULL  2
#define PM32_RM_SIB          4
#define PM32_RM_DISP32       5
#define PM32_SIB_NO_INDEX    4
#define PM32_MODRM_SIB_LENGTH 2
#define PM32_MODRM_DISP32_LENGTH 5
/* The x86 encoding facts both interpreters share are in ntvdmex_x86.h (X86_*). */
/* The ALU group (00..3F, and 80/81/83's /r): its operations and forms. */
#define PM32_ALU_FORMS 6                /* forms 6/7 are other instructions          */
#define PM32_FORM_FROM_REGISTER_END 2   /* forms 0/1: r/m op= reg                   */
#define PM32_FORM_ACCUMULATOR_BYTE 4    /* AL op= imm8                              */
#define PM32_FORM_ACCUMULATOR 5         /* eAX op= imm                              */
/* Condition codes: pairs, the low bit negates. */
#define PM32_CC_PAIR_SHIFT 1
#define PM32_CC_OVERFLOW 0
#define PM32_CC_BELOW 1
#define PM32_CC_ZERO 2
#define PM32_CC_BELOW_OR_EQUAL 3
#define PM32_CC_SIGN 4
#define PM32_CC_PARITY 5
#define PM32_CC_LESS 6
/* Group 3. */
#define PM32_GROUP3_TEST 0
#define PM32_GROUP3_TEST_ALIAS 1
#define PM32_OUT_BIT 2                  /* E4-E7/EC-EF: bit 1 = OUT                  */
#define PM32_MAX_STRING_COUNT 0x10000u
/* Opcodes. */
#define PM32_OP2_SHLD_IMM              0xA4    /* after X86_ESCAPE */
#define PM32_OP2_SHLD_CL               0xA5    /* after X86_ESCAPE */
#define PM32_OP2_SHRD_IMM              0xAC    /* after X86_ESCAPE */
#define PM32_OP2_SHRD_CL               0xAD    /* after X86_ESCAPE */
#define PM32_OP2_IMUL                  0xAF    /* after X86_ESCAPE */

typedef struct _PM32_CPU {
    UINT32 Registers[X86_GENERAL_REGISTERS];        /* EAX ECX EDX EBX ESP EBP ESI EDI                            */
    UINT32 Eip;         /* offset in CS                                               */
    UINT32 Flags;
    UINT32 SegmentBases[X86_SEGMENT_REGISTERS];     /* segment BASES, x86 sreg order: ES CS SS DS FS GS            */
} PM32_CPU, *PPM32_CPU; typedef const PM32_CPU *PCPM32_CPU;

/* ---- register views ------------------------------------------------------------- */
static UINT32 Pm32Mask(INT width) { return width == 1 ? BYTE_MASK_U : width == X86_WORD_SIZE ? WORD_MASK_U : DWORD_MASK_U; }
static UINT32 Pm32SignBit(INT width) { return width == 1 ? X86_BYTE_SIGN_U : width == X86_WORD_SIZE ? X86_WORD_SIGN_U : X86_DWORD_SIGN_U; }
static UINT32 Pm32GetRegister(const PM32_CPU *cpu, INT registerIndex, INT width)
{
    if (width == 1) return (registerIndex < X86_BYTE_REGISTERS) ? (cpu->Registers[registerIndex] & BYTE_MASK) : ((cpu->Registers[registerIndex - X86_BYTE_REGISTERS] >> BYTE_SHIFT) & BYTE_MASK);
    return cpu->Registers[registerIndex & X86_MODRM_REGISTER_MASK] & Pm32Mask(width);
}
static VOID Pm32SetRegister(PM32_CPU *cpu, INT registerIndex, INT width, UINT32 value)
{
    if (width == 1) {
        if (registerIndex < X86_BYTE_REGISTERS) cpu->Registers[registerIndex] = (cpu->Registers[registerIndex] & (~BYTE_MASK_U)) | (value & BYTE_MASK);
        else       cpu->Registers[registerIndex - X86_BYTE_REGISTERS] = (cpu->Registers[registerIndex - X86_BYTE_REGISTERS] & (~HIGH_BYTE_MASK_U)) | ((value & BYTE_MASK) << BYTE_SHIFT);
    } else if (width == X86_WORD_SIZE) cpu->Registers[registerIndex & X86_MODRM_REGISTER_MASK] = (cpu->Registers[registerIndex & X86_MODRM_REGISTER_MASK] & HIGH_WORD_MASK_U) | (value & WORD_MASK);
    else cpu->Registers[registerIndex & X86_MODRM_REGISTER_MASK] = value;
}

/* ---- memory --------------------------------------------------------------------- */
static UINT32 Pm32ReadMemory(UINT32 linear, INT width)
{
    UINT32 value = Pm32HostRead8(linear);
    if (width >= X86_WORD_SIZE) value |= (UINT32)Pm32HostRead8(linear + 1) << BYTE_SHIFT;
    if (width == X86_DWORD_SIZE) { value |= (UINT32)Pm32HostRead8(linear + 2) << WORD_SHIFT; value |= (UINT32)Pm32HostRead8(linear + 3) << TOP_BYTE_SHIFT; }
    return value;
}
static VOID Pm32WriteMemory(UINT32 linear, INT width, UINT32 value)
{
    Pm32HostWrite8(linear, (BYTE)value);
    if (width >= X86_WORD_SIZE) Pm32HostWrite8(linear + 1, (BYTE)(value >> BYTE_SHIFT));
    if (width == X86_DWORD_SIZE) { Pm32HostWrite8(linear + 2, (BYTE)(value >> WORD_SHIFT)); Pm32HostWrite8(linear + 3, (BYTE)(value >> TOP_BYTE_SHIFT)); }
}

/* ---- flags ---------------------------------------------------------------------- */
static INT Pm32Parity(UINT32 value) { value &= BYTE_MASK; value ^= value >> 4; value ^= value >> 2; value ^= value >> 1; return !(value & 1); }
static UINT32 Pm32SzpFlags(UINT32 result, INT width)
{
    UINT32 flags = 0;
    result &= Pm32Mask(width);
    if (!result) flags |= EFLAGS_ZF_U;
    if (result & Pm32SignBit(width)) flags |= EFLAGS_SF_U;
    if (Pm32Parity(result)) flags |= EFLAGS_PF_U;
    return flags;
}
/* ADD/ADC and SUB/SBB/CMP: result + the six arithmetic flags. */
static UINT32 Pm32Add(UINT32 *flags, UINT32 first, UINT32 second, UINT32 carryIn, INT width)
{
    UINT32 mask = Pm32Mask(width), signBit = Pm32SignBit(width);
    UINT64 full = (UINT64)(first & mask) + (UINT64)(second & mask) + carryIn;
    UINT32 result = (UINT32)full & mask, newFlags = Pm32SzpFlags(result, width);
    if (full >> (width * BITS_PER_BYTE)) newFlags |= EFLAGS_CF_U;
    if (((first ^ second ^ result) & PM32_AUXILIARY_BIT)) newFlags |= EFLAGS_AF_U;
    if ((~(first ^ second) & (first ^ result)) & signBit) newFlags |= EFLAGS_OF_U;
    *flags = (*flags & ~PM32_ARITH) | newFlags;
    return result;
}
static UINT32 Pm32Subtract(UINT32 *flags, UINT32 first, UINT32 second, UINT32 borrowIn, INT width)
{
    UINT32 mask = Pm32Mask(width), signBit = Pm32SignBit(width);
    UINT32 result = (first - second - borrowIn) & mask, newFlags = Pm32SzpFlags(result, width);
    if ((UINT64)(first & mask) < (UINT64)(second & mask) + borrowIn) newFlags |= EFLAGS_CF_U;
    if (((first ^ second ^ result) & PM32_AUXILIARY_BIT)) newFlags |= EFLAGS_AF_U;
    if (((first ^ second) & (first ^ result)) & signBit) newFlags |= EFLAGS_OF_U;
    *flags = (*flags & ~PM32_ARITH) | newFlags;
    return result;
}
static UINT32 Pm32Logic(UINT32 *flags, UINT32 result, INT width)
{   /* AND/OR/XOR/TEST: CF=OF=0, AF undefined (left clear, as hardware does in practice) */
    result &= Pm32Mask(width);
    *flags = (*flags & ~PM32_ARITH) | Pm32SzpFlags(result, width);
    return result;
}
/* group-1 op: 0 ADD 1 OR 2 ADC 3 SBB 4 AND 5 SUB 6 XOR 7 CMP. Returns the result; *store
   says whether it is written back (CMP is not). */
static UINT32 Pm32Alu(UINT32 *flags, INT operation, UINT32 first, UINT32 second, INT width, INT *isStore)
{
    UINT32 carry = *flags & EFLAGS_CF_U;
    *isStore = (operation != X86_ALU_CMP);
    switch (operation) {
    case X86_ALU_ADD: return Pm32Add(flags, first, second, 0, width);
    case X86_ALU_OR: return Pm32Logic(flags, first | second, width);
    case X86_ALU_ADC: return Pm32Add(flags, first, second, carry, width);
    case X86_ALU_SBB: return Pm32Subtract(flags, first, second, carry, width);
    case X86_ALU_AND: return Pm32Logic(flags, first & second, width);
    case X86_ALU_SUB: return Pm32Subtract(flags, first, second, 0, width);
    case X86_ALU_XOR: return Pm32Logic(flags, first ^ second, width);
    default: return Pm32Subtract(flags, first, second, 0, width);
    }
}
static INT Pm32Condition(UINT32 flags, INT condition)
{
    INT isTrue;
    switch (condition >> PM32_CC_PAIR_SHIFT) {
    case PM32_CC_OVERFLOW: isTrue = (flags & EFLAGS_OF_U) != 0; break;                              /* O  */
    case PM32_CC_BELOW: isTrue = (flags & EFLAGS_CF_U) != 0; break;                              /* B  */
    case PM32_CC_ZERO: isTrue = (flags & EFLAGS_ZF_U) != 0; break;                              /* Z  */
    case PM32_CC_BELOW_OR_EQUAL: isTrue = (flags & (EFLAGS_CF_U | EFLAGS_ZF_U)) != 0; break;                   /* BE */
    case PM32_CC_SIGN: isTrue = (flags & EFLAGS_SF_U) != 0; break;                              /* S  */
    case PM32_CC_PARITY: isTrue = (flags & EFLAGS_PF_U) != 0; break;                              /* P  */
    case PM32_CC_LESS: isTrue = ((flags & EFLAGS_SF_U) != 0) != ((flags & EFLAGS_OF_U) != 0); break;     /* L  */
    default: isTrue = (flags & EFLAGS_ZF_U) || (((flags & EFLAGS_SF_U) != 0) != ((flags & EFLAGS_OF_U) != 0)); /* LE */
    }
    return (condition & 1) ? !isTrue : isTrue;
}

/* ---- decode ---------------------------------------------------------------------- */
typedef struct _PM32_MODRM {
    INT      IsMemory;     /* 1 = memory operand at Linear                               */
    UINT32 Linear;
    INT      Register;        /* the /r field                                               */
    INT      RegisterMemory;         /* register number when !IsMemory                            */
    INT      Length;        /* bytes consumed by ModRM + SIB + displacement              */
} PM32_MODRM;

/* Code fetch: CS base + EIP + i. */
static BYTE Pm32CodeByte(const PM32_CPU *cpu, UINT32 offset) { return Pm32HostRead8(cpu->SegmentBases[X86_SREG_CS] + cpu->Eip + offset); }
static UINT32 Pm32CodeImmediate(const PM32_CPU *cpu, UINT32 offset, INT width)
{
    UINT32 value = Pm32CodeByte(cpu, offset);
    if (width >= X86_WORD_SIZE) value |= (UINT32)Pm32CodeByte(cpu, offset + 1) << BYTE_SHIFT;
    if (width == X86_DWORD_SIZE) { value |= (UINT32)Pm32CodeByte(cpu, offset + 2) << WORD_SHIFT; value |= (UINT32)Pm32CodeByte(cpu, offset + 3) << TOP_BYTE_SHIFT; }
    return value;
}

/* 32-bit ModRM/SIB. `seg` is an override (0..5) or -1: default DS, or SS for an EBP/ESP base. */
static VOID Pm32Decode(const PM32_CPU *cpu, UINT32 offset, INT segmentOverride, PM32_MODRM *modrm)
{
    BYTE modrmByte = Pm32CodeByte(cpu, offset);
    INT mode = modrmByte >> X86_MODRM_MODE_SHIFT, registerMemory = modrmByte & X86_MODRM_REGISTER_MASK, length = 1, defaultSegment = X86_SREG_DS;
    UINT32 effectiveAddress = 0;
    modrm->Register = (modrmByte >> X86_MODRM_REG_SHIFT) & X86_MODRM_REGISTER_MASK;
    if (mode == X86_MODE_REGISTER) { modrm->IsMemory = 0; modrm->RegisterMemory = registerMemory; modrm->Length = 1; return; }
    modrm->IsMemory = 1;
    if (registerMemory == PM32_RM_SIB) {                                  /* SIB */
        BYTE sib = Pm32CodeByte(cpu, offset + 1);
        INT scale = sib >> X86_MODRM_MODE_SHIFT, indexRegister = (sib >> X86_MODRM_REG_SHIFT) & X86_MODRM_REGISTER_MASK, baseRegister = sib & X86_MODRM_REGISTER_MASK;
        length = PM32_MODRM_SIB_LENGTH;
        if (baseRegister == X86_REG_BP && mode == 0) { effectiveAddress = Pm32CodeImmediate(cpu, offset + length, X86_DWORD_SIZE); length += X86_DWORD_SIZE; }
        else { effectiveAddress = cpu->Registers[baseRegister]; if (baseRegister == X86_REG_SP || baseRegister == X86_REG_BP) defaultSegment = X86_SREG_SS; }
        if (indexRegister != PM32_SIB_NO_INDEX) effectiveAddress += cpu->Registers[indexRegister] << scale;
    } else if (registerMemory == PM32_RM_DISP32 && mode == 0) {               /* disp32 */
        effectiveAddress = Pm32CodeImmediate(cpu, offset + 1, X86_DWORD_SIZE); length = PM32_MODRM_DISP32_LENGTH;
    } else {
        effectiveAddress = cpu->Registers[registerMemory];
        if (registerMemory == PM32_RM_DISP32) defaultSegment = X86_SREG_SS;                         /* [EBP+d] defaults to SS */
    }
    if (mode == X86_MODE_DISP8) { effectiveAddress += (UINT32)(INT32)(INT8)Pm32CodeByte(cpu, offset + length); length += 1; }
    else if (mode == PM32_MODE_DISP_FULL) { effectiveAddress += Pm32CodeImmediate(cpu, offset + length, X86_DWORD_SIZE); length += X86_DWORD_SIZE; }
    modrm->Linear = cpu->SegmentBases[segmentOverride >= 0 ? segmentOverride : defaultSegment] + effectiveAddress;
    modrm->Length = length;
}
static UINT32 Pm32ReadRm(const PM32_CPU *cpu, const PM32_MODRM *modrm, INT width)
{ return modrm->IsMemory ? Pm32ReadMemory(modrm->Linear, width) : Pm32GetRegister(cpu, modrm->RegisterMemory, width); }

/* ---- stack (32-bit) ---------------------------------------------------------------- */
static INT Pm32CanPush(const PM32_CPU *cpu, INT width) { return Pm32HostCanAccess(cpu->SegmentBases[X86_SREG_SS] + cpu->Registers[X86_REG_SP] - (UINT32)width, width, X86_ACCESS_WRITE); }
static VOID Pm32Push(PM32_CPU *cpu, UINT32 value, INT width) { cpu->Registers[X86_REG_SP] -= (UINT32)width; Pm32WriteMemory(cpu->SegmentBases[X86_SREG_SS] + cpu->Registers[X86_REG_SP], width, value); }
static UINT32 Pm32Pop(PM32_CPU *cpu, INT width) { UINT32 value = Pm32ReadMemory(cpu->SegmentBases[X86_SREG_SS] + cpu->Registers[X86_REG_SP], width); cpu->Registers[X86_REG_SP] += (UINT32)width; return value; }

/* ---- shifts (C0/C1/D0-D3). Returns 0 to decline (RCL/RCR). ------------------------ */
static INT Pm32Shift(UINT32 *flags, INT operation, UINT32 value, UINT count, INT width, UINT32 *out)
{
    UINT32 mask = Pm32Mask(width), signBit = Pm32SignBit(width), result = value & mask, newFlags;
    UINT bits = (UINT)width * BITS_PER_BYTE;
    count &= X86_SHIFT_COUNT_MASK;
    if (!count) { *out = result; return 1; }                 /* count 0: nothing, flags untouched */
    newFlags = *flags;
    switch (operation) {
    case X86_SHIFT_ROL: case X86_SHIFT_ROR: {                               /* ROL / ROR */
        UINT rotate = count % bits;
        if (rotate) result = (operation == X86_SHIFT_ROL) ? ((result << rotate) | (result >> (bits - rotate))) & mask : ((result >> rotate) | (result << (bits - rotate))) & mask;
        newFlags &= ~(EFLAGS_CF_U | EFLAGS_OF_U);
        if (operation == X86_SHIFT_ROL) { if (result & 1) newFlags |= EFLAGS_CF_U; if (((result & signBit) != 0) != ((newFlags & EFLAGS_CF_U) != 0)) newFlags |= EFLAGS_OF_U; }
        else { if (result & signBit) newFlags |= EFLAGS_CF_U; if (((result & signBit) != 0) != ((result & (signBit >> 1)) != 0)) newFlags |= EFLAGS_OF_U; }
        *flags = newFlags; *out = result; return 1; }
    case X86_SHIFT_RCL: case X86_SHIFT_RCR: return 0;                       /* RCL / RCR: decline */
    case X86_SHIFT_SHL: case X86_SHIFT_SAL: {                               /* SHL / SAL */
        UINT32 carry = (count <= bits) ? ((result >> (bits - count)) & 1) : 0;
        result = (count < BITS_PER_DWORD) ? (result << count) & mask : 0;
        newFlags = (newFlags & ~PM32_ARITH) | Pm32SzpFlags(result, width);
        if (carry) newFlags |= EFLAGS_CF_U;
        if (((result & signBit) != 0) != (carry != 0)) newFlags |= EFLAGS_OF_U;
        *flags = newFlags; *out = result; return 1; }
    case X86_SHIFT_SHR: {                                       /* SHR */
        UINT32 carry = (result >> (count - 1)) & 1;
        UINT32 isSignSet = (result & signBit) != 0;
        result = (count < BITS_PER_DWORD) ? result >> count : 0;
        newFlags = (newFlags & ~PM32_ARITH) | Pm32SzpFlags(result, width);
        if (carry) newFlags |= EFLAGS_CF_U;
        if (isSignSet) newFlags |= EFLAGS_OF_U;
        *flags = newFlags; *out = result; return 1; }
    default: {                                      /* SAR */
        INT32 signedValue = (width == 1) ? (INT8)result : (width == X86_WORD_SIZE) ? (INT16)result : (INT32)result;
        UINT32 carry = (UINT32)(signedValue >> (count - 1)) & 1;
        result = (UINT32)(signedValue >> (count > BITS_PER_DWORD - 1 ? BITS_PER_DWORD - 1 : count)) & mask;
        newFlags = (newFlags & ~PM32_ARITH) | Pm32SzpFlags(result, width);
        if (carry) newFlags |= EFLAGS_CF_U;
        *flags = newFlags; *out = result; return 1; }
    }
}

/* ---- one instruction ---------------------------------------------------------------- */
/* Returns 1 = executed, 0 = declined with no state changed. */
static INT Pm32Step(PM32_CPU *cpu)
{
    UINT32 offset = 0;                 /* bytes consumed so far                                */
    INT segmentOverride = -1, isOperand16 = 0, repeat = 0;
    BYTE opcode;
    INT operandSize;
    for (;;) {                      /* prefixes */
        opcode = Pm32CodeByte(cpu, offset);
        if (opcode == X86_PREFIX_OPERAND_SIZE) { isOperand16 = 1; ++offset; }
        else if (opcode == X86_PREFIX_ES) { segmentOverride = X86_SREG_ES; ++offset; } else if (opcode == X86_PREFIX_CS) { segmentOverride = X86_SREG_CS; ++offset; }
        else if (opcode == X86_PREFIX_SS) { segmentOverride = X86_SREG_SS; ++offset; } else if (opcode == X86_PREFIX_DS) { segmentOverride = X86_SREG_DS; ++offset; }
        else if (opcode == X86_PREFIX_FS) { segmentOverride = X86_SREG_FS; ++offset; } else if (opcode == X86_PREFIX_GS) { segmentOverride = X86_SREG_GS; ++offset; }
        else if (opcode == X86_PREFIX_REP) { repeat = X86_REPEAT_REP; ++offset; } else if (opcode == X86_PREFIX_REPNE) { repeat = X86_REPEAT_REPNE; ++offset; }
        else if (opcode == X86_PREFIX_ADDRESS_SIZE || opcode == X86_PREFIX_LOCK) return 0;      /* 16-bit addressing / LOCK */
        else break;
        if (offset > X86_PREFIXES_MAX) return 0;
    }
    ++offset;
    operandSize = isOperand16 ? X86_WORD_SIZE : X86_DWORD_SIZE;

#define PM32_DONE(next) do { cpu->Eip += (UINT32)(next); return 1; } while (0)

    /* ---- ALU r/m,r / r,r/m / acc,imm : 00-3F except the segment/BCD holes ---- */
    if (opcode < X86_OP_INC_FIRST && (opcode & X86_MODRM_REGISTER_MASK) < PM32_ALU_FORMS) {
        INT aluOperation = opcode >> X86_MODRM_REG_SHIFT, form = opcode & X86_MODRM_REGISTER_MASK, width = (form & 1) ? operandSize : 1, isStore;
        UINT32 result;
        if (form == PM32_FORM_ACCUMULATOR_BYTE || form == PM32_FORM_ACCUMULATOR) {                        /* AL/eAX, imm */
            UINT32 second = Pm32CodeImmediate(cpu, offset, width), flags = cpu->Flags;
            result = Pm32Alu(&flags, aluOperation, Pm32GetRegister(cpu, X86_REG_AX, width), second, width, &isStore);
            if (isStore) Pm32SetRegister(cpu, X86_REG_AX, width, result);
            cpu->Flags = flags;
            PM32_DONE(offset + (UINT32)width);
        } else {
            PM32_MODRM modrm; UINT32 first, second, flags = cpu->Flags;
            Pm32Decode(cpu, offset, segmentOverride, &modrm);
            if (modrm.IsMemory && !Pm32HostCanAccess(modrm.Linear, width, form < PM32_FORM_FROM_REGISTER_END && aluOperation != X86_ALU_CMP)) return 0;
            if (form < PM32_FORM_FROM_REGISTER_END) { first = Pm32ReadRm(cpu, &modrm, width); second = Pm32GetRegister(cpu, modrm.Register, width); }
            else          { first = Pm32GetRegister(cpu, modrm.Register, width); second = Pm32ReadRm(cpu, &modrm, width); }
            result = Pm32Alu(&flags, aluOperation, first, second, width, &isStore);
            if (isStore) {
                if (form < PM32_FORM_FROM_REGISTER_END) { if (modrm.IsMemory) Pm32WriteMemory(modrm.Linear, width, result); else Pm32SetRegister(cpu, modrm.RegisterMemory, width, result); }
                else Pm32SetRegister(cpu, modrm.Register, width, result);
            }
            cpu->Flags = flags;
            PM32_DONE(offset + (UINT32)modrm.Length);
        }
    }
    /* ---- INC/DEC r32 (40-4F): CF preserved ---- */
    if (opcode >= X86_OP_INC_FIRST && opcode <= X86_OP_DEC_LAST) {
        INT registerIndex = opcode & X86_MODRM_REGISTER_MASK; UINT32 carry = cpu->Flags & EFLAGS_CF_U, flags = cpu->Flags, result;
        result = (opcode < X86_OP_DEC_FIRST) ? Pm32Add(&flags, Pm32GetRegister(cpu, registerIndex, operandSize), 1, 0, operandSize) : Pm32Subtract(&flags, Pm32GetRegister(cpu, registerIndex, operandSize), 1, 0, operandSize);
        cpu->Flags = (flags & ~EFLAGS_CF_U) | carry;
        Pm32SetRegister(cpu, registerIndex, operandSize, result);
        PM32_DONE(offset);
    }
    /* ---- PUSH/POP r (50-5F) ---- */
    if (opcode >= X86_OP_PUSH_FIRST && opcode <= X86_OP_PUSH_LAST) {
        UINT32 value = Pm32GetRegister(cpu, opcode & X86_MODRM_REGISTER_MASK, operandSize);
        if (!Pm32CanPush(cpu, operandSize)) return 0;
        Pm32Push(cpu, value, operandSize); PM32_DONE(offset);
    }
    if (opcode >= X86_OP_POP_FIRST && opcode <= X86_OP_POP_LAST) {
        UINT32 value;
        if (!Pm32HostCanAccess(cpu->SegmentBases[X86_SREG_SS] + cpu->Registers[X86_REG_SP], operandSize, X86_ACCESS_READ)) return 0;
        value = Pm32Pop(cpu, operandSize);
        Pm32SetRegister(cpu, opcode & X86_MODRM_REGISTER_MASK, operandSize, value);
        PM32_DONE(offset);
    }
    /* ---- PUSHAD / POPAD (60/61) ---- */
    if (opcode == X86_OP_PUSHAD) {
        UINT32 originalEsp = cpu->Registers[X86_REG_SP]; INT index;
        if (isOperand16 || !Pm32HostCanAccess(cpu->SegmentBases[X86_SREG_SS] + cpu->Registers[X86_REG_SP] - PM32_PUSHAD_BYTES, PM32_PUSHAD_BYTES, X86_ACCESS_WRITE)) return 0;
        for (index = 0; index < X86_GENERAL_REGISTERS; ++index) Pm32Push(cpu, index == X86_REG_SP ? originalEsp : cpu->Registers[index], X86_DWORD_SIZE);
        PM32_DONE(offset);
    }
    if (opcode == X86_OP_POPAD) {
        INT index; UINT32 saved[X86_GENERAL_REGISTERS];
        if (isOperand16 || !Pm32HostCanAccess(cpu->SegmentBases[X86_SREG_SS] + cpu->Registers[X86_REG_SP], PM32_PUSHAD_BYTES, X86_ACCESS_READ)) return 0;
        for (index = X86_GENERAL_REGISTERS - 1; index >= 0; --index) saved[index] = Pm32Pop(cpu, X86_DWORD_SIZE);
        for (index = 0; index < X86_GENERAL_REGISTERS; ++index) if (index != X86_REG_SP) cpu->Registers[index] = saved[index];   /* the saved ESP is discarded */
        PM32_DONE(offset);
    }
    /* ---- PUSH imm (68/6A) ---- */
    if (opcode == X86_OP_PUSH_IMM || opcode == X86_OP_PUSH_IMM8) {
        UINT32 value = (opcode == X86_OP_PUSH_IMM) ? Pm32CodeImmediate(cpu, offset, operandSize) : (UINT32)(INT32)(INT8)Pm32CodeByte(cpu, offset);
        if (!Pm32CanPush(cpu, operandSize)) return 0;
        Pm32Push(cpu, value, operandSize);
        PM32_DONE(offset + (opcode == X86_OP_PUSH_IMM ? (UINT32)operandSize : 1u));
    }
    /* ---- IMUL r, r/m, imm (69/6B) ---- */
    if (opcode == X86_OP_IMUL_IMM || opcode == X86_OP_IMUL_IMM8) {
        PM32_MODRM modrm; INT64 first, second, product; UINT32 result, next;
        Pm32Decode(cpu, offset, segmentOverride, &modrm);
        if (modrm.IsMemory && !Pm32HostCanAccess(modrm.Linear, operandSize, X86_ACCESS_READ)) return 0;
        first = (operandSize == X86_DWORD_SIZE) ? (INT64)(INT32)Pm32ReadRm(cpu, &modrm, X86_DWORD_SIZE) : (INT64)(INT16)Pm32ReadRm(cpu, &modrm, X86_WORD_SIZE);
        next = offset + (UINT32)modrm.Length;
        if (opcode == X86_OP_IMUL_IMM8) { second = (INT8)Pm32CodeByte(cpu, next); next += 1; }
        else { UINT32 immediate = Pm32CodeImmediate(cpu, next, operandSize); next += (UINT32)operandSize; second = (operandSize == X86_DWORD_SIZE) ? (INT64)(INT32)immediate : (INT64)(INT16)immediate; }
        product = first * second; result = (UINT32)product & Pm32Mask(operandSize);
        Pm32SetRegister(cpu, modrm.Register, operandSize, result);
        cpu->Flags &= ~(EFLAGS_CF_U | EFLAGS_OF_U);
        if (product != ((operandSize == X86_DWORD_SIZE) ? (INT64)(INT32)result : (INT64)(INT16)result)) cpu->Flags |= EFLAGS_CF_U | EFLAGS_OF_U;
        PM32_DONE(next);
    }
    /* ---- Jcc rel8 (70-7F) ---- */
    if (opcode >= X86_OP_JCC_SHORT_FIRST && opcode <= X86_OP_JCC_SHORT_LAST) {
        INT32 displacement = (INT8)Pm32CodeByte(cpu, offset);
        cpu->Eip += offset + 1;
        if (Pm32Condition(cpu->Flags, opcode & X86_CONDITION_MASK)) cpu->Eip += (UINT32)displacement;
        return 1;
    }
    /* ---- group 1 (80/81/83) ---- */
    if (opcode == X86_OP_GROUP1_IMM8 || opcode == X86_OP_GROUP1_IMM || opcode == X86_OP_GROUP1_SIMM8) {
        INT width = (opcode == X86_OP_GROUP1_IMM8) ? 1 : operandSize, isStore; PM32_MODRM modrm; UINT32 first, second, result, next, flags = cpu->Flags;
        Pm32Decode(cpu, offset, segmentOverride, &modrm);
        next = offset + (UINT32)modrm.Length;
        if (opcode == X86_OP_GROUP1_IMM) { second = Pm32CodeImmediate(cpu, next, width); next += (UINT32)width; }
        else { second = (UINT32)(INT32)(INT8)Pm32CodeByte(cpu, next); next += 1; }
        if (modrm.IsMemory && !Pm32HostCanAccess(modrm.Linear, width, modrm.Register != X86_ALU_CMP)) return 0;
        first = Pm32ReadRm(cpu, &modrm, width);
        result = Pm32Alu(&flags, modrm.Register, first, second, width, &isStore);
        if (isStore) { if (modrm.IsMemory) Pm32WriteMemory(modrm.Linear, width, result); else Pm32SetRegister(cpu, modrm.RegisterMemory, width, result); }
        cpu->Flags = flags;
        PM32_DONE(next);
    }
    /* ---- TEST r/m,r (84/85) ---- */
    if (opcode == X86_OP_TEST_BYTE || opcode == X86_OP_TEST) {
        INT width = (opcode & 1) ? operandSize : 1; PM32_MODRM modrm;
        Pm32Decode(cpu, offset, segmentOverride, &modrm);
        if (modrm.IsMemory && !Pm32HostCanAccess(modrm.Linear, width, X86_ACCESS_READ)) return 0;
        Pm32Logic(&cpu->Flags, Pm32ReadRm(cpu, &modrm, width) & Pm32GetRegister(cpu, modrm.Register, width), width);
        PM32_DONE(offset + (UINT32)modrm.Length);
    }
    /* ---- XCHG r/m,r (86/87) ---- */
    if (opcode == X86_OP_XCHG_BYTE || opcode == X86_OP_XCHG) {
        INT width = (opcode & 1) ? operandSize : 1; PM32_MODRM modrm; UINT32 first, second;
        Pm32Decode(cpu, offset, segmentOverride, &modrm);
        if (modrm.IsMemory && !Pm32HostCanAccess(modrm.Linear, width, X86_ACCESS_WRITE)) return 0;
        first = Pm32ReadRm(cpu, &modrm, width); second = Pm32GetRegister(cpu, modrm.Register, width);
        if (modrm.IsMemory) Pm32WriteMemory(modrm.Linear, width, second); else Pm32SetRegister(cpu, modrm.RegisterMemory, width, second);
        Pm32SetRegister(cpu, modrm.Register, width, first);
        PM32_DONE(offset + (UINT32)modrm.Length);
    }
    /* ---- MOV (88-8B) ---- */
    if (opcode >= X86_OP_MOV_TO_RM_BYTE && opcode <= X86_OP_MOV_FROM_RM) {
        INT width = (opcode & 1) ? operandSize : 1; PM32_MODRM modrm;
        Pm32Decode(cpu, offset, segmentOverride, &modrm);
        if (modrm.IsMemory && !Pm32HostCanAccess(modrm.Linear, width, opcode < X86_OP_MOV_FROM_RM_BYTE)) return 0;
        if (opcode < X86_OP_MOV_FROM_RM_BYTE) { UINT32 value = Pm32GetRegister(cpu, modrm.Register, width);
                         if (modrm.IsMemory) Pm32WriteMemory(modrm.Linear, width, value); else Pm32SetRegister(cpu, modrm.RegisterMemory, width, value); }
        else Pm32SetRegister(cpu, modrm.Register, width, Pm32ReadRm(cpu, &modrm, width));
        PM32_DONE(offset + (UINT32)modrm.Length);
    }
    /* ---- LEA (8D): the offset, not the linear address ---- */
    if (opcode == X86_OP_LEA) {
        PM32_MODRM modrm; UINT32 segmentBase;
        Pm32Decode(cpu, offset, -1, &modrm);
        if (!modrm.IsMemory) return 0;
        /* Pm32Decode added a segment base; LEA wants the effective address alone. The
           default segment for the form is DS or SS; subtract the one it used. */
        segmentBase = cpu->SegmentBases[X86_SREG_DS];
        { BYTE modrmByte = Pm32CodeByte(cpu, offset); INT mode = modrmByte >> X86_MODRM_MODE_SHIFT, registerMemory = modrmByte & X86_MODRM_REGISTER_MASK;
          if (registerMemory == PM32_RM_DISP32 && mode != 0) segmentBase = cpu->SegmentBases[X86_SREG_SS];
          if (registerMemory == PM32_RM_SIB) { BYTE sib = Pm32CodeByte(cpu, offset + 1); INT baseRegister = sib & X86_MODRM_REGISTER_MASK;
                         if (baseRegister == X86_REG_SP || (baseRegister == X86_REG_BP && mode != 0)) segmentBase = cpu->SegmentBases[X86_SREG_SS]; } }
        Pm32SetRegister(cpu, modrm.Register, operandSize, modrm.Linear - segmentBase);
        PM32_DONE(offset + (UINT32)modrm.Length);
    }
    /* ---- NOP / XCHG eAX,r (90-97) ---- */
    if (opcode == X86_OP_NOP) { if (repeat) return 0; PM32_DONE(offset); }               /* F3 90 = PAUSE: decline */
    if (opcode > X86_OP_NOP && opcode <= X86_OP_XCHG_LAST) {
        UINT32 first = Pm32GetRegister(cpu, X86_REG_AX, operandSize), second = Pm32GetRegister(cpu, opcode & X86_MODRM_REGISTER_MASK, operandSize);
        Pm32SetRegister(cpu, X86_REG_AX, operandSize, second); Pm32SetRegister(cpu, opcode & X86_MODRM_REGISTER_MASK, operandSize, first); PM32_DONE(offset);
    }
    /* ---- CWDE/CBW (98), CDQ/CWD (99) ---- */
    if (opcode == X86_OP_CWDE) {
        if (operandSize == X86_DWORD_SIZE) cpu->Registers[X86_REG_AX] = (UINT32)(INT32)(INT16)(cpu->Registers[X86_REG_AX] & WORD_MASK);
        else Pm32SetRegister(cpu, X86_REG_AX, X86_WORD_SIZE, (UINT32)(INT16)(INT8)(cpu->Registers[X86_REG_AX] & BYTE_MASK));
        PM32_DONE(offset);
    }
    if (opcode == X86_OP_CDQ) {
        if (operandSize == X86_DWORD_SIZE) cpu->Registers[X86_REG_DX] = (cpu->Registers[X86_REG_AX] & X86_DWORD_SIGN_U) ? DWORD_MASK_U : 0;
        else Pm32SetRegister(cpu, X86_REG_DX, X86_WORD_SIZE, (cpu->Registers[X86_REG_AX] & X86_WORD_SIGN_U) ? WORD_MASK_U : 0);
        PM32_DONE(offset);
    }
    /* ---- MOV moffs (A0-A3) ---- */
    if (opcode >= X86_OP_MOV_FROM_MOFFS_BYTE && opcode <= X86_OP_MOV_TO_MOFFS) {
        INT width = (opcode & 1) ? operandSize : 1; UINT32 linear = cpu->SegmentBases[segmentOverride >= 0 ? segmentOverride : X86_SREG_DS] + Pm32CodeImmediate(cpu, offset, X86_DWORD_SIZE);
        if (!Pm32HostCanAccess(linear, width, opcode >= X86_OP_MOV_TO_MOFFS_BYTE)) return 0;
        if (opcode < X86_OP_MOV_TO_MOFFS_BYTE) Pm32SetRegister(cpu, X86_REG_AX, width, Pm32ReadMemory(linear, width)); else Pm32WriteMemory(linear, width, Pm32GetRegister(cpu, X86_REG_AX, width));
        PM32_DONE(offset + X86_DWORD_SIZE);
    }
    /* ---- TEST acc,imm (A8/A9) ---- */
    if (opcode == X86_OP_TEST_IMM_BYTE || opcode == X86_OP_TEST_IMM) {
        INT width = (opcode & 1) ? operandSize : 1;
        Pm32Logic(&cpu->Flags, Pm32GetRegister(cpu, X86_REG_AX, width) & Pm32CodeImmediate(cpu, offset, width), width);
        PM32_DONE(offset + (UINT32)width);
    }
    /* ---- string ops: MOVS (A4/A5), STOS (AA/AB), LODS (AC/AD), with REP ---- */
    if (opcode == X86_OP_MOVSB || opcode == X86_OP_MOVS || opcode == X86_OP_STOSB || opcode == X86_OP_STOS || opcode == X86_OP_LODSB || opcode == X86_OP_LODS) {
        INT width = (opcode & 1) ? operandSize : 1;
        INT32 delta = (cpu->Flags & EFLAGS_DF_U) ? -width : width;
        UINT32 count = repeat ? cpu->Registers[X86_REG_CX] : 1, index;
        UINT32 sourceBase = cpu->SegmentBases[segmentOverride >= 0 ? segmentOverride : X86_SREG_DS], destinationBase = cpu->SegmentBases[X86_SREG_ES];
        if (repeat == X86_REPEAT_REPNE) return 0;                                  /* REPNE on MOVS/STOS: odd */
        if (count > PM32_MAX_STRING_COUNT) return 0;                              /* bounded; the CPU can have it */
        for (index = 0; index < count; ++index) {                                /* probe the whole range first */
            UINT32 sourceOffset = cpu->Registers[X86_REG_SI] + (UINT32)(delta * (INT32)index), destinationOffset = cpu->Registers[X86_REG_DI] + (UINT32)(delta * (INT32)index);
            if (opcode <= X86_OP_MOVS || opcode >= X86_OP_LODSB) if (!Pm32HostCanAccess(sourceBase + sourceOffset, width, X86_ACCESS_READ)) return 0;
            if (opcode <= X86_OP_STOS)               if (!Pm32HostCanAccess(destinationBase + destinationOffset, width, X86_ACCESS_WRITE)) return 0;
        }
        for (index = 0; index < count; ++index) {
            if (opcode == X86_OP_MOVSB || opcode == X86_OP_MOVS) Pm32WriteMemory(destinationBase + cpu->Registers[X86_REG_DI], width, Pm32ReadMemory(sourceBase + cpu->Registers[X86_REG_SI], width));
            else if (opcode == X86_OP_STOSB || opcode == X86_OP_STOS) Pm32WriteMemory(destinationBase + cpu->Registers[X86_REG_DI], width, Pm32GetRegister(cpu, X86_REG_AX, width));
            else Pm32SetRegister(cpu, X86_REG_AX, width, Pm32ReadMemory(sourceBase + cpu->Registers[X86_REG_SI], width));
            if (opcode <= X86_OP_MOVS || opcode >= X86_OP_LODSB) cpu->Registers[X86_REG_SI] += (UINT32)delta;
            if (opcode <= X86_OP_STOS) cpu->Registers[X86_REG_DI] += (UINT32)delta;
        }
        if (repeat) cpu->Registers[X86_REG_CX] = 0;
        PM32_DONE(offset);
    }
    /* ---- MOV r,imm (B0-BF) ---- */
    if (opcode >= X86_OP_MOV_IMM_BYTE_FIRST && opcode <= X86_OP_MOV_IMM_BYTE_LAST) { Pm32SetRegister(cpu, opcode & X86_MODRM_REGISTER_MASK, 1, Pm32CodeByte(cpu, offset)); PM32_DONE(offset + 1); }
    if (opcode >= X86_OP_MOV_IMM_FIRST && opcode <= X86_OP_MOV_IMM_LAST) { Pm32SetRegister(cpu, opcode & X86_MODRM_REGISTER_MASK, operandSize, Pm32CodeImmediate(cpu, offset, operandSize)); PM32_DONE(offset + (UINT32)operandSize); }
    /* ---- shifts: C0/C1 imm8, D0/D1 by 1, D2/D3 by CL ---- */
    if (opcode == X86_OP_SHIFT_IMM_BYTE || opcode == X86_OP_SHIFT_IMM || (opcode >= X86_OP_SHIFT_ONE_BYTE && opcode <= X86_OP_SHIFT_CL)) {
        INT width = (opcode & 1) ? operandSize : 1; PM32_MODRM modrm; UINT32 next, count, result, flags = cpu->Flags;
        Pm32Decode(cpu, offset, segmentOverride, &modrm);
        next = offset + (UINT32)modrm.Length;
        if (opcode <= X86_OP_SHIFT_IMM) { count = Pm32CodeByte(cpu, next); next += 1; }
        else count = (opcode <= X86_OP_SHIFT_ONE) ? 1 : (cpu->Registers[X86_REG_CX] & BYTE_MASK);
        if (modrm.IsMemory && !Pm32HostCanAccess(modrm.Linear, width, X86_ACCESS_WRITE)) return 0;
        if (!Pm32Shift(&flags, modrm.Register, Pm32ReadRm(cpu, &modrm, width), count, width, &result)) return 0;
        if (modrm.IsMemory) Pm32WriteMemory(modrm.Linear, width, result); else Pm32SetRegister(cpu, modrm.RegisterMemory, width, result);
        cpu->Flags = flags;
        PM32_DONE(next);
    }
    /* ---- RET / RET imm16 (C3/C2) ---- */
    if (opcode == X86_OP_RET || opcode == X86_OP_RET_IMM) {
        UINT32 target, extraPop = (opcode == X86_OP_RET_IMM) ? (Pm32CodeByte(cpu, offset) | ((UINT32)Pm32CodeByte(cpu, offset + 1) << BYTE_SHIFT)) : 0;
        if (isOperand16 || !Pm32HostCanAccess(cpu->SegmentBases[X86_SREG_SS] + cpu->Registers[X86_REG_SP], X86_DWORD_SIZE, X86_ACCESS_READ)) return 0;
        target = Pm32Pop(cpu, X86_DWORD_SIZE);
        cpu->Registers[X86_REG_SP] += extraPop;
        cpu->Eip = target;
        return 1;
    }
    /* ---- MOV r/m,imm (C6/C7) ---- */
    if (opcode == X86_OP_MOV_IMM_RM_BYTE || opcode == X86_OP_MOV_IMM_RM) {
        INT width = (opcode & 1) ? operandSize : 1; PM32_MODRM modrm; UINT32 value;
        Pm32Decode(cpu, offset, segmentOverride, &modrm);
        if (modrm.Register != 0) return 0;
        value = Pm32CodeImmediate(cpu, offset + (UINT32)modrm.Length, width);
        if (modrm.IsMemory && !Pm32HostCanAccess(modrm.Linear, width, X86_ACCESS_WRITE)) return 0;
        if (modrm.IsMemory) Pm32WriteMemory(modrm.Linear, width, value); else Pm32SetRegister(cpu, modrm.RegisterMemory, width, value);
        PM32_DONE(offset + (UINT32)modrm.Length + (UINT32)width);
    }
    /* ---- IN/OUT (E4-E7 imm8, EC-EF DX) ---- */
    if ((opcode >= X86_OP_IN_IMM_BYTE && opcode <= X86_OP_OUT_IMM) || (opcode >= X86_OP_IN_DX_BYTE && opcode <= X86_OP_OUT_DX)) {
        INT width = (opcode & 1) ? operandSize : 1, isOut = (opcode & PM32_OUT_BIT) != 0;
        WORD port = (opcode >= X86_OP_IN_DX_BYTE) ? (WORD)(cpu->Registers[X86_REG_DX] & WORD_MASK) : (WORD)Pm32CodeByte(cpu, offset);
        UINT32 next = (opcode >= X86_OP_IN_DX_BYTE) ? offset : offset + 1;
        if (isOut) Pm32HostOut(port, width, Pm32GetRegister(cpu, X86_REG_AX, width));
        else Pm32SetRegister(cpu, X86_REG_AX, width, Pm32HostIn(port, width));
        PM32_DONE(next);
    }
    /* ---- CALL rel32 (E8), JMP rel32 (E9), JMP rel8 (EB) ---- */
    if (opcode == X86_OP_CALL) {
        UINT32 displacement = Pm32CodeImmediate(cpu, offset, X86_DWORD_SIZE);
        if (isOperand16 || !Pm32CanPush(cpu, X86_DWORD_SIZE)) return 0;
        Pm32Push(cpu, cpu->Eip + offset + X86_DWORD_SIZE, X86_DWORD_SIZE);
        cpu->Eip += offset + X86_DWORD_SIZE + displacement; return 1;
    }
    if (opcode == X86_OP_JMP) { if (isOperand16) return 0; cpu->Eip += offset + X86_DWORD_SIZE + Pm32CodeImmediate(cpu, offset, X86_DWORD_SIZE); return 1; }
    if (opcode == X86_OP_JMP_SHORT) { cpu->Eip += offset + 1 + (UINT32)(INT32)(INT8)Pm32CodeByte(cpu, offset); return 1; }
    /* ---- flag ops ---- */
    if (opcode == X86_OP_CMC) { cpu->Flags ^= EFLAGS_CF_U; PM32_DONE(offset); }
    if (opcode == X86_OP_CLC) { cpu->Flags &= ~EFLAGS_CF_U; PM32_DONE(offset); }
    if (opcode == X86_OP_STC) { cpu->Flags |= EFLAGS_CF_U; PM32_DONE(offset); }
    if (opcode == X86_OP_CLD) { cpu->Flags &= ~EFLAGS_DF_U; PM32_DONE(offset); }
    if (opcode == X86_OP_STD) { cpu->Flags |= EFLAGS_DF_U; PM32_DONE(offset); }
    /* ---- group 3 (F6/F7) ---- */
    if (opcode == X86_OP_GROUP3_BYTE || opcode == X86_OP_GROUP3) {
        INT width = (opcode == X86_OP_GROUP3_BYTE) ? 1 : operandSize; PM32_MODRM modrm; UINT32 operand, next;
        Pm32Decode(cpu, offset, segmentOverride, &modrm);
        next = offset + (UINT32)modrm.Length;
        if (modrm.IsMemory && !Pm32HostCanAccess(modrm.Linear, width, modrm.Register == X86_GROUP3_NOT || modrm.Register == X86_GROUP3_NEG)) return 0;
        operand = Pm32ReadRm(cpu, &modrm, width);
        switch (modrm.Register) {
        case PM32_GROUP3_TEST: case PM32_GROUP3_TEST_ALIAS: Pm32Logic(&cpu->Flags, operand & Pm32CodeImmediate(cpu, next, width), width); next += (UINT32)width; break;
        case X86_GROUP3_NOT: { UINT32 result = ~operand & Pm32Mask(width); if (modrm.IsMemory) Pm32WriteMemory(modrm.Linear, width, result); else Pm32SetRegister(cpu, modrm.RegisterMemory, width, result); break; }
        case X86_GROUP3_NEG: { UINT32 flags = cpu->Flags, result = Pm32Subtract(&flags, 0, operand, 0, width);
                  if (operand & Pm32Mask(width)) flags |= EFLAGS_CF_U; else flags &= ~EFLAGS_CF_U;
                  if (modrm.IsMemory) Pm32WriteMemory(modrm.Linear, width, result); else Pm32SetRegister(cpu, modrm.RegisterMemory, width, result);
                  cpu->Flags = flags; break; }
        case X86_GROUP3_MUL: case X86_GROUP3_IMUL: {                                        /* MUL / IMUL acc */
            INT isOverflow;
            if (width == 1) {
                UINT32 product = (modrm.Register == X86_GROUP3_MUL) ? (cpu->Registers[X86_REG_AX] & BYTE_MASK) * operand
                                          : (UINT32)((INT32)(INT8)cpu->Registers[X86_REG_AX] * (INT32)(INT8)operand) & WORD_MASK;
                Pm32SetRegister(cpu, X86_REG_AX, X86_WORD_SIZE, product);
                isOverflow = (modrm.Register == X86_GROUP3_MUL) ? (product >> BYTE_SHIFT) != 0 : (INT16)product != (INT8)product;
            } else if (width == X86_WORD_SIZE) {
                UINT32 product = (modrm.Register == X86_GROUP3_MUL) ? (cpu->Registers[X86_REG_AX] & WORD_MASK) * operand
                                          : (UINT32)((INT32)(INT16)cpu->Registers[X86_REG_AX] * (INT32)(INT16)operand);
                Pm32SetRegister(cpu, X86_REG_AX, X86_WORD_SIZE, product); Pm32SetRegister(cpu, X86_REG_DX, X86_WORD_SIZE, product >> WORD_SHIFT);
                isOverflow = (modrm.Register == X86_GROUP3_MUL) ? (product >> WORD_SHIFT) != 0 : (INT32)product != (INT16)product;
            } else {
                UINT64 product = (modrm.Register == X86_GROUP3_MUL) ? (UINT64)cpu->Registers[X86_REG_AX] * operand
                                          : (UINT64)((INT64)(INT32)cpu->Registers[X86_REG_AX] * (INT64)(INT32)operand);
                cpu->Registers[X86_REG_AX] = (UINT32)product; cpu->Registers[X86_REG_DX] = (UINT32)(product >> DWORD_SHIFT);
                isOverflow = (modrm.Register == X86_GROUP3_MUL) ? (product >> DWORD_SHIFT) != 0 : (INT64)product != (INT32)product;
            }
            cpu->Flags &= ~(EFLAGS_CF_U | EFLAGS_OF_U);
            if (isOverflow) cpu->Flags |= EFLAGS_CF_U | EFLAGS_OF_U;
            break; }
        default: {                                               /* DIV / IDIV: #DE declines */
            if (!(operand & Pm32Mask(width))) return 0;
            if (width == 1) {
                UINT32 dividend = cpu->Registers[X86_REG_AX] & WORD_MASK;
                if (modrm.Register == X86_GROUP3_DIV) { UINT32 quotient = dividend / operand, remainder = dividend % operand; if (quotient > BYTE_MASK) return 0;
                                  Pm32SetRegister(cpu, X86_REG_AX, X86_WORD_SIZE, quotient | (remainder << BYTE_SHIFT)); }
                else { INT32 signedDividend = (INT16)dividend, divisor = (INT8)operand, quotient = signedDividend / divisor, remainder = signedDividend % divisor;
                       if (quotient > INT8_MAX_VALUE || quotient < INT8_MIN_VALUE) return 0; Pm32SetRegister(cpu, X86_REG_AX, X86_WORD_SIZE, (quotient & BYTE_MASK) | ((remainder & BYTE_MASK) << BYTE_SHIFT)); }
            } else if (width == X86_WORD_SIZE) {
                UINT32 dividend = ((cpu->Registers[X86_REG_DX] & WORD_MASK) << WORD_SHIFT) | (cpu->Registers[X86_REG_AX] & WORD_MASK);
                if (modrm.Register == X86_GROUP3_DIV) { UINT32 quotient = dividend / operand, remainder = dividend % operand; if (quotient > WORD_MASK) return 0;
                                  Pm32SetRegister(cpu, X86_REG_AX, X86_WORD_SIZE, quotient); Pm32SetRegister(cpu, X86_REG_DX, X86_WORD_SIZE, remainder); }
                else { INT32 signedDividend = (INT32)dividend, divisor = (INT16)operand, quotient = signedDividend / divisor, remainder = signedDividend % divisor;
                       if (quotient > INT16_MAX_VALUE || quotient < INT16_MIN_VALUE) return 0; Pm32SetRegister(cpu, X86_REG_AX, X86_WORD_SIZE, (UINT32)quotient); Pm32SetRegister(cpu, X86_REG_DX, X86_WORD_SIZE, (UINT32)remainder); }
            } else {
                UINT64 dividend = ((UINT64)cpu->Registers[X86_REG_DX] << DWORD_SHIFT) | cpu->Registers[X86_REG_AX];
                if (modrm.Register == X86_GROUP3_DIV) { UINT64 quotient = dividend / operand, remainder = dividend % operand; if (quotient > DWORD_MASK_U) return 0;
                                  cpu->Registers[X86_REG_AX] = (UINT32)quotient; cpu->Registers[X86_REG_DX] = (UINT32)remainder; }
                else { INT64 signedDividend = (INT64)dividend, divisor = (INT32)operand, quotient, remainder;
                       if (divisor == -1 && signedDividend == (INT64)0x8000000000000000ULL) return 0;
                       quotient = signedDividend / divisor; remainder = signedDividend % divisor;
                       if (quotient > 2147483647LL || quotient < -2147483648LL) return 0;
                       cpu->Registers[X86_REG_AX] = (UINT32)quotient; cpu->Registers[X86_REG_DX] = (UINT32)remainder; }
            }
            break; }
        }
        PM32_DONE(next);
    }
    /* ---- group 4/5 (FE/FF): INC/DEC r/m; FF: CALL/JMP near r/m, PUSH r/m ---- */
    if (opcode == X86_OP_GROUP4 || opcode == X86_OP_GROUP5) {
        INT width = (opcode == X86_OP_GROUP4) ? 1 : operandSize; PM32_MODRM modrm;
        Pm32Decode(cpu, offset, segmentOverride, &modrm);
        if (modrm.Register <= 1) {
            UINT32 flags = cpu->Flags, carry = cpu->Flags & EFLAGS_CF_U, result;
            if (modrm.IsMemory && !Pm32HostCanAccess(modrm.Linear, width, X86_ACCESS_WRITE)) return 0;
            result = (modrm.Register == 0) ? Pm32Add(&flags, Pm32ReadRm(cpu, &modrm, width), 1, 0, width) : Pm32Subtract(&flags, Pm32ReadRm(cpu, &modrm, width), 1, 0, width);
            if (modrm.IsMemory) Pm32WriteMemory(modrm.Linear, width, result); else Pm32SetRegister(cpu, modrm.RegisterMemory, width, result);
            cpu->Flags = (flags & ~EFLAGS_CF_U) | carry;
            PM32_DONE(offset + (UINT32)modrm.Length);
        }
        if (opcode == X86_OP_GROUP5 && (modrm.Register == X86_GROUP5_CALL || modrm.Register == X86_GROUP5_JMP) && !isOperand16) {
            UINT32 target;
            if (modrm.IsMemory && !Pm32HostCanAccess(modrm.Linear, X86_DWORD_SIZE, X86_ACCESS_READ)) return 0;
            target = Pm32ReadRm(cpu, &modrm, X86_DWORD_SIZE);
            if (modrm.Register == X86_GROUP5_CALL) { if (!Pm32CanPush(cpu, X86_DWORD_SIZE)) return 0; Pm32Push(cpu, cpu->Eip + offset + (UINT32)modrm.Length, X86_DWORD_SIZE); }
            cpu->Eip = target; return 1;
        }
        if (opcode == X86_OP_GROUP5 && modrm.Register == X86_GROUP5_PUSH) {
            UINT32 value;
            if (modrm.IsMemory && !Pm32HostCanAccess(modrm.Linear, operandSize, X86_ACCESS_READ)) return 0;
            if (!Pm32CanPush(cpu, operandSize)) return 0;
            value = Pm32ReadRm(cpu, &modrm, operandSize);
            Pm32Push(cpu, value, operandSize);
            PM32_DONE(offset + (UINT32)modrm.Length);
        }
        return 0;
    }
    /* ---- 0F xx ---- */
    if (opcode == X86_ESCAPE) {
        BYTE opcode2 = Pm32CodeByte(cpu, offset);
        ++offset;
        if (opcode2 >= X86_JCC_NEAR_FIRST && opcode2 <= X86_JCC_NEAR_LAST) {                          /* Jcc rel32 */
            UINT32 displacement = Pm32CodeImmediate(cpu, offset, X86_DWORD_SIZE);
            if (isOperand16) return 0;
            cpu->Eip += offset + X86_DWORD_SIZE;
            if (Pm32Condition(cpu->Flags, opcode2 & X86_CONDITION_MASK)) cpu->Eip += displacement;
            return 1;
        }
        if (opcode2 >= X86_OP2_SETCC_FIRST && opcode2 <= X86_OP2_SETCC_LAST) {                          /* SETcc r/m8 */
            PM32_MODRM modrm; UINT32 value = (UINT32)Pm32Condition(cpu->Flags, opcode2 & X86_CONDITION_MASK);
            Pm32Decode(cpu, offset, segmentOverride, &modrm);
            if (modrm.IsMemory && !Pm32HostCanAccess(modrm.Linear, 1, X86_ACCESS_WRITE)) return 0;
            if (modrm.IsMemory) Pm32WriteMemory(modrm.Linear, 1, value); else Pm32SetRegister(cpu, modrm.RegisterMemory, 1, value);
            PM32_DONE(offset + (UINT32)modrm.Length);
        }
        if (opcode2 == X86_OP2_MOVZX_BYTE || opcode2 == X86_OP2_MOVZX_WORD || opcode2 == X86_OP2_MOVSX_BYTE || opcode2 == X86_OP2_MOVSX_WORD) {  /* MOVZX / MOVSX */
            INT sourceWidth = (opcode2 & 1) ? X86_WORD_SIZE : 1; PM32_MODRM modrm; UINT32 value;
            Pm32Decode(cpu, offset, segmentOverride, &modrm);
            if (modrm.IsMemory && !Pm32HostCanAccess(modrm.Linear, sourceWidth, X86_ACCESS_READ)) return 0;
            value = Pm32ReadRm(cpu, &modrm, sourceWidth);
            if (opcode2 >= X86_OP2_MOVSX_BYTE) value = (sourceWidth == 1) ? (UINT32)(INT32)(INT8)value : (UINT32)(INT32)(INT16)value;
            Pm32SetRegister(cpu, modrm.Register, operandSize, value);
            PM32_DONE(offset + (UINT32)modrm.Length);
        }
        if (opcode2 == PM32_OP2_IMUL) {                                        /* IMUL r, r/m */
            PM32_MODRM modrm; INT64 product; UINT32 result;
            Pm32Decode(cpu, offset, segmentOverride, &modrm);
            if (modrm.IsMemory && !Pm32HostCanAccess(modrm.Linear, operandSize, X86_ACCESS_READ)) return 0;
            if (operandSize == X86_DWORD_SIZE) product = (INT64)(INT32)cpu->Registers[modrm.Register] * (INT64)(INT32)Pm32ReadRm(cpu, &modrm, X86_DWORD_SIZE);
            else        product = (INT64)(INT16)cpu->Registers[modrm.Register] * (INT64)(INT16)Pm32ReadRm(cpu, &modrm, X86_WORD_SIZE);
            result = (UINT32)product & Pm32Mask(operandSize);
            Pm32SetRegister(cpu, modrm.Register, operandSize, result);
            cpu->Flags &= ~(EFLAGS_CF_U | EFLAGS_OF_U);
            if (product != ((operandSize == X86_DWORD_SIZE) ? (INT64)(INT32)result : (INT64)(INT16)result)) cpu->Flags |= EFLAGS_CF_U | EFLAGS_OF_U;
            PM32_DONE(offset + (UINT32)modrm.Length);
        }
        if (opcode2 == PM32_OP2_SHLD_IMM || opcode2 == PM32_OP2_SHLD_CL || opcode2 == PM32_OP2_SHRD_IMM || opcode2 == PM32_OP2_SHRD_CL) {  /* SHLD / SHRD */
            PM32_MODRM modrm; UINT32 next, count, destinationValue, sourceValue, result, flags, bits = (UINT32)operandSize * BITS_PER_BYTE, mask = Pm32Mask(operandSize);
            Pm32Decode(cpu, offset, segmentOverride, &modrm);
            next = offset + (UINT32)modrm.Length;
            if (opcode2 == PM32_OP2_SHLD_IMM || opcode2 == PM32_OP2_SHRD_IMM) { count = Pm32CodeByte(cpu, next); next += 1; } else count = cpu->Registers[X86_REG_CX] & BYTE_MASK;
            count &= X86_SHIFT_COUNT_MASK;
            if (modrm.IsMemory && !Pm32HostCanAccess(modrm.Linear, operandSize, X86_ACCESS_WRITE)) return 0;
            if (!count) PM32_DONE(next);
            if (count > bits) return 0;                            /* undefined: let the CPU */
            destinationValue = Pm32ReadRm(cpu, &modrm, operandSize); sourceValue = Pm32GetRegister(cpu, modrm.Register, operandSize);
            flags = cpu->Flags;
            if (opcode2 <= PM32_OP2_SHLD_CL) {                                    /* SHLD */
                UINT64 concatenated = ((UINT64)destinationValue << bits) | sourceValue;
                result = (UINT32)((concatenated << count) >> bits) & mask;
                flags = (flags & ~PM32_ARITH) | Pm32SzpFlags(result, operandSize);
                if ((destinationValue >> (bits - count)) & 1) flags |= EFLAGS_CF_U;
                if (((result ^ destinationValue) & Pm32SignBit(operandSize))) flags |= EFLAGS_OF_U;
            } else {                                             /* SHRD */
                UINT64 concatenated = ((UINT64)sourceValue << bits) | destinationValue;
                result = (UINT32)(concatenated >> count) & mask;
                flags = (flags & ~PM32_ARITH) | Pm32SzpFlags(result, operandSize);
                if ((destinationValue >> (count - 1)) & 1) flags |= EFLAGS_CF_U;
                if (((result ^ destinationValue) & Pm32SignBit(operandSize))) flags |= EFLAGS_OF_U;
            }
            if (modrm.IsMemory) Pm32WriteMemory(modrm.Linear, operandSize, result); else Pm32SetRegister(cpu, modrm.RegisterMemory, operandSize, result);
            cpu->Flags = flags;
            PM32_DONE(next);
        }
        return 0;
    }
#undef PM32_DONE
    return 0;
}

#endif /* PM32INTERP_H */
