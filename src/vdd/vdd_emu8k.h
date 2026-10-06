/* vdd_emu8k.h -- the E-mu EMU8000, the wavetable synthesizer on the Sound Blaster AWE32 (#233).
 *
 * The specification is Creative's *AWE32/EMU8000 Programmer's Guide*, revision 1.00, Dave
 * Rossum, E-mu/Creative 1994-1996 (the "AWE32 Developer Information Pack"; see
 * docs/ref/SOURCES.md). Section and page references below (§n, p.n) are that guide's. What
 * this model does and does not do, register by register, is docs/inventory/emu8k.md.
 * Clean-room: written from the guide's prose, never from another emulator's EMU8000.
 *
 * The chip in one paragraph. 32 channels, each a sample-playback oscillator reading 16-bit
 * words out of "sound memory" (a 1 MB General MIDI ROM at word address 000000h and, from
 * 200000h up, DRAM the guest uploads into), with an interpolator, a resonant low-pass filter,
 * a volume stage and a pan/effects-send stage (p.19). Each channel's SOUND GENERATOR holds
 * CURRENT values (pitch CPF, volume and cutoff CVCF, address CCCA) that slew towards TARGET
 * values (PTRX, VTFT). An ENVELOPE GENERATOR per channel -- two DAHDSR envelopes and two LFOs,
 * from the "initial" registers IP/IFATN/PEFE/FMMOD/TREMFRQ/FM2FRQ2 -- overwrites those
 * targets every cycle unless it has been turned off (DCYSUSV bit 7). There is no "voice
 * done": a channel plays for ever and must always loop (§5); a note ends when its volume
 * envelope reaches zero.
 *
 * Ports (§2). Three groups, relative to the AWE's base E (BLASTER's `E`, 620h on a legacy
 * card at A220 -- i.e. SB base + 400h):
 *      E+000h  Data0   doubleword (LS word here, MS word at E+002h)
 *      E+400h  Data1   word or doubleword (MS word at E+402h)
 *      E+402h  Data2   word -- the SAME port as Data1's MS word: which one it is depends
 *                      on the register selected (see Emu8kIsData1DoubleWord in the .c)
 *      E+800h  Data3   word
 *      E+802h  Pointer word: bits 4-0 channel, bits 7-5 register number
 * The guide allows only word and doubleword transfers; a doubleword is the LS word to the
 * port and the MS word to the port two higher, in that order.
 *
 * No Windows calls, and NO C LIBRARY: the host links -nostdlib, so there is no libm here
 * either -- every exponential is a table built from one multiplication.
 */
#ifndef NTVDMEX_VDD_EMU8K_H
#define NTVDMEX_VDD_EMU8K_H
#include "../ntvdmex_types.h"
#include "vdd_bus.h"

#define EMU8K_DEFAULT_BASE  0x620          /* SB 220h + 400h (§2); BLASTER "E620"            */
#define EMU8K_VOICES        32
#define EMU8K_RATE_HZ       44100u         /* WC: 65536 counts per 1.486 s (p.13)            */
#define EMU8K_ADDR_MASK     0xFFFFFFu      /* sound memory addresses are 24 bits (p.9)       */
#define EMU8K_DRAM_BASE     0x200000u      /* ROM 000000h-1FFFFFh, DRAM from 200000h (§5)    */
#define EMU8K_DRAM_WORDS    0x40000u       /* the stock AWE32's 512 KB, in 16-bit words       */
#define EMU8K_ROM_WORDS_MAX 0x200000u      /* the space the ROM may occupy (§5)               */
/* The envelope engine's update period. The guide's time unit for every delay register is
   725 us (p.14-16); 32 sample periods at 44.1 kHz is 725.6 us. So the engine here runs
   once per 32 output samples and a delay count is simply a count of engine ticks. */
#define EMU8K_TICK          32u

#define EMU8K_PORT_GROUPS   3              /* Data0, Data1/2, Data3/Pointer (§2)             */
#define EMU8K_INIT_PROGRAMS 4              /* INIT1-4: the effects programs                  */
#define EMU8K_STREAMS       4              /* sound-memory DMA: left/right read, left/right write */
#define EMU8K_STEREO_SIDES  2              /* left, right                                    */

