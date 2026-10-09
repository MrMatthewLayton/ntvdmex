/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * NTVDMEX clean Virtual Device Driver (VDD) ABI.  (M3, ADR-0008)
 *
 * Every device -- video, the PIT timer, sound, input -- is a `ntvdd` plugged
 * into one `VDD_BUS`.  The host core owns the V86 CPU loop and the DOS kernel;
 * it knows nothing device-specific.  Instead it drives the bus, which routes
 * hardware events to whichever VDD claimed them:
 *
 *     IN/OUT on a claimed port   -> the port owner's in/out callback
 *     access in a claimed window -> the memory owner's rd/wr callback
 *     a claimed software INT      -> the interrupt owner's service callback
 *     ~60 Hz frame tick           -> every on_frame subscriber
 *
 * and the bus offers services back to a VDD: raise an emulated IRQ, translate a
 * guest seg:off to a flat host pointer, and present a finished framebuffer.
 *
 * Design rule (ADR-0008): register access is EXPLICIT, through `NTVDD_REGISTERS`
 * (a plain view, not global get/set macros), so a VDD is unit-testable off-VM
 * -- the same discipline that made dos_mcb.h testable without a VM.
 *
 * No Windows calls, only Windows types (src/ntvdmex_types.h); it is shared verbatim by the
 * XP host build and the off-VM test battery (tests/unit/vdd_test.c).
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_NTVDD_H
#define NTVDMEX_NTVDD_H

#include "../ntvdmex_types.h"
/* Devices not yet migrated to the Windows types (#333) still use the <stdint.h> names
 * and have always had them from here. Remove once the last device is converted.
 */
#include <stdint.h>

/* --- guest register view handed to interrupt-service callbacks ------------- */
/* A flat snapshot of the V86 registers a device cares about.  On the real host
 * the bus loads this from the VDM_TIB CONTEXT before the call and stores it back
 * after; off-VM a test fills it directly.  `CarryFlag` carries the carry flag out (DOS
 * error convention); other flag bits are not modelled here.
 */
typedef struct _NTVDD_REGISTERS
{
    UINT32 Eax, Ebx, Ecx, Edx, Esi, Edi, Ebp;
    WORD   Ds, Es;
    BYTE   CarryFlag;       /* carry flag: read+write by the handler */
    BYTE   ZeroFlag;        /* zero flag: e.g. INT 16h AH=01 "key available" */
} NTVDD_REGISTERS, *PNTVDD_REGISTERS;

typedef const NTVDD_REGISTERS *PCNTVDD_REGISTERS;

/* 8/16-bit sub-register accessors (keep call sites readable). */
#define VDD_KEEP_ALL_BUT_AL     0xFFFFFF00u
#define VDD_KEEP_ALL_BUT_AH     0xFFFF00FFu
#define VDD_KEEP_HIGH_WORD      0xFFFF0000u
static inline BYTE VddGetAl(PCNTVDD_REGISTERS registers)
{
    return (BYTE)(registers->Eax);
}

static inline BYTE VddGetAh(PCNTVDD_REGISTERS registers)
{
    return (BYTE)(registers->Eax >> BYTE_SHIFT);
}

static inline WORD VddGetAx(PCNTVDD_REGISTERS registers)
{
    return (WORD)(registers->Eax);
}

static inline WORD VddGetBx(PCNTVDD_REGISTERS registers)
{
    return (WORD)(registers->Ebx);
}

static inline WORD VddGetCx(PCNTVDD_REGISTERS registers)
{
    return (WORD)(registers->Ecx);
}

static inline WORD VddGetDx(PCNTVDD_REGISTERS registers)
{
    return (WORD)(registers->Edx);
}

static inline VOID VddSetAl(PNTVDD_REGISTERS registers, BYTE value)
{
    registers->Eax = (registers->Eax & VDD_KEEP_ALL_BUT_AL) | value;
}

static inline VOID VddSetAh(PNTVDD_REGISTERS registers, BYTE value)
{
    registers->Eax = (registers->Eax & VDD_KEEP_ALL_BUT_AH) | ((UINT32)value << BYTE_SHIFT);
}

