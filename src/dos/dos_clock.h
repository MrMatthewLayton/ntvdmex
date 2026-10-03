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
 *   DOS's CLOCK$ driver WRITES BOTH when a program sets the time (so after 2Bh/2Dh
 *   the RTC agrees), but INT 1Ah AH=03h/05h set only the RTC, and DOS goes on
 *   answering from its own clock (measured, p_clock.asm clk.2c.after.1a03 /
 *   clk.2a.after.1a05). One offset would make an RTC set move DOS's clock too.
 *
 *   Pure C, no Win32: the host supplies "now" as fields, which is what lets the
 *   off-VM battery (tools/dostest/clock_test.c) pin every rule with exact instants.
 *   Resolution is the DOS one, hundredths of a second.
 */
#ifndef DOS_CLOCK_H
#define DOS_CLOCK_H

#include <stdint.h>

typedef struct {
    unsigned year, month, day;      /* full year (1999, not 99); month/day 1-based */
    unsigned hour, min, sec, cs;    /* cs = hundredths                              */
    unsigned dow;                   /* 0 = Sunday, as INT 21h AH=2Ah returns it     */
} dclk_t;

/* Centiseconds the guest is AHEAD of the host (negative = behind). Zero = the host. */
typedef struct {
    int64_t dos_off;                /* INT 21h AH=2Ah/2Ch -- DOS's clock              */
    int64_t rtc_off;                /* INT 1Ah AH=02h/04h and CMOS 00h-09h -- the RTC */
} dclk_state;

