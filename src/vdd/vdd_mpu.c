/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * See vdd_mpu.h.  MPU-401 UART-mode MIDI, on the VDD bus.
 *
 * No Windows calls, only Windows types.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include "vdd_mpu.h"

#define MPU_EMPTY_QUEUE             0xFF    /* The data port with nothing waiting */
#define MPU_HIGH_NIBBLE             0xF0
#define MPU_PROGRAM_CHANGE          0xC0    /* Channel messages, by their high nibble */
#define MPU_CHANNEL_PRESSURE        0xD0
#define MPU_NOTE_OFF                0x80
#define MPU_NOTE_ON                 0x90
#define MPU_KEY_PRESSURE            0xA0
#define MPU_CONTROL_CHANGE          0xB0
#define MPU_PITCH_BEND              0xE0
#define MPU_TIME_CODE               0xF1    /* System common */
#define MPU_SONG_POSITION           0xF2
#define MPU_SONG_SELECT             0xF3
#define MPU_ONE_DATA_BYTE           1
#define MPU_TWO_DATA_BYTES          2
#define MPU_NO_DATA_BYTES           0
#define MPU_DATA1_SHIFT             8       /* midiOutShortMsg: status | data1<<8 | data2<<16 */
#define MPU_DATA2_SHIFT             16
#define MPU_SYSEX_START             0xF0
#define MPU_SYSEX_END               0xF7
#define MPU_REALTIME_FIRST          0xF8    /* F8h-FFh: realtime, standalone */
#define MPU_STATUS_BIT              0x80    /* A byte with bit 7 set is a status byte */
#define MPU_COMMAND_RESET           0xFF
#define MPU_COMMAND_UART_MODE       0x3F
#define MPU_COMMAND_PORT_OFFSET     1       /* Base+1: status (read) / command (write) */
#define MPU_STATUS_DATA_WAITING     0x00    /* DSR clear: a byte is waiting */
#define MPU_OK                      0
#define MPU_FAILED                  (-1)

static VOID MpuInputQueuePush(PMPU_STATE state, BYTE value)
{
    if (state->InputQueueLength >= MPU_INPUT_QUEUE_SIZE)
        return;

    state->InputQueue[(state->InputQueueHead + state->InputQueueLength) % MPU_INPUT_QUEUE_SIZE] = value;
    state->InputQueueLength++;
}

static BYTE MpuInputQueuePop(PMPU_STATE state)
{
    BYTE value;

    if (!state->InputQueueLength)
        return MPU_EMPTY_QUEUE;

    value = state->InputQueue[state->InputQueueHead];
    state->InputQueueHead = (BYTE)((state->InputQueueHead + 1) % MPU_INPUT_QUEUE_SIZE);
    state->InputQueueLength--;
    return value;
}

/* How many data bytes follow a status byte. Program change and channel pressure
 * take one; everything else in the channel range takes two.
 */
static BYTE MpuDataLength(BYTE status)
{
    switch (status & MPU_HIGH_NIBBLE)
    {
    case MPU_PROGRAM_CHANGE:
    case MPU_CHANNEL_PRESSURE:
        return MPU_ONE_DATA_BYTE;

    case MPU_NOTE_OFF:
    case MPU_NOTE_ON:
    case MPU_KEY_PRESSURE:
    case MPU_CONTROL_CHANGE:
    case MPU_PITCH_BEND:
        return MPU_TWO_DATA_BYTES;

    default:
        break;
    }

    switch (status)                             /* system common */
    {
    case MPU_TIME_CODE:
    case MPU_SONG_SELECT:
        return MPU_ONE_DATA_BYTE;

    case MPU_SONG_POSITION:
        return MPU_TWO_DATA_BYTES;

    default:
        return MPU_NO_DATA_BYTES;          /* realtime / undefined */
    }
}

static VOID MpuEmit(PMPU_STATE state)
{
    UINT32 message = (UINT32)state->Status
                 | ((UINT32)state->Data[0] << MPU_DATA1_SHIFT)
                 | ((UINT32)state->Data[1] << MPU_DATA2_SHIFT);

    state->MessagesSent++;

    if (state->Sink)
        state->Sink(state->SinkContext, message);
}

/* #136: SysEx assembly for an attached SysExSink. No-ops without one. */
static VOID MpuSysExPut(PMPU_STATE state, BYTE value)
{
    if (!state->SysExSink || state->IsSysExOverflow)
        return;

    if (state->SysExLength >= MPU_SYSEX_MAX)
    {
        state->IsSysExOverflow = TRUE;
        return;
    }

    state->SysEx[state->SysExLength++] = value;
}

static VOID MpuSysExBegin(PMPU_STATE state)
{
    state->SysExLength = 0;
    state->IsSysExOverflow = FALSE;
    MpuSysExPut(state, MPU_SYSEX_START);
}

static VOID MpuSysExEnd(PMPU_STATE state)
{
    if (!state->SysExSink)
        return;

    MpuSysExPut(state, MPU_SYSEX_END);

    if (state->IsSysExOverflow)
    {
        state->SysExDropped++;
        return;
    }

    state->SysExSent++;
    state->SysExSink(state->SysExContext, state->SysEx, state->SysExLength);
}

/* Assemble a MIDI byte stream into whole messages. Two details matter for real
 * game output: RUNNING STATUS (a stream may send several note-ons under one
 * status byte, which is how sequencers save bandwidth) and realtime bytes, which
 * may appear ANYWHERE -- even between the data bytes of another message -- and
 * must not disturb the message being assembled.
 */
