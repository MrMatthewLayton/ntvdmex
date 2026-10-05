/* dos_clock.h -- the VDM's own clock: what DOS and the RTC say the time is. (GH #250)
 *
 * ── ★ A GUEST MAY SET THE DATE AND TIME; THE HOST'S CLOCK MUST NOT MOVE. ──────────
 *   INT 21h AH=2Bh/2Dh used to answer AL=00 ("done") and change nothing, so a program
 *   that set the date and read it back got today -- the "runs but lies" shape. INT 1Ah
 *   AH=03h/05h and the CMOS clock registers refused outright, for the stated reason
 *   that "we cannot move the host's clock". Both halves were right about the
 *   constraint and wrong about the consequence: the VDM does not need the HOST clock
 *   to move, it needs ITS OWN clock to. So the VDM keeps an OFFSET from the host's
 *   local time, and every reading is host-now + offset. Setting the date or time
 *   moves the offset; nothing ever calls SetLocalTime/SetSystemTime (which would also
 *   need SE_SYSTEMTIME_NAME, and would change the clock under every other program on
 *   the machine). The offset dies with the VDM, like a real machine's DOS clock dies
 *   at power-off when nobody wrote the RTC -- except here nobody CAN write the RTC.
 *
 * ── TWO OFFSETS, BECAUSE AN AT HAS TWO CLOCKS. ─────────────────────────────────
 *   DOS's time comes from the BIOS tick count; the MC146818 RTC runs on its own.
 *   (Ours comes from host-now + DosOffset, re-derived FROM the count whenever the count
 *   was set by anything but the BIOS -- INT 1Ah AH=01h or a raw store, GH #262 --
 *   see DosClockFollowTicks. Both offsets read the one host clock: there is no second
 *   time source to drift, only two views a guest can set separately, as on an AT.)
 *   DOS's CLOCK$ driver WRITES BOTH when a program sets the time (so after 2Bh/2Dh
 *   the RTC agrees), but INT 1Ah AH=03h/05h set only the RTC, and DOS goes on
 *   answering from its own clock (measured, p_clock.asm clk.2c.after.1a03 /
 *   clk.2a.after.1a05). One offset would make an RTC set move DOS's clock too.
 *
 *   Pure C, no Win32 calls (only Windows types, src/ntvdmex_types.h): the host supplies
 *   "now" as fields, which is what lets the off-VM battery (tests/unit/clock_test.c) pin
 *   every rule with exact instants. Resolution is the DOS one, hundredths of a second.
 */
#ifndef NTVDMEX_DOS_CLOCK_H
#define NTVDMEX_DOS_CLOCK_H

#include "../ntvdmex_types.h"

typedef struct _DOS_CLOCK_TIME {
    UINT Year, Month, Day;              /* full year (1999, not 99); month/day 1-based */
    UINT Hour, Minute, Second, Hundredths;
    UINT DayOfWeek;                     /* 0 = Sunday, as INT 21h AH=2Ah returns it     */
} DOS_CLOCK_TIME, *PDOS_CLOCK_TIME;

typedef const DOS_CLOCK_TIME *PCDOS_CLOCK_TIME;

/* Centiseconds the guest is AHEAD of the host (negative = behind). Zero = the host. */
typedef struct _DOS_CLOCK_STATE {
    INT64 DosOffset;                    /* INT 21h AH=2Ah/2Ch -- DOS's clock              */
    INT64 RtcOffset;                    /* INT 1Ah AH=02h/04h and CMOS 00h-09h -- the RTC */
} DOS_CLOCK_STATE, *PDOS_CLOCK_STATE;

/* The calendar. */
#define DOS_CLOCK_JANUARY              1
#define DOS_CLOCK_FEBRUARY             2
#define DOS_CLOCK_DECEMBER             12
#define DOS_CLOCK_MONTHS_PER_YEAR      12
#define DOS_CLOCK_FIRST_DAY            1
#define DOS_CLOCK_LEAP_FEBRUARY_DAYS   29u
#define DOS_CLOCK_FIRST_YEAR           1
#define DOS_CLOCK_LAST_YEAR            9999
#define DOS_CLOCK_LEAP_YEAR_INTERVAL   4
#define DOS_CLOCK_YEARS_PER_CENTURY    100
#define DOS_CLOCK_YEARS_PER_ERA        400
#define DOS_CLOCK_DAYS_PER_YEAR        365
#define DOS_CLOCK_DAYS_PER_WEEK        7
#define DOS_CLOCK_HOURS_PER_DAY        24
#define DOS_CLOCK_MINUTES_PER_HOUR     60
#define DOS_CLOCK_SECONDS_PER_MINUTE   60
#define DOS_CLOCK_HUNDREDTHS_PER_SECOND 100

