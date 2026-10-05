# Inventory — IBM Game Control Adapter (gameport, `201h`)

**Spec:** IBM *Game Control Adapter* technical reference (the 558 quad one-shot, the
resistance-to-time relation, the port layout); Ralf Brown's Interrupt List for INT 15h `AH=84h`
and the INT 11h equipment word. Neither document is held in the repository (see
[`../ref/SOURCES.md`](../ref/SOURCES.md)); there is no `docs/ref/gameport.md` yet.
**Our implementation:** `src/vdd/vdd_joy.c` (58 lines), `src/vdd/vdd_joy.h`; the host stick, the
BIOS service and the equipment bit in `src/host/main.c`. Off-VM battery `tests/unit/joy_test.c`
(19 checks).
**Oracles:** none asked. No probe in `tests/probes/dos/` touches `201h`, and `p_int15.asm` has no
`AH=84h` case.
**Marked:** 2026-09-29, **from the code**.

⚠ `main.c` line numbers are from the 2026-09-29 working tree and that file moves daily; each
citation names the symbol to search for.

---

## Headline — the port is a faithful one-shot; the BIOS service beside it is not

`vdd_joy.c` models the hardware rather than a joystick: a write fires four one-shots, a read
reports which are still timing, the pulse is timed off real elapsed microseconds (QPC) and not off
the number of port reads, an axis with nothing on it never ends, and the buttons are active low.
An adapter configured as absent reads `FFh`, exactly like the unclaimed port it replaced.

The weak half is INT 15h `AH=84h`, which answers from the same host sample by a different route
and disagrees with the port twice: it reports the two unwired buttons of a 2-axis adapter as
**pressed**, and it calls the whole service unsupported when the adapter is fitted but nothing is
plugged in. Its protected-mode twin has no `84h` arm at all.

⚠ `joy_test.c` pins **our** constants (`25 + 4·v` µs), not the IBM relation — it proves the model
is what the header says, not that the header is the spec.

---

## 1. The port

| Unit | Spec | Status | Where / notes | Verification |
|---|---|---|---|---|
| Address decode `200h–207h` (`201h` canonical) | the card decodes the block | **IMPL** | `vdd_joy.c:57`; added to the bus at `main.c:26809` (`vdd_bus_add(&g_bus, &g_joy_dev)`) | `joy_test.c:37-40` |
| **Write, any value: fire all four one-shots** | | **IMPL** | `joy_out` `vdd_joy.c:37-44`; the value is ignored. A write while pulses are still running restarts all four — whether the 558 restarts a running one-shot has not been checked against its datasheet | `joy_test.c:59`, `:70`, `:85` |
| **Read bits 0–3: one-shot outputs**, 1 while timing | | **IMPL** | `joy_in` `vdd_joy.c:24-32` | `joy_test.c:60-66`, `:72-75`, `:87` |
| Axis order: bit 0 A-X, bit 1 A-Y, bit 2 B-X, bit 3 B-Y | | **IMPL** | `axis[0..3]` (`vdd_joy.h:48`), fed X, Y, U, R of host stick 1 (`joy_poll_thread`, `main.c:10169-10170`) | `joy_test.c:63-66`, `:84-87` |
| **Resistance to time**: `t = 24.2 µs + 0.011 µs/Ω × R`, R = 0–100 kΩ → about 24–1124 µs | IBM | ⛔ **PART** | `joy_axis_us` `vdd_joy.h:60`: `25 + 4·v` µs for an 8-bit position (`main.c:10169`, `>> 8`), so **25–1045 µs** in 4 µs steps. Linear and the right minimum, but full deflection reads as about 93 kΩ; the top ~7 % of the span is never produced. A calibrating game does not notice; one with fixed thresholds reads a shorter stick | pins our constants only: `joy_test.c:62-65`, `:71-75` |
| Timebase is real time, not port reads | | **IMPL** | injected clock `vdd_joy.h:30-32`; `joy_now_us` (QPC) `main.c:10131`. With no clock the pulses never end (`vdd_joy.c:27`) — the off-VM "no joystick" | `joy_test.c:20`, `:36` (fake clock) |
| Idle before the first trigger: bits 0–3 read 0 | | **IMPL** | `vdd_joy.c:24` — only a fired one-shot reports; `vdd_joy_reset` returns to idle (`:46-50`) | `joy_test.c:49`, `:90-93` |
| **Absent axis** (open input): never reaches threshold, bit stays 1 | | **IMPL** | `vdd_joy.c:29-30` — stuck for an axis the adapter does not wire (B pair on a 2-axis type), for nothing plugged in, and with no clock | `joy_test.c:50-52`, `:66` |
| **Buttons, bits 4–7** = A1, A2, B1, B2, **0 = pressed** | | **IMPL** | `vdd_joy.c:20-23`; host buttons 1–4 → bits 4–7 (`main.c:10183`) | `joy_test.c:61`, `:78-81` |
| Unwired or unplugged buttons read 1 | open switch | **IMPL** | `vdd_joy.c:16`, `:21-22` (masked to the wired count) | `joy_test.c:49`, `:79` |
| No adapter fitted (`JoystickType` = None) | an empty ISA slot | **IMPL** | reads `FFh` (`vdd_joy.c:19`); writes do nothing (`:41`) | `joy_test.c:43-45` |
| **Two sticks** — a second, independent device on the B pair | the adapter has two ports | ⛔ **PART** | only `JOYSTICKID1` is polled (`main.c:10166`, `pGetPos(0, …)`): the B axes are the same pad's U/R (right stick) and buttons 3–4 the same pad's. Two physical sticks cannot be two players | untested |

