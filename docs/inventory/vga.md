# Inventory — VGA / CRT

**Spec:** IBM *Personal System/2 Display Adapter* and *VGA* technical references; the
standard VGA register set as documented in FreeVGA / the Bochs & PCem models.
**Oracle:** PCem with a real Tseng ET4000/W32p ROM.
**Our implementation:** `src/vdd/vdd_video.c` (3051 lines), `src/vdd/vdd_video.h`.
**Measured:** 2026-09-22, from the code, with citations. Re-measure after any change.

---

## Headline

**41 of 71 registers are fully modelled (58%), 5 are partial, 25 are absent.**
(Counted from the tables below: external 1/0/6, Sequencer 1/1/3, CRTC 12/1/12,
Graphics Controller 7/1/1, Attribute Controller 18/1/2, DAC 2/1/1.) But the number is
not the finding. The finding is *which* ones are missing:

> **We do not model a single register that describes horizontal geometry, address
> generation, or panning.** CRTC `0x00`–`0x05`, `0x08`, `0x14`, `0x17`, the Attribute
> Controller's `0x13`, the Sequencer's `SR1`, the Graphics Controller's `GR6` and the
> Miscellaneous Output register are all absent. Those are precisely the registers that
> make one mode 13h different from another.

So the picture we produce is not derived from what the guest programmed. It is derived
from a **mode number** plus a handful of special cases bolted on where a guest failed.
`vdd_video.h:234` says so in its own words about mode Y: *"snapshot instead … Exact for
a program that fills a whole plane before switching … and approximate for one that
interleaves planes mid-scan. Documented as an approximation because it is one."*

---

## 1. External / General registers

| Port | R/W | Register | Status | Evidence |
|---|---|---|---|---|
| 3C2 | W | **Miscellaneous Output** | **MISS** | not claimed — `vdd_video.c:3036-3048` |
| 3CC | R | Miscellaneous Output (read) | **MISS** | not claimed |
| 3C2 | R | Input Status 0 | **MISS** | not claimed |
| 3C3 | R/W | VGA Enable | **MISS** | not claimed |
| 3?A | R | Input Status 1 | **IMPL** | `status_in`, `vdd_video.c:2420` |
| 3?A | W | Feature Control | **MISS** | `status_out` is a stub |
| 3CA | R | Feature Control (read) | **MISS** | not claimed |

**Misc Output is the worst of these.** Bits 2–3 select the dot clock (25 MHz vs 28 MHz →
640 vs 720 pixels), bit 0 switches the CRTC between 3Bx and 3Dx, and **bits 6–7 are the
sync polarities, which is how a VGA encodes 400- vs 350- vs 480-line vertical size**. A
guest setting Mode X writes `0xE3` here. We never see it.

⚠ The mono aliases `3B4/3B5/3BA` are also unclaimed; `vdd_video.c:428` *computes*
`0x3B4` for mode 7 but no handler is registered for it.

## 2. Sequencer — 3C4 index / 3C5 data

| Reg | Name | Status | Evidence |
|---|---|---|---|
| SR0 | Reset | **MISS** | falls through `seq_set_data` |
| SR1 | **Clocking Mode** | **MISS** | — |
| SR2 | Map Mask | **IMPL** | `seq_set_data`, index 2 |
| SR3 | **Character Map Select** | **MISS** | — |
| SR4 | Memory Mode | **PART** | bit 3 (Chain-4) only; bits 1–2 dropped |

**SR1** carries bit 3 *Dot Clock ÷2* (the 320-wide modes), bit 0 *8/9 dot characters*
(the 720-pixel text mode) and bit 5 *Screen Off* — which guests use to hide a mode
change, and which we would render as a normal frame.
**SR3** selects between the character generator banks: a program that loads a second
font and flips to it gets the first. **SR4 bit 2** is Odd/Even disable — half of how
text mode and the CGA modes address memory, which we special-case by `mkind` instead.

## 3. CRTC — 3D4 index / 3D5 data

