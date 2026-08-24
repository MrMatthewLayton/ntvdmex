/* vdd_sb.c -- see vdd_sb.h.  Sound Blaster 16 DSP, mixer and DMA playback engine,
 * on the VDD bus.  Pure C, no <windows.h>. */
#include "vdd_sb.h"

/* --- the DSP's little output queue ---------------------------------------- */
/* Reads from 2xA come from here, and 2xE reports whether anything is waiting.
   The reset handshake, the version query and the identify command all answer
   through this queue, which is why detection is really a queue test. */
static void sb_outq_push(sb_state *st, uint8_t v)
{
    if (st->outq_len >= SB_OUTQ_MAX) return;
    st->outq[(st->outq_head + st->outq_len) % SB_OUTQ_MAX] = v;
    st->outq_len++;
}
static uint8_t sb_outq_pop(sb_state *st)
{
    uint8_t v;
    if (!st->outq_len) return 0xFF;             /* nothing waiting: bus floats     */
    v = st->outq[st->outq_head];
    st->outq_head = (uint8_t)((st->outq_head + 1) % SB_OUTQ_MAX);
    st->outq_len--;
    return v;
}

int g_sb_absent = 0;      /* nosb.flag -- see the DSP-reset case below */

static void sb_dsp_soft_reset(sb_state *st)
{
    st->cmd = 0; st->nargs = 0; st->want_args = 0;
    st->outq_head = st->outq_len = 0;
    st->xfer_mode = SB_XFER_IDLE;
    st->block_left = 0;
    st->paused = 0;
    st->irq_pending = 0;
}

/* --- transfer programming ------------------------------------------------- */
static void sb_start_block(sb_state *st, uint32_t bytes, int autoinit)
{
    st->block_len  = bytes ? bytes : 1;
    st->block_left = st->block_len;
    st->xfer_mode  = autoinit ? SB_XFER_AUTO : SB_XFER_SINGLE;
    st->paused     = 0;
}

/* Time constant -> sample rate. The DSP stores 256 - 1000000/rate, so a game that
   asks for 11025 Hz writes 165; we invert it exactly the same way. */
static uint32_t sb_rate_from_tc(uint8_t tc)
{
    uint32_t d = 256u - tc;
    return d ? (1000000u / d) : 4000u;
}

/* How many argument bytes each command consumes after its opcode. */
static uint8_t sb_cmd_args(uint8_t c)
{
    if (c >= 0xB0 && c <= 0xCF) return 3;       /* mode byte + 16-bit length      */
    switch (c) {
    case 0x10: return 1;                        /* direct DAC sample              */
    case 0x14: case 0x16: case 0x17: return 2;  /* 8-bit single-cycle DMA length  */
    case 0x40: return 1;                        /* time constant                  */
    case 0x41: case 0x42: return 2;             /* output / input rate (big-endian) */
    case 0x48: return 2;                        /* DMA block size                 */
    case 0x80: return 2;                        /* silence period                 */
    case 0xE0: return 1;                        /* identify                       */
    case 0xE4: return 1;                        /* write test register            */
    default:   return 0;
    }
}

