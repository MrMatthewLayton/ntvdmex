/* vdd_gus.c -- the Gravis UltraSound (GF1). See vdd_gus.h, docs/ref/gus.md (§n below)
 * and docs/inventory/gus.md.
 *
 * Built from the UltraSound SDK v2.22 (manual Ch. 2 and the SDK's own driver source),
 * not from any application: a device is in because it is in the period hardware
 * contract. heaven7 is the acceptance test, not the specification.
 */
#include "vdd_gus.h"

/* ---- register-file helpers -------------------------------------------------------- */

/* Which registers are 16 bits wide (ref §2.1, §2.2). Everything else is 8 and lives at 3X5. */
static int gus_reg16(uint8_t r)
{
    uint8_t v;
    if (r == 0x42 || r == 0x43) return 1;
    if (r >= 0x40 && r < 0x80) return 0;             /* the other globals are 8-bit */
    v = (uint8_t)(r & 0x7F);                         /* voice regs: write 0xh, read 8xh */
    return v == 0x01 || (v >= 0x02 && v <= 0x05) || v == 0x09 || v == 0x0A || v == 0x0B;
}

/* A position in 1/512-sample units from the (high, low) register pair (ref §2.2):
   high bits 12-0 = address 19-7; low bits 15-9 = address 6-0 and, below that, the
   fraction -- four bits for start/end (8-5), nine for the current position (8-0). */
static uint32_t pos_set_hi(uint32_t pos, uint16_t hw)
{
    uint32_t addr = ((uint32_t)(hw & 0x1FFF) << 7) | ((pos >> 9) & 0x7F);
    return (addr << 9) | (pos & 0x1FF);
}
static uint32_t pos_set_lo(uint32_t pos, uint16_t lw, int nine_bit_frac)
{
    uint32_t addr = ((pos >> 9) & ~0x7Fu) | ((uint32_t)(lw >> 9) & 0x7F);
    uint32_t frac = nine_bit_frac ? (lw & 0x1FFu) : ((uint32_t)(lw >> 5) & 0xF) << 5;
    return (addr << 9) | frac;
}
static uint16_t pos_get_hi(uint32_t pos) { return (uint16_t)((pos >> 16) & 0x1FFF); }
static uint16_t pos_get_lo(uint32_t pos, int nine_bit_frac)
{
    uint16_t lw = (uint16_t)(((pos >> 9) & 0x7F) << 9);
    return (uint16_t)(lw | (nine_bit_frac ? (pos & 0x1FF) : (pos & 0x1E0)));
}

/* ---- the latches (ref §5) ---------------------------------------------------------- */

static const uint8_t k_irq_map[8] = { 0, 2, 5, 3, 7, 11, 12, 15 };
static const uint8_t k_dma_map[8] = { 0, 1, 3, 5, 6, 7, 0, 0 };
static uint8_t gus_irq_line(const gus_state *st)
{
    uint8_t l = k_irq_map[st->irq_latch & 7];
    return l ? l : st->irq;
}
static uint8_t gus_dma_line(const gus_state *st)
{
    uint8_t l = k_dma_map[st->dma_latch & 7];
    return l ? l : st->dma_ch;
}

/* ---- interrupts (ref §6) -------------------------------------------------------- */

static int gus_voice_pending(const gus_state *st)
{
    unsigned i;
    for (i = 0; i < GUS_VOICES; ++i)
        if ((st->v[i].ctrl & 0x80) || (st->v[i].vctrl & 0x80)) return 1;
    return 0;
}
static uint8_t gus_irq_status(const gus_state *st)     /* 2X6 */
{
    uint8_t s = 0, i;
    int wave = 0, vol = 0;
    for (i = 0; i < GUS_VOICES; ++i) {
        if (st->v[i].ctrl & 0x80)  wave = 1;
        if (st->v[i].vctrl & 0x80) vol = 1;
    }
    if (st->t1_exp) s |= 0x04;
    if (st->t2_exp) s |= 0x08;
    if (wave)       s |= 0x20;
    if (vol)        s |= 0x40;
    if (st->dma_tc || st->samp_tc) s |= 0x80;
    return s;
}
/* The card's line is asserted while any ENABLED source is pending and the master IRQ
   enable (4Ch bit 2) is on. The host latches an interrupt per call, so raise on the
   rising edge only: a source the guest has not cleared does not interrupt twice. */
