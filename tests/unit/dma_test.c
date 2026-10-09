/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Off-VM unit battery for the ISA DMA controller VDD (vdd_dma.c).
 *
 * The first device of the sound epic: Sound Blaster playback is DMA, so the SB
 * VDD is only as correct as this one. The cases below target the things that are
 * easy to get subtly wrong and impossible to notice later -- the byte-pointer
 * flip-flop, the non-sequential page-register wiring, word addressing on the
 * 16-bit controller, terminal count, and auto-init ring wrap (how every DOS game
 * streams continuous audio).
 *
 * Entirely off-VM: the bus is given a plain 1MB array as guest memory.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include <stdio.h>
#include <string.h>
#include "vdd_dma.h"

static INT g_Total = 0, g_Failures = 0;
#define CHECK(condition,message) do{ g_Total++; if(condition){printf("  PASS  %s\n",(message));} \
    else{printf("  FAIL  %s\n",(message)); g_Failures++;} }while(0)

static BYTE g_GuestMemory[0x100000];

/* A stand-in for a sound card's DREQ lines (#176): the test sets the mask. */
static BYTE g_DreqMask;
static BYTE DmaTestFakeDreq(PCVOID context)
{
    return *(PCBYTE )context;
}

/* Program a channel the way a DOS sound driver does: clear the flip-flop, write
 * address lo/hi, count lo/hi, the page, then the mode, then unmask.
 */
static VOID DmaTestProgram(PVDD_BUS bus, INT channel, UINT32 physical, WORD count, BYTE mode)
{
    INT isSecondController = (channel >= 4);
    WORD addressPort = isSecondController ? (WORD)(0xC0 + ((channel - 4) * 4))     : (WORD)(channel * 2);
    WORD countPort  = isSecondController ? (WORD)(0xC0 + ((channel - 4) * 4) + 2) : (WORD)(channel * 2 + 1);
    WORD flipFlopPort   = isSecondController ? 0xD8 : 0x0C;
    WORD modePort = isSecondController ? 0xD6 : 0x0B;
    WORD maskPort = isSecondController ? 0xD4 : 0x0A;
    WORD pagePort;
    WORD address   = isSecondController ? (WORD)((physical >> 1) & 0xFFFF) : (WORD)(physical & 0xFFFF);
    UINT32 value;

    switch (channel)
    {
    case 0:
        pagePort = 0x87;
    break;

    case 1:
        pagePort = 0x83;
    break;

    case 2:
        pagePort = 0x81;
    break;

    case 3:
        pagePort = 0x82;
    break;

    case 5:
        pagePort = 0x8B;
    break;

    case 6:
        pagePort = 0x89;
    break;

    default:
        pagePort = 0x8A;
    break;
    }
    value = 0;
    VddBusIo(bus, flipFlopPort,   1, 0, &value);
    value = address & 0xFF;
    VddBusIo(bus, addressPort, 1, 0, &value);
    value = address >> 8;
    VddBusIo(bus, addressPort, 1, 0, &value);
    value = count & 0xFF;
    VddBusIo(bus, countPort,  1, 0, &value);
    value = count >> 8;
    VddBusIo(bus, countPort,  1, 0, &value);
    value = (physical >> 16) & 0xFF;
    VddBusIo(bus, pagePort, 1, 0, &value);
    value = mode | (UINT32)(channel & 3);
    VddBusIo(bus, modePort, 1, 0, &value);
    value = (UINT32)(channel & 3);
    VddBusIo(bus, maskPort, 1, 0, &value);   /* unmask */
}

