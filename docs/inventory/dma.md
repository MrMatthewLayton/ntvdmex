# Inventory — 8237A DMA controller

**Spec:** Intel 8237A datasheet; IBM PC/AT TechRef.
**▶ The hardware reference is [`../ref/dma.md`](../ref/dma.md)** — what the chip *does*.
**Our implementation:** `src/vdd/vdd_dma.c` (289 lines), `src/vdd/vdd_dma.h`.
**Oracles:** MS-DOS 6.22 (QEMU), dosbox-x, PCem. Off-VM: `tests/unit/dma_test.c`.
DOS probe: `tests/probes/dos/p_dma.asm`.
**Marked:** 2026-09-23, **from the code**, with citations. **Re-marked** 2026-10-01 for #176 items 2 and 3 (§5), and **2026-10-02 for #246** (§6). DOS probe for the chip's own transfers: `tests/probes/dos/p_dma2.asm`.

⚠ **This surface had never been inventoried and had never been asked of an oracle.**
`dma_test.c` was an off-VM battery written against our own model, so — exactly as the
inventory README warns — it encoded our behaviour rather than the datasheet's. Both are
now fixed: `p_dma.asm` asks three machines, and `dma_test.c` has checks whose
expectations came from them.

---

## Headline

**The transfer engine is good and the register file was a subset.** Everything Sound
Blaster playback needs — the address arithmetic for both widths, auto-initialise,
terminal count, the mask bit — is implemented and is load-bearing for Doom's audio.
What was missing is the parts of the chip *no guest we happen to run has asked for*,
which is precisely the reasoning the spec-first directive exists to replace. Three of them
are now fixed (§2, §5); the rest are priced below.

