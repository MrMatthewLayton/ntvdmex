/*
 * portecho.c -- a complete third-party NTVDMEX device, in one file. (GH #11)
 *
 * It is a deliberately BORING device, because the point of a sample is the
 * mechanism and not the hardware: eight ports at 0x2E0 that behave like a real
 * little ISA card you could write a DOS driver for.
 *
 *   0x2E0  R  identification, 0x4E  ('N')      -- how a driver detects us
 *   0x2E0  W  the data latch
 *   0x2E1  R  the data latch, ONE'S COMPLEMENT -- proves the write arrived and
 *             was transformed, which reading it back unchanged would not
 *   0x2E2  R  how many bytes have been written, low byte  (wraps at 256)
 *   0x2E3  R  the ABI version the host handed us
 *
 * Build it with sdk/build-sample.sh; point the host at the DLL with vdd.txt (see
 * docs/sdk/vdd-sdk.md). tools/dostest/vddtest.asm is a DOS program that drives
 * exactly these ports and prints what it got.
 *
 * ⚠ NOTE WHAT IS NOT HERE: no <windows.h>, no imports from the host, no global
 *   register macros. The state is a struct handed back to us as `self`, which is
 *   what lets the same file be unit-tested with a stub api table.
 */
#include "ntvdmex-vdd.h"

#define PORTECHO_BASE 0x2E0
#define PORTECHO_ID   0x4E              /* 'N' */

typedef struct {
    const ntvdmex_vdd_api *api;
    uint8_t  latch;
    uint32_t writes;
    uint32_t version;
} portecho_state;

static portecho_state g_pe;

static void pe_in(void *self, uint16_t port, uint8_t width, uint32_t *val)
{
    portecho_state *st = (portecho_state *)self;
    (void)width;
    switch (port - PORTECHO_BASE) {
    case 0: *val = PORTECHO_ID;                     break;
    case 1: *val = (uint8_t)~st->latch;             break;
    case 2: *val = (uint8_t)(st->writes & 0xFF);    break;
    case 3: *val = (uint8_t)st->version;            break;
    /* ⚠ AN UNCLAIMED REGISTER READS 0xFF, WHICH IS WHAT AN EMPTY ISA SLOT DOES.
         Answering 0 instead would make a detection routine that probes for
         "anything at all" think the card is present and broken. */
    default: *val = 0xFF;                           break;
    }
}

static void pe_out(void *self, uint16_t port, uint8_t width, uint32_t val)
{
    portecho_state *st = (portecho_state *)self;
    (void)width;
    if ((port - PORTECHO_BASE) == 0) { st->latch = (uint8_t)val; ++st->writes; }
}

NTVDMEX_VDD_EXPORT int NtvdmexVddInit(const ntvdmex_vdd_api *api,
                                      ntvdmex_vdd_bus *bus)
{
    int rc;
    /* ⚠ VERSION FIRST, AND REFUSE RATHER THAN HOPE. A driver that runs against
         an ABI it does not understand is how a plugin model earns its
         reputation; the host reports the refusal and carries on without us. */
    if (!api || api->version != NTVDMEX_VDD_ABI_VERSION) return -1;
    if (api->size < sizeof *api) return -1;

    g_pe.api     = api;
    g_pe.latch   = 0;
    g_pe.writes  = 0;
    g_pe.version = api->version;

    rc = api->claim_ports(bus, PORTECHO_BASE, PORTECHO_BASE + 7,
                          pe_in, pe_out, &g_pe);
    /* READ THE STATUS. See the note on claim_* in the header: a refused claim is
       a device the guest can never reach, and saying so here is the difference
       between a five-minute fix and an afternoon. */
    if (rc != 0) {
        api->log("portecho: claim_ports REFUSED -- the host's port table is full;"
                 " this device is NOT on the bus");
        return -1;
    }
    api->log("portecho: 0x2E0..0x2E7 claimed (id=0x4E)");
    return 0;
}
