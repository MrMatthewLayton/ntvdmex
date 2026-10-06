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
/* #274: the whole of segment 0x40, because 0040:0080/0082 may move the ring anywhere in it. */
static uint8_t bda[0x10000];
/* A fake clock for the keyboard transfer-time tests (T11), and an IRQ counter. */
static uint64_t g_fake_us = 0;
static uint64_t fake_clock(void) { return g_fake_us; }
static uint32_t g_irq1_n = 0;
static void count_irq(void *ctx, uint8_t irq) { if (irq == 1) ++*(uint32_t *)ctx; }

static void fresh(input_state *in, VDD_BUS *bus)
{
    memset(in, 0, sizeof *in);
    memset(bda, 0, sizeof bda);
    in->bus = bus;
    in->bda = bda;
    vdd_input_reset(in);
}

int main(void)
{
    VDD_BUS bus;
    input_state in; memset(&in, 0, sizeof in);
    NTVDD_DEVICE dev = vdd_input_device(&in);
    NTVDD_REGISTERS r; uint16_t k;

    printf("== M3 slice-6 keyboard input battery ==\n");

    in.bda = bda;                    /* before add(): init resets the ring pointers */
    VddBusInitialize(&bus, 0);
    CHECK(VddBusAdd(&bus, &dev) == 0, "add: input init ok");
    CHECK(bus.Interrupts[0x16].Service != 0, "add: INT 16h claimed");

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
    memset(&r, 0, sizeof r); VddSetAh(&r, 0x01);
    VddBusDeliverInterrupt(&bus, 0x16, &r);
    CHECK(r.ZeroFlag == 1, "int16/01: ZF=1 when no key");
    vdd_input_push(&in, 0x1E61);                     /* 'a' */
    memset(&r, 0, sizeof r); VddSetAh(&r, 0x01);
    VddBusDeliverInterrupt(&bus, 0x16, &r);
    CHECK(r.ZeroFlag == 0 && VddGetAx(&r) == 0x1E61, "int16/01: ZF=0 + AX=key when ready");
    /* AH=01 is a peek -> the key is still there */
    CHECK(vdd_input_peek(&in, &k) == 1 && k == 0x1E61, "int16/01: peek did not consume");

    /* T4: INT 16h AH=00 (read) consumes; ZF=1 when empty ----------------- */
    memset(&r, 0, sizeof r); VddSetAh(&r, 0x00);
    VddBusDeliverInterrupt(&bus, 0x16, &r);
    CHECK(r.ZeroFlag == 0 && VddGetAx(&r) == 0x1E61, "int16/00: returns the key");
    CHECK(vdd_input_pop(&in, &k) == 0, "int16/00: consumed the key");
    memset(&r, 0, sizeof r); VddSetAh(&r, 0x00);
    VddBusDeliverInterrupt(&bus, 0x16, &r);
    CHECK(r.ZeroFlag == 1, "int16/00: ZF=1 when empty (host then blocks)");

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
      CHECK(vdd_input_pop(&in, &k) == 1 && k == 0x48E0 && vdd_input_dos_key(k) == 0x4800, "int09: E0 48 -> grey UP 48E0h; DOS CON (the Skyroads menu route) still sees AH=48 AL=0");
      vdd_input_push_scancode(&in, 0xE0); vdd_input_bios_consume(&in);
      vdd_input_push_scancode(&in, 0x4D); vdd_input_bios_consume(&in);
      CHECK(vdd_input_pop(&in, &k) == 1 && k == 0x4DE0 && vdd_input_dos_key(k) == 0x4D00, "int09: E0 4D -> grey RIGHT 4DE0h (CON: 4D00h)");

      /* INT 16h AH=02 must report the live shift state, not a hardcoded zero. */
      vdd_input_push_scancode(&in, 0x1D); vdd_input_bios_consume(&in);      /* Ctrl down */
      memset(&r, 0, sizeof r); VddSetAh(&r, 0x02);
      VddBusDeliverInterrupt(&bus, 0x16, &r);
      CHECK(VddGetAl(&r) == 0x04, "int16/02: reports Ctrl held from 0040:0017");
      vdd_input_push_scancode(&in, 0x2E); vdd_input_bios_consume(&in);      /* Ctrl+C    */
      CHECK(vdd_input_pop(&in, &k) == 1 && k == 0x2E03, "int09: Ctrl+C -> AL=03");
    }

    /* T8: enhanced fns AH=10/11 mirror 00/01; unknown fn never phantom-keys *
     * (regression: QuickBasic INKEY$ uses AH=11h; a default ZF=0 made it     *
     * read a phantom key and exit -- BLIT.EXE drew nothing.)                 */
    fresh(&in, &bus);
    memset(&r, 0, sizeof r); VddSetAh(&r, 0x11);            /* enhanced check, empty */
    VddBusDeliverInterrupt(&bus, 0x16, &r);
    CHECK(r.ZeroFlag == 1, "int16/11: ZF=1 when no key (INKEY$ -> \"\")");
    vdd_input_push(&in, 0x1C0D);                        /* Enter */
    memset(&r, 0, sizeof r); VddSetAh(&r, 0x11);
    VddBusDeliverInterrupt(&bus, 0x16, &r);
    CHECK(r.ZeroFlag == 0 && VddGetAx(&r) == 0x1C0D, "int16/11: ZF=0 + AX=key when ready");
    memset(&r, 0, sizeof r); VddSetAh(&r, 0x10);            /* enhanced read consumes */
    VddBusDeliverInterrupt(&bus, 0x16, &r);
    CHECK(r.ZeroFlag == 0 && VddGetAx(&r) == 0x1C0D, "int16/10: enhanced read returns key");
    CHECK(vdd_input_pop(&in, &k) == 0, "int16/10: consumed the key");
    memset(&r, 0xFF, sizeof r); VddSetAh(&r, 0x55);         /* unknown fn */
    VddBusDeliverInterrupt(&bus, 0x16, &r);
    CHECK(r.ZeroFlag == 1, "int16/unknown: ZF=1, never a phantom key");

    /* T9: ★ A GUEST THAT READS PORT 60h AND THEN CHAINS TO THE BIOS STILL GETS A KEY.
     * QB.EXE 4.5's INT 09h hook (1DDB1h) does `in al,60h`, inspects the byte, and for
     * every ordinary key runs `int 0EFh` -- the BIOS handler it saved. On an 8042 the
     * BIOS's own `in al,60h` reads the SAME byte again. Ours had popped it, so the BIOS
     * arm stored nothing and QBasic could not be typed into. */
    { uint32_t v = 0;
      fresh(&in, &bus);
      vdd_input_push_scancode(&in, 0x1E);                      /* 'a' arrives         */
      VddBusIo(&bus, 0x60, 1, 1, &v);                        /* the HOOK reads it   */
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
      vdd_input_push_scancode(&in, 0x1F); VddBusIo(&bus, 0x60, 1, 1, &v);  /* 's', not chained */
      vdd_input_push_scancode(&in, 0x20); vdd_input_bios_consume(&in);       /* 'd', BIOS path   */
      CHECK(vdd_input_pop(&in, &k) == 1 && k == 0x2064, "int09-chain: a newer byte supersedes the owed one");
      CHECK(vdd_input_pop(&in, &k) == 0, "int09-chain: ...and the un-chained 's' is not resurrected");
      /* The break code of a key the hook read must not become a key either. */
      vdd_input_push_scancode(&in, 0x9E); VddBusIo(&bus, 0x60, 1, 1, &v);
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
      EXPECT(0x73E0, "int09: Ctrl+grey Left (E0 4B) -> 73E0h (#254: AH=00h folds it to 7300h)");
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
      EXPECT(0xE00D, "int09: keypad Enter (E0 1C) -> E00Dh (#254: AH=00h folds it to 1C0Dh)");
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
      VddBusSetSinks(&bus, count_irq, &g_irq1_n, 0, 0); g_irq1_n = 0;
      vdd_input_push_scancode(&in, 0x38);                      /* Alt make            */
      vdd_input_push_scancode(&in, 0x21);                      /* F make              */
      vdd_input_push_scancode(&in, 0xA1);                      /* F break             */
      vdd_input_push_scancode(&in, 0xB8);                      /* Alt break           */
      CHECK(g_irq1_n == 1, "hold: four bytes queued at once raise ONE interrupt");
      VddBusIo(&bus, 0x60, 1, 1, &v);                        /* hook 1 reads        */
      CHECK(v == 0x38, "hold: the first hook reads 38 (Alt make)");
      VddBusIo(&bus, 0x60, 1, 1, &v);                        /* hook 2 re-reads     */
      CHECK(v == 0x38 && in.sc_held_reads == 1, "hold: the second hook reads the SAME byte (was 21: the sequence scrambled)");
      VddBusIo(&bus, 0x64, 1, 1, &v);
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
      VddBusIo(&bus, 0x64, 1, 1, &v);
      CHECK((v & 1) == 1, "hold: ...and OBF is set again");
      VddBusIo(&bus, 0x60, 1, 1, &v);
      CHECK(v == 0x21, "hold: the next interrupt's read is 21 (F make)");
      vdd_input_bios_consume(&in);
      CHECK(vdd_input_pop(&in, &k) == 1 && k == 0x2100, "hold: ...which the chained BIOS turns into Alt+F = 2100h");
      /* Drain the rest at the keyboard's pace. */
      g_fake_us += KBD_XFER_US; vdd_input_poll(&in); VddBusIo(&bus, 0x60, 1, 1, &v); vdd_input_bios_consume(&in);
      g_fake_us += KBD_XFER_US; vdd_input_poll(&in); VddBusIo(&bus, 0x60, 1, 1, &v); vdd_input_bios_consume(&in);
      CHECK(v == 0xB8 && (bda[BDA_KB_FLAGS] & 0x08) == 0 && vdd_input_sc_queued(&in) == 0,
            "hold: Alt break arrives last, in order, and clears the flag");
      CHECK(g_irq1_n == 4, "hold: exactly one interrupt per byte");
      in.time_us = 0; VddBusSetSinks(&bus, 0, 0, 0, 0);
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
        memset(&r, 0, sizeof r); VddSetAh(&r, 0x05); VddSetAl(&r, 0xB1);
        r.Ecx = 0x1C0D;                                       /* Enter */
        VddBusDeliverInterrupt(&bus, 0x16, &r);
        CHECK(VddGetAl(&r) == 0x00, "int16/05: a stored keystroke answers AL=0 (was AL untouched)");
        CHECK(vdd_input_pop(&in, &k) == 1 && k == 0x1C0D,
              "int16/05: ...and the key is really in the ring, in order");

        /* FULL is the other half of the contract: a 16-slot ring takes fifteen and
           then REFUSES, rather than overwriting the head and losing a keystroke the
           guest has already been told about. */
        vdd_input_reset(&in);
        for (i = 0; i < 15; ++i) {
            memset(&r, 0, sizeof r); VddSetAh(&r, 0x05); VddSetAl(&r, 0xB1);
            r.Ecx = (uint32_t)(0x3930 + (i % 10));
            VddBusDeliverInterrupt(&bus, 0x16, &r);
            if (VddGetAl(&r) != 0) break;
        }
        CHECK(i == 15, "int16/05: fifteen entries fit a sixteen-slot ring");
        memset(&r, 0, sizeof r); VddSetAh(&r, 0x05); VddSetAl(&r, 0xB1);
        r.Ecx = 0x3932;
        VddBusDeliverInterrupt(&bus, 0x16, &r);
        CHECK(VddGetAl(&r) == 0x01, "int16/05: the sixteenth is refused with AL=1, not silently dropped");
        CHECK(vdd_input_pop(&in, &k) == 1 && k == 0x3930,
              "int16/05: ...and the OLDEST key survived the refusal");

        /* AH=09h: MEASURED, not derived -- the bit definitions disagree between
           references. #188: 0xB1 on PCem's genuine AMI BIOS (and DOSBox-X); the old
           0x30 was QEMU's SeaBIOS. Bit 4 promises AH=0Ah, so that answers too. */
        memset(&r, 0, sizeof r); VddSetAh(&r, 0x09); VddSetAl(&r, 0xB1);
        VddBusDeliverInterrupt(&bus, 0x16, &r);
        CHECK(VddGetAl(&r) == 0xB1, "int16/09: supported-function mask = B1 (PCem AMI BIOS)");
        memset(&r, 0, sizeof r); VddSetAh(&r, 0x0A); r.Ebx = 0xB1B1;
        VddBusDeliverInterrupt(&bus, 0x16, &r);
        CHECK((r.Ebx & 0xFFFF) == 0x41AB, "int16/0A: keyboard ID 41AB (MF2), as 09h bit 4 promises");

        /* #188: AH=00h/01h DISCARD a code only a 101-key keyboard makes (F11 = 8500h),
           consuming it -- p_kbd 16.01.enh on PCem: empty, head moved on -- while
           AH=11h still sees it. Gray-key E0 forms are rewritten for 00h/01h. */
        vdd_input_reset(&in);
        vdd_input_push(&in, 0x8500);
        memset(&r, 0, sizeof r); VddSetAh(&r, 0x11);
        VddBusDeliverInterrupt(&bus, 0x16, &r);
        CHECK(r.ZeroFlag == 0 && VddGetAx(&r) == 0x8500, "int16/11: the enhanced call sees F11");
        memset(&r, 0, sizeof r); VddSetAh(&r, 0x01);
        VddBusDeliverInterrupt(&bus, 0x16, &r);
        CHECK(r.ZeroFlag == 1, "int16/01: F11 alone reads as empty to an 83-key call");
        CHECK(vdd_input_peek(&in, &k) == 0, "int16/01: ...and it was CONSUMED, not skipped over");
        vdd_input_push(&in, 0x8500); vdd_input_push(&in, 0x4BE0); vdd_input_push(&in, 0xE00D);
        memset(&r, 0, sizeof r); VddSetAh(&r, 0x00);
        VddBusDeliverInterrupt(&bus, 0x16, &r);
        CHECK(r.ZeroFlag == 0 && VddGetAx(&r) == 0x4B00, "int16/00: F11 discarded, gray Left 4BE0 -> 4B00");
        memset(&r, 0, sizeof r); VddSetAh(&r, 0x00);
        VddBusDeliverInterrupt(&bus, 0x16, &r);
        CHECK(r.ZeroFlag == 0 && VddGetAx(&r) == 0x1C0D, "int16/00: keypad Enter E00D -> 1C0D");
        vdd_input_reset(&in);

        /* AH=03h stores nothing, but it must be ANSWERED: a guest that sets the
           typematic rate and gets CF=1 can conclude there is no BIOS here at all. */
        memset(&r, 0, sizeof r); VddSetAh(&r, 0x03); VddSetAl(&r, 0x05); r.CarryFlag = 1;
        VddBusDeliverInterrupt(&bus, 0x16, &r);
        CHECK(r.CarryFlag == 0, "int16/03: set typematic rate is accepted (CF=0)");
        vdd_input_reset(&in);
    }

    /* ── THE 8042 AS A CONTROLLER. docs/ref/kbc.md; docs/inventory/kbc.md. ─────
       Everything above is the keyboard's byte stream. These are the chip it
       passes through -- and two of the things it controls have nothing to do
       with typing. Marked from the code first, so every one was predicted to
       fail; p_kbc.asm asks the same questions of three real machines. */
    {
        uint32_t v;
        vdd_input_reset(&in);

        /* THE STATUS REGISTER. ★ ALL THREE ORACLES ANSWER 0x1C with OBF and AUXB
           masked off (p_kbc kbc.status.idle) -- 6.22, dosbox-x and PCem alike.
           Unanimous, so no judgement was needed; we answered 0x00. Bit 2 is SYS,
           which POST sets on any machine DOS runs on. */
        VddBusIo(&bus, 0x64, 1, 1, &v);
        /* ⚠ 0x1C, NOT 0x14. This check was written as 0x14 -- SYS and INH -- and
             the probe then measured 0x1C on ALL THREE oracles. The extra bit is
             A2 (bit 3): "the last write went to 64h", which on a machine DOS is
             running on was POST's own last command. Writing the expectation from
             the datasheet's bit list got the bits right and their POST STATE
             wrong, which is exactly what an oracle is for. */
        CHECK((v & 0xDE) == 0x1C, "8042: idle status has SYS, INH and A2 set");
        CHECK((v & 0x01) == 0, "8042: ...and OBF clear with nothing buffered");

        /* A COMMAND IS ANSWERED. PCem, on a real AMI BIOS, replies 0x55 to the
           self test; QEMU and dosbox-x do not answer it at all. */
        v = 0xAA; VddBusIo(&bus, 0x64, 1, 0, &v);
        VddBusIo(&bus, 0x64, 1, 1, &v);
        CHECK((v & 0x01) != 0, "8042: AAh self test presents a reply (OBF set)");
        VddBusIo(&bus, 0x60, 1, 1, &v);
        CHECK(v == 0x55, "8042: ...and the reply is 0x55");
        VddBusIo(&bus, 0x64, 1, 1, &v);
        CHECK((v & 0x01) == 0, "8042: ...consumed by the read, so OBF clears");

        /* A CONTROLLER REPLY MUST NOT BE A SCANCODE. This is the whole reason
           the reply lives in its own byte: with a key queued AND a command
           pending, the command's answer comes first. Getting this wrong hands a
           driver a KEYSTROKE where it asked for the output port, and it reads
           bit 1 of it as the state of the A20 gate. */
        vdd_input_reset(&in);
        vdd_input_push_scancode(&in, 0x1E);          /* 'a' waiting             */
        v = 0xD0; VddBusIo(&bus, 0x64, 1, 0, &v);  /* read output port        */
        VddBusIo(&bus, 0x60, 1, 1, &v);
        CHECK(v != 0x1E, "8042: a command reply is not the queued scancode");
        CHECK((v & 0x01) != 0, "8042: the output port has the reset line HIGH");
        CHECK((v & 0x02) != 0, "8042: ...and A20 open, as every BIOS leaves it");

        /* A20 THROUGH ALL THREE DOORS -- ONE WIRE. */
        vdd_input_reset(&in);
        CHECK(vdd_input_a20_get(&in) == 1, "a20: open after POST, as on a real machine");

        v = 0xD1; VddBusIo(&bus, 0x64, 1, 0, &v);  /* write output port       */
        v = 0xDD; VddBusIo(&bus, 0x60, 1, 0, &v);  /* A20 OFF, reset line high */
        CHECK(vdd_input_a20_get(&in) == 0, "a20: the 8042 output port closes it");
        VddBusIo(&bus, 0x92, 1, 1, &v);
        CHECK((v & 0x02) == 0, "a20: ...and port 92h agrees it is shut");

        v = 0x02; VddBusIo(&bus, 0x92, 1, 0, &v);  /* fast A20 opens it       */
        CHECK(vdd_input_a20_get(&in) == 1, "a20: port 92h opens it");
        v = 0xD0; VddBusIo(&bus, 0x64, 1, 0, &v);
        VddBusIo(&bus, 0x60, 1, 1, &v);
        CHECK((v & 0x02) != 0, "a20: ...and the 8042 output port agrees it is open");

        /* ⛔ A RESET REQUEST IS COUNTED, NOT OBEYED. A VDD cannot reboot the
           machine it is a guest on, and pretending to would be worse than the
           count -- but dropping it silently is worse still. */
        {
            uint32_t before = in.kbc_reset_asked;
            v = 0xFE; VddBusIo(&bus, 0x64, 1, 0, &v);
            CHECK(in.kbc_reset_asked == before + 1, "8042: FEh pulse-reset is counted");
            v = 0x01; VddBusIo(&bus, 0x92, 1, 0, &v);
            CHECK(in.kbc_reset_asked == before + 2, "a20: port 92h fast reset is counted too");
        }
        vdd_input_reset(&in);
    }

    /* #136: KEYBOARD LAYOUTS (tables from XP's own layouts, runs/s82/kbdmap.txt). */
    {   uint16_t k; uint8_t sc; int sh;
#define TYPE1(code) do { vdd_input_push_scancode(&in, (code)); vdd_input_bios_consume(&in); \
                         vdd_input_push_scancode(&in, (uint8_t)((code) | 0x80)); vdd_input_bios_consume(&in); } while (0)
