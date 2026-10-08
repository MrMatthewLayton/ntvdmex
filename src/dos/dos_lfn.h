/* dos_lfn.h -- INT 21h AH=71h, the Windows 95 long-filename API: the PURE half. (#210)
 *
 * Stock NTVDM on XP provides the LFN API to DOS programs, and XP's own DOS tools
 * (EDIT.COM first among them -- see the old AH=71h note in dos_int21.c) are written
 * against it. dos_int21.c owns the Win32 half; everything here is arithmetic and byte
 * layout, with no Windows calls (only Windows types, src/ntvdmex_types.h), so
 * tests/unit/lfn_test.c can pin it off-VM:
 *
 *   DosLfnFileTimeToDos / DosLfnDosToFileTime  71A7h, and every DOS-format time below
 *   DosLfnFindPack                             the 318-byte record 714Eh/714Fh fill
 *   DosLfnAttributesOk                         714Eh/7141h's CL (allowed) / CH (required)
 *   DosLfnShortName                            71A8h "generate short name"
 *   DosExtOpenDisposition / ...ActionTaken / ...Access
 *                                              6Ch AND 716Ch / 71A9h: action -> disposition
 *   DosLfnErrFromWin32                         the LFN calls' error mapping
 *
 * ⚠ THE SOURCES ARE RBIL (INT 21h AX=71xxh) AND THE WIN32 STRUCTURE LAYOUTS, NOT A
 *   MEASUREMENT. Every choice below that a stock run could contradict is marked
 *   UNMEASURED, and tests/probes/dos/p_lfn.asm asks stock exactly those questions
 *   (scripts/dospair.sh tests/probes/dos/p_lfn.com). Change a row here only with a
 *   CASE= line from that run beside it.
 */
#ifndef NTVDMEX_DOS_LFN_H
#define NTVDMEX_DOS_LFN_H

#include "../ntvdmex_types.h"
#include "dos_err.h"      /* DosErrFromWin32: the measured rows come first */

/* ── FILETIME <-> DOS DATE/TIME. ──────────────────────────────────────────────────
     A FILETIME counts 100 ns since 1601-01-01; a DOS date is (year-1980)<<9 | month<<5
     | day and a DOS time hour<<11 | minute<<5 | seconds/2. 71A7h adds BH, "10 ms
     intervals to add to the DOS time" -- 0..199, because the DOS time has 2-second
     resolution: the odd second is BH's hundreds.
   ► PURE FORMAT CONVERSION. Whether the caller's FILETIME is UTC or local is the
     handler's business (dos_int21.c converts around this); nothing here knows a zone.
   ⚠ Out of DOS's range (before 1980, after 2107) the conversion FAILS rather than
     clamps -- Win32's FileTimeToDosDateTime does the same. */
#define DOS_LFN_FILETIME_TICKS_PER_SECOND 10000000ull  /* 100 ns ticks per second         */
#define DOS_LFN_FILETIME_UNIX_DAYS        134774ll     /* days 1601-01-01 .. 1970-01-01   */
#define DOS_LFN_FILETIME_TICKS_PER_10MS   100000u      /* 100 ns ticks per 10 ms          */

/* The DOS date and time fields (above), and their range. */
#define DOS_LFN_DOS_EPOCH_YEAR      1980
#define DOS_LFN_DOS_LAST_YEAR       2107
#define DOS_LFN_DATE_YEAR_SHIFT     9
#define DOS_LFN_DATE_MONTH_SHIFT    5
#define DOS_LFN_DATE_MONTH_MASK     0x0F
#define DOS_LFN_DATE_DAY_MASK       0x1F
#define DOS_LFN_TIME_HOUR_SHIFT     11
#define DOS_LFN_TIME_MINUTE_SHIFT   5
#define DOS_LFN_TIME_MINUTE_MASK    0x3F
#define DOS_LFN_TIME_SECONDS_MASK   0x1F
#define DOS_LFN_TIME_SECONDS_UNIT   2u      /* the DOS time counts seconds/2             */
#define DOS_LFN_ODD_SECOND          1       /* ...so the odd second goes into BH         */
#define DOS_LFN_TEN_MS_PER_SECOND   100
#define DOS_LFN_LAST_HOUR           23
#define DOS_LFN_LAST_MINUTE         59
#define DOS_LFN_LAST_EVEN_SECOND    58
#define DOS_LFN_LAST_TEN_MS         199

/* The calendar. */
#define DOS_LFN_JANUARY             1
#define DOS_LFN_FEBRUARY            2
#define DOS_LFN_DECEMBER            12
#define DOS_LFN_MONTHS_PER_YEAR     12
#define DOS_LFN_FIRST_DAY           1
#define DOS_LFN_LEAP_FEBRUARY_DAYS  29
#define DOS_LFN_LEAP_YEAR_INTERVAL  4
#define DOS_LFN_YEARS_PER_CENTURY   100
#define DOS_LFN_YEARS_PER_ERA       400
#define DOS_LFN_DAYS_PER_YEAR       365
#define DOS_LFN_SECONDS_PER_DAY     86400u
#define DOS_LFN_SECONDS_PER_HOUR    3600u
#define DOS_LFN_SECONDS_PER_MINUTE  60u
#define DOS_LFN_MINUTES_PER_HOUR    60

