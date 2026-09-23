# The 8259A Programmable Interrupt Controller

**Spec:** Intel 8259A/8259A-2/8259A-8 datasheet (Programmable Interrupt Controller);
IBM Personal Computer AT Technical Reference for the board wiring.
**Companion:** [`../inventory/pic.md`](../inventory/pic.md) — what *we* do about it.

> **The thesis.** A PIC is not "a thing that makes interrupts happen". It is **three
> 8-bit registers and a priority resolver**, and almost every subtle bug in interrupt
> handling is a disagreement about *one of those registers*, not about delivery. The
> register that matters most is the one software cannot write and rarely reads: the
> **In-Service Register**. It is the reason a handler is not re-entered, and it is
> cleared by exactly one thing — an **EOI**.

---

## 1. The three registers, and the one rule

| Register | Written by | Read by | Meaning |
|---|---|---|---|
| **IRR** — Interrupt Request | the hardware lines | OCW3 then a base-port read | a line has requested service and has not yet been vectored |
| **ISR** — In-Service | the chip, at delivery | OCW3 then a base-port read | a line has been vectored and **not yet acknowledged** |
| **IMR** — Interrupt Mask | OCW1 (data port) | a data-port read | `1` = this line is masked off |

**The rule the whole chip exists to enforce:**

> While a bit is set in the ISR, the chip will not deliver that line **or any line of
> lower priority**. Only an **EOI** clears an ISR bit.

A model that vectors an interrupt without setting ISR — or that clears ISR at delivery
time when the chip is not in auto-EOI mode — has no re-entrancy protection at all, and
the symptom is not "an extra interrupt": it is a handler entered inside itself, for
ever, the moment a guest's ISR re-enables interrupts (which they all do).

---

## 2. The PC's two chips

| | Master | Slave |
|---|---|---|
| Command port (ICW1, OCW2, OCW3) | `20h` | `A0h` |
| Data port (ICW2/3/4, OCW1) | `21h` | `A1h` |
| Vector base the BIOS programs (ICW2) | `08h` → INT 08h–0Fh | `70h` → INT 70h–77h |
| Lines | IRQ0–7 | IRQ8–15 |

The slave's INT output is wired to the master's **IR2**. So:

- A slave line can only reach the CPU if the master's **IRQ2 is unmasked**.
- Delivering a slave line puts **IRQ2 in service on the master** as well as the line
  itself on the slave, and **both** need an EOI — the handler sends one to `A0h` and
  one to `20h`. A handler that EOIs only the slave leaves the master's IR2 in service
  and every subsequent slave interrupt is blocked silently.

⚠ **IRQ2 is also the AT's rerouted slot line.** An ISA card wired to slot IRQ2
actually arrives as **IRQ9**, which the BIOS's INT 71h stub redirects to INT 0Ah. A
probe that masks "IRQ2" to silence a card must mask IRQ9 as well.

---

## 3. Initialisation — the ICW sequence

Writing the command port with **bit 4 set** is ICW1 and begins initialisation. The
chip then expects ICW2, ICW3 and (if asked for) ICW4 on the **data** port, in order.

| Word | Port | Fields |
|---|---|---|
| **ICW1** | cmd | bit 4 = 1 (marks ICW1) · bit 0 **IC4** = ICW4 will follow · bit 1 **SNGL** = single, no slave · bit 3 **LTIM** = level-triggered |
| **ICW2** | data | the **vector base**; the low three bits are supplied by the line number |
| **ICW3** | data | master: a bit per IR line that has a slave attached (`04h` on a PC). slave: its own cascade **identity** (`02h` on a PC) |
| **ICW4** | data | bit 0 **µPM** = 8086/8088 mode · bit 1 **AEOI** = auto-EOI · bit 3 **BUF** · bit 4 **SFNM** |

**ICW1 does more than start the sequence, and the side effects are the part that gets
dropped.** Per the datasheet it also:

1. **clears the IMR**,
2. **resets the edge-sense circuit**,
3. assigns IR7 the lowest priority (priority rotation is reset),
4. sets the slave-mode address to 7,
5. **clears Special Mask Mode**, and
6. **sets the status read to IRR** — i.e. the next base-port read returns the IRR
   regardless of what OCW3 last selected.

⚠ (1) and (6) are guest-visible and independent. A model that clears the mask but not
the read-select will answer a post-init `IN 20h` with the ISR if the guest happened to
have selected it earlier.

