/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The PS/2 pointing device behind the 8042's auxiliary port. (#199)
 *
 * WHY THIS EXISTS. Our INT 33h driver serves every program that asks it, but a program
 * that brings its OWN mouse driver -- a MOUSE.COM, a game's private PS/2 code -- never
 * calls INT 33h. It talks to the mouse itself: 8042 command D4h routes the next byte
 * written to port 60h to the mouse, the mouse answers through the 8042's output buffer
 * with status bit 5 (AUXB) set and IRQ12, and while streaming it sends a 3-byte packet
 * for every movement. With no device there, such a driver found no mouse at all.
 *
 * This is the device: its command set, its replies and its packets, as bytes. No
 * Windows calls and no 8042 -- vdd_input.c owns the controller and asks this for
 * bytes; tests/unit/aux_test.c drives it directly.
 *
 * Sources (not held in the repo; cited by name, as docs/ref/SOURCES.md lists them):
 * - IBM Personal System/2 Hardware Interface Technical Reference, "Keyboard and
 *   Auxiliary Device Controller" and "Pointing Device": the command set, the replies,
 *   the status bytes and the packet layout.
 * - Ralf Brown's Interrupt List, PORTS.LST 0060h/0064h (the D4h/A8h/A9h commands) and
 *   INT 15h AX=C2xxh (the BIOS interface over this device).
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_VDD_AUX_H
#define NTVDMEX_VDD_AUX_H

#include "vdd_bus.h"

/* Mouse commands (written via 8042 D4h), IBM PS/2 TR "Pointing Device". */
#define AUX_COMMAND_SET_SCALING_1_1     0xE6
#define AUX_COMMAND_SET_SCALING_2_1     0xE7
#define AUX_COMMAND_SET_RESOLUTION      0xE8    /* Parameter: 0-3 = 1, 2, 4, 8 counts/mm */
#define AUX_COMMAND_STATUS_REQUEST      0xE9    /* -> ACK, status, resolution, sample rate */
#define AUX_COMMAND_SET_STREAM_MODE     0xEA
#define AUX_COMMAND_READ_DATA           0xEB    /* -> ACK, one packet (remote mode) */
#define AUX_COMMAND_RESET_WRAP_MODE     0xEC
#define AUX_COMMAND_SET_WRAP_MODE       0xEE
#define AUX_COMMAND_SET_REMOTE_MODE     0xF0
#define AUX_COMMAND_GET_DEVICE_ID       0xF2    /* -> ACK, 00h (a standard PS/2 mouse) */
#define AUX_COMMAND_SET_SAMPLE_RATE     0xF3    /* Parameter: reports per second */
#define AUX_COMMAND_ENABLE_REPORTING    0xF4
#define AUX_COMMAND_DISABLE_REPORTING   0xF5
#define AUX_COMMAND_SET_DEFAULTS        0xF6
#define AUX_COMMAND_RESEND              0xFE
#define AUX_COMMAND_RESET               0xFF    /* -> ACK, AAh (self test passed), 00h (ID) */

#define AUX_REPLY_ACK                   0xFA
#define AUX_REPLY_RESEND                0xFE
#define AUX_REPLY_SELF_TEST_PASSED      0xAA
#define AUX_DEVICE_ID_STANDARD          0x00

/* Power-on / F6h defaults (IBM PS/2 TR): 100 reports/s, 4 counts/mm, 1:1, stream mode,
 * reporting disabled.
 */
#define AUX_DEFAULT_SAMPLE_RATE         100
#define AUX_DEFAULT_RESOLUTION          2

/* Packet byte 0. */
#define AUX_PACKET_LEFT                 0x01
#define AUX_PACKET_RIGHT                0x02
#define AUX_PACKET_MIDDLE               0x04
#define AUX_PACKET_ALWAYS_ONE           0x08
#define AUX_PACKET_X_SIGN               0x10
#define AUX_PACKET_Y_SIGN               0x20
#define AUX_PACKET_X_OVERFLOW           0x40
#define AUX_PACKET_Y_OVERFLOW           0x80
#define AUX_PACKET_BYTES                3
#define AUX_MOVEMENT_MAX                255     /* Nine-bit two's complement: -256..255 */

/* E9h's first status byte. */
#define AUX_STATUS_RIGHT                0x01
#define AUX_STATUS_MIDDLE               0x02
#define AUX_STATUS_LEFT                 0x04
#define AUX_STATUS_SCALING_2_1          0x10
#define AUX_STATUS_REPORTING            0x20
#define AUX_STATUS_REMOTE               0x40

#define AUX_QUEUE_SIZE                  64

/* INT 15h AX=C2xxh: THE BIOS's POINTING-DEVICE INTERFACE (RBIL INT 15/AX=C2xx):
 * A driver that would rather not talk to the 8042 asks the BIOS: C205h initialise (BH =
 * packet size), C207h install a far-call handler (ES:BX), C200h BH=1 enable. The BIOS's
 * IRQ12 handler then calls that handler once per packet with four words on the stack --
 * pushed in this order: status, X, Y, Z (0) -- and the handler RETFs without popping them.
 * The services drive the same device the 8042 port does.
 */
