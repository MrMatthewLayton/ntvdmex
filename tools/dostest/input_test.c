/* input_test.c -- off-VM unit battery for the keyboard input VDD (vdd_input.c).
 *
 * M3 slice-6: exercise the key ring buffer + INT 16h servicer (ZF "key ready"
 * semantics) natively, no VM. The host owns blocking (INT 16h AH=00 waits on a
 * key event); here we test the pure non-blocking core.
 */
#include <stdio.h>
#include <string.h>
#include "vdd_input.h"

static int total = 0, fails = 0;
#define CHECK(c,m) do{ total++; if(c){printf("  PASS  %s\n",(m));} \
    else{printf("  FAIL  %s\n",(m)); fails++;} }while(0)

int main(void)
{
    vdd_bus bus;
    input_state in; memset(&in, 0, sizeof in);
    ntvdd dev = vdd_input_device(&in);
    ntvdd_regs r; uint16_t k;

    printf("== M3 slice-6 keyboard input battery ==\n");

    vdd_bus_init(&bus, 0);
    CHECK(vdd_bus_add(&bus, &dev) == 0, "add: input init ok");
    CHECK(bus.ints[0x16].svc != 0, "add: INT 16h claimed");

    /* T1: empty ring -> pop/peek report nothing -------------------------- */
    CHECK(vdd_input_pop(&in, &k) == 0, "ring: empty pop -> 0");
    CHECK(vdd_input_peek(&in, &k) == 0, "ring: empty peek -> 0");

    /* T2: push/peek/pop FIFO ordering ------------------------------------ */
    vdd_input_push(&in, 0x1C0D); vdd_input_push(&in, 0x3920); /* Enter, Space */
    CHECK(vdd_input_peek(&in, &k) == 1 && k == 0x1C0D, "ring: peek = first pushed");
    CHECK(vdd_input_pop(&in, &k) == 1 && k == 0x1C0D, "ring: pop #1 FIFO");
    CHECK(vdd_input_pop(&in, &k) == 1 && k == 0x3920, "ring: pop #2 FIFO");
    CHECK(vdd_input_pop(&in, &k) == 0, "ring: drained");

    /* T3: INT 16h AH=01 (check) -> ZF=1 empty, ZF=0 + AX when ready ------- */
    memset(&r, 0, sizeof r); s_ah(&r, 0x01);
    vdd_bus_deliver_int(&bus, 0x16, &r);
    CHECK(r.zf == 1, "int16/01: ZF=1 when no key");
    vdd_input_push(&in, 0x1E61);                     /* 'a' */
    memset(&r, 0, sizeof r); s_ah(&r, 0x01);
    vdd_bus_deliver_int(&bus, 0x16, &r);
    CHECK(r.zf == 0 && r_ax(&r) == 0x1E61, "int16/01: ZF=0 + AX=key when ready");
    /* AH=01 is a peek -> the key is still there */
    CHECK(vdd_input_peek(&in, &k) == 1 && k == 0x1E61, "int16/01: peek did not consume");

    /* T4: INT 16h AH=00 (read) consumes; ZF=1 when empty ----------------- */
    memset(&r, 0, sizeof r); s_ah(&r, 0x00);
    vdd_bus_deliver_int(&bus, 0x16, &r);
    CHECK(r.zf == 0 && r_ax(&r) == 0x1E61, "int16/00: returns the key");
    CHECK(vdd_input_pop(&in, &k) == 0, "int16/00: consumed the key");
    memset(&r, 0, sizeof r); s_ah(&r, 0x00);
    vdd_bus_deliver_int(&bus, 0x16, &r);
    CHECK(r.zf == 1, "int16/00: ZF=1 when empty (host then blocks)");

    /* T5: ring wraps + drops oldest when full --------------------------- */
    { int i; memset(&in, 0, sizeof in); in.bus = &bus;
      for (i = 0; i < VDD_KBD_SIZE + 5; ++i) vdd_input_push(&in, (uint16_t)(0x100 + i));
      /* capacity is VDD_KBD_SIZE-1 usable; oldest entries were dropped */
      CHECK(vdd_input_pop(&in, &k) == 1 && k > 0x100, "ring: full -> oldest dropped, newest kept");
    }

    /* T6: enhanced fns AH=10/11 mirror 00/01; unknown fn never phantom-keys *
     * (regression: QuickBasic INKEY$ uses AH=11h; a default ZF=0 made it     *
     * read a phantom key and exit -- BLIT.EXE drew nothing.)                 */
    memset(&in, 0, sizeof in); in.bus = &bus;
    memset(&r, 0, sizeof r); s_ah(&r, 0x11);            /* enhanced check, empty */
    vdd_bus_deliver_int(&bus, 0x16, &r);
    CHECK(r.zf == 1, "int16/11: ZF=1 when no key (INKEY$ -> \"\")");
    vdd_input_push(&in, 0x1C0D);                        /* Enter */
    memset(&r, 0, sizeof r); s_ah(&r, 0x11);
    vdd_bus_deliver_int(&bus, 0x16, &r);
    CHECK(r.zf == 0 && r_ax(&r) == 0x1C0D, "int16/11: ZF=0 + AX=key when ready");
    memset(&r, 0, sizeof r); s_ah(&r, 0x10);            /* enhanced read consumes */
    vdd_bus_deliver_int(&bus, 0x16, &r);
    CHECK(r.zf == 0 && r_ax(&r) == 0x1C0D, "int16/10: enhanced read returns key");
    CHECK(vdd_input_pop(&in, &k) == 0, "int16/10: consumed the key");
    memset(&r, 0xFF, sizeof r); s_ah(&r, 0x55);         /* unknown fn */
    vdd_bus_deliver_int(&bus, 0x16, &r);
    CHECK(r.zf == 1, "int16/unknown: ZF=1, never a phantom key");

    printf("\n%d checks, %d failed\n", total, fails);
    return fails ? 1 : 0;
}
