/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The exec loop: NTVDM's own commands and BOP service, the V86 guest's run, its I/O events and interrupts, and HostRunExecLoop.
 *
 * Declarations only (#335): defined in host_exec.c.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_HOST_EXEC_H
#define NTVDMEX_HOST_EXEC_H

#include "host_state.h"

VOID HostRunExecLoop(
    PSTR *cursorIo,
    PSTR const base,
    DOS_MACHINE *machine,
    volatile BYTE * const tib,
    LONG *vdmStatusIo,
    CHAR *programPathBuffer);

#endif
