/* dos_err.h -- INT 21h AH=59h: what DOS makes of a failure, as a measured table.
 *
 * 59h reports a failure as FOUR things, not one:
 *
 *     AX  the extended error code        (the obvious part)
 *     BH  the error CLASS                what kind of problem this is
 *     BL  the suggested ACTION           what the program should do about it
 *     CH  the LOCUS                      where the problem is
 *     CL  ── NOT WRITTEN BY DOS ──       measured: comes back still poisoned
 *
 * Only the code is obvious, and class/action/locus are exactly the sort of value
 * that gets written from memory and is wrong. So this table contains ONLY
 * pairings provoked on the genuine MS-DOS 6.22 oracle by tools/dostest/p_err.asm,
 * and each row cites the CASE= line it came from. GH #34, epic #24.
 *
 * ⚠ THE UNMEASURED ROW IS NOT A GAP TO BE FILLED IN BY GUESSWORK. A code we have
 *   not provoked returns class/action/locus ZERO and the handler logs
 *   "UNMEASURED", because a plausible-looking class is worse than an absent one:
 *   a program that branches on it takes a wrong branch silently, which is the
 *   failure class GH #27 exists to remove. To add a row, extend p_err.asm, run it
 *   on the oracle, and paste what came back -- in that order.
 *
 * Pure -- no Windows types -- so tools/dostest/err_test.c can pin it off-VM.
 */
#ifndef DOS_ERR_H
#define DOS_ERR_H

/* DOS error classes (BH) and suggested actions (BL), as returned by 6.22. The
   names are RBIL's; the VALUES here are only ever the measured ones. */
#define DOS_ECLASS_OUTOFRES   0x01
#define DOS_ECLASS_AUTHZ      0x03      /* access denied family                 */
#define DOS_ECLASS_BADFMT     0x07      /* bad request / invalid parameter      */
#define DOS_ECLASS_NOTFOUND   0x08
#define DOS_ECLASS_ALREADY    0x0C      /* already exists                       */

#define DOS_EACTION_RETRY     0x01
#define DOS_EACTION_ABORT     0x04
#define DOS_EACTION_USER      0x03      /* retry after user intervention        */

#define DOS_ELOCUS_UNKNOWN    0x00
#define DOS_ELOCUS_BLOCKDEV   0x01
#define DOS_ELOCUS_NETWORK    0x02      /* ...and, measured, the file system    */

/* One measured row. `bx` is BH:BL packed as DOS returns it, `ch` the locus. */
typedef struct {
    unsigned short code;
    unsigned short bx;
    unsigned char  ch;
    const char    *evidence;
} dos_err_row_t;

/* ── EVERY ROW IS A LINE OF ORACLE OUTPUT. ────────────────────────────────────
   tools/dostest/p_err.asm on MS-DOS 6.22, verbatim:

     CASE=err.after.3D.missing   AX=0002 BX=0803 CX=02C1     file not found
     CASE=err.after.4E.nopath    AX=0003 BX=0803 CX=02C1     path not found
     CASE=err.after.4E.nofile    AX=0012 BX=0803 CX=02C1     no more files
     CASE=err.after.3F.badhandle AX=0006 BX=0704 CX=01C1     invalid handle
     CASE=err.after.3D.readonly  AX=0005 BX=0303 CX=02C1     access denied
     CASE=err.after.5B.exists    AX=0050 BX=0C03 CX=02C1     file exists

   Note CX: CH is the locus and CL comes back holding the probe's own poison
   (0xC1) in every single row -- so DOS does not write CL, and neither do we. */
static const dos_err_row_t dos_err_table[] = {
    { 0x02, 0x0803, 0x02, "err.after.3D.missing"   },
    { 0x03, 0x0803, 0x02, "err.after.4E.nopath"    },
    { 0x12, 0x0803, 0x02, "err.after.4E.nofile"    },
    { 0x06, 0x0704, 0x01, "err.after.3F.badhandle" },
    { 0x05, 0x0303, 0x02, "err.after.3D.readonly"  },
    { 0x50, 0x0C03, 0x02, "err.after.5B.exists"    },
    /* Invalid drive. ⚠ IT NEEDED A DIFFERENT DOOR: opening "Y:\..." returns 3
       (path not found), not 15, so the code only turns up through AH=47h asking
       for the current directory of a drive with nothing behind it. Provoked, not
       reasoned about -- err.after.47.baddrive AX=000F BX=0803 CX=02C1. */
    { 0x0F, 0x0803, 0x02, "err.after.47.baddrive"  },
};
#define DOS_ERR_ROWS (sizeof(dos_err_table) / sizeof(dos_err_table[0]))

/* Look up a code. Returns 1 and fills bx/ch when the pairing was measured;
   returns 0 and zeroes them when it was not -- see the warning at the top for
   why zero rather than a plausible guess. Code 0 (no error) is not a row: 59h
   after a successful call reports AX=0 with class and locus zero, which is what
   the not-found path already produces. */
static int dos_err_classify(unsigned short code, unsigned short *bx, unsigned char *ch)
{
    unsigned i;
    *bx = 0; *ch = 0;
    if (!code) return 1;                        /* no error: zeroes are correct */
    for (i = 0; i < DOS_ERR_ROWS; ++i) {
        if (dos_err_table[i].code != code) continue;
        *bx = dos_err_table[i].bx;
        *ch = dos_err_table[i].ch;
        return 1;
    }
    return 0;                                   /* caller logs UNMEASURED       */
}

#endif /* DOS_ERR_H */
