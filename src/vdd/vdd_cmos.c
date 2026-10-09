/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * See vdd_cmos.h.  MC146818 RTC + CMOS RAM on the VDD bus.  Pure C.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include "vdd_cmos.h"

/* Status B's bits (MC146818; docs/ref/rtc.md 2). */
#define CMOS_B_SET                  0x80    /* Hold updates: the clock is being set */
#define CMOS_B_PIE                  0x40    /* Periodic interrupt enable */
#define CMOS_B_AIE                  0x20    /* alarm interrupt enable */
#define CMOS_B_UIE                  0x10    /* Update-ended interrupt enable */
#define CMOS_B_UIE_OR_AIE           0x30
#define CMOS_B_BINARY               0x04    /* DM: binary, not BCD */
#define CMOS_B_24_HOUR              0x02    /* 24/12: 24-hour */

/* Status C's flags. */
#define CMOS_C_IRQF                 0x80    /* An interrupt is pending */
#define CMOS_C_PF_AND_IRQF          0xC0    /* Periodic flag, and IRQF with it */
#define CMOS_C_AF                   0x20    /* alarm flag */
#define CMOS_C_UF                   0x10    /* Update-ended flag */

/* Status A and D. */
#define CMOS_A_RATE_MASK            0x0F    /* RS3:0, the periodic rate select */
#define CMOS_A_UIP                  0x80    /* Update in progress (read-only) */
#define CMOS_A_WRITABLE_BITS        0x7F    /* Everything but UIP */
#define CMOS_HOUR_VALUE_BITS        0x7F    /* 12-hour mode: the hours register less PM */
#define CMOS_D_VRT                  0x80    /* Valid RAM and time */
#define CMOS_REGISTER_MASK          0x7F    /* port 70h bits 6:0: the register number */
#define CMOS_NMI_MASK_BIT           0x80    /* port 70h bit 7: the NMI mask */
#define CMOS_ALARM_DONT_CARE        0xC0    /* An alarm field with both top bits set */
#define CMOS_HOUR_PM_BIT            0x80    /* 12-hour mode: the hours register's PM bit */
#define CMOS_HOUR_AM                0x00

/* The periodic rates (docs/ref/rtc.md 2): RS=1 and 2 are special, then 32768 >> (RS-1). */
#define CMOS_RATE_NONE              0
#define CMOS_RATE_SELECT_256HZ      1
#define CMOS_RATE_SELECT_128HZ      2
#define CMOS_RATE_256HZ             256
#define CMOS_RATE_128HZ             128
#define CMOS_TIME_BASE_HZ           32768u
#define CMOS_RTC_IRQ                8
#define CMOS_SECOND_EDGE_GUARD      1000    /* Most second edges caught up in one call */
#define CMOS_PERIODIC_GUARD         100000  /* Most periodic ticks caught up in one call */

/* BCD and the clock. */
#define CMOS_DECIMAL_BASE           10
#define CMOS_BCD_TENS_WEIGHT        16
#define CMOS_BCD_TENS_SHIFT         4
#define CMOS_BCD_TENS_BASE          10u
#define CMOS_BCD_UNITS_MASK         0x0Fu
#define CMOS_HOURS_PER_HALF_DAY     12
#define CMOS_HALF_DAY_HOURS         12u
#define CMOS_NO_HOURS               0u
#define CMOS_DEFAULT_CENTURY        20      /* The reading's defaults before the host fills it */
#define CMOS_DEFAULT_MONTH          1
#define CMOS_DEFAULT_DAY            1

/* Ports. */
#define CMOS_INDEX_PORT             0x70
#define CMOS_DATA_PORT              0x71
#define CMOS_UNDRIVEN_BUS           0xFF

