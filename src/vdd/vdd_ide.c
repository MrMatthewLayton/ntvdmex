/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * See vdd_ide.h.  An IDE/ATA adapter with both channels empty.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include "vdd_ide.h"

#define IDE_EMPTY_CHANNEL   0   /* What every register of an empty channel reads */
#define IDE_OK              0
#define IDE_FAILED          (-1)

VOID VddIdePortIn(PVOID context, WORD port, BYTE width, UINT32 *value)
{
    PIDE_STATE state = (PIDE_STATE)context;

    (VOID)port;
    (VOID)width;
    state->PortReads++;
    /* Nothing drives the data lines: DD7 is pulled down (ATA-3 Table 2 note 3) and
     * DD6:0 are answered 0 by choice -- vdd_ide.h. BSY therefore reads CLEAR on
     * every register of both channels, at every width, which is the single fact a
     * polling loop needs in order to stop.
     */
    *value = IDE_EMPTY_CHANNEL;
}

VOID VddIdePortOut(PVOID context, WORD port, BYTE width, UINT32 value)
{
    PIDE_STATE state = (PIDE_STATE)context;

    (VOID)width;
    state->PortWrites++;
    /* No drive latches anything -- including SRST/nIEN at 3F6h/376h, which are
     * DEVICE bits. A command written to an empty channel is received by nobody:
     * no BSY, no INTRQ, no DRQ, ever. Counted so a trace can say what was asked.
     */
    if (port == IDE_PRIMARY_COMMAND + IDE_COMMAND_REGISTER || port == IDE_SECONDARY_COMMAND + IDE_COMMAND_REGISTER)
    {
        state->Commands++;
        state->LastCommand = (BYTE)value;
    }
}

VOID VddIdeReset(PVOID context)
{
    PIDE_STATE state = (PIDE_STATE)context;

    state->PortReads = state->PortWrites = state->Commands = 0;
    state->LastCommand = 0;
}

INT VddIdeInitialize(PVDD_BUS bus, PVOID context)
{
    PIDE_STATE state = (PIDE_STATE)context;

    state->Bus = bus;
    VddIdeReset(state);
    /* [WARNING]: 3F7h IS NOT CLAIMED HERE -- the FDC owns it (DIR bit 7). See vdd_ide.h. */
    if (VddClaimPorts(bus, IDE_PRIMARY_COMMAND, IDE_PRIMARY_COMMAND + IDE_COMMAND_REGISTER, VddIdePortIn, VddIdePortOut, state))
        return IDE_FAILED;

    if (VddClaimPorts(bus, IDE_PRIMARY_CONTROL, IDE_PRIMARY_CONTROL,     VddIdePortIn, VddIdePortOut, state))
        return IDE_FAILED;

    if (VddClaimPorts(bus, IDE_SECONDARY_COMMAND, IDE_SECONDARY_COMMAND + IDE_COMMAND_REGISTER, VddIdePortIn, VddIdePortOut, state))
        return IDE_FAILED;

    if (VddClaimPorts(bus, IDE_SECONDARY_CONTROL, IDE_SECONDARY_CONTROL + IDE_DRIVE_ADDRESS, VddIdePortIn, VddIdePortOut, state))
        return IDE_FAILED;

    return IDE_OK;
}
