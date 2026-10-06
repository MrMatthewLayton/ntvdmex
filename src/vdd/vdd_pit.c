/* vdd_pit.c -- see vdd_pit.h.  Intel 8254 channel-0 model + BIOS INT 08h/1Ah,
 * on the VDD bus.  Pure C, no <windows.h>. */
#include "vdd_pit.h"

/* The counter modes (Intel 8254, 231164-005). Modes 6 and 7 are aliases of 2 and 3. */
#define PIT_MODE_0              0       /* interrupt on terminal count               */
#define PIT_MODE_1              1       /* hardware retriggerable one-shot           */
#define PIT_MODE_2              2       /* rate generator                            */
#define PIT_MODE_3              3       /* square wave                               */
#define PIT_MODE_4              4       /* software-triggered strobe                 */
#define PIT_MODE_5              5       /* hardware-triggered strobe                 */
#define PIT_MODE_ALIAS_FIRST    6       /* 110 and 111 behave as 010 and 011         */
#define PIT_MODE_ALIAS_OFFSET   4
/* The Control Word (port 43h): SC1:0 | RW1:0 | M2:0 | BCD. */
#define PIT_CW_COUNTER_SHIFT    6
#define PIT_CW_ACCESS_SHIFT     4
#define PIT_CW_ACCESS_MASK      3
#define PIT_CW_MODE_SHIFT       1
#define PIT_CW_MODE_MASK        7
#define PIT_CW_BCD              1
#define PIT_SELECT_COUNTER1     1       /* SC1:0                                     */
#define PIT_SELECT_COUNTER2     2
#define PIT_SELECT_READ_BACK    3
/* RW1:0, the access mode. */
#define PIT_ACCESS_LATCH        0       /* counter latch command                     */
#define PIT_ACCESS_LOW          1       /* LSB only                                  */
#define PIT_ACCESS_HIGH         2       /* MSB only                                  */
#define PIT_ACCESS_LOW_HIGH     3       /* LSB then MSB                              */
/* The Read-Back command (both latch bits ACTIVE LOW) and the status byte it latches. */
#define PIT_READ_BACK_NO_STATUS 0x10    /* bit 4 = 0 latches the status              */
#define PIT_READ_BACK_NO_COUNT  0x20    /* bit 5 = 0 latches the count               */
#define PIT_STATUS_OUT          0x80
#define PIT_STATUS_NULL_COUNT   0x40
#define PIT_STATUS_ACCESS_SHIFT 4
#define PIT_STATUS_MODE_SHIFT   1
#define PIT_STATUS_LOW          0
/* Bytes of a 16-bit count. */
#define PIT_BYTE_SHIFT          8
#define PIT_LOW_BYTE            0xFF
/* Ports. */
#define PIT_COUNTER0_PORT       0x40
#define PIT_COUNTER1_PORT       0x41
#define PIT_COUNTER2_PORT       0x42
#define PIT_CONTROL_PORT        0x43
#define PIT_COUNTER0            0
#define PIT_COUNTER1            1
#define PIT_COUNTER2            2
#define PIT_NO_COUNTER          (-1)
#define PIT_UNDRIVEN_BUS        0xFF
/* Pacing. */
#define PIT_CATCH_UP_GUARD      100000  /* most IRQ0 periods caught up in one call   */
#define PIT_MICROSECONDS_PER_SECOND 1000000u
#define PIT_HALF                2       /* mode 3 runs each half of the period       */
#define PIT_SQUARE_WAVE_STEP    2       /* mode 3 decrements by two                  */
/* POST's choices (see VddPitReset) and DRAM refresh. */
#define PIT_REFRESH_DIVISOR     18
/* The BIOS: data area, vectors, INT 1Ah. */
#define PIT_BIOS_DATA_SEGMENT   0x40
#define PIT_BDA_TICK_COUNT      0x6C    /* 0040:006C, DWORD                          */
#define PIT_BDA_MIDNIGHT_FLAG   0x70    /* 0040:0070, BYTE                           */
#define PIT_TIMER_VECTOR        0x08
#define PIT_TIME_OF_DAY_VECTOR  0x1A
#define PIT_1A_GET_TICKS        0x00
#define PIT_1A_SET_TICKS        0x01
#define PIT_1A_GET_TIME         0x02
#define PIT_1A_SET_TIME         0x03
#define PIT_1A_GET_DATE         0x04
#define PIT_1A_SET_DATE         0x05
#define PIT_TICKS_HIGH_SHIFT    16      /* CX:DX = the tick count                    */
#define PIT_TICKS_LOW_MASK      0xFFFF
#define PIT_DECIMAL_BASE        10
#define PIT_BCD_TENS_WEIGHT     16
#define PIT_BCD_FIELD_SHIFT     4
#define PIT_BCD_UNITS_MASK      0x0F
#define PIT_BCD_MAX_DIGIT       9
#define PIT_DATE_FIELDS         4u      /* century, year, month, day                 */
#define PIT_TIME_FIELDS         3u      /* hour, minute, second                      */
#define PIT_FIELD_COUNT         4
#define PIT_FIELD_0             0
#define PIT_FIELD_1             1
#define PIT_FIELD_2             2
#define PIT_FIELD_3             3
#define PIT_DEFAULT_CENTURY     20      /* the reading's defaults before the host fills it */
#define PIT_MAX_HOUR            23
#define PIT_MAX_MINUTE          59
#define PIT_MAX_SECOND          59
#define PIT_OK                  0
#define PIT_FAILED              (-1)

/* ── ★★★★ THE COUNT STARTS WHEN THE GUEST LOADS IT. ─────────────────────────────
     This used to be `R - (total_clocks % R)`: a free-running divider whose phase had
     nothing to do with the moment the guest wrote the count. For a guest that only
     ever waits for IRQ0 that is invisible. For one that READS THE COUNTER it is a
     random number generator.
   ★ MEASURED ON LEMMINGS (s69). Its "High Performance PC" option calibrates the game
     tick from the CRT: load counter 0 with 0xFFFF in mode 0, count 320 hblanks on
     0x3DA bit 0, latch, and use 0xFFFF - latch as the reload (guest CS:15BB..1643 in
     the real-DOS dump). The user's by-hand run got 0x4BB9 (61.5 Hz -- "the music is
     slower"), the headless rig 0xC40C (23.8 Hz), every run a different number,
     because the read-back was `total_clocks % 0xFFFF` at the latch: uniformly random.
   ► So keep the instant of the load (`load_clocks`) and derive the counting element
     from the clocks since it, per mode (Intel 8254 datasheet, 231164-005):
       mode 0   N - elapsed, on past terminal count through 0xFFFF (one-shot)
       mode 3   "decremented by two on succeeding CLK pulses" and reloaded at zero,
                so the count runs N, N-2, ... twice per period; never an odd LSB
       mode 2   N - (elapsed mod N), reloading at the period
       1, 4, 5  one-shots: as mode 0 (#175)
   ⚠ s69 HISTORY: this model was shipped, blamed for a blank screen, and reverted. The
     blank screen was never the count -- it was IRQ0 re-entering the game's timer ISR
     (the host auto-EOI'd IRQ0; see irq0_ack in the host) once the tick was correct.
     Both halves are now fixed; see docs/STATE.md session 70. */
