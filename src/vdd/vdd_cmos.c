/* vdd_cmos.c -- see vdd_cmos.h.  MC146818 RTC + CMOS RAM on the VDD bus.  Pure C. */
#include "vdd_cmos.h"

static uint8_t bcd(unsigned v) { return (uint8_t)(((v / 10) % 10) * 16 + (v % 10)); }

/* A clock field, in whichever base Status B bit 2 (DM) selects.
   ⚠ HONOURING DM MATTERS MORE THAN IT LOOKS. A PC BIOS leaves DM=0 (BCD) and
     nothing on the shelf changes it -- but a model that ignores the bit hands a
     guest that DID set binary mode a BCD byte, and 0x59 seconds then reads as
     eighty-nine. Refusing the write would be the "runs but lies" shape; honouring
     it is two lines. */
static uint8_t clkval(const cmos_state *st, unsigned v)
{ return (st->status_b & 0x04) ? (uint8_t)v : bcd(v); }

/* The other direction: a byte the guest wrote, in the base DM says it is in. */
static unsigned clkdec(const cmos_state *st, uint8_t v)
{ return (st->status_b & 0x04) ? v : (unsigned)(v >> 4) * 10u + (v & 0x0Fu); }

/* The clock as the chip shows it now: the frozen copy while SET is held. */
static int cmos_reading(cmos_state *st, struct vdd_rtc *n)
{
    if (st->set_held) { *n = st->shadow; return 1; }
    if (!st->rtc_now) return 0;
    n->cent = 20; n->year = 0; n->month = 1; n->day = 1;
    n->hour = 0;  n->min = 0;  n->sec = 0;  n->dow = 0;
    st->rtc_now(st->rtc_ctx, n);
    return 1;
}

/* ── THE PERIODIC RATE, FROM STATUS A BITS 3:0. (docs/ref/rtc.md 2) ──────────
     0 selects none. 1 and 2 are special cases at 256 Hz and 128 Hz; from 3 up
     the rate is 32768 >> (RS - 1), so RS=3 is 8192 Hz, RS=6 is 1024 Hz (what a
     PC BIOS leaves) and RS=15 is 2 Hz. */
uint32_t vdd_cmos_periodic_hz(const cmos_state *st)
{
    unsigned rs = st->status_a & 0x0F;
    if (!rs) return 0;
    if (rs == 1) return 256;
    if (rs == 2) return 128;
    return 32768u >> (rs - 1);
}

static int cmos_clock_reg(cmos_state *st, uint8_t reg, uint8_t *out);

/* ── THE ALARM MATCH RULE, WHICH IS NOT "EQUAL". ─────────────────────────────
     An alarm register whose top two bits are both set (>= 0xC0) is a DON'T CARE
     and matches anything -- that is how "every minute at 30 seconds" is
     programmed, and a model that only compares for equality can never express
     it. (MC146818 datasheet, the alarm registers.) */
static int alarm_field_matches(uint8_t alarm, uint8_t now)
{ return (alarm & 0xC0) == 0xC0 || alarm == now; }

/* One second has passed: raise the update-ended flag, and the alarm flag if the
   clock has reached the programmed time. Returns non-zero if either was set. */
static int cmos_second_edge(cmos_state *st)
{
    int fired = 0;
    if (st->set_held) return 0;                 /* SET: updates are inhibited  */
    if (st->status_b & 0x10) {                  /* UIE: update ended           */
        st->status_c |= 0x10; st->uf_raised++; fired = 1;
    }
    if (st->status_b & 0x20) {                  /* AIE: alarm                  */
        uint8_t h, m, sec;
        if (cmos_clock_reg(st, CMOS_SEC, &sec) &&
            cmos_clock_reg(st, CMOS_MIN, &m) &&
            cmos_clock_reg(st, CMOS_HOUR, &h) &&
            alarm_field_matches(st->ram[0x01], sec) &&
            alarm_field_matches(st->ram[0x03], m) &&
            alarm_field_matches(st->ram[0x05], h)) {
            st->status_c |= 0x20; st->af_raised++; fired = 1;
        }
    }
    return fired;
}

