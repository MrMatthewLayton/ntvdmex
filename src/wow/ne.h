/* ne.h -- 16-bit New Executable loader (GH #128 / #4).
 *
 * The image format every 16-bit Windows program uses, and the first brick of the WOW
 * layer. Header-only and free of any Windows or VDM dependency, by the same convention
 * as src/dos/ -- so the whole thing is exercised off-VM by tools/dostest/ne_test.c in
 * milliseconds instead of a round trip to the rig.
 *
 * ── WRITTEN AGAINST MEASURED BINARIES, NOT THE SPEC. ────────────────────────────────
 * `tools/ne/nedump.py` was built first and pointed at the real files. What they
 * actually contain decided what is implemented here and in what order:
 *
 *   krnl386.exe   4 segs, 13 relocs, ALL of one kind (INTERNALREF/SEGMENT, chained,
 *                 targeting segment 1), and it IMPORTS FROM NOTHING -- it is the
 *                 kernel. This is what `ntvdm -a ...\krnl386.exe` asks WOW to boot.
 *   sysedit.exe   6 segs, 179 relocs over five kinds, importing from KERNEL, GDI,
 *                 USER and SHELL. 147 are IMPORTORDINAL/FAR_ADDR -- ordinary API
 *                 calls by ordinal, which is the gate to running real programs.
 *
 * So a loader that handles INTERNALREF is already enough to place krnl386 in memory,
 * and imports can arrive later without restructuring anything.
 *
 * ⚠ TWO THINGS THE FORMAT DOES THAT ARE EASY TO GET WRONG, AND BOTH ARE HERE:
 *   1. RELOCATION CHAINS. A record does not name one site. Unless the ADDITIVE bit is
 *      set, the word AT the site holds the offset of the NEXT site to patch, and the
 *      chain ends at 0xFFFF. Treating each record as a single fixup silently leaves
 *      most of a segment unrelocated.
 *   2. MOVEABLE TARGETS. An INTERNALREF with a==0xFF does not name a segment; it names
 *      an ENTRY TABLE ORDINAL, which must be looked up to get the real segment:offset.
 */
#ifndef NTVDMEX_NE_H
#define NTVDMEX_NE_H

#include <stdint.h>

#define NE_MAX_SEG 96

/* ne_seg.flags */
#define NE_SEG_DATA        0x0001
#define NE_SEG_MOVEABLE    0x0010
#define NE_SEG_PRELOAD     0x0040
#define NE_SEG_RELOCS      0x0100

/* relocation record: low 2 bits of rel_type */
#define NE_REL_INTERNAL    0
#define NE_REL_IMPORTORD   1
#define NE_REL_IMPORTNAME  2
#define NE_REL_OSFIXUP     3
#define NE_REL_ADDITIVE    0x04      /* ...so do NOT walk a chain */

/* relocation record: addr_type -- what shape of thing is at the site */
#define NE_ADDR_LOBYTE     0
#define NE_ADDR_SEGMENT    2         /* 16-bit segment/selector          */
#define NE_ADDR_FARADDR    3         /* 32-bit off:seg                   */
#define NE_ADDR_OFFSET16   5         /* 16-bit offset                    */
#define NE_ADDR_FARADDR48  11        /* 48-bit off32:seg (386)           */
#define NE_ADDR_OFFSET32   13

typedef struct {
    uint16_t sector, flags, minalloc;
    uint32_t file_off, length;
    uint16_t seg;          /* runtime segment/selector -- filled by the caller  */
    uint8_t *mem;          /* host pointer to this segment's loaded bytes       */
} ne_seg;

typedef struct {
    const uint8_t *img;
    uint32_t img_len, hdr;
    uint16_t prog_flags, autodata, heap, stack;
    uint32_t csip, sssp;
    uint16_t n_seg, n_mod, n_movable, align_shift, expect_ver;
    uint16_t entry_tab, entry_len, seg_tab, res_tab, resident_tab, mod_tab, imp_tab;
    uint8_t  target_os, other_flags;
    ne_seg   seg[NE_MAX_SEG];
    int      err;          /* 0 = ok; otherwise the __LINE__ that rejected it   */
    /* ⚠ How many SITES were actually patched, accumulated across segments. A
         relocation pass that returns "success" having done nothing is
         indistinguishable from one that worked, and the guest only finds out
         later and somewhere else. Count it, print it, and assert on it. */
    uint32_t sites;
} ne_module;

/* Resolve an imported entry point. Returns 0 on success and fills seg:off.
   `mod` is a 1-based index into the module reference table; `ord_or_name` is the
   ordinal (NE_REL_IMPORTORD) or an offset into the imported-names table. */
