# The VGA — a technical reference

> **What this is.** What the hardware does, in our own words, derived from the sources
> below. It is written so that someone could implement a VGA from it without having seen
> our code. What *we* currently implement is a different document:
> [`docs/inventory/vga.md`](../inventory/vga.md).
>
> **Sources.** IBM *Personal System/2 Display Adapter* and VGA technical references; the
> FreeVGA register reference; cross-checked against the PCem and Bochs models and against
> a genuine Tseng ET4000/W32p ROM on PCem. See [SOURCES.md](SOURCES.md).
>
> **Confidence.** Register layouts and bit fields are `[FACT]` unless marked. Anything
> that is a *BIOS convention* rather than a hardware behaviour is called out as such —
> the distinction matters, because we implement the hardware and the BIOS separately and
> have historically conflated them.

---

## 1. The idea: a VGA is a pipeline, not a mode number

This is the single most important thing in this document, and it is the thing our
implementation gets wrong.

A VGA does not have modes. It has **five register files and a pipeline**, and a "mode" is
nothing but a set of values a BIOS writes into them. Mode 13h and Mode X are the same
hardware programmed differently; so are Doom's screen and Wolfenstein's.

```
   CPU write ──▶ ┌──────────────────┐
                 │ Graphics         │  rotate, set/reset, ALU vs latches,
                 │ Controller       │  bit mask, write mode
                 └────────┬─────────┘
                          ▼
   CPU address ─▶ ┌──────────────────┐
                 │ Addressing       │  chain-4 / odd-even / planar
                 │ (SR4, GR5, GR6)  │  → (plane, offset)
                 └────────┬─────────┘
                          ▼
                 ┌──────────────────┐
                 │  256 KB video    │  four 64 KB planes, addressed in parallel
                 │  RAM, 4 planes   │
                 └────────┬─────────┘
                          ▼
   CRTC ───────▶ ┌──────────────────┐
   (geometry +   │ Display address  │  start address, row offset, byte/word/dword,
   address gen)  │ generator        │  row scan counter, line compare
                 └────────┬─────────┘
                          ▼
                 ┌──────────────────┐
                 │ Serializer       │  SR1: 8/9 dots, dot clock ÷2, shift modes
                 │ (Sequencer)      │  GR5: shift register mode, 256-colour
                 └────────┬─────────┘
                          ▼
                 ┌──────────────────┐
                 │ Attribute        │  palette, blink, pixel panning, overscan,
                 │ Controller       │  colour select, 8-bit colour
                 └────────┬─────────┘
                          ▼
                 ┌──────────────────┐
                 │ DAC              │  256 × 18-bit palette, pixel mask
                 └────────┬─────────┘
                          ▼   analogue RGB + sync
```

**Two address paths exist and they are independent.** The *CPU* path (Graphics Controller
+ SR4/GR5/GR6) decides which plane a `mov [A000:xxxx]` lands in. The *display* path (CRTC)
decides which byte the CRT beam reads next. A program can reprogram either without the
other. Conflating them is how a renderer ends up correct for one program and wrong for the
next.

---

## 2. Port map

| Port | Access | Register |
|---|---|---|
| `3C2` | W | Miscellaneous Output |
| `3CC` | R | Miscellaneous Output (read-back) |
| `3C2` | R | Input Status 0 |
| `3C3` | R/W | VGA Enable (motherboard) |
| `3C4` / `3C5` | R/W | Sequencer index / data |
| `3CE` / `3CF` | R/W | Graphics Controller index / data |
| `3C0` | W | Attribute index **and** data (alternating — see §7) |
| `3C1` | R | Attribute data read-back |
| `3C6` | R/W | DAC Pixel Mask |
| `3C7` | W / R | DAC Read Index / DAC State |
| `3C8` | R/W | DAC Write Index |
| `3C9` | R/W | DAC Data (three 6-bit writes per entry) |
| `3D4` / `3D5` | R/W | CRTC index / data — **colour** |
| `3B4` / `3B5` | R/W | CRTC index / data — **mono** |
| `3DA` / `3BA` | R | Input Status 1 (also resets the attribute flip-flop) |
| `3DA` / `3BA` | W | Feature Control |
| `3CA` | R | Feature Control (read-back) |

⚠ **The `3Bx` / `3Dx` pair is chosen by Miscellaneous Output bit 0.** A mono-mode guest
uses `3B4/3B5/3BA` and a colour-mode guest `3D4/3D5/3DA` **for the same registers**. Both
sets must be serviced, or mode 7 silently writes into nothing.

