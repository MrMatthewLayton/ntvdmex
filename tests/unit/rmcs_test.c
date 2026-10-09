/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Off-VM battery for src/host/dpmi_rmcs.h (GH #247): the DPMI real-mode
 * call structure and how INT 31h 0300h routes a vector.
 *
 * WHAT IS PINNED, AND WHY EACH ONE IS HERE.
 *   - The layout, field by field, with a DIFFERENT value in every byte: a swapped pair
 *     of offsets (EBX/EDX, ES/DS) passes any test that fills both with the same thing.
 *   - The write-back covers EVERY output -- 32-bit general registers, FLAGS, ES DS FS GS.
 *     #247 was precisely a write-back that dropped BP, ES and DS, and wrote the low
 *     words only.
 *   - ...and NOTHING ELSE: CS, IP, SS, SP and the reserved dword are left exactly as the
 *     client put them. The spec says they "are not modified", and the pre-#247 0300h
 *     retarget broke that by storing IVT[BL] into the caller's CS:IP.
 *   - The routing table: INT 21h is always host-side; 33h/10h only while the IVT holds
 *     our stub; every other vector -- ours or a guest's -- RUNS; a null vector and the
 *     simintrefl_off.flag rollback do not.
 *   - The CX stack-copy plan, including the refusal that keeps a stale CX harmless and
 *     SP = 0 meaning a full 64 KB.
 *
 *   cc -std=c99 -I src/host -o rmcs_test tests/unit/rmcs_test.c && ./rmcs_test
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "../../src/host/dpmi_rmcs.h"

static INT g_Total = 0;
static INT g_Failures = 0;
#define CHECK(condition,message) do{ g_Total++; if(condition){printf("  PASS  %s\n",(message));} \
    else{printf("  FAIL  %s\n",(message)); g_Failures++;} }while(0)

#define OURS    0x0050  /* DOS_HDLR_SEG -- passed in, so the test needs no layout header */

static VOID RmcsTestFillPattern(PBYTE bytes, UINT count)
{
    UINT index;

    for (index = 0; index < count; ++index)
        bytes[index] = (BYTE)(0x11 + index * 7);
}

