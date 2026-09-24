# XP's own COMMAND.COM as a guest — what actually stops it

**Measured:** 2026-09-24, bare-metal rig, host `bbea3134`.
**Why:** *"If I open stock NTVDM on XP it gives me a DOS prompt, so NTVDMEX should
do the same"* — and the right shape for that is not a shell we write, it is
**`COMMAND.COM` as the guest**, exactly as stock does it.

---

## ✅ ANSWERED (s78): it never called DOS. It issued a BOP we do not implement.

**Read this before anything below it.** Several sections further down reason about
*why COMMAND.COM terminates*. It does not terminate. Everything in this file that
treats the exit as a DOS decision is superseded here.

The instrument was one line: **take the caller off the guest stack, not out of the
handler's registers.** A real-mode `INT` pushes IP, CS, FLAGS, so the call site is at
`SS:SP` — the same frame this code already trusts to return CF. With `cfg\dosver.txt`
= `5.0` and `cfg\dostrace.flag` set, the trace ends:

```
  21:19/00 bx=0001 dx=0001 @00009342:0000038b      <- a real call: sane caller
  21:00/02 bx=0001 dx=95d7 @00000024:00000000      <- the "terminate": caller is GARBAGE
  ==> DOS terminate (AH=00h) ... site=0x0000 bytes@0x0000=58 00 50 00 58 00 ...
```

Every genuine call has a coherent caller (`0100:xxxx` in the resident part, then
`9342:xxxx` in the transient). The terminate reports `0024:0000`, pointing at data.
**There is no pushed interrupt frame, so it did not arrive by `INT`.**

What is at `9342:03ce` is COMMAND.COM's own `C4 C4 54` — a **BOP**. XP's COMMAND.COM
is NTVDM-*aware*: its 50,620-byte image contains **sixteen `C4 C4` sites — fifteen
`BOP 0x54` each followed by a sub-function byte (15 distinct ones), and one
`BOP 0x50`**. Our own numbering collides with both:

| BOP | real NTVDM guest use | ours (`main.c:448`, map at `main.c:499`) |
|---|---|---|
| `0x50` | COMMAND.COM uses it (1 site) | DPMI real→PM entry |
| `0x54` | COMMAND.COM uses it (15 sites, sub-function byte) | `DPMI_RMRET_BOP` |

And the exec loop's last arm hands **any BOP no arm matched** to `dos_int21()`
(`main.c` ~26932). INT 21h's own stub is `C4 C4 20`, so that fall-through was never
required — it just answers for everything. `BOP 0x54 / sub 0x01` arrives with
`AX=0x0002`, is read as INT 21h `AH=00`, and terminates the guest, which we then
report as a **clean exit, code 0**. A guard that returns success is a lie the whole
stack repeats — this is that, one level down.

Confirmed end-to-end by a diagnostic on the fall-through itself:

```
STAGE2: BOP FALL-THROUGH -> INT21: bop=0x54 next=0x01 at 0x00009342:0x000003ce ax=0x00000002
```

⚠ **Behaviour is deliberately unchanged.** Gating the fall-through on `0x20` is
probably right, but the arms above it are long and some may fall through on purpose.
The diagnostic enumerates what actually reaches it across the guest battery first.

### What this retires

- ⛔ *"COMMAND.COM exits at the same point whatever we service"* — true, and it was
  never about what we serviced. `AH=53h`, `AX=122Eh`, the INT-site patcher: all three
  were investigated as causes of an event that is not a DOS call. (`122Eh` is still a
  real gap that is now closed; `53h` returning CF=1 is still worth fixing.)
- ⛔ The byte dump at `VTIB_CS:EIP` on the terminate path. It was reading the address
  the *handler* held. Replaced with `dos_int21_callsite()`, which dumps around the
  guest's pushed return address — and prints `from=<PM: no pushed frame>` rather than
  inventing one in protected mode.
- ★ **An address a handler happens to be holding is not evidence about who called it.**
  Three sessions of readings came off that mistake, and the fix was one 32-bit load
  from a frame the surrounding code already depended on.

### Next

