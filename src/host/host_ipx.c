/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The host's IPX: Winsock AF_IPX, loaded on first use, and the glue to the guest.
 *
 * The function definitions of host_ipx.h, which keeps their declarations and doc comments (#315).
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include "host_ipx.h"
#include "host_bios.h"
#include "host_strings.h"
#include "log.h"

/* Winsock, declared here rather than through winsock2.h (which must precede windows.h and
 * would bring a static import with it). Values from the Platform SDK's winsock2.h and
 * wsipx.h / wsnwlink.h.
 */
#define IPX_WINSOCK_VERSION     0x0202
#define IPX_AF_IPX              6
#define IPX_SOCK_DGRAM          2
#define IPX_NSPROTO_IPX         1000
#define IPX_SOL_SOCKET          0xFFFF
#define IPX_SO_BROADCAST        0x0020
#define IPX_FIONBIO             0x8004667EL
#define IPX_OPTION_PTYPE        0x4000  /* Wsnwlink.h IPX_PTYPE: the packet type sent */
#define IPX_OPTION_RECVHDR      0x4005  /* IPX_RECVHDR: recvfrom returns the IPX header too */
#define IPX_WSADATA_SIZE        512     /* WSADATA is 400 bytes; it is only written */
#define IPX_INVALID_SOCKET      (~(UINT_PTR)0)
#define IPX_SOCKET_ERROR        (-1)

typedef struct _IPX_SOCKADDR                    /* wsipx.h SOCKADDR_IPX: 14 bytes, no padding */
{
    SHORT Family;
    BYTE  Net[IPX_NET_SIZE];
    BYTE  Node[IPX_NODE_SIZE];
    BYTE  Socket[2];                            /* network order */
} IPX_SOCKADDR;

typedef INT      (WINAPI *PIPX_WSA_STARTUP)(WORD, PVOID);

typedef UINT_PTR (WINAPI *PIPX_SOCKET)(INT, INT, INT);

typedef INT      (WINAPI *PIPX_BIND)(UINT_PTR, const VOID *, INT);

typedef INT      (WINAPI *PIPX_CLOSE_SOCKET)(UINT_PTR);

typedef INT      (WINAPI *PIPX_SEND_TO)(UINT_PTR, const CHAR *, INT, INT, const VOID *, INT);

typedef INT      (WINAPI *PIPX_RECEIVE_FROM)(UINT_PTR, CHAR *, INT, INT, VOID *, INT *);

typedef INT      (WINAPI *PIPX_IOCTL_SOCKET)(UINT_PTR, LONG, ULONG *);

typedef INT      (WINAPI *PIPX_SET_SOCKET_OPTION)(UINT_PTR, INT, INT, const CHAR *, INT);

typedef INT      (WINAPI *PIPX_GET_SOCKET_NAME)(UINT_PTR, VOID *, INT *);

static INT IpxBackendOpen(PVOID context, WORD socket);
static VOID IpxBackendClose(PVOID context, WORD socket);
static INT IpxBackendSend(PVOID context, WORD socket, PCBYTE packet, UINT32 length);
static INT IpxBackendReceive(
    PVOID context,
    WORD socket,
    BYTE *packet,
    UINT32 capacity,
    UINT32 *length);
static VOID IpxBackendLocalAddress(PVOID context, BYTE address[IPX_ADDRESS_SIZE]);

IPX_STATE g_Ipx;

static struct
{
    PIPX_WSA_STARTUP Startup;
    PIPX_SOCKET Socket;
    PIPX_BIND Bind;
    PIPX_CLOSE_SOCKET Close;
    PIPX_SEND_TO SendTo;
    PIPX_RECEIVE_FROM ReceiveFrom;
    PIPX_IOCTL_SOCKET Ioctl;
    PIPX_SET_SOCKET_OPTION SetOption;
    PIPX_GET_SOCKET_NAME GetName;
} g_Winsock;

static struct
{
    WORD     Wire;                              /* the IPX socket number, host order */
    UINT_PTR Handle;
    INT      LastType;                          /* the IPX_PTYPE last set on it; -1 = none */
} g_IpxSockets[IPX_SOCKETS_MAX];

static BYTE g_IpxLocal[IPX_ADDRESS_SIZE];
static INT  g_IpxTried;
static INT  g_IpxAvailable;

static IPX_BACKEND g_IpxBackend = { IpxBackendOpen, IpxBackendClose, IpxBackendSend, IpxBackendReceive,
                                    IpxBackendLocalAddress, NULL };

static INT IpxSlotOf(WORD socket)
{
    INT index;

    for (index = 0; index < IPX_SOCKETS_MAX; ++index)
        if (g_IpxSockets[index].Handle && g_IpxSockets[index].Wire == socket)
            return index;

    return -1;
}

/* A datagram AF_IPX socket bound to `socket` (0 = any), non-blocking, broadcast-capable,
 * receiving the IPX header with each packet. IPX_INVALID_SOCKET if any step fails.
 */
static UINT_PTR IpxMakeSocket(WORD socket)
{
    UINT_PTR handle = g_Winsock.Socket(IPX_AF_IPX, IPX_SOCK_DGRAM, IPX_NSPROTO_IPX);
    IPX_SOCKADDR address;
    ULONG nonBlocking = 1;
    INT on = 1;

    if (handle == IPX_INVALID_SOCKET)
        return IPX_INVALID_SOCKET;

    ZeroMemory(&address, sizeof address);
    address.Family = IPX_AF_IPX;
    address.Socket[0] = (BYTE)(socket >> BYTE_SHIFT);
    address.Socket[1] = (BYTE)socket;

    if (g_Winsock.Bind(handle, &address, sizeof address) == IPX_SOCKET_ERROR
        || g_Winsock.Ioctl(handle, IPX_FIONBIO, &nonBlocking) == IPX_SOCKET_ERROR)
    {
        g_Winsock.Close(handle);
        return IPX_INVALID_SOCKET;
    }

    g_Winsock.SetOption(handle, IPX_SOL_SOCKET, IPX_SO_BROADCAST, (const CHAR *)&on, sizeof on);
    g_Winsock.SetOption(handle, IPX_NSPROTO_IPX, IPX_OPTION_RECVHDR, (const CHAR *)&on, sizeof on);
    return handle;
}

static INT IpxBackendOpen(PVOID context, WORD socket)
{
    INT index;
    UINT_PTR handle;

    (VOID)context;

    for (index = 0; index < IPX_SOCKETS_MAX; ++index)
        if (!g_IpxSockets[index].Handle)
            break;

    if (index == IPX_SOCKETS_MAX || (handle = IpxMakeSocket(socket)) == IPX_INVALID_SOCKET)
        return -1;

    g_IpxSockets[index].Wire = socket;
    g_IpxSockets[index].Handle = handle;
    g_IpxSockets[index].LastType = -1;
    return 0;
}

static VOID IpxBackendClose(PVOID context, WORD socket)
{
    INT index;

    (VOID)context;

    for (index = 0; index < IPX_SOCKETS_MAX; ++index)
    {
        if (g_IpxSockets[index].Handle && g_IpxSockets[index].Wire == socket)
        {
            g_Winsock.Close(g_IpxSockets[index].Handle);
            g_IpxSockets[index].Handle = 0;
        }
    }
}

/* Winsock builds the IPX header itself: the destination goes in the address, the packet
 * type in IPX_PTYPE, and only the data after the 30-byte header is sent.
 */
static INT IpxBackendSend(PVOID context, WORD socket, PCBYTE packet, UINT32 length)
{
    INT slot = IpxSlotOf(socket);
    UINT_PTR handle;
    IPX_SOCKADDR destination;
    INT type = packet[IPX_HEADER_TYPE];

    (VOID)context;

    if (slot < 0)
        return -1;

    handle = g_IpxSockets[slot].Handle;

    if (type != g_IpxSockets[slot].LastType)
    {
        g_Winsock.SetOption(handle, IPX_NSPROTO_IPX, IPX_OPTION_PTYPE, (const CHAR *)&type, sizeof type);
        g_IpxSockets[slot].LastType = type;
    }

    ZeroMemory(&destination, sizeof destination);
    destination.Family = IPX_AF_IPX;
    CopyMemory(destination.Net, packet + IPX_HEADER_DESTINATION, IPX_NET_SIZE);
    CopyMemory(destination.Node, packet + IPX_HEADER_DESTINATION + IPX_NET_SIZE, IPX_NODE_SIZE);
    destination.Socket[0] = packet[IPX_HEADER_DESTINATION_SOCKET];
    destination.Socket[1] = packet[IPX_HEADER_DESTINATION_SOCKET + 1];

    return g_Winsock.SendTo(handle, (const CHAR *)packet + IPX_HEADER_SIZE, (INT)(length - IPX_HEADER_SIZE), 0,
                            &destination, sizeof destination) == IPX_SOCKET_ERROR ? -1 : 0;
}

static INT IpxBackendReceive(
    PVOID context,
    WORD socket,
    BYTE *packet,
    UINT32 capacity,
    UINT32 *length)
{
    INT slot = IpxSlotOf(socket);
    IPX_SOCKADDR source;
    INT sourceLength = sizeof source;
    INT got;

    (VOID)context;

    if (slot < 0)
        return 0;

    got = g_Winsock.ReceiveFrom(g_IpxSockets[slot].Handle, (CHAR *)packet, (INT)capacity, 0, &source, &sourceLength);

    if (got < IPX_HEADER_SIZE)                 /* nothing waiting (WSAEWOULDBLOCK), or no header */
        return 0;

    *length = (UINT32)got;
    return 1;
}

static VOID IpxBackendLocalAddress(PVOID context, BYTE address[IPX_ADDRESS_SIZE])
{
    (VOID)context;
    CopyMemory(address, g_IpxLocal, IPX_ADDRESS_SIZE);
}

static PVOID IpxMapReal(PVOID context, WORD segment, WORD offset)
{
    (VOID)context;
    return VddMapFlat(&g_Bus, segment, offset);
}

INT HostIpxInstallCheck(VOID)
{
    if (!g_IpxTried)
    {
        HMODULE module;
        CHAR lineBuffer[160];
        CHAR *lineCursor = lineBuffer;
        BYTE wsaData[IPX_WSADATA_SIZE];

        g_IpxTried = 1;
        module = LoadLibraryA(HOST_MODULE_WS2_32);

        if (module)
        {
            g_Winsock.Startup = (PIPX_WSA_STARTUP)(VOID *)GetProcAddress(module, HOST_EXPORT_WSA_STARTUP);
            g_Winsock.Socket = (PIPX_SOCKET)(VOID *)GetProcAddress(module, HOST_EXPORT_SOCKET);
            g_Winsock.Bind = (PIPX_BIND)(VOID *)GetProcAddress(module, HOST_EXPORT_BIND);
            g_Winsock.Close = (PIPX_CLOSE_SOCKET)(VOID *)GetProcAddress(module, HOST_EXPORT_CLOSE_SOCKET);
            g_Winsock.SendTo = (PIPX_SEND_TO)(VOID *)GetProcAddress(module, HOST_EXPORT_SEND_TO);
            g_Winsock.ReceiveFrom = (PIPX_RECEIVE_FROM)(VOID *)GetProcAddress(module, HOST_EXPORT_RECEIVE_FROM);
            g_Winsock.Ioctl = (PIPX_IOCTL_SOCKET)(VOID *)GetProcAddress(module, HOST_EXPORT_IOCTL_SOCKET);
            g_Winsock.SetOption = (PIPX_SET_SOCKET_OPTION)(VOID *)GetProcAddress(module, HOST_EXPORT_SET_SOCKET_OPTION);
            g_Winsock.GetName = (PIPX_GET_SOCKET_NAME)(VOID *)GetProcAddress(module, HOST_EXPORT_GET_SOCKET_NAME);
        }

        /* NWLink is the test: an AF_IPX socket bound to "any" exists, and its name is our
         * internetwork address (net, node).
         */
        if (g_Winsock.Startup && g_Winsock.Socket && g_Winsock.Bind && g_Winsock.Close && g_Winsock.SendTo
            && g_Winsock.ReceiveFrom && g_Winsock.Ioctl && g_Winsock.SetOption && g_Winsock.GetName
            && g_Winsock.Startup(IPX_WINSOCK_VERSION, wsaData) == 0)
        {
            UINT_PTR probe = IpxMakeSocket(0);

            if (probe != IPX_INVALID_SOCKET)
            {
                IPX_SOCKADDR name;
                INT nameLength = sizeof name;

                if (g_Winsock.GetName(probe, &name, &nameLength) == 0)
                {
                    CopyMemory(g_IpxLocal, name.Net, IPX_NET_SIZE);
                    CopyMemory(g_IpxLocal + IPX_NET_SIZE, name.Node, IPX_NODE_SIZE);
                    g_IpxAvailable = 1;
                }

                g_Winsock.Close(probe);
            }
        }

        if (g_IpxAvailable)
        {
            g_Ipx.Backend = &g_IpxBackend;
            g_Ipx.Map = IpxMapReal;
            VddIpxReset(&g_Ipx);
        }

        lineCursor = LogPut(lineCursor, g_IpxAvailable ? "IPX: installed (Winsock AF_IPX -- NWLink)\r\n"
                                                       : "IPX: not installed (no AF_IPX on this host: NWLink IPX is not installed)\r\n");
        LogAppend(LOG_PATH, lineBuffer, lineCursor);
    }

    return g_IpxAvailable;
}

VOID HostIpxEntry(volatile BYTE *tib)
{
    NTVDD_REGISTERS registers;

    RegistersLoad(&registers, tib);
    g_Ipx.IntervalMarker = *(volatile WORD *)(ULONG_PTR)(BIOS_BDA_BASE + BIOS_BDA_TICK_COUNT);
    HOST_LOCK();
    VddIpxCall(&g_Ipx, &registers);
    HOST_UNLOCK();
    RegistersStore(&registers, tib);
}

VOID HostIpxTick(VOID)
{
    if (g_Ipx.Backend && g_Ipx.SocketCount)
    {
        HOST_LOCK();
        VddIpxPoll(&g_Ipx);
        HOST_UNLOCK();
    }
}
