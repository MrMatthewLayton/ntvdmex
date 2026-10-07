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

INT DosInt21(PDOS_MACHINE m)
{
    volatile BYTE *tib = m->Tib;
    PSTR tp = m->TraceCursor;
    volatile WORD *pfl;
    DWORD ah;
    INT cont = 1;
    DWORD cl_seg = 0, cl_off = 0;               /* the guest's own call site -- below */
    INT   cl_ok  = 0;
    BYTE lfn_alias = 0;                         /* #210: the AL of a 71xxh served by xxh */

    #define R_AX VDM_REG(tib, VTIB_EAX)
    #define R_BX VDM_REG(tib, VTIB_EBX)
    #define R_CX VDM_REG(tib, VTIB_ECX)
    #define R_DX VDM_REG(tib, VTIB_EDX)
    #define R_DS VDM_REG(tib, VTIB_DS)
    #define R_ES VDM_REG(tib, VTIB_ES)
    #define R_SI VDM_REG(tib, VTIB_ESI)
    #define R_DI VDM_REG(tib, VTIB_EDI)
    #define SETAX(v)    (R_AX = (R_AX & 0xFFFF0000u) | ((DWORD)(v) & 0xFFFF))
    #define SET16(r, v) ((r)  = ((r)  & 0xFFFF0000u) | ((DWORD)(v) & 0xFFFF))
    #define OKCF()      (*pfl &= (WORD)~1)
    #define ERRCF()     (*pfl |= 1)
    #define SETZF()     (*pfl |= 0x40)
    #define CLRZF()     (*pfl &= (WORD)~0x40)
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
    #define OUTC(c)     do { BYTE _ch = (BYTE)(c); \
        if (m->FileHandles[1]) { DWORD _w = 0; WriteFile(m->FileHandles[1], &_ch, 1, &_w, NULL); } \
        else { \
            if (m->OutputLength < m->OutputCapacity - 1) m->Output[m->OutputLength++] = (CHAR)_ch; \
            else m->IsOutputTruncated = 1; \
            if (m->ConsoleOut) m->ConsoleOut(m->ConsoleOutContext, _ch); \
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
    pfl = g_DosInt21IsProtectedMode
        ? (volatile WORD *)(tib + VTIB_EFLAGS)
        : (volatile WORD *)(((VDM_REG(tib, VTIB_SS) & 0xFFFF) << 4)
                            + (((VDM_REG(tib, VTIB_ESP) & 0xFFFF) + 4) & 0xFFFF));
    ah = (R_AX >> 8) & 0xFF;
    m->Trampoline = 0;
    /* #251: resume the V86 guest in the AUX/PRN driver code -- see dos_auxprn.asm. */
    #define AUXPRN_TRAMP(e) (m->Trampoline = (WORD)(DOS_AUXPRN_OFF + (e)))
    #define AUXPRN_V86      (m->CanTrampoline && !g_DosInt21IsProtectedMode)
    m->IsCritPending = 0;     /* #34: only ever about THIS call; see the tail */
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
        DWORD sb = (VDM_REG(tib, VTIB_SS) & 0xFFFF) << 4;
        DWORD sp = VDM_REG(tib, VTIB_ESP) & 0xFFFF;
        const volatile BYTE *fr = (const volatile BYTE *)(ULONG_PTR)(sb + sp);
        cl_off = (DWORD)fr[0] | ((DWORD)fr[1] << 8);
        cl_seg = (DWORD)fr[2] | ((DWORD)fr[3] << 8);
        cl_ok  = 1;
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
    if (m->IsTraceAll && ah != 0x0A && m->TraceCount <= DOS_TRACE_MAX) {
        if (++m->TraceCount > DOS_TRACE_MAX) {
            tp = zput(tp, "  21: ... TRACE CAPPED at ");
            tp = zhex(tp, DOS_TRACE_MAX);
            tp = zput(tp, " calls -- the guest is looping; totals are in the summary\r\n");
        } else {
        tp = zput(tp, "  21:"); tp = zhexb(tp, (UINT)ah);
        tp = zput(tp, "/");     tp = zhexb(tp, (UINT)(R_AX & 0xFF));
        tp = zput(tp, " bx="); tp = zhexb(tp, (UINT)((R_BX >> 8) & 0xFF));
        tp = zhexb(tp, (UINT)(R_BX & 0xFF));
        tp = zput(tp, " dx="); tp = zhexb(tp, (UINT)((R_DX >> 8) & 0xFF));
        tp = zhexb(tp, (UINT)(R_DX & 0xFF));
        /* The call site, so a trace of 31 calls says WHERE the guest is, not only
           what it wanted. Two calls from the same offset are a loop; a run of
           rising offsets is start-up walking forward. */
        if (cl_ok) { tp = zput(tp, " @"); tp = zhex(tp, cl_seg);
                     tp = zput(tp, ":"); tp = zhex(tp, cl_off); }
        tp = zput(tp, "\r\n");
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
    if (ah == 0x71) {
        BYTE al71 = (BYTE)(R_AX & 0xFF);
        if (al71 == 0x39 || al71 == 0x3A || al71 == 0x3B || al71 == 0x56 || al71 == 0x6C
            || al71 == 0xA9) {
            lfn_alias = al71;
            ah = (al71 == 0xA9) ? 0x6C : al71;
        }
    }

    if (ah == 0x4C) {                           /* terminate */
        m->ExitCode = (INT)(R_AX & 0xFF);       /* DOS errorlevel */
        tp = zput(tp, "  ==> DOS terminate (AH=4Ch), exit code AL=0x");
        tp = zhex(tp, R_AX & 0xFF);
        tp = DosInt21CallSite(tp, cl_ok, cl_seg, cl_off);
        tp = zput(tp, "\r\n");
        cont = 0;
    } else if (ah == 0x00) {                    /* terminate (CP/M style, = INT 20h) */
        /* Skyroads exits through this one, so "unhandled" was both wrong and misleading:
           we returned an error and let the guest run on into nowhere. It is just 4Ch with
           an exit code of 0. Logging the call site because WHY a game terminates is the
           question, and the CS tells you whether it was the program or something we
           vectored it into. */
        m->ExitCode = 0;
        tp = zput(tp, "  ==> DOS terminate (AH=00h) from CS:IP=0x");
        tp = zhex(tp, VDM_REG(tib, VTIB_CS) & 0xFFFF); tp = zput(tp, ":0x");
        tp = zhex(tp, VDM_REG(tib, VTIB_EIP) & 0xFFFF);
        tp = zput(tp, " ivt8=0x");
        { const volatile BYTE *z = (const volatile BYTE *)0;
          DWORD s8 = (DWORD)z[0x22] | ((DWORD)z[0x23] << 8);
          DWORD o8 = (DWORD)z[0x20] | ((DWORD)z[0x21] << 8);
          DWORD sc = (DWORD)z[0x72] | ((DWORD)z[0x73] << 8);
          DWORD oc = (DWORD)z[0x70] | ((DWORD)z[0x71] << 8);
          tp = zhex(tp, s8); tp = zput(tp, ":0x"); tp = zhex(tp, o8);
          tp = zput(tp, " ivt1C=0x"); tp = zhex(tp, sc);
          tp = zput(tp, ":0x"); tp = zhex(tp, oc); }
        /* ── AND THE BYTES THAT LED HERE -- AT THE GUEST'S ADDRESS, NOT OURS.
             "SILENT VDM DEATH -> GET THE BYTES" is a standing rule here, and the
             first cut of this obeyed the letter of it while dumping from
             VTIB_CS:EIP -- the HANDLER's address. That produced `C4 C4 54`, read
             as a BOP marker, and two conclusions that were both retracted a
             session later. The guest's own call site is the pushed return address
             on its stack; DosInt21CallSite() dumps around that. */
        tp = DosInt21CallSite(tp, cl_ok, cl_seg, cl_off);
        tp = zput(tp, "\r\n");
        cont = 0;
    } else if (ah == 0x02) {                    /* print char DL */
        OUTC(R_DX & 0xFF); OKCF();
    } else if ((ah == 0x01 || ah == 0x07 || ah == 0x08 || ah == 0x0B
                || (ah == 0x06 && (R_DX & 0xFF) == 0xFF))
               && DosHandleIsFile((PVOID const *)m->FileHandles, 0)) {
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
        HANDLE fh0 = (HANDLE)m->FileHandles[0];
        BYTE ch = 0; DWORD got = 0;
        if (ah == 0x0B) {
            DWORD pos = SetFilePointer(fh0, 0, NULL, FILE_CURRENT);
            DWORD sz  = GetFileSize(fh0, NULL);
            SETAX((R_AX & 0xFF00) | ((pos != INVALID_SET_FILE_POINTER && pos < sz) ? 0xFF : 0x00));
            OKCF();
        } else if (!ReadFile(fh0, &ch, 1, &got, NULL) || got == 0) {
            if (ah == 0x06) { SETAX(R_AX & 0xFF00); SETZF(); OKCF(); }
            else m->IsRetry = 1;                /* EOF: block, as DOS does */
        } else {
            if (ah == 0x01) OUTC(ch);
            SETAX((R_AX & 0xFF00) | ch);
            if (ah == 0x06) CLRZF();
            OKCF();
        }
    } else if (ah == 0x01 || ah == 0x07 || ah == 0x08) {   /* read char (01 echoes) */
        /* Poll, do not block. If no key is waiting we ask the host to re-run this INT
           rather than parking the exec thread -- see `retry` in dos_int21.h. */
        INT c = m->ConsoleInNoWait ? m->ConsoleInNoWait(m->ConsoleInContext) : -1;
        if (c < 0) { m->IsRetry = 1; }
        else {
            if (ah == 0x01) OUTC(c);            /* AH=01: echo                     */
            SETAX((R_AX & 0xFF00) | (c & 0xFF)); OKCF();
        }
    } else if (ah == 0x0A && DosHandleIsFile((PVOID const *)m->FileHandles, 0)) {
        /* stdio (s91): the line from a FILE on handle 0 -- bytes up to the CR, the
           LF a text file puts after it skipped at the start of the next line, each
           echoed as the keyboard form echoes them. At EOF with nothing read it
           blocks, as 08h does (p_stdin: PCem + stock). */
        volatile BYTE *buf = (volatile BYTE *)((R_DS << 4) + (R_DX & 0xFFFF));
        INT maxn = buf[0], n = 0;
        BYTE ch; DWORD got;
        HANDLE fh0 = (HANDLE)m->FileHandles[0];
        for (;;) {
            if (!ReadFile(fh0, &ch, 1, &got, NULL) || got == 0) break;
            if (ch == 0x0A && n == 0) continue;
            if (ch == 0x0D) break;
            if (n < maxn - 1) { buf[2 + n++] = ch; OUTC(ch); }
        }
        if (n == 0 && got == 0) m->IsRetry = 1;
        else {
            buf[1] = (BYTE)n; buf[2 + n] = 0x0D;
            OUTC(0x0D); OUTC(0x0A);
            OKCF();
        }
    } else if (ah == 0x0A) {                    /* buffered input DS:DX */
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
        volatile BYTE *buf = (volatile BYTE *)((R_DS << 4) + (R_DX & 0xFFFF));
        INT maxn = buf[0], c;
        if (!m->IsLineActive || m->LineSegment != (WORD)(R_DS & 0xFFFF)
                            || m->LineOffset != (WORD)(R_DX & 0xFFFF)) {
            m->IsLineActive = 1; m->LineLength = 0;
            m->LineSegment = (WORD)(R_DS & 0xFFFF);
            m->LineOffset = (WORD)(R_DX & 0xFFFF);
        }
        for (;;) {
            if (m->LineLength >= maxn - 1) break;       /* buffer full -> take it as a line */
            c = m->ConsoleInNoWait ? m->ConsoleInNoWait(m->ConsoleInContext) : 0x0D;
            if (c < 0) { m->IsRetry = 1; break; }       /* nothing yet -> let the guest run */
            /* s91: a LF at the START of a line is the tail of the previous line's
               CR LF in a redirected file (`command < script`, cmd's `<` lands on the
               host stdin this reads). Every line after the first began with it and
               6.22's COMMAND.COM answered "Bad command or file name" to each. */
            if (c == 0x0A && m->LineLength == 0) continue;
            if (c == 0x0D) { m->IsLineActive = 0; break; }
            if (c == 0x08) {                            /* backspace: rub it out on screen */
                if (m->LineLength > 0) { --m->LineLength; OUTC(0x08); OUTC(' '); OUTC(0x08); }
                continue;
            }
            if (c == 0x00) continue;                    /* extended key: no ASCII, ignore  */
            buf[2 + m->LineLength++] = (BYTE)c; OUTC(c);
        }
        if (!m->IsRetry) {
            buf[1] = (BYTE)m->LineLength; buf[2 + m->LineLength] = 0x0D;
            OUTC(0x0D); OUTC(0x0A);
            m->IsLineActive = 0;
            /* WHAT THE SHELL ACTUALLY RECEIVES. `echo hi` works while a bare `ver`
               comes back "Bad command or file name" -- and the difference between
               them is a SPACE, i.e. whether the command word ends at a delimiter or
               at our terminator. That points straight at these bytes, so print them
               rather than reason about them. */
            if (m->IsTraceAll) { INT k;
              tp = zput(tp, "  INT21 AH=0A line max="); tp = zhexb(tp, (UINT)maxn);
              tp = zput(tp, " n="); tp = zhexb(tp, (UINT)m->LineLength);
              tp = zput(tp, " [");
              for (k = 0; k < m->LineLength + 1 && k < 64; ++k) {
                  tp = zhexb(tp, buf[2 + k]); tp = zput(tp, " ");
              }
              tp = zput(tp, "]\r\n"); }
            OKCF();
        }
    } else if (ah == 0x0B) {                    /* check input status */
        INT ready = m->ConsolePeek ? m->ConsolePeek(m->ConsoleInContext) : 0;
        SETAX((R_AX & 0xFF00) | (ready ? 0xFF : 0x00));   /* FFh = char waiting */
        OKCF();
    } else if (ah == 0x06) {                    /* direct console I/O (DL=FF -> read) */
        if ((R_DX & 0xFF) == 0xFF) {            /* input: non-blocking, ZF=1 if none */
            INT c = m->ConsoleInNoWait ? m->ConsoleInNoWait(m->ConsoleInContext) : -1;
            if (c >= 0) { SETAX((R_AX & 0xFF00) | (c & 0xFF)); CLRZF(); }
            else        { SETAX(R_AX & 0xFF00); SETZF(); }
        } else {                                /* output: write DL, AL=DL */
            OUTC(R_DX & 0xFF); SETAX((R_AX & 0xFF00) | (R_DX & 0xFF));
        }
        OKCF();
    } else if (ah == 0x09) {                    /* print $-string DS:DX */
        const volatile BYTE *s = (const volatile BYTE *)((R_DS << 4) + (R_DX & 0xFFFF));
        INT k; for (k = 0; k < 1024 && *s != '$'; ++k, ++s) OUTC(*s);
        OKCF();
    } else if (ah == 0x40) {                    /* write: BX=handle CX=cnt DS:DX=buf */
        DWORD h = R_BX & 0xFFFF, cnt = R_CX & 0xFFFF;
        PCSTR b = (PCSTR)((R_DS << 4) + (R_DX & 0xFFFF));
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
        if (DosHandleIsFile((PVOID const *)m->FileHandles, h)) {
            DWORD w = 0, we = 0; WORD de = 0;
            if (!WriteFile(m->FileHandles[h], b, cnt, &w, NULL)) we = GetLastError();
            if (we && DosErrFromWin32((unsigned long)we, &de) && DosCritIsHardwareError(de)) {
                g_DosReadWriteDrive = DosHandleDrive(m->FileHandles[h]);
                SETAX(de); ERRCF();
                tp = zput(tp, "  INT21 AH=40 h="); tp = zhex(tp, h);
                tp = zput(tp, " cnt=0x"); tp = zhex(tp, cnt);
                tp = zput(tp, " FAILED win32=0x"); tp = zhex(tp, we);
                tp = zput(tp, " (hardware) drive=");
                if (g_DosReadWriteDrive >= 0) { CHAR dl[3] = { (CHAR)('A' + g_DosReadWriteDrive), ':', 0 }; tp = zput(tp, dl); }
                else tp = zput(tp, "?");
                tp = zput(tp, "\r\n");
            } else { SETAX(w); OKCF(); DosStampVdmNow(m->FileHandles[h]); /* #263 */ }
        }
        /* ── #251: AN UNREDIRECTED 3 IS AUX AND 4 IS PRN, and they go to the BIOS
             (INT 14h / INT 17h) like DOS's own drivers -- they used to be refused
             with error 6 here, after AH=04h/05h had thrown their bytes away. */
        else if ((h == 3 || h == 4) && DosHandleIsDevice((PVOID const *)m->FileHandles, m->StdOpen, h)) {
            if (AUXPRN_V86) AUXPRN_TRAMP(h == 4 ? DOS_AUXPRN_PRN_WRITE : DOS_AUXPRN_AUX_WRITE);
            else {
                DWORD k;
                for (k = 0; k < cnt; ++k) {
                    if (h == 4) { if (m->PrinterOut) (VOID)m->PrinterOut(m->DeviceContext, (BYTE)b[k]); }
                    else if (m->AuxOut) m->AuxOut(m->DeviceContext, (BYTE)b[k]);
                }
                SETAX(cnt); OKCF();
            }
        }
        /* ⚠ ANY device slot, not just 1 and 2 -- after AH=45h the console can be
             sitting in slot 5. A duplicate loses which device it was, so a dup of
             AUX would print here; nothing does that, and the alternative is a
             per-slot identity byte we have no caller for. */
        else if (DosHandleIsDevice((PVOID const *)m->FileHandles, m->StdOpen, h))
             { DWORD k; for (k = 0; k < cnt; ++k) OUTC(b[k]); SETAX(cnt); OKCF(); }
        else { SETAX(6); ERRCF(); }
    } else if (ah == 0x3C || ah == 0x3D) {      /* create / open: DS:DX=ASCIIZ name */
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
        CHAR fn[300]; DWORD slot; HANDLE f;
        DWORD shr = FILE_SHARE_READ | FILE_SHARE_WRITE;
        DosGuestPath(m, R_DS, R_DX, fn, sizeof(fn));
        if (ah == 0x3C)
            f = CreateFileA(fn, GENERIC_READ | GENERIC_WRITE, shr,
                            NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        else {
            DWORD mode = R_AX & 7;
            DWORD acc = (mode == 1) ? GENERIC_WRITE
                      : (mode == 2) ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_READ;
            f = DosOpenStampable(fn, acc, shr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
        }
        if (f != INVALID_HANDLE_VALUE) {
            slot = DosHandleAllocate((PVOID const *)m->FileHandles, m->StdOpen);
            if (slot < DOS_MAX_FILES) { m->FileHandles[slot] = f; SETAX(slot); OKCF();
                                        if (ah == 0x3C) DosStampVdmNow(f); /* #263 */ }
            else { CloseHandle(f); SETAX(4); ERRCF(); }
        } else {
            /* ── ASK WHY IT FAILED. It used to answer 2 for every cause; see
                 DosErrFromWin32() for the two oracle rows that names wrong. */
            DWORD we = GetLastError(); WORD de;
            INT mapped = DosErrFromWin32((unsigned long)we, &de);
            SETAX(de); ERRCF();
            tp = zput(tp, "  INT21 AH=0x"); tp = zhex(tp, ah);
            tp = zput(tp, " ["); tp = zput(tp, fn); tp = zput(tp, "] FAILED win32=0x");
            tp = zhex(tp, we);
            tp = zput(tp, mapped ? " -> AX=0x" : " UNMAPPED, kept -> AX=0x");
            tp = zhex(tp, de); tp = zput(tp, "\r\n");
        }
        tp = zput(tp, "  INT21 AH=0x"); tp = zhex(tp, ah);
        tp = zput(tp, " ["); tp = zput(tp, fn); tp = zput(tp, "] -> AX=0x");
        tp = zhex(tp, R_AX & 0xFFFF); tp = zput(tp, (*pfl & 1) ? " (err)\r\n" : "\r\n");
    } else if (ah == 0x3E) {                    /* close: BX=handle */
        DWORD h = R_BX & 0xFFFF;
        /* Any BOUND handle closes, including a low one the shell redirected -- see
           the note at AH=40h. An unbound 0-4 is the console and closing it is a no-op. */
        if (DosHandleIsFile((PVOID const *)m->FileHandles, h)) DosHandleRelease(m, h);
        else DosHandleSetDevice(&m->StdOpen, h, 0);          /* free the device slot */
        OKCF();
    } else if (ah == 0x3F) {                    /* read: BX=handle CX=cnt -> DS:DX */
        DWORD h = R_BX & 0xFFFF, cnt = R_CX & 0xFFFF, rd = 0;
        PVOID b = (VOID *)((R_DS << 4) + (R_DX & 0xFFFF));
        if (DosHandleIsFile((PVOID const *)m->FileHandles, h)) {  /* bound -> a file, even if low */
            /* ► LOG THE FILE POSITION, THE COUNT AND THE FIRST BYTES. A DOS extender
                 loading an executable is doing nothing but seek+read, so if the image it
                 ends up with is wrong, the first question is whether WE handed it the
                 right bytes -- and that is answerable offline by comparing these lines
                 against the file. Without the position a short or misplaced read is
                 indistinguishable from a correct one. */
            DWORD pos = SetFilePointer(m->FileHandles[h], 0, NULL, FILE_CURRENT);
            /* #275: and a read that FAILS for a hardware reason is a critical error --
               see AH=40h; same rule, same reasons for leaving every other failure
               alone (it used to answer them all as CF=0 with what Win32 read). */
            DWORD we = 0; WORD de = 0;
            if (!ReadFile(m->FileHandles[h], b, cnt, &rd, NULL)) we = GetLastError();
            if (we && DosErrFromWin32((unsigned long)we, &de) && DosCritIsHardwareError(de)) {
                g_DosReadWriteDrive = DosHandleDrive(m->FileHandles[h]);
                SETAX(de); ERRCF();
                tp = zput(tp, "  INT21 AH=3F FAILED win32=0x"); tp = zhex(tp, we);
                tp = zput(tp, " (hardware) drive=");
                if (g_DosReadWriteDrive >= 0) { CHAR dl[3] = { (CHAR)('A' + g_DosReadWriteDrive), ':', 0 }; tp = zput(tp, dl); }
                else tp = zput(tp, "?");
                tp = zput(tp, "\r\n");
            } else { SETAX(rd); OKCF(); }
            tp = zput(tp, "  INT21 AH=3F h="); tp = zhex(tp, h);
            tp = zput(tp, " pos=0x"); tp = zhex(tp, pos);
            tp = zput(tp, " cnt=0x"); tp = zhex(tp, cnt);
            tp = zput(tp, " got=0x"); tp = zhex(tp, rd);
            tp = zput(tp, " -> 0x"); tp = zhex(tp, (DWORD)(ULONG_PTR)b);
            tp = zput(tp, " first="); tp = zdump(tp, (PCBYTE)b, (rd >= 8) ? 8 : 0);
            tp = zput(tp, "\r\n");
        }
        else if (h == 0 && DosHandleIsDevice((PVOID const *)m->FileHandles, m->StdOpen, h)) {
            /* ── #251: STDIN IS THE KEYBOARD, AND DOS READS A LINE FROM IT. ──────────
                 This answered 0 bytes -- end of file -- so any program reading its
                 input through handle 0 (C's gets/scanf/fgets do) saw an empty stream
                 and gave up. DOS's console read is cooked: it echoes, honours
                 backspace, ends at Enter, and returns the line WITH CR LF; a line
                 longer than the caller asked for is handed out over later reads.
                 Collected across retries like AH=0Ah, so the guest keeps running
                 (and taking its interrupts) while it waits for keys. */
            volatile BYTE *bv = (volatile BYTE *)b;
            INT c;
            DWORD n = 0;
            if (m->ConsolePosition >= m->ConsoleLength) {  /* nothing pending: collect a line */
                if (!m->IsConsoleCollecting) { m->IsConsoleCollecting = 1; m->ConsoleTyped = 0; }
                for (;;) {
                    c = m->ConsoleInNoWait ? m->ConsoleInNoWait(m->ConsoleInContext) : 0x0D;
                    if (c < 0) { m->IsRetry = 1; break; }
                    if (c == 0x0D) break;
                    if (c == 0x08) {
                        if (m->ConsoleTyped > 0) { --m->ConsoleTyped; OUTC(0x08); OUTC(' '); OUTC(0x08); }
                        continue;
                    }
                    if (c == 0x00) continue;            /* extended key: no ASCII */
                    if (m->ConsoleTyped >= 127) continue;  /* full: only Enter ends it */
                    m->ConsoleLine[m->ConsoleTyped++] = (BYTE)c; OUTC(c);
                }
                if (m->IsRetry) goto read_done;
                m->ConsoleLine[m->ConsoleTyped] = 0x0D; m->ConsoleLine[m->ConsoleTyped + 1] = 0x0A;
                m->ConsoleLength = m->ConsoleTyped + 2; m->ConsolePosition = 0; m->IsConsoleCollecting = 0;
                OUTC(0x0D); OUTC(0x0A);
            }
            while (n < cnt && m->ConsolePosition < m->ConsoleLength) bv[n++] = m->ConsoleLine[m->ConsolePosition++];
            SETAX(n); OKCF();
        read_done: ;
        }
        else if (h == 3 && AUXPRN_V86 && DosHandleIsDevice((PVOID const *)m->FileHandles, m->StdOpen, h))
             AUXPRN_TRAMP(DOS_AUXPRN_AUX_READ);       /* #251: AUX, through INT 14h */
        else if (DosHandleIsDevice((PVOID const *)m->FileHandles, m->StdOpen, h))
             { SETAX(0); OKCF(); }              /* PRN, a dup, or AUX in PM: EOF */
        else { SETAX(6); ERRCF(); }
    } else if (ah == 0x42) {                    /* lseek: AL=org BX=h CX:DX=off */
        DWORD h = R_BX & 0xFFFF, meth = R_AX & 0xFF;
        LONG dist = (LONG)(((R_CX & 0xFFFF) << 16) | (R_DX & 0xFFFF));
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
        if (DosHandleIsFile((PVOID const *)m->FileHandles, h)) {
            DWORD np = SetFilePointer(m->FileHandles[h], dist, NULL, meth);
            SETAX(np & 0xFFFF);
            R_DX = (R_DX & 0xFFFF0000u) | ((np >> 16) & 0xFFFF); OKCF();
        } else { SETAX(6); ERRCF(); }
    } else if (ah == 0x30) {                    /* get DOS version */
        /* AL=major, AH=minor, BH=OEM, BL:CX=24-bit serial.  GH #28.
           BX and CX were never written before, so a caller saw whatever it had
           left in them and read that as our OEM number and serial.  Values
           confirmed against the 6.22 oracle: BH=0xFF (generic MS-DOS), serial 0.
           The version itself is configurable -- see DosInt21SetVersion(). */
        SETAX(DosVersionWord(m));               /* per process: see v5_psp (#208) */
        SET16(R_BX, 0xFF00);                    /* BH=OEM 0xFF, BL=serial high */
        SET16(R_CX, 0x0000);                    /* serial low                  */
        OKCF();
    } else if (ah == 0x4E || ah == 0x4F) {      /* find first / find next */
        volatile BYTE *d = (volatile BYTE *)((m->DtaSegment << 4) + m->DtaOffset);
        WIN32_FIND_DATAA fd;
        WORD mask;
        INT slot = -1, ok = 0;
        if (ah == 0x4E) {
            CHAR pat[300];
            DosGuestPath(m, R_DS, R_DX, pat, sizeof(pat));
            mask = (WORD)(R_CX & 0xFFFF);
            for (slot = 0; slot < 8 && m->FindHandles[slot]; ++slot) {}
            if (slot >= 8) { slot = 0;                       /* recycle the oldest */
                             FindClose(m->FindHandles[0]); m->FindHandles[0] = 0; }
            { CHAR all[300]; BYTE tm[11]; INT nodir;
              HANDLE hf;
              DosFindSplit(pat, all, sizeof all, tm);            /* DOS matching: see DosFindMatches */
              hf = DosFindFirst(all, tm, mask, &fd, &nodir);
              if (m->IsTraceAll) { tp = zput(tp, "  INT21 AH=4E ["); tp = zput(tp, pat);
                                  tp = zput(tp, "] attr=0x"); tp = zhex(tp, mask);
                                  tp = zput(tp, hf == INVALID_HANDLE_VALUE ? " -> none" : " -> found");
                                  tp = zput(tp, nodir ? " (no such directory)\r\n" : "\r\n"); }
              if (hf == INVALID_HANDLE_VALUE) {
                  /* ORACLE-CONFIRMED, and not what memory suggests: a pattern
                     that matches nothing inside an EXISTING directory is
                     AX=18 "no more files", not AX=2 "file not found". A missing
                     directory is AX=3. */
                  SETAX(nodir ? 3 : 18);
                  /* #34: a HARDWARE failure (not ready, write-protected, ...) is its
                     own DOS code -- Win32 kept DOS's numbers for 19-31 -- and the
                     dispatcher's tail turns it into an INT 24h. */
                  if (g_DosFindWin32Error >= 19 && g_DosFindWin32Error <= 31) SETAX(g_DosFindWin32Error);
                  ERRCF();
              } else {
                  m->FindHandles[slot] = hf;
                  ok = 1;
                  /* Fill DOS's private search area deterministically.  It is
                     DOS-private, but leaving the caller's bytes lying in it
                     means the DTA differs run to run for no reason; real 6.22
                     puts the EXPANDED 11-byte search template there (a "*.*"
                     search reads back as eleven '?'), so do the same. */
                  /* The template, exactly as matched -- 4Fh reads it back from here. */
                  { INT bi; for (bi = 0; bi < 11; ++bi) d[1 + bi] = tm[bi]; }
                  d[0] = 3;                                  /* drive C:        */
                  d[12] = (BYTE)(mask & 0xFF);
                  d[13] = 0; d[14] = 0; d[15] = 0; d[16] = 0;
                  d[17] = 0; d[18] = 0;
                  d[19] = DOS_FIND_MAGIC;
                  d[20] = (BYTE)slot;
              }
            }
        } else {                                             /* 4Fh: continue   */
            mask = (WORD)d[12];
            if (d[19] == DOS_FIND_MAGIC && d[20] < 8 && m->FindHandles[d[20]]) {
                BYTE tm[11]; INT k;
                slot = d[20];
                for (k = 0; k < 11; ++k) tm[k] = d[1 + k];   /* the template 4Eh stored */
                ok = DosFindNext(m->FindHandles[slot], tm, mask, &fd);
            } else {
                SETAX(18); ERRCF();                          /* no search live  */
            }
        }
        if (slot >= 0 && ok) {
            DosDtaFill(d, &fd);
            /* DIR renders blank names, one impossible size repeated, and a 1980-ish
               date -- i.e. it is reading fields we did not put where it looks. Print
               the DTA we hand back, whole, and let the bytes settle it. */
            if (m->IsTraceAll) { INT q;
              tp = zput(tp, "  INT21 AH=4E/4F dta="); tp = zhexb(tp, (UINT)((m->DtaSegment >> 8) & 0xFF));
              tp = zhexb(tp, (UINT)(m->DtaSegment & 0xFF)); tp = zput(tp, ":");
              tp = zhexb(tp, (UINT)((m->DtaOffset >> 8) & 0xFF));
              tp = zhexb(tp, (UINT)(m->DtaOffset & 0xFF));
              tp = zput(tp, " [");
              for (q = 0; q < 44; ++q) { tp = zhexb(tp, (UINT)d[q]); tp = zput(tp, " "); }
              tp = zput(tp, "]\r\n"); }
            SETAX(0); OKCF();                                /* oracle: AX=0000 */
        } else if (slot >= 0 && m->FindHandles[slot] && !ok) {
            FindClose(m->FindHandles[slot]); m->FindHandles[slot] = 0;
            d[19] = 0;
            SETAX(18); ERRCF();                              /* no more files   */
        }
    } else if ((ah >= 0x0F && ah <= 0x17) || (ah >= 0x21 && ah <= 0x24)
               || (ah >= 0x27 && ah <= 0x29)) {  /* ---- the FCB interface ---- */
        volatile BYTE *f = DosFcbAt(R_DS, R_DX);
        CHAR nm[300];
        #define FCB_OK()   SETAX((R_AX & 0xFF00) | 0x00)
        #define FCB_FAIL() SETAX((R_AX & 0xFF00) | 0xFF)
        /* CF is undefined for these on real DOS; leave it as the guest set it. */
        if (ah == 0x0F || ah == 0x16) {         /* open / create */
            HANDLE fh2; DWORD slot;
            DosFcbName(f, nm);
            fh2 = CreateFileA(nm, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL,
                              (ah == 0x16) ? CREATE_ALWAYS : OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, NULL);
            if (fh2 == INVALID_HANDLE_VALUE) FCB_FAIL();
            else {
                FILETIME ft, lf; WORD fdt = 0, ftm = 0;
                DWORD sz = GetFileSize(fh2, NULL);
                slot = DosHandleAllocate((PVOID const *)m->FileHandles, m->StdOpen);
                if (slot >= DOS_MAX_FILES) { CloseHandle(fh2); FCB_FAIL(); }
                else {
                    m->FileHandles[slot] = fh2;
                    if (ah == 0x16) DosStampVdmNow(fh2);      /* #263, before the FCB reads it */
                    if (GetFileTime(fh2, NULL, NULL, &ft)
                        && FileTimeToLocalFileTime(&ft, &lf))
                        FileTimeToDosDateTime(&lf, &fdt, &ftm);
                    f[12] = 0; f[13] = 0;
                    f[14] = 128; f[15] = 0;         /* oracle: record size 128 */
                    f[16] = (BYTE)(sz & 0xFF);        f[17] = (BYTE)((sz >> 8) & 0xFF);
                    f[18] = (BYTE)((sz >> 16) & 0xFF); f[19] = (BYTE)((sz >> 24) & 0xFF);
                    f[20] = (BYTE)(fdt & 0xFF); f[21] = (BYTE)(fdt >> 8);
                    f[22] = (BYTE)(ftm & 0xFF); f[23] = (BYTE)(ftm >> 8);
                    /* DOS replaces a "default drive" 0 with the drive it
                       actually resolved -- measured: the oracle returns 01 when
                       run from A:, DOSBox 03 from C:. We were leaving the
                       caller's 0 in place. */
                    if (!f[0]) f[0] = (BYTE)(DosCurrentDrive(m) + 1);
                    f[24] = DOS_FCB_MAGIC; f[25] = (BYTE)slot;
                    FCB_OK();
                }
            }
        } else if (ah == 0x10) {                /* close */
            if (f[24] == DOS_FCB_MAGIC && f[25] < DOS_MAX_FILES && m->FileHandles[f[25]]) {
                DosHandleRelease(m, f[25]); f[24] = 0; FCB_OK();
            } else FCB_FAIL();
        } else if (ah == 0x11 || ah == 0x12) {  /* find first / find next */
            volatile BYTE *d = (volatile BYTE *)((m->DtaSegment << 4) + m->DtaOffset);
            WIN32_FIND_DATAA fd;
            INT got = 0;
            /* An extended FCB carries its search attribute in the byte just
               before the part DosFcbAt() returns; a normal one asks for ordinary
               files only.  WITHOUT THIS FILTER the search returns "." first --
               measured: our DTA came back with a blank name where the oracle had
               COMMAND.COM, because "." has no 8.3 name to put in the field. */
            WORD fmask = (f != (volatile BYTE *)((R_DS << 4) + (R_DX & 0xFFFF)))
                           ? (WORD)f[-1] : 0;
            /* ── A VOLUME-LABEL SEARCH IS NOT A FILE SEARCH. ────────────────────
                 Attribute 08h means "return the volume label and nothing else", and
                 it is how DIR fills in its header line. There is no file on disk to
                 match, so FindFirstFile cannot answer it -- we used to run the
                 ordinary search and hand back whatever came first, which is why DIR
                 announced `Volume in drive C is COMMAND COM`, the first file in the
                 directory wearing the label's clothes.
                 The label is 11 bytes in the name+ext field, NOT an 8.3 name, so it
                 is padded raw rather than through DosFcbPutName. */
            if (fmask == 0x08) {
                if (ah == 0x11) {
                    CHAR vol[128]; INT vi;
                    volatile BYTE *e;
                    INT ext = (f != (volatile BYTE *)((R_DS << 4) + (R_DX & 0xFFFF))) ? 7 : 0;
                    vol[0] = 0;
                    if (!GetVolumeInformationA("C:\\", vol, sizeof vol,
                                               NULL, NULL, NULL, NULL, 0) || !vol[0]) {
                        if (m->FcbFind) { FindClose(m->FcbFind); m->FcbFind = 0; }
                        m->LastError = 18;
                        FCB_FAIL();                       /* no label: DIR says so   */
                        goto fcb_done;
                    }
                    e = d + ext;
                    if (ext) { INT q; d[0] = 0xFF; for (q = 1; q <= 5; ++q) d[q] = 0; d[6] = 0x08; }
                    e[0] = 3;                             /* drive C:                */
                    for (vi = 0; vi < 11; ++vi) {
                        CHAR ch = vol[vi] ? vol[vi] : ' ';
                        if (!vol[vi]) { e[1 + vi] = ' '; continue; }
                        e[1 + vi] = (BYTE)(ch >= 'a' && ch <= 'z' ? ch - 32 : ch);
                    }
                    e[12] = 0x08;                         /* attribute: volume label */
                    { INT q; for (q = 13; q <= 32; ++q) e[q] = 0; }
                    if (m->FcbFind) { FindClose(m->FcbFind); m->FcbFind = 0; }
                    FCB_OK();
                } else { m->LastError = 18; FCB_FAIL(); }  /* 12h: only ever one label */
                goto fcb_done;
            }
            if (ah == 0x11) {
                /* The FCB's own 11 bytes ARE the template (`????????.???` for DIR), matched
                   against each entry's 8.3 name -- see DosFindMatches. The drive byte
                   picks the directory: "X:*" is that drive's current directory. */
                HANDLE hf; CHAR all[8]; INT k, nodir, n = 0;
                if (m->FcbFind) { FindClose(m->FcbFind); m->FcbFind = 0; }
                for (k = 0; k < 11; ++k) m->FcbTemplate[k] = f[1 + k];
                if (f[0]) { all[n++] = (CHAR)('A' + f[0] - 1); all[n++] = ':'; }
                all[n++] = '*'; all[n] = 0;
                hf = DosFindFirst(all, m->FcbTemplate, fmask, &fd, &nodir);
                if (hf != INVALID_HANDLE_VALUE) { m->FcbFind = hf; got = 1; }
                if (m->IsTraceAll) { CHAR cwd[260]; INT q;
                    GetCurrentDirectoryA(sizeof cwd, cwd);
                    tp = zput(tp, "  INT21 AH=11 ["); tp = zput(tp, all);
                    tp = zput(tp, "] in ["); tp = zput(tp, cwd); tp = zput(tp, "] tmpl=[");
                    for (q = 0; q < 11; ++q) { CHAR c1[2]; c1[0] = (CHAR)m->FcbTemplate[q]; c1[1] = 0; tp = zput(tp, c1); }
                    tp = zput(tp, "] mask=0x"); tp = zhex(tp, fmask);
                    tp = zput(tp, got ? " -> found\r\n" : " -> none\r\n"); }
            } else if (m->FcbFind) {
                got = DosFindNext(m->FcbFind, m->FcbTemplate, fmask, &fd);
                if (!got) { FindClose(m->FcbFind); m->FcbFind = 0; }
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
            if (!got) { m->LastError = 18; FCB_FAIL(); }
            else {
                PCSTR bn = fd.cAlternateFileName[0] ? fd.cAlternateFileName
                                                          : fd.cFileName;
                FILETIME lf; WORD fdt = 0, ftm = 0;
                INT k;
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
                INT ext = (f != (volatile BYTE *)((R_DS << 4) + (R_DX & 0xFFFF))) ? 7 : 0;
                volatile BYTE *e = d + ext;
                if (FileTimeToLocalFileTime(&fd.ftLastWriteTime, &lf))
                    FileTimeToDosDateTime(&lf, &fdt, &ftm);
                if (ext) {
                    d[0] = 0xFF;
                    for (k = 1; k <= 5; ++k) d[k] = 0;
                    d[6] = (BYTE)(fd.dwFileAttributes & 0x3F);
                }
                e[0] = 3;                                 /* drive C:          */
                DosFcbPutName(e + 1, bn);
                e[12] = (BYTE)(fd.dwFileAttributes & 0x3F);
                for (k = 13; k <= 22; ++k) e[k] = 0;
                e[23] = (BYTE)(ftm & 0xFF); e[24] = (BYTE)(ftm >> 8);
                e[25] = (BYTE)(fdt & 0xFF); e[26] = (BYTE)(fdt >> 8);
                e[27] = 0; e[28] = 0;                     /* starting cluster  */
                e[29] = (BYTE)( fd.nFileSizeLow        & 0xFF);
                e[30] = (BYTE)((fd.nFileSizeLow >> 8)  & 0xFF);
                e[31] = (BYTE)((fd.nFileSizeLow >> 16) & 0xFF);
                e[32] = (BYTE)((fd.nFileSizeLow >> 24) & 0xFF);
                FCB_OK();
            }
            fcb_done: ;
        } else if (ah == 0x13) {                /* delete (wildcards allowed) */
            WIN32_FIND_DATAA fd; HANDLE hf; INT any = 0;
            DosFcbName(f, nm);
            hf = FindFirstFileA(nm, &fd);
            if (hf != INVALID_HANDLE_VALUE) {
                do {
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                    if (DeleteFileA(fd.cFileName)) any = 1;
                } while (FindNextFileA(hf, &fd));
                FindClose(hf);
            }
            if (any) FCB_OK(); else FCB_FAIL();
        } else if (ah == 0x17) {                /* rename: new name at f[17..27] */
            CHAR to[300];
            CHAR save[16]; INT k;
            DosFcbName(f, nm);
            for (k = 0; k < 12; ++k) save[k] = (CHAR)f[k];
            { volatile BYTE tmp[12]; tmp[0] = f[0];
              for (k = 1; k < 12; ++k) tmp[k] = f[16 + k];
              DosFcbName(tmp, to); }
            if (MoveFileA(nm, to)) FCB_OK(); else FCB_FAIL();
            (VOID)save;
        } else if (ah == 0x14 || ah == 0x15 || ah == 0x21 || ah == 0x22
                   || ah == 0x27 || ah == 0x28) {         /* record I/O */
            volatile BYTE *d = (volatile BYTE *)((m->DtaSegment << 4) + m->DtaOffset);
            DWORD recsz = (DWORD)f[14] | ((DWORD)f[15] << 8);
            DWORD blk   = (DWORD)f[12] | ((DWORD)f[13] << 8);
            DWORD rec, count = 1, done = 0, k;
            BYTE buf[512];
            if (!recsz) recsz = 128;
            if (recsz > sizeof(buf)) recsz = sizeof(buf);
            if (ah == 0x14 || ah == 0x15) rec = blk * 128 + f[32];
            else rec = (DWORD)f[33] | ((DWORD)f[34] << 8)
                     | ((DWORD)f[35] << 16) | ((DWORD)f[36] << 24);
            if (ah == 0x27 || ah == 0x28) count = R_CX & 0xFFFF;
            if (f[24] != DOS_FCB_MAGIC || f[25] >= DOS_MAX_FILES || !m->FileHandles[f[25]]) SETAX((R_AX & 0xFF00) | 1);
            else {
                HANDLE hh = m->FileHandles[f[25]];
                DWORD n = 0;
                SetFilePointer(hh, (LONG)(rec * recsz), NULL, FILE_BEGIN);
                for (k = 0; k < count; ++k) {
                    if (ah == 0x14 || ah == 0x21 || ah == 0x27) {
                        DWORD j;
                        if (!ReadFile(hh, buf, recsz, &n, NULL) || n == 0) break;
                        for (j = 0; j < recsz; ++j)
                            d[done * recsz + j] = (j < n) ? buf[j] : 0;
                        ++done;
                        if (n < recsz) break;
                    } else {
                        DWORD j;
                        for (j = 0; j < recsz; ++j) buf[j] = d[done * recsz + j];
                        if (!WriteFile(hh, buf, recsz, &n, NULL)) break;
                        ++done;
                    }
                }
                if (ah == 0x27 || ah == 0x28) SET16(R_CX, (WORD)done);
                if (done && !(ah == 0x14 || ah == 0x21 || ah == 0x27))
                    DosStampVdmNow(hh);                 /* #263: an FCB write */
                /* AL: 0 = all done, 1 = end of file / nothing transferred,
                   3 = a partial final record. */
                if (done == count) SETAX((R_AX & 0xFF00) | 0);
                else if (!done)    SETAX((R_AX & 0xFF00) | 1);
                else               SETAX((R_AX & 0xFF00) | 3);
                if (ah == 0x14 || ah == 0x15) {           /* advance sequentially */
                    DWORD nr = rec + done;
                    f[12] = (BYTE)((nr / 128) & 0xFF); f[13] = (BYTE)((nr / 128) >> 8);
                    f[32] = (BYTE)(nr % 128);
                } else {
                    DWORD nr = rec + done;
                    f[33] = (BYTE)(nr & 0xFF);         f[34] = (BYTE)((nr >> 8) & 0xFF);
                    f[35] = (BYTE)((nr >> 16) & 0xFF); f[36] = (BYTE)((nr >> 24) & 0xFF);
                }
            }
        } else if (ah == 0x23) {                /* get file size, in records */
            HANDLE fh3;
            DosFcbName(f, nm);
            fh3 = CreateFileA(nm, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (fh3 == INVALID_HANDLE_VALUE) FCB_FAIL();
            else {
                DWORD sz = GetFileSize(fh3, NULL);
                DWORD recsz = (DWORD)f[14] | ((DWORD)f[15] << 8);
                DWORD recs;
                CloseHandle(fh3);
                if (!recsz) recsz = 128;
                recs = (sz + recsz - 1) / recsz;
                f[33] = (BYTE)(recs & 0xFF);         f[34] = (BYTE)((recs >> 8) & 0xFF);
                f[35] = (BYTE)((recs >> 16) & 0xFF); f[36] = (BYTE)((recs >> 24) & 0xFF);
                FCB_OK();
            }
        } else if (ah == 0x24) {                /* set random record from current */
            DWORD nr = ((DWORD)f[12] | ((DWORD)f[13] << 8)) * 128 + f[32];
            f[33] = (BYTE)(nr & 0xFF);         f[34] = (BYTE)((nr >> 8) & 0xFF);
            f[35] = (BYTE)((nr >> 16) & 0xFF); f[36] = (BYTE)((nr >> 24) & 0xFF);
            OKCF();
        } else if (ah == 0x29) {                /* parse a filename into an FCB */
            CHAR in[300];
            volatile BYTE *dst = (volatile BYTE *)((R_ES << 4) + (R_DI & 0xFFFF));
            INT i2 = 0, wild = 0, k;
            /* ── ★ AL's CONTROL BITS SAY WHAT A MISSING PART LEAVES ALONE. (s81 sweep) ──
                 bit 1: no drive given -> keep the FCB's drive (else 0 = default)
                 bit 2: no name given  -> keep the FCB's name
                 bit 3: no extension   -> keep the FCB's extension
                 Unanimous on msdos622 / dosbox-x / pcem / pcem-vesa (p_fcb.asm
                 int21.29.keepext/keepall/blank). XP's COMMAND.COM builds DIR's search
                 FCB as ??????????? and parses "*" with AL=0Eh; blanking the extension
                 regardless made the template ????????+3 spaces and DIR listed only
                 `.` and `..` -- the user's sweep finding. */
            BYTE al29 = (BYTE)(R_AX & 0xFF);
            BYTE keep[12]; INT j, has_name, has_ext;
            for (k = 0; k < 12; ++k) keep[k] = dst[k];
            DosGuestString(R_DS, R_SI, in, sizeof(in));
            while (in[i2] == ' ' || in[i2] == 9) ++i2;
            dst[0] = 0;
            if (in[i2] && in[i2 + 1] == ':') {
                CHAR dch = in[i2];
                dst[0] = (BYTE)((dch >= 'a' ? dch - 32 : dch) - 'A' + 1);
                i2 += 2;
            } else if (al29 & 0x02) dst[0] = keep[0];
            for (j = i2; !DosFcbIsNameEnd((BYTE)in[j]) && in[j] != '.'; ++j) {}
            has_name = (j > i2);
            has_ext  = (in[j] == '.');
            DosFcbPutName(dst + 1, in + i2);
            if (!has_name && (al29 & 0x04)) for (k = 1; k <= 8;  ++k) dst[k] = keep[k];
            if (!has_ext  && (al29 & 0x08)) for (k = 9; k <= 11; ++k) dst[k] = keep[k];
            for (k = 1; k <= 11; ++k) if (dst[k] == '?' || dst[k] == '*') wild = 1;
            for (k = 12; k <= 15; ++k) dst[k] = 0;
            SETAX((R_AX & 0xFF00) | (wild ? 1 : 0));
            SET16(R_SI, (WORD)((R_SI & 0xFFFF) + i2));
            OKCF();
            /* THE CALL COMMAND.COM'S DISPATCH TURNS ON. `ver ` runs and `ver` does
               not, and the traces diverge on the instruction after the third of
               these -- so print what went in and what came out, both. Reasoning
               about it from the handler's source has already produced two wrong
               models this session. */
            if (m->IsTraceAll) { INT q;
              tp = zput(tp, "  INT21 AH=29 al="); tp = zhexb(tp, (UINT)(R_AX & 0xFF));
              tp = zput(tp, " ds:si="); tp = zhexb(tp, (UINT)((R_DS >> 8) & 0xFF));
              tp = zhexb(tp, (UINT)(R_DS & 0xFF)); tp = zput(tp, ":");
              tp = zhexb(tp, (UINT)(((R_SI & 0xFFFF) >> 8) & 0xFF));
              tp = zhexb(tp, (UINT)(R_SI & 0xFF));
              tp = zput(tp, " in=[");
              for (q = 0; q < 12 && in[q]; ++q) tp = zhexb(tp, (UINT)(BYTE)in[q]), tp = zput(tp, " ");
              tp = zput(tp, "] fcb=[");
              for (q = 0; q < 12; ++q) tp = zhexb(tp, (UINT)dst[q]), tp = zput(tp, " ");
              tp = zput(tp, "]\r\n"); }
        } else FCB_FAIL();
        #undef FCB_OK
        #undef FCB_FAIL
    } else if (ah == 0x4B) {                    /* EXEC: load and run a program */
        BYTE al4b = (BYTE)(R_AX & 0xFF);
        if (al4b == 0x00 || al4b == 0x01) {
            const volatile BYTE *pb =
                (const volatile BYTE *)((R_ES << 4) + (R_BX & 0xFFFF));
            /* AL=01 answers THROUGH this block, so remember where it is. */
            m->ExecBlockSegment = (WORD)(R_ES & 0xFFFF);
            m->ExecBlockOffset = (WORD)(R_BX & 0xFFFF);
            DosGuestPath(m, R_DS, R_DX, m->ExecPath, sizeof(m->ExecPath));
            DosGuestString(R_DS, R_DX, m->ExecName, sizeof(m->ExecName));
            m->ExecEnvironment      = (WORD)(pb[0] | (pb[1] << 8));
            m->ExecTailOffset = (WORD)(pb[2] | (pb[3] << 8));
            m->ExecTailSegment = (WORD)(pb[4] | (pb[5] << 8));
            m->ExecFcb1Offset = (WORD)(pb[6] | (pb[7] << 8));
            m->ExecFcb1Segment = (WORD)(pb[8] | (pb[9] << 8));
            m->ExecFcb2Offset = (WORD)(pb[10] | (pb[11] << 8));
            m->ExecFcb2Segment = (WORD)(pb[12] | (pb[13] << 8));
            m->ExecMode = al4b;
            m->IsExecPending = 1;               /* the host does the rest */
            OKCF();
        } else if (al4b == 0x03) {
            /* ── THE OVERLAY. Two words of parameter block and nothing else:
                 where to put it, and what to relocate by -- and those are NOT
                 the same number (oracle: relocation uses the FACTOR, measured
                 with a factor deliberately unequal to the load segment). No PSP,
                 no allocation, no transfer of control, so the host's normal EXEC
                 path is wrong for it and it branches early. (GH #50) */
            const volatile BYTE *pb =
                (const volatile BYTE *)((R_ES << 4) + (R_BX & 0xFFFF));
            DosGuestPath(m, R_DS, R_DX, m->ExecPath, sizeof(m->ExecPath));
            m->ExecOverlaySegment   = (WORD)(pb[0] | (pb[1] << 8));
            m->ExecOverlayRelocation = (WORD)(pb[2] | (pb[3] << 8));
            m->ExecMode = 0x03;
            m->IsExecPending = 1;
            OKCF();
        } else if (al4b == 0x05) {
            /* ── SET EXECUTION STATE (#165). The second half of a loader's own EXEC:
                 AX=4B01h loaded the program, the loader did its own work, and this
                 tells DOS control is about to go to it. Measured (p_4b05, 6.22 and
                 PCem agree): AX=0000 CF=0, and the CURRENT PSP IS NOT CHANGED -- 4B01h
                 already switched it. DOS uses the block for SETVER's per-program
                 version; we keep no SETVER table, so there is nothing else to do.
                 DOSBox-X refuses it (CF=1 AX=000B): an emulator without the call,
                 not a different DOS. */
            tp = zput(tp, "  INT21 AX=4B05 set execution state -- accepted\r\n");
            SETAX(0); OKCF();
        } else {
            /* AL=02/04 are not DOS 6.22 functions we have measured. */
            tp = zput(tp, "  INT21 AH=4B AL=0x"); tp = zhexb(tp, al4b);
            tp = zput(tp, " UNIMPLEMENTED (overlay load)\r\n");
            m->Unimplemented[0x4B >> 3] |= (BYTE)(1u << (0x4B & 7));
            SETAX(1); ERRCF();
        }
    } else if (ah == 0x1B || ah == 0x1C) {      /* allocation info for a drive */
        /* AL=sectors/cluster, DS:BX -> media descriptor byte, CX=bytes/sector,
           DX=total clusters.  All disk geometry, so the probe compares only CF.
           NOTE this call RETURNS A SEGMENT IN DS -- which is what broke the
           probe's own output until probe_capture learned to restore it. */
        DWORD spc = 0, bps = 0, freec = 0, totc = 0;
        CHAR root[4]; PSTR rp = 0;
        BYTE dl1b = (ah == 0x1C) ? (BYTE)(R_DX & 0xFF) : 0;         /* 1Bh: default drive */
        if (!dl1b && m->VirtualDrive >= 0) dl1b = (BYTE)(m->VirtualDrive + 1);
        if (dl1b) { root[0] = (CHAR)('A' + dl1b - 1); root[1] = ':';
                    root[2] = '\\'; root[3] = 0; rp = root; }
        if (GetDiskFreeSpaceA(rp, &spc, &bps, &freec, &totc)) {
            volatile BYTE *md = (volatile BYTE *)((DOS_CTAB_SEG << 4) + DOS_MEDIA_OFF);
            *md = 0xF8;                          /* fixed disk */
            SETAX((R_AX & 0xFF00) | (spc & 0xFF));
            SET16(R_DS, DOS_CTAB_SEG); SET16(R_BX, DOS_MEDIA_OFF);
            SET16(R_CX, (WORD)bps);
            SET16(R_DX, totc > 0xFFFF ? 0xFFFF : totc);
            OKCF();
        } else { SETAX((R_AX & 0xFF00) | 0xFF); ERRCF(); }
    } else if (ah == 0x1F || ah == 0x32) {      /* get drive parameter block */
        /* DPB contents are disk geometry and its address is host-specific; AL is
           the comparable part -- 00 for a valid drive, FF otherwise (measured). */
        BYTE dl32 = (ah == 0x1F) ? 0 : (BYTE)(R_DX & 0xFF);
        DWORD spc = 0, bps = 0, freec = 0, totc = 0;
        CHAR root[4]; PSTR rp = 0;
        if (dl32) { root[0] = (CHAR)('A' + dl32 - 1); root[1] = ':';
                    root[2] = '\\'; root[3] = 0; rp = root; }
        if (dl32 > 26 || !GetDiskFreeSpaceA(rp, &spc, &bps, &freec, &totc)) {
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
            volatile BYTE *d = (volatile BYTE *)((DOS_CTAB_SEG << 4) + DOS_DPB_OFF);
            BYTE dp[DOS_DPB_LEN];
            UINT drv = dl32 ? (UINT)(dl32 - 1) : 2u;             /* 0-based drive */
            CHAR rt[4]; INT k, rem;
            rt[0] = (CHAR)('A' + drv); rt[1] = ':'; rt[2] = '\\'; rt[3] = 0;
            rem = (GetDriveTypeA(rt) == DRIVE_REMOVABLE);      /* 6.22's floppy: 224, F0h */
            DosDpbBuild(dp, drv, bps ? bps : 512, spc ? spc : 1, rem ? 224 : 512,
                          (totc > 0xFFFE) ? 0xFFFE : totc + 1, rem ? 0xF0 : 0xF8,
                          DOS_DEV_SEG, DOS_DEVICE_OFFSET(DOS_DEVICE_BLOCK), 0xFFFF, 0xFFFF);
            for (k = 0; k < DOS_DPB_LEN; ++k) d[k] = dp[k];
            SET16(R_DS, DOS_CTAB_SEG); SET16(R_BX, DOS_DPB_OFF);
            SETAX(R_AX & 0xFF00);
        }
        OKCF();
    } else if (ah == 0x37) {                    /* get/set the SWITCH character */
        BYTE al37 = (BYTE)(R_AX & 0xFF);
        if (al37 == 0x00) {                     /* oracle: DL = '/' */
            SET16(R_DX, (WORD)((R_DX & 0xFF00) | m->SwitchChar));
            SETAX(R_AX & 0xFF00); OKCF();
        } else if (al37 == 0x01) {
            m->SwitchChar = (BYTE)(R_DX & 0xFF);
            SETAX(R_AX & 0xFF00); OKCF();
        } else { SETAX((R_AX & 0xFF00) | 0xFF); OKCF(); }
    } else if (ah == 0x66) {                    /* get/set global code page */
        BYTE al66 = (BYTE)(R_AX & 0xFF);
        if (al66 == 0x01) {                     /* oracle: BX=DX=437 */
            SET16(R_BX, 437); SET16(R_DX, 437); OKCF();
        } else if (al66 == 0x02) {
            OKCF();                             /* accept; we have only 437 */
        } else { SETAX(1); ERRCF(); }
    } else if (ah == 0x26 || ah == 0x55) {      /* create a PSP / child PSP */
        /* Copy our PSP to the segment in DX and fix up the fields that must
           differ. 55h additionally takes the child's memory top in SI. */
        volatile BYTE *src = (volatile BYTE *)(DOS_PSP_SEG << 4);
        volatile BYTE *dst = (volatile BYTE *)((R_DX & 0xFFFF) << 4);
        INT k;
        for (k = 0; k < 256; ++k) dst[k] = src[k];
        dst[0x16] = (BYTE)(DOS_PSP_SEG & 0xFF);       /* parent PSP segment */
        dst[0x17] = (BYTE)(DOS_PSP_SEG >> 8);
        if (ah == 0x55) {
            dst[0x02] = (BYTE)(R_SI & 0xFF);          /* memory top          */
            dst[0x03] = (BYTE)((R_SI >> 8) & 0xFF);
        }
        OKCF();
    } else if (ah == 0x31) {                    /* terminate and stay resident */
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
        m->TsrKeep = (WORD)(R_DX & 0xFFFF);
        m->IsTsrPending = 1;
        tp = zput(tp, "  INT21 AH=31 TSR: keep 0x"); tp = zhex(tp, m->TsrKeep);
        tp = zput(tp, " paragraphs, vectors LEFT INSTALLED\r\n");
        m->ExitCode = (INT)(R_AX & 0xFF);
        cont = 0;
    } else if (ah == 0x53) {                    /* translate a BPB into a DPB */
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
        { static INT said53 = 0;
          if (!said53) { said53 = 1;
              tp = zput(tp, "  INT21 AH=53 BPB->DPB UNIMPLEMENTED (no installable "
                            "block drivers) -- said once per run\r\n"); } }
        m->Unimplemented[0x53 >> 3] |= (BYTE)(1u << (0x53 & 7));
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
        { BYTE al53 = (BYTE)(R_AX & 0xFF);
          if (al53 < DOS_INT53_COUNT) {
              SETAX(g_DosInt53Answers[al53].Ax);
              if (g_DosInt53Answers[al53].IsCarry) ERRCF(); else OKCF();
          } else { SETAX(0x0001); ERRCF(); }
        }
    } else if (ah == 0x5E) {                    /* network machine name / printer */
        BYTE al5e = (BYTE)(R_AX & 0xFF);
        if (al5e == 0x00) {                     /* oracle: AX=0, CF=0 */
            volatile BYTE *d = (volatile BYTE *)((R_DS << 4) + (R_DX & 0xFFFF));
            INT k; for (k = 0; k < 16; ++k) d[k] = 0;
            SETAX(0); SET16(R_CX, 0); OKCF();
        } else { SETAX(1); ERRCF(); }
    } else if (ah == 0x5F) {                    /* network redirection list */
        tp = zput(tp, "  INT21 AH=5F network redirection: no redirector present\r\n");
        SETAX(1); ERRCF();                      /* invalid function */
    } else if (ah == 0x64) {                    /* set device driver lookahead */
        OKCF();                                 /* internal; accepted, no effect */
    } else if (ah == 0x03 || ah == 0x04 || ah == 0x05) {   /* AUX in / AUX out / PRN out */
        /* ── #251: THESE WENT NOWHERE -- "accepted and discarded", and AUX input
             answered ^Z -- while COM1 and an LPT1 spool both exist. On DOS they are
             the AUX and PRN drivers, which call INT 14h / INT 17h through the IVT
             (p_auxprn logs the exact sequence 6.22 makes), so in V86 the guest is
             resumed in that driver code. In PM the bytes go to the same devices
             directly, and AUX input stays ^Z (no wait loop to run it in). */
        if (AUXPRN_V86)
            AUXPRN_TRAMP(ah == 0x05 ? DOS_AUXPRN_PRN_OUTPUT : ah == 0x04 ? DOS_AUXPRN_AUX_OUTPUT : DOS_AUXPRN_AUX_INPUT);
        else if (ah == 0x03) {
            SETAX((R_AX & 0xFF00) | 0x1A);
            OKCF();
        } else {
            BYTE c = (BYTE)(R_DX & 0xFF);
            if (ah == 0x05) { if (m->PrinterOut) (VOID)m->PrinterOut(m->DeviceContext, c); }
            else if (m->AuxOut) m->AuxOut(m->DeviceContext, c);
            SETAX((R_AX & 0xFF00) | c);         /* oracle: AL = the byte sent */
            OKCF();
        }
    } else if (ah == 0x0C) {                    /* flush input, then run AL     */
        /* AL names the input function to perform after flushing. Anything else
           is just a flush. Re-dispatching is the whole point of the call. */
        BYTE fn = (BYTE)(R_AX & 0xFF);
        while (m->ConsolePeek && m->ConsolePeek(m->ConsoleInContext) && m->ConsoleInNoWait)
            (VOID)m->ConsoleInNoWait(m->ConsoleInContext);
        if (fn == 0x01 || fn == 0x06 || fn == 0x07 || fn == 0x08 || fn == 0x0A) {
            SETAX((WORD)(fn << 8));
            m->IsRetry = 1;                     /* re-enter with AH = that fn  */
        } else OKCF();
    } else if (ah == 0x2E) {                    /* set verify flag */
        m->IsVerifyOn = (BYTE)(R_AX & 0xFF); OKCF();
    } else if (ah == 0x54) {                    /* get verify flag */
        SETAX((R_AX & 0xFF00) | m->IsVerifyOn); OKCF();
    } else if (ah == 0x34) {                    /* get InDOS flag -> ES:BX */
        SET16(R_ES, DOS_SDA_SEG); SET16(R_BX, DOS_INDOS_OFF); OKCF();
    } else if (ah == 0x5D && ((R_AX & 0xFF) == 0x08 || (R_AX & 0xFF) == 0x09)) {
        /* 5D08h/5D09h set and flush the network redirector's sharing retry
           counts. COMMAND.COM calls both at startup. There is no redirector
           here, so accept and ignore -- that is what DOS does on a machine with
           no network, and refusing would make the shell think something failed. */
        OKCF();
    } else if (ah == 0x5D && (R_AX & 0xFF) == 0x06) {   /* get swappable data area */
        SET16(R_DS, DOS_SDA_SEG); SET16(R_SI, DOS_SDA_OFF);
        SET16(R_CX, DOS_SDA_LEN); SET16(R_DX, DOS_SDA_LEN);
        tp = zput(tp, "  INT21 AH=5D06 SDA (minimal: crit-err + InDOS only)\r\n");
        OKCF();
    } else if (ah == 0x39 || ah == 0x3A) {      /* mkdir / rmdir */
        CHAR fn[300];
        INT ok2;
        DosGuestPath(m, R_DS, R_DX, fn, sizeof(fn));
        ok2 = (ah == 0x39) ? (INT)CreateDirectoryA(fn, NULL)
                           : (INT)RemoveDirectoryA(fn);
        /* s91: A SEARCH THE GUEST NEVER FINISHED KEEPS THE DIRECTORY OPEN. DOS has no
             FindClose, so an AH=4Eh/11h search that stopped before "no more files"
             leaves our FindFirstFile handle alive -- and Windows will not remove a
             directory with a search open in it. 6.22's COMMAND.COM searches inside
             a directory on `cd`, and `rmdir` of that (empty) directory then failed
             with "Invalid path, not directory, or directory not empty" (runs/s91,
             chain11b). Close the guest's unfinished searches and try once more. */
        if (!ok2 && ah == 0x3A) {
            INT fk;
            for (fk = 0; fk < 8; ++fk)
                if (m->FindHandles[fk]) { FindClose(m->FindHandles[fk]); m->FindHandles[fk] = 0; }
            if (m->FcbFind) { FindClose(m->FcbFind); m->FcbFind = 0; }
            ok2 = (INT)RemoveDirectoryA(fn);
        }
        if (ok2) OKCF();
        else {
            /* Oracle: mkdir over an existing name is 5 (access denied); rmdir of
               something absent is 3 (path not found). */
            DWORD e = GetLastError();
            SETAX((WORD)(e == ERROR_ALREADY_EXISTS ? 5
                           : e == ERROR_PATH_NOT_FOUND ? 3
                           : e == ERROR_FILE_NOT_FOUND ? 3 : 5));
            ERRCF();
        }
    } else if (ah == 0x41) {                    /* delete file */
        CHAR fn[300];
        DosGuestPath(m, R_DS, R_DX, fn, sizeof(fn));
        if (DeleteFileA(fn)) OKCF();
        else { SETAX(2); ERRCF(); }             /* oracle: absent -> AX=2 */
    } else if (ah == 0x43) {                    /* get/set file attributes */
        CHAR fn[300];
        BYTE al43 = (BYTE)(R_AX & 0xFF);
        DosGuestPath(m, R_DS, R_DX, fn, sizeof(fn));
        if (al43 == 0x00) {
            DWORD a = GetFileAttributesA(fn);
            if (a == 0xFFFFFFFFu) { SETAX(2); ERRCF(); }
            else { SET16(R_CX, (WORD)(a & 0x3F)); SETAX((WORD)(a & 0x3F)); OKCF(); }
        } else if (al43 == 0x01) {
            DWORD a = (DWORD)(R_CX & 0x3F);
            if (!a) a = FILE_ATTRIBUTE_NORMAL;
            if (SetFileAttributesA(fn, a)) OKCF();
            else { SETAX(2); ERRCF(); }
        } else {
            tp = zput(tp, "  INT21 AH=43 AL=0x"); tp = zhexb(tp, al43);
            /* Not a gap: 6.22 and PCem answer AX=1 CF=1 too (p_subfn int21.4302). */
            tp = zput(tp, " not a 6.22 subfunction -> AX=1 CF=1 (matches DOS)\r\n");
            SETAX(1); ERRCF();
        }
    } else if (ah == 0x45 || ah == 0x46) {      /* dup / dup2 */
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
        DWORD src = R_BX & 0xFFFF, dst;
        INT src_dev = DosHandleIsDevice((PVOID const *)m->FileHandles, m->StdOpen, src);
        if (!src_dev && !DosHandleIsFile((PVOID const *)m->FileHandles, src)) { SETAX(6); ERRCF(); }
        else {
            HANDLE nh = 0;
            if (!src_dev
                && !DuplicateHandle(GetCurrentProcess(), m->FileHandles[src],
                                    GetCurrentProcess(), &nh, 0, FALSE,
                                    DUPLICATE_SAME_ACCESS)) { SETAX(6); ERRCF(); }
            else if (ah == 0x45) {
                dst = DosHandleAllocate((PVOID const *)m->FileHandles, m->StdOpen);
                if (dst >= DOS_MAX_FILES) {
                    if (nh) CloseHandle(nh); SETAX(4); ERRCF();
                } else if (src_dev && !DosHandleSetDevice(&m->StdOpen, dst, 1)) {
                    /* Past the device mask. Refuse LOUDLY rather than hand back a
                       slot that would read as a file -- see DOS_DEV_SLOTS. */
                    tp = zput(tp, "  INT21 AH=45 device dup past slot 0x");
                    tp = zhex(tp, DOS_DEV_SLOTS); tp = zput(tp, " -- refused\r\n");
                    SETAX(4); ERRCF();
                } else { m->FileHandles[dst] = nh; SETAX(dst); OKCF(); }
            } else {
                dst = R_CX & 0xFFFF;
                if (dst >= DOS_MAX_FILES) { if (nh) CloseHandle(nh); SETAX(6); ERRCF(); }
                else if (src_dev && !DosHandleSetDevice(&m->StdOpen, dst, 1)) {
                    tp = zput(tp, "  INT21 AH=46 device dup2 past slot 0x");
                    tp = zhex(tp, DOS_DEV_SLOTS); tp = zput(tp, " -- refused\r\n");
                    SETAX(4); ERRCF();
                }
                else { DosHandleRelease(m, dst);
                       m->FileHandles[dst] = nh;        /* 0 when src is a device */
                       if (!src_dev) DosHandleSetDevice(&m->StdOpen, dst, 0);
                       OKCF(); }
            }
        }
    } else if (ah == 0x4D) {                    /* get child return code */
        SETAX(m->ChildReturnCode); m->ChildReturnCode = 0;  /* DOS clears it after reading */
        OKCF();
    } else if (ah == 0x56) {                    /* rename: DS:DX -> ES:DI */
        CHAR from[300], to[300];
        DosGuestPath(m, R_DS, R_DX, from, sizeof(from));
        DosGuestPath(m, R_ES, R_DI, to,   sizeof(to));
        if (MoveFileA(from, to)) OKCF();
        else { DWORD e = GetLastError();
               SETAX((WORD)(e == ERROR_ALREADY_EXISTS ? 5 : 2)); ERRCF(); }
    } else if (ah == 0x57) {                    /* get/set file date and time */
        DWORD h57 = R_BX & 0xFFFF;
        BYTE al57 = (BYTE)(R_AX & 0xFF);
        if (!DosHandleIsFile((PVOID const *)m->FileHandles, h57)) { SETAX(6); ERRCF(); }
        else if (al57 == 0x00) {
            FILETIME ft, lf; WORD fdate = 0, ftime = 0;
            if (GetFileTime(m->FileHandles[h57], NULL, NULL, &ft)
                && FileTimeToLocalFileTime(&ft, &lf)
                && FileTimeToDosDateTime(&lf, &fdate, &ftime)) {
                SET16(R_CX, ftime); SET16(R_DX, fdate); OKCF();
            } else { SETAX(6); ERRCF(); }
        } else if (al57 == 0x01) {
            FILETIME ft, lf;
            if (DosDateTimeToFileTime((WORD)(R_DX & 0xFFFF), (WORD)(R_CX & 0xFFFF), &lf)
                && LocalFileTimeToFileTime(&lf, &ft)
                && SetFileTime(m->FileHandles[h57], NULL, NULL, &ft)) OKCF();
            else { SETAX(6); ERRCF(); }
        } else { SETAX(1); ERRCF(); }
    } else if (ah == 0x5A || ah == 0x5B) {      /* create temp / create new */
        CHAR fn[300]; HANDLE f; DWORD slot;
        DosGuestPath(m, R_DS, R_DX, fn, sizeof(fn));
        if (ah == 0x5A) {                       /* DS:DX is a DIRECTORY path;
                                                   DOS appends a generated name
                                                   and hands it back in place. */
            INT k = 0; static UINT seq = 0;
            PCSTR hexd = "0123456789ABCDEF";
            while (fn[k] && k < 280) ++k;
            if (k && fn[k-1] != '\\' && fn[k-1] != '/') fn[k++] = '\\';
            { UINT v = (UINT)(GetTickCount() + (seq++ * 0x1234u));
              INT j; for (j = 0; j < 8; ++j) fn[k + j] = hexd[(v >> (28 - j*4)) & 0xF]; }
            fn[k + 8] = 0;
            { volatile BYTE *d = (volatile BYTE *)((R_DS << 4) + (R_DX & 0xFFFF));
              INT j = 0; while (fn[j]) { d[j] = (BYTE)fn[j]; ++j; } d[j] = 0; }
        }
        f = CreateFileA(fn, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL,
                        CREATE_NEW, (DWORD)(R_CX & 0x3F) ? (DWORD)(R_CX & 0x3F)
                                                         : FILE_ATTRIBUTE_NORMAL, NULL);
        if (f == INVALID_HANDLE_VALUE) {
            /* Oracle: create-new over an existing file is error 80 (file exists),
               not 5 -- measured, and not the obvious guess. */
            DWORD e = GetLastError();
            SETAX((WORD)(e == ERROR_FILE_EXISTS || e == ERROR_ALREADY_EXISTS ? 80 : 3));
            ERRCF();
        } else {
            slot = DosHandleAllocate((PVOID const *)m->FileHandles, m->StdOpen);
            if (slot < DOS_MAX_FILES) { m->FileHandles[slot] = f; SETAX(slot); OKCF();
                                        DosStampVdmNow(f); /* #263 */ }
            else { CloseHandle(f); SETAX(4); ERRCF(); }
        }
    } else if (ah == 0x5C) {                    /* lock / unlock a byte range */
        DWORD h5c = R_BX & 0xFFFF;
        DWORD off = ((DWORD)(R_CX & 0xFFFF) << 16) | (DWORD)(R_DX & 0xFFFF);
        DWORD len = ((DWORD)(R_SI & 0xFFFF) << 16) | (DWORD)(R_DI & 0xFFFF);
        if (!DosHandleIsFile((PVOID const *)m->FileHandles, h5c)) { SETAX(6); ERRCF(); }
        else {
            BOOL ok5 = ((R_AX & 0xFF) == 0)
                     ? LockFile(m->FileHandles[h5c], off, 0, len, 0)
                     : UnlockFile(m->FileHandles[h5c], off, 0, len, 0);
            if (ok5) OKCF(); else { SETAX(0x21); ERRCF(); }   /* 33 = lock violation */
        }
    } else if (ah == 0x67) {                    /* set maximum handle count */
        /* We keep a fixed DOS_MAX_FILES-entry table, so anything up to that succeeds.
           NOTE the oracle FAILED this with AX=8 (insufficient memory) when asked
           for 30 -- that is a property of ITS memory state at that moment, not a
           rule about DOS, which is why the probe treats the result as
           informational rather than comparable. */
        if ((R_BX & 0xFFFF) <= DOS_MAX_FILES) OKCF();
        else { SETAX(8); ERRCF(); }
    } else if (ah == 0x68 || ah == 0x6A) {      /* commit file (flush) */
        DWORD h68 = R_BX & 0xFFFF;
        if (!DosHandleIsFile((PVOID const *)m->FileHandles, h68)) { SETAX(6); ERRCF(); }
        else { FlushFileBuffers(m->FileHandles[h68]); OKCF(); }
    } else if (ah == 0x6C) {                    /* extended open/create */
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
        CHAR fn[300]; HANDLE f; DWORD slot, disp;
        DWORD acc = (DWORD)DosExtOpenAccess((UINT)(R_BX & 0xFFFF));
        disp = DosExtOpenDisposition((UINT)(R_DX & 0xFFFF));       /* Win32's own numbers */
        DosGuestPath(m, R_DS, R_SI, fn, sizeof(fn));
        /* FILE_SHARE_WRITE too: we do not emulate SHARE.EXE, so a second open of a
           file this VDM holds must not fail -- the rule AH=3Dh learned in session 37
           and this twin had not (#168). */
        SetLastError(0);
        f = DosOpenStampable(fn, acc, FILE_SHARE_READ | FILE_SHARE_WRITE, disp,
                               (DWORD)(R_CX & 0x3F) ? (DWORD)(R_CX & 0x3F)
                                                    : FILE_ATTRIBUTE_NORMAL);
        if (f == INVALID_HANDLE_VALUE) {
            /* Same collapse as AH=3Dh had, same fix -- see DosErrFromWin32(). */
            DWORD we = GetLastError(); WORD de;
            INT mapped = DosErrFromWin32((unsigned long)we, &de);
            SETAX(de); ERRCF();
            tp = zput(tp, lfn_alias ? "  INT21 AX=71" : "  INT21 AH=6C");
            if (lfn_alias) tp = zhexb(tp, lfn_alias);
            tp = zput(tp, " ["); tp = zput(tp, fn);
            tp = zput(tp, "] FAILED win32=0x"); tp = zhex(tp, we);
            tp = zput(tp, mapped ? " -> AX=0x" : " UNMAPPED, kept -> AX=0x");
            tp = zhex(tp, de); tp = zput(tp, "\r\n");
        }
        else {
            WORD res = (WORD)DosExtOpenActionTaken((UINT)disp,
                                                        GetLastError() == ERROR_ALREADY_EXISTS,
                                                        lfn_alias != 0);
            slot = DosHandleAllocate((PVOID const *)m->FileHandles, m->StdOpen);
            if (slot < DOS_MAX_FILES) { m->FileHandles[slot] = f; SETAX(slot); SET16(R_CX, res); OKCF();
                                        if (res != 1) DosStampVdmNow(f); /* #263: created/truncated */ }
            else { CloseHandle(f); SETAX(4); ERRCF(); }
        }
    } else if (ah == 0x59) {                    /* get extended error */
        /* Four answers, not one: extended code (AX), class (BH), suggested
           action (BL) and locus (CH).  The pairings are MEASURED, by provoking
           each failure on the oracle and asking (tests/probes/dos/p_err.asm):
             codes 2, 3, 18  -> BX=0803, CH=02   (not-found family)
             code  6         -> BX=0704, CH=01   (bad handle)
           CL is left ALONE -- the oracle returns it still holding the caller's
           value, so writing it would be an invention. */
        WORD e = m->LastError, bx59 = 0;
        BYTE ch59 = 0;
        /* The table moved to src/dos/dos_err.h so the off-VM battery can pin it
           (tests/unit/err_test.c) and so there is exactly one place a row can
           be added. Rows 5 (access denied) and 0x50 (file exists) were provoked
           and measured in session 52; before that both fell into the UNMEASURED
           arm below. */
        if (!DosErrClassify(e, &bx59, &ch59)) {
            /* Rather than fabricate a class for a code we have not provoked on
               real DOS, say so. Extend p_err.asm and dos_err.h together. */
            tp = zput(tp, "  INT21 AH=59 class/action/locus UNMEASURED for code 0x");
            tp = zhex(tp, e); tp = zput(tp, "\r\n");
        }
        SETAX(e);
        SET16(R_BX, bx59);
        R_CX = (R_CX & 0xFFFF00FFu) | (((DWORD)ch59 & 0xFF) << 8);
        OKCF();
    } else if (ah == 0x60) {                    /* truename: DS:SI -> ES:DI */
        CHAR in[300], out[300];
        DWORD n;
        DosGuestPath(m, R_DS, R_SI, in, sizeof(in));
        n = GetFullPathNameA(in, sizeof(out), out, NULL);
        if (n == 0 || n >= sizeof(out)) { SETAX(3); ERRCF(); }
        else {
            volatile BYTE *d = (volatile BYTE *)((R_ES << 4) + (R_DI & 0xFFFF));
            INT k = 0;
            /* Oracle: a RELATIVE name resolves against the current directory and
               comes back fully qualified and UPPER CASE, and existence is not
               required -- "SUB\\FILE.TXT" became "C:\\SUB\\FILE.TXT" with no such dir. */
            while (out[k] && k < 127) {
                CHAR ch = out[k];
                if (ch >= 'a' && ch <= 'z') ch = (CHAR)(ch - 32);
                d[k] = (BYTE)ch; ++k;
            }
            d[k] = 0;
            OKCF();
        }
    } else if (ah == 0x65) {                    /* get extended country info */
        BYTE al65 = (BYTE)(R_AX & 0xFF);
        if (al65 == 0x01) {
            /* Oracle layout: [0]=1 id, [1-2]=size 0x26, [3-4]=country,
               [5-6]=code page, [7-40]=34-byte country block.  41 bytes total.
               NOTE the block here is the 34-byte form (24 meaningful + 10 zero),
               where AH=38h writes only 24 -- measured, not assumed. */
            volatile BYTE *d = (volatile BYTE *)((R_ES << 4) + (R_DI & 0xFFFF));
            WORD cap = (WORD)(R_CX & 0xFFFF), k;
            BYTE blk[41];
            INT j;
            for (j = 0; j < 41; ++j) blk[j] = 0;
            blk[0] = 0x01; blk[1] = 0x26; blk[2] = 0x00;
            blk[3] = 0x01; blk[4] = 0x00;                /* country 1  */
            blk[5] = 0xB5; blk[6] = 0x01;                /* code page 437 */
            for (j = 0; j < 24; ++j) blk[7 + j] = g_DosCountryUs[j];
            blk[7 + 18] = (BYTE)(DOS_CASEMAP_OFF & 0xFF);
            blk[7 + 19] = (BYTE)(DOS_CASEMAP_OFF >> 8);
            blk[7 + 20] = (BYTE)(DOS_HDLR_SEG & 0xFF);
            blk[7 + 21] = (BYTE)(DOS_HDLR_SEG >> 8);
            for (k = 0; k < 41 && k < cap; ++k) d[k] = blk[k];
            SETAX(0x01B5); OKCF();                       /* oracle: AX = code page */
        } else if (al65 >= 0x02 && al65 <= 0x07) {
            /* Table subfunctions: ES:DI gets a 5-byte descriptor -- the
               subfunction id, then a FAR POINTER to the table itself.  ATTRIB
               wants AL=07 and COMMAND.COM AL=04, which is why AL=01 alone was
               not enough. Offsets from dos_layout.h; contents in dos_ctab.h,
               dumped from the oracle rather than synthesised. */
            volatile BYTE *d = (volatile BYTE *)((R_ES << 4) + (R_DI & 0xFFFF));
            WORD off = 0;
            switch (al65) {
            case 0x02: off = DOS_CTAB_UPPER;   break;
            case 0x04: off = DOS_CTAB_FNUPPER; break;
            case 0x05: off = DOS_CTAB_FNTERM;  break;
            case 0x06: off = DOS_CTAB_COLLATE; break;
            case 0x07: off = DOS_CTAB_DBCS;    break;
            default:   off = DOS_CTAB_UPPER;   break;   /* AL=03, same shape */
            }
            d[0] = al65;
            d[1] = (BYTE)(off & 0xFF);        d[2] = (BYTE)(off >> 8);
            d[3] = (BYTE)(DOS_CTAB_SEG & 0xFF); d[4] = (BYTE)(DOS_CTAB_SEG >> 8);
            SETAX(0x01B5); OKCF();
        } else if (al65 >= 0x20 && al65 <= 0x22) {
            /* ── CAPITALISE: a character (DL), CX bytes at DS:DX, or ASCIIZ at DS:DX.
                 (GH #165) Through the SAME uppercase table AL=02 hands out (dumped
                 from 6.22), so a program that capitalises through DOS and one that
                 reads the table agree. Measured: 'a'->'A', 81h->9Ah, digits kept. */
            if (al65 == 0x20) {
                SET16(R_DX, (R_DX & 0xFF00) | DosCtabUpcase437((BYTE)(R_DX & 0xFF)));
            } else {
                volatile BYTE *s = (volatile BYTE *)((R_DS << 4) + (R_DX & 0xFFFF));
                UINT32 k, n = (al65 == 0x21) ? (UINT32)(R_CX & 0xFFFF) : 0x10000u;
                for (k = 0; k < n; ++k) {
                    if (al65 == 0x22 && s[k] == 0) break;
                    s[k] = DosCtabUpcase437(s[k]);
                }
            }
            OKCF();
        } else if (al65 == 0x23) {
            /* YES/NO for the country: AX = 0 no, 1 yes, 2 neither. Country 1 only,
               like everything else here. Measured: 'y'->1, 'N'->0, 'q'->2. */
            BYTE c = DosCtabUpcase437((BYTE)(R_DX & 0xFF));
            SETAX(c == 'Y' ? 1 : c == 'N' ? 0 : 2); OKCF();
        } else {
            tp = zput(tp, "  INT21 AH=65 AL=0x"); tp = zhex(tp, al65);
            /* Not a gap: 6.22 and PCem answer AX=1 CF=1 too (p_subfn int21.6508). */
            tp = zput(tp, " not a 6.22 subfunction -> AX=1 CF=1 (matches DOS)\r\n");
            SETAX(1); ERRCF();
        }
    } else if (ah == 0x69) {                    /* get/set volume serial number */
        BYTE al69 = (BYTE)(R_AX & 0xFF);
        if (al69 == 0x00) {
            /* Oracle layout: [0-1] NOT WRITTEN (came back poisoned), [2-5]
               serial dword, [6-16] 11-byte label, [17-24] 8-byte fs type. */
            volatile BYTE *d = (volatile BYTE *)((R_DS << 4) + (R_DX & 0xFFFF));
            CHAR label[64], fstype[32];
            DWORD serial = 0, maxc = 0, flags = 0;
            INT k;
            for (k = 0; k < 64; ++k) label[k] = 0;
            for (k = 0; k < 32; ++k) fstype[k] = 0;
            BYTE drv = DosSerialDrive(m, (BYTE)(R_BX & 0xFF));
            if (drv < 26 && g_DosSerialIsSet[drv]) {         /* #165: set this session */
                for (k = 0; k < 23; ++k) d[2 + k] = g_DosSerialInfo[drv][k];
                OKCF();
            } else if (GetVolumeInformationA(NULL, label, sizeof(label), &serial,
                                      &maxc, &flags, fstype, sizeof(fstype))) {
                d[2] = (BYTE)( serial        & 0xFF);
                d[3] = (BYTE)((serial >> 8)  & 0xFF);
                d[4] = (BYTE)((serial >> 16) & 0xFF);
                d[5] = (BYTE)((serial >> 24) & 0xFF);
                for (k = 0; k < 11; ++k) d[6 + k]  = (BYTE)(label[k] ? label[k] : ' ');
                for (k = 0; k < 8;  ++k) d[17 + k] = (BYTE)(fstype[k] ? fstype[k] : ' ');
                OKCF();
            } else { SETAX(0x0F); ERRCF(); }
        } else if (al69 == 0x01) {
            /* ── SET SERIAL (#165). Real DOS writes serial, label and file-system type
                 into the disk's boot record (p_4b05: 6.22 and PCem accept it and 6900h
                 reads the new serial back; DOSBox-X refuses). Our drives are the host's
                 own disks, so by the user's decision (2026-09-28) it is SESSION-ONLY:
                 remembered per drive, answered by 6900h until this VDM ends, and
                 nothing is written to the host disk. Same 25-byte layout as 6900h. */
            const volatile BYTE *sb = (const volatile BYTE *)((R_DS << 4) + (R_DX & 0xFFFF));
            BYTE drv = DosSerialDrive(m, (BYTE)(R_BX & 0xFF));
            INT k;
            if (drv < 26) {
                for (k = 0; k < 23; ++k) g_DosSerialInfo[drv][k] = sb[2 + k];
                g_DosSerialIsSet[drv] = 1;
                tp = zput(tp, "  INT21 AX=6901 set serial -- kept for this session only\r\n");
                OKCF();
            } else { SETAX(0x0F); ERRCF(); }
        } else {
            tp = zput(tp, "  INT21 AH=69 AL=0x"); tp = zhex(tp, al69);
            tp = zput(tp, " UNIMPLEMENTED\r\n");
            m->Unimplemented[0x69 >> 3] |= (BYTE)(1u << (0x69 & 7));
            SETAX(1); ERRCF();
        }
    } else if (ah == 0x47) {                    /* get current directory -> DS:SI */
        /* DOS returns the path WITHOUT the drive letter and WITHOUT a leading
           backslash, ASCIIZ.  Oracle at the root writes exactly ONE byte -- the
           terminating NUL -- and leaves the rest of the caller's 64-byte buffer
           untouched, so we must not pad it.
           Wanted by four of the five real 6.22 tools we ran (TREE, ATTRIB,
           XCOPY, COMMAND.COM), which is why it came first.  GH #32. */
        CHAR cwd[300];
        DWORD n;
        BYTE dl47 = (BYTE)(R_DX & 0xFF);
        BYTE curdrv = (BYTE)(DosCurrentDrive(m) + 1);
        if (m->VirtualDrive >= 0) {           /* a drive Win32 cannot stand on: its =X: or root */
            CHAR spec[3]; spec[0] = (CHAR)('A' + m->VirtualDrive); spec[1] = ':'; spec[2] = 0;
            n = GetFullPathNameA(spec, sizeof(cwd), cwd, NULL);
        } else n = GetCurrentDirectoryA(sizeof(cwd), cwd);
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
        if (dl47 == 0xF0) {
            tp = zput(tp, "  INT21 AH=47 drive 0xF0 (WOW sentinel) -> current drive\r\n");
            dl47 = 0;
        }
        /* ── ★ AND PER-DRIVE CURRENT DIRECTORIES ARE REAL DOS BEHAVIOUR. ─────────
             This answered only for the drive we happened to be on and returned
             "invalid drive" for every other, with a note saying so. Win32 keeps a
             current directory per drive too -- that is what the hidden `=C:`
             environment variables are -- and GetFullPathNameA("X:") reads it. So
             ask for the drive the caller named, and keep the honest refusal for a
             drive that genuinely is not there (GetLogicalDrives), which is the
             error DOS itself returns. GH #32. */
        if (n && dl47 && dl47 != curdrv && dl47 <= 26) {
            if (GetLogicalDrives() & (1u << (dl47 - 1))) {
                CHAR spec[4]; spec[0] = (CHAR)('A' + dl47 - 1); spec[1] = ':';
                spec[2] = 0;
                n = GetFullPathNameA(spec, sizeof(cwd), cwd, NULL);
            } else n = 0;
        }
        if (n == 0 || n >= sizeof(cwd)) {
            tp = zput(tp, "  INT21 AH=47 drive 0x"); tp = zhex(tp, dl47);
            tp = zput(tp, " -> invalid drive\r\n");
            SETAX(0x0F); ERRCF();
        } else {
            volatile BYTE *dst = (volatile BYTE *)((R_DS << 4) + (R_SI & 0xFFFF));
            PCSTR p47 = cwd;
            INT k = 0;
            /* ── #164: SHORT AND UPPER CASE, AS DOS KEEPS IT. (s85) ─────────────────
                 The host hands back whatever case and length the directory was
                 entered with ("...\ntvdmex\demo\win16"). DOS's CDS holds an upper-case
                 8.3 path, and stock NTVDM answers exactly that -- measured beside
                 ours by tests/probes/win16/w_cwd on the rig: ours `...\ntvdmex\demo\...`,
                 stock `...\NTVDMEX\DEMO\...`. */
            {   CHAR sp47[300];
                DWORD sn = GetShortPathNameA(cwd, sp47, sizeof sp47);
                INT u;
                if (sn && sn < sizeof sp47) lstrcpynA(cwd, sp47, sizeof cwd);
                for (u = 0; cwd[u]; ++u)
                    if (cwd[u] >= 'a' && cwd[u] <= 'z') cwd[u] = (CHAR)(cwd[u] - 32);
            }
            if (cwd[1] == ':') p47 += 2;              /* drop "C:"            */
            if (*p47 == '\\' || *p47 == '/') ++p47;   /* drop the separator   */
            while (p47[k] && k < 63) { dst[k] = (BYTE)p47[k]; ++k; }
            dst[k] = 0;
            SETAX(0x0100); OKCF();                    /* oracle: AX=0100      */
        }
    } else if (ah == 0x3B) {                    /* chdir: DS:DX = ASCIIZ path */
        /* ── ★ CHDIR NEVER MOVES THE CURRENT DRIVE. ─────────────────────────────
             Oracle (p_drv.asm): `3Bh C:\ZZDRV` issued from A: leaves 19h at A:, and
             47h for C: then answers ZZDRV -- every drive keeps its own directory.
             SetCurrentDirectoryA("C:\ZZDRV") moves the PROCESS to C:, which made
             the DOS current drive follow it. So a path on another drive only sets
             that drive's =X: variable (which is what "X:" resolves through); the
             process current directory changes only for the drive we are on.
             Also measured: a bare "C:" is path-not-found (AX=3), not a no-op. */
        CHAR fn[300], full[300];
        DWORD n;
        BYTE tgt;
        DosGuestPath(m, R_DS, R_DX, fn, sizeof(fn));
        tp = zput(tp, "  INT21 AH=3B chdir ["); tp = zput(tp, fn); tp = zput(tp, "]");
        if (fn[0] && fn[1] == ':' && !fn[2]) { SETAX(3); ERRCF(); tp = zput(tp, " -> 3 (drive only)\r\n"); }
        else if ((n = GetFullPathNameA(fn, sizeof(full), full, NULL)) == 0 || n >= sizeof(full)
                 || full[1] != ':') { SETAX(3); ERRCF(); tp = zput(tp, " -> 3\r\n"); }
        else {
            tgt = (BYTE)((full[0] | 0x20) - 'a');
            if (tgt == DosCurrentDrive(m)) {
                if (SetCurrentDirectoryA(full)) {
                    m->VirtualDrive = -1;       /* it can be stood on after all */
                    DosNoteDriveDirectory(full);
                    OKCF(); tp = zput(tp, " -> ok\r\n");
                } else { SETAX(3); ERRCF(); tp = zput(tp, " -> 3 (0x"); tp = zhex(tp, GetLastError());
                         tp = zput(tp, ")\r\n"); }   /* oracle: AX=0003, CF=1 */
            } else {
                DWORD a = GetFileAttributesA(full);
                if (a != 0xFFFFFFFFu && (a & FILE_ATTRIBUTE_DIRECTORY)) {
                    DosNoteDriveDirectory(full);
                    OKCF(); tp = zput(tp, " -> ok (another drive's directory; current drive unchanged)\r\n");
                } else { SETAX(3); ERRCF(); tp = zput(tp, " -> 3 (other drive, 0x"); tp = zhex(tp, GetLastError());
                         tp = zput(tp, ")\r\n"); }
            }
        }
    } else if (ah == 0x36) {                    /* get free disk space: DL = drive */
        /* AX=sectors/cluster BX=free clusters CX=bytes/sector DX=total clusters.
           AN INVALID DRIVE RETURNS AX=FFFF WITH CARRY CLEAR -- oracle-confirmed,
           and easy to get wrong: it is not a CF error.  Counts are 16-bit in the
           DOS interface, so a large volume has to be clamped rather than wrapped. */
        BYTE dl36 = (BYTE)(R_DX & 0xFF);
        DWORD spc = 0, bps = 0, freec = 0, totc = 0;
        CHAR root[4]; PSTR rp = 0;
        if (!dl36 && m->VirtualDrive >= 0) dl36 = (BYTE)(m->VirtualDrive + 1);
        if (dl36) { root[0] = (CHAR)('A' + dl36 - 1); root[1] = ':'; root[2] = '\\';
                    root[3] = 0; rp = root; }
        if (dl36 <= 26 && GetDiskFreeSpaceA(rp, &spc, &bps, &freec, &totc)) {
            SETAX((WORD)spc);
            SET16(R_BX, freec > 0xFFFF ? 0xFFFF : freec);
            SET16(R_CX, (WORD)bps);
            SET16(R_DX, totc  > 0xFFFF ? 0xFFFF : totc);
            OKCF();
        } else {
            SETAX(0xFFFF); OKCF();
        }
    } else if (ah == 0x38) {                    /* get/set country information */
        BYTE al38 = (BYTE)(R_AX & 0xFF);
        WORD want = (al38 == 0xFF) ? (WORD)(R_BX & 0xFFFF)
                                       : (WORD)(al38 ? al38 : 1);
        if ((R_DX & 0xFFFF) == 0xFFFF) {        /* DX=FFFF selects SET, not GET */
            /* ── MEASURED, 6.22 AND PCem, NO COUNTRY.SYS (p_subfn): setting the
                 CURRENT country succeeds (AX=1 CF=0); any other fails AX=1 CF=1,
                 because the data for it would come from COUNTRY.SYS and none was
                 loaded. We are that machine: country 1 and nothing else. (GH #165) */
            SETAX(1);
            if (want == 1) OKCF();
            else {
                tp = zput(tp, "  INT21 AH=38 SET country 0x"); tp = zhex(tp, want);
                tp = zput(tp, " refused: only country 1 is loaded (matches DOS without COUNTRY.SYS)\r\n");
                ERRCF();
            }
        } else if (want == 1) {                 /* USA -- the only block we have */
            volatile BYTE *b = (volatile BYTE *)((R_DS << 4) + (R_DX & 0xFFFF));
            INT k;
            for (k = 0; k < 24; ++k) b[k] = g_DosCountryUs[k];
            b[18] = (BYTE)(DOS_CASEMAP_OFF & 0xFF);
            b[19] = (BYTE)(DOS_CASEMAP_OFF >> 8);
            b[20] = (BYTE)(DOS_HDLR_SEG & 0xFF);
            b[21] = (BYTE)(DOS_HDLR_SEG >> 8);
            SETAX(1); SET16(R_BX, 1); OKCF();
        } else {
            /* We only have measured data for country 1. Inventing a block for
               another country would be exactly the from-memory guess the
               programme forbids, so say so rather than fabricate one. */
            /* ...and 6.22 without COUNTRY.SYS answers exactly this: AX=1 CF=1
               (p_subfn int21.382C.get). It was AX=2. */
            tp = zput(tp, "  INT21 AH=38 country 0x"); tp = zhex(tp, want);
            tp = zput(tp, " refused: only country 1 is loaded (matches DOS without COUNTRY.SYS)\r\n");
            SETAX(1); ERRCF();
        }
    } else if (ah == 0x58) {                    /* get/set memory allocation strategy */
        BYTE al58 = (BYTE)(R_AX & 0xFF);
        if (al58 == 0x00)      { SETAX(m->AllocationStrategy); OKCF(); }
        else if (al58 == 0x01) { m->AllocationStrategy = (BYTE)(R_BX & 0xFF); OKCF(); }
        /* Oracle: AL=02 returns the state in AL and LEAVES AH ALONE -- 6.22
           answered AX=5800 to a call made with AX=5802. */
        else if (al58 == 0x02) { SETAX((R_AX & 0xFF00) | m->UmbLink); OKCF(); }
        else if (al58 == 0x03) {
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
            tp = zput(tp, "  INT21 AH=5803 UMB link refused: no UMB provider "
                          "(oracle: AX=0001 CF=1)\r\n");
            SETAX(1); ERRCF();
        }
        else {
            tp = zput(tp, "  INT21 AH=58 AL=0x"); tp = zhex(tp, al58);
            /* Not a gap: 6.22 and PCem answer AX=1 CF=1 too (p_subfn int21.5804). */
            tp = zput(tp, " not a 6.22 subfunction -> AX=1 CF=1 (matches DOS)\r\n");
            SETAX(1); ERRCF();
        }
    } else if (ah == 0x52) {                    /* get list of lists -> ES:BX */
        /* The word at ES:BX-2 is the first MCB segment, and that is the field a
           memory walker actually follows -- it is filled in truthfully from our
           own MCB chain. The rest of SysVars is a stub, so it is ZEROED rather
           than left as whatever was in memory: a walker that follows a garbage
           DPB or SFT pointer wanders off into nonsense, which is the silent
           failure #27 exists to remove, whereas a null pointer stops it. */
        if (m->SysvarsSegment) {
            SET16(R_ES, m->SysvarsSegment);
            SET16(R_BX, m->SysvarsOffset);
            tp = zput(tp, "  INT21 AH=52 list-of-lists (MCB head only; rest stubbed)\r\n");
            OKCF();
        } else {
            tp = zput(tp, "  INT21 AH=52 UNIMPLEMENTED (no SysVars planted)\r\n");
            m->Unimplemented[0x52 >> 3] |= (BYTE)(1u << (0x52 & 7));
            SETAX(1); ERRCF();
        }
    } else if (ah == 0x44) {                    /* IOCTL (C-runtime isatty etc.) */
        BYTE al = (BYTE)(R_AX & 0xFF);
        WORD bx = (WORD)(R_BX & 0xFFFF);
        /* 4400h: the device-information word, in DX AND in AX (6.22 and PCem both
             return AX = DX, p_ioctl's own captures included). #251: AUX is 80C0h and
             PRN A0C0h (bit 13, output-until-busy) -- measured, p_auxprn. */
        if (al == 0x00) {
            WORD w = (bx < 5) ? 0x80D3 : 0x0002;
            if ((bx == 3 || bx == 4) && DosHandleIsDevice((PVOID const *)m->FileHandles, m->StdOpen, bx))
                w = (bx == 3) ? 0x80C0 : 0xA0C0;
            SET16(R_DX, w); SETAX(w); OKCF();
        }
        else if (al == 0x06 || al == 0x07) { SETAX((R_AX & 0xFF00) | 0xFF); OKCF(); }
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
        else if (al == 0x08 || al == 0x09 || al == 0x0E) {
            BYTE drv = (BYTE)(bx & 0xFF);            /* 0 = default drive           */
            UINT ty = 0;
            if (!drv) drv = (BYTE)(DosCurrentDrive(m) + 1);
            if (drv >= 1 && drv <= 26 && (GetLogicalDrives() & (1u << (drv - 1)))) {
                CHAR root[4]; root[0] = (CHAR)('A' + drv - 1); root[1] = ':';
                root[2] = '\\'; root[3] = 0;
                ty = GetDriveTypeA(root);
            }
            if (!ty || ty == 1) { SETAX(0x000F); ERRCF(); }   /* invalid drive      */
            else if (al == 0x08) {
                /* AX = 0 removable, 1 fixed. A CD is removable media. */
                SETAX((ty == DRIVE_REMOVABLE || ty == DRIVE_CDROM) ? 0 : 1); OKCF();
            } else if (al == 0x09) {
                /* DX = the device attribute word; bit 12 = the drive is remote.
                   Nothing else in it is load-bearing for the callers we have. */
                SET16(R_DX, (ty == DRIVE_REMOTE) ? 0x1000 : 0x0000); OKCF();
            } else {
                /* AL = 0 when only one letter maps to the block device, which is
                   true of every drive we can see (we do not emulate a SUBST). */
                SETAX(R_AX & 0xFF00); OKCF();
            }
            tp = zput(tp, "  INT21 AH=44 AL=0x"); tp = zhex(tp, al);
            tp = zput(tp, " drive 0x"); tp = zhex(tp, drv);
            tp = zput(tp, " type "); tp = zhex(tp, ty);
            tp = zput(tp, " -> AX=0x"); tp = zhex(tp, R_AX & 0xFFFF);
            tp = zput(tp, " DX=0x"); tp = zhex(tp, R_DX & 0xFFFF);
            tp = zput(tp, (*pfl & 1) ? " (err)\r\n" : "\r\n");
        }
        /* ── #251: WHAT DOS SUPPORTS SUCCEEDS; THE REST IS "INVALID FUNCTION". ───────
             Every other sub-function answered CF=0 -- success, with nothing done.
             Measured (tests/probes/dos/p_ioctl2) on msdos622, dosbox-x and pcem alike:
             02h/04h (read control data), 0Ch/10h on CON, and the unassigned 12h/1Fh
             answer AX=0001 CF=1; 0Ah, 0Dh and 11h on a fixed disk answer CF=0.
             0Ah reports a local handle (DX bit 15 clear). 0Dh/11h keep today's bare
             success -- a disk-parameter block is a separate piece of work, and turning
             a success into a refusal there would break callers that work now. */
        else if (al == 0x01 || al == 0x0B || al == 0x0D || al == 0x0F) { OKCF(); }
        else if (al == 0x0A) { SET16(R_DX, 0x0000); OKCF(); }
        else if (al == 0x11) { SETAX(R_AX & 0xFF00); OKCF(); }   /* AL=0: supported */
        else                 { SETAX(0x0001); ERRCF(); }
        if (al != 0x08 && al != 0x09 && al != 0x0E) {
            tp = zput(tp, "  INT21 AH=44 ioctl AL=0x"); tp = zhex(tp, al);
            tp = zput(tp, " BX=0x"); tp = zhex(tp, bx); tp = zput(tp, "\r\n");
        }
    } else if (ah == 0x63) {                    /* get DBCS lead-byte table */
        if ((R_AX & 0xFF) == 0) { SET16(R_DS, DOS_HDLR_SEG); SET16(R_SI, DOS_DBCS_OFF); }
        SETAX(R_AX & 0xFF00); OKCF();
        tp = zput(tp, "  INT21 AH=63 DBCS lead-byte table\r\n");
    } else if (ah == 0x25) {                    /* set interrupt vector: AL=int DS:DX */
        DWORD v = (R_AX & 0xFF) * 4;
        *(volatile WORD *)(v)     = (WORD)(R_DX & 0xFFFF);
        *(volatile WORD *)(v + 2) = (WORD)(R_DS & 0xFFFF);
        OKCF();
    } else if (ah == 0x35) {                    /* get interrupt vector: AL=int -> ES:BX */
        DWORD v = (R_AX & 0xFF) * 4;
        SET16(R_BX, *(volatile WORD *)(v));
        SET16(R_ES, *(volatile WORD *)(v + 2));
        OKCF();
    } else if (ah == 0x48) {                    /* allocate BX paras -> AX=seg (err: BX=max) */
        WORD want = (WORD)(R_BX & 0xFFFF), seg = 0, max = 0;
        INT err = DosMcbAllocate(NULL, m->FirstMcb, want, &seg, &max);
        if (err) { SET16(R_AX, err); SET16(R_BX, max); ERRCF(); }
        else     { SET16(R_AX, seg); OKCF(); }
        /* ── THE BLOCK BELONGS TO THE PROGRAM THAT ASKED. (s80) ───────────────────
             DOS stamps an allocation with the CURRENT PSP, and that is how it frees a
             terminated child's memory: every block its PSP owns. DosMcbAllocate() writes
             DOS_PSP_SEG unconditionally, which is right for the top-level program (its
             PSP is DOS_PSP_SEG) and wrong for every child -- so nothing a child
             allocated was ever given back. Measured: DOS/4GW's five real-mode blocks
             outlived Doom, and the next `doom` loaded 85 KB higher. */
        if (!err && seg && m->PspSegment)
            DosMcbWriteWord(DosMcbSegmentAddress(NULL, (WORD)(seg - 1)) + 1, m->PspSegment);
        tp = zput(tp, "  INT21 AH=48 alloc 0x"); tp = zhex(tp, want);
        tp = zput(tp, (*pfl & 1) ? " -> err max=0x" : " -> seg=0x");
        tp = zhex(tp, (*pfl & 1) ? max : (R_AX & 0xFFFF)); tp = zput(tp, "\r\n");
    } else if (ah == 0x49) {                    /* free block: ES=segment */
        INT err = DosMcbFree(NULL, (WORD)(R_ES & 0xFFFF));
        /* ── #258: AND A SUCCESSFUL FREE LEAVES AX = THE BLOCK'S MCB. ────────────────
             Undocumented, measured (tests/probes/dos/p_memax): MS-DOS 6.22 and PCem's
             MS-DOS both return AX = ES-1; dosbox-x leaves AX alone. The Microsoft
             kernel is the authority. We left AX as the caller's 49xx. */
        if (err) { SET16(R_AX, err); ERRCF(); }
        else { SET16(R_AX, (WORD)((R_ES & 0xFFFF) - 1)); OKCF(); }
        tp = zput(tp, "  INT21 AH=49 free seg=0x"); tp = zhex(tp, R_ES & 0xFFFF);
        tp = zput(tp, (*pfl & 1) ? " (err)\r\n" : "\r\n");
        /* A refused free names a block the caller believes in and we do not: show
           what is actually at seg-1, and the chain, so the two can be compared. */
        if (err) {
            const volatile BYTE *mb = (const volatile BYTE *)(((R_ES & 0xFFFF) - 1u) << 4);
            WORD s;
            INT k, n = 0;
            tp = zput(tp, "    at seg-1: ");
            for (k = 0; k < 16; ++k) { tp = zhexb(tp, mb[k]); tp = zput(tp, " "); }
            tp = zput(tp, "\r\n    chain:");
            s = m->FirstMcb;
            while (s && n++ < 40) {
                const volatile BYTE *mc = (const volatile BYTE *)((DWORD)s << 4);
                WORD own = (WORD)(mc[1] | (mc[2] << 8)), sz = (WORD)(mc[3] | (mc[4] << 8));
                tp = zput(tp, " "); tp = zhex(tp, s); tp = zput(tp, mc[0] == 'Z' ? "Z" : mc[0] == 'M' ? "M" : "?");
                tp = zput(tp, "/"); tp = zhex(tp, own); tp = zput(tp, "/"); tp = zhex(tp, sz);
                if (mc[0] != 'M') break;
                s = (WORD)(s + 1 + sz);
            }
            tp = zput(tp, "\r\n");
        }
    } else if (ah == 0x4A) {                    /* resize: ES=block BX=new paras */
        WORD want = (WORD)(R_BX & 0xFFFF), max = 0;
        INT err = DosMcbResize(NULL, (WORD)(R_ES & 0xFFFF), want, &max);
        /* ── #258: A SUCCESSFUL RESIZE LEAVES AX = THE BLOCK'S SEGMENT. ──────────────
             Undocumented, and QuickBASIC 4.5 depends on it: its Quick Library loader
             takes AX after shrinking a top-of-memory block as the block's segment.
             We left the caller's 4Axx there, so `QB /L` loaded the library at 4Axx,
             freed a block that never existed at Make EXE ("Error in loading file
             (QB.QLB) - Internal error") and wrote a garbage .LIB into the link.
             Measured (tests/probes/dos/p_memax): MS-DOS 6.22, dosbox-x and PCem all
             return AX = ES for a shrink, a grow and a same-size resize. */
        if (err) { SET16(R_AX, err); if (err == 8) SET16(R_BX, max); ERRCF(); }
        else { SET16(R_AX, (WORD)(R_ES & 0xFFFF)); OKCF(); }
        tp = zput(tp, "  INT21 AH=4A resize seg=0x"); tp = zhex(tp, R_ES & 0xFFFF);
        tp = zput(tp, " -> 0x"); tp = zhex(tp, want);
        tp = zput(tp, (*pfl & 1) ? " (err)\r\n" : "\r\n");
    } else if (ah == 0x51 || ah == 0x62) {      /* get current PSP -> BX */
        SET16(R_BX, m->PspSegment); OKCF();
    } else if (ah == 0x50) {                    /* set current PSP */
        m->PspSegment = (WORD)(R_BX & 0xFFFF); OKCF();
    } else if (ah == 0x1A) {                    /* set DTA = DS:DX */
        m->DtaSegment = (WORD)(R_DS & 0xFFFF); m->DtaOffset = (WORD)(R_DX & 0xFFFF); OKCF();
    } else if (ah == 0x2F) {                    /* get DTA -> ES:BX */
        SET16(R_ES, m->DtaSegment); SET16(R_BX, m->DtaOffset); OKCF();
    } else if (ah == 0x19) {                    /* get current drive -> AL (C: = 2) */
        /* ── THE CURRENT DRIVE IS THE HOST CURRENT DIRECTORY'S, LIKE AH=47h's. ─────
             This returned a constant while AH=0Eh below was accepted and ignored, so
             a program probing drives the classic way -- select X, read back, compare
             -- found only C:. QB.EXE's file dialog does exactly that (39BCCh..39BE4h)
             and listed one drive on a machine with four. */
        SETAX((R_AX & 0xFF00) | DosCurrentDrive(m)); OKCF();
    } else if (ah == 0x0E) {                    /* select drive -> AL = LASTDRIVE  */
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
        BYTE dl = (BYTE)(R_DX & 0xFF);
        if (dl < 26 && (GetLogicalDrives() & (1u << dl))) {
            CHAR spec[3], cw[300];
            spec[0] = (CHAR)('A' + dl); spec[1] = ':'; spec[2] = 0;
            /* remember the directory we are leaving; "X:" resolves through =X: */
            if (m->VirtualDrive < 0 && GetCurrentDirectoryA(sizeof(cw), cw)) DosNoteDriveDirectory(cw);
            if (SetCurrentDirectoryA(spec)) m->VirtualDrive = -1;
            else {                                       /* e.g. no media: try the root */
                CHAR root[4]; root[0] = spec[0]; root[1] = ':'; root[2] = '\\'; root[3] = 0;
                if (SetCurrentDirectoryA(root)) m->VirtualDrive = -1;
                else {
                    m->VirtualDrive = dl;
                    tp = zput(tp, "  INT21 AH=0E drive "); *tp++ = spec[0];
                    tp = zput(tp, ": exists but is not ready (Win32 error 0x");
                    tp = zhex(tp, GetLastError());
                    tp = zput(tp, ") -> selected as the DOS current drive anyway\r\n");
                }
            }
        }
        SETAX((R_AX & 0xFF00) | DOS_LASTDRIVE); OKCF();
    } else if (ah == 0x0D) {                    /* disk reset (flush) -> nop */
        OKCF();
    } else if (ah == 0x33) {                    /* get/set Ctrl-Break, get true version */
        BYTE al33 = (BYTE)(R_AX & 0xFF);
        /* ── THE FLAG IS STATE, NOT A CONSTANT. (GH #165) ──────────────────────
             Get used to answer "off" and set accepted a value and dropped it, so a
             program that turned checking on read back off. Measured, 6.22 and PCem
             (tests/probes/dos/p_subfn.asm): 3301 DL=1 then 3300 -> DL=1; 3302 swaps
             and returns the OLD state in DL. DH is left alone -- 6.22 does. */
        if (al33 == 0x00) { SET16(R_DX, (R_DX & 0xFF00) | m->IsBreakOn); OKCF(); }
        else if (al33 == 0x01) { m->IsBreakOn = (BYTE)((R_DX & 0xFF) ? 1 : 0); OKCF(); }
        else if (al33 == 0x02) {
            BYTE old = m->IsBreakOn;
            m->IsBreakOn = (BYTE)((R_DX & 0xFF) ? 1 : 0);
            SET16(R_DX, (R_DX & 0xFF00) | old); OKCF();
        }
        else if (al33 == 0x05) { SET16(R_DX, 3); OKCF(); }     /* boot drive = C: */
        else if (al33 == 0x06) {                               /* get TRUE version */
            /* BL=major BH=minor DL=revision DH=flags.  DH bit 3 = DOS in ROM,
               bit 4 = DOS in HMA; we are in neither, so 0.  Note the oracle
               reports DH=0x10 because that image boots DOS=HIGH -- DH is a
               property of the host's configuration, not of the version, which
               is why the probes do not compare it. */
            /* ⚠ Real SETVER leaves the TRUE version alone. Ours follows the same
                 per-process rule anyway: whether XP's shell checks 3306h as well is not
                 measured, and a shell that refuses to start is the costlier mistake. */
            SET16(R_BX, DosVersionWord(m));
            SET16(R_DX, 0x0000);
            OKCF();
        } else {                                               /* not a 6.22 subfn */
            /* Not a gap: 6.22 and PCem both answer AL=FFh (p_subfn int21.3307) and
               leave the carry alone, so we do exactly that. */
            tp = zput(tp, "  INT21 AH=33 AL=0x"); tp = zhex(tp, al33);
            tp = zput(tp, " not a 6.22 subfunction -> AL=FF (matches DOS)\r\n");
            SETAX((R_AX & 0xFF00) | 0xFF);
        }
    } else if (ah == 0x2A) {                    /* get date: CX=yr DH=mon DL=day AL=dow */
        /* The VDM's clock, not the host's: host-now + whatever 2Bh/2Dh set. See
           dos_clock.h. With nothing set the offset is 0 and this is GetLocalTime.
           #262: a tick count the BIOS did not write is followed first, date included
           (its midnight rollovers are DOS's day number moving). */
        DOS_CLOCK_TIME g; DosClockSync(); DosClockRead(g_DosClock.DosOffset, &g);
        SET16(R_CX, g.Year);
        SET16(R_DX, ((g.Month & 0xFF) << 8) | (g.Day & 0xFF));
        SETAX((R_AX & 0xFF00) | (g.DayOfWeek & 0xFF));
        OKCF();
    } else if (ah == 0x2C) {                    /* get time: CH=hr CL=min DH=sec DL=cs */
        /* #262: CLOCK$ reads 0040:006C, so a raw store there moves this (p_tick2c
           tick2c.after.store) -- followed here, once, then host-now + offset again. */
        DOS_CLOCK_TIME g; DosClockSync(); DosClockRead(g_DosClock.DosOffset, &g);
        SET16(R_CX, ((g.Hour & 0xFF) << 8) | (g.Minute & 0xFF));
        SET16(R_DX, ((g.Second & 0xFF) << 8) | (g.Hundredths & 0xFF));
        OKCF();
    } else if (ah == 0x2B || ah == 0x2D) {      /* set date / set time */
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
        DOS_CLOCK_TIME host; INT ok;
        DosClockSync();                 /* #262: 2Bh keeps the time of day the COUNT says */
        DosClockHostNow(&host);
        if (ah == 0x2B) {
            UINT y = R_CX & 0xFFFF, mo = (R_DX >> 8) & 0xFF, d = R_DX & 0xFF;
            ok = DosClockIsDosDateValid(y, mo, d);
            if (ok) DosClockSetDate(&host, &g_DosClock.DosOffset, y, mo, d);
        } else {
            UINT h = (R_CX >> 8) & 0xFF, mi = R_CX & 0xFF;
            UINT s = (R_DX >> 8) & 0xFF, cs = R_DX & 0xFF;
            ok = DosClockIsTimeValid(h, mi, s, cs);
            if (ok) {
                DosClockSetTime(&host, &g_DosClock.DosOffset, h, mi, s, cs);
                if (m->SetTicks) m->SetTicks(m->TicksContext, DosClockTicksFromTime(h, mi, s, cs));
            }
        }
        if (ok) g_DosClock.RtcOffset = g_DosClock.DosOffset;
        tp = zput(tp, "  INT21 AH=0x"); tp = zhexb(tp, (UINT)ah);
        tp = zput(tp, ok ? " VDM clock set (host clock untouched)\r\n"
                         : " refused: invalid -> AL=FF, clock unchanged\r\n");
        SETAX((R_AX & 0xFF00) | (ok ? 0x00 : 0xFF));
        OKCF();
    } else if (ah == 0x71) {                    /* the long-filename API (#210) */
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
        BYTE al71 = (BYTE)(R_AX & 0xFF);
        if (al71 == 0x41) {              /* delete: DS:DX, SI=wildcards, CL/CH */
            CHAR fn[300];
            WORD si41 = (WORD)(R_SI & 0xFFFF);
            DosGuestPath(m, R_DS, R_DX, fn, sizeof(fn));
            if (si41 == 0) {
                /* SI=0: one file, no wildcards -- 41h with an LFN-shaped error code. */
                if (DeleteFileA(fn)) OKCF();
                else { SETAX(DosLfnError(GetLastError())); ERRCF(); }
            } else {
                /* SI=1: every match of the pattern whose attributes pass CL (allowed) and
                   CH (required); directories are never deleted. Success if any went. */
                WIN32_FIND_DATAA fd; HANDLE hf; INT any = 0, cut = 0, i;
                DWORD we = ERROR_FILE_NOT_FOUND;
                CHAR full[300];
                for (i = 0; fn[i]; ++i) if (fn[i] == '\\' || fn[i] == '/' || fn[i] == ':') cut = i + 1;
                hf = FindFirstFileA(fn, &fd);
                if (hf == INVALID_HANDLE_VALUE) we = GetLastError();
                else {
                    do {
                        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                        if (!DosLfnAttributesOk(fd.dwFileAttributes, (BYTE)(R_CX & 0xFF),
                                             (BYTE)((R_CX >> 8) & 0xFF))) continue;
                        if (cut + lstrlenA(fd.cFileName) >= (INT)sizeof(full)) continue;
                        for (i = 0; i < cut; ++i) full[i] = fn[i];
                        lstrcpynA(full + cut, fd.cFileName, sizeof(full) - cut);
                        if (DeleteFileA(full)) any = 1; else we = GetLastError();
                    } while (FindNextFileA(hf, &fd));
                    FindClose(hf);
                }
                if (any) OKCF(); else { SETAX(DosLfnError(we)); ERRCF(); }
            }
        } else if (al71 == 0x43) {              /* attributes and times: DS:DX, BL */
            CHAR fn[300];
            BYTE bl43 = (BYTE)(R_BX & 0xFF);
            WIN32_FILE_ATTRIBUTE_DATA ad;
            DosGuestPath(m, R_DS, R_DX, fn, sizeof(fn));
            if (bl43 == 0x00 || bl43 == 0x02 || bl43 == 0x04 || bl43 == 0x06 || bl43 == 0x08) {
                if (!GetFileAttributesExA(fn, GetFileExInfoStandard, &ad)) {
                    SETAX(DosLfnError(GetLastError())); ERRCF();
                } else if (bl43 == 0x00) {
                    /* CX = the attributes. ⚠ Masked to DOS's six bits as 4300h is (a file
                       with none set reads 0, not Win32's 80h NORMAL); unmeasured on stock. */
                    SET16(R_CX, (WORD)(ad.dwFileAttributes & 0x3F));
                    SETAX((WORD)(ad.dwFileAttributes & 0x3F));       /* stock: AX = CX too (p_lfn) */
                    OKCF();
                } else if (bl43 == 0x02) {
                    /* DX:AX = the size the file occupies (compressed). */
                    DWORD hi = 0, lo;
                    SetLastError(NO_ERROR);
                    lo = GetCompressedFileSizeA(fn, &hi);
                    if (lo == INVALID_FILE_SIZE && GetLastError() != NO_ERROR) {
                        SETAX(DosLfnError(GetLastError())); ERRCF();
                    } else { SETAX(lo & 0xFFFF); SET16(R_DX, (lo >> 16) & 0xFFFF); OKCF(); }
                } else {
                    /* 4: last write -> CX time, DI date. 6: last access -> DI date.
                       8: creation -> CX time, DI date, SI 10 ms units. All LOCAL, as
                       5700h's are. A time before 1980 reads as 0. */
                    const FILETIME *ft = (bl43 == 0x04) ? &ad.ftLastWriteTime
                                       : (bl43 == 0x06) ? &ad.ftLastAccessTime
                                                        : &ad.ftCreationTime;
                    WORD dd = 0, dt = 0; BYTE cs = 0;
                    if (!DosLfnFileTimeToDos(DosFileTimeZoned(ft, 1), &dd, &dt, &cs)) { dd = 0; dt = 0; cs = 0; }
                    SET16(R_DI, dd);
                    if (bl43 != 0x06) SET16(R_CX, dt);
                    if (bl43 == 0x08) SET16(R_SI, cs);
                    OKCF();
                }
            } else if (bl43 == 0x01) {          /* set attributes = CX, as 4301h */
                DWORD a = (DWORD)(R_CX & 0x3F);
                if (!a) a = FILE_ATTRIBUTE_NORMAL;
                if (SetFileAttributesA(fn, a)) OKCF();
                else { SETAX(DosLfnError(GetLastError())); ERRCF(); }
            } else if (bl43 == 0x03 || bl43 == 0x05 || bl43 == 0x07) {
                /* 3: last write = DI date, CX time. 5: last access = DI date (midnight).
                   7: creation = DI date, CX time, SI 10 ms units. Local in, UTC to Win32. */
                WORD dd = (WORD)(R_DI & 0xFFFF);
                WORD dt = (bl43 == 0x05) ? 0 : (WORD)(R_CX & 0xFFFF);
                BYTE  cs = (bl43 == 0x07) ? (BYTE)(R_SI & 0xFF) : 0;
                UINT64 v; FILETIME lf, ft; HANDLE hf;
                if (!DosLfnDosToFileTime(dd, dt, cs, &v)) { SETAX(0x0D); ERRCF(); }   /* invalid data */
                else {
                    lf.dwLowDateTime = (DWORD)v; lf.dwHighDateTime = (DWORD)(v >> 32);
                    hf = DosLfnOpenAttributes(fn);
                    if (hf == INVALID_HANDLE_VALUE) { SETAX(DosLfnError(GetLastError())); ERRCF(); }
                    else {
                        BOOL ok43 = LocalFileTimeToFileTime(&lf, &ft)
                                 && SetFileTime(hf, bl43 == 0x07 ? &ft : NULL,
                                                    bl43 == 0x05 ? &ft : NULL,
                                                    bl43 == 0x03 ? &ft : NULL);
                        DWORD we = ok43 ? 0 : GetLastError();
                        CloseHandle(hf);
                        if (ok43) OKCF(); else { SETAX(DosLfnError(we)); ERRCF(); }
                    }
                }
            } else { SETAX(1); ERRCF(); }       /* BL beyond 8: invalid function */
        } else if (al71 == 0x47) {              /* current directory, long: DL, DS:SI */
            /* AH=47h's drive rules (0 = default, a drive Win32 cannot stand on answers
               through its =X:), then the LONG form of the path -- 47h hands back the
               short upper-case CDS form (#164), this hands back what GetLongPathNameA
               makes of it, case as the directories were created. No drive letter, no
               leading backslash, ASCIIZ (RBIL: buffer of 261 bytes). */
            CHAR cwd[300], lp[300];
            DWORD n = 0;
            BYTE dl = (BYTE)(R_DX & 0xFF), cur = (BYTE)(DosCurrentDrive(m) + 1);
            if (dl == 0 || dl == cur) {
                if (m->VirtualDrive >= 0) {
                    CHAR spec[3]; spec[0] = (CHAR)('A' + m->VirtualDrive); spec[1] = ':'; spec[2] = 0;
                    n = GetFullPathNameA(spec, sizeof(cwd), cwd, NULL);
                } else n = GetCurrentDirectoryA(sizeof(cwd), cwd);
            } else if (dl <= 26 && (GetLogicalDrives() & (1u << (dl - 1)))) {
                CHAR spec[3]; spec[0] = (CHAR)('A' + dl - 1); spec[1] = ':'; spec[2] = 0;
                n = GetFullPathNameA(spec, sizeof(cwd), cwd, NULL);
            }
            if (n == 0 || n >= sizeof(cwd)) { SETAX(0x0F); ERRCF(); }
            else {
                volatile BYTE *dst = (volatile BYTE *)((R_DS << 4) + (R_SI & 0xFFFF));
                PCSTR q = cwd;
                INT k = 0;
                DWORD ln = GetLongPathNameA(cwd, lp, sizeof(lp));
                if (ln && ln < sizeof(lp)) q = lp;
                if (q[0] && q[1] == ':') q += 2;
                if (*q == '\\' || *q == '/') ++q;
                while (q[k] && k < 260) { dst[k] = (BYTE)q[k]; ++k; }
                dst[k] = 0;
                OKCF();
            }
        } else if (al71 == 0x4E || al71 == 0x4F) {   /* find first / next -> ES:DI */
            volatile BYTE *d = (volatile BYTE *)((R_ES << 4) + (R_DI & 0xFFFF));
            INT dosfmt = (R_SI & 0xFFFF) == 1;
            WIN32_FIND_DATAA fd;
            if (al71 == 0x4E) {
                /* DS:DX pattern, matched against long AND short names (Win32's rule, and
                   the LFN API's); CL allowed / CH required attributes; SI time format. */
                CHAR pat[300]; HANDLE hf; UINT slot;
                BYTE allow = (BYTE)(R_CX & 0xFF), need = (BYTE)((R_CX >> 8) & 0xFF);
                DosGuestPath(m, R_DS, R_DX, pat, sizeof(pat));
                hf = FindFirstFileA(pat, &fd);
                if (hf == INVALID_HANDLE_VALUE) { SETAX(DosLfnError(GetLastError())); ERRCF(); }
                else {
                    INT ok = 1;
                    while (!DosLfnAttributesOk(fd.dwFileAttributes, allow, need))
                        if (!FindNextFileA(hf, &fd)) { ok = 0; break; }
                    if (!ok) { FindClose(hf); SETAX(18); ERRCF(); }   /* nothing passed CL/CH */
                    else {
                        for (slot = 0; slot < DOS_LFN_FIND_SLOTS && g_DosLfnFinds[slot]; ++slot) {}
                        if (slot >= DOS_LFN_FIND_SLOTS) {            /* full: recycle, round robin */
                            slot = g_DosLfnNext++ % DOS_LFN_FIND_SLOTS;
                            FindClose(g_DosLfnFinds[slot]);
                            tp = zput(tp, "  INT21 AX=714E search table full -- recycled handle 0x");
                            tp = zhex(tp, slot + 1); tp = zput(tp, "\r\n");
                        }
                        g_DosLfnFinds[slot] = hf; g_DosLfnAllow[slot] = allow; g_DosLfnNeed[slot] = need;
                        DosLfnFindFill(d, &fd, dosfmt);
                        SETAX(slot + 1); SET16(R_CX, 0);              /* CX: no lossy names */
                        OKCF();
                    }
                }
                if (m->IsTraceAll) { tp = zput(tp, "  INT21 AX=714E ["); tp = zput(tp, pat);
                                    tp = zput(tp, (*pfl & 1) ? "] -> none\r\n" : "] -> found\r\n"); }
            } else {
                UINT slot = (UINT)(R_BX & 0xFFFF) - 1u;
                if (slot >= DOS_LFN_FIND_SLOTS || !g_DosLfnFinds[slot]) { SETAX(6); ERRCF(); }
                else {
                    INT ok = 0;
                    while (FindNextFileA(g_DosLfnFinds[slot], &fd))
                        if (DosLfnAttributesOk(fd.dwFileAttributes, g_DosLfnAllow[slot], g_DosLfnNeed[slot])) { ok = 1; break; }
                    /* "No more files" leaves the handle OPEN: the program closes it, 71A1h. */
                    if (!ok) { SETAX(18); ERRCF(); }
                    else { DosLfnFindFill(d, &fd, dosfmt); SET16(R_CX, 0); OKCF(); }
                }
            }
        } else if (al71 == 0xA1) {              /* find close: BX */
            UINT slot = (UINT)(R_BX & 0xFFFF) - 1u;
            if (slot >= DOS_LFN_FIND_SLOTS || !g_DosLfnFinds[slot]) { SETAX(6); ERRCF(); }
            else { FindClose(g_DosLfnFinds[slot]); g_DosLfnFinds[slot] = 0; OKCF(); }
        } else if (al71 == 0x60) {              /* truename: DS:SI -> ES:DI, CL form */
            /* CL=0 the full path (case kept -- 60h upper-cases, this does not: unmeasured),
               1 its SHORT form, 2 its LONG form. 1 and 2 ask the file system, so the
               path must exist; 0 does not (as 60h: "SUB\FILE.TXT" resolves anyway).
               CH (SUBST expansion) is not looked at: we create no SUBST of our own
               that a path would need unwrapping from. */
            CHAR in[300], full[300], out[300];
            BYTE cl = (BYTE)(R_CX & 0xFF);
            DWORD n;
            DosGuestPath(m, R_DS, R_SI, in, sizeof(in));
            n = GetFullPathNameA(in, sizeof(full), full, NULL);
            if (n == 0 || n >= sizeof(full)) { SETAX(3); ERRCF(); }
            else if (cl > 2) { SETAX(1); ERRCF(); }
            else {
                if (cl == 0) { lstrcpynA(out, full, sizeof(out)); n = 1; }
                else if (cl == 1) n = GetShortPathNameA(full, out, sizeof(out));
                else              n = GetLongPathNameA(full, out, sizeof(out));
                if (n == 0 || n >= sizeof(out)) { SETAX(DosLfnError(GetLastError())); ERRCF(); }
                else {
                    volatile BYTE *dd = (volatile BYTE *)((R_ES << 4) + (R_DI & 0xFFFF));
                    INT k = 0;
                    while (out[k] && k < 260) { dd[k] = (BYTE)out[k]; ++k; }
                    dd[k] = 0;
                    SETAX(0);                           /* stock: AX=0000 (p_lfn) */
                    OKCF();
                }
            }
        } else if (al71 == 0xA0) {              /* volume information: DS:DX root, ES:DI/CX */
            /* BX = flags as Win32 reports them, cut to the four RBIL names -- bit 0 case-
               sensitive searches, 1 case preserved, 2 Unicode on disk, 15 compressed --
               plus 4000h "supports the LFN functions", which is the bit that matters.
               CX = the longest component (255), DX = the longest path, MAX_PATH = 260.
               ES:DI gets the file-system name ("NTFS", "FAT") within CX bytes.
             ⚠ DX = 260 is RBIL's "usually"; stock may compute it. AX is left alone. */
            CHAR root[300], fs[64];
            DWORD maxc = 0, fl = 0;
            DosGuestString(R_DS, R_DX, root, sizeof(root));
            if (root[0] && root[1] == ':' && !root[2]) { root[2] = '\\'; root[3] = 0; }  /* Win32 wants "C:\" */
            fs[0] = 0;
            if (!GetVolumeInformationA(root[0] ? root : NULL, NULL, 0, NULL, &maxc, &fl, fs, sizeof(fs))) {
                SETAX(DosLfnError(GetLastError())); ERRCF();
            } else {
                volatile BYTE *dd = (volatile BYTE *)((R_ES << 4) + (R_DI & 0xFFFF));
                UINT cap = (UINT)(R_CX & 0xFFFF), k;
                for (k = 0; cap && k < cap - 1 && fs[k]; ++k) dd[k] = (BYTE)fs[k];
                if (cap) dd[k] = 0;
                SET16(R_BX, (WORD)((fl & 0x0007) | (fl & 0x8000) | 0x4000));
                SET16(R_CX, (WORD)(maxc ? maxc : 255));
                SET16(R_DX, 260);
                OKCF();
            }
            tp = zput(tp, "  INT21 AX=71A0 ["); tp = zput(tp, root);
            tp = zput(tp, "] fs="); tp = zput(tp, fs); tp = zput(tp, " flags=0x"); tp = zhex(tp, fl);
            tp = zput(tp, (*pfl & 1) ? " (err)\r\n" : "\r\n");
        } else if (al71 == 0xA6) {              /* file info by handle: BX -> DS:DX */
            /* BY_HANDLE_FILE_INFORMATION, 52 bytes, exactly as Win32 lays it out (times
               UTC, as a FILETIME is). */
            DWORD h = R_BX & 0xFFFF;
            BY_HANDLE_FILE_INFORMATION bi;
            if (!DosHandleIsFile((PVOID const *)m->FileHandles, h)) { SETAX(6); ERRCF(); }
            else if (!GetFileInformationByHandle(m->FileHandles[h], &bi)) { SETAX(DosLfnError(GetLastError())); ERRCF(); }
            else {
                volatile BYTE *dd = (volatile BYTE *)((R_DS << 4) + (R_DX & 0xFFFF));
                PCBYTE sb = (PCBYTE)&bi;
                UINT k;
                for (k = 0; k < sizeof(bi) && k < 52; ++k) dd[k] = sb[k];
                OKCF();
            }
        } else if (al71 == 0xA7) {              /* time conversion, BL */
            BYTE bla7 = (BYTE)(R_BX & 0xFF);
            /* ⚠ THE ZONE IS A CHOICE, NOT A MEASUREMENT: a FILETIME here is taken as UTC
                 (what 714Eh SI=0 and 71A6h hand out) and the DOS side as LOCAL (what every
                 DOS time this host reports is), so FILETIME -> DOS converts to local time
                 and back. Windows 95's IFSMgr converts the same way; whether NTVDM does is
                 p_lfn's lfn.71A7.ft2dos -- on a machine whose zone is not UTC the hour
                 differs by the offset if this is wrong. */
            if (bla7 == 0x00) {                 /* DS:SI -> QWORD FILETIME -> CX time, DX date, BH */
                const volatile BYTE *q = (const volatile BYTE *)((R_DS << 4) + (R_SI & 0xFFFF));
                FILETIME ft; WORD dd, dt; BYTE cs;
                ft.dwLowDateTime  = (DWORD)q[0] | ((DWORD)q[1] << 8) | ((DWORD)q[2] << 16) | ((DWORD)q[3] << 24);
                ft.dwHighDateTime = (DWORD)q[4] | ((DWORD)q[5] << 8) | ((DWORD)q[6] << 16) | ((DWORD)q[7] << 24);
                if (!DosLfnFileTimeToDos(DosFileTimeZoned(&ft, 1), &dd, &dt, &cs)) { SETAX(0x0D); ERRCF(); }
                else {
                    SET16(R_CX, dt); SET16(R_DX, dd);
                    /* ⚠ INTENDED DIVERGENCE (s92): for an exact even second stock answers
                         BH=C7h (199) -- p_lfn lfn.71A7.ft2dos, one measurement -- where the
                         spec's 10-ms remainder is 0. The spec outranks one oracle reading. */
                    SET16(R_BX, (WORD)((R_BX & 0xFF) | ((WORD)cs << 8)));
                    OKCF();
                }
            } else if (bla7 == 0x01) {          /* CX time, DX date, BH -> ES:DI QWORD */
                UINT64 v; FILETIME lf, ft;
                if (!DosLfnDosToFileTime((WORD)(R_DX & 0xFFFF), (WORD)(R_CX & 0xFFFF),
                                       (BYTE)((R_BX >> 8) & 0xFF), &v)) { SETAX(0x0D); ERRCF(); }
                else {
                    volatile BYTE *q = (volatile BYTE *)((R_ES << 4) + (R_DI & 0xFFFF));
                    INT k;
                    lf.dwLowDateTime = (DWORD)v; lf.dwHighDateTime = (DWORD)(v >> 32);
                    if (!LocalFileTimeToFileTime(&lf, &ft)) ft = lf;
                    for (k = 0; k < 4; ++k) {
                        q[k]     = (BYTE)(ft.dwLowDateTime  >> (8 * k));
                        q[4 + k] = (BYTE)(ft.dwHighDateTime >> (8 * k));
                    }
                    OKCF();
                }
            } else { SETAX(1); ERRCF(); }
        } else if (al71 == 0xA8) {              /* generate short name: DS:SI -> ES:DI, DH */
            /* DH=0: 11 bytes, FCB style; DH=1: "NAME.EXT" ASCIIZ. DL's character-set
               nibbles are not looked at -- see the code-page note at the helpers. */
            CHAR ln[300], s83[13], f11[11];
            volatile BYTE *dd = (volatile BYTE *)((R_ES << 4) + (R_DI & 0xFFFF));
            INT k;
            DosGuestString(R_DS, R_SI, ln, sizeof(ln));
            DosLfnShortName(ln, s83, f11);
            if (((R_DX >> 8) & 0xFF) == 0) for (k = 0; k < 11; ++k) dd[k] = (BYTE)f11[k];
            else { for (k = 0; s83[k]; ++k) dd[k] = (BYTE)s83[k]; dd[k] = 0; }
            OKCF();
        } else if (al71 == 0xAA) {              /* SUBST: BH 0 create / 1 terminate / 2 query */
            /* BL = drive (0 = default, 1 = A:). The host's own DOS-device table is what a
               SUBST is on NT (XP's SUBST.EXE is DefineDosDevice), so this is real: a drive
               created here is visible to the user's session until terminated or logoff.
             ⚠ TERMINATE ONLY UNDOES A SUBST -- a letter whose NT target is "\??\..." --
               never a real disk or a network mapping; anything else is 0Fh. */
            BYTE bh = (BYTE)((R_BX >> 8) & 0xFF), bl = (BYTE)(R_BX & 0xFF);
            CHAR spec[3], tgt[300];
            BYTE drv = (BYTE)(bl ? bl - 1 : DosCurrentDrive(m));
            spec[0] = (CHAR)('A' + (drv < 26 ? drv : 0)); spec[1] = ':'; spec[2] = 0;
            tgt[0] = 0;
            if (drv >= 26 || bh > 2) { SETAX(bh > 2 ? 1 : 0x0F); ERRCF(); }
            else if (bh == 0) {
                CHAR in[300], full[300];
                DWORD n;
                DosGuestPath(m, R_DS, R_DX, in, sizeof(in));
                n = GetFullPathNameA(in, sizeof(full), full, NULL);
                if (GetLogicalDrives() & (1u << drv)) { SETAX(0x0F); ERRCF(); }     /* letter in use */
                else if (n == 0 || n >= sizeof(full)) { SETAX(3); ERRCF(); }
                else if (DefineDosDeviceA(0, spec, full)) {
                    OKCF();
                    tp = zput(tp, "  INT21 AX=71AA SUBST "); tp = zput(tp, spec);
                    tp = zput(tp, " = "); tp = zput(tp, full); tp = zput(tp, " (a host drive)\r\n");
                } else { SETAX(DosLfnError(GetLastError())); ERRCF(); }
            } else {
                DWORD n = QueryDosDeviceA(spec, tgt, sizeof(tgt));
                INT is_subst = n > 4 && tgt[0] == '\\' && tgt[1] == '?' && tgt[2] == '?' && tgt[3] == '\\';
                if (!is_subst) { SETAX(bh == 2 ? 0x89 : 0x0F); ERRCF(); }   /* stock query: 89h (p_lfn) */
                else if (bh == 1) {
                    if (DefineDosDeviceA(DDD_REMOVE_DEFINITION, spec, NULL)) OKCF();
                    else { SETAX(DosLfnError(GetLastError())); ERRCF(); }
                } else {
                    volatile BYTE *dd = (volatile BYTE *)((R_DS << 4) + (R_DX & 0xFFFF));
                    INT k = 0;
                    while (tgt[4 + k] && k < 260) { dd[k] = (BYTE)tgt[4 + k]; ++k; }
                    dd[k] = 0;
                    OKCF();
                }
            }
        } else {
            tp = zput(tp, "  INT21 AX=71"); tp = zhexb(tp, al71);
            tp = zput(tp, " not an LFN function stock provides -> AX=0001 CF=1\r\n");
            /* s92, MEASURED (dospair p_lfn): stock answers an unknown 71xxh -- and 710Dh,
               which it does not provide -- with AX=0001 CF=1, "invalid function", NOT the
               7100h this arm assumed. */
            SETAX(0x0001);
            ERRCF();
        }
        if (m->IsTraceAll) {
            tp = zput(tp, "  INT21 AX=71"); tp = zhexb(tp, al71);
            tp = zput(tp, " -> AX=0x"); tp = zhex(tp, R_AX & 0xFFFF);
            tp = zput(tp, (*pfl & 1) ? " (err)\r\n" : "\r\n");
        }
    } else if (!DosIsDefinedBy622(ah)) {
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
        tp = zput(tp, "  INT21 AH=0x"); tp = zhex(tp, ah);
        tp = zput(tp, " undefined on 6.22 -- AL=0, CF clear (matches DOS)\r\n");
        m->Undefined[(ah & 0xFF) >> 3] |= (BYTE)(1u << (ah & 7));
        OKCF();
    } else {                                    /* unhandled service */
        /* GH #27. Recorded as well as logged, so the STAGE2 block can list every
           service a run actually wanted -- that list is the to-do list.
           Reaching HERE means 6.22 defines a real service at this AH and we have
           not written it yet.  CF=1 is right for that: a quiet "success" would
           tell the program its request worked when nothing happened.  Functions
           DOS does not define are handled above and stay silent, matching DOS. */
        tp = zput(tp, "  INT21 AH=0x"); tp = zhexb(tp, (UINT)ah);
        tp = zput(tp, " AL=0x"); tp = zhexb(tp, (UINT)(R_AX & 0xFF));
        tp = zput(tp, " UNIMPLEMENTED\r\n");
        m->Unimplemented[(ah & 0xFF) >> 3] |= (BYTE)(1u << (ah & 7));
        ERRCF();
    }

    /* GH #34: remember the last failure for AH=59h. Done HERE, once, rather
       than at each of the ~20 error sites -- CF and AX are already exactly what
       the guest is about to see. 59h itself is excluded so reading the error
       does not overwrite it. */
    if (ah != 0x59 && (*pfl & 1)) m->LastError = (WORD)(R_AX & 0xFFFF);

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
    if (lfn_alias && (lfn_alias == 0x3A || lfn_alias == 0x3B) && (*pfl & 1)
        && (R_AX & 0xFFFF) == 3) SETAX(2);
    if ((*pfl & 1) && m->CanRaiseCrit && !g_DosInt21IsProtectedMode && !m->IsCritActive
        && DosCritIsHardwareError((WORD)(R_AX & 0xFFFF))) {
        const volatile BYTE *pn = (const volatile BYTE *)((R_DS << 4) + (R_DX & 0xFFFF));
        INT pathcall = (ah == 0x3C || ah == 0x3D || ah == 0x4E || ah == 0x39 || ah == 0x3A
                        || ah == 0x3B || ah == 0x41 || ah == 0x43 || ah == 0x5A || ah == 0x5B);
        BYTE drv = DosCurrentDrive(m);
        if (pathcall && pn[1] == ':') drv = (BYTE)((pn[0] | 0x20) - 'a');
        if ((ah == 0x3F || ah == 0x40) && g_DosReadWriteDrive >= 0) drv = (BYTE)g_DosReadWriteDrive;
        m->IsCritPending = 1;
        m->CritAl = drv;
        m->CritAh = DosCritInt24Ah((BYTE)ah);
        m->CritCode = (BYTE)((R_AX & 0xFF) - 19);
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
    else if ((*pfl & 1) && (ah == 0x3F || ah == 0x40)
             && DosCritIsHardwareError((WORD)(R_AX & 0xFFFF))) {
        WORD code = (WORD)(R_AX & 0xFFFF);
        SETAX(DosCritFailAx((BYTE)ah, (BYTE)(code - 19)));
        m->LastError = DOS_ERR_FAIL_I24;
        tp = zput(tp, "  INT24 not raised (");
        tp = zput(tp, m->IsCritActive ? "inside the handler" : g_DosInt21IsProtectedMode ? "DPMI client"
                                     : "nested real-mode call");
        tp = zput(tp, "): error 0x"); tp = zhexb(tp, (UINT)code);
        tp = zput(tp, " answered as FAIL -> AX=0x"); tp = zhex(tp, R_AX & 0xFFFF);
        tp = zput(tp, ", 59h=53h\r\n");
    }

    /* ── s91: KEEP THE PSP's JFT TRUTHFUL (see jft_known). V86 only: in protected mode
         the flags are not on a V86 stack and a DPMI client's JFT is not ours to show. */
    if (!g_DosInt21IsProtectedMode && !(*pfl & 1)) {
        if (ah == 0x3C || ah == 0x3D || ah == 0x5A || ah == 0x5B || ah == 0x6C)
            DosJftPut(m, (UINT)(R_AX & 0xFFFF), DosSftValue(m, (UINT)(R_AX & 0xFFFF)));
        else if (ah == 0x45 || ah == 0x46) {
            UINT src = (UINT)(R_BX & 0xFFFF);
            UINT dst = (ah == 0x45) ? (UINT)(R_AX & 0xFFFF) : (UINT)(R_CX & 0xFFFF);
            DosJftPut(m, dst, DosSftValue(m, src));
        } else if (ah == 0x3E)
            DosJftPut(m, (UINT)(R_BX & 0xFFFF), 0xFF);
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
    if (m->IsTraceAll && ah != 0x0A && m->TraceCount <= DOS_TRACE_MAX) {
        tp = zput(tp, "     -> ax="); tp = zhexb(tp, (UINT)((R_AX >> 8) & 0xFF));
        tp = zhexb(tp, (UINT)(R_AX & 0xFF));
        tp = zput(tp, " cf="); tp = zhexb(tp, (UINT)(*pfl & 1));
        tp = zput(tp, "\r\n");
    }

    m->TraceCursor = tp;
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
    return cont;
}
