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

### Status (re-checked against the code 2026-10-01)

✅ **XP's `COMMAND.COM` is the default shell and it is interactive** — prompt, `ver`, `dir`,
and an EXEC'd child that returns (s79, below). ✅ **A no-argument launch reaches it**: the
host writes a four-byte DOS stub and runs it, so CSRSS builds a VDM that the IFEO key
routes back to us (`LAUNCH_STUB_NAME` `main.c:223`, the launcher `main.c:4661`). The
older note here — *"nothing reaches that path yet"* — predates that and is withdrawn.

The interactive `AH=53h` answers are **built in** for an NTVDM-aware shell, detected by
its BOP count (`g_guest_ntaware`, `main.c:26933`; the table `main.c:27431-27459`).
`cfg\int53.txt` still overrides them (`main.c:27460-27487`) and the source is printed every
run. ⚠ The model is still **provisional** (#142): only the shell itself can be asked for
the `AL=0` answer it is given.

### The collision is resolved by ORIGIN, not by renumbering

Every BOP **we** plant, we plant at an address we own — `DOS_HDLR_SEG` for the INT
stubs, DPMI entry/return catchers and callback slots, `DOS_CTAB_SEG` for the BIOS stubs.
A BOP executing anywhere else is the guest calling NTVDM. `g_bop_from_guest` is set from
`CS` before any arm runs, and the DPMI arm is gated on it.

⚠ **Renumbering ours would have been worse**: the numbers we'd move to are equally
NTVDM's, so it relocates the collision instead of removing it. The origin test is exact.

### ✅ The encoding, confirmed across THREE binaries

`C4 C4 <bop> <sub>` — **four bytes**, for `0x50` and `0x54`. Observed in XP's
`COMMAND.COM` first, then confirmed by byte-scanning XP's own 16-bit VDM components, pulled
off the rig:

| binary (XP SP3) | size | `BOP 0x50` | `BOP 0x54` | other |
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

`cfg\bop54.txt` = `cf0` (default) / `cf1`. The shell tests carry straight after sub 01.

| answer | what COMMAND.COM does |
|---|---|
| `CF=1` | **polls the call forever** — one log line per spin, 268,435,180 bytes before it was rate-limited |
| `CF=0` | proceeds to a **second, different** call (`sub 0x0E`), then spins in V86 with no traps |

Neither is known to be right; `CF=1` is known to be a dead end. ⛔ **The instrument is now
rate-limited** — 16 in full, then only when the register signature changes. *An
instrument that scales with a guest's spin rate is a denial-of-service on the thing you
are trying to read.*

### ✅✅ Which BOP numbers stock NTVDM implements

Of the 256 possible BOP numbers, XP SP3's `ntvdm.exe` routes 195 to a single
"unimplemented" handler. **61 BOPs are real:**

```
00 02 06 09 0E 10 11 12 13 14 15 16 17 18 19 1A 1D 21 40 42
50 51 52 53 54 55 56 57 58 59 5A 5B 5C 5D 5E 5F 60 66 67 68
70 71 72 73 74 75 76 77  B8 B9 BA BB BC BD BE BF  C8 C9  FD FE FF
```

⛔⛔ **Our DPMI numbering collides with far more than the two we knew about.** `0x50`,
`0x54`, `0x55`, `0x56`, `0x57`, `0x58`, `0x59`, `0x5A` are *all* real NTVDM BOPs, and we
use every one of them. The origin test covers this — but it is the reason the origin test
had to be the fix rather than renumbering.

### ✅ `BOP 0x54` — the sub-function byte and its range

★ NTVDM steps over a **one-byte sub-function** after `C4 C4 54` — the BOP is three bytes
and the sub-function is the fourth, so a host that answers it must resume at `IP+4`.
**17 sub-functions, `00`–`10`, are live**; nothing above `10` is. That is precisely the
range used across COMMAND.COM (`00 01 02 06 08 09 0A 0B 0D 0E 0F 10`), `ntdos.sys`
(`04 05 07`) and `ntio.sys` (`09 0C`).

| sub | issued by | notes |
|---|---|---|
| `00` | COMMAND.COM | **same effect as the exported `VDDTerminateVDM`** — ends the VDM |
| `01` | COMMAND.COM | **get the next command** — below |
| `02` | COMMAND.COM | |
| `03` | — | **unimplemented in XP as well** |
| `04`, `05`, `07` | `ntdos.sys` | |
| `06`, `08`, `0A`, `0B` | COMMAND.COM | |
| `09` | `ntio.sys`, COMMAND.COM | |
| `0C` | `ntio.sys` | |
| `0D` | COMMAND.COM | the startup batch path — below |
| `0E` | COMMAND.COM | **the second one COMMAND.COM reaches** — keyboard / code page |
| `0F` | COMMAND.COM | the host's `PROMPT` — below |
| `10` | COMMAND.COM | a one-bit query — below |

### ▶ `sub 0x01` = get the next command

**sub 01 asks CSRSS what to run next** (`GetNextVDMCommand`, with a `VDM_COMMAND_INFO`
— an OS ABI, layout from ReactOS) — exactly what a shell does at startup, and exactly the
call our own STAGE1 already makes (`STAGE1: command fetch …`). Input: `DS:DX` → a request
block owned by the shell.

#### The request block at `DS:DX`

Field directions are the interface as established by studying both sides for
interoperability, confirmed by running the shell against our answers:

| off | size | dir | meaning |
|---|---|---|---|
| `+0x00` | word | in | |
| `+0x02` | word | in/out | |
| `+0x04` | byte in, word out | in/out | **the default drive** — the shell selects it with `AH=0Eh` straight after; the guest supplies it (`02` = C:) |
| `+0x06` | word | out | |
| `+0x08:+0x0A` | seg:off | in | **the command-tail buffer** — layout below |
| `+0x0C` | word | in | the tail buffer's capacity (`0x0080`) |
| `+0x0E` | word | in | copied into the `VDM_COMMAND_INFO` |
| `+0x10` | word | **out** | **redirection flags**: bit 0 stdin, bit 1 stdout, bit 2 stderr. Zero for an interactive shell |
| `+0x12` | dword | **in+out** | **the interactive switch** — NULL when nothing is redirected; see below |
| `+0x16` | word | out | **the code page** — the shell compares it with `AH=66h/01`'s `BX` and issues `AH=66h/02` if they differ (behind the `INT 2Fh AX=1400h` NLSFUNC check) |
| `+0x18` | word | in | |
| `+0x1A` | word | out | low byte gates the keyboard path — must be **0** |
| `+0x1C:+0x1E` | seg:off | in | **the program-name buffer** |
| `+0x20` | word | in/out | the name buffer's capacity |
| `+0x22` | word | out | **the TYPE of the program to run** — below |

★ **A defined answer is falsifiable; an uninitialised one is not.** Leaving the block
alone hands COMMAND.COM whatever was already in its memory. But equally: *a named wrong
value is worse than an unchanged right one* — sub 01 writes only the fields it can name.

#### ⛔ `+0x04` IS THE DEFAULT DRIVE, and zeroing it sent the shell to A:

Found by running it. Straight after sub 01 returns, the shell selects `+0x04` as the
default drive (`INT 21h AH=0Eh`). A zero there is drive 0 = **A:**, and our own handler
said so in the same log — *"drive A: exists but is not ready … selected as the DOS current
drive anyway"* — while the shell went quiet. The trace now reads `21:0e/80 … dx=0002` =
**C:** — and it turned out the guest already supplies `02`: **it was never ours to fill
in**, so sub 01 now leaves it alone.

★ **This is what "a defined answer is falsifiable" buys.** The wrong value showed up in
one run and named its own field. An uninitialised block would have produced a different
wrong drive each run and read as flakiness.

⚠ And `dos_cur_drive()` was `static`: it is now published as `dos_int21_cur_drive()`
rather than re-derived in the host, because a second copy of the rule would have
silently dropped the `vdrive` case.

#### The command-tail buffer (`+0x08:+0x0A`)

`[0]` = maximum, `[1]` = length, `[2..]` = text, then **CR** — the `INT 21h AH=0Ah`
layout, because **the shell hands the same buffer to `AH=0Ah`** for its keyboard reads. It
sets `[0]` to `0x80` once, at start-up, and never again.

⇒ **Write the length at `[1]`, the text from `[2]`, a CR after it, and never touch `[0]`.**
The CR is the only load-bearing byte for the parser: with no CR in the buffer the shell's
parser scans the whole 64K segment and never leaves — the "spinning in V86 with no traps"
the headless deadline kept killing.

#### ✅ `+0x22` = the program TYPE, by extension

Stock sets it from the last four characters of the program name:

| name | `+0x22` |
|---|---|
| ends `.BAT` | `2` |
| ends `.EXE` | `4` |
| ends `.COM` | `8` |
| anything else, or a name of 6 characters or fewer | `9` |

So `sub 01` returns *three* things, not one: a command **tail** (`+0x08:+0x0A`), a program
**name** (`+0x1C:+0x1E`, capacity `+0x20`), and that program's **type** (`+0x22`). On the
no-program path the shell empties the name buffer itself.

All three are now answered: the tail as before, the name from `progpath` on the first
call only (⚠ **not** `g_app2` — on the rig CSRSS names `dosstub.com`, the harness stub),
the type by XP's own extension rule, and empty/`9` on every call after.

⛔ An earlier note here read `+0x22`'s three observed values as *"a status enumeration,
one of them very likely 'no more commands'"*. It was not; it is the type.

#### ★★ `+0x12` is not a cookie — it is the interactive switch

The shell loads the dword from its own state before the call and stores it straight back
after, which is exactly what carrying an opaque handle looks like — so it was first
*preserved*. **A zero dword there means "nothing is driving me: read the keyboard"**, and
stock returns zero in precisely the case where nothing is redirected (the same condition
that makes `+0x10` zero). ⛔ *Preserving* it was the bug: the guest reloads its own
non-zero state, we hand it back, and it concludes it is being driven. Now zeroed, tied to
the same condition as the flags.

### ✅ `sub 0x0E` = the keyboard / code-page configuration

Issued straight after `INT 2Fh AX=AD80h` (the KEYB installed-check), with `DS:SI` → a
buffer and `CX = 0x539` (1337 bytes); output in `DX`. ⇒ **sub 0E asks the host for the
console's keyboard layout and code page** (stock: the console's layout; code page 932 on
a Japanese-language system). Ours answers `DX=0` — no KEYB to load.

