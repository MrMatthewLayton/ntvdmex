/* vdd_fdc.c -- see vdd_fdc.h.  Intel 82077AA on the VDD bus.  Pure C. */
#include "vdd_fdc.h"

/* ── THE MAIN STATUS REGISTER IS DERIVED, NEVER STORED. ─────────────────────────
     Every bit of it is a statement about state that lives somewhere else, so a
     stored copy is a second opinion waiting to drift. This is the same lesson the
     8254's OUT pin taught -- "derive it, do not latch it" -- and it is a stronger
     rule here, because MSR is the register the whole protocol turns on.

   ⚠ RQM IS ALWAYS SET WHEN THE CHIP IS OUT OF RESET. A real part drops it for a
     few microseconds between bytes while the FIFO moves; we have no latency to
     model, so we are always ready. That is a difference a guest can only observe
     by timing, and it is in the direction that cannot hang anyone.
   ⚠ NON-DMA (bit 5) IS NEVER SET, because there is no execution phase yet to be
     in. It becomes real with the data commands. */
uint8_t vdd_fdc_msr(const fdc_state *st)
{
    uint8_t m;
    /* Held in reset: the chip is not ready for anything, and says so. A guest
       that writes 00h to DOR and then polls has hung a real machine too. */
    if (st->in_reset) return 0x00;
    m = FDC_MSR_RQM;
    if (st->phase == FDC_PHASE_RES) m |= FDC_MSR_DIO | FDC_MSR_CB;
    else if (st->cmd_len)           m |= FDC_MSR_CB;
    /* Bits 3:0 are "drive n is seeking". Our seeks complete inside the OUT that
       starts them, so no drive is ever mid-seek when software can look. */
    return m;
}

static void fdc_irq(fdc_state *st)
{
    st->irqs++;
    /* ⛔ DMAGATE IS NOT DECORATION. With DOR bit 3 clear the chip still works and
         the interrupt simply never reaches the PIC -- every operation then times
         out at the BIOS layer with nothing reporting a wrong value anywhere. A
         model that ignores the bit serves a driver that set it and silently
         disobeys one that cleared it. */
    if (!(st->dor & FDC_DOR_DMAGATE)) return;
    if (st->bus) VddRaiseIrq(st->bus, 6);
}

/* ── HOW MANY BYTES DOES THIS COMMAND TAKE? (docs/ref/fdc.md 5) ─────────────────
     Counting the opcode itself. The low five bits are the opcode; bits 7:5 are
     MT/MFM/SK modifiers and are not part of it. A command we do not know takes
     exactly one byte and answers 80h -- which is the documented reply and is also
     what keeps an unknown opcode from eating the next real command as a
     parameter. */
static uint8_t fdc_cmd_len(uint8_t op)
{
    switch (op & 0x1F) {
    case 0x03: return 3;        /* SPECIFY                                      */
    case 0x04: return 2;        /* SENSE DRIVE STATUS                           */
    case 0x07: return 2;        /* RECALIBRATE                                  */
    case 0x08: return 1;        /* SENSE INTERRUPT STATUS                       */
    case 0x0A: return 2;        /* READ ID                                      */
    case 0x0E: return 1;        /* DUMPREG                                      */
    case 0x0F: return 3;        /* SEEK                                         */
    case 0x10: return 1;        /* VERSION                                      */
    case 0x12: return 2;        /* PERPENDICULAR MODE                           */
    case 0x13: return 4;        /* CONFIGURE                                    */
    case 0x14: return 1;        /* LOCK                                         */
    /* ⚠ 09h IS WRITE DELETED DATA and it is easy to miss -- it sits in a gap
         between 08h and 0Ah and no detection routine ever issues it. Omitting it
         would have made it an "invalid command" that consumed ONE byte, and its
         eight parameters would then have been read as eight more commands.
       ⚠ 11h (SCAN EQUAL) is a µPD765 command that the 82077AA DOES NOT HAVE. We
         identify as a 90h part, so it must fall through to invalid. */
    case 0x02: case 0x05: case 0x06: case 0x09: case 0x0C:
        return 9;               /* READ TRACK / WRITE / READ / WR+RD DELETED    */
    case 0x0D: return 6;        /* FORMAT TRACK                                 */
    default:   return 1;        /* invalid                                      */
    }
}

