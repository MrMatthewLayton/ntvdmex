/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * MPU-401 MIDI interface VDD (UART mode).  (sound epic, GH #20/#21)
 *
 * The third way a DOS game makes sound: instead of synthesising FM itself or
 * streaming samples, it sends MIDI events to an external synthesiser. On XP there
 * IS one -- the Microsoft GS Wavetable synth -- so the honest implementation is to
 * forward events to the host rather than synthesise them ourselves, which is both
 * far less work and far better sounding than an FM approximation of General MIDI.
 *
 * Ports 0x330 (data) and 0x331 (status read / command write). Two things about
 * the status register catch everyone out: both flags are ACTIVE LOW (a CLEAR bit
 * means ready), and a game will not proceed until it sees the 0xFE acknowledge
 * after the 0xFF reset and 0x3F "enter UART mode" commands.
 *
 * Intelligent mode (the MPU's own sequencer) is deliberately NOT implemented:
 * essentially every DOS game uses UART mode, and a half-built intelligent mode
 * would be worse than none -- a game that detects it would then rely on it.
 *
 * No Windows calls, only Windows types: assembled MIDI messages go to an injected sink, so the
 * device is exercised off-VM by tests/unit/mpu_test.c with no host MIDI at all.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_VDD_MPU_H
#define NTVDMEX_VDD_MPU_H

#include "vdd_bus.h"

#define MPU_DEFAULT_BASE            0x330
#define MPU_DEFAULT_BASE_CHOICE     3       /* MPU_DEFAULT_BASE in the Audio page's list */

/* status register bits, both ACTIVE LOW */
#define MPU_STATUS_DRR              0x40    /* Clear => the port can accept a byte from the guest */
#define MPU_STATUS_DSR              0x80    /* Clear => a byte is waiting for the guest to read */

#define MPU_ACK                     0xFE    /* What a command must answer with */
#define MPU_INPUT_QUEUE_SIZE        8
#define MPU_MIDI_DATA_BYTES         2       /* The most data bytes a short message carries */
#define MPU_DEVICE_NAME             "mpu401"

/* A complete MIDI message, ready for the host synth. `msg` is packed as
 * status | data1<<8 | data2<<16 (the layout midiOutShortMsg wants).
 */
typedef VOID (*PMPU_MIDI_SINK)(PVOID context, UINT32 message);

/* #136: SYSTEM EXCLUSIVE, FOR A SYNTH THAT NEEDS IT:
 * SysEx has always been SWALLOWED here, and for XP's GS Wavetable synth that is the
 * right call -- it is what every build so far has sent it. An MT-32 is a different
 * instrument: a game uploads its own timbres and sets its reverb with DT1 SysEx, and
 * without them it plays the factory patches -- the wrong instruments, not silence.
 * So when Settings > Audio > MIDI picks an EXTERNAL synth (MT-32 / SoundFont, routed
 * by name in midi_route.h), the host sets `SysExSink` and each COMPLETE message,
 * F0 .. F7 inclusive, goes to it whole. NULL = swallowed exactly as before, byte for
 * byte -- the default path does not change.
 *
 * [CAUTION]: A message longer than MPU_SYSEX_MAX is dropped and counted, never truncated: a cut
 * DT1 has a wrong checksum and an MT-32 rejects it anyway, and a cut bulk dump that
 * happened to checksum would write garbage into the synth's memory.
 */
#define MPU_SYSEX_MAX   4096
typedef VOID (*PMPU_SYSEX_SINK)(PVOID context, const BYTE *message, UINT32 length);

typedef struct _MPU_STATE
{
    PVDD_BUS Bus;
    WORD     BasePort;
    BYTE     IsUartMode;            /* 0x3F received: pass bytes straight through */

    BYTE     InputQueue[MPU_INPUT_QUEUE_SIZE];  /* bytes waiting for the guest (mostly ACKs) */
    BYTE     InputQueueHead, InputQueueLength;

    /* running MIDI message assembly */
    BYTE     Status;                /* current status byte (running status) */
    BYTE     Data[MPU_MIDI_DATA_BYTES];
    BYTE     DataCount, DataWanted;
    BYTE     IsInSysEx;

    PMPU_MIDI_SINK Sink;
    PVOID SinkContext;
    UINT32   MessagesSent;          /* messages forwarded (tests + diagnostics) */

    /* #136: SysEx passthrough -- NULL sink = swallowed, as always. Preserved by reset. */
    PMPU_SYSEX_SINK SysExSink;
    PVOID SysExContext;
    UINT32   SysExLength;
    BYTE     IsSysExOverflow;       /* this message outgrew the buffer: drop it */
    UINT32   SysExSent, SysExDropped;
    BYTE     SysEx[MPU_SYSEX_MAX];
} MPU_STATE, *PMPU_STATE;

INT  VddMpuInitialize(_In_ PVDD_BUS bus, _In_ PVOID context);
VOID VddMpuReset(_In_ PVOID context);

/* #190: one raw MIDI byte into this instance's message assembler (running status,
 * realtime, sysex -- the same rules as the data port in UART mode), whatever mode the
 * instance is in and without touching its ports. This is how ANOTHER device's MIDI
 * byte stream reaches the synth: the GUS's 6850 UART transmits bytes, and the host
 * gives it a private MPU_STATE (never added to the bus) whose `Sink` is the synth.
 * A private one, because two byte streams sharing one assembler would corrupt each
 * other's running status.
 */
VOID VddMpuFeed(_Inout_ PMPU_STATE state, _In_ BYTE value);
static inline NTVDD_DEVICE VddMpuDevice(_In_ PMPU_STATE state)
{ NTVDD_DEVICE device;
device.Name = MPU_DEVICE_NAME;
device.Initialize = VddMpuInitialize;
device.Reset = VddMpuReset;
  device.Shutdown = 0;
  device.Context = state;
  return device; }

#endif /* NTVDMEX_VDD_MPU_H */