## 2. BIOS — INT 15h `AH=84h`, joystick support

| Unit | Spec (RBIL) | Status | Where / notes | Verification |
|---|---|---|---|---|
| `DX=0`: read switches → AL bits 4–7 | | ⛔ **PART** | V86 arm `main.c:27967-27970`. The wired-button mask is applied to the **complement**: `((~buttons & mask) & 0x0F) << 4`, so on a 2-axis adapter bits 6–7 (B1, B2) read **0 = pressed**, while the port reads them 1. Correct on a 4-axis adapter | untested |
| `DX=1`: read positions → AX = A-X, BX = A-Y, CX = B-X, DX = B-Y, CF=0 | scale is the BIOS's own | **IMPL** | `main.c:27971-27978` — the 0–255 host sample, not a count timed through the one-shot; B pair 0 on a 2-axis adapter | untested |
| Adapter fitted, **nothing plugged in** | | ⛔ **PART** | `main.c:27964-27966` returns `AH=86h`, CF=1 ("not supported") whenever the stick is not live — while INT 11h bit 12 and the port both say an adapter is fitted. The three interfaces disagree about the same machine | untested |
| Any other `DX` | error | **IMPL** | `AH=86h`, CF=1, `main.c:27979-27981` | untested |
| INT 15h `AH=84h` raised **in protected mode** | same service | ⛔ **MISS** | the PM twin (`vec == 0x15`, `main.c:21358`; arms at `:21390-21400`) answers only `88h`/`86h`; `84h` gets `AH=86h`, CF=1 even with a live stick | — |

## 3. Equipment

| Unit | Spec | Status | Where / notes | Verification |
|---|---|---|---|---|
| INT 11h equipment word **bit 12**: game adapter installed | IBM | **IMPL** | `bios_equipment_word` `main.c:2419`, set iff `JoystickType` ≠ None; both arms share it (V86 `main.c:27905`, PM `main.c:21350`) | untested; `p_bios`'s `int11.equipment` row is abstained (`bios-misc.md:23`) |
| BDA `0040:0010`, the stored copy of the equipment word | IBM | ⛔ **MISS** | no code writes it: the BDA block at `main.c:26794-26802` stops at the LPT table (`0040:000E`). A guest that reads the word from memory instead of calling INT 11h does not get our answer. Wider than the gameport — belongs to the BDA surface too | — |

## 4. What the port needs from the host

| Unit | Status | Where / notes | Verification |
|---|---|---|---|
| Stick sampling | **IMPL** | `joy_poll_thread` `main.c:10146`: `joyGetPosEx` bound at run time from `winmm.dll`, every 15 ms (`main.c:10189`), below-normal priority; the port trap reads only the cached sample. Spawned only when a type is configured (`joy_poll_ensure`, `main.c:10203`) | untested (the battery cannot reach it, `joy_test.c:5-7`) |
| `JoystickType` (None / 2-axis / 4-axis) | **IMPL** | `settings_apply`, `main.c:10342` | untested |
| `JoystickGamepad`: pad D-pad → axis A extremes | **IMPL** | `main.c:10171-10180`. Host convenience, not the adapter | untested |

---

## Gaps worth closing, in order

1. **INT 15h `AH=84h DX=0` reports unwired buttons as pressed** (§2). A one-line defect: mask the
   complement the way the port does — `~(buttons & mask)`, restricted to bits 4–7. A game that
   asks the BIOS on a 2-axis adapter sees buttons 3 and 4 held down permanently.
2. **Make the three interfaces describe one machine** (§2, §3). With an adapter fitted and no
   stick, the BIOS should answer as a BIOS with an adapter does, not "unsupported"; give the PM
   twin the same `84h` arm; and write the BDA copy of the equipment word so it says what INT 11h
   says.
3. **The timing relation** (§1). Map the host sample onto the IBM span, `24.2 + 0.011 × R` µs for
   R = 0–100 kΩ, rather than `25 + 4·v`; confirm the 558's behaviour on a write during a running
   pulse; and make `joy_test.c` assert the spec's figures rather than our own.
4. **A second stick** (§1). Poll `JOYSTICKID2` for the B pair and buttons 3–4 when a second
   device exists.
5. **A probe and an oracle.** Nothing here has been compared with a machine. A `p_joy` probe —
   `201h` before any write, after a write with nothing attached (loop count to a bound), the
   `FFh` of no adapter, INT 15h `84h` both halves, INT 11h bit 12 — run on the four oracles would
   pin what an idle adapter and an absent stick actually read.
