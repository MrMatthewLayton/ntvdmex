/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Off-VM unit battery for the 8250/16550A serial VDD (vdd_comm.c).
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
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include <stdio.h>
#include <string.h>
#include "vdd_comm.h"

static INT g_Total = 0, g_Failures = 0;
#define CHECK(condition,message) do{ g_Total++; if(condition){printf("  PASS  %s\n",(message));} \
    else{printf("  FAIL  %s\n",(message)); g_Failures++;} }while(0)

static VDD_BUS    g_Bus;
static COMM_STATE g_Comm;

#define CAP     64
static BYTE g_Transmitted[CAP];
static INT     g_TransmittedCount;
static VOID CommTestSink(PVOID context, INT port, BYTE byteValue)
{
    (VOID)context;
    (VOID)port;
    if (g_TransmittedCount < CAP) g_Transmitted[g_TransmittedCount++] = byteValue;
}

/* Every IRQ the device raised, by line. The device has no "lower" -- the PIC
 * sees an edge per raise -- so a COUNT is the whole observable.
 */
static INT g_IrqCounts[16];
static VOID CommTestIrqSink(PVOID context, BYTE irq)
{
    (VOID)context;
    if (irq < 16) ++g_IrqCounts[irq];
}

static BYTE g_PrinterBytes[CAP];
static INT     g_PrinterCount;
static VOID CommTestPrinterSink(PVOID context, INT port, BYTE byteValue)
{
    (VOID)context;
    (VOID)port;
    if (g_PrinterCount < CAP) g_PrinterBytes[g_PrinterCount++] = byteValue;
}

#define BASE    0x3F8
static VOID CommTestWrite(WORD port, BYTE byteValue)
{
    UINT32 value=byteValue;
    VddBusIo(&g_Bus,port,1,0,&value);
}

static BYTE CommTestRead(WORD port)
{
    UINT32 value=0;
    VddBusIo(&g_Bus,port,1,1,&value);
    return (BYTE)value;
}

#define wr8(port,value)     CommTestWrite((port),(value))

static VOID CommTestInt14(BYTE ah, BYTE al, WORD dx, PNTVDD_REGISTERS registers)
{
    memset(registers, 0, sizeof *registers);
    registers->Eax = (UINT32)(ah << 8) | al;
    registers->Edx = dx;
    VddBusDeliverInterrupt(&g_Bus, 0x14, registers);
}

