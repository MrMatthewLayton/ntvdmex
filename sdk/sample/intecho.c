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
 * Load: put this DLL's full path on a line in cfg\vdd.txt. Test: tests/probes/dos/p_sdkint.
 */
#include "ntvdmex-vdd.h"

#define INTECHO_VECTOR         0x61
#define INTECHO_FUNCTION_SHIFT 8          /* AH: the function number        */
#define INTECHO_BYTE_MASK      0xFF
#define INTECHO_PRESENCE       0x00       /* AH=00h                         */
#define INTECHO_SUM            0x01       /* AH=01h                         */
#define INTECHO_SIGNATURE      0x4E58u    /* 'NX'                           */
#define INTECHO_HIGH_WORD      0xFFFF0000u
#define INTECHO_LOW_WORD       0xFFFFu
#define INTECHO_COUNT_MASK     0xFFFF     /* CX                             */

static const ntvdmex_vdd_api *g_IntEchoApi;
static ntvdmex_vdd_bus       *g_IntEchoBus;

static void IntEchoInterrupt(void *self, ntvdmex_regs *registers)
{
    unsigned function = (registers->eax >> INTECHO_FUNCTION_SHIFT) & INTECHO_BYTE_MASK;
    (void)self;
    if (function == INTECHO_PRESENCE) {
        registers->eax = (registers->eax & INTECHO_HIGH_WORD) | INTECHO_SIGNATURE;
        registers->ebx = (registers->ebx & INTECHO_HIGH_WORD) | (g_IntEchoApi->version & INTECHO_LOW_WORD);
        registers->cf = 0;
    } else if (function == INTECHO_SUM) {
        const uint8_t *bytes = (const uint8_t *)g_IntEchoApi->map_flat(g_IntEchoBus, registers->ds, (uint16_t)registers->esi);
        uint32_t count = registers->ecx & INTECHO_COUNT_MASK, index;
        uint16_t sum = 0;
        if (!bytes) { registers->cf = 1; return; }
        for (index = 0; index < count; ++index) sum = (uint16_t)(sum + bytes[index]);
        registers->ecx = (registers->ecx & INTECHO_HIGH_WORD) | sum;
        registers->cf = 0;
    } else {
        registers->cf = 1;
    }
}

NTVDMEX_VDD_EXPORT int NtvdmexVddInit(const ntvdmex_vdd_api *api, ntvdmex_vdd_bus *bus)
{
    if (!api || api->version != NTVDMEX_VDD_ABI_VERSION || api->size < sizeof *api) return -1;
    g_IntEchoApi = api; g_IntEchoBus = bus;
    if (api->claim_int(bus, INTECHO_VECTOR, IntEchoInterrupt, 0) != 0) {
        api->log("intecho: claim_int(61h) REFUSED -- already claimed");
        return -1;
    }
    api->log("intecho: INT 61h claimed");
    return 0;
}