---

## 3. External / General registers

### Miscellaneous Output — write `3C2`, read `3CC`

| Bit | Name | Meaning |
|---|---|---|
| 0 | I/OAS | I/O Address Select. `1` → colour (`3Dx`, `3DA`); `0` → mono (`3Bx`, `3BA`) |
| 1 | RAM Enable | `0` disables CPU access to display memory entirely |
| 3:2 | Clock Select | `00` = 25.175 MHz, `01` = 28.322 MHz, `10`/`11` = external / reserved |
| 4 | — | reserved |
| 5 | Odd/Even Page | In odd/even addressing, selects the high or low 64 KB page |
| 6 | HSYNCP | Horizontal sync polarity. `0` = positive |
| 7 | VSYNCP | Vertical sync polarity. `0` = positive |

**The clock select is half of the horizontal geometry.** 25.175 MHz gives 640-pixel-wide
timings; 28.322 MHz gives 720. Nothing else in the register file distinguishes them, so a
host that ignores `3C2` cannot tell a 640-wide mode from a 720-wide one.

**Sync polarity is how the card tells the monitor the vertical size** — a *convention*, not
a hardware effect, but every BIOS follows it and some programs set it directly:

| Vertical size | HSYNCP (b6) | VSYNCP (b7) |
|---|---|---|
| 350 lines | 0 (positive) | 1 (negative) |
| 400 lines | 1 (negative) | 0 (positive) |
| 480 lines | 1 (negative) | 1 (negative) |

⇒ **Mode X (320×240) writes `0xE3`**: colour, RAM enabled, 25.175 MHz, page select high,
both syncs negative = 480-line timing, which is then halved by the CRTC's scan doubling.
A host that never sees `3C2` **cannot distinguish 320×240 from 320×200**.

### Input Status 0 — read `3C2`

| Bit | Name | Meaning |
|---|---|---|
| 4 | SS | Switch Sense — reads back one of the four monitor-ID sense lines |
| 6 | — | CRT interrupt (feature-dependent) |
| 7 | CRT Interrupt | `1` while a vertical retrace interrupt is pending |

⚠ **Measured against MS-DOS 6.22 on real hardware this reads `0x00`.** Our guess agreed;
it is confirmed, not assumed.

### Input Status 1 — read `3DA` / `3BA`

| Bit | Name | Meaning |
|---|---|---|
| 0 | DD | Display Disable — `1` during horizontal *or* vertical blanking |
| 3 | VRetrace | `1` during vertical retrace |
| 5:4 | Diagnostic | Two of the eight attribute-controller output bits, selected by AR12 |

**Reading this port also resets the Attribute Controller's index/data flip-flop** (§7).
That side effect is not optional — it is how every guest resynchronises before programming
the AC, and a host that omits it will see attribute writes land in the wrong register.

Programs poll bit 3 and bit 0 for vsync and for "safe to touch the DAC". Polling loops here
are a *timing* surface: a host that returns a constant will either spin forever or run at
the wrong speed.

### Feature Control — write `3DA`/`3BA`, read `3CA`

Two feature-connector control bits. No standard VGA function depends on them; they are
read/write storage from software's point of view. **[VERIFY]** whether any guest reads
them back expecting what it wrote.

### VGA Enable — `3C3`

Bit 0 enables the VGA's I/O and memory decode at the motherboard level. `0` makes the card
disappear from the bus.

---

## 4. Sequencer — index `3C4`, data `3C5`

| Reg | Name | Bits |
|---|---|---|
| SR0 | Reset | 0 = synchronous reset (active low), 1 = asynchronous reset (active low) |
| SR1 | Clocking Mode | see below |
| SR2 | Map Mask | 3:0 — enable writes to planes 3..0 |
| SR3 | Character Map Select | selects font maps A and B out of eight |
| SR4 | Memory Mode | see below |

### SR1 — Clocking Mode

| Bit | Name | Meaning |
|---|---|---|
| 0 | 8/9 Dot Mode | `1` = 8 dots per character, `0` = 9. Text modes use 9; graphics use 8 |
| 2 | Shift Load | Load the shift registers every *other* character clock |
| 3 | Dot Clock ÷2 | **`1` halves the dot clock** — this is what makes 320-wide modes 320 wide |
| 4 | Shift Four | Load the shift registers every *fourth* character clock (256-colour) |
| 5 | Screen Off | `1` blanks the display and gives the CPU full memory bandwidth |

