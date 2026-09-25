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

## 0. ▶ THE DIRECTIVE, AND WHAT IS NOW KNOWN (2026-09-24)

> **User:** *"We need to support command.com that ships with Windows XP. Running the
> host with no guest should use /System32/command.com as the guest."*

Both halves are in. A no-guest launch loads `C:\WINDOWS\SYSTEM32\COMMAND.COM`
(`cfg\shell.txt` overrides it for testing); the remaining work is this surface.

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

### ⛔ THE WALL, located exactly: an empty command line makes it RESTART

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