static void fdc_result(fdc_state *st, const uint8_t *b, uint8_t n)
{
    uint8_t i;
    for (i = 0; i < n && i < sizeof(st->res); ++i) st->res[i] = b[i];
    st->res_len = i; st->res_pos = 0;
    /* A command with no result bytes is over the moment its last parameter
       lands: the host sees CMD BSY clear and DIO stay low. One with results is
       not over until they are read, which is what lets a driver drain a result
       of unknown length by watching CMD BSY. */
    st->phase   = i ? FDC_PHASE_RES : FDC_PHASE_CMD;
    st->cmd_len = 0;
}

static void fdc_invalid(fdc_state *st)
{
    uint8_t r = 0x80;           /* ST0 with the invalid-command interrupt code  */
    st->invalids++;
    fdc_result(st, &r, 1);
}

/* The seven result bytes every data command ends with: ST0 ST1 ST2 C H R N. */
static void fdc_data_result(fdc_state *st, uint8_t st0, uint8_t st1, uint8_t st2,
                            uint8_t c, uint8_t h, uint8_t r, uint8_t n)
{
    uint8_t b[7];
    b[0] = st0; b[1] = st1; b[2] = st2; b[3] = c; b[4] = h; b[5] = r; b[6] = n;
    fdc_result(st, b, 7);
}

/* ── THE RESET SEQUENCE. (docs/ref/fdc.md 8) ────────────────────────────────────
     Reached from DOR bit 2 going low-then-high, from DSR bit 7, and from power
     on. The data rate SURVIVES a software reset -- a driver is entitled not to
     re-select it -- and so does the CONFIGURE state if LOCK was set, which is the
     entire point of the LOCK command. */
static void fdc_soft_reset(fdc_state *st)
{
    st->resets++;
    st->phase = FDC_PHASE_CMD; st->cmd_len = 0; st->cmd_want = 0;
    st->res_len = 0; st->res_pos = 0;
    if (!st->locked) { st->cfg_byte2 = 0; st->cfg_pretrk = 0; st->perp = 0; }
    /* Drive polling: four sense-interrupts are now owed, one per drive. */
    st->poll_drive = st->poll_off ? 4 : 0;
    st->irq_pending = 0; st->st0_pending = 0;
    fdc_irq(st);
}

/* ── SENSE INTERRUPT STATUS -- the other half of every interrupt. ────────────────
     SEEK and RECALIBRATE have no result phase at all, so this is the only way to
     learn that they finished, and the chip keeps the reason until it is asked.
     Three cases, in priority order, and the third is as documented as the others:
     asked when nothing is pending, the answer is 80h, and that is how a driver
     discovers there is nothing pending. */
static void fdc_sense_interrupt(fdc_state *st)
{
    uint8_t b[2];
    if (st->irq_pending) {
        b[0] = st->st0_pending;
        b[1] = st->pcn[st->st0_pending & 3];
        st->irq_pending = 0;
        fdc_result(st, b, 2);
    } else if (st->poll_drive < 4) {
        b[0] = (uint8_t)(0xC0 | st->poll_drive);    /* ready changed, drive n   */
        b[1] = st->pcn[st->poll_drive];
        st->poll_drive++;
        fdc_result(st, b, 2);
    } else {
        fdc_invalid(st);        /* nothing pending: 80h, by the datasheet       */
    }
}

static void fdc_seek_done(fdc_state *st, uint8_t drive, uint8_t head)
{
    st->st0_pending = (uint8_t)(0x20 | (head << 2) | (drive & 3)); /* SEEK END  */
    st->irq_pending = 1;
    fdc_irq(st);
}

