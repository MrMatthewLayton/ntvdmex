/*
 * vdd_cmos.h -- the MC146818 real-time clock and CMOS RAM (ports 0x70/0x71).
 *
 * ── WHY THIS EXISTS, AND WHY IT IS NOT OPTIONAL. ────────────────────────────────
 * Nothing claimed 0x70/0x71, so the chip did not exist -- while the BIOS SERVICE
 * built on top of it did: INT 1Ah AH=02h/04h is answered out of the host's clock
 * over in the PIT VDD. Firmware present, hardware absent; the same split the 8042
 * turned out to have.
 *
 * ⛔⛔ AND THE FAILURE SHAPE WAS A HANG, NOT A WRONG ANSWER. An unclaimed ISA port
 *   reads 0xFF on this host -- deliberately, so that device detection cannot
 *   mistake an absent card for a present one (see the note in the V86 I/O trap).
 *   Status Register A is 0x0A, and ITS BIT 7 IS **UIP**, "update in progress".
 *   The canonical way to read this chip, in every BIOS and every program that does
 *   it by hand, is:
 *
 *       poll 0Ah until UIP is CLEAR, then read the time registers
 *
 *   With 0xFF coming back, UIP is set for ever and that loop never exits. Not a
 *   plausible wrong time -- a guest that stops.
 *
 * ★ MEASURED (p_rtc.asm, 2026-09-23) before any of this was written: our
 *   rtc.agree.hours = 0000 against 0101 on ALL THREE oracles -- the BIOS door and
 *   the chip door disagreed about the time, because one of them was 0xFF.
 *
 * ── ONE CLOCK, TWO DOORS. ───────────────────────────────────────────────────────
 * INT 1Ah AH=02h/04h and registers 00h-09h are two interfaces onto the SAME clock
 * and must agree, for exactly the reason the 8042's A20 gate and XMS had to: a
 * guest may use either. Both take the host's `rtc_now` hook, so agreement is
 * structural rather than something to keep in step by hand.
 *
 * Pure C, no <windows.h>: the clock is injected, so the whole chip is exercised
 * off-VM by tools/dostest/cmos_test.c.
 */
#ifndef NTVDMEX_VDD_CMOS_H
#define NTVDMEX_VDD_CMOS_H

#include "vdd_bus.h"
#include "vdd_pit.h"        /* struct vdd_rtc -- the same reading INT 1Ah uses */

/* CMOS register numbers worth naming (docs/ref/rtc.md 2 and 3). */
#define CMOS_SEC        0x00
#define CMOS_MIN        0x02
#define CMOS_HOUR       0x04
#define CMOS_DOW        0x06
#define CMOS_DOM        0x07
#define CMOS_MONTH      0x08
#define CMOS_YEAR       0x09
#define CMOS_STATUS_A   0x0A
#define CMOS_STATUS_B   0x0B
#define CMOS_STATUS_C   0x0C
#define CMOS_STATUS_D   0x0D
#define CMOS_EQUIP      0x14
#define CMOS_CENTURY    0x32

typedef struct cmos_state {
    vdd_bus *bus;
    uint8_t  index;         /* the low 7 bits of the last write to 0x70        */
    /* ⛔ BIT 7 OF PORT 0x70 IS THE NMI MASK, not part of the register number.
         Software sets it constantly -- masking NMI across a CMOS access is
         standard BIOS practice -- so a model that takes the whole byte as an
         index reads register 0x8A and finds nothing. Kept separately, and
         reported rather than acted on: we have no NMI to mask. */
    uint8_t  nmi_disabled;
    uint32_t nmi_mask_writes;
    uint8_t  ram[128];      /* the CMOS proper; 0x00-0x0D are overlaid by the clock */
    uint8_t  status_c;      /* the interrupt flags -- CLEARED BY READING (0x0C)  */
    /* The clock. NULL means the registers read as whatever `ram` holds, which is
       what the off-VM battery uses to pin exact values. */
    void   (*rtc_now)(void *ctx, struct vdd_rtc *out);
    void    *rtc_ctx;
} cmos_state;

int  vdd_cmos_init(vdd_bus *b, void *self);
void vdd_cmos_reset(void *self);
static inline ntvdd vdd_cmos_device(cmos_state *st)
{ ntvdd d; d.name = "cmos"; d.init = vdd_cmos_init; d.reset = vdd_cmos_reset;
  d.shutdown = 0; d.self = st; return d; }

#endif /* NTVDMEX_VDD_CMOS_H */
