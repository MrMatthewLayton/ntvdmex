# Inventory — 82077AA floppy disk controller

**Spec:** Intel 82077AA datasheet; NEC µPD765A; IBM PC/AT TechRef.
**▶ The hardware reference is [`../ref/fdc.md`](../ref/fdc.md)** — what the chip *does*.
**Our implementation:** `src/vdd/vdd_fdc.c`, `src/vdd/vdd_fdc.h`.
**Oracles:** MS-DOS 6.22 (QEMU), PCem (real AMI 486 BIOS), dosbox-x.
DOS probe: `tools/dostest/p_fdc.asm` · off-VM battery: `tools/dostest/fdc_test.c` (32 checks).
**Marked:** 2026-09-23, **from the code**, then re-marked against the rig.

---

## Headline — the third "firmware present, chip absent" hole, and it was a hang

The BIOS layer was already complete and correct: INT 13h reads and writes sectors out of a
real image file (`src/host/main.c:24736`, `disk_io` at `main.c:3986`), the geometry is
measured against 6.22 rather than assumed (`src/dos/dos_disk.h`), and both firmware
advertisements agreed a drive was fitted — CMOS byte `10h` = `40h` (`vdd_cmos.c:274`) and
INT 11h's equipment word bit 0 (`main.c:2074`).

**The chip underneath all of that did not exist.** Nothing claimed `3F0h`–`3F7h`, so every
port fell through to the unclaimed default of **`FFh`** (`main.c:10918`). `3F4h` is the
Main Status Register and `FFh` there is `RQM=1, DIO=1` — *"ready, and I am the one
talking"* — so the command-write loop out of the datasheet, which is the loop in every
BIOS and every driver,

```asm
wait:   in al,3F4h / and al,0C0h / cmp al,80h / jne wait
```

**spun for ever**, with no fault, no timeout and nothing in any log. This is the
MC146818's UIP bit one surface later. `00h` would hang too (RQM clear is also *wait*), so
no default saves it — only the chip.

⚠ **Why nothing had noticed.** Our guests reach the drive through INT 13h, which never
touches the ports. The programs that go direct were never on the shelf: disk utilities,
copy-protection loaders, `FORMAT`'s low-level path, Windows 3.x enhanced mode's own floppy
virtualisation. Per the scope rule that is not a reason to defer — it is the reason this
went unmeasured for 77 sessions.

---

## Measured — before, and after

`p_fdc.asm` asks the questions a **detection routine** asks, on the bare-metal rig and on
two oracles that agree with each other.

