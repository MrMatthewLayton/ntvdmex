# Project state — start here

> **This is the canonical resume point.** Read it top to bottom and you will know where the
> project is, what works, what does not, and what to do next. It is deliberately short.
> **History does not live here** — it lives in [`log/sessions/`](log/sessions/), one file per
> session. When this file starts growing session blocks again, split them out; it has
> happened twice now (`return-ntvdm.md` in August, this file in September).

- **Updated:** 2026-09-25 (session 80)
- **Branch:** `m9/completeness`
- **Checkpoint commit:** **`ff0d956`** — the rollback point, and the first one moved since
  `59fac7d`. It built **`71ef4737`**, which the user confirmed by hand on 2026-09-23:
  **typing, a Win16 app, and the text cursor**, on top of Doom and Skyroads run headlessly.
  *No git tags yet: the first will be `0.0.1` at the first beta.*
  ⚠ **That is a three-item confirmation, not a shelf sweep** — the stable zip below stays
  the anchor until someone runs the whole shelf.
  ✅ **CLEARED 2026-09-25.** Eight `src/` changes had stacked here unconfirmed — the RTC
  alarm, the 82077AA, the `AX=122Eh` tables, the BOP work, the shell fallback and the log
  caps — which broke the standing rule of one observable change per by-hand test. The user
  has now run them: **6.22's `COMMAND.COM` interactive**, and Doom, Skyroads and the rest
  of the shelf *"still working as they did before"*. `debug\prev\ntvdmhost_prev.exe` was
  promoted to that build (**`297e2172`**).
  ⚠ **The checkpoint commit above has NOT moved** — it still names `ff0d956`/`71ef4737`.
  Moving it is a deliberate act and belongs with a shelf sweep, not with one good day.
- **Stable package:** `dist\ntvdmex-20260917-4847355.zip`, host `9448cf27`
  (tag `release-20260917b`). **Immutable until a new build is confirmed across the whole
  shelf**, which 2026-09-23's three-item pass is not. `bin\` on the rig is free to churn.
- **Rollback host:** `debug\prev\ntvdmhost_prev.exe` = **`297e2172`** — promoted
  2026-09-25 after the user confirmed by hand: **MS-DOS 6.22's `COMMAND.COM` as an
  interactive prompt**, plus Doom, Skyroads and the rest of the shelf *"still working
  as they did before"*. That retires `71ef4737`, which had been the target while eight
  changes stacked on top of it.
  ⛔ **It did not exist until 2026-09-23.** The file is documented as *"the last build a
  HUMAN confirmed"* and every `bmstage --host` run printed
  `ntvdmhost_prev.exe = , the confirmed one, untouched` — **with an empty md5** — and
  nobody read it. The rollback story had no rollback in it for as long as anyone can tell.
