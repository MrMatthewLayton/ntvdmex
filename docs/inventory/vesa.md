# Inventory — VESA BIOS Extensions (VBE 2.0, with 3.0 noted)

**Spec:** VESA *VBE Core Functions Standard 2.0* (document revision 1.1), [`../ref/vbe20.pdf`](../ref/vbe20.pdf);
VESA *VBE Core Functions Standard 3.0*, [`../ref/vbe30.pdf`](../ref/vbe30.pdf). Both PDFs were
read for this document (text extracted with `pdftotext`); section numbers below are VBE 2.0's
unless marked "3.0". The supplemental functions (4F10h VBE/PM, 4F15h VBE/DDC) are only *listed*
in VBE 2.0 §5.7; their own specifications are **not held** in the repo, and our code says so
(`vdd_video.c:1063-1068`, `:1077-1083`). There is no `docs/ref/vesa.md` yet.
**Our implementation:** `src/vdd/vdd_video.c` — `vesa()` `:720-1155`, the mode tables `:508-543`,
the window and frame plumbing `:555-652`, the state block `:654-717`, the frame build `:3305-3323`;
`src/vdd/vdd_video.h` (`VID_VESA_*` `:48-65`, state `:148-150`, `:176-192`, diagnostics
`:447-474`); `src/vdd/present_ddraw.c` (8bpp and 32bpp snapshots, `:530-545`); `src/vdd/ntvdd.h:61-62`
(the 1280x1024 frame cap). The LFB mapping is DPMI `0800h` in `src/host/main.c`.
**Off-VM:** `tools/dostest/video_test.c` T9–T12f (`:215-474` before #226) and #226's T12g–T12i (`:493-677`: DAC width, retrace wait on a fake clock, 4F02/4F03 and the VGA layer) and the 4F0A check at `:485-486`.
**DOS probes:** `tools/dostest/p_vesa.asm` (4F00 block and every ModeInfoBlock, bytes 0–49) and
`tools/dostest/p_vesapm.asm` (4F0Ah), both `ORACLE-ALSO: pcem-vesa`; `tools/dostest/p_vbepm.asm` (#53: a DPMI
client calls a COPY of the 4F0Ah block; no oracle can run it). Off-VM, `tools/dostest/vbepm_test.c` runs the
block's code through `pm32interp.h`. **No probe exercises
4F02–4F09** — the runtime half has no oracle at all.
**Marked:** 2026-09-29, **from the code**, with citations. ⚠ `src/host/main.c` was being edited
in the working tree while this was written; its citations give the **function name** and an
approximate line — search for the symbol.
**Re-marked for #226** (same day): every row #226 changed says so and cites the code as it
now stands (`vdd_video.c` line numbers at commit `5b78ecc`, plus the function or `case`).
⚠ **The other `:NNN` citations into `vdd_video.c` predate #226 and have drifted** — `vesa()`
moved from `:720` to `:799`, and everything after it by roughly +80 to +250 lines. Search for
the symbol or the `case`.

---

## Headline

**The information half is complete and oracle-checked; the runtime half is implemented to the
letter of the call but not always to its effect.** 4F00 and 4F01 were diffed field by field
against two real VBE implementations in s74 (a Tseng ET4000/W32p ROM under PCem and QEMU's
Bochs VBE) and every graded row agreed. 4F02–4F09 are implemented, spec-tested off-VM, and used
by three guests on the rig (heaven7, ZAR, vesacube). Before #226 four of them did less than they
reported; #226 closed those four in the VDD:
**4F08's 8-bit width now reaches the DAC ports** and the INT 10h palette calls, **4F07 BL=80h
completes at the retrace** (the VDD stamps the wait; ⚠ **the host's wait loop is still owed** —
until `main.c` honours `vdd_video_int10_wait_us()` the call returns at once and only the pacing is
missing), **4F03 returns D14/D15** with `40:87h` bit 7 behind it, and **4F02 leaves a coherent VGA
layer** (kind, chain-4, register file, BDA) instead of the previous mode's; ModeAttributes D5 now
says what our modes are. Still absent: the far-call bank switch (WinFuncPtr), 4F06/4F07 in VESA text modes, and the
runtime half still has no oracle. **4F0Ah, the protected-mode interface, exists since #53** (§13).
⚠ **Not yet on the rig** — every #226 row below is off-VM evidence (`video_test.c` T12g–T12i).

| Group | Units | IMPL | PART | STORE | MISS | N/A |
|---|---|---|---|---|---|---|
| §1 Calling convention and status | 3 | 3 | — | — | — | — |
| §2 4F00h VbeInfoBlock | 16 | 16 | — | — | — | — |
| §3 4F01h the call | 4 | 4 | — | — | — | — |
| §4 4F01h ModeInfoBlock fields | 41 | 38 | 2 | — | 1 | — |
| §5 4F02h set mode | 10 | 8 | — | — | 2 | — |
| §6 4F03h current mode | 2 | 2 | — | — | — | — |
| §7 4F04h save/restore state | 4 | 1 | 2 | 1 | — | — |
| §8 4F05h window control | 7 | 6 | — | — | 1 | — |
| §9 4F06h logical scan line | 6 | 5 | — | — | 1 | — |
| §10 4F07h display start | 8 | 4 | 2 | — | 1 | 1 |
| §11 4F08h DAC width | 4 | 4 | — | — | — | — |
| §12 4F09h palette data | 6 | 6 | — | — | — | — |
| §13 4F0Ah protected-mode interface | 6 | 5 | — | — | 1 | — |
| §14 VBE 3.0 4F0Bh and the supplemental functions | 7 | 4 | — | 1 | 1 | 1 |
| §15 The linear frame buffer | 5 | 3 | 1 | — | — | 1 |
| §16 Presentation | 8 | 7 | — | — | 1 | — |
| §17 The mode list | 34 | 28 | — | — | 6 | — |
| **Total** | **171** | **144** | **7** | **2** | **15** | **3** |

Before #226: 170 units, 124 IMPL, 15 PART, 2 STORE, 26 MISS, 3 N/A. #226 added one unit (§16,
the VESA mode's own display timing) and moved 15 rows (9 PART→IMPL, 5 MISS→IMPL, 1 MISS→PART);
the two §10 rows at PART are PART only for want of the host's wait loop (§10).

### What has been measured on this surface

| Measurement | Where |
|---|---|
| `p_vesa` against QEMU's Bochs VBE and the Tseng ET4000/W32p ROM (`pcem-vesa`): **131 rows, 128 comparable, 100 % agree, 3 abstained** (2026-09-17). ⚠ A recorded score, not a re-run; the rules that shape it are `oracle-rules.json:1235-1336` | `session-74.md:222-236` |
| Found by that oracle and fixed: Capabilities D0 was 0; NumberOfImagePages was 0; YCharSize 8 in 200-line modes; DirectColorModeInfo D1 for 5:5:5; Lin/Bnk image pages | `session-74.md:226-229` |
| `p_vesapm`: 4F0Ah answers `AX=0100` on both real BIOSes -- as we did until #53 (those rows now abstain: neither BIOS has the interface) | `session-74.md:246-249` |
| Heretic: 4F00 through DPMI `0300` with a 256-byte DOS block; our old handler wrote the OEM string at `+100h`, over the next MCB | `session-74.md:511-535` |
| heaven7: 4F00/01/02/07 only; sets `0x4170` (320x240x16, LFB); 16,389 4F07 calls in one run, 15,900 in another, all `(0,0)` — a vsync idiom on one buffer | `session-74.md:51`, `:165-168`, `:180` |
| heaven7 (before 320x240 existed): `BX=4112h` accepted, DPMI `0800` mapped `E0000000h` size `E1000h` = 640·480·3 | `session-74.md:383-387` |
| ZAR: `0x4101` via LFB (DPMI `0800`, then an LDT selector with DPL 0 that NT refused until `dpmi_install` forced DPL 3) and banked 640x480 with 4,735 4F05 calls; both rendered in-game | `session-74.md:33-46` |
| vesacube (`tools/vesacube/`): every banked mode from 4F00/4F01, 4F07 BL=80h page flips, 4F09 palettes; rig screenshots in 640x480x8 and 320x240x16 | `session-74.md:254-259` |

---

## 1. Calling convention and return status (§4.0, §4.1)

| Unit | Status | Where / what is missing | Verification |
|---|---|---|---|
| `INT 10h AH=4Fh` dispatch; AL = function | **IMPL** | `vdd_video.c:1688` → `vesa()` `:720`. Every call is counted per sub-function and per BL (`:723-727`), printed in the STAGE2 summary (`main.c` `WinMain` ≈`:32563-32597`) | rig: STAGE2 VESA lines (heaven7, ZAR) |
| Status: AL=4Fh supported; AH=00h ok, 01h failed, 02h not in this configuration, 03h invalid in this mode | **IMPL** | used per function below; AH=02h/03h where §4.8–§4.12 name them | `video_test.c:265-272`, `:297`, `:304`, `:369-382` |
| A function we do not provide answers `AX=0100h` (AL≠4Fh), not `014Fh` | **IMPL** | `:1153`, and 4F0Ah `:1059` | **oracle**: both real BIOSes answer `0100h` (`session-74.md:246-248`); `video_test.c:485-486` |

A protected-mode client reaches VBE through DPMI `0300h` with a real-mode buffer (Heretic's
trace, `session-74.md:517-521`); `vesa()` maps `ES:DI` as a real-mode segment (`:740`). No
translation of a protected-mode `ES` selector for `INT 10h AH=4Fh` was found in `main.c`.

## 2. 4F00h — Return VBE Controller Information (§4.3)

The block is written in place in the caller's buffer. OEM string at `+22h` and mode list at
`+40h` — both inside the 256-byte form (`:742`).

| Offset | Field | Status | Where / what is missing | Verification |
|---|---|---|---|---|
| — | `'VBE2'` preset ⇒ 512-byte block and the 2.0 fields; otherwise 256 bytes, nothing past `+FFh` | **IMPL** | `:741`, `:743`, `:754` | `video_test.c:215-238` (poisons 256–511); Heretic (`session-74.md:511-535`) |
| `+00h` | VbeSignature `'VESA'` | **IMPL** | `:744` | **oracle** (`vesa.info`, `oracle-rules.json:1235`) |
| `+04h` | VbeVersion `0200h` | **IMPL** | `:745`. Ungraded: 1.2 on the Tseng ROM, 3.0 on Bochs (`oracle-rules.json:1257`) | compared, ungraded |
| `+06h` | OemStringPtr → `"NTVDMEX VESA"` | **IMPL** | #226: with `'VBE2'` preset it points into **OemData** (`+100h`), as §4.3 requires ("must place this string in the OemData area") — `vesa()` case `0x00`, `vdd_video.c:835-851`. A 1.x caller still gets `+22h` of its own block (it has no OemData, and `+100h` is not its memory); real ROMs point into ROM, and a string in the caller's buffer is gone once the caller reuses it — accepted | pointer masked by the probe; `video_test.c:229` (1.x: inside the block), `:238-249` (2.0: in OemData) |
| `+0Ah` | Capabilities D0 = 1: DAC switchable to 8 bits | **IMPL** | `:751`. Since #226 the switch reaches the ports and the INT 10h palette calls too (§11) — the promise is kept for every path | `video_test.c:265`; found by the oracle (`session-74.md:226`), ungraded card property |
| `+0Ah` | Capabilities D1 = 0: VGA-compatible controller | **IMPL** | `:751` | compared, ungraded |
| `+0Ah` | Capabilities D2 = 0: no blank bit needed for 4F09 | **IMPL** | `:751`; consistent with 4F09 BL=80h being treated as 00h (§12) | compared, ungraded |
| `+0Ah` | Capabilities D3–D4 (3.0): stereo signalling, EVC connector = 0 | **IMPL** | `:751` (none, truthfully) | — |
| `+0Eh` | VideoModePtr → list in the Reserved area | **IMPL** | `:752`, `:761-765`; §4.3 names the Reserved area for exactly this | `video_test.c:227-229` |
| — | Mode list terminator `FFFFh`; list never empty (not a "stub") | **IMPL** | `:766`; 28 entries (§17) | `p_vesa` `vesa.modes` (information only, `oracle-rules.json:1260`) |
| `+12h` | TotalMemory = 64 (4 MB) | **IMPL** | `:753`, `VID_VESA_VRAM` `vdd_video.h:55` | compared, ungraded |
| `+14h` | OemSoftwareRev `0100h` (2.0 callers only) | **IMPL** | `:755` | compared, ungraded |
| `+16h` | OemVendorNamePtr | **IMPL** | #226: its own string, `"NTVDMEX"`, in OemData (`vdd_video.c:845-851`); it was the OEM string at `+22h` | masked by the probe; `video_test.c:238-249` |
| `+1Ah` | OemProductNamePtr | **IMPL** | #226: `"NTVDMEX VBE"`, in OemData | masked; `video_test.c:238-249` |
| `+1Eh` | OemProductRevPtr | **IMPL** | #226: `"1.00"` (agrees with OemSoftwareRev `0100h`), in OemData | masked; `video_test.c:238-249` |
| `+100h` | OemData (256 bytes, 2.0 callers) | **IMPL** | #226: the OEM, vendor, product and revision strings are copied here, back to back (46 bytes), as §4.3 says ("copied into this area by the VBE implementation"); the rest stays zero | `video_test.c:238-249` |

## 3. 4F01h — Return VBE Mode Information: the call (§4.4)

| Unit | Status | Where / what is missing | Verification |
|---|---|---|---|
| A listed graphics mode → 256-byte block, `004Fh` | **IMPL** | `:795-877`. D14/D15 of CX are masked off (`:578`) | `video_test.c:240-246`, `:403-405`, `:444-445`; **oracle** (`p_vesa`, per mode) |
| An unlisted mode → `014Fh`, block untouched | **IMPL** | `:878`. `vesa_find` also refuses any mode that does not fit VRAM (`:585`), so the list and the answer cannot disagree | untested |
| A VESA text mode (108h–10Ch) → block in characters, model 0, window `B800h` | **IMPL** | `:771-794` | `video_test.c:420`; `vesa.mi.0108` compared (`oracle-rules.json:1305`) |
| All 256 bytes zeroed first (§4.4: unused fields zero) | **IMPL** | `:799`, `:776` | `p_vesa` bytes 0–49 |

## 4. 4F01h — ModeInfoBlock fields (§4.4; 3.0 fields marked)

`p_vesa` grades bytes 0–49 except those `oracle-rules.json:1272` ignores as card properties
(ModeAttributes, the window arrangement, BitsPerPixel, NumberOfImagePages); WinFuncPtr and
PhysBasePtr are masked. Graded fields are marked **oracle** below; YCharSize and
DirectColorModeInfo D1 are **DISPUTED** between the two oracles on the 15/16bpp modes both
offer, and we follow the Tseng ROM (`oracle-rules.json:1318-1336`).

| Offset | Field | Status | Where / what is missing | Verification |
|---|---|---|---|---|
| `+00h` D0 | mode supported in hardware = 1 | **IMPL** | `0x009B` `:800`; text `0x000F` `:777` | compared, ungraded |
| D1 | reserved = 1 | **IMPL** | `:800` | ungraded |
| D2 | TTY output supported | **IMPL** | 0 in graphics (we do not draw teletype into VESA modes — a recorded choice, `session-74.md:231-232`); 1 in text (`:777`) | ungraded |
| D3 | colour | **IMPL** | `:800`, `:777` | ungraded |
| D4 | graphics (1) / text (0) | **IMPL** | `:800`, `:777` | `video_test.c:420` |
| D5 | 1 = **not** a VGA-compatible mode (graphics); 0 for the VESA text modes | **IMPL** | #226, decided from §4.4: D5 clear promises "the standard VGA I/O ports … can be assumed", and in our VESA modes the VGA CRTC does not drive the picture (4F06/4F07 are the only pitch and start; CR0C/0D/13h move nothing) and the sequencer/GC do not reach the window. Graphics modes now answer `00BBh` (`vdd_video.c:919`, the reasoning in the comment above it). The two measured BIOSes split on exactly this line: QEMU's SeaVGABIOS VBE (DISPI, like ours) says `BBh` for every packed and direct mode (`build/dosdiff-cache/p_vesa.com-3c1fd507…`), the Tseng ET4000/W32p ROM (CRTC-driven extended modes) says `1Fh`/`1Bh` (`runs/s74b_lazy32/pcem_p_vesa_et4000.txt`). The DAC ports and 3DAh still work in a VESA mode; D5 only stops promising the rest. Text modes 108h–10Ch stay `0Fh` | ungraded (`oracle-rules.json:1272` ignores ModeAttributes); `video_test.c:258-263`. ⚠ rig re-gate owed: heaven7, ZAR, vesacube (vesacube's source does not test D5) |
| D6 | 0 = windowed access available | **IMPL** | `:800` | ungraded |
| D7 | 1 = linear frame buffer available | **IMPL** | `:800` with PhysBasePtr `:869` (D7/D6 = "both windowed and linear", §4.4) | rig: heaven7, ZAR |
| D8–D12 (3.0) | double scan, interlace, triple buffering, stereo, dual display start | **IMPL** | 0 — none offered, truthfully | — |
| `+02h` | WinAAttributes `07h` (relocatable, readable, writeable) | **IMPL** | `:801` | ungraded (the Tseng ROM uses a write-only A + read-only B pair) |
| `+03h` | WinBAttributes `00h` (no window B) | **IMPL** | `:801` | ungraded |
| `+04h` | WinGranularity 64 KB | **IMPL** | `:802` | ungraded |
| `+06h` | WinSize 64 KB | **IMPL** | `:802` | ungraded |
| `+08h` | WinASegment `A000h` (text: `B800h`) | **IMPL** | `:803`, `:780` | ungraded |
| `+0Ah` | WinBSegment 0 | **IMPL** | `:803` | ungraded |
| `+0Ch` | WinFuncPtr = NULL | **IMPL** | `:804`. §4.4 permits NULL ("then VBE Function 05h must be used"); the missing far-call entry is its own row in §8 | masked by the probe |
| `+10h` | BytesPerScanLine | **IMPL** | `w × bytes-per-pixel` (`:798`, `:805`); text `cols × 2` (`:782`) | **oracle** |
| `+12h` | XResolution | **IMPL** | `:806`; text in characters `:783` | **oracle** |
| `+14h` | YResolution | **IMPL** | `:806`, `:783` | **oracle** |
| `+16h` | XCharSize = 8 | **IMPL** | `:810`, `:784`. 80-column text: the Tseng ROM says 9; we render 8-dot cells and say 8 — accepted difference (`oracle-rules.json:1305-1315`) | **oracle** |
| `+17h` | YCharSize: 8 at ≤200 lines, 14 at ≤350, else 16 | **IMPL** | `:810` | **oracle**, DISPUTED on 200-line direct-colour modes |
| `+18h` | NumberOfPlanes = 1 | **IMPL** | `:811` | **oracle** |
| `+19h` | BitsPerPixel 8/15/16/24 (text: 4) | **IMPL** | `:811`, `:786`. 5:5:5 as 15 — the Tseng ROM says 16; both occur in the wild | compared, ungraded |
| `+1Ah` | NumberOfBanks = 1 (no scan-line banks) | **IMPL** | `:824` | **oracle** |
| `+1Bh` | MemoryModel: 04h packed, 06h direct, 00h text | **IMPL** | `:816`, `:788` | **oracle** |
| `+1Ch` | BankSize = 0 | **IMPL** | `:834` | **oracle** |
| `+1Dh` | NumberOfImagePages = pages in VRAM − 1, capped 255 | **IMPL** | `:839-841`; text from the 32 KB `B800h` window (`:790`) | `video_test.c:246`; compared, ungraded (memory size) |
| `+1Eh` | Reserved = 1 | **IMPL** | `:842`, `:791` | **oracle** |
| `+1Fh`/`+20h` | RedMaskSize / RedFieldPosition | **IMPL** | 5/10 (15bpp), 5/11 (16bpp), 8/16 (24bpp) `:848-853`; 0 for 8bpp (zeroed `:799`) | **oracle** |
| `+21h`/`+22h` | GreenMaskSize / GreenFieldPosition | **IMPL** | 5/5, 6/5, 8/8 | **oracle** |
| `+23h`/`+24h` | BlueMaskSize / BlueFieldPosition | **IMPL** | 5/0, 5/0, 8/0 | **oracle** |
| `+25h`/`+26h` | RsvdMaskSize / RsvdFieldPosition | **IMPL** | 1/15 for 5:5:5, else 0 | **oracle** |
| `+27h` | DirectColorModeInfo: D0 ramp fixed (0); D1 Rsvd bits usable (1 for 5:5:5) | **IMPL** | `:857` | **oracle**, DISPUTED (Tseng ROM `02h`, Bochs differs) |
| `+28h` | PhysBasePtr = `E0000000h` (graphics); 0 for text | **IMPL** | `:869`, constant `vdd_video.h:63`; honoured by DPMI `0800h` (§15) | masked by the probe; rig: heaven7, ZAR |
| `+2Ch` | OffScreenMemOffset (2.0) | **PART** | 0 (`:799`). To a 2.0 caller that reads as "no off-screen memory", though VRAM beyond the displayed page exists and 4F06/4F07 use it. 3.0 turned these six bytes into Reserved-0, so the value is right only for 3.0 | bytes graded by `p_vesa`; agreement is with a VBE 1.2 ROM and a 3.0 VBE, neither of which has the 2.0 field |
| `+30h` | OffScreenMemSize (2.0) | **PART** | 0, as above | as above |
| `+32h` (3.0) | LinBytesPerScanLine | **IMPL** | `:873` (same pitch as banked) | — (outside the probe's 50 bytes) |
| `+34h` (3.0) | BnkNumberOfImagePages | **IMPL** | `:874` = `+1Dh` | found by the oracle (`session-74.md:229`), not graded |
| `+35h` (3.0) | LinNumberOfImagePages | **IMPL** | `:874` = `+1Dh` | as above |
| `+36h`–`+3Dh` (3.0) | LinRed/Green/Blue/Rsvd MaskSize and FieldPosition | **IMPL** | `:875-876`, copied from `+1Fh`–`+26h` for direct colour | — |
| `+3Eh` (3.0) | MaxPixelClock | **MISS** | 0. Harmless while VbeVersion says 2.0 (the bytes are Reserved to a 2.0 caller); needed with 4F0Bh and 4F02 D11 (§5, §14) | — |

## 5. 4F02h — Set VBE Mode (§4.5)

| Unit | Status | Where / what is missing | Verification |
|---|---|---|---|
| D0–D8 mode number → look up, set geometry and depth | **IMPL** | `:902-913`; text modes go through `INT 10h AH=00h` mode 3 then take the geometry (`:883-900`) | `video_test.c:251-252`, `:420-443`; rig: `0x4101`, `0x4170`, `0x4112`, `0x101` |
| D9–D13 reserved (2.0): a non-zero bit makes the number unknown → `014Fh` | **IMPL** | the lookup compares `num & 3FFFh` (`:578`, `:548`) | untested |
| D11 (3.0): use the caller's CRTCInfoBlock at ES:DI | **MISS** | D11 is inside the compared number, so the call **fails** `014Fh` — correct for the 2.0 we claim, and the reason 3.0's refresh-rate control is absent | — |
| D14: linear (1) / windowed (0) frame buffer | **IMPL** | graphics modes (`vesa_lfb`; `vesa_sync` stops copying the window). #226: a **text** mode with D14 set now fails `014Fh` before anything changes (`vesa()` case `0x02`, `vdd_video.c:1013-1019`) — §4.5: "If D14 is set, and a linear frame buffer model is not available then the call will fail"; the text ModeInfoBlocks say D7 = 0 | `video_test.c:270-272`, `:411`, `:650`; rig: `0x4101`, `0x4170` |
| D15: don't clear display memory | **IMPL** | graphics `:924-927` (both VRAM and the A0000 window); text via AL bit 7 and `clear_text` `:893`, `:897` | untested (the standard-BIOS twin is in `video-bios.md`) |
| An unavailable mode fails `014Fh` and leaves the environment unchanged | **IMPL** | `:930`; nothing is written before the lookup succeeds | untested |
| A mode set resets: DAC width 6 (§4.11), bank 0, display start (0,0), pitch = the mode's own | **IMPL** | `:913-916` | `video_test.c:298-301` (DAC), `:389` (pitch and start) |
| BDA `40:87h` bit 7 — the memory-clear flag 2.0 BIOSes "should also update" | **IMPL** | #226: `modeset_noclear`, set by 4F02 D15 and by INT 10h AH=00h AL bit 7 (the IBM rule the VBE bit is built on), written by `vdd_video_bda_sync` as `60h \| 80h` (`vdd_video.c:478`). SeaVGABIOS writes the same byte the same way (`0x60 \| (flags>>8 & 0x80)` in `vga_set_mode()`, read from QEMU's `vgabios-stdvga.bin`). ⚠ INT 10h AH=0Fh does **not** yet OR this bit into AL as the IBM BIOS does (§4.6's 1.x note describes it) — that is the standard-BIOS surface, `video-bios.md`'s | `video_test.c:637`, `:645-647` |
| Everything else a mode set leaves behind | **IMPL** | #226 (`vesa()` case `0x02`, `vdd_video.c:1059-1121`). **The hazard that was here, read from the code and never seen on a guest:** 4F02 touched neither `mkind` nor the sequencer/GC shadows, so after mode 12h `vdd_video_planar_active()` still said 1 and the host (`main.c` `video_trap_sync`) kept running the guest in its interpreter, whose A0000 stores go through `vga_planar_write()` into the planes — not into the window `vesa_sync` copies into `vesa_vram`; the host's A0000 mapping stayed on a plane section; write mode/bit mask stayed 12h's; `gw/gh`, the text geometry and `40:49h` stayed the old mode's. **Now**, as a VBE BIOS does (SeaVGABIOS, read from QEMU's `vgabios-stdvga.bin`: SR4 chain-4 on, GR5 256-colour, then the BDA): mode 13h's register file (`vga_load_modedef`), kind `LINEAR8` with chain-4 on and the host window back on the linear section (the INT 10h mode-13h arm's `ymap_select(-1)`), `gw/gh` = the VESA extent, and the BDA from `vga_set_mode()`: `40:49h = FFh` (a mode number > FFh; Bochs's older VGABIOS leaves `40:49h` alone), `40:4Ah = XRes/8`, `40:84h = YRes/char height − 1`, `40:85h` = the YCharSize 4F01 reports, cursors and page 0. ⚠ **Host-visible**: INT 33h's default range follows `gw/gh` (now the VESA mode's, was the previous mode's); `CLOSEPROG` re-modes when `mkind != TEXT`, so a VESA-mode parent that EXECs a child is now re-moded to `40:49h`'s FFh on the child's exit (it used to survive by the stale kind) — the host needs to save 4F03 and restore with 4F02 D15 there | `video_test.c:654-676` |
| The DAC palette on a mode set | **MISS** | nothing is loaded; `pal_refresh` (`:928`) only re-derives the presenter palette from whatever the previous mode left. Whether a VBE BIOS loads the default 256-colour DAC here is an **oracle question** (no probe asks it) | — |

## 6. 4F03h — Return Current VBE Mode (§4.6)

| Unit | Status | Where / what is missing | Verification |
|---|---|---|---|
| BX = the mode number of the last 4F02 (or the standard mode) | **IMPL** | `:943-946`; a VESA text mode is remembered until a standard set (`:898`, `:1201`) | `video_test.c:426-427`, `:442-443` |
| BX D14 (linear) and D15 (memory not cleared) | **IMPL** | #226: `vesa_mode_flags` keeps D14/D15 of the last 4F02 and case `0x03` ORs them back (`vdd_video.c:1127-1137`) — §4.6, "Version 2.x Note: … the memory clear flag will be returned". A VESA text mode reports D15; a standard mode reports its number with D15 from AL bit 7, as SeaVGABIOS's 4F03 does (§4.6 only promises accuracy after a 4F02). The save → 4F03 → 4F02 round trip now comes back linear, not banked | `video_test.c:636-647` |

## 7. 4F04h — Save/Restore State (§4.7)

One 896-byte block (`VID_STATE_BYTES`, `:669`) serves this and `INT 10h AH=1Ch`; its layout is
ours (`:654-668`).

| Unit | Status | Where / what is missing | Verification |
|---|---|---|---|
| DL=00h: buffer size in 64-byte blocks | **IMPL** | BX = 14 for any mask (`:934`); over-reporting is permitted | `video_test.c:314` |
| DL=01h: save | **PART** | writes mode, VESA state, the whole DAC at 8 bits, the attribute palette, cursor, page, CRTC start/offset (`:673-691`). **Not saved:** the rest of the VGA register file (sequencer, graphics controller, CRTC timing, Misc Output) that CX D0/D3 name, and the BIOS data area that CX D1 names beyond cursor and page | `video_test.c:328` (nothing past the size) |
| DL=02h: restore | **PART** | re-enters the saved mode with D15 set, then restores the fields above (`:693-717`); a buffer we did not write → `024Fh` (`:696`, `:938`). Restores **everything**, whatever CX asks | `video_test.c:333-337` |
| CX: requested states D0–D3 | **STORE** | written into the block at `+4` (`:677`) and never consulted | — |

## 8. 4F05h — Display Window Control (§4.8)

| Unit | Status | Where / what is missing | Verification |
|---|---|---|---|
| BH=00h set window A to DX (64 KB units) | **IMPL** | `:1141-1147` | `video_test.c:254-261`; rig: ZAR banked, 4,735 calls |
| BH=01h get window A → DX | **IMPL** | `:1148-1149` (this read BL as the selector until s74b) | `video_test.c:264-265` |
| BL=01h window B → `014Fh` | **IMPL** | `:1140` — WinBAttributes says there is none | `video_test.c:266-267` |
| DX past VRAM → `024Fh`, window unchanged | **IMPL** | `:1143` | `video_test.c:268-269` |
| In an LFB mode → `034Fh` (§4.8, "must fail") | **IMPL** | `:1139` | `video_test.c:270-272` |
| The window itself: A0000 as a 64 KB view of VRAM | **IMPL** | copied out on each switch and at each frame (`vesa_sync` `:596-604`, `:1144-1146`, `:3305-3306`) | `video_test.c:340-348` (a guest that writes and never switches again, vesacube) |
| The direct far-call window function (WinFuncPtr) | **MISS** | advertised NULL; §4.4 permits it, but a VBE 1.x program that calls WinFuncPtr without testing it jumps to `0000:0000`. #226 looked at doing it inside `vdd_video.c` — a 6-byte stub `mov ax,4F05h / int 10h / retf` beside the fonts at `B000:2600` (installed by `vdd_video_install_fonts`, RAM the mode-Y remap preserves) — and **left it**: a protected-mode client that sees a non-NULL pointer may start calling it through DPMI `0301h` instead of `0300h`, and whether the `0301h` run loop (`main.c` ≈`:22689`) services the INT 10h BOP the stub makes has not been checked; ZAR does 4,735 bank switches a run through this path. What the host would need: confirm (or add) INT 10h servicing inside `0301h`'s `v86_run`, then the stub is ~10 lines here | — |

## 9. 4F06h — Set/Get Logical Scan Line Length (§4.9)

| Unit | Status | Where / what is missing | Verification |
|---|---|---|---|
| BL=00h set in pixels (rounded up) | **IMPL** | `:965-975`; AH=02h if narrower than the mode or longer than VRAM can hold `h` rows of | `video_test.c:361`, `:369-371` |
| BL=01h get: BX bytes, CX pixels, DX lines | **IMPL** | `:981-984` | `video_test.c:357` |
| BL=02h set in bytes | **IMPL** | `:965-975` | `video_test.c:369` |
| BL=03h get maximum | **IMPL** | `:976-979` — limited by VRAM only; there is no CRTC offset-register ceiling | `video_test.c:365` |
| Valid in VBE text modes (§4.9 note) | **MISS** | a VESA text mode is not `in_vesa`, so `034Fh` (`:964`) | — |
| The pitch reaches the picture | **IMPL** | `vesa_stride` drives the 8bpp frame and the direct-colour conversion (`:628-635`, `:3321`) | `video_test.c:376` |

## 10. 4F07h — Set/Get Display Start (§4.10; 3.0 sub-functions noted)

| Unit | Status | Where / what is missing | Verification |
|---|---|---|---|
| BL=00h set (CX pixel, DX line) | **IMPL** | `:992-1003`; the frame origin moves (`vesa_origin` `:558-562`, `:3322`) | `video_test.c:376`, `:395`; rig: heaven7 |
| BL=01h get (BH=0, CX, DX) | **IMPL** | `:989-991` | `video_test.c:379` |
| BL=80h set during vertical retrace | **PART** | #226, VDD side done (`vesa()` case `0x07`, `vdd_video.c:1181-1241`; `vesa_vbl_release` `:2985`): the start goes to the register and the retrace loads it (`vid_latch`, the s83 schedule, now carrying `vesa_org` → `vesa_org_vs` → `vesa_org_live`); the call completes when the beam is in a retrace that has not already released such a call, else at the next retrace start (one release per retrace — a guest calling twice inside one retrace is paced to one flip a frame, as a `wait while in retrace; wait until in retrace` BIOS loop paces it). The beam is the VESA mode's own (§16). **PART because the wait is the host's to honour**: `int10()` runs under the host lock and cannot spin, so it stamps `int10_wait_until` and returns; ⚠ **`main.c` must, after `vdd_bus_deliver_int(...0x10...)` and `HOST_UNLOCK()` on each INT 10h path (V86 BOP ≈`:27904`, PM ≈`:21318`, DPMI `0300h` ≈`:22665`), loop on `vdd_video_int10_wait_us(&g_vid)` — yield, keep `g_dpmi_iter` alive — before advancing the guest.** Until then the call returns at once (as it always did) and only the pacing is missing; the start still appears from the retrace the call names | `video_test.c:553-624` (T12h, fake clock); vesacube uses it on the rig |
| A start that leaves no full page → `024Fh`, nothing changed | **IMPL** | `:997-1000`; refusals are counted (`vesa_07_rej`) | `video_test.c:382` |
| Valid in VBE text modes (§4.10 note) | **MISS** | `034Fh` (`:988`) | — |
| 3.0 BL=02h / 82h: schedule a start given as a byte address (82h waits) | **PART** | #226: ECX is the byte offset (the start is a byte offset internally now, so any depth's page flip is exact — 3.0's reason for these); same full-page refusal (`024Fh`); BL=01h reads it back as (x, y) at the current pitch. 02h returns at once and the latch takes it at the retrace — **IMPL**; 82h waits exactly as 80h, so it shares 80h's owed host loop — hence PART | `video_test.c:600-616` |
| 3.0 BL=04h: has the scheduled flip happened? | **IMPL** | #226: CX = 1 once the retrace has loaded the register (`vesa_org_vs == vesa_org`, after bringing the schedule up to now), else 0. (Harmless to a 2.0 caller, which never asks.) | `video_test.c:603-607` |
| 3.0 BL=03h/83h/05h/06h: stereoscopic starts and mode | **N/A** | no stereo hardware; ModeAttributes D11 and Capabilities D3 say so. Failing is correct — 3.0's implementation note: "the VBE implementation should fail functions 03h, 05h, 06h and 83h" | `video_test.c:617-618` |

## 11. 4F08h — Set/Get DAC Palette Format (§4.11)

| Unit | Status | Where / what is missing | Verification |
|---|---|---|---|
| BL=00h set BH bits per primary (next lower we have: 6 or 8) | **IMPL** | #226: the width is the RAMDAC's and every path obeys it through `dac_to8`/`dac_from8` (`vdd_video.c:46-57`): port `3C9h` writes and reads (`dac_out` `:1953`, `dac_in` `:1981`), INT 10h AH=10h AL=10h/12h/15h/17h (a VGA BIOS implements those as plain OUTs/INs, so on hardware they follow the width too), and 4F09. `dac[]` keeps 8 bits per primary (6-bit v stored as v<<2, as before), which is also how a switchable RAMDAC holds it — a width switch re-interprets, it does not rescale — so the renderer and the 4F04/AH=1Ch block are unchanged. Any mode set (AH=00h, both 4F02 arms) and power-on reset it to 6 (§4.11) | `video_test.c:493-551` (T12g: ports, AH=10h, 4F09, both resets) |
| BL=01h get | **IMPL** | `:1017-1019` | `video_test.c:277-278` |
| AH=03h in a direct-colour mode | **IMPL** | `:1013` | `video_test.c:302-304` |
| In a standard VGA mode (13h) or a VESA text mode | **IMPL** | #226: only a direct-colour VESA mode answers `034Fh` (§4.11: "failure code AH=03h if called in a direct color or YUV mode"); mode 13h, the text modes and the VESA text modes switch the same RAMDAC | `video_test.c:541-550` |

## 12. 4F09h — Set/Get Palette Data (§4.12)

| Unit | Status | Where / what is missing | Verification |
|---|---|---|---|
| BL=00h set primary palette | **IMPL** | both widths, and the presenter palette follows. #226: the 6-bit set masks each primary (§4.12: "the format of the 6 bits is LSB") through the same `dac_to8` the port uses — bits 6–7 used to spill into the neighbouring field | `video_test.c:283-290`, `:533-536` |
| BL=01h get primary palette | **IMPL** | `:1041-1046` | `video_test.c:292-293` |
| BL=02h/03h secondary palette → `024Fh` (none) | **IMPL** | `:1032`, as §4.12 prescribes | `video_test.c:294-295` |
| BL=80h set during retrace, blank bit on | **IMPL** | as 00h; Capabilities D2 = 0 says no blanking is needed | `video_test.c:537-540` |
| DX + CX past 256 → `024Fh`, nothing changed | **IMPL** | `:1034` | `video_test.c:296-297` |
| Entry format: Blue, Green, Red, Alignment | **IMPL** | `:1037-1046`. ⚠ VBE 2.0's text lists "Alignment, Red, Green, Blue"; VBE 3.0's `PaletteEntry` structure (and every BIOS) is B, G, R, alignment, which is what we do | `video_test.c:293` |

## 13. 4F0Ah — Return VBE Protected Mode Interface (§4.13; 3.0 PM entry)

§4.13 calls 4F0Ah "required". **Implemented by #53 (2026-10-02).** Both real BIOSes we can
execute decline it (`AX=0100h` -- SeaVGABIOS and the Tseng ET4000/W32p ROM, `p_vesapm`), and we
did too while there was no code to hand out; those rows now abstain with the reason. The block
is `src/vdd/vbe_pm.asm` (32-bit, relocatable: relative jumps, the stack and port I/O only),
assembled into `vbe_pm.h` by `tools/gen-vbepm.py`, written to `B260:0000` (after the fonts) at
start-up and again on every 4F0Ah call. It drives the card through our index/data pair
`01CEh`/`01CFh` (`vbe_port_in`/`_out` in `vdd_video.c`: index `05h` bank -- Bochs's number for
it -- `10h`/`11h` display start in dwords, high word commits; `03h`/`06h` read bpp and the line
in pixels; `00h` reads 0, so no Bochs driver mistakes us for one) and the VGA DAC/status ports.

| Unit | Status | Where / what is missing | Verification |
|---|---|---|---|
| BL=00h: ES:DI → table, CX = length; other BL → `014Fh` | **IMPL** | `vesa()` `case 0x0A`; `ES:DI = B260:0000`, `CX = VBE_PM_LEN` | off-VM `vbepm_test.c`; rig `p_vbepm` (`vbepm.4F0A`) |
| Table `+0`: 32-bit Set Window code (BH=00h, BL=window, DX=position) | **IMPL** | port `05h` → `vesa_set_bank`, the function 4F05h now shares; window B and out-of-range refused (counted `vbe_pm_rej`) | `vbepm_test.c` (copied, interpreted, compared with 4F05h get and the VRAM it flushed); rig `p_vbepm` (`vbepm.win.via.4F05`, `vbepm.vram.via.4F05`) |
| Table `+2`: 32-bit Set Display Start (BL=00h/80h, DX:CX = start in DWORDS) | **IMPL** | ports `10h`/`11h`; BL=80h polls `3DAh` bit 3 in the block itself before the commit | `vbepm_test.c` (4F07h get agrees; the retrace wait reads `3DAh`); rig `p_vbepm` (`vbepm.start.via.4F07`) |
| Table `+4`: 32-bit Set Primary Palette (BL=00h/80h, CX count, DX first, ES:EDI B,G,R,pad) | **IMPL** | `3C8h`/`3C9h`, so the 4F08h DAC width applies as for any port write | `vbepm_test.c` (4F09h get reads back the same bytes); rig `p_vbepm` (`vbepm.pal.via.3C9`) |
| Table `+6`: ports/memory sub-table | **IMPL** | `01CEh 01CFh 03C8h 03C9h 03DAh FFFFh`, empty memory list `FFFFh` | `vbepm_test.c` |
| 3.0: the `'PMID'` PMInfoBlock in the ROM image (PMInitialize, 16-bit PM entry) | **MISS** | we claim VBE 2.0; no `PMID` anywhere in `src/` | — |

⚠ **STAGE2 shows who uses it**: `VESA calls by sub-function: ... | 4F0A-block banks= starts=
refused=` -- a client switching banks through the block appears there and NOT in the 4F05
count.

## 14. VBE 3.0 4F0Bh and the supplemental functions (§5.7)

| Function | Unit | Status | Where / what is missing | Verification |
|---|---|---|---|---|
| 4F0Bh (3.0) | get closest pixel clock | **MISS** | `AX=0100h` (`:1153`) — the right answer for the 2.0 we claim | — |
| 4F10h VBE/PM | BL=00h report: version 1.0, all four states | **IMPL** | `:1070` | `video_test.c:465-467`; no oracle, no spec held |
| 4F10h | BL=01h set power state | **STORE** | kept in `vesa_pm_state` (`:1071`) and read back; **nothing blanks** — the code says so (`:1067-1068`) | `video_test.c:468-474` |
| 4F10h | BL=02h get power state | **IMPL** | `:1072` | `video_test.c:470-474` |
| 4F11h FP, 4F12h CI, 4F13h AI, 4F14h OEM, 4F16h GC | optional supplemental specifications | **N/A** | not offered; `AX=0100h` (`:1153`) is the correct "not supported" | — |
| 4F15h VBE/DDC | BL=00h capabilities (DDC1+DDC2, 1 s per block) | **IMPL** | `:1085` | `video_test.c:452-455`; no oracle, no spec held |
| 4F15h | BL=01h read EDID block 0 (synthesised EDID 1.3, valid checksum; block ≠ 0 → `014Fh`) | **IMPL** | `:1086-1124` | `video_test.c:456-463` |

## 15. The linear frame buffer

| Unit | Status | Where / what is missing | Verification |
|---|---|---|---|
| One constant for PhysBasePtr and the mapping | **IMPL** | `VID_VESA_LFB_PHYS` `vdd_video.h:56-63` | — |
| DPMI `0800h` maps the aperture | **PART** | answers the host address of `vesa_vram` (`main.c` `dpmi_service_pm_int_body`, `case 0x0800` ≈`:22423`). Accepted **only** for BX:CX exactly `E0000000h` and SI:DI ≤ 4 MB; a request at an offset into the aperture, or larger than 4 MB, is refused `8021h`. The selector a client then builds had to be fixed in `dpmi_install` (DPL forced to 3, ≈`:16617`) for ZAR | rig: heaven7 (`session-74.md:386`), ZAR (`:33-46`) |
| DPMI `0801h` free the mapping | **IMPL** | a successful no-op; the buffer is the VDD's (`main.c` ≈`:22442`) | untested |
| LFB writes reach the picture; the A0000 window is not copied over them | **IMPL** | `vesa_sync` returns early when `vesa_lfb` (`:601`); frame reads `vesa_vram` (`:3322`) | `video_test.c:411`, `:448`; rig: heaven7, ZAR |
| LFB for a real-mode or non-DPMI (VCPI, raw) client | **N/A** | `E0000000h` is unreachable from V86 mode; only a DPMI client can map it | — |

## 16. Presentation

| Unit | Status | Where / what is missing | Verification |
|---|---|---|---|
| 8bpp packed: pixel indexes the DAC directly (no EGA attribute remap) | **IMPL** | `pal_refresh` `:152-158`; frame at `:3315-3322` | `video_test.c:287-290`, `:340-342`; rig: vesacube, ZAR |
| 15bpp 5:5:5 → ARGB, bit replication | **IMPL** | `vesa_to_argb` `:638-641` | untested beyond the code |
| 16bpp 5:6:5 → ARGB | **IMPL** | `:642-645` | `video_test.c:395`; rig: heaven7 `0x4170`, vesacube 320x240x16 |
| 24bpp 8:8:8 (B, G, R in memory) → ARGB | **IMPL** | `:646-647` | `video_test.c:411`, `:448` |
| 32bpp 8:8:8:8 | **MISS** | no mode advertises it; `vesa_bypp` knows 4 bytes (`:555`) but `vesa_to_argb` treats every depth above 16 as 24-bit (`:646`) | — |
| Logical pitch (4F06) and display start (4F07) applied to every depth | **IMPL** | the displayed start is `vesa_org_live` (the retrace-loaded one, #226) in both the 8bpp frame and the direct-colour conversion | `video_test.c:376-395`, `:553-624` |
| The display timing of a VESA mode (3DAh, the 4F07 retrace wait, the present scheduler) | **IMPL** | #226: `vesa_vgeom` (`vdd_video.c:621`) — the VESA DMT set at 60 Hz (525/628/806/1066 total lines for 480/600/768/1024), 400-line and double-scanned 200-line modes on the VGA's 449-line 70 Hz frame, 240-line double-scanned to 480; blanking from the end of the picture. It was whatever CRTC timing the last **standard** mode left (text: 70 Hz; mode 12h: 60 Hz). Used by `vid_beam`, `vdd_video_frame_us` and `vdd_video_present_ready` alike | `video_test.c:559-566` |
| Frame cap 1280x1024, shared by the mode list, the presenter and its guard | **IMPL** | `ntvdd.h:61-62`, `vdd_video.h:64-65`, `present_ddraw.c:533-535` | `video_test.c:400-416` |

## 17. The mode list

The list 4F00 publishes is `vesa_modes[]` (`:508-532`) then `vesa_text_modes[]` (`:540-543`).
Every entry fits the 4 MB VRAM (`vesa_find` refuses any that does not, `:585`) and the 1280x1024
presenter cap, so **every advertised mode can be set and presented**. *Pages* is the
NumberOfImagePages value 4F01 returns (pages − 1), derived here from `:839-841` — arithmetic on
the code, not a measurement. *Shown on the rig* lists what a guest actually displayed there.

| Mode | Spec | Geometry | Depth / model | Pitch (bytes) | Pages | Status | Shown on the rig |
|---|---|---|---|---|---|---|---|
| `100h` | VBE | 640x400 | 8, packed | 640 | 15 | **IMPL** | — |
| `101h` | VBE | 640x480 | 8, packed | 640 | 12 | **IMPL** | vesacube; ZAR banked and LFB (`0x4101`) |
| `103h` | VBE | 800x600 | 8, packed | 800 | 7 | **IMPL** | — |
| `105h` | VBE | 1024x768 | 8, packed | 1024 | 4 | **IMPL** | — (off-VM `video_test.c:403-407`) |
| `107h` | VBE | 1280x1024 | 8, packed | 1280 | 2 | **IMPL** | — (off-VM `:445`) |
| `10Dh` | VBE | 320x200 | 15 (1:5:5:5), direct | 640 | 31 | **IMPL** | — |
| `10Eh` | VBE | 320x200 | 16 (5:6:5), direct | 640 | 31 | **IMPL** | — |
| `10Fh` | VBE | 320x200 | 24 (8:8:8), direct | 960 | 20 | **IMPL** | — |
| `110h` | VBE | 640x480 | 15, direct | 1280 | 5 | **IMPL** | — |
| `111h` | VBE | 640x480 | 16, direct | 1280 | 5 | **IMPL** | — (off-VM `:395`) |
| `112h` | VBE | 640x480 | 24, direct | 1920 | 3 | **IMPL** | heaven7 (`0x4112`, before `0x4170` existed) |
| `113h` | VBE | 800x600 | 15, direct | 1600 | 3 | **IMPL** | — |
| `114h` | VBE | 800x600 | 16, direct | 1600 | 3 | **IMPL** | — |
| `115h` | VBE | 800x600 | 24, direct | 2400 | 1 | **IMPL** | — |
| `116h` | VBE | 1024x768 | 15, direct | 2048 | 1 | **IMPL** | — |
| `117h` | VBE | 1024x768 | 16, direct | 2048 | 1 | **IMPL** | — |
| `118h` | VBE | 1024x768 | 24, direct | 3072 | 0 | **IMPL** | — (off-VM `0x4118`, `:411`) |
| `119h` | VBE | 1280x1024 | 15, direct | 2560 | 0 | **IMPL** | — |
| `11Ah` | VBE | 1280x1024 | 16, direct | 2560 | 0 | **IMPL** | — |
| `11Bh` | VBE | 1280x1024 | 24, direct | 3840 | 0 | **IMPL** | — (off-VM `0x411B`, `:448`) |
| `151h` | OEM (S3 numbering) | 320x240 | 8, packed | 320 | 53 | **IMPL** | — |
| `160h` | OEM (S3) | 320x240 | 15, direct | 640 | 26 | **IMPL** | — |
| `170h` | OEM (S3) | 320x240 | 16, direct | 640 | 26 | **IMPL** | heaven7 default (`0x4170`); vesacube |
| `108h` | VBE | 80x60 text, 8x8 cell (640x480) | text | 160 | 2 | **IMPL** | — (off-VM `:440`) |
| `109h` | VBE | 132x25 text, 8x16 (1056x400) | text | 264 | 3 | **IMPL** | — (off-VM `:420-434`) |
| `10Ah` | VBE | 132x43 text, 8x8 (1056x344) | text | 264 | 1 | **IMPL** | — |
| `10Bh` | VBE | 132x50 text, 8x8 (1056x400) | text | 264 | 1 | **IMPL** | — |
| `10Ch` | VBE | 132x60 text, 8x8 (1056x480) | text | 264 | 1 | **IMPL** | — (off-VM `:437`) |
| `102h` | VBE | 800x600, 16 colours | 4, planar | — | — | **MISS** | not in the list |
| `104h` | VBE | 1024x768, 16 colours | 4, planar | — | — | **MISS** | not in the list |
| `106h` | VBE | 1280x1024, 16 colours | 4, planar | — | — | **MISS** | not in the list |
| `6Ah` | VBE (7-bit twin of `102h`, set by `INT 10h AH=00h`) | 800x600, 16 colours | planar | — | — | **MISS** | not a standard mode we define either (`video-bios.md`'s surface) |
| `81FFh` | VBE special: all of VRAM, contents preserved; needs a ModeInfoBlock, must not be listed | — | packed | — | — | **MISS** | 4F01 and 4F02 both refuse it (`:578`: `1FFh` is not in the table) |
| Standard VGA modes via 4F02 (BH=0, BL=mode) | §3.0 "may be initialized through VBE Function 02h" | — | — | — | — | **MISS** | `014Fh`. Permitted while they are not in our list (§4.5), but a guest that sets 13h through 4F02 is refused |

---

## Gaps worth closing (most important first)

**Closed by #226** (off-VM; rig gate owed): the old items 1 (4F08 width at the ports), 2 (4F02's
VGA layer, `mkind`, `40:49h`, D5), 3 (4F07 BL=80h — VDD half), 4 (4F03 D14/D15, `40:87h` bit 7),
8 (the 2.0 OEM strings in OemData), 4F08 in standard modes (half of old 11) and 4F07 BL=02h/04h/82h
(part of old 13). What remains, re-ranked:

1. **The host's half of 4F07 BL=80h/82h** (§10). The VDD stamps `int10_wait_until`; `main.c` must
   loop on `vdd_video_int10_wait_us(&g_vid)` after `HOST_UNLOCK()` on each INT 10h delivery path
   (V86 BOP, PM INT 10h, DPMI `0300h`) before advancing the guest, yielding and keeping the
   watchdog (`g_dpmi_iter`) fed. Until it does, the call returns at once and paces nothing; the
   start itself already lands on the retrace. Also worth printing `vesa_07_waits`/`vesa_07_wait_us`
   in the STAGE2 VESA lines.
2. **Rig re-gate for #226** — heaven7 (4F07 BL=80h idiom, `0x4170`), ZAR (banked `0x101` and LFB
   `0x4101`; INT 33h range now follows the VESA extent), vesacube (8bpp palette through 4F09, page
   flips), plus Skyroads/Win16 per the shared-path rule, since `vid_beam`/`vid_latch` changed.
   ModeAttributes D5 is the one change a guest could refuse a mode over; rollback is one constant.
3. **`CLOSEPROG` and a VESA parent** (§5). The host re-modes on `mkind != TEXT` from `40:49h`; with
   a VESA parent that is now FFh. It should save 4F03 at EXEC and restore with 4F02 | D15.
4. **Put the runtime half under an oracle.** `p_vesa`/`p_vesapm` cover 4F00, 4F01 and 4F0A only;
   4F02–4F09 are pinned solely by `video_test.c`, which was written against the PDF and our
   own model. A `p_vesarun` probe (4F02 flags, 4F03 round trip, `40:49h`/`40:87h` after 4F02, 4F06
   maxima, 4F07 refusals, 4F08 then a port read-back, 4F04 sizes) against the Tseng ROM and
   SeaVGABIOS would turn the #226 rows into comparisons — and answers the open
   DAC-on-mode-set question (§5).
5. **The far-call bank switch** (§8). 4F0Ah's protected-mode block exists since #53 (§13);
   WinFuncPtr is still NULL (legal, but a VBE 1.x program that calls it blindly jumps to
   `0000:0000`). The real-mode stub can now be a few bytes doing the same `01CEh`/`01CFh`
   writes as the PM block's SetWindow.
6. **INT 10h AH=0Fh AL bit 7** — the IBM BIOS returns `40:87h` bit 7 there (VBE 2.0 §4.6's 1.x
   note). The bit is now maintained; AH=0Fh does not report it yet (`video-bios.md`'s surface).
7. **DPMI `0800h` at an offset into the aperture** (§15): accept any `[E0000000h, +4 MB)`
   range, not only its exact base.
8. **OffScreenMemOffset/Size** (§4): report the VRAM beyond the displayed page to a 2.0
   caller, or claim VBE 3.0 (where the bytes are reserved) together with its fields. ⚠ `p_vesa`
   grades these bytes and both oracles say 0 — changing them moves parity.
9. **Missing modes and depths** (§16, §17): the planar 16-colour VESA modes `102h`/`104h`/
   `106h` (and `6Ah`), 32bpp 8:8:8:8 modes (the converter needs a 32-bit arm too), and
   `81FFh`.
10. **4F06/4F07 in VESA text modes** (§9–§10): each answers `034Fh` where the spec expects the
    call to work; needs the text renderer to take a pitch and a start.
11. **4F04 coverage** (§7): honour the CX mask and save the full VGA register file and the
    BIOS data area it names.
12. **VBE 3.0**, only if we choose to claim it: 4F02 D11 with a CRTCInfoBlock, 4F0Bh,
    MaxPixelClock, the `PMID` block (4F07 BL=02h/04h/82h are already answered).
