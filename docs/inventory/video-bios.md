# Inventory — VGA BIOS: INT 10h and the BDA video block

**Spec:** IBM *VGA Technical Reference* (the BIOS chapter); Ralf Brown's Interrupt List
(INT 10h `00h`–`1Ch`, BDA `0040:0049`–`0040:008A`, `0040:00A8`). ⚠ **Not held in the
repo** — [`../ref/SOURCES.md`](../ref/SOURCES.md). The *chip* is [vga.md](vga.md) (with
[`../ref/vga.md`](../ref/vga.md)); `AH=4Fh` is [vesa.md](vesa.md). This file is the
firmware: what the BIOS calls do to that chip and to the BDA.
**Our implementation:** `int10` in `src/vdd/vdd_video.c` (`:1406-1933`) and its helpers
(`cell`, `scroll_up`, `teletype`, `glyph_12h`), the BDA writer `vdd_video_bda_sync`
(`:436-482`). The stub is `DOS_HDLR_SEG:0020` = `BOP 10h; IRET` (`main.c:26982-26984`);
V86 arm `main.c:28969-28979`, PM arm `:22226-22239`.
**Probes:** `p_video.asm`, `p_video2.asm` (#188), `p_plan12.asm`, `p_vgareg.asm` (mode
tables). **Off-VM:** `video_test.c`, `vgarom_test.c`.
**Marked:** 2026-10-01, **from the code**. Carried over from `docs/PARITY.md` (retired
2026-09-23) and re-marked.

