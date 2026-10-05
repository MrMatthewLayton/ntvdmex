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
    /* #246: "TC when the word count goes from 0000h to FFFFh" -- the count wraps,
       and a driver polling it for FFFFh to see a single-cycle block end needs it to.
       It rested at 0000h before; p_dma2's TC rows made that visible. */
    { uint32_t lo = 0, hi = 0;
      v = 0; vdd_bus_io(&bus, 0x0C, 1, 0, &v);
      vdd_bus_io(&bus, 0x03, 1, 1, &lo); vdd_bus_io(&bus, 0x03, 1, 1, &hi);
      CHECK(lo == 0xFF && hi == 0xFF, "TC: the current count reads FFFFh through the port"); }
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
        /* ...which masks channel 4, the cascade, and so disconnects controller 1
           (#246). Put it back the way POST does: cascade mode, unmasked. */
        v = 0xC0; vdd_bus_io(&bus, 0xD6, 1, 0, &v);
        v = 0x00; vdd_bus_io(&bus, 0xD4, 1, 0, &v);
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

        /* controller 2's own bit, at D0h, stops channel 5 -- AND, on an AT, channel
           1 as well: controller 1 reaches the bus only through channel 4 (#246).
           Before #246 this case asserted the opposite ("the cascade is not
           modelled"); that was a statement about our model, not the board. */
        program(&bus, 5, 0x86000, 1, DMA_MODE_XFER_READ);
        program(&bus, 1, 0x77000, 3, DMA_MODE_XFER_READ);
        v = 0; vdd_bus_io(&bus, 0x08, 1, 1, &v);                   /* drop TC1   */
        v = DMA_CMD_DISABLE; vdd_bus_io(&bus, 0xD0, 1, 0, &v);
        CHECK(!vdd_dma_grants(&dma, 5) && !vdd_dma_grants(&dma, 1) && !vdd_dma_grants(&dma, 3),
              "disable D0h: refuses channels 4-7 AND, through the cascade, 0-3");
        got = vdd_dma_read(&dma, 5, buf, 4, &tc);
        CHECK(got == 0 && !tc, "disable D0h: channel 5 moves nothing");
        got = vdd_dma_read(&dma, 1, buf, 4, &tc);
        CHECK(got == 0 && !tc, "disable D0h: channel 1 moves nothing either (the AT cascade)");
        g_dreq = 0x02;
        vdd_bus_io(&bus, 0xD0, 1, 1, &v);
        CHECK((v & 0x10) != 0,
              "disable D0h: controller 1 still RAISES HRQ (DREQ4) -- it is the far end that refuses");
        g_dreq = 0;
        v = 0; vdd_bus_io(&bus, 0xD0, 1, 0, &v);                   /* re-enable  */
        CHECK(vdd_dma_grants(&dma, 1), "enable D0h: channel 1 is served again");
        /* masking channel 4 alone does the same thing */
        v = 0x04; vdd_bus_io(&bus, 0xD4, 1, 0, &v);                /* mask ch4   */
        CHECK(!vdd_dma_grants(&dma, 1) && vdd_dma_grants(&dma, 5),
              "cascade: masking channel 4 starves channel 1, not channel 5");
        v = 0x00; vdd_bus_io(&bus, 0xD4, 1, 0, &v);                /* unmask ch4 */
        CHECK(vdd_dma_grants(&dma, 1), "cascade: unmasking channel 4 reconnects controller 1");
        got = vdd_dma_read(&dma, 1, buf, 4, &tc);
        CHECK(got == 4 && tc, "cascade: and the stalled 4-byte block then completes");

        /* master clear clears the command register -- the controller is enabled
           again, and every channel masked (datasheet) */
        v = 0; vdd_bus_io(&bus, 0xDA, 1, 0, &v);
        CHECK(dma.cmd[1] == 0 && dma.ch[5].masked, "master clear D0h: enabled again, channel masked");
        v = 0x01; vdd_bus_io(&bus, 0xD4, 1, 0, &v);                /* unmask ch5 */
        CHECK(vdd_dma_grants(&dma, 5), "master clear D0h: unmasking alone serves channel 5 again");

        /* the DREQ wiring is the machine's: a device reset keeps it */
        vdd_dma_reset(&dma);
        CHECK(dma.dreq_n == 1 && dma.dreq_fn[0] == fake_dreq, "reset: the DREQ registration survives");
        /* ...and a reset leaves what POST leaves: channel 4 cascade, unmasked */
        CHECK((dma.ch[4].mode & DMA_MODE_SELECT) == DMA_MODE_SELECT && !dma.ch[4].masked,
              "reset: channel 4 in cascade mode and unmasked, as POST leaves it");
    }

    /* ── T13: THE REQUEST REGISTER (09h). #246. ───────────────────────────────
       8237A datasheet: a request bit per channel, set/reset by bits 2 and 1:0,
       NON-MASKABLE, cleared at TC and by master clear. With no device on DACK the
       controller carries the cycles out itself. Channel 1 in VERIFY mode: the walk
       happens, memory is not touched, TC latches. */
    {
        uint32_t s;
        vdd_dma_reset(&dma);
        for (i = 0; i < 32; ++i) g_flat[0x78000 + i] = (uint8_t)(0x40 + i);
        program(&bus, 1, 0x78000, 15, 0x80 /* block */ | DMA_MODE_XFER_VERIFY);
        v = 0x05; vdd_bus_io(&bus, 0x0A, 1, 0, &v);                /* MASK ch1   */
        v = 0x05; vdd_bus_io(&bus, 0x09, 1, 0, &v);                /* request 1  */
        CHECK(dma.ch[1].cur_count == 0xFFFF && dma.ch[1].cur_addr == 0x8010,
              "request: a masked channel is served anyway -- 16 verify cycles, count FFFFh");
        CHECK(g_flat[0x78000] == 0x40 && g_flat[0x7800F] == 0x4F, "request: verify touched no memory");
        CHECK(dma.req[0] == 0, "request: the bit cleared itself at TC");
        vdd_bus_io(&bus, 0x08, 1, 1, &s);
        CHECK(s == 0x02, "request: status shows TC1 and no request left pending");

        /* write type: memory gets the undriven bus -- FFh */
        program(&bus, 1, 0x78000, 3, 0x80 | DMA_MODE_XFER_WRITE);
        v = 0x05; vdd_bus_io(&bus, 0x09, 1, 0, &v);
        CHECK(g_flat[0x78000] == 0xFF && g_flat[0x78003] == 0xFF && g_flat[0x78004] == 0x44,
              "request, write type: exactly 4 bytes of float (FFh), not 5");
        CHECK(dma.ch[1].masked, "request: a non-auto-init channel masks itself at TC");

        /* held off by a disabled controller: pending, visible, then served */
        program(&bus, 1, 0x78000, 7, 0x80 | DMA_MODE_XFER_VERIFY);
        v = 0; vdd_bus_io(&bus, 0x08, 1, 1, &v);                   /* drop TCs   */
        v = DMA_CMD_DISABLE; vdd_bus_io(&bus, 0x08, 1, 0, &v);
        v = 0x05; vdd_bus_io(&bus, 0x09, 1, 0, &v);
        CHECK(dma.ch[1].cur_count == 7, "request + disabled controller: nothing moves");
        vdd_bus_io(&bus, 0x08, 1, 1, &s);
        CHECK(s == 0x20, "request + disabled controller: status bit 5 -- pending");
        v = 0; vdd_bus_io(&bus, 0x08, 1, 0, &v);                   /* enable     */
        CHECK(dma.ch[1].cur_count == 0xFFFF && dma.ch[1].cur_addr == 0x8008 && dma.req[0] == 0,
              "request: served the moment the controller is enabled");

        /* reset by bit 2 = 0, before it is served */
        program(&bus, 1, 0x78000, 7, 0x80 | DMA_MODE_XFER_VERIFY);
        v = 0x04; vdd_bus_io(&bus, 0xD4, 1, 0, &v);                /* cascade off */
        v = 0x05; vdd_bus_io(&bus, 0x09, 1, 0, &v);
        CHECK(dma.req[0] == 0x02 && dma.ch[1].cur_count == 7,
              "request + cascade down: latched, not served");
        v = 0x01; vdd_bus_io(&bus, 0x09, 1, 0, &v);                /* reset req 1 */
        CHECK(dma.req[0] == 0, "request: bit 2 = 0 resets the request");
        v = 0x00; vdd_bus_io(&bus, 0xD4, 1, 0, &v);
        CHECK(dma.ch[1].cur_count == 7, "...so restoring the cascade serves nothing");

        /* master clear clears it */
        v = 0x04; vdd_bus_io(&bus, 0xD4, 1, 0, &v);
        v = 0x05; vdd_bus_io(&bus, 0x09, 1, 0, &v);
        v = 0; vdd_bus_io(&bus, 0x0D, 1, 0, &v);
        CHECK(dma.req[0] == 0, "request: master clear clears the request register");
        v = 0x00; vdd_bus_io(&bus, 0xD4, 1, 0, &v);

        /* a cascade-mode channel performs no cycles of its own */
        program(&bus, 3, 0x78000, 3, DMA_MODE_SELECT | DMA_MODE_XFER_WRITE);
        g_flat[0x78000] = 0x11;
        v = 0x07; vdd_bus_io(&bus, 0x09, 1, 0, &v);
        CHECK(g_flat[0x78000] == 0x11 && dma.ch[3].cur_count == 3,
              "request: a channel in cascade mode does no cycles");
        v = 0x03; vdd_bus_io(&bus, 0x09, 1, 0, &v);
    }

    /* ── T14: MEMORY-TO-MEMORY + THE TEMPORARY REGISTER. #246. ────────────────
       Command bit 0; started by channel 0's software request; byte by byte through
       the temporary register from channel 0's address to channel 1's; channel 1's
       count runs and its TC ends it. The temporary register then reads the LAST
       byte moved. */
    {
        uint32_t s;
        vdd_dma_reset(&dma);
        for (i = 0; i < 16; ++i) { g_flat[0x79000 + i] = (uint8_t)(0xA0 + i); g_flat[0x7A000 + i] = 0; }
        program(&bus, 0, 0x79000, 0xFFFF, 0x80 | DMA_MODE_XFER_READ);  /* source  */
        program(&bus, 1, 0x7A000, 9, 0x80 | DMA_MODE_XFER_WRITE);      /* 10 dest */
        v = DMA_CMD_MEM2MEM; vdd_bus_io(&bus, 0x08, 1, 0, &v);
        v = 0x04; vdd_bus_io(&bus, 0x09, 1, 0, &v);                    /* req ch0 */
        CHECK(memcmp(&g_flat[0x7A000], &g_flat[0x79000], 10) == 0 && g_flat[0x7A00A] == 0,
              "m2m: ten bytes copied, the eleventh untouched");
        v = 0; vdd_bus_io(&bus, 0x0D, 1, 1, &v);
        CHECK(v == 0xA9, "m2m: the temporary register holds the last byte moved (A9h)");
        CHECK(dma.ch[1].cur_count == 0xFFFF && dma.ch[0].cur_count == 0xFFFF,
              "m2m: channel 1's count ran out; channel 0's was not consulted");
        CHECK(dma.ch[0].cur_addr == 0x900A, "m2m: channel 0's address stepped ten times");
        vdd_bus_io(&bus, 0x08, 1, 1, &s);
        CHECK((s & 0x0F) == 0x02 && dma.req[0] == 0, "m2m: TC on channel 1, request cleared");

        /* address hold: channel 0 stays on one byte -- a block fill */
        program(&bus, 0, 0x79003, 0, 0x80 | DMA_MODE_XFER_READ);
        program(&bus, 1, 0x7A000, 15, 0x80 | DMA_MODE_XFER_WRITE);
        v = DMA_CMD_MEM2MEM | DMA_CMD_ADDRHOLD; vdd_bus_io(&bus, 0x08, 1, 0, &v);
        v = 0x04; vdd_bus_io(&bus, 0x09, 1, 0, &v);
        CHECK(g_flat[0x7A000] == 0xA3 && g_flat[0x7A00F] == 0xA3 && dma.ch[0].cur_addr == 0x9003,
              "m2m + address hold: sixteen copies of one byte, channel 0 never moved");

        /* channel 1's request alone does not start the copy */
        program(&bus, 1, 0x7A000, 3, 0x80 | DMA_MODE_XFER_WRITE);
        g_flat[0x7A000] = 0x00;
        v = 0x05; vdd_bus_io(&bus, 0x09, 1, 0, &v);
        CHECK(g_flat[0x7A000] == 0x00 && dma.ch[1].cur_count == 3,
              "m2m: a request on channel 1 alone starts nothing");
        v = 0x01; vdd_bus_io(&bus, 0x09, 1, 0, &v);

        /* without command bit 0, channel 0's request is an ordinary device-less one */
        v = 0; vdd_bus_io(&bus, 0x08, 1, 0, &v);
        program(&bus, 0, 0x79000, 1, 0x80 | DMA_MODE_XFER_VERIFY);
        g_flat[0x7A001] = 0x55;
        v = 0x04; vdd_bus_io(&bus, 0x09, 1, 0, &v);
        CHECK(dma.ch[0].cur_count == 0xFFFF && dma.ch[0].cur_addr == 0x9002 && g_flat[0x7A001] == 0x55,
              "no command bit 0: channel 0's request is an ordinary block, no copy");

        /* master clear clears the temporary register */
        v = 0; vdd_bus_io(&bus, 0x0D, 1, 0, &v);
        v = 0xEE; vdd_bus_io(&bus, 0x0D, 1, 1, &v);
        CHECK(v == 0, "temporary register: master clear clears it");
    }

    /* ── T15: STATUS SHOWS A SOFTWARE REQUEST, AND IT RAISES HRQ. #246. ───────── */
    {
        uint32_t s;
        vdd_dma_reset(&dma);
        program(&bus, 1, 0x78000, 3, 0x80 | DMA_MODE_XFER_VERIFY);
        v = DMA_CMD_DISABLE; vdd_bus_io(&bus, 0xD0, 1, 0, &v);     /* cascade off */
        v = 0x05; vdd_bus_io(&bus, 0x09, 1, 0, &v);
        vdd_bus_io(&bus, 0x08, 1, 1, &s);
        CHECK((s & 0xF0) == 0x20, "status 08h: a pending software request shows as DRQ1");
        vdd_bus_io(&bus, 0xD0, 1, 1, &s);
        CHECK((s & 0x10) == 0x10, "status D0h: and controller 1 raises HRQ for it");
        v = 0; vdd_bus_io(&bus, 0xD0, 1, 0, &v);
        vdd_bus_io(&bus, 0x08, 1, 1, &s);
        CHECK(s == 0x02, "status 08h: served on re-enable -- TC1, nothing pending");
    }

    printf("-- %d checks, %d failures --\n", total, fails);
    return fails ? 1 : 0;
}
