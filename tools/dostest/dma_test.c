/* dma_test.c -- off-VM unit battery for the ISA DMA controller VDD (vdd_dma.c).
 *
 * The first device of the sound epic: Sound Blaster playback is DMA, so the SB
 * VDD is only as correct as this one. The cases below target the things that are
 * easy to get subtly wrong and impossible to notice later -- the byte-pointer
 * flip-flop, the non-sequential page-register wiring, word addressing on the
 * 16-bit controller, terminal count, and auto-init ring wrap (how every DOS game
 * streams continuous audio).
 *
 * Entirely off-VM: the bus is given a plain 1MB array as guest memory.
 */
#include <stdio.h>
#include <string.h>
#include "vdd_dma.h"

static int total = 0, fails = 0;
#define CHECK(c,m) do{ total++; if(c){printf("  PASS  %s\n",(m));} \
    else{printf("  FAIL  %s\n",(m)); fails++;} }while(0)

static uint8_t g_flat[0x100000];

/* A stand-in for a sound card's DREQ lines (#176): the test sets the mask. */
static uint8_t g_dreq;
static uint8_t fake_dreq(const void *ctx) { return *(const uint8_t *)ctx; }

/* Program a channel the way a DOS sound driver does: clear the flip-flop, write
   address lo/hi, count lo/hi, the page, then the mode, then unmask. */
static void program(vdd_bus *bus, int ch, uint32_t phys, uint16_t count, uint8_t mode)
{
    int c2 = (ch >= 4);
    uint16_t p_addr = c2 ? (uint16_t)(0xC0 + ((ch - 4) * 4))     : (uint16_t)(ch * 2);
    uint16_t p_cnt  = c2 ? (uint16_t)(0xC0 + ((ch - 4) * 4) + 2) : (uint16_t)(ch * 2 + 1);
    uint16_t p_ff   = c2 ? 0xD8 : 0x0C;
    uint16_t p_mode = c2 ? 0xD6 : 0x0B;
    uint16_t p_mask = c2 ? 0xD4 : 0x0A;
    uint16_t p_page;
    uint16_t addr   = c2 ? (uint16_t)((phys >> 1) & 0xFFFF) : (uint16_t)(phys & 0xFFFF);
    uint32_t v;

    switch (ch) {
    case 0: p_page = 0x87; break;  case 1: p_page = 0x83; break;
    case 2: p_page = 0x81; break;  case 3: p_page = 0x82; break;
    case 5: p_page = 0x8B; break;  case 6: p_page = 0x89; break;
    default: p_page = 0x8A; break;
    }
    v = 0;            vdd_bus_io(bus, p_ff,   1, 0, &v);
    v = addr & 0xFF;  vdd_bus_io(bus, p_addr, 1, 0, &v);
    v = addr >> 8;    vdd_bus_io(bus, p_addr, 1, 0, &v);
    v = count & 0xFF; vdd_bus_io(bus, p_cnt,  1, 0, &v);
    v = count >> 8;   vdd_bus_io(bus, p_cnt,  1, 0, &v);
    v = (phys >> 16) & 0xFF; vdd_bus_io(bus, p_page, 1, 0, &v);
    v = mode | (uint32_t)(ch & 3); vdd_bus_io(bus, p_mode, 1, 0, &v);
    v = (uint32_t)(ch & 3);        vdd_bus_io(bus, p_mask, 1, 0, &v);   /* unmask */
}