/* The constants of the days_from_civil / civil_from_days arithmetic below, which counts
   each year from 1 March. */
#define DOS_LFN_ERA_FLOOR_ADJUST            399     /* YEARS_PER_ERA - 1                 */
#define DOS_LFN_DAYS_PER_ERA                146097
#define DOS_LFN_DAYS_PER_ERA_LESS_ONE       146096
#define DOS_LFN_DAYS_PER_CENTURY            36524
#define DOS_LFN_DAYS_PER_FOUR_YEARS_LESS_ONE 1460
#define DOS_LFN_MARCH_SHIFT                 3
#define DOS_LFN_MARCH_WRAP                  9
#define DOS_LFN_SHIFTED_JANUARY             10
#define DOS_LFN_MONTH_DAYS_NUMERATOR        153
#define DOS_LFN_MONTH_DAYS_ROUNDING         2
#define DOS_LFN_MONTH_DAYS_DENOMINATOR      5
#define DOS_LFN_CIVIL_EPOCH_DAYS            719468

/* Days since 1970-01-01 of a proleptic Gregorian date (H. Hinnant's days_from_civil). */
static inline INT64 DosLfnDaysFromCivil(_In_ INT64 year, _In_ UINT month, _In_ UINT day)
{
    INT64 era, yearOfEra, dayOfYear, dayOfEra;
    year -= month <= DOS_LFN_FEBRUARY;
    era = (year >= 0 ? year : year - DOS_LFN_ERA_FLOOR_ADJUST) / DOS_LFN_YEARS_PER_ERA;
    yearOfEra = year - era * DOS_LFN_YEARS_PER_ERA;
    dayOfYear = (DOS_LFN_MONTH_DAYS_NUMERATOR
                 * (month + (month > DOS_LFN_FEBRUARY ? -DOS_LFN_MARCH_SHIFT : DOS_LFN_MARCH_WRAP))
                 + DOS_LFN_MONTH_DAYS_ROUNDING) / DOS_LFN_MONTH_DAYS_DENOMINATOR + day - 1;
    dayOfEra = yearOfEra * DOS_LFN_DAYS_PER_YEAR + yearOfEra / DOS_LFN_LEAP_YEAR_INTERVAL
             - yearOfEra / DOS_LFN_YEARS_PER_CENTURY + dayOfYear;
    return era * DOS_LFN_DAYS_PER_ERA + dayOfEra - DOS_LFN_CIVIL_EPOCH_DAYS;
}

static inline VOID DosLfnCivilFromDays(_In_ INT64 days, _Out_ PINT64 year, _Out_ PUINT month,
                                       _Out_ PUINT day)
{
    INT64 era, dayOfEra, yearOfEra, dayOfYear, shiftedMonth;
    days += DOS_LFN_CIVIL_EPOCH_DAYS;
    era = (days >= 0 ? days : days - DOS_LFN_DAYS_PER_ERA_LESS_ONE) / DOS_LFN_DAYS_PER_ERA;
    dayOfEra = days - era * DOS_LFN_DAYS_PER_ERA;
    yearOfEra = (dayOfEra - dayOfEra / DOS_LFN_DAYS_PER_FOUR_YEARS_LESS_ONE
                 + dayOfEra / DOS_LFN_DAYS_PER_CENTURY
                 - dayOfEra / DOS_LFN_DAYS_PER_ERA_LESS_ONE) / DOS_LFN_DAYS_PER_YEAR;
    dayOfYear = dayOfEra - (DOS_LFN_DAYS_PER_YEAR * yearOfEra
                            + yearOfEra / DOS_LFN_LEAP_YEAR_INTERVAL
                            - yearOfEra / DOS_LFN_YEARS_PER_CENTURY);
    shiftedMonth = (DOS_LFN_MONTH_DAYS_DENOMINATOR * dayOfYear + DOS_LFN_MONTH_DAYS_ROUNDING)
                   / DOS_LFN_MONTH_DAYS_NUMERATOR;
    *day   = (UINT)(dayOfYear - (DOS_LFN_MONTH_DAYS_NUMERATOR * shiftedMonth
                                 + DOS_LFN_MONTH_DAYS_ROUNDING)
                                / DOS_LFN_MONTH_DAYS_DENOMINATOR + 1);
    *month = (UINT)(shiftedMonth < DOS_LFN_SHIFTED_JANUARY ? shiftedMonth + DOS_LFN_MARCH_SHIFT
                                                           : shiftedMonth - DOS_LFN_MARCH_WRAP);
    *year  = yearOfEra + era * DOS_LFN_YEARS_PER_ERA + (*month <= DOS_LFN_FEBRUARY);
}

