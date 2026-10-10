/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * See vdd_dma.h.  A pair of Intel 8237A DMA controllers plus the
 * AT page registers, on the VDD bus.  Pure C, no <windows.h>.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include "vdd_dma.h"

/* Channels, controllers and the cascade (the AT's two 8237As). */
#define DMA_CHANNEL_MASK                7           /* Channels 0-7 */
#define DMA_CHANNELS_PER_CONTROLLER     4
#define DMA_CONTROLLER_SHIFT            2           /* channel >> 2 = its controller */
#define DMA_LINE_MASK                   3           /* A channel's line within its controller */
#define DMA_CASCADE_CHANNEL             4           /* controller 2's channel 0 carries controller 1 */
#define DMA_CASCADE_BIT                 0x10        /* channel 4 in a DREQ mask */
#define DMA_CONTROLLER2_LINES           0x0E        /* controller 2's lines 1-3 (0 is the cascade) */
#define DMA_CONTROLLER2_SHIFT           4           /* controller 2's lines in a DREQ mask */
#define DMA_CONTROLLER_LINES            0x0F
#define DMA_MEMORY_TO_MEMORY_LINES      0x03        /* Channels 0 and 1 */

/* Addresses. */
#define DMA_PAGE_SHIFT                  16          /* The page register is address bits 23-16 */
#define DMA_WORD_PAGE_MASK              0xFE        /* 16-bit channels: page bit 0 is not used */
#define DMA_WORD_UNIT                   2           /* A 16-bit channel moves words */
#define DMA_WORD_UNIT_BYTES             2u
#define DMA_COUNT_EXPIRED               0xFFFF      /* The count after terminal count */
#define DMA_MAX_TRANSFERS               0x10001u    /* A full count of 65536, plus one */
#define DMA_FLOATING_BYTE               0xFF

/* The 16-bit address/count registers, written and read a byte at a time. */
#define DMA_KEEP_LOW_BYTE               0x00FF
#define DMA_KEEP_HIGH_BYTE              0xFF00

/* Ports: controller 1 at 00h-0Fh, controller 2 at C0h-DFh (word-spaced), pages at 80h-8Fh. */
#define DMA_CONTROLLER1_FIRST_PORT      0x00
#define DMA_CONTROLLER1_LAST_PORT       0x0F
#define DMA_CONTROLLER2_FIRST_PORT      0xC0
#define DMA_CONTROLLER2_LAST_PORT       0xDF
#define DMA_PAGE_FIRST_PORT             0x80
#define DMA_PAGE_LAST_PORT              0x8F
#define DMA_PAGE_INDEX_MASK             0x0F
#define DMA_PAGE_CHANNEL0               0x87
#define DMA_PAGE_CHANNEL1               0x83
#define DMA_PAGE_CHANNEL2               0x81
#define DMA_PAGE_CHANNEL3               0x82
#define DMA_PAGE_CHANNEL5               0x8B
#define DMA_PAGE_CHANNEL6               0x89
#define DMA_PAGE_CHANNEL7               0x8A
#define DMA_CHANNEL0                    0
#define DMA_CHANNEL1                    1
#define DMA_CHANNEL2                    2
#define DMA_CHANNEL3                    3
#define DMA_CHANNEL5                    5
#define DMA_CHANNEL6                    6
#define DMA_CHANNEL7                    7
#define DMA_NO_CHANNEL                  (-1)

/* The controller registers (8237A), by index. */
#define DMA_ADDRESS_COUNT_REGISTERS     8           /* 0-7: address/count pairs, channels 0-3 */
#define DMA_REGISTER_COMMAND            0x8         /* Write: command; read: status */
#define DMA_REGISTER_REQUEST            0x9
#define DMA_REGISTER_SINGLE_MASK        0xA
#define DMA_REGISTER_MODE               0xB
#define DMA_REGISTER_CLEAR_FLIP_FLOP    0xC
#define DMA_REGISTER_MASTER_CLEAR       0xD         /* Write: master clear; read: temporary */
#define DMA_REGISTER_CLEAR_MASK         0xE
#define DMA_REGISTER_WRITE_ALL_MASK     0xF
#define DMA_SET_BIT                     4           /* Request/mask register: set (1) or reset (0) */
#define DMA_STATUS_REQUEST_SHIFT        4           /* status bits 7:4: DREQ */
#define DMA_WORD_ACCESS                 2           /* A 16-bit IN */
#define DMA_OK                          0
#define DMA_FAILED                      (-1)