static void sb_exec(sb_state *st)
{
    uint8_t c = st->cmd;
    const uint8_t *a = st->args;

    if (c >= 0xB0 && c <= 0xCF) {               /* SB16 programmed transfers      */
        int is16   = (c & 0xF0) == 0xB0;
        int autoin = (c & 0x04) != 0;
        int input  = (c & 0x08) != 0;           /* A/D: we do not record           */
        uint32_t units = (uint32_t)a[1] | ((uint32_t)a[2] << 8);
        st->xfer_16bit  = (uint8_t)is16;
        st->xfer_signed = (a[0] & 0x10) ? 1 : 0;
        st->xfer_stereo = (a[0] & 0x20) ? 1 : 0;
        if (input) { st->xfer_mode = SB_XFER_IDLE; return; }
        sb_start_block(st, (units + 1) * (is16 ? 2u : 1u), autoin);
        return;
    }
    switch (c) {
    case 0x10:                                  /* direct DAC write: no DMA       */
        break;
    case 0x14: case 0x16: case 0x17:            /* 8-bit single-cycle DMA output  */
        st->xfer_16bit = 0; st->xfer_signed = 0; st->xfer_stereo = 0;
        sb_start_block(st, ((uint32_t)a[0] | ((uint32_t)a[1] << 8)) + 1, 0);
        break;
    case 0x1C: case 0x2C:                       /* 8-bit auto-init DMA output     */
        st->xfer_16bit = 0; st->xfer_signed = 0;
        sb_start_block(st, st->block_len, 1);
        break;
    case 0x40:
        st->rate_hz = sb_rate_from_tc(a[0]);
        break;
    case 0x41: case 0x42:                       /* rate is BIG-endian here         */
        st->rate_hz = ((uint32_t)a[0] << 8) | a[1];
        break;
    case 0x48:                                  /* block size for auto-init        */
        st->block_len = ((uint32_t)a[0] | ((uint32_t)a[1] << 8)) + 1;
        break;
    case 0xD0: st->paused = 1; break;           /* pause 8-bit DMA                 */
    case 0xD1: st->speaker = 1; break;
    case 0xD3: st->speaker = 0; break;
    case 0xD4: st->paused = 0; break;           /* continue 8-bit DMA              */
    case 0xD5: st->paused = 1; break;           /* pause 16-bit DMA                */
    case 0xD6: st->paused = 0; break;           /* continue 16-bit DMA             */
    case 0xDA: case 0xD9:                       /* leave auto-init after this block */
        if (st->xfer_mode == SB_XFER_AUTO) st->xfer_mode = SB_XFER_SINGLE;
        break;
    case 0xE0:                                  /* identify: reply with ~arg       */
        sb_outq_push(st, (uint8_t)~a[0]);
        break;
    case 0xE1:                                  /* DSP version                     */
        sb_outq_push(st, SB_DSP_VER_MAJOR);
        sb_outq_push(st, SB_DSP_VER_MINOR);
        break;
    case 0xE3:                                  /* copyright string: NUL is enough */
        sb_outq_push(st, 0);
        break;
    case 0xF2: case 0xF3:                       /* force an IRQ (drivers test wiring) */
        st->irq_pending = 1;
        vdd_raise_irq(st->bus, st->irq);
        break;
    default:
        break;                                  /* unknown commands are ignored     */
    }
}

static void sb_dsp_write(sb_state *st, uint8_t v)
{
    st->dsp_writes++;
    if (st->want_args) {                        /* collecting arguments            */
        if (st->nargs < SB_ARG_MAX) st->args[st->nargs++] = v;
        if (st->nargs >= st->want_args) { sb_exec(st); st->cmd = 0; st->want_args = 0; st->nargs = 0; }
        return;
    }
    st->cmd = v;
    st->nargs = 0;
    st->want_args = sb_cmd_args(v);
    if (!st->want_args) { sb_exec(st); st->cmd = 0; }
}

/* --- ports ---------------------------------------------------------------- */
static void sb_out(void *self, uint16_t port, uint8_t w, uint32_t v)
{
    sb_state *st = (sb_state *)self;
    uint8_t off = (uint8_t)(port - st->base), val = (uint8_t)v;
    (void)w;
    switch (off) {
    case 0x0: case 0x2: case 0x8:               /* FM address (mirrors of 0x388)   */
        if (st->opl) st->opl->index = val;
        break;
    case 0x1: case 0x3: case 0x9:               /* FM data                         */
        if (st->opl) vdd_opl_write_reg(st->opl, st->opl->index, val);
        break;
    case 0x4: st->mix_index = val; break;
    case 0x5: st->mix[st->mix_index] = val; break;
    case 0x6:                                   /* DSP reset                       */
        /* The handshake: 1 then 0. Only the falling edge arms 0xAA, which is what
           a detect is really looking for. */
        if (val & 1) { st->reset_state = 1; sb_dsp_soft_reset(st); }
        else if (st->reset_state) {
            st->reset_state = 0;
            /* ► `nosb.flag`: ANSWER THE PROBE WITH SILENCE, i.e. behave as a machine
                 with no Sound Blaster fitted. Withholding the 0xAA is exactly how a
                 card-less PC fails a detect, so a client takes its own no-sound path
                 rather than being lied to about a device we then cannot service.
                 This is a diagnostic knob, not a policy: Doom dies inside DMX sound
                 init, and this is the way to find out what it does WITHOUT sound
                 without going through the command line (which is separately broken).
                 Absent file = fitted, exactly as before. */
            if (!g_sb_absent) sb_outq_push(st, 0xAA);
        }
        break;
    case 0xC: sb_dsp_write(st, val); break;     /* DSP command / data              */
    default: break;
    }
}