static inline UINT DosLfnDaysInMonth(_In_ INT64 year, _In_ UINT month)
{
    static const BYTE daysInMonth[DOS_LFN_MONTHS_PER_YEAR] =
        { 31,28,31,30,31,30,31,31,30,31,30,31 };
    if (month == DOS_LFN_FEBRUARY
        && ((year % DOS_LFN_LEAP_YEAR_INTERVAL == 0 && year % DOS_LFN_YEARS_PER_CENTURY != 0)
            || year % DOS_LFN_YEARS_PER_ERA == 0)) return DOS_LFN_LEAP_FEBRUARY_DAYS;
    return (month >= DOS_LFN_JANUARY && month <= DOS_LFN_DECEMBER) ? daysInMonth[month - 1] : 0;
}

/* FILETIME -> DOS date, time and BH (10 ms units, 0..199). TRUE = ok, FALSE = out of
   range. */
static inline BOOL DosLfnFileTimeToDos(_In_ UINT64 fileTime, _Out_ PWORD dosDate,
                                       _Out_ PWORD dosTime, _Out_ PBYTE tenMs)
{
    UINT64 seconds = fileTime / DOS_LFN_FILETIME_TICKS_PER_SECOND,
           remainder = fileTime % DOS_LFN_FILETIME_TICKS_PER_SECOND;
    INT64  days = (INT64)(seconds / DOS_LFN_SECONDS_PER_DAY) - DOS_LFN_FILETIME_UNIX_DAYS, year;
    UINT   secondOfDay = (UINT)(seconds % DOS_LFN_SECONDS_PER_DAY), month, day, hour, minute,
           second;
    DosLfnCivilFromDays(days, &year, &month, &day);
    if (year < DOS_LFN_DOS_EPOCH_YEAR || year > DOS_LFN_DOS_LAST_YEAR) return FALSE;
    hour = secondOfDay / DOS_LFN_SECONDS_PER_HOUR;
    minute = (secondOfDay / DOS_LFN_SECONDS_PER_MINUTE) % DOS_LFN_MINUTES_PER_HOUR;
    second = secondOfDay % DOS_LFN_SECONDS_PER_MINUTE;
    *dosDate = (WORD)(((UINT)(year - DOS_LFN_DOS_EPOCH_YEAR) << DOS_LFN_DATE_YEAR_SHIFT)
                      | (month << DOS_LFN_DATE_MONTH_SHIFT) | day);
    *dosTime = (WORD)((hour << DOS_LFN_TIME_HOUR_SHIFT) | (minute << DOS_LFN_TIME_MINUTE_SHIFT)
                      | (second / DOS_LFN_TIME_SECONDS_UNIT));
    *tenMs   = (BYTE)((second & DOS_LFN_ODD_SECOND) * DOS_LFN_TEN_MS_PER_SECOND
                      + (UINT)(remainder / DOS_LFN_FILETIME_TICKS_PER_10MS));
    return TRUE;
}

/* DOS date, time, BH -> FILETIME. FALSE = a field DOS cannot hold (month 13, day 30 Feb,
   hour 24, minute 60, a seconds/2 of 30, BH of 200 or more).
   ⚠ UNMEASURED: whether stock refuses these or normalises them as Win32's
     DosDateTimeToFileTime partly does. */
static inline BOOL DosLfnDosToFileTime(_In_ WORD dosDate, _In_ WORD dosTime, _In_ BYTE tenMs,
                                       _Out_ PUINT64 fileTime)
{
    INT64 year   = DOS_LFN_DOS_EPOCH_YEAR + (dosDate >> DOS_LFN_DATE_YEAR_SHIFT);
    UINT  month  = (dosDate >> DOS_LFN_DATE_MONTH_SHIFT) & DOS_LFN_DATE_MONTH_MASK,
          day    = dosDate & DOS_LFN_DATE_DAY_MASK;
    UINT  hour   = dosTime >> DOS_LFN_TIME_HOUR_SHIFT,
          minute = (dosTime >> DOS_LFN_TIME_MINUTE_SHIFT) & DOS_LFN_TIME_MINUTE_MASK,
          second = (dosTime & DOS_LFN_TIME_SECONDS_MASK) * DOS_LFN_TIME_SECONDS_UNIT;
    INT64 days;
    if (month < DOS_LFN_JANUARY || month > DOS_LFN_DECEMBER || day < DOS_LFN_FIRST_DAY
        || day > DosLfnDaysInMonth(year, month)) return FALSE;
    if (hour > DOS_LFN_LAST_HOUR || minute > DOS_LFN_LAST_MINUTE
        || second > DOS_LFN_LAST_EVEN_SECOND || tenMs > DOS_LFN_LAST_TEN_MS) return FALSE;
    days = DosLfnDaysFromCivil(year, month, day) + DOS_LFN_FILETIME_UNIX_DAYS;
    *fileTime = ((UINT64)days * DOS_LFN_SECONDS_PER_DAY + hour * DOS_LFN_SECONDS_PER_HOUR
                 + minute * DOS_LFN_SECONDS_PER_MINUTE + second)
                * DOS_LFN_FILETIME_TICKS_PER_SECOND
        + (UINT64)tenMs * DOS_LFN_FILETIME_TICKS_PER_10MS;
    return TRUE;
}

