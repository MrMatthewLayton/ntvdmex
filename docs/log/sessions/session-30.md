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
| `168Ah` | `cmp al,0x8a / jz 0xd71b` at `d6e9` — tests for AL **unchanged** | ⚠️ **REFUTED in Part 9: `0xd71b` is the ABORT path** |
| `1687h` | `or ax,ax / jnz` then `cmp cl,3` | already implemented |
| `1684h` | `xor di,di / mov es,di` at `2814` *before* asking, `or ax,di / jz` after | safe, but see below |

★ **Its `168Ah` vendor string, at autodata:`0x172a`, is `"MS-DOS"`.** So what krnl386 is
hunting for is NTVDM's private WOW API.

⚠️ **THE TWO LINES ABOVE WERE WRONG, AND PART 9 REFUTES THEM ON HARDWARE.** They said krnl386 "tolerates being refused" and that this work could therefore start without the vendor API. It cannot: `0xd71b` is the **abort path** — the same one the failed-mode-switch and wrong-CS-RPL checks branch to — and it prints `NTVDM KERNEL: Inadequate DPMI Server`. I read the comparison and inferred the outcome from the *sense of the test* rather than following the **jump target**. The `168A` vendor API is **required**.

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

---

## Part 8 — ⚠ THE ENTRY PLAN WAS BACKWARDS: krnl386 starts in V86, not protected mode

Part 7 ended with "next: enter krnl386 in 16-bit PM as a DPMI client". Before writing that
code I read the entry path properly. **It is wrong**, and writing it would have produced a
`#GP` on the third instruction with no obvious cause.

### The proof, from three independent places in krnl386's own code

**1. It does segment arithmetic on `ES`.** At `c045`, ten instructions in:

```
mov ax,es / shl ax,4 / add ax,0x86 / sub ax,0x400
```

`ES << 4` is only meaningful if ES is a **real-mode paragraph**. In protected mode it is a
selector and that shift is nonsense. And it is not nonsense — it computes exactly the
right answer against *both* oracle dumps (MS-DOS: `0x116<<4 + 0x86 - 0x400 = 0xDE6`, and
`0040:0DE6` → linear `0x11E6` = `ES:0x86` ✓; stock ntvdm: `0x0A7<<4` → `0040:06F6` →
`0xAF6` = `ES:0x86` ✓).

**2. It stores through `CS`.** At `c03b`: `mov word [cs:0x30], ds`. A code selector is
never writable on x86 — that store `#GP`s in protected mode. In V86 it is ordinary.

**3. It switches modes itself, and then redoes that store the legal way.** At `c0c2` —
before anything else of substance — it calls the `INT 2Fh 1687` DPMI installation check,
then:

```
pop ax / add ax,0x10 / mov es,ax     ; ES = real-mode paragraph of the DPMI private area
xor ax,ax                            ; AX=0 -> 16-BIT client
call far [0x1726]                    ; the mode-switch entry 2F/1687 handed back
jc  <fail>
mov ax,cs / and al,7 / cmp al,7      ; ...did we land on an LDT selector at RPL 3?
mov bx,cs / mov ax,0x000A / int 31h  ; create a DATA ALIAS of CS
mov [0x598],ax
mov ds,ax
mov [0x30],bx                        ; <-- THE SAME STORE AS c03b, offset 0x30, redone
```

**The same store to offset `0x30`, once per mode.** In V86 through `cs:`; in protected
mode through a DPMI alias, because by then it cannot write through `CS`. That is not
inference — it is the binary doing the identical thing twice and telling you why.

### The corrected model

```
1. entered in V86 at seg1:0xC02B, AX = 0x4B4F ('OK')
2. reads DOS SysVars (INT 21h AH=52h) and the +0x6A table
3. INT 2Fh 1687  -> our DPMI host's mode-switch entry
4. far-calls it with AX=0  -> 16-bit protected mode
5. checks cs & 7 == 7      -> confirms an LDT selector at RPL 3
6. INT 31h 000A            -> data alias of CS
7. INT 31h 0002 x12        -> turns the paragraphs it already knows into selectors
```

★ **This is why `0002` is the function it calls most** — twelve sites — and it is the one
that was missing until Part 5. Every step above is machinery NTVDMEX already has and has
proven on real silicon: V86 execution, INT 21h, `2F/1687`, the mode switch, `000A`, `0002`.
krnl386 is not a new subsystem. It is a new client of working ones.

