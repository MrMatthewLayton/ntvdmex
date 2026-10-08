/* ntvdmex_x86.h -- the x86 architecture's fixed layout, as the host uses it (#333).
 *
 * One name per meaning, defined once (docs/STYLE.md, section 3). Included by
 * ntvdmex_types.h, so every file that has the base types has these too.
 */
#ifndef NTVDMEX_X86_H
#define NTVDMEX_X86_H

/* Real-mode addressing: linear = segment * 16 + offset. */
#define PARAGRAPH_SHIFT     4
#define PARAGRAPH_SIZE      16
#define PARAGRAPH_SIZE_U    16u
#define PARAGRAPH_LAST_BYTE 15              /* added before dividing, to round up to a paragraph */
#define PARAGRAPH_LAST_BYTE_U 15u

/* Paging: a 4 KB page. */
#define PAGE_SHIFT          12

/* The real-mode interrupt vector table: at 0000:0000, one 4-byte entry per vector -- the
   handler's offset word, then its segment word. */
#define IVT_BASE_SEGMENT    0
#define IVT_ENTRY_SIZE      4
#define IVT_ENTRY_SIZE_U    4u
#define IVT_SEGMENT_OFFSET  2
/* The linear addresses of vector `vector`'s offset word and segment word. */
#define IVT_OFFSET_ADDRESS(vector)   ((vector) * IVT_ENTRY_SIZE)
#define IVT_SEGMENT_ADDRESS(vector)  ((vector) * IVT_ENTRY_SIZE + IVT_SEGMENT_OFFSET)

/* EFLAGS. The plain names are `int` literals, the _U names `unsigned` (see ntvdmex_bits.h). */
#define EFLAGS_CF           0x0001
#define EFLAGS_CF_U         0x0001u
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
