/* vdd_gus.h -- the Gravis UltraSound (GF1), north star 2.
 *
 * The hardware reference is docs/ref/gus.md (from the UltraSound SDK v2.22); what this
 * model does and does not do is docs/inventory/gus.md. Section numbers below (§n) are
 * the reference's.
 *
 * The GUS is a synthesizer with its own memory, not a DAC: the guest uploads samples
 * into on-card DRAM (programmed I/O at 3X7, or DMA), then points up to 32 voices at
 * them. This model renders the voices at the GF1's own service rate -- one output sample
 * per pass over the active voices -- and the audio mixer resamples that to the output.
 */
#ifndef NTVDMEX_VDD_GUS_H
#define NTVDMEX_VDD_GUS_H
#include "vdd_bus.h"
#include "vdd_dma.h"

#define GUS_DEFAULT_BASE 0x240      /* 220h is the Sound Blaster's (ref §1)          */
#define GUS_DEFAULT_IRQ  11         /* user decision 2026-09-25; slave delivery s80  */
#define GUS_DEFAULT_DMA  3
#define GUS_BASE_FIRST   0x210      /* the Audio page's base choices: 210h-260h       */
#define GUS_BASE_STEP    0x10
#define GUS_BASE_LAST_CHOICE    5
#define GUS_DEFAULT_BASE_CHOICE 3
#define GUS_DEFAULT_IRQ_CHOICE  4
#define GUS_DEFAULT_DMA_CHOICE  1
/* Where the GUS steps aside to when the Sound Blaster holds its default resource. */
#define GUS_FALLBACK_BASE 0x260
#define GUS_FALLBACK_IRQ  12
#define GUS_FALLBACK_DMA  1
#define GUS_DRAM_SIZE    (1024u * 1024u)   /* 20 address bits (ref §3)               */
#define GUS_VOICES       32
#define GUS_STEREO_CHANNELS 2
#define GUS_DEVICE_NAME  "gus"

/* #190: the GUS's own MIDI port is a 6850 UART (ref §9): it transmits BYTES, not
   messages. The card hands every transmitted byte to this sink, exactly as the wire
   would carry it. The host turns bytes into messages for the synth by feeding a
   private MPU assembler (VddMpuFeed in vdd_mpu.h) whose own sink is the one the
   MPU-401 uses -- so the GUS and the MPU reach the same synth, and each keeps its own
   running status. No sink = bytes go nowhere, as with nothing plugged into MIDI OUT. */
typedef VOID (*PGUS_MIDI_SINK_ROUTINE)(PVOID context, BYTE value);

/* 6850 status bits (ref §9) */
#define GUS_ACIA_RECEIVE_FULL 0x01  /* receive data register full                  */
#define GUS_ACIA_TRANSMIT_EMPTY 0x02          /* transmit data register empty                */
#define GUS_ACIA_OVERRUN 0x20       /* receiver overrun                            */
#define GUS_ACIA_IRQ  0x80          /* the ACIA is requesting an interrupt         */

typedef struct _GUS_VOICE {
    BYTE  Control;          /* 00h: 0 stopped 1 stop 2 16-bit 3 loop 4 bidi 5 IRQ en 6 dir 7 IRQ pend */
    WORD FrequencyControl;  /* 01h: frequency control, 6.9 fixed point in bits 15-1  */
    UINT32 Start, End;      /* 02h-05h, in 1/512-sample units (address << 9 | frac)  */
    UINT32 Position;        /* 0Ah/0Bh: current position, same units                 */
    BYTE  RampRate;         /* 06h: bits 5-0 step, 7-6 update rate                   */
    BYTE  RampStart;        /* 07h: EEEEMMMM                                         */
    BYTE  RampEnd;          /* 08h                                                   */
    WORD Volume;            /* 09h: EEEE MMMMMMMM xxxx (the 12-bit volume in 15-4)   */
    BYTE  Pan;              /* 0Ch                                                   */
    BYTE  VolumeControl;    /* 0Dh: 0 stopped 1 stop 2 rollover 3 loop 4 bidi 5 IRQ en 6 dir 7 IRQ pend */
    UINT32 RampDivider;     /* services since the last ramp step                     */
} GUS_VOICE, *PGUS_VOICE;
typedef const GUS_VOICE *PCGUS_VOICE;

