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
**Off-VM:** `tools/dostest/video_test.c` T9–T12f (`:215-474`) and the 4F0A check at `:485-486`.
**DOS probes:** `tools/dostest/p_vesa.asm` (4F00 block and every ModeInfoBlock, bytes 0–49) and
`tools/dostest/p_vesapm.asm` (4F0Ah), both `ORACLE-ALSO: pcem-vesa`. **No probe exercises
4F02–4F09** — the runtime half has no oracle at all.
**Marked:** 2026-09-29, **from the code**, with citations. ⚠ `src/host/main.c` was being edited
in the working tree while this was written; its citations give the **function name** and an
approximate line — search for the symbol.

---

## Headline

**The information half is complete and oracle-checked; the runtime half is implemented to the
letter of the call but not always to its effect.** 4F00 and 4F01 were diffed field by field
against two real VBE implementations in s74 (a Tseng ET4000/W32p ROM under PCem and QEMU's
Bochs VBE) and every graded row agreed. 4F02–4F09 are implemented, spec-tested off-VM, and used
by three guests on the rig (heaven7, ZAR, vesacube) — but four of them do less than they
report: **4F08's 8-bit DAC width reaches 4F09 and nothing else** (ports `3C7h`/`3C9h` still mask
to 6 bits), **4F07 BL=80h never waits for the retrace it names**, **4F03 drops the LFB and
don't-clear flags**, and **4F02 programs no VGA register and leaves the previous mode's kind in
place**, while 4F01 tells every mode it is VGA-compatible. There is no protected-mode or far-call
bank switch of either kind (4F0Ah, WinFuncPtr).

| Group | Units | IMPL | PART | STORE | MISS | N/A |
|---|---|---|---|---|---|---|
| §1 Calling convention and status | 3 | 3 | — | — | — | — |
| §2 4F00h VbeInfoBlock | 16 | 11 | 4 | — | 1 | — |
| §3 4F01h the call | 4 | 4 | — | — | — | — |
| §4 4F01h ModeInfoBlock fields | 41 | 37 | 3 | — | 1 | — |
| §5 4F02h set mode | 10 | 5 | 2 | — | 3 | — |
| §6 4F03h current mode | 2 | 1 | — | — | 1 | — |
| §7 4F04h save/restore state | 4 | 1 | 2 | 1 | — | — |
| §8 4F05h window control | 7 | 6 | — | — | 1 | — |
| §9 4F06h logical scan line | 6 | 5 | — | — | 1 | — |
| §10 4F07h display start | 8 | 3 | 1 | — | 3 | 1 |
| §11 4F08h DAC width | 4 | 2 | 1 | — | 1 | — |
| §12 4F09h palette data | 6 | 5 | 1 | — | — | — |
| §13 4F0Ah protected-mode interface | 6 | — | — | — | 6 | — |
| §14 VBE 3.0 4F0Bh and the supplemental functions | 7 | 4 | — | 1 | 1 | 1 |
| §15 The linear frame buffer | 5 | 3 | 1 | — | — | 1 |
| §16 Presentation | 7 | 6 | — | — | 1 | — |
| §17 The mode list | 34 | 28 | — | — | 6 | — |
| **Total** | **170** | **124** | **15** | **2** | **26** | **3** |

### What has been measured on this surface

