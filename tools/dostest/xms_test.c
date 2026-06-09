/* xms_test.c -- off-VM unit battery for the XMS core (src/dos/dos_xms.h).
 *
 * Layer-1 test for M4 slice 1, in the same style as mcb_test.c: the XMS
 * allocator + Move logic run against host memory with malloc/free backing
 * hooks and a plain buffer standing in for the guest's conventional window --
 * no VM, no Windows. Exercises version/free-query, alloc/free with pool
 * accounting + handle exhaustion, realloc (grow/shrink/content-preserve),
 * lock/unlock (incl. free-while-locked), and the Move function across all
 * four endpoint combinations (conv<->EMB, EMB<->EMB) plus error paths.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dos_xms.h"

static int total = 0, fails = 0;
#define CHECK(c,m) do{ total++; if(c){printf("  PASS  %s\n",(m));} \
    else{printf("  FAIL  %s\n",(m)); fails++;} }while(0)

/* Backing hooks: real host heap, KB-sized. */
static void *t_alloc(void *ctx, uint32_t kb) { (void)ctx; return calloc((size_t)kb, 1024); }
static void  t_free (void *ctx, void *p, uint32_t kb) { (void)ctx; (void)kb; free(p); }

int main(void)
{
    xms_state x;
    uint16_t h1, h2, h3;
    uint8_t err, lock, freeh;
    uint32_t largest, totfree, size_kb, lin1, lin2;
    int i, ok;

    printf("== M4 XMS core battery ==\n");

    /* A 1 MB pool (1024 KB). */
    xms_init(&x, 1024, t_alloc, t_free, NULL);

    /* T1: fresh state ----------------------------------------------------- */
    CHECK(x.a20 == 0 && x.hma_used == 0, "init: A20 masked, HMA free");
    xms_query_free(&x, &largest, &totfree);
    CHECK(largest == 1024 && totfree == 1024, "fn08: whole pool free initially");

    /* T2: allocate 64 KB -------------------------------------------------- */
    err = 0;
    ok = xms_alloc(&x, 64, &h1, &err);
    CHECK(ok && h1 == 1, "fn09: alloc 64KB -> handle 1");
    xms_query_free(&x, &largest, &totfree);
    CHECK(largest == 960 && totfree == 960, "fn08: 960KB free after 64KB alloc");

    /* T3: handle info ----------------------------------------------------- */
    ok = xms_info(&x, h1, &lock, &freeh, &size_kb, &err);
    CHECK(ok && size_kb == 64 && lock == 0, "fn0E: handle 1 is 64KB, unlocked");
    CHECK(freeh == XMS_MAX_HANDLES - 1, "fn0E: free-handle count reflects 1 in use");

    /* T4: out-of-pool allocation fails with A0 ---------------------------- */
    ok = xms_alloc(&x, 2048, &h2, &err);
    CHECK(!ok && err == XMSERR_NOMEM, "fn09: over-pool alloc fails (A0)");

    /* T5: a second, smaller block, plus a 0-KB block ---------------------- */
    ok = xms_alloc(&x, 128, &h2, &err);
    CHECK(ok && h2 == 2, "fn09: alloc 128KB -> handle 2");
    ok = xms_alloc(&x, 0, &h3, &err);
    CHECK(ok && h3 == 3, "fn09: alloc 0KB -> legal empty handle 3");
    xms_query_free(&x, &largest, &totfree);
    CHECK(totfree == 1024 - 64 - 128, "fn08: pool accounting after 3 allocs");

    /* T6: Move conventional -> EMB, then EMB -> conventional (round trip) -- */
    {
        static uint8_t conv[0x20000];           /* 128KB conventional window    */
        xms_move_t mv;
        const char *msg = "XMS round trip!";
        uint32_t seg = 0x1000, off = 0x0010;    /* conv far ptr 1000:0010       */
        uint8_t *cp = conv + (seg << 4) + off;
        memset(conv, 0, sizeof conv);
        memcpy(cp, msg, 16);

        /* conv 1000:0010  ->  handle 1 offset 0 */
        mv.length = 16; mv.src_handle = 0; mv.src_offset = (seg << 16) | off;
        mv.dst_handle = h1; mv.dst_offset = 0;
        ok = xms_move(&x, conv, &mv, &err);
        CHECK(ok, "fn0B: move conv -> EMB");
        CHECK(memcmp(x.h[h1 - 1].mem, msg, 16) == 0, "fn0B: EMB holds the moved bytes");

        /* handle 1 offset 0  ->  conv 2000:0000 */
        memset(conv, 0, sizeof conv);
        mv.length = 16; mv.src_handle = h1; mv.src_offset = 0;
        mv.dst_handle = 0; mv.dst_offset = (0x2000u << 16) | 0x0000u;
        ok = xms_move(&x, conv, &mv, &err);
        CHECK(ok && memcmp(conv + (0x2000u << 4), msg, 16) == 0, "fn0B: move EMB -> conv (round trip)");
    }

    /* T7: Move EMB -> EMB ------------------------------------------------- */
    {
        xms_move_t mv;
        mv.length = 16; mv.src_handle = h1; mv.src_offset = 0;
        mv.dst_handle = h2; mv.dst_offset = 1024;   /* into the middle of h2    */
        ok = xms_move(&x, NULL, &mv, &err);
        CHECK(ok && memcmp((uint8_t *)x.h[h2 - 1].mem + 1024, x.h[h1 - 1].mem, 16) == 0,
              "fn0B: move EMB -> EMB");
    }

    /* T8: Move error paths ------------------------------------------------ */
    {
        xms_move_t mv;
        mv.length = 15; mv.src_handle = h1; mv.src_offset = 0; mv.dst_handle = h2; mv.dst_offset = 0;
        CHECK(!xms_move(&x, NULL, &mv, &err) && err == XMSERR_BADLEN, "fn0B: odd length rejected (A7)");
        mv.length = 16; mv.src_handle = 99;
        CHECK(!xms_move(&x, NULL, &mv, &err) && err == XMSERR_BADSRCH, "fn0B: bad source handle (A3)");
        mv.src_handle = h1; mv.src_offset = 64u * 1024u;   /* past end of 64KB block */
        CHECK(!xms_move(&x, NULL, &mv, &err) && err == XMSERR_BADSRCO, "fn0B: source offset past end (A4)");
        mv.length = 0; mv.src_handle = h1; mv.src_offset = 0;
        CHECK(xms_move(&x, NULL, &mv, &err), "fn0B: zero-length move is a no-op (ok)");
    }

    /* T9: lock / unlock + free-while-locked ------------------------------- */
    ok = xms_lock(&x, h1, &lin1, &err);
    CHECK(ok && lin1 != 0, "fn0C: lock handle 1 -> nonzero linear addr");
    ok = xms_lock(&x, h2, &lin2, &err);
    CHECK(ok && lin2 != lin1, "fn0C: distinct blocks lock to distinct addrs");
    CHECK(!xms_free(&x, h1, &err) && err == XMSERR_LOCKED, "fn0A: free-while-locked rejected (AB)");
    ok = xms_info(&x, h1, &lock, &freeh, &size_kb, &err);
    CHECK(ok && lock == 1, "fn0E: lock count == 1");
    CHECK(xms_unlock(&x, h1, &err), "fn0D: unlock handle 1");
    CHECK(!xms_unlock(&x, h1, &err) && err == XMSERR_NOTLOCKED, "fn0D: over-unlock rejected (AA)");

    /* T10: realloc grow (preserve) + shrink ------------------------------- */
    {
        uint8_t *m = (uint8_t *)x.h[h1 - 1].mem;
        m[0] = 0xAB; m[64u * 1024u - 1] = 0xCD;     /* mark first+last byte     */
        ok = xms_realloc(&x, h1, 128, &err);        /* grow 64 -> 128 KB        */
        CHECK(ok && x.h[h1 - 1].size_kb == 128, "fn0F: grow 64->128KB");
        m = (uint8_t *)x.h[h1 - 1].mem;
        CHECK(m[0] == 0xAB && m[64u * 1024u - 1] == 0xCD, "fn0F: grow preserves old content");
        ok = xms_realloc(&x, h1, 16, &err);         /* shrink to 16 KB          */
        CHECK(ok && x.h[h1 - 1].size_kb == 16, "fn0F: shrink 128->16KB");
        xms_query_free(&x, &largest, &totfree);
        CHECK(totfree == 1024 - 16 - 128, "fn0F: pool accounting after realloc");
    }

    /* T11: free both, pool fully restored --------------------------------- */
    CHECK(xms_unlock(&x, h2, &err), "fn0D: unlock handle 2 (locked back in T9)");
    CHECK(xms_free(&x, h1, &err), "fn0A: free handle 1");
    CHECK(xms_free(&x, h2, &err), "fn0A: free handle 2");
    CHECK(xms_free(&x, h3, &err), "fn0A: free handle 3 (the 0KB block)");
    CHECK(!xms_free(&x, h1, &err) && err == XMSERR_BADHANDLE, "fn0A: double-free rejected (A2)");
    xms_query_free(&x, &largest, &totfree);
    CHECK(totfree == 1024, "fn08: whole pool free again after frees");

    /* T12: handle exhaustion --------------------------------------------- */
    {
        int got = 0;
        for (i = 0; i < XMS_MAX_HANDLES + 4; ++i) {
            uint16_t hh;
            if (xms_alloc(&x, 0, &hh, &err)) ++got;
            else { CHECK(err == XMSERR_NOHANDLES, "fn09: exhausting handles fails (A1)"); break; }
        }
        CHECK(got == XMS_MAX_HANDLES, "fn09: exactly XMS_MAX_HANDLES 0KB blocks allocatable");
    }

    printf("\n%d checks, %d failed\n", total, fails);
    return fails ? 1 : 0;
}