| Reg | Name | Status | Evidence |
|---|---|---|---|
| 0x00 | Horizontal Total | **MISS** | `crtc_set_data` default, `vdd_video.c:2137` |
| 0x01 | End Horizontal Display | **MISS** | — |
| 0x02 | Start Horizontal Blanking | **MISS** | — |
| 0x03 | End Horizontal Blanking | **MISS** | — |
| 0x04 | Start Horizontal Retrace | **MISS** | — |
| 0x05 | End Horizontal Retrace | **MISS** | — |
| 0x06 | Vertical Total | **IMPL** | `crtc_vtotal_lo` |
| 0x07 | Overflow | **IMPL** | `crtc_overflow` |
| 0x08 | **Preset Row Scan** | **MISS** | — |
| 0x09 | Maximum Scan Line | **PART** | **bit 6 only** (line-compare bit 9); bits 0–4 char height and **bit 7 scan doubling dropped** |
| 0x0A | Cursor Start | **IMPL** | `cur_shape` |
| 0x0B | Cursor End | **IMPL** | `cur_shape` |
| 0x0C | Start Address High | **IMPL** | `crtc_start`, two-write tear guard |
| 0x0D | Start Address Low | **IMPL** | `crtc_start` |
| 0x0E | Cursor Location High | **IMPL** | `crtc_cursor` |
| 0x0F | Cursor Location Low | **IMPL** | `crtc_cursor` |
| 0x10 | Vertical Retrace Start | **MISS** | — |
| 0x11 | Vertical Retrace End (**bit 7 = write-protect CR0–CR7**) | **MISS** | — |
| 0x12 | Vertical Display End | **IMPL** | `crtc_vde_lo` |
| 0x13 | Offset (logical line width) | **IMPL** | `crtc_offset` |
| 0x14 | **Underline Location** (bit 5 Count-by-4, bit 6 **Doubleword mode**) | **MISS** | — |
| 0x15 | Start Vertical Blanking | **IMPL** | `crtc_vbs_lo` |
| 0x16 | End Vertical Blanking | **MISS** | — |
| 0x17 | **CRTC Mode Control** | **MISS** | — |
| 0x18 | Line Compare | **IMPL** | `crtc_lc_low` + overflow + maxscan |

**CR17 is the most important missing register in this document.** Bit 6 selects
byte/word addressing, bit 3 Count-by-Two, bit 5 Address Wrap, bits 0–1 the CMS row
scan. Together with CR14 bit 6 (Doubleword) it *is* the address generator — the thing
that makes chained 13h, unchained mode Y and Mode X different from one another. We
infer that difference from `SR4` bit 3 alone and then approximate the rest.
**CR11 bit 7** write-protects CR0–CR7: we accept writes real hardware would refuse.
**CR09 bit 7** is scan doubling — 320×200 displayed as 400 lines, and half of how
320×240 Mode X is built.

## 4. Graphics Controller — 3CE index / 3CF data

| Reg | Name | Status | Evidence |
|---|---|---|---|
| GR0 | Set/Reset | **IMPL** | `gc_set_data`, `vdd_video.c:2296` |
| GR1 | Enable Set/Reset | **IMPL** | — |
| GR2 | Color Compare | **IMPL** | added s7x for Lemmings |
| GR3 | Data Rotate / Function | **IMPL** | `func_rotate` |
| GR4 | Read Map Select | **IMPL** | `read_map` |
| GR5 | Graphics Mode | **PART** | bits 0–1 write mode, bit 3 read mode. **bit 4 Odd/Even, bit 5 Shift Register, bit 6 256-Color Shift all dropped** |
| GR6 | **Miscellaneous** (Graphics/Alpha, Chain Odd/Even, **Memory Map Select**) | **MISS** | — |
| GR7 | Color Don't Care | **IMPL** | — |
| GR8 | Bit Mask | **IMPL** | `bit_mask` |

**GR6 bits 2–3 choose the memory aperture** — A0000/128K, A0000/64K, B0000/32K or
B8000/32K. We hard-map `A0000` + `B8000` (`vdd_video.h:19-22`), so a guest that
relocates its window writes into a region we are not watching. **GR5 bit 5** is the
CGA-interleave shift and **bit 6** the 256-colour shift: the packed-pixel formatter.

## 5. Attribute Controller — 3C0 (index+data) / 3C1 (read)

