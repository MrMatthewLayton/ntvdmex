/* dos_xms.h -- XMS 3.0 (eXtended Memory Specification) core, host-testable.
 *
 * The memory-extension layer for M4.  XMS is how real-mode DOS programs reach
 * memory above 1 MB without leaving real mode: they obtain the driver entry
 * point via INT 2Fh AX=4310h and then FAR-CALL it with the function in AH.
 *
 * Architecture note (why this is clean under V86 + NtVdmControl): extended
 * memory is *above* the 1 MB the V86 map covers, so we do NOT place it in the
 * guest address space at all.  Each Extended Memory Block (EMB) is a buffer on
 * the HOST heap.  A pure-real-mode client never addresses an EMB directly --
 * it moves data in and out with the Move function (0Bh), which we implement as
 * a memcpy between the host EMB buffer and the guest's conventional window.
 * (Lock (0Ch) hands back a 32-bit linear address for protected-mode/DPMI
 * clients; see the note on xms_lock.)
 *
 * Same discipline as dos_mcb.h: pure <stdint.h>, no <windows.h>, no globals.
 * The backing store is supplied through alloc/free hooks (the host passes
 * VirtualAlloc-based ones; the off-VM battery passes malloc/free), so the
 * allocator logic is identical in V86 and under the native test cc.
 * Verified off-VM by tools/dostest/xms_test.c.
 */
#ifndef DOS_XMS_H
#define DOS_XMS_H

#include <stdint.h>

#define XMS_MAX_HANDLES   64       /* EMB handles 1..XMS_MAX_HANDLES            */
#define XMS_VERSION       0x0300   /* XMS spec version, BCD (3.0)              */
#define XMS_REVISION      0x0300   /* driver internal revision (cosmetic)      */

/* XMS error codes (returned in BL when AX=0). */
#define XMSERR_NOTIMPL    0x80     /* function not implemented                 */
#define XMSERR_DRIVER     0x8E     /* general driver error                     */
#define XMSERR_HMA_NONE   0x90     /* HMA does not exist                       */
#define XMSERR_HMA_INUSE  0x91     /* HMA already in use                       */
#define XMSERR_HMA_NOTALL 0x93     /* HMA not allocated                        */
#define XMSERR_NOMEM      0xA0     /* all extended memory is allocated         */
#define XMSERR_NOHANDLES  0xA1     /* all handles are in use                   */
#define XMSERR_BADHANDLE  0xA2     /* invalid handle                           */
#define XMSERR_BADSRCH    0xA3     /* invalid source handle                    */
#define XMSERR_BADSRCO    0xA4     /* invalid source offset                    */
#define XMSERR_BADDSTH    0xA5     /* invalid destination handle               */
#define XMSERR_BADDSTO    0xA6     /* invalid destination offset               */
#define XMSERR_BADLEN     0xA7     /* invalid length                           */
#define XMSERR_NOTLOCKED  0xAA     /* block is not locked                      */
#define XMSERR_LOCKED     0xAB     /* block is locked                          */
#define XMSERR_LOCKOVF    0xAC     /* lock count overflow                      */

/* One Extended Memory Block. */
typedef struct {
    uint8_t  used;        /* 1 = allocated (handle in use)                     */
    uint8_t  lock;        /* lock count (0 = unlocked)                         */
    uint32_t size_kb;     /* block size in KB (0 is a legal, empty block)      */
    void    *mem;         /* host backing buffer (size_kb*1024 bytes; 0 if KB=0)*/
} xms_handle;

typedef struct {
    int        a20;             /* A20 gate state (0 = masked, 1 = enabled)     */
    int        hma_used;        /* HMA (the 64KB-16 above 1MB) allocated?       */
    uint32_t   total_kb;        /* size of the extended-memory pool we advertise */
    uint32_t   used_kb;         /* KB currently committed across all handles    */
    xms_handle h[XMS_MAX_HANDLES];
    /* Backing-store hooks. alloc returns a zeroed buffer of `kb` KB (or NULL on
       failure); free releases it. ctx is passed through (host state / unused). */
    void *(*alloc)(void *ctx, uint32_t kb);
    void  (*free) (void *ctx, void *p, uint32_t kb);
    void  *ctx;
} xms_state;

/* The 16-byte Move structure (XMS fn 0Bh), pointed at by DS:SI in the guest.
   For a conventional endpoint Handle==0 and Offset is a real-mode far pointer
   (low word = offset, high word = segment); for an EMB, Offset is a 32-bit
   byte offset into that handle's block. */
typedef struct {
    uint32_t length;       /* bytes to move (must be even; 0 is a no-op)        */
    uint16_t src_handle;   /* 0 = conventional                                  */
    uint32_t src_offset;
    uint16_t dst_handle;   /* 0 = conventional                                  */
    uint32_t dst_offset;
} xms_move_t;

/* --- bring-up -------------------------------------------------------------- */

