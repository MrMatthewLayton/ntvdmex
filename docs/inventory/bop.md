# Inventory — the BOP surface (`C4 C4 nn`)

**Surface:** the instruction a 16-bit guest uses to call *out* of the VDM into the
32-bit host. `C4 C4` is an invalid `LES` encoding on a real 8086; NTVDM claims the
whole two-byte prefix and reads the third byte as a call number.

**Spec:** ⛔ **none public.** This is one of the genuinely unspecified surfaces named in
[README.md](README.md) — like the `NtVdmControl` contract and the WOW32 thunk tables.
**The oracle is the spec**, and the oracles here are (a) the Microsoft-shipped 16-bit
binaries that issue BOPs, read directly, and (b) stock ntvdm's behaviour when they do.

⚠ Every row below whose "meaning" column is blank is blank **on purpose**. Nothing in
this file may be filled in from memory; see the PIT mode-3 incident in
[parity-by-inventory](../../README.md) — a remembered fact and a one-oracle
measurement are both single opinions.

---

## 0. ▶ WHY THIS SURFACE IS IN SCOPE — decided 2026-09-25

> **User:** *"We need to support command.com that ships with Windows XP."*
> … *"NTVDMEX's hardware/firmware emulation layer should remain fairly rigid … BUT, for
> MS-DOS, this is something that ultimately I want to be more pluggable."*

**The architecture this settles:**

| layer | pluggable? | rule |
|---|---|---|
| hardware / firmware | **no** | implement *the standard* where one exists (VGA BIOS + VBE/VESA — not S3 Trio, not TNT2); implement the *de-facto devices* where none does (SB16, SB Pro, AWE, OPL2/3, GUS), because being a Sound Blaster **is** the standard |
| DOS | **yes** | our kernel by default, with XP's `System32\command.com` as the shell; `cfg\shell.txt` to bring your own; booting real MS-DOS system files is a future milestone |

⇒ **XP's `COMMAND.COM` is the DEFAULT SHIPPED SHELL, so `BOP 0x54` is the contract
between our DOS and the shell we ship with — not an application asking for a favour.**
It belongs here, sized and marked like any other surface.

⚠ **And it is a second contract, deliberately entered.** Our DOS now answers to the
documented DOS API *for applications* and to NTVDM's private BOP interface *for NT's
shell*. In that narrow respect our kernel is a drop-in for `ntdos.sys`.

### ⛔ The option that was measured and rejected: "be stock"

Loading XP's `ntio.sys` + `ntdos.sys` + `command.com` — i.e. becoming stock ntvdm — was
the user's lean until it was costed:

| | BOP codes | sub-functions |
|---|---|---|
| our kernel + XP's shell | **2** | ~12 of `0x54`, 1 of `0x50` |
| `ntio` + `ntdos` + shell | **38 of the 61 real** | **60 distinct** of `BOP 0x50` in `ntdos.sys` alone |

Two things decided it, neither of them the one expected:

1. ⚠ **The hardware risk is NOT there.** `ntio`/`ntdos` are thin shims — 177 `C4 C4`
   sites and almost no port I/O. They would not stress the PIT, the RTC or the FDC. That
   risk belongs to *booting a real MS-DOS*, whose `IO.SYS` genuinely drives them.
2. ★ **Those 60 sub-functions are the ones already built from spec** — `dem*` is open,
   read, write, find, exec. Being stock means retiring 103 spec-built INT 21h functions
   for 60 reverse-engineered ones, **and capping the project at parity with `ntvdm` for
   ever**: if the DOS *is* `ntdos.sys`, a superset is impossible by construction.

### Status

A no-guest launch loads `C:\WINDOWS\SYSTEM32\COMMAND.COM` (`cfg\shell.txt` overrides).
⚠ In practice nothing reaches that path yet — NT only grants VDM privileges to a process
it started as the VDM, so the host cannot be run with no guest at all; CSRSS always names
a program.

### The collision is resolved by ORIGIN, not by renumbering

Every BOP **we** plant, we plant at an address we own — `DOS_HDLR_SEG` for the INT
stubs, DPMI entry/return catchers and callback slots, `DOS_CTAB_SEG` for the BIOS stubs.
A BOP executing anywhere else is the guest calling NTVDM. `g_bop_from_guest` is set from
`CS` before any arm runs, and the DPMI arm is gated on it.

⚠ **Renumbering ours would have been worse**: the numbers we'd move to are equally
NTVDM's, so it relocates the collision instead of removing it. The origin test is exact.

### ✅ The encoding, confirmed across THREE binaries

`C4 C4 <bop> <sub>` — **four bytes**, for `0x50` and `0x54`. First inferred from
COMMAND.COM alone (at `0x283E`, `jnc` decodes at +4 and is junk at +3), then corroborated
by scanning XP's own 16-bit VDM components, pulled off the rig:

| binary | size | `BOP 0x50` | `BOP 0x54` | other |
|---|---|---|---|---|
| `ntdos.sys` | 27,866 | **73 sites**, subs `00–4A` (74 distinct) | 4 sites, subs `04 05 07` | `0x5A` ×1 |
| `ntio.sys` | 33,840 | 7 sites, subs `0D 11 3B 3D 3E 45` | 2 sites, subs `09 0C` | `0xFE` ×17, and ~20 more |
| `COMMAND.COM` | 50,620 | 1 site, sub `3D` | **15 sites**, subs `00 01 02 06 08 09 0A 0B 0D 0E 0F 10` | — |
| `ntvdm.exe` | 420,864 | — | — | **none** (it is the 32-bit side) |

⇒ **`0x50` is the DOS service BOP** — a dense, contiguously numbered table, which is what
a DOS kernel's call-out interface looks like. **`0x54` is a second, smaller table**
(`00–10`) shared by the DOS kernel, the I/O layer *and* the shell. Ours is not a special
case: COMMAND.COM uses the same interface NTVDM's own DOS does.

⚠ NTVDMEX supplies its **own** DOS, so `ntdos.sys`/`ntio.sys` never load here —
COMMAND.COM's `0x54` calls arrive at us directly. They are an oracle, not a dependency.

### What answering it does — measured, both ways

`cfg\bop54.txt` = `cf0` (default) / `cf1`. The sub-01 site branches on carry immediately.

| answer | what COMMAND.COM does |
|---|---|
| `CF=1` | **polls the call forever** — one log line per spin, 268,435,180 bytes before it was rate-limited |
| `CF=0` | proceeds to a **second, different** call: `sub 0x0E` at `0x5E6`, then spins in V86 with no traps |

Neither is known to be right; `CF=1` is known to be a dead end. ⛔ **The instrument is now
rate-limited** — 16 in full, then only when the register signature changes. *An
instrument that scales with a guest's spin rate is a denial-of-service on the thing you
are trying to read.*

### ✅✅ THE DISPATCH TABLE ITSELF, OUT OF XP's `ntvdm.exe`

`ntvdm.exe` (420,864 bytes, XP SP3, image base `0x0F000000`) carries a **256-entry BOP
dispatch table at RVA `0x064980`**. 195 of the entries share one stub (`0x0F05E04F`) —
that is "unimplemented", and having it named makes the rest exact: **61 BOPs are real.**

```
00 02 06 09 0E 10 11 12 13 14 15 16 17 18 19 1A 1D 21 40 42
50 51 52 53 54 55 56 57 58 59 5A 5B 5C 5D 5E 5F 60 66 67 68
70 71 72 73 74 75 76 77  B8 B9 BA BB BC BD BE BF  C8 C9  FD FE FF
```

⛔⛔ **Our DPMI numbering collides with far more than the two we knew about.** `0x50`,
`0x54`, `0x55`, `0x56`, `0x57`, `0x58`, `0x59`, `0x5A` are *all* real NTVDM BOPs, and we
use every one of them. The origin test covers this — but it is the reason the origin test
had to be the fix rather than renumbering.

### ✅ `BOP 0x54` — the handler, and its sub-function table

`0x0F0085AE`, disassembled:

```
push 0 ; call getIP        ; fetch the byte AFTER the BOP ...
... ; movzx esi,byte [eax]  ; ESI = the SUB-FUNCTION
call getIP ; inc eax ; call setIP    ; ... and STEP OVER IT
push esi ; call 0x0F008606
                 └─> mov eax,[ebp+8] ; call dword [eax*4 + 0x0F065040]
```