### What this costs, and what it does not

The selector stage in Part 7 is **not** the entry path: it gave NE segments LDT
descriptors, and the entry needs them in **V86 memory with real-mode paragraph values**.
Relocation itself needs no change — `ne_apply_relocs` writes whatever segment value it is
given, and a paragraph is just a different number.

Not wasted, either: that stage proved LDT installation works on a WOW launch, proved
relocation against real selectors, and found the force-typed-index bug. But the plan it
was heading toward was aimed at step 4 of the list above, skipping steps 1-3 — entering
directly in protected mode, at an address whose first actions assume real mode.

⚠ **The lesson is the one this project keeps relearning.** "krnl386 is the 386
enhanced-mode kernel, therefore it runs in protected mode" is a plausible inference from
what the thing *is*. It survived four parts of this session in the comments and the
docs. What killed it was reading the instructions.

---

## Part 9 — ★ krnl386 RUNS, switches itself to protected mode, and names its own blocker

Implemented the corrected model from Part 8 and ran it on the rig. It works.

### The V86 placement

Conventional memory is entirely **owned** at that point — `dos_mcb_init` lays one `Z`
block over all `0x9F00` paragraphs, owned by the PSP, which is faithful (real DOS gives a
`.COM` the whole arena and the program shrinks it). The first run said

```
WOWV86: no conventional memory for seg 1, largest free 0x0 paras
```

which reads like exhaustion and is actually *"everything is owned, nothing is free"* —
precisely what a DOS program sees before it calls `AH=4Ah`. Shrinking the PSP block
through `dos_resize()` (not by poking the chain, so the MCB invariants stay true) fixed it:

```
WOWV86: seg 1 CODE len=0xd7fa -> para 0x0141   seg 3 CODE -> para 0x12b2
WOWV86: seg 2 CODE len=0x3ee2 -> para 0x0ec2   seg 4 DATA -> para 0x13db
WOWV86: relocated to paragraphs, sites=0x1ef
ENTRY CS:IP=0141:c02b  DS=13db  SS:SP=1597:0ffe  AX=4b4f ('OK')
```

**495 sites again** — the same count as against selectors, because a paragraph is just a
different number to write.

### It runs, and every step of the predicted model happens

```
STAGE2: WOW entry -- krnl386 in V86 at 0x141:0xc02b DS=0x13db AX=0x4b4f
  INT21 AH=52 list-of-lists                          <- ten instructions in
STAGE2: BOP2F ax=0x1687 ... from=0x50:0x40           <- finds the DPMI host
STAGE2: DPMI 1687 -> AX=0 ES:DI=0x50:0x50
STAGE3: DPMI_BOP far-call LANDED -- switching to PM (16-bit client)
   ... -> PM ok (CS=0x000f:0xd6be) -> DPMI PM loop
INT31h AX=0x000a BX=0x000f -> alias sel 0x0137       <- data alias of CS
```

And the bytes at the protected-mode landing address were
`0f 82 84 00 / 8c c8 / 24 07 / 3c 07 / 75 51 / 8c cb / b8 0a` —
`jc fail / mov ax,cs / and al,7 / cmp al,7 / jnz / mov bx,cs / mov ax,000A`. Exactly the
post-switch verification read out of the binary in Part 8. **The prediction and the
hardware agree instruction for instruction.**

### The silent death, and why it was silent

The first run then stopped dead: no more events, ever, and `tasklist` after 30 s showed
**no host process at all** — terminated, not spinning. That distinction was worth the one
extra round; "spinning" and "silently killed" need opposite next steps.

Cause, found by reading our own code rather than another experiment: the INT-site patcher
that converts `CD nn` into a BOP only recognises `31, 21, 10, 16, 33, 1A, 08`.
**`2F` is not in the list.** A PM guest cannot reach the IVT, so an unlisted INT stays a
raw `CD nn`, and executing it in protected mode goes to the kernel's #GP reflect — which
does not reflect, it silently terminates the VDM. That is the #18 signature exactly. No
DOS/4GW-class client ever issued INT 2Fh from protected mode, so the list never needed it;
krnl386's **next interrupt after the alias** is `INT 2Fh` at `seg1:0xd6e7`.

Added `0x2F` to the list plus a PM service arm mirroring the V86 one. ⚠ Every number
added widens the false-positive surface of what is a *naive byte-pair scan* — the same
shape that once rewrote a `jle` displacement in Doom and cost five sessions. The narrow
list is the mitigation: add a vector only for a guest that provably needs it, and only
with a service arm to receive it.

