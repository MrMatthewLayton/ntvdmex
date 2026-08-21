/* vdd_opl.c -- see vdd_opl.h.  AdLib / OPL2 (YM3812) register file and timers,
 * on the VDD bus.  Pure C, no <windows.h>. */
#include "vdd_opl.h"

/* The OPL's 18 operators are addressed by a register offset that deliberately
   skips 0x06/0x07, 0x0E/0x0F: offsets are 0x00-0x05, 0x08-0x0D, 0x10-0x15, in
   three banks of six. Channel n's two operators are the n-th slot of a bank and
   the one three slots later, so channel 3 is offsets 0x08 and 0x0B -- not 0x06
   and 0x09. Getting this wrong detunes half the channels in a way that sounds
   almost right, so both directions live here and nowhere else. */
int vdd_opl_op_index(int ch, int which)
{
    if (ch < 0 || ch >= OPL_NUM_CH) return -1;
    return (ch / 3) * 6 + (ch % 3) + (which ? 3 : 0);
}

/* register offset (low 5 bits of a 0x20/0x40/0x60/0x80/0xE0 register) -> operator,
   or -1 for the gaps. */
static int opl_off_to_op(uint8_t reg)
{
    int off = reg & 0x1F, bank = off >> 3, slot = off & 7;
    if (slot >= 6 || bank >= 3) return -1;
    return bank * 6 + slot;
}

/* --- timers --------------------------------------------------------------- */
/* Each timer counts UP from its preset; overflowing past 255 raises its status
   flag (unless masked) and reloads the preset, so the period is
   (256 - preset) * resolution. AdLib detection is exactly this measurement:
   preset 0xFF gives one 80us tick, which is why a detect that sees no flag
   concludes there is no card. */
static void opl_timer_step(uint16_t *count, uint8_t preset, uint8_t mask,
                           uint8_t flag, uint8_t *status)
{
    if (++(*count) > 0xFF) {
        *count = preset;
        if (!mask) *status |= (uint8_t)(flag | OPL_ST_IRQ);
    }
}

void vdd_opl_add_us(opl_state *st, uint32_t us)
{
    if (st->t1_run) {
        st->t1_frac_us += us;
        while (st->t1_frac_us >= OPL_T1_US) {
            st->t1_frac_us -= OPL_T1_US;
            opl_timer_step(&st->t1_count, st->t1_preset, st->t1_mask, OPL_ST_T1, &st->status);
        }
    }
    if (st->t2_run) {
        st->t2_frac_us += us;
        while (st->t2_frac_us >= OPL_T2_US) {
            st->t2_frac_us -= OPL_T2_US;
            opl_timer_step(&st->t2_count, st->t2_preset, st->t2_mask, OPL_ST_T2, &st->status);
        }
    }
}

/* --- register file -------------------------------------------------------- */
void vdd_opl_write_reg(opl_state *st, uint8_t reg, uint8_t val)
{
    int i;
    st->reg[reg] = val;
    st->prof_writes++;                                  /* profile: see opl_state */
    if (st->trace) st->trace(reg, val);                 /* dev-only capture hook  */

    if (reg == 0x02) {                                  /* timer 1 preset         */
        st->t1_preset = val;
        if (!st->t1_run) st->t1_count = val;
        return;
    }
    if (reg == 0x03) {                                  /* timer 2 preset         */
        st->t2_preset = val;
        if (!st->t2_run) st->t2_count = val;
        return;
    }
    if (reg == 0x04) {                                  /* timer control          */
        if (val & OPL_TC_IRQ_RST) {                     /* bit 7 resets flags and */
            st->status = 0;                             /* does nothing else      */
            return;
        }
        st->t1_mask = (val & OPL_TC_T1_MASK) ? 1 : 0;
        st->t2_mask = (val & OPL_TC_T2_MASK) ? 1 : 0;
        { uint8_t run1 = (val & OPL_TC_T1_START) ? 1 : 0;
          if (run1 && !st->t1_run) { st->t1_count = st->t1_preset; st->t1_frac_us = 0; }
          st->t1_run = run1; }
        { uint8_t run2 = (val & OPL_TC_T2_START) ? 1 : 0;
          if (run2 && !st->t2_run) { st->t2_count = st->t2_preset; st->t2_frac_us = 0; }
          st->t2_run = run2; }
        return;
    }

    if (reg >= 0x20 && reg <= 0x35) {                   /* AM/VIB/EGT/KSR/MULT    */
        i = opl_off_to_op(reg); if (i < 0) return;
        st->op[i].am   = (val >> 7) & 1;
        st->op[i].vib  = (val >> 6) & 1;
        st->op[i].egt  = (val >> 5) & 1;
        st->op[i].ksr  = (val >> 4) & 1;
        st->op[i].mult = val & 0x0F;
        if (st->op[i].am)  st->prof_am_ops  |= 1u << i;   /* profile: see opl_state */
        if (st->op[i].vib) st->prof_vib_ops |= 1u << i;
        return;
    }
    if (reg >= 0x40 && reg <= 0x55) {                   /* KSL / total level      */
        i = opl_off_to_op(reg); if (i < 0) return;
        st->op[i].ksl = (val >> 6) & 3;
        st->op[i].tl  = val & 0x3F;
        return;
    }
    if (reg >= 0x60 && reg <= 0x75) {                   /* attack / decay         */
        i = opl_off_to_op(reg); if (i < 0) return;
        st->op[i].ar = (val >> 4) & 0x0F;
        st->op[i].dr = val & 0x0F;
        return;
    }
    if (reg >= 0x80 && reg <= 0x95) {                   /* sustain / release      */
        i = opl_off_to_op(reg); if (i < 0) return;
        st->op[i].sl = (val >> 4) & 0x0F;
        st->op[i].rr = val & 0x0F;
        return;
    }
    if (reg >= 0xE0 && reg <= 0xF5) {                   /* waveform select        */
        i = opl_off_to_op(reg); if (i < 0) return;
        st->op[i].wave = val & 3;                       /* OPL2: 4 waveforms      */
        st->prof_wave_mask |= (uint8_t)(1u << (val & 3));
        return;
    }

    if (reg >= 0xA0 && reg <= 0xA8) {                   /* F-number low           */
        int c = reg - 0xA0;
        st->ch[c].fnum = (uint16_t)((st->ch[c].fnum & 0x300) | val);
        return;
    }
    if (reg >= 0xB0 && reg <= 0xB8) {                   /* key-on / block / F hi  */
        int c = reg - 0xB0, kon = (val >> 5) & 1;
        st->ch[c].fnum  = (uint16_t)((st->ch[c].fnum & 0xFF) | ((val & 3) << 8));
        st->ch[c].block = (val >> 2) & 7;
        if (kon && !st->ch[c].keyon) {                  /* key-on edge: restart   */
            int m = vdd_opl_op_index(c, 0), cr = vdd_opl_op_index(c, 1);
            st->op[m].eg_state = OPL_EG_ATTACK; st->op[m].phase = 0;
            st->op[cr].eg_state = OPL_EG_ATTACK; st->op[cr].phase = 0;
            st->prof_keyons++;                          /* profile: see opl_state  */
            if (st->op[m].am  || st->op[cr].am)  st->prof_keyon_am++;
            if (st->op[m].vib || st->op[cr].vib) st->prof_keyon_vib++;
        } else if (!kon && st->ch[c].keyon) {           /* key-off edge: release  */
            st->op[vdd_opl_op_index(c, 0)].eg_state = OPL_EG_RELEASE;
            st->op[vdd_opl_op_index(c, 1)].eg_state = OPL_EG_RELEASE;
        }
        st->ch[c].keyon = (uint8_t)kon;
        return;
    }
    if (reg >= 0xC0 && reg <= 0xC8) {                   /* feedback / connection  */
        int c = reg - 0xC0;
        st->ch[c].fb  = (val >> 1) & 7;
        st->ch[c].cnt = val & 1;
        return;
    }
    /* 0x01 (test/WSE), 0x08 (CSM/NTS), 0xBD (rhythm/depth) are stored in reg[]
       and consulted by the synth; nothing to decode here. */
    if (reg == 0xBD) { st->prof_bd_writes++; st->prof_bd_or |= val; }
    if (reg == 0x01 && (val & 0x20)) st->prof_wse = 1;
}