- **Tracker:** [issues](https://github.com/MrMatthewLayton/ntvdmex/issues) ·
  **Knowledge base:** [wiki](https://github.com/MrMatthewLayton/ntvdmex/wiki) ·
  **History:** [`log/sessions/`](log/sessions/)

---

## What this is

**NTVDMEX is a replacement for `ntvdm.exe` on 32-bit Windows XP SP3.** It runs DOS programs
by executing 16-bit code on the **real CPU** in Virtual-8086 mode — reusing the NT kernel's
own VDM machinery through the undocumented `NtVdmControl` syscall — not by emulating a CPU.
It is not DOSBox and it is deliberately not a fork of anything.

**How it installs:** an Image File Execution Options `Debugger` value on `ntvdm.exe`, so every
MS-DOS *and* Win16 launch routes to NTVDMEX and stock `ntvdm` lies dormant. Not by overwriting
`System32\ntvdm.exe` — that is Windows File Protection territory — and reversible with one
`reg delete`. See [ADR-0007](decisions/0007-intercept-via-ifeo-debugger.md).

**The two bars it was built against, both met:**

- **Real DOS games, with sound** — Doom is fully playable, mouse and audio included.
- **MS Paint and Notepad from Windows 3.x** — both run and both do their job, user-confirmed.
  Paint draws in colour, keeps what it draws, and `File > Save As` writes a valid 24-bit BMP.

---

## The programme now: build from the specs, then test the apps

> **User directive, 2026-09-22:** *"The approach from the beginning should have been build the
> entire emulation layer (hardware, firmware, software) from specs … gaps implemented by what
> we're building, not what's asking for them."*

Every guest closure in this project's log was one application dying in one place, and a session
spent finding it. Each fix was correct; the *method* was not, because the application chose what
got built. The inventory drives the work now and applications are the acceptance test.

**Scope rule (2026-09-23):** a device is in the inventory because it is part of the
**period-correct hardware contract**, never because a guest asked for it. Counting how many
shelf guests touch a surface is app-driven reasoning wearing a spec-first hat.

**Two documents per surface:**

| Document | Says |
|---|---|
| [`ref/<surface>.md`](ref/) | What the hardware does — our own words, cited per section, wiki-ready |
| [`inventory/<surface>.md`](inventory/) | What **we** do — every unit marked IMPL / PART / STORE / MISS / N/A, with a `file:line` |

The full surface list, with the primary source named for each, is in
[`inventory/README.md`](inventory/README.md). **Order: VGA to completion first**, then outward.

---

## Where it actually is

### ✅ Working, confirmed by hand on real hardware

| | Status |
|---|---|
| **Doom** | Fully playable at **high detail** — 3D rendering, status bar, menus, PCM + MIDI, keyboard and mouse. Runs its own 32-bit code through DOS/4GW on real silicon. ⛔ **Low detail is broken — see below.** |
| **Duke Nukem 3D** | Runs, including its Setup program. |
| **Heretic / Hexen** | Both run; Hexen's hi-res loader renders. |
| **Wolfenstein 3D · Skyroads** | Fully playable. |
| **ZAR** | Renders, including its VESA modes, and its mouse buttons arrive. **Silent** — pinned in state 4. |
| **heaven7** | Renders (VBE 2.0 direct colour + linear framebuffer). Music is GUS-only and there is no GUS model yet. |
| **MS-DOS 6.22 `COMMAND.COM`** | Runs as a guest: prompt, line editing, internals, and an external program EXEC'd and returned from. |
| **QBasic / EDIT** | Run, including the Open dialog and building `.EXE`s. |
| **DOS API** | 103 INT 21h functions. XMS 3.0, EMS (LIM 4.0), DPMI 0.9, INT 13h, TSRs, redirection. |
| **Video** | Text (authentic IBM ROM font), mode 13h, mode 12h planar, mode Y, VESA banked + LFB, VBE 2.0/3.0 runtime. Windowed GDI + exclusive-fullscreen DirectDraw, Luna-themed. |
| **Sound** | SB16 PCM at 99.999% delivery, clean-room OPL2/OPL3 FM (MIT; Nuked used only as a black-box oracle), MPU-401 MIDI, PC speaker. |
| **Win16** | Notepad edits and saves text; MS Paint draws in colour and saves bitmaps. Real HWNDs, a turning message loop, the host calls 16-bit code. |
| **Host UI** | Menu bar, status strip, six-tab Settings dialog backed by `HKCU\Software\NTVDMEX`. |
| **Packaging** | A portable zip with `install.bat` / `uninstall.bat` / `status.bat` / `smoke.bat` / `diag.bat`, installed from scratch on three machines. |

### ⛔ Open defects

| | What is known |
|---|---|
| **Doom's low detail renders wrong** | User: *"High detail works. Low detail is broken!"* **Measured:** low detail is dominated by **two-plane map masks** (`0x03` and `0x0c`, ~143k writes each, vs ~16.9k per single-plane mask) and our mode-Y path maps the A0000 window to **one plane at a time**. The status bar loses ~70% of its detail although Doom draws it full-res either way. This is the concrete instance of the mode-Y approximation the VGA inventory predicted. **The fix belongs in VGA steps 3–4, not a special case.** [`inventory/vga.md`](inventory/vga.md) |
| **Mario's mode-Y artefacts** | A strong candidate for the same root cause as the row above. |
| **Duke3D took no keyboard on one machine** | Reproduced on a friend's Win98-era box only; works on both of the user's own boxes. |
| **DOS/4GW guests ran typewriter-slow on one machine, once** | First session on the friend's box; fine after a reboot that *also* changed the BIOS to optimized defaults. **Cause unknown and confounded**; logs unrecoverable. |
| **ZAR is silent** | Renders and plays, no audio. Eight hypotheses refuted. [`zar-dos16m`](log/sessions/) · user framing: *"if ZAR doesn't work then NTVDMEX doesn't work."* |
| **Win16 shelf** | X does not close WinMine/Charmap · Calc/Charmap/Clock draw incorrectly · Bubbles palette · Matrix_1 slow · `graphics\VS87.EXE` · MPLAYER GPF `0001:3983`. |
| **Console/stdio integration** | DOS output is buffered and flushed to `CONOUT$` at exit, so shell redirection and piping are bypassed and every DOS program pops a window. |
| **`MEM /C`** | The main report matches the 6.22 oracle row for row; `MEM /C` still says MSDOS is 1,028K, contradicting its own summary. |
| **Parity, open rows** | **7 of 677 comparable rows, every one accounted for.** `p_tsr` paras-still-held · `xms.08` BH (undefined by spec) · `p_vgamem` the one 13h→unchained case (mode-Y, parked) · `kbc.outport.d0` bits 2/3/6/7 (**one oracle — recorded, deliberately unfixed**) · `dma.status.idle` (channel 2's TC — needs the FDC's data path, not the DMA model) · `fdc.alt.3f6` (**the ATA surface's, not the FDC's**) · `fdc.dumpreg` byte 1 (where the head is; not adjudicable). |
| **`/uninstall` can lock the user out** | It refuses when the IFEO value names a third binary, which the displaced-value restore can produce. Needs a `/force` or a message naming the path. |

### ⏸ Parked

- **Windows 2000** — the host loads (four XP-only imports bound at run time, `ffebdab`), but the
  `NtVdmControl` / WOW contract is unmeasured there. Parked at the user's request; the user's
  second box triple-boots 98/2000/XP and they test by hand.
- **Windows 7** — on the list, 32-bit only. No machine.

---

## ▶▶▶ START HERE — the three north stars, set by the user 2026-09-25 (end of s79)

**Agreed order: `3 → 1 → 2`.** s80 started #3. Full account: [`log/sessions/session-80.md`](log/sessions/session-80.md).

| # | North star | State |
|---|---|---|
| **3** | **Execution chaining** — you cannot get from one program to another | 🟡 **Half done (s80).** Doom now starts and plays when typed at the shell, and quitting no longer faults the host. ⛔ **Quitting still ends the VDM instead of returning to the shell.** |
| **1** | **Graphics** — Wolf3D, Mario and Doom low-res correct | One root cause. **Measure the SR2 write rate first** — the user decides the speed/correctness trade on that data. |
| **2** | **Sound** — Gravis Ultrasound, so Heaven7 plays music | In the contract already; sound has **never been inventoried**. **Cleared to use the archived GUS SDK.** |

✅ **The two questions — ANSWERED by the user 2026-09-25 (s80):**
1. **Graphics performance bar: "measure first, then decide."** Bring back the SR2 write-rate
   numbers for Wolf3D / Mario / Doom low detail; the user makes the call on them.
2. **GUS: yes** — work from the publicly archived Gravis GUS SDK and write our own
   `docs/ref/gus.md` citing it (as with the 82077AA and 16550). Do not mirror the SDK.

### ▶ 3. Execution chaining — the crash is fixed; returning to the parent is next

The user's report: *"Inside DOOM Setup, save settings and run Doom, crashes. Same for
Heretic, Hexen, Duke3D setups"* and *"double-click, command.com, navigate to
demo\msdos\doom and run DOOM — crashes."*

**s79's reading was wrong about the cause.** Doom never chose to exit: DOS/4GW aborted with
*"error (2002): transfer stack overflow on interrupt 09h"*. The key that launches the
program (Enter's break code) reaches DOS/4GW's pass-up `INT 09h` handler before Doom hooks
its own; that handler chains to our PM default stub, which had **no dispatcher arm for
09h**, so every such key abandoned the ISR and leaked a transfer-stack frame. The abort's
`AH=4Ch` then ran *inside* the ISR, the injector resumed the dead client, and the host
faulted. **Fixed in s80** (host `2f803674`, on the rig, ⚠ not yet confirmed by hand):
the default handler reflects IRQs 09h–0Fh to the BIOS, and `g_pm_client_exited` stops a
dead client being resumed. Verified: `chain.bat run`, Doom direct, Skyroads, Win16 Notepad.

▶ **Next: the PM `AH=4Ch` must return to the EXEC parent.** It still ends the whole VDM.
Needs a real DPMI client **teardown** (the switch-in is one-shot global state), then the
real-mode `AH=4Ch` for the current PSP, then a **second** client must work (Doom twice).
Acceptance test: `debug\rig\chain.bat quit` — `ver` must print after Doom exits.
⚠ `chain.bat` is not in `bmstage.sh`'s list; copy it by hand with CRLF.
⚠ Ask the user to try **SETUP → save and launch** by hand on `2f803674` — the same Enter-key
path, but not yet run.

### ▶ 1. Graphics — one root cause, and the parked verdict may be about the wrong design

Wolf3D, Mario and Doom low detail are all **unchained (mode-Y) planar** rendering — one
cause, not three. The spec side is **done**: `ref/vga.md` is 9/9 against a real AMI BIOS
and the IBM VGA ROM, and the register file is at 99.9%. What is missing is *observation*,
and `vdd_video.c` says so itself:

> *"The A0000 aperture is one flat buffer — the page trap is deliberately not armed,
> because arming it makes the interpreter the CPU and collapses the run — so a guest
> write lands there with no record of which plane the map mask had selected."*

⇒ Everything downstream (`modey_copy`, `MODEY_GAP_DEFAULT`) is *guessing which planes a
write went to*, and a guess cannot be made correct.

★ **Do this first, before any design:** the parked objection is about trapping **every
write**. Planar code sets the Map Mask (SR2) and *then* writes a run — so trapping on
**map-mask change** may cost one fault per run instead of one per byte. **Measure the
actual SR2 write rate in Wolf3D, Mario and Doom low detail** before choosing. That
measurement is cheap and nobody has taken it. It may make the trade-off question moot.

### ▶ 2. Sound — the surface with specs named and no documents at all

`ref/SOURCES.md` already names *Gravis Ultrasound — Gravis GUS SDK / Programmer's Guide —
GF1 voices, DRAM, the DMA/IRQ contract*, so this is inside the period-correct hardware
contract and is **not** app-driven. But `inventory/README.md` shows the whole sound row as
`— | —`: **Sound Blaster, OPL and GUS all have a named authority and neither a `ref/` nor
an `inventory/` doc.** SB and OPL were written app-first years ago and never inventoried.
So this north star starts with the two-docs-per-surface work the programme requires.
GUS is a large surface: 32 GF1 voices, on-card DRAM the host DMAs samples into, its own
IRQ/DMA contract, registers at `2X0h`. Heaven7 needs it for music.

---

## Next actions, in order

▶ **★★★★★ YOU CAN NOW JUST OPEN NTVDMEX AND GET A DOS PROMPT (2026-09-25).** Run
`ntvdmhost.exe` with no arguments — double-click, shortcut, Start menu — and a DOS
session comes up with XP's own `COMMAND.COM`, ready to type at. **No `cfg\` files, no
arguments, no knobs.** It refuses with an explanation if NTVDMEX is not installed,
because without the IFEO key the session would silently be *stock* ntvdm's.
  - A bare launch used to reach STAGE1, be refused VDM privilege
    (`NtVdmControl` → `0xC0000022`) and vanish with no window and no message. VDM
    privilege is not askable: NT grants it to a process CSRSS made for a 16-bit image.
    So the launcher writes a four-byte DOS stub and runs *that*; the IFEO key hands the
    VDM back to us with the privilege.
  - ⛔ **The stub's name must be 8.3.** `ntvdmex-shell.com` came back as `NTVDME~1.COM`
    and the whole thing fell through silently.
  - The two things XP's shell needs (DOS 5.00, the private `AH=53h` answers) are no
    longer `cfg\` knobs: they key off a **measured property of the image** — an
    NTVDM-aware guest carries `C4 C4 54` BOPs and XP's shell has fifteen — and only for
    a program loaded *as the shell*. 6.22's `COMMAND.COM` has none and is untouched.

▶ **⛔ THE VISIBLE-QUALITY BLOCKER IS ONE ROOT CAUSE, NOT THREE.** The user's 2026-09-25
by-hand pass: *"Doom low res, Wolf3D and Mario are all still graphically broken (not
entirely, just not correct)"* — everything else fine. All three are **unchained /
mode-Y** rendering, and `vdd_video.c` says why in its own words: *"The A0000 aperture is
one flat buffer — the page trap is deliberately not armed, because arming it makes the
interpreter the CPU and collapses the run — so a guest write lands there with no record
of which plane the map mask had selected."*
  ⇒ It is **parked on a performance judgement, not an impossibility**, and that judgement
  is now the thing standing between NTVDMEX and looking right. **Unparking it is a
  product decision** — it means a write hook on A0000 and paying for it. That call is
  the user's, and it is the next big question.

▶ **★★★★★ XP's OWN `COMMAND.COM` IS AN INTERACTIVE SHELL (2026-09-25).** A **stock XP
box** now gets a working DOS prompt from the shell it already has — prompt, `ver` →
`MS-DOS Version 5.00.500`, `dir` with volume serial and free space, cursor waiting at
the next prompt. That closes the product question below: no Microsoft 6.22 shell is
needed to ship. **991 prompts per 30-second run became 3.**
  - **The last bug was ours, one field wide.** Our `BOP 0x54 sub 01` reply wrote `[0]`
    of the command-tail buffer — which is **DOS's `AH=0Ah` maximum**, set once at
    transient `0x018D` to `0x80` and never re-set, because COMMAND.COM hands the *same
    buffer* to the keyboard read. We zeroed it, our `AH=0Ah` returned an empty line
    without waiting, and the shell printed its prompt again for ever. The log had said
    `INT21 AH=0A line max=00` ×991 the whole time. Fixed: write the length at `[1]`,
    never touch `[0]`.
  - **⚠ The shell was AT the keyboard read and we were answering it with EOF.** Several
    turns read *"prints a prompt, goes back to asking"* as a gate we had not satisfied.
    Every gate was satisfied.
  - **⚠ `int16=[0,0,0,0]` was never evidence of anything.** Our `AH=0Ah` reads the host
    key ring directly and never issues `INT 16h`, so that counter reads zero on a shell
    that works. It reads zero in the successful run too.
  - **⛔ NOT ON BY DEFAULT.** Two `INT 21h AH=53h` answers have to change with it and
    they **contradict the only stock measurement we have**, so they live in
    `cfg\int53.txt` and the built-ins are unchanged. See the next item.

▶ **⛔ THE OPEN QUESTION: `AH=53h` IS CONTEXT-DEPENDENT AND OUR HARNESS IS THE CONTEXT.**
XP's COMMAND.COM cannot read a key while `[0x327]=1`, proved from its own image
(`tools/ntvdm/cmdcom.py`): the read-a-line routine is transient `0x0A0D`, its only two
`AL=0` callers both sit behind `cmp byte [0x327],1 / jz`, and `[0x327]`'s single writer
is `mov al,5 / mov ah,53h / int 21h / mov [0x327],al`. **Stock is interactive, so stock
answers `AL=0`; our probe measured `AL=1`.** The likely difference: `probe.inc` reports
through `INT 21h AH=02` and every stock run is captured with `> FILE`, so we asked the
oracle *"is this console interactive?"* **with its own output redirected**.
**A probe that reports through stdout cannot measure anything that depends on stdout.**
▶ **NEXT, and it needs a human at the box:** run `tools/dostest/p_int53f.com` (writes its
dump with `AH=3Ch/40h/3Eh`, needs no redirection) against **stock ntvdm**, both with and
without `> FILE`, and diff. The IFEO bracket is the documented rig-bricking hazard and is
**not run unattended**. If it confirms `AL=5 → 0` and `AL=2 → CF=1`, promote them to the
built-in defaults in `dos_int21.c` and delete the knob's reason for existing.

▶ **✅✅✅ THERE IS A WORKING DOS PROMPT (2026-09-24).** **MS-DOS 6.22's `COMMAND.COM`
runs under NTVDMEX**: banner, prompt, `ver`, and a real `dir` with volume serial and
free space — 141 INT 21h calls, keystrokes scripted through `cfg\keys.txt`. And a launch
that names **no** program now loads a shell, from `cfg\shell.txt` first and
`C:\WINDOWS\SYSTEM32\COMMAND.COM` second, in the last-resort branch strictly below
CSRSS/`target.txt`/title — verified not to disturb the harness. ⛔ I scored this same run
as *"zero INT 21h calls"* earlier the same day: the check grepped `INT21`, the trace
prints `  21:`. **A pattern that cannot match is not a measurement.**
▶ ~~**The product question, not a code question:** 6.22's shell is Microsoft's and cannot
ship in a public repo, so a stock XP box still falls to XP's own `COMMAND.COM` and still
stops at the BOP below. Either the BOP work happens or the shell is user-supplied.~~
**✅ ANSWERED 2026-09-25 — the BOP work happened.** XP's own shell works (top of this
section). It still needs `cfg\int53.txt` until the un-redirected stock measurement lands.

▶ **★ XP's COMMAND.COM IS NOT A DOS PROBLEM (2026-09-24).** XP's `COMMAND.COM` does not
fail a DOS call and give up — **it never reaches one.** It is NTVDM-*aware*: its image
issues `C4 C4 54` fifteen times (plus one `C4 C4 50`), and the exec loop's last arm
hands **any BOP no arm matched** to `dos_int21()`, where the guest's `AH` picks a DOS
function. `BOP 0x54 / sub 01` arrives with `AX=0x0002`, reads as `AH=00` = *terminate*,
and we report a **clean exit, code 0**. Our `0x50` and `0x54` are DPMI's; the number
space is NTVDM's. Full surface: **[`inventory/bop.md`](inventory/bop.md)**; the
investigation, including three readings it retires: [`research/xp-command-com.md`](research/xp-command-com.md).
The instrument that settled it was one 32-bit load — **the caller is on the guest stack
at `SS:SP`, not in `VTIB_CS:EIP`, which is where the handler is**. Order of work:
(1) gate the fall-through once the battery says what reaches it, (2) un-double-book
`0x57`, (3) measure what `BOP 0x54`'s sub-functions mean against stock ntvdm,
(4) auto-report DOS **5.00** for `command.com` — the guest does `cmp ax,5` on the whole
word, so 5.00 exactly — then the no-guest default, then PIF.

▶ **Rig status, 2026-09-25 end of session 79 — and both builds are USER-CONFIRMED.**
`bin\ntvdmhost.exe` = **`1ea8829d`** (the launcher). `debug\prev\ntvdmhost_prev.exe` was
promoted to **`d57d586c`**, which is the one the user ran the **whole shelf** against:
*"I tested all the usual suspects and they were fine."* `1ea8829d` adds only the launcher
on top of it and carries a **one-item** confirmation: *"double-click bin\ntvdmhost.exe —
tested and working!"* Rolling back to `prev` therefore costs exactly the launcher and
nothing else.
  - ⚠ **The checkpoint commit has still not moved** — it names `ff0d956`/`71ef4737`.
    Moving it is a deliberate act; it is now well behind two confirmed builds.
  - `cfg\` is **clean** — no `int53.txt`, `dosver.txt`, `keys.txt`, `qimode.txt`,
    `capture.flag` or `dostrace.flag` — so the box behaves as a fresh one, and a
    double-click gives a prompt with no files at all. `scripts/bm/int53-interactive.txt`
    is parked at `debug\rig\` where it cannot fire by accident (it is no longer needed:
    the NTVDM-aware detection replaced it).
  - ⚠ `demo\msdos\doom\COMMAND.COM` was copied in for the chaining repro and **removed
    again** — the folder is back as the user left it.
▶ **What the user has confirmed by hand (2026-09-25):** the whole shelf on `d57d586c`;
the double-click launcher on `1ea8829d`. **Still broken, and now north star 1:** Doom low
res, Wolf3D, Mario.
▶ **Run headless on `1ea8829d`:** 6.22's `COMMAND.COM`, XP's `COMMAND.COM` from a bare
double-click (prompt / `ver` / `dir` / EXEC), Doom, Skyroads, heaven7, duke3d, ZAR, and
the Win16 kernel probe. Off-VM 1591/1591.

▶ **Rig status (older):** the watcher is live and everything below has been run through it.
`p_vgaext` came back `1010`, byte for byte with PCem. VGA register parity is **99.9%**
(`689/690` of the bytes the two oracles agree on) — `tools/vgaparity.py`, **re-run, never
quote**, and not comparable to the void 89.7%. Doom and Skyroads confirmed by hand after
the Input Status 0 change. ▶ **Owed from a human: a by-hand look after the mode-set fix**,
which moves the text cursor by one scan line (`0x0607` → the card's own `0x0D0E`),
**and after the floppy controller** (`ea485e7`, host `bbea3134`) — a device that did not
exist before, so the risk to existing guests is low and Doom + Skyroads measure it low,
but it is an observable change and the rule is one at a time with a human in between.

1. **The next surface from [`ref/SOURCES.md`](ref/SOURCES.md).** Done so far: the **8254**
   (5 fixes, clean against oracle consensus), the **VGA register file** (99.9%), and the
   **8259A** (2 fixes) and the **8042 keyboard controller** — `ref/kbc.md`, inventory
   marked from the code, and the status register, the command set, the output port,
   port `92h` and **a single converged A20 bit** all implemented.
   **8237A DMA** (spare page latches), and the **MC146818 RTC/CMOS** — which did not
   exist at all: ports `70h`/`71h` were claimed by nothing, so Status A's UIP bit read
   as set for ever and **the canonical "poll UIP, then read the time" loop never
   exited**. New `vdd_cmos.{c,h}`.
   The RTC's **periodic IRQ8** followed — on the PIT's own pacer, dormant unless a guest
   programs it, timing canary re-run (`n8=0 max_ms=7`).
   The **16550 UART** followed and is the first surface that needed **no fix**: four of
   five probe cases agree on all four hosts, including the loopback modem-line pairing
   and the DLAB bank switch. ⚠ *My prediction that the UART and the FDC would both show
   the "firmware present, chip absent" split was half right* — the FDC is absent, the
   UART was there and in good shape.
   The **82077AA floppy controller** followed, and it was the **third**
   "firmware present, chip absent" hole in a row — and the worst-shaped. Nothing
   claimed `3F0h`–`3F7h`, so `3F4h` (the Main Status Register) read `FFh`, which
   is `RQM=1, DIO=1`: *"ready, and I am the one talking"*. The command-write loop
   out of the datasheet — the loop in every BIOS and every driver —
   `and al,0C0h / cmp al,80h / jne` — **never matched and never exited.** Not a
   plausible wrong value: a machine that stops, with nothing in any log.
   ⛔ **`00h` would hang too** (RQM clear is also *wait*), so no bus default could
   have saved it — only the chip. New `vdd_fdc.{c,h}`: the register file, the
   three-phase command protocol, and every command that does not move sector data.
   **MEASURED on the rig, `fdc.cmdwait`: `01C0` → `0080`**, where `AH` is *"the
   loop never terminated"*. Two oracles agree on every load-bearing row
   (`fdc.version` = `0190`, `fdc.dumpreg` = ten result bytes).
   ⛔⛔ **And one row was a manufactured agreement.** `fdc.dor.low` masked DOR to
   bits 3:0 — and `FFh & 0Fh` is `0Fh`, a plausible DOR — so a port that did not
   exist scored a **MATCH** against PCem. *A mask can manufacture an agreement out
   of an absent device; check what `FFh` becomes after your mask.*
   ▶ **Next candidates in the contract:** the FDC's **data path** (READ/WRITE over
   DMA channel 2, against the image handle INT 13h already holds — marked **PART**
   with an off-VM check asserting the gap so closing it cannot be silent), the
   **IDE/ATA** surface (`3F6h` answers `FFh` where both oracles say `50h` — found
   by the FDC probe, filed against ATA rather than "fixed" by claiming a register
   that is somebody else's), the UART's OUT2 interrupt gate.
   Pick by the contract, not by a guest.
   ⚠ **Owed:** a `p_rtc` case that sets PIE and counts periodic interrupts. The existing
   `statusc.clear` row reads `0000h` because the probe never enables the interrupt, not
   because nothing can raise one.
   ⛔ **Recorded and deliberately unfixed:** the PIC's ICW1 read-select reset and the
   8042 output port's undefined bits are both **blocked on a second oracle** (only PCem
   can see either); we implement Special Fully Nested Mode unconditionally while never
   reading the ICW4 bit that requests it; and keyboard-side commands are still never
   ACKed with `FAh`.
2. ✅ ~~**Populate the VGA mode tables.**~~ **DONE** — and it was **one** defect, not five:
   the table was right, but six registers are read back from a *live shadow* rather than
   from the register file, and the mode set never seeded them. **92.0% → 99.9%.**
   ▶ **One byte left**, recorded rather than bundled: `modeX` `CR0F`, where `crtc_in`
   *derives* the cursor address instead of storing it. Fixing it properly means the BIOS
   cursor calls must write `CR0E`/`CR0F`, which touches every path that moves the cursor.
3. ✅ ~~**Deterministic Win16 tests**~~ — **the harness exists** (`ea485e7`.. `tools/ne/mkne.py`
   + `tools/wintest/`). The blocker was never the tests: **nothing here could BUILD a Win16
   binary** — no OpenWatcom on the machine, none in Homebrew, none in the tree. nasm now
   writes the 16-bit code and `mkne.py` writes the NE around it, every field read off
   `TASKMAN.EXE` with our own `nedump.py`/`nedis.py`. First run on the rig:
   `InitTask`/`WaitEvent`/`InitApp` all succeed, `kernel.getversion` = `5F03`,
   `kernel.getwinflags` = `4C25`.
   ✅ **And compared against stock `ntvdm`** (`tools/wintest/stock.sh`, which drops the
   IFEO key and restores it, refusing to exit quietly unless it proves the key came back):
   `kernel.getversion` **AGREES** — the first Win16 row this project can call *verified* —
   and `kernel.getwinflags` **MISMATCHES**, `4C25` against stock's `4C29`, reproduced
   twice. ⚠ Neither value is ours: both runs load the same `KRNL386.EXE`, so the
   difference is what our VDM presents to it. ⚠ The `WF_CPU386`-vs-`WF_CPU486` reading of
   those bits is an **interpretation from memory, not confirmed**.
   ✅ **CLOSED 2026-09-25, and it was a DPMI bug wearing a Win16 costume.** `GetWinFlags`
   is KERNEL.132 = seg 3:`0x4B` and it is just `mov ax,[0x464]`; `[0x464]` is built at
   seg 1:`0xD68A` out of **`INT 2Fh AX=1687h`'s `CL`** (`cmp cl,3 / je bl=4 / else bl=8`).
   **We hardcoded `CL=3`** at two sites. `DPMI_CPU_CLASS = 4` and the row now reads
   **`4C29`**, the value stock was measured to produce. ⛔ The `AC`-flag hypothesis is
   refuted — nothing on that path tests a flag. ⚠ Not a same-day side-by-side; re-running
   `stock.sh` needs a human (it drops the IFEO key). A/B on the rig in `runs/s79_cl_ab/`.
   See [`inventory/win16.md`](inventory/win16.md).
4. **VGA step 4 — one address generator** from CR17/CR14/GR5/GR6/SR4. ⛔ **PARKED**: the
   A0000 aperture is mapped RAM with no write hook, so mode-Y exactness needs an
   architecture change, not a patch. Instrument in place (`tools/doomdetail.py`).
5. **Then outward**, per [`inventory/README.md`](inventory/README.md).

⚠ **One observable change at a time, with a by-hand check between.** Read-back fidelity is a
guest-visible behaviour change. **The regression set must include Doom at low detail** — every
Doom check this project has ever run was high detail.

---

## How anything here is verified

| Instrument | What it is good for | Command |
|---|---|---|
| **The off-VM battery** | ~30 native unit tests over the DOS kernel and the device models. No VM, no rig. **Run it after touching any shared header.** | `./scripts/offvm.sh` |
| **The parity sweep** | Every DOS probe asked of NTVDMEX and of genuine MS-DOS 6.22 (and PCem where a probe names it), diffed. **46 probes, 748 rows, 0 unusable.** ⛔ **Its score excluded TEN device probes for three sessions** — a trailing comment on a probe's `ORACLE-ALSO:` line built a nonsense command line, the probe reported `NO ROWS`, and an unusable probe leaves *both halves* of the fraction, so the number could only **rise** when something broke (`e618011`, `e89478e`) | `./scripts/paritysweep.sh` |
| **The bare-metal rig** | A real XP box. The only thing that can see a wrong picture. | [`wiki/The-bare-metal-rig`](wiki/The-bare-metal-rig.md) |
| **PCem + a genuine IBM VGA / Tseng ET4000 ROM** | The VGA/VESA/BIOS oracle — the only period-correct firmware we have | ✅ **runs unattended**, ~60 s: `scripts/pcemoracle.py run <x.com>` ⚠ **outside the command sandbox** |
| **Stock `ntvdm` on the rig** | The oracle for everything with no public spec — the WOW32 thunk ABI above all | [`research/stock-vdm-dump-oracle`](research/) |
| **The score** | A model, not a measurement | `./tools/score/score.py` |

⛔ **Re-run these, never quote them.** Any number written down here is stale by definition.

---

## Standing hazards — each of these has cost a session

1. ⛔ **Check the guest's own config before blaming code.** `findstr /I "detaillevel screenblocks" default.cfg`. Doom's detail level has burned three sessions. A game config survives binary swaps, reverts, reboots **and a full wipe**.
2. ⛔ **Find every install before believing a run.** `dir /s /b C:\ntvdmhost.exe`. The rig has had two complete NTVDMEX installations with different game configs; one party launched one and the other described the other, and both were right.
3. ⛔ **A first run after a deploy is not evidence.**
4. ⛔ **A by-hand verdict is only as good as its baseline**, and an A/B between runs you did not control is not an A/B.
5. ⛔ **A guard that returns success is a lie the whole stack repeats.** Verify *state*, not exit codes.
6. ⛔ **An absence in a report means nothing** — a guard's own log line must never share a budget with what it guards against.
7. ⛔ **A regression need not be code.** A folder rename broke Hexen; a flag file containing a log line killed every PM timer.
8. ⛔ **NTVDMEX can jam the whole machine.** The rig is one folder, nothing on `C:`. A dead rig means checking the IFEO key points at an existing binary.
9. ⛔ **Launch a Win16 guest after any shared-path change** — it was dead for five sessions and nobody noticed.
10. ⛔ **Copy the user's log to `runs/` before running anything** — the host truncates `out\ntvdmhost.log`.
11. ⛔⛔ **A KNOB WITH TWO SOURCES: DELETING THE FILE DOES NOT RESTORE THE DEFAULT.** The reported DOS version comes from `cfg\dosver.txt` *or*, persistently, from `HKCU\Software\NTVDMEX\DosVersionMajor/Minor` — written by the Settings dialog and surviving every reboot, wipe and rebuild. The rig was found on 2026-09-24 reporting **5.00 to every DOS guest** from the registry, with no `dosver.txt` anywhere, on a project whose whole parity method diffs against a **6.22** oracle (`p_ver.com` → `int21.30 AX=0005`). Nothing reported it: the host logged a line only when the *file* overrode. Now unconditional and it names the source — `STAGE2: DOS version reported = 05.00 (source: …)`. ⚠ **Before quoting any DOS-side comparison, read that line**, and ask the same question of every other setting the dialog persists.

The full list, with the story behind each, is the wiki's
[Traps and lessons](https://github.com/MrMatthewLayton/ntvdmex/wiki/Traps-and-lessons).

---

## Getting started on a machine that has never seen this

```bash
# Build (macOS/Linux cross-compile to XP-32; needs mingw-w64 i686)
./scripts/build.sh

# Fast test loop -- no VM and no rig needed.
./scripts/offvm.sh
```

Then read [`README.md`](README.md) for the map of the rest of the documentation, and the
wiki's [Building and running](https://github.com/MrMatthewLayton/ntvdmex/wiki/Building-and-running).

---

## The one thing to internalise

**A thing that looks done has been wrong more often than a thing that looks broken.** A
stepped-over call still answers — with stack litter. A half-modelled register returns a
*plausible* wrong number and the guest fails somewhere else entirely. An unimplemented call
cannot return a default. That is why the inventory marks **PART** separately from **MISS**, why
probes poison every output register before asking, and why "user-confirmed by hand" is the only
status that closes a guest.