**Result: the host survives.** Log 61 → 182 lines, process alive at 30 s.

### ★ And then krnl386 says what is wrong, in English

```
INT2Fh(PM) AX=0x168a -> untouched
INT31h AX=0x0301 -> callRM 0x141:0xd73e
INT21h AH=4Ch -> client EXIT after 4 svc
```

That is not a crash — it is a **deliberate abort**. `0xd71b` builds an RMCS, calls back to
real mode via DPMI `0301`, and the routine at `0xd73e` is `mov dx,0xb9a9 / mov ah,9 /
int 21h / retf` — print a string. The string at `seg1:0xb9a9`:

> **`NTVDM KERNEL: Inadequate DPMI Server`**

first in a table that also holds `Unable to initialize heap`, `Unable to open KERNEL
executable`, `Unable to load KERNEL EXE header`, `Win16 Subsystem Initialization Failure`
— a roadmap of the next several failures, in the order we will meet them.

### ⚠ CORRECTION TO PART 6: the "MS-DOS" vendor API is REQUIRED, not optional

Part 6 said of `INT 2Fh 168A`: *"it tests for AL unchanged, meaning 'not supported', then
carries on"* and concluded that this work could start without it. **That is wrong.** The
test is `cmp al,0x8a / jz 0xd71b`, and I read the comparison without following the jump.
`0xd71b` is the abort path — the same one the failed-mode-switch and wrong-CS-RPL checks
branch to. Refusing `168A` *is* "Inadequate DPMI Server".

The mistake is instructive and cheap to repeat: reading a conditional and inferring the
outcome from the *sense of the test* rather than from **where it goes**. The fix is
mechanical — follow the target.

So the vendor API is the next blocker, and it is very likely the same object as
`INT 31h 04F3`: an entry point into NTVDM's private WOW services, which a `168A` query
for the vendor string `"MS-DOS"` is supposed to hand back.

---

## Part 10 — the vendor API, measured; krnl386 gets past its own error message

Part 9 ended with krnl386 printing `NTVDM KERNEL: Inadequate DPMI Server` because we
refuse `INT 2Fh 168A`. So: what does the real one say?

### Asking stock ntvdm — with a DPMI client written for the purpose

`tools/dostest/vendprobe.asm` probes **both modes**, because krnl386 only ever asks after
it has switched and the answer might differ. It does:

```
-- INT 2Fh 168A in REAL mode --        AL=8A  ES:DI=0000:0000
-- INT 2Fh 1687 DPMI check --          private paras: 0003
-- switching to 16-bit PM --
-- INT 2Fh 168A in PROTECTED mode --   AL=00  ES:DI=00C7:2037   LAR(ES)=FB00
```

**It is protected-mode only.** Dumping the 22 bytes at that entry:

```
cmp ax,0      / jnz +5 / mov ax,0x0100 / jmp +8
cmp ax,0x0100 / jnz +5 / mov ax,0x0137
clc / retf                                  <- a known function
stc / retf                                  <- anything else
```

A two-function dispatcher returning constants. Nothing more.

### ★ And krnl386 only needs it to EXIST

Having stored the entry it calls it exactly once:

```
mov ax,0x0100 / call far [0x1726]
jc  skip                  <- CF set: skip
verw ax / jnz skip        <- not a WRITABLE selector: skip
mov es,[0x598] / mov [es:0x32],ax
```

Both failure arms rejoin the normal path. So an honest *"that function is not provided"*
is explicitly tolerated by the guest's own code — the mandatory part is the `AL != 0x8A`
answer to `168A` itself.

### What we implemented, and one thing we deliberately did not

PM `168A` now matches `DS:SI` against `"MS-DOS"` and hands back our own far-callable stub.
Function 0 mirrors the oracle exactly — copying a measured answer, not inventing one.

⚠ **Function `0x0100` returns CF=1 on purpose.** Stock returns `0x0137`, and `verw` proves
that is a *writable selector onto something ntvdm owns*. We do not know what. A selector
onto an empty block of ours would pass `verw`, get stored at `[cs:0x32]`, and later be
read as if it were that something — the "runs but lies" class, which this project treats
as the most expensive kind of failure. Declining is truthful and costs nothing today, and
the rig confirms it: krnl386's own `cmp word [cs:0x32],0 / jz` at `d767` skips the block.

