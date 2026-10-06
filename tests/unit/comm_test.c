/* comm_test.c -- off-VM unit battery for the 8250/16550A serial VDD (vdd_comm.c).
 *
 * The battery is built around LOCAL LOOPBACK, because that is the one part of a
 * UART whose correctness can be established with no peer, no cable and no host
 * device -- and it is what every serial driver, and MSD, uses to decide the port
 * exists. If loopback is right, a driver will believe the port; if it is wrong,
 * the port echoes bytes and still fails every detection routine ever written.
 *
 * What is checked, and why each one is a bug somebody has actually shipped:
 *   - the divisor latch is reachable only with DLAB set, and the SAME two
 *     addresses are RBR/IER without it (a driver that leaves DLAB set writes its
 *     first data byte into the baud divisor)
 *   - loopback maps DTR->DSR, RTS->CTS, OUT1->RI, OUT2->DCD -- the pairing, not
 *     just "some bits come back"
 *   - MSR delta bits latch until MSR is READ, and TERI is a trailing edge
 *   - IIR is a PRIORITY ENCODER with bit 0 INVERTED, and reading it clears THRE
 *   - reading LSR clears the error bits but NOT data-ready
 *   - with nothing attached the modem lines are LOW; inventing DSR+CTS is the
 *     "runs but lies" class
 *   - INT 14h and the port registers describe ONE part: a byte sent through the
 *     BIOS in loopback is readable from RBR, and vice versa
 *   - MCR bit 3 (OUT2) gates the IRQ LINE, not the part: with it clear nothing
 *     reaches the PIC but IIR still names the source, and setting it onto a
 *     pending source raises at once (GH #181)
 *   - COM3 (3E8h) and COM4 (2E8h) are four slots of one device, each with its
 *     own registers, on the line it shares with COM1/COM2 (GH #181)
 */
#include <stdio.h>
#include <string.h>
#include "vdd_comm.h"

static int total = 0, fails = 0;
#define CHECK(c,m) do{ total++; if(c){printf("  PASS  %s\n",(m));} \
    else{printf("  FAIL  %s\n",(m)); fails++;} }while(0)

static VDD_BUS    bus;
static COMM_STATE com;

#define CAP 64
static uint8_t g_tx[CAP];
static int     g_ntx;
static void sink(void *ctx, int port, uint8_t b)
{ (void)ctx; (void)port; if (g_ntx < CAP) g_tx[g_ntx++] = b; }

/* Every IRQ the device raised, by line. The device has no "lower" -- the PIC
   sees an edge per raise -- so a COUNT is the whole observable. */
static int g_irq[16];
static void irqsink(void *ctx, uint8_t irq)
{ (void)ctx; if (irq < 16) ++g_irq[irq]; }

static uint8_t g_lpt_b[CAP];
static int     g_nlpt;
static void lptsink(void *ctx, int port, uint8_t b)
{ (void)ctx; (void)port; if (g_nlpt < CAP) g_lpt_b[g_nlpt++] = b; }

#define BASE 0x3F8
static void wr(uint16_t p, uint8_t v){ uint32_t x=v; VddBusIo(&bus,p,1,0,&x); }
static uint8_t rd(uint16_t p){ uint32_t x=0; VddBusIo(&bus,p,1,1,&x); return (uint8_t)x; }
#define wr8(p,v) wr((p),(v))

static void int14(uint8_t ah, uint8_t al, uint16_t dx, NTVDD_REGISTERS *r)
{
    memset(r, 0, sizeof *r);
    r->Eax = (uint32_t)(ah << 8) | al;
    r->Edx = dx;
    VddBusDeliverInterrupt(&bus, 0x14, r);
}