typedef int (*ne_import_fn)(void *ctx, uint16_t mod, uint16_t ord_or_name,
                            uint16_t *seg, uint16_t *off);

/* ── little-endian readers, bounds-checked ──────────────────────────────────────── */
static uint16_t ne_rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t ne_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void ne_wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

static int ne_ok(const ne_module *m, uint32_t off, uint32_t n)
{
    return off <= m->img_len && n <= m->img_len - off;
}

/* ── parse ──────────────────────────────────────────────────────────────────────────
     MZ at 0, a LONG at 0x3C giving the second header, and 'NE' there. Field offsets
     are spelled out because getting one wrong shifts every table after it and the
     failure looks like corrupt data rather than a bad constant -- which is exactly
     what happened while writing nedump.py. */
static int ne_parse(ne_module *m, const uint8_t *img, uint32_t len)
{
    uint32_t h;
    uint16_t i;
    int k;
    for (k = 0; k < (int)sizeof *m; ++k) ((uint8_t *)m)[k] = 0;
    m->img = img; m->img_len = len;

    if (len < 0x40 || img[0] != 'M' || img[1] != 'Z') { m->err = __LINE__; return -1; }
    h = ne_rd32(img + 0x3C);
    if (!ne_ok(m, h, 0x40)) { m->err = __LINE__; return -1; }
    if (img[h] != 'N' || img[h + 1] != 'E') { m->err = __LINE__; return -1; }
    m->hdr = h;

    m->entry_tab    = ne_rd16(img + h + 0x04);
    m->entry_len    = ne_rd16(img + h + 0x06);
    m->prog_flags   = ne_rd16(img + h + 0x0C);
    m->autodata     = ne_rd16(img + h + 0x0E);
    m->heap         = ne_rd16(img + h + 0x10);
    m->stack        = ne_rd16(img + h + 0x12);
    m->csip         = ne_rd32(img + h + 0x14);
    m->sssp         = ne_rd32(img + h + 0x18);
    m->n_seg        = ne_rd16(img + h + 0x1C);
    m->n_mod        = ne_rd16(img + h + 0x1E);
    m->seg_tab      = ne_rd16(img + h + 0x22);
    m->res_tab      = ne_rd16(img + h + 0x24);
    m->resident_tab = ne_rd16(img + h + 0x26);
    m->mod_tab      = ne_rd16(img + h + 0x28);
    m->imp_tab      = ne_rd16(img + h + 0x2A);
    m->n_movable    = ne_rd16(img + h + 0x30);
    m->align_shift  = ne_rd16(img + h + 0x32);
    m->target_os    = img[h + 0x36];
    m->other_flags  = img[h + 0x37];
    m->expect_ver   = ne_rd16(img + h + 0x3E);

    if (!m->align_shift) m->align_shift = 9;          /* 0 means 512, not 1 */
    if (m->n_seg > NE_MAX_SEG) { m->err = __LINE__; return -1; }
    if (!ne_ok(m, h + m->seg_tab, (uint32_t)m->n_seg * 8)) { m->err = __LINE__; return -1; }

    for (i = 0; i < m->n_seg; ++i) {
        const uint8_t *e = img + h + m->seg_tab + i * 8;
        ne_seg *s = &m->seg[i];
        s->sector   = ne_rd16(e);
        s->length   = ne_rd16(e + 2);
        s->flags    = ne_rd16(e + 4);
        s->minalloc = ne_rd16(e + 6);
        /* A zero length means 64K -- but only if the segment has file data at all. */
        if (!s->length && s->sector) s->length = 0x10000;
        s->file_off = (uint32_t)s->sector << m->align_shift;
        if (s->sector && !ne_ok(m, s->file_off, s->length)) { m->err = __LINE__; return -1; }
    }
    return 0;
}

/* ── entry table ────────────────────────────────────────────────────────────────────
     Bundles of entries, ordinals numbered sequentially from 1 across all bundles.
     Each bundle: BYTE count, BYTE segment indicator.
       indicator 0     -> end of table
       indicator 0xFF  -> MOVEABLE: 6 bytes each (flags, INT 3Fh thunk, segno, offset)
       otherwise       -> FIXED in that segment: 3 bytes each (flags, offset)
     Needed because an INTERNALREF with a==0xFF targets an ORDINAL, not a segment. */
