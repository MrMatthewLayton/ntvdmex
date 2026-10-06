/*
 * vdd_net.c -- NetBIOS through INT 5Ch.  GH #8, s91.  See vdd_net.h.
 */
#include "vdd_net.h"
#include <string.h>

/* The DOS NCB's layout (see vdd_net.h). */
#define NETB_NCB_COMMAND           0x00
#define NETB_NCB_RETCODE           0x01
#define NETB_NCB_LSN               0x02
#define NETB_NCB_NUM               0x03
#define NETB_NCB_BUFFER_OFFSET     0x04
#define NETB_NCB_BUFFER_SEGMENT    0x06
#define NETB_NCB_LENGTH            0x08
#define NETB_NCB_CALLNAME          0x0A
#define NETB_NCB_NAME              0x1A
#define NETB_NCB_RTO               0x2A
#define NETB_NCB_STO               0x2B
#define NETB_NCB_POST_OFFSET       0x2C
#define NETB_NCB_POST_SEGMENT      0x2E
#define NETB_NCB_LANA              0x30
#define NETB_NCB_CMD_CPLT          0x31
#define NETB_HIGH_BYTE             1      /* a little-endian word's second byte       */
#define NETB_BYTE_SHIFT            8
#define NETB_NO_WAIT_BIT           0x80   /* command bit 7: no-wait form              */
#define NETB_COMMAND_MASK          0x7F
#define NETB_IMMEDIATE_ACCEPTED    0      /* AL for a no-wait command that was taken  */

/* The interrupts and INT 2Ah's functions. */
#define NETB_INT_NETBIOS           0x5C
#define NETB_INT_NETWORK           0x2A
#define NETB_2A_INSTALLATION_CHECK 0x00
#define NETB_2A_EXECUTE_RETRY      0x01
#define NETB_2A_EXECUTE            0x04
#define NETB_2A_INSTALLED          0x01   /* AH from the installation check           */
#define NETB_2A_NOT_INSTALLED      0x00
#define NETB_2A_ERROR              0x01   /* AH after an execute: 01h error, 00h ok   */
#define NETB_2A_SUCCESS            0x00
#define NETB_OK                    0
#define NETB_FAILED                (-1)

BYTE VddNetBiosService(PNETBIOS_STATE state, BYTE *ncb, BYTE *buffer)
{
    NETBIOS_REQUEST request;
    BYTE command = ncb[NETB_NCB_COMMAND], returnCode;
    INT isNoWait = (command & NETB_NO_WAIT_BIT) != 0;
    ++state->Calls;
    memset(&request, 0, sizeof request);
    request.Command = (BYTE)(command & NETB_COMMAND_MASK);
    request.LocalSession     = ncb[NETB_NCB_LSN];
    request.NameNumber     = ncb[NETB_NCB_NUM];
    request.Buffer  = buffer;
    request.Length  = (WORD)(ncb[NETB_NCB_LENGTH] | (ncb[NETB_NCB_LENGTH + NETB_HIGH_BYTE] << NETB_BYTE_SHIFT));
    memcpy(request.CallName, ncb + NETB_NCB_CALLNAME, NETB_NAME_SIZE);
    memcpy(request.Name,     ncb + NETB_NCB_NAME, NETB_NAME_SIZE);
    request.ReceiveTimeout  = ncb[NETB_NCB_RTO];
    request.SendTimeout  = ncb[NETB_NCB_STO];
    request.Adapter = ncb[NETB_NCB_LANA];
    if (!state->Submit) {
        /* No NetBIOS on this host at all: the answer a NetBIOS-less adapter
           number gets, so a program's presence test sees an error, not silence. */
        ++state->NoBackendCalls;
        returnCode = NETB_RC_INVALID_ADAPTER;
    } else {
        returnCode = state->Submit(state->SubmitContext, &request);
    }
    ncb[NETB_NCB_RETCODE] = returnCode;
    ncb[NETB_NCB_LSN] = request.LocalSession;
    ncb[NETB_NCB_NUM] = request.NameNumber;
    ncb[NETB_NCB_LENGTH] = (BYTE)request.Length; ncb[NETB_NCB_LENGTH + NETB_HIGH_BYTE] = (BYTE)(request.Length >> NETB_BYTE_SHIFT);
    /* CALL/LISTEN/RECEIVE ANY report the far end's name in callname */
    memcpy(ncb + NETB_NCB_CALLNAME, request.CallName, NETB_NAME_SIZE);
    ncb[NETB_NCB_CMD_CPLT] = returnCode;                       /* cmd_cplt: complete */
    state->LastCommand = command; state->LastReturnCode = returnCode;
    if (isNoWait) {
        ++state->NoWaitCalls;
        if (ncb[NETB_NCB_POST_OFFSET] | ncb[NETB_NCB_POST_OFFSET + NETB_HIGH_BYTE] | ncb[NETB_NCB_POST_SEGMENT] | ncb[NETB_NCB_POST_SEGMENT + NETB_HIGH_BYTE]) {
            state->PostOffset = (WORD)(ncb[NETB_NCB_POST_OFFSET] | (ncb[NETB_NCB_POST_OFFSET + NETB_HIGH_BYTE] << NETB_BYTE_SHIFT));
            state->PostSegment = (WORD)(ncb[NETB_NCB_POST_SEGMENT] | (ncb[NETB_NCB_POST_SEGMENT + NETB_HIGH_BYTE] << NETB_BYTE_SHIFT));
            if (state->IsPostPending) ++state->PostsOwed;   /* the previous one was never run */
            state->IsPostPending = TRUE;
        }
        /* the immediate code: accepted. A command refused outright (invalid
           command / adapter) is refused immediately as well. */
        return (returnCode == NETB_RC_INVALID_COMMAND || returnCode == NETB_RC_INVALID_ADAPTER) ? returnCode : NETB_IMMEDIATE_ACCEPTED;
    }
    return returnCode;
}

