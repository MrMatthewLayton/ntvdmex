/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * See vdd_aux.h. The PS/2 pointing device's command set, replies and packets.
 *
 * No Windows calls, only Windows types.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include "vdd_aux.h"

#define AUX_RESOLUTION_MAX  3
#define AUX_BYTE_MASK       0xFF

/* C202h's BH, as reports per second (RBIL INT 15/AX=C202). */
static const BYTE g_AuxBiosSampleRates[AUX_BIOS_RATE_CHOICES] = { 10, 20, 40, 60, 80, 100, 200 };

static INT AuxNext(INT index)
{
    return (index + 1) % AUX_QUEUE_SIZE;
}

static VOID AuxPush(PAUX_DEVICE device, BYTE value)
{
    INT next = AuxNext(device->Head);

    if (next == device->Tail)                 /* full: the device cannot send, the byte is lost */
    {
        ++device->Dropped;
        return;
    }

    device->Queue[device->Head] = value;
    device->Head = next;
}

/* A reply to a command: remembered for FEh (resend), unless it IS the resend request. */
static VOID AuxReply(PAUX_DEVICE device, BYTE value)
{
    if (value != AUX_REPLY_RESEND)
        device->LastReply = value;

    AuxPush(device, value);
}

static VOID AuxDefaults(PAUX_DEVICE device)
{
    device->IsReporting = 0;
    device->IsRemote = 0;
    device->IsScaling21 = 0;
    device->Resolution = AUX_DEFAULT_RESOLUTION;
    device->SampleRate = AUX_DEFAULT_SAMPLE_RATE;
    device->RemoteDx = 0;
    device->RemoteDy = 0;
}

VOID AuxDeviceReset(PAUX_DEVICE device)
{
    BYTE *bytes = (BYTE *)device;
    UINT index;

    for (index = 0; index < sizeof *device; ++index)
        bytes[index] = 0;

    AuxDefaults(device);
}

static LONG AuxClamp(LONG value, BYTE *overflow, BYTE overflowBit)
{
    if (value > AUX_MOVEMENT_MAX)
    {
        *overflow |= overflowBit;
        return AUX_MOVEMENT_MAX;
    }

    if (value < -(AUX_MOVEMENT_MAX + 1))
    {
        *overflow |= overflowBit;
        return -(AUX_MOVEMENT_MAX + 1);
    }

    return value;
}

VOID AuxDevicePacket(LONG dx, LONG dy, BYTE buttons, BYTE *packet)
{
    BYTE first = (BYTE)(AUX_PACKET_ALWAYS_ONE | (buttons & (AUX_PACKET_LEFT | AUX_PACKET_RIGHT | AUX_PACKET_MIDDLE)));
    LONG x = AuxClamp(dx, &first, AUX_PACKET_X_OVERFLOW);
    LONG y = AuxClamp(-dy, &first, AUX_PACKET_Y_OVERFLOW);   /* the device's Y is up-positive */

    if (x < 0)
        first |= AUX_PACKET_X_SIGN;

    if (y < 0)
        first |= AUX_PACKET_Y_SIGN;

    packet[0] = first;
    packet[1] = (BYTE)(x & AUX_BYTE_MASK);
    packet[2] = (BYTE)(y & AUX_BYTE_MASK);
}

static VOID AuxQueuePacket(PAUX_DEVICE device, LONG dx, LONG dy)
{
    BYTE packet[AUX_PACKET_BYTES];
    INT index;

    AuxDevicePacket(dx, dy, device->Buttons, packet);

    for (index = 0; index < AUX_PACKET_BYTES; ++index)
        AuxPush(device, packet[index]);

    ++device->Packets;
}

