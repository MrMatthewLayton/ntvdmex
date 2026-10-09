/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The device bus behind the ntvdd.h ABI.  (M3, ADR-0008)
 *
 * The bus is a registry: VDDs claim port ranges / memory windows / interrupt
 * vectors / frame ticks during init, and the host calls the VddBus* dispatch
 * entry points from its V86 service loop when the matching hardware event fires.
 *
 * The bus is host-agnostic (no Windows calls, only Windows types): the two effects that need
 * the real host -- raising an IRQ into the kernel ICA and presenting a frame to
 * the screen -- are injected as `sink` callbacks (NULL in the off-VM test). This
 * is what lets vdd_test.c exercise the whole bus + a VDD with no VM.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_VDD_BUS_H
#define NTVDMEX_VDD_BUS_H

/* VddBusIo's direction. */
#define VDD_IO_OUT  0
#define VDD_IO_IN   1

#include "ntvdd.h"

#ifndef VDD_MAX_PORT_RANGES
#define VDD_MAX_PORT_RANGES         48 /* Claimed port ranges (s80: 31 of 32 were in use, and the GUS needs two) */
#define VDD_UNCLAIMED_READ_U        0xFFFFFFFFu     /* A port nobody claims floats high, as an open ISA bus */
#define VDD_UNCLAIMED_READ_BYTE     0xFF
#define VDD_UNCLAIMED_READ_WORD     0xFFFF

/* [CAUTION]: THIS WAS 16, AND IT WAS EXACTLY FULL. Adding one CRTC range pushed the LAST
 * device added -- the MPU-401 -- off the bus: VddClaimPorts() returned -1, nobody
 * looked, and the guest's MIDI port read 0xFF like an empty ISA slot. Doom's music
 * driver reset it four times, got nothing, and played no music at all. Nothing in
 * any log said so; it took diffing an SNDIO trace against a working run to see
 * 0x330 answering 0xffffffff. Hence `ClaimFailures` below and the STAGE0 line that
 * prints it: a device that cannot get on the bus must be LOUD.
 */
#endif
#ifndef VDD_MAX_MEMORY_WINDOWS
#define VDD_MAX_MEMORY_WINDOWS  8   /* Claimed memory windows */
#endif
#ifndef VDD_MAX_FRAME_SUBSCRIBERS
#define VDD_MAX_FRAME_SUBSCRIBERS   8   /* frame-tick subscribers */
#endif
#ifndef VDD_MAX_DEVICES
#define VDD_MAX_DEVICES    24       /* devices on the bus (s87 #179: was 16, and with
                                   GUS + EMU8K fitted the IDE adapter made 17)   */
#endif

#define VDD_INTERRUPT_VECTORS   256     /* One slot per vector */

typedef struct _VDD_PORT_ENTRY
{
    WORD First;
    WORD Last;
    PVDD_PORT_IN_ROUTINE In;
    PVDD_PORT_OUT_ROUTINE Out;
    PVOID Context;
} VDD_PORT_ENTRY, *PVDD_PORT_ENTRY;
typedef struct _VDD_MEMORY_ENTRY
{
    UINT32 Base;
    UINT32 End;
    PVDD_MEMORY_READ_ROUTINE Read;
    PVDD_MEMORY_WRITE_ROUTINE Write;
    PVOID Context;
} VDD_MEMORY_ENTRY, *PVDD_MEMORY_ENTRY;
typedef struct _VDD_INTERRUPT_ENTRY
{
    PVDD_INTERRUPT_ROUTINE Service;
    PVOID Context;
} VDD_INTERRUPT_ENTRY, *PVDD_INTERRUPT_ENTRY;
typedef struct _VDD_FRAME_ENTRY
{
    PVDD_FRAME_ROUTINE Routine;
    PVOID Context;
} VDD_FRAME_ENTRY, *PVDD_FRAME_ENTRY;

/* host-injected effects */
typedef VOID (*PVDD_IRQ_SINK)(PVOID context, BYTE irq);
typedef VOID (*PVDD_PRESENT_SINK)(PVOID context, PCNTVDD_FRAME frame);

struct _VDD_BUS
{
    PVOID          MemoryBase;          /* VddMapFlat base; NULL => V86 absolute */
    PVDD_IRQ_SINK   IrqSink;
    PVOID IrqContext;
    PVDD_PRESENT_SINK PresentSink;
    PVOID PresentContext;

    VDD_PORT_ENTRY      Ports[VDD_MAX_PORT_RANGES];
    INT PortCount;
    VDD_MEMORY_ENTRY    Memory[VDD_MAX_MEMORY_WINDOWS];
    INT MemoryCount;
    VDD_INTERRUPT_ENTRY Interrupts[VDD_INTERRUPT_VECTORS];   /* indexed by vector; Service==NULL=unset */
    VDD_FRAME_ENTRY     FrameSubscribers[VDD_MAX_FRAME_SUBSCRIBERS];
    INT FrameCount;

    PNTVDD_DEVICE  Devices[VDD_MAX_DEVICES];
    INT DeviceCount;
    INT            ClaimFailures;          /* claims refused for want of a table slot */
};

/* --- host-side lifecycle + dispatch (the V86 loop calls these) ------------- */
VOID VddBusInitialize(PVDD_BUS bus, PVOID memoryBase);
VOID VddBusSetSinks(
    PVDD_BUS bus,
    PVDD_IRQ_SINK irqSink,
    PVOID irqContext,
    PVDD_PRESENT_SINK presentSink,
    PVOID presentContext);
INT  VddBusAdd(PVDD_BUS bus, PNTVDD_DEVICE device);     /* runs device->Initialize; 0 = ok */
VOID VddBusResetAll(PVDD_BUS bus);
VOID VddBusShutdownAll(PVDD_BUS bus);

/* dispatch -- each returns 1 if a VDD owned the event, 0 if unclaimed */
INT  VddBusIo(PVDD_BUS bus, WORD port, BYTE width, INT isIn, UINT32 *value);
INT  VddBusMemoryRead(PVDD_BUS bus, UINT32 address, BYTE *value);
INT  VddBusMemoryWrite(PVDD_BUS bus, UINT32 address, BYTE value);
INT  VddBusDeliverInterrupt(PVDD_BUS bus, BYTE vector, PNTVDD_REGISTERS registers);
VOID VddBusFrame(PVDD_BUS bus);               /* fan out the ~60 Hz tick */

#endif /* NTVDMEX_VDD_BUS_H */
