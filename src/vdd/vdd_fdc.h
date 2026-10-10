/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The 82077AA floppy disk controller (ports 3F2h-3F5h, 3F7h).
 *
 * WHY THIS EXISTS, AND WHY IT IS NOT OPTIONAL:
 * Nothing claimed 3F0h-3F7h, so the controller did not exist -- while the BIOS
 * SERVICE built on top of it did: INT 13h reads and writes sectors out of a real
 * image file in the host, the geometry is measured against 6.22, and both firmware
 * advertisements (CMOS byte 10h, the INT 11h equipment word) say a 1.44M drive is
 * fitted. Firmware present, hardware absent -- the same split the 8042 and the
 * MC146818 turned out to have, and the third one in a row.
 *
 * [WARNING]: AND THE FAILURE SHAPE WAS A HANG, NOT A WRONG ANSWER. An unclaimed ISA
 * port reads 0xFF here, deliberately, so that device detection cannot mistake an
 * absent card for a present one. 3F4h is the MAIN STATUS REGISTER, and 0xFF
 * there is RQM=1 with DIO=1 -- "ready, and I am the one talking". The
 * command-write loop out of the datasheet, which is the loop in every BIOS and
 * every driver ever written,
 *
 *     wait:  in al,3F4h / and al,0C0h / cmp al,80h / jne wait
 *
 * compares C0h against 80h, never matches, and SPINS FOR EVER. No fault, no
 * timeout, nothing in any log. This is the RTC's UIP bit one surface later: an
 * absent chip whose float value happens to mean "wait". 00h would hang too (RQM
 * clear is also "wait"), so there is no default that saves it -- only the chip.
 *
 * [INFO]: MEASURED (p_fdc.asm, 2026-09-23) before a line of this was written, on the
 * XP test machine against two oracles that agree:
 *
 *     case            6.22/QEMU   PCem      NTVDMEX
 *     fdc.msr.idle    0080        0080      00FF
 *     fdc.cmdwait     0080        0080      01C0   <- AH=01: it never exited
 *     fdc.version     0190        0190      FFFF
 *     fdc.dumpreg     0A01        0A01      FFFF
 *
 * `fdc.cmdwait` is not a register value, it is the loop above, bounded to 65536
 * turns and asked whether it terminated. On both oracles it did.
 *
 * WHAT THIS IS AND IS NOT:
 * This is the CHIP: the register file, the three-phase command protocol, and every
 * command that does not move sector data. That is the whole of what a detection
 * routine asks and the whole of the hang.
 *
 * [WARNING]: THE DATA COMMANDS ARE **PART**, AND DELIBERATELY SO. READ DATA, WRITE DATA,
 * READ TRACK and FORMAT TRACK are recognised, consume their parameters, and
 * terminate with the documented ABNORMAL TERMINATION result (ST0 bits 7:6 = 01,
 * ST1 = no data) rather than pretending to be invalid commands. They need the
 * 8237A wired to the image handle INT 13h already holds, which is the next step
 * and a bigger one. A reported I/O error is not a good answer; it is a
 * *reported* one, which is the entire difference being bought here.
 *
 * DORMANT BY DEFAULT:
 * IRQ6 is raised only in response to a command the guest issued, and only when the
 * guest has cleared neither DOR bit 3 (DMAGATE) nor the PIC's mask -- and the
 * master IMR starts at 0xFC, IRQ0 and IRQ1 only. A guest that does not ask sees no
 * change, the same property that made the RTC's IRQ8 safe to add.
 *
 * Pure C, no <windows.h>: nothing here touches a file or a clock, so the whole
 * chip is exercised off-VM by tests/unit/fdc_test.c.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_VDD_FDC_H
#define NTVDMEX_VDD_FDC_H

#include "vdd_bus.h"

#define FDC_BASE                0x3F0
#define FDC_DOR                 0x3F2   /* R/W: motors, DRIVE SEL, /RESET, DMAGATE */
#define FDC_TDR                 0x3F3   /* R/W: tape drive register */
#define FDC_MSR                 0x3F4   /* R : main status / W: DSR */
#define FDC_FIFO                0x3F5   /* R/W: command and result bytes */
#define FDC_DIR                 0x3F7   /* R : digital input / W: CCR */

/* [CAUTION]: 3F0h and 3F1h (SRA/SRB) ARE NOT CLAIMED, and 3F6h is not ours at all -- it
 * is the ATA alternate status register. See the notes in vdd_fdc.c.
 */

/* MSR bits (docs/ref/fdc.md 3). */
#define FDC_MSR_RQM             0x80    /* The FIFO is ready for a transfer */
#define FDC_MSR_DIO             0x40    /* 1 = FDC -> CPU (a result is waiting) */
#define FDC_MSR_NDMA            0x20    /* Execution phase in non-DMA mode */
#define FDC_MSR_CB              0x10    /* A command is in progress */

