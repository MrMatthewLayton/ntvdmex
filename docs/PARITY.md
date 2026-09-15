# Parity by inventory

Started 2026-09-15 (session 72), per `docs/PLAN-17th.md`.

Every keyboard and mouse defect this project has paid for was in a surface nobody
had **inventoried**: the INT 09h hook that peeks port 60h and chains, the missing
Alt column, IRQ1 left in service, the BDA display fields reading zero, the text-mode
mouse row that came back doubled. The DOS services — which *were* measured against
MS-DOS 6.22, function by function — caused none of them.

A run's end-of-run report can only ever catch **"asked and refused"**. This inventory
is what catches the other three:

* **answered wrongly** — the call returns, with the wrong number;
* **never asked** — the guest never gets far enough to ask;
* **wrong timing** — the right answer, too late or too early.

## The four states

| state | meaning |
|---|---|
| **missing** | not implemented; a guest that asks gets a refusal or nothing |
| **guessed** | implemented from documentation or memory, never compared |
| **implemented** | implemented and exercised by a guest, but no reference comparison |
| **verified** | a probe compares it against a reference machine, and they agree |

**verified means the expectation was RECORDED ON THE REFERENCE, not written from
memory.** That distinction is the whole of M9, and it has bitten twice: a PIT test
written from belief certified the bug it was meant to catch, and `16.09.support`
below "agreed" on its first run purely because nothing had touched AL.

## How to add one

```bash
# 1. write tools/dostest/p_<thing>.asm using probe.inc (PROBE_BEGIN / EMIT / POISON)
nasm -f bin tools/dostest/p_<thing>.asm -o tools/dostest/p_<thing>.com

# 2. ask the reference and the subject the SAME question, and diff
python3 scripts/dosdiff.py tools/dostest/p_<thing>.com --host msdos622 --host ntvdmex
```