/* Every parameter byte has arrived: carry the command out. */
static void fdc_execute(fdc_state *st)
{
    uint8_t op = (uint8_t)(st->cmd[0] & 0x1F);
    uint8_t drive, head;

    st->cmds++;
    switch (op) {

    case 0x10: {                /* VERSION -- the detection command             */
        /* 90h says "enhanced 82077AA": CONFIGURE, LOCK and PERPENDICULAR MODE
           exist. MEASURED at 90h on both oracles, one of them a real AMI BIOS. */
        uint8_t r = 0x90;
        fdc_result(st, &r, 1);
        break;
    }

    case 0x08:                  /* SENSE INTERRUPT STATUS                       */
        fdc_sense_interrupt(st);
        break;

    case 0x03:                  /* SPECIFY -- step rate, head load, non-DMA     */
        st->srt_hut = st->cmd[1];
        st->hlt_nd  = st->cmd[2];
        fdc_result(st, 0, 0);   /* no result phase, and no interrupt            */
        break;

    case 0x13:                  /* CONFIGURE                                    */
        st->cfg_byte2  = st->cmd[2];
        st->cfg_pretrk = st->cmd[3];
        /* Bit 4 of byte 2 is POLL: SET means drive polling is DISABLED. */
        st->poll_off   = (uint8_t)((st->cmd[2] & 0x10) ? 1 : 0);
        fdc_result(st, 0, 0);
        break;

    case 0x12:                  /* PERPENDICULAR MODE                           */
        st->perp = st->cmd[1];
        fdc_result(st, 0, 0);
        break;

    case 0x14: {                /* LOCK -- bit 7 of the opcode is the new value */
        st->locked = (uint8_t)((st->cmd[0] & 0x80) ? 1 : 0);
        { uint8_t r = (uint8_t)(st->locked ? 0x10 : 0x00);
          fdc_result(st, &r, 1); }
        break;
    }

    case 0x04: {                /* SENSE DRIVE STATUS -> ST3                    */
        uint8_t r;
        drive = (uint8_t)(st->cmd[1] & 3);
        head  = (uint8_t)((st->cmd[1] >> 2) & 1);
        /* Bit 5 READY is always 1 on the 82077AA; bit 3 TWO SIDE is 1 for the
           only drive we advertise; bit 4 TRACK0 is a fact about where the head
           is; bit 6 WRITE PROTECTED is 0 because the image handle is opened for
           writing (INT 13h writes through it). */
        r = (uint8_t)(0x28 | (head << 2) | drive);
        if (!st->pcn[drive]) r |= 0x10;
        fdc_result(st, &r, 1);
        break;
    }

    case 0x0E: {                /* DUMPREG -- ten bytes, no side effects        */
        uint8_t b[10];
        b[0] = st->pcn[0]; b[1] = st->pcn[1];
        b[2] = st->pcn[2]; b[3] = st->pcn[3];
        b[4] = st->srt_hut; b[5] = st->hlt_nd;
        b[6] = st->last_eot;
        b[7] = (uint8_t)((st->locked ? 0x80 : 0) | (st->perp & 0x7F));
        b[8] = st->cfg_byte2;
        b[9] = st->cfg_pretrk;
        fdc_result(st, b, 10);
        break;
    }

    case 0x07:                  /* RECALIBRATE -- seek to cylinder 0            */
        drive = (uint8_t)(st->cmd[1] & 3);
        st->pcn[drive] = 0;
        fdc_result(st, 0, 0);   /* no result phase: the interrupt is the report */
        fdc_seek_done(st, drive, 0);
        break;

    case 0x0F:                  /* SEEK                                         */
        drive = (uint8_t)(st->cmd[1] & 3);
        head  = (uint8_t)((st->cmd[1] >> 2) & 1);
        st->pcn[drive] = st->cmd[2];
        fdc_result(st, 0, 0);
        fdc_seek_done(st, drive, head);
        break;

    case 0x0A:                  /* READ ID -- what is under the head right now  */
        drive = (uint8_t)(st->cmd[1] & 3);
        head  = (uint8_t)((st->cmd[1] >> 2) & 1);
        fdc_data_result(st, (uint8_t)((head << 2) | drive), 0, 0,
                        st->pcn[drive], head, 1, 2);
        st->st0_pending = (uint8_t)((head << 2) | drive);
        st->irq_pending = 1;
        fdc_irq(st);
        break;

    /* ── THE DATA COMMANDS. RECOGNISED, AND HONESTLY FAILED. ──────────────────
         ⛔ THIS IS **PART** AND IT IS MARKED PART. They need the 8237A pointed at
           the image handle INT 13h already holds, which is the next step. Until
           then they terminate the way a real controller terminates over an
           unformatted track: ST0 interrupt code 01 (abnormal termination) with
           ST1 bit 2 (no data -- sector not found), the full seven result bytes,
           and the interrupt a real one would raise.
         ⚠ The alternative -- answering 80h, "invalid command" -- would be a
           different and worse lie: it says the CONTROLLER does not know the
           command, on a chip that has just identified itself as a 90h part that
           by definition does. A driver can act on a media error. It cannot act on
           a controller that contradicts itself. */
    case 0x02: case 0x05: case 0x06: case 0x09: case 0x0C: {
        uint8_t c, h, r, n;
        drive = (uint8_t)(st->cmd[1] & 3);
        head  = (uint8_t)((st->cmd[1] >> 2) & 1);
        c = st->cmd[2]; h = st->cmd[3]; r = st->cmd[4]; n = st->cmd[5];
        st->last_eot = st->cmd[6];
        fdc_data_result(st, (uint8_t)(0x40 | (head << 2) | drive), 0x04, 0x00,
                        c, h, r, n);
        st->st0_pending = (uint8_t)(0x40 | (head << 2) | drive);
        st->irq_pending = 1;
        fdc_irq(st);
        break;
    }

    case 0x0D:                  /* FORMAT TRACK -- same, and never destructive  */
        drive = (uint8_t)(st->cmd[1] & 3);
        head  = (uint8_t)((st->cmd[1] >> 2) & 1);
        st->last_eot = st->cmd[3];
        fdc_data_result(st, (uint8_t)(0x40 | (head << 2) | drive), 0x04, 0x00,
                        st->pcn[drive], head, 1, st->cmd[2]);
        st->st0_pending = (uint8_t)(0x40 | (head << 2) | drive);
        st->irq_pending = 1;
        fdc_irq(st);
        break;

    default:
        fdc_invalid(st);
        break;
    }
}