/* The day-count arithmetic below (Hinnant's), which counts each year from 1 March. */
#define DOS_CLOCK_DAYS_PER_ERA                 146097
#define DOS_CLOCK_DAYS_PER_ERA_LESS_ONE        146096
#define DOS_CLOCK_DAYS_PER_CENTURY             36524
#define DOS_CLOCK_DAYS_PER_FOUR_YEARS_LESS_ONE 1460
#define DOS_CLOCK_MARCH_SHIFT                  3
#define DOS_CLOCK_MARCH_WRAP                   9
#define DOS_CLOCK_SHIFTED_JANUARY              10
#define DOS_CLOCK_MONTH_DAYS_NUMERATOR         153
#define DOS_CLOCK_MONTH_DAYS_ROUNDING          2
#define DOS_CLOCK_MONTH_DAYS_DENOMINATOR       5
#define DOS_CLOCK_DAY0_WEEKDAY                 3    /* day 0 (0000-03-01) was a Wednesday */

static inline BOOL DosClockIsLeapYear(_In_ UINT year)
{
    return (year % DOS_CLOCK_LEAP_YEAR_INTERVAL == 0 && year % DOS_CLOCK_YEARS_PER_CENTURY != 0)
        || year % DOS_CLOCK_YEARS_PER_ERA == 0;
}

static inline UINT DosClockDaysInMonth(_In_ UINT year, _In_ UINT month)
{
    static const BYTE daysInMonth[DOS_CLOCK_MONTHS_PER_YEAR] =
        { 31,28,31,30,31,30,31,31,30,31,30,31 };
    if (month < DOS_CLOCK_JANUARY || month > DOS_CLOCK_DECEMBER) return 0;
    return (month == DOS_CLOCK_FEBRUARY && DosClockIsLeapYear(year)) ? DOS_CLOCK_LEAP_FEBRUARY_DAYS
                                                                     : daysInMonth[month - 1];
}

/* A calendar date at all (any year 1..9999). */
static inline BOOL DosClockIsRealDate(_In_ UINT year, _In_ UINT month, _In_ UINT day)
{
    return year >= DOS_CLOCK_FIRST_YEAR && year <= DOS_CLOCK_LAST_YEAR
        && month >= DOS_CLOCK_JANUARY && month <= DOS_CLOCK_DECEMBER
        && day >= DOS_CLOCK_FIRST_DAY && day <= DosClockDaysInMonth(year, month);
}

/* ── WHAT INT 21h AH=2Bh ACCEPTS. ────────────────────────────────────────────────
     RBIL: CX = 1980..2099. Measured by p_clock.asm on 6.22 under QEMU and on PCem's
     AMI 486: 1979 and 2100 are refused with AL=FFh, 2099-12-31 is taken, and the
     calendar is the Gregorian one -- 2000-02-29 is taken (divisible by 400);
     2001-02-29, Feb 30, Apr 31, month 0/13 and day 0 are refused. DOSBox-X agrees on
     everything but 2100, which it takes (abstained in oracle-rules.json: two Microsoft
     kernels and the spec against one emulator). DOS's date is a day count from
     1980-01-01, which is where the lower bound comes from. */
#define DOS_CLOCK_DOS_FIRST_YEAR       1980
#define DOS_CLOCK_DOS_LAST_YEAR        2099

static inline BOOL DosClockIsDosDateValid(_In_ UINT year, _In_ UINT month, _In_ UINT day)
{
    return year >= DOS_CLOCK_DOS_FIRST_YEAR && year <= DOS_CLOCK_DOS_LAST_YEAR
        && DosClockIsRealDate(year, month, day);
}

/* INT 21h AH=2Dh: hour 0-23, minute 0-59, second 0-59, hundredths 0-99. */
static inline BOOL DosClockIsTimeValid(_In_ UINT hour, _In_ UINT minute, _In_ UINT second,
                                       _In_ UINT hundredths)
{
    return hour < DOS_CLOCK_HOURS_PER_DAY && minute < DOS_CLOCK_MINUTES_PER_HOUR
        && second < DOS_CLOCK_SECONDS_PER_MINUTE && hundredths < DOS_CLOCK_HUNDREDTHS_PER_SECOND;
}

/* Days since 0000-03-01 in the proleptic Gregorian calendar (Hinnant's algorithm).
   Only DIFFERENCES are ever used, so the epoch is arbitrary. */