**On the rig:** `vendor=[MS-DOS] -> SUPPORTED`, the abort is gone, and krnl386 proceeds
into `INT 31h 0002` (`seg 0x110 -> sel 0x147`) — the paragraph-to-selector call added in
Part 5, doing exactly the job predicted for it.

### ★ A general instrument: what did the patcher leave behind?

The INT-site patcher now reports the **residual `CD nn` byte pairs** as a histogram by
vector. Every one is a vector we did not claim, and a PM guest executing one is silently
terminated — which is precisely how the INT 2Fh death happened, and finding *that* meant
reading the patcher's constant list by hand afterwards.

It is an upper bound: the region holds data, and `CD` precedes an opcode byte often
enough. But **a vector absent from the list cannot kill the guest**, which makes it a real
shortlist of suspects rather than a guess. It immediately named `41h ×6` — exactly the six
kernel-debugger sites `neints.py` found — and `11h ×1`.

Both are now claimed. `INT 11h` in PM answers `0x4021`, the **same** equipment word as the
V86 arm: krnl386 does `test al,2` for a coprocessor at `c136` and sets a kernel flag from
it, so the two modes disagreeing would mean the guest believes different hardware
depending on when it asked. `INT 41h` returns registers untouched — which *is* the "no
debugger present" answer — and exists to stop a silent death, not to provide a service.

Residual `0x22` → `0x1b`.

### Where it now stops

krnl386 dies **later, and not on an unclaimed vector** — it never reaches an INT 41h or
11h site. Last observed state is `0x0f:0xd715` (`mov si,ax / xor bx,bx / pop cx / ret`),
returning into protected-mode execution at `c0c5`, which then calls `d762`, `0x6763`,
`0x3021`. So it is a genuine fault somewhere in there, not a missing interrupt — a
different class of problem from every death so far this session.

---

## Part 11 — the fault bracketed in one round, and a second correction I owe

### Reference projects, first

`leecher1337/ntvdmx64` came up. Verdict recorded in [`docs/reference-projects.md`](../../reference-projects.md)
(the file `risks.md` R8 has always pointed at and which did not exist): **do not read
`ntvdmpatch/`** — patches against the leaked NT4 source, no LICENSE, and a patch quotes
its context, so reading one is reading leaked Microsoft source *for the exact subsystem we
are implementing*. Worse than a licence problem, because provenance taint cannot be purged
from history the way `DOOM1.WAD` was. Only the readme prose and directory listing were read.

★ **And a useful negative result:** neither open unknown is publicly documented anywhere.
`INT 31h 04F3` is absent from the DPMI 0.9 spec and RBIL; SysVars`+0x6A` is past the
documented end of the list of lists; and Microsoft's own KB Q220155 *"Troubleshooting
NTVDM and WOW Startup Errors"* documents **no message text at all**. The rig-as-oracle
method is not a fallback here — it is the only source, and it has already answered three
questions no external project could have.

### The fault: three candidates to one, in a single rig round

`PMBP_PATH` breakpoints at the return site of each call made from `c0c5`:

```
0D4D9  c0c9  about to call d762      <- HIT
0D4DC  c0cc  d762 RETURNED           <- never fired
0D4E1  c0d1  0x6763 RETURNED         <- never fired
0D4E6  c0d6  0x3021 RETURNED         <- never fired
```

One hit, and it is the *entry*. **`d762` never returns.** That is what the breakpoint
facility was built for and it cost one 30-second round.

### ★ And the cause is the vendor function I declined

Inside `d762`, `[cs:0x32]` is zero — we returned CF=1 for vendor function `0x0100`, so
krnl386 never stored a selector there — and it takes the `jz d7c4` branch. That branch
calls `0x5888` four times, and `0x5888` opens:

```
5890  mov si,[0x5ac]
5894  mov ds,word [cs:0x32]     <- DS = 0
5899  mov dx,cx
589b  mov ax,[si]               <- dereference -> #GP -> VDM silently terminated
```

Loading a null DS is legal. Dereferencing it four bytes later is not.

⚠️ **CORRECTION TO PART 10.** I wrote that both failure arms at the call site *"rejoin the
normal path, so declining a function is explicitly tolerated by the guest's own code"*.
That is true **locally** and false **globally**: krnl386 guards `[cs:0x32]` at `d767` and
does **not** guard it at `5894`. I read the guard and generalised from it.

