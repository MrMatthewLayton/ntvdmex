═══════════════════════════════════════════════════════════════════════════════
██ PLAN — M9: DOS/BIOS COMPLETENESS (TDD). START HERE 2026-08-20. ██
═══════════════════════════════════════════════════════════════════════════════

▶ **THE SUPERVISOR PROMPT LIVES IN GITHUB: EPIC #24.** Read that issue first — it holds the
  what/why/how, the oracle policy, the failure-mode table and the full sub-issue checklist
  (#25-#46, milestone "M9 - DOS/BIOS completeness (TDD)"). This block is the local context
  that goes with it. Branch `main` is clean and pushed at `d07c27c`+; the spike branch is
  merged and deleted; start fresh from main.

▶ WHERE WE GOT TO: **Skyroads is fully playable** — menus, Controls screen, level select,
  gameplay, sound, and text, all user-confirmed on the physical box (sessions 11-12). That is
  the FIRST of the three titles on the acceptance bar (Doom / Skyroads / ZAR).

▶ THE DECISION (user, 2026-08-19): **completeness before breadth.** Hardening one game exposed
  how uneven the layer is underneath, so before running more applications we close the gaps
  test-first. The governing principle, in the user's words: *"There should be no cause in
  NTVDMEX itself to fail — whatever we throw at it should just work."* Illegal instructions in
  guest programs aside, a failure must never be OURS. The achievable form of that invariant is
  **no SILENT failure**: every unimplemented thing announces itself.

▶ WHAT THE SURVEY FOUND (2026-08-19) — the implementation is deep exactly where Skyroads
  walked and thin everywhere else:
    INT 10h    22 functions; unknown function = SILENT NO-OP (no error, no log)
    INT 10h    modes: only 13h and 12h are branched on -- everything else SILENTLY becomes
               80x25 text, so mode 0 gives 80x25 not 40x25, and mode 11h gives a text screen
               while the program writes pixels into A0000 (planar engine is 12h-only)
    INT 21h    37 functions; unknown = CF=1 + a log line, but AX is NOT set to an error code
    I/O ports  ~10 device ranges; unclaimed reads 0xFF, writes swallowed, port recorded
    IVT        planted: 08 09 10 16 1A 1C 21 2F 33 67, IRET stubs on 0A-0F and 70-77.
               EVERYTHING ELSE READS 0000:0000 -- INT 13h/11h/12h/15h/25h/26h far-jump into
               the IVT and execute it as code. This is the landmine; fix it first (#27).

▶ THE METHOD, AND THE ONE RULE: document the gap -> write tests FIRST and run them RED to
  prove it -> implement to green. **NEVER write an expected value from memory of what DOS
  does.** Tests written from belief encode our misconceptions as PASSING tests, which is
  worse than no tests because they look authoritative. This codebase has already been bitten:
  `15991e9` "fixed" the INT 10h font pointer from a plausible reading of the spec and was
  wrong -- only a screenshot of the actual pixels caught it. Expected values come from Ralf
  Brown's Interrupt List, confirmed against a real executable oracle.

▶ THE ORACLE = A PANEL, NOT A REFERENCE. Truth is AGREEMENT; disagreement is a flagged
  decision with a recorded rationale, never a coin-flip.
    - **MS-DOS 6.22 under QEMU on the dev Mac** = primary. Genuine Microsoft kernel, so for
      INT 21h semantics it IS the standard. User has 6.22 (believed floppy images -- CONFIRM
      THE MEDIA FORM FIRST, #25). Fast, offline, no rig.
    - **Stock ntvdm on the XP rig** = "what does the thing we are replacing do". Fine for the
      DOS API (a competent DOS 5.0 reimplementation); WORTHLESS for devices/sound/VESA, which
      is exactly where it is weak and where NTVDMEX differentiates. Never truth on its own --
      that was the user's own objection and it is correct.
    - **DOSBox** = a fourth voice (decades of distilled compatibility fixes).
    - **FreeDOS** = DELIBERATELY DEMOTED. Known divergence on picky software (it fails the
      Windows 98 installer's version/internals checks -- the user hit this personally). NO
      vote on truth, but it is the only oracle whose SOURCE YOU CAN READ when the others
      disagree and you need to know why.
    - **BIOS-LAYER CAVEAT, and it is real:** QEMU runs SeaBIOS, so for INT 10h/16h a QEMU
      answer is just another reimplementation's opinion regardless of which DOS sits on top.
      Real BIOS truth = the Dell OptiPlex booted off a DOS stick against its actual VGA BIOS:
      a horrible loop (physical reboots, no remote control), so it is the RARE TIEBREAKER for
      disputed BIOS cases, not part of the daily cycle. Note both bugs fixed in session 12
      were BIOS-layer, so this is the weakest part of our evidence base.

▶ ORDERING: prerequisites (#25 oracle ✅ DONE 2026-08-20 — see session-13 block below;
  #26 differential harness, #27 loud failure) FIRST.
  Then run the real applications -- command.com, edit.com, qbasic, Doom -- to turn the gap
  list into an EVIDENCE-RANKED list, because real programs exercise combinations no test
  author invents. Then TDD each gap in priority order. The two load-bearing gaps for
  `command.com` are **#30 EXEC (4Bh)** and **#29 find first/next (4Eh/4Fh)**; without them a
  shell cannot launch a program or list a directory.

▶ DOS VERSION IS A FEATURE, NOT A CONSTANT (#28): we currently report **5.0**
  (`AH=30h` -> `AX=0x0005`), same as ntvdm, which does not match the 6.22 oracle. The user
  wants it **selectable from the NTVDMEX menu** (the menu scaffold exists), and selecting a
  version must ALIGN THE API BEHAVIOUR, not just the number. Default to 6.22 so we match the
  oracle; it is also the friendlier lie, since most version checks are floor checks.

▶ TEST TIERS -- put each test in the right one: off-VM C battery (`tools/dostest/run.sh`,
  currently **325 checks**, runs on the Mac in seconds) for anything that is pure logic; a
  guest `.COM` through the differential harness for anything guest-observable; the rig
  (`selftest`, currently 8/8) as the final gate. selftest is a SMOKE TEST on the physical box
  at ~2 min a round -- it is NOT the TDD loop, do not try to make it one.

▶ DEFERRED BY DECISION -- DO NOT PICK UP UNASKED: keyboard/music latency (see the session-12
  block below). The user played it and rates it "genuinely playable, a little sluggish"; lag
  is down to milliseconds. Also queued behind this epic: hardware grounding (CPU affinity,
  SpeedStep), which lands on our timing path since guest clocks come from
  QueryPerformanceCounter.

═══════════════════════════════════════════════════════════════════════════════
██ CHECKPOINT — 2026-08-20 (session 13). ██
═══════════════════════════════════════════════════════════════════════════════

▶ RESTART POINT: branch `main`. **#25 (the MS-DOS 6.22 oracle) IS DONE AND WORKING.**
  Full write-up: `docs/research/dos-oracle.md`. Nothing is committed yet.

★★★ **THE PRIMARY ORACLE EXISTS.** Genuine Microsoft MS-DOS 6.22 under QEMU on the dev
Mac, queryable in ~3 s, offline, no rig:
    ./scripts/oracle.sh tools/dostest/dosver.com     # run a guest binary on real DOS
    ./scripts/oracle.sh --batch "VER"                # run DOS commands
    ./scripts/oracle.sh --selftest                   # 4/4 PASS
  Media = four retail floppies in `./msdos-622` (gitignored). Build =
  `python3 scripts/dosoracle/build.py`, ~75 s, produces `vm/dos622.img` (504 MB, 194 files
  in C:\DOS). Importable as `from dosoracle import Oracle` — that is the interface #26's
  differential harness should drive.

  ▶ **THE INSTALL DOES NOT DRIVE SETUP.EXE, DELIBERATELY.** SETUP is interactive and would
    need keystroke synthesis against screen state — the fragile loop that yields a slightly
    different disk every run. `PACKING.LST` on Disk 1 is MICROSOFT'S OWN compressed→expanded
    name table for all three disks, so the install is a deterministic list of EXPAND/COPY
    commands run over 5 non-interactive QEMU passes. Same genuine binaries either way.
    The supplemental disk has no PACKING.LST; its rules come from its own SETUP.BAT/SD6COPY.BAT.
    Names are NOT derivable from the underscore — `GORILLA.BA_`→`.BAS` but `DRVBOOT.BA_`→`.BAT`,
    `WNTOOLS.GR_`→`.GRB`. Unresolvable names are SKIPPED AND LOGGED, never dropped silently.

  ▶ CAPTURE, two paths on purpose: stdout via a scratch floppy (needs NO cooperation from the
    program, so unmodified binaries like MEM/DIR work) and COM1 serial (survives a guest that
    hangs after printing). Guest exits itself via `QUIT.COM` → QEMU `isa-debug-exit` → host
    status 85. No keystrokes, no screen scraping, no timing assumptions.

★★ **THE CARDINAL RULE ALREADY CAUGHT ME — IN MY OWN SELFTEST.** I asserted
`DIR C:\DOS\*.EXE | FIND "File(s)"` from memory of what DIR prints. Real 6.22 prints
**`file(s)` LOWER-CASE** and DOS's FIND is case-sensitive, so it matched nothing — **and the
check still reported PASS**, because it only asserted "some output appeared" and a stray
sentinel satisfied that. Both halves are the lesson: an expectation from memory is wrong more
often than it feels, and a weak assertion converts a silent failure into a confident green tick.
Selftest cases now assert a substring transcribed from the oracle's real output.

★ FIRST ORACLE-CONFIRMED EVIDENCE, for **#28 (configurable DOS version)** —
`tools/dostest/dosver.com` (new):
    INT21.30   major=06 minor=16 oem=FF serial=00:0000     ⇒ AH=30h must return **AX=0x1606**
    INT21.3306 major=06 minor=16 rev=00 flags=10 cf=00
  We currently return **AX=0x0005** ("5.0"). BH=0xFF = generic MS-DOS OEM; serial is zero.
  ► **DH=0x10 from 3306h is CONFIG-DEPENDENT, not a constant** — bit 4 = "DOS is in the HMA",
    and this image boots `DOS=HIGH`. Any test asserting DH must control DOS=HIGH too.

▶ TRAPS FOUND BUILDING IT (all cost real time; all in docs/research/dos-oracle.md):
  • **DOS parses `<` and `>` inside ECHO.** Sentinels `---8<---`/`--->8---` made DOS write a
    file literally called `8---`. Use `[BEGIN]`/`[END]`.
  • `ECHO foo > file` writes `"foo "` — the space before the redirect is part of the text.
  • **vvfat is NOT USABLE.** `fat:ro:` as a hard disk is refused ("Block node is read-only");
    as `fat:floppy:ro:` it served the **WRONG FILE CONTENTS** (TYPE of a 20-byte file printed
    fragments of vvfat's own volume structures) and then hung the guest. `--dir` synthesises a
    throwaway raw FAT image with mtools instead. This supersedes #25's `-drive file=fat:ro:`
    instruction — the reason for that instruction (vvfat is corruption-prone) stands; the
    remedy does not go far enough.
  • **The dev sandbox refuses to bind a unix socket anywhere**, TMPDIR included — the QEMU
    monitor runs on **stdio**, not QMP.
  • The floppies' boot-sector OEM field says `MSDOS5.0` on all four disks. That is what 6.22's
    own FORMAT writes; it is NOT a version indicator. Genuineness was confirmed from the
    `1994-05-31 6:22` build stamp on every file instead.

▶ KNOWN GAP, NOT PAPERED OVER: screenshots are captured only on TIMEOUT, as a diagnostic.
  There is no capture-screen-on-success path, so this oracle cannot answer "what pixels did
  that produce". Per the epic's own BIOS caveat QEMU video would be weak evidence anyway
  (SeaBIOS, not a real VGA BIOS) — video questions want the Dell OptiPlex.

★★★ **#26 DIFFERENTIAL HARNESS — TWO OF FOUR HOSTS LIVE, DIFF WORKING.**
    `./scripts/dosdiff.py tools/dostest/p_ver.com` runs ONE unchanged .COM on every available
    host and prints a per-field agreement table.
  ▶ **THE CANONICAL DUMP** is `tools/dostest/probe.inc` (shared NASM include). A probe emits
    `CASE=<name> SIG=<regs> AX=.. BX=.. .. CF=x`. Write new probes with it, not by hand.
    ► **`SIG` is the load-bearing idea.** It declares which registers actually carry THAT
      case's answer. All registers are dumped, but DS/ES follow the PSP (`04BD` on the oracle,
      `0813` under DOSBox) and most FLAGS bits are undefined after a DOS call — comparing them
      blindly manufactures disagreements that bury the real ones.
    ► `probe_capture` MUST be the instruction immediately after the call under test.
  ▶ **VOTING: NTVDMEX DOES NOT VOTE.** It is the `subject`; the others are `oracle`s. Letting
    the thing being graded into the consensus would be circular. Hosts that cannot run are
    reported UNAVAILABLE WITH THE REASON, never silently skipped — a diff that quietly dropped
    a host would read as agreement.
  ▶ **RECORDED RATIONALES ARE IN THE TOOL**, not just prose: `tools/dostest/oracle-rules.json`
    (mirrored in `docs/research/oracle-disagreements.md`). A rule makes a host ABSTAIN on one
    field, and the reason prints on every run. Reason: a permanently DISPUTED row you have
    learned to ignore is worse than no row.
  ▶ LIVE: `msdos622` (QEMU oracle), `dosbox-x`, and **`ntvdmex` ON THE BARE-METAL RIG — the
    subject column works, so the harness now GRADES US.** Adapter = `NtvdmexRig` in dosdiff.py,
    driving the existing SMB watcher loop (drop probe in `bm/tests/`, write `cmd.txt`, read
    `result_<name>.log`, pull the `==> DOS OUTPUT: [...]` block). It waits for the
    `STAGE2: complete` line rather than a fixed duration, which is the documented fix for the
    partial-log/SMB-cache trap, and it refuses to run if `watcher.txt` is not MOVING (a stale
    heartbeat looks identical to a live one).
  ▶ **THE RIG IP MOVES — DO NOT HARDCODE IT.** It was `.34`, is `.29` today (2026-08-20) after
    a broadband outage; `.27` was a mis-recollection. Find it, then
    `mount_smbfs -N //guest@<ip>/ntvdmex /tmp/xpshare`. A ping sweep is the quick way, but note
    an ARP sweep leaves INCOMPLETE entries for every address — `arp -a` after a sweep lists all
    254 and means nothing. Probe port 445 instead. LAN needs `dangerouslyDisableSandbox`.
  ▶ STILL NOT WIRED: stock `ntvdm`. It needs the rt.bat variant AND a user decision — the
    repo's own note says a stock full-screen DOS run wedges the box's display, costing a
    physical reboot. Flagged to the user, not run.

★★ **FIRST REAL GRADING — 5 MISMATCHES, AND ONE IS THE #27 SILENT-FAILURE CLASS CAUGHT LIVE:**
    case             field  msdos622 dosbox-x ntvdmex
    int21.30         AX     1606     0005     0005     MISMATCH  (known, #28)
    int21.30         BX     FF00     FF00     B1B1     MISMATCH  <- UNTOUCHED
    int21.30         CX     0000     0000     C1C1     MISMATCH  <- UNTOUCHED
    int21.3306       BX     1606     0005     B1B1     MISMATCH  <- UNTOUCHED
    int21.3306       DL     00       00       D1       MISMATCH  <- UNTOUCHED
    int21.3306       CF     0        0        0        AGREE     <- THE DANGEROUS ONE
  ► `B1B1/C1C1/D1D1` = the probe's POISON pattern, i.e. **the register was never written**.
    Added after the first rig run returned `BX=3246` for 3306h and 3246 turned out to be a
    value MY OWN `probe_emit` had left in BX. Without poison, "untouched" and "deliberately
    zero" are indistinguishable and you will misread leftovers as answers.
  ► **CONFIRMED IN SOURCE, not inferred:**
    - `src/dos/dos_int21.c:235` — `ah == 0x33` handles ONLY `AL=00` (get Ctrl-Break); every
      other subfunction, `AL=06` included, falls through to `OKCF()`. So "get true version"
      returns **CF=0 (success) with all registers untouched**. The guest is told it succeeded
      and handed garbage. This is exactly #27's target, caught by the harness on day one.
    - `src/dos/dos_int21.c:177` — `ah == 0x30` is `SETAX(0x0005); OKCF();`. BX and CX are never
      written, so the OEM number and the 24-bit serial are whatever the CALLER left there.
      Real DOS returns BH=0xFF, BL:CX=0.
  ▶ **DOSBOX CANNOT RUN HEADLESS.** `SDL_VIDEODRIVER=dummy` makes dosbox-x HANG and makes
    dosbox-staging ABORT ("Could not initialize video: OpenGL ... driver (dummy)"). The adapter
    opens a real window briefly; it will not work over plain ssh. Use `dosbox-x`, not staging.
  ▶ FIRST DISAGREEMENT, already settled: DOSBox-X reports **AX=0005 (DOS 5.0)** against the
    oracle's **1606 (6.22)** — because DOSBox's version is a CONFIGURABLE EMULATOR SETTING, so
    it is evidence about DOSBox's default, not about MS-DOS. It abstains. (Note we currently
    report 0005 too — matching DOSBox's default, not real DOS.)

★★★ **#27 LOUD FAILURE — DONE (host v146, rig-verified, selftest 8/8, off-VM 325/325).**
  ▶ **THE NULL-VECTOR LANDMINE IS REAL, BUT NOT WHERE THE SURVEY SAID.** Measured before
    fixing, and the measurement narrowed the fix. The vectors #27 named (11h/12h/13h/15h/25h/26h)
    are **NOT null on the rig** — they carry the VDM's own BIOS entries (INT 13h read
    `F000:5595`, INT 11h `F000:F84D`). Blanket-planting an IRET over those would swap a working
    handler for a silent "success" — the exact failure mode the issue exists to remove. The
    genuinely null ones are the USER-INTERRUPT ranges: **`0x60-0x66` and `0x78-0xF5`, 133 of
    them.** So the fix fills only vectors that read 0000:0000, and names them:
      `STAGE0: null IVT vectors -> IRET stub: 0x60-0x66 0x78-0xf5`
  ▶ **A RUN NOW YIELDS A TO-DO LIST** (STAGE2, always printed — "none" is a positive statement
    that nothing was missing, which a suppressed line is not):
      `STAGE2: INT21 unimplemented: AH=0x73 AH=0x88 AH=0xff`
      `STAGE2: INT10 unimplemented: none`
      `STAGE2: video modes unsupported: none`
    INT 10h unknown functions and unsupported mode numbers are recorded in the video VDD
    (`unimpl_fn`/`unimpl_mode` bitmaps in video_state, kept dependency-free) and drained by the
    host. The INT 21h tail logs `AH=0x.. UNIMPLEMENTED` and sets a bitmap.
  ▶ **THE DOS-OUTPUT CAPTURE WAS SILENTLY TRUNCATING AT 1 KB** (`char dosout[1024]`), which cut
    a probe dump mid-line — and the missing rows READ AS AGREEMENT in the harness. Two fixes:
    buffer raised to 16 KB, and truncation now appends `<<<OUTPUT TRUNCATED>>>` instead of
    dropping bytes. **Also fixed in dosdiff.py: a field with no subject data is now NO-DATA, not
    AGREE.** That was a false-green in my own tool, found by being bitten by it.
  ▶ **#27's STATED FIX IS REFUTED BY THE ORACLE.** The issue says *"Set AX=1 (invalid function)
    alongside CF on unhandled INT 21h calls; DOS sets both, we only set carry."* Real MS-DOS
    6.22 sets **NEITHER** — `AX` unchanged, `CF=0` — measured on AH=FFh/73h/88h. We return
    CF=1, so we DIVERGE, and that is **kept deliberately**: our unhandled tail is reached both
    by functions DOS does not define (match = CF=0) and by functions DOS defines that we have
    not written yet (4Bh/4Eh/39h — where CF=0 would claim success for a no-op). Splitting them
    needs a table of which AH values 6.22 defines; #29-#38 need that table anyway. Recorded in
    full in `docs/research/oracle-disagreements.md` so nobody "fixes" it without the argument.
  ▶ DOSBox-X abstains on `int21.73/CF`: AH=73h is the DOS 7.1 FAT32 group, absent from 6.22.

★★★ **THE 6.22 FUNCTION TABLE + #28 VERSION + FIRST REAL APP RUNNING (host v149).**
  ▶ **`dos622_defines()` in dos_int21.c splits the unhandled tail in two**, because the halves
    want opposite answers: a service 6.22 DEFINES that we have not written → CF=1, loud (a quiet
    "success" makes the guest believe a no-op worked); a service 6.22 does NOT define → AX
    unchanged, CF=0, exactly as real DOS. We now MATCH the oracle on the second class.
    **BOUNDARY MEASURED** (`tools/dostest/p_defs.asm`): 6Dh..E0h all return with every poisoned
    register intact — nothing happens. 6Ch is the highest that does anything. Documented nulls
    18h/1Dh/1Eh/20h/61h/6Bh carry the same signature and sit on the quiet side.
    ► **TRAP:** "AX came back unchanged" is NOT a test for absence — 54h returns AL=0 when
      verify is off (same as the AL=0 passed in) and 2Ch leaves AX alone while writing CX/DX.
      The signature that works is EVERY OUTPUT REGISTER STILL POISONED. The probe's control
      cases exist to catch a broken discriminator and here they earned it.
  ▶ **#28 DONE (core): we report 6.22.** AH=30h → AX=0x1606, **BH=0xFF and CX=0 now actually
    written** (they never were — callers read their own leftovers as our OEM/serial). AH=33h
    gained AL=01/05/06; 3306h returns the true version. Version lives in `m->ver_major/minor`,
    default 6/22. **The menu selector is NOT built yet** — that half of #28 remains.
  ▶ **#35 STARTED: 58h (alloc strategy, all 4 subfns) + 52h (list of lists).** Oracle-confirmed
    defaults: 5800h→AX=0000 first fit, 5802h→AL=00 UMBs unlinked. For 52h only the word at
    ES:BX-2 (first MCB segment) is real and the rest of SysVars is **deliberately ZEROED** — a
    walker that follows a garbage DPB/SFT pointer wanders into nonsense (silent failure),
    whereas a null pointer stops it.

★★★★ **REAL MS-DOS 6.22 `MEM.EXE` RUNS UNDER NTVDMEX AND PRINTS ITS MEMORY REPORT.** First
genuine 6.22 utility working. The loop that got there is the point and it is now self-sustaining:
run a real app → read `STAGE2: INT21 unimplemented:` → implement exactly that → re-run.
    run 1: "Incorrect DOS version"        → #28
    run 2: to-do list = AH=0x52, AH=0x58  → implemented
    run 3: renders; to-do list = AH=0x38  (country info, #38) ← NEXT
  ► Real 6.22 binaries are staged on the rig in `bm/tests/` (MEM, CHKDSK, TREE, ATTRIB, MORE,
    XCOPY, COMMAND.COM), extracted from the oracle image with mtools into `build/dosapps/`.

★★ **A REAL LATENT BUG FOUND ON THE WAY — the "safe IRET stub" was not an IRET.** The shared
stub lived at `DOS_HDLR_SEG:0x66`, but the DPMI real-mode callback slots are based at 0x60 with
a 4-byte stride, so **slot 1 owns 0x64-0x66** and its third byte (`DPMI_CB_BOP` = 0x55) is
written AFTER the stub. 0x55 decodes as `PUSH BP`, then execution runs into uninitialised
memory. Every vector pointed at the "safe" stub was pointed at a crash. Latent for IRQ 2-7/8-15
since session 11 — and #27's null-vector sweep had just aimed 133 more vectors at it. Moved to
`DOS_IRET_STUB_OFF 0x0058`, and **the three async-delivery guards that compare against the stub
offset were updated with it** (they are what stops us injecting an IRQ the guest never hooked).
A handler-segment offset map is now in main.c next to the define — check it before adding stubs.

★★★ **THE EVIDENCE-RANKED GAP LIST (host v151), from running the real 6.22 tools on the rig.**
  This is the artifact the epic asked for, and it is ranked by what programs ACTUALLY call:
      AH=47h  get current dir      TREE, ATTRIB, XCOPY, COMMAND.COM   4 of 5  <- done
      AH=65h  ext country info     ATTRIB, COMMAND.COM                2
      AH=4Eh  find first           TREE (load-bearing for DIR, #29)    1  <- NEXT
      AH=60h  truename             CHKDSK                             1
      AH=5Dh  server/internal      COMMAND.COM                        1
      AH=11h  FCB find first       TREE (#36)                         1
      AH=59h  extended error       TREE (#34)                         1
      AH=69h  disk serial          TREE (#35)                         1
  ▶ DONE THIS ROUND, all oracle-matched in dosdiff: **38h country info (#38), 47h get current
    dir + 3Bh chdir (#32), 36h free space (#35)**, on top of 30h/33h/52h/58h.
    To-do lists shrank: TREE `11 3B 47 4E 69`→`11 4E 59 69`; COMMAND.COM `47 5D 65`→`5D 65`;
    CHKDSK `36 60`→`60`; MEM.EXE → **none**.
  ▶ **MEASURE THE BUFFER, NOT JUST THE REGISTERS.** 38h's country block is **24 bytes, not the
    commonly quoted 34** — proved by poisoning the destination with 0xEE first; DOS leaves
    everything past byte 23 alone. Writing 34 would have silently clobbered 10 bytes of the
    caller's memory. Same trick showed 47h at the root writes exactly ONE byte (the NUL).
  ▶ 38h hands back a FAR pointer to DOS's case-map routine; ours points at a planted **RETF**
    (`DOS_CASEMAP_OFF`), so a program that actually calls it returns safely (identity mapping)
    instead of jumping into nothing.
  ▶ **36h on a bad drive returns AX=FFFF with CARRY CLEAR** — not a CF error. Easy to get wrong.
  ▶ TWO MORE DOSBOX ABSTENTIONS, both "correct answer to a different question": 36h's
    sectors-per-cluster is VOLUME GEOMETRY (oracle 16, DOSBox 64, rig 8) so it is no longer
    compared at all; and DOSBox **always mounts Z:**, so the "drive does not exist" case is
    false there.

▶ **KNOWN DEFECT, NOT PAPERED OVER: MEM.EXE runs to completion but its NUMBERS ARE WRONG.**
  "Largest executable program size 0K (4,294,967,280 bytes)" is -16 unsigned; conventional free
  reads 0K; XMS reads 0K despite a 16 MB pool. Zero unimplemented functions are reported, so
  this is a SILENT wrongness in the MCB chain / SysVars stub (52h returns only the MCB head) and
  in XMS reporting. Worth fixing before trusting any memory-related parity claim.

★★★ **#29 FIND FIRST/NEXT (4Eh/4Fh) DONE — `TREE.COM` NOW RENDERS ITS DIRECTORY LISTING.**
  Oracle-matched on every case. Second real 6.22 tool working (after MEM.EXE).
  ▶ **DTA BLOCK LAYOUT read off the oracle** and documented in dos_int21.c. The dump
    cross-checks itself: the size field came back 0xD575 = 54645 = COMMAND.COM's exact byte
    count. Block is **43 bytes** — byte 43 came back still poisoned, so nothing past it may be
    written. Fields: [21] attr, [22-23] time, [24-25] date, [26-29] size, [30-42] name ASCIIZ.
  ▶ **A PATTERN THAT MATCHES NOTHING IS AX=18 ("no more files"), NOT AX=2.** Memory says 2;
    the oracle says 0x12. A missing *directory* is AX=3. Getting this from memory would have
    been wrong in the most plausible-looking way.
  ▶ Win32 handles live in `m->find_h[8]`; the slot is stashed in the DTA's own search-state
    bytes, so a program that saves and restores its DTA between calls resumes the right search.
  ▶ **WE MATCH A 6.22 QUIRK: a FAILED 4Eh does not clobber a live search in the DTA**, so a
    following 4Fh carries on the earlier search. DOSBox-X resets it. We match DOS because our
    4Eh only writes the DTA search fields on success — now recorded as deliberate, not accident.
  ▶ Probe case naming matters: this case was first called `4F.exhausted`, which made a correct
    result look like our bug. Renamed to `4F.after.failed.4E`.

▶ **COVERAGE (measured): 45 of the 103 INT 21h services 6.22 defines = 44%.** Of the 58 missing,
  19 are the legacy FCB group (#36). Per-app remaining: MEM **none**, CHKDSK `60`,
  COMMAND.COM `5D 65`, TREE `11 59 69`.

★★★ **59h / 60h / 65h / 69h DONE. EVERY APP IS DOWN TO ONE MISSING FUNCTION:**
      MEM **none** · CHKDSK **none** · ATTRIB `43` · COMMAND.COM `5D` · TREE `11`
  ▶ **#34 extended error: class/action/locus are MEASURED, not guessed.** `p_err.asm` provokes
    each failure and asks 59h what DOS made of it: codes 2/3/18 → BX=0803 CH=02; code 6 →
    BX=0704 CH=01. **CL is never written** (came back poisoned), so we leave it alone. Codes we
    have not provoked LOG "class/action/locus UNMEASURED" rather than inventing a class.
    `last_err` is captured in ONE place — the end of the dispatcher, where CF and AX are already
    what the guest will see — not at each of the ~20 error sites.
  ▶ **#38 character tables: DUMPED, NOT SYNTHESISED.** ATTRIB wants 65h AL=07, COMMAND.COM
    AL=04, so the AL=01 country block was not enough. Each subfunction returns ES:DI → a 5-byte
    descriptor {id, FAR ptr}. The tables are in `src/dos/dos_ctab.h`, dumped byte-for-byte off
    the oracle by following those pointers (`p_ctab.asm`). The collating table in particular is
    NOT guessable — it folds accented chars onto unaccented letters in a specific way.
    Rig output is byte-identical to the oracle for all five tables.

★★ **A BUG THAT WEDGED EVERY GUEST, INCLUDING SELFTEST — READ THIS BEFORE PLACING DATA IN LOW
MEMORY.** The tables first went at `DOS_CTAB_SEG 0x0071`, which is the DOS-resident MCB filler
block's data area (linear 0x710) and looks like exactly the right home. **Linear 0x714 is the
KERNEL's VDM interrupt-state dword** — the `[0x714]` session 10 found wedges guests when written
from user mode. Planting 600 bytes at 0x710 wrote straight over it, and EVERY run died with no
log past "running .COM": selftest, p_misc, p_ctab. Moved to `0x0090` (linear 0x900), which
clears it and still ends below the next MCB header at 0xFF0. selftest recovered immediately.
  ► The lesson generalises: the MCB map says that block is free, and the KERNEL disagrees.

▶ **HARNESS GAP (known, not fixed): `EMIT_BUF` contents are dumped but NOT diffed.** dosdiff
  compares registers only, so the five character tables were verified by eye against the oracle
  dump rather than by the tool. Buffer contents are the substance of many DOS calls (country
  block, DTA, tables) — comparing them belongs in dosdiff.

▶▶ RESUME — NEXT STEPS (in order):
  1. Finish the app queue: **43h** (ATTRIB), **5Dh** (COMMAND.COM), **11h/12h** FCB find (TREE, #36).
  2. **#30 EXEC (4Bh)** — after which a shell can actually launch a program.
  3. Fix the MEM.EXE numbers (silent wrongness, see above).
  4. Then the BIOS layer (#39-#46), which is the thin part: ~19 INT 10h functions, only 2 of
     ~19 video modes, and INT 13h/14h/15h/17h/25h/26h essentially absent. Note the oracle is
     NOT truth there (SeaBIOS) — video questions need the Dell box.
  2. **Finish #26: wire stock ntvdm** (still needs the rt.bat IFEO variant + the user's call on
     the display-wedge risk).
  3. #28's menu selector. Until `ntvdmex` runs there is NO subject in the
     table and the harness cannot grade us — it only cross-checks oracles. Stock `ntvdm` needs
     an rt.bat variant that drops the IFEO Debugger key for the baseline run and restores it.
  2. **#27 make every unimplemented path LOUD.** The IVT landmine first (vectors that read
     0000:0000 and get executed as code).
  3. Then the EVIDENCE PASS: run command.com / edit.com / qbasic / Doom and rank the gaps by
     what real programs actually hit, before implementing any of #29-#46.

═══════════════════════════════════════════════════════════════════════════════
██ CHECKPOINT — 2026-08-19 (session 12). ██
═══════════════════════════════════════════════════════════════════════════════

▶ RESTART POINT (2026-08-19, session 12): branch spike/dpmi-16bit-switch. Host rebuilt clean
  and deployed to `bm/`; rig healthy (watcher + controld beating); share knobs CLEARED (no
  qimode.txt, no keys.txt, no capture.flag, headless cap back to 30 s). Verified this session:
  selftest on the rig **ALL TESTS PASSED**, off-VM input battery **34/34** (rewritten — it now
  tests the guest's BDA, not a host-side stand-in).

★★★★★ **SKYROADS MENUS NOW NAVIGATE** — screenshot-confirmed: intro → menu → DOWN to
"Controls" → Enter → the Controls screen, and on the level-select screen the cursor moves off
"Red Heat / Road 1" to Asteroid Belt. Session 11 got the game PLAYING; this session got its
MENUS working, which was the "arrows are dead in the menu and intro" report.

  ▶ TWO ROOT CAUSES, BOTH ABOVE THE DELIVERY LAYER (delivery was already correct):
  1. **The BIOS keyboard buffer was never maintained.** Our INT 09h stub consumed each
     scancode and DISCARDED it. INT 16h only appeared to work because the window proc pushed
     keycodes into a SEPARATE host-side ring that no DOS program can see. Recognise it by:
     the guest sees `0040:001A` == `0040:001C` == `0x001E`, frozen, forever. INT 09h now
     tracks the E0 prefix + shift/ctrl/alt/lock into `0040:0017`, translates make codes to
     BIOS keycodes (AL=0 for extended keys) and fills the real ring at `0040:001E`; INT 16h
     reads that same buffer; the parallel host-side ring is GONE (one buffer, by construction).
  2. **DOS could not express an extended key.** Every INT 21h console read did
     `return k & 0xFF`, so an arrow (0x4800) arrived as a lone NUL with the scancode thrown
     away. DOS returns NUL and then the SCANCODE ON THE NEXT CALL; `g_conin_pending` now
     carries that second byte across conin/coninnb/conpeek. Without this, arrows are
     structurally unreadable through DOS no matter how perfect the hardware layer is.
  Both were required: (1) puts the key where DOS looks, (2) lets DOS say "arrow".

  ▶ ROUTES — a fix for one proves NOTHING about the other:
  Skyroads' MENU reads keys through **INT 21h** (the guest parks at `DOS_HDLR_SEG:0000`, the
  INT 21h BOP, for most of a run — that heartbeat is what cracked this). IN-GAME it hooks
  **INT 09h and reads port 60h itself** (measured p60=358 in a gameplay run).

  ▶ TRAPS THAT COST TIME THIS SESSION — do not repeat:
  - **`int16=[0,0,0,0]` + `p60=0` does NOT mean "reads no keyboard."** It means "reads by a
    route that leaves no trace" — i.e. DOS calls or direct BDA polling. An earlier session
    concluded the intro reads nothing; it was reading via INT 21h the whole time.
  - **Skyroads has an ATTRACT LOOP** that reaches the credits and even demo gameplay unaided.
    Frames of "it's in game!" are worthless without a NO-KEY control run at the same timings.
    I misread attract frames as success once before the control run corrected it.
  - **Never leave `qimode.txt` on the share.** It drives synthetic keys every 250 ms, which
    makes any interactive probe look wedged (it cost the user a trip to the box).
  - A probe that installs its OWN INT 09h (keyprobe) BYPASSES the host BIOS handler, so it
    cannot test the BDA path at all. That is what `bdaprobe.com` is for — it hooks nothing.
  - Disproved by instrumentation, not argument: the scancode FIFO is NOT overflowing
    (`sc_drop=0` over 514 pushes). The new `sc_push`/`sc_drop` counters exist for this.

  ▶ BEHAVIOUR CHANGE TO KNOW: a guest that hooks INT 09h and does NOT chain now gets no
  INT 16h keys — faithful to real hardware (it replaced the BIOS ISR), but it changed
  keyprobe's output to `B16=(none)`. Guests that chain are unaffected.

  ▶ NEW TOOLING: `tools/dostest/keyprobe.com` (prompted per-key ground truth: RAW port-60h
  bytes / INT 16h AX / shift flags / BDA head-tail) and `tools/dostest/bdaprobe.com` (hooks
  nothing; watches 0040:001A-001C). Two new share knobs: `headless_ms.txt` (decimal ms,
  overrides the 30 s headless cap, clamped to 10 min — needed for interactive runs) and
  `keys.txt` (**scripted** synthetic keystrokes: `w1500` waits, `4d` taps, `e4d` taps an
  EXTENDED key — a hardcoded "tap UP 400x" cannot reach a screen, and UP is a no-op on a menu
  whose first item is already selected, so it cannot tell success from failure).

  ▶ GARBLED TEXT: **FIXED** (`633aae5`) and **user-confirmed in-game** — "Road Completed"
  renders correctly on the physical box, and gameplay through a whole road is therefore
  observed, not inferred. Two bugs, the first HIDING the second: (1) `regs_store` wrote back
  only EAX/EBX/ECX/EDX while `regs_load` read all seven, so **ES:BP was discarded** and the
  guest drew text from whatever pointer it already held -- which is why 15991e9, correctly
  setting ES:BP, changed nothing; (2) the 8x8 ROM font was MANUFACTURED by OR-ing row pairs
  of the 8x16, filling every counter ('A' solid, 'E' noise). Real 8x8/8x14/8x16 dumps now
  ship. The tell that cracked it: after fixing the font data alone the render was
  BYTE-IDENTICAL, proving the guest had never read our table.

  ▶ PERFORMANCE, as played by the user on the physical box (a calibration, not a complaint):
  **genuinely playable**, but with the feel of a game speced for a 386 16MHz / 2MB running on a
  **386 8MHz / 512KB** — a little sluggish. Keyboard and music lag are perceptible but now in
  the **milliseconds**. **DEFERRED BY DECISION — do not pick this up unasked.** When it is
  picked up: keys are still restricted to the SYNCHRONOUS exec-loop path in `host_irq_sink`
  (async key delivery off by default after it once made things worse) while the timer gets
  async delivery, and no instrument measures the real latency yet (needs an echo-on-arrival
  probe, no settle, no drain — the 1-2 s seen in keyprobe was that probe's own settle).

  ▶ NEXT DIRECTION (user's call, 2026-08-19): **GO BROAD, NOT DEEP.** We have hardened exactly
  ONE real DOS application. Start running a plethora of others -- `command.com`, `edit.com`,
  `qbasic`, Doom, and on -- and let breadth of exposure tease out the remaining problems.
  Polishing Skyroads further is NOT the priority.

  ▶ NEW WORKSTREAM: **hardware grounding** — CPU affinity, SpeedStep / power management and
  friends, handled in realistically stable code. This lands directly on our timing path: guest
  clocks come from QueryPerformanceCounter (session-11 `host_pit_sync`), so core migration and
  frequency scaling are in it. Note the framing: **XP's own ntvdm never grounded any of this**,
  so it is superset territory and a real differentiator rather than parity work.

═══════════════════════════════════════════════════════════════════════════════
██ CHECKPOINT — 2026-08-19 (session 11). ██
═══════════════════════════════════════════════════════════════════════════════

▶ RESTART POINT (2026-08-19, session 11): HEAD = `3164690`; branch spike/dpmi-16bit-switch;
  **38 commits UNPUSHED**. Host = **dpmi-harness-v126**, built clean, deployed to the share `bm/`,
  and RE-VERIFIED on the rig: selftest **8/8 PASS**, off-VM battery **519/519**. Rig healthy
  (watcher + controld beating). Working tree clean except the same pre-existing untracked files
  that are NOT mine (MAINICON.ico, demos/, scripts/kd_*.py, scripts/trace_break.py) and the
  untracked native `tools/dostest/*_test` binaries (repo convention). New this session:
  `build/re/` holds XP's ntoskrnl/ntvdm/ntdll pulled off the box for RE (gitignored; /tmp was
  wiped, so re-fetch with the cmd.txt injection trick below if it disappears again).

  THIS SESSION went after session 10's blocker ("we cannot asynchronously interrupt a V86
  guest"). It found the kernel lever, found that a line of session-10 code was ACTIVELY
  WEDGING guests, and -- the most consequential result -- found that **the blocker was
  misdiagnosed**: Skyroads is not starved of injection points, it is being refused them.

★★★★★ **SKYROADS IS PLAYABLE** (user-confirmed on the physical box, 2026-08-19). Host v126.
It boots, renders, animates its intro, plays OPL music and PCM, takes keyboard input and
plays. Getting there took four fixes after the async-injection breakthrough, three of them
timing and NONE of them in the sound code:
  1. **The guest's CLOCK was driven by the UI thread.** Skyroads reads PIT counter 0 directly
     (`out 43h,al; in al,40h; in al,40h` at 0110:5a85 -- caught by the new IO-SITE dump), so
     that counter IS its sense of time; we advanced it from the frame loop, which is starved
     exactly when the guest is hammering I/O. Everything time-paced crawled: fade, music
     tempo (pitch was always right -- the OPL is correct, only the sequencer was slow) and the
     rate it fed PCM (slow AND pitched down). Now `host_pit_sync()` derives clocks from
     QueryPerformanceCounter, called from BOTH threads -- exec (so a poll reads real time) and
     UI (so the clock runs while the guest spins). Driving it from one only deadlocks.
  2. **My own 55 ms rate limit on async IRQ0 pinned the timer to 18 Hz** while the game asked
     for 180. Removed. Per-3s delivered ticks went ~3 -> ~540.
  3. **Timer ticks were coalesced by a boolean pending flag** and dropped whenever the guest
     was CLI'd (which is most of the time -- it CLIs around 256-colour palette writes, 585k
     writes to 0x3C9 per run). Now a saturating count (cap 4), like a real 8259 latching.
     Delivery is now 540/541 per 3 s against a programmed 180 Hz.
  4. **Keyboard IRQ1 never got the async path**, so input arrived whenever the game yielded.
  Plus: default IRET stubs for IRQ2-7/8-15 (their vectors pointed at unowned ROM); never
  deliver a line whose vector is still that stub (Skyroads installs no SB ISR, and delivering
  derailed it); INT 21h AH=00 implemented; audio thread at TIME_CRITICAL with 6 buffers
  (starvation by the 100%-busy exec thread was heard as ticking).
  ► METHOD NOTE worth keeping: every one of these was found by MEASURING, not reasoning --
    per-3s heartbeat deltas showed the game doing all its work in 3 s then idling at ~1 Hz,
    and a port histogram showed the real load was the palette. My first port histogram was
    WRONG (12 slots, filled before the hottest port appeared) and my first slowness diagnosis
    was wrong with it. Widen the instrument before trusting the reading.

★★★★ THE BLOCKER IS BROKEN: **WE CAN NOW INTERRUPT A SPINNING V86 GUEST.** Three sessions
were stuck behind this. The kernel will not do it (VdmQueueInterrupt is transition-only --
proven, see below), so we do it ourselves: **a suspended thread's CONTEXT is readable and
writable even while it sits inside VdmStartExecution, and for a V86 thread that context IS
the guest's frame.** `async_inject_irq()` (src/host/main.c) does from the device thread
exactly what the CPU does on a hardware interrupt -- push FLAGS/CS/IP on the guest stack,
vector CS:EIP through the IVT -- via SuspendThread / GetThreadContext / SetThreadContext /
ResumeThread. Commit `c1e5c34`, host v116, opt-in behind **qimode bit 4** (`10`).
  ► Guard rails (we are rewriting a context the kernel is actively running): only when
    EFLAGS.VM is set; only when the guest's interrupts are on (**IF or VIF**); never while CS
    is our own handler segment; and never unless the exec thread is inside `v86_run`
    (`g_in_exec`), so it cannot race the exec loop. Declines are counted (`async_bail`).
  ► **The timer needed this as much as the devices did** -- arguably more. IRQ0 now takes the
    async path too, rate-limited to the 55 ms BIOS tick.
  ► MEASURED: `qirq.com` reports **c0d=03** -- three interrupts delivered into a guest
    spinning in a pure memory loop that cannot trap -- then exits cleanly. **selftest 8/8
    with the path ON**, PIT case included.
  ► ★★ **SKYROADS' INTRO NOW ANIMATES.** It no longer freezes: the full 30 s runs with
    `io_events` still climbing and the BIOS tick advancing, the host exits cleanly instead of
    being force-killed, and **8+ distinct captured frames** show the ship flying from the
    foreground away down the road (session 10 got 3 frames then a static screen). PNGs in
    build/shots/shot_skyroads_shot*.png.
  ► DIAGNOSTIC WORTH KEEPING: the IVT dump at injection time showed **Skyroads never installs
    an SB ISR at all** (vectors 0x0B-0x0F all still point at unowned BIOS stubs). It was never
    waiting on the Sound Blaster -- it was waiting on the TICK. Injecting IRQ 5 alone just
    vectored it into ROM at F000:A390. Do not assume a game's device IRQ is what it wants.

★★★ SESSION 10'S DIAGNOSIS STANDS -- I briefly concluded otherwise and the instrumentation
refuted me; the corrected story is below. Mid-session I saw Skyroads' 4.5M port-I/O traps
and concluded the exec loop had millions of injection points it was refusing. It does not.
A refusal log at the gate proves `irqn_refused = 0` for the whole run: **we never decline
anything**. The 4.5M traps all happen in the first 6 s, before the transfer exists.

★★★ THE MEASURED SKYROADS TIMELINE (heartbeat, bare metal -- this is the useful artifact):
    ~8.5 s  SB block programmed: len 0x7d64 (32,100 bytes) @ rate 0x1788 (6024 Hz)
    8.5-13s block_left drains at EXACTLY the programmed rate (0x665a -> 0x23c0 in 3 s)
    ~13 s   `io_events` FREEZES at 0x45aba0 and never moves again; guest parked at
            DOS_HDLR_SEG:0x0037 -- the `CD 1C` in our INT 08h stub, i.e. it has entered its
            own INT 1Ch handler and spins there taking no traps at all
    ~14 s   block completes: blocks=1, raised[5]=1, sb_irq=5 -- the IRQ IS raised, ONE
            SECOND AFTER the guest stopped trapping, into an exec loop that can never run
  ⇒ **At the instant that matters there is genuinely no injection point.** Async delivery
    is required, exactly as session 10 said.
  ► THE SILVER LINING IS REAL: DMA -> SB -> mixer is **functionally correct end to end**.
    The block is programmed, drained at the right rate, and completed. Only the completion
    interrupt cannot reach the guest. That is a much smaller remaining gap than it looked.

★★★ WHY WE REFUSE: **VME. The guest's STI sets VIF (bit 19), not IF -- and every gate we
have reads IF.** Virtual Mode Extensions are enabled for our VDM; that is PROVEN, not
assumed: the kernel sets EFLAGS.VIP in our guest's frame, a branch it only takes when
`KeI386VirtualIntExtensions` has V86 VME on. Under VME a V86 guest's CLI/STI never touch
IF. So a game that enables interrupts by EXECUTING STI -- rather than by inheriting IF=1
from the entry EFLAGS (session 10's fix, which is why programs that never STI worked) --
looks permanently interrupt-disabled to us. Gates are now `IF || VIF` (commit `b7bc111`). This is correct on VME hardware and selftest
stays 8/8, but **it is not what unblocks Skyroads** -- see the timeline above.
  ► ALSO TESTED AND REFUTED, do not repeat: an entry trampoline that makes the guest execute
    a real `sti` (`sti; jmp far <entry>` at DOS_HDLR_SEG:0x60, qimode bit 3) so VIF is set
    the only way the CPU accepts. No delivery. And qirq.com already executed its own `sti`
    before spinning, so the earlier runs had refuted this before I built it.

★★ THE ASYNC LEVER, RE'd FROM XP's KERNEL (full write-up + every address in
docs/research/dpmi-under-ntvdmcontrol.md, "Runs 87-93"):
  • **`NtVdmControl(VdmQueueInterrupt=1, ServiceData)` -- ServiceData is a THREAD HANDLE**,
    not a pointer. It queues an APC to that thread, which IS the preemption we lacked.
    Rig-confirmed accepted (`st=0`) and its APC demonstrably runs.
  • The APC's gate `VdmpCanDeliver` (0x56dce0) reads **VIF** on VME hardware, not IF -- the
    same root cause as above. Every run stopped there: it sets VIP and defers.
  • The kernel emulates a **full 8259 in memory we hand it at VdmInitialize**, which we had
    been passing ZEROED -- so it could never dispatch anything. Layout now recovered and
    programmed (master 0x08 / slave 0x70): `v86_ica_raise/eoi/set_mask/state`.
  • `VTIB+0x5A8 = 3` is written by that same APC -- session 10's event-3 "interrupt pending"
    reflect, seen from the kernel side.

★★★ A SESSION-10 LINE WAS WEDGING GUESTS: `*(DWORD*)0x714 |= 1` (VDM_INT_HARDWARE) in
`host_irq_sink`. With VIP set and VIF clear the guest's next IRET faults under VME into a
dispatch that refuses to deliver and re-arms VIP -- the guest froze at `DOS_HDLR_SEG:0x0003`
with the exec loop starved and NO exit path running. Reproduced in every run that set the
bit **including the control that made no queue call at all**. Removed; the same probe then
runs to a clean `INT 21h 4Ch` exit. (It is NOT what freezes Skyroads -- tested directly,
v104 froze identically. Do not conflate the two.)
  ► Also measured and worth not repeating: bit 9 (0x200) of `[0x714]` is the VDM's virtual
    interrupt flag and **the KERNEL already maintains it** (read set from the first
    instruction). Writing it from user mode only clobbers correct state.

★ NEW TEST + TOOLING
  • `tools/dostest/qirq.asm/.com` -- async-IRQ probe: hooks INT 05h and INT 0Dh with separate
    counters (which one fires tells you WHICH kernel path delivered), then waits in a pure
    memory spin that cannot trap, so any vector that fires was delivered asynchronously.
  • `qimode.txt` on the share = a no-rebuild knob (hex digit: bits0-1 = `[0x714]` bits to set,
    bit2 = raise a periodic IRQ 5, bit3 = start the guest with VIF). **Absent = everything off**,
    so normal runs are untouched. Delete it before non-experiment runs.
  • A headless **heartbeat** (always on) logs guest CS:IP/EFLAGS + counters every 500 ms. This
    is what turned "the log just stops" into a timestamped last known position.
  • Fetching files off the box for RE, no physical access:
    `printf 'dpmitest.com&copy C:\\WINDOWS\\system32\\ntoskrnl.exe "<share>\\re_ntoskrnl.exe"\r\n' > cmd.txt`
    (the watcher interpolates cmd.txt into a command line, so `&` chains a command after rt.bat).

▶ DEAD LEADS -- CLOSED BY MEASUREMENT/DISASSEMBLY THIS SESSION, do not re-open:
  • **`VdmQueueInterrupt` as an async lever** (see above -- transition-only, tested on the real
    Skyroads IRQ). The RE of it is still valuable and stays documented; the *use* is dead.
  • **`VdmPMCliControl` (service 13) is PM-ONLY.** ServiceData is a pointer to a subfunction
    dword: {0,1} clear/set bit 0 of the word at VdmObjects+0xBA (the PM client's virtual CLI),
    {2} = the CLI-timeout watchdog that force-sets `[0x714] |= 0x200`, {3,4} dispatch helpers.
    It never touches the V86 VIF. (The previous checkpoint called this the top lead. It isn't.)
  • **`VdmDelayInterrupt` (service 2)** takes a 12-byte struct keyed by IRQ line -- it is the
    ICA's delay/undelay machinery (`pDelayIrq`/`pUndelayIrq`/`pDelayIret`), not an async timer.
  • Setting EFLAGS.VIF via the VTIB CONTEXT (sanitised away), and making the guest execute a
    real `sti` (above). Neither produces a kernel dispatch.

★★★ THE ASYNC LEVER IS CLOSED -- `VdmQueueInterrupt` CANNOT PREEMPT A RUNNING V86 GUEST.
Tested at the only moment that matters: Skyroads with the lever armed on its REAL Sound
Blaster IRQ (qimode `9`, no artificial raiser). `qi_calls=1`, status 0, fired exactly at the
block completion -- and nothing happened: `state714` went to `...31` and STAYED, `io_events`
stayed frozen, `irqn_inj` stayed 0, for the remaining 15 s. Matches the disassembly: the APC's
first pass always requeues itself as a **user-mode** APC (0x46fead; it is entered with
NormalContext = 0 and only a non-zero NormalContext reaches the dispatch), and a user APC is
never delivered to a thread spinning inside VdmStartExecution. **The service is
transition-only.** Do not spend more time on it. Probe: `tools/dostest/qirq2.asm`, which
retargets the kernel's PIC to base 0x60 so a KERNEL-delivered IRQ 5 arrives as INT 65h and a
HOST-delivered one as INT 0Dh -- previously the same vector, hence unattributable.

★★ A REAL BUG FIXED ON THE WAY: `inject_int` pushed a **16-bit** FLAGS word, truncating away
EFLAGS.VIF -- so the guest's IRET restored its virtual interrupt state from a zero bit and came
back with interrupts off PERMANENTLY. Measured on qirq2: after the first injected INT 08h,
`irq0_inj` stuck at 1 for the entire run. The CPU folds VIF into the pushed IF itself when it
vectors a hardware interrupt; we synthesise the frame, so we must too (commit `8d42d15`).
This did NOT unblock Skyroads (its timer was already flowing, 483 ticks), but it was silently
disabling interrupts for any guest whose enable lives in VIF.

▶▶ RESUME — NEXT STEPS (in order):
  1. **SOAK THE ASYNC PATH, THEN MAKE IT THE DEFAULT** (drop the qimode bit 4 gate). Run the
     whole tests/ battery with it on -- especially the graphical demos, the DPMI/PM tests (it
     must never fire while the guest is in PM: the VM check covers that, verify it does), and a
     long Skyroads run. Watch `async_bail`: a high bail count means the guard rails are refusing
     more than they should. The one real risk is a torn context if a guard is wrong, so look for
     any run whose guest wanders to an unexpected CS:IP.
  2. **Skyroads is playable -- now judge it against the bar.** Remaining user-reported
     roughness after v126 (all UNVERIFIED by me; they need ears/hands on the box): is the
     music now smooth and correct tempo, is the PCM still pitched low (if so that is a
     separate SB rate bug -- suspect the mixer's resample ratio, NOT the timer), and is input
     latency acceptable. Then: a game that DOES install an SB ISR, to exercise the device-IRQ
     half of async delivery end to end (Skyroads never installs one).
  3. (was 2) Drive Skyroads further -- keyboard input through the
     menu, as run 86 did. Then the sound epic's real acceptance test: a game that DOES install an
     SB ISR (Skyroads does not -- see above), so the device-IRQ half of async delivery gets
     exercised end to end.
  4. PIC VDD claiming 0x20/0x21 is a PREREQUISITE for the kernel ICA path, not a nicety: the ICA's
     ISR bit stays set until an EOI clears it (`v86_ica_eoi`), or that line never fires again.
     Skyroads already writes EOI to 0x20 and it goes nowhere (still in `unclaimed ports`).
  5. Calibrate the OPL envelope rates (`OPL_EG_ANCHOR`); decide on un-folding SB stereo.
  6. `git push` -- 33 commits are sitting local.

▶ HARNESS GOTCHA (cost me a wrong conclusion this session): **rt.bat copies the log while the
  host may still be finishing, and SMB caches the result.** A `result_*.log` read too early is a
  PARTIAL file -- I read one that was missing its last lines and concluded a counter was absent.
  After `cmd.txt` disappears, wait ~25-40 s, `ls` the share to force a readdir, and sanity-check
  the version string and a known-final line (`STAGE2: complete` or the HEADLESS report).

▶ OPEN QUESTION (unexplained, low priority): in the runs that set the hardware-pending bit, the
  process ended at ~8 s having reached NO exit path -- not the guest's 4Ch flush, not the 30 s
  deadline backstop's report. The heartbeat proves it was alive until then. Kernel-side VDM
  termination is the obvious suspect. Harmless now that the bit is gone.

═══════════════════════════════════════════════════════════════════════════════
██ CHECKPOINT — 2026-08-19 (session 10). SUPERSEDED by session 11 above. ██
═══════════════════════════════════════════════════════════════════════════════

▶ RESTART POINT (2026-08-19, IDE restart): HEAD = `a44eae8`; branch spike/dpmi-16bit-switch;
  **22 commits UNPUSHED**. Host = **dpmi-harness-v97**, built clean + deployed to the share `bm/`.
  Working tree clean except (a) the pre-existing untracked files that are NOT mine (MAINICON.ico,
  demos/, scripts/kd_*.py, scripts/trace_break.py) and (b) the freshly-compiled native test binaries
  (tools/dostest/{dma,opl,opl_synth,sb,mpu,audio}_test) which are deliberately untracked -- repo
  convention is that `.com` guest binaries ARE tracked (can't assemble on the target) and native
  test binaries are NOT. Rig healthy: watcher + controld both beating, nothing wedged.
  THIS SESSION: the sound epic (#20/#21) got its FIVE devices + mixer built and tested (520 off-VM
  checks), and -- the bigger result -- a one-line host bug that had been freezing the guest's clock
  since forever was found and fixed. Skyroads now detects the Sound Blaster and plays a DMA block.
  ONE structural gap remains before it reaches gameplay; see THE BLOCKER below.

★★★ THE HEADLINE FIX: **V86 guests were started with IF=0**. `VTIB_EFLAGS_V86` was `0x20002` --
no interrupt flag. DOS enters a program with interrupts already enabled, so DOS programs never
issue STI; the guest therefore ran with interrupts disabled for its ENTIRE life. The host's IRQ0
delivery gate never opened, INT 08h was never injected, and the BIOS tick at 0040:006C never
advanced. Anything paced on the tick (Skyroads' sound/PIT init, FM music, PCM block timing) hung.
MEASURED, not inferred: iobench.com saw **34M port-I/O events with irq0_inj=0 and bda_tick frozen**,
while `[0x714]` sat with VDM_INT_TIMER permanently pending. Fix = `0x20202` (commit `b0d92e6`).
  ► Note `VTIB_EFLAGS_PM` ALWAYS had IF set (0x202) -- which is exactly why every protected-mode
    timer result (run 78, pm32irq) worked while real mode "crawled ~100x too slow". That asymmetry
    was the clue sitting in plain sight in ntvdm.h.

★★ SETTING IF EXPOSED TWO KERNEL BEHAVIOURS WE HAD NOT BEEN USING (both now implemented):
  • **event 3 is OVERLOADED.** Besides the I/O reflect it is how the kernel says "a hardware
    interrupt is pending and the VDM can take it" -- the interrupt assist we thought we lacked.
    It ONLY fires with IF=1, which is why session 9 never saw it. Distinguished from a genuine GP
    fault by the kernel's own pending bits in `[0x714]&3`; we clear them, latch IRQ0, resume at the
    SAME EIP (nothing faulted, so nothing must be stepped over).
  • **event 1 = kernel-decoded STRING I/O** (REP INS/OUTS), previously an unhandled hard stop.
    The kernel leaves a decoded descriptor at VTIB+0x5A8 -- observed for `rep outsb` to 0x3C9:
    `{1, 2, port|size<<16 (0x000103C9), 1, count (0x40), seg:off (0x01000355)}`. We deliberately
    service it from the guest's own SI/DI/CX/DF instead, depending only on fields whose meaning is
    established; the two agreed on the run that found it.

★ THROUGHPUT: THE SESSION-9 PREMISE WAS WRONG. Port I/O was NEVER the bottleneck. iobench.com on
bare metal (tools/dostest/iobench.asm, results in result_iobench.com.log):
    single `IN AL,DX`        1.60 us   |  `IN`+`LOOP` burst   0.082 us  (19.5x faster)
    single `OUT DX,AL`       1.30 us   |  `REP OUTSB` (ev 1)  0.072 us  (21x, was UNSUPPORTED)
~1.5us per trapped access is roughly real-ISA speed. The earlier "~40us" figure came from
misreading a v80 debug counter. The burst fast path is still worth having (an AdLib register write
spends 43 trapped accesses, 35 of them a settling delay) but it is an optimisation, not the fix.

★ THE RIG CAN NO LONGER WEDGE. Clearing g_running only stops a loop that gets a turn, and a guest
spinning in pure V86 code never returns from v86_run -- that wedged rt.bat's `start /wait`
permanently and needed a manual `controld kill` every time. The headless backstop now waits a 3s
grace then FORCES process exit, and on the way out logs: the frozen CS:IP + 12 bytes there, every
counter, and the list of unclaimed ports the guest probed. That report is how this session
diagnosed everything. (It also caught a 256-byte buffer overflow in my own reporting code that was
silently killing the thread before it could log -- buffer is 512 and flushes in sections now.)

★★ SOUND EPIC (#20/#21): ALL FIVE DEVICES + MIXER BUILT AND TESTED. **520 off-VM checks, 0 fails**
(`./tools/dostest/run.sh`). Commits `7aaeab6` (devices) and `a44eae8` (mixer + sink).
  src/vdd/vdd_dma.c        8237 pair: page wiring, byte-pointer flip-flop, 16-bit WORD addressing,
                           terminal count, auto-init ring wrap.                        41 checks
  src/vdd/vdd_opl.c        OPL2 register file + REAL timers (80us/320us). Replaces the detect stub
                           that had been bolted onto the video VDD (it toggled status bits on every
                           read -- told games a card existed then stranded them).       35 checks
  src/vdd/vdd_opl_synth.c  OPL2 FM, written from documented YM3812 behaviour, NOT ported, so it is
                           ours and MIT-clean. Log-domain like the chip: an operator is
                           exp2(-(logsin+env+TL+KSL)) -- adds and shifts, no multiplies or floats.
                           Tables generated by tools/gen-opl-tables.py (host is -nostdlib: no libm).
                           PITCH MEASURES 392.0 Hz vs the chip's published 388.4 Hz formula. 10 checks
  src/vdd/vdd_sb.c         SB16: DSP reset handshake, version/identify, mixer, rate via time
                           constant or 0x41, single-cycle + auto-init DMA, completion IRQ + acks,
                           FM mirror at 2x0/2x8.                                        36 checks
  src/vdd/vdd_mpu.c        MPU-401 UART mode -> host midiOut (XP has a GS Wavetable synth, so
                           forwarding beats an FM approximation of GM). Running status, realtime
                           bytes interleaved mid-message, sysex.                        21 checks
  src/vdd/vdd_audio.c      Mixer: resamples OPL (native 49716 Hz) + SB (game's rate) onto 44100 and
                           sums, honouring the SB16 mixer's own master/voice/FM volume registers
                           (that is what gives real headroom).                          16 checks
  src/vdd/audio_wave.c     waveOut + midiOut, bound at RUNTIME like present_ddraw binds ddraw, so
                           the import list is unchanged.
  ► DESIGN CAVEATS, both flagged in-source: OPL envelope RATES are empirically anchored
    (`OPL_EG_ANCHOR`) and are within ~2x of real silicon at the extremes -- pitch is exact and note
    shapes are right, so it reads as a different feel, not wrong notes. Calibrate against a
    reference recording. SB stereo is folded to mono. Neither blocks a game.
  ► THE MIXER IS ALSO THE TRANSPORT, not just the audible path: `vdd_sb_render()` is what walks the
    DMA buffer and raises the completion IRQ. audio_wave keeps pumping even with NO sound device
    (discarding samples), or every SB game would hang on a silent machine.

★★ SKYROADS STATUS -- the sound stack demonstrably WORKS; one gap stops gameplay.
  Then (session 9): black screen, stuck in the OPL register-write helper, never reached video.
  Now (v97, bare metal, `printf 'skyroads\r\n' > cmd.txt`):
    • Renders its INTRO ROAD SCENE in mode 13h (12 shots, 3 distinct frames).
      PNG kept at build/shots/skyroads_v88_last.png -- checkerboard road + nebula sky.
    • **Finds the Sound Blaster at 0x220**: its probe sweep collapsed from 19 unclaimed ports
      (0x210-0x260, finding nothing) to 4 -- `0x216 0x21A 0x21E` (probes 0x210, then STOPS) + `0x20`
      (the PIC, which nothing claims -- see resume item 4).
    • Programs the DSP and PLAYS A BLOCK: `sb_dspwr=6 sb_rate=0x1788 (6024 Hz) sb_blocks=1`.
    • Audio is genuinely running: `silent=0` (waveOut OPENED) and `mixed=0x144E00` = 1,330,176
      frames = **30.2 s of audio at 44100 Hz for a 30 s run**, i.e. real time, no underruns.
    • Timer is healthy: `irq0_inj=483`, tick advancing.
    • **`irqn_inj=0`** <-- THE GAP. The completion IRQ is RAISED and never DELIVERED.

★★★ THE BLOCKER (pick this up first): **we cannot asynchronously interrupt a V86 guest.**
The guest is spinning inside its own INT 1Ch handler waiting for the SB IRQ (frozen snapshot is
CS:IP=0x0050:0x0037 = our INT 08h stub at its `CD 1C` chain, bytes `cd 1c cf ...`), and our exec
loop only regains control when the guest TRAPS. So the injection point never arrives. This is the
same root limitation behind two earlier symptoms this session (the rig wedge; and, before the IF
fix, the frozen tick). Real ntvdm does not have it because the KERNEL owns interrupt delivery via
the ICA; we run the guest in-process and are blind until it faults.
  ► **ALREADY TRIED AND FAILED -- do not repeat:** setting the FIXED_NTVDMSTATE hardware-interrupt-
    pending bit (`*(DWORD*)0x714 |= 1`) from the audio thread does NOT preempt a running guest.
    v97 was built and run with exactly that: `intpend` stayed at its startup value of 1 and
    `irqn_inj` stayed 0. The kernel evidently only consults that word at its own transition points.
    (The mechanism DOES exist though: the kernel preempted us with event 3 at startup when IT had a
    timer pending -- so the lever is on the kernel side, not in that user-mode word.)
  ► Device IRQs 2-7 ARE now wired end-to-end otherwise: `host_irq_sink` previously handled only
    IRQ 0 and 1 and silently DROPPED the SB's IRQ 5; they now latch in `g_irqn_pending[]` and
    inject as INT (8+irq) with the same IF gating. The stub re-entrancy guard was also relaxed to
    just the BOP itself (a real PC nests a device IRQ inside a timer handler whenever IF is set).
    So the moment we can preempt, delivery should just work.

▶▶ RESUME — NEXT STEPS (in order):
  1. **RE the kernel's interrupt-queue interface** (the agreed direction). Find the NtVdmControl
     service real ntvdm uses to queue a hardware interrupt into a running VDM -- i.e. what makes the
     kernel break out of V86 and hand us an event. Landmarks are already mapped (see the session-3/
     5/7/8 RE notes below, docs/research/dpmi-under-ntvdmcontrol.md, /tmp/ntvdmex-re/ntoskrnl.exe
     with symbols, r2 base 0x400000, KernBase 0x804d7000). Confirm with the Skyroads run: success =
     `irqn_inj` becomes non-zero and the game leaves its INT 1Ch handler.
     ALTERNATIVES if that proves expensive: (a) force periodic guest yields so an injection point
     always exists (cheaper, less faithful, costs overhead on every run); (b) park Skyroads and take
     DOOM instead -- its setup can select no-sound and the 32-bit extender path is already proven,
     so it exercises a different axis while this waits.
  2. Once IRQs land: re-run Skyroads expecting the intro to ANIMATE (frames currently go static
     after 3), then drive it to the cockpit; then Doom with sound.
  3. Calibrate the OPL envelope rates against a reference recording (`OPL_EG_ANCHOR`), and decide
     whether SB stereo needs un-folding.
  4. Claim ports 0x20/0x21 with a PIC VDD. Skyroads writes EOI to 0x20 and it currently goes
     nowhere; harmless today but it means we ignore IRQ masking, which will matter for games that
     mask/unmask around critical sections.
  5. `git push` -- 22 commits are sitting local.

▶ HARNESS GOTCHAS LEARNED THIS SESSION (all cost real time):
  • **SMB ATTRIBUTE CACHING HIDES RESULT FILES.** `ls result_x.log` can say "No such file" while the
    file exists. Force a readdir first: `ls /tmp/xpshare/ >/dev/null` then stat/tail. Several
    "TIMEOUT" conclusions this session were this, not a failed run.
  • **`build.sh 2>&1 | grep -E "error|..." && cp ...` FIRES ON FAILURE** -- grep exits 0 when it
    MATCHES the error text, so the `&&` chain happily deploys a stale binary. Cost a confusing run
    against the wrong host. Check the version string in the log (`grep -o 'dpmi-harness-v[0-9]*'`)
    whenever a result looks like it ignored your change.
  • The BOX CLOCK RUNS ~30s AHEAD of the Mac. When comparing watcher.txt/controld.txt heartbeats to
    `date`, a "stale" watcher may be perfectly fine. controld fresher than watcher = watcher is busy
    inside `start /wait` (i.e. a test is running), which is normal, not a wedge.
  • **NEVER poll the BIOS tick by reading 0040:006C directly from a probe.** The host injects INT 08h
    from a loop that only runs when the guest traps, so a pure memory spin on the tick never advances
    it, never returns, and cannot even be stopped by the headless deadline. It wedged the rig once.
    Poll with INT 1Ah instead (a BOP -> the host gets a turn). See the note atop iobench.asm.
  • Test conventions: `.com` guest binaries are TRACKED, native `*_test` binaries are NOT.

═══════════════════════════════════════════════════════════════════════════════
██ CHECKPOINT — 2026-08-19 (session 9). SUPERSEDED by session 10 above. ██
═══════════════════════════════════════════════════════════════════════════════

▶ RESTART POINT (2026-08-19, IDE restart): HEAD = 2fb8d27 + doc commit; host = dpmi-harness-v81
  (staged on the share bm/); branch spike/dpmi-16bit-switch; 18 commits UNPUSHED. Working tree clean
  except the pre-existing untracked files (MAINICON.ico, demos/, scripts/kd_*.py, etc. -- NONE mine).
  Box = healthy resting state (watcher looping, controld v2 running). REMOTE CONTROL FULLY WORKS:
  `echo reboot > /tmp/xpshare/control.txt` reboots; `echo kill > .../control.txt` unwedges a hung host;
  `printf 'capture\r\n' > /tmp/xpshare/capture.flag` enables self-screenshots; fire tests via
  `printf '<name>\r\n' > /tmp/xpshare/cmd.txt` then read result_<name>.log + shot_<name>_*.bmp.
  Remount after a reboot: `mount_smbfs -N //guest@192.168.1.34/ntvdmex /tmp/xpshare` (LAN ops need
  dangerouslyDisableSandbox). THIS SESSION'S ARC: cracked the bare-metal PM wall (v68), proved 32-bit
  async IRQ visually (pm32irq, remote self-screenshots), built the remote-test rig (controld daemon +
  game-aware harness + self-screenshots), and brought up Skyroads -- which LOADS + RUNS but is blocked
  in sound/PIT-timing init (see the "SKYROADS BRING-UP" section below). NEXT = user's call: Doom (DOS/4GW,
  setup can disable sound) vs the sound epic vs another game. The retro I/O reflect fix (v74/v81) is the
  key durable bug fix this session -- real-mode direct port I/O now services on this hardware.

★★★ THE BARE-METAL PM WALL IS CRACKED (16-bit AND 32-bit DPMI real-CPU PM run on real
silicon). Session 8's "the kernel declines to run our PM VDM" diagnosis was WRONG: the
event-3-at-entry was NOT a kernel PM-init gap -- it was OUR OWN monitor's interrupt-pending
guard in src/vdm/dpmi_enter.S (label 2, lines 45-51 & 122): when the client enters PM with
IF=1 (VTIB_EFLAGS_PM=0x202) AND ds:[0x714]&3 signals a pending hardware interrupt, dpmi_enter
reports VTIB_EVENT=3 and DOES NOT enter the guest (EIP stuck at entry -> the exact session-8
symptom: event 3 at `mov ax,0x400`, EIP unadvanced). We run PM IN-PROCESS (far-jmp), NOT via
VdmStartExecution, so while PM executes the kernel is not managing this VDM's interrupt assist
-- [0x714]'s pending bits are STALE real-mode state (a timer IRQ0 latched during the pre-switch
DOS INT 21h calls). On real 3.3GHz silicon a tick is essentially ALWAYS pending at switch time
(QEMU+HVF's dilated clock rarely had one) -> the monitor bailed with event 3 forever.

FIX (committed `c0831b1`, host v68): in the main PM loop, on ev==3 clear the stale [0x714]&3
pending bits and re-enter (bounded to 0x10000 so a genuinely re-arming pending can't spin).
The guard fires EXACTLY ONCE per run then PM executes free -- confirming the bits are stale,
not re-arming (as predicted).

BARE-METAL-CONFIRMED via the SMB loop (real-CPU path, g_dpmi_use_interp=0), each ran the
event-3 guard once then ran to a clean INT 21h 4Ch exit:
  • dpmitest.com  -- FULL 16-bit DPMI surface: 0400/0100/0205/0204/0900/0901/0300/0301/0303
      (real-mode callback with NESTED INT 31h+21h inside it). The headline.
  • outprobe.com  -- 16-bit PM `OUT` to VGA DAC survives + resumes.
  • pm32io.com    -- 32-bit (D/B=1) PM `OUT` reflects + services.
  • pm32sw.com    -- the 32-bit MODE SWITCH itself yields a working 32-bit CS; OUT serviced.
  • pm32flat.com  -- base-0 ~2GB G=1 FLAT selector (the DOS/4GW model) renders via ES:[0A0000h],
      775 svc calls, clean exit. The 32-bit flat model EXECUTES on real HW.

COMMITS THIS SESSION (branch `spike/dpmi-16bit-switch`, all local -- PUSH when ready):
  • `242f884` v67 -- route real-HW I/O reflect (event 3) through host_try_io at 4 sites +
      AUTOEXIT_PATH headless-exit + PM-fault byte-dump diagnostic + filebuf 512KB (session-8 fixes).
  • `777fd40` -- session-8 checkpoint doc.
  • `c0831b1` v68 -- THE CRACK (clear stale event-3 pending guard).
  • `d438552` -- session-9 checkpoint doc.
  • `afafbcb` v69 -- headless robustness (log cap + PM-loop time cap + byte-dump rate-limit). ← HEAD.
  Build clean: `./scripts/build.sh` -> build/ntvdmhost.exe (v69, KERNEL32-only). Staged to bm/.

RIG: RECOVERED + GREEN (selftest 8/8). v69 deployed and RE-CONFIRMED on real HW: dpmitest runs
the full 16-bit DPMI surface + clean 4Ch exit, log is ~3 KB (cap works, no regression).

★ REMOTE-TEST UPGRADE (session 9, box is "not easily accessible" -> minimise human touch):
  • HOST SELF-SCREENSHOT (v70, commit fee5702): present_ddraw_save_bmp() dumps the 8bpp back-buffer
    (occlusion-proof) to C:\ntvdmex\shotNN.bmp every ~2s in headless mode (UI-thread timer, capped 40);
    rt.bat ships them to the share as `shot_<test>_shotNN.bmp`. So GRAPHICAL runs (Skyroads, pm32gfx,
    mode13, pm32irq box-march) are now verifiable REMOTELY by reading/diffing BMPs -- VNC/monitor no
    longer needed. Palette byte-order matches gdi_present (0xAARRGGBB -> RGBQUAD). NOT yet end-to-end
    tested (needs the watcher up).
  • runwatch.bat now SELF-INSTALLS to `%ALLUSERSPROFILE%\Start Menu\Programs\Startup\ntvdmex-watch.bat`
    and refreshes watcher.txt every loop (real heartbeat). After ONE bootstrap double-click, every
    reboot AUTO-STARTS the watcher (box auto-logs in) -> no more per-reboot human action. Combined with
    v69's headless caps (infinite/wedged runs self-exit in 30s), the rig is now largely hands-off.
  • Can't bootstrap autostart purely over SMB: guest can mount only ntvdmex + SharedDocs (=All Users\
    Documents); C$/ADMIN$ are DENIED (simple file sharing), and Startup is outside those shares. So the
    ONE remaining human action is a single double-click of bm\runwatch.bat; it's self-perpetuating after.
  • Shares (smbutil view, guest): ntvdmex, SharedDocs (rw), C$/ADMIN$ (denied), IPC$. Ports: 445/139
    open, 135 closed -> NO remote-exec / remote-shutdown RPC reachable.
  BM SCRIPTS now snapshotted in scripts/bm/ (rt.bat, runwatch.bat, controld.c).

★ v70 WEDGE + v71 BULLETPROOFING + controld (the recovery story):
  • v70 added host self-screenshot; a real-mode run (selftest) then HUNG under v70, and the REAL-MODE
    exec loop had NO headless cap (only the PM loop got one in v69) -> it wedged rt.bat's start/wait
    permanently. VNC is fully dead now (capture returns NO file after a DOS mode switch) + no remote-exec,
    so it could not be unwedged remotely -> the box needs a power-cycle. LESSON: any exec path without a
    headless cap can permanently wedge the rig.
  • v71 (commit bf43e33) BULLETPROOFS it: (1) the real-mode V86 loop now honours PM_HEADLESS_MS=30s too
    (a hung real-mode run self-exits -> would have auto-recovered the v70 wedge in 30s); (2) self-capture
    is now OPT-IN via CAPTURE_FLAG (a file on the share) so non-graphical tests never touch the capture
    path. With real-mode + PM caps, NO run can permanently wedge the rig again.
  • controld.exe (commit 2c8a0e8, scripts/bm/controld.c, build-controld.sh): a tiny XP-safe (no-CRT,
    KERNEL32/USER32/ADVAPI32 only) CONTROL DAEMON, separate from the test watcher, that NEVER runs guest
    code so it can't wedge. It polls `control.txt` on the share: `reboot`->ExitWindowsEx force reboot,
    `poweroff`, `kill`->taskkill ntvdmhost (unwedge a hung host). Writes `controld.txt` heartbeat; singleton.
    rt.bat + runwatch.bat both `start` it (singleton) and rt.bat refreshes the Startup runwatch each run.
  • BOOTSTRAP DONE (2026-08-19): box power-cycled, watcher auto-started from Startup, controld
    bootstrapped via rt.bat. PROVEN: controld receives commands (control.txt) + writes controld.txt
    heartbeat; **`kill` REMOTELY RECOVERED a wedged watcher** (taskkilled a hung ntvdmhost -> start/wait
    returned -> watcher resumed). NOT yet working: **`reboot`** -- v1's ExitWindowsEx reached the handler
    but never rebooted (box stayed up, returned to idle) = a privilege/API detail.
  • controld v2 (commit 6081b81, STAGED as bm/controld_v2.exe): reboot via `shutdown.exe -r -f` primary +
    ExitWindowsEx fallback that REPORTS GetLastError to the heartbeat; new `quit` command. Can't hot-swap
    while v1 runs (singleton mutex + .exe file-locked on share, v1 has no quit). runwatch.bat now
    SELF-UPGRADES controld on start (taskkill controld -> copy controld_v2.exe -> relaunch).
    ▶ NEXT SESSION (one small action): restart the watcher (or Start->reboot) ONCE -> v2 loads ->
      `echo reboot > /tmp/xpshare/control.txt` should reboot via shutdown.exe (heartbeat shows the error
      code if not). Then remote reboot works with no physical access ever again.
  • ALSO OPEN: v71 `selftest.com` HANGS the watcher (real-mode run; the 30s real-mode cap did NOT fire ->
    likely hangs INSIDE v86_run/VdmStartExecution so the loop-top cap never runs). controld `kill`
    recovers it. This hang appeared v70+ (v68 selftest passed) but is NOT the BMP capture (v71 capture is
    opt-in/off). INVESTIGATE next session: what in v70/v71 makes a real-mode guest wander off. Meanwhile
    dpmitest (PM path) is the safe smoke-test. HEAD = 6081b81 (12 unpushed).

pm32irq REFRAMED (it was NOT a code bug). `pm32irq.com` is an INFINITE mode-13h animation demo
(`jmp .frame`; never calls INT 21h 4Ch) -- like animate/bounce/mode13/pm32gfx. Under the headless
SMB auto-exit harness it ran the PM loop forever (wedged rt.bat's `start /wait` -> wedged the
watcher) and logged each INT 31h 0400 yield every iteration -> a 148 MB log FLOOD. The 32-bit
async-IRQ frame code it exercises ALREADY WORKS (dpmi_inject_pm_irq widens the IRET frame to dword
for 32-bit handlers, run 83; see main.c ~line 2034). v69 hardens the host so no runaway can harm
the rig again: 4 MB log cap (log.h LOG_MAX_BYTES), a 30 s headless PM-loop wall-clock cap
(g_headless + PM_HEADLESS_MS -> self-exits so the watcher survives), and byte-dump rate-limit (32).
  ► RECOVERY (if ever wedged again): NO remote-exec (only SMB 445/139; 135 closed, guest-only, no
    net/rpcclient/impacket -> a remote `shutdown` RPC is NOT reachable). Blind VNC input works
    (capture dead): Win+R -> `taskkill /f /im ntvdmhost.exe` -> Enter can clear a hung host, but
    it's a gamble without a screen (an earlier stray Alt+F4 likely closed the watcher window). With
    v69's caps a wedge shouldn't recur; worst case, kill ntvdmhost / restart runwatch.bat on the box.

INFINITE VISUAL DEMOS ARE MONITOR-ONLY: pm32irq, pm32gfx, animate, bounce, mode13, blitfast, and
the vga/vesa demos loop forever and must be watched on the box's PHYSICAL display, NOT the headless
SMB loop (which auto-times-out at 30 s now, so they no longer wedge -- but you still won't SEE them).

★★★ 2026-08-19: pm32irq VISUALLY CONFIRMED on BARE METAL via the host self-screenshots (fully remote,
  no monitor). Enabled capture.flag, fired pm32irq.com; host wrote 11 shotNN.bmp (320x200 mode13),
  rt.bat copied them, I read+analysed them on the Mac: the red 24px box MARCHES x=20->65->116->168->219
  (wrap at 240) -> 30->81->... driven ONLY by [hits] which is bumped ONLY by the hooked 32-bit PM INT 08h
  ISR. ⇒ **async IRQ0 injection into a 32-bit PM timer hook WORKS on the real CPU** (the widened dword
  IRET frame, run 83, is correct) -- the checkpoint's "last 32-bit gap" is CLOSED. The earlier pm32irq
  "hang" was purely the infinite-demo-under-headless-harness issue (fixed by v71's 30s cap), NOT a broken
  async path. Self-screenshot pipeline PROVEN end-to-end. PNGs: build/shots/pm32irq_*.png.

★★ 2026-08-19 SKYROADS BRING-UP (real-mode game via the game-aware harness). Skyroads LOADS + RUNS
  on the host (game rt.bat: folder->C:\game, target SKYROADS.EXE, capture). Surfaced + fixed 3 real host
  gaps (commit 2fb8d27, host v81): (1) ★ RETRO I/O REFLECT -- on this box the IOPL-0 IN/OUT #GP reflects
  as event 3 with CS:IP pointing AFTER the faulting insn (EIP already advanced), so host_try_io (decodes
  AT CS:IP) declined; new host_try_io_retro decodes the IN/OUT ENDING at CS:IP + services w/o advancing
  EIP (fixes real-mode port I/O generally; Skyroads' IN AL,DX vblank poll now flows); (2) OPL2 detect stub
  at 388/389 (status bits toggle so detection loops complete; no FM synth); (3) wall-clock PIT pump in the
  real-mode loop (a heavy I/O-trap loop starves the UI thread that raises IRQ0). BLOCKED: Skyroads hooks
  INT 08h and paces its init on the PIT rate IT programs -- needs real timer/OPL emulation (the SOUND EPIC
  #20/#21), not a quick fix. So Skyroads is sound/timing-init-bound. Remote control ALL WORKING now:
  reboot via `echo reboot > control.txt` (controld v2 shutdown.exe) or the watcher-injection
  `q&shutdown -r -f -t 00` > cmd.txt; `kill` unwedges; self-screenshots via capture.flag.
  OPTIONS from here: (a) SOUND EPIC (OPL2 timer + PIT-rate emulation -- unblocks Skyroads + many games);
  (b) a LESS sound-coupled real-mode game; (c) DOOM (DOS/4GW 32-bit -- the path we cracked; Doom's setup
  can select no-sound, so it may reach gameplay without the sound epic). HOST NOW v81, staged.

▶▶ RESUME — NEXT STEPS (in order):
  1. DONE (2026-08-19): pm32irq 32-bit async IRQ visually confirmed remotely. Next visuals to grab the
     same way (set capture.flag, fire, read shot_*.bmp): pm32gfx (32-bit gradient), mode13, and Skyroads
     (real-mode game -- but MIND the v71 real-mode selftest hang below; a real-mode graphical run may hang
     -> use controld `kill` to recover).
  2. A REAL DOS/4GW extender, then DOOM (the acceptance test) -- the 32-bit flat model executes on bare
     metal (pm32flat) AND 32-bit async timer hooks work (pm32irq), so both prerequisites Doom needs are in.
  3. Broader INT 31h surface as a real extender demands it (0305/0306 raw switch, page-lock, phys-map).
  OPEN BUG: v71 selftest.com HANGS the watcher (real-mode; 30s cap didn't fire -> likely hangs INSIDE
  v86_run so the loop-top check never runs). controld `kill` recovers it. Investigate before trusting
  real-mode graphical runs (Skyroads). PM-path tests (dpmitest, pm32*) are safe + self-cap at 30s.
  (Everything above the session-8 banner is now the authoritative state; session-8's "kernel
   won't run PM" conclusion is SUPERSEDED -- it was our own guard.)

═══════════════════════════════════════════════════════════════════════════════
██ CHECKPOINT — 2026-08-18 (session 8). SUPERSEDED by session 9 above. ██
═══════════════════════════════════════════════════════════════════════════════

BIG PICTURE: this session (1) landed runs 83-86 (committed+pushed), then (2) PIVOTED
TO **BARE-METAL TESTING on a REAL Windows XP box** because QEMU+HVF SIGABRTs on DOS/4GW's
paged 32-bit PM (`mmu_gva_to_gpa`, see [[hvf-paged-pm-blocker]]). On real silicon: **real
mode fully works (selftest 8/8)**; **protected mode hit a NEW, OBSERVABLE frontier** (kernel
returns event 3 the instant we run PM). The HVF VM is effectively RETIRED for this work.

REPO STATE (branch `spike/dpmi-16bit-switch`):
  • COMMITTED + pushed: `af5246a` (run 86 raw keyboard + runs 83-85), `5928ff6` (E0 keys +
    per-byte IRQ1 counter + qmp key-hold). HEAD = `5928ff6`.
  • **UNCOMMITTED (the bare-metal fixes, v65→v67):** `src/host/main.c` + `src/vdm/ntvdm.h`.
    These are GOOD and bare-metal-confirmed for real mode — COMMIT THEM FIRST on resume:
      - ntvdm.h: `VDM_EVENT_IO_HW 3` + taxonomy note (real HW reflects I/O as event 3; HVF used 0).
      - main.c: route event 3 through host_try_io at 4 sites (grep VDM_EVENT_IO_HW); `AUTOEXIT_PATH`
        marker + auto-exit-on-guest-exit (test mode, line ~2849); `filebuf` 128KB→512KB;
        the "GH#18 PM-FAULT ev=.. bytes=.." byte-dump diagnostic in the PM loop (~line 2804);
        version string now `dpmi-harness-v67`.
  • Host builds clean: `./scripts/build.sh` → build/ntvdmhost.exe (v67). KERNEL32-only.
  • New scripts (committed? NO — untracked): scripts/build-game-iso.sh, build-doom-iso.sh;
    games/ is gitignored. tools/dostest/pm32irq.* pm32flat.* were committed in af5246a.

▶▶ THE BARE-METAL TEST RIG — how I drive tests MYSELF (no user clicks). ══════════════════
  BOX: Dell OptiPlex 760, Core2 E8600 3.33GHz, Quadro FX 1800, 4GB, SB X-Fi, XP Pro SP3.
       IP = **192.168.1.34**. On the same LAN as this Mac. (Network ops need
       `dangerouslyDisableSandbox` — LAN IP isn't in the sandbox allowlist.)
  SMB SHARE (THE reliable control channel): `//192.168.1.34/ntvdmex` (GUEST access, no pw).
       Mount: `mkdir -p /tmp/xpshare; mount_smbfs -N //guest@192.168.1.34/ntvdmex /tmp/xpshare`
       Maps to `C:\Documents and Settings\All Users\Documents\ntvdmex` on the box. Read+write.
  VNC (UltraVNC :5900, pw `NTVDMEX`, mirror driver installed): **INPUT works, CAPTURE is
       black + WEDGES after any DOS full-screen mode switch** — UNRELIABLE, do NOT depend on
       it. vncdo venv: `/tmp/vncenv/bin/vncdo -s 192.168.1.34::5900 -p NTVDMEX capture x.png`.
       (Reboot un-wedges it; capture still black on this Quadro even with mirror driver + accel
       off + 16bpp — treat VNC as dead for viewing; use SMB + the box's physical monitor.)
  THE AUTONOMOUS SMB TEST LOOP (robust, VNC-free):
    1. A WATCHER must be RUNNING on the box: `bm/runwatch.bat` (user double-clicks once in the
       console; it loops, drops `watcher.txt` heartbeat). If absent on resume, ask user to run it.
    2. Fire a test:  `printf 'selftest.com\r\n' > /tmp/xpshare/cmd.txt`
    3. Watcher (~2s poll) runs `rt.bat selftest.com`, which: pulls fresh host from bm/, sets IFEO
       Debugger→C:\ntvdmex\ntvdmhost.exe, writes target.txt, drops the autoexit marker, runs
       dosstub (→ntvdm→our host; host AUTO-EXITS when the DOS prog exits), then copies
       C:\ntvdmex\ntvdmhost.log → `result_selftest.com.log` on the share.
    4. Read `/tmp/xpshare/result_selftest.com.log`. The host log includes the guest's console
       output ("==> DOS OUTPUT: [...]") so PASS/FAIL tables are readable.
  DEPLOY A NEW HOST: build → `cp build/ntvdmhost.exe /tmp/xpshare/bm/ntvdmhost.exe`. rt.bat
       auto-pulls it every run — no separate stage step. (Bump the version string to confirm.)
  SHARE LAYOUT: `bm/` = {ntvdmhost.exe(v67), dosstub.com, rt.bat, stageall.bat, runwatch.bat,
       tests/ (32 binaries: selftest, memtest, xms/ems/timer/mouse/key, io*, mode/vga/vesa,
       dpmitest, pmfault, pm32*, i310102, dpmiback, ...)}. Root: result_*.log, watcher.txt, cmd.txt.
  ONLY TEST NTVDMEX (per user): do NOT run stock ntvdm — XP's ntvdm is known-good, and a stock
       full-screen DOS run is what wedges VNC. NTVDMEX is windowed.

FINDINGS THIS SESSION (bare metal): ════════════════════════════════════════════════════════
  • **selftest = 8/8 PASS on real hardware** (DOS mem, File I/O, XMS, EMS, PIT, mouse, keyboard,
    video 13h). Real mode is SOLID. Confirmed autonomously via the SMB loop.
  • **event-3 = the real-hardware I/O reflect.** HVF reflected IOPL-0 IN/OUT as VTIB_EVENT=0;
    real silicon uses **VTIB_EVENT=3**. That was the ONLY diff blocking real-mode I/O (Skyroads
    stopped on `IN AL,DX`). Fixed (VDM_EVENT_IO_HW). host_try_io self-validates so routing is safe.
  • **dpmitest (16-bit PM): the SWITCH works, the kernel WON'T RUN PM.** svc10/11 install the LDT
    (CS=0x0f valid, AR=0xfa present/code/DPL3, base 0x1000), then the very first PM step returns
    **event 3 at a harmless `mov ax,0x0400` (b8 00 04), EIP UNADVANCED** — i.e. the kernel declines
    to execute our PM VDM. On HVF this class of problem was a SILENT terminate; on real silicon it's
    an OBSERVABLE event with full state = real #18 progress. Likely cause (matches old #18 RE): stock
    ntvdm does a fuller PM-run init — `VdmPMCliControl` (NtVdmControl service 13) + PM interrupt-flag
    control (fcn.0f00532e reads getMSW/PE bit) — that our host skips; HVF was lenient, real silicon isn't.
  • Current build: `g_dpmi_use_interp = 0` (real-CPU kernel PM path). Interpreter fallback (=1,
    ran i310102/DPMIBACK on HVF) is UNTESTED on bare metal.

▶▶ RESUME — NEXT STEPS (in order): ════════════════════════════════════════════════════════
  0. COMMIT the uncommitted bare-metal fixes (event-3, auto-exit, filebuf, PM-fault diagnostic).
  1. CRACK the PM-entry event 3 (the Doom/DOS4GW gate, now tractable because observable):
     (a) on the unhandled event 3, try RESUME/step a few times — does EIP advance (per-instruction
         trap) or stick (hard "won't run PM")? (b) RE stock ntvdm's PM-run path and replicate the
         `VdmPMCliControl`/service-13 + MSW/PE init before VdmStartExecution; (c) confirm whether a
         PM `C4 C4` BOP reflects as event 4 on real HW (it did on HVF) — if PM BOPs also changed, the
         INT-patch scheme needs the same event-3 treatment.
  2. Quick win option: build with `g_dpmi_use_interp = 1`, fire `i310102.exe` / `dpmiback.com` — proves
     16-bit DPMI works on bare metal via the interpreter regardless of the kernel PM issue.
  3. Skyroads should now render (event-3 I/O fix) — but it's graphical, and VNC viewing is dead;
     needs the user's physical monitor, or a working capture. Real-mode game validation still pending.

HVF VM: retired for this work but still on disk (vm/xp.qcow2). The Doom shareware was extracted to
  C:\DOOMS on that VM (irrelevant now). Do NOT chase DOS/4GW on HVF — it SIGABRTs on paged PM.

───────────────────────────────────────────────────────────────────────────────────────────────
(Everything below is the SESSION-7 checkpoint — HISTORICAL context; superseded by the above.)
───────────────────────────────────────────────────────────────────────────────────────────────

═══════════════════════════════════════════════════════════════════════════════
██ CHECKPOINT — 2026-08-18 (session 7). ██
═══════════════════════════════════════════════════════════════════════════════

BIG PICTURE: the 32-bit DOS/4GW real-CPU foundation is now PROVEN END-TO-END and
ALL PUSHED. GH #18 is broken for both 16-bit (runs 72-78) and 32-bit (runs 80-82).

REPO STATE (branch `spike/dpmi-16bit-switch`): **all pushed to origin, 0 unpushed**
  (HEAD = `274b4aa`). New this session: `f86e850` (runs 80-81 + run 78 confirm),
  `274b4aa` (run 82). Working tree clean except pre-existing untracked (MAINICON.ico,
  demos/, scripts/kd_*.py, scripts/trace_break.py, vm/ artifacts) — NONE mine; leave them.
  Host builds clean: `./scripts/build.sh` → build/ntvdmhost.exe (has runs 80-82).

WHAT WAS DONE + VM-CONFIRMED THIS SESSION (all now FACT; docs runs 78/80/81/82,
  [[dpmi-realcpu-pm]] memory):
  • run 78 = async IRQ0 injection into a hooked PM INT 08h — CONFIRMED (tmrhook box
    marched off the injected ISR, no polling; user saw it live). The timer-hook path Doom needs.
  • run 80 = a 32-bit (CS.D=1) PM `OUT` reflects as event 0 and is serviced — the run-72
    gate holds for 32-bit. Prereq: fixed INT 31h 0009 D/B parse (was CH low nibble → now
    `(ECX>>12)&0xF`). Probe tools/dostest/pm32io.asm.
  • run 81 = the DPMI mode SWITCH itself produces a working 32-bit CS: `dpmi_switch_to_pm`
    honors client_is_32bit (D/B=1 CS/DS/SS); main.c derives is32=EAX&1 + mirrors to g_ldt[];
    INT 2Fh 1687 now BX bit0=1. No 16-bit regression. Probe pm32sw.asm.
  • run 82 = a 32-bit renderer: mode 13h + grayscale DAC palette (768 PM OUTs) + a `rep stosd`
    vertical-gradient framebuffer fill → the Luna window showed the ramp. Probe pm32gfx.asm.

VM STATE (IMPORTANT): the XP VM was **left RUNNING** with 2 idle DOS windows (pm32sw +
  pm32gfx). An IDE restart likely orphans/kills that qemu. ON RESUME, start clean:
  `pkill -9 -f qemu-system-x86_64` then `./scripts/xp-vm.sh run` (boots the CURRENT
  vm/xp.qcow2, which is the freshly-restored clean debug-only image from this session).
  If it wedges on the logon logo: `cp vm/xp-debugonly-backup.qcow2 vm/xp.qcow2` then relaunch.

▶▶ RESUME — #3 remaining work for a REAL extender + REAL game (run-79 inventory items 3-5),
  needed once a 32-bit client uses callbacks / async timer hooks / >64K offsets:
  1. Widen the two catcher IRET frames (`dpmi_run_callback`, `dpmi_inject_pm_irq`) + the
     injected-IRQ frame to DWORD EFLAGS/CS/EIP when the target selector is 32-bit
     (`dpmi_sel_is32`). The catcher/trampoline CODE selectors must have D/B matching the client.
  2. Gate the `EIP/off & 0xFFFF` masks in the PM loop / `dpmi_service_pm_int` / `g_pm_int[].off`
     / buffer-offset reads on `dpmi_sel_is32` (note `g_int_vec[]` is only 0x10000 wide — a
     32-bit fault EIP>64K needs a different dispatch key).
  3. Base-0 ~2GB G=1 flat-selector ALLOC test (INT 31h 0000/0007/0008/0009) — the DOS/4GW flat
     model; verify it installs (dpmi_install already does the >1MB→G=1 path) and executes.
  4. Then a real DOS/4GW extender, then a real game (the acceptance test).

HARNESS LESSONS (hard-won this session): **ONE VDM/probe at a time** — 4 concurrent VDMs on
  this time-dilated HVF guest REBOOTED XP, which then wedged >6 min on the dirty-boot logon
  (autochk at 102% CPU) → forced a restore. Prefer a FRESH BOOT per probe. A looping/idle prior
  probe also holds `\\.\COM1`, so later probes get no serial → the SCREENDUMP is authoritative.
  Autorun CD trigger: fresh volume LABEL each mount (`autorun.inf` `open=<run>.bat`), hot-swap via
  `python3 scripts/qmp.py cd <abs-iso>`. Only ONE qemu at a time. See [[vdm-host-test-harness]].
═══════════════════════════════════════════════════════════════════════════════

═══════════════════════════════════════════════════════════════════════════════
██ POWER-DOWN CHECKPOINT — 2026-08-13 (session 6). SUPERSEDED — history below. ██
═══════════════════════════════════════════════════════════════════════════════

WHY THIS EXISTS: the PC was powered down mid-track. Everything needed to resume cold
is in this banner; the dated session logs below are history/detail.

REPO STATE (branch `spike/dpmi-16bit-switch`):
  • 3 LOCAL commits, NOT pushed to origin — `735637d` (run 78), `b5e2c5c` (run 79),
    `a1ee68a` (handoff). `git log origin/spike/dpmi-16bit-switch..HEAD` to see them.
    Decide whether to `git push` on resume (was left unpushed intentionally).
  • Working tree clean except pre-existing untracked files (MAINICON.ico, demos/,
    scripts/kd_*.py, scripts/trace_break.py) — NONE are mine this session; leave them.
  • Host builds clean: `./scripts/build.sh` → build/ntvdmhost.exe. tmrhook.com assembles.

VM STATE: powered OFF, clean. No qemu running. Backup intact at
  vm/xp-debugonly-backup.qcow2 (restore → vm/xp.qcow2 if XP wedges). This session did
  NOT boot the VM (user deferred VM-confirm to keep momentum). So TWO things are unverified.

WHAT WAS DONE THIS SESSION (detail in the SESSION 6 block just below, + docs runs 78-79):
  • run 78 = #2b async IRQ0 injection into a hooked PM INT 08h — IMPLEMENTED, code compiles,
    but NOT VM-confirmed.
  • run 79 = #3 (32-bit/DOS4GW) kickoff — landed D/B-aware host_try_io_pm (safe, no
    regression) + a full line-referenced 32-bit plan. Core 32-bit work NOT started (needs VM).

▶▶ RESUME — DO THESE TWO, IN ORDER (both need the XP VM booted):
  1. CONFIRM run 78. Boot VM (`./scripts/xp-vm.sh run`; ~5-6 min, poll screendumps until a
     stable non-black/non-loading frame >2MB). Trigger `tmrhook.com` via an AUTORUN CD with a
     FRESH volume label (hdiutil makehybrid; XP caches autorun by label) — the runner is
     tools/dostest/tmhrun.bat. Take 2 QMP screendumps ≥1 s apart: the red box should MARCH
     across mode 13h with NO input and NO INT 1Ah polling → proves the host is injecting IRQ0
     into the client's own PM INT 08h handler ~18.2×/s. If it marches, mark run 78 a FACT
     (update docs/research/dpmi-under-ntvdmcontrol.md run 78 + [[dpmi-realcpu-pm]] memory).
  2. START #3 for real. FIRST PROBE: write a minimal 32-bit client (16-bit DPMI entry → alloc a
     code selector → INT 31h 0009 set access 0xFA + D/B bit → far-jmp into 32-bit → a 32-bit
     `OUT 0x3C8,AL` → report), the smallest test of "does the kernel reflect a 32-bit PM I/O as
     event 0" (the run-72 gate, for 32-bit). If yes, proceed down the run-79 plan: honor
     client_is_32bit in src/vdm/dpmi.c switch as a base-0 ~2GB G=1 flat selector (NOT 4GB — XP
     NtSetLdtEntries rejects flat-4GB; stock ntvdm runs the same ~2GB cap, so DOS4GW is reachable
     to parity); 1687 BX bit0=1; arm VTIB_FLT_FLAG with client width; widen the 2 catcher IRET
     frames (dpmi_run_callback, dpmi_inject_pm_irq) to dword EFLAGS/CS/EIP; gate the EIP/off
     &0xFFFF masks on dpmi_sel_is32(). Full inventory = docs run 79.

HARNESS GOTCHAS (hard-won): only ONE qemu at a time (`pkill -9 -f qemu-system-x86_64` before a
  fresh launch); never `rm vm/qmp.sock` while qemu holds it; QMP send-key is LOSSY → always
  trigger clients via autorun CD (fresh label each mount); never `pkill` a KD session mid-halt
  (stale-halt wedge = reboot-only). Standing directive: DPMI is NOT done — measure against the
  games bar (Doom/Skyroads/ZAR playable), not the last milestone (see [[dpmi-completeness-directive]]).
═══════════════════════════════════════════════════════════════════════════════

Resume NTVDMEX — GH #18 real-CPU protected mode, via the guest kernel debugger. Session 3 SOLVED the KD tooling (the ~28 "wall" is gone) — the debugger is now a working, reliable instrument. North star unchanged (superset of XP-32 ntvdm; bar = Doom/Skyroads/ZAR).

★★★ SESSION 6 (2026-08-13) — runs 78-79 committed on `spike/dpmi-16bit-switch` (in sync w/ origin as of session start; new commits `735637d` run 78, `b5e2c5c` run 79 are LOCAL, push when ready). VM was NOT booted this session (user deferred VM-confirm to keep momentum). TWO deliverables:
  • RUN 78 (#2b async IRQ0 injection) — IMPLEMENTED, **VM-CONFIRM PENDING**. `dpmi_inject_pm_irq()` in src/host/main.c: on a latched IRQ0, if the client hooked INT 08h in PM (`g_pm_int[8]`, via 0205) and `g_dpmi_vi`, snapshot the interrupted PM ctx, push a 16-bit IRET frame → PM-return catcher on the client stack, vector to the handler, run it through the shared dispatcher, restore ctx on IRET (clears/restores g_dpmi_vi like a real INT). Main-loop wiring keeps polled `pit_int08` + adds `g_pm_irq0_latch` (survives CLI) + `g_in_pm_irq` guard. Catcher-selector install factored to shared `dpmi_ensure_pmret_sel()`. Probe `tools/dostest/tmrhook.asm` (+ `tmhrun.bat`): HOOKS INT 08h; ISR bumps a counter; box marches off the counter with NO INT 1Ah polling → movement proves injection. **CONFIRM: run `D:\tmhrun.bat` via autorun CD; 2 screendumps ≥1s apart, box should march.**
  • RUN 79 (#3 32-bit kickoff) — `host_try_io_pm` now **D/B-aware** (new `dpmi_sel_is32()` reads `g_ldt[].flags&0x4`; picks default opsize + full-EIP step from it). PROVABLY no regression: no current client sets D/B (none uses INT 31h 0009), so all D=0 clients decode identically. Docs run 79 = the full line-referenced 32-bit inventory + ordered plan. RESUME #3 (NEEDS VM): (1) honor `client_is_32bit` in `src/vdm/dpmi.c` switch — build a **base-0 ~2GB G=1 flat** selector (NOT 4GB: XP `NtSetLdtEntries` rejects flat-4GB; stock ntvdm runs under the same ~2GB cap, so DOS4GW reachable to parity); (2) 1687 BX bit0=1; (3) arm `VTIB_FLT_FLAG` with client width; (4) widen the two catcher IRET frames (`dpmi_run_callback`, `dpmi_inject_pm_irq`) to dword EFLAGS/CS/EIP when target is 32-bit; (5) gate the `EIP/off & 0xFFFF` masks on `dpmi_sel_is32`. **FIRST PROBE:** a minimal 32-bit client (16-bit DPMI entry → alloc code sel → 0009 set 0xFA+D/B → far-jmp 32-bit → 32-bit `OUT 0x3C8` → report) — smallest test of "does the kernel reflect 32-bit PM I/O as event 0" (the run-72 gate, for 32-bit).

★★★ SESSION 4 (2026-08-11) — THE FAULT-TRIGGER QUESTION IS ANSWERED (and Session-3's recommendation below is REFUTED). Session 3 said "use a BOP-based DPMI trigger (dpbrun/i31run), NOT raw pmfault." **WRONG — VM-confirmed via KD.** A BOP client can NEVER reach the reflect at 0x4f67f8: NTVDM BOPs are `C4 C4` = an invalid LES encoding ⇒ #UD ⇒ KiTrap06 ⇒ VdmDispatchBop, whereas 0x4f67f8 hangs off #GP ⇒ KiTrap0D (via the "BOP-only" gate 0x565041). DIFFERENT TRAP VECTORS. Proof: ran DPMIBACK on the real-CPU path (host v62, g_dpmi_use_interp=0) with the reflect bp armed — it ran end-to-end (PM switch, 0301 round-trip, clean exit) and the bp was NEVER hit (0 serviced). ⇒ **ONLY a raw privileged-instruction #GP (pfrun/pmfault HLT) routes toward 0x4f67f8.** So the correct trigger is `D:\pfrun.bat` after all — Session 3 had the polarity backwards.
  TOOLING: built `scripts/pmfault_observe.py` — classifies every KD halt (reflect-bp / benign break-in 0x80527bdc / benign LOAD_SYMBOLS 0x8052e4c4 / **UNEXPECTED FAULT**) and dumps ctx+stack+code + single-steps the unexpected one. FIXED a receiver race (it first mixed wait_state_change + get_context → desync → the kernel stalled at a 0xCC at raised IRQL → watchdog REBOOT before pfrun even ran; the reflect bp itself is SAFE — the dpmiback session armed the same bp and idled fine). Now get_context-ONLY, exactly like desktop_trace. The repeated resets pushed XP into its "did not start successfully" recovery menu → restored `vm/xp.qcow2` from `vm/xp-debugonly-backup.qcow2` (clean). VM currently POWERED OFF but clean.
  ★★★ RUN 71 (2026-08-11) — pfrun OBSERVATION SUCCEEDED, and it's the #18 ANSWER (docs run 71). Two fixes cracked it: TRIGGER = an autorun CD (`autorun.inf`→pfrun.bat, `/tmp/ntvdmex-auto.iso`) mounted via QMP the INSTANT the bp arms (a log-watcher auto-fires `qmp.py cd` on `armed h=`) — 100% reliable (this VM's send-key drops chars even at 0.6s/char) AND keeps the guest BUSY so it does NOT reboot (the 4 idle-wait attempts all rebooted; arming the 0xCC at 0x805cd7f8 + letting the dilated HVF guest sit idle destabilizes it). RESULT: pfrun ran real-CPU (serial: PM switch CS=0f:12c → "about to HLT" → serial STOPS), and the KD observer saw **NOTHING** — no reflect-bp hit at 0x4f67f8, no exception, no bugcheck, guest healthy. KD was provably live (pfrun could only run because the observer resumed the guest correctly). **⇒ the kernel's KiTrap0D SILENTLY TERMINATES the VDM on a raw PM #GP — a HANDLED path, so it never reaches the reflect and never breaks KD. Chasing the raw-#GP reflect is a DEAD END** (directly confirms runs 65-69's "invisible fault").
  ★★ OPTION C IS DEAD (2026-08-11), and the DPMI-init RE thread advanced (docs "Kernel RE session 8"). Option C (BOP-patch privileged insns) is blocked two ways: the BOP is 2 bytes (C4 C4) but HLT/CLI/STI/IN-OUT-DX are 1 byte (no in-place swap), AND those are ops the KERNEL is meant to virtualize, not us. RE session 8 (fresh ntoskrnl disasm) reframes #18: `0x4f67f8` is NOT "the reflect entry" — it's a small reflect-DECISION function (`if EPROCESS+0x158 (VdmObjects)==0 return 0; else classify [0x714] bit3`), and it sits INSIDE KiTrap0D's in-kernel VDM instruction EMULATOR (bytes right before it are `out dx,eax` — the kernel emulates OUT for a VDM). Our raw HLT is dispatched to a terminate branch, never reaching `0x4f67f8`. **⇒ HLT was a MISLEADING probe** (the kernel has no HLT case); the game-relevant ops (IN/OUT/CLI/STI) may already be emulated in-kernel for a proper VDM. ★★★ RUN 72 (2026-08-11) — BREAKTHROUGH, the OUT probe answered it: a real-CPU PM `OUT` is trapped by the kernel and reflected to our monitor as **event 0 (I/O)** — the SAME event our V86 device path already handles. Built `tools/dostest/outprobe.asm` (OUT DX,AL to VGA 0x3C8/0x3C9 instead of HLT) + `outrun.bat`, ran via autorun CD (NO KD, no reboot risk). serial: PM switch OK → "about to OUT" → `DPMI: unexpected PM stop event=0x00000000 CS:EIP=0f:0x138` (frozen ON the OUT). **⇒ real-CPU PM I/O virtualization WORKS at the kernel level; HLT was a red herring (no kernel case → terminate).** The only gap is HOST-side: our DPMI PM loop services only event==4 (BOP)+the reflect, so it treats event==0 as "unexpected" and spins → watchdog kills it.
  ★★★ RUN 73 (2026-08-11) — DONE + VM-CONFIRMED: real-CPU PM port I/O now flows to our VDDs. Added `host_try_io_pm()` in src/host/main.c (PM-addressed twin of `host_try_io`: decodes IN/OUT at `dpmi_sel_base(CS)+EIP`, dispatches `vdd_bus_io`, advances EIP) and wired it into the DPMI PM loop on `ev==VDM_EVENT_IO(0)`/`GPFAULT(2)`. outprobe.com now prints "OUT survived -- guest RESUMED" for BOTH OUTs (0x3C8/0x3C9) and exits cleanly (4Ch). So the #18 wall is broken: real-CPU protected-mode port I/O is virtualized to our VDDs, no #GP reflect needed.
  ★★★ RUNS 73b + 74 (2026-08-11) — DONE + VM-CONFIRMED (visual): (73b/ioverify) PM DAC write→readback round-trips through the VDD (`read back = 0A 14 1E`), proving IN+OUT both service end-to-end. (74/mode13) a REAL protected-mode VGA client renders on the real CPU: `INT 10h` mode 13h (now routed to the VDD in PM), alloc an A0000 framebuffer selector (`INT 31h 0000/0007/0008`), `stosb`×64000 pixel fill, and the ntvdmhost Luna window SHOWS the 320×200 gradient. Host changes: patch scan also rewrites `CD 10`; `dpmi_service_pm_int` routes `vec==0x10`→video VDD; new `g_dpmi_done` so the run-52 watchdog stands down on clean exit (keeps the window up). ⇒ real-CPU PM graphics through our VDD, no reflect/interpreter. Milestone #6 capability demonstrated.
  ★★★ RUN 75 (2026-08-11) — DONE + VM-CONFIRMED (visual): real-CPU PM ANIMATION. animate.com loops forever scrolling a mode-13h gradient (fill with a per-frame phase offset + INT 31h 0400 yield); two screendumps 6s apart differ = motion, no watchdog kill. Host: PM loop cap raised to run-until-window-close (g_running); watchdog now kills ONLY a sustained freeze (resets on g_dpmi_iter progress), stands down on g_dpmi_done. So a game-shaped redraw loop runs indefinitely on the real CPU rendering through the VDD.
  ★ DPMI is NOT done — see [[dpmi-completeness-directive]]. Answer "is DPMI done?" against the games bar (Doom/Skyroads/ZAR), not the last milestone. DONE so far (real-CPU 16-bit PM, VM-confirmed): run 72 PM I/O trap=event0, 73 I/O servicing, 73b ioverify round-trip, 74 mode13 static render, 75 animation, bounce (palette via PM OUT + moving box), **run 76 INPUT (INT 16h arrow keys drive a box; also fixed extended-key capture in WM_KEYDOWN, which fixes V86 arrows too; INT 33h mouse routed, untested)**.
  REMAINING (suggested order): (1) DONE input (run 76); (2a) DONE polled timing (run 77 — INT 1Ah + BIOS tick advance in the PM loop, timerbox VM-confirmed); (2b) **async IRQ0 injection to a PM INT 08h hook** (games like Doom HOOK the timer) — SPEC (worked out, not yet built): in the PM loop when `InterlockedExchange(&g_irq0_pending,0)` fires, after `pit_int08`, if `!g_isr_active && g_dpmi_vi && g_pm_int[0x08].sel`: save CS/EIP/EFLAGS/vi, push a 16-bit IRET frame {EFLAGS, g_pmret_sel, DPMI_PMRET_OFF} onto the PM stack (linear = `dpmi_sel_base(SS)+(SP&0xFFFF)`, via pokew), set CS:EIP=g_pm_int[8], `g_dpmi_vi=0; g_isr_active=1`; then in the main PM loop detect the catcher (ev==BOP && csv==g_pmret_sel && eip==DPMI_PMRET_OFF) → restore saved CS:EIP/EFLAGS + vi, `g_isr_active=0`, continue. PREREQ: ensure g_pmret_sel + the DPMI_PMRET catcher BOP are allocated at PM ENTRY (currently lazy on first 0303). Test client: a PM prog that INT 31h 0205-installs an INT 08h handler that ++a counter and moves a box → box moves with NO polling. Watch: don't double-count the BIOS tick (pit_int08 already ++0040:006C); a hooking handler usually doesn't touch it. (3) **32-bit DPMI / DOS4GW** — the big one, Doom's extender (all work so far is 16-bit; needs 32-bit flat selectors + 32-bit PM exec + 32-bit EIP/IO handling; host_try_io_pm masks EIP to 16-bit); (4) **raw mode switch (INT 31h 0305/0306)** for real extenders (RAWJMP7 stalled here); (5) **broader INT 31h surface** — 0002 seg→desc, 0600-0604 page-lock, 0800/0801 phys-map (VESA LFB); (6) **a REAL extender + REAL game** end-to-end (the acceptance test); (7) retire the g_dpmi_use_interp toggle (confirm real-CPU path runs i310102/DPMIBACK). Probes: mode13/animate/bounce/kbdbox + *run.bat. Trigger via AUTORUN CD (fresh LABEL each time; GUI keyboard is lossy). HARNESS GOTCHA: only ONE qemu at a time — never `rm vm/qmp.sock` while a qemu holds it (unlinks the socket → screendump fails); `pkill -9 -f qemu-system-x86_64` to clear leftovers before a fresh launch. Docs: runs 72-76 + RE session 8. Option C below is SUPERSEDED/dead.
  (SUPERSEDED) RESUME (Session 5) = the VALIDATED real-CPU-PM direction is run-68 OPTION C: do NOT rely on the #GP reflect. **Pre-patch identifiable privileged instructions (HLT/CLI/STI/IN/OUT/LGDT/LIDT/…) to `C4 C4` BOPs**, like the existing INT-site scan (`dpmi_patch_int_sites`), and service them via the already-proven host BOP/event-4 mechanism (the same path that runs i310102 + DPMIBACK). NEXT SPIKE: extend the patch scan to privileged opcodes → re-run pfrun (its HLT now a BOP) → confirm the host services it instead of the VDM dying. If that pays off it opens real-CPU PM for the game class; if not, the interpreter (g_dpmi_use_interp=1) already covers 16-bit DPMI and the Sound epic (#20/#21) is the other high-value track. Tooling ready: `scripts/pmfault_observe.py` (passive KD observer, classifies+dumps), `scripts/gtype.sh`, the AUTORUN-CD trigger (build: stage + `autorun.inf` open=pfrun.bat → hdiutil; mount on `armed h=`). VM boot recipe: `./scripts/xp-vm.sh run` → poll screendump sz>2MB, exclude black a50ae736 / loading 5ea20161 → native 1024×768. If resets pile up → XP recovery menu → restore `vm/xp.qcow2` from `vm/xp-debugonly-backup.qcow2`. See docs runs 70-71 + [[kd-guest-debugger-ops]].
  (Session-3 text below is retained for the KD operational recipe, but its "BOP trigger" conclusion in ★ THE LAST MILE is superseded by the above.)

★ SESSION-3 RESULT (2026-08-07): KD TOOLING WORKS END-TO-END. `scripts/desktop_trace.py` breaks in at the idle /debug-only desktop, arms the reflect bp @ 0x805cd7f8, and services the module-load break stream ROBUSTLY (drives continues off get_context as the authoritative halt-detector; the old wall was just a MISSED state-change packet, not corruption). VALIDATED: serviced 28 of pfrun's module loads past the old wall, zero wedge, stayed live. The **/debug-ONLY provisioned image is backed up as `vm/xp-debugonly-backup.qcow2`** (cp over vm/xp.qcow2 to reset; boots normally to desktop — NO re-provision needed). NewState constants: 0x3030=EXCEPTION, 0x3031=LOAD_SYMBOLS.

★ THE LAST MILE (next step): with the bp armed, `pfrun` (RAW PM HLT #GP via pmfault) did NOT hit 0x4f67f8 — it bugchecked/rebooted the guest. The reflect is `KiTrap0D→0x565041(BOP-only)→0x4f67f8`, so a RAW #GP bypasses the BOP-gated 0x565041 and never reaches the reflect. **RESUME = build/mount a BOP-based DPMI trigger (i310102 / dpbrun-style, INT 31h path) instead of raw pmfault, then run `python3 -u scripts/desktop_trace.py` (break-in ~110s → arm → GUI-launch the trigger → catch reflect → 400-step single-step trace).** The tooling is ready; only the trigger needs swapping. GUI trigger recipe: Start(50,748)→Run(232,596)+Enter→type path→OK button ~(188,715), move-away-before-click, be patient (slow VM).

STATE OF THE VM (session 3, 2026-08-06):
- **Fresh XP reinstalled, provisioned, backed up.** `vm/xp-fresh-install-backup.qcow2` = bare fresh install (autologin Test/blank, NO /debug). `vm/xp-break-stuck.qcow2` = provisioned (autologon + /debug on COM2 + IFEO ntvdm→ntvdmhost) PLUS **/break** — halts deterministically at boot init; the halt PERSISTS so you can hammer continue variants with NO reboot between tries (cp it over `vm/xp.qcow2` to reset). Internal snapshot `freshinstall` also exists.
- **KD attach + read is SOLID:** KernBase 0x804d7000, GetVersion 15.2600, read_vmem, get/set_context (verified set_context moves EIP), write_bp/restore_bp all work. Reflect entry static 0x4f67f8 → runtime **0x805cd7f8** (slide 0x800d7000).

THE BREAKTHROUGH (why "resume" seemed broken):
- resume()/cont() were fine. The real obstacle: with **/debug**, the kernel executes an int3 (**DbgLoadImageSymbols**, runtime **0x8052e4c4**) on EVERY module load so a debugger can load symbols — each HALTS the CPU until continued past (advance EIP+1 since byte@EIP==0xCC, then DbgKdContinueApi/DBG_CONTINUE). Confirmed on the wire: Continue is ACK'd with the correct id, kernel resumes, re-breaks at the next module.
- **CRUCIAL corollary:** these module-load breaks ONLY halt when a debugger is CONNECTED. With /debug and NOBODY connected, boot runs normally to the desktop (breaks are no-ops). So **/break was the wrong turn** — it forces a connection during boot, turning every module load into a serviced halt (molasses + the sync wall). The RIGHT architecture: /debug-only (no /break) → boot normally to desktop → connect at the desktop → break-in once → arm bp → trigger pfrun → service pfrun's ~10-15 module-load breaks → catch the reflect #GP.

THE ~28 WALL — SOLVED (was NOT a KDCOM sync bug, despite earlier suspicion):
- At the "wall" the kernel is HALTED and fully responsive (get_context works) — the client had simply MISSED one state-change packet (receive timing) and then waited while the kernel waited for us (mutual stall). NewState decode confirmed the ~28 are LOAD_SYMBOLS notifications (0x3031), NOT int3 exceptions; pc=0x8052e4c4 is a MmIsAddressValid validator the reporting thread sits in.
- FIX (in `scripts/desktop_trace.py`, the current harness): drive continues off `get_context` (returns EIP if halted, None if running) as the authoritative halt-detector; decouple Continue from receive; single-byte break-in (cadence>secs) for zero leftover. Superseded scripts: kd_boot_servicer.py, kd_servicer2.py, trace_break.py (all /break-era; ignore). Off-VM RE env: /tmp/ntvdmex-re/ntoskrnl.exe (HAS symbols; r2 base 0x400000); vm/kddll_ref.c = ReactOS KDCOM source.
- Old /break test-bed (`vm/xp-break-stuck.qcow2`) is now only of historical use — /break forces molasses; the /debug-only backup is the one to use.

TOOLING BUILT THIS SESSION (all in scripts/):
- `kd_boot_servicer.py [bp_runtime_hex] [max_secs]` — poll-attach, arm bp, continuously service the break stream, distinguish the reflect bp (0x805cd7f8) from symbol breaks (0x8052e4c4), single-step-trace on the reflect HIT. **This is the harness to finish** once sync is robust.
- `trace_break.py` — earlier /break attach+trace (superseded by kd_boot_servicer).
- `kdclient.py` — break_in() now sparse (cadence=90, secs=260); do_session deadline 600.

THE RECIPE TO FINISH (once sync is robust):
1. Get a /debug-ONLY provisioned image (restore `vm/xp-fresh-install-backup.qcow2` → re-provision with `vm/provision.iso` autorun, which adds /debug WITHOUT /break). Boots normally to desktop.
2. At the idle desktop, run the (sync-hardened) servicer: break-in once, arm bp @ 0x805cd7f8, then service continuously.
3. Trigger `C:\ntvdmex\pfrun.bat` in the guest (GUI: Start→Run; files already at C:\ntvdmex — do NOT remount provision.iso, its autorun re-runs provision.cmd). The guest runs freely at idle desktop so GUI input works.
4. pfrun → ntvdmhost → PM fault → #GP → reflect bp HIT at 0x805cd7f8 → single-step 400 logging rebased EIP+EAX → SEE the gate that returns 0.
Reflect chain (static VAs, r2 base 0x400000): 0x4f67f8(entry)→0x4f6f67→0x4f6efd(CS:EIP from VDM_TIB[class*0x10])→0x4f6e6f(SS:ESP=[TIB+0x638]:0x1000)→gates 0x4f6d3c(CS)/0x4f6dc0(SS) via 0x45dd5f. class=6 for #GP. **NEVER bp 0x4f6d3c/0x4f6dc0** (hot in normal selector validation → floods KD → bugcheck). Only bp 0x4f67f8 + single-step.

HARD-WON RULES (see [[kd-guest-debugger-ops]]): never `pkill` a KD script while CPU~100% (stale-halt wedge → reboot-only); background python needs `-u` (block-buffered else); `system_reset` is the safe image-safe recovery and provisioning persists; QEMU/qmp ops need `dangerouslyDisableSandbox:true`; screendump-diff (identical over ~6s) distinguishes halted vs live. VM is EXTREMELY slow (~110s break-in latency, multi-min boots). Landmarks: ntoskrnl VA base 0x400000, KernBase 0x804d7000; ntvdm 0x0f000000; host v62 = build/ntvdmhost.exe (g_dpmi_use_interp=0 real-CPU + reflect trampoline; =1 restores the VM-confirmed interpreter). VM launch = `./scripts/xp-vm.sh run` (COM1→vm/serial.log, COM2→vm/kd.sock, QMP→vm/qmp.sock).

FALLBACK (still true): the interpreter (g_dpmi_use_interp=1) already runs i310102 + DPMIBACK end-to-end — 16-bit DPMI is effectively done for the games bar. If the KD sync fight isn't worth it, ship the interpreter and pivot to the Sound epic (#20 SB16 / #21 OPL), the biggest unbuilt piece. But the KD trace is now genuinely CLOSE — only the sync-hardening stands between here and the #18 observation.