### ✅ `sub 0x10` = a one-bit query

Output in `AL`, tested zero/non-zero; stock answers 0 or 1 from a single host flag. We
were not setting `AL` at all — an unimplemented call that still ANSWERS, at random. Now
**`AL = 0`**, the value the keyboard path needs (below).

### ✅ `sub 0x0F` = the host's `PROMPT`

`BX` in (0, or a block size after an `AH=48h`/`49h` allocation), `BX` out — the shell
compares it with the size it holds. Stock answers from the host's `PROMPT` environment
variable when it is in that mode, and otherwise returns **`BX = 0`**. Our first answer was `BX = 0`,
XP's own default; §2b has the current one. (Previously answered with no register set at
all, i.e. the
guest read whatever it happened to be holding — as was `sub 0D`.)

### ✅ `sub 0x0D` = "give me a path to open" — the startup batch file

With `/p` the shell allocates a 7-paragraph block (`AH=48h`), issues the BOP with `DS:DX`
into it, and **immediately** opens it (`AX=3D00h`). Stock writes an **OEM-code-page
path, at most `0x40` bytes**, there. Which path is an inference from context (stock uses
`autoexec.nt`, and this is the `/p` startup path) — **but it is verified by behaviour**:
whatever we write is what the guest opens next:

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

### ▶ `/p` — the permanent-shell switch

