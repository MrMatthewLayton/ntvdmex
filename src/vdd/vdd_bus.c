/* vdd_bus.c -- see vdd_bus.h / ntvdd.h.  The device bus implementation: the
 * claim registries (called by VDDs during init) and the dispatch entry points
 * (called by the host's V86 service loop).  Pure C, no <windows.h>. */
#include "vdd_bus.h"

/* --- lifecycle ------------------------------------------------------------ */
void vdd_bus_init(vdd_bus *b, void *mem_base)
{
    int i;
    b->mem_base = mem_base;
    b->irq_sink = 0; b->irq_ctx = 0;
    b->present_sink = 0; b->present_ctx = 0;
    b->n_ports = 0; b->n_mem = 0; b->n_frame = 0; b->n_dev = 0;
    for (i = 0; i < 256; ++i) { b->ints[i].svc = 0; b->ints[i].self = 0; }
}

void vdd_bus_set_sinks(vdd_bus *b, vdd_irq_sink irq, void *irq_ctx,
                       vdd_present_sink present, void *present_ctx)
{
    b->irq_sink = irq; b->irq_ctx = irq_ctx;
    b->present_sink = present; b->present_ctx = present_ctx;
}

int vdd_bus_add(vdd_bus *b, ntvdd *dev)
{
    if (b->n_dev >= VDD_MAX_DEV) return -1;
    b->dev[b->n_dev++] = dev;
    return dev->init ? dev->init(b, dev->self) : 0;
}

void vdd_bus_reset_all(vdd_bus *b)
{
    int i;
    for (i = 0; i < b->n_dev; ++i)
        if (b->dev[i]->reset) b->dev[i]->reset(b->dev[i]->self);
}

void vdd_bus_shutdown_all(vdd_bus *b)
{
    int i;
    for (i = 0; i < b->n_dev; ++i)
        if (b->dev[i]->shutdown) b->dev[i]->shutdown(b->dev[i]->self);
}

/* --- claim registries (the ntvdd.h ABI surface VDDs call) ----------------- */
int vdd_claim_ports(vdd_bus *b, uint16_t lo, uint16_t hi,
                    ntvdd_in_fn in, ntvdd_out_fn out, void *self)
{
    vdd_port_ent *e;
    if (lo > hi || b->n_ports >= VDD_MAX_PORTS) return -1;
    e = &b->ports[b->n_ports++];
    e->lo = lo; e->hi = hi; e->in = in; e->out = out; e->self = self;
    return 0;
}

int vdd_claim_mem(vdd_bus *b, uint32_t base, uint32_t size,
                  ntvdd_rd_fn rd, ntvdd_wr_fn wr, void *self)
{
    vdd_mem_ent *e;
    if (!size || b->n_mem >= VDD_MAX_MEM) return -1;
    e = &b->mem[b->n_mem++];
    e->base = base; e->end = base + size - 1; e->rd = rd; e->wr = wr; e->self = self;
    return 0;
}

int vdd_claim_int(vdd_bus *b, uint8_t vec, ntvdd_int_fn svc, void *self)
{
    if (b->ints[vec].svc) return -1;        /* already claimed */
    b->ints[vec].svc = svc; b->ints[vec].self = self;
    return 0;
}

int vdd_on_frame(vdd_bus *b, ntvdd_frame_fn fn, void *self)
{
    vdd_frame_ent *e;
    if (b->n_frame >= VDD_MAX_FRAME) return -1;
    e = &b->frame[b->n_frame++];
    e->fn = fn; e->self = self;
    return 0;
}

/* --- services VDDs call back into ----------------------------------------- */
void vdd_raise_irq(vdd_bus *b, uint8_t irq)
{
    if (b->irq_sink) b->irq_sink(b->irq_ctx, irq);
}

void *vdd_map_flat(vdd_bus *b, uint16_t seg, uint16_t off)
{
    uint32_t flat = ((uint32_t)seg << 4) + off;     /* real-mode linear address */
    return (uint8_t *)b->mem_base + flat;           /* base==NULL => absolute   */
}

void *vdd_map_lin(vdd_bus *b, uint32_t linear)
{
    return (uint8_t *)b->mem_base + linear;         /* base==NULL => absolute   */
}

void vdd_present(vdd_bus *b, const ntvdd_frame *f)
{
    if (b->present_sink) b->present_sink(b->present_ctx, f);
}

/* --- dispatch (the host's V86 service loop calls these) ------------------- */
int vdd_bus_io(vdd_bus *b, uint16_t port, uint8_t width, int is_in, uint32_t *val)
{
    int i;
    for (i = 0; i < b->n_ports; ++i) {
        vdd_port_ent *e = &b->ports[i];
        if (port < e->lo || port > e->hi) continue;
        if (is_in)  { if (e->in)  { *val = 0; e->in(e->self, port, width, val); } else *val = 0xFFFFFFFFu; }
        else        { if (e->out) e->out(e->self, port, width, *val); }
        return 1;
    }
    return 0;
}

int vdd_bus_mem_read(vdd_bus *b, uint32_t addr, uint8_t *out)
{
    int i;
    for (i = 0; i < b->n_mem; ++i) {
        vdd_mem_ent *e = &b->mem[i];
        if (addr < e->base || addr > e->end) continue;
        *out = e->rd ? e->rd(e->self, addr - e->base) : 0xFF;
        return 1;
    }
    return 0;
}

int vdd_bus_mem_write(vdd_bus *b, uint32_t addr, uint8_t v)
{
    int i;
    for (i = 0; i < b->n_mem; ++i) {
        vdd_mem_ent *e = &b->mem[i];
        if (addr < e->base || addr > e->end) continue;
        if (e->wr) e->wr(e->self, addr - e->base, v);
        return 1;
    }
    return 0;
}

int vdd_bus_deliver_int(vdd_bus *b, uint8_t vec, ntvdd_regs *r)
{
    if (!b->ints[vec].svc) return 0;
    b->ints[vec].svc(b->ints[vec].self, r);
    return 1;
}

void vdd_bus_frame(vdd_bus *b)
{
    int i;
    for (i = 0; i < b->n_frame; ++i)
        if (b->frame[i].fn) b->frame[i].fn(b->frame[i].self);
}
