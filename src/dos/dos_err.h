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
 * pairings provoked on the genuine MS-DOS 6.22 oracle by tests/probes/dos/p_err.asm,
 * and each row cites the CASE= line it came from. GH #34, epic #24.
 *
 * ⚠ THE UNMEASURED ROW IS NOT A GAP TO BE FILLED IN BY GUESSWORK. A code we have
 *   not provoked returns class/action/locus ZERO and the handler logs
 *   "UNMEASURED", because a plausible-looking class is worse than an absent one:
 *   a program that branches on it takes a wrong branch silently, which is the
 *   failure class GH #27 exists to remove. To add a row, extend p_err.asm, run it
 *   on the oracle, and paste what came back -- in that order.
 *
 * No Windows calls, only Windows types (src/ntvdmex_types.h), so tests/unit/err_test.c
 * can pin it off-VM.
 */
#ifndef NTVDMEX_DOS_ERR_H
#define NTVDMEX_DOS_ERR_H

#include "../ntvdmex_types.h"

/* DOS error classes (BH) and suggested actions (BL), as returned by 6.22. The
   names are RBIL's; the VALUES here are only ever the measured ones. */
#define DOS_ERR_CLASS_OUT_OF_RESOURCE   0x01
#define DOS_ERR_CLASS_AUTHORIZATION     0x03      /* access denied family                 */
#define DOS_ERR_CLASS_BAD_FORMAT        0x07      /* bad request / invalid parameter      */
#define DOS_ERR_CLASS_NOT_FOUND         0x08
#define DOS_ERR_CLASS_ALREADY_EXISTS    0x0C      /* already exists                       */
#define DOS_ERR_CLASS_FAIL_I24          0x0D      /* after an INT 24h answered FAIL       */

#define DOS_ERR_ACTION_RETRY            0x01
#define DOS_ERR_ACTION_ABORT            0x04
#define DOS_ERR_ACTION_USER_INTERVENTION 0x03     /* retry after user intervention        */

/* Loci (CH), named as RBIL's AH=59h table names them: 01h unknown or not appropriate,
   02h block device. (These were once named one place off -- 01h "block device", 02h
   "network" -- which is why file-system errors seemed to report "network".) */
#define DOS_ERR_LOCUS_UNMEASURED        0x00      /* the unmeasured row's zero            */
#define DOS_ERR_LOCUS_UNKNOWN           0x01
#define DOS_ERR_LOCUS_BLOCK_DEVICE      0x02

/* BX as 59h returns it: the class in BH, the action in BL. */
#define DOS_ERR_CLASS_SHIFT             8
#define DOS_ERR_CLASS_ACTION(errorClass, action) (((errorClass) << DOS_ERR_CLASS_SHIFT) | (action))

/* The DOS extended error codes (AX) this header names. */
#define DOS_ERR_NONE                    0x00
#define DOS_ERR_INVALID_FUNCTION        0x01
#define DOS_ERR_FILE_NOT_FOUND          0x02
#define DOS_ERR_PATH_NOT_FOUND          0x03
#define DOS_ERR_TOO_MANY_OPEN_FILES     0x04
#define DOS_ERR_ACCESS_DENIED           0x05
#define DOS_ERR_INVALID_HANDLE          0x06
#define DOS_ERR_INSUFFICIENT_MEMORY     0x08
#define DOS_ERR_INVALID_BLOCK           0x09      /* invalid memory block address         */
#define DOS_ERR_INVALID_DATA            0x0D
#define DOS_ERR_INVALID_DRIVE           0x0F
#define DOS_ERR_NO_MORE_FILES           0x12
#define DOS_ERR_LOCK_VIOLATION          0x21
#define DOS_ERR_SHARING_VIOLATION       0x20
#define DOS_ABSOLUTE_UNKNOWN_UNIT      0x0201  /* INT 25h/26h AX: AH=02h, AL=01h unknown unit     */
#define DOS_ABSOLUTE_SECTOR_NOT_FOUND  0x0208  /* INT 25h/26h AX: AH=02h, AL=08h sector not found */
#define DOS_ERR_FILE_EXISTS             0x50
#define DOS_ERR_FAIL_I24   0x53     /* 59h after a FAILed critical error: "fail on INT 24" */

/* One measured row. `ClassAndAction` is BH:BL packed as DOS returns it, `Locus` the
   locus (CH). */
