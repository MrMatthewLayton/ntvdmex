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

#define P32_CF 0x0001u
#define P32_PF 0x0004u
#define P32_AF 0x0010u
#define P32_ZF 0x0040u
#define P32_SF 0x0080u
#define P32_DF 0x0400u
#define P32_OF 0x0800u
#define P32_ARITH (P32_CF | P32_PF | P32_AF | P32_ZF | P32_SF | P32_OF)

typedef struct _PM32_CPU {
    UINT32 Registers[8];        /* EAX ECX EDX EBX ESP EBP ESI EDI                            */
    UINT32 Eip;         /* offset in CS                                               */
    UINT32 Flags;
    UINT32 SegmentBases[6];     /* segment BASES, x86 sreg order: ES CS SS DS FS GS            */
} PM32_CPU, *PPM32_CPU; typedef const PM32_CPU *PCPM32_CPU;

/* ---- register views ------------------------------------------------------------- */
static UINT32 Pm32Mask(INT width) { return width == 1 ? 0xFFu : width == 2 ? 0xFFFFu : 0xFFFFFFFFu; }
static UINT32 Pm32SignBit(INT width) { return width == 1 ? 0x80u : width == 2 ? 0x8000u : 0x80000000u; }
static UINT32 Pm32GetRegister(const PM32_CPU *cpu, INT registerIndex, INT width)
{
    if (width == 1) return (registerIndex < 4) ? (cpu->Registers[registerIndex] & 0xFF) : ((cpu->Registers[registerIndex - 4] >> 8) & 0xFF);
    return cpu->Registers[registerIndex & 7] & Pm32Mask(width);
}
static VOID Pm32SetRegister(PM32_CPU *cpu, INT registerIndex, INT width, UINT32 value)
{
    if (width == 1) {
        if (registerIndex < 4) cpu->Registers[registerIndex] = (cpu->Registers[registerIndex] & 0xFFFFFF00u) | (value & 0xFF);
        else       cpu->Registers[registerIndex - 4] = (cpu->Registers[registerIndex - 4] & 0xFFFF00FFu) | ((value & 0xFF) << 8);
    } else if (width == 2) cpu->Registers[registerIndex & 7] = (cpu->Registers[registerIndex & 7] & 0xFFFF0000u) | (value & 0xFFFF);
    else cpu->Registers[registerIndex & 7] = value;
}

/* ---- memory --------------------------------------------------------------------- */
static UINT32 Pm32ReadMemory(UINT32 linear, INT width)
{
    UINT32 value = Pm32HostRead8(linear);
    if (width >= 2) value |= (UINT32)Pm32HostRead8(linear + 1) << 8;
    if (width == 4) { value |= (UINT32)Pm32HostRead8(linear + 2) << 16; value |= (UINT32)Pm32HostRead8(linear + 3) << 24; }
    return value;
}
static VOID Pm32WriteMemory(UINT32 linear, INT width, UINT32 value)
{
    Pm32HostWrite8(linear, (BYTE)value);
    if (width >= 2) Pm32HostWrite8(linear + 1, (BYTE)(value >> 8));
    if (width == 4) { Pm32HostWrite8(linear + 2, (BYTE)(value >> 16)); Pm32HostWrite8(linear + 3, (BYTE)(value >> 24)); }
}

/* ---- flags ---------------------------------------------------------------------- */
static INT Pm32Parity(UINT32 value) { value &= 0xFF; value ^= value >> 4; value ^= value >> 2; value ^= value >> 1; return !(value & 1); }
static UINT32 Pm32SzpFlags(UINT32 result, INT width)
{
    UINT32 flags = 0;
    result &= Pm32Mask(width);
    if (!result) flags |= P32_ZF;
    if (result & Pm32SignBit(width)) flags |= P32_SF;
    if (Pm32Parity(result)) flags |= P32_PF;
    return flags;
}
/* ADD/ADC and SUB/SBB/CMP: result + the six arithmetic flags. */
static UINT32 Pm32Add(UINT32 *flags, UINT32 first, UINT32 second, UINT32 carryIn, INT width)
{
    UINT32 mask = Pm32Mask(width), signBit = Pm32SignBit(width);
    UINT64 full = (UINT64)(first & mask) + (UINT64)(second & mask) + carryIn;
    UINT32 result = (UINT32)full & mask, newFlags = Pm32SzpFlags(result, width);
    if (full >> (width * 8)) newFlags |= P32_CF;
    if (((first ^ second ^ result) & 0x10)) newFlags |= P32_AF;
    if ((~(first ^ second) & (first ^ result)) & signBit) newFlags |= P32_OF;
    *flags = (*flags & ~P32_ARITH) | newFlags;
    return result;
}
static UINT32 Pm32Subtract(UINT32 *flags, UINT32 first, UINT32 second, UINT32 borrowIn, INT width)
{
    UINT32 mask = Pm32Mask(width), signBit = Pm32SignBit(width);
    UINT32 result = (first - second - borrowIn) & mask, newFlags = Pm32SzpFlags(result, width);
    if ((UINT64)(first & mask) < (UINT64)(second & mask) + borrowIn) newFlags |= P32_CF;
    if (((first ^ second ^ result) & 0x10)) newFlags |= P32_AF;
    if (((first ^ second) & (first ^ result)) & signBit) newFlags |= P32_OF;
    *flags = (*flags & ~P32_ARITH) | newFlags;
    return result;
}
static UINT32 Pm32Logic(UINT32 *flags, UINT32 result, INT width)
{   /* AND/OR/XOR/TEST: CF=OF=0, AF undefined (left clear, as hardware does in practice) */
    result &= Pm32Mask(width);
    *flags = (*flags & ~P32_ARITH) | Pm32SzpFlags(result, width);
    return result;
}
/* group-1 op: 0 ADD 1 OR 2 ADC 3 SBB 4 AND 5 SUB 6 XOR 7 CMP. Returns the result; *store
   says whether it is written back (CMP is not). */