/* DOR bits (docs/ref/fdc.md 7). */
#define FDC_DOR_MOTORS          0xF0
#define FDC_DOR_DMA_GATE        0x08    /* 0 disconnects IRQ6 and the DMA request */
#define FDC_DOR_NRESET          0x04    /* ACTIVE LOW: 0 holds the chip in reset */
#define FDC_DOR_DRIVE_SELECT    0x03

#define FDC_PHASE_COMMAND       0       /* The host is writing a command */
#define FDC_BUFFER_SIZE         16      /* Command and result bytes held */
#define FDC_DRIVES              4
#define FDC_DEVICE_NAME         "fdc"
#define FDC_PHASE_RESULT        1       /* The host is reading a result */

typedef struct _FDC_STATE
{
    PVDD_BUS Bus;

    BYTE  Dor;                   /* 3F2h, as last written */
    BYTE  Tdr;                   /* 3F3h */
    BYTE  Dsr;                   /* 3F4h write: data rate, precomp, power */
    BYTE  Ccr;                   /* 3F7h write: data rate, the other door */
    BYTE  IsInReset;              /* DOR bit 2 is low: the chip is held down */

    /* THE CONVERSATION. This is the part that makes the chip a chip: */
    BYTE  Phase;                 /* FDC_PHASE_COMMAND or FDC_PHASE_RESULT */
    BYTE  Command[FDC_BUFFER_SIZE];               /* the command byte and its parameters */
    BYTE  CommandLength;               /* how many have arrived */
    BYTE  CommandWanted;              /* how many this command takes, opcode incl. */
    BYTE  Result[FDC_BUFFER_SIZE];               /* the result bytes, oldest first */
    BYTE ResultLength;
    BYTE ResultPosition;

    /* PER-DRIVE STATE: */
    BYTE  PresentCylinder[FDC_DRIVES];                /* present cylinder number */

    /* THE INTERRUPT, AND THE COMMAND THAT COLLECTS IT: */
    BYTE  IsIrqPending;           /* an interrupt is waiting to be sensed */
    BYTE  PendingSt0;           /* what SENSE INTERRUPT STATUS will report */
    /* Reset leaves the chip expecting FOUR sense-interrupts, one per drive, each
     * answering C0h|drive ("ready changed") -- unless CONFIGURE turned drive
     * polling off. Four, not one: a model that answers a single sense leaves a
     * driver that issued four with 80h for three of them. (ref/fdc.md 8)
     */
    BYTE  PollDrive;            /* 0..4: how many of the four are still owed */
    BYTE  IsPollDisabled;              /* CONFIGURE asked for no drive polling */

    /* WHAT THE FIRMWARE PROGRAMMED, so DUMPREG can hand it back: */
    BYTE  StepRateHeadUnload;               /* SPECIFY byte 1: step rate | head unload */
    BYTE  HeadLoadNonDma;                /* SPECIFY byte 2: head load | non-DMA */
    /* CONFIGURE bytes 2 and 3 */
    BYTE ConfigureByte2;
    BYTE ConfigurePrecompTrack;
    BYTE  Perpendicular;                  /* PERPENDICULAR MODE */
    BYTE  IsLocked;                /* LOCK: keep CONFIGURE across a s/w reset */
    BYTE  LastEot;              /* the sector count of the last data command */

    /* Counters a run can report -- the same habit as every other VDD here. */
    UINT32 Commands;
    UINT32 InvalidCommands;
    UINT32 Irqs;
    UINT32 Resets;
} FDC_STATE, *PFDC_STATE;

typedef const FDC_STATE *PCFDC_STATE;

INT  VddFdcInitialize(_In_ PVDD_BUS bus, _In_ PVOID context);
VOID VddFdcReset(PVOID context);

/* Exposed for the off-VM battery, which drives the chip through the same two
 * doors the guest does rather than reaching into the struct.
 */
VOID    VddFdcPortOut(_In_ PVOID context, _In_ WORD port, _In_ BYTE width, _In_ UINT32 value);
VOID    VddFdcPortIn(_In_ PVOID context, _In_ WORD port, _In_ BYTE width, _Out_ UINT32 *value);
BYTE VddFdcMainStatus(_In_ PCFDC_STATE state);

static inline NTVDD_DEVICE VddFdcDevice(_In_ PFDC_STATE state)
{ NTVDD_DEVICE device;
device.Name = FDC_DEVICE_NAME;
device.Initialize = VddFdcInitialize;
device.Reset = VddFdcReset;
  device.Shutdown = 0;
  device.Context = state;
  return device; }

#endif /* NTVDMEX_VDD_FDC_H */