static inline VOID VddSetAx(PNTVDD_REGISTERS registers, WORD value)
{
    registers->Eax = (registers->Eax & VDD_KEEP_HIGH_WORD) | value;
}

static inline VOID VddSetBx(PNTVDD_REGISTERS registers, WORD value)
{
    registers->Ebx = (registers->Ebx & VDD_KEEP_HIGH_WORD) | value;
}

static inline VOID VddSetCx(PNTVDD_REGISTERS registers, WORD value)
{
    registers->Ecx = (registers->Ecx & VDD_KEEP_HIGH_WORD) | value;
}

static inline VOID VddSetDx(PNTVDD_REGISTERS registers, WORD value)
{
    registers->Edx = (registers->Edx & VDD_KEEP_HIGH_WORD) | value;
}

/* --- a finished frame the video VDD hands to the presentation layer -------- */
/* The largest frame the contract carries. Both ends size their buffers from this:
 * the video VDD may not advertise a mode wider or taller than the presenter can
 * show, and the presenter may not drop a frame the VDD is allowed to produce.
 * (s74b: was 800x600 in two headers that had to agree by hand; 1024x768 lets the
 * VBE 1.2 modes real games list -- 0x105, 0x116..0x118 -- be published.)
 */
#define NTVDD_FRAME_MAX_WIDTH   1280    /* 1280x1024 (0x107) and 132 columns x 8 = 1056 */
#define NTVDD_FRAME_MAX_HEIGHT  1024
#define NTVDD_PALETTE_ENTRIES   256
typedef struct _NTVDD_FRAME
{
    WORD            Width, Height;  /* logical resolution in pixels */
    BYTE            BitsPerPixel;   /* 8 (palettised) or 32 (ARGB) */
    UINT32          Stride;         /* bytes per scanline of `Pixels` */
    const BYTE     *Pixels;         /* framebuffer (indices if BitsPerPixel==8) */
    const UINT32   *Palette;        /* 256 ARGB entries (8bpp only; else NULL) */
    /* -- RASTER-SPLIT PALETTE (s70). A guest may rewrite DAC entries MID-FRAME so
     * that the rows above the beam and the rows below it are shown in different
     * colours -- Lemmings does exactly this from its timer tick at row 160 (the
     * level in one palette, the toolbar in another). One palette per frame
     * cannot draw that: whichever set the snapshot caught colours the WHOLE
     * screen, and the picture flickers between two wrong versions. So the video
     * VDD keeps, per entry, the value in effect at the frame start (`PaletteBase`)
     * and the value written mid-frame with the row it was written at
     * (`PaletteSplit`/`SplitRow`, 0 = none) stamped with the frame number. All
     * NULL when the device does not track it. Resolve with VddFramePaletteAt.
     */
    const UINT32   *PaletteBase;
    const UINT32   *PaletteSplit;
    const WORD     *SplitRow;
    const UINT32   *SplitFrame;
    UINT32          FrameNumber;    /* the frame number at snapshot time */
} NTVDD_FRAME, *PNTVDD_FRAME;

typedef const NTVDD_FRAME *PCNTVDD_FRAME;

/* A split older than this many frames is stale: the DAC simply holds. */
#define NTVDD_SPLIT_MAX_AGE     2u

/* The colour of entry `paletteIndex` on frame row `row`: the mid-frame value from its row
 * down, the frame-start value above it, and the live palette once a split is
 * older than two frames (the guest stopped doing it; the DAC simply holds). Two,
 * not one: our tick can arrive a frame late under a host stall, and a boundary
 * that vanishes for that one frame is itself a flicker.
 */
static inline UINT32 VddFramePaletteAt(PCNTVDD_FRAME frame, UINT row, UINT paletteIndex)
{
    if (frame->SplitRow && frame->SplitRow[paletteIndex]
        && (UINT32)(frame->FrameNumber - frame->SplitFrame[paletteIndex]) <= NTVDD_SPLIT_MAX_AGE)
        return (row >= frame->SplitRow[paletteIndex]) ? frame->PaletteSplit[paletteIndex] : frame->PaletteBase[paletteIndex];
    return frame->Palette[paletteIndex];
}