This is the *same mistake shape* as the `168A` one two parts ago — reading a check and
inferring the program's behaviour from it, rather than following through to everywhere the
value is used. Twice in one session, on the same slot, from the same habit. The rule that
would have caught both: **a guard proves the author considered one path, not that every
path is guarded.** Grep for every reference to the address before concluding it is optional.

### ★ What `[cs:0x32]` actually is — and why this is an architectural finding

The block we skip initialises it:

```
d76f  mov es,word [cs:0x32]
d777  mov ax,0 / call <INT 31h wrapper>   ; DPMI 0000: allocate 1 descriptor
d77d  and al,0xf8                         ; selector -> descriptor-table BYTE OFFSET
d77f  mov [0x5ac],ax
d784  mov word [es:si],0xffff             ; write AT that offset, through [cs:0x32]
d789  mov word [es:si+5],0x0f00           ; ...at descriptor byte 5, the access byte
```

krnl386 takes a selector from DPMI `0000`, converts it to a **table offset**, and writes
descriptor bytes through `[cs:0x32]`. And `0x5888` then pops a free-list entry and writes
`[di+5] = 0x0F` — access byte, P=0, i.e. *mark this descriptor not-present*.

⇒ **The "MS-DOS" vendor API's function `0x0100` returns a writable selector onto the LDT
itself.** That is why stock's `0x0137` must pass `verw`, and why the API is mandatory:
NTVDM's WOW hands krnl386 **direct write access to the descriptor table** so it can manage
selectors without a DPMI call per operation.

That is a real architectural constraint, not a missing stub, and it is the largest open
design question this epic has produced so far.

---

## Part 12 — the vendor window IS the descriptor table; ours is not reachable

Measured what stock's `168A` actually hands back (`tools/dostest/vendprobe.asm`, extended
to call the entry):

```
returned AX=0137  verw:WRITABLE  limit=5FFF  base=001140B0
our CS=019F       CS base(0006)=00006DD0
descriptor at window[CS & 0xFFF8]: FF FF D0 6D 00 FA 00 00
```

which decodes to base `0x00006DD0`, access `0xFA` (code, readable). Two independent routes
agree on the base **and** the access byte is the right type for CS. It is the real
descriptor table, 3072 entries, at a **user-mode** address.

Then: is ours? Added a self-search — install descriptors with unique contents, hunt for
those exact eight bytes in our own address space. **Not found.** So the "hand back a real
LDT selector" option is not available to us the way it is to stock.

⚠️ **THE INSTRUMENT WAS WRONG BOTH WAYS BEFORE IT WAS TRUSTWORTHY, AND THAT IS THE PART
WORTH KEEPING.**

1. **False negative.** The second probe base was `0xA5A52000` — 2.77 GB, above the ~2 GB
   validator cap *documented in the same file a few hundred lines up*. The descriptor was
   refused, so the search could never find what was never installed. Fixed, and `LAR`
   verification added so a refused descriptor and an unmappable table can no longer
   produce the same log line.
2. **False positive.** It then reported a table at `0x02219000`. The two probes were
   **consecutive** indices — and two consecutive descriptors sit 8 bytes apart both in a
   real table *and* in the buffer `NtSetLdtEntries` marshals its two entries through. It
   was matching the buffer.
   ★ **Caught because the reported base MOVED between runs while the winning address did
   not** — impossible for a table where index N lives at base + N×8. *A number that
   changes when it shouldn't is the cheapest lie-detector this project has.* Fixed with a
   five-slot gap: 40 bytes apart in a table, still 8 in a buffer.

## Part 13 — the descriptor-table SHADOW, which validates itself

Since we cannot hand over the real table: a page-aligned **shadow**, seeded from
`g_ldt[]`, returned by vendor function `0x0100`, reconciled into the real LDT via
`NtSetLdtEntries` on entry to any protected-mode interrupt service. Plus `INT 31h 000D`
(allocate *specific* descriptor) — what a client asks once it manages the table itself.

★ **The open question was whether a plain shadow suffices or writes must be TRAPPED** (the
A0000 planar page-protection pattern). Rather than argue it, the reconcile logs every
changed entry — so the shadow is also the instrument that answers it:

```
LDTSYNC idx 0x08 <- guest wrote base=0x00000400 limit=0x0 acc=0xf3  ok
```

