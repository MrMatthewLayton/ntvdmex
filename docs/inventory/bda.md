# Inventory — the BIOS Data Area (`0040:0000`–`0040:00FF`) and the EBDA

**Spec:** IBM PC/AT and PS/2 BIOS Technical References (the data-area tables); Ralf Brown's
Interrupt List, `MEMORY.LST` (`0040:xxxx`). ⚠ **Not held in the repo** —
[`../ref/SOURCES.md`](../ref/SOURCES.md) names them. No `docs/ref/bda.md` yet.
**Our implementation:** there is no BDA module. The area is guest memory at linear `400h`,
and each device writes its own fields: the keyboard VDD (`g_in.bda`, `main.c:27845`), the
video VDD (`g_vid.bda`, `main.c:27810`), the PIT VDD (`pit_bda`, `vdd_pit.c:520`), and one
block of host code for the port tables (`main.c:27926-27943`), and — since #253 — one
header for the fields that describe the machine and the EBDA (`src/dos/bios_bda.h`,
`bios_bda_init`, called `main.c:27955`).
**Probes:** `p_kbd`, `p_video`, `p_video2`, `p_bios`, `p_int15`, `p_mcb`, `p_lpt`, and
`bdaprobe.asm`. **Off-VM:** `bda_test.c`, `pit_test.c` T17.
**Marked:** 2026-10-01, **from the code** — every row says which code writes the field.
**Re-marked:** 2026-10-01 for #253 (§1 `0Eh`/`10h`/`13h`, §5 the tick seed, §6 the EBDA).
A field nothing in `src/` writes is **MISS**, whatever value happens to be there: it holds
whatever the VDM's low memory held at start, which is not ours and has not been measured.

---

## Headline

**The BDA is right wherever a device of ours owns the field, and absent wherever the
field belongs to a BIOS function we answer from somewhere else.** Three of those absences
were the "two doors onto one value" shape this project has paid for before (the equipment
word vs `0040:0000`, cost a session for COMM.DRV); #253 closed the first two and the EBDA:

1. ✅ **`0040:0010` (equipment) and `0040:0013` (memory size)** are written at start-up by
   `bios_bda_init` from `bios_equipment_word()` and `BIOS_BASE_MEM_KB` — the same two
   expressions INT 11h and INT 12h return — and `0010` is re-written when the joystick
   setting changes. Before #253 neither was written and both read 0.
2. ✅ **The tick count `0040:006C` is the time of day**: seeded at start-up with the ticks
   since midnight from the clock INT 1Ah `AH=02h` reads (`vdd_pit_seed_time_of_day`).
   Before #253 every launch began at tick 0, i.e. 00:00:00.
3. **The diskette fields** (`003E`–`0048`, `008B`, `008F`–`0095`) are not written: INT 13h
   keeps its status in a host variable (`g_disk_status`), so `0040:0041` never shows the
   last operation's result.

And ✅ **there is a 1 KB EBDA at `9FC0h`**, said consistently by five answers: INT 12h
639 KB, the MCB chain ending at `9FC0h`, `0040:000E` = `9FC0h`, INT 15h `C1h` ES=`9FC0h`
CF=0, and the `C0h` table's feature-1 bit 2. Before #253 the last three said *no EBDA*
while the first two (both measured on 6.22 and agreeing with it) withheld its kilobyte.
**Why the EBDA and not 640 KB:** the two values that implied it are the oracle-verified
ones; 6.22's machine (SeaBIOS) does have a 1 KB EBDA at `9FC0h`; PCem's AMI has none, but
its INT 12h was never measured, and going to 640 KB would have moved `int12.memk`,
`mcb.chain.ends.at` and every PSP+02h away from the oracle. Reasoning in `bios_bda.h`.

| Group | Units | IMPL | PART | STORE | MISS | N/A |
|---|---|---|---|---|---|---|
| §1 Ports, equipment, memory (`00h`–`16h`) | 7 | 5 | — | — | — | 2 |
| §2 Keyboard (`17h`–`3Dh`, `71h`, `80h`–`83h`, `96h`–`97h`) | 9 | 3 | 1 | — | 5 | — |
| §3 Diskette and fixed disk (`3Eh`–`48h`, `74h`–`77h`, `8Bh`–`95h`) | 6 | — | — | — | 6 | — |
| §4 Video (`49h`–`66h`, `84h`–`8Ah`, `A8h`) | 6 | 4 | 1 | — | 1 | — |
| §5 Timer, reset, timeouts, wait flags (`67h`–`7Fh`, `98h`–`A0h`) | 7 | 4 | — | — | 1 | 2 |
| §6 The rest (`F0h`–`FFh`, `0050:0000`, the EBDA) | 3 | 2 | — | — | 1 | — |
| **Total** | **38** | **18** | **2** | **—** | **14** | **4** |

