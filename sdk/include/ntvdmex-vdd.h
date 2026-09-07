/*
 * ntvdmex-vdd.h -- the public SDK header for third-party NTVDMEX devices.
 * GH #11, ADR-0008 ("clean internal plugin ABI").
 *
 * A VDD is a DLL that claims some hardware -- a range of I/O ports, a window of
 * guest memory, an interrupt vector, a per-frame tick -- and is called back when
 * the guest touches it. That is the whole model.
 *
 * ── WHY A FUNCTION TABLE AND NOT IMPORTS ────────────────────────────────────
 * Microsoft's VDD ABI has third-party DLLs IMPORT their services (VDDInstallIOHook
 * and friends) from `NTVDM.EXE` by name, which binds a driver to the filename of
 * the process hosting it. Ours does not: the host hands you a table of function
 * pointers at init. A VDD built against this header therefore has NO import from
 * the host at all, which means
 *   - it loads under a host named anything (ours is ntvdmhost.exe, not ntvdm.exe),
 *   - it can be compiled and unit-tested with a stub table and no VM,
 *   - and the ABI can grow without breaking a built driver: `size` and `version`
 *     are checked, and new members are only ever APPENDED.
 * The MS-compatible `vddsvc.h` veneer of ADR-0008 is a separate, later thing that
 * layers over the same bus; it is not this.
 *
 * ── WHAT A VDD LOOKS LIKE ───────────────────────────────────────────────────
 *     #include <ntvdmex-vdd.h>
 *     static void my_in (void *self, uint16_t port, uint8_t w, uint32_t *val);
 *     static void my_out(void *self, uint16_t port, uint8_t w, uint32_t  val);
 *
 *     NTVDMEX_VDD_EXPORT int NtvdmexVddInit(const ntvdmex_vdd_api *api,
 *                                           ntvdmex_vdd_bus *bus)
 *     {
 *         if (api->version != NTVDMEX_VDD_ABI_VERSION) return -1;
 *         return api->claim_ports(bus, 0x2E0, 0x2E7, my_in, my_out, &my_state);
 *     }
 *
 * See sdk/sample/portecho.c for a complete one, and docs/sdk/vdd-sdk.md for the
 * rules that are NOT expressible in a header (threading, what you may block on).
 */
#ifndef NTVDMEX_VDD_SDK_H
#define NTVDMEX_VDD_SDK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bump ONLY when an existing member changes meaning. Appending does not. */
#define NTVDMEX_VDD_ABI_VERSION 1u

#if defined(_WIN32)
#  define NTVDMEX_VDD_EXPORT __declspec(dllexport)
#else
#  define NTVDMEX_VDD_EXPORT
#endif

/* The bus is OPAQUE. It is the host's registry of who owns what; a VDD only ever
   passes the pointer back to the calls below. Making it opaque is deliberate --
   the internal layout has changed twice (VDD_MAX_PORTS 16 -> 32 among them) and
   a driver compiled against the old shape must not care. */
typedef struct ntvdmex_vdd_bus ntvdmex_vdd_bus;

/* ── CALLBACK SIGNATURES ─────────────────────────────────────────────────────
   These MUST match src/vdd/ntvdd.h, which is the in-tree source of truth; the
   SDK build asserts it (see sdk/sample/abi_check.c). `self` is whatever you
   handed the claim call, so one DLL can serve several instances.
 ⚠ `width` is 1, 2 or 4 BYTES, not bits. A guest may read a 16-bit port with one
   `in ax,dx`, and a device that assumes 1 silently truncates. */
typedef void (*ntvdmex_in_fn)   (void *self, uint16_t port, uint8_t width, uint32_t *val);
typedef void (*ntvdmex_out_fn)  (void *self, uint16_t port, uint8_t width, uint32_t  val);
typedef uint8_t (*ntvdmex_rd_fn)(void *self, uint32_t off);    /* offset WITHIN your window */
typedef void    (*ntvdmex_wr_fn)(void *self, uint32_t off, uint8_t v);
typedef void (*ntvdmex_frame_fn)(void *self);

