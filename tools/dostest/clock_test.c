/* clock_test.c -- the VDM clock (src/dos/dos_clock.h), pinned off-VM.  GH #250.
 *
 * The validity rules are the ones p_clock.asm measured on MS-DOS 6.22 (QEMU), PCem
 * (real AMI BIOS) and DOSBox-X -- each check names its probe row. The days of the
 * week are calendar facts the same probe read back from 2Ah's AL.
 *
 *   cc -std=c99 -I src/dos -o clock_test tools/dostest/clock_test.c && ./clock_test
 */
#include <stdio.h>
#include "dos_clock.h"

dclk_state g_dos_clock;

static int checks, fails;

static void eq(const char *what, long long got, long long want)
{
    ++checks;
    if (got == want) return;
    ++fails;
    printf("  FAIL %-58s got %lld, want %lld\n", what, got, want);
}

static dclk_t mk(unsigned y, unsigned mo, unsigned d,
                 unsigned h, unsigned mi, unsigned s, unsigned cs)
{
    dclk_t t; t.year = y; t.month = mo; t.day = d;
    t.hour = h; t.min = mi; t.sec = s; t.cs = cs; t.dow = 0;
    return t;
}

static void dow(const char *what, unsigned y, unsigned m, unsigned d, unsigned want)
{
    dclk_t t = mk(y, m, d, 0, 0, 0, 0), u;
    dclk_unpack(dclk_pack(&t), &u);
    eq(what, u.dow, want);
}

