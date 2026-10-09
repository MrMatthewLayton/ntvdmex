/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The 32-bit protected-mode interpreter's CPU state (PM32_CPU) and its encoding constants.
 *
 * Declarations only (#335): split out of pm32interp.h so the host files that hold a
 * CPU state or read its constants need not include the interpreter itself.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef PM32CPU_H
#define PM32CPU_H

#include "../ntvdmex_types.h"
#include "../ntvdmex_x86.h"

#define PM32_ARITH                      (EFLAGS_CF_U | EFLAGS_PF_U | EFLAGS_AF_U | EFLAGS_ZF_U | EFLAGS_SF_U | EFLAGS_OF_U)

/* Operand widths, masks and signs. */
#define PM32_PUSHAD_BYTES               32      /* Eight DWORD registers */

/* Flags. */
#define PM32_AUXILIARY_BIT              0x10    /* The carry out of bit 3 */

/* ModRM and SIB. */
#define PM32_MODE_DISP_FULL             2
#define PM32_RM_SIB                     4
#define PM32_RM_DISP32                  5
#define PM32_SIB_NO_INDEX               4
#define PM32_MODRM_SIB_LENGTH           2
#define PM32_MODRM_DISP32_LENGTH        5

/* The x86 encoding facts both interpreters share are in ntvdmex_x86.h (X86_*). */
/* The ALU group (00..3F, and 80/81/83's /r): its operations and forms. */
#define PM32_ALU_FORMS                  6       /* Forms 6/7 are other instructions */
#define PM32_FORM_FROM_REGISTER_END     2       /* Forms 0/1: r/m op= reg */
#define PM32_FORM_ACCUMULATOR_BYTE      4       /* AL op= imm8 */
#define PM32_FORM_ACCUMULATOR           5       /* eAX op= imm */

/* Condition codes: pairs, the low bit negates. */
#define PM32_CC_PAIR_SHIFT              1
#define PM32_CC_OVERFLOW                0
#define PM32_CC_BELOW                   1
#define PM32_CC_ZERO                    2
#define PM32_CC_BELOW_OR_EQUAL          3
#define PM32_CC_SIGN                    4
#define PM32_CC_PARITY                  5
#define PM32_CC_LESS                    6

/* Group 3. */
#define PM32_GROUP3_TEST                0
#define PM32_GROUP3_TEST_ALIAS          1
#define PM32_OUT_BIT                    2       /* E4-E7/EC-EF: bit 1 = OUT */
#define PM32_MAX_STRING_COUNT           0x10000u

/* Opcodes. */
#define PM32_OP2_SHLD_IMM               0xA4    /* After X86_ESCAPE */
#define PM32_OP2_SHLD_CL                0xA5    /* After X86_ESCAPE */
#define PM32_OP2_SHRD_IMM               0xAC    /* After X86_ESCAPE */
#define PM32_OP2_SHRD_CL                0xAD    /* After X86_ESCAPE */
#define PM32_OP2_IMUL                   0xAF    /* After X86_ESCAPE */

typedef struct _PM32_CPU
{
    UINT32 Registers[X86_GENERAL_REGISTERS];        /* EAX ECX EDX EBX ESP EBP ESI EDI */
    UINT32 Eip;         /* offset in CS */
    UINT32 Flags;
    UINT32 SegmentBases[X86_SEGMENT_REGISTERS];     /* segment BASES, x86 sreg order: ES CS SS DS FS GS */
} PM32_CPU, *PPM32_CPU; typedef const PM32_CPU *PCPM32_CPU;

#endif
