/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The ISA DMA controller VDD: a pair of Intel 8237As.  (sound epic)
 *
 * Sound Blaster playback is DMA, not port writes: the DSP is told "play N bytes"
 * and the 8237 feeds it from guest memory autonomously. So the SB VDD cannot be
 * built until a DMA controller exists, and this device lands first.
 *
 * Two cascaded controllers, as on every PC/AT:
 *   - controller 1, channels 0-3, 8-bit transfers, ports 0x00-0x0F
 *   - controller 2, channels 4-7, 16-bit transfers, ports 0xC0-0xDF (2x spacing)
 * plus the page registers at 0x80-0x8F, which supply the high address bits the
 * 8237's own 16-bit address register cannot reach.
 *
 * Address arithmetic differs per controller and is the classic place to get this
 * wrong: an 8-bit channel addresses BYTES as (page << 16) | addr, while a 16-bit
 * channel addresses WORDS as ((page & 0xFE) << 16) | (addr << 1), and its count
 * is in words too. Both count "transfers - 1", so a 100-byte block programs 99.
 *
 * A sound device does not read guest memory itself; it pulls through this VDD via
 * VddDmaRead(), which walks the current address, honours the decrement and
 * auto-init mode bits, and raises terminal count -- so auto-init ring buffers
 * (how every DOS game streams continuous audio) work without the caller knowing.
 *
 * Pure C, no <windows.h>: the only outside effect is VddMapLinear(), so the whole
 * controller is exercised off-VM by tests/unit/dma_test.c.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_VDD_DMA_H
#define NTVDMEX_VDD_DMA_H

#include "vdd_bus.h"

/* mode register (0x0B / 0xD6) bit fields */
#define DMA_MODE_CHANNEL                0x03    /* Which channel this mode byte programs */
#define DMA_MODE_TRANSFER               0x0C    /* 00 verify, 01 write(dev->mem), 10 read(mem->dev) */
#define DMA_MODE_TRANSFER_VERIFY        0x00
#define DMA_MODE_TRANSFER_WRITE         0x04
#define DMA_MODE_TRANSFER_READ          0x08
#define DMA_MODE_AUTOINIT               0x10    /* Reload base addr/count at terminal count */
#define DMA_MODE_DECREMENT              0x20    /* Walk the address downwards */
#define DMA_MODE_SELECT                 0xC0    /* 00 demand, 01 single, 10 block, 11 cascade */

/* THE COMMAND REGISTER (0x08 / 0xD0), per the Intel 8237A datasheet:
 * Written per controller, cleared by master clear. Bits 0-2 are honoured; the rest
 * are STORED and nothing reads them -- see docs/inventory/dma.md for why each is
 * safe to leave that way on a PC, and what honouring it would take.
 *   bit 0  memory-to-memory (ch0 -> ch1 through the temporary register)  HONOURED (#246)
 *   bit 1  channel 0 address hold (only meaningful with bit 0)           HONOURED (#246)
 *   bit 2  CONTROLLER DISABLE -- no DACK is given on ANY of its four     HONOURED
 *          channels, so no byte moves and no TC is reached; DREQs stay
 *          pending (status bits 7:4) until the guest re-enables it
 *   bit 3  compressed timing (a bus-cycle length: nothing to model)      stored
 *   bit 4  rotating priority (we serve one channel per call: no arbiter) stored
 *   bit 5  extended write (a strobe width: nothing to model)             stored
 *   bit 6  DREQ sense active-LOW                                         stored
 *   bit 7  DACK sense active-HIGH                                        stored
 *
 * [CAUTION]: Bits 6 and 7 are the board's WIRING contract, not a mode: a PC's cards drive
 * DREQ active-high, so a guest that flips bit 6 has told the chip every idle line
 * is a request. Nothing in the period software we know does it, and modelling it
 * would mean inventing transfers no device asked for.
 */
#define DMA_COMMAND_MEMORY_TO_MEMORY    0x01
#define DMA_COMMAND_ADDRESS_HOLD        0x02
#define DMA_COMMAND_DISABLE             0x04
#define DMA_COMMAND_COMPRESSED          0x08
#define DMA_COMMAND_ROTATE              0x10
#define DMA_COMMAND_EXTENDED_WRITE      0x20
#define DMA_COMMAND_DREQ_LOW            0x40
#define DMA_COMMAND_DACK_HIGH           0x80

