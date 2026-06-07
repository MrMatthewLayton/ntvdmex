/*
 * vdd_bus.h -- the device bus behind the ntvdd.h ABI.  (M3, ADR-0008)
 *
 * The bus is a registry: VDDs claim port ranges / memory windows / interrupt
 * vectors / frame ticks during init, and the host calls the vdd_bus_* dispatch
 * entry points from its V86 service loop when the matching hardware event fires.
 *
 * The bus is host-agnostic and pure (no <windows.h>): the two effects that need
 * the real host -- raising an IRQ into the kernel ICA and presenting a frame to
 * the screen -- are injected as `sink` callbacks (NULL in the off-VM test). This
 * is what lets vdd_test.c exercise the whole bus + a VDD with no VM.
 */
#ifndef NTVDMEX_VDD_BUS_H
#define NTVDMEX_VDD_BUS_H

#include "ntvdd.h"

#ifndef VDD_MAX_PORTS
#define VDD_MAX_PORTS  16       /* claimed port ranges                          */
#endif
#ifndef VDD_MAX_MEM
#define VDD_MAX_MEM    8        /* claimed memory windows                       */
#endif
#ifndef VDD_MAX_FRAME
#define VDD_MAX_FRAME  8        /* frame-tick subscribers                       */
#endif
#ifndef VDD_MAX_DEV
#define VDD_MAX_DEV    16       /* devices on the bus                           */
#endif

typedef struct { uint16_t lo, hi; ntvdd_in_fn in; ntvdd_out_fn out; void *self; } vdd_port_ent;
typedef struct { uint32_t base, end; ntvdd_rd_fn rd; ntvdd_wr_fn wr; void *self; } vdd_mem_ent;
typedef struct { ntvdd_int_fn svc; void *self; } vdd_int_ent;
typedef struct { ntvdd_frame_fn fn; void *self; } vdd_frame_ent;

/* host-injected effects */
typedef void (*vdd_irq_sink)(void *ctx, uint8_t irq);
typedef void (*vdd_present_sink)(void *ctx, const ntvdd_frame *f);

struct vdd_bus {
    void          *mem_base;            /* map_flat base; NULL => V86 absolute  */
    vdd_irq_sink   irq_sink;     void *irq_ctx;
    vdd_present_sink present_sink; void *present_ctx;

    vdd_port_ent   ports[VDD_MAX_PORTS];   int n_ports;
    vdd_mem_ent    mem[VDD_MAX_MEM];       int n_mem;
    vdd_int_ent    ints[256];              /* indexed by vector; svc==NULL=unset */
    vdd_frame_ent  frame[VDD_MAX_FRAME];   int n_frame;

    ntvdd         *dev[VDD_MAX_DEV];       int n_dev;
};

/* --- host-side lifecycle + dispatch (the V86 loop calls these) ------------- */
void vdd_bus_init(vdd_bus *b, void *mem_base);
void vdd_bus_set_sinks(vdd_bus *b, vdd_irq_sink irq, void *irq_ctx,
                       vdd_present_sink present, void *present_ctx);
int  vdd_bus_add(vdd_bus *b, ntvdd *dev);     /* runs dev->init; 0 = ok        */
void vdd_bus_reset_all(vdd_bus *b);
void vdd_bus_shutdown_all(vdd_bus *b);

/* dispatch -- each returns 1 if a VDD owned the event, 0 if unclaimed */
int  vdd_bus_io  (vdd_bus *b, uint16_t port, uint8_t width, int is_in, uint32_t *val);
int  vdd_bus_mem_read (vdd_bus *b, uint32_t addr, uint8_t *out);
int  vdd_bus_mem_write(vdd_bus *b, uint32_t addr, uint8_t v);
int  vdd_bus_deliver_int(vdd_bus *b, uint8_t vec, ntvdd_regs *r);
void vdd_bus_frame(vdd_bus *b);               /* fan out the ~60 Hz tick       */

#endif /* NTVDMEX_VDD_BUS_H */