/* --- ports 0x388 / 0x389 -------------------------------------------------- */
static void opl_out(void *self, uint16_t port, uint8_t w, uint32_t v)
{
    opl_state *st = (opl_state *)self;
    (void)w;
    if ((port & 1) == 0) st->index = (uint8_t)v;        /* 0x388: address latch   */
    else                 vdd_opl_write_reg(st, st->index, (uint8_t)v);
}

static void opl_in(void *self, uint16_t port, uint8_t w, uint32_t *v)
{
    opl_state *st = (opl_state *)self;
    (void)w;
    /* 0x388 reads the status register; the data port is write-only on an OPL2. */
    *v = ((port & 1) == 0) ? st->status : 0xFF;
}

static void opl_frame(void *self)
{
    opl_state *st = (opl_state *)self;
    if (st->ext_clock) return;          /* host pumps real elapsed time instead */
    vdd_opl_add_us(st, st->frame_us);
}

/* --- lifecycle ------------------------------------------------------------ */
void vdd_opl_reset(void *self)
{
    opl_state *st = (opl_state *)self;
    vdd_bus *bus = st->bus;
    uint32_t fus = st->frame_us, shz = st->sample_hz;
    uint8_t  ext = st->ext_clock;
    unsigned i; uint8_t *p = (uint8_t *)st;
    for (i = 0; i < sizeof(*st); ++i) p[i] = 0;
    st->bus = bus;
    st->frame_us  = fus ? fus : OPL_DEFAULT_FRAME_US;
    st->sample_hz = shz ? shz : OPL_DEFAULT_HZ;
    st->ext_clock = ext;
    /* SILENT MEANS FULLY ATTENUATED, NOT ZERO. env counts attenuation, so zeroing
       the struct leaves every operator at FULL VOLUME waiting for its first note.
       Key-on does not reset env -- measured: the reference resumes an interrupted
       attack from where it was rather than restarting from silence -- so the very
       first note of a run attacked instantly at full level no matter what its
       attack rate said. That reads as "too loud" and as "no attack", and it is
       both: it is why our first measured attack time was 0.00 ms at every rate.  */
    for (i = 0; i < OPL_NUM_OP; ++i) {
        st->op[i].eg_state = OPL_EG_OFF;
        st->op[i].env = OPL_ENV_FULL;
    }
}

int vdd_opl_init(vdd_bus *b, void *self)
{
    opl_state *st = (opl_state *)self;
    st->bus = b;
    if (!st->frame_us)  st->frame_us  = OPL_DEFAULT_FRAME_US;
    if (!st->sample_hz) st->sample_hz = OPL_DEFAULT_HZ;
    if (vdd_claim_ports(b, 0x388, 0x389, opl_in, opl_out, st)) return -1;
    if (vdd_on_frame(b, opl_frame, st)) return -1;
    return 0;
}
