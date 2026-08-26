# Session 21 — 2026-08-24

> Archived verbatim from `return-ntvdm.md`, which was the single rolling handoff
> file until 2026-08-26. Split by session so it can be read by topic and date
> rather than by scrolling. **Nothing has been edited** — including conclusions
> that later sessions REFUTED, which are kept on purpose: the refutations are
> some of the most valuable material here.

```
═══════════════════════════════════════════════════════════════════════════════
██ ▶▶▶ SESSION 21 (2026-08-24). Written MID-SESSION; session 22 above continues ██
██     the same day and supersedes the "NEXT SESSION" list at its end.          ██
██     **DOOM PLAYS ITS DEMO, WITH SOUND, AND THE                              ██
██     MENU OPENS.** THE FIVE-SESSION `R_ExecuteSetViewSize` DEATH WAS OURS: ██
██     THE INT-SITE PATCHER WAS CORRUPTING A `jle` DISPLACEMENT.             ██
═══════════════════════════════════════════════════════════════════════════════

  Branch `m9/completeness`. Commits `9435485`, `46462d5`, `ebd1bd3`.
  Gates GREEN on the final binary: off-VM **629 checks / 18 suites, 0 failed**;
  check-imports pass; bare-metal `selftest.com` 8/8; `dpmitest.com` 0300/0301/0303 +
  nested INT 31h clean; `dpmiback.com` clean (its `<<< MISMATCH >>>` is the
  documented benign sentinel -- do not chase it).

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★★★★★ WHAT WORKS NOW, WITH NO GUEST PATCH OF ANY KIND                        │
└──────────────────────────────────────────────────────────────────────────────┘
```
   startup      every line stock ntvdm prints, through I_StartupSound /
                I_StartupTimer / D_CheckNetGame / S_Init / HU_Init / ST_Init
   video        the attract DEMO renders -- full 3D, 320x200 unchained (mode Y),
                status bar, weapon, sprites. Confirmed on the physical screen.
   PCM          sb_blocks=0xe27 at 11025 Hz through the SB + DMA into the mixer,
                host_wave=open, underruns=0
   MIDI         midi_msgs=0x2d8 out of the MPU-401 to XP's GS Wavetable synth,
                host_midi=open
   keyboard     12 scancodes pushed -> p60=12 read, 0 dropped, 0 stranded.
                ESC opens the MENU over the demo; it closes again.
```
  ⚠ `pmbp.txt` IS NO LONGER NEEDED AND MUST BE ABSENT. Session 19/20's
    `03aed1fe ... 5` skip is obsolete -- it papered over the bug fixed here.

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★★★★★ THE BUG. IT WAS OURS, AND IT HAD BEEN THERE ALL ALONG.                 │
└──────────────────────────────────────────────────────────────────────────────┘

  `dpmi_patch_code_region()` rewrites `CD nn` -> BOP because a raw protected-mode
  INT is the one fault XP will not reflect. It matched the BYTE PAIR, with no idea
  where instructions start. At obj1+0x3593f Doom's code is
```
     39 fa   7e cd   31 c9      cmp edx,edi / jle -51 / xor ecx,ecx
             ^^ ^^^^^^^