---

## 1. Ports, equipment, memory

| Offset | Field | Status | Who writes it / what is missing | Verification |
|---|---|---|---|---|
| `00h`–`07h` | COM1–COM4 base ports | **IMPL** | `main.c:27939-27941`: one loop over the UART VDD's four slots (#181) — each slot's own base if it is fitted, else 0. COM1/COM2 (`3F8h`, `2F8h`) are fitted; COM3/COM4 exist as slots but are not fitted, so they read 0 | by hand (COMM.DRV loads, #128) |
| `08h`–`0Dh` | LPT1–LPT3 base ports | **IMPL** | `main.c:27942-27943`: `378h` if LPT1 is fitted, then 0 | `p_lpt` (blocked on PCem) |
| `0Eh` | EBDA segment (AT and later) | **IMPL** | #253: `9FC0h`, by `bios_bda_init` (`bios_bda.h`; `main.c:27955`) — the same `BIOS_EBDA_SEG` INT 15h `C1h` returns (`main.c:29233-29238`). Was written 0 as "LPT4: none", which on an AT reads "no EBDA" | `bda_test`; `p_int15 int15.c1.status` (CF only) |
| `10h` | equipment word | **IMPL** | #253: `bios_equipment_word()` (`main.c:2477-2501`) — INT 11h's own function — written by `bios_bda_init` at start-up and by `bios_bda_refresh_equipment` (`:2509-2513`) when `settings_apply` changes the joystick type (bit 12). ⚠ INT 11h still *computes*; a guest that writes `0010` itself is not seen by INT 11h, as it would be on a real BIOS | `bda_test`; no probe compares `0010` with INT 11h yet |
| `12h` | POST / manufacturing test flags | **N/A** | no POST runs | — |
| `13h` | memory size, KB | **IMPL** | #253: `BIOS_BASE_MEM_KB` = 639, written by `bios_bda_init`; INT 12h returns the same constant (`main.c:29108-29119`) | `bda_test`; no probe compares `0013` with INT 12h yet |
| `15h`–`16h` | adapter memory / PS/2 error codes | **N/A** | vendor- and model-specific | — |

## 2. Keyboard

Marked in full in [keyboard.md](keyboard.md) §2; summarised here so the area is complete.

| Offset | Field | Status | Who writes it | Verification |
|---|---|---|---|---|
| `17h` | shift flags | **PART** | `vdd_input.c:404-412`; bit 7 (Insert) never toggled | provisional |
| `18h` | extended shift flags | **MISS** | only cleared (`vdd_input.c:769`) | — |
| `19h` | Alt+keypad accumulator | **MISS** | | — |
| `1Ah`/`1Ch` | ring head / tail | **IMPL** | `vdd_input.c:9-67` | provisional |
| `1Eh`–`3Dh` | the ring | **IMPL** | `vdd_input.h:30-31` | provisional |
| `71h` | Ctrl-Break flag | **MISS** | | — |
| `80h`/`82h` | ring start / end | **MISS** | the ring ignores them | — |
| `96h` | keyboard mode / type | **IMPL** | bit 4 set (`vdd_input.c:774`); bits 0/1 never written | provisional |
| `97h` | LED flags | **MISS** | | — |

## 3. Diskette and fixed disk

| Offset | Field | Status | Notes |
|---|---|---|---|
| `3Eh` | diskette recalibrate status | **MISS** | INT 13h reads the image directly ([dos-services.md](dos-services.md) §6) and the FDC is never driven |
| `3Fh`–`40h` | motor status / motor-off countdown | **MISS** | INT 08h never counts it down ([bios-misc.md](bios-misc.md) §3) |
| `41h` | last diskette operation status | **MISS** | INT 13h keeps it in `g_disk_status` (`main.c:29276-29324`); `AH=01h` answers from there, and the BDA byte never changes |
| `42h`–`48h` | FDC result bytes | **MISS** | |
| `74h`–`77h` | fixed disk status, **number of fixed disks**, control, port offset | **PART** | `0040:0075` (the count of hard disks) is **written `0`** since #179, agreeing with INT 13h and the empty IDE channels ([ide.md](ide.md)); `74h`/`76h`/`77h` not written |
| `8Bh`–`95h` | data rate, fixed-disk status/error/interrupt flags, media state, current cylinders | **MISS** | |

## 4. Video

Marked in full in [video-bios.md](video-bios.md) §6.

| Offset | Field | Status | Who writes it | Verification |
|---|---|---|---|---|
| `49h`–`4Fh` | mode, columns, page size, page offset | **IMPL** | `vdd_video_bda_sync` (`vdd_video.c:441-465`) | **oracle** (#188) |
| `50h`–`5Fh` | cursor per page | **PART** | only the active page's slot (`:466-467`) | — |
| `60h`–`64h` | cursor shape, active page, CRTC port | **IMPL** | `:468-473` | **oracle** / provisional |
| `65h`–`66h` | CGA mode-select / palette | **IMPL** | `vdd_video.c:1449-1453` | **oracle** (#188) |
| `84h`–`89h` | rows, height, EGA/VGA info and flags | **IMPL** | `:474-481` (`89h` PART in video-bios.md) | provisional |
| `8Ah`, `A8h` | display-combination index; Video Save Pointer table | **MISS** | not written | — |

## 5. Timer, reset, timeouts, wait flags

| Offset | Field | Status | Who writes it / what is missing | Verification |
|---|---|---|---|---|
| `6Ch`–`6Fh` | tick count | **IMPL** | `pit_int08`, and two host paths that do the BIOS's bookkeeping while a guest cannot take IRQ0 (`async_inject` nested-RM arm, the flat-PM no-hook arm in `main.c`) — since s92 all three through ONE body, `pit_bios_tick` (`vdd_pit.h`), which also keeps the #262 witness: a count the BIOS did not write is a guest's store, and DOS's clock follows it | provisional (`int1a.00.advances`); `p_tick2c`; pit_test T8b |
| — | the tick count **is time of day** (seeded at start from the clock) | **IMPL** | #253: `vdd_pit_seed_time_of_day` (`vdd_pit.c:560-570`, called `main.c:27812` right after the PIT joins the bus) — `secs * 1193182 / 65536` from `rtc_now`, the clock INT 1Ah `AH=02h` and the CMOS read, and `0070` cleared. Seconds resolution, as POST has from the RTC | `pit_test` T17; no probe compares `AH=00h` with `AH=02h` yet |
| `70h` | midnight rollover flag | **IMPL** | `vdd_pit.c:533`, cleared by INT 1Ah (`:578-588`) and by the start-up seed | provisional (`int1a.00.midnight`) |
| `67h`–`6Ah` | shutdown / reset re-entry pointer | **N/A** | the VDM is never reset into real mode | — |
| `6Bh` | last unexpected interrupt | **N/A** | written only by a BIOS's default IRQ handler; ours are bare `IRET`s (`main.c:27008`, `:27011-27018`) | — |
| `78h`–`7Fh` | printer and serial timeouts | **MISS** | not written; INT 14h/17h do not use them | — |
| `98h`–`A0h` | user wait flag pointer / count / active flag (INT 15h `83h`) | **IMPL** | #206: written by INT 15h `83h` (`main.c:29131-29134`), cleared when the event posts (`main.c:5648`) or is cancelled (`main.c:29122`) | **oracle** via `p_int15w` (the event itself; the BDA bytes are not compared) |

## 6. The rest

| Location | Field | Status | Notes |
|---|---|---|---|
| `0040:00F0`–`00FF` | inter-application communication area | **IMPL** | free for programs; nothing of ours writes it, which is the contract |
| `0050:0000` | print-screen status byte | **MISS** | INT 05h is never invoked ([keyboard.md](keyboard.md) §3). ⚠ `0050:` is also where QBasic keeps its own slots |
| EBDA | extended BIOS data area | **IMPL** | #253: 1 KB at `9FC0:0000`, the kilobyte INT 12h and the MCB chain already withheld (`DOS_MEM_TOP`, `dos_mcb.h:35`). Byte 0 = size in KB (1), the rest zeroed (`bios_bda_init`). Advertised by `0040:000E`, INT 15h `C1h` (V86; PM refuses) and `C0h` feature-1 bit 2 (`64h`). Nothing of ours lives there: `dos_mcb_reserve_top` carves below it (`bda_test`). ⚠ CMOS `15h`/`16h` still says 640 — correct, that is memory *fitted*. ⚠ WOW: krnl386's arena was measured reaching `0xA0000` (`main.c:16996`), i.e. over this kilobyte; harmless (nothing of ours reads the EBDA back) but unconfirmed on a Win16 launch |

---

## What to fix, in order

1. ~~`0040:0010`/`0013` from INT 11h/12h's functions~~ — #253.
2. ~~Seed `0040:006C` with the time of day~~ — #253.
3. ~~Settle the EBDA~~ — #253: a real 1 KB EBDA at `9FC0h`.
4. A probe that reads `0040:000E`/`0010`/`0013`/`006C` beside INT 15h `C1h`/INT 11h/12h/1Ah
   and emits *agreement* (a derived boolean), so the two-doors property is an oracle row
   and not only an off-VM one.
5. `0040:0041` from INT 13h's status; `0040:0075` = 0 deliberately; the timeout tables at
   `0078h`–`007Fh` with their BIOS defaults.
6. `0040:00A8` — see [video-bios.md](video-bios.md).
