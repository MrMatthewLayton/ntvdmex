/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Off-VM unit battery for the MPU-401 MIDI VDD (vdd_mpu.c).
 *
 * The device forwards whole MIDI messages to a sink, so the battery captures them
 * and checks the assembly rules that real game output depends on:
 *
 *   - the reset / UART-mode handshake acknowledges with 0xFE (without it, drivers
 *     conclude there is no interface)
 *   - the status register's flags are ACTIVE LOW, which is the classic way to get
 *     an MPU driver stuck waiting for a ready bit that never clears
 *   - RUNNING STATUS: a sequencer sends many notes under one status byte
 *   - REALTIME bytes (clock, start, stop) may arrive between the data bytes of
 *     another message and must not corrupt it
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include <stdio.h>
#include <string.h>
#include "vdd_mpu.h"

static INT g_Total = 0;
static INT g_Failures = 0;
#define CHECK(condition,message) do{ g_Total++; if(condition){printf("  PASS  %s\n",(message));} \
    else{printf("  FAIL  %s\n",(message)); g_Failures++;} }while(0)

static BYTE g_GuestMemory[0x10000];
static VDD_BUS g_Bus;
static MPU_STATE g_Mpu;

#define CAP     32
static UINT32 g_Messages[CAP];
static INT      g_MessageCount;
static VOID MpuTestSink(PVOID context, UINT32 message)
{
    (VOID)context;
    if (g_MessageCount < CAP)
        g_Messages[g_MessageCount++] = message;
}

/* #136: the SysEx sink, for an external synth. */
static BYTE  g_SysEx[MPU_SYSEX_MAX];
static UINT32 g_SysExLength;
static INT      g_SysExCount;
static VOID MpuTestSysExSink(PVOID context, PCBYTE bytes, UINT32 length)
{
    (VOID)context;
    memcpy(g_SysEx, bytes, length);
    g_SysExLength = length;
    g_SysExCount++;
}

#define BASE    MPU_DEFAULT_BASE
static VOID MpuTestWrite(WORD port, BYTE byteValue)
{
    UINT32 value=byteValue;

    VddBusIo(&g_Bus,port,1,0,&value);
}

static BYTE MpuTestRead(WORD port)
{
    UINT32 value=0;

    VddBusIo(&g_Bus,port,1,1,&value);
    return (BYTE)value;
}

