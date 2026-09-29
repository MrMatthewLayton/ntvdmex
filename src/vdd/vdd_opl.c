/* vdd_opl.c -- see vdd_opl.h.  AdLib / OPL2 (YM3812) and OPL3 (YMF262) register
 * file and timers, on the VDD bus.  Pure C, no <windows.h>. */
#include "vdd_opl.h"

/* The OPL's 18 operators are addressed by a register offset that deliberately
   skips 0x06/0x07, 0x0E/0x0F: offsets are 0x00-0x05, 0x08-0x0D, 0x10-0x15, in
   three banks of six. Channel n's two operators are the n-th slot of a bank and
   the one three slots later, so channel 3 is offsets 0x08 and 0x0B -- not 0x06
   and 0x09. Getting this wrong detunes half the channels in a way that sounds
   almost right, so both directions live here and nowhere else.
   The OPL3's array 1 is the same layout again: channel 9+n is array 1's channel
   n, and its operators are 18 + (what channel n's would be). */
int vdd_opl_op_index(int ch, int which)
{
    int arr, lc;
    if (ch < 0 || ch >= OPL3_NUM_CH) return -1;
    arr = ch / OPL_NUM_CH; lc = ch % OPL_NUM_CH;
    return arr * OPL_NUM_OP + (lc / 3) * 6 + (lc % 3) + (which ? 3 : 0);
}

/* NEW (0x105 bit 0) on a fitted OPL3. Everything the OPL3 adds is gated on it. */
int vdd_opl_new_mode(const opl_state *st)
{
    return st->opl3 && (st->reg[OPL3_REG_NEW] & 1);
}

/* ── 4-OPERATOR PAIRS (register 0x104, OPL3 with NEW set). Six bits, six pairs,
     and the pairs are fixed by the chip, not chosen: bit 0 joins channels 0 and 3,
     bit 1 joins 1+4, bit 2 joins 2+5, and bits 3-5 do the same in array 1 (9+12,
     10+13, 11+14). The FIRST channel of a pair owns the voice -- its F-number,
     block, key-on, feedback and output routing drive all four operators; the
     second contributes its two operators and its CNT bit (which, with the first
     channel's CNT, picks one of four algorithms; see vdd_opl_synth.c) and nothing
     else. Returns 1 for the first channel of a live pair, 2 for the second, 0 for
     a channel that is an ordinary two-operator voice. */
