/* fdc_test.c -- off-VM battery for the 82077AA floppy controller (vdd_fdc.c).
 *
 * ⛔ The gap this device closed was a HANG, not a wrong answer. Nothing claimed
 *   3F0h-3F7h, an unclaimed ISA port reads 0xFF on this host (deliberately --
 *   see the V86 I/O trap), and 3F4h is the MAIN STATUS REGISTER. 0xFF there is
 *   RQM=1 with DIO=1: "ready, and I am the one talking". The datasheet's own
 *   command-write loop -- `in al,3F4h / and al,0C0h / cmp al,80h / jne` -- then
 *   never matches and never exits. See docs/ref/fdc.md 3.
 *
 * ── HOW THESE CHECKS WERE WRITTEN. ─────────────────────────────────────────────
 * From the datasheet and from p_fdc.asm's two-oracle run, NOT from vdd_fdc.c.
 * `driver_send`/`driver_drain` below are deliberately the loops a REAL driver
 * runs, polling MSR between every byte, rather than calls into the model: a test
 * that reached into the struct could not have caught the hang, because the hang
 * is in the handshake and not in any value.
 *
 * ★ PINNED BY TWO ORACLES that agree (6.22 under QEMU, and PCem with a real AMI
 *   486 BIOS): MSR at rest is 80h, VERSION answers 90h in exactly one result
 *   byte, and DUMPREG answers in exactly ten.
 */
#include <stdio.h>
#include <string.h>
#include "vdd_fdc.h"

static int total = 0, fails = 0;
#define CHECK(cond, msg) do {                                  \
        total++;                                               \
        if (cond) { printf("  PASS  %s\n", (msg)); }           \
        else      { printf("  FAIL  %s\n", (msg)); fails++; }  \
    } while (0)

static uint8_t g_flat[0x1000];
static int g_irq6;
static void irq_sink(void *ctx, uint8_t irq) { (void)ctx; if (irq == 6) g_irq6++; }

static uint8_t rd(VDD_BUS *bus, uint16_t port)
{ uint32_t v = 0; VddBusIo(bus, port, 1, 1, &v); return (uint8_t)v; }
static void wr(VDD_BUS *bus, uint16_t port, uint8_t val)
{ uint32_t v = val; VddBusIo(bus, port, 1, 0, &v); }

/* ── THE DRIVER'S OWN SEND LOOP, BOUNDED. Returns 0 if it would have hung. ────
     ⛔⛔ AND THE RETURN VALUE IS NOT OPTIONAL. The first version of this file
       ignored it, and that quietly gutted a check: with 09h missing from the
       command length table the chip executed on byte one, went to result phase,
       and every one of the eight parameter sends that followed FAILED SILENTLY --
       after which the drain found the 7 result bytes it was looking for and the
       test passed. A send that would have hung is the single most important thing
       this file can observe, so it is counted and asserted rather than returned
       into nothing. (The project's own rule: a guard that returns success is a lie
       the whole stack repeats.) */
static int g_send_fail;
static int driver_send(VDD_BUS *bus, uint8_t byte)
{
    int spin;
    for (spin = 0; spin < 10000; ++spin)
        if ((rd(bus, FDC_MSR) & 0xC0) == 0x80) { wr(bus, FDC_FIFO, byte); return 1; }
    g_send_fail++;
    return 0;
}

/* ── THE LENGTH-FREE DRAIN: read while RQM=1 && DIO=1, stop when CMD BSY clears.
     This is how a driver reads a result whose length it does not know, and it is
     what p_fdc.asm does on the real machines. Returns the count. */
static int driver_drain(VDD_BUS *bus, uint8_t *out, int max)
{
    int n = 0, spin;
    for (spin = 0; spin < 10000 && n < max; ++spin) {
        uint8_t m = rd(bus, FDC_MSR);
        if (!(m & FDC_MSR_CB)) break;
        if ((m & 0xC0) == 0xC0) out[n++] = rd(bus, FDC_FIFO);
    }
    return n;
}

