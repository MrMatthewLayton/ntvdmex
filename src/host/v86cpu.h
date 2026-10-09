/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The 8086 interpreter's CPU state (V86_CPU) and its encoding constants.
 *
 * Declarations only (#335): split out of v86interp.h so the host files that hold a
 * CPU state or read its constants need not include the interpreter itself.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef V86CPU_H
#define V86CPU_H

#include "../ntvdmex_types.h"
#include "../ntvdmex_x86.h"

#define V86_GUEST_LIMIT             0x110000u   /* 1MB + HMA: guest linear range */
#define V86_FLAGS_MODELLED          0x0ED5u     /* CF PF AF ZF SF TF IF DF OF: the flags kept */
#define V86_LAHF_FLAGS              0xD5u       /* SF ZF AF PF CF: what LAHF/SAHF move */

/* The x86 encoding facts both interpreters share -- ModRM fields, opcode extensions, sign
 * bits, register counts -- are in ntvdmex_x86.h (X86_*). What follows is this one's own.
 */
/* ModRM (16-bit addressing). */
#define V86_MODE_DISP16             2
#define V86_THIRD_BYTE              2           /* Byte offsets of a dword's upper half */
#define V86_FOURTH_BYTE             3
#define V86_PARITY_TABLE            0x6996u
#define V86_OFFSET_MAX              0xFFFFu     /* The last offset of a 64 KB segment */
#define V86_INT32_MAX               2147483647LL
#define V86_INT32_MIN               (-2147483648LL)

/* A descriptor's access rights, as LAR returns them, and a selector's RPL. */
#define V86_ACCESS_PRESENT          0x8000u
#define V86_ACCESS_CODE             0x0800u
#define V86_ACCESS_DEFAULT_BIG      (1u << 22)
#define V86_SELECTOR_RPL_MASK       3

/* The 16-bit ModRM r/m field (mode 0..2), and the mode with no displacement. */
#define V86_MODE_NO_DISP            0
#define V86_RM_BX_SI                0
#define V86_RM_BX_DI                1
#define V86_RM_BP_SI                2
#define V86_RM_BP_DI                3
#define V86_RM_SI                   4
#define V86_RM_DI                   5
#define V86_RM_BP                   6           /* Mode 0: a 16-bit displacement alone */

/* ALU operations (the reg field of 80-83, bits 3-5 of 00-3D) and their six forms. */
#define V86_ALU_FORM_RM_BYTE        0           /* R/m8, r8 */
#define V86_ALU_FORM_RM             1           /* R/m, r */
#define V86_ALU_FORM_REG_BYTE       2           /* r8, r/m8 */
#define V86_ALU_FORM_REG            3           /* R, r/m */
#define V86_ALU_FORM_AL_IMM         4           /* AL, imm8 */
#define V86_ALU_FORM_COUNT          6

/* Condition codes (the low nibble of Jcc/SETcc). */
#define V86_CC_OVERFLOW             0x0
#define V86_CC_NOT_OVERFLOW         0x1
#define V86_CC_BELOW                0x2
#define V86_CC_NOT_BELOW            0x3
#define V86_CC_ZERO                 0x4
#define V86_CC_NOT_ZERO             0x5
#define V86_CC_BELOW_OR_EQUAL       0x6
#define V86_CC_ABOVE                0x7
#define V86_CC_SIGN                 0x8
#define V86_CC_NOT_SIGN             0x9
#define V86_CC_PARITY               0xA
#define V86_CC_NOT_PARITY           0xB
#define V86_CC_LESS                 0xC
#define V86_CC_NOT_LESS             0xD
#define V86_CC_LESS_OR_EQUAL        0xE

/* Group 3 (F6/F7) and group 5 (FF) operations, by reg field. */
#define V86_GROUP5_CALL_FAR         3
#define V86_GROUP5_JMP_FAR          5

/* Stack frames: a far return address, and an interrupt's (that plus FLAGS). */
#define V86_FAR_FRAME16             4           /* IP, CS */
#define V86_FAR_FRAME32             8           /* EIP, CS (a dword) */
#define V86_IRET_FRAME16            6           /* IP, CS, FLAGS */
#define V86_IRET_FRAME32            12          /* EIP, CS, EFLAGS */
#define V86_FAR_POINTER32           6           /* ptr16:32: offset dword, then the segment */
#define V86_INT3_VECTOR             3
#define V86_ENTER_LEVEL_MASK        0x1F
#define V86_ENTER_OPERAND_LENGTH    3           /* imm16 frame size, imm8 nesting level */

/* Opcodes. */
#define V86_OP2_LAR                 0x02        /* After X86_ESCAPE */
#define V86_OP2_LSL                 0x03        /* After X86_ESCAPE */
#define V86_OP2_PUSH_FS             0xA0        /* After X86_ESCAPE */
#define V86_OP2_POP_FS              0xA1        /* After X86_ESCAPE */
#define V86_OP2_PUSH_GS             0xA8        /* After X86_ESCAPE */
#define V86_OP2_POP_GS              0xA9        /* After X86_ESCAPE */

typedef struct _V86_CPU
{
    UINT32 Registers[X86_GENERAL_REGISTERS];      /* E-AX/CX/DX/BX/SP/BP/SI/DI (full 32-bit; 16-/8-bit views mask) */
    WORD Segments[X86_SEGMENT_REGISTERS];    /* ES CS SS DS FS GS       (x86 sreg encoding) */
    WORD Ip;        /* address size stays 16-bit (the 0x67 prefix still bails) */
    UINT32 Flags;
} V86_CPU, *PV86_CPU; typedef const V86_CPU *PCV86_CPU;

#endif
