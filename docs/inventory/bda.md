# Inventory — the BIOS Data Area (`0040:0000`–`0040:00FF`) and the EBDA

**Spec:** IBM PC/AT and PS/2 BIOS Technical References (the data-area tables); Ralf Brown's
Interrupt List, `MEMORY.LST` (`0040:xxxx`). ⚠ **Not held in the repo** —
[`../ref/SOURCES.md`](../ref/SOURCES.md) names them. No `docs/ref/bda.md` yet.
**Our implementation:** there is no BDA module. The area is guest memory at linear `400h`,
and each device writes its own fields: the keyboard VDD (`g_in.bda`, `main.c:27732`), the
video VDD (`g_vid.bda`, `main.c:27697`), the PIT VDD (`pit_bda`, `vdd_pit.c:520`), and one
block of host code for the port tables (`main.c:27778-27787`).
**Probes:** `p_kbd`, `p_video`, `p_video2`, `p_bios`, `p_lpt`, and `bdaprobe.asm`.
**Marked:** 2026-10-01, **from the code** — every row says which code writes the field.
A field nothing in `src/` writes is **MISS**, whatever value happens to be there: it holds
whatever the VDM's low memory held at start, which is not ours and has not been measured.

---

## Headline

**The BDA is right wherever a device of ours owns the field, and absent wherever the
field belongs to a BIOS function we answer from somewhere else.** Three of those absences
are the "two doors onto one value" shape this project has paid for before (the equipment
word vs `0040:0000`, cost a session for COMM.DRV):

1. **`0040:0010` (equipment) and `0040:0013` (memory size) are never written**, while INT 11h
   and INT 12h compute their answers (`bios_equipment_word`, `main.c:2466-2490`;
   `DOS_MEM_TOP`). A program that reads the BDA directly — which is what INT 11h *is* on a
   real BIOS — gets a different machine from one that calls the interrupt.
2. **The tick count `0040:006C` is never set to the time of day.** It counts up from
   whatever it held at start; nothing seeds it from the host clock. INT 1Ah `AH=00h` is
   therefore not "ticks since midnight", and neither is any program's reading of `006C`.
3. **The diskette fields** (`003E`–`0048`, `008B`, `008F`–`0095`) are not written: INT 13h
   keeps its status in a host variable (`g_disk_status`), so `0040:0041` never shows the
   last operation's result.

