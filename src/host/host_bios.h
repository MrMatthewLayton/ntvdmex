/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The BIOS and the devices it fronts: serial and parallel ports, keyboard actions,
 *   INT 15h, print screen, and the BIOS BOP dispatcher.
 *
 * Declarations only (#335): defined in host_bios.c.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_HOST_BIOS_H
#define NTVDMEX_HOST_BIOS_H

#include "host_state.h"

extern volatile DWORD g_Int15EventLinear;
extern NETBIOS_STATE g_Net;
extern NTVDD_DEVICE g_NetDevice;
extern BYTE g_GenericStubVector[DOS_GENSTUB_N];
extern HANDLE g_ComSpool[COMM_MAX_PORTS];
extern INT g_ComFailed[COMM_MAX_PORTS];
extern INT g_BdaReady;
extern volatile LONGLONG g_Int15WaitEnd;
extern WORD g_DosMemoryTop;

VOID SerialOut(PCSTR buffer, PCSTR end);
VOID ComTransmitSink(PVOID context, INT port, BYTE byteValue);
WORD BiosEquipmentWord(VOID);
VOID BiosBdaRefreshEquipment(VOID);
VOID SerialInitialize(VOID);
INT LptSpoolPut(BYTE character);
VOID LptTransmitSink(PVOID context, INT port, BYTE byteValue);
INT KeyboardActionEntry(INT keyboardAction);
INT Int15Hooked(VOID);
UINT Int15MoveBlockAt(volatile BYTE *tib, DWORD gdtLinear);
VOID RegistersLoad(NTVDD_REGISTERS *registers, volatile BYTE *tib);
VOID RegistersStore(NTVDD_REGISTERS *registers, volatile BYTE *tib);
INT V86BiosBop(volatile BYTE *tib, UINT bopNumber, PSTR *logCursor, PSTR base);

#endif
