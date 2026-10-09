/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Off-VM battery for the empty IDE/ATA adapter (vdd_ide.c). GH #179.
 *
 * [WARNING]: The gap this device closed was a HANG. With nothing on 1F0h-1F7h/3F6h the ISA
 * default FFh reads as BSY=1, and the ATA's own wait -- `test al,80h / jnz` --
 * never exits. See vdd_ide.h and docs/inventory/ide.md.
 *
 * HOW THESE CHECKS WERE WRITTEN:
 * From ATA-3 (X3T13/2008D rev 7b) -- Table 2 note 3 (pull-down on DD7), 8.7.1 (h)
 * (an absent device's status is 00h after reset), 8.7.2's host note (the write-
 * then-read-back presence test) -- and from the three things a DETECTION ROUTINE
 * does: wait for BSY clear, test for a latching register file, issue IDENTIFY and
 * wait for DRQ. `IdeTestHostIn` reproduces the host's unclaimed-port rule (main.c,
 * host_io_do: FFh when no VDD owns the port), so the negative control below is
 * the machine as it was before this device, not an assumption about it.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include <stdio.h>
#include <string.h>
#include "vdd_ide.h"

static INT g_Total = 0;
static INT g_Failures = 0;
#define CHECK(condition, message) do {                                  \
        g_Total++;                                               \
        if (condition) { printf("  PASS  %s\n", (message)); }           \
        else      { printf("  FAIL  %s\n", (message)); g_Failures++; }  \
    } while (0)

static INT g_IrqCount;
static VOID IdeTestIrqSink(PVOID context, BYTE irq)
{
    (VOID)context;
    (VOID)irq;
    g_IrqCount++;
}

/* The host's rule for an IN: a VDD's answer, or FFh(s) when nobody claims it. */
static UINT32 IdeTestHostIn(PVDD_BUS bus, WORD port, BYTE width)
{
    UINT32 value = 0;

    if (!VddBusIo(bus, port, width, 1, &value))
        value = 0xFFFFFFFFu;
    return width == 1 ? (value & 0xFF) : width == 2 ? (value & 0xFFFF) : value;
}

static VOID IdeTestHostOut(PVDD_BUS bus, WORD port, BYTE byteValue)
{
    UINT32 value = byteValue;

    VddBusIo(bus, port, 1, 0, &value);
}

/* The datasheet's BSY wait, bounded. 1 = it would have exited. */
static INT IdeTestBusyWaitExits(PVDD_BUS bus, WORD port)
{
    INT spin;

    for (spin = 0; spin < 65536; ++spin)
        if (!(IdeTestHostIn(bus, port, 1) & IDE_STATUS_BUSY))
            return 1;
    return 0;
}

/* The presence test: write two patterns, read them back. 1 = "a device latched". */
static INT IdeTestLatches(PVDD_BUS bus, WORD commandBase)
{
    IdeTestHostOut(bus, commandBase + 2, 0x55);
    IdeTestHostOut(bus, commandBase + 3, 0xAA);
    if (IdeTestHostIn(bus, commandBase + 2, 1) == 0x55 && IdeTestHostIn(bus, commandBase + 3, 1) == 0xAA)
        return 1;
    IdeTestHostOut(bus, commandBase + 2, 0xAA);
    IdeTestHostOut(bus, commandBase + 3, 0x55);
    return IdeTestHostIn(bus, commandBase + 2, 1) == 0xAA && IdeTestHostIn(bus, commandBase + 3, 1) == 0x55;
}