INT main(VOID)
{
    NTVDD_REGISTERS registers;
    printf("== GH #9: 8250/16550A serial port battery ==\n");

    memset(&g_Comm, 0, sizeof g_Comm);
    g_Comm.Ports[0].BasePort = BASE;   g_Comm.Ports[0].Irq = 4; g_Comm.Ports[0].IsFitted = 1;
    g_Comm.Ports[1].BasePort = 0x2F8;  g_Comm.Ports[1].Irq = 3; g_Comm.Ports[1].IsFitted = 1;
    g_Comm.Ports[2].BasePort = 0x3E8;  g_Comm.Ports[2].Irq = 4; g_Comm.Ports[2].IsFitted = 1;   /* COM3 */
    g_Comm.Ports[3].BasePort = 0x2E8;  g_Comm.Ports[3].Irq = 3; g_Comm.Ports[3].IsFitted = 1;   /* COM4 */
    g_Comm.Printers[0].BasePort = 0x378;  g_Comm.Printers[0].IsFitted = 1;
    g_Comm.Sink = CommTestSink; g_Comm.SinkContext = 0;
    g_Comm.PrinterSink = CommTestPrinterSink; g_Comm.PrinterSinkContext = 0;
    VddBusInitialize(&g_Bus, 0);
    {
        NTVDD_DEVICE device = VddCommDevice(&g_Comm);
        VddBusAdd(&g_Bus, &device);
    }
    VddBusSetSinks(&g_Bus, CommTestIrqSink, 0, 0, 0);

    /* ---- reset state ------------------------------------------------------ */
    CHECK(CommTestRead(BASE + COMM_LSR) == (COMM_LSR_THR_EMPTY | COMM_LSR_TRANSMITTER_EMPTY),
          "after reset LSR = THRE|TEMT (an empty transmitter, so a driver that "
          "polls before its first write does not hang)");
    CHECK((CommTestRead(BASE + COMM_IIR) & 0x01) == 0x01,
          "IIR bit 0 SET means no interrupt pending (the bit is inverted)");
    CHECK(CommTestRead(BASE + COMM_MSR) == 0x00,
          "with nothing attached the modem lines are LOW, not a convenient DSR+CTS");

    /* ---- the scratch register is how drivers probe for the part ----------- */
    CommTestWrite(BASE + COMM_SCR, 0x5A);
    CHECK(CommTestRead(BASE + COMM_SCR) == 0x5A, "scratch register holds a byte (the probe)");
    CommTestWrite(BASE + COMM_SCR, 0xA5);
    CHECK(CommTestRead(BASE + COMM_SCR) == 0xA5, "scratch register holds a second, different byte");

    /* ---- DLAB ------------------------------------------------------------- */
    CommTestWrite(BASE + COMM_LCR, 0x83);                 /* DLAB + 8N1 */
    CommTestWrite(BASE + COMM_RBR, 0x0C);                 /* divisor low  = 12 (9600) */
    CommTestWrite(BASE + COMM_IER, 0x00);                 /* divisor high */
    CHECK(CommTestRead(BASE + COMM_RBR) == 0x0C, "with DLAB set, base+0 reads the divisor LOW byte");
    CHECK(CommTestRead(BASE + COMM_IER) == 0x00, "with DLAB set, base+1 reads the divisor HIGH byte");
    CommTestWrite(BASE + COMM_LCR, 0x03);                 /* DLAB off, 8N1 */
    CHECK(CommTestRead(BASE + COMM_LCR) == 0x03, "LCR reads back what was written");
    CommTestWrite(BASE + COMM_IER, 0x0F);
    CHECK(CommTestRead(BASE + COMM_IER) == 0x0F, "with DLAB clear, base+1 is the interrupt enable");
    CommTestWrite(BASE + COMM_IER, 0x00);

    /* ---- transmit reaches the sink when loopback is OFF -------------------- */
    g_TransmittedCount = 0;
    CommTestWrite(BASE + COMM_RBR, 'A');
    CHECK(g_TransmittedCount == 1 && g_Transmitted[0] == 'A', "out of loopback a transmitted byte reaches the host sink");
    CHECK((CommTestRead(BASE + COMM_LSR) & COMM_LSR_DATA_READY) == 0,
          "...and does NOT appear in the receiver (the pins are connected outward)");

    /* ---- LOCAL LOOPBACK: the self-test every driver runs ------------------- */
    CommTestWrite(BASE + COMM_MCR, COMM_MCR_LOOP);
    g_TransmittedCount = 0;
    CommTestWrite(BASE + COMM_RBR, 0x5A);
    CHECK(g_TransmittedCount == 0, "in loopback the byte does NOT go to the host");
    CHECK((CommTestRead(BASE + COMM_LSR) & COMM_LSR_DATA_READY) == COMM_LSR_DATA_READY, "in loopback the byte sets DATA READY");
    CHECK(CommTestRead(BASE + COMM_RBR) == 0x5A, "...and reads back from RBR unchanged");
    CHECK((CommTestRead(BASE + COMM_LSR) & COMM_LSR_DATA_READY) == 0, "reading RBR clears DATA READY");

    /* the four control->status pairings, checked ONE AT A TIME so a swapped
     * pair cannot hide behind another bit being right
     */
    CommTestWrite(BASE + COMM_MCR, (BYTE)(COMM_MCR_LOOP | COMM_MCR_DTR));
    CHECK((CommTestRead(BASE + COMM_MSR) & 0xF0) == COMM_MSR_DSR,  "loopback: DTR -> DSR, alone");
    CommTestWrite(BASE + COMM_MCR, (BYTE)(COMM_MCR_LOOP | COMM_MCR_RTS));
    CHECK((CommTestRead(BASE + COMM_MSR) & 0xF0) == COMM_MSR_CTS,  "loopback: RTS -> CTS, alone");
    CommTestWrite(BASE + COMM_MCR, (BYTE)(COMM_MCR_LOOP | COMM_MCR_OUT1));
    CHECK((CommTestRead(BASE + COMM_MSR) & 0xF0) == COMM_MSR_RI,   "loopback: OUT1 -> RI, alone");
    CommTestWrite(BASE + COMM_MCR, (BYTE)(COMM_MCR_LOOP | COMM_MCR_OUT2));
    CHECK((CommTestRead(BASE + COMM_MSR) & 0xF0) == COMM_MSR_DCD,  "loopback: OUT2 -> DCD, alone");

    /* ---- MSR delta bits latch until the register is READ ------------------- */
    CommTestWrite(BASE + COMM_MCR, COMM_MCR_LOOP);                 /* drop every line */
    CommTestRead(BASE + COMM_MSR);                           /* acknowledge, deltas clear */
    CommTestWrite(BASE + COMM_MCR, (BYTE)(COMM_MCR_LOOP | COMM_MCR_DTR | COMM_MCR_RTS));
    { BYTE modemStatus = CommTestRead(BASE + COMM_MSR);
      CHECK((modemStatus & COMM_MSR_DELTA_DSR) == COMM_MSR_DELTA_DSR,        "asserting DTR sets the DDSR delta bit");
      CHECK((modemStatus & COMM_MSR_DELTA_CTS) == COMM_MSR_DELTA_CTS,        "asserting RTS sets the DCTS delta bit");
      CHECK((CommTestRead(BASE + COMM_MSR) & 0x0F) == 0, "reading MSR is the acknowledgement: deltas clear"); }

    /* TERI is a TRAILING edge -- set on 1->0, not on 0->1 */
    CommTestWrite(BASE + COMM_MCR, (BYTE)(COMM_MCR_LOOP | COMM_MCR_OUT1));   /* RI 0 -> 1 */
    CHECK((CommTestRead(BASE + COMM_MSR) & COMM_MSR_TRAILING_RI) == 0, "RI going HIGH does NOT set TERI");
    CommTestWrite(BASE + COMM_MCR, COMM_MCR_LOOP);                          /* RI 1 -> 0 */
    CHECK((CommTestRead(BASE + COMM_MSR) & COMM_MSR_TRAILING_RI) == COMM_MSR_TRAILING_RI, "RI going LOW DOES set TERI");

    /* ---- IIR: priority, and reading it clears THRE ------------------------- */
    CommTestWrite(BASE + COMM_MCR, COMM_MCR_LOOP);
    CommTestWrite(BASE + COMM_IER, 0x00);
    CHECK((CommTestRead(BASE + COMM_IIR) & 0x01) == 1, "with every source masked, IIR reports nothing pending");
    CommTestWrite(BASE + COMM_IER, COMM_IER_THR_EMPTY);
    CommTestWrite(BASE + COMM_RBR, 'x');                      /* transmit -> THRE owed */
    { BYTE first = CommTestRead(BASE + COMM_IIR);
      /* RX data available outranks THRE, and the loopback byte is now waiting,
       * so with BOTH enabled the answer must be 0x04.
       */
      CommTestWrite(BASE + COMM_IER, (BYTE)(COMM_IER_THR_EMPTY | COMM_IER_RECEIVED_DATA));
      CHECK(first == 0x02, "IIR reports THRE (0x02) when only THRE is enabled");
      CHECK(CommTestRead(BASE + COMM_IIR) == 0x04,
            "received-data-available (0x04) OUTRANKS THRE -- IIR is a priority encoder"); }
    CommTestRead(BASE + COMM_RBR);                           /* drain the byte */
    CHECK((CommTestRead(BASE + COMM_IIR) & 0x01) == 1,
          "reading IIR cleared THRE, so once the byte is drained nothing is owed "
          "(an IIR that keeps reporting THRE is an interrupt storm)");

    /* ---- LSR clears errors on read, but not data-ready --------------------- */
    CommTestWrite(BASE + COMM_IER, 0x00);
    CommTestWrite(BASE + COMM_RBR, 'q');                      /* loopback: DR set */
    g_Comm.Ports[0].Lsr |= COMM_LSR_OVERRUN;                        /* pretend an overrun */
    { BYTE lineStatus = CommTestRead(BASE + COMM_LSR);
      CHECK((lineStatus & COMM_LSR_OVERRUN) == COMM_LSR_OVERRUN, "LSR reports the overrun");
      CHECK((CommTestRead(BASE + COMM_LSR) & COMM_LSR_OVERRUN) == 0, "reading LSR clears the ERROR bits");
      CHECK((CommTestRead(BASE + COMM_LSR) & COMM_LSR_DATA_READY) == COMM_LSR_DATA_READY,
            "...but NOT data-ready, which is not an error and is still true"); }
    CommTestRead(BASE + COMM_RBR);

    /* ---- the FIFO control register resets the receiver --------------------- */
    CommTestWrite(BASE + COMM_RBR, '1'); CommTestWrite(BASE + COMM_RBR, '2');
    CHECK((CommTestRead(BASE + COMM_LSR) & COMM_LSR_DATA_READY) == COMM_LSR_DATA_READY, "two loopback bytes are waiting");
    CommTestWrite(BASE + COMM_IIR, 0x02);                     /* FCR: clear receive FIFO */
    CHECK((CommTestRead(BASE + COMM_LSR) & COMM_LSR_DATA_READY) == 0, "FCR bit 1 clears the receive FIFO");
    CHECK((CommTestRead(BASE + COMM_IIR) & 0xC0) == 0,
          "IIR does not claim an enabled FIFO until FCR bit 0 is set");
    CommTestWrite(BASE + COMM_IIR, 0x01);
    CHECK((CommTestRead(BASE + COMM_IIR) & 0xC0) == 0xC0, "with FCR bit 0 set, IIR reports a 16550 FIFO");
    CommTestWrite(BASE + COMM_IIR, 0x00);

    /* ---- host -> guest ----------------------------------------------------- */
    CommTestWrite(BASE + COMM_MCR, 0x00);                     /* out of loopback */
    CHECK(VddCommReceive(&g_Comm, 0, 'H') == 0, "the host can push a received byte");
    CHECK((CommTestRead(BASE + COMM_LSR) & COMM_LSR_DATA_READY) == COMM_LSR_DATA_READY, "...which sets data-ready");
    CHECK(CommTestRead(BASE + COMM_RBR) == 'H', "...and reads out of RBR");

    /* ---- OUT2 GATES THE LINE, NOT THE PART (GH #181) ------------------------
     * All out of loopback, deliberately: whether loopback closes the gate is
     * the one question here the datasheets and the Super I/O parts answer
     * differently, and it is not pinned until an oracle is asked.
     */
    CommTestWrite(BASE + COMM_MCR, 0x00);                     /* OUT2 clear */
    CommTestWrite(BASE + COMM_IER, COMM_IER_RECEIVED_DATA);
    memset(g_IrqCounts, 0, sizeof g_IrqCounts);
    VddCommReceive(&g_Comm, 0, 'i');
    CHECK(g_IrqCounts[4] == 0,
          "OUT2 clear: a received byte with RDA enabled raises NO IRQ (the buffer "
          "between the part and the PIC is off)");
    CHECK(CommTestRead(BASE + COMM_IIR) == 0x04,
          "...but IIR still reports received-data-available (0x04) -- the gate is "
          "outside the chip, so a polling driver reads the true source");
    CommTestWrite(BASE + COMM_MCR, COMM_MCR_OUT2);
    CHECK(g_IrqCounts[4] == 1,
          "setting OUT2 with that interrupt still pending raises IRQ4 at once "
          "(the late-OUT2 driver order still gets its interrupt)");
    CHECK(CommTestRead(BASE + COMM_RBR) == 'i', "...and the byte that was waiting is the one read");
    CHECK((CommTestRead(BASE + COMM_IIR) & 0x01) == 1, "...after which nothing is owed");
    VddCommReceive(&g_Comm, 0, 'j');
    CHECK(g_IrqCounts[4] == 2, "OUT2 set: a received byte with RDA enabled raises IRQ4");
    CHECK(g_IrqCounts[3] == 0, "...on COM1's line only, not COM2's");
    CommTestRead(BASE + COMM_RBR);
    CommTestWrite(BASE + COMM_MCR, 0x00);                     /* close the gate again */
    CommTestWrite(BASE + COMM_IER, COMM_IER_THR_EMPTY);
    memset(g_IrqCounts, 0, sizeof g_IrqCounts);
    g_TransmittedCount = 0;
    CommTestWrite(BASE + COMM_RBR, 't');                      /* to the sink: THRE owed */
    CHECK(g_TransmittedCount == 1 && g_IrqCounts[4] == 0,
          "OUT2 clear: a transmit with THRE enabled raises NO IRQ either");
    CHECK(CommTestRead(BASE + COMM_IIR) == 0x02,
          "...while IIR still reports THRE (0x02), and that read acknowledges it");
    CommTestWrite(BASE + COMM_MCR, COMM_MCR_OUT2);
    CHECK(g_IrqCounts[4] == 0,
          "opening the gate after THRE was acknowledged raises nothing -- nothing is owed");
    CommTestWrite(BASE + COMM_IER, 0x00);
    CommTestWrite(BASE + COMM_MCR, 0x00);

    /* ---- #245 (s90): the 16550 remainder, each line from the PC16550D --------- */
    /* enabling THRE with the holding register already empty IS a THRE interrupt */
    CommTestWrite(BASE + COMM_MCR, COMM_MCR_OUT2);
    CommTestWrite(BASE + COMM_IER, 0x00);
    CommTestRead(BASE + COMM_IIR);
    memset(g_IrqCounts, 0, sizeof g_IrqCounts);
    CommTestWrite(BASE + COMM_IER, COMM_IER_THR_EMPTY);
    CHECK(g_IrqCounts[4] == 1, "#245: setting ETBEI with THR empty raises THRE at once (the "
                         "interrupt-driven transmit kick-start)");
    CHECK(CommTestRead(BASE + COMM_IIR) == 0x02, "...and IIR names it THRE");
    CommTestWrite(BASE + COMM_IER, COMM_IER_THR_EMPTY);
    CHECK((CommTestRead(BASE + COMM_IIR) & 0x01) == 1,
          "re-writing ETBEI when it is ALREADY set is not a new 0->1 edge: nothing owed");
    /* INT 14h AH=01 owes the same THRE the port write does */
    memset(g_IrqCounts, 0, sizeof g_IrqCounts);
    CommTestInt14(0x01, 'b', 0, &registers);
    CHECK(g_IrqCounts[4] == 1 && CommTestRead(BASE + COMM_IIR) == 0x02,
          "#245: a byte sent through INT 14h raises THRE like a THR write");
    CommTestWrite(BASE + COMM_IER, 0x00);
    /* LOOPBACK CLOSES THE IRQ GATE (output pins forced inactive, OUT2 included) */
    CommTestWrite(BASE + COMM_MCR, (BYTE)(COMM_MCR_LOOP | COMM_MCR_OUT2));
    CommTestWrite(BASE + COMM_IER, COMM_IER_RECEIVED_DATA);
    memset(g_IrqCounts, 0, sizeof g_IrqCounts);
    CommTestWrite(BASE + COMM_RBR, 'L');
    CHECK(g_IrqCounts[4] == 0, "#245: in loopback a received byte raises NO IRQ even with OUT2 "
                         "set -- the OUT2 PIN is forced inactive, and on a PC it is the gate");
    CHECK(CommTestRead(BASE + COMM_IIR) == 0x04, "...while the part still reports RDA in IIR");
    CommTestRead(BASE + COMM_RBR);
    /* BREAK in loopback: one zero character, BI set, an RLS interrupt owed */
    CommTestWrite(BASE + COMM_IER, COMM_IER_LINE_STATUS | COMM_IER_RECEIVED_DATA);
    CommTestWrite(BASE + COMM_LCR, (BYTE)(0x03 | COMM_LCR_BREAK));
    CHECK(CommTestRead(BASE + COMM_IIR) == 0x06, "#245: SET BREAK in loopback owes a line-status "
                                       "interrupt (IIR 0x06, the highest priority)");
    { BYTE lineStatus = CommTestRead(BASE + COMM_LSR);
      CHECK((lineStatus & (COMM_LSR_BREAK | COMM_LSR_DATA_READY)) == (COMM_LSR_BREAK | COMM_LSR_DATA_READY), "...LSR reports BREAK and a character");
      CHECK((CommTestRead(BASE + COMM_LSR) & COMM_LSR_BREAK) == 0, "...and the LSR read clears BI"); }
    CHECK(CommTestRead(BASE + COMM_RBR) == 0x00, "...the character is a single ZERO");
    CHECK((CommTestRead(BASE + COMM_LSR) & COMM_LSR_DATA_READY) == 0, "...and only one of them");
    CommTestWrite(BASE + COMM_LCR, (BYTE)(0x03 | COMM_LCR_BREAK));
    CHECK((CommTestRead(BASE + COMM_LSR) & COMM_LSR_DATA_READY) == 0, "holding break is not a second break");
    CommTestWrite(BASE + COMM_LCR, 0x03);
    CHECK(g_Comm.Ports[0].Breaks == 1, "the break was counted once");
    /* THE RECEIVE FIFO'S TRIGGER LEVEL, out of loopback */
    CommTestWrite(BASE + COMM_MCR, COMM_MCR_OUT2);
    CommTestWrite(BASE + COMM_IER, COMM_IER_RECEIVED_DATA);
    CommTestWrite(BASE + COMM_IIR, 0x41);                     /* FCR: FIFO on, trigger = 4 */
    VddCommReceive(&g_Comm, 0, '1'); VddCommReceive(&g_Comm, 0, '2');
    CHECK(CommTestRead(BASE + COMM_IIR) == 0xCC, "#245: 2 bytes below a trigger of 4 -> CHARACTER "
                                       "TIMEOUT (IIR 0xCC with the FIFO bits)");
    VddCommReceive(&g_Comm, 0, '3'); VddCommReceive(&g_Comm, 0, '4');
    CHECK(CommTestRead(BASE + COMM_IIR) == 0xC4, "...4 bytes reach the trigger -> RDA (0xC4)");
    CHECK(CommTestRead(BASE + COMM_RBR) == '1', "...read in order");
    CHECK(CommTestRead(BASE + COMM_IIR) == 0xCC, "...3 left, below the trigger again -> timeout");
    CommTestRead(BASE + COMM_RBR); CommTestRead(BASE + COMM_RBR); CommTestRead(BASE + COMM_RBR);
    CHECK((CommTestRead(BASE + COMM_IIR) & 0x0F) == 0x01, "...drained: nothing owed");
    CommTestWrite(BASE + COMM_IIR, 0xC1);                     /* trigger = 14 */
    {
        INT index;
        for (index = 0; index < 20; ++index) VddCommReceive(&g_Comm, 0, (BYTE)('a' + index));
    }
    CHECK(CommTestRead(BASE + COMM_IIR) == 0xC4, "20 queued, 16 in the FIFO >= 14 -> RDA");
    CHECK((CommTestRead(BASE + COMM_LSR) & COMM_LSR_OVERRUN) == 0,
          "...and the 4 beyond the FIFO wait on the WIRE: no overrun for a burst the host queued");
    CommTestWrite(BASE + COMM_IIR, 0x07);                     /* clear both FIFOs, FIFO on */
    CHECK((CommTestRead(BASE + COMM_LSR) & COMM_LSR_DATA_READY) == 0, "FCR bit 1 empties the receiver");
    CommTestWrite(BASE + COMM_IIR, 0x00);                     /* back to 8250 behaviour */
    VddCommReceive(&g_Comm, 0, 'z');
    CHECK(CommTestRead(BASE + COMM_IIR) == 0x04, "FIFO off: one byte is RDA, no timeout, no FIFO bits");
    CommTestRead(BASE + COMM_RBR);
    CommTestWrite(BASE + COMM_IER, 0x00);
    CommTestWrite(BASE + COMM_MCR, 0x00);

    /* ---- INT 14h describes THE SAME PART ----------------------------------- */
    CommTestWrite(BASE + COMM_MCR, COMM_MCR_LOOP);
    CommTestInt14(0x01, 'Z', 0, &registers);                       /* BIOS send, in loopback */
    CHECK((CommTestRead(BASE + COMM_LSR) & COMM_LSR_DATA_READY) == COMM_LSR_DATA_READY,
          "a byte sent through INT 14h in loopback arrives in the SAME receiver");
    CHECK(CommTestRead(BASE + COMM_RBR) == 'Z', "...and reads out of RBR");

    VddCommReceive(&g_Comm, 0, 'k');
    CommTestInt14(0x02, 0, 0, &registers);
    CHECK((registers.Eax & 0xFF) == 'k', "INT 14h AH=02 returns a byte the HOST pushed");
    CHECK(((registers.Eax >> 8) & 0x80) == 0, "...with the TIMEOUT bit CLEAR");
    CommTestInt14(0x02, 0, 0, &registers);
    CHECK((registers.Eax & 0xFFFF) == 0x8000,
          "INT 14h AH=02 with nothing waiting reports TIMEOUT (and does not hang the guest)");

    CommTestInt14(0x00, 0xE3, 0, &registers);                      /* 9600 8N1 */
    CommTestWrite(BASE + COMM_LCR, 0x83);
    CHECK(CommTestRead(BASE + COMM_RBR) == 12,
          "INT 14h AH=00 at 9600 baud leaves divisor 12, readable through DLAB -- "
          "the BIOS and the registers describe one port, not two");
    CommTestWrite(BASE + COMM_LCR, 0x03);

    CommTestInt14(0x03, 0, 1, &registers);
    CHECK((registers.Eax & 0xFFFF) != 0x8000, "COM2 is fitted and answers INT 14h AH=03");
    CommTestInt14(0x03, 0, 4, &registers);
    CHECK((registers.Eax & 0xFFFF) == 0x8000, "a port index past COM4 reports TIMEOUT");

    /* ---- COM3 and COM4: two more slots of the same device (GH #181) --------- */
    CommTestWrite(0x3E8 + COMM_SCR, 0x33);
    CommTestWrite(0x2E8 + COMM_SCR, 0x44);
    CHECK(CommTestRead(0x3E8 + COMM_SCR) == 0x33 && CommTestRead(0x2E8 + COMM_SCR) == 0x44,
          "COM3 (3E8h) and COM4 (2E8h) each answer the scratch probe");
    CHECK(CommTestRead(BASE + COMM_SCR) != 0x33 && CommTestRead(0x2F8 + COMM_SCR) != 0x44,
          "...with their OWN registers, not aliases of COM1/COM2");
    CommTestWrite(0x3E8 + COMM_MCR, (BYTE)(COMM_MCR_LOOP | COMM_MCR_DTR | COMM_MCR_RTS));
    CommTestWrite(0x3E8 + COMM_RBR, 0xC3);
    CHECK(CommTestRead(0x3E8 + COMM_RBR) == 0xC3 && (CommTestRead(0x3E8 + COMM_MSR) & 0xF0) == (COMM_MSR_DSR | COMM_MSR_CTS),
          "COM3 passes the loopback self-test (byte echo + DTR/RTS -> DSR/CTS)");
    CHECK((CommTestRead(BASE + COMM_LSR) & COMM_LSR_DATA_READY) == 0, "...and COM1's receiver saw none of it");
    CommTestWrite(0x3E8 + COMM_MCR, COMM_MCR_OUT2);
    CommTestWrite(0x3E8 + COMM_IER, COMM_IER_RECEIVED_DATA);
    CommTestWrite(0x2E8 + COMM_MCR, COMM_MCR_OUT2);
    CommTestWrite(0x2E8 + COMM_IER, COMM_IER_RECEIVED_DATA);
    memset(g_IrqCounts, 0, sizeof g_IrqCounts);
    VddCommReceive(&g_Comm, 2, 'c');
    CHECK(g_IrqCounts[4] == 1 && g_IrqCounts[3] == 0, "COM3 raises IRQ4, the line it shares with COM1");
    VddCommReceive(&g_Comm, 3, 'd');
    CHECK(g_IrqCounts[3] == 1 && g_IrqCounts[4] == 1, "COM4 raises IRQ3, the line it shares with COM2");
    CHECK(CommTestRead(0x3E8 + COMM_RBR) == 'c' && CommTestRead(0x2E8 + COMM_RBR) == 'd',
          "each byte arrives on the port it was pushed to");
    CommTestInt14(0x03, 0, 2, &registers);
    CHECK((registers.Eax & 0xFFFF) != 0x8000, "a fitted COM3 answers INT 14h DX=2");
    CommTestInt14(0x03, 0, 3, &registers);
    CHECK((registers.Eax & 0xFFFF) != 0x8000, "a fitted COM4 answers INT 14h DX=3");
    CommTestWrite(0x3E8 + COMM_IER, 0); CommTestWrite(0x3E8 + COMM_MCR, 0);
    CommTestWrite(0x2E8 + COMM_IER, 0); CommTestWrite(0x2E8 + COMM_MCR, 0);

    /* A slot that is NOT fitted -- the host's default for COM3/COM4 -- must be
     * absent on every route at once: no registers, no INT 14h, not counted.
     */
    { static VDD_BUS otherBus; static COMM_STATE otherComm; NTVDD_REGISTERS otherRegisters; UINT32 value = 0;
      memset(&otherComm, 0, sizeof otherComm);
      otherComm.Ports[0].BasePort = BASE;  otherComm.Ports[0].Irq = 4; otherComm.Ports[0].IsFitted = 1;
      otherComm.Ports[1].BasePort = 0x2F8; otherComm.Ports[1].Irq = 3; otherComm.Ports[1].IsFitted = 1;
      VddBusInitialize(&otherBus, 0);
      {
          NTVDD_DEVICE device = VddCommDevice(&otherComm);
          VddBusAdd(&otherBus, &device);
      }
      CHECK(VddBusIo(&otherBus, 0x3E8 + COMM_SCR, 1, 1, &value) == 0,
            "an unfitted COM3 leaves 3E8h unclaimed (the guest reads the bus float)");
      CHECK(VddCommIsFitted(&otherComm, 2) == 0 && VddCommIsFitted(&otherComm, 3) == 0,
            "...and vdd_comm_fitted says so, which is what INT 11h and the BDA read");
      memset(&otherRegisters, 0, sizeof otherRegisters); otherRegisters.Eax = 0x0300; otherRegisters.Edx = 3;
      VddBusDeliverInterrupt(&otherBus, 0x14, &otherRegisters);
      CHECK((otherRegisters.Eax & 0xFFFF) == 0x8000, "a port that is NOT fitted reports TIMEOUT"); }

    /* ---- the parallel port: the byte leaves on the STROBE EDGE ------------- */
    g_PrinterCount = 0;
    CHECK((CommTestRead(0x379) & LPT_STATUS_BUSY) == LPT_STATUS_BUSY,
          "LPT status reports NOT BUSY (bit 7 is INVERTED -- a 0 hangs every "
          "printing program on its first byte)");
    CHECK((CommTestRead(0x379) & LPT_STATUS_PAPER_OUT) == 0, "LPT reports paper loaded");
    CHECK((CommTestRead(0x379) & LPT_STATUS_SELECT) == LPT_STATUS_SELECT, "LPT reports the printer online");
    wr8(0x378, 'P');
    CHECK(CommTestRead(0x378) == 'P', "the LPT data latch reads back");
    CHECK(g_PrinterCount == 0, "writing DATA alone prints NOTHING -- the byte is only latched");
    wr8(0x37A, LPT_CONTROL_STROBE);                 /* 0 -> 1: the latching edge */
    CHECK(g_PrinterCount == 1 && g_PrinterBytes[0] == 'P', "the RISING edge of STROBE prints the latched byte");
    wr8(0x37A, LPT_CONTROL_STROBE);                 /* still high: no new edge */
    CHECK(g_PrinterCount == 1, "holding STROBE high does not print it again");
    wr8(0x37A, 0);
    CHECK(g_PrinterCount == 1, "the falling edge does not print either");
    wr8(0x378, 'Q'); wr8(0x37A, LPT_CONTROL_STROBE);
    CHECK(g_PrinterCount == 2 && g_PrinterBytes[1] == 'Q', "the next latch+strobe prints the next byte");

    /* [CAUTION]: In a dialect scripts/offvm.sh parses. This line used to read "N/M checks
     * passed", which the runner does not know -- so the whole battery ran on
     * its exit status and was counted as 0 checks.
     */
    printf("\n== %d checks, %d failed\n", g_Total, g_Failures);
    return g_Failures ? 1 : 0;
}
