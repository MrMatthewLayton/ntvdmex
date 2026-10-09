/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Off-VM battery for the BIOS INT 09h side-calls (GH #254).
 *
 * src/dos/bios_kbdact.h is guest code the V86 INT 09h arm resumes the guest in for
 * Ctrl-Break (INT 1Bh), Print Screen (INT 05h), SysReq (INT 15h AX=8500h/8501h) and
 * Pause (spin until 0040:0018 bit 3 clears). Run here in v86interp with the vectors
 * pointed at recorders, from a stack holding the INT 09h frame.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>     /* v86interp.h is written with the fixed-width types */
#include "ntvdmex_types.h"

/* The guest's memory and the interpreter's callbacks (names fixed by v86interp.h). */
#define KBDACT_TEST_MEMORY_SIZE     0x110000
#define KBDACT_TEST_FLOATING_BUS    0xFF
#define KBDACT_TEST_NONE            (-1)

static BYTE g_GuestMemory[KBDACT_TEST_MEMORY_SIZE];
static BYTE V86HostRead8(DWORD linear)
{
    return (linear < sizeof g_GuestMemory) ? g_GuestMemory[linear] : 0;
}

static VOID V86HostWrite8(DWORD linear, BYTE value)
{
    if (linear < sizeof g_GuestMemory)
        g_GuestMemory[linear] = value;
}

static DWORD V86HostIn(WORD port, INT width)
{
    (VOID)port;
    (VOID)width;
    return KBDACT_TEST_FLOATING_BUS;
}

static INT g_LastOutPort = KBDACT_TEST_NONE, g_LastOutValue = KBDACT_TEST_NONE;
static VOID V86HostOut(WORD port, INT width, DWORD value)
{
    (VOID)width;
    g_LastOutPort = port;
    g_LastOutValue = (INT)value;
}

#include "../../src/host/v86interp.h"
#include "dos_layout.h"
#include "bios_kbdact.h"

static INT g_Checks = 0, g_Failures = 0;

static VOID KeyboardActionTestCheck(BOOL passed, PCSTR message)
{
    g_Checks++;
    if (passed)
    {
        printf("  PASS  %s\n", (message));
    }
    else
    {
        printf("  FAIL  %s\n", (message));
        g_Failures++;
    }
}

/* The handler for a vector sits at KBDACT_TEST_HANDLER_SEGMENT:vector*2 = HLT, IRET. */
#define KBDACT_TEST_HANDLER_SEGMENT     0x2000
#define KBDACT_TEST_RETURN_SEGMENT      0x3000
#define KBDACT_TEST_STACK_SEGMENT       0x5000
#define KBDACT_TEST_HANDLER_STRIDE      2
#define KBDACT_TEST_VECTOR_COUNT        256
#define KBDACT_TEST_IVT_ENTRY_SIZE      4
#define KBDACT_TEST_IVT_SEGMENT         2       /* The segment word in an IVT entry */
#define KBDACT_TEST_IVT_SEGMENT_HIGH    3
#define KBDACT_TEST_HLT                 0xF4
#define KBDACT_TEST_IRET                0xCF
#define KBDACT_TEST_BOP_BYTE            0xC4    /* C4 C4 09: BOP 09h */
#define KBDACT_TEST_BOP_09H             0x09
#define KBDACT_TEST_BOP_NUMBER_BYTE     2
#define KBDACT_TEST_BOP_SIZE            3

/* The stack: an INT 09h frame (IP, CS, FLAGS) below the top, returning to
 * KBDACT_TEST_RETURN_SEGMENT:0000.
 */
#define KBDACT_TEST_STACK_TOP           0xFFF0
#define KBDACT_TEST_FRAME_SIZE          6
#define KBDACT_TEST_FRAME_CS            0xFFEC
#define KBDACT_TEST_FRAME_CS_HIGH       0xFFED
#define KBDACT_TEST_FRAME_FLAGS         0xFFEE
#define KBDACT_TEST_CALLER_FLAGS        0x02    /* Caller FLAGS: IF=0 */
#define KBDACT_TEST_FLAGS_POP_OFFSET    4       /* The IRET pops FLAGS at SS:SP+4 */
#define KBDACT_TEST_WORD_SIZE           2

/* icpu's register and segment slots. */
#define KBDACT_TEST_AX                  0
#define KBDACT_TEST_DX                  2
#define KBDACT_TEST_SP                  4
#define KBDACT_TEST_CS                  1
#define KBDACT_TEST_SS                  2
#define KBDACT_TEST_DS                  3

#define KBDACT_TEST_FLAG_CF             0x0001u
#define KBDACT_TEST_FLAG_IF             0x200
#define KBDACT_TEST_INITIAL_FLAGS       0x0002
#define KBDACT_TEST_SAVED_AX            0x1234
#define KBDACT_TEST_SAVED_AX_LOW        0x34
#define KBDACT_TEST_SAVED_AX_HIGH       0x12
#define KBDACT_TEST_SAVED_DS            0x7777
#define KBDACT_TEST_AL_MASK             0xFF

/* The pause loop's flag: 0040:0018 bit 3. */
#define KBDACT_TEST_PAUSE_FLAGS_LINEAR  0x418
#define KBDACT_TEST_PAUSE_BIT           0x08

/* The vectors and AX values the routines call with. */
#define KBDACT_TEST_INT_05H             0x05
#define KBDACT_TEST_INT_15H             0x15
#define KBDACT_TEST_INT_17H             0x17
#define KBDACT_TEST_INT_1BH             0x1B
#define KBDACT_TEST_SYSREQ_DOWN_AX      0x8500
#define KBDACT_TEST_SYSREQ_UP_AX        0x8501
#define KBDACT_TEST_INTERCEPT_AX        0x4F1E  /* AH=4Fh, AL = scancode 1Eh */
#define KBDACT_TEST_SCANCODE            0x1E
#define KBDACT_TEST_PRINTED_AX          0x0058  /* AH=00h, AL='X' */
#define KBDACT_TEST_EOI_PORT            0x20
#define KBDACT_TEST_EOI                 0x20

/* The end of the block the code shares with the AUX/PRN code. */
#define KBDACT_TEST_BLOCK_END           0x6F0

/* Step budgets, and the step at which a pause test clears the bit. */
#define KBDACT_TEST_BUDGET              1000
#define KBDACT_TEST_PAUSE_BUDGET        5000
#define KBDACT_TEST_CLEAR_PAUSE_AT      300
#define KBDACT_TEST_NEVER_CLEAR         0

/* What KeyboardActionTestRun returns. */
#define KBDACT_TEST_RETURNED            0       /* Back at KBDACT_TEST_RETURN_SEGMENT:0000 */
#define KBDACT_TEST_STOPPED             1       /* At g_StopOffset */
#define KBDACT_TEST_OUT_OF_BUDGET       (-1)
#define KBDACT_TEST_LOST                (-2)

static INT g_LastVector; static WORD g_LastAx; static INT g_CallCount;
static INT g_InterruptFlagAtCall;
/* #244: this vector's handler returns CF=0 (a swallow) */
static INT g_ClearCarryVector = KBDACT_TEST_NONE;
/* #244/#274: stop at this DOS_CTAB_SEG offset (a BOP site) */
static INT g_StopOffset = KBDACT_TEST_NONE;

static V86_CPU KeyboardActionTestSetup(UINT entry)
{
    V86_CPU cpu;
    INT vector;

    memset(&cpu, 0, sizeof cpu);
    memset(g_GuestMemory, 0, sizeof g_GuestMemory);
    memcpy(g_GuestMemory + ((DWORD)DOS_CTAB_SEG << PARAGRAPH_SHIFT) + DOS_KBDACT_OFF,
           g_BiosKeyboardActionCode, sizeof g_BiosKeyboardActionCode);
    for (vector = 0; vector < KBDACT_TEST_VECTOR_COUNT; ++vector)
    {
        g_GuestMemory[vector * KBDACT_TEST_IVT_ENTRY_SIZE] =
            (BYTE)(vector * KBDACT_TEST_HANDLER_STRIDE);
        g_GuestMemory[vector * KBDACT_TEST_IVT_ENTRY_SIZE + 1] =
            (BYTE)((vector * KBDACT_TEST_HANDLER_STRIDE) >> BYTE_SHIFT);
        g_GuestMemory[vector * KBDACT_TEST_IVT_ENTRY_SIZE + KBDACT_TEST_IVT_SEGMENT] =
            KBDACT_TEST_HANDLER_SEGMENT & BYTE_MASK;
        g_GuestMemory[vector * KBDACT_TEST_IVT_ENTRY_SIZE + KBDACT_TEST_IVT_SEGMENT_HIGH] =
            KBDACT_TEST_HANDLER_SEGMENT >> BYTE_SHIFT;
        g_GuestMemory[((DWORD)KBDACT_TEST_HANDLER_SEGMENT << PARAGRAPH_SHIFT)
                      + vector * KBDACT_TEST_HANDLER_STRIDE] = KBDACT_TEST_HLT;
        g_GuestMemory[((DWORD)KBDACT_TEST_HANDLER_SEGMENT << PARAGRAPH_SHIFT)
                      + vector * KBDACT_TEST_HANDLER_STRIDE + 1] = KBDACT_TEST_IRET;
    }
    g_GuestMemory[(DWORD)KBDACT_TEST_RETURN_SEGMENT << PARAGRAPH_SHIFT] =
        KBDACT_TEST_HLT;
    cpu.Segments[KBDACT_TEST_SS] = KBDACT_TEST_STACK_SEGMENT;
    cpu.Registers[KBDACT_TEST_SP] = KBDACT_TEST_STACK_TOP - KBDACT_TEST_FRAME_SIZE;
    g_GuestMemory[((DWORD)KBDACT_TEST_STACK_SEGMENT << PARAGRAPH_SHIFT)
                  + KBDACT_TEST_FRAME_CS] = KBDACT_TEST_RETURN_SEGMENT & BYTE_MASK;
    g_GuestMemory[((DWORD)KBDACT_TEST_STACK_SEGMENT << PARAGRAPH_SHIFT)
                  + KBDACT_TEST_FRAME_CS_HIGH] =
        KBDACT_TEST_RETURN_SEGMENT >> BYTE_SHIFT;
    g_GuestMemory[((DWORD)KBDACT_TEST_STACK_SEGMENT << PARAGRAPH_SHIFT)
                  + KBDACT_TEST_FRAME_FLAGS] = KBDACT_TEST_CALLER_FLAGS;
    cpu.Segments[KBDACT_TEST_CS] = DOS_CTAB_SEG;
    cpu.Ip = (WORD)(DOS_KBDACT_OFF + entry);
    cpu.Flags = KBDACT_TEST_INITIAL_FLAGS;
    cpu.Registers[KBDACT_TEST_AX] = KBDACT_TEST_SAVED_AX;
    cpu.Segments[KBDACT_TEST_DS] = KBDACT_TEST_SAVED_DS;
    g_CallCount = 0;
    g_LastVector = KBDACT_TEST_NONE;
    return cpu;
}

/* Run until home; `budget` steps max. A HLT in the handler block is a call. */
static INT KeyboardActionTestRun(PV86_CPU cpu, INT budget, INT clearPauseAfter)
{
    INT stepCount = 0;

    for (;;)
    {
        if (++stepCount > budget)
            return KBDACT_TEST_OUT_OF_BUDGET;
        if (clearPauseAfter && stepCount == clearPauseAfter)
            g_GuestMemory[KBDACT_TEST_PAUSE_FLAGS_LINEAR] &= (BYTE)~KBDACT_TEST_PAUSE_BIT;
        if (g_StopOffset >= 0 && cpu->Segments[KBDACT_TEST_CS] == DOS_CTAB_SEG
            && cpu->Ip == (WORD)g_StopOffset)
            return KBDACT_TEST_STOPPED;
        if (V86Step(cpu))
            continue;
        if (cpu->Segments[KBDACT_TEST_CS] == KBDACT_TEST_RETURN_SEGMENT && cpu->Ip == 0)
            return KBDACT_TEST_RETURNED;
        if (cpu->Segments[KBDACT_TEST_CS] == KBDACT_TEST_HANDLER_SEGMENT)
        {
            g_LastVector = cpu->Ip / KBDACT_TEST_HANDLER_STRIDE;
            g_LastAx = (WORD)cpu->Registers[KBDACT_TEST_AX];
            ++g_CallCount;
            g_InterruptFlagAtCall = 0;
            if (g_LastVector == g_ClearCarryVector)   /* the IRET pops FLAGS at SS:SP+4 */
                g_GuestMemory[((DWORD)cpu->Segments[KBDACT_TEST_SS] << PARAGRAPH_SHIFT)
                              + (WORD)(cpu->Registers[KBDACT_TEST_SP] + KBDACT_TEST_FLAGS_POP_OFFSET)]
                    &= (BYTE)~KBDACT_TEST_FLAG_CF;
            cpu->Ip = (WORD)(cpu->Ip + 1);
            continue;
        }
        return KBDACT_TEST_LOST;
    }
}

/* #244: push the interrupted code's AX (1234h) above the INT 09h frame. */
static VOID KeyboardActionTestPushSavedAx(PV86_CPU cpu)
{
    cpu->Registers[KBDACT_TEST_SP] -= KBDACT_TEST_WORD_SIZE;
    g_GuestMemory[((DWORD)KBDACT_TEST_STACK_SEGMENT << PARAGRAPH_SHIFT)
                  + cpu->Registers[KBDACT_TEST_SP]] = KBDACT_TEST_SAVED_AX_LOW;
    g_GuestMemory[((DWORD)KBDACT_TEST_STACK_SEGMENT << PARAGRAPH_SHIFT)
                  + cpu->Registers[KBDACT_TEST_SP] + 1] = KBDACT_TEST_SAVED_AX_HIGH;
}

INT main(VOID)
{
    V86_CPU cpu;

    printf("== kbdact_test: BIOS INT 09h side-calls (#254)\n");
    KeyboardActionTestCheck(sizeof g_BiosKeyboardActionCode <= DOS_KBDACT_LEN
          && DOS_AUXPRN_OFF + DOS_AUXPRN_LEN <= DOS_KBDACT_OFF
          && DOS_KBDACT_OFF + DOS_KBDACT_LEN <= KBDACT_TEST_BLOCK_END,
          "fits between the AUX/PRN code and the block's end");

    cpu = KeyboardActionTestSetup(BIOS_KEYBOARD_ACTION_BREAK);
    KeyboardActionTestCheck(KeyboardActionTestRun(&cpu, KBDACT_TEST_BUDGET, KBDACT_TEST_NEVER_CLEAR)
                                == KBDACT_TEST_RETURNED
                            && g_CallCount == 1 && g_LastVector == KBDACT_TEST_INT_1BH,
                            "brk: calls INT 1Bh once, returns");
    KeyboardActionTestCheck((WORD)cpu.Registers[KBDACT_TEST_AX] == KBDACT_TEST_SAVED_AX
                            && cpu.Registers[KBDACT_TEST_SP] == KBDACT_TEST_STACK_TOP
                            && !(cpu.Flags & KBDACT_TEST_FLAG_IF),
                            "brk: AX kept, stack balanced, caller's IF restored");

    cpu = KeyboardActionTestSetup(BIOS_KEYBOARD_ACTION_PRINT_SCREEN);
    KeyboardActionTestCheck(KeyboardActionTestRun(&cpu, KBDACT_TEST_BUDGET, KBDACT_TEST_NEVER_CLEAR)
                                == KBDACT_TEST_RETURNED
                            && g_CallCount == 1 && g_LastVector == KBDACT_TEST_INT_05H,
                            "prt: calls INT 05h once");

    cpu = KeyboardActionTestSetup(BIOS_KEYBOARD_ACTION_SYSREQ_DOWN);
    KeyboardActionTestCheck(KeyboardActionTestRun(&cpu, KBDACT_TEST_BUDGET, KBDACT_TEST_NEVER_CLEAR)
                                == KBDACT_TEST_RETURNED
                            && g_CallCount == 1 && g_LastVector == KBDACT_TEST_INT_15H
                            && g_LastAx == KBDACT_TEST_SYSREQ_DOWN_AX
                            && (WORD)cpu.Registers[KBDACT_TEST_AX] == KBDACT_TEST_SAVED_AX,
                            "sysd: INT 15h AX=8500h, AX restored");
    cpu = KeyboardActionTestSetup(BIOS_KEYBOARD_ACTION_SYSREQ_UP);
    KeyboardActionTestCheck(KeyboardActionTestRun(&cpu, KBDACT_TEST_BUDGET, KBDACT_TEST_NEVER_CLEAR)
                                == KBDACT_TEST_RETURNED
                            && g_CallCount == 1 && g_LastVector == KBDACT_TEST_INT_15H
                            && g_LastAx == KBDACT_TEST_SYSREQ_UP_AX,
                            "sysu: INT 15h AX=8501h");

    cpu = KeyboardActionTestSetup(BIOS_KEYBOARD_ACTION_PAUSE);
    g_GuestMemory[KBDACT_TEST_PAUSE_FLAGS_LINEAR] = KBDACT_TEST_PAUSE_BIT;
    KeyboardActionTestCheck(KeyboardActionTestRun(&cpu, KBDACT_TEST_PAUSE_BUDGET,
                                                  KBDACT_TEST_NEVER_CLEAR)
                                == KBDACT_TEST_OUT_OF_BUDGET,
                            "pause: spins while 0040:0018 bit 3 is set");
    KeyboardActionTestCheck(cpu.Flags & KBDACT_TEST_FLAG_IF, "pause: ...with interrupts ON");
    cpu = KeyboardActionTestSetup(BIOS_KEYBOARD_ACTION_PAUSE);
    g_GuestMemory[KBDACT_TEST_PAUSE_FLAGS_LINEAR] = KBDACT_TEST_PAUSE_BIT;
    KeyboardActionTestCheck(KeyboardActionTestRun(&cpu, KBDACT_TEST_PAUSE_BUDGET,
                                                  KBDACT_TEST_CLEAR_PAUSE_AT)
                                == KBDACT_TEST_RETURNED && g_CallCount == 0,
                            "pause: returns once the bit clears");
    KeyboardActionTestCheck((WORD)cpu.Registers[KBDACT_TEST_AX] == KBDACT_TEST_SAVED_AX
                            && cpu.Segments[KBDACT_TEST_DS] == KBDACT_TEST_SAVED_DS
                            && cpu.Registers[KBDACT_TEST_SP] == KBDACT_TEST_STACK_TOP,
                            "pause: AX and DS restored, stack balanced");

    /* -- #244: k4f, the INT 15h AH=4Fh call. The host has pushed the interrupted AX
     * (1234h) above the INT 09h frame and loaded AX = 4F00h | scancode.
     */
    KeyboardActionTestCheck(g_BiosKeyboardActionCode[BIOS_KEYBOARD_ACTION_IRET] == KBDACT_TEST_IRET,
                            "k4f: KBDACT_IRET names a bare IRET");
    KeyboardActionTestCheck(g_BiosKeyboardActionCode[BIOS_KEYBOARD_ACTION_INTERCEPT_BOP]
                                == KBDACT_TEST_BOP_BYTE
                            && g_BiosKeyboardActionCode[BIOS_KEYBOARD_ACTION_INTERCEPT_BOP + 1]
                                == KBDACT_TEST_BOP_BYTE
                            && g_BiosKeyboardActionCode[BIOS_KEYBOARD_ACTION_INTERCEPT_BOP
                                                        + KBDACT_TEST_BOP_NUMBER_BYTE]
                                == KBDACT_TEST_BOP_09H,
                            "k4f: KBDACT_K4F_BOP names a BOP 09h");
    cpu = KeyboardActionTestSetup(BIOS_KEYBOARD_ACTION_INTERCEPT);
    KeyboardActionTestPushSavedAx(&cpu);
    cpu.Registers[KBDACT_TEST_AX] = KBDACT_TEST_INTERCEPT_AX;
    g_StopOffset = DOS_KBDACT_OFF + BIOS_KEYBOARD_ACTION_INTERCEPT_BOP;
    KeyboardActionTestCheck(KeyboardActionTestRun(&cpu, KBDACT_TEST_BUDGET, KBDACT_TEST_NEVER_CLEAR)
                                == KBDACT_TEST_STOPPED
                            && g_CallCount == 1 && g_LastVector == KBDACT_TEST_INT_15H
                            && g_LastAx == KBDACT_TEST_INTERCEPT_AX,
                            "k4f: calls INT 15h with AH=4Fh AL=scancode (CF=1 on entry) ...");
    KeyboardActionTestCheck((cpu.Registers[KBDACT_TEST_AX] & KBDACT_TEST_AL_MASK) == KBDACT_TEST_SCANCODE
                            && (cpu.Flags & KBDACT_TEST_FLAG_CF)
                            && cpu.Registers[KBDACT_TEST_SP]
                                == KBDACT_TEST_STACK_TOP - KBDACT_TEST_FRAME_SIZE
                                   - KBDACT_TEST_WORD_SIZE,
        "k4f: ...a CF=1 answer reaches the translate BOP with AL as left, the saved AX still pushed");
    cpu = KeyboardActionTestSetup(BIOS_KEYBOARD_ACTION_INTERCEPT);
    KeyboardActionTestPushSavedAx(&cpu);
    cpu.Registers[KBDACT_TEST_AX] = KBDACT_TEST_INTERCEPT_AX;
    g_ClearCarryVector = KBDACT_TEST_INT_15H;
    g_LastOutPort = g_LastOutValue = KBDACT_TEST_NONE;
    KeyboardActionTestCheck(KeyboardActionTestRun(&cpu, KBDACT_TEST_BUDGET, KBDACT_TEST_NEVER_CLEAR)
                                == KBDACT_TEST_RETURNED && g_CallCount == 1,
                            "k4f: a CF=0 answer (swallow) never reaches the BOP...");
    KeyboardActionTestCheck(g_LastOutPort == KBDACT_TEST_EOI_PORT
                            && g_LastOutValue == KBDACT_TEST_EOI,
                            "k4f: ...sends the BIOS's own EOI (20h to port 20h)...");
    KeyboardActionTestCheck((WORD)cpu.Registers[KBDACT_TEST_AX] == KBDACT_TEST_SAVED_AX
                            && cpu.Registers[KBDACT_TEST_SP] == KBDACT_TEST_STACK_TOP,
                            "k4f: ...restores AX and IRETs, stack balanced");
    g_ClearCarryVector = KBDACT_TEST_NONE;

    /* -- #274: p5, the default INT 05h. The host's two BOPs are simulated here: begin
     * hands back one byte ('X'), next says "done" (CF=1).
     */
    KeyboardActionTestCheck(g_BiosKeyboardActionCode[BIOS_KEYBOARD_ACTION_DEFAULT_INT05_BEGIN]
                                == KBDACT_TEST_BOP_BYTE
                            && g_BiosKeyboardActionCode[BIOS_KEYBOARD_ACTION_DEFAULT_INT05_NEXT]
                                == KBDACT_TEST_BOP_BYTE
                            && g_BiosKeyboardActionCode[BIOS_KEYBOARD_ACTION_DEFAULT_INT05_BEGIN
                                                        + KBDACT_TEST_BOP_NUMBER_BYTE]
                                == KBDACT_TEST_BOP_09H
                            && g_BiosKeyboardActionCode[BIOS_KEYBOARD_ACTION_DEFAULT_INT05_NEXT
                                                        + KBDACT_TEST_BOP_NUMBER_BYTE]
                                == KBDACT_TEST_BOP_09H,
                            "p5: both sites are BOP 09h");
    cpu = KeyboardActionTestSetup(BIOS_KEYBOARD_ACTION_DEFAULT_INT05);
    g_StopOffset = DOS_KBDACT_OFF + BIOS_KEYBOARD_ACTION_DEFAULT_INT05_BEGIN;
    KeyboardActionTestCheck(KeyboardActionTestRun(&cpu, KBDACT_TEST_BUDGET, KBDACT_TEST_NEVER_CLEAR)
                                == KBDACT_TEST_STOPPED
                            && (cpu.Flags & KBDACT_TEST_FLAG_IF),
                            "p5: STI, then the begin BOP");
    cpu.Registers[KBDACT_TEST_AX] = KBDACT_TEST_PRINTED_AX;
    cpu.Registers[KBDACT_TEST_DX] = 0;
    cpu.Flags &= ~KBDACT_TEST_FLAG_CF;
    cpu.Ip += KBDACT_TEST_BOP_SIZE;   /* host: emit 'X' */
    g_StopOffset = DOS_KBDACT_OFF + BIOS_KEYBOARD_ACTION_DEFAULT_INT05_NEXT;
    KeyboardActionTestCheck(KeyboardActionTestRun(&cpu, KBDACT_TEST_BUDGET, KBDACT_TEST_NEVER_CLEAR)
                                == KBDACT_TEST_STOPPED
                            && g_CallCount == 1 && g_LastVector == KBDACT_TEST_INT_17H
                            && g_LastAx == KBDACT_TEST_PRINTED_AX,
                            "p5: the byte goes out through INT 17h AH=00h, then the next BOP");
    cpu.Registers[KBDACT_TEST_AX] = KBDACT_TEST_SAVED_AX;
    cpu.Flags |= KBDACT_TEST_FLAG_CF;
    cpu.Ip += KBDACT_TEST_BOP_SIZE;    /* host: done */
    g_StopOffset = KBDACT_TEST_NONE;
    KeyboardActionTestCheck(KeyboardActionTestRun(&cpu, KBDACT_TEST_BUDGET, KBDACT_TEST_NEVER_CLEAR)
                                == KBDACT_TEST_RETURNED
                            && g_CallCount == 1 && cpu.Registers[KBDACT_TEST_SP] == KBDACT_TEST_STACK_TOP
                            && !(cpu.Flags & KBDACT_TEST_FLAG_IF),
        "p5: CF=1 from the host ends it: IRET, stack balanced, caller's IF back");
    cpu = KeyboardActionTestSetup(BIOS_KEYBOARD_ACTION_DEFAULT_INT05);
    g_StopOffset = DOS_KBDACT_OFF + BIOS_KEYBOARD_ACTION_DEFAULT_INT05_BEGIN;
    (VOID)KeyboardActionTestRun(&cpu, KBDACT_TEST_BUDGET, KBDACT_TEST_NEVER_CLEAR);
    cpu.Flags |= KBDACT_TEST_FLAG_CF;
    cpu.Ip += KBDACT_TEST_BOP_SIZE;    /* host: busy */
    g_StopOffset = KBDACT_TEST_NONE;
    KeyboardActionTestCheck(KeyboardActionTestRun(&cpu, KBDACT_TEST_BUDGET, KBDACT_TEST_NEVER_CLEAR)
                                == KBDACT_TEST_RETURNED && g_CallCount == 0,
                            "p5: CF=1 at begin (already printing) -> straight back, no INT 17h");

    printf("\n%d checks, %d failed\n", g_Checks, g_Failures);
    return g_Failures ? 1 : 0;
}