/* THE 8237 AS ITS OWN BUS MASTER: SOFTWARE REQUESTS. (#246):
 * Every transfer above is PULLED by a device. A request-register bit is a DREQ no
 * device made, so the controller has to carry the cycles out itself -- and nothing
 * is on the other end of DACK. What that means, per transfer type:
 *   verify (00)  address and count walk; no memory cycle at all (the datasheet's
 *                "pseudo transfers")
 *   read   (10)  memory is read onto the bus for a device that is not listening:
 *                nothing observable but the walk
 *   write  (01)  memory is WRITTEN from a data bus nobody drives: the bytes are
 *                FFh, the same float every unclaimed port reads on this host
 *   11           illegal; treated as verify, which touches nothing
 * The request stays asserted until TC ("cleared upon generation of a TC"), so in
 * demand, single and block mode alike the channel runs to terminal count. A
 * channel in CASCADE mode does no cycles of its own and ignores it.
 *
 * [CAUTION]: MEMORY OUTSIDE THE VDM IS NOT TOUCHED. A guest linear address is a host VA only
 * for the V86 space (the first 1 MB + the HMA); the 8237's 24-bit physical address
 * has no meaning beyond it here, and a write there could land anywhere in the host
 * process. Cycles beyond 110000h walk and count, and move nothing.
 */
#define DMA_PHYSICAL_LIMIT              0x110000u

/* Page register port -> channel. The mapping is not sequential; it is what IBM
 * wired, and getting it wrong silently corrupts the high address bits.
 */
static INT DmaPageChannel(WORD port)
{
    switch (port)
    {
    case DMA_PAGE_CHANNEL0:
        return DMA_CHANNEL0;

    case DMA_PAGE_CHANNEL1:
        return DMA_CHANNEL1;

    case DMA_PAGE_CHANNEL2:
        return DMA_CHANNEL2;

    case DMA_PAGE_CHANNEL3:
        return DMA_CHANNEL3;

    case DMA_PAGE_CHANNEL5:
        return DMA_CHANNEL5;

    case DMA_PAGE_CHANNEL6:
        return DMA_CHANNEL6;

    case DMA_PAGE_CHANNEL7:
        return DMA_CHANNEL7;

    default:
        return DMA_NO_CHANNEL;            /* 0x80 / 0x84-0x86 / 0x88 / 0x8C-0x8F: unused */
    }
}

/* address arithmetic: */
/* 8-bit channels address bytes directly; 16-bit channels address WORDS inside a
 * 128K page, so the address register is shifted and the page's low bit ignored.
 */
UINT32 VddDmaCurrentPhysical(PCDMA_STATE state, BYTE channelNumber)
{
    PCDMA_CHANNEL channel = &state->Channels[channelNumber & DMA_CHANNEL_MASK];

    if ((channelNumber & DMA_CHANNEL_MASK) < DMA_CHANNELS_PER_CONTROLLER)
        return ((UINT32)channel->Page << DMA_PAGE_SHIFT) | channel->CurrentAddress;

    return (((UINT32)channel->Page & DMA_WORD_PAGE_MASK) << DMA_PAGE_SHIFT) | ((UINT32)channel->CurrentAddress << 1);
}

UINT32 VddDmaRemaining(PCDMA_STATE state, BYTE channelNumber)
{
    PCDMA_CHANNEL channel = &state->Channels[channelNumber & DMA_CHANNEL_MASK];
    UINT32 units = (UINT32)channel->CurrentCount + 1;        /* 8237 counts n-1 */

    return ((channelNumber & DMA_CHANNEL_MASK) < DMA_CHANNELS_PER_CONTROLLER) ? units : units * DMA_WORD_UNIT;
}

/* ONE TRANSFER RETIRED: walk the address, count down, and at terminal count do
 * what the 8237 does -- latch TC, clear the channel's software request ("cleared
 * upon generation of a TC", datasheet), and either reload (auto-initialise) or set
 * the channel's own mask bit. Returns 1 if this transfer was the terminal one.
 */