**Stock starts its shell with `/p`** on the PSP command tail; we were launching it with no
arguments at all. Without `/p` COMMAND.COM is a one-shot command runner; with it, it is
the resident shell. ⚠ This was visible from the start and was read straight past for two
sessions, because the answer was being sought inside the BOP interface.

### ⛔ `AH=66h` was not the gap

`AH=66h` looked like the gap — it returned `AX` unchanged — and **it is fully implemented**
(`dos_int21.c:1204`, `BX=DX=437`). AX simply is not one of its outputs. That is the
**fourth** time the "AX unchanged ⇒ unserviced" heuristic would have invented a gap; the
file's own `dos622_defines` comment warns about exactly this.

### ⛔⛔⛔ FOUR INSTRUMENTS IN ONE SESSION NEEDED A CAP

The shell's loop turned every per-call log line into a file. In order:

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

### ⛔⛔ THE PRIVATE NT CONTRACT IS NOT ONLY BOPs — IT REACHES INTO INT 21h

XP's COMMAND.COM issues **`INT 21h AH=53h` with `AL` = 2, 5 and 7**, uses `AL` as a
selector and reads the answer back out of `AL` (and, for `AL=2`, `CF`). Documented
`AH=53h` is **BPB→DPB**, takes `DS:SI`/`ES:BP`, and has **no `AL` sub-function**: this is
a private query, and only stock can answer what it means.

★ Our handler returned `AX=1`, `CF=1` — "unimplemented" — and **that was an answer, not
an absence**: the shell acts on it.

#### ✅✅ `AH=53h` MEASURED AGAINST STOCK NTVDM — 8/8 AGREE

