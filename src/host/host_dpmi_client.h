/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * A DPMI client session: starting it, running it in slices, reflecting its faults to its handlers, delivering its interrupts, ending it.
 *
 * Declarations only (#335): defined in host_dpmi_client.c.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_HOST_DPMI_CLIENT_H
#define NTVDMEX_HOST_DPMI_CLIENT_H

#include "host_state.h"

INT DpmiStartClientSession(
    PSTR *cursorIo,
    PSTR const base,
    volatile BYTE * const tib,
    DOS_MACHINE *machine);

#endif