int opl_4op_role(const opl_state *st, int c)
{
    int lc, bit, role;
    if (!vdd_opl_new_mode(st) || c < 0 || c >= OPL3_NUM_CH) return 0;
    lc = c % OPL_NUM_CH;
    if (lc < 3)      { bit = lc;     role = 1; }
    else if (lc < 6) { bit = lc - 3; role = 2; }
    else return 0;
    if (c >= OPL_NUM_CH) bit += 3;
    return ((st->reg[OPL3_REG_4OP] >> bit) & 1) ? role : 0;
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
/* nosb.flag: no FM chip fitted. Kept as its own flag rather than reading the Sound
   Blaster's, because the off-VM suites link these two VDDs SEPARATELY -- referencing
   vdd_sb.c's copy from here broke `opl_synth_test` with an undefined symbol and cost
   half the gate (580 checks -> 244) until run.sh caught it. */
int g_opl_absent = 0;

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
/* Key one OPERATOR, rather than a channel. Rhythm mode needs this: four of the five
   percussion voices are single operators keyed independently from 0xBD, so the
   channel-wide key-on the melodic path uses cannot express them. */
static void opl_key_op(opl_state *st, int opi, int on)
{
    if (on) {
        st->op[opi].eg_state = OPL_EG_ATTACK;
        st->op[opi].phase = 0;
    } else if (st->op[opi].eg_state != OPL_EG_OFF) {
        st->op[opi].eg_state = OPL_EG_RELEASE;
    }
}

/* 0xBD's low five bits key the percussion voices. MEASURED, not assumed -- each
   operator was silenced in turn and the drum that went quiet named the owner
   (tools/oplref/oplprobe.c, experiment M):
       bit 0 hi-hat -> op13      bit 1 cymbal -> op17     bit 2 tom-tom -> op14
       bit 3 snare  -> op16      bit 4 bass drum -> channel 6, BOTH operators
   The bass drum is an ordinary two-operator FM voice; the other four are single
   operators heard directly. */
static void opl_rhythm_write(opl_state *st, uint8_t old, uint8_t val)
{
    static const uint8_t drum_op[4] = { 13, 17, 14, 16 };   /* HH, TC, TT, SD     */
    int b;
    if (!(val & OPL_BD_RHY)) {                  /* leaving rhythm mode: all quiet */
        if (old & OPL_BD_RHY)
            for (b = 12; b < 18; ++b) opl_key_op(st, b, 0);
        return;
    }
    if (!(old & OPL_BD_RHY)) old = 0;           /* entering: every set bit is new */
    for (b = 0; b < 4; ++b)
        if (((val >> b) & 1) != ((old >> b) & 1)) {
            opl_key_op(st, drum_op[b], (val >> b) & 1);
            if ((val >> b) & 1) st->prof_rhythm_hits[b]++;
        }
    if ((val & 0x10) != (old & 0x10)) {         /* bass drum keys both operators  */
        opl_key_op(st, 12, val & 0x10);
        opl_key_op(st, 15, val & 0x10);
        if (val & 0x10) st->prof_rhythm_hits[4]++;
    }
}

/* Key-on / key-off edge for channel `c` -- both its operators, or all four when it
   leads a 4-operator pair (the pair is ONE voice, keyed from the first channel). */
static void opl_key_channel(opl_state *st, int c, int kon)
{
    int n = (opl_4op_role(st, c) == 1) ? 2 : 1, k;
    if (kon && !st->ch[c].keyon) {                      /* key-on edge: restart   */
        int am = 0, vib = 0;
        for (k = 0; k < n; ++k) {
            int m = vdd_opl_op_index(c + 3 * k, 0), cr = vdd_opl_op_index(c + 3 * k, 1);
            st->op[m].eg_state = OPL_EG_ATTACK; st->op[m].phase = 0;
            st->op[cr].eg_state = OPL_EG_ATTACK; st->op[cr].phase = 0;
            am  |= st->op[m].am  | st->op[cr].am;
            vib |= st->op[m].vib | st->op[cr].vib;
        }
        st->prof_keyons++;                              /* profile: see opl_state  */
        if (am)  st->prof_keyon_am++;
        if (vib) st->prof_keyon_vib++;
    } else if (!kon && st->ch[c].keyon) {               /* key-off edge: release  */
        for (k = 0; k < n; ++k) {
            st->op[vdd_opl_op_index(c + 3 * k, 0)].eg_state = OPL_EG_RELEASE;
            st->op[vdd_opl_op_index(c + 3 * k, 1)].eg_state = OPL_EG_RELEASE;
        }
    }
    st->ch[c].keyon = (uint8_t)kon;
}

/* ── THE REGISTER FILE, BOTH ARRAYS. `reg` is 9 bits: bit 8 is the array (A1 on
     the address write). Array 1 has the same per-operator and per-channel
     registers as array 0 at the same offsets -- 0x120-0x135 is its AM/VIB/..., 0x1A0
     its F-numbers -- driving operators 18-35 and channels 9-17. What it does NOT
     have is array 0's globals: there are no timers, no 0xBD and no WSE in array 1.
     Its only globals are its own two, 0x104 (4-op pairs) and 0x105 (NEW).
   ⚠ ARRAY 1 LATCHES WITH NEW CLEAR. Every write is decoded into the operator and
     channel state exactly as array 0's is -- a driver commonly programs its voices
     first and sets NEW after -- but nothing in it is RENDERED, keyed into the
     4-op pairing, or routed until NEW is set (vdd_opl_synth.c). */
void vdd_opl_write_reg(opl_state *st, uint16_t reg9, uint8_t val)
{
    int i, arr;
    uint8_t reg, old;
    if (reg9 >= OPL3_NUM_REG) return;
    arr = reg9 >> 8;
    if (arr && !st->opl3) return;                       /* an OPL2 has no array 1 */
    reg = (uint8_t)reg9;
    old = st->reg[reg9];                        /* before the store: edge detection */
    st->reg[reg9] = val;
    st->prof_writes++;                                  /* profile: see opl_state */
    if (st->trace && !arr) st->trace(reg, val);         /* dev-only capture hook  */

    if (arr && reg < 0x20) return;      /* 0x104/0x105: stored, consulted where used;
                                           0x101-0x103, 0x108: nothing in array 1 */

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
        if (val & 0x80) st->prof_am_ops  |= 1u << i;      /* profile: see opl_state */
        if (val & 0x40) st->prof_vib_ops |= 1u << i;      /* (slot, either array)   */
        i += arr * OPL_NUM_OP;
        st->op[i].am   = (val >> 7) & 1;
        st->op[i].vib  = (val >> 6) & 1;
        st->op[i].egt  = (val >> 5) & 1;
        st->op[i].ksr  = (val >> 4) & 1;
        st->op[i].mult = val & 0x0F;
        return;
    }
    if (reg >= 0x40 && reg <= 0x55) {                   /* KSL / total level      */
        i = opl_off_to_op(reg); if (i < 0) return;
        i += arr * OPL_NUM_OP;
        st->op[i].ksl = (val >> 6) & 3;
        st->op[i].tl  = val & 0x3F;
        return;
    }
    if (reg >= 0x60 && reg <= 0x75) {                   /* attack / decay         */
        i = opl_off_to_op(reg); if (i < 0) return;
        i += arr * OPL_NUM_OP;
        st->op[i].ar = (val >> 4) & 0x0F;
        st->op[i].dr = val & 0x0F;
        return;
    }
    if (reg >= 0x80 && reg <= 0x95) {                   /* sustain / release      */
        i = opl_off_to_op(reg); if (i < 0) return;
        i += arr * OPL_NUM_OP;
        st->op[i].sl = (val >> 4) & 0x0F;
        st->op[i].rr = val & 0x0F;
        return;
    }
    if (reg >= 0xE0 && reg <= 0xF5) {                   /* waveform select        */
        i = opl_off_to_op(reg); if (i < 0) return;
        i += arr * OPL_NUM_OP;
        /* All three bits kept; which of them COUNT is the synth's call, because
           it depends on NEW / WSE as they stand when the note plays. */
        st->op[i].wave = val & 7;
        st->prof_wave_mask |= (uint8_t)(1u << (val & 7));
        return;
    }

    if (reg >= 0xA0 && reg <= 0xA8) {                   /* F-number low           */
        int c = reg - 0xA0 + arr * OPL_NUM_CH;
        st->ch[c].fnum = (uint16_t)((st->ch[c].fnum & 0x300) | val);
        return;
    }
    if (reg >= 0xB0 && reg <= 0xB8) {                   /* key-on / block / F hi  */
        int c = reg - 0xB0 + arr * OPL_NUM_CH, kon = (val >> 5) & 1;
        st->ch[c].fnum  = (uint16_t)((st->ch[c].fnum & 0xFF) | ((val & 3) << 8));
        st->ch[c].block = (val >> 2) & 7;
        /* In rhythm mode channels 6-8 ARE the percussion voices, keyed from 0xBD.
           Their own key-on bit is not theirs to use any more -- but the F-number
           and block above still are, because that is how a driver tunes the drums.
           Array 0 only: rhythm mode has no counterpart in array 1. */
        if (c >= 6 && c <= 8 && (st->reg[0xBD] & OPL_BD_RHY)) return;
        /* The second channel of a 4-op pair has no key of its own: its F-number
           and block are latched above (and ignored), its key-on bit is ignored. */
        if (opl_4op_role(st, c) == 2) return;
        opl_key_channel(st, c, kon);
        return;
    }
    if (reg >= 0xC0 && reg <= 0xC8) {                   /* feedback / connection  */
        int c = reg - 0xC0 + arr * OPL_NUM_CH;
        st->ch[c].fb  = (val >> 1) & 7;
        st->ch[c].cnt = val & 1;
        /* bits 4-7 (output routing, OPL3) are read from reg[] by the synth */
        return;
    }
    if (arr) return;                    /* 0x1BD etc.: array 1 has no such globals */
    /* 0x01 (test/WSE), 0x08 (CSM/NTS), 0xBD (rhythm/depth) are stored in reg[]
       and consulted by the synth; nothing to decode here. */
    if (reg == 0xBD) {
        opl_rhythm_write(st, old, val);
        st->prof_bd_writes++; st->prof_bd_or |= val;
    }
    if (reg == 0x01 && (val & 0x20)) st->prof_wse = 1;
}

