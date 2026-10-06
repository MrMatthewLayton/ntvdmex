/* vdd_mpu.c -- see vdd_mpu.h.  MPU-401 UART-mode MIDI, on the VDD bus.
 * Pure C, no <windows.h>. */
#include "vdd_mpu.h"

static void mpu_inq_push(mpu_state *st, uint8_t v)
{
    if (st->inq_len >= MPU_INQ_MAX) return;
    st->inq[(st->inq_head + st->inq_len) % MPU_INQ_MAX] = v;
    st->inq_len++;
}
static uint8_t mpu_inq_pop(mpu_state *st)
{
    uint8_t v;
    if (!st->inq_len) return 0xFF;
    v = st->inq[st->inq_head];
    st->inq_head = (uint8_t)((st->inq_head + 1) % MPU_INQ_MAX);
    st->inq_len--;
    return v;
}

/* How many data bytes follow a status byte. Program change and channel pressure
   take one; everything else in the channel range takes two. */
static uint8_t mpu_data_len(uint8_t status)
{
    switch (status & 0xF0) {
    case 0xC0: case 0xD0: return 1;
    case 0x80: case 0x90: case 0xA0: case 0xB0: case 0xE0: return 2;
    default: break;
    }
    switch (status) {                           /* system common                   */
    case 0xF1: case 0xF3: return 1;
    case 0xF2: return 2;
    default: return 0;                          /* realtime / undefined            */
    }
}

static void mpu_emit(mpu_state *st)
{
    uint32_t msg = (uint32_t)st->status
                 | ((uint32_t)st->data[0] << 8)
                 | ((uint32_t)st->data[1] << 16);
    st->sent++;
    if (st->sink) st->sink(st->sink_ctx, msg);
}

/* #136: SysEx assembly for an attached sysex_sink. No-ops without one. */
static void mpu_sysex_put(mpu_state *st, uint8_t b)
{
    if (!st->sysex_sink || st->sysex_over) return;
    if (st->sysex_len >= MPU_SYSEX_MAX) { st->sysex_over = 1; return; }
    st->sysex[st->sysex_len++] = b;
}
static void mpu_sysex_begin(mpu_state *st)
{
    st->sysex_len = 0; st->sysex_over = 0;
    mpu_sysex_put(st, 0xF0);
}
static void mpu_sysex_end(mpu_state *st)
{
    if (!st->sysex_sink) return;
    mpu_sysex_put(st, 0xF7);
    if (st->sysex_over) { st->sysex_dropped++; return; }
    st->sysex_sent++;
    st->sysex_sink(st->sysex_ctx, st->sysex, st->sysex_len);
}

/* Assemble a MIDI byte stream into whole messages. Two details matter for real
   game output: RUNNING STATUS (a stream may send several note-ons under one
   status byte, which is how sequencers save bandwidth) and realtime bytes, which
   may appear ANYWHERE -- even between the data bytes of another message -- and
   must not disturb the message being assembled. */
static void mpu_midi_byte(mpu_state *st, uint8_t b)
{
    if (b >= 0xF8) {                            /* realtime: standalone, no data   */
        uint8_t save_status = st->status;
        uint8_t s0 = st->data[0], s1 = st->data[1], n = st->ndata;
        st->status = b; st->data[0] = st->data[1] = 0;
        mpu_emit(st);
        st->status = save_status; st->data[0] = s0; st->data[1] = s1; st->ndata = n;
        return;
    }
    /* SysEx: swallowed, unless a synth that wants it is attached (#136, see the header).
       ⚠ The swallow path is byte-for-byte the old one, including that a status byte
         inside an unterminated message is swallowed too. With a sink attached that byte
         ENDS the message (MIDI 1.0: any status but a realtime one terminates SysEx); the
         unfinished message is dropped and counted, and the status byte is handled as
         what it is. */
    if (b == 0xF0) { st->in_sysex = 1; mpu_sysex_begin(st); return; }
    if (b == 0xF7) { if (st->in_sysex) mpu_sysex_end(st); st->in_sysex = 0; return; }
    if (st->in_sysex) {
        if (!st->sysex_sink) return;
        if (!(b & 0x80)) { mpu_sysex_put(st, b); return; }
        st->in_sysex = 0; st->sysex_dropped++;
    }

    if (b & 0x80) {                             /* new status byte                 */
        st->status = b;
        st->ndata = 0;
        st->want_data = mpu_data_len(b);
        if (!st->want_data) { st->data[0] = st->data[1] = 0; mpu_emit(st); }
        return;
    }
    if (!st->status) return;                    /* data with no status: ignore     */
    if (st->ndata < 2) st->data[st->ndata] = b;
    st->ndata++;
    if (st->ndata >= st->want_data) {
        if (st->want_data < 2) st->data[1] = 0;
        mpu_emit(st);
        st->ndata = 0;                          /* running status: keep st->status */
    }
}

void vdd_mpu_feed(mpu_state *st, uint8_t byte) { mpu_midi_byte(st, byte); }

static void mpu_out(void *self, uint16_t port, uint8_t w, uint32_t v)
{
    mpu_state *st = (mpu_state *)self;
    uint8_t val = (uint8_t)v;
    (void)w;
    if (port == st->base) {                     /* data port                        */
        if (st->uart_mode) mpu_midi_byte(st, val);
        return;
    }
    /* command port: only the two commands every game issues are implemented, and
       both must be acknowledged or the driver decides there is no interface. */
    switch (val) {
    case 0xFF:                                  /* reset                            */
        st->uart_mode = 0; st->status = 0; st->ndata = 0; st->in_sysex = 0;
        mpu_inq_push(st, MPU_ACK);
        break;
    case 0x3F:                                  /* enter UART mode                  */
        st->uart_mode = 1;
        mpu_inq_push(st, MPU_ACK);
        break;
    default:
        mpu_inq_push(st, MPU_ACK);              /* acknowledge and ignore           */
        break;
    }
}

static void mpu_in(void *self, uint16_t port, uint8_t w, uint32_t *v)
{
    mpu_state *st = (mpu_state *)self;
    (void)w;
    if (port == st->base) { *v = mpu_inq_pop(st); return; }
    /* Status: BOTH flags are active low. DRR clear = we can take a byte (always);
       DSR clear = a byte is waiting to be read. */
    *v = (uint8_t)(st->inq_len ? 0x00 : MPU_ST_DSR);
}

void vdd_mpu_reset(void *self)
{
    mpu_state *st = (mpu_state *)self;
    VDD_BUS *bus = st->bus; uint16_t base = st->base;
    mpu_midi_sink sink = st->sink; void *ctx = st->sink_ctx;
    mpu_sysex_sink xsink = st->sysex_sink; void *xctx = st->sysex_ctx;   /* #136 */
    unsigned i; uint8_t *p = (uint8_t *)st;
    for (i = 0; i < sizeof(*st); ++i) p[i] = 0;
    st->bus = bus; st->base = base; st->sink = sink; st->sink_ctx = ctx;
    st->sysex_sink = xsink; st->sysex_ctx = xctx;
}

int vdd_mpu_init(VDD_BUS *b, void *self)
{
    mpu_state *st = (mpu_state *)self;
    st->bus = b;
    if (!st->base) st->base = MPU_DEFAULT_BASE;
    if (VddClaimPorts(b, st->base, (uint16_t)(st->base + 1), mpu_in, mpu_out, st))
        return -1;
    return 0;
}
