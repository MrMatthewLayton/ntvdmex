/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * See dos_int21.h. Faithful port of the INT 21h handlers from
 * tools/vdmhost/vdmhost.c; AH=48/49/4A delegate to the shared dos_mcb.h allocator.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include "dos_int21.h"
#include "dos_mcb.h"
#include "dos_layout.h"
#include "dos_sysvars.h"    /* #48: AH=1Fh/32h build their DPB with the chain's builder */
#include "dos_fh.h"         /* the handle table's two rules -- allocation + classification */
#include "dos_err.h"        /* AH=59h class/action/locus, measured on the oracle */
#include "log.h"            /* LogPut / LogHex */
#include "dos_ctab.h"       /* CP437 tables dumped from the 6.22 oracle */
#include "dos_auxprn.h"     /* #251: the AUX/PRN driver entries (guest code) */
#include "dos_lfn.h"        /* #210: the long-filename API's pure half */

/* Characters, drives, paths. */
#define DOS_INT21_PATH_SIZE                     300
#define DOS_INT21_DRIVE_ROOT_SIZE               3       /* "A:" */
#define DOS_INT21_DRIVE_VARIABLE_SIZE           5       /* "=A:", the drive's current directory */
#define DOS_INT21_DRIVE_PREFIX_ROOM             3       /* "A:" + NUL */
#define DOS_INT21_WILDCARD_ROOM                 2       /* "*" + NUL */
#define DOS_INT21_SERIAL_INFO_SIZE              23      /* AH=69h's block, per drive */
#define DOS_INT21_MS_PER_HUNDREDTH              10

/* The find-first DTA (AH=4Eh): what DOS leaves for the caller. */
#define DOS_INT21_DTA_ATTRIBUTE                 21
#define DOS_INT21_DTA_TIME                      22
#define DOS_INT21_DTA_DATE                      24
#define DOS_INT21_DTA_SIZE                      26
#define DOS_INT21_DTA_NAME                      30
#define DOS_INT21_DTA_NAME_LENGTH               12      /* 8.3, with its dot */

/* FCBs. */
#define DOS_INT21_XFCB_FLAG                     0xFF    /* An extended FCB starts 0xFF... */
#define DOS_INT21_XFCB_HEADER                   7       /* ...and the FCB proper follows 7 bytes on */
#define DOS_INT21_FCB_NAME                      1       /* The drive byte, then the 8.3 name */
#define DOS_INT21_FCB_BASE_LENGTH               8
#define DOS_INT21_FCB_EXTENSION                 9
#define DOS_INT21_EXTENSION_LENGTH              3

/* The handle table: standard handles, SFT indexes in the JFT. */
#define DOS_INT21_STD_HANDLES                   5       /* Stdin, stdout, stderr, stdaux, stdprn */
#define DOS_INT21_STDAUX_HANDLE                 3
#define DOS_INT21_STDPRN_HANDLE                 4
#define DOS_INT21_STD_OPEN_ALL                  0x1F    /* All five open */
#define DOS_INT21_STD_OPEN_BITS                 32
#define DOS_INT21_SFT_FIRST_HOST                3       /* Past AUX, CON, PRN: a host handle */
#define DOS_INT21_SFT_LAST                      255
#define DOS_INT21_SHELL_VERSION_MAJOR           5       /* What an NTVDM-aware shell is told: 5.00 */
#define DOS_INT21_SHELL_VERSION_MINOR           0
#define DOS_INT21_DEFAULT_VERSION_MAJOR         6       /* The oracle: 6.22 */
#define DOS_INT21_DEFAULT_VERSION_MINOR         22

/* NtQueryObject, for the drive a handle is on. */
#define DOS_INT21_OBJECT_NAME_INFORMATION       1
#define DOS_INT21_OBJECT_NAME_SIZE              1024
#define DOS_INT21_NT_NAME_SIZE                  600
#define DOS_INT21_DEVICE_NAME_SIZE              80
#define DOS_INT21_WCHAR_BYTES                   2

/* The INT 21h the guest executed, and its frame. */
#define DOS_INT21_CALLSITE_BEFORE               24      /* The trace's bytes around the call site */
#define DOS_INT21_CALLSITE_AFTER                10
#define DOS_INT21_CALLSITE_MAX                  48

/* Console I/O. */
#define DOS_INT21_CRLF_LENGTH                   2
#define DOS_INT21_DIRECT_INPUT                  0xFF    /* AH=06h DL=FFh: read, don't write */
#define DOS_INT21_INPUT_READY                   0xFF    /* AH=0Bh/06h: a character is waiting */
#define DOS_INT21_INPUT_NONE                    0x00
#define DOS_INT21_LINE_COUNT                    1       /* AH=0Ah's buffer: max, count, then text */
#define DOS_INT21_LINE_TEXT                     2
#define DOS_INT21_CONSOLE_LINE_MAX              127     /* AH=3Fh from CON: a line, then CR LF */
#define DOS_INT21_PRINT_STRING_MAX              1024    /* AH=09h: the most a '$' string is read */
#define DOS_INT21_TRACE_LINE_MAX                64
#define DOS_INT21_TRACE_FIRST_BYTES             8
#define DOS_INT21_TRACE_DTA_BYTES               44

/* Files. */
#define DOS_INT21_HARD_ERROR_FIRST              19      /* Win32 19..31: the critical-error codes */
#define DOS_INT21_HARD_ERROR_LAST               31
#define DOS_INT21_OEM_SERIAL_HIGH               0xFF00  /* AH=30h: BH = OEM FFh, BL = serial high */
#define DOS_INT21_SERIAL_LOW                    0x0000
#define DOS_INT21_ATTRIBUTE_VOLUME              0x08
#define DOS_INT21_VOLUME_SIZE                   128
#define DOS_INT21_ALL_PATTERN_SIZE              8

/* The find-first DTA's reserved area: our search, as DOS keeps its own there. */
#define DOS_INT21_FIND_DRIVE_C                  3       /* The drive byte: C: */
#define DOS_INT21_FIND_MASK                     12
#define DOS_INT21_FIND_RESERVED                 13
#define DOS_INT21_FIND_TAG                      19      /* DOS_FIND_MAGIC: the slot below is ours */
#define DOS_INT21_FIND_SLOT                     20

/* The FCB, past the drive byte and the 8.3 name. */
#define DOS_INT21_FCB_DRIVE_AND_NAME            12
#define DOS_INT21_FCB_BLOCK                     12      /* Current block */
#define DOS_INT21_FCB_RECORD_SIZE               14
#define DOS_INT21_FCB_FILE_SIZE                 16
#define DOS_INT21_FCB_NEW_NAME                  16      /* AH=17h: the new name's drive byte */
#define DOS_INT21_FCB_DATE                      20
#define DOS_INT21_FCB_TIME                      22
#define DOS_INT21_FCB_TAG                       24      /* DOS_FCB_MAGIC: the handle below is ours */
#define DOS_INT21_FCB_HANDLE                    25
#define DOS_INT21_FCB_CURRENT_RECORD            32
#define DOS_INT21_FCB_RANDOM_RECORD             33
#define DOS_INT21_FCB_SAVED_SIZE                16
#define DOS_INT21_FCB_DEFAULT_RECORD            128
#define DOS_INT21_FCB_RECORDS_PER_BLOCK         128
#define DOS_INT21_FCB_RECORD_BUFFER             512
#define DOS_INT21_FCB_END_OF_FILE               1       /* AL: nothing transferred */
#define DOS_INT21_FCB_PARTIAL_RECORD            3       /* AL: a partial final record */
#define DOS_INT21_XFCB_ATTRIBUTE                6       /* An extended FCB's attribute byte */

/* A found entry, as AH=11h/12h leave it after the drive byte (a directory entry). */
#define DOS_INT21_DIRENTRY_ATTRIBUTE            12
#define DOS_INT21_DIRENTRY_RESERVED             13
#define DOS_INT21_DIRENTRY_TIME                 23
#define DOS_INT21_DIRENTRY_DATE                 25
#define DOS_INT21_DIRENTRY_CLUSTER              27
#define DOS_INT21_DIRENTRY_SIZE                 29
#define DOS_INT21_DIRENTRY_LAST                 32

/* The vectors AH=00h unwinds. */
#define DOS_INT21_IVT_TIMER                     0x20    /* INT 08h's vector */
#define DOS_INT21_IVT_USER_TICK                 0x70    /* INT 1Ch's vector */

/* A subfunction (AL) that gets or sets. */
#define DOS_INT21_GET                           0x00
#define DOS_INT21_SET                           0x01
#define DOS_INT21_HEX_DIGIT_BITS                4
#define DOS_INT21_CH_CLEAR_MASK                 0xFFFF00FFu

/* EXEC (AH=4Bh): AL, and the parameter block. */
#define DOS_INT21_EXEC_PB_TAIL                  2       /* Environment, then tail, FCB1, FCB2 */
#define DOS_INT21_EXEC_PB_FCB1                  6
#define DOS_INT21_EXEC_PB_FCB2                  10

/* Drives: free space and the DPB. */
#define DOS_INT21_DRIVE_C                       2       /* 0-based */
#define DOS_INT21_ROOT_PATH_SIZE                4       /* "A:\" */
#define DOS_INT21_INVALID_DRIVE_AL              0xFF
#define DOS_INT21_MEDIA_FIXED                   0xF8
#define DOS_INT21_MEDIA_FLOPPY                  0xF0
#define DOS_INT21_DEFAULT_SECTOR_SIZE           512
#define DOS_INT21_FLOPPY_ROOT_ENTRIES           224     /* 6.22's floppy */
#define DOS_INT21_FIXED_ROOT_ENTRIES            512
#define DOS_INT21_MAX_CLUSTER                   0xFFFE
#define DOS_INT21_DPB_LAST                      0xFFFF  /* The next-DPB pointer: none */
#define DOS_INT21_WOW_CURRENT_DRIVE             0xF0    /* AH=47h DL=F0h: WOW's "current drive" */
#define DOS_INT21_DRIVE_PREFIX_LENGTH           2       /* "C:" */
#define DOS_INT21_CURRENT_DIRECTORY_MAX         63
#define DOS_INT21_CURRENT_DIRECTORY_AX          0x0100  /* Oracle: AX=0100h */
#define DOS_INT21_LABEL_SIZE                    64
#define DOS_INT21_FILE_SYSTEM_SIZE              32
#define DOS_INT21_FILE_SYSTEM_LENGTH            8
#define DOS_INT21_SERIAL_INFO                   2       /* AH=69h's block: info level, serial, ... */
#define DOS_INT21_SERIAL_LABEL                  6
#define DOS_INT21_SERIAL_FILE_SYSTEM            17

/* Countries and code pages (AH=38h/65h/66h). */
#define DOS_INT21_CODE_PAGE                     437
#define DOS_INT21_CODE_PAGE_GET                 0x01
#define DOS_INT21_CODE_PAGE_SET                 0x02
#define DOS_INT21_COUNTRY_US                    0x01
#define DOS_INT21_COUNTRY_GENERAL               0x01    /* AX=6501h: the general information */
#define DOS_INT21_COUNTRY_UPPERCASE             0x02
#define DOS_INT21_COUNTRY_FILENAME_UPPERCASE    0x04
#define DOS_INT21_COUNTRY_FILENAME_TERMINATORS  0x05
#define DOS_INT21_COUNTRY_COLLATING             0x06
#define DOS_INT21_COUNTRY_DBCS                  0x07
#define DOS_INT21_CAPITALIZE_CHAR               0x20
#define DOS_INT21_CAPITALIZE_STRING             0x21
#define DOS_INT21_CAPITALIZE_ASCIIZ             0x22
#define DOS_INT21_CAPITALIZE_ASCIIZ_MAX         0x10000u
#define DOS_INT21_YES_NO                        0x23
#define DOS_INT21_NO                            0
#define DOS_INT21_YES                           1
#define DOS_INT21_NEITHER                       2
#define DOS_INT21_COUNTRY_INFO_SIZE             41
#define DOS_INT21_COUNTRY_DATA_SIZE             0x26    /* What follows the size word */
#define DOS_INT21_COUNTRY_ID                    3
#define DOS_INT21_COUNTRY_CODE_PAGE             5
#define DOS_INT21_COUNTRY_TABLE                 7
#define DOS_INT21_COUNTRY_TABLE_SIZE            24
#define DOS_INT21_COUNTRY_CASEMAP               18      /* The table's case-map far pointer */

/* Temporary names (AH=5Ah). */
#define DOS_INT21_TEMP_DIRECTORY_MAX            280
#define DOS_INT21_TEMP_SEED_STEP                0x1234u
#define DOS_INT21_TEMP_NAME_DIGITS              8
#define DOS_INT21_TEMP_TOP_SHIFT                28      /* The first of eight hex digits */

/* Network and server calls. */
#define DOS_INT21_MACHINE_NAME_SIZE             16
#define DOS_INT21_SERVER_SWAPPABLE_AREA         0x06
#define DOS_INT21_SERVER_PRINTER_MODE           0x08
#define DOS_INT21_SERVER_PRINTER_FLUSH          0x09
#define DOS_INT21_TRUENAME_MAX                  127
#define DOS_INT21_ZERO_FLAG                     0x0040
#define DOS_INT21_TRACE_MCB_MAX                 40
#define DOS_INT21_INVALID_DRIVE_AX              0xFFFF  /* AH=36h: no such drive */
#define DOS_INT21_COUNTRY_IN_BX                 0xFF    /* AH=38h AL=FFh: the country code is in BX */
#define DOS_INT21_COUNTRY_SET                   0xFFFF  /* AH=38h DX=FFFFh: set, not get */
#define DOS_INT21_STRATEGY_GET                  0x00    /* AH=58h */
#define DOS_INT21_STRATEGY_SET                  0x01
#define DOS_INT21_UMB_LINK_GET                  0x02
#define DOS_INT21_UMB_LINK_SET                  0x03
#define DOS_INT21_BREAK_GET                     0x00    /* AH=33h */
#define DOS_INT21_BREAK_SET                     0x01
#define DOS_INT21_BREAK_SWAP                    0x02
#define DOS_INT21_BOOT_DRIVE                    0x05
#define DOS_INT21_BOOT_DRIVE_C                  3       /* 1-based */
#define DOS_INT21_TRUE_VERSION                  0x06
#define DOS_INT21_SUBFUNCTION_INVALID_AL        0xFF
#define DOS_INT21_CLOCK_SET                     0x00    /* AH=2Bh/2Dh: AL */
#define DOS_INT21_CLOCK_INVALID                 0xFF

/* IOCTL (AH=44h). */
#define DOS_INT21_DEVICE_INFO_CONSOLE           0x80D3  /* AX=4400h DX, as 6.22 answers */
#define DOS_INT21_DEVICE_INFO_FILE              0x0002
#define DOS_INT21_DEVICE_INFO_AUX               0x80C0
#define DOS_INT21_DEVICE_INFO_PRN               0xA0C0
#define DOS_INT21_NT_PREFIX_LENGTH              4       /* "\\??\\" */
#define DOS_INT21_NONE                          0x0000
#define DOS_INT21_PARSE_KEEP_DRIVE              0x02    /* AH=29h AL: keep what the FCB already has */
#define DOS_INT21_PARSE_KEEP_NAME               0x04
#define DOS_INT21_PARSE_KEEP_EXTENSION          0x08

/* INT 21h 4Eh/4Fh find-first/find-next.  GH #29:
 *
 * DTA BLOCK LAYOUT, read off the oracle byte for byte (tests/probes/dos/p_find.asm).
 * The dump cross-checks itself: the size field came back 0xD575 = 54645, which is
 * COMMAND.COM's exact byte count.
 *
 *   [0]      drive number            [1-11]  11-byte search template
 *   [12]     search attributes       [13-14] directory entry number
 *   [15-16]  starting cluster        [17-20] reserved (DOS search state)
 *   [21]     attribute of the file found
 *   [22-23]  time      [24-25] date      [26-29] size (dword)
 *   [30-42]  filename, ASCIIZ, 13 bytes
 *
 * The block is 43 bytes: byte 43 came back still poisoned, so nothing beyond it
 * may be written.
 *
 * DOS keeps its own search state in the first 21 bytes.  We keep a Win32 handle
 * instead and stash the slot number there, so a program that saves and restores
 * its DTA between calls -- which real programs do -- resumes the right search.
 */
#define DOS_FIND_MAGIC                          0x4E

/* The FCB interface (AH=0Fh-24h, 27h-29h).  GH #36:
 *
 * The pre-1983 file API.  Little 6.22-era software uses it, but TREE.COM does,
 * and it is 19 of the 103 services 6.22 defines.
 *
 * MEASURED ON THE ORACLE (tests/probes/dos/p_fcb.asm), and the first fact matters
 * more than the rest: **FCB calls report success in AL (00 ok, FF fail), and the
 * CARRY FLAG IS UNDEFINED** -- a successful open came back with CF=1.  Anything
 * that treats carry as the result here is reading noise.
 *
 * An opened FCB comes back with bytes 0-31 filled and the current-record and
 * random-record fields (32-36) LEFT ALONE:
 *   [0] drive  [1-8] name  [9-11] ext  [12-13] current block
 *   [14-15] record size (128)  [16-19] file size  [20-21] date  [22-23] time
 *   [24-31] DOS's own workspace -- we keep our handle slot there
 *   [32] current record  [33-36] random record
 *
 * AH=11h/12h put a 33-byte directory entry in the DTA (that dump cross-checked
 * itself: the size field read 54645, COMMAND.COM's exact length):
 *   [0] drive  [1-11] name+ext  [12] attribute  [13-22] reserved
 *   [23-24] time  [25-26] date  [27-28] starting cluster  [29-32] size
 */
#define DOS_FCB_MAGIC                           0x46

/* #210: THE LONG-FILENAME API'S WIN32 HALF -- helpers for the AH=71h arm:
 * The pure half (time conversion, the find record, short names, the action word) is
 * dos_lfn.h; these only fetch what Win32 knows and hand it over.
 *
 * - LONG NAMES GO THROUGH THE SAME ...A FILE APIs AS EVERY OTHER DOS CALL HERE, i.e. the
 *   ANSI code page: this host never calls SetFileApisToOEM, so 3Dh, 4Eh and 71xxh all
 *   agree with each other about what a byte above 7Fh names. Stock NTVDM converts DOS
 *   names with the OEM code page (its file APIs are set to OEM -- from the NT design, NOT
 *   measured here). For ASCII the two are identical, and on the rig (CP437 OEM / 1252
 *   ANSI) they differ only above 7Fh -- a non-ASCII long name is the place to look if a
 *   program and stock disagree about one.
 *
 * - THE SEARCH TABLE. 714Eh hands the caller a HANDLE (AX) that it passes back to 714Fh
 *   and closes with 71A1h -- unlike 4Eh, whose state rides in the DTA. Kept here, one
 *   per VDM like g_DosSerialInfo; AX = slot + 1 so a handle is never 0.
 *
 * [CAUTION]: A program that never calls 71A1h leaks its slot until the table wraps: the 17th
 * live search recycles the oldest (round robin), as 4Eh recycles its eighth. Windows
 * 95 closes them when the PSP terminates; we have no per-PSP owner record yet.
 *
 * [CAUTION]: The handle VALUE is ours. Stock's numbering is not measured and p_lfn does not
 * compare AX on 714Eh -- a program that treats the handle as opaque cannot tell.
 */
#define DOS_LFN_FIND_SLOTS                      16

/* #275: WHICH DRIVE AN OPEN FILE IS ON, for INT 24h's AL on a 3Fh/40h failure:
 * We keep no SFT, so the handle has to be asked. XP has no GetFinalPathNameByHandle;
 * NtQueryObject(ObjectNameInformation) gives the file object's NT name
 * ("\Device\Floppy0\X.TXT") and QueryDosDeviceA("A:") the drive's NT device, which
 * works with no media in the drive -- the very case this is for. The match is the
 * pure DosCritDriveFromNtName (dos_err.h, off-VM tested).
 *
 * [CAUTION]: Only ever called on a DISK file whose ReadFile/WriteFile just failed with a
 * hardware error -- never on a pipe, where a name query can block.
 * -1 = could not tell (the caller keeps the current drive, as #34 did).
 */
typedef struct
{
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
} DOS_UNICODE_STRING;

typedef LONG (WINAPI *DOS_NT_QUERY_OBJECT)(HANDLE, INT, PVOID, ULONG, PULONG);

/* Set when the caller is servicing INT 21h for a client that is still in PROTECTED
 * mode (a DPMI client), so CF/ZF go to the live VTIB_EFLAGS instead of a pushed V86
 * FLAGS frame that does not exist there. See the pfl assignment below.
 */
INT g_DosInt21IsProtectedMode = 0;

/* THE VDM'S CLOCK (GH #250) -- see dos_clock.h. One per VDM, starts at the host's
 * time, moved only by a guest's own set calls.
 */
DOS_CLOCK_STATE g_DosClock;

/* GH #262: DOS'S CLOCK FOLLOWS THE TICK COUNT WHEN SOMEONE ELSE SET IT:
 * The host wires g_DosTickTake to the PIT's witness (vdd_pit_tick_take, under the
 * PIT's lock); NULL off-VM. DosClockSync is called before DOS's clock is read or
 * set -- AH=2Ah/2Bh/2Ch/2Dh and every file stamp -- so a raw store to 0040:006C is
 * seen by the next thing that asks DOS the time, as CLOCK$ would see it. With no
 * store pending it is one compare under the lock and nothing else.
 */
INT (*g_DosTickTake)(UINT32 *ticks, UINT32 *wraps, UINT32 *since) = 0;

/* INT 21h AH=53h private sub-functions, indexed by AL. See the handler for how each
 * row was measured and why this is a table and not a switch. Defaults = the stock
 * ntvdm measurement of 2026-09-25, which the host overrides from cfg\int53.txt.
 *
 * [CAUTION]: CHANGING A DEFAULT HERE IS A BEHAVIOUR CHANGE FOR EVERY GUEST -- the knob exists
 * so an experiment does not have to be one.
 */
DOS_INT53_ANSWER g_DosInt53Answers[DOS_INT53_COUNT] = {
    /* AL=00 */ { 0x0005, 0 },   /* documented form, asked with SI=BP=0 */
    /* AL=01 */ { 0x0001, 1 },   /* genuinely unsupported: DOS "invalid function" */
    /* AL=02 */ { 0x5300, 0 },   /* top of COMMAND.COM's main loop -- CF is the gate */
    /* AL=03 */ { 0x0001, 1 },   /* genuinely unsupported */
    /* AL=04 */ { 0x5300, 0 },
    /* AL=05 */ { 0x5301, 0 },   /* [CAUTION] context-dependent, see the handler */
    /* AL=06 */ { 0x5300, 0 },
    /* AL=07 */ { 0x5301, 0 },
};

/* MS-DOS 6.22 country block for country 1 (USA), INT 21h AH=38h.  GH #38.
 *
 * TRANSCRIBED FROM THE ORACLE, byte for byte (tests/probes/dos/p_ctry.asm):
 *   [0-1]  0000     date format, 0 = USA month/day/year
 *   [2-6]  "$"      currency symbol, ASCIIZ in 5 bytes
 *   [7-8]  ","      thousands separator      [9-10]  "."  decimal separator
 *   [11-12] "-"     date separator           [13-14] ":"  time separator
 *   [15]   00       currency format          [16]    02   digits after decimal
 *   [17]   00       time format, 0 = 12-hour
 *   [18-21]         FAR pointer to DOS's case-map routine -- filled in at run time
 *   [22-23] ","     data-list separator
 *
 * THE LENGTH IS MEASURED, NOT ASSUMED. The probe poisoned the destination with
 * 0xEE first: real DOS writes exactly 24 bytes and leaves everything past them
 * alone. The commonly quoted "34-byte block" would have had us zeroing 10 bytes
 * of the caller's memory that DOS never touches.
 */
static const BYTE g_DosCountryUs[24] = {
    0x00, 0x00,
    0x24, 0x00, 0x00, 0x00, 0x00,
    0x2C, 0x00,   0x2E, 0x00,   0x2D, 0x00,   0x3A, 0x00,
    0x00,   0x02,   0x00,
    0x00, 0x00, 0x00, 0x00,               /* case-map FAR ptr, patched below */
    0x2C, 0x00
};

/* FindFirstFileA + skip to the first DOS match; INVALID_HANDLE_VALUE if none (the
 * handle is closed then, and *nodir says whether the DIRECTORY itself was missing).
 */
/* #34: the Win32 error of the last FindFirstFileA that failed outright, so a drive
 * that is NOT READY (21) can be told from "no such file" -- the first is a critical
 * error and goes to INT 24h, the second is an ordinary answer. 0 = it did not fail.
 */
static DWORD g_DosFindWin32Error;

/* The drive DosHandleDrive found for THIS call's failed 3Fh/40h; -1 = none. Read by
 * the INT 24h tail of DosInt21, reset at its entry (the g_DosFindWin32Error pattern).
 */
static INT g_DosReadWriteDrive = -1;

/* #165: 6901h's serial, label and file-system type, per drive, for this session only
 * (see the 6901h arm). 23 bytes = the 6900h/6901h block from offset 2.
 */
static BYTE g_DosSerialIsSet[DOS_DRIVE_LETTERS];
static BYTE g_DosSerialInfo[DOS_DRIVE_LETTERS][DOS_INT21_SERIAL_INFO_SIZE];
static HANDLE  g_DosLfnFinds[DOS_LFN_FIND_SLOTS];
static BYTE g_DosLfnAllow[DOS_LFN_FIND_SLOTS];
static BYTE g_DosLfnNeed[DOS_LFN_FIND_SLOTS];
static UINT g_DosLfnNext;

VOID DosInt21SetProtectedMode(INT isOn)
{
    g_DosInt21IsProtectedMode = isOn ? 1 : 0;
}

VOID DosClockHostNow(PDOS_CLOCK_TIME time)
{
    SYSTEMTIME localTime;

    GetLocalTime(&localTime);
    time->Year = localTime.wYear;
    time->Month = localTime.wMonth;
    time->Day = localTime.wDay;
    time->Hour = localTime.wHour;
    time->Minute = localTime.wMinute;
    time->Second = localTime.wSecond;
    time->Hundredths = (UINT)(localTime.wMilliseconds / DOS_INT21_MS_PER_HUNDREDTH);
    time->DayOfWeek = localTime.wDayOfWeek;
}

VOID DosClockRead(INT64 offset, PDOS_CLOCK_TIME out)
{
    DOS_CLOCK_TIME host;

    DosClockHostNow(&host);

    if (offset == 0) /* the common case, exactly as before */
    {
        *out = host;
        return;
    }

    DosClockApplyOffset(&host, offset, out);
}

VOID DosClockFollow(UINT32 ticks, UINT32 wraps, UINT32 since)
{
    DOS_CLOCK_TIME host;

    DosClockHostNow(&host);
    DosClockFollowTicks(&host, &g_DosClock.DosOffset, ticks, wraps, since);
}

VOID DosClockSync(VOID)
{
    UINT32 ticks;
    UINT32 wraps;
    UINT32 since;

    if (g_DosTickTake && g_DosTickTake(&ticks, &wraps, &since))
        DosClockFollow(ticks, wraps, since);
}

/* GH #263: A FILE CARRIES DOS'S DATE, NOT THE HOST'S:
 * File I/O is Win32's and Win32 stamps a write with the machine's clock, so after
 * INT 21h AH=2Bh set 1999-06-15 a program's new file still said today. DOS stamps
 * a created or written file with ITS clock; ours is host-now + DosOffset. Applied
 * at create and after every write, only while a guest has moved the clock
 * (DosOffset != 0) -- an untouched VDM never calls this, exactly as before. Local
 * time, the inverse of what AH=57h AL=00h reads back. A refusal (a handle without
 * FILE_WRITE_ATTRIBUTES) leaves Win32's stamp, which is what there was.
 * - EVERY CREATE AND EVERY WRITE PATH (s92): 3Ch and 40h had it; 5Ah/5Bh, 6Ch's
 *   create/truncate, the FCB create (16h) and FCB writes (15h/22h/28h), and the
 *   protected-mode twins in main.c (3Ch/5Bh/40h for a DPMI or Win16 client) did not.
 *
 * [CAUTION]: STAMPED AT THE WRITE, NOT AT THE CLOSE. DOS keeps the time in the SFT and writes
 * the directory entry at close; the two differ by however long the file stays open
 * after its last write -- 2-second resolution in the entry, UNMEASURED against 6.22
 * for a file held open across a second boundary. NTFS keeps an explicitly-set write
 * time for the rest of the handle's life, so the close does not overwrite it.
 */
VOID DosStampVdmNow(HANDLE file)
{
    DOS_CLOCK_TIME clock;
    SYSTEMTIME systemTime;
    FILETIME localTime;
    FILETIME fileTime;

    if (!file || file == INVALID_HANDLE_VALUE)
        return;

    DosClockSync();                             /* #262: a raw 006C store moves it too */

    if (!g_DosClock.DosOffset)
        return;

    DosClockRead(g_DosClock.DosOffset, &clock);
    systemTime.wYear = (WORD)clock.Year;
    systemTime.wMonth = (WORD)clock.Month;
    systemTime.wDayOfWeek = (WORD)clock.DayOfWeek;
    systemTime.wDay = (WORD)clock.Day;
    systemTime.wHour = (WORD)clock.Hour;
    systemTime.wMinute = (WORD)clock.Minute;
    systemTime.wSecond = (WORD)clock.Second;
    systemTime.wMilliseconds = (WORD)(clock.Hundredths * (UINT)DOS_INT21_MS_PER_HUNDREDTH);

    if (SystemTimeToFileTime(&systemTime, &localTime) && LocalFileTimeToFileTime(&localTime, &fileTime))
        SetFileTime(file, NULL, NULL, &fileTime);
}

/* Does MS-DOS 6.22 provide a MEANINGFUL service at this AH?  GH #27.
 *
 * This is the line between "we are missing something" and "DOS has nothing here
 * either", and the two need opposite behaviour: the first must fail loudly so a
 * no-op is never mistaken for success, the second must stay silent so we do not
 * invent an error real DOS never reports.
 *
 * THE BOUNDARY IS MEASURED, NOT REMEMBERED (tests/probes/dos/p_defs.asm, run against
 * the 6.22 oracle).  Every probed AH from 6Dh upward -- 6Dh, 6Eh, 6Fh, 70h, 71h,
 * 72h, 74h, 80h, A0h, E0h -- returns with AX unchanged, CF clear and every
 * poisoned output register still holding its poison: nothing happened at all.
 * 6Ch is the highest that does anything.
 *
 * The exceptions inside the table were measured the same way and carry the same
 * do-nothing signature, so they belong on the quiet side: 18h, 1Dh, 1Eh and 20h
 * are DOS's documented internal null functions, 61h is reserved, 6Bh is a null
 * function from DOS 5 on.
 *
 * A CAUTION FOR WHOEVER EXTENDS THIS: "AX came back unchanged" is NOT on its own
 * a reliable test for absence.  AH=54h (get verify flag) returns AL=0 when verify
 * is off, which is indistinguishable from the AL=0 that went in, and AH=2Ch
 * leaves AX alone while writing CX and DX.  The signature that actually works is
 * EVERY output register still poisoned.
 */
static INT DosIsDefinedBy622(BYTE function)
{
    if (function > DOS_FN_LAST_622)
        return 0;

    switch (function)
    {
    case DOS_FN_NULL_18:
    case DOS_FN_NULL_1D:
    case DOS_FN_NULL_1E:
    case DOS_FN_NULL_20:   /* internal null functions */

    case DOS_FN_UNUSED_61:                        /* reserved */

    case DOS_FN_NULL_6B:                          /* null function (DOS 5+) */
        return 0;

    default:
        return 1;
    }
}

/* OPEN WITH THE RIGHT TO RE-STAMP IT. (#168):
 * DOS lets AX=5701h set a file's date and time through ANY handle, read-only ones
 * included -- it only updates the SFT and writes the entry at close. NT's
 * SetFileTime needs FILE_WRITE_ATTRIBUTES on the handle, which GENERIC_READ lacks,
 * so on a 3D00h handle it failed and the file kept "now" (p_file int21.5700.stamp:
 * 6.22 read back 12:34:56 2001-09-17, we read back the wall clock). Ask for it too,
 * and fall back to the plain request where it is refused (a read-only medium or
 * share) -- the open must never be lost for the sake of the stamp. Attribute
 * rights are not subject to sharing, so this changes no share-mode outcome.
 */
static HANDLE DosOpenStampable(
    PCSTR fileName,
    DWORD access,
    DWORD share,
    DWORD disposition,
    DWORD attributes)
{
    HANDLE file = CreateFileA(fileName, access | FILE_WRITE_ATTRIBUTES, share, NULL, disposition, attributes, NULL);

    if (file == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED)
        file = CreateFileA(fileName, access, share, NULL, disposition, attributes, NULL);

    return file;
}

static INT DosDtaMatchesAttributes(DWORD attributes, WORD mask)
{
    /* DOS's rule is "normal files always match; these extras only if asked". */
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) && !(mask & DOS_LFN_ATTRIBUTE_DIRECTORY))
        return 0;

    if ((attributes & FILE_ATTRIBUTE_HIDDEN)    && !(mask & DOS_LFN_ATTRIBUTE_HIDDEN))
        return 0;

    if ((attributes & FILE_ATTRIBUTE_SYSTEM)    && !(mask & DOS_LFN_ATTRIBUTE_SYSTEM))
        return 0;

    return 1;
}

static VOID DosDtaFill(volatile BYTE *dta, const WIN32_FIND_DATAA *findData)
{
    FILETIME localTime;
    WORD fileDate = 0;
    WORD fileTime = 0;
    PCSTR name = findData->cAlternateFileName[0] ? findData->cAlternateFileName : findData->cFileName;
    INT index;

    if (FileTimeToLocalFileTime(&findData->ftLastWriteTime, &localTime))
        FileTimeToDosDateTime(&localTime, &fileDate, &fileTime);  /* DOS times are LOCAL */

    dta[DOS_INT21_DTA_ATTRIBUTE] = (BYTE)(findData->dwFileAttributes & DOS_LFN_ATTRIBUTE_MASK);
    dta[DOS_INT21_DTA_TIME] = (BYTE)(fileTime & BYTE_MASK);
    dta[DOS_INT21_DTA_TIME + 1] = (BYTE)(fileTime >> BYTE_SHIFT);
    dta[DOS_INT21_DTA_DATE] = (BYTE)(fileDate & BYTE_MASK);
    dta[DOS_INT21_DTA_DATE + 1] = (BYTE)(fileDate >> BYTE_SHIFT);
    dta[DOS_INT21_DTA_SIZE] = (BYTE)( findData->nFileSizeLow        & BYTE_MASK);
    dta[DOS_INT21_DTA_SIZE + 1] = (BYTE)((findData->nFileSizeLow >> BYTE_SHIFT)  & BYTE_MASK);
    dta[DOS_INT21_DTA_SIZE + 2] = (BYTE)((findData->nFileSizeLow >> WORD_SHIFT) & BYTE_MASK);
    dta[DOS_INT21_DTA_SIZE + 3] = (BYTE)((findData->nFileSizeLow >> TOP_BYTE_SHIFT) & BYTE_MASK);

    for (index = 0; index < DOS_INT21_DTA_NAME_LENGTH && name[index]; ++index)
    {
        CHAR character = name[index];

        if (character >= 'a' && character <= 'z')
            character = (CHAR)(character - ASCII_CASE_BIT);                                        /* DOS reports 8.3 upper */

        dta[DOS_INT21_DTA_NAME + index] = (BYTE)character;
    }

    dta[DOS_INT21_DTA_NAME + index] = 0;
}

static volatile BYTE *DosFcbAt(DWORD segment, DWORD offset)
{
    volatile BYTE *fcb = (volatile BYTE *)((segment << PARAGRAPH_SHIFT) + (offset & WORD_MASK));

    return (fcb[0] == DOS_INT21_XFCB_FLAG) ? fcb + DOS_INT21_XFCB_HEADER : fcb;  /* skip an extended FCB's prefix */
}

/* Build "D:NAME.EXT" from an FCB's drive/name/extension fields. */
static VOID DosFcbName(const volatile BYTE *fcb, PSTR out)
{
    INT index;
    INT length = 0;

    if (fcb[0])
    {
        out[length++] = (CHAR)('A' + fcb[0] - 1);
        out[length++] = ':';
    }

    for (index = DOS_INT21_FCB_NAME; index <= DOS_INT21_FCB_BASE_LENGTH && fcb[index] != ' '; ++index)
        out[length++] = (CHAR)fcb[index];

    if (fcb[DOS_INT21_FCB_EXTENSION] != ' ')
    {
        out[length++] = '.';

        for (index = DOS_INT21_FCB_EXTENSION; index <= DOS_FCB_NAME_SIZE && fcb[index] != ' '; ++index)
            out[length++] = (CHAR)fcb[index];
    }

    out[length] = 0;
}

/* A DOS FILENAME ENDS AT A TERMINATOR, NOT ONLY AT A NUL:
 * The set is DOS's own, and we already publish it to guests as the AH=65h AL=05
 * "filename terminator" table (dos_ctab.h): every control character and the space
 * (0x00-0x20), plus the punctuation that separates a path from what follows.
 * '.' is not here -- it is the extension separator and the caller handles it.
 *
 * [CAUTION]: WHY THIS IS NOT COSMETIC. A command line is terminated by 0x0D, and the name
 * builder below used to copy that CR straight into the FCB. COMMAND.COM matches
 * its internal command table by comparing the entry's characters and then checking
 * that the NEXT byte of the FCB is blank -- so `ver` parsed to "VER\r    " and
 * missed, while `ver ` parsed to "VER \r   " and hit purely because the user had
 * typed the blank we should have supplied. That is why every internal command was
 * "Bad command or file name" until you put a space after it.
 */
static INT DosFcbIsNameEnd(BYTE character)
{
    if (character <= ASCII_SPACE)
        return 1;                                      /* NUL, CR, TAB, space, any control */

    return character == '"' || character == '/' || character == '\\' || character == '[' || character == ']' || character == ':'
        || character == '|' || character == '<'  || character == '>'  || character == '+' || character == '=' || character == ';'
        || character == ',';
}