static VOID VddNetBiosInt5C(PVOID context, PNTVDD_REGISTERS registers)
{
    PNETBIOS_STATE state = (PNETBIOS_STATE)context;
    BYTE *ncb = (BYTE *)VddMapFlat(state->Bus, registers->Es, VddGetBx(registers));
    BYTE *buffer = NULL;
    WORD bufferOffset, bufferSegment;
    if (!ncb) { VddSetAl(registers, NETB_RC_INVALID_BUFFER); return; }
    bufferOffset = (WORD)(ncb[NETB_NCB_BUFFER_OFFSET] | (ncb[NETB_NCB_BUFFER_OFFSET + NETB_HIGH_BYTE] << NETB_BYTE_SHIFT));
    bufferSegment = (WORD)(ncb[NETB_NCB_BUFFER_SEGMENT] | (ncb[NETB_NCB_BUFFER_SEGMENT + NETB_HIGH_BYTE] << NETB_BYTE_SHIFT));
    if (bufferOffset | bufferSegment) buffer = (BYTE *)VddMapFlat(state->Bus, bufferSegment, bufferOffset);
    VddSetAl(registers, VddNetBiosService(state, ncb, buffer));
}

/* INT 2Ah, the network/critical-section interface (Microsoft Networks):
     AH=00h installation check -> AH<>0 (stock NTVDM: 01h, p_netb)
     AH=01h execute NetBIOS request with error retry, AH=04h without: ES:BX = NCB,
            AL = the NCB's command on entry; returns AL = retcode, AH = 00h success /
            01h error
     AH=80h/81h/82h begin/end critical section, end all: nothing to serialise here
   Anything else returns with the registers as they came. */
static VOID VddNetBiosInt2A(PVOID context, PNTVDD_REGISTERS registers)
{
    PNETBIOS_STATE state = (PNETBIOS_STATE)context;
    switch (VddGetAh(registers)) {
    case NETB_2A_INSTALLATION_CHECK:
        VddSetAh(registers, state->Submit ? NETB_2A_INSTALLED : NETB_2A_NOT_INSTALLED);
        break;
    case NETB_2A_EXECUTE_RETRY: case NETB_2A_EXECUTE: {
        BYTE returnCode;
        VddNetBiosInt5C(context, registers);
        returnCode = VddGetAl(registers);
        VddSetAh(registers, returnCode ? NETB_2A_ERROR : NETB_2A_SUCCESS);
        break; }
    default:
        break;
    }
}

INT VddNetBiosInitialize(PVDD_BUS bus, PVOID context)
{
    PNETBIOS_STATE state = (PNETBIOS_STATE)context;
    state->Bus = bus;
    if (VddClaimInterrupt(bus, NETB_INT_NETBIOS, VddNetBiosInt5C, state) != NETB_OK) return NETB_FAILED;
    return VddClaimInterrupt(bus, NETB_INT_NETWORK, VddNetBiosInt2A, state) != NETB_OK ? NETB_FAILED : NETB_OK;
}

VOID VddNetBiosReset(PVOID context) { (VOID)context; }

VOID VddNetBiosSetBackend(PNETBIOS_STATE state, PNETBIOS_SUBMIT_ROUTINE submitRoutine, PVOID context)
{ state->Submit = submitRoutine; state->SubmitContext = context; }
