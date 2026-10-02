/* vdd_ide.c -- see vdd_ide.h.  An IDE/ATA adapter with both channels empty. */
#include "vdd_ide.h"

void vdd_ide_in(void *self, uint16_t port, uint8_t width, uint32_t *val)
{
    ide_state *st = (ide_state *)self;
    (void)port; (void)width;
    st->reads++;
    /* Nothing drives the data lines: DD7 is pulled down (ATA-3 Table 2 note 3) and
       DD6:0 are answered 0 by choice -- vdd_ide.h. BSY therefore reads CLEAR on
       every register of both channels, at every width, which is the single fact a
       polling loop needs in order to stop. */
    *val = 0;
}

void vdd_ide_out(void *self, uint16_t port, uint8_t width, uint32_t val)
{
    ide_state *st = (ide_state *)self;
    (void)width;
    st->writes++;
    /* No drive latches anything -- including SRST/nIEN at 3F6h/376h, which are
       DEVICE bits. A command written to an empty channel is received by nobody:
       no BSY, no INTRQ, no DRQ, ever. Counted so a trace can say what was asked. */
    if (port == IDE_PRI_CMD + 7 || port == IDE_SEC_CMD + 7) {
        st->cmds++;
        st->last_cmd = (uint8_t)val;
    }
}

void vdd_ide_reset(void *self)
{
    ide_state *st = (ide_state *)self;
    st->reads = st->writes = st->cmds = 0;
    st->last_cmd = 0;
}

int vdd_ide_init(vdd_bus *b, void *self)
{
    ide_state *st = (ide_state *)self;
    st->bus = b;
    vdd_ide_reset(st);
    /* ⛔ 3F7h IS NOT CLAIMED HERE -- the FDC owns it (DIR bit 7). See vdd_ide.h. */
    if (vdd_claim_ports(b, IDE_PRI_CMD, IDE_PRI_CMD + 7, vdd_ide_in, vdd_ide_out, st)) return -1;
    if (vdd_claim_ports(b, IDE_PRI_CTL, IDE_PRI_CTL,     vdd_ide_in, vdd_ide_out, st)) return -1;
    if (vdd_claim_ports(b, IDE_SEC_CMD, IDE_SEC_CMD + 7, vdd_ide_in, vdd_ide_out, st)) return -1;
    if (vdd_claim_ports(b, IDE_SEC_CTL, IDE_SEC_CTL + 1, vdd_ide_in, vdd_ide_out, st)) return -1;
    return 0;
}