static void sb_in(void *self, uint16_t port, uint8_t w, uint32_t *v)
{
    sb_state *st = (sb_state *)self;
    uint8_t off = (uint8_t)(port - st->base);
    (void)w;
    switch (off) {
    case 0x0: case 0x8:                         /* FM status through the mirror    */
        *v = st->opl ? st->opl->status : 0xFF;
        break;
    case 0x5:                                   /* mixer data                      */
        /* 0x82 is the IRQ-status register: bit 0 = 8-bit DMA, bit 1 = 16-bit. */
        if (st->mix_index == 0x82)
            *v = (uint8_t)(st->irq_pending ? (st->xfer_16bit ? 0x02 : 0x01) : 0x00);
        else
            *v = st->mix[st->mix_index];
        break;
    case 0xA: *v = sb_outq_pop(st); break;      /* DSP read data                   */
    case 0xC: *v = 0x00; break;                 /* write status: never busy        */
    case 0xE:                                   /* read status + 8-bit IRQ ack     */
        *v = (uint8_t)(st->outq_len ? 0xFF : 0x7F);   /* bit 7 = data available    */
        if (!st->xfer_16bit) st->irq_pending = 0;
        break;
    case 0xF:                                   /* 16-bit IRQ ack                  */
        st->irq_pending = 0;
        *v = 0xFF;
        break;
    default: *v = 0xFF; break;
    }
}

/* --- playback ------------------------------------------------------------- */
/* Pull one sample's worth of bytes through the DMA controller and turn it into a
   signed 16-bit value. 8-bit SB data is UNSIGNED (0x80 is silence) unless the
   game said otherwise; 16-bit is signed. Stereo is folded to mono by averaging,
   which is honest for a first cut and keeps the mixer single-channel. */
static int16_t sb_fetch_sample(sb_state *st, int *ended)
{
    uint8_t ch = st->xfer_16bit ? st->dma16 : st->dma8;
    uint8_t raw[4];
    uint32_t want = (uint32_t)(st->xfer_16bit ? 2 : 1) * (st->xfer_stereo ? 2u : 1u);
    uint32_t got;
    int32_t l = 0, r = 0;
    int tc = 0;

    *ended = 0;
    if (!st->dma) return 0;
    got = vdd_dma_read(st->dma, ch, raw, want, &tc);
    if (got < want) { *ended = 1; return 0; }

    if (st->xfer_16bit) {
        l = (int16_t)((uint16_t)raw[0] | ((uint16_t)raw[1] << 8));
        r = st->xfer_stereo ? (int16_t)((uint16_t)raw[2] | ((uint16_t)raw[3] << 8)) : l;
    } else {
        l = st->xfer_signed ? (int8_t)raw[0] * 256 : ((int32_t)raw[0] - 128) * 256;
        r = st->xfer_stereo
              ? (st->xfer_signed ? (int8_t)raw[1] * 256 : ((int32_t)raw[1] - 128) * 256)
              : l;
    }
    if (st->cap_buf && st->cap_len + want <= st->cap_cap) {
        uint32_t i2;
        for (i2 = 0; i2 < want; ++i2) st->cap_buf[st->cap_len++] = raw[i2];
    }
    st->block_left = (st->block_left > want) ? (st->block_left - want) : 0;
    return (int16_t)((l + r) / 2);
}

