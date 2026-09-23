# Session 55 — Win16 dialogs -- the unlock of the day (USER thunk 0xEF)

> Session 55. Split out of `docs/STATE.md` on 2026-09-23.

> Verbatim from `docs/STATE.md`. Nothing has been edited or corrected,
> including conclusions a later session refuted — same rule as the rest of
> this archive. See [`README.md`](README.md).

## ★ SESSION 55 — 80.2% → 83.1%  (paused mid-afternoon at the user's request)

**Seven commits, all rig-gated. The user's goal for the day was >90%; it was
not reached and the reason is measured rather than guessed — see the rate
note at the end.**

### ▶ ⏸ WHERE WE STOPPED, AND THE QUESTION ON THE TABLE

The session was paused so the user could restart the IDE. They will resume
with *"Continue where we left off"* and asked to be **presented with the four
options below again** before any more work is done.

| option | what it means | rough value |
|---|---|---|
| **Chase the number** | the bounded not-started items: VDD/driver SDK (#11), VESA 4F0A PM bank switching (#53), serial VDD (#9), net VDD (#8), plus the last ~25 Win16 services | **+3.9**, predictable, → ~87%. Mostly checkbox items rather than visible capability |
| **Make the guests work** | find which pass re-patches CALC's FP site (unblocks CALC + TASKMAN + CARDFILE), give PROGMAN its groups, get SYSEDIT saving | fewer points (**+0.26** per partial→done) but it is the north-star work |
| **Mixed** | guests for ~2h while the user is around to confirm, then harvest the bounded items | — |
| **DOS defects** | WRITE's "not enough disk space" on a 243 GB disk; `MEM /C` contradicting its own summary | small, but this is the *runs but lies* class |

### ▶ ★★★★★ WIN16 DIALOGS — THE UNLOCK OF THE DAY (USER thunk `0xEF`)

`USER.EXE` implements `CreateDialog`/`DialogBox`/`DialogBoxParam` in its OWN
16-bit code, so `neneeds.py` calls them `native16` and prices them at
nothing. **They are not free**: that code does the
FindResource/LoadResource/LockResource itself and then calls out to a 32-bit
helper at **USER id `0xEF`, which no export maps to**. Stepped over, CALC
produced no window at all.

**The arguments are MEASURED**, from one CALC run — 22 bytes, and the WOWBOP
line above the call settles the important one (`ax=cx=dx=0x0b47`, the
selector USER had just had back from `LockResource`):

```
+20 hInstance   +16 template FAR*   +14 parent   +10 dlgproc   +6 initparam
+2  TEMPLATE LENGTH IN BYTES        +0  still unexplained (0 in every run)
```

★ **`+2` is proved on two different values**: CALC passes `0x0140` and its
`SC` dialog is 320 bytes; SOUND RECORDER passes `0x0210` and its `DIALOG 1`
is 528. ⚠ It was nearly read as a window handle because CALC's `0x0140` also
happened to be the next handle our own allocator would issue — **a number
matching something is not a number meaning it.**

★ The template reading was confirmed **offline before any host code existed**
(`tools/ne/neres.py dialog`, new this session), which is the discipline
session 43 held the menu decoder to: a wrong offset does not spell
*'Calculator'*, *'SciCalc'*, and buttons reading Hex/Dec/Oct/Bin/Hyp/Inv.

⚠ **One thing is a JUDGEMENT and is marked as one in the code**: we show a
dialog whose template omits `WS_VISIBLE`, because that is what the dialog
manager does and we are its 32-bit half. Which of DialogBox/CreateDialog this
id serves is **not known**. ▶ To settle it, disassemble `USER.EXE` at
`0x03ff:0x4bdc`.

### ▶ ★★★ FIVE GUESTS MOVED OFF `todo` — the shelf sweep

`scripts/bm/shelfsweep.bat` launches each never-run guest in turn. **Four of
seven produced windows on the first sweep**, which is the shelf method
working exactly as recorded: *the run names the blocker.*

| | |
|---|---|
| **CALC** | real `Calculator`, own icon, Edit/View/Help, taskbar button; **15/15 `SciCalc` controls at the template's rectangles**, `rigshot tree` showing 13 hidden + 2 visible = EXACTLY the 13 `SW_HIDE` and 2 `SW_SHOW` CALC itself made. Runs its whole `WM_INITDIALOG`. |
| **TASKMAN** | real `Task List` via the `#32770` fallback class, 8/8 controls |
| **PROGMAN** | ★ **the Windows 3.1 SHELL** — 1260×742 `Program Manager`, own icon, File/Options/Window/Help, taskbar button. MDI client empty grey. |
| **TERMINAL** | 1260×742, menu, scrollbars, and **a status bar it is drawing into — `Level: 1` and a live clock**. Third guest on the `0xEF` helper (`Default Serial Port` dialog with real static/listbox/OK). |
| **SOUNDREC** | real `Sound Recorder` with its microphone icon, File/Edit/Effects/Help, panels, wave display, scrollbar, five transport buttons |
| **PACKAGER** | real `Object Packager` with Appearance/Content panes and a working Description/Picture radio group |

★★ **PACKAGER WAS UNBLOCKED BY SOMETHING THAT WAS NOT ITS RECORDED BLOCKER.**
Its note said `EnumTaskWindows`, still unimplemented. The real cause was that
registering `BUTTON`/`STATIC`/`SCROLLBAR`/`COMBOBOX`/`#32770` as system
classes for the dialog work also fixed its `CreateWindow "STATIC"`. ⇒ the run
is the oracle, again.

### ▶ ★★★★★ INT 34h..3Fh IS FLOATING-POINT CODE, NOT AN INTERRUPT RANGE

**The patcher's own comment was wrong and it cost three guests.** It rewrites
`CD nn` to a BOP for any voted vector because "servicing is harmless for any
vector: our default PM handlers cover all 256 and simply IRET". True of
interrupts. **These are not interrupts**: Microsoft's FP emulator encodes
x87 opcodes as `CD nn` in that range (0x34..0x3B = ESC D8..DF, 0x3C =
segment override + ESC, **0x3D = FWAIT**), and 0x3E/0x3F are the far-call and
overlay fixups. Rewriting one does not make it serviceable, it makes it stop
being the instruction.

CALC — **a calculator, which imports WIN87EM** — built its whole dialog, ran
its whole init, then died at
`bytes@eip-2 = 3d cb c4 c4 07 cd 3d cb cd 39` = FWAIT, RETF, our BOP, more FP.

★ **The instrument that named it is three lines and is now permanent**: the
PM-stop line asks `pmap` whether the byte pair is one of OUR patches, because
the log had always left the reader to guess between a real BOP, a patched INT
we failed to resolve, and data being executed. First run with it:
`pmap[eip]=INT 0x39 ★ THIS IS A SITE WE PATCHED`.

⚠ **DOOM WAS DOING IT TOO** — the guard fires 4× in Doom's own code.
Regression clean: `V_Init..ST_Init`, 3,503,139 bytes.

⚠⚠ **IT DOES NOT YET UNBLOCK CALC AND THAT IS THE HONEST STATE.** The guard
stops us corrupting NEW sites. CALC's site was patched by an EARLIER pass and
`if (pmap_get(lin)) continue` skips it before the guard is reached — the
region containing it reports "patched 0 INT sites" precisely because the bytes
were already `C4 C4`. **Which pass writes it is not yet known, and that is the
next step — not another repair.**

⚠⚠⚠ **AND THE OBVIOUS FIX WAS MEASURED WRONG.** Healing at the point of
failure (restore `CD nn`, leave EIP, re-execute) ran **120,670 times on one
address**: something re-patches it between heals, so "self-limiting because
the map entry is cleared" was simply false. Reverted, with a scanner repair
pass that never fired once. An infinite loop is worse than a clean stop.

### ▶ #28 CLOSED — the DOS version is configurable and THE GUEST SAYS SO

Everything was already in the tree; what was missing was a run where the
guest reports it. `scripts/bm/verver.bat`, three cases: `6.22` → `INT21.30
major=06 minor=16 oem=FF serial=00:0000`, `3.31` → `03/1F` on both calls, no
override → the registry default. Case 1 reproduces **every field** of the
recorded 6.22 oracle except `flags`, which `dos-oracle.md` already excludes by
name (DH bit 4 = DOS in HMA, a property of that image's CONFIG.SYS).
⚠ The first run of that gate reported the HOST side only — `findstr` matched
`#PROBE`, a string the probe does not print, and the wait loop polled for a
done-marker **the previous run had left on the share**. Delete both
destinations from the driving side BEFORE dispatching.

### ▶ THE RIG SCRIPTS THAT EARNED THEIR KEEP (all fixed, all in the repo)

* **`wowlive.bat` and `wowrun.bat` re-add the IFEO value before every launch.**
     GH #132 drops it after three unclean starts and every batch's `taskkill`
     manufactures them. It happened **twice today**.
* **`bmwow.sh` moves `wowsched.txt`/`wowcall.txt` aside itself** (`SWITCHES=1`
     keeps them) and **prints the gate signature next to the baseline**. That
     number had been hand-counted out of the log every session it was ever
     quoted. Without the move-aside the gate reads **280/291/74** and looks like
     catastrophic drift; with it, `85 / 113 / 57 · 0001:229C`.
* **`bmwow.sh` fails loudly on an empty log** rather than printing `0/0/0`,
     which means "no run happened", not "everything regressed".

### ▶ WHAT THE NUMBERS DID, AND THE RATE

`guests` 39% → **55%** (5 done, 12 partial, 2 todo of 19) ·
`breadth` 83% → **91%** (287 serviced / 27 to do) · `dos-version` 0 → **100%**.

⚠ **The day's rate was ~+1.05 points/hour**, and the last 50 minutes produced
a real finding and **zero points**. That is the shape of what is left: +6.9 to
reach 90%, over work that is harder than what has been done, not easier.

---