/* ── THE FIND RECORD 714Eh/714Fh WRITE AT ES:DI. ─────────────────────────────────
     RBIL's "Windows95 long filename find record", which is WIN32_FIND_DATAA byte for
     byte -- 318 (13Eh) bytes:
       00h DWORD attributes      04h QWORD creation time   0Ch QWORD last access
       14h QWORD last write      1Ch DWORD size HIGH       20h DWORD size LOW
       24h 8 bytes reserved      2Ch 260 bytes long name   130h 14 bytes short name
     SI on the call picks the time format: 0 = 64-bit FILETIME, 1 = DOS date/time
     (RBIL: "date in high word, time in low word" of the first DWORD).
   ⚠ UNMEASURED, and the probe prints each: (1) the attribute DWORD is passed as Win32
     reports it (a plain NTFS file reads 20h ARCHIVE; a file with none set would read
     80h NORMAL); (2) in DOS format the QWORD's HIGH dword is written 0; (3) the short
     name is Win32's cAlternateFileName, i.e. EMPTY when the long name is already a
     valid 8.3 name; (4) everything past each name's NUL is zeroed, so the record is
     deterministic (Win32 leaves garbage there). */
#define DOS_LFN_FIND_RECORD_SIZE    0x13E
#define DOS_LFN_FIND_ATTRIBUTES     0x00
#define DOS_LFN_FIND_CREATION_TIME  0x04
#define DOS_LFN_FIND_ACCESS_TIME    0x0C
#define DOS_LFN_FIND_WRITE_TIME     0x14
#define DOS_LFN_FIND_SIZE_HIGH      0x1C
#define DOS_LFN_FIND_SIZE_LOW       0x20
#define DOS_LFN_FIND_LONG_NAME      0x2C
#define DOS_LFN_FIND_SHORT_NAME     0x130
#define DOS_LFN_FIND_LONG_NAME_CHARS  259   /* the 260-byte field, less its NUL          */
#define DOS_LFN_FIND_SHORT_NAME_CHARS 13    /* the 14-byte field, less its NUL           */

/* A QWORD time: its high DWORD 4 bytes in; in DOS format, the date in the high word. */
#define DOS_LFN_FIND_TIME_HIGH      4
#define DOS_LFN_DOS_DATE_SHIFT      16

/* A little-endian DWORD: the low byte first, then the next at +1, and so on. */
#define DOS_LFN_BYTE1               1
#define DOS_LFN_BYTE2               2
#define DOS_LFN_BYTE3               3
#define DOS_LFN_BYTE1_SHIFT         8
#define DOS_LFN_BYTE2_SHIFT         16
#define DOS_LFN_BYTE3_SHIFT         24

typedef struct _DOS_LFN_FIND_ENTRY {
    DWORD  Attributes;
    UINT64 CreationTime, LastAccessTime, LastWriteTime;  /* FILETIMEs, in whatever zone the
                                                            caller wants                    */
    DWORD  SizeHigh, SizeLow;
    PCSTR  LongName;
    PCSTR  ShortName;            /* "" when the long name is already 8.3         */
} DOS_LFN_FIND_ENTRY, *PDOS_LFN_FIND_ENTRY;

typedef const DOS_LFN_FIND_ENTRY *PCDOS_LFN_FIND_ENTRY;

static inline VOID DosLfnPutDword(_Out_writes_bytes_(sizeof(DWORD)) PBYTE destination,
                                  _In_ DWORD value)
{
    destination[0] = (BYTE)value; destination[DOS_LFN_BYTE1] = (BYTE)(value >> DOS_LFN_BYTE1_SHIFT);
    destination[DOS_LFN_BYTE2] = (BYTE)(value >> DOS_LFN_BYTE2_SHIFT);
    destination[DOS_LFN_BYTE3] = (BYTE)(value >> DOS_LFN_BYTE3_SHIFT);
}

static inline VOID DosLfnPutTime(_Out_writes_bytes_(sizeof(UINT64)) PBYTE destination,
                                 _In_ UINT64 fileTime, _In_ BOOL isDosFormat)
{
    if (isDosFormat) {
        WORD dosDate = 0, dosTime = 0; BYTE tenMs = 0;
        if (!fileTime || !DosLfnFileTimeToDos(fileTime, &dosDate, &dosTime, &tenMs)) {
            dosDate = 0; dosTime = 0;
        }
        DosLfnPutDword(destination, ((DWORD)dosDate << DOS_LFN_DOS_DATE_SHIFT) | dosTime);
        DosLfnPutDword(destination + DOS_LFN_FIND_TIME_HIGH, 0);
    } else {
        DosLfnPutDword(destination, (DWORD)fileTime);
        DosLfnPutDword(destination + DOS_LFN_FIND_TIME_HIGH,
                       (DWORD)(fileTime >> DWORD_SHIFT));
    }
}