static inline int dclk_leap(unsigned y)
{ return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

static inline unsigned dclk_mdays(unsigned y, unsigned m)
{
    static const unsigned char d[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    if (m < 1 || m > 12) return 0;
    return (m == 2 && dclk_leap(y)) ? 29u : d[m - 1];
}

/* A calendar date at all (any year 1..9999). */
static inline int dclk_date_real(unsigned y, unsigned m, unsigned d)
{ return y >= 1 && y <= 9999 && m >= 1 && m <= 12 && d >= 1 && d <= dclk_mdays(y, m); }

/* ── WHAT INT 21h AH=2Bh ACCEPTS. ────────────────────────────────────────────────
     RBIL: CX = 1980..2099. Measured by p_clock.asm on 6.22 under QEMU and on PCem's
     AMI 486: 1979 and 2100 are refused with AL=FFh, 2099-12-31 is taken, and the
     calendar is the Gregorian one -- 2000-02-29 is taken (divisible by 400);
     2001-02-29, Feb 30, Apr 31, month 0/13 and day 0 are refused. DOSBox-X agrees on
     everything but 2100, which it takes (abstained in oracle-rules.json: two Microsoft
     kernels and the spec against one emulator). DOS's date is a day count from
     1980-01-01, which is where the lower bound comes from. */
static inline int dclk_dos_date_ok(unsigned y, unsigned m, unsigned d)
{ return y >= 1980 && y <= 2099 && dclk_date_real(y, m, d); }

/* INT 21h AH=2Dh: hour 0-23, minute 0-59, second 0-59, hundredths 0-99. */
static inline int dclk_time_ok(unsigned h, unsigned mi, unsigned s, unsigned cs)
{ return h < 24 && mi < 60 && s < 60 && cs < 100; }

/* Days since 0000-03-01 in the proleptic Gregorian calendar (Hinnant's algorithm).
   Only DIFFERENCES are ever used, so the epoch is arbitrary. */
static inline int64_t dclk_days(unsigned y, unsigned m, unsigned d)
{
    int64_t yy = (int64_t)y - (m <= 2 ? 1 : 0);
    int64_t era = yy / 400;                          /* y >= 1, so no negative floor */
    int64_t yoe = yy - era * 400;
    int64_t mp  = (m > 2) ? (int64_t)m - 3 : (int64_t)m + 9;
    int64_t doy = (153 * mp + 2) / 5 + (int64_t)d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe;
}

#define DCLK_CS_PER_DAY ((int64_t)24 * 60 * 60 * 100)

static inline int64_t dclk_pack(const dclk_t *t)
{
    return dclk_days(t->year, t->month, t->day) * DCLK_CS_PER_DAY
         + (((int64_t)t->hour * 60 + t->min) * 60 + t->sec) * 100 + t->cs;
}

static inline void dclk_unpack(int64_t v, dclk_t *t)
{
    int64_t days = v / DCLK_CS_PER_DAY, rem = v % DCLK_CS_PER_DAY;
    int64_t era, doe, yoe, y, doy, mp;
    if (rem < 0) { rem += DCLK_CS_PER_DAY; --days; }
    t->cs   = (unsigned)(rem % 100); rem /= 100;
    t->sec  = (unsigned)(rem % 60);  rem /= 60;
    t->min  = (unsigned)(rem % 60);  rem /= 60;
    t->hour = (unsigned)rem;
    /* day 0 (0000-03-01) was a Wednesday */
    t->dow  = (unsigned)(((days % 7) + 7 + 3) % 7);
    era = (days >= 0 ? days : days - 146096) / 146097;
    doe = days - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y   = yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp  = (5 * doy + 2) / 153;
    t->day   = (unsigned)(doy - (153 * mp + 2) / 5 + 1);
    t->month = (unsigned)(mp < 10 ? mp + 3 : mp - 9);
    t->year  = (unsigned)(y + (t->month <= 2 ? 1 : 0));
}

/* The guest's reading: the host's now, moved by `off`. */
static inline void dclk_read(const dclk_t *host, int64_t off, dclk_t *out)
{ dclk_unpack(dclk_pack(host) + off, out); }

/* Set the DATE and keep the guest's time of day: the offset becomes whatever makes
   host-now read as (y, m, d, <current guest time>). The caller has validated. */
static inline void dclk_set_date(const dclk_t *host, int64_t *off,
                                 unsigned y, unsigned m, unsigned d)
{
    dclk_t g;
    dclk_read(host, *off, &g);
    g.year = y; g.month = m; g.day = d;
    *off = dclk_pack(&g) - dclk_pack(host);
}

/* Set the TIME and keep the guest's date. */
static inline void dclk_set_time(const dclk_t *host, int64_t *off,
                                 unsigned h, unsigned mi, unsigned s, unsigned cs)
{
    dclk_t g;
    dclk_read(host, *off, &g);
    g.hour = h; g.min = mi; g.sec = s; g.cs = cs;
    *off = dclk_pack(&g) - dclk_pack(host);
}

/* BIOS ticks since midnight for a time of day: centiseconds * 1193182 / (65536*100),
   the same rule as pit_ticks_since_midnight but at DOS's finer resolution.
   ⚠ 23:59:59.99 lands EXACTLY on the BIOS's day length (0x1800B0), a count the BIOS
     itself never holds -- it wraps to 0 and sets the midnight flag on reaching it.
     Clamped one short, so the next tick is the one that turns the day over. */
static inline uint32_t dclk_ticks(unsigned h, unsigned mi, unsigned s, unsigned cs)
{
    uint64_t c = (((uint64_t)h * 60 + mi) * 60 + s) * 100 + cs;
    uint32_t t = (uint32_t)((c * 1193182u) / 6553600u);
    return t < 0x1800B0u ? t : 0x1800AFu;
}

/* The other direction (GH #262): the time of day a BIOS tick count stands for, as
   DOS's CLOCK$ reads it -- centiseconds = ticks * 65536 * 100 / 1193182. A count at
   or past a day's length is clamped to the last centisecond of it. */
static inline void dclk_from_ticks(uint32_t ticks, unsigned *h, unsigned *mi,
                                   unsigned *s, unsigned *cs)
{
    uint64_t c = ((uint64_t)ticks * 6553600u) / 1193182u;
    if (c >= (uint64_t)DCLK_CS_PER_DAY) c = (uint64_t)DCLK_CS_PER_DAY - 1;
    *cs = (unsigned)(c % 100); c /= 100;
    *s  = (unsigned)(c % 60);  c /= 60;
    *mi = (unsigned)(c % 60);  c /= 60;
    *h  = (unsigned)c;
}

/* The one clock the whole VDM shares (defined in dos_int21.c). */
extern dclk_state g_dos_clock;

#endif /* DOS_CLOCK_H */
