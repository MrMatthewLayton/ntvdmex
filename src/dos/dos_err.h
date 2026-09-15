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

/* ── WIN32 -> DOS, FOR EVERY CALL THAT FAILS THROUGH CreateFileA. (s72) ───────
   AH=3Dh answered **2 ("file not found") for every possible failure**, because
   the handler read `f == INVALID_HANDLE_VALUE` and stopped asking. Measured by
   tools/dostest/p_err.asm against MS-DOS 6.22, that is wrong twice over:

     CASE=err.after.3D.readonly  oracle AX=0005  ours AX=0002   access denied
     CASE=err.after.3D.baddrive  oracle AX=0003  ours AX=0002   path not found

   and it is not cosmetic -- a program that gets "not found" for a file that is
   plainly there goes looking for it instead of reporting the real problem. The
   note beside the 0x0F row above records the same trap from the other side: the
   DOS answer for opening "Y:\..." is 3, NOT 15, so this table must not "improve"
   on it. The comment at AH=3Ch/3Dh already blamed this collapse for the GDI.EXE
   wall; the sharing half was fixed then and the mapping half was left.

   ⚠ BOTH SIDES OF EVERY ROW ARE MEASURED. The DOS side is the oracle CASE= line
     quoted beside it; the WIN32 side is what the rig actually reported, read off
     the handler's own `win32=0x..` log during the run that closed these rows:

       INT21 AH=3d [ZZNOSUCH.XYZ]    FAILED win32=0x2 -> AX=0x2
       INT21 AH=3d [ZZRDONLY.TMP]    FAILED win32=0x5 -> AX=0x5
       INT21 AH=3d [Y:\ZZNOSUCH.XYZ] FAILED win32=0x3 -> AX=0x3

     An unmapped code is logged `UNMAPPED` and keeps the old answer rather than
     being collapsed silently, exactly as dos_err_classify() refuses to invent a
     class.
   ⚠ THERE IS DELIBERATELY NO ERROR_INVALID_DRIVE (15) ROW. The obvious guess is
     that "Y:\..." arrives as 15 and should map to DOS 3 -- but measured, it
     arrives as **win32=3**, so a 15 row would be an unexercised invention
     dressed as evidence. If a door is ever found that does produce 15, provoke
     it and add the row then, in that order.

   Numeric rather than the ERROR_* macros so this header stays free of
   windows.h and tools/dostest/err_test.c can keep pinning it off-VM. */
#define W32_FILE_NOT_FOUND      2u
#define W32_PATH_NOT_FOUND      3u
#define W32_TOO_MANY_OPEN       4u
#define W32_ACCESS_DENIED       5u

static int dos_err_from_win32(unsigned long e, unsigned short *dos)
{
    switch (e) {
    /* ── MEASURED, both sides. See the log lines quoted above. ─────────────── */
    case W32_FILE_NOT_FOUND: *dos = 0x02; return 1;  /* err.after.3D.missing  AX=0002 */
    /* Also the bad-drive door: "Y:\..." arrives here as 3, not 15. */
    case W32_PATH_NOT_FOUND: *dos = 0x03; return 1;  /* err.after.3D.baddrive AX=0003 */
    case W32_ACCESS_DENIED:  *dos = 0x05; return 1;  /* err.after.3D.readonly AX=0005 */
    /* ── AN IDENTITY, NOT A MEASUREMENT, AND LABELLED AS SUCH. DOS error 4 IS
         "too many open files" and the handler already answers 4 when it runs out
         of its own slots, so the two names denote one condition. NOT provoked by
         a probe: to promote it, extend p_err.asm to exhaust the handle table. */
    case W32_TOO_MANY_OPEN:  *dos = 0x04; return 1;
    default: *dos = 0x02; return 0;                  /* caller logs win32= and keeps 2 */
    }
}

#endif /* DOS_ERR_H */
