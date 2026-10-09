/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Off-VM battery for the VBE 2.0 protected-mode interface (#53).
 *
 * INT 10h AX=4F0Ah hands a protected-mode client a block of 32-bit code to COPY and CALL.
 * The only honest test of that is to do what a client does: take ES:DI and CX, copy the
 * block somewhere else entirely, and execute its three entry points -- here with the
 * host's own flat 32-bit interpreter (src/host/pm32interp.h), whose port hooks go to the
 * real video VDD on a bus. Then compare with what the INT 10h forms (4F05h/4F07h/4F09h)
 * do to the same machine. No oracle runs this: both real BIOSes we can execute answer
 * 4F0Ah with AX=0100h. The expectations are VBE 2.0 section 4.13's.
 *
 * Every expectation was written before the 4F0Ah arm existed and fails on that code
 * (4F0Ah -> AX=0100h, no block, no ports at 01CEh/01CFh).
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "vdd_video.h"

static BYTE g_GuestMemory[0x200000];          /* guest memory: real-mode MB + room to copy into */
static BYTE g_VideoMemory[VIDEO_APERTURE_SIZE];
static VIDEO_STATE g_Video;
static VDD_BUS g_Bus;
static UINT64 g_FakeMicroseconds = 1000000;
static UINT64 VbePmTestFakeClock(VOID)
{
    return g_FakeMicroseconds;
}

/* The interpreter's host hooks: flat memory, and port I/O onto the VDD bus. Each IN
 * advances the fake clock 50 us, so a retrace wait on 3DAh makes progress.
 */
static UINT32 g_InCount, g_OutCount;
static BYTE  Pm32HostRead8(UINT32 linear)
{
    return linear < sizeof g_GuestMemory ? g_GuestMemory[linear] : 0xFF;
}

static VOID     Pm32HostWrite8(UINT32 linear, BYTE value)
{
    if (linear < sizeof g_GuestMemory)
        g_GuestMemory[linear] = value;
}

static INT      Pm32HostCanAccess(UINT32 linear, INT width, INT isWrite)
{
    (VOID)isWrite;
    return linear + (UINT32)width <= sizeof g_GuestMemory;
}

static UINT32 Pm32HostIn(WORD port, INT width)
{
    UINT32 value = 0;

    g_FakeMicroseconds += 50;
    ++g_InCount;
    VddBusIo(&g_Bus, port, (BYTE)width, 1, &value);
    return value;
}

static VOID     Pm32HostOut(WORD port, INT width, UINT32 value)
{
    UINT32 busValue = value;

    ++g_OutCount;
    VddBusIo(&g_Bus, port, (BYTE)width, 0, &busValue);
}

#include "../../src/host/pm32interp.h"

static INT g_Total = 0, g_Failures = 0;
#define CHECK(condition,message) do{ g_Total++; if(condition){printf("  PASS  %s\n",(message));} \
    else{printf("  FAIL  %s\n",(message)); g_Failures++;} }while(0)

#define COPY    0x150000u   /* Where the "client" copies the block */
#define STACK   0x180000u
#define RETADR  0x1F0000u   /* A return address nothing executes */

/* Near-call entry `off` of the copied block with these registers; run to the RET.
 * 1 = returned to RETADR with ESP balanced; 0 = the interpreter declined something
 * (the code used an instruction outside its set) or it never returned.
 */