★ **The 4-byte encoding is now read off NTVDM's own dispatcher**, not inferred: it takes
`IP`, adds **1**, and stores it back — `C4 C4 54` is three bytes and the sub-function is
the fourth. That retires the inference from `jnc` decoding in §3.

**The sub-function table at VA `0x0F065040` has exactly 17 live entries** — `0x00`–`0x10`,
after which every slot repeats one address. That is precisely the range used across
COMMAND.COM (`00 01 02 06 08 09 0A 0B 0D 0E 0F 10`), `ntdos.sys` (`04 05 07`) and
`ntio.sys` (`09 0C`). Three binaries and the table agree.

| sub | handler VA | notes |
|---|---|---|
| `00` | `0F04EDE7` | **= exported `VDDTerminateVDM`** |
| `01` | `0F00A966` | **see below — the one that blocks us** |
| `02` | `0F015D63` | |
| `03` | `0F05E04F` | **= the unimplemented stub. XP does not implement it either.** |
| `04` | `0F008621` | used by `ntdos.sys` |
| `05` | `0F00B848` | used by `ntdos.sys` |
| `06` | `0F04DB4A` | `cmd`-module range (cf. `cmdCheckTemp` at `0F04CB2F`) |
| `07` | `0F0198AE` | used by `ntdos.sys` |
| `08` | `0F04EBBB` | `cmd`-module range |
| `09` | `0F00D187` | used by `ntio.sys` |
| `0A` | `0F04EB43` | `cmd`-module range |
| `0B` | `0F04ECF2` | `cmd`-module range |
| `0C` | `0F00C342` | used by `ntio.sys` |
| `0D` | `0F012C94` | |
| `0E` | `0F0156EC` | **the second one COMMAND.COM reaches** |
| `0F` | `0F00BEF6` | |
| `10` | `0F04C829` | `cmd`-module range |

### ▶ `sub 0x01` = get the next command

`0x0F00A966` allocates a `0x338` stack frame and fills a structure at `0x0F09BE48…BEDC`
with: a `0x104`-byte (MAX_PATH) buffer at `[ebp-0x248]`, a second at `[ebp-0x28C]`, and a
word `0x0105`. That is a **`VDM_COMMAND_INFO`**, and `GetNextVDMCommand` is in the
import strings. ⇒ **sub 01 asks CSRSS what to run next** — exactly what a shell does at
startup, and exactly the call our own STAGE1 already makes (`STAGE1: command fetch …`).

It reads its request block from `DS:DX` — `(getDS()<<4) + getDX()`, then fields at
`+0x0E`, `+0x1C`, `+0x1E`, `+0x20`. COMMAND.COM sets `DX=0x95D7` before the call, which
is that block.

#### The request block at `DS:DX`, read off BOTH sides

Field accesses in `0x0F00A966` (`ebx = (DS<<4)+DX`), cross-checked against what
COMMAND.COM stores and reloads around its call site at guest `0x3CE`:

| off | size | dir | evidence |
|---|---|---|---|
| `+0x00` | word | in | `movzx eax,word [ebx]` |
| `+0x02` | word | in/out | read, then `mov [ebx+2],ax` |
| `+0x04` | byte in, word out | in/out | `mov al,[ebx+4]` / `mov [ebx+4],ax` |
| `+0x06` | word | out | |
| `+0x08`, `+0x0A` | word | in | |
| `+0x0E` | word | in | copied into the `VDM_COMMAND_INFO` |
| `+0x10` | word | **out** | flags. COMMAND.COM: `test [blk+0x10],7` then `,1` |
| `+0x12` | dword | **in+out** | a **cookie**: COMMAND.COM loads it from its own `[es:0x32B]/[es:0x32D]` *before* the call and stores it straight back *after* — it must round-trip |
| `+0x16` | word | out | |
| `+0x18` | word | in | compared against 0 |
| `+0x1A` | word | out | COMMAND.COM keeps the low byte at `[es:0x32F]` |
| `+0x1C`:`+0x1E` | seg:off | in | combined as `(seg<<4)+off` |
| `+0x20` | word | in/out | |
| `+0x22` | word | out | status — XP writes `4`, `8` or `9` |

**Implemented as a defined "no command"**: zeros into the OUT fields, `AX=0`, `CF=0`,
and `+0x12` **deliberately preserved**. ⚠ That the zeros *mean* "nothing to run" is a
claim, not a decode — but the alternative is not neutral: leaving the block alone hands
COMMAND.COM whatever was already in its memory. **A defined answer is falsifiable; an
uninitialised one is not.** Measured: it now proceeds past sub 01 every time.

### ✅ `sub 0x0E` = the keyboard / code-page configuration

`0x0F0156EC`, and the imports name it outright:

```
call [GetSystemDefaultLangID] ; cmp si,0x411        <- LANG_JAPANESE
  ├─ yes: call [GetKeyboardType] (USER32) ; lengths 0x1B5 / 0x3A4 ; writes "JP"
  └─ no : lea eax,[ebp-0x18] ; call [GetConsoleKeyboardLayoutNameA]
```

`0x1B5` = **437**, the US OEM code page. `0x3A4` = **932**, Japanese Shift-JIS. And the
call site fits: immediately before it COMMAND.COM issues `INT 2Fh AX=AD80h`, the KEYB
installed-check, then passes `DS:SI` = a buffer with `CX = 0x539` (1337 bytes).

⇒ **sub 0E asks the host for the console's keyboard layout and code page.**

### ⛔ `+0x04` IS THE DEFAULT DRIVE, and zeroing it sent the shell to A:

Found by running it. COMMAND.COM, two instructions after `sub 01` returns:

```
guest 0x6B8   mov dx,[0x95DB]      ; = blk+0x04
guest 0x6BC   mov [0x9612],dl
guest 0x6C0   mov ah,0x0E ; int 21h ; SELECT DEFAULT DRIVE
```

A zero there is drive 0 = **A:**, and our own handler said so in the same log —
*"drive A: exists but is not ready … selected as the DOS current drive anyway"* — while
the shell went quiet. Now filled from `dos_int21_cur_drive()`, and the trace reads
`21:0e/80 … dx=0002` = **C:**.

★ **This is what "a defined answer is falsifiable" buys.** The wrong value showed up in
one run and named its own field. An uninitialised block would have produced a different
wrong drive each run and read as flakiness.

⚠ And `dos_cur_drive()` was `static`: it is now published as `dos_int21_cur_drive()`
rather than re-derived in the host, because a second copy of the rule would have
silently dropped the `vdrive` case.

### ▶ Where it stands

With both sub-functions answered, XP's COMMAND.COM makes **32 INT 21h calls** (was 9 at
the version gate), gets through code-page setup — `AH=66h/01`, `INT 2Fh AX=1400h`
(NLSFUNC check) — and selects **C:** as its default drive. It then copies a counted
string from `0x9327` to `0x93AA`, calls guest `0x5664`, and idles without further DOS
calls: `frozen at CS:IP=0x0050:0x0037`, our own INT 08h stub's `CD 1C` chain, with
`irq0_inj=4` and `raised_any=0x25B`. **It is taking interrupts, not wedged.**

⚠ `AH=66h` looked like the gap — it returned `AX` unchanged — and **it is fully
implemented** (`dos_int21.c:1204`, `BX=DX=437`). AX simply is not one of its outputs.
That is the **fourth** time the "AX unchanged ⇒ unserviced" heuristic would have
invented a gap; the file's own `dos622_defines` comment warns about exactly this.

### ✅ Guest `0x5664` was the command-line PARSER, and it had nothing to parse

```
guest 0x31F4  mov si,0x93AC
       loop:  lodsb / cmp al,0x22 (quote) / cmp al,0x0D / jnz loop
```

**With no CR in the buffer, SI walks the whole 64K segment and never leaves** — exactly
the "spinning in V86 with no traps" the headless deadline kept killing.

Dumping the request block *before* writing it named the buffer:

```
blk in = 26 02 | 00 01 | 02 00 | 00 00 | 42 93 | 27 93 | 80 00 ...
           +00     +02     +04     +06     +08     +0A     +0C
```