static inline VOID DosLfnFindPack(_Out_writes_bytes_(DOS_LFN_FIND_RECORD_SIZE)
                                  BYTE record[DOS_LFN_FIND_RECORD_SIZE],
                                  _In_ PCDOS_LFN_FIND_ENTRY entry, _In_ BOOL isDosFormat)
{
    INT byteIndex;
    for (byteIndex = 0; byteIndex < DOS_LFN_FIND_RECORD_SIZE; ++byteIndex) record[byteIndex] = 0;
    DosLfnPutDword(record + DOS_LFN_FIND_ATTRIBUTES, entry->Attributes);
    DosLfnPutTime(record + DOS_LFN_FIND_CREATION_TIME, entry->CreationTime, isDosFormat);
    DosLfnPutTime(record + DOS_LFN_FIND_ACCESS_TIME, entry->LastAccessTime, isDosFormat);
    DosLfnPutTime(record + DOS_LFN_FIND_WRITE_TIME, entry->LastWriteTime, isDosFormat);
    DosLfnPutDword(record + DOS_LFN_FIND_SIZE_HIGH, entry->SizeHigh);
    DosLfnPutDword(record + DOS_LFN_FIND_SIZE_LOW, entry->SizeLow);
    for (byteIndex = 0;
         byteIndex < DOS_LFN_FIND_LONG_NAME_CHARS && entry->LongName && entry->LongName[byteIndex];
         ++byteIndex)
        record[DOS_LFN_FIND_LONG_NAME + byteIndex] = (BYTE)entry->LongName[byteIndex];
    for (byteIndex = 0;
         byteIndex < DOS_LFN_FIND_SHORT_NAME_CHARS && entry->ShortName
             && entry->ShortName[byteIndex];
         ++byteIndex)
        record[DOS_LFN_FIND_SHORT_NAME + byteIndex] = (BYTE)entry->ShortName[byteIndex];
}

/* ── 714Eh / 7141h ATTRIBUTE MASKS. CL = ALLOWED, CH = REQUIRED (RBIL). ──────────────
     The allowed half is DOS's own find rule -- a hidden, system or directory entry
     appears only when CL asks for it, ordinary files always (dta_match in dos_int21.c
     is the 4Eh twin) -- and every bit in CH must be present. Read-only and archive
     never exclude (RBIL: "bits 0 and 5 ignored" in CL). */
#define DOS_LFN_ATTRIBUTE_HIDDEN     0x02
#define DOS_LFN_ATTRIBUTE_SYSTEM     0x04
#define DOS_LFN_ATTRIBUTE_DIRECTORY  0x10
#define DOS_LFN_ATTRIBUTE_MASK       0x3F

static inline BOOL DosLfnAttributesOk(_In_ DWORD attributes, _In_ BYTE allowed, _In_ BYTE required)
{
    if ((attributes & DOS_LFN_ATTRIBUTE_DIRECTORY) && !(allowed & DOS_LFN_ATTRIBUTE_DIRECTORY))
        return FALSE;
    if ((attributes & DOS_LFN_ATTRIBUTE_HIDDEN) && !(allowed & DOS_LFN_ATTRIBUTE_HIDDEN))
        return FALSE;
    if ((attributes & DOS_LFN_ATTRIBUTE_SYSTEM) && !(allowed & DOS_LFN_ATTRIBUTE_SYSTEM))
        return FALSE;
    return (attributes & (required & DOS_LFN_ATTRIBUTE_MASK))
           == (DWORD)(required & DOS_LFN_ATTRIBUTE_MASK);
}

/* ── 71A8h: A SHORT NAME FOR A LONG ONE. ─────────────────────────────────────────
     Output both forms: `shortName` "NAME.EXT" ASCIIZ (DH=1) and `fcbName` 11 bytes
     space-padded with no dot (DH=0).
     A name that is ALREADY a legal 8.3 name comes back as itself, upper-cased. Anything
     else gets NT's generated form (RtlGenerate8dot3Name, the first alias NTFS hands
     out): blanks and every '.' but the last dropped from the base, the characters
     DOS forbids in a short name ( + , ; = [ ] ) turned into '_', upper-cased, the
     first SIX kept, then "~1"; the extension is the first three characters after the
     LAST dot, cleaned the same way. "A long file name.txt" -> "ALONGF~1.TXT".
   ⚠ UNMEASURED AGAINST STOCK, and the first thing p_lfn asks. Windows 95's 71A8h is
     documented (RBIL) as generating the BASIS name; whether it -- or NTVDM -- adds the
     numeric tail is exactly the question. The "~1" form is what NT's own generator
     produces and what the file system would assign a lone file of that name. */
