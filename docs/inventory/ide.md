# Inventory — IDE / ATA (and ATAPI) host adapter

**Spec:** ANSI *AT Attachment* (ATA-1, X3.221-1994) and its successors through ATA/ATAPI-4
for the PACKET command set; IBM PC/AT TechRef for the board wiring (IRQ14, the `1F0h`/`3F6h`
blocks). ⚠ **Not held in the repo** — [`../ref/SOURCES.md`](../ref/SOURCES.md). No
`docs/ref/ide.md`.
**Our implementation:** **none.** No VDD claims `1F0h`–`1F7h`, `3F6h`, `170h`–`177h` or
`376h`. An unclaimed port reads `FFh` and a write is dropped (`main.c:13481-13496`, the
"unclaimed ISA port floats high" rule), and the port is listed in the run's
`unclaimed ports touched` line (`io_unclaimed_note`, `main.c:13121-13128`).
**Probe:** `tools/dostest/p_fdc.asm` asks `3F6h` as its `fdc.alt.3f6` case — that is how
this surface was found ([sweep.md](sweep.md)). No ATA probe exists.
**Marked:** 2026-10-01, **from the code** — i.e. from the absence of any claim
(`vdd_claim_ports` call sites, `src/vdd/*.c`).

---

## Headline

**The machine has no IDE adapter, and that was never decided — it is simply what
happened.** The rest of the project's disk story *is* a decision: INT 13h exposes no fixed
disk, and a drive is an image file or it is absent ([dos-services.md](dos-services.md) §6).
But a 386/486 of the period has an IDE adapter on the board, and both oracles that answered
the `3F6h` question have one (`50h`: DRDY + DSC, a ready drive). We answer `FFh`.

⚠ **`FFh` is not a neutral answer for this chip.** In the ATA status register bit 7 is
**BSY**, and the datasheet's own wait is "poll until BSY clears". A driver that does not
first special-case `FFh` as "nothing here" waits for its timeout — the shape recorded in
[fdc.md](fdc.md) for the floppy controller, whose MSR read `FFh` and whose command loop
never exited. Whether real detection code special-cases it is a
question for the guests, and has not been asked.

**The choice to make** (not made here): either (a) no adapter, and say so consistently —
`0040:0075` = 0 ([bda.md](bda.md) §3), INT 13h `DL=80h` "not ready", IRQ14 never raised; or
(b) an adapter with **no drives**, so detection code finds the controller, sees no device
(BSY clear — the ATA documents put a pull-down on DD7 for exactly this; the value to
answer is to be read off the document, not from memory), and moves on at once. (b) is the period-correct board; (a) is today's behaviour by accident.

| Group | Units | IMPL | PART | STORE | MISS | N/A |
|---|---|---|---|---|---|---|
| §1 Primary channel registers | 10 | — | 1 | — | 9 | — |
| §2 Secondary channel registers | 10 | — | — | — | 10 | — |
| §3 Interrupts and DMA | 2 | — | — | — | 2 | — |
| §4 ATA / ATAPI command set | 9 | — | — | — | 9 | — |
| §5 The BIOS side | 3 | — | — | — | — | 3 |
| **Total** | **34** | **—** | **1** | **—** | **30** | **3** |

---

## 1. Primary channel (`1F0h`–`1F7h`, `3F6h`–`3F7h`)

| Port | Access | Register | Status | Notes |
|---|---|---|---|---|
| `1F0h` | R/W (16-bit) | Data | **MISS** | unclaimed: reads `FFFFh` |
| `1F1h` | R / W | Error / Features | **MISS** | |
| `1F2h` | R/W | Sector Count | **MISS** | |
| `1F3h` | R/W | Sector Number / LBA 7:0 | **MISS** | |
| `1F4h` | R/W | Cylinder Low / LBA 15:8 | **MISS** | |
| `1F5h` | R/W | Cylinder High / LBA 23:16 | **MISS** | a write-then-read of `1F4h`/`1F5h` is the classic "is there a controller" probe; it reads back `FFh` |
| `1F6h` | R/W | Drive/Head (DEV bit, LBA 27:24) | **MISS** | |
| `1F7h` | R / W | Status / Command | **MISS** | reads `FFh` — BSY set (see the headline) |
| `3F6h` | R / W | Alternate Status / Device Control (SRST, nIEN) | **MISS** | measured `FFh`; 6.22/QEMU and PCem both `50h` (`p_fdc fdc.alt.3f6`, [fdc.md](fdc.md)) |
| `3F7h` | R | Drive Address (bits 6:0) — shared with the FDC's DIR (bit 7) | **PART** | the FDC claims the byte (`vdd_fdc.c:435`) and answers bit 7; bits 6:0 are the ATA adapter's on an AT, and are recorded in [fdc.md](fdc.md) as a choice, not a measurement |

## 2. Secondary channel (`170h`–`177h`, `376h`–`377h`)

All ten registers as §1: **MISS**, unclaimed, read `FFh`. (Ten rows in the count; not
repeated here.)

## 3. Interrupts and DMA

| Unit | Status | Notes |
|---|---|---|
| IRQ14 (primary) / IRQ15 (secondary) | **MISS** | nothing raises them; the vectors (`76h`/`77h`) point at our shared `IRET` stub (`main.c:27011-27018`) |
| Bus-master / multiword DMA | **MISS** | no PCI IDE function, no `8237` channel use |

## 4. Command set

All **MISS** — there is no register file to receive a command.

| Command | Code | Status |
|---|---|---|
| IDENTIFY DEVICE | `ECh` | **MISS** |
| READ SECTORS / WRITE SECTORS | `20h` / `30h` | **MISS** |
| READ MULTIPLE / WRITE MULTIPLE / SET MULTIPLE MODE | `C4h` / `C5h` / `C6h` | **MISS** |
| INITIALIZE DEVICE PARAMETERS | `91h` | **MISS** |
| EXECUTE DEVICE DIAGNOSTIC | `90h` | **MISS** |
| RECALIBRATE / SEEK | `1xh` / `7xh` | **MISS** |
| SET FEATURES | `EFh` | **MISS** |
| ATAPI: IDENTIFY PACKET DEVICE | `A1h` | **MISS** |
| ATAPI: PACKET (the CD-ROM path MSCDEX drivers use) | `A0h` | **MISS** |

## 5. The BIOS side

| Unit | Status | Notes |
|---|---|---|
| INT 13h `DL=80h`+ (fixed disks) | **N/A** | by design: no raw access to the host's disks ([dos-services.md](dos-services.md) §6) |
| `0040:0075` number of fixed disks | **N/A** | follows from the line above; ⚠ but it is not *written* 0 ([bda.md](bda.md) §3) |
| INT 41h / INT 46h fixed-disk parameter tables | **N/A** | no fixed disk to describe; the vectors are not ours |

---

## What to fix, in order

1. Decide (a) or (b) in the headline, and write it down where the disk design lives
   (`src/dos/dos_disk.h`).
2. If (b): claim both channels and answer as an adapter with no device behind it — status
   `00h` after a `SRST`, no BSY, IRQs masked — so detection finishes immediately. That
   closes `p_fdc fdc.alt.3f6` against both oracles by *owning* the register, which is the
   condition [sweep.md](sweep.md) set for it.
3. Only then, if a guest needs one, an ATAPI CD-ROM behind it (the MSCDEX inventory).