⇒ **`+0x08:+0x0A` = `0x9342:0x9327`**, and `0x9327` is exactly the address COMMAND.COM's
own copy loop reads (`cl=[0x9327]; cx+=3; movsb` to `0x93AA`, parse from `0x93AC`). Two
independent sources, the same address. Layout: `[0]` length, `[1]` unused, `[2..]` text,
then **CR**. We now write the empty tail `00 00 0D` there.

★ And the dump settled the design as well as the address: the guest already supplies
`+0x04 = 02` (C:). **It was never ours to fill in.** So sub 01 now writes only the flags
word and the command tail, and leaves every field it cannot name alone — *a named wrong
value is worse than an unchanged right one.*

### ⛔ Where it is now: a LOOP, not a hang

With a CR to find, COMMAND.COM stops scanning and runs its init — and then runs it
again, forever:

```
21:19/0d  21:5d/09  21:5d/08  21:53/02  21:19/00  21:66/01  21:0e/80   ×N
```

It asks `sub 01` again each time, is told "no command", and restarts. That is progress —
it is executing rather than scanning memory — but `flags = 0` evidently does not mean
"go interactive". COMMAND.COM branches on `test [blk+0x10],7` then `test …,1`; **the
flag bits are the next thing to earn.**

### ⛔⛔⛔ FOUR INSTRUMENTS IN ONE SESSION NEEDED A CAP

The loop turned every per-call log line into a file. In order:

| instrument | what it wrote | fix |
|---|---|---|
| NTVDM BOP logger | 268,435,180 bytes | 16 in full, then on register change |
| …the same, again | 268,435,219 bytes | **a novelty filter is not a cap** — a loop differs every pass. Hard ceiling of 64 |
| INT 21h trace (`dostrace.flag`) | 2,166,824 lines | `DOS_TRACE_MAX` 4000, shared with the `->` result half |
| `BOP2F` INT 2Fh logger | 211 MB on its own | 512 lines |
| `AH=53h` unimplemented note | 2,713 lines per 200 KB | said once per run |

Result: **268 MB → 442 KB.**

⚠ `LOG_MAX_BYTES` (256 MB) already stopped the *disk* filling. It did not make a log
readable, and a 268 MB file over SMB is its own outage. **A cap on the sink is not a cap
on the instrument.**

⛔ And the first BOP rate-limit had a worse bug than its size: it `continue`d out of the
arm, **skipping the sub 01 and sub 0E handlers** — so past the 17th call the guest was
told something different from what the first sixteen were told, silently. *A quiet
instrument that also changes behaviour is not an instrument.* It now gates the log only.

### ✅✅✅ XP's OWN COMMAND.COM EXECUTES COMMANDS (2026-09-25)

`cfg\bopcmd.txt` hands the shell one command line through `sub 01`, once, and empties
every reply after (a guest that asks again must not be given the same command again —
that is the difference between testing a mechanism and building a fork bomb):

```
cfg\bopcmd.txt = "ver"        cfg\bopcmd.txt = "dir"
  MS-DOS Version 5.00.500        Volume in drive C has no label
                                 Volume Serial Number is 0C09-23F0
                                 Directory of C:\...\debug\tests\cmdcom
                                 .            <DIR>     09-24-26   5:18p
                                 ..           <DIR>     09-24-26   5:18p
                                         2 file(s)          0 bytes
                                                   268431360 bytes free
```

**The whole path works end to end** — `sub 01` → command line in the guest's buffer →
COMMAND.COM's parser → execution → console output. Two different commands, one a builtin
and one that walks the filesystem.

### ✅ `+0x10` decoded: three bits, one per redirected handle

`0x0F00A966` does not write `+0x10` directly; it passes `&blk[0x10]` to `0x0F04D568`,
which builds it from the `VDM_COMMAND_INFO`:

```
flags = 0
if [info+0x10] != 0: flags  = 1        ; StdIn
if [info+0x14] != 0: flags |= 2        ; StdOut
if [info+0x18] != 0: flags |= 4        ; StdError
if flags == 0: early out
```

⇒ COMMAND.COM's `test [blk+0x10],7` is *"was anything redirected?"* and `test …,1` is
*"was stdin?"*. **Zero is correct for an interactive shell**, so our answer was right
here — worth stating, because it was a guess until this read.

Also pinned from the same tail: `+0x16` is the **code page** (COMMAND.COM compares it
against `AH=66h/01`'s `BX` and issues `AH=66h/02` if they differ, guarded by the
`INT 2Fh AX=1400h` NLSFUNC check), `+0x04` ← `[0x0F09BEDA]`, `+0x06` ← `[0x0F077338]`,
`+0x1A` ← the byte at `[0x0F09BEDC]`.

### ⛔ What is still missing: "you are interactive"

With an empty answer COMMAND.COM asks again immediately — **1,255,260 `sub 01` calls in a
30-second run**. That is its *command loop*: get a command, run it, repeat. It never
prints a prompt or reads the keyboard, so nothing yet tells it that no more commands are
coming and it should go interactive.

#### ⛔ `+0x22` is NOT a status word — it is a file-extension dispatch

I wrote that three values for one call *"is a status enumeration, and one of them is very
likely 'no more commands'"*. It is not. `0x0F00AEA8..AF1E`:

```
ax = [0x0F09BED2]                       ; length of the program name
if ax <= 6            -> +0x22 = 9      ; too short to carry an extension
edi = nameptr + ax-5                    ; the last four characters
strncmp(edi, ".EXE") == 0 -> +0x22 = 4
strncmp(edi, ".COM") == 0 -> +0x22 = 8
strncmp(edi, ".BAT") == 0 -> +0x22 = 2   else 9
```

The three constants are at VA `0x0F0037B0`/`B8`/`C0` and read **`.BAT`**, **`.COM`**,
**`.EXE`**. ⇒ **`+0x22` is the TYPE of the program to run.**

So `sub 01` returns *three* things, not one: a command **tail** (`+0x08:+0x0A`), a program
**name** (`+0x1C:+0x1E`, capacity `+0x20`), and that program's **type** (`+0x22`). The
guest agrees — at guest `0x3167` it loads `+0x22` and, on the no-program path, writes a
zero to the first byte of the name buffer at `0x9473`, which is exactly the
`+0x1C:+0x1E` we are handed.

All three are now answered: the tail as before, the name from `progpath` on the first
call only (⚠ **not** `g_app2` — on the rig CSRSS names `dosstub.com`, the harness stub),
the type by XP's own extension rule, and empty/`9` on every call after.

### ⛔ The loop is NOT an error path — it is the shell's main loop

Every field is now well-formed and every one matches XP's own rule, and COMMAND.COM
**still** asks ~1,250,000 times per 30-second run. Three answers tried — nothing,
its own path with type 8, and `dosstub.com` with type 8 — all identical.

★ That is the point: we already know it **executes** a command when given one and prints
the result (`ver`, `dir`). So the loop is `for(;;) { get next command; run it; }` working
exactly as designed — running flat out because our answer returns *immediately*.

### ✅ THE INTERACTIVE PATH IS FULLY MAPPED (2026-09-25)

COMMAND.COM has exactly **one** `AH=0Ah` site — guest `0x0A23`, inside a routine at guest
`0x0A0D` that is called with `AL` as its parameter: **`AL=0` reads a line from the
keyboard, `AL≠0` does not**. Four callers; the one that matters is guest `0x0C2F`.

The gate chain in front of it, guest `0x0BF5`…`0x0C2F`:

```
cmp byte [0x327],1   ; jz  -> away          (not from sub 01)
cmp byte [0x32F],0   ; jnz -> away          <- blk +0x1A, low byte
C4 C4 54 10          ; or al,al ; jnz -> away   <- BOP 0x54 SUB 0x10
cmp byte [0x32A],1   ; jnz -> away
cmp word [0x32B],0   ; jz  -> PROMPT        <- blk +0x12, low half
cmp word [0x32D],0   ; jnz -> away          <- blk +0x14, high half
                       AL=0 ; call 0x0A0D   ; ** the prompt **
```

