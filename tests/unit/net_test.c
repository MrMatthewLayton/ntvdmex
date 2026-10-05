/*
 * net_test.c -- the NetBIOS device (src/vdd/vdd_net.c), off-VM.  GH #8, s91.
 *
 * The device's job is the DOS side: read the 64-byte NCB, hand the backend the right
 * fields, and write the answer back where DOS keeps it. A fake backend records what
 * it was given and answers what the test tells it to, so every field crossing is
 * checked both ways without a network.
 */
#include <stdio.h>
#include <string.h>
#include "vdd_net.h"

static int total = 0, fails = 0;
#define CHECK(c,m) do{ total++; if(c){printf("  PASS  %s\n",(m));} \
    else{printf("  FAIL  %s\n",(m)); fails++;} }while(0)

static netb_ncb g_seen;
static int      g_calls;
static uint8_t  g_ret, g_lsn, g_num;
static uint16_t g_len;

static uint8_t fake(void *ctx, netb_ncb *n)
{
    (void)ctx;
    g_seen = *n; ++g_calls;
    n->lsn = g_lsn; n->num = g_num;
    if (g_len) n->length = g_len;
    if (n->command == 0x10 /* CALL */) memcpy(n->callname, "FAREND          ", 16);
    if (n->buffer && n->length) n->buffer[0] = 0xAB;
    n->retcode = g_ret;
    return g_ret;
}

int main(void)
{
    net_state st;
    uint8_t ncb[NETB_NCB_SIZE], buf[64];
    uint8_t al;
    memset(&st, 0, sizeof st);

    /* ── no backend: an error, never silence ── */
    memset(ncb, 0, sizeof ncb); ncb[0] = 0x7F;
    al = vdd_net_service(&st, ncb, NULL);
    CHECK(al == NETB_ILLLANA && ncb[1] == NETB_ILLLANA, "no host NetBIOS: invalid adapter (23h) in AL and retcode");
    CHECK(ncb[0x31] == NETB_ILLLANA, "...and cmd_cplt says complete with that code");

    vdd_net_set_backend(&st, fake, NULL);

    /* ── a wait command: every field in, every answer out ── */
    memset(ncb, 0, sizeof ncb);
    ncb[0] = 0x30;                                 /* ADD NAME */
    memcpy(ncb + 0x1A, "NTVDMEXPROBE    ", 16);
    memcpy(ncb + 0x0A, "CALLNAMEXXXXXXXX", 16);
    ncb[2] = 7; ncb[3] = 9; ncb[0x2A] = 5; ncb[0x2B] = 6; ncb[0x30] = 1;
    g_ret = 0; g_lsn = 0x11; g_num = 0x22; g_len = 0;
    al = vdd_net_service(&st, ncb, NULL);
    CHECK(g_calls == 1 && g_seen.command == 0x30, "ADD NAME reaches the backend as 30h");
    CHECK(!memcmp(g_seen.name, "NTVDMEXPROBE    ", 16), "name[16] read from NCB+1Ah");
    CHECK(!memcmp(g_seen.callname, "CALLNAMEXXXXXXXX", 16), "callname[16] read from NCB+0Ah");
    CHECK(g_seen.lsn == 7 && g_seen.num == 9, "lsn/num read from +02/+03");
    CHECK(g_seen.rto == 5 && g_seen.sto == 6 && g_seen.lana == 1, "rto/sto/lana read from +2A/+2B/+30");
    CHECK(al == 0 && ncb[1] == 0 && ncb[0x31] == 0, "success: AL, retcode and cmd_cplt all 00");
    CHECK(ncb[2] == 0x11 && ncb[3] == 0x22, "the lsn and name number the backend gave are written back");

    /* ── a buffer and its length ── */
    memset(ncb, 0, sizeof ncb); memset(buf, 0, sizeof buf);
    ncb[0] = 0x33; ncb[0x0A] = '*'; ncb[8] = 64; ncb[9] = 0;
    g_len = 60;
    al = vdd_net_service(&st, ncb, buf);
    CHECK(g_seen.buffer == buf && g_seen.length == 64, "ADAPTER STATUS: the resolved buffer and its 64-byte length handed over");
    CHECK(buf[0] == 0xAB, "the backend writes straight into the guest's buffer");
    CHECK(ncb[8] == 60 && ncb[9] == 0, "the length actually returned (60) is written back to +08");
    g_len = 0;

    /* ── an error retcode ── */
    memset(ncb, 0, sizeof ncb); ncb[0] = 0x7F; g_ret = NETB_ILLCMD;
    al = vdd_net_service(&st, ncb, NULL);
    CHECK(al == NETB_ILLCMD && ncb[1] == NETB_ILLCMD, "invalid command: 03h in AL and retcode (the presence test)");

    /* ── CALL returns the far end's name ── */
    memset(ncb, 0, sizeof ncb); ncb[0] = 0x10; g_ret = 0;
    al = vdd_net_service(&st, ncb, NULL);
    CHECK(!memcmp(ncb + 0x0A, "FAREND          ", 16), "CALL: callname comes back from the backend");

    /* ── no-wait ── */
    memset(ncb, 0, sizeof ncb); ncb[0] = 0x80 | 0x30; g_ret = 0x0D;   /* duplicate name */
    al = vdd_net_service(&st, ncb, NULL);
    CHECK(g_seen.command == 0x30, "no-wait: the backend sees the WAIT form (bit 7 stripped)");
    CHECK(al == 0, "no-wait: AL is the IMMEDIATE code -- accepted (00)");
    CHECK(ncb[1] == 0x0D && ncb[0x31] == 0x0D, "no-wait: the FINAL code is in retcode and cmd_cplt (not FFh: complete)");
    CHECK(st.posts_owed == 0, "no POST routine given: none owed");
    memset(ncb, 0, sizeof ncb); ncb[0] = 0x80 | 0x31; ncb[0x2C] = 0x34; ncb[0x2E] = 0x12; g_ret = 0;
    vdd_net_service(&st, ncb, NULL);
    CHECK(st.post_pending && st.post_seg == 0x0012 && st.post_off == 0x0034,
          "a POST routine is recorded for the host to run (seg:off from NCB+2Ch)");
    CHECK(st.posts_owed == 0, "...and nothing is owed while the host has not missed one");
    st.post_pending = 0;
    memset(ncb, 0, sizeof ncb); ncb[0] = 0xFF; g_ret = NETB_ILLCMD;
    al = vdd_net_service(&st, ncb, NULL);
    CHECK(al == NETB_ILLCMD, "no-wait invalid command is refused IMMEDIATELY (03h in AL)");

    printf("\n== %d checks, %d failed\n", total, fails);
    return fails ? 1 : 0;
}