static inline void xms_init(xms_state *x, uint32_t total_kb,
                            void *(*alloc)(void *, uint32_t),
                            void  (*free)(void *, void *, uint32_t),
                            void *ctx) {
    int i;
    /* ── A20 IS ON, AND SAYING OTHERWISE WAS A LIE ABOUT OUR OWN MACHINE. ─────
         This started masked, so AH=07h (query A20) answered "disabled" until a
         guest happened to call AH=03h. But an NT VDM does not wrap at 1 MB --
         the line is effectively always open -- and extended memory is
         unreachable with A20 masked, so a memory reporter that asks first and
         allocates second is told the pool it can see is unusable.
       Oracle, tools/dostest/p_xms.asm on MS-DOS 6.22 with HIMEM.SYS loaded:
         CASE=xms.07.query.a20 SIG=AX,BX AX=0001 BX=B100
       i.e. enabled. Ours answered AX=0000. (GH #47) */
    x->a20 = 1;
    x->hma_used = 0;
    x->total_kb = total_kb;
    x->used_kb = 0;
    for (i = 0; i < XMS_MAX_HANDLES; ++i) {
        x->h[i].used = 0; x->h[i].lock = 0; x->h[i].size_kb = 0; x->h[i].mem = 0;
    }
    x->alloc = alloc; x->free = free; x->ctx = ctx;
}

/* Resolve a handle number (1-based) to its slot, or NULL if invalid/free. */
static inline xms_handle *xms_get(xms_state *x, uint16_t handle) {
    if (handle == 0 || handle > XMS_MAX_HANDLES) return 0;
    if (!x->h[handle - 1].used) return 0;
    return &x->h[handle - 1];
}

/* --- fn 08h: query free extended memory ------------------------------------ *
 * Returns the largest free block (KB) and the total free (KB). With a single
 * pool the largest free block is the whole remaining pool. */
static inline void xms_query_free(const xms_state *x,
                                  uint32_t *largest_kb, uint32_t *total_kb) {
    uint32_t freekb = (x->total_kb > x->used_kb) ? (x->total_kb - x->used_kb) : 0;
    if (largest_kb) *largest_kb = freekb;
    if (total_kb)   *total_kb   = freekb;
}

/* --- fn 09h: allocate an EMB of `kb` KB ------------------------------------ *
 * On success returns 1 and *out_handle = the new handle (1-based). On failure
 * returns 0 and *err = an XMS error code. A 0-KB request is legal (an empty
 * block to be grown later by Reallocate). */
static inline int xms_alloc(xms_state *x, uint32_t kb,
                            uint16_t *out_handle, uint8_t *err) {
    int i; void *buf = 0;
    if (kb > 0 && x->used_kb + kb > x->total_kb) { if (err) *err = XMSERR_NOMEM; return 0; }
    for (i = 0; i < XMS_MAX_HANDLES; ++i) if (!x->h[i].used) break;
    if (i == XMS_MAX_HANDLES) { if (err) *err = XMSERR_NOHANDLES; return 0; }
    if (kb > 0) {
        buf = x->alloc ? x->alloc(x->ctx, kb) : 0;
        if (!buf) { if (err) *err = XMSERR_NOMEM; return 0; }
    }
    x->h[i].used = 1; x->h[i].lock = 0; x->h[i].size_kb = kb; x->h[i].mem = buf;
    x->used_kb += kb;
    if (out_handle) *out_handle = (uint16_t)(i + 1);
    return 1;
}

/* --- fn 0Ah: free an EMB --------------------------------------------------- *
 * Fails if the block is still locked. */
static inline int xms_free(xms_state *x, uint16_t handle, uint8_t *err) {
    xms_handle *h = xms_get(x, handle);
    if (!h) { if (err) *err = XMSERR_BADHANDLE; return 0; }
    if (h->lock) { if (err) *err = XMSERR_LOCKED; return 0; }
    if (h->mem && x->free) x->free(x->ctx, h->mem, h->size_kb);
    x->used_kb -= h->size_kb;
    h->used = 0; h->lock = 0; h->size_kb = 0; h->mem = 0;
    return 1;
}

/* --- fn 0Fh: reallocate an EMB to `new_kb` KB ------------------------------ *
 * Preserves min(old,new) bytes of content. Fails if the block is locked. */
static inline int xms_realloc(xms_state *x, uint16_t handle, uint32_t new_kb, uint8_t *err) {
    xms_handle *h = xms_get(x, handle);
    void *nb = 0; uint32_t i, keep;
    if (!h) { if (err) *err = XMSERR_BADHANDLE; return 0; }
    if (h->lock) { if (err) *err = XMSERR_LOCKED; return 0; }
    if (new_kb == h->size_kb) return 1;
    if (new_kb > h->size_kb &&
        x->used_kb - h->size_kb + new_kb > x->total_kb) { if (err) *err = XMSERR_NOMEM; return 0; }
    if (new_kb > 0) {
        nb = x->alloc ? x->alloc(x->ctx, new_kb) : 0;
        if (!nb) { if (err) *err = XMSERR_NOMEM; return 0; }
        keep = (new_kb < h->size_kb ? new_kb : h->size_kb) * 1024u;
        for (i = 0; i < keep; ++i) ((uint8_t *)nb)[i] = ((uint8_t *)h->mem)[i];
    }
    if (h->mem && x->free) x->free(x->ctx, h->mem, h->size_kb);
    x->used_kb = x->used_kb - h->size_kb + new_kb;
    h->mem = nb; h->size_kb = new_kb;
    return 1;
}