typedef struct _GUS_STATE {
    PVDD_BUS   Bus;
    PDMA_STATE Dma;
    WORD   BasePort;
    BYTE    Irq, DmaChannel;         /* ULTRASND's GF1 IRQ and DRAM DMA               */
    BYTE    MidiIrq, RecordDma;      /* ULTRASND's MIDI IRQ and record DMA; 0 = the same
                                        as Irq / DmaChannel (the SDK's "combine")         */
    BYTE   *Dram;                    /* GUS_DRAM_SIZE bytes, owned by the host      */

    /* the indirect register file (ref §2) */
    BYTE    VoicePage, RegisterSelect;
    WORD   LowByteLatch;             /* 3X4 byte written ahead of 3X5 for a 16-bit reg */

    /* globals (ref §2.1) */
    BYTE    ResetRegister;           /* 4Ch: bit 0 run, 1 DAC, 2 master IRQ         */
    BYTE    ActiveVoices;            /* 14..32                                       */
    BYTE    DmaControl;              /* 41h as written                               */
    BYTE    IsDmaTerminalCount;      /* 41h bit 6 on read: TC pending                */
    WORD   DmaAddress;               /* 42h: DRAM address bits 19-4                  */
    BYTE    IsDmaWaiting;            /* 41h go written, 8237 not ready yet           */
    UINT32   DramIoAddress;          /* 43h/44h                                      */
    BYTE    TimerControl;            /* 45h                                          */
    BYTE    Timer1Load, Timer2Load;  /* 46h/47h                                      */
    BYTE    SampleFrequency, SampleControl, IsSampleTerminalCount;   /* 48h/49h: recording (silence)  */
    UINT32   SampleAccumulatorNs;    /* GF1 time towards the next ADC sample         */
    BYTE    SamplePending;           /* ADC bytes not yet moved (a 16-bit channel
                                        moves them in pairs)                          */
    BYTE    JoystickTrim;            /* 4Bh                                          */

    /* board (ref §5, §9) */
    BYTE    MixControl;              /* 2X0                                          */
    BYTE    IsLatchArmed;            /* 2X0 was the last write: 2XB may be written   */
    BYTE    IrqLatch, DmaLatch;      /* 2XB bank 0, as written (bit 7 dropped)       */
    /* ...and what they DECODE to (ref §5): the lines the card actually drives. 0 = none.
       Reset loads them from Irq/DmaChannel/MidiIrq/RecordDma -- the state ULTRINIT leaves a
       card in, which is what every DOS program finds at its start (see VddGusReset). */
    BYTE    Gf1IrqLine, MidiIrqLine, DramDmaLine, RecordDmaLine;
    BYTE    RegisterControl;         /* 2XF: the bank 2XB reaches (0, 5, 6)          */
    BYTE    RegisterClear;           /* 2XB bank 5: "write 0 to clear power-up IRQs" */
    BYTE    Jumper;                  /* 2XB bank 6: bit 1 MIDI decode, bit 2 joystick */
    BYTE    AdlibIndex;              /* 2X8 write                                    */
    BYTE    AdlibMask;               /* 2X9 bits 5/6                                 */
    BYTE    IsTimer1Running, IsTimer2Running;
    BYTE    Timer1Value, Timer2Value;
    BYTE    IsTimer1Expired, IsTimer2Expired;       /* expired flags (2X8 read, 2X6 bits 2/3)       */
    UINT32   Timer1AccumulatorNs, Timer2AccumulatorNs;
    BYTE    MidiControl;             /* 3X0 write: 6850 control                      */
    BYTE    MidiStatus;              /* 6850 status bits RDRF/TDRE/OVRN (IRQ derived) */
    BYTE    MidiReceive;             /* 6850 receive data register                   */
    PGUS_MIDI_SINK_ROUTINE MidiSink; PVOID MidiSinkContext;   /* host-owned; survives reset   */
    BYTE    IsLineUp;                /* the GF1 IRQ line is asserted (edge detection) */
    BYTE    IsMidiLineUp;            /* the MIDI IRQ line, when it is a separate one */

    GUS_VOICE  Voices[GUS_VOICES];

    /* diagnostics */
    UINT32   IoWrites, IoReads, DramPokes, DramPeeks, DmaUploads, DmaBytes;
    UINT32   VoiceStarts, IrqsRaised, FifoReads, Renders, SamplesOut;
    UINT32   LatchLockedOut;         /* 2XB writes refused by the lock-out           */
    UINT32   DmaDownloads, DmaDownloadBytes;    /* card -> PC DRAM reads (41h bit 1) */
    UINT32   SampleTakes, SampleBytes;          /* record (49h) takes and bytes      */
    UINT32   MidiTransmitted, MidiReceivedBytes;          /* 6850 bytes out / looped back in   */
    UINT32   OutputMuted;            /* samples rendered while 2X0 bit 1 cut line out */
    UINT32   OutputNonZero;          /* output samples that were not silence         */
    UINT32   OutputPeak;             /* largest |sample| produced                    */
} GUS_STATE, *PGUS_STATE;
typedef const GUS_STATE *PCGUS_STATE;

INT  VddGusInitialize(_In_ PVDD_BUS bus, _In_ PVOID context);
VOID VddGusReset(_In_ PVOID context);
static inline NTVDD_DEVICE VddGusDevice(_In_ PGUS_STATE state)
{ NTVDD_DEVICE device; device.Name = GUS_DEVICE_NAME; device.Initialize = VddGusInitialize; device.Reset = VddGusReset;
  device.Shutdown = 0; device.Context = state; return device; }

/* The GF1's output rate for the current active-voice count (ref §4), in Hz. */
UINT32 VddGusRateHz(_In_ PCGUS_STATE state);

/* Render `count` mono samples at VddGusRateHz(), advancing every voice, volume ramp and
   timer by that much GF1 time, and raising the interrupts that time produces. */
VOID VddGusRender(_Inout_ PGUS_STATE state, _Out_writes_(count) INT16 *output, _In_ UINT32 count);
/* #189: the same, as panned interleaved L/R pairs (2*count samples). */
VOID VddGusRenderStereo(_Inout_ PGUS_STATE state, _Out_writes_(GUS_STEREO_CHANNELS * count) INT16 *output, _In_ UINT32 count);

/* The linear gain (Q16) of a 12-bit GF1 volume (ref §7). Exposed for the test. */
UINT32 VddGusVolumeGain(_In_ WORD volume12);

#endif /* NTVDMEX_VDD_GUS_H */
