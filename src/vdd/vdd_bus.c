/* vdd_bus.c -- see vdd_bus.h / ntvdd.h.  The device bus implementation: the
 * claim registries (called by VDDs during init) and the dispatch entry points
 * (called by the host's V86 service loop).  No Windows calls, only Windows types. */
#include "vdd_bus.h"

#define VDD_BUS_OK             0
#define VDD_BUS_FAILED         (-1)
#define VDD_BUS_CLAIMED        1       /* dispatch: a VDD owned the event              */
#define VDD_BUS_UNCLAIMED      0
#define VDD_BUS_FLOATING_PORT  0xFFFFFFFFu  /* an IN with no handler: an empty ISA slot */
#define VDD_BUS_FLOATING_BYTE  0xFF
#define VDD_BUS_SEGMENT_SHIFT  4       /* real mode: linear = segment << 4             */
#define VDD_BUS_LAST_OFFSET    1       /* an inclusive window ends at base + size - 1 */

/* --- lifecycle ------------------------------------------------------------ */
VOID VddBusInitialize(PVDD_BUS bus, PVOID memoryBase)
{
    INT vector;
    bus->MemoryBase = memoryBase;
    bus->IrqSink = 0; bus->IrqContext = 0;
    bus->PresentSink = 0; bus->PresentContext = 0;
    bus->PortCount = 0; bus->MemoryCount = 0; bus->FrameCount = 0; bus->DeviceCount = 0;
    /* ⚠ ClaimFailures too: a bus on the stack (the off-VM tests) otherwise starts
       with garbage in the one counter that says a device failed to get on. */
    bus->ClaimFailures = 0;
    for (vector = 0; vector < VDD_INTERRUPT_VECTORS; ++vector) { bus->Interrupts[vector].Service = 0; bus->Interrupts[vector].Context = 0; }
}

VOID VddBusSetSinks(PVDD_BUS bus, PVDD_IRQ_SINK irqSink, PVOID irqContext,
                    PVDD_PRESENT_SINK presentSink, PVOID presentContext)
{
    bus->IrqSink = irqSink; bus->IrqContext = irqContext;
    bus->PresentSink = presentSink; bus->PresentContext = presentContext;
}

INT VddBusAdd(PVDD_BUS bus, PNTVDD_DEVICE device)
{
    if (bus->DeviceCount >= VDD_MAX_DEVICES) { bus->ClaimFailures++; return VDD_BUS_FAILED; }
    bus->Devices[bus->DeviceCount++] = device;
    return device->Initialize ? device->Initialize(bus, device->Context) : VDD_BUS_OK;
}

VOID VddBusResetAll(PVDD_BUS bus)
{
    INT deviceIndex;
    for (deviceIndex = 0; deviceIndex < bus->DeviceCount; ++deviceIndex)
        if (bus->Devices[deviceIndex]->Reset) bus->Devices[deviceIndex]->Reset(bus->Devices[deviceIndex]->Context);
}

VOID VddBusShutdownAll(PVDD_BUS bus)
{
    INT deviceIndex;
    for (deviceIndex = 0; deviceIndex < bus->DeviceCount; ++deviceIndex)
        if (bus->Devices[deviceIndex]->Shutdown) bus->Devices[deviceIndex]->Shutdown(bus->Devices[deviceIndex]->Context);
}

/* --- claim registries (the ntvdd.h ABI surface VDDs call) ----------------- */
INT VddClaimPorts(PVDD_BUS bus, WORD firstPort, WORD lastPort,
                  PVDD_PORT_IN_ROUTINE inRoutine, PVDD_PORT_OUT_ROUTINE outRoutine, PVOID context)
{
    PVDD_PORT_ENTRY entry;
    if (firstPort > lastPort || bus->PortCount >= VDD_MAX_PORT_RANGES) { bus->ClaimFailures++; return VDD_BUS_FAILED; }
    entry = &bus->Ports[bus->PortCount++];
    entry->First = firstPort; entry->Last = lastPort; entry->In = inRoutine; entry->Out = outRoutine; entry->Context = context;
    return VDD_BUS_OK;
}

INT VddClaimMemory(PVDD_BUS bus, UINT32 base, UINT32 size,
                   PVDD_MEMORY_READ_ROUTINE readRoutine, PVDD_MEMORY_WRITE_ROUTINE writeRoutine, PVOID context)
{
    PVDD_MEMORY_ENTRY entry;
    if (!size || bus->MemoryCount >= VDD_MAX_MEMORY_WINDOWS) { bus->ClaimFailures++; return VDD_BUS_FAILED; }
    entry = &bus->Memory[bus->MemoryCount++];
    entry->Base = base; entry->End = base + size - VDD_BUS_LAST_OFFSET; entry->Read = readRoutine; entry->Write = writeRoutine; entry->Context = context;
    return VDD_BUS_OK;
}