VOID AuxDeviceWrite(PAUX_DEVICE device, BYTE value)
{
    ++device->Commands;

    /* WRAP MODE echoes every byte back -- except its two ways out. */
    if (device->IsWrap && value != AUX_COMMAND_RESET_WRAP_MODE && value != AUX_COMMAND_RESET)
    {
        AuxPush(device, value);
        return;
    }

    if (device->PendingCommand)                                     /* a parameter byte */
    {
        if (device->PendingCommand == AUX_COMMAND_SET_RESOLUTION)
            device->Resolution = (BYTE)(value <= AUX_RESOLUTION_MAX ? value : AUX_RESOLUTION_MAX);
        else
            device->SampleRate = value;

        device->PendingCommand = 0;
        AuxReply(device, AUX_REPLY_ACK);
        return;
    }

    switch (value)
    {
    case AUX_COMMAND_RESET:                                          /* ACK, AAh, ID */
        AuxDefaults(device);
        device->IsWrap = 0;
        device->Buttons = 0;
        AuxReply(device, AUX_REPLY_ACK);
        AuxReply(device, AUX_REPLY_SELF_TEST_PASSED);
        AuxReply(device, AUX_DEVICE_ID_STANDARD);
        break;

    case AUX_COMMAND_RESEND:
        AuxPush(device, device->LastReply);
        break;

    case AUX_COMMAND_SET_DEFAULTS:
        AuxDefaults(device);
        AuxReply(device, AUX_REPLY_ACK);
        break;

    case AUX_COMMAND_DISABLE_REPORTING:
        device->IsReporting = 0;
        AuxReply(device, AUX_REPLY_ACK);
        break;

    case AUX_COMMAND_ENABLE_REPORTING:
        device->IsReporting = 1;
        AuxReply(device, AUX_REPLY_ACK);
        break;

    case AUX_COMMAND_SET_SAMPLE_RATE:
    case AUX_COMMAND_SET_RESOLUTION:
        device->PendingCommand = value;
        AuxReply(device, AUX_REPLY_ACK);
        break;

    case AUX_COMMAND_GET_DEVICE_ID:
        AuxReply(device, AUX_REPLY_ACK);
        AuxReply(device, AUX_DEVICE_ID_STANDARD);
        break;

    case AUX_COMMAND_SET_REMOTE_MODE:
        device->IsRemote = 1;
        device->RemoteDx = 0;
        device->RemoteDy = 0;
        AuxReply(device, AUX_REPLY_ACK);
        break;

    case AUX_COMMAND_SET_WRAP_MODE:
        device->IsWrap = 1;
        AuxReply(device, AUX_REPLY_ACK);
        break;

    case AUX_COMMAND_RESET_WRAP_MODE:
        device->IsWrap = 0;
        AuxReply(device, AUX_REPLY_ACK);
        break;

    case AUX_COMMAND_READ_DATA:                                       /* ACK, then a packet */
        AuxReply(device, AUX_REPLY_ACK);
        AuxQueuePacket(device, device->RemoteDx, device->RemoteDy);
        device->RemoteDx = 0;
        device->RemoteDy = 0;
        break;

    case AUX_COMMAND_SET_STREAM_MODE:
        device->IsRemote = 0;
        AuxReply(device, AUX_REPLY_ACK);
        break;

    case AUX_COMMAND_STATUS_REQUEST:                                  /* ACK, status, resolution, rate */
    {
        BYTE status = 0;

        if (device->Buttons & AUX_PACKET_RIGHT)
            status |= AUX_STATUS_RIGHT;

        if (device->Buttons & AUX_PACKET_MIDDLE)
            status |= AUX_STATUS_MIDDLE;

        if (device->Buttons & AUX_PACKET_LEFT)
            status |= AUX_STATUS_LEFT;

        if (device->IsScaling21)
            status |= AUX_STATUS_SCALING_2_1;

        if (device->IsReporting)
            status |= AUX_STATUS_REPORTING;

        if (device->IsRemote)
            status |= AUX_STATUS_REMOTE;

        AuxReply(device, AUX_REPLY_ACK);
        AuxReply(device, status);
        AuxReply(device, device->Resolution);
        AuxReply(device, device->SampleRate);
        break;
    }

    case AUX_COMMAND_SET_SCALING_2_1:
        device->IsScaling21 = 1;
        AuxReply(device, AUX_REPLY_ACK);
        break;

    case AUX_COMMAND_SET_SCALING_1_1:
        device->IsScaling21 = 0;
        AuxReply(device, AUX_REPLY_ACK);
        break;

    default:                                                          /* not a command: ask again */
        AuxReply(device, AUX_REPLY_RESEND);
        break;
    }
}

INT AuxDeviceMove(PAUX_DEVICE device, LONG dx, LONG dy, BYTE buttons)
{
    BYTE newButtons = (BYTE)(buttons & (AUX_PACKET_LEFT | AUX_PACKET_RIGHT | AUX_PACKET_MIDDLE));
    INT changed = (dx || dy || newButtons != device->Buttons);

    device->Buttons = newButtons;

    if (device->IsRemote)
    {
        device->RemoteDx += dx;
        device->RemoteDy += dy;
        return 0;
    }

    if (!device->IsReporting || device->IsWrap || !changed)
        return 0;

    AuxQueuePacket(device, dx, dy);
    return 1;
}

INT AuxDeviceQueued(PCAUX_DEVICE device)
{
    INT depth = device->Head - device->Tail;

    return depth < 0 ? depth + AUX_QUEUE_SIZE : depth;
}

BYTE AuxDevicePeek(PCAUX_DEVICE device)
{
    return (device->Head == device->Tail) ? 0 : device->Queue[device->Tail];
}

