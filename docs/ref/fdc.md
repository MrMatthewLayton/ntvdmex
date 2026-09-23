# The 82077AA floppy disk controller

**Spec:** Intel 82077AA *CHMOS Single-Chip Floppy Disk Controller* datasheet
(order 290166); NEC µPD765A for the command set it inherits; IBM PC/AT Technical
Reference for the port addresses, the IRQ and the DMA channel.
**Companion:** [`../inventory/fdc.md`](../inventory/fdc.md) — what *we* do about it.

> **The thesis.** Every other chip in this inventory is a **register file**: you write a
> latch, you read it back, and a wrong bit is a wrong bit. The floppy controller is a
> **conversation**. The host and the chip take turns, and whose turn it is lives in two
> bits of one register. Get those two bits wrong and nothing is *wrong* — the two sides
> simply stop being able to speak, and both wait for ever.

---

## 1. The ports, the line and the channel

| | |
|---|---|
| **Primary base** | `3F0h` (secondary `370h`) |
| **Interrupt** | **IRQ6** (vector `0Eh`) |
| **DMA** | **channel 2** — 8-bit, and the reason a floppy transfer needs the 8237A |

⚠ `3F6h` is **not decoded by the FDC**. On a PC/AT it belongs to the hard-disk
controller (the alternate status / device control register). A model that claims the
whole eight-port block takes a register that is not its own.

## 2. The register file

The 82077AA answers in one of two personalities, selected by a pin (IDENT / mode):
**PC/AT mode** and **PS/2 mode**. Both are in the field; the difference is confined to
offsets 0, 1 and 7.

| Offset | Port | Read | Write |
|---|---|---|---|
| 0 | `3F0h` | **SRA** status register A *(PS/2 only)* | — |
| 1 | `3F1h` | **SRB** status register B *(PS/2 only)* | — |
| 2 | `3F2h` | **DOR** digital output | DOR |
| 3 | `3F3h` | **TDR** tape drive | TDR |
| 4 | `3F4h` | **MSR** main status | **DSR** data rate select |
| 5 | `3F5h` | **FIFO** — the data register | FIFO |
| 6 | `3F6h` | *(not the FDC's)* | *(not the FDC's)* |
| 7 | `3F7h` | **DIR** digital input | **CCR** configuration control |

⚠⚠ **Offset 4 and offset 7 are each two different registers depending on direction.**
Read `3F4h` and you get the handshake; write it and you have issued a data-rate change
and possibly a software reset. This is the same shape as the UART's DLAB bank — one
address, two meanings — except here the two are not even related, and there is no bit
to tell them apart. Only the direction of the bus cycle.

⚠ **On a PC/AT-mode part, offsets 0 and 1 are not driven.** They float, and a PC/AT
machine reads them as an empty bus. This is a genuine two-machines case, not a defect:
*"what does `3F0h` read"* has a different right answer on an AT and on a PS/2.

## 3. MSR — the two bits the whole protocol turns on

Read-only at `3F4h`. This is the register a driver polls before **every single byte** in
either direction.

| Bit | Name | Meaning |
|---|---|---|
| 7 | **RQM** | Request for master — the FIFO is ready for a transfer |
| 6 | **DIO** | Direction: **1 = FDC → CPU** (read a result), **0 = CPU → FDC** (write a command) |
| 5 | **NON-DMA** | Execution phase is in non-DMA mode (the host must move every byte) |
| 4 | **CMD BSY** | A command is in progress — set from the first command byte until the last result byte is read |
| 3:0 | **DRV n BUSY** | Drive *n* is in a seek |

The canonical send-a-byte loop, and it is written this way in every BIOS and every
driver, because it is the way the datasheet writes it:

```asm
wait:   in      al, 3F4h
        and     al, 0C0h                ; RQM and DIO only
        cmp     al, 80h                 ; RQM=1, DIO=0 -> it wants a command byte
        jne     wait
        mov     al, cmd
        out     3F5h, al
```

⛔⛔⛔ **`FFh` IS THE WORST POSSIBLE ANSWER HERE, AND IT IS WHAT AN EMPTY BUS GIVES.**
`FFh` is RQM=1 *and* DIO=1: *"I am ready, and I am talking to you."* The loop above
compares `C0h` against `80h`, never matches, and **spins for ever** — the machine does
not fault, does not time out, and does not report anything. It is exactly the shape of
the RTC's UIP bit, where an absent chip read `FFh` and *"poll until UIP clears"* never
cleared. An absent floppy controller that answered `00h` would at least fail fast.

**The idle value is `80h`:** ready, direction host-to-chip, no command in progress.

## 4. The three phases