/* The guest's registers, for an interrupt service. Deliberately a STRUCT PASSED
   IN rather than the global get/set macros MS uses: it is what keeps a device
   testable with no VM, and it is the one place ADR-0008 says we diverge on
   purpose. `cf` is the carry flag the guest will see on return -- DOS reports
   failure in it, and a service that forgets it reports success. */
typedef struct ntvdmex_regs {
    uint32_t eax, ebx, ecx, edx, esi, edi, ebp;
    uint16_t ds, es;
    uint8_t  cf;
} ntvdmex_regs;

typedef void (*ntvdmex_int_fn)(void *self, ntvdmex_regs *r);

/* ── THE HOST'S SERVICES ─────────────────────────────────────────────────────
   Handed to NtvdmexVddInit. Check `version` before using anything; check `size`
   before reading a member added after version 1.
 ⚠ EVERY claim_* RETURNS A STATUS AND YOU MUST READ IT. The host's table is
   finite, and a refused claim is why a device is silently absent -- the guest
   then reads 0xFF from an empty slot and blames itself. This cost the project a
   whole investigation once (the MPU-401, when VDD_MAX_PORTS was exactly full),
   which is why the host now logs refusals loudly and why this note is here. */
typedef struct ntvdmex_vdd_api {
    uint32_t size;                  /* sizeof(ntvdmex_vdd_api) as the host built it */
    uint32_t version;               /* NTVDMEX_VDD_ABI_VERSION                      */

    /* Claim hardware. All ranges INCLUSIVE. 0 = ok, negative = refused. */
    int  (*claim_ports)(ntvdmex_vdd_bus *b, uint16_t lo, uint16_t hi,
                        ntvdmex_in_fn in, ntvdmex_out_fn out, void *self);
    int  (*claim_mem)  (ntvdmex_vdd_bus *b, uint32_t base, uint32_t size,
                        ntvdmex_rd_fn rd, ntvdmex_wr_fn wr, void *self);
    int  (*claim_int)  (ntvdmex_vdd_bus *b, uint8_t vec,
                        ntvdmex_int_fn svc, void *self);
    int  (*on_frame)   (ntvdmex_vdd_bus *b, ntvdmex_frame_fn fn, void *self);

    /* Services you may call back into. */
    void (*raise_irq)  (ntvdmex_vdd_bus *b, uint8_t irq);
    /* Guest seg:off -> a host pointer you can read and write. NULL if it does
       not resolve; check it, because a guest is free to pass you nonsense. */
    void *(*map_flat)  (ntvdmex_vdd_bus *b, uint16_t seg, uint16_t off);
    /* A PHYSICAL linear address. seg:off cannot express one -- ISA DMA
       addresses memory as page<<16 | offset -- so this is the form a
       DMA-driving device needs. */
    void *(*map_lin)   (ntvdmex_vdd_bus *b, uint32_t linear);
    /* Write a line into the host's trace. Use it: a device that fails quietly
       is indistinguishable from one that was never loaded. */
    void (*log)        (const char *msg);
} ntvdmex_vdd_api;

/* ── THE ENTRY POINT ─────────────────────────────────────────────────────────
   Exported by name, undecorated. Return 0 to stay loaded; anything else and the
   host reports the failure and unloads you -- which is better than a half-armed
   device the guest can reach.
 ⚠ CLAIM EVERYTHING HERE AND NOWHERE ELSE. Init is the only moment the host
   guarantees the bus is quiet; a claim from inside a callback races the
   dispatcher that is calling you. */
typedef int (*NtvdmexVddInitFn)(const ntvdmex_vdd_api *api, ntvdmex_vdd_bus *bus);
#define NTVDMEX_VDD_INIT_NAME "NtvdmexVddInit"

#ifdef __cplusplus
}
#endif
#endif /* NTVDMEX_VDD_SDK_H */
