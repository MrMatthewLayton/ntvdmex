# Inventory — MC146818 RTC and CMOS RAM

**Spec:** Motorola MC146818 datasheet; IBM PC/AT TechRef for the CMOS map.
**▶ The hardware reference is [`../ref/rtc.md`](../ref/rtc.md)** — what the chip *does*.
**Our implementation:** `src/vdd/vdd_cmos.c`, `src/vdd/vdd_cmos.h` *(new 2026-09-23)*.
**Oracles:** MS-DOS 6.22 (QEMU), dosbox-x, PCem. Off-VM: `tests/unit/cmos_test.c`.
DOS probe: `tests/probes/dos/p_rtc.asm`.
**Marked:** 2026-09-23, **from the code**.

---

## Headline — the chip did not exist, and the failure was a hang

**Nothing claimed ports `70h`/`71h`.** The BIOS *service* built on top of the chip did
exist — INT 1Ah `AH=02h`/`04h` is answered out of the host's clock, over in the **PIT**
VDD — so firmware was present and the hardware underneath it was absent. The same split
the 8042 turned out to have.

> ⛔⛔ **And this one hung rather than lying.** An unclaimed ISA port reads **`0xFF`** on
> this host — deliberately, so device detection cannot mistake an absent card for a
> present one (`main.c`, the V86 I/O trap). Status Register A is `0Ah`, and **its bit 7
> is UIP**. The canonical way to read this chip, in every BIOS and every program that
> does it by hand, is *"poll `0Ah` until UIP is clear, then read the time"*. With `0xFF`
> coming back, **UIP was set for ever and that loop never exited.**

⚠ I first read this as a *silent wrong answer* rather than a hang, from `iio_in` — which
leaves `0` for unclaimed ports. That is the **interpreter-side** path; the V86 trap the
guest actually goes through returns `0xFF`, with a comment saying why. **Measuring it
settled which path mattered**: `p_rtc`'s `rtc.statusb` came back `0x8F`, which is
`0xFF & 0x8F` and could not have come from a zero.

---

## Measured, 2026-09-23 — `p_rtc.asm` against three oracles and the test machine

| case | 6.22/QEMU | dosbox-x | PCem | ours (was → now) |
|---|---|---|---|---|
| `rtc.agree.hours` | `0101` | `0101` | `0101` | `0000` → **`0101`** ✅ |
| `rtc.statusb` | `0002` | `0003` | `0002` | `008F` → **`0002`** ✅ |
| `rtc.statusd.vrt` | `0080` | `0080` | `0080` | `0080` → `0080` ⚠ |
| `rtc.equip.low` | `0006` | `0007` | `000D` | `000F` → `0005` — **not adjudicable** |
| `rtc.statusc.clear` | `1000` | `0000` | `5000` | `FFFF` → `0000` ⛔ open |

### ✅ One clock, two doors

`rtc.agree.hours` does not emit a *time* — a time is not comparable across hosts — it
emits **whether INT 1Ah and the chip's hours register say the same thing**. All three
oracles: yes. Us, before: **no**. The two doors are now given the same `rtc_now` hook by
the host, so their agreement is *structural* rather than something to keep in step by
hand — the same principle as A20's three doors.

### ⚠ `statusd.vrt` was right by accident

VRT is bit 7 of Status D and means *"the battery held; this CMOS is valid"*. We agreed
with all three oracles **before the device existed**, because `0xFF` happens to have bit 7
set. A row that agrees for the wrong reason is worth naming: it would have kept agreeing
through any change that did not happen to preserve that bit.

### ⛔ `statusc.clear` is open — a missing *source*, not a missing register

QEMU answers `1000h` and PCem `5000h`: flags latched (update-ended, and periodic on PCem)
and then **cleared by the read**, which is how IRQ8 is acknowledged at the chip. We answer
`0000h`, and the reason has *changed* since this row was first written:

- **Then:** clear-on-read was implemented but we raised no IRQ8 at all, so nothing ever
  set a flag.
- **Now (§4):** the periodic interrupt works and does set PF and IRQF — but **the probe
  never enables it.** `p_rtc` reads Status C without setting PIE, so on our host there is
  genuinely nothing pending. The oracles' flags come from *their* firmware having enabled
  the update-ended interrupt.

⇒ **The row is still `0000h` and is still not a defect, but for a different reason**, and
a row whose *explanation* changes while its *value* does not is exactly the kind that goes
quietly wrong. What is owed is a probe case that enables PIE and counts — not a fix.

---

## 1. Ports

| Port | Access | Register | Status | Evidence |
|---|---|---|---|---|
| `70h` | W | index + **NMI mask (bit 7)** | ✅ **IMPL** | `cmos_out` — the mask is kept out of the index and **counted, not acted on** |
| `70h` | R | *(write-only on the part)* | **N/A** | answers `0xFF` consistently |
| `71h` | R/W | data | ✅ **IMPL** | `cmos_in`/`cmos_out` |

## 2. The clock registers