static INT PitOutPin(BYTE mode, UINT32 reload, UINT64 elapsed);

/* The count law, by mode. Split out of PitCurrentCount so counters 1 and 2 can
   use the same arithmetic instead of a second copy that drifts from it.
   Returns the counting element in BINARY; `wrap` is where it comes back round
   (65536, or 10000 in BCD -- see PitWrap). */
static UINT32 PitCountLaw(BYTE mode, UINT32 reload, UINT64 elapsed, UINT32 wrap)
{
    /* #175: modes 1, 4 and 5 are ONE-SHOTS too -- after terminal count the counter
       runs on through the wrap, it does not reload (docs/ref/pit.md mode table). They
       used the periodic law below, which reloads every `reload` clocks. */
    if (mode == PIT_MODE_0 || mode == PIT_MODE_1 || mode == PIT_MODE_4 || mode == PIT_MODE_5) {
        /* One-shot: counting does not stop at terminal count, it runs on through
           the wrap. In binary that was `(uint16_t)(R - elapsed)`, i.e. mod 65536
           by truncation; BCD wraps at 10000 instead, so do the modulus openly. */
        UINT32 remainder = (UINT32)(elapsed % wrap);
        UINT32 count   = (reload >= remainder) ? (reload - remainder) : (reload + wrap - remainder);
        return count % wrap;                     /* the maximum count reads as zero  */
    }
    if (mode == PIT_MODE_3) {
        UINT32 half  = (reload + 1) / PIT_HALF;
        UINT32 phase = (UINT32)(elapsed % half);
        return (reload - PIT_SQUARE_WAVE_STEP * phase) % wrap;       /* R when phase==0 (the max -> 0)   */
    }
    return (reload - (UINT32)(elapsed % reload)) % wrap;
}

/* The counting element, as a number. BINARY -- pit_read_count converts. */
static UINT32 PitCurrentCount(PCPIT_STATE state)
{
    UINT64 elapsed = (state->TotalClocks >= state->LoadClocks)
                     ? state->TotalClocks - state->LoadClocks : 0;
    return PitCountLaw(state->Mode, VddPitEffectiveReload(state), elapsed, PitWrap(state->IsBcd));
}

/* ── THE ONLY TWO PLACES BCD EXISTS: THE PORT BOUNDARY. ──────────────────────
     Everything inside the device counts in binary (see the note in vdd_pit.h);
     a count crosses `43h`/`40h-42h` in whatever base the Control Word selected. */
static WORD PitCountBytes(BYTE isBcd, UINT32 count)
{ return isBcd ? PitToBcd(count) : (WORD)count; }

static WORD PitCountValue(BYTE isBcd, WORD written)
{ return isBcd ? (WORD)PitFromBcd(written) : written; }

/* Counters 1 and 2. They have no IRQ engine and no accumulator: a read is simply
   "how far has the counting element got since it was loaded". */
/* Clocks this counter has actually counted. ⚠ NOT simply total - load: while the
   GATE is low the counting element is stopped, so the elapsed count freezes at
   whatever it had reached. */
static UINT64 PitCounterElapsed(PCPIT_STATE state, PCPIT_COUNTER counter)
{
    /* #175: in modes 1 and 5 the gate is a TRIGGER, not an enable -- once started
       the count runs whatever the gate's level (docs/ref/pit.md §5, mode table). */
    if (!counter->Gate && counter->Mode != PIT_MODE_1 && counter->Mode != PIT_MODE_5) return counter->GateElapsed;
    return (state->TotalClocks >= counter->LoadClocks) ? state->TotalClocks - counter->LoadClocks : 0;
}

static UINT32 PitCounterCount(PCPIT_STATE state, PCPIT_COUNTER counter)
{
    return PitCountLaw(counter->Mode, VddPitCounterEffectiveReload(counter), PitCounterElapsed(state, counter),
                         PitWrap(counter->IsBcd));
}

/* Port 61h bit 0 -> counter 2's GATE. A high-to-low edge freezes the elapsed
   count; a low-to-high edge slides load_clocks forward so counting RESUMES from
   there. Idempotent: writing the same level twice must not nudge the count,
   because a guest polling port 61h rewrites the whole byte constantly. */
VOID VddPitCounter2Gate(PPIT_STATE state, INT isHigh)
{
    PPIT_COUNTER counter = &state->Counter2;
    if (!!isHigh == !!counter->Gate) return;
    /* ── #175: WHAT A GATE EDGE MEANS DEPENDS ON THE MODE (docs/ref/pit.md §5).
         1, 5  a rising edge TRIGGERS: (re)load the count and start; the level is
               otherwise ignored.
         2, 3  a falling edge stops the count and forces OUT high; a rising edge
               RELOADS -- measured on all three oracles (p_pit pit.m2.retrig.reload),
               where this used to resume.
         0, 4  the gate is an enable: low pauses, high resumes where it left off. */
    if (counter->Mode == PIT_MODE_1 || counter->Mode == PIT_MODE_5) {
        if (isHigh) { counter->LoadClocks = state->TotalClocks; counter->IsTriggered = 1; }
        counter->Gate = (BYTE)(isHigh ? 1 : 0);
        return;
    }
    if (counter->Mode == PIT_MODE_2 || counter->Mode == PIT_MODE_3) {
        if (isHigh) { counter->LoadClocks = state->TotalClocks; counter->Gate = 1; }
        else    { counter->GateElapsed = PitCounterElapsed(state, counter); counter->Gate = 0; }
        return;
    }
    if (isHigh) { counter->LoadClocks = state->TotalClocks - counter->GateElapsed; counter->Gate = 1; }
    else    { counter->GateElapsed = PitCounterElapsed(state, counter);               counter->Gate = 0; }
}

/* #175: a channel's OUT pin, with the two gate rules the count law cannot see:
   modes 1/5 hold OUT high until triggered, and modes 2/3 force it high while the
   gate is low. Everything else is PitOutPin's function of mode and elapsed. */
static INT PitCounterOut(PCPIT_STATE state, PCPIT_COUNTER counter)
{
    if ((counter->Mode == PIT_MODE_1 || counter->Mode == PIT_MODE_5) && !counter->IsTriggered) return 1;
    if ((counter->Mode == PIT_MODE_2 || counter->Mode == PIT_MODE_3) && !counter->Gate) return 1;
    return PitOutPin(counter->Mode, VddPitCounterEffectiveReload(counter), PitCounterElapsed(state, counter));
}

/* Counter 2's OUT pin, as port 61h bit 5 reports it. */
INT VddPitCounter2Out(PCPIT_STATE state)
{
    return PitCounterOut(state, &state->Counter2);
}

/* A count write, lo/hi buffered exactly as counter 0 does it: a HALF-WRITTEN
   COUNT IS NOT A COUNT (see the note on port 0x40 below -- the same defect would
   otherwise be reintroduced here). */