Every command is up to three phases, and the chip will not skip one:

1. **Command phase.** The host writes the command byte and its parameters, polling
   RQM/DIO before each. `CMD BSY` sets on the first byte.
2. **Execution phase.** Present only on commands that move data or a head. Data moves by
   DMA (channel 2) or, if the SPECIFY command asked for it, by the host reading `3F5h`
   once per byte with `NON-DMA` set in MSR. Ends with **IRQ6**.
3. **Result phase.** DIO flips to 1 and the host reads result bytes from `3F5h`, polling
   before each. `CMD BSY` clears when the **last** result byte is read.

⚠ **A command with result bytes is not finished until they are read.** Abandoning a
command half way leaves the chip mid-conversation: the next byte the next caller writes
is consumed as a parameter of the command already in progress. There is no framing, no
timeout and no resynchronisation short of a reset. *This is why a probe must never
abort mid-command.*

⚠ **Result length is discoverable without knowing the command.** Read `3F5h` while
`RQM=1 && DIO=1`, and stop when `CMD BSY` clears. That drains any command, including an
invalid one, without a table.

## 5. The commands

The opcode is the low 5 bits; bits 7:5 are modifiers (`MT` multi-track, `MFM` density,
`SK` skip deleted).

| Opcode | Command | Params | Results | Moves a head | Raises IRQ6 |
|---|---|---|---|---|---|
| `02h` | READ TRACK | 8 | 7 | yes | yes |
| `03h` | **SPECIFY** | 2 | **0** | no | no |
| `04h` | SENSE DRIVE STATUS | 1 | 1 (ST3) | no | no |
| `05h` | WRITE DATA | 8 | 7 | yes | yes |
| `06h` | READ DATA | 8 | 7 | yes | yes |
| `07h` | RECALIBRATE | 1 | **0** | **yes** | yes |
| `08h` | **SENSE INTERRUPT STATUS** | 0 | 2 (ST0, PCN) | no | no |
| `09h` | ⚠ **WRITE DELETED DATA** | 8 | 7 | yes | yes |
| `0Ah` | READ ID | 1 | 7 | yes (index) | yes |
| `0Ch` | READ DELETED DATA | 8 | 7 | yes | yes |
| `0Dh` | FORMAT TRACK | 5 | 7 | **yes, destructive** | yes |
| `0Eh` | DUMPREG | 0 | 10 | no | no |
| `0Fh` | SEEK | 2 | **0** | **yes** | yes |
| `10h` | **VERSION** | 0 | **1** | no | no |
| `12h` | PERPENDICULAR MODE | 1 | 0 | no | no |
| `13h` | CONFIGURE | 3 | 0 | no | no |
| `14h` | LOCK | 0 | 1 | no | no |
| *other* | **invalid** | — | 1 (`ST0 = 80h`) | no | no |

⚠ **Two easy mistakes in that table, and they are opposite mistakes.** `09h` WRITE DELETED
DATA sits in a gap between `08h` and `0Ah` and no detection routine ever issues it — omit
it and it becomes an "invalid command" that consumes **one** byte, after which its eight
parameters are read as eight more commands. `11h` (SCAN EQUAL) and `18h` (a National
Semiconductor part-ID command) go the other way: they are in the µPD765 and PC8477
literature but **not in the 82077AA**, and a part that answers `90h` to VERSION and then
accepts them is describing a chip that does not exist.

⚠ **A command length table is a framing decision, not a lookup.** Getting one entry wrong
does not produce one wrong answer; it desynchronises everything after it.

★ **`10h` VERSION is the detection command.** No parameters, no execution phase, no
interrupt, no head movement, one result byte, and it leaves nothing behind. An enhanced
82077AA answers **`90h`**; an original µPD765A or 8272A does not recognise the opcode at
all and gives the invalid-command reply **`80h`**. A driver that wants to know whether
it may use CONFIGURE, LOCK or PERPENDICULAR MODE asks this and nothing else.

★ **`08h` SENSE INTERRUPT STATUS is the other half of every interrupt.** The chip raises
IRQ6 and then *keeps the reason* until this is asked; the seek and recalibrate commands
have no result phase at all, so this is the only way to learn they finished. Issued when
nothing is pending, it answers `80h` — invalid — and that is itself the documented way
to discover there is nothing pending.

## 6. The result bytes

**ST0** — bits 7:6 interrupt code (`00` normal, `01` abnormal, `10` invalid command,
`11` ready changed), 5 seek end, 4 equipment check, 3 *(82077: always 0)*, 2 head
address, 1:0 drive select.

**ST1** — 7 end of cylinder, 5 data error (CRC), 4 overrun, 2 no data, 1 not writable,
0 missing address mark.

