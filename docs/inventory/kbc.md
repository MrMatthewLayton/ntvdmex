# Inventory — 8042 keyboard controller

**Spec:** IBM AT TechRef; Intel 8042 UPI datasheet; PS/2 TechRef for port `92h`.
**▶ The hardware reference is [`../ref/kbc.md`](../ref/kbc.md)** — what the chip *does*.
This file is the companion: what **we** do about it.
**Our implementation:** `src/vdd/vdd_input.c` (`kbd_hw_in`/`kbd_hw_out`); the A20 flag
in `src/host/main.c` (XMS).
**Oracles:** MS-DOS 6.22 (QEMU), dosbox-x, PCem. Off-VM: `tests/unit/input_test.c`.
**Marked:** 2026-09-23, **from the code**, with citations.

⚠ **This surface has never been inventoried.** `keyboard.md` covers INT 16h and the BDA
— the *firmware* — and lists *"port 60h/64h re-read semantics, 8042 status bits"* under
**"Not yet inventoried"**. This is that.

---

## Headline

**Was:** *"We model the keyboard's FIFO and nothing else about the controller."* `60h`
popped a scancode, `64h` answered one bit of eight, every 8042 *command* was counted and
discarded, and the output port — **which is where A20 and the CPU reset line live** — did
not exist.

> ⛔ **A20 was three separate flags, two of them missing.** XMS kept `g_xms.a20` and it
> was the only one; the 8042 gate was not implemented and port `92h` **was claimed by
> nothing**, so a write vanished and a read returned the bus's `0xFF` — whose bit 1 is
> set, so an empty bus was telling guests A20 was on. **They are one wire.** A guest that
> enabled A20 the hardware way and then asked XMS `AH=07h` was told it was off.

**✅ Now:** the status register, the 8042 command set, the output port, port `92h` and a
single converged A20 bit are all implemented and measured — see *Measured, 2026-09-23*
below. The tables in §1–§5 carry the **before → after** so the reasoning stays legible.
⚠ Keyboard-side ACKs (§5) are still MISS.

| Group | Marked from the code |
|---|---|
| `60h` data — scancode FIFO | **IMPL** — and carefully; the transfer-hold is modelled |
| `64h` status register | ~~PART, 1 bit of 8~~ → ✅ **IMPL** |
| 8042 commands (`64h` writes) | ~~MISS~~ → ✅ **IMPL** — 9 commands answered |
| Output port / **A20** / reset | ~~MISS~~ → ✅ **IMPL**; reset **counted**, not obeyed |
| Port `92h` (fast A20) | ~~UNCLAIMED~~ → ✅ **IMPL**, same bit as the output port |
| Keyboard commands (`60h` writes) | ⛔ **PART** — `F3h`'s parameter is captured; **nothing is ever ACKed** |

---

## 1. Ports

| Port | Access | Register | Status | Evidence |
|---|---|---|---|---|
| `60h` | R | output buffer | **IMPL** | `kbd_hw_in`, `vdd_input.c:383` — pops, re-asserts the line, models the re-read hold |
| `60h` | W | keyboard command / 8042 parameter | ⛔ **PART** | `kbd_hw_out`, `vdd_input.c:403` — logged; only `F3h`'s rate byte is kept |
| `64h` | R | status register | ✅ **IMPL** | `vdd_input.c:435` — ~~`sc_avail ? 0x01 : 0x00`~~ |
| `64h` | W | 8042 command | ✅ **IMPL** | `vdd_input.c:483` — ~~counted into `kbd_out_log` and dropped~~ |
| `92h` | R/W | System Control Port A | ✅ **IMPL** | `syscon_in`/`syscon_out`, `vdd_input.c:402` |

## 2. The status register (`64h` read)

| Bit | Name | Status | Evidence |
|---|---|---|---|
| 0 | OBF | **IMPL** | `vdd_input.c:380` |
| 1 | IBF | **N/A-by-luck** | always 0 — which *happens* to mean "ready", so the write path never blocks |
| 2 | **SYS** | ✅ **IMPL** | set; a machine that has POSTed reads **1** |
| 3 | A2 | ✅ **IMPL** | `kbc_last_was_cmd`, `vdd_input.c:483`; **starts SET** — see below |
| 4 | INH | ✅ **IMPL** | set: the keyboard is not inhibited |
| 5 | AUXB | **N/A** | no PS/2 aux path to fill it |
| 6:7 | TIMEOUT / PARITY | **N/A** | error conditions a model has no way to produce |