typedef struct _DOS_ERR_ROW {
    WORD  Code;
    WORD  ClassAndAction;
    BYTE  Locus;
    PCSTR Evidence;
} DOS_ERR_ROW, *PDOS_ERR_ROW;

typedef const DOS_ERR_ROW *PCDOS_ERR_ROW;

/* ── EVERY ROW IS A LINE OF ORACLE OUTPUT. ────────────────────────────────────
   tests/probes/dos/p_err.asm on MS-DOS 6.22, verbatim:

     CASE=err.after.3D.missing   AX=0002 BX=0803 CX=02C1     file not found
     CASE=err.after.4E.nopath    AX=0003 BX=0803 CX=02C1     path not found
     CASE=err.after.4E.nofile    AX=0012 BX=0803 CX=02C1     no more files
     CASE=err.after.3F.badhandle AX=0006 BX=0704 CX=01C1     invalid handle
     CASE=err.after.3D.readonly  AX=0005 BX=0303 CX=02C1     access denied
     CASE=err.after.5B.exists    AX=0050 BX=0C03 CX=02C1     file exists

   Note CX: CH is the locus and CL comes back holding the probe's own poison
   (0xC1) in every single row -- so DOS does not write CL, and neither do we. */
static const DOS_ERR_ROW g_DosErrTable[] = {
    { DOS_ERR_FILE_NOT_FOUND,
      DOS_ERR_CLASS_ACTION(DOS_ERR_CLASS_NOT_FOUND, DOS_ERR_ACTION_USER_INTERVENTION),
      DOS_ERR_LOCUS_BLOCK_DEVICE, "err.after.3D.missing"   },
    { DOS_ERR_PATH_NOT_FOUND,
      DOS_ERR_CLASS_ACTION(DOS_ERR_CLASS_NOT_FOUND, DOS_ERR_ACTION_USER_INTERVENTION),
      DOS_ERR_LOCUS_BLOCK_DEVICE, "err.after.4E.nopath"    },
    { DOS_ERR_NO_MORE_FILES,
      DOS_ERR_CLASS_ACTION(DOS_ERR_CLASS_NOT_FOUND, DOS_ERR_ACTION_USER_INTERVENTION),
      DOS_ERR_LOCUS_BLOCK_DEVICE, "err.after.4E.nofile"    },
    { DOS_ERR_INVALID_HANDLE,
      DOS_ERR_CLASS_ACTION(DOS_ERR_CLASS_BAD_FORMAT, DOS_ERR_ACTION_ABORT),
      DOS_ERR_LOCUS_UNKNOWN, "err.after.3F.badhandle" },
    { DOS_ERR_ACCESS_DENIED,
      DOS_ERR_CLASS_ACTION(DOS_ERR_CLASS_AUTHORIZATION, DOS_ERR_ACTION_USER_INTERVENTION),
      DOS_ERR_LOCUS_BLOCK_DEVICE, "err.after.3D.readonly"  },
    { DOS_ERR_FILE_EXISTS,
      DOS_ERR_CLASS_ACTION(DOS_ERR_CLASS_ALREADY_EXISTS, DOS_ERR_ACTION_USER_INTERVENTION),
      DOS_ERR_LOCUS_BLOCK_DEVICE, "err.after.5B.exists"    },
    /* Invalid drive. ⚠ IT NEEDED A DIFFERENT DOOR: opening "Y:\..." returns 3
       (path not found), not 15, so the code only turns up through AH=47h asking
       for the current directory of a drive with nothing behind it. Provoked, not
       reasoned about -- err.after.47.baddrive AX=000F BX=0803 CX=02C1. */
    { DOS_ERR_INVALID_DRIVE,
      DOS_ERR_CLASS_ACTION(DOS_ERR_CLASS_NOT_FOUND, DOS_ERR_ACTION_USER_INTERVENTION),
      DOS_ERR_LOCUS_BLOCK_DEVICE, "err.after.47.baddrive"  },
    /* #34: after an INT 24h answered FAIL -- class 0Dh, action 04h, locus 1 (unknown).
       p_crit crit.4e.fail.59 AX=0053 BX=0D04 CX=0100, 6.22/QEMU and PCem alike. */
    { DOS_ERR_FAIL_I24,
      DOS_ERR_CLASS_ACTION(DOS_ERR_CLASS_FAIL_I24, DOS_ERR_ACTION_ABORT),
      DOS_ERR_LOCUS_UNKNOWN, "crit.4e.fail.59"        },
};
#define DOS_ERR_ROWS (sizeof(g_DosErrTable) / sizeof(g_DosErrTable[0]))