static INT VbePmTestCallPm(
    UINT32 offset,
    UINT32 ebx,
    UINT32 ecx,
    UINT32 edx,
    UINT32 edi,
    UINT32 esBase,
    PPM32_CPU output)
{
    PM32_CPU cpu;
    long step;

    memset(&cpu, 0, sizeof cpu);
    cpu.Registers[0] = 0xA5A5A5A5u;
    cpu.Registers[1] = ecx;
    cpu.Registers[2] = edx;
    cpu.Registers[3] = ebx;
    cpu.Registers[5] = 0xB5B5B5B5u;
    cpu.Registers[6] = 0xC6C6C6C6u;
    cpu.Registers[7] = edi;
    cpu.Registers[4] = STACK - 4;
    g_GuestMemory[STACK - 4] = (BYTE)RETADR;
    g_GuestMemory[STACK - 3] = (BYTE)(RETADR >> 8);
    g_GuestMemory[STACK - 2] = (BYTE)(RETADR >> 16);
    g_GuestMemory[STACK - 1] = (BYTE)(RETADR >> 24);
    cpu.Eip = COPY + offset;
    cpu.Flags = 0x202;
    cpu.SegmentBases[0] = esBase;                    /* ES; CS/SS/DS flat 0 */
    for (step = 0; step < 2000000; ++step)
    {
        if (cpu.Eip == RETADR)
            break;
        if (!Pm32Step(&cpu))
        {
            printf("    declined at +%#x (op %02X)\n", cpu.Eip - COPY, g_GuestMemory[cpu.Eip]);
            return 0;
        }
    }
    if (output)
        *output = cpu;
    return cpu.Eip == RETADR && cpu.Registers[4] == STACK;
}

static VOID VbePmTestInt10(
    UINT32 eax,
    UINT32 ebx,
    UINT32 ecx,
    UINT32 edx,
    PNTVDD_REGISTERS registers)
{
    memset(registers, 0, sizeof *registers);
    registers->Eax = eax;
    registers->Ebx = ebx;
    registers->Ecx = ecx;
    registers->Edx = edx;
    VddBusDeliverInterrupt(&g_Bus, 0x10, registers);
}

