/* dos_ems.h -- LIM EMS 4.0 (Expanded Memory) core, host-testable.
 *
 * The second memory-extension layer for M4. EMS predates XMS and is reached via
 * INT 67h (the EMM driver, "EMMXXXX0"). Where XMS hands a real-mode program a
 * Move API, EMS gives it a 64 KB *page frame* in the upper-memory area split
 * into four 16 KB physical windows; the program maps any of its 16 KB logical
 * pages (drawn from a large expanded-memory pool) into those windows and then
 * reads/writes them directly through the frame.
 *
 * How this works under V86 without trapping (page-frame shadowing): expanded
 * memory lives in HOST buffers (one per handle, logical_pages * 16 KB). Mapping
 * logical page L into physical window P is a memcpy: first WRITE BACK whatever
 * 16 KB currently occupies window P to its backing logical page (the guest may
 * have written it directly), then READ IN page L. Between maps the guest's
 * direct accesses to the frame are just RAM accesses -- no fault, no trap. The
 * backing buffer is authoritative for every logical page EXCEPT the (<=4)
 * currently resident in a window, which is exactly the EMS access model.
 *
 * Same discipline as dos_mcb.h / dos_xms.h: pure <stdint.h>, no <windows.h>, no
 * globals; backing store via alloc/free hooks; the page-frame window is a caller
 * supplied pointer (host: the mapped 0xE0000 RAM; tests: a 64 KB buffer).
 * Verified off-VM by tools/dostest/ems_test.c.
 */
#ifndef DOS_EMS_H
#define DOS_EMS_H

#include <stdint.h>

#define EMS_PAGE_SIZE     0x4000u   /* 16 KB logical/physical page                */
#define EMS_PHYS_PAGES    4         /* the page frame is four 16 KB windows        */
#define EMS_FRAME_SIZE    (EMS_PHYS_PAGES * EMS_PAGE_SIZE)  /* 64 KB               */
#define EMS_MAX_HANDLES   64
#define EMS_VERSION       0x40      /* LIM EMS 4.0, BCD in AL                       */

/* EMM status codes (returned in AH). */
#define EMS_OK            0x00
#define EMSERR_INTERNAL   0x80      /* internal driver error                       */
#define EMSERR_HARDWARE   0x81      /* hardware malfunction                        */
#define EMSERR_BADHANDLE  0x83      /* invalid handle                              */
#define EMSERR_UNDEFFUNC  0x84      /* undefined function requested                */
#define EMSERR_NOHANDLES  0x85      /* no more handles available                   */
#define EMSERR_SAVERESTORE 0x86     /* page-map save/restore error                 */
#define EMSERR_TOOMANY    0x87      /* more pages requested than physically exist  */
#define EMSERR_NOTENOUGH  0x88      /* not enough free pages to satisfy request    */
#define EMSERR_ZEROPAGES  0x89      /* zero pages requested (alloc)                 */
#define EMSERR_BADLOGICAL 0x8A      /* logical page out of range for the handle    */
#define EMSERR_BADPHYS    0x8B      /* illegal physical-page (window) number       */
#define EMSERR_SAVED      0x8D      /* page map already saved for this handle      */
#define EMSERR_NOTSAVED   0x8E      /* no saved page map for this handle           */

typedef struct {
    uint8_t  used;
    uint16_t pages;        /* logical 16 KB pages this handle owns (0 is legal)    */
    void    *mem;          /* host buffer, pages * 16 KB                           */
    /* EMS 4.0 page-map save/restore (fn 47h/48h): one snapshot of the 4 windows. */
    uint8_t  saved;
    struct { uint16_t handle, logical; uint8_t mapped; } save[EMS_PHYS_PAGES];
} ems_handle;

typedef struct {
    uint16_t frame_seg;    /* page-frame segment (e.g. 0xE000)                     */
    uint16_t total_pages;  /* size of the expanded-memory pool, in 16 KB pages     */
    uint16_t used_pages;   /* pages committed across all handles                   */
    /* current contents of the four physical windows */
    struct { uint16_t handle, logical; uint8_t mapped; } phys[EMS_PHYS_PAGES];
    volatile uint8_t *frame;          /* the 64 KB page-frame window (RAM)         */
    ems_handle h[EMS_MAX_HANDLES];
    void *(*alloc)(void *ctx, uint32_t pages);          /* -> pages*16KB buffer    */
    void  (*free) (void *ctx, void *p, uint32_t pages);
    void  *ctx;
} ems_state;

/* --- bring-up -------------------------------------------------------------- */

