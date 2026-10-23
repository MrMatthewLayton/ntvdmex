# The 8042 keyboard controller

**Spec:** IBM Personal Computer AT Technical Reference (the 8042 command set and the
status register); Intel 8042 UPI datasheet; the PS/2 Technical Reference for System
Control Port A.
**Companion:** [`../inventory/kbc.md`](../inventory/kbc.md) — what *we* do about it.

> **The thesis.** The 8042 is not "the keyboard". It is a **microcontroller sitting
> between the CPU and the keyboard**, and two of the things it controls have nothing to
> do with typing: **the A20 gate** and **the CPU reset line**. A model that treats
> `60h`/`64h` as a scancode FIFO answers the keyboard correctly and leaves a hole where
> every DOS extender, every XMS driver and every "reboot the machine" call goes.

⚠ **Distinct from the keyboard itself.** Scan code sets, make/break and typematic are
the *keyboard's* contract and are described with INT 16h in
[`../inventory/keyboard.md`](../inventory/keyboard.md). This file is the controller.

---

## 1. The two ports, and the four things behind them

| Port | Write | Read |
|---|---|---|
| `60h` | data — a **keyboard** command, or the parameter byte of an 8042 command | the output buffer: a scancode, or an 8042 command's reply |
| `64h` | an **8042 command** | the **status register** |

The single most important consequence: **`60h` is two different things depending on what
was last written to `64h`**, and `64h` is a command port on write and a status port on
read. Four meanings, two addresses.

## 2. The status register — read `64h`

| Bit | Name | Meaning |
|---|---|---|
| 0 | **OBF** | output buffer full — there is a byte for the CPU at `60h` |
| 1 | **IBF** | input buffer full — the controller has not yet taken the last byte written |
| 2 | **SYS** | system flag — **set by POST**, so it is `1` on any machine DOS is running on |
| 3 | **A2** | the last write was to `64h` (a command) rather than `60h` (data) |
| 4 | **INH** | keyboard inhibited (the keylock) — `1` = *not* inhibited on most ATs |
| 5 | **AUXB** | auxiliary (PS/2 mouse) output buffer full |
| 6 | **TIMEOUT** | transmission timeout |
| 7 | **PARITY** | parity error on the keyboard line |

**The protocol every driver follows** is written against IBF and OBF, in this order:

1. Poll `64h` until **IBF is clear**, then write the command to `64h`.
2. If the command takes a parameter, poll until IBF is clear again, then write it to `60h`.
3. If the command returns a byte, poll until **OBF is set**, then read it from `60h`.

⚠ A model that returns `0x00` when idle says *"IBF clear"* (fine, step 1 proceeds) and
*"OBF clear"* for ever, so **step 3 spins until the driver's timeout**. Whether that is
a hang or a slow failure depends entirely on whether the driver has a timeout.

⚠ **SYS is `1` in reality and `0` on a naive model.** It is read by code that wants to
know whether this is a cold boot.

## 3. The 8042 command set — written to `64h`

| Cmd | Name | Behaviour |
|---|---|---|
| `20h` | Read Command Byte | the command byte appears at `60h` |
| `60h` | Write Command Byte | the next byte written to `60h` is it |
| `A7h` / `A8h` | Disable / Enable auxiliary device | |
| `A9h` | Test auxiliary interface | result at `60h` |
| `AAh` | **Self test** | returns **`55h`** at `60h` |
| `ABh` | Interface test | returns `00h` for "no error" |
| `ADh` / `AEh` | **Disable / Enable keyboard** | the classic "quiet the keyboard while I do this" pair |
| `C0h` | Read input port | |
| `D0h` | **Read output port** | the output port appears at `60h` |
| `D1h` | **Write output port** | the next byte written to `60h` becomes it |
| `D2h` | Write keyboard output buffer | the next byte written to `60h` appears in the output buffer as if the keyboard sent it (IRQ1 if enabled). PS/2-class / AMI KBC; not on the original AT 8042 (RBIL PORTS.LST) |
| `FEh` | **Pulse the reset line** | **resets the CPU** — this is how DOS reboots the machine |

### The command byte (`20h` / `60h`)

| Bit | Meaning |
|---|---|
| 0 | keyboard interrupt (IRQ1) enable |
| 1 | auxiliary interrupt (IRQ12) enable |
| 2 | system flag — what `64h` bit 2 reads back |
| 4 | keyboard clock disable |
| 5 | auxiliary clock disable |
| 6 | scan code translation (set 2 → set 1) |

### The output port (`D0h` / `D1h`) — where A20 lives

| Bit | Meaning |
|---|---|
| 0 | **CPU reset**, active low — writing a `0` here **resets the machine** |
| 1 | **A20 gate** — `1` = A20 enabled, addresses above 1 MB reachable |
| 4 | output buffer full (IRQ1) |
| 5 | auxiliary output buffer full (IRQ12) |
| 6 | keyboard clock |
| 7 | keyboard data |

⚠⚠ **Bit 0 is the reason `D1h` is dangerous.** Every correct A20-enable sequence writes
`D1h` then a byte with **bit 0 set** — `DFh` to enable A20, `DDh` to disable it. A byte
with bit 0 clear is a reboot, and it is a reboot *immediately*, not at the next
instruction.

## 4. A20 — three gates, one line

A PC offers more than one way to control the same wire, and **software picks whichever
it likes**, so they must agree:

