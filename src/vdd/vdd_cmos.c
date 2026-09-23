/* vdd_cmos.c -- see vdd_cmos.h.  MC146818 RTC + CMOS RAM on the VDD bus.  Pure C. */
#include "vdd_cmos.h"

static uint8_t bcd(unsigned v) { return (uint8_t)(((v / 10) % 10) * 16 + (v % 10)); }

/* ── THE CLOCK REGISTERS ARE DERIVED, NOT STORED. ────────────────────────────
     Keeping a copy and ticking it would be a second clock to drift from the one
     INT 1Ah reads. docs/ref/rtc.md 2: a PC BIOS leaves Status B with DM=0 and
     24/12=1, i.e. **BCD, 24-hour**, and every DOS program that reads this chip
     by hand assumes exactly that -- so 0x12 in the hours register is twelve
     o'clock, not eighteen. */
static int cmos_clock_reg(cmos_state *st, uint8_t reg, uint8_t *out)
{
    struct vdd_rtc n;
    if (!st->rtc_now) return 0;
    n.cent = 20; n.year = 0; n.month = 1; n.day = 1;
    n.hour = 0;  n.min = 0;  n.sec = 0;
    st->rtc_now(st->rtc_ctx, &n);
    switch (reg) {
    case CMOS_SEC:     *out = bcd(n.sec);   return 1;
    case CMOS_MIN:     *out = bcd(n.min);   return 1;
    case CMOS_HOUR:    *out = bcd(n.hour);  return 1;
    case CMOS_DOM:     *out = bcd(n.day);   return 1;
    case CMOS_MONTH:   *out = bcd(n.month); return 1;
    case CMOS_YEAR:    *out = bcd(n.year);  return 1;
    case CMOS_CENTURY: *out = bcd(n.cent);  return 1;
    /* ⚠ THE DAY OF WEEK IS NOT DERIVED AND THE REASON IS WORTH RECORDING. The
         host's clock reading (struct vdd_rtc) does not carry one, and computing
         it here would mean a calendar rule in a device model. It reads from
         `ram`, where reset leaves 1 (Sunday) -- a fixed, wrong-six-days-in-seven
         answer. Nothing on the shelf reads it; recorded in inventory/rtc.md
         rather than quietly derived from an algorithm nobody checked. */
    default: return 0;
    }
}

static uint8_t cmos_read(cmos_state *st, uint8_t reg)
{
    uint8_t v;
    if (cmos_clock_reg(st, reg, &v)) return v;
    switch (reg) {
    /* ── STATUS A. BIT 7 IS **UIP** AND WE REPORT IT CLEAR, ALWAYS. ──────────
         The chip sets UIP for ~2 ms once a second while it updates its own
         registers, and software waits for it to fall before reading the time.
         We have no such window -- the registers are derived from the host clock
         at the instant of the read, so they are never mid-update -- and
         answering "never busy" is therefore TRUE of this model rather than a
         convenient lie. Bits 6:4 = 010 is the normal 32.768 kHz divider setting,
         bits 3:0 the periodic rate the BIOS leaves. */
    case CMOS_STATUS_A: return 0x26;
    /* ── STATUS B: BCD, 24-HOUR. ★ MEASURED 0x02 on 6.22-under-QEMU AND on
         PCem's real AMI BIOS; dosbox-x answers 0x03, which is the same plus
         DSE (daylight saving). Two of three, including the period-correct
         machine, and 0x02 is what the bit definitions say a PC leaves. */
    case CMOS_STATUS_B: return 0x02;
    /* ── STATUS C IS CLEARED BY BEING READ. ─────────────────────────────────
         That is how IRQ8 is acknowledged at the chip: the flags latch and the
         read clears them, so a handler that does not read 0Ch gets exactly one
         interrupt and then silence. We raise no IRQ8 yet, so the flags are
         always 0 -- but the CLEAR-ON-READ contract is implemented anyway,
         because it is the part a driver's logic is built on. */
    case CMOS_STATUS_C: { uint8_t c = st->status_c; st->status_c = 0; return c; }
    /* ── STATUS D BIT 7 IS VRT, "valid RAM and time". CLEAR means "the battery
         died and everything in here is garbage", which firmware and setup
         programs act on. With no chip at all we answered 0xFF, whose bit 7 is
         set -- so this row was RIGHT BY ACCIDENT and agreed with all three
         oracles. It is now right on purpose. */
    case CMOS_STATUS_D: return 0x80;
    default: break;
    }
    return st->ram[reg & 0x7F];
}

