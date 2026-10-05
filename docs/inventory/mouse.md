# Inventory — mouse: the INT 33h driver, and the pointing hardware under it

**Spec:** Microsoft *Mouse Programmer's Reference* (INT 33h); Ralf Brown's Interrupt List
(INT 33h, including the v7/v8 additions `25h`–`35h`); IBM PS/2 TechRef (the auxiliary
device on the 8042, INT 15h `AH=C2h`); the Microsoft serial-mouse protocol. ⚠ **Not held in
the repo** — [`../ref/SOURCES.md`](../ref/SOURCES.md). No `docs/ref/mouse.md` yet.
**Our implementation:** `mouse_int33` in `src/host/main.c` (≈`:8315-8569`), its state and
coordinate helpers (≈`:7642-8182`), the callback injector `mouse_cb_try` (≈`:8573`) and
`dpmi_inject_pm_mousecb` (≈`:25096`). The driver is **host code, not a VDD**: there is no
mouse device on the bus.
**DOS probes:** `tests/probes/dos/p_mouse.asm`, `p_mouse2.asm` (the v7/v8 calls, #249) and `p_mouse3.asm` (09h in VRAM, 2Bh-34h, 18h/19h -- #264/#265) (with `.pre`/`.deps`: the oracle loads the
real `MOUSE.COM` first). **Oracle:** Microsoft `MOUSE.COM` 6.24 on MS-DOS 6.22 under QEMU — a
**real driver**, so a row that agrees with it is **oracle**, not provisional.
**Marked:** 2026-10-01, **from the code**; §1 re-marked 2026-10-02 for #249 (`p_mouse2.asm`), and 2026-10-04 for #264/#265 (`p_mouse3.asm`, **not yet run on any oracle**). Carried over from `docs/PARITY.md` (retired
2026-09-23) and re-marked. ⚠ `main.c` line numbers drift; the `case` labels do not.

---

## Headline

**Every function `MOUSE.COM` 6.24 defines is answered, and the ones a probe can ask agree
with the real driver. Since #249 (2026-10-02) so does every v7/v8 call our `24h` = 8.00
entitles a guest to make -- or `32h` says plainly that it is absent.** What is left:

1. **`25h`–`34h` are answered** (`p_mouse2.asm`): `25h 26h 27h 2Ah 2Fh 30h 31h 32h` with
   the documented values, `28h`/`29h` with "cannot"/"none". Since #265 the acceleration
   profiles `2Bh`–`2Eh`, the settings block `33h` and the `.INI` name `34h` answer too
   (stored and handed back, **not applied** -- the same decision as `0Fh`), and `32h`
   reports all fourteen (`E7FFh`). ⚠ Their default contents and several register
   choices are UNMEASURED (`src/host/i33_driver.h` names each); `p_mouse3` asks.
   ⚠ The only executed voices are MOUSE.COM 6.24 (which predates them all) and DOSBox-X's
   built-in 8.05 driver; rows only the spec can grade are marked so.
