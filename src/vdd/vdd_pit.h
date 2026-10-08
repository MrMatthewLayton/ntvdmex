/*
 * vdd_pit.h -- the PIT (8254) timer VDD.  (M3 slice-2, ADR-0008)
 *
 * The simplest device: it validates the bus end-to-end before the heavy video
 * work.  It models Intel 8254 channel 0 (ports 0x40-0x43), converts elapsed
 * wall-clock (delivered as the bus frame tick) into IRQ0 pulses at the
 * programmed rate (~18.2065 Hz by default), and provides the BIOS timer
 * services INT 08h (tick bookkeeping at 0040:006C) and INT 1Ah (time-of-day).
 *
 * Pure C, no <windows.h>: all device state is explicit and the only outside
 * effects go through the bus (raise_irq, map_flat), so the whole VDD is
 * exercised off-VM by tests/unit/pit_test.c.
 */
#ifndef NTVDMEX_VDD_PIT_H
#define NTVDMEX_VDD_PIT_H

#include "vdd_bus.h"

#define PIT_INPUT_HZ     1193182u      /* 8254 input clock                      */
#define PIT_INPUT_HZ_ULL 1193182ull
#define PIT_TICKS_PER_DAY 0x1800B0u    /* 1,573,040 INT 8 ticks / 24h (BIOS)    */
#define PIT_PORT_COUNTER0 0x40         /* channel 0's count                     */
#define PIT_PORT_CONTROL  0x43         /* the mode/command register             */
#define PIT_COUNTERS      3              /* counters 0, 1 and 2                   */
#define PIT_BINARY_WRAP   0x10000u       /* a count of 0 means 65536 in binary... */
#define PIT_BCD_WRAP      10000u         /* ...and 10000 in four-decade BCD       */
#define PIT_BCD_DIGIT     0xF            /* one BCD decade                        */
#define PIT_BCD_TENS_SHIFT      4
#define PIT_BCD_HUNDREDS_SHIFT  8
#define PIT_BCD_THOUSANDS_SHIFT 12
#define PIT_BCD_TEN       10u
#define PIT_BCD_HUNDRED   100u
#define PIT_BCD_THOUSAND  1000u
#define PIT_SECONDS_PER_HOUR    3600u
#define PIT_SECONDS_PER_MINUTE  60u
#define PIT_CLOCKS_PER_TICK     65536u   /* one BIOS tick: a full count of 65536  */
#define PIT_DEFAULT_FRAME_US 16667u    /* ~60 Hz host frame tick                */
#define PIT_DEVICE_NAME   "pit"

/* A wall-clock reading, in ordinary binary -- INT 1Ah converts to BCD at the edge. */
/* dow: 1 = Sunday .. 7 = Saturday, the MC146818's own numbering; 0 = unknown (s81, #182). */
typedef struct _PIT_RTC_READING { UINT Century, Year, Month, Day, Hour, Minute, Second, DayOfWeek; } PIT_RTC_READING, *PPIT_RTC_READING;

typedef const PIT_RTC_READING *PCPIT_RTC_READING;

/* ── ★★★ BCD IS A BOUNDARY FORMAT, NOT A SECOND SET OF ARITHMETIC. ───────────────
     Control Word bit 0 selects four-decade BCD counting: the counter runs
     9999 -> 0000 and a written count of 0000 means 10000, not 65536 (Intel 8254,
     231164-005, "Control Word Format" and "Write Operations"). The naive shape is
     to teach every count law to step in decimal; the cheap and exactly equivalent
     one is to keep ALL internal state binary -- reload, latch, the count laws,
     the IRQ0 divisor -- and convert only where a guest's bytes cross the port:
     DECODE on a count write, ENCODE on a count read. The counting element is then
     the same code in both modes, which is the point: a second arithmetic path is a
     second thing to get wrong, and only one of the two would ever be exercised.
   ⚠ INVALID DIGITS ARE NOT DEFINED BY THE DATASHEET. A guest may write 0x1A into a
     decade that only has states 0-9. We decode each nibble AT ITS DECADE WEIGHT
     (0x1A -> 20), because that reproduces what four decade down-counters actually
     do next -- 0x1A, 0x19, 0x18 ... -- rather than inventing a clamp. The value
     re-enters the legal range within ten clocks and stays there.
   ⚠ docs/ref/pit.md 3 is the paragraph this implements; docs/inventory/pit.md 2
     records that NO oracle we have models BCD, so this is spec-implemented and
     UNVERIFIABLE against a machine. It is the one surface here where the
     datasheet outranks the emulators. */