⚠ **Bit 5 is a real behaviour, not a hint.** Programs blank the screen with it to load a
palette or a whole frame without snow, then clear it. A host that ignores it shows the
tearing the program was carefully avoiding; a host that ignores the *clear* shows nothing
at all.

### SR4 — Memory Mode

| Bit | Name | Meaning |
|---|---|---|
| 1 | Extended Memory | `1` = the full 256 KB is addressable (always set on a real VGA) |
| 2 | Odd/Even Disable | `1` = linear/planar addressing; `0` = **odd/even** (§8) |
| 3 | Chain 4 | `1` = **chain-4** addressing (§8). This is what makes mode 13h linear |

**These two bits choose between three completely different memory layouts.** Modelling
bit 3 and dropping bit 2 — which is what we do today — means odd/even and planar are
indistinguishable to the host.

---

## 5. CRTC — index `3D4`/`3B4`, data `3D5`/`3B5`

Registers `0x00`–`0x18`. This file does two jobs that are worth separating in an
implementation: **timing/geometry** (§5.1) and **display address generation** (§5.2).

⚠ **CR11 bit 7 write-protects CR00–CR07.** While it is set, writes to those seven
registers are *discarded by the hardware*. Measured on real hardware; we currently latch
CR11 and ignore the protection, so a guest that sets the bit and then writes garbage —
which some do deliberately, trusting the protection — corrupts our geometry.

### 5.1 Timing and geometry

| Reg | Name | Notes |
|---|---|---|
| `00` | Horizontal Total | Total character clocks per scanline, minus 5 |
| `01` | End Horizontal Display | Last displayed character clock |
| `02` | Start Horizontal Blanking | |
| `03` | End Horizontal Blanking | 4:0 = end (low 5 bits); 6:5 = display enable skew; **7 = EVRA** |
| `04` | Start Horizontal Retrace | |
| `05` | End Horizontal Retrace | 4:0 = end; 6:5 = retrace skew; **7 = bit 5 of End Horizontal Blanking** |
| `06` | Vertical Total | Low 8 bits; bits 8–9 in CR07 |
| `07` | Overflow | b0 VT8 · b1 VDE8 · b2 VRS8 · b3 VBS8 · b4 LC8 · b5 VT9 · b6 VDE9 · b7 VRS9 |
| `09` | Maximum Scan Line | 4:0 = max scan line (character height − 1) · b5 = VBS9 · b6 = LC9 · **b7 = Scan Doubling** |
| `10` | Vertical Retrace Start | Low 8 bits |
| `11` | Vertical Retrace End | 3:0 = end · b5 = disable vertical interrupt · b6 = 5 refresh cycles · **b7 = CR00–CR07 write protect** |
| `12` | Vertical Display End | Low 8 bits |
| `15` | Start Vertical Blanking | Low 8 bits |
| `16` | End Vertical Blanking | |
| `18` | Line Compare | Low 8 bits; b8 in CR07.4, b9 in CR09.6 — split-screen |

**Deriving the visible geometry** (this is what step 3 of the inventory plan needs):

```
dot_clock  = MiscOut[3:2] == 0 ? 25.175 MHz : 28.322 MHz
dots_per_char = SR1.0 ? 8 : 9
if (SR1.3) dot_clock /= 2                      # 320-wide modes

width_px   = (CR01 + 1) * dots_per_char

vde        = CR12 | (CR07.1 << 8) | (CR07.6 << 9)
height_px  = vde + 1
if (CR09.7)  height_px /= 2                    # scan doubling: 400→200, 480→240
if (CR17.2)  height_px /= 2                    # vertical total double
char_height = (CR09[4:0]) + 1                  # text rows = height_px / char_height
```

⇒ **320×240 Mode X falls straight out of this**: MiscOut `0xE3` selects 480-line sync,
`CR12`+overflow give 479, `CR09.7` halves it to 240, `SR1.3` halves the dot clock to give
320. Every value the mode needs is in the register file. None of it needs a mode number.

### 5.2 The display address generator

This is the half we drop entirely today, and it is the half Doom programs.