krnl386 **does** write descriptors directly (base `0x400` = the BIOS data area), and
reconcile-on-interrupt is sufficient for the sequence it actually runs: allocate via
`000D`, write, then call `04F2` — both calls are sync points. A trapping shadow is not
needed **unless the log ever shows a descriptor used before we saw it written**.

⚠️ **A real bug, caught by the log being loud on the first run.** The sync reads "shadow
differs from `g_ldt[]`" as "the guest wrote this" — so descriptors *we* allocate after the
shadow exists look guest-zeroed, and it pushed those zeros into the real LDT, destroying
live entries. Three lines said "guest wrote base=0 limit=0 acc=0" for indices the guest
had never touched. Fixed by calling `wow_shadow_put()` from `dpmi_install()`:
**both directions, or the difference test means nothing.**

## Part 14 — the private `04Fx` family, decoded from its call sites

- **`04F2` = "I modified CX descriptors from selector BX — commit them".** All eight call
  sites are identical in shape:
  ```
  6089  mov byte [bx+0x5],0xf3   ; the ACCESS byte, written through the window
  608d  or bl,0x7                ; -> a selector
  6092  mov cx,0x1               ; how many
  6095  mov ax,0x4f2 / int 31h   ; commit
  ```
  ⇒ **stock ntvdm needs a flush too**, so our shadow is not a workaround for lacking the
  real LDT — it is the *same design*, and `04F2` is where the guest asks for the sync.
- **`04F1` = the private twin of `0000`.** Reached through a dispatcher at `seg1:0x6638`
  keyed on the DPMI function number in AL, where `AL=0x0B` (get descriptor) is served
  **locally** by two `movsd` out of the window. The family is a fast path: reads from the
  window, only what must reach the host becomes a call.

Measured with the same PMBP breakpoints that bracketed the fault — before, only `c0c9`
hit; after, `c0cc` (`d762` **returned**) and `c0d1` (`0x6763` returned) hit too.

## Part 15 — ★★ krnl386 calls a 32-bit companion via BOPs, and the surface is 82 functions

It then halted three instructions into `0x3021` with `unexpected PM stop`, on bytes
`c4 c4 53`.

**`C4 C4` is a BOP — and this one is krnl386's OWN.** Checked, not assumed: the bytes are
in the **file**, there is no relocation record in range, and guest memory matches the file
exactly. (Our INT-site patcher writes those same two bytes, so "it's our patch" was the
easy and completely wrong answer.) seg1 holds 13 native sites: `0x51`×1, `0x53`×1,
`0x56`×10, `0xFE`×1.

⚠️ **Lengths differ** — `0x53` carries a sub-function byte (4 bytes), the rest are 3. Read
it off what follows: after `0x56` comes `83 C4 nn` (`add sp,nn`, a cdecl cleanup, which
also tells you `0x56` takes stack arguments). Wrong length resumes the guest
mid-instruction.

★ **`0x53/03` = "give me the 32-bit dispatch entry", and NULL is the CORRECT answer** —
every `0x56` site is guarded by `call dword far [0x6ac]` *if set*, else BOP, so NULL
selects the per-call path krnl386 already implements. Confirmed: `jz 0x3074` taken and
`0x3021` **returns** to `c0d6`.

★ **`0x51` is the generic 16→32 GATEWAY, not a service.** Its site is a thunk prologue
(`seg1:0x2bb6`) that publishes the frame as SS:BP and BOPs. Every call arrives from a
per-function stub of one fixed shape:

```
push word <arg byte count>
push word 0
push word <FUNCTION ID>
nop / push cs / call 0x2bb6
```

matching the live frame exactly (`bp+6` ID, `bp+10` arg bytes, `bp+12..` args). **Static
and dynamic agree, which is what makes either trustworthy.**

⇒ `tools/ne/wowthunks.py` reads that out of any 16-bit module.
[`docs/research/wow32-call-surface.md`](../../research/wow32-call-surface.md) is the
result: **82 distinct function IDs with argument sizes — the whole API, enumerated before
implementing any of it.** Six further stubs call a different thunk (`0xaae8`), listed
separately rather than folded in.

⚠️ **Only krnl386 has these stubs.** user.exe, gdi.exe, system.drv, keyboard.drv and
wowexec have **none** — they funnel through KERNEL via the `push api-index / call far
KERNEL / push argcount` tables behind gdi's 366 additive `__MOD_GDI` relocations (part 4,
before we knew what they were for). **So the 16↔32 boundary lives in exactly one module.**