int main(void)
{
    /* ── validity: p_clock clk.2b.* / clk.2d.* (AL=00 taken, AL=FF refused) ── */
    eq("clk.2b.19991231 taken",        dclk_dos_date_ok(1999, 12, 31), 1);
    eq("clk.2b.feb30 refused",         dclk_dos_date_ok(2001, 2, 30), 0);
    eq("clk.2b.leap2000 taken",        dclk_dos_date_ok(2000, 2, 29), 1);
    eq("clk.2b.feb29.2001 refused",    dclk_dos_date_ok(2001, 2, 29), 0);
    eq("clk.2b.y1979 refused",         dclk_dos_date_ok(1979, 12, 31), 0);
    eq("clk.2b.y1980 taken",           dclk_dos_date_ok(1980, 1, 1), 1);
    eq("clk.2b.y2099 taken",           dclk_dos_date_ok(2099, 12, 31), 1);
    eq("clk.2b.y2100 refused",         dclk_dos_date_ok(2100, 1, 1), 0);
    eq("clk.2b.month13 refused",       dclk_dos_date_ok(2004, 13, 1), 0);
    eq("clk.2b.month0 refused",        dclk_dos_date_ok(2004, 0, 1), 0);
    eq("clk.2b.day0 refused",          dclk_dos_date_ok(2004, 4, 0), 0);
    eq("clk.2b.apr31 refused",         dclk_dos_date_ok(2004, 4, 31), 0);
    eq("clk.2b.leap2004 taken",        dclk_dos_date_ok(2004, 2, 29), 1);
    eq("1900 is not a leap year",      dclk_leap(1900), 0);
    eq("clk.2d.123410 taken",          dclk_time_ok(12, 34, 10, 0), 1);
    eq("clk.2d.235959 taken",          dclk_time_ok(23, 59, 59, 99), 1);
    eq("clk.2d.000000 taken",          dclk_time_ok(0, 0, 0, 0), 1);
    eq("clk.2d.h24 refused",           dclk_time_ok(24, 0, 0, 0), 0);
    eq("clk.2d.m60 refused",           dclk_time_ok(12, 60, 0, 0), 0);
    eq("clk.2d.s60 refused",           dclk_time_ok(12, 0, 60, 0), 0);
    eq("clk.2d.cs100 refused",         dclk_time_ok(12, 0, 0, 100), 0);

    /* ── day of week, 2Ah's AL (0 = Sunday) ── */
    dow("1999-12-31 is a Friday (clk.2a.after.19991231)", 1999, 12, 31, 5);
    dow("2000-02-29 is a Tuesday (clk.2a.after.leap2000)", 2000, 2, 29, 2);
    dow("1980-01-01 is a Tuesday (clk.2a.after.y1980)",   1980, 1, 1, 2);
    dow("2099-12-31 is a Thursday (clk.2a.after.y2099)",  2099, 12, 31, 4);
    dow("2004-02-29 is a Sunday (clk.2a.after.leap2004)", 2004, 2, 29, 0);
    dow("2000-01-01 is a Saturday (clk.midnight.2a)",     2000, 1, 1, 6);
    dow("2026-10-02 is a Friday",                         2026, 10, 2, 5);

    /* ── pack/unpack round trip over every day 1980..2099 ── */
    {
        unsigned y, m, d, bad = 0, n = 0;
        int64_t prev = 0;
        for (y = 1980; y <= 2099; ++y)
            for (m = 1; m <= 12; ++m)
                for (d = 1; d <= dclk_mdays(y, m); ++d) {
                    dclk_t t = mk(y, m, d, 13, 7, 41, 59), u;
                    int64_t v = dclk_pack(&t);
                    dclk_unpack(v, &u);
                    if (u.year != y || u.month != m || u.day != d || u.hour != 13
                        || u.min != 7 || u.sec != 41 || u.cs != 59) ++bad;
                    if (n && v - prev != DCLK_CS_PER_DAY) ++bad;   /* contiguous days */
                    prev = v; ++n;
                }
        eq("round trip, every day 1980..2099", bad, 0);
        eq("days in 1980..2099", n, 43830);
    }

    /* ── the offset: setting date/time moves the GUEST reading only ── */
    {
        dclk_t host = mk(2026, 10, 2, 9, 15, 30, 25), g;
        int64_t off = 0;
        dclk_read(&host, off, &g);
        eq("offset 0 reads the host (hour)", g.hour, 9);
        eq("offset 0 reads the host (day)", g.day, 2);

        dclk_set_date(&host, &off, 1999, 12, 31);
        dclk_read(&host, off, &g);
        eq("set date: year", g.year, 1999);
        eq("set date: month", g.month, 12);
        eq("set date: day", g.day, 31);
        eq("set date keeps the time of day (hour)", g.hour, 9);
        eq("set date keeps the time of day (cs)", g.cs, 25);
        eq("set date: whole days only", off % DCLK_CS_PER_DAY, 0);

        dclk_set_time(&host, &off, 23, 59, 59, 50);
        dclk_read(&host, off, &g);
        eq("set time keeps the date (day)", g.day, 31);
        eq("set time: hour", g.hour, 23);
        eq("set time: cs", g.cs, 50);

        /* the host advances 60 cs: the guest crosses midnight into 2000 */
        host.cs = 85; host.sec = 30;   /* +60 cs */
        dclk_read(&host, off, &g);
        eq("midnight: year rolls to 2000", g.year, 2000);
        eq("midnight: 01-01", g.month * 100 + g.day, 101);
        eq("midnight: 00:00:00.10", g.hour * 10000 + g.min * 100 + g.sec, 0);
        eq("midnight: cs", g.cs, 10);
        eq("midnight: Saturday", g.dow, 6);

        /* the host's clock is an INPUT: nothing here can have written it */
        eq("host untouched (year)", host.year, 2026);
    }

    /* ── ticks since midnight (0040:006C after 2Dh) ── */
    eq("ticks 00:00:00.00", dclk_ticks(0, 0, 0, 0), 0);
    eq("ticks 12:34:10.00 (clk.1a00.after.2d CX=000C)", dclk_ticks(12, 34, 10, 0) >> 16, 0xC);
    eq("ticks 12:34:10.00", dclk_ticks(12, 34, 10, 0), 823844);
    /* #262: the inverse, as CLOCK$ reads a count -- p_tick2c's two targets. */
    { unsigned h, mi, sx, cs;
      dclk_from_ticks(0x000A026Cu, &h, &mi, &sx, &cs);
      eq("from_ticks 000A:026C -> 10h", h, 10); eq("from_ticks 000A:026C -> 00m", mi, 0);
      eq("from_ticks 000A:026C -> 29.96s (655980 ticks is just short of :30)", sx * 100 + cs, 2996);
      dclk_from_ticks(0x000B8277u, &h, &mi, &sx, &cs);
      eq("from_ticks 000B:8277 -> 11h", h, 11); eq("from_ticks 000B:8277 -> 30m", mi, 30);
      dclk_from_ticks(dclk_ticks(12, 34, 10, 0), &h, &mi, &sx, &cs);
      /* a tick is ~5.5 cs, so reading a time back from its count is exact only to a tick */
      eq("from_ticks(ticks(12:34:10)) = 12:34:09.96, within one tick", sx * 100 + cs, 996);
      dclk_from_ticks(0x1800B0u, &h, &mi, &sx, &cs);
      eq("from_ticks at a full day clamps to 23h", h, 23); }
    eq("ticks 23:59:59.99 < a day (0x1800B0)", dclk_ticks(23, 59, 59, 99) < 0x1800B0u, 1);
    eq("ticks_cs(1) = 5 (a tick is 5.49 cs, floored)", dclk_ticks_cs(1), 5);
    /* 0x1800B0 is the BIOS's day, but at 1193182 Hz it is 86399.85 s, not 86400 */
    eq("ticks_cs(a day's count) = 8639985 (24h - 0.15 s)", dclk_ticks_cs(0x1800B0u), 8639985);

    /* ── #262 case B: DOS's clock follows a count the BIOS did not write ── */
    {
        dclk_t host = mk(2026, 10, 2, 9, 15, 30, 25), g;
        int64_t off = 0;
        /* p_tick2c B: 11:30:30 stored (000B:8277), read at once */
        dclk_follow_ticks(&host, &off, 0x000B8277u, 0, 0);
        dclk_read(&host, off, &g);
        eq("follow 000B:8277 -> 11:30", g.hour * 100 + g.min, 1130);
        eq("follow 000B:8277 -> :29.95 (4142995 cs)", g.sec * 100 + g.cs, 2995);
        eq("follow keeps the date", g.year * 10000 + g.month * 100 + g.day, 20261002);
        /* the host moves on 2 s: the guest's clock runs on from the store */
        host.sec = 32;
        dclk_read(&host, off, &g);
        eq("then runs on host time (:29.95 -> :31.95)", g.sec * 100 + g.cs, 3195);

        /* a store at 23:59:59 on the guest's 1999-12-31; the count wraps once, read
           5 s of ticks after the store: 2000-01-01 00:00:04 -- and the date is the
           STORE's day + 1, though host-now + offset had not crossed midnight. */
        host = mk(2026, 10, 2, 9, 0, 0, 0); off = 0;
        dclk_set_date(&host, &off, 1999, 12, 31);
        dclk_set_time(&host, &off, 12, 0, 0, 0);
        host.sec = 5;                                     /* +5 s of host time */
        dclk_follow_ticks(&host, &off, dclk_ticks(0, 0, 4, 0), 1, 91 /* ~5 s of ticks */);
        dclk_read(&host, off, &g);
        eq("wrap: 2000-01-01", g.year * 10000 + g.month * 100 + g.day, 20000101);
        eq("wrap: 00:00:03.9x (a tick short of :04)", g.hour * 10000 + g.min * 100 + g.sec, 3);
        eq("wrap: Saturday", g.dow, 6);

        /* ★ the trap the `since` argument exists for: the guest's own clock crossed
           midnight AFTER the store (it read 23:59:58 then; now 00:00:08 next day), and
           the count wrapped once. Taking "today" from host-now + offset would add the
           wrap to a day that already contains it -- 2000-01-02. DOS's day number is the
           store's day plus the wraps: 2000-01-01. */
        host = mk(2026, 10, 2, 9, 0, 0, 0); off = 0;
        dclk_set_date(&host, &off, 1999, 12, 31);
        dclk_set_time(&host, &off, 23, 59, 58, 0);
        host.sec = 10;                                    /* guest now 2000-01-01 00:00:08 */
        dclk_follow_ticks(&host, &off, dclk_ticks(0, 0, 5, 0), 1, 182 /* ~10 s of ticks */);
        dclk_read(&host, off, &g);
        eq("store before the guest's midnight: 2000-01-01, not 01-02",
           g.year * 10000 + g.month * 100 + g.day, 20000101);
        eq("... at 00:00", g.hour * 100 + g.min, 0);

        /* no wrap, a store read the same instant: only the time of day moves */
        host = mk(2026, 10, 2, 23, 59, 59, 0); off = 0;
        dclk_follow_ticks(&host, &off, dclk_ticks(1, 2, 3, 0), 0, 0);
        dclk_read(&host, off, &g);
        eq("store 01:02:03 at the host's 23:59:59 keeps the host's day",
           g.year * 10000 + g.month * 100 + g.day, 20261002);
        eq("... and reads 01:02", g.hour * 100 + g.min, 102);

        /* the day's length itself (a count the BIOS never holds) reads 23:59:59.85;
           a nonsense count past it is clamped to the last hundredth, never a next day */
        host = mk(2026, 10, 2, 9, 0, 0, 0); off = 0;
        dclk_follow_ticks(&host, &off, 0x1800B0u, 0, 0);
        dclk_read(&host, off, &g);
        eq("count 0x1800B0 -> 23:59:59.85", g.hour * 1000000 + g.min * 10000 + g.sec * 100 + g.cs,
           23595985);
        off = 0;
        dclk_follow_ticks(&host, &off, 0xFFFFFFFFu, 0, 0);
        dclk_read(&host, off, &g);
        eq("count FFFFFFFF -> 23:59:59.99, same day", g.day * 100000000LL + g.hour * 1000000
           + g.min * 10000 + g.sec * 100 + g.cs, 223595999LL);
    }

    printf("== %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