static inline INT64 DosClockDays(_In_ UINT year, _In_ UINT month, _In_ UINT day)
{
    INT64 shiftedYear = (INT64)year - (month <= DOS_CLOCK_FEBRUARY ? 1 : 0);
    INT64 era = shiftedYear / DOS_CLOCK_YEARS_PER_ERA;   /* year >= 1, so no negative floor */
    INT64 yearOfEra = shiftedYear - era * DOS_CLOCK_YEARS_PER_ERA;
    INT64 shiftedMonth = (month > DOS_CLOCK_FEBRUARY) ? (INT64)month - DOS_CLOCK_MARCH_SHIFT
                                                      : (INT64)month + DOS_CLOCK_MARCH_WRAP;
    INT64 dayOfYear = (DOS_CLOCK_MONTH_DAYS_NUMERATOR * shiftedMonth
                       + DOS_CLOCK_MONTH_DAYS_ROUNDING)
                      / DOS_CLOCK_MONTH_DAYS_DENOMINATOR + (INT64)day - 1;
    INT64 dayOfEra = yearOfEra * DOS_CLOCK_DAYS_PER_YEAR + yearOfEra / DOS_CLOCK_LEAP_YEAR_INTERVAL
                     - yearOfEra / DOS_CLOCK_YEARS_PER_CENTURY + dayOfYear;
    return era * DOS_CLOCK_DAYS_PER_ERA + dayOfEra;
}

#define DOS_CLOCK_HUNDREDTHS_PER_DAY \
    ((INT64)DOS_CLOCK_HOURS_PER_DAY * DOS_CLOCK_MINUTES_PER_HOUR * DOS_CLOCK_SECONDS_PER_MINUTE \
     * DOS_CLOCK_HUNDREDTHS_PER_SECOND)

static inline INT64 DosClockPack(_In_ PCDOS_CLOCK_TIME time)
{
    return DosClockDays(time->Year, time->Month, time->Day) * DOS_CLOCK_HUNDREDTHS_PER_DAY
         + (((INT64)time->Hour * DOS_CLOCK_MINUTES_PER_HOUR + time->Minute)
            * DOS_CLOCK_SECONDS_PER_MINUTE + time->Second) * DOS_CLOCK_HUNDREDTHS_PER_SECOND
         + time->Hundredths;
}

static inline VOID DosClockUnpack(_In_ INT64 packed, _Out_ PDOS_CLOCK_TIME time)
{
    INT64 days = packed / DOS_CLOCK_HUNDREDTHS_PER_DAY,
          remainder = packed % DOS_CLOCK_HUNDREDTHS_PER_DAY;
    INT64 era, dayOfEra, yearOfEra, year, dayOfYear, shiftedMonth;
    if (remainder < 0) { remainder += DOS_CLOCK_HUNDREDTHS_PER_DAY; --days; }
    time->Hundredths = (UINT)(remainder % DOS_CLOCK_HUNDREDTHS_PER_SECOND);
    remainder /= DOS_CLOCK_HUNDREDTHS_PER_SECOND;
    time->Second = (UINT)(remainder % DOS_CLOCK_SECONDS_PER_MINUTE);
    remainder /= DOS_CLOCK_SECONDS_PER_MINUTE;
    time->Minute = (UINT)(remainder % DOS_CLOCK_MINUTES_PER_HOUR);
    remainder /= DOS_CLOCK_MINUTES_PER_HOUR;
    time->Hour = (UINT)remainder;
    /* day 0 (0000-03-01) was a Wednesday */
    time->DayOfWeek = (UINT)(((days % DOS_CLOCK_DAYS_PER_WEEK) + DOS_CLOCK_DAYS_PER_WEEK
                              + DOS_CLOCK_DAY0_WEEKDAY) % DOS_CLOCK_DAYS_PER_WEEK);
    era = (days >= 0 ? days : days - DOS_CLOCK_DAYS_PER_ERA_LESS_ONE) / DOS_CLOCK_DAYS_PER_ERA;
    dayOfEra = days - era * DOS_CLOCK_DAYS_PER_ERA;
    yearOfEra = (dayOfEra - dayOfEra / DOS_CLOCK_DAYS_PER_FOUR_YEARS_LESS_ONE
                 + dayOfEra / DOS_CLOCK_DAYS_PER_CENTURY
                 - dayOfEra / DOS_CLOCK_DAYS_PER_ERA_LESS_ONE) / DOS_CLOCK_DAYS_PER_YEAR;
    year = yearOfEra + era * DOS_CLOCK_YEARS_PER_ERA;
    dayOfYear = dayOfEra - (DOS_CLOCK_DAYS_PER_YEAR * yearOfEra
                            + yearOfEra / DOS_CLOCK_LEAP_YEAR_INTERVAL
                            - yearOfEra / DOS_CLOCK_YEARS_PER_CENTURY);
    shiftedMonth = (DOS_CLOCK_MONTH_DAYS_DENOMINATOR * dayOfYear + DOS_CLOCK_MONTH_DAYS_ROUNDING)
                   / DOS_CLOCK_MONTH_DAYS_NUMERATOR;
    time->Day   = (UINT)(dayOfYear - (DOS_CLOCK_MONTH_DAYS_NUMERATOR * shiftedMonth
                                      + DOS_CLOCK_MONTH_DAYS_ROUNDING)
                                     / DOS_CLOCK_MONTH_DAYS_DENOMINATOR + 1);
    time->Month = (UINT)(shiftedMonth < DOS_CLOCK_SHIFTED_JANUARY
                         ? shiftedMonth + DOS_CLOCK_MARCH_SHIFT
                         : shiftedMonth - DOS_CLOCK_MARCH_WRAP);
    time->Year  = (UINT)(year + (time->Month <= DOS_CLOCK_FEBRUARY ? 1 : 0));
}