static UINT32 Pm32Alu(UINT32 *flags, INT operation, UINT32 first, UINT32 second, INT width, INT *isStore)
{
    UINT32 carry = *flags & P32_CF;
    *isStore = (operation != 7);
    switch (operation) {
    case 0: return Pm32Add(flags, first, second, 0, width);
    case 1: return Pm32Logic(flags, first | second, width);
    case 2: return Pm32Add(flags, first, second, carry, width);
    case 3: return Pm32Subtract(flags, first, second, carry, width);
    case 4: return Pm32Logic(flags, first & second, width);
    case 5: return Pm32Subtract(flags, first, second, 0, width);
    case 6: return Pm32Logic(flags, first ^ second, width);
    default: return Pm32Subtract(flags, first, second, 0, width);
    }
}
static INT Pm32Condition(UINT32 flags, INT condition)
{
    INT isTrue;
    switch (condition >> 1) {
    case 0: isTrue = (flags & P32_OF) != 0; break;                              /* O  */
    case 1: isTrue = (flags & P32_CF) != 0; break;                              /* B  */
    case 2: isTrue = (flags & P32_ZF) != 0; break;                              /* Z  */
    case 3: isTrue = (flags & (P32_CF | P32_ZF)) != 0; break;                   /* BE */
    case 4: isTrue = (flags & P32_SF) != 0; break;                              /* S  */
    case 5: isTrue = (flags & P32_PF) != 0; break;                              /* P  */
    case 6: isTrue = ((flags & P32_SF) != 0) != ((flags & P32_OF) != 0); break;     /* L  */
    default: isTrue = (flags & P32_ZF) || (((flags & P32_SF) != 0) != ((flags & P32_OF) != 0)); /* LE */
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
static BYTE Pm32CodeByte(const PM32_CPU *cpu, UINT32 offset) { return Pm32HostRead8(cpu->SegmentBases[1] + cpu->Eip + offset); }
static UINT32 Pm32CodeImmediate(const PM32_CPU *cpu, UINT32 offset, INT width)
{
    UINT32 value = Pm32CodeByte(cpu, offset);
    if (width >= 2) value |= (UINT32)Pm32CodeByte(cpu, offset + 1) << 8;
    if (width == 4) { value |= (UINT32)Pm32CodeByte(cpu, offset + 2) << 16; value |= (UINT32)Pm32CodeByte(cpu, offset + 3) << 24; }
    return value;
}

/* 32-bit ModRM/SIB. `seg` is an override (0..5) or -1: default DS, or SS for an EBP/ESP base. */
static VOID Pm32Decode(const PM32_CPU *cpu, UINT32 offset, INT segmentOverride, PM32_MODRM *modrm)
{
    BYTE modrmByte = Pm32CodeByte(cpu, offset);
    INT mode = modrmByte >> 6, registerMemory = modrmByte & 7, length = 1, defaultSegment = 3;
    UINT32 effectiveAddress = 0;
    modrm->Register = (modrmByte >> 3) & 7;
    if (mode == 3) { modrm->IsMemory = 0; modrm->RegisterMemory = registerMemory; modrm->Length = 1; return; }
    modrm->IsMemory = 1;
    if (registerMemory == 4) {                                  /* SIB */
        BYTE sib = Pm32CodeByte(cpu, offset + 1);
        INT scale = sib >> 6, indexRegister = (sib >> 3) & 7, baseRegister = sib & 7;
        length = 2;
        if (baseRegister == 5 && mode == 0) { effectiveAddress = Pm32CodeImmediate(cpu, offset + length, 4); length += 4; }
        else { effectiveAddress = cpu->Registers[baseRegister]; if (baseRegister == 4 || baseRegister == 5) defaultSegment = 2; }
        if (indexRegister != 4) effectiveAddress += cpu->Registers[indexRegister] << scale;
    } else if (registerMemory == 5 && mode == 0) {               /* disp32 */
        effectiveAddress = Pm32CodeImmediate(cpu, offset + 1, 4); length = 5;
    } else {
        effectiveAddress = cpu->Registers[registerMemory];
        if (registerMemory == 5) defaultSegment = 2;                         /* [EBP+d] defaults to SS */
    }
    if (mode == 1) { effectiveAddress += (UINT32)(INT32)(INT8)Pm32CodeByte(cpu, offset + length); length += 1; }
    else if (mode == 2) { effectiveAddress += Pm32CodeImmediate(cpu, offset + length, 4); length += 4; }
    modrm->Linear = cpu->SegmentBases[segmentOverride >= 0 ? segmentOverride : defaultSegment] + effectiveAddress;
    modrm->Length = length;
}
static UINT32 Pm32ReadRm(const PM32_CPU *cpu, const PM32_MODRM *modrm, INT width)
{ return modrm->IsMemory ? Pm32ReadMemory(modrm->Linear, width) : Pm32GetRegister(cpu, modrm->RegisterMemory, width); }

/* ---- stack (32-bit) ---------------------------------------------------------------- */
static INT Pm32CanPush(const PM32_CPU *cpu, INT width) { return Pm32HostCanAccess(cpu->SegmentBases[2] + cpu->Registers[4] - (UINT32)width, width, 1); }
static VOID Pm32Push(PM32_CPU *cpu, UINT32 value, INT width) { cpu->Registers[4] -= (UINT32)width; Pm32WriteMemory(cpu->SegmentBases[2] + cpu->Registers[4], width, value); }
static UINT32 Pm32Pop(PM32_CPU *cpu, INT width) { UINT32 value = Pm32ReadMemory(cpu->SegmentBases[2] + cpu->Registers[4], width); cpu->Registers[4] += (UINT32)width; return value; }

/* ---- shifts (C0/C1/D0-D3). Returns 0 to decline (RCL/RCR). ------------------------ */
static INT Pm32Shift(UINT32 *flags, INT operation, UINT32 value, UINT count, INT width, UINT32 *out)
{
    UINT32 mask = Pm32Mask(width), signBit = Pm32SignBit(width), result = value & mask, newFlags;
    UINT bits = (UINT)width * 8;
    count &= 0x1F;
    if (!count) { *out = result; return 1; }                 /* count 0: nothing, flags untouched */
    newFlags = *flags;
    switch (operation) {
    case 0: case 1: {                               /* ROL / ROR */
        UINT rotate = count % bits;
        if (rotate) result = (operation == 0) ? ((result << rotate) | (result >> (bits - rotate))) & mask : ((result >> rotate) | (result << (bits - rotate))) & mask;
        newFlags &= ~(P32_CF | P32_OF);
        if (operation == 0) { if (result & 1) newFlags |= P32_CF; if (((result & signBit) != 0) != ((newFlags & P32_CF) != 0)) newFlags |= P32_OF; }
        else { if (result & signBit) newFlags |= P32_CF; if (((result & signBit) != 0) != ((result & (signBit >> 1)) != 0)) newFlags |= P32_OF; }
        *flags = newFlags; *out = result; return 1; }
    case 2: case 3: return 0;                       /* RCL / RCR: decline */
    case 4: case 6: {                               /* SHL / SAL */
        UINT32 carry = (count <= bits) ? ((result >> (bits - count)) & 1) : 0;
        result = (count < 32) ? (result << count) & mask : 0;
        newFlags = (newFlags & ~P32_ARITH) | Pm32SzpFlags(result, width);
        if (carry) newFlags |= P32_CF;
        if (((result & signBit) != 0) != (carry != 0)) newFlags |= P32_OF;
        *flags = newFlags; *out = result; return 1; }
    case 5: {                                       /* SHR */
        UINT32 carry = (result >> (count - 1)) & 1;
        UINT32 isSignSet = (result & signBit) != 0;
        result = (count < 32) ? result >> count : 0;
        newFlags = (newFlags & ~P32_ARITH) | Pm32SzpFlags(result, width);
        if (carry) newFlags |= P32_CF;
        if (isSignSet) newFlags |= P32_OF;
        *flags = newFlags; *out = result; return 1; }
    default: {                                      /* SAR */
        INT32 signedValue = (width == 1) ? (INT8)result : (width == 2) ? (INT16)result : (INT32)result;
        UINT32 carry = (UINT32)(signedValue >> (count - 1)) & 1;
        result = (UINT32)(signedValue >> (count > 31 ? 31 : count)) & mask;
        newFlags = (newFlags & ~P32_ARITH) | Pm32SzpFlags(result, width);
        if (carry) newFlags |= P32_CF;
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
        if (opcode == 0x66) { isOperand16 = 1; ++offset; }
        else if (opcode == 0x26) { segmentOverride = 0; ++offset; } else if (opcode == 0x2E) { segmentOverride = 1; ++offset; }
        else if (opcode == 0x36) { segmentOverride = 2; ++offset; } else if (opcode == 0x3E) { segmentOverride = 3; ++offset; }
        else if (opcode == 0x64) { segmentOverride = 4; ++offset; } else if (opcode == 0x65) { segmentOverride = 5; ++offset; }
        else if (opcode == 0xF3) { repeat = 1; ++offset; } else if (opcode == 0xF2) { repeat = 2; ++offset; }
        else if (opcode == 0x67 || opcode == 0xF0) return 0;      /* 16-bit addressing / LOCK */
        else break;
        if (offset > 4) return 0;
    }
    ++offset;
    operandSize = isOperand16 ? 2 : 4;

#define PM32_DONE(next) do { cpu->Eip += (UINT32)(next); return 1; } while (0)

    /* ---- ALU r/m,r / r,r/m / acc,imm : 00-3F except the segment/BCD holes ---- */
    if (opcode < 0x40 && (opcode & 7) < 6) {
        INT aluOperation = opcode >> 3, form = opcode & 7, width = (form & 1) ? operandSize : 1, isStore;
        UINT32 result;
        if (form == 4 || form == 5) {                        /* AL/eAX, imm */
            UINT32 second = Pm32CodeImmediate(cpu, offset, width), flags = cpu->Flags;
            result = Pm32Alu(&flags, aluOperation, Pm32GetRegister(cpu, 0, width), second, width, &isStore);
            if (isStore) Pm32SetRegister(cpu, 0, width, result);
            cpu->Flags = flags;
            PM32_DONE(offset + (UINT32)width);
        } else {
            PM32_MODRM modrm; UINT32 first, second, flags = cpu->Flags;
            Pm32Decode(cpu, offset, segmentOverride, &modrm);
            if (modrm.IsMemory && !Pm32HostCanAccess(modrm.Linear, width, form < 2 && aluOperation != 7)) return 0;
            if (form < 2) { first = Pm32ReadRm(cpu, &modrm, width); second = Pm32GetRegister(cpu, modrm.Register, width); }
            else          { first = Pm32GetRegister(cpu, modrm.Register, width); second = Pm32ReadRm(cpu, &modrm, width); }
            result = Pm32Alu(&flags, aluOperation, first, second, width, &isStore);
            if (isStore) {
                if (form < 2) { if (modrm.IsMemory) Pm32WriteMemory(modrm.Linear, width, result); else Pm32SetRegister(cpu, modrm.RegisterMemory, width, result); }
                else Pm32SetRegister(cpu, modrm.Register, width, result);
            }
            cpu->Flags = flags;
            PM32_DONE(offset + (UINT32)modrm.Length);
        }
    }
    /* ---- INC/DEC r32 (40-4F): CF preserved ---- */
    if (opcode >= 0x40 && opcode <= 0x4F) {
        INT registerIndex = opcode & 7; UINT32 carry = cpu->Flags & P32_CF, flags = cpu->Flags, result;
        result = (opcode < 0x48) ? Pm32Add(&flags, Pm32GetRegister(cpu, registerIndex, operandSize), 1, 0, operandSize) : Pm32Subtract(&flags, Pm32GetRegister(cpu, registerIndex, operandSize), 1, 0, operandSize);
        cpu->Flags = (flags & ~P32_CF) | carry;
        Pm32SetRegister(cpu, registerIndex, operandSize, result);
        PM32_DONE(offset);
    }
    /* ---- PUSH/POP r (50-5F) ---- */
    if (opcode >= 0x50 && opcode <= 0x57) {
        UINT32 value = Pm32GetRegister(cpu, opcode & 7, operandSize);
        if (!Pm32CanPush(cpu, operandSize)) return 0;
        Pm32Push(cpu, value, operandSize); PM32_DONE(offset);
    }
    if (opcode >= 0x58 && opcode <= 0x5F) {
        UINT32 value;
        if (!Pm32HostCanAccess(cpu->SegmentBases[2] + cpu->Registers[4], operandSize, 0)) return 0;
        value = Pm32Pop(cpu, operandSize);
        Pm32SetRegister(cpu, opcode & 7, operandSize, value);
        PM32_DONE(offset);
    }
    /* ---- PUSHAD / POPAD (60/61) ---- */
    if (opcode == 0x60) {
        UINT32 originalEsp = cpu->Registers[4]; INT index;
        if (isOperand16 || !Pm32HostCanAccess(cpu->SegmentBases[2] + cpu->Registers[4] - 32, 32, 1)) return 0;
        for (index = 0; index < 8; ++index) Pm32Push(cpu, index == 4 ? originalEsp : cpu->Registers[index], 4);
        PM32_DONE(offset);
    }
    if (opcode == 0x61) {
        INT index; UINT32 saved[8];
        if (isOperand16 || !Pm32HostCanAccess(cpu->SegmentBases[2] + cpu->Registers[4], 32, 0)) return 0;
        for (index = 7; index >= 0; --index) saved[index] = Pm32Pop(cpu, 4);
        for (index = 0; index < 8; ++index) if (index != 4) cpu->Registers[index] = saved[index];   /* the saved ESP is discarded */
        PM32_DONE(offset);
    }
    /* ---- PUSH imm (68/6A) ---- */
    if (opcode == 0x68 || opcode == 0x6A) {
        UINT32 value = (opcode == 0x68) ? Pm32CodeImmediate(cpu, offset, operandSize) : (UINT32)(INT32)(INT8)Pm32CodeByte(cpu, offset);
        if (!Pm32CanPush(cpu, operandSize)) return 0;
        Pm32Push(cpu, value, operandSize);
        PM32_DONE(offset + (opcode == 0x68 ? (UINT32)operandSize : 1u));
    }
    /* ---- IMUL r, r/m, imm (69/6B) ---- */
    if (opcode == 0x69 || opcode == 0x6B) {
        PM32_MODRM modrm; INT64 first, second, product; UINT32 result, next;
        Pm32Decode(cpu, offset, segmentOverride, &modrm);
        if (modrm.IsMemory && !Pm32HostCanAccess(modrm.Linear, operandSize, 0)) return 0;
        first = (operandSize == 4) ? (INT64)(INT32)Pm32ReadRm(cpu, &modrm, 4) : (INT64)(INT16)Pm32ReadRm(cpu, &modrm, 2);
        next = offset + (UINT32)modrm.Length;
        if (opcode == 0x6B) { second = (INT8)Pm32CodeByte(cpu, next); next += 1; }
        else { UINT32 immediate = Pm32CodeImmediate(cpu, next, operandSize); next += (UINT32)operandSize; second = (operandSize == 4) ? (INT64)(INT32)immediate : (INT64)(INT16)immediate; }
        product = first * second; result = (UINT32)product & Pm32Mask(operandSize);
        Pm32SetRegister(cpu, modrm.Register, operandSize, result);
        cpu->Flags &= ~(P32_CF | P32_OF);
        if (product != ((operandSize == 4) ? (INT64)(INT32)result : (INT64)(INT16)result)) cpu->Flags |= P32_CF | P32_OF;
        PM32_DONE(next);
    }
    /* ---- Jcc rel8 (70-7F) ---- */
    if (opcode >= 0x70 && opcode <= 0x7F) {
        INT32 displacement = (INT8)Pm32CodeByte(cpu, offset);
        cpu->Eip += offset + 1;
        if (Pm32Condition(cpu->Flags, opcode & 0x0F)) cpu->Eip += (UINT32)displacement;
        return 1;
    }
    /* ---- group 1 (80/81/83) ---- */
    if (opcode == 0x80 || opcode == 0x81 || opcode == 0x83) {
        INT width = (opcode == 0x80) ? 1 : operandSize, isStore; PM32_MODRM modrm; UINT32 first, second, result, next, flags = cpu->Flags;
        Pm32Decode(cpu, offset, segmentOverride, &modrm);
        next = offset + (UINT32)modrm.Length;
        if (opcode == 0x81) { second = Pm32CodeImmediate(cpu, next, width); next += (UINT32)width; }
        else { second = (UINT32)(INT32)(INT8)Pm32CodeByte(cpu, next); next += 1; }
        if (modrm.IsMemory && !Pm32HostCanAccess(modrm.Linear, width, modrm.Register != 7)) return 0;
        first = Pm32ReadRm(cpu, &modrm, width);
        result = Pm32Alu(&flags, modrm.Register, first, second, width, &isStore);
        if (isStore) { if (modrm.IsMemory) Pm32WriteMemory(modrm.Linear, width, result); else Pm32SetRegister(cpu, modrm.RegisterMemory, width, result); }
        cpu->Flags = flags;
        PM32_DONE(next);
    }
    /* ---- TEST r/m,r (84/85) ---- */
    if (opcode == 0x84 || opcode == 0x85) {
        INT width = (opcode & 1) ? operandSize : 1; PM32_MODRM modrm;
        Pm32Decode(cpu, offset, segmentOverride, &modrm);
        if (modrm.IsMemory && !Pm32HostCanAccess(modrm.Linear, width, 0)) return 0;
        Pm32Logic(&cpu->Flags, Pm32ReadRm(cpu, &modrm, width) & Pm32GetRegister(cpu, modrm.Register, width), width);
        PM32_DONE(offset + (UINT32)modrm.Length);
    }
    /* ---- XCHG r/m,r (86/87) ---- */
    if (opcode == 0x86 || opcode == 0x87) {
        INT width = (opcode & 1) ? operandSize : 1; PM32_MODRM modrm; UINT32 first, second;
        Pm32Decode(cpu, offset, segmentOverride, &modrm);
        if (modrm.IsMemory && !Pm32HostCanAccess(modrm.Linear, width, 1)) return 0;
        first = Pm32ReadRm(cpu, &modrm, width); second = Pm32GetRegister(cpu, modrm.Register, width);
        if (modrm.IsMemory) Pm32WriteMemory(modrm.Linear, width, second); else Pm32SetRegister(cpu, modrm.RegisterMemory, width, second);
        Pm32SetRegister(cpu, modrm.Register, width, first);
        PM32_DONE(offset + (UINT32)modrm.Length);
    }
    /* ---- MOV (88-8B) ---- */
    if (opcode >= 0x88 && opcode <= 0x8B) {
        INT width = (opcode & 1) ? operandSize : 1; PM32_MODRM modrm;
        Pm32Decode(cpu, offset, segmentOverride, &modrm);
        if (modrm.IsMemory && !Pm32HostCanAccess(modrm.Linear, width, opcode < 0x8A)) return 0;
        if (opcode < 0x8A) { UINT32 value = Pm32GetRegister(cpu, modrm.Register, width);
                         if (modrm.IsMemory) Pm32WriteMemory(modrm.Linear, width, value); else Pm32SetRegister(cpu, modrm.RegisterMemory, width, value); }
        else Pm32SetRegister(cpu, modrm.Register, width, Pm32ReadRm(cpu, &modrm, width));
        PM32_DONE(offset + (UINT32)modrm.Length);
    }
    /* ---- LEA (8D): the offset, not the linear address ---- */
    if (opcode == 0x8D) {
        PM32_MODRM modrm; UINT32 segmentBase;
        Pm32Decode(cpu, offset, -1, &modrm);
        if (!modrm.IsMemory) return 0;
        /* Pm32Decode added a segment base; LEA wants the effective address alone. The
           default segment for the form is DS or SS; subtract the one it used. */
        segmentBase = cpu->SegmentBases[3];
        { BYTE modrmByte = Pm32CodeByte(cpu, offset); INT mode = modrmByte >> 6, registerMemory = modrmByte & 7;
          if (registerMemory == 5 && mode != 0) segmentBase = cpu->SegmentBases[2];
          if (registerMemory == 4) { BYTE sib = Pm32CodeByte(cpu, offset + 1); INT baseRegister = sib & 7;
                         if (baseRegister == 4 || (baseRegister == 5 && mode != 0)) segmentBase = cpu->SegmentBases[2]; } }
        Pm32SetRegister(cpu, modrm.Register, operandSize, modrm.Linear - segmentBase);
        PM32_DONE(offset + (UINT32)modrm.Length);
    }
    /* ---- NOP / XCHG eAX,r (90-97) ---- */
    if (opcode == 0x90) { if (repeat) return 0; PM32_DONE(offset); }               /* F3 90 = PAUSE: decline */
    if (opcode > 0x90 && opcode <= 0x97) {
        UINT32 first = Pm32GetRegister(cpu, 0, operandSize), second = Pm32GetRegister(cpu, opcode & 7, operandSize);
        Pm32SetRegister(cpu, 0, operandSize, second); Pm32SetRegister(cpu, opcode & 7, operandSize, first); PM32_DONE(offset);
    }
    /* ---- CWDE/CBW (98), CDQ/CWD (99) ---- */
    if (opcode == 0x98) {
        if (operandSize == 4) cpu->Registers[0] = (UINT32)(INT32)(INT16)(cpu->Registers[0] & 0xFFFF);
        else Pm32SetRegister(cpu, 0, 2, (UINT32)(INT16)(INT8)(cpu->Registers[0] & 0xFF));
        PM32_DONE(offset);
    }
    if (opcode == 0x99) {
        if (operandSize == 4) cpu->Registers[2] = (cpu->Registers[0] & 0x80000000u) ? 0xFFFFFFFFu : 0;
        else Pm32SetRegister(cpu, 2, 2, (cpu->Registers[0] & 0x8000u) ? 0xFFFFu : 0);
        PM32_DONE(offset);
    }
    /* ---- MOV moffs (A0-A3) ---- */
    if (opcode >= 0xA0 && opcode <= 0xA3) {
        INT width = (opcode & 1) ? operandSize : 1; UINT32 linear = cpu->SegmentBases[segmentOverride >= 0 ? segmentOverride : 3] + Pm32CodeImmediate(cpu, offset, 4);
        if (!Pm32HostCanAccess(linear, width, opcode >= 0xA2)) return 0;
        if (opcode < 0xA2) Pm32SetRegister(cpu, 0, width, Pm32ReadMemory(linear, width)); else Pm32WriteMemory(linear, width, Pm32GetRegister(cpu, 0, width));
        PM32_DONE(offset + 4);
    }
    /* ---- TEST acc,imm (A8/A9) ---- */
    if (opcode == 0xA8 || opcode == 0xA9) {
        INT width = (opcode & 1) ? operandSize : 1;
        Pm32Logic(&cpu->Flags, Pm32GetRegister(cpu, 0, width) & Pm32CodeImmediate(cpu, offset, width), width);
        PM32_DONE(offset + (UINT32)width);
    }
    /* ---- string ops: MOVS (A4/A5), STOS (AA/AB), LODS (AC/AD), with REP ---- */
    if (opcode == 0xA4 || opcode == 0xA5 || opcode == 0xAA || opcode == 0xAB || opcode == 0xAC || opcode == 0xAD) {
        INT width = (opcode & 1) ? operandSize : 1;
        INT32 delta = (cpu->Flags & P32_DF) ? -width : width;
        UINT32 count = repeat ? cpu->Registers[1] : 1, index;
        UINT32 sourceBase = cpu->SegmentBases[segmentOverride >= 0 ? segmentOverride : 3], destinationBase = cpu->SegmentBases[0];
        if (repeat == 2) return 0;                                  /* REPNE on MOVS/STOS: odd */
        if (count > 0x10000u) return 0;                              /* bounded; the CPU can have it */
        for (index = 0; index < count; ++index) {                                /* probe the whole range first */
            UINT32 sourceOffset = cpu->Registers[6] + (UINT32)(delta * (INT32)index), destinationOffset = cpu->Registers[7] + (UINT32)(delta * (INT32)index);
            if (opcode <= 0xA5 || opcode >= 0xAC) if (!Pm32HostCanAccess(sourceBase + sourceOffset, width, 0)) return 0;
            if (opcode <= 0xAB)               if (!Pm32HostCanAccess(destinationBase + destinationOffset, width, 1)) return 0;
        }
        for (index = 0; index < count; ++index) {
            if (opcode == 0xA4 || opcode == 0xA5) Pm32WriteMemory(destinationBase + cpu->Registers[7], width, Pm32ReadMemory(sourceBase + cpu->Registers[6], width));
            else if (opcode == 0xAA || opcode == 0xAB) Pm32WriteMemory(destinationBase + cpu->Registers[7], width, Pm32GetRegister(cpu, 0, width));
            else Pm32SetRegister(cpu, 0, width, Pm32ReadMemory(sourceBase + cpu->Registers[6], width));
            if (opcode <= 0xA5 || opcode >= 0xAC) cpu->Registers[6] += (UINT32)delta;
            if (opcode <= 0xAB) cpu->Registers[7] += (UINT32)delta;
        }
        if (repeat) cpu->Registers[1] = 0;
        PM32_DONE(offset);
    }
    /* ---- MOV r,imm (B0-BF) ---- */
    if (opcode >= 0xB0 && opcode <= 0xB7) { Pm32SetRegister(cpu, opcode & 7, 1, Pm32CodeByte(cpu, offset)); PM32_DONE(offset + 1); }
    if (opcode >= 0xB8 && opcode <= 0xBF) { Pm32SetRegister(cpu, opcode & 7, operandSize, Pm32CodeImmediate(cpu, offset, operandSize)); PM32_DONE(offset + (UINT32)operandSize); }
    /* ---- shifts: C0/C1 imm8, D0/D1 by 1, D2/D3 by CL ---- */
    if (opcode == 0xC0 || opcode == 0xC1 || (opcode >= 0xD0 && opcode <= 0xD3)) {
        INT width = (opcode & 1) ? operandSize : 1; PM32_MODRM modrm; UINT32 next, count, result, flags = cpu->Flags;
        Pm32Decode(cpu, offset, segmentOverride, &modrm);
        next = offset + (UINT32)modrm.Length;
        if (opcode <= 0xC1) { count = Pm32CodeByte(cpu, next); next += 1; }
        else count = (opcode <= 0xD1) ? 1 : (cpu->Registers[1] & 0xFF);
        if (modrm.IsMemory && !Pm32HostCanAccess(modrm.Linear, width, 1)) return 0;
        if (!Pm32Shift(&flags, modrm.Register, Pm32ReadRm(cpu, &modrm, width), count, width, &result)) return 0;
        if (modrm.IsMemory) Pm32WriteMemory(modrm.Linear, width, result); else Pm32SetRegister(cpu, modrm.RegisterMemory, width, result);
        cpu->Flags = flags;
        PM32_DONE(next);
    }
    /* ---- RET / RET imm16 (C3/C2) ---- */
    if (opcode == 0xC3 || opcode == 0xC2) {
        UINT32 target, extraPop = (opcode == 0xC2) ? (Pm32CodeByte(cpu, offset) | ((UINT32)Pm32CodeByte(cpu, offset + 1) << 8)) : 0;
        if (isOperand16 || !Pm32HostCanAccess(cpu->SegmentBases[2] + cpu->Registers[4], 4, 0)) return 0;
        target = Pm32Pop(cpu, 4);
        cpu->Registers[4] += extraPop;
        cpu->Eip = target;
        return 1;
    }
    /* ---- MOV r/m,imm (C6/C7) ---- */
    if (opcode == 0xC6 || opcode == 0xC7) {
        INT width = (opcode & 1) ? operandSize : 1; PM32_MODRM modrm; UINT32 value;
        Pm32Decode(cpu, offset, segmentOverride, &modrm);
        if (modrm.Register != 0) return 0;
        value = Pm32CodeImmediate(cpu, offset + (UINT32)modrm.Length, width);
        if (modrm.IsMemory && !Pm32HostCanAccess(modrm.Linear, width, 1)) return 0;
        if (modrm.IsMemory) Pm32WriteMemory(modrm.Linear, width, value); else Pm32SetRegister(cpu, modrm.RegisterMemory, width, value);
        PM32_DONE(offset + (UINT32)modrm.Length + (UINT32)width);
    }
    /* ---- IN/OUT (E4-E7 imm8, EC-EF DX) ---- */
    if ((opcode >= 0xE4 && opcode <= 0xE7) || (opcode >= 0xEC && opcode <= 0xEF)) {
        INT width = (opcode & 1) ? operandSize : 1, isOut = (opcode & 2) != 0;
        WORD port = (opcode >= 0xEC) ? (WORD)(cpu->Registers[2] & 0xFFFF) : (WORD)Pm32CodeByte(cpu, offset);
        UINT32 next = (opcode >= 0xEC) ? offset : offset + 1;
        if (isOut) Pm32HostOut(port, width, Pm32GetRegister(cpu, 0, width));
        else Pm32SetRegister(cpu, 0, width, Pm32HostIn(port, width));
        PM32_DONE(next);
    }
    /* ---- CALL rel32 (E8), JMP rel32 (E9), JMP rel8 (EB) ---- */
    if (opcode == 0xE8) {
        UINT32 displacement = Pm32CodeImmediate(cpu, offset, 4);
        if (isOperand16 || !Pm32CanPush(cpu, 4)) return 0;
        Pm32Push(cpu, cpu->Eip + offset + 4, 4);
        cpu->Eip += offset + 4 + displacement; return 1;
    }
    if (opcode == 0xE9) { if (isOperand16) return 0; cpu->Eip += offset + 4 + Pm32CodeImmediate(cpu, offset, 4); return 1; }
    if (opcode == 0xEB) { cpu->Eip += offset + 1 + (UINT32)(INT32)(INT8)Pm32CodeByte(cpu, offset); return 1; }
    /* ---- flag ops ---- */
    if (opcode == 0xF5) { cpu->Flags ^= P32_CF; PM32_DONE(offset); }
    if (opcode == 0xF8) { cpu->Flags &= ~P32_CF; PM32_DONE(offset); }
    if (opcode == 0xF9) { cpu->Flags |= P32_CF; PM32_DONE(offset); }
    if (opcode == 0xFC) { cpu->Flags &= ~P32_DF; PM32_DONE(offset); }
    if (opcode == 0xFD) { cpu->Flags |= P32_DF; PM32_DONE(offset); }
    /* ---- group 3 (F6/F7) ---- */
    if (opcode == 0xF6 || opcode == 0xF7) {
        INT width = (opcode == 0xF6) ? 1 : operandSize; PM32_MODRM modrm; UINT32 operand, next;
        Pm32Decode(cpu, offset, segmentOverride, &modrm);
        next = offset + (UINT32)modrm.Length;
        if (modrm.IsMemory && !Pm32HostCanAccess(modrm.Linear, width, modrm.Register == 2 || modrm.Register == 3)) return 0;
        operand = Pm32ReadRm(cpu, &modrm, width);
        switch (modrm.Register) {
        case 0: case 1: Pm32Logic(&cpu->Flags, operand & Pm32CodeImmediate(cpu, next, width), width); next += (UINT32)width; break;
        case 2: { UINT32 result = ~operand & Pm32Mask(width); if (modrm.IsMemory) Pm32WriteMemory(modrm.Linear, width, result); else Pm32SetRegister(cpu, modrm.RegisterMemory, width, result); break; }
        case 3: { UINT32 flags = cpu->Flags, result = Pm32Subtract(&flags, 0, operand, 0, width);
                  if (operand & Pm32Mask(width)) flags |= P32_CF; else flags &= ~P32_CF;
                  if (modrm.IsMemory) Pm32WriteMemory(modrm.Linear, width, result); else Pm32SetRegister(cpu, modrm.RegisterMemory, width, result);
                  cpu->Flags = flags; break; }
        case 4: case 5: {                                        /* MUL / IMUL acc */
            INT isOverflow;
            if (width == 1) {
                UINT32 product = (modrm.Register == 4) ? (cpu->Registers[0] & 0xFF) * operand
                                          : (UINT32)((INT32)(INT8)cpu->Registers[0] * (INT32)(INT8)operand) & 0xFFFF;
                Pm32SetRegister(cpu, 0, 2, product);
                isOverflow = (modrm.Register == 4) ? (product >> 8) != 0 : (INT16)product != (INT8)product;
            } else if (width == 2) {
                UINT32 product = (modrm.Register == 4) ? (cpu->Registers[0] & 0xFFFF) * operand
                                          : (UINT32)((INT32)(INT16)cpu->Registers[0] * (INT32)(INT16)operand);
                Pm32SetRegister(cpu, 0, 2, product); Pm32SetRegister(cpu, 2, 2, product >> 16);
                isOverflow = (modrm.Register == 4) ? (product >> 16) != 0 : (INT32)product != (INT16)product;
            } else {
                UINT64 product = (modrm.Register == 4) ? (UINT64)cpu->Registers[0] * operand
                                          : (UINT64)((INT64)(INT32)cpu->Registers[0] * (INT64)(INT32)operand);
                cpu->Registers[0] = (UINT32)product; cpu->Registers[2] = (UINT32)(product >> 32);
                isOverflow = (modrm.Register == 4) ? (product >> 32) != 0 : (INT64)product != (INT32)product;
            }
            cpu->Flags &= ~(P32_CF | P32_OF);
            if (isOverflow) cpu->Flags |= P32_CF | P32_OF;
            break; }
        default: {                                               /* DIV / IDIV: #DE declines */
            if (!(operand & Pm32Mask(width))) return 0;
            if (width == 1) {
                UINT32 dividend = cpu->Registers[0] & 0xFFFF;
                if (modrm.Register == 6) { UINT32 quotient = dividend / operand, remainder = dividend % operand; if (quotient > 0xFF) return 0;
                                  Pm32SetRegister(cpu, 0, 2, quotient | (remainder << 8)); }
                else { INT32 signedDividend = (INT16)dividend, divisor = (INT8)operand, quotient = signedDividend / divisor, remainder = signedDividend % divisor;
                       if (quotient > 127 || quotient < -128) return 0; Pm32SetRegister(cpu, 0, 2, (quotient & 0xFF) | ((remainder & 0xFF) << 8)); }
            } else if (width == 2) {
                UINT32 dividend = ((cpu->Registers[2] & 0xFFFF) << 16) | (cpu->Registers[0] & 0xFFFF);
                if (modrm.Register == 6) { UINT32 quotient = dividend / operand, remainder = dividend % operand; if (quotient > 0xFFFF) return 0;
                                  Pm32SetRegister(cpu, 0, 2, quotient); Pm32SetRegister(cpu, 2, 2, remainder); }
                else { INT32 signedDividend = (INT32)dividend, divisor = (INT16)operand, quotient = signedDividend / divisor, remainder = signedDividend % divisor;
                       if (quotient > 32767 || quotient < -32768) return 0; Pm32SetRegister(cpu, 0, 2, (UINT32)quotient); Pm32SetRegister(cpu, 2, 2, (UINT32)remainder); }
            } else {
                UINT64 dividend = ((UINT64)cpu->Registers[2] << 32) | cpu->Registers[0];
                if (modrm.Register == 6) { UINT64 quotient = dividend / operand, remainder = dividend % operand; if (quotient > 0xFFFFFFFFu) return 0;
                                  cpu->Registers[0] = (UINT32)quotient; cpu->Registers[2] = (UINT32)remainder; }
                else { INT64 signedDividend = (INT64)dividend, divisor = (INT32)operand, quotient, remainder;
                       if (divisor == -1 && signedDividend == (INT64)0x8000000000000000ULL) return 0;
                       quotient = signedDividend / divisor; remainder = signedDividend % divisor;
                       if (quotient > 2147483647LL || quotient < -2147483648LL) return 0;
                       cpu->Registers[0] = (UINT32)quotient; cpu->Registers[2] = (UINT32)remainder; }
            }
            break; }
        }
        PM32_DONE(next);
    }
    /* ---- group 4/5 (FE/FF): INC/DEC r/m; FF: CALL/JMP near r/m, PUSH r/m ---- */
    if (opcode == 0xFE || opcode == 0xFF) {
        INT width = (opcode == 0xFE) ? 1 : operandSize; PM32_MODRM modrm;
        Pm32Decode(cpu, offset, segmentOverride, &modrm);
        if (modrm.Register <= 1) {
            UINT32 flags = cpu->Flags, carry = cpu->Flags & P32_CF, result;
            if (modrm.IsMemory && !Pm32HostCanAccess(modrm.Linear, width, 1)) return 0;
            result = (modrm.Register == 0) ? Pm32Add(&flags, Pm32ReadRm(cpu, &modrm, width), 1, 0, width) : Pm32Subtract(&flags, Pm32ReadRm(cpu, &modrm, width), 1, 0, width);
            if (modrm.IsMemory) Pm32WriteMemory(modrm.Linear, width, result); else Pm32SetRegister(cpu, modrm.RegisterMemory, width, result);
            cpu->Flags = (flags & ~P32_CF) | carry;
            PM32_DONE(offset + (UINT32)modrm.Length);
        }
        if (opcode == 0xFF && (modrm.Register == 2 || modrm.Register == 4) && !isOperand16) {
            UINT32 target;
            if (modrm.IsMemory && !Pm32HostCanAccess(modrm.Linear, 4, 0)) return 0;
            target = Pm32ReadRm(cpu, &modrm, 4);
            if (modrm.Register == 2) { if (!Pm32CanPush(cpu, 4)) return 0; Pm32Push(cpu, cpu->Eip + offset + (UINT32)modrm.Length, 4); }
            cpu->Eip = target; return 1;
        }
        if (opcode == 0xFF && modrm.Register == 6) {
            UINT32 value;
            if (modrm.IsMemory && !Pm32HostCanAccess(modrm.Linear, operandSize, 0)) return 0;
            if (!Pm32CanPush(cpu, operandSize)) return 0;
            value = Pm32ReadRm(cpu, &modrm, operandSize);
            Pm32Push(cpu, value, operandSize);
            PM32_DONE(offset + (UINT32)modrm.Length);
        }
        return 0;
    }
    /* ---- 0F xx ---- */
    if (opcode == 0x0F) {
        BYTE opcode2 = Pm32CodeByte(cpu, offset);
        ++offset;
        if (opcode2 >= 0x80 && opcode2 <= 0x8F) {                          /* Jcc rel32 */
            UINT32 displacement = Pm32CodeImmediate(cpu, offset, 4);
            if (isOperand16) return 0;
            cpu->Eip += offset + 4;
            if (Pm32Condition(cpu->Flags, opcode2 & 0x0F)) cpu->Eip += displacement;
            return 1;
        }
        if (opcode2 >= 0x90 && opcode2 <= 0x9F) {                          /* SETcc r/m8 */
            PM32_MODRM modrm; UINT32 value = (UINT32)Pm32Condition(cpu->Flags, opcode2 & 0x0F);
            Pm32Decode(cpu, offset, segmentOverride, &modrm);
            if (modrm.IsMemory && !Pm32HostCanAccess(modrm.Linear, 1, 1)) return 0;
            if (modrm.IsMemory) Pm32WriteMemory(modrm.Linear, 1, value); else Pm32SetRegister(cpu, modrm.RegisterMemory, 1, value);
            PM32_DONE(offset + (UINT32)modrm.Length);
        }
        if (opcode2 == 0xB6 || opcode2 == 0xB7 || opcode2 == 0xBE || opcode2 == 0xBF) {  /* MOVZX / MOVSX */
            INT sourceWidth = (opcode2 & 1) ? 2 : 1; PM32_MODRM modrm; UINT32 value;
            Pm32Decode(cpu, offset, segmentOverride, &modrm);
            if (modrm.IsMemory && !Pm32HostCanAccess(modrm.Linear, sourceWidth, 0)) return 0;
            value = Pm32ReadRm(cpu, &modrm, sourceWidth);
            if (opcode2 >= 0xBE) value = (sourceWidth == 1) ? (UINT32)(INT32)(INT8)value : (UINT32)(INT32)(INT16)value;
            Pm32SetRegister(cpu, modrm.Register, operandSize, value);
            PM32_DONE(offset + (UINT32)modrm.Length);
        }
        if (opcode2 == 0xAF) {                                        /* IMUL r, r/m */
            PM32_MODRM modrm; INT64 product; UINT32 result;
            Pm32Decode(cpu, offset, segmentOverride, &modrm);
            if (modrm.IsMemory && !Pm32HostCanAccess(modrm.Linear, operandSize, 0)) return 0;
            if (operandSize == 4) product = (INT64)(INT32)cpu->Registers[modrm.Register] * (INT64)(INT32)Pm32ReadRm(cpu, &modrm, 4);
            else        product = (INT64)(INT16)cpu->Registers[modrm.Register] * (INT64)(INT16)Pm32ReadRm(cpu, &modrm, 2);
            result = (UINT32)product & Pm32Mask(operandSize);
            Pm32SetRegister(cpu, modrm.Register, operandSize, result);
            cpu->Flags &= ~(P32_CF | P32_OF);
            if (product != ((operandSize == 4) ? (INT64)(INT32)result : (INT64)(INT16)result)) cpu->Flags |= P32_CF | P32_OF;
            PM32_DONE(offset + (UINT32)modrm.Length);
        }
        if (opcode2 == 0xA4 || opcode2 == 0xA5 || opcode2 == 0xAC || opcode2 == 0xAD) {  /* SHLD / SHRD */
            PM32_MODRM modrm; UINT32 next, count, destinationValue, sourceValue, result, flags, bits = (UINT32)operandSize * 8, mask = Pm32Mask(operandSize);
            Pm32Decode(cpu, offset, segmentOverride, &modrm);
            next = offset + (UINT32)modrm.Length;
            if (opcode2 == 0xA4 || opcode2 == 0xAC) { count = Pm32CodeByte(cpu, next); next += 1; } else count = cpu->Registers[1] & 0xFF;
            count &= 0x1F;
            if (modrm.IsMemory && !Pm32HostCanAccess(modrm.Linear, operandSize, 1)) return 0;
            if (!count) PM32_DONE(next);
            if (count > bits) return 0;                            /* undefined: let the CPU */
            destinationValue = Pm32ReadRm(cpu, &modrm, operandSize); sourceValue = Pm32GetRegister(cpu, modrm.Register, operandSize);
            flags = cpu->Flags;
            if (opcode2 <= 0xA5) {                                    /* SHLD */
                UINT64 concatenated = ((UINT64)destinationValue << bits) | sourceValue;
                result = (UINT32)((concatenated << count) >> bits) & mask;
                flags = (flags & ~P32_ARITH) | Pm32SzpFlags(result, operandSize);
                if ((destinationValue >> (bits - count)) & 1) flags |= P32_CF;
                if (((result ^ destinationValue) & Pm32SignBit(operandSize))) flags |= P32_OF;
            } else {                                             /* SHRD */
                UINT64 concatenated = ((UINT64)sourceValue << bits) | destinationValue;
                result = (UINT32)(concatenated >> count) & mask;
                flags = (flags & ~P32_ARITH) | Pm32SzpFlags(result, operandSize);
                if ((destinationValue >> (count - 1)) & 1) flags |= P32_CF;
                if (((result ^ destinationValue) & Pm32SignBit(operandSize))) flags |= P32_OF;
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
