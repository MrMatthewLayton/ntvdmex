/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The IPX API (src/vdd/vdd_ipx.c), off-VM.  #315.
 *
 * The model's job is the DOS side: the ECB, the socket table, the listen queue, the
 * IPX header IPX fills on send. A fake LOOPBACK backend stands in for Winsock AF_IPX:
 * whatever is sent to a socket is queued for that socket, so a packet a program sends
 * to itself comes back through its own listening ECB -- every byte crossing checked.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include <stdio.h>
#include <string.h>
#include "vdd_ipx.h"
#define CHECK(condition,message) do{ g_Total++; if(condition){printf("  PASS  %s\n",(message));} \
    else{printf("  FAIL  %s\n",(message)); g_Failures++;} }while(0)

/* the loopback backend: */
#define IPX_TEST_QUEUE  8

static INT g_Total = 0;
static INT g_Failures = 0;

static BYTE g_Memory[0x20000];                       /* the guest's first 128 KB */
static BYTE   g_Queue[IPX_TEST_QUEUE][IPX_PACKET_MAX];
static UINT32 g_QueueLength[IPX_TEST_QUEUE];
static WORD   g_QueueSocket[IPX_TEST_QUEUE];
static INT    g_QueueCount;
static INT    g_Opened;
static INT    g_Closed;
static INT    g_RefuseOpen;
static const BYTE g_LocalAddress[IPX_ADDRESS_SIZE] = { 0x00, 0x00, 0x00, 0x01, 0x02, 0x60, 0x8C, 0x11, 0x22, 0x33 };

static INT IpxTestOpen(PVOID context, WORD socket)
{
    (VOID)context;
    (VOID)socket;

    if (g_RefuseOpen)
        return -1;

    ++g_Opened;
    return 0;
}

static VOID IpxTestClose(PVOID context, WORD socket)
{
    (VOID)context;
    (VOID)socket;
    ++g_Closed;
}

static INT IpxTestSend(PVOID context, WORD socket, PCBYTE packet, UINT32 length)
{
    (VOID)context;
    (VOID)socket;

    if (g_QueueCount >= IPX_TEST_QUEUE)
        return -1;

    memcpy(g_Queue[g_QueueCount], packet, length);
    g_QueueLength[g_QueueCount] = length;
    g_QueueSocket[g_QueueCount] = (WORD)((packet[IPX_HEADER_DESTINATION_SOCKET] << 8) | packet[IPX_HEADER_DESTINATION_SOCKET + 1]);
    ++g_QueueCount;
    return 0;
}

static INT IpxTestReceive(PVOID context, WORD socket, BYTE *packet, UINT32 capacity, UINT32 *length)
{
    INT index;

    (VOID)context;

    for (index = 0; index < g_QueueCount; ++index)
    {
        if (g_QueueSocket[index] == socket && g_QueueLength[index] <= capacity)
        {
            memcpy(packet, g_Queue[index], g_QueueLength[index]);
            *length = g_QueueLength[index];
            memmove(&g_Queue[index], &g_Queue[index + 1], (SIZE_T)(g_QueueCount - index - 1) * IPX_PACKET_MAX);
            memmove(&g_QueueLength[index], &g_QueueLength[index + 1], (SIZE_T)(g_QueueCount - index - 1) * sizeof g_QueueLength[0]);
            memmove(&g_QueueSocket[index], &g_QueueSocket[index + 1], (SIZE_T)(g_QueueCount - index - 1) * sizeof g_QueueSocket[0]);
            --g_QueueCount;
            return 1;
        }
    }

    return 0;
}

static VOID IpxTestLocalAddress(PVOID context, BYTE address[IPX_ADDRESS_SIZE])
{
    (VOID)context;
    memcpy(address, g_LocalAddress, IPX_ADDRESS_SIZE);
}

static PVOID IpxTestMap(PVOID context, WORD segment, WORD offset)
{
    UINT32 linear = ((UINT32)segment << 4) + offset;

    (VOID)context;
    return linear < sizeof g_Memory ? &g_Memory[linear] : NULL;
}

