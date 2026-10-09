/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * A complete third-party NTVDMEX device, in one file. (GH #11)
 *
 * It is a deliberately BORING device, because the point of a sample is the
 * mechanism and not the hardware: eight ports at 0x2E0 that behave like a real
 * little ISA card you could write a DOS driver for.
 *
 * 0x2E0  R  identification, 0x4E  ('N')      -- how a driver detects us
 * 0x2E0  W  the data latch
 * 0x2E1  R  the data latch, ONE'S COMPLEMENT -- proves the write arrived and
 *           was transformed, which reading it back unchanged would not
 * 0x2E2  R  how many bytes have been written, low byte  (wraps at 256)
 * 0x2E3  R  the ABI version the host handed us
 *
 * Build it with sdk/build-sample.sh; point the host at the DLL with vdd.txt (see
 * docs/sdk/vdd-sdk.md). tests/probes/dos/vddtest.asm is a DOS program that drives
 * exactly these ports and prints what it got.
 *
 * [CAUTION]: NOTE WHAT IS NOT HERE: no <windows.h>, no imports from the host, no global
 * register macros. The state is a struct handed back to us as `self`, which is
 * what lets the same file be unit-tested with a stub api table.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include "ntvdmex-vdd.h"

#define PORTECHO_BASE                   0x2E0
#define PORTECHO_ID                     0x4E    /* 'N' */
#define PORTECHO_PORTS                  8
#define PORTECHO_ID_REGISTER            0       /* R: PORTECHO_ID; W: the data latch */
#define PORTECHO_COMPLEMENT_REGISTER    1
#define PORTECHO_COUNT_REGISTER         2
#define PORTECHO_VERSION_REGISTER       3
#define PORTECHO_LOW_BYTE               0xFF
#define PORTECHO_EMPTY_SLOT             0xFF    /* What an empty ISA slot reads */

typedef struct _PORTECHO_STATE
{
    const ntvdmex_vdd_api *Api;
    uint8_t  Latch;
    uint32_t Writes;
    uint32_t Version;
} PORTECHO_STATE, *PPORTECHO_STATE;

static PORTECHO_STATE g_PortEcho;

static void PortEchoIn(void *self, uint16_t port, uint8_t width, uint32_t *value)
{
    PPORTECHO_STATE state = (PPORTECHO_STATE)self;

    (void)width;
    switch (port - PORTECHO_BASE)
    {
    case PORTECHO_ID_REGISTER:
        *value = PORTECHO_ID;
    break;

    case PORTECHO_COMPLEMENT_REGISTER:
        *value = (uint8_t)~state->Latch;
    break;

    case PORTECHO_COUNT_REGISTER:
        *value = (uint8_t)(state->Writes & PORTECHO_LOW_BYTE);
    break;

    case PORTECHO_VERSION_REGISTER:
        *value = (uint8_t)state->Version;
    break;

    /* [CAUTION]: AN UNCLAIMED REGISTER READS 0xFF, WHICH IS WHAT AN EMPTY ISA SLOT DOES.
     * Answering 0 instead would make a detection routine that probes for
     * "anything at all" think the card is present and broken.
     */
    default:
        *value = PORTECHO_EMPTY_SLOT;
    break;
    }
}

static void PortEchoOut(void *self, uint16_t port, uint8_t width, uint32_t value)
{
    PPORTECHO_STATE state = (PPORTECHO_STATE)self;

    (void)width;
    if ((port - PORTECHO_BASE) == PORTECHO_ID_REGISTER)
    {
        state->Latch = (uint8_t)value;
        ++state->Writes;
    }
}

NTVDMEX_VDD_EXPORT int NtvdmexVddInit(const ntvdmex_vdd_api *api, ntvdmex_vdd_bus *bus)
{
    int status;

    /* [CAUTION]: VERSION FIRST, AND REFUSE RATHER THAN HOPE. A driver that runs against
     * an ABI it does not understand is how a plugin model earns its
     * reputation; the host reports the refusal and carries on without us.
     */
    if (!api || api->version != NTVDMEX_VDD_ABI_VERSION)
        return -1;
    if (api->size < sizeof *api)
        return -1;

    g_PortEcho.Api     = api;
    g_PortEcho.Latch   = 0;
    g_PortEcho.Writes  = 0;
    g_PortEcho.Version = api->version;

    status = api->claim_ports(bus, PORTECHO_BASE, PORTECHO_BASE + PORTECHO_PORTS - 1,
                              PortEchoIn, PortEchoOut, &g_PortEcho);
    /* READ THE STATUS. See the note on claim_* in the header: a refused claim is
     * a device the guest can never reach, and saying so here is the difference
     * between a five-minute fix and an afternoon.
     */
    if (status != 0)
    {
        api->log("portecho: claim_ports REFUSED -- the host's port table is full;"
                 " this device is NOT on the bus");
        return -1;
    }
    api->log("portecho: 0x2E0..0x2E7 claimed (id=0x4E)");
    return 0;
}
