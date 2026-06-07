/*
 * ntvdd.h -- NTVDMEX clean Virtual Device Driver (VDD) ABI.  (M3, ADR-0008)
 *
 * Every device -- video, the PIT timer, sound, input -- is a `ntvdd` plugged
 * into one `vdd_bus`.  The host core owns the V86 CPU loop and the DOS kernel;
 * it knows nothing device-specific.  Instead it drives the bus, which routes
 * hardware events to whichever VDD claimed them:
 *
 *     IN/OUT on a claimed port   -> the port owner's in/out callback
 *     access in a claimed window -> the memory owner's rd/wr callback
 *     a claimed software INT      -> the interrupt owner's service callback
 *     ~60 Hz frame tick           -> every on_frame subscriber
 *
 * and the bus offers services back to a VDD: raise an emulated IRQ, translate a
 * guest seg:off to a flat host pointer, and present a finished framebuffer.
 *
 * Design rule (ADR-0008): register access is EXPLICIT, through `ntvdd_regs`
 * (a plain view, not global get/set macros), so a VDD is unit-testable off-VM
 * -- the same discipline that made dos_mcb.h testable without a VM.
 *
 * This header is freestanding (no <windows.h>); it is shared verbatim by the
 * XP host build and the off-VM test battery (tools/dostest/vdd_test.c).
 */
#ifndef NTVDMEX_NTVDD_H
#define NTVDMEX_NTVDD_H

#include <stdint.h>

/* --- guest register view handed to interrupt-service callbacks ------------- */
/* A flat snapshot of the V86 registers a device cares about.  On the real host
   the bus loads this from the VDM_TIB CONTEXT before the call and stores it back
   after; off-VM a test fills it directly.  `cf` carries the carry flag out (DOS
   error convention); other flag bits are not modelled here. */
typedef struct ntvdd_regs {
    uint32_t eax, ebx, ecx, edx, esi, edi, ebp;
    uint16_t ds, es;
    uint8_t  cf;            /* carry flag: read+write by the handler            */
    uint8_t  zf;            /* zero flag: e.g. INT 16h AH=01 "key available"    */
} ntvdd_regs;

/* 8/16-bit sub-register accessors (keep call sites readable). */
static inline uint8_t  r_al(const ntvdd_regs *r){ return (uint8_t)(r->eax); }
static inline uint8_t  r_ah(const ntvdd_regs *r){ return (uint8_t)(r->eax >> 8); }
static inline uint16_t r_ax(const ntvdd_regs *r){ return (uint16_t)(r->eax); }
static inline uint16_t r_bx(const ntvdd_regs *r){ return (uint16_t)(r->ebx); }
static inline uint16_t r_cx(const ntvdd_regs *r){ return (uint16_t)(r->ecx); }
static inline uint16_t r_dx(const ntvdd_regs *r){ return (uint16_t)(r->edx); }
static inline void s_al(ntvdd_regs *r, uint8_t v){ r->eax = (r->eax & 0xFFFFFF00u) | v; }
static inline void s_ah(ntvdd_regs *r, uint8_t v){ r->eax = (r->eax & 0xFFFF00FFu) | ((uint32_t)v << 8); }
static inline void s_ax(ntvdd_regs *r, uint16_t v){ r->eax = (r->eax & 0xFFFF0000u) | v; }
static inline void s_bx(ntvdd_regs *r, uint16_t v){ r->ebx = (r->ebx & 0xFFFF0000u) | v; }
static inline void s_cx(ntvdd_regs *r, uint16_t v){ r->ecx = (r->ecx & 0xFFFF0000u) | v; }
static inline void s_dx(ntvdd_regs *r, uint16_t v){ r->edx = (r->edx & 0xFFFF0000u) | v; }

/* --- a finished frame the video VDD hands to the presentation layer -------- */
typedef struct ntvdd_frame {
    uint16_t        w, h;           /* logical resolution in pixels             */
    uint8_t         bpp;            /* 8 (palettised) or 32 (ARGB)              */
    uint32_t        stride;         /* bytes per scanline of `pixels`           */
    const uint8_t  *pixels;         /* framebuffer (indices if bpp==8)          */
    const uint32_t *palette;        /* 256 ARGB entries (bpp==8 only; else NULL)*/
} ntvdd_frame;

/* --- device-supplied callback signatures ---------------------------------- */
typedef void (*ntvdd_in_fn)  (void *self, uint16_t port, uint8_t width, uint32_t *val);
typedef void (*ntvdd_out_fn) (void *self, uint16_t port, uint8_t width, uint32_t  val);
typedef uint8_t (*ntvdd_rd_fn)(void *self, uint32_t off);   /* off within window */
typedef void    (*ntvdd_wr_fn)(void *self, uint32_t off, uint8_t v);
typedef void (*ntvdd_int_fn) (void *self, ntvdd_regs *r);
typedef void (*ntvdd_frame_fn)(void *self);

/* --- the bus (opaque to VDDs except through these calls) ------------------- */
typedef struct vdd_bus vdd_bus;

/* claim_*: a VDD registers interest during init().  All ranges inclusive. */
int  vdd_claim_ports(vdd_bus *b, uint16_t lo, uint16_t hi,
                     ntvdd_in_fn in, ntvdd_out_fn out, void *self);
int  vdd_claim_mem  (vdd_bus *b, uint32_t base, uint32_t size,
                     ntvdd_rd_fn rd, ntvdd_wr_fn wr, void *self);
int  vdd_claim_int  (vdd_bus *b, uint8_t vec, ntvdd_int_fn svc, void *self);
int  vdd_on_frame   (vdd_bus *b, ntvdd_frame_fn fn, void *self);

/* services a VDD may call back into */
void  vdd_raise_irq (vdd_bus *b, uint8_t irq);
void *vdd_map_flat  (vdd_bus *b, uint16_t seg, uint16_t off);
void  vdd_present    (vdd_bus *b, const ntvdd_frame *f);

/* --- a device descriptor -------------------------------------------------- */
typedef struct ntvdd {
    const char *name;
    int  (*init)(vdd_bus *b, void *self);   /* claim hooks; 0 = ok             */
    void (*reset)(void *self);              /* DOS process start / mode reset  */
    void (*shutdown)(void *self);
    void *self;                             /* device instance state           */
} ntvdd;

#endif /* NTVDMEX_NTVDD_H */