static inline void ems_init(ems_state *e, uint16_t frame_seg, uint16_t total_pages,
                            volatile uint8_t *frame,
                            void *(*alloc)(void *, uint32_t),
                            void  (*free)(void *, void *, uint32_t), void *ctx) {
    int i, p;
    e->frame_seg = frame_seg;
    e->total_pages = total_pages;
    e->used_pages = 0;
    e->frame = frame;
    for (p = 0; p < EMS_PHYS_PAGES; ++p) { e->phys[p].handle = 0; e->phys[p].logical = 0; e->phys[p].mapped = 0; }
    for (i = 0; i < EMS_MAX_HANDLES; ++i) {
        e->h[i].used = 0; e->h[i].pages = 0; e->h[i].mem = 0; e->h[i].saved = 0;
    }
    e->alloc = alloc; e->free = free; e->ctx = ctx;
}

static inline ems_handle *ems_get(ems_state *e, uint16_t handle) {
    if (handle >= EMS_MAX_HANDLES) return 0;
    if (!e->h[handle].used) return 0;
    return &e->h[handle];
}

/* Copy helper: 16 KB between the page-frame window and a handle's backing page. */
static inline void ems_copy(volatile uint8_t *dst, volatile uint8_t *src) {
    uint32_t i; for (i = 0; i < EMS_PAGE_SIZE; ++i) dst[i] = src[i];
}

/* Write back whatever currently sits in physical window `p` to its backing page. */
static inline void ems_writeback(ems_state *e, int p) {
    ems_handle *h;
    if (!e->phys[p].mapped) return;
    h = ems_get(e, e->phys[p].handle);
    if (h && h->mem && e->phys[p].logical < h->pages)
        ems_copy((volatile uint8_t *)((uint8_t *)h->mem + (uint32_t)e->phys[p].logical * EMS_PAGE_SIZE),
                 e->frame + (uint32_t)p * EMS_PAGE_SIZE);
}

/* --- fn 42h: page counts --------------------------------------------------- */
static inline void ems_counts(const ems_state *e, uint16_t *free_pages, uint16_t *total_pages) {
    if (total_pages) *total_pages = e->total_pages;
    if (free_pages)  *free_pages  = (uint16_t)(e->total_pages - e->used_pages);
}

/* --- fn 43h: allocate `pages` logical pages, returns a handle ------------- *
 * EMS forbids a zero-page allocation here (fn 43h); fn 5Ah allows it. */
static inline int ems_alloc(ems_state *e, uint16_t pages, uint16_t *out_handle, uint8_t *err) {
    int i; void *buf = 0;
    if (pages == 0) { if (err) *err = EMSERR_ZEROPAGES; return 0; }
    if (pages > e->total_pages) { if (err) *err = EMSERR_TOOMANY; return 0; }
    if (e->used_pages + pages > e->total_pages) { if (err) *err = EMSERR_NOTENOUGH; return 0; }
    for (i = 0; i < EMS_MAX_HANDLES; ++i) if (!e->h[i].used) break;
    if (i == EMS_MAX_HANDLES) { if (err) *err = EMSERR_NOHANDLES; return 0; }
    buf = e->alloc ? e->alloc(e->ctx, pages) : 0;
    if (!buf) { if (err) *err = EMSERR_NOTENOUGH; return 0; }
    e->h[i].used = 1; e->h[i].pages = pages; e->h[i].mem = buf; e->h[i].saved = 0;
    e->used_pages += pages;
    if (out_handle) *out_handle = (uint16_t)i;
    if (err) *err = EMS_OK;
    return 1;
}

/* --- fn 44h: map logical page `logical` of `handle` into window `phys` ----- *
 * logical == 0xFFFF unmaps the window (LIM 4.0). */
static inline int ems_map(ems_state *e, uint8_t phys, uint16_t logical, uint16_t handle, uint8_t *err) {
    ems_handle *h;
    if (phys >= EMS_PHYS_PAGES) { if (err) *err = EMSERR_BADPHYS; return 0; }
    h = ems_get(e, handle);
    if (!h) { if (err) *err = EMSERR_BADHANDLE; return 0; }
    if (logical == 0xFFFF) {                         /* unmap this window         */
        ems_writeback(e, phys);
        e->phys[phys].mapped = 0;
        if (err) *err = EMS_OK;
        return 1;
    }
    if (logical >= h->pages) { if (err) *err = EMSERR_BADLOGICAL; return 0; }
    ems_writeback(e, phys);                          /* save the outgoing page    */
    ems_copy(e->frame + (uint32_t)phys * EMS_PAGE_SIZE,
             (volatile uint8_t *)((uint8_t *)h->mem + (uint32_t)logical * EMS_PAGE_SIZE));
    e->phys[phys].handle = handle; e->phys[phys].logical = logical; e->phys[phys].mapped = 1;
    if (err) *err = EMS_OK;
    return 1;
}