| Reg | Name | Status | Evidence |
|---|---|---|---|
| 0x00–0x0F | Palette | **IMPL** | `attr_out`, `vdd_video.c:279` |
| 0x10 | Mode Control | **PART** | stored in `attr_mode`; **bit 6 (8-bit colour), bit 5 (pixel-pan compat), bit 3 (blink) consumed only indirectly** |
| 0x11 | Overscan Colour | **IMPL** | `vpal[16]` |
| 0x12 | **Colour Plane Enable** | **MISS** | `default: break;  /* 12h plane enable, 13h pan */` — `vdd_video.c:312` |
| 0x13 | **Horizontal Pixel Panning** | **MISS** | same line |
| 0x14 | Colour Select | **IMPL** | `attr_cse` |

The comment at `vdd_video.c:312` names both missing registers and drops them. **AR13 is
the horizontal smooth-scroll register** — with CR08 (Preset Row Scan) for the vertical,
it is how a scrolling game moves the screen by less than a character cell. Neither
exists here, so any guest that smooth-scrolls can only judder in 8-pixel steps.

## 6. DAC

| Port | Register | Status | Evidence |
|---|---|---|---|
| 3C6 | **Pixel Mask** | **MISS** | not claimed — reads return the bus default |
| 3C7 | W: read index / R: DAC state | **PART** | write index only, `dac_out`, `vdd_video.c:1658` |
| 3C8 | Write index | **IMPL** | — |
| 3C9 | Data (3× 6-bit) | **IMPL** | `dac_comp`/`dac_latch` |

**3C6 defaults to 0xFF and is ANDed with every pixel** on its way to the DAC. Games use
it for palette-independent fades and for hiding a redraw. Unclaimed, a guest's write is
discarded and the fade does nothing.

---

## What the missing registers would buy, by guest

| Guest / symptom | Register(s) it turns on | Today |
|---|---|---|
| **Doom** unchained 13h page flip | SR4.3 ✅, CR17, CR14.6, CR09 | works via the mode-Y snapshot **heuristic** (`vdd_video.h:234`) |
| **Mode X** 320×240 (open: Mario artefacts) | Misc Out `0xE3`, CR09.7, CR06/07/10/11/12/15/16 | we cannot even tell 320×240 from 320×200 |
| Smooth horizontal scroll | **AR13**, CR08, AR10.5 | 8-pixel steps only |
| Fades / redraw hiding | **3C6 Pixel Mask**, SR1.5 Screen Off | writes discarded |
| Second font / 512-char text | **SR3** | first font always |
| A guest that relocates its aperture | **GR6.2-3** | writes land outside our window |
| 360×480, 720-pixel text, tweaked widths | **Misc Out.2-3, SR1.0, CR00–CR05** | invisible |
| Plane-masked display effects | **AR12** | dropped at `vdd_video.c:312` |

---

## Measured: what our guests actually program (step 1, 2026-09-22)

The capture layer landed, and the first two guests settle the argument. Both run mode
13h. `written-by-guest` counts come from `STAGE2: VGAREG` in each run's log.

| | Sequencer | CRTC | Graphics Controller | AC | Misc Out |
|---|---|---|---|---|---|
| **Doom** | `02`×1,101,409 `04`×1 | `0C`×986 **`14`×1 `17`×1** | `04`×3,534 `05`×221 **`06`×1** | none | never |
| **Skyroads** | none | none | none | none | never |

**Doom programs CR14, CR17 and GR6 exactly once each — and we drop all three.** Those
are precisely the three registers this document named as the address generator before
the measurement existed. Doom writes them at mode-set time to build unchained 13h, we
discard them, and then reconstruct what they must have meant with the mode-Y snapshot
heuristic. It works because someone spent a session making it work for Doom.

**Skyroads programs nothing at all** — BIOS mode set, then linear writes to A000. Same
mode number, zero registers.

Two guests, one mode, six registers versus none. That is the whole thesis in one table:
on period hardware both work because the hardware *is* the registers; here each needed
its own code path because there was nothing for them to be different *in*.

⚠ Also measured, and a gap in its own right: **our INT 10h mode set programs none of
the file.** `selftest.com` sets modes through the BIOS and every group reports
`written-by-guest: none`. A real VGA BIOS writes all ~70 registers on a mode set, which
is where a guest's "read back what the BIOS left" expectations come from — and it is
what step 3 must derive geometry *from*. Closing that belongs with step 2's probe:
`vga_defaults.h` is already generated from a probe against genuine 6.22 but covers only
the AC palette and the DAC, so the SEQ/CRTC/GC/external tables do not exist yet.

