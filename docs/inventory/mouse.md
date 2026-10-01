# Inventory — mouse: the INT 33h driver, and the pointing hardware under it

**Spec:** Microsoft *Mouse Programmer's Reference* (INT 33h); Ralf Brown's Interrupt List
(INT 33h, including the v7/v8 additions `25h`–`35h`); IBM PS/2 TechRef (the auxiliary
device on the 8042, INT 15h `AH=C2h`); the Microsoft serial-mouse protocol. ⚠ **Not held in
the repo** — [`../ref/SOURCES.md`](../ref/SOURCES.md). No `docs/ref/mouse.md` yet.
**Our implementation:** `mouse_int33` in `src/host/main.c` (≈`:8263-8517`), its state and
coordinate helpers (≈`:7590-8130`), the callback injector `mouse_cb_try` (≈`:8521`) and
`dpmi_inject_pm_mousecb` (≈`:24990`). The driver is **host code, not a VDD**: there is no
mouse device on the bus.
**DOS probe:** `tools/dostest/p_mouse.asm` (with `p_mouse.pre`/`.deps`: the oracle loads the
real `MOUSE.COM` first). **Oracle:** Microsoft `MOUSE.COM` 6.24 on MS-DOS 6.22 under QEMU — a
**real driver**, so a row that agrees with it is **oracle**, not provisional.
**Marked:** 2026-10-01, **from the code**. Carried over from `docs/PARITY.md` (retired
2026-09-23) and re-marked. ⚠ `main.c` line numbers drift; the `case` labels do not.

---

## Headline

**Every function `MOUSE.COM` 6.24 defines is answered, and the ones a probe can ask agree
with the real driver.** Three things are not what they look like:

1. **We report driver version 8.00** (`24h`), so a guest is entitled to call the v7/v8
   functions `25h`–`34h`. **None is implemented.** They reach `default:`, which counts the
   call and returns the caller's own registers — "success, and here is what you passed".
   `32h` (*get active advanced functions*) is the worst: it is the call a guest uses to
   ask which of the others exist, and it gets its own `AX` back as the answer bitmask.
2. **The sensitivity and mickey-ratio calls are STORE.** `0Fh`, `13h` and `1Ah` are
   stored, read back by `1Bh` and saved by `16h`, but the pointer follows the host's
   cursor and `0Bh` scales by the host's `msens` setting — nothing consumes them.
3. **There is no pointing hardware.** No PS/2 auxiliary device behind the 8042, no
   INT 15h `AH=C2h`, no serial mouse on COM1. A guest that brings its own mouse driver
   finds nothing to drive.

| Group | Units | IMPL | PART | STORE | MISS | N/A |
|---|---|---|---|---|---|---|
| §1 INT 33h functions | 43 | 18 | 2 | 3 | 16 | 4 |
| §2 Event-handler delivery | 4 | 4 | — | — | — | — |
| §3 Pointing hardware | 4 | — | — | — | 4 | — |
| **Total** | **51** | **22** | **2** | **3** | **20** | **4** |

---

## 1. INT 33h functions

Entry: the vector is `DOS_HDLR_SEG:0030` = `BOP 33h; IRET` (`main.c:26875`). V86 arm
`main.c:28875-28879`, PM arm `:22152-22156`, DPMI `0300h` simulation `:23467`. Every entry
reaches the one `mouse_int33`. Coordinates live in a **virtual screen derived from the
mode** (`g_vid.gw/gh`, `i33_w`/`i33_h` `:7711-7739`), with text-mode 8-pixel cell
snapping (`i33_snap`).

