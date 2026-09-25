# Session 79 — XP's own COMMAND.COM becomes an interactive shell

**Date:** 2026-09-25 · **Branch:** `m9/completeness` · **Rig:** bare-metal XP box, watcher live.
**Host builds this session:** `3eb6f8dc` (the knob) → **`63e21be9`** (the fix).

---

## The result

**XP's own `C:\WINDOWS\System32\COMMAND.COM` runs as an interactive DOS shell under
NTVDMEX.** Prompt, `ver` → `MS-DOS Version 5.00.500`, `dir` with volume serial and free
space, cursor waiting at the next prompt. **991 prompts per 30-second run became 3.**

That answers a *product* question, not just a code one: MS-DOS 6.22's shell is
Microsoft's and cannot ship in a public repo, so a stock XP box was always going to fall
back to the shell it already has. It now works.

⛔ **Not on by default.** Two `INT 21h AH=53h` answers have to change with it and they
contradict the only stock measurement we hold, so they live in `cfg\int53.txt`. See
*The open question* below.

---

## How it went, in order

### 1. Static analysis settled what five sessions of instrumenting had not

Nothing about the first half of this needed the rig. `tools/ntvdm/cmdcom.py` re-derives
from COMMAND.COM's own bytes:

* the transient origin (anchored on the single `C4 C4 54 01` against the host's measured
  `9342:03CE`, so it is computed, not remembered),
* every BOP site,
* every instruction that reads or writes the state block `[0x320..0x32F]`,
* every caller of the read-a-line routine and the `AL` each passes.

Three beliefs fell out of it immediately:

| belief before | what the image says |
|---|---|
| `[0x32A]=1` "IS THE LOOP" | `[0x32A]` has **one writer in the whole binary**, and it writes **0**. The `1` is the image's linked-in default and it **passes** the gate. |
| `[0x327]` "does not mean what I read it to mean" | It means exactly that. Both `AL=0` (keyboard-reading) callers of `0x0A0D` sit behind `cmp byte [0x327],1 / jz away`; there is **no other path** to a keystroke. |
| the loop had no identified top | It is `INT 21h AX=5302h` at transient `0x0341`, and the branch is its **carry flag**. |

### 2. Three rig runs, one variable each

The host got a `cfg\int53.txt` knob whose **defaults are the measured values**, so a run
with no file is byte-for-byte the old behaviour. Then:

| `AL=2` | `AL=5` | result |
|---|---|---|
| `CF=0` (default) | `AL=1` (default) | 32× `sub 01`, 32× `sub 0E`, `sub 0x10` never reached — the baseline, reproduced exactly on the new build |
| **`CF=1`** | `AL=1` | **identical** — the chain bails at its first gate |
| **`CF=1`** | **`AL=0`** | `sub 0x10` fires for the first time ever; the 1.25M spin stops dead |

Both answers are needed, and the prediction that only one of them would move anything
was itself a prediction that came true. That is the value of running the single-variable
step you expect to do nothing.

### 3. …and then the real bug, which was ours

With the gates open, COMMAND.COM printed **991 prompts and read nothing**. The trace:

```
INT21 AH=0A line max=00 n=00 [0d ]            x991
```