| Reg | Name | Notes |
|---|---|---|
| `0C`/`0D` | Start Address High / Low | The memory address the first displayed pixel comes from — **this is the page-flip register** |
| `08` | Preset Row Scan | 4:0 = initial row scan (smooth vertical scroll); 6:5 = byte panning |
| `13` | Offset | **Logical line width**, in words or dwords — how far the address advances per scanline |
| `14` | Underline Location | 4:0 = underline row · b5 = Count By 4 · **b6 = DWord mode** |
| `17` | CRTC Mode Control | see below |
| `18` | Line Compare | Scanline at which the address generator resets to 0 — split screen |

**CR17 — CRTC Mode Control:**

| Bit | Name | Meaning |
|---|---|---|
| 0 | CMS0 | Compatibility Mode Support — substitutes row-scan bit 0 for MA13 (CGA-style interleave) |
| 1 | SRC | Select Row Scan Counter — substitutes row-scan bit 1 for MA14 (Hercules-style) |
| 2 | Vertical Total Double | Divide the scanline clock by 2 |
| 3 | Count By Two | Advance the address every *other* character clock |
| 5 | Address Wrap | `0` wraps at MA13 (CGA compatibility); `1` at MA15 |
| 6 | **Word/Byte Mode** | `0` = word mode (address shifted left 1); `1` = **byte mode** |
| 7 | Sync Enable | `0` holds the CRTC in reset — no sync output |

**The address a scanline starts from:**

```
addr = start_address + row * row_pitch          # start_address = (CR0C<<8)|CR0D
row_pitch = CR13 * (CR14.6 ? 4 : (CR17.6 ? 1 : 2))
```

then, per character clock, the generator advances and applies the byte/word/dword shift
and any CMS/SRC substitution. **Byte mode (`CR17.6 = 1`) is what an unchained 256-colour
mode needs**, because in word mode the hardware would shift the address and halve the
effective line length.

⇒ **Doom writes exactly `CR14`, `CR17` and `GR06` once each, at startup.** Those three
are the address generator. Skyroads writes none of them. Both are "mode 13h". That
measurement is the whole argument for this document.

---

## 6. Graphics Controller — index `3CE`, data `3CF`

This is the CPU **write** path. Every `mov [A000:xxxx], al` passes through it.

| Reg | Name | Bits |
|---|---|---|
| `00` | Set/Reset | 3:0 — the colour supplied to planes 3..0 |
| `01` | Enable Set/Reset | 3:0 — per plane, use Set/Reset instead of CPU data |
| `02` | Color Compare | 3:0 — the colour read mode 1 compares against |
| `03` | Data Rotate | 2:0 = rotate right count; **4:3 = ALU function** (00 none, 01 AND, 10 OR, 11 XOR) |
| `04` | Read Map Select | 1:0 — which plane read mode 0 returns |
| `05` | Graphics Mode | see below |
| `06` | Miscellaneous | see below |
| `07` | Color Don't Care | 3:0 — planes excluded from the read-mode-1 comparison |
| `08` | **Bit Mask** | Per-bit: `1` = take the new value, `0` = take the latch |

### GR5 — Graphics Mode

| Bit | Name | Meaning |
|---|---|---|
| 1:0 | Write Mode | 0–3, see below |
| 3 | Read Mode | `0` = read a plane; `1` = colour compare |
| 4 | Host Odd/Even | `1` = odd/even CPU addressing |
| 5 | Shift Register | `1` = CGA-style interleaved two-bit shift |
| 6 | 256-Color Mode | `1` = Shift 256 — four bits per pixel from all four planes |

### GR6 — Miscellaneous Graphics

| Bit | Name | Meaning |
|---|---|---|
| 0 | Alphanumeric Disable | `1` = graphics mode |
| 1 | Chain Odd/Even | `1` = A0 selects the plane pair |
| 3:2 | Memory Map Select | `00` = `A0000`–`BFFFF` (128 K) · `01` = `A0000`–`AFFFF` (64 K) · `10` = `B0000`–`B7FFF` (32 K) · `11` = `B8000`–`BFFFF` (32 K) |

**The memory map select is not cosmetic.** It decides where the aperture *is*. A guest that
sets `11` and writes to `B8000` expects those writes to reach video memory.

### The four write modes

**The latches.** Any CPU *read* of video memory loads four 8-bit latches, one per plane,
regardless of read mode. Those latches are the other operand of every write. This is the
mechanism behind fast planar blits: read a byte (loading 32 bits), write a byte (storing
all 32).

