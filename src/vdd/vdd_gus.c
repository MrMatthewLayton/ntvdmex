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

/* The two 3-bit codes of the 2XB latches. Code 0 is "no line" in both tables. */
static const uint8_t k_irq_map[8] = { 0, 2, 5, 3, 7, 11, 12, 15 };
static const uint8_t k_dma_map[8] = { 0, 1, 3, 5, 6, 7, 0, 0 };
static uint8_t gus_code_of(const uint8_t *map, uint8_t line)
{
    uint8_t c;
    for (c = 1; c < 8; ++c) if (map[c] && map[c] == line) return c;
    return 0;
}

/* #190: decode the latches into the lines the card drives (ref §5).
     IRQ latch: bits 2-0 GF1, 5-3 MIDI, bit 6 = both on the GF1 line.
     DMA latch: bits 2-0 DRAM, 5-3 record, bit 6 = both on the DRAM channel.
   2X0 bit 4 is the mix register's own "combine the GF1 and MIDI IRQs"; either asks
   for one line. */
static void gus_latch_decode(gus_state *st)
{
    st->gf1_irq_line  = k_irq_map[st->irq_latch & 7];
    st->midi_irq_line = ((st->irq_latch & 0x40) || (st->mix & 0x10))
                      ? st->gf1_irq_line : k_irq_map[(st->irq_latch >> 3) & 7];
    st->dram_dma_line = k_dma_map[st->dma_latch & 7];
    st->rec_dma_line  = (st->dma_latch & 0x40) ? st->dram_dma_line
                                               : k_dma_map[(st->dma_latch >> 3) & 7];
}

/* 2X0 bit 3 powers the IRQ and DMA drivers: with it clear the card drives NO line,
   whatever the latches say (ref §5). */
static int gus_drivers_on(const gus_state *st) { return (st->mix & 0x08) != 0; }
static uint8_t gus_dram_dma(const gus_state *st) { return gus_drivers_on(st) ? st->dram_dma_line : 0; }
static uint8_t gus_rec_dma(const gus_state *st)  { return gus_drivers_on(st) ? st->rec_dma_line  : 0; }

/* ---- the MIDI UART, a 6850 (ref §9) ---------------------------------------------- */

/* The ACIA's own interrupt request: receive full with CR7 (receive IRQ enable), or
   transmit empty with CR6-5 = 01 -- the only one of the four transmit-control codes
   that enables the transmit IRQ (the others are RTS high, and RTS low + break).
   While CR1-0 = 11 the ACIA is held in master reset and requests nothing. */
static int gus_midi_tx_irq(const gus_state *st)
{
    return (st->midi_ctrl & 0x03) != 0x03 && (st->midi_ctrl & 0x60) == 0x20
        && (st->midi_stat & GUS_ACIA_TDRE);
}
static int gus_midi_rx_irq(const gus_state *st)
{
    return (st->midi_ctrl & 0x03) != 0x03 && (st->midi_ctrl & 0x80)
        && (st->midi_stat & GUS_ACIA_RDRF);
}
/* 2XB bank 6 bit 1: the MIDI port's address decode. Off, 3X0/3X1 are an empty bus. */
static int gus_midi_decoded(const gus_state *st) { return (st->jumper & 0x02) != 0; }

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
    if (gus_midi_tx_irq(st)) s |= 0x01;              /* #190: the UART's two sources */
    if (gus_midi_rx_irq(st)) s |= 0x02;
    if (st->t1_exp) s |= 0x04;
    if (st->t2_exp) s |= 0x08;
    if (wave)       s |= 0x20;
    if (vol)        s |= 0x40;
    if (st->dma_tc || st->samp_tc) s |= 0x80;
    return s;
}
/* One physical line: raise on its rising edge only (the host latches an interrupt per
   call, so a source the guest has not cleared must not interrupt twice). */
