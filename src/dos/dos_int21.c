/* dos_int21.c -- see dos_int21.h. Faithful port of the INT 21h handlers from
 * tools/vdmhost/vdmhost.c; AH=48/49/4A delegate to the shared dos_mcb.h allocator. */
#include "dos_int21.h"
#include "dos_mcb.h"
#include "dos_layout.h"
#include "dos_sysvars.h"  /* #48: AH=1Fh/32h build their DPB with the chain's builder */
#include "dos_fh.h"      /* the handle table's two rules -- allocation + classification */
#include "dos_err.h"      /* AH=59h class/action/locus, measured on the oracle */
#include "log.h"          /* zput / zhex */
#include "dos_ctab.h"     /* CP437 tables dumped from the 6.22 oracle */
#include "dos_auxprn.h"   /* #251: the AUX/PRN driver entries (guest code) */
#include "dos_lfn.h"      /* #210: the long-filename API's pure half */

/* Set when the caller is servicing INT 21h for a client that is still in PROTECTED
   mode (a DPMI client), so CF/ZF go to the live VTIB_EFLAGS instead of a pushed V86
   FLAGS frame that does not exist there. See the pfl assignment below. */
INT g_DosInt21IsProtectedMode = 0;
VOID DosInt21SetProtectedMode(INT isOn) { g_DosInt21IsProtectedMode = isOn ? 1 : 0; }

/* ── THE VDM'S CLOCK (GH #250) -- see dos_clock.h. One per VDM, starts at the host's
     time, moved only by a guest's own set calls. */
DOS_CLOCK_STATE g_DosClock;

VOID DosClockHostNow(PDOS_CLOCK_TIME time)
{
    SYSTEMTIME localTime;
    GetLocalTime(&localTime);
    time->Year = localTime.wYear; time->Month = localTime.wMonth; time->Day = localTime.wDay;
    time->Hour = localTime.wHour; time->Minute = localTime.wMinute; time->Second = localTime.wSecond;
    time->Hundredths = (UINT)(localTime.wMilliseconds / 10); time->DayOfWeek = localTime.wDayOfWeek;
}

VOID DosClockRead(INT64 offset, PDOS_CLOCK_TIME out)
{
    DOS_CLOCK_TIME host;
    DosClockHostNow(&host);
    if (offset == 0) { *out = host; return; }   /* the common case, exactly as before */
    DosClockApplyOffset(&host, offset, out);
}

/* ── GH #262: DOS'S CLOCK FOLLOWS THE TICK COUNT WHEN SOMEONE ELSE SET IT. ─────────
     The host wires g_DosTickTake to the PIT's witness (vdd_pit_tick_take, under the
     PIT's lock); NULL off-VM. DosClockSync is called before DOS's clock is read or
     set -- AH=2Ah/2Bh/2Ch/2Dh and every file stamp -- so a raw store to 0040:006C is
     seen by the next thing that asks DOS the time, as CLOCK$ would see it. With no
     store pending it is one compare under the lock and nothing else. */
INT (*g_DosTickTake)(UINT32 *ticks, UINT32 *wraps, UINT32 *since) = 0;

VOID DosClockFollow(UINT32 ticks, UINT32 wraps, UINT32 since)
{
    DOS_CLOCK_TIME host;
    DosClockHostNow(&host);
    DosClockFollowTicks(&host, &g_DosClock.DosOffset, ticks, wraps, since);
}

VOID DosClockSync(VOID)
{
    UINT32 ticks, wraps, since;
    if (g_DosTickTake && g_DosTickTake(&ticks, &wraps, &since)) DosClockFollow(ticks, wraps, since);
}

/* ── GH #263: A FILE CARRIES DOS'S DATE, NOT THE HOST'S. ─────────────────────────
     File I/O is Win32's and Win32 stamps a write with the machine's clock, so after
     INT 21h AH=2Bh set 1999-06-15 a program's new file still said today. DOS stamps
     a created or written file with ITS clock; ours is host-now + DosOffset. Applied
     at create and after every write, only while a guest has moved the clock
     (DosOffset != 0) -- an untouched VDM never calls this, exactly as before. Local
     time, the inverse of what AH=57h AL=00h reads back. A refusal (a handle without
     FILE_WRITE_ATTRIBUTES) leaves Win32's stamp, which is what there was.
   ► EVERY CREATE AND EVERY WRITE PATH (s92): 3Ch and 40h had it; 5Ah/5Bh, 6Ch's
     create/truncate, the FCB create (16h) and FCB writes (15h/22h/28h), and the
     protected-mode twins in main.c (3Ch/5Bh/40h for a DPMI or Win16 client) did not.
   ⚠ STAMPED AT THE WRITE, NOT AT THE CLOSE. DOS keeps the time in the SFT and writes
     the directory entry at close; the two differ by however long the file stays open
     after its last write -- 2-second resolution in the entry, UNMEASURED against 6.22
     for a file held open across a second boundary. NTFS keeps an explicitly-set write
     time for the rest of the handle's life, so the close does not overwrite it. */
VOID DosStampVdmNow(HANDLE file)
{
    DOS_CLOCK_TIME clock; SYSTEMTIME systemTime; FILETIME localTime, fileTime;
    if (!file || file == INVALID_HANDLE_VALUE) return;
    DosClockSync();                             /* #262: a raw 006C store moves it too */
    if (!g_DosClock.DosOffset) return;
    DosClockRead(g_DosClock.DosOffset, &clock);
    systemTime.wYear = (WORD)clock.Year; systemTime.wMonth = (WORD)clock.Month; systemTime.wDayOfWeek = (WORD)clock.DayOfWeek;
    systemTime.wDay = (WORD)clock.Day;   systemTime.wHour = (WORD)clock.Hour;   systemTime.wMinute = (WORD)clock.Minute;
    systemTime.wSecond = (WORD)clock.Second; systemTime.wMilliseconds = (WORD)(clock.Hundredths * 10u);
    if (SystemTimeToFileTime(&systemTime, &localTime) && LocalFileTimeToFileTime(&localTime, &fileTime))
        SetFileTime(file, NULL, NULL, &fileTime);
}

/* INT 21h AH=53h private sub-functions, indexed by AL. See the handler for how each
   row was measured and why this is a table and not a switch. Defaults = the stock
   ntvdm measurement of 2026-09-25, which the host overrides from cfg\int53.txt.
   ⚠ CHANGING A DEFAULT HERE IS A BEHAVIOUR CHANGE FOR EVERY GUEST -- the knob exists
     so an experiment does not have to be one. */
DOS_INT53_ANSWER g_DosInt53Answers[DOS_INT53_COUNT] = {
    /* AL=00 */ { 0x0005, 0 },   /* documented form, asked with SI=BP=0             */
    /* AL=01 */ { 0x0001, 1 },   /* genuinely unsupported: DOS "invalid function"   */
    /* AL=02 */ { 0x5300, 0 },   /* top of COMMAND.COM's main loop -- CF is the gate */
    /* AL=03 */ { 0x0001, 1 },   /* genuinely unsupported                            */
    /* AL=04 */ { 0x5300, 0 },
    /* AL=05 */ { 0x5301, 0 },   /* ⚠ context-dependent, see the handler            */
    /* AL=06 */ { 0x5300, 0 },
    /* AL=07 */ { 0x5301, 0 },
};

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
    if (function > 0x6C) return 0;
    switch (function) {
    case 0x18: case 0x1D: case 0x1E: case 0x20:   /* internal null functions */
    case 0x61:                                    /* reserved                */
    case 0x6B:                                    /* null function (DOS 5+)  */
        return 0;
    default:
        return 1;
    }
}

/* ── OPEN WITH THE RIGHT TO RE-STAMP IT. (#168) ────────────────────────────────────
     DOS lets AX=5701h set a file's date and time through ANY handle, read-only ones
     included -- it only updates the SFT and writes the entry at close. NT's
     SetFileTime needs FILE_WRITE_ATTRIBUTES on the handle, which GENERIC_READ lacks,
     so on a 3D00h handle it failed and the file kept "now" (p_file int21.5700.stamp:
     6.22 read back 12:34:56 2001-09-17, we read back the wall clock). Ask for it too,
     and fall back to the plain request where it is refused (a read-only medium or
     share) -- the open must never be lost for the sake of the stamp. Attribute
     rights are not subject to sharing, so this changes no share-mode outcome. */
static HANDLE DosOpenStampable(PCSTR fileName, DWORD access, DWORD share, DWORD disposition, DWORD attributes)
{
    HANDLE file = CreateFileA(fileName, access | FILE_WRITE_ATTRIBUTES, share, NULL, disposition, attributes, NULL);
    if (file == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED)
        file = CreateFileA(fileName, access, share, NULL, disposition, attributes, NULL);
    return file;
}

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

/* ---- INT 21h 4Eh/4Fh find-first/find-next.  GH #29. --------------------------
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
#define DOS_FIND_MAGIC 0x4E

static INT DosDtaMatchesAttributes(DWORD attributes, WORD mask)
{
    /* DOS's rule is "normal files always match; these extras only if asked". */
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) && !(mask & 0x10)) return 0;
    if ((attributes & FILE_ATTRIBUTE_HIDDEN)    && !(mask & 0x02)) return 0;
    if ((attributes & FILE_ATTRIBUTE_SYSTEM)    && !(mask & 0x04)) return 0;
    return 1;
}

static VOID DosDtaFill(volatile BYTE *dta, const WIN32_FIND_DATAA *findData)
{
    FILETIME localTime;
    WORD fileDate = 0, fileTime = 0;
    PCSTR name = findData->cAlternateFileName[0] ? findData->cAlternateFileName : findData->cFileName;
    INT index;
    if (FileTimeToLocalFileTime(&findData->ftLastWriteTime, &localTime))
        FileTimeToDosDateTime(&localTime, &fileDate, &fileTime);  /* DOS times are LOCAL */
    dta[21] = (BYTE)(findData->dwFileAttributes & 0x3F);
    dta[22] = (BYTE)(fileTime & 0xFF);  dta[23] = (BYTE)(fileTime >> 8);
    dta[24] = (BYTE)(fileDate & 0xFF);  dta[25] = (BYTE)(fileDate >> 8);
    dta[26] = (BYTE)( findData->nFileSizeLow        & 0xFF);
    dta[27] = (BYTE)((findData->nFileSizeLow >> 8)  & 0xFF);
    dta[28] = (BYTE)((findData->nFileSizeLow >> 16) & 0xFF);
    dta[29] = (BYTE)((findData->nFileSizeLow >> 24) & 0xFF);
    for (index = 0; index < 12 && name[index]; ++index) {
        CHAR character = name[index];
        if (character >= 'a' && character <= 'z') character = (CHAR)(character - 32);  /* DOS reports 8.3 upper */
        dta[30 + index] = (BYTE)character;
    }
    dta[30 + index] = 0;
}

/* ---- The FCB interface (AH=0Fh-24h, 27h-29h).  GH #36. ----------------------
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
#define DOS_FCB_MAGIC 0x46

static volatile BYTE *DosFcbAt(DWORD segment, DWORD offset)
{
    volatile BYTE *fcb = (volatile BYTE *)((segment << 4) + (offset & 0xFFFF));
    return (fcb[0] == 0xFF) ? fcb + 7 : fcb;  /* skip an extended FCB's prefix */
}

/* Build "D:NAME.EXT" from an FCB's drive/name/extension fields. */
static VOID DosFcbName(const volatile BYTE *fcb, PSTR out)
{
    INT index, length = 0;
    if (fcb[0]) { out[length++] = (CHAR)('A' + fcb[0] - 1); out[length++] = ':'; }
    for (index = 1; index <= 8 && fcb[index] != ' '; ++index) out[length++] = (CHAR)fcb[index];
    if (fcb[9] != ' ') {
        out[length++] = '.';
        for (index = 9; index <= 11 && fcb[index] != ' '; ++index) out[length++] = (CHAR)fcb[index];
    }
    out[length] = 0;
}

/* ── A DOS FILENAME ENDS AT A TERMINATOR, NOT ONLY AT A NUL. ─────────────────────
     The set is DOS's own, and we already publish it to guests as the AH=65h AL=05
     "filename terminator" table (dos_ctab.h): every control character and the space
     (0x00-0x20), plus the punctuation that separates a path from what follows.
     '.' is not here -- it is the extension separator and the caller handles it.
   ⚠ WHY THIS IS NOT COSMETIC. A command line is terminated by 0x0D, and the name
     builder below used to copy that CR straight into the FCB. COMMAND.COM matches
     its internal command table by comparing the entry's characters and then checking
     that the NEXT byte of the FCB is blank -- so `ver` parsed to "VER\r    " and
     missed, while `ver ` parsed to "VER \r   " and hit purely because the user had
     typed the blank we should have supplied. That is why every internal command was
     "Bad command or file name" until you put a space after it. */
static INT DosFcbIsNameEnd(BYTE character)
{
    if (character <= 0x20) return 1;            /* NUL, CR, TAB, space, any control */
    return character == '"' || character == '/' || character == '\\' || character == '[' || character == ']' || character == ':'
        || character == '|' || character == '<'  || character == '>'  || character == '+' || character == '=' || character == ';'
        || character == ',';
}

static VOID DosFcbPutName(volatile BYTE *destination, PCSTR name)
{
    INT source = 0, index;
    for (index = 0; index < 11; ++index) destination[index] = ' ';
    /* "." AND ".." ARE NAMES, NOT EXTENSIONS. The rule below ends the name at the
       first '.', which for these two directory entries ends it at character zero and
       leaves eleven blanks -- DIR then printed an empty column where the oracle
       shows "." and "..". DOS stores them literally in the name field. */
    if (name[0] == '.') {
        destination[0] = '.';
        if (name[1] == '.' && (name[2] == 0 || name[2] == '.')) destination[1] = '.';
        if (name[1] == 0 || name[1] == '.') return;
    }
    /* ── ★ `*` IS EXPANDED INTO `?`s, IT IS NOT STORED. ─────────────────────────
         An FCB name field has no room for a star and no meaning for one: the only
         wildcard the format knows is `?`, so DOS fills the rest of the field with
         them as it parses. Oracle-measured (p_fcb.asm int21.29.wild/starstar on
         6.22): "*.BAS" lands as 00 3F×8 'BAS' and "*.*" as 00 3F×11.
       ► THIS IS WHY QBASIC'S OPEN DIALOG LISTED NO FILES. It parses the pattern
         with AH=29h and then matches each directory entry against the parsed FCB.
         We stored `*` literally, so the template read `*` + seven blanks, nothing
         matched it, and the file pane came up empty -- while the directory pane
         beside it, which is not pattern-filtered, was perfectly correct. The
         symptom pointed at the search, the directory entries and the renderer; the
         cause was in the parser none of them go through. */
    for (index = 0; index < 8 && !DosFcbIsNameEnd((BYTE)name[source]) && name[source] != '.'; ++index, ++source) {
        if (name[source] == '*') { while (index < 8) destination[index++] = '?'; break; }
        destination[index] = (BYTE)(name[source] >= 'a' && name[source] <= 'z' ? name[source] - 32 : name[source]);
    }
    while (!DosFcbIsNameEnd((BYTE)name[source]) && name[source] != '.') ++source;
    if (name[source] == '.') ++source;
    for (index = 8; index < 11 && !DosFcbIsNameEnd((BYTE)name[source]); ++index, ++source) {
        if (name[source] == '*') { while (index < 11) destination[index++] = '?'; break; }
        destination[index] = (BYTE)(name[source] >= 'a' && name[source] <= 'z' ? name[source] - 32 : name[source]);
    }
}

/* ── ★ A DOS SEARCH MATCHES THE 8.3 NAME AGAINST AN 11-BYTE TEMPLATE. (s81 sweep) ──
     We handed DOS patterns straight to FindFirstFileA, which matches them against the
     LONG name. XP's COMMAND.COM lists a directory with an FCB search (AH=11h/12h) on
     `????????.???`, and `ntvdmhost.exe` -- nine characters before the dot -- does not
     fit that as a long name, so the user's `dir` in bin\ printed only `.` and `..`.
     Real NTVDM matches the SHORT name (NTVDMH~1.EXE), which is the only name DOS has.
   ► So enumerate the directory with `*` and decide each entry DOS's way: its 8.3 name
     (the short alias, or the long name when that is already a legal 8.3 name -- and
     no name at all otherwise: such a file is invisible to DOS, as it is on NTVDM),
     laid out as 11 bytes, matched position by position, `?` matching anything. */
static INT DosShortNameOf(const WIN32_FIND_DATAA *findData, BYTE out[11])
{
    PCSTR baseName = findData->cAlternateFileName[0] ? findData->cAlternateFileName : findData->cFileName;
    if (!findData->cAlternateFileName[0] && baseName[0] != '.') {  /* the long name must BE 8.3 */
        INT baseLength = 0, extensionLength = -1, index;
        for (index = 0; baseName[index]; ++index) {
            if (baseName[index] == '.') { if (extensionLength >= 0) return 0; extensionLength = 0; continue; }
            if (baseName[index] == ' ' || DosFcbIsNameEnd((BYTE)baseName[index])) return 0;
            if (extensionLength >= 0) { if (++extensionLength > 3) return 0; } else if (++baseLength > 8) return 0;
        }
        if (!baseLength) return 0;
    }
    DosFcbPutName((volatile BYTE *)out, baseName);
    return 1;
}
static INT DosTemplateMatches(const BYTE nameTemplate[11], const BYTE name[11])
{
    INT index;
    for (index = 0; index < 11; ++index) {
        BYTE character = nameTemplate[index];
        if (character == '?') continue;
        if (character >= 'a' && character <= 'z') character = (BYTE)(character - 32);
        if (character != name[index]) return 0;
    }
    return 1;
}
static INT DosFindMatches(const WIN32_FIND_DATAA *findData, const BYTE nameTemplate[11], WORD mask)
{
    BYTE name[11];
    return DosDtaMatchesAttributes(findData->dwFileAttributes, mask) && DosShortNameOf(findData, name) && DosTemplateMatches(nameTemplate, name);
}
/* Split a host path pattern into "directory\*" (for FindFirstFileA) and the final
   component's 11-byte template. */
static VOID DosFindSplit(PCSTR pattern, PSTR directoryPattern, INT directoryPatternSize, BYTE nameTemplate[11])
{
    INT index, cut = 0;
    for (index = 0; pattern[index]; ++index) if (pattern[index] == '\\' || pattern[index] == '/' || pattern[index] == ':') cut = index + 1;
    for (index = 0; index < cut && index < directoryPatternSize - 2; ++index) directoryPattern[index] = pattern[index];
    directoryPattern[index++] = '*'; directoryPattern[index] = 0;
    DosFcbPutName((volatile BYTE *)nameTemplate, pattern + cut);
}
/* FindFirstFileA + skip to the first DOS match; INVALID_HANDLE_VALUE if none (the
   handle is closed then, and *nodir says whether the DIRECTORY itself was missing). */
/* #34: the Win32 error of the last FindFirstFileA that failed outright, so a drive
   that is NOT READY (21) can be told from "no such file" -- the first is a critical
   error and goes to INT 24h, the second is an ordinary answer. 0 = it did not fail. */
static DWORD g_DosFindWin32Error;
static HANDLE DosFindFirst(PCSTR directoryPattern, const BYTE nameTemplate[11], WORD mask,
                             WIN32_FIND_DATAA *findData, PINT isNoDirectory)
{
    HANDLE find = FindFirstFileA(directoryPattern, findData);
    *isNoDirectory = 0;
    g_DosFindWin32Error = 0;
    if (find == INVALID_HANDLE_VALUE) {
        g_DosFindWin32Error = GetLastError();
        *isNoDirectory = (g_DosFindWin32Error == ERROR_PATH_NOT_FOUND);
        return find;
    }
    while (!DosFindMatches(findData, nameTemplate, mask))
        if (!FindNextFileA(find, findData)) { FindClose(find); return INVALID_HANDLE_VALUE; }
    return find;
}
/* ── #275: WHICH DRIVE AN OPEN FILE IS ON, for INT 24h's AL on a 3Fh/40h failure. ──
     We keep no SFT, so the handle has to be asked. XP has no GetFinalPathNameByHandle;
     NtQueryObject(ObjectNameInformation) gives the file object's NT name
     ("\Device\Floppy0\X.TXT") and QueryDosDeviceA("A:") the drive's NT device, which
     works with no media in the drive -- the very case this is for. The match is the
     pure DosCritDriveFromNtName (dos_err.h, off-VM tested).
   ⚠ Only ever called on a DISK file whose ReadFile/WriteFile just failed with a
     hardware error -- never on a pipe, where a name query can block.
   -1 = could not tell (the caller keeps the current drive, as #34 did). */
typedef struct { USHORT Length, MaximumLength; PWSTR Buffer; } DOS_UNICODE_STRING;
typedef LONG (WINAPI *DOS_NT_QUERY_OBJECT)(HANDLE, INT, PVOID, ULONG, PULONG);
static INT DosHandleDrive(HANDLE file)
{
    static DOS_NT_QUERY_OBJECT queryObject;
    union { DOS_UNICODE_STRING String; BYTE Raw[1024]; } objectName;
    CHAR name[600], deviceBuffers[26][80];
    PCSTR devices[26];
    ULONG returned = 0;
    DWORD drives = GetLogicalDrives();
    INT drive, length;
    if (!queryObject) {
        HMODULE ntdll = GetModuleHandleA("ntdll.dll");
        if (ntdll) queryObject = (DOS_NT_QUERY_OBJECT)GetProcAddress(ntdll, "NtQueryObject");
        if (!queryObject) return -1;
    }
    if (queryObject(file, 1 /* ObjectNameInformation */, &objectName, sizeof(objectName) - 2, &returned) < 0
        || !objectName.String.Buffer || !objectName.String.Length) return -1;
    length = WideCharToMultiByte(CP_ACP, 0, objectName.String.Buffer, objectName.String.Length / 2, name, sizeof(name) - 1, NULL, NULL);
    if (length <= 0) return -1;
    name[length] = 0;
    for (drive = 0; drive < 26; ++drive) {
        CHAR root[3] = { (CHAR)('A' + drive), ':', 0 };
        devices[drive] = NULL;
        if (!(drives & (1u << drive))) continue;
        if (QueryDosDeviceA(root, deviceBuffers[drive], sizeof(deviceBuffers[drive]))) devices[drive] = deviceBuffers[drive];
    }
    return DosCritDriveFromNtName(name, devices);
}
/* The drive DosHandleDrive found for THIS call's failed 3Fh/40h; -1 = none. Read by
   the INT 24h tail of DosInt21, reset at its entry (the g_DosFindWin32Error pattern). */
static INT g_DosReadWriteDrive = -1;

static INT DosFindNext(HANDLE find, const BYTE nameTemplate[11], WORD mask, WIN32_FIND_DATAA *findData)
{
    do { if (!FindNextFileA(find, findData)) return 0; } while (!DosFindMatches(findData, nameTemplate, mask));
    return 1;
}

/* Copy an ASCIIZ string out of V86 memory (seg:off) into a host buffer. */
static VOID DosGuestString(DWORD segment, DWORD offset, PSTR destination, INT capacity)
{
    const volatile BYTE *source = (const volatile BYTE *)((segment << 4) + (offset & 0xFFFF));
    INT index;
    for (index = 0; index < capacity - 1 && source[index]; ++index) destination[index] = (CHAR)source[index];
    destination[index] = 0;
}

/* The current drive, 0 = A:. The process current directory's, unless AH=0Eh
   selected a drive Win32 could not enter (m->vdrive, see the header). */
static BYTE DosCurrentDrive(PCDOS_MACHINE machine)
{
    CHAR directory[300];
    DWORD length;
    if (machine->VirtualDrive >= 0) return (BYTE)machine->VirtualDrive;
    length = GetCurrentDirectoryA(sizeof(directory), directory);
    return (length >= 2 && directory[1] == ':') ? (BYTE)((directory[0] | 0x20) - 'a') : DOS_CURRENT_DRIVE;
}

/* See the header: the host needs this for the NTVDM BOP 0x54 sub 01 reply, and must
   not re-derive it -- a second copy of the rule would drop `vdrive`. */
BYTE DosInt21CurrentDrive(PCDOS_MACHINE machine) { return DosCurrentDrive(machine); }

/* #165: 6901h's serial, label and file-system type, per drive, for this session only
   (see the 6901h arm). 23 bytes = the 6900h/6901h block from offset 2. */
static BYTE g_DosSerialIsSet[26];
static BYTE g_DosSerialInfo[26][23];
/* BL as 69h takes it: 0 = the default drive, 1 = A:, ... -> 0-based, 26 if invalid. */
static BYTE DosSerialDrive(PCDOS_MACHINE machine, BYTE driveNumber)
{
    if (driveNumber == 0) return DosCurrentDrive(machine);
    return (BYTE)(driveNumber <= 26 ? driveNumber - 1 : 26);
}

/* Keep Win32's per-drive current directory in step. GetFullPathNameA("X:") and
   SetCurrentDirectoryA("X:") read the hidden `=X:` environment variable, which
   cmd.exe and the CRT maintain and SetCurrentDirectoryA itself does NOT -- so
   without this, C: -> D: -> C: came back to C:'s ROOT, where DOS returns to the
   directory it left. The guest never sees the process environment (its block is
   built separately), so these variables cost it nothing. */
static VOID DosNoteDriveDirectory(PCSTR fullPath)
{
    CHAR variable[5];
    if (!fullPath || !fullPath[0] || fullPath[1] != ':') return;
    variable[0] = '='; variable[1] = (CHAR)(fullPath[0] & ~0x20); variable[2] = ':'; variable[3] = 0;
    SetEnvironmentVariableA(variable, fullPath);
}

/* A guest path, as Win32 should see it: DosGuestString, then -- only while the current
   drive is one Win32 cannot stand on -- a relative path is prefixed with that
   drive so it resolves (and fails) THERE. "X:..." and "\\server" are left alone.
   A device name survives the prefix: Win32 reads "A:CON" as CON, as DOS does. */
static VOID DosGuestPath(PCDOS_MACHINE machine, DWORD segment, DWORD offset, PSTR destination, INT capacity)
{
    CHAR guestPath[300];
    INT length = 0, index;
    DosGuestString(segment, offset, guestPath, sizeof(guestPath));
    if (machine->VirtualDrive >= 0 && guestPath[0] && guestPath[1] != ':' && !(guestPath[0] == '\\' && guestPath[1] == '\\')
        && capacity > 3) {
        destination[length++] = (CHAR)('A' + machine->VirtualDrive); destination[length++] = ':';
    }
    for (index = 0; guestPath[index] && length < capacity - 1; ++index) destination[length++] = guestPath[index];
    destination[length] = 0;
}

VOID DosInt21Initialize(PDOS_MACHINE machine, WORD firstMcb)
{
    INT index;
    for (index = 0; index < DOS_MAX_FILES; ++index) machine->FileHandles[index] = 0;
    for (index = 0; index < 8; ++index) machine->FindHandles[index] = 0;
    machine->LastError = 0;
    machine->IsVerifyOn = 0;
    machine->ChildReturnCode = 0;
    machine->FcbFind = 0;
    machine->SwitchChar = '/';  /* oracle-confirmed 6.22 default */
    machine->HandleDepth = 0;  /* no EXEC in progress: nothing saved */
    machine->SetTicks = 0; machine->TicksContext = 0;  /* the host wires these after init (#250) */
    machine->IsCritPending = 0; machine->IsCritActive = 0; machine->TermType = 0;  /* #34 */
    machine->CanRaiseCrit = 0;                                    /* #275 */
    { INT index2; for (index2 = 0; index2 < DOS_SHELL_PSP_SLOTS; ++index2) machine->ShellPsps[index2] = 0; }
    machine->ShellVersionMajor = 5; machine->ShellVersionMinor = 0;  /* what XP's COMMAND.COM demands */
    machine->IsBreakOn = 0;  /* BREAK=OFF, DOS's default. ⚠ m is a stack local and this
                               function sets fields one by one -- nothing zeroes it */
    machine->VirtualDrive = -1;  /* the current drive is the process current directory's */
    machine->PspSegment = DOS_PSP_SEG;
    {   INT index2;          /* s91: the JFT a fresh PSP carries (DosPspBuild) */
        static const BYTE initialJft[5] = { 1, 1, 1, 0, 2 };
        for (index2 = 0; index2 < 20; ++index2) machine->JftKnown[index2] = index2 < 5 ? initialJft[index2] : 0xFF;
        for (index2 = 0; index2 < 256; ++index2) machine->SftHost[index2] = 0; }
    machine->IsExecPending = 0;
    machine->IsTsrPending = 0; machine->TsrKeep = 0;
    machine->FirstMcb = firstMcb;
    machine->DtaSegment = DOS_PSP_SEG;
    machine->DtaOffset = 0x0080;
    machine->OutputLength = 0; machine->IsOutputTruncated = 0;
    machine->IsLineActive = 0; machine->LineLength = 0; machine->LineSegment = 0; machine->LineOffset = 0;
    machine->TraceCount = 0;
    machine->StdOpen = 0x1F;                /* stdin/stdout/stderr/aux/prn all open */
    { INT index3; for (index3 = 0; index3 < 32; ++index3) { machine->Unimplemented[index3] = 0; machine->Undefined[index3] = 0; } }
    machine->ExitCode = 0;
    /* GH #28: default to 6.22 so we match the oracle. It is also the friendlier
       lie -- most version checks are floor checks, and real 6.22 tools refuse to
       run at all under a lower number ("Incorrect DOS version" from MEM.EXE was
       the first thing the evidence pass hit). */
    machine->VersionMajor = 6; machine->VersionMinor = 22;
    /* Oracle-confirmed 6.22 defaults: 5800h -> AX=0000 (first fit),
       5802h -> AL=00 (UMBs not linked). */
    machine->AllocationStrategy = 0; machine->UmbLink = 0;
    machine->SysvarsSegment = 0; machine->SysvarsOffset = 0;
    machine->ConsoleOut = 0;
    machine->ConsoleOutContext = 0;
    machine->ConsoleIn = 0;
    machine->ConsoleInContext = 0;
    machine->ConsoleInNoWait = 0;
    machine->ConsolePeek = 0;
}