static INT DmaAdvance(PDMA_STATE state, BYTE channelNumber, INT *isTerminalCount)
{
    PDMA_CHANNEL channel = &state->Channels[channelNumber & DMA_CHANNEL_MASK];

    if (channel->Mode & DMA_MODE_DECREMENT)
        channel->CurrentAddress--;
    else
        channel->CurrentAddress++;

    if (channel->CurrentCount == 0)                      /* terminal count reached */
    {
        channel->IsTerminalCount = 1;
        state->Request[(channelNumber & DMA_CHANNEL_MASK) >> DMA_CONTROLLER_SHIFT] &= (BYTE)~(1u << (channelNumber & DMA_LINE_MASK));

        if (isTerminalCount)
            *isTerminalCount = 1;

        if (channel->Mode & DMA_MODE_AUTOINIT)        /* ring buffer: reload */
        {
            channel->CurrentAddress  = channel->BaseAddress;
            channel->CurrentCount = channel->BaseCount;
        }
        else
        {
            /* [INFO]: "TC when the word count goes from 0000h to FFFFh" (datasheet): the
             * count really does wrap, and a driver that waits for a single-cycle
             * block by polling the count for FFFFh is waiting for exactly this.
             * Before #246 it rested at 0000h -- p_dma2's TC rows showed it.
             */
            channel->CurrentCount = DMA_COUNT_EXPIRED;
            channel->IsMasked = 1;                      /* single-cycle: the 8237 masks it */
        }

        return 1;
    }

    channel->CurrentCount--;
    return 0;
}

/* One unit of transfer (1 byte on ch0-3, 1 word on ch4-7). Returns 0 when the
 * channel has hit terminal count and stopped, so the caller ends the block.
 */
static INT DmaStep(
    PDMA_STATE state,
    BYTE channelNumber,
    BYTE *destination,
    const BYTE *source,
    UINT32 *transferred,
    INT *isTerminalCount)
{
    PDMA_CHANNEL channel = &state->Channels[channelNumber & DMA_CHANNEL_MASK];
    UINT32 unitBytes = ((channelNumber & DMA_CHANNEL_MASK) < DMA_CHANNELS_PER_CONTROLLER) ? 1u : DMA_WORD_UNIT_BYTES;
    BYTE *memory = (BYTE *)VddMapLinear(state->Bus, VddDmaCurrentPhysical(state, channelNumber));
    UINT32 byteIndex;

    if (destination)
        for (byteIndex = 0; byteIndex < unitBytes; ++byteIndex)
            destination[byteIndex] = memory[byteIndex];
    else
        for (byteIndex = 0; byteIndex < unitBytes; ++byteIndex)
            memory[byteIndex] = source[byteIndex];

    *transferred += unitBytes;

    /* a ring keeps streaming through its TC; a single-cycle block stops there */
    if (DmaAdvance(state, channelNumber, isTerminalCount) && !(channel->Mode & DMA_MODE_AUTOINIT))
        return 0;

    return 1;
}

/* THE GRANT. Mask bit AND controller-disable, asked in one place (see the header).
 * Before #176 this was `if (c->masked) return 0;` inline, and the command register's
 * bit 2 was stored and read by nothing -- a guest that disabled the controller to
 * stop a transfer got the transfer anyway.
 */
/* The grant as ONE controller sees it: its own mask bit, its own disable bit. This
 * is what decides whether controller 1 raises HRQ (= DREQ4) at all.
 */
static INT DmaControllerGrants(PCDMA_STATE state, BYTE channelNumber)
{
    channelNumber &= DMA_CHANNEL_MASK;

    if (state->Channels[channelNumber].IsMasked)
        return 0;

    if (state->Command[channelNumber >> DMA_CONTROLLER_SHIFT] & DMA_COMMAND_DISABLE)
        return 0;                                                                                /* ch0-3 -> cmd[0], 4-7 -> cmd[1] */

    return 1;
}

/* IS CONTROLLER 1 CONNECTED TO THE BUS? (the AT cascade, #246):
 * Its HRQ is controller 2's DREQ4 and its HLDA is controller 2's DACK4, so it is
 * served only while channel 4 is unmasked and controller 2 is enabled. Channel 4's
 * MODE is not asked: a guest that reprograms it out of cascade mode has broken the
 * board too, but whether a real part then starves or runs channel 4's own cycles is
 * a bus question we have no answer for, and no guest is known to do it.
 */
static INT DmaIsCascadeUp(PCDMA_STATE state)
{
    return !state->Channels[DMA_CASCADE_CHANNEL].IsMasked && !(state->Command[1] & DMA_COMMAND_DISABLE);
}

INT VddDmaGrants(PCDMA_STATE state, BYTE channelNumber)
{
    channelNumber &= DMA_CHANNEL_MASK;

    if (!DmaControllerGrants(state, channelNumber))
        return 0;

    if (channelNumber < DMA_CHANNELS_PER_CONTROLLER && !DmaIsCascadeUp(state))
        return 0;

    return 1;
}