static VOID PitCounterWriteCount(PPIT_STATE state, PPIT_COUNTER counter, BYTE byteValue)
{
    WORD count;
    if (counter->Access == PIT_ACCESS_LOW)      count = byteValue;                        /* lo only         */
    else if (counter->Access == PIT_ACCESS_HIGH) count = (WORD)(byteValue << PIT_BYTE_SHIFT);       /* hi only         */
    else {                                                   /* lo then hi      */
        if (!counter->IsWriteHighNext) { counter->WriteLow = byteValue; counter->IsWriteHighNext = 1; return; }
        count = (WORD)((byteValue << PIT_BYTE_SHIFT) | counter->WriteLow);
        counter->IsWriteHighNext = 0;
    }
    counter->Reload = PitCountValue(counter->IsBcd, count);   /* BCD in, binary stored */
    if (counter->Mode == PIT_MODE_1 || counter->Mode == PIT_MODE_5) counter->IsTriggered = 0;   /* #175: armed, not started */
    counter->IsNullCount = 0;                 /* the count has reached the CE        */
    counter->LoadClocks = state->TotalClocks;
    counter->GateElapsed = 0;             /* a NEW count: nothing counted of it yet. Keeping the
                                        old count's gate-frozen elapsed made the next gate
                                        rise resume past terminal count -- OUT high at once,
                                        so a second "time it with counter 2" wait returned
                                        immediately (s83, p_pit0; pit_test T_WAIT). */
}

/* A read, honouring the latch and the access mode -- the same contract as 0x40. */
static VOID PitCounterReadCount(PPIT_STATE state, PPIT_COUNTER counter, UINT32 *value)
{
    WORD count = PitCountBytes(counter->IsBcd, counter->IsLatched ? counter->Latch : PitCounterCount(state, counter));
    BYTE accessMode = counter->Access ? counter->Access : PIT_ACCESS_LOW_HIGH;
    if (accessMode == PIT_ACCESS_LOW)      { *value = count & PIT_LOW_BYTE;        counter->IsLatched = 0; }
    else if (accessMode == PIT_ACCESS_HIGH) { *value = (count >> PIT_BYTE_SHIFT) & PIT_LOW_BYTE; counter->IsLatched = 0; }
    else {
        if (!counter->IsReadHighNext) { *value = count & PIT_LOW_BYTE; counter->IsReadHighNext = 1; }
        else { *value = (count >> PIT_BYTE_SHIFT) & PIT_LOW_BYTE; counter->IsReadHighNext = 0; counter->IsLatched = 0; }
    }
}

/* One control word, applied to counter 1 or 2. */
static VOID PitCounterControl(PPIT_STATE state, PPIT_COUNTER counter, BYTE controlWord)
{
    BYTE accessMode = (BYTE)((controlWord >> PIT_CW_ACCESS_SHIFT) & PIT_CW_ACCESS_MASK);
    if (accessMode == PIT_ACCESS_LATCH) {                              /* Counter Latch Command       */
        counter->Latch = (WORD)PitCounterCount(state, counter);
        counter->IsLatched = 1; counter->IsReadHighNext = 0;
        return;
    }
    counter->Access   = accessMode;
    counter->IsTriggered     = 0;                 /* #175: a new mode waits for its own trigger */
    counter->ProgrammedMode = (BYTE)((controlWord >> PIT_CW_MODE_SHIFT) & PIT_CW_MODE_MASK);
    counter->Mode     = (counter->ProgrammedMode >= PIT_MODE_ALIAS_FIRST) ? (BYTE)(counter->ProgrammedMode - PIT_MODE_ALIAS_OFFSET) : counter->ProgrammedMode;
    counter->IsBcd      = (BYTE)(controlWord & PIT_CW_BCD);
    counter->IsWriteHighNext  = 0;
    counter->IsNullCount = 1;                 /* CR written-to-be, CE not yet loaded */
    counter->LoadClocks = state->TotalClocks;
    counter->GateElapsed = 0;             /* see PitCounterWriteCount */
}

/* --- the time engine: clocks -> IRQ0 pulses ------------------------------- */
VOID VddPitAddClocks(PPIT_STATE state, UINT32 clocks)
{
    UINT32 reload = VddPitEffectiveReload(state);
    INT iterations = 0;
    state->TotalClocks += clocks;
    /* ── IRQ0 IS OUT's RISING EDGE, AND A ONE-SHOT HAS ONE (#175). ───────────────────
         Mode 0: OUT low at the Control Word, high at terminal count, and it stays high.
         Mode 4: a one-clock strobe low at terminal count. Either way ONE edge per count
         loaded -- measured on QEMU, DOSBox-X and PCem (tests/probes/dos/p_pit0.asm), a bare
         re-write after terminal count included. Modes 1 and 5 start on a GATE rising
         edge, and counter 0's gate is tied high on a PC, so they never start: no edge.
         Until #175 every mode raised once per period, as if it were mode 2. */
    if (state->Mode != PIT_MODE_2 && state->Mode != PIT_MODE_3) {
        state->Accumulator = 0;
        if (state->IsIrqArmed && state->TotalClocks - state->LoadClocks >= reload) {
            state->IsIrqArmed = 0;
            VddRaiseIrq(state->Bus, 0);
        }
        return;
    }
    state->Accumulator += clocks;
    while (state->Accumulator >= reload && iterations++ < PIT_CATCH_UP_GUARD) {
        state->Accumulator -= reload;
        VddRaiseIrq(state->Bus, 0);           /* IRQ0 -> guest takes INT 08h     */
        /* A count written WITHOUT a Control Word in modes 2/3 is "loaded at the end
           of the current counting cycle" (datasheet, mode 2; mode 3 says half-cycle,
           approximated here as the full one). This is that end. */
        if (state->IsNextPending) {
            state->Reload       = state->NextReload;
            state->IsNextPending = 0;
            state->LoadClocks  = state->TotalClocks - state->Accumulator;   /* the period boundary */
            reload = VddPitEffectiveReload(state);
        }
    }
}

/* ── ★★★★★ WHEN A COUNT WRITE RESTARTS THE PERIOD -- FROM THE DATASHEET, NOT BELIEF. ──
     Intel 8254 (231164-005), the modes every DOS timer uses:
       Mode 2 / Mode 3: "After writing a Control Word and initial count, the Counter will
         be loaded on the next CLK pulse. This allows the Counter to be synchronized by
         software." -- i.e. Control Word THEN count = load now, period restarts.
       Mode 2 / Mode 3: "Writing a new count while counting does not affect the current
         counting sequence ... the new count will be loaded at the end of the current
         counting cycle." -- i.e. a BARE count write (no Control Word since the last
         load) leaves the period in flight alone and takes over at its end.
       Modes 0/1/4/5: "If a new count is written to the Counter, it will be loaded on the
         next CLK pulse and counting will continue from the new count." -- restart.
   ⛔⛔ THE s69 REGRESSION, BOTH WAYS. 59ee731 restarted on EVERY count write (wrong for a
     bare write in 2/3); 9eec369 "corrected" it to NEVER restart in 2/3 -- also wrong,
     because Lemmings writes `out 43h,36h` BEFORE its count on every tick, which is the
     "synchronized by software" case. Its game ISR runs at IRQ0, spins on 0x3DA for the
     vertical retrace, and THEN reprograms the count: the tick is meant to be locked to
     the retrace, with the IRQ landing ~320 lines after it. Never restarting made the IRQ
     free-run at ~98 Hz against a 70 Hz retrace, so the ISR was re-entered before it
     could finish and the game loop starved (the "stuck palette fade"). Each of the two
     wrong models had a test written to certify it. These are written from the quotes
     above; [[m9-completeness-programme]] rule: never from memory.
   ► `cw_armed` is set by a Control Word for this counter and cleared by the load it
     arms; `next_pending` parks a bare mode-2/3 count until VddPitAddClocks reaches
     the end of the period. `load_clocks` moves only when the counting element really
     (re)starts, because that is what PitCurrentCount measures the read-back from. */