static inline UINT32 PitFromBcd(WORD value)
{ return (UINT32)((value & PIT_BCD_DIGIT) + ((value >> PIT_BCD_TENS_SHIFT) & PIT_BCD_DIGIT) * PIT_BCD_TEN
                  + ((value >> PIT_BCD_HUNDREDS_SHIFT) & PIT_BCD_DIGIT) * PIT_BCD_HUNDRED + ((value >> PIT_BCD_THOUSANDS_SHIFT) & PIT_BCD_DIGIT) * PIT_BCD_THOUSAND); }

static inline WORD PitToBcd(UINT32 value)
{ value %= PIT_BCD_WRAP;                      /* 10000 (the BCD maximum) reads back as 0000 */
  return (WORD)((((value / PIT_BCD_THOUSAND) % PIT_BCD_TEN) << PIT_BCD_THOUSANDS_SHIFT) | (((value / PIT_BCD_HUNDRED) % PIT_BCD_TEN) << PIT_BCD_HUNDREDS_SHIFT)
                  | (((value / PIT_BCD_TEN) % PIT_BCD_TEN) << PIT_BCD_TENS_SHIFT) | (value % PIT_BCD_TEN)); }

/* The wrap of the counting element: where a count that runs past zero comes back. */
static inline UINT32 PitWrap(BYTE isBcd) { return isBcd ? PIT_BCD_WRAP : PIT_BINARY_WRAP; }

/* ── COUNTERS 1 AND 2, AS COUNTERS. ──────────────────────────────────────────
   Counter 0 keeps its own flat fields below and is DELIBERATELY not folded in
   here. It carries the IRQ0 engine, the accumulator and the pacer's guard, it is
   the hottest path in the device, and s61 measured what happens when its timing
   is disturbed. A refactor that moved it would be a second change riding along
   with this one; the duplication is the cheaper risk.

   What these two need is only what a guest can OBSERVE: a reload, an access mode
   and its read/write phases, a latch, and the moment the count was loaded --
   because a count read is computed from elapsed clocks, not stored. */
typedef struct _PIT_COUNTER {
    WORD Reload;        /* BINARY, decoded at the write; 0 => the maximum   */
    BYTE  Access;        /* 1=lo, 2=hi, 3=lo/hi                              */
    BYTE  Mode;          /* EFFECTIVE mode 0-5 (6/7 normalised)              */
    BYTE  ProgrammedMode;      /* as programmed -- for the Read-Back status byte    */
    BYTE  IsBcd;           /* control word bit 0: counts in four-decade BCD    */
    BYTE  IsWriteHighNext;       /* lo/hi write phase                                */
    BYTE  WriteLow;         /* LSB buffered in lo/hi mode                       */
    BYTE  IsReadHighNext;       /* lo/hi read phase                                 */
    BYTE  IsLatched;       /* a snapshot is frozen for reading                 */
    WORD Latch;         /* ...that snapshot                                 */
    UINT64 LoadClocks;   /* total_clocks when the count was last loaded      */
    BYTE  IsNullCount;      /* a count is written but not yet in the counting element */
    BYTE  Gate;          /* GATE input: 1 = counting. Counter 2's comes from
                               port 61h bit 0; counters 0 and 1 are tied high on a
                               PC and never use this.                            */
    UINT64 GateElapsed;  /* clocks counted when the gate last went LOW, so a
                               gate that comes back high RESUMES rather than
                               restarts -- which is the difference between a
                               paused stopwatch and a reset one (modes 0 and 4). */
    BYTE  IsTriggered;          /* #175: modes 1/5 -- a GATE rising edge has started
                               the count since the Control Word / count write.   */
} PIT_COUNTER, *PPIT_COUNTER;

typedef const PIT_COUNTER *PCPIT_COUNTER;

