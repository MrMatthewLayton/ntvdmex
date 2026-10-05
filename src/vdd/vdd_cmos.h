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
 * off-VM by tests/unit/cmos_test.c.
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
/* KB above 1 MB: the same 15 MB INT 15h AH=88h reports. #48: THE one number for this
   machine's extended memory -- AH=88h (both modes), SysVars+0x45 and the XMS pool
   (this less the HMA) are all derived from it in main.c. */
#define CMOS_EXT_KB     0x3C00u
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
    uint8_t  status_a;      /* divider + periodic rate select                    */
    uint8_t  status_b;      /* SET/PIE/AIE/UIE/SQWE/DM/24-12/DSE                 */
    uint8_t  status_c;      /* the interrupt flags -- CLEARED BY READING (0x0C)  */
    /* ── THE PERIODIC INTERRUPT. (docs/ref/rtc.md 4) ─────────────────────────
         IRQ8 at the rate in Status A bits 3:0 -- a fast, steady tick that is
         INDEPENDENT OF THE 8254, which is exactly why Windows and DOS extenders
         use it: a guest that has reprogrammed the PIT has not touched this one.
       ⚠ DORMANT BY DEFAULT, AND THAT IS WHAT MAKES IT SAFE TO ADD. Nothing is
         raised unless the guest has BOTH set PIE in Status B and a non-zero rate
         select, AND unmasked IRQ8 on the slave PIC and IRQ2 on the master. All
         four are off at reset, so a guest that does not ask sees no change.
       ► Driven from the host's existing PIT pacer, in the same lock and off the
         same QueryPerformanceCounter delta -- no second thread, and no second
         opinion about how much time has passed. */
    uint64_t pf_accum;      /* PIT-rate clocks not yet turned into periodic ticks */
    uint32_t pf_raised;     /* periodic interrupts raised (a run should say)      */
    /* The once-a-second edge, for the UPDATE-ENDED and ALARM flags. Accumulated
       from the same clocks rather than polled off the host clock, so the pacer
       calls rtc_now once a SECOND instead of once a tick. */
    uint64_t sec_accum;
    uint32_t uf_raised, af_raised;
    /* The clock. NULL means the registers read as whatever `ram` holds, which is
       what the off-VM battery uses to pin exact values. */
    void   (*rtc_now)(void *ctx, struct vdd_rtc *out);
    void    *rtc_ctx;
    /* ── GH #261: AND THE OTHER DIRECTION -- a guest writing the clock registers.
         The same hook INT 1Ah AH=03h/05h uses (vdd_pit.h): what=0 hour/min/sec,
         what=1 cent/year/month/day, binary; it moves the VDM's RTC offset, never
         the machine's clock. NULL = refused, as before -- which is what the off-VM
         battery's refusal checks still pin. Shares rtc_ctx. */
    int    (*rtc_set)(void *ctx, const struct vdd_rtc *in, int what);
    /* Status B's SET bit (7): while it is held the chip does not update, and a
       program writes the time a field at a time without the clock carrying in
       between. Modelled as a FROZEN COPY taken when SET goes high: reads come from
       it, writes go into it, and SET going low commits it in one step. */
    uint8_t  set_held;
    struct vdd_rtc shadow;
    /* #136: base memory FITTED, in KB, for 15h/16h -- Settings > Conventional Memory.
         0 = 640, the machine every build so far has described. Set by the host BEFORE
         init and preserved across reset like the hooks above: it is how the board is
         populated, not something POST decides. */
    uint16_t base_kb;
} cmos_state;

/* Advance the periodic divider by `clocks` PIT-input-rate clocks and raise IRQ8
   as often as the programmed rate says. A no-op unless the guest enabled it. */
void vdd_cmos_add_clocks(cmos_state *st, uint32_t clocks);

/* The programmed periodic rate in Hz, or 0 if none. docs/ref/rtc.md 2. */
uint32_t vdd_cmos_periodic_hz(const cmos_state *st);

int  vdd_cmos_init(vdd_bus *b, void *self);
void vdd_cmos_reset(void *self);
static inline ntvdd vdd_cmos_device(cmos_state *st)
{ ntvdd d; d.name = "cmos"; d.init = vdd_cmos_init; d.reset = vdd_cmos_reset;
  d.shutdown = 0; d.self = st; return d; }

#endif /* NTVDMEX_VDD_CMOS_H */
