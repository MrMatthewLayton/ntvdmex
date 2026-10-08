/* ntvdmex_x86.h -- the x86 architecture's fixed layout, as the host uses it (#333).
 *
 * One name per meaning, defined once (docs/STYLE.md, section 3). Included by
 * ntvdmex_types.h, so every file that has the base types has these too.
 */
#ifndef NTVDMEX_X86_H
#define NTVDMEX_X86_H

/* Register numbers, as the instruction encoding has them: (E)AX..(E)DI, and ES..GS. */
#define X86_REG_AX              0
#define X86_REG_CX              1
#define X86_REG_DX              2
#define X86_REG_BX              3
#define X86_REG_SP              4
#define X86_REG_BP              5
#define X86_REG_SI              6
#define X86_REG_DI              7
#define X86_GENERAL_REGISTERS   8
#define X86_SREG_ES             0
#define X86_SREG_CS             1
#define X86_SREG_SS             2
#define X86_SREG_DS             3
#define X86_SREG_FS             4
#define X86_SREG_GS             5
#define X86_SEGMENT_REGISTERS   6

/* Operand and structure sizes, in bytes. */
#define X86_WORD_SIZE       2
#define X86_DWORD_SIZE      4
#define X86_DESCRIPTOR_SIZE 8               /* a GDT/LDT/IDT entry                      */
/* A 16-bit far frame on the stack: IP, then CS, then (an interrupt's) FLAGS. */
#define X86_FRAME16_CS          2
#define X86_FRAME16_FLAGS       4
#define X86_FAR_RETURN16_SIZE   4           /* IP, CS                                   */
#define X86_IRET16_SIZE         6           /* IP, CS, FLAGS                            */

/* Exceptions: 32 reserved vectors; #GP is 13. An error code's bit 1 says the selector is an
   IDT entry. INT n is two bytes, CD nn. */
#define X86_EXCEPTIONS          32
#define X86_EXCEPTION_GP        13
#define X86_ERROR_CODE_IDT      0x2
#define X86_INT_LENGTH          2

/* Real-mode addressing: linear = segment * 16 + offset. */
#define PARAGRAPH_SHIFT     4
#define PARAGRAPH_SIZE      16
#define PARAGRAPH_SIZE_U    16u
#define PARAGRAPH_LAST_BYTE 15              /* added before dividing, to round up to a paragraph */
#define PARAGRAPH_LAST_BYTE_U 15u

/* Paging: a 4 KB page. */
#define PAGE_SHIFT          12

/* A segment spans at most 64 KB; a far pointer is offset then segment. */
#define X86_SEGMENT_SIZE_U      0x10000u
#define X86_SEGMENT_LIMIT_64K   0xFFFF
#define X86_FAR_POINTER_SEGMENT 2
#define X86_SELECTOR_NULL_MASK  0xFFFC      /* a selector is null when these bits are   */

/* The real-mode interrupt vector table: at 0000:0000, one 4-byte entry per vector -- the
   handler's offset word, then its segment word. */
#define IVT_BASE_SEGMENT    0
#define PAGE_LAST_BYTE_U    0xFFFu          /* added before masking, to round up a page */
#define IVT_SIZE            0x400           /* 256 vectors x 4 bytes                    */
#define IVT_VECTORS         256
#define IVT_ENTRY_SIZE      4
#define IVT_ENTRY_SIZE_U    4u
#define IVT_SEGMENT_OFFSET  2
/* The linear addresses of vector `vector`'s offset word and segment word. */
#define IVT_OFFSET_ADDRESS(vector)   ((vector) * IVT_ENTRY_SIZE)
#define IVT_SEGMENT_ADDRESS(vector)  ((vector) * IVT_ENTRY_SIZE + IVT_SEGMENT_OFFSET)