| Measurement | Where |
|---|---|
| `p_vesa` against QEMU's Bochs VBE and the Tseng ET4000/W32p ROM (`pcem-vesa`): **131 rows, 128 comparable, 100 % agree, 3 abstained** (2026-09-17). ⚠ A recorded score, not a re-run; the rules that shape it are `oracle-rules.json:1235-1336` | `session-74.md:222-236` |
| Found by that oracle and fixed: Capabilities D0 was 0; NumberOfImagePages was 0; YCharSize 8 in 200-line modes; DirectColorModeInfo D1 for 5:5:5; Lin/Bnk image pages | `session-74.md:226-229` |
| `p_vesapm`: 4F0Ah answers `AX=0100` on both real BIOSes, as we do | `session-74.md:246-249` |
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
| `+06h` | OemStringPtr → `"NTVDMEX VESA"` | **PART** | points at `+22h` of the caller's block for **both** callers (`:746`, `:760`). §4.3: a 2.0 BIOS "must place this string in the **OemData** area" (`+100h`) when `'VBE2'` was preset. For a 1.x caller the string lives in a buffer the caller owns, so it is gone once the caller reuses it; real ROMs point into ROM | pointer masked by the probe; `video_test.c:229` (inside the block) |
| `+0Ah` | Capabilities D0 = 1: DAC switchable to 8 bits | **IMPL** | `:751` — but see §11: the switch reaches 4F09 only | `video_test.c:248`; found by the oracle (`session-74.md:226`), ungraded card property |
| `+0Ah` | Capabilities D1 = 0: VGA-compatible controller | **IMPL** | `:751` | compared, ungraded |
| `+0Ah` | Capabilities D2 = 0: no blank bit needed for 4F09 | **IMPL** | `:751`; consistent with 4F09 BL=80h being treated as 00h (§12) | compared, ungraded |
| `+0Ah` | Capabilities D3–D4 (3.0): stereo signalling, EVC connector = 0 | **IMPL** | `:751` (none, truthfully) | — |
| `+0Eh` | VideoModePtr → list in the Reserved area | **IMPL** | `:752`, `:761-765`; §4.3 names the Reserved area for exactly this | `video_test.c:227-229` |
| — | Mode list terminator `FFFFh`; list never empty (not a "stub") | **IMPL** | `:766`; 28 entries (§17) | `p_vesa` `vesa.modes` (information only, `oracle-rules.json:1260`) |
| `+12h` | TotalMemory = 64 (4 MB) | **IMPL** | `:753`, `VID_VESA_VRAM` `vdd_video.h:55` | compared, ungraded |
| `+14h` | OemSoftwareRev `0100h` (2.0 callers only) | **IMPL** | `:755` | compared, ungraded |
| `+16h` | OemVendorNamePtr | **PART** | points at the **OEM string** (`:756`), not a vendor name, and not in OemData | masked by the probe |
| `+1Ah` | OemProductNamePtr | **PART** | the same OEM string (`:757`) | masked |
| `+1Eh` | OemProductRevPtr | **PART** | the same OEM string (`:758`) | masked |
| `+100h` | OemData (256 bytes, 2.0 callers) | **MISS** | zero-filled (`:743`); none of the three strings §4.3 says are copied here is copied here | — |

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
| D5 | 0 = VGA-compatible mode: "the standard VGA I/O ports … can be assumed" | **PART** | claimed for every mode (`:800`), but 4F02 programs **no** VGA register (§5): CRTC, sequencer and graphics controller keep the previous mode's values | ungraded |
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
| D14: linear (1) / windowed (0) frame buffer | **PART** | graphics modes IMPL (`:911`; `vesa_sync` stops copying the window, `:601`). A **text** mode with D14 set is accepted (`:883`); §4.5: "If D14 is set, and a linear frame buffer model is not available then the call will fail" | `video_test.c:270-272`, `:411`; rig: `0x4101`, `0x4170` |
| D15: don't clear display memory | **IMPL** | graphics `:924-927` (both VRAM and the A0000 window); text via AL bit 7 and `clear_text` `:893`, `:897` | untested (the standard-BIOS twin is in `video-bios.md`) |
| An unavailable mode fails `014Fh` and leaves the environment unchanged | **IMPL** | `:930`; nothing is written before the lookup succeeds | untested |
| A mode set resets: DAC width 6 (§4.11), bank 0, display start (0,0), pitch = the mode's own | **IMPL** | `:913-916` | `video_test.c:298-301` (DAC), `:389` (pitch and start) |
| BDA `40:87h` bit 7 — the memory-clear flag 2.0 BIOSes "should also update" | **MISS** | `vdd_video_bda_sync` writes `60h` unconditionally (`:444`) | — |
| Everything else a mode set leaves behind | **PART** | ⚠ **no VGA register is programmed and the mode *kind* is not changed**: `:905-929` touch neither the CRTC/sequencer/GC nor `mkind` (the code says so, `:152-156`), and `40:49h` keeps the previous standard mode (`:409`). After a planar mode (12h) the host's planar interpreter stays armed, because it keys on `mkind == VID_KIND_PLANAR` (`main.c` `video_trap_sync` ≈`:14004`), so A0000 stores in a banked VESA mode could be routed into the planes. **Not observed on any guest — a hazard read from the code**, and it contradicts ModeAttributes D5 (§4) | untested |
| The DAC palette on a mode set | **MISS** | nothing is loaded; `pal_refresh` (`:928`) only re-derives the presenter palette from whatever the previous mode left. Whether a VBE BIOS loads the default 256-colour DAC here is an **oracle question** (no probe asks it) | — |

## 6. 4F03h — Return Current VBE Mode (§4.6)

