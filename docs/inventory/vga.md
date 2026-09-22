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

## The recommendation

**Make the register file real, then derive the picture from it.** Concretely, in this
order — each step is independently shippable and testable:

1. **Capture everything** (no behaviour change). Claim `3C2/3C3/3C6/3CA/3CC` and the
   `3B4/3B5/3BA` aliases; store all of SEQ/CRTC/GC/AC into complete arrays, including
   the indices nothing consumes yet; add read-back for every index the hardware allows.
   Add a `vgaregs` dump to STAGE2. **This immediately makes "which registers do our
   guests actually program?" answerable — the inventory's own evidence base — and gives
   PCem something to diff against.**
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