```
  The `cd` is the **jle's displacement**; the `31` is the **xor's opcode**. Patched,
  `7e c4  c4 c9`: loop 2's back edge jumped to obj1+0x35905 -- the middle of a `jl` --
  and the guest ran `cmp ecx,[ebx+0x034fe02d]` with an ANGLE in ebx. Wild read, #PF at
  CPL 3, VDM torn down silently.
  Two more sites were being corrupted the same way, unnoticed: obj1+0x0ae0f (a
  `call rel32` displacement) and obj1+0x0512d (a word in a data table).

  ★ **SESSION 20'S "IT IS THE STRETCH LENGTH" IS REFUTED.** The skip "saved" the run
    because it stopped the corrupted jump being TAKEN. This session's own instruments
    killed the theory outright: a **27 ms** BOP-free stretch survived earlier in
    startup (`PMSTRETCH`), and the async injector never touched loop 2 at all
    (`ASYNC-SITE`: six distinct sites in a whole run, every one in the ms-delay spin).

  **THE FIX — `src/host/x86len.h`.** An x86 instruction-LENGTH decoder (16/32-bit) plus
  a boundary test: decode forward from each of the preceding 48 bytes, count how many
  streams land on the candidate. Patch if it is a confirmed instruction start; reject
  ONLY when a confirmed instruction covers it AND that instruction is a RELATIVE BRANCH.
  ⚠ **A STRICTER RULE WAS TRIED AND IS WRONG. DO NOT "TIGHTEN" THIS.** "Reject anything
    a confirmed instruction covers" also rejects DOS/4GW's `mov ah,30h / int 21h`
    version check, which sits right after the string `"requires DOS/16M\n\r$"`: every
    backward anchor decodes ASCII, so it scores 1 vote in 48 -- by votes alone
    indistinguishable from Doom's `jle` at 3 in 48. Refusing it left a raw INT 21h in PM
    and killed the run inside the extender's startup, 54,000 log lines EARLIER.
    ▶ **THE TWO ERRORS ARE NOT SYMMETRIC**: a spurious patch usually costs nothing, a
      missed real site is instant death. Lean toward keeping.
  ▶ The region's D/B bit is now plumbed from the descriptor. Decoding DOS/4GW's 16-bit
    modules as 32-bit rejects obvious real sites; the scan is idempotent, so a site
    refused under a wrong width gets another chance on the next pass.
  ▶ Validated against objdump over 242 candidate byte pairs in three images; pinned by
    `tools/dostest/x86len_test.c` in the off-VM battery.

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★★★★ THE OTHER FIVE, ALL FOUND BY MEASUREMENT, ALL OURS                      │
└──────────────────────────────────────────────────────────────────────────────┘
```
 1 TIMER 27x FAST      DPMI_IRQ0_BATCH injected a fixed 64 ticks per async return
                       with no reference to elapsed time: 169,032 ISR entries in 45 s
                       against the 6,300 Doom asks for at 140 Hz. g_irq0_pending
                       cannot answer "how many are owed" (it saturates at 4 on
                       purpose), so the batch drains g_pm_tick_owed now.
 2 ABANDONED ISR       dpmi_inject_pm_irq() gave up after 64 phases and restored the
   (this froze the     interrupted context, leaving the client's handler part-run.
    clock)             ONCE per run, and fatal: it leaked DMX's dispatcher depth
                       (4->3) and one 4KB frame of its private interrupt stack,
                       permanently. ticcount froze at 0x61, I_GetTime() stopped,
                       TryRunTics() spun forever -- "the title renders and nothing
                       ever happens". A PHASE IS NOT A UNIT OF TIME: it is one PM
                       entry, and the MIDI driver pays ~11 per byte written. Bound by
                       WALL CLOCK (500 ms); log loudly if we ever stop early.
 3 "SB isn't           The card answered its reset with 0xAA and reported DSP 4.05.
   responding"         What failed was DSP command 0xF2 -- "assert your interrupt so
                       I can find your line". async_inject_irq() decided "has the
                       guest hooked this line?" from the REAL-MODE IVT only, and a
                       DPMI client hooks the PM vector. Ask both tables.
 4 NO PM KEYBOARD      IRQ0 has had a cooperative PM path since #2b; IRQ1 had only
   PATH                the async injector, which gets ONE attempt per keystroke.
                       The PM loop now offers a pending IRQ1 every pass.
 5 PENDING CLAIMED     ...and the count was decremented AFTER the handler ran, while
   TOO LATE            the handler itself re-raises (the ISR reads 0x60 from inside
                       the injection and the 8042 re-asserts). That cancelled the
                       interrupt the ISR had just raised: three bytes delivered,
                       scleft=1, keyboard dead for the rest of the run. Claim first.
```

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★★★ NEW INSTRUMENTS — every one of them earned its place this session        │
└──────────────────────────────────────────────────────────────────────────────┘
```
   ASYNC-SITE     each distinct PM CS:EIP the async injector finds the CPU at, ONCE.
                  The guest's timer runs at 16 kHz, so a per-attempt line is
                  unreadable and a capped one goes quiet before the interesting part;
                  deduping by SITE is neither. The observation pass RESUMES the thread
                  and returns WITHOUT injecting, so the line is on disk before
                  anything is rewritten.
   PMSTRETCH      wall-clock of each dpmi_enter_pm(), new maxima only. This is what
                  refuted "the stretch is too long".
   SNDIO          the sound-card conversation (SB / OPL / MPU), first 300 accesses.
                  ⚠ io_hot_note() is only called on the V86 arm, so STAGE2's "hot
                    ports:" is EMPTY for a PM client. It is not a measurement.
   KEYIRQ/KEYPM   every keyboard IRQ raise and every PM delivery attempt, with all
                  five gate values. Bounded; a run with no keys pays nothing.
   STAGE2 sound   sb_blocks/sb_rate/midi_msgs/mpu_uart AND whether the HOST wave and
                  MIDI devices opened -- a silent run with a happy guest and a silent
                  run with no device look identical from the guest's side.
   pmwatch.txt    up to 4 linear addresses, dumped on every IRQ0<-PM line. This is
                  what caught the frozen clock. Addresses came straight out of Doom's
                  disassembly: I_GetTime is `mov eax,ds:0x2913c / ret`, DMX keeps its
                  dispatcher depth at 0x283e8 and its stack at 0x283f0 (obj3-relative).
   capture.flag   its CONTENTS are now the screenshot period in ms. 40 frames at the
                  old fixed 300 ms only saw the first TWELVE SECONDS, so a run whose
                  first scripted key is at 14 s looked like "the keys did nothing".
   keys.txt       scripted scancodes for the synthetic-key thread (qimode bit 5,
                  `printf '20' > qimode.txt`). Format: `w<ms>` waits, `e` = extended
                  prefix, two hex digits = a make/break pair.
                  e.g. `w15000 01 w3000 e50 w2000 e50 w2000 e48 w6000 01 w5000`
