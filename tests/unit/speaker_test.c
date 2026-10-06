/* speaker_test.c -- off-VM unit battery for the PC-speaker VDD (vdd_speaker.c).
 *
 * The third device on the bus, proving the VDD ABI generalises. The speaker is
 * PIT channel 2 (tone) gated by port 0x61. We program channel 2 through the PIT
 * VDD, drive port 0x61 through the speaker VDD, and check the reported tone +
 * active state -- entirely off-VM, like the other batteries. No audio is
 * produced (that's M7); this validates the device model + the port plumbing.
 */
#include <stdio.h>
#include <string.h>
#include "vdd_speaker.h"

static int total = 0, fails = 0;
#define CHECK(c,m) do{ total++; if(c){printf("  PASS  %s\n",(m));} \
    else{printf("  FAIL  %s\n",(m)); fails++;} }while(0)

static uint8_t g_flat[0x100000];

int main(void)
{
    VDD_BUS bus;
    pit_state pit; memset(&pit, 0, sizeof pit);
    SPEAKER_STATE spk; memset(&spk, 0, sizeof spk); spk.Pit = &pit;
    NTVDD_DEVICE pdev = vdd_pit_device(&pit);
    NTVDD_DEVICE sdev = VddSpeakerDevice(&spk);
    uint32_t v;

    printf("== M3 PC-speaker VDD battery ==\n");

    VddBusInitialize(&bus, g_flat);
    CHECK(VddBusAdd(&bus, &pdev) == 0, "add: pit ok");
    CHECK(VddBusAdd(&bus, &sdev) == 0, "add: speaker ok (3rd device on the bus)");
    CHECK(bus.Ports[bus.PortCount - 1].First == 0x61, "add: speaker claimed port 0x61");

    /* T1: off by default ------------------------------------------------- */
    CHECK(!VddSpeakerIsActive(&spk), "init: speaker inactive");

    /* T2: program PIT channel 2 to 1000 Hz (reload = 1193182/1000 = 1193) -- *
     * 0x43 = 10 11 011 0 = 0xB6 (ch2, lo/hi, mode 3); then lo, hi of 1193.   */
    v = 0xB6; VddBusIo(&bus, 0x43, 1, 0, &v);
    v = 1193 & 0xFF;  VddBusIo(&bus, 0x42, 1, 0, &v);     /* lo */
    v = 1193 >> 8;    VddBusIo(&bus, 0x42, 1, 0, &v);     /* hi -> reload 1193 */
    CHECK(pit.ch2_reload == 1193, "ch2: reload latched (lo/hi) = 1193");
    CHECK(pit_ch2_hz(&pit) == PIT_INPUT_HZ / 1193, "ch2: ~1000 Hz tone");

    /* T3: enabling gate+data (port 0x61 bits 0+1) turns the speaker on ---- */
    v = 0x03; VddBusIo(&bus, 0x61, 1, 0, &v);
    CHECK(VddSpeakerIsActive(&spk), "0x61=3: speaker active");
    CHECK(VddSpeakerHz(&spk) == PIT_INPUT_HZ / 1193, "active: reports the ch2 tone");

    /* T4: only one of the two bits => not active ------------------------- */
    v = 0x01; VddBusIo(&bus, 0x61, 1, 0, &v);
    CHECK(!VddSpeakerIsActive(&spk), "0x61=1 (gate only): inactive");
    v = 0x02; VddBusIo(&bus, 0x61, 1, 0, &v);
    CHECK(!VddSpeakerIsActive(&spk), "0x61=2 (data only): inactive");

    /* T5: turn off ------------------------------------------------------- */
    v = 0x00; VddBusIo(&bus, 0x61, 1, 0, &v);
    CHECK(!VddSpeakerIsActive(&spk), "0x61=0: speaker off");

    /* T6: reads echo the control bits and toggle the refresh bit (bit 4) -- */
    v = 0x03; VddBusIo(&bus, 0x61, 1, 0, &v);
    { uint32_t a = 0, b = 0;
      VddBusIo(&bus, 0x61, 1, 1, &a);
      VddBusIo(&bus, 0x61, 1, 1, &b);
      CHECK((a & 0x03) == 0x03 && (b & 0x03) == 0x03, "in 0x61: control bits echoed");
      CHECK((a & 0x10) != (b & 0x10), "in 0x61: refresh bit 4 toggles between reads"); }

    printf("\n%d checks, %d failed\n", total, fails);
    return fails ? 1 : 0;
}