⚠ Doom page-flips by writing **`0C` only** (986 times, no `0D` at all): its pages are
64 KB-aligned, so the low byte never changes. Any logic that waits for the `0C`+`0D`
pair before committing a flip would never commit one for Doom. Ours commits on `0C`
(measured `start=32768` at exit), so this is a note for step 3, not a live defect.

## Step 2, first results — the probe against genuine MS-DOS 6.22 (2026-09-22)

`tools/dostest/p_vgareg.asm` dumps the whole file (64 bytes: Misc/Feature/InpStat0/
DACMask, SR0–4, CR00–18, GR0–8, AR00–14) after each BIOS mode set, plus three
behavioural cases. Full data: `tools/dostest/vgareg.ref.txt`.

⚠ **PCem was unavailable.** It exits early even for `p_vesa`, which has run 128/128
there — so the fault is the oracle's, not the probe's, and the rows marked OPEN below
must not be acted on until it answers. (Likely its documented GUI/desktop-session
dependency; the runs above were driven from a non-interactive session.)

**CONFIRMED — our guess was right.** Input Status 0 (`3C2` read) = `0x00` on 6.22, which
is what step 1 chose and flagged as unverified. One down.

**CONFIRMED DEFECT — CRTC write protect is not honoured.** `vga.cr11wp.protected.cr00`:
with CR11 bit 7 set, a write of `0x55` to CR00 is **refused** by hardware (reads back
`0x5F`, mode 3's real Horizontal Total) and **accepted** by us (`0x55`). Written from
the spec before any fix, and it failed on first run — as intended.

**CONFIRMED GAP, and far starker than the static inventory could show — a BIOS mode set
leaves ~60 meaningful register values on real hardware, and about five here.**

| Mode | Misc Out (6.22 → ours) | Real CR00–CR18 | Ours |
|---|---|---|---|
| 03h text | `67` → `67` | `5F4F5082 5581BF1F 004F0D0E …A3FF` | zeros but CR0A/0B/13 |
| 0Dh 320×200×16 | **`63`** → `67` | `2D272890 2B80BF1F 00C0…E3FF` | same zeros |
| 12h 640×480×16 | **`E3`** → `67` | `5F4F5082 5480…04E3FF` | same zeros |
| 13h 320×200×256 | **`63`** → `67` | `5F4F5082 5480BF1F 0041…A3FF` | same zeros |
| 07h mono | **`66`** → `67` | (via 3B4 — reads at all only since step 1) | same zeros |

Mode 12h really does leave **`0xE3`** in Miscellaneous Output — the 480-line sync
polarity this document predicted — and 0Dh/13h leave `0x63` (÷2 dot clock), 07h `0x66`
(bit 0 clear: CRTC at 3Bx). **We report `0x67` for every mode, because our INT 10h never
programs it.** Likewise the Sequencer (`03 00 03 00 02` for mode 3 — odd/even, planes
0+1 — versus our `00 00 0F 00 00`), the Graphics Controller (`…10 0E 0F FF`: GR5 odd/even,
GR6 = B800 + chain-odd/even — versus our `…00 00 00 FF`), and AR10/AR12/AR13 (`0C 00 0F 08`
for mode 3, **`41` for mode 13h — bit 6, the 8-bit colour formatter** — versus our zeros).

Our AC palette registers AR00–AR0F match the oracle exactly in every colour mode, which
is `vga_defaults.h` doing its job and is the pattern the rest should follow.

**OPEN — do not act on one host.**
- **DAC Pixel Mask** reads `0x00` on the oracle, `0xFF` here. `0xFF` is the documented
  reset value and `0x00` would mean a blanked screen, so this smells like a read-path
  quirk (the hidden-DAC-register trick fires after four consecutive `3C6` reads with no
  `3C7/3C8/3C9` access between — which this probe does). Needs PCem, and possibly a
  probe that touches `3C8` between reads.
- **Mode 7's AC palette** from the oracle (`20 08 08 08…18`) disagrees with
  `vga_defaults.h`'s own mode-7 row (`20 01 08 03…0F`), which is itself labelled as
  measured on 6.22. One of the two was read by a different route (INT 10h AH=10h vs port
  `3C1`). Worth settling before either is trusted.

## ★★★★★ THE WORKED EXAMPLE — DOOM'S LOW DETAIL (s75, 2026-09-22)

The user's directive said to build from the spec rather than from what an app asks for.
This is what the alternative costs, measured in one evening.

**The hardware mechanism is one sentence:** a store passes through the Sequencer map
mask and lands in EVERY plane the mask selects, at the moment of the write.

**What we built instead:** a named special case, "mode Y", which points the A0000 window
at ONE plane and, for a multi-plane mask, at a scratch buffer whose writes are
reconstructed afterwards by diffing against a seed.

**What happened:**
- Doom HIGH detail writes single-plane masks. Someone made that case work. It works.
- Doom LOW detail writes TWO-plane masks -- `0x03` x143,490 and `0x0c` x143,514 against
  ~16,900 for each single-plane mask. Nobody implemented against it. It is broken:
  the status bar loses ~70% of its detail (3613 -> 941 odd-column changes) and menu
  letterforms survive in planes that were never updated.
- **And it cannot be patched at that layer.** The diff cannot distinguish "written again
  with the same value" from "not written" -- and Doom's doubled pixels are the same
  colour, so that case is constant. Advancing the seed (`4aadc0b`) fixed the 3D view
  (16059 -> a consistent 352) and made the stale-plane artefacts worse; it was reverted
  (`52af908`). Over-fan = wrong data, under-fan = stale data, and no policy is right.

▶ **THE POINT: the approximation discarded information the hardware model has for free.**
The mask is present at the store. Reconstructing it later asks a question the buffer
cannot answer. That is the structural cost of filling gaps from what an app asked for,
and no amount of tuning recovers it -- the fix has to move to the layer that still has
the information, which is steps 3-4 below.

## The recommendation

**Make the register file real, then derive the picture from it.** Concretely, in this
order — each step is independently shippable and testable:

1. ~~**Capture everything**~~ — **DONE 2026-09-22.** Ports `3C2/3C3/3C6/3CA/3CC` and the
   `3B4/3B5/3BA` mono aliases claimed; all four indexed groups stored with a per-index
   write count; spec read-back for every index (they used to return 0 or `0xFF`); the
   `STAGE2: VGAREG` dump, printed on **both** exit paths — the forced-exit path prints
   no STAGE2 block, and Doom and ZAR both leave that way.
   ⚠ Reads of the five newly-claimed ports changed from the bus's absent-device `0xFF`
   to hardware values. Input Status 0 (`3C2` read) returns `0x00` and is marked
   UNVERIFIED in the dump itself, pending step 2's oracle — it is not guessed at.
2. **A probe + the oracle.** `tools/dostest/p_vgareg.com`: write and read back every
   register, dump the file after each BIOS mode set, run against PCem's ET4000. The
   expectations come from the spec **first**, as the VESA work did — see them fail, then
   fix. That closes "does our reset state match a real card?", which nothing has asked.
3. **Derive geometry from the file.** Width/height/stride/start/pan/doubling computed
   from Misc Out + CRTC + SR, not from `st->mode`. Mode X then works because it *is*
   those registers, not because anyone implemented Mode X.
4. **Derive addressing from CR17/CR14/GR5/GR6/SR4** — one address generator replacing
   `chain4` + the mode-Y snapshot + `mkind`, so the four different mode 13h idioms stop
   being four code paths.
5. **Retire the heuristics** (`modey_flush`'s snapshot, the `ymap_*` host hooks) only
   once 3 and 4 are carrying the guests, one guest at a time, rolling back on "worse".

⚠ **Steps 3–5 touch every rendering path and must not be one commit.** The whole shelf
— Doom, Wolf3D, Mario, Skyroads, Lemmings, ZAR, Hexen, heaven7 — is the regression set,
and it is a by-hand set: the headless rig cannot see a wrong picture that still renders.

## Re-measuring

This file's numbers are a measurement of the code on 2026-09-22 and will go stale the
moment the code moves. Re-derive them, do not quote them:
`grep -n "case 0x" src/vdd/vdd_video.c` over the four `*_set_data` handlers and
`vdd_claim_ports` at `vdd_video.c:3036`.