typedef struct _PIT_STATE {
    PVDD_BUS Bus;
    WORD Reload;        /* channel-0 reload latch, BINARY (0 => the maximum) */
    BYTE  Access;        /* access mode: 1=lo, 2=hi, 3=lo/hi                  */
    BYTE  Mode;          /* EFFECTIVE mode 0-5 (shapes the count read-back)   */
    BYTE  ProgrammedMode;      /* the three bits AS PROGRAMMED. Modes 6 and 7 do not
                               exist -- 110 IS mode 2 and 111 IS mode 3 -- so `mode`
                               is normalised for BEHAVIOUR. But MEASURED on a real
                               8254 (tests/probes/dos/p_pit.asm, pit.mode6.readback =
                               0x0C): the Read-Back status byte reports the bits the
                               guest WROTE, un-normalised. Keeping both is what lets
                               the behaviour be right and the read-back be honest. */
    BYTE  IsWriteHighNext;       /* lo/hi write phase (0 => lo next)                 */
    BYTE  WriteLow;         /* the LSB written so far in lo/hi mode -- see PitPortOut */
    BYTE  IsReadHighNext;       /* lo/hi read phase                                 */
    BYTE  IsLatched;       /* a count snapshot is latched for reading          */
    WORD Latch;         /* the latched count                                */
    UINT64 TotalClocks;  /* monotonic PIT input clocks (for count reads)     */
    UINT64 LoadClocks;   /* total_clocks when the count was last loaded: the
                               counting element runs from HERE (see PitCurrentCount) */
    UINT64 Accumulator;         /* clocks not yet turned into IRQ0 pulses           */
    BYTE  IsControlWordArmed;      /* a Control Word was written since the last load: the
                               next count write LOADS and RESTARTS the period, in every
                               mode (8254: "synchronized by software"). See PitLoad. */
    BYTE  IsNextPending;  /* modes 2/3, count written WITHOUT a Control Word: it is
                               held here and loaded at the end of the current period  */
    WORD NextReload;   /* ...that held count                                */
    BYTE  IsIrqArmed;     /* modes 0/4: a count was loaded and its terminal count
                               has not raised IRQ0 yet. OUT rises ONCE per count in a
                               one-shot mode, and the PIC counts rising edges (#175). */
    UINT32 OneShotLoads; /* counter-0 counts loaded in modes 0/1/4/5: whether a guest
                               was exposed to the one-shot IRQ0 rule at all (STAGE2) */
    UINT32 Restarts;      /* loads that RESTARTED the period (CW+count / one-shot
                               modes); the host watches it -- see host_pit_resync_check */
    UINT32 FrameMicroseconds;      /* microseconds per bus frame tick                  */
    WORD Counter2Reload;    /* channel-2 reload, the PC-speaker tone divisor -- the
                               RAW bytes as written, so VddPitCounter2Hz decodes BCD     */
    BYTE  Counter2Access;    /* channel-2 access mode (1=lo, 2=hi, 3=lo/hi)      */
    BYTE  Counter2IsWriteHighNext;   /* channel-2 lo/hi write phase                      */
    BYTE  Counter2WriteLow;     /* channel-2 LSB held until the MSB commits (#256)  */
    /* ⚠ ch2_reload/ch2_access/ch2_wr_flip above are the SPEAKER's view and stay
         authoritative for VddPitCounter2Hz(); c2 below mirrors them and adds what a
         COUNTER needs. Two views of one counter is not lovely, but rewiring the
         audio path is a separate change from making the port readable. */
    BYTE  IsBcd;           /* counter 0's control-word BCD bit: counts in BCD  */
    BYTE  StatusLatched[PIT_COUNTERS]; /* a Read-Back status byte is latched for this counter */
    BYTE  StatusLatch[PIT_COUNTERS];   /* ...that byte                                     */
    PIT_COUNTER Counter1;            /* counter 1 -- DRAM refresh, free-running          */
    PIT_COUNTER Counter2;            /* counter 2 -- the PC speaker, as a counter        */
    /* ── HOST SERIALIZATION HOOK (may be NULL, e.g. in the off-VM tests). ─────────
       A real 8254 counts on its own crystal, in parallel with the CPU; this model
       only counts when a thread runs its code, and s61 measured what happens when
       that thread has to queue behind the video renderer for the DEVICE lock: the
       clock stops, then lurches (86% of a played session's timing stalls). So the
       host drives VddPitAddClocks from a pacer thread under a PIT-ONLY lock --
       and these handlers, which arrive under the DEVICE lock, must take that same
       PIT lock or a guest reprogramming the reload races the pacer mid-count.
       The hook keeps this file pure C: enter=1 before counter state, enter=0 after. */
    VOID   (*Guard)(PVOID context, INT enter);
    PVOID    GuardContext;
    /* ── THE REAL-TIME CLOCK BEHIND INT 1Ah AH=02h/04h. ──────────────────────────
         Injectable so the battery can pin the exact BCD a known instant produces;
         NULL means the C library clock, which is what the host uses. The PIT owns
         these because it already owns INT 1Ah (the tick half of the same service). */
    VOID   (*RtcNow)(PVOID context, PPIT_RTC_READING reading);
    PVOID    RtcContext;
    /* ── AND THE OTHER DIRECTION: INT 1Ah AH=03h (what=0, hour/min/sec) and AH=05h
         (what=1, cent/year/month/day), decoded from BCD. Returns 1 if the clock took
         it. GH #250: the host moves the VDM's RTC offset, never the machine's clock.
         NULL = refused, as before (CF=1). Shares rtc_ctx. */
    INT    (*RtcSet)(PVOID context, PCPIT_RTC_READING reading, INT what);
    /* GH #262: INT 1Ah AH=01h set the tick count. On DOS the CLOCK$ driver reads the
       time of day FROM that count, so the host moves DOS's clock to match (measured:
       p_tick2c on 6.22, DOSBox-X and PCem all follow). NULL = the count alone. */
    VOID   (*TicksSet)(PVOID context, UINT32 ticks);
    /* ── GH #262 CASE B: WAS 0040:006C LAST WRITTEN BY THE BIOS? ─────────────────────
         tick_witness is the count the BIOS itself last left there (every increment, the
         seed, AH=01h's and AH=2Dh's reloads). A count that differs at the next increment
         or at the next DOS clock read was STORED by the guest -- and DOS's clock must
         then follow it (dos_clock.h, DosClockFollowTicks). From the first such sighting
         until the host takes it: tick_wraps = midnight rollovers, tick_since = BIOS
         ticks counted. One compare and a store per tick -- see VddPitBiosTick. */
    UINT32 TickWitness;
    BYTE  IsTickForeign;
    UINT32 TickWraps;
    UINT32 TickSince;
} PIT_STATE, *PPIT_STATE;