/* What POST leaves (docs/ref/rtc.md 3; see VddCmosReset). */
#define CMOS_POST_STATUS_A          0x26    /* 32.768 kHz divider, 1024 Hz rate select */
#define CMOS_POST_STATUS_B          0x02    /* BCD, 24-hour, every interrupt disabled */
#define CMOS_POST_DAY_OF_WEEK       0x01
#define CMOS_DIAGNOSTIC             0x0E
#define CMOS_POST_DIAGNOSTIC        0x00    /* No errors */
#define CMOS_FLOPPY_TYPES           0x10
#define CMOS_ONE_1440K_FLOPPY       0x40
#define CMOS_POST_EQUIPMENT         0x25    /* 1 floppy, colour 80x25, FPU */
#define CMOS_BASE_KB_LOW            0x15
#define CMOS_BASE_KB_HIGH           0x16
#define CMOS_DEFAULT_BASE_KB        640u
#define CMOS_EXTENDED_KB_LOW        0x17    /* Configured */
#define CMOS_EXTENDED_KB_HIGH       0x18
#define CMOS_POST_EXTENDED_LOW      0x30    /* POST-detected */
#define CMOS_POST_EXTENDED_HIGH     0x31
#define CMOS_CHECKSUM_FIRST         0x10
#define CMOS_CHECKSUM_LAST          0x2D
#define CMOS_CHECKSUM_HIGH          0x2E
#define CMOS_CHECKSUM_LOW           0x2F
#define CMOS_LOW_BYTE               0xFF
#define CMOS_OK                     0
#define CMOS_FAILED                 (-1)
#define CMOS_NOT_A_CLOCK_REGISTER   0
#define CMOS_CLOCK_REGISTER         1

static BYTE CmosToBcd(UINT value)
{
    return (BYTE)(((value / CMOS_DECIMAL_BASE) % CMOS_DECIMAL_BASE) * CMOS_BCD_TENS_WEIGHT + (value % CMOS_DECIMAL_BASE));
}

/* A clock field, in whichever base Status B bit 2 (DM) selects.
 *
 * [CAUTION]: HONOURING DM MATTERS MORE THAN IT LOOKS. A PC BIOS leaves DM=0 (BCD) and
 * nothing on the shelf changes it -- but a model that ignores the bit hands a
 * guest that DID set binary mode a BCD byte, and 0x59 seconds then reads as
 * eighty-nine. Refusing the write would be the "runs but lies" shape; honouring
 * it is two lines.
 */
static BYTE CmosClockValue(PCCMOS_STATE state, UINT value)
{
    return (state->StatusB & CMOS_B_BINARY) ? (BYTE)value : CmosToBcd(value);
}

/* The other direction: a byte the guest wrote, in the base DM says it is in. */
static UINT CmosClockDecode(PCCMOS_STATE state, BYTE value)
{
    return (state->StatusB & CMOS_B_BINARY) ? value : (UINT)(value >> CMOS_BCD_TENS_SHIFT) * CMOS_BCD_TENS_BASE + (value & CMOS_BCD_UNITS_MASK);
}

/* The clock as the chip shows it now: the frozen copy while SET is held. */
static INT CmosReading(PCMOS_STATE state, PIT_RTC_READING *reading)
{
    if (state->IsSetHeld)
    {
        *reading = state->Shadow;
        return TRUE;
    }

    if (!state->RtcNow)
        return FALSE;

    reading->Century = CMOS_DEFAULT_CENTURY;
    reading->Year = 0;
    reading->Month = CMOS_DEFAULT_MONTH;
    reading->Day = CMOS_DEFAULT_DAY;
    reading->Hour = 0;
    reading->Minute = 0;
    reading->Second = 0;
    reading->DayOfWeek = 0;
    state->RtcNow(state->RtcContext, reading);
    return TRUE;
}

/* THE PERIODIC RATE, FROM STATUS A BITS 3:0. (docs/ref/rtc.md 2):
 * 0 selects none. 1 and 2 are special cases at 256 Hz and 128 Hz; from 3 up
 * the rate is 32768 >> (RS - 1), so RS=3 is 8192 Hz, RS=6 is 1024 Hz (what a
 * PC BIOS leaves) and RS=15 is 2 Hz.
 */
