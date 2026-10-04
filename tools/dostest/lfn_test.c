/* lfn_test.c -- INT 21h AH=71h (the long-filename API), the pure half, pinned off-VM. (#210)
 *
 * What this pins is ARITHMETIC AND LAYOUT, not stock NTVDM's behaviour: the FILETIME
 * expectations are computed independently (Python's calendar.timegm + the 1601 epoch
 * offset, 11644473600 s), the record offsets are WIN32_FIND_DATAA's, and the action
 * table is RBIL's. Where stock could answer differently (the short-name tail, the
 * DOS-format high dword, ...) dos_lfn.h says UNMEASURED and tools/dostest/p_lfn.asm
 * asks it -- a green run here is not a claim of parity.
 *
 *   cc -std=c99 -I src/dos -o lfn_test tools/dostest/lfn_test.c && ./lfn_test
 */
#include <stdio.h>
#include <string.h>
#include "dos_lfn.h"

static int checks, fails;

static void eq(const char *what, unsigned long long got, unsigned long long want)
{
    ++checks;
    if (got == want) return;
    ++fails;
    printf("  FAIL %-58s got 0x%llX, want 0x%llX\n", what, got, want);
}

static void streq(const char *what, const char *got, const char *want)
{
    ++checks;
    if (!strcmp(got, want)) return;
    ++fails;
    printf("  FAIL %-58s got \"%s\", want \"%s\"\n", what, got, want);
}

static void ft2dos(const char *what, uint64_t ft, int ok, unsigned date, unsigned time, unsigned cs)
{
    uint16_t d = 0xEEEE, t = 0xEEEE; uint8_t c = 0xEE;
    char l[160];
    int r = dos_lfn_ft_to_dos(ft, &d, &t, &c);
    snprintf(l, sizeof l, "%s: converts", what); eq(l, r, ok);
    if (!ok || !r) return;
    snprintf(l, sizeof l, "%s: DX date", what);  eq(l, d, date);
    snprintf(l, sizeof l, "%s: CX time", what);  eq(l, t, time);
    snprintf(l, sizeof l, "%s: BH 10ms", what);  eq(l, c, cs);
}

static void dos2ft(const char *what, unsigned date, unsigned time, unsigned cs, int ok, uint64_t want)
{
    uint64_t ft = 0xEEEEEEEEull;
    char l[160];
    int r = dos_lfn_dos_to_ft((uint16_t)date, (uint16_t)time, (uint8_t)cs, &ft);
    snprintf(l, sizeof l, "%s: converts", what); eq(l, r, ok);
    if (ok && r) { snprintf(l, sizeof l, "%s: FILETIME", what); eq(l, ft, want); }
}

static void shortname(const char *lng, const char *s83, const char *fcb11)
{
    char s[13], f[12], l[160];
    memset(s, 'Z', sizeof s);
    dos_lfn_short_name(lng, s, f);
    f[11] = 0;
    snprintf(l, sizeof l, "71A8 DH=1 \"%s\"", lng); streq(l, s, s83);
    snprintf(l, sizeof l, "71A8 DH=0 \"%s\"", lng); streq(l, f, fcb11);
}

static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