static VOID PitLoad(PPIT_STATE state, WORD written)
{
    WORD count = PitCountValue(state->IsBcd, written);  /* BCD in, binary stored */
    INT isPeriodic = (state->Mode == PIT_MODE_2 || state->Mode == PIT_MODE_3);
    if (isPeriodic && !state->IsControlWordArmed) {         /* bare write: takes over at period end */
        state->NextReload  = count;
        state->IsNextPending = 1;
        return;
    }
    state->Reload       = count;                /* Control Word + count, or a one-shot mode */
    state->LoadClocks  = state->TotalClocks;
    state->Accumulator        = 0;
    state->IsControlWordArmed     = 0;
    state->IsNextPending = 0;
    state->IsIrqArmed    = (BYTE)(state->Mode == PIT_MODE_0 || state->Mode == PIT_MODE_4);   /* see add_clocks */
    if (!isPeriodic) state->OneShotLoads++;
    state->Restarts++;
}

/* See PIT_STATE.guard in the header: every touch of counter state from a caller
   the HOST does not already serialize against the pacer goes through this. */
#define PIT_GUARD(state, isEntering) do { if ((state)->Guard) (state)->Guard((state)->GuardContext, (isEntering)); } while (0)

static VOID PitFrame(PVOID context)
{
    PPIT_STATE state = (PPIT_STATE)context;
    UINT32 clocks;
    /* The host that drives time from a pacer sets frame_us = 0; add_clocks(0) is
       a no-op ARITHMETICALLY but its u64 read-modify-writes still race the pacer
       on a 32-bit build, so do not touch the state at all. */
    if (!state->FrameMicroseconds) return;
    clocks = (UINT32)(((UINT64)PIT_INPUT_HZ * state->FrameMicroseconds) / PIT_MICROSECONDS_PER_SECOND);
    PIT_GUARD(state, 1);
    VddPitAddClocks(state, clocks);
    PIT_GUARD(state, 0);
}


/* ── THE OUT PIN, DERIVED RATHER THAN STORED. ────────────────────────────────
   OUT is a function of the mode and how far the count has got, so there is no
   state to keep in step -- and keeping it derived means it cannot go stale the
   way a cached flag would. docs/ref/pit.md 5 is the table this implements. */
static INT PitOutPin(BYTE mode, UINT32 reload, UINT64 elapsed)
{
    UINT32 phase;
    switch (mode) {
    case PIT_MODE_0:                         /* low while counting, high at TC  */
    case PIT_MODE_1:
        return elapsed >= reload;
    case PIT_MODE_2:                         /* high, low for ONE clock at 1    */
        phase = (UINT32)(elapsed % reload);
        return phase != (reload - 1);
    case PIT_MODE_3: {                       /* square wave: high half, low half */
        UINT32 half = (reload + 1) / PIT_HALF;
        phase = (UINT32)(elapsed % reload);
        return phase < half;
    }
    case PIT_MODE_4:                         /* high, one-clock strobe at TC    */
    case PIT_MODE_5:
        return elapsed != reload;
    default:
        return 1;
    }
}

/* Status byte: b7 OUT, b6 null count, b5:4 access, b3:1 mode, b0 BCD.
   ⚠ b3:1 IS mode_raw, NOT the normalised mode. Measured on a real 8254
     (p_pit.asm pit.mode6.readback = 0x0C): programming 110 reads back 110 even
     though the counter behaves as mode 2. */
static BYTE PitStatusOf(PCPIT_STATE state, INT counterIndex)
{
    BYTE accessMode, programmedMode, isBcd, isNullCount;
    UINT32 reload; UINT64 elapsed; INT outPin;
    if (counterIndex == PIT_COUNTER0) {
        accessMode = state->Access ? state->Access : PIT_ACCESS_LOW_HIGH; programmedMode = state->ProgrammedMode; isBcd = state->IsBcd;
        isNullCount = (BYTE)(state->IsControlWordArmed || state->IsNextPending);
        reload = VddPitEffectiveReload(state);
        elapsed = (state->TotalClocks >= state->LoadClocks)
                ? state->TotalClocks - state->LoadClocks : 0;
        outPin = PitOutPin(state->Mode, reload, elapsed);
    } else {
        PCPIT_COUNTER counter = (counterIndex == PIT_COUNTER1) ? &state->Counter1 : &state->Counter2;
        accessMode = counter->Access ? counter->Access : PIT_ACCESS_LOW_HIGH; programmedMode = counter->ProgrammedMode; isBcd = counter->IsBcd;
        isNullCount = counter->IsNullCount;
        (VOID)reload; (VOID)elapsed;
        outPin = PitCounterOut(state, counter);
    }
    return (BYTE)((outPin ? PIT_STATUS_OUT : PIT_STATUS_LOW) | (isNullCount ? PIT_STATUS_NULL_COUNT : PIT_STATUS_LOW)
                     | ((accessMode & PIT_CW_ACCESS_MASK) << PIT_STATUS_ACCESS_SHIFT) | ((programmedMode & PIT_CW_MODE_MASK) << PIT_STATUS_MODE_SHIFT) | (isBcd & PIT_CW_BCD));
}

/* ── THE READ-BACK COMMAND. ──────────────────────────────────────────────────
   ⚠ BOTH LATCH BITS ARE ACTIVE LOW: bit 5 = 0 latches the count, bit 4 = 0
     latches the status. Writing 1s asks for nothing, which is the natural-looking
     mistake. Bits 3/2/1 select counters 2/1/0, and one command may address
     several -- that is the whole point of having it. */