`dosdiff.py` runs the identical `.COM` on each host and lines the answers up. The
oracles vote on truth; **NTVDMEX never votes** — it is the thing being graded. A row
that cannot be a contract (a load segment, a driver's private buffer size) gets an
abstention *with a written rationale* in `tools/dostest/oracle-rules.json`, never a
shrug.

Sidecars beside the probe:

* `p_x.deps` — files staged next to it (one per line, relative to the probe).
* `p_x.pre` — commands to run **before** it, **oracle only**. This is what loads
  `MOUSE.COM` so the oracle has an INT 33h at all. It is oracle-only on purpose:
  loading a mouse driver on *our* side would install its INT 33h over ours and
  measure the wrong thing entirely.

### ⚠ Two traps this exercise walked into on day one

1. **POISON the output registers, always.** A host that ignores a call leaves the
   register exactly as the caller passed it, and "untouched" is indistinguishable
   from "answered". `16.09.support` and `i33.26.maxvirt` both read as clean matches
   until the probe stamped `B1`/`C1C1` in first. Only ever omit POISON where the
   register is an *input*.
2. **Guard every blocking call.** An unguarded `INT 16h AH=00h` on a host whose ring
   never fills blocks forever and the probe dies as a harness timeout — an absence
   that reads as a hang instead of as data. `p_kbd.asm`'s `read16`/`read10` peek
   first and emit `AX=DEAD` for "nothing waiting".

## References, and what each is good for

| reference | good for | NOT good for |
|---|---|---|
| **MS-DOS 6.22 under QEMU** (`scripts/oracle.sh`) | INT 21h — a genuine Microsoft kernel; and any DRIVER run on it (MOUSE.COM) | the BIOS: QEMU's SeaBIOS is a rewrite |
| **PCem + genuine IBM/AMI ROM** | INT 10h/16h, the BDA, 8042 and chip timing | *(not yet running — see below)* |
| **datasheets** (8042, 8259, 8254) | chip rules; quote them in the test | anything about what a BIOS chose to do |
| **stock ntvdm on the rig** | what we are replacing, for the DOS API | devices, sound, VESA |

⚠ **The keyboard rows below are PROVISIONAL.** They were measured against SeaBIOS,
which is a reimplementation, not IBM's ROM. They are recorded as *verified against
the oracle we have* and must be re-confirmed on PCem. One row is already known to be
suspicious — see `16.01.enh` below.

⛔ **PCem is still blocked.** It launches (`scripts/pcem-fixlinks.sh` repaired the
dyld failure) but the floppy protocol times out: the GUI comes up and `A:\OUT.TXT`
never gets `[END]`, so the guest's boot / AUTOEXEC hook / floppy read are unverified.
Until that is fixed there is no BIOS-level reference on this machine.

---

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

# Video — INT 10h and the BDA block (`p_video.asm`)

Modes 03h, 01h, 07h, 12h, 13h, 06h: for each, the BDA video block, `AH=0Fh`, and a
cursor set/get round trip. **19 mismatches on the first run; all but one closed.**

⚠ **Provisional against SeaVGABIOS.** Good for "is this field written at all, and is
it the standard value"; worthless as a raster or palette reference.

| item | state | notes |
|---|---|---|
| `0040:0049` mode, `004A` columns | verified | all six modes |
| `0040:0084` rows-1, `0085` char height | verified | the fields that read zero in s71 |
| **`0040:004C` page size** | verified (06h/12h/13h) | **was a flat `0x2000`** — see below |
| `0040:0063` CRTC index port | verified | `3D4h`, and `3B4h` in mode 07h |
| `0040:0062` active page, `004E` page offset | verified | |
| `AH=0Fh` mode/columns in AX | verified | |
| **`AH=0Fh` must not clobber BL** | verified | **was zeroing all of BX** |
| `AH=02h`/`03h` cursor set/get round trip | verified | position and shape |
| **cursor shape is `0000` in a graphics mode** | verified | **was the text underline `0607`** |
| **`AH=11h AL=30h` CX** | verified | **was the requested table's height, not the screen's** |
| `0040:0065` mode-select register | ⛔ **blocked on PCem** | we say `0x29` for mode 3; SeaVGABIOS writes nothing there at all. Ours is very likely right, but "likely" is not measured. **Do not "fix" this to match QEMU.** |

### Gaps this found and closed (s72)

* **`AH=11h AL=30h` returned the wrong character height.** The classic gotcha in this
  call: `BH` selects which *table* `ES:BP` points at, but **`CX` reports the height of
  the font the screen is currently drawing with**. We returned the requested table's
  height — so `BH=0` (the 8x8 upper half) answered `CX=8` in mode 3, contradicting our
  **own** BDA byte at `0040:0085` two lines of probe output earlier. Every 43- and
  50-line editor sizes the screen from `CX`.
* **The graphics page size at `0040:004C` was a flat `0x2000` for every mode.**
  Measured: mode 06h is `0x4000` and mode 12h is `0xA000`. A program that pages by
  adding this to its offset lands inside the previous page. 06h/12h/13h are
  oracle-verified; the other graphics modes now use the standard VGA BIOS table and
  are **not** verified.
* **`AH=0Fh` zeroed the whole of BX.** `BH` is the active page; `BL` is not defined by
  the call and the real BIOS leaves it alone — we were taking the caller's `BL` with
  it. The oracle returns the probe's poison in `BL`, which is how this was seen at all.
* **The cursor shape in a graphics mode.** `AH=03h` and `0040:0060` reported the text
  underline `0607` in modes 06h/12h/13h; the BIOS reports `0000`. The stored shape is
  left alone so returning to text restores it.

### ⚠ A harness artifact this probe walked into

The first version asked *where the cursor happened to be*. That compared the two
**harnesses**, not the two hosts: the oracle redirects stdout to a file so nothing has
moved the cursor, while our run captures output and the screen cursor has walked down
the page. Six rows of confident red for no defect at all. It now **sets** the cursor
and then reads it back, which is the actual contract and is harness-independent.
▶ When a probe row differs on every single case, suspect the harness before the host.

## Not yet inventoried (video)

| item | state | why |
|---|---|---|
| palette / DAC contents after a mode set | implemented | needs PCem: QEMU's VGA is not a palette reference |
| `1112h`/`1111h`/`1114h` row counts end to end | implemented | s71 work; `p_video.asm` covers the resulting BDA but not each call |
| the raster (split, retrace timing, 0x3DA phase) | implemented | needs real hardware; QEMU shows one palette per frame |
| page flipping (`AH=05h`) across the new page sizes | guessed | the page-size fix makes this worth asking |
| non-verified graphics page sizes (04h/05h/0Dh/0Eh/0Fh/10h/11h) | guessed | standard table; no guest we run uses them |

---

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

# DOS file services — the FCB parser (`p_fcb.asm`, `p_find.asm`)

| item | state | notes |
|---|---|---|
| **`AH=29h` expands `*` into `?`s** | verified | **was stored literally** — `*.BAS` → `00 3F×8 'BAS'`, `*.*` → `00 3F×11` |
| `AH=29h` drive field, AL wildcard flag | verified | |
| `AH=0Fh/10h/11h/12h/13h/16h` FCB open/close/find/delete | verified | |
| `AH=4Eh/4Fh` find-first/next, error codes | verified | `AX=18` no-match in an existing dir, `AX=3` missing dir |
| a FAILED `4Eh` leaves a live search alone | verified | ⚠ mismatched in ONE run and clean in three since, nothing changed — the *first run after a deploy* pattern |

**The gap (s72):** `AH=29h` stored `*` literally. QBasic parses its file pattern with
29h and then matches each directory entry against the parsed FCB, so nothing matched
and its Open dialog listed **no files** while the directory pane beside it was correct.
⛔ I twice guessed QB used the **FCB search**; it enumerates with `AH=4Eh/4Fh`. One
trace line settled what two rounds of reasoning had not.

---

# Next, in order

1. **The DPMI/protected-mode IRQ0 arm**, which still auto-EOIs and which `p_pic.asm`
   cannot reach — it is Doom's path and by-hand only. A PM-entering probe would close
   the last hole in the interrupt inventory.
2. **Unblock PCem**, then re-ask every ⚠ and ⛔ row above — `16.01.enh` (does `AH=01h`
   skip an enhanced key?) and `bda.crtc/CX` (`0040:0065`) are the two that are
   *blocked*, not merely provisional.
3. A **driven** mouse probe (QMP `input-send-event`) for the callback surface
   (`AX=000Ch`/`0014h`), which is the half `p_mouse.asm` cannot reach.
4. **By hand on the rig:** the s72 mouse coordinate change (reset centring + cell
   snapping) is the path QBasic's mouse and Doom's mouse-look run through. Probes and
   battery are green; a human has not looked at it.
