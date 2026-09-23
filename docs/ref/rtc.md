# The MC146818 real-time clock and CMOS RAM

**Spec:** Motorola MC146818 datasheet; IBM PC/AT Technical Reference for the CMOS
memory map (which is a *BIOS convention*, not part of the chip).
**Companion:** [`../inventory/rtc.md`](../inventory/rtc.md) — what *we* do about it.

> **The thesis.** One chip does two unrelated jobs: it is **a clock** and it is **64
> bytes of battery-backed RAM**, and software reaches both through the same two ports.
> A third job hides in the index register — **bit 7 is the NMI mask** — so code that
> writes an index without thinking about that bit is enabling or disabling NMI as a side
> effect, every time.

---

## 1. Two ports

| Port | Purpose |
|---|---|
| `70h` | **index**, write-only. Bits 6:0 select the register; **bit 7 masks NMI** |
| `71h` | data, read/write |

⚠ **Port `70h` is write-only and reads are undefined**; software must not assume it can
read back the index it set.

⚠ **Every access is two instructions and they are not atomic.** An interrupt between the
index write and the data access, whose handler also touches the CMOS, leaves the first
access reading the *second* one's register. Careful code masks interrupts across the
pair; a lot of code does not.

## 2. The clock registers — `00h`–`0Dh`

| Reg | Contents |
|---|---|
| `00h` | seconds |
| `01h` | seconds **alarm** |
| `02h` | minutes |
| `03h` | minutes alarm |
| `04h` | hours |
| `05h` | hours alarm |
| `06h` | day of week (1 = Sunday) |
| `07h` | day of month |
| `08h` | month |
| `09h` | year (00–99) |

The century is **not** in the chip; the BIOS keeps it in CMOS RAM, conventionally at
`32h`.

### Status Register A — `0Ah`

| Bits | Field |
|---|---|
| 7 | **UIP** — update in progress |
| 6:4 | divider / oscillator control (`010` = the 32.768 kHz normal setting) |
| 3:0 | periodic-interrupt rate select |

⚠⚠ **UIP is the bit every correct reader waits on.** The chip updates its time registers
once a second and they must not be read during that window, so the canonical sequence is
*"poll `0Ah` until UIP is clear, then read the time"*. A model that answers `0Ah` with a
byte whose bit 7 is **set** turns that loop into a **hang**; one that answers `0x00`
lets it through instantly, which is the safe direction but is not the same as being
right.

### Status Register B — `0Bh`

| Bit | Field |
|---|---|
| 7 | **SET** — halt updates so the time can be written |
| 6 | PIE — periodic interrupt enable |
| 5 | AIE — alarm interrupt enable |
| 4 | UIE — update-ended interrupt enable |
| 3 | SQWE — square wave enable |
| 2 | **DM** — data mode: `0` = **BCD**, `1` = binary |
| 1 | **24/12** — `1` = 24-hour |
| 0 | DSE — daylight saving enable |

**A PC BIOS leaves `DM = 0` and `24/12 = 1`: BCD, 24-hour.** So `0x12` in the hours
register is twelve o'clock, not eighteen. Every DOS program that reads the chip directly
assumes this, and the BIOS's own INT 1Ah `AH=02h`/`04h` hands the same BCD straight
through.

### Status Register C — `0Ch`, and the trap in it

| Bit | Field |
|---|---|
| 7 | IRQF — an interrupt is pending |
| 6 | PF — periodic |
| 5 | AF — alarm |
| 4 | UF — update ended |

⚠⚠ **Reading `0Ch` CLEARS it**, and that is how IRQ8 is acknowledged at the chip. Two
consequences software depends on:

- An IRQ8 handler that does **not** read `0Ch` gets exactly one interrupt and then
  silence, because the chip will not re-assert while a flag is set.
- Two pieces of code both reading `0Ch` steal each other's flags.

### Status Register D — `0Dh`

Bit 7 is **VRT**, *valid RAM and time*: `1` means the battery has held. **`0` means "the
CMOS is garbage"**, and firmware and setup programs act on it.

## 3. CMOS RAM — `0Eh`–`7Fh`

The chip does not care what is in these; the **BIOS** does. The conventional map:

| Reg | Contents |
|---|---|
| `0Eh` | POST diagnostic status |
| `0Fh` | **shutdown status** — how to behave after the next reset; `0Ah` is the 286/386 "return to real mode" trick |
| `10h` | floppy drive types, one nibble each |
| `12h` | hard disk types |
| `14h` | **equipment byte** — floppies fitted, video type, coprocessor |
| `15h`–`16h` | base memory, KB |
| `17h`–`18h` | extended memory, KB |
| `2Eh`–`2Fh` | **checksum** over `10h`–`2Dh` |
| `30h`–`31h` | extended memory again (as POST found it) |
| `32h` | **century**, BCD |

⚠ **The checksum is load-bearing.** A setup program that writes a byte in `10h`–`2Dh`
and does not fix `2Eh`/`2Fh` makes the BIOS declare the CMOS invalid at the next boot.

## 4. IRQ8

The chip raises **IRQ8** — the slave PIC's first line — for three separate reasons, each
with its own enable in `0Bh` and its own flag in `0Ch`: **periodic** (at the rate in
`0Ah` bits 3:0, up to 8 kHz), **alarm** (when the time matches registers `01`/`03`/`05`),
and **update ended**.

The periodic interrupt is what Windows and DOS extenders use for a fast, steady tick that
is independent of whatever a game has done to the 8254 — which is exactly why it matters
to a VDM: **a guest that reprograms the PIT has not touched this one.**

---

## Sources

- **Motorola MC146818 datasheet** — the register file, UIP, the status registers, the
  three interrupt sources, and BCD/binary mode.
- **IBM PC/AT Technical Reference** — the CMOS memory map, the checksum range, and the
  NMI mask in the index register.
