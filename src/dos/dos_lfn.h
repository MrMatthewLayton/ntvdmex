/* dos_lfn.h -- INT 21h AH=71h, the Windows 95 long-filename API: the PURE half. (#210)
 *
 * Stock NTVDM on XP provides the LFN API to DOS programs, and XP's own DOS tools
 * (EDIT.COM first among them -- see the old AH=71h note in dos_int21.c) are written
 * against it. dos_int21.c owns the Win32 half; everything here is arithmetic and byte
 * layout, kept free of windows.h so tools/dostest/lfn_test.c can pin it off-VM:
 *
 *   dos_lfn_ft_to_dos / dos_lfn_dos_to_ft   71A7h, and every DOS-format time below
 *   dos_lfn_find_pack                       the 318-byte record 714Eh/714Fh fill
 *   dos_lfn_attr_ok                         714Eh/7141h's CL (allowed) / CH (required)
 *   dos_lfn_short_name                      71A8h "generate short name"
 *   dos_ext_open_disp / _taken / _access    6Ch AND 716Ch / 71A9h: action -> disposition
 *   dos_lfn_err_from_win32                  the LFN calls' error mapping
 *
 * ⚠ THE SOURCES ARE RBIL (INT 21h AX=71xxh) AND THE WIN32 STRUCTURE LAYOUTS, NOT A
 *   MEASUREMENT. Every choice below that a stock run could contradict is marked
 *   UNMEASURED, and tools/dostest/p_lfn.asm asks stock exactly those questions
 *   (scripts/dospair.sh tools/dostest/p_lfn.com). Change a row here only with a
 *   CASE= line from that run beside it.
 */
#ifndef DOS_LFN_H
#define DOS_LFN_H

#include <stdint.h>
#include "dos_err.h"      /* dos_err_from_win32: the measured rows come first */

/* ── FILETIME <-> DOS DATE/TIME. ──────────────────────────────────────────────────
     A FILETIME counts 100 ns since 1601-01-01; a DOS date is (year-1980)<<9 | month<<5
     | day and a DOS time hour<<11 | minute<<5 | seconds/2. 71A7h adds BH, "10 ms
     intervals to add to the DOS time" -- 0..199, because the DOS time has 2-second
     resolution: the odd second is BH's hundreds.
   ► PURE FORMAT CONVERSION. Whether the caller's FILETIME is UTC or local is the
     handler's business (dos_int21.c converts around this); nothing here knows a zone.
   ⚠ Out of DOS's range (before 1980, after 2107) the conversion FAILS rather than
     clamps -- Win32's FileTimeToDosDateTime does the same. */
#define DOS_LFN_FT_SEC        10000000ull      /* 100 ns ticks per second           */
#define DOS_LFN_FT_UNIX_DAYS  134774ll         /* days 1601-01-01 .. 1970-01-01     */