static void cmos_out(void *self, uint16_t port, uint8_t w, uint32_t v)
{
    cmos_state *st = (cmos_state *)self;
    uint8_t b = (uint8_t)v;
    (void)w;
    if (port == 0x70) {
        /* Bit 7 is the NMI mask, not part of the register number -- see the note
           in the header. Counted, because a run should be able to say whether a
           guest masks NMI, and not acted on, because we have no NMI to mask. */
        if (b & 0x80) st->nmi_mask_writes++;
        st->nmi_disabled = (uint8_t)((b & 0x80) ? 1 : 0);
        st->index = (uint8_t)(b & 0x7F);
        return;
    }
    /* Port 0x71 write. The clock registers and the status registers are derived
       or read-only here; everything else is battery-backed RAM and takes it.
       ⚠ WE DO NOT LET A GUEST SET THE CLOCK. Writing 00h-09h would have to move
         the HOST's clock, which we cannot do -- and accepting the write while
         changing nothing is the "runs but lies" shape this project rules out.
         Same reasoning as INT 1Ah AH=03h/05h, which are deliberately not
         answered (see vdd_pit.c). */
    if (st->index <= CMOS_STATUS_D) return;
    st->ram[st->index & 0x7F] = b;
}

static void cmos_in(void *self, uint16_t port, uint8_t w, uint32_t *val)
{
    cmos_state *st = (cmos_state *)self;
    (void)w;
    /* ⚠ PORT 0x70 IS WRITE-ONLY ON THE PART and a read of it is undefined.
         Answer consistently rather than plausibly -- 0xFF is what an undriven
         bus gives, and it is what this port gave before anything claimed it. */
    if (port == 0x70) { *val = 0xFF; return; }
    *val = cmos_read(st, st->index);
}

void vdd_cmos_reset(void *self)
{
    cmos_state *st = (cmos_state *)self;
    vdd_bus *bus = st->bus;
    void (*now)(void *, struct vdd_rtc *) = st->rtc_now;
    void *ctx = st->rtc_ctx;
    unsigned i;
    for (i = 0; i < sizeof(*st); ++i) ((uint8_t *)st)[i] = 0;
    st->bus = bus; st->rtc_now = now; st->rtc_ctx = ctx;
    /* ── WHAT POST LEAVES IN THE CMOS. (docs/ref/rtc.md 3) ───────────────────
         These are BIOS conventions, not chip behaviour, and they are here
         because a machine DOS is running on has been through POST -- the same
         reasoning as the 8042's status register starting with SYS set.
       ⚠ THE EQUIPMENT BYTE'S LOW NIBBLE IS NOT ADJUDICABLE. p_rtc measured 6 on
         QEMU, 7 on dosbox-x and 0x0D on PCem: it describes the MACHINE's video
         type and coprocessor, so three machines legitimately give three answers.
         0x05 = colour 80x25, coprocessor present, which is what we present. */
    st->ram[CMOS_DOW]   = 0x01;              /* see the note in cmos_clock_reg */
    st->ram[0x0E]       = 0x00;              /* POST diagnostic: no errors     */
    st->ram[0x10]       = 0x40;              /* one 1.44M floppy               */
    st->ram[CMOS_EQUIP] = 0x25;              /* 1 floppy, colour 80x25, FPU    */
    st->ram[0x15]       = 0x80; st->ram[0x16] = 0x02;   /* 640 KB base memory  */
}

int vdd_cmos_init(vdd_bus *b, void *self)
{
    cmos_state *st = (cmos_state *)self;
    st->bus = b;
    if (!st->ram[CMOS_EQUIP]) vdd_cmos_reset(st);   /* the host builds us zeroed */
    st->bus = b;
    if (vdd_claim_ports(b, 0x70, 0x71, cmos_in, cmos_out, st)) return -1;
    return 0;
}