/* Is any entry split on this frame? Lets a presenter keep its single-palette fast
 * path for the 99% of frames that have none.
 */
static inline INT VddFrameHasSplit(PCNTVDD_FRAME frame)
{
    UINT paletteIndex;
    if (!frame->SplitRow) return FALSE;
    for (paletteIndex = 0; paletteIndex < NTVDD_PALETTE_ENTRIES; ++paletteIndex)
        if (frame->SplitRow[paletteIndex] && (UINT32)(frame->FrameNumber - frame->SplitFrame[paletteIndex]) <= NTVDD_SPLIT_MAX_AGE) return TRUE;
    return FALSE;
}

/* --- device-supplied callback signatures ---------------------------------- */
/* UINT32, not DWORD, for the port value: it is the same type as the uint32_t the
 * devices not yet migrated implement their handlers with.
 */
typedef VOID (*PVDD_PORT_IN_ROUTINE)  (PVOID context, WORD port, BYTE width, UINT32 *value);
typedef VOID (*PVDD_PORT_OUT_ROUTINE) (PVOID context, WORD port, BYTE width, UINT32  value);
typedef BYTE (*PVDD_MEMORY_READ_ROUTINE)(PVOID context, UINT32 offset);   /* offset within window */
typedef VOID (*PVDD_MEMORY_WRITE_ROUTINE)(PVOID context, UINT32 offset, BYTE value);
typedef VOID (*PVDD_INTERRUPT_ROUTINE) (PVOID context, PNTVDD_REGISTERS registers);
typedef VOID (*PVDD_FRAME_ROUTINE)(PVOID context);

/* --- the bus (opaque to VDDs except through these calls) ------------------- */
typedef struct _VDD_BUS VDD_BUS, *PVDD_BUS;

/* claim_*: a VDD registers interest during init().  All ranges inclusive. */
INT  VddClaimPorts(PVDD_BUS bus, WORD firstPort, WORD lastPort,
                   PVDD_PORT_IN_ROUTINE inRoutine, PVDD_PORT_OUT_ROUTINE outRoutine, PVOID context);
INT  VddClaimMemory(PVDD_BUS bus, UINT32 base, UINT32 size,
                    PVDD_MEMORY_READ_ROUTINE readRoutine, PVDD_MEMORY_WRITE_ROUTINE writeRoutine, PVOID context);
INT  VddClaimInterrupt(PVDD_BUS bus, BYTE vector, PVDD_INTERRUPT_ROUTINE serviceRoutine, PVOID context);
INT  VddOnFrame(PVDD_BUS bus, PVDD_FRAME_ROUTINE frameRoutine, PVOID context);

/* services a VDD may call back into */
VOID  VddRaiseIrq(PVDD_BUS bus, BYTE irq);
PVOID VddMapFlat(PVDD_BUS bus, WORD segment, WORD offset);
/* Map a PHYSICAL linear address. seg:off cannot express one: ISA DMA addresses
 * memory as page<<16 | offset, so the DMA VDD needs the flat form directly.
 */
PVOID VddMapLinear(PVDD_BUS bus, UINT32 linear);
VOID  VddPresent(PVDD_BUS bus, PCNTVDD_FRAME frame);

/* --- a device descriptor -------------------------------------------------- */
typedef struct _NTVDD_DEVICE
{
    const CHAR *Name;
    INT  (*Initialize)(PVDD_BUS bus, PVOID context);   /* claim hooks; 0 = ok */
    VOID (*Reset)(PVOID context);           /* DOS process start / mode reset */
    VOID (*Shutdown)(PVOID context);
    PVOID Context;                          /* device instance state */
} NTVDD_DEVICE, *PNTVDD_DEVICE;

#endif /* NTVDMEX_NTVDD_H */
