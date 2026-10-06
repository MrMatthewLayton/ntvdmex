/* ide_test.c -- off-VM battery for the empty IDE/ATA adapter (vdd_ide.c). GH #179.
 *
 * ⛔ The gap this device closed was a HANG. With nothing on 1F0h-1F7h/3F6h the ISA
 *   default FFh reads as BSY=1, and the ATA's own wait -- `test al,80h / jnz` --
 *   never exits. See vdd_ide.h and docs/inventory/ide.md.
 *
 * ── HOW THESE CHECKS WERE WRITTEN. ─────────────────────────────────────────────
 * From ATA-3 (X3T13/2008D rev 7b) -- Table 2 note 3 (pull-down on DD7), 8.7.1 (h)
 * (an absent device's status is 00h after reset), 8.7.2's host note (the write-
 * then-read-back presence test) -- and from the three things a DETECTION ROUTINE
 * does: wait for BSY clear, test for a latching register file, issue IDENTIFY and
 * wait for DRQ. `host_in` reproduces the host's unclaimed-port rule (main.c,
 * host_io_do: FFh when no VDD owns the port), so the negative control below is
 * the machine as it was before this device, not an assumption about it.
 */
#include <stdio.h>
#include <string.h>
#include "vdd_ide.h"

static int total = 0, fails = 0;
#define CHECK(cond, msg) do {                                  \
        total++;                                               \
        if (cond) { printf("  PASS  %s\n", (msg)); }           \
        else      { printf("  FAIL  %s\n", (msg)); fails++; }  \
    } while (0)

static int g_irqs;
static void irq_sink(void *ctx, uint8_t irq) { (void)ctx; (void)irq; g_irqs++; }

/* The host's rule for an IN: a VDD's answer, or FFh(s) when nobody claims it. */
static uint32_t host_in(VDD_BUS *bus, uint16_t port, uint8_t width)
{
    uint32_t v = 0;
    if (!VddBusIo(bus, port, width, 1, &v)) v = 0xFFFFFFFFu;
    return width == 1 ? (v & 0xFF) : width == 2 ? (v & 0xFFFF) : v;
}
static void host_out(VDD_BUS *bus, uint16_t port, uint8_t val)
{ uint32_t v = val; VddBusIo(bus, port, 1, 0, &v); }

/* The datasheet's BSY wait, bounded. 1 = it would have exited. */
static int bsy_wait_exits(VDD_BUS *bus, uint16_t port)
{
    int spin;
    for (spin = 0; spin < 65536; ++spin)
        if (!(host_in(bus, port, 1) & IDE_STATUS_BUSY)) return 1;
    return 0;
}

/* The presence test: write two patterns, read them back. 1 = "a device latched". */
static int latches(VDD_BUS *bus, uint16_t cmdbase)
{
    host_out(bus, cmdbase + 2, 0x55); host_out(bus, cmdbase + 3, 0xAA);
    if (host_in(bus, cmdbase + 2, 1) == 0x55 && host_in(bus, cmdbase + 3, 1) == 0xAA) return 1;
    host_out(bus, cmdbase + 2, 0xAA); host_out(bus, cmdbase + 3, 0x55);
    return host_in(bus, cmdbase + 2, 1) == 0xAA && host_in(bus, cmdbase + 3, 1) == 0x55;
}