**Write Mode 0** — the general case:
1. Rotate the CPU byte right by `GR3[2:0]`.
2. For each plane: if `GR1` enables set/reset for it, replace the byte with `GR0`'s bit for
   that plane expanded to 8 bits; otherwise use the rotated CPU byte.
3. Apply the ALU function `GR3[4:3]` against that plane's latch.
4. For each bit: `GR8` set → result; `GR8` clear → **latch**.
5. Store to the planes enabled by `SR2` (Map Mask).

**Write Mode 1** — copy the latches straight to memory. The CPU byte is ignored entirely;
`SR2` still gates which planes are written. This is the second half of a planar blit.

**Write Mode 2** — the CPU byte's low four bits are a *colour*. Each plane gets that
plane's bit expanded to 8 bits, then ALU, then bit mask, then map mask. No rotate.

**Write Mode 3** — the CPU byte is rotated and then **ANDed with `GR8`** to form the
effective bit mask; the colour comes from `GR0` (Set/Reset) as though set/reset were
enabled for all planes. Used for fast text/pattern drawing.

### The two read modes

**Read Mode 0** — return the byte at that offset from the plane in `GR4`.

**Read Mode 1** — *colour compare*. Return a byte whose bit *n* is 1 where the pixel at
bit *n* matches `GR2`, considering only the planes not masked out by `GR7`.

⚠ **Read mode 1 is how a program reads back a planar screen**, and Doom's `I_ReadScreen`
is in this class. A host that implements only read mode 0 will return plausible bytes and
the program will compute something wrong from them.

---

## 7. Attribute Controller — `3C0` / `3C1`

**The flip-flop.** `3C0` is index *and* data on the same port, alternating. Reading
`3DA`/`3BA` resets it to "expect index". A guest that loses sync writes its palette into
its mode-control register. Every guest therefore reads `3DA` first — which means a host
that does not implement the reset side effect will corrupt attribute programming in a way
that looks like a palette bug.

Writing an index also carries bit 5, **Palette Address Source**: `0` while loading the
palette (display off), `1` to re-enable the display. Leaving it clear leaves a black screen.

| Reg | Name | Notes |
|---|---|---|
| `00`–`0F` | Palette | 6 bits each: the 16 text/EGA attributes → DAC entries |
| `10` | Mode Control | see below |
| `11` | Overscan Color | The border colour |
| `12` | Color Plane Enable | 3:0 — planes that take part in the display; 5:4 select which two bits appear in Input Status 1 |
| `13` | Horizontal Pixel Panning | 3:0 — shift the display 0–8 pixels left, for smooth scrolling |
| `14` | Color Select | 1:0 → DAC bits 5:4 · 3:2 → DAC bits 7:6, when AR10.7 is set |

### AR10 — Mode Control

| Bit | Name | Meaning |
|---|---|---|
| 0 | Graphics/Alpha | `1` = graphics |
| 1 | Mono Emulation | |
| 2 | Line Graphics Enable | Replicate the 8th dot into the 9th for box-drawing characters (`0xC0`–`0xDF`) |
| 3 | **Blink Enable** | `1` = attribute bit 7 means blink; `0` = it means intense background |
| 5 | Pixel Panning Mode | |
| 6 | 8-bit Color Enable | `1` = 256-colour: two 4-bit pixels combine into one 8-bit index |
| 7 | Palette Bits 5-4 Select | `1` = take DAC bits 5:4 from AR14 instead of the palette register |

⚠ **Bit 3 is the single source of a whole class of "text is wrong" bugs** — it is why
QBasic's labels lost letters. Bit 2 is why box-drawing characters have gaps if unmodelled.

---

## 8. Memory addressing — how a CPU address becomes (plane, offset)

**This is where our open defect lives**, so it is worth stating exactly.

The VGA has 256 KB as **four parallel 64 KB planes**. Three addressing schemes decide how
the CPU's linear offset into the aperture maps onto them.

### Chain-4 — `SR4.3 = 1`

```
plane  = A & 3
offset = A >> 2
```

Consecutive CPU addresses walk across the planes. 64 K of aperture reaches all 256 K, and
one byte is one pixel: this is **mode 13h**. The cost is that only ¼ of memory is reachable
per plane, so there is no room for a second page — which is exactly why Doom turns it off.

### Odd/Even — `SR4.2 = 0` (with `GR5.4` / `GR6.1`)