Our `BOP 0x54 sub 01` "no command" answer wrote `c[0] = 0`. That reading of `[0]` came
from the guest's own copy loop at transient `0x06C7` (`mov cl,[0x9327] / add cx,3 /
rep movsb`), which really does treat `[0]` as a count — and it is completely wrong about
the buffer, because **the same buffer is handed to `INT 21h AH=0Ah`**:

```
transient 0x018D   mov byte [ss:0x9327],0x80    <- ONCE, during start-up
transient 0x0A21   mov dx,0x9327 / mov ah,0Ah / int 21h
```

`0x80` is DOS's buffered-input **maximum**. We zeroed it on the first BOP; `maxn - 1`
became `-1`; our `AH=0Ah` returned an empty line without waiting; COMMAND.COM saw the
`CR` and printed its prompt again.

Fix: **write the length at `[1]`, where DOS puts it, and never touch `[0]`.** The request
block's `+0x0C` already told us the capacity was `0x0080` — two independent sources, and
the one we overwrote was the same number.

```
INT21 AH=0A line max=80 n=03 [76 65 72 0d ]   <- "ver"
INT21 AH=0A line max=80 n=03 [64 69 72 0d ]   <- "dir"
```

---

## The open question, and why it is not closed

XP's COMMAND.COM cannot read a key while `[0x327]=1`. `[0x327]`'s only writer is
`mov al,5 / mov ah,53h / int 21h / mov [0x327],al`. Stock's shell **is** interactive.
Therefore stock answers `AL=0` when the shell asks — and our probe measured `AL=1`.

⛔ **The likely difference is our own harness.** `probe.inc` reports through `INT 21h
AH=02`, and every stock measurement this project holds was captured with `> FILE`. So we
asked the oracle *"is this console interactive?"* with its own output redirected.

**A probe that reports through stdout cannot measure anything that depends on stdout.**

`tools/dostest/p_int53f.asm` asks the same eight questions through `AH=3Ch/40h/3Eh` and
needs no redirection at all (`%define PROBE_FILE` before the include; proved inert —
`p_ver`, `p_misc` and `p_unimp` all reassemble byte-identical to their committed
binaries). It has been verified end-to-end **against our own host** on the rig.

▶ **Owed from a human:** run it against **stock ntvdm**, both with and without `> FILE`,
and diff. The IFEO bracket is the documented rig-bricking hazard and was deliberately not
run unattended. If it confirms `AL=5 → 0` and `AL=2 → CF=1`, promote them to the built-in
defaults and delete the knob's reason for existing.

---

## Lessons worth carrying

* ⛔⛔⛔ **A field you did not write is not a field you cannot break.** `[0]` had a
  correct, evidenced reading (the guest's own copy loop) that was true of that loop and
  wrong about the buffer. **Ask what ELSE reads the bytes you are about to write.**
* ⛔⛔ **`int16=[0,0,0,0]` was never evidence.** Our `AH=0Ah` reads the host key ring and
  never issues `INT 16h`, so that counter reads zero on a shell that works perfectly — it
  reads zero in the successful run too. It was quoted as proof of "nothing consumes them"
  for two sessions. **Before quoting a counter as absence, check that success would move
  it.**
* ⛔ **The earlier `AL=5 → 0` experiment was graded on the wrong symptom.** It looked for
  the banner, which `[0x326]` blocks independently, so it could not have shown the
  keyboard path opening even though it did. **A negative result is only as good as the
  observable you chose.**
* ★ **Run the single-variable step you expect to do nothing.** `CF=1` alone changing
  *nothing* is what proved `[0x327]` was the first gate rather than one of several.
* ★ **The binary outranked five sessions of instrumenting.** Every conclusion here came
  from ~50 KB of guest code that was on disk the whole time. `cmdcom.py` exists so the
  next person does not have to re-read it by hand — and so an off-by-two in a
  disassembler's output cannot survive into a doc, which it did twice today.

---

---

## Second result: the Win16 `GetWinFlags` mismatch was a DPMI bug in a Win16 costume

The first deterministic Win16 test this project ever ran found one defect:
`kernel.getwinflags` = **`4C25` from us, `4C29` from stock**, reproduced twice. It sat
open with the note that the `WF_CPU386`/`WF_CPU486` reading was *"an interpretation from
memory, not confirmed"*, and a plausible mechanism attached (krnl386 toggling the `AC`
flag to tell a 486 from a 386).

Reading the binary took about ten minutes and refuted the mechanism outright.

`GetWinFlags` is KERNEL.132 → segment 3, offset `0x4B`, and it is essentially one
instruction: `mov ax,[0x464]`. That word is built in one place, segment 1 `0xD68A`:

```
mov ax,0x1687 ; int 2Fh      ; the DPMI installation check
or  ax,ax  ; jne -> bail
cmp cl,3   ; jb  -> bail
mov bl,4   ; CL == 3
je  +2
mov bl,8   ; CL >  3
mov [0x464],bx
```

**`CL` is the whole difference, and we hardcoded `3`** — at two sites, with a comment
reading "CL=3 (386)" written long before anyone knew what consumed it. `DPMI_CPU_CLASS`
is now `4`, and the row reads **`4C29`**.

⚠ Note what the fix does and does not rest on. "4 means 80486" is still from memory and
there is no DPMI document in this repo. What it rests on is that **stock returns `CL>3`**
(proved by its measured `4C29`) and **4 is the smallest such value** — so we match the
oracle without claiming more than the measurement supports. `tools/dostest/p_dpmins.com`
records why no cheap oracle exists: **MS-DOS 6.22 and DOSBox-X both leave every register
untouched** — neither has a DPMI host at all.

### The A/B, because CL is observable to every DPMI guest

`runs/s79_cl_ab/`, interleaved, one change:

| guest | evidence |
|---|---|
| heaven7 | `DPMI:` 704 = 704; mode sets, VESA, planar, video summaries identical |
| duke3d | `DPMI:` 1056 = 1056; `sb replay blocks_checked=4CB REPLAYED=1F1` identical |
| doom | `sb_dspwr/sb_blocks/sb_mode/sb_rate`, `pit_reload/pit_mode`, all three `VGAREG written-by-guest` lines identical |
| skyroads | `n8=0 max_ms=7` |
| zar | identical freeze at `CS:IP=0347:1327`; INT 31h 2277 = 2277 |

★ **ZAR's baseline was taken by rolling the host back, not assumed.** The *after* run went
first and looked like a regression — no mode set, frozen early — until the *before* run
showed the identical freeze. **A single run of a guest you have no baseline for is not
evidence in either direction**, and the temptation to read one as a regression is strong
precisely when you have just changed something.

### Lesson

★ **A mismatch in one layer can be a defect in another.** This was filed under WOW/Win16
for a session because that is where the symptom was measured. The cause was in the DPMI
installation check, three layers down, in a value chosen as a placeholder years earlier.
**Follow the value, not the subsystem.**

---

## Verification run this session

* **Off-VM battery:** 1591 checks, 0 failed — BATTERY GREEN.
* **MS-DOS 6.22's `COMMAND.COM`:** banner, `ver`, `dir` — unchanged, no regression.
  DOS version correctly back to **6.22**, source printed as the registry.
* **Skyroads:** `n8=0 max_ms=7`, `pacer_prio=0 joy_thread=0 pit_split=1`, no anomalous
  IRQ0 gaps.
* **Doom:** mode 13h set by the guest, SB streaming (`sb_blocks=0xA9E`), INT 33h engaged.
* ▶ **Owed from a human:** the usual by-hand pass. The command-tail fix is on a path only
  XP's COMMAND.COM reaches, and the `AH=53h` defaults are byte-identical to before.

## Commits

`fb8a501` · `9effc64` · `5fa4286` · `bbcc0f6` · `f882732` · `096c313`