void vdd_cmos_add_clocks(cmos_state *st, uint32_t clocks)
{
    uint32_t hz, period;
    int guard = 0;
    /* ── THE ONCE-A-SECOND EDGE, for UF and AF. Accumulated from the same clocks
         the periodic divider uses rather than polled off the host clock, so
         rtc_now is called once a SECOND instead of once a pacer tick. */
    if (st->status_b & 0x30) {                  /* UIE or AIE enabled          */
        st->sec_accum += clocks;
        while (st->sec_accum >= PIT_INPUT_HZ && guard++ < 1000) {
            st->sec_accum -= PIT_INPUT_HZ;
            if (cmos_second_edge(st)) {
                st->status_c |= 0x80;           /* IRQF: something is pending  */
                if (st->bus) vdd_raise_irq(st->bus, 8);
            }
        }
    } else {
        st->sec_accum = 0;
    }
    guard = 0;
    /* Dormant unless the guest asked for it -- see the note in the header. Doing
       nothing at all (rather than accumulating) means enabling PIE later starts
       from now rather than replaying a backlog of ticks nobody was listening for. */
    if (!(st->status_b & 0x40)) { st->pf_accum = 0; return; }
    hz = vdd_cmos_periodic_hz(st);
    if (!hz) { st->pf_accum = 0; return; }
    period = PIT_INPUT_HZ / hz;                 /* PIT-rate clocks per periodic tick */
    if (!period) return;
    st->pf_accum += clocks;
    while (st->pf_accum >= period && guard++ < 100000) {
        st->pf_accum -= period;
        /* PF, and IRQF because an interrupt is now pending. Both are cleared by a
           read of Status C -- which is how a handler acknowledges the chip. */
        st->status_c |= 0xC0;
        st->pf_raised++;
        if (st->bus) vdd_raise_irq(st->bus, 8);
    }
}

/* ── THE CLOCK REGISTERS ARE DERIVED, NOT STORED. ────────────────────────────
     Keeping a copy and ticking it would be a second clock to drift from the one
     INT 1Ah reads. docs/ref/rtc.md 2: a PC BIOS leaves Status B with DM=0 and
     24/12=1, i.e. **BCD, 24-hour**, and every DOS program that reads this chip
     by hand assumes exactly that -- so 0x12 in the hours register is twelve
     o'clock, not eighteen. */