| case | 6.22/QEMU | PCem | NTVDMEX **before** | NTVDMEX **after** |
|---|---|---|---|---|
| `fdc.msr.idle` — the byte the protocol turns on | `0080` | `0080` | `00FF` | **`0080`** ✅ |
| `fdc.cmdwait` — *does the datasheet's own loop exit?* | `0080` | `0080` | **`01C0`** ⛔ | **`0080`** ✅ |
| `fdc.dor.gate` — /RESET and DMAGATE | `000C` | `000C` | `000C` ⚠ | **`000C`** ✅ |
| `fdc.dir.dskchg` — the disk-change line | `0000` | `0000` | `0080` ⛔ | **`0000`** ✅ |
| `fdc.version` — count + ID byte | `0190` | `0190` | `FFFF` | **`0190`** ✅ |
| `fdc.dumpreg` — count + first byte | `0A01` | `0A01` | `FFFF` | `0A00` ⚠ |
| `fdc.alt.3f6` — *not ours* | `0050` | `0050` | `00FF` | `0000` ✅ owned by the IDE adapter (#179); the oracles' drive abstained |
| `fdc.desync` — what we left behind | `0000` | `0000` | `0000` | `0000` ✅ |
| `fdc.dor.raw` | `001C` | `00FF` | `00FF` | `000C` — *disputed* |
| `fdc.dir.raw` | `0000` | `0001` | `00FF` | `0000` — *disputed* |
| `fdc.sra.srb` | `FFC1` | `FF51` | `FFFF` | `FFFF` — *disputed* |

★★★ **`fdc.cmdwait` is not a register value — it is the loop, bounded to 65536 turns and
asked whether it terminated.** `AH=01` means it did not. That row is the whole reason this
surface was urgent, and it is the row that moved.

⛔⛔ **`fdc.dor.low` was a manufactured agreement and it is gone.** The case originally
masked DOR to bits 3:0 — and `FFh & 0Fh` is `0Fh`, a perfectly plausible DOR (out of
reset, gated, drive 3). NTVDMEX scored `000F` against PCem's `000F` and **read as a MATCH
from a port that did not exist.** The coincidence belonged to the mask, not to the
machine. The case now emits the raw byte, where `FFh` cannot hide, and narrows the
adjudicable question to the two bits that are a property of the *chip* rather than of the
driver. *A property check passes by luck; pin exact values.*

---

## 1. The register file

| Offset | Register | Status | Evidence |
|---|---|---|---|
| `3F0h` | SRA (PS/2 only) | **N/A** — not claimed | both oracles read `FFh` here; we present a PC/AT part, which does not drive it |
| `3F1h` | SRB (PS/2 only) | **N/A** — not claimed | both oracles *do* drive it and **disagree** (`C1h` vs `51h`); nothing to copy |
| `3F2h` | **DOR** — motors, DRIVE SEL, /RESET, DMAGATE | ✅ **IMPL** | `vdd_fdc_out`; reset edge, gate and drive select all honoured |
| `3F3h` | TDR tape drive | **STORE** | round-trips; nothing consumes it, and neither does any oracle |
| `3F4h` read | **MSR** | ✅ **IMPL** | `vdd_fdc_msr` — **derived, never stored** |
| `3F4h` write | DSR — data rate, s/w reset, power down | ✅ **IMPL** | bit 7 resets and self-clears; the rate survives it |
| `3F5h` | **FIFO** — command and result bytes | ✅ **IMPL** | `fdc_fifo_read`/`fdc_fifo_write` |
| `3F6h` | *(ATA alternate status — not ours)* | **N/A** | deliberately not claimed; see the open row below |
| `3F7h` read | **DIR** — DSKCHG | ✅ **IMPL** | bit 7 = 0; bits 6:0 recorded as a choice, not measured |
| `3F7h` write | CCR — data rate | **STORE** | there is no wire to run at the wrong speed |

★ **MSR is derived from state that lives elsewhere, never latched.** A stored copy is a
second opinion waiting to drift — the same rule the 8254's OUT pin arrived at, and a
stronger one here because MSR *is* the protocol.

## 2. The protocol

| Item | Status | Evidence |
|---|---|---|
| Command / execution / result phases | ✅ **IMPL** | `fdc_execute`, `fdc_result` |
| RQM/DIO handshake per byte | ✅ **IMPL** | measured: `fdc.cmdwait` `0080` on the rig |
| `CMD BSY` set on the first command byte, cleared on the **last** result byte | ✅ **IMPL** | `fdc_test.c` checks both edges — it is what lets a driver drain a result of unknown length |
| Invalid command → one byte, `ST0 = 80h`, **and stays in frame** | ✅ **IMPL** | the framing matters more than the value: an unknown opcode that guessed a parameter count would eat the next real command |
| Non-DMA execution (MSR bit 5) | ⛔ **MISS** | there is no execution phase to be in yet |
| RQM dropping between bytes | ⚠ **N/A-by-design** | we have no latency to model; observable only by timing, and in the direction that cannot hang anyone |

## 3. The commands

| Command | Status | Notes |
|---|---|---|
| `10h` **VERSION** → `90h` | ✅ **IMPL** | measured `0190` on the rig and both oracles |
| `08h` **SENSE INTERRUPT STATUS** | ✅ **IMPL** | pending → ST0+PCN · reset polling → `C0h\|drive` · otherwise `80h` |
| `0Eh` **DUMPREG** — ten bytes | ✅ **IMPL** | count measured at 10 on all three |
| `03h` SPECIFY · `13h` CONFIGURE · `12h` PERPENDICULAR | ✅ **IMPL** | stored, and handed back by DUMPREG |
| `14h` LOCK | ✅ **IMPL** | and it really keeps CONFIGURE across a software reset |
| `04h` SENSE DRIVE STATUS (ST3) | ✅ **IMPL** | READY, TRACK0, TWO SIDE, head, drive |
| `07h` RECALIBRATE · `0Fh` SEEK | ✅ **IMPL** | no result phase; the interrupt is the report |
| `0Ah` READ ID | **PART** | answers from the present cylinder without touching the medium |
| `06h` READ · `05h` WRITE · `02h` READ TRACK · `0Ch` READ DELETED · `0Dh` FORMAT | ⛔ **PART** | **see below** |

### ✅ The DUMPREG byte order was written from memory — and it is right

`vdd_fdc.c`'s ten result bytes came out of my recollection of the datasheet's table and
went straight into the code and into [`ref/fdc.md`](../ref/fdc.md) without anything
checking them. **That is the exact shape this project keeps getting wrong**: a register
that is implemented, plausible, and never compared — a wrong order does not *fail*, it
hands a driver ten believable numbers in the wrong slots.

PCem's own source is vendored in this tree (`pcem/src/pcem-dev/src/floppy/fdc.c:961`), and
it is the implementation that produced the measured `0A01`. Its order is:

```
track[0] track[1] 0 0  specify[0] specify[1] eot  (perp&0x7f)|lock  config pretrk
```

**Byte for byte ours.** It also confirms the reading of the measured first byte: `01` is
the **present cylinder of drive 0**, so *"where the head is"* was the right
interpretation of the one row where we differ. PCem further confirms `0x94`/`0x14` as
LOCK/UNLOCK (bit 7 of the opcode), which is how we decode it.

⚠ **Corroboration, not proof.** PCem is a reimplementation; agreement with it raises
confidence in a reading of the datasheet, it does not replace one. So the order was then
**asked of the machines** — `build/probes/p_fdcreg.asm`, a read-only probe that emits all
ten bytes:

| | PCN0 | PCN1 | PCN2 | PCN3 | SRT/HUT | HLT/ND | EOT | LOCK\|PERP | CONFIG | PRETRK |
|---|---|---|---|---|---|---|---|---|---|---|
| **6.22/QEMU** | `01` | `00` | `00` | `00` | `0A` | `03` | **`12`** | `00` | `60` | `00` |
| **PCem** | `00` | `00` | `00` | `00` | `BF` | `02` | `24` | `0C` | `08` | `00` |
| **ours** | `00` | `00` | `00` | `00` | `00` | `00` | `00` | `00` | `00` | `00` |

★ **`12h` = 18 is the sectors-per-track of a 1.44M floppy**, which pins byte 7 as EOT
beyond argument; the PCN block and PRETRK agree at zero on both machines. **The order is
confirmed.** Count = `000A` and MSR = `0080` agree on all three.

### ⚠ But our ten bytes are all zero, and that is a gap this probe found

Nothing has ever issued SPECIFY or CONFIGURE to us, and DUMPREG hands back what arrived
rather than inventing defaults — so a guest that reads it learns nothing. On a real
machine POST has already programmed the chip. This is the same question the CMOS Status
A/B bytes and the 8042's status register both had to answer, and it splits three ways:

| Byte | Verdict |
|---|---|
| **EOT** (byte 7) | ✅ **Correctly zero.** It is the sector count of the **last data command**, not configuration — PCem's own `res[7] = eot[drive]` is set by read/write. We have no data commands yet, so there is no residue. It will populate itself when the data path lands |
| **SRT/HUT, HLT/ND, CONFIG, LOCK\|PERP** | ⚠ **Not adjudicable.** The two oracles disagree on *every one* (`0A`/`BF`, `03`/`02`, `60`/`08`, `00`/`0C`) because they are a **BIOS's** choices, not the chip's — cause 1 in [`oracle-disagreements.md`](../research/oracle-disagreements.md). There is no value to copy, and a guest that cares issues SPECIFY itself. **Zero is recorded as a choice, not a measurement** |
| **PCN** (bytes 1–4) | ⚠ Truthful — our head has never moved. See the coherence row below |

### ⛔ …and PCem does **not** implement `09h`/`0Ch`. Do not "fix" toward it

PCem's command dispatch accepts `03 04 05 06 07 08 0a 0d 0e 0f 10 12 13 14/94` (and `42`)
— and **not `09h` WRITE DELETED DATA, `0Ch` READ DELETED DATA, `11h` or `18h`.**

That splits the two framing decisions made this session, and they must not be lumped:

| Opcode | Our call | PCem | Verdict |
|---|---|---|---|
| `11h` SCAN EQUAL, `18h` part ID | **invalid** | invalid | ✅ agreed — µPD765/PC8477 commands an 82077AA does not have |
| `09h`, `0Ch` **deleted-data** | 9-byte data commands | **not implemented** | ⚠ **The spec outranks the oracle here** |

The deleted-data commands *are* in the 82077AA command set. PCem omits them because no
guest it runs has ever issued one — *"device models that stop where their workloads stop"*,
the pattern already written up in
[`oracle-disagreements.md`](../research/oracle-disagreements.md). **A `0` from a host
without the feature is the absence of a measurement, not a measurement of absence** —
exactly the reasoning that let PIT BCD be implemented against two silent oracles.

⛔ **Consequence for the probe:** `p_fdc` must never gain a case that asks PCem about `09h`
or `0Ch`, because PCem would answer "invalid", the row would read as OUR mismatch, and the
repair would be to *delete a command the part has*. Recorded as **implemented from the
spec, unconfirmed by any machine** — which is the honest state, and a smaller claim than
the rest of this page.

⛔ **The data commands are PART, deliberately, and loudly.** They are recognised, consume
their parameters, and terminate with the documented **abnormal termination** — ST0
interrupt code `01`, ST1 bit 2 (*no data*), the full seven result bytes and the interrupt
a real part raises. They do **not** answer *"invalid command"*: that would be a chip
contradicting the `90h` it just gave for VERSION, and a driver can act on a media error
where it cannot act on a controller that contradicts itself. `fdc_test.c` §14 asserts this
known gap **so that closing it cannot be silent.**

## 4. The wiring

| Item | Status | Notes |
|---|---|---|
| **IRQ6** | ✅ **IMPL** | raised only in response to a command the guest issued, and gated on DOR bit 3 |
| **DMAGATE** as a real gate | ✅ **IMPL** | ⛔ ignoring it is a *silent* kill: transfers complete, the interrupt never reaches the PIC, and nothing reports a wrong value anywhere |
| Dormant by default | ✅ | the master IMR starts `0xFC` (IRQ0+IRQ1). A guest that does not ask sees no change — the property that made the RTC's IRQ8 safe to add |
| **DMA channel 2** | ⛔ **MISS** | the 8237A model exists and channel 2 is idle. ⚠ It is also the channel a real transfer uses, which is why `p_dma.asm` refuses to touch it |
| Motor-off timing | ⛔ **MISS** | the motor bits round-trip; nothing spins down |

## 5. Still open

Tracked in GitHub: [#178](https://github.com/MrMatthewLayton/ntvdmex/issues/178), [#179](https://github.com/MrMatthewLayton/ntvdmex/issues/179) (the list that was here was moved there verbatim, 2026-09-27).

## What to fix, in order

Tracked in GitHub: [#178](https://github.com/MrMatthewLayton/ntvdmex/issues/178) (the list that was here was moved there verbatim, 2026-09-27).

