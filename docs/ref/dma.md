# The 8237A DMA controller

**Spec:** Intel 8237A Programmable DMA Controller datasheet; IBM PC/AT Technical
Reference for the two-controller wiring and the page registers.
**Companion:** [`../inventory/dma.md`](../inventory/dma.md) — what *we* do about it.

> **The thesis.** The 8237A is a **16-bit device behind an 8-bit port**, and almost
> everything awkward about it follows from that one fact: a single flip-flop decides
> whether the next byte you read or write is the low half or the high half, it is
> **shared between the address and count registers**, and it is **per controller, not
> per channel**. Software that gets it wrong does not fail — it programs a different
> address.

---

## 1. Two controllers, eight channels

| | Controller 1 | Controller 2 |
|---|---|---|
| Channels | 0–3 | 4–7 |
| Ports | `00h`–`0Fh` | `C0h`–`DFh`, **two bytes apart** |
| Transfer width | 8-bit | 16-bit |
| Cascade | — | channel 4 cascades controller 1 |

Channel 4 is not usable: it is how controller 1 reaches the bus.

**On a PC the channels are spoken for:** 0 was DRAM refresh on the XT (the AT moved it
to a timer), **2 is the floppy**, 3 is the hard disk on some machines, and 1, 5, 6, 7 are
the ones a sound card takes.

## 2. Per-channel registers

| Port (ctrl 1) | Register |
|---|---|
| `00h`, `02h`, `04h`, `06h` | base+current **address** for channels 0–3 |
| `01h`, `03h`, `05h`, `07h` | base+current **count** for channels 0–3 |

**Writing sets both the base and the current register; reading returns the current
one.** So a program can read back what it just wrote, and then watch it move.

**The count is "transfers − 1".** A 100-byte block programs 99. Terminal count happens
when the count rolls from 0 to `FFFFh`, which is also why a count of 0 means *one*
transfer, not none.

### The address arithmetic differs per controller, and this is the classic error

| | 8-bit channel (0–3) | 16-bit channel (5–7) |
|---|---|---|
| Physical address | `(page << 16) \| addr` | `((page & 0xFE) << 16) \| (addr << 1)` |
| Address units | bytes | **words** |
| Count units | bytes | **words** |

A 16-bit channel therefore cannot start on an odd address and cannot cross a 128 KB
boundary, where an 8-bit channel is stuck inside 64 KB.

## 3. The page registers — `80h`–`8Fh`

The 8237's own address register is 16 bits, so the high bits come from a separate latch
outside the chip:

| Port | Channel |
|---|---|
| `87h` | 0 |
| `83h` | 1 |
| `81h` | 2 |
| `82h` | 3 |
| `8Bh` | 5 |
| `89h` | 6 |
| `8Ah` | 7 |

⚠ **The gaps in that table are not gaps in the hardware.** `80h`, `84h`–`86h`, `88h`,
`8Ch`–`8Fh` are **real read/write latches** on a PC — the decoder is cheap and does not
bother to leave them out. `80h` doubles as the POST diagnostic port, and code has been
known to use the others as free scratch bytes. A model that answers `FFh` there is
describing an empty bus, not a PC.

## 4. Control registers — controller 1 at `08h`–`0Fh`

| Port | Write | Read |
|---|---|---|
| `08h` | **Command** | **Status** |
| `09h` | Request (software DRQ) | — |
| `0Ah` | Single mask bit | — |
| `0Bh` | **Mode** | — |
| `0Ch` | **Clear byte pointer** (the flip-flop) | — |
| `0Dh` | **Master clear** | Temporary register |
| `0Eh` | Clear mask register | — |
| `0Fh` | Write all mask bits | — |

### The status register — read `08h`

| Bits | Meaning |
|---|---|
| 3:0 | **terminal count reached** on channels 0–3 |
| 7:4 | **request pending** (DRQ asserted) on channels 0–3 |

⚠ **Reading the status register CLEARS the TC bits.** They are latched precisely so a
driver that was busy elsewhere does not miss the end of a block, and they are
destructive to read — so two pieces of code polling status will steal each other's
notifications.

### The mode register — write `0Bh`

| Bits | Field |
|---|---|
| 1:0 | channel this byte programs |
| 3:2 | transfer type: `00` verify, `01` **write** (device → memory), `10` **read** (memory → device) |
| 4 | **auto-initialise** — reload base into current at terminal count |
| 5 | address **decrement** |
| 7:6 | mode: `00` demand, `01` single, `10` block, `11` cascade |

**Auto-initialise is what makes continuous audio possible**: at terminal count the
channel reloads its base address and count and keeps going, so a DOS game's ring buffer
streams for ever without the CPU reprogramming anything.

### Master clear — write `0Dh`

Equivalent to a hardware reset of that controller: the command, status, request and
temporary registers are cleared, the flip-flop is reset, and **the mask register is
SET** — every channel disabled.

⚠⚠ **On a PC that masks the floppy.** Anything that issues a master clear must reprogram
or unmask channel 2 afterwards, or the machine loses its disk.

---

## Sources

- **Intel 8237A datasheet** — the register set, the flip-flop, the mode and command
  encodings, auto-initialise, and the master-clear side effects.
- **IBM PC/AT Technical Reference** — the two cascaded controllers, the port map, the
  page-register assignments, and the channel allocations.