static VOID PitReadBack(PPIT_STATE state, BYTE command)
{
    INT counterIndex;
    for (counterIndex = 0; counterIndex < PIT_COUNTERS; ++counterIndex) {
        if (!(command & (1u << (counterIndex + 1)))) continue;          /* not selected        */
        if (!(command & PIT_READ_BACK_NO_STATUS)) {                             /* latch STATUS        */
            if (!state->StatusLatched[counterIndex]) {                    /* first latch wins    */
                state->StatusLatch[counterIndex] = PitStatusOf(state, counterIndex);
                state->StatusLatched[counterIndex] = 1;
            }
        }
        if (!(command & PIT_READ_BACK_NO_COUNT)) {                             /* latch COUNT         */
            if (counterIndex == PIT_COUNTER0) {
                if (!state->IsLatched) { state->Latch = (WORD)PitCurrentCount(state); state->IsLatched = 1; state->IsReadHighNext = 0; }
            } else {
                PPIT_COUNTER counter = (counterIndex == PIT_COUNTER1) ? &state->Counter1 : &state->Counter2;
                if (!counter->IsLatched) { counter->Latch = (WORD)PitCounterCount(state, counter); counter->IsLatched = 1; counter->IsReadHighNext = 0; }
            }
        }
    }
}

/* --- 8254 ports 0x40-0x43 ------------------------------------------------- */
static VOID PitPortOutLocked(PPIT_STATE state, WORD port, BYTE byteValue)
{
    if (port == PIT_CONTROL_PORT) {                      /* mode/command register           */
        BYTE counterIndex = (BYTE)(byteValue >> PIT_CW_COUNTER_SHIFT);
        BYTE accessMode = (BYTE)((byteValue >> PIT_CW_ACCESS_SHIFT) & PIT_CW_ACCESS_MASK);
        if (counterIndex == PIT_SELECT_COUNTER2) {                       /* channel 2: speaker tone AND a counter */
            if (accessMode != PIT_ACCESS_LATCH) { state->Counter2Access = accessMode; state->Counter2IsWriteHighNext = 0; }
            PitCounterControl(state, &state->Counter2, byteValue);  /* latch command included          */
            return;
        }
        if (counterIndex == PIT_SELECT_COUNTER1) { PitCounterControl(state, &state->Counter1, byteValue); return; }
        if (counterIndex == PIT_SELECT_READ_BACK) { PitReadBack(state, byteValue); return; }
        if (accessMode == PIT_ACCESS_LATCH) {                      /* latch count command             */
            state->Latch = (WORD)PitCurrentCount(state);
            state->IsLatched = 1; state->IsReadHighNext = 0;
        } else {
            /* A Control Word arms the next count write to load and restart (see
               PitLoad) and clears the count register ("CRM and CRL are cleared when
               the Counter is programmed"), so a parked bare write is discarded. The
               counting element is NOT stopped: the datasheet halts counting on the first
               byte of a COUNT, not on the Control Word, and a guest that programs a
               mode and never writes a count (Lemmings' calibration exit: `out 43h,36h`
               then a read) must keep ticking at the old rate, as the real part does.
               ⚠ A latched count is deliberately KEPT across this: "(or until the
               Counter is reprogrammed)" would unlatch it, but the value the OL would
               then follow is the same CE a handful of clocks on, and Lemmings reads
               it right here -- keeping the latch is the same number without the
               mode-3 by-two arithmetic being applied to a mode-0 count. */
            /* ── MODES 6 AND 7 ARE NOT MODES. ─────────────────────────────────
                 The field is three bits wide but only six modes exist: 110 IS mode 2
                 and 111 IS mode 3 (docs/ref/pit.md 3). Storing the raw bits let every
                 later test -- `mode == 2` for the periodic bare-count load rule,
                 `mode == 3` for the decrement-by-two count law -- silently exclude a
                 guest that programmed the alias. Normalise for BEHAVIOUR here and
                 keep the raw bits for the read-back status byte, which a real 8254
                 reports UN-normalised (measured: pit.mode6.readback = 0x0C). */
            state->ProgrammedMode = (BYTE)((byteValue >> PIT_CW_MODE_SHIFT) & PIT_CW_MODE_MASK);
            state->Mode = (state->ProgrammedMode >= PIT_MODE_ALIAS_FIRST) ? (BYTE)(state->ProgrammedMode - PIT_MODE_ALIAS_OFFSET) : state->ProgrammedMode;
            state->Access = accessMode; state->IsBcd = (BYTE)(byteValue & PIT_CW_BCD); state->IsWriteHighNext = 0;
            state->IsControlWordArmed = 1; state->IsNextPending = 0;
            state->IsIrqArmed = 0;           /* a CW restarts the mode: OUT at its initial
                                            level, no edge until a count is loaded */
        }
    } else if (port == PIT_COUNTER0_PORT) {               /* channel-0 reload write          */
        /* ── ★★★★ A HALF-WRITTEN COUNT IS NOT A COUNT. ──────────────────────────────
             This used to READ-MODIFY-WRITE `reload` on every byte, so between a guest's
             two `out 40h` instructions the divisor was (old MSB | new LSB) -- a value
             the guest never asked for and, when the two halves disagree, one that can
             be arbitrarily small.
           ★ MEASURED ON ZAR (GH #23): it programs 0x8002 (36.41 Hz, near enough 2x the
             BIOS rate) as lo=0x02 then hi=0x80, arriving with reload=0 (65536). Under
             the old rule the FIRST byte alone left `reload = (0 & 0xFF00) | 0x02` =
             **2, i.e. 596,591 Hz**, and it stayed there for the whole gap between the
             two port writes. That gap is not microseconds for us: each `out`
             is a trap out of protected mode and back, and host_pit_sync() runs on ports
             0x40-0x43, so the transient rate got sampled. The run's own counters show
             the damage exactly: `raises=29657` in 45 s against a programmed 36.4 Hz,
             i.e. ~29,000 interrupts the 8254 never generated, each costing a
             SuspendThread round trip under the device lock and saturating the owed
             backlog (`owed_max=64`). The tell was `PIT-RELOAD 0x2 (hz=0x91a6f)` sitting
             in the log with nothing else out of range.
           ► THE 8254 BUFFERS. Writing the LSB does not load the counter; the count
             register is loaded when the MSB arrives. So buffer it and commit once --
             the guest's two writes become ONE rate change, which is what the hardware
             does and what every guest is written against.
           ⚠ AND LSB-ONLY / MSB-ONLY ZERO THE OTHER HALF (Intel 8254 datasheet). That
             is the same read-modify-write mistake in a second dress -- a guest that
             re-rates a channel with a single MSB write would inherit whatever LSB
             happened to be standing. ZAR does not take this path (it uses lo/hi), so
             this half is fidelity, fixed on its own merits and not on its evidence.
           ⚠⚠ THE PIT IS THE MOST SHARED PATH IN THIS PROJECT -- see the note on
             host_irq_sink's throttle, where a change measured on Doom cost SKYROADS a
             fifth of its clock. Re-gate BOTH before believing this. */
        /* The committed count goes through PitLoad, which decides (per mode and per
           whether a Control Word preceded it) between "load and restart now" and
           "take over at the end of the period". See the note above PitLoad. */
        BYTE accessMode = state->Access ? state->Access : PIT_ACCESS_LOW_HIGH;
        if (accessMode == PIT_ACCESS_LOW)      PitLoad(state, byteValue);                        /* LSB only: MSB := 0 */
        else if (accessMode == PIT_ACCESS_HIGH) PitLoad(state, (WORD)((WORD)byteValue << PIT_BYTE_SHIFT));  /* MSB only: LSB := 0 */
        else {                               /* lo then hi -- ONE atomic load   */
            if (!state->IsWriteHighNext) { state->WriteLow = byteValue; state->IsWriteHighNext = 1; }
            else { PitLoad(state, (WORD)(((WORD)byteValue << PIT_BYTE_SHIFT) | state->WriteLow)); state->IsWriteHighNext = 0; }
        }
    } else if (port == PIT_COUNTER2_PORT) {               /* channel-2 reload (speaker tone) */
        /* ── #256: THE SPEAKER DIVISOR FOLLOWS COUNTER 0's RULE (above). ───────────
             This was read-modify-written: an LSB-only or MSB-only write kept the stale
             other half, and the LSB of a lo/hi pair re-tuned the speaker for the gap
             before its MSB. The 8254 zeroes the other half on a single-byte access and
             loads a lo/hi count when the MSB arrives. Only the TONE moves here -- the
             counter view below (and IRQ0) is untouched. */
        BYTE accessMode = state->Counter2Access ? state->Counter2Access : PIT_ACCESS_LOW_HIGH;
        if (accessMode == PIT_ACCESS_LOW) state->Counter2Reload = (WORD)byteValue;                     /* MSB := 0 */
        else if (accessMode == PIT_ACCESS_HIGH) state->Counter2Reload = (WORD)((WORD)byteValue << PIT_BYTE_SHIFT);   /* LSB := 0 */
        else {                               /* lo then hi -- ONE load          */
            if (!state->Counter2IsWriteHighNext) { state->Counter2WriteLow = byteValue; state->Counter2IsWriteHighNext = 1; }
            else { state->Counter2Reload = (WORD)(((WORD)byteValue << PIT_BYTE_SHIFT) | state->Counter2WriteLow); state->Counter2IsWriteHighNext = 0; }
        }
        /* ...and the same byte, into the COUNTER view. The speaker only ever
           wanted a divisor; a guest that programs counter 2 to MEASURE something
           needs the load moment recorded too. */
        state->Counter2.Access = accessMode;
        PitCounterWriteCount(state, &state->Counter2, byteValue);
    } else if (port == PIT_COUNTER1_PORT) {               /* counter 1 -- DRAM refresh       */
        PitCounterWriteCount(state, &state->Counter1, byteValue);
    }
}

