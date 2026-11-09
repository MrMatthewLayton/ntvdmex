/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Off-VM unit battery for the PS/2 pointing device (vdd_aux.c, #199): its command set,
 * replies and packets, and the INT 15h AX=C2xxh BIOS interface over it. The 8042 side
 * (D4h, AUXB, IRQ12) is in input_test.c.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include <stdio.h>
#include <string.h>
#include "vdd_aux.h"
#define CHECK(condition,message) do{ g_Total++; if(condition){printf("  PASS  %s\n",(message));} \
    else{printf("  FAIL  %s\n",(message)); g_Failures++;} }while(0)

static INT g_Total = 0;
static INT g_Failures = 0;

/* Write one byte, then take exactly `count` replies into `out`; 1 if exactly that many came. */
static INT AuxTestSend(PAUX_DEVICE device, BYTE value, BYTE *out, INT count)
{
    INT index;

    AuxDeviceWrite(device, value);

    if (AuxDeviceQueued(device) != count)
        return 0;

    for (index = 0; index < count; ++index)
        out[index] = AuxDevicePop(device);

    return 1;
}

INT main(VOID)
{
    AUX_DEVICE device;
    AUX_BIOS bios;
    AUX_BIOS_RESULT result;
    BYTE reply[4];
    BYTE packet[AUX_PACKET_BYTES];

    printf("== the PS/2 pointing device (#199) ==\n");
    AuxDeviceReset(&device);
    CHECK(AuxDeviceQueued(&device) == 0 && !AuxDeviceIsStreaming(&device), "power-on: quiet, not streaming");

    /* T1: the reset and the ID */
    CHECK(AuxTestSend(&device, AUX_COMMAND_RESET, reply, 3) && reply[0] == 0xFA && reply[1] == 0xAA && reply[2] == 0x00,
          "FFh reset -> FA AA 00");
    CHECK(AuxTestSend(&device, AUX_COMMAND_GET_DEVICE_ID, reply, 2) && reply[0] == 0xFA && reply[1] == 0x00,
          "F2h get ID -> FA 00 (a standard mouse)");

    /* T2: parameters, each byte ACKed */
    CHECK(AuxTestSend(&device, AUX_COMMAND_SET_SAMPLE_RATE, reply, 1) && reply[0] == 0xFA, "F3h -> FA");
    CHECK(AuxTestSend(&device, 40, reply, 1) && reply[0] == 0xFA && device.SampleRate == 40, "F3h's rate -> FA, kept");
    CHECK(AuxTestSend(&device, AUX_COMMAND_SET_RESOLUTION, reply, 1) && AuxTestSend(&device, 3, reply, 1)
          && device.Resolution == 3, "E8h + 3 -> resolution 3");
    CHECK(AuxTestSend(&device, AUX_COMMAND_SET_SCALING_2_1, reply, 1) && device.IsScaling21, "E7h -> scaling 2:1");

    /* T3: E9h status request */
    CHECK(AuxTestSend(&device, AUX_COMMAND_STATUS_REQUEST, reply, 4) && reply[0] == 0xFA
          && reply[1] == AUX_STATUS_SCALING_2_1 && reply[2] == 3 && reply[3] == 40,
          "E9h -> FA, status (scaling 2:1), resolution, rate");
    CHECK(AuxTestSend(&device, AUX_COMMAND_SET_DEFAULTS, reply, 1) && device.SampleRate == 100
          && device.Resolution == 2 && !device.IsScaling21, "F6h -> the defaults (100/s, 4 counts/mm, 1:1)");

    /* T4: streaming */
    CHECK(AuxDeviceMove(&device, 5, 5, 0) == 0 && AuxDeviceQueued(&device) == 0, "movement before F4h: nothing sent");
    CHECK(AuxTestSend(&device, AUX_COMMAND_ENABLE_REPORTING, reply, 1) && AuxDeviceIsStreaming(&device), "F4h -> FA, streaming");
    CHECK(AuxDeviceMove(&device, 3, -2, AUX_PACKET_LEFT) == 1 && AuxDeviceQueued(&device) == 3, "a move -> one 3-byte packet");
    packet[0] = AuxDevicePop(&device);
    packet[1] = AuxDevicePop(&device);
    packet[2] = AuxDevicePop(&device);
    CHECK(packet[0] == (AUX_PACKET_ALWAYS_ONE | AUX_PACKET_LEFT) && packet[1] == 3 && packet[2] == 2,
          "packet: left button, bit 3, X +3, Y +2 (screen up = device up)");
    CHECK(AuxDeviceMove(&device, 0, 0, AUX_PACKET_LEFT) == 0, "no movement, same buttons: no packet");
    CHECK(AuxDeviceMove(&device, 0, 0, 0) == 1, "a button released: a packet");
    (VOID)AuxDevicePop(&device);
    (VOID)AuxDevicePop(&device);
    (VOID)AuxDevicePop(&device);
    CHECK(AuxTestSend(&device, AUX_COMMAND_DISABLE_REPORTING, reply, 1) && !AuxDeviceIsStreaming(&device), "F5h -> FA, quiet");

    /* T5: the packet's signs and overflow */
    AuxDevicePacket(-1, 1, AUX_PACKET_RIGHT, packet);
    CHECK(packet[0] == (AUX_PACKET_ALWAYS_ONE | AUX_PACKET_RIGHT | AUX_PACKET_X_SIGN | AUX_PACKET_Y_SIGN)
          && packet[1] == 0xFF && packet[2] == 0xFF, "packet: -1/-1 are sign + FFh, right button");
    AuxDevicePacket(400, -400, 0, packet);
    CHECK((packet[0] & (AUX_PACKET_X_OVERFLOW | AUX_PACKET_Y_OVERFLOW)) == (AUX_PACKET_X_OVERFLOW | AUX_PACKET_Y_OVERFLOW)
          && packet[1] == 0xFF && packet[2] == 0xFF, "packet: past 255 sets the overflow bits and clamps");

    /* T6: remote mode, EBh, wrap, resend, an unknown byte */
    CHECK(AuxTestSend(&device, AUX_COMMAND_SET_REMOTE_MODE, reply, 1), "F0h -> FA");
    (VOID)AuxDeviceMove(&device, 4, 0, 0);
    (VOID)AuxDeviceMove(&device, 2, 0, 0);
    CHECK(AuxDeviceQueued(&device) == 0, "remote mode: movement is held, not streamed");
    CHECK(AuxTestSend(&device, AUX_COMMAND_READ_DATA, reply, 4) && reply[0] == 0xFA && reply[2] == 6,
          "EBh -> FA + the packet of all the held movement (X 6)");
    CHECK(AuxTestSend(&device, AUX_COMMAND_SET_STREAM_MODE, reply, 1), "EAh -> FA");
    CHECK(AuxTestSend(&device, AUX_COMMAND_SET_WRAP_MODE, reply, 1) && AuxTestSend(&device, 0x55, reply, 1)
          && reply[0] == 0x55, "EEh wrap mode echoes a byte");
    CHECK(AuxTestSend(&device, AUX_COMMAND_RESET_WRAP_MODE, reply, 1) && reply[0] == 0xFA && !device.IsWrap, "ECh leaves wrap mode");
    CHECK(AuxTestSend(&device, AUX_COMMAND_RESEND, reply, 1) && reply[0] == 0xFA, "FEh resends the last reply");
    CHECK(AuxTestSend(&device, 0x12, reply, 1) && reply[0] == 0xFE, "a byte that is no command -> FEh");

    /* T7: INT 15h AX=C2xxh */
    printf("== INT 15h AX=C2xxh ==\n");
    AuxDeviceReset(&device);
    memset(&bios, 0, sizeof bios);
    AuxBiosService(&device, &bios, AUX_BIOS_INITIALIZE, 3, 0, 0, &result);
    CHECK(result.Status == AUX_BIOS_OK && bios.PacketSize == 3, "C205h BH=3: initialised, 3-byte packets");
    AuxBiosService(&device, &bios, AUX_BIOS_INITIALIZE, 9, 0, 0, &result);
    CHECK(result.Status == AUX_BIOS_INVALID_INPUT, "C205h BH=9 -> AH=02 (invalid input)");
    AuxBiosService(&device, &bios, AUX_BIOS_ENABLE, 1, 0, 0, &result);
    CHECK(result.Status == AUX_BIOS_NO_HANDLER && !bios.IsEnabled, "C200h enable with no handler -> AH=05");
    AuxBiosService(&device, &bios, AUX_BIOS_SET_HANDLER, 0, 0x1234, 0x0010, &result);
    CHECK(result.Status == AUX_BIOS_OK && bios.HandlerSegment == 0x1234 && bios.HandlerOffset == 0x0010, "C207h ES:BX kept");
    AuxBiosService(&device, &bios, AUX_BIOS_ENABLE, 1, 0, 0, &result);
    CHECK(result.Status == AUX_BIOS_OK && AuxBiosIsActive(&bios) && device.IsReporting, "C200h BH=1: enabled, the device reports");
    AuxBiosService(&device, &bios, AUX_BIOS_GET_TYPE, 0, 0, 0, &result);
    CHECK(result.Outputs == AUX_BIOS_SET_BH && result.Bh == 0x00, "C204h -> BH=00");
    AuxBiosService(&device, &bios, AUX_BIOS_SET_SAMPLE_RATE, 2, 0, 0, &result);
    CHECK(result.Status == AUX_BIOS_OK && device.SampleRate == 40, "C202h BH=2 -> 40 reports/s");
    AuxBiosService(&device, &bios, AUX_BIOS_SET_SAMPLE_RATE, 7, 0, 0, &result);
    CHECK(result.Status == AUX_BIOS_INVALID_INPUT, "C202h BH=7 -> AH=02");
    AuxBiosService(&device, &bios, AUX_BIOS_SET_RESOLUTION, 1, 0, 0, &result);
    CHECK(result.Status == AUX_BIOS_OK && device.Resolution == 1, "C203h BH=1");
    AuxBiosService(&device, &bios, AUX_BIOS_EXTENDED, 2, 0, 0, &result);
    AuxBiosService(&device, &bios, AUX_BIOS_EXTENDED, 0, 0, 0, &result);
    CHECK(result.Outputs == (AUX_BIOS_SET_BL | AUX_BIOS_SET_CL | AUX_BIOS_SET_DL)
          && result.Bl == (AUX_STATUS_SCALING_2_1 | AUX_STATUS_REPORTING) && result.Cl == 1 && result.Dl == 40
          && AuxDeviceQueued(&device) == 0, "C206h BH=0 -> BL status, CL resolution, DL rate (nothing left queued)");
    AuxBiosService(&device, &bios, AUX_BIOS_RESET, 0, 0, 0, &result);
    CHECK(result.Bh == 0x00 && result.Bl == 0xAA && !bios.IsEnabled && device.SampleRate == 100,
          "C201h -> BH=00 BL=AA, disabled, defaults");
    AuxBiosService(&device, &bios, 0x08, 0, 0, 0, &result);
    CHECK(result.Status == AUX_BIOS_INVALID_FUNCTION, "C208h -> AH=01 (invalid function)");
    AuxBiosService(&device, &bios, AUX_BIOS_SET_HANDLER, 0, 0, 0, &result);
    CHECK(!AuxBiosIsActive(&bios), "C207h 0:0 removes the handler");

    /* T8: MOUSE.COM 6.24A's init (from the binary, 0x353A), twice -- QB resets its INT 33h
     * after a DOS Shell, and the driver re-runs the sequence (s101). Idempotent.
     */
    printf("== a guest driver's init, run twice ==\n");
    AuxDeviceReset(&device);
    memset(&bios, 0, sizeof bios);
    CHECK(!AuxGuestOwnsMouse(&bios, 0), "power-on: no guest driver owns the mouse");
    {
        INT pass;

        for (pass = 0; pass < 2; ++pass)
        {
            AuxBiosService(&device, &bios, AUX_BIOS_INITIALIZE, 3, 0, 0, &result);
            AuxBiosService(&device, &bios, AUX_BIOS_RESET, 0, 0, 0, &result);
            AuxBiosService(&device, &bios, AUX_BIOS_SET_RESOLUTION, 3, 0, 0, &result);
            AuxBiosService(&device, &bios, AUX_BIOS_SET_HANDLER, 0, 0x471E, 0x0123, &result);
            AuxBiosService(&device, &bios, AUX_BIOS_ENABLE, 1, 0, 0, &result);
            AuxBiosService(&device, &bios, AUX_BIOS_EXTENDED, 1, 0, 0, &result);
            AuxBiosService(&device, &bios, AUX_BIOS_SET_SAMPLE_RATE, 3, 0, 0, &result);
            CHECK(result.Status == AUX_BIOS_OK && AuxBiosIsActive(&bios) && device.IsReporting
                  && bios.HandlerSegment == 0x471E && AuxGuestOwnsMouse(&bios, 0),
                  pass ? "the same init again: still enabled, same handler, owns the mouse"
                       : "C205h C201h C203h C207h C200h C206h C202h: enabled, owns the mouse");
        }
    }
    AuxBiosService(&device, &bios, AUX_BIOS_ENABLE, 0, 0, 0, &result);
    CHECK(!AuxGuestOwnsMouse(&bios, 0), "C200h BH=0: disabled, no longer owns it");
    CHECK(AuxGuestOwnsMouse(&bios, 1), "a driver streaming through the 8042 owns it too");

    printf("\n== %d checks, %d failed\n", g_Total, g_Failures);
    return g_Failures ? 1 : 0;
}