/* --- fn 45h: deallocate a handle ------------------------------------------- */
static inline int ems_free(ems_state *e, uint16_t handle, uint8_t *err) {
    ems_handle *h = ems_get(e, handle);
    int p;
    if (!h) { if (err) *err = EMSERR_BADHANDLE; return 0; }
    for (p = 0; p < EMS_PHYS_PAGES; ++p)            /* drop any live windows      */
        if (e->phys[p].mapped && e->phys[p].handle == handle) e->phys[p].mapped = 0;
    if (h->mem && e->free) e->free(e->ctx, h->mem, h->pages);
    e->used_pages -= h->pages;
    h->used = 0; h->pages = 0; h->mem = 0; h->saved = 0;
    if (err) *err = EMS_OK;
    return 1;
}

/* --- fn 4Ch: pages owned by a handle --------------------------------------- */
static inline int ems_handle_pages(ems_state *e, uint16_t handle, uint16_t *pages, uint8_t *err) {
    ems_handle *h = ems_get(e, handle);
    if (!h) { if (err) *err = EMSERR_BADHANDLE; return 0; }
    if (pages) *pages = h->pages;
    if (err) *err = EMS_OK;
    return 1;
}

/* --- fn 4Bh: number of open handles ---------------------------------------- */
static inline int ems_handle_count(const ems_state *e) {
    int i, n = 0;
    for (i = 0; i < EMS_MAX_HANDLES; ++i) if (e->h[i].used) ++n;
    return n;
}

/* --- fn 51h: reallocate a handle's page count ------------------------------ *
 * Preserves min(old,new) pages of content. Active mappings of pages that no
 * longer exist after a shrink are dropped. */
static inline int ems_realloc(ems_state *e, uint16_t handle, uint16_t new_pages, uint8_t *err) {
    ems_handle *h = ems_get(e, handle);
    void *nb = 0; uint32_t keep, i; int p;
    if (!h) { if (err) *err = EMSERR_BADHANDLE; return 0; }
    if (new_pages == h->pages) { if (err) *err = EMS_OK; return 1; }
    if (new_pages > h->pages &&
        e->used_pages - h->pages + new_pages > e->total_pages) { if (err) *err = EMSERR_NOTENOUGH; return 0; }
    /* flush any of this handle's live windows so the backing buffer is current  */
    for (p = 0; p < EMS_PHYS_PAGES; ++p)
        if (e->phys[p].mapped && e->phys[p].handle == handle) ems_writeback(e, p);
    if (new_pages > 0) {
        nb = e->alloc ? e->alloc(e->ctx, new_pages) : 0;
        if (!nb) { if (err) *err = EMSERR_NOTENOUGH; return 0; }
        keep = (new_pages < h->pages ? new_pages : h->pages) * EMS_PAGE_SIZE;
        for (i = 0; i < keep; ++i) ((uint8_t *)nb)[i] = ((uint8_t *)h->mem)[i];
    }
    for (p = 0; p < EMS_PHYS_PAGES; ++p)            /* drop now-invalid mappings  */
        if (e->phys[p].mapped && e->phys[p].handle == handle && e->phys[p].logical >= new_pages)
            e->phys[p].mapped = 0;
    if (h->mem && e->free) e->free(e->ctx, h->mem, h->pages);
    e->used_pages = (uint16_t)(e->used_pages - h->pages + new_pages);
    h->mem = nb; h->pages = new_pages;
    if (err) *err = EMS_OK;
    return 1;
}

/* --- fn 47h/48h: save / restore the page-map context for a handle ---------- */
static inline int ems_save_map(ems_state *e, uint16_t handle, uint8_t *err) {
    ems_handle *h = ems_get(e, handle);
    int p;
    if (!h) { if (err) *err = EMSERR_BADHANDLE; return 0; }
    if (h->saved) { if (err) *err = EMSERR_SAVED; return 0; }
    for (p = 0; p < EMS_PHYS_PAGES; ++p) {
        h->save[p].handle  = e->phys[p].handle;
        h->save[p].logical = e->phys[p].logical;
        h->save[p].mapped  = e->phys[p].mapped;
    }
    h->saved = 1;
    if (err) *err = EMS_OK;
    return 1;
}

static inline int ems_restore_map(ems_state *e, uint16_t handle, uint8_t *err) {
    ems_handle *h = ems_get(e, handle);
    int p;
    if (!h) { if (err) *err = EMSERR_BADHANDLE; return 0; }
    if (!h->saved) { if (err) *err = EMSERR_NOTSAVED; return 0; }
    for (p = 0; p < EMS_PHYS_PAGES; ++p) {
        if (h->save[p].mapped) ems_map(e, (uint8_t)p, h->save[p].logical, h->save[p].handle, err);
        else { ems_writeback(e, p); e->phys[p].mapped = 0; }
    }
    h->saved = 0;
    if (err) *err = EMS_OK;
    return 1;
}

#endif /* DOS_EMS_H */