INT main(VOID)
{
    NTVDD_DEVICE device;
    NTVDD_REGISTERS registers;
    PM32_CPU cpu;
    UINT32 block, length, index;
    WORD windowEntry, startEntry, paletteEntry, portsOffset;

    printf("== VBE 4F0Ah protected-mode interface battery (#53) ==\n");
    memset(&g_Video, 0, sizeof g_Video);
    g_Video.VideoMemory = g_VideoMemory;
    device = VddVideoDevice(&g_Video);
    VddBusInitialize(&g_Bus, g_GuestMemory);
    VddBusSetSinks(&g_Bus, 0, 0, 0, 0);
    CHECK(VddBusAdd(&g_Bus, &device) == 0, "video VDD on the bus (01CEh/01CFh claimed with the rest)");
    g_Video.TimeUs = VbePmTestFakeClock;

    /* ---- 4F0Ah BL=00h: the table ---- */
    VbePmTestInt10(0x4F0A, 0x0000, 0xC1C1, 0, &registers);
    CHECK((registers.Eax & 0xFFFF) == 0x004F, "4F0Ah BL=00h: AX=004Fh (was 0100h, 'no such function')");
    block = ((UINT32)registers.Es << 4) + (registers.Edi & 0xFFFF);
    length = registers.Ecx & 0xFFFF;
    CHECK(registers.Es == VDD_VBEPM_SEG && (registers.Edi & 0xFFFF) == 0 && length >= 16 && length < 0x100,
          "4F0Ah: ES:DI = B260:0000, CX = the block's length (code included)");
    windowEntry   = (WORD)(g_GuestMemory[block + 0] | (g_GuestMemory[block + 1] << 8));
    startEntry = (WORD)(g_GuestMemory[block + 2] | (g_GuestMemory[block + 3] << 8));
    paletteEntry   = (WORD)(g_GuestMemory[block + 4] | (g_GuestMemory[block + 5] << 8));
    portsOffset = (WORD)(g_GuestMemory[block + 6] | (g_GuestMemory[block + 7] << 8));
    CHECK(windowEntry >= 8 && startEntry >= 8 && paletteEntry >= 8 && windowEntry < length && startEntry < length && paletteEntry < length,
          "table: the three entry offsets lie inside the block, past the 4-word header");
    {   /* the port list: words, FFFFh-terminated, then an empty memory list */
        INT has1ce = 0, has1cf = 0, has3c9 = 0, has3da = 0, count = 0;
        UINT32 listAddress = block + portsOffset;
        for (;;) { WORD port = (WORD)(g_GuestMemory[listAddress] | (g_GuestMemory[listAddress + 1] << 8));
        listAddress += 2;
                   if (port == 0xFFFF || ++count > 16)
                       break;
                   has1ce |= port == 0x1CE;
                   has1cf |= port == 0x1CF;
                   has3c9 |= port == 0x3C9;
                   has3da |= port == 0x3DA; }
        CHECK(has1ce && has1cf && has3c9 && has3da && g_GuestMemory[listAddress] == 0xFF && g_GuestMemory[listAddress + 1] == 0xFF,
              "table +6: every port the code touches, FFFFh, then an empty memory list (FFFFh)");
    }
    VbePmTestInt10(0x4F0A, 0x0001, 0, 0, &registers);
    CHECK((registers.Eax & 0xFFFF) == 0x014F, "4F0Ah BL=01h: 014Fh (the subfunction does not exist)");

    /* ---- the client copies the block somewhere else entirely ---- */
    memcpy(g_GuestMemory + COPY, g_GuestMemory + block, length);
    memset(g_GuestMemory + block, 0xCC, length);          /* and the original is gone: nothing may point back */

    /* ---- outside a VESA mode the ports refuse and change nothing ---- */
    {   UINT32 rejectedBefore = g_Video.VbePmRejected;
        CHECK(VbePmTestCallPm(windowEntry, 0x0000, 0, 3, 0, 0, NULL) && g_Video.VbePmRejected == rejectedBefore + 1 && g_Video.VesaBank == 0,
              "SetWindow in mode 3: returns, refused, counted (vbe_pm_rej)"); }

    /* ---- 640x480x8 banked ---- */
    VbePmTestInt10(0x4F02, 0x0101, 0, 0, &registers);
    CHECK((registers.Eax & 0xFFFF) == 0x004F && g_Video.IsVesa && !g_Video.IsVesaLfb, "4F02h 0101h: banked 640x480x8");
    memset(g_VideoMemory, 0x11, VIDEO_VESA_WINDOW);       /* the client draws bank 0 ... */
    CHECK(VbePmTestCallPm(windowEntry, 0x0000, 0, 2, 0, 0, &cpu), "SetWindow (copied, near-called) returns to the caller, ESP balanced");
    CHECK(g_Video.VesaBank == 2 && g_Video.VbePmBankCount == 1, "SetWindow DX=2: window A is bank 2");
    CHECK(g_Video.VesaVram[0] == 0x11 && g_Video.VesaVram[VIDEO_VESA_WINDOW - 1] == 0x11,
          "SetWindow: the old window was flushed into bank 0, as 4F05h does");
    CHECK(cpu.Registers[0] == 0xA5A5A5A5u && cpu.Registers[2] == 2 && cpu.Registers[3] == 0 && cpu.Registers[5] == 0xB5B5B5B5u && cpu.Registers[6] == 0xC6C6C6C6u,
          "SetWindow: EAX/EDX/EBX/EBP/ESI preserved");
    VbePmTestInt10(0x4F05, 0x0100, 0, 0, &registers);
    CHECK((registers.Edx & 0xFFFF) == 2, "4F05h BH=01h (get) agrees: bank 2");
    CHECK(VbePmTestCallPm(windowEntry, 0x0001, 0, 1, 0, 0, NULL) && g_Video.VesaBank == 2,
          "SetWindow BL=01h (window B, which does not exist): no change");
    CHECK(VbePmTestCallPm(windowEntry, 0x0000, 0, 0x7FFF, 0, 0, NULL) && g_Video.VesaBank == 2,
          "SetWindow past the end of VRAM: refused, bank unchanged");
    CHECK(VbePmTestCallPm(windowEntry, 0x0000, 0, 0, 0, 0, NULL) && g_Video.VesaBank == 0 && g_VideoMemory[0] == 0x11,
          "SetWindow back to 0: bank 0's bytes come back into the window");

    /* ---- SetDisplayStart: CX/DX = start in DWORDs; 4F07h BL=01h reads it back ---- */
    {   UINT32 origin = 640u * 100u + 64u;     /* (64,100) */
        CHECK(VbePmTestCallPm(startEntry, 0x0000, (origin / 4) & 0xFFFF, (origin / 4) >> 16, 0, 0, NULL)
              && g_Video.VesaOrigin == origin && g_Video.VesaOriginLive == origin && g_Video.VbePmStartCount == 1,
              "SetDisplayStart BL=00h: start = DX:CX * 4, shown at once");
        VbePmTestInt10(0x4F07, 0x0001, 0, 0, &registers);
        CHECK((registers.Ecx & 0xFFFF) == 64 && (registers.Edx & 0xFFFF) == 100, "4F07h BL=01h agrees: x=64, y=100");
    }
    {   UINT32 origin = 640u * 480u, insBefore = g_InCount;          /* page 2 */
        CHECK(VbePmTestCallPm(startEntry, 0x0080, (origin / 4) & 0xFFFF, (origin / 4) >> 16, 0, 0, NULL)
              && g_Video.VesaOrigin == origin && g_InCount > insBefore + 2,
              "SetDisplayStart BL=80h: waits on 3DAh for the retrace, then sets it");
    }
    {   UINT32 origin = g_Video.VesaOrigin, farOrigin = 0x3FFFFFu;     /* far past 4 MB */
        CHECK(VbePmTestCallPm(startEntry, 0x0000, farOrigin & 0xFFFF, farOrigin >> 16, 0, 0, NULL) && g_Video.VesaOrigin == origin,
              "SetDisplayStart past VRAM: refused, start unchanged");
    }

    /* ---- SetPalette: ES:EDI = B,G,R,pad entries; 4F09h BL=01h reads them back ---- */
    {   static const BYTE entries[3 * 4] = { 0x01, 0x02, 0x03, 0, 0x3F, 0x00, 0x20, 0, 0x10, 0x11, 0x12, 0 };
        UINT32 dataAddress = 0x160000u, readBackAddress = 0x9000u;      /* ES base 0x100000 + EDI 0x60000 */
        memcpy(g_GuestMemory + dataAddress, entries, sizeof entries);
        CHECK(VbePmTestCallPm(paletteEntry, 0x0000, 3, 0x40, 0x60000, 0x100000, &cpu), "SetPalette (ES:EDI, 3 entries at 40h) returns");
        CHECK(cpu.Registers[1] == 3 && cpu.Registers[2] == 0x40 && cpu.Registers[7] == 0x60000, "SetPalette: ECX/EDX/EDI preserved");
        memset(&registers, 0, sizeof registers);
        registers.Eax = 0x4F09;
        registers.Ebx = 0x0001;
        registers.Ecx = 3;
        registers.Edx = 0x40;
        registers.Es = (WORD)(readBackAddress >> 4);
        registers.Edi = 0;
        VddBusDeliverInterrupt(&g_Bus, 0x10, &registers);
        CHECK((registers.Eax & 0xFFFF) == 0x004F && memcmp(g_GuestMemory + readBackAddress, entries, sizeof entries) == 0,
              "4F09h BL=01h reads back exactly what SetPalette wrote (B,G,R, 6-bit)");
        CHECK(VbePmTestCallPm(paletteEntry, 0x0080, 1, 0x41, 0x60004, 0x100000, NULL), "SetPalette BL=80h (retrace wait) returns");
    }

    /* ---- the LFB form of the mode has no window to switch ---- */
    VbePmTestInt10(0x4F02, 0x4101, 0, 0, &registers);
    {   UINT32 rejectedBefore = g_Video.VbePmRejected;
        CHECK(VbePmTestCallPm(windowEntry, 0x0000, 0, 1, 0, 0, NULL) && g_Video.VbePmRejected == rejectedBefore + 1,
              "SetWindow in an LFB mode: refused (4F05h answers 03h there)"); }

    /* ---- the block in guest memory is restored by every 4F0Ah call ---- */
    VbePmTestInt10(0x4F0A, 0x0000, 0, 0, &registers);
    for (index = 0; index < length; ++index)
        if (g_GuestMemory[block + index] != g_GuestMemory[COPY + index])
            break;
    CHECK(index == length, "4F0Ah again: the block at B260:0000 is rewritten, byte for byte");

    printf("\n%d checks, %d failed\n", g_Total, g_Failures);
    return g_Failures ? 1 : 0;
}
