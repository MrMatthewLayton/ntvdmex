# Video BIOS — INT 10h and the BDA block

> **Carried over from `docs/PARITY.md` on 2026-09-23**, which is now retired — this is
> the same measured data in the inventory's home. See
> [`README.md`](README.md) for the method and the status vocabulary.
>
> ⚠ **The `state` column below is still PARITY's four-state vocabulary**
> (`missing` / `guessed` / `implemented` / `verified`). Re-marking it against the code
> in IMPL / PART / STORE / MISS / N-A, with a `file:line` per row, is owed — and is the
> point of doing it: a row that reads `implemented` here may well be **PART**, which is
> the state that returns a plausible wrong answer.

# Video — INT 10h and the BDA block (`p_video.asm`)

Modes 03h, 01h, 07h, 12h, 13h, 06h: for each, the BDA video block, `AH=0Fh`, and a
cursor set/get round trip. **19 mismatches on the first run; all but one closed.**

⚠ **Provisional against SeaVGABIOS.** Good for "is this field written at all, and is
it the standard value"; worthless as a raster or palette reference.

| item | state | notes |
|---|---|---|
| `0040:0049` mode, `004A` columns | verified | all six modes |
| `0040:0084` rows-1, `0085` char height | verified | the fields that read zero in s71 |
| **`0040:004C` page size** | verified (06h/12h/13h) | **was a flat `0x2000`** — see below |
| `0040:0063` CRTC index port | verified | `3D4h`, and `3B4h` in mode 07h |
| `0040:0062` active page, `004E` page offset | verified | |
| `AH=0Fh` mode/columns in AX | verified | |
| **`AH=0Fh` must not clobber BL** | verified | **was zeroing all of BX** |
| `AH=02h`/`03h` cursor set/get round trip | verified | position and shape |
| **cursor shape is `0000` in a graphics mode** | verified | **was the text underline `0607`** |
| **`AH=11h AL=30h` CX** | verified | **was the requested table's height, not the screen's** |
| `0040:0065` mode-select register | ⛔ **blocked on PCem** | we say `0x29` for mode 3; SeaVGABIOS writes nothing there at all. Ours is very likely right, but "likely" is not measured. **Do not "fix" this to match QEMU.** |

### Gaps this found and closed (s72)

* **`AH=11h AL=30h` returned the wrong character height.** The classic gotcha in this
  call: `BH` selects which *table* `ES:BP` points at, but **`CX` reports the height of
  the font the screen is currently drawing with**. We returned the requested table's
  height — so `BH=0` (the 8x8 upper half) answered `CX=8` in mode 3, contradicting our
  **own** BDA byte at `0040:0085` two lines of probe output earlier. Every 43- and
  50-line editor sizes the screen from `CX`.
* **The graphics page size at `0040:004C` was a flat `0x2000` for every mode.**
  Measured: mode 06h is `0x4000` and mode 12h is `0xA000`. A program that pages by
  adding this to its offset lands inside the previous page. 06h/12h/13h are
  oracle-verified; the other graphics modes now use the standard VGA BIOS table and
  are **not** verified.
* **`AH=0Fh` zeroed the whole of BX.** `BH` is the active page; `BL` is not defined by
  the call and the real BIOS leaves it alone — we were taking the caller's `BL` with
  it. The oracle returns the probe's poison in `BL`, which is how this was seen at all.
* **The cursor shape in a graphics mode.** `AH=03h` and `0040:0060` reported the text
  underline `0607` in modes 06h/12h/13h; the BIOS reports `0000`. The stored shape is
  left alone so returning to text restores it.

### ⚠ A harness artifact this probe walked into

The first version asked *where the cursor happened to be*. That compared the two
**harnesses**, not the two hosts: the oracle redirects stdout to a file so nothing has
moved the cursor, while our run captures output and the screen cursor has walked down
the page. Six rows of confident red for no defect at all. It now **sets** the cursor
and then reads it back, which is the actual contract and is harness-independent.
▶ When a probe row differs on every single case, suspect the harness before the host.

## Not yet inventoried (video)

| item | state | why |
|---|---|---|
| palette / DAC contents after a mode set | implemented | needs PCem: QEMU's VGA is not a palette reference |
| `1112h`/`1111h`/`1114h` row counts end to end | implemented | s71 work; `p_video.asm` covers the resulting BDA but not each call |
| the raster (split, retrace timing, 0x3DA phase) | implemented | needs real hardware; QEMU shows one palette per frame |
| page flipping (`AH=05h`) across the new page sizes | guessed | the page-size fix makes this worth asking |
| non-verified graphics page sizes (04h/05h/0Dh/0Eh/0Fh/10h/11h) | guessed | standard table; no guest we run uses them |

---