★★ **`+0x12` is not a cookie — it is the interactive switch.** I called it one because
COMMAND.COM loads it from its own `[0x32B]/[0x32D]` before the call and stores it
straight back after, which is exactly what carrying an opaque handle looks like. A
**zero** dword there means *"nothing is driving me: read the keyboard"*. And XP writes
zero in precisely this case: sub 01's tail calls `0x0F04D568`, which ORs one bit per
non-null redirection handle and, when the result is 0, takes the early out at
`0x0F04D5E4` — `xor ebx,ebx … mov eax,ebx ; ret 8` — returning **NULL**. ⛔ *Preserving*
it was the bug: the guest reloads its own non-zero state, we hand it back, and it
concludes it is being driven. Now zeroed, tied to the same condition as the flags.

★ **`BOP 0x54 sub 0x10` is a one-bit query** and it sits on this path.
`0x0F04C829` is five instructions: `cmp dword [0x0F06BB30],0 ; setnz al ; setAL ; ret`.
We were not setting `AL` at all — an unimplemented call that still ANSWERS, at random.
Now `AL = 0`.

`+0x1A` is likewise not ours to leave alone: its low byte becomes `[0x32F]`, tested two
gates earlier. Zeroed.

### ✅✅✅ XP's COMMAND.COM PRINTS A PROMPT (2026-09-25)

```
C:\DOCUME~1\ALLUSE~1\DOCUME~1\ntvdmex\debug\tests\cmdcom>
```

Two things got it there, and **neither was a BOP field I had been staring at**.

**1. `/p` — the permanent-shell switch, on the PSP command tail.** `ntvdm.exe` carries the
format string `%s=%s%s /p %s\system32`; stock NTVDM launches its shell with `/p`, and we
were launching it with no arguments at all. Without `/p` COMMAND.COM is a one-shot command
runner; with it, it is the resident shell. ⚠ This was in the string dump from the start
and I read straight past it for two sessions, because I was looking for the answer inside
the BOP interface.

**2. `sub 0x0D` = "give me a path to open" — the startup batch file.** The guest named
this gap itself: with `/p` it allocates a 7-paragraph block (`AH=48h → 0x0D6D`), issues
the BOP with `DS:DX` into it, and the **very next instruction** is `mov ax,0x3D00 ;
int 21h`. We wrote nothing, so it opened `""` and our DOS said "path not found".

XP's handler (`0x0F012C94`) takes a host ANSI string from `[0x0F09BFC4]` through
`RtlInitAnsiString` → `RtlAnsiStringToUnicodeString` → `RtlUnicodeStringToOemString`,
writing the OEM result to `(getDS()<<4)+getDX()` with a **`0x40`-byte cap**. Which path is
an inference from context (`ntvdm.exe` carries `autoexec.nt`, and this is the `/p` startup
path) — **but it is verified by behaviour**: whatever we write is what the guest opens
next, and the log now reads

```
sub 0D answered: startup batch [C:\AUTOEXEC.BAT] at 0x0000D6F0
INT21 AH=3Dh [C:\AUTOEXEC.BAT] -> AX=0x05
```

⛔ **We do not default to XP's `AUTOEXEC.NT`, deliberately.** The real one loads
`mscdexnt.exe`, `redir` and **`dosx`** — NT's DPMI host, which we provide ourselves and
which has no business in our VDM. `C:\AUTOEXEC.BAT` is the honest default for a DOS that
is ours; `cfg\autoexec.txt` points it anywhere, including at NT's. ⚠ A path that does not
exist is the normal case — a DOS with no AUTOEXEC.BAT simply has none. The bug was the
empty string, not the missing file.

### ⛔⛔ THE PRIVATE NT CONTRACT IS NOT ONLY BOPs -- IT REACHES INTO INT 21h

Tracing the last gate backwards found the blocker **outside this surface entirely**.
COMMAND.COM's *resident* part, guest `0x1692`:

```
mov al,5 ; mov ah,53h ; int 21h ; mov [0x327],al
mov al,7 ; mov ah,53h ; int 21h ; mov [0x328],al
```

Documented `AH=53h` is **BPB->DPB**, takes `DS:SI`/`ES:BP`, and has **no `AL`
sub-function**. XP's COMMAND.COM uses it as a private query with `AL` as a selector and
reads the answer out of `AL` (it also uses `AL=2`). Our handler returned `AX=1` and
`CF=1` -- "unimplemented" -- which put a **1** in `[0x327]`, and three separate gates read
`cmp byte [0x327],1 / jz` as *"do not be the interactive shell"*.

★ So our "unimplemented" was not inert. **It was an answer, and the wrong one.**

⚠ `AX=0` is **provisional and not measured**. It is "not 1", chosen to test whether
`[0x327]` was the gate -- and it was not enough on its own. The right values can only come
from `NTDOS.SYS`: a stock ntvdm run is the only oracle for a private call. **Do not
promote it to a fact without that run.** (6.22's `COMMAND.COM` re-tested after the change:
unaffected -- prompt, `ver`, `dir` all still correct.)

### ✅✅ `AH=53h` MEASURED AGAINST STOCK NTVDM -- 8/8 AGREE

`debug\rig\dosstock.bat P_INT53.COM` drops the IFEO key, runs the probe under stock,
restores the key **unconditionally** and proves it back (before/after both read the same
path, and the target is checked to exist). The bracket is modelled on `w16stock.bat`;
`stockdump.bat` could not be reused -- it restores a hardcoded `C:\ntvdmex\` path that is
not where this rig's host lives.

| `AX` in | stock | ours | | `AX` in | stock | ours |
|---|---|---|---|---|---|---|
| `5300` | `AX=0005 CF=0` | ✅ | | `5304` | `AX=5300 CF=0` | ✅ |
| `5301` | `AX=0001 CF=1` | ✅ | | `5305` | `AX=5301 CF=0` | ✅ |
| `5302` | `AX=5300 CF=0` | ✅ | | `5306` | `AX=5300 CF=0` | ✅ |
| `5303` | `AX=0001 CF=1` | ✅ | | `5307` | `AX=5301 CF=0` | ✅ |

⇒ `AH` is preserved and `AL` carries a 0/1 answer for `02 04 05 06 07`; `01` and `03`
are genuinely unsupported (`AX=1`, `CF=1` -- DOS's "invalid function").

### ⛔⛔ AND THE MEASUREMENT OVERTURNED BOTH OF MY READINGS

**1. The original "unimplemented" was accidentally RIGHT.** It returned `AX=1`, i.e.
`AL=1` -- exactly what stock returns for `AL=5` and `AL=7`, the two COMMAND.COM stores.
I "fixed" it to `AX=0` on a theory and made it **wrong**.

**2. The theory was wrong too.** I read `cmp byte [0x327],1 / jz` as *"do not be the
interactive shell"*. **Stock sets `[0x327]=1` and IS interactive**, so the jump target is
the normal path and my polarity reading was backwards. Every conclusion that leaned on it
is withdrawn.

★ The value was marked **provisional** when it was guessed, and that is the only reason
this is a correction rather than a fact quietly embedded in the code. **Guessing a value
for a private call is not cheaper than measuring it -- it is the same work done twice, and
the guess has to be found and removed.**

⚠ `AL=00`'s row was measured with `SI=0`/`BP=0`, as COMMAND.COM issues it. It is **not**
a claim about the documented BPB->DPB call given a real BPB, which we still do not
implement.

### ✅ THE CONTROL I SHOULD HAVE RUN FIRST: what does STOCK do with the SAME launch?

Same binary, same `/p`, same harness, IFEO key dropped so stock services it:

```
Microsoft(R) Windows DOS
(C)Copyright Microsoft Corp 1990-2001.
The Vdm Redirector is already loaded

