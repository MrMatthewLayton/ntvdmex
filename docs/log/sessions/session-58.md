# Session 58 — 86.9% unchanged, and four real defects were fixed anyway

> Session 58. Split out of `docs/STATE.md` on 2026-09-23.

> Verbatim from `docs/STATE.md`. Nothing has been edited or corrected,
> including conclusions a later session refuted — same rule as the rest of
> this archive. See [`README.md`](README.md).

## ★ SESSION 58 — 86.9% (unchanged, and four real defects were fixed anyway)

### ▶ ★★★★★ RESUME HERE: ZAR BANNERS AND LOADS. IT DOES NOT RUN.

```
DOS/4GW Protected Mode Run-time  Version 1.97
Copyright (c) Rational Systems, Inc. 1990-1994

Z.A.R.  Demo Version  v1.02 (net:v1.00)  Apr 06 1998 02:47:03
Copyright (C) 1997,1998 Maddox Games Ltd., Auric Vision Ltd.

Game loading...
```

That is on the XP desktop, in our VDM window. It was a 47 MB `#GP` loop this
morning. **It still does not run** — see "where it stops" below.

⚠ **THE DAY'S GOAL WAS >95% AND THE SCORE DID NOT MOVE AT ALL.** That is the
honest outcome and it is worth understanding rather than explaining away:
`guest-zar` is BINARY (playable or not), everything else touched was already at
1.0, and the user's own framing is the right one — *"if ZAR doesn't work then
NTVDMEX doesn't work"*, because the IFEO key routes EVERY DOS launch through us,
so a program stock runs and we refuse is a REGRESSION ON THE USER'S MACHINE, not
a missing feature. ▶ **The score model under-prices this**: `guest-zar` sits at
w3 in the DOS section, priced like a third game, when it is really evidence for
or against the superset claim the whole project rests on. Worth a deliberate
model change; it is the user's call, not one to slip in.

### ▶ FOUR DEFECTS FIXED, IN THE ORDER THEY WERE FOUND