static VOID PitPortOut(PVOID context, WORD port, BYTE width, UINT32 value)
{
    PPIT_STATE state = (PPIT_STATE)context;
    (VOID)width;
    PIT_GUARD(state, 1);
    PitPortOutLocked(state, port, (BYTE)value);
    PIT_GUARD(state, 0);
}

static VOID PitPortInLocked(PPIT_STATE state, WORD port, UINT32 *value)
{
    WORD count; BYTE accessMode;
    INT counterIndex = (port == PIT_COUNTER0_PORT) ? PIT_COUNTER0 : (port == PIT_COUNTER1_PORT) ? PIT_COUNTER1 : (port == PIT_COUNTER2_PORT) ? PIT_COUNTER2 : PIT_NO_COUNTER;
    /* A LATCHED STATUS IS READ BEFORE ANYTHING ELSE, and consumed by that read.
       When read-back latched both, the status comes out first and the count
       after it (docs/ref/pit.md 4). */
    if (counterIndex >= 0 && state->StatusLatched[counterIndex]) {
        *value = state->StatusLatch[counterIndex]; state->StatusLatched[counterIndex] = 0; return;
    }
    if (port == PIT_COUNTER1_PORT) { PitCounterReadCount(state, &state->Counter1, value); return; }
    if (port == PIT_COUNTER2_PORT) { PitCounterReadCount(state, &state->Counter2, value); return; }
    /* 0x43 is write-only on the part; a read is undefined. Answer consistently
       rather than plausibly -- see docs/inventory/pit.md 1. */
    if (port != PIT_COUNTER0_PORT) { *value = PIT_UNDRIVEN_BUS; return; }
    count = PitCountBytes(state->IsBcd, state->IsLatched ? state->Latch : PitCurrentCount(state));
    accessMode = state->Access ? state->Access : PIT_ACCESS_LOW_HIGH;
    if (accessMode == PIT_ACCESS_LOW) { *value = count & PIT_LOW_BYTE; state->IsLatched = 0; }
    else if (accessMode == PIT_ACCESS_HIGH) { *value = (count >> PIT_BYTE_SHIFT) & PIT_LOW_BYTE; state->IsLatched = 0; }
    else {                                   /* lo then hi                      */
        if (!state->IsReadHighNext) { *value = count & PIT_LOW_BYTE; state->IsReadHighNext = 1; }
        else { *value = (count >> PIT_BYTE_SHIFT) & PIT_LOW_BYTE; state->IsReadHighNext = 0; state->IsLatched = 0; }
    }
}

static VOID PitPortIn(PVOID context, WORD port, BYTE width, UINT32 *value)
{
    PPIT_STATE state = (PPIT_STATE)context;
    (VOID)width;
    PIT_GUARD(state, 1);
    PitPortInLocked(state, port, value);
    PIT_GUARD(state, 0);
}

/* --- BIOS timer services -------------------------------------------------- */
/* The BIOS data area lives at segment 0x40; tick count = DWORD at 0040:006C,
   the 24-hour-rollover flag = BYTE at 0040:0070. */
static BYTE *PitBiosDataArea(PPIT_STATE state) { return (BYTE *)VddMapFlat(state->Bus, PIT_BIOS_DATA_SEGMENT, 0); }

/* INT 08h -- the BIOS timer tick (runs when the guest takes IRQ0). Increments
   the tick count and handles the midnight rollover.
   TODO(v86 wiring): the authentic path also chains INT 1Ch and EOIs the PIC;
   that needs the IVT/re-entry plumbing from slice-1b, so it is deferred. */
static VOID PitInt08(PVOID context, PNTVDD_REGISTERS registers)
{
    PPIT_STATE state = (PPIT_STATE)context;
    BYTE *biosData = PitBiosDataArea(state);
    (VOID)registers;
    VddPitBiosTick(state, (volatile UINT32 *)(biosData + PIT_BDA_TICK_COUNT), biosData + PIT_BDA_MIDNIGHT_FLAG);   /* + #262 witness */
}

/* INT 1Ah -- BIOS time-of-day. AH=00 get tick count (+ clear midnight flag),
   AH=01 set tick count. */
/* ── INT 1Ah's RTC HALF. ─────────────────────────────────────────────────────────
     AH=02h/04h fell into `default:` -- "RTC subfns not modelled yet" -- which leaves
     every register exactly as the caller passed it. Found by p_bios.asm: the probe
     poisons CX/DX and the poison came straight back (C1C1/D1D1), where the 6.22
     oracle answers with the time and date in BCD.
   ⚠ BCD IS THE CONTRACT. A guest that parses these as binary (every one of them --
     that is what the BIOS returns) reads garbage from a binary answer: 0x22 seconds
     is twenty-two, not thirty-four. */