**What a PC's BIOS writes:** master `11h, 08h, 04h, 01h`; slave `11h, 70h, 02h, 01h`.
Edge-triggered, cascaded, 8086 mode, **no auto-EOI** — so on a PC every handler must
EOI, and the ISR is real.

---

## 4. Operation Command Words

### OCW1 — the mask. Data port, any time after initialisation.

A plain read/write of the IMR. Masking a line does **not** clear a pending request:
unmask it again and the interrupt arrives.

### OCW2 — the EOI and rotation family. Command port, bits 4:3 = `00`.

| Bits 7:5 | Command |
|---|---|
| `001` (`20h`) | **Non-specific EOI** — clears the *highest-priority* ISR bit |
| `011` (`60h`) | **Specific EOI** — clears the bit named by L2:L0 |
| `101` (`A0h`) | Rotate on non-specific EOI |
| `111` (`E0h`) | Rotate on specific EOI |
| `100` (`80h`) / `000` | Set / clear **rotate in auto-EOI** mode |
| `110` (`C0h`) | **Set priority** — L2:L0 becomes the *lowest* priority |

⚠ **Non-specific EOI clears the highest-priority bit that is set, which is not
necessarily the line you are handling.** It is the right call in a fully nested
system and the wrong one the moment priorities have been rotated.

**Priority rotation** exists so no device can starve: after a rotate-on-EOI the line
just serviced becomes the lowest priority. A PC BIOS never rotates, and fixed priority
— IRQ0 highest, IRQ7 lowest, IRQ8–15 sitting at IRQ2's position — is what every DOS
program assumes.

### OCW3 — read select, poll, special mask. Command port, bits 4:3 = `01`.

| Bits | Field |
|---|---|
| 1:0 | **RR / RIS** — with bit 1 set, bit 0 chooses which register the *next* base-port read returns: `0` = IRR, `1` = ISR. Bit 1 clear leaves the selection alone. |
| 2 | **P — Poll** |
| 6:5 | **ESMM / SMM** — `11` sets Special Mask Mode, `10` clears it |

**The Poll command** is an alternative to vectoring: write OCW3 with P = 1 and the
*next* read of the command port returns a byte whose **bit 7 = 1 if an interrupt is
pending**, with bits 2:0 giving its level. The read **acts as an acknowledge** — it
sets the ISR bit and clears the IRR bit, exactly as a real acknowledge cycle would.
It is how software drives the chip with interrupts disabled.

⚠ **A poll read and a status read are the same `IN` on the same port.** The only thing
that distinguishes them is which OCW3 was written last, so a model that implements the
status read and ignores P will hand a polling guest an IRR byte that it will read as a
level number — silently, and plausibly.

**Special Mask Mode** inverts the rule in §1 for masked lines: while SMM is set, a line
that is *masked* no longer blocks lower-priority lines from being delivered, even
though its ISR bit is set. It is how a handler lets lower-priority interrupts in while
it finishes.

---

## 5. Fully nested mode, and the cascade trap

In the default **fully nested** mode the slave's own in-service bit *and* the master's
IR2 bit are both set while a slave line is handled. Because IR2 is in service on the
master, **no other slave line can get through** — even one of higher priority on the
slave — until the master is EOI'd.

**Special Fully Nested Mode** (ICW4 bit 4) is the fix for exactly that: with SFNM set
on the master, a higher-priority slave line *is* allowed through while IR2 is in
service. A PC's BIOS does **not** set SFNM.

⇒ **A model that lets a second slave line through while IR2 is in service is
implementing SFNM whether it means to or not.** The two are distinguishable by a guest
and the default is the restrictive one.

---

## 6. What this means for a host that injects interrupts

We do not have a wire; the host decides when to vector a line into the guest. The
chip's contract still has to hold, and it reduces to three questions asked in order:

1. **Is it masked?** (IMR)
2. **Is it, or anything of higher priority, in service?** (ISR — §1)
3. **For a slave line: is IRQ2 usable?** (master IMR bit 2, and §5)

...and one obligation: **set the ISR bit at delivery unless AEOI is programmed**, and
clear it only on an EOI from the guest. Anything the host vectors that the guest will
*not* EOI — a do-nothing stub, a BIOS service the host answers itself — must be
auto-EOI'd by the host, or that line's priority level is dead for the rest of the run.

---

## Sources

- **Intel 8259A Programmable Interrupt Controller datasheet** — the register model,
  the ICW/OCW encodings, poll, SMM, SFNM, rotation, and the ICW1 side-effect list.
- **IBM Personal Computer AT Technical Reference** — the two-chip cascade, the port
  and vector assignments, and the IRQ2/IRQ9 rerouting.
