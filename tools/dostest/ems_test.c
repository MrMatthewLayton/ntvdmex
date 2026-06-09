/* ems_test.c -- off-VM unit battery for the EMS core (src/dos/dos_ems.h).
 *
 * Layer-1 test for M4 slice 2, in the style of xms_test.c. A 64 KB buffer
 * stands in for the page-frame window (the host maps real 0xE0000 RAM there);
 * handle backing pages are malloc'd. The key thing under test is page-frame
 * *shadowing*: mapping a logical page memcpys it into a physical window, the
 * "guest" writes the window directly, and remapping must write those changes
 * back to the logical page so they survive -- this is how EMS works without a
 * memory trap. Also covers counts/alloc/dealloc, realloc grow/shrink, the
 * logical/physical range errors, and save/restore page maps.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dos_ems.h"

static int total = 0, fails = 0;
#define CHECK(c,m) do{ total++; if(c){printf("  PASS  %s\n",(m));} \
    else{printf("  FAIL  %s\n",(m)); fails++;} }while(0)

static void *t_alloc(void *ctx, uint32_t pages) { (void)ctx; return calloc((size_t)pages, EMS_PAGE_SIZE); }
static void  t_free (void *ctx, void *p, uint32_t pages) { (void)ctx; (void)pages; free(p); }

/* a byte the guest would write straight into a physical window */
static void poke_window(ems_state *e, int phys, uint32_t off, uint8_t v) {
    e->frame[(uint32_t)phys * EMS_PAGE_SIZE + off] = v;
}
static uint8_t peek_window(ems_state *e, int phys, uint32_t off) {
    return e->frame[(uint32_t)phys * EMS_PAGE_SIZE + off];
}