Unimplemented BOPs now **step over and log** rather than halting the guest, which turns
the wall into a trace — flagged in the log as the deliberate lie it is, because any
finding downstream of a stepped-over BOP is suspect. Serviced calls went **4 → 38**, and
krnl386 advanced to **message #2 of its own error table**:
`NTVDM KERNEL: Unable to initialize heap`. On that path it asks for exactly three:

```
FUNC=0x78  args=4   (c123 000f)
FUNC=0xcf  args=0
FUNC=0xb8  args=16  (65ed 000f  0040 0000  3000 0000  8080 0008)
```

`0x3000` and `0x40` together are `MEM_COMMIT|MEM_RESERVE` and `PAGE_EXECUTE_READWRITE`,
`0x88080` is ≈544 KB, and the call sits immediately before the heap failure — so `0xb8` is
very likely the allocator. Recorded as the strong inference it is: the argument **order**
is not pinned down and two readings are possible.

## Part 16 — Win16 windows get Luna, measured

Asked rather than assumed: ran `sysedit.exe` under **stock ntvdm** and captured the
desktop ([evidence](../../research/evidence/win16-luna-stock.jpg),
[title bar](../../research/evidence/win16-luna-titlebar.png)). Full Luna chrome — gradient
caption, Tahoma-bold title, Luna frame and caption buttons, cream Luna menu bar (not
classic `#C0C0C0`), a normal taskbar button, and themed MDI child captions.

**Why:** XP's `user.exe` is a 47 KB **thunk stub with no drawing code** — which is exactly
why it has no BOP stubs of its own and funnels through KERNEL. Every window it creates is
a genuine HWND; `win32k` draws the non-client area and does not care that the caller is
16-bit. Theming is applied below the point where bitness is visible.

⇒ **If WOW32 is implemented by forwarding to the real `user32`/`gdi32`, Luna is free**,
along with taskbar buttons, Alt-Tab and snapping. Reimplementing a window manager inside
the VDM would mean drawing Luna ourselves *and* losing desktop integration — a fidelity
regression, not a feature. Client-area *controls* stay classic 3-D (no comctl32 v6
activation context); that is stock behaviour and the right target, since forcing v6 would
change control metrics and break Win16 apps that hard-code sizes.

---

## ★ THE NORTH STAR FOR #128

> **Run MS Paint and Notepad from Windows 3.x under NTVDMEX.**

Chosen deliberately, and it is a good bar for the same reasons Doom was for the DOS side:
they are small, iconic, and they exercise the whole stack honestly — NE loading, the
KERNEL 16→32 boundary, USER windows and menus, GDI drawing, mouse and keyboard input.
`PBRUSH.EXE` in particular cannot be faked: it has to paint.

**Where that bar sits today: not started.** No Win16 program has run. `wowexec.exe` — the
program that would launch an app — has never been executed, and nothing has drawn a pixel.
What exists is the bootstrap: the module set loads, binds and gets selectors, and krnl386
itself runs far enough to name what it wants next.

**Distance to it, honestly.** 82 WOW32 functions, none documented anywhere, each needing
the same reverse-engineering treatment that `168A`, `04F2` and `0x53` got — read the call
sites, infer, implement, confirm the guest gets further. Then user/gdi's separate
KERNEL-funnelled path, then wowexec, then the app. krnl386's error table names five
milestones and one is cleared.

## Next actions

1. **Pin the argument order for `0xb8`**, then implement it with `0x78` and `0xcf` to
   clear "Unable to initialize heap" — error #2 of five.
2. **Then `0x56`** (10 sites; the `add sp,nn` after each gives its argument byte count).
3. Work down [`wow32-call-surface.md`](../../research/wow32-call-surface.md) as krnl386
   demands each ID; the error table orders the milestones.
4. Still open, neither yet fatal: **`INT 31h 04F3`** and the **SysVars+0x6A** table.
5. Independent of WOW: **#131 console/stdio**.

**Rig left with:** IFEO `Debugger` **set** (restored and verified after the stock
screenshot), `wowtry.flag` **present**, `pmbp.txt` **present** (delete to disarm), and the
WOW module set staged in `guest/ne/` on the build machine. DOS regression verified this
session: `selftest.com` **8/8 PASS**.
⚠️ `C:\ntvdmex\ldtprobe.log` is an **untruncated** sink holding many runs — read the LAST
run's lines.