/* Days since 1970-01-01 of a proleptic Gregorian date (H. Hinnant's days_from_civil). */
static inline int64_t dos_lfn_days_from_civil(int64_t y, unsigned m, unsigned d)
{
    int64_t era, yoe, doy, doe;
    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static inline void dos_lfn_civil_from_days(int64_t z, int64_t *y, unsigned *m, unsigned *d)
{
    int64_t era, doe, yoe, doy, mp;
    z += 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = z - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp  = (5 * doy + 2) / 153;
    *d  = (unsigned)(doy - (153 * mp + 2) / 5 + 1);
    *m  = (unsigned)(mp < 10 ? mp + 3 : mp - 9);
    *y  = yoe + era * 400 + (*m <= 2);
}

static inline unsigned dos_lfn_days_in_month(int64_t y, unsigned m)
{
    static const unsigned char dim[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
    return (m >= 1 && m <= 12) ? dim[m - 1] : 0;
}

/* FILETIME -> DOS date, time and BH (10 ms units, 0..199). 1 = ok, 0 = out of range. */
static inline int dos_lfn_ft_to_dos(uint64_t ft, uint16_t *date, uint16_t *time, uint8_t *cs200)
{
    uint64_t secs = ft / DOS_LFN_FT_SEC, rem = ft % DOS_LFN_FT_SEC;
    int64_t  days = (int64_t)(secs / 86400u) - DOS_LFN_FT_UNIX_DAYS, y;
    unsigned sod  = (unsigned)(secs % 86400u), m, d, h, mi, s;
    dos_lfn_civil_from_days(days, &y, &m, &d);
    if (y < 1980 || y > 2107) return 0;
    h = sod / 3600; mi = (sod / 60) % 60; s = sod % 60;
    *date  = (uint16_t)(((unsigned)(y - 1980) << 9) | (m << 5) | d);
    *time  = (uint16_t)((h << 11) | (mi << 5) | (s / 2));
    *cs200 = (uint8_t)((s & 1) * 100 + (unsigned)(rem / 100000u));
    return 1;
}

/* DOS date, time, BH -> FILETIME. 0 = a field DOS cannot hold (month 13, day 30 Feb,
   hour 24, minute 60, a seconds/2 of 30, BH of 200 or more).
   ⚠ UNMEASURED: whether stock refuses these or normalises them as Win32's
     DosDateTimeToFileTime partly does. */
static inline int dos_lfn_dos_to_ft(uint16_t date, uint16_t time, uint8_t cs200, uint64_t *ft)
{
    int64_t  y  = 1980 + (date >> 9);
    unsigned m  = (date >> 5) & 0x0F, d = date & 0x1F;
    unsigned h  = time >> 11, mi = (time >> 5) & 0x3F, s = (time & 0x1F) * 2u;
    int64_t  days;
    if (m < 1 || m > 12 || d < 1 || d > dos_lfn_days_in_month(y, m)) return 0;
    if (h > 23 || mi > 59 || s > 58 || cs200 > 199) return 0;
    days = dos_lfn_days_from_civil(y, m, d) + DOS_LFN_FT_UNIX_DAYS;
    *ft = ((uint64_t)days * 86400u + h * 3600u + mi * 60u + s) * DOS_LFN_FT_SEC
        + (uint64_t)cs200 * 100000u;
    return 1;
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
#define DOS_LFN_FIND_LEN    0x13E
#define DOS_LFN_FIND_LONG   0x2C
#define DOS_LFN_FIND_SHORT  0x130

typedef struct {
    uint32_t    attr;
    uint64_t    ctime, atime, wtime;   /* FILETIMEs, in whatever zone the caller wants */
    uint32_t    size_hi, size_lo;
    const char *long_name;
    const char *short_name;            /* "" when the long name is already 8.3         */
} dos_lfn_find_t;

static inline void dos_lfn_put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static inline void dos_lfn_put_time(uint8_t *p, uint64_t ft, int dos_fmt)
{
    if (dos_fmt) {
        uint16_t dd = 0, dt = 0; uint8_t cs = 0;
        if (!ft || !dos_lfn_ft_to_dos(ft, &dd, &dt, &cs)) { dd = 0; dt = 0; }
        dos_lfn_put32(p, ((uint32_t)dd << 16) | dt);
        dos_lfn_put32(p + 4, 0);
    } else {
        dos_lfn_put32(p, (uint32_t)ft);
        dos_lfn_put32(p + 4, (uint32_t)(ft >> 32));
    }
}

static inline void dos_lfn_find_pack(uint8_t out[DOS_LFN_FIND_LEN], const dos_lfn_find_t *f, int dos_fmt)
{
    int k;
    for (k = 0; k < DOS_LFN_FIND_LEN; ++k) out[k] = 0;
    dos_lfn_put32(out + 0x00, f->attr);
    dos_lfn_put_time(out + 0x04, f->ctime, dos_fmt);
    dos_lfn_put_time(out + 0x0C, f->atime, dos_fmt);
    dos_lfn_put_time(out + 0x14, f->wtime, dos_fmt);
    dos_lfn_put32(out + 0x1C, f->size_hi);
    dos_lfn_put32(out + 0x20, f->size_lo);
    for (k = 0; k < 259 && f->long_name && f->long_name[k]; ++k)
        out[DOS_LFN_FIND_LONG + k] = (uint8_t)f->long_name[k];
    for (k = 0; k < 13 && f->short_name && f->short_name[k]; ++k)
        out[DOS_LFN_FIND_SHORT + k] = (uint8_t)f->short_name[k];
}

/* ── 714Eh / 7141h ATTRIBUTE MASKS. CL = ALLOWED, CH = REQUIRED (RBIL). ──────────────
     The allowed half is DOS's own find rule -- a hidden, system or directory entry
     appears only when CL asks for it, ordinary files always (dta_match in dos_int21.c
     is the 4Eh twin) -- and every bit in CH must be present. Read-only and archive
     never exclude (RBIL: "bits 0 and 5 ignored" in CL). */
static inline int dos_lfn_attr_ok(uint32_t attr, uint8_t allowed, uint8_t required)
{
    if ((attr & 0x10) && !(allowed & 0x10)) return 0;
    if ((attr & 0x02) && !(allowed & 0x02)) return 0;
    if ((attr & 0x04) && !(allowed & 0x04)) return 0;
    return (attr & (required & 0x3F)) == (uint32_t)(required & 0x3F);
}

/* ── 71A8h: A SHORT NAME FOR A LONG ONE. ─────────────────────────────────────────
     Output both forms: `s83` "NAME.EXT" ASCIIZ (DH=1) and `fcb` 11 bytes space-padded
     with no dot (DH=0).
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
static inline int dos_lfn_bad83(unsigned char c)
{
    return c < 0x20 || c == '"' || c == '*' || c == '+' || c == ',' || c == '/' || c == ':'
        || c == ';' || c == '<' || c == '=' || c == '>' || c == '?' || c == '[' || c == '\\'
        || c == ']' || c == '|';
}

static inline char dos_lfn_up(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

/* Is `n` (one path component) already a legal 8.3 name? */
static inline int dos_lfn_is_83(const char *n)
{
    int b = 0, e = -1, i;
    if (!n[0] || n[0] == '.') return 0;
    for (i = 0; n[i]; ++i) {
        unsigned char c = (unsigned char)n[i];
        if (c == '.') { if (e >= 0) return 0; e = 0; continue; }
        if (c == ' ' || dos_lfn_bad83(c)) return 0;
        if (e >= 0) { if (++e > 3) return 0; } else if (++b > 8) return 0;
    }
    return b > 0 && e != 0;                 /* "NAME." (a dot and no extension) is not */
}

static inline char dos_lfn_clean83(unsigned char c)
{
    if (c == '+' || c == ',' || c == ';' || c == '=' || c == '[' || c == ']') return '_';
    return dos_lfn_up((char)c);
}

static inline void dos_lfn_short_name(const char *lng, char s83[13], char fcb[11])
{
    const char *n = lng, *p, *dot = 0;
    char base[9], ext[4];
    int nb = 0, ne = 0, k;
    for (p = lng; *p; ++p) if (*p == '\\' || *p == '/' || *p == ':') n = p + 1;   /* last component */
    if (dos_lfn_is_83(n)) {
        for (p = n; *p && *p != '.'; ++p) if (nb < 8) base[nb++] = dos_lfn_up(*p);
        if (*p == '.') for (++p; *p && ne < 3; ++p) ext[ne++] = dos_lfn_up(*p);
    } else {
        for (p = n; *p; ++p) if (*p == '.' && p != n) dot = p;
        for (p = n; *p && p != dot && nb < 6; ++p) {
            unsigned char c = (unsigned char)*p;
            if (c == ' ' || c == '.' || c < 0x20 || c == '"' || c == '*' || c == '/'
                || c == ':' || c == '<' || c == '>' || c == '?' || c == '\\' || c == '|') continue;
            base[nb++] = dos_lfn_clean83(c);
        }
        if (!nb) base[nb++] = '_';
        base[nb++] = '~'; base[nb++] = '1';
        if (dot) for (p = dot + 1; *p && ne < 3; ++p) {
            unsigned char c = (unsigned char)*p;
            if (c == ' ' || c == '.' || c < 0x20 || c == '"' || c == '*' || c == '/'
                || c == ':' || c == '<' || c == '>' || c == '?' || c == '\\' || c == '|') continue;
            ext[ne++] = dos_lfn_clean83(c);
        }
    }
    for (k = 0; k < 11; ++k) fcb[k] = ' ';
    for (k = 0; k < nb; ++k) fcb[k] = base[k];
    for (k = 0; k < ne; ++k) fcb[8 + k] = ext[k];
    for (k = 0; k < nb; ++k) s83[k] = base[k];
    if (ne) { s83[nb] = '.'; for (k = 0; k < ne; ++k) s83[nb + 1 + k] = ext[k]; s83[nb + 1 + ne] = 0; }
    else s83[nb] = 0;
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
#define DOS_DISP_CREATE_NEW         1u
#define DOS_DISP_CREATE_ALWAYS      2u
#define DOS_DISP_OPEN_EXISTING      3u
#define DOS_DISP_OPEN_ALWAYS        4u
#define DOS_DISP_TRUNCATE_EXISTING  5u

static inline unsigned dos_ext_open_disp(unsigned action)
{
    unsigned exists = action & 0x0F, missing = (action >> 4) & 0x0F;
    if (exists == 2 && missing == 1) return DOS_DISP_CREATE_ALWAYS;
    if (exists == 1 && missing == 1) return DOS_DISP_OPEN_ALWAYS;
    if (exists == 0 && missing == 1) return DOS_DISP_CREATE_NEW;
    if (exists == 2 && missing == 0) return DOS_DISP_TRUNCATE_EXISTING;
    return DOS_DISP_OPEN_EXISTING;
}

/* CX on success: 1 opened, 2 created, 3 replaced (truncated). `existed` = the file was
   there before the call (Win32: GetLastError() == ERROR_ALREADY_EXISTS after an
   OPEN_ALWAYS / CREATE_ALWAYS that succeeded).
   ► #210: CREATE_ALWAYS ON A FILE THAT WAS NOT THERE IS "CREATED" (2), not "replaced".
     This answered 3 for every CREATE_ALWAYS -- RBIL's own table says otherwise, and
     716Ch "create or truncate" of a new long name is the probe's very first create. */
static inline unsigned dos_ext_open_taken(unsigned disp, int existed, int lfn)
{
    switch (disp) {
    case DOS_DISP_CREATE_NEW:        return 2;
    case DOS_DISP_TRUNCATE_EXISTING: return 3;
    /* s92, MEASURED (dospair p_lfn): stock's AH=6Ch says 3 ("replaced") for action 12h
       whether the file existed or not (lfn.6C.12.new/.exists) -- but its 716Ch says 2
       ("created") for a name that was not there (lfn.716C.create). Two arms, two
       answers; the LFN one is RBIL's. */
    case DOS_DISP_CREATE_ALWAYS:     return (lfn && !existed) ? 2 : 3;
    case DOS_DISP_OPEN_ALWAYS:       return existed ? 1 : 2;
    default:                         return 1;
    }
}

/* BX bits 0-2: 0 read, 1 write, 2 read/write -> GENERIC_READ 80000000h / GENERIC_WRITE
   40000000h. Only bits 0-1 are looked at, as AH=6Ch always has (mode 3, "no access",
   is not a mode DOS defines and falls to read, exactly as AH=6Ch always has). */
static inline unsigned long dos_ext_open_access(unsigned mode)
{
    mode &= 3;
    return (mode == 1) ? 0x40000000ul : (mode == 2) ? 0xC0000000ul : 0x80000000ul;
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
static inline int dos_lfn_err_from_win32(unsigned long e, unsigned short *dos)
{
    switch (e) {
    case 6: case 17: case 18: *dos = (unsigned short)e; return 1;
    case 123: *dos = 3; return 1;
    case 145: *dos = 5; return 1;
    default:  return dos_err_from_win32(e, dos);
    }
}

#endif /* DOS_LFN_H */