static VOID DosFcbPutName(volatile BYTE *destination, PCSTR name)
{
    INT source = 0;
    INT index;

    for (index = 0; index < DOS_FCB_NAME_SIZE; ++index)
        destination[index] = ' ';

    /* "." AND ".." ARE NAMES, NOT EXTENSIONS. The rule below ends the name at the
     * first '.', which for these two directory entries ends it at character zero and
     * leaves eleven blanks -- DIR then printed an empty column where the oracle
     * shows "." and "..". DOS stores them literally in the name field.
     */
    if (name[0] == '.')
    {
        destination[0] = '.';

        if (name[1] == '.' && (name[2] == 0 || name[2] == '.'))
            destination[1] = '.';

        if (name[1] == 0 || name[1] == '.')
            return;
    }

    /* `*` IS EXPANDED INTO `?`s, IT IS NOT STORED (Importance = 1):
     * An FCB name field has no room for a star and no meaning for one: the only
     * wildcard the format knows is `?`, so DOS fills the rest of the field with
     * them as it parses. Oracle-measured (p_fcb.asm int21.29.wild/starstar on
     * 6.22): "*.BAS" lands as 00 3Fx8 'BAS' and "*.*" as 00 3Fx11.
     * - THIS IS WHY QBASIC'S OPEN DIALOG LISTED NO FILES. It parses the pattern
     *   with AH=29h and then matches each directory entry against the parsed FCB.
     *   We stored `*` literally, so the template read `*` + seven blanks, nothing
     *   matched it, and the file pane came up empty -- while the directory pane
     *   beside it, which is not pattern-filtered, was perfectly correct. The
     *   symptom pointed at the search, the directory entries and the renderer; the
     *   cause was in the parser none of them go through.
     */
    for (index = 0; index < DOS_INT21_FCB_BASE_LENGTH && !DosFcbIsNameEnd((BYTE)name[source]) && name[source] != '.'; ++index, ++source)
    {
        if (name[source] == '*')
        {
            while (index < DOS_INT21_FCB_BASE_LENGTH)
                destination[index++] = '?';

            break;
        }

        destination[index] = (BYTE)(name[source] >= 'a' && name[source] <= 'z' ? name[source] - ASCII_CASE_BIT : name[source]);
    }

    while (!DosFcbIsNameEnd((BYTE)name[source]) && name[source] != '.') ++source;

    if (name[source] == '.')
        ++source;

    for (index = DOS_INT21_FCB_BASE_LENGTH; index < DOS_FCB_NAME_SIZE && !DosFcbIsNameEnd((BYTE)name[source]); ++index, ++source)
    {
        if (name[source] == '*')
        {
            while (index < DOS_FCB_NAME_SIZE)
                destination[index++] = '?';

            break;
        }

        destination[index] = (BYTE)(name[source] >= 'a' && name[source] <= 'z' ? name[source] - ASCII_CASE_BIT : name[source]);
    }
}

/* A DOS SEARCH MATCHES THE 8.3 NAME AGAINST AN 11-BYTE TEMPLATE. (s81 sweep) (Importance = 1):
 * We handed DOS patterns straight to FindFirstFileA, which matches them against the
 * LONG name. XP's COMMAND.COM lists a directory with an FCB search (AH=11h/12h) on
 * `????????.???`, and `ntvdmhost.exe` -- nine characters before the dot -- does not
 * fit that as a long name, so the user's `dir` in bin\ printed only `.` and `..`.
 * Real NTVDM matches the SHORT name (NTVDMH~1.EXE), which is the only name DOS has.
 * - So enumerate the directory with `*` and decide each entry DOS's way: its 8.3 name
 *   (the short alias, or the long name when that is already a legal 8.3 name -- and
 *   no name at all otherwise: such a file is invisible to DOS, as it is on NTVDM),
 *   laid out as 11 bytes, matched position by position, `?` matching anything.
 */
static INT DosShortNameOf(const WIN32_FIND_DATAA *findData, BYTE out[DOS_FCB_NAME_SIZE])
{
    PCSTR baseName = findData->cAlternateFileName[0] ? findData->cAlternateFileName : findData->cFileName;

    if (!findData->cAlternateFileName[0] && baseName[0] != '.')    /* the long name must BE 8.3 */
    {
        INT baseLength = 0;
        INT extensionLength = -1;
        INT index;

        for (index = 0; baseName[index]; ++index)
        {
            if (baseName[index] == '.')
            {
                if (extensionLength >= 0)
                    return 0;

                extensionLength = 0;
                continue;
            }

            if (baseName[index] == ' ' || DosFcbIsNameEnd((BYTE)baseName[index]))
                return 0;

            if (extensionLength >= 0)
            {
                if (++extensionLength > DOS_INT21_EXTENSION_LENGTH)
                    return 0;
            }
            else if (++baseLength > DOS_INT21_FCB_BASE_LENGTH)
                return 0;
        }

        if (!baseLength)
            return 0;
    }

    DosFcbPutName((volatile BYTE *)out, baseName);
    return 1;
}

static INT DosTemplateMatches(
    const BYTE nameTemplate[DOS_FCB_NAME_SIZE],
    const BYTE name[DOS_FCB_NAME_SIZE])
{
    INT index;

    for (index = 0; index < DOS_FCB_NAME_SIZE; ++index)
    {
        BYTE character = nameTemplate[index];

        if (character == '?')
            continue;

        if (character >= 'a' && character <= 'z')
            character = (BYTE)(character - ASCII_CASE_BIT);

        if (character != name[index])
            return 0;
    }

    return 1;
}

static INT DosFindMatches(
    const WIN32_FIND_DATAA *findData,
    const BYTE nameTemplate[DOS_FCB_NAME_SIZE],
    WORD mask)
{
    BYTE name[DOS_FCB_NAME_SIZE];

    return DosDtaMatchesAttributes(findData->dwFileAttributes, mask) && DosShortNameOf(findData, name) && DosTemplateMatches(nameTemplate, name);
}

/* Split a host path pattern into "directory\*" (for FindFirstFileA) and the final
 * component's 11-byte template.
 */
static VOID DosFindSplit(
    PCSTR pattern,
    PSTR directoryPattern,
    INT directoryPatternSize,
    BYTE nameTemplate[DOS_FCB_NAME_SIZE])
{
    INT index;
    INT cut = 0;

    for (index = 0; pattern[index]; ++index)
        if (pattern[index] == '\\' || pattern[index] == '/' || pattern[index] == ':')
            cut = index + 1;

    for (index = 0; index < cut && index < directoryPatternSize - DOS_INT21_WILDCARD_ROOM; ++index)
        directoryPattern[index] = pattern[index];

    directoryPattern[index++] = '*';
    directoryPattern[index] = 0;
    DosFcbPutName((volatile BYTE *)nameTemplate, pattern + cut);
}

static HANDLE DosFindFirst(
    PCSTR directoryPattern,
    const BYTE nameTemplate[DOS_FCB_NAME_SIZE],
    WORD mask,
    WIN32_FIND_DATAA *findData,
    PINT isNoDirectory)
{
    HANDLE find = FindFirstFileA(directoryPattern, findData);

    *isNoDirectory = 0;
    g_DosFindWin32Error = 0;

    if (find == INVALID_HANDLE_VALUE)
    {
        g_DosFindWin32Error = GetLastError();
        *isNoDirectory = (g_DosFindWin32Error == ERROR_PATH_NOT_FOUND);
        return find;
    }

    while (!DosFindMatches(findData, nameTemplate, mask))
        if (!FindNextFileA(find, findData))
        {
            FindClose(find);
            return INVALID_HANDLE_VALUE;
        }

    return find;
}

static INT DosHandleDrive(HANDLE file)
{
    static DOS_NT_QUERY_OBJECT queryObject;
    union
    {
        DOS_UNICODE_STRING String;
        BYTE Raw[DOS_INT21_OBJECT_NAME_SIZE];
    } objectName;
    CHAR name[DOS_INT21_NT_NAME_SIZE];
    CHAR deviceBuffers[DOS_DRIVE_LETTERS][DOS_INT21_DEVICE_NAME_SIZE];
    PCSTR devices[DOS_DRIVE_LETTERS];
    ULONG returned = 0;
    DWORD drives = GetLogicalDrives();
    INT drive;
    INT length;

    if (!queryObject)
    {
        HMODULE ntdll = GetModuleHandleA("ntdll.dll");

        if (ntdll)
            queryObject = (DOS_NT_QUERY_OBJECT)GetProcAddress(ntdll, "NtQueryObject");

        if (!queryObject)
            return -1;
    }

    if (queryObject(file, DOS_INT21_OBJECT_NAME_INFORMATION, &objectName, sizeof(objectName) - DOS_INT21_WCHAR_BYTES, &returned) < 0
        || !objectName.String.Buffer || !objectName.String.Length)
        return -1;

    length = WideCharToMultiByte(CP_ACP, 0, objectName.String.Buffer, objectName.String.Length / DOS_INT21_WCHAR_BYTES, name, sizeof(name) - 1, NULL, NULL);

    if (length <= 0)
        return -1;

    name[length] = 0;

    for (drive = 0; drive < DOS_DRIVE_LETTERS; ++drive)
    {
        CHAR root[DOS_INT21_DRIVE_ROOT_SIZE] = { (CHAR)('A' + drive), ':', 0 };
        devices[drive] = NULL;

        if (!(drives & (1u << drive)))
            continue;

        if (QueryDosDeviceA(root, deviceBuffers[drive], sizeof(deviceBuffers[drive])))
            devices[drive] = deviceBuffers[drive];
    }

    return DosCritDriveFromNtName(name, devices);
}

static INT DosFindNext(
    HANDLE find,
    const BYTE nameTemplate[DOS_FCB_NAME_SIZE],
    WORD mask,
    WIN32_FIND_DATAA *findData)
{
    do
    {
        if (!FindNextFileA(find, findData))
            return 0;
    } while (!DosFindMatches(findData, nameTemplate, mask));

    return 1;
}

/* Copy an ASCIIZ string out of V86 memory (seg:off) into a host buffer. */
static VOID DosGuestString(DWORD segment, DWORD offset, PSTR destination, INT capacity)
{
    const volatile BYTE *source = (const volatile BYTE *)((segment << PARAGRAPH_SHIFT) + (offset & WORD_MASK));
    INT index;

    for (index = 0; index < capacity - 1 && source[index]; ++index)
        destination[index] = (CHAR)source[index];

    destination[index] = 0;
}

/* The current drive, 0 = A:. The process current directory's, unless AH=0Eh
 * selected a drive Win32 could not enter (m->vdrive, see the header).
 */
static BYTE DosCurrentDrive(PCDOS_MACHINE machine)
{
    CHAR directory[DOS_INT21_PATH_SIZE];
    DWORD length;

    if (machine->VirtualDrive >= 0)
        return (BYTE)machine->VirtualDrive;

    length = GetCurrentDirectoryA(sizeof(directory), directory);
    return (length >= 2 && directory[1] == ':') ? (BYTE)((directory[0] | ASCII_CASE_BIT) - 'a') : DOS_CURRENT_DRIVE;
}

/* See the header: the host needs this for the NTVDM BOP 0x54 sub 01 reply, and must
 * not re-derive it -- a second copy of the rule would drop `vdrive`.
 */
BYTE DosInt21CurrentDrive(PCDOS_MACHINE machine)
{
    return DosCurrentDrive(machine);
}

/* BL as 69h takes it: 0 = the default drive, 1 = A:, ... -> 0-based, 26 if invalid. */
static BYTE DosSerialDrive(PCDOS_MACHINE machine, BYTE driveNumber)
{
    if (driveNumber == 0)
        return DosCurrentDrive(machine);

    return (BYTE)(driveNumber <= DOS_DRIVE_LETTERS ? driveNumber - 1 : DOS_DRIVE_LETTERS);
}

/* Keep Win32's per-drive current directory in step. GetFullPathNameA("X:") and
 * SetCurrentDirectoryA("X:") read the hidden `=X:` environment variable, which
 * cmd.exe and the CRT maintain and SetCurrentDirectoryA itself does NOT -- so
 * without this, C: -> D: -> C: came back to C:'s ROOT, where DOS returns to the
 * directory it left. The guest never sees the process environment (its block is
 * built separately), so these variables cost it nothing.
 */
static VOID DosNoteDriveDirectory(PCSTR fullPath)
{
    CHAR variable[DOS_INT21_DRIVE_VARIABLE_SIZE];

    if (!fullPath || !fullPath[0] || fullPath[1] != ':')
        return;

    variable[0] = '=';
    variable[1] = (CHAR)(fullPath[0] & ~ASCII_CASE_BIT);
    variable[2] = ':';
    variable[3] = 0;
    SetEnvironmentVariableA(variable, fullPath);
}

/* A guest path, as Win32 should see it: DosGuestString, then -- only while the current
 * drive is one Win32 cannot stand on -- a relative path is prefixed with that
 * drive so it resolves (and fails) THERE. "X:..." and "\\server" are left alone.
 * A device name survives the prefix: Win32 reads "A:CON" as CON, as DOS does.
 */
static VOID DosGuestPath(
    PCDOS_MACHINE machine,
    DWORD segment,
    DWORD offset,
    PSTR destination,
    INT capacity)
{
    CHAR guestPath[DOS_INT21_PATH_SIZE];
    INT length = 0;
    INT index;

    DosGuestString(segment, offset, guestPath, sizeof(guestPath));

    if (machine->VirtualDrive >= 0 && guestPath[0] && guestPath[1] != ':' && !(guestPath[0] == '\\' && guestPath[1] == '\\')
        && capacity > DOS_INT21_DRIVE_PREFIX_ROOM)
    {
        destination[length++] = (CHAR)('A' + machine->VirtualDrive);
        destination[length++] = ':';
    }

    for (index = 0; guestPath[index] && length < capacity - 1; ++index)
        destination[length++] = guestPath[index];

    destination[length] = 0;
}

VOID DosInt21Initialize(PDOS_MACHINE machine, WORD firstMcb)
{
    INT index;

    for (index = 0; index < DOS_MAX_FILES; ++index)
        machine->FileHandles[index] = 0;

    for (index = 0; index < DOS_FIND_SLOTS; ++index)
        machine->FindHandles[index] = 0;

    machine->LastError = 0;
    machine->IsVerifyOn = 0;
    machine->ChildReturnCode = 0;
    machine->FcbFind = 0;
    machine->SwitchChar = '/';  /* oracle-confirmed 6.22 default */
    machine->HandleDepth = 0;  /* no EXEC in progress: nothing saved */
    machine->SetTicks = 0;
    machine->TicksContext = 0;  /* the host wires these after init (#250) */
    machine->IsCritPending = 0;
    machine->IsCritActive = 0;
    machine->TermType = 0;  /* #34 */
    machine->CanRaiseCrit = 0;                                    /* #275 */
    {
        INT index2;

        for (index2 = 0; index2 < DOS_SHELL_PSP_SLOTS; ++index2)
            machine->ShellPsps[index2] = 0;
    }
    machine->ShellVersionMajor = DOS_INT21_SHELL_VERSION_MAJOR;
    machine->ShellVersionMinor = DOS_INT21_SHELL_VERSION_MINOR;  /* what XP's COMMAND.COM demands */
    machine->IsBreakOn = 0;  /* BREAK=OFF, DOS's default. [CAUTION] m is a stack local and this
                               function sets fields one by one -- nothing zeroes it */
    machine->VirtualDrive = -1;  /* the current drive is the process current directory's */
    machine->PspSegment = DOS_PSP_SEG;
    {   INT index2;          /* s91: the JFT a fresh PSP carries (DosPspBuild) */
        static const BYTE initialJft[DOS_INT21_STD_HANDLES] = { DOS_PSP_JFT_STDIN_ENTRY, DOS_PSP_JFT_STDOUT_ENTRY, DOS_PSP_JFT_STDERR_ENTRY, DOS_PSP_JFT_AUX_ENTRY, DOS_PSP_JFT_PRN_ENTRY };

        for (index2 = 0; index2 < DOS_PSP_JFT_HANDLES; ++index2)
            machine->JftKnown[index2] = index2 < DOS_INT21_STD_HANDLES ? initialJft[index2] : DOS_PSP_JFT_CLOSED;

        for (index2 = 0; index2 < DOS_SFT_INDEXES; ++index2)
            machine->SftHost[index2] = 0; }

    machine->IsExecPending = 0;
    machine->IsTsrPending = 0;
    machine->TsrKeep = 0;
    machine->FirstMcb = firstMcb;
    machine->DtaSegment = DOS_PSP_SEG;
    machine->DtaOffset = DOS_PSP_COMMAND_TAIL_LENGTH;
    machine->OutputLength = 0;
    machine->IsOutputTruncated = 0;
    machine->IsLineActive = 0;
    machine->LineLength = 0;
    machine->LineSegment = 0;
    machine->LineOffset = 0;
    machine->TraceCount = 0;
    machine->StdOpen = DOS_INT21_STD_OPEN_ALL;                /* stdin/stdout/stderr/aux/prn all open */
    {
        INT index3;

        for (index3 = 0; index3 < DOS_SERVICE_BITS; ++index3)
        {
            machine->Unimplemented[index3] = 0;
            machine->Undefined[index3] = 0;
        }
    }
    machine->ExitCode = 0;
    /* GH #28: default to 6.22 so we match the oracle. It is also the friendlier
     * lie -- most version checks are floor checks, and real 6.22 tools refuse to
     * run at all under a lower number ("Incorrect DOS version" from MEM.EXE was
     * the first thing the evidence pass hit).
     */
    machine->VersionMajor = DOS_INT21_DEFAULT_VERSION_MAJOR;
    machine->VersionMinor = DOS_INT21_DEFAULT_VERSION_MINOR;
    /* Oracle-confirmed 6.22 defaults: 5800h -> AX=0000 (first fit),
     * 5802h -> AL=00 (UMBs not linked).
     */
    machine->AllocationStrategy = 0;
    machine->UmbLink = 0;
    machine->SysvarsSegment = 0;
    machine->SysvarsOffset = 0;
    machine->ConsoleOut = 0;
    machine->ConsoleOutContext = 0;
    machine->ConsoleIn = 0;
    machine->ConsoleInContext = 0;
    machine->ConsoleInNoWait = 0;
    machine->ConsolePeek = 0;
}

/* See the header. The comment at AH=30h has promised this function since GH #28;
 * COMMAND.COM is what finally needed it.
 */
/* PER-PROCESS HANDLE TABLES. (s81) See DOS_MACHINE::hsave for why: */
/* Does any SAVED (i.e. parent's) table still hold this Win32 handle? Then a child
 * closing or overwriting it must not CloseHandle it -- the parent gets it back.
 */
static INT DosHandleIsHeldByParent(PCDOS_MACHINE machine, HANDLE handle)
{
    INT depth;
    INT index;

    if (!handle)
        return 0;

    for (depth = 0; depth < machine->HandleDepth && depth < DOS_HANDLE_STACK_DEPTH; ++depth)
        for (index = 0; index < DOS_MAX_FILES; ++index)
            if (machine->HandleStack[depth].FileHandles[index] == handle)
                return 1;

    return 0;
}

/* Take a Win32 handle out of the current table: closed for real only if no parent
 * still holds it. Every site that used to CloseHandle(m->fh[x]) comes through here.
 */
VOID DosHandleRelease(PDOS_MACHINE machine, UINT slot)
{
    if (slot >= DOS_MAX_FILES || !machine->FileHandles[slot])
        return;

    if (!DosHandleIsHeldByParent(machine, machine->FileHandles[slot]))
        CloseHandle(machine->FileHandles[slot]);

    machine->FileHandles[slot] = 0;
}

VOID DosHandlesPush(PDOS_MACHINE machine)
{
    INT index;

    if (machine->HandleDepth >= DOS_HANDLE_STACK_DEPTH) /* too deep: counted, not saved */
    {
        ++machine->HandleDepth;
        return;
    }

    for (index = 0; index < DOS_MAX_FILES; ++index)
        machine->HandleStack[machine->HandleDepth].FileHandles[index] = machine->FileHandles[index];

    machine->HandleStack[machine->HandleDepth].StdOpen = machine->StdOpen;

    for (index = 0; index < DOS_PSP_JFT_HANDLES; ++index)
        machine->HandleStack[machine->HandleDepth].JftKnown[index] = machine->JftKnown[index];

    ++machine->HandleDepth;
}

/* s91: THE JFT (see jft_known in dos_int21.h): */
static volatile BYTE *DosJftOf(WORD psp, UINT *count)
{
    volatile BYTE *pspBytes = (volatile BYTE *)(ULONG_PTR)((DWORD)psp << PARAGRAPH_SHIFT);
    UINT jftSize;
    UINT jftOffset;
    UINT jftSegment;

    *count = 0;

    if (!psp)
        return NULL;

    jftSize = (UINT)(pspBytes[DOS_PSP_JFT_SIZE] | (pspBytes[DOS_PSP_JFT_SIZE + 1] << BYTE_SHIFT));
    jftOffset = (UINT)(pspBytes[DOS_PSP_JFT_POINTER] | (pspBytes[DOS_PSP_JFT_POINTER + 1] << BYTE_SHIFT));
    jftSegment = (UINT)(pspBytes[DOS_PSP_JFT_POINTER + 2] | (pspBytes[DOS_PSP_JFT_POINTER + 3] << BYTE_SHIFT));

    if (!jftSegment || !jftSize)
        return NULL;

    *count = jftSize > DOS_PSP_JFT_HANDLES ? DOS_PSP_JFT_HANDLES : jftSize;
    return (volatile BYTE *)(ULONG_PTR)(((DWORD)jftSegment << PARAGRAPH_SHIFT) + jftOffset);
}

/* The pseudo SFT index for what handle h is bound to now. */
static BYTE DosSftValue(PDOS_MACHINE machine, UINT handle)
{
    UINT value;
    UINT freeValue = 0;

    if (handle < DOS_MAX_FILES && machine->FileHandles[handle])
    {
        for (value = DOS_INT21_SFT_FIRST_HOST; value < DOS_INT21_SFT_LAST; ++value)
        {
            if (machine->SftHost[value] == machine->FileHandles[handle])
                return (BYTE)value;

            if (!machine->SftHost[value] && !freeValue)
                freeValue = value;
        }

        if (!freeValue)                     /* table full: forget the stale entries */
        {
            for (value = DOS_INT21_SFT_FIRST_HOST; value < DOS_INT21_SFT_LAST; ++value)
                machine->SftHost[value] = 0;

            freeValue = DOS_INT21_SFT_FIRST_HOST;
        }

        machine->SftHost[freeValue] = machine->FileHandles[handle];
        return (BYTE)freeValue;
    }

    if (handle < DOS_INT21_STD_OPEN_BITS && (machine->StdOpen & (1u << handle)))
        return (BYTE)(handle == DOS_INT21_STDAUX_HANDLE ? DOS_PSP_JFT_AUX_ENTRY : handle == DOS_INT21_STDPRN_HANDLE ? DOS_PSP_JFT_PRN_ENTRY : DOS_PSP_JFT_STDIN_ENTRY);

    return DOS_PSP_JFT_CLOSED;
}

static VOID DosJftPut(PDOS_MACHINE machine, UINT handle, BYTE value)
{
    UINT count;
    volatile BYTE *jft = DosJftOf(machine->PspSegment, &count);

    if (jft && handle < count)
    {
        jft[handle] = value;
        machine->JftKnown[handle] = value;
    }
}

VOID DosJftReset(PDOS_MACHINE machine)
{
    UINT count;
    UINT handle;
    volatile BYTE *jft = DosJftOf(machine->PspSegment, &count);

    for (handle = 0; handle < DOS_PSP_JFT_HANDLES; ++handle)
        machine->JftKnown[handle] = (jft && handle < count) ? jft[handle] : DOS_PSP_JFT_CLOSED;
}

VOID DosJftExec(PDOS_MACHINE machine, WORD childPsp)
{
    UINT count;
    UINT childCount;
    UINT handle;
    volatile BYTE *jft = DosJftOf(machine->PspSegment, &count);
    volatile BYTE *childJft = DosJftOf(childPsp, &childCount);

    if (!jft)
        return;

    /* Only the five STANDARD handles: shell redirection is all this is for, and the
     * final s91 regression run showed a Win16 task's file create coming back as handle
     * 18h instead of 6 once higher slots were re-bound from a JFT we do not own.
     */
    for (handle = 0; handle < count && handle < DOS_INT21_STD_HANDLES; ++handle)
    {
        BYTE value = jft[handle];

        if (value == machine->JftKnown[handle])
            continue;                                      /* ours: fh[] already says so */

        if (value == DOS_PSP_JFT_CLOSED)
        {
            machine->FileHandles[handle] = 0;

            if (handle < DOS_INT21_STD_OPEN_BITS)
                machine->StdOpen &= ~(1u << handle);
        }
        else if (value <= DOS_PSP_JFT_PRN_ENTRY)
        {
            machine->FileHandles[handle] = 0;

            if (handle < DOS_INT21_STD_OPEN_BITS)
                machine->StdOpen |= (1u << handle);
        }
        else if (machine->SftHost[value])
        {
            machine->FileHandles[handle] = machine->SftHost[value];
        }
    }

    for (handle = 0; handle < count && childJft && handle < childCount; ++handle)
        childJft[handle] = jft[handle];                                                                            /* DOS copies the JFT */

    for (handle = 0; handle < DOS_PSP_JFT_HANDLES; ++handle)
        machine->JftKnown[handle] = (childJft && handle < childCount) ? childJft[handle] : DOS_PSP_JFT_CLOSED;
}

VOID DosHandlesPop(PDOS_MACHINE machine, INT isTsr)
{
    INT index;
    INT depth;

    if (machine->HandleDepth <= 0)
        return;

    depth = --machine->HandleDepth;

    if (depth >= DOS_HANDLE_STACK_DEPTH)
        return;                                              /* matched an unsaved push */

    /* What the child still has open and the parent never had: DOS closes those at
     * terminate. Checked against the parent table being restored, and against every
     * older one, so nothing a caller further up holds is touched. A TSR keeps its.
     */
    if (!isTsr)
        for (index = 0; index < DOS_MAX_FILES; ++index)
        {
            HANDLE handle = machine->FileHandles[index];
            INT index2;
            INT isDuplicate = 0;

            if (!handle || DosHandleIsHeldByParent(machine, handle))
                continue;

            for (index2 = 0; index2 < DOS_MAX_FILES; ++index2)
                if (machine->HandleStack[depth].FileHandles[index2] == handle)
                {
                    isDuplicate = 1;
                    break;
                }

            for (index2 = 0; index2 < index && !isDuplicate; ++index2)
                if (machine->FileHandles[index2] == handle)
                    isDuplicate = 1;                                                                                                 /* closed already */

            if (!isDuplicate)
                CloseHandle(handle);
        }

    for (index = 0; index < DOS_MAX_FILES; ++index)
        machine->FileHandles[index] = machine->HandleStack[depth].FileHandles[index];

    machine->StdOpen = machine->HandleStack[depth].StdOpen;

    for (index = 0; index < DOS_PSP_JFT_HANDLES; ++index)
        machine->JftKnown[index] = machine->HandleStack[depth].JftKnown[index];
}

VOID DosInt21SetShellPsp(PDOS_MACHINE machine, WORD psp, INT isOn)
{
    INT index;

    for (index = 0; index < DOS_SHELL_PSP_SLOTS; ++index)
        if (machine->ShellPsps[index] == psp)
            machine->ShellPsps[index] = 0;

    if (isOn) for (index = 0; index < DOS_SHELL_PSP_SLOTS; ++index) if (!machine->ShellPsps[index])
    {
        machine->ShellPsps[index] = psp;
        break;
    }
}

/* The version THIS process is told -- see DOS_MACHINE::v5_psp. */
static WORD DosVersionWord(PCDOS_MACHINE machine)
{
    INT index;

    for (index = 0; index < DOS_SHELL_PSP_SLOTS; ++index)
        if (machine->ShellPsps[index] && machine->ShellPsps[index] == machine->PspSegment)
            return (WORD)((machine->ShellVersionMinor << BYTE_SHIFT) | machine->ShellVersionMajor);

    return (WORD)((machine->VersionMinor << BYTE_SHIFT) | machine->VersionMajor);
}

VOID DosInt21SetVersion(PDOS_MACHINE machine, BYTE major, BYTE minor)
{
    if (!machine || !major)
        return;                                 /* major 0 is not a DOS version */

    machine->VersionMajor = major;
    machine->VersionMinor = minor;
}

static UINT64 DosFileTime64(const FILETIME *fileTime)
{
    return ((UINT64)fileTime->dwHighDateTime << DWORD_SHIFT) | fileTime->dwLowDateTime;
}

/* Local time for a DOS-format answer (SI=1), as every DOS time this host reports is
 * local (DosDtaFill, 5700h). A FILETIME answer (SI=0) is Win32's own, i.e. UTC.
 */
static UINT64 DosFileTimeZoned(const FILETIME *fileTime, INT isLocal)
{
    FILETIME localTime;

    if (isLocal && (fileTime->dwLowDateTime || fileTime->dwHighDateTime) && FileTimeToLocalFileTime(fileTime, &localTime))
        return DosFileTime64(&localTime);

    return DosFileTime64(fileTime);
}

static VOID DosLfnFindFill(
    volatile BYTE *destination,
    const WIN32_FIND_DATAA *findData,
    INT isDosFormat)
{
    BYTE record[DOS_LFN_FIND_RECORD_SIZE];
    DOS_LFN_FIND_ENTRY entry;
    INT index;

    entry.Attributes = findData->dwFileAttributes;
    entry.CreationTime = DosFileTimeZoned(&findData->ftCreationTime, isDosFormat);
    entry.LastAccessTime = DosFileTimeZoned(&findData->ftLastAccessTime, isDosFormat);
    entry.LastWriteTime = DosFileTimeZoned(&findData->ftLastWriteTime, isDosFormat);
    entry.SizeHigh = findData->nFileSizeHigh;
    entry.SizeLow = findData->nFileSizeLow;
    entry.LongName = findData->cFileName;
    entry.ShortName = findData->cAlternateFileName;
    DosLfnFindPack(record, &entry, isDosFormat);

    for (index = 0; index < DOS_LFN_FIND_RECORD_SIZE; ++index)
        destination[index] = record[index];
}

/* The DOS error for a failed LFN call, from the Win32 one (dos_lfn.h). */
static WORD DosLfnError(DWORD win32Error)
{
    WORD dosError = DOS_ERR_FILE_NOT_FOUND;

    (VOID)DosLfnErrFromWin32((unsigned long)win32Error, &dosError);
    return dosError;
}

/* Open a file OR a directory just to set its times (7143h BL=3/5/7). Directories need
 * FILE_FLAG_BACKUP_SEMANTICS; sharing is everything, as 3Dh's is.
 */
