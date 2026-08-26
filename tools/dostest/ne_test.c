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

/* ── a synthetic LIBRARY to import from ─────────────────────────────────────────────
     Exports ordinal 1 (fixed, exported), 2 (moveable, exported, reachable by the name
     BAR from the NON-resident table) and 3 (present but NOT exported). Its own name is
     TESTLIB, which is what an importer refers to it by. */
static uint8_t limg[0x1000];
static uint8_t lsegmem[0x200];

#define LRES  0x100          /* resident names,  relative to HDR */
#define LNRES 0x300          /* non-resident,    ABSOLUTE        */

static void lw16(uint32_t o, uint16_t v) { limg[o] = (uint8_t)v; limg[o+1] = (uint8_t)(v>>8); }
static void lw32(uint32_t o, uint32_t v) { lw16(o, (uint16_t)v); lw16(o+2, (uint16_t)(v>>16)); }
static uint32_t lpstr(uint32_t o, const char *s, uint16_t ord)
{
    uint32_t n = (uint32_t)strlen(s), i;
    limg[o] = (uint8_t)n;
    for (i = 0; i < n; ++i) limg[o+1+i] = (uint8_t)s[i];
    lw16(o + 1 + n, ord);
    return o + 1 + n + 2;
}

static void build_lib(void)
{
    uint32_t s1 = 0x200, e, o;
    memset(limg, 0, sizeof limg);
    limg[0] = 'M'; limg[1] = 'Z';
    lw32(0x3C, HDR);
    limg[HDR] = 'N'; limg[HDR+1] = 'E';
    lw16(HDR+0x04, ENTTAB);
    lw16(HDR+0x0C, 0x8001);         /* LIBRARY | SINGLEDATA  */
    lw16(HDR+0x0E, 0);
    lw32(HDR+0x14, (1u<<16) | 0x00);
    lw16(HDR+0x1C, 1);              /* one segment           */
    lw16(HDR+0x22, SEGTAB);
    lw16(HDR+0x26, LRES);
    lw16(HDR+0x30, 1);
    lw16(HDR+0x32, SECSHIFT);
    limg[HDR+0x36] = 2;

    lw16(HDR+SEGTAB+0, (uint16_t)(s1 >> SECSHIFT)); lw16(HDR+SEGTAB+2, 0x40);
    lw16(HDR+SEGTAB+4, 0);                          lw16(HDR+SEGTAB+6, 0x40);

    e = HDR + ENTTAB;
    limg[e++] = 1; limg[e++] = 1;                      /* bundle: 1 FIXED in seg 1 */
    limg[e++] = 0x01; lw16(e, 0x0010); e += 2;         /* ord 1, EXPORTED          */
    limg[e++] = 1; limg[e++] = 0xFF;                   /* bundle: 1 MOVEABLE       */
    limg[e++] = 0x01; limg[e++] = 0xCD; limg[e++] = 0x3F;
    limg[e++] = 1; lw16(e, 0x0020); e += 2;            /* ord 2 -> seg 1 : 0x0020  */
    limg[e++] = 1; limg[e++] = 1;
    limg[e++] = 0x00; lw16(e, 0x0030); e += 2;         /* ord 3, NOT exported      */
    limg[e++] = 1; limg[e++] = NE_ENT_ABSOLUTE;        /* bundle: 1 ABSOLUTE       */
    limg[e++] = 0x01; lw16(e, 0xA000); e += 2;         /* ord 4 = the CONSTANT     */
    limg[e++] = 0;                                     /* terminator               */
    lw16(HDR+0x06, (uint16_t)(e - (HDR + ENTTAB)));    /* entry table length       */

    o = lpstr(HDR + LRES, "TESTLIB", 0);               /* entry 0 = module name    */
    o = lpstr(o, "FOO", 1);
    limg[o] = 0;

    o = lpstr(LNRES, "a description", 0);              /* entry 0 = description    */
    o = lpstr(o, "BAR", 2);
    limg[o] = 0;
    lw32(HDR+0x2C, LNRES);                             /* ABSOLUTE, and a DWORD    */
    lw16(HDR+0x20, (uint16_t)(o + 1 - LNRES));
}