/* An ECB at 1000:<ecbOffset> for `socket` (host order) with one fragment at 1000:<data>. */
static BYTE *IpxTestEcb(WORD ecbOffset, WORD socket, WORD data, WORD size)
{
    BYTE *ecb = &g_Memory[0x10000 + ecbOffset];

    memset(ecb, 0, IPX_ECB_FRAGMENTS + IPX_ECB_FRAGMENT_SIZE);
    ecb[IPX_ECB_SOCKET] = (BYTE)(socket >> 8);
    ecb[IPX_ECB_SOCKET + 1] = (BYTE)socket;
    ecb[IPX_ECB_FRAGMENT_COUNT] = 1;
    ecb[IPX_ECB_FRAGMENTS] = (BYTE)data;
    ecb[IPX_ECB_FRAGMENTS + 1] = (BYTE)(data >> 8);
    ecb[IPX_ECB_FRAGMENTS + 2] = 0x00;
    ecb[IPX_ECB_FRAGMENTS + 3] = 0x10;              /* segment 1000h */
    ecb[IPX_ECB_FRAGMENTS + 4] = (BYTE)size;
    ecb[IPX_ECB_FRAGMENTS + 5] = (BYTE)(size >> 8);
    return ecb;
}

static VOID IpxTestCall(PIPX_STATE state, NTVDD_REGISTERS *registers, WORD function, WORD si)
{
    registers->Ebx = function;
    registers->Es = 0x1000;
    registers->Esi = si;
    VddIpxCall(state, registers);
}