UINT32 VddCmosPeriodicHz(PCCMOS_STATE state)
{
    UINT rateSelect = state->StatusA & CMOS_A_RATE_MASK;

    if (!rateSelect)
        return CMOS_RATE_NONE;

    if (rateSelect == CMOS_RATE_SELECT_256HZ)
        return CMOS_RATE_256HZ;

    if (rateSelect == CMOS_RATE_SELECT_128HZ)
        return CMOS_RATE_128HZ;

    return CMOS_TIME_BASE_HZ >> (rateSelect - 1);
}

/* THE ALARM MATCH RULE, WHICH IS NOT "EQUAL":
 * An alarm register whose top two bits are both set (>= 0xC0) is a DON'T CARE
 * and matches anything -- that is how "every minute at 30 seconds" is
 * programmed, and a model that only compares for equality can never express
 * it. (MC146818 datasheet, the alarm registers.)
 */
static INT CmosAlarmFieldMatches(BYTE alarm, BYTE current)
{
    return (alarm & CMOS_ALARM_DONT_CARE) == CMOS_ALARM_DONT_CARE || alarm == current;
}

/* THE CLOCK REGISTERS ARE DERIVED, NOT STORED:
 * Keeping a copy and ticking it would be a second clock to drift from the one
 * INT 1Ah reads. docs/ref/rtc.md 2: a PC BIOS leaves Status B with DM=0 and
 * 24/12=1, i.e. **BCD, 24-hour**, and every DOS program that reads this chip
 * by hand assumes exactly that -- so 0x12 in the hours register is twelve
 * o'clock, not eighteen.
 */
static INT CmosClockRegister(PCMOS_STATE state, BYTE registerIndex, BYTE *value)
{
    PIT_RTC_READING reading;

    if (!CmosReading(state, &reading))
        return CMOS_NOT_A_CLOCK_REGISTER;

    switch (registerIndex)
    {
    case CMOS_SECONDS:
        *value = CmosClockValue(state, reading.Second);
    return CMOS_CLOCK_REGISTER;

    case CMOS_MINUTES:
        *value = CmosClockValue(state, reading.Minute);
    return CMOS_CLOCK_REGISTER;

    case CMOS_HOURS:
    {
        /* [CAUTION]: 12-HOUR MODE IS NOT "SUBTRACT TWELVE". Status B bit 1 clear selects
         * it, and then BIT 7 OF THIS REGISTER IS PM -- midnight is 12 AM and
         * noon is 12 PM, neither of which is hour 0. A model that ignores the
         * bit tells a 12-hour guest that 14:00 is 2 AM.
         */
        UINT hour = reading.Hour;
        BYTE pmBit = 0;

        if (!(state->StatusB & CMOS_B_24_HOUR))
        {
            pmBit = (BYTE)(hour >= CMOS_HOURS_PER_HALF_DAY ? CMOS_HOUR_PM_BIT : CMOS_HOUR_AM);
            hour %= CMOS_HOURS_PER_HALF_DAY;

            if (!hour)
                hour = CMOS_HOURS_PER_HALF_DAY;
        }

        *value = (BYTE)(CmosClockValue(state, hour) | pmBit);
        return CMOS_CLOCK_REGISTER; }

    /* [CAUTION]: 01h, 03h and 05h -- the ALARM registers -- are deliberately NOT claimed
     * here. They are storage the guest owns, not a view of the host clock, so
     * they fall through to ram[] on both the read and the write paths.
     */
    case CMOS_DAY_OF_MONTH:
        *value = CmosClockValue(state, reading.Day);
    return CMOS_CLOCK_REGISTER;

    case CMOS_MONTH:
        *value = CmosClockValue(state, reading.Month);
    return CMOS_CLOCK_REGISTER;

    case CMOS_YEAR:
        *value = CmosClockValue(state, reading.Year);
    return CMOS_CLOCK_REGISTER;

    case CMOS_CENTURY:
        *value = CmosClockValue(state, reading.Century);
    return CMOS_CLOCK_REGISTER;

    /* THE DAY OF WEEK COMES FROM THE HOST, NOT A CALENDAR RULE. (s81, #182) It
     * was fixed at 1 (Sunday) because the clock reading carried no weekday and a
     * device model should not hold calendar arithmetic. Windows' GetLocalTime
     * already knows it, so the host passes it through (1 = Sunday, the chip's
     * numbering); a reader with no weekday (0) still falls back to ram[].
     */
    case CMOS_DAY_OF_WEEK:
        if (!reading.DayOfWeek)
            return CMOS_NOT_A_CLOCK_REGISTER;

        *value = CmosClockValue(state, reading.DayOfWeek);
        return CMOS_CLOCK_REGISTER;

    default:
        return CMOS_NOT_A_CLOCK_REGISTER;
    }
}