| Unit | Status | Where / what is missing | Verification |
|---|---|---|---|
| BX = the mode number of the last 4F02 (or the standard mode) | **IMPL** | `:943-946`; a VESA text mode is remembered until a standard set (`:898`, `:1201`) | `video_test.c:426-427`, `:442-443` |
| BX D14 (linear) and D15 (memory not cleared) | **MISS** | the mode is stored `& 3FFFh` (`:907`), so both flags are dropped. §4.6, "Version 2.x Note: … the memory clear flag will be returned". A guest that saves 4F03 and re-sets it with 4F02 comes back **banked**, and `vesa_sync` then paints the stale A0000 window over the first 64 KB of the LFB it keeps drawing into (`:596-604`) | — |

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
| The direct far-call window function (WinFuncPtr) | **MISS** | advertised NULL (`:804`); §4.4 permits it, but a VBE 1.x program that calls WinFuncPtr without testing it jumps to `0000:0000` | — |

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
| BL=80h set during vertical retrace | **PART** | treated exactly as 00h (`:992`): the call **returns at once** and the new start shows at the next present. A guest that uses it to wait for the retrace — heaven7's 15,900 `(0,0)` calls are that idiom (`session-74.md:165-168`) — is not paced by it | `video_test.c:386` (origin only); vesacube uses it on the rig |
| A start that leaves no full page → `024Fh`, nothing changed | **IMPL** | `:997-1000`; refusals are counted (`vesa_07_rej`) | `video_test.c:382` |
| Valid in VBE text modes (§4.10 note) | **MISS** | `034Fh` (`:988`) | — |
| 3.0 BL=02h / 82h: schedule a start given as a byte address (82h waits) | **MISS** | `014Fh` (`:1004`) | — |
| 3.0 BL=04h: has the scheduled flip happened? | **MISS** | `014Fh` | — |
| 3.0 BL=03h/83h/05h/06h: stereoscopic starts and mode | **N/A** | no stereo hardware; ModeAttributes D11 and Capabilities D3 say so. Failing is correct | — |

## 11. 4F08h — Set/Get DAC Palette Format (§4.11)

| Unit | Status | Where / what is missing | Verification |
|---|---|---|---|
| BL=00h set BH bits per primary (next lower we have: 6 or 8) | **PART** | the width is accepted and reported (`:1014-1016`) but **only 4F09 honours it** (`:1031`). Port `3C9h` still stores `value & 3Fh` (`dac_out` `:1704`) and reads back `>> 2` (`dac_in` `:1729-1731`), so a guest that switches to 8 bits and then programs the DAC by port — the common way — loses the top two bits of every primary (`80h` becomes 0) | `video_test.c:277-286` (the call, via 4F09 only) |
| BL=01h get | **IMPL** | `:1017-1019` | `video_test.c:277-278` |
| AH=03h in a direct-colour mode | **IMPL** | `:1013` | `video_test.c:302-304` |
| In a standard VGA mode (13h) or a VESA text mode | **MISS** | `034Fh` because the mode is not `in_vesa` (`:1013`); §4.11 reserves AH=03h for direct-colour/YUV modes only | — |

## 12. 4F09h — Set/Get Palette Data (§4.12)

| Unit | Status | Where / what is missing | Verification |
|---|---|---|---|
| BL=00h set primary palette | **PART** | IMPL at both widths, and the presenter palette follows (`:1036-1040`, `:1049`). In 6-bit mode a value above `3Fh` is shifted without masking, so bits 6–7 of green and blue spill into the low bits of red and green (`:1038-1040`); hardware ignores them. Off by at most 3/255 | `video_test.c:283-290` |
| BL=01h get primary palette | **IMPL** | `:1041-1046` | `video_test.c:292-293` |
| BL=02h/03h secondary palette → `024Fh` (none) | **IMPL** | `:1032`, as §4.12 prescribes | `video_test.c:294-295` |
| BL=80h set during retrace, blank bit on | **IMPL** | as 00h; Capabilities D2 = 0 says no blanking is needed | untested |
| DX + CX past 256 → `024Fh`, nothing changed | **IMPL** | `:1034` | `video_test.c:296-297` |
| Entry format: Blue, Green, Red, Alignment | **IMPL** | `:1037-1046`. ⚠ VBE 2.0's text lists "Alignment, Red, Green, Blue"; VBE 3.0's `PaletteEntry` structure (and every BIOS) is B, G, R, alignment, which is what we do | `video_test.c:293` |

## 13. 4F0Ah — Return VBE Protected Mode Interface (§4.13; 3.0 PM entry)

§4.13 calls 4F0Ah "required". Both real BIOSes we measured decline it as we do, so our answer
agrees with the oracles while the function is absent.