static void gus_line(gus_state *st, uint8_t line, int level, uint8_t *up)
{
    if (!line) level = 0;
    if (level && !*up && st->bus) {
        vdd_raise_irq(st->bus, line);
        st->irqs_raised++;
    }
    *up = (uint8_t)(level != 0);
}
/* The card's lines are asserted while any ENABLED source is pending, the master IRQ
   enable (4Ch bit 2) is on and 2X0 bit 3 powers the drivers (ref §5, §6).
   #190: the GF1 sources go out on the IRQ latch's GF1 line, the UART's on its MIDI
   line -- which is the GF1 line itself when the latch or 2X0 bit 4 combines them. */
static void gus_irq_update(gus_state *st)
{
    int gf1 = gus_voice_pending(st)
           || (st->dma_tc && (st->dma_ctrl & 0x20))
           || (st->samp_tc && (st->samp_ctrl & 0x20))
           || (st->t1_exp && (st->timer_ctrl & 0x04))
           || (st->t2_exp && (st->timer_ctrl & 0x08));
    int midi = gus_midi_tx_irq(st) || gus_midi_rx_irq(st);
    uint8_t gl = st->gf1_irq_line, ml = st->midi_irq_line;
    if (!(st->reset & 0x04) || !gus_drivers_on(st)) gf1 = midi = 0;
    if (ml == gl) { gus_line(st, gl, gf1 || midi, &st->line_up); st->midi_line_up = 0; }
    else          { gus_line(st, gl, gf1, &st->line_up); gus_line(st, ml, midi, &st->midi_line_up); }
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

/* Is the 8237 ready to serve a DRQ on `ch`? A masked channel is not: the card holds
   DRQ and waits, exactly as it would on the bus. (vdd_dma_remaining cannot say so --
   it is count + 1 and never 0.) */
static int gus_dma_ready(const gus_state *st, uint8_t ch)
{
    return st->dma && ch && !st->dma->ch[ch & 7].masked;
}

/* The DRAM DMA (ref §3), both directions, instantaneous once the 8237 serves it.
     41h bit 1 = 0: PC -> card, an upload (vdd_dma_read pulls guest memory).
     41h bit 1 = 1: card -> PC, #190: DRAM contents pushed into guest memory through
                    vdd_dma_write -- the guest programs its 8237 channel for a WRITE
                    (device -> memory) transfer, as for any card that is read.
   Bit 7 inverts the MSB of the data passing through, in both directions: it is a
   sign conversion, and converting on the way out undoes converting on the way in. */
static void gus_dma_try(gus_state *st)
{
    uint8_t ch = gus_dram_dma(st);
    uint32_t left, addr, n;
    int tc = 0, card_to_pc;
    uint8_t buf[512];
    if (!st->dma_waiting || !st->dram || !gus_dma_ready(st, ch)) return;
    left = vdd_dma_remaining(st->dma, ch);
    card_to_pc = (st->dma_ctrl & 0x02) != 0;
    addr = (uint32_t)st->dma_addr << 4;
    if (st->dma_ctrl & 0x04) addr = gus_untranslate16(addr);
    while (left && !tc) {
        uint32_t k, chunk = left > sizeof buf ? (uint32_t)sizeof buf : left;
        if (card_to_pc) {
            for (k = 0; k < chunk; ++k) {
                uint8_t b = st->dram[(addr + k) & (GUS_DRAM_SIZE - 1)];
                if ((st->dma_ctrl & 0x80) && (!(st->dma_ctrl & 0x40) || ((addr + k) & 1))) b ^= 0x80;
                buf[k] = b;
            }
            n = vdd_dma_write(st->dma, ch, buf, chunk, &tc);
            st->dma_down_bytes += n;
        } else {
            n = vdd_dma_read(st->dma, ch, buf, chunk, &tc);
            for (k = 0; k < n; ++k) {
                uint8_t b = buf[k];
                /* bit 7 = invert the MSB: bit 7 of every byte for 8-bit data, bit 15 of
                   every word (the odd byte) for 16-bit data (41h bit 6 written = 16-bit). */
                if (st->dma_ctrl & 0x80) {
                    if (!(st->dma_ctrl & 0x40) || ((addr + k) & 1)) b ^= 0x80;
                }
                st->dram[(addr + k) & (GUS_DRAM_SIZE - 1)] = b;
            }
            st->dma_bytes += n;
        }
        if (!n) break;
        addr += n; left -= n;
    }
    st->dma_waiting = 0;
    if (card_to_pc) st->dma_downloads++; else st->dma_uploads++;
    st->dma_tc = 1;
    gus_irq_update(st);
}

/* ---- the record path (ref §2.1: 48h rate, 49h control) ------------------------------ */

/* #190: sampling. 49h bit 0 starts the ADC; each sample goes card -> PC through the
   8237 on the RECORD DMA channel (the DMA latch's bits 5-3, or the DRAM channel when
   bit 6 combines them) at 9 878 400 / (16 x (48h + 2)) Hz, two bytes a sample when
   bit 1 asks for stereo. There is no input device behind line in or the mic, so every
   byte is the ADC's MIDSCALE: 80h -- 8-bit offset binary, the PC's unsigned sample
   format -- or 00h when 49h bit 7 inverts the MSB for signed data. Paced by rendered
   GF1 time, like the timers, so a program that times its take sees the rate it asked
   for; at terminal count the take stops (bit 0 is dropped) and 49h reports TC pending
   in bit 6, interrupting when bit 5 asked. A masked channel holds the ADC, as on the
   bus. */
static void gus_record(gus_state *st, uint32_t ns)
{
    uint8_t ch = gus_rec_dma(st), buf[2];
    uint32_t rate, per, unit;
    int tc = 0;
    if (!(st->samp_ctrl & 0x01)) return;
    if (!gus_dma_ready(st, ch)) return;
    unit = (ch & 4) ? 2u : 1u;                       /* a 16-bit channel moves words */
    rate = 9878400u / (16u * ((uint32_t)st->samp_freq + 2u));
    per  = 1000000000u / (rate ? rate : 1u);
    st->samp_acc_ns += ns;
    while (st->samp_acc_ns >= per && (st->samp_ctrl & 0x01)) {
        st->samp_acc_ns -= per;
        st->samp_pend = (uint8_t)(st->samp_pend + ((st->samp_ctrl & 0x02) ? 2 : 1));
        while (st->samp_pend >= unit) {
            uint32_t n;
            buf[0] = buf[1] = (st->samp_ctrl & 0x80) ? 0x00 : 0x80;
            n = vdd_dma_write(st->dma, ch, buf, unit, &tc);
            if (!n) return;                          /* masked under us: hold the ADC */
            st->samp_pend = (uint8_t)(st->samp_pend - n);
            st->samp_bytes += n;
            if (tc) {
                st->samp_ctrl &= (uint8_t)~0x01;
                st->samp_tc = 1; st->samp_acc_ns = 0; st->samp_pend = 0;
                gus_irq_update(st);
                return;
            }
        }
    }
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
    st->samp_acc_ns = 0; st->samp_pend = 0;
    st->t1_run = st->t2_run = 0; st->t1_exp = st->t2_exp = 0;
    st->line_up = 0; st->midi_line_up = 0;
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
        /* #190: a take runs through the 8237 at the 48h rate -- see gus_record. */
        if ((b & 0x01) && !(st->samp_ctrl & 0x01)) {
            st->samp_acc_ns = 0; st->samp_pend = 0; st->samp_takes++;
        }
        st->samp_ctrl = b;
        gus_irq_update(st);
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
    case 0x000:
        /* Mix control (ref §5). Bit 0 line in off and bit 2 mic on reach nothing here --
           there is no input device, the ADC hears silence either way; bit 1 (line out
           off) mutes the render; bit 3 powers the IRQ/DMA drivers; bit 4 combines the
           GF1 and MIDI IRQs; bit 5 loops the UART's transmit back to its receive; bit 6
           picks which latch the next 2XB write reaches. */
        st->mix = (uint8_t)val; arm = 1;
        gus_latch_decode(st);
        gus_irq_update(st);
        gus_dma_try(st);                             /* drivers just powered: a DRQ waits */
        break;
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
        /* #190: 2XF picks the bank behind 2XB (board rev 3.4+, ref §5). */
        switch (st->regctl) {
        case 0:                                      /* the classic IRQ / DMA latches */
            if (st->mix & 0x40) st->irq_latch = (uint8_t)(val & 0x7F);
            else                st->dma_latch = (uint8_t)(val & 0x7F);
            gus_latch_decode(st);
            gus_irq_update(st);
            gus_dma_try(st);
            break;
        case 5:
            /* "Write 0 to clear power-up IRQs": whatever the card was asserting when
               it powered up is let go. The lines drop, so a source still pending
               afterwards interrupts afresh on the next update. */
            st->reg_clr = (uint8_t)val;
            if (!(val & 0xFF)) { st->line_up = 0; st->midi_line_up = 0; }
            break;
        case 6:                                      /* the jumper register */
            st->jumper = (uint8_t)val;
            break;
        default: break;                              /* no register behind the rest */
        }
        break;
    case 0x00F: st->regctl = (uint8_t)(val & 7); break;
    case 0x100:
        /* 6850 control (ref §9). CR1-0 = 11 is master reset: receive emptied, overrun
           cleared, transmitter empty -- and the ACIA held until a different code. */
        if (!gus_midi_decoded(st)) break;
        st->midi_ctrl = (uint8_t)val;
        if ((val & 0x03) == 0x03) { st->midi_stat = GUS_ACIA_TDRE; st->midi_rx = 0; }
        gus_irq_update(st);
        break;
    case 0x101: {
        /* 6850 transmit. The byte leaves at once (the wire is not modelled at 31 250
           baud: the synth behind the sink is not a wire), so TDRE is back before the
           guest can look -- but it DID drop: with the transmit IRQ on, each byte is a
           fresh empty edge, which is what a driver's IRQ-driven send loop waits for.
           2X0 bit 5 loops TxD to RxD inside the card: the byte is received, and does
           not reach MIDI OUT. */
        uint8_t byte = (uint8_t)val;
        if (!gus_midi_decoded(st) || (st->midi_ctrl & 0x03) == 0x03) break;
        st->midi_stat &= (uint8_t)~GUS_ACIA_TDRE;
        gus_irq_update(st);
        if (st->mix & 0x20) {
            if (st->midi_stat & GUS_ACIA_RDRF) st->midi_stat |= GUS_ACIA_OVRN;
            st->midi_rx = byte; st->midi_stat |= GUS_ACIA_RDRF; st->midi_rx_bytes++;
        } else if (st->midi_sink) st->midi_sink(st->midi_sink_ctx, byte);
        st->midi_tx++;
        st->midi_stat |= GUS_ACIA_TDRE;
        gus_irq_update(st);
        break; }
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
    case 0x100:                                      /* 6850 status (ref §9) */
        if (!gus_midi_decoded(st)) break;
        r = st->midi_stat;
        if (gus_midi_tx_irq(st) || gus_midi_rx_irq(st)) r |= GUS_ACIA_IRQ;
        break;
    case 0x101:                                      /* 6850 receive: clears RDRF, OVRN */
        if (!gus_midi_decoded(st)) break;
        r = st->midi_rx;
        st->midi_stat &= (uint8_t)~(GUS_ACIA_RDRF | GUS_ACIA_OVRN);
        gus_irq_update(st);
        break;
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

/* ── #189: PAN. The GF1 places each voice at one of 16 positions (reg 0Ch: 0 = hard
     left, 15 = hard right, 7/8 = the middle). This is a BALANCE law, not a split: a side
     stays at full level until the voice moves away from it, so a centred voice comes
     out of each channel exactly as loud as the old mono sum -- nothing a program already
     plays gets quieter -- and a hard-panned one is silent on the far side. Q8 gains. */
static int32_t gus_pan_l(uint8_t p) { int32_t g = (int32_t)(15 - (p & 15)) * 512 / 15; return g > 256 ? 256 : g; }
static int32_t gus_pan_r(uint8_t p) { int32_t g = (int32_t)(p & 15) * 512 / 15;        return g > 256 ? 256 : g; }
static int16_t gus_clip(int32_t a) { return (int16_t)(a > 32767 ? 32767 : (a < -32768 ? -32768 : a)); }

/* One render loop, two output shapes (as vdd_sb): `stereo` 0 writes the mono sum it
   always did, 1 writes panned L/R pairs at out[2i], out[2i+1]. */
static void gus_render(gus_state *st, int16_t *out, uint32_t n, int stereo)
{
    uint32_t i, k, ns = 1000000000u / (vdd_gus_rate_hz(st) ? vdd_gus_rate_hz(st) : 44100u);
    st->renders++;
    gus_dma_try(st);                                 /* a DMA that was waiting on the 8237 */
    for (i = 0; i < n; ++i) {
        int32_t acc = 0, accl = 0, accr = 0;
        int16_t m;
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
                    s  = (s * g) >> 16;
                    acc += s;
                    if (stereo) { accl += (s * gus_pan_l(v->pan)) >> 8;
                                  accr += (s * gus_pan_r(v->pan)) >> 8; }
                }
                gus_voice_step(st, v);
                gus_ramp_step(v);
            }
        }
        gus_timers(st, ns);
        gus_record(st, ns);
        /* #190: 2X0 bit 1 = line out DISABLED (active-high, ref §5). The voices still
           run -- the GF1 does not know the amplifier is off -- but nothing is heard. */
        if (st->mix & 0x02) { acc = accl = accr = 0; st->out_muted++; }
        m = gus_clip(acc >> 1);                      /* headroom for many voices */
        if (stereo) { out[2*i] = gus_clip(accl >> 1); out[2*i+1] = gus_clip(accr >> 1); }
        else          out[i] = m;
        if (m) {                                     /* is anything actually audible? */
            uint32_t a = (uint32_t)(m < 0 ? -m : m);
            st->out_nonzero++;
            if (a > st->out_peak) st->out_peak = a;
        }
    }
    st->samples_out += n;
    gus_irq_update(st);
}

