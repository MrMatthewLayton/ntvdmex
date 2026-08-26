/* ne_test.c -- off-VM battery for the NE loader (GH #128 / #4).
 *
 * Two halves, deliberately:
 *
 *  1. SYNTHETIC. A hand-built NE image covering every relocation shape the loader
 *     claims to handle, including the two that are easy to get wrong -- chained
 *     records and moveable (entry-ordinal) targets. These ALWAYS run, so a fresh
 *     clone gets real coverage with nothing to download.
 *
 *  2. REAL BINARIES, IF PRESENT. guest/ne/krnl386.exe and sysedit.exe are
 *     Microsoft's and are NOT in this repository, so these cases SKIP when absent
 *     rather than fail. The expected numbers come from tools/ne/nedump.py run
 *     against the real files -- see the commit that added it.
 *
 * Build+run via tools/dostest/run.sh.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/wow/ne.h"

static int pass, fail, skip;
static void ok(int c, const char *what)
{
    if (c) { ++pass; printf("  PASS  %s\n", what); }
    else   { ++fail; printf("  FAIL  %s\n", what); }
}
static void skipped(const char *what) { ++skip; printf("  SKIP  %s\n", what); }

/* ── build a synthetic NE in memory ─────────────────────────────────────────────── */
#define HDR 0x40
#define SEGTAB 0x40          /* relative to HDR */
#define ENTTAB 0x60
#define SECSHIFT 4

static uint8_t img[0x4000];

static void w16(uint32_t o, uint16_t v) { img[o] = (uint8_t)v; img[o+1] = (uint8_t)(v>>8); }
static void w32(uint32_t o, uint32_t v) { w16(o, (uint16_t)v); w16(o+2, (uint16_t)(v>>16)); }

/* Two segments. Seg1 = code with relocations, seg2 = data.
   Relocation records exercise: SEGMENT/chained, FARADDR/chained, OFFSET16/additive,
   and an INTERNALREF whose target is a MOVEABLE entry ordinal. */
static void build(void)
{
    uint32_t s1 = 0x200, s2 = 0x400, rel;
    memset(img, 0, sizeof img);
    img[0] = 'M'; img[1] = 'Z';
    w32(0x3C, HDR);
    img[HDR] = 'N'; img[HDR+1] = 'E';
    w16(HDR+0x04, ENTTAB);          /* entry table offset  */
    w16(HDR+0x06, 16);              /* entry table length  */
    w16(HDR+0x0C, 0x0001);          /* prog flags          */
    w16(HDR+0x0E, 2);               /* autodata = seg 2    */
    w32(HDR+0x14, (1u<<16) | 0x10); /* CS:IP = seg1:0x10   */
    w32(HDR+0x18, (2u<<16) | 0x00);
    w16(HDR+0x1C, 2);               /* 2 segments          */
    w16(HDR+0x22, SEGTAB);
    w16(HDR+0x30, 1);               /* 1 moveable entry    */
    w16(HDR+0x32, SECSHIFT);
    img[HDR+0x36] = 2;              /* target = Windows    */
    w16(HDR+0x3E, 0x030A);

    /* segment table: sector, length, flags, minalloc */
    w16(HDR+SEGTAB+0, (uint16_t)(s1 >> SECSHIFT)); w16(HDR+SEGTAB+2, 0x40);
    w16(HDR+SEGTAB+4, NE_SEG_RELOCS);              w16(HDR+SEGTAB+6, 0x80);
    w16(HDR+SEGTAB+8, (uint16_t)(s2 >> SECSHIFT)); w16(HDR+SEGTAB+10, 0x20);
    w16(HDR+SEGTAB+12, NE_SEG_DATA);               w16(HDR+SEGTAB+14, 0x20);

    /* entry table: one bundle, 1 moveable entry -> ordinal 1 = seg 2, offset 0x1234 */
    img[HDR+ENTTAB+0] = 1;      /* count      */
    img[HDR+ENTTAB+1] = 0xFF;   /* moveable   */
    img[HDR+ENTTAB+2] = 0;      /* flags      */
    img[HDR+ENTTAB+3] = 0xCD; img[HDR+ENTTAB+4] = 0x3F;   /* INT 3Fh thunk */
    img[HDR+ENTTAB+5] = 2;      /* segment 2  */
    w16(HDR+ENTTAB+6, 0x1234);  /* offset     */

    /* seg1 relocation table sits right after its 0x40 bytes of data */
    rel = s1 + 0x40;
    w16(rel, 4);                                   /* 4 records */

    /* (a) SEGMENT, chained: site 0x00 -> 0x02 -> end. target = seg 2 */
    img[rel+2+0] = NE_ADDR_SEGMENT; img[rel+2+1] = NE_REL_INTERNAL;
    w16(rel+2+2, 0x0000); w16(rel+2+4, 2); w16(rel+2+6, 0);
    w16(s1 + 0x00, 0x0002);      /* chain link -> next site 0x02 */
    w16(s1 + 0x02, 0xFFFF);      /* end of chain                 */

    /* (b) FARADDR, single: site 0x10, target = seg 2 : 0x0040 */
    img[rel+10+0] = NE_ADDR_FARADDR; img[rel+10+1] = NE_REL_INTERNAL;
    w16(rel+10+2, 0x0010); w16(rel+10+4, 2); w16(rel+10+6, 0x0040);
    w16(s1 + 0x10, 0xFFFF);

    /* (c) OFFSET16, ADDITIVE (must NOT walk a chain): site 0x20 */
    img[rel+18+0] = NE_ADDR_OFFSET16;
    img[rel+18+1] = NE_REL_INTERNAL | NE_REL_ADDITIVE;
    w16(rel+18+2, 0x0020); w16(rel+18+4, 2); w16(rel+18+6, 0x00AA);
    w16(s1 + 0x20, 0x0030);      /* looks like a chain link; must be ignored */
    w16(s1 + 0x30, 0xBEEF);      /* sentinel: must survive untouched          */

    /* (d) INTERNALREF to a MOVEABLE target: a=0xFF, b=ordinal 1 */
    img[rel+26+0] = NE_ADDR_FARADDR; img[rel+26+1] = NE_REL_INTERNAL;
    w16(rel+26+2, 0x0028); w16(rel+26+4, 0x00FF); w16(rel+26+6, 1);
    w16(s1 + 0x28, 0xFFFF);
}

