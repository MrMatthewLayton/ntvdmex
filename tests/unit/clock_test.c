/* clock_test.c -- the VDM clock (src/dos/dos_clock.h), pinned off-VM.  GH #250.
 *
 * The validity rules are the ones p_clock.asm measured on MS-DOS 6.22 (QEMU), PCem
 * (real AMI BIOS) and DOSBox-X -- each check names its probe row. The days of the
 * week are calendar facts the same probe read back from 2Ah's AL.
 *
 *   cc -std=c99 -I src/dos -o clock_test tests/unit/clock_test.c && ./clock_test
 *
 * Dates and times are written as their year, month, day, hour, minute, second and
 * hundredths, the way each check's name reads them.
 */
#include <stdio.h>
#include "dos_clock.h"

DOS_CLOCK_STATE g_DosClock;

/* Days of the week as 2Ah's AL numbers them. */
#define CLOCK_TEST_SUNDAY             0
#define CLOCK_TEST_TUESDAY            2
#define CLOCK_TEST_THURSDAY           4
#define CLOCK_TEST_FRIDAY             5
#define CLOCK_TEST_SATURDAY           6

/* The round trip: its years, and how many days they hold. */
#define CLOCK_TEST_FIRST_YEAR         1980
#define CLOCK_TEST_LAST_YEAR          2099
#define CLOCK_TEST_DAYS_1980_TO_2099  43830

/* Combine fields into one number so a check can compare them at once. */
#define CLOCK_TEST_YEAR_SCALE         10000
#define CLOCK_TEST_MONTH_SCALE        100
#define CLOCK_TEST_HOUR_SCALE         10000
#define CLOCK_TEST_MINUTE_SCALE       100
#define CLOCK_TEST_SECOND_SCALE       100     /* seconds * 100 + hundredths              */
#define CLOCK_TEST_HMS_HOUR_SCALE     1000000 /* hour, minute, second and hundredths     */
#define CLOCK_TEST_HMS_MINUTE_SCALE   10000
#define CLOCK_TEST_DAY_SCALE          100000000LL

/* BIOS tick counts (0040:006C). */
#define CLOCK_TEST_TICKS_12_34_10     823844
#define CLOCK_TEST_TICKS_HIGH_SHIFT   16      /* CX, as INT 1Ah AH=00h splits the count  */
#define CLOCK_TEST_TICKS_HIGH_12_34   0xC     /* clk.1a00.after.2d CX=000C               */
#define CLOCK_TEST_TICKS_10_00_29     0x000A026Cu
#define CLOCK_TEST_TICKS_11_30_30     0x000B8277u
#define CLOCK_TEST_TICKS_PER_DAY      0x1800B0u
#define CLOCK_TEST_TICKS_NONSENSE     0xFFFFFFFFu
#define CLOCK_TEST_HUNDREDTHS_IN_TICK 5
#define CLOCK_TEST_HUNDREDTHS_IN_DAY  8639985 /* 0x1800B0 ticks: 24h - 0.15 s             */
#define CLOCK_TEST_TICKS_5_SECONDS    91      /* ~5 s of ticks                           */
#define CLOCK_TEST_TICKS_10_SECONDS   182     /* ~10 s of ticks                          */

static INT g_Checks, g_Failures;

static VOID ClockTestExpect(PCSTR description, INT64 actual, INT64 expected)
{
    ++g_Checks;
    if (actual == expected) return;
    ++g_Failures;
    printf("  FAIL %-58s got %lld, want %lld\n", description, actual, expected);
}

static DOS_CLOCK_TIME ClockTestMake(UINT year, UINT month, UINT day,
                                    UINT hour, UINT minute, UINT second, UINT hundredths)
{
    DOS_CLOCK_TIME time; time.Year = year; time.Month = month; time.Day = day;
    time.Hour = hour; time.Minute = minute; time.Second = second; time.Hundredths = hundredths;
    time.DayOfWeek = CLOCK_TEST_SUNDAY;
    return time;
}

static VOID ClockTestDayOfWeek(PCSTR description, UINT year, UINT month, UINT day,
                               UINT expectedDayOfWeek)
{
    DOS_CLOCK_TIME time = ClockTestMake(year, month, day, 0, 0, 0, 0), unpacked;
    DosClockUnpack(DosClockPack(&time), &unpacked);
    ClockTestExpect(description, unpacked.DayOfWeek, expectedDayOfWeek);
}

