/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Off-VM unit battery for the VDD device bus (vdd_bus.c/ntvdd.h).
 *
 * Slice-1 of M3 (ADR-0008): prove the bus routes hardware events to whichever
 * VDD claimed them -- I/O ports, memory windows, software interrupts, the frame
 * tick -- and that the service callbacks (raise_irq, map_flat, present) work,
 * all natively on the build host with no XP VM. A stand-in "fake PIT" VDD
 * exercises the same claim surface the real vdd_pit will use.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include <stdio.h>
#include <string.h>
#include "vdd_bus.h"

static INT g_Total = 0, g_Failures = 0;
#define CHECK(condition, message) do {                                  \
        g_Total++;                                               \
        if (condition) { printf("  PASS  %s\n", (message)); }           \
        else      { printf("  FAIL  %s\n", (message)); g_Failures++; }  \
    } while (0)

/* ---- a fake device: claims the PIT ports, INT 1Ah, and a frame tick ------ */
typedef struct
{
    PVDD_BUS Bus;
    BYTE  Reload;        /* last byte OUT to port 0x40 */
    UINT32 Ticks;         /* incremented each frame; raises IRQ0 */
    INT      ResetCalls;
    BYTE  LastInPort;
} FAKE_PIT;

static VOID VddTestPitOut(PVOID self, WORD port, BYTE width, UINT32 value)
{
    FAKE_PIT *pit = (FAKE_PIT *)self;
    (VOID)width;
    if (port == 0x40) pit->Reload = (BYTE)value;
}

static VOID VddTestPitIn(PVOID self, WORD port, BYTE width, PUINT32 value)
{
    FAKE_PIT *pit = (FAKE_PIT *)self;
    (VOID)width;
    pit->LastInPort = (BYTE)port;
    *value = pit->Reload;
}

static VOID VddTestPitInt1a(PVOID self, PNTVDD_REGISTERS registers)
{
    FAKE_PIT *pit = (FAKE_PIT *)self;
    if (VddGetAh(registers) == 0)
    {
        VddSetCx(registers, (WORD)(pit->Ticks >> 16));
        VddSetDx(registers, (WORD)pit->Ticks);
        registers->CarryFlag = 0;
    }
}

static VOID VddTestPitFrame(PVOID self)
{
    FAKE_PIT *pit = (FAKE_PIT *)self;
    pit->Ticks++;
    VddRaiseIrq(pit->Bus, 0);
}

static VOID VddTestPitReset(PVOID self)
{
    FAKE_PIT *pit = (FAKE_PIT *)self;
    pit->ResetCalls++;
    pit->Reload = 0;
    pit->Ticks = 0;
}

static INT VddTestPitInit(PVDD_BUS bus, PVOID self)
{
    FAKE_PIT *pit = (FAKE_PIT *)self;
    pit->Bus = bus;
    if (VddClaimPorts(bus, 0x40, 0x43, VddTestPitIn, VddTestPitOut, pit)) return -1;
    if (VddClaimInterrupt(bus, 0x1A, VddTestPitInt1a, pit)) return -1;
    if (VddOnFrame(bus, VddTestPitFrame, pit)) return -1;
    return 0;
}

/* ---- a fake video-ish device: claims a memory window at 0xB8000 ---------- */
static BYTE g_FakeVram[0x8000];
static BYTE VddTestMemoryRead(PVOID self, UINT32 offset)
{
    (VOID)self;
    return g_FakeVram[offset & 0x7FFF];
}

static VOID    VddTestMemoryWrite(PVOID self, UINT32 offset, BYTE value)
{
    (VOID)self;
    g_FakeVram[offset & 0x7FFF] = value;
}

/* ---- host-injected sinks (count effects so the test can assert) ---------- */
static INT  g_IrqCount = 0; static BYTE g_LastIrq = 0xFF;
static VOID VddTestIrqSink(PVOID context, BYTE irq)
{
    (VOID)context;
    g_IrqCount++;
    g_LastIrq = irq;
}

static INT  g_PresentCount = 0; static NTVDD_FRAME g_LastFrame;
static VOID VddTestPresentSink(PVOID context, PCNTVDD_FRAME frame)
{
    (VOID)context;
    g_PresentCount++;
    g_LastFrame = *frame;
}