/* Look up a code. Returns TRUE and fills classAndAction/locus when the pairing was
   measured; returns FALSE and zeroes them when it was not -- see the warning at the top
   for why zero rather than a plausible guess. Code 0 (no error) is not a row: 59h
   after a successful call reports AX=0 with class and locus zero, which is what
   the not-found path already produces. */
static BOOL DosErrClassify(_In_ WORD errorCode, _Out_ PWORD classAndAction, _Out_ PBYTE locus)
{
    UINT rowIndex;
    *classAndAction = 0; *locus = 0;
    if (!errorCode) return TRUE;                /* no error: zeroes are correct */
    for (rowIndex = 0; rowIndex < DOS_ERR_ROWS; ++rowIndex) {
        if (g_DosErrTable[rowIndex].Code != errorCode) continue;
        *classAndAction = g_DosErrTable[rowIndex].ClassAndAction;
        *locus = g_DosErrTable[rowIndex].Locus;
        return TRUE;
    }
    return FALSE;                               /* caller logs UNMEASURED       */
}

/* ── WIN32 -> DOS, FOR EVERY CALL THAT FAILS THROUGH CreateFileA. (s72) ───────
   AH=3Dh answered **2 ("file not found") for every possible failure**, because
   the handler read `f == INVALID_HANDLE_VALUE` and stopped asking. Measured by
   tests/probes/dos/p_err.asm against MS-DOS 6.22, that is wrong twice over:

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
     being collapsed silently, exactly as DosErrClassify() refuses to invent a
     class.
   ⚠ THERE IS DELIBERATELY NO ERROR_INVALID_DRIVE (15) ROW. The obvious guess is
     that "Y:\..." arrives as 15 and should map to DOS 3 -- but measured, it
     arrives as **win32=3**, so a 15 row would be an unexercised invention
     dressed as evidence. If a door is ever found that does produce 15, provoke
     it and add the row then, in that order.

   Numeric rather than the ERROR_* macros, which the off-VM build (no windows.h)
   does not have, so tests/unit/err_test.c can keep pinning it off-VM. */
#define DOS_ERR_WIN32_FILE_NOT_FOUND      2u
#define DOS_ERR_WIN32_PATH_NOT_FOUND      3u
#define DOS_ERR_WIN32_TOO_MANY_OPEN       4u
#define DOS_ERR_WIN32_ACCESS_DENIED       5u
#define DOS_ERR_WIN32_FILE_EXISTS        80u
#define DOS_ERR_WIN32_ALREADY_EXISTS    183u

/* ── #34: THE HARDWARE ERRORS, 19-31: the same numbers in DOS and Win32 (see below). Named
     as winerror.h names them. */
#define DOS_ERR_WRITE_PROTECT           19
#define DOS_ERR_BAD_UNIT                20
#define DOS_ERR_NOT_READY               21
#define DOS_ERR_BAD_COMMAND             22
#define DOS_ERR_CRC                     23
#define DOS_ERR_BAD_LENGTH              24
#define DOS_ERR_SEEK                    25
#define DOS_ERR_NOT_DOS_DISK            26
#define DOS_ERR_SECTOR_NOT_FOUND        27
#define DOS_ERR_OUT_OF_PAPER            28
#define DOS_ERR_WRITE_FAULT             29
#define DOS_ERR_READ_FAULT              30
#define DOS_ERR_GEN_FAILURE             31
#define DOS_ERR_HARDWARE_FIRST          DOS_ERR_WRITE_PROTECT
#define DOS_ERR_HARDWARE_LAST           DOS_ERR_GEN_FAILURE