INT VddDmaAddDreq(PDMA_STATE state, PDMA_DREQ_ROUTINE dreqRoutine, PCVOID dreqContext)
{
    UINT entryIndex;

    for (entryIndex = 0; entryIndex < state->DreqCount; ++entryIndex)
        if (state->DreqRoutines[entryIndex] == dreqRoutine && state->DreqContexts[entryIndex] == dreqContext)
            return 0;

    if (state->DreqCount >= DMA_DREQ_MAX)
        return DMA_FAILED;

    state->DreqRoutines[state->DreqCount] = dreqRoutine;
    state->DreqContexts[state->DreqCount] = dreqContext;
    ++state->DreqCount;
    return DMA_OK;
}

/* THE DREQ PINS, DERIVED:
 * Every device answers for itself (see PDMA_DREQ_ROUTINE in the header). Channel 4 is not
 * a device's: on an AT it is the CASCADE, wired to controller 1's HRQ, and the
 * 8237A raises HRQ when it has a request it is prepared to serve -- an unmasked
 * channel on an enabled controller. So bit 4 is derived from bits 0-3 through the
 * same grant every transfer uses. [CAUTION] Whatever a device claims for channel 4 is
 * dropped: nothing on an AT can drive that line except controller 1.
 */
BYTE VddDmaDreq(PCDMA_STATE state)
{
    BYTE requests = 0;
    BYTE line;
    UINT entryIndex;

    for (entryIndex = 0; entryIndex < state->DreqCount; ++entryIndex)
        requests |= state->DreqRoutines[entryIndex](state->DreqContexts[entryIndex]);

    requests &= (BYTE)~DMA_CASCADE_BIT;
    /* HRQ from controller 1: a hardware request it would serve (its own mask and
     * disable bits -- NOT the cascade's, which is the far end of this very wire), or a
     * software request, which the datasheet calls non-maskable.
     */
    for (line = 0; line < DMA_CHANNELS_PER_CONTROLLER; ++line)
        if (((requests & (1u << line)) && DmaControllerGrants(state, line))
         || ((state->Request[0] & (1u << line)) && !(state->Command[0] & DMA_COMMAND_DISABLE)))
        {
            requests |= DMA_CASCADE_BIT;
            break;
        }

    /* the request register shows in status 7:4 like any DREQ (#246); a software
     * request "on" channel 4 is not a line anything drives, so it is dropped too
     */
    requests |= (BYTE)(state->Request[0] | ((state->Request[1] & DMA_CONTROLLER2_LINES) << DMA_CONTROLLER2_SHIFT));
    return requests;
}

static UINT32 DmaTransfer(
    PDMA_STATE state,
    BYTE channelNumber,
    BYTE *destination,
    const BYTE *source,
    UINT32 byteCount,
    INT *isTerminalCount)
{
    UINT32 unitBytes = ((channelNumber & DMA_CHANNEL_MASK) < DMA_CHANNELS_PER_CONTROLLER) ? 1u : DMA_WORD_UNIT_BYTES;
    UINT32 transferred = 0;

    if (isTerminalCount)
        *isTerminalCount = 0;

    if (!VddDmaGrants(state, channelNumber))
        return 0;                                           /* no DACK: nothing moves, no TC */

    while (transferred + unitBytes <= byteCount)
    {
        if (!DmaStep(state, channelNumber, destination ? destination + transferred : 0, source ? source + transferred : 0, &transferred, isTerminalCount))
            break;                              /* stopped at terminal count */
    }

    return transferred;
}

UINT32 VddDmaRead(
    PDMA_STATE state,
    BYTE channelNumber,
    BYTE *destination,
    UINT32 byteCount,
    INT *isTerminalCount)
{
    return DmaTransfer(state, channelNumber, destination, 0, byteCount, isTerminalCount);
}

UINT32 VddDmaWrite(
    PDMA_STATE state,
    BYTE channelNumber,
    const BYTE *source,
    UINT32 byteCount,
    INT *isTerminalCount)
{
    return DmaTransfer(state, channelNumber, 0, source, byteCount, isTerminalCount);
}

static BYTE *DmaMemory(PDMA_STATE state, UINT32 physical)
{
    if (physical >= DMA_PHYSICAL_LIMIT)
        return 0;

    return (BYTE *)VddMapLinear(state->Bus, physical);
}

