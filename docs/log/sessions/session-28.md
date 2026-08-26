# Session 28 — 2026-08-26

> Archived verbatim from `return-ntvdm.md`, which was the single rolling handoff
> file until 2026-08-26. Split by session so it can be read by topic and date
> rather than by scrolling. **Nothing has been edited** — including conclusions
> that later sessions REFUTED, which are kept on purpose: the refutations are
> some of the most valuable material here.

```
═══════════════════════════════════════════════════════════════════════════════
██ ▶▶▶ SESSION 28 (2026-08-26). A REAL DOS SHELL RUNS; SETTINGS ARE IN THE     ██
██     REGISTRY WITH A DIALOG. Branch `m9/completeness`, PUSHED.               ██
═══════════════════════════════════════════════════════════════════════════════

Doom was finished (see the session-27 block below, still true and user-confirmed). This
session went BROAD instead: point the host at a real COMMAND.COM, which is the other
shape of DOS guest entirely — resident, line-at-a-time, walks directories, EXECs
children that must come back. No game exercises that. It found five defects in an
afternoon, four of them in code every guest uses.

▶ THIS SESSION'S COMMITS (all pushed)
```
  cd28879  mouse: DOOM WAS ASKING ALL ALONG -- ES:(E)DI, and the (E) was masked away
  13f3a60  doc: session 27 -- the mouse fault, and why both filed explanations were wrong
  7f5d9bb  doc: DOOM IS FULLY PLAYABLE -- the mouse is user-confirmed on the rig
  272279e  dos: run XP's own COMMAND.COM as a guest -- version is a knob, 53h refuted
  da55dfe  dos: MS-DOS 6.22's COMMAND.COM runs -- prompt, internals, DIR, and EXEC
  6fa84b9  ui: settings in the registry, a dialog, a visible cursor and a blinking one
```

┌──────────────────────────────────────────────────────────────────────────────┐
│ ▶▶ WHAT RUNS NOW: MS-DOS 6.22's OWN COMMAND.COM, ON REAL SILICON             │
└──────────────────────────────────────────────────────────────────────────────┘
```
    Microsoft(R) MS-DOS(R) Version 6.22
                 (C)Copyright Microsoft Corp 1981-1994.

    C:\dostest>dir
     Volume in drive C has no label
     Volume Serial Number is 0C09-23F0
     Directory of C:\dostest
    .            <DIR>         08-26-26   8:25a
    ..           <DIR>         08-26-26   8:25a
    ATTRIB   EXE        11,208 08-26-26   8:25a
    COMMAND  COM        54,645 08-26-26   7:55a
            6 file(s)         65,857 bytes
                         268,431,360 bytes free
    C:\dostest>attrib
      A          C:\dostest\COMMAND.COM     <- an EXTERNAL program: loaded, run,
      ...                                      and RETURNED FROM (AH=4Bh EXEC)
```
  WORKS: VER VOL CLS ECHO SET TYPE COPY DIR ATTRIB EXIT, line editing with backspace,
  correct "File not found", and a child process that comes back.

★ THE FIVE DEFECTS, none of them shell-specific:
  1. **AH=0Ah BLOCKED THE EXEC THREAD**, which deadlocks a shell outright. AH=01/07/08
     and INT 16h were fixed years ago to poll via `retry`; 0Ah was the last input call
     still parking the thread in C. What it waits for CANNOT ARRIVE while it waits: the
     BIOS key ring is filled by the guest's own INT 09h ISR, which cannot run because we
     are inside its INT 21h call. Measured: 40 scancodes queued, IRQ1 attempted 691
     times, EVERY one refused `not_in_exec`. Now collects the line ACROSS retries,
     keeping its position in the guest's own buffer.
  2. **A DOS FILENAME ENDS AT A TERMINATOR, NOT ONLY AT A NUL.** `fcb_put_name` copied
     the command line's 0x0D into the FCB, so AH=29h parsed `ver` to "VER\r    ".
     COMMAND.COM matches its table entry then checks THE NEXT BYTE IS BLANK — so every
     internal command was "Bad command or file name" UNLESS YOU TYPED A SPACE AFTER IT,
     which supplied the blank we should have. `ver ` worked and `ver` did not.
  3. **AN EXTENDED SEARCH RETURNS AN EXTENDED RESULT.** We skipped the 7-byte prefix on
     the way IN and wrote the answer back in the SHORT layout, so DIR — which must
     search with an extended FCB to see directories — read every field seven bytes
     early: blank names, one impossible size repeated, volume label "COM".
  4. **A FAILED SEARCH MUST SAY WHY.** Extended errors are only recorded where CF is
     set and FCB calls leave CF alone, so an exhausted search left `last_err` holding
     COMMAND.COM's startup probe for 0xFFFF paragraphs — error 8. DIR finished its
     listing, asked AH=59h why, and printed "Insufficient memory" over its own summary.
  5. Volume-label searches (attribute 08h) are not file searches — answered from
     GetVolumeInformation; and "." / ".." are NAMES, not empty extensions.

┌──────────────────────────────────────────────────────────────────────────────┐
│ ▶▶ SETTINGS: REGISTRY + DIALOG, AND THE PRECEDENCE CONTRACT                  │
└──────────────────────────────────────────────────────────────────────────────┘
  `HKCU\Software\NTVDMEX`, edited from **File > Settings...** (also wired to the old
  "Configuration Tool..." stub). Dialog template in `res/ntvdmhost.rc`, control IDs in
  `res/settings_ids.h` (shared with `src/host/settings.h`, because windres and the C
  compiler must agree). Values: ShowHostCursor, BlinkTextCursor, MouseSensitivity,
  DosVersionMajor/Minor, PitPace, UiTickMs.

⚠ **THE TEXT-FILE KNOBS STILL WIN, AND THAT IS THE WHOLE DESIGN:**
```
        built-in default   <   registry   <   text file on the share