| Item | Status | Evidence |
|---|---|---|
| `00h`–`09h`, `32h` derived from the host clock, in **BCD** | ✅ **IMPL** | `cmos_clock_reg` |
| Status A — UIP **clear**, divider `010` | ✅ **IMPL** | `0x26`; see below |
| Status B — BCD, 24-hour | ✅ **IMPL** | `0x02`, measured on QEMU **and** PCem |
| Status C — **cleared by reading** | ✅ **IMPL** | PF and IRQF are set by the periodic tick (§4) |
| Status D — VRT set | ✅ **IMPL** | |
| **Day of week (`06h`)** | ✅ **IMPL** | s81 #182: from the VDM clock's weekday (`host_rtc_now`, 1 = Sunday); `ram[]` only when a reader supplies none. A guest WRITE of `06h` stays refused — the weekday is derived from the date |
| Writing the clock (`00h`–`09h`, `32h`) | ✅ **IMPL** | #261 (`8f43870`): moves the VDM's RTC offset (`rtc_set` → `host_rtc_set`, the hook INT 1Ah `03h`/`05h` use); the host's clock never moves. SET (Status B bit 7) freezes a copy and commits it on release; DM (BCD/binary) and 12/24-hour (bit 1, `04h` bit 7 = PM) decoded. DOS's own clock does not follow a chip write, as on an AT | `p_rtcw` (3 hosts, disputes in `oracle-rules.json`); cmos_test; `p_vclock` F owed |

⚠ **UIP reads clear because that is TRUE of this model, not because it is convenient.**
The real chip sets it for ~2 ms once a second while it updates its own registers; ours are
derived from the host clock at the instant of the read, so they are never mid-update.

## 3. CMOS RAM and the POST defaults

| Item | Status | Evidence |
|---|---|---|
| `0Eh`–`7Fh` read/write storage | ✅ **IMPL** | |
| POST defaults: diagnostic, floppy types, equipment, base memory | ✅ **IMPL** | `vdd_cmos_reset`; base memory `15h/16h` follows Settings > Conventional Memory (`cmos_state.base_kb`, #136; 0 = 640 KB, unchanged) — `cmos_test.c` |
| The checksum at `2Eh`/`2Fh` | ✅ **IMPL** | computed at reset over `10h`–`2Dh`; a guest that writes into the range invalidates it, exactly as on a real machine |
| Extended-memory bytes `17h`/`18h`, `30h`/`31h` | ⛔ **MISS** | left zero — should follow the host's own XMS size |

⚠ **The equipment byte's low nibble is not adjudicable** — it describes the *machine*, and
three oracles gave three answers (`6`, `7`, `0Dh`). What matters is that it is populated
at all; before this device, a guest read `0xFF` from it: *"no floppies, no video, no
coprocessor"*, with every bit set.

## 4. IRQ8

| Source | Enable | Flag | Status |
|---|---|---|---|
| **Periodic** | `0Bh` bit 6 | `0Ch` bit 6 | ✅ **IMPL** *(2026-09-23)* |
| **Alarm** | `0Bh` bit 5 | `0Ch` bit 5 | ✅ **IMPL** *(2026-09-23)* |
| **Update ended** | `0Bh` bit 4 | `0Ch` bit 4 | ✅ **IMPL** *(2026-09-23)* |

✅ **The periodic interrupt works**, at the rate in Status A bits 3:0 — a fast, steady
tick **independent of the 8254**, which is exactly why Windows and DOS extenders use it:
*a guest that has reprogrammed the PIT has not touched this one.*

**It rides the host's existing PIT pacer**, off the same `QueryPerformanceCounter` delta
and inside the same lock, rather than a second thread. One pacer, one lock, **one opinion
about how much time has passed** — s61 measured what a second clock does to this project.

⚠ **Dormant unless asked, and that is what made it safe to add.** Nothing is raised
unless the guest sets **PIE** *and* a non-zero rate select, and even then nothing reaches
it until **IRQ8 is unmasked on the slave PIC and IRQ2 on the master**. All of that is off
at reset. `cmos_test.c` pins the negative case explicitly: a whole second of clocks with
PIE clear raises **not one** interrupt.

⚠ **Disabling PIE drops the part-accumulated tick**, so re-enabling starts from *now*
rather than firing immediately off a stale remainder — and that is pinned too.

**Timing canary re-run on the test machine after this landed:** Skyroads `n8=0 max_ms=7` against
the documented guard of `n8=0 max_ms≈6`, with `pacer_prio=0 joy_thread=0 pit_split=1`
unchanged. The extra call in the pacer costs nothing measurable, and no IRQ8 appeared in
the run — the tick stayed dormant, as designed.

### ⛔ And the alarm registers were being refused — a defect I introduced myself

Making Status B writable un-blocked the *periodic* interrupt. The **alarm** stayed
unreachable, because `"everything below 0x0E is read-only"` swept up `01h`, `03h` and
`05h` — the seconds/minutes/hours **alarm** — which is the only way to set an alarm at all.

> **The rule is not "low registers are read-only". It is *we cannot move the host's
> clock*** — which applies to `00`/`02`/`04` and the date, and not to a comparison value
> the guest owns.

Exactly the same shape as refusing Status B, made twice in one file within hours.

⚠ **And the match rule is not equality.** An alarm byte with its top two bits set is a
**don't care**, which is how *"every minute at 42 seconds"* is expressed — a model that
only compares for equality cannot represent it at all.

### And two lies removed while the control registers became writable

Status B had to become writable for PIE to be reachable at all, and once it was, two
bits that had been *ignored* became bits a guest could actually set:

- **DM (bit 2) — binary vs BCD.** A model that ignores it hands a guest that set binary
  mode a BCD byte, and `0x59` seconds reads as **eighty-nine**.
- **24/12 (bit 1).** 12-hour mode is **not "subtract twelve"**: bit 7 of the hours
  register is PM, midnight is 12 AM and noon is 12 PM, neither of which is hour 0. A
  model that ignores it tells a 12-hour guest that 14:00 is **2 AM**.

Both are two lines and both are now honoured. ⚠ **UIP stays read-only** — it is the chip
telling software when it may read, never software telling the chip anything.

---

## What to fix, in order

Tracked in GitHub: [#182](https://github.com/MrMatthewLayton/ntvdmex/issues/182) (the list that was here was moved there verbatim, 2026-09-27).