static int ne_entry_lookup(const ne_module *m, uint16_t ordinal,
                           uint16_t *seg_no, uint16_t *off)
{
    uint32_t o = m->hdr + m->entry_tab;
    uint32_t end = o + m->entry_len;
    uint16_t cur = 1;
    if (!ordinal || !ne_ok(m, o, m->entry_len)) return -1;
    while (o + 2 <= end) {
        uint8_t cnt = m->img[o], ind = m->img[o + 1];
        uint32_t rec = o + 2, step;
        if (!cnt) break;                       /* count 0 terminates */
        step = (ind == 0xFF) ? 6u : (ind == 0 ? 0u : 3u);
        if (!step) { o = rec; cur = (uint16_t)(cur + cnt); continue; }  /* null bundle */
        if (ordinal >= cur && ordinal < cur + cnt) {
            uint32_t e = rec + (uint32_t)(ordinal - cur) * step;
            if (!ne_ok(m, e, step)) return -1;
            if (ind == 0xFF) { *seg_no = m->img[e + 3]; *off = ne_rd16(m->img + e + 4); }
            else             { *seg_no = ind;           *off = ne_rd16(m->img + e + 1); }
            return 0;
        }
        cur = (uint16_t)(cur + cnt);
        o = rec + (uint32_t)cnt * step;
    }
    return -1;
}

/* ── relocations ────────────────────────────────────────────────────────────────────
     Applied to segment `idx` (0-based) after its bytes are in seg[idx].mem and every
     segment's runtime `seg` value is known. The record table lives immediately AFTER
     the segment's file data: a WORD count, then 8 bytes each. */
static int ne_apply_relocs(ne_module *m, int idx, ne_import_fn imp, void *ctx)
{
    ne_seg *s = &m->seg[idx];
    uint32_t o, n, i;
    if (!(s->flags & NE_SEG_RELOCS) || !s->sector) return 0;
    o = s->file_off + s->length;
    if (!ne_ok(m, o, 2)) { m->err = __LINE__; return -1; }
    n = ne_rd16(m->img + o); o += 2;
    if (!ne_ok(m, o, n * 8)) { m->err = __LINE__; return -1; }

    for (i = 0; i < n; ++i) {
        const uint8_t *r = m->img + o + i * 8;
        uint8_t  at = r[0], rt = r[1];
        uint16_t site = ne_rd16(r + 2), a = ne_rd16(r + 4), b = ne_rd16(r + 6);
        uint16_t tseg = 0, toff = 0;

        switch (rt & 3) {
        case NE_REL_INTERNAL:
            if ((a & 0xFF) == 0xFF) {           /* target names an entry ordinal */
                uint16_t sn;
                if (ne_entry_lookup(m, b, &sn, &toff) != 0) { m->err = __LINE__; return -1; }
                if (!sn || sn > m->n_seg) { m->err = __LINE__; return -1; }
                tseg = m->seg[sn - 1].seg;
            } else {
                if (!a || a > m->n_seg) { m->err = __LINE__; return -1; }
                tseg = m->seg[a - 1].seg;
                toff = b;
            }
            break;
        case NE_REL_IMPORTORD:
        case NE_REL_IMPORTNAME:
            /* Not resolvable without the exporting module. LOUD, not silent: a
               relocation left unapplied is a far call into nothing, and the guest
               dies far away from here with no clue why. */
            if (!imp || imp(ctx, a, b, &tseg, &toff) != 0) { m->err = __LINE__; return -1; }
            break;
        default:
            m->err = __LINE__; return -1;       /* OSFIXUP: none seen; refuse it */
        }

        /* Walk the chain (or patch once, if ADDITIVE). */
        for (;;) {
            uint16_t next;
            if (site + 2u > s->length) break;    /* a chain may run off the end */
            next = ne_rd16(s->mem + site);
            switch (at) {
            case NE_ADDR_SEGMENT:  ne_wr16(s->mem + site, tseg); break;
            case NE_ADDR_OFFSET16: ne_wr16(s->mem + site, toff); break;
            case NE_ADDR_FARADDR:
                if (site + 4u > s->length) break;
                ne_wr16(s->mem + site, toff);
                ne_wr16(s->mem + site + 2, tseg);
                break;
            case NE_ADDR_LOBYTE:   s->mem[site] = (uint8_t)toff; break;
            default:               m->err = __LINE__; return -1;
            }
            ++m->sites;
            if (rt & NE_REL_ADDITIVE) break;     /* additive records are not chained */
            if (next == 0xFFFF) break;
            site = next;
        }
    }
    return 0;
}

/* Convenience: how many bytes a segment needs in memory (minalloc can exceed the
   file length -- BSS-style tail that must be present and zeroed). */
static uint32_t ne_seg_alloc_size(const ne_seg *s)
{
    uint32_t want = s->length;
    if (s->minalloc && s->minalloc > want) want = s->minalloc;
    if (!want) want = 1;
    return want;
}

#endif /* NTVDMEX_NE_H */