typedef const PIT_STATE *PCPIT_STATE;

/* ── THE BIOS TICK (INT 08h's bookkeeping), the ONE body for every place that does it:
     PitInt08, and the host's two inline bumps for a guest that cannot take IRQ0 right
     now (main.c: nested real-mode calls, a flat PM client with no INT 08h hook). Each of
     those used to carry its own copy; a copy that did not keep the witness would make
     every tick it counted look like a guest's store. ⚠ IRQ0 path: keep it trivial. */
static inline VOID VddPitBiosTick(PPIT_STATE state, volatile UINT32 *tickCount, volatile BYTE *midnightFlag)
{
    UINT32 count = *tickCount;
    if (count != state->TickWitness && !state->IsTickForeign) {
        state->IsTickForeign = 1; state->TickWraps = 0; state->TickSince = 0;
    }
    if (++count >= PIT_TICKS_PER_DAY) { count = 0; *midnightFlag = 1; if (state->IsTickForeign) state->TickWraps++; }
    if (state->IsTickForeign) state->TickSince++;
    *tickCount = count;
    state->TickWitness = count;
}

/* A count the BIOS/DOS itself just wrote (seed, AH=2Dh's reload): nothing foreign. */
static inline VOID VddPitTickOwned(PPIT_STATE state, UINT32 count)
{ state->TickWitness = count; state->IsTickForeign = 0; state->TickWraps = 0; state->TickSince = 0; }

/* Has the count been set by anything but the BIOS since the last call? If so, hand
   back what DOS's clock must follow (see DosClockFollowTicks), forget it, and take the
   count as the BIOS's own from here. 0 = nothing happened, outputs untouched. The
   caller holds whatever serializes it against the tick (the host: g_pit_cs). */
static inline INT VddPitTickTake(PPIT_STATE state, UINT32 count,
                                  UINT32 *takenTicks, UINT32 *takenWraps, UINT32 *takenSince)
{
    if (!state->IsTickForeign && count == state->TickWitness) return 0;
    if (!state->IsTickForeign) { state->TickWraps = 0; state->TickSince = 0; }   /* stored just now */
    *takenTicks = count; *takenWraps = state->TickWraps; *takenSince = state->TickSince;
    VddPitTickOwned(state, count);
    return 1;
}