static void gus_irq_update(gus_state *st)
{
    int any = gus_voice_pending(st)
           || (st->dma_tc && (st->dma_ctrl & 0x20))
           || (st->samp_tc && (st->samp_ctrl & 0x20))
           || (st->t1_exp && (st->timer_ctrl & 0x04))
           || (st->t2_exp && (st->timer_ctrl & 0x08));
    if (!(st->reset & 0x04)) any = 0;
    if (any && !st->line_up && st->bus) {
        vdd_raise_irq(st->bus, gus_irq_line(st));
        st->irqs_raised++;
    }
    st->line_up = (uint8_t)(any != 0);
}

/* 8Fh: one pending voice event per read, cleared by the read (ref §6). Bits 7/6 are
   ACTIVE-LOW; both 1 means nothing is left. */
static uint8_t gus_irq_fifo(gus_state *st)
{
    unsigned i;
    st->fifo_reads++;
    for (i = 0; i < GUS_VOICES; ++i) {
        gus_voice *v = &st->v[i];
        if ((v->ctrl & 0x80) || (v->vctrl & 0x80)) {
            uint8_t r = (uint8_t)(0x20 | i);
            if (!(v->ctrl & 0x80))  r |= 0x80;
            if (!(v->vctrl & 0x80)) r |= 0x40;
            v->ctrl  &= 0x7F;
            v->vctrl &= 0x7F;
            gus_irq_update(st);
            return r;
        }
    }
    return 0xE0;
}

/* ---- DRAM DMA (ref §3) ---------------------------------------------------------- */

/* The 16-bit-channel address translation, undone (ref §2.1). */
static uint32_t gus_untranslate16(uint32_t t) { return ((t & 0x1FFFFu) << 1) | (t & 0xC0000u); }

static void gus_dma_try(gus_state *st)
{
    uint8_t ch = gus_dma_line(st);
    uint32_t left, addr, n;
    int tc = 0;
    uint8_t buf[512];
    if (!st->dma_waiting || !st->dma || !ch || !st->dram) return;
    left = vdd_dma_remaining(st->dma, ch);
    if (!left) return;                                  /* the 8237 is not ready yet */
    addr = (uint32_t)st->dma_addr << 4;
    if (st->dma_ctrl & 0x04) addr = gus_untranslate16(addr);
    while (left && !tc) {
        uint32_t k, chunk = left > sizeof buf ? (uint32_t)sizeof buf : left;
        if (st->dma_ctrl & 0x02) break;                 /* card -> PC: not modelled   */
        n = vdd_dma_read(st->dma, ch, buf, chunk, &tc);
        if (!n) break;
        for (k = 0; k < n; ++k) {
            uint8_t b = buf[k];
            /* bit 7 = invert the MSB: bit 7 of every byte for 8-bit data, bit 15 of
               every word (the odd byte) for 16-bit data (41h bit 6 written = 16-bit). */
            if (st->dma_ctrl & 0x80) {
                if (!(st->dma_ctrl & 0x40) || ((addr + k) & 1)) b ^= 0x80;
            }
            st->dram[(addr + k) & (GUS_DRAM_SIZE - 1)] = b;
        }
        addr += n; left -= n; st->dma_bytes += n;
    }
    st->dma_waiting = 0;
    st->dma_uploads++;
    st->dma_tc = 1;
    gus_irq_update(st);
}

/* ---- the chip reset (4Ch bit 0 = 0) ------------------------------------------------- */

static void gus_chip_reset(gus_state *st)
{
    unsigned i;
    for (i = 0; i < GUS_VOICES; ++i) {
        gus_voice *v = &st->v[i];
        v->ctrl = 0x03; v->vctrl = 0x03;    /* stopped, voice and ramp */
        v->fc = 0x0400; v->start = v->end = v->pos = 0;
        v->ramp_rate = 0; v->ramp_start = 0; v->ramp_end = 0;
        v->vol = 0; v->pan = 7; v->ramp_div = 0;
    }
    st->active = 14;
    st->dma_ctrl = 0; st->dma_tc = 0; st->dma_waiting = 0;
    st->timer_ctrl = 0; st->samp_ctrl = 0; st->samp_tc = 0;
    st->t1_run = st->t2_run = 0; st->t1_exp = st->t2_exp = 0;
    st->line_up = 0;
}

/* ---- register write / read ------------------------------------------------------- */