INT VddClaimInterrupt(PVDD_BUS bus, BYTE vector, PVDD_INTERRUPT_ROUTINE serviceRoutine, PVOID context)
{
    if (bus->Interrupts[vector].Service) return VDD_BUS_FAILED;        /* already claimed */
    bus->Interrupts[vector].Service = serviceRoutine; bus->Interrupts[vector].Context = context;
    return VDD_BUS_OK;
}

INT VddOnFrame(PVDD_BUS bus, PVDD_FRAME_ROUTINE frameRoutine, PVOID context)
{
    PVDD_FRAME_ENTRY entry;
    if (bus->FrameCount >= VDD_MAX_FRAME_SUBSCRIBERS) { bus->ClaimFailures++; return VDD_BUS_FAILED; }
    entry = &bus->FrameSubscribers[bus->FrameCount++];
    entry->Routine = frameRoutine; entry->Context = context;
    return VDD_BUS_OK;
}

/* --- services VDDs call back into ----------------------------------------- */
VOID VddRaiseIrq(PVDD_BUS bus, BYTE irq)
{
    if (bus->IrqSink) bus->IrqSink(bus->IrqContext, irq);
}

PVOID VddMapFlat(PVDD_BUS bus, WORD segment, WORD offset)
{
    UINT32 flat = ((UINT32)segment << VDD_BUS_SEGMENT_SHIFT) + offset;     /* real-mode linear address */
    return (BYTE *)bus->MemoryBase + flat;           /* base==NULL => absolute   */
}

PVOID VddMapLinear(PVDD_BUS bus, UINT32 linear)
{
    return (BYTE *)bus->MemoryBase + linear;         /* base==NULL => absolute   */
}

VOID VddPresent(PVDD_BUS bus, PCNTVDD_FRAME frame)
{
    if (bus->PresentSink) bus->PresentSink(bus->PresentContext, frame);
}

/* --- dispatch (the host's V86 service loop calls these) ------------------- */
INT VddBusIo(PVDD_BUS bus, WORD port, BYTE width, INT isIn, UINT32 *value)
{
    INT entryIndex;
    for (entryIndex = 0; entryIndex < bus->PortCount; ++entryIndex) {
        PVDD_PORT_ENTRY entry = &bus->Ports[entryIndex];
        if (port < entry->First || port > entry->Last) continue;
        if (isIn)  { if (entry->In)  { *value = 0; entry->In(entry->Context, port, width, value); } else *value = VDD_BUS_FLOATING_PORT; }
        else        { if (entry->Out) entry->Out(entry->Context, port, width, *value); }
        return VDD_BUS_CLAIMED;
    }
    return VDD_BUS_UNCLAIMED;
}

INT VddBusMemoryRead(PVDD_BUS bus, UINT32 address, BYTE *value)
{
    INT entryIndex;
    for (entryIndex = 0; entryIndex < bus->MemoryCount; ++entryIndex) {
        PVDD_MEMORY_ENTRY entry = &bus->Memory[entryIndex];
        if (address < entry->Base || address > entry->End) continue;
        *value = entry->Read ? entry->Read(entry->Context, address - entry->Base) : VDD_BUS_FLOATING_BYTE;
        return VDD_BUS_CLAIMED;
    }
    return VDD_BUS_UNCLAIMED;
}

INT VddBusMemoryWrite(PVDD_BUS bus, UINT32 address, BYTE value)
{
    INT entryIndex;
    for (entryIndex = 0; entryIndex < bus->MemoryCount; ++entryIndex) {
        PVDD_MEMORY_ENTRY entry = &bus->Memory[entryIndex];
        if (address < entry->Base || address > entry->End) continue;
        if (entry->Write) entry->Write(entry->Context, address - entry->Base, value);
        return VDD_BUS_CLAIMED;
    }
    return VDD_BUS_UNCLAIMED;
}

INT VddBusDeliverInterrupt(PVDD_BUS bus, BYTE vector, PNTVDD_REGISTERS registers)
{
    if (!bus->Interrupts[vector].Service) return VDD_BUS_UNCLAIMED;
    bus->Interrupts[vector].Service(bus->Interrupts[vector].Context, registers);
    return VDD_BUS_CLAIMED;
}

VOID VddBusFrame(PVDD_BUS bus)
{
    INT subscriberIndex;
    for (subscriberIndex = 0; subscriberIndex < bus->FrameCount; ++subscriberIndex)
        if (bus->FrameSubscribers[subscriberIndex].Routine) bus->FrameSubscribers[subscriberIndex].Routine(bus->FrameSubscribers[subscriberIndex].Context);
}