1. Enumerate BOP fall-throughs across the guest battery, then gate it.
2. Find out what NTVDM's `BOP 0x54` sub-functions *are* — **measured, not remembered**.
   The 15 sub-function bytes and their call sites are in the image; stock ntvdm on the
   same box is the oracle.
3. Only then: the DOS 5.0 auto-report, the no-guest default, PIF.

---

## It is not "we need a shell" — it loads and runs today

`C:\WINDOWS\SYSTEM32\COMMAND.COM` (50,620 bytes) **loads and runs** under NTVDMEX
today. Launched with `scripts/bm/dosrun.bat - C:\WINDOWS\SYSTEM32\COMMAND.COM`:

```
STAGE2: loaded 0x0000c5bc from C:\WINDOWS\SYSTEM32\command.com
STAGE2: running .COM (entry 0x0100:0x0100)
STAGE2: BOP2F ax=0xb700   APPEND install check
STAGE2: BOP2F ax=0x4300   XMS install check
STAGE2: BOP2F ax=0x4310   XMS entry point
STAGE2: BOP2F ax=0x5501   COMMAND.COM interface
STAGE2: BOP2F ax=0xb707
STAGE2: BOP2F ax=0x122e   ×5      <- then it terminates, cleanly, printing nothing
STAGE2: complete
```

## ⛔ The DOS version IS necessary — I said it was a red herring and it is not

`main.c` (~23206) records that XP's COMMAND.COM refuses 6.22 outright — *"Incorrect DOS
version"* — and there is a `cfg\dosver.txt` knob. I wrote here that **"forcing 5.0
changed nothing: the same five calls, the same clean exit."** That was wrong. I compared
the two runs' SUMMARY lines instead of their calls. Counted properly:

| reported version | INT 2Fh calls COMMAND.COM makes |
|---|---|
| **6.22** (our default) | **zero** — it refuses at the version check and exits |
| **5.0** | **nine** — `b700 4300 4310 5501 b707` then `122e` ×5 |

⇒ **The version override is the difference between "refuses immediately" and "gets to
the last step before printing".** It is step one, not a distraction.

⚠ The user found this, not me: they opened `System32\COMMAND.COM` by hand with the
override removed and got *"Incorrect DOS version"* on screen — the failure I had just
told them did not matter.

⚠ **And one of my own readings was wrong on the way here.** I reported *"zero INT 21h
calls — the classic tell that the guest did nothing"*. The guest was running the whole
time; my grep matched one log format while these are logged as `BOP2F` lines.
**An absence produced by a bad pattern is not an absence in the report.**

---

## What `AX=122Eh` is, read off COMMAND.COM itself

`from=` in the log is our own INT 2Fh BOP stub, not the caller, so the call sites were
found statically: seven `B8 2E 12` (`mov ax,122Eh`) in the binary, **four consecutive at
file offsets `0x7d2b`–`0x7d58` and a fifth at `0x7d85`** — exactly the 4+1 observed.
Disassembled (`.COM`, so guest offset = file offset + `0x100`):

```asm
00007E25  xor cx,cx
00007E27  mov es,cx
00007E29  xor di,di            ; ES:DI = 0000:0000 GOING IN
00007E2B  mov ax,0x122e
00007E2E  mov dl,0x0           ; DL selects the table: 0, 2, 4, 6, then 8
00007E30  int 0x2f
00007E32  mov word [0x917a],es ; ...and it STORES WHAT COMES BACK
00007E36  mov [0x9178],di
          ; repeated verbatim for dl=2 -> [0x9182]:[0x9180]
          ;                      dl=4 -> [0x9192]:[0x9190]
          ;                      dl=6 -> [0x9176]:[0x9174]
00007E67  call 0xaf03          ; two more table getters that are not 122Eh
00007E7A  call 0xaf24
00007E85  mov ax,0x122e
00007E88  mov dl,0x8           ; the fifth -> [0x919a]:[0x9198]
```

⇒ **`AX=122Eh` with `DL=n` is a GET: it must return a far pointer in `ES:DI`.**
COMMAND.COM zeroes `ES:DI` first, calls, and stores the result into its own
table-pointer variables.