/* x86 opcode bytes, as the interpreters and the instruction-length decoder use them. */
#define X86_PREFIX_ES           0x26
#define X86_PREFIX_CS           0x2E
#define X86_PREFIX_SS           0x36
#define X86_PREFIX_DS           0x3E
#define X86_PREFIX_FS           0x64
#define X86_PREFIX_GS           0x65
#define X86_PREFIX_OPERAND_SIZE 0x66
#define X86_PREFIX_ADDRESS_SIZE 0x67
#define X86_PREFIX_LOCK         0xF0
#define X86_PREFIX_REPNE        0xF2
#define X86_PREFIX_REP          0xF3
#define X86_ESCAPE              0x0F    /* two-byte opcodes                      */
#define X86_ESCAPE_38           0x38    /* three-byte opcodes                    */
#define X86_ESCAPE_3A           0x3A    /* ...with an imm8                       */
#define X86_JCC_NEAR_FIRST      0x80    /* 0F 80..8F: jcc rel16/32               */
#define X86_JCC_NEAR_LAST       0x8F
#define X86_OP_PUSH_ES                 0x06
#define X86_OP_POP_ES                  0x07
#define X86_OP_PUSH_CS                 0x0E
#define X86_OP_PUSH_SS                 0x16
#define X86_OP_POP_SS                  0x17
#define X86_OP_PUSH_DS                 0x1E
#define X86_OP_POP_DS                  0x1F
#define X86_OP_INC_FIRST               0x40
#define X86_OP_DEC_FIRST               0x48
#define X86_OP_DEC_LAST                0x4F
#define X86_OP_PUSH_FIRST              0x50
#define X86_OP_PUSH_LAST               0x57
#define X86_OP_POP_FIRST               0x58
#define X86_OP_POP_LAST                0x5F
#define X86_OP_PUSHA                   0x60
#define X86_OP_PUSHAD                  0x60
#define X86_OP_POPA                    0x61
#define X86_OP_POPAD                   0x61
#define X86_OP_PUSH_IMM                0x68
#define X86_OP_IMUL_IMM                0x69
#define X86_OP_PUSH_IMM8               0x6A
#define X86_OP_IMUL_IMM8               0x6B
#define X86_OP_JCC_SHORT_FIRST         0x70
#define X86_OP_JCC_SHORT_LAST          0x7F
#define X86_OP_GROUP1_IMM8             0x80
#define X86_OP_GROUP1_IMM              0x81
#define X86_OP_GROUP1_SIMM8            0x83
#define X86_OP_TEST_BYTE               0x84
#define X86_OP_TEST                    0x85
#define X86_OP_XCHG_BYTE               0x86
#define X86_OP_XCHG                    0x87
#define X86_OP_MOV_TO_RM_BYTE          0x88
#define X86_OP_MOV_TO_RM               0x89
#define X86_OP_MOV_FROM_RM_BYTE        0x8A
#define X86_OP_MOV_FROM_RM             0x8B
#define X86_OP_MOV_FROM_SREG           0x8C
#define X86_OP_LEA                     0x8D
#define X86_OP_MOV_TO_SREG             0x8E
#define X86_OP_POP_RM                  0x8F
#define X86_OP_NOP                     0x90
#define X86_OP_XCHG_FIRST              0x91
#define X86_OP_XCHG_LAST               0x97
#define X86_OP_CBW                     0x98
#define X86_OP_CWDE                    0x98
#define X86_OP_CWD                     0x99
#define X86_OP_CDQ                     0x99
#define X86_OP_CALL_FAR                0x9A
#define X86_OP_WAIT                    0x9B
#define X86_OP_PUSHF                   0x9C
#define X86_OP_POPF                    0x9D
#define X86_OP_SAHF                    0x9E
#define X86_OP_LAHF                    0x9F
#define X86_OP_MOV_FROM_MOFFS_BYTE     0xA0
#define X86_OP_MOV_FROM_MOFFS          0xA1
#define X86_OP_MOV_TO_MOFFS_BYTE       0xA2
#define X86_OP_MOV_TO_MOFFS            0xA3
#define X86_OP_MOVSB                   0xA4
#define X86_OP_MOVS                    0xA5
#define X86_OP_CMPSB                   0xA6
#define X86_OP_CMPS                    0xA7
#define X86_OP_TEST_IMM_BYTE           0xA8
#define X86_OP_TEST_IMM                0xA9
#define X86_OP_STOSB                   0xAA
#define X86_OP_STOS                    0xAB
#define X86_OP_LODSB                   0xAC
#define X86_OP_LODS                    0xAD
#define X86_OP_SCASB                   0xAE
#define X86_OP_SCAS                    0xAF
#define X86_OP_MOV_IMM_BYTE_FIRST      0xB0
#define X86_OP_MOV_IMM_BYTE_LAST       0xB7
#define X86_OP_MOV_IMM_FIRST           0xB8
#define X86_OP_MOV_IMM_LAST            0xBF
#define X86_OP_SHIFT_IMM_BYTE          0xC0
#define X86_OP_SHIFT_IMM               0xC1
#define X86_OP_RET_IMM                 0xC2
#define X86_OP_RET                     0xC3
#define X86_OP_LES                     0xC4
#define X86_OP_LDS                     0xC5
#define X86_OP_MOV_IMM_RM_BYTE         0xC6
#define X86_OP_MOV_IMM_RM              0xC7
#define X86_OP_ENTER                   0xC8
#define X86_OP_LEAVE                   0xC9
#define X86_OP_RETF_IMM                0xCA
#define X86_OP_RETF                    0xCB
#define X86_OP_INT3                    0xCC
#define X86_OP_INT                     0xCD
#define X86_OP_IRET                    0xCF
#define X86_OP_SHIFT_ONE_BYTE          0xD0
#define X86_OP_SHIFT_ONE               0xD1
#define X86_OP_SHIFT_CL_BYTE           0xD2
#define X86_OP_SHIFT_CL                0xD3
#define X86_OP_XLAT                    0xD7
#define X86_OP_LOOPNE                  0xE0
#define X86_OP_LOOPE                   0xE1
#define X86_OP_LOOP                    0xE2
#define X86_OP_JCXZ                    0xE3
#define X86_OP_IN_IMM_BYTE             0xE4
#define X86_OP_IN_IMM                  0xE5
#define X86_OP_OUT_IMM_BYTE            0xE6
#define X86_OP_OUT_IMM                 0xE7
#define X86_OP_CALL                    0xE8
#define X86_OP_JMP                     0xE9
#define X86_OP_JMP_FAR                 0xEA
#define X86_OP_JMP_SHORT               0xEB
#define X86_OP_IN_DX_BYTE              0xEC
#define X86_OP_IN_DX                   0xED
#define X86_OP_OUT_DX_BYTE             0xEE
#define X86_OP_OUT_DX                  0xEF
#define X86_OP_CMC                     0xF5
#define X86_OP_GROUP3_BYTE             0xF6
#define X86_OP_GROUP3                  0xF7
#define X86_OP_CLC                     0xF8
#define X86_OP_STC                     0xF9
#define X86_OP_CLI                     0xFA
#define X86_OP_STI                     0xFB
#define X86_OP_CLD                     0xFC
#define X86_OP_STD                     0xFD
#define X86_OP_GROUP4                  0xFE
#define X86_OP_GROUP5                  0xFF