static VOID DmaSoftwareBlock(PDMA_STATE state, BYTE channelNumber)
{
    PDMA_CHANNEL channel = &state->Channels[channelNumber];
    UINT32 unitBytes = (channelNumber < DMA_CHANNELS_PER_CONTROLLER) ? 1u : DMA_WORD_UNIT_BYTES;
    UINT32 byteIndex;
    UINT32 iterations;

    state->SoftwareRuns++;

    for (iterations = 0; iterations < DMA_MAX_TRANSFERS; ++iterations)
    {
        if ((channel->Mode & DMA_MODE_TRANSFER) == DMA_MODE_TRANSFER_WRITE)
        {
            BYTE *memory = DmaMemory(state, VddDmaCurrentPhysical(state, channelNumber));

            if (memory)
                for (byteIndex = 0; byteIndex < unitBytes; ++byteIndex)
                    memory[byteIndex] = DMA_FLOATING_BYTE;
        }

        if (DmaAdvance(state, channelNumber, FALSE))
            break;
    }
}

/* MEMORY-TO-MEMORY, controller 1 only (command bit 0). 8237A datasheet: started by
 * a software request on channel 0; each byte is read at channel 0's current address
 * into the TEMPORARY register, then written at channel 1's; both addresses step
 * (channel 0's held still when command bit 1 is set -- "a single word written to a
 * block of memory"); CHANNEL 1's word count is the one that runs, and its TC ends
 * the service. Channel 0's count is not consulted.
 *
 * [CAUTION]: At that EOP each of the two channels auto-initialises or masks itself by its own
 * mode bit, and only channel 1 latches TC. That is a reading of the datasheet, not
 * a measurement (see docs/inventory/dma.md for what the oracles said).
 */
static VOID DmaMemoryToMemory(PDMA_STATE state)
{
    PDMA_CHANNEL channel0 = &state->Channels[0];
    PDMA_CHANNEL channel1 = &state->Channels[1];
    UINT32 iterations;

    state->MemoryToMemoryRuns++;

    for (iterations = 0; iterations < DMA_MAX_TRANSFERS; ++iterations)
    {
        BYTE *source = DmaMemory(state, VddDmaCurrentPhysical(state, 0));
        BYTE *destination = DmaMemory(state, VddDmaCurrentPhysical(state, 1));
        state->Temporary[0] = source ? *source : DMA_FLOATING_BYTE;

        if (destination)
            *destination = state->Temporary[0];

        if (!(state->Command[0] & DMA_COMMAND_ADDRESS_HOLD))
        {
            if (channel0->Mode & DMA_MODE_DECREMENT)
                channel0->CurrentAddress--;
            else
                channel0->CurrentAddress++;
        }

        if (channel1->Mode & DMA_MODE_DECREMENT)
            channel1->CurrentAddress--;
        else
            channel1->CurrentAddress++;

        if (channel1->CurrentCount == 0)
            break;

        channel1->CurrentCount--;
    }

    channel1->IsTerminalCount = 1;
    state->Request[0] &= (BYTE)~DMA_MEMORY_TO_MEMORY_LINES;

    if (channel0->Mode & DMA_MODE_AUTOINIT)
    {
        channel0->CurrentAddress = channel0->BaseAddress;
        channel0->CurrentCount = channel0->BaseCount;
    }
    else
        channel0->IsMasked = 1;

    if (channel1->Mode & DMA_MODE_AUTOINIT)
    {
        channel1->CurrentAddress = channel1->BaseAddress;
        channel1->CurrentCount = channel1->BaseCount;
    }
    else /* the count wraps, as above */
    {
        channel1->CurrentCount = DMA_COUNT_EXPIRED;
        channel1->IsMasked = 1;
    }
}

/* Serve every pending software request the controllers are in a position to serve.
 * Called after every register write, because a request written while its controller
 * is disabled -- or while the cascade is down -- is served the moment that changes.
 */
static VOID DmaSoftwareService(PDMA_STATE state)
{
    INT controller;
    INT line;

    for (controller = 0; controller < DMA_CONTROLLERS; ++controller)
    {
        if (!state->Request[controller])
            continue;

        if (state->Command[controller] & DMA_COMMAND_DISABLE)
            continue;

        if (controller == 0 && !DmaIsCascadeUp(state))
            continue;

        for (line = 0; line < DMA_CHANNELS_PER_CONTROLLER; ++line)                 /* fixed priority: 0 highest */
        {
            BYTE channelNumber = (BYTE)(controller * DMA_CHANNELS_PER_CONTROLLER + line);

            if (!(state->Request[controller] & (1u << line)))
                continue;

            if (channelNumber == DMA_CASCADE_CHANNEL)
                continue;                                                     /* the cascade itself */

            if ((state->Channels[channelNumber].Mode & DMA_MODE_SELECT) == DMA_MODE_SELECT)
                continue;

            if (controller == 0 && (state->Command[0] & DMA_COMMAND_MEMORY_TO_MEMORY))
            {
                /* in memory-to-memory mode channels 0 and 1 belong to the copy:
                 * channel 0's request starts it, channel 1's alone starts nothing
                 */
                if (line == 0)
                    DmaMemoryToMemory(state);

                if (line <= 1)
                    continue;
            }

            DmaSoftwareBlock(state, channelNumber);
        }
    }
}