C:\DOCUME~1\ALLUSE~1\DOCUME~1\NTVDMEX\DEBUG\TESTS\DOS>
```

**Stock prints a banner and a prompt and then sits there too.** Launched this way -- as a
*program*, with stdout redirected -- stock is no more interactive than we are. So the
remaining difference is much smaller than "it works there and not here":

| | stock | ours |
|---|---|---|
| banner | ✅ `Microsoft(R) Windows DOS` + copyright | ❌ none |
| redirector notice | ✅ `The Vdm Redirector is already loaded` | ❌ none |
| prompt | ✅ | ✅ |
| interactive under redirected stdout | ❌ | ❌ |

★ **One host is not a pass, and one host is not a FAILURE either.** I had been treating
"not interactive" as our defect for two sessions without ever asking what the reference
does in the same position. It does the same thing. The real difference is the banner --
i.e. something earlier in start-up that we skip -- and the *launch path*: stock's own
COMMAND.COM is started by `ntio.sys` during DOS boot, not run as a program.

### ⛔⛔⛔ AND THE CONTROL LEFT THE RIG ROUTING TO STOCK

`dosstock.bat`'s first cut ran the guest **inline and waited for it**. Fine for a probe,
which exits. `COMMAND.COM /p` does not exit -- it sits at its prompt -- so the batch
never reached its restore, and the box was left with **no IFEO key**: every DOS and Win16
launch silently going to stock, with logs that would look entirely plausible.

Caught within a minute because the state file had no `---- IFEO after ----` section, and
repaired by hand (`reg add`, then a probe run confirming our host answered again).

⚠ `w16stock.bat` had this right -- `start` plus a timed kill -- and I did not copy it.
**A bracket whose restore can be skipped by the thing it brackets is not a bracket.**
Fixed, and re-run to prove the restore now happens.

### ⛔ The banner is COMMAND.COM's own, behind THREE gates

Stock's extra output is not a mystery of ours: **`Microsoft(R) Windows DOS` /
`(C)Copyright Microsoft Corp 1990-2001` lives inside `COMMAND.COM`**, in the same
counted-message table as `Incorrect DOS version` (guest `0x21D7`), at guest `0x220A`.
`ntdos.sys`, `ntio.sys` and `ntvdm.exe` do not contain it. (*"The Vdm Redirector is
already loaded"* comes from `redir`, an `autoexec.nt` TSR we deliberately do not load --
that line is **expected** to be absent.)

Its print site, resident guest `0x1C22`:

```
cmp byte [0x326],1 ; jz  -> skip
cmp byte [0x327],1 ; jz  -> skip          <- from AH=53h AL=5
cmp word [0x2B1],0 ; jnz -> skip
mov dx,0x220A ; call print                <- the banner
```

### ⛔⛔ AND THIS QUALIFIES THE 8/8 RESULT

Stock **prints** the banner. Our probe says stock's `AH=53h AL=5` returns `AL=1`. A `1`
in `[0x327]` **skips** the banner. Both cannot be true of the same run, so one of these
holds:

- more gates decide it (**confirmed**: forcing `AL=5 -> 0` did *not* produce the banner,
  so `[0x326]` and/or `[0x2B1]` also block), or
- **the private query is context-dependent** -- our probe asked it as a standalone `.COM`,
  and COMMAND.COM asks it as the shell during start-up.

⚠ **So `8/8 AGREE` means "agrees in the context we measured", not "is the right answer
for the shell".** A private call measured outside the caller's own situation is a
one-machine, one-moment reading. The values are kept -- they are still the only measured
ones -- but the row is marked *provisional-in-context* rather than verified.

⚠ The experiment that established this (`AL=5 -> 0`) was **reverted**: it disagreed with
the only measurement we have, and a guess that contradicts an oracle is worse than no
change.

### ✅✅ THE STATE BLOCK, DUMPED WHOLE -- the instrument that should have come first

Every decision COMMAND.COM makes about being a shell is a `cmp byte [0x32x],n` against a
handful of bytes in its **resident** data segment (`0x0100` for a `.COM`; the transient
reaches them by loading `DS` from `[cs:0x95FE]`). The host now dumps them at each BOP:

```
cc[0x320..0x32F] = 00 00 00 00 01 00 01 01 01 00 01 00 00 00 00 00
                               ^324 ^325 ^326 ^327 ^328 ^329 ^32A
```

| byte | value | meaning |
|---|---|---|
| `[0x326]` | **01** | banner gate A — **1 skips the banner** |
| `[0x327]` | **01** | banner gate B, and the `jz` at guest `0x0BF5` — from `AH=53h AL=5` |
| `[0x328]` | 01 | from `AH=53h AL=7` |
| `[0x32A]` | **01** | ⛔ **"1 IS THE LOOP" WAS BACKWARDS — see the correction below** |
| `[0x32B]/[0x32D]` | 00 00 | our `blk+0x12` ✅ |
| `[0x32F]` | 00 | our `blk+0x1A` ✅ |

★ Five turns of *find a gate, answer it, re-run* produced one caveat. **One dump of the
whole block produced the entire decision state at once**, and it should have been the
first instrument, not the sixth. The pattern is the same one the caller-off-the-stack
instrument taught at the start of this work: *stop deducing state you can print.*

### ✅ `[0x326]` traced: `INT 2Fh AX=5501h`, and stock ANSWERS it

Resident guest `0x16E5`:

```
mov ax,5501h ; int 2Fh
or  ax,ax
jnz -> [0x326] = 1          ; banner SUPPRESSED
    -> store DS:SI away     ; banner allowed
```

`tools/dostest/p_2f55.asm`, measured both ways:

| | `AX` in | `AX` out | `DS:SI` |
|---|---|---|---|
| **stock** | `5501` | **`0000`** | **`040F:0104`** — a real pointer |
| **ours** | `5501` | `5501` (unanswered) | unchanged |
| stock | `5500` | `5500` (**not** answered) | unchanged |

⇒ Stock answers `5501h` with `AX=0` **and a pointer**, so its banner prints; we answer
nothing, so `[0x326]=1` and ours does not. `5500h` is a useful negative: stock does not
answer it either, so a handler must not claim the whole `55xx` range.

⚠ **NOT IMPLEMENTED, deliberately.** COMMAND.COM *stores* that `DS:SI` and uses it later.
Returning `AX=0` with a pointer to something we invented would be an unimplemented call
answering at random -- the exact failure this file already records twice. The banner is
cosmetic.

---

## ★★★★★ XP's COMMAND.COM IS AN INTERACTIVE SHELL (2026-09-25, s79)

```
C:\DOCUME~1\ALLUSE~1\DOCUME~1\ntvdmex\debug\tests\cmdcom>ver

MS-DOS Version 5.00.500

C:\DOCUME~1\ALLUSE~1\DOCUME~1\ntvdmex\debug\tests\cmdcom>dir

 Volume in drive C has no label
 Volume Serial Number is 0C09-23F0
 Directory of C:\DOCUME~1\ALLUSE~1\DOCUME~1\ntvdmex\debug\tests\cmdcom
.            <DIR>         09-24-26   5:18p
..           <DIR>         09-24-26   5:18p
        2 file(s)              0 bytes
             268,431,360 bytes free

C:\DOCUME~1\ALLUSE~1\DOCUME~1\ntvdmex\debug\tests\cmdcom>_
```

Keystrokes scripted through `cfg\keys.txt` with `qimode=0x20`, shots by
`cfg\capture.flag`. **991 prompts per 30-second run became 3.** That matters for the
product: 6.22's shell is Microsoft's and cannot ship, so this is the shell a stock XP
box actually has.

### ✅ And it EXECs a child, and gets control back

Typing the name of a `.COM` in the current directory runs it, its output lands on the
shell's screen, and the shell reprompts:

```
C:\DOCUME~1\...\cmdcom>pv

#PROBE dosver
CASE=int21.30 SIG=AX,BX,CX AX=0005 BX=FF00 ...
#END