**1. A 32-bit DPMI client got a 16-bit exception frame.** The rule was already
written in our own source for INTERRUPT frames (*"the frame width follows the
CLIENT'S MODE, not the handler selector's D bit"*) and had never been applied to
the EXCEPTION frame. Confirmed against the binary: DOS4GW's `#GP` handler reads
`[bp+0x12]`/`[bp+0x16]` = frame `+0x0C`/`+0x10`, the DPMI **32-bit** EIP and CS
slots, and leaves by `66 cb` (RETFD, eight bytes). Our 16-byte frame had nothing
at `+0x10`, so it loaded DS=0 and faulted on its own first memory read, for ever.
47,089,105 bytes of log → 252,718.

**2. ★★★★★ OUR INT-SITE PATCHER CORRUPTED A CALL — THIRD INSTANCE.**
`dpmi_patch_code_region` rewrote a call's DISPLACEMENT:
```
file  cs[0x5682] = e8 cd e4   call 0x3b52   (rel16 = 0xe4cd)
guest cs[0x5682] = e8 c4 c4   <- `cd e4` read as INT E4h, made a BOP
```
The x86len vote passed it correctly on its own terms — everything in front is
DATA (zeros), and an odd-aligned stream decodes `00 e8` as `add al,ch` and lands
exactly there. **The vote is a heuristic about where instructions START; it
cannot know a region is not code at all.** The corrupted call landed at `0x1b49`,
MID-INSTRUCTION, decoded as `mov ah,al / les ax,[di]`, RESYNCHRONISED at
`0x1b4f`, and so skipped both the `[0x34]` test and the `int 15h` the real
routine begins with — which is why neither ever appeared in the log and why two
earlier readings of this bug were wrong.
⇒ **THE 64K SCANNER ALREADY HAD THE ANSWER AND THIS SCANNER NEVER GOT IT**:
*evidence only — patch a vector only with a guest that provably needs it and a
service arm to receive it.* Now restricted to the vectors `dpmi_service_pm_int`
actually implements: `{08,10,11,15,16,1A,21,2F,31,33,41}`. Not patching is SAFE
(a raw INT in PM is serviced out of the `#GP`, session 34) — patching is an
OPTIMISATION and it has now cost three guests (Doom s21, CALC/TASKMAN/CARDFILE
s55, ZAR s58).

**3. `INT 31h AX=0200h/0201h` — get/set REAL-MODE interrupt vector.** DPMI 0.9
core, and we had NEITHER half while the protected-mode twins `0204`/`0205` worked
all around them. DOS/16M reads all 256 and installs 39; every one was refused.
The IVT is the store, not a shadow table — V86 `int nn`, `INT 21h AH=35h` and
this call must not disagree. Doom exercises it (99 get, 2 set).

**4. `INT 21h AX=FF80h` — "lock this region".** DOS/16M requires it and reads
CF=1 as fatal (`jae ok / push 0x22 / call fatal`; 0x22 is message 34 = `DOS/16M
error: [34] DPMI host error (cannot lock stack)`). Answered CF=0, for the reason
our own `INT 31h AX=0600` arm already gives: guest memory here is never paged, so
a lock is already true. Scoped to FF80h exactly.

**5. ★★★★★ THE SECOND PM RUN LOOP HAD NO FAULT ARM — this is the one that got
the banner.** `dpmi_dispatch_to_pm_handler` runs the client's own INT handler to
completion in a NESTED protected-mode loop. It handles PMRET, event 3 and I/O,
then hands everything else to `dpmi_service_pm_int` — and never had an arm for
THAT HANDLER FAULTING. The kernel reflects such a fault onto our fault-site stubs
as usual, but here the BOP was read as an interrupt: `dpmi_service_pm_int` saw the
stub's own `C4 C4 57` and dispatched it to the WOW handler, where **0x57 is the
WOW callback id — a genuine collision with `DPMI_FAULT_BOP`**. Verdict
"UNIMPLEMENTED, STEPPED OVER", exception never delivered, guest re-executed the
site for ever. Measured: `cs=0x017f == g_dpmi_flt_code_sel`, `eip=0x694 ==
DPMI_FAULT_SITE(13)` — a `#GP` inside DOS/16M's INT 21h handler while ZAR asked
the DOS version. **That is why ZAR printed nothing**: the call that would have
printed was the one being stepped over. Now handed back to the main loop, which
owns the whole delivery path. ⚠ Cannot affect WOW: a real WOW trampoline lives in
a guest 16-bit code selector, never in `g_dpmi_flt_code_sel`. Doom fires the new
arm ZERO times.

### ▶ AND ONE FEATURE: `dosenv.txt`

The guest environment was a hardcoded four (COMSPEC/PATH/PROMPT/BLASTER), so a
DOS program configured through its environment **could not be configured at
all**. One `NAME=VALUE` per line, `#` comments. MEASURED, not asserted — the host
dumps the block back OUT OF GUEST MEMORY after building it. ⚠ It does NOT fix
ZAR (DOS/4GW faults before reading `DOS4GVM`); kept because the gap is real.
⚠ Not applied on the WOW path: that block is krnl386's and it finds its own
executable by scanning to the double NUL.

### ▶ WHERE ZAR STOPS NOW — THE NEXT THREAD

It loads **~4.8 MB of a data file and then stops progressing**: at 45 s the last
read is `pos=0x49d454` (854 reads); at 130 s `pos=0x454c28` (1118 reads).
- **It never calls `INT 10h` AT ALL** — no video init is ever attempted.
- It settles into polling **its own** `INT 21h` hook (`0x027f:0x0084`) for
     `AH=2Ch` with NO `0302` round trip — ~156,000 iterations against two buffers
     (`0x04250030`, `0x0425185a`).
- Its idle is a **wait-for-next-tick** loop at `0x03b71317`, which spins only
     while `elapsed == 0`. NOT a hang: `pmwatch` shows the counter advancing one per
     ISR (`0x86→0x87 … 0x680→0x681`), `done=1` every time. **Our timer has the
     effect the guest is waiting for.**

▶ **NEXT STEP:** instrument the TRANSITION where the reads stop, not the steady
state. Catch the LAST `AH=3F` read's caller and follow it forward — that is a
different dig from anything tried today, and the steady-state sampling has
nothing left to give.

⚠⚠ **REFUTED IN SESSION 58 — DO NOT RE-TRY.**
- **CPU speed.** A 1998 game calibrating against 3.3 GHz is the classic failure.
     `cpuspd.txt=6` (233 MHz, Pentium MMX): identical behaviour.
- **The COM port.** The banner says `(net:v1.00)`, ZAR.CFG configures a modem on
     0x3F8 with an `"ATZ"` string, and s56 made our serial VDD claim that port and
     compute the equipment word from it — we now advertise TWO serial ports where we
     advertised none, and a modem init awaiting a reply would hang exactly like
     this. `net ComPortNum -1`: identical. (ZAR.CFG restored, share and box.)
- **Our `AH=2Ch`.** `GetLocalTime`, genuinely advancing — and the guest does not
     even ask us; it services `AH=2Ch` in its own hook.
- **`DOS4GVM=@ZAR.VMC`** (the game's own launcher sets it): reaches the guest,
     verified in the env block; DOS/4GW faults before reading it.
- **`INT 15h AH=88h`.** Chased TWICE and wrong both times. It is never called —
     DOS/16M installs its own PM INT 15h handler (its 33 `AX=0205` calls) and
     answers internally. ⚠ The `0x3C00` "matching the XMS pool" answer IS still a
     real double-count defect (a real HIMEM reports 0 once it owns the memory) —
     worth fixing deliberately, on its own merits, with Doom re-gated.
- **VESA.** `guest-zar` is filed as "needs VBE 2.0 hi-colour + LFB". **It does
     not.** ZAR's own `USER1.CFG` ships `VGA_320x200` (mode 13h, supported since M3)
     and it dies long before any video call. Do not plan the VESA work around ZAR.
- **MZ+LE page layout.** `DOS4GW.EXE` is a PURE MZ image (`e_lfanew` is garbage);
     the load module is CONTIGUOUS and `file = guest + 0x9B10` holds throughout. An
     earlier note here claiming otherwise was wrong — the real error was
     disassembling from an unaligned offset.

### ▶ INSTRUMENTS ADDED (all generic, all earned by getting something wrong)

| knob / output | what it answers |
|---|---|
| `dsprobe.txt` | named DS offsets dumped at every `#GP` — a guest's branch state |
| `csprobe.txt` | the same against CS — **guest code vs the file image**, which is what found the patcher corruption |
| `@ss:sp` at a `#GP` | who CALLED the faulting routine, off the guest stack |
| `@ds:0000`, `csbase`, `code[ip±0x20]` | locate a fault in a binary instead of guessing |
| `pmap` line in the WOWBOP report | **is this BOP ours or the guest's own `C4 C4`** — works for ANY guest; the old file-image check needed `g_wow_nmod` |
| `code@eip` on ASYNC-PM | what code an injected tick interrupted — turns "looping at X" into "polling Y" |
| `scripts/bm/zarlong.bat` | a run with REAL wall-clock, `nolog` for long ones |
| `scripts/bm/zarout.bat` | ZAR's stdout under BOTH hosts, in text |
| `rigshot arrange <exe> left\|right` | move windows by OWNING PROCESS (ours and stock share captions) |
| `scripts/bm/sxs.bat`, `scripts/bmsxs.sh` | a BATCH of guests up under both hosts at once, left running |

### ▶ TRAPS LEARNED THE HARD WAY THIS SESSION

⚠⚠ **THE SHARE ROOT IS PART OF THE HARNESS, NOT SCRATCH SPACE.** Tidying it
695 → 32 entries broke the rig TWICE: `wowtry.flag` is the **WOW opt-in** (its
absence made the whole shelf report *"16-bit Windows not supported"*), and the
root-level `.bat` files are called BY PATH from `rt.bat` and repo scripts, so
`bmqueue.sh doom` timed out with no result log. All 158 `.bat` restored; root is
~203 entries with the ~500 `.bmp`/`.log`/stale `.txt` still archived in
`archive/2026-09-08-pre-s58/`. Check anything moved against `rt.bat`,
`runwatch.bat` and every `%RES%\…` reference in `scripts/` — not just `src/`.

⚠⚠ **NEVER `start /wait` OUR HOST.** Without the `autoexit` marker it keeps its
window open after the guest ends, so `/wait` never returns and the box wedges —
it took a `controld kill` to clear, twice.

⚠ **THE HARNESS ITSELF STARTED LYING.** `rt.bat` caps a run at 45 s and the
compare scripts shoot at ~30 s. Fine while ZAR died in half a second; once it got
to "Game loading..." they cut it off mid-load and the evidence read *"no video
mode set"* when the truth was *"not finished yet"* (`HEADLESS: deadline
reached`). When a guest starts working, re-check the instrument's assumptions.

⚠ **AN INSTRUMENT THAT FAILS BY PRINTING A PLAUSIBLE WRONG ANSWER** is worse than
one that fails loudly: the first env-block dump appended into the running report
bounded by `p < base + 3800`, which was already passed, so every readable byte
was dropped and it printed `[......]` — indistinguishable from an EMPTY
ENVIRONMENT.

### ▶ REGRESSION BASELINES AT `4768150` (all re-run this session)

- off-VM battery: **62/62 + 27 + 27 + 48 + 23 + 17, 0 failed** (8 new `dosenv` checks)
- Doom: **all ten init markers, `STAGE2: complete`** — and it EXERCISES the new
     code (99 `getRMvec`, 2 `setRMvec`, 195 sites correctly left unpatched across 21
     regions), while firing the new fault arm ZERO times
- WOW gate: **110 / 113 / 32 · 0001:229C**, the s56/s57 baseline exactly

⚠ **CALC and WRITE were left on the rig under BOTH hosts awaiting a verdict and
never judged** — the day went to ZAR instead. `guests` is still the heaviest
lever at +0.26 per confirmation, and the user's standing rule is side-by-side,
which `bmsxs.sh` now does for a whole batch.

---