uint32_t vdd_sb_render(sb_state *st, int16_t *out, uint32_t frames)
{
    uint32_t n;
    for (n = 0; n < frames; ++n) {
        int ended = 0;
        if (st->xfer_mode == SB_XFER_IDLE || st->paused) { out[n] = 0; continue; }

        out[n] = sb_fetch_sample(st, &ended);

        if (st->block_left == 0 || ended) {
            /* Block complete: this is the interrupt the game is waiting for. In
               auto-init it reloads and keeps streaming (the ring buffer every DOS
               game uses); single-cycle stops until reprogrammed. */
            /* ► RECORD THE BOUNDARY BEFORE ANYTHING REACTS TO IT. The IRQ below can
                 reach the guest and the reload has already happened inside dma_step,
                 so this is the only instant at which the three candidate culprits are
                 still distinguishable. Taken before vdd_raise_irq() deliberately. */
            if (st->blocks < SB_BLKLOG_MAX && st->dma) {
                struct sb_blkrec *b = &st->blklog[st->blocks];
                uint8_t dch = st->xfer_16bit ? st->dma16 : st->dma8;
                const dma_chan *c = &st->dma->ch[dch & 7];
                b->cap_off    = st->cap_len;
                b->block_len  = st->block_len;
                b->phys       = vdd_dma_cur_phys(st->dma, dch);
                b->cur_count  = c->cur_count;
                b->base_addr  = c->base_addr;
                b->base_count = c->base_count;
                b->page       = c->page;
                b->mode       = c->mode;
                b->ended      = (uint8_t)ended;
                /* cur_addr back at base means the 8237 wrapped this fetch: the block
                   we just finished and the ring's end coincide. If they routinely do
                   NOT coincide, our block accounting and the guest's disagree, which
                   is exactly the two-frame skew being hunted. */
                b->reloaded   = (uint8_t)(c->cur_addr == c->base_addr);
                st->blklog_n  = st->blocks + 1;      /* entries actually filled */
            }
            st->irq_pending = 1;
            st->blocks++;
            vdd_raise_irq(st->bus, st->irq);
            if (st->xfer_mode == SB_XFER_AUTO && !ended) st->block_left = st->block_len;
            else                                        st->xfer_mode  = SB_XFER_IDLE;
        }
    }
    return frames;
}

/* --- lifecycle ------------------------------------------------------------ */
void vdd_sb_reset(void *self)
{
    sb_state *st = (sb_state *)self;
    vdd_bus *bus = st->bus; dma_state *dma = st->dma; opl_state *opl = st->opl;
    uint16_t base = st->base;
    uint8_t irq = st->irq, d8 = st->dma8, d16 = st->dma16;
    uint32_t dw = st->dsp_writes, bl = st->blocks;
    unsigned i; uint8_t *p = (uint8_t *)st;
    for (i = 0; i < sizeof(*st); ++i) p[i] = 0;
    st->bus = bus; st->dma = dma; st->opl = opl;
    st->base = base; st->irq = irq; st->dma8 = d8; st->dma16 = d16;
    st->dsp_writes = dw; st->blocks = bl;
    st->rate_hz = 22050;
    st->block_len = 1;
    st->mix[0x22] = 0xCC;                       /* master volume, powered-up value */
    st->mix[0x04] = 0xCC;                       /* voice volume                     */
}

int vdd_sb_init(vdd_bus *b, void *self)
{
    sb_state *st = (sb_state *)self;
    st->bus = b;
    if (!st->base)  st->base  = SB_DEFAULT_BASE;
    if (!st->irq)   st->irq   = SB_DEFAULT_IRQ;
    if (!st->dma8)  st->dma8  = SB_DEFAULT_DMA8;
    if (!st->dma16) st->dma16 = SB_DEFAULT_DMA16;
    if (!st->rate_hz)   st->rate_hz = 22050;
    if (!st->block_len) st->block_len = 1;
    if (vdd_claim_ports(b, st->base, (uint16_t)(st->base + 0x0F), sb_in, sb_out, st))
        return -1;
    return 0;
}
