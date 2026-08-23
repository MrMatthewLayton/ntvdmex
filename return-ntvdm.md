═══════════════════════════════════════════════════════════════════════════════
██ ▶▶▶ SESSION 18 (2026-08-22). DOOM'S OWN CODE RUNS AND ITS DPMI WORKS.      ██
██     IT STOPS AT `I_StartupTimer()` BECAUSE OF ONE ARCHITECTURAL CEILING,   ██
██     AND THE SPIKE THAT LIFTS IT ALREADY RUNS.                              ██
═══════════════════════════════════════════════════════════════════════════════

  Branch `m9/completeness`, HEAD `965e154`. Gates at the end: **off-VM 349/349**
  (8 suites), **selftest.com 8/8** on the rig, `dpmitest.com` + `dpmiback.com` both
  `STAGE2: complete` with correct output, `check-imports.sh` clean, share clean.
  **Doom is NOT playable and does not reach the menu.**

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★★★★ START HERE. ONE BOUNDED BUG, THEN TWO QUESTIONS THAT COLLAPSE A LOT.    │
└──────────────────────────────────────────────────────────────────────────────┘

  ⚠⚠ **SESSION 19 CORRECTION — "8 CLEAN PM ENTRIES" WAS NEVER TRUE, AND ITEM 1 AS
     WRITTEN IS MIS-SCOPED.** `INT 31h 0301` is not broken. The guest never
     correctly executes the stores that FILL its RMCS. Measured, twice
     (`build/pmk5.log`, `build/pmk6.log`), and the old text below is kept only so
     the correction reads against it.

  ⚠ **AND ITEM 1 BELOW IS ITSELF TOO STRONG — see "THE PER-ENTRY FAULT IS HARMLESS"
     further down.** The faults are real and the measurements hold, but they do not
     kill a client: `pmstep.com` completes under `pmkernel.flag`. Read both.

  **1. THE KERNEL PM PATH FAULTS ON ORDINARY GUEST MEMORY STORES.** Under
     `pmkernel.flag`, `dpmitest.com` takes **one unhandled fault per PM entry, eight
     of eight** — six `exc=0xC0000005`, two `exc=0xC000001E` — and the bytes at the
     faulting EIP decode to plain stores, not to any `INT nn`:
     ```
        at=0x0133  a3 00 06        mov [0x600],ax
        at=0x0141  89 16 04 06     mov [0x604],dx
        at=0x01a5  c7 06 9a 04 ..  mov word [0x49a],0x0100   <- an RMCS field
     ```
     The RMCS reading all zeros is a SYMPTOM of this, not a separate bug.
     ▶ The old story — "the kernel reflects a PM `INT nn`, so `dpmi_crash_veh`
       services it" (run 26) — **is refuted.** That arm was answering genuine faults
       as "INT 31h, unsupported function", rewriting EAX and CF and forcing CS/SS,
       once per entry. Fixed in `f6c5b16`: it now services only a provable `CD nn`
       at the faulting EIP. **That fix did NOT change the run** — a control with the
       fake servicing removed is identical line for line. Do not re-try it.
     ▶ **AND THE GUEST IS EXECUTING FROM MID-INSTRUCTION ADDRESSES.** Entry 3 enters
       at `0x152` and faults at `0x155`, which is inside `b8 04 02`
       (`mov ax,0x0204` at `0x154`). At `0x155` those bytes are `add al,2`; the VEH
       reads `AX=0x0206` and the outer loop then reads `AX=0x0208` at the BOP — two
       executions of `add al,2` over `0x0204`. Every number matches, so this is not
       a mislabelled register: **control flow itself is desynchronising.** Three of
       the eight `INT 31h` dispositions in that log are consequently wrong
       (`0x0400`→`0x0001`, `0x0204`→`0x0208`, `0x0901`→`0x0902`).
     ▶ **NEXT, AND IT IS ONE RUN:** find out whether the fault is the FIRST store to
       a page (the sentinel at `0x1600` goes `00`→`01` across hit #2, so a faulting
       store DOES land on retry — that smells like a page the kernel will not fix up
       for a PM VDM). Dump the page protection of the guest's low memory
       (`VirtualQuery`) at the fault, and log every fault rather than one per entry.
  **2. THE INT→BOP PATCH MAP IS STILL NEEDED** — settled, and it is the inverse of
     what this file used to say. The kernel does NOT natively reflect a PM `INT nn`
     to us; every reflect we thought we saw was a fault. The BOPs are what make
     `VdmStartExecution` return at all (`ev=0x4`, eight times).
  **3. DOES THE KERNEL DELIVER IRQ0 WITH NO INJECTION AT ALL? ASKED DIRECTLY —
     AND IT CANNOT BE ANSWERED YET.** `tools/dostest/pmtick.asm` (new) hooks IRQ0
     with `INT 31h 0205`, spins, and reports PM handler entries alongside the BIOS
     tick either side. Run it with **both** `pmkernel.flag` and `pmnoirq.flag`;
     without pmnoirq OUR injector answers instead of the kernel.
     ```
        far-jmp + pmnoirq   PM entries = 0x0000  tick delta = 0x0024  tick0 = 0x2C2D
        pmkernel + pmnoirq  dies in PM entry 1 -- no answer
     ```
     ▶ The first line is worth having on its own: **the ceiling is now MEASURED, not
       just reasoned.** It was an argument from `POPFD` at CPL 3 and `VdmpCanDeliver`;
       now 36 ticks of real time pass with a hooked PM handler and nothing arrives.
     ▶ The second line: under the kernel the probe dies at PM entry 1 —
       `VdmStartExecution` never returns, no watchdog line, no wind-down
       (`build/pmk7_pmtick.log`) — before it ever reaches the hook.
       ⚠ I first read that as "the store faults block everything, fix them first."
         **That was wrong**, and `pmstep.com` completing under the same flag is the
         refutation. The right next move is to **bisect pmstep → pmtick**, not to go
         after the faults: two tiny clients, one completes and one dies, and the
         difference between them is a subroutine call, an `INT 31h 0205` hook and a
         `sti`. That is a couple of runs, and it answers item 3 as a side effect.

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★★★★ THE PER-ENTRY FAULT IS HARMLESS, AND A CLIENT RUNS TO COMPLETION        │
└──────────────────────────────────────────────────────────────────────────────┘

  **`pmstep.com` COMPLETES UNDER `pmkernel.flag`** — `STAGE2: complete`, printing
  from protected mode, its store landing. Seven PM entries, one fault each, every
  one swallowed by the VEH and harmless (`build/pmk9_pmstep.log`).
  ⇒ **The one-fault-per-entry is NOT the wall.** Item 1 above still describes real
    faults, but "the kernel PM path faults on ordinary stores and that is why
    everything dies" is TOO STRONG — I wrote it, and a fifty-line client disproves
    it. `dpmitest`'s zero RMCS and `pmtick`'s death at entry 1 need another cause.

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★★★★ FIXED (`6c6b5e8`): RESUME A PM FAULT AT THE **ENTRY** EIP               │
└──────────────────────────────────────────────────────────────────────────────┘

  **THE ZERO RMCS IS CLOSED.** `dpmitest.com` under `pmkernel.flag`:
  ```
     callRM 0x0100:0x0277                      (was callRM 0x0000:0x0000)
     "0301 real-mode far-call OK (sentinel BEEF)!"   printed FROM PROTECTED MODE
     INT 31h sequence now 0400 0100 0205 0204 0900 0901 0300 0301
        -- exactly the client source; three of these used to arrive with a
           corrupted AX (0001, 0208, 0902)
  ```

  **WHAT IT WAS.** The kernel's exception record reports a fault EIP that is simply
  **not reliable**, and the VEH was resuming there.
  ⚠ **THE "entry+0 OR entry+3" RULE IN THE HISTORY BELOW IS WRONG — IGNORE IT.**
    `pmal.com` entry 1 is `mov ax,0x4C00` at `0x144` and faults at `0x145` (+1);
    `pmstep.com`'s BYTE-IDENTICAL entry 1 faults at +3. Same code, different offset,
    so the address is not a function of the instruction stream. **Three address
    "patterns" in a row turned out to be sampling artifacts** — see the method note.

  **HOW IT WAS SETTLED — STOP INFERRING, MAKE THE GUEST COUNT.** `tools/dostest/pmal.asm`
  fills the PM entry with `mov al,imm8` (2 bytes each, ascending) so **AL is a program
  counter the fault log already prints**:
  ```
     fault 1   AX=0x0000 at E+0   AL untouched            -> nothing executed
     fault 2   AX=0x090B at E+1   unchanged               -> `mov ax,0x4C00` had NOT run
     pmtick    AX=0x0901 at E+3   AX would be 0x17        -> `mov ax,ds` had NOT run
  ```
  ⇒ **the guest has executed NOTHING when the fault arrives.** The fault is spurious,
    raised at PM entry; only the reported EIP is wrong. So resume at the EIP the host
    handed to `VdmStartExecution` — which it knows exactly.
  ▶ This also retro-explains the `AH=4Ch` loose end: `pmstep` resumed at +3, which
    **skipped** its `mov ax,0x4C00`, so the exit ran with the wrong AX. Same bug.
  ```
     pmal.com    died -> COMPLETES      pmstep.com  completes, and now exits cleanly
     pmtick.com  died at entry 1 -> reaches the spin
  ```

  **THE DISCRIMINATOR IS `EDX`, NOT THE EXCEPTION CODE** (`b8043d8`). Three codes now
  denote the SAME spurious entry event — `0xC0000005`, `0xC000001E` and `0x80000003`.
  Keying on the code mislabels one every time a new client appears: pmtick's
  `STATUS_BREAKPOINT` "at 0x3dc" looked real, but guest `0x3dc` is the middle of the
  string `"PMTICK: mode switch FAILED (CF=1)"` — message data, no `0xCC` in it.
  What IS invariant, and was in the very first log: **the kernel puts the ENTRY EIP in
  `EDX`**, and it is not the guest's own (pmtick entry 2: ctx `EDX=0x20b`, the entry,
  while the guest's real EDX was `0x2c2d`). Anything else goes to the fatal dump.

  ⚠ **STILL OPEN, AND IT IS THE NEXT THREAD.** `pmtick` entry 2 (`enter 0x20b`, the
    tail of its `rm_tick`) ends at `cs:eip=0x3f:0x0080` — and **`0x0080` is
    `DPMI_FAULT_COFF`, our own GH#18 PM-fault trampoline.** A near `ret` cannot change
    CS, so the guest took a GENUINE PM fault in `mov ax,dx / pop dx / pop cx / pop bx
    / ret` and the trampoline caught it correctly. A real fault, not a reporting
    artifact. **So item 3 is STILL unanswered.**
  ▶ **Its stack is NOT the cause — already checked.** The entry-2 dump decodes
    cleanly: `[0xfff8]=0x013a` (the `call rm_tick` return), then pushed BX/CX/DX, plus
    `[0xfffc]=0x0100 [0xfffa]=0x0127` — the **un-popped `call far [entry]` mode-switch
    frame**. That accounts for the 4 bytes I first mistook for a re-executed `CALL`.
    Don't re-derive it.

  **THE BISECT, for the record** (all under `pmkernel.flag`, all COMPLETE, so all
  four constructs are INNOCENT): `pmcall` (+ a PM `CALL`), `pmt1a` (+ `INT 1Ah`),
  `pmsubint` (+ `INT 1Ah` inside a subroutine behind register pushes). `pmnoirq` is
  innocent too — `pmtick` dies with it absent as well.

  **`pmvehpass.flag` (new) settled the other half:** let a non-INT PM fault fall
  THROUGH the VEH and `VdmStartExecution` does NOT return it as an event — the run
  wedges on the first fault with no event and no return. **The VEH swallow is
  load-bearing.** Don't retry it.

  ▶ **WHERE TO GO NEXT.** `pmstep` completes and `pmtick` dies at entry 1; both are
    tiny 16-bit clients. **Bisect between them** — that is a much shorter path than
    reasoning about the kernel. The obvious differences: pmtick calls a subroutine,
    hooks `INT 31h 0205`, and does `sti`.
  ⚠ Still unexplained, do not file away: `dpmitest` entry 0 reaches its BOP with
    `AX=0x0001` where the guest wrote `0x0400` and the TIB says `0x0000` at entry.
    Something SETS AX to 1.
  ⚠ Also real, also unexplained: under `pmkernel`, **`INT 21h AH=4Ch` does not
    terminate the client.** pmstep ran on past its own exit into unrelated code and
    ended on `GH#18 PM-FAULT ev=0x1` at `0x0f:0xe638`.

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★★★★ THE CEILING, AND WHY IT MAKES THE SPIKE THE MAIN LINE                   │
└──────────────────────────────────────────────────────────────────────────────┘

  Three facts. Together they say the CURRENT architecture cannot ever finish Doom:

  1. **The guest runs with VIF clear however we write it.** The mode switch writes
     `EFLAGS=0x80202` (IF|VIF) into the TIB; a live `GetThreadContext` of the
     suspended guest — `CS=0x187`, its own flat code selector — reads
     `efl=0x00000246`: **IF=1, VIF=0, VIP=0**.
  2. **`POPFD` at CPL 3 cannot modify IF, IOPL, VM, VIP or VIF.** PM entry loads
     flags with `push [ebx+0x398] ; popfd` (`dpmi_enter.S`), so our VIF write is
     discarded and the guest runs with the host thread's ordinary user-mode flags.
  3. **The kernel's delivery gate reads VIF, not IF** (`VdmpCanDeliver`; see the
     `EFLAGS_VIF_BIT` note in `ntvdm.h`, where this once cost the real-mode timer).

  ⇒ **While PM runs IN-PROCESS via far-jmp, the kernel can NEVER hand a hardware
    interrupt to a protected-mode client.** Only ring 0 can set those flags. That is
    why the async `SuspendThread`/`SetThreadContext` injector had to be invented —
    it does the kernel's job from user mode — and why it is fragile enough to tear
    the VDM down (proven by control, below).

  **AND THE SPIKE LIFTS IT.** `VdmStartExecution` **runs protected mode** — commit
  `d127228`, opt-in via **`pmkernel.flag`**, default path untouched:
  ```
     PMKERNEL[0] enter cs:eip=0x0f:0x12e ss:esp=0x1f:0xfffe msw=0x1
     PMKERNEL[0] VdmStartExecution -> st=0x0 ev=0x4 cs:eip=0x0f:0x131
  ```
  Eight consecutive entries (`0x12e…0x1cf`), eight `INT 31h` serviced, and
  `dpmitest.com` prints FROM PROTECTED MODE via `INT 31h 0300`.
  ⚠ **READ THE SESSION-19 CORRECTION ABOVE BEFORE TRUSTING THIS PARAGRAPH.** The
    entries are real and the print is real, but they are NOT clean: each one takes an
    unhandled access violation on a guest store, and three of the eight `INT 31h`
    calls arrive with the wrong AX. "Eight entries, eight INT 31h" was a LINE COUNT,
    and I wrote it up as a health claim without decoding what the lines said.
  ▶ **The early spike's "VdmStartExecution faults when it runs PM" DOES NOT
    REPRODUCE.** It was measured when almost none of the DPMI host existed — and the
    entire in-process architecture (INT→BOP patch map + async injector) was built on
    that one result.
  ▶ **THE FIX THAT GOT PAST ENTRY 1**, a hazard documented elsewhere in this file
    biting from a new direction: with a 16-bit SS the CPU maintains **SP only**, so
    ESP's top half keeps host junk. The far-jmp path stores it and reloads it
    harmlessly with `lss`; **the kernel takes the CONTEXT's ESP whole**:
    ```
       entry 0   ss:esp=0x1f:0x0000fffe   -> returns ev=4
       entry 1   ss:esp=0x17:0xb33afffa   -> NEVER RETURNS
    ```
    `0xb33a` is a host thread-stack address. Narrowing ESP to the descriptor took it
    from 1 entry to 8.

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★★★ THE EXISTENCE PROOF: STOCK NTVDM REACHES DOOM'S TITLE SCREEN ON THIS BOX │
└──────────────────────────────────────────────────────────────────────────────┘

  `scripts/bm/stockdoom.bat` via `controld exec`; trace: `build/doom_stock_reference.txt`.
  ```
     I_StartupTimer()            <-- NTVDMEX STOPS DEAD ON THIS LINE
       calling DMX_Init
     D_CheckNetGame / startskill 2 / player 1 of 1 / S_Init / HU_Init / ST_Init
  ```
  `ST_Init` is the last line before the game loop.
  ▶ **This had never been run** — `rt_stock.bat` only reaches targets staged into
    `bm\tests\`, so it could not run Doom at all. "Stock runs Doom, therefore so can
    we" was a load-bearing ASSUMPTION for the whole project. One run settled it.
  ▶ It proves the bar is achievable ON THIS HARDWARE, gives a line-by-line reference
    to diff, and proves **the kernel CAN deliver a timer interrupt to a DOS/4GW PM
    client** — the exact thing the ceiling above denies us.
  ▶ First visible difference: stock reports `DPMI memory: 0xf00000, 0x800000
    allocated for zone`; we report `0x0`. Our `0500`/`0501` info is not the real
    host's.
  ⚠ **THE IFEO DEBUGGER KEY IS THE HAZARD.** `stockdoom.bat` deletes it to get stock
    and restores it on every exit path, proving the restore with a `reg query` into
    `stockdoom_state.txt`. **Left absent, every later test silently measures stock
    ntvdm while the logs look entirely plausible.** Check that file and re-run
    `selftest.com` after, every time. Graphics targets carry a display-wedge risk.

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★★★ WHERE DOOM ACTUALLY GETS TO NOW (and it is a long way)                   │
└──────────────────────────────────────────────────────────────────────────────┘

  ```
     P_Init: Checking cmd-line parameters...
     V_Init: allocate screens.        M_LoadDefaults: Load system defaults.
     Z_Init: Init zone memory allocation daemon.
     DPMI memory: 0x0, 0x800000 allocated for zone
     W_Init: Init WADfiles.  -> adding doom1.wad  -> shareware version.
     M_Init  R_Init: Init DOOM refresh daemon - [   ]...................
     P_Init: Init Playloop state.     I_Init: Setting up machine state.
     I_StartupDPMI / I_StartupMouse / Mouse: detected / CyberMan: Wrong mouse driver
     I_StartupJoystick / I_StartupKeyboard / I_StartupSound
     I_StartupTimer()          <-- stops here
  ```
  **Its DPMI works**: one run services **5,331 INT 31h calls across 25 distinct
  services**, and **464 `INT 21h` calls routed through Doom's OWN protected-mode
  handler**. Sets `INT 10h` mode 3. Opens `default.cfg`. Identifies the shareware
  WAD correctly. Canonical log: `build/doom_s18_final.log`.
  ▶ Doom's timer ISR IS hooked and DOES run: ticks are delivered into
    `0x187:0x03ae31f0`, it IRETs cleanly (`done=1 phases=1`), and the counter it
    increments (`0x03b68820`) advances by exactly one per tick. **Delivery is sound.
    Do not re-investigate it.** What kills the run is the mechanism doing the
    delivering.

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★★★ THE FOUR BUGS THAT WERE SESSION 18's WALL. ALL OURS.                     │
└──────────────────────────────────────────────────────────────────────────────┘

  1. **THE CLIENT'S EXECUTABLE KNOWS WHICH OF ITS MEMORY IS CODE — ASK IT.** An LE
     flags its EXECUTABLE objects and DOS/4GW allocates ONE `0501` block per object
     at exactly the page-rounded virtual size. Exact on DOOM.EXE:
     ```
        obj1 vsize 0x44f71 READ|EXEC|BIG32   -> 0501 of 0x45000 @ 0x03AD0000
        obj2 vsize 0x00019 READ|EXEC|ALIAS16 -> 0501 of 0x01000 @ 0x03B30000
        obj3 vsize 0x85e10 READ|WRITE|BIG32  -> 0501 of 0x86000 @ 0x03B40000
     ```
     `dpmi_le_learn()` parses this out of `filebuf`; `0501` tags the block;
     `dpmi_scan_code_blocks()` patches it. **VERIFIED BY COUNT:** the scan patches
     **0x44** sites and obj1 contains **exactly 0x44** `CD nn` pairs in our vector
     set. Zero data bytes touched. Re-run that check if this ever regresses.
     Only objects ≥ 64 KB are keyed on (a page-rounded size is a weak key when
     small); small code objects get a based descriptor, which `0009`/`000C` already
     patches. The block is allocated ~1100 log lines before its last page arrives,
     so the scan is idempotent and re-runs on every `0501` and code declaration.
  2. **AN EIP IS ONLY 16 BITS WIDE WHEN ITS CODE SELECTOR IS.** `EIP & 0xFFFF`
     truncated a flat 32-bit EIP, so Doom's first `int 21h` (AH=30h at obj1+0x40be5)
     fired our BOP exactly as intended at linear `0x03b10be5` and the masked lookup
     asked for `0x0be5` — dying as "unexpected PM stop" AT THE INSTRUCTION THAT
     PROVED THE PATCH WORKED. `dpmi_pm_eip()` asks the descriptor.
  3. **THE PM-RETURN CATCHER'S D/B BIT IS PART OF THE CALLER'S IDENTITY.** An
     extender reads the frame's return CS to size pointer arguments. Ours was
     16-bit, so DOS/4GW truncated Doom's flat pointers:
     `app DS:EDX=0x18f:0x03b69b80 "default.cfg"` → `RMCS DS:DX=0x000:0x9b80`, open
     failed. The stack frame width was ALREADY right — **frame width and advertised
     caller width are two different questions.**
  4. **THE RMCS COPY-BACK OMITTED FLAGS, SO EVERY DOS CALL REPORTED SUCCESS.**
     `0300`/`0301`/`0302` copied eight registers back and dropped the ninth. Watcom
     reads *only* CF (`int 21h ; rcl eax,1 ; ror eax,1`). `access("doom2f.wad")`
     returned "error 2" with CF=0, so Doom picked the FRENCH wad, opened it
     (failing, also as success), and read forever from a handle it never got.

  Also: the host now owns PM vectors 08h–0Fh for a 32-bit client (Doom hooks its
  timer with `AX=2508 int 21h`, not `INT 31h 0205`; letting DOS/4GW service it gives
  CF=1 then `fatal error (1001): error in interrupt chain` — a chain with two
  owners). `INT 31h 0204` returns the offset at the CLIENT's width, not the
  handler selector's. The watchdog flushes captured DOS output before killing a
  wedged run.

┌──────────────────────────────────────────────────────────────────────────────┐
│ ⚠⚠ DO NOT RE-SPEND A SESSION ON THESE. ALL KILLED BY MEASUREMENT.            │
└──────────────────────────────────────────────────────────────────────────────┘

  * **GH #18 / the `#GP` reflect is a PROVEN DEAD END.** Run 71, with a kernel
    debugger attached: the kernel handles a raw non-BOP PM `#GP` by **silently
    tearing the VDM down** — a *handled* path, so it never reaches `0x4f67f8`, never
    bugchecks, never breaks KD. The `+0x638` machinery is correct AND satisfied
    (runs 65-67). Nothing about it can make this fault visible. **I proposed it
    anyway this session without re-reading run 71. Don't.**
  * **The async mechanism is what tears the VDM down** — control experiment,
    `qimode.txt`=`40` (`qi_susp=0`): async ON → 5 ticks then silent teardown; async
    OFF → 0 ticks, guest wedges, watchdog ends it CLEANLY.
  * **Not the ISR** (zero `HLT`/`INT3` in the whole ISR path — scanned).
    **Not nesting exhaustion** (budget `0x03b683e8` and stack top `0x03b683f0`
    IDENTICAL before and after every entry). **Not a tick count** (batch 64 → 5
    ticks, batch 3 → 4). **Not "the Nth tick releases the delay"** (a breakpoint on
    the delay's return address obj1+`0x10f6c` was armed and NEVER hit). **Not VIP**
    (`efl=0x246` every time: neither VIF nor VIP set — refuted by my own log
    *before* I ran it).
  * **Coalescing the tick drain** and **giving `AH=35h` a safe chain target** were
    both tried; **neither changed anything** (5 ticks either way).
  * Session 17's four: **CLI/STI in PM are FINE**; **IOPL cannot be raised** (the
    kernel strips it); **`AX=FF00` failing is a red herring**; **`DOOM.ETX` is not
    an error signal**.

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★ A SEPARATE, SHARP, REPRODUCIBLE BUG: COMMAND-LINE ARGUMENTS                 │
└──────────────────────────────────────────────────────────────────────────────┘

  **Passing ANY argument makes DOS/4GW quit before printing a character.** The
  DPMI/INT 21h trace is identical to a working run for **all 617 of its lines** then
  stops — right after the `AH=30h` version check (returns `0x1606` = 6.22, correct)
  and before the `AH=35h`/`AH=25h` vector-0 install a working run does next. The
  tail is well-formed: `cmdtail len=0x05 [20 2d 7a 7a 7a 0d]` = `" -zzz\r"`.
  ▶ Blocks `doom -nosound`, the obvious way to take the sound path out of the
    picture, and every game that takes options.
  ▶ Arguments go in **`doomargs.txt`** on the share root (`doomrun.bat` reads it).
  ▶ Cheap next probe: the same target under `stock <target>` for comparison.