static BOOL DosErrFromWin32(_In_ DWORD win32Error, _Out_ PWORD dosError)
{
    switch (win32Error) {
    /* ── MEASURED, both sides. See the log lines quoted above. ─────────────── */
    case DOS_ERR_WIN32_FILE_NOT_FOUND:                       /* err.after.3D.missing  AX=0002 */
        *dosError = DOS_ERR_FILE_NOT_FOUND; return TRUE;
    /* Also the bad-drive door: "Y:\..." arrives here as 3, not 15. */
    case DOS_ERR_WIN32_PATH_NOT_FOUND:                       /* err.after.3D.baddrive AX=0003 */
        *dosError = DOS_ERR_PATH_NOT_FOUND; return TRUE;
    case DOS_ERR_WIN32_ACCESS_DENIED:                        /* err.after.3D.readonly AX=0005 */
        *dosError = DOS_ERR_ACCESS_DENIED; return TRUE;
    /* ── AN IDENTITY, NOT A MEASUREMENT, AND LABELLED AS SUCH. DOS error 4 IS
         "too many open files" and the handler already answers 4 when it runs out
         of its own slots, so the two names denote one condition. NOT provoked by
         a probe: to promote it, extend p_err.asm to exhaust the handle table. */
    case DOS_ERR_WIN32_TOO_MANY_OPEN:  *dosError = DOS_ERR_TOO_MANY_OPEN_FILES; return TRUE;
    /* #168: p_file int21.6C.exists -- 6Ch "fail if it exists" on a file that does:
         6.22 answers AX=0050. CREATE_NEW reports ERROR_FILE_EXISTS; CreateDirectory
         and MoveFile say ERROR_ALREADY_EXISTS for the same condition. */
    case DOS_ERR_WIN32_FILE_EXISTS:
    case DOS_ERR_WIN32_ALREADY_EXISTS: *dosError = DOS_ERR_FILE_EXISTS; return TRUE;
    /* ── #34: THE HARDWARE ERRORS, 19-31, ARE THE SAME NUMBERS ON BOTH SIDES. Win32
         kept DOS's codes for them (ERROR_WRITE_PROTECT 19 .. ERROR_GEN_FAILURE 31;
         an empty floppy drive is ERROR_NOT_READY, 21). An identity, and labelled as
         one -- and the code that matters: 19-31 is what turns a failure into a
         CRITICAL error that goes to the program's INT 24h (DosCrit*, below). */
    case DOS_ERR_WRITE_PROTECT: case DOS_ERR_BAD_UNIT: case DOS_ERR_NOT_READY:
    case DOS_ERR_BAD_COMMAND: case DOS_ERR_CRC: case DOS_ERR_BAD_LENGTH: case DOS_ERR_SEEK:
    case DOS_ERR_NOT_DOS_DISK: case DOS_ERR_SECTOR_NOT_FOUND: case DOS_ERR_OUT_OF_PAPER:
    case DOS_ERR_WRITE_FAULT: case DOS_ERR_READ_FAULT: case DOS_ERR_GEN_FAILURE:
        *dosError = (WORD)win32Error; return TRUE;
    default:                                         /* caller logs win32= and keeps 2 */
        *dosError = DOS_ERR_FILE_NOT_FOUND; return FALSE;
    }
}

/* ── #34: THE CRITICAL-ERROR CONTRACT (INT 24h), the pure half. ─────────────────
     A DOS error 19-31 from a disk call is a HARDWARE error, and DOS does not just
     return it: it calls INT 24h with
        AH  bit 7 = 0 (a disk); bit 0 = 1 for a WRITE; bits 2-1 = the area (0 DOS
            system, 1 FAT, 2 directory, 3 data); bits 3/4/5 = FAIL/RETRY/IGNORE are
            allowed answers
        AL  the drive (0 = A:)      DI  the error, low byte = code - 19 (2 = not ready)
     and the handler answers 0 IGNORE, 1 RETRY, 2 ABORT, 3 FAIL. Every value below is
     a row p_crit.asm measured on 6.22 (QEMU) and PCem with drive A: failed "not
     ready" -- see the evidence beside each. */
/* INT 24h's answer in AL, and what DOS makes of it. */
#define DOS_CRIT_ACTION_IGNORE 0
#define DOS_CRIT_ACTION_RETRY  1
#define DOS_CRIT_ACTION_ABORT  2
#define DOS_CRIT_ACTION_FAIL   3
#define DOS_TERM_NORMAL          0      /* AH of AH=4Dh: how the child ended            */
#define DOS_TERM_CRITICAL_ABORT  2
#define DOS_CRIT_ABORT_RETURN_CODE  0x00  /* AL of AH=4Dh after an abort: PCem crit.abort.4d=0200 */

static inline BOOL DosCritIsHardwareError(_In_ WORD errorCode)
{ return errorCode >= DOS_ERR_HARDWARE_FIRST && errorCode <= DOS_ERR_HARDWARE_LAST; }