/* ── THE FIFO, WHICH IS THE WHOLE PROTOCOL. ─────────────────────────────────── */
static void fdc_fifo_write(fdc_state *st, uint8_t v)
{
    /* Writing while a result is waiting is a protocol error by the host. A real
       part ignores it; so do we, rather than letting it become the first byte of
       a command the driver never issued. */
    if (st->phase == FDC_PHASE_RES) return;
    if (!st->cmd_len) st->cmd_want = fdc_cmd_len(v);
    if (st->cmd_len < sizeof(st->cmd)) st->cmd[st->cmd_len] = v;
    st->cmd_len++;
    if (st->cmd_len >= st->cmd_want) fdc_execute(st);
}

static uint8_t fdc_fifo_read(fdc_state *st)
{
    uint8_t v;
    /* Reading when there is nothing to read hands back the last byte on a real
       part. Returning 0xFF here would re-create the very ambiguity this device
       exists to remove. */
    if (st->phase != FDC_PHASE_RES) return st->res_len ? st->res[st->res_len - 1] : 0x80;
    v = st->res[st->res_pos++];
    if (st->res_pos >= st->res_len) {
        /* The last result byte has been read: CMD BSY clears and the chip is
           idle again. This edge is what a length-free drain watches for.
           ⚠ res_len is NOT cleared, so a stray read past the end hands back the
             last byte the way a real part does, rather than 80h -- which would
             look like an invalid-command reply to something that had simply read
             one byte too many. */
        st->phase = FDC_PHASE_CMD; st->res_pos = 0;
    }
    return v;
}

void vdd_fdc_out(void *self, uint16_t port, uint8_t width, uint32_t val)
{
    fdc_state *st = (fdc_state *)self;
    uint8_t v = (uint8_t)val;
    (void)width;
    switch (port) {
    case FDC_DOR: {
        uint8_t was = st->dor;
        st->dor = v;
        /* ⛔ BIT 2 IS ACTIVE LOW AND IT IS THE SOFTWARE RESET EVERY BIOS USES.
             A plausible-looking "turn everything off" of 00h holds the chip in
             reset, ungates the interrupt AND stops the motors, all at once. */
        if (!(v & FDC_DOR_NRESET)) {
            st->in_reset = 1;
        } else if (st->in_reset || !(was & FDC_DOR_NRESET)) {
            st->in_reset = 0;
            fdc_soft_reset(st);
        }
        break;
    }
    case FDC_TDR:  st->tdr = v; break;
    case FDC_MSR:                               /* the WRITE side is DSR        */
        st->dsr = v;
        if (v & 0x80) {                         /* bit 7: software reset        */
            /* Self-clearing: the datasheet's reset bit resets and releases. The
               data rate in bits 1:0 SURVIVES, which is why a driver may reset
               through this door and not re-select it. */
            st->dsr = (uint8_t)(v & 0x7F);
            st->in_reset = 0;
            fdc_soft_reset(st);
        }
        break;
    case FDC_FIFO: fdc_fifo_write(st, v); break;
    case FDC_DIR:  st->ccr = (uint8_t)(v & 0x03); break;  /* the write side is CCR */
    default: break;
    }
}