INT main(VOID)
{
    printf("== sound epic: MPU-401 MIDI battery ==\n");

    memset(&g_Mpu, 0, sizeof g_Mpu);
    g_Mpu.Sink = MpuTestSink;
    VddBusInitialize(&g_Bus, g_GuestMemory);
    {
        NTVDD_DEVICE device = VddMpuDevice(&g_Mpu);
        CHECK(VddBusAdd(&g_Bus, &device) == 0, "add: mpu401 ok");
    }

    /* T1: the handshake ----------------------------------------------------- */
    CHECK((MpuTestRead(BASE + 1) & MPU_STATUS_DSR) != 0, "status: DSR set (active low) => no data waiting");
    MpuTestWrite(BASE + 1, 0xFF);                                   /* reset */
    CHECK((MpuTestRead(BASE + 1) & MPU_STATUS_DSR) == 0, "status: DSR CLEAR once the ACK is queued");
    CHECK(MpuTestRead(BASE) == MPU_ACK, "reset: acknowledges with 0xFE");
    MpuTestWrite(BASE + 1, 0x3F);                                   /* enter UART mode */
    CHECK(MpuTestRead(BASE) == MPU_ACK, "uart: acknowledges with 0xFE");
    CHECK(g_Mpu.IsUartMode == 1, "uart: mode entered");
    CHECK((MpuTestRead(BASE + 1) & MPU_STATUS_DRR) == 0, "status: DRR clear => ready to accept data");

    /* T2: a plain note-on --------------------------------------------------- */
    g_MessageCount = 0;
    MpuTestWrite(BASE, 0x90);
    MpuTestWrite(BASE, 0x3C);
    MpuTestWrite(BASE, 0x64);       /* note on C4 vel 100 */
    CHECK(g_MessageCount == 1, "note-on: one complete message emitted");
    CHECK(g_Messages[0] == (0x90u | (0x3Cu << 8) | (0x64u << 16)),
          "note-on: packed as status | data1<<8 | data2<<16");

    /* nothing is emitted until the message is complete */
    g_MessageCount = 0;
    MpuTestWrite(BASE, 0x80);
    MpuTestWrite(BASE, 0x3C);
    CHECK(g_MessageCount == 0, "partial message: nothing emitted yet");
    MpuTestWrite(BASE, 0x40);
    CHECK(g_MessageCount == 1, "partial message: emitted once complete");

    /* T3: running status ---------------------------------------------------- */
    g_MessageCount = 0;
    MpuTestWrite(BASE, 0x90);                                       /* status once... */
    MpuTestWrite(BASE, 0x40);
    MpuTestWrite(BASE, 0x7F);                       /* ...then note pairs */
    MpuTestWrite(BASE, 0x43);
    MpuTestWrite(BASE, 0x7F);
    MpuTestWrite(BASE, 0x47);
    MpuTestWrite(BASE, 0x7F);
    CHECK(g_MessageCount == 3, "running status: three notes under one status byte");
    CHECK((g_Messages[1] & 0xFF) == 0x90 && ((g_Messages[1] >> 8) & 0xFF) == 0x43,
          "running status: second note keeps the status byte");

    /* T4: one-data-byte messages -------------------------------------------- */
    g_MessageCount = 0;
    MpuTestWrite(BASE, 0xC0);
    MpuTestWrite(BASE, 0x30);                       /* program change */
    CHECK(g_MessageCount == 1, "program change: completes after ONE data byte");
    CHECK(g_Messages[0] == (0xC0u | (0x30u << 8)), "program change: second data byte is zero");

    /* T5: realtime bytes may interrupt a message ---------------------------- */
    g_MessageCount = 0;
    MpuTestWrite(BASE, 0x90);
    MpuTestWrite(BASE, 0x3C);                       /* mid-message... */
    MpuTestWrite(BASE, 0xF8);                                       /* ...timing clock */
    CHECK(g_MessageCount == 1 && g_Messages[0] == 0xF8, "realtime: clock emitted immediately");
    MpuTestWrite(BASE, 0x64);                                       /* finish the note-on */
    CHECK(g_MessageCount == 2, "realtime: the interrupted note-on still completes");
    CHECK(g_Messages[1] == (0x90u | (0x3Cu << 8) | (0x64u << 16)),
          "realtime: interrupted message is not corrupted");

    /* T6: sysex is swallowed, not mistaken for channel data ----------------- */
    g_MessageCount = 0;
    MpuTestWrite(BASE, 0xF0);
    MpuTestWrite(BASE, 0x41);
    MpuTestWrite(BASE, 0x10);
    MpuTestWrite(BASE, 0xF7);
    CHECK(g_MessageCount == 0, "sysex: swallowed without emitting garbage");
    g_MessageCount = 0;
    MpuTestWrite(BASE, 0x90);
    MpuTestWrite(BASE, 0x3C);
    MpuTestWrite(BASE, 0x64);
    CHECK(g_MessageCount == 1, "sysex: normal messages resume afterwards");

    /* T6b (#136): with NO sysex sink, a status byte inside an unterminated SysEx is
     * swallowed too -- the old behaviour, byte for byte.
     */
    g_MessageCount = 0;
    MpuTestWrite(BASE, 0xF0);
    MpuTestWrite(BASE, 0x41);
    MpuTestWrite(BASE, 0x90);
    MpuTestWrite(BASE, 0x3C);
    MpuTestWrite(BASE, 0x64);
    CHECK(g_MessageCount == 0, "sysex, no sink: an embedded status byte is swallowed (unchanged)");
    MpuTestWrite(BASE, 0xF7);
    CHECK(g_Mpu.SysExSent == 0 && g_Mpu.SysExDropped == 0, "sysex, no sink: nothing counted");

    /* T6c (#136): an attached sink gets each COMPLETE message, F0..F7 inclusive. */
    g_Mpu.SysExSink = MpuTestSysExSink;
    g_SysExCount = 0;
    g_MessageCount = 0;
    {   static const BYTE dt1Message[] = { 0xF0, 0x41, 0x10, 0x16, 0x12, 0x10, 0x00, 0x01,
                                       0x02, 0x6D, 0xF7 };
        UINT index;
        for (index = 0; index < sizeof dt1Message; ++index)
            MpuTestWrite(BASE, dt1Message[index]);
        CHECK(g_SysExCount == 1 && g_SysExLength == sizeof dt1Message && memcmp(g_SysEx, dt1Message, sizeof dt1Message) == 0,
              "sysex sink: one MT-32 DT1 delivered whole, F0..F7");
        CHECK(g_MessageCount == 0, "sysex sink: no short message leaks out of it");
    }
    /* realtime inside SysEx is a short message and does not break the SysEx */
    g_SysExCount = 0;
    g_MessageCount = 0;
    MpuTestWrite(BASE, 0xF0);
    MpuTestWrite(BASE, 0x41);
    MpuTestWrite(BASE, 0xF8);
    MpuTestWrite(BASE, 0x10);
    MpuTestWrite(BASE, 0xF7);
    CHECK(g_MessageCount == 1 && g_Messages[0] == 0xF8, "sysex sink: realtime inside passes as a short message");
    CHECK(g_SysExCount == 1 && g_SysExLength == 4 && g_SysEx[1] == 0x41 && g_SysEx[2] == 0x10,
          "sysex sink: ...and the SysEx around it stays intact");
    /* a status byte ends an unterminated SysEx: dropped, counted, then handled */
    g_SysExCount = 0;
    g_MessageCount = 0;
    {   UINT32 droppedBefore = g_Mpu.SysExDropped;
        MpuTestWrite(BASE, 0xF0);
        MpuTestWrite(BASE, 0x41);
        MpuTestWrite(BASE, 0x90);
        MpuTestWrite(BASE, 0x3C);
        MpuTestWrite(BASE, 0x64);
        CHECK(g_SysExCount == 0 && g_Mpu.SysExDropped == droppedBefore + 1, "sysex sink: unterminated message dropped");
        CHECK(g_MessageCount == 1 && g_Messages[0] == (0x90u | (0x3Cu << 8) | (0x64u << 16)),
              "sysex sink: the status byte that ended it is a note-on");
    }
    /* longer than MPU_SYSEX_MAX: dropped whole, never truncated */
    g_SysExCount = 0;
    {   UINT32 droppedBefore = g_Mpu.SysExDropped;
    UINT index;
        MpuTestWrite(BASE, 0xF0);
        for (index = 0; index < MPU_SYSEX_MAX + 10; ++index)
            MpuTestWrite(BASE, 0x11);
        MpuTestWrite(BASE, 0xF7);
        CHECK(g_SysExCount == 0 && g_Mpu.SysExDropped == droppedBefore + 1, "sysex sink: oversize message dropped, not cut");
        MpuTestWrite(BASE, 0xF0);
        MpuTestWrite(BASE, 0x7E);
        MpuTestWrite(BASE, 0xF7);
        CHECK(g_SysExCount == 1 && g_SysExLength == 3, "sysex sink: the next message is fine");
    }
    /* reset keeps the sink, like the short-message sink */
    VddMpuReset(&g_Mpu);
    CHECK(g_Mpu.SysExSink == MpuTestSysExSink && g_Mpu.Sink == MpuTestSink, "reset: both sinks preserved");
    g_Mpu.SysExSink = NULL;

    /* T7: data before UART mode goes nowhere --------------------------------- */
    VddMpuReset(&g_Mpu);
    g_MessageCount = 0;
    MpuTestWrite(BASE, 0x90);
    MpuTestWrite(BASE, 0x3C);
    MpuTestWrite(BASE, 0x64);
    CHECK(g_MessageCount == 0, "not in UART mode: data bytes are ignored");

    printf("-- %d checks, %d failures --\n", g_Total, g_Failures);
    return g_Failures ? 1 : 0;
}