int main(void)
{
    VDD_BUS bus; FDC_STATE fd; NTVDD_DEVICE dev;
    uint8_t r[16];
    int n;

    memset(&fd, 0, sizeof(fd));
    VddBusInitialize(&bus, g_flat);
    VddBusSetSinks(&bus, irq_sink, NULL, NULL, NULL);
    dev = VddFdcDevice(&fd);
    CHECK(VddBusAdd(&bus, &dev) == 0, "fdc: gets on the bus");

    /* ════ 1. THE HANG. ══════════════════════════════════════════════════════
         The single load-bearing byte in the chip. 80h = RQM set, DIO clear (it
         wants a command), CMD BSY clear, no drive seeking. Measured at 0080 on
         BOTH oracles. FFh -- what an absent chip gave -- is the value that spins
         the caller for ever. */
    CHECK(rd(&bus, FDC_MSR) == 0x80, "msr: at rest reads 80h, not FFh");
    CHECK(driver_send(&bus, 0x10), "msr: the datasheet's command-write loop EXITS");

    /* ════ 2. VERSION -- the detection command. ══════════════════════════════
         Just sent above. One result byte, 90h ("enhanced 82077AA"), and nothing
         left behind. Both oracles: 0190. */
    n = driver_drain(&bus, r, 16);
    CHECK(n == 1 && r[0] == 0x90, "version: exactly one result byte, and it is 90h");
    CHECK(rd(&bus, FDC_MSR) == 0x80, "version: the chip is idle again afterwards");

    /* ════ 3. CMD BSY IS THE FRAME. ══════════════════════════════════════════
         Set from the first command byte, cleared when the LAST result byte is
         read. It is the only thing that lets a driver drain a result of unknown
         length, so it is worth pinning at both edges. SENSE DRIVE STATUS takes
         one parameter and gives one result, which exercises both. */
    driver_send(&bus, 0x04);
    CHECK((rd(&bus, FDC_MSR) & (FDC_MSR_CB | FDC_MSR_DIO)) == FDC_MSR_CB,
          "phases: CMD BSY set mid-command, DIO still host-to-chip");
    driver_send(&bus, 0x00);
    CHECK((rd(&bus, FDC_MSR) & (FDC_MSR_CB | FDC_MSR_DIO))
              == (FDC_MSR_CB | FDC_MSR_DIO),
          "phases: DIO flips once a result is waiting");
    n = driver_drain(&bus, r, 16);
    CHECK(n == 1 && (r[0] & 0x20) && (r[0] & 0x10),
          "sense drive: ST3 says READY and TRACK0 at cylinder 0");
    CHECK(!(rd(&bus, FDC_MSR) & FDC_MSR_CB),
          "phases: CMD BSY clears on the LAST result byte, not before");

    /* ════ 4. DUMPREG -- ten bytes, drained without a length table. ══════════
         Both oracles answered a count of exactly 10 (0A01). A model that gives a
         different number breaks every driver that uses the CMD BSY rule. */
    driver_send(&bus, 0x0E);
    n = driver_drain(&bus, r, 16);
    CHECK(n == 10, "dumpreg: exactly ten result bytes");

    /* ════ 5. SPECIFY HAS NO RESULT PHASE, AND DUMPREG HANDS IT BACK. ════════
         A command with zero results must leave DIO low and CMD BSY clear the
         moment its last parameter lands -- if it left DIO high, the next thing
         the driver did would be to read a byte that does not exist. */
    driver_send(&bus, 0x03);
    driver_send(&bus, 0xDF);
    driver_send(&bus, 0x02);
    CHECK(rd(&bus, FDC_MSR) == 0x80, "specify: no result phase, chip idle at once");
    driver_send(&bus, 0x0E);
    n = driver_drain(&bus, r, 16);
    CHECK(n == 10 && r[4] == 0xDF && r[5] == 0x02,
          "dumpreg: reports the SPECIFY bytes that actually arrived");

    /* ════ 6. AN INVALID COMMAND IS ONE BYTE OF 80h -- AND STAYS IN FRAME. ═══
         The reply matters less than the framing: an unknown opcode that consumed
         a guessed number of parameters would eat the NEXT real command. Checked
         by issuing a good command immediately afterwards. */
    driver_send(&bus, 0x1F);
    n = driver_drain(&bus, r, 16);
    CHECK(n == 1 && r[0] == 0x80, "invalid: one byte, ST0 = 80h");
    driver_send(&bus, 0x10);
    n = driver_drain(&bus, r, 16);
    CHECK(n == 1 && r[0] == 0x90,
          "invalid: does NOT swallow the next command as a parameter");

    /* ════ 7. SEEK, ITS INTERRUPT, AND THE COMMAND THAT COLLECTS IT. ═════════
         SEEK has no result phase at all: the interrupt IS the report, and SENSE
         INTERRUPT STATUS is the only way to learn it finished. ST0 bit 5 is SEEK
         END; the second byte is the present cylinder. */
    g_irq6 = 0;
    driver_send(&bus, 0x0F); driver_send(&bus, 0x00); driver_send(&bus, 0x27);
    CHECK(g_irq6 == 1, "seek: raises IRQ6 exactly once");
    CHECK(rd(&bus, FDC_MSR) == 0x80, "seek: no result phase");
    driver_send(&bus, 0x08);
    n = driver_drain(&bus, r, 16);
    CHECK(n == 2 && (r[0] & 0x20) && r[1] == 0x27,
          "sense int: SEEK END, and the present cylinder is where we sent it");
    driver_send(&bus, 0x08);
    n = driver_drain(&bus, r, 16);
    CHECK(n == 1 && r[0] == 0x80,
          "sense int: asked twice, the second says 80h -- nothing pending");

    /* ════ 8. DMAGATE IS NOT DECORATION. ════════════════════════════════════
         With DOR bit 3 clear the chip still works and the interrupt simply never
         reaches the PIC. A model that ignores the bit silently disobeys a driver
         that cleared it -- and there is no wrong VALUE anywhere to notice. */
    g_irq6 = 0;
    wr(&bus, FDC_DOR, (uint8_t)(FDC_DOR_NRESET));      /* gate OFF, out of reset */
    driver_send(&bus, 0x07); driver_send(&bus, 0x00);  /* RECALIBRATE            */
    CHECK(g_irq6 == 0, "dmagate: clear -> IRQ6 is not delivered");
    wr(&bus, FDC_DOR, (uint8_t)(FDC_DOR_NRESET | FDC_DOR_DMA_GATE));
    g_irq6 = 0;
    driver_send(&bus, 0x07); driver_send(&bus, 0x00);
    CHECK(g_irq6 == 1, "dmagate: set -> IRQ6 is delivered");

    /* ════ 9. DOR BIT 2 IS AN ACTIVE-LOW RESET, AND IT REALLY HOLDS. ═════════
         A plausible "turn everything off" of 00h holds the chip down. MSR then
         reads 00h -- which hangs a poller, on a real machine too. Faithful, and
         worth a check precisely because it looks like a bug. */
    wr(&bus, FDC_DOR, 0x00);
    CHECK(rd(&bus, FDC_MSR) == 0x00, "reset: held low -> MSR reads 00h");
    CHECK(rd(&bus, FDC_DOR) == 0x00, "dor: reads back what was written");

    /* ════ 10. AND RELEASING IT RUNS THE RESET SEQUENCE -- FOUR SENSES. ══════
         Reset leaves the chip expecting FOUR sense-interrupts, one per drive,
         each answering C0h|drive ("ready changed"). Four, not one: a model that
         answers a single sense hands a driver that issued four an 80h for three
         of them. docs/ref/fdc.md 8. */
    g_irq6 = 0;
    wr(&bus, FDC_DOR, (uint8_t)(FDC_DOR_NRESET | FDC_DOR_DMA_GATE));
    CHECK(rd(&bus, FDC_MSR) == 0x80, "reset: released -> ready for a command");
    CHECK(g_irq6 == 1, "reset: raises one interrupt");
    {
        int i, ok = 1;
        for (i = 0; i < 4; ++i) {
            driver_send(&bus, 0x08);
            n = driver_drain(&bus, r, 16);
            if (n != 2 || r[0] != (uint8_t)(0xC0 | i)) ok = 0;
        }
        CHECK(ok, "reset: four sense-interrupts, C0h|drive, one per drive");
        driver_send(&bus, 0x08);
        n = driver_drain(&bus, r, 16);
        CHECK(n == 1 && r[0] == 0x80, "reset: the fifth sense says 80h");
    }

    /* ════ 11. DSR BIT 7 IS THE OTHER RESET DOOR, AND THE DATA RATE SURVIVES.
         A driver is entitled to reset through DSR and not re-select the rate. */
    wr(&bus, FDC_DIR, 0x02);                  /* CCR: 250 kbps                  */
    wr(&bus, FDC_MSR, 0x81);                  /* DSR: reset, rate 01            */
    CHECK(rd(&bus, FDC_MSR) == 0x80, "dsr: software reset leaves the chip ready");
    CHECK((fd.Dsr & 0x03) == 0x01 && fd.Ccr == 0x02,
          "dsr: the data rate survives the reset it asked for");

    /* ════ 12. DIR BIT 7 IS DSKCHG AND WE ANSWER 0. ═════════════════════════
         FFh there means "the disk has been changed" on every access for ever,
         which makes the drive look permanently unreliable rather than absent.
         Both oracles read 0. It is also the same claim INT 13h AH=15h already
         makes through the other door: one medium, two doors, one answer. */
    CHECK((rd(&bus, FDC_DIR) & 0x80) == 0x00, "dir: DSKCHG clear, not FFh");

    /* ════ 13. LOCK KEEPS CONFIGURE ACROSS A SOFTWARE RESET. ════════════════
         That is the whole purpose of the command, and the only observable
         difference between a locked and an unlocked part. */
    driver_send(&bus, 0x13);                          /* CONFIGURE              */
    driver_send(&bus, 0x00); driver_send(&bus, 0x2F); driver_send(&bus, 0x05);
    driver_send(&bus, 0x94); driver_drain(&bus, r, 16);   /* LOCK (bit 7 set)   */
    wr(&bus, FDC_MSR, 0x80);                          /* DSR software reset     */
    driver_send(&bus, 0x0E);
    n = driver_drain(&bus, r, 16);
    CHECK(n == 10 && r[8] == 0x2F && r[9] == 0x05,
          "lock: CONFIGURE survives a software reset while locked");

    /* ════ 14. THE DATA COMMANDS ARE **PART**, AND FAIL THE WAY THE CHIP DOES.
         READ DATA has no data path yet. It must NOT answer "invalid command" --
         that would be a chip contradicting the 90h it just gave for VERSION. It
         terminates the way a real controller terminates over an unformatted
         track: ST0 interrupt code 01 (abnormal), ST1 bit 2 (no data), seven
         bytes. ⚠ THIS CHECK ASSERTS A KNOWN GAP. It changes when the DMA path
         lands, and it is here so that landing cannot be silent. */
    g_send_fail = 0;
    driver_send(&bus, 0xE6);                          /* READ DATA, MT+MFM+SK   */
    driver_send(&bus, 0x00); driver_send(&bus, 0x00); driver_send(&bus, 0x00);
    driver_send(&bus, 0x01); driver_send(&bus, 0x02); driver_send(&bus, 0x12);
    driver_send(&bus, 0x1B); driver_send(&bus, 0xFF);
    n = driver_drain(&bus, r, 16);
    CHECK(g_send_fail == 0 && n == 7,
          "read data: all nine bytes accepted, seven result bytes back");
    CHECK((r[0] & 0xC0) == 0x40 && (r[1] & 0x04),
          "read data: abnormal termination + no data -- NOT 'invalid command'");

    /* ════ 15. THE COMMAND LENGTH TABLE IS A FRAMING DECISION. ══════════════
         ⚠ 09h WRITE DELETED DATA sits in a gap between 08h and 0Ah and no
           detection routine ever issues it -- I had left it out. Omitted, it
           becomes an "invalid command" that consumes ONE byte, and its eight
           parameters are then read as EIGHT MORE COMMANDS. Getting one entry
           wrong does not produce one wrong answer; it desynchronises everything
           after it. So the check is not "what did 09h reply" but "is the chip
           still in frame afterwards".
         ⚠ 11h (SCAN EQUAL) and 18h (a National part-ID command) are the opposite
           mistake: µPD765/PC8477 commands the 82077AA does NOT have. A part that
           answers 90h to VERSION and then accepts them describes a chip that
           does not exist. */
    g_send_fail = 0;
    driver_send(&bus, 0x09);                          /* WRITE DELETED DATA     */
    driver_send(&bus, 0x00); driver_send(&bus, 0x00); driver_send(&bus, 0x00);
    driver_send(&bus, 0x01); driver_send(&bus, 0x02); driver_send(&bus, 0x12);
    driver_send(&bus, 0x1B); driver_send(&bus, 0xFF);
    n = driver_drain(&bus, r, 16);
    /* ★ g_send_fail IS THE WHOLE CHECK. If 09h is missing from the length table
         the chip executes on byte one, flips to result phase, and the next eight
         sends are refused -- while the drain still finds 7 bytes and `n == 7`
         still passes. Only the refused sends can tell the two apart. */
    CHECK(g_send_fail == 0 && n == 7,
          "09h: eight parameters accepted -- not executed on byte one");
    driver_send(&bus, 0x10);
    n = driver_drain(&bus, r, 16);
    CHECK(n == 1 && r[0] == 0x90, "09h: the chip is still in frame afterwards");

    driver_send(&bus, 0x11);                          /* SCAN EQUAL: not ours   */
    n = driver_drain(&bus, r, 16);
    CHECK(n == 1 && r[0] == 0x80, "11h: a 765 command an 82077AA does not have");
    driver_send(&bus, 0x18);                          /* National part ID       */
    n = driver_drain(&bus, r, 16);
    CHECK(n == 1 && r[0] == 0x80, "18h: not an Intel 82077AA command either");
    driver_send(&bus, 0x10);
    n = driver_drain(&bus, r, 16);
    CHECK(n == 1 && r[0] == 0x90, "invalid opcodes leave the chip in frame");

    printf("\n%d checks, %d failed\n", total, fails);
    return fails ? 1 : 0;
}