`debug\rig\dosstock.bat P_INT53.COM` drops the IFEO key, runs the probe under stock,
restores the key **unconditionally** and proves it back (before/after both read the same
path, and the target is checked to exist). The bracket is modelled on `w16stock.bat`;
`stockdump.bat` could not be reused — it restores a hardcoded `C:\ntvdmex\` path that is
not where this rig's host lives.

| `AX` in | stock | ours | | `AX` in | stock | ours |
|---|---|---|---|---|---|---|
| `5300` | `AX=0005 CF=0` | ✅ | | `5304` | `AX=5300 CF=0` | ✅ |
| `5301` | `AX=0001 CF=1` | ✅ | | `5305` | `AX=5301 CF=0` | ✅ |
| `5302` | `AX=5300 CF=0` | ✅ | | `5306` | `AX=5300 CF=0` | ✅ |
| `5303` | `AX=0001 CF=1` | ✅ | | `5307` | `AX=5301 CF=0` | ✅ |

⇒ `AH` is preserved and `AL` carries a 0/1 answer for `02 04 05 06 07`; `01` and `03`
are genuinely unsupported (`AX=1`, `CF=1` — DOS's "invalid function").

⛔⛔ **And the measurement overturned a guess.** The original "unimplemented" `AX=1` was
accidentally **right** for `AL=5`/`AL=7`; it had been "fixed" to `AX=0` on a theory and
made **wrong**. The value was marked **provisional** when it was guessed, and that is the
only reason this is a correction rather than a fact quietly embedded in the code.
**Guessing a value for a private call is not cheaper than measuring it — it is the same
work done twice, and the guess has to be found and removed.**

⚠ `AL=00`'s row was measured with `SI=0`/`BP=0`, as COMMAND.COM issues it. It is **not**
a claim about the documented BPB→DPB call given a real BPB, which we still do not
implement.

#### ⇒ `AH=53h AL=5` IS CONTEXT-DEPENDENT

Stock's own shell **is** interactive, and (below) the shell only reads the keyboard if
`AL=5` answers **0**. A standalone probe under stock measures **1**. Both cannot be true
of the same question, so they are not the same question.

✅ **Measured (s84, #142): redirection is NOT the difference.** `probe.inc` reports
through stdout, so every earlier stock reading was taken with `> FILE` in place;
`tests/probes/dos/p_int53f.asm` asks the same eight questions through `AH=3Ch/40h/3Eh`
and needs no redirection. Stock, both ways, answers all eight identically
(`5305 → AX=5301`, `5302 → 5300 CF=0`); only DS differs (`runs/s84/stock/`). So the
remaining difference is the **caller**: a probe is always a child of stock's shell, and
the `AL=0` answer can only come when the shell itself asks. The NTVDM-aware-shell branch
in `main.c` stays the model, still marked provisional; `cfg\int53.txt` overrides it.

⚠ **So `8/8 AGREE` means "agrees in the context we measured", not "is the right answer
for the shell".** A private call measured outside the caller's own situation is a
one-machine, one-moment reading.

### ✅ THE CONTROL I SHOULD HAVE RUN FIRST: what does STOCK do with the SAME launch?

Same binary, same `/p`, same harness, IFEO key dropped so stock services it:

```
Microsoft(R) Windows DOS
(C)Copyright Microsoft Corp 1990-2001.
The Vdm Redirector is already loaded