/* --- ports 0x388-0x38B ------------------------------------------------------ */
/* The chip has two address lines. A0 picks address (0) or data (1); A1 picks the
   register ARRAY for an address write -- which is why an OPL3 is four ports, and
   why the data port does not care which of 0x389/0x38B it is: there is one 9-bit
   latch, and a data write goes wherever it points. An OPL2 has no A1 at all
   (an AdLib decodes 0x388/0x389 only), so for it 0x38A/0x38B are not there. */
void vdd_opl_write_addr(opl_state *st, int array, uint8_t val)
{
    if (array && !st->opl3) return;                     /* no array 1 on an OPL2  */
    st->index = (uint16_t)((array ? 0x100 : 0) | val);
}

void vdd_opl_write_data(opl_state *st, uint8_t val)
{
    vdd_opl_write_reg(st, st->index, val);
}

/* ── THE STATUS BYTE, AND WHAT THE OPL3 CHANGES IN IT. Bits 7-5 are IRQ, T1, T2
     on both chips -- the AdLib detect (reset, read 0x00, run T1, read 0xC0) masks
     with 0xE0 and passes identically on either. Bits 2-1 are the difference: a
     YM3812 reads them as 1, a YMF262 as 0, and that is the OPL3 detect -- an
     OPL2 idles at 0x06 and reads 0xC6 after the timer test, an OPL3 at 0x00 and
     0xC0. Before #232 the model read 0x00 while being an OPL2, i.e. it told every
     OPL3 detect that array 1 was there when it was not.
   ► `nosb.flag` ALSO UNFITS THE OPL. The AdLib detect is a status-register
     dance (mask/reset the timers, read 0xC0, run timer 1, read 0x80) -- a
     machine with no FM chip floats the bus and reads 0xFF, which fails it.
     Doom's DMX uses ADLIB for music even with no Sound Blaster DSP, so
     withholding only the DSP's 0xAA left music running and the run still died
     in the timer ISR. One knob, no sound devices at all: that is the point of
     the knob, which is to find out what Doom does with NO music rather than to
     ship a machine without an OPL. */