/* Two segments. Seg1 = code with relocations, seg2 = data.
   Relocation records exercise: SEGMENT/chained, FARADDR/chained, OFFSET16/additive,
   and an INTERNALREF whose target is a MOVEABLE entry ordinal. With `imports`, two
   more records arrive: an IMPORTORDINAL and an IMPORTNAME, both against TESTLIB. */
#define MODTAB 0x80          /* relative to HDR */
#define IMPTAB 0x90

static void build(int imports)
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

    /* module reference + imported names tables, so an importer can name TESTLIB */
    w16(HDR+0x1E, (uint16_t)(imports ? 1 : 0));    /* n_mod                       */
    w16(HDR+0x28, MODTAB);
    w16(HDR+0x2A, IMPTAB);
    w16(HDR+MODTAB, 0);                            /* ref 1 -> imp name at +0     */
    img[HDR+IMPTAB+0] = 7; memcpy(img+HDR+IMPTAB+1, "TESTLIB", 7);
    img[HDR+IMPTAB+8] = 3; memcpy(img+HDR+IMPTAB+9, "BAR", 3);

    /* seg1 relocation table sits right after its 0x40 bytes of data */
    rel = s1 + 0x40;
    w16(rel, (uint16_t)(imports ? 6 : 4));

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

    if (!imports) return;
    /* (e) IMPORTORDINAL: TESTLIB ordinal 1 -> site 0x34 (0x30 is (c)'s sentinel) */
    img[rel+34+0] = NE_ADDR_FARADDR; img[rel+34+1] = NE_REL_IMPORTORD;
    w16(rel+34+2, 0x0034); w16(rel+34+4, 1); w16(rel+34+6, 1);
    w16(s1 + 0x34, 0xFFFF);
    /* (f) IMPORTNAME: TESTLIB "BAR" (imported-names offset 8) -> site 0x3A */
    img[rel+42+0] = NE_ADDR_FARADDR; img[rel+42+1] = NE_REL_IMPORTNAME;
    w16(rel+42+2, 0x003A); w16(rel+42+4, 1); w16(rel+42+6, 8);
    w16(s1 + 0x3A, 0xFFFF);
}

static uint8_t segmem[2][0x200];

