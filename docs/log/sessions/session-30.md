# Session 30 — 2026-08-26

Two halves. First: **the repository itself** — made public, history cleaned, tracker
reconciled with reality, docs consolidated, wiki published. Then: **the WOW/Win16
epic (#128)**, which is now on the critical path because #129 turned out to be
impossible.

---

## Part 1 — the repo can now be picked up cold

**Public, with a wiki.** GitHub *silently* refuses wikis on private Free-plan repos —
the API accepts the PATCH and `has_wiki` just stays `false`. Going public unlocked it.

⚠️ **Before publishing, history was audited — and it mattered.**
`tools/doomoracle/DOOM1.WAD` (4.2 MB) was still reachable in history. A past commit had
removed it saying *"it is id's, not ours"*, and publishing would have undone that,
because removing a file does not remove it from history. Purged with `git filter-repo`
along with three of Japheth's HX test binaries. `.git` went **55 MB → 4.4 MB**. Backup
bundle at `../ntvdmex-prepurge-backup.bundle`.
**When making any repo public, audit HISTORY, not just the tree.**

A full copyright audit followed: no Microsoft/IBM/id binaries were ever committed;
Nuked-OPL is fetched to a gitignored dir and never linked; the ReactOS-derived
`VDM_COMMAND_INFO` layout is an OS ABI. One residual: four annotated lines of ntoskrnl
disassembly in `docs/research/dpmi-under-ntvdmcontrol.md`, flagged but not changed.

**Tracker reconciled: 58 → 140 issues (43 open, 97 closed, 9 closed epics).**
`tools/gh/backfill.py` is the manifest — re-runnable, matches on title. Every closed
issue names its evidence. Four that *look* done are deliberately left open with the
missing half stated (#28, #34, #49, #56).

**Docs consolidated.** `return-ntvdm.md` (4,000 lines) split verbatim into this archive
— verified line by line, zero non-blank lines lost. `docs/STATE.md` rebuilt; it had said
*"M4 in progress"* for three weeks while Doom was fully playable. Wiki authored in
`docs/wiki/` and published by `tools/wiki/publish.sh` (repo is source of truth).

**The fresh-clone test found a defect in the docs written the same day.** Cloning the
public repo and following `STATE.md` verbatim: build fine, but the documented test loop
(`for t in *_test`) matches **nothing** — those binaries are gitignored. Under `bash`
that silently "passes" a loop that ran zero tests. `./tools/dostest/run.sh` is the entry
point; it compiles *then* runs. Also added a rig-provisioning page, since standing up a
new XP box from zero previously lived only in the scripts.

---

## Part 2 — #129 Win16 passthrough: IMPOSSIBLE, and that changes the roadmap

**Detection works.** Measured, not assumed — the host never logged its own command line,
so that instrument came first:

```
DOS : ntvdmhost.exe "…\ntvdm.exe" -f -i20
WOW : ntvdmhost.exe "…\ntvdm.exe" -f -i1 -w -a …\krnl386.exe
```

`-w` is the discriminator (some sources say `-m`; **not on XP SP3**). Second independent
tell: `GetNextVDMCommand` returns FALSE `err=0x57` on a WOW launch. Matched as a whole
token, not a substring — a DOS path containing `-w` handed to stock would be a worse
failure than the one the guard prevents.

**Handing the launch back cannot be done.** Three routes, all closed by measurement:

| Route | Result |
|---|---|
| Spawn `System32\ntvdm.exe` | IFEO is keyed on image **name**, evaluated inside `CreateProcess` → re-enters us → fork bomb per launch |
| Spawn a **renamed copy** | Child exits `rc=0xFF` instantly. *Not* a `STARTUPINFO` problem — passing our real one gave the identical result |
| Point `wowcmdline` at the renamed copy | **"sysedit.exe is not a valid Win32 application"** — Windows validates the VDM image's identity ⇒ this is the root cause of the `rc=0xFF` above |
| Repoint `Control\WOW\cmdline` | Does not route to us before **or after a reboot**; breaks DOS launching entirely |

The `cmdline`/`wowcmdline` split is real and tantalising (`-a …\krnl386` there matches the
live launch exactly) but repointing it does not work — the original "disproven" finding
stands, with the reboot hypothesis now eliminated.

⇒ **`wow_refuse()` ships**: detect, then fail loudly with a dialog naming the cause and
the exact `reg delete`. DOS regression checked — MS-DOS 6.22 still boots.

⇒ **There is no safe "leave it installed" story until WOW exists.** #130 (installer) is
blocked on #128, which moved from far-future onto the critical path.

---

## Part 3 — #128 WOW: the NE loader works

`tools/ne/nedump.py` was written **before** the C loader and pointed at real binaries,
because what they contain decides what must be implemented.

**`src/wow/ne.h`** — header-only, no Windows/VDM dependency, exercised off-VM by an 18th
battery (`ne_test`, 35 checks: synthetic image always runs; real-binary assertions SKIP
when the Microsoft files are absent).

**On the rig, krnl386 loads and relocates inside NTVDMEX:**

```
WOWTRY: parsed OK. segs=4 mods=0 movable=0xa4 align=4 autodata=4
  CS:IP=1:c02b  SS:SP=0:0  expects Win 3.10
  relocs seg 1..4 ok, cumulative sites patched=0x1ef
```

★ **495 patched sites from 13 relocation records.** That number is the whole argument for
getting relocation *chains* right: the obvious reading (one record, one fixup) would have
patched 13 sites and silently missed 97%, and krnl386 would have died somewhere unrelated.

### The LDT blocker — and a retraction

Selectors were refused with `STATUS_INVALID_PARAMETER_1`. Varying index/access/limit/base
changed nothing.

**I first concluded "refused on WOW launches, accepted on DOS launches". That was WRONG** —
the experiment had two variables (the WOW probe ran after `v86_init`, the DOS probe ~500
lines later). The clean 2×2 showed `dos-early` fails too. Bisecting:

```
dos-early / B-emsframe / C-csrss-register / D-after-getcommand   REFUSED
E-after-get-tib                                                  SUCCESS  ← flips here
```

**`v86_get_tib()` is what makes the kernel accept LDT entries** — not `csrss_get_command()`,
which was the seductive answer *because* it's the call that fails on `-w`. It isn't: D
still fails after the fetch succeeds. And `v86_get_tib()` works fine on a WOW launch, so
one call moved earlier **unblocks it**. There was never a WOW-specific restriction — an
ordering requirement the DOS path satisfied by accident of where its code sits.

### krnl386 is a LIBRARY — the execution plan was wrong

About to jump to `seg1:0xc02b`, re-read the header: `prog_flags 0x8309` includes `0x8000`.

```
krnl386 / user / gdi   LIBRARY  SS:SP=0:0  stack=0
wowexec / sysedit      PROGRAM  stack=0x2000
```

A DLL has **no stack of its own** and its `CS:IP` is an *initialisation* entry with the
Win16 DLL register convention. Jumping there would have produced a meaningless death that
looked like a finding.

**The real bootstrap, per the binaries:** init krnl386 → init user + gdi → run
**wowexec.exe** (the PROGRAM, first thing with a stack) → wowexec launches the app. Which
is also why the launch says `-a …\krnl386.exe`: the first *library* to bootstrap.

---

## ⚠️ Traps that cost rounds today

- **`WinMain` has THREE `log_write` calls and every one TRUNCATES.** A probe placed after
  the first, then the second, had its output silently erased by the next — reading exactly
  like "the code never ran". Cost three rounds. Diagnostics go after the **last** one
  (~line 9941) or into `p`. `C:\ntvdmex\ldtprobe.log` is now an untruncated sink.
- **Grepping the binary for a string proves nothing.** `zput` is `static inline`, so GCC
  turns short literals into immediate stores and they never reach `.rdata`. I nearly
  concluded a function had been optimised away.
- **SMB attribute caching served a stale copied log** — again. Copy to a fresh name.
- **`sips` crops from the CENTRE**, not the top-left. Two screenshots of grass.

---

## Part 4 — #128 imports: modules can now bind to each other

Item 2 of the list below, done — and it found two defects that would have produced a
guest that *starts* and then behaves like nothing on earth, which is the expensive kind.

`src/wow/ne.h` gained the four name tables (resident, non-resident, module reference,
imported names), export lookup by ordinal and by name, and an **`ne_registry`** that
turns an import record into a target module's `selector:offset`.

krnl386 was a poor test of any of this: **it imports from nothing.** `user.exe` and
`gdi.exe` import from KERNEL at 810 sites between them, and that is where the defects
were. All three findings below came from the binaries, not from the format documentation.

### ★ 1. Entry-table indicator `0xFE` is not a segment number

It is an **ABSOLUTE constant** — there is no segment, and the "offset" *is* the value.
krnl386 has 30:

```
@113 = 0x0003  __AHSHIFT      @174 = 0xa000  __A000H      @193 = 0x0040  __0040H
@114 = 0x0008  __AHINCR       @178 = 0x0001  __WINFLAGS   @574 = 0x0000  __MOD_GDI
```

Read as "segment 254" against a 4-segment module they are rejected — which is exactly
how gdi.exe first failed to relocate here, because **366 of its records import
`__MOD_GDI`**. `ne_export_by_ordinal` now returns segment number 0 to mean "no segment,
the value is the whole answer"; segments are numbered from 1, so 0 was free.

⚠ My first cross-check script said all of GDI's imports resolved. It was wrong in the
same way the loader was — I had written the ordinal walk by copying the loader's logic,
so the instrument inherited the defect. What caught it was the C failing where the
Python passed, then asking *which ordinal*.

### ★ 2. ADDITIVE means ADD, and we were replacing

Every ADDITIVE record in the corpus targets a `__MOD_*` whose value is **0**, so the
fixup itself is indistinguishable between add and replace. **The word already at each
site is not.** gdi's 366 sites hold `0x7b`, `0x7c`, `0x7d`, `0x7e`, `0x97`, `0xaf`… all
different. The bytes around one:

```
68 7e 00           push 0x007e          <- the site
9a ff ff 00 00     call far <KERNEL>    <- an unrelocated FAR_ADDR import
6a 06              push 6
```

A WOW thunk table whose pushed word is the **API index**. Replacing turns all 810 of
gdi's and user's into `push 0` and sends every call to function zero. Adding leaves them
alone, which is what a zero addend should do.

⚠ **The synthetic test's expectation for this had been written from memory, and was
wrong.** It asserted the site ended up holding the fixup value. The real binaries
refuted it. The cardinal M9 rule applies to WOW too.

### ★ 3. Import-by-name must search the NON-resident table

`user.exe` imports `KERNEL.GETWOWCOMPATFLAGSEX` **by name**, and that export is only in
krnl386's non-resident table (@521, one of **312** kept there). A lookup that stops at
the resident table finds none of them and fails on the first real binary. Entry 0 of
each table is skipped: resident entry 0 is the module's own name, non-resident entry 0
is the description (`"Microsoft Windows Kernel Interface Version 4.00"`), and both carry
ordinal 0.

### ★ Relocation is not idempotent — the host was relocating twice

Session 30's selector stage relocated with placeholder segment values at load time and
**again** against real selectors. That cannot work: a chained record finds its next site
by reading the word **at** the current site, and the first pass overwrites exactly those
words with addresses. The second pass follows garbage. It was written down as a curiosity
("we relocated twice"); it was a bug.

The host is now two phases, and the order is forced by the data:

1. `wow_load_modules()` — parse, allocate, copy bytes. **No relocation.**
2. `wow_bind_modules()` — a selector for every segment of every module, **then**
   relocate once through the registry.

`ne_registry_resolve` refuses a target whose selector is still 0, so getting the order
wrong fails loudly instead of writing `0000:xxxx`.

### Where it stands

The host loads **krnl386 + user + gdi + wowexec** together, from the directory of the
`-a` argument. Off-VM, against the real binaries:

| | |
|---|---|
| KERNEL | all relocations resolved, **495 sites** — matches the rig's `0x1ef` exactly |
| GDI | all resolved, **781 sites** |
| USER | stops at **SYSTEM** |
| WOWEXEC | stops at **KEYBOARD** |
| SYSEDIT | stops at **SHELL** |

Those three stops are not failures — they are the modules not extracted yet, and each
one **names itself** rather than dying somewhere unrelated. NE battery 35 → **106
checks**; suite 18 batteries, **736 checks**, exit 0.

Incidental, and worth remembering when the thunking work starts: krnl386 exports two
*unnamed* absolutes, `@454 = 0x001b` and `@455 = 0x0023` — NT's flat user-mode code and
data selectors.

---

## Part 5 — reading krnl386 instead of theorising about its init convention

Item 2 of Part 4's list was "work out the DLL initialisation calling convention".
The method that has paid off repeatedly on this project — **read the guest binary** —
answered it in minutes, and the answer is not the documented convention.

### ★ The init entry is gated on a magic value

```
c02b  cmp ax,0x4b4f      ; 'OK'
c02e  jz  0xc033
c030  xor ax,ax
c032  retf               ; ...otherwise it just returns 0
```

The host passes **AX = 0x4B4F**. It is *not* the documented Win16 `LibMain` convention
(`DI`=hInstance, `CX`=heap, `ES:SI`=cmdline): `DI` is pushed and used as a scratch four
instructions later. Entering with the LibMain registers would have made krnl386 return 0
on its second instruction — a "failure" that would have looked like a loader bug and sent
the next session after the wrong thing.

`DS` *is* live (`mov [0x59a],cs` / `mov [0x59c],ds` / `mov [cs:0x30],ds`), and it pushes
immediately, so a stack must be supplied — which the header cannot give, because a
LIBRARY has `SS:SP = 0:0`.

### ★ krnl386 is a DPMI client

Fifteen instructions in, before anything else:

```
c0f9  mov bx,[0x26f]
c0fd  mov ax,0x0002
c100  int 0x31           ; DPMI: segment -> descriptor
```

`tools/ne/neints.py` (new) reads the whole service list off the binary:

```
INT 31h  53 sites  0002 x12, 0203 x18, 0202 x9, 0006 x2, 0007 x2, 000A x2,
                   0000, 0001, 0301, and one non-standard 04F3
INT 21h  51 sites  AH=25h x12 and 35h x4 (vectors), 52h x2 (list of lists),
                   42h x6, 4Ch x4, 3Dh/3Eh/3Fh, 50h/55h/71h/DCh
INT 2Fh   5 sites  1600, 1684, 1687, 1689, 168A
INT 41h   6 sites  the Windows kernel-debugger interface
INT 11h, 10h, 2Ah, 5Ch
```

This is **good news**: krnl386 lands on the DPMI 0.9 host that already runs unmodified
third-party clients on real silicon, and on a DOS layer with 103 INT 21h functions. It
is not a new subsystem; it is a new client of two working ones.

### ★ DPMI 0002 was missing — and it is the one krnl386 calls most

Twelve sites, against two each for 0006 and 000A. Implemented with a **cache**, because
the same segment must return the *same* selector: the descriptor belongs to the host and
the client is told never to free it, so a fresh LDT entry per call would leak one per
call and let one part of a client modify a mapping another part is still using. `0001`
drops the cache entry if a client frees one anyway, so the next `0002` rebuilds instead
of returning a selector that is no longer present.

`04F3` is left **unimplemented and flagged**, not guessed at:

```
c8fc  mov bx,cs / mov si,bx / mov dx,0x4e81 / mov di,0x4ebb / mov ax,0x4f3 / int 31h
```

Coherent and aligned, so the call is real. The register shape is 0306's raw-mode-switch
pair being **set** rather than got — probably an NTVDM-private WOW extension. It needs
the kernel-RE treatment, not a plausible-sounding answer.

⚠ `neints.py` is a **linear sweep** and says so in its own docstring. Code segments
contain data, so a sweep decodes some of it as instructions and invents `int` sites.
Both of this project's instrument-lied incidents were this exact shape (session 21's
`CD nn` byte-pair patcher; session 18's count measured on the wrong bytes). It prints
the raw `0xCD` byte count as an upper bound and declares itself desynced if it ever
exceeds it — 122 found against 154 raw, so: an upper bound and a to-do list, not a census.

---

## Part 6 — asking what krnl386 needs, instead of building what it might

Part 5 produced a list of services krnl386 calls. The obvious next move is to implement
them. The cheaper one is to find out which are actually load-bearing — and most are not.

### INT 2Fh: five calls, four already correct, and none a blocker

`neints.py` found `1600`, `1684`, `1687`, `1689`, `168A`. Our handler answers exactly one
(`1687`). That reads as four gaps. Reading the **call sites** says zero:

| | krnl386's own code | verdict |
|---|---|---|
| `1600h` | `cmp al,3` at `c14b` — and **discards the flags**; the `jmp` at `c157` skips the block the comparison would have chosen | steers nothing |
| `1689h` | `c2f5f` jumps away without reading a register | fire-and-forget |
| `168Ah` | `cmp al,0x8a / jz` at `d6e9` — it tests for AL **unchanged**, meaning "not supported", and carries on | tolerates refusal |
| `1687h` | `or ax,ax / jnz` then `cmp cl,3` | already implemented |
| `1684h` | `xor di,di / mov es,di` at `2814` *before* asking, `or ax,di / jz` after | safe, but see below |

★ **Its `168Ah` vendor string, at autodata:`0x172a`, is `"MS-DOS"`.** So what krnl386 is
hunting for is NTVDM's private WOW API — and it **tolerates being refused**. That is why
this work can start at all without reverse-engineering that API first.

`1684h` was the one worth changing, and *not* because krnl386 needs it. It is a
pointer-returning call that was returning whatever happened to be in `ES:DI`, and the
caller far-calls the result. krnl386 pre-zeroes, so it was safe — but that is the
**caller** being careful, and it is not something to rely on from callers we have not
read. It now returns `ES:DI = 0:0` explicitly.

### The SysVars question — answered by the MS-DOS 6.22 oracle, and it answered "not here"

krnl386's init calls `INT 21h AH=52h` ten instructions in and then reads fields out of
the returned segment. We plant a stub whose only real field is the first-MCB word at
`BX-2` (#35, deliberately: a null pointer stops a memory walker where a garbage one sends
it wandering). So what is supposed to be there?

`tools/dostest/lolprobe.com` (new) asked genuine MS-DOS 6.22. Every documented field
checks out, the literal `NUL     ` device name included:

```
ES=0116 BX=0026
BX-02 first MCB  0x0253      BX+10 max bytes/block  512
BX+00 first DPB  0116:136a   BX+12 disk buffers     0116:006d
BX+04 first SFT  0116:00cc   BX+16 CDS array        0350:0000
BX+08 CLOCK$     0070:0059   BX+1a FCB table        031e:0000
BX+0c CON        0070:0023   BX+20/21  3 block devices, LASTDRIVE 5
```

⚠ **The dump is self-authenticating**, which matters on a project that has already lost a
session to a stale artefact: the SFT entries at `0x1A0` and `0x1E0` contain
`OUT     TXT` and `LOLPROBECOM` — the probe's own two files. It cannot be a leftover.

★ **But the field krnl386 actually wants is not a DOS field.** It reads `[ES:BX+0x6A]`
and treats the result as an offset to a structure holding six more offsets (`+00`, `+0C`,
`+10`, `+18`, `+24`, `+28`), later pairing each with a selector made by DPMI `0002` over
the SysVars segment. Under MS-DOS 6.22 `BX+0x6A` is `0x44B7` — past the documented list
of lists — and what is *at* `0x44B7` disassembles as DOS kernel **code**:

```
or al,al / jz / mov ah,0x3a / cmp byte [0x214c],0 / mov di,[0x216f]
```

not a table of offsets. So this is an **NTVDM contract**: ntvdm's DOS plants a WOW block
at SysVars+0x6A that MS-DOS never had. The oracle cannot answer it and it must not be
guessed.

### Where the blockers actually are

Two, and both are NTVDM contracts that no amount of reading our own binaries will settle:

1. **SysVars+0x6A** — what ntvdm plants there.
2. **`INT 31h 04F3`** — the non-standard DPMI call from Part 5.

Both fall out of **one cheap rig round**: run `lolprobe.com` under **stock ntvdm** (the box
already has `stock <target>`) and diff against the MS-DOS 6.22 baseline now on disk at
`docs/research/evidence/lolprobe-msdos622.txt`.

---

## Part 7 — the rig was never down, and the whole graph binds on real silicon

The "rig unreachable" in Part 6 was **my own sandbox blocking raw sockets**. The share was
mounted the entire time and the watcher was heartbeating. Worth remembering: a failed
`nc` from inside a sandbox is evidence about the sandbox, not about the box.

### ★ SysVars+0x6A is an NTVDM contract, and it is now measured

`lolprobe.com` under **stock ntvdm** (evidence: `docs/research/evidence/lolprobe-stock-ntvdm.txt`).
Stock returns `ES=00A7 BX=0026`, and `[ES:BX+6A] = 0x1482` — a plain offset *inside* the
SysVars segment, where MS-DOS 6.22 had DOS kernel code. At that offset is exactly the
structure krnl386 expects: **eleven far pointers**, every one segmented to the SysVars
segment itself, followed by code (`1E 50 B8 40` = `push ds / push ax / mov ax,40`).

```
+00 00a7:0047 <-krnl386   +04 00a7:003c            +08 00a7:13f3
+0c 00a7:0338 <-krnl386   +10 00a7:0332 <-krnl386  +14 00a7:0612
+18 00a7:0325 <-krnl386   +1c 00a7:13ca            +20 00a7:00ce
+24 00a7:0326 <-krnl386   +28 00a7:0328 <-krnl386
```

Every one of the six offsets krnl386 reads lands on a real entry. And **entry +0x20 is
`0x00CE`, which is exactly what the list of lists gives as the first SFT** (`00A7:00CE`)
— an independent identification proving the table is genuine DOS-internals pointers, not
a coincidence at the right offset. The MS-DOS 6.22 baseline was what made this legible:
without it, `0x1482` is just a number.

### ★ The entire XP WOW module graph closes

The three "stops" the loader reported — SYSTEM, KEYBOARD, SHELL — were a **shopping
list**. Pulled them off the box along with mouse, sound, comm, toolhelp, winnls, wifeman
and commdlg. **15 modules, every import resolved, and not one line of loader code
changed.** That is the whole payoff for making a failed import *name its module* instead
of just failing.

On the rig, all eleven the host loads bind, and the site counts match the off-VM battery
**to the digit**:

```
KERNEL 495   SYSTEM 16   KEYBOARD 22   MOUSE 0   SOUND 36   COMM 58
GDI 781      USER 1269   SHELL 74      TOOLHELP 138          WOWEXEC 144
```

27 LDT descriptors installed, 27 `LAR` readbacks ok, zero failures.
Evidence: `docs/research/evidence/wow-bind-rig.txt`.

### ★ And the readback caught a silent #GP before it happened

KERNEL's segment 1 logged `" CODE ... LAR ok"` — and read back **`ar=0xf200`**, a DATA
descriptor, while segments 2 and 3 of the same module read `0xfa00`.

`g_ldt_next` starts at **3**, and `dpmi_install()` **force-types indices 2 and 3 to
writable data**. That is a deliberate hack for the DPMI path — i310102's C runtime
retypes its first allocation to code and then `#GP`s on it — but WOW's first allocation
is not a stack. It is krnl386's **code** segment. The very next step of this work is
jumping to `sel:0xC02B`, which would have `#GP`d instantly, against a log claiming the
code selector installed cleanly. A silent death, far from the cause, of exactly the kind
this project keeps paying for.

Fixed by naming the reserved floor (`DPMI_LDT_RESERVED`, shared with the INT 31h `0001`
free path so the two cannot drift) and having the WOW stage step past it. Confirmed on
hardware: KERNEL seg 1 is now `sel 0x37 ar=0xfa00`, and across all 27 descriptors **zero
CODE reads back as DATA and zero DATA as CODE**.

★ **That readback exists precisely so the CPU, not our bookkeeping, says whether a
descriptor installed.** It has now paid for itself. Every "instrument" in this project
that merely re-reports what we wrote has eventually lied; this one asks the hardware.

## Next actions

1. **Enter krnl386.** Everything it needs is now either in place or measured: a code
   selector that is genuinely code, `AX=0x4B4F`, `DS`=autodata, a supplied stack (it is a
   LIBRARY, `SS:SP = 0:0`), 16-bit PM as a DPMI client.
2. **Plant a SysVars+0x6A table.** The shape is known — eleven far pointers into the
   SysVars segment. What the six krnl386 reads *mean* is still open; the offsets
   (`0x0325`, `0x0326`, `0x0328`, `0x0332`, `0x0338`, `0x0047`) cluster tightly, which
   suggests DOS internal flags. Identify them before inventing values.
3. **`INT 31h 04F3`** — the last NTVDM contract. Kernel-RE, not a guess.
4. Independent of WOW: **#131 console/stdio**.

**Not blockers, established by reading the call sites** — INT 2Fh `1600`/`1684`/`1689`/
`168A`, and the "MS-DOS" vendor API krnl386 asks for and tolerates being refused.

**Rig left with:** IFEO `Debugger` **set** (pointing at `C:\ntvdmex\ntvdmhost.exe`) and
`wowtry.flag` **present**; the WOW module set staged in `guest/ne/` on the build machine.
Clear both to return the box to stock.
