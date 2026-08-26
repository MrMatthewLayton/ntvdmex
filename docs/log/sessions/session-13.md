# Session 13 — 2026-08-20

> Archived verbatim from `return-ntvdm.md`, which was the single rolling handoff
> file until 2026-08-26. Split by session so it can be read by topic and date
> rather than by scrolling. **Nothing has been edited** — including conclusions
> that later sessions REFUTED, which are kept on purpose: the refutations are
> some of the most valuable material here.

```
═══════════════════════════════════════════════════════════════════════════════
██ CHECKPOINT — 2026-08-20 (session 13). M9 API COMPLETE; MODE 12h IS THE WALL. ██
═══════════════════════════════════════════════════════════════════════════════

▶ RESTART POINT: branch `m9/completeness`, **19 commits UNPUSHED**, working tree clean apart
  from the usual not-mine untracked files (MAINICON.ico, demos/, scripts/kd_*.py,
  trace_break.py) and the untracked native `tools/dostest/*_test` binaries (repo convention).
  Host = **dpmi-harness-v180**, built clean, deployed to the share `bm/`. Rig healthy.
  **Share knobs CLEARED** — no capture.flag, no noa000.flag, no interp12.flag. Leave them that
  way; a stray knob makes every later run lie to you.
  Verified at v180: selftest **8/8**, off-VM battery **325/325**, all 15 probes clean.

▶ **THE RIG IP: `192.168.1.29`. TRY IT FIRST — A REBOOT DOES NOT MOVE IT.** (User's
  correction, 2026-08-21, after I swept the LAN following a reboot for no reason.) Only a
  **network drop** has ever moved it: `.34` → `.29` after a broadband outage. So sweep only
  when `.29` genuinely does not answer on port 445. Note an ARP sweep leaves INCOMPLETE
  entries for every address, so `arp -a` afterwards lists all 254 and means nothing — probe
  port 445 instead. LAN access needs `dangerouslyDisableSandbox`.
  `mount_smbfs -N //guest@192.168.1.29/ntvdmex /tmp/xpshare`
  ▶ **If the box goes down, the SMB mount goes STALE and any `ls` of it HANGS** (it cost a
    3-minute timeout). Once the box is back the mount may simply be empty — just re-run
    `mount_smbfs`. Do not reach for a forced unmount first.

┌──────────────────────────────────────────────────────────────────────────────┐
│ 1. WHERE M9 GOT TO                                                            │
└──────────────────────────────────────────────────────────────────────────────┘

  **INT 21h: 103/103.** Every service MS-DOS 6.22 defines is implemented and oracle-matched.
  **BIOS: complete** — modes, palette, character generator, VESA, and INT 11h/12h/13h/14h/15h/
  17h/20h/25h/26h/27h/28h/29h, every one of which was previously a bare IRET handing the caller
  its own registers back.
  **All five real 6.22 tools report `INT21 unimplemented: none`** — MEM, CHKDSK, TREE, ATTRIB,
  COMMAND.COM. EXEC works: a parent launches a child, the child runs, the parent resumes with
  the child's exit code.

  GitHub: **17 issues closed**, 9 raised (#47-#55), 5 left open with a comment saying exactly
  what remains. Epic #24's body carries the full picture.

  ▶▶ **COVERAGE IS NOT CORRECTNESS, AND WE HAVE PROOF.** MEM.EXE reports nothing missing and
     prints WRONG NUMBERS (#47): "largest executable program size 0K (4,294,967,280 bytes)",
     conventional free 0K, XMS 0K against a 16 MB pool. Every function it calls is implemented
     and oracle-matched. **That is the failure mode this whole epic exists to remove, surviving
     precisely BECAUSE the coverage is complete.** Fix it before any memory-parity claim.

┌──────────────────────────────────────────────────────────────────────────────┐
│ 2. THE NEXT TASK: WHY DOES PROTECTING A0000 STALL THE VDM?  (GH #55)          │
└──────────────────────────────────────────────────────────────────────────────┘

  **Mode 12h has never rendered.** Six of the ten QuickBASIC demos in `demos/` are SCREEN 12
  and all six are affected; the three SCREEN 13 ones work. Skyroads is 13h, which is why our
  one game never touched this path. (Sources in `demos/src` — BLIT.BAS is four lines and draws
  random filled boxes in all 16 colours.)

  ★★★ **THE MEASUREMENT. Same program, same build, one knob:**
        trap armed             io_events =         10   guest frozen at 0050:0037 (58/60 HBs)
        trap OFF               io_events = 22,532,292   guest running, PC moving every sample
        trap off + interpret   io_events =         10   two batches of 46/38 instrs, then quiet
    `PAGE_NOACCESS` and `PAGE_READONLY` behave IDENTICALLY, so it is NOT reads-vs-writes — it
    is protecting the range at all. Knob: **`noa000.flag`** on the share disables the trap.

  ★★★ **THE FRAMING THAT MATTERS, and it narrows the RE a lot:** with the trap simply OFF the
    guest RUNS PERFECTLY — 22.5M events, correct execution, PC advancing. The only thing wrong
    is that its A0000 writes bypass the planar engine into the raw aperture. **So the guest is
    not the problem and the planar engine is not the problem. The sole issue is INTERCEPTING
    those writes.** The question is therefore narrow: what does the VDM require of that address
    range that VirtualProtect breaks, and **is there a kernel-sanctioned way to ask for the
    same interception** (a VDD memory hook, the kernel's own A0000 handling) rather than doing
    it behind the kernel's back?

  ▶ **RULED OUT BY MEASUREMENT — DO NOT RE-INVESTIGATE ANY OF THESE:**
    • **The mode table** (#39). Resolves 12h correctly: `mode=0x12/kind=01/640x480`, proved by
      the `STAGE2: mode sets:` line. I nearly rewrote it anyway.
    • **The planar write engine.** Complete and correct — 4 write modes, set/reset, ALU, bit
      mask, latches. I nearly rewrote THIS anyway too.
    • **The IVT.** `ivt08=0050:0034 ivt1C=0050:003a`; QuickBASIC hooks neither.
    • **Async IRQ injection.** 545 successes, ZERO bails, zero nest-blocks.
    • **The "mode-12h MOV-store decoder gap"** from the M3 notes: `interp-refused=0`. The
      interpreter never declines an opcode. **THAT LEAD IS DEAD.**
    • **Unhandled events.** None — no `STAGE2: stop event` line; every event is serviced.
    • **Interpreter-driven mode 12h** (`interp12.flag`, committed as a measured negative). The
      reasoning was sound — port traps alone hand us control 22M times, so no page protection
      should be needed — but the storm gate is met twice in thirty seconds and the guest goes
      quiet afterwards.

  ★★ **FIXED ON THE WAY (keep, it stands alone): `host_interp()` could not take interrupts.**
    It ran up to 2,000,000 guest instructions in the host with no way to be interrupted. A
    guest loop that can only END on an interrupt — BLIT's `DO WHILE INKEY$ = ""` — ran there
    forever. It now checks for a pending IRQ every 256 instructions and yields. 15x improvement.
    The interpreter stands in for the CPU; a real CPU takes interrupts mid-loop.

  ▶ **WHY THIS PROBABLY NEVER WORKED:** the M3 planar trap was **VM-confirmed on HVF and never
    on real hardware**, and there is precedent for exactly this class of difference — session 8
    found HVF reflects IOPL-0 I/O as event 0 while real silicon uses event 3.

┌──────────────────────────────────────────────────────────────────────────────┐
│ 3. THE TOOLING BUILT THIS SESSION — USE IT, DO NOT REBUILD IT                 │
└──────────────────────────────────────────────────────────────────────────────┘

  **THE ORACLE (#25)** — genuine MS-DOS 6.22 under QEMU, ~3s a query, offline.
      ./scripts/oracle.sh tools/dostest/p_ver.com      # run a guest binary on real DOS
      ./scripts/oracle.sh --batch "VER"                # run DOS commands
      ./scripts/oracle.sh --selftest                   # 4/4
      python3 scripts/dosoracle/build.py               # rebuild the image, ~75s
    Media = `./msdos-622` (4 retail floppies, gitignored). Runs `snapshot=on`, so a probe
    calling destructive DOS functions cannot corrupt it. Full write-up:
    `docs/research/dos-oracle.md`.
    ► **PIXELS:** there is no capture-on-success path. The only way to get a picture out is the
      TIMEOUT screendump: `dosoracle.py run BLIT.EXE --timeout 22 --screenshot out.ppm`.
      That is how `build/shots/demos/oracle_blit.png` (the mode-12h reference) was captured.

  **THE DIFFERENTIAL HARNESS (#26)** — one .COM, three hosts, one diff.
      python3 scripts/dosdiff.py tools/dostest/p_ver.com
    • **NTVDMEX does not vote.** It is the subject; the oracles vote. Letting the thing being
      graded into its own consensus would be circular.
    • **`SIG`** — each case declares which registers hold ITS answer. DS/ES follow the PSP and
      most FLAGS bits are undefined after a DOS call.
    • **Buffers are diffed too**, with `ignore_bytes` as the buffer analogue of SIG.
    • **54 recorded rationales** in `tools/dostest/oracle-rules.json`, merged not first-match.
    • 15 probes in `tools/dostest/p_*.asm`, all built on `probe.inc`. Companion files via a
      `<probe>.deps` sidecar. Every host runs a probe from its own directory.
    • **NOT WIRED: stock `ntvdm`** — needs an rt.bat variant that drops the IFEO Debugger key,
      and a decision on the display-wedge risk.

  **THE LOUD-FAILURE BLOCK (#27)** — every run ends with a to-do list:
      STAGE2: INT21 unimplemented: / undefined-on-6.22: / BIOS partial: / INT10 unimplemented:
      STAGE2: video modes unsupported: / mode sets: / video now: / ivt08=...
    Plus `INTERP-REFUSED` (names any opcode the interpreter declines) and `BATCH ran=...`
    (how far each interpreter escalation got). **These instruments are what killed four wrong
    hypotheses this session. Read them before theorising.**

┌──────────────────────────────────────────────────────────────────────────────┐
│ 4. TRAPS — EVERY ONE OF THESE COST REAL TIME                                  │
└──────────────────────────────────────────────────────────────────────────────┘

  ▶ **THE REFERENCE IS THE ORACLE, NEVER OUR OWN PREVIOUS BUILD.** I called mode 12h "a
    regression I introduced today" because our new output differed from our old. **Neither was
    correct** — the oracle showed 16 colours, both of our builds showed 2. *Different is not
    wrong when nothing is right.* One oracle run settled in seconds what an hour of comparing
    our own screenshots could not.
  ▶ **BOP NUMBERS ARE A SHARED NAMESPACE** across DOS, BIOS, XMS (0x43) and DPMI (0x50-0x57).
    Planting INT 20h with BOP 0x20 — already INT 21h's — made the BIOS dispatch intercept EVERY
    INT 21h call as "terminate program"; selftest exited at its first DOS call with no output.
  ▶ **LINEAR 0x714 IS KERNEL VDM STATE.** Putting the AH=65h tables at segment 0x0071 (the
    DOS-resident MCB block's data area — by the memory map, exactly the right home) wrote over
    it and wedged EVERY guest including selftest. The map says free; the kernel disagrees.
  ▶ **DOS CALLS THAT RETURN A SEGMENT IN DS** (1Bh, 1Ch, 32h, 52h) corrupted the probes' own
    output: every probe store is DS-relative, so the probe wrote its state into DOS's segment
    and printed labels read from there. Fixed in `probe_capture`, not per-probe.
  ▶ **MEASURE THE BUFFER, NOT JUST THE REGISTERS.** The country block is **24 bytes, not the
    commonly quoted 34** — poison the destination with 0xEE first. Writing 34 would clobber ten
    bytes of the caller's memory.
  ▶ **POISON THE OUTPUT REGISTERS.** "Untouched" and "deliberately zero" are otherwise
    identical, and BX may hold leftovers from the probe's own print routine — which is exactly
    what happened (a "result" of 3246 that `probe_emit` had left there).
  ▶ **A WEAK ASSERTION IS WORSE THAN NO TEST.** My own selftest asserted `FIND "File(s)"`,
    matched nothing (6.22 prints `file(s)` lower-case, FIND is case-sensitive) and still
    reported PASS, because it only checked "some output appeared".
  ▶ **CASE NAMES MATTER.** A case first called `4F.exhausted` actually measured whether a
    failed find-first clobbers the DTA — the misleading name framed a CORRECT result as our bug.
  ▶ **A .COM OWNS ALL OF MEMORY**, so EXEC returns AX=0008 until the parent gives some back
    with 4Ah. Not a defect — it is what every real shell does. "EXEC says out of memory" is
    otherwise a mystifying first symptom.
  ▶ **DOSBOX CANNOT RUN HEADLESS.** `SDL_VIDEODRIVER=dummy` hangs dosbox-x and aborts
    dosbox-staging. The adapter opens a real window; it will not work over plain ssh.
  ▶ **THE DEV SANDBOX REFUSES TO BIND A UNIX SOCKET ANYWHERE**, TMPDIR included — the QEMU
    monitor runs on stdio. `$TMPDIR` also differs inside and outside the sandbox; use absolute
    paths when handing files between the two.
  ▶ **`cd` PERSISTS BETWEEN COMMANDS** in this harness. A leaked `cd` silently ran a whole
    probe sweep from the wrong directory and reported nothing.

▶▶ RESUME — NEXT STEPS (in order):
  1. **GH #55 — RE XP's VDM memory handling.** The narrow question: what does the VDM require
     of A0000 that VirtualProtect breaks, and is there a kernel-sanctioned interception (VDD
     memory hook / the kernel's own A0000 path)? Everything else is ruled out and listed above.
     Acceptance: BLIT.EXE renders 16-colour filled boxes matching `build/shots/demos/
     oracle_blit.png`. Then the other five SCREEN 12 demos.
  2. **GH #47 — MEM.EXE's wrong numbers.** Silent wrongness in the MCB chain / stubbed SysVars
     (#48) / XMS reporting. Do NOT close it by making the numbers look nicer.
  3. **Then the demo sweep** — 10 QuickBASIC demos, screenshots, USER WATCHING THE SCREEN
     (they asked to be told before it runs). `capture.flag` on the share enables self-capture;
     shots come back as `shot_<test>_*.bmp`. **Delete the flag afterwards.**
  4. Finish #26 (stock ntvdm host) and #28 (the version menu) if the display-wedge risk is
     acceptable.
  5. `git push` — 19 commits sitting local on `m9/completeness`.
```