BYTE AuxDevicePop(PAUX_DEVICE device)
{
    BYTE value;

    if (device->Head == device->Tail)
        return 0;

    value = device->Queue[device->Tail];
    device->Tail = AuxNext(device->Tail);
    return value;
}

INT AuxDeviceIsStreaming(PCAUX_DEVICE device)
{
    return device->IsReporting && !device->IsRemote && !device->IsWrap;
}

static VOID AuxBiosDefaults(PAUX_DEVICE device, PAUX_BIOS bios)
{
    AuxDefaults(device);              /* the device's own: 100/s, 4 counts/mm, 1:1, quiet */
    device->IsWrap = 0;
    bios->IsEnabled = 0;
}

/* C206h BH=0: the device's own E9h answer (status, resolution, rate), asked of a copy so
 * nothing lands in the real device's output queue.
 */
static VOID AuxBiosStatus(PCAUX_DEVICE device, PAUX_BIOS_RESULT result)
{
    AUX_DEVICE probe = *device;

    probe.Head = probe.Tail = 0;
    AuxDeviceWrite(&probe, AUX_COMMAND_STATUS_REQUEST);
    (VOID)AuxDevicePop(&probe);                                /* the ACK */
    result->Outputs = AUX_BIOS_SET_BL | AUX_BIOS_SET_CL | AUX_BIOS_SET_DL;
    result->Bl = AuxDevicePop(&probe);
    result->Cl = AuxDevicePop(&probe);
    result->Dl = AuxDevicePop(&probe);
}

VOID AuxBiosService(
    PAUX_DEVICE device,
    PAUX_BIOS bios,
    BYTE function,
    BYTE bh,
    WORD es,
    WORD bx,
    PAUX_BIOS_RESULT result)
{
    result->Status = AUX_BIOS_OK;
    result->Outputs = 0;
    result->Bh = result->Bl = result->Cl = result->Dl = 0;
    ++bios->Calls;

    switch (function)
    {
    case AUX_BIOS_ENABLE:
        if (bh > 1)
            result->Status = AUX_BIOS_INVALID_INPUT;
        else if (bh && !bios->HandlerSegment && !bios->HandlerOffset)
            result->Status = AUX_BIOS_NO_HANDLER;           /* nothing to call with the packets */
        else
        {
            bios->IsEnabled = bh;
            device->IsReporting = bh;
        }

        break;

    case AUX_BIOS_RESET:
        AuxBiosDefaults(device, bios);
        result->Outputs = AUX_BIOS_SET_BH | AUX_BIOS_SET_BL;
        result->Bh = AUX_DEVICE_ID_STANDARD;
        result->Bl = AUX_REPLY_SELF_TEST_PASSED;
        break;

    case AUX_BIOS_SET_SAMPLE_RATE:
        if (bh >= AUX_BIOS_RATE_CHOICES)
            result->Status = AUX_BIOS_INVALID_INPUT;
        else
            device->SampleRate = g_AuxBiosSampleRates[bh];

        break;

    case AUX_BIOS_SET_RESOLUTION:
        if (bh > AUX_RESOLUTION_MAX)
            result->Status = AUX_BIOS_INVALID_INPUT;
        else
            device->Resolution = bh;

        break;

    case AUX_BIOS_GET_TYPE:
        result->Outputs = AUX_BIOS_SET_BH;
        result->Bh = AUX_DEVICE_ID_STANDARD;
        break;

    case AUX_BIOS_INITIALIZE:
        if (bh < AUX_BIOS_PACKET_SIZE_MIN || bh > AUX_BIOS_PACKET_SIZE_MAX)
            result->Status = AUX_BIOS_INVALID_INPUT;
        else
        {
            AuxBiosDefaults(device, bios);
            bios->PacketSize = bh;
        }

        break;

    case AUX_BIOS_EXTENDED:
        if (bh == 0)
            AuxBiosStatus(device, result);
        else if (bh == 1 || bh == 2)
            device->IsScaling21 = (BYTE)(bh == 2);
        else
            result->Status = AUX_BIOS_INVALID_INPUT;

        break;

    case AUX_BIOS_SET_HANDLER:
        bios->HandlerSegment = es;
        bios->HandlerOffset = bx;

        if (!es && !bx)
            bios->IsEnabled = 0;                             /* no handler, nothing to enable */

        break;

    default:
        result->Status = AUX_BIOS_INVALID_FUNCTION;
        break;
    }
}

INT AuxBiosIsActive(const AUX_BIOS *bios)
{
    return bios->IsEnabled && (bios->HandlerSegment || bios->HandlerOffset);
}