```
  `settings_load()` runs at the TOP of the knob block in WinMain, never after it. The
  rig configures this host by writing files and re-launching; if the registry overrode
  them, every headless measurement would silently report whatever was last clicked in a
  dialog on that box. That is the exact "my instrument lied" failure this project keeps
  paying for. **Nothing in the harness had to change.** Do not "tidy" this ordering.

  ▶ HOST CURSOR IS NOW VISIBLE BY DEFAULT (it was hidden on an unmeasured lag theory).
  ▶ TEXT CURSOR BLINKS and takes its SHAPE from the guest (INT 10h AH=01 CX), which is
    also how a program HIDES it (CH bit 5, or start past end) — so shape and hide had
    to arrive together or an editor that turned the cursor off would get a blinking one.
    Phase comes from the injected clock, so the VDD stays pure/off-VM testable; with no
    clock the cursor is simply steady. MEASURED: 40 frames 200 ms apart on a static
    screen change by EXACTLY 16 pixels (one 8x2 cell) every ~400-600 ms, zero between.

═══════════════════════════════════════════════════════════════════════════════
██ ▶▶▶ OPEN, WITH THE EVIDENCE ALREADY GATHERED — START HERE NEXT TIME        ██
═══════════════════════════════════════════════════════════════════════════════

**1. `echo x > file` WRITES TO THE SCREEN AND LEAVES THE FILE 0 BYTES.** The one thing
   I could not land. THREE attempts failed and each aimed at the WRITE end:
     (a) AH=40h honouring a bound handle — no change;
     (b) AH=02h/`OUTC` routed through handle 1 — no change (ECHO does use AH=02h, so
         this was necessary, just not sufficient);
     (c) lowest-free-handle allocation, DOS-style — no change.
   All three are CORRECT on their own terms and are kept (handles 0-4 are ordinary
   table slots; DOS hands out the lowest free one; `std_open` tracks which of the five
   standard handles are still open). But the trace does NOT fit the textbook idiom:
```
     21:3c/06 ...        <- CREATE hi.txt
     21:3e/24 bx=0001    <- CLOSE handle 1, AFTER the create
     21:3e/24 bx=0005..0013
     ...no 45h and no 46h anywhere in the whole run...
