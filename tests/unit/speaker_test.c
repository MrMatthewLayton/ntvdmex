/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Off-VM unit battery for the PC-speaker VDD (vdd_speaker.c).
 *
 * The third device on the bus, proving the VDD ABI generalises. The speaker is
 * PIT channel 2 (tone) gated by port 0x61. We program channel 2 through the PIT
 * VDD, drive port 0x61 through the speaker VDD, and check the reported tone +
 * active state -- entirely off-VM, like the other batteries. No audio is
 * produced (that's M7); this validates the device model + the port plumbing.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include <stdio.h>
#include <string.h>
#include "vdd_speaker.h"

static INT g_Total = 0;
static INT g_Failures = 0;
#define CHECK(condition,message) do{ g_Total++; if(condition){printf("  PASS  %s\n",(message));} \
    else{printf("  FAIL  %s\n",(message)); g_Failures++;} }while(0)

static BYTE g_GuestMemory[0x100000];

INT main(VOID)
{
    VDD_BUS bus;
    PIT_STATE pit;

    memset(&pit, 0, sizeof pit);
    SPEAKER_STATE spk;
    memset(&spk, 0, sizeof spk);
    spk.Pit = &pit;
    NTVDD_DEVICE pdev = VddPitDevice(&pit);
    NTVDD_DEVICE sdev = VddSpeakerDevice(&spk);
    UINT32 value;

    printf("== M3 PC-speaker VDD battery ==\n");

    VddBusInitialize(&bus, g_GuestMemory);
    CHECK(VddBusAdd(&bus, &pdev) == 0, "add: pit ok");
    CHECK(VddBusAdd(&bus, &sdev) == 0, "add: speaker ok (3rd device on the bus)");
    CHECK(bus.Ports[bus.PortCount - 1].First == 0x61, "add: speaker claimed port 0x61");

    /* T1: off by default ------------------------------------------------- */
    CHECK(!VddSpeakerIsActive(&spk), "init: speaker inactive");

    /* T2: program PIT channel 2 to 1000 Hz (reload = 1193182/1000 = 1193) -- *
     * 0x43 = 10 11 011 0 = 0xB6 (ch2, lo/hi, mode 3); then lo, hi of 1193.
     */
    value = 0xB6;
    VddBusIo(&bus, 0x43, 1, 0, &value);
    value = 1193 & 0xFF;
    VddBusIo(&bus, 0x42, 1, 0, &value);     /* lo */
    value = 1193 >> 8;
    VddBusIo(&bus, 0x42, 1, 0, &value);     /* hi -> reload 1193 */
    CHECK(pit.Counter2Reload == 1193, "ch2: reload latched (lo/hi) = 1193");
    CHECK(VddPitCounter2Hz(&pit) == PIT_INPUT_HZ / 1193, "ch2: ~1000 Hz tone");

    /* T3: enabling gate+data (port 0x61 bits 0+1) turns the speaker on ---- */
    value = 0x03;
    VddBusIo(&bus, 0x61, 1, 0, &value);
    CHECK(VddSpeakerIsActive(&spk), "0x61=3: speaker active");
    CHECK(VddSpeakerHz(&spk) == PIT_INPUT_HZ / 1193, "active: reports the ch2 tone");

    /* T4: only one of the two bits => not active ------------------------- */
    value = 0x01;
    VddBusIo(&bus, 0x61, 1, 0, &value);
    CHECK(!VddSpeakerIsActive(&spk), "0x61=1 (gate only): inactive");
    value = 0x02;
    VddBusIo(&bus, 0x61, 1, 0, &value);
    CHECK(!VddSpeakerIsActive(&spk), "0x61=2 (data only): inactive");

    /* T5: turn off ------------------------------------------------------- */
    value = 0x00;
    VddBusIo(&bus, 0x61, 1, 0, &value);
    CHECK(!VddSpeakerIsActive(&spk), "0x61=0: speaker off");

    /* T6: reads echo the control bits and toggle the refresh bit (bit 4) -- */
    value = 0x03;
    VddBusIo(&bus, 0x61, 1, 0, &value);
    { UINT32 firstRead = 0, secondRead = 0;
      VddBusIo(&bus, 0x61, 1, 1, &firstRead);
      VddBusIo(&bus, 0x61, 1, 1, &secondRead);
      CHECK((firstRead & 0x03) == 0x03 && (secondRead & 0x03) == 0x03, "in 0x61: control bits echoed");
      CHECK((firstRead & 0x10) != (secondRead & 0x10), "in 0x61: refresh bit 4 toggles between reads"); }

    printf("\n%d checks, %d failed\n", g_Total, g_Failures);
    return g_Failures ? 1 : 0;
}