static HANDLE DosLfnOpenAttributes(PCSTR fileName)
{
    return CreateFileA(fileName, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
}

/* THE CALL SITE, AND THE CODE AROUND IT:
 * Used on the terminate paths, where "an address says WHERE a guest gave up, never
 * WHY" -- the standing SILENT VDM DEATH -> GET THE BYTES rule -- and the bytes only
 * answer that if they are the GUEST's, at the address the guest actually called
 * from. `seg:off` here is the pushed return address, so the `CD 21` that got us
 * here begins two bytes earlier; that is the site, and the branch that chose it is
 * upstream of it.
 *
 * [CAUTION]: 24 bytes before the site and 8 after. Disassemble with `ndisasm -b 16 -o <addr>`.
 *
 * [CAUTION]: Clamped at the segment base: a low return offset must not read below zero.
 */
static PSTR DosInt21CallSite(PSTR trace, INT isFramed, DWORD segment, DWORD offset)
{
    DWORD base;
    DWORD site;
    DWORD low;
    DWORD count;
    DWORD index;
    const volatile BYTE *bytes;

    if (!isFramed)
        return LogPut(trace, " from=<PM: no pushed frame>");

    trace = LogPut(trace, " from=0x"); trace = LogHex(trace, segment);
    trace = LogPut(trace, ":0x");      trace = LogHex(trace, offset);
    site = (offset >= X86_INT_LENGTH) ? offset - X86_INT_LENGTH : 0;      /* the CD 21 itself */
    trace = LogPut(trace, " site=0x");  trace = LogHex(trace, site);
    base = (segment & WORD_MASK) << PARAGRAPH_SHIFT;
    low   = (site >= DOS_INT21_CALLSITE_BEFORE) ? site - DOS_INT21_CALLSITE_BEFORE : 0;
    bytes    = (const volatile BYTE *)(ULONG_PTR)(base + low);
    count    = (site - low) + DOS_INT21_CALLSITE_AFTER;
    trace = LogPut(trace, " bytes@0x"); trace = LogHex(trace, low); trace = LogPut(trace, "=");

    for (index = 0; index < count && index < DOS_INT21_CALLSITE_MAX; ++index)
    {
        trace = LogHexByte(trace, bytes[index]);
        trace = LogPut(trace, " ");
    }

    return trace;
}

INT DosInt21(PDOS_MACHINE machine)
{
    volatile BYTE *tib = machine->Tib;
    PSTR trace = machine->TraceCursor;
    volatile WORD *guestFlags;
    DWORD function;
    INT shouldContinue = 1;
    /* the guest's own call site -- below */
    DWORD callSegment = 0;
    DWORD callOffset = 0;
    INT   isCallFramed  = 0;
    BYTE isLfnAlias = 0;                        /* #210: the AL of a 71xxh served by xxh */

    #define R_AX VDM_REG(tib, VTIB_EAX)
    #define R_BX VDM_REG(tib, VTIB_EBX)
    #define R_CX VDM_REG(tib, VTIB_ECX)
    #define R_DX VDM_REG(tib, VTIB_EDX)
    #define R_DS VDM_REG(tib, VTIB_DS)
    #define R_ES VDM_REG(tib, VTIB_ES)
    #define R_SI VDM_REG(tib, VTIB_ESI)
    #define R_DI VDM_REG(tib, VTIB_EDI)
    #define SETAX(value)        (R_AX = (R_AX & HIGH_WORD_MASK_U) | ((DWORD)(value) & WORD_MASK))
    #define SET16(reg, value)   ((reg)  = ((reg)  & HIGH_WORD_MASK_U) | ((DWORD)(value) & WORD_MASK))
    #define OKCF()      (*guestFlags &= (WORD)~EFLAGS_CF)
    #define ERRCF()     (*guestFlags |= EFLAGS_CF)
    #define SETZF()     (*guestFlags |= DOS_INT21_ZERO_FLAG)
    #define CLRZF()     (*guestFlags &= (WORD)~DOS_INT21_ZERO_FLAG)
    /* Dropping output silently once cost a wrong conclusion: a probe's dump was
     * cut mid-line, the harness saw fewer results than it asked for, and the
     * missing rows read as agreement. Record the drop so the log can say so.
     */
    /* AH=02h/09h WRITE TO STANDARD OUTPUT, NOT TO THE SCREEN:
     * That distinction is invisible until something redirects, and then it is the
     * whole feature: ECHO does not use AH=40h, it prints with AH=02h, so a shell
     * doing `echo hello > hi.txt` sends the text through here. With this macro
     * hard-wired to the console sink the redirect was accepted, the file was
     * created, the text went to the SCREEN, and hi.txt was left empty at 0 bytes.
     * Fixing AH=40h alone did not move it -- measured, twice -- because ECHO never
     * goes near AH=40h.
     * A bound handle 1 is a file (see the note there); an unbound one is the
     * console, which is the ordinary case and behaves exactly as before.
     */
    #define OUTC(outputValue)     do { BYTE outputChar = (BYTE)(outputValue); \
        if (machine->FileHandles[1]) { DWORD outputWritten = 0; WriteFile(machine->FileHandles[1], &outputChar, 1, &outputWritten, NULL); } \
        else { \
            if (machine->OutputLength < machine->OutputCapacity - 1) machine->Output[machine->OutputLength++] = (CHAR)outputChar; \
            else machine->IsOutputTruncated = 1; \
            if (machine->ConsoleOut) machine->ConsoleOut(machine->ConsoleOutContext, outputChar); \
        } } while (0)

    /* CF is returned via the FLAGS the INT pushed on the V86 stack (SS:SP+4): the
     * handler's IRET restores FLAGS from there, so the live EFlags get clobbered.
     * - NOT IN PROTECTED MODE. A DPMI client's INT 21h is serviced by the host with the
     *   guest still in PM, where SS holds a SELECTOR -- so `SS<<4` is not the stack at
     *   all (0x1f -> linear 0x1f0) and this would both fail to return CF and scribble
     *   on low memory. There is no pushed-FLAGS frame to honour there either: the PM
     *   dispatcher resumes the client by advancing EIP past the BOP, so the live
     *   VTIB_EFLAGS *is* what the client sees. Point at its low word, which carries
     *   CF/ZF. Set by the PM caller via DosInt21SetProtectedMode().
     */
    guestFlags = g_DosInt21IsProtectedMode
        ? (volatile WORD *)(tib + VTIB_EFLAGS)
        : (volatile WORD *)(((VDM_REG(tib, VTIB_SS) & WORD_MASK) << PARAGRAPH_SHIFT)
                            + (((VDM_REG(tib, VTIB_ESP) & WORD_MASK) + X86_FRAME16_FLAGS) & WORD_MASK));
    function = (R_AX >> BYTE_SHIFT) & BYTE_MASK;
    machine->Trampoline = 0;
    /* #251: resume the V86 guest in the AUX/PRN driver code -- see dos_auxprn.asm. */
    #define AUXPRN_TRAMP(entry) (machine->Trampoline = (WORD)(DOS_AUXPRN_OFF + (entry)))
    #define AUXPRN_V86      (machine->CanTrampoline && !g_DosInt21IsProtectedMode)
    machine->IsCritPending = 0;  /* #34: only ever about THIS call; see the tail */
    g_DosReadWriteDrive = -1;  /* #275: likewise */

    /* WHO CALLED, OFF THE GUEST STACK (Importance = 2):
     * VTIB_CS:EIP is where the HANDLER is, not where the guest is. Last session
     * read a terminate site out of it, found `C4 C4 54` there, and built two
     * readings on top of a byte dump of the wrong address -- both since
     * retracted. A real-mode INT pushes IP, CS, FLAGS, so the true call site is
     * at SS:SP: offset first, then segment. The same three words are already
     * trusted four lines up, where CF is returned via SS:SP+4 -- so this reads
     * the frame the code is ALREADY relying on, and costs one 32-bit load.
     * The technique is not new here either; the XMS entry logger's note says
     * "LOG WHO CALLED, NOT JUST WHAT THEY ASKED".
     *
     * [CAUTION]: REAL MODE ONLY. In PM the dispatcher advances EIP past the BOP -- there is
     * no pushed frame, and SS is a selector, so SS<<4 is not the stack at all
     * (the same trap the pfl note above exists to warn about).
     */
    if (!g_DosInt21IsProtectedMode)
    {
        DWORD stackBase = (VDM_REG(tib, VTIB_SS) & WORD_MASK) << PARAGRAPH_SHIFT;
        DWORD stackPointer = VDM_REG(tib, VTIB_ESP) & WORD_MASK;
        const volatile BYTE *frame = (const volatile BYTE *)(ULONG_PTR)(stackBase + stackPointer);
        callOffset = (DWORD)frame[0] | ((DWORD)frame[1] << BYTE_SHIFT);
        callSegment = (DWORD)frame[X86_FRAME16_CS] | ((DWORD)frame[X86_FRAME16_CS + 1] << BYTE_SHIFT);
        isCallFramed  = 1;
    }

    /* EVERY CALL, WHEN ASKED:
     * Most handlers here trace only what they think is interesting, which is fine
     * until the question is "what does the guest do BETWEEN two calls we can see".
     * COMMAND.COM accepts `ver ` and rejects `ver`, with a provably identical line
     * buffer apart from one space -- so the answer is in the calls it makes after
     * reading the line, and those are exactly the ones nothing prints. Two traces
     * differing by one space is a DIFFERENTIAL experiment, which beats reasoning
     * about a parser we cannot see.
     *
     * [CAUTION]: AH=0Ah is excluded: it now polls via `retry`, so tracing it would bury the
     * log in thousands of identical lines -- it prints its completed line instead.
     * Gated by a flag file so no other run pays for this.
     */
    /* [WARNING] A CAP, AND IT IS THE THIRD INSTRUMENT IN ONE SESSION TO NEED ONE.
     * The BOP logger ran away twice (268 MB each) before it got a hard ceiling; this
     * one has none at all, and the moment XP's COMMAND.COM reached a command LOOP it
     * wrote 2,166,824 trace lines and the same quarter-gigabyte. The flag file makes
     * it opt-in, which is not the same as bounded -- opt-in only says who pays.
     *
     * 4000 lines, then one line saying it stopped and how many it has seen. A trace
     * whose size depends on the guest's loop rate cannot be read either way, and the
     * first 4000 calls are where the answer is.
     */
    if (machine->IsTraceAll && function != DOS_FN_BUFFERED_INPUT && machine->TraceCount <= DOS_TRACE_MAX)
    {
        if (++machine->TraceCount > DOS_TRACE_MAX)
        {
            trace = LogPut(trace, "  21: ... TRACE CAPPED at ");
            trace = LogHex(trace, DOS_TRACE_MAX);
            trace = LogPut(trace, " calls -- the guest is looping; totals are in the summary\r\n");
        }
        else
        {
        trace = LogPut(trace, "  21:"); trace = LogHexByte(trace, (UINT)function);
        trace = LogPut(trace, "/");     trace = LogHexByte(trace, (UINT)(R_AX & BYTE_MASK));
        trace = LogPut(trace, " bx="); trace = LogHexByte(trace, (UINT)((R_BX >> BYTE_SHIFT) & BYTE_MASK));
        trace = LogHexByte(trace, (UINT)(R_BX & BYTE_MASK));
        trace = LogPut(trace, " dx="); trace = LogHexByte(trace, (UINT)((R_DX >> BYTE_SHIFT) & BYTE_MASK));
        trace = LogHexByte(trace, (UINT)(R_DX & BYTE_MASK));
        /* The call site, so a trace of 31 calls says WHERE the guest is, not only
         * what it wanted. Two calls from the same offset are a loop; a run of
         * rising offsets is start-up walking forward.
         */
        if (isCallFramed) { trace = LogPut(trace, " @"); trace = LogHex(trace, callSegment);
                     trace = LogPut(trace, ":");
                     trace = LogHex(trace, callOffset); }

        trace = LogPut(trace, "\r\n");
        }
    }

    /* #210: FIVE LONG-FILENAME CALLS ARE THEIR SHORT-NAME TWINS, REGISTER FOR REGISTER.
     * 7139h mkdir, 713Ah rmdir, 713Bh chdir (DS:DX), 7156h rename (DS:DX -> ES:DI) and
     * 716Ch / 71A9h extended open (BX, CX, DX, DS:SI) take exactly what 39h/3Ah/3Bh/56h/
     * 6Ch take, and none of those arms reads AL. Our short-name arms were never
     * short-name-only -- they hand the string to Win32, which resolves a long name as
     * readily as an 8.3 one -- so the LFN call is served by the same code, error
     * mapping, handle allocation and JFT bookkeeping (the s91 tail below keys on
     * `ah`, which is the point: a 716Ch handle lands in the PSP's JFT like a 6Ch one).
     *
     * [CAUTION]: So these five answer with the short arms' measured 6.22 error codes (39h over an
     * existing name = 5, 3Bh to nowhere = 3, ...). Whether STOCK's LFN arms use the same
     * numbers is p_lfn's lfn.7139.again / lfn.713B.missing rows. 71A9h ("server"
     * open, a global handle on Windows 95) is an ordinary 716Ch here: there is one
     * process and one handle table. Everything else in AH=71h is the arm further down.
     */
    if (function == DOS_FN_LFN)
    {
        BYTE subfunction = (BYTE)(R_AX & BYTE_MASK);

        if (subfunction == DOS_FN_MKDIR || subfunction == DOS_FN_RMDIR || subfunction == DOS_FN_CHDIR || subfunction == DOS_FN_RENAME || subfunction == DOS_FN_EXTENDED_OPEN
            || subfunction == DOS_INT21_LFN_SERVER_OPEN)
        {
            isLfnAlias = subfunction;
            function = (subfunction == DOS_INT21_LFN_SERVER_OPEN) ? DOS_FN_EXTENDED_OPEN : subfunction;
        }
    }

    if (function == DOS_FN_EXIT)                       /* terminate */
    {
        machine->ExitCode = (INT)(R_AX & BYTE_MASK);  /* DOS errorlevel */
        trace = LogPut(trace, "  ==> DOS terminate (AH=4Ch), exit code AL=0x");
        trace = LogHex(trace, R_AX & BYTE_MASK);
        trace = DosInt21CallSite(trace, isCallFramed, callSegment, callOffset);
        trace = LogPut(trace, "\r\n");
        shouldContinue = 0;
    }
    else if (function == DOS_FN_TERMINATE)                /* terminate (CP/M style, = INT 20h) */
    {
        /* Skyroads exits through this one, so "unhandled" was both wrong and misleading:
         * we returned an error and let the guest run on into nowhere. It is just 4Ch with
         * an exit code of 0. Logging the call site because WHY a game terminates is the
         * question, and the CS tells you whether it was the program or something we
         * vectored it into.
         */
        machine->ExitCode = 0;
        trace = LogPut(trace, "  ==> DOS terminate (AH=00h) from CS:IP=0x");
        trace = LogHex(trace, VDM_REG(tib, VTIB_CS) & WORD_MASK); trace = LogPut(trace, ":0x");
        trace = LogHex(trace, VDM_REG(tib, VTIB_EIP) & WORD_MASK);
        trace = LogPut(trace, " ivt8=0x");
        { const volatile BYTE *ivt = (const volatile BYTE *)0;
          DWORD timerSegment = (DWORD)ivt[DOS_INT21_IVT_TIMER + 2] | ((DWORD)ivt[DOS_INT21_IVT_TIMER + 3] << BYTE_SHIFT);
          DWORD timerOffset = (DWORD)ivt[DOS_INT21_IVT_TIMER] | ((DWORD)ivt[DOS_INT21_IVT_TIMER + 1] << BYTE_SHIFT);
          DWORD tickSegment = (DWORD)ivt[DOS_INT21_IVT_USER_TICK + 2] | ((DWORD)ivt[DOS_INT21_IVT_USER_TICK + 3] << BYTE_SHIFT);
          DWORD tickOffset = (DWORD)ivt[DOS_INT21_IVT_USER_TICK] | ((DWORD)ivt[DOS_INT21_IVT_USER_TICK + 1] << BYTE_SHIFT);
          trace = LogHex(trace, timerSegment); trace = LogPut(trace, ":0x"); trace = LogHex(trace, timerOffset);
          trace = LogPut(trace, " ivt1C=0x"); trace = LogHex(trace, tickSegment);
          trace = LogPut(trace, ":0x");
          trace = LogHex(trace, tickOffset); }
        /* AND THE BYTES THAT LED HERE -- AT THE GUEST'S ADDRESS, NOT OURS.
         * "SILENT VDM DEATH -> GET THE BYTES" is a standing rule here, and the
         * first cut of this obeyed the letter of it while dumping from
         * VTIB_CS:EIP -- the HANDLER's address. That produced `C4 C4 54`, read
         * as a BOP marker, and two conclusions that were both retracted a
         * session later. The guest's own call site is the pushed return address
         * on its stack; DosInt21CallSite() dumps around that.
         */
        trace = DosInt21CallSite(trace, isCallFramed, callSegment, callOffset);
        trace = LogPut(trace, "\r\n");
        shouldContinue = 0;
    }
    else if (function == DOS_FN_CHAR_OUTPUT)                /* print char DL */
    {
        OUTC(R_DX & BYTE_MASK);
        OKCF();
    }
    else if ((function == DOS_FN_CHAR_INPUT_ECHO || function == DOS_FN_DIRECT_INPUT || function == DOS_FN_CHAR_INPUT || function == DOS_FN_INPUT_STATUS
                || (function == DOS_FN_DIRECT_CONSOLE_IO && (R_DX & BYTE_MASK) == DOS_INT21_DIRECT_INPUT))
               && DosHandleIsFile((PVOID const *)machine->FileHandles, 0))
    {
        /* stdio (s91): CONSOLE INPUT FROM A FILE ON HANDLE 0. `prog < file` is the
         * shell AH=46h-ing a file onto handle 0, and DOS's console-input functions
         * read HANDLE 0 -- these read the keyboard whatever handle 0 was, so a
         * redirected program never saw its file (and hung waiting for a key).
         * What a file gives back, measured with tests/probes/dos/p_stdin against REAL
         * MS-DOS on a real BIOS (PCem) and stock NTVDM, which agree byte for byte:
         * 01h/07h/08h  the next byte; AT END OF FILE THEY BLOCK -- neither
         *              returns (DOSBox-X answers 0Ah; it is the odd one out).
         *              Blocking here is a retry, so the guest keeps its ISRs.
         * 06h DL=FFh   the next byte, ZF clear; at EOF AL=00h and ZF SET
         * 0Bh          FFh while bytes remain, 00h at EOF
         * (3Fh on handle 0 already reads the file: 0 bytes, CF clear, at EOF.)
         */
        HANDLE file = (HANDLE)machine->FileHandles[0];
        BYTE character = 0;
        DWORD received = 0;

        if (function == DOS_FN_INPUT_STATUS)
        {
            DWORD position = SetFilePointer(file, 0, NULL, FILE_CURRENT);
            DWORD size  = GetFileSize(file, NULL);
            SETAX((R_AX & HIGH_BYTE_MASK) | ((position != INVALID_SET_FILE_POINTER && position < size) ? DOS_INT21_INPUT_READY : DOS_INT21_INPUT_NONE));
            OKCF();
        }
        else if (!ReadFile(file, &character, 1, &received, NULL) || received == 0)
        {
            if (function == DOS_FN_DIRECT_CONSOLE_IO)
            {
                SETAX(R_AX & HIGH_BYTE_MASK);
                SETZF();
                OKCF();
            }
            else
                machine->IsRetry = 1;           /* EOF: block, as DOS does */
        }
        else
        {
            if (function == DOS_FN_CHAR_INPUT_ECHO)
                OUTC(character);

            SETAX((R_AX & HIGH_BYTE_MASK) | character);

            if (function == DOS_FN_DIRECT_CONSOLE_IO)
                CLRZF();

            OKCF();
        }
    }
    else if (function == DOS_FN_CHAR_INPUT_ECHO || function == DOS_FN_DIRECT_INPUT || function == DOS_FN_CHAR_INPUT)    /* read char (01 echoes) */
    {
        /* Poll, do not block. If no key is waiting we ask the host to re-run this INT
         * rather than parking the exec thread -- see `retry` in dos_int21.h.
         */
        INT character = machine->ConsoleInNoWait ? machine->ConsoleInNoWait(machine->ConsoleInContext) : -1;

        if (character < 0)
        {
            machine->IsRetry = 1;
        }
        else
        {
            if (function == DOS_FN_CHAR_INPUT_ECHO)
                OUTC(character);                                      /* AH=01: echo */

            SETAX((R_AX & HIGH_BYTE_MASK) | (character & BYTE_MASK));
            OKCF();
        }
    }
    else if (function == DOS_FN_BUFFERED_INPUT && DosHandleIsFile((PVOID const *)machine->FileHandles, 0))
    {
        /* stdio (s91): the line from a FILE on handle 0 -- bytes up to the CR, the
         * LF a text file puts after it skipped at the start of the next line, each
         * echoed as the keyboard form echoes them. At EOF with nothing read it
         * blocks, as 08h does (p_stdin: PCem + stock).
         */
        volatile BYTE *buffer = (volatile BYTE *)((R_DS << PARAGRAPH_SHIFT) + (R_DX & WORD_MASK));
        INT maximumLength = buffer[0];
        INT length = 0;
        BYTE character;
        DWORD received;
        HANDLE file = (HANDLE)machine->FileHandles[0];

        for (;;)
        {
            if (!ReadFile(file, &character, 1, &received, NULL) || received == 0)
                break;

            if (character == ASCII_LF && length == 0)
                continue;

            if (character == ASCII_CR)
                break;

            if (length < maximumLength - 1)
            {
                buffer[DOS_INT21_LINE_TEXT + length++] = character;
                OUTC(character);
            }
        }

        if (length == 0 && received == 0)
            machine->IsRetry = 1;
        else
        {
            buffer[DOS_INT21_LINE_COUNT] = (BYTE)length;
            buffer[DOS_INT21_LINE_TEXT + length] = ASCII_CR;
            OUTC(ASCII_CR);
            OUTC(ASCII_LF);
            OKCF();
        }
    }
    else if (function == DOS_FN_BUFFERED_INPUT)                /* buffered input DS:DX */
    {
        /* THE LAST INPUT CALL THAT PARKED THE EXEC THREAD, AND IT DEADLOCKS A SHELL.
         * This used to sit in a loop on the BLOCKING m->conin until it had a whole
         * line. AH=01/07/08 and INT 16h were both fixed years ago to poll via
         * `retry` (see the note on that field), and the reason is spelled out
         * there: blocking in C stops the GUEST dead. For a game that meant a frozen
         * screen. For COMMAND.COM it means never running at all, because the thing
         * it is waiting for CANNOT ARRIVE while it waits:
         *     COMMAND.COM -> INT 21h AH=0Ah -> we block on the BIOS key ring
         *     ...the BIOS key ring is filled by the guest's own INT 09h ISR
         *     ...which cannot run, because we are blocked inside its INT 21h call.
         * Measured exactly that way: the shell printed its banner and prompt, then
         * 40 scancodes went into the FIFO and IRQ1 was attempted 691 times, EVERY
         * one refused as `not_in_exec`. The keys were there the whole time and the
         * guest was never running to take them.
         * - SO COLLECT THE LINE ACROSS RETRIES. The characters accumulate in the
         *   GUEST's buffer (untouched between retries) and we keep only our position
         *   in it; a different DS:DX is a different call, not a continuation. Each
         *   retry leaves EIP on the BOP, so the guest re-executes the INT and gets to
         *   run its ISRs in between -- which is what a real DOS does, since the BIOS
         *   spins in the guest with interrupts enabled.
         */
        volatile BYTE *buffer = (volatile BYTE *)((R_DS << PARAGRAPH_SHIFT) + (R_DX & WORD_MASK));
        INT maximumLength = buffer[0];
        INT character;

        if (!machine->IsLineActive || machine->LineSegment != (WORD)(R_DS & WORD_MASK)
                            || machine->LineOffset != (WORD)(R_DX & WORD_MASK))
        {
            machine->IsLineActive = 1;
            machine->LineLength = 0;
            machine->LineSegment = (WORD)(R_DS & WORD_MASK);
            machine->LineOffset = (WORD)(R_DX & WORD_MASK);
        }

        for (;;)
        {
            if (machine->LineLength >= maximumLength - 1)
                break;                                            /* buffer full -> take it as a line */

            character = machine->ConsoleInNoWait ? machine->ConsoleInNoWait(machine->ConsoleInContext) : ASCII_CR;

            if (character < 0) /* nothing yet -> let the guest run */
            {
                machine->IsRetry = 1;
                break;
            }

            /* s91: a LF at the START of a line is the tail of the previous line's
             * CR LF in a redirected file (`command < script`, cmd's `<` lands on the
             * host stdin this reads). Every line after the first began with it and
             * 6.22's COMMAND.COM answered "Bad command or file name" to each.
             */
            if (character == ASCII_LF && machine->LineLength == 0)
                continue;

            if (character == ASCII_CR)
            {
                machine->IsLineActive = 0;
                break;
            }

            if (character == ASCII_BACKSPACE)                      /* backspace: rub it out on screen */
            {
                if (machine->LineLength > 0)
                {
                    --machine->LineLength;
                    OUTC(ASCII_BACKSPACE);
                    OUTC(' ');
                    OUTC(ASCII_BACKSPACE);
                }

                continue;
            }

            if (character == ASCII_NUL)
                continue;                                    /* extended key: no ASCII, ignore */

            buffer[DOS_INT21_LINE_TEXT + machine->LineLength++] = (BYTE)character;
            OUTC(character);
        }

        if (!machine->IsRetry)
        {
            buffer[DOS_INT21_LINE_COUNT] = (BYTE)machine->LineLength;
            buffer[DOS_INT21_LINE_TEXT + machine->LineLength] = ASCII_CR;
            OUTC(ASCII_CR);
            OUTC(ASCII_LF);
            machine->IsLineActive = 0;
            /* WHAT THE SHELL ACTUALLY RECEIVES. `echo hi` works while a bare `ver`
             * comes back "Bad command or file name" -- and the difference between
             * them is a SPACE, i.e. whether the command word ends at a delimiter or
             * at our terminator. That points straight at these bytes, so print them
             * rather than reason about them.
             */
            if (machine->IsTraceAll) { INT index;
              trace = LogPut(trace, "  INT21 AH=0A line max="); trace = LogHexByte(trace, (UINT)maximumLength);
              trace = LogPut(trace, " n="); trace = LogHexByte(trace, (UINT)machine->LineLength);
              trace = LogPut(trace, " [");

              for (index = 0; index < machine->LineLength + 1 && index < DOS_INT21_TRACE_LINE_MAX; ++index)
              {
                  trace = LogHexByte(trace, buffer[DOS_INT21_LINE_TEXT + index]); trace = LogPut(trace, " ");
              }

              trace = LogPut(trace, "]\r\n"); }

            OKCF();
        }
    }
    else if (function == DOS_FN_INPUT_STATUS)                /* check input status */
    {
        INT isReady = machine->ConsolePeek ? machine->ConsolePeek(machine->ConsoleInContext) : 0;
        SETAX((R_AX & HIGH_BYTE_MASK) | (isReady ? DOS_INT21_INPUT_READY : DOS_INT21_INPUT_NONE));  /* FFh = char waiting */
        OKCF();
    }
    else if (function == DOS_FN_DIRECT_CONSOLE_IO)                /* direct console I/O (DL=FF -> read) */
    {
        if ((R_DX & BYTE_MASK) == DOS_INT21_DIRECT_INPUT)              /* input: non-blocking, ZF=1 if none */
        {
            INT character = machine->ConsoleInNoWait ? machine->ConsoleInNoWait(machine->ConsoleInContext) : -1;

            if (character >= 0)
            {
                SETAX((R_AX & HIGH_BYTE_MASK) | (character & BYTE_MASK));
                CLRZF();
            }
            else
            {
                SETAX(R_AX & HIGH_BYTE_MASK);
                SETZF();
            }
        }
        else                                  /* output: write DL, AL=DL */
        {
            OUTC(R_DX & BYTE_MASK);
            SETAX((R_AX & HIGH_BYTE_MASK) | (R_DX & BYTE_MASK));
        }

        OKCF();
    }
    else if (function == DOS_FN_PRINT_STRING)                /* print $-string DS:DX */
    {
        const volatile BYTE *text = (const volatile BYTE *)((R_DS << PARAGRAPH_SHIFT) + (R_DX & WORD_MASK));
        INT index;

        for (index = 0; index < DOS_INT21_PRINT_STRING_MAX && *text != '$'; ++index, ++text)
            OUTC(*text);

        OKCF();
    }
    else if (function == DOS_FN_WRITE)                /* write: BX=handle CX=cnt DS:DX=buf */
    {
        DWORD handle = R_BX & WORD_MASK;
        DWORD count = R_CX & WORD_MASK;
        PCSTR buffer = (PCSTR)((R_DS << PARAGRAPH_SHIFT) + (R_DX & WORD_MASK));
        /* HANDLES 0-4 ARE TABLE ENTRIES, NOT A SPECIAL CASE:
         * DOS pre-opens stdin/stdout/stderr/aux/prn as ordinary slots in the same
         * handle table as everything else, and that is precisely WHY redirection
         * works: the shell opens the file, dup2s it over handle 1, runs the
         * command, then dup2s the saved copy back. Treating 1 and 2 as "the
         * console, always" accepted the redirect and then ignored it -- measured,
         * `echo hello world > hi.txt` printed to the screen and left an EMPTY
         * hi.txt on disk, which is the worst of both.
         * So: a BOUND handle is a file, whatever its number; only an unbound low
         * handle is the console.
         */
        /* #275: A WRITE THAT FAILS FOR A HARDWARE REASON IS A CRITICAL ERROR. The
         * result of WriteFile was never looked at: a write to a file whose floppy
         * was pulled, or that went write-protected, answered CF=0 with however many
         * bytes Win32 managed (usually 0) -- a success that never happened. A
         * hardware error (19-31, same numbers on both sides) now goes back as that
         * code with CF=1, which the tail turns into the program's INT 24h (or, where
         * INT 24h cannot be raised, into the answer FAIL gives).
         *
         * [CAUTION]: ANY OTHER FAILURE KEEPS THE OLD ANSWER, deliberately: disk full is CF=0
         * with a short count on DOS too, and the rest (access denied on a read-only
         * handle = DOS 5) is a separate, unmeasured question.
         */
        if (DosHandleIsFile((PVOID const *)machine->FileHandles, handle))
        {
            DWORD written = 0;
            DWORD win32Error = 0;
            WORD dosError = 0;

            if (!WriteFile(machine->FileHandles[handle], buffer, count, &written, NULL))
                win32Error = GetLastError();

            if (win32Error && DosErrFromWin32((unsigned long)win32Error, &dosError) && DosCritIsHardwareError(dosError))
            {
                g_DosReadWriteDrive = DosHandleDrive(machine->FileHandles[handle]);
                SETAX(dosError);
                ERRCF();
                trace = LogPut(trace, "  INT21 AH=40 h="); trace = LogHex(trace, handle);
                trace = LogPut(trace, " cnt=0x"); trace = LogHex(trace, count);
                trace = LogPut(trace, " FAILED win32=0x"); trace = LogHex(trace, win32Error);
                trace = LogPut(trace, " (hardware) drive=");

                if (g_DosReadWriteDrive >= 0)
                {
                    CHAR driveText[DOS_INT21_DRIVE_ROOT_SIZE] = { (CHAR)('A' + g_DosReadWriteDrive), ':', 0 };
                    trace = LogPut(trace, driveText);
                }
                else
                    trace = LogPut(trace, "?");

                trace = LogPut(trace, "\r\n");
            }
            else
            {
                SETAX(written);
                OKCF();
                DosStampVdmNow(machine->FileHandles[handle]); /* #263 */
            }
        }
        /* #251: AN UNREDIRECTED 3 IS AUX AND 4 IS PRN, and they go to the BIOS
         * (INT 14h / INT 17h) like DOS's own drivers -- they used to be refused
         * with error 6 here, after AH=04h/05h had thrown their bytes away.
         */
        else if ((handle == DOS_INT21_STDAUX_HANDLE || handle == DOS_INT21_STDPRN_HANDLE) && DosHandleIsDevice((PVOID const *)machine->FileHandles, machine->StdOpen, handle))
        {
            if (AUXPRN_V86)
                AUXPRN_TRAMP(handle == DOS_INT21_STDPRN_HANDLE ? DOS_AUXPRN_PRN_WRITE : DOS_AUXPRN_AUX_WRITE);
            else
            {
                DWORD index;

                for (index = 0; index < count; ++index)
                {
                    if (handle == DOS_INT21_STDPRN_HANDLE)
                    {
                        if (machine->PrinterOut)
                            (VOID)machine->PrinterOut(machine->DeviceContext, (BYTE)buffer[index]);
                    }
                    else if (machine->AuxOut)
                        machine->AuxOut(machine->DeviceContext, (BYTE)buffer[index]);
                }

                SETAX(count);
                OKCF();
            }
        }
        /* [CAUTION]: ANY device slot, not just 1 and 2 -- after AH=45h the console can be
         * sitting in slot 5. A duplicate loses which device it was, so a dup of
         * AUX would print here; nothing does that, and the alternative is a
         * per-slot identity byte we have no caller for.
         */
        else if (DosHandleIsDevice((PVOID const *)machine->FileHandles, machine->StdOpen, handle))
             {
                 DWORD index;

                 for (index = 0; index < count; ++index)
                     OUTC(buffer[index]);

                 SETAX(count);
                 OKCF();
             }
        else
        {
            SETAX(DOS_ERR_INVALID_HANDLE);
            ERRCF();
        }
    }
    else if (function == DOS_FN_CREATE || function == DOS_FN_OPEN)    /* create / open: DS:DX=ASCIIZ name */
    {
        /* DOS HANDS OUT THE LOWEST FREE HANDLE, AND THAT IS HOW `>` WORKS:
         * COMMAND.COM does not redirect with dup2. It CLOSES handle 1 and then
         * creates the target, relying on the new file landing in the slot the
         * console just vacated -- measured, the trace is `3Ch create` followed
         * immediately by `40h write to handle 1` with no 45h/46h anywhere.
         * Allocating from 5 upwards, as this did, makes that impossible: the file
         * got handle 5, handle 1 was still the console, so the text went to the
         * screen and the file stayed 0 bytes. Two earlier fixes (AH=40h, then
         * AH=02h) were aimed at the write end and neither moved it, because the
         * write end was never wrong -- the HANDLE NUMBER was.
         */
        /* WE DO NOT EMULATE SHARE.EXE, SO WE MUST NOT ENFORCE IT. (session 37):
         * FILE_SHARE_READ here means a second open of a file this VDM already holds
         * for writing fails with ERROR_SHARING_VIOLATION -- a lock bare DOS does not
         * have, reported back as DOS error 2 "file not found", which sends the guest
         * looking for a file that is there. The protected-mode twin of this call cost
         * the GDI.EXE wall exactly that way. Share everything; the access mode below
         * still comes from the guest.
         */
        CHAR fileName[DOS_INT21_PATH_SIZE];
        DWORD slot;
        HANDLE file;
        DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE;
        DosGuestPath(machine, R_DS, R_DX, fileName, sizeof(fileName));

        if (function == DOS_FN_CREATE)
            file = CreateFileA(fileName, GENERIC_READ | GENERIC_WRITE, share,
                            NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        else
        {
            DWORD mode = R_AX & DOS_INT21_OPEN_ACCESS_MASK;
            DWORD access = (mode == DOS_INT21_OPEN_WRITE) ? GENERIC_WRITE
                      : (mode == DOS_INT21_OPEN_READ_WRITE) ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_READ;
            file = DosOpenStampable(fileName, access, share, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
        }

        if (file != INVALID_HANDLE_VALUE)
        {
            slot = DosHandleAllocate((PVOID const *)machine->FileHandles, machine->StdOpen);

            if (slot < DOS_MAX_FILES) { machine->FileHandles[slot] = file;
            SETAX(slot);
            OKCF();

                                        if (function == DOS_FN_CREATE)
                                            DosStampVdmNow(file); /* #263 */ }
            else
            {
                CloseHandle(file);
                SETAX(DOS_ERR_TOO_MANY_OPEN_FILES);
                ERRCF();
            }
        }
        else
        {
            /* ASK WHY IT FAILED. It used to answer 2 for every cause; see
             * DosErrFromWin32() for the two oracle rows that names wrong.
             */
            DWORD win32Error = GetLastError();
            WORD dosError;
            INT isMapped = DosErrFromWin32((unsigned long)win32Error, &dosError);
            SETAX(dosError);
            ERRCF();
            trace = LogPut(trace, "  INT21 AH=0x"); trace = LogHex(trace, function);
            trace = LogPut(trace, " ["); trace = LogPut(trace, fileName); trace = LogPut(trace, "] FAILED win32=0x");
            trace = LogHex(trace, win32Error);
            trace = LogPut(trace, isMapped ? " -> AX=0x" : " UNMAPPED, kept -> AX=0x");
            trace = LogHex(trace, dosError); trace = LogPut(trace, "\r\n");
        }

        trace = LogPut(trace, "  INT21 AH=0x"); trace = LogHex(trace, function);
        trace = LogPut(trace, " ["); trace = LogPut(trace, fileName); trace = LogPut(trace, "] -> AX=0x");
        trace = LogHex(trace, R_AX & WORD_MASK); trace = LogPut(trace, (*guestFlags & 1) ? " (err)\r\n" : "\r\n");
    }
    else if (function == DOS_FN_CLOSE)                /* close: BX=handle */
    {
        DWORD handle = R_BX & WORD_MASK;
        /* Any BOUND handle closes, including a low one the shell redirected -- see
         * the note at AH=40h. An unbound 0-4 is the console and closing it is a no-op.
         */
        if (DosHandleIsFile((PVOID const *)machine->FileHandles, handle))
            DosHandleRelease(machine, handle);
        else
            DosHandleSetDevice(&machine->StdOpen, handle, FALSE);   /* free the device slot */

        OKCF();
    }
    else if (function == DOS_FN_READ)                /* read: BX=handle CX=cnt -> DS:DX */
    {
        DWORD handle = R_BX & WORD_MASK;
        DWORD count = R_CX & WORD_MASK;
        DWORD read = 0;
        PVOID buffer = (VOID *)((R_DS << PARAGRAPH_SHIFT) + (R_DX & WORD_MASK));

        if (DosHandleIsFile((PVOID const *)machine->FileHandles, handle))    /* bound -> a file, even if low */
        {
            /* - LOG THE FILE POSITION, THE COUNT AND THE FIRST BYTES. A DOS extender
             * loading an executable is doing nothing but seek+read, so if the image it
             * ends up with is wrong, the first question is whether WE handed it the
             * right bytes -- and that is answerable offline by comparing these lines
             * against the file. Without the position a short or misplaced read is
             * indistinguishable from a correct one.
             */
            DWORD position = SetFilePointer(machine->FileHandles[handle], 0, NULL, FILE_CURRENT);
            /* #275: and a read that FAILS for a hardware reason is a critical error --
             * see AH=40h; same rule, same reasons for leaving every other failure
             * alone (it used to answer them all as CF=0 with what Win32 read).
             */
            DWORD win32Error = 0;
            WORD dosError = 0;

            if (!ReadFile(machine->FileHandles[handle], buffer, count, &read, NULL))
                win32Error = GetLastError();

            if (win32Error && DosErrFromWin32((unsigned long)win32Error, &dosError) && DosCritIsHardwareError(dosError))
            {
                g_DosReadWriteDrive = DosHandleDrive(machine->FileHandles[handle]);
                SETAX(dosError);
                ERRCF();
                trace = LogPut(trace, "  INT21 AH=3F FAILED win32=0x"); trace = LogHex(trace, win32Error);
                trace = LogPut(trace, " (hardware) drive=");

                if (g_DosReadWriteDrive >= 0)
                {
                    CHAR driveText[DOS_INT21_DRIVE_ROOT_SIZE] = { (CHAR)('A' + g_DosReadWriteDrive), ':', 0 };
                    trace = LogPut(trace, driveText);
                }
                else
                    trace = LogPut(trace, "?");

                trace = LogPut(trace, "\r\n");
            }
            else
            {
                SETAX(read);
                OKCF();
            }

            trace = LogPut(trace, "  INT21 AH=3F h="); trace = LogHex(trace, handle);
            trace = LogPut(trace, " pos=0x"); trace = LogHex(trace, position);
            trace = LogPut(trace, " cnt=0x"); trace = LogHex(trace, count);
            trace = LogPut(trace, " got=0x"); trace = LogHex(trace, read);
            trace = LogPut(trace, " -> 0x"); trace = LogHex(trace, (DWORD)(ULONG_PTR)buffer);
            trace = LogPut(trace, " first="); trace = LogDump(trace, (PCBYTE)buffer, (read >= DOS_INT21_TRACE_FIRST_BYTES) ? DOS_INT21_TRACE_FIRST_BYTES : 0);
            trace = LogPut(trace, "\r\n");
        }
        else if (handle == 0 && DosHandleIsDevice((PVOID const *)machine->FileHandles, machine->StdOpen, handle))
        {
            /* #251: STDIN IS THE KEYBOARD, AND DOS READS A LINE FROM IT:
             * This answered 0 bytes -- end of file -- so any program reading its
             * input through handle 0 (C's gets/scanf/fgets do) saw an empty stream
             * and gave up. DOS's console read is cooked: it echoes, honours
             * backspace, ends at Enter, and returns the line WITH CR LF; a line
             * longer than the caller asked for is handed out over later reads.
             * Collected across retries like AH=0Ah, so the guest keeps running
             * (and taking its interrupts) while it waits for keys.
             */
            volatile BYTE *bytes = (volatile BYTE *)buffer;
            INT character;
            DWORD transferred = 0;

            if (machine->ConsolePosition >= machine->ConsoleLength)    /* nothing pending: collect a line */
            {
                if (!machine->IsConsoleCollecting)
                {
                    machine->IsConsoleCollecting = 1;
                    machine->ConsoleTyped = 0;
                }

                for (;;)
                {
                    character = machine->ConsoleInNoWait ? machine->ConsoleInNoWait(machine->ConsoleInContext) : ASCII_CR;

                    if (character < 0)
                    {
                        machine->IsRetry = 1;
                        break;
                    }

                    if (character == ASCII_CR)
                        break;

                    if (character == ASCII_BACKSPACE)
                    {
                        if (machine->ConsoleTyped > 0)
                        {
                            --machine->ConsoleTyped;
                            OUTC(ASCII_BACKSPACE);
                            OUTC(' ');
                            OUTC(ASCII_BACKSPACE);
                        }

                        continue;
                    }

                    if (character == ASCII_NUL)
                        continue;                            /* extended key: no ASCII */

                    if (machine->ConsoleTyped >= DOS_INT21_CONSOLE_LINE_MAX)
                        continue;                                                       /* full: only Enter ends it */

                    machine->ConsoleLine[machine->ConsoleTyped++] = (BYTE)character;
                    OUTC(character);
                }

                if (machine->IsRetry)
                    goto readDone;

                machine->ConsoleLine[machine->ConsoleTyped] = ASCII_CR;
                machine->ConsoleLine[machine->ConsoleTyped + 1] = ASCII_LF;
                machine->ConsoleLength = machine->ConsoleTyped + DOS_INT21_CRLF_LENGTH;
                machine->ConsolePosition = 0;
                machine->IsConsoleCollecting = 0;
                OUTC(ASCII_CR);
                OUTC(ASCII_LF);
            }

            while (transferred < count && machine->ConsolePosition < machine->ConsoleLength) bytes[transferred++] = machine->ConsoleLine[machine->ConsolePosition++];

            SETAX(transferred);
            OKCF();
        readDone: ;
        }
        else if (handle == DOS_INT21_STDAUX_HANDLE && AUXPRN_V86 && DosHandleIsDevice((PVOID const *)machine->FileHandles, machine->StdOpen, handle))
             AUXPRN_TRAMP(DOS_AUXPRN_AUX_READ);       /* #251: AUX, through INT 14h */
        else if (DosHandleIsDevice((PVOID const *)machine->FileHandles, machine->StdOpen, handle))
             { /* PRN, a dup, or AUX in PM: EOF */
                 SETAX(0);
                 OKCF();
             }
        else
        {
            SETAX(DOS_ERR_INVALID_HANDLE);
            ERRCF();
        }
    }
    else if (function == DOS_FN_SEEK)                /* lseek: AL=org BX=h CX:DX=off */
    {
        DWORD handle = R_BX & WORD_MASK;
        DWORD method = R_AX & BYTE_MASK;
        LONG distance = (LONG)(((R_CX & WORD_MASK) << WORD_SHIFT) | (R_DX & WORD_MASK));
        /* A BOUND HANDLE IS A FILE, WHATEVER ITS NUMBER. (GH #133):
         * This read `h >= 5`, and that is how `>>` was broken while `>` worked:
         * the shell redirects stdout by closing handle 1 and opening the
         * target into the slot it vacates, then seeks to end-of-file before
         * appending. Excluding handles below 5 refused that seek with error 6
         * on a handle that IS a file -- the last survivor of #133, after the
         * create and both write paths had been fixed.
         * Oracle, tests/probes/dos/p_redir.asm on MS-DOS 6.22:
         * CASE=int21.42.end.on.h1 SIG=AX,DX,CF AX=0004 DX=0000 CF=0
         * i.e. real DOS seeks handle 1 to the end and reports 4 bytes.
         */
        if (DosHandleIsFile((PVOID const *)machine->FileHandles, handle))
        {
            DWORD newPosition = SetFilePointer(machine->FileHandles[handle], distance, NULL, method);
            SETAX(newPosition & WORD_MASK);
            R_DX = (R_DX & HIGH_WORD_MASK_U) | ((newPosition >> WORD_SHIFT) & WORD_MASK);
            OKCF();
        }
        else
        {
            SETAX(DOS_ERR_INVALID_HANDLE);
            ERRCF();
        }
    }
    else if (function == DOS_FN_GET_VERSION)                /* get DOS version */
    {
        /* AL=major, AH=minor, BH=OEM, BL:CX=24-bit serial.  GH #28.
         * BX and CX were never written before, so a caller saw whatever it had
         * left in them and read that as our OEM number and serial.  Values
         * confirmed against the 6.22 oracle: BH=0xFF (generic MS-DOS), serial 0.
         * The version itself is configurable -- see DosInt21SetVersion().
         */
        SETAX(DosVersionWord(machine));         /* per process: see v5_psp (#208) */
        SET16(R_BX, DOS_INT21_OEM_SERIAL_HIGH);                    /* BH=OEM 0xFF, BL=serial high */
        SET16(R_CX, DOS_INT21_SERIAL_LOW);                    /* serial low */
        OKCF();
    }
    else if (function == DOS_FN_FIND_FIRST || function == DOS_FN_FIND_NEXT)    /* find first / find next */
    {
        volatile BYTE *dta = (volatile BYTE *)((machine->DtaSegment << PARAGRAPH_SHIFT) + machine->DtaOffset);
        WIN32_FIND_DATAA findData;
        WORD mask;
        INT slot = -1;
        INT isOk = 0;

        if (function == DOS_FN_FIND_FIRST)
        {
            CHAR pattern[DOS_INT21_PATH_SIZE];
            DosGuestPath(machine, R_DS, R_DX, pattern, sizeof(pattern));
            mask = (WORD)(R_CX & WORD_MASK);

            for (slot = 0; slot < DOS_FIND_SLOTS && machine->FindHandles[slot]; ++slot)
            {
            }

            if (slot >= DOS_FIND_SLOTS) { slot = 0;                       /* recycle the oldest */
                             FindClose(machine->FindHandles[0]);
                             machine->FindHandles[0] = 0; }

            { CHAR directoryPattern[DOS_INT21_PATH_SIZE];
            BYTE nameTemplate[DOS_FCB_NAME_SIZE];
            INT isNoDirectory;
              HANDLE find;
              DosFindSplit(pattern, directoryPattern, sizeof directoryPattern, nameTemplate);  /* DOS matching: see DosFindMatches */
              find = DosFindFirst(directoryPattern, nameTemplate, mask, &findData, &isNoDirectory);

              if (machine->IsTraceAll) { trace = LogPut(trace, "  INT21 AH=4E ["); trace = LogPut(trace, pattern);
                                  trace = LogPut(trace, "] attr=0x"); trace = LogHex(trace, mask);
                                  trace = LogPut(trace, find == INVALID_HANDLE_VALUE ? " -> none" : " -> found");
                                  trace = LogPut(trace, isNoDirectory ? " (no such directory)\r\n" : "\r\n"); }

              if (find == INVALID_HANDLE_VALUE)
              {
                  /* ORACLE-CONFIRMED, and not what memory suggests: a pattern
                   * that matches nothing inside an EXISTING directory is
                   * AX=18 "no more files", not AX=2 "file not found". A missing
                   * directory is AX=3.
                   */
                  SETAX(isNoDirectory ? DOS_ERR_PATH_NOT_FOUND : DOS_ERR_NO_MORE_FILES);
                  /* #34: a HARDWARE failure (not ready, write-protected, ...) is its
                   * own DOS code -- Win32 kept DOS's numbers for 19-31 -- and the
                   * dispatcher's tail turns it into an INT 24h.
                   */
                  if (g_DosFindWin32Error >= DOS_INT21_HARD_ERROR_FIRST && g_DosFindWin32Error <= DOS_INT21_HARD_ERROR_LAST)
                      SETAX(g_DosFindWin32Error);

                  ERRCF();
              }
              else
              {
                  machine->FindHandles[slot] = find;
                  isOk = 1;
                  /* Fill DOS's private search area deterministically.  It is
                   * DOS-private, but leaving the caller's bytes lying in it
                   * means the DTA differs run to run for no reason; real 6.22
                   * puts the EXPANDED 11-byte search template there (a "*.*"
                   * search reads back as eleven '?'), so do the same.
                   */
                  /* The template, exactly as matched -- 4Fh reads it back from here. */
                  {
                      INT nameIndex;

                      for (nameIndex = 0; nameIndex < DOS_FCB_NAME_SIZE; ++nameIndex)
                          dta[DOS_INT21_FCB_NAME + nameIndex] = nameTemplate[nameIndex];
                  }
                  dta[0] = DOS_INT21_FIND_DRIVE_C;                                /* drive C: */
                  dta[DOS_INT21_FIND_MASK] = (BYTE)(mask & BYTE_MASK);
                  dta[DOS_INT21_FIND_RESERVED] = 0;
                  dta[DOS_INT21_FIND_RESERVED + 1] = 0;
                  dta[DOS_INT21_FIND_RESERVED + 2] = 0;
                  dta[DOS_INT21_FIND_RESERVED + 3] = 0;
                  dta[DOS_INT21_FIND_RESERVED + 4] = 0;
                  dta[DOS_INT21_FIND_RESERVED + 5] = 0;
                  dta[DOS_INT21_FIND_TAG] = DOS_FIND_MAGIC;
                  dta[DOS_INT21_FIND_SLOT] = (BYTE)slot;
              }
            }
        }
        else                                               /* 4Fh: continue */
        {
            mask = (WORD)dta[DOS_INT21_FIND_MASK];

            if (dta[DOS_INT21_FIND_TAG] == DOS_FIND_MAGIC && dta[DOS_INT21_FIND_SLOT] < DOS_FIND_SLOTS && machine->FindHandles[dta[DOS_INT21_FIND_SLOT]])
            {
                BYTE nameTemplate[DOS_FCB_NAME_SIZE];
                INT index;
                slot = dta[DOS_INT21_FIND_SLOT];

                for (index = 0; index < DOS_FCB_NAME_SIZE; ++index)
                    nameTemplate[index] = dta[DOS_INT21_FCB_NAME + index];                                                  /* the template 4Eh stored */

                isOk = DosFindNext(machine->FindHandles[slot], nameTemplate, mask, &findData);
            }
            else
            {
                SETAX(DOS_ERR_NO_MORE_FILES);
                ERRCF();                          /* no search live */
            }
        }

        if (slot >= 0 && isOk)
        {
            DosDtaFill(dta, &findData);
            /* DIR renders blank names, one impossible size repeated, and a 1980-ish
             * date -- i.e. it is reading fields we did not put where it looks. Print
             * the DTA we hand back, whole, and let the bytes settle it.
             */
            if (machine->IsTraceAll) { INT position;
              trace = LogPut(trace, "  INT21 AH=4E/4F dta="); trace = LogHexByte(trace, (UINT)((machine->DtaSegment >> BYTE_SHIFT) & BYTE_MASK));
              trace = LogHexByte(trace, (UINT)(machine->DtaSegment & BYTE_MASK)); trace = LogPut(trace, ":");
              trace = LogHexByte(trace, (UINT)((machine->DtaOffset >> BYTE_SHIFT) & BYTE_MASK));
              trace = LogHexByte(trace, (UINT)(machine->DtaOffset & BYTE_MASK));
              trace = LogPut(trace, " [");

              for (position = 0; position < DOS_INT21_TRACE_DTA_BYTES; ++position)
              {
                  trace = LogHexByte(trace, (UINT)dta[position]);
                  trace = LogPut(trace, " ");
              }

              trace = LogPut(trace, "]\r\n"); }

            SETAX(0);
            OKCF();                                /* oracle: AX=0000 */
        }
        else if (slot >= 0 && machine->FindHandles[slot] && !isOk)
        {
            FindClose(machine->FindHandles[slot]);
            machine->FindHandles[slot] = 0;
            dta[DOS_INT21_FIND_TAG] = 0;
            SETAX(DOS_ERR_NO_MORE_FILES);
            ERRCF();                              /* no more files */
        }
    }
    else if ((function >= DOS_FN_FCB_OPEN && function <= DOS_FN_FCB_RENAME) || (function >= DOS_FN_FCB_READ_RANDOM && function <= DOS_FN_FCB_SET_RANDOM_RECORD)
               || (function >= DOS_FN_FCB_READ_BLOCK && function <= DOS_FN_PARSE_FILENAME))    /* ---- the FCB interface ---- */
    {
        volatile BYTE *fcb = DosFcbAt(R_DS, R_DX);
        CHAR name[DOS_INT21_PATH_SIZE];
        #define FCB_OK()   SETAX((R_AX & HIGH_BYTE_MASK) | 0x00)
        #define FCB_FAIL() SETAX((R_AX & HIGH_BYTE_MASK) | 0xFF)
        /* CF is undefined for these on real DOS; leave it as the guest set it. */
        if (function == DOS_FN_FCB_OPEN || function == DOS_FN_FCB_CREATE)    /* open / create */
        {
            HANDLE file;
            DWORD slot;
            DosFcbName(fcb, name);
            file = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL,
                              (function == DOS_FN_FCB_CREATE) ? CREATE_ALWAYS : OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, NULL);

            if (file == INVALID_HANDLE_VALUE)
                FCB_FAIL();
            else
            {
                FILETIME fileTime;
                FILETIME localTime;
                WORD dosDate = 0;
                WORD dosTime = 0;
                DWORD size = GetFileSize(file, NULL);
                slot = DosHandleAllocate((PVOID const *)machine->FileHandles, machine->StdOpen);

                if (slot >= DOS_MAX_FILES)
                {
                    CloseHandle(file);
                    FCB_FAIL();
                }
                else
                {
                    machine->FileHandles[slot] = file;

                    if (function == DOS_FN_FCB_CREATE)
                        DosStampVdmNow(file);                                 /* #263, before the FCB reads it */

                    if (GetFileTime(file, NULL, NULL, &fileTime)
                        && FileTimeToLocalFileTime(&fileTime, &localTime))
                        FileTimeToDosDateTime(&localTime, &dosDate, &dosTime);

                    fcb[DOS_INT21_FCB_BLOCK] = 0;
                    fcb[DOS_INT21_FCB_BLOCK + 1] = 0;
                    fcb[DOS_INT21_FCB_RECORD_SIZE] = DOS_INT21_FCB_DEFAULT_RECORD;
                    fcb[DOS_INT21_FCB_RECORD_SIZE + 1] = 0;     /* oracle: record size 128 */
                    fcb[DOS_INT21_FCB_FILE_SIZE] = (BYTE)(size & BYTE_MASK);
                    fcb[DOS_INT21_FCB_FILE_SIZE + 1] = (BYTE)((size >> BYTE_SHIFT) & BYTE_MASK);
                    fcb[DOS_INT21_FCB_FILE_SIZE + 2] = (BYTE)((size >> WORD_SHIFT) & BYTE_MASK);
                    fcb[DOS_INT21_FCB_FILE_SIZE + 3] = (BYTE)((size >> TOP_BYTE_SHIFT) & BYTE_MASK);
                    fcb[DOS_INT21_FCB_DATE] = (BYTE)(dosDate & BYTE_MASK);
                    fcb[DOS_INT21_FCB_DATE + 1] = (BYTE)(dosDate >> BYTE_SHIFT);
                    fcb[DOS_INT21_FCB_TIME] = (BYTE)(dosTime & BYTE_MASK);
                    fcb[DOS_INT21_FCB_TIME + 1] = (BYTE)(dosTime >> BYTE_SHIFT);
                    /* DOS replaces a "default drive" 0 with the drive it
                     * actually resolved -- measured: the oracle returns 01 when
                     * run from A:, DOSBox 03 from C:. We were leaving the
                     * caller's 0 in place.
                     */
                    if (!fcb[0])
                        fcb[0] = (BYTE)(DosCurrentDrive(machine) + 1);

                    fcb[DOS_INT21_FCB_TAG] = DOS_FCB_MAGIC;
                    fcb[DOS_INT21_FCB_HANDLE] = (BYTE)slot;
                    FCB_OK();
                }
            }
        }
        else if (function == DOS_FN_FCB_CLOSE)            /* close */
        {
            if (fcb[DOS_INT21_FCB_TAG] == DOS_FCB_MAGIC && fcb[DOS_INT21_FCB_HANDLE] < DOS_MAX_FILES && machine->FileHandles[fcb[DOS_INT21_FCB_HANDLE]])
            {
                DosHandleRelease(machine, fcb[DOS_INT21_FCB_HANDLE]);
                fcb[DOS_INT21_FCB_TAG] = 0;
                FCB_OK();
            }
            else
                FCB_FAIL();
        }
        else if (function == DOS_FN_FCB_FIND_FIRST || function == DOS_FN_FCB_FIND_NEXT)    /* find first / find next */
        {
            volatile BYTE *dta = (volatile BYTE *)((machine->DtaSegment << PARAGRAPH_SHIFT) + machine->DtaOffset);
            WIN32_FIND_DATAA findData;
            INT received = 0;
            /* An extended FCB carries its search attribute in the byte just
             * before the part DosFcbAt() returns; a normal one asks for ordinary
             * files only.  WITHOUT THIS FILTER the search returns "." first --
             * measured: our DTA came back with a blank name where the oracle had
             * COMMAND.COM, because "." has no 8.3 name to put in the field.
             */
            WORD mask = (fcb != (volatile BYTE *)((R_DS << PARAGRAPH_SHIFT) + (R_DX & WORD_MASK)))
                           ? (WORD)fcb[-1] : 0;
            /* A VOLUME-LABEL SEARCH IS NOT A FILE SEARCH:
             * Attribute 08h means "return the volume label and nothing else", and
             * it is how DIR fills in its header line. There is no file on disk to
             * match, so FindFirstFile cannot answer it -- we used to run the
             * ordinary search and hand back whatever came first, which is why DIR
             * announced `Volume in drive C is COMMAND COM`, the first file in the
             * directory wearing the label's clothes.
             * The label is 11 bytes in the name+ext field, NOT an 8.3 name, so it
             * is padded raw rather than through DosFcbPutName.
             */
            if (mask == DOS_INT21_ATTRIBUTE_VOLUME)
            {
                if (function == DOS_FN_FCB_FIND_FIRST)
                {
                    CHAR volume[DOS_INT21_VOLUME_SIZE];
                    INT volumeIndex;
                    volatile BYTE *extension;
                    INT isExtended = (fcb != (volatile BYTE *)((R_DS << PARAGRAPH_SHIFT) + (R_DX & WORD_MASK))) ? DOS_INT21_XFCB_HEADER : 0;
                    volume[0] = 0;

                    if (!GetVolumeInformationA("C:\\", volume, sizeof volume,
                                               NULL, NULL, NULL, NULL, 0) || !volume[0])
                    {
                        if (machine->FcbFind)
                        {
                            FindClose(machine->FcbFind);
                            machine->FcbFind = 0;
                        }

                        machine->LastError = DOS_ERR_NO_MORE_FILES;
                        FCB_FAIL();                       /* no label: DIR says so */
                        goto fcbDone;
                    }

                    extension = dta + isExtended;

                    if (isExtended)
                    {
                        INT position;
                        dta[0] = DOS_INT21_XFCB_FLAG;

                        for (position = 1; position < DOS_INT21_XFCB_ATTRIBUTE; ++position)
                            dta[position] = 0;

                        dta[DOS_INT21_XFCB_ATTRIBUTE] = DOS_INT21_ATTRIBUTE_VOLUME;
                    }

                    extension[0] = DOS_INT21_FIND_DRIVE_C;                     /* drive C: */

                    for (volumeIndex = 0; volumeIndex < DOS_FCB_NAME_SIZE; ++volumeIndex)
                    {
                        CHAR character = volume[volumeIndex] ? volume[volumeIndex] : ' ';

                        if (!volume[volumeIndex])
                        {
                            extension[1 + volumeIndex] = ' ';
                            continue;
                        }

                        extension[DOS_INT21_FCB_NAME + volumeIndex] = (BYTE)(character >= 'a' && character <= 'z' ? character - ASCII_CASE_BIT : character);
                    }

                    extension[DOS_INT21_DIRENTRY_ATTRIBUTE] = DOS_INT21_ATTRIBUTE_VOLUME;                 /* attribute: volume label */
                    {
                        INT position;

                        for (position = DOS_INT21_DIRENTRY_RESERVED; position <= DOS_INT21_DIRENTRY_LAST; ++position)
                            extension[position] = 0;
                    }

                    if (machine->FcbFind)
                    {
                        FindClose(machine->FcbFind);
                        machine->FcbFind = 0;
                    }

                    FCB_OK();
                }
                else /* 12h: only ever one label */
                {
                    machine->LastError = DOS_ERR_NO_MORE_FILES;
                    FCB_FAIL();
                }

                goto fcbDone;
            }

            if (function == DOS_FN_FCB_FIND_FIRST)
            {
                /* The FCB's own 11 bytes ARE the template (`????????.???` for DIR), matched
                 * against each entry's 8.3 name -- see DosFindMatches. The drive byte
                 * picks the directory: "X:*" is that drive's current directory.
                 */
                HANDLE find;
                CHAR allPattern[DOS_INT21_ALL_PATTERN_SIZE];
                INT index;
                INT isNoDirectory;
                INT count = 0;

                if (machine->FcbFind)
                {
                    FindClose(machine->FcbFind);
                    machine->FcbFind = 0;
                }

                for (index = 0; index < DOS_FCB_NAME_SIZE; ++index)
                    machine->FcbTemplate[index] = fcb[DOS_INT21_FCB_NAME + index];

                if (fcb[0])
                {
                    allPattern[count++] = (CHAR)('A' + fcb[0] - 1);
                    allPattern[count++] = ':';
                }

                allPattern[count++] = '*';
                allPattern[count] = 0;
                find = DosFindFirst(allPattern, machine->FcbTemplate, mask, &findData, &isNoDirectory);

                if (find != INVALID_HANDLE_VALUE)
                {
                    machine->FcbFind = find;
                    received = 1;
                }

                if (machine->IsTraceAll) { CHAR currentDirectory[MAX_PATH];
                INT position;
                    GetCurrentDirectoryA(sizeof currentDirectory, currentDirectory);
                    trace = LogPut(trace, "  INT21 AH=11 ["); trace = LogPut(trace, allPattern);
                    trace = LogPut(trace, "] in ["); trace = LogPut(trace, currentDirectory); trace = LogPut(trace, "] tmpl=[");

                    for (position = 0; position < DOS_FCB_NAME_SIZE; ++position)
                    {
                        CHAR pair[2];
                        pair[0] = (CHAR)machine->FcbTemplate[position];
                        pair[1] = 0;
                        trace = LogPut(trace, pair);
                    }

                    trace = LogPut(trace, "] mask=0x"); trace = LogHex(trace, mask);
                    trace = LogPut(trace, received ? " -> found\r\n" : " -> none\r\n"); }
            }
            else if (machine->FcbFind)
            {
                received = DosFindNext(machine->FcbFind, machine->FcbTemplate, mask, &findData);

                if (!received)
                {
                    FindClose(machine->FcbFind);
                    machine->FcbFind = 0;
                }
            }

            /* A FAILED SEARCH MUST SAY WHY, OR THE LAST FAILURE SPEAKS FOR IT:
             * The extended error (AH=59h) is only recorded where CF comes back set,
             * and FCB calls deliberately leave CF alone -- so an exhausted search
             * left `last_err` holding whatever failed previously. In a shell that
             * is COMMAND.COM's own startup probe: it asks AH=48h for 0xFFFF
             * paragraphs to learn the largest block, which fails with code 8.
             * DIR then ends its listing, asks AH=59h why, is told "insufficient
             * memory", and prints exactly that instead of its summary line. The
             * listing was RIGHT and the epitaph was three commands stale.
             * 18 = "no more files", which is what DOS reports here.
             */
            if (!received)
            {
                machine->LastError = DOS_ERR_NO_MORE_FILES;
                FCB_FAIL();
            }
            else
            {
                PCSTR baseName = findData.cAlternateFileName[0] ? findData.cAlternateFileName
                                                          : findData.cFileName;
                FILETIME localTime;
                WORD dosDate = 0;
                WORD dosTime = 0;
                INT index;
                /* AN EXTENDED SEARCH RETURNS AN EXTENDED RESULT:
                 * We already skip the 7-byte prefix on the way IN (DosFcbAt), and
                 * then wrote the answer back in the SHORT layout regardless -- so
                 * a caller that searched with an extended FCB read every field
                 * seven bytes early. DIR does exactly that (it must, to see
                 * directories and the volume label), which is why its listing came
                 * out with blank names, one impossible size repeated down the
                 * column, and a volume label of "COM" -- the tail of COMMAND.COM
                 * read as an 11-byte label.
                 * The prefix is FFh, five reserved bytes, then the attribute of
                 * the file found; the ordinary result follows it unchanged.
                 */
                INT isExtended = (fcb != (volatile BYTE *)((R_DS << PARAGRAPH_SHIFT) + (R_DX & WORD_MASK))) ? DOS_INT21_XFCB_HEADER : 0;
                volatile BYTE *extension = dta + isExtended;

                if (FileTimeToLocalFileTime(&findData.ftLastWriteTime, &localTime))
                    FileTimeToDosDateTime(&localTime, &dosDate, &dosTime);

                if (isExtended)
                {
                    dta[0] = DOS_INT21_XFCB_FLAG;

                    for (index = 1; index < DOS_INT21_XFCB_ATTRIBUTE; ++index)
                        dta[index] = 0;

                    dta[DOS_INT21_XFCB_ATTRIBUTE] = (BYTE)(findData.dwFileAttributes & DOS_LFN_ATTRIBUTE_MASK);
                }

                extension[0] = DOS_INT21_FIND_DRIVE_C;                         /* drive C: */
                DosFcbPutName(extension + 1, baseName);
                extension[DOS_INT21_DIRENTRY_ATTRIBUTE] = (BYTE)(findData.dwFileAttributes & DOS_LFN_ATTRIBUTE_MASK);

                for (index = DOS_INT21_DIRENTRY_RESERVED; index < DOS_INT21_DIRENTRY_TIME; ++index)
                    extension[index] = 0;

                extension[DOS_INT21_DIRENTRY_TIME] = (BYTE)(dosTime & BYTE_MASK);
                extension[DOS_INT21_DIRENTRY_TIME + 1] = (BYTE)(dosTime >> BYTE_SHIFT);
                extension[DOS_INT21_DIRENTRY_DATE] = (BYTE)(dosDate & BYTE_MASK);
                extension[DOS_INT21_DIRENTRY_DATE + 1] = (BYTE)(dosDate >> BYTE_SHIFT);
                extension[DOS_INT21_DIRENTRY_CLUSTER] = 0;
                extension[DOS_INT21_DIRENTRY_CLUSTER + 1] = 0;     /* starting cluster */
                extension[DOS_INT21_DIRENTRY_SIZE] = (BYTE)( findData.nFileSizeLow        & BYTE_MASK);
                extension[DOS_INT21_DIRENTRY_SIZE + 1] = (BYTE)((findData.nFileSizeLow >> BYTE_SHIFT)  & BYTE_MASK);
                extension[DOS_INT21_DIRENTRY_SIZE + 2] = (BYTE)((findData.nFileSizeLow >> WORD_SHIFT) & BYTE_MASK);
                extension[DOS_INT21_DIRENTRY_SIZE + 3] = (BYTE)((findData.nFileSizeLow >> TOP_BYTE_SHIFT) & BYTE_MASK);
                FCB_OK();
            }

            fcbDone: ;
        }
        else if (function == DOS_FN_FCB_DELETE)            /* delete (wildcards allowed) */
        {
            WIN32_FIND_DATAA findData;
            HANDLE find;
            INT isFound = 0;
            DosFcbName(fcb, name);
            find = FindFirstFileA(name, &findData);

            if (find != INVALID_HANDLE_VALUE)
            {
                do
                {
                    if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                        continue;

                    if (DeleteFileA(findData.cFileName))
                        isFound = 1;
                } while (FindNextFileA(find, &findData));

                FindClose(find);
            }

            if (isFound)
                FCB_OK();
            else
                FCB_FAIL();
        }
        else if (function == DOS_FN_FCB_RENAME)            /* rename: new name at f[17..27] */
        {
            CHAR destinationPath[DOS_INT21_PATH_SIZE];
            CHAR saved[DOS_INT21_FCB_SAVED_SIZE];
            INT index;
            DosFcbName(fcb, name);

            for (index = 0; index < DOS_INT21_FCB_DRIVE_AND_NAME; ++index)
                saved[index] = (CHAR)fcb[index];

            { volatile BYTE temporary[DOS_INT21_FCB_DRIVE_AND_NAME];
            temporary[0] = fcb[0];

              for (index = 1; index < DOS_INT21_FCB_DRIVE_AND_NAME; ++index)
                  temporary[index] = fcb[DOS_INT21_FCB_NEW_NAME + index];

              DosFcbName(temporary, destinationPath); }

            if (MoveFileA(name, destinationPath))
                FCB_OK();
            else
                FCB_FAIL();

            (VOID)saved;
        }
        else if (function == DOS_FN_FCB_READ_SEQUENTIAL || function == DOS_FN_FCB_WRITE_SEQUENTIAL || function == DOS_FN_FCB_READ_RANDOM || function == DOS_FN_FCB_WRITE_RANDOM
                   || function == DOS_FN_FCB_READ_BLOCK || function == DOS_FN_FCB_WRITE_BLOCK)    /* record I/O */
        {
            volatile BYTE *dta = (volatile BYTE *)((machine->DtaSegment << PARAGRAPH_SHIFT) + machine->DtaOffset);
            DWORD recordSize = (DWORD)fcb[DOS_INT21_FCB_RECORD_SIZE] | ((DWORD)fcb[DOS_INT21_FCB_RECORD_SIZE + 1] << BYTE_SHIFT);
            DWORD block   = (DWORD)fcb[DOS_INT21_FCB_BLOCK] | ((DWORD)fcb[DOS_INT21_FCB_BLOCK + 1] << BYTE_SHIFT);
            DWORD record;
            DWORD count = 1;
            DWORD done = 0;
            DWORD index;
            BYTE buffer[DOS_INT21_FCB_RECORD_BUFFER];

            if (!recordSize)
                recordSize = DOS_INT21_FCB_DEFAULT_RECORD;

            if (recordSize > sizeof(buffer))
                recordSize = sizeof(buffer);

            if (function == DOS_FN_FCB_READ_SEQUENTIAL || function == DOS_FN_FCB_WRITE_SEQUENTIAL)
                record = block * DOS_INT21_FCB_RECORDS_PER_BLOCK + fcb[DOS_INT21_FCB_CURRENT_RECORD];
            else record = (DWORD)fcb[DOS_INT21_FCB_RANDOM_RECORD] | ((DWORD)fcb[DOS_INT21_FCB_RANDOM_RECORD + 1] << BYTE_SHIFT)
                     | ((DWORD)fcb[DOS_INT21_FCB_RANDOM_RECORD + 2] << WORD_SHIFT) | ((DWORD)fcb[DOS_INT21_FCB_RANDOM_RECORD + 3] << TOP_BYTE_SHIFT);

            if (function == DOS_FN_FCB_READ_BLOCK || function == DOS_FN_FCB_WRITE_BLOCK)
                count = R_CX & WORD_MASK;

            if (fcb[DOS_INT21_FCB_TAG] != DOS_FCB_MAGIC || fcb[DOS_INT21_FCB_HANDLE] >= DOS_MAX_FILES || !machine->FileHandles[fcb[DOS_INT21_FCB_HANDLE]])
                SETAX((R_AX & HIGH_BYTE_MASK) | DOS_INT21_FCB_END_OF_FILE);
            else
            {
                HANDLE file = machine->FileHandles[fcb[DOS_INT21_FCB_HANDLE]];
                DWORD length = 0;
                SetFilePointer(file, (LONG)(record * recordSize), NULL, FILE_BEGIN);

                for (index = 0; index < count; ++index)
                {
                    if (function == DOS_FN_FCB_READ_SEQUENTIAL || function == DOS_FN_FCB_READ_RANDOM || function == DOS_FN_FCB_READ_BLOCK)
                    {
                        DWORD index2;

                        if (!ReadFile(file, buffer, recordSize, &length, NULL) || length == 0)
                            break;

                        for (index2 = 0; index2 < recordSize; ++index2)
                            dta[done * recordSize + index2] = (index2 < length) ? buffer[index2] : 0;

                        ++done;

                        if (length < recordSize)
                            break;
                    }
                    else
                    {
                        DWORD index2;

                        for (index2 = 0; index2 < recordSize; ++index2)
                            buffer[index2] = dta[done * recordSize + index2];

                        if (!WriteFile(file, buffer, recordSize, &length, NULL))
                            break;

                        ++done;
                    }
                }

                if (function == DOS_FN_FCB_READ_BLOCK || function == DOS_FN_FCB_WRITE_BLOCK)
                    SET16(R_CX, (WORD)done);

                if (done && !(function == DOS_FN_FCB_READ_SEQUENTIAL || function == DOS_FN_FCB_READ_RANDOM || function == DOS_FN_FCB_READ_BLOCK))
                    DosStampVdmNow(file);               /* #263: an FCB write */

                /* AL: 0 = all done, 1 = end of file / nothing transferred,
                 * 3 = a partial final record.
                 */
                if (done == count)
                    SETAX((R_AX & HIGH_BYTE_MASK) | 0);
                else if (!done)
                    SETAX((R_AX & HIGH_BYTE_MASK) | DOS_INT21_FCB_END_OF_FILE);
                else
                    SETAX((R_AX & HIGH_BYTE_MASK) | DOS_INT21_FCB_PARTIAL_RECORD);

                if (function == DOS_FN_FCB_READ_SEQUENTIAL || function == DOS_FN_FCB_WRITE_SEQUENTIAL)    /* advance sequentially */
                {
                    DWORD nextRecord = record + done;
                    fcb[DOS_INT21_FCB_BLOCK] = (BYTE)((nextRecord / DOS_INT21_FCB_RECORDS_PER_BLOCK) & BYTE_MASK);
                    fcb[DOS_INT21_FCB_BLOCK + 1] = (BYTE)((nextRecord / DOS_INT21_FCB_RECORDS_PER_BLOCK) >> BYTE_SHIFT);
                    fcb[DOS_INT21_FCB_CURRENT_RECORD] = (BYTE)(nextRecord % DOS_INT21_FCB_RECORDS_PER_BLOCK);
                }
                else
                {
                    DWORD nextRecord = record + done;
                    fcb[DOS_INT21_FCB_RANDOM_RECORD] = (BYTE)(nextRecord & BYTE_MASK);
                    fcb[DOS_INT21_FCB_RANDOM_RECORD + 1] = (BYTE)((nextRecord >> BYTE_SHIFT) & BYTE_MASK);
                    fcb[DOS_INT21_FCB_RANDOM_RECORD + 2] = (BYTE)((nextRecord >> WORD_SHIFT) & BYTE_MASK);
                    fcb[DOS_INT21_FCB_RANDOM_RECORD + 3] = (BYTE)((nextRecord >> TOP_BYTE_SHIFT) & BYTE_MASK);
                }
            }
        }
        else if (function == DOS_FN_FCB_FILE_SIZE)            /* get file size, in records */
        {
            HANDLE file;
            DosFcbName(fcb, name);
            file = CreateFileA(name, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

            if (file == INVALID_HANDLE_VALUE)
                FCB_FAIL();
            else
            {
                DWORD size = GetFileSize(file, NULL);
                DWORD recordSize = (DWORD)fcb[DOS_INT21_FCB_RECORD_SIZE] | ((DWORD)fcb[DOS_INT21_FCB_RECORD_SIZE + 1] << BYTE_SHIFT);
                DWORD records;
                CloseHandle(file);

                if (!recordSize)
                    recordSize = DOS_INT21_FCB_DEFAULT_RECORD;

                records = (size + recordSize - 1) / recordSize;
                fcb[DOS_INT21_FCB_RANDOM_RECORD] = (BYTE)(records & BYTE_MASK);
                fcb[DOS_INT21_FCB_RANDOM_RECORD + 1] = (BYTE)((records >> BYTE_SHIFT) & BYTE_MASK);
                fcb[DOS_INT21_FCB_RANDOM_RECORD + 2] = (BYTE)((records >> WORD_SHIFT) & BYTE_MASK);
                fcb[DOS_INT21_FCB_RANDOM_RECORD + 3] = (BYTE)((records >> TOP_BYTE_SHIFT) & BYTE_MASK);
                FCB_OK();
            }
        }
        else if (function == DOS_FN_FCB_SET_RANDOM_RECORD)            /* set random record from current */
        {
            DWORD nextRecord = ((DWORD)fcb[DOS_INT21_FCB_BLOCK] | ((DWORD)fcb[DOS_INT21_FCB_BLOCK + 1] << BYTE_SHIFT)) * DOS_INT21_FCB_RECORDS_PER_BLOCK + fcb[DOS_INT21_FCB_CURRENT_RECORD];
            fcb[DOS_INT21_FCB_RANDOM_RECORD] = (BYTE)(nextRecord & BYTE_MASK);
            fcb[DOS_INT21_FCB_RANDOM_RECORD + 1] = (BYTE)((nextRecord >> BYTE_SHIFT) & BYTE_MASK);
            fcb[DOS_INT21_FCB_RANDOM_RECORD + 2] = (BYTE)((nextRecord >> WORD_SHIFT) & BYTE_MASK);
            fcb[DOS_INT21_FCB_RANDOM_RECORD + 3] = (BYTE)((nextRecord >> TOP_BYTE_SHIFT) & BYTE_MASK);
            OKCF();
        }
        else if (function == DOS_FN_PARSE_FILENAME)            /* parse a filename into an FCB */
        {
            CHAR input[DOS_INT21_PATH_SIZE];
            volatile BYTE *destination = (volatile BYTE *)((R_ES << PARAGRAPH_SHIFT) + (R_DI & WORD_MASK));
            INT inputIndex = 0;
            INT isWild = 0;
            INT index;
            /* AL's CONTROL BITS SAY WHAT A MISSING PART LEAVES ALONE. (s81 sweep) (Importance = 1):
             * bit 1: no drive given -> keep the FCB's drive (else 0 = default)
             * bit 2: no name given  -> keep the FCB's name
             * bit 3: no extension   -> keep the FCB's extension
             * Unanimous on msdos622 / dosbox-x / pcem / pcem-vesa (p_fcb.asm
             * int21.29.keepext/keepall/blank). XP's COMMAND.COM builds DIR's search
             * FCB as ??????????? and parses "*" with AL=0Eh; blanking the extension
             * regardless made the template ????????+3 spaces and DIR listed only
             * `.` and `..` -- the user's sweep finding.
             */
            BYTE subfunction = (BYTE)(R_AX & BYTE_MASK);
            BYTE kept[DOS_INT21_FCB_DRIVE_AND_NAME];
            INT index2;
            INT hasName;
            INT hasExtension;

            for (index = 0; index < DOS_INT21_FCB_DRIVE_AND_NAME; ++index)
                kept[index] = destination[index];

            DosGuestString(R_DS, R_SI, input, sizeof(input));

            while (input[inputIndex] == ' ' || input[inputIndex] == ASCII_TAB)
                ++inputIndex;

            destination[0] = 0;

            if (input[inputIndex] && input[inputIndex + 1] == ':')
            {
                CHAR character = input[inputIndex];
                destination[0] = (BYTE)((character >= 'a' ? character - ASCII_CASE_BIT : character) - 'A' + 1);
                inputIndex += DOS_INT21_DRIVE_PREFIX_LENGTH;
            }
            else if (subfunction & DOS_INT21_PARSE_KEEP_DRIVE)
                destination[0] = kept[0];

            for (index2 = inputIndex; !DosFcbIsNameEnd((BYTE)input[index2]) && input[index2] != '.'; ++index2)
            {
            }

            hasName = (index2 > inputIndex);
            hasExtension  = (input[index2] == '.');
            DosFcbPutName(destination + 1, input + inputIndex);

            if (!hasName && (subfunction & DOS_INT21_PARSE_KEEP_NAME))
                for (index = DOS_INT21_FCB_NAME; index <= DOS_INT21_FCB_BASE_LENGTH;  ++index)
                    destination[index] = kept[index];

            if (!hasExtension  && (subfunction & DOS_INT21_PARSE_KEEP_EXTENSION))
                for (index = DOS_INT21_FCB_EXTENSION; index <= DOS_FCB_NAME_SIZE; ++index)
                    destination[index] = kept[index];

            for (index = DOS_INT21_FCB_NAME; index <= DOS_FCB_NAME_SIZE; ++index)
                if (destination[index] == '?' || destination[index] == '*')
                    isWild = 1;

            for (index = DOS_INT21_FCB_BLOCK; index < DOS_INT21_FCB_FILE_SIZE; ++index)
                destination[index] = 0;

            SETAX((R_AX & HIGH_BYTE_MASK) | (isWild ? 1 : 0));
            SET16(R_SI, (WORD)((R_SI & WORD_MASK) + inputIndex));
            OKCF();
            /* THE CALL COMMAND.COM'S DISPATCH TURNS ON. `ver ` runs and `ver` does
             * not, and the traces diverge on the instruction after the third of
             * these -- so print what went in and what came out, both. Reasoning
             * about it from the handler's source has already produced two wrong
             * models this session.
             */
            if (machine->IsTraceAll) { INT position;
              trace = LogPut(trace, "  INT21 AH=29 al="); trace = LogHexByte(trace, (UINT)(R_AX & BYTE_MASK));
              trace = LogPut(trace, " ds:si="); trace = LogHexByte(trace, (UINT)((R_DS >> BYTE_SHIFT) & BYTE_MASK));
              trace = LogHexByte(trace, (UINT)(R_DS & BYTE_MASK)); trace = LogPut(trace, ":");
              trace = LogHexByte(trace, (UINT)(((R_SI & WORD_MASK) >> BYTE_SHIFT) & BYTE_MASK));
              trace = LogHexByte(trace, (UINT)(R_SI & BYTE_MASK));
              trace = LogPut(trace, " in=[");

              for (position = 0; position < DOS_INT21_FCB_DRIVE_AND_NAME && input[position]; ++position)
                  trace = LogHexByte(trace, (UINT)(BYTE)input[position]), trace = LogPut(trace, " ");

              trace = LogPut(trace, "] fcb=[");

              for (position = 0; position < DOS_INT21_FCB_DRIVE_AND_NAME; ++position)
                  trace = LogHexByte(trace, (UINT)destination[position]), trace = LogPut(trace, " ");

              trace = LogPut(trace, "]\r\n"); }
        }
        else
            FCB_FAIL();
        #undef FCB_OK
        #undef FCB_FAIL
    }
    else if (function == DOS_FN_EXEC)                /* EXEC: load and run a program */
    {
        BYTE subfunction = (BYTE)(R_AX & BYTE_MASK);

        if (subfunction == DOS_INT21_EXEC_LOAD_AND_GO || subfunction == DOS_INT21_EXEC_LOAD_ONLY)
        {
            const volatile BYTE *parameterBlock =
                (const volatile BYTE *)((R_ES << PARAGRAPH_SHIFT) + (R_BX & WORD_MASK));
            /* AL=01 answers THROUGH this block, so remember where it is. */
            machine->ExecBlockSegment = (WORD)(R_ES & WORD_MASK);
            machine->ExecBlockOffset = (WORD)(R_BX & WORD_MASK);
            DosGuestPath(machine, R_DS, R_DX, machine->ExecPath, sizeof(machine->ExecPath));
            DosGuestString(R_DS, R_DX, machine->ExecName, sizeof(machine->ExecName));
            machine->ExecEnvironment      = (WORD)(parameterBlock[0] | (parameterBlock[1] << BYTE_SHIFT));
            machine->ExecTailOffset = (WORD)(parameterBlock[DOS_INT21_EXEC_PB_TAIL] | (parameterBlock[DOS_INT21_EXEC_PB_TAIL + 1] << BYTE_SHIFT));
            machine->ExecTailSegment = (WORD)(parameterBlock[DOS_INT21_EXEC_PB_TAIL + 2] | (parameterBlock[DOS_INT21_EXEC_PB_TAIL + 3] << BYTE_SHIFT));
            machine->ExecFcb1Offset = (WORD)(parameterBlock[DOS_INT21_EXEC_PB_FCB1] | (parameterBlock[DOS_INT21_EXEC_PB_FCB1 + 1] << BYTE_SHIFT));
            machine->ExecFcb1Segment = (WORD)(parameterBlock[DOS_INT21_EXEC_PB_FCB1 + 2] | (parameterBlock[DOS_INT21_EXEC_PB_FCB1 + 3] << BYTE_SHIFT));
            machine->ExecFcb2Offset = (WORD)(parameterBlock[DOS_INT21_EXEC_PB_FCB2] | (parameterBlock[DOS_INT21_EXEC_PB_FCB2 + 1] << BYTE_SHIFT));
            machine->ExecFcb2Segment = (WORD)(parameterBlock[DOS_INT21_EXEC_PB_FCB2 + 2] | (parameterBlock[DOS_INT21_EXEC_PB_FCB2 + 3] << BYTE_SHIFT));
            machine->ExecMode = subfunction;
            machine->IsExecPending = 1;         /* the host does the rest */
            OKCF();
        }
        else if (subfunction == DOS_INT21_EXEC_OVERLAY)
        {
            /* THE OVERLAY. Two words of parameter block and nothing else:
             * where to put it, and what to relocate by -- and those are NOT
             * the same number (oracle: relocation uses the FACTOR, measured
             * with a factor deliberately unequal to the load segment). No PSP,
             * no allocation, no transfer of control, so the host's normal EXEC
             * path is wrong for it and it branches early. (GH #50)
             */
            const volatile BYTE *parameterBlock =
                (const volatile BYTE *)((R_ES << PARAGRAPH_SHIFT) + (R_BX & WORD_MASK));
            DosGuestPath(machine, R_DS, R_DX, machine->ExecPath, sizeof(machine->ExecPath));
            machine->ExecOverlaySegment   = (WORD)(parameterBlock[0] | (parameterBlock[1] << BYTE_SHIFT));
            machine->ExecOverlayRelocation = (WORD)(parameterBlock[DOS_INT21_EXEC_PB_TAIL] | (parameterBlock[DOS_INT21_EXEC_PB_TAIL + 1] << BYTE_SHIFT));
            machine->ExecMode = DOS_INT21_EXEC_OVERLAY;
            machine->IsExecPending = 1;
            OKCF();
        }
        else if (subfunction == DOS_INT21_EXEC_SET_STATE)
        {
            /* SET EXECUTION STATE (#165). The second half of a loader's own EXEC:
             * AX=4B01h loaded the program, the loader did its own work, and this
             * tells DOS control is about to go to it. Measured (p_4b05, 6.22 and
             * PCem agree): AX=0000 CF=0, and the CURRENT PSP IS NOT CHANGED -- 4B01h
             * already switched it. DOS uses the block for SETVER's per-program
             * version; we keep no SETVER table, so there is nothing else to do.
             * DOSBox-X refuses it (CF=1 AX=000B): an emulator without the call,
             * not a different DOS.
             */
            trace = LogPut(trace, "  INT21 AX=4B05 set execution state -- accepted\r\n");
            SETAX(0);
            OKCF();
        }
        else
        {
            /* AL=02/04 are not DOS 6.22 functions we have measured. */
            trace = LogPut(trace, "  INT21 AH=4B AL=0x"); trace = LogHexByte(trace, subfunction);
            trace = LogPut(trace, " UNIMPLEMENTED (overlay load)\r\n");
            machine->Unimplemented[DOS_FN_EXEC >> BITMAP_BYTE_SHIFT] |= (BYTE)(1u << (DOS_FN_EXEC & BITMAP_BIT_MASK));
            SETAX(DOS_ERR_INVALID_FUNCTION);
            ERRCF();
        }
    }
    else if (function == DOS_FN_GET_DEFAULT_DRIVE_INFO || function == DOS_FN_GET_DRIVE_INFO)    /* allocation info for a drive */
    {
        /* AL=sectors/cluster, DS:BX -> media descriptor byte, CX=bytes/sector,
         * DX=total clusters.  All disk geometry, so the probe compares only CF.
         * NOTE this call RETURNS A SEGMENT IN DS -- which is what broke the
         * probe's own output until probe_capture learned to restore it.
         */
        DWORD sectorsPerCluster = 0;
        DWORD bytesPerSector = 0;
        DWORD freeClusters = 0;
        DWORD totalClusters = 0;
        CHAR root[DOS_INT21_ROOT_PATH_SIZE];
        PSTR rootPointer = 0;
        BYTE driveNumber = (function == DOS_FN_GET_DRIVE_INFO) ? (BYTE)(R_DX & BYTE_MASK) : 0;  /* 1Bh: default drive */

        if (!driveNumber && machine->VirtualDrive >= 0)
            driveNumber = (BYTE)(machine->VirtualDrive + 1);

        if (driveNumber) { root[0] = (CHAR)('A' + driveNumber - 1);
        root[1] = ':';
                    root[2] = '\\';
                    root[3] = 0;
                    rootPointer = root; }

        if (GetDiskFreeSpaceA(rootPointer, &sectorsPerCluster, &bytesPerSector, &freeClusters, &totalClusters))
        {
            volatile BYTE *media = (volatile BYTE *)((DOS_CTAB_SEG << PARAGRAPH_SHIFT) + DOS_MEDIA_OFF);
            *media = DOS_INT21_MEDIA_FIXED;                       /* fixed disk */
            SETAX((R_AX & HIGH_BYTE_MASK) | (sectorsPerCluster & BYTE_MASK));
            SET16(R_DS, DOS_CTAB_SEG);
            SET16(R_BX, DOS_MEDIA_OFF);
            SET16(R_CX, (WORD)bytesPerSector);
            SET16(R_DX, totalClusters > WORD_MASK ? WORD_MASK : totalClusters);
            OKCF();
        }
        else
        {
            SETAX((R_AX & HIGH_BYTE_MASK) | DOS_INT21_INVALID_DRIVE_AL);
            ERRCF();
        }
    }
    else if (function == DOS_FN_GET_DEFAULT_DPB || function == DOS_FN_GET_DPB)    /* get drive parameter block */
    {
        /* DPB contents are disk geometry and its address is host-specific; AL is
         * the comparable part -- 00 for a valid drive, FF otherwise (measured).
         */
        BYTE driveNumber = (function == DOS_FN_GET_DEFAULT_DPB) ? 0 : (BYTE)(R_DX & BYTE_MASK);
        DWORD sectorsPerCluster = 0;
        DWORD bytesPerSector = 0;
        DWORD freeClusters = 0;
        DWORD totalClusters = 0;
        CHAR root[DOS_INT21_ROOT_PATH_SIZE];
        PSTR rootPointer = 0;

        if (driveNumber) { root[0] = (CHAR)('A' + driveNumber - 1);
        root[1] = ':';
                    root[2] = '\\';
                    root[3] = 0;
                    rootPointer = root; }

        if (driveNumber > DOS_DRIVE_LETTERS || !GetDiskFreeSpaceA(rootPointer, &sectorsPerCluster, &bytesPerSector, &freeClusters, &totalClusters))
        {
            SETAX((R_AX & HIGH_BYTE_MASK) | DOS_INT21_INVALID_DRIVE_AL);
        }
        else
        {
            /* #48: THE SAME BUILDER AS THE AH=52h CHAIN, so the two DPBs a program
             * can reach for one drive describe one volume. This copy had its own
             * inline fields: FAT sectors, root start and data start ZERO (a volume
             * whose files overlap its FAT), a highest-cluster word that wrapped past
             * 0xFFFF where the chain's clamps at 0xFFFE, and unit 0 where the chain
             * says unit = drive (6.22: one IO.SYS driver, A: unit 0 .. C: unit 2). DosDpbFatLayout's
             * note in dos_sysvars.h says what the derived fields are and are not.
             *
             * [CAUTION]: Still a separate copy at DOS_DPB_OFF rather than a pointer INTO the
             * chain (6.22 returns the chain's own DPB); that is a pointer change for a
             * later pass, and the probe compares only AL here.
             */
            volatile BYTE *dpbBytes = (volatile BYTE *)((DOS_CTAB_SEG << PARAGRAPH_SHIFT) + DOS_DPB_OFF);
            BYTE dpb[DOS_DPB_LEN];
            UINT drive = driveNumber ? (UINT)(driveNumber - 1) : (UINT)DOS_INT21_DRIVE_C;  /* 0-based drive */
            CHAR rootText[DOS_INT21_ROOT_PATH_SIZE];
            INT index;
            INT remaining;
            rootText[0] = (CHAR)('A' + drive);
            rootText[1] = ':';
            rootText[2] = '\\';
            rootText[3] = 0;
            remaining = (GetDriveTypeA(rootText) == DRIVE_REMOVABLE);  /* 6.22's floppy: 224, F0h */
            DosDpbBuild(dpb, drive, bytesPerSector ? bytesPerSector : DOS_INT21_DEFAULT_SECTOR_SIZE, sectorsPerCluster ? sectorsPerCluster : 1, remaining ? DOS_INT21_FLOPPY_ROOT_ENTRIES : DOS_INT21_FIXED_ROOT_ENTRIES,
                          (totalClusters > DOS_INT21_MAX_CLUSTER) ? DOS_INT21_MAX_CLUSTER : totalClusters + 1, remaining ? DOS_INT21_MEDIA_FLOPPY : DOS_INT21_MEDIA_FIXED,
                          DOS_DEV_SEG, DOS_DEVICE_OFFSET(DOS_DEVICE_BLOCK), DOS_INT21_DPB_LAST, DOS_INT21_DPB_LAST);

            for (index = 0; index < DOS_DPB_LEN; ++index)
                dpbBytes[index] = dpb[index];

            SET16(R_DS, DOS_CTAB_SEG);
            SET16(R_BX, DOS_DPB_OFF);
            SETAX(R_AX & HIGH_BYTE_MASK);
        }

        OKCF();
    }
    else if (function == DOS_FN_SWITCH_CHAR)                /* get/set the SWITCH character */
    {
        BYTE subfunction = (BYTE)(R_AX & BYTE_MASK);

        if (subfunction == DOS_INT21_GET)                /* oracle: DL = '/' */
        {
            SET16(R_DX, (WORD)((R_DX & HIGH_BYTE_MASK) | machine->SwitchChar));
            SETAX(R_AX & HIGH_BYTE_MASK);
            OKCF();
        }
        else if (subfunction == DOS_INT21_SET)
        {
            machine->SwitchChar = (BYTE)(R_DX & BYTE_MASK);
            SETAX(R_AX & HIGH_BYTE_MASK);
            OKCF();
        }
        else
        {
            SETAX((R_AX & HIGH_BYTE_MASK) | DOS_INT21_INVALID_DRIVE_AL);
            OKCF();
        }
    }
    else if (function == DOS_FN_CODE_PAGE)                /* get/set global code page */
    {
        BYTE subfunction = (BYTE)(R_AX & BYTE_MASK);

        if (subfunction == DOS_INT21_CODE_PAGE_GET)                /* oracle: BX=DX=437 */
        {
            SET16(R_BX, DOS_INT21_CODE_PAGE);
            SET16(R_DX, DOS_INT21_CODE_PAGE);
            OKCF();
        }
        else if (subfunction == DOS_INT21_CODE_PAGE_SET)
        {
            OKCF();                             /* accept; we have only 437 */
        }
        else
        {
            SETAX(DOS_ERR_INVALID_FUNCTION);
            ERRCF();
        }
    }
    else if (function == DOS_FN_CREATE_PSP || function == DOS_FN_CREATE_CHILD_PSP)    /* create a PSP / child PSP */
    {
        /* Copy our PSP to the segment in DX and fix up the fields that must
         * differ. 55h additionally takes the child's memory top in SI.
         */
        volatile BYTE *source = (volatile BYTE *)(DOS_PSP_SEG << PARAGRAPH_SHIFT);
        volatile BYTE *destination = (volatile BYTE *)((R_DX & WORD_MASK) << PARAGRAPH_SHIFT);
        INT index;

        for (index = 0; index < DOS_PSP_SIZE; ++index)
            destination[index] = source[index];

        destination[DOS_PSP_PARENT] = (BYTE)(DOS_PSP_SEG & BYTE_MASK);  /* parent PSP segment */
        destination[DOS_PSP_PARENT + 1] = (BYTE)(DOS_PSP_SEG >> BYTE_SHIFT);

        if (function == DOS_FN_CREATE_CHILD_PSP)
        {
            destination[DOS_PSP_MEMORY_TOP] = (BYTE)(R_SI & BYTE_MASK);  /* memory top */
            destination[DOS_PSP_MEMORY_TOP + 1] = (BYTE)((R_SI >> BYTE_SHIFT) & BYTE_MASK);
        }

        OKCF();
    }
    else if (function == DOS_FN_KEEP_PROCESS)                /* terminate and stay resident */
    {
        /* STAY RESIDENT. (GH #49):
         * DX is the paragraph count to KEEP, counted from the PSP. Residency
         * is three things, and the host does all three on this flag:
         *   the memory block is RESIZED, not freed;
         *   the interrupt vectors it installed are NOT unwound; and
         *   control returns to whatever EXEC'd it, with the image intact.
         *
         * [CAUTION]: AT DEPTH 0 THERE IS NOTHING TO RETURN TO. A top-level program that
         * TSRs has no parent inside this VDM, so the run ends either way --
         * but say which, because "resident" and "exited" look identical in a
         * log and a TSR that believes it installed and did not is exactly the
         * silent failure #27 exists to remove.
         */
        machine->TsrKeep = (WORD)(R_DX & WORD_MASK);
        machine->IsTsrPending = 1;
        trace = LogPut(trace, "  INT21 AH=31 TSR: keep 0x"); trace = LogHex(trace, machine->TsrKeep);
        trace = LogPut(trace, " paragraphs, vectors LEFT INSTALLED\r\n");
        machine->ExitCode = (INT)(R_AX & BYTE_MASK);
        shouldContinue = 0;
    }
    else if (function == DOS_FN_BPB_TO_DPB)                /* translate a BPB into a DPB */
    {
        /* [CAUTION]: TESTED AS THE CAUSE OF THE COMMAND.COM EXIT, AND REFUTED. XP's own
         * COMMAND.COM calls this during init and terminates shortly after,
         * printing nothing, and this was the ONLY unimplemented call in the whole
         * run -- which made it the tempting answer rather than the proven one.
         * Answering SUCCESS with a zeroed DPB was tried: the shell still exits, at
         * the SAME CS:IP (0x95eb:0x03ce), after the same 32 ms. So 53h is not what
         * stops it, and reporting success here would have bought nothing at the
         * price of a call that lies. Reverted deliberately.
         * What COMMAND.COM asks for and does not get is INT 2Fh AX=122Eh, the five
         * DOS error-message table addresses (DL=00/02/04/06/08) -- see the widened
         * BOP2F log. That is the next thing to chase, and "died after" is still not
         * "died because": prove it before implementing it.
         */
        /* [WARNING]: ONE LINE PER CALL, AND A LOOPING GUEST TURNS THAT INTO A FILE. XP's
         * COMMAND.COM re-runs its init forever while `sub 01` answers "no command",
         * and this note alone was 2,713 lines in the last 200 KB of a 42 MB log.
         * Say it once; `unimpl21[]` already carries the fact for the summary.
         */
        { static INT isSaid = 0;

          if (!isSaid) { isSaid = 1;
              trace = LogPut(trace, "  INT21 AH=53 BPB->DPB UNIMPLEMENTED (no installable "
                            "block drivers) -- said once per run\r\n"); } }

        machine->Unimplemented[DOS_FN_BPB_TO_DPB >> BITMAP_BYTE_SHIFT] |= (BYTE)(1u << (DOS_FN_BPB_TO_DPB & BITMAP_BIT_MASK));
        /* MEASURED AGAINST STOCK NTVDM, 2026-09-25 (Importance = 3):
         * Documented AH=53h is BPB->DPB and takes DS:SI / ES:BP with NO AL
         * sub-function. XP's COMMAND.COM uses it as a PRIVATE QUERY with AL as a
         * selector and reads the answer out of AL -- AL=5 and AL=7 at start-up,
         * AL=2 elsewhere (the INT 21h trace). NTDOS.SYS is the only implementation,
         * so stock ntvdm on the rig is the only oracle --
         * `debug\rig\dosstock.bat P_INT53.COM`, which
         * drops the IFEO key and PROVES it back. Real MS-DOS cannot be asked: the
         * documented form BUILDS a DPB from a caller-supplied BPB, and a fabricated
         * pointer HANGS 6.22 (measured twice).
         *
         *   AL=00 -> AX=0005 CF=0     AL=04 -> AX=5300 CF=0
         *   AL=01 -> AX=0001 CF=1     AL=05 -> AX=5301 CF=0
         *   AL=02 -> AX=5300 CF=0     AL=06 -> AX=5300 CF=0
         *   AL=03 -> AX=0001 CF=1     AL=07 -> AX=5301 CF=0
         *
         * AH is preserved and AL carries a 0/1 answer for 02/04/05/06/07; 01 and
         * 03 are genuinely unsupported (AX=1, CF=1 -- DOS's "invalid function").
         *
         * [WARNING]: AND THE ORIGINAL "UNIMPLEMENTED" WAS ACCIDENTALLY RIGHT. It returned AX=1,
         * i.e. AL=1 -- exactly what stock returns for AL=5 and AL=7, the two COMMAND.COM keeps. I
         * then "fixed" it to AX=0 on the theory that AL=5 answering 1 was blocking the interactive
         * path, and made it WRONG -- against the only measurement there is. Reverted, and marked
         * provisional, which is the only reason that was a correction rather than a fact.
         * **Guessing a value for a private call is not cheaper than measuring it; it is the same
         * work twice.**
         *
         * [INFO]: AND THE THEORY IT WAS REVERTED WITH IS NOW DISPROVED (s79). The revert carried a
         * second claim -- "stock answers AL=5 with 1 and IS interactive, so that answer does not
         * gate the prompt" -- which was an INFERENCE from the standalone probe, not an observation.
         * In fact XP's COMMAND.COM reads its command line from the keyboard only while its AL=5
         * answer is 0, and nothing else it does changes that. Stock IS interactive. Therefore, IN
         * THE SHELL'S CONTEXT, stock's AX=5305h returns AL=0 -- and our probe measured AL=1.
         *
         * **AL=5 IS CONTEXT-DEPENDENT, and the 8/8 "agreement" is an agreement about
         * the context we measured in.** The prime suspect is the measurement rig
         * itself: probe.inc reports through INT 21h AH=02 and every stock run is
         * captured with `> file`, so the oracle was asked "are you interactive?"
         * with its own output redirected. `tests/probes/dos/p_int53f.asm` asks the same
         * eight questions but writes its answers through AH=3Ch/40h, so it can be run
         * with NOTHING redirected.
         *
         * [CAUTION]: AL=00's row was measured with SI=0 and BP=0, as COMMAND.COM issues it. It
         * is NOT a claim about the documented BPB->DPB call given a real BPB, which we
         * still do not implement.
         */
        { BYTE subfunction = (BYTE)(R_AX & BYTE_MASK);

          if (subfunction < DOS_INT53_COUNT)
          {
              SETAX(g_DosInt53Answers[subfunction].Ax);

              if (g_DosInt53Answers[subfunction].IsCarry)
                  ERRCF();
              else
                  OKCF();
          }
          else
          {
              SETAX(DOS_ERR_INVALID_FUNCTION);
              ERRCF();
          }
        }
    }
    else if (function == DOS_FN_NETWORK_MACHINE)                /* network machine name / printer */
    {
        BYTE subfunction = (BYTE)(R_AX & BYTE_MASK);

        if (subfunction == DOS_INT21_GET)                /* oracle: AX=0, CF=0 */
        {
            volatile BYTE *buffer = (volatile BYTE *)((R_DS << PARAGRAPH_SHIFT) + (R_DX & WORD_MASK));
            INT index;

            for (index = 0; index < DOS_INT21_MACHINE_NAME_SIZE; ++index)
                buffer[index] = 0;

            SETAX(0);
            SET16(R_CX, 0);
            OKCF();
        }
        else
        {
            SETAX(DOS_ERR_INVALID_FUNCTION);
            ERRCF();
        }
    }
    else if (function == DOS_FN_REDIRECTION)                /* network redirection list */
    {
        trace = LogPut(trace, "  INT21 AH=5F network redirection: no redirector present\r\n");
        SETAX(DOS_ERR_INVALID_FUNCTION);
        ERRCF();                      /* invalid function */
    }
    else if (function == DOS_FN_SET_LOOKAHEAD)                /* set device driver lookahead */
    {
        OKCF();                                 /* internal; accepted, no effect */
    }
    else if (function == DOS_FN_AUX_INPUT || function == DOS_FN_AUX_OUTPUT || function == DOS_FN_PRINTER_OUTPUT)    /* AUX in / AUX out / PRN out */
    {
        /* #251: THESE WENT NOWHERE -- "accepted and discarded", and AUX input
         * answered ^Z -- while COM1 and an LPT1 spool both exist. On DOS they are
         * the AUX and PRN drivers, which call INT 14h / INT 17h through the IVT
         * (p_auxprn logs the exact sequence 6.22 makes), so in V86 the guest is
         * resumed in that driver code. In PM the bytes go to the same devices
         * directly, and AUX input stays ^Z (no wait loop to run it in).
         */
        if (AUXPRN_V86)
            AUXPRN_TRAMP(function == DOS_FN_PRINTER_OUTPUT ? DOS_AUXPRN_PRN_OUTPUT : function == DOS_FN_AUX_OUTPUT ? DOS_AUXPRN_AUX_OUTPUT : DOS_AUXPRN_AUX_INPUT);
        else if (function == DOS_FN_AUX_INPUT)
        {
            SETAX((R_AX & HIGH_BYTE_MASK) | ASCII_END_OF_FILE);
            OKCF();
        }
        else
        {
            BYTE character = (BYTE)(R_DX & BYTE_MASK);

            if (function == DOS_FN_PRINTER_OUTPUT)
            {
                if (machine->PrinterOut)
                    (VOID)machine->PrinterOut(machine->DeviceContext, character);
            }
            else if (machine->AuxOut)
                machine->AuxOut(machine->DeviceContext, character);

            SETAX((R_AX & HIGH_BYTE_MASK) | character);  /* oracle: AL = the byte sent */
            OKCF();
        }
    }
    else if (function == DOS_FN_FLUSH_AND_INPUT)                /* flush input, then run AL */
    {
        /* AL names the input function to perform after flushing. Anything else
         * is just a flush. Re-dispatching is the whole point of the call.
         */
        BYTE inputFunction = (BYTE)(R_AX & BYTE_MASK);

        while (machine->ConsolePeek && machine->ConsolePeek(machine->ConsoleInContext) && machine->ConsoleInNoWait)
            (VOID)machine->ConsoleInNoWait(machine->ConsoleInContext);

        if (inputFunction == DOS_FN_CHAR_INPUT_ECHO || inputFunction == DOS_FN_DIRECT_CONSOLE_IO || inputFunction == DOS_FN_DIRECT_INPUT || inputFunction == DOS_FN_CHAR_INPUT || inputFunction == DOS_FN_BUFFERED_INPUT)
        {
            SETAX((WORD)(inputFunction << BYTE_SHIFT));
            machine->IsRetry = 1;               /* re-enter with AH = that fn */
        }
        else
            OKCF();
    }
    else if (function == DOS_FN_SET_VERIFY)                /* set verify flag */
    {
        machine->IsVerifyOn = (BYTE)(R_AX & BYTE_MASK);
        OKCF();
    }
    else if (function == DOS_FN_GET_VERIFY)                /* get verify flag */
    {
        SETAX((R_AX & HIGH_BYTE_MASK) | machine->IsVerifyOn);
        OKCF();
    }
    else if (function == DOS_FN_GET_INDOS_FLAG)                /* get InDOS flag -> ES:BX */
    {
        SET16(R_ES, DOS_SDA_SEG);
        SET16(R_BX, DOS_INDOS_OFF);
        OKCF();
    }
    else if (function == DOS_FN_SERVER && ((R_AX & BYTE_MASK) == DOS_INT21_SERVER_PRINTER_MODE || (R_AX & BYTE_MASK) == DOS_INT21_SERVER_PRINTER_FLUSH))
    {
        /* 5D08h/5D09h set and flush the network redirector's sharing retry
         * counts. COMMAND.COM calls both at startup. There is no redirector
         * here, so accept and ignore -- that is what DOS does on a machine with
         * no network, and refusing would make the shell think something failed.
         */
        OKCF();
    }
    else if (function == DOS_FN_SERVER && (R_AX & BYTE_MASK) == DOS_INT21_SERVER_SWAPPABLE_AREA)    /* get swappable data area */
    {
        SET16(R_DS, DOS_SDA_SEG);
        SET16(R_SI, DOS_SDA_OFF);
        SET16(R_CX, DOS_SDA_LEN);
        SET16(R_DX, DOS_SDA_LEN);
        trace = LogPut(trace, "  INT21 AH=5D06 SDA (minimal: crit-err + InDOS only)\r\n");
        OKCF();
    }
    else if (function == DOS_FN_MKDIR || function == DOS_FN_RMDIR)    /* mkdir / rmdir */
    {
        CHAR fileName[DOS_INT21_PATH_SIZE];
        INT isOk;
        DosGuestPath(machine, R_DS, R_DX, fileName, sizeof(fileName));
        isOk = (function == DOS_FN_MKDIR) ? (INT)CreateDirectoryA(fileName, NULL)
                           : (INT)RemoveDirectoryA(fileName);
        /* s91: A SEARCH THE GUEST NEVER FINISHED KEEPS THE DIRECTORY OPEN. DOS has no
         * FindClose, so an AH=4Eh/11h search that stopped before "no more files"
         * leaves our FindFirstFile handle alive -- and Windows will not remove a
         * directory with a search open in it. 6.22's COMMAND.COM searches inside
         * a directory on `cd`, and `rmdir` of that (empty) directory then failed
         * with "Invalid path, not directory, or directory not empty" (runs/s91,
         * chain11b). Close the guest's unfinished searches and try once more.
         */
        if (!isOk && function == DOS_FN_RMDIR)
        {
            INT findSlot;

            for (findSlot = 0; findSlot < DOS_FIND_SLOTS; ++findSlot)
                if (machine->FindHandles[findSlot])
                {
                    FindClose(machine->FindHandles[findSlot]);
                    machine->FindHandles[findSlot] = 0;
                }

            if (machine->FcbFind)
            {
                FindClose(machine->FcbFind);
                machine->FcbFind = 0;
            }

            isOk = (INT)RemoveDirectoryA(fileName);
        }

        if (isOk)
            OKCF();
        else
        {
            /* Oracle: mkdir over an existing name is 5 (access denied); rmdir of
             * something absent is 3 (path not found).
             */
            DWORD error = GetLastError();
            SETAX((WORD)(error == ERROR_ALREADY_EXISTS ? DOS_ERR_ACCESS_DENIED
                           : error == ERROR_PATH_NOT_FOUND ? DOS_ERR_PATH_NOT_FOUND
                           : error == ERROR_FILE_NOT_FOUND ? DOS_ERR_PATH_NOT_FOUND : DOS_ERR_ACCESS_DENIED));
            ERRCF();
        }
    }
    else if (function == DOS_FN_DELETE)                /* delete file */
    {
        CHAR fileName[DOS_INT21_PATH_SIZE];
        DosGuestPath(machine, R_DS, R_DX, fileName, sizeof(fileName));

        if (DeleteFileA(fileName))
            OKCF();
        else /* oracle: absent -> AX=2 */
        {
            SETAX(DOS_ERR_FILE_NOT_FOUND);
            ERRCF();
        }
    }
    else if (function == DOS_FN_FILE_ATTRIBUTES)                /* get/set file attributes */
    {
        CHAR fileName[DOS_INT21_PATH_SIZE];
        BYTE subfunction = (BYTE)(R_AX & BYTE_MASK);
        DosGuestPath(machine, R_DS, R_DX, fileName, sizeof(fileName));

        if (subfunction == DOS_INT21_GET)
        {
            DWORD attributes = GetFileAttributesA(fileName);

            if (attributes == INVALID_FILE_ATTRIBUTES)
            {
                SETAX(DOS_ERR_FILE_NOT_FOUND);
                ERRCF();
            }
            else
            {
                SET16(R_CX, (WORD)(attributes & DOS_LFN_ATTRIBUTE_MASK));
                SETAX((WORD)(attributes & DOS_LFN_ATTRIBUTE_MASK));
                OKCF();
            }
        }
        else if (subfunction == DOS_INT21_SET)
        {
            DWORD attributes = (DWORD)(R_CX & DOS_LFN_ATTRIBUTE_MASK);

            if (!attributes)
                attributes = FILE_ATTRIBUTE_NORMAL;

            if (SetFileAttributesA(fileName, attributes))
                OKCF();
            else
            {
                SETAX(DOS_ERR_FILE_NOT_FOUND);
                ERRCF();
            }
        }
        else
        {
            trace = LogPut(trace, "  INT21 AH=43 AL=0x"); trace = LogHexByte(trace, subfunction);
            /* Not a gap: 6.22 and PCem answer AX=1 CF=1 too (p_subfn int21.4302). */
            trace = LogPut(trace, " not a 6.22 subfunction -> AX=1 CF=1 (matches DOS)\r\n");
            SETAX(DOS_ERR_INVALID_FUNCTION);
            ERRCF();
        }
    }
    else if (function == DOS_FN_DUP || function == DOS_FN_DUP2)    /* dup / dup2 */
    {
        /* A DEVICE IS DUPLICABLE, AND THAT IS THE WHOLE POINT OF 45h (Importance = 1):
         * This refused anything that was not a FILE, so `dup(1)` -- the first
         * step of every save-redirect-restore sequence a shell performs --
         * came back error 6 and the restore could never happen. Found by
         * running tests/probes/dos/p_redir.asm on the rig against the same probe
         * on the oracle; the two disagreed on one line:
         *   oracle : CASE=int21.45.dup.stdout AX=0005 CF=0
         *   NTVDMEX: CASE=int21.45.dup.stdout AX=0006 CF=1
         * The failure was not even visible as itself: the probe's LATER output
         * vanished into the test file, because with stdout never restored the
         * next open took handle 1. A wrong answer that eats the evidence of
         * itself is why this is measured against an oracle rather than read.
         * Duplicating a device produces another handle ON THAT DEVICE -- no
         * Win32 handle exists to duplicate, so the copy is a device slot too.
         */
        DWORD sourceHandle = R_BX & WORD_MASK;
        DWORD targetHandle;
        INT isSourceDevice = DosHandleIsDevice((PVOID const *)machine->FileHandles, machine->StdOpen, sourceHandle);

        if (!isSourceDevice && !DosHandleIsFile((PVOID const *)machine->FileHandles, sourceHandle))
        {
            SETAX(DOS_ERR_INVALID_HANDLE);
            ERRCF();
        }
        else
        {
            HANDLE newHandle = 0;

            if (!isSourceDevice
                && !DuplicateHandle(GetCurrentProcess(), machine->FileHandles[sourceHandle],
                                    GetCurrentProcess(), &newHandle, 0, FALSE,
                                    DUPLICATE_SAME_ACCESS))
            {
                SETAX(DOS_ERR_INVALID_HANDLE);
                ERRCF();
            }
            else if (function == DOS_FN_DUP)
            {
                targetHandle = DosHandleAllocate((PVOID const *)machine->FileHandles, machine->StdOpen);

                if (targetHandle >= DOS_MAX_FILES)
                {
                    if (newHandle)
                        CloseHandle(newHandle);

                    SETAX(DOS_ERR_TOO_MANY_OPEN_FILES);
                    ERRCF();
                }
                else if (isSourceDevice && !DosHandleSetDevice(&machine->StdOpen, targetHandle, TRUE))
                {
                    /* Past the device mask. Refuse LOUDLY rather than hand back a
                     * slot that would read as a file -- see DOS_DEV_SLOTS.
                     */
                    trace = LogPut(trace, "  INT21 AH=45 device dup past slot 0x");
                    trace = LogHex(trace, DOS_DEV_SLOTS); trace = LogPut(trace, " -- refused\r\n");
                    SETAX(DOS_ERR_TOO_MANY_OPEN_FILES);
                    ERRCF();
                }
                else
                {
                    machine->FileHandles[targetHandle] = newHandle;
                    SETAX(targetHandle);
                    OKCF();
                }
            }
            else
            {
                targetHandle = R_CX & WORD_MASK;

                if (targetHandle >= DOS_MAX_FILES)
                {
                    if (newHandle)
                        CloseHandle(newHandle);

                    SETAX(DOS_ERR_INVALID_HANDLE);
                    ERRCF();
                }
                else if (isSourceDevice && !DosHandleSetDevice(&machine->StdOpen, targetHandle, TRUE))
                {
                    trace = LogPut(trace, "  INT21 AH=46 device dup2 past slot 0x");
                    trace = LogHex(trace, DOS_DEV_SLOTS); trace = LogPut(trace, " -- refused\r\n");
                    SETAX(DOS_ERR_TOO_MANY_OPEN_FILES);
                    ERRCF();
                }
                else { DosHandleRelease(machine, targetHandle);
                       machine->FileHandles[targetHandle] = newHandle;  /* 0 when src is a device */

                       if (!isSourceDevice)
                           DosHandleSetDevice(&machine->StdOpen, targetHandle, FALSE);

                       OKCF(); }
            }
        }
    }
    else if (function == DOS_FN_GET_RETURN_CODE)                /* get child return code */
    {
        SETAX(machine->ChildReturnCode);
        machine->ChildReturnCode = 0;  /* DOS clears it after reading */
        OKCF();
    }
    else if (function == DOS_FN_RENAME)                /* rename: DS:DX -> ES:DI */
    {
        CHAR sourcePath[DOS_INT21_PATH_SIZE];
        CHAR destinationPath[DOS_INT21_PATH_SIZE];
        DosGuestPath(machine, R_DS, R_DX, sourcePath, sizeof(sourcePath));
        DosGuestPath(machine, R_ES, R_DI, destinationPath,   sizeof(destinationPath));

        if (MoveFileA(sourcePath, destinationPath))
            OKCF();
        else { DWORD error = GetLastError();
               SETAX((WORD)(error == ERROR_ALREADY_EXISTS ? DOS_ERR_ACCESS_DENIED : DOS_ERR_FILE_NOT_FOUND));
               ERRCF(); }
    }
    else if (function == DOS_FN_FILE_DATE_TIME)                /* get/set file date and time */
    {
        DWORD handle = R_BX & WORD_MASK;
        BYTE subfunction = (BYTE)(R_AX & BYTE_MASK);

        if (!DosHandleIsFile((PVOID const *)machine->FileHandles, handle))
        {
            SETAX(DOS_ERR_INVALID_HANDLE);
            ERRCF();
        }
        else if (subfunction == DOS_INT21_GET)
        {
            FILETIME fileTime;
            FILETIME localTime;
            WORD dosDate = 0;
            WORD dosTime = 0;

            if (GetFileTime(machine->FileHandles[handle], NULL, NULL, &fileTime)
                && FileTimeToLocalFileTime(&fileTime, &localTime)
                && FileTimeToDosDateTime(&localTime, &dosDate, &dosTime))
            {
                SET16(R_CX, dosTime);
                SET16(R_DX, dosDate);
                OKCF();
            }
            else
            {
                SETAX(DOS_ERR_INVALID_HANDLE);
                ERRCF();
            }
        }
        else if (subfunction == DOS_INT21_SET)
        {
            FILETIME fileTime;
            FILETIME localTime;

            if (DosDateTimeToFileTime((WORD)(R_DX & WORD_MASK), (WORD)(R_CX & WORD_MASK), &localTime)
                && LocalFileTimeToFileTime(&localTime, &fileTime)
                && SetFileTime(machine->FileHandles[handle], NULL, NULL, &fileTime))
                OKCF();
            else
            {
                SETAX(DOS_ERR_INVALID_HANDLE);
                ERRCF();
            }
        }
        else
        {
            SETAX(DOS_ERR_INVALID_FUNCTION);
            ERRCF();
        }
    }
    else if (function == DOS_FN_CREATE_TEMP || function == DOS_FN_CREATE_NEW)    /* create temp / create new */
    {
        CHAR fileName[DOS_INT21_PATH_SIZE];
        HANDLE file;
        DWORD slot;
        DosGuestPath(machine, R_DS, R_DX, fileName, sizeof(fileName));

        if (function == DOS_FN_CREATE_TEMP) {                 /* DS:DX is a DIRECTORY path;
                                                   DOS appends a generated name
                                                   and hands it back in place. */
            INT index = 0;
            static UINT sequence = 0;
            PCSTR hexDigits = HEX_DIGITS_UPPER;

            while (fileName[index] && index < DOS_INT21_TEMP_DIRECTORY_MAX)
                ++index;

            if (index && fileName[index-1] != '\\' && fileName[index-1] != '/')
                fileName[index++] = '\\';

            { UINT seed = (UINT)(GetTickCount() + (sequence++ * DOS_INT21_TEMP_SEED_STEP));
              INT index2;

              for (index2 = 0; index2 < DOS_INT21_TEMP_NAME_DIGITS; ++index2)
                  fileName[index + index2] = hexDigits[(seed >> (DOS_INT21_TEMP_TOP_SHIFT - index2*DOS_INT21_HEX_DIGIT_BITS)) & NIBBLE_MASK]; }

            fileName[index + DOS_INT21_TEMP_NAME_DIGITS] = 0;
            { volatile BYTE *buffer = (volatile BYTE *)((R_DS << PARAGRAPH_SHIFT) + (R_DX & WORD_MASK));
              INT index2 = 0;

              while (fileName[index2])
              {
                  buffer[index2] = (BYTE)fileName[index2];
                  ++index2;
              }

              buffer[index2] = 0; }
        }

        file = CreateFileA(fileName, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL,
                        CREATE_NEW, (DWORD)(R_CX & DOS_LFN_ATTRIBUTE_MASK) ? (DWORD)(R_CX & DOS_LFN_ATTRIBUTE_MASK)
                                                         : FILE_ATTRIBUTE_NORMAL, NULL);

        if (file == INVALID_HANDLE_VALUE)
        {
            /* Oracle: create-new over an existing file is error 80 (file exists),
             * not 5 -- measured, and not the obvious guess.
             */
            DWORD error = GetLastError();
            SETAX((WORD)(error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS ? DOS_ERR_FILE_EXISTS : DOS_ERR_PATH_NOT_FOUND));
            ERRCF();
        }
        else
        {
            slot = DosHandleAllocate((PVOID const *)machine->FileHandles, machine->StdOpen);

            if (slot < DOS_MAX_FILES) { machine->FileHandles[slot] = file;
            SETAX(slot);
            OKCF();
                                        DosStampVdmNow(file); /* #263 */ }
            else
            {
                CloseHandle(file);
                SETAX(DOS_ERR_TOO_MANY_OPEN_FILES);
                ERRCF();
            }
        }
    }
    else if (function == DOS_FN_LOCK)                /* lock / unlock a byte range */
    {
        DWORD handle = R_BX & WORD_MASK;
        DWORD offset = ((DWORD)(R_CX & WORD_MASK) << WORD_SHIFT) | (DWORD)(R_DX & WORD_MASK);
        DWORD length = ((DWORD)(R_SI & WORD_MASK) << WORD_SHIFT) | (DWORD)(R_DI & WORD_MASK);

        if (!DosHandleIsFile((PVOID const *)machine->FileHandles, handle))
        {
            SETAX(DOS_ERR_INVALID_HANDLE);
            ERRCF();
        }
        else
        {
            BOOL isOk = ((R_AX & BYTE_MASK) == 0)
                     ? LockFile(machine->FileHandles[handle], offset, 0, length, 0)
                     : UnlockFile(machine->FileHandles[handle], offset, 0, length, 0);

            if (isOk)
                OKCF();
            else /* 33 = lock violation */
            {
                SETAX(DOS_ERR_LOCK_VIOLATION);
                ERRCF();
            }
        }
    }
    else if (function == DOS_FN_SET_HANDLE_COUNT)                /* set maximum handle count */
    {
        /* We keep a fixed DOS_MAX_FILES-entry table, so anything up to that succeeds.
         * NOTE the oracle FAILED this with AX=8 (insufficient memory) when asked
         * for 30 -- that is a property of ITS memory state at that moment, not a
         * rule about DOS, which is why the probe treats the result as
         * informational rather than comparable.
         */
        if ((R_BX & WORD_MASK) <= DOS_MAX_FILES)
            OKCF();
        else
        {
            SETAX(DOS_ERR_INSUFFICIENT_MEMORY);
            ERRCF();
        }
    }
    else if (function == DOS_FN_COMMIT || function == DOS_FN_COMMIT_6A)    /* commit file (flush) */
    {
        DWORD handle = R_BX & WORD_MASK;

        if (!DosHandleIsFile((PVOID const *)machine->FileHandles, handle))
        {
            SETAX(DOS_ERR_INVALID_HANDLE);
            ERRCF();
        }
        else
        {
            FlushFileBuffers(machine->FileHandles[handle]);
            OKCF();
        }
    }
    else if (function == DOS_FN_EXTENDED_OPEN)                /* extended open/create */
    {
        /* BX=mode, CX=attributes, DX=action, DS:SI=name.
         * action: bits 0-3 if it exists (0 fail, 1 open, 2 truncate),
         *         bits 4-7 if it does not (0 fail, 1 create).
         * CX on return says what happened: 1 opened, 2 created, 3 truncated.
         */
        /* #210: AND 716Ch / 71A9h, THE LONG-FILENAME OPEN, ARRIVE HERE TOO -- same
         * registers (BX mode, CX attributes, DX action, DS:SI name; 716Ch's DI alias
         * hint is not used), and Win32 takes a long name as readily as a short one.
         * The action word, the access and the "action taken" in CX are dos_lfn.h's,
         * pinned by tests/unit/lfn_test.c. ONE CHANGE OF ANSWER rode in with the
         * move: CREATE_ALWAYS on a file that was NOT there now reports CX=2 (created),
         * where it said 3 (replaced) for every CREATE_ALWAYS -- RBIL's table, and the
         * probe's first 716Ch is exactly that call. [CAUTION] Unmeasured on 6.22 for 6Ch:
         * p_file's three 6Ch rows (open / exists / missing) do not reach it.
         */
        CHAR fileName[DOS_INT21_PATH_SIZE];
        HANDLE file;
        DWORD slot;
        DWORD disposition;
        DWORD access = (DWORD)DosExtOpenAccess((UINT)(R_BX & WORD_MASK));
        disposition = DosExtOpenDisposition((UINT)(R_DX & WORD_MASK));  /* Win32's own numbers */
        DosGuestPath(machine, R_DS, R_SI, fileName, sizeof(fileName));
        /* FILE_SHARE_WRITE too: we do not emulate SHARE.EXE, so a second open of a
         * file this VDM holds must not fail -- the rule AH=3Dh learned in session 37
         * and this twin had not (#168).
         */
        SetLastError(0);
        file = DosOpenStampable(fileName, access, FILE_SHARE_READ | FILE_SHARE_WRITE, disposition,
                               (DWORD)(R_CX & DOS_LFN_ATTRIBUTE_MASK) ? (DWORD)(R_CX & DOS_LFN_ATTRIBUTE_MASK)
                                                    : FILE_ATTRIBUTE_NORMAL);

        if (file == INVALID_HANDLE_VALUE)
        {
            /* Same collapse as AH=3Dh had, same fix -- see DosErrFromWin32(). */
            DWORD win32Error = GetLastError();
            WORD dosError;
            INT isMapped = DosErrFromWin32((unsigned long)win32Error, &dosError);
            SETAX(dosError);
            ERRCF();
            trace = LogPut(trace, isLfnAlias ? "  INT21 AX=71" : "  INT21 AH=6C");

            if (isLfnAlias)
                trace = LogHexByte(trace, isLfnAlias);

            trace = LogPut(trace, " ["); trace = LogPut(trace, fileName);
            trace = LogPut(trace, "] FAILED win32=0x"); trace = LogHex(trace, win32Error);
            trace = LogPut(trace, isMapped ? " -> AX=0x" : " UNMAPPED, kept -> AX=0x");
            trace = LogHex(trace, dosError); trace = LogPut(trace, "\r\n");
        }
        else
        {
            WORD action = (WORD)DosExtOpenActionTaken((UINT)disposition,
                                                        GetLastError() == ERROR_ALREADY_EXISTS,
                                                        isLfnAlias != 0);
            slot = DosHandleAllocate((PVOID const *)machine->FileHandles, machine->StdOpen);

            if (slot < DOS_MAX_FILES) { machine->FileHandles[slot] = file;
            SETAX(slot);
            SET16(R_CX, action);
            OKCF();

                                        if (action != 1)
                                            DosStampVdmNow(file); /* #263: created/truncated */ }
            else
            {
                CloseHandle(file);
                SETAX(DOS_ERR_TOO_MANY_OPEN_FILES);
                ERRCF();
            }
        }
    }
    else if (function == DOS_FN_EXTENDED_ERROR)                /* get extended error */
    {
        /* Four answers, not one: extended code (AX), class (BH), suggested
         * action (BL) and locus (CH).  The pairings are MEASURED, by provoking
         * each failure on the oracle and asking (tests/probes/dos/p_err.asm):
         *   codes 2, 3, 18  -> BX=0803, CH=02   (not-found family)
         *   code  6         -> BX=0704, CH=01   (bad handle)
         * CL is left ALONE -- the oracle returns it still holding the caller's
         * value, so writing it would be an invention.
         */
        WORD error = machine->LastError;
        WORD classAndAction = 0;
        BYTE locus = 0;
        /* The table moved to src/dos/dos_err.h so the off-VM battery can pin it
         * (tests/unit/err_test.c) and so there is exactly one place a row can
         * be added. Rows 5 (access denied) and 0x50 (file exists) were provoked
         * and measured in session 52; before that both fell into the UNMEASURED
         * arm below.
         */
        if (!DosErrClassify(error, &classAndAction, &locus))
        {
            /* Rather than fabricate a class for a code we have not provoked on
             * real DOS, say so. Extend p_err.asm and dos_err.h together.
             */
            trace = LogPut(trace, "  INT21 AH=59 class/action/locus UNMEASURED for code 0x");
            trace = LogHex(trace, error); trace = LogPut(trace, "\r\n");
        }

        SETAX(error);
        SET16(R_BX, classAndAction);
        R_CX = (R_CX & DOS_INT21_CH_CLEAR_MASK) | (((DWORD)locus & BYTE_MASK) << BYTE_SHIFT);
        OKCF();
    }
    else if (function == DOS_FN_TRUENAME)                /* truename: DS:SI -> ES:DI */
    {
        CHAR input[DOS_INT21_PATH_SIZE];
        CHAR output[DOS_INT21_PATH_SIZE];
        DWORD count;
        DosGuestPath(machine, R_DS, R_SI, input, sizeof(input));
        count = GetFullPathNameA(input, sizeof(output), output, NULL);

        if (count == 0 || count >= sizeof(output))
        {
            SETAX(DOS_ERR_PATH_NOT_FOUND);
            ERRCF();
        }
        else
        {
            volatile BYTE *buffer = (volatile BYTE *)((R_ES << PARAGRAPH_SHIFT) + (R_DI & WORD_MASK));
            INT index = 0;
            /* Oracle: a RELATIVE name resolves against the current directory and
             * comes back fully qualified and UPPER CASE, and existence is not
             * required -- "SUB\\FILE.TXT" became "C:\\SUB\\FILE.TXT" with no such dir.
             */
            while (output[index] && index < DOS_INT21_TRUENAME_MAX)
            {
                CHAR character = output[index];

                if (character >= 'a' && character <= 'z')
                    character = (CHAR)(character - ASCII_CASE_BIT);

                buffer[index] = (BYTE)character;
                ++index;
            }

            buffer[index] = 0;
            OKCF();
        }
    }
    else if (function == DOS_FN_EXTENDED_COUNTRY)                /* get extended country info */
    {
        BYTE subfunction = (BYTE)(R_AX & BYTE_MASK);

        if (subfunction == DOS_INT21_COUNTRY_GENERAL)
        {
            /* Oracle layout: [0]=1 id, [1-2]=size 0x26, [3-4]=country,
             * [5-6]=code page, [7-40]=34-byte country block.  41 bytes total.
             * NOTE the block here is the 34-byte form (24 meaningful + 10 zero),
             * where AH=38h writes only 24 -- measured, not assumed.
             */
            volatile BYTE *buffer = (volatile BYTE *)((R_ES << PARAGRAPH_SHIFT) + (R_DI & WORD_MASK));
            WORD capacity = (WORD)(R_CX & WORD_MASK);
            WORD index;
            BYTE info[DOS_INT21_COUNTRY_INFO_SIZE];
            INT index2;

            for (index2 = 0; index2 < DOS_INT21_COUNTRY_INFO_SIZE; ++index2)
                info[index2] = 0;

            info[0] = DOS_INT21_COUNTRY_GENERAL;
            info[1] = DOS_INT21_COUNTRY_DATA_SIZE;
            info[2] = 0x00;
            info[DOS_INT21_COUNTRY_ID] = DOS_INT21_COUNTRY_US;
            info[DOS_INT21_COUNTRY_ID + 1] = 0x00;              /* country 1 */
            info[DOS_INT21_COUNTRY_CODE_PAGE] = (BYTE)(DOS_INT21_CODE_PAGE & BYTE_MASK);
            info[DOS_INT21_COUNTRY_CODE_PAGE + 1] = (BYTE)(DOS_INT21_CODE_PAGE >> BYTE_SHIFT);              /* code page 437 */

            for (index2 = 0; index2 < DOS_INT21_COUNTRY_TABLE_SIZE; ++index2)
                info[DOS_INT21_COUNTRY_TABLE + index2] = g_DosCountryUs[index2];

            info[DOS_INT21_COUNTRY_TABLE + DOS_INT21_COUNTRY_CASEMAP] = (BYTE)(DOS_CASEMAP_OFF & BYTE_MASK);
            info[DOS_INT21_COUNTRY_TABLE + DOS_INT21_COUNTRY_CASEMAP + 1] = (BYTE)(DOS_CASEMAP_OFF >> BYTE_SHIFT);
            info[DOS_INT21_COUNTRY_TABLE + DOS_INT21_COUNTRY_CASEMAP + 2] = (BYTE)(DOS_HDLR_SEG & BYTE_MASK);
            info[DOS_INT21_COUNTRY_TABLE + DOS_INT21_COUNTRY_CASEMAP + 3] = (BYTE)(DOS_HDLR_SEG >> BYTE_SHIFT);

            for (index = 0; index < DOS_INT21_COUNTRY_INFO_SIZE && index < capacity; ++index)
                buffer[index] = info[index];

            SETAX(DOS_INT21_CODE_PAGE);
            OKCF();                       /* oracle: AX = code page */
        }
        else if (subfunction >= DOS_INT21_COUNTRY_UPPERCASE && subfunction <= DOS_INT21_COUNTRY_DBCS)
        {
            /* Table subfunctions: ES:DI gets a 5-byte descriptor -- the
             * subfunction id, then a FAR POINTER to the table itself.  ATTRIB
             * wants AL=07 and COMMAND.COM AL=04, which is why AL=01 alone was
             * not enough. Offsets from dos_layout.h; contents in dos_ctab.h,
             * dumped from the oracle rather than synthesised.
             */
            volatile BYTE *buffer = (volatile BYTE *)((R_ES << PARAGRAPH_SHIFT) + (R_DI & WORD_MASK));
            WORD offset = 0;

            switch (subfunction)
            {
            case DOS_INT21_COUNTRY_UPPERCASE:
                offset = DOS_CTAB_UPPER;
            break;

            case DOS_INT21_COUNTRY_FILENAME_UPPERCASE:
                offset = DOS_CTAB_FNUPPER;
            break;

            case DOS_INT21_COUNTRY_FILENAME_TERMINATORS:
                offset = DOS_CTAB_FNTERM;
            break;

            case DOS_INT21_COUNTRY_COLLATING:
                offset = DOS_CTAB_COLLATE;
            break;

            case DOS_INT21_COUNTRY_DBCS:
                offset = DOS_CTAB_DBCS;
            break;

            default:
                offset = DOS_CTAB_UPPER;
            break;  /* AL=03, same shape */
            }

            buffer[0] = subfunction;
            buffer[1] = (BYTE)(offset & BYTE_MASK);
            buffer[2] = (BYTE)(offset >> BYTE_SHIFT);
            buffer[3] = (BYTE)(DOS_CTAB_SEG & BYTE_MASK);
            buffer[4] = (BYTE)(DOS_CTAB_SEG >> BYTE_SHIFT);
            SETAX(DOS_INT21_CODE_PAGE);
            OKCF();
        }
        else if (subfunction >= DOS_INT21_CAPITALIZE_CHAR && subfunction <= DOS_INT21_CAPITALIZE_ASCIIZ)
        {
            /* CAPITALISE: a character (DL), CX bytes at DS:DX, or ASCIIZ at DS:DX.
             * (GH #165) Through the SAME uppercase table AL=02 hands out (dumped
             * from 6.22), so a program that capitalises through DOS and one that
             * reads the table agree. Measured: 'a'->'A', 81h->9Ah, digits kept.
             */
            if (subfunction == DOS_INT21_CAPITALIZE_CHAR)
            {
                SET16(R_DX, (R_DX & HIGH_BYTE_MASK) | DosCtabUpcase437((BYTE)(R_DX & BYTE_MASK)));
            }
            else
            {
                volatile BYTE *text = (volatile BYTE *)((R_DS << PARAGRAPH_SHIFT) + (R_DX & WORD_MASK));
                UINT32 index;
                UINT32 count = (subfunction == DOS_INT21_CAPITALIZE_STRING) ? (UINT32)(R_CX & WORD_MASK) : DOS_INT21_CAPITALIZE_ASCIIZ_MAX;

                for (index = 0; index < count; ++index)
                {
                    if (subfunction == DOS_INT21_CAPITALIZE_ASCIIZ && text[index] == 0)
                        break;

                    text[index] = DosCtabUpcase437(text[index]);
                }
            }

            OKCF();
        }
        else if (subfunction == DOS_INT21_YES_NO)
        {
            /* YES/NO for the country: AX = 0 no, 1 yes, 2 neither. Country 1 only,
             * like everything else here. Measured: 'y'->1, 'N'->0, 'q'->2.
             */
            BYTE character = DosCtabUpcase437((BYTE)(R_DX & BYTE_MASK));
            SETAX(character == 'Y' ? DOS_INT21_YES : character == 'N' ? DOS_INT21_NO : DOS_INT21_NEITHER);
            OKCF();
        }
        else
        {
            trace = LogPut(trace, "  INT21 AH=65 AL=0x"); trace = LogHex(trace, subfunction);
            /* Not a gap: 6.22 and PCem answer AX=1 CF=1 too (p_subfn int21.6508). */
            trace = LogPut(trace, " not a 6.22 subfunction -> AX=1 CF=1 (matches DOS)\r\n");
            SETAX(DOS_ERR_INVALID_FUNCTION);
            ERRCF();
        }
    }
    else if (function == DOS_FN_DISK_SERIAL)                /* get/set volume serial number */
    {
        BYTE subfunction = (BYTE)(R_AX & BYTE_MASK);

        if (subfunction == DOS_INT21_GET)
        {
            /* Oracle layout: [0-1] NOT WRITTEN (came back poisoned), [2-5]
             * serial dword, [6-16] 11-byte label, [17-24] 8-byte fs type.
             */
            volatile BYTE *buffer = (volatile BYTE *)((R_DS << PARAGRAPH_SHIFT) + (R_DX & WORD_MASK));
            CHAR label[DOS_INT21_LABEL_SIZE];
            CHAR fileSystemType[DOS_INT21_FILE_SYSTEM_SIZE];
            DWORD serial = 0;
            DWORD maximumComponent = 0;
            DWORD flags = 0;
            INT index;

            for (index = 0; index < DOS_INT21_LABEL_SIZE; ++index)
                label[index] = 0;

            for (index = 0; index < DOS_INT21_FILE_SYSTEM_SIZE; ++index)
                fileSystemType[index] = 0;

            BYTE drive = DosSerialDrive(machine, (BYTE)(R_BX & BYTE_MASK));

            if (drive < DOS_DRIVE_LETTERS && g_DosSerialIsSet[drive])       /* #165: set this session */
            {
                for (index = 0; index < DOS_INT21_SERIAL_INFO_SIZE; ++index)
                    buffer[DOS_INT21_SERIAL_INFO + index] = g_DosSerialInfo[drive][index];

                OKCF();
            }
            else if (GetVolumeInformationA(NULL, label, sizeof(label), &serial,
                                      &maximumComponent, &flags, fileSystemType, sizeof(fileSystemType)))
            {
                buffer[DOS_INT21_SERIAL_INFO] = (BYTE)( serial        & BYTE_MASK);
                buffer[DOS_INT21_SERIAL_INFO + 1] = (BYTE)((serial >> BYTE_SHIFT)  & BYTE_MASK);
                buffer[DOS_INT21_SERIAL_INFO + 2] = (BYTE)((serial >> WORD_SHIFT) & BYTE_MASK);
                buffer[DOS_INT21_SERIAL_INFO + 3] = (BYTE)((serial >> TOP_BYTE_SHIFT) & BYTE_MASK);

                for (index = 0; index < DOS_FCB_NAME_SIZE; ++index)
                    buffer[DOS_INT21_SERIAL_LABEL + index]  = (BYTE)(label[index] ? label[index] : ' ');

                for (index = 0; index < DOS_INT21_FILE_SYSTEM_LENGTH;  ++index)
                    buffer[DOS_INT21_SERIAL_FILE_SYSTEM + index] = (BYTE)(fileSystemType[index] ? fileSystemType[index] : ' ');

                OKCF();
            }
            else
            {
                SETAX(DOS_ERR_INVALID_DRIVE);
                ERRCF();
            }
        }
        else if (subfunction == DOS_INT21_SET)
        {
            /* SET SERIAL (#165). Real DOS writes serial, label and file-system type
             * into the disk's boot record (p_4b05: 6.22 and PCem accept it and 6900h
             * reads the new serial back; DOSBox-X refuses). Our drives are the host's
             * own disks, so by the user's decision (2026-09-28) it is SESSION-ONLY:
             * remembered per drive, answered by 6900h until this VDM ends, and
             * nothing is written to the host disk. Same 25-byte layout as 6900h.
             */
            const volatile BYTE *source = (const volatile BYTE *)((R_DS << PARAGRAPH_SHIFT) + (R_DX & WORD_MASK));
            BYTE drive = DosSerialDrive(machine, (BYTE)(R_BX & BYTE_MASK));
            INT index;

            if (drive < DOS_DRIVE_LETTERS)
            {
                for (index = 0; index < DOS_INT21_SERIAL_INFO_SIZE; ++index)
                    g_DosSerialInfo[drive][index] = source[DOS_INT21_SERIAL_INFO + index];

                g_DosSerialIsSet[drive] = 1;
                trace = LogPut(trace, "  INT21 AX=6901 set serial -- kept for this session only\r\n");
                OKCF();
            }
            else
            {
                SETAX(DOS_ERR_INVALID_DRIVE);
                ERRCF();
            }
        }
        else
        {
            trace = LogPut(trace, "  INT21 AH=69 AL=0x"); trace = LogHex(trace, subfunction);
            trace = LogPut(trace, " UNIMPLEMENTED\r\n");
            machine->Unimplemented[DOS_FN_DISK_SERIAL >> BITMAP_BYTE_SHIFT] |= (BYTE)(1u << (DOS_FN_DISK_SERIAL & BITMAP_BIT_MASK));
            SETAX(DOS_ERR_INVALID_FUNCTION);
            ERRCF();
        }
    }
    else if (function == DOS_FN_GET_CURRENT_DIRECTORY)                /* get current directory -> DS:SI */
    {
        /* DOS returns the path WITHOUT the drive letter and WITHOUT a leading
         * backslash, ASCIIZ.  Oracle at the root writes exactly ONE byte -- the
         * terminating NUL -- and leaves the rest of the caller's 64-byte buffer
         * untouched, so we must not pad it.
         * Wanted by four of the five real 6.22 tools we ran (TREE, ATTRIB,
         * XCOPY, COMMAND.COM), which is why it came first.  GH #32.
         */
        CHAR currentDirectory[DOS_INT21_PATH_SIZE];
        DWORD count;
        BYTE driveNumber = (BYTE)(R_DX & BYTE_MASK);
        BYTE currentDrive = (BYTE)(DosCurrentDrive(machine) + 1);

        if (machine->VirtualDrive >= 0)       /* a drive Win32 cannot stand on: its =X: or root */
        {
            CHAR driveSpec[DOS_INT21_DRIVE_ROOT_SIZE];
            driveSpec[0] = (CHAR)('A' + machine->VirtualDrive);
            driveSpec[1] = ':';
            driveSpec[2] = 0;
            count = GetFullPathNameA(driveSpec, sizeof(currentDirectory), currentDirectory, NULL);
        }
        else
            count = GetCurrentDirectoryA(sizeof(currentDirectory), currentDirectory);

        /* 0xF0 IS krnl386 TALKING TO ntvdm, AND WE ARE ntvdm. (#128, s37) (Importance = 1):
         * For some drives krnl386 issues this call with DL = 0xF0 (seen in the
         * INT 21h trace, s37). 0xF0 is not a drive under any DOS convention --
         * it is a sentinel between the two halves of one product, asking for
         * the current directory, so answering it is implementing a protocol
         * rather than guessing at one. It is what stopped WOWEXEC.EXE
         * resolving: while this call failed, krnl386 gave up on building the
         * path before the PATH search was ever reached.
         *
         * [CAUTION]: Only the EXACT sentinel, never "any invalid drive". A DOS program that
         * passes garbage in DL still gets the error DOS gives it -- turning that
         * into a plausible answer would be the "runs but lies" class.
         */
        if (driveNumber == DOS_INT21_WOW_CURRENT_DRIVE)
        {
            trace = LogPut(trace, "  INT21 AH=47 drive 0xF0 (WOW sentinel) -> current drive\r\n");
            driveNumber = 0;
        }

        /* AND PER-DRIVE CURRENT DIRECTORIES ARE REAL DOS BEHAVIOUR (Importance = 1):
         * This answered only for the drive we happened to be on and returned
         * "invalid drive" for every other, with a note saying so. Win32 keeps a
         * current directory per drive too -- that is what the hidden `=C:`
         * environment variables are -- and GetFullPathNameA("X:") reads it. So
         * ask for the drive the caller named, and keep the honest refusal for a
         * drive that genuinely is not there (GetLogicalDrives), which is the
         * error DOS itself returns. GH #32.
         */
        if (count && driveNumber && driveNumber != currentDrive && driveNumber <= DOS_DRIVE_LETTERS)
        {
            if (GetLogicalDrives() & (1u << (driveNumber - 1)))
            {
                CHAR driveSpec[DOS_INT21_ROOT_PATH_SIZE];
                driveSpec[0] = (CHAR)('A' + driveNumber - 1);
                driveSpec[1] = ':';
                driveSpec[2] = 0;
                count = GetFullPathNameA(driveSpec, sizeof(currentDirectory), currentDirectory, NULL);
            }
            else
                count = 0;
        }

        if (count == 0 || count >= sizeof(currentDirectory))
        {
            trace = LogPut(trace, "  INT21 AH=47 drive 0x"); trace = LogHex(trace, driveNumber);
            trace = LogPut(trace, " -> invalid drive\r\n");
            SETAX(DOS_ERR_INVALID_DRIVE);
            ERRCF();
        }
        else
        {
            volatile BYTE *destination = (volatile BYTE *)((R_DS << PARAGRAPH_SHIFT) + (R_SI & WORD_MASK));
            PCSTR path = currentDirectory;
            INT index = 0;
            /* #164: SHORT AND UPPER CASE, AS DOS KEEPS IT. (s85):
             * The host hands back whatever case and length the directory was
             * entered with ("...\ntvdmex\demo\win16"). DOS's CDS holds an upper-case
             * 8.3 path, and stock NTVDM answers exactly that -- measured beside
             * ours by tests/probes/win16/w_cwd on the rig: ours `...\ntvdmex\demo\...`,
             * stock `...\NTVDMEX\DEMO\...`.
             */
            {   CHAR shortPath[DOS_INT21_PATH_SIZE];
                DWORD shortLength = GetShortPathNameA(currentDirectory, shortPath, sizeof shortPath);
                INT charIndex;

                if (shortLength && shortLength < sizeof shortPath)
                    lstrcpynA(currentDirectory, shortPath, sizeof currentDirectory);

                for (charIndex = 0; currentDirectory[charIndex]; ++charIndex)
                    if (currentDirectory[charIndex] >= 'a' && currentDirectory[charIndex] <= 'z')
                        currentDirectory[charIndex] = (CHAR)(currentDirectory[charIndex] - ASCII_CASE_BIT);
            }

            if (currentDirectory[1] == ':')
                path += DOS_INT21_DRIVE_PREFIX_LENGTH;                              /* drop "C:" */

            if (*path == '\\' || *path == '/')
                ++path;                                 /* drop the separator */

            while (path[index] && index < DOS_INT21_CURRENT_DIRECTORY_MAX)
            {
                destination[index] = (BYTE)path[index];
                ++index;
            }

            destination[index] = 0;
            SETAX(DOS_INT21_CURRENT_DIRECTORY_AX);
            OKCF();                    /* oracle: AX=0100 */
        }
    }
    else if (function == DOS_FN_CHDIR)                /* chdir: DS:DX = ASCIIZ path */
    {
        /* CHDIR NEVER MOVES THE CURRENT DRIVE (Importance = 1):
         * Oracle (p_drv.asm): `3Bh C:\ZZDRV` issued from A: leaves 19h at A:, and
         * 47h for C: then answers ZZDRV -- every drive keeps its own directory.
         * SetCurrentDirectoryA("C:\ZZDRV") moves the PROCESS to C:, which made
         * the DOS current drive follow it. So a path on another drive only sets
         * that drive's =X: variable (which is what "X:" resolves through); the
         * process current directory changes only for the drive we are on.
         * Also measured: a bare "C:" is path-not-found (AX=3), not a no-op.
         */
        CHAR fileName[DOS_INT21_PATH_SIZE];
        CHAR fullPath[DOS_INT21_PATH_SIZE];
        DWORD count;
        BYTE target;
        DosGuestPath(machine, R_DS, R_DX, fileName, sizeof(fileName));
        trace = LogPut(trace, "  INT21 AH=3B chdir ["); trace = LogPut(trace, fileName); trace = LogPut(trace, "]");

        if (fileName[0] && fileName[1] == ':' && !fileName[DOS_INT21_DRIVE_PREFIX_LENGTH])
        {
            SETAX(DOS_ERR_PATH_NOT_FOUND);
            ERRCF();
            trace = LogPut(trace, " -> 3 (drive only)\r\n");
        }
        else if ((count = GetFullPathNameA(fileName, sizeof(fullPath), fullPath, NULL)) == 0 || count >= sizeof(fullPath)
                 || fullPath[1] != ':')
        {
            SETAX(DOS_ERR_PATH_NOT_FOUND);
            ERRCF();
            trace = LogPut(trace, " -> 3\r\n");
        }
        else
        {
            target = (BYTE)((fullPath[0] | ASCII_CASE_BIT) - 'a');

            if (target == DosCurrentDrive(machine))
            {
                if (SetCurrentDirectoryA(fullPath))
                {
                    machine->VirtualDrive = -1;  /* it can be stood on after all */
                    DosNoteDriveDirectory(fullPath);
                    OKCF();
                    trace = LogPut(trace, " -> ok\r\n");
                }
                else { SETAX(DOS_ERR_PATH_NOT_FOUND);
                ERRCF();
                trace = LogPut(trace, " -> 3 (0x");
                trace = LogHex(trace, GetLastError());
                         trace = LogPut(trace, ")\r\n"); }  /* oracle: AX=0003, CF=1 */
            }
            else
            {
                DWORD attributes = GetFileAttributesA(fullPath);

                if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY))
                {
                    DosNoteDriveDirectory(fullPath);
                    OKCF();
                    trace = LogPut(trace, " -> ok (another drive's directory; current drive unchanged)\r\n");
                }
                else { SETAX(DOS_ERR_PATH_NOT_FOUND);
                ERRCF();
                trace = LogPut(trace, " -> 3 (other drive, 0x");
                trace = LogHex(trace, GetLastError());
                         trace = LogPut(trace, ")\r\n"); }
            }
        }
    }
    else if (function == DOS_FN_GET_FREE_SPACE)                /* get free disk space: DL = drive */
    {
        /* AX=sectors/cluster BX=free clusters CX=bytes/sector DX=total clusters.
         * AN INVALID DRIVE RETURNS AX=FFFF WITH CARRY CLEAR -- oracle-confirmed,
         * and easy to get wrong: it is not a CF error.  Counts are 16-bit in the
         * DOS interface, so a large volume has to be clamped rather than wrapped.
         */
        BYTE driveNumber = (BYTE)(R_DX & BYTE_MASK);
        DWORD sectorsPerCluster = 0;
        DWORD bytesPerSector = 0;
        DWORD freeClusters = 0;
        DWORD totalClusters = 0;
        CHAR root[DOS_INT21_ROOT_PATH_SIZE];
        PSTR rootPointer = 0;

        if (!driveNumber && machine->VirtualDrive >= 0)
            driveNumber = (BYTE)(machine->VirtualDrive + 1);

        if (driveNumber) { root[0] = (CHAR)('A' + driveNumber - 1);
        root[1] = ':';
        root[2] = '\\';
                    root[3] = 0;
                    rootPointer = root; }

        if (driveNumber <= DOS_DRIVE_LETTERS && GetDiskFreeSpaceA(rootPointer, &sectorsPerCluster, &bytesPerSector, &freeClusters, &totalClusters))
        {
            SETAX((WORD)sectorsPerCluster);
            SET16(R_BX, freeClusters > WORD_MASK ? WORD_MASK : freeClusters);
            SET16(R_CX, (WORD)bytesPerSector);
            SET16(R_DX, totalClusters  > WORD_MASK ? WORD_MASK : totalClusters);
            OKCF();
        }
        else
        {
            SETAX(DOS_INT21_INVALID_DRIVE_AX);
            OKCF();
        }
    }
    else if (function == DOS_FN_COUNTRY_INFO)                /* get/set country information */
    {
        BYTE subfunction = (BYTE)(R_AX & BYTE_MASK);
        WORD wanted = (subfunction == DOS_INT21_COUNTRY_IN_BX) ? (WORD)(R_BX & WORD_MASK)
                                       : (WORD)(subfunction ? subfunction : 1);

        if ((R_DX & WORD_MASK) == DOS_INT21_COUNTRY_SET)          /* DX=FFFF selects SET, not GET */
        {
            /* MEASURED, 6.22 AND PCem, NO COUNTRY.SYS (p_subfn): setting the
             * CURRENT country succeeds (AX=1 CF=0); any other fails AX=1 CF=1,
             * because the data for it would come from COUNTRY.SYS and none was
             * loaded. We are that machine: country 1 and nothing else. (GH #165)
             */
            SETAX(1);

            if (wanted == 1)
                OKCF();
            else
            {
                trace = LogPut(trace, "  INT21 AH=38 SET country 0x"); trace = LogHex(trace, wanted);
                trace = LogPut(trace, " refused: only country 1 is loaded (matches DOS without COUNTRY.SYS)\r\n");
                ERRCF();
            }
        }
        else if (wanted == 1)                 /* USA -- the only block we have */
        {
            volatile BYTE *buffer = (volatile BYTE *)((R_DS << PARAGRAPH_SHIFT) + (R_DX & WORD_MASK));
            INT index;

            for (index = 0; index < DOS_INT21_COUNTRY_TABLE_SIZE; ++index)
                buffer[index] = g_DosCountryUs[index];

            buffer[DOS_INT21_COUNTRY_CASEMAP] = (BYTE)(DOS_CASEMAP_OFF & BYTE_MASK);
            buffer[DOS_INT21_COUNTRY_CASEMAP + 1] = (BYTE)(DOS_CASEMAP_OFF >> BYTE_SHIFT);
            buffer[DOS_INT21_COUNTRY_CASEMAP + 2] = (BYTE)(DOS_HDLR_SEG & BYTE_MASK);
            buffer[DOS_INT21_COUNTRY_CASEMAP + 3] = (BYTE)(DOS_HDLR_SEG >> BYTE_SHIFT);
            SETAX(1);
            SET16(R_BX, 1);
            OKCF();
        }
        else
        {
            /* We only have measured data for country 1. Inventing a block for
             * another country would be exactly the from-memory guess the
             * programme forbids, so say so rather than fabricate one.
             */
            /* ...and 6.22 without COUNTRY.SYS answers exactly this: AX=1 CF=1
             * (p_subfn int21.382C.get). It was AX=2.
             */
            trace = LogPut(trace, "  INT21 AH=38 country 0x"); trace = LogHex(trace, wanted);
            trace = LogPut(trace, " refused: only country 1 is loaded (matches DOS without COUNTRY.SYS)\r\n");
            SETAX(DOS_ERR_INVALID_FUNCTION);
            ERRCF();
        }
    }
    else if (function == DOS_FN_ALLOCATION_STRATEGY)                /* get/set memory allocation strategy */
    {
        BYTE subfunction = (BYTE)(R_AX & BYTE_MASK);

        if (subfunction == DOS_INT21_STRATEGY_GET)
        {
            SETAX(machine->AllocationStrategy);
            OKCF();
        }
        else if (subfunction == DOS_INT21_STRATEGY_SET)
        {
            machine->AllocationStrategy = (BYTE)(R_BX & BYTE_MASK);
            OKCF();
        }
        /* Oracle: AL=02 returns the state in AL and LEAVES AH ALONE -- 6.22
         * answered AX=5800 to a call made with AX=5802.
         */
        else if (subfunction == DOS_INT21_UMB_LINK_GET)
        {
            SETAX((R_AX & HIGH_BYTE_MASK) | machine->UmbLink);
            OKCF();
        }
        else if (subfunction == DOS_INT21_UMB_LINK_SET)
        {
            /* YOU CANNOT LINK A UMB CHAIN THAT DOES NOT EXIST. (GH #47) (Importance = 1):
             * This accepted the call and stored the flag, and that is why
             * MEM.EXE reported 1,664K of "Upper" memory on a machine with
             * none, and counted the free tail of conventional memory as an
             * upper block -- leaving Conventional free at 0K and "Largest
             * executable program size" at -16 bytes.
             * MEM's own trace is what named it: `21:58/03 bx=0001`, set UMB
             * link ON, immediately before it walks the chain.
             * Oracle, tests/probes/dos/p_umb.asm on MS-DOS 6.22 booted with no
             * EMM386 and no DOS=UMB -- the same configuration we present:
             * CASE=int21.5803.link.on  AX=0001 CF=1
             * CASE=int21.5802.after.on AX=5800          (still not linked)
             * CASE=int21.5803.link.off AX=0001 CF=1
             * i.e. real DOS REFUSES, in both directions, with error 1. We
             * provide no upper memory blocks at all, so refusing is not a
             * limitation being papered over -- it is the true answer.
             */
            trace = LogPut(trace, "  INT21 AH=5803 UMB link refused: no UMB provider "
                          "(oracle: AX=0001 CF=1)\r\n");
            SETAX(DOS_ERR_INVALID_FUNCTION);
            ERRCF();
        }
        else
        {
            trace = LogPut(trace, "  INT21 AH=58 AL=0x"); trace = LogHex(trace, subfunction);
            /* Not a gap: 6.22 and PCem answer AX=1 CF=1 too (p_subfn int21.5804). */
            trace = LogPut(trace, " not a 6.22 subfunction -> AX=1 CF=1 (matches DOS)\r\n");
            SETAX(DOS_ERR_INVALID_FUNCTION);
            ERRCF();
        }
    }
    else if (function == DOS_FN_GET_LIST_OF_LISTS)                /* get list of lists -> ES:BX */
    {
        /* The word at ES:BX-2 is the first MCB segment, and that is the field a
         * memory walker actually follows -- it is filled in truthfully from our
         * own MCB chain. The rest of SysVars is a stub, so it is ZEROED rather
         * than left as whatever was in memory: a walker that follows a garbage
         * DPB or SFT pointer wanders off into nonsense, which is the silent
         * failure #27 exists to remove, whereas a null pointer stops it.
         */
        if (machine->SysvarsSegment)
        {
            SET16(R_ES, machine->SysvarsSegment);
            SET16(R_BX, machine->SysvarsOffset);
            trace = LogPut(trace, "  INT21 AH=52 list-of-lists (MCB head only; rest stubbed)\r\n");
            OKCF();
        }
        else
        {
            trace = LogPut(trace, "  INT21 AH=52 UNIMPLEMENTED (no SysVars planted)\r\n");
            machine->Unimplemented[DOS_FN_GET_LIST_OF_LISTS >> BITMAP_BYTE_SHIFT] |= (BYTE)(1u << (DOS_FN_GET_LIST_OF_LISTS & BITMAP_BIT_MASK));
            SETAX(DOS_ERR_INVALID_FUNCTION);
            ERRCF();
        }
    }
    else if (function == DOS_FN_IOCTL)                /* IOCTL (C-runtime isatty etc.) */
    {
        BYTE subfunction = (BYTE)(R_AX & BYTE_MASK);
        WORD handle = (WORD)(R_BX & WORD_MASK);
        /* 4400h: the device-information word, in DX AND in AX (6.22 and PCem both
         * return AX = DX, p_ioctl's own captures included). #251: AUX is 80C0h and
         * PRN A0C0h (bit 13, output-until-busy) -- measured, p_auxprn.
         */
        if (subfunction == DOS_INT21_GET)
        {
            WORD deviceInfo = (handle < DOS_INT21_STD_HANDLES) ? DOS_INT21_DEVICE_INFO_CONSOLE : DOS_INT21_DEVICE_INFO_FILE;

            if ((handle == DOS_INT21_STDAUX_HANDLE || handle == DOS_INT21_STDPRN_HANDLE) && DosHandleIsDevice((PVOID const *)machine->FileHandles, machine->StdOpen, handle))
                deviceInfo = (handle == DOS_INT21_STDAUX_HANDLE) ? DOS_INT21_DEVICE_INFO_AUX : DOS_INT21_DEVICE_INFO_PRN;

            SET16(R_DX, deviceInfo);
            SETAX(deviceInfo);
            OKCF();
        }
        else if (subfunction == DOS_INT21_IOCTL_INPUT_STATUS || subfunction == DOS_INT21_IOCTL_OUTPUT_STATUS)
        {
            SETAX((R_AX & HIGH_BYTE_MASK) | DOS_INT21_INPUT_READY);
            OKCF();
        }
        /* THE DRIVE-CLASSIFICATION TRIO. (GH #32, #128, session 37) (Importance = 2):
         * AL = 08h "is this block device removable", 09h "is it remote", 0Eh "get
         * the logical drive map". BL is the drive (0 = default, 1 = A) and all
         * three answer in REGISTERS -- no buffer anywhere near them.
         *
         * [CAUTION]: They used to fall into the `else { OKCF(); }` below, which is the worst
         * answer available: carry clear, meaning success, with the registers the
         * caller happened to be holding. That is the "runs but lies" class, and it
         * is what krnl386 uses to decide whether a drive is local. It probes every
         * drive with 44/08, 44/09 and 44/0E in a loop (measured: once per drive,
         * descending), and with all three lying it treated drive C: as
         * non-local -- after which every INT 21h AH=47h for C: failed on its
         * side, its path canonicalisation failed, and LoadModule("WOWEXEC.EXE")
         * reported "file not found" without ever opening a file.
         * - Answered from the host, which is where the guest's drives really are.
         * - NOT yet checked against the MS-DOS 6.22 oracle -- the register contract
         *   here is from the documented interface, not from a run. Worth a panel
         *   (#24) since the whole point of M9 is that we do not write these from
         *   memory. The DX bits beyond 12 are the ones to confirm.
         */
        else if (subfunction == DOS_INT21_IOCTL_REMOVABLE || subfunction == DOS_INT21_IOCTL_REMOTE_DRIVE || subfunction == DOS_INT21_IOCTL_GET_DRIVE_MAP)
        {
            BYTE drive = (BYTE)(handle & BYTE_MASK);      /* 0 = default drive */
            UINT type = 0;

            if (!drive)
                drive = (BYTE)(DosCurrentDrive(machine) + 1);

            if (drive >= 1 && drive <= DOS_DRIVE_LETTERS && (GetLogicalDrives() & (1u << (drive - 1))))
            {
                CHAR root[DOS_INT21_ROOT_PATH_SIZE];
                root[0] = (CHAR)('A' + drive - 1);
                root[1] = ':';
                root[2] = '\\';
                root[3] = 0;
                type = GetDriveTypeA(root);
            }

            if (!type || type == DRIVE_NO_ROOT_DIR) /* invalid drive */
            {
                SETAX(DOS_ERR_INVALID_DRIVE);
                ERRCF();
            }
            else if (subfunction == DOS_INT21_IOCTL_REMOVABLE)
            {
                /* AX = 0 removable, 1 fixed. A CD is removable media. */
                SETAX((type == DRIVE_REMOVABLE || type == DRIVE_CDROM) ? 0 : 1);
                OKCF();
            }
            else if (subfunction == DOS_INT21_IOCTL_REMOTE_DRIVE)
            {
                /* DX = the device attribute word; bit 12 = the drive is remote.
                 * Nothing else in it is load-bearing for the callers we have.
                 */
                SET16(R_DX, (type == DRIVE_REMOTE) ? DOS_INT21_IOCTL_REMOTE_BIT : DOS_INT21_NONE);
                OKCF();
            }
            else
            {
                /* AL = 0 when only one letter maps to the block device, which is
                 * true of every drive we can see (we do not emulate a SUBST).
                 */
                SETAX(R_AX & HIGH_BYTE_MASK);
                OKCF();
            }

            trace = LogPut(trace, "  INT21 AH=44 AL=0x"); trace = LogHex(trace, subfunction);
            trace = LogPut(trace, " drive 0x"); trace = LogHex(trace, drive);
            trace = LogPut(trace, " type "); trace = LogHex(trace, type);
            trace = LogPut(trace, " -> AX=0x"); trace = LogHex(trace, R_AX & WORD_MASK);
            trace = LogPut(trace, " DX=0x"); trace = LogHex(trace, R_DX & WORD_MASK);
            trace = LogPut(trace, (*guestFlags & 1) ? " (err)\r\n" : "\r\n");
        }
        /* #251: WHAT DOS SUPPORTS SUCCEEDS; THE REST IS "INVALID FUNCTION":
         * Every other sub-function answered CF=0 -- success, with nothing done.
         * Measured (tests/probes/dos/p_ioctl2) on msdos622, dosbox-x and pcem alike:
         * 02h/04h (read control data), 0Ch/10h on CON, and the unassigned 12h/1Fh
         * answer AX=0001 CF=1; 0Ah, 0Dh and 11h on a fixed disk answer CF=0.
         * 0Ah reports a local handle (DX bit 15 clear). 0Dh/11h keep today's bare
         * success -- a disk-parameter block is a separate piece of work, and turning
         * a success into a refusal there would break callers that work now.
         */
        else if (subfunction == DOS_INT21_IOCTL_SET_DEVICE_INFO || subfunction == DOS_INT21_IOCTL_SET_RETRY || subfunction == DOS_INT21_IOCTL_GENERIC_BLOCK || subfunction == DOS_INT21_IOCTL_SET_DRIVE_MAP)
        {
            OKCF();
        }
        else if (subfunction == DOS_INT21_IOCTL_REMOTE_HANDLE)
        {
            SET16(R_DX, DOS_INT21_NONE);
            OKCF();
        }
        else if (subfunction == DOS_INT21_IOCTL_QUERY_GENERIC) /* AL=0: supported */
        {
            SETAX(R_AX & HIGH_BYTE_MASK);
            OKCF();
        }
        else
        {
            SETAX(DOS_ERR_INVALID_FUNCTION);
            ERRCF();
        }

        if (subfunction != DOS_INT21_IOCTL_REMOVABLE && subfunction != DOS_INT21_IOCTL_REMOTE_DRIVE && subfunction != DOS_INT21_IOCTL_GET_DRIVE_MAP)
        {
            trace = LogPut(trace, "  INT21 AH=44 ioctl AL=0x"); trace = LogHex(trace, subfunction);
            trace = LogPut(trace, " BX=0x"); trace = LogHex(trace, handle); trace = LogPut(trace, "\r\n");
        }
    }
    else if (function == DOS_FN_DBCS_TABLE)                /* get DBCS lead-byte table */
    {
        if ((R_AX & BYTE_MASK) == 0)
        {
            SET16(R_DS, DOS_HDLR_SEG);
            SET16(R_SI, DOS_DBCS_OFF);
        }

        SETAX(R_AX & HIGH_BYTE_MASK);
        OKCF();
        trace = LogPut(trace, "  INT21 AH=63 DBCS lead-byte table\r\n");
    }
    else if (function == DOS_FN_SET_VECTOR)                /* set interrupt vector: AL=int DS:DX */
    {
        DWORD vectorOffset = (R_AX & BYTE_MASK) * IVT_ENTRY_SIZE;
        *(volatile WORD *)(vectorOffset)     = (WORD)(R_DX & WORD_MASK);
        *(volatile WORD *)(vectorOffset + IVT_SEGMENT_OFFSET) = (WORD)(R_DS & WORD_MASK);
        OKCF();
    }
    else if (function == DOS_FN_GET_VECTOR)                /* get interrupt vector: AL=int -> ES:BX */
    {
        DWORD vectorOffset = (R_AX & BYTE_MASK) * IVT_ENTRY_SIZE;
        SET16(R_BX, *(volatile WORD *)(vectorOffset));
        SET16(R_ES, *(volatile WORD *)(vectorOffset + IVT_SEGMENT_OFFSET));
        OKCF();
    }
    else if (function == DOS_FN_ALLOCATE)                /* allocate BX paras -> AX=seg (err: BX=max) */
    {
        WORD wanted = (WORD)(R_BX & WORD_MASK);
        WORD segment = 0;
        WORD maximum = 0;
        INT error = DosMcbAllocate(NULL, machine->FirstMcb, wanted, &segment, &maximum);

        if (error)
        {
            SET16(R_AX, error);
            SET16(R_BX, maximum);
            ERRCF();
        }
        else
        {
            SET16(R_AX, segment);
            OKCF();
        }

        /* THE BLOCK BELONGS TO THE PROGRAM THAT ASKED. (s80):
         * DOS stamps an allocation with the CURRENT PSP, and that is how it frees a
         * terminated child's memory: every block its PSP owns. DosMcbAllocate() writes
         * DOS_PSP_SEG unconditionally, which is right for the top-level program (its
         * PSP is DOS_PSP_SEG) and wrong for every child -- so nothing a child
         * allocated was ever given back. Measured: DOS/4GW's five real-mode blocks
         * outlived Doom, and the next `doom` loaded 85 KB higher.
         */
        if (!error && segment && machine->PspSegment)
            DosMcbWriteWord(DosMcbSegmentAddress(NULL, (WORD)(segment - 1)) + 1, machine->PspSegment);

        trace = LogPut(trace, "  INT21 AH=48 alloc 0x"); trace = LogHex(trace, wanted);
        trace = LogPut(trace, (*guestFlags & 1) ? " -> err max=0x" : " -> seg=0x");
        trace = LogHex(trace, (*guestFlags & 1) ? maximum : (R_AX & WORD_MASK)); trace = LogPut(trace, "\r\n");
    }
    else if (function == DOS_FN_FREE)                /* free block: ES=segment */
    {
        INT error = DosMcbFree(NULL, (WORD)(R_ES & WORD_MASK));
        /* #258: AND A SUCCESSFUL FREE LEAVES AX = THE BLOCK'S MCB:
         * Undocumented, measured (tests/probes/dos/p_memax): MS-DOS 6.22 and PCem's
         * MS-DOS both return AX = ES-1; dosbox-x leaves AX alone. The Microsoft
         * kernel is the authority. We left AX as the caller's 49xx.
         */
        if (error)
        {
            SET16(R_AX, error);
            ERRCF();
        }
        else
        {
            SET16(R_AX, (WORD)((R_ES & WORD_MASK) - 1));
            OKCF();
        }

        trace = LogPut(trace, "  INT21 AH=49 free seg=0x"); trace = LogHex(trace, R_ES & WORD_MASK);
        trace = LogPut(trace, (*guestFlags & 1) ? " (err)\r\n" : "\r\n");
        /* A refused free names a block the caller believes in and we do not: show
         * what is actually at seg-1, and the chain, so the two can be compared.
         */
        if (error)
        {
            const volatile BYTE *mcb = (const volatile BYTE *)(((R_ES & WORD_MASK) - 1u) << PARAGRAPH_SHIFT);
            WORD segment;
            INT index;
            INT count = 0;
            trace = LogPut(trace, "    at seg-1: ");

            for (index = 0; index < PARAGRAPH_SIZE; ++index)
            {
                trace = LogHexByte(trace, mcb[index]);
                trace = LogPut(trace, " ");
            }

            trace = LogPut(trace, "\r\n    chain:");
            segment = machine->FirstMcb;

            while (segment && count++ < DOS_INT21_TRACE_MCB_MAX)
            {
                const volatile BYTE *chainBlock = (const volatile BYTE *)((DWORD)segment << PARAGRAPH_SHIFT);
                WORD owner = (WORD)(chainBlock[DOS_MCB_OWNER] | (chainBlock[DOS_MCB_OWNER + 1] << BYTE_SHIFT));
                WORD size = (WORD)(chainBlock[DOS_MCB_SIZE] | (chainBlock[DOS_MCB_SIZE + 1] << BYTE_SHIFT));
                trace = LogPut(trace, " "); trace = LogHex(trace, segment); trace = LogPut(trace, chainBlock[0] == 'Z' ? "Z" : chainBlock[0] == 'M' ? "M" : "?");
                trace = LogPut(trace, "/"); trace = LogHex(trace, owner); trace = LogPut(trace, "/"); trace = LogHex(trace, size);

                if (chainBlock[0] != 'M')
                    break;

                segment = (WORD)(segment + 1 + size);
            }

            trace = LogPut(trace, "\r\n");
        }
    }
    else if (function == DOS_FN_RESIZE)                /* resize: ES=block BX=new paras */
    {
        WORD wanted = (WORD)(R_BX & WORD_MASK);
        WORD maximum = 0;
        INT error = DosMcbResize(NULL, (WORD)(R_ES & WORD_MASK), wanted, &maximum);
        /* #258: A SUCCESSFUL RESIZE LEAVES AX = THE BLOCK'S SEGMENT:
         * Undocumented, and QuickBASIC 4.5 depends on it: its Quick Library loader
         * takes AX after shrinking a top-of-memory block as the block's segment.
         * We left the caller's 4Axx there, so `QB /L` loaded the library at 4Axx,
         * freed a block that never existed at Make EXE ("Error in loading file
         * (QB.QLB) - Internal error") and wrote a garbage .LIB into the link.
         * Measured (tests/probes/dos/p_memax): MS-DOS 6.22, dosbox-x and PCem all
         * return AX = ES for a shrink, a grow and a same-size resize.
         */
        if (error)
        {
            SET16(R_AX, error);

            if (error == DOS_ERR_INSUFFICIENT_MEMORY)
                SET16(R_BX, maximum);

            ERRCF();
        }
        else
        {
            SET16(R_AX, (WORD)(R_ES & WORD_MASK));
            OKCF();
        }

        trace = LogPut(trace, "  INT21 AH=4A resize seg=0x"); trace = LogHex(trace, R_ES & WORD_MASK);
        trace = LogPut(trace, " -> 0x"); trace = LogHex(trace, wanted);
        trace = LogPut(trace, (*guestFlags & 1) ? " (err)\r\n" : "\r\n");
    }
    else if (function == DOS_FN_GET_PSP_UNDOCUMENTED || function == DOS_FN_GET_PSP)    /* get current PSP -> BX */
    {
        SET16(R_BX, machine->PspSegment);
        OKCF();
    }
    else if (function == DOS_FN_SET_PSP)                /* set current PSP */
    {
        machine->PspSegment = (WORD)(R_BX & WORD_MASK);
        OKCF();
    }
    else if (function == DOS_FN_SET_DTA)                /* set DTA = DS:DX */
    {
        machine->DtaSegment = (WORD)(R_DS & WORD_MASK);
        machine->DtaOffset = (WORD)(R_DX & WORD_MASK);
        OKCF();
    }
    else if (function == DOS_FN_GET_DTA)                /* get DTA -> ES:BX */
    {
        SET16(R_ES, machine->DtaSegment);
        SET16(R_BX, machine->DtaOffset);
        OKCF();
    }
    else if (function == DOS_FN_GET_DRIVE)                /* get current drive -> AL (C: = 2) */
    {
        /* THE CURRENT DRIVE IS THE HOST CURRENT DIRECTORY'S, LIKE AH=47h's:
         * This returned a constant while AH=0Eh below was accepted and ignored, so
         * a program probing drives the classic way -- select X, read back, compare
         * -- found only C:. QB.EXE's file dialog does exactly that (39BCCh..39BE4h)
         * and listed one drive on a machine with four.
         */
        SETAX((R_AX & HIGH_BYTE_MASK) | DosCurrentDrive(machine));
        OKCF();
    }
    else if (function == DOS_FN_SELECT_DRIVE)                /* select drive -> AL = LASTDRIVE */
    {
        /* Win32 keeps a current directory per drive (the hidden =X: variables), and
         * "X:" as a path means that directory -- so selecting a drive is one call,
         * and a later relative open lands where DOS would put it. A drive that is
         * not there is left unselected, as DOS leaves it; AL is LASTDRIVE either
         * way, which is the documented answer and what a program sizes its drive
         * list from.
         * A DRIVE THAT IS THERE BUT NOT READY IS STILL SELECTED (Importance = 1):
         * Oracle (p_drv.asm): `0Eh B:` on a one-floppy 6.22 machine selects the
         * phantom B: with nothing in it and 19h reads back 1; only a letter with
         * no device behind it (D: with LASTDRIVE=E, Z:) is refused. Win32 refuses
         * to chdir onto an empty floppy or CD-ROM (NOT READY), and this used to
         * leave the guest on C: -- so QB.EXE's select/read-back probe found ONE
         * drive on a machine with four. Now the selection is held in m->vdrive
         * and every relative path goes to that drive (DosGuestPath), where the access
         * fails as DOS's would.
         */
        BYTE driveNumber = (BYTE)(R_DX & BYTE_MASK);

        if (driveNumber < DOS_DRIVE_LETTERS && (GetLogicalDrives() & (1u << driveNumber)))
        {
            CHAR driveSpec[DOS_INT21_DRIVE_ROOT_SIZE];
            CHAR currentDirectory[DOS_INT21_PATH_SIZE];
            driveSpec[0] = (CHAR)('A' + driveNumber);
            driveSpec[1] = ':';
            driveSpec[2] = 0;
            /* remember the directory we are leaving; "X:" resolves through =X: */
            if (machine->VirtualDrive < 0 && GetCurrentDirectoryA(sizeof(currentDirectory), currentDirectory))
                DosNoteDriveDirectory(currentDirectory);

            if (SetCurrentDirectoryA(driveSpec))
                machine->VirtualDrive = -1;
            else                                         /* e.g. no media: try the root */
            {
                CHAR root[DOS_INT21_ROOT_PATH_SIZE];
                root[0] = driveSpec[0];
                root[1] = ':';
                root[2] = '\\';
                root[3] = 0;

                if (SetCurrentDirectoryA(root))
                    machine->VirtualDrive = -1;
                else
                {
                    machine->VirtualDrive = driveNumber;
                    trace = LogPut(trace, "  INT21 AH=0E drive ");
                    *trace++ = driveSpec[0];
                    trace = LogPut(trace, ": exists but is not ready (Win32 error 0x");
                    trace = LogHex(trace, GetLastError());
                    trace = LogPut(trace, ") -> selected as the DOS current drive anyway\r\n");
                }
            }
        }

        SETAX((R_AX & HIGH_BYTE_MASK) | DOS_LASTDRIVE);
        OKCF();
    }
    else if (function == DOS_FN_DISK_RESET)                /* disk reset (flush) -> nop */
    {
        OKCF();
    }
    else if (function == DOS_FN_BREAK_STATE)                /* get/set Ctrl-Break, get true version */
    {
        BYTE subfunction = (BYTE)(R_AX & BYTE_MASK);
        /* THE FLAG IS STATE, NOT A CONSTANT. (GH #165):
         * Get used to answer "off" and set accepted a value and dropped it, so a
         * program that turned checking on read back off. Measured, 6.22 and PCem
         * (tests/probes/dos/p_subfn.asm): 3301 DL=1 then 3300 -> DL=1; 3302 swaps
         * and returns the OLD state in DL. DH is left alone -- 6.22 does.
         */
        if (subfunction == DOS_INT21_BREAK_GET)
        {
            SET16(R_DX, (R_DX & HIGH_BYTE_MASK) | machine->IsBreakOn);
            OKCF();
        }
        else if (subfunction == DOS_INT21_BREAK_SET)
        {
            machine->IsBreakOn = (BYTE)((R_DX & BYTE_MASK) ? 1 : 0);
            OKCF();
        }
        else if (subfunction == DOS_INT21_BREAK_SWAP)
        {
            BYTE previous = machine->IsBreakOn;
            machine->IsBreakOn = (BYTE)((R_DX & BYTE_MASK) ? 1 : 0);
            SET16(R_DX, (R_DX & HIGH_BYTE_MASK) | previous);
            OKCF();
        }
        else if (subfunction == DOS_INT21_BOOT_DRIVE) /* boot drive = C: */
        {
            SET16(R_DX, DOS_INT21_BOOT_DRIVE_C);
            OKCF();
        }
        else if (subfunction == DOS_INT21_TRUE_VERSION)                          /* get TRUE version */
        {
            /* BL=major BH=minor DL=revision DH=flags.  DH bit 3 = DOS in ROM,
             * bit 4 = DOS in HMA; we are in neither, so 0.  Note the oracle
             * reports DH=0x10 because that image boots DOS=HIGH -- DH is a
             * property of the host's configuration, not of the version, which
             * is why the probes do not compare it.
             */
            /* [CAUTION]: Real SETVER leaves the TRUE version alone. Ours follows the same
             * per-process rule anyway: whether XP's shell checks 3306h as well is not
             * measured, and a shell that refuses to start is the costlier mistake.
             */
            SET16(R_BX, DosVersionWord(machine));
            SET16(R_DX, DOS_INT21_NONE);
            OKCF();
        }
        else                                                 /* not a 6.22 subfn */
        {
            /* Not a gap: 6.22 and PCem both answer AL=FFh (p_subfn int21.3307) and
             * leave the carry alone, so we do exactly that.
             */
            trace = LogPut(trace, "  INT21 AH=33 AL=0x"); trace = LogHex(trace, subfunction);
            trace = LogPut(trace, " not a 6.22 subfunction -> AL=FF (matches DOS)\r\n");
            SETAX((R_AX & HIGH_BYTE_MASK) | DOS_INT21_SUBFUNCTION_INVALID_AL);
        }
    }
    else if (function == DOS_FN_GET_DATE)                /* get date: CX=yr DH=mon DL=day AL=dow */
    {
        /* The VDM's clock, not the host's: host-now + whatever 2Bh/2Dh set. See
         * dos_clock.h. With nothing set the offset is 0 and this is GetLocalTime.
         * #262: a tick count the BIOS did not write is followed first, date included
         * (its midnight rollovers are DOS's day number moving).
         */
        DOS_CLOCK_TIME clock;
        DosClockSync();
        DosClockRead(g_DosClock.DosOffset, &clock);
        SET16(R_CX, clock.Year);
        SET16(R_DX, ((clock.Month & BYTE_MASK) << BYTE_SHIFT) | (clock.Day & BYTE_MASK));
        SETAX((R_AX & HIGH_BYTE_MASK) | (clock.DayOfWeek & BYTE_MASK));
        OKCF();
    }
    else if (function == DOS_FN_GET_TIME)                /* get time: CH=hr CL=min DH=sec DL=cs */
    {
        /* #262: CLOCK$ reads 0040:006C, so a raw store there moves this (p_tick2c
         * tick2c.after.store) -- followed here, once, then host-now + offset again.
         */
        DOS_CLOCK_TIME clock;
        DosClockSync();
        DosClockRead(g_DosClock.DosOffset, &clock);
        SET16(R_CX, ((clock.Hour & BYTE_MASK) << BYTE_SHIFT) | (clock.Minute & BYTE_MASK));
        SET16(R_DX, ((clock.Second & BYTE_MASK) << BYTE_SHIFT) | (clock.Hundredths & BYTE_MASK));
        OKCF();
    }
    else if (function == DOS_FN_SET_DATE || function == DOS_FN_SET_TIME)    /* set date / set time */
    {
        /* GH #250: THESE ANSWERED "DONE" AND CHANGED NOTHING (Importance = 1):
         * A program that set the date and read it back got today. Now they move the
         * VDM's clock -- an OFFSET from the host's, so the machine's own clock is
         * never touched (dos_clock.h says why that is the only safe shape). AL=FFh
         * for anything DOS refuses, and a refused call leaves the clock alone --
         * p_clock.asm measured both, on 6.22, PCem and DOSBox-X.
         * - AND THE OTHER TWO CLOCKS FOLLOW, as they do on an AT, because DOS's CLOCK$
         *   driver writes them: the RTC (INT 1Ah AH=02h/04h, CMOS 00h-09h) is synced
         *   to the new reading, and on a time set the BIOS tick count at 0040:006C is
         *   reloaded with the ticks since midnight (p_clock clk.1a02.after.2d,
         *   clk.1a00.after.2d, clk.1a04.after.2b).
         */
        DOS_CLOCK_TIME host;
        INT isOk;
        DosClockSync();                 /* #262: 2Bh keeps the time of day the COUNT says */
        DosClockHostNow(&host);

        if (function == DOS_FN_SET_DATE)
        {
            UINT year = R_CX & WORD_MASK;
            UINT month = (R_DX >> BYTE_SHIFT) & BYTE_MASK;
            UINT day = R_DX & BYTE_MASK;
            isOk = DosClockIsDosDateValid(year, month, day);

            if (isOk)
                DosClockSetDate(&host, &g_DosClock.DosOffset, year, month, day);
        }
        else
        {
            UINT hour = (R_CX >> BYTE_SHIFT) & BYTE_MASK;
            UINT minute = R_CX & BYTE_MASK;
            UINT second = (R_DX >> BYTE_SHIFT) & BYTE_MASK;
            UINT hundredths = R_DX & BYTE_MASK;
            isOk = DosClockIsTimeValid(hour, minute, second, hundredths);

            if (isOk)
            {
                DosClockSetTime(&host, &g_DosClock.DosOffset, hour, minute, second, hundredths);

                if (machine->SetTicks)
                    machine->SetTicks(machine->TicksContext, DosClockTicksFromTime(hour, minute, second, hundredths));
            }
        }

        if (isOk)
            g_DosClock.RtcOffset = g_DosClock.DosOffset;

        trace = LogPut(trace, "  INT21 AH=0x"); trace = LogHexByte(trace, (UINT)function);
        trace = LogPut(trace, isOk ? " VDM clock set (host clock untouched)\r\n"
                         : " refused: invalid -> AL=FF, clock unchanged\r\n");
        SETAX((R_AX & HIGH_BYTE_MASK) | (isOk ? DOS_INT21_CLOCK_SET : DOS_INT21_CLOCK_INVALID));
        OKCF();
    }
    else if (function == DOS_FN_LFN)                /* the long-filename API (#210) */
    {
        /* AH=71h, THE WINDOWS 95 LONG-FILENAME API, AS STOCK NTVDM PROVIDES IT. (#210)
         * Until now this arm answered every 71xxh with AX=7100h CF=1, the documented
         * "no LFN API here" (s81: 6.22's own answer, AX=7100h with CF CLEAR, had XP's
         * EDIT.COM take 7100h for a file handle -- p_subfn int21.716C). Stock NTVDM
         * implements the API, and XP's DOS tools are written against it.
         * - SERVED HERE: 710Dh reset drive, 7141h delete (SI=1: wildcards + CL/CH), 7143h
         *   attributes and times (BL 0-8), 7147h current directory (long form), 714Eh/
         *   714Fh/71A1h find, 7160h truename (CL 0 full / 1 short / 2 long), 71A0h volume
         *   information, 71A6h file info by handle, 71A7h time conversion, 71A8h short
         *   name, 71AAh SUBST. 7139h/713Ah/713Bh/7156h/716Ch/71A9h never reach this arm:
         *   they are their short-name twins (see `lfn_alias` at the top).
         * - ANYTHING ELSE -- 71A2h-71A5h, 71FFh, ... -- IS AX=7100h CF=1, the answer the
         *   whole API used to give and the one LFN clients test for. [CAUTION] That stock answers
         *   an unknown 71xxh this way is p_lfn's lfn.71FF row, not yet measured.
         *
         * [CAUTION]: EVERY REGISTER CONTRACT HERE IS RBIL's, NOT A MEASUREMENT. Which registers
         * stock writes on success (does 71A0h touch AX? does 7143h BL=0 copy CX into AX
         * as 4300h does on 6.22?) is what tests/probes/dos/p_lfn.asm prints; the arms
         * below write only the outputs RBIL names and leave AX alone on success.
         */
        BYTE subfunction = (BYTE)(R_AX & BYTE_MASK);

        if (subfunction == DOS_INT21_LFN_DELETE)         /* delete: DS:DX, SI=wildcards, CL/CH */
        {
            CHAR fileName[DOS_INT21_PATH_SIZE];
            WORD flags = (WORD)(R_SI & WORD_MASK);
            DosGuestPath(machine, R_DS, R_DX, fileName, sizeof(fileName));

            if (flags == 0)
            {
                /* SI=0: one file, no wildcards -- 41h with an LFN-shaped error code. */
                if (DeleteFileA(fileName))
                    OKCF();
                else
                {
                    SETAX(DosLfnError(GetLastError()));
                    ERRCF();
                }
            }
            else
            {
                /* SI=1: every match of the pattern whose attributes pass CL (allowed) and
                 * CH (required); directories are never deleted. Success if any went.
                 */
                WIN32_FIND_DATAA findData;
                HANDLE find;
                INT isFound = 0;
                INT cut = 0;
                INT position;
                DWORD win32Error = ERROR_FILE_NOT_FOUND;
                CHAR fullPath[DOS_INT21_PATH_SIZE];

                for (position = 0; fileName[position]; ++position)
                    if (fileName[position] == '\\' || fileName[position] == '/' || fileName[position] == ':')
                        cut = position + 1;

                find = FindFirstFileA(fileName, &findData);

                if (find == INVALID_HANDLE_VALUE)
                    win32Error = GetLastError();
                else
                {
                    do
                    {
                        if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                            continue;

                        if (!DosLfnAttributesOk(findData.dwFileAttributes, (BYTE)(R_CX & BYTE_MASK),
                                             (BYTE)((R_CX >> BYTE_SHIFT) & BYTE_MASK)))
                            continue;

                        if (cut + lstrlenA(findData.cFileName) >= (INT)sizeof(fullPath))
                            continue;

                        for (position = 0; position < cut; ++position)
                            fullPath[position] = fileName[position];

                        lstrcpynA(fullPath + cut, findData.cFileName, sizeof(fullPath) - cut);

                        if (DeleteFileA(fullPath))
                            isFound = 1;
                        else
                            win32Error = GetLastError();
                    } while (FindNextFileA(find, &findData));

                    FindClose(find);
                }

                if (isFound)
                    OKCF();
                else
                {
                    SETAX(DosLfnError(win32Error));
                    ERRCF();
                }
            }
        }
        else if (subfunction == DOS_INT21_LFN_ATTRIBUTES)         /* attributes and times: DS:DX, BL */
        {
            CHAR fileName[DOS_INT21_PATH_SIZE];
            BYTE action = (BYTE)(R_BX & BYTE_MASK);
            WIN32_FILE_ATTRIBUTE_DATA attributeData;
            DosGuestPath(machine, R_DS, R_DX, fileName, sizeof(fileName));

            if (action == DOS_INT21_LFN_ATTR_GET_ATTRIBUTES || action == DOS_INT21_LFN_ATTR_GET_COMPRESSED_SIZE || action == DOS_INT21_LFN_ATTR_GET_WRITE_TIME || action == DOS_INT21_LFN_ATTR_GET_ACCESS_TIME || action == DOS_INT21_LFN_ATTR_GET_CREATION_TIME)
            {
                if (!GetFileAttributesExA(fileName, GetFileExInfoStandard, &attributeData))
                {
                    SETAX(DosLfnError(GetLastError()));
                    ERRCF();
                }
                else if (action == DOS_INT21_LFN_ATTR_GET_ATTRIBUTES)
                {
                    /* CX = the attributes. [CAUTION] Masked to DOS's six bits as 4300h is (a file
                     * with none set reads 0, not Win32's 80h NORMAL); unmeasured on stock.
                     */
                    SET16(R_CX, (WORD)(attributeData.dwFileAttributes & DOS_LFN_ATTRIBUTE_MASK));
                    SETAX((WORD)(attributeData.dwFileAttributes & DOS_LFN_ATTRIBUTE_MASK));  /* stock: AX = CX too (p_lfn) */
                    OKCF();
                }
                else if (action == DOS_INT21_LFN_ATTR_GET_COMPRESSED_SIZE)
                {
                    /* DX:AX = the size the file occupies (compressed). */
                    DWORD high = 0;
                    DWORD low;
                    SetLastError(NO_ERROR);
                    low = GetCompressedFileSizeA(fileName, &high);

                    if (low == INVALID_FILE_SIZE && GetLastError() != NO_ERROR)
                    {
                        SETAX(DosLfnError(GetLastError()));
                        ERRCF();
                    }
                    else
                    {
                        SETAX(low & WORD_MASK);
                        SET16(R_DX, (low >> WORD_SHIFT) & WORD_MASK);
                        OKCF();
                    }
                }
                else
                {
                    /* 4: last write -> CX time, DI date. 6: last access -> DI date.
                     * 8: creation -> CX time, DI date, SI 10 ms units. All LOCAL, as
                     * 5700h's are. A time before 1980 reads as 0.
                     */
                    const FILETIME *targetTime = (action == DOS_INT21_LFN_ATTR_GET_WRITE_TIME) ? &attributeData.ftLastWriteTime
                                       : (action == DOS_INT21_LFN_ATTR_GET_ACCESS_TIME) ? &attributeData.ftLastAccessTime
                                                        : &attributeData.ftCreationTime;
                    WORD dosDate = 0;
                    WORD dosTime = 0;
                    BYTE hundredths = 0;

                    if (!DosLfnFileTimeToDos(DosFileTimeZoned(targetTime, DOS_TIME_LOCAL), &dosDate, &dosTime, &hundredths))
                    {
                        dosDate = 0;
                        dosTime = 0;
                        hundredths = 0;
                    }

                    SET16(R_DI, dosDate);

                    if (action != DOS_INT21_LFN_ATTR_GET_ACCESS_TIME)
                        SET16(R_CX, dosTime);

                    if (action == DOS_INT21_LFN_ATTR_GET_CREATION_TIME)
                        SET16(R_SI, hundredths);

                    OKCF();
                }
            }
            else if (action == DOS_INT21_LFN_ATTR_SET_ATTRIBUTES)          /* set attributes = CX, as 4301h */
            {
                DWORD attributes = (DWORD)(R_CX & DOS_LFN_ATTRIBUTE_MASK);

                if (!attributes)
                    attributes = FILE_ATTRIBUTE_NORMAL;

                if (SetFileAttributesA(fileName, attributes))
                    OKCF();
                else
                {
                    SETAX(DosLfnError(GetLastError()));
                    ERRCF();
                }
            }
            else if (action == DOS_INT21_LFN_ATTR_SET_WRITE_TIME || action == DOS_INT21_LFN_ATTR_SET_ACCESS_TIME || action == DOS_INT21_LFN_ATTR_SET_CREATION_TIME)
            {
                /* 3: last write = DI date, CX time. 5: last access = DI date (midnight).
                 * 7: creation = DI date, CX time, SI 10 ms units. Local in, UTC to Win32.
                 */
                WORD dosDate = (WORD)(R_DI & WORD_MASK);
                WORD dosTime = (action == DOS_INT21_LFN_ATTR_SET_ACCESS_TIME) ? 0 : (WORD)(R_CX & WORD_MASK);
                BYTE  hundredths = (action == DOS_INT21_LFN_ATTR_SET_CREATION_TIME) ? (BYTE)(R_SI & BYTE_MASK) : 0;
                UINT64 fileTime64;
                FILETIME localTime;
                FILETIME fileTime;
                HANDLE find;

                if (!DosLfnDosToFileTime(dosDate, dosTime, hundredths, &fileTime64)) /* invalid data */
                {
                    SETAX(DOS_ERR_INVALID_DATA);
                    ERRCF();
                }
                else
                {
                    localTime.dwLowDateTime = (DWORD)fileTime64;
                    localTime.dwHighDateTime = (DWORD)(fileTime64 >> DWORD_SHIFT);
                    find = DosLfnOpenAttributes(fileName);

                    if (find == INVALID_HANDLE_VALUE)
                    {
                        SETAX(DosLfnError(GetLastError()));
                        ERRCF();
                    }
                    else
                    {
                        BOOL isOk = LocalFileTimeToFileTime(&localTime, &fileTime)
                                 && SetFileTime(find, action == DOS_INT21_LFN_ATTR_SET_CREATION_TIME ? &fileTime : NULL,
                                                    action == DOS_INT21_LFN_ATTR_SET_ACCESS_TIME ? &fileTime : NULL,
                                                    action == DOS_INT21_LFN_ATTR_SET_WRITE_TIME ? &fileTime : NULL);
                        DWORD win32Error = isOk ? 0 : GetLastError();
                        CloseHandle(find);

                        if (isOk)
                            OKCF();
                        else
                        {
                            SETAX(DosLfnError(win32Error));
                            ERRCF();
                        }
                    }
                }
            }
            else /* BL beyond 8: invalid function */
            {
                SETAX(DOS_ERR_INVALID_FUNCTION);
                ERRCF();
            }
        }
        else if (subfunction == DOS_INT21_LFN_CURRENT_DIRECTORY)         /* current directory, long: DL, DS:SI */
        {
            /* AH=47h's drive rules (0 = default, a drive Win32 cannot stand on answers
             * through its =X:), then the LONG form of the path -- 47h hands back the
             * short upper-case CDS form (#164), this hands back what GetLongPathNameA
             * makes of it, case as the directories were created. No drive letter, no
             * leading backslash, ASCIIZ (RBIL: buffer of 261 bytes).
             */
            CHAR currentDirectory[DOS_INT21_PATH_SIZE];
            CHAR longPath[DOS_INT21_PATH_SIZE];
            DWORD count = 0;
            BYTE driveNumber = (BYTE)(R_DX & BYTE_MASK);
            BYTE currentDrive = (BYTE)(DosCurrentDrive(machine) + 1);

            if (driveNumber == 0 || driveNumber == currentDrive)
            {
                if (machine->VirtualDrive >= 0)
                {
                    CHAR driveSpec[DOS_INT21_DRIVE_ROOT_SIZE];
                    driveSpec[0] = (CHAR)('A' + machine->VirtualDrive);
                    driveSpec[1] = ':';
                    driveSpec[2] = 0;
                    count = GetFullPathNameA(driveSpec, sizeof(currentDirectory), currentDirectory, NULL);
                }
                else
                    count = GetCurrentDirectoryA(sizeof(currentDirectory), currentDirectory);
            }
            else if (driveNumber <= DOS_DRIVE_LETTERS && (GetLogicalDrives() & (1u << (driveNumber - 1))))
            {
                CHAR driveSpec[DOS_INT21_DRIVE_ROOT_SIZE];
                driveSpec[0] = (CHAR)('A' + driveNumber - 1);
                driveSpec[1] = ':';
                driveSpec[2] = 0;
                count = GetFullPathNameA(driveSpec, sizeof(currentDirectory), currentDirectory, NULL);
            }

            if (count == 0 || count >= sizeof(currentDirectory))
            {
                SETAX(DOS_ERR_INVALID_DRIVE);
                ERRCF();
            }
            else
            {
                volatile BYTE *destination = (volatile BYTE *)((R_DS << PARAGRAPH_SHIFT) + (R_SI & WORD_MASK));
                PCSTR path = currentDirectory;
                INT index = 0;
                DWORD longLength = GetLongPathNameA(currentDirectory, longPath, sizeof(longPath));

                if (longLength && longLength < sizeof(longPath))
                    path = longPath;

                if (path[0] && path[1] == ':')
                    path += DOS_INT21_DRIVE_PREFIX_LENGTH;

                if (*path == '\\' || *path == '/')
                    ++path;

                while (path[index] && index < MAX_PATH)
                {
                    destination[index] = (BYTE)path[index];
                    ++index;
                }

                destination[index] = 0;
                OKCF();
            }
        }
        else if (subfunction == DOS_FN_FIND_FIRST || subfunction == DOS_FN_FIND_NEXT)    /* find first / next -> ES:DI */
        {
            volatile BYTE *buffer = (volatile BYTE *)((R_ES << PARAGRAPH_SHIFT) + (R_DI & WORD_MASK));
            INT isDosFormat = (R_SI & WORD_MASK) == 1;
            WIN32_FIND_DATAA findData;

            if (subfunction == DOS_FN_FIND_FIRST)
            {
                /* DS:DX pattern, matched against long AND short names (Win32's rule, and
                 * the LFN API's); CL allowed / CH required attributes; SI time format.
                 */
                CHAR pattern[DOS_INT21_PATH_SIZE];
                HANDLE find;
                UINT slot;
                BYTE allowedAttributes = (BYTE)(R_CX & BYTE_MASK);
                BYTE requiredAttributes = (BYTE)((R_CX >> BYTE_SHIFT) & BYTE_MASK);
                DosGuestPath(machine, R_DS, R_DX, pattern, sizeof(pattern));
                find = FindFirstFileA(pattern, &findData);

                if (find == INVALID_HANDLE_VALUE)
                {
                    SETAX(DosLfnError(GetLastError()));
                    ERRCF();
                }
                else
                {
                    INT isOk = 1;

                    while (!DosLfnAttributesOk(findData.dwFileAttributes, allowedAttributes, requiredAttributes))
                        if (!FindNextFileA(find, &findData))
                        {
                            isOk = 0;
                            break;
                        }

                    if (!isOk) /* nothing passed CL/CH */
                    {
                        FindClose(find);
                        SETAX(DOS_ERR_NO_MORE_FILES);
                        ERRCF();
                    }
                    else
                    {
                        for (slot = 0; slot < DOS_LFN_FIND_SLOTS && g_DosLfnFinds[slot]; ++slot)
                        {
                        }

                        if (slot >= DOS_LFN_FIND_SLOTS)              /* full: recycle, round robin */
                        {
                            slot = g_DosLfnNext++ % DOS_LFN_FIND_SLOTS;
                            FindClose(g_DosLfnFinds[slot]);
                            trace = LogPut(trace, "  INT21 AX=714E search table full -- recycled handle 0x");
                            trace = LogHex(trace, slot + 1); trace = LogPut(trace, "\r\n");
                        }

                        g_DosLfnFinds[slot] = find;
                        g_DosLfnAllow[slot] = allowedAttributes;
                        g_DosLfnNeed[slot] = requiredAttributes;
                        DosLfnFindFill(buffer, &findData, isDosFormat);
                        SETAX(slot + 1);
                        SET16(R_CX, 0);              /* CX: no lossy names */
                        OKCF();
                    }
                }

                if (machine->IsTraceAll) { trace = LogPut(trace, "  INT21 AX=714E ["); trace = LogPut(trace, pattern);
                                    trace = LogPut(trace, (*guestFlags & 1) ? "] -> none\r\n" : "] -> found\r\n"); }
            }
            else
            {
                UINT slot = (UINT)(R_BX & WORD_MASK) - 1u;

                if (slot >= DOS_LFN_FIND_SLOTS || !g_DosLfnFinds[slot])
                {
                    SETAX(DOS_ERR_INVALID_HANDLE);
                    ERRCF();
                }
                else
                {
                    INT isOk = 0;

                    while (FindNextFileA(g_DosLfnFinds[slot], &findData))
                        if (DosLfnAttributesOk(findData.dwFileAttributes, g_DosLfnAllow[slot], g_DosLfnNeed[slot]))
                        {
                            isOk = 1;
                            break;
                        }

                    /* "No more files" leaves the handle OPEN: the program closes it, 71A1h. */
                    if (!isOk)
                    {
                        SETAX(DOS_ERR_NO_MORE_FILES);
                        ERRCF();
                    }
                    else
                    {
                        DosLfnFindFill(buffer, &findData, isDosFormat);
                        SET16(R_CX, 0);
                        OKCF();
                    }
                }
            }
        }
        else if (subfunction == DOS_INT21_LFN_FIND_CLOSE)         /* find close: BX */
        {
            UINT slot = (UINT)(R_BX & WORD_MASK) - 1u;

            if (slot >= DOS_LFN_FIND_SLOTS || !g_DosLfnFinds[slot])
            {
                SETAX(DOS_ERR_INVALID_HANDLE);
                ERRCF();
            }
            else
            {
                FindClose(g_DosLfnFinds[slot]);
                g_DosLfnFinds[slot] = 0;
                OKCF();
            }
        }
        else if (subfunction == DOS_FN_TRUENAME)         /* truename: DS:SI -> ES:DI, CL form */
        {
            /* CL=0 the full path (case kept -- 60h upper-cases, this does not: unmeasured),
             * 1 its SHORT form, 2 its LONG form. 1 and 2 ask the file system, so the
             * path must exist; 0 does not (as 60h: "SUB\FILE.TXT" resolves anyway).
             * CH (SUBST expansion) is not looked at: we create no SUBST of our own
             * that a path would need unwrapping from.
             */
            CHAR input[DOS_INT21_PATH_SIZE];
            CHAR fullPath[DOS_INT21_PATH_SIZE];
            CHAR output[DOS_INT21_PATH_SIZE];
            BYTE nameKind = (BYTE)(R_CX & BYTE_MASK);
            DWORD count;
            DosGuestPath(machine, R_DS, R_SI, input, sizeof(input));
            count = GetFullPathNameA(input, sizeof(fullPath), fullPath, NULL);

            if (count == 0 || count >= sizeof(fullPath))
            {
                SETAX(DOS_ERR_PATH_NOT_FOUND);
                ERRCF();
            }
            else if (nameKind > DOS_INT21_TRUENAME_LONG)
            {
                SETAX(DOS_ERR_INVALID_FUNCTION);
                ERRCF();
            }
            else
            {
                if (nameKind == 0)
                {
                    lstrcpynA(output, fullPath, sizeof(output));
                    count = 1;
                }
                else if (nameKind == 1)
                    count = GetShortPathNameA(fullPath, output, sizeof(output));
                else
                    count = GetLongPathNameA(fullPath, output, sizeof(output));

                if (count == 0 || count >= sizeof(output))
                {
                    SETAX(DosLfnError(GetLastError()));
                    ERRCF();
                }
                else
                {
                    volatile BYTE *buffer = (volatile BYTE *)((R_ES << PARAGRAPH_SHIFT) + (R_DI & WORD_MASK));
                    INT index = 0;

                    while (output[index] && index < MAX_PATH)
                    {
                        buffer[index] = (BYTE)output[index];
                        ++index;
                    }

                    buffer[index] = 0;
                    SETAX(0);                           /* stock: AX=0000 (p_lfn) */
                    OKCF();
                }
            }
        }
        else if (subfunction == DOS_INT21_LFN_VOLUME_INFO)         /* volume information: DS:DX root, ES:DI/CX */
        {
            /* BX = flags as Win32 reports them, cut to the four RBIL names -- bit 0 case-
             * sensitive searches, 1 case preserved, 2 Unicode on disk, 15 compressed --
             * plus 4000h "supports the LFN functions", which is the bit that matters.
             * CX = the longest component (255), DX = the longest path, MAX_PATH = 260.
             * ES:DI gets the file-system name ("NTFS", "FAT") within CX bytes.
             *
             * [CAUTION]: DX = 260 is RBIL's "usually"; stock may compute it. AX is left alone.
             */
            CHAR root[DOS_INT21_PATH_SIZE];
            CHAR fileSystem[DOS_INT21_LABEL_SIZE];
            DWORD maximumComponent = 0;
            DWORD flags = 0;
            DosGuestString(R_DS, R_DX, root, sizeof(root));

            if (root[0] && root[1] == ':' && !root[DOS_INT21_DRIVE_PREFIX_LENGTH]) /* Win32 wants "C:\" */
            {
                root[DOS_INT21_DRIVE_PREFIX_LENGTH] = '\\';
                root[DOS_INT21_DRIVE_PREFIX_LENGTH + 1] = 0;
            }

            fileSystem[0] = 0;

            if (!GetVolumeInformationA(root[0] ? root : NULL, NULL, 0, NULL, &maximumComponent, &flags, fileSystem, sizeof(fileSystem)))
            {
                SETAX(DosLfnError(GetLastError()));
                ERRCF();
            }
            else
            {
                volatile BYTE *buffer = (volatile BYTE *)((R_ES << PARAGRAPH_SHIFT) + (R_DI & WORD_MASK));
                UINT capacity = (UINT)(R_CX & WORD_MASK);
                UINT index;

                for (index = 0; capacity && index < capacity - 1 && fileSystem[index]; ++index)
                    buffer[index] = (BYTE)fileSystem[index];

                if (capacity)
                    buffer[index] = 0;

                SET16(R_BX, (WORD)((flags & DOS_INT21_FS_CASE_FLAGS) | (flags & DOS_INT21_FS_COMPRESSED) | DOS_INT21_FS_LFN_APIS));
                SET16(R_CX, (WORD)(maximumComponent ? maximumComponent : DOS_INT21_MAX_COMPONENT));
                SET16(R_DX, MAX_PATH);
                OKCF();
            }

            trace = LogPut(trace, "  INT21 AX=71A0 ["); trace = LogPut(trace, root);
            trace = LogPut(trace, "] fs="); trace = LogPut(trace, fileSystem); trace = LogPut(trace, " flags=0x"); trace = LogHex(trace, flags);
            trace = LogPut(trace, (*guestFlags & 1) ? " (err)\r\n" : "\r\n");
        }
        else if (subfunction == DOS_INT21_LFN_HANDLE_INFO)         /* file info by handle: BX -> DS:DX */
        {
            /* BY_HANDLE_FILE_INFORMATION, 52 bytes, exactly as Win32 lays it out (times
             * UTC, as a FILETIME is).
             */
            DWORD handle = R_BX & WORD_MASK;
            BY_HANDLE_FILE_INFORMATION fileInfo;

            if (!DosHandleIsFile((PVOID const *)machine->FileHandles, handle))
            {
                SETAX(DOS_ERR_INVALID_HANDLE);
                ERRCF();
            }
            else if (!GetFileInformationByHandle(machine->FileHandles[handle], &fileInfo))
            {
                SETAX(DosLfnError(GetLastError()));
                ERRCF();
            }
            else
            {
                volatile BYTE *buffer = (volatile BYTE *)((R_DS << PARAGRAPH_SHIFT) + (R_DX & WORD_MASK));
                PCBYTE infoBytes = (PCBYTE)&fileInfo;
                UINT index;

                for (index = 0; index < sizeof(fileInfo) && index < DOS_INT21_HANDLE_INFO_SIZE; ++index)
                    buffer[index] = infoBytes[index];

                OKCF();
            }
        }
        else if (subfunction == DOS_INT21_LFN_TIME_CONVERT)         /* time conversion, BL */
        {
            BYTE action = (BYTE)(R_BX & BYTE_MASK);
            /* [CAUTION]: THE ZONE IS A CHOICE, NOT A MEASUREMENT: a FILETIME here is taken as UTC
             * (what 714Eh SI=0 and 71A6h hand out) and the DOS side as LOCAL (what every
             * DOS time this host reports is), so FILETIME -> DOS converts to local time
             * and back. Windows 95's IFSMgr converts the same way; whether NTVDM does is
             * p_lfn's lfn.71A7.ft2dos -- on a machine whose zone is not UTC the hour
             * differs by the offset if this is wrong.
             */
            if (action == DOS_INT21_TIME_TO_DOS)                 /* DS:SI -> QWORD FILETIME -> CX time, DX date, BH */
            {
                const volatile BYTE *bytes = (const volatile BYTE *)((R_DS << PARAGRAPH_SHIFT) + (R_SI & WORD_MASK));
                FILETIME fileTime;
                WORD dosDate;
                WORD dosTime;
                BYTE hundredths;
                fileTime.dwLowDateTime  = (DWORD)bytes[0] | ((DWORD)bytes[1] << BYTE_SHIFT) | ((DWORD)bytes[2] << WORD_SHIFT) | ((DWORD)bytes[3] << TOP_BYTE_SHIFT);
                fileTime.dwHighDateTime = (DWORD)bytes[4] | ((DWORD)bytes[5] << BYTE_SHIFT) | ((DWORD)bytes[6] << WORD_SHIFT) | ((DWORD)bytes[7] << TOP_BYTE_SHIFT);

                if (!DosLfnFileTimeToDos(DosFileTimeZoned(&fileTime, DOS_TIME_LOCAL), &dosDate, &dosTime, &hundredths))
                {
                    SETAX(DOS_ERR_INVALID_DATA);
                    ERRCF();
                }
                else
                {
                    SET16(R_CX, dosTime);
                    SET16(R_DX, dosDate);
                    /* [CAUTION]: INTENDED DIVERGENCE (s92): for an exact even second stock answers
                     * BH=C7h (199) -- p_lfn lfn.71A7.ft2dos, one measurement -- where the
                     * spec's 10-ms remainder is 0. The spec outranks one oracle reading.
                     */
                    SET16(R_BX, (WORD)((R_BX & BYTE_MASK) | ((WORD)hundredths << BYTE_SHIFT)));
                    OKCF();
                }
            }
            else if (action == DOS_INT21_TIME_FROM_DOS)          /* CX time, DX date, BH -> ES:DI QWORD */
            {
                UINT64 fileTime64;
                FILETIME localTime;
                FILETIME fileTime;

                if (!DosLfnDosToFileTime((WORD)(R_DX & WORD_MASK), (WORD)(R_CX & WORD_MASK),
                                       (BYTE)((R_BX >> BYTE_SHIFT) & BYTE_MASK), &fileTime64))
                {
                    SETAX(DOS_ERR_INVALID_DATA);
                    ERRCF();
                }
                else
                {
                    volatile BYTE *bytes = (volatile BYTE *)((R_ES << PARAGRAPH_SHIFT) + (R_DI & WORD_MASK));
                    INT index;
                    localTime.dwLowDateTime = (DWORD)fileTime64;
                    localTime.dwHighDateTime = (DWORD)(fileTime64 >> DWORD_SHIFT);

                    if (!LocalFileTimeToFileTime(&localTime, &fileTime))
                        fileTime = localTime;

                    for (index = 0; index < X86_DWORD_SIZE; ++index)
                    {
                        bytes[index]     = (BYTE)(fileTime.dwLowDateTime  >> (BYTE_SHIFT * index));
                        bytes[X86_DWORD_SIZE + index] = (BYTE)(fileTime.dwHighDateTime >> (BYTE_SHIFT * index));
                    }

                    OKCF();
                }
            }
            else
            {
                SETAX(DOS_ERR_INVALID_FUNCTION);
                ERRCF();
            }
        }
        else if (subfunction == DOS_INT21_LFN_SHORT_NAME)         /* generate short name: DS:SI -> ES:DI, DH */
        {
            /* DH=0: 11 bytes, FCB style; DH=1: "NAME.EXT" ASCIIZ. DL's character-set
             * nibbles are not looked at -- see the code-page note at the helpers.
             */
            CHAR longName[DOS_INT21_PATH_SIZE];
            CHAR shortName[DOS_INT21_DTA_NAME_LENGTH + 1];
            CHAR fcbName[DOS_FCB_NAME_SIZE];
            volatile BYTE *buffer = (volatile BYTE *)((R_ES << PARAGRAPH_SHIFT) + (R_DI & WORD_MASK));
            INT index;
            DosGuestString(R_DS, R_SI, longName, sizeof(longName));
            DosLfnShortName(longName, shortName, fcbName);

            if (((R_DX >> BYTE_SHIFT) & BYTE_MASK) == 0)
                for (index = 0; index < DOS_FCB_NAME_SIZE; ++index)
                    buffer[index] = (BYTE)fcbName[index];
            else
            {
                for (index = 0; shortName[index]; ++index)
                    buffer[index] = (BYTE)shortName[index];

                buffer[index] = 0;
            }

            OKCF();
        }
        else if (subfunction == DOS_INT21_LFN_SUBST)         /* SUBST: BH 0 create / 1 terminate / 2 query */
        {
            /* BL = drive (0 = default, 1 = A:). The host's own DOS-device table is what a
             * SUBST is on NT (XP's SUBST.EXE is DefineDosDevice), so this is real: a drive
             * created here is visible to the user's session until terminated or logoff.
             *
             * [CAUTION]: TERMINATE ONLY UNDOES A SUBST -- a letter whose NT target is "\??\..." --
             * never a real disk or a network mapping; anything else is 0Fh.
             */
            BYTE action = (BYTE)((R_BX >> BYTE_SHIFT) & BYTE_MASK);
            BYTE driveNumber = (BYTE)(R_BX & BYTE_MASK);
            CHAR driveSpec[DOS_INT21_DRIVE_ROOT_SIZE];
            CHAR target[DOS_INT21_PATH_SIZE];
            BYTE drive = (BYTE)(driveNumber ? driveNumber - 1 : DosCurrentDrive(machine));
            driveSpec[0] = (CHAR)('A' + (drive < DOS_DRIVE_LETTERS ? drive : 0));
            driveSpec[1] = ':';
            driveSpec[2] = 0;
            target[0] = 0;

            if (drive >= DOS_DRIVE_LETTERS || action > DOS_INT21_SUBST_QUERY)
            {
                SETAX(action > DOS_INT21_SUBST_QUERY ? DOS_ERR_INVALID_FUNCTION : DOS_ERR_INVALID_DRIVE);
                ERRCF();
            }
            else if (action == 0)
            {
                CHAR input[DOS_INT21_PATH_SIZE];
                CHAR fullPath[DOS_INT21_PATH_SIZE];
                DWORD count;
                DosGuestPath(machine, R_DS, R_DX, input, sizeof(input));
                count = GetFullPathNameA(input, sizeof(fullPath), fullPath, NULL);

                if (GetLogicalDrives() & (1u << drive)) /* letter in use */
                {
                    SETAX(DOS_ERR_INVALID_DRIVE);
                    ERRCF();
                }
                else if (count == 0 || count >= sizeof(fullPath))
                {
                    SETAX(DOS_ERR_PATH_NOT_FOUND);
                    ERRCF();
                }
                else if (DefineDosDeviceA(0, driveSpec, fullPath))
                {
                    OKCF();
                    trace = LogPut(trace, "  INT21 AX=71AA SUBST "); trace = LogPut(trace, driveSpec);
                    trace = LogPut(trace, " = "); trace = LogPut(trace, fullPath); trace = LogPut(trace, " (a host drive)\r\n");
                }
                else
                {
                    SETAX(DosLfnError(GetLastError()));
                    ERRCF();
                }
            }
            else
            {
                DWORD length = QueryDosDeviceA(driveSpec, target, sizeof(target));
                INT isSubst = length > DOS_INT21_NT_PREFIX_LENGTH && target[0] == '\\' && target[1] == '?' && target[2] == '?' && target[3] == '\\';

                if (!isSubst) /* stock query: 89h (p_lfn) */
                {
                    SETAX(action == DOS_INT21_SUBST_QUERY ? DOS_INT21_SUBST_NONE_ERROR : DOS_ERR_INVALID_DRIVE);
                    ERRCF();
                }
                else if (action == 1)
                {
                    if (DefineDosDeviceA(DDD_REMOVE_DEFINITION, driveSpec, NULL))
                        OKCF();
                    else
                    {
                        SETAX(DosLfnError(GetLastError()));
                        ERRCF();
                    }
                }
                else
                {
                    volatile BYTE *buffer = (volatile BYTE *)((R_DS << PARAGRAPH_SHIFT) + (R_DX & WORD_MASK));
                    INT index = 0;

                    while (target[DOS_INT21_NT_PREFIX_LENGTH + index] && index < MAX_PATH)
                    {
                        buffer[index] = (BYTE)target[DOS_INT21_NT_PREFIX_LENGTH + index];
                        ++index;
                    }

                    buffer[index] = 0;
                    OKCF();
                }
            }
        }
        else
        {
            trace = LogPut(trace, "  INT21 AX=71"); trace = LogHexByte(trace, subfunction);
            trace = LogPut(trace, " not an LFN function stock provides -> AX=0001 CF=1\r\n");
            /* s92, MEASURED (dospair p_lfn): stock answers an unknown 71xxh -- and 710Dh,
             * which it does not provide -- with AX=0001 CF=1, "invalid function", NOT the
             * 7100h this arm assumed.
             */
            SETAX(DOS_ERR_INVALID_FUNCTION);
            ERRCF();
        }

        if (machine->IsTraceAll)
        {
            trace = LogPut(trace, "  INT21 AX=71"); trace = LogHexByte(trace, subfunction);
            trace = LogPut(trace, " -> AX=0x"); trace = LogHex(trace, R_AX & WORD_MASK);
            trace = LogPut(trace, (*guestFlags & 1) ? " (err)\r\n" : "\r\n");
        }
    }
    else if (!DosIsDefinedBy622(function))
    {
        /* MS-DOS 6.22 has nothing here, and what IT does is the specification:
         * return with AL cleared and CF clear, touching nothing else.  Measured on
         * the oracle (tests/probes/dos/p_defs.asm) -- AH=6Dh..E0h, plus the
         * documented null functions, all come back with every poisoned output
         * register intact.  Failing loudly here would be US inventing an error
         * that real DOS does not report, which breaks programs that probe for
         * an extension by calling it and checking CF.
         */
        /* [WARNING]: "AX unchanged" WAS HALF A MEASUREMENT (s81). p_defs and p_unimp both
         * called with AL=00h, so "AX unchanged" and "AL cleared" read the same. With
         * AL non-zero (p_subfn: AX=716Ch, 7147h) 6.22 and PCem both answer AX=7100h:
         * DOS ZEROES AL. And that is load-bearing: AX=7100h is exactly how a long-
         * filename client learns there is no LFN API -- XP's EDIT.COM took our
         * unchanged 716Ch for a file handle and reported "Error 6" on EDIT.INI.
         */
        SETAX(R_AX & HIGH_BYTE_MASK);
        trace = LogPut(trace, "  INT21 AH=0x"); trace = LogHex(trace, function);
        trace = LogPut(trace, " undefined on 6.22 -- AL=0, CF clear (matches DOS)\r\n");
        machine->Undefined[(function & BYTE_MASK) >> BITMAP_BYTE_SHIFT] |= (BYTE)(1u << (function & BITMAP_BIT_MASK));
        OKCF();
    }
    else                                      /* unhandled service */
    {
        /* GH #27. Recorded as well as logged, so the STAGE2 block can list every
         * service a run actually wanted -- that list is the to-do list.
         * Reaching HERE means 6.22 defines a real service at this AH and we have
         * not written it yet.  CF=1 is right for that: a quiet "success" would
         * tell the program its request worked when nothing happened.  Functions
         * DOS does not define are handled above and stay silent, matching DOS.
         */
        trace = LogPut(trace, "  INT21 AH=0x"); trace = LogHexByte(trace, (UINT)function);
        trace = LogPut(trace, " AL=0x"); trace = LogHexByte(trace, (UINT)(R_AX & BYTE_MASK));
        trace = LogPut(trace, " UNIMPLEMENTED\r\n");
        machine->Unimplemented[(function & BYTE_MASK) >> BITMAP_BYTE_SHIFT] |= (BYTE)(1u << (function & BITMAP_BIT_MASK));
        ERRCF();
    }

    /* GH #34: remember the last failure for AH=59h. Done HERE, once, rather
     * than at each of the ~20 error sites -- CF and AX are already exactly what
     * the guest is about to see. 59h itself is excluded so reading the error
     * does not overwrite it.
     */
    if (function != DOS_FN_EXTENDED_ERROR && (*guestFlags & 1))
        machine->LastError = (WORD)(R_AX & WORD_MASK);

    /* #34: A HARDWARE ERROR IS A CRITICAL ERROR. Codes 19-31 (not ready, write-
     * protected, ...) go to the program's INT 24h before the call returns; the host
     * makes that call (crit_raise in main.c) and acts on the answer. Here we only
     * say so, and what the handler is to be told. Real mode only (a DPMI client's
     * reflection is separate work), and never while a handler is already running:
     * DOS does not nest INT 24h -- inside one, the call just fails.
     */
    /* #275: ...AND ONLY WHERE IT CAN BE. crit_raise_ok is set by the main V86 exec
     * loop alone (it is the one that acts on crit_pending); #34 keyed this on "not
     * protected mode", so the nested real-mode loops -- a DPMI 0301h/0302h
     * procedure, a reflected IRQ's handler -- set crit_pending and nobody raised it.
     * A handle call (3Fh/40h, the file's own drive in AL) is now raised too.
     */
    /* s92, MEASURED (dospair p_lfn lfn.713B.missing / 713A.again): stock's LFN chdir and
     * rmdir say 2 (file not found) for a directory that is not there, where 6.22's short
     * 3Bh/3Ah -- whose code serves them -- say 3.
     */
    if (isLfnAlias && (isLfnAlias == DOS_FN_RMDIR || isLfnAlias == DOS_FN_CHDIR) && (*guestFlags & EFLAGS_CF)
        && (R_AX & WORD_MASK) == DOS_ERR_PATH_NOT_FOUND)
        SETAX(DOS_ERR_FILE_NOT_FOUND);

    if ((*guestFlags & 1) && machine->CanRaiseCrit && !g_DosInt21IsProtectedMode && !machine->IsCritActive
        && DosCritIsHardwareError((WORD)(R_AX & WORD_MASK)))
    {
        const volatile BYTE *pathBytes = (const volatile BYTE *)((R_DS << PARAGRAPH_SHIFT) + (R_DX & WORD_MASK));
        INT isPathCall = (function == DOS_FN_CREATE || function == DOS_FN_OPEN || function == DOS_FN_FIND_FIRST || function == DOS_FN_MKDIR || function == DOS_FN_RMDIR
                        || function == DOS_FN_CHDIR || function == DOS_FN_DELETE || function == DOS_FN_FILE_ATTRIBUTES || function == DOS_FN_CREATE_TEMP || function == DOS_FN_CREATE_NEW);
        BYTE drive = DosCurrentDrive(machine);

        if (isPathCall && pathBytes[1] == ':')
            drive = (BYTE)((pathBytes[0] | ASCII_CASE_BIT) - 'a');

        if ((function == DOS_FN_READ || function == DOS_FN_WRITE) && g_DosReadWriteDrive >= 0)
            drive = (BYTE)g_DosReadWriteDrive;

        machine->IsCritPending = 1;
        machine->CritAl = drive;
        machine->CritAh = DosCritInt24Ah((BYTE)function);
        machine->CritCode = (BYTE)((R_AX & BYTE_MASK) - DOS_INT21_HARD_ERROR_FIRST);
    }
    /* #275: A 3Fh/40h HARDWARE ERROR WHERE INT 24h CANNOT BE RAISED IS ANSWERED AS
     * FAIL. Three such places: (1) inside the program's own INT 24h handler -- DOS
     * never nests one, and MS-DOS 4.0's HardErr (CTRLC.ASM, GOT_RIGHT_CODE) answers
     * `AL=3` itself when ERRORMODE is set, i.e. FAIL; (2) a DPMI client's INT 21h,
     * served here in protected mode; (3) the nested real-mode loops (crit_raise_ok
     * clear). FAIL is the answer our default INT 24h handler (`mov al,3 / iret`)
     * gives, and the only one that neither loops (RETRY) nor ends the program
     * (ABORT) nor invents data (IGNORE) behind the caller's back.
     *
     * [CAUTION]: DPMI, BY THE SPEC, IS NOT THIS. DPMI 0.9 has DOS raise INT 24h in REAL mode
     * and the host reflect it to the client's protected-mode INT 24h handler if it
     * installed one (0205h), else run the real-mode vector -- usually COMMAND.COM's
     * "Abort, Retry, Fail?". We serve a PM client's INT 21h host-side, so there is no
     * real-mode DOS call to raise it from; doing it properly needs the 0301h nested-
     * V86 run factored out of the INT 31h switch (main.c) so the host can run the
     * real-mode INT 24h vector, or a PM handler, from inside a PM INT 21h. Open.
     *
     * [CAUTION]: PATH CALLS ARE LEFT AS #34 LEFT THEM (the raw 19-31 code) in these places:
     * changing what a DPMI client -- krnl386 and every Win16 program included --
     * sees for a drive probe on an empty A: is a behaviour change nobody has asked
     * for or measured. 3Fh/40h had no previous answer worth keeping (it was a false
     * success).
     */
    else if ((*guestFlags & 1) && (function == DOS_FN_READ || function == DOS_FN_WRITE)
             && DosCritIsHardwareError((WORD)(R_AX & WORD_MASK)))
    {
        WORD code = (WORD)(R_AX & WORD_MASK);
        SETAX(DosCritFailAx((BYTE)function, (BYTE)(code - DOS_INT21_HARD_ERROR_FIRST)));
        machine->LastError = DOS_ERR_FAIL_I24;
        trace = LogPut(trace, "  INT24 not raised (");
        trace = LogPut(trace, machine->IsCritActive ? "inside the handler" : g_DosInt21IsProtectedMode ? "DPMI client"
                                     : "nested real-mode call");
        trace = LogPut(trace, "): error 0x"); trace = LogHexByte(trace, (UINT)code);
        trace = LogPut(trace, " answered as FAIL -> AX=0x"); trace = LogHex(trace, R_AX & WORD_MASK);
        trace = LogPut(trace, ", 59h=53h\r\n");
    }

    /* s91: KEEP THE PSP's JFT TRUTHFUL (see jft_known). V86 only: in protected mode
     * the flags are not on a V86 stack and a DPMI client's JFT is not ours to show.
     */
    if (!g_DosInt21IsProtectedMode && !(*guestFlags & 1))
    {
        if (function == DOS_FN_CREATE || function == DOS_FN_OPEN || function == DOS_FN_CREATE_TEMP || function == DOS_FN_CREATE_NEW || function == DOS_FN_EXTENDED_OPEN)
            DosJftPut(machine, (UINT)(R_AX & WORD_MASK), DosSftValue(machine, (UINT)(R_AX & WORD_MASK)));
        else if (function == DOS_FN_DUP || function == DOS_FN_DUP2)
        {
            UINT sourceHandle = (UINT)(R_BX & WORD_MASK);
            UINT targetHandle = (function == DOS_FN_DUP) ? (UINT)(R_AX & WORD_MASK) : (UINT)(R_CX & WORD_MASK);
            DosJftPut(machine, targetHandle, DosSftValue(machine, sourceHandle));
        }
        else if (function == DOS_FN_CLOSE)
            DosJftPut(machine, (UINT)(R_BX & WORD_MASK), DOS_PSP_JFT_CLOSED);
    }

    /* AND WHAT WE ANSWERED, WHICH IS THE HALF THAT WAS MISSING:
     * The entry trace above prints the call; it did not print the RESULT, so a
     * run said what the guest asked and never what it was told. That is only
     * half a differential instrument: XP's COMMAND.COM makes 31 calls and then
     * terminates, and "which one came back an error" is the whole question --
     * unanswerable from the inbound line alone.
     *
     * [CAUTION]: Same flag, same AH=0Ah exclusion AND THE SAME CAP, so the pairing stays
     * one-to-one and a reader can line `21:xx/yy` up with the `->` under it. The cap
     * has to be shared: capping only the inbound half would leave a file of orphaned
     * results, which is worse than either.
     */
    if (machine->IsTraceAll && function != DOS_FN_BUFFERED_INPUT && machine->TraceCount <= DOS_TRACE_MAX)
    {
        trace = LogPut(trace, "     -> ax="); trace = LogHexByte(trace, (UINT)((R_AX >> BYTE_SHIFT) & BYTE_MASK));
        trace = LogHexByte(trace, (UINT)(R_AX & BYTE_MASK));
        trace = LogPut(trace, " cf="); trace = LogHexByte(trace, (UINT)(*guestFlags & 1));
        trace = LogPut(trace, "\r\n");
    }

    machine->TraceCursor = trace;
    #undef R_AX
    #undef R_BX
    #undef R_CX
    #undef R_DX
    #undef R_DS
    #undef R_ES
    #undef R_SI
    #undef SETAX
    #undef SET16
    #undef OKCF
    #undef ERRCF
    #undef SETZF
    #undef CLRZF
    #undef OUTC
    #undef AUXPRN_TRAMP
    #undef AUXPRN_V86
    return shouldContinue;
}