/* status register (read 0x08 / 0xD0) */
#define DMA_STATUS_TERMINAL_COUNT       0x0F    /* Terminal count, ch 0-3 of this controller; clear on read */
#define DMA_STATUS_REQUEST              0xF0    /* Request pending, ch 0-3 of this controller; DERIVED */

/* WHO IS ASSERTING DREQ? THE DEVICE KNOWS; THE 8237 ONLY SEES THE PIN:
 * Status bits 7:4 report each channel's DREQ input. On the bus that is a wire the
 * card drives; here the card's own state already says whether it wants the bus (an
 * SB with a transfer armed, a GUS whose 41h "go" is waiting on the 8237), so the
 * bits are DERIVED at the moment of the status read rather than latched -- a second
 * copy of the same fact would only be a second thing to get out of step.
 * A device that can request DMA registers one function at init; it returns a mask
 * of the channels 0-7 whose DREQ it is asserting right now.
 *
 * [INFO]: Independent of the MASK and of the CONTROLLER-DISABLE bit, by design: those decide
 * whether the 8237 ANSWERS a request, not whether one is being made. A request that
 * the controller is refusing is exactly the one a status read must show.
 *
 * [CAUTION]: It is the wiring of the machine, so it SURVIVES VddDmaReset -- a guest's master
 * clear does not unplug the sound card.
 */
typedef BYTE (*PDMA_DREQ_ROUTINE)(PCVOID context);
#define DMA_DREQ_MAX                4

#define DMA_CHANNELS                8       /* Two 8237s: channels 0-3 and 4-7 */
#define DMA_CONTROLLER_8BIT         0       /* The first 8237: channels 0-3 */
#define DMA_CONTROLLER_16BIT        1       /* The second: channels 4-7 */
#define DMA_PORT_CHANNEL1_COUNT     0x03    /* The 8-bit SB channel's count: the poll port */
#define DMA_FIRST_16BIT_CHANNEL     4       /* Channels 0-3 are 8-bit, 4-7 16-bit */
#define DMA_CONTROLLERS             2
#define DMA_PAGE_PORTS              16      /* 80h-8Fh */

typedef struct _DMA_CHANNEL
{
    WORD BaseAddress, CurrentAddress;   /* byte offset (ch0-3) or word offset (ch4-7) */
    WORD BaseCount, CurrentCount; /* transfers-1, as the guest programmed it */
    BYTE  Page;                  /* high address bits, from ports 0x80-0x8F */
    BYTE  Mode;                  /* last mode byte written for this channel */
    BYTE  IsMasked;                /* 1 = channel disabled (mask register) */
    BYTE  IsTerminalCount;                    /* terminal count reached; cleared on status rd */
} DMA_CHANNEL, *PDMA_CHANNEL;

typedef const DMA_CHANNEL *PCDMA_CHANNEL;