int main(void)
{
    NTVDD_REGISTERS r;
    printf("== GH #9: 8250/16550A serial port battery ==\n");

    memset(&com, 0, sizeof com);
    com.Ports[0].BasePort = BASE;   com.Ports[0].Irq = 4; com.Ports[0].IsFitted = 1;
    com.Ports[1].BasePort = 0x2F8;  com.Ports[1].Irq = 3; com.Ports[1].IsFitted = 1;
    com.Ports[2].BasePort = 0x3E8;  com.Ports[2].Irq = 4; com.Ports[2].IsFitted = 1;   /* COM3 */
    com.Ports[3].BasePort = 0x2E8;  com.Ports[3].Irq = 3; com.Ports[3].IsFitted = 1;   /* COM4 */
    com.Printers[0].BasePort = 0x378;  com.Printers[0].IsFitted = 1;
    com.Sink = sink; com.SinkContext = 0;
    com.PrinterSink = lptsink; com.PrinterSinkContext = 0;
    VddBusInitialize(&bus, 0);
    { NTVDD_DEVICE d = VddCommDevice(&com); VddBusAdd(&bus, &d); }
    VddBusSetSinks(&bus, irqsink, 0, 0, 0);

    /* ---- reset state ------------------------------------------------------ */
    CHECK(rd(BASE + COMM_LSR) == (COMM_LSR_THR_EMPTY | COMM_LSR_TRANSMITTER_EMPTY),
          "after reset LSR = THRE|TEMT (an empty transmitter, so a driver that "
          "polls before its first write does not hang)");
    CHECK((rd(BASE + COMM_IIR) & 0x01) == 0x01,
          "IIR bit 0 SET means no interrupt pending (the bit is inverted)");
    CHECK(rd(BASE + COMM_MSR) == 0x00,
          "with nothing attached the modem lines are LOW, not a convenient DSR+CTS");

    /* ---- the scratch register is how drivers probe for the part ----------- */
    wr(BASE + COMM_SCR, 0x5A);
    CHECK(rd(BASE + COMM_SCR) == 0x5A, "scratch register holds a byte (the probe)");
    wr(BASE + COMM_SCR, 0xA5);
    CHECK(rd(BASE + COMM_SCR) == 0xA5, "scratch register holds a second, different byte");

    /* ---- DLAB ------------------------------------------------------------- */
    wr(BASE + COMM_LCR, 0x83);                 /* DLAB + 8N1                    */
    wr(BASE + COMM_RBR, 0x0C);                 /* divisor low  = 12 (9600)      */
    wr(BASE + COMM_IER, 0x00);                 /* divisor high                  */
    CHECK(rd(BASE + COMM_RBR) == 0x0C, "with DLAB set, base+0 reads the divisor LOW byte");
    CHECK(rd(BASE + COMM_IER) == 0x00, "with DLAB set, base+1 reads the divisor HIGH byte");
    wr(BASE + COMM_LCR, 0x03);                 /* DLAB off, 8N1                 */
    CHECK(rd(BASE + COMM_LCR) == 0x03, "LCR reads back what was written");
    wr(BASE + COMM_IER, 0x0F);
    CHECK(rd(BASE + COMM_IER) == 0x0F, "with DLAB clear, base+1 is the interrupt enable");
    wr(BASE + COMM_IER, 0x00);

    /* ---- transmit reaches the sink when loopback is OFF -------------------- */
    g_ntx = 0;
    wr(BASE + COMM_RBR, 'A');
    CHECK(g_ntx == 1 && g_tx[0] == 'A', "out of loopback a transmitted byte reaches the host sink");
    CHECK((rd(BASE + COMM_LSR) & COMM_LSR_DATA_READY) == 0,
          "...and does NOT appear in the receiver (the pins are connected outward)");

    /* ---- LOCAL LOOPBACK: the self-test every driver runs ------------------- */
    wr(BASE + COMM_MCR, COMM_MCR_LOOP);
    g_ntx = 0;
    wr(BASE + COMM_RBR, 0x5A);
    CHECK(g_ntx == 0, "in loopback the byte does NOT go to the host");
    CHECK((rd(BASE + COMM_LSR) & COMM_LSR_DATA_READY) == COMM_LSR_DATA_READY, "in loopback the byte sets DATA READY");
    CHECK(rd(BASE + COMM_RBR) == 0x5A, "...and reads back from RBR unchanged");
    CHECK((rd(BASE + COMM_LSR) & COMM_LSR_DATA_READY) == 0, "reading RBR clears DATA READY");

    /* the four control->status pairings, checked ONE AT A TIME so a swapped
       pair cannot hide behind another bit being right */
    wr(BASE + COMM_MCR, (uint8_t)(COMM_MCR_LOOP | COMM_MCR_DTR));
    CHECK((rd(BASE + COMM_MSR) & 0xF0) == COMM_MSR_DSR,  "loopback: DTR -> DSR, alone");
    wr(BASE + COMM_MCR, (uint8_t)(COMM_MCR_LOOP | COMM_MCR_RTS));
    CHECK((rd(BASE + COMM_MSR) & 0xF0) == COMM_MSR_CTS,  "loopback: RTS -> CTS, alone");
    wr(BASE + COMM_MCR, (uint8_t)(COMM_MCR_LOOP | COMM_MCR_OUT1));
    CHECK((rd(BASE + COMM_MSR) & 0xF0) == COMM_MSR_RI,   "loopback: OUT1 -> RI, alone");
    wr(BASE + COMM_MCR, (uint8_t)(COMM_MCR_LOOP | COMM_MCR_OUT2));
    CHECK((rd(BASE + COMM_MSR) & 0xF0) == COMM_MSR_DCD,  "loopback: OUT2 -> DCD, alone");

    /* ---- MSR delta bits latch until the register is READ ------------------- */
    wr(BASE + COMM_MCR, COMM_MCR_LOOP);                 /* drop every line           */
    rd(BASE + COMM_MSR);                           /* acknowledge, deltas clear */
    wr(BASE + COMM_MCR, (uint8_t)(COMM_MCR_LOOP | COMM_MCR_DTR | COMM_MCR_RTS));
    { uint8_t m = rd(BASE + COMM_MSR);
      CHECK((m & COMM_MSR_DELTA_DSR) == COMM_MSR_DELTA_DSR,        "asserting DTR sets the DDSR delta bit");
      CHECK((m & COMM_MSR_DELTA_CTS) == COMM_MSR_DELTA_CTS,        "asserting RTS sets the DCTS delta bit");
      CHECK((rd(BASE + COMM_MSR) & 0x0F) == 0, "reading MSR is the acknowledgement: deltas clear"); }

    /* TERI is a TRAILING edge -- set on 1->0, not on 0->1 */
    wr(BASE + COMM_MCR, (uint8_t)(COMM_MCR_LOOP | COMM_MCR_OUT1));   /* RI 0 -> 1         */
    CHECK((rd(BASE + COMM_MSR) & COMM_MSR_TRAILING_RI) == 0, "RI going HIGH does NOT set TERI");
    wr(BASE + COMM_MCR, COMM_MCR_LOOP);                          /* RI 1 -> 0        */
    CHECK((rd(BASE + COMM_MSR) & COMM_MSR_TRAILING_RI) == COMM_MSR_TRAILING_RI, "RI going LOW DOES set TERI");

    /* ---- IIR: priority, and reading it clears THRE ------------------------- */
    wr(BASE + COMM_MCR, COMM_MCR_LOOP);
    wr(BASE + COMM_IER, 0x00);
    CHECK((rd(BASE + COMM_IIR) & 0x01) == 1, "with every source masked, IIR reports nothing pending");
    wr(BASE + COMM_IER, COMM_IER_THR_EMPTY);
    wr(BASE + COMM_RBR, 'x');                      /* transmit -> THRE owed     */
    { uint8_t first = rd(BASE + COMM_IIR);
      /* RX data available outranks THRE, and the loopback byte is now waiting,
         so with BOTH enabled the answer must be 0x04. */
      wr(BASE + COMM_IER, (uint8_t)(COMM_IER_THR_EMPTY | COMM_IER_RECEIVED_DATA));
      CHECK(first == 0x02, "IIR reports THRE (0x02) when only THRE is enabled");
      CHECK(rd(BASE + COMM_IIR) == 0x04,
            "received-data-available (0x04) OUTRANKS THRE -- IIR is a priority encoder"); }
    rd(BASE + COMM_RBR);                           /* drain the byte            */
    CHECK((rd(BASE + COMM_IIR) & 0x01) == 1,
          "reading IIR cleared THRE, so once the byte is drained nothing is owed "
          "(an IIR that keeps reporting THRE is an interrupt storm)");

    /* ---- LSR clears errors on read, but not data-ready --------------------- */
    wr(BASE + COMM_IER, 0x00);
    wr(BASE + COMM_RBR, 'q');                      /* loopback: DR set          */
    com.Ports[0].Lsr |= COMM_LSR_OVERRUN;                        /* pretend an overrun        */
    { uint8_t s = rd(BASE + COMM_LSR);
      CHECK((s & COMM_LSR_OVERRUN) == COMM_LSR_OVERRUN, "LSR reports the overrun");
      CHECK((rd(BASE + COMM_LSR) & COMM_LSR_OVERRUN) == 0, "reading LSR clears the ERROR bits");
      CHECK((rd(BASE + COMM_LSR) & COMM_LSR_DATA_READY) == COMM_LSR_DATA_READY,
            "...but NOT data-ready, which is not an error and is still true"); }
    rd(BASE + COMM_RBR);

    /* ---- the FIFO control register resets the receiver --------------------- */
    wr(BASE + COMM_RBR, '1'); wr(BASE + COMM_RBR, '2');
    CHECK((rd(BASE + COMM_LSR) & COMM_LSR_DATA_READY) == COMM_LSR_DATA_READY, "two loopback bytes are waiting");
    wr(BASE + COMM_IIR, 0x02);                     /* FCR: clear receive FIFO   */
    CHECK((rd(BASE + COMM_LSR) & COMM_LSR_DATA_READY) == 0, "FCR bit 1 clears the receive FIFO");
    CHECK((rd(BASE + COMM_IIR) & 0xC0) == 0,
          "IIR does not claim an enabled FIFO until FCR bit 0 is set");
    wr(BASE + COMM_IIR, 0x01);
    CHECK((rd(BASE + COMM_IIR) & 0xC0) == 0xC0, "with FCR bit 0 set, IIR reports a 16550 FIFO");
    wr(BASE + COMM_IIR, 0x00);

    /* ---- host -> guest ----------------------------------------------------- */
    wr(BASE + COMM_MCR, 0x00);                     /* out of loopback           */
    CHECK(VddCommReceive(&com, 0, 'H') == 0, "the host can push a received byte");
    CHECK((rd(BASE + COMM_LSR) & COMM_LSR_DATA_READY) == COMM_LSR_DATA_READY, "...which sets data-ready");
    CHECK(rd(BASE + COMM_RBR) == 'H', "...and reads out of RBR");

    /* ---- OUT2 GATES THE LINE, NOT THE PART (GH #181) ------------------------
       All out of loopback, deliberately: whether loopback closes the gate is
       the one question here the datasheets and the Super I/O parts answer
       differently, and it is not pinned until an oracle is asked. */
    wr(BASE + COMM_MCR, 0x00);                     /* OUT2 clear                */
    wr(BASE + COMM_IER, COMM_IER_RECEIVED_DATA);
    memset(g_irq, 0, sizeof g_irq);
    VddCommReceive(&com, 0, 'i');
    CHECK(g_irq[4] == 0,
          "OUT2 clear: a received byte with RDA enabled raises NO IRQ (the buffer "
          "between the part and the PIC is off)");
    CHECK(rd(BASE + COMM_IIR) == 0x04,
          "...but IIR still reports received-data-available (0x04) -- the gate is "
          "outside the chip, so a polling driver reads the true source");
    wr(BASE + COMM_MCR, COMM_MCR_OUT2);
    CHECK(g_irq[4] == 1,
          "setting OUT2 with that interrupt still pending raises IRQ4 at once "
          "(the late-OUT2 driver order still gets its interrupt)");
    CHECK(rd(BASE + COMM_RBR) == 'i', "...and the byte that was waiting is the one read");
    CHECK((rd(BASE + COMM_IIR) & 0x01) == 1, "...after which nothing is owed");
    VddCommReceive(&com, 0, 'j');
    CHECK(g_irq[4] == 2, "OUT2 set: a received byte with RDA enabled raises IRQ4");
    CHECK(g_irq[3] == 0, "...on COM1's line only, not COM2's");
    rd(BASE + COMM_RBR);
    wr(BASE + COMM_MCR, 0x00);                     /* close the gate again      */
    wr(BASE + COMM_IER, COMM_IER_THR_EMPTY);
    memset(g_irq, 0, sizeof g_irq);
    g_ntx = 0;
    wr(BASE + COMM_RBR, 't');                      /* to the sink: THRE owed    */
    CHECK(g_ntx == 1 && g_irq[4] == 0,
          "OUT2 clear: a transmit with THRE enabled raises NO IRQ either");
    CHECK(rd(BASE + COMM_IIR) == 0x02,
          "...while IIR still reports THRE (0x02), and that read acknowledges it");
    wr(BASE + COMM_MCR, COMM_MCR_OUT2);
    CHECK(g_irq[4] == 0,
          "opening the gate after THRE was acknowledged raises nothing -- nothing is owed");
    wr(BASE + COMM_IER, 0x00);
    wr(BASE + COMM_MCR, 0x00);

    /* ---- #245 (s90): the 16550 remainder, each line from the PC16550D --------- */
    /* enabling THRE with the holding register already empty IS a THRE interrupt */
    wr(BASE + COMM_MCR, COMM_MCR_OUT2);
    wr(BASE + COMM_IER, 0x00);
    rd(BASE + COMM_IIR);
    memset(g_irq, 0, sizeof g_irq);
    wr(BASE + COMM_IER, COMM_IER_THR_EMPTY);
    CHECK(g_irq[4] == 1, "#245: setting ETBEI with THR empty raises THRE at once (the "
                         "interrupt-driven transmit kick-start)");
    CHECK(rd(BASE + COMM_IIR) == 0x02, "...and IIR names it THRE");
    wr(BASE + COMM_IER, COMM_IER_THR_EMPTY);
    CHECK((rd(BASE + COMM_IIR) & 0x01) == 1,
          "re-writing ETBEI when it is ALREADY set is not a new 0->1 edge: nothing owed");
    /* INT 14h AH=01 owes the same THRE the port write does */
    memset(g_irq, 0, sizeof g_irq);
    int14(0x01, 'b', 0, &r);
    CHECK(g_irq[4] == 1 && rd(BASE + COMM_IIR) == 0x02,
          "#245: a byte sent through INT 14h raises THRE like a THR write");
    wr(BASE + COMM_IER, 0x00);
    /* LOOPBACK CLOSES THE IRQ GATE (output pins forced inactive, OUT2 included) */
    wr(BASE + COMM_MCR, (uint8_t)(COMM_MCR_LOOP | COMM_MCR_OUT2));
    wr(BASE + COMM_IER, COMM_IER_RECEIVED_DATA);
    memset(g_irq, 0, sizeof g_irq);
    wr(BASE + COMM_RBR, 'L');
    CHECK(g_irq[4] == 0, "#245: in loopback a received byte raises NO IRQ even with OUT2 "
                         "set -- the OUT2 PIN is forced inactive, and on a PC it is the gate");
    CHECK(rd(BASE + COMM_IIR) == 0x04, "...while the part still reports RDA in IIR");
    rd(BASE + COMM_RBR);
    /* BREAK in loopback: one zero character, BI set, an RLS interrupt owed */
    wr(BASE + COMM_IER, COMM_IER_LINE_STATUS | COMM_IER_RECEIVED_DATA);
    wr(BASE + COMM_LCR, (uint8_t)(0x03 | COMM_LCR_BREAK));
    CHECK(rd(BASE + COMM_IIR) == 0x06, "#245: SET BREAK in loopback owes a line-status "
                                       "interrupt (IIR 0x06, the highest priority)");
    { uint8_t l = rd(BASE + COMM_LSR);
      CHECK((l & (COMM_LSR_BREAK | COMM_LSR_DATA_READY)) == (COMM_LSR_BREAK | COMM_LSR_DATA_READY), "...LSR reports BREAK and a character");
      CHECK((rd(BASE + COMM_LSR) & COMM_LSR_BREAK) == 0, "...and the LSR read clears BI"); }
    CHECK(rd(BASE + COMM_RBR) == 0x00, "...the character is a single ZERO");
    CHECK((rd(BASE + COMM_LSR) & COMM_LSR_DATA_READY) == 0, "...and only one of them");
    wr(BASE + COMM_LCR, (uint8_t)(0x03 | COMM_LCR_BREAK));
    CHECK((rd(BASE + COMM_LSR) & COMM_LSR_DATA_READY) == 0, "holding break is not a second break");
    wr(BASE + COMM_LCR, 0x03);
    CHECK(com.Ports[0].Breaks == 1, "the break was counted once");
    /* THE RECEIVE FIFO'S TRIGGER LEVEL, out of loopback */
    wr(BASE + COMM_MCR, COMM_MCR_OUT2);
    wr(BASE + COMM_IER, COMM_IER_RECEIVED_DATA);
    wr(BASE + COMM_IIR, 0x41);                     /* FCR: FIFO on, trigger = 4  */
    VddCommReceive(&com, 0, '1'); VddCommReceive(&com, 0, '2');
    CHECK(rd(BASE + COMM_IIR) == 0xCC, "#245: 2 bytes below a trigger of 4 -> CHARACTER "
                                       "TIMEOUT (IIR 0xCC with the FIFO bits)");
    VddCommReceive(&com, 0, '3'); VddCommReceive(&com, 0, '4');
    CHECK(rd(BASE + COMM_IIR) == 0xC4, "...4 bytes reach the trigger -> RDA (0xC4)");
    CHECK(rd(BASE + COMM_RBR) == '1', "...read in order");
    CHECK(rd(BASE + COMM_IIR) == 0xCC, "...3 left, below the trigger again -> timeout");
    rd(BASE + COMM_RBR); rd(BASE + COMM_RBR); rd(BASE + COMM_RBR);
    CHECK((rd(BASE + COMM_IIR) & 0x0F) == 0x01, "...drained: nothing owed");
    wr(BASE + COMM_IIR, 0xC1);                     /* trigger = 14               */
    { int i; for (i = 0; i < 20; ++i) VddCommReceive(&com, 0, (uint8_t)('a' + i)); }
    CHECK(rd(BASE + COMM_IIR) == 0xC4, "20 queued, 16 in the FIFO >= 14 -> RDA");
    CHECK((rd(BASE + COMM_LSR) & COMM_LSR_OVERRUN) == 0,
          "...and the 4 beyond the FIFO wait on the WIRE: no overrun for a burst the host queued");
    wr(BASE + COMM_IIR, 0x07);                     /* clear both FIFOs, FIFO on  */
    CHECK((rd(BASE + COMM_LSR) & COMM_LSR_DATA_READY) == 0, "FCR bit 1 empties the receiver");
    wr(BASE + COMM_IIR, 0x00);                     /* back to 8250 behaviour     */
    VddCommReceive(&com, 0, 'z');
    CHECK(rd(BASE + COMM_IIR) == 0x04, "FIFO off: one byte is RDA, no timeout, no FIFO bits");
    rd(BASE + COMM_RBR);
    wr(BASE + COMM_IER, 0x00);
    wr(BASE + COMM_MCR, 0x00);

    /* ---- INT 14h describes THE SAME PART ----------------------------------- */
    wr(BASE + COMM_MCR, COMM_MCR_LOOP);
    int14(0x01, 'Z', 0, &r);                       /* BIOS send, in loopback    */
    CHECK((rd(BASE + COMM_LSR) & COMM_LSR_DATA_READY) == COMM_LSR_DATA_READY,
          "a byte sent through INT 14h in loopback arrives in the SAME receiver");
    CHECK(rd(BASE + COMM_RBR) == 'Z', "...and reads out of RBR");

    VddCommReceive(&com, 0, 'k');
    int14(0x02, 0, 0, &r);
    CHECK((r.Eax & 0xFF) == 'k', "INT 14h AH=02 returns a byte the HOST pushed");
    CHECK(((r.Eax >> 8) & 0x80) == 0, "...with the TIMEOUT bit CLEAR");
    int14(0x02, 0, 0, &r);
    CHECK((r.Eax & 0xFFFF) == 0x8000,
          "INT 14h AH=02 with nothing waiting reports TIMEOUT (and does not hang the guest)");

    int14(0x00, 0xE3, 0, &r);                      /* 9600 8N1                  */
    wr(BASE + COMM_LCR, 0x83);
    CHECK(rd(BASE + COMM_RBR) == 12,
          "INT 14h AH=00 at 9600 baud leaves divisor 12, readable through DLAB -- "
          "the BIOS and the registers describe one port, not two");
    wr(BASE + COMM_LCR, 0x03);

    int14(0x03, 0, 1, &r);
    CHECK((r.Eax & 0xFFFF) != 0x8000, "COM2 is fitted and answers INT 14h AH=03");
    int14(0x03, 0, 4, &r);
    CHECK((r.Eax & 0xFFFF) == 0x8000, "a port index past COM4 reports TIMEOUT");

    /* ---- COM3 and COM4: two more slots of the same device (GH #181) --------- */
    wr(0x3E8 + COMM_SCR, 0x33);
    wr(0x2E8 + COMM_SCR, 0x44);
    CHECK(rd(0x3E8 + COMM_SCR) == 0x33 && rd(0x2E8 + COMM_SCR) == 0x44,
          "COM3 (3E8h) and COM4 (2E8h) each answer the scratch probe");
    CHECK(rd(BASE + COMM_SCR) != 0x33 && rd(0x2F8 + COMM_SCR) != 0x44,
          "...with their OWN registers, not aliases of COM1/COM2");
    wr(0x3E8 + COMM_MCR, (uint8_t)(COMM_MCR_LOOP | COMM_MCR_DTR | COMM_MCR_RTS));
    wr(0x3E8 + COMM_RBR, 0xC3);
    CHECK(rd(0x3E8 + COMM_RBR) == 0xC3 && (rd(0x3E8 + COMM_MSR) & 0xF0) == (COMM_MSR_DSR | COMM_MSR_CTS),
          "COM3 passes the loopback self-test (byte echo + DTR/RTS -> DSR/CTS)");
    CHECK((rd(BASE + COMM_LSR) & COMM_LSR_DATA_READY) == 0, "...and COM1's receiver saw none of it");
    wr(0x3E8 + COMM_MCR, COMM_MCR_OUT2);
    wr(0x3E8 + COMM_IER, COMM_IER_RECEIVED_DATA);
    wr(0x2E8 + COMM_MCR, COMM_MCR_OUT2);
    wr(0x2E8 + COMM_IER, COMM_IER_RECEIVED_DATA);
    memset(g_irq, 0, sizeof g_irq);
    VddCommReceive(&com, 2, 'c');
    CHECK(g_irq[4] == 1 && g_irq[3] == 0, "COM3 raises IRQ4, the line it shares with COM1");
    VddCommReceive(&com, 3, 'd');
    CHECK(g_irq[3] == 1 && g_irq[4] == 1, "COM4 raises IRQ3, the line it shares with COM2");
    CHECK(rd(0x3E8 + COMM_RBR) == 'c' && rd(0x2E8 + COMM_RBR) == 'd',
          "each byte arrives on the port it was pushed to");
    int14(0x03, 0, 2, &r);
    CHECK((r.Eax & 0xFFFF) != 0x8000, "a fitted COM3 answers INT 14h DX=2");
    int14(0x03, 0, 3, &r);
    CHECK((r.Eax & 0xFFFF) != 0x8000, "a fitted COM4 answers INT 14h DX=3");
    wr(0x3E8 + COMM_IER, 0); wr(0x3E8 + COMM_MCR, 0);
    wr(0x2E8 + COMM_IER, 0); wr(0x2E8 + COMM_MCR, 0);

    /* A slot that is NOT fitted -- the host's default for COM3/COM4 -- must be
       absent on every route at once: no registers, no INT 14h, not counted. */
    { static VDD_BUS b2; static COMM_STATE c2; NTVDD_REGISTERS r2; uint32_t x = 0;
      memset(&c2, 0, sizeof c2);
      c2.Ports[0].BasePort = BASE;  c2.Ports[0].Irq = 4; c2.Ports[0].IsFitted = 1;
      c2.Ports[1].BasePort = 0x2F8; c2.Ports[1].Irq = 3; c2.Ports[1].IsFitted = 1;
      VddBusInitialize(&b2, 0);
      { NTVDD_DEVICE d = VddCommDevice(&c2); VddBusAdd(&b2, &d); }
      CHECK(VddBusIo(&b2, 0x3E8 + COMM_SCR, 1, 1, &x) == 0,
            "an unfitted COM3 leaves 3E8h unclaimed (the guest reads the bus float)");
      CHECK(VddCommIsFitted(&c2, 2) == 0 && VddCommIsFitted(&c2, 3) == 0,
            "...and vdd_comm_fitted says so, which is what INT 11h and the BDA read");
      memset(&r2, 0, sizeof r2); r2.Eax = 0x0300; r2.Edx = 3;
      VddBusDeliverInterrupt(&b2, 0x14, &r2);
      CHECK((r2.Eax & 0xFFFF) == 0x8000, "a port that is NOT fitted reports TIMEOUT"); }

    /* ---- the parallel port: the byte leaves on the STROBE EDGE ------------- */
    g_nlpt = 0;
    CHECK((rd(0x379) & LPT_STATUS_BUSY) == LPT_STATUS_BUSY,
          "LPT status reports NOT BUSY (bit 7 is INVERTED -- a 0 hangs every "
          "printing program on its first byte)");
    CHECK((rd(0x379) & LPT_STATUS_PAPER_OUT) == 0, "LPT reports paper loaded");
    CHECK((rd(0x379) & LPT_STATUS_SELECT) == LPT_STATUS_SELECT, "LPT reports the printer online");
    wr8(0x378, 'P');
    CHECK(rd(0x378) == 'P', "the LPT data latch reads back");
    CHECK(g_nlpt == 0, "writing DATA alone prints NOTHING -- the byte is only latched");
    wr8(0x37A, LPT_CONTROL_STROBE);                 /* 0 -> 1: the latching edge     */
    CHECK(g_nlpt == 1 && g_lpt_b[0] == 'P', "the RISING edge of STROBE prints the latched byte");
    wr8(0x37A, LPT_CONTROL_STROBE);                 /* still high: no new edge       */
    CHECK(g_nlpt == 1, "holding STROBE high does not print it again");
    wr8(0x37A, 0);
    CHECK(g_nlpt == 1, "the falling edge does not print either");
    wr8(0x378, 'Q'); wr8(0x37A, LPT_CONTROL_STROBE);
    CHECK(g_nlpt == 2 && g_lpt_b[1] == 'Q', "the next latch+strobe prints the next byte");

    /* ⚠ In a dialect scripts/offvm.sh parses. This line used to read "N/M checks
         passed", which the runner does not know -- so the whole battery ran on
         its exit status and was counted as 0 checks. */
    printf("\n== %d checks, %d failed\n", total, fails);
    return fails ? 1 : 0;
}