| Gate | How |
|---|---|
| **The 8042 output port** | `D1h` to `64h`, then `DFh`/`DDh` to `60h`. The original, and the slowest — it is why "A20 is slow" is folklore. |
| **System Control Port A, `92h`** | bit 1 = A20, bit 0 = **fast reset**. PS/2 and everything after. One `OUT`. |
| **The XMS driver** | `AH=03h`/`05h` enable, `04h`/`06h` disable, `07h` query — a *software* interface that ends up driving one of the two above. |

⇒ **A guest that enables A20 through the 8042 and then asks XMS whether A20 is on must
be told yes.** They are the same wire. A model that keeps a flag per interface will
answer "no" to a driver that used a different door, and the driver will conclude the
machine cannot do XMS.

⚠ **Port `92h` bit 0 is a reset too** — the "fast reset" — and has the same hazard as
the output port's bit 0.

## 5. Keyboard-side commands — written to `60h`

These go *through* the controller to the keyboard, and the keyboard replies.

| Cmd | Name | Reply |
|---|---|---|
| `EDh` | Set LEDs | `FAh` (ACK), then the LED byte, then another `FAh` |
| `EEh` | **Echo** | `EEh` — the cheapest "is anything there" test |
| `F0h` | Set scan code set | `FAh`, then the parameter, then `FAh` |
| `F2h` | Read keyboard ID | `FAh`, then `ABh 83h` |
| `F3h` | Set typematic rate/delay | `FAh`, then the parameter, then `FAh` |
| `F4h` / `F5h` | Enable / disable scanning | `FAh` |
| `FFh` | Reset | `FAh`, then **`AAh`** when the self-test passes |

> **Every one of these is acknowledged with `FAh`.** A model that accepts the byte and
> stays silent leaves a driver polling OBF for an ACK that never comes.

## 6. The auxiliary device — the PS/2 mouse (#199)

`D4h` to `64h` routes the **next** byte written to `60h` to the mouse instead of the
keyboard. The mouse answers through the same output buffer, with **status bit 5 (AUXB)**
set beside OBF, and raises **IRQ12** if command-byte bit 1 enables it. Command-byte bit 5
(also `A7h`/`A8h`) stops the mouse's clock: its bytes wait. `A9h` tests the interface
(`00h` = no error).

| Cmd | Name | Reply |
|---|---|---|
| `FFh` | Reset | `FAh`, `AAh` (self test passed), `00h` (device ID) |
| `FEh` | Resend | the last byte again |
| `F6h` | Set defaults | `FAh` — 100 reports/s, 4 counts/mm, 1:1, stream mode, reporting off |
| `F5h` / `F4h` | Disable / enable reporting | `FAh` |
| `F3h` | Set sample rate | `FAh`, the rate, `FAh` |
| `F2h` | Get device ID | `FAh`, `00h` (a standard mouse) |
| `F0h` / `EAh` | Remote / stream mode | `FAh` |
| `EEh` / `ECh` | Set / reset wrap (echo) mode | `FAh` |
| `EBh` | Read data (remote mode) | `FAh`, then one packet |
| `E9h` | Status request | `FAh`, status (bit 0 right, 1 middle, 2 left, 4 scaling 2:1, 5 reporting, 6 remote), resolution, sample rate |
| `E8h` | Set resolution | `FAh`, `0`-`3` (1, 2, 4, 8 counts/mm), `FAh` |
| `E7h` / `E6h` | Scaling 2:1 / 1:1 | `FAh` |

**The packet**, three bytes per report: byte 0 = buttons (bit 0 left, 1 right, 2 middle),
**bit 3 always 1**, bit 4 X sign, bit 5 Y sign, bit 6 X overflow, bit 7 Y overflow; byte 1
= X movement, byte 2 = Y movement, each the low 8 bits of a 9-bit two's complement. **Y is
positive UP**, the opposite of screen coordinates.

**The BIOS interface, INT 15h `AX=C2xxh`:** `C200h` enable (BH=1) / disable, `C201h`
reset (→ BH = ID, BL = `AAh`), `C202h` sample rate (BH 0-6 = 10, 20, 40, 60, 80, 100,
200/s), `C203h` resolution (BH 0-3), `C204h` get type (→ BH = ID), `C205h` initialise
(BH = packet size 1-8), `C206h` extended (BH=0 status → BL/CL/DL as `E9h`; 1/2 scaling),
`C207h` install the far-call handler (ES:BX). Status in AH, CF on error: `01h` invalid
function, `02h` invalid input, `05h` no far call installed. The BIOS's IRQ12 handler calls
the handler once per packet with four words pushed in this order — status, X, Y, Z (0) —
and pops them itself after the handler's `RETF`.

---

## Sources

- **IBM Personal Computer AT Technical Reference** — the 8042 command set, the status
  register, the command byte and output port bit assignments, and the reset line.
- **Intel 8042 UPI-42 datasheet** — the controller itself.
- **IBM PS/2 Technical Reference** — System Control Port A (`92h`) and fast A20.
- **IBM Personal System/2 Hardware Interface Technical Reference**, "Keyboard and
  Auxiliary Device Controller" and "Pointing Device" — §6's commands, replies, status and
  packet. **Ralf Brown's Interrupt List**, PORTS.LST `0060h`/`0064h` and INT 15h
  `AX=C2xxh` — §6's BIOS interface. Neither is held in the repo (cited by name, as
  [`SOURCES.md`](SOURCES.md) lists them); §6 is **unmeasured** until `p_ps2aux` runs.