C:\DOCUME~1\...\cmdcom>_
```

```
EXEC: "C:\DOCUME~1\ALLUSE~1\DOCUME~1\ntvdmex\debug\tests\cmdcom\PV.COM"
EXEC: child at seg=0x0241 entry=0241:0100 (COM) depth=01
EXEC: child exited rc=0x00, parent resumed (depth=00)
```

⇒ **The whole shell contract turns**: read a line, parse it, `AH=4Bh` a child, let the
child write, take control back, prompt again. That is the broad, boring slice of the DOS
API no game exercises, and it is the reason a shell was chosen as the M9 test.

### ▶ How to turn it on

The two `AH=53h` rows it needs are **not** the built-in defaults (see the open question
below). `scripts/bm/int53-interactive.txt` is the ready-made file; copy it to
`cfg\int53.txt`. The host prints the whole table **and its source** every run, so it can
never be on silently — which is the guard the `dosver.txt` incident bought.

### ⛔⛔⛔ The last bug was a buffer WE corrupted, one field wide

Our `sub 01` "no command" answer wrote `c[0] = 0`. That read of `[0]` came from the
guest's own copy loop at transient `0x06C7` (`mov cl,[0x9327] / add cx,3 / rep movsb`),
which does treat `[0]` as a count -- and it is completely wrong about the buffer,
because **the same buffer is handed to `INT 21h AH=0Ah`**:

```
transient 0x018D   mov byte [ss:0x9327],0x80    <- ONCE, during start-up
transient 0x0A21   mov dx,0x9327 / mov ah,0Ah / int 21h
```

`0x80` is DOS's buffered-input **maximum**, set a single time and never re-set. We
zeroed it on the first BOP, our `AH=0Ah` computed `maxn - 1 = -1`, returned an empty
line without waiting, and COMMAND.COM saw a `CR` at `[2]` and printed its prompt again.
For ever. **The log said so the whole time:**

```
INT21 AH=0A line max=00 n=00 [0d ]            x991
INT21 AH=0A line max=80 n=03 [76 65 72 0d ]   <- after the fix: "ver"
INT21 AH=0A line max=80 n=03 [64 69 72 0d ]   <- "dir"
```

⇒ **Write the length at `[1]`, where DOS puts it, and never touch `[0]`.** Nothing
downstream needs the length — the copy at `0x06C7` takes `[0]+3` bytes (a superset) and
the parser scans from `+2` for the `CR`, so the `CR` is the only load-bearing byte. The
request block's `+0x0C` already told us the capacity was `0x0080`: two independent
sources, and the one we overwrote was the same number.

⚠ **The shell was AT the keyboard read and we were answering it with EOF.** Several
turns read the symptom — *"it prints a prompt and goes straight back to asking"* — as a
gate we had not satisfied. Every gate was satisfied.

### ⛔ `[0x32A] = 1` IS NOT THE LOOP. It is a linked-in image default.

`tools/ntvdm/cmdcom.py` settles it mechanically: `[0x32A]` has **exactly one writer in
the entire binary**, at transient `0x0C2A`, and it writes **0** — two instructions
before the keyboard read. The `01` in the dump is the image's own static initialiser
(`[0x320..0x32F] = 00 00 00 00 01 00 00 00 00 00 01 00 00 00 00 00` as linked). So `1`
is the value that **passes** the gate at `0x0C03`/`0x0C15`, not the one that traps it.

### ★ The loop's top is `INT 21h AX=5302h`, and the gate is its CARRY FLAG

Transient `0x0341`:

```
0341: push ax/si/bp ; xor si,si ; xor bp,bp
0348: mov al,2 ; mov ah,53h ; int 21h      ; ** THE TOP OF THE SHELL'S MAIN LOOP **
0351: jnc 0x0358
0353: jmp 0x0BF5                           ; CF=1 -> THE GATE CHAIN (prompt/keyboard)
0356: jmp 0x0361                           ; <- where the gate chain's "away" lands
0358: cmp byte [0x32A],1
035D: jz  0x0361                           ; =1 -> ask the HOST for a command
035F: jmp 0x0353                           ; !=1 -> the gate chain
0361: ...build the request block... 03CE: BOP 0x54 sub 01
```

With `CF=0` and `[0x32A]=1` — both true of every run before this session — the gate
chain is **never entered at all**, which is why `sub 0x10` had never once fired. Both
answers have to change together, and that was measured one at a time on the rig:

| `AL=2` | `AL=5` | result |
|---|---|---|
| `CF=0` (default) | `AL=1` (default) | 32× `sub 01`, 32× `sub 0E`, `sub 0x10` never reached |
| **`CF=1`** | `AL=1` | **identical** — the chain bails at its first gate, `[0x327]` |
| **`CF=1`** | **`AL=0`** | `sub 0x10` fires for the first time; the spin stops dead |

### ⇒ `AH=53h AL=5` IS CONTEXT-DEPENDENT, and this is a proof rather than a theory

The read-a-line routine is transient `0x0A0D`; `AL=0` reads the keyboard and `AL≠0`
only prints the prompt. It has four callers, and **both** of the `AL=0` ones (`0x0924`
and `0x0C33`) sit directly behind `cmp byte [0x327],1 / jz away`. `[0x327]` has exactly
one writer, `resident 0x169B`:

```
mov al,5 ; mov ah,53h ; int 21h ; mov [0x327],al
```

Stock's COMMAND.COM **is** interactive. Therefore stock answers `AL=0` when the shell
asks. Our probe measured `AL=1`. Both cannot be true of the same question, so they are
not the same question.

⛔ **The likely difference is the harness.** `probe.inc` reports through `INT 21h AH=02`
and every stock measurement is captured with `> FILE` — so the oracle was asked *"is
this console interactive?"* with its own output redirected. **A probe that reports
through stdout cannot measure anything that depends on stdout.**
`tools/dostest/p_int53f.asm` asks the same eight questions through `AH=3Ch/40h/3Eh` and
needs no redirection; run it **both ways** against stock and diff, because one run
cannot tell *"the value is X"* from *"the value is X when redirected"*.

⇒ Until that is measured, the two values live in **`cfg\int53.txt`** and the built-in
defaults stay exactly as measured. A run with no file behaves as it did before. **This
is deliberately not a fix** — the whole reason the earlier `AL=5 -> 0` experiment had to
be retracted was that it was a guess against the only measurement there was.

⚠ And the claim that retraction rested on — *"stock sets `[0x327]=1` and IS interactive,
so that gate does not mean what I read it to mean"* — was an inference from the
standalone probe, not an observation. The gate means exactly what it looked like.

⚠ The earlier experiment was also graded on the **wrong symptom**: it checked for the
banner, which `[0x326]` blocks independently, so it could not have shown the keyboard
path opening even when it did.

---

### ⚠ The bracket needed fixing TWICE

1. It ran the guest **inline and waited** -- fine for a probe, fatal for `COMMAND.COM /p`,
   which never exits. Left the rig with no IFEO key (recorded above).
2. The fix used `start ... > file`, which redirects **START**, not the process it
   launches -- so the next run captured **nothing**, an empty file that looks exactly
   like a guest which printed nothing. Now `start "" cmd /c "... > file"`: redirected by
   the inner shell, detached, and killable.

### ⛔ Real MS-DOS cannot be asked at all

`tools/dostest/p_int53.asm` **hangs MS-DOS 6.22.** Measured twice -- once with a broken
probe and once with a correct one -- so the hang is the **call**: `--host msdos622` never
reaches `QUIT.COM`. The documented form does not *report*, it **builds** a DPB from a
caller-supplied BPB; a fabricated pointer is a mutation, not a question. PCem and
dosbox-x are assumed the same and have not been tried. The probe says so at the top.

⚠ The probe's first cut built its eight case names with a `%1` NASM did not expand, so
all eight emitted under ONE name -- eight questions collapsed into one answer. Longhand
now. *It only announced itself by producing two rows for eight cases.*

`tools/dostest/p_int53.asm` sweeps `AX=5300h`–5307h -- the documented form, the three
XP's COMMAND.COM issues (`02 05 07`), and the gaps, so that a handler answering only the
three we know about could not pass.

⛔⛔ **It hangs MS-DOS 6.22.** Measured twice -- once with a broken probe and once with a
correct one -- so the hang is the **call**, not the probe: `--host msdos622` never reaches
`QUIT.COM`. Documented `AH=53h` does not *report* anything; it **builds** a DPB from a BPB
the caller supplies. Handing it a fabricated pointer is not a question, it is a mutation,
and a real DOS does not survive being asked. PCem and dosbox-x must be assumed the same
and have not been tried.

⇒ There is **no safe oracle for the documented form**, and the private `AL`
sub-functions exist only in `NTDOS.SYS`. **Stock ntvdm is the only oracle**, which needs
the IFEO bracket -- the documented rig-bricking hazard. Not run unattended.

⚠ The probe's first cut built its eight case names with a `%1` substitution NASM did not
expand, so all eight emitted under ONE name: eight questions collapsed into one answer.
Written out longhand now. *A probe that looks fine and measures nothing is the worst kind.*

### ✅ Two more sub-functions answered

| sub | what it is | our answer |
|---|---|---|
| `0D` | the startup batch path (above) | `cfg\autoexec.txt`, default `C:\AUTOEXEC.BAT` |
| `0F` | **the host's `PROMPT`**. `0x0F00BEF6`: if `[0x0F0650BC]` is zero it calls `setBX(0)` and returns; otherwise `GetEnvironmentVariableA` with the name at VA `0x0F00393C` = **`"PROMPT"`** | `BX = 0` -- XP's own zero-path |

Both were previously answered with **no register set at all**, i.e. the guest read
whatever it happened to be holding.

### ⛔ ~~It is not interactive yet~~ — ✅ CLOSED 2026-09-25, and read this as a lesson

*Kept because the reasoning in it was sound and still pointed the wrong way.*

> The prompt prints; the keyboard is not read. Keys reach the guest — `sc_push=0x10`,
> `irq1_inj=0x10`, `KEYLAT deliver n=0x0F` — and nothing consumes them: `int16=[0,0,0,0]`,
> and the `sub 01` spin continues at ~1.25M per run.

⚠ **`int16=[0,0,0,0]` is not evidence that nothing consumed them.** Our `AH=0Ah` reads the
host's own key ring through `m->coninnb`; it never issues `INT 16h`, so that counter reads
zero on a shell that is working perfectly. It reads zero in the ✅ run above too. The
counter that actually moved was `INT21 AH=0A line max=`, and it was in the log all along.

⇒ See **XP's COMMAND.COM IS AN INTERACTIVE SHELL** above: the gate chain was the right
map, `[0x327]` was the right gate, and the last step was a buffer we corrupted ourselves.

### ⛔ The earlier wall, for the record: an empty command line makes it RESTART

Only **two** sub-functions ever fire — `01` and `0E`, 32 of each in a 30-second run. Guest
`0x0BED` (sub `0x10`) is **never reached**, so none of the gate work above is exercised
yet. The loop turns back before it.

Guest `0x06C4`…`0x06EF`, straight after the `AH=0Eh` drive select:

```
copy the counted line at 0x9327 -> 0x93AA
call 0x31F4          ; the parser
jz   +8              ; else  jmp <elsewhere>
call 0x393E          ; "run the command line"
jnc  +13             ; ** CF SET -> ** jmp 0x0104  = RESTART THE TRANSIENT
test byte [0x998E],2 ; jnz -> the same restart
cmp  word [0x9C4C],0 ; jz -> onward, eventually to the prompt
```

⇒ **`call 0x393E` returns carry on an empty command line, and COMMAND.COM restarts.**
That is the 1.25M-iteration loop, and it is not the shell's command loop after all — it
is the transient re-entering itself.

▶ Next: what makes `0x393E` set carry. Either an empty line is simply not a legal
answer — in which case the real behaviour is that `GetNextVDMCommand` **blocks** — or one
of `[0x998E]`, `[0x9C4C]` or the line format is wrong.

▶ **The older hypothesis, kept because it is still live: `GetNextVDMCommand` BLOCKS.** On NT, CSRSS does
not return until there is a command for this VDM. Our instant "nothing" turns a blocking
wait into a spin. ⚠ But blocking cannot be the whole story either, because a shell that
waits for ever never prints a prompt — so something must still distinguish *"wait for
work"* from *"be the interactive shell"*, and that distinction has not been found.

⚠ **Do not implement the block until that second half is understood.** Parking the exec
thread is the exact defect the `retry` pattern exists to avoid ([[dos-shell-and-settings]]:
`AH=0Ah` blocking the exec thread deadlocked a shell), and it would turn a visible spin
into an invisible hang.

---

## 1. The problem this file exists to record

**The number space is NTVDM's, and we have been allocating out of it as if it were
ours.** Our codes are assigned in `src/host/main.c` (the map comment at ~line 499) and
were chosen freely. At least two of them collide with numbers a real NTVDM guest
actually issues:

| code | what a real NTVDM guest uses it for | what WE do with it | source |
|---|---|---|---|
| `0x50` | issued once by XP's `COMMAND.COM` | `DPMI_BOP` — real→protected mode switch | `main.c:441` |
| `0x54` | issued **15×** by XP's `COMMAND.COM`, with a sub-function byte | `DPMI_RMRET_BOP` — DPMI 0301 return catcher | `main.c:448` |

And a collision *inside* our own allocations, worth fixing on sight:

| code | use A | use B |
|---|---|---|
| `0x57` | `DPMI_FAULT_BOP` (`main.c:527`) | `WOWCALL_BOP_CODE` (`wow/wowcall.h:89`) |

### ✅ The fall-through is gated (2026-09-24)

An unmatched BOP is now **refused and logged**, not handed to `dos_int21()`:

```
STAGE2: UNIMPLEMENTED BOP -- refusing (NOT an INT 21h call): bop=0x54 next=0x01
        at 0x9342:0x03ce ax=0x0002 bx=0x0001 dx=0x95d7