/* One second has passed: raise the update-ended flag, and the alarm flag if the
 * clock has reached the programmed time. Returns non-zero if either was set.
 */
static INT CmosSecondEdge(PCMOS_STATE state)
{
    INT hasFired = FALSE;

    if (state->IsSetHeld)
        return FALSE;                                  /* SET: updates are inhibited */

    if (state->StatusB & CMOS_B_UIE)              /* UIE: update ended */
    {
        state->StatusC |= CMOS_C_UF;
        state->UpdateEndedRaised++;
        hasFired = TRUE;
    }

    if (state->StatusB & CMOS_B_AIE)              /* AIE: alarm */
    {
        BYTE hours;
        BYTE minutes;
        BYTE seconds;

        if (CmosClockRegister(state, CMOS_SECONDS, &seconds) &&
            CmosClockRegister(state, CMOS_MINUTES, &minutes) &&
            CmosClockRegister(state, CMOS_HOURS, &hours) &&
            CmosAlarmFieldMatches(state->Ram[CMOS_SECONDS_ALARM], seconds) &&
            CmosAlarmFieldMatches(state->Ram[CMOS_MINUTES_ALARM], minutes) &&
            CmosAlarmFieldMatches(state->Ram[CMOS_HOURS_ALARM], hours))
        {
            state->StatusC |= CMOS_C_AF;
            state->AlarmRaised++;
            hasFired = TRUE;
        }
    }

    return hasFired;
}

VOID VddCmosAddClocks(PCMOS_STATE state, UINT32 clocks)
{
    UINT32 rateHz;
    UINT32 period;
    INT guard = 0;

    /* THE ONCE-A-SECOND EDGE, for UF and AF. Accumulated from the same clocks
     * the periodic divider uses rather than polled off the host clock, so
     * rtc_now is called once a SECOND instead of once a pacer tick.
     */
    if (state->StatusB & CMOS_B_UIE_OR_AIE)       /* UIE or AIE enabled */
    {
        state->SecondAccumulator += clocks;

        while (state->SecondAccumulator >= PIT_INPUT_HZ && guard++ < CMOS_SECOND_EDGE_GUARD)
        {
            state->SecondAccumulator -= PIT_INPUT_HZ;

            if (CmosSecondEdge(state))
            {
                state->StatusC |= CMOS_C_IRQF;    /* IRQF: something is pending */

                if (state->Bus)
                    VddRaiseIrq(state->Bus, CMOS_RTC_IRQ);
            }
        }
    }
    else
    {
        state->SecondAccumulator = 0;
    }

    guard = 0;
    /* Dormant unless the guest asked for it -- see the note in the header. Doing
     * nothing at all (rather than accumulating) means enabling PIE later starts
     * from now rather than replaying a backlog of ticks nobody was listening for.
     */
    if (!(state->StatusB & CMOS_B_PIE))
    {
        state->PeriodicAccumulator = 0;
        return;
    }

    rateHz = VddCmosPeriodicHz(state);

    if (!rateHz)
    {
        state->PeriodicAccumulator = 0;
        return;
    }

    period = PIT_INPUT_HZ / rateHz;                 /* PIT-rate clocks per periodic tick */

    if (!period)
        return;

    state->PeriodicAccumulator += clocks;

    while (state->PeriodicAccumulator >= period && guard++ < CMOS_PERIODIC_GUARD)
    {
        state->PeriodicAccumulator -= period;
        /* PF, and IRQF because an interrupt is now pending. Both are cleared by a
         * read of Status C -- which is how a handler acknowledges the chip.
         */
        state->StatusC |= CMOS_C_PF_AND_IRQF;
        state->PeriodicRaised++;

        if (state->Bus)
            VddRaiseIrq(state->Bus, CMOS_RTC_IRQ);
    }
}