int main(void)
{
    static const uint16_t chan[2][2] = { { IDE_PRIMARY_COMMAND, IDE_PRIMARY_CONTROL },
                                         { IDE_SECONDARY_COMMAND, IDE_SECONDARY_CONTROL } };
    VDD_BUS bus; IDE_STATE st; NTVDD_DEVICE dev;
    int c, spin, drq;
    uint16_t p;

    /* ── NEGATIVE CONTROL: the machine before this device. ── */
    VddBusInitialize(&bus, NULL);
    CHECK(host_in(&bus, IDE_PRIMARY_CONTROL, 1) == 0xFF, "control: unclaimed 3F6h floats to FFh");
    CHECK(!bsy_wait_exits(&bus, IDE_PRIMARY_CONTROL),
          "control: on FFh the BSY wait NEVER exits (the hang this closes)");

    /* ── the adapter, fitted ── */
    memset(&st, 0, sizeof st);
    VddBusInitialize(&bus, NULL);
    VddBusSetSinks(&bus, irq_sink, NULL, NULL, NULL);
    dev = VddIdeDevice(&st);
    CHECK(VddBusAdd(&bus, &dev) == 0 && bus.ClaimFailures == 0, "adapter claims both channels");

    for (c = 0; c < 2; ++c) {
        uint16_t cmd = chan[c][0], ctl = chan[c][1];
        char m[96];
        sprintf(m, "%03Xh: alternate status reads 00h (BSY clear, DD7 pulled down)", ctl);
        CHECK(host_in(&bus, ctl, 1) == 0x00, m);
        sprintf(m, "%03Xh: status reads 00h", cmd + 7);
        CHECK(host_in(&bus, cmd + 7, 1) == 0x00, m);
        sprintf(m, "%03Xh: the canonical BSY wait exits", ctl);
        CHECK(bsy_wait_exits(&bus, ctl), m);
        sprintf(m, "%03Xh: ...and on the status register too", cmd + 7);
        CHECK(bsy_wait_exits(&bus, cmd + 7), m);
        /* Every task-file register: 0, and BSY clear on all of them. */
        { int all0 = 1;
          for (p = cmd + 1; p <= cmd + 7; ++p) if (host_in(&bus, p, 1) != 0) all0 = 0;
          sprintf(m, "%03Xh-%03Xh: every task-file register reads 0", cmd + 1, cmd + 7);
          CHECK(all0, m); }
        sprintf(m, "%03Xh: 16-bit data read is 0000h, 32-bit is 0", cmd);
        CHECK(host_in(&bus, cmd, 2) == 0 && host_in(&bus, cmd, 4) == 0, m);
        /* ⛔ the presence test must NOT find a drive */
        sprintf(m, "%03Xh: 55h/AAh write-read-back does not echo (no false drive)", cmd + 2);
        CHECK(!latches(&bus, cmd), m);
        /* device 1 selected: ATA-3 8.7.1(h) -- an absent device's status is 00h */
        host_out(&bus, cmd + 6, 0xB0);
        sprintf(m, "%03Xh: device 1 selected, status 00h", ctl);
        CHECK(host_in(&bus, ctl, 1) == 0x00, m);
        host_out(&bus, cmd + 6, 0xA0);
        /* SRST through device control: still nothing busy afterwards */
        host_out(&bus, ctl, 0x04); host_out(&bus, ctl, 0x00);
        sprintf(m, "%03Xh: after SRST set+clear, BSY still clear", ctl);
        CHECK(!(host_in(&bus, ctl, 1) & IDE_STATUS_BUSY), m);
        /* IDENTIFY: nobody receives it -- no DRQ, no IRQ, no error to read. */
        g_irqs = 0;
        host_out(&bus, cmd + 7, 0xEC);
        drq = 0;
        for (spin = 0; spin < 1000; ++spin) if (host_in(&bus, ctl, 1) & IDE_STATUS_DATA_REQUEST) drq = 1;
        sprintf(m, "%03Xh: IDENTIFY to an empty channel never raises DRQ", cmd + 7);
        CHECK(!drq, m);
        sprintf(m, "%03Xh: ...and raises no interrupt", cmd + 7);
        CHECK(g_irqs == 0, m);
        sprintf(m, "%03Xh: ...and the Error register is still 0", cmd + 1);
        CHECK(host_in(&bus, cmd + 1, 1) == 0x00, m);
    }

    /* ── the edges of the claim ── */
    CHECK(host_in(&bus, 0x3F7, 1) == 0xFF, "3F7h is NOT ours -- left for the FDC (DIR)");
    CHECK(host_in(&bus, 0x377, 1) == 0x00, "377h, the secondary drive-address register, is");
    CHECK(host_in(&bus, 0x1EF, 1) == 0xFF && host_in(&bus, 0x1F8, 1) == 0xFF,
          "1EFh and 1F8h are outside the primary block");
    CHECK(host_in(&bus, 0x3F5, 1) == 0xFF, "3F5h (the FDC's FIFO) is not claimed by us");
    CHECK(st.Commands == 2 && st.LastCommand == 0xEC, "diagnostics: two commands seen, last ECh");

    printf("\n%d checks, %d failed\n", total, fails);
    return fails ? 1 : 0;
}