#define EMU8K_DEVICE_NAME   "emu8k"

/* One of the two DAHDSR envelopes (p.19: ENVELOPE 1 = modulation, ENVELOPE 2 = volume). */
typedef struct _EMU8K_ENVELOPE {
    BYTE     Phase;          /* EMU8K_ENV_*                                                  */
    DWORD    TickCount;      /* ticks spent in the current timed phase (delay, hold)         */
    INT32    Amplitude;      /* attack level, Q16 linear (0..65536)                          */
    INT32    Attenuation;    /* attenuation below peak after the attack, Q16 dB              */
} EMU8K_ENVELOPE, *PEMU8K_ENVELOPE;
enum { EMU8K_ENV_OFF = 0, EMU8K_ENV_DELAY, EMU8K_ENV_ATTACK, EMU8K_ENV_HOLD,
       EMU8K_ENV_DECAY, EMU8K_ENV_RELEASE, EMU8K_ENV_DONE };

typedef struct _EMU8K_VOICE {
    /* ---- the register file, as the guest sees it (p.6-7) ---- */
    DWORD    Cpf;            /* Data0 r0: current pitch (31-16), fractional address (15-0) */
    DWORD    Ptrx;           /* Data0 r1: pitch target, reverb send, aux byte              */
    DWORD    Cvcf;           /* Data0 r2: current volume (31-16), current cutoff (15-0)    */
    DWORD    Vtft;           /* Data0 r3: volume target, cutoff target                     */
    DWORD    Data0Register4, Data0Register5;  /* not in the guide's map -- stored, read back */
    DWORD    Psst;           /* Data0 r6: pan (31-24), loop start (23-0)                   */
    DWORD    Csl;            /* Data0 r7: chorus send (31-24), loop end (23-0)             */
    DWORD    Ccca;           /* Data1 r0: Q (31-28), DMA/WR/RIGHT (26-24), address (23-0)  */
    WORD     Envvol, Dcysusv, Envval, Dcysus;          /* Data1 r4-r7                      */
    WORD     Atkhldv, Lfo1val, Atkhld, Lfo2val;        /* Data2 r4-r7                      */
    WORD     Ip, Ifatn, Pefe, Fmmod, Tremfrq, Fm2frq2; /* Data3 r0-r5                      */
    WORD     Data3Register6, Data3Register7;  /* not in the map -- stored, read back        */

    /* ---- the engine's own state ---- */
    EMU8K_ENVELOPE VolumeEnvelope, ModulationEnvelope;  /* ENV2 and ENV1                    */
    DWORD    Lfo1Phase, Lfo2Phase;     /* triangle phase, a full cycle = 2^32              */
    DWORD    Lfo1Delay, Lfo2Delay;     /* ticks of LFO delay still to run                  */
    INT32    CurrentVolume;  /* current volume as CV << 14: slews to VT within a tick      */
    INT32    CurrentVolumeStep;
    INT32    GainLeft, GainRight;      /* pan gains, Q8 (0..256), latched at each tick     */
    /* the low-pass filter: direct form I, Q28 coefficients, cached for (cutoff, Q) */
    WORD     FilterCutoff; BYTE FilterQ, FilterIsValid, FilterIsBypassed;
    INT32    B0, B1, B2, A1, A2;
    INT32    X1, X2, Y1, Y2;
} EMU8K_VOICE, *PEMU8K_VOICE;

/* Optional host clock for WC, in microseconds (monotonic). NULL: WC counts rendered samples. */
typedef UINT64 (*PEMU8K_CLOCK_ROUTINE)(PVOID context);