static BYTE CmosRead(PCMOS_STATE state, BYTE registerIndex)
{
    BYTE value;

    if (CmosClockRegister(state, registerIndex, &value))
        return value;

    switch (registerIndex)
    {
    /* STATUS A. BIT 7 IS **UIP** AND WE REPORT IT CLEAR, ALWAYS:
     * The chip sets UIP for ~2 ms once a second while it updates its own
     * registers, and software waits for it to fall before reading the time.
     * We have no such window -- the registers are derived from the host clock
     * at the instant of the read, so they are never mid-update -- and
     * answering "never busy" is therefore TRUE of this model rather than a
     * convenient lie -- and it is why bit 7 is masked off on the WRITE path
     * instead of being stored: UIP is the chip telling software when it may
     * read, never software telling the chip anything.
     */
    case CMOS_STATUS_A:
        return state->StatusA;

    /* STATUS B: BCD, 24-HOUR. MEASURED 0x02 on 6.22-under-QEMU AND on
     * PCem's real AMI BIOS; dosbox-x answers 0x03, which is the same plus
     * DSE (daylight saving). Two of three, including the period-correct
     * machine, and 0x02 is what the bit definitions say a PC leaves.
     */
    case CMOS_STATUS_B:
        return state->StatusB;

    /* STATUS C IS CLEARED BY BEING READ:
     * That is how IRQ8 is acknowledged at the chip: the flags latch and the
     * read clears them, so a handler that does not read 0Ch gets exactly one
     * interrupt and then silence. VddCmosAddClocks sets PF and IRQF here.
     */
    case CMOS_STATUS_C:
    {
        BYTE flags = state->StatusC;
        state->StatusC = 0;
        return flags;
    }

    /* STATUS D BIT 7 IS VRT, "valid RAM and time". CLEAR means "the battery
     * died and everything in here is garbage", which firmware and setup
     * programs act on. With no chip at all we answered 0xFF, whose bit 7 is
     * set -- so this row was RIGHT BY ACCIDENT and agreed with all three
     * oracles. It is now right on purpose.
     */
    case CMOS_STATUS_D:
        return CMOS_D_VRT;

    default:
        break;
    }

    return state->Ram[registerIndex & CMOS_REGISTER_MASK];
}

