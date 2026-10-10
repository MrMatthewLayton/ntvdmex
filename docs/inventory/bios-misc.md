# Inventory — system BIOS services: INT 08h, 11h, 12h, 14h, 15h, 17h, 1Ah, 1Ch

**Spec:** IBM PC/AT and PS/2 BIOS Technical References; Ralf Brown's Interrupt List.
⚠ **Not held in the repo** — [`../ref/SOURCES.md`](../ref/SOURCES.md). The tick and RTC
hardware under these calls is [pit.md](pit.md) and [rtc.md](rtc.md); the UART and printer
port are [uart.md](uart.md). **INT 13h/25h/26h** are in [dos-services.md](dos-services.md)
§4; **INT 10h** is [video-bios.md](video-bios.md); **INT 16h/09h** is
[keyboard.md](keyboard.md); **INT 33h** is [mouse.md](mouse.md).
**Our implementation:** the V86 BIOS arm in `src/host/main.c` (≈`:29037-29383`, *"BIOS
services: INT 11h/12h/13h/14h/15h/17h/25h/26h"*), its PM twins (≈`:22263-22404`), INT 08h/1Ah
in `src/vdd/vdd_pit.c` (`:517-599`), INT 14h in `src/vdd/vdd_comm.c` (`:224-271`).
**Probes:** `p_bios.asm`, `p_int15.asm`, `p_lpt.asm`. **Off-VM:** `bda_test.c` (#253), `pit_test.c` (T7, T8,
T16, T17), `comm_test.c`.
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
INT 15h is where the surface thins: `AH=88h` reports the extended memory that XMS also owns, and the post-1994 memory calls
(`E801h`, `E820h`) and A20 calls (`2400h`–`2403h`) are refused. (#206 made `AH=86h` and `83h` really wait in V86 and answered `4Fh`;
#256 made the PM `86h` wait too -- PM `83h` is still refused -- and INT 17h honour `DX`.)

| Group | Units | IMPL | PART | STORE | MISS | N/A |
|---|---|---|---|---|---|---|
| §1 INT 11h / 12h | 2 | 2 | — | — | — | — |
| §2 INT 1Ah | 7 | 4 | — | — | 2 | 1 |
| §3 INT 08h / INT 1Ch | 4 | 3 | — | — | 1 | — |
| §4 INT 15h | 17 | 4 | 4 | — | 7 | 2 |
| §5 INT 14h | 5 | 4 | — | — | 1 | — |
| §6 INT 17h | 4 | 4 | — | — | — | — |
| **Total** | **39** | **19** | **5** | **—** | **12** | **3** |

---

## 1. INT 11h and INT 12h

| Unit | Status | Where / what is missing | Verification |
|---|---|---|---|
| INT 11h equipment word | **IMPL** | `bios_equipment_word` (`main.c:2477-2501`): floppy, 80x25 colour, one LPT, the serial count **from the UART VDD**, bit 1 from the `Fpu` setting, bit 12 from the joystick setting. V86 `:29102-29107`; PM twin uses the same function (`:22358-22371`). ★ #253: `0040:0010` is written from the same function at start-up (`bios_bda_init`, `main.c:27955`) and re-written when the joystick setting changes (`bios_bda_refresh_equipment`, `:2509-2513`, called from `settings_apply` `:10998`) | abstained (`int11.equip`: it describes the machine); `p_lpt int11.equipment` blocked on PCem |
| INT 12h conventional memory | **IMPL** | `main.c:29108-29119`: `BIOS_BASE_MEM_KB` (`src/dos/bios_bda.h`) = `DOS_MEM_TOP` in KB = 639. ★ #253: the withheld kilobyte is now a real 1 KB EBDA at `9FC0h` — `0040:000E`, INT 15h `C1h` and the `C0h` feature bit 2 all say so, and `0040:0013` holds the same constant ([bda.md](bda.md) §1, §6) | provisional (`int12.memk` = `027Fh`, agrees with 6.22) |

## 2. INT 1Ah

`pit_int1a`, `vdd_pit.c:560-599`; stub `DOS_HDLR_SEG:003C` (`main.c:27022`), V86 arm
`main.c:29384-29393`, PM arm `:22263-22274`.

| AH | Unit | Status | Where / what is missing | Verification |
|---|---|---|---|---|
| `00h` | read tick count, midnight flag | **IMPL** | `:566-572`; AL = flag, cleared by the read. ★ #253: the count is ticks since midnight — seeded at start-up from the same `rtc_now` AH=02h reads (`vdd_pit_seed_time_of_day`, `vdd_pit.c:560-570`; called `main.c:27812`) | provisional (`int1a.00.midnight`, `.advances`); pit_test T7, T17 (seed, and AH=00h/02h agree to the second) |
| `01h` | set tick count | **IMPL** | `pit_int1a`; #262 A: DOS's clock follows the new count (`ticks_set` → `host_ticks_set`), the RTC does not | **oracle** (`p_tick2c` A); pit_test T8/T8b |
| — | a raw store to `0040:006C` | **IMPL** | #262 B (s92): the PIT keeps a **witness** of the last count the BIOS wrote (`pit_bios_tick`, the one tick body for `pit_int08` and the host's two inline bumps); a differing count is a guest's store, and DOS's next `2Ah`/`2Ch`/stamp follows it. One compare per tick | `p_tick2c` B (3 hosts follow), `p_vclock` C/D owed; pit_test T8b, clock_test |
| `02h` | read RTC time, BCD | **IMPL** | `:578-585`; host clock via `rtc_now` (`main.c:13185`, local time). DL (DST) = 0. No clock installed → not answered | provisional (`int1a.02.isbcd`); pit_test T16 |
| `04h` | read RTC date, BCD | **IMPL** | `:586-591` | provisional (`int1a.04.isbcd`) |
| `03h`/`05h` | set RTC time / date | **IMPL** | #250: moves the VDM's RTC offset (`g_DosClock.RtcOffset`), never the host clock; invalid BCD / impossible dates `CF=1`. DOS's clock does not follow (an AT's two clocks) | **oracle** (`p_clock` `clk.1a02.after.1a03`, `clk.2c.after.1a03`) |
| `06h`/`07h` | set / reset RTC alarm | **MISS** | `default:` — registers and CF as passed | — |
| other | unknown function | **MISS** | `default:` leaves **CF as the caller had it** rather than setting it, so an unsupported call can read as success | — |

## 3. INT 08h and INT 1Ch

| Unit | Status | Where / what is missing | Verification |
|---|---|---|---|
| INT 08h: tick `0040:006C` += 1, midnight rollover into `0040:0070` | **IMPL** | `pit_int08` `vdd_pit.c:526-534`; stub `BOP 08h; INT 1Ch; IRET` (`main.c:25783`), arm `:29026-29035`; PM `:22263-22274` | pit_test |
| INT 08h ends with the EOI | **IMPL** | `vdd_pic_eoi(&g_pic, 0)` `main.c:29031`, PM `:22269` | by hand (a 30 s run once got exactly one tick without it) |
| INT 08h counts down the diskette motor (`0040:0040`) and turns it off | **MISS** | no writer; the FDC exists now ([fdc.md](fdc.md)), so this is a real gap, not a missing device | — |
| INT 1Ch: called once per tick, default `IRET` | **IMPL** | `bop1c` `main.c:25784`, planted `:26994` | provisional (`int1c.called`, `.perTick`, `.nested`) |

## 4. INT 15h

V86 arm `main.c:29069-29214`; PM arm `:22349-22404`. Anything not listed: `AH=86h`, CF=1,
logged as `INT15 UNIMPL` (`:29193-29208`).

| AH | Unit | Status | Where / what is missing | Verification |
|---|---|---|---|---|
| `24h` | A20 gate: disable / enable / status / support (`2400h`–`2403h`) | **MISS** | refused. The A20 flag already exists and is shared by the 8042, port `92h` and XMS (`vdd_input_a20_*`, `vdd_input.c:491-494`) | — |
| `4Fh` | keyboard intercept (a hook the BIOS calls) | **IMPL** | #206: an explicit default handler, CF=1 with AL untouched — "process this key" (`main.c:29139-29143`). #244: our INT 09h now CALLS it whenever IVT[15h] is hooked (`bios_kbdact.asm` `k4f`) — [keyboard.md](keyboard.md) §3 | **oracle** (`p_int15w`, 6.22 + DOSBox-X; PCem's AMI answers `AH=86h` and is recorded as such in `oracle-rules.json`) |
| `80h`–`82h` | device open / close / program terminate (hooks) | **MISS** | refused with CF=1 | — |
| `83h` | event wait (set bit 7 of `ES:BX` after CX:DX µs; `AL=01h` cancels) | **PART** | #206, V86: `main.c:29118-29138`; the BDA mirrors it (`0040:0098`–`00A0`); the flag is posted from the 1 kHz pacer (`i15_event_poll`, `main.c:5639-5650`); a second request while one runs is refused `AH=83h` CF=1. **PM refuses it** (`:22387-22391`) | **oracle** (`p_int15w`: 6.22, DOSBox-X, PCem and ours agree, #206) |
| `84h` | joystick | **IMPL** | `:29144-29181`, from the gameport VDD's sample; no stick → `AH=86h` CF=1 | untested ([gameport.md](gameport.md)) |
| `85h` | SysReq (a hook the BIOS calls) | **MISS** | never called (keyboard.md §3) | — |
| `86h` | wait CX:DX microseconds | **IMPL** | #206, V86: re-executes its BOP until the deadline, taking interrupts meanwhile; refused (busy) while an `83h` event runs. #256, PM: from the top-level PM loop the BOP is re-executed the same way (the loop keeps ticking the BIOS clock and delivering IRQ0); from a nested dispatch (an injected ISR, a callback) it waits in place with interrupts held | **oracle** for V86 (`p_int15w`, four hosts); PM on the test machine only (`p_pm256` — no oracle runs a DPMI host) |
| `87h` | move extended memory block | **IMPL** | V86 (`int15_move_block`; success = AH=0, CF=0, ZF=1). #244: **PM too** — `ES:(E)SI` read as a PM pointer (selector base + offset, ESI for a 32-bit client) and the same copy (`int15_move_block_at`). ⚠ DPMI 0.9 reflects INT 15h untranslated, so a strict host would run it with a meaningless real-mode ES; translating is the only reading under which a PM caller's request means anything — a decision, recorded in the PM arm, unmeasured | **oracle** for the V86 round trip (`p_int15` on PCem); PM untested |
| `88h` | extended memory size | **PART** | `:29071-29095`: always `3C00h` (15 MB). ⚠ **The same memory is also handed out by XMS**; a real machine with HIMEM reports 0 here. Recorded in the code and deliberately not changed. **#48 (2026-10-04):** the value is `CMOS_EXT_KB` in both modes, the same constant as CMOS and SysVars+`45h`; the XMS pool is that less the 64 K HMA (was 16384, more than the machine) | untested |
| `89h` | switch to protected mode | **N/A** | a V86 guest cannot be handed the CPU; DPMI is the route | — |
| `90h`/`91h` | device busy / interrupt complete (hooks) | **MISS** | never called by our INT 13h/16h waits | — |
| `C0h` | system configuration table | **IMPL** | V86 **IMPL**: `ES:BX` → `DOS_CTAB_SEG:DOS_SYSCONF_OFF`, model bytes `FC 01 00` from PCem's AMI, feature bits set only where true (`:27543-27560`, arm `:29239-29243`) — feature 1 is `74h`: bit 2 (EBDA, #253) and bit 4 (INT 09h calls `4Fh`, #244). #244: **PM answers it too** — `ES` = a selector over `DOS_CTAB_SEG` (`dpmi_seg_to_desc`, as INT 21h `34h`/`52h`), `BX` = `DOS_SYSCONF_OFF`. ⚠ The session-36 Win16 driver now takes its model-`FCh` (AT) path instead of its no-table path — **owed a Win16 shelf re-run** | **oracle** (`p_int15` C0h on PCem) |
| `C1h` | EBDA segment | **PART** | #253: V86 **IMPL** — `ES=9FC0h`, CF=0, AX untouched (`main.c:29233-29238`), agreeing with INT 12h's 639 KB and `0040:000E`. **PM refuses it** (`:22404-22407`), like `C0h`: a segment in a PM `ES` would be a raw paragraph, not a selector. Now SeaBIOS-shaped; PCem's AMI and dosbox-x have no EBDA and abstain (`oracle-rules.json`) | **oracle** vs 6.22/SeaBIOS (`p_int15`) — owed a re-run |
| `C2h` | PS/2 pointing device | **MISS** | [mouse.md](mouse.md) §3 | — |
| `C3h`/`C4h` | watchdog / POS (MCA) | **N/A** | Micro Channel only | — |
| `E801h` | extended memory, large configurations | **MISS** | refused; DOS extenders and newer HIMEMs ask this before `88h` | — |
| `E820h` | system memory map | **MISS** | refused | — |

## 5. INT 14h

`comm_int14`, `vdd_comm.c:224-271`; the V86 arm only delivers it (`main.c:29215-29238`).
The BIOS and the UART registers are one device.

| AH | Unit | Status | Where | Verification |
|---|---|---|---|---|
| `00h` | initialise (baud/parity/stop/length → divisor, LCR) | **IMPL** | `:238-249` | comm_test |
| `01h` | send AL | **IMPL** | `:250-255`; loopback or the host sink | comm_test |
| `02h` | receive | **IMPL** | `:256-263`; `AH=80h` timeout when empty | blocked on PCem (`int14.02.recv.timeout`) |
| `03h` | status | **IMPL** | `:264-266` | blocked on PCem (`int14.03.status`) |
| `04h`/`05h` | extended initialise / modem control (PS/2) | **MISS** | `default:` `AX=8000h` (`:267-269`) | — |

## 6. INT 17h

V86 arm `main.c:29239-29271`; one printer, spooled to a file and shared with the `378h`
port model (`lpt_spool_put`).

| AH | Unit | Status | Where / what is missing | Verification |
|---|---|---|---|---|
| `00h` | print AL | **IMPL** | `:29247-29263`; `90h` ready, or `28h` (I/O error + out of paper) when the byte went nowhere | blocked on PCem (`int17.00.print` ×2) |
| `01h`/`02h` | initialise / status | **IMPL** | `:29264-29267` | — |
| `DX` | printer number | **IMPL** | #256: the port comes from `0040:0008+2*DX`; no port there (or `DX` > 2, or a base other than our fitted 378h) returns at once with every register as passed | **oracle** (`p_int17` LPT3 / `DX=3`: 6.22, DOSBox-X, PCem agree) |
| other | unknown function | **IMPL** | #256: `AX` as passed (6.22/SeaBIOS, DOSBox-X). PCem's AMI answers the status instead -- disputed, recorded in `oracle-rules.json` | **oracle** (`p_int17`, two of three; AMI abstains with a rationale) |

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

`int1a.00.advances` reported **"the clock does not advance"** on the test machine and nowhere
else. It was the probe: a 400-unit spin is longer than a tick on the emulated 486 and
*shorter* than one on the test machine, so the probe gave up before the counter moved. ▶ **When a
row fails on one host only, check the probe's own assumptions about time before you
touch the host.** And the first `int1a/02` off-VM check asserted `0x1729` for 23:41 —
BCD 23:41 is `0x2341`. It failed on its first run, which is why expectations are
executed rather than reasoned about.

## What to fix, in order

1. INT 15h `86h`/`83h` from protected mode: the PM arm still answers `86h` at once and
   refuses `83h`; give it #206's V86 behaviour.
2. INT 15h `2400h`–`2403h` on the shared A20 flag; `E801h`/`E820h` from the same
   numbers `88h` and XMS use.
3. ~~INT 17h: honour `DX`; refuse unknown functions~~ — #256.
4. ~~Settle the EBDA question~~ — #253: a real 1 KB EBDA at `9FC0h`. Still owed: decide
   `88h` vs XMS deliberately, with Doom and the batteries re-gated.
5. INT 08h's diskette-motor countdown, now that the FDC exists.