int main(void)
{
    vdd_bus bus;
    dma_state dma; memset(&dma, 0, sizeof dma);
    ntvdd ddev = vdd_dma_device(&dma);
    uint8_t buf[512];
    uint32_t v, got;
    int tc, i;

    printf("== sound epic: ISA DMA controller battery ==\n");

    memset(g_flat, 0, sizeof g_flat);
    vdd_bus_init(&bus, g_flat);
    CHECK(vdd_bus_add(&bus, &ddev) == 0, "add: dma device ok");

    /* T1: all channels start masked (master clear at init) ------------------ */
    CHECK(dma.ch[1].masked == 1, "init: channel 1 masked after master clear");
    got = vdd_dma_read(&dma, 1, buf, 16, &tc);
    CHECK(got == 0, "masked channel transfers nothing");

    /* T2: flip-flop + page wiring ------------------------------------------ */
    v = 0;    vdd_bus_io(&bus, 0x0C, 1, 0, &v);      /* clear byte pointer      */
    v = 0x34; vdd_bus_io(&bus, 0x02, 1, 0, &v);      /* ch1 addr lo             */
    v = 0x12; vdd_bus_io(&bus, 0x02, 1, 0, &v);      /* ch1 addr hi             */
    CHECK(dma.ch[1].base_addr == 0x1234, "flip-flop: lo then hi -> 0x1234");
    CHECK(dma.ch[1].cur_addr == 0x1234, "current address loaded from base");
    v = 0x07; vdd_bus_io(&bus, 0x83, 1, 0, &v);      /* ch1 page (port 0x83!)   */
    CHECK(dma.ch[1].page == 0x07, "page register 0x83 maps to channel 1");
    CHECK(vdd_dma_cur_phys(&dma, 1) == 0x71234, "8-bit channel: phys = page<<16|addr");

    /* the flip-flop must alternate, not latch: a second lo/hi pair works too  */
    v = 0x78; vdd_bus_io(&bus, 0x02, 1, 0, &v);
    v = 0x56; vdd_bus_io(&bus, 0x02, 1, 0, &v);
    CHECK(dma.ch[1].base_addr == 0x5678, "flip-flop alternates across writes");

    /* T3: a plain single-cycle read transfer (memory -> device) ------------- */
    for (i = 0; i < 256; ++i) g_flat[0x71000 + i] = (uint8_t)i;
    program(&bus, 1, 0x71000, 99, DMA_MODE_XFER_READ);   /* 100 bytes           */
    CHECK(dma.ch[1].masked == 0, "unmask via single-mask register");
    CHECK(vdd_dma_remaining(&dma, 1) == 100, "remaining = count+1 bytes");

    memset(buf, 0, sizeof buf);
    got = vdd_dma_read(&dma, 1, buf, 40, &tc);
    CHECK(got == 40, "partial read: 40 bytes");
    CHECK(!tc, "partial read: no terminal count yet");
    CHECK(buf[0] == 0 && buf[39] == 39, "partial read: correct bytes from guest memory");
    CHECK(vdd_dma_cur_phys(&dma, 1) == 0x71000 + 40, "address advanced by 40");
    CHECK(vdd_dma_remaining(&dma, 1) == 60, "remaining dropped to 60");

    /* T4: terminal count stops a non-auto-init channel and masks it --------- */
    got = vdd_dma_read(&dma, 1, buf, 100, &tc);
    CHECK(got == 60, "read past the end stops at terminal count (60 of 100)");
    CHECK(tc, "terminal count reported");
    CHECK(dma.ch[1].masked == 1, "8237 masks a non-auto-init channel at TC");
    CHECK(buf[59] == 99, "last byte of the block is correct");
    got = vdd_dma_read(&dma, 1, buf, 8, &tc);
    CHECK(got == 0, "channel is finished: no further transfer");

    /* T5: status register reports TC and clears it on read ------------------ */
    vdd_bus_io(&bus, 0x08, 1, 1, &v);
    CHECK((v & 0x02) != 0, "status: TC bit set for channel 1");
    vdd_bus_io(&bus, 0x08, 1, 1, &v);
    CHECK((v & 0x02) == 0, "status: reading it clears TC");

    /* T6: auto-init wraps and keeps streaming (the audio ring buffer) ------- */
    for (i = 0; i < 16; ++i) g_flat[0x72000 + i] = (uint8_t)(0xA0 + i);
    program(&bus, 1, 0x72000, 15, DMA_MODE_XFER_READ | DMA_MODE_AUTOINIT);
    memset(buf, 0, sizeof buf);
    got = vdd_dma_read(&dma, 1, buf, 40, &tc);
    CHECK(got == 40, "auto-init: transfer continues past the end");
    CHECK(tc, "auto-init: terminal count still reported");
    CHECK(dma.ch[1].masked == 0, "auto-init: channel stays unmasked");
    CHECK(buf[0] == 0xA0 && buf[15] == 0xAF, "auto-init: first pass correct");
    CHECK(buf[16] == 0xA0 && buf[31] == 0xAF, "auto-init: wrapped to base, second pass");
    CHECK(buf[32] == 0xA0, "auto-init: third pass continues the ring");
    CHECK(vdd_dma_cur_phys(&dma, 1) == 0x72000 + 8, "auto-init: address mid-ring after 40");

    /* T7: decrement mode walks the address downwards ------------------------ */
    program(&bus, 3, 0x73100, 3, DMA_MODE_XFER_READ | DMA_MODE_DECREMENT);
    for (i = 0; i < 4; ++i) g_flat[0x73100 - i] = (uint8_t)(0x10 + i);
    memset(buf, 0, sizeof buf);
    got = vdd_dma_read(&dma, 3, buf, 4, &tc);
    CHECK(got == 4, "decrement: 4 bytes transferred");
    CHECK(buf[0] == 0x10 && buf[1] == 0x11 && buf[3] == 0x13,
          "decrement: address walked downwards");

    /* T8: 16-bit controller -- word addressing, word counts ----------------- */
    for (i = 0; i < 32; ++i) g_flat[0x84000 + i] = (uint8_t)(0x40 + i);
    program(&bus, 5, 0x84000, 7, DMA_MODE_XFER_READ);    /* 8 words = 16 bytes  */
    CHECK(dma.ch[5].base_addr == 0x2000, "16-bit channel: address register is a WORD address");
    CHECK(vdd_dma_cur_phys(&dma, 5) == 0x84000, "16-bit channel: phys = (page&0xFE)<<16|addr<<1");
    CHECK(vdd_dma_remaining(&dma, 5) == 16, "16-bit channel: remaining counts BYTES (8 words)");
    memset(buf, 0, sizeof buf);
    got = vdd_dma_read(&dma, 5, buf, 16, &tc);
    CHECK(got == 16, "16-bit channel: 16 bytes transferred");
    CHECK(tc, "16-bit channel: terminal count at 8 words");
    CHECK(buf[0] == 0x40 && buf[15] == 0x4F, "16-bit channel: correct bytes");

    /* T9: device -> memory (recording direction) ---------------------------- */
    program(&bus, 1, 0x75000, 7, DMA_MODE_XFER_WRITE);
    for (i = 0; i < 8; ++i) buf[i] = (uint8_t)(0xE0 + i);
    got = vdd_dma_write(&dma, 1, buf, 8, &tc);
    CHECK(got == 8, "write direction: 8 bytes accepted");
    CHECK(g_flat[0x75000] == 0xE0 && g_flat[0x75007] == 0xE7,
          "write direction: bytes landed in guest memory");

    /* T10: mask register variants ------------------------------------------ */
    v = 0x0F; vdd_bus_io(&bus, 0x0F, 1, 0, &v);          /* mask all 4 channels */
    CHECK(dma.ch[0].masked && dma.ch[3].masked, "write-all-mask masks every channel");
    v = 0x00; vdd_bus_io(&bus, 0x0E, 1, 0, &v);          /* clear mask register */
    CHECK(!dma.ch[0].masked && !dma.ch[3].masked, "clear-mask unmasks every channel");

    /* ── THE PAGE PORTS THAT MAP TO NO CHANNEL. docs/ref/dma.md 3. ────────────
       Seven of the sixteen ports at 80h-8Fh carry a channel's high address bits;
       the other nine are read/write latches on a PC anyway, because the address
       decoder does not bother to leave them out.
       ★ MEASURED: dosbox-x and PCem (real AMI BIOS) both read back a written
         0x5A at port 80h; only 6.22-under-QEMU answers 0xFF. We answered 0xFF --
         an empty bus rather than a machine. */
    {
        uint32_t v;
        v = 0x5A; vdd_bus_io(&bus, 0x80, 1, 0, &v);
        v = 0;    vdd_bus_io(&bus, 0x80, 1, 1, &v);
        CHECK(v == 0x5A, "page: port 80h is a latch, not an empty bus");

        v = 0xA5; vdd_bus_io(&bus, 0x8C, 1, 0, &v);
        v = 0;    vdd_bus_io(&bus, 0x8C, 1, 1, &v);
        CHECK(v == 0xA5, "page: so is 8Ch");

        /* ...and the mapped ones are unaffected: the spare latches must not be
           the same storage, or writing scratch would move a channel's page. */
        v = 0x33; vdd_bus_io(&bus, 0x83, 1, 0, &v);      /* channel 1's page    */
        CHECK(dma.ch[1].page == 0x33, "page: 83h still reaches channel 1");
        v = 0;    vdd_bus_io(&bus, 0x80, 1, 1, &v);
        CHECK(v == 0x5A, "page: ...and did not disturb the spare at 80h");
    }

    /* ── T11: STATUS BITS 7:4 -- "REQUEST PENDING". #176. ──────────────────────
       8237A datasheet: bits 4-7 "are set whenever their corresponding channel is
       requesting service". They read 0 always before #176. A stand-in device
       drives the DREQ lines through the same registration a sound card uses. */
    {
        uint32_t s;
        CHECK(vdd_dma_add_dreq(&dma, fake_dreq, &g_dreq) == 0, "dreq: a device registers its lines");
        CHECK(vdd_dma_add_dreq(&dma, fake_dreq, &g_dreq) == 0 && dma.dreq_n == 1,
              "dreq: registering the same device twice is one registration");

        v = 0; vdd_bus_io(&bus, 0x0D, 1, 0, &v);         /* master clear ctrl 1   */
        v = 0; vdd_bus_io(&bus, 0xDA, 1, 0, &v);         /* master clear ctrl 2   */
        g_dreq = 0;
        vdd_bus_io(&bus, 0x08, 1, 1, &s);
        CHECK((s & DMA_STATUS_DRQ) == 0, "status 08h: no device requesting -> bits 7:4 clear");

        g_dreq = 0x0A;                                    /* DREQ 1 and 3          */
        vdd_bus_io(&bus, 0x08, 1, 1, &s);
        CHECK((s & DMA_STATUS_DRQ) == 0xA0, "status 08h: DREQ1+DREQ3 -> bits 5 and 7");
        CHECK(dma.ch[1].masked && dma.ch[3].masked,
              "...and that is with both channels MASKED: a refused request is still pending");

        g_dreq = 0xA0;                                    /* DREQ 5 and 7          */
        vdd_bus_io(&bus, 0x08, 1, 1, &s);
        CHECK((s & DMA_STATUS_DRQ) == 0, "status 08h: controller 2's requests are not controller 1's");
        vdd_bus_io(&bus, 0xD0, 1, 1, &s);
        CHECK((s & DMA_STATUS_DRQ) == 0xA0, "status D0h: DREQ5+DREQ7 -> bits 5 and 7");

        /* channel 4 is the CASCADE: controller 1's HRQ, raised only for a request
           controller 1 would serve. A device cannot drive it itself. */
        g_dreq = 0x10;
        vdd_bus_io(&bus, 0xD0, 1, 1, &s);
        CHECK((s & 0x10) == 0, "status D0h: a device's claim on channel 4 is dropped");
        g_dreq = 0x02;                                    /* DREQ1, ch1 masked     */
        vdd_bus_io(&bus, 0xD0, 1, 1, &s);
        CHECK((s & 0x10) == 0, "status D0h bit 4: masked ch1 request -> no HRQ, no DREQ4");
        program(&bus, 1, 0x76000, 15, DMA_MODE_XFER_READ);
        vdd_bus_io(&bus, 0xD0, 1, 1, &s);
        CHECK((s & 0x10) != 0, "status D0h bit 4: unmasked ch1 request -> the cascade requests");

        /* TC clears on read; DRQ does not -- the request is still there afterwards */
        got = vdd_dma_read(&dma, 1, buf, 16, &tc);
        CHECK(got == 16 && tc, "status: a 16-byte block reaches TC on channel 1");
        vdd_bus_io(&bus, 0x08, 1, 1, &s);
        CHECK(s == (0x02 | 0x20), "status: TC1 and DRQ1 together on the first read");
        vdd_bus_io(&bus, 0x08, 1, 1, &s);
        CHECK(s == 0x20, "status: the read cleared TC1 and left DRQ1 standing");
        g_dreq = 0;
        vdd_bus_io(&bus, 0x08, 1, 1, &s);
        CHECK(s == 0x00, "status: DRQ1 falls when the device stops requesting");
    }

    /* ── T12: COMMAND BIT 2 -- CONTROLLER DISABLE. #176. ───────────────────────
       Stored and read by nothing before #176. With it set the 8237 gives no DACK
       on any of the controller's channels: no byte moves, the address and count
       stand still, no TC -- and re-enabling resumes exactly where it stopped. */
    {
        uint32_t s;
        for (i = 0; i < 64; ++i) g_flat[0x77000 + i] = (uint8_t)(0x80 + i);
        program(&bus, 1, 0x77000, 31, DMA_MODE_XFER_READ);         /* 32 bytes   */
        got = vdd_dma_read(&dma, 1, buf, 8, &tc);
        CHECK(got == 8 && buf[7] == 0x87, "disable: 8 bytes move while enabled");
        CHECK(vdd_dma_grants(&dma, 1), "grants: unmasked + enabled -> served");

        v = DMA_CMD_DISABLE; vdd_bus_io(&bus, 0x08, 1, 0, &v);
        CHECK(!vdd_dma_grants(&dma, 1), "grants: command bit 2 on 08h -> refused");
        CHECK(dma.ch[1].masked == 0, "disable: is not the mask -- the mask bit is untouched");
        memset(buf, 0, sizeof buf);
        got = vdd_dma_read(&dma, 1, buf, 64, &tc);
        CHECK(got == 0 && !tc, "disable: a read moves nothing and reaches no TC");
        CHECK(vdd_dma_cur_phys(&dma, 1) == 0x77008 && vdd_dma_remaining(&dma, 1) == 24,
              "disable: address and count stand still");
        got = vdd_dma_write(&dma, 1, buf, 4, &tc);
        CHECK(got == 0 && g_flat[0x77008] == 0x88, "disable: the write direction is refused too");

        g_dreq = 0x02;
        vdd_bus_io(&bus, 0x08, 1, 1, &s);
        CHECK(s == 0x20, "disable: the request shows as pending, with no TC");
        vdd_bus_io(&bus, 0xD0, 1, 1, &s);
        CHECK((s & 0x10) == 0, "disable: a disabled controller 1 raises no HRQ (D0h bit 4)");
        g_dreq = 0;

        /* the other controller is not affected */
        for (i = 0; i < 4; ++i) g_flat[0x86000 + i] = (uint8_t)(0x60 + i);
        program(&bus, 5, 0x86000, 1, DMA_MODE_XFER_READ);          /* 2 words    */
        got = vdd_dma_read(&dma, 5, buf, 4, &tc);
        CHECK(got == 4 && buf[0] == 0x60, "disable 08h: controller 2's channel 5 still moves");

        v = 0; vdd_bus_io(&bus, 0x08, 1, 0, &v);                   /* re-enable  */
        got = vdd_dma_read(&dma, 1, buf, 64, &tc);
        CHECK(got == 24 && tc, "re-enable: the remaining 24 bytes move, then TC");
        CHECK(buf[0] == 0x88 && buf[23] == 0x9F, "re-enable: resumed at byte 8, not the base");

        /* controller 2's own bit, at D0h, stops channel 5 and not channel 1 */
        program(&bus, 5, 0x86000, 1, DMA_MODE_XFER_READ);
        program(&bus, 1, 0x77000, 3, DMA_MODE_XFER_READ);
        v = DMA_CMD_DISABLE; vdd_bus_io(&bus, 0xD0, 1, 0, &v);
        CHECK(!vdd_dma_grants(&dma, 5) && vdd_dma_grants(&dma, 1),
              "disable D0h: refuses channels 4-7 only");
        got = vdd_dma_read(&dma, 5, buf, 4, &tc);
        CHECK(got == 0 && !tc, "disable D0h: channel 5 moves nothing");
        got = vdd_dma_read(&dma, 1, buf, 4, &tc);
        CHECK(got == 4, "disable D0h: channel 1 is untouched (the cascade is not modelled)");

        /* master clear clears the command register -- the controller is enabled
           again, and every channel masked (datasheet) */
        v = 0; vdd_bus_io(&bus, 0xDA, 1, 0, &v);
        CHECK(dma.cmd[1] == 0 && dma.ch[5].masked, "master clear D0h: enabled again, channel masked");
        v = 0x01; vdd_bus_io(&bus, 0xD4, 1, 0, &v);                /* unmask ch5 */
        CHECK(vdd_dma_grants(&dma, 5), "master clear D0h: unmasking alone serves channel 5 again");

        /* the DREQ wiring is the machine's: a device reset keeps it */
        vdd_dma_reset(&dma);
        CHECK(dma.dreq_n == 1 && dma.dreq_fn[0] == fake_dreq, "reset: the DREQ registration survives");
    }

    printf("-- %d checks, %d failures --\n", total, fails);
    return fails ? 1 : 0;
}
