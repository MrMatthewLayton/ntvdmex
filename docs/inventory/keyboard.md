# Keyboard — INT 16h, the BDA and the 8042

> **Carried over from `docs/PARITY.md` on 2026-09-23**, which is now retired — this is
> the same measured data in the inventory's home. See
> [`README.md`](README.md) for the method and the status vocabulary.
>
> ⚠ **The `state` column below is still PARITY's four-state vocabulary**
> (`missing` / `guessed` / `implemented` / `verified`). Re-marking it against the code
> in IMPL / PART / STORE / MISS / N-A, with a `file:line` per row, is owed — and is the
> point of doing it: a row that reads `implemented` here may well be **PART**, which is
> the state that returns a plausible wrong answer.

# Keyboard — INT 16h and the BDA (`p_kbd.asm`)

32 fields, all **AGREE** with the 6.22 oracle as of 2026-09-15.

| item | state | notes |
|---|---|---|
| ring geometry `0040:0080/0082` | verified | `001E`..`003E`, sixteen 2-byte slots |
| `0040:0096` bit 4 enhanced-keyboard | verified | set; a zero here makes a guest use the 83-key subset |
| empty ring ⇒ `AH=01h/11h` ZF=1 | verified | empty is `head == tail` |
| `AH=01h` peek does not consume | verified | head unmoved, tail one on |
| `AH=00h` read consumes, advances head | verified | |
| `AH=10h/11h` enhanced read/peek | verified | F11 `8500h` returned intact |
| extended key with `AL=0` (`4B00h` Left) | verified | ordinary calls DO return these |
| ring **wrap** at the last slot | verified | tail returns to `001E`, order preserved |
| **`AH=05h` push into the ring** | verified | **was MISSING — see below** |
| `AH=05h` full ⇒ `AL=1` | verified | 16 slots hold 15; the 16th is refused, head survives |
| `AH=02h/12h` shift status | verified | reads the same `0017`/`0018` the guest can |
| **`AH=09h` supported-function mask** | verified | **was MISSING**; `0x30`, measured not derived |
| `AH=03h` set typematic | verified | answered; `CF=0` |
| `16.01.enh` — does `AH=01h` SKIP an enhanced key? | ⚠ **provisional** | oracle says **no** (returns `8500h`, head unmoved) and we match it. The documented IBM rule is that `AH=00h/01h` skip codes only a 101-key keyboard can make. **SeaBIOS may simply not implement the skip.** Re-ask on PCem before trusting either behaviour. |

### Gaps this found and closed (s72, `c5c3e60`)

* **`AH=05h` did nothing at all.** It fell into the `default` arm, which sets ZF and
  leaves AX exactly as passed — so a key-stuffing program read its **own** byte back
  out of AL and took it for success, and nothing was ever queued. This is how DOSKEY,
  installers that pre-answer their own prompts, and every TSR that drives another
  program put keys in.
* **`AH=09h` likewise.** Caught only once the probe poisoned AL.
* **`AH=03h`** was answered by accident (the default arm happens not to touch CF).

## Not yet inventoried (keyboard)

| item | state | why it needs PCem or a live run |
|---|---|---|
| scancode → ASCII for the whole four-column table | implemented | pinned off-VM (input battery T10) against the IBM table, not against a machine |
| port 60h/64h re-read semantics, 8042 status bits | implemented | the s71 transfer-hold; a *timing* contract, so it needs real hardware |
| typematic repeat rate as a guest observes it | guessed | host OS owns repeat; no BIOS-readable copy |
| `AH=04h` keyclick, `AH=0Ah` keyboard ID | missing | never asked by any guest we run |
| LED state `0040:0097` | missing | |
| INT 09h → IRQ1 → EOI ordering as a hooked handler sees it | implemented | `p_pic.asm` not yet written — **next** |

---