⚠ **Verification.** `p_video` was compared against 6.22 under QEMU, whose VGA BIOS is
**SeaVGABIOS** — a rewrite. Those rows are **provisional**: good for *"is this field
written, and is it the standard value"*, worthless as a raster or palette reference. In
s84 (#188, `9a91149`) PCem's genuine IBM VGA ROM answered `p_video2`: the page sizes,
`0040:0065/0066`, and `AH=05h` page flips in modes 03h and 0Dh. Those rows are **oracle**.

---

## Headline

**The mode set, the BDA, the palette/DAC calls and the font calls are solid; the
character and pixel services are a text-mode-and-mode-12h subset.** The shape of the gap:

- **Text output in the other graphics modes goes into the text buffer.** `09h`, `0Ah`,
  `0Eh` and `13h` write `(char, attr)` pairs through `cell()` (`:398-399`), which always
  addresses `B800:0`. In mode **13h** that is memory the screen does not show; in the
  **CGA modes 04h–06h** it *is* the frame buffer, so the bytes appear as pixel noise.
  Only the planar modes get glyphs, and those are drawn **8x16 at a 640-pixel stride**
  (`glyph_12h` `:1389-1401`) — right for 11h/12h, wrong for 0Dh, 0Eh, 0Fh and 10h.
- **Pages are bookkeeping only.** `05h` stores the page and the BDA follows (oracle-
  verified), but the CRTC start address is never moved and every write still lands on
  page 0. `02h`/`03h`/`08h`–`0Ah`/`13h` ignore `BH`.
- **`12h` answers "supported" for every `BL` it does not implement** (`:1897`) — the
  shape that hid `BL=31h` from Lemmings until it was fixed.

| Group | Units | IMPL | PART | STORE | MISS | N/A |
|---|---|---|---|---|---|---|
| §1 Mode, cursor, page, scroll, characters, pixels (`00h`–`0Fh`) | 16 | 2 | 12 | 1 | 1 | — |
| §2 `10h` palette and DAC | 17 | 13 | 2 | — | 2 | — |
| §3 `11h` character generator | 6 | 3 | 1 | — | 2 | — |
| §4 `12h` alternate select | 9 | 2 | — | — | 6 | 1 |
| §5 `13h`, `1Ah`–`1Ch`, the rest (`4Fh` → vesa.md) | 6 | 2 | 2 | — | 1 | 1 |
| §6 BDA video block and video vectors | 21 | 14 | 2 | — | 5 | — |
| **Total** | **75** | **36** | **19** | **1** | **17** | **2** |

---

## 1. `AH=00h`–`0Fh`

| AH | Unit | Status | Where / what is missing | Verification |
|---|---|---|---|---|
| `00h` | set mode | **IMPL** | `:1413-1553`: modes 00h–07h and 0Dh–13h (`vid_modes` `:380-396`); AL bit 7 keeps VRAM (`:1428`, recorded in `0040:0087`); reloads DAC, register file (`vga_load_modedef`, measured tables), CRTC, sequencer chain-4, blink, ROM font; returns the *mode flag* in AL (`:1542`). Modes 08h–0Ch are not VGA modes and are logged as unimplemented | **oracle** for the register tables ([vga.md](vga.md), `p_vgareg` on PCem) and AL (`p_plan12` on PCem); provisional for the BDA rows |
| `01h` | set cursor shape | **PART** | `:1554` stores CX raw (CRTC `0Ah`/`0Bh` read the same state, `:2614-2615`). ⚠ **CGA cursor emulation** — `0040:0087` bit 0 clear says it is on (`:478`), so the BIOS should scale an 8-line shape to the 16-line cell — is not done | untested |
| `02h` | set cursor position | **PART** | `:1555`; **`BH` (page) ignored** — one cursor for all pages | provisional (`p_video` set/get round trip, page 0) |
| `03h` | get cursor position and shape | **PART** | `:1556-1563`; shape `0000` in graphics modes (measured fix); `BH` ignored | provisional |
| `04h` | read light pen | **MISS** | `default:` (`:1927-1930`) leaves `AH=04h`; the VGA BIOS answers `AH=00h`, "not triggered" | — |
| `05h` | select active page | **STORE** | `:1564` stores `st->page`, and the BDA follows (§6). **The CRTC start address is not moved**, and `cell()` always writes page 0 — the displayed page never changes | **oracle** for the BDA effect (`p_video2`, #188) |
| `06h` | scroll up | **PART** | `scroll_up` `:484-500`: text modes only — in a graphics mode it shifts bytes at `B800:0` | provisional (text) |
| `07h` | scroll down | **PART** | `:1689-1711`; text only, as `06h` | untested |
| `08h` | read character and attribute | **PART** | `:1569-1570`: text only, page ignored. Reading a character back from **graphics** pixels (the BIOS matches the glyph) is not done | untested |
| `09h` | write character and attribute | **PART** | `:1571-1581`: text ✓; planar modes as 8x16 glyphs at 640-pixel stride (wrong for 0Dh/0Eh/0Fh/10h); **mode 13h and CGA modes write into `B800:0`**; `BL` bit 7 (XOR) ignored; page ignored | untested |
| `0Ah` | write character only | **PART** | same arm as `09h` | untested |
| `0Bh` | set border / background / CGA palette | **PART** | `:1712-1715`: `BH=0` sets the overscan (border) — right in text, but in graphics modes it should set the **background** (palette 0) and the CGA intensity bit; `BH=1` selects the CGA palette (consumed by `render_cga` `:3460`) | untested |
| `0Ch` | write pixel | **PART** | `:1582-1597`: mode 13h ✓; planar at **640x480 geometry** for every planar mode; **CGA modes not written**; `AL` bit 7 (XOR) ignored; page ignored | untested |
| `0Dh` | read pixel | **PART** | `:1716-1731`: mode 13h and planar (stride from `gw` ✓); **CGA modes read 0** | untested |
| `0Eh` | teletype | **PART** | `:1598-1603`, `teletype` `:513-529`: text ✓ (BEL, BS, CR, LF, scroll); planar = `glyph_12h` (geometry as `09h`); **mode 13h and CGA go to `B800:0`**; colour in `BL` used only by the planar path | provisional (text, via DOS output) |
| `0Fh` | get mode, columns, page | **IMPL** | `:1604-1610`; `BL` preserved (measured fix) | **oracle** for `BH` after a page flip (#188); provisional for `BL` |

## 2. `AH=10h` — palette and DAC

`:1611-1688`. DAC values go through the DAC width (`dac_pack_w`/`dac_from8`) as a real
BIOS's `OUT`s would (#226).

| AL | Unit | Status | Where / what is missing | Verification |
|---|---|---|---|---|
| `00h` | set one palette register | **IMPL** | `:1631-1634` | untested |
| `01h` | set border | **IMPL** | `:1635-1636` | untested |
| `02h` | set all 16 + border | **IMPL** | `:1637-1642` | untested |
| `03h` | blink / intensity | **IMPL** | `:1643-1649`; AR10 bit 3 follows | provisional (`ar10.blink.*`) |
| `07h` | get one palette register | **IMPL** | `:1650-1652` | untested |
| `08h` | get border | **IMPL** | `:1653-1654` | untested |
| `09h` | get all 16 + border | **IMPL** | `:1655-1657` | untested |
| `10h` | set one DAC register | **IMPL** | `:1616-1621` | untested |
| `12h` | set a block of DAC registers | **IMPL** | `:1622-1630` | by hand (Lemmings) |
| `13h` | select colour paging mode / page | **PART** | `:1658-1659`: `BL=00h` (paging mode) and `BL=01h` (page) are **the same store**; the value is consumed by nothing — the renderer never pages the DAC | untested |
| `15h` | get one DAC register | **IMPL** | `:1660-1664` | untested |
| `17h` | get a block of DAC registers | **IMPL** | `:1665-1673` | untested |
| `18h` | set the PEL mask | **MISS** | logged as unimplemented (`:1685-1686`); port `3C6h` itself is modelled ([vga.md](vga.md)) | — |
| `19h` | get the PEL mask | **MISS** | same | — |
| `1Ah` | get colour paging state | **PART** | `:1674-1675`: `BL` always `00h`, `BH` = the conflated value above | untested |
| `1Bh` | sum to grey scale | **IMPL** | `:1676-1684`, 30/59/11 weights | untested |
| — | default palette loading (`12h BL=31h`) honoured by the mode set | **IMPL** | `def_pal_off` (`:290`, `:1893-1896`) | by hand (Lemmings, the fix) |

## 3. `AH=11h` — character generator

`:1757-1875`.

| AL | Unit | Status | Where / what is missing | Verification |
|---|---|---|---|---|
| `00h`/`10h` | load a user font (`10h` also recomputes the rows) | **IMPL** | `:1817-1851`; up to `VID_CELL_H` scan lines; `BL` (font block) ignored | untested |
| `01h`/`02h`/`04h`, `11h`/`12h`/`14h` | load ROM 8x14 / 8x8 / 8x16 (`1x`: rows follow — the 43/50-line calls) | **IMPL** | `:1852-1867` | provisional (`p_video` covers the resulting BDA, not each call) |
| `03h` | set block specifier | **PART** | ⛔ `(al & 0x0F) <= 0x04` (`:1805`) sends `03h` down the ROM-font arm with height 16 (`:1859`): it **drops a loaded user font** instead of selecting font blocks | untested |
| `20h`–`24h` | set graphics-mode font pointers (INT 1Fh / INT 43h, rows) | **MISS** | `:1869-1871`: answers `DL` only; neither vector nor any row count is stored | — |
| `30h` | get font information | **IMPL** | `:1759-1804`; `ES:BP` = our tables; **CX is the on-screen height**, not the table's (measured fix) | provisional (`AH=11h AL=30h CX`) |
| other | — | **MISS** | logged as unimplemented (`:1872-1874`) | — |

## 4. `AH=12h` — alternate select

`:1876-1898`. ⛔ **Every `BL` without an arm answers `AL=12h` — "supported" — and does
nothing** (`:1897`).

| BL | Unit | Status | Where / what is missing | Verification |
|---|---|---|---|---|
| `10h` | get EGA/VGA information | **IMPL** | `:1878`: `BX=0003h` (colour, 256 KB), `CX=0009h` | untested |
| `20h` | alternate print-screen handler | **MISS** | reports success | — |
| `30h` | select scan lines for text modes (200/350/400) | **MISS** | reports success; the next text-mode set is always 400 lines | — |
| `31h` | default palette loading on/off | **IMPL** | `:1893-1896` | by hand (Lemmings) |
| `32h` | video addressing on/off | **MISS** | reports success | — |
| `33h` | grey-scale summing on/off | **MISS** | reports success; `10h/1Bh` exists, but a mode set never sums | — |
| `34h` | cursor emulation on/off | **MISS** | reports success (see `01h`) | — |
| `35h` | display switch | **N/A** | for machines with two display adapters | — |
| `36h` | video refresh on/off | **MISS** | reports success | — |

## 5. `AH=13h`, `1Ah`–`1Ch`, and the rest

| AH | Unit | Status | Where / what is missing | Verification |
|---|---|---|---|---|
| `13h` | write string | **PART** | `:1746-1756`: **always moves the cursor** (AL bit 0 clear should leave it); BEL/BS/CR/LF are **stored as glyphs**, not executed; text modes only; page ignored | untested |
| `1Ah` `AL=00h` | get display combination | **IMPL** | `:1899-1902`, `BX=0008h` (VGA colour, no secondary) | untested |
| `1Ah` `AL=01h` | set display combination | **MISS** | not distinguished from `00h`: the caller's `BX` is overwritten with `0008h` | — |
| `1Bh` | functionality / state information | **PART** | `:1903-1925`: mode, columns, rows, cell height are live; **256 colours, 8 pages and scan-line code 0 for every mode**, and the static table is a constant | untested |
| `1Ch` | save / restore video state | **IMPL** | `:1732-1745`; the size reported is the size written (`VID_STATE_BLOCKS`, measured fix) | untested |
| `4Fh` | VESA BIOS extensions | — | [vesa.md](vesa.md) | — |
| other | unknown function | **N/A** | `:1927-1930`: logged (`unimpl_fn`), registers untouched — what a VGA BIOS does with a function it lacks | — |

## 6. The BDA video block, and the vectors the BIOS owns

`vdd_video_bda_sync` (`:436-482`) rewrites the block after every INT 10h call and every
frame — *derived, not maintained*.

| Offset / vector | Unit | Status | Where / what is missing | Verification |
|---|---|---|---|---|
| `0049h` | current mode | **IMPL** | `:441` | **oracle** for 04h/05h/0Dh/0Eh/10h/11h (#188); provisional for 01h/03h/06h/07h/12h/13h |
| `004Ah` | columns | **IMPL** | `:442` | as `0049h` |
| `004Ch` | page size | **IMPL** | `:443-463`; per mode (was a flat `2000h`) | **oracle** (06h/12h/13h on 6.22; the rest on PCem, #188) |
| `004Eh` | current page offset | **IMPL** | `:464-465` | **oracle** (`AH=05h` flips, #188) |
| `0050h`–`005Fh` | cursor position, per page | **PART** | `:466-467`: only the **active** page's slot is written, from the single cursor; the other seven are never kept | — |
| `0060h` | cursor shape | **IMPL** | `:468-470`; `0000h` in graphics modes | provisional |
| `0062h` | active page | **IMPL** | `:471` | **oracle** (#188) |
| `0063h` | CRTC base port | **IMPL** | `:472-473`; `3B4h` in mode 07h | provisional |
| `0065h` | CGA mode-select register | **IMPL** | `:1449-1453`: the CGA table for 00h–07h, left alone by the EGA/VGA modes | **oracle** (PCem IBM VGA ROM + DOSBox-X; SeaVGABIOS abstains) |
| `0066h` | CGA palette register | **IMPL** | `:1452` | **oracle** (#188) |
| `0084h` | rows − 1 | **IMPL** | `:474` | provisional |
| `0085h` | character height | **IMPL** | `:475` | provisional |
| `0087h` | EGA/VGA control (256 KB, bit 7 = last mode set kept VRAM) | **IMPL** | `:478` | untested |
| `0088h` | switch settings | **IMPL** | `:479`, `09h` | untested |
| `0089h` | VGA flags: VGA active, scan-line selection | **PART** | `:481`: bits 7/4 are **derived from the current mode's height**; the BIOS keeps the count chosen by `12h BL=30h` for the next text-mode set (which is MISS, §4) | untested |
| `008Ah` | display-combination index | **MISS** | not written | — |
| `00A8h` | Video Save Pointer table | **MISS** | not written — the far pointer to the parameter table, the dynamic save area and the font overrides is whatever was there | — |
| INT `1Dh` | video parameter table | **MISS** | the vector is not ours; nothing in `src/` writes it | — |
| INT `1Fh` | 8x8 font, characters 80h–FFh | **MISS** | the vector is not written (`11h/30h BH=0` answers with our table instead, `:1771-1772`) | — |
| INT `43h` | graphics-mode font | **MISS** | not written (`BH=1` answers from `cell_h`, `:1775-1784`). With the `11h/2xh` MISS, §3, these are the same gap | — |
| `0040:0065`/`0066` at start-up | initial values | **IMPL** | mode 3's table is written by the first mode set; the old fixed `29h`/`30h` is gone | **oracle** (#188) |

---

## Measured history (kept)

### Gaps the probe found and closed (s72)

* **`AH=11h AL=30h` returned the wrong character height.** `BH` selects which *table*
  `ES:BP` points at, but **`CX` reports the height of the font the screen is currently
  drawing with**. We returned the requested table's height — so `BH=0` (the 8x8 upper
  half) answered `CX=8` in mode 3, contradicting our **own** BDA byte at `0040:0085`.
  Every 43- and 50-line editor sizes the screen from `CX`.
* **The graphics page size at `0040:004C` was a flat `0x2000` for every mode.**
  Measured: mode 06h is `0x4000` and mode 12h is `0xA000`.
* **`AH=0Fh` zeroed the whole of BX.** `BL` is not defined by the call and the real BIOS
  leaves it alone. The oracle returns the probe's poison in `BL`, which is how this was
  seen at all.
* **The cursor shape in a graphics mode.** `AH=03h` and `0040:0060` reported the text
  underline `0607` in modes 06h/12h/13h; the BIOS reports `0000`.

### ⚠ A harness artefact this probe walked into

The first version asked *where the cursor happened to be*. That compared the two
**harnesses**: the oracle redirects stdout to a file so nothing has moved the cursor,
while our run captures output and the cursor has walked down the page. Six rows of
confident red for no defect. It now **sets** the cursor and reads it back. ▶ When a probe
row differs on every single case, suspect the harness before the host.

### #188 (s84) — what PCem settled

`0040:0065/0066` hold the CGA table for modes 00h–07h (`2C 28 2D 29 2A 2E 1E 29`; `0066` =
`30h`, `3Fh` in mode 6) and the EGA/VGA modes leave both alone; we used to write
`29h`/`30h` once at start-up. Page sizes for 04h/05h/0Dh/0Eh/10h/11h and `AH=05h` page
flips (0062, 004E, `AH=0Fh` BH) in modes 03h and 0Dh were already right and are now
verified. Mode 01h's cursor became the ROM's `0D0E`.

## What to fix, in order

1. Text output (`09h`/`0Ah`/`0Eh`/`13h`) in mode 13h and the CGA modes, and the planar
   glyph path at each mode's own cell size and stride. Then `0Ch`/`0Dh` for CGA.
2. Pages that page: `05h` moves the CRTC start address, and the character services
   honour `BH` and the per-page cursors in `0040:0050`.
3. `12h`: refuse what is not implemented, or implement `30h` and `34h` (both change what
   a later mode set does).
4. `11h AL=03h` must not drop a user font; `11h AL=20h–24h` and INT 1Fh/43h.
5. `0040:00A8` (the Video Save Pointer table) and INT 1Dh.
