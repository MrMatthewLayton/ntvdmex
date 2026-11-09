/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The IPX API: sockets, ECBs, send and listen, over a host backend.
 *
 * The function definitions of vdd_ipx.h, which keeps their declarations and doc comments (#315).
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include "vdd_ipx.h"

#define IPX_HIGH_BYTE       1
#define IPX_CHECKSUM_NONE   0xFFFF

/* A socket number as the wire has it, from the ECB's two bytes (big-endian). */
static WORD IpxWireWord(PCBYTE bytes)
{
    return (WORD)((bytes[0] << BYTE_SHIFT) | bytes[IPX_HIGH_BYTE]);
}

static VOID IpxPutWireWord(BYTE *bytes, WORD value)
{
    bytes[0] = (BYTE)(value >> BYTE_SHIFT);
    bytes[IPX_HIGH_BYTE] = (BYTE)value;
}

static WORD IpxLittleWord(PCBYTE bytes)
{
    return (WORD)(bytes[0] | (bytes[IPX_HIGH_BYTE] << BYTE_SHIFT));
}

/* DX carries a socket "big-endian": DL is the wire's high byte. */
static WORD IpxSocketFromDx(WORD dx)
{
    return (WORD)((dx << BYTE_SHIFT) | (dx >> BYTE_SHIFT));
}

static BYTE *IpxMap(PIPX_STATE state, WORD segment, WORD offset)
{
    return state->Map ? (BYTE *)state->Map(state->MapContext, segment, offset) : NULL;
}

static INT IpxFindSocket(PIPX_STATE state, WORD socket)
{
    INT index;

    for (index = 0; index < state->SocketCount; ++index)
        if (state->Sockets[index] == socket)
            return index;

    return -1;
}

/* An event happened to this ECB: final bytes in place, and its ESR (not called) counted. */
static VOID IpxComplete(PIPX_STATE state, BYTE *ecb, BYTE completion)
{
    ecb[IPX_ECB_COMPLETION] = completion;
    ecb[IPX_ECB_IN_USE] = IPX_IN_USE_FREE;

    if (IpxLittleWord(ecb + IPX_ECB_ESR_OFFSET) | IpxLittleWord(ecb + IPX_ECB_ESR_SEGMENT))
        ++state->EsrsSkipped;
}

/* Remove listen `index`, keeping the posting order (the first posted is filled first). */
static VOID IpxDropListen(PIPX_STATE state, INT index)
{
    INT move;

    for (move = index; move + 1 < state->ListenCount; ++move)
        state->Listens[move] = state->Listens[move + 1];

    --state->ListenCount;
}

/* The fragments of `ecb`, summed and checked; 0 if malformed. */
static UINT32 IpxFragmentTotal(PIPX_STATE state, PCBYTE ecb)
{
    WORD count = IpxLittleWord(ecb + IPX_ECB_FRAGMENT_COUNT);
    UINT32 total = 0;
    WORD index;

    if (count == 0 || count > IPX_ECB_FRAGMENTS_MAX)
        return 0;

    for (index = 0; index < count; ++index)
    {
        PCBYTE fragment = ecb + IPX_ECB_FRAGMENTS + index * IPX_ECB_FRAGMENT_SIZE;
        WORD size = IpxLittleWord(fragment + 4);

        if (size && !IpxMap(state, IpxLittleWord(fragment + 2), IpxLittleWord(fragment)))
            return 0;

        total += size;
    }

    return total;
}

static VOID IpxOpenSocket(PIPX_STATE state, PNTVDD_REGISTERS registers)
{
    WORD socket = IpxSocketFromDx(VddGetDx(registers));

    if (socket == 0)                                       /* dynamic: the next free one */
    {
        INT tries;

        for (tries = 0; tries <= IPX_DYNAMIC_SOCKET_LAST - IPX_DYNAMIC_SOCKET_FIRST; ++tries)
        {
            WORD candidate = state->NextDynamicSocket;
            state->NextDynamicSocket = (WORD)(candidate >= IPX_DYNAMIC_SOCKET_LAST ? IPX_DYNAMIC_SOCKET_FIRST : candidate + 1);

            if (IpxFindSocket(state, candidate) < 0)
            {
                socket = candidate;
                break;
            }
        }
    }
    else if (IpxFindSocket(state, socket) >= 0)
    {
        VddSetAl(registers, IPX_RESULT_ALREADY_OPEN);
        return;
    }

    if (socket == 0 || state->SocketCount >= IPX_SOCKETS_MAX
        || state->Backend->Open(state->Backend->Context, socket) != 0)
    {
        VddSetAl(registers, IPX_RESULT_TABLE_FULL);
        return;
    }

    state->Sockets[state->SocketCount++] = socket;
    VddSetDx(registers, IpxSocketFromDx(socket));                 /* back in DX's byte order */
    VddSetAl(registers, IPX_RESULT_OK);
}

static VOID IpxCloseSocket(PIPX_STATE state, PNTVDD_REGISTERS registers)
{
    WORD socket = IpxSocketFromDx(VddGetDx(registers));
    INT found = IpxFindSocket(state, socket);
    INT index;

    if (found < 0)
        return;

    state->Backend->Close(state->Backend->Context, socket);
    state->Sockets[found] = state->Sockets[--state->SocketCount];

    for (index = 0; index < state->ListenCount; )            /* its listeners end, cancelled */
    {
        if (state->Listens[index].Socket == socket)
        {
            BYTE *ecb = IpxMap(state, state->Listens[index].Segment, state->Listens[index].Offset);

            if (ecb)
                IpxComplete(state, ecb, IPX_COMPLETION_CANCELLED);

            IpxDropListen(state, index);
        }
        else
            ++index;
    }
}

static VOID IpxSend(PIPX_STATE state, PNTVDD_REGISTERS registers)
{
    BYTE *ecb = IpxMap(state, registers->Es, (WORD)registers->Esi);
    BYTE packet[IPX_PACKET_MAX];
    UINT32 total;
    UINT32 length = 0;
    WORD socket;
    WORD count;
    WORD index;
    BYTE address[IPX_ADDRESS_SIZE];

    if (!ecb)
        return;

    socket = IpxWireWord(ecb + IPX_ECB_SOCKET);
    total = IpxFragmentTotal(state, ecb);

    if (IpxFindSocket(state, socket) < 0)
    {
        IpxComplete(state, ecb, IPX_COMPLETION_FAILURE);
        return;
    }

    if (total < IPX_HEADER_SIZE || total > IPX_PACKET_MAX)
    {
        IpxComplete(state, ecb, IPX_COMPLETION_MALFORMED);
        return;
    }

    count = IpxLittleWord(ecb + IPX_ECB_FRAGMENT_COUNT);

    for (index = 0; index < count; ++index)                  /* gather */
    {
        PCBYTE fragment = ecb + IPX_ECB_FRAGMENTS + index * IPX_ECB_FRAGMENT_SIZE;
        WORD size = IpxLittleWord(fragment + 4);
        PCBYTE source = size ? IpxMap(state, IpxLittleWord(fragment + 2), IpxLittleWord(fragment)) : NULL;
        WORD byteIndex;

        for (byteIndex = 0; byteIndex < size; ++byteIndex)
            packet[length++] = source[byteIndex];
    }

    /* What IPX fills on send: checksum, length, transport control, the source address. */
    IpxPutWireWord(packet + IPX_HEADER_CHECKSUM, IPX_CHECKSUM_NONE);
    IpxPutWireWord(packet + IPX_HEADER_LENGTH, (WORD)length);
    packet[IPX_HEADER_TRANSPORT] = 0;
    state->Backend->LocalAddress(state->Backend->Context, address);

    for (index = 0; index < IPX_ADDRESS_SIZE; ++index)
        packet[IPX_HEADER_SOURCE + index] = address[index];

    IpxPutWireWord(packet + IPX_HEADER_SOURCE_SOCKET, socket);
    ecb[IPX_ECB_IN_USE] = IPX_IN_USE_SENDING;

    if (state->Backend->Send(state->Backend->Context, socket, packet, length) != 0)
    {
        IpxComplete(state, ecb, IPX_COMPLETION_UNDELIVERABLE);
        return;
    }

    ++state->PacketsSent;
    IpxComplete(state, ecb, IPX_COMPLETION_OK);
}

static VOID IpxListen(PIPX_STATE state, PNTVDD_REGISTERS registers)
{
    BYTE *ecb = IpxMap(state, registers->Es, (WORD)registers->Esi);
    WORD socket;

    if (!ecb)
        return;

    socket = IpxWireWord(ecb + IPX_ECB_SOCKET);

    if (IpxFindSocket(state, socket) < 0)
    {
        IpxComplete(state, ecb, IPX_COMPLETION_FAILURE);
        VddSetAl(registers, IPX_RESULT_NOT_OPEN);
        return;
    }

    if (state->ListenCount >= IPX_LISTENS_MAX || !IpxFragmentTotal(state, ecb))
    {
        IpxComplete(state, ecb, IPX_COMPLETION_MALFORMED);
        VddSetAl(registers, IPX_RESULT_OK);
        return;
    }

    ecb[IPX_ECB_IN_USE] = IPX_IN_USE_LISTENING;
    state->Listens[state->ListenCount].Segment = registers->Es;
    state->Listens[state->ListenCount].Offset = (WORD)registers->Esi;
    state->Listens[state->ListenCount].Socket = socket;
    ++state->ListenCount;
    VddSetAl(registers, IPX_RESULT_OK);
}

static VOID IpxCancel(PIPX_STATE state, PNTVDD_REGISTERS registers)
{
    BYTE *ecb = IpxMap(state, registers->Es, (WORD)registers->Esi);
    INT index;

    if (!ecb || ecb[IPX_ECB_IN_USE] == IPX_IN_USE_FREE)
    {
        VddSetAl(registers, IPX_RESULT_NOT_IN_USE);
        return;
    }

    for (index = 0; index < state->ListenCount; ++index)
    {
        if (state->Listens[index].Segment == registers->Es && state->Listens[index].Offset == (WORD)registers->Esi)
        {
            IpxDropListen(state, index);
            IpxComplete(state, ecb, IPX_COMPLETION_CANCELLED);
            VddSetAl(registers, IPX_RESULT_OK);
            return;
        }
    }

    VddSetAl(registers, IPX_RESULT_CANNOT_CANCEL);
}

static VOID IpxGetLocalTarget(PIPX_STATE state, PNTVDD_REGISTERS registers)
{
    PCBYTE destination = IpxMap(state, registers->Es, (WORD)registers->Esi);
    BYTE *immediate = IpxMap(state, registers->Es, (WORD)registers->Edi);
    INT index;

    if (!destination || !immediate)
        return;

    /* One network: the immediate address of a node is the node itself. */
    for (index = 0; index < IPX_NODE_SIZE; ++index)
        immediate[index] = destination[IPX_NET_SIZE + index];

    VddSetCx(registers, IPX_TRANSPORT_TICKS);
    VddSetAl(registers, IPX_RESULT_OK);
}

VOID VddIpxReset(PIPX_STATE state)
{
    state->SocketCount = 0;
    state->ListenCount = 0;
    state->NextDynamicSocket = IPX_DYNAMIC_SOCKET_FIRST;
}

VOID VddIpxCall(PIPX_STATE state, PNTVDD_REGISTERS registers)
{
    ++state->Calls;

    if (!state->Backend)
        return;

    if (!state->NextDynamicSocket)
        state->NextDynamicSocket = IPX_DYNAMIC_SOCKET_FIRST;

    switch (VddGetBx(registers))
    {
    case IPX_FN_OPEN_SOCKET:
        IpxOpenSocket(state, registers);
        break;

    case IPX_FN_CLOSE_SOCKET:
        IpxCloseSocket(state, registers);
        break;

    case IPX_FN_GET_LOCAL_TARGET:
        IpxGetLocalTarget(state, registers);
        break;

    case IPX_FN_SEND:
        IpxSend(state, registers);
        break;

    case IPX_FN_LISTEN:
        IpxListen(state, registers);
        break;

    case IPX_FN_CANCEL:
        IpxCancel(state, registers);
        break;

    case IPX_FN_INTERVAL_MARKER:
        VddSetAx(registers, state->IntervalMarker);
        break;

    case IPX_FN_INTERNETWORK_ADDRESS:
    {   BYTE *address = IpxMap(state, registers->Es, (WORD)registers->Esi);

        if (address)
            state->Backend->LocalAddress(state->Backend->Context, address);

        break; }

    case IPX_FN_RELINQUISH:
        break;                                       /* the poll below is the work */

    default:
        ++state->UnsupportedCalls;
        break;
    }

    VddIpxPoll(state);
}

INT VddIpxPoll(PIPX_STATE state)
{
    BYTE packet[IPX_PACKET_MAX];
    INT delivered = 0;
    INT socketIndex;

    if (!state->Backend)
        return 0;

    for (socketIndex = 0; socketIndex < state->SocketCount; ++socketIndex)
    {
        WORD socket = state->Sockets[socketIndex];
        UINT32 length;

        while (state->Backend->Receive(state->Backend->Context, socket, packet, sizeof packet, &length))
        {
            INT listenIndex;
            BYTE *ecb = NULL;
            WORD count;
            WORD fragmentIndex;
            UINT32 copied = 0;

            for (listenIndex = 0; listenIndex < state->ListenCount; ++listenIndex)
                if (state->Listens[listenIndex].Socket == socket)
                    break;

            if (listenIndex == state->ListenCount)
            {
                ++state->PacketsDropped;                      /* nobody listening: IPX drops it */
                continue;
            }

            ecb = IpxMap(state, state->Listens[listenIndex].Segment, state->Listens[listenIndex].Offset);
            IpxDropListen(state, listenIndex);

            if (!ecb)
                continue;

            count = IpxLittleWord(ecb + IPX_ECB_FRAGMENT_COUNT);

            for (fragmentIndex = 0; fragmentIndex < count && copied < length; ++fragmentIndex)    /* scatter */
            {
                PCBYTE fragment = ecb + IPX_ECB_FRAGMENTS + fragmentIndex * IPX_ECB_FRAGMENT_SIZE;
                WORD size = IpxLittleWord(fragment + 4);
                BYTE *target = size ? IpxMap(state, IpxLittleWord(fragment + 2), IpxLittleWord(fragment)) : NULL;
                WORD byteIndex;

                for (byteIndex = 0; byteIndex < size && copied < length && target; ++byteIndex)
                    target[byteIndex] = packet[copied++];
            }

            if (length >= IPX_HEADER_SIZE)                   /* who sent it: the immediate address */
            {
                INT index;

                for (index = 0; index < IPX_NODE_SIZE; ++index)
                    ecb[IPX_ECB_IMMEDIATE_ADDRESS + index] = packet[IPX_HEADER_SOURCE_NODE + index];
            }

            ++state->PacketsReceived;
            ++delivered;
            IpxComplete(state, ecb, copied < length ? IPX_COMPLETION_MALFORMED : IPX_COMPLETION_OK);   /* FDh: overflow */
        }
    }

    return delivered;
}
