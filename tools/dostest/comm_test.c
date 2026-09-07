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
 */
#include <stdio.h>
#include <string.h>
#include "vdd_comm.h"

static int total = 0, fails = 0;
#define CHECK(c,m) do{ total++; if(c){printf("  PASS  %s\n",(m));} \
    else{printf("  FAIL  %s\n",(m)); fails++;} }while(0)

static vdd_bus    bus;
static comm_state com;

#define CAP 64
static uint8_t g_tx[CAP];
static int     g_ntx;
static void sink(void *ctx, int port, uint8_t b)
{ (void)ctx; (void)port; if (g_ntx < CAP) g_tx[g_ntx++] = b; }

static uint8_t g_lpt_b[CAP];
static int     g_nlpt;
static void lptsink(void *ctx, int port, uint8_t b)
{ (void)ctx; (void)port; if (g_nlpt < CAP) g_lpt_b[g_nlpt++] = b; }

#define BASE 0x3F8
static void wr(uint16_t p, uint8_t v){ uint32_t x=v; vdd_bus_io(&bus,p,1,0,&x); }
static uint8_t rd(uint16_t p){ uint32_t x=0; vdd_bus_io(&bus,p,1,1,&x); return (uint8_t)x; }
#define wr8(p,v) wr((p),(v))

static void int14(uint8_t ah, uint8_t al, uint16_t dx, ntvdd_regs *r)
{
    memset(r, 0, sizeof *r);
    r->eax = (uint32_t)(ah << 8) | al;
    r->edx = dx;
    vdd_bus_deliver_int(&bus, 0x14, r);
}