static uint8_t segmem[2][0x200];

int main(void)
{
    ne_module m;
    printf("== WOW: NE loader battery (GH #128/#4) ==\n");

    build();
    ok(ne_parse(&m, img, sizeof img) == 0, "synthetic image parses");
    ok(m.n_seg == 2, "2 segments");
    ok(m.align_shift == SECSHIFT, "align shift honoured");
    ok(m.autodata == 2, "autodata segment = 2");
    ok((m.csip >> 16) == 1 && (m.csip & 0xFFFF) == 0x10, "CS:IP = seg1:0x0010");
    ok(m.target_os == 2, "target OS = Windows");
    ok(m.seg[0].file_off == 0x200, "seg1 file offset from sector<<shift");
    ok(ne_seg_alloc_size(&m.seg[0]) == 0x80, "seg1 alloc size uses minalloc, not length");

    /* entry table */
    {   uint16_t sn = 0, off = 0;
        ok(ne_entry_lookup(&m, 1, &sn, &off) == 0 && sn == 2 && off == 0x1234,
           "moveable entry ordinal 1 -> seg 2:0x1234");
        ok(ne_entry_lookup(&m, 2, &sn, &off) != 0, "ordinal past the end is rejected");
        ok(ne_entry_lookup(&m, 0, &sn, &off) != 0, "ordinal 0 is rejected");
    }

    /* load + relocate */
    memcpy(segmem[0], img + m.seg[0].file_off, m.seg[0].length);
    memcpy(segmem[1], img + m.seg[1].file_off, m.seg[1].length);
    m.seg[0].mem = segmem[0]; m.seg[0].seg = 0x1000;
    m.seg[1].mem = segmem[1]; m.seg[1].seg = 0x2000;

    ok(ne_apply_relocs(&m, 0, NULL, NULL) == 0, "relocations apply");
    /* 4 records, one of which is a 2-site chain -> 5 sites. "Success" with 0 sites
       patched would otherwise be indistinguishable from success. */
    ok(m.sites == 5, "5 sites patched (4 records, one a 2-link chain)");
    ok(ne_rd16(segmem[0] + 0x00) == 0x2000, "chained SEGMENT: first site patched");
    ok(ne_rd16(segmem[0] + 0x02) == 0x2000, "chained SEGMENT: SECOND site patched too");
    ok(ne_rd16(segmem[0] + 0x10) == 0x0040 && ne_rd16(segmem[0] + 0x12) == 0x2000,
       "FARADDR writes off then seg");
    ok(ne_rd16(segmem[0] + 0x20) == 0x00AA, "ADDITIVE OFFSET16 patched");
    ok(ne_rd16(segmem[0] + 0x30) == 0xBEEF,
       "ADDITIVE record did NOT follow the value as a chain link");
    ok(ne_rd16(segmem[0] + 0x28) == 0x1234 && ne_rd16(segmem[0] + 0x2A) == 0x2000,
       "moveable INTERNALREF resolved through the entry table");

    /* an unresolvable import must FAIL, not quietly leave a far call to nowhere */
    {   ne_module m2; uint8_t tmp[0x200];
        build();
        img[0x200 + 0x40 + 2 + 1] = NE_REL_IMPORTORD;   /* record (a) -> import */
        ne_parse(&m2, img, sizeof img);
        memcpy(tmp, img + m2.seg[0].file_off, m2.seg[0].length);
        m2.seg[0].mem = tmp; m2.seg[0].seg = 0x1000;
        m2.seg[1].mem = segmem[1]; m2.seg[1].seg = 0x2000;
        ok(ne_apply_relocs(&m2, 0, NULL, NULL) != 0,
           "unresolved IMPORTORDINAL is refused loudly");
        ok(m2.err != 0, "...and records where it gave up");
    }

    /* malformed input */
    {   ne_module bad; uint8_t junk[64];
        memset(junk, 0, sizeof junk);
        ok(ne_parse(&bad, junk, sizeof junk) != 0, "no MZ -> rejected");
        junk[0] = 'M'; junk[1] = 'Z';
        ok(ne_parse(&bad, junk, sizeof junk) != 0, "MZ but no NE -> rejected");
    }

    /* ── real binaries, if the user supplied them ─────────────────────────────── */
    {
        static const struct { const char *path, *name; int segs, movable; } REAL[] = {
            { "guest/ne/krnl386.exe", "krnl386", 4, 164 },
            { "guest/ne/sysedit.exe", "sysedit", 6, 21 },
        };
        size_t k;
        for (k = 0; k < sizeof REAL / sizeof REAL[0]; ++k) {
            /* run.sh may invoke us from the repo root or from tools/dostest --
               try both rather than silently SKIPping the most valuable cases. */
            FILE *f = fopen(REAL[k].path, "rb");
            uint8_t *buf; long n; ne_module r;
            if (!f) {
                char alt[256];
                snprintf(alt, sizeof alt, "../../%s", REAL[k].path);
                f = fopen(alt, "rb");
            }
            if (!f) { skipped(REAL[k].name); continue; }
            fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
            buf = malloc((size_t)n);
            if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
                fclose(f); free(buf); skipped(REAL[k].name); continue;
            }
            fclose(f);
            ok(ne_parse(&r, buf, (uint32_t)n) == 0, REAL[k].name);
            ok(r.n_seg == REAL[k].segs, "  segment count matches nedump");
            ok(r.n_movable == REAL[k].movable, "  moveable entry count matches nedump");
            ok(r.target_os == 2, "  targets Windows");
            if (!strcmp(REAL[k].name, "krnl386")) {
                /* The measured fact that makes this the cheap first milestone. */
                ok(r.n_mod == 0, "  krnl386 imports from NOTHING");
                ok((r.csip >> 16) == 1 && (r.csip & 0xFFFF) == 0xc02b,
                   "  krnl386 CS:IP = seg1:0xc02b");
                ok(r.align_shift == 4, "  krnl386 align shift 4");
            } else {
                ok(r.n_mod == 4, "  sysedit imports from 4 modules");
            }
            free(buf);
        }
    }

    printf("\n%d checks, %d failed, %d skipped\n", pass + fail, fail, skip);
    return fail ? 1 : 0;
}
