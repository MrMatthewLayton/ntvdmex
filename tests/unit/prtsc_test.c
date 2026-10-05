/* prtsc_test.c -- off-VM battery for the default INT 05h's byte sequencer (#274).
 *
 * src/dos/bios_prtsc.h decides which byte the guest's p5 loop (bios_kbdact.asm) prints
 * next through INT 17h, and whether INT 17h's answer ends the job. Driven here with a
 * fake screen and a fake printer status.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "bios_prtsc.h"

static int total = 0, fails = 0;
#define CHECK(c,m) do{ total++; if(c){printf("  PASS  %s\n",(m));} \
    else{printf("  FAIL  %s\n",(m)); fails++;} }while(0)

static uint8_t screen[50][132];
static int reads;
static uint8_t readc(void *ctx, uint8_t row, uint8_t col)
{ (void)ctx; ++reads; return screen[row][col]; }

/* Run a whole job; the printer answers `ah_ok` for every byte except byte number
   `fail_at` (1-based, 0 = never), which gets `ah_bad`. Returns the final rc. */
static uint8_t out[8000];
static int nout;
static int job(bios_prtsc *s, uint8_t cols, uint8_t rows84, int fail_at, uint8_t ah_bad)
{
    uint8_t ch, ah = 0;
    int rc;
    nout = 0; reads = 0;
    bios_prtsc_begin(s, cols, rows84, 0, 0x1234);
    for (;;) {
        rc = bios_prtsc_step(s, ah, readc, 0, &ch);
        if (rc != PRTSC_EMIT) return rc;
        if (nout < (int)sizeof out) out[nout] = ch;
        ++nout;
        ah = (fail_at && nout == fail_at) ? ah_bad : 0x90;   /* 90h: selected, not busy */
    }
}

int main(void)
{
    bios_prtsc s;
    int rc, r, c, ok;
    printf("== prtsc_test: the default INT 05h (#274) ==\n");
    for (r = 0; r < 50; ++r) for (c = 0; c < 132; ++c) screen[r][c] = (uint8_t)('A' + (r + c) % 26);
    screen[0][0] = 0;                                     /* a NUL cell */

    rc = job(&s, 80, 24, 0, 0);
    CHECK(rc == PRTSC_DONE && !s.active, "80x25: the job ends DONE and inactive");
    CHECK(nout == 2 + 25 * (80 + 2), "80x25: 2 + 25 x (80 + 2) = 2052 bytes through INT 17h");
    CHECK(out[0] == 0x0A && out[1] == 0x0D, "the initial line end is LF, CR (IBM listing, as recalled)");
    CHECK(out[2] == ' ', "a NUL cell prints as a space");
    CHECK(out[3] == screen[0][1] && out[81] == screen[0][79], "row 0, columns 1 and 79 in order");
    CHECK(out[82] == 0x0A && out[83] == 0x0D && out[84] == screen[1][0], "each row ends LF, CR; row 1 follows");
    CHECK(reads == 80 * 25, "one screen read per cell");
    CHECK(s.cursor == 0x1234, "the saved cursor is kept for the host to put back");

    rc = job(&s, 80, 0, 0, 0);
    CHECK(rc == PRTSC_DONE && nout == 2 + 25 * 82, "0040:0084 = 0 (never set) -> 25 rows");
    rc = job(&s, 132, 49, 0, 0);
    CHECK(rc == PRTSC_DONE && nout == 2 + 50 * 134, "132x50: columns from AH=0Fh, rows from 0040:0084 + 1");

    rc = job(&s, 80, 24, 3, 0x01);                        /* time-out on the 1st cell */
    CHECK(rc == PRTSC_ERROR && !s.active && nout == 3, "a time-out (AH bit 0) on a cell ends the job: ERROR, no more bytes");
    rc = job(&s, 80, 24, 50, 0x08);
    CHECK(rc == PRTSC_ERROR && nout == 50, "an I/O error (AH bit 3) ends it too");
    rc = job(&s, 80, 24, 100, 0x20);
    CHECK(rc == PRTSC_ERROR && nout == 100, "out of paper (AH bit 5) ends it too");
    rc = job(&s, 80, 24, 100, 0xD6);                      /* every bit but 29h */
    CHECK(rc == PRTSC_DONE && nout == 2052, "AH bits outside 29h (busy, ack, selected...) are not errors");
    rc = job(&s, 80, 24, 1, 0x29);                        /* the initial LF fails */
    CHECK(rc == PRTSC_DONE && nout == 2052, "a line-end byte's status is not tested (IBM tests screen bytes only)");
    rc = job(&s, 80, 24, 82, 0x29);                       /* the LAST cell of row 0 */
    CHECK(rc == PRTSC_ERROR && nout == 82, "...but the last cell before a line end is");

    ok = 1;
    memset(&s, 0, sizeof s);
    { uint8_t ch; ok = bios_prtsc_step(&s, 0, readc, 0, &ch) == PRTSC_DONE; }
    CHECK(ok, "a step with no job running says DONE");
    rc = job(&s, 0, 24, 0, 0);
    CHECK(rc == PRTSC_DONE && nout == 2, "0 columns: just the initial line end");

    printf("\n%d checks, %d failed\n", total, fails);
    return fails ? 1 : 0;
}