int main(void)
{
    ntvdd_regs r;
    printf("== GH #9: 8250/16550A serial port battery ==\n");

    memset(&com, 0, sizeof com);
    com.p[0].base = BASE;   com.p[0].irq = 4; com.p[0].fitted = 1;
    com.p[1].base = 0x2F8;  com.p[1].irq = 3; com.p[1].fitted = 1;
    com.l[0].base = 0x378;  com.l[0].fitted = 1;
    com.sink = sink; com.sink_ctx = 0;
    com.lpt_sink = lptsink; com.lpt_sink_ctx = 0;
    vdd_bus_init(&bus, 0);
    { ntvdd d = vdd_comm_device(&com); vdd_bus_add(&bus, &d); }

    /* ---- reset state ------------------------------------------------------ */
    CHECK(rd(BASE + COMM_LSR) == (LSR_THRE | LSR_TEMT),
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
    CHECK((rd(BASE + COMM_LSR) & LSR_DR) == 0,
          "...and does NOT appear in the receiver (the pins are connected outward)");

    /* ---- LOCAL LOOPBACK: the self-test every driver runs ------------------- */
    wr(BASE + COMM_MCR, MCR_LOOP);
    g_ntx = 0;
    wr(BASE + COMM_RBR, 0x5A);
    CHECK(g_ntx == 0, "in loopback the byte does NOT go to the host");
    CHECK((rd(BASE + COMM_LSR) & LSR_DR) == LSR_DR, "in loopback the byte sets DATA READY");
    CHECK(rd(BASE + COMM_RBR) == 0x5A, "...and reads back from RBR unchanged");
    CHECK((rd(BASE + COMM_LSR) & LSR_DR) == 0, "reading RBR clears DATA READY");

    /* the four control->status pairings, checked ONE AT A TIME so a swapped
       pair cannot hide behind another bit being right */
    wr(BASE + COMM_MCR, (uint8_t)(MCR_LOOP | MCR_DTR));
    CHECK((rd(BASE + COMM_MSR) & 0xF0) == MSR_DSR,  "loopback: DTR -> DSR, alone");
    wr(BASE + COMM_MCR, (uint8_t)(MCR_LOOP | MCR_RTS));
    CHECK((rd(BASE + COMM_MSR) & 0xF0) == MSR_CTS,  "loopback: RTS -> CTS, alone");
    wr(BASE + COMM_MCR, (uint8_t)(MCR_LOOP | MCR_OUT1));
    CHECK((rd(BASE + COMM_MSR) & 0xF0) == MSR_RI,   "loopback: OUT1 -> RI, alone");
    wr(BASE + COMM_MCR, (uint8_t)(MCR_LOOP | MCR_OUT2));
    CHECK((rd(BASE + COMM_MSR) & 0xF0) == MSR_DCD,  "loopback: OUT2 -> DCD, alone");

    /* ---- MSR delta bits latch until the register is READ ------------------- */
    wr(BASE + COMM_MCR, MCR_LOOP);                 /* drop every line           */
    rd(BASE + COMM_MSR);                           /* acknowledge, deltas clear */
    wr(BASE + COMM_MCR, (uint8_t)(MCR_LOOP | MCR_DTR | MCR_RTS));
    { uint8_t m = rd(BASE + COMM_MSR);
      CHECK((m & MSR_DDSR) == MSR_DDSR,        "asserting DTR sets the DDSR delta bit");
      CHECK((m & MSR_DCTS) == MSR_DCTS,        "asserting RTS sets the DCTS delta bit");
      CHECK((rd(BASE + COMM_MSR) & 0x0F) == 0, "reading MSR is the acknowledgement: deltas clear"); }

    /* TERI is a TRAILING edge -- set on 1->0, not on 0->1 */
    wr(BASE + COMM_MCR, (uint8_t)(MCR_LOOP | MCR_OUT1));   /* RI 0 -> 1         */
    CHECK((rd(BASE + COMM_MSR) & MSR_TERI) == 0, "RI going HIGH does NOT set TERI");
    wr(BASE + COMM_MCR, MCR_LOOP);                          /* RI 1 -> 0        */
    CHECK((rd(BASE + COMM_MSR) & MSR_TERI) == MSR_TERI, "RI going LOW DOES set TERI");

    /* ---- IIR: priority, and reading it clears THRE ------------------------- */
    wr(BASE + COMM_MCR, MCR_LOOP);
    wr(BASE + COMM_IER, 0x00);
    CHECK((rd(BASE + COMM_IIR) & 0x01) == 1, "with every source masked, IIR reports nothing pending");
    wr(BASE + COMM_IER, IER_THRE);
    wr(BASE + COMM_RBR, 'x');                      /* transmit -> THRE owed     */
    { uint8_t first = rd(BASE + COMM_IIR);
      /* RX data available outranks THRE, and the loopback byte is now waiting,
         so with BOTH enabled the answer must be 0x04. */
      wr(BASE + COMM_IER, (uint8_t)(IER_THRE | IER_RDA));
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
    com.p[0].lsr |= LSR_OE;                        /* pretend an overrun        */
    { uint8_t s = rd(BASE + COMM_LSR);
      CHECK((s & LSR_OE) == LSR_OE, "LSR reports the overrun");
      CHECK((rd(BASE + COMM_LSR) & LSR_OE) == 0, "reading LSR clears the ERROR bits");
      CHECK((rd(BASE + COMM_LSR) & LSR_DR) == LSR_DR,
            "...but NOT data-ready, which is not an error and is still true"); }
    rd(BASE + COMM_RBR);

    /* ---- the FIFO control register resets the receiver --------------------- */
    wr(BASE + COMM_RBR, '1'); wr(BASE + COMM_RBR, '2');
    CHECK((rd(BASE + COMM_LSR) & LSR_DR) == LSR_DR, "two loopback bytes are waiting");
    wr(BASE + COMM_IIR, 0x02);                     /* FCR: clear receive FIFO   */
    CHECK((rd(BASE + COMM_LSR) & LSR_DR) == 0, "FCR bit 1 clears the receive FIFO");
    CHECK((rd(BASE + COMM_IIR) & 0xC0) == 0,
          "IIR does not claim an enabled FIFO until FCR bit 0 is set");
    wr(BASE + COMM_IIR, 0x01);
    CHECK((rd(BASE + COMM_IIR) & 0xC0) == 0xC0, "with FCR bit 0 set, IIR reports a 16550 FIFO");
    wr(BASE + COMM_IIR, 0x00);

    /* ---- host -> guest ----------------------------------------------------- */
    wr(BASE + COMM_MCR, 0x00);                     /* out of loopback           */
    CHECK(vdd_comm_rx(&com, 0, 'H') == 0, "the host can push a received byte");
    CHECK((rd(BASE + COMM_LSR) & LSR_DR) == LSR_DR, "...which sets data-ready");
    CHECK(rd(BASE + COMM_RBR) == 'H', "...and reads out of RBR");

    /* ---- INT 14h describes THE SAME PART ----------------------------------- */
    wr(BASE + COMM_MCR, MCR_LOOP);
    int14(0x01, 'Z', 0, &r);                       /* BIOS send, in loopback    */
    CHECK((rd(BASE + COMM_LSR) & LSR_DR) == LSR_DR,
          "a byte sent through INT 14h in loopback arrives in the SAME receiver");
    CHECK(rd(BASE + COMM_RBR) == 'Z', "...and reads out of RBR");

    vdd_comm_rx(&com, 0, 'k');
    int14(0x02, 0, 0, &r);
    CHECK((r.eax & 0xFF) == 'k', "INT 14h AH=02 returns a byte the HOST pushed");
    CHECK(((r.eax >> 8) & 0x80) == 0, "...with the TIMEOUT bit CLEAR");
    int14(0x02, 0, 0, &r);
    CHECK((r.eax & 0xFFFF) == 0x8000,
          "INT 14h AH=02 with nothing waiting reports TIMEOUT (and does not hang the guest)");

    int14(0x00, 0xE3, 0, &r);                      /* 9600 8N1                  */
    wr(BASE + COMM_LCR, 0x83);
    CHECK(rd(BASE + COMM_RBR) == 12,
          "INT 14h AH=00 at 9600 baud leaves divisor 12, readable through DLAB -- "
          "the BIOS and the registers describe one port, not two");
    wr(BASE + COMM_LCR, 0x03);

    int14(0x03, 0, 1, &r);
    CHECK((r.eax & 0xFFFF) != 0x8000, "COM2 is fitted and answers INT 14h AH=03");
    int14(0x03, 0, 3, &r);
    CHECK((r.eax & 0xFFFF) == 0x8000, "a port that is NOT fitted reports TIMEOUT");

    /* ---- the parallel port: the byte leaves on the STROBE EDGE ------------- */
    g_nlpt = 0;
    CHECK((rd(0x379) & LPT_ST_BUSY) == LPT_ST_BUSY,
          "LPT status reports NOT BUSY (bit 7 is INVERTED -- a 0 hangs every "
          "printing program on its first byte)");
    CHECK((rd(0x379) & LPT_ST_PAPER) == 0, "LPT reports paper loaded");
    CHECK((rd(0x379) & LPT_ST_SELECT) == LPT_ST_SELECT, "LPT reports the printer online");
    wr8(0x378, 'P');
    CHECK(rd(0x378) == 'P', "the LPT data latch reads back");
    CHECK(g_nlpt == 0, "writing DATA alone prints NOTHING -- the byte is only latched");
    wr8(0x37A, LPT_CT_STROBE);                 /* 0 -> 1: the latching edge     */
    CHECK(g_nlpt == 1 && g_lpt_b[0] == 'P', "the RISING edge of STROBE prints the latched byte");
    wr8(0x37A, LPT_CT_STROBE);                 /* still high: no new edge       */
    CHECK(g_nlpt == 1, "holding STROBE high does not print it again");
    wr8(0x37A, 0);
    CHECK(g_nlpt == 1, "the falling edge does not print either");
    wr8(0x378, 'Q'); wr8(0x37A, LPT_CT_STROBE);
    CHECK(g_nlpt == 2 && g_lpt_b[1] == 'Q', "the next latch+strobe prints the next byte");

    printf("\n%d/%d checks passed\n", total - fails, total);
    return fails ? 1 : 0;
}
