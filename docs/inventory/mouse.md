# Mouse — INT 33h

> **Carried over from `docs/PARITY.md` on 2026-09-23**, which is now retired — this is
> the same measured data in the inventory's home. See
> [`README.md`](README.md) for the method and the status vocabulary.
>
> ⚠ **The `state` column below is still PARITY's four-state vocabulary**
> (`missing` / `guessed` / `implemented` / `verified`). Re-marking it against the code
> in IMPL / PART / STORE / MISS / N-A, with a `file:line` per row, is owed — and is the
> point of doing it: a row that reads `implemented` here may well be **PART**, which is
> the state that returns a plausible wrong answer.

# Mouse — INT 33h (`p_mouse.asm`)

Compared against the real Microsoft `MOUSE.COM` 6.24 on 6.22. All contract rows
**AGREE** as of 2026-09-15; five rows carry recorded abstentions (below).

| item | state | notes |
|---|---|---|
| `AX=0000h` reset ⇒ `AX=FFFF`, `BX=2` | verified | |
| **reset centres the pointer** | verified | **was wrong** — see below |
| `AX=0003h` position + buttons | verified | `320,96` after reset in mode 3 |
| **`AX=0004h` set position, read back** | verified | **snapping was missing** — see below |
| text-mode **cell snapping** to 8 virtual px | verified | `100,50` → `96,48`; `101,51` → `96,48` |
| `AX=0007h/0008h` ranges and **clamping** | verified | high and low, both axes |
| `AX=000Bh` relative motion, zeroed by the read | verified | |
| `AX=0005h/0006h` press/release counts | verified | |
| `AX=001Ah/001Bh` sensitivity get/set pair | verified | stored and returned, all three fields |
| `AX=0021h` software reset | verified | |
| `AX=0001h/0002h` show/hide | verified | survivable, no fabricated answer |
| `AX=000Fh` mickeys per 8 pixels | verified | does not disturb the position |
| `AX=0024h` version/type `CL` | verified | `04FF` — PS/2, and the real driver answers `FF` not `0` |

### ★ The gap this found and closed (s72)

**The driver's virtual screen was derived from the PRESENT SURFACE, not the video
mode.** Every helper used `g_vid.frame.w/h` — the dimensions of the snapshot the UI
thread presents. That surface does not exist until something has been drawn, so in a
headless run (and in the window between a mode set and the first present)
`frame.h == 0`, and then:

* `i33_text()` evaluated **false** in a genuine text mode (`0 > 200`), so the whole
  640x200 text-mode scaling never engaged;
* `i33_vmaxy()` fell back to **479**, in a twenty-five-row text screen.

Measured: after a reset in mode 3 the real driver reports `y=96`; we reported `240`
— off the 640x200 virtual screen entirely, i.e. **row 30 of a 25-row display**. The
`MOUSEI33` line said it in one read once it was asked to: `text=0 mkind=0 fh=0
vmaxy=1df`. Fixed by keying off `g_vid.gw/gh`, the **mode's** extent, which is also
what the real driver keys off (it hooks INT 10h and rebuilds its screen on a mode
change).

Two smaller ones alongside it:

* **reset did not centre the pointer** — it left it wherever it was, which before any
  mouse movement is the startup `320,240`.
* **no cell snapping** — the real driver quantises to its 8x8 text cell; we returned
  the exact value we were handed, so we disagreed with the driver about which cell
  the pointer was in, which is the only question a text UI asks.

⚠ **NOT YET CONFIRMED BY HAND.** This changes the coordinate path that QBasic's mouse
(user-confirmed, s71) and Doom's mouse-look run through. The probe and the off-VM
battery are green and Skyroads is on baseline, but a by-hand pass is owed.

### Recorded abstentions (`tools/dostest/oracle-rules.json`)

| row | why the oracle cannot be truth |
|---|---|
| `i33.vector/AX` | the handler's load segment — no host-independent answer exists. Kept because segment **0** (no driver) is the one answer that matters. |
| `i33.24.version/BX` | which `MOUSE.COM` is on the reference disk, not the contract. We report 8.00 deliberately (higher, so `version >=` gates open). ⚠ the standing risk is a guest then using a call 8.0 added that we lack. |
| `i33.26.maxvirt/CX,DX` | **MOUSE.COM 6.24 does not implement 26h** — the poison survives the call. We are a superset here; the row is checked for *internal* consistency instead, and it read 479 until the fix above. |
| `i33.15.statesize/BX` | each driver's private save-state size. What must hold is that 15h/16h/17h agree with **each other**. |

## Not yet inventoried (mouse)

| item | state | why |
|---|---|---|
| `AX=000Ch/0014h` event callbacks | implemented | the s71 work; needs real events, so it needs a **driven** probe (QMP mouse injection), not a static one |
| `AX=0016h/0017h` save/restore state round-trip | implemented | self-consistent by construction; not yet round-tripped in a probe |
| `AX=000Ah` text cursor masks | implemented | QBasic uses it; no reference comparison yet |
| `AX=0009h` graphics cursor shape | implemented | |
| `AX=0010h` exclusion area | guessed | never observed to matter |
| mickey→pixel motion ratio as the guest sees it | guessed | needs injected movement |
| **behaviour when a guest loads its OWN `MOUSE.COM`** | ⚠ unknown | it would install its INT 33h over ours and then talk to hardware we may not emulate. Nobody has tried it. |

---