typedef struct _EMU8K_STATE {
    VDD_BUS  *Bus;
    WORD      BasePort;              /* E: Data0 at E, Data1/2 at E+400h, Data3/Ptr at E+800h */
    PWORD     Dram;                  /* DramWords 16-bit words, owned by the host           */
    DWORD     DramWords;             /* 0 = none fitted                                     */
    PCWORD    Rom;                   /* the GM ROM image, RomWords long; NULL = reads zero  */
    DWORD     RomWords;
    PEMU8K_CLOCK_ROUTINE Clock; PVOID ClockContext;

    WORD      Pointer;               /* the Pointer register as written                     */
    BYTE      ByteLatch[EMU8K_PORT_GROUPS];  /* byte-access latches, one per port group     */
    WORD      Hwcf1, Hwcf2, Hwcf3;   /* Data1 r1 ch 29/30/31                                */
    DWORD     Hwcf4, Hwcf5, Hwcf6;   /* Data1 r1 ch 9/10/13                                 */
    WORD      EffectsInit[EMU8K_INIT_PROGRAMS][EMU8K_VOICES];  /* INIT1-4 (Data1/2 r2/r3)  */
    DWORD     Data1Register1[EMU8K_VOICES];  /* Data1 r1, channels the map does not name    */
    WORD      Data2Register1[EMU8K_VOICES];  /* Data2 r1, likewise                          */

    /* the four sound-memory DMA streams (§5): 0 left read, 1 right read, 2 left write,
       3 right write -- the same code CCCA bits 25-24 give a channel allocated to one */
    DWORD     SoundMemoryAddress[EMU8K_STREAMS];        /* SMALR, SMARR, SMALW, SMARW: bits 23-0 */
    WORD      SoundMemoryReadLatch[EMU8K_STEREO_SIDES]; /* SMLD/SMRD read (the prefetched word) */
    WORD      SoundMemoryWriteLatch[EMU8K_STEREO_SIDES];/* SMLD/SMRD write registers          */
    BYTE      SoundMemoryEmpty[EMU8K_STEREO_SIDES];     /* SMALR/SMARR bit 31                 */
    BYTE      SoundMemoryFull[EMU8K_STEREO_SIDES];      /* SMALW/SMARW bit 31                 */

    DWORD     WallClock;             /* sample counter (render-driven)                       */
    DWORD     TickPosition;          /* 0..EMU8K_TICK-1: where in the engine period we are  */
    EMU8K_VOICE Voices[EMU8K_VOICES];

    /* diagnostics */
    DWORD     IoWrites, IoReads, ByteIoCount;
    DWORD     SoundMemoryWordsWritten, SoundMemoryWordsRead, SoundMemoryRomWrites, SoundMemoryHeld;
    DWORD     NotesStarted, Releases, Renders, SamplesOut, NonZeroSamplesOut, PeakSampleOut;
} EMU8K_STATE, *PEMU8K_STATE;

typedef const EMU8K_STATE *PCEMU8K_STATE;

INT  VddEmu8kInitialize(_In_ VDD_BUS *bus, _In_ PVOID context);
VOID VddEmu8kReset(_In_ PVOID context);
static inline NTVDD_DEVICE VddEmu8kDevice(_In_ PEMU8K_STATE state)
{ NTVDD_DEVICE device; device.Name = EMU8K_DEVICE_NAME; device.Initialize = VddEmu8kInitialize; device.Reset = VddEmu8kReset;
  device.Shutdown = 0; device.Context = state; return device; }

/* The chip's output rate: fixed, 44.1 kHz (the WC period, p.13). */
static inline DWORD VddEmu8kRateHz(_In_ PCEMU8K_STATE state) { (VOID)state; return EMU8K_RATE_HZ; }

/* Render `frameCount` stereo frames (2*frameCount interleaved L/R samples) at VddEmu8kRateHz(),
   advancing every channel, envelope, LFO and the sample counter by that much chip time. */
VOID VddEmu8kRenderStereo(_Inout_ PEMU8K_STATE state, _Out_writes_(2 * frameCount) PINT16 output,
                          _In_ DWORD frameCount);

/* Exposed for the test: the attack time (us) of ATKHLDV/ATKHLD bits 6-0, and the decay /
   release time per dB (us) of DCYSUSV/DCYSUS bits 6-0. 0 = never / no decay. */
DWORD VddEmu8kAttackMicroseconds(_In_ BYTE rateCode);
DWORD VddEmu8kDecayMicrosecondsPerDb(_In_ BYTE rateCode);

#endif /* NTVDMEX_VDD_EMU8K_H */