void vdd_gus_render(gus_state *st, int16_t *out, uint32_t n)    { gus_render(st, out, n, 0); }
void vdd_gus_render_st(gus_state *st, int16_t *out, uint32_t n) { gus_render(st, out, n, 1); }

/* ---- the bus ------------------------------------------------------------------- */

void vdd_gus_reset(void *self)
{
    gus_state *st = (gus_state *)self;
    st->page = st->sel = 0; st->lo_latch = 0;
    st->reset = 0; st->dram_io = 0; st->dma_addr = 0;
    st->latch_armed = 0; st->regctl = 0; st->reg_clr = 0;
    st->adlib_idx = 0; st->adlib_mask = 0;
    /* #190: the card as ULTRINIT leaves it, because that is the card every DOS program
       meets -- nothing on a PC runs before the boot-time init that a real GUS owner
       has in AUTOEXEC.BAT, and a program that only POLLS (heaven7) never programs
       the board itself. So: the latches hold ULTRASND's own numbers, combined where
       they are equal (as the SDK's UltraSetInterface does), line out on, line in off,
       drivers powered (2X0 = 09h, the SDK's final write, ref §5), both decodes
       enabled in the jumper register. A value with no code in the latch table cannot
       be latched, and is driven as it is. */
    st->mix = 0x09;
    st->jumper = 0x06;
    {
        uint8_t mi = st->midi_irq ? st->midi_irq : st->irq;
        uint8_t rd = st->rec_dma  ? st->rec_dma  : st->dma_ch;
        st->irq_latch = (uint8_t)(gus_code_of(k_irq_map, st->irq)
                      | (mi == st->irq ? 0x40 : (gus_code_of(k_irq_map, mi) << 3)));
        st->dma_latch = (uint8_t)(gus_code_of(k_dma_map, st->dma_ch)
                      | (rd == st->dma_ch ? 0x40 : (gus_code_of(k_dma_map, rd) << 3)));
        st->gf1_irq_line = st->irq; st->dram_dma_line = st->dma_ch;
        st->midi_irq_line = mi;     st->rec_dma_line  = rd;
    }
    st->midi_ctrl = 0x00; st->midi_stat = GUS_ACIA_TDRE; st->midi_rx = 0;   /* reset, then released */
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
