/*
 * vdd_ide.h -- the AT's IDE/ATA host adapter, fitted, with NO DRIVES behind it.
 *              Primary 1F0h-1F7h + 3F6h, secondary 170h-177h + 376h-377h. (GH #179)
 *
 * ── WHY THIS EXISTS: AN ABSENT ADAPTER READ AS A PERMANENTLY BUSY DRIVE. ─────────
 * Nothing claimed the ATA blocks, so every register fell through to the unclaimed
 * ISA default of FFh. In the ATA Status / Alternate Status register bit 7 is BSY,
 * and the datasheet's own wait is
 *
 *       wait:  in al,dx / test al,80h / jnz wait       ; until BSY clears
 *
 * -- which on FFh never exits. Fourth instance of the same shape after the RTC's
 * UIP, the 82077AA's MSR and its DSKCHG line: an absent chip whose float value
 * means "wait". (docs/inventory/ide.md headline; memory: absent-chip-float-means-wait.)
 *
 * ── THE DECISION (ide.md "the choice to make"): (b), AN ADAPTER WITH NO DEVICES. ──
 * A 386/486 of the period has the adapter on the board or on a paddle card, and
 * both oracles that answered 3F6h have one. What it has NOT got, here, is a disk:
 * INT 13h exposes no fixed disk by design (dos_disk.h -- a drive is an image file or
 * it is absent), and an XP VDM may not touch the host's disks at all. So the adapter
 * is fitted and both channels are EMPTY, and every answer below is what an empty
 * channel says -- consistently with 0040:0075 = 0 and with INT 13h DL=80h.
 *
 * ── WHAT AN EMPTY CHANNEL READS, FROM THE DOCUMENT, NOT FROM MEMORY. ─────────────
 * Every task-file register lives IN THE DRIVE; the adapter is only a decoder and a
 * buffer. With no drive nothing drives DD15:0 during a read, and then:
 *
 *  * DD7 = 0. ATA-3 (X3T13/2008D rev 7b) Table 2 note 3: "It is recommended that a
 *    host have a 10 kOhm pull-down resistor and not a pull-up resistor on DD7 to
 *    allow a host to recognize the absence of a device at power-up." DD7 is BSY in
 *    the status register, so the canonical BSY wait EXITS AT ONCE. This is the bit
 *    that matters and it is the one the standard pins.
 *  * DD6:0 are not driven by anything the standard names. We answer 0 for them, and
 *    that is a CHOICE, recorded as one. It is the value the same standard gives an
 *    absent device's registers where it does specify them -- ATA-3 8.7.1 (h): with
 *    device 1 absent, device 0 answers device 1's Status/Alternate Status "00h
 *    following a reset" -- and it is what QEMU and Bochs answer for an empty channel.
 *  ⛔ 7Fh (pull-down on DD7, the rest floating high) was considered and REJECTED.
 *    It is what some real PCI controllers read, and it says DRDY=1 DRQ=1 ERR=1: a
 *    detection routine that waits for BSY clear and then for DRDY would believe a
 *    drive is ready and has data for it, issue IDENTIFY, and read 256 words of
 *    float. Linux special-cases 7Fh for exactly that reason; a DOS-era routine need
 *    not. 00h cannot be read as "a device with something to say".
 *  ⛔ AND NOTHING LATCHES. The classic presence test (ATA-3 8.7.2's own note; Linux
 *    ata_devchk) writes 55h/AAh to Sector Count / Sector Number and reads them back:
 *    a register file that echoed writes would announce a drive that is not there.
 *    Writes are counted and dropped -- there is no device to receive them.
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
 */
#ifndef NTVDMEX_VDD_IDE_H
#define NTVDMEX_VDD_IDE_H

#include "vdd_bus.h"

#define IDE_PRI_CMD   0x1F0     /* 1F0h-1F7h: data .. status/command            */
#define IDE_PRI_CTL   0x3F6     /* alternate status / device control            */
#define IDE_SEC_CMD   0x170     /* 170h-177h                                    */
#define IDE_SEC_CTL   0x376     /* 376h alt status / device control, 377h DA    */

/* ATA status bits, for the tests and for anyone reading a trace. */
#define ATA_SR_BSY    0x80
#define ATA_SR_DRDY   0x40
#define ATA_SR_DRQ    0x08
#define ATA_SR_ERR    0x01

typedef struct {
    vdd_bus *bus;
    /* Diagnostics only -- what a guest tried to do to the empty channels. Nothing
       here is ever read back to the guest; there is no state to read. */
    uint32_t reads, writes;
    uint32_t cmds;              /* writes to 1F7h/177h: commands nobody received */
    uint8_t  last_cmd;          /* the last of them (ECh = IDENTIFY, etc.)      */
} ide_state;

int  vdd_ide_init (vdd_bus *b, void *self);
void vdd_ide_reset(void *self);
void vdd_ide_in   (void *self, uint16_t port, uint8_t width, uint32_t *val);
void vdd_ide_out  (void *self, uint16_t port, uint8_t width, uint32_t val);

static inline ntvdd vdd_ide_device(ide_state *st)
{ ntvdd d; d.name = "ide"; d.init = vdd_ide_init; d.reset = vdd_ide_reset;
  d.shutdown = 0; d.self = st; return d; }

#endif /* NTVDMEX_VDD_IDE_H */