static UINT PitToBcdByte(UINT value) { return ((value / PIT_DECIMAL_BASE) % PIT_DECIMAL_BASE) * PIT_BCD_TENS_WEIGHT + (value % PIT_DECIMAL_BASE); }

/* The clock is the HOST's to supply -- this file stays free of <time.h>, which the
   XP-targeting CRT does not link anyway. No clock installed means the call is NOT
   answered, exactly as before: a fabricated date would be worse than no answer. */
static INT PitRtc(PPIT_STATE state, PPIT_RTC_READING reading)
{
    if (!state->RtcNow) return 0;
    reading->Century = PIT_DEFAULT_CENTURY; reading->Year = 0; reading->Month = 1; reading->Day = 1; reading->DayOfWeek = 0;
    reading->Hour = 0;  reading->Minute = 0;  reading->Second = 0;
    state->RtcNow(state->RtcContext, reading);
    return 1;
}

INT VddPitSeedTimeOfDay(PPIT_STATE state)
{
    PIT_RTC_READING reading;
    BYTE *biosData;
    if (!state->Bus || !PitRtc(state, &reading)) return 0;
    if (reading.Hour > PIT_MAX_HOUR || reading.Minute > PIT_MAX_MINUTE || reading.Second > PIT_MAX_SECOND) return FALSE;
    biosData = PitBiosDataArea(state);
    *(UINT32 *)(biosData + PIT_BDA_TICK_COUNT) = VddPitTicksSinceMidnight(reading.Hour, reading.Minute, reading.Second);
    biosData[PIT_BDA_MIDNIGHT_FLAG] = 0;
    VddPitTickOwned(state, *(UINT32 *)(biosData + PIT_BDA_TICK_COUNT));   /* the BIOS's own count (#262) */
    return 1;
}

static VOID PitInt1A(PVOID context, PNTVDD_REGISTERS registers)
{
    PPIT_STATE state = (PPIT_STATE)context;
    BYTE *biosData = PitBiosDataArea(state);
    UINT32 *tickCount = (UINT32 *)(biosData + PIT_BDA_TICK_COUNT);
    switch (VddGetAh(registers)) {
    case PIT_1A_GET_TICKS:
        VddSetCx(registers, (WORD)(*tickCount >> PIT_TICKS_HIGH_SHIFT));
        VddSetDx(registers, (WORD)(*tickCount & PIT_TICKS_LOW_MASK));
        VddSetAl(registers, biosData[PIT_BDA_MIDNIGHT_FLAG]);
        biosData[PIT_BDA_MIDNIGHT_FLAG] = 0;
        registers->CarryFlag = 0;
        break;
    case PIT_1A_SET_TICKS:
        *tickCount = ((UINT32)VddGetCx(registers) << PIT_TICKS_HIGH_SHIFT) | VddGetDx(registers);
        biosData[PIT_BDA_MIDNIGHT_FLAG] = 0;
        registers->CarryFlag = 0;
        /* #262: the host moves DOS's clock to the new count (and takes it as the
           BIOS's own through VddPitTickTake); with no host, it is simply owned. */
        if (state->TicksSet) state->TicksSet(state->RtcContext, *tickCount);
        else VddPitTickOwned(state, *tickCount);
        break;
    case PIT_1A_GET_TIME: {                             /* get RTC time, BCD               */
        PIT_RTC_READING reading; if (!PitRtc(state, &reading)) break;
        VddSetCx(registers, (WORD)((PitToBcdByte(reading.Hour) << PIT_BYTE_SHIFT) | PitToBcdByte(reading.Minute)));
        /* DL = daylight-saving flag. 0 = standard time; we do not track a DST rule
           the guest could act on, and saying 1 would invite one. */
        VddSetDx(registers, (WORD)(PitToBcdByte(reading.Second) << PIT_BYTE_SHIFT));
        registers->CarryFlag = 0;
        break; }
    case PIT_1A_GET_DATE: {                             /* get RTC date, BCD               */
        PIT_RTC_READING reading; if (!PitRtc(state, &reading)) break;
        VddSetCx(registers, (WORD)((PitToBcdByte(reading.Century) << PIT_BYTE_SHIFT) | PitToBcdByte(reading.Year)));
        VddSetDx(registers, (WORD)((PitToBcdByte(reading.Month) << PIT_BYTE_SHIFT) | PitToBcdByte(reading.Day)));
        registers->CarryFlag = 0;
        break; }
    case PIT_1A_SET_TIME:                               /* set RTC time, BCD               */
    case PIT_1A_SET_DATE: {                             /* set RTC date, BCD               */
        /* ── GH #250: SET, BUT NOT THE HOST'S CLOCK. ──────────────────────────────
             These were refused because "we cannot move the host's clock" -- true, and
             not the question: the VDM's RTC is host-now plus an offset the host keeps
             (src/dos/dos_clock.h), and setting it moves only that. Measured on 6.22,
             PCem and DOSBox-X by p_clock.asm: the RTC reads back what was set
             (clk.1a02.after.1a03, clk.1a04.after.1a05) and DOS's own clock does NOT
             follow (clk.2c.after.1a03).
           ⚠ A REAL BIOS STORES WHATEVER IT IS GIVEN; this one cannot store a time no
             calendar has (invalid BCD, hour 25, February 30), so those are refused
             with CF=1 and the clock is left alone. DL (the DST flag) is accepted and
             not kept -- AH=02h reports standard time, see above. */
        PIT_RTC_READING requested;
        UINT bcdCh = VddGetCx(registers) >> PIT_BYTE_SHIFT, bcdCl = VddGetCx(registers) & PIT_LOW_BYTE, bcdDh = VddGetDx(registers) >> PIT_BYTE_SHIFT, bcdDl = VddGetDx(registers) & PIT_LOW_BYTE;
        INT isDate = (VddGetAh(registers) == PIT_1A_SET_DATE);
        UINT fields[PIT_FIELD_COUNT]; UINT fieldIndex; INT isInvalid = 0;
        fields[PIT_FIELD_0] = bcdCh; fields[PIT_FIELD_1] = bcdCl; fields[PIT_FIELD_2] = bcdDh; fields[PIT_FIELD_3] = bcdDl;
        for (fieldIndex = 0; fieldIndex < (isDate ? PIT_DATE_FIELDS : PIT_TIME_FIELDS); ++fieldIndex) {
            if ((fields[fieldIndex] & PIT_BCD_UNITS_MASK) > PIT_BCD_MAX_DIGIT || (fields[fieldIndex] >> PIT_BCD_FIELD_SHIFT) > PIT_BCD_MAX_DIGIT) isInvalid = TRUE;
            fields[fieldIndex] = (fields[fieldIndex] >> PIT_BCD_FIELD_SHIFT) * PIT_DECIMAL_BASE + (fields[fieldIndex] & PIT_BCD_UNITS_MASK);
        }
        requested.Century = requested.Year = requested.Month = requested.Day = requested.Hour = requested.Minute = requested.Second = requested.DayOfWeek = 0;
        if (isDate) { requested.Century = fields[PIT_FIELD_0]; requested.Year = fields[PIT_FIELD_1]; requested.Month = fields[PIT_FIELD_2]; requested.Day = fields[PIT_FIELD_3]; }
        else      { requested.Hour = fields[PIT_FIELD_0]; requested.Minute = fields[PIT_FIELD_1]; requested.Second = fields[PIT_FIELD_2]; }
        registers->CarryFlag = (BYTE)((!isInvalid && state->RtcSet && state->RtcSet(state->RtcContext, &requested, isDate)) ? 0 : 1);
        break; }
    default:
        /* 06h/07h (alarm) are not answered here; the alarm lives in the CMOS model
           (vdd_cmos.c), which a guest can program through ports 70h/71h. */
        break;
    }
}