| Group | Marked from the code |
|---|---|
| Per-channel address / count + the flip-flop | **IMPL** |
| Page registers — the seven that map to channels | **IMPL** |
| Page registers — the **nine spare latches** | ~~MISS~~ → ✅ **IMPL** *(2026-09-23)* |
| Mode register | **IMPL** (stored; transfer type, auto-init and decrement all consumed) |
| Mask: single bit, clear-all, write-all | **IMPL** |
| Status: TC bits + clear-on-read | **IMPL** |
| Status: **DRQ bits 7:4** | ~~MISS~~ → ✅ **IMPL** *(2026-10-01, #176)* — derived from the devices, §5 |
| Command register | ~~STORE~~ → **PART** *(2026-10-01, #176)* — **bit 2 (controller disable) honoured**; the other seven stored, §5 |
| Request register (`09h`) | ~~MISS~~ → ✅ **IMPL** *(2026-10-02, #246)* — spec-implemented, no oracle implements it (§6) |
| Memory-to-memory transfer | ~~MISS~~ → ✅ **IMPL** *(#246)* — spec-implemented, unverifiable on the panel (§6) |
| Temporary register (read `0Dh`) | ~~MISS~~ → ✅ **IMPL** *(#246)* (§6) |
| The AT cascade (channel 4 gates controller 1) | ~~not modelled~~ → ✅ **IMPL** *(#246)* — POST state + the grant (§6) |
| Count wraps `0000h` → `FFFFh` at TC | ~~rested at `0000h`~~ → ✅ **IMPL** *(#246)* (§6) |

---

## 1. Ports

| Port | Access | Register | Status | Evidence |
|---|---|---|---|---|
| `00h`–`07h` | R/W | addr/count, channels 0–3 | **IMPL** | `dma_out`/`dma_in`, `vdd_dma.c` — `dma_write_half`/`dma_read_half` share one flip-flop per controller, as the part does |
| `C0h`–`CFh` | R/W | addr/count, channels 4–7 | **IMPL** | same, with the 2× port spacing and word units |
| `08h`/`D0h` | W | command | **PART** | bit 2 read by `vdd_dma_grants`; bits 0,1,3–7 stored only — §5 |
| `08h`/`D0h` | R | status | ✅ **IMPL** | TC bits clear on read; DRQ bits 7:4 derived per read from `vdd_dma_dreq` (and not cleared by it) — §5 |
| `09h`/`D2h` | W | request (software DRQ) | ✅ **IMPL** | `req[]`; served by the controller itself — §6 |
| `0Ah`/`D4h` | W | single mask bit | **IMPL** | |
| `0Bh`/`D6h` | W | mode | **IMPL** | stored per channel; the engine consumes XFER, AUTOINIT and DECREMENT |
| `0Ch`/`D8h` | W | clear byte pointer | **IMPL** | |
| `0Dh`/`DAh` | W | master clear | **IMPL** | `dma_master_clear` — clears cmd, TC and the flip-flop, and **sets every mask bit** |
| `0Dh`/`DAh` | R | temporary register | ✅ **IMPL** | `temp[]`, the last byte of a memory-to-memory copy — §6 |
| `0Eh`/`DCh` | W | clear mask register | **IMPL** | |
| `0Fh`/`DEh` | W | write all mask bits | **IMPL** | |
| `80h`–`8Fh` | R/W | page registers | ✅ **IMPL** | seven map to channels, nine latch in `page_spare[]` |

## 2. ✅ The spare page latches — FIXED 2026-09-23

`dma_page_chan` (`vdd_dma.c`) returns `-1` for `80h`, `84h`–`86h`, `88h`, `8Ch`–`8Fh`,
and both the read and write paths **used to** do nothing — the comment on that arm read:

```c
default:   return -1;            /* 0x80 / 0x84-0x86 / 0x88 / 0x8C-0x8F: unused */
```

⛔ **"Unused" was true of the CHANNEL MAPPING and false of the HARDWARE.** Those ports
are real read/write latches on a PC — the address decoder does not bother to leave them
out — and `80h` is also the POST diagnostic port. We answered `FFh`, which describes an
empty bus rather than a machine. They now latch in `page_spare[]`.

⚠ **Whether any guest cares is not the question.** The scope rule is that a device is in
because it is in the period-correct hardware contract; "no guest has asked" is the
reasoning this programme exists to stop.

**Measured** (`p_dma.asm dma.page.spare80`): dosbox-x and **PCem, on a real AMI 486
BIOS**, both read back a written `0x5A`; only 6.22-under-QEMU answers `0xFF`. ⚠ **That is
not a general rule about QEMU** — see [why oracles
disagree](../testing.md#why-oracles-disagree). Fixed;
`page_spare[]` in `vdd_dma.h`, and `dma_test.c` pins that the spare latches are **not the
same storage** as a channel's page.

## 2b. What the probe confirmed, and one thing it caught in the act

| case | 6.22/QEMU | dosbox-x | PCem | ours |
|---|---|---|---|---|
| `dma.ch1.addr` | `1234` | `1234` | `1234` | `1234` ✅ |
| `dma.ch1.count` | `5678` | `5678` | `5678` | `5678` ✅ |
| `dma.page.ch1` | `005A` | `005A` | `005A` | `005A` ✅ |
| `dma.page.spare80` | `00FF` | `005A` | `005A` | `00FF` → **`005A`** ✅ |
| `dma.status.idle` | `0400` | `0000` | `0400` | `0000` ⛔ open |

✅ **The flip-flop is right, and the count case proves the strong form of it.** It
deliberately does *not* clear the byte pointer between the address case and the count
case, so it only lines up if the model shares **one flip-flop per controller** between
the two registers, as the part does. All four hosts agree.

⛔ **`dma.status.idle` is open, and it is not a register-file defect.** `AH` is the first
status read and `AL` the second. QEMU and PCem answer `0400h`: bit 2 of the first read is
**channel 2's terminal count, latched by the floppy transfer that loaded the probe
itself**, and the second read is `0x00` because **reading status clears the TC bits**.
That is the datasheet's latching behaviour caught in the act.

We and dosbox-x answer `0000h` — **not** because the latch is missing (it is implemented,
and `dma_test.c` pins it) but because **neither of us models a floppy on a DMA channel**,
so nothing ever sets channel 2's TC. The difference is a missing *device*, not a missing
register.

## 3. What the transfer engine gets right, and why it is not in doubt

| Item | Status | Evidence |
|---|---|---|
| 8-bit channel: `(page << 16) \| addr`, byte units | **IMPL** | `vdd_dma_cur_phys` |
| 16-bit channel: `((page & 0xFE) << 16) \| (addr << 1)`, **word** units | **IMPL** | same |
| Count is transfers − 1 | **IMPL** | |
| Auto-initialise reloads base into current at TC | **IMPL** | `vdd_dma_read`/`_write` |
| A non-auto-init channel **masks itself** at TC | **IMPL** | as the part does |
| No DACK on a masked channel **or a disabled controller** | ✅ **IMPL** *(#176)* | `vdd_dma_grants` — the one test every data path asks |
| Address decrement | **IMPL** | |
| TC latched and cleared on a status read | **IMPL** | |

✅ This half is exercised continuously by Sound Blaster playback and is the reason Doom
has audio. Nothing below detracts from it.

## 4. Instrumentation that is worth keeping

`rd_addr[]`, `rd_count[]`, `count_reads`, `rd_status[]` and the width counters
`rd_w1/w2/w4` exist to answer a specific question — *does the guest's mixer find the play
head by counting IRQs or by reading the current address?* The header records why
(`vdd_dma.h`), including that **a count of port reads is not a count of polls**, because
one 16-bit poll is two 8-bit reads. That is a good note and it stays.

## 5. ✅ Status DRQ bits and the controller-disable bit — FIXED 2026-10-01 (#176 items 2, 3)

### Status bits 7:4 — request pending

The datasheet: bits 4–7 *"are set whenever their corresponding channel is requesting
service."* They always read `0`. The request is a pin the **card** drives, and the card's
own state already says whether it wants the bus, so the bits are **derived at the read**,
not latched:

- A device that can request DMA registers a `dma_dreq_fn` once from its init
  (`vdd_dma_add_dreq`); it returns the mask of channels it is asserting now.
  `vdd_dma_dreq` ORs them. The registration is **machine wiring** and survives
  `vdd_dma_reset` (a guest's master clear does not unplug the sound card).
- **SB** (`sb_dreq`, `vdd_sb.c`): its transfer channel while a transfer is armed and not
  paused. ⚠ On the wire DREQ pulses once per byte; we have no byte clock on the CPU's
  side, so the bit reads **steady** for the duration of the transfer.
- **GUS** (`gus_dreq`, `vdd_gus.c`): the DRAM channel while a 41h "go" waits on the 8237
  (`dma_waiting`), and the record channel while 49h bit 0 has the ADC sampling.
- **Channel 4 (controller 2, bit 4)** is the AT **cascade**: controller 1's HRQ, raised
  for a request controller 1 would serve (an unmasked channel on an enabled controller).
  Derived from bits 0–3 through `vdd_dma_grants`; a device's claim on channel 4 is
  dropped.
- **Independent of the mask and of the disable bit**, by design: those decide whether
  the 8237 *answers*; a refused request is exactly the one status must show.
- **FDC: no DREQ** — the floppy has no execution-phase data path yet (`vdd_fdc.c`), so
  channel 2 never requests. That is the same missing device `dma.status.idle` (§2b) is
  waiting on.

Reading status still clears the TC bits and **never** the DRQ bits (`dma_test` T11).

### Command register — what each bit does, and what we do with it

| Bit | Datasheet meaning | Status | Why |
|---|---|---|---|
| 0 | memory-to-memory (ch0 → temp → ch1) | ✅ **IMPL** *(#246)* | `dma_m2m`, started by channel 0's software request — §6 |
| 1 | channel 0 address hold (with bit 0) | ✅ **IMPL** *(#246)* | channel 0's address stands still: a block fill |
| 2 | **controller disable** | ✅ **IMPL** | `vdd_dma_grants` |
| 3 | compressed timing | **STORE** | a bus-cycle length; we have no bus cycles |
| 4 | rotating priority | **STORE** | we have no arbiter: each device pulls its own channel on its own thread, so there is no contention to order |
| 5 | extended write selection | **STORE** | a strobe width |
| 6 | DREQ sense active-low | **STORE** | the board's wiring contract — a PC's cards drive DREQ active-high, so flipping it tells the chip every idle line is a request; modelling it would invent transfers no device asked for |
| 7 | DACK sense active-high | **STORE** | the same, for the acknowledge |

**Bit 2.** With it set the controller gives no DACK on any of its four channels: no byte
moves, the address and count stand still, no TC — and the DREQs stay pending. Every
data path asks **one** helper, `vdd_dma_grants(st, ch)` = mask bit clear **and** the
channel's controller not disabled:

| Path | How it honours it |
|---|---|
| `vdd_dma_read` / `vdd_dma_write` (`dma_xfer`) | returns 0 bytes, no TC — replaces the old inline `if (c->masked)` |
| SB playback (`sb_render`, `vdd_sb.c`) | asks before the fetch; refused ⇒ **holds** (silence, `out_nodack`), no IRQ, block counter frozen, resumes on re-enable |
| GUS DRAM DMA (`gus_dma_ready`) | holds `dma_waiting`; retried every render |
| GUS record (`gus_record`) | the ADC holds |
| FDC | no data path exists yet — nothing to gate |

Master clear clears the command register, so it also **re-enables** the controller.

⚠ **SB behaviour change, deliberate, on a shared path.** Before #176 an SB fetch the 8237
refused (then only: a masked channel) came back short and `sb_render` took it for **the
end of the block** — it raised an IRQ the card never raises and dropped the transfer to
IDLE. Now a masked channel holds the DSP exactly as a disabled controller does, as the
hardware does. A guest whose DSP length is **longer** than its single-cycle 8237 count
used to be rescued by that spurious IRQ; it now waits, as on a real card. **Needs a test-machine
re-gate of the audio guests (Doom, ZAR, Skyroads) before it ships.**

✅ **SUPERSEDED by #246 (§6): the cascade IS modelled now.** The paragraph below is the pre-#246 record.

⚠ **The cascade is NOT modelled for transfers.** On an AT, controller 1 reaches the bus
through channel 4, so masking channel 4 or disabling controller 2 starves channels 0–3
too. We do not run the BIOS that unmasks channel 4 (master clear leaves it masked), so
honouring it would silence every 8-bit transfer. Doing it properly means: the host's
POST state unmasks channel 4 and programs it for cascade mode, then `vdd_dma_grants`
for ch0–3 also requires `vdd_dma_grants(st, 4)`. Status bit 4 (the cascade's DREQ) *is*
derived, because it reads a pin, not a grant.

### What item 4 (request + temporary registers, memory-to-memory) would take — ✅ done in #246, see §6

- **Request register (`09h`/`D2h`)**: a per-channel software-request latch (bits 1:0 the
  channel, bit 2 set/reset), cleared at TC and by master clear. It ORs into the DREQ
  mask (status bits 7:4) — `vdd_dma_dreq` is the place — and, for block mode, is a
  request no device pulls, so the controller itself has to *perform* the transfer.
- **Memory-to-memory (command bit 0)**: a software request on channel 0 runs a block:
  read a byte at ch0's address into the **temporary register**, write it at ch1's
  address, step both (ch0's address held when bit 1 is set), until ch1's TC. That is
  the controller's first transfer with **no device on either end**, so it needs an
  executor inside `vdd_dma.c` (run synchronously on the `09h` write is the honest
  model). Controller 1 only, 8-bit.
- **Temporary register (read `0Dh`/`DAh`)**: the last byte that passed through a
  mem-to-mem transfer; cleared by master clear.
- **Probe**: `p_dma.asm` can only test it with channels 0 and 1 — **never channel 2**
  (the probe's own output) and never a master clear on controller 1.

---

## 6. ✅ The chip's own transfers and the AT cascade — #246 (2026-10-02)

**From the Intel 8237A datasheet, and the panel cannot check it:** `p_dma2.asm` asked all
three oracles and **none implements a software request at all** — QEMU's i8257 latches it into
status bit 5 and runs nothing (it serves only channels with a registered device), dosbox-x and
PCem's emulated 8237 ignore `09h`. Their `000Fh` is *the absence of a measurement* (the
`pit.bcd` template), recorded with that rationale as an all-oracle abstention on `dma2.*`.

| case (`p_dma2`) | 6.22/QEMU | dosbox-x | PCem | ours, first cut (`c533aa71`) | ours **final** (`b0e3b5b9`) | datasheet |
|---|---|---|---|---|---|---|
| `dma2.req.count` — 16 verify cycles on a MASKED ch1 | `000F` | `000F` | `000F` | `0000` | **`FFFF`** | `FFFF` (non-maskable; count wraps at TC) |
| `dma2.req.status.moved` — AH TC1/DRQ1, AL bytes walked | `2000` | `0000` | `0000` | `0210` | **`0210`** | TC1 set, request cleared, 16 walked |
| `dma2.m2m.copy` — first:tenth byte copied | `0000` | `0000` | `0000` | `A0A9` | **`A0A9`** | `A0A9` |
| `dma2.m2m.past.temp` — byte 11 : temporary register | `0000` | `00FF` | `0000` | `00A9` | **`00A9`** | `00A9` |
| `dma2.cascade.held.served` — ch4 masked : unmasked | `0707` | `0707` | `0707` | `0700` | **`07FF`** | held, then served |

The first cut rested the count at `0000h` after TC (the engine's behaviour since the sound
epic); the datasheet says the count goes `0000h` → `FFFFh` to generate TC, and the probe made
the difference visible — fixed in the same commit. `p_dma` is unchanged by #246 (4 AGREE, the
known `dma.status.idle` dispute). The pre-#246 host would answer every row like the oracles:
`09h` was `case 0x9: break;`.

What was built (`vdd_dma.c`):
- **Request register** `req[2]`: bit 2 set/reset, bits 1:0 the channel; **non-maskable**;
  cleared at TC and by master clear; ORed into `vdd_dma_dreq` (status 7:4, and HRQ = DREQ4).
  With no device on DACK the **controller performs the cycles itself** (`dma_soft_block`):
  verify walks only; read touches nothing observable; **write stores `FFh`** — the undriven
  bus, the same float an unclaimed port reads. Runs to TC in demand, single and block mode
  alike (the request stays asserted until TC); a cascade-mode channel does no cycles.
- **Memory-to-memory** (command bit 0, controller 1): channel 0's request copies
  ch0 → **temporary register** → ch1, channel 1's count runs, its TC ends it; bit 1 holds
  channel 0's address. ⚠ At that EOP each channel auto-initialises or masks by its own mode
  bit and only channel 1 latches TC — **a reading of the datasheet, not a measurement.**
- **Temporary register** (read `0Dh`/`DAh`): the last byte moved; cleared by master clear.
- **The AT cascade:** `vdd_dma_grants` for channels 0–3 also needs channel 4 unmasked and
  controller 2 enabled. **POST state** (`dma_post`, from `vdd_dma_reset`/`init`): channel 4
  in cascade mode (`C0h`), unmasked — what the BIOS leaves. HRQ (status D0h bit 4) is still
  raised by controller 1 when the far end refuses it. Channel 4's *mode* is not asked.
- **The count wraps `0000h` → `FFFFh` at TC** on a non-auto-init channel (it rested at
  `0000h`); a driver polling the count for `FFFFh` to see a single-cycle block end needs it.
- ⚠ **Memory beyond `110000h` is never touched** by these engine-driven cycles: a guest
  linear address is a host VA only inside the V86 space.
- A pending request is re-examined after **every** register write (a re-enable, the cascade
  coming back), since nothing else would wake it.

Off-VM: `dma_test.c` T13–T15 (+ the cascade rows in T12 and the FFFFh row in T4) — 110 checks;
four mutations (no cascade check, no `req` in DREQ, no clear-at-TC, no POST unmask) each fail
2–8 of them. ⚠ **Shared with the sound cards** (`vdd_dma_grants` is what SB and GUS ask), so gated on
the test machine, interleaved A/B ×2, unattended with `cfg\wavrec.flag` (A = `2ae28649`, #179 only;
B = `b0e3b5b9`, #246) — s87:

| run | Doom sounding s / IRQ5 | ZAR sounding s / IRQ5 |
|---|---|---|
| A1 | 49 of 59 / 4998 | 66 of 70 / 712 |
| B1 | 49 of 61 / 5175 | 65 of 70 / 706 |
| A2 | 49 of 60 / 5038 | 66 of 70 / 710 |
| B2 | 49 of 60 / 5018 | 65 of 70 / 705 |

Unchanged within run-to-run spread (Doom identical; ZAR 65 vs 66 on both B runs — one
second, at the launch/close edge, the same size as A1↔A2 peak variation). Not a listening test.

## What to fix, in order

Tracked in GitHub: [#176](https://github.com/MrMatthewLayton/ntvdmex/issues/176) (the list that was here was moved there verbatim, 2026-09-27).

