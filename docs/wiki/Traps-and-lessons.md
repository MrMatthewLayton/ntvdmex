# Traps and lessons

The expensive things. Most of these cost at least a session; several cost five.

They sort into one shape almost without exception: **an instrument that lied.** Not a
missing feature, not a hard bug — a measurement that was confidently wrong, believed, and
steered by. If you read only one page here, read this one.

---

## The method, distilled

**1. Build ground truth from something that is not you.**
The oracles are: the game's own data files (`tools/doomoracle` reads Doom's WAD), a real
MS-DOS 6.22 under QEMU, **stock `ntvdm` itself**, and Nuked-OPL as a black box. Oracles
vote on truth; agreement between them *is* truth. **NTVDMEX does not vote** — it is the
subject under test, and letting it into the consensus is circular. Disagreement between
oracles is reported as DISPUTED, never resolved by majority.

**2. Read the guest binary.**
After ~20 Doom runs of host instruments with nothing improving, disassembling `DOOM.EXE`
fixed the status bar within the hour: `I_ReadScreen` cycles GR4 and never writes the map
mask, so every read was served from the *write* plane. Every exclusion made beforehand was
about writes. They were all correct and the **category** was wrong.

**3. When a guest dies at an address, diff the bytes there against the file on disk.**
This found a bug that had killed Doom for five sessions and was entirely ours:
`dpmi_patch_code_region` matched `CD nn` as a byte pair and rewrote a `jle`'s
*displacement*. Not a DOS bug, not a DPMI bug — our own patcher corrupting the guest.