/* register file: */
/* Address and count are 16-bit registers behind an 8-bit port, so the controller
 * keeps a flip-flop selecting which half the next access hits. Software clears it
 * (port 0x0C / 0xD8) before programming a channel; forgetting to model it swaps
 * the halves and sends DMA to a wild address.
 */
static VOID DmaWriteHalf(WORD *registerValue, BYTE *flipFlop, BYTE value)
{
    if (*flipFlop)
        *registerValue = (WORD)((*registerValue & DMA_KEEP_LOW_BYTE) | ((WORD)value << BYTE_SHIFT));
    else
        *registerValue = (WORD)((*registerValue & DMA_KEEP_HIGH_BYTE) | value);

    *flipFlop ^= 1;
}

static BYTE DmaReadHalf(WORD registerValue, BYTE *flipFlop)
{
    BYTE value = *flipFlop ? (BYTE)(registerValue >> BYTE_SHIFT) : (BYTE)registerValue;

    *flipFlop ^= 1;
    return value;
}

static VOID DmaMasterClear(PDMA_STATE state, INT controller)
{
    INT firstChannel = controller ? DMA_CHANNELS_PER_CONTROLLER : 0;
    INT channelNumber;

    state->FlipFlop[controller]  = 0;
    state->Command[controller] = 0;
    state->Request[controller] = 0;                  /* "the entire register is cleared by a Reset" */
    state->Temporary[controller] = 0;

    for (channelNumber = firstChannel; channelNumber < firstChannel + DMA_CHANNELS_PER_CONTROLLER; ++channelNumber)
    {
        state->Channels[channelNumber].IsMasked = 1;
        state->Channels[channelNumber].IsTerminalCount = 0;
    }
}