static void gus_reg_write(gus_state *st, uint8_t r, uint16_t val)
{
    gus_voice *v = &st->v[st->page & 0x1F];
    uint8_t b = (uint8_t)val;
    switch (r) {
    /* voice (ref §2.2) */
    case 0x00: {
        int was_stopped = (v->ctrl & 0x03) != 0;
        v->ctrl = (uint8_t)((b & 0x7F) | (v->ctrl & 0x80));
        if (b & 0x02) v->ctrl |= 0x01;               /* stop -> stopped */
        if (!(b & 0x20)) v->ctrl &= 0x7F;            /* IRQ disabled: nothing pending */
        if (was_stopped && !(v->ctrl & 0x03)) st->voice_starts++;
        gus_irq_update(st);
        break; }
    case 0x01: v->fc = val; break;
    case 0x02: v->start = pos_set_hi(v->start, val); break;
    case 0x03: v->start = pos_set_lo(v->start, val, 0); break;
    case 0x04: v->end   = pos_set_hi(v->end, val); break;
    case 0x05: v->end   = pos_set_lo(v->end, val, 0); break;
    case 0x06: v->ramp_rate  = b; break;
    case 0x07: v->ramp_start = b; break;
    case 0x08: v->ramp_end   = b; break;
    case 0x09: v->vol = (uint16_t)(val & 0xFFF0); break;
    case 0x0A: v->pos = pos_set_hi(v->pos, val); break;
    case 0x0B: v->pos = pos_set_lo(v->pos, val, 1); break;
    case 0x0C: v->pan = (uint8_t)(b & 0x0F); break;
    case 0x0D:
        v->vctrl = (uint8_t)((b & 0x7F) | (v->vctrl & 0x80));
        if (b & 0x02) v->vctrl |= 0x01;
        if (!(b & 0x20)) v->vctrl &= 0x7F;
        gus_irq_update(st);
        break;
    case 0x0E: {
        uint8_t n = (uint8_t)((b & 0x1F) + 1);
        st->active = (uint8_t)(n < 14 ? 14 : n);
        break; }
    /* global (ref §2.1) */
    case 0x41:
        st->dma_ctrl = b;
        if (b & 0x01) { st->dma_waiting = 1; gus_dma_try(st); }
        else st->dma_waiting = 0;
        break;
    case 0x42: st->dma_addr = val; break;
    case 0x43: st->dram_io = (st->dram_io & 0xF0000u) | val; break;
    case 0x44: st->dram_io = (st->dram_io & 0x0FFFFu) | ((uint32_t)(b & 0x0F) << 16); break;
    case 0x45:
        st->timer_ctrl = b;
        if (!(b & 0x04)) st->t1_exp = 0;
        if (!(b & 0x08)) st->t2_exp = 0;
        gus_irq_update(st);
        break;
    case 0x46: st->t1_load = b; st->t1_val = b; break;
    case 0x47: st->t2_load = b; st->t2_val = b; break;
    case 0x48: st->samp_freq = b; break;
    case 0x49:
        st->samp_ctrl = b;
        if (b & 0x01) { st->samp_tc = 1; gus_irq_update(st); }   /* no input: an empty take */
        break;
    case 0x4B: st->jtrim = b; break;
    case 0x4C:
        if (!(b & 0x01)) gus_chip_reset(st);
        st->reset = b;
        gus_irq_update(st);
        break;
    default: break;
    }
}

static uint16_t gus_reg_read(gus_state *st, uint8_t r)
{
    gus_voice *v = &st->v[st->page & 0x1F];
    switch (r) {
    case 0x80: return v->ctrl;
    case 0x81: return v->fc;
    case 0x82: return pos_get_hi(v->start);
    case 0x83: return pos_get_lo(v->start, 0);
    case 0x84: return pos_get_hi(v->end);
    case 0x85: return pos_get_lo(v->end, 0);
    case 0x86: return v->ramp_rate;
    case 0x87: return v->ramp_start;
    case 0x88: return v->ramp_end;
    case 0x89: return v->vol;
    case 0x8A: return pos_get_hi(v->pos);
    case 0x8B: return pos_get_lo(v->pos, 1);
    case 0x8C: return v->pan;
    case 0x8D: return v->vctrl;
    case 0x8E: return (uint16_t)(0xC0 | (st->active - 1));
    case 0x8F: return gus_irq_fifo(st);
    case 0x41: {                                     /* TC pending in bit 6, cleared by the read */
        uint8_t r8 = (uint8_t)((st->dma_ctrl & 0xBF) | (st->dma_tc ? 0x40 : 0));
        st->dma_tc = 0; gus_irq_update(st);
        return r8; }
    case 0x45: return st->timer_ctrl;
    case 0x49: {
        uint8_t r8 = (uint8_t)((st->samp_ctrl & 0xBF) | (st->samp_tc ? 0x40 : 0));
        st->samp_tc = 0; gus_irq_update(st);
        return r8; }
    case 0x4C: return st->reset;
    default:   return 0;
    }
}

