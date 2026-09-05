/* fh_test.c -- the DOS handle table's two rules, pinned off-VM.  GH #133, #131.
 *
 * Every expectation here is a line from tools/dostest/p_redir.asm run on the
 * genuine MS-DOS 6.22 oracle, quoted in the check's name.  None of it is written
 * from memory of what DOS does -- that is the cardinal rule of epic #24, and the
 * reason this file cites the CASE= line it came from.
 *
 *   cc -std=c99 -I src/dos -o fh_test tools/dostest/fh_test.c && ./fh_test
 */
#include <stdio.h>
#include <string.h>
#include "dos_fh.h"

static int checks, fails;

static void eq(const char *what, long got, long want)
{
    ++checks;
    if (got == want) return;
    ++fails;
    printf("  FAIL %-52s got %ld, want %ld\n", what, got, want);
}

/* A table in the state dos_int21_init leaves it: nothing bound, all five
   standard handles open as devices. */
static void std_table(void *fh[DOS_MAX_FILES], unsigned *std_open)
{
    memset(fh, 0, sizeof(void *) * DOS_MAX_FILES);
    *std_open = 0x1F;                       /* bits 0-4: stdin..prn */
}

/* Any non-NULL value stands for "bound to a Win32 handle". */
static void *const BOUND = (void *)(size_t)0xF11E;