static VOID MpuMidiByte(PMPU_STATE state, BYTE value)
{
    if (value >= MPU_REALTIME_FIRST)            /* realtime: standalone, no data */
    {
        BYTE savedStatus = state->Status;
        BYTE savedData0 = state->Data[0];
        BYTE savedData1 = state->Data[1];
        BYTE savedCount = state->DataCount;
        state->Status = value;
        state->Data[0] = state->Data[1] = 0;
        MpuEmit(state);
        state->Status = savedStatus;
        state->Data[0] = savedData0;
        state->Data[1] = savedData1;
        state->DataCount = savedCount;
        return;
    }

    /* SysEx: swallowed, unless a synth that wants it is attached (#136, see the header).
     *
     * [CAUTION]: The swallow path is byte-for-byte the old one, including that a status byte
     * inside an unterminated message is swallowed too. With a sink attached that byte
     * ENDS the message (MIDI 1.0: any status but a realtime one terminates SysEx); the
     * unfinished message is dropped and counted, and the status byte is handled as
     * what it is.
     */
    if (value == MPU_SYSEX_START)
    {
        state->IsInSysEx = TRUE;
        MpuSysExBegin(state);
        return;
    }

    if (value == MPU_SYSEX_END)
    {
        if (state->IsInSysEx)
            MpuSysExEnd(state);

        state->IsInSysEx = FALSE;
        return;
    }

    if (state->IsInSysEx)
    {
        if (!state->SysExSink)
            return;

        if (!(value & MPU_STATUS_BIT))
        {
            MpuSysExPut(state, value);
            return;
        }

        state->IsInSysEx = FALSE;
        state->SysExDropped++;
    }

    if (value & MPU_STATUS_BIT)                 /* new status byte */
    {
        state->Status = value;
        state->DataCount = 0;
        state->DataWanted = MpuDataLength(value);

        if (!state->DataWanted)
        {
            state->Data[0] = state->Data[1] = 0;
            MpuEmit(state);
        }

        return;
    }

    if (!state->Status)
        return;                                 /* data with no status: ignore */

    if (state->DataCount < MPU_MIDI_DATA_BYTES)
        state->Data[state->DataCount] = value;

    state->DataCount++;

    if (state->DataCount >= state->DataWanted)
    {
        if (state->DataWanted < MPU_MIDI_DATA_BYTES)
            state->Data[1] = 0;

        MpuEmit(state);
        state->DataCount = 0;                   /* running status: keep state->Status */
    }
}

VOID VddMpuFeed(PMPU_STATE state, BYTE value)
{
    MpuMidiByte(state, value);
}

static VOID MpuPortOut(PVOID context, WORD port, BYTE width, UINT32 value)
{
    PMPU_STATE state = (PMPU_STATE)context;
    BYTE byteValue = (BYTE)value;

    (VOID)width;

    if (port == state->BasePort)                /* data port */
    {
        if (state->IsUartMode)
            MpuMidiByte(state, byteValue);

        return;
    }

    /* command port: only the two commands every game issues are implemented, and
     * both must be acknowledged or the driver decides there is no interface.
     */
    switch (byteValue)
    {
    case MPU_COMMAND_RESET:                     /* reset */
        state->IsUartMode = FALSE;
        state->Status = 0;
        state->DataCount = 0;
        state->IsInSysEx = FALSE;
        MpuInputQueuePush(state, MPU_ACK);
        break;

    case MPU_COMMAND_UART_MODE:                 /* enter UART mode */
        state->IsUartMode = TRUE;
        MpuInputQueuePush(state, MPU_ACK);
        break;

    default:
        MpuInputQueuePush(state, MPU_ACK);      /* acknowledge and ignore */
        break;
    }
}

static VOID MpuPortIn(PVOID context, WORD port, BYTE width, UINT32 *value)
{
    PMPU_STATE state = (PMPU_STATE)context;

    (VOID)width;

    if (port == state->BasePort)
    {
        *value = MpuInputQueuePop(state);
        return;
    }

    /* Status: BOTH flags are active low. DRR clear = we can take a byte (always);
     * DSR clear = a byte is waiting to be read.
     */
    *value = (BYTE)(state->InputQueueLength ? MPU_STATUS_DATA_WAITING : MPU_STATUS_DSR);
}

VOID VddMpuReset(PVOID context)
{
    PMPU_STATE state = (PMPU_STATE)context;
    PVDD_BUS bus = state->Bus;
    WORD basePort = state->BasePort;
    PMPU_MIDI_SINK sink = state->Sink;
    PVOID sinkContext = state->SinkContext;
    PMPU_SYSEX_SINK sysExSink = state->SysExSink;
    PVOID sysExContext = state->SysExContext;   /* #136 */
    UINT byteIndex;
    BYTE *stateBytes = (BYTE *)state;

    for (byteIndex = 0; byteIndex < sizeof(*state); ++byteIndex)
        stateBytes[byteIndex] = 0;

    state->Bus = bus;
    state->BasePort = basePort;
    state->Sink = sink;
    state->SinkContext = sinkContext;
    state->SysExSink = sysExSink;
    state->SysExContext = sysExContext;
}

INT VddMpuInitialize(PVDD_BUS bus, PVOID context)
{
    PMPU_STATE state = (PMPU_STATE)context;

    state->Bus = bus;

    if (!state->BasePort)
        state->BasePort = MPU_DEFAULT_BASE;

    if (VddClaimPorts(bus, state->BasePort, (WORD)(state->BasePort + MPU_COMMAND_PORT_OFFSET), MpuPortIn, MpuPortOut, state))
        return MPU_FAILED;

    return MPU_OK;
}