#define SHIFTED(code) do { vdd_input_push_scancode(&in, 0x2A); vdd_input_bios_consume(&in); TYPE1(code); \
                           vdd_input_push_scancode(&in, 0xAA); vdd_input_bios_consume(&in); } while (0)
        fresh(&in, &bus);
        in.layout = 1;                                         /* United Kingdom */
        SHIFTED(0x03); CHECK(vdd_input_pop(&in, &k) == 1 && k == 0x0322, "UK: Shift+2 is \" (0322h)");
        SHIFTED(0x04); CHECK(vdd_input_pop(&in, &k) == 1 && k == 0x049C, "UK: Shift+3 is \x9C (the pound sign in CP437)");
        SHIFTED(0x28); CHECK(vdd_input_pop(&in, &k) == 1 && k == 0x2840, "UK: Shift+' is @");
        TYPE1(0x2B);   CHECK(vdd_input_pop(&in, &k) == 1 && k == 0x2B23, "UK: the key beside Enter is #");
        TYPE1(0x1E);   CHECK(vdd_input_pop(&in, &k) == 1 && k == 0x1E61, "UK: an unchanged key is untouched (a)");
        CHECK(vdd_input_char_to_key(&in, '@', &sc, &sh) && sc == 0x28 && sh == 1,
              "UK paste: '@' is typed as Shift + the ' key");
        in.layout = 0;                                         /* US again */
        SHIFTED(0x03); CHECK(vdd_input_pop(&in, &k) == 1 && k == 0x0340, "US: Shift+2 is still @");
        CHECK(vdd_input_char_to_key(&in, '@', &sc, &sh) && sc == 0x03 && sh == 1,
              "US paste: '@' is Shift+2");
        in.layout = 2;                                         /* German: Y and Z swap */
        TYPE1(0x15);   CHECK(vdd_input_pop(&in, &k) == 1 && k == 0x157A, "DE: the key at US-Y types z");
        vdd_input_push_scancode(&in, 0x1D); vdd_input_bios_consume(&in);  /* Ctrl down */
        TYPE1(0x15);   CHECK(vdd_input_pop(&in, &k) == 1 && k == 0x151A, "DE: Ctrl+ that key is Ctrl+Z (1Ah)");
        vdd_input_push_scancode(&in, 0x9D); vdd_input_bios_consume(&in);  /* Ctrl up */
        in.layout = 0;