**4. A trace that prints the REQUEST but not the ANSWER is half an instrument.**
The in-guest redirection bug ([#133](https://github.com/MrMatthewLayton/ntvdmex/issues/133))
is still open precisely because three fixes were guessed before anyone printed what
`AH=3Ch` *returns*. The handle number is the whole question.

**5. Predict the number before the run.**
If you cannot say roughly what a counter should read, you cannot tell a wrong answer from
a surprising one.

**6. A fix measured on one guest is a fix for none.**
A PIT throttle validated on Doom cost **Skyroads 24% of its clock** for two sessions. No
counter flagged it — the user's ear did. After touching any shared path, re-run a guest
from the *other* class (V86 vs DPMI).

---

## Instruments that lied, specifically

**A counter's LAYOUT is a claim.** An entire root cause — "the timer starves the audio" —
came from printing an async-only counter next to `raises`. Real delivery was 91%, not 39%.
*Neither number was wrong.* The adjacency was the lie. Also in the same family: `owed_max`
was a high-water mark being read as a current value, and `why=0` meant both "success" and
"never set".

**A metric that moves the wrong way.** A home-made even-column video metric got *worse*
when a real bug was fixed, and was steered by for several rounds before being replaced with
the WAD oracle.

**A stale artefact is worse than a missing one.** Nothing collected `sb.raw` off the rig, so
a "before" run and an "after" run analysed the *same hours-old file* and produced
byte-identical histograms. **Delete the destination before the run, and `md5` any two
artefacts you call before/after.**

**An instrument must not infer its own frame.** `sbref.py` guessed the DMA block grid (real
boundaries are `15 + n*256`) and reported a burst span in plane offsets that was never
converted to rows. Two wrong root causes, both refutable from data already on disk.

**The newest example, from the session that wrote this page.** `dlgcheck.py` — a fresh tool
for verifying dialog layout — reported 47 problems, of which **45 were its own fault**: a
`COMBOBOX`'s height in a dialog template is the height of its *dropped list*, not the closed
control. An instrument's model of its subject is a claim, and must be checked before its
output is believed.

**The rig is not necessarily unattended.** A window was observed minimising itself during a
capture-toggle test and was filed as an unexplained anomaly. It was the user, at the box,
using the window. Four clean re-tests said "not reproducible" — the right conclusion for
the wrong reason. Desktop-level observations (focus, z-order, minimize, cursor) are not the
program's behaviour until you have established nobody is touching the machine.

---

## Conclusions that were confidently wrong

Kept because the pattern matters more than the facts.

| Filed as | Actually |
|---|---|
| "Doom never asks for mouse input" | Doom was calling AX=3/0Bh **2915 times in 45 s**. Both filed explanations were wrong too. The cause was a 32-bit `EDI` masked to 16 bits, so we read the function number out of, and wrote answers into, junk memory. The tell: 2595 calls reading back the value *our own reset* had written. |
| "The timer starves the audio" (session 23) | Refuted at the first link by session 24. The echo was real, smaller than reported, and had a different mechanism. |
| "Doom dies because the stretch is too long" (session 20) | Refuted by session 21. It was our own INT-site patcher. |
| "`AH=53h` is why XP's COMMAND.COM exits" | Tested and **refuted** — answering success with a zeroed DPB gave the identical CS:IP after the identical 32 ms. Reverted rather than left in. |
| "The kernel won't run PM" (session 8) | It was our own `dpmi_enter.S` interrupt-pending guard firing on a stale value. |
| "`Control\WOW\cmdline` is the interception point" | It is not. IFEO `Debugger` is. |
| "Input latency regressed 25→66 ms" | The headless rig runs with nobody typing, so every input-latency counter measured a path no key travels. The comparison was interactive-vs-headless. |

---

## Platform traps

- **VME is on**, so a V86 guest's `STI` sets **VIF (bit 19)**, not IF. Any gate testing IF
  alone is wrong.
- **Never poke `[0x714] |= 1`** — it livelocks guests.
- **DPMI initial selectors must be D/B=0 even for a 32-bit client.** The RETF-on-failure
  path proves the client's post-switch code is still 16-bit. Setting D/B=1 ran DOS/4GW's
  stub as 32-bit — wild writes through ESI, silent death. *Our own tests hid this and
  regressed when it was fixed.*
- **EIP is only 16-bit when its selector is.**
- **Never swallow a key-UP** in a low-level keyboard hook. Capture is entered while the hook
  is not yet installed, so eating the *up* leaves Windows believing Win is held forever —
  after which every keystroke becomes Win+key, and pressing D minimises your window.
- **A minimized window is still `WS_VISIBLE`.** It lists, `FindWindow` finds it, and it
  screenshots as bare desktop.
- **`windres` uppercases window class names** — `SysTabControl32` is stored as
  `SYSTABCONTROL32`. Harmless (class lookup is case-insensitive) but grepping the binary for
  the mixed-case name finds nothing and looks like the control was dropped.
- **The build is no-CRT on purpose.** The toolchain is UCRT-default and UCRT is absent on XP.
  Also: GCC will happily turn your hand-rolled `strlen` loop into a *call to `strlen`* unless
  you pass `-ffreestanding -fno-builtin`.

---

## Rig traps

Each of these cost a reboot or a session. See [The bare-metal rig](The-bare-metal-rig).

- **Two EXEs.** `ntvdmhost.exe` is the host; `ntvdmex.exe` is a small launcher. Deploying the
  wrong one makes the *launcher* the IFEO debugger, which relaunches into itself — and runs
  still "succeed" because the harness copies a stale log. **Checksum what you deploy.**
- **Never edit a Windows `.bat` in Python text mode on macOS** — it strips CRs, `cmd.exe`
  breaks on `goto`, and the whole watcher loop dies one iteration in.
- **A swallowed `copy` error runs a stale binary.** `copy ... >nul` hid a failure while an
  interactive host still held the target open. The only symptom was one missing log line.
- **SMB attribute caching lies about mtime and size.** Don't conclude "the run didn't happen"
  from one stale `stat`.
- **The mount drops silently** and writes then go to a local directory that shadows the
  mountpoint, where everything succeeds against nothing.
- **`watcher.txt` merely existing means nothing** — a dead watcher and a busy watcher look
  identical. The only reliable "it started" signal is the command file being *consumed*.