C:\DOCUME~1\ALLUSE~1\DOCUME~1\NTVDMEX\DEBUG\TESTS\DOS>
```

**Stock prints a banner and a prompt and then sits there too.** Launched this way — as a
*program*, with stdout redirected — stock is no more interactive than we were. So the
remaining difference was much smaller than "it works there and not here":

| | stock | ours |
|---|---|---|
| banner | ✅ `Microsoft(R) Windows DOS` + copyright | ❌ none |
| redirector notice | ✅ `The Vdm Redirector is already loaded` | ❌ none |
| prompt | ✅ | ✅ |
| interactive under redirected stdout | ❌ | ❌ |

★ **One host is not a pass, and one host is not a FAILURE either.** "Not interactive" was
treated as our defect for two sessions without ever asking what the reference does in the
same position. It does the same thing. Stock's own COMMAND.COM is started by `ntio.sys`
during DOS boot, not run as a program.

### ⛔⛔⛔ AND THE CONTROL LEFT THE RIG ROUTING TO STOCK

`dosstock.bat`'s first cut ran the guest **inline and waited for it**. Fine for a probe,
which exits. `COMMAND.COM /p` does not exit — it sits at its prompt — so the batch
never reached its restore, and the box was left with **no IFEO key**: every DOS and Win16
launch silently going to stock, with logs that would look entirely plausible.

Caught within a minute because the state file had no `---- IFEO after ----` section, and
repaired by hand (`reg add`, then a probe run confirming our host answered again).

⚠ `w16stock.bat` had this right — `start` plus a timed kill — and it was not copied.
**A bracket whose restore can be skipped by the thing it brackets is not a bracket.**
Fixed, and re-run to prove the restore now happens.

### ⛔ The banner is COMMAND.COM's own

Stock's extra output is not a mystery of ours: **`Microsoft(R) Windows DOS` /
`(C)Copyright Microsoft Corp 1990-2001` is COMMAND.COM's own message** (in the same
message set as `Incorrect DOS version`); `ntdos.sys`, `ntio.sys` and `ntvdm.exe` do not
contain it. (*"The Vdm Redirector is already loaded"* comes from `redir`, an
`autoexec.nt` TSR we deliberately do not load — that line is **expected** to be absent.)

The shell prints it only when **all** of these hold: `INT 2Fh AX=5501h` was answered with
`AX=0`, `AH=53h AL=5` did not answer 1, and a third start-up condition. Forcing
`AL=5 → 0` alone did *not* produce it — and that experiment was **reverted**: it
disagreed with the only measurement we have, and a guess that contradicts an oracle is
worse than no change. (It was also graded on the **wrong symptom**: the banner is blocked
independently, so it could not have shown the keyboard path opening even when it did.)

#### `INT 2Fh AX=5501h` — stock ANSWERS it

`tests/probes/dos/p_2f55.asm`, measured both ways:

| | `AX` in | `AX` out | `DS:SI` |
|---|---|---|---|
| **stock** | `5501` | **`0000`** | **`040F:0104`** — a real pointer |
| **ours** | `5501` | `5501` (unanswered) | unchanged |
| stock | `5500` | `5500` (**not** answered) | unchanged |

⇒ Stock answers `5501h` with `AX=0` **and a pointer**, so its banner prints; we answer
nothing, so ours does not. `5500h` is a useful negative: stock does not answer it either,
so a handler must not claim the whole `55xx` range.

⚠ **NOT IMPLEMENTED, deliberately.** COMMAND.COM *keeps* that `DS:SI` and uses it later.
Returning `AX=0` with a pointer to something we invented would be an unimplemented call
answering at random — the exact failure this file already records twice. The banner is
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

~~Copy `scripts/bm/int53-interactive.txt` to `cfg\int53.txt`.~~ **Superseded (s79):** it is
on by default for an NTVDM-aware shell — see *Status* at the top. `cfg\int53.txt` remains
an explicit override, and the host prints the table and its source every run.

### ✅ THE KEYBOARD PATH: what the host must answer

XP's COMMAND.COM reads a line from the keyboard (`INT 21h AH=0Ah`, into the sub 01 tail
buffer) only when every one of these host answers says so:

| # | host answer | value for "read the keyboard" |
|---|---|---|
| 1 | `INT 21h AX=5302h` — issued at the top of each pass of the shell's main loop | **`CF=1`** (`CF=0` sends it straight to sub 01 for another command) |
| 2 | `INT 21h AX=5305h`, asked once at start-up | **`AL=0`** |
| 3 | sub 01's `+0x1A` low byte | **0** |
| 4 | `BOP 0x54 sub 0x10` | **`AL=0`** |
| 5 | sub 01's `+0x12` dword | **0** (nothing redirected) |

With `CF=0` on (1) — true of every run before this was found — the rest are **never
consulted at all**, which is why `sub 0x10` had never once fired. (1) and (2) have to
change together, and that was measured one at a time on the rig:

| `AL=2` | `AL=5` | result |
|---|---|---|
| `CF=0` (default) | `AL=1` (default) | 32× `sub 01`, 32× `sub 0E`, `sub 0x10` never reached |
| **`CF=1`** | `AL=1` | **identical** — the path bails at its first condition |
| **`CF=1`** | **`AL=0`** | `sub 0x10` fires for the first time; the spin stops dead |

★ **Dumping the shell's whole decision state at once** — rather than finding one condition,
answering it and re-running — produced this table in one step after five turns of the
other method. *Stop deducing state you can print.*

### ⛔⛔⛔ The last bug was a buffer WE corrupted, one field wide

Our `sub 01` "no command" answer wrote `[0] = 0` in the tail buffer, on the reading that
`[0]` is a count. But **the same buffer is handed to `INT 21h AH=0Ah`**, and `[0]` is
DOS's buffered-input **maximum**, set by the shell a single time and never re-set. We
zeroed it on the first BOP, our `AH=0Ah` computed `maxn - 1 = -1`, returned an empty line
without waiting, and COMMAND.COM saw a `CR` at `[2]` and printed its prompt again. For
ever. **The log said so the whole time:**

```
INT21 AH=0A line max=00 n=00 [0d ]            x991
INT21 AH=0A line max=80 n=03 [76 65 72 0d ]   <- after the fix: "ver"
INT21 AH=0A line max=80 n=03 [64 69 72 0d ]   <- "dir"
```

⇒ **Write the length at `[1]`, where DOS puts it, and never touch `[0]`.** The request
block's `+0x0C` already told us the capacity was `0x0080`: two independent sources, and
the one we overwrote was the same number.

⚠ **The shell was AT the keyboard read and we were answering it with EOF.** Several
turns read the symptom — *"it prints a prompt and goes straight back to asking"* — as a
condition we had not satisfied. Every condition was satisfied.

### ⛔ ~~It is not interactive yet~~ — ✅ CLOSED 2026-09-25, and read this as a lesson

*Kept because the reasoning in it was sound and still pointed the wrong way.*

> The prompt prints; the keyboard is not read. Keys reach the guest — `sc_push=0x10`,
> `irq1_inj=0x10`, `KEYLAT deliver n=0x0F` — and nothing consumes them: `int16=[0,0,0,0]`,
> and the `sub 01` spin continues at ~1.25M per run.

⚠ **`int16=[0,0,0,0]` is not evidence that nothing consumed them.** Our `AH=0Ah` reads the
host's own key ring through `m->coninnb`; it never issues `INT 16h`, so that counter reads
zero on a shell that is working perfectly. It reads zero in the ✅ run above too. The
counter that actually moved was `INT21 AH=0A line max=`, and it was in the log all along.

### ⛔ The earlier walls, for the record

- **The main loop.** With an empty answer COMMAND.COM asked again immediately —
  **~1,250,000 `sub 01` calls in a 30-second run**, with every field well-formed and
  three different answers giving identical runs. That is its *command loop* — get a
  command, run it, repeat — running flat out because our answer returned *immediately*.
- **An empty command line made the transient part restart** rather than reach the
  prompt; the cause was the zeroed buffer maximum above.
- **The hypothesis that `GetNextVDMCommand` BLOCKS** (on NT, CSRSS does not return until
  there is a command for this VDM) was never needed: the keyboard path above is what
  distinguishes *"wait for work"* from *"be the interactive shell"*. ⚠ Parking the exec
  thread is the exact defect the `retry` pattern exists to avoid
  ([[dos-shell-and-settings]]: `AH=0Ah` blocking the exec thread deadlocked a shell).

---

### ⚠ The bracket needed fixing TWICE

1. It ran the guest **inline and waited** — fine for a probe, fatal for `COMMAND.COM /p`,
   which never exits. Left the rig with no IFEO key (recorded above).
2. The fix used `start ... > file`, which redirects **START**, not the process it
   launches — so the next run captured **nothing**, an empty file that looks exactly
   like a guest which printed nothing. Now `start "" cmd /c "... > file"`: redirected by
   the inner shell, detached, and killable.

### ⛔ Real MS-DOS cannot be asked at all

`tests/probes/dos/p_int53.asm` sweeps `AX=5300h`–`5307h` — the documented form, the three
XP's COMMAND.COM issues (`02 05 07`), and the gaps, so that a handler answering only the
three we know about could not pass.

⛔⛔ **It hangs MS-DOS 6.22.** Measured twice — once with a broken probe and once with a
correct one — so the hang is the **call**, not the probe: `--host msdos622` never reaches
`QUIT.COM`. Documented `AH=53h` does not *report* anything; it **builds** a DPB from a BPB
the caller supplies. Handing it a fabricated pointer is not a question, it is a mutation,
and a real DOS does not survive being asked. PCem and dosbox-x must be assumed the same
and have not been tried.

⇒ There is **no safe oracle for the documented form**, and the private `AL`
sub-functions exist only in `NTDOS.SYS`. **Stock ntvdm is the only oracle**, which needs
the IFEO bracket — the documented rig-bricking hazard. Not run unattended.

⚠ The probe's first cut built its eight case names with a `%1` substitution NASM did not
expand, so all eight emitted under ONE name: eight questions collapsed into one answer.
Written out longhand now. *A probe that looks fine and measures nothing is the worst kind.*


---

## 1. The problem this file exists to record

**The number space is NTVDM's, and we have been allocating out of it as if it were
ours.** Our codes are assigned in `src/host/main.c` (the `#define`s at `:550-699`) and
were chosen freely. At least two of them collide with numbers a real NTVDM guest
actually issues:

