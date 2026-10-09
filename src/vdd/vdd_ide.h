/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The AT's IDE/ATA host adapter, fitted, with NO DRIVES behind it.
 *            Primary 1F0h-1F7h + 3F6h, secondary 170h-177h + 376h-377h. (GH #179)
 *
 * WHY THIS EXISTS: AN ABSENT ADAPTER READ AS A PERMANENTLY BUSY DRIVE:
 * Nothing claimed the ATA blocks, so every register fell through to the unclaimed
 * ISA default of FFh. In the ATA Status / Alternate Status register bit 7 is BSY,
 * and the datasheet's own wait is
 *
 *     wait:  in al,dx / test al,80h / jnz wait       ; until BSY clears
 *
 * -- which on FFh never exits. Fourth instance of the same shape after the RTC's
 * UIP, the 82077AA's MSR and its DSKCHG line: an absent chip whose float value
 * means "wait". (docs/inventory/ide.md headline; memory: absent-chip-float-means-wait.)
 *
 * THE DECISION (ide.md "the choice to make"): (b), AN ADAPTER WITH NO DEVICES:
 * A 386/486 of the period has the adapter on the board or on a paddle card, and
 * both oracles that answered 3F6h have one. What it has NOT got, here, is a disk:
 * INT 13h exposes no fixed disk by design (dos_disk.h -- a drive is an image file or
 * it is absent), and an XP VDM may not touch the host's disks at all. So the adapter
 * is fitted and both channels are EMPTY, and every answer below is what an empty
 * channel says -- consistently with 0040:0075 = 0 and with INT 13h DL=80h.
 *
 * WHAT AN EMPTY CHANNEL READS, FROM THE DOCUMENT, NOT FROM MEMORY:
 * Every task-file register lives IN THE DRIVE; the adapter is only a decoder and a
 * buffer. With no drive nothing drives DD15:0 during a read, and then:
 *
 * * DD7 = 0. ATA-3 (X3T13/2008D rev 7b) Table 2 note 3: "It is recommended that a
 *  host have a 10 kOhm pull-down resistor and not a pull-up resistor on DD7 to
 *  allow a host to recognize the absence of a device at power-up." DD7 is BSY in
 *  the status register, so the canonical BSY wait EXITS AT ONCE. This is the bit
 *  that matters and it is the one the standard pins.
 * * DD6:0 are not driven by anything the standard names. We answer 0 for them, and
 *  that is a CHOICE, recorded as one. It is the value the same standard gives an
 *  absent device's registers where it does specify them -- ATA-3 8.7.1 (h): with
 *  device 1 absent, device 0 answers device 1's Status/Alternate Status "00h
 *  following a reset" -- and it is what QEMU and Bochs answer for an empty channel.
 *
 * [WARNING]: 7Fh (pull-down on DD7, the rest floating high) was considered and REJECTED.
 * It is what some real PCI controllers read, and it says DRDY=1 DRQ=1 ERR=1: a
 * detection routine that waits for BSY clear and then for DRDY would believe a
 * drive is ready and has data for it, issue IDENTIFY, and read 256 words of
 * float. Linux special-cases 7Fh for exactly that reason; a DOS-era routine need
 * not. 00h cannot be read as "a device with something to say".
 *
 * [WARNING]: AND NOTHING LATCHES. The classic presence test (ATA-3 8.7.2's own note; Linux
 * ata_devchk) writes 55h/AAh to Sector Count / Sector Number and reads them back:
 * a register file that echoed writes would announce a drive that is not there.
 * Writes are counted and dropped -- there is no device to receive them.
 *
 * So: every read is 0, at any width, on both channels; every write is dropped; no
 * IRQ14/15 is ever raised (nIEN is a device bit too, and no device exists to assert
 * INTRQ). This is deliberately the WHOLE model. Fitting a drive -- or an ATAPI
 * CD-ROM for MSCDEX -- is a separate decision (ide.md "what to fix" item 3).
 *
 * 3F7h stays the FDC's (vdd_fdc.c): on an AT bit 7 is the floppy controller's DIR
 * DSKCHG, and bits 6:0 are the hard-disk drive address register -- which is driven
 * by the DRIVE, so with no drive the FDC's 0 there is consistent with this file.
 * 377h, the secondary's drive address register, has no floppy controller beside it
 * (no second FDC at 370h) and is claimed here.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_VDD_IDE_H
#define NTVDMEX_VDD_IDE_H

#include "vdd_bus.h"

#define IDE_PRIMARY_COMMAND         0x1F0   /* 1F0h-1F7h: data .. status/command */
#define IDE_PRIMARY_CONTROL         0x3F6   /* Alternate status / device control */
#define IDE_SECONDARY_COMMAND       0x170   /* 170h-177h */
#define IDE_SECONDARY_CONTROL       0x376   /* 376h alt status / device control, 377h DA */
#define IDE_COMMAND_REGISTER        7       /* Base+7: status (read) / command (write) */
#define IDE_DRIVE_ADDRESS           1       /* Control+1: the drive address register */
#define IDE_DEVICE_NAME             "ide"

/* ATA status bits, for the tests and for anyone reading a trace. */
#define IDE_STATUS_BUSY             0x80
#define IDE_STATUS_DEVICE_READY     0x40
#define IDE_STATUS_DATA_REQUEST     0x08
#define IDE_STATUS_ERROR            0x01

typedef struct _IDE_STATE
{
    PVDD_BUS Bus;
    /* Diagnostics only -- what a guest tried to do to the empty channels. Nothing
     * here is ever read back to the guest; there is no state to read.
     */
    UINT32 PortReads;
    UINT32 PortWrites;
    UINT32 Commands;            /* writes to 1F7h/177h: commands nobody received */
    BYTE   LastCommand;         /* the last of them (ECh = IDENTIFY, etc.) */
} IDE_STATE, *PIDE_STATE;

INT  VddIdeInitialize(_In_ PVDD_BUS bus, _In_ PVOID context);
VOID VddIdeReset(_In_ PVOID context);
VOID VddIdePortIn(_In_ PVOID context, _In_ WORD port, _In_ BYTE width, _Out_ UINT32 *value);
VOID VddIdePortOut(_In_ PVOID context, _In_ WORD port, _In_ BYTE width, _In_ UINT32 value);

static inline NTVDD_DEVICE VddIdeDevice(_In_ PIDE_STATE state)
{ NTVDD_DEVICE device;
device.Name = IDE_DEVICE_NAME;
device.Initialize = VddIdeInitialize;
device.Reset = VddIdeReset;
  device.Shutdown = 0;
  device.Context = state;
  return device; }

#endif /* NTVDMEX_VDD_IDE_H */