int main(void)
{
    static uint8_t frame[EMS_FRAME_SIZE];   /* the 64 KB page-frame window       */
    ems_state e;
    uint16_t h1, h2, freep, totp, pages;
    uint8_t err;
    int ok;

    printf("== M4 EMS core battery ==\n");

    /* 256-page pool (4 MB) framed at E000:0. */
    memset(frame, 0, sizeof frame);
    ems_init(&e, 0xE000, 256, frame, t_alloc, t_free, NULL);

    /* T1: bring-up + counts ---------------------------------------------- */
    CHECK(e.frame_seg == 0xE000, "init: page frame at E000");
    ems_counts(&e, &freep, &totp);
    CHECK(freep == 256 && totp == 256, "fn42: 256 free / 256 total initially");

    /* T2: zero-page alloc is rejected (fn 43h) --------------------------- */
    CHECK(!ems_alloc(&e, 0, &h1, &err) && err == EMSERR_ZEROPAGES, "fn43: zero pages rejected (89h)");

    /* T3: allocate 8 pages ----------------------------------------------- */
    ok = ems_alloc(&e, 8, &h1, &err);
    CHECK(ok && err == EMS_OK, "fn43: allocate 8 pages -> handle");
    ems_counts(&e, &freep, &totp);
    CHECK(freep == 248, "fn42: 248 free after 8-page alloc");
    ok = ems_handle_pages(&e, h1, &pages, &err);
    CHECK(ok && pages == 8, "fn4C: handle owns 8 pages");
    CHECK(ems_handle_count(&e) == 1, "fn4B: 1 open handle");

    /* T4: over-pool alloc fails ------------------------------------------ */
    CHECK(!ems_alloc(&e, 1000, &h2, &err) && err == EMSERR_TOOMANY, "fn43: > pool rejected (87h)");
    ok = ems_alloc(&e, 250, &h2, &err);
    CHECK(!ok && err == EMSERR_NOTENOUGH, "fn43: not-enough-free rejected (88h)");

    /* T5: map errors ----------------------------------------------------- */
    CHECK(!ems_map(&e, 4, 0, h1, &err) && err == EMSERR_BADPHYS, "fn44: physical window >3 rejected (8Bh)");
    CHECK(!ems_map(&e, 0, 8, h1, &err) && err == EMSERR_BADLOGICAL, "fn44: logical page >= owned rejected (8Ah)");
    CHECK(!ems_map(&e, 0, 0, 99, &err) && err == EMSERR_BADHANDLE, "fn44: bad handle rejected (83h)");

    /* T6: SHADOWING -- map page 0 into window 0, write it, remap, verify it  *
     * was written back to the logical page (the heart of EMS).            */
    ok = ems_map(&e, 0, 0, h1, &err);
    CHECK(ok, "fn44: map logical 0 -> window 0");
    poke_window(&e, 0, 0x0000, 0xA1);           /* guest writes the window     */
    poke_window(&e, 0, 0x3FFF, 0xA2);
    ok = ems_map(&e, 0, 1, h1, &err);           /* swap in logical page 1      */
    CHECK(ok, "fn44: map logical 1 -> window 0 (writes back page 0)");
    poke_window(&e, 0, 0x0000, 0xB1);           /* mark page 1                 */
    ok = ems_map(&e, 0, 0, h1, &err);           /* bring page 0 back           */
    CHECK(ok && peek_window(&e, 0, 0x0000) == 0xA1 && peek_window(&e, 0, 0x3FFF) == 0xA2,
          "fn44: page 0 content survived the swap (shadow write-back)");
    ok = ems_map(&e, 0, 1, h1, &err);           /* and page 1's mark survived  */
    CHECK(ok && peek_window(&e, 0, 0x0000) == 0xB1, "fn44: page 1 content also survived");

    /* T7: two pages live in two windows at once -------------------------- */
    ems_map(&e, 1, 2, h1, &err);
    poke_window(&e, 1, 0x10, 0xC3);
    ems_map(&e, 2, 3, h1, &err);
    poke_window(&e, 2, 0x10, 0xD4);
    CHECK(peek_window(&e, 1, 0x10) == 0xC3 && peek_window(&e, 2, 0x10) == 0xD4,
          "fn44: independent windows hold independent pages");

    /* T8: unmap a window (logical 0xFFFF) writes back + clears ----------- */
    ok = ems_map(&e, 1, 0xFFFF, h1, &err);
    CHECK(ok && !e.phys[1].mapped, "fn44: logical 0xFFFF unmaps the window");

    /* T9: save / restore the page map ------------------------------------ */
    ok = ems_save_map(&e, h1, &err);
    CHECK(ok, "fn47: save page map");
    CHECK(!ems_save_map(&e, h1, &err) && err == EMSERR_SAVED, "fn47: double-save rejected (8Dh)");
    ems_map(&e, 0, 4, h1, &err);                /* perturb the mapping         */
    ems_map(&e, 2, 5, h1, &err);
    ok = ems_restore_map(&e, h1, &err);
    CHECK(ok && e.phys[0].logical == 1 && e.phys[2].logical == 3,
          "fn48: restore brings back the saved windows");
    CHECK(!ems_restore_map(&e, h1, &err) && err == EMSERR_NOTSAVED, "fn48: restore-without-save rejected (8Eh)");

    /* T10: realloc grow preserves content -------------------------------- */
    {
        uint8_t *m;
        ems_map(&e, 0, 0, h1, &err);
        poke_window(&e, 0, 0x20, 0x5A);         /* dirty page 0 via the window */
        ok = ems_realloc(&e, h1, 16, &err);     /* 8 -> 16 pages (flush+grow)  */
        CHECK(ok && e.h[h1].pages == 16, "fn51: grow 8->16 pages");
        m = (uint8_t *)e.h[h1].mem;
        CHECK(m[0x20] == 0x5A, "fn51: grow preserved the written-back page 0");
        ems_counts(&e, &freep, &totp);
        CHECK(freep == 240, "fn42: 240 free after grow to 16");
        ok = ems_realloc(&e, h1, 4, &err);      /* shrink to 4 pages           */
        CHECK(ok && e.h[h1].pages == 4, "fn51: shrink 16->4 pages");
    }

    /* T11: free the handle, pool restored, windows dropped --------------- */
    ems_map(&e, 0, 0, h1, &err);                /* a live window before free   */
    ok = ems_free(&e, h1, &err);
    CHECK(ok && !e.phys[0].mapped, "fn45: dealloc drops the handle's live windows");
    ems_counts(&e, &freep, &totp);
    CHECK(freep == 256, "fn42: whole pool free again after dealloc");
    CHECK(!ems_free(&e, h1, &err) && err == EMSERR_BADHANDLE, "fn45: double-free rejected (83h)");
    CHECK(ems_handle_count(&e) == 0, "fn4B: 0 open handles at end");

    printf("\n%d checks, %d failed\n", total, fails);
    return fails ? 1 : 0;
}
