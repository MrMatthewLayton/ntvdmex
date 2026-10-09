/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * NetBIOS through INT 5Ch: the network interface a DOS program under NT
 * has.  GH #8, s91.
 *
 * WHY NetBIOS AND NOT A PACKET DRIVER. #8 named "packet driver / NDIS-era interface
 * as appropriate". The reference decides what is appropriate: stock XP NTVDM answers
 * INT 5Ch by handing the program's NCB to the NT NetBIOS driver (tests/probes/dos/p_netb
 * measured it), and it offers no packet driver -- that needs raw Ethernet frames, which
 * a user-mode process on XP cannot send without a capture driver. NetBIOS is what DOS
 * network programs of the era (and LAN Manager / Novell NetBIOS clients) speak.
 *
 * SHAPE. The device owns the DOS side: it reads the 64-byte NCB at ES:BX, resolves its
 * buffer, hands a host-neutral NETBIOS_REQUEST to a BACKEND, and writes the answer back where
 * DOS keeps it (retcode, lsn, num, length, callname, cmd_cplt). The backend is the
 * host's (main.c: Win32 Netbios() in netapi32.dll); the off-VM test gives it a fake.
 *
 * The DOS NCB (IBM NetBIOS Technical Reference; identical in every DOS NetBIOS):
 * +00 command   +01 retcode  +02 lsn  +03 num  +04 buffer (far)  +08 length
 * +0A callname[16]  +1A name[16]  +2A rto  +2B sto  +2C post (far)  +30 lana
 * +31 cmd_cplt  +32 reserved[14]
 * A command with bit 7 set is NO-WAIT: INT 5Ch returns at once with AL = the
 * IMMEDIATE code (0 = accepted), and the final answer arrives in retcode/cmd_cplt
 * (FFh while pending), with the POST routine called if one is given.
 *
 * [CAUTION]: THIS DEVICE COMPLETES A NO-WAIT COMMAND BEFORE RETURNING. The final answer is in
 * the NCB by the time the program looks, which every polling program accepts. A POST
 * routine is recorded (IsPostPending) and the HOST runs it as INT 5Ch returns: the
 * stub's IRET goes to the POST routine with ES:BX = the NCB, and its IRET to the
 * caller -- as if the command had completed the instant it was issued.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_VDD_NET_H
#define NTVDMEX_VDD_NET_H
#include "vdd_bus.h"

#define NETB_NCB_SIZE               64
#define NETB_RC_INVALID_COMMAND     0x03    /* Retcode: invalid command */
#define NETB_RC_INVALID_ADAPTER     0x23    /* Retcode: invalid adapter (lana) number */
#define NETB_RC_INVALID_BUFFER      0x01    /* Retcode: illegal buffer length / address */
#define NETB_RC_PENDING             0xFF
#define NETB_NAME_SIZE              16
#define NETB_DEVICE_NAME            "netbios"

typedef struct _NETBIOS_REQUEST
{
    BYTE   Command;                /* wait form: bit 7 stripped */
    BYTE   ReturnCode, LocalSession, NameNumber;
    BYTE  *Buffer;                 /* host address of the guest buffer, or NULL */
    WORD   Length;
    BYTE   CallName[NETB_NAME_SIZE], Name[NETB_NAME_SIZE];
    BYTE   ReceiveTimeout, SendTimeout, Adapter;
} NETBIOS_REQUEST, *PNETBIOS_REQUEST;

/* The host's NetBIOS. Synchronous; returns the final retcode (also in request->ReturnCode). */
typedef BYTE (*PNETBIOS_SUBMIT_ROUTINE)(PVOID context, PNETBIOS_REQUEST request);

typedef struct _NETBIOS_STATE
{
    PVDD_BUS       Bus;
    PNETBIOS_SUBMIT_ROUTINE Submit;   PVOID SubmitContext;
    UINT32 Calls, NoWaitCalls, PostsOwed, NoBackendCalls;
    /* s91: a no-wait command's POST routine, owed to the guest as soon as INT 5Ch
     * returns. The HOST delivers it (it alone can edit the guest's return frame --
     * see v86_bios_bop) and clears IsPostPending; PostsOwed counts the ones that
     * could not be delivered.
     */
    INT    IsPostPending;
    WORD   PostSegment, PostOffset;
    BYTE   LastCommand, LastReturnCode;
} NETBIOS_STATE, *PNETBIOS_STATE;

INT  VddNetBiosInitialize(_In_ PVDD_BUS bus, _In_ PVOID context);
VOID VddNetBiosReset(_In_ PVOID context);
VOID VddNetBiosSetBackend(_Inout_ PNETBIOS_STATE state, _In_opt_ PNETBIOS_SUBMIT_ROUTINE submitRoutine, _In_opt_ PVOID context);

/* The whole INT 5Ch service on an NCB already mapped to host memory: exposed for the
 * off-VM test. `buffer` = the NCB's buffer resolved by the caller (NULL if none).
 * Returns AL.
 */
BYTE VddNetBiosService(_Inout_ PNETBIOS_STATE state, _Inout_updates_(NETB_NCB_SIZE) BYTE *ncb, _In_opt_ BYTE *buffer);

static inline NTVDD_DEVICE VddNetBiosDevice(_In_ PNETBIOS_STATE state)
{ NTVDD_DEVICE device; device.Name = NETB_DEVICE_NAME; device.Initialize = VddNetBiosInitialize; device.Reset = VddNetBiosReset;
  device.Shutdown = 0; device.Context = state; return device; }

#endif /* NTVDMEX_VDD_NET_H */