And **there is no EBDA**, said consistently by three answers (`0040:000E` = 0, INT 15h
`C1h` CF=1, the `C0h` table's bit 2 clear) — but INT 12h reports 639 KB, and the code's own
comment for that says the top 1 KB *is* the EBDA (`dos_mcb.h:35`).

| Group | Units | IMPL | PART | STORE | MISS | N/A |
|---|---|---|---|---|---|---|
| §1 Ports, equipment, memory (`00h`–`16h`) | 7 | 3 | — | — | 2 | 2 |
| §2 Keyboard (`17h`–`3Dh`, `71h`, `80h`–`83h`, `96h`–`97h`) | 9 | 3 | 1 | — | 5 | — |
| §3 Diskette and fixed disk (`3Eh`–`48h`, `74h`–`77h`, `8Bh`–`95h`) | 6 | — | — | — | 6 | — |
| §4 Video (`49h`–`66h`, `84h`–`8Ah`, `A8h`) | 6 | 4 | 1 | — | 1 | — |
| §5 Timer, reset, timeouts, wait flags (`67h`–`7Fh`, `98h`–`A0h`) | 7 | 2 | — | — | 3 | 2 |
| §6 The rest (`F0h`–`FFh`, `0050:0000`, the EBDA) | 3 | 1 | — | — | 1 | 1 |
| **Total** | **38** | **13** | **2** | **—** | **18** | **5** |

---

## 1. Ports, equipment, memory

| Offset | Field | Status | Who writes it / what is missing | Verification |
|---|---|---|---|---|
| `00h`–`07h` | COM1–COM4 base ports | **IMPL** | `main.c:27783-27785`: `3F8h`, `2F8h` — exactly the UARTs that claimed their ports — then 0, 0 | by hand (COMM.DRV loads, #128) |
| `08h`–`0Dh` | LPT1–LPT3 base ports | **IMPL** | `main.c:27786-27787`: `378h` if LPT1 is fitted, then 0 | `p_lpt` (blocked on PCem) |
| `0Eh` | EBDA segment (AT and later) | **IMPL** | `main.c:27787` (`bda[7] = 0`): "no EBDA" — consistent with INT 15h `C1h` (§6) | — |
| `10h` | equipment word | **MISS** | **not written.** INT 11h answers from `bios_equipment_word()` (`main.c:2466-2490`) and the two can disagree | — |
| `12h` | POST / manufacturing test flags | **N/A** | no POST runs | — |
| `13h` | memory size, KB | **MISS** | **not written.** INT 12h answers `DOS_MEM_TOP`-derived 639 (`main.c:28939-28947`) | — |
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
| `41h` | last diskette operation status | **MISS** | INT 13h keeps it in `g_disk_status` (`main.c:29109-29157`); `AH=01h` answers from there, and the BDA byte never changes |
| `42h`–`48h` | FDC result bytes | **MISS** | |
| `74h`–`77h` | fixed disk status, **number of fixed disks**, control, port offset | **MISS** | `0040:0075` (the count of hard disks a program may read before using INT 13h `80h`) is not written; we expose no fixed disks by design |
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
| `6Ch`–`6Fh` | tick count | **IMPL** | `pit_int08` (`vdd_pit.c:526-534`), and two host paths that do the BIOS's bookkeeping while a guest cannot take IRQ0 (`main.c:3467-3470`, `:24688-24691`) | provisional (`int1a.00.advances`) |
| — | the tick count **is time of day** (seeded at start from the clock) | **MISS** | ⛔ nothing seeds `006C`: the only writers are the increments above. INT 1Ah `AH=00h` and every direct reader get "ticks since an unknown start", not ticks since midnight | — |
| `70h` | midnight rollover flag | **IMPL** | `vdd_pit.c:533`, cleared by INT 1Ah (`:569-575`) | provisional (`int1a.00.midnight`) |
| `67h`–`6Ah` | shutdown / reset re-entry pointer | **N/A** | the VDM is never reset into real mode | — |
| `6Bh` | last unexpected interrupt | **N/A** | written only by a BIOS's default IRQ handler; ours are bare `IRET`s (`main.c:26895`, `:26898-26905`) | — |
| `78h`–`7Fh` | printer and serial timeouts | **MISS** | not written; INT 14h/17h do not use them | — |
| `98h`–`A0h` | user wait flag pointer / count / active flag (INT 15h `83h`/`86h`) | **MISS** | INT 15h `83h` is refused and `86h` returns at once ([bios-misc.md](bios-misc.md) §4) | — |

## 6. The rest

| Location | Field | Status | Notes |
|---|---|---|---|
| `0040:00F0`–`00FF` | inter-application communication area | **IMPL** | free for programs; nothing of ours writes it, which is the contract |
| `0050:0000` | print-screen status byte | **MISS** | INT 05h is never invoked ([keyboard.md](keyboard.md) §3). ⚠ `0050:` is also where QBasic keeps its own slots |
| EBDA | extended BIOS data area | **N/A** | none fitted: `0040:000E` = 0, INT 15h `C1h` CF=1, `C0h` feature bit 2 clear. ⚠ INT 12h's 639 KB (`DOS_MEM_TOP = 9FC0h`, `dos_mcb.h:35`, *"640K - 1K EBDA"*) describes one — either reserve and advertise it, or report 640 KB. Measured answers exist for both choices (s72: 6.22 says 639) |

---

## What to fix, in order

1. Write `0040:0010` from `bios_equipment_word()` and `0040:0013` from `DOS_MEM_TOP`, at
   start and whenever a setting that feeds them changes — one function, two doors.
2. Seed `0040:006C` from the host's local time at start (ticks since midnight), the way
   POST does from the RTC.
3. Settle the EBDA: the 1 KB INT 12h withholds is either an EBDA (`000E`, `C1h`, `C0h`
   bit 2) or it is not withheld.
4. `0040:0041` from INT 13h's status; `0040:0075` = 0 deliberately; the timeout tables at
   `0078h`–`007Fh` with their BIOS defaults.
5. `0040:00A8` — see [video-bios.md](video-bios.md).