static int cmos_clock_reg(cmos_state *st, uint8_t reg, uint8_t *out)
{
    struct vdd_rtc n;
    if (!cmos_reading(st, &n)) return 0;
    switch (reg) {
    case CMOS_SEC:     *out = clkval(st, n.sec);   return 1;
    case CMOS_MIN:     *out = clkval(st, n.min);   return 1;
    case CMOS_HOUR: {
        /* ⚠ 12-HOUR MODE IS NOT "SUBTRACT TWELVE". Status B bit 1 clear selects
             it, and then BIT 7 OF THIS REGISTER IS PM -- midnight is 12 AM and
             noon is 12 PM, neither of which is hour 0. A model that ignores the
             bit tells a 12-hour guest that 14:00 is 2 AM. */
        unsigned h = n.hour;
        uint8_t pm = 0;
        if (!(st->status_b & 0x02)) {
            pm = (uint8_t)(h >= 12 ? 0x80 : 0x00);
            h %= 12; if (!h) h = 12;
        }
        *out = (uint8_t)(clkval(st, h) | pm);
        return 1; }
    /* ⚠ 01h, 03h and 05h -- the ALARM registers -- are deliberately NOT claimed
         here. They are storage the guest owns, not a view of the host clock, so
         they fall through to ram[] on both the read and the write paths. */
    case CMOS_DOM:     *out = clkval(st, n.day);   return 1;
    case CMOS_MONTH:   *out = clkval(st, n.month); return 1;
    case CMOS_YEAR:    *out = clkval(st, n.year);  return 1;
    case CMOS_CENTURY: *out = clkval(st, n.cent);  return 1;
    /* ── THE DAY OF WEEK COMES FROM THE HOST, NOT A CALENDAR RULE. (s81, #182) It
         was fixed at 1 (Sunday) because the clock reading carried no weekday and a
         device model should not hold calendar arithmetic. Windows' GetLocalTime
         already knows it, so the host passes it through (1 = Sunday, the chip's
         numbering); a reader with no weekday (0) still falls back to ram[]. */
    case CMOS_DOW:
        if (!n.dow) return 0;
        *out = clkval(st, n.dow); return 1;
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
         convenient lie -- and it is why bit 7 is masked off on the WRITE path
         instead of being stored: UIP is the chip telling software when it may
         read, never software telling the chip anything. */
    case CMOS_STATUS_A: return st->status_a;
    /* ── STATUS B: BCD, 24-HOUR. ★ MEASURED 0x02 on 6.22-under-QEMU AND on
         PCem's real AMI BIOS; dosbox-x answers 0x03, which is the same plus
         DSE (daylight saving). Two of three, including the period-correct
         machine, and 0x02 is what the bit definitions say a PC leaves. */
    case CMOS_STATUS_B: return st->status_b;
    /* ── STATUS C IS CLEARED BY BEING READ. ─────────────────────────────────
         That is how IRQ8 is acknowledged at the chip: the flags latch and the
         read clears them, so a handler that does not read 0Ch gets exactly one
         interrupt and then silence. vdd_cmos_add_clocks sets PF and IRQF here. */
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
    /* Port 0x71 write. The clock registers move the VDM's own RTC (GH #261, below
       -- an offset from the host's, src/dos/dos_clock.h; the machine's clock never
       moves); the status registers are the guest's control bits or read-only;
       everything else is battery-backed RAM and takes it. */
    if (st->index == CMOS_STATUS_A) {
        /* Bit 7 is UIP and is READ-ONLY -- it is the chip telling software when
           it may read, not software telling the chip anything. */
        st->status_a = (uint8_t)(b & 0x7F);
        return;
    }
    if (st->index == CMOS_STATUS_B) {
        /* ── GH #261: SET (bit 7). Going high freezes a copy of the clock and,
             per the MC146818 datasheet, CLEARS UIE -- no update-ended interrupt
             for a clock that is not updating. Going low commits the copy: the
             date, then the time, through the same hook INT 1Ah AH=05h/03h use. */
        if ((b & 0x80) && !st->set_held) {
            struct vdd_rtc n;
            if (cmos_reading(st, &n)) { st->shadow = n; st->set_held = 1; }
            b = (uint8_t)(b & ~0x10);
        } else if (!(b & 0x80) && st->set_held) {
            st->set_held = 0;
            if (st->rtc_set) {
                st->rtc_set(st->rtc_ctx, &st->shadow, 1);
                st->rtc_set(st->rtc_ctx, &st->shadow, 0);
            }
        }
        st->status_b = b;
        /* Disabling the periodic interrupt drops any part-accumulated tick, so
           re-enabling it starts from now rather than firing immediately. */
        if (!(b & 0x40)) st->pf_accum = 0;
        return;
    }
    /* ⛔ THE ALARM REGISTERS ARE NOT THE CLOCK, AND BLANKET-REFUSING THEM WAS A
         DEFECT I INTRODUCED. 01h, 03h and 05h are the seconds/minutes/hours
         ALARM, and writing them is the only way to set an alarm at all -- so
         "everything below 0x0E is read-only" made the alarm interrupt
         unreachable in the same way refusing Status B made the periodic one
         unreachable. The rule is not "low registers are read-only"; it is
         "WE CANNOT MOVE THE HOST'S CLOCK", and that applies to 00/02/04 and to
         the date, not to a comparison value the guest owns. */
    if (st->index == 0x01 || st->index == 0x03 || st->index == 0x05) {
        st->ram[st->index] = b;
        return;
    }
    /* ── ★ GH #261: THE CLOCK ITSELF. With a host hook (rtc_set) a write moves
         the VDM's RTC -- frozen copy while SET is held, committed at once when
         it is not. The byte is decoded in the base DM selects, and the hours
         register in the mode bit 1 selects (12-hour: bit 7 is PM).
       ⚠ THE DAY OF WEEK (06h) STAYS REFUSED: the VDM's weekday is derived from
         its date, and a free-running counter the guest can set to anything has
         no place in an offset. Without the hook every clock write is refused. */
    if (st->index == CMOS_SEC || st->index == CMOS_MIN || st->index == CMOS_HOUR ||
        st->index == CMOS_DOM || st->index == CMOS_MONTH || st->index == CMOS_YEAR ||
        st->index == CMOS_CENTURY) {
        struct vdd_rtc n;
        int date = st->index >= CMOS_DOM;
        if (!st->rtc_set || !cmos_reading(st, &n)) return;
        switch (st->index) {
        case CMOS_SEC:   n.sec = clkdec(st, b); break;
        case CMOS_MIN:   n.min = clkdec(st, b); break;
        case CMOS_HOUR:
            if (st->status_b & 0x02) n.hour = clkdec(st, b);
            else { unsigned h = clkdec(st, (uint8_t)(b & 0x7F)) % 12u;
                   n.hour = h + ((b & 0x80) ? 12u : 0u); }
            break;
        case CMOS_DOM:   n.day   = clkdec(st, b); break;
        case CMOS_MONTH: n.month = clkdec(st, b); break;
        case CMOS_YEAR:  n.year  = clkdec(st, b); break;
        default:         n.cent  = clkdec(st, b); break;
        }
        if (st->set_held) st->shadow = n;
        else st->rtc_set(st->rtc_ctx, &n, date);
        return;
    }
    if (st->index <= CMOS_STATUS_D) return;     /* DOW + Status C/D: read-only */
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
    int  (*set)(void *, const struct vdd_rtc *, int) = st->rtc_set;
    void *ctx = st->rtc_ctx;
    uint16_t base_kb = st->base_kb;                       /* #136: preserved, as above */
    unsigned i;
    for (i = 0; i < sizeof(*st); ++i) ((uint8_t *)st)[i] = 0;
    st->bus = bus; st->rtc_now = now; st->rtc_set = set; st->rtc_ctx = ctx;
    st->base_kb = base_kb;
    /* ── WHAT POST LEAVES IN THE CMOS. (docs/ref/rtc.md 3) ───────────────────
         These are BIOS conventions, not chip behaviour, and they are here
         because a machine DOS is running on has been through POST -- the same
         reasoning as the 8042's status register starting with SYS set.
       ⚠ THE EQUIPMENT BYTE'S LOW NIBBLE IS NOT ADJUDICABLE. p_rtc measured 6 on
         QEMU, 7 on dosbox-x and 0x0D on PCem: it describes the MACHINE's video
         type and coprocessor, so three machines legitimately give three answers.
         0x05 = colour 80x25, coprocessor present, which is what we present. */
    /* ⚠ THESE TWO ARE WHAT A PC BIOS LEAVES, and Status B's value was MEASURED:
         0x02 on 6.22-under-QEMU and on PCem's real AMI BIOS alike -- BCD, 24-hour,
         and every interrupt DISABLED. Status A's 0x26 is the normal 32.768 kHz
         divider with the BIOS's usual 1024 Hz rate select, which is inert while
         PIE is clear. */
    st->status_a = 0x26;
    st->status_b = 0x02;
    st->ram[CMOS_DOW]   = 0x01;              /* see the note in cmos_clock_reg */
    st->ram[0x0E]       = 0x00;              /* POST diagnostic: no errors     */
    st->ram[0x10]       = 0x40;              /* one 1.44M floppy               */
    st->ram[CMOS_EQUIP] = 0x25;              /* 1 floppy, colour 80x25, FPU    */
    /* Base memory FITTED: 640 KB (0280h) unless Settings > Conventional Memory says less
       (#136). The EBDA and INT 12h's 639 are carved out of this by the BIOS, not here. */
    {   uint16_t kb = st->base_kb ? st->base_kb : 640u;
        st->ram[0x15] = (uint8_t)(kb & 0xFF); st->ram[0x16] = (uint8_t)(kb >> 8); }
    /* ── EXTENDED MEMORY, AS POST WOULD HAVE COUNTED IT (s81, #182). 17h/18h are the
         configured and 30h/31h the POST-detected KB above 1 MB; a real BIOS answers
         INT 15h AH=88h from the latter. Ours answers 88h with 0x3C00 (15 MB, main.c),
         so CMOS says the same -- two views of one machine must not disagree. */
    st->ram[0x17] = (uint8_t)(CMOS_EXT_KB & 0xFF); st->ram[0x18] = (uint8_t)(CMOS_EXT_KB >> 8);
    st->ram[0x30] = (uint8_t)(CMOS_EXT_KB & 0xFF); st->ram[0x31] = (uint8_t)(CMOS_EXT_KB >> 8);
    /* ── THE CHECKSUM OVER 10h-2Dh, WHICH A BIOS VERIFIES AT BOOT. ───────────
         A setup program that writes a configuration byte and does not fix this
         makes the BIOS declare the CMOS invalid next time. We are not that
         BIOS -- but leaving it zero means anything that DOES verify it decides
         our CMOS is corrupt, which is the same wrong answer Status D's VRT bit
         used to give. ⚠ Computed at reset only: a guest that writes into the
         range invalidates it, exactly as on a real machine. */
    { unsigned i, sum = 0;
      for (i = 0x10; i <= 0x2D; ++i) sum += st->ram[i];
      st->ram[0x2E] = (uint8_t)(sum >> 8);
      st->ram[0x2F] = (uint8_t)(sum & 0xFF); }
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