typedef struct _DMA_STATE
{
    PVDD_BUS Bus;
    DMA_CHANNEL Channels[DMA_CHANNELS];
    BYTE  FlipFlop[DMA_CONTROLLERS];                 /* per-controller lo/hi byte-pointer flip-flop */
    /* THE NINE PAGE PORTS THAT MAP TO NO CHANNEL ARE STILL REAL LATCHES:
     * Seven of the sixteen ports at 80h-8Fh carry a DMA channel's high address
     * bits. The other nine -- 80h, 84h-86h, 88h, 8Ch-8Fh -- are read/write
     * storage on a PC anyway, because the address decoder does not bother to
     * leave them out, and 80h doubles as the POST diagnostic port.
     *
     * [INFO]: MEASURED (p_dma.asm dma.page.spare80, 2026-09-23): dosbox-x AND PCem, on
     * a real AMI BIOS, both read back a written 0x5A. Only 6.22-under-QEMU
     * answers 0xFF. [CAUTION] That is NOT "QEMU is the outlier" as a general rule --
     * counted over a session it is not even true; see
     * docs/research/oracle-disagreements.md.
     *
     * [CAUTION]: We answered 0xFF, which describes an EMPTY BUS rather than a machine.
     * Indexed by the low nibble of the port; the mapped ports never reach it.
     */
    BYTE  SparePage[DMA_PAGE_PORTS];
    BYTE  Command[DMA_CONTROLLERS];  /* per-controller command register; DMA_COMMAND_* */
    /* THE REQUEST REGISTER (09h / D2h) -- A DREQ WRITTEN BY SOFTWARE. (#246):
     * 8237A datasheet: one request bit per channel, "non-maskable and subject to
     * prioritization", set or reset individually by a write (bits 1:0 the channel,
     * bit 2 set/reset), "cleared upon generation of a TC or external EOP", and the
     * whole register cleared by a reset (master clear). Bit c of req[ctrl] is
     * channel (ctrl*4 + c). ORed into VddDmaDreq, so it shows in status 7:4.
     */
    BYTE  Request[DMA_CONTROLLERS];
    /* THE TEMPORARY REGISTER (read 0Dh / DAh). (#246):
     * Holds each byte of a memory-to-memory transfer between its read (channel 0's
     * address) and its write (channel 1's); afterwards "the last word moved can be
     * read by the microprocessor". Cleared by a reset. Controller 2 never does a
     * memory-to-memory transfer on an AT (its channels 0/1 are 4 = the cascade and
     * 5), so temp[1] stays 0.
     */
    BYTE  Temporary[DMA_CONTROLLERS];
    UINT32 SoftwareRuns, MemoryToMemoryRuns;      /* diagnostics: requests served by the 8237 itself */
    PDMA_DREQ_ROUTINE DreqRoutines[DMA_DREQ_MAX];   /* who drives DREQ -- host wiring, see above */
    PCVOID DreqContexts[DMA_DREQ_MAX];
    BYTE  DreqCount;
    /* DOES THE GUEST ASK US WHERE THE PLAY HEAD IS?:
     * A double-buffering sound driver has two ways to decide which half of the
     * DMA ring is safe to write: count the block-completion IRQs, or READ THE
     * 8237's CURRENT ADDRESS. If DMX does the latter then the fidelity of
     * cur_addr -- which advances on the AUDIO thread here, in whatever chunks
     * waveOut happens to ask for -- is load-bearing for every refill decision,
     * and a ring replay would be the guest writing where we told it to write.
     * If it never reads them, that whole family of causes is dead and the
     * refill must be driven by the IRQ count alone. Nothing distinguishes the
     * two today: SNDIO traces only the card's own ports, and the hot-port
     * histogram is empty for a protected-mode client. Three counters settle it.
     */
    UINT32 AddressReads[DMA_CHANNELS], ChannelCountReads[DMA_CHANNELS];  /* guest reads of CurrentAddress / CurrentCount */
    /* A COUNT READ IS THE GUEST'S MIXER SAYING 'I AM RUNNING NOW':
     * DMX's refill routine polls the 8237's current count as its FIRST action
     * (DOOM.EXE 0x56884), before it decides which block to fill. So a read of the
     * count register is the one externally visible moment at which the guest is
     * known to be mid-refill -- which is exactly the phase signal the ACK cannot
     * give, because DMX acknowledges the interrupt BEFORE it refills (0x53024's
     * status read runs ahead of the `call [0x584]` that does the work).
     */
    UINT32 CountReads;              /* monotonic: any guest read of a count reg */
    UINT32 StatusReads[DMA_CONTROLLERS];             /* ...and of the status register (TC bits) */
    /* [CAUTION]: A COUNT OF PORT READS IS NOT A COUNT OF POLLS. The 8237's count register is
     * 16 bits behind an 8-bit port with a lo/hi flip-flop, so one poll is TWO
     * reads -- unless the guest issues a 16-bit IN, which `DmaPortIn` currently
     * serves by ignoring the width and returning a single half. Dividing reads by
     * two to get a poll rate is an assumption about which of those is happening,
     * and the whole "DMX looks less often than blocks complete" reading rests on
     * it. Count the widths and let the run say.
     */
    UINT32 CountReadsByte, CountReadsWord, CountReadsDword;      /* count-register reads by operand width */
} DMA_STATE, *PDMA_STATE;