/* Effective reload. `reload` is always BINARY (decoded at the write); 0 means the
   maximum, which is 65536 in binary counting and 10000 in BCD. */
static inline UINT32 VddPitEffectiveReload(PCPIT_STATE state)
{ return state->Reload ? state->Reload : PitWrap(state->IsBcd); }

/* The same rule for counters 1 and 2. */
static inline UINT32 VddPitCounterEffectiveReload(PCPIT_COUNTER counter)
{ return counter->Reload ? (UINT32)counter->Reload : PitWrap(counter->IsBcd); }

/* Channel-2 output frequency in Hz (the PC-speaker tone); 0 if not programmed.
   ⚠ ch2_reload is the SPEAKER's view and holds the RAW bytes the guest wrote
     (see the note in PIT_STATE), so a BCD guest's divisor has to be decoded here
     -- otherwise programming 0x1000 BCD sounds a tone 4096/1000 too low. */
static inline UINT32 VddPitCounter2Hz(PCPIT_STATE state)
{ UINT32 reload = state->Counter2.IsBcd ? PitFromBcd(state->Counter2Reload) : state->Counter2Reload;
  if (!reload) reload = PitWrap(state->Counter2.IsBcd);
  return PIT_INPUT_HZ / reload; }

/* Build the device descriptor to hand to VddBusAdd(). */
INT  VddPitInitialize(_In_ PVDD_BUS bus, _In_ PVOID context);
VOID VddPitReset(_In_ PVOID context);
static inline NTVDD_DEVICE VddPitDevice(_In_ PPIT_STATE state)
{ NTVDD_DEVICE device; device.Name = PIT_DEVICE_NAME; device.Initialize = VddPitInitialize; device.Reset = VddPitReset;
  device.Shutdown = 0; device.Context = state; return device; }

/* ── ★★ THE TICK COUNT IS THE TIME OF DAY. (GH #253) ─────────────────────────────
     0040:006C is not "ticks since the machine started": POST reads the RTC and sets it
     to the ticks since MIDNIGHT, and INT 1Ah AH=00h, DOS's clock, and every program
     that reads 006C directly all treat it as such. Ours was never seeded, so it counted
     up from whatever the VDM's low memory held -- zero, i.e. 00:00:00 at every launch.
     One tick is 65536 PIT input clocks, so the count at a given instant is
     seconds * 1193182 / 65536 (~18.2065/s), floored. 23:59:59 gives 1,573,024, under
     the BIOS's own rollover at PIT_TICKS_PER_DAY, so a seeded count can never start
     at or past midnight. Seconds resolution, as POST has: the RTC has no finer field. */
static inline UINT32 VddPitTicksSinceMidnight(UINT hour, UINT minute, UINT second)
{ UINT64 seconds = (UINT64)hour * PIT_SECONDS_PER_HOUR + (UINT64)minute * PIT_SECONDS_PER_MINUTE + second;
  return (UINT32)((seconds * PIT_INPUT_HZ) / PIT_CLOCKS_PER_TICK); }

/* Seed 0040:006C from rtc_now (the same clock INT 1Ah AH=02h answers from, so the
   two halves of INT 1Ah cannot disagree about the time) and clear the midnight flag
   at 0040:0070. Returns 1 if seeded; 0 -- touching nothing -- when there is no clock
   or it read an impossible time, which leaves the old count rather than a made-up one.
   Call once the PIT is on the bus (it writes through the bus's flat map). */
INT  VddPitSeedTimeOfDay(_Inout_ PPIT_STATE state);

/* Advance time by `clocks` PIT input clocks, emitting IRQ0 per elapsed reload.
   Exposed (not just driven by the frame tick) so tests can feed exact counts. */
VOID VddPitAddClocks(_Inout_ PPIT_STATE state, _In_ UINT32 clocks);

/* ── COUNTER 2'S GATE AND OUT PIN -- the PC's only software-visible pair. ─────
   The speaker VDD owns port 61h, so it pushes bit 0 in here and reads bit 5 back
   out. Keeping the PIT ignorant of the speaker (rather than having it reach for
   port 61h itself) is what lets the off-VM battery drive the gate directly. */
VOID VddPitCounter2Gate(_Inout_ PPIT_STATE state, _In_ INT isHigh);
INT  VddPitCounter2Out(_In_ PCPIT_STATE state);

#endif /* NTVDMEX_VDD_PIT_H */