**Our INT 2Fh handler passes everything it does not recognise straight through** — a
deliberate policy, and documented as such (`main.c` ~24866: *"we pass everything we do
not recognise straight through, so the guest reads its own registers back as our reply
— the same 'does nothing, reports success' shape as the DPMI 0300 bug"*). Here the
guest supplied `0000:0000`, so it reads `0000:0000` back and **stores five null
pointers**, then terminates without printing.

⚠ `DH` in the log is `0xBA` — leftover, because the code only ever sets `DL`. Only
`DL` is the selector.

⚠ **What the five tables CONTAIN is still unknown and is not guessed here.** The
observed facts are the selector values and that a far pointer is expected back.

---

## ✅ MEASURED: what a working DOS actually returns

`tools/dostest/p_int2f.asm` asks exactly what COMMAND.COM asks — same `AX`, same `DL`
values, `ES:DI` zeroed first. Two real Microsoft kernels (6.22 under QEMU, and PCem):

| call | 6.22 | PCem | ours |
|---|---|---|---|
| `DL=0` | `0001:0D8F` | `0001:0D8F` | `0000:0000` ⛔ |
| `DL=2` | `0001:0B3B` | `0001:0B3B` | `0000:0000` ⛔ |
| `DL=4` | `0001:0D8F` | `0001:0D8F` | `0000:0000` ⛔ |
| `DL=6` | `0000:0000` | `0000:0000` | `0000:0000` ✅ |
| `DL=8` | `03E7:0188` | `03EA:0188` | `0000:0000` ⛔ |
| `AX=5501h` | unchanged | unchanged | unchanged ✅ |

**7 mismatches.** Three things fall out, none of them guessed:

1. ★ **`DL=6` is legitimately null on real DOS too.** A null is not automatically fatal —
   which kills the tidy story that COMMAND.COM "dies the moment it needs one".
2. ★ **`AX=5501h` is not a gap.** All three leave it unchanged; only a resident
   COMMAND.COM answers it. One less thing to build.
3. ★ **The contents are build-specific, so there is nothing canonical to copy.** With the
   pointers dereferenced (32 bytes each), `DL=8`'s table is byte-identical on both
   machines (`CD024102…`) but `DL=0/2/4` differ — they point into low kernel memory
   (`0001:0D8F` ≈ linear `0xD9F`). ⇒ **The requirement is four dereferenceable pointers
   and a null, not specific bytes.**

⛔⛔ **And the first cut of that dump manufactured an agreement.** It emitted only the
bytes, with a guard leaving the buffer zeroed on a null pointer — so *our* null emitted
sixteen zeros, the real machines emitted sixteen zeros they had actually read, and the row
came back **AGREE**. Fixed with a `seen` flag per table, so "did not look" and "looked,
and it was zeros" are different values. **Same shape as the floppy `DOR` mask, twice in
one day.**

## ✅ IMPLEMENTED — and it was NOT the blocker

`INT 2Fh AX=122Eh` is served (`main.c`, the BOP2F arm). The tables live at
`DOS_INT2F_TBL_A/B/C` in `DOS_CTAB_SEG`, in space reclaimed from **`DOS_CDS_OFF`, a dead
reservation**: its comment still claimed `0x4E0..0x697` for the CDS array, but s71 moved
that array into its own block and left the define behind, referenced by nothing. *A stale
reservation is worse than no reservation — it reads as "occupied" for ever.*

Verified against both real kernels, on the facts that are machine-independent:

| case | 6.22 | PCem | ours |
|---|---|---|---|
| `int2f.122e.nonnull` (bitmask) | `0017` | `0017` | **`0017`** ✅ |
| `int2f.122e.dl0eq4` | `0001` | `0001` | **`0001`** ✅ |
| `dl0/2/4/8.seen` (dereferenced) | 1 | 1 | **1** ✅ |

⛔⛔ **The probe's first cut compared the POINTERS, and that was seven permanent false
mismatches.** They are addresses — there is no reason ours should equal another machine's,
and the two oracles do not agree with each other either (`DL=8` is `03E7` on 6.22 and
`03EA` on PCem). Left alone they would have parked seven rows that read MISMATCH for ever
in the parity score this session had just spent the morning making honest. Replaced with
**relationships**: which selectors answer non-null, and whether `DL=0` and `DL=4` hand back
the same pointer. Both are facts about the service; neither depends on where DOS loaded.

### ⚠ And COMMAND.COM still exits at exactly the same point

```
STAGE2: 2F/122E dl=00 -> ES:DI=0x0090:0x04e0
STAGE2: 2F/122E dl=02 -> ES:DI=0x0090:0x0520
STAGE2: 2F/122E dl=04 -> ES:DI=0x0090:0x04e0
STAGE2: 2F/122E dl=06 -> ES:DI=0x0000:0x0000
STAGE2: 2F/122E dl=08 -> ES:DI=0x0090:0x0560
STAGE2: task done
```

All five answered correctly, and it terminates anyway. **This was predicted here before it
was implemented** — *"the five `122Eh` calls are only the last INT 2Fh calls we LOG, and
COMMAND.COM has more initialisation after them"* — and it is the useful kind of negative
result: a real gap is closed and the search has moved on.

Disassembling past the fifth call, the next thing it does is more variable init, another
getter, and then **`INT 21h AH=63h`** (get DBCS lead-byte table) at `0x7EC4`.

⛔ **The limiting factor is now visibility, not any one missing call.** The host logs
**zero INT 21h lines for this guest**, so what happens after the tables is invisible.
Guessing the next blocker from the disassembly alone is exactly the "read the evidence
backwards" mistake this page already records once.

## ⛔⛔⛔ The repo had already warned me, in the exact words

There was no need to build a trace: `m.trace_all` has existed all along, gated by
`cfg\dostrace.flag` and described as *"a differential instrument, not a default"*. It only
printed the call, not the answer, so a **result line was added** (`dos_int21.c`) — that
half was genuinely missing, and "which call came back an error" is unanswerable without it.

With it on, XP's COMMAND.COM makes **31 INT 21h calls and never prints anything**, ending
at `AH=00h` (terminate) from:

```
==> DOS terminate (AH=00h) from CS:IP=0x9342:0x03ce
```

⚠ **The previous session recorded `0x95eb:0x03ce`.** Different segment — COMMAND.COM
relocates its transient portion, and where depends on the memory map — but **the same
offset, the same instruction.** It dies in exactly the same place with `122Eh` now fully
answered.

And `dos_int21.c`'s own `AH=53h` handler already said so, before I started:

> *"TESTED AS THE CAUSE OF THE COMMAND.COM EXIT, AND REFUTED… Answering SUCCESS with a
> zeroed DPB was tried: the shell still exits, at the SAME CS:IP, after the same 32 ms…
> What COMMAND.COM asks for and does not get is INT 2Fh AX=122Eh… **That is the next thing
> to chase, and "died after" is still not "died because": prove it before implementing
> it.**"*

⇒ A previous session had **already identified `122Eh` as the suspect, already refuted
`53h`, and already written down the warning against implementing on "died after"
reasoning**. I rediscovered the suspect from scratch, implemented it, and got precisely
the outcome that note predicts. **The work was not wasted — `122Eh` was a real gap and is
now correct against two kernels — but the method was: I should have grepped the handler
for the thing I was about to chase.**

## ▶ What is actually next

The trace's results narrow it, and the narrowing must be done by comparison, not by
reading:

| call | our answer | status |
|---|---|---|
| `AH=53h` | `AX=0001 CF=1` (explicit error) | **refuted as the cause**, twice now |
| `AH=63h` | `AX` unchanged | get DBCS lead-byte table — suspect |
| `AH=5Dh AL=08/09` | `AX` unchanged | suspect |

⚠ **"AX unchanged" is NOT a reliable test for "not serviced."** `AH=25h` (set vector),
`33h` and `50h` return nothing in AX and look identical on a perfectly working DOS. I
nearly reported a list of false gaps off that heuristic — a property check passing by
luck, the same shape as the DOR mask and the table-bytes dump.

## ✅ All three suspects were already implemented — the heuristic was false

Grepping the handlers first (the lesson from one section up) settled all three in minutes,
with no probe needed:

| call | verdict |
|---|---|
| `AH=63h` | **implemented** — returns `DS:SI` = a real DBCS table and clears AL. `6300 & 0xFF00` *is* `6300`, so "AX unchanged" was pure coincidence |
| `AH=5Dh AL=08/09` | **implemented** — accept-and-ignore, with a note explaining that is what DOS does with no redirector, and that refusing "would make the shell think something failed" |
| `AH=53h` | **implemented as an explicit error**, and already refuted as the cause |

⇒ **Every INT 21h call XP's COMMAND.COM makes during startup is serviced.** The exit is
not a missing DOS call. And the "AX unchanged ⇒ not serviced" heuristic produced **three
false gaps out of three** — I would have chased all of them.

## ⚠ The bytes at the exit — an anomaly, recorded, NOT explained

Following the standing rule (*silent death → get the bytes*), the `AH=00h` terminate log
now dumps the instruction stream around the call site. Run with the trace on:

```
==> DOS terminate (AH=00h) from CS:IP=0x9342:0x03ce
    bytes@0x03ae= ... 26 8b 16 2d 03  89 16 eb 95  07  ba d7 95  c4 c4 54 01 73 62 ...
```

Disassembled, the run-up is ordinary and the last instruction before the site is
`mov dx,0x95d7` — which matches the trace exactly (`21:00/02 … dx=95d7`). Then at
`0x03ce`, where CS:IP points:

```
000003CB  BA D7 95     mov dx,0x95d7
000003CE  C4 C4 54 ...
```

⚠⚠ **`C4 C4 nn` is our own BOP encoding, and this does not add up:**

- The BOP number reads **`0x54`**, but INT 21h is serviced as **BOP `0x20`**.
- The only thing that writes `C4 C4` into a *guest's* code is the **INT-site patcher**,
  and that is explicitly a **protected-mode** device — *"a raw `CD nn` in PM is the one
  fault XP will not reflect"*. COMMAND.COM is a real-mode DOS program.
- A real-mode INT 21h should reach us through the IVT stub in `DOS_HDLR_SEG`, in which
  case the live `CS` at the BOP would be `0x0050`, not the guest's `0x9342`.

⛔ **Three readings are possible and I am not choosing between them without evidence:**
the site really was patched (and the patcher is reaching a guest it should not); the
logged `CS:IP` is not the INT site; or those bytes are COMMAND.COM's own data and the
whole reading is wrong. ⚠ This project has a standing note that **the INT-site patcher has
broken guests five times** (Doom's jump table four, heaven7 once) and ships a
`cfg\nopmpatch.flag` specifically to A/B it.

## ✅ A/B run — the patcher is exonerated, and my reading was wrong twice

Same build, `cfg\nopmpatch.flag` present: **byte-for-byte identical.** Same `CS:IP`, same
`c4 c4 54 01 73 62`. And the run's log contains **no patch activity and no DPMI at all** —
the patcher never ran, so it could not have been implicated.

Two of my readings collapse with it:

1. ⛔ **"BOP number `0x54`" was nonsense.** `dpmi_repatch` replaces `CD nn` with a
   **two-byte** `C4 C4` *by design* — the vector comes from the recorded site map
   (`g_pmap_vec`), not from a third byte. So the `54` was never a BOP number; it is simply
   the next instruction.
2. ⛔ **Those bytes are COMMAND.COM's own.** Nothing of ours is at that address.

### ⚠ Which means the logged CS:IP is probably NOT the call site — and that undoes an
### earlier claim of mine on this page

If no patch happened, there is no `C4 C4` BOP at `0x03ce`, and `CD 21` is not there either
(`mov dx,0x95d7` occupies `0x3CB`–`0x3CD`, so `0x3CE` is where the bytes are, and they are
`C4 C4`). **So the INT 21h did not come from `0x9342:0x03ce`.**

⛔ Earlier this page said: *"the previous session recorded `0x95eb:0x03ce` … the same
offset, the same instruction. It dies in exactly the same place."* **That inference is now
weaker than stated.** The offset reproducing across runs is real and worth something — it
is deterministic — but calling it "the same instruction" assumed the address was the call
site, and the bytes there say it is not. The byte-dump instrument answered a question
about the wrong address.

▶ **Next: get the caller from the guest stack, not from `CS:IP`.** A real-mode INT pushes
`IP`, `CS`, `FLAGS`, so the true call site is at `SS:SP` — and this codebase already uses
exactly that technique elsewhere (*"LOG WHO CALLED, NOT JUST WHAT THEY ASKED … the return
address is sitting on the guest stack at SS:SP"*, the XMS entry logger). That is the
instrument to add, and it should have been the first one: **an address a handler happens
to be holding is not evidence about who called it.**
**That is one run, and it decides between the readings without arguing about them.**

## ⏸ The placement, and why it took care

The four tables need a home in low memory. `dos_layout.h`'s own high-water mark is
`DOS_CDS_OFF` at `0x04E0` in a block ending near `0x06F0`, and that file is a catalogue of
what goes wrong there: a placement on linear `0x714` "breaks EVERY guest, selftest
included"; a zeroed table at SysVars+0x6A had **krnl386 writing through it into our own
handler code**. Choosing a spare ~200 bytes against a drive table whose length is
unverified is not a thing to do quickly.

**What is left to do, in order:**

1. Verify the free extent of the `DOS_CTAB_SEG` block (what `DOS_CDS_OFF` actually spans).
2. Place four small tables there; return them for `DL=0/2/4/8`, null for `DL=6`.
3. Report DOS **5.0** when the guest is `command.com` — measured above as the difference
   between "refuses at the version check" and "reaches the table calls".
4. Re-run `dosrun.bat - C:\WINDOWS\SYSTEM32\COMMAND.COM` and see how much further it
   gets. ⚠ **It may not be the last blocker** — the five `122Eh` calls are only the last
   INT 2Fh calls we log, and there is more initialisation after them.
5. Only then: default to `COMMAND.COM` when nothing is named, strictly **below**
   `target.txt`.

## The older plan, kept because the machinery exists

XP's COMMAND.COM **works under stock ntvdm on this same box**, so stock implements
`122Eh` and the correct answers are sitting in memory a few feet away. The machinery to
read them already exists:

1. `scripts/bmstockdump.sh` dumps a **live stock VDM** from outside with `vdmdump.exe`.
2. Run COMMAND.COM under stock (`scripts/bm/w16stock.bat` is the pattern for dropping
   the IFEO key and restoring it), dump, then read COMMAND.COM's own variables at
   `[0x9178]/[0x917a]`, `[0x9180]/[0x9182]`, `[0x9190]/[0x9192]`, `[0x9174]/[0x9176]`,
   `[0x9198]/[0x919a]`.
3. Those five far pointers name the tables; dump the bytes at each.

⇒ The format does not have to be recalled or inferred from a spec we do not hold. **It
can be read off the machine that already answers correctly.**

---

## What the prompt actually costs

| | |
|---|---|
| **Default to `COMMAND.COM` when nothing is named** | Small — one more rung on the target chain. ⚠ It must sit strictly **below `target.txt`**, or the headless harness (which relies on "CSRSS named nothing → `target.txt`") breaks |
| **Report DOS 5.0 for it** | Trivial — the knob exists; make it automatic when the guest *is* `command.com` rather than a file on the share |
| **Service `INT 2Fh AX=122Eh`** | **The real work.** Five far pointers to correctly-shaped tables. Bounded, and with a live oracle for the contents |
| **Whatever it asks next** | Unknown until `122Eh` answers. The same loop applies |

⛔ **`cfg\dosver.txt` was REMOVED from the rig afterwards.** Left at 5.0 it would have
made every later DOS test report a different DOS version with entirely plausible logs —
the `pmnoirq.flag` shape.

A copy of the binary under test is at `debug\out\XPCMD.BIN` on the share (not committed
— it is Microsoft's).