#define AUX_BIOS_ENABLE                 0x00    /* AL: BH = 0 disable, 1 enable */
#define AUX_BIOS_RESET                  0x01    /* -> BH = device ID, BL = AAh */
#define AUX_BIOS_SET_SAMPLE_RATE        0x02    /* BH = 0-6: 10, 20, 40, 60, 80, 100, 200 /s */
#define AUX_BIOS_SET_RESOLUTION         0x03    /* BH = 0-3: 1, 2, 4, 8 counts/mm */
#define AUX_BIOS_GET_TYPE               0x04    /* -> BH = device ID */
#define AUX_BIOS_INITIALIZE             0x05    /* BH = packet size, 1-8 bytes */
#define AUX_BIOS_EXTENDED               0x06    /* BH = 0 status (-> BL, CL, DL), 1 scaling 1:1, 2 scaling 2:1 */
#define AUX_BIOS_SET_HANDLER            0x07    /* ES:BX = the far-call handler (0:0 removes it) */

#define AUX_BIOS_OK                     0x00    /* AH */
#define AUX_BIOS_INVALID_FUNCTION       0x01
#define AUX_BIOS_INVALID_INPUT          0x02
#define AUX_BIOS_NO_HANDLER             0x05    /* "no far call installed" */

#define AUX_BIOS_RATE_CHOICES           7
#define AUX_BIOS_PACKET_SIZE_MIN        1
#define AUX_BIOS_PACKET_SIZE_MAX        8
#define AUX_BIOS_SET_BH                 0x01    /* AUX_BIOS_RESULT.Outputs: which registers to set */
#define AUX_BIOS_SET_BL                 0x02
#define AUX_BIOS_SET_CL                 0x04
#define AUX_BIOS_SET_DL                 0x08

typedef struct _AUX_DEVICE
{
    BYTE   Queue[AUX_QUEUE_SIZE];   /* Bytes the mouse has sent, waiting for the 8042 */
    INT    Head;
    INT    Tail;
    BYTE   PendingCommand;          /* E8h/F3h awaiting its parameter; 0 = none */
    BYTE   IsReporting;             /* F4h: stream packets */
    BYTE   IsRemote;                /* F0h: packets only on EBh */
    BYTE   IsWrap;                  /* EEh: echo every byte until ECh/FFh */
    BYTE   IsScaling21;
    BYTE   Resolution;              /* 0-3 */
    BYTE   SampleRate;
    BYTE   Buttons;                 /* AUX_PACKET_LEFT/RIGHT/MIDDLE, as last reported */
    BYTE   LastReply;               /* What FEh sends again */
    LONG   RemoteDx;                /* Remote mode: movement since the last EBh */
    LONG   RemoteDy;
    UINT32 Commands;                /* Bytes written to the device */
    UINT32 Packets;                 /* Packets queued */
    UINT32 Dropped;                 /* Bytes lost to a full queue */
} AUX_DEVICE, *PAUX_DEVICE;

typedef const AUX_DEVICE *PCAUX_DEVICE;

typedef struct _AUX_BIOS
{
    BYTE IsEnabled;                 /* C200h BH=1 */
    BYTE PacketSize;                /* C205h */
    WORD HandlerSegment;            /* C207h */
    WORD HandlerOffset;
    UINT32 Calls;
} AUX_BIOS, *PAUX_BIOS;

typedef struct _AUX_BIOS_RESULT
{
    BYTE Status;                    /* AH; CF = (Status != AUX_BIOS_OK) */
    BYTE Outputs;                   /* AUX_BIOS_SET_* */
    BYTE Bh;
    BYTE Bl;
    BYTE Cl;
    BYTE Dl;
} AUX_BIOS_RESULT, *PAUX_BIOS_RESULT;

/* Power-on state: the defaults, reporting off, nothing queued. */
VOID AuxDeviceReset(_Out_ PAUX_DEVICE device);

/* A byte written to the device (8042 D4h, then 60h). Queues the device's replies. */
VOID AuxDeviceWrite(_Inout_ PAUX_DEVICE device, _In_ BYTE value);

/* The host moved the mouse: dx right-positive, dy DOWN-positive (screen sense),
 * buttons as AUX_PACKET_LEFT/RIGHT/MIDDLE. Streams a packet when reporting in stream
 * mode, accumulates for EBh in remote mode, drops it otherwise. Returns 1 if a packet
 * was queued.
 */
INT  AuxDeviceMove(_Inout_ PAUX_DEVICE device, _In_ LONG dx, _In_ LONG dy, _In_ BYTE buttons);

/* Build one packet (pure): the 3 bytes for a movement and a button state, with the
 * sign and overflow bits. dy is screen sense (down-positive); the packet's Y is up-positive.
 */
VOID AuxDevicePacket(
    _In_ LONG dx,
    _In_ LONG dy,
    _In_ BYTE buttons,
    _Out_writes_(AUX_PACKET_BYTES) BYTE *packet);

INT  AuxDeviceQueued(_In_ PCAUX_DEVICE device);         /* bytes waiting */
BYTE AuxDevicePeek(_In_ PCAUX_DEVICE device);           /* the next byte (0 if none) */
BYTE AuxDevicePop(_Inout_ PAUX_DEVICE device);          /* take it (0 if none) */

/* Whether a program has put the device into streaming (F4h, stream mode): the host
 * then feeds it movement.
 */
INT  AuxDeviceIsStreaming(_In_ PCAUX_DEVICE device);

/* One INT 15h AX=C2xxh call (AL = function), on the device. ES:BX only matter to C207h. */
VOID AuxBiosService(
    _Inout_ PAUX_DEVICE device,
    _Inout_ PAUX_BIOS bios,
    _In_ BYTE function,
    _In_ BYTE bh,
    _In_ WORD es,
    _In_ WORD bx,
    _Out_ PAUX_BIOS_RESULT result);

/* Does the BIOS have a handler to call with packets (enabled, handler installed)? */
INT  AuxBiosIsActive(_In_ const AUX_BIOS *bios);

#endif
