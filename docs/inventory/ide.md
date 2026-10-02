# Inventory — IDE / ATA (and ATAPI) host adapter

**Spec:** ANSI *AT Attachment* (ATA-1, X3.221-1994) and its successors through ATA/ATAPI-4
for the PACKET command set; IBM PC/AT TechRef for the board wiring (IRQ14, the `1F0h`/`3F6h`
blocks). ⚠ **Not held in the repo** — [`../ref/SOURCES.md`](../ref/SOURCES.md). No
`docs/ref/ide.md`.
**Our implementation:** `src/vdd/vdd_ide.c` / `vdd_ide.h` (GH #179, 2026-10-02) — **an adapter
fitted, both channels EMPTY.** Claims `1F0h`–`1F7h`, `3F6h`, `170h`–`177h`, `376h`–`377h`
(`3F7h` stays the FDC's). Every read is `00h` at any width, every write is dropped, no IRQ.
**Spec held:** ATA-3, X3T13/2008D rev 7b (working draft, fetched for #179 — Table 2 note 3,
8.7.1, 8.7.2). Not mirrored in the repo.
**Probes:** `tools/dostest/p_ide.asm` (12 rows, both channels); `p_fdc.asm fdc.alt.3f6`.
Off-VM: `tools/dostest/ide_test.c` (32 checks, with the pre-#179 hang as a negative control).
**Marked:** 2026-10-01 from the code (absence of any claim); **re-marked 2026-10-02** for #179.

---

## Headline

### ✅ 2026-10-02 (#179): decided (b) — an adapter with no drives — and built

| case (`p_ide`) | 6.22/QEMU | dosbox-x | PCem | ours **before** | ours **after** |
|---|---|---|---|---|---|
| `ide.pri.bsywait` — *does the ATA's own BSY wait exit?* | `0000` | `0000` | `0000` | **`0001`** ⛔ hang | **`0000`** ✅ |
| `ide.pri.alt.bsy` | `0000` | `0000` | `0000` | `0080` | `0000` ✅ |
| `ide.sec.bsywait` | `0000` | `0000` | `0000` | **`0001`** ⛔ | `0000` ✅ |
| `ide.sec.alt.bsy` | `0000` | `0000` | `0000` | `0080` | `0000` ✅ |
| `ide.pri.dev1.alt` — absent device 1 (ATA-3 8.7.1 h) | `0000` | `0000` | `0000` | `00FF` | `0000` ✅ |
| `ide.sec.dev1.alt` | `0000` | `0000` | `0000` | `00FF` | `0000` ✅ |
| `ide.sec.alt.raw` | `0000` | `0000` | `0000` | `00FF` | `0000` ✅ |
| `ide.pri.alt.raw` | `0050` | `0000` | `0050` | `00FF` | `0000` — *machine fact* |
| `ide.pri.echo` (55h/AAh read-back) | `55AA` | `0000` | `55AA` | `FFFF` | `0000` — *machine fact* |
| `ide.pri.status.alt` | `5050` | `0000` | `5050` | `FFFF` | `0000` — *machine fact* |
| `ide.sec.echo` | `55AA` | `0000` | `55AA` | `FFFF` | `0000` — *machine fact* |
| `ide.sec.status.alt` | `5050` | `0000` | `0000` | `FFFF` | `0000` — *machine fact* |

"Before" = host `491216ad` (`918c3a0`), "after" = `2ae28649`. **The four contract rows hold
on all four hosts**; the five *machine fact* rows are drive-present (6.22, PCem) against
drive-absent (dosbox-x, us) and are abstained in `oracle-rules.json` with that rationale.
**dosbox-x is exactly our configuration** — adapter fitted, no drive — and answers `00h` on
every one of the twelve rows, as we do. `p_fdc fdc.alt.3f6` now reads `0000` (owned; the
oracles' `0050` abstained the same way).

**Why `00h` and not `7Fh`** (the other value an empty channel is known to read):
- DD7 = BSY = 0 is from the document: *"a host have a 10 kΩ pull-down resistor … on DD7 to
  allow a host to recognize the absence of a device"* (ATA-3 Table 2 note 3).
- DD6:0 are undriven and the document does not name a value — **0 is a recorded choice**:
  it is what the same standard prescribes for an absent device's status where it does speak
  (8.7.1 h: device 0 answers for absent device 1 with `00h` after reset — measured `0000`
  on all three oracles, `*.dev1.alt`), and what QEMU, Bochs and dosbox-x answer.
- `7Fh` reads DRDY=1 DRQ=1 ERR=1 — a routine that waits for BSY clear then DRDY would
  believe a drive is ready with data, issue IDENTIFY and read 256 words of float.
- **Nothing latches**: the 55h/AAh presence test (ATA-3 8.7.2's own host note) must not find
  a drive. Writes go nowhere; an IDENTIFY written to `1F7h` raises no DRQ and no IRQ14.

The BIOS side now says the same thing: `0040:0075` (fixed disks) is **written** `0`
(`main.c`, beside the adapter's `vdd_bus_add`). One fact, three doors.

⚠ `VDD_MAX_DEV` was 16 with 15 used (16 with GUS + EMU8K): the adapter would have been the
17th and silently refused — the MPU-401 trap again. Raised to 24.

### The original headline (2026-10-01), kept for the record

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

Re-marked 2026-10-02 against decision (b). A register of an **empty** channel is IMPL when it
answers what an empty channel answers; the command set is N/A *by that decision* (there is
no device to execute it) and returns to MISS the day a drive is fitted.

| Group | Units | IMPL | PART | STORE | MISS | N/A |
|---|---|---|---|---|---|---|
| §1 Primary channel registers | 10 | 9 | 1 | — | — | — |
| §2 Secondary channel registers | 10 | 10 | — | — | — | — |
| §3 Interrupts and DMA | 2 | 1 | — | — | — | 1 |
| §4 ATA / ATAPI command set | 9 | — | — | — | — | 9 |
| §5 The BIOS side | 3 | 1 | — | — | — | 2 |
| **Total** | **34** | **21** | **1** | **—** | **—** | **12** |

---

## 1. Primary channel (`1F0h`–`1F7h`, `3F6h`–`3F7h`)

| Port | Access | Register | Status | Notes |
|---|---|---|---|---|
| `1F0h` | R/W (16-bit) | Data | **IMPL** (empty) | `0000h` at 16 bits, `0` at 32; writes dropped |
| `1F1h` | R / W | Error / Features | **IMPL** (empty) | `00h` |
| `1F2h` | R/W | Sector Count | **IMPL** (empty) | `00h`; does **not** latch — `ide.pri.echo` |
| `1F3h` | R/W | Sector Number / LBA 7:0 | **IMPL** (empty) | ditto |
| `1F4h` | R/W | Cylinder Low / LBA 15:8 | **IMPL** (empty) | `00h` |
| `1F5h` | R/W | Cylinder High / LBA 23:16 | **IMPL** (empty) | `00h` |
| `1F6h` | R/W | Drive/Head (DEV bit, LBA 27:24) | **IMPL** (empty) | `00h`; device 1 selected reads `00h` too — `ide.pri.dev1.alt`, all four hosts |
| `1F7h` | R / W | Status / Command | **IMPL** (empty) | `00h`, BSY clear; a command is counted (`ide_state.cmds`) and received by nobody |
| `3F6h` | R / W | Alternate Status / Device Control (SRST, nIEN) | **IMPL** (empty) | `00h`; SRST/nIEN are DEVICE bits, so nothing latches them |
| `3F7h` | R | Drive Address (bits 6:0) — shared with the FDC's DIR (bit 7) | **PART** | the FDC claims the byte (`vdd_fdc.c:435`) and answers bit 7; bits 6:0 are the ATA adapter's on an AT, and are recorded in [fdc.md](fdc.md) as a choice, not a measurement |

## 2. Secondary channel (`170h`–`177h`, `376h`–`377h`)

All ten registers as §1: **IMPL (empty)**, `170h`–`177h` + `376h`, and `377h` (the
secondary's drive address register — no second FDC sits beside it, so it is claimed here and
reads `00h`). (Ten rows in the count; not repeated here.)

## 3. Interrupts and DMA

| Unit | Status | Notes |
|---|---|---|
| IRQ14 (primary) / IRQ15 (secondary) | **IMPL** (empty) | INTRQ is driven by a device; with none, never raised (`ide_test`: IDENTIFY → no IRQ). The vectors (`76h`/`77h`) point at our shared `IRET` stub |
| Bus-master / multiword DMA | **N/A** | no PCI IDE function on a period ISA adapter; nothing to move |

## 4. Command set

All **N/A by decision (b)** — the register file is the DRIVE's, and there is no drive. A
command written to `1F7h`/`177h` is counted and dropped. Each row returns to **MISS** the day
a drive (or an ATAPI CD-ROM) is fitted.

| Command | Code | Status |
|---|---|---|
| IDENTIFY DEVICE | `ECh` | N/A (no drive) |
| READ SECTORS / WRITE SECTORS | `20h` / `30h` | N/A (no drive) |
| READ MULTIPLE / WRITE MULTIPLE / SET MULTIPLE MODE | `C4h` / `C5h` / `C6h` | N/A (no drive) |
| INITIALIZE DEVICE PARAMETERS | `91h` | N/A (no drive) |
| EXECUTE DEVICE DIAGNOSTIC | `90h` | N/A (no drive) |
| RECALIBRATE / SEEK | `1xh` / `7xh` | N/A (no drive) |
| SET FEATURES | `EFh` | N/A (no drive) |
| ATAPI: IDENTIFY PACKET DEVICE | `A1h` | N/A (no drive) |
| ATAPI: PACKET (the CD-ROM path MSCDEX drivers use) | `A0h` | N/A (no drive) |

## 5. The BIOS side

| Unit | Status | Notes |
|---|---|---|
| INT 13h `DL=80h`+ (fixed disks) | **N/A** | by design: no raw access to the host's disks ([dos-services.md](dos-services.md) §6) |
| `0040:0075` number of fixed disks | ✅ **IMPL** | **written** `0` at start-up since #179 (`main.c`, beside the adapter) — agrees with INT 13h and the empty channels |
| INT 41h / INT 46h fixed-disk parameter tables | **N/A** | no fixed disk to describe; the vectors are not ours |

---

## What to fix, in order

1. ✅ Decided (b) and built (#179, 2026-10-02) — recorded in `src/vdd/vdd_ide.h`.
2. ✅ Owned `3F6h`; `p_fdc fdc.alt.3f6` closed by ownership.
3. An ATAPI CD-ROM behind the secondary channel (the MSCDEX inventory) — a separate decision.
