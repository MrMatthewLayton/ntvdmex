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

/* Stand-in for guest segment 0x40. The ring lives in the GUEST's BIOS data area now, so a
   test that leaves this NULL is testing nothing at all -- every push would be discarded. */
static uint8_t bda[0x100];

static void fresh(input_state *in, vdd_bus *bus)
{
    memset(in, 0, sizeof *in);
    memset(bda, 0, sizeof bda);
    in->bus = bus;
    in->bda = bda;
    vdd_input_reset(in);
}

int main(void)
{
    vdd_bus bus;
    input_state in; memset(&in, 0, sizeof in);
    ntvdd dev = vdd_input_device(&in);
    ntvdd_regs r; uint16_t k;

    printf("== M3 slice-6 keyboard input battery ==\n");

    in.bda = bda;                    /* before add(): init resets the ring pointers */
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

    /* T5: ring wraps; a FULL buffer discards the NEWEST key, as the BIOS does ---
     * (It used to drop the oldest. In a ring of whole keystrokes that merely loses the
     * wrong key; in the scancode FIFO the same rule deleted E0 prefixes and stranded
     * break codes, which is what "arrows dead, space stuck" was made of.) */
    { int i; fresh(&in, &bus);
      for (i = 0; i < 20; ++i) vdd_input_push(&in, (uint16_t)(0x100 + i));  /* 15 fit */
      CHECK(vdd_input_pop(&in, &k) == 1 && k == 0x100, "ring: full -> oldest KEPT");
      { int n = 1; while (vdd_input_pop(&in, &k)) n++;
        CHECK(n == 15, "ring: holds 15 keys (16 slots, one always empty)"); }
    }

    /* T6: the guest's BDA is where the keys actually are ------------------ */
    { uint16_t head, tail;
      fresh(&in, &bus);
      head = (uint16_t)(bda[BDA_KB_HEAD] | (bda[BDA_KB_HEAD+1] << 8));
      tail = (uint16_t)(bda[BDA_KB_TAIL] | (bda[BDA_KB_TAIL+1] << 8));
      CHECK(head == BDA_KB_START && tail == BDA_KB_START, "bda: reset leaves head==tail==001E");
      vdd_input_push(&in, 0x1C0D);
      tail = (uint16_t)(bda[BDA_KB_TAIL] | (bda[BDA_KB_TAIL+1] << 8));
      CHECK(tail == BDA_KB_START + 2, "bda: a key ADVANCES the tail (was frozen forever)");
      CHECK((bda[BDA_KB_START] | (bda[BDA_KB_START+1] << 8)) == 0x1C0D,
            "bda: the keycode is stored at 0040:001E where a DOS program reads it");
    }

    /* T7: INT 09h translation -- the step the stub never performed -------- *
     * A scancode is not a keystroke: it needs the E0 prefix, the shift state and the
     * make/break distinction applied before it means anything to a program.          */
    { fresh(&in, &bus);
      vdd_input_push_scancode(&in, 0x1E); vdd_input_bios_consume(&in);      /* 'a' */
      CHECK(vdd_input_pop(&in, &k) == 1 && k == 0x1E61, "int09: 1E -> AH=1E AL='a'");

      vdd_input_push_scancode(&in, 0x9E); vdd_input_bios_consume(&in);      /* 'a' release */
      CHECK(vdd_input_pop(&in, &k) == 0, "int09: break code stores nothing");

      vdd_input_push_scancode(&in, 0x2A); vdd_input_bios_consume(&in);      /* LShift down */
      CHECK((bda[BDA_KB_FLAGS] & 0x02) != 0, "int09: LShift sets 0040:0017 bit 1");
      vdd_input_push_scancode(&in, 0x1E); vdd_input_bios_consume(&in);
      CHECK(vdd_input_pop(&in, &k) == 1 && k == 0x1E41, "int09: shift+1E -> 'A'");
      vdd_input_push_scancode(&in, 0xAA); vdd_input_bios_consume(&in);      /* LShift up  */
      CHECK((bda[BDA_KB_FLAGS] & 0x02) == 0, "int09: LShift release clears the flag");

      /* The whole point: an arrow is E0 + code, and must arrive as AL=0 so the guest can
         tell it from a character. This is the Skyroads menu case, end to end. */
      vdd_input_push_scancode(&in, 0xE0); vdd_input_bios_consume(&in);
      CHECK(vdd_input_pop(&in, &k) == 0, "int09: E0 prefix alone stores nothing");
      vdd_input_push_scancode(&in, 0x48); vdd_input_bios_consume(&in);
      CHECK(vdd_input_pop(&in, &k) == 1 && k == 0x4800, "int09: E0 48 -> UP (AH=48 AL=0)");
      vdd_input_push_scancode(&in, 0xE0); vdd_input_bios_consume(&in);
      vdd_input_push_scancode(&in, 0x4D); vdd_input_bios_consume(&in);
      CHECK(vdd_input_pop(&in, &k) == 1 && k == 0x4D00, "int09: E0 4D -> RIGHT");

      /* INT 16h AH=02 must report the live shift state, not a hardcoded zero. */
      vdd_input_push_scancode(&in, 0x1D); vdd_input_bios_consume(&in);      /* Ctrl down */
      memset(&r, 0, sizeof r); s_ah(&r, 0x02);
      vdd_bus_deliver_int(&bus, 0x16, &r);
      CHECK(r_al(&r) == 0x04, "int16/02: reports Ctrl held from 0040:0017");
      vdd_input_push_scancode(&in, 0x2E); vdd_input_bios_consume(&in);      /* Ctrl+C    */
      CHECK(vdd_input_pop(&in, &k) == 1 && k == 0x2E03, "int09: Ctrl+C -> AL=03");
    }

    /* T8: enhanced fns AH=10/11 mirror 00/01; unknown fn never phantom-keys *
     * (regression: QuickBasic INKEY$ uses AH=11h; a default ZF=0 made it     *
     * read a phantom key and exit -- BLIT.EXE drew nothing.)                 */
    fresh(&in, &bus);
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
