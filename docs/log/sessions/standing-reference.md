# Standing reference

> Archived verbatim from `return-ntvdm.md`, which was the single rolling handoff
> file until 2026-08-26. Split by session so it can be read by topic and date
> rather than by scrolling. **Nothing has been edited** — including conclusions
> that later sessions REFUTED, which are kept on purpose: the refutations are
> some of the most valuable material here.

```
═══════════════════════════════════════════════════════════════════════════════
██  STANDING REFERENCE — rig operations, instruments, landmarks, older traps.  ██
██  Everything below is still true; the narrative history has been pruned.     ██
██  ⚠ "this session" below means SESSION 17 or 15 -- NOT 18, NOT 19.            ██
═══════════════════════════════════════════════════════════════════════════════
┌──────────────────────────────────────────────────────────────────────────────┐
│ ★★ FOUR DEAD ENDS, EACH RULED OUT BY MEASUREMENT. DO NOT RE-SPEND A SESSION. │
└──────────────────────────────────────────────────────────────────────────────┘

  1. **`CLI`/`STI` in PM are FINE.** `tools/dostest/pmfault.asm` settles the family in
     twenty seconds:
     ```
        IN AL,0x21   SURVIVED   (arrives as VDM_EVENT_IO -- the CONTROL CASE)
        STI          SURVIVED
        CLI          SURVIVED
        INT3         DIED
        HLT          DIED
     ```
     A previous handoff named CLI/STI as the blocker and GH #18 as the critical path.
     **That was wrong** (see the method lesson below).
  2. **IOPL cannot be raised.** Setting IOPL=3 in `VTIB_EFLAGS_PM` does nothing — the
     kernel STRIPS it (live EFLAGS across a whole run: 0x…0296/0292/0206/0202/0246,
     bits 12-13 never set). `NtSetInformationProcess(ProcessUserModeIOPL)` is worse: at
     CPL <= IOPL the I/O permission bitmap is BYPASSED, so guest `IN`/`OUT` would reach
     real hardware instead of our VDDs.
  3. **`AX=FF00` failing is a RED HERRING.** It is the last service before the run ends,
     which is exactly why it looked causal. Its caller settles it: the code after the
     call is `testb $0x1,0x347e / jne / jmp` — a MEMORY flag. **FF00's CF is never
     tested.** Its handler services it internally and never chains, so it is not a
     missing host service either.
  4. **`DOOM.ETX` is NOT an error signal.** DOS/4GW opens `<program>.ETX` — its
     error-TEXT file — during normal startup, before the DPMI switch.

  ▶ **GH #18 IS STILL WANTED** (`INT3`/`HLT` still kill the VDM, so a raw PM trap is
    undeliverable) but it is **not** what stands between us and Doom.

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★★★ THE METHOD LESSON, AND IT IS THE EXPENSIVE ONE                           │
└──────────────────────────────────────────────────────────────────────────────┘

  I committed a handoff naming CLI/STI as the blocker on the strength of (a) a
  breakpoint that fired on an `STI` and (b) a skip test that moved the death by one
  byte. Both were *consistent* with "STI kills us"; **neither was evidence for it.** The
  control case (`IN`, already known to reflect) would have caught it in one run, and I
  only built the probe AFTER writing the wrong conclusion down.

  ▶ **BUILD THE INSTRUMENT THAT CAN SAY NO BEFORE YOU WRITE DOWN A YES.**
  ▶ Corollary, learned three times this session: **an instrument that faults kills the
    run it exists to observe.** `IsBadReadPtr` (faults on purpose), the breakpoint
    footprint eating a call target, `dpmi_bp_arm()` reading unmapped memory.

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★★ THE INSTRUMENTS. USE THEM BEFORE THEORISING.                              │
└──────────────────────────────────────────────────────────────────────────────┘

  **`pmbp.txt` on the share — guest breakpoints.** One line per breakpoint; absent file
  = zero cost. Up to 32.
  ```
    <hex LINEAR addr>  [dump addr]  [skip bytes]  [mode]  [repeat]   # comment
    00018d62 00005ca0                      # break, and dump that memory on hit
    00011ad2 00000000 00000001             # break, then STEP OVER 1 byte
    00011ad2 00000000 00000000 00000001    # plant a 1-byte INT3 instead of the BOP
    0000a8b4 00000000 00000000 00000000 00000001   # REPEATING (re-arms)
  ```
  * one-shot by default — right for "how far did it get", useless in a loop;
  * **repeat** re-arms via a pending flag once the guest is off the footprint, so put
    **at least two** repeating breakpoints in a loop and they alternate;
  * **skip** turns a breakpoint into a one-instruction patch ("would it survive if this
    simply did not happen?");
  * **mode 1** plants `CC` (INT3) — one byte, the only thing that fits over CLI/STI.
  ⚠ **A BREAKPOINT HAS A TWO-BYTE FOOTPRINT.** It displaces the byte AFTER the one you
    name. One placed on a `c3` (ret) ate the entry point of a routine called two
    instructions earlier; `call` landed on the second half of our BOP, decoded as
    `LES DX,[BX+0x8b]`, read past the segment limit and killed the VDM — and the log
    presented that as the CLIENT's death, mid-bisection. Overlaps are now refused and the
    footprint is printed. **Put breakpoints on instructions of at least two bytes.**
  ⚠ **DELETE `pmbp.txt` WHEN DONE** — a stale one silently alters every later run.

  **`tools/dostest/mkbp.py <carve.bin> <off> [--count N] [--base L] [--carve-off O]`**
  writes a sweep from a disassembly, skipping one-byte instructions.

  **`tools/dostest/pmfault.asm`** builds five ~350-byte clients — `pmfsti` / `pmfcli` /
  `pmfint3` / `pmfhlt` / `pmfin` — that enter PM, execute ONE privileged instruction and
  print a verdict. Twenty seconds against Doom's forty. **`pmfin` is the CONTROL CASE on
  purpose:** without it, "nothing happened" cannot distinguish a real wall from a broken
  test. Selected at ASSEMBLY time because `rt.bat` launches targets with no arguments.

  **Other knobs on the share:** `pmverbose.flag` (per-event checkpoint dump: 8 → 0x100000
  — the firehose, needed to find a last-known position), `pmnoirq.flag` (above).
  `LOG_MAX_BYTES` is now **32 MB** (was 4 MB, which truncated mid-startup).

┌──────────────────────────────────────────────────────────────────────────────┐
│ CLIENT LANDMARKS ALREADY MAPPED (save yourself the bisection)                │
└──────────────────────────────────────────────────────────────────────────────┘

  ```
    mod:0x84    PM interrupt dispatch TABLE: `call <common> ; db <vector>` per entry
    mod:0x550   the common dispatcher -- begins `LAR eax,SS` + `bt eax,22`, i.e. it
                tests the D/B bit of OUR stack descriptor to size its frame
    mod:0x4b60  the generic INT 21h thunk (register block in, `int 21h` at 0x4b7f,
                results written back; epilogue 0x4b81..0x4ba7 ends `retf`)
    mod:0x4ce   the 16->32 GATEWAY: cli / load 32-bit SS:ESP / jmp 0x691
    mod:0x691   ... rep movsl the frame, `mov ss,bx`, `mov esp,ebp`
    mod:0x6d5   the `IRETD` into the application  (entered 281 times)
    mod:0x4eb0  the AX=FF00 wrapper (`push 0xff00` at 0x4ed5, returns at 0x4edd)
    obj2:0x745  extender startup tail -> 0x797 -> 0x7a6 -> `jmp 0x823`
    obj2:0x823  `lcall <mod>:0x4ce`  = the call into 32-bit code
    obj2:0xb00a the FF00 caller's return point
  ```
  The unwind after the final FF00 (all breakpointed and hit, in order):
  `mod:0x4b81 → mod:0x4bc5 → mod:0x4edd → obj2:0xb00a → obj2:0x745`.

┌──────────────────────────────────────────────────────────────────────────────┐
│ 0. RESTARTING THE RIG (it is healthy and SELF-RECOVERABLE as of session 16)  │
└──────────────────────────────────────────────────────────────────────────────┘

  Every LAN command below needs `dangerouslyDisableSandbox` (the rig is not in the
  sandbox's allowed-hosts list, so a plain probe returns a FALSE "down").

  ```
  nc -z -w 3 192.168.1.29 445                                  # .29 is stable
  mkdir -p /tmp/xpshare
  mount_smbfs -N //guest@192.168.1.29/ntvdmex /tmp/xpshare     # remount after a reboot
  ```

  ★ **IS THE WATCHER ALIVE?** Do NOT test by "does watcher.txt exist".
    `runwatch.bat` writes it ONCE before `:loop` and again every ~3 s inside the loop.
    So the signature is:
      `watcher.txt` mtime ADVANCING + `controld.txt` advancing  → healthy
      `watcher.txt` FROZEN + `controld.txt` advancing           → **WATCHER DEAD**
    (frozen-but-controld-beating is exactly what a broken `goto` looks like; that is
     how session 16 found the LF-only Startup copy.)
    ```
    stat -f '%Sm %N' -t '%H:%M:%S' /tmp/xpshare/watcher.txt /tmp/xpshare/controld.txt
    ```

  ★★ **IF THE WATCHER IS DEAD, YOU CAN NOW FIX IT REMOTELY** — this was impossible
     before session 16 and cost a physical trip to the box:
    ```
    printf 'exec cmd /c "C:\Documents and Settings\All Users\Documents\ntvdmex\bm\runwatch.bat"\r\n' \
      > /tmp/xpshare/control.txt
    ```
    `controld` gained a generic `exec` (scripts/bm/controld.c). Running `bm\runwatch.bat`
    also re-installs a correct CRLF copy to Startup, so it repairs the cause too.

  **REBOOT** — two INDEPENDENT channels, on purpose, so each can recover the other:
    ```
    printf 'reboot\r\n' > /tmp/xpshare/control.txt      # via controld
    printf 'reboot\r\n' > /tmp/xpshare/cmd.tmp && mv ... cmd.txt   # via the watcher (rt.bat arm)
    ```
    ~75 s, then **umount + remount** (the mount goes stale across a reboot).

  **DRIVE A TEST — write `cmd.txt` ATOMICALLY, never with a plain `>`:**
    ```
    printf 'selftest.com\r\n' > /tmp/xpshare/cmd.tmp
    mv /tmp/xpshare/cmd.tmp /tmp/xpshare/cmd.txt
    ```
    Targets: a file in `bm\tests\`; a DIRECTORY on the share root = a "game" (runs
    `<name>.EXE`); `stock <target>` = STOCK NTVDM oracle; `doom` = C:\DOOMS\DOOM.EXE;
    `reboot`. `headless_ms.txt` caps a run (max 600000). Guest console output IS
    captured into the host log, so probes need no screen-reading.

  ⚠ **`cmd.txt` DISAPPEARING MEANS THE RUN *STARTED*, NOT FINISHED.** `runwatch.bat`
    deletes it BEFORE invoking rt.bat. I misread this twice in session 16 and read a
    half-written log. **The completion signal is `result_<target>.log` SIZE GOING
    STABLE** (poll it; 3 s apart, twice equal).

  ⚠ **DEPLOY THE RIGHT EXE.** `build/ntvdmhost.exe` (~420 KB) is the host;
    `build/ntvdmex.exe` (~20 KB) is the launcher and deploying it self-relaunches and
    leaves runs "succeeding" on a STALE log. Verify:
    ```
    cp build/ntvdmhost.exe /tmp/xpshare/bm/ntvdmhost.exe
    md5 -q build/ntvdmhost.exe /tmp/xpshare/bm/ntvdmhost.exe    # must match
    ./scripts/check-imports.sh build/ntvdmhost.exe              # XP-safe (no UCRT)
    ```

  ⚠ **`bm\rt.bat` EDITS NEED A REBOOT.** Only `runwatch.bat` at startup copies it to
    `C:\WINDOWS\rt.bat`, which is what the watcher actually runs.

  ⚠ **SMB attribute caching lies.** `stat` reported a log unchanged right after a run
    that HAD rewritten it; `ls -lt` on the directory showed the truth.

  ⚠ **Never edit a Windows .bat with Python text mode on macOS** — it strips CRs and
    `cmd.exe` then fails on `goto`/labels, i.e. the watcher loop. Read/write BINARY and
    normalise to CRLF, then confirm with `file`.

  BASELINE AFTER ANY RESTART: `selftest.com` → **8/8 PASS** (re-verified end of
  session 17, twice, with the final build).

  ★ **DISASSEMBLING THE CLIENT IS NOW A LOCAL, OFFLINE OPERATION** — this is what made
    session 17 short, and it costs one command to set up:
    ```
    printf 'exec cmd /c copy "C:\DOOMS\*.EXE" "C:\Documents and Settings\All Users\Documents\ntvdmex\doombin\"\r\n' \
      > /tmp/xpshare/control.txt
    ```
    Then map a guest address to a file offset by SEARCHING for bytes the log already
    dumped (`bytes@cs:eip=`), which pins the whole segment in one step:
    ```
    python3 -c "d=open('DOOM.EXE','rb').read(); print(hex(d.find(bytes.fromhex('919 8c3b0ff...'.replace(' ','')))))"
    # DOS/4GW's 16-bit half: guest 0x0F:off == file 0x1DD0+off
    # its runtime-loaded PM module: guest 0x8F:off == file 0xF384+off
    i686-w64-mingw32-objdump -D -b binary -m i8086 --start-address=0x... carved.bin
    ```
    ⚠ Pick a pattern with **no relocated immediates** in it — `mov di,<selector>` differs
      between file and memory, and a longer "safer" pattern that includes one will simply
      not be found.

┌──────────────────────────────────────────────────────────────────────────────┐
│ 2. NEW HARNESS — STOCK NTVDM IS NOW A FIRST-CLASS ORACLE (closes #26's gap)   │
└──────────────────────────────────────────────────────────────────────────────┘

  #26 left stock ntvdm unwired ("needs an rt.bat variant that drops the IFEO
  Debugger key, and a decision on the display-wedge risk"). Both done:
    • `bm\rt_stock.bat` — drops the IFEO Debugger value, runs the target, **restores
      it on every exit path** and writes `stock_state.txt` with `reg query` output
      proving it. Leaving that key absent is the failure worth fearing: every later
      test would silently measure stock ntvdm while the logs looked plausible.
    • `rt.bat` now dispatches `stock <target>` → `rt_stock.bat`, so the oracle is
      drivable **remotely**, like any other test.
    • Output is redirected to a file (`result_stock_<target>.txt`) — there is no
      host log under stock, and the window closes the instant the program exits,
      which is exactly how the first attempt lost its results.
  ▶ Display-wedge risk applies to GRAPHICS targets. Text-mode probes carry none.
  ▶ **This is the answer to "what does real DOS actually do?" for everything from
    here on.** It settled the keyboard question in one run after I had produced
    three wrong hypotheses from reasoning.

┌──────────────────────────────────────────────────────────────────────────────┐
│ 3. KEYBOARD — PINNED BY THE USER, IMPROVED 1-in-10 → 1-in-5, NOT FIXED        │
└──────────────────────────────────────────────────────────────────────────────┘

  Symptom: crash in Skyroads holding UP; the restarted level should accelerate, as
  it does on real DOS. **Ruled out by measurement:** the scancode FIFO (3205 pushes,
  ZERO drops, peak depth 6 of 32) and lock contention (max wait 0.57 ms, hold
  0.80 ms). Root cause was architectural — we modelled an AT keyboard but
  outsourced its REPEAT to Windows' message queue, and the UI thread stalls up to
  857 ms. We now generate typematic ourselves, pumped from both threads.

  ▶▶ **SESSION 16 — DO NOT JUST RETUNE THE RATE; THE MEASUREMENT IS AIMED WRONG.**
   • **Skyroads never programs its own typematic**: a 30 s run measured
     `kbd 8042: writes=0 typematic_set=0`. So OUR generated rate is the only source.
     (Caveat: that run only covered the ATTRACT LOOP — `int16=[0,0,0,0]`, `p60=0` —
     so it does not rule out a `0xF3` once gameplay starts.)
   • **`tymat.com` measures the INT 16h PATH ONLY** — deliberately, per its own header,
     because stock ntvdm may not grant raw 8042 access. But Skyroads reads IN-GAME via
     **INT 09h + port 60h**. So our verified 35.3/s says nothing about the path the bug
     actually lives on. That fits the otherwise-odd fact that making repeats 3.3x
     faster (92 ms → 28 ms) only moved failures from 1-in-10 to 1-in-5.
   • ⇒ **NEXT STEP IS AN INT 09h-LEVEL PROBE WITH A HELD KEY**, not a constant change:
     does a held key produce ~N makes/second at the guest's INT 09h handler under
     NTVDMEX? Counters already exist (`ty_sent`, `irq1_inj`, `sc_push`, `sc_drop`).
     Only once that is known does the SPI->ms mapping (still overshooting: ours 35.3/s
     vs stock 22.1/s, delay 500 ms vs stock 385 ms) become the right thing to fix.
  **STILL OPEN:** rate fidelity. Measured with `tymat.com` under both hosts:
        stock 385 ms / 22.1 per second      ours now 494 ms / 35.3 per second
  We **overshoot** — the documented SPI mapping ("31 ≈ 30/s") does not match what a
  DOS program observes under stock. Fix the mapping against the oracle, then
  re-test the actual restart behaviour, which is the only acceptance test that
  counts. Also still unanswered: **does Skyroads set its own rate via 8042 `0xF3`?**
  Those writes are now logged (`STAGE2: kbd 8042:`) but only a SKYROADS run answers
  it — `tymat.com` reports `writes=0` and that says nothing about the game.

┌──────────────────────────────────────────────────────────────────────────────┐
│ 4. ★★ TRAPS FROM SESSION 15 — EVERY ONE COST REAL TIME, SEVERAL WERE MINE     │
└──────────────────────────────────────────────────────────────────────────────┘

  1. **TWO EXEs, AND ONLY ONE IS THE HOST.** `build/ntvdmhost.exe` (~409 KB) is the
     DOS host — deploy THAT. `build/ntvdmex.exe` (~20 KB) is the launcher and is
     stale. Deploying the launcher makes `rt.bat` install it as the IFEO Debugger,
     and its job is to launch ntvdm.exe — which redirects straight back into it.
     **Runs still "succeed" in ~20 s because rt.bat copies a STALE log.** Tell: the
     same md5 for two different targets. Only a reboot cleared it. **CHECKSUM THE
     DEPLOYED BINARY AGAINST THE LOCAL ONE EVERY TIME.**
  2. **NEVER EDIT WINDOWS BATCH FILES WITH PYTHON TEXT MODE ON macOS.** It strips
     the CRs on read and writes LF. `cmd.exe` breaks on `goto`/labels first, which
     is the entire watcher loop. Read and write **binary**, normalise to CRLF. I did
     this to all three harness scripts and killed the watcher with it.
  3. **`$TMPDIR` DIFFERS INSIDE AND OUTSIDE THE SANDBOX.** A file written by a
     sandboxed command is not where an unsandboxed one looks. Use absolute paths, or
     patch the file on the share in place.
  4. **THE STALE-`TN` RACE (user caught this one).** `for /f ... do set TN=%%c` only
     assigns if the read yields a line; an SMB-empty `cmd.txt` left `TN` at its
     PREVIOUS value, so the watcher silently re-ran the last target. Queued
     `skyroads`, got `p_ver`, and the log looked entirely plausible. Fixed with
     `set TN=` + an empty check; **also write `cmd.txt` atomically via rename.**
  5. **THE WATCHER DIES WHEN THE HOST EXITS.** `runwatch.bat` ran the test with
     `call`, i.e. inside its own `cmd.exe`, and the host is linked
     `--subsystem,console` so it shares in console teardown. Now `cmd /c`, giving
     the test its own process. (Reported by the user; the earlier deaths predate my
     CRLF damage, so these are two separate causes.)
  6. **A CONSTANT LAG READS EXACTLY LIKE A WRONG WAVEFORM.** Both audio harnesses
     now report best-lag correlation. The bass drum scored -0.135 at lag 0 and
     **+0.999 at lag 4**.

┌──────────────────────────────────────────────────────────────────────────────┐
│ 5. ★★★ THE METHOD LESSON OF SESSION 15 — READ THIS BEFORE FIXING ANYTHING     │
└──────────────────────────────────────────────────────────────────────────────┘

  **I shipped a regression by reasoning where I should have measured.** I capped the
  PIT catch-up at 10 ms on the assumption that syncs are always closer together than
  that. They are not — `host_pit_sync` takes `g_lock` and a heavy I/O-trap loop
  starves the UI thread, which the code says in a comment I had already read. The
  user's verdict was immediate: *"speed is all over the place now... a definite
  regression."* The original burst was at least CORRECT ON AVERAGE; discarding time
  is a bigger and permanent error. **Reverted.** One counter would have refuted the
  assumption before it shipped.

  Then **three successive wrong hypotheses about the keyboard** — FIFO overflow,
  lock contention, and a remembered typematic rate — before the user said *"I'd be
  better testing the exact behavior in stock NTVDM."* That was better methodology
  than anything I had produced, and it settled the question in one run.

  ▶ **THE RULE, restated for a domain this file had not yet aimed it at:** the
    cardinal rule is not only about DOS API expectations. It applies to **hardware
    timing constants, repeat rates, and anything else you "know"**. Take it from an
    executable oracle. We now have three: MS-DOS 6.22 under QEMU, Nuked OPL3, and —
    new — **stock ntvdm on the rig itself**.

═══════════════════════════════════════════════════════════════════════════════
██ THE OPL TIMBRE FAULT IS FIXED (#21). WHAT IS LEFT IS THREE DRUM VOICES.   ██
═══════════════════════════════════════════════════════════════════════════════

╔══════════════════════════════════════════════════════════════════════════════╗
║ ▶▶▶ SESSION 15 (2026-08-21): "the instruments sound flat" IS CLOSED.         ║
║     Commits `94dc86f`, `fc6995b`, `8159d28` on `m9/completeness`.            ║
╚══════════════════════════════════════════════════════════════════════════════╝

**THE COMPLAINT WAS:** Skyroads plays "the right tune, but the instruments sound a
bit flat" / "melodic synths sound inaccurate". Same 90 s trace, same harness:

                          BEFORE      AFTER      (and at best alignment)
    waveform correlation  0.4119  ->  0.9114              0.9255
    envelope correlation  0.8680  ->  0.9732
    level ratio           1.628   ->  1.004
    RMS error              152%   ->   42%

**The melodic synthesis — the actual complaint — is at 0.96-0.97.** The per-segment
scores show it plainly: 0.962 and 0.971 over the first 20 s, before any percussion
enters. Everything below 0.92 after that is the three unimplemented drum voices.

┌──────────────────────────────────────────────────────────────────────────────┐
│ WHAT WAS WRONG. FIVE DEFECTS + TWO MISSING FEATURES, ALL MEASURED             │
└──────────────────────────────────────────────────────────────────────────────┘

  1. **MODULATION DEPTH WAS HALVED — this was the timbre fault itself.** An
     operator's output goes straight into the phase index, so full modulation
     swings the carrier FOUR whole cycles; we did two. Measured ratio 0.501, flat
     across the whole TL sweep. Halving the index changes neither pitch, tempo nor
     loudness — phase modulation preserves power — only WHICH harmonics exist.
     That is exactly "right tune, wrong instruments". Feedback was halved in the
     same place: our FB=n matched the reference's FB=n-1 step for step.
  2. **THE ENVELOPE NEVER STARTED FROM SILENCE.** `env` counts ATTENUATION, so a
     zeroed struct is FULL VOLUME. Key-on does not reset it (measured — the
     reference resumes an interrupted attack), so the first note of a run jumped to
     full level whatever its attack rate said. Attack measured 0.00 ms at EVERY
     rate against the reference's 1689 ms at AR=1.
  3. **THE RATE LAW HAD NO SUB-STEPS.** Speed is `(4 + rate_lo) / 2^(15 - rate_hi)`
     — linear 4:5:6:7 inside a group of four, doubling at the boundary. We shifted
     by whole octaves and rounded the mantissa away: mid-range decays 1.5x too slow.
  4. **KSL WAS OFF BY AN OCTAVE AND A FACTOR OF TWO.** The ROM is in 0.75 dB units,
     not 0.375, and the octave origin is 8, not 7 — together under-attenuating high
     notes by up to 18 dB, so bass and treble sat at the wrong relative levels
     across the whole keyboard.
  5. **AR=0 MEANT "INSTANT" INSTEAD OF "NEVER"** — turning silent voices into loud
     ones.
  6/7. **TREMOLO AND VIBRATO** implemented (0xBD was stored and never acted on).

  **Already correct, and now under regression:** total level (1.003 at full scale,
  0.75 dB/step to 0.01 dB), all four waveforms, all sixteen MULT settings, pitch.

┌──────────────────────────────────────────────────────────────────────────────┐
│ ▶▶▶ THE NEXT TASK: SNARE, HI-HAT AND CYMBAL. ~548+138+70 HITS PER SONG.       │
└──────────────────────────────────────────────────────────────────────────────┘

  Rhythm mode is implemented except for these three voices. They need the chip's
  **special phase generator**: a boolean function of bits taken from TWO phase
  accumulators, plus a noise source. **I deliberately did not guess it** — writing
  it from half-memory produces something plausible and wrong, and the harness would
  score it as an improvement because ANY sound beats silence. They are COUNTED
  instead and reported in STAGE2 as `NOT SYNTHESISED`.

  **EVERYTHING NEEDED TO DERIVE THEM IS ALREADY MEASURED** (`oplprobe rhythm`):

    voice       envelope   PHASE runs on   tonality   dominant component
    bass drum   op12+op15  own             1.000      ordinary 2-op FM   ✔ DONE
    tom-tom     op14       own             1.000      a plain sine       ✔ DONE
    snare       op16       **op13's**      0.509      H2 (2x op13)
    hi-hat      op13       op13 + op17     0.003      essentially noise
    cymbal      op17       op13 + op17     0.749      near Nyquist

    Percussion is summed at **DOUBLE amplitude** (tom-tom peaks 8170 where an
    ordinary operator peaks 4085). Verified per voice: tom-tom **+1.000**, bass
    drum **+0.999**.

  ★ **THE SUGGESTED METHOD, and it is tractable.** The cymbal's output is BINARY —
    its RMS equals its peak, at 0.707 of full scale — so its phase alternates
    between two values and **the output sign is a one-bit sequence you can read
    straight out of the oracle.** Extract that bit sequence, compute the candidate
    phase bits yourself (you control op13/op17 rates via MULT and F-num), and
    SEARCH over small boolean functions of those bits for the one that reproduces
    it. That is a derivation, not a guess, and it is a for-loop.

  ▶ **DO NOT MEASURE A NOISE VOICE WITH WAVEFORM CORRELATION.** Uncorrelated noise
    of exactly the right character scores 0. Judge hi-hat and snare on envelope
    correlation, RMS and the per-segment level instead.

  ▶ **AFTER THAT:** the user has not yet heard any of this. **The acceptance test is
    still the user's ears on the physical box.** Everything here is measured against
    a reference core, which is necessary and not sufficient.

┌──────────────────────────────────────────────────────────────────────────────┐
│ RIG STATUS AT THE END OF SESSION 15 — VERIFIED, AND LEFT CLEAN                │
└──────────────────────────────────────────────────────────────────────────────┘

  Rig `192.168.1.29`, share mounted, **new host deployed and VERIFIED on the
  physical box: `selftest.com` -> `==== ALL TESTS PASSED ====`.** Off-VM battery
  325/325. Host cross-builds clean and passes `check-imports.sh`.
  Share left clean: `headless_ms.txt` back to 30000, no cmd.txt, no control.txt,
  no flags. Watcher up, controld beating.

  ▶ **NOT DONE: a Skyroads run on the rig.** Two attempts at a 60 s cap produced no
    `result_skyroads.log` and left the watcher blocked in `rt.bat`'s `start /wait`,
    which needed a reboot to clear. The previous session used a **90 s** cap for
    this game; try that first. The rhythm counters are therefore verified as
    PRESENT (they print, correctly zero, on every run) but have not yet been seen
    counting real hits on hardware — the 548/142/138/70 figures come from replaying
    the captured trace offline, which is solid but is not the same evidence.

  ★★★ **THE TRAP THAT COST TWO REBOOTS, AND IT WILL CATCH ANYONE: THE BUILD
      PRODUCES TWO EXEs AND ONLY ONE OF THEM IS THE HOST.**
        build/ntvdmhost.exe   ~409 KB   ** THIS is the DOS host — deploy THIS **
        build/ntvdmex.exe      ~20 KB   the launcher; it is not rebuilt and is stale
    I copied `ntvdmex.exe` over `bm/ntvdmhost.exe`. `rt.bat` then installed it as
    the **IFEO Debugger for ntvdm.exe** — and that binary's job is to LAUNCH
    ntvdm.exe, so every launch redirected straight back into it. The box wedged.
    ▶ **The symptom is deeply misleading:** runs still "complete" in ~20 s and
      `rt.bat` faithfully copies `C:\ntvdmex\ntvdmhost.log` to `result_<target>.log`
      — so you get a plausible log for a run that never happened. **It was the OLD
      log every time.** `controld kill` did not clear it; only a reboot did.
    ▶ **HOW TO TELL, in one command:** checksum the result against a known previous
      log. `md5 result_p_ver.com.log result_skyroads.log` returning the SAME hash
      for two different targets is the tell. Grepping the log for something only
      your new build prints is the other.
    ▶ `result_selftest.log` on the share is a **stale copy of a Skyroads run** left
      by this. The real one is `result_selftest.com.log` — `rt.bat` resolves targets
      out of `bm\tests\`, so bare `selftest` matches nothing and silently falls
      through to copying the previous log.

┌──────────────────────────────────────────────────────────────────────────────┐
│ THE METHOD — NUKED AS A BLACK-BOX ORACLE. THIS IS NOT OPTIONAL CEREMONY.      │
└──────────────────────────────────────────────────────────────────────────────┘

▶ **DO NOT READ `build/oplref/opl3.c` FOR CONSTANTS.** Nuked OPL3 is **LGPL-2.1**;
  `vdd_opl_synth.c` is deliberately clean-room MIT ("written from the documented
  YM3812 behaviour rather than ported from an existing core, so it is ours and
  MIT-clean"). Reading it forfeits that, and the loss is not limited to the line you
  looked at: some constants are FORCED by the hardware (one valid value, no exposure)
  and others are the implementation's own CHOICES (structure, edge cases, rounding) —
  **and you cannot tell which is which by looking.** Reading contaminates you for all
  of it. The user was asked and chose to keep the MIT posture.

▶ **DO use it as an ORACLE:** controlled input -> observe output -> derive the value.
▶ **DO read public, non-LGPL documentation** for the expectation: OPL2/OPL3 datasheets
  and the public hardware write-ups. This is the project's cardinal rule pointed at a
  new domain — **docs for the expectation, executable oracle for the verification.**
▶ **Converging on the same constant Nuked uses is EXPECTED and FINE.** Clean-room is
  about provenance, not divergence; the Phoenix BIOS was functionally identical by
  design. (Engineering practice, not legal advice.)
▶ The usual objection is "clean-room is the long way round". Normally yes — two teams,
  months. **Here it is a for-loop**: the harness scores automatically, so a sweep is
  minutes. The saving from reading the source is small.

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★ THE SINGLE-NOTE RIG — `oplprobe`. THIS IS WHAT MADE THE SESSION SHORT.      │
└──────────────────────────────────────────────────────────────────────────────┘

  `./tools/oplref/build.sh` now builds TWO binaries. **`oplcmp` proves the timbre is
  wrong; it cannot say WHICH parameter**, because every note of a game trace moves
  every variable at once. `oplprobe` does the opposite: holds one channel still,
  moves ONE register, and reports a DERIVED PHYSICAL QUANTITY for both cores.

      ./build/oplref/oplprobe <experiment>     # or `all`
        validate  silence/determinism/pure-tone/pitch -- RUN THIS FIRST, ALWAYS
        tl        total level: full scale and dB per step
        mod       MODULATION INDEX in radians -- the prime suspect, fitted from
                  the sideband amplitudes via a Bessel fit
        fb        feedback          wave  the four waveforms     mult  all sixteen
        ksl / kslrom   the whole block x F-num surface, and the ROM read back
        env       attack/decay/release times     egrate  the RATE LAW, derived
        attack    the attack CURVE (geometric), fitted against the decay slope
        lfo       tremolo/vibrato rate, depth AND SHAPE
        rhythm    maps percussion from the outside: which operator, whose phase

  ▶ **EVERY CONSTANT IN `vdd_opl_synth.c` NAMES THE EXPERIMENT THAT PRODUCED IT.**
    Re-derive rather than argue; a sweep is seconds.

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★★ FOUR TRAPS IN THE INSTRUMENT ITSELF. ALL FOUR PRODUCED CONFIDENT NONSENSE. │
└──────────────────────────────────────────────────────────────────────────────┘

  Every one of these was MY measurement being wrong, not the synth — and each
  looked right at the time. This is the same lesson the rest of this file keeps
  teaching, pointed at a new domain.

  1. **A "PURE TONE" THAT WAS NOT PURE.** A modulator parked at TL=63 is only
     -47 dB, not silent, and it still bends the carrier: 5% THD. Fine for a level
     reading, fatal for a distortion one. **Park the unwanted operator on ANOTHER
     HARMONIC (MULT=12)** so its residual cannot reach the bins being measured.
  2. **THE QUANTISATION FLOOR.** Fitting a decay slope over points near zero
     amplitude — where the 16-bit output has stopped moving — reported a rate 8%
     too slow AND a beautifully constant anchor column that made it look correct.
     Only when I raised the floor did the divisor snap to exactly 2^15.
  3. **A CONSTANT LAG READS EXACTLY LIKE A WRONG WAVEFORM.** The bass drum scored
     **-0.135 at lag 0 and +0.999 at lag 4** — identical waveform, four samples
     apart. The reference is cycle-accurate and reaches its output a few samples
     after the write that caused it. **Both harnesses now report best-lag
     correlation**; without it I would have hunted a defect that does not exist.
  4. **AN UNRESOLVABLE READING IS NOT EVIDENCE.** The vibrato sweep printed
     "121 cents" at F-num 0x100 — a pitch whose test note has too few zero
     crossings per window to resolve a 7-cent shift. That reading is now DELETED
     from the sweep rather than reported, because a number with no precision behind
     it is worse than no number.

┌──────────────────────────────────────────────────────────────────────────────┐
│ THE TOOLING — BUILT AND WORKING. USE IT, DO NOT REBUILD IT.                   │
└──────────────────────────────────────────────────────────────────────────────┘

  **1. CAPTURE a register trace from a real run** (host writes it itself):
      : > /tmp/xpshare/opltrace.flag            # the knob; absent = zero cost
      printf '90000' > /tmp/xpshare/headless_ms.txt
      rm -f /tmp/xpshare/result_skyroads.log /tmp/xpshare/opltrace.txt
      printf 'skyroads\r\n' > /tmp/xpshare/cmd.txt
      # wait for result_skyroads.log to appear, then:
      cp /tmp/xpshare/opltrace.txt build/oplref/skyroads.txt
      rm -f /tmp/xpshare/opltrace.flag          # ALWAYS clear it afterwards
    Format: one `us reg val` per line, hex. Timestamps are the guest's REAL write
    times, so a replay reproduces its phrasing. Hook is `opl_state.trace`, set by the
    host only when the flag exists, so the VDD stays pure C.
    ► ★★ **NEVER end a trace run with controld `kill`.** It is `taskkill /f`; the
      trace and the whole STAGE2 block are written during the CLEAN wind-down, so a
      killed run yields a log file with the useful half missing. **It looks like it
      worked.** Let the headless cap expire, or quit the guest from its own menu
      (rt.bat sets `autoexit`). This cost the user replaying Skyroads twice.

  **2. BUILD the comparison harness** (pulls the reference out-of-tree on demand):
      ./tools/oplref/fetch.sh      # -> build/oplref/opl3.[ch]  (gitignored)
      ./tools/oplref/build.sh      # -> build/oplref/oplcmp

  **3. COMPARE:**
      ./build/oplref/oplcmp build/oplref/skyroads.txt build/oplref
    Prints the metrics above and writes `ours.wav`, `ref.wav`, `envelope.csv`.
    **`ref.wav` is what Skyroads should sound like; `ours.wav` is what we produce.**

┌──────────────────────────────────────────────────────────────────────────────┐
│ THE PLAN, IN ORDER                                                            │
└──────────────────────────────────────────────────────────────────────────────┘

  **0. VALIDATE THE HARNESS FIRST (~30 min). Do not bisect against an unverified
     instrument.** It has ALREADY produced one artefact: the reference was driven
     with `OPL3_WriteRegBuffered` (a realtime write-latency queue) and that alone
     cost 0.17 vs 0.41 waveform correlation — **a quarter of the apparent defect was
     mine, not the synth's.** Sanity checks worth doing: silence in -> silence out in
     both; a single pure tone matches; the same trace twice is bit-identical.

  **1. Build the SINGLE-NOTE experiment (~30 min).** A hand-written trace: one
     operator pair, one sustained note, ONE variable moving. A game trace proves the
     timbre is wrong; one note with one variable NAMES THE PARAMETER. This is what
     turns a multi-hour search into a short one.

  **2. Derive the constants, in this order** (each is a sweep scored automatically):
     a. **Attenuation / TL mapping** — one operator, no modulation, sweep TL 0..63,
        measure output amplitude in both cores. Should settle the 1.628 level ratio
        on its own.
     b. **Modulation index scaling** — two operators, fixed carrier, sweep modulator
        TL, compare sideband amplitudes. **PRIME SUSPECT for the timbre.**
     c. **Feedback scaling** — one self-modulating operator, sweep FB 0..7.

  **3. THEN the two genuinely missing features** (both currently no-ops; `0xBD` is
     stored in `reg[]` and never acted on — see `vdd_opl.c`):
     a. **Tremolo/vibrato LFOs.** MEASURED as genuinely used: **103 notes start with
        tremolo and 149 with vibrato out of 982**. Those counters are per-note EDGES,
        so they are trustworthy. ~2-3 h.
     b. **Rhythm mode** — the 5 percussion voices. **ONLY IF PROVEN NEEDED**, see the
        correction below. ~half a day.

  **ACCEPTANCE:** beat the baseline above — waveform correlation toward 0.9+, level
  ratio toward 1.0 — and the user confirms by ear on the rig. Every synth change now
  has a regression score, so never change it without re-running `oplcmp`.

  **ESTIMATE GIVEN (honest, wide because the parameter is not yet identified):**
  ~half a day if it is a scaling constant; up to two days if it is structural (our
  1/96 dB, 8.8 fixed-point envelope vs the chip's exact 9-bit attenuation pipeline)
  and all gaps are closed.

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★ A WRONG CONCLUSION I REACHED — DO NOT REPEAT IT                             │
└──────────────────────────────────────────────────────────────────────────────┘

  I added register counters, read `bd_or=0xFB` + 888 writes to `0xBD`, and announced
  that Skyroads drives ~888 percussion hits and our missing rhythm mode was the cause.
  **That was wrong, and the user — who knows the game — corrected it: the music is
  mostly melodic synths.**
  ▶ **The flaw was the instrument.** `bd_or` is an **OR across the whole run**, so it
    cannot distinguish "these bits were set once during init or a silence-all reset"
    from "drums play throughout". It is a weak assertion dressed as evidence —
    exactly the trap already recorded in this file, in a new costume.
  ▶ **The second trace makes it starker:** 2144 of 4243 writes go to `0xBD` against
    only 545 notes — **~4 writes per note**, which is a driver hammering the register
    in its update loop, not drum triggering.
  ▶ **Trustworthy counters** in the same STAGE2 line are the per-note EDGE ones:
    `keyon_am`, `keyon_vib`, `keyons`. Use those; treat every `_or` field as a hint.
  ▶ **The lesson generalises:** a counter can tell you a feature was TOUCHED. Only a
    waveform comparison can tell you a feature SOUNDS WRONG. That is why the harness
    exists and why it should be the first thing you reach for.

  ▶ WHERE THE CODE IS: `src/vdd/vdd_opl.c` (device + registers, the profile counters),
    `src/vdd/vdd_opl_synth.c` (**the FM synthesis — the thing to fix**),
    `src/vdd/opl_tables.h` (log-sin / exp tables), `tools/oplref/` (harness, in tree),
    `build/oplref/` (gitignored: the reference core, the binary, the WAVs, the CSV).

▶▶▶ **SESSION 14 (2026-08-20, late): THE MODE-12h WALL IS DOWN. #55 IS CLOSED.**
  BLIT.EXE renders 640x480 16-colour filled boxes on the physical box, matching
  `build/shots/demos/oracle_blit.png` in kind (the boxes are random per run).
  Frames in `build/shots/p12/`. Commit `b3abbce`.

  ★ **THE MOVE THAT WORKED WAS TO STOP TRYING TO INTERCEPT THE WRITES.** Session 13
    had already measured the answer and framed it as a question about interception:
    with the A0000 page trap off the guest runs perfectly (22.5M I/O events) and the
    only defect is that its planar writes bypass the VGA engine. The unexamined
    assumption was that we therefore had to SEE those writes. We don't — we can
    simply BE the CPU that performs them. While a planar mode is set the guest now
    runs in the host interpreter, whose A0000 accesses go through the planar write
    engine by construction (`imem_r8`/`imem_w8`). No page protection, no kernel RE,
    no VDD memory hook. **The planned next step — disassembling XP's VDM memory
    handling — was not needed and was not done.**

  ★ **THREE BUGS HAD TO DIE FIRST, and two of them were silent corrupters:**
    1. The interpreter could not survive an interrupt: no `INT nn`, no `IRET`, no far
       `JMP`/`CALL`. It stopped at the first DOS call and handed the guest back to
       V86 — precisely where the writes become invisible. Now modelled, vectoring
       through the real IVT. **`LES` (`C4`) stays unmodelled ON PURPOSE**: bailing on
       it is how a BOP still reaches the kernel. Modelling it would swallow every
       DOS/BIOS call in the system.
    2. **`host_interp` never wrote the SEGMENT registers back.** It modelled `POP ES`
       and `MOV DS,AX` and then threw the result away, so the guest resumed with the
       segment it had BEFORE the batch and the offset the batch had reached. Harmless
       while batching was confined to a fill loop that reloads nothing; fatal the
       moment CS changes on every interrupt.
    3. **`POPF` masked `IF` out of the flag image**, so every interpreted `POPF`
       silently disabled the guest's interrupts.

  ★ MEASURED, same build, same program, one policy switch:
        page trap    io_events=0x1d       plane-nonzero = 0/0/0/0        frozen
        interpret    io_events=0x50d4e6   plane-nonzero = 1f5b/389e/79c6/25f8
    575M instructions interpreted, 11 captured frames. `p12off.flag` reverts to the
    page trap. STAGE2 reports `p12-batches/instrs/bails`, and **every opcode the
    interpreter declines is named (`P12-BAIL`)** — that list is the to-do list for
    this path, and it is how you tell "we ran it" from "we lost the guest to V86".

  ★★★ **THEN ALL TEN WERE WATCHED LIVE, ONE AT A TIME, AND ALL TEN RENDER CORRECTLY.**
    Full write-up + every observation: **`docs/research/demo-sweep-findings.md`** — read
    that before touching video or timing. Headlines:
      • **NOT ONE PIXEL DEFECT.** Every defect found is TIMING.
      • **The retrace bit (0x3DA) is untimed** — we toggle it on every read, so
        `WAIT &H3DA,8` returns instantly and anything that paces on vblank runs
        unbounded (BOUNCEBX, MATRIX_2, CAVE). **CAVE proves this is PRE-EXISTING and
        not ours: it is SCREEN 13, which never touches the interpreter.** Re-check
        Skyroads against it — its "a little sluggish" calibration predates knowing this.
      • **USER FEATURE REQUEST, and it is load-bearing: a menu dropdown for approximate
        CPU speed** (33/66/100/200 MHz). Two of the five speed-affected demos pace
        themselves with a busy-wait or not at all, so NO retrace fix can ever reach
        them. See finding #3 for implementation notes across both execution paths.
      • **MOUSE was NOT a defect** and neither was INT 33h. The oracle ran the same
        binary on genuine MS-DOS 6.22: it exits in 2.9 s there too. `mousetst.com` then
        proved INT 33h works end to end — cursor tracks, left button draws
        (`build/shots/mousetst_live.png`). The only real item is cosmetic: our arrow is
        hand-drawn and the user has a 16×16 cursor to swap in.
      • Three readings were WRONG and corrected by evidence mid-sweep (see the method
        note at the end of that file). Every one of them looked obviously right.

  ★ **THE DEMO SWEEP RAN: ALL TEN QuickBASIC DEMOS, ALL TEN DRAW.** Six SCREEN 12,
    three SCREEN 13, one SCREEN 0; `video modes unsupported: none` everywhere.
        BLIT      16-colour random filled boxes -- matches the oracle in kind
        MATRIX_1  full-screen Matrix rain, glyphs + green ramp  ← the strongest one
        MATRIX_2  same, sparser (planes 0b04/0b21/0b47/0bed)
        BUBBLES   greyscale starfield. **The greys are CORRECT** -- BUBBLES.BAS sets
                  its own `PALETTE index, index*4` ramp, so this also proves the
                  palette path. Do not "fix" it into colour.
        BOUNCEBX  40x40 filled box, caught mid erase-redraw (planes b4/00/b4/00)
        MOUSE     sets 12h, returns to mode 3 and exits in ~3 s -- ONE shot, and the
                  only demo whose behaviour is not yet explained. Look here first.
        CAVE / GFXCOPY / PALETTE (13h) and VS87 (text) unchanged -- no regression.
    Frames: `build/shots/p12/`. Screenshots need `capture.flag` on the share; it was
    deleted afterwards, as it must be.

  ▶ **WHAT IS STILL OPEN HERE:** every bail is a stretch of guest execution running
    on the real CPU with its A0000 writes going nowhere. BLIT had ~5.3M of them
    against 515k batches, so the picture is right but not provably complete. Work the
    `P12-BAIL` list before claiming planar parity.

  ▶ THE PAGE-TRAP FREEZE ITSELF IS STILL UNEXPLAINED and is now a curiosity rather
    than a blocker. If it is ever picked up: the exec thread does not return from
    `VdmStartExecution` at all (the TIB's CS:IP stays frozen at whatever the last
    event left it — 0050:0037, the `CD 1C` in our INT 08h stub, is a STALE reading,
    not where the guest is). Do not read it as "the guest is in its timer handler".


▶ **READ IN THIS ORDER:** (1) this block, (2) the session-13 checkpoint below — it holds the
  measurements, the ruled-out list and the traps, (3) **GitHub epic #24** for the programme's
  standing policy, and **#55** for the task in front of you.

▶ **M9 STATUS: INT 21h 103/103, BIOS complete, all 15 probes clean against the oracle panel.**
  17 sub-issues closed, 9 raised (#47-#55), 5 left open with a comment stating exactly what
  remains. Verified at host `v180`: selftest 8/8, off-VM battery 325/325.

▶ **THE STANDING PRINCIPLE (user, 2026-08-19), unchanged:** *"There should be no cause in
  NTVDMEX itself to fail — whatever we throw at it should just work."* The achievable form is
  **no SILENT failure**: every unimplemented thing announces itself. That is now built in — a
  run ends with a to-do list (`STAGE2: INT21 unimplemented:` and friends), and it is how the
  whole evidence pass was driven.

▶ **THE CARDINAL RULE, and it has now earned its keep three times over:** *never write a test
  expectation from memory of what DOS does.* Take it from RBIL **confirmed against the oracle**.
  Refuted from memory this session: #27's own headline instruction (real DOS sets NEITHER AX
  nor CF on an unhandled call), the find-first "not found" code (18, not 2), and the FCB
  convention (result in AL; **carry is undefined** — a successful open returns CF=1). Each
  would have made us *less* accurate while looking like a fix.

▶ **THE DECISION THAT SHAPED THE SESSION (user, 2026-08-20):** completeness before breadth,
  then push for 100%, then EXEC, then the demos. All delivered except the demos, which are
  blocked on mode 12h.

▶ **WHERE THE REAL FRONTIER IS NOW: COVERAGE IS NOT CORRECTNESS.**
  We have 100% of the documented API, oracle-matched. We have **application** evidence for
  exactly one game (Skyroads, mode 13h) and four command-line tools. Two independent things say
  that is not the same as working:
    • **#47** — MEM.EXE reports nothing missing and prints WRONG NUMBERS. The failure mode this
      epic exists to remove, surviving *because* the coverage is complete.
    • **#55** — mode 12h has never rendered. Six of the ten QuickBASIC demos need it.
  **Next milestone should be measured in APPLICATIONS THAT BEHAVE CORRECTLY, not functions
  implemented.** The bar remains Doom / Skyroads / ZAR.

▶ **THE ACCEPTANCE BAR:** Skyroads is fully playable (menus, gameplay, sound, text — confirmed
  on the physical box, sessions 11-12). Doom and ZAR remain; both are DOS/4GW, so they sit
  behind the DPMI workstream, not this one.

▶ **TEST TIERS — put each test in the right one:** off-VM C battery (`tools/dostest/run.sh`,
  **325 checks**, runs on the Mac in seconds) for anything that is pure logic; a guest `.COM`
  through **`scripts/dosdiff.py`** for anything guest-observable; the rig (`selftest`, 8/8) as
  the final gate. selftest is a SMOKE TEST at ~2 min a round — it is NOT the TDD loop.
  ► The fast loop is now the **oracle** (`./scripts/oracle.sh <probe>.com`, ~3 s, offline).

▶ DEFERRED BY DECISION — DO NOT PICK UP UNASKED: keyboard/music latency (user rates Skyroads
  "genuinely playable, a little sluggish"; lag is in the milliseconds). Also queued: hardware
  grounding (CPU affinity, SpeedStep), which lands on our timing path since guest clocks come
  from QueryPerformanceCounter.
```
