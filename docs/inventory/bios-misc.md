# Inventory — system BIOS services: INT 08h, 11h, 12h, 14h, 15h, 17h, 1Ah, 1Ch

**Spec:** IBM PC/AT and PS/2 BIOS Technical References; Ralf Brown's Interrupt List.
⚠ **Not held in the repo** — [`../ref/SOURCES.md`](../ref/SOURCES.md). The tick and RTC
hardware under these calls is [pit.md](pit.md) and [rtc.md](rtc.md); the UART and printer
port are [uart.md](uart.md). **INT 13h/25h/26h** are in [dos-services.md](dos-services.md)
§4; **INT 10h** is [video-bios.md](video-bios.md); **INT 16h/09h** is
[keyboard.md](keyboard.md); **INT 33h** is [mouse.md](mouse.md).
**Our implementation:** the V86 BIOS arm in `src/host/main.c` (≈`:28916-29215`, *"BIOS
services: INT 11h/12h/13h/14h/15h/17h/25h/26h"*), its PM twins (≈`:22157-22298`), INT 08h/1Ah
in `src/vdd/vdd_pit.c` (`:517-599`), INT 14h in `src/vdd/vdd_comm.c` (`:207-254`).
**Probes:** `p_bios.asm`, `p_int15.asm`, `p_lpt.asm`. **Off-VM:** `pit_test.c` (T7, T8,
T16), `comm_test.c`.
**Marked:** 2026-10-01, **from the code**. Carried over from `docs/PARITY.md` (retired
2026-09-23) and re-marked; this file now also covers INT 08h, 14h, 15h and 17h, which
had no inventory.

⚠ **Verification.** `p_bios` was compared against 6.22 under QEMU, i.e. **SeaBIOS** — a
reimplementation, so those rows are **provisional**. `p_int15` was asked of PCem's real
AMI 486 BIOS (s81, #54) and those rows are **oracle**. `p_lpt`'s five rows are recorded in
[sweep.md](sweep.md) as *blocked on PCem*.

---

## Headline

**The tick, the RTC read, the equipment word and memory size are right and measured.**
INT 15h is where the surface thins: **`AH=86h` (wait) returns at once without waiting**,
`AH=88h` reports the extended memory that XMS also owns, and the post-1994 memory calls
(`E801h`, `E820h`) and A20 calls (`2400h`–`2403h`) are refused. INT 17h ignores `DX`, so
every printer number is LPT1.

| Group | Units | IMPL | PART | STORE | MISS | N/A |
|---|---|---|---|---|---|---|
| §1 INT 11h / 12h | 2 | 2 | — | — | — | — |
| §2 INT 1Ah | 7 | 4 | — | — | 2 | 1 |
| §3 INT 08h / INT 1Ch | 4 | 3 | — | — | 1 | — |
| §4 INT 15h | 17 | 1 | 4 | — | 9 | 3 |
| §5 INT 14h | 5 | 4 | — | — | 1 | — |
| §6 INT 17h | 4 | 2 | 1 | — | 1 | — |
| **Total** | **39** | **16** | **5** | **—** | **14** | **4** |

---

## 1. INT 11h and INT 12h

| Unit | Status | Where / what is missing | Verification |
|---|---|---|---|
| INT 11h equipment word | **IMPL** | `bios_equipment_word` (`main.c:2466-2490`): floppy, 80x25 colour, one LPT, the serial count **from the UART VDD**, bit 1 from the `Fpu` setting, bit 12 from the joystick setting. V86 `:28933-28938`; PM twin uses the same function (`:22229-22242`) | abstained (`int11.equip`: it describes the machine); `p_lpt int11.equipment` blocked on PCem |
| INT 12h conventional memory | **IMPL** | `:28939-28947`, derived from `DOS_MEM_TOP` = 639 KB. ⚠ The comment says the top 1 KB is the EBDA, but `0040:000E` is written `0` (`:27787`), INT 15h `C1h` answers CF=1 and the `C0h` table says *no EBDA* — three answers that there is none. See the BDA/EBDA row in the README | provisional (`int12.memk` = `027Fh`) |

## 2. INT 1Ah

`pit_int1a`, `vdd_pit.c:560-599`; stub `DOS_HDLR_SEG:003C` (`main.c:26909`), V86 arm
`main.c:29216-29225`, PM arm `:22157-22168`.

| AH | Unit | Status | Where / what is missing | Verification |
|---|---|---|---|---|
| `00h` | read tick count, midnight flag | **IMPL** | `:566-572`; AL = flag, cleared by the read | provisional (`int1a.00.midnight`, `.advances`); pit_test T7 |
| `01h` | set tick count | **IMPL** | `:573-577` | pit_test T8 |
| `02h` | read RTC time, BCD | **IMPL** | `:578-585`; host clock via `rtc_now` (`main.c:13133`, local time). DL (DST) = 0. No clock installed → not answered | provisional (`int1a.02.isbcd`); pit_test T16 |
| `04h` | read RTC date, BCD | **IMPL** | `:586-591` | provisional (`int1a.04.isbcd`) |
| `03h`/`05h` | set RTC time / date | **N/A** | `:592-597`: deliberately not answered — we cannot move the host clock, and `CF=0` with no effect would be the "runs but lies" shape. Same decision as the CMOS clock registers (`vdd_cmos.c:202-206`). ⚠ `AH=01h` *does* keep a guest-local tick, so a per-VDM offset is possible | unmeasured on any oracle |
| `06h`/`07h` | set / reset RTC alarm | **MISS** | `default:` — registers and CF as passed | — |
| other | unknown function | **MISS** | `default:` leaves **CF as the caller had it** rather than setting it, so an unsupported call can read as success | — |

## 3. INT 08h and INT 1Ch

| Unit | Status | Where / what is missing | Verification |
|---|---|---|---|
| INT 08h: tick `0040:006C` += 1, midnight rollover into `0040:0070` | **IMPL** | `pit_int08` `vdd_pit.c:526-534`; stub `BOP 08h; INT 1Ch; IRET` (`main.c:25677`), arm `:28905-28914`; PM `:22157-22168` | pit_test |
| INT 08h ends with the EOI | **IMPL** | `vdd_pic_eoi(&g_pic, 0)` `main.c:28910`, PM `:22163` | by hand (a 30 s run once got exactly one tick without it) |
| INT 08h counts down the diskette motor (`0040:0040`) and turns it off | **MISS** | no writer; the FDC exists now ([fdc.md](fdc.md)), so this is a real gap, not a missing device | — |
| INT 1Ch: called once per tick, default `IRET` | **IMPL** | `bop1c` `main.c:25678`, planted `:26881` | provisional (`int1c.called`, `.perTick`, `.nested`) |

## 4. INT 15h

V86 arm `main.c:28948-29047`; PM arm `:22243-22298`. Anything not listed: `AH=86h`, CF=1,
logged as `INT15 UNIMPL` (`:29026-29041`).

| AH | Unit | Status | Where / what is missing | Verification |
|---|---|---|---|---|
| `24h` | A20 gate: disable / enable / status / support (`2400h`–`2403h`) | **MISS** | refused. The A20 flag already exists and is shared by the 8042, port `92h` and XMS (`vdd_input_a20_*`, `vdd_input.c:491-494`) | — |
| `4Fh` | keyboard intercept (a hook the BIOS calls) | **N/A** | a guest *calling* it gets CF=1, which is what an unhooked BIOS returns. That our INT 09h never *calls* it is [keyboard.md](keyboard.md) §3 | — |
| `80h`–`82h` | device open / close / program terminate (hooks) | **MISS** | refused with CF=1 | — |
| `83h` | event wait | **MISS** | | — |
| `84h` | joystick | **IMPL** | `:28977-29014`, from the gameport VDD's sample; no stick → `AH=86h` CF=1 | untested ([gameport.md](gameport.md)) |
| `85h` | SysReq (a hook the BIOS calls) | **MISS** | never called (keyboard.md §3) | — |
| `86h` | wait CX:DX microseconds | **PART** | ⛔ `:28975-28976`: **returns at once**, CF=0 — *"the PIT already paces us"*. A program that waits one second with `86h` waits zero. Same in PM (`:22279-22280`) | untested |
| `87h` | move extended memory block | **PART** | V86 **IMPL** (`int15_move_block` `:8153`, arm `:29020-29025`; success = AH=0, CF=0, ZF=1); **PM refuses** it (`:22274`) | **oracle** for the V86 round trip (`p_int15` on PCem) |
| `88h` | extended memory size | **PART** | `:28950-28974`: always `3C00h` (15 MB). ⚠ **The same memory is also handed out by XMS**; a real machine with HIMEM reports 0 here. Recorded in the code and deliberately not changed | untested |
| `89h` | switch to protected mode | **N/A** | a V86 guest cannot be handed the CPU; DPMI is the route | — |
| `90h`/`91h` | device busy / interrupt complete (hooks) | **MISS** | never called by our INT 13h/16h waits | — |
| `C0h` | system configuration table | **PART** | V86 **IMPL**: `ES:BX` → `DOS_CTAB_SEG:DOS_SYSCONF_OFF`, model bytes `FC 01 00` from PCem's AMI, feature bits set only where true (`:27403-27420`, arm `:29015-29019`). **PM refuses it**, knowingly (`:22261-22274`) | **oracle** (`p_int15` C0h on PCem) |
| `C1h` | EBDA segment | **MISS** | CF=1 — matches the AMI under PCem, but contradicts INT 12h's 639 KB (§1) | **oracle** (`p_int15`) |
| `C2h` | PS/2 pointing device | **MISS** | [mouse.md](mouse.md) §3 | — |
| `C3h`/`C4h` | watchdog / POS (MCA) | **N/A** | Micro Channel only | — |
| `E801h` | extended memory, large configurations | **MISS** | refused; DOS extenders and newer HIMEMs ask this before `88h` | — |
| `E820h` | system memory map | **MISS** | refused | — |

## 5. INT 14h

`comm_int14`, `vdd_comm.c:207-254`; the V86 arm only delivers it (`main.c:29048-29071`).
The BIOS and the UART registers are one device.

| AH | Unit | Status | Where | Verification |
|---|---|---|---|---|
| `00h` | initialise (baud/parity/stop/length → divisor, LCR) | **IMPL** | `:221-232` | comm_test |
| `01h` | send AL | **IMPL** | `:233-238`; loopback or the host sink | comm_test |
| `02h` | receive | **IMPL** | `:239-246`; `AH=80h` timeout when empty | blocked on PCem (`int14.02.recv.timeout`) |
| `03h` | status | **IMPL** | `:247-249` | blocked on PCem (`int14.03.status`) |
| `04h`/`05h` | extended initialise / modem control (PS/2) | **MISS** | `default:` `AX=8000h` (`:250-252`) | — |

## 6. INT 17h

V86 arm `main.c:29072-29104`; one printer, spooled to a file and shared with the `378h`
port model (`lpt_spool_put`).

| AH | Unit | Status | Where / what is missing | Verification |
|---|---|---|---|---|
| `00h` | print AL | **IMPL** | `:29080-29096`; `90h` ready, or `28h` (I/O error + out of paper) when the byte went nowhere | blocked on PCem (`int17.00.print` ×2) |
| `01h`/`02h` | initialise / status | **IMPL** | `:29097-29100` | — |
| `DX` | printer number | **PART** | ⛔ never read — `DX=1`/`2` (LPT2/LPT3, not fitted) is served as LPT1 instead of answering *timeout* | — |
| other | unknown function | **MISS** | answers `90h` "ready", CF=0 (`:29101-29103`) — success for a call that does nothing | — |

---

## Measured history (kept)

### The gap the probe found and closed (s72)

**`INT 1Ah` AH=02h and AH=04h fell into `default:`** — a comment reading "RTC subfns
not modelled yet" — which leaves every register exactly as the caller passed it. The
probe poisons CX/DX and got the poison **straight back** (`C1C1`/`D1D1`) where the
oracle answers with the time and date. Guests use these for file timestamps, save-game
dates and as a seed. **BCD is the contract**: a guest reads them as BCD because that
is what a BIOS returns, so a binary 34 would be read as 22.

The clock is **host-supplied** through a `rtc_now` hook on the PIT VDD rather than
`<time.h>` — which the XP-targeting CRT does not link anyway, and which would have put
libc time inside a portable VDD. With no clock installed the call is still **not
answered**: fabricating a date is worse than silence.

### ⚠ A harness artefact

`int1a.00.advances` reported **"the clock does not advance"** on the rig and nowhere
else. It was the probe: a 400-unit spin is longer than a tick on the emulated 486 and
*shorter* than one on the rig, so the probe gave up before the counter moved. ▶ **When a
row fails on one host only, check the probe's own assumptions about time before you
touch the host.** And the first `int1a/02` off-VM check asserted `0x1729` for 23:41 —
BCD 23:41 is `0x2341`. It failed on its first run, which is why expectations are
executed rather than reasoned about.

## What to fix, in order

1. INT 15h `86h`: actually wait (the guest's own PIT ticks are available), or at least
   for a bounded time — a zero wait breaks every program that times with it.
2. INT 15h `2400h`–`2403h` on the shared A20 flag; `E801h`/`E820h` from the same
   numbers `88h` and XMS use.
3. INT 17h: honour `DX`; refuse unknown functions.
4. Settle the EBDA question (INT 12h vs `C1h` vs `0040:000E`), and decide `88h` vs XMS
   deliberately, with Doom and the batteries re-gated.
5. INT 08h's diskette-motor countdown, now that the FDC exists.