#define X86_OP_AND_AL_IMM              0x24
#define X86_OP_JZ_SHORT                0x74
#define X86_OP_JNZ_SHORT               0x75
#define X86_OP_MOV_AH_IMM              0xB4
#define X86_OP_CMP_AX_IMM              0x3D
/* BOP (BIOS Operation): the 3-byte sequence C4 C4 nn is an invalid opcode the
   kernel reflects back to the host as a VDM event, carrying the byte nn. The host
   advances EIP past the 3 bytes and re-enters; a trailing IRET (CF) resumes the
   guest. Real-mode INT 21h is vectored through the IVT to a handler that runs a
   BOP, so every INT 21h surfaces to the host. */
#define VDM_BOP0 0xC4
#define VDM_BOP1 0xC4
#define VDM_BOP_LENGTH              3   /* C4 C4 nn                                 */
#define VDM_BOP_SUBFUNCTION_LENGTH  4   /* C4 C4 nn sub                             */
#define VDM_BOP_NUMBER_OFFSET       2   /* nn: the byte after C4 C4                 */

/* EFLAGS. The plain names are `int` literals, the _U names `unsigned` (see ntvdmex_bits.h). */
#define EFLAGS_CF           0x0001
#define EFLAGS_CF_U         0x0001u
#define EFLAGS_RESERVED_ONE 0x0002
#define EFLAGS_RESERVED_ONE_U 0x0002u       /* bit 1 always reads 1                     */
#define EFLAGS_PF_U         0x0004u
#define EFLAGS_AF_U         0x0010u
#define EFLAGS_ZF           0x0040
#define EFLAGS_ZF_U         0x0040u
#define EFLAGS_SF_U         0x0080u
#define EFLAGS_TF_U         0x0100u
#define EFLAGS_IF           0x0200
#define EFLAGS_IF_U         0x0200u
#define EFLAGS_DF           0x0400
#define EFLAGS_DF_U         0x0400u
#define EFLAGS_OF_U         0x0800u
#define EFLAGS_STATUS_DF_U  0x0CD5u         /* CF PF AF ZF SF DF OF                     */
#define EFLAGS_RF_U         0x00010000u
#define EFLAGS_VM           0x20000         /* bit 17: set = V86, clear = PM            */
#define EFLAGS_VM_U         0x00020000u
/* EFLAGS.VIF (bit 19) -- the VIRTUAL interrupt flag. On a VME-capable CPU (every box
   we target) the kernel's "can I deliver a hardware interrupt to this VDM right now?"
   test reads VIF, NOT IF: with VME on, a V86 frame counts as interruptible only
   when EFlags & EFLAGS_VIF is set. A guest started with IF=1 but VIF=0 therefore looks to the
   kernel like interrupts are disabled forever, so its interrupt-assist never delivers
   and it just sets VIP (bit 20) and defers -- which is exactly what the rig showed. */