INT main(VOID)
{
    static BYTE guestMemory[0x100000];      /* stand-in "guest memory" for map_flat */
    VDD_BUS bus;
    FAKE_PIT pit; memset(&pit, 0, sizeof pit);
    NTVDD_DEVICE pitDevice = { "fake-pit", VddTestPitInit, VddTestPitReset, 0, &pit };
    NTVDD_DEVICE videoDevice = { "fake-vid", 0, 0, 0, 0 };
    UINT32 value; BYTE byteValue; NTVDD_REGISTERS registers;

    printf("== M3 slice-1 VDD bus battery ==\n");

    VddBusInitialize(&bus, guestMemory);
    VddBusSetSinks(&bus, VddTestIrqSink, 0, VddTestPresentSink, 0);

    /* T0: add the PIT device -> its init claims ports/int/frame -------------- */
    CHECK(VddBusAdd(&bus, &pitDevice) == 0, "add: fake-pit init ok");
    CHECK(bus.PortCount == 1 && bus.FrameCount == 1, "add: one port range + one frame sub");
    CHECK(bus.Interrupts[0x1A].Service != 0, "add: INT 1Ah claimed");

    /* memory window claimed directly (no device wrapper needed for the test) */
    CHECK(VddClaimMemory(&bus, 0xB8000, sizeof(g_FakeVram), VddTestMemoryRead, VddTestMemoryWrite, &videoDevice) == 0,
          "claim: B8000 window");

    /* T1: I/O OUT then IN round-trips through the owner --------------------- */
    value = 0x12;
    CHECK(VddBusIo(&bus, 0x40, 1, 0, &value) == 1, "io: OUT 0x40 handled");
    CHECK(pit.Reload == 0x12, "io: OUT stored device state");
    value = 0;
    CHECK(VddBusIo(&bus, 0x41, 1, 1, &value) == 1, "io: IN 0x41 handled");
    CHECK(value == 0x12 && pit.LastInPort == 0x41, "io: IN returned device state");

    /* T2: an unclaimed port is not owned ----------------------------------- */
    CHECK(VddBusIo(&bus, 0x3F8, 1, 1, &value) == 0, "io: unclaimed port 0x3F8 -> 0");

    /* T3: memory window read/write routes by offset ------------------------ */
    CHECK(VddBusMemoryWrite(&bus, 0xB8000 + 10, 0xAA) == 1, "mem: write into B8000 window");
    CHECK(g_FakeVram[10] == 0xAA, "mem: write hit device offset 10");
    CHECK(VddBusMemoryRead(&bus, 0xB8000 + 10, &byteValue) == 1 && byteValue == 0xAA, "mem: read back");
    CHECK(VddBusMemoryWrite(&bus, 0xA0000, 0x55) == 0, "mem: outside window -> 0");

    /* T4: a claimed software interrupt is delivered with reg view ---------- */
    memset(&registers, 0, sizeof registers); pit.Ticks = 0x00ABCDEF; VddSetAh(&registers, 0x00);
    CHECK(VddBusDeliverInterrupt(&bus, 0x1A, &registers) == 1, "int: INT 1Ah delivered");
    CHECK(VddGetCx(&registers) == 0x00AB && VddGetDx(&registers) == 0xCDEF && registers.CarryFlag == 0, "int: AH=0 returned tick CX:DX");
    CHECK(VddBusDeliverInterrupt(&bus, 0x21, &registers) == 0, "int: unclaimed INT 21h -> 0");

    /* T5: frame tick fans out -> device advances + raises IRQ0 via sink ----- */
    pit.Ticks = 0; g_IrqCount = 0;
    VddBusFrame(&bus);
    CHECK(pit.Ticks == 1, "frame: tick advanced device");
    CHECK(g_IrqCount == 1 && g_LastIrq == 0, "frame: raised IRQ0 through sink");

    /* T6: map_flat resolves seg:off against the base ----------------------- */
    {
        PBYTE mapped = (PBYTE)VddMapFlat(&bus, 0xB800, 0x000F);
        CHECK(mapped == guestMemory + 0xB800F, "map_flat: seg:off -> base+linear");
    }

    /* T7: present routes a frame to the sink ------------------------------- */
    {
        static const BYTE pixels[4] = {1,2,3,4};
        NTVDD_FRAME frame; memset(&frame, 0, sizeof frame);
        frame.Width = 320; frame.Height = 200; frame.BitsPerPixel = 8; frame.Stride = 320; frame.Pixels = pixels;
        g_PresentCount = 0;
        VddPresent(&bus, &frame);
        CHECK(g_PresentCount == 1 && g_LastFrame.Width == 320 && g_LastFrame.BitsPerPixel == 8,
              "present: frame routed to sink");
    }

    /* T8: reset fans out to devices --------------------------------------- */
    pit.ResetCalls = 0; pit.Reload = 0x99;
    VddBusResetAll(&bus);
    CHECK(pit.ResetCalls == 1 && pit.Reload == 0, "reset: device reset called");

    /* T9: double-claiming an interrupt vector is refused ------------------- */
    CHECK(VddClaimInterrupt(&bus, 0x1A, VddTestPitInt1a, &pit) == -1, "claim: INT 1Ah double-claim refused");

    printf("\n%d checks, %d failed\n", g_Total, g_Failures);
    return g_Failures ? 1 : 0;
}