/* See the header. The comment at AH=30h has promised this function since GH #28;
   COMMAND.COM is what finally needed it. */
/* ── PER-PROCESS HANDLE TABLES. (s81) See DOS_MACHINE::hsave for why. ────────── */
/* Does any SAVED (i.e. parent's) table still hold this Win32 handle? Then a child
   closing or overwriting it must not CloseHandle it -- the parent gets it back. */
static INT DosHandleIsHeldByParent(PCDOS_MACHINE machine, HANDLE handle)
{
    INT depth, index;
    if (!handle) return 0;
    for (depth = 0; depth < machine->HandleDepth && depth < DOS_HANDLE_STACK_DEPTH; ++depth)
        for (index = 0; index < DOS_MAX_FILES; ++index)
            if (machine->HandleStack[depth].FileHandles[index] == handle) return 1;
    return 0;
}

/* Take a Win32 handle out of the current table: closed for real only if no parent
   still holds it. Every site that used to CloseHandle(m->fh[x]) comes through here. */
VOID DosHandleRelease(PDOS_MACHINE machine, UINT slot)
{
    if (slot >= DOS_MAX_FILES || !machine->FileHandles[slot]) return;
    if (!DosHandleIsHeldByParent(machine, machine->FileHandles[slot])) CloseHandle(machine->FileHandles[slot]);
    machine->FileHandles[slot] = 0;
}

VOID DosHandlesPush(PDOS_MACHINE machine)
{
    INT index;
    if (machine->HandleDepth >= DOS_HANDLE_STACK_DEPTH) { ++machine->HandleDepth; return; }  /* too deep: counted, not saved */
    for (index = 0; index < DOS_MAX_FILES; ++index) machine->HandleStack[machine->HandleDepth].FileHandles[index] = machine->FileHandles[index];
    machine->HandleStack[machine->HandleDepth].StdOpen = machine->StdOpen;
    for (index = 0; index < 20; ++index) machine->HandleStack[machine->HandleDepth].JftKnown[index] = machine->JftKnown[index];
    ++machine->HandleDepth;
}

/* ── s91: THE JFT (see jft_known in dos_int21.h). ─────────────────────────────────── */
static volatile BYTE *DosJftOf(WORD psp, UINT *count)
{
    volatile BYTE *pspBytes = (volatile BYTE *)(ULONG_PTR)((DWORD)psp << 4);
    UINT jftSize, jftOffset, jftSegment;
    *count = 0;
    if (!psp) return NULL;
    jftSize = (UINT)(pspBytes[0x32] | (pspBytes[0x33] << 8));
    jftOffset = (UINT)(pspBytes[0x34] | (pspBytes[0x35] << 8));
    jftSegment = (UINT)(pspBytes[0x36] | (pspBytes[0x37] << 8));
    if (!jftSegment || !jftSize) return NULL;
    *count = jftSize > 20 ? 20 : jftSize;
    return (volatile BYTE *)(ULONG_PTR)(((DWORD)jftSegment << 4) + jftOffset);
}
/* The pseudo SFT index for what handle h is bound to now. */
static BYTE DosSftValue(PDOS_MACHINE machine, UINT handle)
{
    UINT value, freeValue = 0;
    if (handle < DOS_MAX_FILES && machine->FileHandles[handle]) {
        for (value = 3; value < 255; ++value) {
            if (machine->SftHost[value] == machine->FileHandles[handle]) return (BYTE)value;
            if (!machine->SftHost[value] && !freeValue) freeValue = value;
        }
        if (!freeValue) {                   /* table full: forget the stale entries */
            for (value = 3; value < 255; ++value) machine->SftHost[value] = 0;
            freeValue = 3;
        }
        machine->SftHost[freeValue] = machine->FileHandles[handle];
        return (BYTE)freeValue;
    }
    if (handle < 32 && (machine->StdOpen & (1u << handle))) return (BYTE)(handle == 3 ? 0 : handle == 4 ? 2 : 1);
    return 0xFF;
}
static VOID DosJftPut(PDOS_MACHINE machine, UINT handle, BYTE value)
{
    UINT count;
    volatile BYTE *jft = DosJftOf(machine->PspSegment, &count);
    if (jft && handle < count) { jft[handle] = value; machine->JftKnown[handle] = value; }
}
VOID DosJftReset(PDOS_MACHINE machine)
{
    UINT count, handle;
    volatile BYTE *jft = DosJftOf(machine->PspSegment, &count);
    for (handle = 0; handle < 20; ++handle) machine->JftKnown[handle] = (jft && handle < count) ? jft[handle] : 0xFF;
}
VOID DosJftExec(PDOS_MACHINE machine, WORD childPsp)
{
    UINT count, childCount, handle;
    volatile BYTE *jft = DosJftOf(machine->PspSegment, &count), *childJft = DosJftOf(childPsp, &childCount);
    if (!jft) return;
    /* Only the five STANDARD handles: shell redirection is all this is for, and the
       final s91 regression run showed a Win16 task's file create coming back as handle
       18h instead of 6 once higher slots were re-bound from a JFT we do not own. */
    for (handle = 0; handle < count && handle < 5; ++handle) {
        BYTE value = jft[handle];
        if (value == machine->JftKnown[handle]) continue;  /* ours: fh[] already says so */
        if (value == 0xFF) {
            machine->FileHandles[handle] = 0;
            if (handle < 32) machine->StdOpen &= ~(1u << handle);
        } else if (value <= 2) {
            machine->FileHandles[handle] = 0;
            if (handle < 32) machine->StdOpen |= (1u << handle);
        } else if (machine->SftHost[value]) {
            machine->FileHandles[handle] = machine->SftHost[value];
        }
    }
    for (handle = 0; handle < count && childJft && handle < childCount; ++handle) childJft[handle] = jft[handle];  /* DOS copies the JFT */
    for (handle = 0; handle < 20; ++handle) machine->JftKnown[handle] = (childJft && handle < childCount) ? childJft[handle] : 0xFF;
}

VOID DosHandlesPop(PDOS_MACHINE machine, INT isTsr)
{
    INT index, depth;
    if (machine->HandleDepth <= 0) return;
    depth = --machine->HandleDepth;
    if (depth >= DOS_HANDLE_STACK_DEPTH) return;             /* matched an unsaved push */
    /* What the child still has open and the parent never had: DOS closes those at
       terminate. Checked against the parent table being restored, and against every
       older one, so nothing a caller further up holds is touched. A TSR keeps its. */
    if (!isTsr)
        for (index = 0; index < DOS_MAX_FILES; ++index) {
            HANDLE handle = machine->FileHandles[index];
            INT index2, isDuplicate = 0;
            if (!handle || DosHandleIsHeldByParent(machine, handle)) continue;
            for (index2 = 0; index2 < DOS_MAX_FILES; ++index2)
                if (machine->HandleStack[depth].FileHandles[index2] == handle) { isDuplicate = 1; break; }
            for (index2 = 0; index2 < index && !isDuplicate; ++index2) if (machine->FileHandles[index2] == handle) isDuplicate = 1;  /* closed already */
            if (!isDuplicate) CloseHandle(handle);
        }
    for (index = 0; index < DOS_MAX_FILES; ++index) machine->FileHandles[index] = machine->HandleStack[depth].FileHandles[index];
    machine->StdOpen = machine->HandleStack[depth].StdOpen;
    for (index = 0; index < 20; ++index) machine->JftKnown[index] = machine->HandleStack[depth].JftKnown[index];
}

VOID DosInt21SetShellPsp(PDOS_MACHINE machine, WORD psp, INT isOn)
{
    INT index;
    for (index = 0; index < DOS_SHELL_PSP_SLOTS; ++index) if (machine->ShellPsps[index] == psp) machine->ShellPsps[index] = 0;
    if (isOn) for (index = 0; index < DOS_SHELL_PSP_SLOTS; ++index) if (!machine->ShellPsps[index]) { machine->ShellPsps[index] = psp; break; }
}

/* The version THIS process is told -- see DOS_MACHINE::v5_psp. */
static WORD DosVersionWord(PCDOS_MACHINE machine)
{
    INT index;
    for (index = 0; index < DOS_SHELL_PSP_SLOTS; ++index)
        if (machine->ShellPsps[index] && machine->ShellPsps[index] == machine->PspSegment)
            return (WORD)((machine->ShellVersionMinor << 8) | machine->ShellVersionMajor);
    return (WORD)((machine->VersionMinor << 8) | machine->VersionMajor);
}

VOID DosInt21SetVersion(PDOS_MACHINE machine, BYTE major, BYTE minor)
{
    if (!machine || !major) return;             /* major 0 is not a DOS version */
    machine->VersionMajor = major; machine->VersionMinor = minor;
}

/* ── #210: THE LONG-FILENAME API'S WIN32 HALF -- helpers for the AH=71h arm. ─────────
     The pure half (time conversion, the find record, short names, the action word) is
     dos_lfn.h; these only fetch what Win32 knows and hand it over.

   ► LONG NAMES GO THROUGH THE SAME ...A FILE APIs AS EVERY OTHER DOS CALL HERE, i.e. the
     ANSI code page: this host never calls SetFileApisToOEM, so 3Dh, 4Eh and 71xxh all
     agree with each other about what a byte above 7Fh names. Stock NTVDM converts DOS
     names with the OEM code page (its file APIs are set to OEM -- from the NT design, NOT
     measured here). For ASCII the two are identical, and on the rig (CP437 OEM / 1252
     ANSI) they differ only above 7Fh -- a non-ASCII long name is the place to look if a
     program and stock disagree about one.

   ► THE SEARCH TABLE. 714Eh hands the caller a HANDLE (AX) that it passes back to 714Fh
     and closes with 71A1h -- unlike 4Eh, whose state rides in the DTA. Kept here, one
     per VDM like g_DosSerialInfo; AX = slot + 1 so a handle is never 0.
   ⚠ A program that never calls 71A1h leaks its slot until the table wraps: the 17th
     live search recycles the oldest (round robin), as 4Eh recycles its eighth. Windows
     95 closes them when the PSP terminates; we have no per-PSP owner record yet.
   ⚠ The handle VALUE is ours. Stock's numbering is not measured and p_lfn does not
     compare AX on 714Eh -- a program that treats the handle as opaque cannot tell. */
#define DOS_LFN_FIND_SLOTS 16
static HANDLE  g_DosLfnFinds[DOS_LFN_FIND_SLOTS];
static BYTE g_DosLfnAllow[DOS_LFN_FIND_SLOTS], g_DosLfnNeed[DOS_LFN_FIND_SLOTS];
static UINT g_DosLfnNext;

static UINT64 DosFileTime64(const FILETIME *fileTime)
{
    return ((UINT64)fileTime->dwHighDateTime << 32) | fileTime->dwLowDateTime;
}

/* Local time for a DOS-format answer (SI=1), as every DOS time this host reports is
   local (DosDtaFill, 5700h). A FILETIME answer (SI=0) is Win32's own, i.e. UTC. */
static UINT64 DosFileTimeZoned(const FILETIME *fileTime, INT isLocal)
{
    FILETIME localTime;
    if (isLocal && (fileTime->dwLowDateTime || fileTime->dwHighDateTime) && FileTimeToLocalFileTime(fileTime, &localTime))
        return DosFileTime64(&localTime);
    return DosFileTime64(fileTime);
}

static VOID DosLfnFindFill(volatile BYTE *destination, const WIN32_FIND_DATAA *findData, INT isDosFormat)
{
    BYTE record[DOS_LFN_FIND_RECORD_SIZE];
    DOS_LFN_FIND_ENTRY entry;
    INT index;
    entry.Attributes = findData->dwFileAttributes;
    entry.CreationTime = DosFileTimeZoned(&findData->ftCreationTime, isDosFormat);
    entry.LastAccessTime = DosFileTimeZoned(&findData->ftLastAccessTime, isDosFormat);
    entry.LastWriteTime = DosFileTimeZoned(&findData->ftLastWriteTime, isDosFormat);
    entry.SizeHigh = findData->nFileSizeHigh; entry.SizeLow = findData->nFileSizeLow;
    entry.LongName = findData->cFileName; entry.ShortName = findData->cAlternateFileName;
    DosLfnFindPack(record, &entry, isDosFormat);
    for (index = 0; index < DOS_LFN_FIND_RECORD_SIZE; ++index) destination[index] = record[index];
}

/* The DOS error for a failed LFN call, from the Win32 one (dos_lfn.h). */
static WORD DosLfnError(DWORD win32Error)
{
    WORD dosError = 2;
    (VOID)DosLfnErrFromWin32((unsigned long)win32Error, &dosError);
    return dosError;
}

/* Open a file OR a directory just to set its times (7143h BL=3/5/7). Directories need
   FILE_FLAG_BACKUP_SEMANTICS; sharing is everything, as 3Dh's is. */