/* The INT 24h answer bits (AH bits 3-5), named as DOS's own source names them. */
#define DOS_CRIT_ALLOW_FAIL    0x08
#define DOS_CRIT_ALLOW_RETRY   0x10
#define DOS_CRIT_ALLOW_IGNORE  0x20

/* The rest of INT 24h's AH: bit 0 = a WRITE, bits 2-1 = the area. */
#define DOS_CRIT_WRITE         1
#define DOS_CRIT_AREA_SHIFT    1
#define DOS_CRIT_AREA_DATA     3
#define DOS_CRIT_AH_PATH_CALL  0x1A     /* p_crit crit.*.int24 BX=1A00 */

/* The INT 21h functions on an already-open file that DosCritInt24Ah tells apart. */
#define DOS_CRIT_FUNCTION_READ   0x3F
#define DOS_CRIT_FUNCTION_WRITE  0x40

/* AH for INT 24h, from the INT 21h function that failed.
   ★ MEASURED: a path call (4Eh, 3Dh, 3Ch) on a drive that is not ready gives AH=1Ah
     on both genuine kernels -- a READ (bit 0 clear, even for a create: DOS fails
     reading the disk before it gets to write anything), area 1 = the FAT (the first
     thing DOS reads from a changed disk), FAIL and RETRY allowed and IGNORE NOT.
     (6.22 under QEMU said 1Ch -- the directory -- once its FAT was cached; PCem said
     1Ah every time. We hold no FAT, so the uncached answer is the consistent one.)
   ── #275: 3Fh/40h ON AN ALREADY-OPEN FILE = A DATA-AREA TRANSFER, AND IGNORE IS
     ALLOWED THERE. From Microsoft's published MS-DOS 4.0 kernel source (github.com/
     microsoft/MS-DOS, v4.0/src/DOS), not from memory:
       * DISK2.ASM DISKREAD and DISK3.ASM DISKWRITE set `[ALLOWED] = allowed_RETRY +
         allowed_FAIL + allowed_IGNORE` before the transfer (DirRead / FATSecRd, the
         FAT and directory reads, set RETRY+FAIL only -- which is the 1Ah above);
       * CTRLC.ASM HardErr builds AH as area<<1 | READOP | ALLOWED, the area counted
         from the failing sector: 0 reserved, 1 FAT, 2 directory, 3 data.
     So read = 3Eh (data, read, F+R+I) and write = 3Fh (the same with bit 0 set).
   ⚠ 6.22 IS NOT 4.0, AND NOTHING HERE IS MEASURED YET: p_crit2.asm (crit2.h3f.* /
     crit2.h40.*) asks exactly this of 6.22 and PCem, and the area can legitimately
     come back 1 if the kernel has to walk an uncached FAT before the data sector --
     the probe reads and writes the file's FIRST cluster precisely so it does not. */
static inline BYTE DosCritInt24Ah(_In_ BYTE function)
{
    if (function == DOS_CRIT_FUNCTION_READ)
        return DOS_CRIT_ALLOW_FAIL | DOS_CRIT_ALLOW_RETRY | DOS_CRIT_ALLOW_IGNORE
               | (DOS_CRIT_AREA_DATA << DOS_CRIT_AREA_SHIFT);                       /* 3Eh */
    if (function == DOS_CRIT_FUNCTION_WRITE)
        return DOS_CRIT_ALLOW_FAIL | DOS_CRIT_ALLOW_RETRY | DOS_CRIT_ALLOW_IGNORE
               | (DOS_CRIT_AREA_DATA << DOS_CRIT_AREA_SHIFT) | DOS_CRIT_WRITE;      /* 3Fh */
    return DOS_CRIT_AH_PATH_CALL;                /* p_crit crit.*.int24 BX=1A00 */
}

/* AX the failed call returns when the handler answers FAIL (59h then says 53h).
   ★ MEASURED: find-first returns 0003 "path not found" on both genuine kernels
     (crit.4e.fail.call); open and create return 0003 on PCem (QEMU, with its FAT
     cached, 0002/0005 -- recorded as state, not contract). The path calls all get 3.
   ── #275: 3Fh/40h return 0005 "access denied". MS-DOS 4.0 source: a FAILed DREAD /
     DWRITE comes back with carry and both DISKREAD and DISKWRITE leave through
     SET_ACC_ERR (DISK2.ASM: `MOV AX,error_access_denied`); the dispatcher's errorMap
     (MS_CODE.ASM) then sees FAILERR and sets the EXTENDED error to error_FAIL_I24
     (53h) -- the same 59h answer the path calls measured. ⚠ Unmeasured on 6.22:
     p_crit2 crit2.h3f.fail.call / crit2.h40.fail.call. */