INT main(VOID)
{
    VDD_BUS bus;
    DMA_STATE dma;

    memset(&dma, 0, sizeof dma);
    NTVDD_DEVICE dmaDevice = VddDmaDevice(&dma);
    BYTE buffer[512];
    UINT32 value, actual;
    INT isTerminalCount, index;

    printf("== sound epic: ISA DMA controller battery ==\n");

    memset(g_GuestMemory, 0, sizeof g_GuestMemory);
    VddBusInitialize(&bus, g_GuestMemory);
    CHECK(VddBusAdd(&bus, &dmaDevice) == 0, "add: dma device ok");

    /* T1: all channels start masked (master clear at init) ------------------ */
    CHECK(dma.Channels[1].IsMasked == 1, "init: channel 1 masked after master clear");
    actual = VddDmaRead(&dma, 1, buffer, 16, &isTerminalCount);
    CHECK(actual == 0, "masked channel transfers nothing");

    /* T2: flip-flop + page wiring ------------------------------------------ */
    value = 0;
    VddBusIo(&bus, 0x0C, 1, 0, &value);      /* clear byte pointer */
    value = 0x34;
    VddBusIo(&bus, 0x02, 1, 0, &value);      /* ch1 addr lo */
    value = 0x12;
    VddBusIo(&bus, 0x02, 1, 0, &value);      /* ch1 addr hi */
    CHECK(dma.Channels[1].BaseAddress == 0x1234, "flip-flop: lo then hi -> 0x1234");
    CHECK(dma.Channels[1].CurrentAddress == 0x1234, "current address loaded from base");
    value = 0x07;
    VddBusIo(&bus, 0x83, 1, 0, &value);      /* ch1 page (port 0x83!) */
    CHECK(dma.Channels[1].Page == 0x07, "page register 0x83 maps to channel 1");
    CHECK(VddDmaCurrentPhysical(&dma, 1) == 0x71234, "8-bit channel: phys = page<<16|addr");

    /* the flip-flop must alternate, not latch: a second lo/hi pair works too */
    value = 0x78;
    VddBusIo(&bus, 0x02, 1, 0, &value);
    value = 0x56;
    VddBusIo(&bus, 0x02, 1, 0, &value);
    CHECK(dma.Channels[1].BaseAddress == 0x5678, "flip-flop alternates across writes");

    /* T3: a plain single-cycle read transfer (memory -> device) ------------- */
    for (index = 0; index < 256; ++index)
        g_GuestMemory[0x71000 + index] = (BYTE)index;
    DmaTestProgram(&bus, 1, 0x71000, 99, DMA_MODE_TRANSFER_READ);   /* 100 bytes */
    CHECK(dma.Channels[1].IsMasked == 0, "unmask via single-mask register");
    CHECK(VddDmaRemaining(&dma, 1) == 100, "remaining = count+1 bytes");

    memset(buffer, 0, sizeof buffer);
    actual = VddDmaRead(&dma, 1, buffer, 40, &isTerminalCount);
    CHECK(actual == 40, "partial read: 40 bytes");
    CHECK(!isTerminalCount, "partial read: no terminal count yet");
    CHECK(buffer[0] == 0 && buffer[39] == 39, "partial read: correct bytes from guest memory");
    CHECK(VddDmaCurrentPhysical(&dma, 1) == 0x71000 + 40, "address advanced by 40");
    CHECK(VddDmaRemaining(&dma, 1) == 60, "remaining dropped to 60");

    /* T4: terminal count stops a non-auto-init channel and masks it --------- */
    actual = VddDmaRead(&dma, 1, buffer, 100, &isTerminalCount);
    CHECK(actual == 60, "read past the end stops at terminal count (60 of 100)");
    CHECK(isTerminalCount, "terminal count reported");
    CHECK(dma.Channels[1].IsMasked == 1, "8237 masks a non-auto-init channel at TC");
    CHECK(buffer[59] == 99, "last byte of the block is correct");
    /* #246: "TC when the word count goes from 0000h to FFFFh" -- the count wraps,
     * and a driver polling it for FFFFh to see a single-cycle block end needs it to.
     * It rested at 0000h before; p_dma2's TC rows made that visible.
     */
    { UINT32 low = 0, high = 0;
      value = 0;
      VddBusIo(&bus, 0x0C, 1, 0, &value);
      VddBusIo(&bus, 0x03, 1, 1, &low);
      VddBusIo(&bus, 0x03, 1, 1, &high);
      CHECK(low == 0xFF && high == 0xFF, "TC: the current count reads FFFFh through the port"); }
    actual = VddDmaRead(&dma, 1, buffer, 8, &isTerminalCount);
    CHECK(actual == 0, "channel is finished: no further transfer");

    /* T5: status register reports TC and clears it on read ------------------ */
    VddBusIo(&bus, 0x08, 1, 1, &value);
    CHECK((value & 0x02) != 0, "status: TC bit set for channel 1");
    VddBusIo(&bus, 0x08, 1, 1, &value);
    CHECK((value & 0x02) == 0, "status: reading it clears TC");

    /* T6: auto-init wraps and keeps streaming (the audio ring buffer) ------- */
    for (index = 0; index < 16; ++index)
        g_GuestMemory[0x72000 + index] = (BYTE)(0xA0 + index);
    DmaTestProgram(&bus, 1, 0x72000, 15, DMA_MODE_TRANSFER_READ | DMA_MODE_AUTOINIT);
    memset(buffer, 0, sizeof buffer);
    actual = VddDmaRead(&dma, 1, buffer, 40, &isTerminalCount);
    CHECK(actual == 40, "auto-init: transfer continues past the end");
    CHECK(isTerminalCount, "auto-init: terminal count still reported");
    CHECK(dma.Channels[1].IsMasked == 0, "auto-init: channel stays unmasked");
    CHECK(buffer[0] == 0xA0 && buffer[15] == 0xAF, "auto-init: first pass correct");
    CHECK(buffer[16] == 0xA0 && buffer[31] == 0xAF, "auto-init: wrapped to base, second pass");
    CHECK(buffer[32] == 0xA0, "auto-init: third pass continues the ring");
    CHECK(VddDmaCurrentPhysical(&dma, 1) == 0x72000 + 8, "auto-init: address mid-ring after 40");

    /* T7: decrement mode walks the address downwards ------------------------ */
    DmaTestProgram(&bus, 3, 0x73100, 3, DMA_MODE_TRANSFER_READ | DMA_MODE_DECREMENT);
    for (index = 0; index < 4; ++index)
        g_GuestMemory[0x73100 - index] = (BYTE)(0x10 + index);
    memset(buffer, 0, sizeof buffer);
    actual = VddDmaRead(&dma, 3, buffer, 4, &isTerminalCount);
    CHECK(actual == 4, "decrement: 4 bytes transferred");
    CHECK(buffer[0] == 0x10 && buffer[1] == 0x11 && buffer[3] == 0x13,
          "decrement: address walked downwards");

    /* T8: 16-bit controller -- word addressing, word counts ----------------- */
    for (index = 0; index < 32; ++index)
        g_GuestMemory[0x84000 + index] = (BYTE)(0x40 + index);
    DmaTestProgram(&bus, 5, 0x84000, 7, DMA_MODE_TRANSFER_READ);    /* 8 words = 16 bytes */
    CHECK(dma.Channels[5].BaseAddress == 0x2000, "16-bit channel: address register is a WORD address");
    CHECK(VddDmaCurrentPhysical(&dma, 5) == 0x84000, "16-bit channel: phys = (page&0xFE)<<16|addr<<1");
    CHECK(VddDmaRemaining(&dma, 5) == 16, "16-bit channel: remaining counts BYTES (8 words)");
    memset(buffer, 0, sizeof buffer);
    actual = VddDmaRead(&dma, 5, buffer, 16, &isTerminalCount);
    CHECK(actual == 16, "16-bit channel: 16 bytes transferred");
    CHECK(isTerminalCount, "16-bit channel: terminal count at 8 words");
    CHECK(buffer[0] == 0x40 && buffer[15] == 0x4F, "16-bit channel: correct bytes");

    /* T9: device -> memory (recording direction) ---------------------------- */
    DmaTestProgram(&bus, 1, 0x75000, 7, DMA_MODE_TRANSFER_WRITE);
    for (index = 0; index < 8; ++index)
        buffer[index] = (BYTE)(0xE0 + index);
    actual = VddDmaWrite(&dma, 1, buffer, 8, &isTerminalCount);
    CHECK(actual == 8, "write direction: 8 bytes accepted");
    CHECK(g_GuestMemory[0x75000] == 0xE0 && g_GuestMemory[0x75007] == 0xE7,
          "write direction: bytes landed in guest memory");

    /* T10: mask register variants ------------------------------------------ */
    value = 0x0F;
    VddBusIo(&bus, 0x0F, 1, 0, &value);          /* mask all 4 channels */
    CHECK(dma.Channels[0].IsMasked && dma.Channels[3].IsMasked, "write-all-mask masks every channel");
    value = 0x00;
    VddBusIo(&bus, 0x0E, 1, 0, &value);          /* clear mask register */
    CHECK(!dma.Channels[0].IsMasked && !dma.Channels[3].IsMasked, "clear-mask unmasks every channel");

    /* THE PAGE PORTS THAT MAP TO NO CHANNEL. docs/ref/dma.md 3:
     * Seven of the sixteen ports at 80h-8Fh carry a channel's high address bits;
     * the other nine are read/write latches on a PC anyway, because the address
     * decoder does not bother to leave them out.
     *
     * [INFO]: MEASURED: dosbox-x and PCem (real AMI BIOS) both read back a written
     * 0x5A at port 80h; only 6.22-under-QEMU answers 0xFF. We answered 0xFF --
     * an empty bus rather than a machine.
     */
    {
        UINT32 value;
        value = 0x5A;
        VddBusIo(&bus, 0x80, 1, 0, &value);
        value = 0;
        VddBusIo(&bus, 0x80, 1, 1, &value);
        CHECK(value == 0x5A, "page: port 80h is a latch, not an empty bus");

        value = 0xA5;
        VddBusIo(&bus, 0x8C, 1, 0, &value);
        value = 0;
        VddBusIo(&bus, 0x8C, 1, 1, &value);
        CHECK(value == 0xA5, "page: so is 8Ch");

        /* ...and the mapped ones are unaffected: the spare latches must not be
         * the same storage, or writing scratch would move a channel's page.
         */
        value = 0x33;
        VddBusIo(&bus, 0x83, 1, 0, &value);      /* channel 1's page */
        CHECK(dma.Channels[1].Page == 0x33, "page: 83h still reaches channel 1");
        value = 0;
        VddBusIo(&bus, 0x80, 1, 1, &value);
        CHECK(value == 0x5A, "page: ...and did not disturb the spare at 80h");
    }

    /* T11: STATUS BITS 7:4 -- "REQUEST PENDING". #176:
     * 8237A datasheet: bits 4-7 "are set whenever their corresponding channel is
     * requesting service". They read 0 always before #176. A stand-in device
     * drives the DREQ lines through the same registration a sound card uses.
     */
    {
        UINT32 status;
        CHECK(VddDmaAddDreq(&dma, DmaTestFakeDreq, &g_DreqMask) == 0, "dreq: a device registers its lines");
        CHECK(VddDmaAddDreq(&dma, DmaTestFakeDreq, &g_DreqMask) == 0 && dma.DreqCount == 1,
              "dreq: registering the same device twice is one registration");

        value = 0;
        VddBusIo(&bus, 0x0D, 1, 0, &value);         /* master clear ctrl 1 */
        value = 0;
        VddBusIo(&bus, 0xDA, 1, 0, &value);         /* master clear ctrl 2 */
        /* ...which masks channel 4, the cascade, and so disconnects controller 1
         * (#246). Put it back the way POST does: cascade mode, unmasked.
         */
        value = 0xC0;
        VddBusIo(&bus, 0xD6, 1, 0, &value);
        value = 0x00;
        VddBusIo(&bus, 0xD4, 1, 0, &value);
        g_DreqMask = 0;
        VddBusIo(&bus, 0x08, 1, 1, &status);
        CHECK((status & DMA_STATUS_REQUEST) == 0, "status 08h: no device requesting -> bits 7:4 clear");

        g_DreqMask = 0x0A;                                    /* DREQ 1 and 3 */
        VddBusIo(&bus, 0x08, 1, 1, &status);
        CHECK((status & DMA_STATUS_REQUEST) == 0xA0, "status 08h: DREQ1+DREQ3 -> bits 5 and 7");
        CHECK(dma.Channels[1].IsMasked && dma.Channels[3].IsMasked,
              "...and that is with both channels MASKED: a refused request is still pending");

        g_DreqMask = 0xA0;                                    /* DREQ 5 and 7 */
        VddBusIo(&bus, 0x08, 1, 1, &status);
        CHECK((status & DMA_STATUS_REQUEST) == 0, "status 08h: controller 2's requests are not controller 1's");
        VddBusIo(&bus, 0xD0, 1, 1, &status);
        CHECK((status & DMA_STATUS_REQUEST) == 0xA0, "status D0h: DREQ5+DREQ7 -> bits 5 and 7");

        /* channel 4 is the CASCADE: controller 1's HRQ, raised only for a request
         * controller 1 would serve. A device cannot drive it itself.
         */
        g_DreqMask = 0x10;
        VddBusIo(&bus, 0xD0, 1, 1, &status);
        CHECK((status & 0x10) == 0, "status D0h: a device's claim on channel 4 is dropped");
        g_DreqMask = 0x02;                                    /* DREQ1, ch1 masked */
        VddBusIo(&bus, 0xD0, 1, 1, &status);
        CHECK((status & 0x10) == 0, "status D0h bit 4: masked ch1 request -> no HRQ, no DREQ4");
        DmaTestProgram(&bus, 1, 0x76000, 15, DMA_MODE_TRANSFER_READ);
        VddBusIo(&bus, 0xD0, 1, 1, &status);
        CHECK((status & 0x10) != 0, "status D0h bit 4: unmasked ch1 request -> the cascade requests");

        /* TC clears on read; DRQ does not -- the request is still there afterwards */
        actual = VddDmaRead(&dma, 1, buffer, 16, &isTerminalCount);
        CHECK(actual == 16 && isTerminalCount, "status: a 16-byte block reaches TC on channel 1");
        VddBusIo(&bus, 0x08, 1, 1, &status);
        CHECK(status == (0x02 | 0x20), "status: TC1 and DRQ1 together on the first read");
        VddBusIo(&bus, 0x08, 1, 1, &status);
        CHECK(status == 0x20, "status: the read cleared TC1 and left DRQ1 standing");
        g_DreqMask = 0;
        VddBusIo(&bus, 0x08, 1, 1, &status);
        CHECK(status == 0x00, "status: DRQ1 falls when the device stops requesting");
    }

    /* T12: COMMAND BIT 2 -- CONTROLLER DISABLE. #176:
     * Stored and read by nothing before #176. With it set the 8237 gives no DACK
     * on any of the controller's channels: no byte moves, the address and count
     * stand still, no TC -- and re-enabling resumes exactly where it stopped.
     */
    {
        UINT32 status;
        for (index = 0; index < 64; ++index)
            g_GuestMemory[0x77000 + index] = (BYTE)(0x80 + index);
        DmaTestProgram(&bus, 1, 0x77000, 31, DMA_MODE_TRANSFER_READ);         /* 32 bytes */
        actual = VddDmaRead(&dma, 1, buffer, 8, &isTerminalCount);
        CHECK(actual == 8 && buffer[7] == 0x87, "disable: 8 bytes move while enabled");
        CHECK(VddDmaGrants(&dma, 1), "grants: unmasked + enabled -> served");

        value = DMA_COMMAND_DISABLE;
        VddBusIo(&bus, 0x08, 1, 0, &value);
        CHECK(!VddDmaGrants(&dma, 1), "grants: command bit 2 on 08h -> refused");
        CHECK(dma.Channels[1].IsMasked == 0, "disable: is not the mask -- the mask bit is untouched");
        memset(buffer, 0, sizeof buffer);
        actual = VddDmaRead(&dma, 1, buffer, 64, &isTerminalCount);
        CHECK(actual == 0 && !isTerminalCount, "disable: a read moves nothing and reaches no TC");
        CHECK(VddDmaCurrentPhysical(&dma, 1) == 0x77008 && VddDmaRemaining(&dma, 1) == 24,
              "disable: address and count stand still");
        actual = VddDmaWrite(&dma, 1, buffer, 4, &isTerminalCount);
        CHECK(actual == 0 && g_GuestMemory[0x77008] == 0x88, "disable: the write direction is refused too");

        g_DreqMask = 0x02;
        VddBusIo(&bus, 0x08, 1, 1, &status);
        CHECK(status == 0x20, "disable: the request shows as pending, with no TC");
        VddBusIo(&bus, 0xD0, 1, 1, &status);
        CHECK((status & 0x10) == 0, "disable: a disabled controller 1 raises no HRQ (D0h bit 4)");
        g_DreqMask = 0;

        /* the other controller is not affected */
        for (index = 0; index < 4; ++index)
            g_GuestMemory[0x86000 + index] = (BYTE)(0x60 + index);
        DmaTestProgram(&bus, 5, 0x86000, 1, DMA_MODE_TRANSFER_READ);          /* 2 words */
        actual = VddDmaRead(&dma, 5, buffer, 4, &isTerminalCount);
        CHECK(actual == 4 && buffer[0] == 0x60, "disable 08h: controller 2's channel 5 still moves");

        value = 0;
        VddBusIo(&bus, 0x08, 1, 0, &value);                   /* re-enable */
        actual = VddDmaRead(&dma, 1, buffer, 64, &isTerminalCount);
        CHECK(actual == 24 && isTerminalCount, "re-enable: the remaining 24 bytes move, then TC");
        CHECK(buffer[0] == 0x88 && buffer[23] == 0x9F, "re-enable: resumed at byte 8, not the base");

        /* controller 2's own bit, at D0h, stops channel 5 -- AND, on an AT, channel
         * 1 as well: controller 1 reaches the bus only through channel 4 (#246).
         * Before #246 this case asserted the opposite ("the cascade is not
         * modelled"); that was a statement about our model, not the board.
         */
        DmaTestProgram(&bus, 5, 0x86000, 1, DMA_MODE_TRANSFER_READ);
        DmaTestProgram(&bus, 1, 0x77000, 3, DMA_MODE_TRANSFER_READ);
        value = 0;
        VddBusIo(&bus, 0x08, 1, 1, &value);                   /* drop TC1 */
        value = DMA_COMMAND_DISABLE;
        VddBusIo(&bus, 0xD0, 1, 0, &value);
        CHECK(!VddDmaGrants(&dma, 5) && !VddDmaGrants(&dma, 1) && !VddDmaGrants(&dma, 3),
              "disable D0h: refuses channels 4-7 AND, through the cascade, 0-3");
        actual = VddDmaRead(&dma, 5, buffer, 4, &isTerminalCount);
        CHECK(actual == 0 && !isTerminalCount, "disable D0h: channel 5 moves nothing");
        actual = VddDmaRead(&dma, 1, buffer, 4, &isTerminalCount);
        CHECK(actual == 0 && !isTerminalCount, "disable D0h: channel 1 moves nothing either (the AT cascade)");
        g_DreqMask = 0x02;
        VddBusIo(&bus, 0xD0, 1, 1, &value);
        CHECK((value & 0x10) != 0,
              "disable D0h: controller 1 still RAISES HRQ (DREQ4) -- it is the far end that refuses");
        g_DreqMask = 0;
        value = 0;
        VddBusIo(&bus, 0xD0, 1, 0, &value);                   /* re-enable */
        CHECK(VddDmaGrants(&dma, 1), "enable D0h: channel 1 is served again");
        /* masking channel 4 alone does the same thing */
        value = 0x04;
        VddBusIo(&bus, 0xD4, 1, 0, &value);                /* mask ch4 */
        CHECK(!VddDmaGrants(&dma, 1) && VddDmaGrants(&dma, 5),
              "cascade: masking channel 4 starves channel 1, not channel 5");
        value = 0x00;
        VddBusIo(&bus, 0xD4, 1, 0, &value);                /* unmask ch4 */
        CHECK(VddDmaGrants(&dma, 1), "cascade: unmasking channel 4 reconnects controller 1");
        actual = VddDmaRead(&dma, 1, buffer, 4, &isTerminalCount);
        CHECK(actual == 4 && isTerminalCount, "cascade: and the stalled 4-byte block then completes");

        /* master clear clears the command register -- the controller is enabled
         * again, and every channel masked (datasheet)
         */
        value = 0;
        VddBusIo(&bus, 0xDA, 1, 0, &value);
        CHECK(dma.Command[1] == 0 && dma.Channels[5].IsMasked, "master clear D0h: enabled again, channel masked");
        value = 0x01;
        VddBusIo(&bus, 0xD4, 1, 0, &value);                /* unmask ch5 */
        CHECK(VddDmaGrants(&dma, 5), "master clear D0h: unmasking alone serves channel 5 again");

        /* the DREQ wiring is the machine's: a device reset keeps it */
        VddDmaReset(&dma);
        CHECK(dma.DreqCount == 1 && dma.DreqRoutines[0] == DmaTestFakeDreq, "reset: the DREQ registration survives");
        /* ...and a reset leaves what POST leaves: channel 4 cascade, unmasked */
        CHECK((dma.Channels[4].Mode & DMA_MODE_SELECT) == DMA_MODE_SELECT && !dma.Channels[4].IsMasked,
              "reset: channel 4 in cascade mode and unmasked, as POST leaves it");
    }

    /* T13: THE REQUEST REGISTER (09h). #246:
     * 8237A datasheet: a request bit per channel, set/reset by bits 2 and 1:0,
     * NON-MASKABLE, cleared at TC and by master clear. With no device on DACK the
     * controller carries the cycles out itself. Channel 1 in VERIFY mode: the walk
     * happens, memory is not touched, TC latches.
     */
    {
        UINT32 status;
        VddDmaReset(&dma);
        for (index = 0; index < 32; ++index)
            g_GuestMemory[0x78000 + index] = (BYTE)(0x40 + index);
        DmaTestProgram(&bus, 1, 0x78000, 15, 0x80 /* block */ | DMA_MODE_TRANSFER_VERIFY);
        value = 0x05;
        VddBusIo(&bus, 0x0A, 1, 0, &value);                /* MASK ch1 */
        value = 0x05;
        VddBusIo(&bus, 0x09, 1, 0, &value);                /* request 1 */
        CHECK(dma.Channels[1].CurrentCount == 0xFFFF && dma.Channels[1].CurrentAddress == 0x8010,
              "request: a masked channel is served anyway -- 16 verify cycles, count FFFFh");
        CHECK(g_GuestMemory[0x78000] == 0x40 && g_GuestMemory[0x7800F] == 0x4F, "request: verify touched no memory");
        CHECK(dma.Request[0] == 0, "request: the bit cleared itself at TC");
        VddBusIo(&bus, 0x08, 1, 1, &status);
        CHECK(status == 0x02, "request: status shows TC1 and no request left pending");

        /* write type: memory gets the undriven bus -- FFh */
        DmaTestProgram(&bus, 1, 0x78000, 3, 0x80 | DMA_MODE_TRANSFER_WRITE);
        value = 0x05;
        VddBusIo(&bus, 0x09, 1, 0, &value);
        CHECK(g_GuestMemory[0x78000] == 0xFF && g_GuestMemory[0x78003] == 0xFF && g_GuestMemory[0x78004] == 0x44,
              "request, write type: exactly 4 bytes of float (FFh), not 5");
        CHECK(dma.Channels[1].IsMasked, "request: a non-auto-init channel masks itself at TC");

        /* held off by a disabled controller: pending, visible, then served */
        DmaTestProgram(&bus, 1, 0x78000, 7, 0x80 | DMA_MODE_TRANSFER_VERIFY);
        value = 0;
        VddBusIo(&bus, 0x08, 1, 1, &value);                   /* drop TCs */
        value = DMA_COMMAND_DISABLE;
        VddBusIo(&bus, 0x08, 1, 0, &value);
        value = 0x05;
        VddBusIo(&bus, 0x09, 1, 0, &value);
        CHECK(dma.Channels[1].CurrentCount == 7, "request + disabled controller: nothing moves");
        VddBusIo(&bus, 0x08, 1, 1, &status);
        CHECK(status == 0x20, "request + disabled controller: status bit 5 -- pending");
        value = 0;
        VddBusIo(&bus, 0x08, 1, 0, &value);                   /* enable */
        CHECK(dma.Channels[1].CurrentCount == 0xFFFF && dma.Channels[1].CurrentAddress == 0x8008 && dma.Request[0] == 0,
              "request: served the moment the controller is enabled");

        /* reset by bit 2 = 0, before it is served */
        DmaTestProgram(&bus, 1, 0x78000, 7, 0x80 | DMA_MODE_TRANSFER_VERIFY);
        value = 0x04;
        VddBusIo(&bus, 0xD4, 1, 0, &value);                /* cascade off */
        value = 0x05;
        VddBusIo(&bus, 0x09, 1, 0, &value);
        CHECK(dma.Request[0] == 0x02 && dma.Channels[1].CurrentCount == 7,
              "request + cascade down: latched, not served");
        value = 0x01;
        VddBusIo(&bus, 0x09, 1, 0, &value);                /* reset req 1 */
        CHECK(dma.Request[0] == 0, "request: bit 2 = 0 resets the request");
        value = 0x00;
        VddBusIo(&bus, 0xD4, 1, 0, &value);
        CHECK(dma.Channels[1].CurrentCount == 7, "...so restoring the cascade serves nothing");

        /* master clear clears it */
        value = 0x04;
        VddBusIo(&bus, 0xD4, 1, 0, &value);
        value = 0x05;
        VddBusIo(&bus, 0x09, 1, 0, &value);
        value = 0;
        VddBusIo(&bus, 0x0D, 1, 0, &value);
        CHECK(dma.Request[0] == 0, "request: master clear clears the request register");
        value = 0x00;
        VddBusIo(&bus, 0xD4, 1, 0, &value);

        /* a cascade-mode channel performs no cycles of its own */
        DmaTestProgram(&bus, 3, 0x78000, 3, DMA_MODE_SELECT | DMA_MODE_TRANSFER_WRITE);
        g_GuestMemory[0x78000] = 0x11;
        value = 0x07;
        VddBusIo(&bus, 0x09, 1, 0, &value);
        CHECK(g_GuestMemory[0x78000] == 0x11 && dma.Channels[3].CurrentCount == 3,
              "request: a channel in cascade mode does no cycles");
        value = 0x03;
        VddBusIo(&bus, 0x09, 1, 0, &value);
    }

    /* T14: MEMORY-TO-MEMORY + THE TEMPORARY REGISTER. #246:
     * Command bit 0; started by channel 0's software request; byte by byte through
     * the temporary register from channel 0's address to channel 1's; channel 1's
     * count runs and its TC ends it. The temporary register then reads the LAST
     * byte moved.
     */
    {
        UINT32 status;
        VddDmaReset(&dma);
        for (index = 0; index < 16; ++index)
        {
            g_GuestMemory[0x79000 + index] = (BYTE)(0xA0 + index);
            g_GuestMemory[0x7A000 + index] = 0;
        }
        DmaTestProgram(&bus, 0, 0x79000, 0xFFFF, 0x80 | DMA_MODE_TRANSFER_READ);  /* source */
        DmaTestProgram(&bus, 1, 0x7A000, 9, 0x80 | DMA_MODE_TRANSFER_WRITE);      /* 10 dest */
        value = DMA_COMMAND_MEMORY_TO_MEMORY;
        VddBusIo(&bus, 0x08, 1, 0, &value);
        value = 0x04;
        VddBusIo(&bus, 0x09, 1, 0, &value);                    /* req ch0 */
        CHECK(memcmp(&g_GuestMemory[0x7A000], &g_GuestMemory[0x79000], 10) == 0 && g_GuestMemory[0x7A00A] == 0,
              "m2m: ten bytes copied, the eleventh untouched");
        value = 0;
        VddBusIo(&bus, 0x0D, 1, 1, &value);
        CHECK(value == 0xA9, "m2m: the temporary register holds the last byte moved (A9h)");
        CHECK(dma.Channels[1].CurrentCount == 0xFFFF && dma.Channels[0].CurrentCount == 0xFFFF,
              "m2m: channel 1's count ran out; channel 0's was not consulted");
        CHECK(dma.Channels[0].CurrentAddress == 0x900A, "m2m: channel 0's address stepped ten times");
        VddBusIo(&bus, 0x08, 1, 1, &status);
        CHECK((status & 0x0F) == 0x02 && dma.Request[0] == 0, "m2m: TC on channel 1, request cleared");

        /* address hold: channel 0 stays on one byte -- a block fill */
        DmaTestProgram(&bus, 0, 0x79003, 0, 0x80 | DMA_MODE_TRANSFER_READ);
        DmaTestProgram(&bus, 1, 0x7A000, 15, 0x80 | DMA_MODE_TRANSFER_WRITE);
        value = DMA_COMMAND_MEMORY_TO_MEMORY | DMA_COMMAND_ADDRESS_HOLD;
        VddBusIo(&bus, 0x08, 1, 0, &value);
        value = 0x04;
        VddBusIo(&bus, 0x09, 1, 0, &value);
        CHECK(g_GuestMemory[0x7A000] == 0xA3 && g_GuestMemory[0x7A00F] == 0xA3 && dma.Channels[0].CurrentAddress == 0x9003,
              "m2m + address hold: sixteen copies of one byte, channel 0 never moved");

        /* channel 1's request alone does not start the copy */
        DmaTestProgram(&bus, 1, 0x7A000, 3, 0x80 | DMA_MODE_TRANSFER_WRITE);
        g_GuestMemory[0x7A000] = 0x00;
        value = 0x05;
        VddBusIo(&bus, 0x09, 1, 0, &value);
        CHECK(g_GuestMemory[0x7A000] == 0x00 && dma.Channels[1].CurrentCount == 3,
              "m2m: a request on channel 1 alone starts nothing");
        value = 0x01;
        VddBusIo(&bus, 0x09, 1, 0, &value);

        /* without command bit 0, channel 0's request is an ordinary device-less one */
        value = 0;
        VddBusIo(&bus, 0x08, 1, 0, &value);
        DmaTestProgram(&bus, 0, 0x79000, 1, 0x80 | DMA_MODE_TRANSFER_VERIFY);
        g_GuestMemory[0x7A001] = 0x55;
        value = 0x04;
        VddBusIo(&bus, 0x09, 1, 0, &value);
        CHECK(dma.Channels[0].CurrentCount == 0xFFFF && dma.Channels[0].CurrentAddress == 0x9002 && g_GuestMemory[0x7A001] == 0x55,
              "no command bit 0: channel 0's request is an ordinary block, no copy");

        /* master clear clears the temporary register */
        value = 0;
        VddBusIo(&bus, 0x0D, 1, 0, &value);
        value = 0xEE;
        VddBusIo(&bus, 0x0D, 1, 1, &value);
        CHECK(value == 0, "temporary register: master clear clears it");
    }

    /* T15: STATUS SHOWS A SOFTWARE REQUEST, AND IT RAISES HRQ. #246: */
    {
        UINT32 status;
        VddDmaReset(&dma);
        DmaTestProgram(&bus, 1, 0x78000, 3, 0x80 | DMA_MODE_TRANSFER_VERIFY);
        value = DMA_COMMAND_DISABLE;
        VddBusIo(&bus, 0xD0, 1, 0, &value);     /* cascade off */
        value = 0x05;
        VddBusIo(&bus, 0x09, 1, 0, &value);
        VddBusIo(&bus, 0x08, 1, 1, &status);
        CHECK((status & 0xF0) == 0x20, "status 08h: a pending software request shows as DRQ1");
        VddBusIo(&bus, 0xD0, 1, 1, &status);
        CHECK((status & 0x10) == 0x10, "status D0h: and controller 1 raises HRQ for it");
        value = 0;
        VddBusIo(&bus, 0xD0, 1, 0, &value);
        VddBusIo(&bus, 0x08, 1, 1, &status);
        CHECK(status == 0x02, "status 08h: served on re-enable -- TC1, nothing pending");
    }

    printf("-- %d checks, %d failures --\n", g_Total, g_Failures);
    return g_Failures ? 1 : 0;
}