static HANDLE DosLfnOpenAttributes(PCSTR fileName)
{
    return CreateFileA(fileName, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
}

/* ── THE CALL SITE, AND THE CODE AROUND IT. ──────────────────────────────────────
     Used on the terminate paths, where "an address says WHERE a guest gave up, never
     WHY" -- the standing SILENT VDM DEATH -> GET THE BYTES rule -- and the bytes only
     answer that if they are the GUEST's, at the address the guest actually called
     from. `seg:off` here is the pushed return address, so the `CD 21` that got us
     here begins two bytes earlier; that is the site, and the branch that chose it is
     upstream of it.
   ⚠ 24 bytes before the site and 8 after. Disassemble with `ndisasm -b 16 -o <addr>`.
   ⚠ Clamped at the segment base: a low return offset must not read below zero. */
static PSTR DosInt21CallSite(PSTR trace, INT isFramed, DWORD segment, DWORD offset)
{
    DWORD base, site, low, count, index;
    const volatile BYTE *bytes;
    if (!isFramed) return zput(trace, " from=<PM: no pushed frame>");
    trace = zput(trace, " from=0x"); trace = zhex(trace, segment);
    trace = zput(trace, ":0x");      trace = zhex(trace, offset);
    site = (offset >= 2) ? offset - 2 : 0;      /* the CD 21 itself */
    trace = zput(trace, " site=0x");  trace = zhex(trace, site);
    base = (segment & 0xFFFF) << 4;
    low   = (site >= 24) ? site - 24 : 0;
    bytes    = (const volatile BYTE *)(ULONG_PTR)(base + low);
    count    = (site - low) + 10;
    trace = zput(trace, " bytes@0x"); trace = zhex(trace, low); trace = zput(trace, "=");
    for (index = 0; index < count && index < 48; ++index) { trace = zhexb(trace, bytes[index]); trace = zput(trace, " "); }
    return trace;
}

INT DosInt21(PDOS_MACHINE machine)
{
    volatile BYTE *tib = machine->Tib;
    PSTR trace = machine->TraceCursor;
    volatile WORD *guestFlags;
    DWORD function;
    INT shouldContinue = 1;
    DWORD callSegment = 0, callOffset = 0;      /* the guest's own call site -- below */
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
    #define SETAX(value)        (R_AX = (R_AX & 0xFFFF0000u) | ((DWORD)(value) & 0xFFFF))
    #define SET16(reg, value)   ((reg)  = ((reg)  & 0xFFFF0000u) | ((DWORD)(value) & 0xFFFF))
    #define OKCF()      (*guestFlags &= (WORD)~1)
    #define ERRCF()     (*guestFlags |= 1)
    #define SETZF()     (*guestFlags |= 0x40)
    #define CLRZF()     (*guestFlags &= (WORD)~0x40)
    /* Dropping output silently once cost a wrong conclusion: a probe's dump was
       cut mid-line, the harness saw fewer results than it asked for, and the
       missing rows read as agreement. Record the drop so the log can say so. */
    /* ── AH=02h/09h WRITE TO STANDARD OUTPUT, NOT TO THE SCREEN. ─────────────────
         That distinction is invisible until something redirects, and then it is the
         whole feature: ECHO does not use AH=40h, it prints with AH=02h, so a shell
         doing `echo hello > hi.txt` sends the text through here. With this macro
         hard-wired to the console sink the redirect was accepted, the file was
         created, the text went to the SCREEN, and hi.txt was left empty at 0 bytes.
         Fixing AH=40h alone did not move it -- measured, twice -- because ECHO never
         goes near AH=40h.
         A bound handle 1 is a file (see the note there); an unbound one is the
         console, which is the ordinary case and behaves exactly as before. */
    #define OUTC(outputValue)     do { BYTE outputChar = (BYTE)(outputValue); \
        if (machine->FileHandles[1]) { DWORD outputWritten = 0; WriteFile(machine->FileHandles[1], &outputChar, 1, &outputWritten, NULL); } \
        else { \
            if (machine->OutputLength < machine->OutputCapacity - 1) machine->Output[machine->OutputLength++] = (CHAR)outputChar; \
            else machine->IsOutputTruncated = 1; \
            if (machine->ConsoleOut) machine->ConsoleOut(machine->ConsoleOutContext, outputChar); \
        } } while (0)

    /* CF is returned via the FLAGS the INT pushed on the V86 stack (SS:SP+4): the
       handler's IRET restores FLAGS from there, so the live EFlags get clobbered.
       ► NOT IN PROTECTED MODE. A DPMI client's INT 21h is serviced by the host with the
         guest still in PM, where SS holds a SELECTOR -- so `SS<<4` is not the stack at
         all (0x1f -> linear 0x1f0) and this would both fail to return CF and scribble
         on low memory. There is no pushed-FLAGS frame to honour there either: the PM
         dispatcher resumes the client by advancing EIP past the BOP, so the live
         VTIB_EFLAGS *is* what the client sees. Point at its low word, which carries
         CF/ZF. Set by the PM caller via DosInt21SetProtectedMode(). */
    guestFlags = g_DosInt21IsProtectedMode
        ? (volatile WORD *)(tib + VTIB_EFLAGS)
        : (volatile WORD *)(((VDM_REG(tib, VTIB_SS) & 0xFFFF) << 4)
                            + (((VDM_REG(tib, VTIB_ESP) & 0xFFFF) + 4) & 0xFFFF));
    function = (R_AX >> 8) & 0xFF;
    machine->Trampoline = 0;
    /* #251: resume the V86 guest in the AUX/PRN driver code -- see dos_auxprn.asm. */
    #define AUXPRN_TRAMP(entry) (machine->Trampoline = (WORD)(DOS_AUXPRN_OFF + (entry)))
    #define AUXPRN_V86      (machine->CanTrampoline && !g_DosInt21IsProtectedMode)
    machine->IsCritPending = 0;  /* #34: only ever about THIS call; see the tail */
    g_DosReadWriteDrive = -1;  /* #275: likewise */

    /* ── ★★ WHO CALLED, OFF THE GUEST STACK. ───────────────────────────────────
         VTIB_CS:EIP is where the HANDLER is, not where the guest is. Last session
         read a terminate site out of it, found `C4 C4 54` there, and built two
         readings on top of a byte dump of the wrong address -- both since
         retracted. A real-mode INT pushes IP, CS, FLAGS, so the true call site is
         at SS:SP: offset first, then segment. The same three words are already
         trusted four lines up, where CF is returned via SS:SP+4 -- so this reads
         the frame the code is ALREADY relying on, and costs one 32-bit load.
         The technique is not new here either; the XMS entry logger's note says
         "LOG WHO CALLED, NOT JUST WHAT THEY ASKED".
       ⚠ REAL MODE ONLY. In PM the dispatcher advances EIP past the BOP -- there is
         no pushed frame, and SS is a selector, so SS<<4 is not the stack at all
         (the same trap the pfl note above exists to warn about). */
    if (!g_DosInt21IsProtectedMode) {
        DWORD stackBase = (VDM_REG(tib, VTIB_SS) & 0xFFFF) << 4;
        DWORD stackPointer = VDM_REG(tib, VTIB_ESP) & 0xFFFF;
        const volatile BYTE *frame = (const volatile BYTE *)(ULONG_PTR)(stackBase + stackPointer);
        callOffset = (DWORD)frame[0] | ((DWORD)frame[1] << 8);
        callSegment = (DWORD)frame[2] | ((DWORD)frame[3] << 8);
        isCallFramed  = 1;
    }

    /* ── EVERY CALL, WHEN ASKED. ────────────────────────────────────────────────
         Most handlers here trace only what they think is interesting, which is fine
         until the question is "what does the guest do BETWEEN two calls we can see".
         COMMAND.COM accepts `ver ` and rejects `ver`, with a provably identical line
         buffer apart from one space -- so the answer is in the calls it makes after
         reading the line, and those are exactly the ones nothing prints. Two traces
         differing by one space is a DIFFERENTIAL experiment, which beats reasoning
         about a parser we cannot see.
       ⚠ AH=0Ah is excluded: it now polls via `retry`, so tracing it would bury the
         log in thousands of identical lines -- it prints its completed line instead.
         Gated by a flag file so no other run pays for this. */
    /* ── ⛔⛔⛔ A CAP, AND IT IS THE THIRD INSTRUMENT IN ONE SESSION TO NEED ONE.
         The BOP logger ran away twice (268 MB each) before it got a hard ceiling; this
         one has none at all, and the moment XP's COMMAND.COM reached a command LOOP it
         wrote 2,166,824 trace lines and the same quarter-gigabyte. The flag file makes
         it opt-in, which is not the same as bounded -- opt-in only says who pays.
       ⇒ 4000 lines, then one line saying it stopped and how many it has seen. A trace
         whose size depends on the guest's loop rate cannot be read either way, and the
         first 4000 calls are where the answer is. */
    if (machine->IsTraceAll && function != 0x0A && machine->TraceCount <= DOS_TRACE_MAX) {
        if (++machine->TraceCount > DOS_TRACE_MAX) {
            trace = zput(trace, "  21: ... TRACE CAPPED at ");
            trace = zhex(trace, DOS_TRACE_MAX);
            trace = zput(trace, " calls -- the guest is looping; totals are in the summary\r\n");
        } else {
        trace = zput(trace, "  21:"); trace = zhexb(trace, (UINT)function);
        trace = zput(trace, "/");     trace = zhexb(trace, (UINT)(R_AX & 0xFF));
        trace = zput(trace, " bx="); trace = zhexb(trace, (UINT)((R_BX >> 8) & 0xFF));
        trace = zhexb(trace, (UINT)(R_BX & 0xFF));
        trace = zput(trace, " dx="); trace = zhexb(trace, (UINT)((R_DX >> 8) & 0xFF));
        trace = zhexb(trace, (UINT)(R_DX & 0xFF));
        /* The call site, so a trace of 31 calls says WHERE the guest is, not only
           what it wanted. Two calls from the same offset are a loop; a run of
           rising offsets is start-up walking forward. */
        if (isCallFramed) { trace = zput(trace, " @"); trace = zhex(trace, callSegment);
                     trace = zput(trace, ":"); trace = zhex(trace, callOffset); }
        trace = zput(trace, "\r\n");
        }
    }

    /* ── #210: FIVE LONG-FILENAME CALLS ARE THEIR SHORT-NAME TWINS, REGISTER FOR REGISTER.
         7139h mkdir, 713Ah rmdir, 713Bh chdir (DS:DX), 7156h rename (DS:DX -> ES:DI) and
         716Ch / 71A9h extended open (BX, CX, DX, DS:SI) take exactly what 39h/3Ah/3Bh/56h/
         6Ch take, and none of those arms reads AL. Our short-name arms were never
         short-name-only -- they hand the string to Win32, which resolves a long name as
         readily as an 8.3 one -- so the LFN call is served by the same code, error
         mapping, handle allocation and JFT bookkeeping (the s91 tail below keys on
         `ah`, which is the point: a 716Ch handle lands in the PSP's JFT like a 6Ch one).
       ⚠ So these five answer with the short arms' measured 6.22 error codes (39h over an
         existing name = 5, 3Bh to nowhere = 3, ...). Whether STOCK's LFN arms use the same
         numbers is p_lfn's lfn.7139.again / lfn.713B.missing rows. 71A9h ("server"
         open, a global handle on Windows 95) is an ordinary 716Ch here: there is one
         process and one handle table. Everything else in AH=71h is the arm further down. */
    if (function == 0x71) {
        BYTE subfunction = (BYTE)(R_AX & 0xFF);
        if (subfunction == 0x39 || subfunction == 0x3A || subfunction == 0x3B || subfunction == 0x56 || subfunction == 0x6C
            || subfunction == 0xA9) {
            isLfnAlias = subfunction;
            function = (subfunction == 0xA9) ? 0x6C : subfunction;
        }
    }

    if (function == 0x4C) {                     /* terminate */
        machine->ExitCode = (INT)(R_AX & 0xFF);  /* DOS errorlevel */
        trace = zput(trace, "  ==> DOS terminate (AH=4Ch), exit code AL=0x");
        trace = zhex(trace, R_AX & 0xFF);
        trace = DosInt21CallSite(trace, isCallFramed, callSegment, callOffset);
        trace = zput(trace, "\r\n");
        shouldContinue = 0;
    } else if (function == 0x00) {              /* terminate (CP/M style, = INT 20h) */
        /* Skyroads exits through this one, so "unhandled" was both wrong and misleading:
           we returned an error and let the guest run on into nowhere. It is just 4Ch with
           an exit code of 0. Logging the call site because WHY a game terminates is the
           question, and the CS tells you whether it was the program or something we
           vectored it into. */
        machine->ExitCode = 0;
        trace = zput(trace, "  ==> DOS terminate (AH=00h) from CS:IP=0x");
        trace = zhex(trace, VDM_REG(tib, VTIB_CS) & 0xFFFF); trace = zput(trace, ":0x");
        trace = zhex(trace, VDM_REG(tib, VTIB_EIP) & 0xFFFF);
        trace = zput(trace, " ivt8=0x");
        { const volatile BYTE *ivt = (const volatile BYTE *)0;
          DWORD timerSegment = (DWORD)ivt[0x22] | ((DWORD)ivt[0x23] << 8);
          DWORD timerOffset = (DWORD)ivt[0x20] | ((DWORD)ivt[0x21] << 8);
          DWORD tickSegment = (DWORD)ivt[0x72] | ((DWORD)ivt[0x73] << 8);
          DWORD tickOffset = (DWORD)ivt[0x70] | ((DWORD)ivt[0x71] << 8);
          trace = zhex(trace, timerSegment); trace = zput(trace, ":0x"); trace = zhex(trace, timerOffset);
          trace = zput(trace, " ivt1C=0x"); trace = zhex(trace, tickSegment);
          trace = zput(trace, ":0x"); trace = zhex(trace, tickOffset); }
        /* ── AND THE BYTES THAT LED HERE -- AT THE GUEST'S ADDRESS, NOT OURS.
             "SILENT VDM DEATH -> GET THE BYTES" is a standing rule here, and the
             first cut of this obeyed the letter of it while dumping from
             VTIB_CS:EIP -- the HANDLER's address. That produced `C4 C4 54`, read
             as a BOP marker, and two conclusions that were both retracted a
             session later. The guest's own call site is the pushed return address
             on its stack; DosInt21CallSite() dumps around that. */
        trace = DosInt21CallSite(trace, isCallFramed, callSegment, callOffset);
        trace = zput(trace, "\r\n");
        shouldContinue = 0;
    } else if (function == 0x02) {              /* print char DL */
        OUTC(R_DX & 0xFF); OKCF();
    } else if ((function == 0x01 || function == 0x07 || function == 0x08 || function == 0x0B
                || (function == 0x06 && (R_DX & 0xFF) == 0xFF))
               && DosHandleIsFile((PVOID const *)machine->FileHandles, 0)) {
        /* ── stdio (s91): CONSOLE INPUT FROM A FILE ON HANDLE 0. `prog < file` is the
             shell AH=46h-ing a file onto handle 0, and DOS's console-input functions
             read HANDLE 0 -- these read the keyboard whatever handle 0 was, so a
             redirected program never saw its file (and hung waiting for a key).
             What a file gives back, measured with tests/probes/dos/p_stdin against REAL
             MS-DOS on a real BIOS (PCem) and stock NTVDM, which agree byte for byte:
               01h/07h/08h  the next byte; AT END OF FILE THEY BLOCK -- neither
                            returns (DOSBox-X answers 0Ah; it is the odd one out).
                            Blocking here is a retry, so the guest keeps its ISRs.
               06h DL=FFh   the next byte, ZF clear; at EOF AL=00h and ZF SET
               0Bh          FFh while bytes remain, 00h at EOF
             (3Fh on handle 0 already reads the file: 0 bytes, CF clear, at EOF.) */
        HANDLE file = (HANDLE)machine->FileHandles[0];
        BYTE character = 0; DWORD received = 0;
        if (function == 0x0B) {
            DWORD position = SetFilePointer(file, 0, NULL, FILE_CURRENT);
            DWORD size  = GetFileSize(file, NULL);
            SETAX((R_AX & 0xFF00) | ((position != INVALID_SET_FILE_POINTER && position < size) ? 0xFF : 0x00));
            OKCF();
        } else if (!ReadFile(file, &character, 1, &received, NULL) || received == 0) {
            if (function == 0x06) { SETAX(R_AX & 0xFF00); SETZF(); OKCF(); }
            else machine->IsRetry = 1;          /* EOF: block, as DOS does */
        } else {
            if (function == 0x01) OUTC(character);
            SETAX((R_AX & 0xFF00) | character);
            if (function == 0x06) CLRZF();
            OKCF();
        }
    } else if (function == 0x01 || function == 0x07 || function == 0x08) {  /* read char (01 echoes) */
        /* Poll, do not block. If no key is waiting we ask the host to re-run this INT
           rather than parking the exec thread -- see `retry` in dos_int21.h. */
        INT character = machine->ConsoleInNoWait ? machine->ConsoleInNoWait(machine->ConsoleInContext) : -1;
        if (character < 0) { machine->IsRetry = 1; }
        else {
            if (function == 0x01) OUTC(character);  /* AH=01: echo                     */
            SETAX((R_AX & 0xFF00) | (character & 0xFF)); OKCF();
        }
    } else if (function == 0x0A && DosHandleIsFile((PVOID const *)machine->FileHandles, 0)) {
        /* stdio (s91): the line from a FILE on handle 0 -- bytes up to the CR, the
           LF a text file puts after it skipped at the start of the next line, each
           echoed as the keyboard form echoes them. At EOF with nothing read it
           blocks, as 08h does (p_stdin: PCem + stock). */
        volatile BYTE *buffer = (volatile BYTE *)((R_DS << 4) + (R_DX & 0xFFFF));
        INT maximumLength = buffer[0], length = 0;
        BYTE character; DWORD received;
        HANDLE file = (HANDLE)machine->FileHandles[0];
        for (;;) {
            if (!ReadFile(file, &character, 1, &received, NULL) || received == 0) break;
            if (character == 0x0A && length == 0) continue;
            if (character == 0x0D) break;
            if (length < maximumLength - 1) { buffer[2 + length++] = character; OUTC(character); }
        }
        if (length == 0 && received == 0) machine->IsRetry = 1;
        else {
            buffer[1] = (BYTE)length; buffer[2 + length] = 0x0D;
            OUTC(0x0D); OUTC(0x0A);
            OKCF();
        }
    } else if (function == 0x0A) {              /* buffered input DS:DX */
        /* ── THE LAST INPUT CALL THAT PARKED THE EXEC THREAD, AND IT DEADLOCKS A SHELL.
             This used to sit in a loop on the BLOCKING m->conin until it had a whole
             line. AH=01/07/08 and INT 16h were both fixed years ago to poll via
             `retry` (see the note on that field), and the reason is spelled out
             there: blocking in C stops the GUEST dead. For a game that meant a frozen
             screen. For COMMAND.COM it means never running at all, because the thing
             it is waiting for CANNOT ARRIVE while it waits:
                 COMMAND.COM -> INT 21h AH=0Ah -> we block on the BIOS key ring
                 ...the BIOS key ring is filled by the guest's own INT 09h ISR
                 ...which cannot run, because we are blocked inside its INT 21h call.
             Measured exactly that way: the shell printed its banner and prompt, then
             40 scancodes went into the FIFO and IRQ1 was attempted 691 times, EVERY
             one refused as `not_in_exec`. The keys were there the whole time and the
             guest was never running to take them.
           ► SO COLLECT THE LINE ACROSS RETRIES. The characters accumulate in the
             GUEST's buffer (untouched between retries) and we keep only our position
             in it; a different DS:DX is a different call, not a continuation. Each
             retry leaves EIP on the BOP, so the guest re-executes the INT and gets to
             run its ISRs in between -- which is what a real DOS does, since the BIOS
             spins in the guest with interrupts enabled. */
        volatile BYTE *buffer = (volatile BYTE *)((R_DS << 4) + (R_DX & 0xFFFF));
        INT maximumLength = buffer[0], character;
        if (!machine->IsLineActive || machine->LineSegment != (WORD)(R_DS & 0xFFFF)
                            || machine->LineOffset != (WORD)(R_DX & 0xFFFF)) {
            machine->IsLineActive = 1; machine->LineLength = 0;
            machine->LineSegment = (WORD)(R_DS & 0xFFFF);
            machine->LineOffset = (WORD)(R_DX & 0xFFFF);
        }
        for (;;) {
            if (machine->LineLength >= maximumLength - 1) break;  /* buffer full -> take it as a line */
            character = machine->ConsoleInNoWait ? machine->ConsoleInNoWait(machine->ConsoleInContext) : 0x0D;
            if (character < 0) { machine->IsRetry = 1; break; }  /* nothing yet -> let the guest run */
            /* s91: a LF at the START of a line is the tail of the previous line's
               CR LF in a redirected file (`command < script`, cmd's `<` lands on the
               host stdin this reads). Every line after the first began with it and
               6.22's COMMAND.COM answered "Bad command or file name" to each. */
            if (character == 0x0A && machine->LineLength == 0) continue;
            if (character == 0x0D) { machine->IsLineActive = 0; break; }
            if (character == 0x08) {                    /* backspace: rub it out on screen */
                if (machine->LineLength > 0) { --machine->LineLength; OUTC(0x08); OUTC(' '); OUTC(0x08); }
                continue;
            }
            if (character == 0x00) continue;            /* extended key: no ASCII, ignore  */
            buffer[2 + machine->LineLength++] = (BYTE)character; OUTC(character);
        }
        if (!machine->IsRetry) {
            buffer[1] = (BYTE)machine->LineLength; buffer[2 + machine->LineLength] = 0x0D;
            OUTC(0x0D); OUTC(0x0A);
            machine->IsLineActive = 0;
            /* WHAT THE SHELL ACTUALLY RECEIVES. `echo hi` works while a bare `ver`
               comes back "Bad command or file name" -- and the difference between
               them is a SPACE, i.e. whether the command word ends at a delimiter or
               at our terminator. That points straight at these bytes, so print them
               rather than reason about them. */
            if (machine->IsTraceAll) { INT index;
              trace = zput(trace, "  INT21 AH=0A line max="); trace = zhexb(trace, (UINT)maximumLength);
              trace = zput(trace, " n="); trace = zhexb(trace, (UINT)machine->LineLength);
              trace = zput(trace, " [");
              for (index = 0; index < machine->LineLength + 1 && index < 64; ++index) {
                  trace = zhexb(trace, buffer[2 + index]); trace = zput(trace, " ");
              }
              trace = zput(trace, "]\r\n"); }
            OKCF();
        }
    } else if (function == 0x0B) {              /* check input status */
        INT isReady = machine->ConsolePeek ? machine->ConsolePeek(machine->ConsoleInContext) : 0;
        SETAX((R_AX & 0xFF00) | (isReady ? 0xFF : 0x00));  /* FFh = char waiting */
        OKCF();
    } else if (function == 0x06) {              /* direct console I/O (DL=FF -> read) */
        if ((R_DX & 0xFF) == 0xFF) {            /* input: non-blocking, ZF=1 if none */
            INT character = machine->ConsoleInNoWait ? machine->ConsoleInNoWait(machine->ConsoleInContext) : -1;
            if (character >= 0) { SETAX((R_AX & 0xFF00) | (character & 0xFF)); CLRZF(); }
            else        { SETAX(R_AX & 0xFF00); SETZF(); }
        } else {                                /* output: write DL, AL=DL */
            OUTC(R_DX & 0xFF); SETAX((R_AX & 0xFF00) | (R_DX & 0xFF));
        }
        OKCF();
    } else if (function == 0x09) {              /* print $-string DS:DX */
        const volatile BYTE *text = (const volatile BYTE *)((R_DS << 4) + (R_DX & 0xFFFF));
        INT index; for (index = 0; index < 1024 && *text != '$'; ++index, ++text) OUTC(*text);
        OKCF();
    } else if (function == 0x40) {              /* write: BX=handle CX=cnt DS:DX=buf */
        DWORD handle = R_BX & 0xFFFF, count = R_CX & 0xFFFF;
        PCSTR buffer = (PCSTR)((R_DS << 4) + (R_DX & 0xFFFF));
        /* ── HANDLES 0-4 ARE TABLE ENTRIES, NOT A SPECIAL CASE. ──────────────────
             DOS pre-opens stdin/stdout/stderr/aux/prn as ordinary slots in the same
             handle table as everything else, and that is precisely WHY redirection
             works: the shell opens the file, dup2s it over handle 1, runs the
             command, then dup2s the saved copy back. Treating 1 and 2 as "the
             console, always" accepted the redirect and then ignored it -- measured,
             `echo hello world > hi.txt` printed to the screen and left an EMPTY
             hi.txt on disk, which is the worst of both.
             So: a BOUND handle is a file, whatever its number; only an unbound low
             handle is the console. */
        /* ── #275: A WRITE THAT FAILS FOR A HARDWARE REASON IS A CRITICAL ERROR. The
             result of WriteFile was never looked at: a write to a file whose floppy
             was pulled, or that went write-protected, answered CF=0 with however many
             bytes Win32 managed (usually 0) -- a success that never happened. A
             hardware error (19-31, same numbers on both sides) now goes back as that
             code with CF=1, which the tail turns into the program's INT 24h (or, where
             INT 24h cannot be raised, into the answer FAIL gives).
           ⚠ ANY OTHER FAILURE KEEPS THE OLD ANSWER, deliberately: disk full is CF=0
             with a short count on DOS too, and the rest (access denied on a read-only
             handle = DOS 5) is a separate, unmeasured question. */
        if (DosHandleIsFile((PVOID const *)machine->FileHandles, handle)) {
            DWORD written = 0, win32Error = 0; WORD dosError = 0;
            if (!WriteFile(machine->FileHandles[handle], buffer, count, &written, NULL)) win32Error = GetLastError();
            if (win32Error && DosErrFromWin32((unsigned long)win32Error, &dosError) && DosCritIsHardwareError(dosError)) {
                g_DosReadWriteDrive = DosHandleDrive(machine->FileHandles[handle]);
                SETAX(dosError); ERRCF();
                trace = zput(trace, "  INT21 AH=40 h="); trace = zhex(trace, handle);
                trace = zput(trace, " cnt=0x"); trace = zhex(trace, count);
                trace = zput(trace, " FAILED win32=0x"); trace = zhex(trace, win32Error);
                trace = zput(trace, " (hardware) drive=");
                if (g_DosReadWriteDrive >= 0) { CHAR driveText[3] = { (CHAR)('A' + g_DosReadWriteDrive), ':', 0 }; trace = zput(trace, driveText); }
                else trace = zput(trace, "?");
                trace = zput(trace, "\r\n");
            } else { SETAX(written); OKCF(); DosStampVdmNow(machine->FileHandles[handle]); /* #263 */ }
        }
        /* ── #251: AN UNREDIRECTED 3 IS AUX AND 4 IS PRN, and they go to the BIOS
             (INT 14h / INT 17h) like DOS's own drivers -- they used to be refused
             with error 6 here, after AH=04h/05h had thrown their bytes away. */
        else if ((handle == 3 || handle == 4) && DosHandleIsDevice((PVOID const *)machine->FileHandles, machine->StdOpen, handle)) {
            if (AUXPRN_V86) AUXPRN_TRAMP(handle == 4 ? DOS_AUXPRN_PRN_WRITE : DOS_AUXPRN_AUX_WRITE);
            else {
                DWORD index;
                for (index = 0; index < count; ++index) {
                    if (handle == 4) { if (machine->PrinterOut) (VOID)machine->PrinterOut(machine->DeviceContext, (BYTE)buffer[index]); }
                    else if (machine->AuxOut) machine->AuxOut(machine->DeviceContext, (BYTE)buffer[index]);
                }
                SETAX(count); OKCF();
            }
        }
        /* ⚠ ANY device slot, not just 1 and 2 -- after AH=45h the console can be
             sitting in slot 5. A duplicate loses which device it was, so a dup of
             AUX would print here; nothing does that, and the alternative is a
             per-slot identity byte we have no caller for. */
        else if (DosHandleIsDevice((PVOID const *)machine->FileHandles, machine->StdOpen, handle))
             { DWORD index; for (index = 0; index < count; ++index) OUTC(buffer[index]); SETAX(count); OKCF(); }
        else { SETAX(6); ERRCF(); }
    } else if (function == 0x3C || function == 0x3D) {  /* create / open: DS:DX=ASCIIZ name */
        /* ── DOS HANDS OUT THE LOWEST FREE HANDLE, AND THAT IS HOW `>` WORKS. ─────
             COMMAND.COM does not redirect with dup2. It CLOSES handle 1 and then
             creates the target, relying on the new file landing in the slot the
             console just vacated -- measured, the trace is `3Ch create` followed
             immediately by `40h write to handle 1` with no 45h/46h anywhere.
             Allocating from 5 upwards, as this did, makes that impossible: the file
             got handle 5, handle 1 was still the console, so the text went to the
             screen and the file stayed 0 bytes. Two earlier fixes (AH=40h, then
             AH=02h) were aimed at the write end and neither moved it, because the
             write end was never wrong -- the HANDLE NUMBER was. */
        /* ── WE DO NOT EMULATE SHARE.EXE, SO WE MUST NOT ENFORCE IT. (session 37) ──
             FILE_SHARE_READ here means a second open of a file this VDM already holds
             for writing fails with ERROR_SHARING_VIOLATION -- a lock bare DOS does not
             have, reported back as DOS error 2 "file not found", which sends the guest
             looking for a file that is there. The protected-mode twin of this call cost
             the GDI.EXE wall exactly that way. Share everything; the access mode below
             still comes from the guest. */
        CHAR fileName[300]; DWORD slot; HANDLE file;
        DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE;
        DosGuestPath(machine, R_DS, R_DX, fileName, sizeof(fileName));
        if (function == 0x3C)
            file = CreateFileA(fileName, GENERIC_READ | GENERIC_WRITE, share,
                            NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        else {
            DWORD mode = R_AX & 7;
            DWORD access = (mode == 1) ? GENERIC_WRITE
                      : (mode == 2) ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_READ;
            file = DosOpenStampable(fileName, access, share, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
        }
        if (file != INVALID_HANDLE_VALUE) {
            slot = DosHandleAllocate((PVOID const *)machine->FileHandles, machine->StdOpen);
            if (slot < DOS_MAX_FILES) { machine->FileHandles[slot] = file; SETAX(slot); OKCF();
                                        if (function == 0x3C) DosStampVdmNow(file); /* #263 */ }
            else { CloseHandle(file); SETAX(4); ERRCF(); }
        } else {
            /* ── ASK WHY IT FAILED. It used to answer 2 for every cause; see
                 DosErrFromWin32() for the two oracle rows that names wrong. */
            DWORD win32Error = GetLastError(); WORD dosError;
            INT isMapped = DosErrFromWin32((unsigned long)win32Error, &dosError);
            SETAX(dosError); ERRCF();
            trace = zput(trace, "  INT21 AH=0x"); trace = zhex(trace, function);
            trace = zput(trace, " ["); trace = zput(trace, fileName); trace = zput(trace, "] FAILED win32=0x");
            trace = zhex(trace, win32Error);
            trace = zput(trace, isMapped ? " -> AX=0x" : " UNMAPPED, kept -> AX=0x");
            trace = zhex(trace, dosError); trace = zput(trace, "\r\n");
        }
        trace = zput(trace, "  INT21 AH=0x"); trace = zhex(trace, function);
        trace = zput(trace, " ["); trace = zput(trace, fileName); trace = zput(trace, "] -> AX=0x");
        trace = zhex(trace, R_AX & 0xFFFF); trace = zput(trace, (*guestFlags & 1) ? " (err)\r\n" : "\r\n");
    } else if (function == 0x3E) {              /* close: BX=handle */
        DWORD handle = R_BX & 0xFFFF;
        /* Any BOUND handle closes, including a low one the shell redirected -- see
           the note at AH=40h. An unbound 0-4 is the console and closing it is a no-op. */
        if (DosHandleIsFile((PVOID const *)machine->FileHandles, handle)) DosHandleRelease(machine, handle);
        else DosHandleSetDevice(&machine->StdOpen, handle, 0);  /* free the device slot */
        OKCF();
    } else if (function == 0x3F) {              /* read: BX=handle CX=cnt -> DS:DX */
        DWORD handle = R_BX & 0xFFFF, count = R_CX & 0xFFFF, read = 0;
        PVOID buffer = (VOID *)((R_DS << 4) + (R_DX & 0xFFFF));
        if (DosHandleIsFile((PVOID const *)machine->FileHandles, handle)) {  /* bound -> a file, even if low */
            /* ► LOG THE FILE POSITION, THE COUNT AND THE FIRST BYTES. A DOS extender
                 loading an executable is doing nothing but seek+read, so if the image it
                 ends up with is wrong, the first question is whether WE handed it the
                 right bytes -- and that is answerable offline by comparing these lines
                 against the file. Without the position a short or misplaced read is
                 indistinguishable from a correct one. */
            DWORD position = SetFilePointer(machine->FileHandles[handle], 0, NULL, FILE_CURRENT);
            /* #275: and a read that FAILS for a hardware reason is a critical error --
               see AH=40h; same rule, same reasons for leaving every other failure
               alone (it used to answer them all as CF=0 with what Win32 read). */
            DWORD win32Error = 0; WORD dosError = 0;
            if (!ReadFile(machine->FileHandles[handle], buffer, count, &read, NULL)) win32Error = GetLastError();
            if (win32Error && DosErrFromWin32((unsigned long)win32Error, &dosError) && DosCritIsHardwareError(dosError)) {
                g_DosReadWriteDrive = DosHandleDrive(machine->FileHandles[handle]);
                SETAX(dosError); ERRCF();
                trace = zput(trace, "  INT21 AH=3F FAILED win32=0x"); trace = zhex(trace, win32Error);
                trace = zput(trace, " (hardware) drive=");
                if (g_DosReadWriteDrive >= 0) { CHAR driveText[3] = { (CHAR)('A' + g_DosReadWriteDrive), ':', 0 }; trace = zput(trace, driveText); }
                else trace = zput(trace, "?");
                trace = zput(trace, "\r\n");
            } else { SETAX(read); OKCF(); }
            trace = zput(trace, "  INT21 AH=3F h="); trace = zhex(trace, handle);
            trace = zput(trace, " pos=0x"); trace = zhex(trace, position);
            trace = zput(trace, " cnt=0x"); trace = zhex(trace, count);
            trace = zput(trace, " got=0x"); trace = zhex(trace, read);
            trace = zput(trace, " -> 0x"); trace = zhex(trace, (DWORD)(ULONG_PTR)buffer);
            trace = zput(trace, " first="); trace = zdump(trace, (PCBYTE)buffer, (read >= 8) ? 8 : 0);
            trace = zput(trace, "\r\n");
        }
        else if (handle == 0 && DosHandleIsDevice((PVOID const *)machine->FileHandles, machine->StdOpen, handle)) {
            /* ── #251: STDIN IS THE KEYBOARD, AND DOS READS A LINE FROM IT. ──────────
                 This answered 0 bytes -- end of file -- so any program reading its
                 input through handle 0 (C's gets/scanf/fgets do) saw an empty stream
                 and gave up. DOS's console read is cooked: it echoes, honours
                 backspace, ends at Enter, and returns the line WITH CR LF; a line
                 longer than the caller asked for is handed out over later reads.
                 Collected across retries like AH=0Ah, so the guest keeps running
                 (and taking its interrupts) while it waits for keys. */
            volatile BYTE *bytes = (volatile BYTE *)buffer;
            INT character;
            DWORD transferred = 0;
            if (machine->ConsolePosition >= machine->ConsoleLength) {  /* nothing pending: collect a line */
                if (!machine->IsConsoleCollecting) { machine->IsConsoleCollecting = 1; machine->ConsoleTyped = 0; }
                for (;;) {
                    character = machine->ConsoleInNoWait ? machine->ConsoleInNoWait(machine->ConsoleInContext) : 0x0D;
                    if (character < 0) { machine->IsRetry = 1; break; }
                    if (character == 0x0D) break;
                    if (character == 0x08) {
                        if (machine->ConsoleTyped > 0) { --machine->ConsoleTyped; OUTC(0x08); OUTC(' '); OUTC(0x08); }
                        continue;
                    }
                    if (character == 0x00) continue;    /* extended key: no ASCII */
                    if (machine->ConsoleTyped >= 127) continue;  /* full: only Enter ends it */
                    machine->ConsoleLine[machine->ConsoleTyped++] = (BYTE)character; OUTC(character);
                }
                if (machine->IsRetry) goto readDone;
                machine->ConsoleLine[machine->ConsoleTyped] = 0x0D; machine->ConsoleLine[machine->ConsoleTyped + 1] = 0x0A;
                machine->ConsoleLength = machine->ConsoleTyped + 2; machine->ConsolePosition = 0; machine->IsConsoleCollecting = 0;
                OUTC(0x0D); OUTC(0x0A);
            }
            while (transferred < count && machine->ConsolePosition < machine->ConsoleLength) bytes[transferred++] = machine->ConsoleLine[machine->ConsolePosition++];
            SETAX(transferred); OKCF();
        readDone: ;
        }
        else if (handle == 3 && AUXPRN_V86 && DosHandleIsDevice((PVOID const *)machine->FileHandles, machine->StdOpen, handle))
             AUXPRN_TRAMP(DOS_AUXPRN_AUX_READ);       /* #251: AUX, through INT 14h */
        else if (DosHandleIsDevice((PVOID const *)machine->FileHandles, machine->StdOpen, handle))
             { SETAX(0); OKCF(); }              /* PRN, a dup, or AUX in PM: EOF */
        else { SETAX(6); ERRCF(); }
    } else if (function == 0x42) {              /* lseek: AL=org BX=h CX:DX=off */
        DWORD handle = R_BX & 0xFFFF, method = R_AX & 0xFF;
        LONG distance = (LONG)(((R_CX & 0xFFFF) << 16) | (R_DX & 0xFFFF));
        /* ── A BOUND HANDLE IS A FILE, WHATEVER ITS NUMBER. (GH #133) ─────────
             This read `h >= 5`, and that is how `>>` was broken while `>` worked:
             the shell redirects stdout by closing handle 1 and opening the
             target into the slot it vacates, then seeks to end-of-file before
             appending. Excluding handles below 5 refused that seek with error 6
             on a handle that IS a file -- the last survivor of #133, after the
             create and both write paths had been fixed.
             Oracle, tests/probes/dos/p_redir.asm on MS-DOS 6.22:
               CASE=int21.42.end.on.h1 SIG=AX,DX,CF AX=0004 DX=0000 CF=0
             i.e. real DOS seeks handle 1 to the end and reports 4 bytes. */
        if (DosHandleIsFile((PVOID const *)machine->FileHandles, handle)) {
            DWORD newPosition = SetFilePointer(machine->FileHandles[handle], distance, NULL, method);
            SETAX(newPosition & 0xFFFF);
            R_DX = (R_DX & 0xFFFF0000u) | ((newPosition >> 16) & 0xFFFF); OKCF();
        } else { SETAX(6); ERRCF(); }
    } else if (function == 0x30) {              /* get DOS version */
        /* AL=major, AH=minor, BH=OEM, BL:CX=24-bit serial.  GH #28.
           BX and CX were never written before, so a caller saw whatever it had
           left in them and read that as our OEM number and serial.  Values
           confirmed against the 6.22 oracle: BH=0xFF (generic MS-DOS), serial 0.
           The version itself is configurable -- see DosInt21SetVersion(). */
        SETAX(DosVersionWord(machine));         /* per process: see v5_psp (#208) */
        SET16(R_BX, 0xFF00);                    /* BH=OEM 0xFF, BL=serial high */
        SET16(R_CX, 0x0000);                    /* serial low                  */
        OKCF();
    } else if (function == 0x4E || function == 0x4F) {  /* find first / find next */
        volatile BYTE *dta = (volatile BYTE *)((machine->DtaSegment << 4) + machine->DtaOffset);
        WIN32_FIND_DATAA findData;
        WORD mask;
        INT slot = -1, isOk = 0;
        if (function == 0x4E) {
            CHAR pattern[300];
            DosGuestPath(machine, R_DS, R_DX, pattern, sizeof(pattern));
            mask = (WORD)(R_CX & 0xFFFF);
            for (slot = 0; slot < 8 && machine->FindHandles[slot]; ++slot) {}
            if (slot >= 8) { slot = 0;                       /* recycle the oldest */
                             FindClose(machine->FindHandles[0]); machine->FindHandles[0] = 0; }
            { CHAR directoryPattern[300]; BYTE nameTemplate[11]; INT isNoDirectory;
              HANDLE find;
              DosFindSplit(pattern, directoryPattern, sizeof directoryPattern, nameTemplate);  /* DOS matching: see DosFindMatches */
              find = DosFindFirst(directoryPattern, nameTemplate, mask, &findData, &isNoDirectory);
              if (machine->IsTraceAll) { trace = zput(trace, "  INT21 AH=4E ["); trace = zput(trace, pattern);
                                  trace = zput(trace, "] attr=0x"); trace = zhex(trace, mask);
                                  trace = zput(trace, find == INVALID_HANDLE_VALUE ? " -> none" : " -> found");
                                  trace = zput(trace, isNoDirectory ? " (no such directory)\r\n" : "\r\n"); }
              if (find == INVALID_HANDLE_VALUE) {
                  /* ORACLE-CONFIRMED, and not what memory suggests: a pattern
                     that matches nothing inside an EXISTING directory is
                     AX=18 "no more files", not AX=2 "file not found". A missing
                     directory is AX=3. */
                  SETAX(isNoDirectory ? 3 : 18);
                  /* #34: a HARDWARE failure (not ready, write-protected, ...) is its
                     own DOS code -- Win32 kept DOS's numbers for 19-31 -- and the
                     dispatcher's tail turns it into an INT 24h. */
                  if (g_DosFindWin32Error >= 19 && g_DosFindWin32Error <= 31) SETAX(g_DosFindWin32Error);
                  ERRCF();
              } else {
                  machine->FindHandles[slot] = find;
                  isOk = 1;
                  /* Fill DOS's private search area deterministically.  It is
                     DOS-private, but leaving the caller's bytes lying in it
                     means the DTA differs run to run for no reason; real 6.22
                     puts the EXPANDED 11-byte search template there (a "*.*"
                     search reads back as eleven '?'), so do the same. */
                  /* The template, exactly as matched -- 4Fh reads it back from here. */
                  { INT nameIndex; for (nameIndex = 0; nameIndex < 11; ++nameIndex) dta[1 + nameIndex] = nameTemplate[nameIndex]; }
                  dta[0] = 3;                                /* drive C:        */
                  dta[12] = (BYTE)(mask & 0xFF);
                  dta[13] = 0; dta[14] = 0; dta[15] = 0; dta[16] = 0;
                  dta[17] = 0; dta[18] = 0;
                  dta[19] = DOS_FIND_MAGIC;
                  dta[20] = (BYTE)slot;
              }
            }
        } else {                                             /* 4Fh: continue   */
            mask = (WORD)dta[12];
            if (dta[19] == DOS_FIND_MAGIC && dta[20] < 8 && machine->FindHandles[dta[20]]) {
                BYTE nameTemplate[11]; INT index;
                slot = dta[20];
                for (index = 0; index < 11; ++index) nameTemplate[index] = dta[1 + index];  /* the template 4Eh stored */
                isOk = DosFindNext(machine->FindHandles[slot], nameTemplate, mask, &findData);
            } else {
                SETAX(18); ERRCF();                          /* no search live  */
            }
        }
        if (slot >= 0 && isOk) {
            DosDtaFill(dta, &findData);
            /* DIR renders blank names, one impossible size repeated, and a 1980-ish
               date -- i.e. it is reading fields we did not put where it looks. Print
               the DTA we hand back, whole, and let the bytes settle it. */
            if (machine->IsTraceAll) { INT position;
              trace = zput(trace, "  INT21 AH=4E/4F dta="); trace = zhexb(trace, (UINT)((machine->DtaSegment >> 8) & 0xFF));
              trace = zhexb(trace, (UINT)(machine->DtaSegment & 0xFF)); trace = zput(trace, ":");
              trace = zhexb(trace, (UINT)((machine->DtaOffset >> 8) & 0xFF));
              trace = zhexb(trace, (UINT)(machine->DtaOffset & 0xFF));
              trace = zput(trace, " [");
              for (position = 0; position < 44; ++position) { trace = zhexb(trace, (UINT)dta[position]); trace = zput(trace, " "); }
              trace = zput(trace, "]\r\n"); }
            SETAX(0); OKCF();                                /* oracle: AX=0000 */
        } else if (slot >= 0 && machine->FindHandles[slot] && !isOk) {
            FindClose(machine->FindHandles[slot]); machine->FindHandles[slot] = 0;
            dta[19] = 0;
            SETAX(18); ERRCF();                              /* no more files   */
        }
    } else if ((function >= 0x0F && function <= 0x17) || (function >= 0x21 && function <= 0x24)
               || (function >= 0x27 && function <= 0x29)) {  /* ---- the FCB interface ---- */
        volatile BYTE *fcb = DosFcbAt(R_DS, R_DX);
        CHAR name[300];
        #define FCB_OK()   SETAX((R_AX & 0xFF00) | 0x00)
        #define FCB_FAIL() SETAX((R_AX & 0xFF00) | 0xFF)
        /* CF is undefined for these on real DOS; leave it as the guest set it. */
        if (function == 0x0F || function == 0x16) {  /* open / create */
            HANDLE file; DWORD slot;
            DosFcbName(fcb, name);
            file = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL,
                              (function == 0x16) ? CREATE_ALWAYS : OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, NULL);
            if (file == INVALID_HANDLE_VALUE) FCB_FAIL();
            else {
                FILETIME fileTime, localTime; WORD dosDate = 0, dosTime = 0;
                DWORD size = GetFileSize(file, NULL);
                slot = DosHandleAllocate((PVOID const *)machine->FileHandles, machine->StdOpen);
                if (slot >= DOS_MAX_FILES) { CloseHandle(file); FCB_FAIL(); }
                else {
                    machine->FileHandles[slot] = file;
                    if (function == 0x16) DosStampVdmNow(file);  /* #263, before the FCB reads it */
                    if (GetFileTime(file, NULL, NULL, &fileTime)
                        && FileTimeToLocalFileTime(&fileTime, &localTime))
                        FileTimeToDosDateTime(&localTime, &dosDate, &dosTime);
                    fcb[12] = 0; fcb[13] = 0;
                    fcb[14] = 128; fcb[15] = 0;     /* oracle: record size 128 */
                    fcb[16] = (BYTE)(size & 0xFF);        fcb[17] = (BYTE)((size >> 8) & 0xFF);
                    fcb[18] = (BYTE)((size >> 16) & 0xFF); fcb[19] = (BYTE)((size >> 24) & 0xFF);
                    fcb[20] = (BYTE)(dosDate & 0xFF); fcb[21] = (BYTE)(dosDate >> 8);
                    fcb[22] = (BYTE)(dosTime & 0xFF); fcb[23] = (BYTE)(dosTime >> 8);
                    /* DOS replaces a "default drive" 0 with the drive it
                       actually resolved -- measured: the oracle returns 01 when
                       run from A:, DOSBox 03 from C:. We were leaving the
                       caller's 0 in place. */
                    if (!fcb[0]) fcb[0] = (BYTE)(DosCurrentDrive(machine) + 1);
                    fcb[24] = DOS_FCB_MAGIC; fcb[25] = (BYTE)slot;
                    FCB_OK();
                }
            }
        } else if (function == 0x10) {          /* close */
            if (fcb[24] == DOS_FCB_MAGIC && fcb[25] < DOS_MAX_FILES && machine->FileHandles[fcb[25]]) {
                DosHandleRelease(machine, fcb[25]); fcb[24] = 0; FCB_OK();
            } else FCB_FAIL();
        } else if (function == 0x11 || function == 0x12) {  /* find first / find next */
            volatile BYTE *dta = (volatile BYTE *)((machine->DtaSegment << 4) + machine->DtaOffset);
            WIN32_FIND_DATAA findData;
            INT received = 0;
            /* An extended FCB carries its search attribute in the byte just
               before the part DosFcbAt() returns; a normal one asks for ordinary
               files only.  WITHOUT THIS FILTER the search returns "." first --
               measured: our DTA came back with a blank name where the oracle had
               COMMAND.COM, because "." has no 8.3 name to put in the field. */
            WORD mask = (fcb != (volatile BYTE *)((R_DS << 4) + (R_DX & 0xFFFF)))
                           ? (WORD)fcb[-1] : 0;
            /* ── A VOLUME-LABEL SEARCH IS NOT A FILE SEARCH. ────────────────────
                 Attribute 08h means "return the volume label and nothing else", and
                 it is how DIR fills in its header line. There is no file on disk to
                 match, so FindFirstFile cannot answer it -- we used to run the
                 ordinary search and hand back whatever came first, which is why DIR
                 announced `Volume in drive C is COMMAND COM`, the first file in the
                 directory wearing the label's clothes.
                 The label is 11 bytes in the name+ext field, NOT an 8.3 name, so it
                 is padded raw rather than through DosFcbPutName. */
            if (mask == 0x08) {
                if (function == 0x11) {
                    CHAR volume[128]; INT volumeIndex;
                    volatile BYTE *extension;
                    INT isExtended = (fcb != (volatile BYTE *)((R_DS << 4) + (R_DX & 0xFFFF))) ? 7 : 0;
                    volume[0] = 0;
                    if (!GetVolumeInformationA("C:\\", volume, sizeof volume,
                                               NULL, NULL, NULL, NULL, 0) || !volume[0]) {
                        if (machine->FcbFind) { FindClose(machine->FcbFind); machine->FcbFind = 0; }
                        machine->LastError = 18;
                        FCB_FAIL();                       /* no label: DIR says so   */
                        goto fcbDone;
                    }
                    extension = dta + isExtended;
                    if (isExtended) { INT position; dta[0] = 0xFF; for (position = 1; position <= 5; ++position) dta[position] = 0; dta[6] = 0x08; }
                    extension[0] = 3;                     /* drive C:                */
                    for (volumeIndex = 0; volumeIndex < 11; ++volumeIndex) {
                        CHAR character = volume[volumeIndex] ? volume[volumeIndex] : ' ';
                        if (!volume[volumeIndex]) { extension[1 + volumeIndex] = ' '; continue; }
                        extension[1 + volumeIndex] = (BYTE)(character >= 'a' && character <= 'z' ? character - 32 : character);
                    }
                    extension[12] = 0x08;                 /* attribute: volume label */
                    { INT position; for (position = 13; position <= 32; ++position) extension[position] = 0; }
                    if (machine->FcbFind) { FindClose(machine->FcbFind); machine->FcbFind = 0; }
                    FCB_OK();
                } else { machine->LastError = 18; FCB_FAIL(); }  /* 12h: only ever one label */
                goto fcbDone;
            }
            if (function == 0x11) {
                /* The FCB's own 11 bytes ARE the template (`????????.???` for DIR), matched
                   against each entry's 8.3 name -- see DosFindMatches. The drive byte
                   picks the directory: "X:*" is that drive's current directory. */
                HANDLE find; CHAR allPattern[8]; INT index, isNoDirectory, count = 0;
                if (machine->FcbFind) { FindClose(machine->FcbFind); machine->FcbFind = 0; }
                for (index = 0; index < 11; ++index) machine->FcbTemplate[index] = fcb[1 + index];
                if (fcb[0]) { allPattern[count++] = (CHAR)('A' + fcb[0] - 1); allPattern[count++] = ':'; }
                allPattern[count++] = '*'; allPattern[count] = 0;
                find = DosFindFirst(allPattern, machine->FcbTemplate, mask, &findData, &isNoDirectory);
                if (find != INVALID_HANDLE_VALUE) { machine->FcbFind = find; received = 1; }
                if (machine->IsTraceAll) { CHAR currentDirectory[260]; INT position;
                    GetCurrentDirectoryA(sizeof currentDirectory, currentDirectory);
                    trace = zput(trace, "  INT21 AH=11 ["); trace = zput(trace, allPattern);
                    trace = zput(trace, "] in ["); trace = zput(trace, currentDirectory); trace = zput(trace, "] tmpl=[");
                    for (position = 0; position < 11; ++position) { CHAR pair[2]; pair[0] = (CHAR)machine->FcbTemplate[position]; pair[1] = 0; trace = zput(trace, pair); }
                    trace = zput(trace, "] mask=0x"); trace = zhex(trace, mask);
                    trace = zput(trace, received ? " -> found\r\n" : " -> none\r\n"); }
            } else if (machine->FcbFind) {
                received = DosFindNext(machine->FcbFind, machine->FcbTemplate, mask, &findData);
                if (!received) { FindClose(machine->FcbFind); machine->FcbFind = 0; }
            }
            /* ── A FAILED SEARCH MUST SAY WHY, OR THE LAST FAILURE SPEAKS FOR IT. ──
                 The extended error (AH=59h) is only recorded where CF comes back set,
                 and FCB calls deliberately leave CF alone -- so an exhausted search
                 left `last_err` holding whatever failed previously. In a shell that
                 is COMMAND.COM's own startup probe: it asks AH=48h for 0xFFFF
                 paragraphs to learn the largest block, which fails with code 8.
                 DIR then ends its listing, asks AH=59h why, is told "insufficient
                 memory", and prints exactly that instead of its summary line. The
                 listing was RIGHT and the epitaph was three commands stale.
                 18 = "no more files", which is what DOS reports here. */
            if (!received) { machine->LastError = 18; FCB_FAIL(); }
            else {
                PCSTR baseName = findData.cAlternateFileName[0] ? findData.cAlternateFileName
                                                          : findData.cFileName;
                FILETIME localTime; WORD dosDate = 0, dosTime = 0;
                INT index;
                /* ── AN EXTENDED SEARCH RETURNS AN EXTENDED RESULT. ──────────────
                     We already skip the 7-byte prefix on the way IN (DosFcbAt), and
                     then wrote the answer back in the SHORT layout regardless -- so
                     a caller that searched with an extended FCB read every field
                     seven bytes early. DIR does exactly that (it must, to see
                     directories and the volume label), which is why its listing came
                     out with blank names, one impossible size repeated down the
                     column, and a volume label of "COM" -- the tail of COMMAND.COM
                     read as an 11-byte label.
                     The prefix is FFh, five reserved bytes, then the attribute of
                     the file found; the ordinary result follows it unchanged. */
                INT isExtended = (fcb != (volatile BYTE *)((R_DS << 4) + (R_DX & 0xFFFF))) ? 7 : 0;
                volatile BYTE *extension = dta + isExtended;
                if (FileTimeToLocalFileTime(&findData.ftLastWriteTime, &localTime))
                    FileTimeToDosDateTime(&localTime, &dosDate, &dosTime);
                if (isExtended) {
                    dta[0] = 0xFF;
                    for (index = 1; index <= 5; ++index) dta[index] = 0;
                    dta[6] = (BYTE)(findData.dwFileAttributes & 0x3F);
                }
                extension[0] = 3;                         /* drive C:          */
                DosFcbPutName(extension + 1, baseName);
                extension[12] = (BYTE)(findData.dwFileAttributes & 0x3F);
                for (index = 13; index <= 22; ++index) extension[index] = 0;
                extension[23] = (BYTE)(dosTime & 0xFF); extension[24] = (BYTE)(dosTime >> 8);
                extension[25] = (BYTE)(dosDate & 0xFF); extension[26] = (BYTE)(dosDate >> 8);
                extension[27] = 0; extension[28] = 0;     /* starting cluster  */
                extension[29] = (BYTE)( findData.nFileSizeLow        & 0xFF);
                extension[30] = (BYTE)((findData.nFileSizeLow >> 8)  & 0xFF);
                extension[31] = (BYTE)((findData.nFileSizeLow >> 16) & 0xFF);
                extension[32] = (BYTE)((findData.nFileSizeLow >> 24) & 0xFF);
                FCB_OK();
            }
            fcbDone: ;
        } else if (function == 0x13) {          /* delete (wildcards allowed) */
            WIN32_FIND_DATAA findData; HANDLE find; INT isFound = 0;
            DosFcbName(fcb, name);
            find = FindFirstFileA(name, &findData);
            if (find != INVALID_HANDLE_VALUE) {
                do {
                    if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                    if (DeleteFileA(findData.cFileName)) isFound = 1;
                } while (FindNextFileA(find, &findData));
                FindClose(find);
            }
            if (isFound) FCB_OK(); else FCB_FAIL();
        } else if (function == 0x17) {          /* rename: new name at f[17..27] */
            CHAR destinationPath[300];
            CHAR saved[16]; INT index;
            DosFcbName(fcb, name);
            for (index = 0; index < 12; ++index) saved[index] = (CHAR)fcb[index];
            { volatile BYTE temporary[12]; temporary[0] = fcb[0];
              for (index = 1; index < 12; ++index) temporary[index] = fcb[16 + index];
              DosFcbName(temporary, destinationPath); }
            if (MoveFileA(name, destinationPath)) FCB_OK(); else FCB_FAIL();
            (VOID)saved;
        } else if (function == 0x14 || function == 0x15 || function == 0x21 || function == 0x22
                   || function == 0x27 || function == 0x28) {  /* record I/O */
            volatile BYTE *dta = (volatile BYTE *)((machine->DtaSegment << 4) + machine->DtaOffset);
            DWORD recordSize = (DWORD)fcb[14] | ((DWORD)fcb[15] << 8);
            DWORD block   = (DWORD)fcb[12] | ((DWORD)fcb[13] << 8);
            DWORD record, count = 1, done = 0, index;
            BYTE buffer[512];
            if (!recordSize) recordSize = 128;
            if (recordSize > sizeof(buffer)) recordSize = sizeof(buffer);
            if (function == 0x14 || function == 0x15) record = block * 128 + fcb[32];
            else record = (DWORD)fcb[33] | ((DWORD)fcb[34] << 8)
                     | ((DWORD)fcb[35] << 16) | ((DWORD)fcb[36] << 24);
            if (function == 0x27 || function == 0x28) count = R_CX & 0xFFFF;
            if (fcb[24] != DOS_FCB_MAGIC || fcb[25] >= DOS_MAX_FILES || !machine->FileHandles[fcb[25]]) SETAX((R_AX & 0xFF00) | 1);
            else {
                HANDLE file = machine->FileHandles[fcb[25]];
                DWORD length = 0;
                SetFilePointer(file, (LONG)(record * recordSize), NULL, FILE_BEGIN);
                for (index = 0; index < count; ++index) {
                    if (function == 0x14 || function == 0x21 || function == 0x27) {
                        DWORD index2;
                        if (!ReadFile(file, buffer, recordSize, &length, NULL) || length == 0) break;
                        for (index2 = 0; index2 < recordSize; ++index2)
                            dta[done * recordSize + index2] = (index2 < length) ? buffer[index2] : 0;
                        ++done;
                        if (length < recordSize) break;
                    } else {
                        DWORD index2;
                        for (index2 = 0; index2 < recordSize; ++index2) buffer[index2] = dta[done * recordSize + index2];
                        if (!WriteFile(file, buffer, recordSize, &length, NULL)) break;
                        ++done;
                    }
                }
                if (function == 0x27 || function == 0x28) SET16(R_CX, (WORD)done);
                if (done && !(function == 0x14 || function == 0x21 || function == 0x27))
                    DosStampVdmNow(file);               /* #263: an FCB write */
                /* AL: 0 = all done, 1 = end of file / nothing transferred,
                   3 = a partial final record. */
                if (done == count) SETAX((R_AX & 0xFF00) | 0);
                else if (!done)    SETAX((R_AX & 0xFF00) | 1);
                else               SETAX((R_AX & 0xFF00) | 3);
                if (function == 0x14 || function == 0x15) {  /* advance sequentially */
                    DWORD nextRecord = record + done;
                    fcb[12] = (BYTE)((nextRecord / 128) & 0xFF); fcb[13] = (BYTE)((nextRecord / 128) >> 8);
                    fcb[32] = (BYTE)(nextRecord % 128);
                } else {
                    DWORD nextRecord = record + done;
                    fcb[33] = (BYTE)(nextRecord & 0xFF);         fcb[34] = (BYTE)((nextRecord >> 8) & 0xFF);
                    fcb[35] = (BYTE)((nextRecord >> 16) & 0xFF); fcb[36] = (BYTE)((nextRecord >> 24) & 0xFF);
                }
            }
        } else if (function == 0x23) {          /* get file size, in records */
            HANDLE file;
            DosFcbName(fcb, name);
            file = CreateFileA(name, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (file == INVALID_HANDLE_VALUE) FCB_FAIL();
            else {
                DWORD size = GetFileSize(file, NULL);
                DWORD recordSize = (DWORD)fcb[14] | ((DWORD)fcb[15] << 8);
                DWORD records;
                CloseHandle(file);
                if (!recordSize) recordSize = 128;
                records = (size + recordSize - 1) / recordSize;
                fcb[33] = (BYTE)(records & 0xFF);         fcb[34] = (BYTE)((records >> 8) & 0xFF);
                fcb[35] = (BYTE)((records >> 16) & 0xFF); fcb[36] = (BYTE)((records >> 24) & 0xFF);
                FCB_OK();
            }
        } else if (function == 0x24) {          /* set random record from current */
            DWORD nextRecord = ((DWORD)fcb[12] | ((DWORD)fcb[13] << 8)) * 128 + fcb[32];
            fcb[33] = (BYTE)(nextRecord & 0xFF);         fcb[34] = (BYTE)((nextRecord >> 8) & 0xFF);
            fcb[35] = (BYTE)((nextRecord >> 16) & 0xFF); fcb[36] = (BYTE)((nextRecord >> 24) & 0xFF);
            OKCF();
        } else if (function == 0x29) {          /* parse a filename into an FCB */
            CHAR input[300];
            volatile BYTE *destination = (volatile BYTE *)((R_ES << 4) + (R_DI & 0xFFFF));
            INT inputIndex = 0, isWild = 0, index;
            /* ── ★ AL's CONTROL BITS SAY WHAT A MISSING PART LEAVES ALONE. (s81 sweep) ──
                 bit 1: no drive given -> keep the FCB's drive (else 0 = default)
                 bit 2: no name given  -> keep the FCB's name
                 bit 3: no extension   -> keep the FCB's extension
                 Unanimous on msdos622 / dosbox-x / pcem / pcem-vesa (p_fcb.asm
                 int21.29.keepext/keepall/blank). XP's COMMAND.COM builds DIR's search
                 FCB as ??????????? and parses "*" with AL=0Eh; blanking the extension
                 regardless made the template ????????+3 spaces and DIR listed only
                 `.` and `..` -- the user's sweep finding. */
            BYTE subfunction = (BYTE)(R_AX & 0xFF);
            BYTE kept[12]; INT index2, hasName, hasExtension;
            for (index = 0; index < 12; ++index) kept[index] = destination[index];
            DosGuestString(R_DS, R_SI, input, sizeof(input));
            while (input[inputIndex] == ' ' || input[inputIndex] == 9) ++inputIndex;
            destination[0] = 0;
            if (input[inputIndex] && input[inputIndex + 1] == ':') {
                CHAR character = input[inputIndex];
                destination[0] = (BYTE)((character >= 'a' ? character - 32 : character) - 'A' + 1);
                inputIndex += 2;
            } else if (subfunction & 0x02) destination[0] = kept[0];
            for (index2 = inputIndex; !DosFcbIsNameEnd((BYTE)input[index2]) && input[index2] != '.'; ++index2) {}
            hasName = (index2 > inputIndex);
            hasExtension  = (input[index2] == '.');
            DosFcbPutName(destination + 1, input + inputIndex);
            if (!hasName && (subfunction & 0x04)) for (index = 1; index <= 8;  ++index) destination[index] = kept[index];
            if (!hasExtension  && (subfunction & 0x08)) for (index = 9; index <= 11; ++index) destination[index] = kept[index];
            for (index = 1; index <= 11; ++index) if (destination[index] == '?' || destination[index] == '*') isWild = 1;
            for (index = 12; index <= 15; ++index) destination[index] = 0;
            SETAX((R_AX & 0xFF00) | (isWild ? 1 : 0));
            SET16(R_SI, (WORD)((R_SI & 0xFFFF) + inputIndex));
            OKCF();
            /* THE CALL COMMAND.COM'S DISPATCH TURNS ON. `ver ` runs and `ver` does
               not, and the traces diverge on the instruction after the third of
               these -- so print what went in and what came out, both. Reasoning
               about it from the handler's source has already produced two wrong
               models this session. */
            if (machine->IsTraceAll) { INT position;
              trace = zput(trace, "  INT21 AH=29 al="); trace = zhexb(trace, (UINT)(R_AX & 0xFF));
              trace = zput(trace, " ds:si="); trace = zhexb(trace, (UINT)((R_DS >> 8) & 0xFF));
              trace = zhexb(trace, (UINT)(R_DS & 0xFF)); trace = zput(trace, ":");
              trace = zhexb(trace, (UINT)(((R_SI & 0xFFFF) >> 8) & 0xFF));
              trace = zhexb(trace, (UINT)(R_SI & 0xFF));
              trace = zput(trace, " in=[");
              for (position = 0; position < 12 && input[position]; ++position) trace = zhexb(trace, (UINT)(BYTE)input[position]), trace = zput(trace, " ");
              trace = zput(trace, "] fcb=[");
              for (position = 0; position < 12; ++position) trace = zhexb(trace, (UINT)destination[position]), trace = zput(trace, " ");
              trace = zput(trace, "]\r\n"); }
        } else FCB_FAIL();
        #undef FCB_OK
        #undef FCB_FAIL
    } else if (function == 0x4B) {              /* EXEC: load and run a program */
        BYTE subfunction = (BYTE)(R_AX & 0xFF);
        if (subfunction == 0x00 || subfunction == 0x01) {
            const volatile BYTE *parameterBlock =
                (const volatile BYTE *)((R_ES << 4) + (R_BX & 0xFFFF));
            /* AL=01 answers THROUGH this block, so remember where it is. */
            machine->ExecBlockSegment = (WORD)(R_ES & 0xFFFF);
            machine->ExecBlockOffset = (WORD)(R_BX & 0xFFFF);
            DosGuestPath(machine, R_DS, R_DX, machine->ExecPath, sizeof(machine->ExecPath));
            DosGuestString(R_DS, R_DX, machine->ExecName, sizeof(machine->ExecName));
            machine->ExecEnvironment      = (WORD)(parameterBlock[0] | (parameterBlock[1] << 8));
            machine->ExecTailOffset = (WORD)(parameterBlock[2] | (parameterBlock[3] << 8));
            machine->ExecTailSegment = (WORD)(parameterBlock[4] | (parameterBlock[5] << 8));
            machine->ExecFcb1Offset = (WORD)(parameterBlock[6] | (parameterBlock[7] << 8));
            machine->ExecFcb1Segment = (WORD)(parameterBlock[8] | (parameterBlock[9] << 8));
            machine->ExecFcb2Offset = (WORD)(parameterBlock[10] | (parameterBlock[11] << 8));
            machine->ExecFcb2Segment = (WORD)(parameterBlock[12] | (parameterBlock[13] << 8));
            machine->ExecMode = subfunction;
            machine->IsExecPending = 1;         /* the host does the rest */
            OKCF();
        } else if (subfunction == 0x03) {
            /* ── THE OVERLAY. Two words of parameter block and nothing else:
                 where to put it, and what to relocate by -- and those are NOT
                 the same number (oracle: relocation uses the FACTOR, measured
                 with a factor deliberately unequal to the load segment). No PSP,
                 no allocation, no transfer of control, so the host's normal EXEC
                 path is wrong for it and it branches early. (GH #50) */
            const volatile BYTE *parameterBlock =
                (const volatile BYTE *)((R_ES << 4) + (R_BX & 0xFFFF));
            DosGuestPath(machine, R_DS, R_DX, machine->ExecPath, sizeof(machine->ExecPath));
            machine->ExecOverlaySegment   = (WORD)(parameterBlock[0] | (parameterBlock[1] << 8));
            machine->ExecOverlayRelocation = (WORD)(parameterBlock[2] | (parameterBlock[3] << 8));
            machine->ExecMode = 0x03;
            machine->IsExecPending = 1;
            OKCF();
        } else if (subfunction == 0x05) {
            /* ── SET EXECUTION STATE (#165). The second half of a loader's own EXEC:
                 AX=4B01h loaded the program, the loader did its own work, and this
                 tells DOS control is about to go to it. Measured (p_4b05, 6.22 and
                 PCem agree): AX=0000 CF=0, and the CURRENT PSP IS NOT CHANGED -- 4B01h
                 already switched it. DOS uses the block for SETVER's per-program
                 version; we keep no SETVER table, so there is nothing else to do.
                 DOSBox-X refuses it (CF=1 AX=000B): an emulator without the call,
                 not a different DOS. */
            trace = zput(trace, "  INT21 AX=4B05 set execution state -- accepted\r\n");
            SETAX(0); OKCF();
        } else {
            /* AL=02/04 are not DOS 6.22 functions we have measured. */
            trace = zput(trace, "  INT21 AH=4B AL=0x"); trace = zhexb(trace, subfunction);
            trace = zput(trace, " UNIMPLEMENTED (overlay load)\r\n");
            machine->Unimplemented[0x4B >> 3] |= (BYTE)(1u << (0x4B & 7));
            SETAX(1); ERRCF();
        }
    } else if (function == 0x1B || function == 0x1C) {  /* allocation info for a drive */
        /* AL=sectors/cluster, DS:BX -> media descriptor byte, CX=bytes/sector,
           DX=total clusters.  All disk geometry, so the probe compares only CF.
           NOTE this call RETURNS A SEGMENT IN DS -- which is what broke the
           probe's own output until probe_capture learned to restore it. */
        DWORD sectorsPerCluster = 0, bytesPerSector = 0, freeClusters = 0, totalClusters = 0;
        CHAR root[4]; PSTR rootPointer = 0;
        BYTE driveNumber = (function == 0x1C) ? (BYTE)(R_DX & 0xFF) : 0;  /* 1Bh: default drive */
        if (!driveNumber && machine->VirtualDrive >= 0) driveNumber = (BYTE)(machine->VirtualDrive + 1);
        if (driveNumber) { root[0] = (CHAR)('A' + driveNumber - 1); root[1] = ':';
                    root[2] = '\\'; root[3] = 0; rootPointer = root; }
        if (GetDiskFreeSpaceA(rootPointer, &sectorsPerCluster, &bytesPerSector, &freeClusters, &totalClusters)) {
            volatile BYTE *media = (volatile BYTE *)((DOS_CTAB_SEG << 4) + DOS_MEDIA_OFF);
            *media = 0xF8;                       /* fixed disk */
            SETAX((R_AX & 0xFF00) | (sectorsPerCluster & 0xFF));
            SET16(R_DS, DOS_CTAB_SEG); SET16(R_BX, DOS_MEDIA_OFF);
            SET16(R_CX, (WORD)bytesPerSector);
            SET16(R_DX, totalClusters > 0xFFFF ? 0xFFFF : totalClusters);
            OKCF();
        } else { SETAX((R_AX & 0xFF00) | 0xFF); ERRCF(); }
    } else if (function == 0x1F || function == 0x32) {  /* get drive parameter block */
        /* DPB contents are disk geometry and its address is host-specific; AL is
           the comparable part -- 00 for a valid drive, FF otherwise (measured). */
        BYTE driveNumber = (function == 0x1F) ? 0 : (BYTE)(R_DX & 0xFF);
        DWORD sectorsPerCluster = 0, bytesPerSector = 0, freeClusters = 0, totalClusters = 0;
        CHAR root[4]; PSTR rootPointer = 0;
        if (driveNumber) { root[0] = (CHAR)('A' + driveNumber - 1); root[1] = ':';
                    root[2] = '\\'; root[3] = 0; rootPointer = root; }
        if (driveNumber > 26 || !GetDiskFreeSpaceA(rootPointer, &sectorsPerCluster, &bytesPerSector, &freeClusters, &totalClusters)) {
            SETAX((R_AX & 0xFF00) | 0xFF);
        } else {
            /* ── #48: THE SAME BUILDER AS THE AH=52h CHAIN, so the two DPBs a program
                 can reach for one drive describe one volume. This copy had its own
                 inline fields: FAT sectors, root start and data start ZERO (a volume
                 whose files overlap its FAT), a highest-cluster word that wrapped past
                 0xFFFF where the chain's clamps at 0xFFFE, and unit 0 where the chain
                 says unit = drive (6.22: one IO.SYS driver, A: unit 0 .. C: unit 2). DosDpbFatLayout's
                 note in dos_sysvars.h says what the derived fields are and are not.
               ⚠ Still a separate copy at DOS_DPB_OFF rather than a pointer INTO the
                 chain (6.22 returns the chain's own DPB); that is a pointer change for a
                 later pass, and the probe compares only AL here. */
            volatile BYTE *dpbBytes = (volatile BYTE *)((DOS_CTAB_SEG << 4) + DOS_DPB_OFF);
            BYTE dpb[DOS_DPB_LEN];
            UINT drive = driveNumber ? (UINT)(driveNumber - 1) : 2u;  /* 0-based drive */
            CHAR rootText[4]; INT index, remaining;
            rootText[0] = (CHAR)('A' + drive); rootText[1] = ':'; rootText[2] = '\\'; rootText[3] = 0;
            remaining = (GetDriveTypeA(rootText) == DRIVE_REMOVABLE);  /* 6.22's floppy: 224, F0h */
            DosDpbBuild(dpb, drive, bytesPerSector ? bytesPerSector : 512, sectorsPerCluster ? sectorsPerCluster : 1, remaining ? 224 : 512,
                          (totalClusters > 0xFFFE) ? 0xFFFE : totalClusters + 1, remaining ? 0xF0 : 0xF8,
                          DOS_DEV_SEG, DOS_DEVICE_OFFSET(DOS_DEVICE_BLOCK), 0xFFFF, 0xFFFF);
            for (index = 0; index < DOS_DPB_LEN; ++index) dpbBytes[index] = dpb[index];
            SET16(R_DS, DOS_CTAB_SEG); SET16(R_BX, DOS_DPB_OFF);
            SETAX(R_AX & 0xFF00);
        }
        OKCF();
    } else if (function == 0x37) {              /* get/set the SWITCH character */
        BYTE subfunction = (BYTE)(R_AX & 0xFF);
        if (subfunction == 0x00) {              /* oracle: DL = '/' */
            SET16(R_DX, (WORD)((R_DX & 0xFF00) | machine->SwitchChar));
            SETAX(R_AX & 0xFF00); OKCF();
        } else if (subfunction == 0x01) {
            machine->SwitchChar = (BYTE)(R_DX & 0xFF);
            SETAX(R_AX & 0xFF00); OKCF();
        } else { SETAX((R_AX & 0xFF00) | 0xFF); OKCF(); }
    } else if (function == 0x66) {              /* get/set global code page */
        BYTE subfunction = (BYTE)(R_AX & 0xFF);
        if (subfunction == 0x01) {              /* oracle: BX=DX=437 */
            SET16(R_BX, 437); SET16(R_DX, 437); OKCF();
        } else if (subfunction == 0x02) {
            OKCF();                             /* accept; we have only 437 */
        } else { SETAX(1); ERRCF(); }
    } else if (function == 0x26 || function == 0x55) {  /* create a PSP / child PSP */
        /* Copy our PSP to the segment in DX and fix up the fields that must
           differ. 55h additionally takes the child's memory top in SI. */
        volatile BYTE *source = (volatile BYTE *)(DOS_PSP_SEG << 4);
        volatile BYTE *destination = (volatile BYTE *)((R_DX & 0xFFFF) << 4);
        INT index;
        for (index = 0; index < 256; ++index) destination[index] = source[index];
        destination[0x16] = (BYTE)(DOS_PSP_SEG & 0xFF);  /* parent PSP segment */
        destination[0x17] = (BYTE)(DOS_PSP_SEG >> 8);
        if (function == 0x55) {
            destination[0x02] = (BYTE)(R_SI & 0xFF);  /* memory top          */
            destination[0x03] = (BYTE)((R_SI >> 8) & 0xFF);
        }
        OKCF();
    } else if (function == 0x31) {              /* terminate and stay resident */
        /* ── STAY RESIDENT. (GH #49) ──────────────────────────────────────────
             DX is the paragraph count to KEEP, counted from the PSP. Residency
             is three things, and the host does all three on this flag:
               the memory block is RESIZED, not freed;
               the interrupt vectors it installed are NOT unwound; and
               control returns to whatever EXEC'd it, with the image intact.
           ⚠ AT DEPTH 0 THERE IS NOTHING TO RETURN TO. A top-level program that
             TSRs has no parent inside this VDM, so the run ends either way --
             but say which, because "resident" and "exited" look identical in a
             log and a TSR that believes it installed and did not is exactly the
             silent failure #27 exists to remove. */
        machine->TsrKeep = (WORD)(R_DX & 0xFFFF);
        machine->IsTsrPending = 1;
        trace = zput(trace, "  INT21 AH=31 TSR: keep 0x"); trace = zhex(trace, machine->TsrKeep);
        trace = zput(trace, " paragraphs, vectors LEFT INSTALLED\r\n");
        machine->ExitCode = (INT)(R_AX & 0xFF);
        shouldContinue = 0;
    } else if (function == 0x53) {              /* translate a BPB into a DPB */
        /* ⚠ TESTED AS THE CAUSE OF THE COMMAND.COM EXIT, AND REFUTED. XP's own
             COMMAND.COM calls this during init and terminates shortly after,
             printing nothing, and this was the ONLY unimplemented call in the whole
             run -- which made it the tempting answer rather than the proven one.
             Answering SUCCESS with a zeroed DPB was tried: the shell still exits, at
             the SAME CS:IP (0x95eb:0x03ce), after the same 32 ms. So 53h is not what
             stops it, and reporting success here would have bought nothing at the
             price of a call that lies. Reverted deliberately.
             What COMMAND.COM asks for and does not get is INT 2Fh AX=122Eh, the five
             DOS error-message table addresses (DL=00/02/04/06/08) -- see the widened
             BOP2F log. That is the next thing to chase, and "died after" is still not
             "died because": prove it before implementing it. */
        /* ⛔ ONE LINE PER CALL, AND A LOOPING GUEST TURNS THAT INTO A FILE. XP's
             COMMAND.COM re-runs its init forever while `sub 01` answers "no command",
             and this note alone was 2,713 lines in the last 200 KB of a 42 MB log.
             Say it once; `unimpl21[]` already carries the fact for the summary. */
        { static INT isSaid = 0;
          if (!isSaid) { isSaid = 1;
              trace = zput(trace, "  INT21 AH=53 BPB->DPB UNIMPLEMENTED (no installable "
                            "block drivers) -- said once per run\r\n"); } }
        machine->Unimplemented[0x53 >> 3] |= (BYTE)(1u << (0x53 & 7));
        /* ── ★★★ MEASURED AGAINST STOCK NTVDM, 2026-09-25. ───────────────────
             Documented AH=53h is BPB->DPB and takes DS:SI / ES:BP with NO AL
             sub-function. XP's COMMAND.COM uses it as a PRIVATE QUERY with AL as a
             selector and reads the answer out of AL -- AL=5 and AL=7 at start-up,
             AL=2 elsewhere (the INT 21h trace). NTDOS.SYS is the only implementation,
             so stock ntvdm on the rig is the only oracle --
             `debug\rig\dosstock.bat P_INT53.COM`, which
             drops the IFEO key and PROVES it back. Real MS-DOS cannot be asked: the
             documented form BUILDS a DPB from a caller-supplied BPB, and a fabricated
             pointer HANGS 6.22 (measured twice).

               AL=00 -> AX=0005 CF=0     AL=04 -> AX=5300 CF=0
               AL=01 -> AX=0001 CF=1     AL=05 -> AX=5301 CF=0
               AL=02 -> AX=5300 CF=0     AL=06 -> AX=5300 CF=0
               AL=03 -> AX=0001 CF=1     AL=07 -> AX=5301 CF=0

             ⇒ AH is preserved and AL carries a 0/1 answer for 02/04/05/06/07; 01 and
               03 are genuinely unsupported (AX=1, CF=1 -- DOS's "invalid function").

           ⛔⛔ AND THE ORIGINAL "UNIMPLEMENTED" WAS ACCIDENTALLY RIGHT. It returned
             AX=1, i.e. AL=1 -- exactly what stock returns for AL=5 and AL=7, the two
             COMMAND.COM keeps. I then "fixed" it to AX=0 on the theory that AL=5
             answering 1 was blocking the interactive path, and made it WRONG --
             against the only measurement there is. Reverted, and marked provisional, which is the only
             reason that was a correction rather than a fact. **Guessing a value for a
             private call is not cheaper than measuring it; it is the same work twice.**

           ★★★ AND THE THEORY IT WAS REVERTED WITH IS NOW DISPROVED (s79). The revert
             carried a second claim -- "stock answers AL=5 with 1 and IS interactive,
             so that answer does not gate the prompt" -- which was an INFERENCE from
             the standalone probe, not an observation. In fact XP's COMMAND.COM reads
             its command line from the keyboard only while its AL=5 answer is 0, and
             nothing else it does changes that. Stock IS interactive. Therefore, IN THE SHELL'S CONTEXT, stock's
             AX=5305h returns AL=0 -- and our probe measured AL=1.

           ⇒ **AL=5 IS CONTEXT-DEPENDENT, and the 8/8 "agreement" is an agreement about
             the context we measured in.** The prime suspect is the measurement rig
             itself: probe.inc reports through INT 21h AH=02 and every stock run is
             captured with `> file`, so the oracle was asked "are you interactive?"
             with its own output redirected. `tests/probes/dos/p_int53f.asm` asks the same
             eight questions but writes its answers through AH=3Ch/40h, so it can be run
             with NOTHING redirected.

           ⚠ AL=00's row was measured with SI=0 and BP=0, as COMMAND.COM issues it. It
             is NOT a claim about the documented BPB->DPB call given a real BPB, which we
             still do not implement. */
        { BYTE subfunction = (BYTE)(R_AX & 0xFF);
          if (subfunction < DOS_INT53_COUNT) {
              SETAX(g_DosInt53Answers[subfunction].Ax);
              if (g_DosInt53Answers[subfunction].IsCarry) ERRCF(); else OKCF();
          } else { SETAX(0x0001); ERRCF(); }
        }
    } else if (function == 0x5E) {              /* network machine name / printer */
        BYTE subfunction = (BYTE)(R_AX & 0xFF);
        if (subfunction == 0x00) {              /* oracle: AX=0, CF=0 */
            volatile BYTE *buffer = (volatile BYTE *)((R_DS << 4) + (R_DX & 0xFFFF));
            INT index; for (index = 0; index < 16; ++index) buffer[index] = 0;
            SETAX(0); SET16(R_CX, 0); OKCF();
        } else { SETAX(1); ERRCF(); }
    } else if (function == 0x5F) {              /* network redirection list */
        trace = zput(trace, "  INT21 AH=5F network redirection: no redirector present\r\n");
        SETAX(1); ERRCF();                      /* invalid function */
    } else if (function == 0x64) {              /* set device driver lookahead */
        OKCF();                                 /* internal; accepted, no effect */
    } else if (function == 0x03 || function == 0x04 || function == 0x05) {  /* AUX in / AUX out / PRN out */
        /* ── #251: THESE WENT NOWHERE -- "accepted and discarded", and AUX input
             answered ^Z -- while COM1 and an LPT1 spool both exist. On DOS they are
             the AUX and PRN drivers, which call INT 14h / INT 17h through the IVT
             (p_auxprn logs the exact sequence 6.22 makes), so in V86 the guest is
             resumed in that driver code. In PM the bytes go to the same devices
             directly, and AUX input stays ^Z (no wait loop to run it in). */
        if (AUXPRN_V86)
            AUXPRN_TRAMP(function == 0x05 ? DOS_AUXPRN_PRN_OUTPUT : function == 0x04 ? DOS_AUXPRN_AUX_OUTPUT : DOS_AUXPRN_AUX_INPUT);
        else if (function == 0x03) {
            SETAX((R_AX & 0xFF00) | 0x1A);
            OKCF();
        } else {
            BYTE character = (BYTE)(R_DX & 0xFF);
            if (function == 0x05) { if (machine->PrinterOut) (VOID)machine->PrinterOut(machine->DeviceContext, character); }
            else if (machine->AuxOut) machine->AuxOut(machine->DeviceContext, character);
            SETAX((R_AX & 0xFF00) | character);  /* oracle: AL = the byte sent */
            OKCF();
        }
    } else if (function == 0x0C) {              /* flush input, then run AL     */
        /* AL names the input function to perform after flushing. Anything else
           is just a flush. Re-dispatching is the whole point of the call. */
        BYTE inputFunction = (BYTE)(R_AX & 0xFF);
        while (machine->ConsolePeek && machine->ConsolePeek(machine->ConsoleInContext) && machine->ConsoleInNoWait)
            (VOID)machine->ConsoleInNoWait(machine->ConsoleInContext);
        if (inputFunction == 0x01 || inputFunction == 0x06 || inputFunction == 0x07 || inputFunction == 0x08 || inputFunction == 0x0A) {
            SETAX((WORD)(inputFunction << 8));
            machine->IsRetry = 1;               /* re-enter with AH = that fn  */
        } else OKCF();
    } else if (function == 0x2E) {              /* set verify flag */
        machine->IsVerifyOn = (BYTE)(R_AX & 0xFF); OKCF();
    } else if (function == 0x54) {              /* get verify flag */
        SETAX((R_AX & 0xFF00) | machine->IsVerifyOn); OKCF();
    } else if (function == 0x34) {              /* get InDOS flag -> ES:BX */
        SET16(R_ES, DOS_SDA_SEG); SET16(R_BX, DOS_INDOS_OFF); OKCF();
    } else if (function == 0x5D && ((R_AX & 0xFF) == 0x08 || (R_AX & 0xFF) == 0x09)) {
        /* 5D08h/5D09h set and flush the network redirector's sharing retry
           counts. COMMAND.COM calls both at startup. There is no redirector
           here, so accept and ignore -- that is what DOS does on a machine with
           no network, and refusing would make the shell think something failed. */
        OKCF();
    } else if (function == 0x5D && (R_AX & 0xFF) == 0x06) {  /* get swappable data area */
        SET16(R_DS, DOS_SDA_SEG); SET16(R_SI, DOS_SDA_OFF);
        SET16(R_CX, DOS_SDA_LEN); SET16(R_DX, DOS_SDA_LEN);
        trace = zput(trace, "  INT21 AH=5D06 SDA (minimal: crit-err + InDOS only)\r\n");
        OKCF();
    } else if (function == 0x39 || function == 0x3A) {  /* mkdir / rmdir */
        CHAR fileName[300];
        INT isOk;
        DosGuestPath(machine, R_DS, R_DX, fileName, sizeof(fileName));
        isOk = (function == 0x39) ? (INT)CreateDirectoryA(fileName, NULL)
                           : (INT)RemoveDirectoryA(fileName);
        /* s91: A SEARCH THE GUEST NEVER FINISHED KEEPS THE DIRECTORY OPEN. DOS has no
             FindClose, so an AH=4Eh/11h search that stopped before "no more files"
             leaves our FindFirstFile handle alive -- and Windows will not remove a
             directory with a search open in it. 6.22's COMMAND.COM searches inside
             a directory on `cd`, and `rmdir` of that (empty) directory then failed
             with "Invalid path, not directory, or directory not empty" (runs/s91,
             chain11b). Close the guest's unfinished searches and try once more. */
        if (!isOk && function == 0x3A) {
            INT findSlot;
            for (findSlot = 0; findSlot < 8; ++findSlot)
                if (machine->FindHandles[findSlot]) { FindClose(machine->FindHandles[findSlot]); machine->FindHandles[findSlot] = 0; }
            if (machine->FcbFind) { FindClose(machine->FcbFind); machine->FcbFind = 0; }
            isOk = (INT)RemoveDirectoryA(fileName);
        }
        if (isOk) OKCF();
        else {
            /* Oracle: mkdir over an existing name is 5 (access denied); rmdir of
               something absent is 3 (path not found). */
            DWORD error = GetLastError();
            SETAX((WORD)(error == ERROR_ALREADY_EXISTS ? 5
                           : error == ERROR_PATH_NOT_FOUND ? 3
                           : error == ERROR_FILE_NOT_FOUND ? 3 : 5));
            ERRCF();
        }
    } else if (function == 0x41) {              /* delete file */
        CHAR fileName[300];
        DosGuestPath(machine, R_DS, R_DX, fileName, sizeof(fileName));
        if (DeleteFileA(fileName)) OKCF();
        else { SETAX(2); ERRCF(); }             /* oracle: absent -> AX=2 */
    } else if (function == 0x43) {              /* get/set file attributes */
        CHAR fileName[300];
        BYTE subfunction = (BYTE)(R_AX & 0xFF);
        DosGuestPath(machine, R_DS, R_DX, fileName, sizeof(fileName));
        if (subfunction == 0x00) {
            DWORD attributes = GetFileAttributesA(fileName);
            if (attributes == 0xFFFFFFFFu) { SETAX(2); ERRCF(); }
            else { SET16(R_CX, (WORD)(attributes & 0x3F)); SETAX((WORD)(attributes & 0x3F)); OKCF(); }
        } else if (subfunction == 0x01) {
            DWORD attributes = (DWORD)(R_CX & 0x3F);
            if (!attributes) attributes = FILE_ATTRIBUTE_NORMAL;
            if (SetFileAttributesA(fileName, attributes)) OKCF();
            else { SETAX(2); ERRCF(); }
        } else {
            trace = zput(trace, "  INT21 AH=43 AL=0x"); trace = zhexb(trace, subfunction);
            /* Not a gap: 6.22 and PCem answer AX=1 CF=1 too (p_subfn int21.4302). */
            trace = zput(trace, " not a 6.22 subfunction -> AX=1 CF=1 (matches DOS)\r\n");
            SETAX(1); ERRCF();
        }
    } else if (function == 0x45 || function == 0x46) {  /* dup / dup2 */
        /* ── ★ A DEVICE IS DUPLICABLE, AND THAT IS THE WHOLE POINT OF 45h. ────
             This refused anything that was not a FILE, so `dup(1)` -- the first
             step of every save-redirect-restore sequence a shell performs --
             came back error 6 and the restore could never happen. Found by
             running tests/probes/dos/p_redir.asm on the rig against the same probe
             on the oracle; the two disagreed on one line:
               oracle : CASE=int21.45.dup.stdout AX=0005 CF=0
               NTVDMEX: CASE=int21.45.dup.stdout AX=0006 CF=1
             The failure was not even visible as itself: the probe's LATER output
             vanished into the test file, because with stdout never restored the
             next open took handle 1. A wrong answer that eats the evidence of
             itself is why this is measured against an oracle rather than read.
           Duplicating a device produces another handle ON THAT DEVICE -- no
           Win32 handle exists to duplicate, so the copy is a device slot too. */
        DWORD sourceHandle = R_BX & 0xFFFF, targetHandle;
        INT isSourceDevice = DosHandleIsDevice((PVOID const *)machine->FileHandles, machine->StdOpen, sourceHandle);
        if (!isSourceDevice && !DosHandleIsFile((PVOID const *)machine->FileHandles, sourceHandle)) { SETAX(6); ERRCF(); }
        else {
            HANDLE newHandle = 0;
            if (!isSourceDevice
                && !DuplicateHandle(GetCurrentProcess(), machine->FileHandles[sourceHandle],
                                    GetCurrentProcess(), &newHandle, 0, FALSE,
                                    DUPLICATE_SAME_ACCESS)) { SETAX(6); ERRCF(); }
            else if (function == 0x45) {
                targetHandle = DosHandleAllocate((PVOID const *)machine->FileHandles, machine->StdOpen);
                if (targetHandle >= DOS_MAX_FILES) {
                    if (newHandle) CloseHandle(newHandle); SETAX(4); ERRCF();
                } else if (isSourceDevice && !DosHandleSetDevice(&machine->StdOpen, targetHandle, 1)) {
                    /* Past the device mask. Refuse LOUDLY rather than hand back a
                       slot that would read as a file -- see DOS_DEV_SLOTS. */
                    trace = zput(trace, "  INT21 AH=45 device dup past slot 0x");
                    trace = zhex(trace, DOS_DEV_SLOTS); trace = zput(trace, " -- refused\r\n");
                    SETAX(4); ERRCF();
                } else { machine->FileHandles[targetHandle] = newHandle; SETAX(targetHandle); OKCF(); }
            } else {
                targetHandle = R_CX & 0xFFFF;
                if (targetHandle >= DOS_MAX_FILES) { if (newHandle) CloseHandle(newHandle); SETAX(6); ERRCF(); }
                else if (isSourceDevice && !DosHandleSetDevice(&machine->StdOpen, targetHandle, 1)) {
                    trace = zput(trace, "  INT21 AH=46 device dup2 past slot 0x");
                    trace = zhex(trace, DOS_DEV_SLOTS); trace = zput(trace, " -- refused\r\n");
                    SETAX(4); ERRCF();
                }
                else { DosHandleRelease(machine, targetHandle);
                       machine->FileHandles[targetHandle] = newHandle;  /* 0 when src is a device */
                       if (!isSourceDevice) DosHandleSetDevice(&machine->StdOpen, targetHandle, 0);
                       OKCF(); }
            }
        }
    } else if (function == 0x4D) {              /* get child return code */
        SETAX(machine->ChildReturnCode); machine->ChildReturnCode = 0;  /* DOS clears it after reading */
        OKCF();
    } else if (function == 0x56) {              /* rename: DS:DX -> ES:DI */
        CHAR sourcePath[300], destinationPath[300];
        DosGuestPath(machine, R_DS, R_DX, sourcePath, sizeof(sourcePath));
        DosGuestPath(machine, R_ES, R_DI, destinationPath,   sizeof(destinationPath));
        if (MoveFileA(sourcePath, destinationPath)) OKCF();
        else { DWORD error = GetLastError();
               SETAX((WORD)(error == ERROR_ALREADY_EXISTS ? 5 : 2)); ERRCF(); }
    } else if (function == 0x57) {              /* get/set file date and time */
        DWORD handle = R_BX & 0xFFFF;
        BYTE subfunction = (BYTE)(R_AX & 0xFF);
        if (!DosHandleIsFile((PVOID const *)machine->FileHandles, handle)) { SETAX(6); ERRCF(); }
        else if (subfunction == 0x00) {
            FILETIME fileTime, localTime; WORD dosDate = 0, dosTime = 0;
            if (GetFileTime(machine->FileHandles[handle], NULL, NULL, &fileTime)
                && FileTimeToLocalFileTime(&fileTime, &localTime)
                && FileTimeToDosDateTime(&localTime, &dosDate, &dosTime)) {
                SET16(R_CX, dosTime); SET16(R_DX, dosDate); OKCF();
            } else { SETAX(6); ERRCF(); }
        } else if (subfunction == 0x01) {
            FILETIME fileTime, localTime;
            if (DosDateTimeToFileTime((WORD)(R_DX & 0xFFFF), (WORD)(R_CX & 0xFFFF), &localTime)
                && LocalFileTimeToFileTime(&localTime, &fileTime)
                && SetFileTime(machine->FileHandles[handle], NULL, NULL, &fileTime)) OKCF();
            else { SETAX(6); ERRCF(); }
        } else { SETAX(1); ERRCF(); }
    } else if (function == 0x5A || function == 0x5B) {  /* create temp / create new */
        CHAR fileName[300]; HANDLE file; DWORD slot;
        DosGuestPath(machine, R_DS, R_DX, fileName, sizeof(fileName));
        if (function == 0x5A) {                 /* DS:DX is a DIRECTORY path;
                                                   DOS appends a generated name
                                                   and hands it back in place. */
            INT index = 0; static UINT sequence = 0;
            PCSTR hexDigits = "0123456789ABCDEF";
            while (fileName[index] && index < 280) ++index;
            if (index && fileName[index-1] != '\\' && fileName[index-1] != '/') fileName[index++] = '\\';
            { UINT seed = (UINT)(GetTickCount() + (sequence++ * 0x1234u));
              INT index2; for (index2 = 0; index2 < 8; ++index2) fileName[index + index2] = hexDigits[(seed >> (28 - index2*4)) & 0xF]; }
            fileName[index + 8] = 0;
            { volatile BYTE *buffer = (volatile BYTE *)((R_DS << 4) + (R_DX & 0xFFFF));
              INT index2 = 0; while (fileName[index2]) { buffer[index2] = (BYTE)fileName[index2]; ++index2; } buffer[index2] = 0; }
        }
        file = CreateFileA(fileName, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL,
                        CREATE_NEW, (DWORD)(R_CX & 0x3F) ? (DWORD)(R_CX & 0x3F)
                                                         : FILE_ATTRIBUTE_NORMAL, NULL);
        if (file == INVALID_HANDLE_VALUE) {
            /* Oracle: create-new over an existing file is error 80 (file exists),
               not 5 -- measured, and not the obvious guess. */
            DWORD error = GetLastError();
            SETAX((WORD)(error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS ? 80 : 3));
            ERRCF();
        } else {
            slot = DosHandleAllocate((PVOID const *)machine->FileHandles, machine->StdOpen);
            if (slot < DOS_MAX_FILES) { machine->FileHandles[slot] = file; SETAX(slot); OKCF();
                                        DosStampVdmNow(file); /* #263 */ }
            else { CloseHandle(file); SETAX(4); ERRCF(); }
        }
    } else if (function == 0x5C) {              /* lock / unlock a byte range */
        DWORD handle = R_BX & 0xFFFF;
        DWORD offset = ((DWORD)(R_CX & 0xFFFF) << 16) | (DWORD)(R_DX & 0xFFFF);
        DWORD length = ((DWORD)(R_SI & 0xFFFF) << 16) | (DWORD)(R_DI & 0xFFFF);
        if (!DosHandleIsFile((PVOID const *)machine->FileHandles, handle)) { SETAX(6); ERRCF(); }
        else {
            BOOL isOk = ((R_AX & 0xFF) == 0)
                     ? LockFile(machine->FileHandles[handle], offset, 0, length, 0)
                     : UnlockFile(machine->FileHandles[handle], offset, 0, length, 0);
            if (isOk) OKCF(); else { SETAX(0x21); ERRCF(); }  /* 33 = lock violation */
        }
    } else if (function == 0x67) {              /* set maximum handle count */
        /* We keep a fixed DOS_MAX_FILES-entry table, so anything up to that succeeds.
           NOTE the oracle FAILED this with AX=8 (insufficient memory) when asked
           for 30 -- that is a property of ITS memory state at that moment, not a
           rule about DOS, which is why the probe treats the result as
           informational rather than comparable. */
        if ((R_BX & 0xFFFF) <= DOS_MAX_FILES) OKCF();
        else { SETAX(8); ERRCF(); }
    } else if (function == 0x68 || function == 0x6A) {  /* commit file (flush) */
        DWORD handle = R_BX & 0xFFFF;
        if (!DosHandleIsFile((PVOID const *)machine->FileHandles, handle)) { SETAX(6); ERRCF(); }
        else { FlushFileBuffers(machine->FileHandles[handle]); OKCF(); }
    } else if (function == 0x6C) {              /* extended open/create */
        /* BX=mode, CX=attributes, DX=action, DS:SI=name.
           action: bits 0-3 if it exists (0 fail, 1 open, 2 truncate),
                   bits 4-7 if it does not (0 fail, 1 create).
           CX on return says what happened: 1 opened, 2 created, 3 truncated. */
        /* ── #210: AND 716Ch / 71A9h, THE LONG-FILENAME OPEN, ARRIVE HERE TOO -- same
             registers (BX mode, CX attributes, DX action, DS:SI name; 716Ch's DI alias
             hint is not used), and Win32 takes a long name as readily as a short one.
             The action word, the access and the "action taken" in CX are dos_lfn.h's,
             pinned by tests/unit/lfn_test.c. ONE CHANGE OF ANSWER rode in with the
             move: CREATE_ALWAYS on a file that was NOT there now reports CX=2 (created),
             where it said 3 (replaced) for every CREATE_ALWAYS -- RBIL's table, and the
             probe's first 716Ch is exactly that call. ⚠ Unmeasured on 6.22 for 6Ch:
             p_file's three 6Ch rows (open / exists / missing) do not reach it. */
        CHAR fileName[300]; HANDLE file; DWORD slot, disposition;
        DWORD access = (DWORD)DosExtOpenAccess((UINT)(R_BX & 0xFFFF));
        disposition = DosExtOpenDisposition((UINT)(R_DX & 0xFFFF));  /* Win32's own numbers */
        DosGuestPath(machine, R_DS, R_SI, fileName, sizeof(fileName));
        /* FILE_SHARE_WRITE too: we do not emulate SHARE.EXE, so a second open of a
           file this VDM holds must not fail -- the rule AH=3Dh learned in session 37
           and this twin had not (#168). */
        SetLastError(0);
        file = DosOpenStampable(fileName, access, FILE_SHARE_READ | FILE_SHARE_WRITE, disposition,
                               (DWORD)(R_CX & 0x3F) ? (DWORD)(R_CX & 0x3F)
                                                    : FILE_ATTRIBUTE_NORMAL);
        if (file == INVALID_HANDLE_VALUE) {
            /* Same collapse as AH=3Dh had, same fix -- see DosErrFromWin32(). */
            DWORD win32Error = GetLastError(); WORD dosError;
            INT isMapped = DosErrFromWin32((unsigned long)win32Error, &dosError);
            SETAX(dosError); ERRCF();
            trace = zput(trace, isLfnAlias ? "  INT21 AX=71" : "  INT21 AH=6C");
            if (isLfnAlias) trace = zhexb(trace, isLfnAlias);
            trace = zput(trace, " ["); trace = zput(trace, fileName);
            trace = zput(trace, "] FAILED win32=0x"); trace = zhex(trace, win32Error);
            trace = zput(trace, isMapped ? " -> AX=0x" : " UNMAPPED, kept -> AX=0x");
            trace = zhex(trace, dosError); trace = zput(trace, "\r\n");
        }
        else {
            WORD action = (WORD)DosExtOpenActionTaken((UINT)disposition,
                                                        GetLastError() == ERROR_ALREADY_EXISTS,
                                                        isLfnAlias != 0);
            slot = DosHandleAllocate((PVOID const *)machine->FileHandles, machine->StdOpen);
            if (slot < DOS_MAX_FILES) { machine->FileHandles[slot] = file; SETAX(slot); SET16(R_CX, action); OKCF();
                                        if (action != 1) DosStampVdmNow(file); /* #263: created/truncated */ }
            else { CloseHandle(file); SETAX(4); ERRCF(); }
        }
    } else if (function == 0x59) {              /* get extended error */
        /* Four answers, not one: extended code (AX), class (BH), suggested
           action (BL) and locus (CH).  The pairings are MEASURED, by provoking
           each failure on the oracle and asking (tests/probes/dos/p_err.asm):
             codes 2, 3, 18  -> BX=0803, CH=02   (not-found family)
             code  6         -> BX=0704, CH=01   (bad handle)
           CL is left ALONE -- the oracle returns it still holding the caller's
           value, so writing it would be an invention. */
        WORD error = machine->LastError, classAndAction = 0;
        BYTE locus = 0;
        /* The table moved to src/dos/dos_err.h so the off-VM battery can pin it
           (tests/unit/err_test.c) and so there is exactly one place a row can
           be added. Rows 5 (access denied) and 0x50 (file exists) were provoked
           and measured in session 52; before that both fell into the UNMEASURED
           arm below. */
        if (!DosErrClassify(error, &classAndAction, &locus)) {
            /* Rather than fabricate a class for a code we have not provoked on
               real DOS, say so. Extend p_err.asm and dos_err.h together. */
            trace = zput(trace, "  INT21 AH=59 class/action/locus UNMEASURED for code 0x");
            trace = zhex(trace, error); trace = zput(trace, "\r\n");
        }
        SETAX(error);
        SET16(R_BX, classAndAction);
        R_CX = (R_CX & 0xFFFF00FFu) | (((DWORD)locus & 0xFF) << 8);
        OKCF();
    } else if (function == 0x60) {              /* truename: DS:SI -> ES:DI */
        CHAR input[300], output[300];
        DWORD count;
        DosGuestPath(machine, R_DS, R_SI, input, sizeof(input));
        count = GetFullPathNameA(input, sizeof(output), output, NULL);
        if (count == 0 || count >= sizeof(output)) { SETAX(3); ERRCF(); }
        else {
            volatile BYTE *buffer = (volatile BYTE *)((R_ES << 4) + (R_DI & 0xFFFF));
            INT index = 0;
            /* Oracle: a RELATIVE name resolves against the current directory and
               comes back fully qualified and UPPER CASE, and existence is not
               required -- "SUB\\FILE.TXT" became "C:\\SUB\\FILE.TXT" with no such dir. */
            while (output[index] && index < 127) {
                CHAR character = output[index];
                if (character >= 'a' && character <= 'z') character = (CHAR)(character - 32);
                buffer[index] = (BYTE)character; ++index;
            }
            buffer[index] = 0;
            OKCF();
        }
    } else if (function == 0x65) {              /* get extended country info */
        BYTE subfunction = (BYTE)(R_AX & 0xFF);
        if (subfunction == 0x01) {
            /* Oracle layout: [0]=1 id, [1-2]=size 0x26, [3-4]=country,
               [5-6]=code page, [7-40]=34-byte country block.  41 bytes total.
               NOTE the block here is the 34-byte form (24 meaningful + 10 zero),
               where AH=38h writes only 24 -- measured, not assumed. */
            volatile BYTE *buffer = (volatile BYTE *)((R_ES << 4) + (R_DI & 0xFFFF));
            WORD capacity = (WORD)(R_CX & 0xFFFF), index;
            BYTE info[41];
            INT index2;
            for (index2 = 0; index2 < 41; ++index2) info[index2] = 0;
            info[0] = 0x01; info[1] = 0x26; info[2] = 0x00;
            info[3] = 0x01; info[4] = 0x00;              /* country 1  */
            info[5] = 0xB5; info[6] = 0x01;              /* code page 437 */
            for (index2 = 0; index2 < 24; ++index2) info[7 + index2] = g_DosCountryUs[index2];
            info[7 + 18] = (BYTE)(DOS_CASEMAP_OFF & 0xFF);
            info[7 + 19] = (BYTE)(DOS_CASEMAP_OFF >> 8);
            info[7 + 20] = (BYTE)(DOS_HDLR_SEG & 0xFF);
            info[7 + 21] = (BYTE)(DOS_HDLR_SEG >> 8);
            for (index = 0; index < 41 && index < capacity; ++index) buffer[index] = info[index];
            SETAX(0x01B5); OKCF();                       /* oracle: AX = code page */
        } else if (subfunction >= 0x02 && subfunction <= 0x07) {
            /* Table subfunctions: ES:DI gets a 5-byte descriptor -- the
               subfunction id, then a FAR POINTER to the table itself.  ATTRIB
               wants AL=07 and COMMAND.COM AL=04, which is why AL=01 alone was
               not enough. Offsets from dos_layout.h; contents in dos_ctab.h,
               dumped from the oracle rather than synthesised. */
            volatile BYTE *buffer = (volatile BYTE *)((R_ES << 4) + (R_DI & 0xFFFF));
            WORD offset = 0;
            switch (subfunction) {
            case 0x02: offset = DOS_CTAB_UPPER;   break;
            case 0x04: offset = DOS_CTAB_FNUPPER; break;
            case 0x05: offset = DOS_CTAB_FNTERM;  break;
            case 0x06: offset = DOS_CTAB_COLLATE; break;
            case 0x07: offset = DOS_CTAB_DBCS;    break;
            default:   offset = DOS_CTAB_UPPER;   break;  /* AL=03, same shape */
            }
            buffer[0] = subfunction;
            buffer[1] = (BYTE)(offset & 0xFF);        buffer[2] = (BYTE)(offset >> 8);
            buffer[3] = (BYTE)(DOS_CTAB_SEG & 0xFF); buffer[4] = (BYTE)(DOS_CTAB_SEG >> 8);
            SETAX(0x01B5); OKCF();
        } else if (subfunction >= 0x20 && subfunction <= 0x22) {
            /* ── CAPITALISE: a character (DL), CX bytes at DS:DX, or ASCIIZ at DS:DX.
                 (GH #165) Through the SAME uppercase table AL=02 hands out (dumped
                 from 6.22), so a program that capitalises through DOS and one that
                 reads the table agree. Measured: 'a'->'A', 81h->9Ah, digits kept. */
            if (subfunction == 0x20) {
                SET16(R_DX, (R_DX & 0xFF00) | DosCtabUpcase437((BYTE)(R_DX & 0xFF)));
            } else {
                volatile BYTE *text = (volatile BYTE *)((R_DS << 4) + (R_DX & 0xFFFF));
                UINT32 index, count = (subfunction == 0x21) ? (UINT32)(R_CX & 0xFFFF) : 0x10000u;
                for (index = 0; index < count; ++index) {
                    if (subfunction == 0x22 && text[index] == 0) break;
                    text[index] = DosCtabUpcase437(text[index]);
                }
            }
            OKCF();
        } else if (subfunction == 0x23) {
            /* YES/NO for the country: AX = 0 no, 1 yes, 2 neither. Country 1 only,
               like everything else here. Measured: 'y'->1, 'N'->0, 'q'->2. */
            BYTE character = DosCtabUpcase437((BYTE)(R_DX & 0xFF));
            SETAX(character == 'Y' ? 1 : character == 'N' ? 0 : 2); OKCF();
        } else {
            trace = zput(trace, "  INT21 AH=65 AL=0x"); trace = zhex(trace, subfunction);
            /* Not a gap: 6.22 and PCem answer AX=1 CF=1 too (p_subfn int21.6508). */
            trace = zput(trace, " not a 6.22 subfunction -> AX=1 CF=1 (matches DOS)\r\n");
            SETAX(1); ERRCF();
        }
    } else if (function == 0x69) {              /* get/set volume serial number */
        BYTE subfunction = (BYTE)(R_AX & 0xFF);
        if (subfunction == 0x00) {
            /* Oracle layout: [0-1] NOT WRITTEN (came back poisoned), [2-5]
               serial dword, [6-16] 11-byte label, [17-24] 8-byte fs type. */
            volatile BYTE *buffer = (volatile BYTE *)((R_DS << 4) + (R_DX & 0xFFFF));
            CHAR label[64], fileSystemType[32];
            DWORD serial = 0, maximumComponent = 0, flags = 0;
            INT index;
            for (index = 0; index < 64; ++index) label[index] = 0;
            for (index = 0; index < 32; ++index) fileSystemType[index] = 0;
            BYTE drive = DosSerialDrive(machine, (BYTE)(R_BX & 0xFF));
            if (drive < 26 && g_DosSerialIsSet[drive]) {     /* #165: set this session */
                for (index = 0; index < 23; ++index) buffer[2 + index] = g_DosSerialInfo[drive][index];
                OKCF();
            } else if (GetVolumeInformationA(NULL, label, sizeof(label), &serial,
                                      &maximumComponent, &flags, fileSystemType, sizeof(fileSystemType))) {
                buffer[2] = (BYTE)( serial        & 0xFF);
                buffer[3] = (BYTE)((serial >> 8)  & 0xFF);
                buffer[4] = (BYTE)((serial >> 16) & 0xFF);
                buffer[5] = (BYTE)((serial >> 24) & 0xFF);
                for (index = 0; index < 11; ++index) buffer[6 + index]  = (BYTE)(label[index] ? label[index] : ' ');
                for (index = 0; index < 8;  ++index) buffer[17 + index] = (BYTE)(fileSystemType[index] ? fileSystemType[index] : ' ');
                OKCF();
            } else { SETAX(0x0F); ERRCF(); }
        } else if (subfunction == 0x01) {
            /* ── SET SERIAL (#165). Real DOS writes serial, label and file-system type
                 into the disk's boot record (p_4b05: 6.22 and PCem accept it and 6900h
                 reads the new serial back; DOSBox-X refuses). Our drives are the host's
                 own disks, so by the user's decision (2026-09-28) it is SESSION-ONLY:
                 remembered per drive, answered by 6900h until this VDM ends, and
                 nothing is written to the host disk. Same 25-byte layout as 6900h. */
            const volatile BYTE *source = (const volatile BYTE *)((R_DS << 4) + (R_DX & 0xFFFF));
            BYTE drive = DosSerialDrive(machine, (BYTE)(R_BX & 0xFF));
            INT index;
            if (drive < 26) {
                for (index = 0; index < 23; ++index) g_DosSerialInfo[drive][index] = source[2 + index];
                g_DosSerialIsSet[drive] = 1;
                trace = zput(trace, "  INT21 AX=6901 set serial -- kept for this session only\r\n");
                OKCF();
            } else { SETAX(0x0F); ERRCF(); }
        } else {
            trace = zput(trace, "  INT21 AH=69 AL=0x"); trace = zhex(trace, subfunction);
            trace = zput(trace, " UNIMPLEMENTED\r\n");
            machine->Unimplemented[0x69 >> 3] |= (BYTE)(1u << (0x69 & 7));
            SETAX(1); ERRCF();
        }
    } else if (function == 0x47) {              /* get current directory -> DS:SI */
        /* DOS returns the path WITHOUT the drive letter and WITHOUT a leading
           backslash, ASCIIZ.  Oracle at the root writes exactly ONE byte -- the
           terminating NUL -- and leaves the rest of the caller's 64-byte buffer
           untouched, so we must not pad it.
           Wanted by four of the five real 6.22 tools we ran (TREE, ATTRIB,
           XCOPY, COMMAND.COM), which is why it came first.  GH #32. */
        CHAR currentDirectory[300];
        DWORD count;
        BYTE driveNumber = (BYTE)(R_DX & 0xFF);
        BYTE currentDrive = (BYTE)(DosCurrentDrive(machine) + 1);
        if (machine->VirtualDrive >= 0) {     /* a drive Win32 cannot stand on: its =X: or root */
            CHAR driveSpec[3]; driveSpec[0] = (CHAR)('A' + machine->VirtualDrive); driveSpec[1] = ':'; driveSpec[2] = 0;
            count = GetFullPathNameA(driveSpec, sizeof(currentDirectory), currentDirectory, NULL);
        } else count = GetCurrentDirectoryA(sizeof(currentDirectory), currentDirectory);
        /* ── ★ 0xF0 IS krnl386 TALKING TO ntvdm, AND WE ARE ntvdm. (#128, s37) ──
             For some drives krnl386 issues this call with DL = 0xF0 (seen in the
             INT 21h trace, s37). 0xF0 is not a drive under any DOS convention --
             it is a sentinel between the two halves of one product, asking for
             the current directory, so answering it is implementing a protocol
             rather than guessing at one. It is what stopped WOWEXEC.EXE
             resolving: while this call failed, krnl386 gave up on building the
             path before the PATH search was ever reached.
           ⚠ Only the EXACT sentinel, never "any invalid drive". A DOS program that
             passes garbage in DL still gets the error DOS gives it -- turning that
             into a plausible answer would be the "runs but lies" class. */
        if (driveNumber == 0xF0) {
            trace = zput(trace, "  INT21 AH=47 drive 0xF0 (WOW sentinel) -> current drive\r\n");
            driveNumber = 0;
        }
        /* ── ★ AND PER-DRIVE CURRENT DIRECTORIES ARE REAL DOS BEHAVIOUR. ─────────
             This answered only for the drive we happened to be on and returned
             "invalid drive" for every other, with a note saying so. Win32 keeps a
             current directory per drive too -- that is what the hidden `=C:`
             environment variables are -- and GetFullPathNameA("X:") reads it. So
             ask for the drive the caller named, and keep the honest refusal for a
             drive that genuinely is not there (GetLogicalDrives), which is the
             error DOS itself returns. GH #32. */
        if (count && driveNumber && driveNumber != currentDrive && driveNumber <= 26) {
            if (GetLogicalDrives() & (1u << (driveNumber - 1))) {
                CHAR driveSpec[4]; driveSpec[0] = (CHAR)('A' + driveNumber - 1); driveSpec[1] = ':';
                driveSpec[2] = 0;
                count = GetFullPathNameA(driveSpec, sizeof(currentDirectory), currentDirectory, NULL);
            } else count = 0;
        }
        if (count == 0 || count >= sizeof(currentDirectory)) {
            trace = zput(trace, "  INT21 AH=47 drive 0x"); trace = zhex(trace, driveNumber);
            trace = zput(trace, " -> invalid drive\r\n");
            SETAX(0x0F); ERRCF();
        } else {
            volatile BYTE *destination = (volatile BYTE *)((R_DS << 4) + (R_SI & 0xFFFF));
            PCSTR path = currentDirectory;
            INT index = 0;
            /* ── #164: SHORT AND UPPER CASE, AS DOS KEEPS IT. (s85) ─────────────────
                 The host hands back whatever case and length the directory was
                 entered with ("...\ntvdmex\demo\win16"). DOS's CDS holds an upper-case
                 8.3 path, and stock NTVDM answers exactly that -- measured beside
                 ours by tests/probes/win16/w_cwd on the rig: ours `...\ntvdmex\demo\...`,
                 stock `...\NTVDMEX\DEMO\...`. */
            {   CHAR shortPath[300];
                DWORD shortLength = GetShortPathNameA(currentDirectory, shortPath, sizeof shortPath);
                INT charIndex;
                if (shortLength && shortLength < sizeof shortPath) lstrcpynA(currentDirectory, shortPath, sizeof currentDirectory);
                for (charIndex = 0; currentDirectory[charIndex]; ++charIndex)
                    if (currentDirectory[charIndex] >= 'a' && currentDirectory[charIndex] <= 'z') currentDirectory[charIndex] = (CHAR)(currentDirectory[charIndex] - 32);
            }
            if (currentDirectory[1] == ':') path += 2;  /* drop "C:"            */
            if (*path == '\\' || *path == '/') ++path;  /* drop the separator   */
            while (path[index] && index < 63) { destination[index] = (BYTE)path[index]; ++index; }
            destination[index] = 0;
            SETAX(0x0100); OKCF();                    /* oracle: AX=0100      */
        }
    } else if (function == 0x3B) {              /* chdir: DS:DX = ASCIIZ path */
        /* ── ★ CHDIR NEVER MOVES THE CURRENT DRIVE. ─────────────────────────────
             Oracle (p_drv.asm): `3Bh C:\ZZDRV` issued from A: leaves 19h at A:, and
             47h for C: then answers ZZDRV -- every drive keeps its own directory.
             SetCurrentDirectoryA("C:\ZZDRV") moves the PROCESS to C:, which made
             the DOS current drive follow it. So a path on another drive only sets
             that drive's =X: variable (which is what "X:" resolves through); the
             process current directory changes only for the drive we are on.
             Also measured: a bare "C:" is path-not-found (AX=3), not a no-op. */
        CHAR fileName[300], fullPath[300];
        DWORD count;
        BYTE target;
        DosGuestPath(machine, R_DS, R_DX, fileName, sizeof(fileName));
        trace = zput(trace, "  INT21 AH=3B chdir ["); trace = zput(trace, fileName); trace = zput(trace, "]");
        if (fileName[0] && fileName[1] == ':' && !fileName[2]) { SETAX(3); ERRCF(); trace = zput(trace, " -> 3 (drive only)\r\n"); }
        else if ((count = GetFullPathNameA(fileName, sizeof(fullPath), fullPath, NULL)) == 0 || count >= sizeof(fullPath)
                 || fullPath[1] != ':') { SETAX(3); ERRCF(); trace = zput(trace, " -> 3\r\n"); }
        else {
            target = (BYTE)((fullPath[0] | 0x20) - 'a');
            if (target == DosCurrentDrive(machine)) {
                if (SetCurrentDirectoryA(fullPath)) {
                    machine->VirtualDrive = -1;  /* it can be stood on after all */
                    DosNoteDriveDirectory(fullPath);
                    OKCF(); trace = zput(trace, " -> ok\r\n");
                } else { SETAX(3); ERRCF(); trace = zput(trace, " -> 3 (0x"); trace = zhex(trace, GetLastError());
                         trace = zput(trace, ")\r\n"); }  /* oracle: AX=0003, CF=1 */
            } else {
                DWORD attributes = GetFileAttributesA(fullPath);
                if (attributes != 0xFFFFFFFFu && (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    DosNoteDriveDirectory(fullPath);
                    OKCF(); trace = zput(trace, " -> ok (another drive's directory; current drive unchanged)\r\n");
                } else { SETAX(3); ERRCF(); trace = zput(trace, " -> 3 (other drive, 0x"); trace = zhex(trace, GetLastError());
                         trace = zput(trace, ")\r\n"); }
            }
        }
    } else if (function == 0x36) {              /* get free disk space: DL = drive */
        /* AX=sectors/cluster BX=free clusters CX=bytes/sector DX=total clusters.
           AN INVALID DRIVE RETURNS AX=FFFF WITH CARRY CLEAR -- oracle-confirmed,
           and easy to get wrong: it is not a CF error.  Counts are 16-bit in the
           DOS interface, so a large volume has to be clamped rather than wrapped. */
        BYTE driveNumber = (BYTE)(R_DX & 0xFF);
        DWORD sectorsPerCluster = 0, bytesPerSector = 0, freeClusters = 0, totalClusters = 0;
        CHAR root[4]; PSTR rootPointer = 0;
        if (!driveNumber && machine->VirtualDrive >= 0) driveNumber = (BYTE)(machine->VirtualDrive + 1);
        if (driveNumber) { root[0] = (CHAR)('A' + driveNumber - 1); root[1] = ':'; root[2] = '\\';
                    root[3] = 0; rootPointer = root; }
        if (driveNumber <= 26 && GetDiskFreeSpaceA(rootPointer, &sectorsPerCluster, &bytesPerSector, &freeClusters, &totalClusters)) {
            SETAX((WORD)sectorsPerCluster);
            SET16(R_BX, freeClusters > 0xFFFF ? 0xFFFF : freeClusters);
            SET16(R_CX, (WORD)bytesPerSector);
            SET16(R_DX, totalClusters  > 0xFFFF ? 0xFFFF : totalClusters);
            OKCF();
        } else {
            SETAX(0xFFFF); OKCF();
        }
    } else if (function == 0x38) {              /* get/set country information */
        BYTE subfunction = (BYTE)(R_AX & 0xFF);
        WORD wanted = (subfunction == 0xFF) ? (WORD)(R_BX & 0xFFFF)
                                       : (WORD)(subfunction ? subfunction : 1);
        if ((R_DX & 0xFFFF) == 0xFFFF) {        /* DX=FFFF selects SET, not GET */
            /* ── MEASURED, 6.22 AND PCem, NO COUNTRY.SYS (p_subfn): setting the
                 CURRENT country succeeds (AX=1 CF=0); any other fails AX=1 CF=1,
                 because the data for it would come from COUNTRY.SYS and none was
                 loaded. We are that machine: country 1 and nothing else. (GH #165) */
            SETAX(1);
            if (wanted == 1) OKCF();
            else {
                trace = zput(trace, "  INT21 AH=38 SET country 0x"); trace = zhex(trace, wanted);
                trace = zput(trace, " refused: only country 1 is loaded (matches DOS without COUNTRY.SYS)\r\n");
                ERRCF();
            }
        } else if (wanted == 1) {               /* USA -- the only block we have */
            volatile BYTE *buffer = (volatile BYTE *)((R_DS << 4) + (R_DX & 0xFFFF));
            INT index;
            for (index = 0; index < 24; ++index) buffer[index] = g_DosCountryUs[index];
            buffer[18] = (BYTE)(DOS_CASEMAP_OFF & 0xFF);
            buffer[19] = (BYTE)(DOS_CASEMAP_OFF >> 8);
            buffer[20] = (BYTE)(DOS_HDLR_SEG & 0xFF);
            buffer[21] = (BYTE)(DOS_HDLR_SEG >> 8);
            SETAX(1); SET16(R_BX, 1); OKCF();
        } else {
            /* We only have measured data for country 1. Inventing a block for
               another country would be exactly the from-memory guess the
               programme forbids, so say so rather than fabricate one. */
            /* ...and 6.22 without COUNTRY.SYS answers exactly this: AX=1 CF=1
               (p_subfn int21.382C.get). It was AX=2. */
            trace = zput(trace, "  INT21 AH=38 country 0x"); trace = zhex(trace, wanted);
            trace = zput(trace, " refused: only country 1 is loaded (matches DOS without COUNTRY.SYS)\r\n");
            SETAX(1); ERRCF();
        }
    } else if (function == 0x58) {              /* get/set memory allocation strategy */
        BYTE subfunction = (BYTE)(R_AX & 0xFF);
        if (subfunction == 0x00)      { SETAX(machine->AllocationStrategy); OKCF(); }
        else if (subfunction == 0x01) { machine->AllocationStrategy = (BYTE)(R_BX & 0xFF); OKCF(); }
        /* Oracle: AL=02 returns the state in AL and LEAVES AH ALONE -- 6.22
           answered AX=5800 to a call made with AX=5802. */
        else if (subfunction == 0x02) { SETAX((R_AX & 0xFF00) | machine->UmbLink); OKCF(); }
        else if (subfunction == 0x03) {
            /* ── ★ YOU CANNOT LINK A UMB CHAIN THAT DOES NOT EXIST. (GH #47) ───
                 This accepted the call and stored the flag, and that is why
                 MEM.EXE reported 1,664K of "Upper" memory on a machine with
                 none, and counted the free tail of conventional memory as an
                 upper block -- leaving Conventional free at 0K and "Largest
                 executable program size" at -16 bytes.
                 MEM's own trace is what named it: `21:58/03 bx=0001`, set UMB
                 link ON, immediately before it walks the chain.
               Oracle, tests/probes/dos/p_umb.asm on MS-DOS 6.22 booted with no
               EMM386 and no DOS=UMB -- the same configuration we present:
                 CASE=int21.5803.link.on  AX=0001 CF=1
                 CASE=int21.5802.after.on AX=5800          (still not linked)
                 CASE=int21.5803.link.off AX=0001 CF=1
               i.e. real DOS REFUSES, in both directions, with error 1. We
               provide no upper memory blocks at all, so refusing is not a
               limitation being papered over -- it is the true answer. */
            trace = zput(trace, "  INT21 AH=5803 UMB link refused: no UMB provider "
                          "(oracle: AX=0001 CF=1)\r\n");
            SETAX(1); ERRCF();
        }
        else {
            trace = zput(trace, "  INT21 AH=58 AL=0x"); trace = zhex(trace, subfunction);
            /* Not a gap: 6.22 and PCem answer AX=1 CF=1 too (p_subfn int21.5804). */
            trace = zput(trace, " not a 6.22 subfunction -> AX=1 CF=1 (matches DOS)\r\n");
            SETAX(1); ERRCF();
        }
    } else if (function == 0x52) {              /* get list of lists -> ES:BX */
        /* The word at ES:BX-2 is the first MCB segment, and that is the field a
           memory walker actually follows -- it is filled in truthfully from our
           own MCB chain. The rest of SysVars is a stub, so it is ZEROED rather
           than left as whatever was in memory: a walker that follows a garbage
           DPB or SFT pointer wanders off into nonsense, which is the silent
           failure #27 exists to remove, whereas a null pointer stops it. */
        if (machine->SysvarsSegment) {
            SET16(R_ES, machine->SysvarsSegment);
            SET16(R_BX, machine->SysvarsOffset);
            trace = zput(trace, "  INT21 AH=52 list-of-lists (MCB head only; rest stubbed)\r\n");
            OKCF();
        } else {
            trace = zput(trace, "  INT21 AH=52 UNIMPLEMENTED (no SysVars planted)\r\n");
            machine->Unimplemented[0x52 >> 3] |= (BYTE)(1u << (0x52 & 7));
            SETAX(1); ERRCF();
        }
    } else if (function == 0x44) {              /* IOCTL (C-runtime isatty etc.) */
        BYTE subfunction = (BYTE)(R_AX & 0xFF);
        WORD handle = (WORD)(R_BX & 0xFFFF);
        /* 4400h: the device-information word, in DX AND in AX (6.22 and PCem both
             return AX = DX, p_ioctl's own captures included). #251: AUX is 80C0h and
             PRN A0C0h (bit 13, output-until-busy) -- measured, p_auxprn. */
        if (subfunction == 0x00) {
            WORD deviceInfo = (handle < 5) ? 0x80D3 : 0x0002;
            if ((handle == 3 || handle == 4) && DosHandleIsDevice((PVOID const *)machine->FileHandles, machine->StdOpen, handle))
                deviceInfo = (handle == 3) ? 0x80C0 : 0xA0C0;
            SET16(R_DX, deviceInfo); SETAX(deviceInfo); OKCF();
        }
        else if (subfunction == 0x06 || subfunction == 0x07) { SETAX((R_AX & 0xFF00) | 0xFF); OKCF(); }
        /* ── ★★ THE DRIVE-CLASSIFICATION TRIO. (GH #32, #128, session 37) ─────────
             AL = 08h "is this block device removable", 09h "is it remote", 0Eh "get
             the logical drive map". BL is the drive (0 = default, 1 = A) and all
             three answer in REGISTERS -- no buffer anywhere near them.
           ⚠ They used to fall into the `else { OKCF(); }` below, which is the worst
             answer available: carry clear, meaning success, with the registers the
             caller happened to be holding. That is the "runs but lies" class, and it
             is what krnl386 uses to decide whether a drive is local. It probes every
             drive with 44/08, 44/09 and 44/0E in a loop (measured: once per drive,
             descending), and with all three lying it treated drive C: as
             non-local -- after which every INT 21h AH=47h for C: failed on its
             side, its path canonicalisation failed, and LoadModule("WOWEXEC.EXE")
             reported "file not found" without ever opening a file.
           ▸ Answered from the host, which is where the guest's drives really are.
           ▸ NOT yet checked against the MS-DOS 6.22 oracle -- the register contract
             here is from the documented interface, not from a run. Worth a panel
             (#24) since the whole point of M9 is that we do not write these from
             memory. The DX bits beyond 12 are the ones to confirm. */
        else if (subfunction == 0x08 || subfunction == 0x09 || subfunction == 0x0E) {
            BYTE drive = (BYTE)(handle & 0xFF);      /* 0 = default drive           */
            UINT type = 0;
            if (!drive) drive = (BYTE)(DosCurrentDrive(machine) + 1);
            if (drive >= 1 && drive <= 26 && (GetLogicalDrives() & (1u << (drive - 1)))) {
                CHAR root[4]; root[0] = (CHAR)('A' + drive - 1); root[1] = ':';
                root[2] = '\\'; root[3] = 0;
                type = GetDriveTypeA(root);
            }
            if (!type || type == 1) { SETAX(0x000F); ERRCF(); }  /* invalid drive      */
            else if (subfunction == 0x08) {
                /* AX = 0 removable, 1 fixed. A CD is removable media. */
                SETAX((type == DRIVE_REMOVABLE || type == DRIVE_CDROM) ? 0 : 1); OKCF();
            } else if (subfunction == 0x09) {
                /* DX = the device attribute word; bit 12 = the drive is remote.
                   Nothing else in it is load-bearing for the callers we have. */
                SET16(R_DX, (type == DRIVE_REMOTE) ? 0x1000 : 0x0000); OKCF();
            } else {
                /* AL = 0 when only one letter maps to the block device, which is
                   true of every drive we can see (we do not emulate a SUBST). */
                SETAX(R_AX & 0xFF00); OKCF();
            }
            trace = zput(trace, "  INT21 AH=44 AL=0x"); trace = zhex(trace, subfunction);
            trace = zput(trace, " drive 0x"); trace = zhex(trace, drive);
            trace = zput(trace, " type "); trace = zhex(trace, type);
            trace = zput(trace, " -> AX=0x"); trace = zhex(trace, R_AX & 0xFFFF);
            trace = zput(trace, " DX=0x"); trace = zhex(trace, R_DX & 0xFFFF);
            trace = zput(trace, (*guestFlags & 1) ? " (err)\r\n" : "\r\n");
        }
        /* ── #251: WHAT DOS SUPPORTS SUCCEEDS; THE REST IS "INVALID FUNCTION". ───────
             Every other sub-function answered CF=0 -- success, with nothing done.
             Measured (tests/probes/dos/p_ioctl2) on msdos622, dosbox-x and pcem alike:
             02h/04h (read control data), 0Ch/10h on CON, and the unassigned 12h/1Fh
             answer AX=0001 CF=1; 0Ah, 0Dh and 11h on a fixed disk answer CF=0.
             0Ah reports a local handle (DX bit 15 clear). 0Dh/11h keep today's bare
             success -- a disk-parameter block is a separate piece of work, and turning
             a success into a refusal there would break callers that work now. */
        else if (subfunction == 0x01 || subfunction == 0x0B || subfunction == 0x0D || subfunction == 0x0F) { OKCF(); }
        else if (subfunction == 0x0A) { SET16(R_DX, 0x0000); OKCF(); }
        else if (subfunction == 0x11) { SETAX(R_AX & 0xFF00); OKCF(); }  /* AL=0: supported */
        else                 { SETAX(0x0001); ERRCF(); }
        if (subfunction != 0x08 && subfunction != 0x09 && subfunction != 0x0E) {
            trace = zput(trace, "  INT21 AH=44 ioctl AL=0x"); trace = zhex(trace, subfunction);
            trace = zput(trace, " BX=0x"); trace = zhex(trace, handle); trace = zput(trace, "\r\n");
        }
    } else if (function == 0x63) {              /* get DBCS lead-byte table */
        if ((R_AX & 0xFF) == 0) { SET16(R_DS, DOS_HDLR_SEG); SET16(R_SI, DOS_DBCS_OFF); }
        SETAX(R_AX & 0xFF00); OKCF();
        trace = zput(trace, "  INT21 AH=63 DBCS lead-byte table\r\n");
    } else if (function == 0x25) {              /* set interrupt vector: AL=int DS:DX */
        DWORD vectorOffset = (R_AX & 0xFF) * 4;
        *(volatile WORD *)(vectorOffset)     = (WORD)(R_DX & 0xFFFF);
        *(volatile WORD *)(vectorOffset + 2) = (WORD)(R_DS & 0xFFFF);
        OKCF();
    } else if (function == 0x35) {              /* get interrupt vector: AL=int -> ES:BX */
        DWORD vectorOffset = (R_AX & 0xFF) * 4;
        SET16(R_BX, *(volatile WORD *)(vectorOffset));
        SET16(R_ES, *(volatile WORD *)(vectorOffset + 2));
        OKCF();
    } else if (function == 0x48) {              /* allocate BX paras -> AX=seg (err: BX=max) */
        WORD wanted = (WORD)(R_BX & 0xFFFF), segment = 0, maximum = 0;
        INT error = DosMcbAllocate(NULL, machine->FirstMcb, wanted, &segment, &maximum);
        if (error) { SET16(R_AX, error); SET16(R_BX, maximum); ERRCF(); }
        else     { SET16(R_AX, segment); OKCF(); }
        /* ── THE BLOCK BELONGS TO THE PROGRAM THAT ASKED. (s80) ───────────────────
             DOS stamps an allocation with the CURRENT PSP, and that is how it frees a
             terminated child's memory: every block its PSP owns. DosMcbAllocate() writes
             DOS_PSP_SEG unconditionally, which is right for the top-level program (its
             PSP is DOS_PSP_SEG) and wrong for every child -- so nothing a child
             allocated was ever given back. Measured: DOS/4GW's five real-mode blocks
             outlived Doom, and the next `doom` loaded 85 KB higher. */
        if (!error && segment && machine->PspSegment)
            DosMcbWriteWord(DosMcbSegmentAddress(NULL, (WORD)(segment - 1)) + 1, machine->PspSegment);
        trace = zput(trace, "  INT21 AH=48 alloc 0x"); trace = zhex(trace, wanted);
        trace = zput(trace, (*guestFlags & 1) ? " -> err max=0x" : " -> seg=0x");
        trace = zhex(trace, (*guestFlags & 1) ? maximum : (R_AX & 0xFFFF)); trace = zput(trace, "\r\n");
    } else if (function == 0x49) {              /* free block: ES=segment */
        INT error = DosMcbFree(NULL, (WORD)(R_ES & 0xFFFF));
        /* ── #258: AND A SUCCESSFUL FREE LEAVES AX = THE BLOCK'S MCB. ────────────────
             Undocumented, measured (tests/probes/dos/p_memax): MS-DOS 6.22 and PCem's
             MS-DOS both return AX = ES-1; dosbox-x leaves AX alone. The Microsoft
             kernel is the authority. We left AX as the caller's 49xx. */
        if (error) { SET16(R_AX, error); ERRCF(); }
        else { SET16(R_AX, (WORD)((R_ES & 0xFFFF) - 1)); OKCF(); }
        trace = zput(trace, "  INT21 AH=49 free seg=0x"); trace = zhex(trace, R_ES & 0xFFFF);
        trace = zput(trace, (*guestFlags & 1) ? " (err)\r\n" : "\r\n");
        /* A refused free names a block the caller believes in and we do not: show
           what is actually at seg-1, and the chain, so the two can be compared. */
        if (error) {
            const volatile BYTE *mcb = (const volatile BYTE *)(((R_ES & 0xFFFF) - 1u) << 4);
            WORD segment;
            INT index, count = 0;
            trace = zput(trace, "    at seg-1: ");
            for (index = 0; index < 16; ++index) { trace = zhexb(trace, mcb[index]); trace = zput(trace, " "); }
            trace = zput(trace, "\r\n    chain:");
            segment = machine->FirstMcb;
            while (segment && count++ < 40) {
                const volatile BYTE *chainBlock = (const volatile BYTE *)((DWORD)segment << 4);
                WORD owner = (WORD)(chainBlock[1] | (chainBlock[2] << 8)), size = (WORD)(chainBlock[3] | (chainBlock[4] << 8));
                trace = zput(trace, " "); trace = zhex(trace, segment); trace = zput(trace, chainBlock[0] == 'Z' ? "Z" : chainBlock[0] == 'M' ? "M" : "?");
                trace = zput(trace, "/"); trace = zhex(trace, owner); trace = zput(trace, "/"); trace = zhex(trace, size);
                if (chainBlock[0] != 'M') break;
                segment = (WORD)(segment + 1 + size);
            }
            trace = zput(trace, "\r\n");
        }
    } else if (function == 0x4A) {              /* resize: ES=block BX=new paras */
        WORD wanted = (WORD)(R_BX & 0xFFFF), maximum = 0;
        INT error = DosMcbResize(NULL, (WORD)(R_ES & 0xFFFF), wanted, &maximum);
        /* ── #258: A SUCCESSFUL RESIZE LEAVES AX = THE BLOCK'S SEGMENT. ──────────────
             Undocumented, and QuickBASIC 4.5 depends on it: its Quick Library loader
             takes AX after shrinking a top-of-memory block as the block's segment.
             We left the caller's 4Axx there, so `QB /L` loaded the library at 4Axx,
             freed a block that never existed at Make EXE ("Error in loading file
             (QB.QLB) - Internal error") and wrote a garbage .LIB into the link.
             Measured (tests/probes/dos/p_memax): MS-DOS 6.22, dosbox-x and PCem all
             return AX = ES for a shrink, a grow and a same-size resize. */
        if (error) { SET16(R_AX, error); if (error == 8) SET16(R_BX, maximum); ERRCF(); }
        else { SET16(R_AX, (WORD)(R_ES & 0xFFFF)); OKCF(); }
        trace = zput(trace, "  INT21 AH=4A resize seg=0x"); trace = zhex(trace, R_ES & 0xFFFF);
        trace = zput(trace, " -> 0x"); trace = zhex(trace, wanted);
        trace = zput(trace, (*guestFlags & 1) ? " (err)\r\n" : "\r\n");
    } else if (function == 0x51 || function == 0x62) {  /* get current PSP -> BX */
        SET16(R_BX, machine->PspSegment); OKCF();
    } else if (function == 0x50) {              /* set current PSP */
        machine->PspSegment = (WORD)(R_BX & 0xFFFF); OKCF();
    } else if (function == 0x1A) {              /* set DTA = DS:DX */
        machine->DtaSegment = (WORD)(R_DS & 0xFFFF); machine->DtaOffset = (WORD)(R_DX & 0xFFFF); OKCF();
    } else if (function == 0x2F) {              /* get DTA -> ES:BX */
        SET16(R_ES, machine->DtaSegment); SET16(R_BX, machine->DtaOffset); OKCF();
    } else if (function == 0x19) {              /* get current drive -> AL (C: = 2) */
        /* ── THE CURRENT DRIVE IS THE HOST CURRENT DIRECTORY'S, LIKE AH=47h's. ─────
             This returned a constant while AH=0Eh below was accepted and ignored, so
             a program probing drives the classic way -- select X, read back, compare
             -- found only C:. QB.EXE's file dialog does exactly that (39BCCh..39BE4h)
             and listed one drive on a machine with four. */
        SETAX((R_AX & 0xFF00) | DosCurrentDrive(machine)); OKCF();
    } else if (function == 0x0E) {              /* select drive -> AL = LASTDRIVE  */
        /* Win32 keeps a current directory per drive (the hidden =X: variables), and
           "X:" as a path means that directory -- so selecting a drive is one call,
           and a later relative open lands where DOS would put it. A drive that is
           not there is left unselected, as DOS leaves it; AL is LASTDRIVE either
           way, which is the documented answer and what a program sizes its drive
           list from.
           ── ★ A DRIVE THAT IS THERE BUT NOT READY IS STILL SELECTED. ─────────────
             Oracle (p_drv.asm): `0Eh B:` on a one-floppy 6.22 machine selects the
             phantom B: with nothing in it and 19h reads back 1; only a letter with
             no device behind it (D: with LASTDRIVE=E, Z:) is refused. Win32 refuses
             to chdir onto an empty floppy or CD-ROM (NOT READY), and this used to
             leave the guest on C: -- so QB.EXE's select/read-back probe found ONE
             drive on a machine with four. Now the selection is held in m->vdrive
             and every relative path goes to that drive (DosGuestPath), where the access
             fails as DOS's would. */
        BYTE driveNumber = (BYTE)(R_DX & 0xFF);
        if (driveNumber < 26 && (GetLogicalDrives() & (1u << driveNumber))) {
            CHAR driveSpec[3], currentDirectory[300];
            driveSpec[0] = (CHAR)('A' + driveNumber); driveSpec[1] = ':'; driveSpec[2] = 0;
            /* remember the directory we are leaving; "X:" resolves through =X: */
            if (machine->VirtualDrive < 0 && GetCurrentDirectoryA(sizeof(currentDirectory), currentDirectory)) DosNoteDriveDirectory(currentDirectory);
            if (SetCurrentDirectoryA(driveSpec)) machine->VirtualDrive = -1;
            else {                                       /* e.g. no media: try the root */
                CHAR root[4]; root[0] = driveSpec[0]; root[1] = ':'; root[2] = '\\'; root[3] = 0;
                if (SetCurrentDirectoryA(root)) machine->VirtualDrive = -1;
                else {
                    machine->VirtualDrive = driveNumber;
                    trace = zput(trace, "  INT21 AH=0E drive "); *trace++ = driveSpec[0];
                    trace = zput(trace, ": exists but is not ready (Win32 error 0x");
                    trace = zhex(trace, GetLastError());
                    trace = zput(trace, ") -> selected as the DOS current drive anyway\r\n");
                }
            }
        }
        SETAX((R_AX & 0xFF00) | DOS_LASTDRIVE); OKCF();
    } else if (function == 0x0D) {              /* disk reset (flush) -> nop */
        OKCF();
    } else if (function == 0x33) {              /* get/set Ctrl-Break, get true version */
        BYTE subfunction = (BYTE)(R_AX & 0xFF);
        /* ── THE FLAG IS STATE, NOT A CONSTANT. (GH #165) ──────────────────────
             Get used to answer "off" and set accepted a value and dropped it, so a
             program that turned checking on read back off. Measured, 6.22 and PCem
             (tests/probes/dos/p_subfn.asm): 3301 DL=1 then 3300 -> DL=1; 3302 swaps
             and returns the OLD state in DL. DH is left alone -- 6.22 does. */
        if (subfunction == 0x00) { SET16(R_DX, (R_DX & 0xFF00) | machine->IsBreakOn); OKCF(); }
        else if (subfunction == 0x01) { machine->IsBreakOn = (BYTE)((R_DX & 0xFF) ? 1 : 0); OKCF(); }
        else if (subfunction == 0x02) {
            BYTE previous = machine->IsBreakOn;
            machine->IsBreakOn = (BYTE)((R_DX & 0xFF) ? 1 : 0);
            SET16(R_DX, (R_DX & 0xFF00) | previous); OKCF();
        }
        else if (subfunction == 0x05) { SET16(R_DX, 3); OKCF(); }  /* boot drive = C: */
        else if (subfunction == 0x06) {                        /* get TRUE version */
            /* BL=major BH=minor DL=revision DH=flags.  DH bit 3 = DOS in ROM,
               bit 4 = DOS in HMA; we are in neither, so 0.  Note the oracle
               reports DH=0x10 because that image boots DOS=HIGH -- DH is a
               property of the host's configuration, not of the version, which
               is why the probes do not compare it. */
            /* ⚠ Real SETVER leaves the TRUE version alone. Ours follows the same
                 per-process rule anyway: whether XP's shell checks 3306h as well is not
                 measured, and a shell that refuses to start is the costlier mistake. */
            SET16(R_BX, DosVersionWord(machine));
            SET16(R_DX, 0x0000);
            OKCF();
        } else {                                               /* not a 6.22 subfn */
            /* Not a gap: 6.22 and PCem both answer AL=FFh (p_subfn int21.3307) and
               leave the carry alone, so we do exactly that. */
            trace = zput(trace, "  INT21 AH=33 AL=0x"); trace = zhex(trace, subfunction);
            trace = zput(trace, " not a 6.22 subfunction -> AL=FF (matches DOS)\r\n");
            SETAX((R_AX & 0xFF00) | 0xFF);
        }
    } else if (function == 0x2A) {              /* get date: CX=yr DH=mon DL=day AL=dow */
        /* The VDM's clock, not the host's: host-now + whatever 2Bh/2Dh set. See
           dos_clock.h. With nothing set the offset is 0 and this is GetLocalTime.
           #262: a tick count the BIOS did not write is followed first, date included
           (its midnight rollovers are DOS's day number moving). */
        DOS_CLOCK_TIME clock; DosClockSync(); DosClockRead(g_DosClock.DosOffset, &clock);
        SET16(R_CX, clock.Year);
        SET16(R_DX, ((clock.Month & 0xFF) << 8) | (clock.Day & 0xFF));
        SETAX((R_AX & 0xFF00) | (clock.DayOfWeek & 0xFF));
        OKCF();
    } else if (function == 0x2C) {              /* get time: CH=hr CL=min DH=sec DL=cs */
        /* #262: CLOCK$ reads 0040:006C, so a raw store there moves this (p_tick2c
           tick2c.after.store) -- followed here, once, then host-now + offset again. */
        DOS_CLOCK_TIME clock; DosClockSync(); DosClockRead(g_DosClock.DosOffset, &clock);
        SET16(R_CX, ((clock.Hour & 0xFF) << 8) | (clock.Minute & 0xFF));
        SET16(R_DX, ((clock.Second & 0xFF) << 8) | (clock.Hundredths & 0xFF));
        OKCF();
    } else if (function == 0x2B || function == 0x2D) {  /* set date / set time */
        /* ── ★ GH #250: THESE ANSWERED "DONE" AND CHANGED NOTHING. ──────────────────
             A program that set the date and read it back got today. Now they move the
             VDM's clock -- an OFFSET from the host's, so the machine's own clock is
             never touched (dos_clock.h says why that is the only safe shape). AL=FFh
             for anything DOS refuses, and a refused call leaves the clock alone --
             p_clock.asm measured both, on 6.22, PCem and DOSBox-X.
           ► AND THE OTHER TWO CLOCKS FOLLOW, as they do on an AT, because DOS's CLOCK$
             driver writes them: the RTC (INT 1Ah AH=02h/04h, CMOS 00h-09h) is synced
             to the new reading, and on a time set the BIOS tick count at 0040:006C is
             reloaded with the ticks since midnight (p_clock clk.1a02.after.2d,
             clk.1a00.after.2d, clk.1a04.after.2b). */
        DOS_CLOCK_TIME host; INT isOk;
        DosClockSync();                 /* #262: 2Bh keeps the time of day the COUNT says */
        DosClockHostNow(&host);
        if (function == 0x2B) {
            UINT year = R_CX & 0xFFFF, month = (R_DX >> 8) & 0xFF, day = R_DX & 0xFF;
            isOk = DosClockIsDosDateValid(year, month, day);
            if (isOk) DosClockSetDate(&host, &g_DosClock.DosOffset, year, month, day);
        } else {
            UINT hour = (R_CX >> 8) & 0xFF, minute = R_CX & 0xFF;
            UINT second = (R_DX >> 8) & 0xFF, hundredths = R_DX & 0xFF;
            isOk = DosClockIsTimeValid(hour, minute, second, hundredths);
            if (isOk) {
                DosClockSetTime(&host, &g_DosClock.DosOffset, hour, minute, second, hundredths);
                if (machine->SetTicks) machine->SetTicks(machine->TicksContext, DosClockTicksFromTime(hour, minute, second, hundredths));
            }
        }
        if (isOk) g_DosClock.RtcOffset = g_DosClock.DosOffset;
        trace = zput(trace, "  INT21 AH=0x"); trace = zhexb(trace, (UINT)function);
        trace = zput(trace, isOk ? " VDM clock set (host clock untouched)\r\n"
                         : " refused: invalid -> AL=FF, clock unchanged\r\n");
        SETAX((R_AX & 0xFF00) | (isOk ? 0x00 : 0xFF));
        OKCF();
    } else if (function == 0x71) {              /* the long-filename API (#210) */
        /* ── ★ AH=71h, THE WINDOWS 95 LONG-FILENAME API, AS STOCK NTVDM PROVIDES IT. (#210)
             Until now this arm answered every 71xxh with AX=7100h CF=1, the documented
             "no LFN API here" (s81: 6.22's own answer, AX=7100h with CF CLEAR, had XP's
             EDIT.COM take 7100h for a file handle -- p_subfn int21.716C). Stock NTVDM
             implements the API, and XP's DOS tools are written against it.
           ► SERVED HERE: 710Dh reset drive, 7141h delete (SI=1: wildcards + CL/CH), 7143h
             attributes and times (BL 0-8), 7147h current directory (long form), 714Eh/
             714Fh/71A1h find, 7160h truename (CL 0 full / 1 short / 2 long), 71A0h volume
             information, 71A6h file info by handle, 71A7h time conversion, 71A8h short
             name, 71AAh SUBST. 7139h/713Ah/713Bh/7156h/716Ch/71A9h never reach this arm:
             they are their short-name twins (see `lfn_alias` at the top).
           ► ANYTHING ELSE -- 71A2h-71A5h, 71FFh, ... -- IS AX=7100h CF=1, the answer the
             whole API used to give and the one LFN clients test for. ⚠ That stock answers
             an unknown 71xxh this way is p_lfn's lfn.71FF row, not yet measured.
           ⚠ EVERY REGISTER CONTRACT HERE IS RBIL's, NOT A MEASUREMENT. Which registers
             stock writes on success (does 71A0h touch AX? does 7143h BL=0 copy CX into AX
             as 4300h does on 6.22?) is what tests/probes/dos/p_lfn.asm prints; the arms
             below write only the outputs RBIL names and leave AX alone on success. */
        BYTE subfunction = (BYTE)(R_AX & 0xFF);
        if (subfunction == 0x41) {       /* delete: DS:DX, SI=wildcards, CL/CH */
            CHAR fileName[300];
            WORD flags = (WORD)(R_SI & 0xFFFF);
            DosGuestPath(machine, R_DS, R_DX, fileName, sizeof(fileName));
            if (flags == 0) {
                /* SI=0: one file, no wildcards -- 41h with an LFN-shaped error code. */
                if (DeleteFileA(fileName)) OKCF();
                else { SETAX(DosLfnError(GetLastError())); ERRCF(); }
            } else {
                /* SI=1: every match of the pattern whose attributes pass CL (allowed) and
                   CH (required); directories are never deleted. Success if any went. */
                WIN32_FIND_DATAA findData; HANDLE find; INT isFound = 0, cut = 0, position;
                DWORD win32Error = ERROR_FILE_NOT_FOUND;
                CHAR fullPath[300];
                for (position = 0; fileName[position]; ++position) if (fileName[position] == '\\' || fileName[position] == '/' || fileName[position] == ':') cut = position + 1;
                find = FindFirstFileA(fileName, &findData);
                if (find == INVALID_HANDLE_VALUE) win32Error = GetLastError();
                else {
                    do {
                        if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                        if (!DosLfnAttributesOk(findData.dwFileAttributes, (BYTE)(R_CX & 0xFF),
                                             (BYTE)((R_CX >> 8) & 0xFF))) continue;
                        if (cut + lstrlenA(findData.cFileName) >= (INT)sizeof(fullPath)) continue;
                        for (position = 0; position < cut; ++position) fullPath[position] = fileName[position];
                        lstrcpynA(fullPath + cut, findData.cFileName, sizeof(fullPath) - cut);
                        if (DeleteFileA(fullPath)) isFound = 1; else win32Error = GetLastError();
                    } while (FindNextFileA(find, &findData));
                    FindClose(find);
                }
                if (isFound) OKCF(); else { SETAX(DosLfnError(win32Error)); ERRCF(); }
            }
        } else if (subfunction == 0x43) {       /* attributes and times: DS:DX, BL */
            CHAR fileName[300];
            BYTE action = (BYTE)(R_BX & 0xFF);
            WIN32_FILE_ATTRIBUTE_DATA attributeData;
            DosGuestPath(machine, R_DS, R_DX, fileName, sizeof(fileName));
            if (action == 0x00 || action == 0x02 || action == 0x04 || action == 0x06 || action == 0x08) {
                if (!GetFileAttributesExA(fileName, GetFileExInfoStandard, &attributeData)) {
                    SETAX(DosLfnError(GetLastError())); ERRCF();
                } else if (action == 0x00) {
                    /* CX = the attributes. ⚠ Masked to DOS's six bits as 4300h is (a file
                       with none set reads 0, not Win32's 80h NORMAL); unmeasured on stock. */
                    SET16(R_CX, (WORD)(attributeData.dwFileAttributes & 0x3F));
                    SETAX((WORD)(attributeData.dwFileAttributes & 0x3F));  /* stock: AX = CX too (p_lfn) */
                    OKCF();
                } else if (action == 0x02) {
                    /* DX:AX = the size the file occupies (compressed). */
                    DWORD high = 0, low;
                    SetLastError(NO_ERROR);
                    low = GetCompressedFileSizeA(fileName, &high);
                    if (low == INVALID_FILE_SIZE && GetLastError() != NO_ERROR) {
                        SETAX(DosLfnError(GetLastError())); ERRCF();
                    } else { SETAX(low & 0xFFFF); SET16(R_DX, (low >> 16) & 0xFFFF); OKCF(); }
                } else {
                    /* 4: last write -> CX time, DI date. 6: last access -> DI date.
                       8: creation -> CX time, DI date, SI 10 ms units. All LOCAL, as
                       5700h's are. A time before 1980 reads as 0. */
                    const FILETIME *targetTime = (action == 0x04) ? &attributeData.ftLastWriteTime
                                       : (action == 0x06) ? &attributeData.ftLastAccessTime
                                                        : &attributeData.ftCreationTime;
                    WORD dosDate = 0, dosTime = 0; BYTE hundredths = 0;
                    if (!DosLfnFileTimeToDos(DosFileTimeZoned(targetTime, 1), &dosDate, &dosTime, &hundredths)) { dosDate = 0; dosTime = 0; hundredths = 0; }
                    SET16(R_DI, dosDate);
                    if (action != 0x06) SET16(R_CX, dosTime);
                    if (action == 0x08) SET16(R_SI, hundredths);
                    OKCF();
                }
            } else if (action == 0x01) {        /* set attributes = CX, as 4301h */
                DWORD attributes = (DWORD)(R_CX & 0x3F);
                if (!attributes) attributes = FILE_ATTRIBUTE_NORMAL;
                if (SetFileAttributesA(fileName, attributes)) OKCF();
                else { SETAX(DosLfnError(GetLastError())); ERRCF(); }
            } else if (action == 0x03 || action == 0x05 || action == 0x07) {
                /* 3: last write = DI date, CX time. 5: last access = DI date (midnight).
                   7: creation = DI date, CX time, SI 10 ms units. Local in, UTC to Win32. */
                WORD dosDate = (WORD)(R_DI & 0xFFFF);
                WORD dosTime = (action == 0x05) ? 0 : (WORD)(R_CX & 0xFFFF);
                BYTE  hundredths = (action == 0x07) ? (BYTE)(R_SI & 0xFF) : 0;
                UINT64 fileTime64; FILETIME localTime, fileTime; HANDLE find;
                if (!DosLfnDosToFileTime(dosDate, dosTime, hundredths, &fileTime64)) { SETAX(0x0D); ERRCF(); }  /* invalid data */
                else {
                    localTime.dwLowDateTime = (DWORD)fileTime64; localTime.dwHighDateTime = (DWORD)(fileTime64 >> 32);
                    find = DosLfnOpenAttributes(fileName);
                    if (find == INVALID_HANDLE_VALUE) { SETAX(DosLfnError(GetLastError())); ERRCF(); }
                    else {
                        BOOL isOk = LocalFileTimeToFileTime(&localTime, &fileTime)
                                 && SetFileTime(find, action == 0x07 ? &fileTime : NULL,
                                                    action == 0x05 ? &fileTime : NULL,
                                                    action == 0x03 ? &fileTime : NULL);
                        DWORD win32Error = isOk ? 0 : GetLastError();
                        CloseHandle(find);
                        if (isOk) OKCF(); else { SETAX(DosLfnError(win32Error)); ERRCF(); }
                    }
                }
            } else { SETAX(1); ERRCF(); }       /* BL beyond 8: invalid function */
        } else if (subfunction == 0x47) {       /* current directory, long: DL, DS:SI */
            /* AH=47h's drive rules (0 = default, a drive Win32 cannot stand on answers
               through its =X:), then the LONG form of the path -- 47h hands back the
               short upper-case CDS form (#164), this hands back what GetLongPathNameA
               makes of it, case as the directories were created. No drive letter, no
               leading backslash, ASCIIZ (RBIL: buffer of 261 bytes). */
            CHAR currentDirectory[300], longPath[300];
            DWORD count = 0;
            BYTE driveNumber = (BYTE)(R_DX & 0xFF), currentDrive = (BYTE)(DosCurrentDrive(machine) + 1);
            if (driveNumber == 0 || driveNumber == currentDrive) {
                if (machine->VirtualDrive >= 0) {
                    CHAR driveSpec[3]; driveSpec[0] = (CHAR)('A' + machine->VirtualDrive); driveSpec[1] = ':'; driveSpec[2] = 0;
                    count = GetFullPathNameA(driveSpec, sizeof(currentDirectory), currentDirectory, NULL);
                } else count = GetCurrentDirectoryA(sizeof(currentDirectory), currentDirectory);
            } else if (driveNumber <= 26 && (GetLogicalDrives() & (1u << (driveNumber - 1)))) {
                CHAR driveSpec[3]; driveSpec[0] = (CHAR)('A' + driveNumber - 1); driveSpec[1] = ':'; driveSpec[2] = 0;
                count = GetFullPathNameA(driveSpec, sizeof(currentDirectory), currentDirectory, NULL);
            }
            if (count == 0 || count >= sizeof(currentDirectory)) { SETAX(0x0F); ERRCF(); }
            else {
                volatile BYTE *destination = (volatile BYTE *)((R_DS << 4) + (R_SI & 0xFFFF));
                PCSTR path = currentDirectory;
                INT index = 0;
                DWORD longLength = GetLongPathNameA(currentDirectory, longPath, sizeof(longPath));
                if (longLength && longLength < sizeof(longPath)) path = longPath;
                if (path[0] && path[1] == ':') path += 2;
                if (*path == '\\' || *path == '/') ++path;
                while (path[index] && index < 260) { destination[index] = (BYTE)path[index]; ++index; }
                destination[index] = 0;
                OKCF();
            }
        } else if (subfunction == 0x4E || subfunction == 0x4F) {  /* find first / next -> ES:DI */
            volatile BYTE *buffer = (volatile BYTE *)((R_ES << 4) + (R_DI & 0xFFFF));
            INT isDosFormat = (R_SI & 0xFFFF) == 1;
            WIN32_FIND_DATAA findData;
            if (subfunction == 0x4E) {
                /* DS:DX pattern, matched against long AND short names (Win32's rule, and
                   the LFN API's); CL allowed / CH required attributes; SI time format. */
                CHAR pattern[300]; HANDLE find; UINT slot;
                BYTE allowedAttributes = (BYTE)(R_CX & 0xFF), requiredAttributes = (BYTE)((R_CX >> 8) & 0xFF);
                DosGuestPath(machine, R_DS, R_DX, pattern, sizeof(pattern));
                find = FindFirstFileA(pattern, &findData);
                if (find == INVALID_HANDLE_VALUE) { SETAX(DosLfnError(GetLastError())); ERRCF(); }
                else {
                    INT isOk = 1;
                    while (!DosLfnAttributesOk(findData.dwFileAttributes, allowedAttributes, requiredAttributes))
                        if (!FindNextFileA(find, &findData)) { isOk = 0; break; }
                    if (!isOk) { FindClose(find); SETAX(18); ERRCF(); }  /* nothing passed CL/CH */
                    else {
                        for (slot = 0; slot < DOS_LFN_FIND_SLOTS && g_DosLfnFinds[slot]; ++slot) {}
                        if (slot >= DOS_LFN_FIND_SLOTS) {            /* full: recycle, round robin */
                            slot = g_DosLfnNext++ % DOS_LFN_FIND_SLOTS;
                            FindClose(g_DosLfnFinds[slot]);
                            trace = zput(trace, "  INT21 AX=714E search table full -- recycled handle 0x");
                            trace = zhex(trace, slot + 1); trace = zput(trace, "\r\n");
                        }
                        g_DosLfnFinds[slot] = find; g_DosLfnAllow[slot] = allowedAttributes; g_DosLfnNeed[slot] = requiredAttributes;
                        DosLfnFindFill(buffer, &findData, isDosFormat);
                        SETAX(slot + 1); SET16(R_CX, 0);              /* CX: no lossy names */
                        OKCF();
                    }
                }
                if (machine->IsTraceAll) { trace = zput(trace, "  INT21 AX=714E ["); trace = zput(trace, pattern);
                                    trace = zput(trace, (*guestFlags & 1) ? "] -> none\r\n" : "] -> found\r\n"); }
            } else {
                UINT slot = (UINT)(R_BX & 0xFFFF) - 1u;
                if (slot >= DOS_LFN_FIND_SLOTS || !g_DosLfnFinds[slot]) { SETAX(6); ERRCF(); }
                else {
                    INT isOk = 0;
                    while (FindNextFileA(g_DosLfnFinds[slot], &findData))
                        if (DosLfnAttributesOk(findData.dwFileAttributes, g_DosLfnAllow[slot], g_DosLfnNeed[slot])) { isOk = 1; break; }
                    /* "No more files" leaves the handle OPEN: the program closes it, 71A1h. */
                    if (!isOk) { SETAX(18); ERRCF(); }
                    else { DosLfnFindFill(buffer, &findData, isDosFormat); SET16(R_CX, 0); OKCF(); }
                }
            }
        } else if (subfunction == 0xA1) {       /* find close: BX */
            UINT slot = (UINT)(R_BX & 0xFFFF) - 1u;
            if (slot >= DOS_LFN_FIND_SLOTS || !g_DosLfnFinds[slot]) { SETAX(6); ERRCF(); }
            else { FindClose(g_DosLfnFinds[slot]); g_DosLfnFinds[slot] = 0; OKCF(); }
        } else if (subfunction == 0x60) {       /* truename: DS:SI -> ES:DI, CL form */
            /* CL=0 the full path (case kept -- 60h upper-cases, this does not: unmeasured),
               1 its SHORT form, 2 its LONG form. 1 and 2 ask the file system, so the
               path must exist; 0 does not (as 60h: "SUB\FILE.TXT" resolves anyway).
               CH (SUBST expansion) is not looked at: we create no SUBST of our own
               that a path would need unwrapping from. */
            CHAR input[300], fullPath[300], output[300];
            BYTE nameKind = (BYTE)(R_CX & 0xFF);
            DWORD count;
            DosGuestPath(machine, R_DS, R_SI, input, sizeof(input));
            count = GetFullPathNameA(input, sizeof(fullPath), fullPath, NULL);
            if (count == 0 || count >= sizeof(fullPath)) { SETAX(3); ERRCF(); }
            else if (nameKind > 2) { SETAX(1); ERRCF(); }
            else {
                if (nameKind == 0) { lstrcpynA(output, fullPath, sizeof(output)); count = 1; }
                else if (nameKind == 1) count = GetShortPathNameA(fullPath, output, sizeof(output));
                else              count = GetLongPathNameA(fullPath, output, sizeof(output));
                if (count == 0 || count >= sizeof(output)) { SETAX(DosLfnError(GetLastError())); ERRCF(); }
                else {
                    volatile BYTE *buffer = (volatile BYTE *)((R_ES << 4) + (R_DI & 0xFFFF));
                    INT index = 0;
                    while (output[index] && index < 260) { buffer[index] = (BYTE)output[index]; ++index; }
                    buffer[index] = 0;
                    SETAX(0);                           /* stock: AX=0000 (p_lfn) */
                    OKCF();
                }
            }
        } else if (subfunction == 0xA0) {       /* volume information: DS:DX root, ES:DI/CX */
            /* BX = flags as Win32 reports them, cut to the four RBIL names -- bit 0 case-
               sensitive searches, 1 case preserved, 2 Unicode on disk, 15 compressed --
               plus 4000h "supports the LFN functions", which is the bit that matters.
               CX = the longest component (255), DX = the longest path, MAX_PATH = 260.
               ES:DI gets the file-system name ("NTFS", "FAT") within CX bytes.
             ⚠ DX = 260 is RBIL's "usually"; stock may compute it. AX is left alone. */
            CHAR root[300], fileSystem[64];
            DWORD maximumComponent = 0, flags = 0;
            DosGuestString(R_DS, R_DX, root, sizeof(root));
            if (root[0] && root[1] == ':' && !root[2]) { root[2] = '\\'; root[3] = 0; }  /* Win32 wants "C:\" */
            fileSystem[0] = 0;
            if (!GetVolumeInformationA(root[0] ? root : NULL, NULL, 0, NULL, &maximumComponent, &flags, fileSystem, sizeof(fileSystem))) {
                SETAX(DosLfnError(GetLastError())); ERRCF();
            } else {
                volatile BYTE *buffer = (volatile BYTE *)((R_ES << 4) + (R_DI & 0xFFFF));
                UINT capacity = (UINT)(R_CX & 0xFFFF), index;
                for (index = 0; capacity && index < capacity - 1 && fileSystem[index]; ++index) buffer[index] = (BYTE)fileSystem[index];
                if (capacity) buffer[index] = 0;
                SET16(R_BX, (WORD)((flags & 0x0007) | (flags & 0x8000) | 0x4000));
                SET16(R_CX, (WORD)(maximumComponent ? maximumComponent : 255));
                SET16(R_DX, 260);
                OKCF();
            }
            trace = zput(trace, "  INT21 AX=71A0 ["); trace = zput(trace, root);
            trace = zput(trace, "] fs="); trace = zput(trace, fileSystem); trace = zput(trace, " flags=0x"); trace = zhex(trace, flags);
            trace = zput(trace, (*guestFlags & 1) ? " (err)\r\n" : "\r\n");
        } else if (subfunction == 0xA6) {       /* file info by handle: BX -> DS:DX */
            /* BY_HANDLE_FILE_INFORMATION, 52 bytes, exactly as Win32 lays it out (times
               UTC, as a FILETIME is). */
            DWORD handle = R_BX & 0xFFFF;
            BY_HANDLE_FILE_INFORMATION fileInfo;
            if (!DosHandleIsFile((PVOID const *)machine->FileHandles, handle)) { SETAX(6); ERRCF(); }
            else if (!GetFileInformationByHandle(machine->FileHandles[handle], &fileInfo)) { SETAX(DosLfnError(GetLastError())); ERRCF(); }
            else {
                volatile BYTE *buffer = (volatile BYTE *)((R_DS << 4) + (R_DX & 0xFFFF));
                PCBYTE infoBytes = (PCBYTE)&fileInfo;
                UINT index;
                for (index = 0; index < sizeof(fileInfo) && index < 52; ++index) buffer[index] = infoBytes[index];
                OKCF();
            }
        } else if (subfunction == 0xA7) {       /* time conversion, BL */
            BYTE action = (BYTE)(R_BX & 0xFF);
            /* ⚠ THE ZONE IS A CHOICE, NOT A MEASUREMENT: a FILETIME here is taken as UTC
                 (what 714Eh SI=0 and 71A6h hand out) and the DOS side as LOCAL (what every
                 DOS time this host reports is), so FILETIME -> DOS converts to local time
                 and back. Windows 95's IFSMgr converts the same way; whether NTVDM does is
                 p_lfn's lfn.71A7.ft2dos -- on a machine whose zone is not UTC the hour
                 differs by the offset if this is wrong. */
            if (action == 0x00) {               /* DS:SI -> QWORD FILETIME -> CX time, DX date, BH */
                const volatile BYTE *bytes = (const volatile BYTE *)((R_DS << 4) + (R_SI & 0xFFFF));
                FILETIME fileTime; WORD dosDate, dosTime; BYTE hundredths;
                fileTime.dwLowDateTime  = (DWORD)bytes[0] | ((DWORD)bytes[1] << 8) | ((DWORD)bytes[2] << 16) | ((DWORD)bytes[3] << 24);
                fileTime.dwHighDateTime = (DWORD)bytes[4] | ((DWORD)bytes[5] << 8) | ((DWORD)bytes[6] << 16) | ((DWORD)bytes[7] << 24);
                if (!DosLfnFileTimeToDos(DosFileTimeZoned(&fileTime, 1), &dosDate, &dosTime, &hundredths)) { SETAX(0x0D); ERRCF(); }
                else {
                    SET16(R_CX, dosTime); SET16(R_DX, dosDate);
                    /* ⚠ INTENDED DIVERGENCE (s92): for an exact even second stock answers
                         BH=C7h (199) -- p_lfn lfn.71A7.ft2dos, one measurement -- where the
                         spec's 10-ms remainder is 0. The spec outranks one oracle reading. */
                    SET16(R_BX, (WORD)((R_BX & 0xFF) | ((WORD)hundredths << 8)));
                    OKCF();
                }
            } else if (action == 0x01) {        /* CX time, DX date, BH -> ES:DI QWORD */
                UINT64 fileTime64; FILETIME localTime, fileTime;
                if (!DosLfnDosToFileTime((WORD)(R_DX & 0xFFFF), (WORD)(R_CX & 0xFFFF),
                                       (BYTE)((R_BX >> 8) & 0xFF), &fileTime64)) { SETAX(0x0D); ERRCF(); }
                else {
                    volatile BYTE *bytes = (volatile BYTE *)((R_ES << 4) + (R_DI & 0xFFFF));
                    INT index;
                    localTime.dwLowDateTime = (DWORD)fileTime64; localTime.dwHighDateTime = (DWORD)(fileTime64 >> 32);
                    if (!LocalFileTimeToFileTime(&localTime, &fileTime)) fileTime = localTime;
                    for (index = 0; index < 4; ++index) {
                        bytes[index]     = (BYTE)(fileTime.dwLowDateTime  >> (8 * index));
                        bytes[4 + index] = (BYTE)(fileTime.dwHighDateTime >> (8 * index));
                    }
                    OKCF();
                }
            } else { SETAX(1); ERRCF(); }
        } else if (subfunction == 0xA8) {       /* generate short name: DS:SI -> ES:DI, DH */
            /* DH=0: 11 bytes, FCB style; DH=1: "NAME.EXT" ASCIIZ. DL's character-set
               nibbles are not looked at -- see the code-page note at the helpers. */
            CHAR longName[300], shortName[13], fcbName[11];
            volatile BYTE *buffer = (volatile BYTE *)((R_ES << 4) + (R_DI & 0xFFFF));
            INT index;
            DosGuestString(R_DS, R_SI, longName, sizeof(longName));
            DosLfnShortName(longName, shortName, fcbName);
            if (((R_DX >> 8) & 0xFF) == 0) for (index = 0; index < 11; ++index) buffer[index] = (BYTE)fcbName[index];
            else { for (index = 0; shortName[index]; ++index) buffer[index] = (BYTE)shortName[index]; buffer[index] = 0; }
            OKCF();
        } else if (subfunction == 0xAA) {       /* SUBST: BH 0 create / 1 terminate / 2 query */
            /* BL = drive (0 = default, 1 = A:). The host's own DOS-device table is what a
               SUBST is on NT (XP's SUBST.EXE is DefineDosDevice), so this is real: a drive
               created here is visible to the user's session until terminated or logoff.
             ⚠ TERMINATE ONLY UNDOES A SUBST -- a letter whose NT target is "\??\..." --
               never a real disk or a network mapping; anything else is 0Fh. */
            BYTE action = (BYTE)((R_BX >> 8) & 0xFF), driveNumber = (BYTE)(R_BX & 0xFF);
            CHAR driveSpec[3], target[300];
            BYTE drive = (BYTE)(driveNumber ? driveNumber - 1 : DosCurrentDrive(machine));
            driveSpec[0] = (CHAR)('A' + (drive < 26 ? drive : 0)); driveSpec[1] = ':'; driveSpec[2] = 0;
            target[0] = 0;
            if (drive >= 26 || action > 2) { SETAX(action > 2 ? 1 : 0x0F); ERRCF(); }
            else if (action == 0) {
                CHAR input[300], fullPath[300];
                DWORD count;
                DosGuestPath(machine, R_DS, R_DX, input, sizeof(input));
                count = GetFullPathNameA(input, sizeof(fullPath), fullPath, NULL);
                if (GetLogicalDrives() & (1u << drive)) { SETAX(0x0F); ERRCF(); }   /* letter in use */
                else if (count == 0 || count >= sizeof(fullPath)) { SETAX(3); ERRCF(); }
                else if (DefineDosDeviceA(0, driveSpec, fullPath)) {
                    OKCF();
                    trace = zput(trace, "  INT21 AX=71AA SUBST "); trace = zput(trace, driveSpec);
                    trace = zput(trace, " = "); trace = zput(trace, fullPath); trace = zput(trace, " (a host drive)\r\n");
                } else { SETAX(DosLfnError(GetLastError())); ERRCF(); }
            } else {
                DWORD length = QueryDosDeviceA(driveSpec, target, sizeof(target));
                INT isSubst = length > 4 && target[0] == '\\' && target[1] == '?' && target[2] == '?' && target[3] == '\\';
                if (!isSubst) { SETAX(action == 2 ? 0x89 : 0x0F); ERRCF(); }  /* stock query: 89h (p_lfn) */
                else if (action == 1) {
                    if (DefineDosDeviceA(DDD_REMOVE_DEFINITION, driveSpec, NULL)) OKCF();
                    else { SETAX(DosLfnError(GetLastError())); ERRCF(); }
                } else {
                    volatile BYTE *buffer = (volatile BYTE *)((R_DS << 4) + (R_DX & 0xFFFF));
                    INT index = 0;
                    while (target[4 + index] && index < 260) { buffer[index] = (BYTE)target[4 + index]; ++index; }
                    buffer[index] = 0;
                    OKCF();
                }
            }
        } else {
            trace = zput(trace, "  INT21 AX=71"); trace = zhexb(trace, subfunction);
            trace = zput(trace, " not an LFN function stock provides -> AX=0001 CF=1\r\n");
            /* s92, MEASURED (dospair p_lfn): stock answers an unknown 71xxh -- and 710Dh,
               which it does not provide -- with AX=0001 CF=1, "invalid function", NOT the
               7100h this arm assumed. */
            SETAX(0x0001);
            ERRCF();
        }
        if (machine->IsTraceAll) {
            trace = zput(trace, "  INT21 AX=71"); trace = zhexb(trace, subfunction);
            trace = zput(trace, " -> AX=0x"); trace = zhex(trace, R_AX & 0xFFFF);
            trace = zput(trace, (*guestFlags & 1) ? " (err)\r\n" : "\r\n");
        }
    } else if (!DosIsDefinedBy622(function)) {
        /* MS-DOS 6.22 has nothing here, and what IT does is the specification:
           return with AL cleared and CF clear, touching nothing else.  Measured on
           the oracle (tests/probes/dos/p_defs.asm) -- AH=6Dh..E0h, plus the
           documented null functions, all come back with every poisoned output
           register intact.  Failing loudly here would be US inventing an error
           that real DOS does not report, which breaks programs that probe for
           an extension by calling it and checking CF. */
        /* ⛔ "AX unchanged" WAS HALF A MEASUREMENT (s81). p_defs and p_unimp both
             called with AL=00h, so "AX unchanged" and "AL cleared" read the same. With
             AL non-zero (p_subfn: AX=716Ch, 7147h) 6.22 and PCem both answer AX=7100h:
             DOS ZEROES AL. And that is load-bearing: AX=7100h is exactly how a long-
             filename client learns there is no LFN API -- XP's EDIT.COM took our
             unchanged 716Ch for a file handle and reported "Error 6" on EDIT.INI. */
        SETAX(R_AX & 0xFF00);
        trace = zput(trace, "  INT21 AH=0x"); trace = zhex(trace, function);
        trace = zput(trace, " undefined on 6.22 -- AL=0, CF clear (matches DOS)\r\n");
        machine->Undefined[(function & 0xFF) >> 3] |= (BYTE)(1u << (function & 7));
        OKCF();
    } else {                                    /* unhandled service */
        /* GH #27. Recorded as well as logged, so the STAGE2 block can list every
           service a run actually wanted -- that list is the to-do list.
           Reaching HERE means 6.22 defines a real service at this AH and we have
           not written it yet.  CF=1 is right for that: a quiet "success" would
           tell the program its request worked when nothing happened.  Functions
           DOS does not define are handled above and stay silent, matching DOS. */
        trace = zput(trace, "  INT21 AH=0x"); trace = zhexb(trace, (UINT)function);
        trace = zput(trace, " AL=0x"); trace = zhexb(trace, (UINT)(R_AX & 0xFF));
        trace = zput(trace, " UNIMPLEMENTED\r\n");
        machine->Unimplemented[(function & 0xFF) >> 3] |= (BYTE)(1u << (function & 7));
        ERRCF();
    }

    /* GH #34: remember the last failure for AH=59h. Done HERE, once, rather
       than at each of the ~20 error sites -- CF and AX are already exactly what
       the guest is about to see. 59h itself is excluded so reading the error
       does not overwrite it. */
    if (function != 0x59 && (*guestFlags & 1)) machine->LastError = (WORD)(R_AX & 0xFFFF);

    /* ── #34: A HARDWARE ERROR IS A CRITICAL ERROR. Codes 19-31 (not ready, write-
         protected, ...) go to the program's INT 24h before the call returns; the host
         makes that call (crit_raise in main.c) and acts on the answer. Here we only
         say so, and what the handler is to be told. Real mode only (a DPMI client's
         reflection is separate work), and never while a handler is already running:
         DOS does not nest INT 24h -- inside one, the call just fails. */
    /* ── #275: ...AND ONLY WHERE IT CAN BE. crit_raise_ok is set by the main V86 exec
         loop alone (it is the one that acts on crit_pending); #34 keyed this on "not
         protected mode", so the nested real-mode loops -- a DPMI 0301h/0302h
         procedure, a reflected IRQ's handler -- set crit_pending and nobody raised it.
         A handle call (3Fh/40h, the file's own drive in AL) is now raised too. */
    /* s92, MEASURED (dospair p_lfn lfn.713B.missing / 713A.again): stock's LFN chdir and
       rmdir say 2 (file not found) for a directory that is not there, where 6.22's short
       3Bh/3Ah -- whose code serves them -- say 3. */
    if (isLfnAlias && (isLfnAlias == 0x3A || isLfnAlias == 0x3B) && (*guestFlags & 1)
        && (R_AX & 0xFFFF) == 3) SETAX(2);
    if ((*guestFlags & 1) && machine->CanRaiseCrit && !g_DosInt21IsProtectedMode && !machine->IsCritActive
        && DosCritIsHardwareError((WORD)(R_AX & 0xFFFF))) {
        const volatile BYTE *pathBytes = (const volatile BYTE *)((R_DS << 4) + (R_DX & 0xFFFF));
        INT isPathCall = (function == 0x3C || function == 0x3D || function == 0x4E || function == 0x39 || function == 0x3A
                        || function == 0x3B || function == 0x41 || function == 0x43 || function == 0x5A || function == 0x5B);
        BYTE drive = DosCurrentDrive(machine);
        if (isPathCall && pathBytes[1] == ':') drive = (BYTE)((pathBytes[0] | 0x20) - 'a');
        if ((function == 0x3F || function == 0x40) && g_DosReadWriteDrive >= 0) drive = (BYTE)g_DosReadWriteDrive;
        machine->IsCritPending = 1;
        machine->CritAl = drive;
        machine->CritAh = DosCritInt24Ah((BYTE)function);
        machine->CritCode = (BYTE)((R_AX & 0xFF) - 19);
    }
    /* ── #275: A 3Fh/40h HARDWARE ERROR WHERE INT 24h CANNOT BE RAISED IS ANSWERED AS
         FAIL. Three such places: (1) inside the program's own INT 24h handler -- DOS
         never nests one, and MS-DOS 4.0's HardErr (CTRLC.ASM, GOT_RIGHT_CODE) answers
         `AL=3` itself when ERRORMODE is set, i.e. FAIL; (2) a DPMI client's INT 21h,
         served here in protected mode; (3) the nested real-mode loops (crit_raise_ok
         clear). FAIL is the answer our default INT 24h handler (`mov al,3 / iret`)
         gives, and the only one that neither loops (RETRY) nor ends the program
         (ABORT) nor invents data (IGNORE) behind the caller's back.
       ⚠ DPMI, BY THE SPEC, IS NOT THIS. DPMI 0.9 has DOS raise INT 24h in REAL mode
         and the host reflect it to the client's protected-mode INT 24h handler if it
         installed one (0205h), else run the real-mode vector -- usually COMMAND.COM's
         "Abort, Retry, Fail?". We serve a PM client's INT 21h host-side, so there is no
         real-mode DOS call to raise it from; doing it properly needs the 0301h nested-
         V86 run factored out of the INT 31h switch (main.c) so the host can run the
         real-mode INT 24h vector, or a PM handler, from inside a PM INT 21h. Open.
       ⚠ PATH CALLS ARE LEFT AS #34 LEFT THEM (the raw 19-31 code) in these places:
         changing what a DPMI client -- krnl386 and every Win16 program included --
         sees for a drive probe on an empty A: is a behaviour change nobody has asked
         for or measured. 3Fh/40h had no previous answer worth keeping (it was a false
         success). */
    else if ((*guestFlags & 1) && (function == 0x3F || function == 0x40)
             && DosCritIsHardwareError((WORD)(R_AX & 0xFFFF))) {
        WORD code = (WORD)(R_AX & 0xFFFF);
        SETAX(DosCritFailAx((BYTE)function, (BYTE)(code - 19)));
        machine->LastError = DOS_ERR_FAIL_I24;
        trace = zput(trace, "  INT24 not raised (");
        trace = zput(trace, machine->IsCritActive ? "inside the handler" : g_DosInt21IsProtectedMode ? "DPMI client"
                                     : "nested real-mode call");
        trace = zput(trace, "): error 0x"); trace = zhexb(trace, (UINT)code);
        trace = zput(trace, " answered as FAIL -> AX=0x"); trace = zhex(trace, R_AX & 0xFFFF);
        trace = zput(trace, ", 59h=53h\r\n");
    }

    /* ── s91: KEEP THE PSP's JFT TRUTHFUL (see jft_known). V86 only: in protected mode
         the flags are not on a V86 stack and a DPMI client's JFT is not ours to show. */
    if (!g_DosInt21IsProtectedMode && !(*guestFlags & 1)) {
        if (function == 0x3C || function == 0x3D || function == 0x5A || function == 0x5B || function == 0x6C)
            DosJftPut(machine, (UINT)(R_AX & 0xFFFF), DosSftValue(machine, (UINT)(R_AX & 0xFFFF)));
        else if (function == 0x45 || function == 0x46) {
            UINT sourceHandle = (UINT)(R_BX & 0xFFFF);
            UINT targetHandle = (function == 0x45) ? (UINT)(R_AX & 0xFFFF) : (UINT)(R_CX & 0xFFFF);
            DosJftPut(machine, targetHandle, DosSftValue(machine, sourceHandle));
        } else if (function == 0x3E)
            DosJftPut(machine, (UINT)(R_BX & 0xFFFF), 0xFF);
    }

    /* ── AND WHAT WE ANSWERED, WHICH IS THE HALF THAT WAS MISSING. ──────────────
         The entry trace above prints the call; it did not print the RESULT, so a
         run said what the guest asked and never what it was told. That is only
         half a differential instrument: XP's COMMAND.COM makes 31 calls and then
         terminates, and "which one came back an error" is the whole question --
         unanswerable from the inbound line alone.
       ⚠ Same flag, same AH=0Ah exclusion AND THE SAME CAP, so the pairing stays
         one-to-one and a reader can line `21:xx/yy` up with the `->` under it. The cap
         has to be shared: capping only the inbound half would leave a file of orphaned
         results, which is worse than either. */
    if (machine->IsTraceAll && function != 0x0A && machine->TraceCount <= DOS_TRACE_MAX) {
        trace = zput(trace, "     -> ax="); trace = zhexb(trace, (UINT)((R_AX >> 8) & 0xFF));
        trace = zhexb(trace, (UINT)(R_AX & 0xFF));
        trace = zput(trace, " cf="); trace = zhexb(trace, (UINT)(*guestFlags & 1));
        trace = zput(trace, "\r\n");
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