```
plane  = (A & 1) | (MiscOut.5 ? 2 : 0)
offset = A & ~1
```

Even addresses → plane 0/2, odd → plane 1/3. This is how **text modes** put a character in
one plane and its attribute in the next, and how CGA-compatible graphics modes work.

### Planar (unchained) — `SR4.2 = 1`, `SR4.3 = 0`

```
offset = A                       # the same offset in every plane
plane  = writes: every plane enabled by SR2 (Map Mask)
         reads:  the plane in GR4 (read mode 0), or all of them (read mode 1)
```

**One CPU write can therefore land in one, two, three or four planes at once**, and this is
the normal case, not an edge case. Mode 12h uses it for 16 colours. Mode X / mode Y use it
for 256 colours with four times the memory and free page flipping.

> ⛔ **This is the defect.** Our implementation maps the `A0000` window to **one plane at a
> time** and handles multi-plane masks by writing to a scratch buffer and fanning out later
> by diffing against a seed. Doom's low-detail drawer is dominated by **two-plane** masks
> (`0x03` and `0x0c`, ~143,000 writes each, against ~16,900 for any single-plane mask), so
> the approximation is load-bearing exactly where the picture is wrong. No seed policy can
> fix it: over-fanning writes wrong data, under-fanning leaves stale data, and because
> doubled pixels are the *same colour*, a diff cannot tell "written again with the same
> value" from "not written".
>
> **The fix is to route a store through the map mask to every selected plane at write
> time** — which is what this section describes and what the hardware does.

---

## 9. The DAC

| Port | Function |
|---|---|
| `3C6` | Pixel Mask — ANDed with every pixel index before lookup. Defaults to `0xFF` |
| `3C8` | Write Index — set, then write three bytes to `3C9` (R, G, B) |
| `3C7` (W) | Read Index — set, then read three bytes from `3C9` |
| `3C7` (R) | DAC State — bits 1:0: `00` = last access was a write, `11` = a read |
| `3C9` | Data — **6 bits per component**, so the top two bits read back as 0 |

**256 entries × 18 bits.** Two traps:

- **`3C6` is not always `0xFF`.** Programs use it for fades and for split-palette effects.
  A famous copy-protection idiom writes `0x00` here to blank the screen. ⚠ Our value and a
  real card's disagreed in the s75 probe and the question is still open — it needs PCem.
- **The 6-bit truncation is observable.** A program that writes `0x3F` and reads back `0xFF`
  knows it is not talking to a VGA.

---

## 10. Where a BIOS fits, and why it is a separate surface

Everything above is **hardware**. `INT 10h` is **firmware** — a program that writes these
registers on the guest's behalf, plus a font, plus the BIOS Data Area at `0040:`.

The distinction is not pedantry. Measured against genuine MS-DOS 6.22 on real hardware, a
BIOS mode set leaves **~60 values across the register files**; ours leaves about **five**.
A guest that sets a mode through the BIOS and then *reads a register back* — to learn the
geometry, or to save and restore the card around its own mode switch — is told zero by us
and the truth by a real machine.

So there are two obligations, not one:

1. **The hardware** must behave as this document describes when written directly.
2. **The BIOS** must leave behind what a real BIOS leaves behind, for every mode it
   supports — which is a table of measured values, not a guess.

⚠ **Read-back fidelity is a guest-visible behaviour change.** Returning real values where
we returned zero changes what the guest computes, and its logic is not ours to predict.
Treat it with the same care as a rendering change: one commit, one by-hand check.

---

## 11. What to implement, in order

Derived from §1: build the pipeline, not the modes.

1. **Capture every register.** ✅ Done — the full file is latched with per-index write
   counts, and dumped on both exit paths.
2. **Measure what a real BIOS leaves.** ✅ Done for five modes via `tools/dostest/p_vgareg.asm`;
   the remaining modes need PCem.
3. **Derive geometry** from Miscellaneous Output + CRTC + SR1, per §5.1 — replacing the
   mode-number table.
4. **Derive addressing** from SR4/GR5/GR6 and route writes through the map mask at write
   time, per §8 — replacing `chain4`, the mode-Y snapshot and `mkind` with one address
   generator. **This is where Doom's low detail is fixed.**
5. **Retire the heuristics** once 3 and 4 carry the load.

Coverage against this document, marked from the code, is
[`docs/inventory/vga.md`](../inventory/vga.md).