uint8_t vdd_opl_read_status(const opl_state *st)
{
    if (g_opl_absent) return 0xFF;
    return (uint8_t)(st->status | (st->opl3 ? 0 : OPL_ST_OPL2_ID));
}

static void opl_out(void *self, uint16_t port, uint8_t w, uint32_t v)
{
    opl_state *st = (opl_state *)self;
    int a1 = (port & 2) ? 1 : 0;
    (void)w;
    if (a1 && !st->opl3) return;                        /* OPL2: not decoded      */
    if ((port & 1) == 0) vdd_opl_write_addr(st, a1, (uint8_t)v);  /* 0x388/0x38A  */
    else                 vdd_opl_write_data(st, (uint8_t)v);      /* 0x389/0x38B  */
}

static void opl_in(void *self, uint16_t port, uint8_t w, uint32_t *v)
{
    opl_state *st = (opl_state *)self;
    (void)w;
    /* The data ports are write-only on both chips. 0x38A on an OPL3 reads status
       too: the datasheet's read cycle is specified with A0 low and says nothing
       of A1, and a chip that does not latch on a read has no reason to decode it
       -- ⚠ an INFERENCE, owed a check against a real SB16. On an OPL2 0x38A/0x38B
       float, exactly as on an AdLib, whose decode stops at 0x389. */
    if ((port & 1) || ((port & 2) && !st->opl3)) { *v = 0xFF; return; }
    *v = vdd_opl_read_status(st);
}

/* Every voice to its release, both arrays: the key-off a program that ended
   never sent. The F-number stays, so each note decays through its own envelope
   rather than clicking off. */
void vdd_opl_all_notes_off(opl_state *st)
{
    int c;
    for (c = 0; c < OPL3_NUM_CH; ++c) {
        uint16_t r = (uint16_t)((c / OPL_NUM_CH) * 0x100 + 0xB0 + c % OPL_NUM_CH);
        if (c >= OPL_NUM_CH && !st->opl3) break;
        if (st->reg[r] & 0x20) vdd_opl_write_reg(st, r, (uint8_t)(st->reg[r] & ~0x20));
    }
    if (st->reg[0xBD] & 0x1F)                   /* rhythm drums, keyed separately */
        vdd_opl_write_reg(st, 0xBD, (uint8_t)(st->reg[0xBD] & ~0x1F));
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
    uint8_t  ext = st->ext_clock, opl3 = st->opl3;
    unsigned i; uint8_t *p = (uint8_t *)st;
    for (i = 0; i < sizeof(*st); ++i) p[i] = 0;
    st->bus = bus;
    st->frame_us  = fus ? fus : OPL_DEFAULT_FRAME_US;
    st->sample_hz = shz ? shz : OPL_DEFAULT_HZ;
    st->ext_clock = ext;
    st->opl3      = opl3;       /* the card, not the guest's state: NEW is 0 again */
    /* SILENT MEANS FULLY ATTENUATED, NOT ZERO. env counts attenuation, so zeroing
       the struct leaves every operator at FULL VOLUME waiting for its first note.
       Key-on does not reset env -- measured: the reference resumes an interrupted
       attack from where it was rather than restarting from silence -- so the very
       first note of a run attacked instantly at full level no matter what its
       attack rate said. That reads as "too loud" and as "no attack", and it is
       both: it is why our first measured attack time was 0.00 ms at every rate.  */
    for (i = 0; i < OPL3_NUM_OP; ++i) {
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
    /* All four ports, whichever chip: on an OPL2 the top two answer as nothing
       (see opl_out/opl_in), and the host can then change the chip without
       re-plumbing the bus. */
    if (vdd_claim_ports(b, 0x388, 0x38B, opl_in, opl_out, st)) return -1;
    if (vdd_on_frame(b, opl_frame, st)) return -1;
    return 0;
}
