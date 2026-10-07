/* interp_side.c -- ONE copy of src/host/v86interp.h, wrapped so that two copies (two
 * different versions of the header) can be linked into one binary. GH #194.
 *
 * Compiled twice by scripts/interpfuzz.sh (MODE=superset):
 *     -DSIDE=ref_ -DINTERP_H="<HEAD's header>"     and     -DSIDE=new_  (working tree)
 * Everything in the header is `static`, so each object has its own interpreter, its own
 * guest memory and its own hooks; the only exported names are the SIDE-prefixed ones
 * below. tests/unit/interp_superset.c drives both in lockstep.
 */
#include <stdint.h>
#include <string.h>
#include "interp_xcpu.h"

typedef unsigned char BYTE;

#ifndef SIDE
#error "define SIDE (ref_ or new_)"
#endif
#define CAT2(a, b) a##b
#define CAT(a, b) CAT2(a, b)
#define FN(n) CAT(SIDE, n)

static BYTE MEM[XMEM_SIZE];
static uint64_t g_fx;                                 /* this step's effects, hashed */
static void fx(uint64_t v)
{ int i; for (i = 0; i < 8; ++i) { g_fx ^= (BYTE)(v >> (i * 8)); g_fx *= 1099511628211ULL; } }

/* An undo log, so a step the OTHER side did not take can be rolled back. */
#define UNDO_MAX (1u << 20)
static uint32_t g_ulin[UNDO_MAX]; static BYTE g_uold[UNDO_MAX];
static uint32_t g_un; static int g_uon, g_uover;

static uint8_t V86HostRead8(uint32_t lin) { return (lin < sizeof MEM) ? MEM[lin] : 0xFF; }
static void V86HostWrite8(uint32_t lin, uint8_t v)
{
    fx(((uint64_t)lin << 8) | v);
    if (lin >= sizeof MEM) return;
    if (g_uon) { if (g_un < UNDO_MAX) { g_ulin[g_un] = lin; g_uold[g_un] = MEM[lin]; ++g_un; } else g_uover = 1; }
    MEM[lin] = v;
}
static uint32_t V86HostIn(uint16_t port, int width)
{ fx(0x1000000ULL | port | ((uint64_t)width << 16)); return (uint32_t)port * 2654435761u; }
static void V86HostOut(uint16_t port, int width, uint32_t val)
{ fx(0x2000000ULL | port | ((uint64_t)width << 16) | ((uint64_t)val << 32)); }

#ifndef INTERP_H
#define INTERP_H "../../src/host/v86interp.h"
#endif
#include INTERP_H

/* A FAKE descriptor table for the protected-mode run -- identical on both sides, so any
   difference is the interpreter's. Index mod 4: 16-bit code / data / 32-bit code /
   not present; limits alternate 64 KB and 32 KB; bases are spread over the low 1 MB. */
static uint32_t fake_seg2lin(uint16_t sel) { return ((uint32_t)(sel >> 3) * 0x1230u) & 0xFFFF0u; }
static int fake_sel_desc(uint16_t sel, uint32_t *ar, uint32_t *limit)
{
    unsigned idx = sel >> 3, k = idx & 3;
    if (!(sel & 4) || idx == 0 || k == 3) return 0;
    *ar = (k == 1 ? 0xF2u : 0xFAu) << 8;
    if (k == 2) *ar |= 0x4u << 20;                    /* D = 1 */
    *limit = (idx & 4) ? 0x7FFFu : 0xFFFFu;
    return 1;
}

void FN(init)(const uint8_t *img, int pm)
{
    memcpy(MEM, img, sizeof MEM);
    g_V86SegmentToLinear  = pm ? fake_seg2lin : 0;
    g_V86SelectorDescriptor = pm ? fake_sel_desc : 0;
}
void FN(poke)(uint32_t lin, uint8_t v) { if (lin < sizeof MEM) MEM[lin] = v; }
const uint8_t *FN(mem)(void) { return MEM; }
void FN(sync)(const uint8_t *img) { memcpy(MEM, img, sizeof MEM); }

int FN(step)(xcpu *x, uint64_t *effects)
{
    V86_CPU c; int ok, i;
    for (i = 0; i < 8; ++i) c.Registers[i] = x->r[i];
    for (i = 0; i < 6; ++i) c.Segments[i] = x->seg[i];
    c.Ip = x->ip; c.Flags = x->flags;
    g_fx = 1469598103934665603ULL;
    ok = V86Step(&c);
    for (i = 0; i < 8; ++i) x->r[i] = c.Registers[i];
    for (i = 0; i < 6; ++i) x->seg[i] = c.Segments[i];
    x->ip = c.Ip; x->flags = c.Flags;
    *effects = g_fx;
    return ok;
}

void FN(undo_begin)(void) { g_un = 0; g_uover = 0; g_uon = 1; }
/* Roll back every write since undo_begin. Returns 0 if the log overflowed (the caller
   must then resync the whole image). */
int FN(undo_rollback)(void)
{
    uint32_t i = g_un;
    g_uon = 0;
    while (i) { --i; MEM[g_ulin[i]] = g_uold[i]; }
    return !g_uover;
}
void FN(undo_end)(void) { g_uon = 0; }