static inline WORD DosCritFailAx(_In_ BYTE function, _In_ BYTE errorCode)
{
    (VOID)errorCode;
    if (function == DOS_CRIT_FUNCTION_READ || function == DOS_CRIT_FUNCTION_WRITE)
        return DOS_ERR_ACCESS_DENIED;
    return DOS_ERR_PATH_NOT_FOUND;
}

/* #275: what a 3Fh/40h answers when its INT 24h said IGNORE. MS-DOS 4.0 DREAD: an
   IGNORE returns with carry CLEAR, so DISKREAD/DISKWRITE carry on as if the sectors
   had moved -- the call reports the bytes it was asked for (a read still stops at
   end of file, as every read does) and the file position advances by them. Whatever
   is in the caller's buffer for an ignored read is whatever was there.
   `isSizeKnown` = 0 when the host could not learn the file size (then the request).
   ⚠ Unmeasured on 6.22: p_crit2 crit2.h3f.ignore.call / crit2.h40.ignore.call. */
static inline WORD DosCritIgnoreCount(_In_ BYTE function, _In_ WORD requestedCount,
                                      _In_ DWORD filePosition, _In_ DWORD fileSize,
                                      _In_ BOOL isSizeKnown)
{
    if (function == DOS_CRIT_FUNCTION_READ && isSizeKnown) {
        DWORD bytesLeft = (fileSize > filePosition) ? fileSize - filePosition : 0;
        return (WORD)(bytesLeft < requestedCount ? bytesLeft : requestedCount);
    }
    return requestedCount;
}

/* The drive letters A: to Z:, and what DosCritDriveFromNtName answers for no match. */
#define DOS_CRIT_DRIVE_COUNT   26
#define DOS_CRIT_NO_DRIVE      (-1)
#define DOS_CRIT_CASE_OFFSET   32       /* 'a' - 'A' */
#define DOS_CRIT_PATH_SEPARATOR '\\'

/* #275: AL for a handle call -- the drive the OPEN FILE lives on, not the current
   drive (DOS takes it from the DPB the SFT names; we have no SFT). The host asks
   Windows for the file object's NT name ("\Device\Floppy0\DIR\FILE.TXT") and for
   each drive letter's NT device ("A:" -> "\Device\Floppy0"); the drive is the
   letter whose device is a whole-component prefix of the name. devices[k] = NULL or ""
   for a letter that does not exist. -1 = no match (a mapped network drive, a SUBST):
   the caller keeps the current drive, which is what this answered before. */
static inline INT DosCritDriveFromNtName(
    _In_opt_ PCSTR ntName,
    _In_reads_(DOS_CRIT_DRIVE_COUNT) PCSTR const devices[DOS_CRIT_DRIVE_COUNT])
{
    INT driveIndex;
    if (!ntName) return DOS_CRIT_NO_DRIVE;
    for (driveIndex = 0; driveIndex < DOS_CRIT_DRIVE_COUNT; ++driveIndex) {
        PCSTR devicePosition = devices[driveIndex], namePosition = ntName;
        if (!devicePosition || !*devicePosition) continue;
        while (*devicePosition && *namePosition) {
            CHAR deviceChar = *devicePosition, nameChar = *namePosition;
            if (deviceChar >= 'A' && deviceChar <= 'Z')
                deviceChar = (CHAR)(deviceChar + DOS_CRIT_CASE_OFFSET);
            if (nameChar >= 'A' && nameChar <= 'Z')
                nameChar = (CHAR)(nameChar + DOS_CRIT_CASE_OFFSET);
            if (deviceChar != nameChar) break;
            ++devicePosition; ++namePosition;
        }
        if (!*devicePosition
            && (*namePosition == DOS_CRIT_PATH_SEPARATOR || *namePosition == 0)) return driveIndex;
    }
    return DOS_CRIT_NO_DRIVE;
}

#endif /* NTVDMEX_DOS_ERR_H */