┌──────────────────────────────────────────────────────────────────────────────┐
│ RUNNING IT — AND THE KNOBS                                                   │
└──────────────────────────────────────────────────────────────────────────────┘

  ```
  cp build/ntvdmhost.exe /tmp/xpshare/bm/ntvdmhost.exe
  md5 -q build/ntvdmhost.exe /tmp/xpshare/bm/ntvdmhost.exe   # MUST match, every time
  rm -f /tmp/xpshare/result_doom.log
  printf 'doom\r\n' > /tmp/xpshare/cmd.tmp && mv /tmp/xpshare/cmd.tmp /tmp/xpshare/cmd.txt
  ```
  Poll `result_doom.log` for a stable size (3 s apart, twice equal). A clean
  wind-down writes `STAGE2: complete`; its absence means the VDM was killed — but a
  **wedged** run now still flushes `==> DOS OUTPUT (wedged):`, so the program's own
  output survives either way.

  **Share knobs** (absent file = off, zero cost):
  ```
    pmkernel.flag   run PM under VdmStartExecution instead of the far-jmp  ★ THE SPIKE
    pmnoirq.flag    suppress IRQ0 -> PM injection entirely
    pmverbose.flag  per-event checkpoint firehose (cp_max 8 -> 0x100000)
    pmwatch.txt     up to 4 hex LINEAR addresses, dumped either side of each injection
    pmbp.txt        guest breakpoints (format in the standing reference below)
    doomargs.txt    extra command line for doom  (currently fatal — see the bug above)
    qimode.txt      hex bits; 0x40 = disable async delivery (qi_susp=0)
                    ⚠ it also stops g_hcpu being created, so ANY tool needing the
                      exec-thread handle silently goes away with it
    headless_ms.txt run cap in ms (currently 45000)
  ```
  Gates before any commit: `./tools/dostest/run.sh` (8 suites, 349 checks),
  `selftest.com` on the rig, `dpmitest.com` + `dpmiback.com`, `check-imports.sh`.

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★★ OFFLINE: DOOM'S LE IS FULLY MAPPED. DO NOT RE-DERIVE IT.                  │
└──────────────────────────────────────────────────────────────────────────────┘

  `build/doom_obj{1,2,3}.bin` are extracted. To regenerate, or for another game:
  ```
    LE header   search for "LE\0\0" AND VALIDATE (byte/word order 0, format level 0,
                cpu 1-4, os 1-4, 1<=nobj<=64). DOOM.EXE: file 0x27acc.
    ⚠ e_lfanew IS NOT A POINTER TO IT in a bound exe -- DOOM.EXE's reads 0x09b40000,
      off the end of a 0xad511-byte file. The MZ stub is the extender.
    ⚠ HEADER LAYOUT: cpu +0x08, objtab +0x40, nobj +0x44, pagemap +0x48. NOT the LX
      layout -- being 8 bytes off parses cleanly into GARBAGE ("265032 pages, 0
      objects") and I nearly believed it.
    ⚠ THE `datapages` FIELD AT +0x80 IS WRONG FOR A BOUND IMAGE (says 0x1ce00, which
      is INSIDE the extender). Real base = 0x42014, flush to EOF. CONFIRM BY
      CONTENT: the entry disassembles to `jmp` over "WATCOM C/C++32 Run-Time
      system", and obj3 contains "doom1.wad" / "Z_Init" / "-devparm".
    mapping     obj1 file 0x42014 -> guest 0x03AD0000, entry EIP 0x40b48
                obj3 file 0x88014 -> guest 0x03B40000
      cross-check: the log's `setbase 0x03b10b48` == 0x03AD0000 + 0x40b48.
  ```
  Landmarks inside obj1:
  ```
    0x40b48  LE entry -- `jmp` over the Watcom copyright banner
    0x40be5  `mov ah,30h ; int 21h`   (the first application INT)
    0x406d0  file-open thunk: `mov ah,3Dh ; int 21h ; rcl eax,1 ; ror eax,1`
    0x40985  `mov ah,3Fh ; int 21h`   (spun 20969 times on bug 4)
    0x10f5f  Sound Blaster DSP reset: `out dx,al` 1 / delay / 0 / delay
    0x153b0  the millisecond delay -- SPINS on the tick at [0x28820] (lin 0x03b68820)
    0x153dc  that spin: `cmp [0x28820],eax ; je` -- two instructions, no I/O, no INT
    0x131f0  Doom's timer ISR (Watcom __interrupt wrapper) -> 0x134e0 shared body
    0x1359a  its stack switch `lss esp,[edx+8]`, 0x1359e the indirect `call *ebx`
    0x41e41  a table of `int N ; ret` thunks (Watcom's generic dispatch)
  ```