#define DOS_LFN_FIRST_PRINTABLE     0x20    /* below this, a control character           */
#define DOS_LFN_CASE_OFFSET         32      /* 'a' - 'A'                                 */
#define DOS_LFN_BASE_CHARS          8       /* an 8.3 name: 8 of base ...                */
#define DOS_LFN_EXTENSION_CHARS     3       /* ... and 3 of extension                    */
#define DOS_LFN_BASE_BUFFER_SIZE    9
#define DOS_LFN_EXTENSION_BUFFER_SIZE 4
#define DOS_LFN_NO_EXTENSION        (-1)    /* no dot seen yet                           */
#define DOS_LFN_ALIAS_BASE_CHARS    6       /* the generated form keeps the first SIX... */
#define DOS_LFN_ALIAS_TILDE         '~'     /* ...then "~1"                              */
#define DOS_LFN_ALIAS_TAIL          '1'
#define DOS_LFN_REPLACEMENT_CHAR    '_'
#define DOS_LFN_EXTENSION_DOT       '.'
#define DOS_LFN_FCB_PAD             ' '
#define DOS_LFN_SHORT_NAME_SIZE     13      /* "NAME.EXT" and its NUL                    */
#define DOS_LFN_FCB_NAME_SIZE       11

static inline BOOL DosLfnIsBadShortNameChar(_In_ BYTE character)
{
    return character < DOS_LFN_FIRST_PRINTABLE || character == '"' || character == '*'
        || character == '+' || character == ',' || character == '/' || character == ':'
        || character == ';' || character == '<' || character == '=' || character == '>'
        || character == '?' || character == '[' || character == '\\' || character == ']'
        || character == '|';
}

static inline CHAR DosLfnUpperCase(_In_ CHAR character)
{
    return (character >= 'a' && character <= 'z') ? (CHAR)(character - DOS_LFN_CASE_OFFSET)
                                                   : character;
}

/* Is `name` (one path component) already a legal 8.3 name? */
static inline BOOL DosLfnIsShortName(_In_ PCSTR name)
{
    INT baseLength = 0, extensionLength = DOS_LFN_NO_EXTENSION, charIndex;
    if (!name[0] || name[0] == DOS_LFN_EXTENSION_DOT) return FALSE;
    for (charIndex = 0; name[charIndex]; ++charIndex) {
        BYTE character = (BYTE)name[charIndex];
        if (character == DOS_LFN_EXTENSION_DOT) {
            if (extensionLength >= 0) return FALSE;
            extensionLength = 0; continue;
        }
        if (character == ' ' || DosLfnIsBadShortNameChar(character)) return FALSE;
        if (extensionLength >= 0) { if (++extensionLength > DOS_LFN_EXTENSION_CHARS) return FALSE; }
        else if (++baseLength > DOS_LFN_BASE_CHARS) return FALSE;
    }
    return baseLength > 0 && extensionLength != 0;   /* "NAME." (a dot and no extension) is not */
}

static inline CHAR DosLfnCleanShortNameChar(_In_ BYTE character)
{
    if (character == '+' || character == ',' || character == ';' || character == '='
        || character == '[' || character == ']') return DOS_LFN_REPLACEMENT_CHAR;
    return DosLfnUpperCase((CHAR)character);
}

static inline VOID DosLfnShortName(_In_ PCSTR longName,
                                   _Out_writes_(DOS_LFN_SHORT_NAME_SIZE)
                                   CHAR shortName[DOS_LFN_SHORT_NAME_SIZE],
                                   _Out_writes_(DOS_LFN_FCB_NAME_SIZE)
                                   CHAR fcbName[DOS_LFN_FCB_NAME_SIZE])
{
    PCSTR component = longName, cursor, lastDot = 0;
    CHAR base[DOS_LFN_BASE_BUFFER_SIZE], extension[DOS_LFN_EXTENSION_BUFFER_SIZE];
    INT baseLength = 0, extensionLength = 0, charIndex;
    for (cursor = longName; *cursor; ++cursor)                         /* last component */
        if (*cursor == '\\' || *cursor == '/' || *cursor == ':') component = cursor + 1;
    if (DosLfnIsShortName(component)) {
        for (cursor = component; *cursor && *cursor != DOS_LFN_EXTENSION_DOT; ++cursor)
            if (baseLength < DOS_LFN_BASE_CHARS) base[baseLength++] = DosLfnUpperCase(*cursor);
        if (*cursor == DOS_LFN_EXTENSION_DOT)
            for (++cursor; *cursor && extensionLength < DOS_LFN_EXTENSION_CHARS; ++cursor)
                extension[extensionLength++] = DosLfnUpperCase(*cursor);
    } else {
        for (cursor = component; *cursor; ++cursor)
            if (*cursor == DOS_LFN_EXTENSION_DOT && cursor != component) lastDot = cursor;
        for (cursor = component;
             *cursor && cursor != lastDot && baseLength < DOS_LFN_ALIAS_BASE_CHARS; ++cursor) {
            BYTE character = (BYTE)*cursor;
            if (character == ' ' || character == DOS_LFN_EXTENSION_DOT
                || character < DOS_LFN_FIRST_PRINTABLE || character == '"' || character == '*'
                || character == '/' || character == ':' || character == '<' || character == '>'
                || character == '?' || character == '\\' || character == '|') continue;
            base[baseLength++] = DosLfnCleanShortNameChar(character);
        }
        if (!baseLength) base[baseLength++] = DOS_LFN_REPLACEMENT_CHAR;
        base[baseLength++] = DOS_LFN_ALIAS_TILDE; base[baseLength++] = DOS_LFN_ALIAS_TAIL;
        if (lastDot)
            for (cursor = lastDot + 1;
                 *cursor && extensionLength < DOS_LFN_EXTENSION_CHARS; ++cursor) {
                BYTE character = (BYTE)*cursor;
                if (character == ' ' || character == DOS_LFN_EXTENSION_DOT
                    || character < DOS_LFN_FIRST_PRINTABLE || character == '"' || character == '*'
                    || character == '/' || character == ':' || character == '<'
                    || character == '>' || character == '?' || character == '\\'
                    || character == '|') continue;
                extension[extensionLength++] = DosLfnCleanShortNameChar(character);
            }
    }
    for (charIndex = 0; charIndex < DOS_LFN_FCB_NAME_SIZE; ++charIndex)
        fcbName[charIndex] = DOS_LFN_FCB_PAD;
    for (charIndex = 0; charIndex < baseLength; ++charIndex) fcbName[charIndex] = base[charIndex];
    for (charIndex = 0; charIndex < extensionLength; ++charIndex)
        fcbName[DOS_LFN_BASE_CHARS + charIndex] = extension[charIndex];
    for (charIndex = 0; charIndex < baseLength; ++charIndex) shortName[charIndex] = base[charIndex];
    if (extensionLength) {
        shortName[baseLength] = DOS_LFN_EXTENSION_DOT;
        for (charIndex = 0; charIndex < extensionLength; ++charIndex)
            shortName[baseLength + 1 + charIndex] = extension[charIndex];
        shortName[baseLength + 1 + extensionLength] = 0;
    }
    else shortName[baseLength] = 0;
}