**ST2** — 6 control mark, 5 data error in data field, 4 wrong cylinder, 1 bad cylinder,
0 missing data address mark.

**ST3** (from SENSE DRIVE STATUS only) — 6 write protected, 5 *(ready, always 1 on the
82077)*, 4 track 0, 2 head address, 1:0 drive select.

After a data command the last four result bytes are **C H R N** — the cylinder, head,
record and size the controller *ended on*, not the ones it was asked for.

## 7. DOR — reset, motors and the interrupt gate

Write-only on a PC/AT, readable on the 82077AA. `3F2h`.

| Bit | Name | Meaning |
|---|---|---|
| 7:4 | **MOT EN 3:0** | Motor enable, one per drive. 1 = spinning |
| 3 | **DMAGATE** | **0 disconnects IRQ6 and the DMA request lines entirely** |
| 2 | **/RESET** | **0 holds the controller in reset.** A driver resets by writing 0 then 1 |
| 1:0 | **DRIVE SEL** | Which drive the chip is addressing |

⚠ **Bit 2 is active-low and it is the software reset every BIOS uses.** Writing `00h`
here — a plausible-looking "turn everything off" — holds the chip in reset, ungates the
interrupt *and* stops the motors, all at once.

⛔ **Leaving DMAGATE clear is a silent kill.** The chip still works; the transfers still
complete; the interrupt simply never reaches the PIC and every operation times out at
the BIOS layer instead. Nothing reports a wrong value anywhere.

## 8. Reset — and the four sense-interrupts

Reset (from DOR bit 2, from DSR bit 7, or from the pin) leaves:

- MSR = `80h`, the FIFO in command phase, `CMD BSY` clear.
- The data rate **preserved** if the reset came from DSR/DOR, so a driver need not
  re-select it.
- **An interrupt pending**, and — unless CONFIGURE has turned drive polling off — the
  chip expects **four** SENSE INTERRUPT STATUS commands, one per drive, each answering
  `ST0 = C0h | drive` (*ready changed*) and a PCN of 0.

⚠ **Four, not one.** A model that raises the interrupt and answers a single sense leaves
three phantom drives *"changed"* in the driver's view and a driver that issued four gets
`80h` for three of them. Both halves of that are visible behaviour.

## 9. Data rate, and the disk-change line

**CCR** (write `3F7h`) and **DSR** (write `3F4h`) both carry the data-rate selector in
their low two bits — `00` 500 kbps, `01` 300, `10` 250, `11` 1 Mbps — and the last write
to either wins. DSR additionally has **bit 7 software reset** and **bit 6 power down**.

**DIR** (read `3F7h`), bit 7 = **DSKCHG**. The line is set by the drive when the door has
been opened since the last access, and it is **cleared only by a successful seek to a
new cylinder** — not by reading DIR. On a PC/AT-mode part **bits 6:0 are not driven**,
so a real AT reads the rest of the byte as bus float.

⛔ **`FFh` here is the second hang.** DSKCHG stuck set means *"the disk has been changed"*
on every access for ever; DOS re-reads the FAT and the directory before every operation
and can conclude the medium is unreadable. Unlike the MSR case this one does not spin —
it just makes the drive appear permanently unreliable, which is harder to attribute.

## 10. What the BIOS keeps at 0040:

The controller's software state is not all in the chip. INT 13h keeps its own at:

| Address | |
|---|---|
| `0040:003E` | recalibrate status — bit per drive, plus bit 7 "an interrupt happened" |
| `0040:003F` | motor status — bit per drive, bits 7 read/write |
| `0040:0040` | motor-off countdown, ticked down by INT 08h |
| `0040:0041` | last operation's status byte |
| `0040:0042`–`0040:0048` | the seven result bytes of the last command |
| `0040:0090`–`0040:0091` | media state per drive |

⚠ A guest that drives the chip directly and a guest that calls INT 13h are using **two
state machines over one chip**, and the BIOS's copy is not invalidated by anything the
first one does.

## Sources

- Intel, *82077AA CHMOS Single-Chip Floppy Disk Controller*, order number 290166 —
  register map, command table, result-byte encodings, reset behaviour, DSR/CCR.
- NEC, *µPD765A/765B Floppy Disk Controller* — the command set the 82077 is compatible
  with, and the three-phase protocol.
- IBM, *Personal Computer AT Technical Reference* — port block, IRQ6, DMA channel 2, and
  the 0040: BIOS data area layout.
- Ralf Brown's Interrupt List, ports section — cross-check on `3F0h`–`3F7h` and on which
  offsets a PC/AT-mode part actually drives.