static VOID DmaPortOut(PVOID context, WORD port, BYTE width, UINT32 value)
{
    PDMA_STATE state = (PDMA_STATE)context;
    BYTE byteValue = (BYTE)value;
    INT controller;
    INT registerIndex;
    INT channelNumber;

    (VOID)width;

    if (port >= DMA_PAGE_FIRST_PORT && port <= DMA_PAGE_LAST_PORT)                    /* page registers */
    {
        INT pageChannel = DmaPageChannel(port);

        if (pageChannel >= 0)
            state->Channels[pageChannel].Page = byteValue;
        else
            state->SparePage[port & DMA_PAGE_INDEX_MASK] = byteValue;           /* a latch all the same */

        return;
    }

    controller = (port >= DMA_CONTROLLER2_FIRST_PORT) ? 1 : 0;
    registerIndex  = controller ? ((port - DMA_CONTROLLER2_FIRST_PORT) >> 1) : port;           /* controller 2: 2x spacing */

    if (registerIndex < DMA_ADDRESS_COUNT_REGISTERS)                                         /* per-channel addr/count */
    {
        channelNumber = (controller ? DMA_CHANNELS_PER_CONTROLLER : 0) + (registerIndex >> 1);

        if (registerIndex & 1)                                     /* count */
        {
            DmaWriteHalf(&state->Channels[channelNumber].BaseCount, &state->FlipFlop[controller], byteValue);
            state->Channels[channelNumber].CurrentCount = state->Channels[channelNumber].BaseCount;
        }
        else                                           /* address */
        {
            DmaWriteHalf(&state->Channels[channelNumber].BaseAddress, &state->FlipFlop[controller], byteValue);
            state->Channels[channelNumber].CurrentAddress = state->Channels[channelNumber].BaseAddress;
        }

        return;
    }

    switch (registerIndex)
    {
    case DMA_REGISTER_COMMAND:
        state->Command[controller] = byteValue;
        break;                /* command: bit 2 is read by
                                                            VddDmaGrants; the rest
                                                            are stored (see the header) */

    case DMA_REGISTER_REQUEST:                                            /* request register (#246) */
        if (byteValue & DMA_SET_BIT)
            state->Request[controller] |= (BYTE)(1u << (byteValue & DMA_LINE_MASK));
        else
            state->Request[controller] &= (BYTE)~(1u << (byteValue & DMA_LINE_MASK));

        break;

    case DMA_REGISTER_SINGLE_MASK:                                            /* single mask bit */
        channelNumber = (controller ? DMA_CHANNELS_PER_CONTROLLER : 0) + (byteValue & DMA_LINE_MASK);
        state->Channels[channelNumber].IsMasked = (byteValue & DMA_SET_BIT) ? 1 : 0;
        break;

    case DMA_REGISTER_MODE:                                            /* mode */
        channelNumber = (controller ? DMA_CHANNELS_PER_CONTROLLER : 0) + (byteValue & DMA_MODE_CHANNEL);
        state->Channels[channelNumber].Mode = byteValue;
        break;

    case DMA_REGISTER_CLEAR_FLIP_FLOP:
        state->FlipFlop[controller] = 0;
        break;                   /* clear byte pointer */

    case DMA_REGISTER_MASTER_CLEAR:
        DmaMasterClear(state, controller);
        break;         /* master clear */

    case DMA_REGISTER_CLEAR_MASK:                                            /* clear mask register */
        for (channelNumber = controller ? DMA_CHANNELS_PER_CONTROLLER : 0; channelNumber < (controller ? DMA_CHANNELS : DMA_CHANNELS_PER_CONTROLLER); ++channelNumber)
            state->Channels[channelNumber].IsMasked = 0;

        break;

    case DMA_REGISTER_WRITE_ALL_MASK:                                            /* write all mask bits */
        for (channelNumber = 0; channelNumber < DMA_CHANNELS_PER_CONTROLLER; ++channelNumber)
            state->Channels[(controller ? DMA_CHANNELS_PER_CONTROLLER : 0) + channelNumber].IsMasked = (byteValue >> channelNumber) & 1;

        break;

    default:
        break;
    }

    /* any write can make a pending software request servable (the request itself, a
     * re-enable, the cascade coming up) -- and there is nothing else to wake it
     */
    if (state->Request[0] | state->Request[1])
        DmaSoftwareService(state);
}

static VOID DmaPortIn(PVOID context, WORD port, BYTE width, UINT32 *value)
{
    PDMA_STATE state = (PDMA_STATE)context;
    INT controller;
    INT registerIndex;
    INT channelNumber;
    INT line;
    /* `w` is OBSERVED but still not acted on: the 8237 is an 8-bit device and every
     * read below returns one half through the flip-flop, which is faithful for the
     * `in al,dx` the BIOS and every driver we have seen use. It is recorded because
     * the poll RATE cannot be derived from the read count without knowing it.
     */

    if (port >= DMA_PAGE_FIRST_PORT && port <= DMA_PAGE_LAST_PORT)
    {
        INT pageChannel = DmaPageChannel(port);
        /* [CAUTION]: "Unused" was true of the CHANNEL MAPPING and false of the hardware:
         * these ports are latches whether or not a channel reads them, and
         * 0xFF was us describing an empty bus. See page_spare in the header.
         */
        *value = (pageChannel >= 0) ? state->Channels[pageChannel].Page : state->SparePage[port & DMA_PAGE_INDEX_MASK];
        return;
    }

    controller = (port >= DMA_CONTROLLER2_FIRST_PORT) ? 1 : 0;
    registerIndex  = controller ? ((port - DMA_CONTROLLER2_FIRST_PORT) >> 1) : port;

    if (registerIndex < DMA_ADDRESS_COUNT_REGISTERS)
    {
        channelNumber = (controller ? DMA_CHANNELS_PER_CONTROLLER : 0) + (registerIndex >> 1);

        if (registerIndex & 1)
        {
            ++state->ChannelCountReads[channelNumber];
            ++state->CountReads;

            if      (width == 1)
                ++state->CountReadsByte;
            else if (width == DMA_WORD_ACCESS)
                ++state->CountReadsWord;
            else
                ++state->CountReadsDword;
        }
        else
            ++state->AddressReads[channelNumber];

        *value = (registerIndex & 1) ? DmaReadHalf(state->Channels[channelNumber].CurrentCount, &state->FlipFlop[controller])
                         : DmaReadHalf(state->Channels[channelNumber].CurrentAddress,  &state->FlipFlop[controller]);
        return;
    }

    if (registerIndex == DMA_REGISTER_COMMAND)                              /* status: TC bits 0-3, DRQ 4-7 */
    {
        BYTE status = 0;
        ++state->StatusReads[controller];

        for (line = 0; line < DMA_CHANNELS_PER_CONTROLLER; ++line)
        {
            channelNumber = (controller ? DMA_CHANNELS_PER_CONTROLLER : 0) + line;

            if (state->Channels[channelNumber].IsTerminalCount)
                status |= (BYTE)(1 << line);

            state->Channels[channelNumber].IsTerminalCount = 0;                 /* reading status clears TC */
        }

        /* BITS 7:4 -- "set whenever their corresponding channel is requesting
         * service" (8237A datasheet). They always read 0 before #176. Derived, not
         * latched, so the read that clears TC cannot touch them: a request is still
         * pending after you look at it, until the device stops making it.
         */
        status |= (BYTE)(((VddDmaDreq(state) >> (controller ? DMA_CHANNELS_PER_CONTROLLER : 0)) & DMA_CONTROLLER_LINES) << DMA_STATUS_REQUEST_SHIFT);
        *value = status;
        return;
    }

    if (registerIndex == DMA_REGISTER_MASTER_CLEAR) /* temporary register (#246) */
    {
        *value = state->Temporary[controller];
        return;
    }

    *value = DMA_FLOATING_BYTE;
}