/* ── 6Ch / 716Ch / 71A9h: THE ACTION WORD. ─────────────────────────────────────────
     DX bits 0-3 = what to do if the file EXISTS (0 fail, 1 open, 2 truncate), bits
     4-7 if it does NOT (0 fail, 1 create). Returned as Win32's CreateFile disposition
     NUMBER (windows.h-free; the values are Win32's own):
       CREATE_NEW 1   CREATE_ALWAYS 2   OPEN_EXISTING 3   OPEN_ALWAYS 4
       TRUNCATE_EXISTING 5
   ⚠ An action DOS has no meaning for (0x00, 0x13, ...) falls to OPEN_EXISTING -- what
     AH=6Ch has always answered here; unmeasured, and kept so the refactor that moved
     this out of dos_int21.c changes no behaviour. */
#define DOS_EXT_OPEN_CREATE_NEW         1u
#define DOS_EXT_OPEN_CREATE_ALWAYS      2u
#define DOS_EXT_OPEN_OPEN_EXISTING      3u
#define DOS_EXT_OPEN_OPEN_ALWAYS        4u
#define DOS_EXT_OPEN_TRUNCATE_EXISTING  5u

/* The action word's two halves (above). */
#define DOS_EXT_OPEN_ACTION_MASK        0x0F
#define DOS_EXT_OPEN_IF_MISSING_SHIFT   4
#define DOS_EXT_OPEN_IF_EXISTS_FAIL     0
#define DOS_EXT_OPEN_IF_EXISTS_OPEN     1
#define DOS_EXT_OPEN_IF_EXISTS_TRUNCATE 2
#define DOS_EXT_OPEN_IF_MISSING_FAIL    0
#define DOS_EXT_OPEN_IF_MISSING_CREATE  1

static inline UINT DosExtOpenDisposition(_In_ UINT action)
{
    UINT ifExists = action & DOS_EXT_OPEN_ACTION_MASK,
         ifMissing = (action >> DOS_EXT_OPEN_IF_MISSING_SHIFT) & DOS_EXT_OPEN_ACTION_MASK;
    if (ifExists == DOS_EXT_OPEN_IF_EXISTS_TRUNCATE && ifMissing == DOS_EXT_OPEN_IF_MISSING_CREATE)
        return DOS_EXT_OPEN_CREATE_ALWAYS;
    if (ifExists == DOS_EXT_OPEN_IF_EXISTS_OPEN && ifMissing == DOS_EXT_OPEN_IF_MISSING_CREATE)
        return DOS_EXT_OPEN_OPEN_ALWAYS;
    if (ifExists == DOS_EXT_OPEN_IF_EXISTS_FAIL && ifMissing == DOS_EXT_OPEN_IF_MISSING_CREATE)
        return DOS_EXT_OPEN_CREATE_NEW;
    if (ifExists == DOS_EXT_OPEN_IF_EXISTS_TRUNCATE && ifMissing == DOS_EXT_OPEN_IF_MISSING_FAIL)
        return DOS_EXT_OPEN_TRUNCATE_EXISTING;
    return DOS_EXT_OPEN_OPEN_EXISTING;
}

/* CX on success: 1 opened, 2 created, 3 replaced (truncated). `didExist` = the file was
   there before the call (Win32: GetLastError() == ERROR_ALREADY_EXISTS after an
   OPEN_ALWAYS / CREATE_ALWAYS that succeeded).
   ► #210: CREATE_ALWAYS ON A FILE THAT WAS NOT THERE IS "CREATED" (2), not "replaced".
     This answered 3 for every CREATE_ALWAYS -- RBIL's own table says otherwise, and
     716Ch "create or truncate" of a new long name is the probe's very first create. */