| Unit | Status | Where / what is missing | Verification |
|---|---|---|---|
| BL=00h: ES:DI → table, CX = length | **MISS** | `AX=0100h` (`:1053-1061`); flagged in `unimpl_fn` | **oracle** agrees (`session-74.md:246-248`, `p_vesapm`); `video_test.c:485-486` |
| Table `+0`: 32-bit Set Window code | **MISS** | — every bank switch from a protected-mode client is a DPMI `0300h` round trip (ZAR: 4,735) | — |
| Table `+2`: 32-bit Set Display Start code (DX:CX address form) | **MISS** | — | — |
| Table `+4`: 32-bit Set Primary Palette code | **MISS** | — | — |
| Table `+6`: ports/memory sub-table | **MISS** | — | — |
| 3.0: the `'PMID'` PMInfoBlock in the ROM image (PMInitialize, 16-bit PM entry) | **MISS** | no `PMID` anywhere in `src/` | — |

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
| Logical pitch (4F06) and display start (4F07) applied to every depth | **IMPL** | `:628-635`, `:3321-3322` | `video_test.c:376-395` |
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

1. **Make 4F08's 8-bit width reach the DAC ports** (§11). After `4F08 BH=8`, `3C9h` writes are
   still masked to 6 bits and reads still shifted right by 2 (`vdd_video.c:1704`, `:1729-1731`);
   INT 10h `AH=10h` palette calls likewise. Any guest that follows the spec — query
   Capabilities D0, set 8 bits, then program the DAC the usual way — gets wrong colours with a
   `004Fh` in hand. Capabilities D0 = 1 is currently a promise kept for 4F09 only.
2. **Give 4F02 the effects of a mode set** (§5). It programs no VGA register, leaves `mkind`
   and `40:49h` at the previous mode, and after mode 12h leaves the planar interpreter armed
   (`main.c` `video_trap_sync`), while ModeAttributes D5 tells the guest the mode is
   VGA-compatible. Setting a VESA mode from a planar setup screen is ordinary; it has not been
   observed going wrong, which is why this is a hazard and not a finding. A probe (and a
   `video_test` case: 12h, then 4F02 `101h`, then an A0000 store) settles it.
3. **Make 4F07 BL=80h wait for the vertical retrace** (§10). It returns at once today, so the
   standard VESA vsync/flip idiom does not pace anything — heaven7 calls it ~16,000 times a
   run for exactly that reason. The beam model already exists (`vid_beam`); the start should
   also latch at the retrace rather than at the next present.
4. **Return D14/D15 from 4F03 and set `40:87h` bit 7** (§6, §5). The 2.0 contract; without it
   a guest that saves and restores its mode with 4F03 → 4F02 comes back banked, and
   `vesa_sync` then overwrites the first 64 KB of the LFB it is still drawing into every frame.
5. **A direct bank-switch entry of either kind** (§8, §13). WinFuncPtr is NULL (legal, but a
   VBE 1.x program that calls it blindly jumps to `0000:0000`) and 4F0Ah is declined (as the
   two measured BIOSes also do). Every banked protected-mode frame pays a DPMI `0300h` round
   trip per bank. A real-mode far-call stub is small; the 4F0Ah code block is larger.
6. **Put the runtime half under an oracle.** `p_vesa`/`p_vesapm` cover 4F00, 4F01 and 4F0A only;
   4F02–4F09 are pinned solely by `video_test.c`, which was written against the PDF and our
   own model. A `p_vesarun` probe (4F02 flags, 4F03 round trip, 4F06 maxima, 4F07 refusals,
   4F08 then a port read-back, 4F04 sizes) against the Tseng ROM would turn every "untested"
   above into a comparison — and answers the open DAC-on-mode-set question (§5).
7. **DPMI `0800h` at an offset into the aperture** (§15): accept any `[E0000000h, +4 MB)`
   range, not only its exact base.
8. **The 2.0 OEM fields** (§2): with `'VBE2'` preset, copy the OEM, vendor, product and
   revision strings into OemData (`+100h`) as §4.3 requires, and give the last three their
   own text instead of the OEM string.
9. **OffScreenMemOffset/Size** (§4): report the VRAM beyond the displayed page to a 2.0
   caller, or claim VBE 3.0 (where the bytes are reserved) together with its fields.
10. **Missing modes and depths** (§16, §17): the planar 16-colour VESA modes `102h`/`104h`/
    `106h` (and `6Ah`), 32bpp 8:8:8:8 modes (the converter needs a 32-bit arm too), and
    `81FFh`.
11. **4F06/4F07 in VESA text modes and 4F08 in standard modes** (§9–§11): each answers
    `034Fh` where the spec expects the call to work.
12. **4F04 coverage** (§7): honour the CX mask and save the full VGA register file and the
    BIOS data area it names.
13. **VBE 3.0**, only if we choose to claim it: 4F02 D11 with a CRTCInfoBlock, 4F0Bh,
    MaxPixelClock, 4F07 BL=02h/04h/82h, the `PMID` block.