┌──────────────────────────────────────────────────────────────────────────────┐
│ ★★★ METHOD — THIS SESSION PAID FOR THESE                                     │
└──────────────────────────────────────────────────────────────────────────────┘

  ▶ **CHECK THE LOAD-BEARING ASSUMPTION FIRST.** "Stock ntvdm runs Doom" underpinned
    the whole project and had never been run. One command settled it — and it
    produced the best result of the session.
  ▶ **AN INSTRUMENT THAT LIES IS WORSE THAN NONE, AND MINE DID.** The first LE
    extraction used the header's `datapages` field, landed inside the extender, and
    reported "57 `CD 21` sites in Doom's code" — 30× over chance and it looked like
    proof. **Before believing a count, DECODE A SAMPLE of what you counted**; the
    disassembly showed 16-bit `lcall` where 32-bit code belonged.
  ▶ **PREDICT THE NUMBER BEFORE THE RUN.** "obj1 should hold ~66 sites" turned "did
    it work?" into "did it patch exactly the code object and nothing else?" —
    answer 0x44 both sides. A count matching a pre-registered prediction is
    evidence; "the log got bigger" is not.
  ▶ **RUN THE CONTROL BEFORE THE THEORY.** `qimode=40` (async off) narrowed the
    teardown to the mechanism in ONE run, after hours of hypotheses.
  ▶ **READ YOUR OWN LOG BEFORE THEORISING PAST IT.** The VIP theory was refuted by
    `efl=0x246` lines already sitting in the log. Twice in one day I theorised past
    available evidence.
  ▶ **RE-READ THE HANDOFF BEFORE PROPOSING A DIRECTION.** I recommended GH #18,
    which this very file records as a measured dead end.
  ▶ **(SESSION 19) A COUNT OF LOG LINES IS NOT A HEALTH CHECK — DECODE WHAT THEY
    SAY.** "8 clean entries, 8 INT 31h serviced" went into a commit subject and the
    top of this file. Eight lines existed; every one of them was a fault we had
    mislabelled, and three carried the wrong AX. This is the SAME lesson as session
    18's "57 CD 21 sites" — *decode a sample of what you counted* — and it caught me
    again one session later, on my own instrument's output rather than a client's.
  ▶ **(SESSION 19) I EXTRACTED THREE ADDRESS PATTERNS FROM FAULT LOGS AND ALL THREE
    WERE ARTIFACTS** — "fixed +3", then "entry+0 or entry+3" (committed, then refuted
    by the very next client), then the mid-instruction story. What finally worked was
    not a better pattern but a DIFFERENT KIND OF DATA: make the guest carry a counter
    (`pmal.asm` puts a program counter in AL) so the log reports GUEST PROGRESS rather
    than an address I have to interpret. **When two readings of an address disagree,
    stop reading addresses.**
  ▶ **(SESSION 19) WHEN THE SAMPLES ARE INCIDENTAL, CHOOSE THE CODE INSTEAD.** Ten
    fault addresses read off whatever the clients happened to have at those offsets
    fitted FOUR incompatible rules, and I picked the wrong one and wrote it into this
    file as "the sharpest lead". A fifty-line client whose instruction lengths make
    the four rules predict four DIFFERENT addresses settled it in one run — and the
    lead was an artifact. When log archaeology supports several stories, stop reading
    and start arranging.
  ▶ **(SESSION 19) A NEGATIVE RESULT FROM A COMPLEX CLIENT IS NOT A PROPERTY OF THE
    PLATFORM.** "dpmitest's RMCS is corrupt and pmtick dies, therefore the kernel PM
    path cannot run guests" — then the simplest possible client ran to completion on
    it. Before concluding a path is broken, try the smallest thing that could work.
  ▶ **(SESSION 19) WHEN AN ARM INTERPRETS, MAKE IT LOG THE PRIMITIVES TOO.**
    `dpmi_crash_veh` printed a confident `AX=… -> UNSUPPORTED (CF=1)` and never the
    exception code or fault address. Adding four fields to one line refuted a
    two-session-old story in a single run. An instrument that prints its CONCLUSION
    but not its EVIDENCE is how a wrong story survives.

╔══════════════════════════════════════════════════════════════════════════════╗
║ THE USER'S INSTRUCTION (2026-08-22): "North star is playable Doom"           ║
╚══════════════════════════════════════════════════════════════════════════════╝

═══════════════════════════════════════════════════════════════════════════════
██  STANDING REFERENCE — rig operations, instruments, landmarks, older traps.  ██
██  Everything below is still true; the narrative history has been pruned.     ██
██  ⚠ "this session" in the material below means SESSION 17 or 15, not 18.     ██
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