INT main(VOID)
{
    printf("== DPMI real-mode call structure (GH #247) ==\n");

    CHECK(RMCS_SIZE == 50, "the structure is 50 bytes");
    CHECK(RMCS_EDI == 0x00 && RMCS_ESI == 0x04 && RMCS_EBP == 0x08 && RMCS_EBX == 0x10
          && RMCS_EDX == 0x14 && RMCS_ECX == 0x18 && RMCS_EAX == 0x1C,
          "general registers at 00/04/08/10/14/18/1C (0C reserved)");
    CHECK(RMCS_FLAGS == 0x20 && RMCS_ES == 0x22 && RMCS_DS == 0x24 && RMCS_FS == 0x26
          && RMCS_GS == 0x28 && RMCS_IP == 0x2A && RMCS_CS == 0x2C && RMCS_SP == 0x2E
          && RMCS_SS == 0x30, "FLAGS ES DS FS GS IP CS SP SS at 20..30");

    /* ---- read: every field, full width, little-endian, any alignment ---- */
    { BYTE buffer[RMCS_SIZE + 1];
    RMCS_REGS registers;
    PBYTE record = buffer + 1;   /* odd address */
      RmcsTestFillPattern(buffer, sizeof buffer);
      RmcsRead(record, &registers);
      CHECK(registers.Edi == (UINT32)(record[0] | record[1] << 8 | record[2] << 16 | (UINT32)record[3] << 24), "EDI read as a full dword");
      CHECK(registers.Eax == (UINT32)(record[0x1C] | record[0x1D] << 8 | record[0x1E] << 16 | (UINT32)record[0x1F] << 24),
            "EAX read as a full dword (the top half is an input on a 386)");
      CHECK(registers.Ebx != registers.Edx && registers.Ebx == (UINT32)(record[0x10] | record[0x11] << 8 | record[0x12] << 16 | (UINT32)record[0x13] << 24),
            "EBX from +10, not +14");
      CHECK(registers.Es == (WORD)(record[0x22] | record[0x23] << 8) && registers.Ds == (WORD)(record[0x24] | record[0x25] << 8),
            "ES from +22, DS from +24");
      CHECK(registers.Fs == (WORD)(record[0x26] | record[0x27] << 8) && registers.Gs == (WORD)(record[0x28] | record[0x29] << 8),
            "FS from +26, GS from +28 (they were never read: the 0301 arm set FS=GS=SS)");
      CHECK(registers.Flags == (WORD)(record[0x20] | record[0x21] << 8), "FLAGS from +20"); }

    /* ---- write: every output, and nothing that is not one ---- */
    { BYTE record[RMCS_SIZE], before[RMCS_SIZE];
    RMCS_REGS registers;
      RmcsTestFillPattern(record, sizeof record);
      memcpy(before, record, sizeof record);
      registers.Edi = 0xD1D2D3D4u;
      registers.Esi = 0x51525354u;
      registers.Ebp = 0xB1B2B3B4u;
      registers.Ebx = 0x0B0C0D0Eu;
      registers.Edx = 0xDDCCBBAAu;
      registers.Ecx = 0xC0C1C2C3u;
      registers.Eax = 0xA0A1A2A3u;
      registers.Flags = 0x0247;
      registers.Es = 0xE5E5;
      registers.Ds = 0xD5D5;
      registers.Fs = 0xF5F5;
      registers.Gs = 0x6565;
      RmcsWrite(record, &registers);
      CHECK(RmcsRead32(record, RMCS_EBP) == 0xB1B2B3B4u, "EBP written back (pre-#247 0300h dropped it)");
      CHECK(RmcsRead16(record, RMCS_ES) == 0xE5E5 && RmcsRead16(record, RMCS_DS) == 0xD5D5,
            "ES and DS written back (pre-#247 0300h dropped both -- AH=35h's ES:BX came back as the caller's ES)");
      CHECK(RmcsRead16(record, RMCS_FS) == 0xF5F5 && RmcsRead16(record, RMCS_GS) == 0x6565, "FS and GS written back");
      CHECK(RmcsRead32(record, RMCS_EAX) == 0xA0A1A2A3u && RmcsRead32(record, RMCS_EBX) == 0x0B0C0D0Eu
            && RmcsRead32(record, RMCS_ECX) == 0xC0C1C2C3u && RmcsRead32(record, RMCS_EDX) == 0xDDCCBBAAu
            && RmcsRead32(record, RMCS_ESI) == 0x51525354u && RmcsRead32(record, RMCS_EDI) == 0xD1D2D3D4u,
            "all seven general registers written back at full width");
      CHECK(RmcsRead16(record, RMCS_FLAGS) == 0x0247, "FLAGS written back (CF is the DOS answer)");
      CHECK(memcmp(record + RMCS_IP, before + RMCS_IP, 8) == 0,
            "IP, CS, SP, SS NOT modified -- the spec's rule; the old 0300 retarget wrote CS:IP");
      CHECK(memcmp(record + 0x0C, before + 0x0C, 4) == 0, "the reserved dword at +0C is not touched");
      { RMCS_REGS readBack;
      RmcsRead(record, &readBack);
        CHECK(memcmp(&registers, &readBack, sizeof registers) == 0, "write then read is the identity"); } }

    /* ---- 0300h routing ---- */
    CHECK(RmcsSimIntRoute(0x21, OURS, 0x0000, 1, OURS) == SIMINT_FAST, "21h, ours: host-side");
    CHECK(RmcsSimIntRoute(0x21, 0x1234, 0x0010, 1, OURS) == SIMINT_FAST,
          "21h, HOOKED by a guest: still host-side (the documented deviation, kept for the shelf)");
    CHECK(RmcsSimIntRoute(0x33, OURS, 0x0030, 1, OURS) == SIMINT_FAST, "33h, ours: host-side (Doom's mouse)");
    CHECK(RmcsSimIntRoute(0x10, OURS, 0x0020, 1, OURS) == SIMINT_FAST, "10h, ours: host-side (ZAR's int86 video)");
    CHECK(RmcsSimIntRoute(0x33, 0x2000, 0x0100, 1, OURS) == SIMINT_RUN, "33h hooked by a guest driver: RUN it");
    CHECK(RmcsSimIntRoute(0x10, 0xC000, 0x1234, 1, OURS) == SIMINT_RUN, "10h hooked (a VESA TSR): RUN it");
    CHECK(RmcsSimIntRoute(0x16, OURS, 0x0028, 1, OURS) == SIMINT_RUN,
          "16h, OUR stub: RUN it (pre-#247: echoed with CF=0)");
    CHECK(RmcsSimIntRoute(0x1A, OURS, 0x003C, 1, OURS) == SIMINT_RUN, "1Ah, our stub: RUN it");
    CHECK(RmcsSimIntRoute(0x15, 0x0090, 0x0010, 1, OURS) == SIMINT_RUN,
          "15h, our BIOS stub at DOS_CTAB_SEG: RUN it");
    CHECK(RmcsSimIntRoute(0x66, 0x34D3, 0x01D1, 1, OURS) == SIMINT_RUN, "66h, ZAR's Miles trampoline: RUN it");
    CHECK(RmcsSimIntRoute(0x66, 0x0000, 0x0000, 1, OURS) == SIMINT_NONE, "a NULL vector is never executed");
    CHECK(RmcsSimIntRoute(0x33, 0x0000, 0x0000, 1, OURS) == SIMINT_NONE,
          "a null 33h is neither ours nor runnable: NOT run, never 0:0");
    CHECK(RmcsSimIntRoute(0x16, OURS, 0x0028, 0, OURS) == SIMINT_NONE
          && RmcsSimIntRoute(0x66, 0x34D3, 0x01D1, 0, OURS) == SIMINT_NONE,
          "simintrefl_off.flag: nothing but 21/33/10 runs (pre-#247 routing)");
    CHECK(RmcsSimIntRoute(0x33, 0x2000, 0x0100, 0, OURS) == SIMINT_FAST
          && RmcsSimIntRoute(0x10, 0xC000, 0x1234, 0, OURS) == SIMINT_FAST,
          "simintrefl_off.flag: 33h/10h host-side whoever owns them (pre-#247)");

    /* ---- CX words of stack ---- */
    { WORD stackPointer;
      CHECK(RmcsStackPlan(0xFF00, 0, 6, &stackPointer) && stackPointer == 0xFF00, "CX=0: nothing copied, SP unchanged");
      CHECK(RmcsStackPlan(0xFF00, 3, 6, &stackPointer) && stackPointer == 0xFEFA, "CX=3: six bytes below SP");
      CHECK(!RmcsStackPlan(0x0100, 0x80, 6, &stackPointer) && stackPointer == 0x0100,
            "CX that does not fit: refused, SP unchanged (the call runs as before #247)");
      CHECK(RmcsStackPlan(0x0106, 0x80, 6, &stackPointer) && stackPointer == 0x0006, "exactly fits with the frame below");
      CHECK(!RmcsStackPlan(0x0105, 0x80, 6, &stackPointer), "one byte short: refused");
      CHECK(RmcsStackPlan(0x0000, 4, 6, &stackPointer) && stackPointer == 0xFFF8, "SP=0 is a full 64 KB, not an empty stack");
      CHECK(!RmcsStackPlan(0xFF00, 0xFFFF, 6, &stackPointer), "CX=FFFFh (128 KB) is never copied"); }

    printf("\n%d checks, %d failed\n", g_Total, g_Failures);
    return g_Failures ? 1 : 0;
}