```
   ► THE ONE FACT STILL MISSING is **what AH=3Ch RETURNS** — the handle number is the
     whole question and my trace prints only the request. I wrote that instrument and
     its patch silently failed to apply (the assertion fired, the build went ahead), so
     I stopped rather than guess a fourth time. DO THAT FIRST. Log the returned slot and
     `std_open`, then read the order again.

**2. THE PROMPT DEGRADES `C:\dostest>` → `C>` AFTER AN EXEC.** `$p` is the current
   directory, so AH=47h stops answering after a child runs. Spotted, NOT diagnosed —
   undiagnosed, not known-benign. Suspect our EXEC path disturbs the current directory.

**3. XP's OWN COMMAND.COM still exits during init**, now that the version knob gets it
   past "Incorrect DOS version" (set 5.00). It reaches deep shell init and terminates
   from CS:IP=0x95eb:0x03ce having printed nothing. AH=53h (BPB->DPB) was TESTED as the
   cause and **REFUTED** — answering success with a zeroed DPB gave the same CS:IP after
   the same 32 ms — and reverted rather than left lying. What it asks for and does not
   get is **INT 2Fh AX=122Eh**, the five DOS error-message table addresses
   (DL=00/02/04/06/08, five calls in a row, `ds:si=95eb:047a`). We pass unrecognised
   INT 2Fh straight through, so the guest reads its own registers back as our answer.
   ⚠ "Died after 122Eh" is still not "died because of 122Eh". Prove it first.
   ⚠ `guest/xp-command.com` is 50,620 bytes, Apr 14 2008 — pulled off the box, and
     `guest/` is gitignored (Microsoft's bytes; extract, don't commit).

**4. THE SETTINGS DIALOG HAS NEVER BEEN OPENED BY A HUMAN.** The template is embedded
   and its strings are in the binary (checked), the values are proven to load and apply,
   but nobody has clicked it. If it renders wrong, that is where to look first.

**5. Carried forward from session 25:** sound is 99.999% not 100% (hardware grounding
   untested); the Doom MELT/wipe screen may still pixelate.

═══════════════════════════════════════════════════════════════════════════════
██ ▶▶▶ RIG / HARNESS — WHAT CHANGED THIS SESSION                              ██
═══════════════════════════════════════════════════════════════════════════════
  Box 192.168.1.29. `mount_smbfs -N //guest@192.168.1.29/ntvdmex /tmp/xpshare`.
  ⚠ THE MOUNT DROPS. When it does, writes to /tmp/xpshare go to a LOCAL directory that
    shadows the mountpoint and everything silently succeeds against nothing. Symptom:
    `controld.txt` and `bm/` missing. Fix: delete the stray local files, remount.
  ⚠ Writing to the share needs `dangerouslyDisableSandbox` (the sandbox blocks it).
    Ping is filtered — ICMP silence does NOT mean the box is down; write a probe file.

  NEW LAUNCHERS
    `scripts/bm/cmdcom.bat`      headless shell test; `cmdcom.bat xp` for XP's own.
                                 Sets the qimode key gate FOR THE RUN AND PUTS IT BACK —
                                 a knob left flipped by a test is a trap for the next
                                 person at the box.
    `scripts/bm/cmdex.bat`       INTERACTIVE shell for a human: deletes `autoexit`
                                 (otherwise the 45 s headless deadline kills the session
                                 mid-typing) and forces qimode=0.
    `tools/dostest/extract-dos-file.py NAME guest/`   pulls a file out of the vendored
                                 msdos-622 floppy images (FAT12). COMMAND.COM and
                                 ATTRIB.EXE are on Disk1. `guest/` is gitignored.

  KEY SCRIPT (`keys.txt`) now understands `d2a` / `u2a` — make-only and break-only — so
  a MODIFIER CAN BE HELD. Every token used to be make+break, so `2a 34 aa` released
  shift before the period arrived: a test that typed `>` typed `.`, and the redirection
  test silently tested something else entirely.
  ⚠ The key script only runs when qimode bit 0x20 is set. Without it a headless shell
    just blocks at its prompt for the whole run and the log looks idle (`sc_push=0`).

  `dostrace.flag` on the share = log EVERY INT 21h call (AH/AL/BX/DX). It is a
  DIFFERENTIAL instrument: two runs differing by one typed space is what found defect 2.

⚠ TRAPS HIT AGAIN THIS SESSION, BOTH OLD FRIENDS
  * **A SWALLOWED `copy` ERROR RAN A STALE BINARY.** `cmdcom.bat`'s `copy ... >nul` hid
    a failure while an interactive host still held C:\ntvdmex\ntvdmhost.exe open. The
    ONLY symptom was one missing log line, and I nearly diagnosed the code instead.
    `deploy.bat` writes the deployed size to `deployed.txt` — use it whenever a change
    appears not to have taken.
  * **`controld` EXEC ARGUMENT QUOTING.** The share path has spaces, so
    `exec cmd /c "…\cmdcom.bat xp"` silently does nothing. Needs the inner path quoted:
    `exec cmd /c ""…\cmdcom.bat" xp"`. A run that never starts looks exactly like a run
    that produced no output.
  * I ALSO CHECKED A COPIED-BACK ARTEFACT BEFORE THE BATCH HAD REACHED THE COPY, and
    told the user the copy had failed. It had not. Wait for the run to finish.

★ METHOD, AND IT IS THE SAME LESSON AS THE MOUSE FIX BELOW: every one of these fell to
  WIDENING AN INSTRUMENT, and every wrong turn came from modelling instead of measuring.
  The AH=29h fix came from printing what went in AND what came out; my two prior models
  of `fcb_put_name` were both wrong and both plausible. The redirection defect is still
  open precisely because I guessed three times and only then went back to the trace.
  **A trace that prints the REQUEST but not the ANSWER is half an instrument.**
```