/* --- fn 0Eh: get EMB handle information ------------------------------------ */
static inline int xms_info(const xms_state *x, uint16_t handle,
                           uint8_t *lock_count, uint8_t *free_handles,
                           uint32_t *size_kb, uint8_t *err) {
    int i, freeh = 0;
    const xms_handle *h;
    if (handle == 0 || handle > XMS_MAX_HANDLES || !x->h[handle - 1].used) {
        if (err) *err = XMSERR_BADHANDLE;
        return 0;
    }
    h = &x->h[handle - 1];
    for (i = 0; i < XMS_MAX_HANDLES; ++i) if (!x->h[i].used) ++freeh;
    if (lock_count)   *lock_count   = h->lock;
    if (free_handles) *free_handles = (uint8_t)(freeh > 255 ? 255 : freeh);
    if (size_kb)      *size_kb      = h->size_kb;
    return 1;
}

/* --- fn 0Ch / 0Dh: lock / unlock an EMB ------------------------------------ *
 * Lock pins the block and returns its 32-bit linear address. Under our model
 * the EMB lives on the host heap, so the "linear address" is the host pointer
 * truncated to 32 bits -- meaningful only to a protected-mode/DPMI client that
 * shares our flat address space (M4 DPMI work); a pure real-mode client uses
 * Move instead and never dereferences this. */
static inline int xms_lock(xms_state *x, uint16_t handle, uint32_t *linaddr, uint8_t *err) {
    xms_handle *h = xms_get(x, handle);
    if (!h) { if (err) *err = XMSERR_BADHANDLE; return 0; }
    if (h->lock == 0xFF) { if (err) *err = XMSERR_LOCKOVF; return 0; }
    ++h->lock;
    if (linaddr) *linaddr = (uint32_t)(uintptr_t)h->mem;
    return 1;
}

static inline int xms_unlock(xms_state *x, uint16_t handle, uint8_t *err) {
    xms_handle *h = xms_get(x, handle);
    if (!h) { if (err) *err = XMSERR_BADHANDLE; return 0; }
    if (h->lock == 0) { if (err) *err = XMSERR_NOTLOCKED; return 0; }
    --h->lock;
    return 1;
}

/* --- fn 0Bh: move a block -------------------------------------------------- *
 * Copies mv->length bytes from the source endpoint to the destination. An
 * endpoint with handle 0 is conventional memory: its offset is a real-mode far
 * pointer (low word = offset, high word = segment) resolved against `conv_base`
 * (NULL => absolute V86 addressing: linear = (seg<<4)+off). An endpoint with a
 * nonzero handle is an EMB: its offset is a byte offset into that block.
 * Returns 1 on success; 0 with *err set on a bad handle/offset/length. */
static inline int xms_move(xms_state *x, volatile uint8_t *conv_base,
                           const xms_move_t *mv, uint8_t *err) {
    volatile uint8_t *src, *dst;
    uint32_t len = mv->length, i;
    if (len == 0) return 1;                 /* a 0-length move is a legal no-op  */
    if (len & 1) { if (err) *err = XMSERR_BADLEN; return 0; }

    if (mv->src_handle == 0) {
        uint32_t seg = (mv->src_offset >> 16) & 0xFFFF, off = mv->src_offset & 0xFFFF;
        src = (volatile uint8_t *)((uintptr_t)conv_base + (seg << 4) + off);
    } else {
        xms_handle *h = xms_get(x, mv->src_handle);
        if (!h) { if (err) *err = XMSERR_BADSRCH; return 0; }
        if (mv->src_offset + len > h->size_kb * 1024u) { if (err) *err = XMSERR_BADSRCO; return 0; }
        src = (volatile uint8_t *)((uint8_t *)h->mem + mv->src_offset);
    }
    if (mv->dst_handle == 0) {
        uint32_t seg = (mv->dst_offset >> 16) & 0xFFFF, off = mv->dst_offset & 0xFFFF;
        dst = (volatile uint8_t *)((uintptr_t)conv_base + (seg << 4) + off);
    } else {
        xms_handle *h = xms_get(x, mv->dst_handle);
        if (!h) { if (err) *err = XMSERR_BADDSTH; return 0; }
        if (mv->dst_offset + len > h->size_kb * 1024u) { if (err) *err = XMSERR_BADDSTO; return 0; }
        dst = (volatile uint8_t *)((uint8_t *)h->mem + mv->dst_offset);
    }
    /* Overlap-safe copy (real HIMEM permits an overlapping move within a block). */
    if (dst <= src) for (i = 0; i < len; ++i) dst[i] = src[i];
    else            for (i = len; i-- > 0; )  dst[i] = src[i];
    return 1;
}

#endif /* DOS_XMS_H */
