# Session 54 — The clock: a stepped-over call, for the fifth time

> Session 54. Split out of `docs/STATE.md` on 2026-09-23.

> Verbatim from `docs/STATE.md`. Nothing has been edited or corrected,
> including conclusions a later session refuted — same rule as the rest of
> this archive. See [`README.md`](README.md).

## ★ SESSION 54 — 79.6% → 80.2%

**Product work, mostly. Everything below is committed and rig-gated.**

- **AN INSTALLER (#13, the M10 gap).** `ntvdmhost.exe /install | /uninstall |
     /status`, and the same three on the File menu behind a confirmation. It is
     **reversible, not merely removable**: a `Debugger` value belonging to another
     program is SAVED before we displace it and RESTORED on uninstall; one we never
     installed is REFUSED rather than deleted. Verified by READING THE VALUE BACK,
     never by a return code. 21 off-VM checks on the decision (quoted/unquoted, case,
     trailing space, forward slashes, a path that merely *starts* with ours); six
     behavioural cases on the rig.
- **CPU SPEED (#56)** — 18 speeds, Unlimited…8 MHz, on the Machine menu and the
     Settings CPU page. Duty cycle by suspending the exec thread.
- **THE WHOLE DISPLAY PAGE IS ALSO THE VIEW MENU**, and menu changes are
     **session-only** (`g_set` = in force, `g_set_disk` = saved). Window size resizes
     live; aspect is a **lock** (None/4:3/16:9/16:10) with a 640×480 floor; scales
     that cannot fit the desktop are **greyed**, not silently substituted.
- **The status strip is three real parts**, name-hugging divider, bitness+mode
     joined ("16-bit Real mode").
- **The DOS text cursor is an underscore again** — see the cursor note below.

### ▶ ⚠⚠ THE THREE THINGS THAT COST ME TIME TODAY — READ BEFORE RUNNING ANYTHING

1. **THE RIG SILENTLY STOPPED ROUTING TO US, TWICE.** GH #132's recovery drops the
      IFEO `Debugger` value after three consecutive unclean starts — and every rig
      batch begins with `taskkill`, which manufactures them. A guest then comes up
      under **stock ntvdm** and everything looks normal. I showed the user a Clock
      window that was stock's. ⇒ **`reg add` the key before EVERY case in a batch**,
      make the batch print a loud line when the host log is missing, and use
      **`ntvdmhost.exe /status`** — it diagnoses this in one command now.
2. **A `copy /y` STRAIGHT AFTER `taskkill` FAILS SILENTLY.** Windows has not
      released the image handle yet; with output sent to `nul` the OLD binary stays
      and the run looks entirely normal. I gate-tested the previous build for a whole
      run. `wowlive.bat` already had this written down — **reuse the scripts.**
3. **ONE SCREENSHOT IS NOT A MEASUREMENT of anything that repaints.** I reported
      Clock fixed from a single frame; it was one phase of a loop that is still
      running. Two shots minimum, and diff them.

### ▶ ★★★★★ THE CLOCK: A STEPPED-OVER CALL, FOR THE FIFTH TIME

`CLOCK.EXE` asks DOS for the date the ordinary way — **`INT 21h AH=2Ah`** in
protected mode. Inside a WOW VDM there is no DOS underneath, so **krnl386 owns that
vector and thunks it out as seg1 `FUNC 0x86`**. Unimplemented ⇒ stepped over ⇒
answered 0 ⇒ no date ⇒ blank face ⇒ invalidate ⇒ ask again. **10,317 times in
fourteen seconds**, which is why its log came back at **234 MB**.

Implemented (FAT packing: date high word, time low). **Clock now renders the correct
time** — `20:55:04`, measured.

⚠⚠ **BUT IT IS NOT FIXED, AND I SAID IT WAS.** The repaint loop is STILL THERE:
8,922 date calls served in ~20 s (≈450/s, where a clock needs 1/s), and consecutive
frames differ by 28.5% of the client — the face is caught mid-erase. Answering
`0x86` changed **what** it paints, not **how often**. `guests` stays 5 done + 5
partial.

⚠ **It also corrected a recorded inference.** The old note said Clock's time "comes
from krnl386's own 16-bit code, so there is no thunk to observe". The two *measured*
hypotheses beside it were right; that one was a guess wearing the same clothes.

**NEXT ON CLOCK, in order:** (a) find what drives the invalidate loop — the date
service no longer returns a sentinel, so whatever remains is a second cause;
(b) `20:55:04 PM` is 24-hour digits with a 12-hour suffix — `[intl]` `iTime` /
`s1159` / `s2359` out of WIN.INI, a formatting bug, not the date service.

### ▶ THE OTHER TWO PARTIAL GUESTS (same sweep, same day)

- **RECORDER** — titled window, **blank client, no menu bar**.
- **MPLAYER** — **no window at all**. Its recorded blocker
     (`SPI_GETICONTITLELOGFONT`) has since been implemented, so that note is stale.
- Both show the **same ~77 stepped-over startup calls** Clock did, so those are
     shared and survivable — **neither blocker is in that set.**

### ▶ THE CURSOR (user-reported, fixed)

`ABC123-` instead of `ABC123_`. DOS asks for its cursor in scan lines of an
**8-line** cell (underline 6-7, insert block 0-7); our cell is 16, so the underline
landed halfway up. Our own default did it too (`cur_shape` init `0x0607`). Fixed
with the VGA BIOS's own **cursor-emulation** rule (IBM/Bochs/SeaBIOS), reproduced
exactly: 6-7 → 14-15, 0-7 → 1-15. Overwrite underline **confirmed at a real
COMMAND.COM prompt**; the insert block is implemented and unit-tested but **not
witnessed** — plain COMMAND.COM may never set `0x0007` (DOSKEY and full-screen
editors do).

### ▶ #131 STDIO — LOCATED, NOT FIXED, AND THE OLD HYPOTHESIS IS DEAD

Five routes to the console handle are now eliminated. The fifth
(`VDM_COMMAND_INFO.StdIn/StdOut/StdErr`) returned non-handles — and **the
instrument that showed it named the real defect**: the WHOLE struct is junk
(`CreationFlags` 0x4d445674 and `CodePage` 0x78654e74 are ASCII and constant,
`TaskId` 0), so **`GetNextVDMCommand` returns TRUE and populates nothing**. The
command line says why: it arrives as `-f` with **no `-i<taskid>`**.
⇒ **#131 is the same defect as M2.5's "recover the real command line from CSRSS's
multi-call protocol". Fixing one fixes both.**
⚠ **NOT a subsystem problem** — we are already CUI, pinned in CMakeLists.
► Target proven reachable: `hello.com > out.txt` writes **136 bytes under stock and
0 under ours**, same box, same command.

### ▶ WHERE THE NEXT POINTS ARE (weight × remaining, measured)

| item | now | full close | note |
|---|---|---|---|
| `guests` (w15) | 39% | **+5.97** | **+0.26 per guest** partial→done — the heaviest lever by far |
| `breadth` (w12) | 83% | +1.33 | ~0.025 per service |
| `guest-zar` (w3) | 0% | +2.33 | VBE hi-colour + LFB |
| `flat-thunks` (w4) | 25% | +1.96 | architectural |
| `stdio` (w3) | 50% | +1.16 | blocked on the CSRSS handshake above |
| `dos-version` (w1) | 0% | +0.78 | the setting already exists and applies |

---