void vdd_fdc_in(void *self, uint16_t port, uint8_t width, uint32_t *val)
{
    fdc_state *st = (fdc_state *)self;
    (void)width;
    switch (port) {
    case FDC_DOR:  *val = st->dor; break;       /* readable on an 82077AA       */
    case FDC_TDR:  *val = st->tdr; break;
    case FDC_MSR:  *val = vdd_fdc_msr(st); break;
    case FDC_FIFO: *val = fdc_fifo_read(st); break;
    case FDC_DIR:
        /* ── DIR BIT 7 IS DSKCHG, AND WE ANSWER 0. ────────────────────────────
             The medium behind us is a file that cannot be swapped while the VDM
             runs, so "the door has been opened" is false -- and it is the same
             truthful claim INT 13h AH=15h already makes through the other door
             ("floppy without change-line support"). One medium, two doors, one
             answer, which is the rule the A20 gate and the RTC both arrived at.
           ⚠ BITS 6:0 ARE NOT ADJUDICABLE. A PC/AT-mode part does not drive them;
             our two oracles both DO drive them and disagree (6.22/QEMU 00h, PCem
             01h), so there is no answer to copy. Zero is recorded as a choice,
             not measured. */
        *val = 0x00;
        break;
    default: *val = 0xFF; break;
    }
}

void vdd_fdc_reset(void *self)
{
    fdc_state *st = (fdc_state *)self;
    VDD_BUS *bus = st->bus;
    unsigned i;
    for (i = 0; i < sizeof(*st); ++i) ((uint8_t *)st)[i] = 0;
    st->bus = bus;
    /* ── WHAT POST LEAVES. ───────────────────────────────────────────────────
         Out of reset, DMA and interrupts gated through, drive 0 selected, motors
         off: DOR = 0Ch. MEASURED on 6.22/QEMU, which reads exactly 0Ch. PCem's
         AMI BIOS leaves 0Fh -- the same two chip bits with drive 3 selected --
         so bits 3:2 are the adjudicable part and both oracles agree on them.
       ⚠ The BIOS's own SPECIFY values are not guessed here. They arrive through
         the port when a driver programs them, and DUMPREG hands back what
         arrived rather than an invented default. */
    st->dor      = FDC_DOR_NRESET | FDC_DOR_DMAGATE;    /* 0x0C                 */
    st->in_reset = 0;
    st->phase    = FDC_PHASE_CMD;
    /* A machine that has been through POST has already had its four
       sense-interrupts collected by the BIOS. Starting at 4 means the first
       thing a guest asks is answered "nothing pending" rather than with three
       phantom drives that changed while it was not looking. */
    st->poll_drive = 4;
}

int vdd_fdc_init(VDD_BUS *b, void *self)
{
    fdc_state *st = (fdc_state *)self;
    st->bus = b;
    if (!st->dor) vdd_fdc_reset(st);            /* the host builds us zeroed    */
    st->bus = b;
    /* ── TWO CLAIMS, AND THE GAPS ARE DELIBERATE. ────────────────────────────
         3F0h/3F1h (SRA/SRB) are driven ONLY by a part strapped for PS/2 mode. We
         present a PC/AT machine, so they are left unclaimed and float to FFh --
         which is what 6.22/QEMU AND PCem both report for 3F0h. (They both drive
         3F1h and disagree about its value, C1h against 51h, so there is nothing
         there to copy either.)
       ⛔ 3F6h IS NOT OURS AT ALL. The FDC does not decode offset 6; on a PC/AT it
         is the hard-disk controller's alternate status register, and both oracles
         answer 50h there (DRDY|DSC) from their IDE side while we answer FFh.
         THAT IS A REAL GAP AND IT BELONGS TO THE ATA SURFACE, NOT THIS ONE.
         Claiming the whole eight-port block would have "fixed" the row by taking
         a register that is somebody else's. */
    if (VddClaimPorts(b, FDC_DOR, FDC_FIFO, vdd_fdc_in, vdd_fdc_out, st)) return -1;
    if (VddClaimPorts(b, FDC_DIR, FDC_DIR,  vdd_fdc_in, vdd_fdc_out, st)) return -1;
    return 0;
}