#undef TYPE1
#undef SHIFTED
    }

    /* T12: #254 -- the grey-key E0 forms, AH=12h's layout, and what the BIOS INT 09h
     * does besides store a key (Ctrl-Break, Print Screen, SysReq, Pause, Insert). */
    { int act;
      fresh(&in, &bus);
#define KEY(sc) (vdd_input_push_scancode(&in, (uint8_t)(sc)), vdd_input_bios_consume(&in))
#define EXPECT(code, msg) CHECK(vdd_input_pop(&in, &k) == 1 && k == (code), msg)
#define NOKEY(msg) CHECK(vdd_input_pop(&in, &k) == 0, msg)
#define AH12() (memset(&r, 0, sizeof r), VddSetAh(&r, 0x12), VddBusDeliverInterrupt(&bus, 0x16, &r), (uint8_t)(VddGetAx(&r) >> 8))
      KEY(0xE0); KEY(0x4B); KEY(0xE0); KEY(0xCB);
      EXPECT(0x4BE0, "e0: grey Left -> 4BE0h");
      KEY(0x4B); KEY(0xCB);
      EXPECT(0x4B00, "e0: keypad Left (no E0) -> 4B00h -- the two clusters now differ");
      KEY(0x2A); KEY(0xE0); KEY(0x48); KEY(0xE0); KEY(0xC8); KEY(0xAA);
      EXPECT(0x48E0, "e0: Shift+grey Up -> 48E0h (Shift changes nothing)");
      KEY(0xE0); KEY(0x35); KEY(0xE0); KEY(0xB5);
      EXPECT(0xE02F, "e0: keypad / -> E02Fh");
      KEY(0x1D); KEY(0xE0); KEY(0x1C); KEY(0xE0); KEY(0x9C); KEY(0x9D);
      EXPECT(0xE00A, "e0: Ctrl+keypad Enter -> E00Ah");
      KEY(0x1D); KEY(0xE0); KEY(0x48); KEY(0xE0); KEY(0xC8); KEY(0x9D);
      EXPECT(0x8DE0, "e0: Ctrl+grey Up -> 8DE0h");
      CHECK(vdd_input_dos_key(0x4BE0) == 0x4B00 && vdd_input_dos_key(0xE00D) == 0x1C0D
            && vdd_input_dos_key(0xE02F) == 0x352F && vdd_input_dos_key(0x1E61) == 0x1E61,
            "e0: DOS's CON sees the 83-key forms (no 0E0h character)");
      vdd_input_push(&in, 0x8DE0); vdd_input_push(&in, 0x73E0);
      memset(&r, 0, sizeof r); VddSetAh(&r, 0x00); VddBusDeliverInterrupt(&bus, 0x16, &r);
      CHECK(r.ZeroFlag == 0 && VddGetAx(&r) == 0x7300, "e0: AH=00h discards Ctrl+Up (no 83-key code), folds 73E0h -> 7300h");

      /* AH=12h, and the left/right halves of Ctrl and Alt */
      CHECK(AH12() == 0x00, "12h: nothing held -> AH=00");
      KEY(0x1D);
      CHECK(AH12() == 0x01 && (bda[BDA_KB_FLAGS] & 0x04), "12h: left Ctrl -> AH bit 0, 0017 Ctrl");
      KEY(0xE0); KEY(0x1D);
      CHECK(AH12() == 0x05, "12h: + right Ctrl -> AH bits 0 and 2");
      KEY(0x9D);
      CHECK(AH12() == 0x04 && (bda[BDA_KB_FLAGS] & 0x04),
            "12h: left released, right still held -> 0017 Ctrl STAYS set");
      KEY(0xE0); KEY(0x9D);
      CHECK(AH12() == 0x00 && !(bda[BDA_KB_FLAGS] & 0x04), "12h: both released -> Ctrl clear");
      KEY(0xE0); KEY(0x38);
      CHECK(AH12() == 0x08 && (bda[0x96] & 0x08), "12h: right Alt -> AH bit 3, 0096 bit 3");
      KEY(0xE0); KEY(0xB8);
      KEY(0x3A);
      CHECK(AH12() == 0x40 && (bda[BDA_KB_FLAGS] & 0x40), "12h: Caps held -> AH bit 6, Caps on");
      KEY(0x3A);                                           /* typematic repeat */
      CHECK(bda[BDA_KB_FLAGS] & 0x40, "lock: a held key's repeats do not re-toggle Caps");
      KEY(0xBA); KEY(0x3A); KEY(0xBA);
      CHECK(!(bda[BDA_KB_FLAGS] & 0x40) && AH12() == 0x00, "lock: a second press toggles Caps off");

      /* SysReq */
      act = KEY(0x54);
      CHECK(act == KB_ACT_SYSRQ_D && AH12() == 0x80 && (bda[BDA_KB_FLAGS2] & 0x04),
            "sysrq: press -> INT 15h 8500h, AH=12h bit 7, 0018 bit 2");
      act = KEY(0xD4);
      CHECK(act == KB_ACT_SYSRQ_U && AH12() == 0x00, "sysrq: release -> INT 15h 8501h");
      NOKEY("sysrq: stores nothing");

      /* Print Screen */
      act = KEY(0xE0); act = KEY(0x37);
      CHECK(act == KB_ACT_PRTSC, "prtsc: E0 37 -> INT 05h");
      KEY(0xE0); KEY(0xB7);
      NOKEY("prtsc: stores nothing (was 3700h)");
      KEY(0x37); KEY(0xB7);
      EXPECT(0x372A, "prtsc: keypad * (no E0) is still '*'");
      KEY(0x1D); KEY(0xE0); KEY(0x37); KEY(0xE0); KEY(0xB7); KEY(0x9D);
      EXPECT(0x7200, "prtsc: Ctrl+PrtSc -> 7200h");

      /* Ctrl-Break */
      vdd_input_push(&in, 0x1E61); vdd_input_push(&in, 0x1F73);
      bda[0x71] = 0;
      KEY(0x1D); KEY(0xE0); act = KEY(0x46);
      CHECK(act == KB_ACT_BREAK, "break: Ctrl + E0 46 -> INT 1Bh");
      CHECK(bda[0x71] & 0x80, "break: 0040:0071 bit 7 set");
      EXPECT(0x0000, "break: the ring was emptied and 0000h stored");
      NOKEY("break: nothing else left in the ring");
      KEY(0xE0); KEY(0xC6); KEY(0x9D);
      CHECK(!(bda[BDA_KB_FLAGS] & 0x10), "break: Scroll Lock NOT toggled");
      KEY(0x46); KEY(0xC6);
      CHECK(bda[BDA_KB_FLAGS] & 0x10, "break: plain Scroll Lock still toggles");
      KEY(0x46); KEY(0xC6);

      /* Pause */
      act = KEY(0xE1); act = KEY(0x1D); act = KEY(0x45);
      CHECK(act == KB_ACT_PAUSE && (bda[BDA_KB_FLAGS2] & 0x08), "pause: E1 1D 45 -> pause, 0018 bit 3");
      CHECK(!(bda[BDA_KB_FLAGS] & 0x24), "pause: neither Ctrl nor NumLock changed");
      KEY(0xE1); KEY(0x9D); KEY(0xC5);
      CHECK(bda[BDA_KB_FLAGS2] & 0x08, "pause: its break sequence does not end it");
      KEY(0x2A); KEY(0xAA);
      CHECK(bda[BDA_KB_FLAGS2] & 0x08, "pause: a shift key does not end it");
      KEY(0x1E); KEY(0x9E);
      CHECK(!(bda[BDA_KB_FLAGS2] & 0x08), "pause: the next keystroke ends it...");
      NOKEY("pause: ...and is thrown away");
      act = KEY(0xE1); act = KEY(0x1D); act = KEY(0x45);
      vdd_input_pause_cancel(&in);
      CHECK(!(bda[BDA_KB_FLAGS2] & 0x08), "pause: cancel (a caller that cannot spin) clears it");
      KEY(0xE1); KEY(0x9D); KEY(0xC5);

      /* Insert */
      KEY(0xE0); KEY(0x52);
      CHECK((bda[BDA_KB_FLAGS] & 0x80) && (bda[BDA_KB_FLAGS2] & 0x80), "ins: grey Insert toggles 0017 bit 7, held 0018 bit 7");
      EXPECT(0x52E0, "ins: and is stored (52E0h)");
      KEY(0xE0); KEY(0x52);                                /* repeat */
      CHECK(bda[BDA_KB_FLAGS] & 0x80, "ins: a repeat does not toggle back");
      (void)vdd_input_pop(&in, &k);
      KEY(0xE0); KEY(0xD2);
      CHECK(!(bda[BDA_KB_FLAGS2] & 0x80), "ins: release clears the held bit");
      KEY(0x45); KEY(0xC5);                                /* NumLock on */
      KEY(0x52); KEY(0xD2);
      CHECK(bda[BDA_KB_FLAGS] & 0x80, "ins: keypad 0 with NumLock on is '0', no toggle");
      EXPECT(0x5230, "ins: ...stored as '0'");
#undef KEY
#undef EXPECT
#undef NOKEY
#undef AH12
    }

    /* T13: #244 / #274 -- the keyboard BIOS remainders.
         (a) the ring's bounds come from 0040:0080/0082, not from two constants;
         (b) Alt + keypad digits = the character with that decimal code (0040:0019);
         (c) the INT 09h handler split around INT 15h AH=4Fh: fetch() + translate();
         (d) 8042 command D2h puts a byte in the output buffer as if typed, IRQ1 included;
         (e) the bytes the host sends for Pause and Ctrl+Break. */
    {   uint16_t v16;
        uint32_t v;
        uint8_t  b[6];
        int      n, norep, sc, act, i, ok;
#define KEY(sc) (vdd_input_push_scancode(&in, (uint8_t)(sc)), vdd_input_bios_consume(&in))
#define EXPECT(code, msg) CHECK(vdd_input_pop(&in, &k) == 1 && k == (code), msg)
#define NOKEY(msg) CHECK(vdd_input_pop(&in, &k) == 0, msg)
#define W16(o) ((uint16_t)(bda[(o)] | (bda[(o) + 1] << 8)))
#define SET16(o, x) (bda[(o)] = (uint8_t)(x), bda[(o) + 1] = (uint8_t)((x) >> 8))
        /* (a) */
        fresh(&in, &bus);
        CHECK(W16(0x80) == 0x001E && W16(0x82) == 0x003E,
              "bounds: reset writes POST's 0040:0080 = 001Eh, 0040:0082 = 003Eh (nothing wrote them)");
        SET16(0x82, 0x0026);                                    /* a 4-slot ring          */
        CHECK(vdd_input_push(&in, 0x1E61) && vdd_input_push(&in, 0x3062) && vdd_input_push(&in, 0x2E63),
              "bounds: a 4-slot ring (1E..26) takes three keys");
        CHECK(vdd_input_push(&in, 0x2064) == 0, "bounds: ...and refuses the fourth (full = 3, not 15)");
        CHECK(W16(BDA_KB_TAIL) == 0x0024, "bounds: tail at 0024h");
        CHECK(vdd_input_pop(&in, &k) && k == 0x1E61 && vdd_input_pop(&in, &k) && k == 0x3062,
              "bounds: FIFO order holds");
        CHECK(vdd_input_push(&in, 0x2064) && W16(BDA_KB_TAIL) == 0x001E,
              "bounds: the tail WRAPS at the relocated end 0026h, back to 001Eh (was: ran on to 003Eh)");
        CHECK(vdd_input_pop(&in, &k) && k == 0x2E63 && vdd_input_pop(&in, &k) && k == 0x2064
              && W16(BDA_KB_HEAD) == 0x001E, "bounds: the head wraps the same way");
        CHECK(vdd_input_pop(&in, &k) == 0, "bounds: empty again");
        /* moved OUT of the BDA: a 64-slot ring at 0040:0200 */
        SET16(0x80, 0x0200); SET16(0x82, 0x0280);
        SET16(BDA_KB_HEAD, 0x0200); SET16(BDA_KB_TAIL, 0x0200);
        ok = 1;
        for (i = 0; i < 63; ++i) ok &= vdd_input_push(&in, (uint16_t)(0x1E00 + i));
        CHECK(ok && vdd_input_push(&in, 0x1E61) == 0,
              "bounds: an enlarged ring at 0200h..0280h holds 63 keys, then is full");
        CHECK(W16(0x0200) == 0x1E00 && W16(0x027C) == 0x1E3E,
              "bounds: the keys are stored IN the relocated buffer");
        ok = 1;
        for (i = 0; i < 63; ++i) ok &= (vdd_input_pop(&in, &k) && k == (uint16_t)(0x1E00 + i));
        CHECK(ok && vdd_input_pop(&in, &k) == 0, "bounds: ...and come back out in order");
        /* a pair that cannot describe a ring falls back to POST's */
        SET16(0x80, 0x0021); SET16(0x82, 0x0010);               /* odd and inverted       */
        CHECK(vdd_input_push(&in, 0x1E61) && W16(BDA_KB_HEAD) == 0x001E && W16(BDA_KB_TAIL) == 0x0020,
              "bounds: an odd / inverted pair is ignored -> the 001E..003E ring (head/tail reset into it)");
        CHECK(vdd_input_pop(&in, &k) && k == 0x1E61, "bounds: ...and the key is readable there");
        SET16(0x80, 0x001E); SET16(0x82, 0x0022);               /* 2 slots: one key        */
        memset(&r, 0, sizeof r); VddSetAh(&r, 0x05); r.Ecx = 0x1E61; VddBusDeliverInterrupt(&bus, 0x16, &r);
        v16 = (uint16_t)VddGetAl(&r);
        memset(&r, 0, sizeof r); VddSetAh(&r, 0x05); r.Ecx = 0x3062; VddBusDeliverInterrupt(&bus, 0x16, &r);
        CHECK(v16 == 0 && VddGetAl(&r) == 1, "bounds: INT 16h AH=05h into a 2-slot ring: stored, then AL=1 full");
        memset(&r, 0, sizeof r); VddSetAh(&r, 0x00); VddBusDeliverInterrupt(&bus, 0x16, &r);
        CHECK(r.ZeroFlag == 0 && VddGetAx(&r) == 0x1E61, "bounds: INT 16h AH=00h reads it back");
        SET16(0x82, 0x003E);

        /* (b) */
        fresh(&in, &bus);
        KEY(0x38); KEY(0x52); KEY(0xD2); KEY(0x4D); KEY(0xCD); KEY(0x4C); KEY(0xCC);   /* Alt 0 6 5 */
        CHECK(bda[BDA_KB_ALTNUM] == 65, "altnum: Alt + keypad 0,6,5 accumulates 65 in 0040:0019");
        NOKEY("altnum: ...and stores nothing while Alt is held");
        KEY(0xB8);
        EXPECT(0x0041, "altnum: Alt released -> 0041h ('A', AH=0) stored");
        CHECK(bda[BDA_KB_ALTNUM] == 0, "altnum: ...and the accumulator is cleared");
        KEY(0x38); KEY(0x50); KEY(0xD0); KEY(0x4C); KEY(0xCC); KEY(0x52); KEY(0xD2); KEY(0xB8);   /* 2 5 0 */
        EXPECT(0x00FA, "altnum: Alt+2+5+0 -> 00FAh");
        KEY(0x38); KEY(0x51); KEY(0xD1); KEY(0x52); KEY(0xD2); KEY(0x52); KEY(0xD2); KEY(0xB8);   /* 3 0 0 */
        EXPECT(0x002C, "altnum: Alt+3+0+0 wraps as a byte: 300 mod 256 = 44 -> 002Ch");
        KEY(0x38); KEY(0xB8);
        NOKEY("altnum: Alt alone stores nothing (accumulator 0)");
        KEY(0x38); KEY(0x4F); KEY(0xCF); KEY(0x21); KEY(0xA1); KEY(0xB8);   /* Alt 1, then F */
        EXPECT(0x2100, "altnum: another key under Alt is its Alt code (2100h)...");
        NOKEY("altnum: ...and throws the accumulated digit away");
        KEY(0x38); KEY(0xE0); KEY(0x4F); KEY(0xE0); KEY(0xCF); KEY(0xB8);  /* Alt + grey End */
        EXPECT(0x9F00, "altnum: a GREY key is not a keypad digit (Alt+grey End = 9F00h)");
        NOKEY("altnum: ...and accumulates nothing");
        KEY(0x45); KEY(0xC5);                                   /* NumLock on: no effect  */
        KEY(0xE0); KEY(0x38); KEY(0x48); KEY(0xC8); KEY(0xE0); KEY(0xB8);  /* right Alt 8 */
        EXPECT(0x0008, "altnum: right Alt works too, whatever NumLock says (Alt+8 -> 0008h)");
        KEY(0x38); KEY(0xE0); KEY(0x38); KEY(0x49); KEY(0xC9); KEY(0xB8);  /* both Alts, 9 */
        NOKEY("altnum: with both Alts down, releasing ONE stores nothing yet");
        KEY(0xE0); KEY(0xB8);
        EXPECT(0x0009, "altnum: ...the last Alt up stores it (0009h)");

        /* (c) */
        fresh(&in, &bus);
        CHECK(vdd_input_bios_fetch(&in) == -1, "split: fetch with nothing presented -> -1");
        vdd_input_push_scancode(&in, 0x1E);
        sc = vdd_input_bios_fetch(&in);
        CHECK(sc == 0x1E && vdd_input_sc_queued(&in) == 0, "split: fetch takes the byte out of the controller");
        NOKEY("split: ...and translates nothing on its own");
        act = vdd_input_bios_translate(&in, 0x30);              /* the hook changed AL */
        CHECK(act == KB_ACT_NONE, "split: translate returns its action");
        EXPECT(0x3062, "split: a hook that changed AL 1Eh -> 30h stores 'b', not 'a'");
        vdd_input_push_scancode(&in, 0x1E);
        v = 0; VddBusIo(&bus, 0x60, 1, 1, &v);                 /* a guest hook read 60h */
        CHECK(vdd_input_bios_fetch(&in) == 0x1E, "split: fetch after a hook's port read gets the SAME byte (sc_bios_owed)");
        CHECK(vdd_input_bios_fetch(&in) == -1, "split: ...once");

        /* (d) */
        fresh(&in, &bus);
        VddBusSetSinks(&bus, count_irq, &g_irq1_n, 0, 0); g_irq1_n = 0;
        v = 0xD2; VddBusIo(&bus, 0x64, 1, 0, &v);
        CHECK(g_irq1_n == 0 && vdd_input_sc_queued(&in) == 0, "kbc D2h: the command alone presents nothing");
        v = 0x1E; VddBusIo(&bus, 0x60, 1, 0, &v);
        CHECK(g_irq1_n == 1 && vdd_input_sc_queued(&in) == 1, "kbc D2h: the next 60h write is presented, with IRQ1");
        v = 0; VddBusIo(&bus, 0x64, 1, 1, &v);
        CHECK(v & 1, "kbc D2h: OBF set");
        CHECK(vdd_input_bios_consume(&in) == KB_ACT_NONE, "kbc D2h: the BIOS takes it as a keystroke...");
        EXPECT(0x1E61, "kbc D2h: ...'a' in the ring");
        v = 0x3A; VddBusIo(&bus, 0x60, 1, 0, &v);              /* no D2h: a KEYBOARD cmd */
        CHECK(vdd_input_sc_queued(&in) == 0, "kbc D2h: one byte per command (the next 60h write is not a keystroke)");
        VddBusSetSinks(&bus, 0, 0, 0, 0);

        /* (e) */
        n = vdd_input_host_key_bytes(0x45, 0, 0, b, &norep);
        CHECK(n == 6 && norep && b[0] == 0xE1 && b[1] == 0x1D && b[2] == 0x45 && b[3] == 0xE1
              && b[4] == 0x9D && b[5] == 0xC5, "host: Pause (45h, not extended) -> E1 1D 45 E1 9D C5 on the press");
        n = vdd_input_host_key_bytes(0x45, 0, 1, b, &norep);
        CHECK(n == 0 && norep, "host: ...nothing on the release, never repeated");
        n = vdd_input_host_key_bytes(0x45, 1, 0, b, &norep);
        CHECK(n == 2 && !norep && b[0] == 0xE0 && b[1] == 0x45, "host: NumLock (45h extended) is unchanged: E0 45");
        n = vdd_input_host_key_bytes(0x46, 1, 0, b, &norep);
        CHECK(n == 4 && norep && b[0] == 0xE0 && b[1] == 0x46 && b[2] == 0xE0 && b[3] == 0xC6,
              "host: Ctrl+Break (46h extended) -> E0 46 E0 C6 on the press");
        n = vdd_input_host_key_bytes(0x46, 1, 1, b, &norep);
        CHECK(n == 0 && norep, "host: ...nothing on the release");
        n = vdd_input_host_key_bytes(0x46, 0, 0, b, &norep);
        CHECK(n == 1 && !norep && b[0] == 0x46, "host: Scroll Lock (46h) is unchanged");
        n = vdd_input_host_key_bytes(0x48, 1, 1, b, &norep);
        CHECK(n == 2 && b[0] == 0xE0 && b[1] == 0xC8, "host: an ordinary grey key: E0 C8 on the release");
        /* ...and what our BIOS makes of them */
        fresh(&in, &bus);
        n = vdd_input_host_key_bytes(0x45, 0, 0, b, &norep);
        act = KB_ACT_NONE;
        for (i = 0; i < n; ++i) { int a = KEY(b[i]); if (a != KB_ACT_NONE) act = a; }
        CHECK(act == KB_ACT_PAUSE && (bda[BDA_KB_FLAGS2] & 0x08) && !(bda[BDA_KB_FLAGS] & 0x20),
              "host+bios: the Pause key now PAUSES (0018 bit 3) instead of toggling NumLock");
        vdd_input_pause_cancel(&in);
        KEY(0x1D);                                              /* Ctrl down           */
        n = vdd_input_host_key_bytes(0x46, 1, 0, b, &norep);
        act = KB_ACT_NONE;
        for (i = 0; i < n; ++i) { int a = KEY(b[i]); if (a != KB_ACT_NONE) act = a; }
        KEY(0x9D);
        CHECK(act == KB_ACT_BREAK && (bda[0x71] & 0x80), "host+bios: Ctrl+Break -> INT 1Bh action, 0071h bit 7");
        EXPECT(0x0000, "host+bios: ...0000h in the ring");
#undef KEY
#undef EXPECT
#undef NOKEY
#undef W16
#undef SET16
    }

    printf("\n%d checks, %d failed\n", total, fails);
    return fails ? 1 : 0;
}