#define DOS_EXT_OPEN_TAKEN_OPENED       1
#define DOS_EXT_OPEN_TAKEN_CREATED      2
#define DOS_EXT_OPEN_TAKEN_REPLACED     3

static inline UINT DosExtOpenActionTaken(_In_ UINT disposition, _In_ BOOL didExist,
                                         _In_ BOOL isLongNameCall)
{
    switch (disposition) {
    case DOS_EXT_OPEN_CREATE_NEW:        return DOS_EXT_OPEN_TAKEN_CREATED;
    case DOS_EXT_OPEN_TRUNCATE_EXISTING: return DOS_EXT_OPEN_TAKEN_REPLACED;
    /* s92, MEASURED (dospair p_lfn): stock's AH=6Ch says 3 ("replaced") for action 12h
       whether the file existed or not (lfn.6C.12.new/.exists) -- but its 716Ch says 2
       ("created") for a name that was not there (lfn.716C.create). Two arms, two
       answers; the LFN one is RBIL's. */
    case DOS_EXT_OPEN_CREATE_ALWAYS:
        return (isLongNameCall && !didExist) ? DOS_EXT_OPEN_TAKEN_CREATED
                                             : DOS_EXT_OPEN_TAKEN_REPLACED;
    case DOS_EXT_OPEN_OPEN_ALWAYS:
        return didExist ? DOS_EXT_OPEN_TAKEN_OPENED : DOS_EXT_OPEN_TAKEN_CREATED;
    default:                             return DOS_EXT_OPEN_TAKEN_OPENED;
    }
}

/* BX bits 0-2: 0 read, 1 write, 2 read/write -> GENERIC_READ 80000000h / GENERIC_WRITE
   40000000h. Only bits 0-1 are looked at, as AH=6Ch always has (mode 3, "no access",
   is not a mode DOS defines and falls to read, exactly as AH=6Ch always has). */
#define DOS_EXT_OPEN_ACCESS_MASK        3
#define DOS_EXT_OPEN_ACCESS_WRITE       1
#define DOS_EXT_OPEN_ACCESS_READ_WRITE  2
#define DOS_EXT_OPEN_GENERIC_READ       0x80000000ul
#define DOS_EXT_OPEN_GENERIC_WRITE      0x40000000ul
#define DOS_EXT_OPEN_GENERIC_READ_WRITE 0xC0000000ul

static inline DWORD DosExtOpenAccess(_In_ UINT mode)
{
    mode &= DOS_EXT_OPEN_ACCESS_MASK;
    return (mode == DOS_EXT_OPEN_ACCESS_WRITE) ? DOS_EXT_OPEN_GENERIC_WRITE
         : (mode == DOS_EXT_OPEN_ACCESS_READ_WRITE) ? DOS_EXT_OPEN_GENERIC_READ_WRITE
         : DOS_EXT_OPEN_GENERIC_READ;
}

/* ── WHAT AN LFN CALL ANSWERS FOR A WIN32 FAILURE. ─────────────────────────────────
     dos_err.h's measured table first (2, 3, 5, 4, 80, 19-31). Then the identities the
     LFN calls meet that the 6.22 calls never did -- each is the SAME number on both
     sides, labelled as such, NOT provoked on stock:
       6  invalid handle (71A1h/714Fh/71A6h on a dead handle)
       17 not same device (7156h across drives)
       18 no more files (714Eh/714Fh)
     and two translations, both UNMEASURED:
       123 ERROR_INVALID_NAME -> 3 (RBIL 7160h: "03h malformed path")
       145 ERROR_DIR_NOT_EMPTY -> 5 (what 3Ah answers for a full directory)
     Anything else: 2, and the caller logs the Win32 code. */
#define DOS_LFN_WIN32_INVALID_HANDLE    6
#define DOS_LFN_WIN32_NOT_SAME_DEVICE   17
#define DOS_LFN_WIN32_NO_MORE_FILES     18
#define DOS_LFN_WIN32_INVALID_NAME      123
#define DOS_LFN_WIN32_DIR_NOT_EMPTY     145

static inline BOOL DosLfnErrFromWin32(_In_ DWORD win32Error, _Out_ PWORD dosError)
{
    switch (win32Error) {
    case DOS_LFN_WIN32_INVALID_HANDLE: case DOS_LFN_WIN32_NOT_SAME_DEVICE:
    case DOS_LFN_WIN32_NO_MORE_FILES:  *dosError = (WORD)win32Error; return TRUE;
    case DOS_LFN_WIN32_INVALID_NAME:   *dosError = DOS_ERR_PATH_NOT_FOUND; return TRUE;
    case DOS_LFN_WIN32_DIR_NOT_EMPTY:  *dosError = DOS_ERR_ACCESS_DENIED; return TRUE;
    default:  return DosErrFromWin32(win32Error, dosError);
    }
}

#endif /* NTVDMEX_DOS_LFN_H */