/* lifecycle: */
/* WHAT POST LEAVES ON AN AT: CHANNEL 4 IN CASCADE MODE, UNMASKED. (#246):
 * The BIOS does this once at power-on (mode C0h to D6h, then unmask channel 4 at
 * D4h), and every DOS program inherits it -- it is what connects controller 1 to
 * the bus at all. Without it the cascade could not be honoured: the power-on master
 * clear leaves channel 4 masked, and every 8-bit channel would be starved.
 */
static VOID DmaPost(PDMA_STATE state)
{
    state->Channels[DMA_CASCADE_CHANNEL].Mode   = DMA_MODE_SELECT;         /* C0h: cascade, channel 0 of ctrl 2 */
    state->Channels[DMA_CASCADE_CHANNEL].IsMasked = 0;
}

VOID VddDmaReset(PVOID context)
{
    PDMA_STATE state = (PDMA_STATE)context;
    PVDD_BUS bus = state->Bus;
    PDMA_DREQ_ROUTINE dreqRoutines[DMA_DREQ_MAX];
    PCVOID dreqContexts[DMA_DREQ_MAX];
    BYTE dreqCount = state->DreqCount;
    UINT index;
    BYTE *stateBytes = (BYTE *)state;

    for (index = 0; index < DMA_DREQ_MAX; ++index)
    {
        dreqRoutines[index] = state->DreqRoutines[index];
        dreqContexts[index] = state->DreqContexts[index];
    }

    for (index = 0; index < sizeof(*state); ++index)
        stateBytes[index] = 0;

    state->Bus = bus;
    /* the DREQ wiring is the machine's, not the chip's: a reset keeps it */
    for (index = 0; index < DMA_DREQ_MAX; ++index)
    {
        state->DreqRoutines[index] = dreqRoutines[index];
        state->DreqContexts[index] = dreqContexts[index];
    }

    state->DreqCount = dreqCount;
    DmaMasterClear(state, DMA_CONTROLLER_8BIT);
    DmaMasterClear(state, DMA_CONTROLLER_16BIT);
    DmaPost(state);
}

INT VddDmaInitialize(PVDD_BUS bus, PVOID context)
{
    PDMA_STATE state = (PDMA_STATE)context;

    state->Bus = bus;
    DmaMasterClear(state, DMA_CONTROLLER_8BIT);
    DmaMasterClear(state, DMA_CONTROLLER_16BIT);
    DmaPost(state);

    if (VddClaimPorts(bus, DMA_CONTROLLER1_FIRST_PORT, DMA_CONTROLLER1_LAST_PORT, DmaPortIn, DmaPortOut, state))
        return DMA_FAILED;                                                                                                           /* controller 1 */

    if (VddClaimPorts(bus, DMA_PAGE_FIRST_PORT, DMA_PAGE_LAST_PORT, DmaPortIn, DmaPortOut, state))
        return DMA_FAILED;                                                                                             /* page regs */

    if (VddClaimPorts(bus, DMA_CONTROLLER2_FIRST_PORT, DMA_CONTROLLER2_LAST_PORT, DmaPortIn, DmaPortOut, state))
        return DMA_FAILED;                                                                                                           /* controller 2 */

    return DMA_OK;
}
