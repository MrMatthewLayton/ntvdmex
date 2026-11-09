/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * IPX, the NetWare datagram interface a DOS game finds with INT 2Fh AX=7A00h (#315).
 *
 * WHY THIS EXISTS. Stock XP NTVDM offers IPX through vwipxspx.dll when the NWLink IPX
 * protocol is installed: INT 2Fh AX=7A00h answers AL=FFh and ES:DI = a far-call entry,
 * and the program then opens sockets and sends and listens with Event Control Blocks.
 * Without NWLink there is no IPX, and the install check answers "not installed". This
 * is the same split: the model below is the IPX API (the ECB, the socket table, the
 * listen queue), and a BACKEND -- the host's Winsock AF_IPX, which exists only where
 * NWLink is installed -- moves the packets. No backend, no IPX.
 *
 * THE INTERFACE (Novell's IPX API; RBIL INT 7Ah and INT 2Fh AX=7A00h). Called by FAR CALL
 * to the entry with BX = function:
 *   00h open socket     AL = longevity, DX = socket (big-endian; 0 = assign one)
 *                       -> AL = 00h ok, FEh table full, FFh already open; DX = socket
 *   01h close socket    DX = socket; every ECB listening on it ends with FCh
 *   02h get local target ES:SI -> 12-byte address (net, node, socket), ES:DI -> 6-byte
 *                       immediate address -> AL = 00h, CX = transport time (ticks)
 *   03h send packet     ES:SI -> ECB
 *   04h listen          ES:SI -> ECB -> AL = 00h, FFh socket not open
 *   06h cancel event    ES:SI -> ECB -> AL = 00h cancelled, F9h cannot, FFh not in use
 *   08h interval marker AX = the BIOS tick (0040:006Ch)
 *   09h internetwork address ES:SI -> 10-byte buffer (net, node)
 *   0Ah relinquish control -- here: deliver what has arrived
 *
 * THE EVENT CONTROL BLOCK (offsets): 00h link, 04h ESR far pointer, 08h in-use (FEh
 * listening, FFh sending, 00h done), 09h completion code, 0Ah socket (big-endian), 0Ch
 * IPX workspace, 10h driver workspace, 1Ch immediate address (6), 22h fragment count,
 * 24h fragments: { far pointer, WORD size } each. Completion codes: 00h ok, FCh
 * cancelled, FDh malformed / overflow, FEh undeliverable, FFh socket not open / failure.
 *
 * THE IPX HEADER is the first 30 bytes of the first fragment: checksum (FFFFh), length
 * (big-endian), transport control, packet type, destination net (4) node (6) socket
 * (2), source net node socket. The program fills the packet type and destination; on
 * send IPX fills the rest.
 *
 * [CAUTION]: WHAT IS NOT DONE, ON PURPOSE:
 * - EVENT SERVICE ROUTINES ARE NOT CALLED. An ECB's in-use and completion bytes are final
 *   when its event happens, so a program that polls them works -- Doom's IPXSETUP does --
 *   but one that relies only on its ESR does not. Counted (EsrsSkipped).
 * - 05h/07h (schedule IPX/AES events) and the SPX functions (10h-16h) are not offered;
 *   they leave AL as passed and are counted (UnsupportedCalls).
 * - Events are delivered when the program calls IPX and on each BIOS tick, not the
 *   instant a packet lands -- the granularity of a program that polls between frames.
 *
 * No CRT, no Winsock here: this file is portable and tested off-machine (ipx_test.c).
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_VDD_IPX_H
#define NTVDMEX_VDD_IPX_H

#include "vdd_bus.h"

#define IPX_HEADER_SIZE                 30
#define IPX_PACKET_MAX                  576     /* The largest IPX packet: 30 + 546 */
#define IPX_SOCKETS_MAX                 20      /* Novell's default socket table */
#define IPX_LISTENS_MAX                 64
#define IPX_ADDRESS_SIZE                10      /* Net (4) + node (6) */
#define IPX_NODE_SIZE                   6
#define IPX_NET_SIZE                    4

/* ECB offsets */
#define IPX_ECB_LINK                    0x00
#define IPX_ECB_ESR_OFFSET              0x04
#define IPX_ECB_ESR_SEGMENT             0x06
#define IPX_ECB_IN_USE                  0x08
#define IPX_ECB_COMPLETION              0x09
#define IPX_ECB_SOCKET                  0x0A
#define IPX_ECB_IMMEDIATE_ADDRESS       0x1C
#define IPX_ECB_FRAGMENT_COUNT          0x22
#define IPX_ECB_FRAGMENTS               0x24
#define IPX_ECB_FRAGMENT_SIZE           6       /* Far pointer + WORD size */
#define IPX_ECB_FRAGMENTS_MAX           16

/* IPX header offsets */
#define IPX_HEADER_CHECKSUM             0x00
#define IPX_HEADER_LENGTH               0x02
#define IPX_HEADER_TRANSPORT            0x04
#define IPX_HEADER_TYPE                 0x05
#define IPX_HEADER_DESTINATION          0x06    /* Net 4, node 6, socket 2 */
#define IPX_HEADER_DESTINATION_SOCKET   0x10
#define IPX_HEADER_SOURCE               0x12
#define IPX_HEADER_SOURCE_NODE          0x16
#define IPX_HEADER_SOURCE_SOCKET        0x1C

/* in-use and completion values */
#define IPX_IN_USE_FREE                 0x00
#define IPX_IN_USE_LISTENING            0xFE
#define IPX_IN_USE_SENDING              0xFF
#define IPX_COMPLETION_OK               0x00
#define IPX_COMPLETION_CANCELLED        0xFC
#define IPX_COMPLETION_MALFORMED        0xFD
#define IPX_COMPLETION_UNDELIVERABLE    0xFE
#define IPX_COMPLETION_FAILURE          0xFF

/* function numbers (BX) and their AL answers */
#define IPX_FN_OPEN_SOCKET              0x00
#define IPX_FN_CLOSE_SOCKET             0x01
#define IPX_FN_GET_LOCAL_TARGET         0x02
#define IPX_FN_SEND                     0x03
#define IPX_FN_LISTEN                   0x04
#define IPX_FN_CANCEL                   0x06
#define IPX_FN_INTERVAL_MARKER          0x08
#define IPX_FN_INTERNETWORK_ADDRESS     0x09
#define IPX_FN_RELINQUISH               0x0A
#define IPX_RESULT_OK                   0x00
#define IPX_RESULT_CANNOT_CANCEL        0xF9
#define IPX_RESULT_TABLE_FULL           0xFE
#define IPX_RESULT_ALREADY_OPEN         0xFF
#define IPX_RESULT_NOT_OPEN             0xFF
#define IPX_RESULT_NOT_IN_USE           0xFF
#define IPX_DYNAMIC_SOCKET_FIRST        0x4000  /* Host order; dynamic sockets 4000h-7FFFh */
#define IPX_DYNAMIC_SOCKET_LAST         0x7FFF
#define IPX_TRANSPORT_TICKS             1       /* 02h's CX: one tick away -- the same network */

/* The host's IPX. Socket numbers are the WIRE's (big-endian) as the ECB holds them. */
typedef struct _IPX_BACKEND
{
    INT  (*Open)(PVOID context, WORD socket);                                   /* 0 = opened */
    VOID (*Close)(PVOID context, WORD socket);
    INT  (*Send)(PVOID context, WORD socket, PCBYTE packet, UINT32 length);     /* 0 = sent */
    /* Non-blocking: 1 and *length if a whole packet (header included) was taken, else 0. */
    INT  (*Receive)(PVOID context, WORD socket, BYTE *packet, UINT32 capacity, UINT32 *length);
    VOID (*LocalAddress)(PVOID context, BYTE address[IPX_ADDRESS_SIZE]);
    PVOID Context;
} IPX_BACKEND, *PIPX_BACKEND;

/* A guest real-mode segment:offset to host memory, or NULL. */
typedef PVOID (*PIPX_MAP_ROUTINE)(PVOID context, WORD segment, WORD offset);

typedef struct _IPX_LISTEN
{
    WORD Segment;               /* the ECB */
    WORD Offset;
    WORD Socket;
} IPX_LISTEN;

typedef struct _IPX_STATE
{
    PIPX_BACKEND Backend;       /* NULL = no IPX on this host */
    PIPX_MAP_ROUTINE Map;
    PVOID MapContext;
    WORD Sockets[IPX_SOCKETS_MAX];
    INT  SocketCount;
    IPX_LISTEN Listens[IPX_LISTENS_MAX];
    INT  ListenCount;
    WORD NextDynamicSocket;     /* host order */
    WORD IntervalMarker;        /* 08h: set by the host from 0040:006Ch before each call */
    UINT32 Calls;
    UINT32 PacketsSent;
    UINT32 PacketsReceived;
    UINT32 PacketsDropped;      /* arrived with no ECB listening */
    UINT32 EsrsSkipped;
    UINT32 UnsupportedCalls;
} IPX_STATE, *PIPX_STATE;

VOID VddIpxReset(_Inout_ PIPX_STATE state);

/* One far call to the entry. Registers in, registers out (AL/AX/CX/DX as the function says). */
VOID VddIpxCall(_Inout_ PIPX_STATE state, _Inout_ PNTVDD_REGISTERS registers);

/* Deliver arrived packets to the ECBs listening for them. Returns how many were delivered. */
INT VddIpxPoll(_Inout_ PIPX_STATE state);

#endif /* NTVDMEX_VDD_IPX_H */
