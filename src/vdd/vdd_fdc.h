/*
 * vdd_fdc.h -- the 82077AA floppy disk controller (ports 3F2h-3F5h, 3F7h).
 *
 * ── WHY THIS EXISTS, AND WHY IT IS NOT OPTIONAL. ────────────────────────────────
 * Nothing claimed 3F0h-3F7h, so the controller did not exist -- while the BIOS
 * SERVICE built on top of it did: INT 13h reads and writes sectors out of a real
 * image file in the host, the geometry is measured against 6.22, and both firmware
 * advertisements (CMOS byte 10h, the INT 11h equipment word) say a 1.44M drive is
 * fitted. Firmware present, hardware absent -- the same split the 8042 and the
 * MC146818 turned out to have, and the third one in a row.
 *
 * ⛔⛔⛔ AND THE FAILURE SHAPE WAS A HANG, NOT A WRONG ANSWER. An unclaimed ISA
 *   port reads 0xFF here, deliberately, so that device detection cannot mistake an
 *   absent card for a present one. 3F4h is the MAIN STATUS REGISTER, and 0xFF
 *   there is RQM=1 with DIO=1 -- "ready, and I am the one talking". The
 *   command-write loop out of the datasheet, which is the loop in every BIOS and
 *   every driver ever written,
 *
 *       wait:  in al,3F4h / and al,0C0h / cmp al,80h / jne wait
 *
 *   compares C0h against 80h, never matches, and SPINS FOR EVER. No fault, no
 *   timeout, nothing in any log. This is the RTC's UIP bit one surface later: an
 *   absent chip whose float value happens to mean "wait". 00h would hang too (RQM
 *   clear is also "wait"), so there is no default that saves it -- only the chip.
 *
 * ★ MEASURED (p_fdc.asm, 2026-09-23) before a line of this was written, on the
 *   bare-metal rig against two oracles that agree:
 *
 *       case            6.22/QEMU   PCem      NTVDMEX
 *       fdc.msr.idle    0080        0080      00FF
 *       fdc.cmdwait     0080        0080      01C0   <- AH=01: it never exited
 *       fdc.version     0190        0190      FFFF
 *       fdc.dumpreg     0A01        0A01      FFFF
 *
 *   `fdc.cmdwait` is not a register value, it is the loop above, bounded to 65536
 *   turns and asked whether it terminated. On both oracles it did.
 *
 * ── WHAT THIS IS AND IS NOT. ────────────────────────────────────────────────────
 * This is the CHIP: the register file, the three-phase command protocol, and every
 * command that does not move sector data. That is the whole of what a detection
 * routine asks and the whole of the hang.
 *
 * ⛔ THE DATA COMMANDS ARE **PART**, AND DELIBERATELY SO. READ DATA, WRITE DATA,
 *   READ TRACK and FORMAT TRACK are recognised, consume their parameters, and
 *   terminate with the documented ABNORMAL TERMINATION result (ST0 bits 7:6 = 01,
 *   ST1 = no data) rather than pretending to be invalid commands. They need the
 *   8237A wired to the image handle INT 13h already holds, which is the next step
 *   and a bigger one. A reported I/O error is not a good answer; it is a
 *   *reported* one, which is the entire difference being bought here.
 *
 * ── DORMANT BY DEFAULT. ─────────────────────────────────────────────────────────
 * IRQ6 is raised only in response to a command the guest issued, and only when the
 * guest has cleared neither DOR bit 3 (DMAGATE) nor the PIC's mask -- and the
 * master IMR starts at 0xFC, IRQ0 and IRQ1 only. A guest that does not ask sees no
 * change, the same property that made the RTC's IRQ8 safe to add.
 *
 * Pure C, no <windows.h>: nothing here touches a file or a clock, so the whole
 * chip is exercised off-VM by tests/unit/fdc_test.c.
 */
#ifndef NTVDMEX_VDD_FDC_H
#define NTVDMEX_VDD_FDC_H

#include "vdd_bus.h"

#define FDC_BASE        0x3F0
#define FDC_DOR         0x3F2       /* R/W: motors, DRIVE SEL, /RESET, DMAGATE  */
#define FDC_TDR         0x3F3       /* R/W: tape drive register                 */
#define FDC_MSR         0x3F4       /* R  : main status  /  W: DSR              */
#define FDC_FIFO        0x3F5       /* R/W: command and result bytes            */
#define FDC_DIR         0x3F7       /* R  : digital input / W: CCR              */

/* ⚠ 3F0h and 3F1h (SRA/SRB) ARE NOT CLAIMED, and 3F6h is not ours at all -- it
     is the ATA alternate status register. See the notes in vdd_fdc.c. */