static VOID CmosPortOut(PVOID context, WORD port, BYTE width, UINT32 value)
{
    PCMOS_STATE state = (PCMOS_STATE)context;
    BYTE byteValue = (BYTE)value;

    (VOID)width;

    if (port == CMOS_INDEX_PORT)
    {
        /* Bit 7 is the NMI mask, not part of the register number -- see the note
         * in the header. Counted, because a run should be able to say whether a
         * guest masks NMI, and not acted on, because we have no NMI to mask.
         */
        if (byteValue & CMOS_NMI_MASK_BIT)
            state->NmiMaskWrites++;

        state->IsNmiDisabled = (BYTE)((byteValue & CMOS_NMI_MASK_BIT) ? TRUE : FALSE);
        state->Index = (BYTE)(byteValue & CMOS_REGISTER_MASK);
        return;
    }

    /* Port 0x71 write. The clock registers move the VDM's own RTC (GH #261, below
     * -- an offset from the host's, src/dos/dos_clock.h; the machine's clock never
     * moves); the status registers are the guest's control bits or read-only;
     * everything else is battery-backed RAM and takes it.
     */
    if (state->Index == CMOS_STATUS_A)
    {
        /* Bit 7 is UIP and is READ-ONLY -- it is the chip telling software when
         * it may read, not software telling the chip anything.
         */
        state->StatusA = (BYTE)(byteValue & CMOS_A_WRITABLE_BITS);
        return;
    }

    if (state->Index == CMOS_STATUS_B)
    {
        /* GH #261: SET (bit 7). Going high freezes a copy of the clock and,
         * per the MC146818 datasheet, CLEARS UIE -- no update-ended interrupt
         * for a clock that is not updating. Going low commits the copy: the
         * date, then the time, through the same hook INT 1Ah AH=05h/03h use.
         */
        if ((byteValue & CMOS_B_SET) && !state->IsSetHeld)
        {
            PIT_RTC_READING reading;

            if (CmosReading(state, &reading))
            {
                state->Shadow = reading;
                state->IsSetHeld = 1;
            }

            byteValue = (BYTE)(byteValue & ~CMOS_B_UIE);
        }
        else if (!(byteValue & CMOS_B_SET) && state->IsSetHeld)
        {
            state->IsSetHeld = 0;

            if (state->RtcSet)
            {
                state->RtcSet(state->RtcContext, &state->Shadow, 1);
                state->RtcSet(state->RtcContext, &state->Shadow, 0);
            }
        }

        state->StatusB = byteValue;
        /* Disabling the periodic interrupt drops any part-accumulated tick, so
         * re-enabling it starts from now rather than firing immediately.
         */
        if (!(byteValue & CMOS_B_PIE))
            state->PeriodicAccumulator = 0;

        return;
    }

    /* [WARNING]: THE ALARM REGISTERS ARE NOT THE CLOCK, AND BLANKET-REFUSING THEM WAS A
     * DEFECT I INTRODUCED. 01h, 03h and 05h are the seconds/minutes/hours
     * ALARM, and writing them is the only way to set an alarm at all -- so
     * "everything below 0x0E is read-only" made the alarm interrupt
     * unreachable in the same way refusing Status B made the periodic one
     * unreachable. The rule is not "low registers are read-only"; it is
     * "WE CANNOT MOVE THE HOST'S CLOCK", and that applies to 00/02/04 and to
     * the date, not to a comparison value the guest owns.
     */
    if (state->Index == CMOS_SECONDS_ALARM || state->Index == CMOS_MINUTES_ALARM || state->Index == CMOS_HOURS_ALARM)
    {
        state->Ram[state->Index] = byteValue;
        return;
    }

    /* GH #261: THE CLOCK ITSELF. With a host hook (rtc_set) a write moves
     * the VDM's RTC -- frozen copy while SET is held, committed at once when
     * it is not. The byte is decoded in the base DM selects, and the hours
     * register in the mode bit 1 selects (12-hour: bit 7 is PM).
     *
     * [CAUTION]: THE DAY OF WEEK (06h) STAYS REFUSED: the VDM's weekday is derived from
     * its date, and a free-running counter the guest can set to anything has
     * no place in an offset. Without the hook every clock write is refused.
     */
    if (state->Index == CMOS_SECONDS || state->Index == CMOS_MINUTES || state->Index == CMOS_HOURS ||
        state->Index == CMOS_DAY_OF_MONTH || state->Index == CMOS_MONTH || state->Index == CMOS_YEAR ||
        state->Index == CMOS_CENTURY)
    {
        PIT_RTC_READING reading;
        INT isDate = state->Index >= CMOS_DAY_OF_MONTH;

        if (!state->RtcSet || !CmosReading(state, &reading))
            return;

        switch (state->Index)
        {
        case CMOS_SECONDS:
            reading.Second = CmosClockDecode(state, byteValue);
        break;

        case CMOS_MINUTES:
            reading.Minute = CmosClockDecode(state, byteValue);
        break;

        case CMOS_HOURS:
            if (state->StatusB & CMOS_B_24_HOUR)
                reading.Hour = CmosClockDecode(state, byteValue);
            else { UINT hour = CmosClockDecode(state, (BYTE)(byteValue & CMOS_HOUR_VALUE_BITS)) % CMOS_HALF_DAY_HOURS;
                   reading.Hour = hour + ((byteValue & CMOS_HOUR_PM_BIT) ? CMOS_HALF_DAY_HOURS : CMOS_NO_HOURS); }

            break;

        case CMOS_DAY_OF_MONTH:
            reading.Day   = CmosClockDecode(state, byteValue);
        break;

        case CMOS_MONTH:
            reading.Month = CmosClockDecode(state, byteValue);
        break;

        case CMOS_YEAR:
            reading.Year  = CmosClockDecode(state, byteValue);
        break;

        default:
            reading.Century  = CmosClockDecode(state, byteValue);
        break;
        }

        if (state->IsSetHeld)
            state->Shadow = reading;
        else
            state->RtcSet(state->RtcContext, &reading, isDate);

        return;
    }

    if (state->Index <= CMOS_STATUS_D)
        return;                                    /* DOW + Status C/D: read-only */

    state->Ram[state->Index & CMOS_REGISTER_MASK] = byteValue;
}