2. **The sensitivity and mickey-ratio calls are N/A by decision.** `0Fh`, `13h` and the
   speeds of `1Ah` are stored and read back (and since #249 kept APART, as the real
   driver keeps them -- `1Bh` used to return `0Fh`'s ratio), but the pointer is the
   host's cursor and `0Bh` scales by the host's `msens`; applying them would change the
   pointer's feel in every game that sets them.
3. **`09h`'s bitmap IS drawn since #264** -- as an OVERLAY on the presenter's snapshot,
   not in video memory (a decision, recorded on the row). The arrow used to be stamped
   into the frame, which in mode 13h is the guest's own A0000 aperture: it wrote the
   pointer into guest VRAM and never restored it. Fixed with the same change.
4. **There is no pointing hardware.** No PS/2 auxiliary device behind the 8042, no
   INT 15h `AH=C2h`, no serial mouse on COM1. A guest that brings its own mouse driver
   finds nothing to drive.

| Group | Units | IMPL | PART | STORE | MISS | N/A |
|---|---|---|---|---|---|---|
| §1 INT 33h functions | 43 | 34 | 2 | 1 | — | 6 |
| §2 Event-handler delivery | 5 | 5 | — | — | — | — |
| §3 Pointing hardware | 4 | — | — | — | 4 | — |
| **Total** | **52** | **39** | **2** | **1** | **4** | **6** |

---

## 1. INT 33h functions

Entry: the vector is `DOS_HDLR_SEG:0030` = `BOP 33h; IRET` (`main.c:26988`). V86 arm
`main.c:28996-29000`, PM arm `:22258-22262`, DPMI `0300h` simulation `:23573`. Every entry
reaches the one `mouse_int33`. Coordinates live in a **virtual screen derived from the
mode** (`g_vid.gw/gh`, `i33_w`/`i33_h` `:7763-7791`), with text-mode 8-pixel cell
snapping (`i33_snap`).

| AX | Unit | Status | Where / what is missing | Verification |
|---|---|---|---|---|
| `0000h` | reset and status | **IMPL** | `:8366-8371`; `AX=FFFFh BX=2`; centres the pointer; `nomouse.flag` answers "no driver" (`:7712`) | **oracle** (`i33.00.reset`, `i33.03.afterreset`) |
| `0001h` | show cursor | **IMPL** | `:8372-8374`, hide count never below 0. The pointer is drawn by the host overlay, on 8-bpp frames only (`:12188`) — the real driver does not draw in VESA direct-colour modes either | **oracle** (`i33.01.show`, survivable) |
| `0002h` | hide cursor | **IMPL** | `:8375-8377` | untested |
| `0003h` | position and buttons | **IMPL** | `:8378-8382` | **oracle** (`i33.03.afterreset`) |
| `0004h` | set position | **IMPL** | `:8383-8388`, clamped and snapped | **oracle** (`i33.04.setpos`, `.odd`) |
| `0005h`/`0006h` | press / release counts | **IMPL** | `:8395-8412`; count drained by the read; position of the last transition, or the current one if none | **oracle** (`i33.05.press`, `i33.06.release`) |
| `0007h`/`0008h` | horizontal / vertical range | **IMPL** | `:8413-8424`, `i33_set_range` `:8082`; the pointer is re-clamped at once | **oracle** (`i33.07.clamp.hi`, `.lo`) |
| `0009h` | define graphics cursor | **IMPL** | #264: the 32 mask words are read from `ES:DX` (PM-aware, refused and counted if unreadable -- `gcbad=`) and DRAWN in every graphics mode: screen mask AND, cursor mask XOR, bit 15 leftmost, hot spot on the pointer (`i33_driver.h`, `ms_draw_gfx_cursor`). One bit a pixel, 16 wide; CGA 4-colour a bit PAIR a pixel, 8 wide, through the renderer's own palette table. A reset puts the host arrow back. The hot spot is still reported by `2Ah`. ⚠ **OVERLAY, NOT VRAM, by decision**: a real driver writes video memory (save-under / restore); from the host that would race a guest writing A0000 natively, so the bitmap is drawn on the presenter's private snapshot and a guest reading VRAM back never sees it. ⚠ UNMEASURED: the XOR value in 256-colour modes (we use DOSBox's `0Fh`), the hot spot's unit (screen pixels), CGA's 8-pixel bit-pair form | `p_mouse3` `i33.09.13h.*` (VRAM rows, the hidden restore, the hot-spot unit) -- **not yet run**; the subject is EXPECTED to differ on the `vram` rows; mouse_test.c pins our arithmetic |
| `000Ah` | define text cursor | **PART** | `BX=0` (software cursor, AND/XOR masks) is drawn (`vdd_video_text_cursor`); `BX=1` (use the hardware cursor, scan lines CX..DX) is recorded for `25h`/`27h` (#249) and drawn as the software default | **DOSBox-X** for the masks read back by `27h` (`i33.27.masks.set`) |
| `000Bh` | motion counters | **IMPL** | `i33_take_motion` (shared with `27h` since #249 -- one pair of accumulators, as the driver has); raw device counts when available, zeroed by the read, clamped to signed 16 bits | **oracle** (`i33.0B.motion`, `.again`) |
| `000Ch` | set event handler | **IMPL** | a flat PM client's 32-bit offset is kept whole (`mouse_i33_off`). Delivery: §2 | untested (QBasic, ZAR by hand) |
| `000Dh`/`000Eh` | light-pen emulation on / off | **N/A** | named no-ops (#249): no outputs, and there is no light pen for INT 10h `AH=04h` to report | -- |
| `000Fh` | mickeys per 8 pixels | **N/A** | stored, read back by nothing but the save state. ⛔ **Deliberately not applied**: the pointer IS the host cursor (absolute, Windows' own ballistics), and `0Bh` reports device counts scaled by `msens` -- applying a guest ratio on top would change pointer feel for every game that sets it (#249 kept movement unchanged). Since #249 it no longer overwrites `1Ah`'s speeds | **oracle** (`i33.0F.after`; `p_mouse2` `i33.0F.notsens`: `0Fh` leaves `1Bh` alone, MOUSE.COM 6.24 + DOSBox-X) |
| `0010h` | conditional-off region | **N/A** | accepted, no effect. The pointer is overlaid on the presenter's snapshot (since #264 -- before that, in 13h, it was written INTO guest VRAM), so there is no saved-under cursor to corrupt. ⚠ The pointer stays visible inside the region | -- |
| `0013h` | double-speed threshold (mickeys/s) | **N/A** | stored, not applied -- the same reason as `0Fh`. Its own variable since #249: `1Bh`'s `DX` is a different (0-100) value | **oracle** (`p_mouse2` `i33.13.vs.1B`) |
| `0014h` | exchange event handler | **IMPL** | returns the previous mask/segment/offset | untested |
| `0015h` | state buffer size | **IMPL** | `sizeof(i33_state)` | abstained (`i33.15.statesize`: each driver's private size) |
| `0016h`/`0017h` | save / restore state | **IMPL** | `i33_state_save`/`_load`; a versioned block (`'NV39'` since #249 added the speed/v8 fields); a bad pointer or foreign block is refused, not faulted (`i33_guest_ptr`) | untested -- self-consistent by construction, never round-tripped by a probe |
| `0018h`/`0019h` | set / get alternate (shift-qualified) handler | **IMPL** | #265: up to three handlers, one per Shift/Ctrl/Alt combination (bits 5-7 of `CX`); `18h` = `AX=0018h`, or `FFFFh` for a mask with no shift bit or a fourth combination; the same combination again REPLACES its slot. `19h` finds by the shift bits: `BX:DX` = the handler, `CX` = its mask, `CX=0` = none. A reset clears them. Delivered: §2. ⚠ UNMEASURED: replace-on-same-combination, 19h's match rule, the reset | `p_mouse3` `i33.18.*`, `i33.19.*` -- **not yet run** (MOUSE.COM 6.24 has 18h/19h, so the 6.22 oracle CAN answer); mouse_test.c |
| `001Ah` | set sensitivity | **IMPL** | stores the horizontal/vertical **speed** and the double-speed **speed** (0-100 each), read back by `1Bh` -- its own variables since #249 (they were `0Fh`'s and `13h`'s). Not applied, as `0Fh` | **oracle** (`i33.1B.sensitivity`; `p_mouse2` `i33.0F.notsens`) |
| `001Bh` | get sensitivity | **IMPL** | returns what `1Ah` stored; 50/50/50 after a reset (was 8/16/64) | **oracle** (`p_mouse2` `i33.1B.reset`, MOUSE.COM 6.24 + DOSBox-X) |
| `001Ch` | set interrupt rate | **STORE** | kept (codes 0-4) and reported in `25h` bits 11-8 (#249); events are delivered at the host's rate whatever it says | -- |
| `001Dh`/`001Eh` | set / get display page | **IMPL** | stored and read back (#249). The pointer is drawn by the host over whatever is displayed, so there is no per-page cursor to move | **oracle** (`p_mouse2` `i33.1E.page`) |
| `001Fh` | disable driver | **N/A** | answers the documented failure `AX=FFFFh`. We are the driver; there is no earlier INT 33h vector to hand back in `ES:BX` | untested |
| `0020h` | enable driver | **IMPL** | no outputs, no state change (#249). It shared `21h`'s arm, so enabling also RESET the ranges/handler/counts and answered `AX=FFFFh` | **oracle** (`p_mouse2` `i33.20.enable` `AX` untouched, `i33.20.keeps.range`) |
| `0021h` | software reset | **IMPL** | `i33_reset_state` | **oracle** (`i33.21.softreset`) |
| `0022h`/`0023h` | set / get language | **IMPL** | the US driver: `22h` accepted, `23h` = `0` (English) (#249) | **oracle** (`p_mouse2` `i33.23.language`) |
| `0024h` | version, type, IRQ | **IMPL** | `BX=0800h` (8.00), `CX=04FFh` (PS/2, IRQ `FFh` as the real driver says). ★ Since #249 every call 8.00 entitles a guest to make is either answered or reported absent by `32h` | **oracle** for `CX`; `BX` abstained (which `MOUSE.COM` is on the disk) |
| `0025h` | general driver information | **IMPL** | `AX` = integrated driver (bit 14), cursor type (13-12: software text / hardware text / graphics, from the mode and `0Ah`), `1Ch`'s rate (11-8), no display drivers (7-0) -- `4300h` in mode 3; `BX`/`CX`/`DX` = 0 (#249) | **DOSBox-X** for `BX`-`DX`; `AX` abstained (DOSBox-X answers a constant `4101h`) |
| `0026h` | maximum virtual coordinates | **IMPL** | the range maxima (as DOSBox-X: `p_mouse2` `i33.26.fenced`) | abstained -- `MOUSE.COM` 6.24 lacks `26h`; checked for internal consistency |
| `0027h` | screen/cursor masks and mickey counts | **IMPL** | `AX`/`BX` = the text masks (or `0Ah BX=1`'s scan lines), `CX`/`DX` = mickeys since the last read, drained -- the same counters as `0Bh` (#249) | **DOSBox-X** (`i33.27.masks.reset`, `.set`) |
| `0028h`/`0029h` | set / enumerate video modes | **PART** | answered (#249): `28h` `CL=FFh` (the driver has no mode list; INT 10h sets modes), `29h` `CX=0` (end of list, `DX=0`; `DS` is not written -- it would be loaded into a PM caller's selector). Not claimed by `32h` | **DOSBox-X** for `29h` |
| `002Ah` | cursor hot spot | **IMPL** | `AX` = the MS visibility counter (0 shown, negative hidden), `BX`/`CX` = `09h`'s hot spot (0,0 after a reset), `DX=4` (PS/2) (#249) | **DOSBox-X** (`i33.2A.*`) |
| `002Bh`-`002Eh` | acceleration profiles | **IMPL** | #265, RBIL's contracts: `2Bh` loads a 144h-byte block from `ES:SI` and makes `BX` (1-4) active, `FFFFh` restores the defaults; `2Ch` = `AX=0`, `BX` active, `ES:SI` -> the block; `2Dh` selects `BX` (1-4) or only asks (`FFFFh`), `ES:SI` -> the active name, `FFFEh` for an invalid `BX`; `2Eh` sets the four names from `ES:SI` (`BL=0`) or restores and returns the defaults (`BL!=0`). Pointers into the driver are `VDD_MOUSE_SEG` (B270h), a selector over it for a PM caller. ⛔ **Stored, not applied** -- as `0Fh`. ⚠ UNMEASURED: the default curves/names (ours: "1.0 everywhere", MS 8.x's control-panel names), the default profile (1), `2Bh`'s failure value, `2Eh BL!=0` | `p_mouse3` `i33.2B.*`-`i33.2E.*` -- **not yet run**; only an 8.x driver can answer (6.24 predates them) |
| `002Fh` | mouse hardware reset | **IMPL** | `AX=FFFFh` (done); the driver's state is untouched (#249) | spec only (RBIL); DOSBox-X lacks it |
| `0030h` | BallPoint information | **IMPL** | `AX=FFFFh`, "no BallPoint attached" (#249) | spec only (RBIL); DOSBox-X lacks it |
| `0031h` | current min/max virtual coordinates | **IMPL** | `AX`/`BX` = minima, `CX`/`DX` = maxima of the `07h`/`08h` range (#249) | **DOSBox-X** (`i33.31.range`) |
| `0032h` | active advanced functions | **IMPL** | `AX=E7FFh` since #265 (was `E43Ch`) -- bit 15 = `25h` ... bit 0 = `34h`: every one but `28h`/`29h`; `BX`/`CX`/`DX` = 0 (#249). It returned the caller's `AX` (`0032h`) as the bitmask | spec only -- both executed voices lack it (MOUSE.COM 6.24 predates it; DOSBox-X claims 8.05 and returns the poison) |
| `0033h` | switch settings and acceleration data | **IMPL** | #265: `CX` = the buffer size, `ES:DX` -> it; `AX=0`, `CX` = bytes written (at most 154h; a short buffer gets the head of the block). A 16-byte header (type 4, language 0, `1Ah`'s speeds, the active profile, `1Ch`'s rate, the rest 0) then the profile block. ⚠ UNMEASURED: each header byte's coding, truncation vs refusal | `p_mouse3` `i33.33.*` -- **not yet run** |
| `0034h` | `MOUSE.INI` path | **IMPL** | #265: `AX=0`, `ES:DX` -> `"MOUSE.INI"` (ASCIIZ, no path). There is no such file -- the name is the one a guest would look for, and opening it fails as on a machine without one. ⚠ UNMEASURED (a real driver names the file it read) | `p_mouse3` `i33.34.*` -- **not yet run** |
| `0035h` | LCD large pointer | **N/A** | LCD-panel driver support only | -- |
| `53C1h` | Logitech CyberMan / SWIFT probe | **IMPL** | `:8557-8559`, `AX=0` = "no SWIFT support". Doom asks and prints the answer | untested |
| any other | — | **IMPL** | `:8567`: counted in `g_ms_i33_unimpl`, registers untouched; the site and `AX` are recorded (`:8321-8351`) | — |

## 2. Event-handler delivery

| Unit | Status | Where | Verification |
|---|---|---|---|
| Event bits 0–6: motion, L/R/M press and release | **IMPL** | `mouse_evt_raise` `:7898-7916`, raised at `:7933`, `:7936`, `:12734`, `:12801` | untested |
| One callback per event, in order, with that moment's buttons and position; motion coalesced, buttons never | **IMPL** | 32-entry ring (`MS_EVQ`, `:7875-7878`) | untested |
| V86 delivery: a far call with `AX`=condition, `BX`=buttons, `CX/DX`=position, returning to `BOP 35h` | **IMPL** | `mouse_cb_try` `:8573-8652`; return stub `DOS_HDLR_SEG:00E0` (`:7859`), re-verified before every call | by hand (QBasic) |
| PM delivery to a DPMI client's handler | **IMPL** | `dpmi_inject_pm_mousecb` `:25096` | by hand (ZAR, s74c) |
| `18h` handlers: an event with Shift/Ctrl/Alt held goes to the handler for EXACTLY that combination (if it asked for the event), `AX` = event bits \| shift bits; otherwise to `0Ch`'s. One call per event, both paths (`mouse_evq_take`, `i33_pick`); the shift state is BDA `0040:0017` read at delivery. ⚠ UNMEASURED: exact match, one-call-per-event | **IMPL** (#265) | `mouse_evq_take` | mouse_test.c only -- a probe cannot Shift-click; no game known to use it |

## 3. Pointing hardware

| Unit | Status | Notes |
|---|---|---|
| PS/2 auxiliary device on the 8042 (`D4h` write-to-aux, `AUXB` status, IRQ12) | **MISS** | `kbd_hw_out` handles `A7h`/`A8h` as command-byte bits only (`vdd_input.c:605-606`); `D4h` is not decoded and nothing raises IRQ12 |
| INT 15h `AH=C2h` PS/2 pointing-device BIOS | **MISS** | falls to the INT 15h unimplemented arm, `AH=86h CF=1` (`main.c:29193-29208`) |
| Serial mouse on COM1 (the `'M'` identification, 3-byte packets) | **MISS** | `vdd_comm.c` has no mouse source; the host never feeds pointer data into a UART. ([uart.md](uart.md) records PCem's `'M'` byte) |
| A guest that loads its **own** `MOUSE.COM` | **MISS** | follows from the three above: its INT 33h replaces ours and then finds no device. Nobody has tried it |

---

## Measured history (kept)

### ★ The gap the probe found and closed (s72)

**The driver's virtual screen was derived from the PRESENT SURFACE, not the video
mode.** Every helper used `g_vid.frame.w/h` — the dimensions of the snapshot the UI
thread presents. That surface does not exist until something has been drawn, so in a
headless run (and in the window between a mode set and the first present)
`frame.h == 0`, and then:

* `i33_text()` evaluated **false** in a genuine text mode (`0 > 200`), so the whole
  640x200 text-mode scaling never engaged;
* `i33_vmaxy()` fell back to **479**, in a twenty-five-row text screen.

Measured: after a reset in mode 3 the real driver reports `y=96`; we reported `240`
— off the 640x200 virtual screen entirely, i.e. **row 30 of a 25-row display**. Fixed by
keying off `g_vid.gw/gh`, the **mode's** extent, which is also what the real driver keys
off (it hooks INT 10h and rebuilds its screen on a mode change).

Two smaller ones alongside it: **reset did not centre the pointer**, and **there was no
cell snapping** — the real driver quantises to its 8x8 text cell (`100,50` → `96,48`;
`101,51` → `96,48`).

⚠ **Not yet confirmed by hand** at the time: the change touched the coordinate path that
QBasic's mouse and Doom's mouse-look run through.

### Recorded abstentions (`tests/probes/dos/oracle-rules.json`)

| row | why the oracle cannot be truth |
|---|---|
| `i33.vector/AX` | the handler's load segment — no host-independent answer exists. Kept because segment **0** (no driver) is the one answer that matters. |
| `i33.24.version/BX` | which `MOUSE.COM` is on the reference disk, not the contract. We report 8.00 deliberately (higher, so `version >=` gates open). The standing risk -- every v7/v8 call such a gate opens was MISS -- was closed by #249: they answer, or `32h` says they are absent. |
| `i33.26.maxvirt/CX,DX` | **MOUSE.COM 6.24 does not implement 26h** — the poison survives the call. We are a superset here; the row is checked for *internal* consistency instead. |
| `i33.15.statesize/BX` | each driver's private save-state size. What must hold is that 15h/16h/17h agree with **each other**. |

## What to fix, in order

1. ~~Report a version whose functions we answer, or implement `25h`/`31h`/`32h`~~ -- done,
   #249 (`p_mouse2.asm`): all of `25h`–`34h` answer or are reported absent by `32h`.
   ~~The rest: #265 (`2Bh`–`2Eh`, `33h`, `34h`, and `18h`/`19h` delivery)~~ -- done
   (2026-10-04), unmeasured: run `p_mouse3` on DOSBox-X (8.05) and, if one can be got,
   an MS Mouse 8.x driver -- 6.24 predates them.
2. ~~`0Fh`/`13h`/`1Ah`~~ -- recorded N/A with the reason, and kept apart (#249).
3. ~~`09h`: draw the guest's graphics cursor~~ -- done, #264 (overlay). Run `p_mouse3`
   on the oracles to pin the XOR value, the hot-spot unit and how far "the driver writes
   VRAM" matters; re-gate a game with its own graphics cursor by hand.
4. ~~`20h` must not reset; `1Dh`/`1Eh`, `22h`/`23h` should answer~~ -- done, oracle-
   verified (#249).
5. The PS/2 auxiliary device and INT 15h `C2h`, so a guest-loaded driver has hardware
   (#199).