| code | what a real NTVDM guest uses it for | what WE do with it | source |
|---|---|---|---|
| `0x50` | issued once by XP's `COMMAND.COM` | `DPMI_BOP` — real→protected mode switch | `main.c:550` |
| `0x54` | issued **15×** by XP's `COMMAND.COM`, with a sub-function byte | `DPMI_RMRET_BOP` — DPMI 0301 return catcher | `main.c:557` |

And a collision *inside* our own allocations, worth fixing on sight:

| code | use A | use B |
|---|---|---|
| `0x57` | `DPMI_FAULT_BOP` (`main.c:684`) | `WOWCALL_BOP_CODE` (`wow/wowcall.h:89`) — still shared, 2026-10-01 |

### ✅ The fall-through is gated (2026-09-24)

An unmatched BOP is now **refused and logged**, not handed to `dos_int21()`:

```
STAGE2: UNIMPLEMENTED BOP -- refusing (NOT an INT 21h call): bop=0x54 next=0x01
        at <cs:ip> ax=0x0002 bx=0x0001 dx=<blk>
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
STAGE2: BOP FALL-THROUGH -> INT21: bop=0x54 next=0x01 at <cs:ip> ax=0x0002
```

---

## 2. Our allocations

Read off the code, `file:line` each, **re-cited 2026-10-01** (the previous citations were
from before `main.c` grew by ~6,000 lines). Status is about *our* implementation. Every
BOP we plant lives at `DOS_HDLR_SEG` or `DOS_CTAB_SEG`; anything executed elsewhere is the
guest's (`g_bop_from_guest`, `main.c:560`, set at `:28964`).

