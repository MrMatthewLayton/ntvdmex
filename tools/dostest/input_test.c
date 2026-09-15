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
/* A fake clock for the keyboard transfer-time tests (T11), and an IRQ counter. */
static uint64_t g_fake_us = 0;
static uint64_t fake_clock(void) { return g_fake_us; }
static uint32_t g_irq1_n = 0;
static void count_irq(void *ctx, uint8_t irq) { if (irq == 1) ++*(uint32_t *)ctx; }

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

    /* T9: ★ A GUEST THAT READS PORT 60h AND THEN CHAINS TO THE BIOS STILL GETS A KEY.
     * QB.EXE 4.5's INT 09h hook (1DDB1h) does `in al,60h`, inspects the byte, and for
     * every ordinary key runs `int 0EFh` -- the BIOS handler it saved. On an 8042 the
     * BIOS's own `in al,60h` reads the SAME byte again. Ours had popped it, so the BIOS
     * arm stored nothing and QBasic could not be typed into. */
    { uint32_t v = 0;
      fresh(&in, &bus);
      vdd_input_push_scancode(&in, 0x1E);                      /* 'a' arrives         */
      vdd_bus_io(&bus, 0x60, 1, 1, &v);                        /* the HOOK reads it   */
      CHECK(v == 0x1E, "int09-chain: the guest hook reads 1E from port 60h");
      CHECK(vdd_input_sc_pending(&in) == 0, "int09-chain: ...and the FIFO is now empty");
      vdd_input_bios_consume(&in);                             /* it chains to the BIOS */
      CHECK(vdd_input_pop(&in, &k) == 1 && k == 0x1E61,
            "int09-chain: the BIOS arm still translates the byte the hook took (QB typing)");
      CHECK(in.sc_owed_served == 1, "int09-chain: ...and counts that it did");
      /* A second chain with nothing new must not re-serve the same byte. */
      vdd_input_bios_consume(&in);
      CHECK(vdd_input_pop(&in, &k) == 0, "int09-chain: an owed byte is served ONCE");
      /* A hook that does NOT chain (Doom's) leaves nothing behind for a later key:
         the next scancode supersedes the owed one, so one key press = one key. */
      vdd_input_push_scancode(&in, 0x1F); vdd_bus_io(&bus, 0x60, 1, 1, &v);  /* 's', not chained */
      vdd_input_push_scancode(&in, 0x20); vdd_input_bios_consume(&in);       /* 'd', BIOS path   */
      CHECK(vdd_input_pop(&in, &k) == 1 && k == 0x2064, "int09-chain: a newer byte supersedes the owed one");
      CHECK(vdd_input_pop(&in, &k) == 0, "int09-chain: ...and the un-chained 's' is not resurrected");
      /* The break code of a key the hook read must not become a key either. */
      vdd_input_push_scancode(&in, 0x9E); vdd_bus_io(&bus, 0x60, 1, 1, &v);
      vdd_input_bios_consume(&in);
      CHECK(vdd_input_pop(&in, &k) == 0, "int09-chain: an owed BREAK code stores nothing");
    }

    /* T10: THE FOUR BIOS COLUMNS -- Shift, Ctrl and ALT, per the IBM table. Alt was
     * never consulted before, so Alt+F typed an 'f' where every DOS editor's menu bar
     * expects 2100h. These are the exact codes editors bind. */
    { fresh(&in, &bus);
#define KEY(sc) (vdd_input_push_scancode(&in, (uint8_t)(sc)), vdd_input_bios_consume(&in))
#define EXPECT(code, msg) CHECK(vdd_input_pop(&in, &k) == 1 && k == (code), msg)
#define NOKEY(msg) CHECK(vdd_input_pop(&in, &k) == 0, msg)
      KEY(0x38); KEY(0x21); KEY(0xA1); KEY(0xB8);              /* Alt+F               */
      EXPECT(0x2100, "int09: Alt+F -> 2100h (a menu accelerator, NOT the letter f)");
      KEY(0x38); KEY(0x3B); KEY(0xBB); KEY(0xB8);              /* Alt+F1              */
      EXPECT(0x6800, "int09: Alt+F1 -> 6800h");
      KEY(0x2A); KEY(0x3B); KEY(0xBB); KEY(0xAA);              /* Shift+F1            */
      EXPECT(0x5400, "int09: Shift+F1 -> 5400h");
      KEY(0x1D); KEY(0x3B); KEY(0xBB); KEY(0x9D);              /* Ctrl+F1             */
      EXPECT(0x5E00, "int09: Ctrl+F1 -> 5E00h");
      KEY(0x1D); KEY(0xE0); KEY(0x4B); KEY(0xE0); KEY(0xCB); KEY(0x9D);   /* Ctrl+Left */
      EXPECT(0x7300, "int09: Ctrl+Left (E0 4B) -> 7300h (word left in every editor)");
      KEY(0x38); KEY(0xE0); KEY(0x4B); KEY(0xE0); KEY(0xCB); KEY(0xB8);   /* Alt+Left  */
      EXPECT(0x9B00, "int09: Alt+Left -> 9B00h (enhanced BIOS)");
      KEY(0x1D); KEY(0x02); KEY(0x82); KEY(0x9D);              /* Ctrl+1              */
      NOKEY("int09: Ctrl+1 stores NOTHING, as the BIOS does");
      KEY(0x1D); KEY(0x0E); KEY(0x8E); KEY(0x9D);              /* Ctrl+Backspace      */
      EXPECT(0x0E7F, "int09: Ctrl+Backspace -> 0E7Fh");
      KEY(0x38); KEY(0x39); KEY(0xB9); KEY(0xB8);              /* Alt+Space           */
      EXPECT(0x3920, "int09: Alt+Space -> 3920h (Alt does not silence Space)");
      KEY(0x3A); KEY(0xBA);                                    /* CapsLock on         */
      KEY(0x1E); KEY(0x9E);
      EXPECT(0x1E41, "int09: CapsLock + a -> 'A'");
      KEY(0x2A); KEY(0x1E); KEY(0x9E); KEY(0xAA);
      EXPECT(0x1E61, "int09: CapsLock + Shift + a -> 'a' (Caps inverts Shift for letters)");
      KEY(0x02); KEY(0x82);
      EXPECT(0x0231, "int09: CapsLock leaves '1' alone");
      KEY(0x3A); KEY(0xBA);                                    /* CapsLock off        */
      KEY(0x47); KEY(0xC7);
      EXPECT(0x4700, "int09: keypad 7 with NumLock off -> Home (4700h)");
      KEY(0x45); KEY(0xC5);                                    /* NumLock on          */
      KEY(0x47); KEY(0xC7);
      EXPECT(0x4737, "int09: keypad 7 with NumLock on -> '7'");
      KEY(0x2A); KEY(0x47); KEY(0xC7); KEY(0xAA);
      EXPECT(0x4700, "int09: Shift undoes NumLock -> Home again");
      KEY(0xE0); KEY(0x1C); KEY(0xE0); KEY(0x9C);              /* keypad Enter        */
      EXPECT(0x1C0D, "int09: keypad Enter (E0 1C) -> 1C0Dh");
      KEY(0x57); KEY(0xD7);
      EXPECT(0x8500, "int09: F11 -> 8500h");
      CHECK((bda[BDA_KB_FLAGS] & 0x0F) == 0, "int09: no modifier left held after all that");
#undef KEY
#undef EXPECT
#undef NOKEY
    }

    /* T11: ★ THE KEYBOARD'S TRANSFER TIME -- two reads in one handler see ONE byte.
     * QB.EXE layers two INT 09h hooks and then chains the BIOS; all three read port 60h
     * for the same interrupt. With bytes already queued our FIFO handed each a different
     * one. With a clock the next byte is held for KBD_XFER_US after a pop, exactly as the
     * keyboard cannot send while the 8042's buffer is full. */
    { uint32_t v = 0; int irqs;
      fresh(&in, &bus);
      in.time_us = fake_clock; g_fake_us = 1000000;
      vdd_bus_set_sinks(&bus, count_irq, &g_irq1_n, 0, 0); g_irq1_n = 0;
      vdd_input_push_scancode(&in, 0x38);                      /* Alt make            */
      vdd_input_push_scancode(&in, 0x21);                      /* F make              */
      vdd_input_push_scancode(&in, 0xA1);                      /* F break             */
      vdd_input_push_scancode(&in, 0xB8);                      /* Alt break           */
      CHECK(g_irq1_n == 1, "hold: four bytes queued at once raise ONE interrupt");
      vdd_bus_io(&bus, 0x60, 1, 1, &v);                        /* hook 1 reads        */
      CHECK(v == 0x38, "hold: the first hook reads 38 (Alt make)");
      vdd_bus_io(&bus, 0x60, 1, 1, &v);                        /* hook 2 re-reads     */
      CHECK(v == 0x38 && in.sc_held_reads == 1, "hold: the second hook reads the SAME byte (was 21: the sequence scrambled)");
      vdd_bus_io(&bus, 0x64, 1, 1, &v);
      CHECK((v & 1) == 0, "hold: OBF reads clear while the keyboard is still sending the next byte");
      vdd_input_bios_consume(&in);                             /* BIOS chained        */
      CHECK(vdd_input_pop(&in, &k) == 0 && (bda[BDA_KB_FLAGS] & 0x08),
            "hold: the chained BIOS translates the owed 38 -> Alt flag set, no key, FIFO untouched");
      CHECK(vdd_input_sc_queued(&in) == 3, "hold: three bytes still queued");
      irqs = (int)g_irq1_n;
      vdd_input_poll(&in);
      CHECK((int)g_irq1_n == irqs, "hold: no new interrupt before the transfer time passes");
      g_fake_us += KBD_XFER_US;
      vdd_input_poll(&in);
      CHECK((int)g_irq1_n == irqs + 1, "hold: the transfer time passes -> IRQ1 for the next byte");
      vdd_bus_io(&bus, 0x64, 1, 1, &v);
      CHECK((v & 1) == 1, "hold: ...and OBF is set again");
      vdd_bus_io(&bus, 0x60, 1, 1, &v);
      CHECK(v == 0x21, "hold: the next interrupt's read is 21 (F make)");
      vdd_input_bios_consume(&in);
      CHECK(vdd_input_pop(&in, &k) == 1 && k == 0x2100, "hold: ...which the chained BIOS turns into Alt+F = 2100h");
      /* Drain the rest at the keyboard's pace. */
      g_fake_us += KBD_XFER_US; vdd_input_poll(&in); vdd_bus_io(&bus, 0x60, 1, 1, &v); vdd_input_bios_consume(&in);
      g_fake_us += KBD_XFER_US; vdd_input_poll(&in); vdd_bus_io(&bus, 0x60, 1, 1, &v); vdd_input_bios_consume(&in);
      CHECK(v == 0xB8 && (bda[BDA_KB_FLAGS] & 0x08) == 0 && vdd_input_sc_queued(&in) == 0,
            "hold: Alt break arrives last, in order, and clears the flag");
      CHECK(g_irq1_n == 4, "hold: exactly one interrupt per byte");
      in.time_us = 0; vdd_bus_set_sinks(&bus, 0, 0, 0, 0);
    }

    /* T12: ★ THE WRITE SIDE OF THE RING -- INT 16h AH=05h, AH=09h, AH=03h.
         All three fell into the `default` arm, which sets ZF and leaves AX exactly
         as the caller passed it. So a key-stuffing program read its OWN byte back
         out of AL and took it for "stored, success", and nothing was ever queued.
         Found by p_kbd.asm against the 6.22 oracle -- and only once that probe
         POISONED AL, because "untouched" and "answered 00" are the same picture
         otherwise. This is the whole of DOSKEY, of an installer that pre-answers
         its own prompt, and of every TSR that drives another program. */
    {   int i;
        vdd_input_reset(&in);
        memset(&r, 0, sizeof r); s_ah(&r, 0x05); s_al(&r, 0xB1);
        r.ecx = 0x1C0D;                                       /* Enter */
        vdd_bus_deliver_int(&bus, 0x16, &r);
        CHECK(r_al(&r) == 0x00, "int16/05: a stored keystroke answers AL=0 (was AL untouched)");
        CHECK(vdd_input_pop(&in, &k) == 1 && k == 0x1C0D,
              "int16/05: ...and the key is really in the ring, in order");

        /* FULL is the other half of the contract: a 16-slot ring takes fifteen and
           then REFUSES, rather than overwriting the head and losing a keystroke the
           guest has already been told about. */
        vdd_input_reset(&in);
        for (i = 0; i < 15; ++i) {
            memset(&r, 0, sizeof r); s_ah(&r, 0x05); s_al(&r, 0xB1);
            r.ecx = (uint32_t)(0x3930 + (i % 10));
            vdd_bus_deliver_int(&bus, 0x16, &r);
            if (r_al(&r) != 0) break;
        }
        CHECK(i == 15, "int16/05: fifteen entries fit a sixteen-slot ring");
        memset(&r, 0, sizeof r); s_ah(&r, 0x05); s_al(&r, 0xB1);
        r.ecx = 0x3932;
        vdd_bus_deliver_int(&bus, 0x16, &r);
        CHECK(r_al(&r) == 0x01, "int16/05: the sixteenth is refused with AL=1, not silently dropped");
        CHECK(vdd_input_pop(&in, &k) == 1 && k == 0x3930,
              "int16/05: ...and the OLDEST key survived the refusal");

        /* AH=09h: 0x30 is MEASURED on the 6.22 oracle, not derived -- the bit
           definitions disagree between references, which is exactly the kind of
           expectation M9 forbids writing from memory. */
        memset(&r, 0, sizeof r); s_ah(&r, 0x09); s_al(&r, 0xB1);
        vdd_bus_deliver_int(&bus, 0x16, &r);
        CHECK(r_al(&r) == 0x30, "int16/09: supported-function mask = 30 (oracle-measured)");

        /* AH=03h stores nothing, but it must be ANSWERED: a guest that sets the
           typematic rate and gets CF=1 can conclude there is no BIOS here at all. */
        memset(&r, 0, sizeof r); s_ah(&r, 0x03); s_al(&r, 0x05); r.cf = 1;
        vdd_bus_deliver_int(&bus, 0x16, &r);
        CHECK(r.cf == 0, "int16/03: set typematic rate is accepted (CF=0)");
        vdd_input_reset(&in);
    }

    printf("\n%d checks, %d failed\n", total, fails);
    return fails ? 1 : 0;
}