/* ---- ports (ref §1) ---------------------------------------------------------- */

static void gus_out(void *self, uint16_t port, uint8_t w, uint32_t val)
{
    gus_state *st = (gus_state *)self;
    uint16_t off = (uint16_t)(port - st->base);      /* 0x000-0x00F, or 0x100-0x107 */
    int arm = 0;
    st->io_writes++;
    switch (off) {
    case 0x000: st->mix = (uint8_t)val; arm = 1; break;
    case 0x008: st->adlib_idx = (uint8_t)val; break;
    case 0x009:
        if (st->adlib_idx == 4) {
            if (val & 0x80) { st->t1_exp = st->t2_exp = 0; gus_irq_update(st); break; }
            st->adlib_mask = (uint8_t)(val & 0x60);
            st->t1_run = (uint8_t)(val & 1); st->t2_run = (uint8_t)((val >> 1) & 1);
            if (st->t1_run) { st->t1_val = st->t1_load; st->t1_acc_ns = 0; }
            if (st->t2_run) { st->t2_val = st->t2_load; st->t2_acc_ns = 0; }
        }
        break;
    case 0x00B:
        /* The write must be the NEXT one after 2X0, or it is locked out (ref §5). */
        if (!st->latch_armed) { st->latch_locked_out++; break; }
        if (st->regctl == 0) {
            if (st->mix & 0x40) st->irq_latch = (uint8_t)(val & 0x7F);
            else                st->dma_latch = (uint8_t)(val & 0x7F);
        }
        /* regctl 5 = "write 0 to clear power-up IRQs", 6 = the jumper register: stored nowhere */
        break;
    case 0x00F: st->regctl = (uint8_t)(val & 7); break;
    case 0x100: st->midi_ctrl = (uint8_t)val; break;
    case 0x101: break;                               /* MIDI transmit: no port behind it */
    case 0x102: st->page = (uint8_t)(val & 0x1F); break;
    case 0x103: st->sel = (uint8_t)val; break;
    case 0x104:
        if (w >= 2) { gus_reg_write(st, st->sel, (uint16_t)val); break; }
        /* A byte to 3X4 is the LOW half of a 16-bit register: latched, and the write
           to 3X5 that follows completes it (ref §2). */
        st->lo_latch = (uint16_t)(val & 0xFF);
        break;
    case 0x105:
        if (gus_reg16(st->sel)) gus_reg_write(st, st->sel, (uint16_t)(((val & 0xFF) << 8) | st->lo_latch));
        else                    gus_reg_write(st, st->sel, (uint16_t)(val & 0xFF));
        break;
    case 0x107:
        if (st->dram) st->dram[st->dram_io & (GUS_DRAM_SIZE - 1)] = (uint8_t)val;
        st->dram_pokes++;
        break;
    default: break;
    }
    st->latch_armed = (uint8_t)arm;
}

static void gus_in(void *self, uint16_t port, uint8_t w, uint32_t *val)
{
    gus_state *st = (gus_state *)self;
    uint16_t off = (uint16_t)(port - st->base);
    uint32_t r = 0xFF;
    st->io_reads++;
    switch (off) {
    case 0x006: r = gus_irq_status(st); break;
    case 0x008: r = (uint32_t)((st->t1_exp && !(st->adlib_mask & 0x40) ? 0x40 : 0)
                             | (st->t2_exp && !(st->adlib_mask & 0x20) ? 0x20 : 0));
                if (r) r |= 0x80;
                break;
    case 0x00F: r = st->regctl; break;
    case 0x100: r = 0x02; break;                     /* 6850 status: transmitter empty */
    case 0x101: r = 0x00; break;
    case 0x102: r = st->page; break;
    case 0x103: r = st->sel; break;
    case 0x104: {
        uint16_t v = gus_reg16(st->sel) ? gus_reg_read(st, (uint8_t)(st->sel | 0x80)) : 0;
        r = (w >= 2) ? v : (v & 0xFF);
        break; }
    case 0x105: {
        uint8_t s = st->sel;
        if (s < 0x40) s |= 0x80;                     /* voice regs read at 80h+ (ref §2.2) */
        r = gus_reg16(st->sel) ? (uint32_t)(gus_reg_read(st, s) >> 8) : (uint32_t)(gus_reg_read(st, s) & 0xFF);
        break; }
    case 0x107:
        r = st->dram ? st->dram[st->dram_io & (GUS_DRAM_SIZE - 1)] : 0xFF;
        st->dram_peeks++;
        break;
    default: break;
    }
    *val = r;
}