INT main(VOID)
{
    IPX_STATE state;
    IPX_BACKEND backend = { IpxTestOpen, IpxTestClose, IpxTestSend, IpxTestReceive, IpxTestLocalAddress, NULL };
    NTVDD_REGISTERS registers;
    BYTE *ecb;
    BYTE *packet;

    memset(&state, 0, sizeof state);
    memset(&registers, 0, sizeof registers);
    state.Map = IpxTestMap;
    VddIpxReset(&state);

    /* No backend: the entry changes nothing (the install check said "not installed"). */
    registers.Eax = 0x1234;
    registers.Edx = 0x5560;
    IpxTestCall(&state, &registers, IPX_FN_OPEN_SOCKET, 0);
    CHECK(registers.Eax == 0x1234 && registers.Edx == 0x5560, "no backend: open socket leaves the registers alone");

    state.Backend = &backend;

    /* 00h open: socket 869Ch, given in DX "big-endian" (DL = 86h, DH = 9Ch). */
    registers.Edx = 0x9C86;
    IpxTestCall(&state, &registers, IPX_FN_OPEN_SOCKET, 0);
    CHECK((registers.Eax & 0xFF) == IPX_RESULT_OK && registers.Edx == 0x9C86 && g_Opened == 1, "00h: socket 869Ch opens, DX as given");
    IpxTestCall(&state, &registers, IPX_FN_OPEN_SOCKET, 0);
    CHECK((registers.Eax & 0xFF) == IPX_RESULT_ALREADY_OPEN, "00h: the same socket again -> FFh");
    registers.Edx = 0;
    IpxTestCall(&state, &registers, IPX_FN_OPEN_SOCKET, 0);
    CHECK((registers.Eax & 0xFF) == IPX_RESULT_OK && registers.Edx == 0x0040, "00h: DX=0 assigns a dynamic socket (4000h, DX=0040h)");
    g_RefuseOpen = 1;
    registers.Edx = 0x1111;
    IpxTestCall(&state, &registers, IPX_FN_OPEN_SOCKET, 0);
    CHECK((registers.Eax & 0xFF) == IPX_RESULT_TABLE_FULL, "00h: the backend refuses -> FEh");
    g_RefuseOpen = 0;

    /* 09h: the internetwork address. */
    IpxTestCall(&state, &registers, IPX_FN_INTERNETWORK_ADDRESS, 0x0800);
    CHECK(memcmp(&g_Memory[0x10800], g_LocalAddress, IPX_ADDRESS_SIZE) == 0, "09h: net and node written at ES:SI");

    /* 02h: local target = the destination node, transport time 1 tick. */
    memset(&g_Memory[0x10900], 0, 12);
    memcpy(&g_Memory[0x10904], "\x01\x02\x03\x04\x05\x06", 6);
    registers.Edi = 0x0920;
    IpxTestCall(&state, &registers, IPX_FN_GET_LOCAL_TARGET, 0x0900);
    CHECK(memcmp(&g_Memory[0x10920], "\x01\x02\x03\x04\x05\x06", 6) == 0 && (registers.Ecx & 0xFFFF) == IPX_TRANSPORT_TICKS,
          "02h: the immediate address is the node; CX = 1 tick");

    /* 04h listen on 869Ch: in-use FEh. */
    ecb = IpxTestEcb(0x0000, 0x869C, 0x0200, 100);
    IpxTestCall(&state, &registers, IPX_FN_LISTEN, 0x0000);
    CHECK((registers.Eax & 0xFF) == IPX_RESULT_OK && ecb[IPX_ECB_IN_USE] == IPX_IN_USE_LISTENING, "04h: listening, in-use FEh");

    /* 03h send to ourselves: header filled by IPX, delivered to the listening ECB. */
    packet = &g_Memory[0x10400];
    memset(packet, 0xAA, 40);
    packet[IPX_HEADER_TYPE] = 4;
    memcpy(packet + IPX_HEADER_DESTINATION, g_LocalAddress, IPX_ADDRESS_SIZE);
    packet[IPX_HEADER_DESTINATION_SOCKET] = 0x86;
    packet[IPX_HEADER_DESTINATION_SOCKET + 1] = 0x9C;
    memcpy(packet + IPX_HEADER_SIZE, "HELLO", 5);
    ecb = IpxTestEcb(0x0100, 0x869C, 0x0400, IPX_HEADER_SIZE + 5);
    g_Memory[0x10100 + IPX_ECB_ESR_OFFSET] = 0x34;      /* an ESR: not called, counted */
    IpxTestCall(&state, &registers, IPX_FN_SEND, 0x0100);
    CHECK(ecb[IPX_ECB_IN_USE] == IPX_IN_USE_FREE && ecb[IPX_ECB_COMPLETION] == IPX_COMPLETION_OK, "03h: sent, in-use 00h, completion 00h");
    CHECK(state.EsrsSkipped == 1, "03h: the ECB's ESR is counted, not called");
    {   BYTE *received = &g_Memory[0x10200];
        BYTE *listener = &g_Memory[0x10000];
        CHECK(listener[IPX_ECB_IN_USE] == IPX_IN_USE_FREE && listener[IPX_ECB_COMPLETION] == IPX_COMPLETION_OK,
              "the listening ECB completes (the send's poll delivered it)");
        CHECK(received[IPX_HEADER_CHECKSUM] == 0xFF && received[IPX_HEADER_CHECKSUM + 1] == 0xFF, "header: checksum FFFFh");
        CHECK(received[IPX_HEADER_LENGTH] == 0 && received[IPX_HEADER_LENGTH + 1] == IPX_HEADER_SIZE + 5, "header: length 35, big-endian");
        CHECK(received[IPX_HEADER_TRANSPORT] == 0 && received[IPX_HEADER_TYPE] == 4, "header: transport 0, the program's packet type");
        CHECK(memcmp(received + IPX_HEADER_SOURCE, g_LocalAddress, IPX_ADDRESS_SIZE) == 0
              && received[IPX_HEADER_SOURCE_SOCKET] == 0x86 && received[IPX_HEADER_SOURCE_SOCKET + 1] == 0x9C,
              "header: source = our address and socket");
        CHECK(memcmp(received + IPX_HEADER_SIZE, "HELLO", 5) == 0, "the data arrives after the header");
        CHECK(memcmp(listener + IPX_ECB_IMMEDIATE_ADDRESS, g_LocalAddress + IPX_NET_SIZE, IPX_NODE_SIZE) == 0,
              "the listener's immediate address is the sender's node"); }

    /* A packet with nobody listening is dropped, as IPX does. */
    IpxTestCall(&state, &registers, IPX_FN_SEND, 0x0100);
    CHECK(state.PacketsDropped == 1 && g_QueueCount == 0, "a packet with no ECB listening is dropped");

    /* Overflow: a listener too small for the packet -> FDh. */
    IpxTestEcb(0x0000, 0x869C, 0x0200, 10);
    IpxTestCall(&state, &registers, IPX_FN_LISTEN, 0x0000);
    IpxTestCall(&state, &registers, IPX_FN_SEND, 0x0100);
    CHECK(g_Memory[0x10000 + IPX_ECB_COMPLETION] == IPX_COMPLETION_MALFORMED, "a packet larger than the fragments -> completion FDh");

    /* Malformed send: fewer bytes than a header. */
    ecb = IpxTestEcb(0x0100, 0x869C, 0x0400, 10);
    IpxTestCall(&state, &registers, IPX_FN_SEND, 0x0100);
    CHECK(ecb[IPX_ECB_COMPLETION] == IPX_COMPLETION_MALFORMED, "03h: less than a header -> FDh");

    /* Send on a socket that is not open -> FFh. */
    ecb = IpxTestEcb(0x0100, 0x7777, 0x0400, IPX_HEADER_SIZE);
    IpxTestCall(&state, &registers, IPX_FN_SEND, 0x0100);
    CHECK(ecb[IPX_ECB_COMPLETION] == IPX_COMPLETION_FAILURE, "03h: socket not open -> FFh");

    /* 04h on a socket that is not open -> AL=FFh. */
    ecb = IpxTestEcb(0x0300, 0x7777, 0x0400, 50);
    IpxTestCall(&state, &registers, IPX_FN_LISTEN, 0x0300);
    CHECK((registers.Eax & 0xFF) == IPX_RESULT_NOT_OPEN, "04h: socket not open -> FFh");

    /* 06h cancel a listener, then a free ECB. */
    ecb = IpxTestEcb(0x0300, 0x869C, 0x0400, 50);
    IpxTestCall(&state, &registers, IPX_FN_LISTEN, 0x0300);
    IpxTestCall(&state, &registers, IPX_FN_CANCEL, 0x0300);
    CHECK((registers.Eax & 0xFF) == IPX_RESULT_OK && ecb[IPX_ECB_COMPLETION] == IPX_COMPLETION_CANCELLED && state.ListenCount == 0,
          "06h: a listener is cancelled (FCh)");
    IpxTestCall(&state, &registers, IPX_FN_CANCEL, 0x0300);
    CHECK((registers.Eax & 0xFF) == IPX_RESULT_NOT_IN_USE, "06h: an ECB not in use -> FFh");

    /* 08h interval marker. */
    state.IntervalMarker = 0x1234;
    IpxTestCall(&state, &registers, IPX_FN_INTERVAL_MARKER, 0);
    CHECK((registers.Eax & 0xFFFF) == 0x1234, "08h: AX = the BIOS tick");

    /* 01h close: its listeners end, cancelled. */
    ecb = IpxTestEcb(0x0300, 0x869C, 0x0400, 50);
    IpxTestCall(&state, &registers, IPX_FN_LISTEN, 0x0300);
    registers.Edx = 0x9C86;
    IpxTestCall(&state, &registers, IPX_FN_CLOSE_SOCKET, 0);
    CHECK(g_Closed == 1 && ecb[IPX_ECB_COMPLETION] == IPX_COMPLETION_CANCELLED && ecb[IPX_ECB_IN_USE] == IPX_IN_USE_FREE,
          "01h: closing ends its listeners (FCh)");

    /* Unsupported: SPX establish (11h) is counted, registers alone. */
    registers.Eax = 0x00AB;
    IpxTestCall(&state, &registers, 0x11, 0);
    CHECK(state.UnsupportedCalls == 1 && (registers.Eax & 0xFF) == 0xAB, "unsupported function: counted, AL as passed");

    printf("\n== %d checks, %d failed\n", g_Total, g_Failures);
    return g_Failures ? 1 : 0;
}