/* The guest's reading: the host's now, moved by `offset`. */
static inline VOID DosClockApplyOffset(_In_ PCDOS_CLOCK_TIME hostNow, _In_ INT64 offset,
                                       _Out_ PDOS_CLOCK_TIME guestNow)
{ DosClockUnpack(DosClockPack(hostNow) + offset, guestNow); }

/* Set the DATE and keep the guest's time of day: the offset becomes whatever makes
   host-now read as (year, month, day, <current guest time>). The caller has validated. */
static inline VOID DosClockSetDate(_In_ PCDOS_CLOCK_TIME hostNow, _Inout_ PINT64 offset,
                                   _In_ UINT year, _In_ UINT month, _In_ UINT day)
{
    DOS_CLOCK_TIME guestNow;
    DosClockApplyOffset(hostNow, *offset, &guestNow);
    guestNow.Year = year; guestNow.Month = month; guestNow.Day = day;
    *offset = DosClockPack(&guestNow) - DosClockPack(hostNow);
}

/* Set the TIME and keep the guest's date. */
static inline VOID DosClockSetTime(_In_ PCDOS_CLOCK_TIME hostNow, _Inout_ PINT64 offset,
                                   _In_ UINT hour, _In_ UINT minute, _In_ UINT second,
                                   _In_ UINT hundredths)
{
    DOS_CLOCK_TIME guestNow;
    DosClockApplyOffset(hostNow, *offset, &guestNow);
    guestNow.Hour = hour; guestNow.Minute = minute; guestNow.Second = second;
    guestNow.Hundredths = hundredths;
    *offset = DosClockPack(&guestNow) - DosClockPack(hostNow);
}

/* The BIOS tick rate: centiseconds * 1193182 / (65536*100) ticks. */
#define DOS_CLOCK_PIT_HZ               1193182u
#define DOS_CLOCK_PIT_DIVISOR_CS       6553600u     /* 65536 * 100 */
#define DOS_CLOCK_TICKS_PER_DAY        0x1800B0u    /* the BIOS's day length (below) */
#define DOS_CLOCK_LAST_TICK_OF_DAY     0x1800AFu

/* BIOS ticks since midnight for a time of day: centiseconds * 1193182 / (65536*100),
   the same rule as pit_ticks_since_midnight but at DOS's finer resolution.
   ⚠ 23:59:59.99 lands EXACTLY on the BIOS's day length (0x1800B0), a count the BIOS
     itself never holds -- it wraps to 0 and sets the midnight flag on reaching it.
     Clamped one short, so the next tick is the one that turns the day over. */
static inline DWORD DosClockTicksFromTime(_In_ UINT hour, _In_ UINT minute, _In_ UINT second,
                                          _In_ UINT hundredths)
{
    UINT64 totalHundredths = (((UINT64)hour * DOS_CLOCK_MINUTES_PER_HOUR + minute)
                              * DOS_CLOCK_SECONDS_PER_MINUTE + second)
                             * DOS_CLOCK_HUNDREDTHS_PER_SECOND + hundredths;
    DWORD ticks = (DWORD)((totalHundredths * DOS_CLOCK_PIT_HZ) / DOS_CLOCK_PIT_DIVISOR_CS);
    return ticks < DOS_CLOCK_TICKS_PER_DAY ? ticks : DOS_CLOCK_LAST_TICK_OF_DAY;
}

/* The other direction (GH #262): the time of day a BIOS tick count stands for, as
   DOS's CLOCK$ reads it -- centiseconds = ticks * 65536 * 100 / 1193182. A count at
   or past a day's length is clamped to the last centisecond of it. */