/* ---- the voice engine (ref §4, §7) --------------------------------------------- */

uint32_t vdd_gus_vol_gain(uint16_t vol12)
{
    /* ref §7: the SDK's own linear table pins the curve -- one exponent step per
       octave, the mantissa linear within it: amplitude ∝ 2^E × (256 + M) / 256. */
    uint32_t e = (vol12 >> 8) & 0x0F, m = vol12 & 0xFF;
    if (!vol12) return 0;
    return (((256u + m) << e) >> 8);                 /* Q16: 0xFFF -> 65408 ≈ 1.0 */
}

uint32_t vdd_gus_rate_hz(const gus_state *st)
{
    /* 1.6197 us per voice per pass (ref §4): 14 voices -> 44.1 kHz. */
    uint32_t a = st->active ? st->active : 14;
    return 617400u / a;
}

static int32_t gus_fetch(const gus_state *st, const gus_voice *v, uint32_t addr)
{
    if (v->ctrl & 0x04) {                            /* 16-bit: the address is translated */
        uint32_t p = gus_untranslate16(addr & 0xFFFFFu) & (GUS_DRAM_SIZE - 2);
        return (int16_t)(st->dram[p] | (st->dram[p + 1] << 8));
    }
    return (int32_t)(int8_t)st->dram[addr & (GUS_DRAM_SIZE - 1)] << 8;
}

static void gus_voice_step(gus_state *st, gus_voice *v)
{
    uint32_t inc = (uint32_t)(v->fc >> 1), old = v->pos;
    if (v->ctrl & 0x03) return;                      /* stopped: holds its place */
    if (v->ctrl & 0x40) {                            /* decreasing */
        v->pos = (old >= inc) ? old - inc : 0;
        if (old > v->start && v->pos <= v->start) {
            if (v->vctrl & 0x04) { if (v->ctrl & 0x20) v->ctrl |= 0x80; }
            else if (v->ctrl & 0x08) {
                if (v->ctrl & 0x10) { v->ctrl &= (uint8_t)~0x40; v->pos = v->start + (v->start - v->pos); }
                else                  v->pos = v->end - (v->start - v->pos);
                if (v->ctrl & 0x20) v->ctrl |= 0x80;
            } else { v->ctrl |= 0x01; v->pos = v->start; if (v->ctrl & 0x20) v->ctrl |= 0x80; }
        }
    } else {
        v->pos = old + inc;
        if (old < v->end && v->pos >= v->end) {
            if (v->vctrl & 0x04) { if (v->ctrl & 0x20) v->ctrl |= 0x80; }     /* rollover */
            else if (v->ctrl & 0x08) {
                if (v->ctrl & 0x10) { v->ctrl |= 0x40; v->pos = v->end - (v->pos - v->end); }
                else                  v->pos = v->start + (v->pos - v->end);
                if (v->ctrl & 0x20) v->ctrl |= 0x80;
            } else { v->ctrl |= 0x01; v->pos = v->end; if (v->ctrl & 0x20) v->ctrl |= 0x80; }
        }
    }
}

static void gus_ramp_step(gus_voice *v)
{
    static const uint32_t div[4] = { 1, 8, 64, 512 };
    int32_t vol12, lo, hi, step;
    if (v->vctrl & 0x03) return;
    if (++v->ramp_div < div[(v->ramp_rate >> 6) & 3]) return;
    v->ramp_div = 0;
    step = v->ramp_rate & 0x3F;
    vol12 = v->vol >> 4;
    lo = (int32_t)v->ramp_start << 4;
    hi = (int32_t)v->ramp_end << 4;
    if (v->vctrl & 0x40) {                           /* decreasing */
        vol12 -= step;
        if (vol12 <= lo) {
            if (v->vctrl & 0x08) { if (v->vctrl & 0x10) { v->vctrl &= (uint8_t)~0x40; vol12 = lo; } else vol12 = hi; }
            else { vol12 = lo; v->vctrl |= 0x01; }
            if (v->vctrl & 0x20) v->vctrl |= 0x80;
        }
    } else {
        vol12 += step;
        if (vol12 >= hi) {
            if (v->vctrl & 0x08) { if (v->vctrl & 0x10) { v->vctrl |= 0x40; vol12 = hi; } else vol12 = lo; }
            else { vol12 = hi; v->vctrl |= 0x01; }
            if (v->vctrl & 0x20) v->vctrl |= 0x80;
        }
    }
    if (vol12 < 0) vol12 = 0;
    if (vol12 > 0xFFF) vol12 = 0xFFF;
    v->vol = (uint16_t)(vol12 << 4);
}