| code | name | purpose | status | planted / handled |
|---|---|---|---|---|
| `0x08` | — | INT 08h timer tick (`BOP; INT 1Ch; IRET`) | IMPL | `main.c:25783` / `:29023-29035` |
| `0x09` | — | INT 09h BIOS keyboard | IMPL | `:25790` / `:29005-29020` |
| `0x10` | — | INT 10h video | IMPL | `:25777` / `:28969-28979` |
| `0x16` | — | INT 16h keyboard | IMPL | `:25778` / `:28980-28995` |
| `0x1A` | — | INT 1Ah BIOS time | IMPL | `:25791` / `:29384-29393` |
| `0x20` | — | **INT 21h DOS** | IMPL | `:25776`, `:26978` / `dos_int21` (the exec loop's last arm, `:32208`) |
| `0x2F` | — | INT 2Fh multiplex | IMPL | `:25792` / `:29394-29530` |
| `0x33` | — | INT 33h mouse | IMPL | `:25779` / `:28996-29000` |
| `0x35` | `MS_CB_BOP` | mouse callback return | IMPL | `:7860` / `:29001-29004` |
| `0x43` | — | XMS far-call entry (`RETF`) | IMPL | `:25795` / `:29531-29559` |
| `0x50` | `DPMI_BOP` | DPMI real→PM switch | IMPL | `:550` / `:29573-29574`, gated on origin ⚠ **collides** |
| `0x54` | `DPMI_RMRET_BOP` | DPMI 0300/0301 return catcher | IMPL | `:557`, `:27054` / `:20027`, `:23731` ⚠ **collides** |
| `0x55` | `DPMI_CB_BOP` | DPMI 0303 real-mode callback entry | IMPL | `:598`, `:27059` / `:20033`, `:23740` |
| `0x56` | `DPMI_PMRET_BOP` | DPMI PM-return catcher | IMPL | `:601`, `:27062` |
| `0x57` | `DPMI_FAULT_BOP` | PM fault trampoline | IMPL | `:684`, `:17647`, `:17687` ⚠ **shared** |
| `0x57` | `WOWCALL_BOP_CODE` | WOW 16-bit call return | IMPL | `wowcall.h:89`, `main.c:16823` ⚠ **shared** |
| `0x58` | `DPMI_RAW2PM_BOP` | DPMI 0306 real→PM | IMPL | `:614`, `:27069` |
| `0x59` | `DPMI_RAW2RM_BOP` | DPMI 0306 PM→real | IMPL | `:616`, `:27071` |
| `0x5A` | `DPMI_FLTRET_BOP` | fault-handler return | IMPL | `:699`, `:17692` |
| `0x67` | — | INT 67h EMM | IMPL | `:25796` / `:29561-29568` |
| `0x11`–`0x17`, `0x25`–`0x29`, `0x30` (INT 20h) | — | BIOS and DOS-adjacent services | IMPL/PART | table `:25806-25811` / arm `:29037-29383` — marked per unit in [bios-misc.md](bios-misc.md) and [dos-services.md](dos-services.md) |
| *else* | — | refused + logged, exit `0xBD` | ✅ gated | `:32208-32214` |

### 2b. The guest's BOPs we answer: `0x54` and `0x50`

Arm: `main.c:31610-32206`, entered only when `g_bop_from_guest` (`:31610-31612`). An
unhandled sub-function is answered with `CF` from `cfg\bop54.txt` (default **`CF=0`**,
`:31617-31633`) and skipped (`EIP += 4`).

| BOP / sub | Meaning, as far as it is established | Status | Where |
|---|---|---|---|
| `54/00` | end the VDM (`EXIT`) — `VDDTerminateVDM` in stock | **IMPL** | `:32187-32192` |
| `54/01` | get the next command (line, cookie, the routed program) | **IMPL** | `:31738-32019` |
| `54/02` | shell state query (`AL` output) | **MISS** | default arm; meaning not established |
| `54/03` | — | **N/A** | unimplemented in stock too |
| `54/04`, `05`, `07` | used by `ntdos.sys` only | **N/A** | our DOS replaces `ntdos.sys`; never issued here |
| `54/06` | (`CF` output; `BX/AX` = the two words of sub 01's `+0x12` dword) | **MISS** | default arm |
| `54/08`, `0A` | stack-frame calls (`CF` output) | **MISS** | default arm |
| `54/09`, `0C` | used by `ntio.sys` (and `09` once by the shell) | **MISS** | default arm |
| `54/0B` | (`CF` output) | **MISS** | default arm |
| `54/0D` | the startup batch file (AUTOEXEC) | **IMPL** | `:32112-32138`; `cfg\autoexec.txt` |
| `54/0E` | keyboard / code-page configuration | **IMPL** | `:32193-32199`; `DX=0`, no KEYB |
| `54/0F` | the environment for a `/P` shell | **IMPL** | `:32020-32111` |
| `54/10` | one unnamed global (`AL` output) | **PART** | `:32152-32161`: `AL=0`, recorded as a reading, not a decode |
| `50/3D` | the shell's version-refusal path | **MISS** | default arm; only reached by a wrong DOS version |

## 3. What XP's `COMMAND.COM` actually issues

**Measured, not remembered:** every `C4 C4` in XP SP3's `C:\WINDOWS\SYSTEM32\COMMAND.COM`
(50,620 bytes), located by byte scan; the register usage of each was studied for
interoperability. **16 sites: 15 × `0x54`, 1 × `0x50`.**

| BOP / sub | sites | inputs | outputs |
|---|---|---|---|
| `54/00` | 1 | — | — (ends the VDM) |
| `54/01` | 1 | `DS:DX` → the request block (§0) | `AX`, `CF`, the block's OUT fields |
| `54/02` | 1 | `DX=0x0258` | **`AL`** |
| `54/06` | 1 | `BX`/`AX` = the two words of sub 01's `+0x12` dword | **`CF`** |
| `54/08` | 2 | a six-word `0xFFFF` frame on the stack, `BP=SP`, `AH`; `ES` → a current-directory structure | **`CF`**; the frame |
| `54/09` | 1 | — | — |
| `54/0A` | 1 | a six-word `0xFFFF` frame, `BP=SP`, `ES`; preceded by `AH=19h` | `AX`, **`CF`**; the frame |
| `54/0B` | 2 | `CX`/`BX` = the two words of sub 01's `+0x12` dword | **`CF`** |
| `54/0D` | 1 | `DS:DX` → a buffer | the path written there (then opened with `AX=3D00h`) |
| `54/0E` | 1 | `DS:SI` → a buffer, `CX=0x0539` | **`DX`** |
| `54/0F` | 2 | `BX` (0, or a block size after `AH=48h`/`49h`) | **`BX`** |
| `54/10` | 1 | — | **`AL`** |
| `50/3D` | 1 | `ES`=PSP, after a failed version check | — (followed immediately by `INT 20h`) |

### What can already be said from this

1. **`BOP 0x54` takes a one-byte sub-function immediately after the code.** Twelve
   distinct sub-functions: `00 01 02 06 08 09 0A 0B 0D 0E 0F 10`.
2. **Some sub-functions are stack-based.** `08` and `0A` take a six-word `0xFFFF` frame
   with `BP=SP` — a frame the host writes into — and return a carry flag the host sets.
3. **`AH=19h` (get current drive) is issued immediately before several of them** (`08`,
   `0A`, `0B`), which looks like a deliberate state sync rather than a coincidence.
4. **The `+0x12` dword is passed back repeatedly** in `CX`/`BX` or `BX`/`AX`.
5. ★ **`BOP 0x54 / sub 01` is the one that killed us** (§1): the live trace's `DX` is the
   request block.

⚠ What each sub-function **means** is recorded in §0 only where it has been established
by behaviour or by stock's answer; the rest stay blank on purpose.

### And one thing that IS established: the DOS version check

**XP's COMMAND.COM refuses to run unless `INT 21h AH=30h` returns exactly `AX=0x0005`
(5.00), testing the whole word** — `AL`=major, `AH`=minor — so it demands **exactly
5.00**, not "5 or later"; anything else prints *"Incorrect DOS version"*.
`cfg\dosver.txt` = `5.0` satisfies it; anything else does not. That matches the measured
0-calls-vs-9-calls result in [xp-command-com.md](../research/xp-command-com.md) and pins
*why*.

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

Tracked in GitHub: [#170](https://github.com/MrMatthewLayton/ntvdmex/issues/170) (the list that was here was moved there verbatim, 2026-09-27).