INT main(VOID)
{
    /* ── validity: p_clock clk.2b.* / clk.2d.* (AL=00 taken, AL=FF refused) ── */
    ClockTestExpect("clk.2b.19991231 taken",        DosClockIsDosDateValid(1999, 12, 31), TRUE);
    ClockTestExpect("clk.2b.feb30 refused",         DosClockIsDosDateValid(2001, 2, 30), FALSE);
    ClockTestExpect("clk.2b.leap2000 taken",        DosClockIsDosDateValid(2000, 2, 29), TRUE);
    ClockTestExpect("clk.2b.feb29.2001 refused",    DosClockIsDosDateValid(2001, 2, 29), FALSE);
    ClockTestExpect("clk.2b.y1979 refused",         DosClockIsDosDateValid(1979, 12, 31), FALSE);
    ClockTestExpect("clk.2b.y1980 taken",           DosClockIsDosDateValid(1980, 1, 1), TRUE);
    ClockTestExpect("clk.2b.y2099 taken",           DosClockIsDosDateValid(2099, 12, 31), TRUE);
    ClockTestExpect("clk.2b.y2100 refused",         DosClockIsDosDateValid(2100, 1, 1), FALSE);
    ClockTestExpect("clk.2b.month13 refused",       DosClockIsDosDateValid(2004, 13, 1), FALSE);
    ClockTestExpect("clk.2b.month0 refused",        DosClockIsDosDateValid(2004, 0, 1), FALSE);
    ClockTestExpect("clk.2b.day0 refused",          DosClockIsDosDateValid(2004, 4, 0), FALSE);
    ClockTestExpect("clk.2b.apr31 refused",         DosClockIsDosDateValid(2004, 4, 31), FALSE);
    ClockTestExpect("clk.2b.leap2004 taken",        DosClockIsDosDateValid(2004, 2, 29), TRUE);
    ClockTestExpect("1900 is not a leap year",      DosClockIsLeapYear(1900), FALSE);
    ClockTestExpect("clk.2d.123410 taken",          DosClockIsTimeValid(12, 34, 10, 0), TRUE);
    ClockTestExpect("clk.2d.235959 taken",          DosClockIsTimeValid(23, 59, 59, 99), TRUE);
    ClockTestExpect("clk.2d.000000 taken",          DosClockIsTimeValid(0, 0, 0, 0), TRUE);
    ClockTestExpect("clk.2d.h24 refused",           DosClockIsTimeValid(24, 0, 0, 0), FALSE);
    ClockTestExpect("clk.2d.m60 refused",           DosClockIsTimeValid(12, 60, 0, 0), FALSE);
    ClockTestExpect("clk.2d.s60 refused",           DosClockIsTimeValid(12, 0, 60, 0), FALSE);
    ClockTestExpect("clk.2d.cs100 refused",         DosClockIsTimeValid(12, 0, 0, 100), FALSE);

    /* ── day of week, 2Ah's AL (0 = Sunday) ── */
    ClockTestDayOfWeek("1999-12-31 is a Friday (clk.2a.after.19991231)", 1999, 12, 31,
                       CLOCK_TEST_FRIDAY);
    ClockTestDayOfWeek("2000-02-29 is a Tuesday (clk.2a.after.leap2000)", 2000, 2, 29,
                       CLOCK_TEST_TUESDAY);
    ClockTestDayOfWeek("1980-01-01 is a Tuesday (clk.2a.after.y1980)",   1980, 1, 1,
                       CLOCK_TEST_TUESDAY);
    ClockTestDayOfWeek("2099-12-31 is a Thursday (clk.2a.after.y2099)",  2099, 12, 31,
                       CLOCK_TEST_THURSDAY);
    ClockTestDayOfWeek("2004-02-29 is a Sunday (clk.2a.after.leap2004)", 2004, 2, 29,
                       CLOCK_TEST_SUNDAY);
    ClockTestDayOfWeek("2000-01-01 is a Saturday (clk.midnight.2a)",     2000, 1, 1,
                       CLOCK_TEST_SATURDAY);
    ClockTestDayOfWeek("2026-10-02 is a Friday",                         2026, 10, 2,
                       CLOCK_TEST_FRIDAY);

    /* ── pack/unpack round trip over every day 1980..2099 ── */
    {
        UINT year, month, day, mismatches = 0, dayCount = 0;
        INT64 previous = 0;
        for (year = CLOCK_TEST_FIRST_YEAR; year <= CLOCK_TEST_LAST_YEAR; ++year)
            for (month = DOS_CLOCK_JANUARY; month <= DOS_CLOCK_DECEMBER; ++month)
                for (day = DOS_CLOCK_FIRST_DAY; day <= DosClockDaysInMonth(year, month); ++day) {
                    DOS_CLOCK_TIME time = ClockTestMake(year, month, day, 13, 7, 41, 59), unpacked;
                    INT64 packed = DosClockPack(&time);
                    DosClockUnpack(packed, &unpacked);
                    if (unpacked.Year != year || unpacked.Month != month || unpacked.Day != day
                        || unpacked.Hour != 13 || unpacked.Minute != 7 || unpacked.Second != 41
                        || unpacked.Hundredths != 59) ++mismatches;
                    if (dayCount && packed - previous != DOS_CLOCK_HUNDREDTHS_PER_DAY)
                        ++mismatches;                                  /* contiguous days */
                    previous = packed; ++dayCount;
                }
        ClockTestExpect("round trip, every day 1980..2099", mismatches, 0);
        ClockTestExpect("days in 1980..2099", dayCount, CLOCK_TEST_DAYS_1980_TO_2099);
    }

    /* ── the offset: setting date/time moves the GUEST reading only ── */
    {
        DOS_CLOCK_TIME hostNow = ClockTestMake(2026, 10, 2, 9, 15, 30, 25), guestNow;
        INT64 offset = 0;
        DosClockApplyOffset(&hostNow, offset, &guestNow);
        ClockTestExpect("offset 0 reads the host (hour)", guestNow.Hour, 9);
        ClockTestExpect("offset 0 reads the host (day)", guestNow.Day, 2);

        DosClockSetDate(&hostNow, &offset, 1999, 12, 31);
        DosClockApplyOffset(&hostNow, offset, &guestNow);
        ClockTestExpect("set date: year", guestNow.Year, 1999);
        ClockTestExpect("set date: month", guestNow.Month, 12);
        ClockTestExpect("set date: day", guestNow.Day, 31);
        ClockTestExpect("set date keeps the time of day (hour)", guestNow.Hour, 9);
        ClockTestExpect("set date keeps the time of day (cs)", guestNow.Hundredths, 25);
        ClockTestExpect("set date: whole days only", offset % DOS_CLOCK_HUNDREDTHS_PER_DAY, 0);

        DosClockSetTime(&hostNow, &offset, 23, 59, 59, 50);
        DosClockApplyOffset(&hostNow, offset, &guestNow);
        ClockTestExpect("set time keeps the date (day)", guestNow.Day, 31);
        ClockTestExpect("set time: hour", guestNow.Hour, 23);
        ClockTestExpect("set time: cs", guestNow.Hundredths, 50);

        /* the host advances 60 cs: the guest crosses midnight into 2000 */
        hostNow.Hundredths = 85; hostNow.Second = 30;   /* +60 cs */
        DosClockApplyOffset(&hostNow, offset, &guestNow);
        ClockTestExpect("midnight: year rolls to 2000", guestNow.Year, 2000);
        ClockTestExpect("midnight: 01-01", guestNow.Month * CLOCK_TEST_MONTH_SCALE + guestNow.Day,
                        101);
        ClockTestExpect("midnight: 00:00:00.10",
                        guestNow.Hour * CLOCK_TEST_HOUR_SCALE
                        + guestNow.Minute * CLOCK_TEST_MINUTE_SCALE + guestNow.Second, 0);
        ClockTestExpect("midnight: cs", guestNow.Hundredths, 10);
        ClockTestExpect("midnight: Saturday", guestNow.DayOfWeek, CLOCK_TEST_SATURDAY);

        /* the host's clock is an INPUT: nothing here can have written it */
        ClockTestExpect("host untouched (year)", hostNow.Year, 2026);
    }

    /* ── ticks since midnight (0040:006C after 2Dh) ── */
    ClockTestExpect("ticks 00:00:00.00", DosClockTicksFromTime(0, 0, 0, 0), 0);
    ClockTestExpect("ticks 12:34:10.00 (clk.1a00.after.2d CX=000C)",
                    DosClockTicksFromTime(12, 34, 10, 0) >> CLOCK_TEST_TICKS_HIGH_SHIFT,
                    CLOCK_TEST_TICKS_HIGH_12_34);
    ClockTestExpect("ticks 12:34:10.00", DosClockTicksFromTime(12, 34, 10, 0),
                    CLOCK_TEST_TICKS_12_34_10);
    /* #262: the inverse, as CLOCK$ reads a count -- p_tick2c's two targets. */
    { UINT hour, minute, second, hundredths;
      DosClockTimeFromTicks(CLOCK_TEST_TICKS_10_00_29, &hour, &minute, &second, &hundredths);
      ClockTestExpect("from_ticks 000A:026C -> 10h", hour, 10);
      ClockTestExpect("from_ticks 000A:026C -> 00m", minute, 0);
      ClockTestExpect("from_ticks 000A:026C -> 29.96s (655980 ticks is just short of :30)",
                      second * CLOCK_TEST_SECOND_SCALE + hundredths, 2996);
      DosClockTimeFromTicks(CLOCK_TEST_TICKS_11_30_30, &hour, &minute, &second, &hundredths);
      ClockTestExpect("from_ticks 000B:8277 -> 11h", hour, 11);
      ClockTestExpect("from_ticks 000B:8277 -> 30m", minute, 30);
      DosClockTimeFromTicks(DosClockTicksFromTime(12, 34, 10, 0), &hour, &minute, &second,
                            &hundredths);
      /* a tick is ~5.5 cs, so reading a time back from its count is exact only to a tick */
      ClockTestExpect("from_ticks(ticks(12:34:10)) = 12:34:09.96, within one tick",
                      second * CLOCK_TEST_SECOND_SCALE + hundredths, 996);
      DosClockTimeFromTicks(CLOCK_TEST_TICKS_PER_DAY, &hour, &minute, &second, &hundredths);
      ClockTestExpect("from_ticks at a full day clamps to 23h", hour, 23); }
    ClockTestExpect("ticks 23:59:59.99 < a day (0x1800B0)",
                    DosClockTicksFromTime(23, 59, 59, 99) < CLOCK_TEST_TICKS_PER_DAY, TRUE);
    ClockTestExpect("ticks_cs(1) = 5 (a tick is 5.49 cs, floored)", DosClockHundredthsFromTicks(1),
                    CLOCK_TEST_HUNDREDTHS_IN_TICK);
    /* 0x1800B0 is the BIOS's day, but at 1193182 Hz it is 86399.85 s, not 86400 */
    ClockTestExpect("ticks_cs(a day's count) = 8639985 (24h - 0.15 s)",
                    DosClockHundredthsFromTicks(CLOCK_TEST_TICKS_PER_DAY),
                    CLOCK_TEST_HUNDREDTHS_IN_DAY);

    /* ── #262 case B: DOS's clock follows a count the BIOS did not write ── */
    {
        DOS_CLOCK_TIME hostNow = ClockTestMake(2026, 10, 2, 9, 15, 30, 25), guestNow;
        INT64 offset = 0;
        /* p_tick2c B: 11:30:30 stored (000B:8277), read at once */
        DosClockFollowTicks(&hostNow, &offset, CLOCK_TEST_TICKS_11_30_30, 0, 0);
        DosClockApplyOffset(&hostNow, offset, &guestNow);
        ClockTestExpect("follow 000B:8277 -> 11:30",
                        guestNow.Hour * CLOCK_TEST_MINUTE_SCALE + guestNow.Minute, 1130);
        ClockTestExpect("follow 000B:8277 -> :29.95 (4142995 cs)",
                        guestNow.Second * CLOCK_TEST_SECOND_SCALE + guestNow.Hundredths, 2995);
        ClockTestExpect("follow keeps the date",
                        guestNow.Year * CLOCK_TEST_YEAR_SCALE
                        + guestNow.Month * CLOCK_TEST_MONTH_SCALE + guestNow.Day, 20261002);
        /* the host moves on 2 s: the guest's clock runs on from the store */
        hostNow.Second = 32;
        DosClockApplyOffset(&hostNow, offset, &guestNow);
        ClockTestExpect("then runs on host time (:29.95 -> :31.95)",
                        guestNow.Second * CLOCK_TEST_SECOND_SCALE + guestNow.Hundredths, 3195);

        /* a store at 23:59:59 on the guest's 1999-12-31; the count wraps once, read
           5 s of ticks after the store: 2000-01-01 00:00:04 -- and the date is the
           STORE's day + 1, though host-now + offset had not crossed midnight. */
        hostNow = ClockTestMake(2026, 10, 2, 9, 0, 0, 0); offset = 0;
        DosClockSetDate(&hostNow, &offset, 1999, 12, 31);
        DosClockSetTime(&hostNow, &offset, 12, 0, 0, 0);
        hostNow.Second = 5;                                     /* +5 s of host time */
        DosClockFollowTicks(&hostNow, &offset, DosClockTicksFromTime(0, 0, 4, 0), 1,
                            CLOCK_TEST_TICKS_5_SECONDS);
        DosClockApplyOffset(&hostNow, offset, &guestNow);
        ClockTestExpect("wrap: 2000-01-01",
                        guestNow.Year * CLOCK_TEST_YEAR_SCALE
                        + guestNow.Month * CLOCK_TEST_MONTH_SCALE + guestNow.Day, 20000101);
        ClockTestExpect("wrap: 00:00:03.9x (a tick short of :04)",
                        guestNow.Hour * CLOCK_TEST_HOUR_SCALE
                        + guestNow.Minute * CLOCK_TEST_MINUTE_SCALE + guestNow.Second, 3);
        ClockTestExpect("wrap: Saturday", guestNow.DayOfWeek, CLOCK_TEST_SATURDAY);

        /* ★ the trap the `since` argument exists for: the guest's own clock crossed
           midnight AFTER the store (it read 23:59:58 then; now 00:00:08 next day), and
           the count wrapped once. Taking "today" from host-now + offset would add the
           wrap to a day that already contains it -- 2000-01-02. DOS's day number is the
           store's day plus the wraps: 2000-01-01. */
        hostNow = ClockTestMake(2026, 10, 2, 9, 0, 0, 0); offset = 0;
        DosClockSetDate(&hostNow, &offset, 1999, 12, 31);
        DosClockSetTime(&hostNow, &offset, 23, 59, 58, 0);
        hostNow.Second = 10;                                    /* guest now 2000-01-01 00:00:08 */
        DosClockFollowTicks(&hostNow, &offset, DosClockTicksFromTime(0, 0, 5, 0), 1,
                            CLOCK_TEST_TICKS_10_SECONDS);
        DosClockApplyOffset(&hostNow, offset, &guestNow);
        ClockTestExpect("store before the guest's midnight: 2000-01-01, not 01-02",
                        guestNow.Year * CLOCK_TEST_YEAR_SCALE
                        + guestNow.Month * CLOCK_TEST_MONTH_SCALE + guestNow.Day, 20000101);
        ClockTestExpect("... at 00:00", guestNow.Hour * CLOCK_TEST_MINUTE_SCALE + guestNow.Minute,
                        0);

        /* no wrap, a store read the same instant: only the time of day moves */
        hostNow = ClockTestMake(2026, 10, 2, 23, 59, 59, 0); offset = 0;
        DosClockFollowTicks(&hostNow, &offset, DosClockTicksFromTime(1, 2, 3, 0), 0, 0);
        DosClockApplyOffset(&hostNow, offset, &guestNow);
        ClockTestExpect("store 01:02:03 at the host's 23:59:59 keeps the host's day",
                        guestNow.Year * CLOCK_TEST_YEAR_SCALE
                        + guestNow.Month * CLOCK_TEST_MONTH_SCALE + guestNow.Day, 20261002);
        ClockTestExpect("... and reads 01:02",
                        guestNow.Hour * CLOCK_TEST_MINUTE_SCALE + guestNow.Minute, 102);

        /* the day's length itself (a count the BIOS never holds) reads 23:59:59.85;
           a nonsense count past it is clamped to the last hundredth, never a next day */
        hostNow = ClockTestMake(2026, 10, 2, 9, 0, 0, 0); offset = 0;
        DosClockFollowTicks(&hostNow, &offset, CLOCK_TEST_TICKS_PER_DAY, 0, 0);
        DosClockApplyOffset(&hostNow, offset, &guestNow);
        ClockTestExpect("count 0x1800B0 -> 23:59:59.85",
                        guestNow.Hour * CLOCK_TEST_HMS_HOUR_SCALE
                        + guestNow.Minute * CLOCK_TEST_HMS_MINUTE_SCALE
                        + guestNow.Second * CLOCK_TEST_SECOND_SCALE + guestNow.Hundredths,
                        23595985);
        offset = 0;
        DosClockFollowTicks(&hostNow, &offset, CLOCK_TEST_TICKS_NONSENSE, 0, 0);
        DosClockApplyOffset(&hostNow, offset, &guestNow);
        ClockTestExpect("count FFFFFFFF -> 23:59:59.99, same day",
                        guestNow.Day * CLOCK_TEST_DAY_SCALE
                        + guestNow.Hour * CLOCK_TEST_HMS_HOUR_SCALE
                        + guestNow.Minute * CLOCK_TEST_HMS_MINUTE_SCALE
                        + guestNow.Second * CLOCK_TEST_SECOND_SCALE + guestNow.Hundredths,
                        223595999LL);
    }

    printf("== %d checks, %d failed\n", g_Checks, g_Failures);
    return g_Failures ? 1 : 0;
}
