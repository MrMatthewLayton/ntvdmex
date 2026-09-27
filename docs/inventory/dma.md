# Inventory — 8237A DMA controller

**Spec:** Intel 8237A datasheet; IBM PC/AT TechRef.
**▶ The hardware reference is [`../ref/dma.md`](../ref/dma.md)** — what the chip *does*.
**Our implementation:** `src/vdd/vdd_dma.c` (233 lines), `src/vdd/vdd_dma.h`.
**Oracles:** MS-DOS 6.22 (QEMU), dosbox-x, PCem. Off-VM: `tools/dostest/dma_test.c`.
DOS probe: `tools/dostest/p_dma.asm`.
**Marked:** 2026-09-23, **from the code**, with citations.

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
which is precisely the reasoning the spec-first directive exists to replace. One of them
is now fixed (§2); the rest are priced below.

| Group | Marked from the code |
|---|---|
| Per-channel address / count + the flip-flop | **IMPL** |
| Page registers — the seven that map to channels | **IMPL** |
| Page registers — the **nine spare latches** | ~~MISS~~ → ✅ **IMPL** *(2026-09-23)* |
| Mode register | **IMPL** (stored; transfer type, auto-init and decrement all consumed) |
| Mask: single bit, clear-all, write-all | **IMPL** |
| Status: TC bits + clear-on-read | **IMPL** |
| Status: **DRQ bits 7:4** | ⛔ **MISS** — always 0 |
| Command register | ⛔ **STORE** — `cmd[]` is written and **never read by anything** |
| Request register (`09h`) | ⛔ **MISS** — `case 0x9: break;` |
| Memory-to-memory transfer | ⛔ **MISS** |
| Temporary register (read `0Dh`) | ⛔ **MISS** — returns `FFh` |

---

## 1. Ports

| Port | Access | Register | Status | Evidence |
|---|---|---|---|---|
| `00h`–`07h` | R/W | addr/count, channels 0–3 | **IMPL** | `dma_out`/`dma_in`, `vdd_dma.c` — `dma_write_half`/`dma_read_half` share one flip-flop per controller, as the part does |
| `C0h`–`CFh` | R/W | addr/count, channels 4–7 | **IMPL** | same, with the 2× port spacing and word units |
| `08h`/`D0h` | W | command | ⛔ **STORE** | `st->cmd[ctrl] = val` and nothing consumes it |
| `08h`/`D0h` | R | status | ⛔ **PART** | TC bits and clear-on-read are right; **DRQ bits 7:4 are always 0** |
| `09h`/`D2h` | W | request (software DRQ) | ⛔ **MISS** | `case 0x9: break;` |
| `0Ah`/`D4h` | W | single mask bit | **IMPL** | |
| `0Bh`/`D6h` | W | mode | **IMPL** | stored per channel; the engine consumes XFER, AUTOINIT and DECREMENT |
| `0Ch`/`D8h` | W | clear byte pointer | **IMPL** | |
| `0Dh`/`DAh` | W | master clear | **IMPL** | `dma_master_clear` — clears cmd, TC and the flip-flop, and **sets every mask bit** |
| `0Dh`/`DAh` | R | temporary register | ⛔ **MISS** | falls through to `*val = 0xFF` |
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
not a general rule about QEMU** — see [the oracle-profile
note](../research/oracle-disagreements.md). Fixed;
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

---

## What to fix, in order

Tracked in GitHub: [#176](https://github.com/MrMatthewLayton/ntvdmex/issues/176) (the list that was here was moved there verbatim, 2026-09-27).