⚠ **IBF reading 0 is the lucky direction and worth recording as such.** The canonical
driver loop is *"wait until IBF is clear, then write"*, so a constant 0 lets every write
through immediately. Had the polarity been the other way, every 8042 driver on earth
would hang on us. It is not a decision; it is an accident that happens to be safe.

## 3. 8042 commands — `64h` writes

**Every one WAS MISS**: `kbd_hw_out` incremented a counter, appended to a 16-entry log,
and returned. The log stays — it is how a run says what guests actually ask for — but the
commands are now answered.

| Cmd | Status | What a guest loses |
|---|---|---|
| `AAh` self test | ✅ **IMPL** | ~~polls OBF for `55h` for ever~~ — `vdd_input.c:492` |
| `ABh` interface test | ✅ **IMPL** | returns `00h` |
| `20h`/`60h` read/write command byte | ✅ **IMPL** | stored and read back |
| `ADh`/`AEh` disable/enable keyboard | ⛔ **STORE** | sets the command byte's clock bits; **nothing consumes them** — our FIFO is host-driven |
| `D0h` **read output port** | ✅ **IMPL** | ~~returned a stale scancode~~ |
| `D1h` **write output port** | ✅ **IMPL** | the A20 gate, `vdd_input.c:523` |
| `D2h` write keyboard output buffer | ✅ **IMPL** *(#244)* | the next `60h` byte goes into the scancode FIFO as if typed, IRQ1 included (`kbd_hw_out`). A PS/2 / AMI-KBC command (the AT 8042 lacked it) — from documentation; `p_kbd3` injects through it, so its `int09.4f.*` rows also say which oracles have it. input_test T13(d) |
| `FEh` pulse reset | ⚠ **COUNTED** | a VDD cannot reboot its own host; `kbc_reset_asked` |

⛔ **`D0h` was worse than "missing", and this is why the reply lives in its own byte.**
The command was discarded, so the guest's next read of `60h` returned `sc_last` — **the
last scancode**. A driver reading the output port to check A20 got a plausible byte whose
bit 1 was whatever the last key happened to set: the *"runs but lies"* shape. Controller
replies are now a separate one-deep buffer that is drained before any scancode, and
`input_test.c` pins exactly that case (a key queued *and* a command pending).

## 4. A20 — one wire, three doors, two of them bricked

| Door | Status | Evidence |
|---|---|---|
| XMS `AH=03h`–`07h` | ✅ **IMPL** | now a **view** onto the controller's bit |
| 8042 output port bit 1 | ✅ **IMPL** | **owns** the bit |
| Port `92h` bit 1 | ✅ **IMPL** | the same bit |
| **The address wrap itself** | ⛔ **DELIBERATELY NOT MODELLED** | recorded at `main.c:23410` — *"no A20 aliasing … we model the A20 FLAG but not the address wrap"*, because remapping the view on every toggle is expensive |

✅ **CONVERGED 2026-09-23.** ⚠ **The recorded decision not to model the wrap is separate
from this gap and still stands.** What is wrong is not that the wire has no effect; it is that **the three ways
of asking about it do not agree with each other.** A guest that opens A20 through the
8042 and then queries XMS is told `0`, and concludes the machine cannot do XMS at all —
which is a much worse failure than "A20 does not actually wrap".

⇒ **The fix that is worth making is to converge the flag**, not to implement aliasing:
one A20 bit, written by the 8042's output port, by port `92h` and by XMS alike, and read
back by all three.

## 5. Keyboard commands — `60h` writes

| Item | Status | Evidence |
|---|---|---|
| `F3h` typematic — the rate byte is captured | **IMPL** (as instrumentation) | `vdd_input.c:419` |
| **Any `FAh` ACK, ever** | ⛔ **MISS** | `kbd_hw_out` never enqueues a reply |
| `EEh` echo, `F2h` ID, `FFh` reset | **MISS** | |
| LED state | **MISS** | also missing from the BDA — `keyboard.md` |

⛔ **Nothing is ever acknowledged.** Every keyboard command is answered by the keyboard
with `FAh`, and a driver that writes `EDh` and waits for it waits for ever. The comment
at `vdd_input.c:398` is explicit that accept-and-ignore is a *deliberate* holding
position — *"changing what the keyboard does on the strength of an untested guess is how
the last two attempts at this went"* — which is the right instinct and is exactly what a
probe against three oracles is for.

---

## Measured, 2026-09-23 — `p_kbc.asm` against three oracles and the rig

| case | 6.22/QEMU | dosbox-x | PCem | ours (was → now) |
|---|---|---|---|---|
| `kbc.status.idle` | `001C` | `001C` | `001C` | `0000` → **`001C`** ✅ |
| `kbc.selftest.55` | `0000` | `0000` | **`0155`** | `0000` → **`0155`** ✅ |
| `kbc.outport.d0` | `00FF` | `00FF` | **`01CF`** | `00FF` → **`0103`** ⚠ |
| `kbc.port92.read` | `0002` | `0002` | `00FF` | `00FF` → **`0002`** ✅ |
| `kbc.a20.readback` | `0001` | `0001` | **`0101`** | `0001` → **`0101`** ✅ |

### ★ The split runs the other way here, and that is the finding

On the 8254's BCD bit and the 8259's poll, the real-BIOS machine was one of the hosts
*without* the feature. **Here PCem answers every 8042 command and the two software
emulators answer none of them.** It is the period-correct machine that has the chip and
the emulators that cut the corner — about as strong as evidence gets.

⚠ **And it cuts the other way one row down.** PCem answers `00FF` for port `92h` — an
undecoded port floating high — because its machine is an AT-class 486 and System Control
Port A is a PS/2-era addition. It is *period-correct absent* there. That does not make
PCem the worse oracle any more than the rows above make it the better one: **it makes it
a different machine**, and the question each time is which machine the contract is
written against. NTVDMEX targets XP-era hardware, where `92h` exists.

### ✅ Fixed

- **The status register**, from 1 bit of 8 to the real thing. Unanimous across all three
  oracles, so no judgement was needed.
  ⚠ **And the probe caught an error in my own expectation.** The off-VM check was written
  from the datasheet's bit list as `0x14` — SYS + INH — and all three machines said
  `0x1C`. The extra bit is **A2**: *"the last write went to `64h`"*, which on a machine
  DOS is running on was POST's own last command. **Writing the bits from the spec got
  them right and their POST state wrong**, which is exactly what an oracle is for.
- **The 8042 command set** — `AAh`, `ABh`, `20h`/`60h`, `ADh`/`AEh`, `A7h`/`A8h`,
  `D0h`/`D1h`. Replies live in their own byte, so **a controller reply can never be a
  scancode** — which is what the old code handed back, and what a driver reads bit 1 of
  as the A20 gate.
- **Port `92h` is claimed**, and its bit 1 is the same bit as the output port's.
- **A20 converged.** The 8042 output port, port `92h` and XMS `AH=03h`–`07h` are now
  three views of **one bit** (`vdd_input_a20_get/set`). Before, XMS owned the only flag,
  so a guest that opened the gate the hardware way and asked XMS was told it was shut —
  whose honest reading is *"this machine cannot do XMS"*.
  ⚠ **Still no address wrap**, and that decision is unchanged and separate. What was
  wrong was that the three ways of *asking* disagreed.
- **Reset requests are counted, not obeyed** — `FEh` and `92h` bit 0. A VDD cannot reboot
  the machine it is a guest on; pretending to would be worse than the count, and dropping
  it silently worse still.

### ⚠ Not agreement yet: the output port's other bits

PCem answers `0xCF`, we answer `0x03`. **Bit 0 (reset line high) and bit 1 (A20 open)
match** — the two bits with a defined meaning to software. PCem also sets bits 2, 3, 6
and 7 (the keyboard clock and data lines, and two undefined bits). **One oracle is not a
pass**, so those are left alone and recorded here rather than copied.

---

## What to fix, in order

Tracked in GitHub: [#180](https://github.com/MrMatthewLayton/ntvdmex/issues/180) (the list that was here was moved there verbatim, 2026-09-27).

