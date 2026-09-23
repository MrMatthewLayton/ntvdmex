# Misc BIOS — INT 11h, 12h, 1Ah, 1Ch

> **Carried over from `docs/PARITY.md` on 2026-09-23**, which is now retired — this is
> the same measured data in the inventory's home. See
> [`README.md`](README.md) for the method and the status vocabulary.
>
> ⚠ **The `state` column below is still PARITY's four-state vocabulary**
> (`missing` / `guessed` / `implemented` / `verified`). Re-marking it against the code
> in IMPL / PART / STORE / MISS / N-A, with a `file:line` per row, is owed — and is the
> point of doing it: a row that reads `implemented` here may well be **PART**, which is
> the state that returns a plausible wrong answer.

# Misc BIOS — INT 11h, 12h, 1Ah, 1Ch (`p_bios.asm`)

| item | state | notes |
|---|---|---|
| `INT 12h` conventional memory | verified | `027Fh` = 639 KB, same as the oracle |
| `INT 1Ah AH=00h` tick count advances | verified | |
| `INT 1Ah AH=00h` midnight flag | verified | `0`, and cleared by the read |
| **`INT 1Ah AH=02h` RTC time in BCD** | verified | **was unimplemented** — see below |
| **`INT 1Ah AH=04h` RTC date in BCD** | verified | **was unimplemented** |
| ★ **`INT 1Ch` is called, once per tick, never re-entered** | verified | the vector a game actually hooks |
| `INT 11h` equipment word | abstained | describes the machine; ours says 2 serial ports because 2 are fitted. Must stay consistent with `vdd_comm_fitted()` |
| `INT 1Ah AH=03h/05h` set time/date | **missing, deliberately** | we cannot move the host clock, and accepting with `CF=0` would be the "runs but lies" shape — `TIME` would report success and change nothing. Unmeasured on the reference too |
| `INT 1Ah AH=06h/07h` alarm | missing | never asked by a guest we run |

### The gap this found and closed (s72)

**`INT 1Ah` AH=02h and AH=04h fell into `default:`** — a comment reading "RTC subfns
not modelled yet" — which leaves every register exactly as the caller passed it. The
probe poisons CX/DX and got the poison **straight back** (`C1C1`/`D1D1`) where the
oracle answers with the time and date. Guests use these for file timestamps, save-game
dates and as a seed. **BCD is the contract**: a guest reads them as BCD because that
is what a BIOS returns, so a binary 34 would be read as 22.

The clock is now **host-supplied** through a `rtc_now` hook on the PIT VDD rather than
`<time.h>` — which the XP-targeting CRT does not link anyway, and which would have put
libc time inside a portable VDD. With no clock installed the call is still **not
answered**: fabricating a date is worse than silence, because a guest would stamp
every file with it. The battery injects a fixed instant so the BCD conversion is
pinned rather than read off the wall.

### ⚠ A second harness artifact, same shape as the video one

`int1a.00.advances` reported **"the clock does not advance"** on the rig and nowhere
else. It was the probe: a 400-unit spin is longer than a tick on the emulated 486 and
*shorter* than one on the rig, so the probe gave up before the counter moved. Raising
the bound to the one every other wait uses made it agree. ▶ Same lesson as the cursor
rows: **when a row fails on one host only, check the probe's own assumptions about
time before you touch the host.**

▶ And a third, in the battery rather than a probe: the first `int1a/02` check asserted
`0x1729` for 23:41, which is simply wrong arithmetic — BCD 23:41 is `0x2341`. It
failed on its first run, which is exactly why the expectation is written down and
executed rather than reasoned about.

---