static void gus_timers(gus_state *st, uint32_t ns)
{
    if (st->t1_run) {
        st->t1_acc_ns += ns;
        while (st->t1_acc_ns >= 80000u) {            /* 80 us a tick (ref §9) */
            st->t1_acc_ns -= 80000u;
            if (++st->t1_val == 0) {
                st->t1_val = st->t1_load;
                if (!(st->adlib_mask & 0x40)) st->t1_exp = 1;
            }
        }
    }
    if (st->t2_run) {
        st->t2_acc_ns += ns;
        while (st->t2_acc_ns >= 320000u) {           /* 320 us */
            st->t2_acc_ns -= 320000u;
            if (++st->t2_val == 0) {
                st->t2_val = st->t2_load;
                if (!(st->adlib_mask & 0x20)) st->t2_exp = 1;
            }
        }
    }
}

void vdd_gus_render(gus_state *st, int16_t *out, uint32_t n)
{
    uint32_t i, k, ns = 1000000000u / (vdd_gus_rate_hz(st) ? vdd_gus_rate_hz(st) : 44100u);
    st->renders++;
    gus_dma_try(st);                                 /* a DMA that was waiting on the 8237 */
    for (i = 0; i < n; ++i) {
        int32_t acc = 0;
        if (st->dram && (st->reset & 0x03) == 0x03) {   /* running, DAC enabled */
            for (k = 0; k < st->active && k < GUS_VOICES; ++k) {
                gus_voice *v = &st->v[k];
                uint32_t addr = v->pos >> 9, frac = v->pos & 0x1FF;
                int32_t s0, s1, s, g;
                g = (int32_t)vdd_gus_vol_gain((uint16_t)(v->vol >> 4));
                if (g) {
                    s0 = gus_fetch(st, v, addr);
                    s1 = gus_fetch(st, v, addr + 1);
                    s  = s0 + (((s1 - s0) * (int32_t)frac) >> 9);
                    acc += (s * g) >> 16;
                }
                gus_voice_step(st, v);
                gus_ramp_step(v);
            }
        }
        gus_timers(st, ns);
        acc >>= 1;                                   /* headroom for many voices */
        out[i] = (int16_t)(acc > 32767 ? 32767 : (acc < -32768 ? -32768 : acc));
        if (out[i]) {                                /* is anything actually audible? */
            uint32_t a = (uint32_t)(out[i] < 0 ? -out[i] : out[i]);
            st->out_nonzero++;
            if (a > st->out_peak) st->out_peak = a;
        }
    }
    st->samples_out += n;
    gus_irq_update(st);
}

/* ---- the bus ------------------------------------------------------------------- */

void vdd_gus_reset(void *self)
{
    gus_state *st = (gus_state *)self;
    st->page = st->sel = 0; st->lo_latch = 0;
    st->reset = 0; st->dram_io = 0; st->dma_addr = 0;
    st->mix = 0x03; st->latch_armed = 0; st->irq_latch = st->dma_latch = 0; st->regctl = 0;
    st->adlib_idx = 0; st->adlib_mask = 0; st->midi_ctrl = 0;
    gus_chip_reset(st);
}

int vdd_gus_init(vdd_bus *b, void *self)
{
    gus_state *st = (gus_state *)self;
    st->bus = b;
    if (!st->base)   st->base   = GUS_DEFAULT_BASE;
    if (!st->irq)    st->irq    = GUS_DEFAULT_IRQ;
    if (!st->dma_ch) st->dma_ch = GUS_DEFAULT_DMA;
    vdd_gus_reset(st);
    if (vdd_claim_ports(b, st->base, (uint16_t)(st->base + 0x0F), gus_in, gus_out, st)) return -1;
    if (vdd_claim_ports(b, (uint16_t)(st->base + 0x100), (uint16_t)(st->base + 0x107), gus_in, gus_out, st)) return -1;
    return 0;
}