static VOID CmosPortIn(PVOID context, WORD port, BYTE width, UINT32 *value)
{
    PCMOS_STATE state = (PCMOS_STATE)context;

    (VOID)width;
    /* [CAUTION]: PORT 0x70 IS WRITE-ONLY ON THE PART and a read of it is undefined.
     * Answer consistently rather than plausibly -- 0xFF is what an undriven
     * bus gives, and it is what this port gave before anything claimed it.
     */
    if (port == CMOS_INDEX_PORT)
    {
        *value = CMOS_UNDRIVEN_BUS;
        return;
    }

    *value = CmosRead(state, state->Index);
}

VOID VddCmosReset(PVOID context)
{
    PCMOS_STATE state = (PCMOS_STATE)context;
    VDD_BUS *bus = state->Bus;
    VOID (*rtcNow)(PVOID , PIT_RTC_READING *) = state->RtcNow;
    INT  (*rtcSet)(PVOID , const PIT_RTC_READING *, INT) = state->RtcSet;
    PVOID rtcContext = state->RtcContext;
    WORD baseKb = state->BaseKb;                       /* #136: preserved, as above */
    UINT byteIndex;

    for (byteIndex = 0; byteIndex < sizeof(*state); ++byteIndex)
        ((PBYTE)state)[byteIndex] = 0;

    state->Bus = bus;
    state->RtcNow = rtcNow;
    state->RtcSet = rtcSet;
    state->RtcContext = rtcContext;
    state->BaseKb = baseKb;
    /* WHAT POST LEAVES IN THE CMOS. (docs/ref/rtc.md 3):
     * These are BIOS conventions, not chip behaviour, and they are here
     * because a machine DOS is running on has been through POST -- the same
     * reasoning as the 8042's status register starting with SYS set.
     *
     * [CAUTION]: THE EQUIPMENT BYTE'S LOW NIBBLE IS NOT ADJUDICABLE. p_rtc measured 6 on
     * QEMU, 7 on dosbox-x and 0x0D on PCem: it describes the MACHINE's video
     * type and coprocessor, so three machines legitimately give three answers.
     * 0x05 = colour 80x25, coprocessor present, which is what we present.
     */
    /* [CAUTION]: THESE TWO ARE WHAT A PC BIOS LEAVES, and Status B's value was MEASURED:
     * 0x02 on 6.22-under-QEMU and on PCem's real AMI BIOS alike -- BCD, 24-hour,
     * and every interrupt DISABLED. Status A's 0x26 is the normal 32.768 kHz
     * divider with the BIOS's usual 1024 Hz rate select, which is inert while
     * PIE is clear.
     */
    state->StatusA = CMOS_POST_STATUS_A;
    state->StatusB = CMOS_POST_STATUS_B;
    state->Ram[CMOS_DAY_OF_WEEK]   = CMOS_POST_DAY_OF_WEEK;              /* see the note in CmosClockRegister */
    state->Ram[CMOS_DIAGNOSTIC] = CMOS_POST_DIAGNOSTIC;              /* POST diagnostic: no errors */
    state->Ram[CMOS_FLOPPY_TYPES] = CMOS_ONE_1440K_FLOPPY;              /* one 1.44M floppy */
    state->Ram[CMOS_EQUIPMENT] = CMOS_POST_EQUIPMENT;              /* 1 floppy, colour 80x25, FPU */
    /* Base memory FITTED: 640 KB (0280h) unless Settings > Conventional Memory says less
     * (#136). The EBDA and INT 12h's 639 are carved out of this by the BIOS, not here.
     */
    {   WORD baseMemoryKb = state->BaseKb ? state->BaseKb : CMOS_DEFAULT_BASE_KB;
        state->Ram[CMOS_BASE_KB_LOW] = (BYTE)(baseMemoryKb & CMOS_LOW_BYTE);
        state->Ram[CMOS_BASE_KB_HIGH] = (BYTE)(baseMemoryKb >> BYTE_SHIFT); }
    /* EXTENDED MEMORY, AS POST WOULD HAVE COUNTED IT (s81, #182). 17h/18h are the
     * configured and 30h/31h the POST-detected KB above 1 MB; a real BIOS answers
     * INT 15h AH=88h from the latter. Ours answers 88h with 0x3C00 (15 MB, main.c),
     * so CMOS says the same -- two views of one machine must not disagree.
     */
    state->Ram[CMOS_EXTENDED_KB_LOW] = (BYTE)(CMOS_EXTENDED_KB & CMOS_LOW_BYTE);
    state->Ram[CMOS_EXTENDED_KB_HIGH] = (BYTE)(CMOS_EXTENDED_KB >> BYTE_SHIFT);
    state->Ram[CMOS_POST_EXTENDED_LOW] = (BYTE)(CMOS_EXTENDED_KB & CMOS_LOW_BYTE);
    state->Ram[CMOS_POST_EXTENDED_HIGH] = (BYTE)(CMOS_EXTENDED_KB >> BYTE_SHIFT);
    /* THE CHECKSUM OVER 10h-2Dh, WHICH A BIOS VERIFIES AT BOOT:
     * A setup program that writes a configuration byte and does not fix this
     * makes the BIOS declare the CMOS invalid next time. We are not that
     * BIOS -- but leaving it zero means anything that DOES verify it decides
     * our CMOS is corrupt, which is the same wrong answer Status D's VRT bit
     * used to give. [CAUTION] Computed at reset only: a guest that writes into the
     * range invalidates it, exactly as on a real machine.
     */
    { UINT byteIndex, checksum = 0;

      for (byteIndex = CMOS_CHECKSUM_FIRST; byteIndex <= CMOS_CHECKSUM_LAST; ++byteIndex)
          checksum += state->Ram[byteIndex];

      state->Ram[CMOS_CHECKSUM_HIGH] = (BYTE)(checksum >> BYTE_SHIFT);
      state->Ram[CMOS_CHECKSUM_LOW] = (BYTE)(checksum & CMOS_LOW_BYTE); }
}

INT VddCmosInitialize(PVDD_BUS bus, PVOID context)
{
    PCMOS_STATE state = (PCMOS_STATE)context;

    state->Bus = bus;

    if (!state->Ram[CMOS_EQUIPMENT])
        VddCmosReset(state);                                /* the host builds us zeroed */

    state->Bus = bus;

    if (VddClaimPorts(bus, CMOS_INDEX_PORT, CMOS_DATA_PORT, CmosPortIn, CmosPortOut, state))
        return CMOS_FAILED;

    return CMOS_OK;
}
