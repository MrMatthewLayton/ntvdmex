/*
 * intecho.c -- the SDK's second sample: a software-interrupt service.  (#11, #315, s91)
 *
 * portecho.c shows ports. This shows the other two things a driver most often wants:
 * claim_int (an INT the guest calls) and map_flat (reaching the guest's memory).
 *
 *   INT 61h AH=00h  presence: AX = 'NX' (4E58h), BX = the host's ABI version, CF clear
 *   INT 61h AH=01h  CX = the sum of the CX bytes at DS:SI, CF clear (CF set if DS:SI
 *                   does not resolve)
 *   anything else   CF set, registers untouched
 *
 * INT 61h is a USER vector (60h-66h are the ones the PC reserves for programs), which
 * is what the host's generic stubs accept; a claim on a DOS, BIOS or IRQ vector is
 * refused by the host and said so in its log.
 * Load: put this DLL's full path on a line in cfg\vdd.txt. Test: tools/dostest/p_sdkint.
 */
#include "ntvdmex-vdd.h"

#define INTECHO_VEC 0x61

static const ntvdmex_vdd_api *g_api;
static ntvdmex_vdd_bus       *g_bus;

static void ie_int(void *self, ntvdmex_regs *r)
{
    unsigned ah = (r->eax >> 8) & 0xFF;
    (void)self;
    if (ah == 0x00) {
        r->eax = (r->eax & 0xFFFF0000u) | 0x4E58u;
        r->ebx = (r->ebx & 0xFFFF0000u) | (g_api->version & 0xFFFFu);
        r->cf = 0;
    } else if (ah == 0x01) {
        const uint8_t *p = (const uint8_t *)g_api->map_flat(g_bus, r->ds, (uint16_t)r->esi);
        uint32_t n = r->ecx & 0xFFFF, i;
        uint16_t sum = 0;
        if (!p) { r->cf = 1; return; }
        for (i = 0; i < n; ++i) sum = (uint16_t)(sum + p[i]);
        r->ecx = (r->ecx & 0xFFFF0000u) | sum;
        r->cf = 0;
    } else {
        r->cf = 1;
    }
}

NTVDMEX_VDD_EXPORT int NtvdmexVddInit(const ntvdmex_vdd_api *api, ntvdmex_vdd_bus *bus)
{
    if (!api || api->version != NTVDMEX_VDD_ABI_VERSION || api->size < sizeof *api) return -1;
    g_api = api; g_bus = bus;
    if (api->claim_int(bus, INTECHO_VEC, ie_int, 0) != 0) {
        api->log("intecho: claim_int(61h) REFUSED -- already claimed");
        return -1;
    }
    api->log("intecho: INT 61h claimed");
    return 0;
}
