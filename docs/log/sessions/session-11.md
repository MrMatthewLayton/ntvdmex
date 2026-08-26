# Session 11 — 2026-08-19

> Archived verbatim from `return-ntvdm.md`, which was the single rolling handoff
> file until 2026-08-26. Split by session so it can be read by topic and date
> rather than by scrolling. **Nothing has been edited** — including conclusions
> that later sessions REFUTED, which are kept on purpose: the refutations are
> some of the most valuable material here.

```
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

★★★★ **ROUND 3 (host v165): DOS API 102/103 = 99%, BIOS ~70%, OVERALL ~85%.**
  ▶ **INT 21h: only 4Bh EXEC remains.** Added this round: the FCB group (0Fh-24h, 27h-29h),
    1Bh/1Ch/1Fh/32h drive params, 26h/55h PSP creation, 31h TSR, 37h switch char ('/'),
    53h, 5Eh/5Fh, 64h, 66h code page (437), plus the earlier file/handle batch.
    **All five real 6.22 apps report `INT21 unimplemented: none`** — MEM, CHKDSK, TREE,
    ATTRIB, COMMAND.COM.
  ▶ **BIOS: eight interrupts planted** (11h/12h/13h/14h/15h/17h/25h/26h) — every one was a
    bare IRET before, handing the caller its own registers back. New STAGE2 line reports
    partial/unimplemented BIOS services.
  ▶ **#39 VIDEO MODES: the epic's named defect is fixed.** There is now a MODE TABLE
    (`vid_modes[]`): text 0/1/2/3/7 with real geometry (mode 0 is 40 columns, not 80), planar
    0Dh/0Eh/0Fh/10h/11h/12h with per-mode resolution, linear 13h. The renderer follows
    `st->mkind` + `st->gw/gh` instead of branching on 12h/13h only, so **11h no longer shows a
    text screen while the program writes pixels**. CGA 4/5/6 are marked UNSUPPORTED and LOUD
    rather than approximated -- their two-bank interleaved B800 layout shares nothing with the
    planar path, and quietly showing text is the failure mode #27 exists to remove.

★★ **TWO SCAFFOLDING BUGS FOUND THIS ROUND, both of which would have corrupted future work:**
  1. **DOS calls that RETURN A SEGMENT IN DS** (1Bh, 1Ch, 32h, 52h) broke the probes' own
     output: every probe store is DS-relative, so the probe wrote its state into DOS's segment
     and printed labels read from there -- the dump came out as unlabelled hex, then as
     fragments of executable code. Fixed IN `probe_capture` (restores DS=CS on exit, after
     capturing the guest's DS faithfully), not per-probe, so it cannot be forgotten.
  2. **The oracle harness decoded helper output as UTF-8**, so any probe whose buffer dump held
     a byte above 0x7F aborted the whole run with a UnicodeDecodeError. Now CP437. The harness
     must never be the thing that fails on unusual data — that is the data worth seeing.

★★★★★ **#30 EXEC (4Bh) WORKS — INT 21h IS 103/103. host v166.**
    EXEC: "P_CHILD.COM"
    EXEC: child at seg=0x1101 entry=1101:0100 (COM) depth=01
    EXEC: child exited rc=0x2a, parent resumed (depth=00)
  Oracle-matched on all three properties that matter: the child RAN, the parent RESUMED at the
  instruction after its INT 21h, and the child's EXIT CODE came back through AH=4Dh.
  ▶ **HOW THE RETURN WORKS — the load-bearing idea.** The parent entered via `INT 21h`, so the
    CPU pushed FLAGS/CS/IP on ITS stack and we are inside our BOP stub. We snapshot the parent's
    ENTIRE frame — including CS:IP pointing AT the BOP and SS:SP pointing at that IRET frame —
    then overwrite it with the child's entry state. On child exit we put the frame back and step
    EIP past the BOP, so the stub's own IRET pops the parent's own frame and lands exactly where
    EXEC returning normally would have. **No stack is unwound by hand.** Nesting stack is 8 deep
    (`g_exec[]` in main.c); child memory is freed on exit.
  ▶ SPLIT BY LAYER: `dos_int21` only RECORDS the request (path + parameter block) and sets
    `exec_pending`; the host's `exec_begin()` does the load and the transfer, because the loader,
    file I/O and the guest register frame all live there.
  ▶ **A .COM OWNS ALL OF MEMORY, so EXEC returns AX=0008 until the parent gives some back.**
    The probe shrinks itself with 4Ah first — that is not a workaround, it is what COMMAND.COM
    does before launching anything. Worth knowing before diagnosing an "out of memory" EXEC.
  ▶ AL=01 (load-without-execute) and AL=03 (overlay) are LOUD-unimplemented, not silent.
  ▶ NEW: probes can ship companion files via a `<probe>.deps` sidecar, and all three hosts now
    run a probe FROM ITS OWN DIRECTORY so a relative companion path resolves everywhere.

★★★★★ **ROUND 4 (host v170): DOS 103/103, BIOS COMPLETE. ALL 15 PROBES CLEAN.**
  ▶ **#39 CGA modes 4/5/6 now RENDER.** They were the last "unsupported" modes and the layout
    is why: rows INTERLEAVE between two 8 KB banks at B800 (even rows from 0, odd from 0x2000)
    and pixels are 2 bits (4/5) or 1 bit (6), packed high-bit-first. Nothing is shared with the
    planar path — approximating them with a text screen was never going to work.
  ▶ **#41 palette complete**: 10h AL=00/01/02/03/07/08/09/13/15/17/1A/1B, plus **0Bh** (border +
    CGA palette), **0Dh** (read pixel), **07h** (scroll DOWN — 06h scrolled up and 07h fell
    through to nothing, so downward scrolls silently did nothing), **1Ch** save/restore state.
  ▶ **#40 character generator**: 11h AL=x1-x4 ROM font selection and AL=20-24 graphics font
    pointers. User-font LOADS (AL=x0) are LOUD — we render from our own tables, so accepting a
    user font would silently draw the wrong glyphs.
  ▶ **#42 VESA complete**: 4F03/06/07/08/09. 4F0A (PM interface) and 4F15 (DDC) report NOT
    SUPPORTED rather than returning success with a null pointer a client would call into.
  ▶ **#46 INT 20h/27h/28h/29h planted.** 29h (fast console out) was an IRET that swallowed
    output silently.

★★ **A REGRESSION I CAUSED AND THE SELFTEST CAUGHT — the reason that gate exists.**
  I planted INT 20h with **BOP number 0x20 — which is ALREADY the INT 21h handler's**. The new
  BIOS dispatch sits ahead of INT 21h, so it intercepted EVERY INT 21h call as "terminate
  program": selftest exited at its first DOS call with no output at all. BOP numbers are a
  SHARED NAMESPACE across DOS, BIOS, XMS (0x43) and DPMI (0x50-0x57). INT 20h now uses BOP 0x30
  and the table is {vector, bopnum} pairs so the two can differ. **Check the namespace before
  adding a BOP.**

▶ **PROBE HYGIENE, prompted by making every host run from its own directory:** several probes
  were comparing values that describe WHERE THE PROBE IS rather than what DOS does — the default
  drive, the current directory, the volume serial, truename's base. Those are now dumped but not
  compared, with the reason recorded. Also dropped: AX after 47h/60h/69h, which RBIL documents as
  destroyed and which the hosts duly disagree on.
  ► And a real fidelity fix it exposed: FCB open now fills in the RESOLVED DRIVE (DOS replaces a
    "default drive" 0 with the actual drive; we were leaving the caller's 0).

▶ **54 recorded rationales** in `tools/dostest/oracle-rules.json`. Every DOSBox divergence is
  explained, and in each case **we match the genuine kernel** — including the CP437 collating
  table, where DOSBox fails to fold lower case onto upper and ours is byte-identical to 6.22's.

★★★★ **MODE 12h: ROOT-CAUSE HUNT. One real deadlock FIXED; the remaining blocker is
KERNEL-SIDE and is the next piece of work.**

▶ **SCOPE, from the QuickBASIC demos in `demos/` (sources in `demos/src`):**
    SCREEN 12 — BLIT, BOUNCEBX, BUBBLES, MATRIX_1, MATRIX_2, MOUSE   **6 of 10, all broken**
    SCREEN 13 — CAVE, GFXCOPY, PALETTE                                 work (PALETTE confirmed)
    SCREEN 0  — VS87
  Matches the user's recollection exactly ("the mode 12h ones never did, at least not very
  well"). Skyroads is 13h, which is why our one game never touched this path.

★★★ **FIXED: `host_interp()` ran up to 2,000,000 guest instructions WITH NO WAY TO TAKE AN
INTERRUPT.** BLIT's outer loop is `DO WHILE INKEY$ = ""` — it can only END when an interrupt
fires. Escalated to the interpreter, it burned the whole cap, returned, re-faulted,
re-escalated. **TEN I/O events in thirty seconds.** The interpreter is standing in for the CPU
and a real CPU takes interrupts mid-loop, so it now checks for a pending IRQ every 256
instructions and yields. **15x improvement (10 -> 157 events, 8x more pixel data).** selftest
still 8/8. That fix stands on its own regardless of the rest.

★★★ **THE REMAINING BLOCKER: ARMING THE A0000 PAGE TRAP STOPS THE GUEST RUNNING.**
    trap ON  -> io_events = 10,          guest frozen at 0050:0037 in 58/60 heartbeats
    trap OFF -> io_events = 22,532,292,  guest running QB code, PC moving every sample
  `PAGE_NOACCESS` and `PAGE_READONLY` behave IDENTICALLY, so it is not reads-vs-writes: it is
  protecting that range at all. Diagnostic knob added: **`noa000.flag` on the share** disables
  the trap (absent = normal). Delete it after use.
  ► With the trap off the guest's real inner loop is visible at its PC:
    `DEC DX / MOV AL,07 / OUT DX,AL / INC DX / MOV AL,0F / OUT DX,AL` — per-pixel VGA register
    reprogramming, exactly what the batching interpreter exists to absorb.

▶ **RULED OUT BY MEASUREMENT — do not re-investigate these:**
  • **The mode table** (#39). Resolves 12h correctly: `mode=0x12/kind=01/640x480`. New
    `STAGE2: mode sets:` line proves it.
  • **The planar write engine.** Complete and correct — 4 write modes, set/reset, ALU, bit
    mask, latches. I nearly rewrote working code TWICE on the strength of a screenshot.
  • **The IVT.** `ivt08=0050:0034 ivt1C=0050:003a`, and QuickBASIC has NOT hooked either.
  • **Async IRQ injection.** 545 successes, **zero bails**, zero nest-blocks.
  • **The "mode-12h MOV-store decoder gap"** from the M3 notes: `interp-refused=0`. The
    interpreter never declines an opcode. That lead is DEAD.
  • **Unhandled events.** None — no `STAGE2: stop event` line; every event is serviced.

▶ **THE LIKELY SHAPE OF THE ANSWER.** The M3 planar trap was **VM-confirmed on HVF, never on
  real hardware**, and there is precedent for exactly this class of difference: session 8 found
  HVF reflects IOPL-0 I/O as event 0 while real silicon uses event 3. So the A0000 trap may
  simply never have worked on the rig. Next step is kernel-side: **why does VirtualProtect on
  A0000 stall `VdmStartExecution`** — same class of work as the #18 reflect RE, not a patch.
  ► If that proves hard, the alternative is to stop trapping altogether and drive the
    INTERPRETER from mode set. It already runs the guest correctly and now yields properly, and
    it needs no page protection at all.

▶ **A METHOD NOTE WORTH KEEPING.** I called this "a regression I introduced today" on the
  strength of our new output differing from our old. **Neither was correct** — the oracle showed
  16 colours, both builds showed 2. Different is not wrong when nothing is right. The reference
  is the ORACLE, never our own previous build; one oracle run settled in seconds what an hour of
  comparing our own screenshots could not. (`dosoracle.py run BLIT.EXE --timeout 22 --screenshot`
  — the timeout path is currently the only way to get pixels out of the oracle.)

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
██  PRUNED: SESSIONS 6, 8, 9, 10 (stale restart snapshots)                     ██
═══════════════════════════════════════════════════════════════════════════════

  These were "RESTART POINT" blocks — HEAD hashes, unpushed-commit counts, working
  -tree state on branch `spike/dpmi-16bit-switch`. Every one of those facts is now
  WRONG, and stale operational state in a rehydration doc is worse than none.
  Removed 2026-08-22; recoverable from git history. What they established that still
  matters is kept in memory and in docs/research/dpmi-under-ntvdmcontrol.md:
    * **session 8** — the pivot to BARE-METAL testing (QEMU+HVF SIGABRTs on DOS/4GW
      paged 32-bit PM); real mode 8/8 on real silicon.
    * **session 9** — THE CRACK: 16- AND 32-bit DPMI real-CPU PM RUN on bare metal.
      Session 8's "the kernel won't run PM" was OUR OWN `dpmi_enter.S`
      interrupt-pending guard firing on a STALE `[0x714]&3`; fix v68 `c0831b1`.
      (Referenced from memory as "return-ntvdm.md session-9" — full detail is in
      docs/research/dpmi-under-ntvdmcontrol.md, runs 65-79.)
    * **session 10** — host v97 iteration on the same track.
    * **session 6** — power-down snapshot; runs 78-79 (async IRQ0 injection, the
      D/B-aware `host_try_io_pm`).
  The KD / GH #18 history that sat at the end of this file is likewise in
  [[kd-guest-debugger-ops]] and the research doc — including run 71, whose verdict
  (a raw PM #GP silently terminates the VDM) is quoted in DO-NOT-RE-SPEND above.
```