| AX | Unit | Status | Where / what is missing | Verification |
|---|---|---|---|---|
| `0000h` | reset and status | **IMPL** | `:8314-8319`; `AX=FFFFh BX=2`; centres the pointer; `nomouse.flag` answers "no driver" (`:7660`) | **oracle** (`i33.00.reset`, `i33.03.afterreset`) |
| `0001h` | show cursor | **IMPL** | `:8320-8322`, hide count never below 0. The pointer is drawn by the host overlay, on 8-bpp frames only (`:12136`) — the real driver does not draw in VESA direct-colour modes either | **oracle** (`i33.01.show`, survivable) |
| `0002h` | hide cursor | **IMPL** | `:8323-8325` | untested |
| `0003h` | position and buttons | **IMPL** | `:8326-8330` | **oracle** (`i33.03.afterreset`) |
| `0004h` | set position | **IMPL** | `:8331-8336`, clamped and snapped | **oracle** (`i33.04.setpos`, `.odd`) |
| `0005h`/`0006h` | press / release counts | **IMPL** | `:8343-8360`; count drained by the read; position of the last transition, or the current one if none | **oracle** (`i33.05.press`, `i33.06.release`) |
| `0007h`/`0008h` | horizontal / vertical range | **IMPL** | `:8361-8372`, `i33_set_range` `:8030`; the pointer is re-clamped at once | **oracle** (`i33.07.clamp.hi`, `.lo`) |
| `0009h` | define graphics cursor | **MISS** | `:8373-8379`: counted (`g_ms_shape_sets`) and **discarded** — the host overlay always draws its own arrow, so a game's crosshair is never seen | — |
| `000Ah` | define text cursor | **PART** | `:8380-8391`: `BX=0` (software cursor, AND/XOR masks) is drawn (`vdd_video_text_cursor`); `BX=1` (use the hardware cursor, scan lines CX..DX) is counted and drawn as the software default | untested (QBasic uses `BX=0`, by hand) |
| `000Bh` | motion counters | **IMPL** | `:8392-8408`; raw device counts when available, zeroed by the read, clamped to signed 16 bits | **oracle** (`i33.0B.motion`, `.again`) |
| `000Ch` | set event handler | **IMPL** | `:8414-8419`; a flat PM client's 32-bit offset is kept whole (`mouse_i33_off` `:7647`). Delivery: §2 | untested (QBasic, ZAR by hand) |
| `000Dh`/`000Eh` | light-pen emulation on / off | **MISS** | `default:` (`:8515`) | — |
| `000Fh` | mickeys per 8 pixels | **STORE** | `:8430-8435`; read back by `1Bh`, saved by `16h`, **never applied** to motion | **oracle** for "does not disturb the position" (`i33.0F.after`) only |
| `0010h` | conditional-off region | **N/A** | `:8436-8441`: accepted, no effect. The frame is re-rendered from VRAM every present and the pointer overlaid on top, so there is no saved-under cursor to corrupt. ⚠ The pointer stays visible inside the region | — |
| `0013h` | double-speed threshold | **STORE** | `:8442-8444`; never consumed | untested |
| `0014h` | exchange event handler | **IMPL** | `:8420-8429`; returns the previous mask/segment/offset | untested |
| `0015h` | state buffer size | **IMPL** | `:8445-8447`, `sizeof(i33_state)` | abstained (`i33.15.statesize`: each driver's private size) |
| `0016h`/`0017h` | save / restore state | **IMPL** | `:8448-8456`, `i33_state_save`/`_load` `:8050-8083`; a versioned block; a bad pointer or foreign block is refused, not faulted (`i33_guest_ptr` `:8085`) | untested — self-consistent by construction, never round-tripped by a probe |
| `0018h`/`0019h` | set / get alternate (shift-qualified) handler | **MISS** | `default:` | — |
| `001Ah` | set sensitivity | **STORE** | `:8457-8463`; stores `0Fh`'s and `13h`'s values, consumed by nothing | **oracle** (`i33.1B.sensitivity` round trip) |
| `001Bh` | get sensitivity | **IMPL** | `:8464-8468`, returns what was stored | **oracle** (`i33.1B.sensitivity`) |
| `001Ch` | set interrupt rate | **MISS** | `default:` | — |
| `001Dh`/`001Eh` | set / get display page | **MISS** | `default:`; `1Eh` returns the caller's `BX` | — |
| `001Fh` | disable driver | **N/A** | `:8487-8489`: answers the documented failure `AX=FFFFh`. We are the driver; there is no earlier INT 33h vector to hand back in `ES:BX` | untested |
| `0020h` | enable driver | **PART** | `:8469-8478`: shares `21h`'s arm, so enabling **also resets** the driver (ranges, handler, counts) — enable should not | untested |
| `0021h` | software reset | **IMPL** | `:8469-8478` | **oracle** (`i33.21.softreset`) |
| `0022h`/`0023h` | set / get language | **MISS** | `default:`; `23h` returns the caller's `BX` where the US driver says `0` | — |
| `0024h` | version, type, IRQ | **IMPL** | `:8490-8495`; `BX=0800h` (8.00), `CX=04FFh` (PS/2, IRQ `FFh` as the real driver says) | **oracle** for `CX`; `BX` abstained (which `MOUSE.COM` is on the disk) |
| `0025h` | general driver information | **MISS** | `default:` — reachable, because we claim 8.00 | — |
| `0026h` | maximum virtual coordinates | **IMPL** | `:8496-8500` | abstained — `MOUSE.COM` 6.24 lacks `26h`; checked for internal consistency |
| `0027h` | screen/cursor masks and mickey counts | **MISS** | `default:` | — |
| `0028h`/`0029h` | set / enumerate video modes | **MISS** | `default:` | — |
| `002Ah` | cursor hot spot | **MISS** | `default:` | — |
| `002Bh`–`002Eh` | acceleration profiles | **MISS** | `default:` | — |
| `002Fh` | mouse hardware reset | **MISS** | `default:` | — |
| `0030h` | BallPoint information | **N/A** | BallPoint hardware only | — |
| `0031h` | current min/max virtual coordinates | **MISS** | `default:` — the values exist (`i33_rangex_*` `:8017-8020`) | — |
| `0032h` | active advanced functions | **MISS** | ⛔ `default:` returns the caller's `AX` as the function bitmask | — |
| `0033h` | switch settings and acceleration data | **MISS** | `default:` | — |
| `0034h` | `MOUSE.INI` path | **MISS** | `default:` | — |
| `0035h` | LCD large pointer | **N/A** | LCD-panel driver support only | — |
| `53C1h` | Logitech CyberMan / SWIFT probe | **IMPL** | `:8505-8507`, `AX=0` = "no SWIFT support". Doom asks and prints the answer | untested |
| any other | — | **IMPL** | `:8515`: counted in `g_ms_i33_unimpl`, registers untouched; the site and `AX` are recorded (`:8269-8299`) | — |

## 2. Event-handler delivery

| Unit | Status | Where | Verification |
|---|---|---|---|
| Event bits 0–6: motion, L/R/M press and release | **IMPL** | `mouse_evt_raise` `:7846-7864`, raised at `:7881`, `:7884`, `:12682`, `:12749` | untested |
| One callback per event, in order, with that moment's buttons and position; motion coalesced, buttons never | **IMPL** | 32-entry ring (`MS_EVQ`, `:7823-7826`) | untested |
| V86 delivery: a far call with `AX`=condition, `BX`=buttons, `CX/DX`=position, returning to `BOP 35h` | **IMPL** | `mouse_cb_try` `:8521-8600`; return stub `DOS_HDLR_SEG:00E0` (`:7807`), re-verified before every call | by hand (QBasic) |
| PM delivery to a DPMI client's handler | **IMPL** | `dpmi_inject_pm_mousecb` `:24990` | by hand (ZAR, s74c) |

## 3. Pointing hardware

| Unit | Status | Notes |
|---|---|---|
| PS/2 auxiliary device on the 8042 (`D4h` write-to-aux, `AUXB` status, IRQ12) | **MISS** | `kbd_hw_out` handles `A7h`/`A8h` as command-byte bits only (`vdd_input.c:605-606`); `D4h` is not decoded and nothing raises IRQ12 |
| INT 15h `AH=C2h` PS/2 pointing-device BIOS | **MISS** | falls to the INT 15h unimplemented arm, `AH=86h CF=1` (`main.c:29026-29041`) |
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

### Recorded abstentions (`tools/dostest/oracle-rules.json`)

| row | why the oracle cannot be truth |
|---|---|
| `i33.vector/AX` | the handler's load segment — no host-independent answer exists. Kept because segment **0** (no driver) is the one answer that matters. |
| `i33.24.version/BX` | which `MOUSE.COM` is on the reference disk, not the contract. We report 8.00 deliberately (higher, so `version >=` gates open). ⚠ **The standing risk is now confirmed from the code: every v7/v8 call such a gate opens is MISS (§1).** |
| `i33.26.maxvirt/CX,DX` | **MOUSE.COM 6.24 does not implement 26h** — the poison survives the call. We are a superset here; the row is checked for *internal* consistency instead. |
| `i33.15.statesize/BX` | each driver's private save-state size. What must hold is that 15h/16h/17h agree with **each other**. |

## What to fix, in order

1. Either report a version whose function set we actually answer (6.26 or 7.0x), or
   implement `25h`, `31h` and `32h` — the last with an honest bitmask — so a
   version-gated guest is not handed its own registers.
2. Make `0Fh`/`13h`/`1Ah` mean something for a guest that reads motion only through
   `03h`, or record them as N/A with the reason.
3. `09h`: draw the guest's graphics cursor (screen and cursor masks, hot spot) instead of
   the host arrow when one has been defined.
4. `20h` must not reset; `1Dh`/`1Eh`, `18h`/`19h`, `22h`/`23h` should at least answer.
5. The PS/2 auxiliary device and INT 15h `C2h`, so a guest-loaded driver has hardware.
