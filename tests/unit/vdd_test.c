/* vdd_test.c -- off-VM unit battery for the VDD device bus (vdd_bus.c/ntvdd.h).
 *
 * Slice-1 of M3 (ADR-0008): prove the bus routes hardware events to whichever
 * VDD claimed them -- I/O ports, memory windows, software interrupts, the frame
 * tick -- and that the service callbacks (raise_irq, map_flat, present) work,
 * all natively on the build host with no XP VM. A stand-in "fake PIT" VDD
 * exercises the same claim surface the real vdd_pit will use.
 */
#include <stdio.h>
#include <string.h>
#include "vdd_bus.h"

static int total = 0, fails = 0;
#define CHECK(cond, msg) do {                                  \
        total++;                                               \
        if (cond) { printf("  PASS  %s\n", (msg)); }           \
        else      { printf("  FAIL  %s\n", (msg)); fails++; }  \
    } while (0)

/* ---- a fake device: claims the PIT ports, INT 1Ah, and a frame tick ------ */
typedef struct {
    vdd_bus *bus;
    uint8_t  reload;        /* last byte OUT to port 0x40                       */
    uint32_t ticks;         /* incremented each frame; raises IRQ0             */
    int      reset_calls;
    uint8_t  last_in_port;
} fake_pit;

static void pit_out(void *self, uint16_t port, uint8_t w, uint32_t v)
{ fake_pit *p = (fake_pit *)self; (void)w; if (port == 0x40) p->reload = (uint8_t)v; }

static void pit_in(void *self, uint16_t port, uint8_t w, uint32_t *v)
{ fake_pit *p = (fake_pit *)self; (void)w; p->last_in_port = (uint8_t)port; *v = p->reload; }

static void pit_int1a(void *self, ntvdd_regs *r)
{ fake_pit *p = (fake_pit *)self; if (r_ah(r) == 0) { s_cx(r, (uint16_t)(p->ticks >> 16)); s_dx(r, (uint16_t)p->ticks); r->cf = 0; } }

static void pit_frame(void *self)
{ fake_pit *p = (fake_pit *)self; p->ticks++; vdd_raise_irq(p->bus, 0); }

static void pit_reset(void *self)
{ fake_pit *p = (fake_pit *)self; p->reset_calls++; p->reload = 0; p->ticks = 0; }

static int pit_init(vdd_bus *b, void *self)
{
    fake_pit *p = (fake_pit *)self;
    p->bus = b;
    if (vdd_claim_ports(b, 0x40, 0x43, pit_in, pit_out, p)) return -1;
    if (vdd_claim_int(b, 0x1A, pit_int1a, p)) return -1;
    if (vdd_on_frame(b, pit_frame, p)) return -1;
    return 0;
}

/* ---- a fake video-ish device: claims a memory window at 0xB8000 ---------- */
static uint8_t vram[0x8000];
static uint8_t mem_rd(void *self, uint32_t off) { (void)self; return vram[off & 0x7FFF]; }
static void    mem_wr(void *self, uint32_t off, uint8_t v) { (void)self; vram[off & 0x7FFF] = v; }

/* ---- host-injected sinks (count effects so the test can assert) ---------- */
static int  g_irq_count = 0; static uint8_t g_last_irq = 0xFF;
static void irq_sink(void *ctx, uint8_t irq) { (void)ctx; g_irq_count++; g_last_irq = irq; }
static int  g_present_count = 0; static ntvdd_frame g_last_frame;
static void present_sink(void *ctx, const ntvdd_frame *f) { (void)ctx; g_present_count++; g_last_frame = *f; }

