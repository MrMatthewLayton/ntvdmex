# Interrupt controller — the 8259 as a handler sees it

> **Carried over from `docs/PARITY.md` on 2026-09-23**, which is now retired — this is
> the same measured data in the inventory's home. See
> [`README.md`](README.md) for the method and the status vocabulary.
>
> ⚠ **The `state` column below is still PARITY's four-state vocabulary**
> (`missing` / `guessed` / `implemented` / `verified`). Re-marking it against the code
> in IMPL / PART / STORE / MISS / N-A, with a `file:line` per row, is owed — and is the
> point of doing it: a row that reads `implemented` here may well be **PART**, which is
> the state that returns a plausible wrong answer.

# Interrupt controller — the 8259 as a handler sees it (`p_pic.asm`)

10 fields, all **AGREE** as of 2026-09-15. Nothing was broken here — which is the
result, because this is where the two worst bugs in the project lived and neither had
a regression test until now.

| item | state | notes |
|---|---|---|
| mask write/read-back, master and slave | verified | `0FCh` / `0FFh`, restored immediately |
| ISR reads zero when nothing is in service | verified | a stuck bit = the next interrupt of that priority never arrives |
| **IRQ0's ISR bit is SET inside a hooked INT 08h** | verified | before the BIOS EOIs |
| **...and CLEAR after the BIOS EOIs** | verified | the EOI is what clears it |
| ★★ **the handler is never RE-ENTERED** | verified | **this is the Lemmings bug as a number.** Auto-EOI on delivery re-entered the ISR once per tick, for ever |
| masking IRQ0 actually stops delivery | verified | self-calibrating spin: measures two ticks, then spins the same amount masked |
| unmasking resumes it | verified | |

⚠ **This is the V86/real-mode delivery arm only.** The **DPMI (protected-mode) arm
still auto-EOIs IRQ0** — that is Doom's path and it is by-hand only, so this probe
says nothing about it. See [[irq0-must-be-held-in-service]].

⚠ **IRQ1's in-service behaviour is NOT covered.** The s71 bug (our INT 09h arm never
EOI'd, so a guest that chains to the BIOS typed one character and went deaf) needs a
*keypress* to reproduce, and the oracle has nobody at the keyboard. It is pinned
off-VM instead (input battery T9/T11) and would need a rig-only keyed run to compare.

---
