/*
 * vdd_net.c -- NetBIOS through INT 5Ch.  GH #8, s91.  See vdd_net.h.
 */
#include "vdd_net.h"
#include <string.h>

uint8_t vdd_net_service(net_state *st, uint8_t *ncb, uint8_t *buf)
{
    netb_ncb n;
    uint8_t cmd = ncb[0], ret;
    int nowait = (cmd & 0x80) != 0;
    ++st->calls;
    memset(&n, 0, sizeof n);
    n.command = (uint8_t)(cmd & 0x7F);
    n.lsn     = ncb[2];
    n.num     = ncb[3];
    n.buffer  = buf;
    n.length  = (uint16_t)(ncb[8] | (ncb[9] << 8));
    memcpy(n.callname, ncb + 0x0A, 16);
    memcpy(n.name,     ncb + 0x1A, 16);
    n.rto  = ncb[0x2A];
    n.sto  = ncb[0x2B];
    n.lana = ncb[0x30];
    if (!st->submit) {
        /* No NetBIOS on this host at all: the answer a NetBIOS-less adapter
           number gets, so a program's presence test sees an error, not silence. */
        ++st->no_backend;
        ret = NETB_ILLLANA;
    } else {
        ret = st->submit(st->ctx, &n);
    }
    ncb[1] = ret;
    ncb[2] = n.lsn;
    ncb[3] = n.num;
    ncb[8] = (uint8_t)n.length; ncb[9] = (uint8_t)(n.length >> 8);
    /* CALL/LISTEN/RECEIVE ANY report the far end's name in callname */
    memcpy(ncb + 0x0A, n.callname, 16);
    ncb[0x31] = ret;                       /* cmd_cplt: complete */
    st->last_cmd = cmd; st->last_ret = ret;
    if (nowait) {
        ++st->nowait;
        if (ncb[0x2C] | ncb[0x2D] | ncb[0x2E] | ncb[0x2F]) {
            st->post_off = (uint16_t)(ncb[0x2C] | (ncb[0x2D] << 8));
            st->post_seg = (uint16_t)(ncb[0x2E] | (ncb[0x2F] << 8));
            if (st->post_pending) ++st->posts_owed;   /* the previous one was never run */
            st->post_pending = 1;
        }
        /* the immediate code: accepted. A command refused outright (invalid
           command / adapter) is refused immediately as well. */
        return (ret == NETB_ILLCMD || ret == NETB_ILLLANA) ? ret : 0;
    }
    return ret;
}

static void net_int5c(void *self, ntvdd_regs *r)
{
    net_state *st = (net_state *)self;
    uint8_t *ncb = (uint8_t *)vdd_map_flat(st->bus, r->es, r_bx(r));
    uint8_t *buf = NULL;
    uint16_t boff, bseg;
    if (!ncb) { s_al(r, NETB_BADBUF); return; }
    boff = (uint16_t)(ncb[4] | (ncb[5] << 8));
    bseg = (uint16_t)(ncb[6] | (ncb[7] << 8));
    if (boff | bseg) buf = (uint8_t *)vdd_map_flat(st->bus, bseg, boff);
    s_al(r, vdd_net_service(st, ncb, buf));
}

/* INT 2Ah, the network/critical-section interface (Microsoft Networks):
     AH=00h installation check -> AH<>0 (stock NTVDM: 01h, p_netb)
     AH=01h execute NetBIOS request with error retry, AH=04h without: ES:BX = NCB,
            AL = the NCB's command on entry; returns AL = retcode, AH = 00h success /
            01h error
     AH=80h/81h/82h begin/end critical section, end all: nothing to serialise here
   Anything else returns with the registers as they came. */
static void net_int2a(void *self, ntvdd_regs *r)
{
    net_state *st = (net_state *)self;
    switch (r_ah(r)) {
    case 0x00:
        s_ah(r, st->submit ? 0x01 : 0x00);
        break;
    case 0x01: case 0x04: {
        uint8_t ret;
        net_int5c(self, r);
        ret = r_al(r);
        s_ah(r, ret ? 0x01 : 0x00);
        break; }
    default:
        break;
    }
}

int vdd_net_init(vdd_bus *b, void *self)
{
    net_state *st = (net_state *)self;
    st->bus = b;
    if (vdd_claim_int(b, 0x5C, net_int5c, st) != 0) return -1;
    return vdd_claim_int(b, 0x2A, net_int2a, st) != 0 ? -1 : 0;
}

void vdd_net_reset(void *self) { (void)self; }

void vdd_net_set_backend(net_state *st, netb_submit_fn fn, void *ctx)
{ st->submit = fn; st->ctx = ctx; }