/* MSR bits (docs/ref/fdc.md 3). */
#define FDC_MSR_RQM     0x80        /* the FIFO is ready for a transfer         */
#define FDC_MSR_DIO     0x40        /* 1 = FDC -> CPU (a result is waiting)     */
#define FDC_MSR_NDMA    0x20        /* execution phase in non-DMA mode          */
#define FDC_MSR_CB      0x10        /* a command is in progress                 */

/* DOR bits (docs/ref/fdc.md 7). */
#define FDC_DOR_MOTORS  0xF0
#define FDC_DOR_DMAGATE 0x08        /* 0 disconnects IRQ6 and the DMA request   */
#define FDC_DOR_NRESET  0x04        /* ACTIVE LOW: 0 holds the chip in reset    */
#define FDC_DOR_DSEL    0x03

#define FDC_PHASE_CMD   0           /* the host is writing a command            */
#define FDC_PHASE_RES   1           /* the host is reading a result             */

typedef struct fdc_state {
    VDD_BUS *bus;

    uint8_t  dor;                   /* 3F2h, as last written                    */
    uint8_t  tdr;                   /* 3F3h                                     */
    uint8_t  dsr;                   /* 3F4h write: data rate, precomp, power    */
    uint8_t  ccr;                   /* 3F7h write: data rate, the other door    */
    uint8_t  in_reset;              /* DOR bit 2 is low: the chip is held down  */

    /* ── THE CONVERSATION. This is the part that makes the chip a chip. ────── */
    uint8_t  phase;                 /* FDC_PHASE_CMD or FDC_PHASE_RES           */
    uint8_t  cmd[16];               /* the command byte and its parameters      */
    uint8_t  cmd_len;               /* how many have arrived                    */
    uint8_t  cmd_want;              /* how many this command takes, opcode incl.*/
    uint8_t  res[16];               /* the result bytes, oldest first           */
    uint8_t  res_len, res_pos;

    /* ── PER-DRIVE STATE. ───────────────────────────────────────────────────── */
    uint8_t  pcn[4];                /* present cylinder number                  */

    /* ── THE INTERRUPT, AND THE COMMAND THAT COLLECTS IT. ───────────────────── */
    uint8_t  irq_pending;           /* an interrupt is waiting to be sensed      */
    uint8_t  st0_pending;           /* what SENSE INTERRUPT STATUS will report   */
    /* Reset leaves the chip expecting FOUR sense-interrupts, one per drive, each
       answering C0h|drive ("ready changed") -- unless CONFIGURE turned drive
       polling off. Four, not one: a model that answers a single sense leaves a
       driver that issued four with 80h for three of them. (ref/fdc.md 8) */
    uint8_t  poll_drive;            /* 0..4: how many of the four are still owed */
    uint8_t  poll_off;              /* CONFIGURE asked for no drive polling      */

    /* ── WHAT THE FIRMWARE PROGRAMMED, so DUMPREG can hand it back. ─────────── */
    uint8_t  srt_hut;               /* SPECIFY byte 1: step rate | head unload   */
    uint8_t  hlt_nd;                /* SPECIFY byte 2: head load | non-DMA       */
    uint8_t  cfg_byte2, cfg_pretrk; /* CONFIGURE bytes 2 and 3                   */
    uint8_t  perp;                  /* PERPENDICULAR MODE                        */
    uint8_t  locked;                /* LOCK: keep CONFIGURE across a s/w reset   */
    uint8_t  last_eot;              /* the sector count of the last data command */

    /* Counters a run can report -- the same habit as every other VDD here. */
    uint32_t cmds, invalids, irqs, resets;
} fdc_state;

int  vdd_fdc_init(VDD_BUS *b, void *self);
void vdd_fdc_reset(void *self);

static inline NTVDD_DEVICE vdd_fdc_device(fdc_state *st)
{ NTVDD_DEVICE d; d.Name = "fdc"; d.Initialize = vdd_fdc_init; d.Reset = vdd_fdc_reset;
  d.Shutdown = 0; d.Context = st; return d; }

/* Exposed for the off-VM battery, which drives the chip through the same two
   doors the guest does rather than reaching into the struct. */
void    vdd_fdc_out(void *self, uint16_t port, uint8_t width, uint32_t val);
void    vdd_fdc_in (void *self, uint16_t port, uint8_t width, uint32_t *val);
uint8_t vdd_fdc_msr(const fdc_state *st);

#endif /* NTVDMEX_VDD_FDC_H */