typedef const DMA_STATE *PCDMA_STATE;

/* Build the device descriptor to hand to VddBusAdd(). */
#define DMA_DEVICE_NAME     "dma"

INT  VddDmaInitialize(_In_ PVDD_BUS bus, _In_ PVOID context);
VOID VddDmaReset(_In_ PVOID context);
static inline NTVDD_DEVICE VddDmaDevice(_In_ PDMA_STATE state)
{ NTVDD_DEVICE device;
device.Name = DMA_DEVICE_NAME;
device.Initialize = VddDmaInitialize;
device.Reset = VddDmaReset;
  device.Shutdown = 0;
  device.Context = state;
  return device; }

/* The physical address the next transfer on `ch` will touch. */
UINT32 VddDmaCurrentPhysical(_In_ PCDMA_STATE state, _In_ BYTE channelNumber);

/* Bytes still to transfer before terminal count (count+1 units, scaled to bytes). */
UINT32 VddDmaRemaining(_In_ PCDMA_STATE state, _In_ BYTE channelNumber);

/* WOULD THE 8237 ANSWER A DREQ ON `ch` RIGHT NOW?:
 * The ONE question every DMA-moving path asks, so the conditions live in one place:
 * the channel's mask bit is clear AND its controller is not disabled (command bit 2).
 * VddDmaRead/_write ask it themselves; a device that must decide whether to HOLD
 * its own state machine (an SB that must not end its block, a GUS whose upload must
 * wait) asks it before it pulls, rather than reading `masked` or `cmd[]` itself.
 *
 * [INFO]: AND THE AT CASCADE (#246): controller 1 reaches the bus only through channel 4 of
 * controller 2, so for channels 0-3 the grant ALSO needs channel 4 unmasked and
 * controller 2 enabled. Masking channel 4 or disabling controller 2 starves every
 * 8-bit channel, as on the board. VddDmaReset/init leave the machine as POST does
 * (channel 4 in cascade mode, unmasked) -- before #246 nothing did, which is why
 * this could not be honoured: channel 4 sat masked from the power-on master clear.
 */
INT VddDmaGrants(_In_ PCDMA_STATE state, _In_ BYTE channelNumber);

/* The DREQ lines, channels 0-7 as bits 0-7, from every registered device. Channel 4
 * is controller 1's HRQ (the cascade): asserted while controller 1 has a request it
 * would serve.
 */
BYTE VddDmaDreq(_In_ PCDMA_STATE state);

/* A device that can request DMA calls this once, from its init. Returns -1 if the
 * table is full; registering the same (fn, ctx) twice is harmless.
 */
INT VddDmaAddDreq(
    _Inout_ PDMA_STATE state,
    _In_ PDMA_DREQ_ROUTINE dreqRoutine,
    _In_opt_ PCVOID dreqContext);

/* Pull up to `n` bytes from guest memory into `dst` (memory -> device: playback).
 * Stops early at terminal count on a non-auto-init channel (and masks it, as the
 * 8237 does); an auto-init channel reloads and keeps going, so a ring buffer
 * streams forever. Returns bytes actually transferred; 0 if the 8237 would not
 * serve the channel (VddDmaGrants: masked, or its controller disabled) -- no
 * byte moves, the address and count stand still, and no TC is raised.
 * `tc_out` (optional) is set non-zero if terminal count was reached.
 */
UINT32 VddDmaRead(
    _Inout_ PDMA_STATE state,
    _In_ BYTE channelNumber,
    _Out_writes_(byteCount) BYTE *destination,
    _In_ UINT32 byteCount,
    _Out_opt_ INT *isTerminalCount);

/* Push `n` bytes into guest memory (device -> memory: recording). Same rules. */
UINT32 VddDmaWrite(
    _Inout_ PDMA_STATE state,
    _In_ BYTE channelNumber,
    _In_reads_(byteCount) const BYTE *source,
    _In_ UINT32 byteCount,
    _Out_opt_ INT *isTerminalCount);

#endif /* NTVDMEX_VDD_DMA_H */
