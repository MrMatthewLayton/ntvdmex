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

static INT g_Total = 0, g_Failures = 0;
#define CHECK(condition,message) do{ g_Total++; if(condition){printf("  PASS  %s\n",(message));} \
    else{printf("  FAIL  %s\n",(message)); g_Failures++;} }while(0)

static NETBIOS_REQUEST g_SeenRequest;
static INT      g_Calls;
static BYTE  g_ReturnCode, g_LocalSession, g_NameNumber;
static WORD g_Length;

static BYTE NetTestFakeBackend(PVOID context, PNETBIOS_REQUEST request)
{
    (VOID)context;
    g_SeenRequest = *request; ++g_Calls;
    request->LocalSession = g_LocalSession; request->NameNumber = g_NameNumber;
    if (g_Length) request->Length = g_Length;
    if (request->Command == 0x10 /* CALL */) memcpy(request->CallName, "FAREND          ", 16);
    if (request->Buffer && request->Length) request->Buffer[0] = 0xAB;
    request->ReturnCode = g_ReturnCode;
    return g_ReturnCode;
}

INT main(VOID)
{
    NETBIOS_STATE state;
    BYTE controlBlock[NETB_NCB_SIZE], buffer[64];
    BYTE returnCode;
    memset(&state, 0, sizeof state);

    /* ── no backend: an error, never silence ── */
    memset(controlBlock, 0, sizeof controlBlock); controlBlock[0] = 0x7F;
    returnCode = VddNetBiosService(&state, controlBlock, NULL);
    CHECK(returnCode == NETB_RC_INVALID_ADAPTER && controlBlock[1] == NETB_RC_INVALID_ADAPTER, "no host NetBIOS: invalid adapter (23h) in AL and retcode");
    CHECK(controlBlock[0x31] == NETB_RC_INVALID_ADAPTER, "...and cmd_cplt says complete with that code");

    VddNetBiosSetBackend(&state, NetTestFakeBackend, NULL);

    /* ── a wait command: every field in, every answer out ── */
    memset(controlBlock, 0, sizeof controlBlock);
    controlBlock[0] = 0x30;                                 /* ADD NAME */
    memcpy(controlBlock + 0x1A, "NTVDMEXPROBE    ", 16);
    memcpy(controlBlock + 0x0A, "CALLNAMEXXXXXXXX", 16);
    controlBlock[2] = 7; controlBlock[3] = 9; controlBlock[0x2A] = 5; controlBlock[0x2B] = 6; controlBlock[0x30] = 1;
    g_ReturnCode = 0; g_LocalSession = 0x11; g_NameNumber = 0x22; g_Length = 0;
    returnCode = VddNetBiosService(&state, controlBlock, NULL);
    CHECK(g_Calls == 1 && g_SeenRequest.Command == 0x30, "ADD NAME reaches the backend as 30h");
    CHECK(!memcmp(g_SeenRequest.Name, "NTVDMEXPROBE    ", 16), "name[16] read from NCB+1Ah");
    CHECK(!memcmp(g_SeenRequest.CallName, "CALLNAMEXXXXXXXX", 16), "callname[16] read from NCB+0Ah");
    CHECK(g_SeenRequest.LocalSession == 7 && g_SeenRequest.NameNumber == 9, "lsn/num read from +02/+03");
    CHECK(g_SeenRequest.ReceiveTimeout == 5 && g_SeenRequest.SendTimeout == 6 && g_SeenRequest.Adapter == 1, "rto/sto/lana read from +2A/+2B/+30");
    CHECK(returnCode == 0 && controlBlock[1] == 0 && controlBlock[0x31] == 0, "success: AL, retcode and cmd_cplt all 00");
    CHECK(controlBlock[2] == 0x11 && controlBlock[3] == 0x22, "the lsn and name number the backend gave are written back");

    /* ── a buffer and its length ── */
    memset(controlBlock, 0, sizeof controlBlock); memset(buffer, 0, sizeof buffer);
    controlBlock[0] = 0x33; controlBlock[0x0A] = '*'; controlBlock[8] = 64; controlBlock[9] = 0;
    g_Length = 60;
    returnCode = VddNetBiosService(&state, controlBlock, buffer);
    CHECK(g_SeenRequest.Buffer == buffer && g_SeenRequest.Length == 64, "ADAPTER STATUS: the resolved buffer and its 64-byte length handed over");
    CHECK(buffer[0] == 0xAB, "the backend writes straight into the guest's buffer");
    CHECK(controlBlock[8] == 60 && controlBlock[9] == 0, "the length actually returned (60) is written back to +08");
    g_Length = 0;

    /* ── an error retcode ── */
    memset(controlBlock, 0, sizeof controlBlock); controlBlock[0] = 0x7F; g_ReturnCode = NETB_RC_INVALID_COMMAND;
    returnCode = VddNetBiosService(&state, controlBlock, NULL);
    CHECK(returnCode == NETB_RC_INVALID_COMMAND && controlBlock[1] == NETB_RC_INVALID_COMMAND, "invalid command: 03h in AL and retcode (the presence test)");

    /* ── CALL returns the far end's name ── */
    memset(controlBlock, 0, sizeof controlBlock); controlBlock[0] = 0x10; g_ReturnCode = 0;
    returnCode = VddNetBiosService(&state, controlBlock, NULL);
    CHECK(!memcmp(controlBlock + 0x0A, "FAREND          ", 16), "CALL: callname comes back from the backend");

    /* ── no-wait ── */
    memset(controlBlock, 0, sizeof controlBlock); controlBlock[0] = 0x80 | 0x30; g_ReturnCode = 0x0D;   /* duplicate name */
    returnCode = VddNetBiosService(&state, controlBlock, NULL);
    CHECK(g_SeenRequest.Command == 0x30, "no-wait: the backend sees the WAIT form (bit 7 stripped)");
    CHECK(returnCode == 0, "no-wait: AL is the IMMEDIATE code -- accepted (00)");
    CHECK(controlBlock[1] == 0x0D && controlBlock[0x31] == 0x0D, "no-wait: the FINAL code is in retcode and cmd_cplt (not FFh: complete)");
    CHECK(state.PostsOwed == 0, "no POST routine given: none owed");
    memset(controlBlock, 0, sizeof controlBlock); controlBlock[0] = 0x80 | 0x31; controlBlock[0x2C] = 0x34; controlBlock[0x2E] = 0x12; g_ReturnCode = 0;
    VddNetBiosService(&state, controlBlock, NULL);
    CHECK(state.IsPostPending && state.PostSegment == 0x0012 && state.PostOffset == 0x0034,
          "a POST routine is recorded for the host to run (seg:off from NCB+2Ch)");
    CHECK(state.PostsOwed == 0, "...and nothing is owed while the host has not missed one");
    state.IsPostPending = 0;
    memset(controlBlock, 0, sizeof controlBlock); controlBlock[0] = 0xFF; g_ReturnCode = NETB_RC_INVALID_COMMAND;
    returnCode = VddNetBiosService(&state, controlBlock, NULL);
    CHECK(returnCode == NETB_RC_INVALID_COMMAND, "no-wait invalid command is refused IMMEDIATELY (03h in AL)");

    printf("\n== %d checks, %d failed\n", g_Total, g_Failures);
    return g_Failures ? 1 : 0;
}