INT main(VOID)
{
    static const WORD channels[2][2] = { { IDE_PRIMARY_COMMAND, IDE_PRIMARY_CONTROL },
                                         { IDE_SECONDARY_COMMAND, IDE_SECONDARY_CONTROL } };
    VDD_BUS bus;
    IDE_STATE state;
    NTVDD_DEVICE device;
    INT channel;
    INT spin;
    INT sawDataRequest;
    WORD port;

    /* NEGATIVE CONTROL: the machine before this device: */
    VddBusInitialize(&bus, NULL);
    CHECK(IdeTestHostIn(&bus, IDE_PRIMARY_CONTROL, 1) == 0xFF, "control: unclaimed 3F6h floats to FFh");
    CHECK(!IdeTestBusyWaitExits(&bus, IDE_PRIMARY_CONTROL),
          "control: on FFh the BSY wait NEVER exits (the hang this closes)");

    /* the adapter, fitted: */
    memset(&state, 0, sizeof state);
    VddBusInitialize(&bus, NULL);
    VddBusSetSinks(&bus, IdeTestIrqSink, NULL, NULL, NULL);
    device = VddIdeDevice(&state);
    CHECK(VddBusAdd(&bus, &device) == 0 && bus.ClaimFailures == 0, "adapter claims both channels");

    for (channel = 0; channel < 2; ++channel)
    {
        WORD commandPort = channels[channel][0];
        WORD controlPort = channels[channel][1];
        CHAR description[96];
        sprintf(description, "%03Xh: alternate status reads 00h (BSY clear, DD7 pulled down)", controlPort);
        CHECK(IdeTestHostIn(&bus, controlPort, 1) == 0x00, description);
        sprintf(description, "%03Xh: status reads 00h", commandPort + 7);
        CHECK(IdeTestHostIn(&bus, commandPort + 7, 1) == 0x00, description);
        sprintf(description, "%03Xh: the canonical BSY wait exits", controlPort);
        CHECK(IdeTestBusyWaitExits(&bus, controlPort), description);
        sprintf(description, "%03Xh: ...and on the status register too", commandPort + 7);
        CHECK(IdeTestBusyWaitExits(&bus, commandPort + 7), description);
        /* Every task-file register: 0, and BSY clear on all of them. */
        { INT all0 = 1;
          for (port = commandPort + 1; port <= commandPort + 7; ++port)
              if (IdeTestHostIn(&bus, port, 1) != 0)
                  all0 = 0;
          sprintf(description, "%03Xh-%03Xh: every task-file register reads 0", commandPort + 1, commandPort + 7);
          CHECK(all0, description); }
        sprintf(description, "%03Xh: 16-bit data read is 0000h, 32-bit is 0", commandPort);
        CHECK(IdeTestHostIn(&bus, commandPort, 2) == 0 && IdeTestHostIn(&bus, commandPort, 4) == 0, description);
        /* [WARNING]: the presence test must NOT find a drive */
        sprintf(description, "%03Xh: 55h/AAh write-read-back does not echo (no false drive)", commandPort + 2);
        CHECK(!IdeTestLatches(&bus, commandPort), description);
        /* device 1 selected: ATA-3 8.7.1(h) -- an absent device's status is 00h */
        IdeTestHostOut(&bus, commandPort + 6, 0xB0);
        sprintf(description, "%03Xh: device 1 selected, status 00h", controlPort);
        CHECK(IdeTestHostIn(&bus, controlPort, 1) == 0x00, description);
        IdeTestHostOut(&bus, commandPort + 6, 0xA0);
        /* SRST through device control: still nothing busy afterwards */
        IdeTestHostOut(&bus, controlPort, 0x04);
        IdeTestHostOut(&bus, controlPort, 0x00);
        sprintf(description, "%03Xh: after SRST set+clear, BSY still clear", controlPort);
        CHECK(!(IdeTestHostIn(&bus, controlPort, 1) & IDE_STATUS_BUSY), description);
        /* IDENTIFY: nobody receives it -- no DRQ, no IRQ, no error to read. */
        g_IrqCount = 0;
        IdeTestHostOut(&bus, commandPort + 7, 0xEC);
        sawDataRequest = 0;
        for (spin = 0; spin < 1000; ++spin)
            if (IdeTestHostIn(&bus, controlPort, 1) & IDE_STATUS_DATA_REQUEST)
                sawDataRequest = 1;
        sprintf(description, "%03Xh: IDENTIFY to an empty channel never raises DRQ", commandPort + 7);
        CHECK(!sawDataRequest, description);
        sprintf(description, "%03Xh: ...and raises no interrupt", commandPort + 7);
        CHECK(g_IrqCount == 0, description);
        sprintf(description, "%03Xh: ...and the Error register is still 0", commandPort + 1);
        CHECK(IdeTestHostIn(&bus, commandPort + 1, 1) == 0x00, description);
    }

    /* the edges of the claim: */
    CHECK(IdeTestHostIn(&bus, 0x3F7, 1) == 0xFF, "3F7h is NOT ours -- left for the FDC (DIR)");
    CHECK(IdeTestHostIn(&bus, 0x377, 1) == 0x00, "377h, the secondary drive-address register, is");
    CHECK(IdeTestHostIn(&bus, 0x1EF, 1) == 0xFF && IdeTestHostIn(&bus, 0x1F8, 1) == 0xFF,
          "1EFh and 1F8h are outside the primary block");
    CHECK(IdeTestHostIn(&bus, 0x3F5, 1) == 0xFF, "3F5h (the FDC's FIFO) is not claimed by us");
    CHECK(state.Commands == 2 && state.LastCommand == 0xEC, "diagnostics: two commands seen, last ECh");

    printf("\n%d checks, %d failed\n", g_Total, g_Failures);
    return g_Failures ? 1 : 0;
}