```

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★★★ METHOD — SESSION 21 PAID FOR THESE                                       │
└──────────────────────────────────────────────────────────────────────────────┘

  ▶ **THE CALLED-OFF SUSPECT WAS THE CULPRIT'S NEIGHBOUR.** Five sessions looked for a
    fault in Doom because the death was *inside Doom's code*. Nobody asked what WE had
    written into that code. **When a guest dies at an address, diff the bytes there
    against the file on disk.** One `DPMI-BP: armed at ... (displaced 7e c4)` line --
    where DOOM.EXE says `7e cd` -- was the whole answer, and it was free.
  ▶ **TWO REPEATING BREAKPOINTS IN A LOOP RE-ARM EACH OTHER.** Session 20 recorded that
    `rep` cannot re-arm inside a BOP-free stretch and reached for `skip` instead. True
    for ONE breakpoint (the re-arm refuses while the guest stands on the footprint);
    false for two, which alternate. That is what made the loop-2 probe possible.
  ▶ **A COUNTER THAT SATURATES CANNOT BE A BACKLOG**, and a fixed batch is not catch-up.
  ▶ **NEVER ABANDON A GUEST HANDLER MIDWAY.** It is not a timeout, it is a silent state
    corruption, and the damage surfaces thousands of events later somewhere else.
  ▶ **AN INSTRUMENT THAT HARDCODES WHAT IT IS REPORTING IS WORSE THAN NONE.** ASYNC-PM
    printed "vec=0x08" literally, whatever it had delivered, so the log could not be
    asked whether a key had ever arrived.
  ▶ **CLAIM BEFORE YOU RUN, NOT AFTER.** Anything the handler can re-raise must be
    consumed before the handler runs, or the handler's own request is cancelled.

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★★★★★ NEXT SESSION: START HERE                                               │
└──────────────────────────────────────────────────────────────────────────────┘

  **1. PLAY IT.** The bar is a HUMAN at the keyboard: new game, move, shoot, use.
     Everything measured so far is scripted keys against the demo. The known-unknowns
     are held keys (typematic -- see the standing KEYBOARD section, still open) and
     whether the 1-in-3 long-run flake from session 19 survives the fix (it should not;
     it was this bug, but it has not been measured over enough runs to say so).
  **2. SOUND FIDELITY, NOT SOUND PRESENCE.** PCM and MIDI are flowing and the host
     devices are open; nobody has LISTENED yet. Ask the user. The OPL synth is unused
     here (`opl: writes=0`) because Doom picks the MPU-401 when it answers -- if FM
     music is wanted, that is a device-selection question, not a synth question.
  **3. THE CRTC (0x3D4/0x3D5) IS STILL UNCLAIMED** -- `STAGE2: unclaimed ports touched`.
     Page flipping is inferred from the data (`modey_page`) instead of read from the
     register, and claiming it has regressed Doom three times for reasons still
     UNKNOWN. Now that the real killer is gone, retry it -- gated, three runs.
  **4. `dpmi_invoke_callback()` STILL HAS A BARE 64-PHASE CAP** (search `ph < 64`).
     It is the same shape as the bug that froze the clock. Fix it before it bites.

  ⚠ **RIG STATE.** `qimode.txt`, `keys.txt`, `capture.flag`, `pmbp.txt` and `pmwatch.txt`
    were all REMOVED before the gate runs. A stale one silently alters every later run.
  ⚠ Every write to `/tmp/xpshare` needs `dangerouslyDisableSandbox`; a sandboxed write
    FAILS SILENTLY and you then read a stale log.
  ▶ Evidence archive + exact repro: **`docs/research/doom-int-site-patch/`** (three
    gzipped runs and the menu screenshot; `build/` is gitignored).
```