int main(void)
{
    void *fh[DOS_MAX_FILES];
    unsigned std_open;
    unsigned i;

    printf("== DOS handle table (dos_fh.h) -- rules measured on MS-DOS 6.22\n");

    /* ── RULE 1: ALLOCATION IS THE LOWEST FREE SLOT ─────────────────────────
       Oracle: CASE=int21.3c.baseline SIG=AX,CF AX=0005 CF=0
       With stdin/stdout/stderr/aux/prn open, a create gets 5. */
    std_table(fh, &std_open);
    eq("3Ch baseline, five devices open -> 5", dos_fh_alloc(fh, std_open), 5);

    /* Oracle: CASE=int21.3c.after.close1 SIG=AX,CF AX=0001 CF=0
       ★ THE FACT #133 was missing. Close handle 1 and the next create lands in
       the slot stdout just vacated -- which is the whole of `>`. A loop that
       starts at 5 cannot produce this, and that is what made the text go to the
       screen while the file stayed 0 bytes. */
    std_table(fh, &std_open);
    std_open &= ~(1u << 1);                 /* the shell closed stdout */
    eq("3Ch after close(1) -> 1  [the redirect]", dos_fh_alloc(fh, std_open), 1);

    /* Input redirection is the same rule on the other end. */
    std_table(fh, &std_open);
    std_open &= ~(1u << 0);
    eq("3Dh after close(0) -> 0  [`< file`]", dos_fh_alloc(fh, std_open), 0);

    /* A closed-and-refilled slot is not free any more: the SECOND create must
       move on to 5 rather than handing out handle 1 twice. Getting this wrong
       would alias the redirect target and whatever the command opens next. */
    std_table(fh, &std_open);
    std_open &= ~(1u << 1);
    fh[1] = BOUND;                          /* the target took slot 1 */
    eq("second create with 1 taken -> 5", dos_fh_alloc(fh, std_open), 5);

    /* Ordinary allocation above the standard handles still packs downwards into
       any hole a close left, which is what DOS does and what programs that
       count on handle numbers rely on. */
    std_table(fh, &std_open);
    fh[5] = BOUND; fh[6] = BOUND; fh[7] = BOUND;
    eq("5,6,7 bound -> 8", dos_fh_alloc(fh, std_open), 8);
    fh[6] = 0;                              /* close the middle one */
    eq("...then close 6 -> 6 (fills the hole)", dos_fh_alloc(fh, std_open), 6);

    /* Full table reports DOS_MAX_FILES so the caller can raise error 4 rather
       than index off the end. */
    std_table(fh, &std_open);
    std_open = 0;
    for (i = 0; i < DOS_MAX_FILES; ++i) fh[i] = BOUND;
    eq("full table -> DOS_MAX_FILES", dos_fh_alloc(fh, std_open), DOS_MAX_FILES);

    /* ── RULE 2: A BOUND HANDLE IS A FILE, WHATEVER ITS NUMBER ──────────────
       Oracle: CASE=int21.42.end.on.h1 SIG=AX,DX,CF AX=0004 DX=0000 CF=0
       Real DOS seeks handle 1 to end-of-file and reports 4 bytes. AH=42h used
       to guard with `h >= 5` and refused this with error 6 -- the last
       survivor of #133, and the reason `>>` could not find end-of-file. */
    std_table(fh, &std_open);
    std_open &= ~(1u << 1);
    fh[1] = BOUND;
    eq("bound handle 1 IS a file (42h/40h/3Fh/3Eh)", dos_fh_is_file(fh, 1), 1);
    eq("bound handle 1 is NOT a device", dos_fh_is_device(fh, std_open, 1), 0);

    /* ...and the ordinary case is unchanged: an UNBOUND low handle is the
       console, which is what makes a non-redirected program print. */
    std_table(fh, &std_open);
    eq("unbound handle 1 is a device", dos_fh_is_device(fh, std_open, 1), 1);
    eq("unbound handle 1 is NOT a file", dos_fh_is_file(fh, 1), 0);
    eq("unbound handle 2 (stderr) is a device", dos_fh_is_device(fh, std_open, 2), 1);

    /* A closed standard handle is neither: not a device any more, not yet a
       file. It is simply free, which is what makes rule 1 hand it out. */
    std_table(fh, &std_open);
    std_open &= ~(1u << 1);
    eq("closed handle 1 is not a device", dos_fh_is_device(fh, std_open, 1), 0);
    eq("closed handle 1 is not a file", dos_fh_is_file(fh, 1), 0);

    /* ── RULE 3: A DEVICE IS DUPLICABLE, AND NOT ONLY INTO SLOTS 0-4. ───────
       Oracle: CASE=int21.45.dup.stdout SIG=CF AX=0005 CF=0
       Real DOS duplicates the console into slot 5, which is then ALSO the
       console. NTVDMEX returned AX=0006 CF=1 on the rig because a device could
       only be represented below slot 5 -- so `dup(1)`, the first step of every
       save-redirect-restore a shell performs, was impossible.
       ★ The wrong answer ATE ITS OWN EVIDENCE: with stdout never restored, the
       probe's remaining output went into the file under test instead of the
       dump. Hence the rule gets its own checks here. */
    std_table(fh, &std_open);
    eq("dup(1): the copy lands in slot 5", dos_fh_alloc(fh, std_open), 5);
    eq("...and slot 5 can be marked a device", dos_fh_set_device(&std_open, 5, 1), 1);
    eq("...so slot 5 IS a device", dos_fh_is_device(fh, std_open, 5), 1);
    eq("...and is NOT a file (nothing to WriteFile to)", dos_fh_is_file(fh, 5), 0);
    eq("...and is not handed out again", dos_fh_alloc(fh, std_open), 6);

    /* The restore: dup2(saved, 1) puts the device back on handle 1. */
    std_open &= ~(1u << 1);                 /* the shell had closed stdout */
    eq("before restore, 1 is free", dos_fh_alloc(fh, std_open), 1);
    dos_fh_set_device(&std_open, 1, 1);     /* dup2(5, 1) */
    eq("after dup2(5,1), handle 1 is the console again",
       dos_fh_is_device(fh, std_open, 1), 1);

    /* Past the mask the answer must be REFUSED, not rounded: a device slot that
       silently read as a file is the "runs but lies" class. */
    std_table(fh, &std_open);
    eq("device mark at slot 31 succeeds", dos_fh_set_device(&std_open, 31, 1), 1);
    eq("device mark at slot 32 is REFUSED", dos_fh_set_device(&std_open, 32, 1), 0);
    eq("...and slot 32 is therefore not a device", dos_fh_is_device(fh, std_open, 32), 0);

    /* Bounds: a guest can put anything in BX, so both predicates take a raw
       value and must not index off the end. */
    std_table(fh, &std_open);
    eq("h = DOS_MAX_FILES is not a file", dos_fh_is_file(fh, DOS_MAX_FILES), 0);
    eq("h = 0xFFFF is not a file", dos_fh_is_file(fh, 0xFFFF), 0);
    eq("h = 0xFFFF is not a device", dos_fh_is_device(fh, std_open, 0xFFFF), 0);

    printf("== %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