int main(void)
{
    ne_module m;
    printf("== WOW: NE loader battery (GH #128/#4) ==\n");

    build(0);
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
    /* ★ 0x0030 was already at the site and 0x00AA is the fixup: ADDITIVE ADDS them.
         This expectation used to read `== 0x00AA` -- written from memory, and wrong.
         gdi.exe refuted it: 366 of its records are additive against a zero-valued
         __MOD_GDI, over sites holding a thunk's API index. */
    ok(ne_rd16(segmem[0] + 0x20) == 0x0030 + 0x00AA,
       "ADDITIVE OFFSET16 ADDS to the value at the site, it does not replace it");
    ok(ne_rd16(segmem[0] + 0x30) == 0xBEEF,
       "ADDITIVE record did NOT follow the value as a chain link");
    ok(ne_rd16(segmem[0] + 0x28) == 0x1234 && ne_rd16(segmem[0] + 0x2A) == 0x2000,
       "moveable INTERNALREF resolved through the entry table");

    /* ★ RELOCATION IS NOT IDEMPOTENT, and this is the check that says so. A chained
         record finds its next site by reading the word AT the current site -- which
         the first pass has just overwritten with an address. Running the pass twice
         over the same memory therefore follows garbage. Anything that wants to
         relocate "again later" (against real selectors, say) must reload the segment
         bytes from the file image first. */
    {   uint32_t first = m.sites;
        m.sites = 0;
        ne_apply_relocs(&m, 0, NULL, NULL);
        ok(m.sites != first,
           "re-relocating an already-patched segment does NOT reproduce the first pass");
    }

    /* an unresolvable import must FAIL, not quietly leave a far call to nowhere */
    {   ne_module m2; uint8_t tmp[0x200];
        build(0);
        img[0x200 + 0x40 + 2 + 1] = NE_REL_IMPORTORD;   /* record (a) -> import */
        ne_parse(&m2, img, sizeof img);
        memcpy(tmp, img + m2.seg[0].file_off, m2.seg[0].length);
        m2.seg[0].mem = tmp; m2.seg[0].seg = 0x1000;
        m2.seg[1].mem = segmem[1]; m2.seg[1].seg = 0x2000;
        ok(ne_apply_relocs(&m2, 0, NULL, NULL) != 0,
           "unresolved IMPORTORDINAL is refused loudly");
        ok(m2.err != 0, "...and records where it gave up");
    }

    /* ── names, exports, and cross-module imports (synthetic) ─────────────────── */
    {   ne_module lib, app; ne_registry reg; uint8_t appseg[2][0x200];
        uint16_t ord = 0, sn = 0, so = 0;
        char nm[NE_MAX_NAME];

        build_lib();
        ok(ne_parse(&lib, limg, sizeof limg) == 0, "synthetic library parses");
        ok((lib.prog_flags & NE_PROG_LIBRARY) != 0, "...and reports itself a LIBRARY");
        ok(ne_own_name(&lib, nm, sizeof nm) == 0 && !strcmp(nm, "TESTLIB"),
           "own name comes from resident-names entry 0");
        ok(ne_export_by_name(&lib, "FOO", &ord) == 0 && ord == 1,
           "export by name, RESIDENT table");
        ok(ne_export_by_name(&lib, "BAR", &ord) == 0 && ord == 2,
           "export by name, NON-RESIDENT table (krnl386 puts 312 exports there)");
        ok(ne_export_by_name(&lib, "bar", &ord) == 0 && ord == 2, "...case-insensitive");
        ok(ne_export_by_name(&lib, "TESTLIB", &ord) != 0,
           "the module's OWN name is not an export");
        ok(ne_export_by_name(&lib, "a description", &ord) != 0,
           "the non-resident DESCRIPTION is not an export either");
        ok(ne_export_by_name(&lib, "NOPE", &ord) != 0, "an absent name fails");
        ok(ne_export_by_ordinal(&lib, 1, &sn, &so) == 0 && sn == 1 && so == 0x10,
           "export by ordinal, FIXED entry");
        ok(ne_export_by_ordinal(&lib, 2, &sn, &so) == 0 && sn == 1 && so == 0x20,
           "export by ordinal, MOVEABLE entry resolves past its INT 3Fh thunk");
        ok(ne_export_by_ordinal(&lib, 3, &sn, &so) != 0,
           "an entry WITHOUT the EXPORTED bit is refused");
        ok(ne_export_by_ordinal(&lib, 4, &sn, &so) == 0 && sn == 0 && so == 0xA000,
           "an ABSOLUTE entry (indicator 0xFE) yields seg_no 0 and its constant");

        /* load the library's one segment and give it a selector */
        lib.seg[0].mem = lsegmem; lib.seg[0].seg = 0x3000;
        memcpy(lsegmem, limg + lib.seg[0].file_off, lib.seg[0].length);

        build(1);
        ok(ne_parse(&app, img, sizeof img) == 0, "importer parses");
        ok(app.n_mod == 1, "importer references 1 module");
        ok(ne_ref_name(&app, 1, nm, sizeof nm) == 0 && !strcmp(nm, "TESTLIB"),
           "module reference 1 names TESTLIB");
        ok(ne_ref_name(&app, 2, nm, sizeof nm) != 0, "a reference past the end fails");
        ok(ne_imported_name(&app, 8, nm, sizeof nm) == 0 && !strcmp(nm, "BAR"),
           "imported-names offset 8 reads BAR");

        memcpy(appseg[0], img + app.seg[0].file_off, app.seg[0].length);
        memcpy(appseg[1], img + app.seg[1].file_off, app.seg[1].length);
        app.seg[0].mem = appseg[0]; app.seg[0].seg = 0x1000;
        app.seg[1].mem = appseg[1]; app.seg[1].seg = 0x2000;

        memset(&reg, 0, sizeof reg);
        ok(ne_registry_add(&reg, &lib) == 0, "library registers");
        ok(ne_registry_find(&reg, "testlib") == &lib, "registry lookup is case-insensitive");
        ok(ne_registry_find(&reg, "KERNEL") == NULL, "an unregistered module is not found");

        ok(ne_apply_relocs(&app, 0, ne_registry_resolve, &reg) == 0,
           "imports resolve through the registry");
        ok(ne_rd16(appseg[0] + 0x34) == 0x0010 && ne_rd16(appseg[0] + 0x36) == 0x3000,
           "IMPORTORDINAL patched to TESTLIB's selector:offset");
        ok(ne_rd16(appseg[0] + 0x3A) == 0x0020 && ne_rd16(appseg[0] + 0x3C) == 0x3000,
           "IMPORTNAME resolved via the non-resident table and patched");

        /* the ordering rule, enforced rather than merely documented */
        {   ne_module l2 = lib; ne_module a2; uint8_t tmp[2][0x200]; ne_registry r2;
            l2.seg[0].seg = 0;                       /* selectors not assigned yet */
            ne_parse(&a2, img, sizeof img);
            memcpy(tmp[0], img + a2.seg[0].file_off, a2.seg[0].length);
            memcpy(tmp[1], img + a2.seg[1].file_off, a2.seg[1].length);
            a2.seg[0].mem = tmp[0]; a2.seg[0].seg = 0x1000;
            a2.seg[1].mem = tmp[1]; a2.seg[1].seg = 0x2000;
            memset(&r2, 0, sizeof r2);
            ne_registry_add(&r2, &l2);
            ok(ne_apply_relocs(&a2, 0, ne_registry_resolve, &r2) != 0,
               "relocating BEFORE the target has selectors is refused, not silently 0000:xxxx");
        }

        /* a missing module must name itself -- "KEYBOARD" is exactly what wowexec
           will hit, and a failure that does not say which module is a dead end */
        {   ne_module a3; uint8_t tmp[2][0x200]; ne_registry r3;
            ne_parse(&a3, img, sizeof img);
            memcpy(tmp[0], img + a3.seg[0].file_off, a3.seg[0].length);
            memcpy(tmp[1], img + a3.seg[1].file_off, a3.seg[1].length);
            a3.seg[0].mem = tmp[0]; a3.seg[0].seg = 0x1000;
            a3.seg[1].mem = tmp[1]; a3.seg[1].seg = 0x2000;
            memset(&r3, 0, sizeof r3);
            ok(ne_apply_relocs(&a3, 0, ne_registry_resolve, &r3) != 0,
               "an import from an unloaded module fails");
            ok(!strcmp(r3.fail_mod, "TESTLIB"), "...and the registry names which module");
        }
    }

    /* malformed input */
    {   ne_module bad; uint8_t junk[64];
        memset(junk, 0, sizeof junk);
        ok(ne_parse(&bad, junk, sizeof junk) != 0, "no MZ -> rejected");
        junk[0] = 'M'; junk[1] = 'Z';
        ok(ne_parse(&bad, junk, sizeof junk) != 0, "MZ but no NE -> rejected");
    }

    /* ── real binaries, if the user supplied them ─────────────────────────────────
         This is the half that matters. Everything above proves the loader does what
         the loader was written to do; this proves the real WOW binaries agree.

         The whole set is loaded, given selectors, registered, and relocated in the
         order the registry documents -- which is the exact sequence the host will
         run.

       ★ THIS IS THE ENTIRE XP WOW MODULE GRAPH, and it CLOSES: 15 modules, every
         import resolved, nothing missing. Earlier the set was five and USER,
         WOWEXEC and SYSEDIT stopped at SYSTEM, KEYBOARD and SHELL -- so the loader
         named the three files to go and fetch, and fetching them (off the rig, out
         of %SystemRoot%\System32) closed every stop with no code change at all.
         That is the payoff for making a failed import name its module instead of
         just failing. */
    {
        static const struct {
            const char *path, *own; int segs, movable, mods; const char *missing;
        } REAL[] = {
            { "guest/ne/krnl386.exe",   "KERNEL",    4, 164, 0, NULL },
            { "guest/ne/system.drv",    "SYSTEM",    2,   0, 1, NULL },
            { "guest/ne/keyboard.drv",  "KEYBOARD",  2,   0, 1, NULL },
            { "guest/ne/mouse.drv",     "MOUSE",     2,   5, 0, NULL },
            { "guest/ne/sound.drv",     "SOUND",     2,   0, 1, NULL },
            { "guest/ne/comm.drv",      "COMM",      4,  21, 2, NULL },
            { "guest/ne/gdi.exe",       "GDI",       2, 355, 1, NULL },
            { "guest/ne/user.exe",      "USER",      3,   0, 2, NULL },
            { "guest/ne/shell.dll",     "SHELL",     2,  36, 1, NULL },
            { "guest/ne/toolhelp.dll",  "TOOLHELP",  2,   0, 2, NULL },
            { "guest/ne/winnls.dll",    "WINNLS",    2,  38, 1, NULL },
            { "guest/ne/wifeman.dll",   "WIFEMAN",   2,  86, 1, NULL },
            { "guest/ne/commdlg.dll",   "COMMDLG",   6,  13, 3, NULL },
            { "guest/ne/wowexec.exe",   "WOWEXEC",   2,   2, 4, NULL },
            { "guest/ne/sysedit.exe",   "SYSEDIT",   6,  21, 4, NULL },
        };
        enum { NREAL = sizeof REAL / sizeof REAL[0] };
        static ne_module r[NREAL];
        int present[NREAL];
        ne_registry reg;
        size_t k;
        int i;

        memset(&reg, 0, sizeof reg);
        memset(present, 0, sizeof present);

        /* step 1: parse and load every module's segments */
        for (k = 0; k < NREAL; ++k) {
            /* run.sh may invoke us from the repo root or from tools/dostest --
               try both rather than silently SKIPping the most valuable cases. */
            FILE *f = fopen(REAL[k].path, "rb");
            uint8_t *buf; long n; char nm[NE_MAX_NAME];
            if (!f) {
                char alt[256];
                snprintf(alt, sizeof alt, "../../%s", REAL[k].path);
                f = fopen(alt, "rb");
            }
            if (!f) { skipped(REAL[k].own); continue; }
            fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
            buf = malloc((size_t)n);
            if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
                fclose(f); free(buf); skipped(REAL[k].own); continue;
            }
            fclose(f);
            printf("  -- %s\n", REAL[k].path);
            ok(ne_parse(&r[k], buf, (uint32_t)n) == 0, "  parses");
            ok(r[k].n_seg == REAL[k].segs, "  segment count matches nedump");
            ok(r[k].n_movable == REAL[k].movable, "  moveable entry count matches nedump");
            ok(r[k].target_os == 2, "  targets Windows");
            ok(r[k].n_mod == REAL[k].mods, "  module reference count matches nedump");
            ok(ne_own_name(&r[k], nm, sizeof nm) == 0 && !strcmp(nm, REAL[k].own),
               "  own name (NOT the file name)");
            /* LIBRARY vs PROGRAM decides whether its CS:IP may be jumped to at all.
               Across the whole set exactly two are PROGRAMs -- wowexec (the one WOW
               actually runs) and sysedit (an ordinary app). Everything else, the
               kernel included, is a library with SS:SP = 0:0. */
            ok(((r[k].prog_flags & NE_PROG_LIBRARY) != 0) ==
               (strcmp(REAL[k].own, "WOWEXEC") != 0 && strcmp(REAL[k].own, "SYSEDIT") != 0),
               "  LIBRARY bit agrees with the bootstrap plan");

            for (i = 0; i < (int)r[k].n_seg; ++i) {
                ne_seg *s = &r[k].seg[i];
                uint32_t need = ne_seg_alloc_size(s);
                s->mem = (uint8_t *)calloc(1, need);
                if (s->sector) memcpy(s->mem, buf + s->file_off, s->length);
                /* step 2: a selector, distinct per module and segment. Real values
                   come from the LDT on the host; only their distinctness matters. */
                s->seg = (uint16_t)(((k + 1) << 8) | ((i + 1) << 3) | 7);
            }
            ok(ne_registry_add(&reg, &r[k]) == 0, "  registers under its own name");
            present[k] = 1;
        }

        if (present[0]) {
            /* The measured facts that made krnl386 the cheap first milestone. */
            ok(r[0].n_mod == 0, "krnl386 imports from NOTHING");
            ok((r[0].csip >> 16) == 1 && (r[0].csip & 0xFFFF) == 0xc02b,
               "krnl386 CS:IP = seg1:0xc02b");
            ok(r[0].align_shift == 4, "krnl386 align shift 4");
            /* ...and the one export every by-name lookup was built for. */
            {   uint16_t o = 0;
                ok(ne_export_by_name(&r[0], "GETWOWCOMPATFLAGSEX", &o) == 0 && o == 521,
                   "KERNEL.GETWOWCOMPATFLAGSEX @521 -- and ONLY the non-resident table has it");
                ok(ne_export_by_name(&r[0], "GLOBALALLOC", &o) == 0,
                   "KERNEL.GLOBALALLOC resolves by name");
            }
            /* The 30 absolute exports, which is what indicator 0xFE turned out to be.
               Values, not addresses -- and gdi/user import 810 sites' worth. */
            {   uint16_t o = 0, sn = 0, so = 0;
                ok(ne_export_by_name(&r[0], "__AHINCR", &o) == 0 &&
                   ne_export_by_ordinal(&r[0], o, &sn, &so) == 0 && sn == 0 && so == 8,
                   "KERNEL.__AHINCR is an ABSOLUTE whose value is 8");
                ok(ne_export_by_name(&r[0], "__A000H", &o) == 0 &&
                   ne_export_by_ordinal(&r[0], o, &sn, &so) == 0 && sn == 0 && so == 0xA000,
                   "KERNEL.__A000H is an ABSOLUTE whose value is 0xA000");
                ok(ne_export_by_name(&r[0], "__MOD_GDI", &o) == 0 && o == 574,
                   "KERNEL.__MOD_GDI @574 -- the export that made gdi.exe fail as 'segment 254'");
            }
        }

        /* step 3: relocate. Every import that can be satisfied, must be. */
        for (k = 0; k < NREAL; ++k) {
            char what[128];
            int rc = 0;
            if (!present[k]) continue;
            r[k].sites = 0;
            for (i = 0; i < (int)r[k].n_seg && rc == 0; ++i)
                rc = ne_apply_relocs(&r[k], i, ne_registry_resolve, &reg);
            if (REAL[k].missing) {
                snprintf(what, sizeof what, "%s: relocation stops at %s, the module we "
                         "did not load", REAL[k].own, REAL[k].missing);
                ok(rc != 0 && !strcmp(reg.fail_mod, REAL[k].missing), what);
            } else {
                /* "Resolved everything" is indistinguishable from "did nothing"
                   unless the sites are counted -- but a module with no relocation
                   records at all is legitimately zero. mouse.drv is exactly that:
                   2 segments, no relocs, imports nothing. So ask the segments what
                   to expect rather than assuming every module has fixups. */
                int has_relocs = 0;
                for (i = 0; i < (int)r[k].n_seg; ++i)
                    if ((r[k].seg[i].flags & NE_SEG_RELOCS) && r[k].seg[i].sector)
                        has_relocs = 1;
                snprintf(what, sizeof what,
                         "%s: EVERY relocation resolved (%u sites)", REAL[k].own, r[k].sites);
                ok(rc == 0, what);
                ok(has_relocs ? r[k].sites > 0 : r[k].sites == 0,
                   has_relocs ? "  ...and it patched something"
                              : "  ...and it has no relocation records, so zero is right");
            }
        }
    }

    printf("\n%d checks, %d failed, %d skipped\n", pass + fail, fail, skip);
    return fail ? 1 : 0;
}