int main(void)
{
    printf("== INT 21h AH=71h long-filename API, pure half (dos_lfn.h)\n");

    /* ── 71A7h BL=0: FILETIME -> DOS. Constants from calendar.timegm. ──────────── */
    ft2dos("1980-01-01 00:00:00 (DOS epoch)", 119600064000000000ull, 1, 0x0021, 0x0000, 0);
    /* The stamp p_file sets with 5701h: 12:34:56 2001-09-17. */
    ft2dos("2001-09-17 12:34:56", 126452036960000000ull, 1, 0x2B31, 0x645C, 0);
    /* An ODD second and 250 ms: the DOS time keeps 56 (28*2); BH = 100 + 25. */
    ft2dos("2001-09-17 12:34:57.250", 126452036972500000ull, 1, 0x2B31, 0x645C, 125);
    ft2dos("2107-12-31 23:59:58 (last DOS second)", 159992927980000000ull, 1,
           (127u << 9) | (12u << 5) | 31u, (23u << 11) | (59u << 5) | 29u, 0);
    ft2dos("2108-01-01 (past DOS's range)", 159992928000000000ull, 0, 0, 0, 0);
    ft2dos("1979-12-31 23:59:59 (before it)", 119600063990000000ull, 0, 0, 0, 0);
    ft2dos("2000-02-29 06:07:08 (leap day)", 125962780280000000ull, 1,
           (20u << 9) | (2u << 5) | 29u, (6u << 11) | (7u << 5) | 4u, 0);
    /* The probe's own 71A7h case (p_lfn lfn.71A7.ft2dos): 2026-10-04 12:00:00. */
    ft2dos("2026-10-04 12:00:00 (p_lfn's value)", 0x01DD53F7E1B8E000ull, 1, 0x5D44, 0x6000, 0);

    /* ── 71A7h BL=1: DOS -> FILETIME, and the round trip. ──────────────────────── */
    dos2ft("DOS epoch", 0x0021, 0x0000, 0, 1, 119600064000000000ull);
    dos2ft("2001-09-17 12:34:56", 0x2B31, 0x645C, 0, 1, 126452036960000000ull);
    dos2ft("2001-09-17 12:34:57.250 via BH=125", 0x2B31, 0x645C, 125, 1, 126452036972500000ull);
    dos2ft("leap day 2000-02-29", (20u << 9) | (2u << 5) | 29u, (6u << 11) | (7u << 5) | 4u, 0, 1,
           125962780280000000ull);
    dos2ft("2001-02-29 (no such day)", (21u << 9) | (2u << 5) | 29u, 0, 0, 0, 0);
    dos2ft("month 13", (21u << 9) | (13u << 5) | 1u, 0, 0, 0, 0);
    dos2ft("day 0", (21u << 9) | (1u << 5), 0, 0, 0, 0);
    dos2ft("hour 24", 0x2B31, 24u << 11, 0, 0, 0);
    dos2ft("seconds/2 = 30", 0x2B31, 30u, 0, 0, 0);
    dos2ft("BH = 200", 0x2B31, 0x645C, 200, 0, 0);
    {   uint16_t d, t; uint8_t c; uint64_t back = 0;
        dos_lfn_ft_to_dos(126452036972500000ull, &d, &t, &c);
        dos_lfn_dos_to_ft(d, t, c, &back);
        eq("round trip FILETIME -> DOS -> FILETIME (10 ms exact)", back, 126452036972500000ull); }

    /* ── 714Eh/714Fh: the 318-byte record. ─────────────────────────────────────── */
    {   uint8_t r[DOS_LFN_FIND_LEN + 4];
        dos_lfn_find_t f;
        int k, junk = 0;
        memset(r, 0xCC, sizeof r);
        f.attr = 0x20; f.size_hi = 0x11223344; f.size_lo = 0x55667788;
        f.ctime = 119600064000000000ull; f.atime = 0; f.wtime = 126452036972500000ull;
        f.long_name = "A long file name.txt"; f.short_name = "ALONGF~1.TXT";
        dos_lfn_find_pack(r, &f, 0);
        eq("record length is 318 (13Eh)", DOS_LFN_FIND_LEN, 318);
        eq("bytes past the record untouched", r[DOS_LFN_FIND_LEN], 0xCC);
        eq("00h attributes", rd32(r), 0x20);
        eq("04h creation low",  rd32(r + 0x04), (uint32_t)119600064000000000ull);
        eq("08h creation high", rd32(r + 0x08), (uint32_t)(119600064000000000ull >> 32));
        eq("0Ch last access (0)", rd32(r + 0x0C) | rd32(r + 0x10), 0);
        eq("14h last write low",  rd32(r + 0x14), (uint32_t)126452036972500000ull);
        eq("18h last write high", rd32(r + 0x18), (uint32_t)(126452036972500000ull >> 32));
        eq("1Ch size HIGH first", rd32(r + 0x1C), 0x11223344);
        eq("20h size low",        rd32(r + 0x20), 0x55667788);
        eq("24h reserved zeroed", rd32(r + 0x24) | rd32(r + 0x28), 0);
        streq("2Ch long name", (const char *)r + 0x2C, "A long file name.txt");
        streq("130h short name", (const char *)r + 0x130, "ALONGF~1.TXT");
        for (k = 0x2C + 21; k < 0x130; ++k) junk |= r[k];
        eq("long name field zero past its NUL", junk, 0);
        /* SI=1: DOS date in the high word, time in the low word; high dword 0. */
        dos_lfn_find_pack(r, &f, 1);
        eq("SI=1 write time = date<<16|time", rd32(r + 0x14), (0x2B31u << 16) | 0x645Cu);
        eq("SI=1 write time high dword 0", rd32(r + 0x18), 0);
        eq("SI=1 creation = DOS epoch", rd32(r + 0x04), (0x0021u << 16));
        eq("SI=1 a zero FILETIME stays 0", rd32(r + 0x0C), 0);
        /* A 259-character long name fills the field and keeps its NUL. */
        {   char big[400]; memset(big, 'x', 300); big[300] = 0;
            f.long_name = big; dos_lfn_find_pack(r, &f, 0);
            eq("an over-long name is cut at 259 + NUL", r[0x2C + 259], 0);
            eq("...and the short name field is intact", r[0x130], 'A'); }
    }

    /* ── CL allowed / CH required. ─────────────────────────────────────────────── */
    eq("plain file, CL=0",               dos_lfn_attr_ok(0x20, 0x00, 0x00), 1);
    eq("directory, CL=0 -> hidden",      dos_lfn_attr_ok(0x10, 0x00, 0x00), 0);
    eq("directory, CL=10h",              dos_lfn_attr_ok(0x10, 0x10, 0x00), 1);
    eq("hidden file needs CL bit 1",     dos_lfn_attr_ok(0x22, 0x00, 0x00), 0);
    eq("system file with CL=16h",        dos_lfn_attr_ok(0x24, 0x16, 0x00), 1);
    eq("CH=10h: a file is not a dir",    dos_lfn_attr_ok(0x20, 0x10, 0x10), 0);
    eq("CH=10h: a dir is",               dos_lfn_attr_ok(0x10, 0x10, 0x10), 1);
    eq("read-only never excludes",       dos_lfn_attr_ok(0x21, 0x00, 0x00), 1);
    eq("Win32 NORMAL (80h) is a file",   dos_lfn_attr_ok(0x80, 0x00, 0x00), 1);

    /* ── 71A8h. ────────────────────────────────────────────────────────────────── */
    shortname("A long file name.txt", "ALONGF~1.TXT", "ALONGF~1TXT");
    shortname("Long Directory Name",  "LONGDI~1",     "LONGDI~1   ");
    shortname("readme.txt",           "README.TXT",   "README  TXT");   /* already 8.3 */
    shortname("COMMAND.COM",          "COMMAND.COM",  "COMMAND COM");
    shortname("a.b.c.txt",            "ABC~1.TXT",    "ABC~1   TXT");   /* dots dropped */
    shortname("my+file=1.html",       "MY_FIL~1.HTM", "MY_FIL~1HTM");   /* + = -> _ */
    shortname("x y.z",                "XY~1.Z",       "XY~1    Z  ");
    shortname("C:\\dir\\Some Name.doc", "SOMENA~1.DOC", "SOMENA~1DOC"); /* last component */
    shortname("NAME.",                "NAME~1",       "NAME~1     ");   /* a bare dot is not 8.3 */
    shortname("toolongname.txt",      "TOOLON~1.TXT", "TOOLON~1TXT");

    /* ── 6Ch / 716Ch action word. ─────────────────────────────────────────────── */
    eq("action 01h open|fail",      dos_ext_open_disp(0x01), DOS_DISP_OPEN_EXISTING);
    eq("action 10h fail|create",    dos_ext_open_disp(0x10), DOS_DISP_CREATE_NEW);
    eq("action 11h open|create",    dos_ext_open_disp(0x11), DOS_DISP_OPEN_ALWAYS);
    eq("action 12h trunc|create",   dos_ext_open_disp(0x12), DOS_DISP_CREATE_ALWAYS);
    eq("action 02h trunc|fail",     dos_ext_open_disp(0x02), DOS_DISP_TRUNCATE_EXISTING);
    eq("action 00h (meaningless) -> open, as 6Ch always did", dos_ext_open_disp(0x00), DOS_DISP_OPEN_EXISTING);
    eq("taken: open existing      -> 1", dos_ext_open_taken(DOS_DISP_OPEN_EXISTING, 1, 0), 1);
    eq("taken: create new         -> 2", dos_ext_open_taken(DOS_DISP_CREATE_NEW, 0, 0), 2);
    eq("taken: truncate existing  -> 3", dos_ext_open_taken(DOS_DISP_TRUNCATE_EXISTING, 1, 0), 3);
    eq("taken: open-always, there -> 1", dos_ext_open_taken(DOS_DISP_OPEN_ALWAYS, 1, 0), 1);
    eq("taken: open-always, new   -> 2", dos_ext_open_taken(DOS_DISP_OPEN_ALWAYS, 0, 0), 2);
    eq("taken: create-always, there -> 3 (replaced)", dos_ext_open_taken(DOS_DISP_CREATE_ALWAYS, 1, 0), 3);
    eq("taken: create-always, new   -> 3 (stock 6Ch, p_lfn)", dos_ext_open_taken(DOS_DISP_CREATE_ALWAYS, 0, 0), 3);
    eq("taken: create-always, new, 716Ch -> 2 (stock, p_lfn)", dos_ext_open_taken(DOS_DISP_CREATE_ALWAYS, 0, 1), 2);
    eq("taken: create-always, there, 716Ch -> 3", dos_ext_open_taken(DOS_DISP_CREATE_ALWAYS, 1, 1), 3);
    eq("BX mode 0 -> GENERIC_READ",       dos_ext_open_access(0), 0x80000000ul);
    eq("BX mode 1 -> GENERIC_WRITE",      dos_ext_open_access(1), 0x40000000ul);
    eq("BX mode 2 -> read|write",         dos_ext_open_access(2), 0xC0000000ul);
    eq("BX 0x2042 (share bits) -> r|w",   dos_ext_open_access(0x2042), 0xC0000000ul);

    /* ── errors. ────────────────────────────────────────────────────────────────── */
    {   unsigned short d = 0;
        eq("w32 2 -> 2 (measured row)",   dos_lfn_err_from_win32(2, &d) && d == 2, 1);
        eq("w32 18 -> 18 no more files",  dos_lfn_err_from_win32(18, &d) && d == 18, 1);
        eq("w32 6 -> 6 invalid handle",   dos_lfn_err_from_win32(6, &d) && d == 6, 1);
        eq("w32 145 dir not empty -> 5",  dos_lfn_err_from_win32(145, &d) && d == 5, 1);
        eq("w32 183 already exists -> 80", dos_lfn_err_from_win32(183, &d) && d == 0x50, 1);
        eq("w32 87 unmapped -> 0, AX 2",  dos_lfn_err_from_win32(87, &d) == 0 && d == 2, 1); }

    printf("== %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