/* --- lifecycle ------------------------------------------------------------ */
VOID VddPitReset(PVOID context)
{
    PPIT_STATE state = (PPIT_STATE)context;
    PVDD_BUS bus = state->Bus; UINT32 frameMicroseconds = state->FrameMicroseconds;
    VOID (*guardRoutine)(VOID *, INT) = state->Guard; VOID *guardContext = state->GuardContext;
    VOID (*rtcNow)(VOID *, PIT_RTC_READING *) = state->RtcNow; VOID *rtcContext = state->RtcContext;
    INT  (*rtcSet)(VOID *, const PIT_RTC_READING *, INT) = state->RtcSet;
    VOID (*ticksSet)(VOID *, UINT32) = state->TicksSet;
    UINT32 tickWitness = state->TickWitness;    /* 0040:006C is memory, not the chip: kept */
    UINT byteIndex; BYTE *stateBytes = (BYTE *)state;
    for (byteIndex = 0; byteIndex < sizeof(*state); ++byteIndex) stateBytes[byteIndex] = 0;     /* zero, then restore links */
    state->Bus = bus;
    state->Access = PIT_ACCESS_LOW_HIGH;
    /* ── COUNTER 0 COMES OUT OF POST IN A PERIODIC MODE, NOT MODE 0. ─────────
         Reset zeroes everything, which left mode = 0 -- a one-shot. Nothing had
         noticed, because the IRQ0 engine raises from the accumulator regardless
         of mode; what it got wrong was everything a guest can ASK. Read-Back
         reported mode 0 where a real machine says mode 2 (measured:
         p_pit.asm pit.rdback.st0 = 0x34 vs our 0x30), and the bare-count load
         rule took the one-shot path -- reloading immediately instead of at the
         end of the period.
       ⚠⚠ THE MODE IS A BIOS CHOICE, AND PCem HAS NOW SETTLED IT: **MODE 3**.
         Three answers, and the majority was wrong: SeaBIOS/QEMU leaves mode 2,
         dosbox-x mode 3, and a REAL AMI 486 BIOS with a genuine IBM VGA ROM
         leaves mode 3 (p_pit pit.rdback.st0 = 0x36). This project targets a
         period-correct PC, so 3 is the answer that matters.
       ⛔ AND docs/ref/pit.md SAID MODE 3 FIRST, FROM MEMORY, AND I "CORRECTED" IT
         TO 2 ON THE STRENGTH OF ONE ORACLE. The original claim was right. A
         single-oracle measurement is not more trustworthy than a remembered fact
         just because it is a measurement -- it is one machine's answer, and this
         one happened to be the unrepresentative machine. */
    state->Mode = PIT_MODE_3; state->ProgrammedMode = PIT_MODE_3;
    state->FrameMicroseconds = frameMicroseconds ? frameMicroseconds : PIT_DEFAULT_FRAME_US;
    state->Guard = guardRoutine; state->GuardContext = guardContext;        /* the lock survives a reset */
    state->RtcNow = rtcNow; state->RtcContext = rtcContext; state->RtcSet = rtcSet;   /* and so does the clock */
    state->TicksSet = ticksSet;
    state->TickWitness = tickWitness;
    /* ── COUNTER 1 IS FREE-RUNNING BEFORE ANYONE PROGRAMS IT. ────────────────
         On a PC the BIOS sets it to mode 2, divisor 18 for DRAM refresh and then
         leaves it alone forever; its gate is tied high. A guest that simply READS
         it -- which is the whole reason timing loops use counter 1, nothing else
         touches it -- must see a counter in motion. Left at reset's zeros it
         would read a constant, which is the "no time is passing" answer this
         surface was measured to be giving. */
    state->Counter1.Reload = PIT_REFRESH_DIVISOR; state->Counter1.Access = PIT_ACCESS_LOW_HIGH; state->Counter1.Mode = PIT_MODE_2; state->Counter1.ProgrammedMode = PIT_MODE_2;
    state->Counter1.Gate = 1;                        /* tied high on the board */
    /* Counter 2 is NOT free-running: its gate is port 61h bit 0 and software
       decides. Left at reset defaults until a guest programs it. */
    state->Counter2.Access = PIT_ACCESS_LOW_HIGH;
}

INT VddPitInitialize(PVDD_BUS bus, PVOID context)
{
    PPIT_STATE state = (PPIT_STATE)context;
    state->Bus = bus;
    if (!state->FrameMicroseconds) state->FrameMicroseconds = PIT_DEFAULT_FRAME_US;
    if (!state->Access)   state->Access = PIT_ACCESS_LOW_HIGH;
    /* ⚠ THE POST DEFAULTS BELONG HERE TOO, NOT ONLY IN VddPitReset. The host
         builds g_pit as a zeroed global and calls init; it does NOT call reset on
         the startup path, so a default written only into reset is a default the
         running host never gets. Putting mode 2 in reset alone left Read-Back
         still reporting mode 0 on the rig -- the change measured as having done
         nothing, which is how a fix that is not wired up looks. */
    if (!state->Mode && !state->ProgrammedMode) { state->Mode = PIT_MODE_3; state->ProgrammedMode = PIT_MODE_3; }
    if (!state->Counter1.Reload) {                       /* counter 1: free-running refresh */
        state->Counter1.Reload = PIT_REFRESH_DIVISOR; state->Counter1.Access = PIT_ACCESS_LOW_HIGH; state->Counter1.Mode = PIT_MODE_2; state->Counter1.ProgrammedMode = PIT_MODE_2;
        state->Counter1.Gate = 1;                    /* tied high on the board */
    }
    if (!state->Counter2.Access) state->Counter2.Access = PIT_ACCESS_LOW_HIGH;
    if (VddClaimPorts(bus, PIT_COUNTER0_PORT, PIT_CONTROL_PORT, PitPortIn, PitPortOut, state)) return PIT_FAILED;
    if (VddClaimInterrupt(bus, PIT_TIMER_VECTOR, PitInt08, state)) return PIT_FAILED;
    if (VddClaimInterrupt(bus, PIT_TIME_OF_DAY_VECTOR, PitInt1A, state)) return PIT_FAILED;
    if (VddOnFrame(bus, PitFrame, state)) return PIT_FAILED;
    return PIT_OK;
}