#define EFLAGS_VIF          0x80000
#define EFLAGS_VIF_U        0x00080000u
#define EFLAGS_VIP          0x100000        /* bit 20: a virtual interrupt is pending   */

/* The PC's interrupt vectors. The PICs as the BIOS programs them: IRQ 0-7 -> 08h-0Fh,
   IRQ 8-15 -> 70h-77h. */
#define PIC_LINES_PER_CHIP          8
#define PIC_MASTER_VECTOR_BASE      0x08
#define PIC_SLAVE_VECTOR_BASE       0x70
#define VECTOR_IRQ0                 0x08
#define VECTOR_IRQ1                 0x09
#define VECTOR_IRQ2                 0x0A
#define VECTOR_IRQ3                 0x0B
#define VECTOR_IRQ5                 0x0D
#define VECTOR_IRQ7                 0x0F
#define VECTOR_IRQ8                 0x70
#define VECTOR_IRQ15                0x77

#define VECTOR_NMI                  0x02
#define VECTOR_PRINT_SCREEN         0x05
#define VECTOR_TIMER                0x08    /* IRQ 0                                    */
#define VECTOR_KEYBOARD             0x09    /* IRQ 1                                    */
#define VECTOR_VIDEO                0x10    /* video BIOS                               */
#define VECTOR_EQUIPMENT            0x11    /* BIOS equipment list                      */
#define VECTOR_SERIAL               0x14    /* BIOS serial ports                        */
#define VECTOR_SYSTEM               0x15    /* BIOS miscellaneous/system services       */
#define VECTOR_KEYBOARD_SERVICES    0x16    /* keyboard BIOS                            */
#define VECTOR_MEMORY_SIZE          0x12    /* BIOS base memory size                   */
#define VECTOR_DISK                 0x13    /* BIOS disk services                      */
#define VECTOR_PRINTER              0x17    /* BIOS printer                            */
#define VECTOR_ABSOLUTE_DISK_READ   0x25
#define VECTOR_ABSOLUTE_DISK_WRITE  0x26
#define VECTOR_TERMINATE_RESIDENT   0x27    /* terminate and stay resident             */
#define VECTOR_DOS_IDLE             0x28
#define VECTOR_FAST_CONSOLE_OUTPUT  0x29
#define VECTOR_TIME                 0x1A    /* BIOS time                                */
#define VECTOR_USER_TICK            0x1C    /* called from the timer tick               */
#define VECTOR_TERMINATE            0x20    /* program terminate                        */
#define VECTOR_DOS                  0x21
#define VECTOR_TERMINATE_ADDRESS    0x22
#define VECTOR_CTRL_C               0x23
#define VECTOR_CRITICAL_ERROR       0x24
#define VECTOR_NETWORK              0x2A    /* the network/critical-section interface   */
#define VECTOR_MULTIPLEX            0x2F
#define VECTOR_DPMI                 0x31
#define VECTOR_MOUSE                0x33
#define VECTOR_FLOATING_POINT_FIRST 0x34    /* the floating-point emulator's vectors    */
#define VECTOR_FLOATING_POINT_LAST  0x3F
#define VECTOR_KERNEL_DEBUGGER      0x41    /* the Windows kernel debugger              */
#define VECTOR_NETBIOS              0x5C
#define VECTOR_EMS                  0x67
/* The vectors no DOS, BIOS or IRQ owns: the user vectors. */
#define VECTOR_USER_RANGE1_FIRST    0x60
#define VECTOR_USER_RANGE1_LAST     0x66
#define VECTOR_USER_RANGE2_FIRST    0x68
#define VECTOR_USER_RANGE2_LAST     0x6F
#define VECTOR_USER_RANGE3_FIRST    0x78
#define VECTOR_USER_RANGE3_LAST     0xFE

#endif /* NTVDMEX_X86_H */