STAGE2: task done -> reporting exit code 0x000000bd to CSRSS
```

**We stop; we do not skip.** Skipping needs the BOP's encoded *length*, which is
per-call — `0x54` carries a sub-function byte, so `EIP += 3` would leave that byte to be
executed as an instruction. And the exit code is a distinct non-zero `0xBD`: the defect
being closed was a guest killed by an unimplemented call and reported as a **clean exit
0**, so reusing 0 would have left the lie in place with better logging on top of it.

**The gate is measured, not reasoned.** The diagnostic shipped first (`92e2136`) and the
battery was run with it — necessary, because the BIOS arm deliberately sets
`handled = 0` and falls through, so the control flow alone does not settle who depends
on this:

| battery | guests | all confirmed loaded | fall-throughs |
|---|---|---|---|
| DOS | Doom · Hexen · Duke3D · Heretic · heaven7 · Skyroads · Wolf3D · Mario · Lemmings · Chasm · Gothica · Radiance · Fusion · Skyxmas · vesacube · MEM · 6.22 `COMMAND.COM` | 17/17 | **0** |
| Win16 | Notepad · Paint · WinMine | 3/3 | **0** |
| — | **XP's `COMMAND.COM`** | 1/1 | **1** |

⛔ **Lemmings' first row was a lie and nearly went into that table.** The harness said
`NO SUCH TARGET` (wrong entry name) and the count of fall-throughs was, truthfully,
zero — from a guest that never started. Every row above is gated on
`STAGE2: target.txt loaded`, not on the absence of a hit. ⚠ And the *first* version of
that check looked for `STAGE2: loaded`, which is not what the host prints — it scored
**every** guest as "never ran", including the one with a 4.9 MB log. An absence proves
nothing, and a presence-check that never matches proves nothing either.

### ⛔ What the fall-through did before the gate

The real-mode exec loop matches each BOP code with an exact `==`, and **its last arm
hands anything unmatched to `dos_int21()`** (`main.c` ~26932). INT 21h's own stub is
`C4 C4 20`, so that fall-through was never required — it simply answers for every BOP
it was never given, using the guest's `AH` as if it were a DOS function number.

XP's `COMMAND.COM` issues `BOP 0x54 / sub 0x01` with `AX=0x0002`. `AH=0` is DOS
"terminate". The guest was killed by its own unimplemented call and we reported a
**clean exit, code 0**. See [xp-command-com.md](../research/xp-command-com.md).

Diagnostic in place since `92e2136`:

```
STAGE2: BOP FALL-THROUGH -> INT21: bop=0x54 next=0x01 at 0x9342:0x03ce ax=0x0002
```

---

## 2. Our allocations

Read off the code, `file:line` each. Status is about *our* implementation.

| code | name | purpose | status | where |
|---|---|---|---|---|
| `0x08` | — | INT 08h timer tick stub | IMPL | `main.c:21965` |
| `0x09` | — | INT 09h BIOS keyboard | IMPL | `main.c:21972` |
| `0x10` | — | INT 10h video | IMPL | `main.c:21959` |
| `0x16` | — | INT 16h keyboard | IMPL | `main.c:21960` |
| `0x1A` | — | INT 1Ah BIOS time | IMPL | `main.c:21973` |
| `0x20` | — | **INT 21h DOS** | IMPL | `main.c:21958` |
| `0x2F` | — | INT 2Fh multiplex | IMPL | `main.c:21974` |
| `0x33` | — | INT 33h mouse | IMPL | `main.c:21961` |
| `0x35` | `MS_CB_BOP` | mouse callback return | IMPL | `main.c:6384` |
| `0x43` | — | XMS far-call entry | IMPL | `main.c:21977` |
| `0x50` | `DPMI_BOP` | DPMI real→PM switch | IMPL | `main.c:441` ⚠ **collides** |
| `0x54` | `DPMI_RMRET_BOP` | DPMI 0301 return catcher | IMPL | `main.c:448` ⚠ **collides** |
| `0x55` | `DPMI_CB_BOP` | DPMI 0303 callback entry | IMPL | `main.c:454` |
| `0x56` | `DPMI_PMRET_BOP` | DPMI PM-return catcher | IMPL | `main.c:457` |
| `0x57` | `DPMI_FAULT_BOP` | PM fault trampoline | IMPL | `main.c:527` ⚠ **shared** |
| `0x57` | `WOWCALL_BOP_CODE` | WOW 16-bit call return | IMPL | `wowcall.h:89` ⚠ **shared** |
| `0x58` | `DPMI_RAW2PM_BOP` | DPMI 0306 real→PM | IMPL | `main.c:470` |
| `0x59` | `DPMI_RAW2RM_BOP` | DPMI 0306 PM→real | IMPL | `main.c:472` |
| `0x5A` | `DPMI_FLTRET_BOP` | fault-handler return | IMPL | `main.c:542` |
| `0x67` | — | INT 67h EMM | IMPL | `main.c:21978` |
| `0x11`–`0x17`, `0x25`–`0x30` | — | BIOS services (equipment, memory, disk, …) | IMPL/PART | `main.c:21988`, arm at `~24575` |
| *else* | — | refused + logged, exit `0xBD` | ✅ gated | `main.c` ~26950 |

---

## 3. What XP's `COMMAND.COM` actually issues

**Measured, not remembered:** every `C4 C4` in `C:\WINDOWS\SYSTEM32\COMMAND.COM`
(50,620 bytes, md5 `be67d29ca914de072d9971e3fffc4050`), located by byte scan and
disassembled with `ndisasm -b 16`. **16 sites: 15 × `0x54`, 1 × `0x50`.**

| file off | BOP | sub | registers set immediately before | what the code does with the result |
|---|---|---|---|---|
| `0x02AB` | 54 | `0F` | `BX=0` (after `AH=48h` alloc, `ES`=block) | `AX=BX` → compared against 0 |
| `0x02D0` | 54 | `0F` | `BX`=block size (after 49h/48h realloc) | `cmp bx,ax` — **BX is an output** |
| `0x1581` | 50 | — | `ES`=PSP, after `AH=30h` version check | followed immediately by `INT 20h` |
| `0x1A84` | 54 | `02` | `DX=0x0258` | `mov [0x2d2],al` — **AL is an output** |
| `0x1B74` | 54 | `0D` | `DS:DX` → a buffer just built by `stosw` | then `AX=0x3D00`, `INT 21h` (open) |
| `0x1DF3` | 54 | `08` | 6-word `0xFFFF` frame, `BP=SP`, `ES`=CDS, `AH=0` | `lahf` / `add sp,0xC` — **CF is an output** |
| `0x283E` | 54 | `01` | `DX=0x95D7` (a struct filled from `[es:0x32B/0x32D]`) | `jnc`, then `cmp ax,0x8000` |
| `0x2A56` | 54 | `0E` | `SI=0x04B9`, `CX=0x0539` | `or dx,dx` — **DX is an output** |
| `0x2B24` | 54 | `09` | — (after a size calc stored at `[0x961F]`) | falls straight through |
| `0x2DEB` | 54 | `0A` | `0xFFFF` frame, `BP=SP`, `ES=[0x458]`, `AH=19h` first | `BP=AX`, `lahf`, `add sp,0xC` |
| `0x2E25` | 54 | `0B` | `CX=[0x32B]`, `BX=[0x32D]`, `AH=19h` first | `jnc` — **CF is an output** |
| `0x2EE5` | 54 | `08` | 6-word `0xFFFF` frame, `BP=SP`, `AH=1` | `lahf` / `add sp,0xC` |
| `0x2F1B` | 54 | `0B` | `CX=[es:0x32B]`, `BX=[es:0x32D]` | — |
| `0x303D` | 54 | `06` | `BX=[0x32B]`, `AX=[0x32D]` | `jc` — **CF is an output** |
| `0x307D` | 54 | `10` | — | `or al,al` — **AL is an output** |
| `0x4E24` | 54 | `00` | — | — |

### What can already be said from this

1. **`BOP 0x54` takes a one-byte sub-function immediately after the code.** Eleven
   distinct sub-functions: `00 01 02 06 08 09 0A 0B 0D 0E 0F 10`.
2. **Some sub-functions are stack-based.** `08` and `0A` push six `0xFFFF` words, set
   `BP=SP`, call, then `lahf` / `add sp,0xC` / `sahf` — a frame the host writes into,
   and a carry flag the host sets.
3. **`AH=19h` (get current drive) is issued immediately before several of them**, which
   looks like a deliberate state sync rather than a coincidence — it appears at `0x1DF3`,
   `0x2DEB`, `0x2E25`, `0x2F1B`.
4. **The pair `[0x32B]` / `[0x32D]` is passed repeatedly** in `CX`/`BX` or `BX`/`AX`.
5. ★ **`BOP 0x54 / sub 01` is the one that kills us**, and the live trace corroborates
   the static read exactly: `DX=0x95D7` in the log, `mov dx,0x95d7` at file `0x283E`.

⚠ **NOT established: what any of these sub-functions MEAN.** `sub 01` sits on the path
that a shell would use to obtain its next command line, and `0x8000` is a suggestive
thing to compare a return against — **that is a hypothesis with no measurement behind
it.** Do not write it into this table until stock ntvdm has been asked.

### And one thing that IS established, from the same disassembly

At file `0x1563`:

```
mov ah,0x50 ; mov bx,es ; int 21h     ; set PSP
mov ah,0x30 ; int 21h                 ; get DOS version
cmp ax,0x0005                         ; <-- AX, not AL
jz  ok
mov dx,0x21d7 ; call print            ; "Incorrect DOS version"
```

The check is `cmp ax,5` on the **whole word** — `AL`=major, `AH`=minor — so it demands
**exactly 5.00**, not "5 or later". `cfg\dosver.txt` = `5.0` satisfies it; anything else
does not. That matches the measured 0-calls-vs-9-calls result in
[xp-command-com.md](../research/xp-command-com.md) and pins *why*.

---

## 3a. Who else issues one — a measured negative

Byte-scanned every `.exe`/`.com` under `demo/msdos/` and every `.exe`/`.dll`/`.drv`
under `demo/win16/` on the rig. `C4 C4` pairs turn up in nineteen DOS binaries and
three Win16 ones, and **every one of them is noise**: runs of `C4` inside compressed or
resource data (`ZAR/SOUND/SETSOUND.EXE` alone has 362, 344 of them `C4 C4 C4`), with no
third byte matching a plausible call number and none of them at a code site.

⚠ A byte pattern is not an executed instruction, so this is weak evidence on its own —
but it agrees with the sweep (below) and with the shape of the mechanism: **BOPs are how
*NT's own* 16-bit components talk to the 32-bit side.** Ordinary period software has no
reason to know the instruction exists. That bounds the collision risk to
Microsoft-shipped guests — which is a small set, and `COMMAND.COM` is the one that
matters.

---

## 4. Owed

1. ~~Gate the fall-through.~~ ✅ done, measured — see §1.
2. **Resolve the `0x57` double-booking** between `DPMI_FAULT_BOP` and
   `WOWCALL_BOP_CODE`. They are probably never live in the same guest, but "probably" is
   how the `0x54` collision survived too.
3. **Re-number ours out of the range real NTVDM guests use**, or dispatch on more than
   the code byte. A collision is only invisible until an NTVDM-aware guest turns up.
4. **Measure `BOP 0x54`'s sub-functions against stock ntvdm** — `tools/wintest/stock.sh`
   is the pattern for dropping the IFEO key (⛔ read its warnings first). The refusal log
   line now prints `AX`, `BX`, `DX` and the sub-function byte, so a side-by-side against
   stock has something to compare.
5. **6.22's own `COMMAND.COM` loads, runs, and makes ZERO INT 21h calls** (this battery,
   `result_dos.log`). It issues no BOPs either, so it is a different question entirely
   and has not been looked at.