static inline UINT64 DosClockHundredthsFromTicks(_In_ DWORD ticks)
{ return ((UINT64)ticks * DOS_CLOCK_PIT_DIVISOR_CS) / DOS_CLOCK_PIT_HZ; }

static inline VOID DosClockTimeFromTicks(_In_ DWORD ticks, _Out_ PUINT hour, _Out_ PUINT minute,
                                         _Out_ PUINT second, _Out_ PUINT hundredths)
{
    UINT64 elapsed = DosClockHundredthsFromTicks(ticks);
    if (elapsed >= (UINT64)DOS_CLOCK_HUNDREDTHS_PER_DAY)
        elapsed = (UINT64)DOS_CLOCK_HUNDREDTHS_PER_DAY - 1;
    *hundredths = (UINT)(elapsed % DOS_CLOCK_HUNDREDTHS_PER_SECOND);
    elapsed /= DOS_CLOCK_HUNDREDTHS_PER_SECOND;
    *second = (UINT)(elapsed % DOS_CLOCK_SECONDS_PER_MINUTE);
    elapsed /= DOS_CLOCK_SECONDS_PER_MINUTE;
    *minute = (UINT)(elapsed % DOS_CLOCK_MINUTES_PER_HOUR);
    elapsed /= DOS_CLOCK_MINUTES_PER_HOUR;
    *hour = (UINT)elapsed;
}

/* ── GH #262 CASE B: DOS'S CLOCK FOLLOWS A TICK COUNT THE BIOS DID NOT WRITE. ─────────
     On MS-DOS, AH=2Ah/2Ch go through CLOCK$, which reads INT 1Ah AH=00h: the time of
     day IS the count at 0040:006C, and the date is DOS's own day number, advanced by
     one each time the call hands back the midnight flag. So a program that stores to
     006C directly moves DOS's time (p_tick2c tick2c.after.store, 6.22 / PCem /
     DOSBox-X). Ours keeps DOS's clock as host-now + DosOffset, which no lost tick can
     slow; the PIT tells us when the count stopped being one the BIOS produced (a
     WITNESS of the last value it wrote -- vdd_pit.h) and only then is the offset
     re-derived from the count. A VDM nobody stores into never comes here.

     ticks   the count now (what CLOCK$ would read)
     wraps   BIOS midnight rollovers since the foreign store was first seen
     since   BIOS ticks counted since then -- how long ago the store was, measured in
             the guest's own ticks, so the DATE is the day the store was made on (DOS's
             day number), not whatever day host-now has reached since.

     target = midnight of (guest-now - since) + wraps days + the count's time of day.

   ⚠ APPROXIMATIONS, ALL BELOW WHAT ANY PROBE CAN SEE:
       - the count -> time conversion floors to the hundredth (DosClockHundredthsFromTicks),
         so a reading is exact only to a tick (~5.49 cs) -- the same granularity real DOS
         has, but 6.22's own CLOCK$ arithmetic is NOT disassembled here; whether it
         rounds or floors the hundredths is UNMEASURED (p_tick2c compares CX only);
       - `since` can be one tick short (a store between two ticks is seen at the
         second), which matters only if the store was made within ~55 ms of midnight;
       - every wrap counts a day. An AT BIOS sets the flag to 1, not a count, so a real
         DOS that went two days without reading the clock loses one; we do not. */
static inline VOID DosClockFollowTicks(_In_ PCDOS_CLOCK_TIME hostNow, _Inout_ PINT64 offset,
                                       _In_ DWORD ticks, _In_ UINT wraps, _In_ DWORD since)
{
    INT64 hostPacked = DosClockPack(hostNow);
    INT64 atStore = hostPacked + *offset - (INT64)DosClockHundredthsFromTicks(since);
    /* pack is never negative */
    INT64 storeMidnight = atStore - (atStore % DOS_CLOCK_HUNDREDTHS_PER_DAY);
    UINT64 timeOfDay = DosClockHundredthsFromTicks(ticks);
    if (timeOfDay >= (UINT64)DOS_CLOCK_HUNDREDTHS_PER_DAY)
        timeOfDay = (UINT64)DOS_CLOCK_HUNDREDTHS_PER_DAY - 1;
    *offset = storeMidnight + (INT64)wraps * DOS_CLOCK_HUNDREDTHS_PER_DAY + (INT64)timeOfDay
              - hostPacked;
}

/* The one clock the whole VDM shares (defined in dos_int21.c). */
extern DOS_CLOCK_STATE g_DosClock;

#endif /* NTVDMEX_DOS_CLOCK_H */