int main(void)
{
    static uint8_t flat[0x100000];      /* stand-in "guest memory" for map_flat */
    vdd_bus bus;
    fake_pit pit; memset(&pit, 0, sizeof pit);
    ntvdd pit_dev = { "fake-pit", pit_init, pit_reset, 0, &pit };
    ntvdd vid_dev = { "fake-vid", 0, 0, 0, 0 };
    uint32_t val; uint8_t b8; ntvdd_regs r;

    printf("== M3 slice-1 VDD bus battery ==\n");

    vdd_bus_init(&bus, flat);
    vdd_bus_set_sinks(&bus, irq_sink, 0, present_sink, 0);

    /* T0: add the PIT device -> its init claims ports/int/frame -------------- */
    CHECK(vdd_bus_add(&bus, &pit_dev) == 0, "add: fake-pit init ok");
    CHECK(bus.n_ports == 1 && bus.n_frame == 1, "add: one port range + one frame sub");
    CHECK(bus.ints[0x1A].svc != 0, "add: INT 1Ah claimed");

    /* memory window claimed directly (no device wrapper needed for the test) */
    CHECK(vdd_claim_mem(&bus, 0xB8000, sizeof(vram), mem_rd, mem_wr, &vid_dev) == 0,
          "claim: B8000 window");

    /* T1: I/O OUT then IN round-trips through the owner --------------------- */
    val = 0x12;
    CHECK(vdd_bus_io(&bus, 0x40, 1, 0, &val) == 1, "io: OUT 0x40 handled");
    CHECK(pit.reload == 0x12, "io: OUT stored device state");
    val = 0;
    CHECK(vdd_bus_io(&bus, 0x41, 1, 1, &val) == 1, "io: IN 0x41 handled");
    CHECK(val == 0x12 && pit.last_in_port == 0x41, "io: IN returned device state");

    /* T2: an unclaimed port is not owned ----------------------------------- */
    CHECK(vdd_bus_io(&bus, 0x3F8, 1, 1, &val) == 0, "io: unclaimed port 0x3F8 -> 0");

    /* T3: memory window read/write routes by offset ------------------------ */
    CHECK(vdd_bus_mem_write(&bus, 0xB8000 + 10, 0xAA) == 1, "mem: write into B8000 window");
    CHECK(vram[10] == 0xAA, "mem: write hit device offset 10");
    CHECK(vdd_bus_mem_read(&bus, 0xB8000 + 10, &b8) == 1 && b8 == 0xAA, "mem: read back");
    CHECK(vdd_bus_mem_write(&bus, 0xA0000, 0x55) == 0, "mem: outside window -> 0");

    /* T4: a claimed software interrupt is delivered with reg view ---------- */
    memset(&r, 0, sizeof r); pit.ticks = 0x00ABCDEF; s_ah(&r, 0x00);
    CHECK(vdd_bus_deliver_int(&bus, 0x1A, &r) == 1, "int: INT 1Ah delivered");
    CHECK(r_cx(&r) == 0x00AB && r_dx(&r) == 0xCDEF && r.cf == 0, "int: AH=0 returned tick CX:DX");
    CHECK(vdd_bus_deliver_int(&bus, 0x21, &r) == 0, "int: unclaimed INT 21h -> 0");

    /* T5: frame tick fans out -> device advances + raises IRQ0 via sink ----- */
    pit.ticks = 0; g_irq_count = 0;
    vdd_bus_frame(&bus);
    CHECK(pit.ticks == 1, "frame: tick advanced device");
    CHECK(g_irq_count == 1 && g_last_irq == 0, "frame: raised IRQ0 through sink");

    /* T6: map_flat resolves seg:off against the base ----------------------- */
    {
        uint8_t *q = (uint8_t *)vdd_map_flat(&bus, 0xB800, 0x000F);
        CHECK(q == flat + 0xB800F, "map_flat: seg:off -> base+linear");
    }

    /* T7: present routes a frame to the sink ------------------------------- */
    {
        static const uint8_t px[4] = {1,2,3,4};
        ntvdd_frame f; memset(&f, 0, sizeof f);
        f.w = 320; f.h = 200; f.bpp = 8; f.stride = 320; f.pixels = px;
        g_present_count = 0;
        vdd_present(&bus, &f);
        CHECK(g_present_count == 1 && g_last_frame.w == 320 && g_last_frame.bpp == 8,
              "present: frame routed to sink");
    }

    /* T8: reset fans out to devices --------------------------------------- */
    pit.reset_calls = 0; pit.reload = 0x99;
    vdd_bus_reset_all(&bus);
    CHECK(pit.reset_calls == 1 && pit.reload == 0, "reset: device reset called");

    /* T9: double-claiming an interrupt vector is refused ------------------- */
    CHECK(vdd_claim_int(&bus, 0x1A, pit_int1a, &pit) == -1, "claim: INT 1Ah double-claim refused");

    printf("\n%d checks, %d failed\n", total, fails);
    return fails ? 1 : 0;
}
