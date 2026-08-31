# Session 37 — GDI.EXE was never rejected; we could not open it

**Date:** 2026-08-31 · **Branch:** `m9/completeness` · **Issue:** [#128](https://github.com/MrMatthewLayton/ntvdmex/issues/128)

**In one paragraph.** All eight 16-bit system modules load now, and krnl386 runs on past the
boot module list into whatever comes next. The `GDI.EXE` wall reported `0x0B`
ERROR_BAD_FORMAT and the cause was a **file-sharing violation**: `AL` in a DOS open is a
*bit field*, not a number — bits 0-2 access, bits 4-6 the SHARE.EXE sharing mode, bit 7
no-inherit — and the protected-mode arm compared the whole byte against 0 and 1, so
krnl386's `al = 0x80` (read access with the inheritance bit, the most ordinary open there
is) fell through to "anything else" and asked Windows for `GENERIC_READ | GENERIC_WRITE`.
USER.EXE imports GDI, so `GDI.EXE` was **already open and never closed** when the boot list
loaded it by name, and a second open asking for WRITE against a `FILE_SHARE_READ` handle is
`ERROR_SHARING_VIOLATION`. GDI was the only module ever open twice at once, which is why it
alone failed. Two instruments had to be fixed before either run could be read at all, and
one of them silently ate six of eleven breakpoints. The frontier is now **an `OpenFile`
whose `OFSTRUCT` pointer is outside its own segment**, reached after krnl386 finishes the
module list and starts enumerating drives.

---

## ★★★ The wall: `0x0B` was a failed OPEN, not a rejected header

### The function has exactly two ways to say `0x0B`

`seg2:0x218a` is "open + read + validate the NE header, then allocate the module database
and fill it". Over its whole `0x570` bytes it has **two** sites that produce `0x0B`, and
nothing else:

```
2242  mov ax,0x0b     <- VALIDATION: short 0x40 read | no MZ | e_lfanew==0 |
                         LSEEK failed | short NE read | no "NE" signature
22cc  mov ax,0x0b     <- GlobalAlloc returned 0 | the table read came up short |
                         ne_flags & 0x2000 (link errors) | ne_ver < 4
```

The last two of `0x22cc`'s four are excluded **by the file**: `gdi.exe` has `flags=0x8309`
and `ver=5.60`. So the whole question is which of two instructions executes, and one
breakpoint on each answers it. The full sweep, one run:

```
# addr   mode   what it reads
cca4     2      seg1 anchor: DI names the module being loaded
0642     22     the caller's stage-2 result in AX
21d2     22     AX = bytes of the first 0x40 read (CX=0x40)
221c     22     after LSEEK to e_lfanew: DX:AX = position, CF
222d     22     AX = bytes of the NE-header read (CX=0x40)
2242     22     ---- 0x0B route A
227d     22     AX = GlobalAlloc(0x42, DI); DI = the size
22c1     22     AX = the AH=3Fh result
22c8     22     BX = bytes read vs CX = requested
22cc     22     ---- 0x0B route B
22e1     22     SUCCESS: past both
```

**Predicted before the run, and wrong:** that `0x22cc` would fire on a short read, because
GDI's table read (`0x953` bytes) is the largest in the set and USER.EXE's `0x75a` was the
previous maximum. Wrong twice over — the read succeeds, and the `pm_int21_xfer` clamp that
would have caused it is `0x4000`, checked in the source before the run rather than after.

### What actually fired

`0x22cc` never fired at all. `0x2242` fired **once**, and `0x21d2` reported

```
21d2 read1  AX=0x0006  BX=0x0002  CX=0x0040
   @ds:si=00 00 0e 00 07 55 cf 01 94 00 50 00 00 00 3f 01
```

`AX = 6` is not "six bytes"; it is **ERROR_INVALID_HANDLE**, from file handle **2**. And
handle 2 was never a handle: the open had failed, and `2` is the DOS code the host returns
for *every* open failure. The log line said

```
INT21h AH=0000003d open "C:\WINDOWS\SYSTEM32\GDI.EXE" -> AX=0x00000002
```

which means two opposite things — error 2, or handle 2 — and the whole wall had been read
the wrong way round off that one line.

### The cause, measured

With `GetLastError` and `AL` added to the trace:

```
open "C:\WINDOWS\SYSTEM32\GDI.EXE" al=0x80 -> AX=0x00000007 CF=0    <- USER.EXE's import
...                                                                  (never closed)
open "C:\WINDOWS\SYSTEM32\GDI.EXE" al=0x80 -> AX=0x00000002 CF=1 FAILED gle=0x20
```

`gle = 0x20 = ERROR_SHARING_VIOLATION`, and the trace names the holder. `al = 0x80` is
DOS open mode: access `0` (read) with bit 7 (no-inherit) set. The host read the byte as a
whole number:

```c
DWORD acc = ((ax & 0xFF) == 0) ? GENERIC_READ
          : ((ax & 0xFF) == 1) ? GENERIC_WRITE : (GENERIC_READ | GENERIC_WRITE);
```

so `0x80` asked for write access on a file already held with `FILE_SHARE_READ`.

### Three fixes, all of them ours

- **Access comes from `AL & 7`**, on the protected-mode arm and the V86 arm alike.
- **We do not emulate SHARE.EXE, so we must not enforce it.** Both arms now pass
  `FILE_SHARE_READ | FILE_SHARE_WRITE`. Bare DOS locks nothing; `FILE_SHARE_READ` invented
  a restriction the guest's DOS does not have, and the V86 arm had the same latent defect
  waiting for any guest that opens one file twice.
- **A failed open no longer answers `2` to everything.** Sharing, access, path and
  handle-exhaustion failures get their own DOS codes. Answering "file not found" to a
  sharing violation is the *runs but lies* class: the caller retries a path that is right
  and concludes the file is missing.

**Result:** `GDI.EXE` loads (`AX = 0x0386`), and with it every module in krnl386's boot
list — `SYSTEM.DRV`, `KEYBOARD.DRV`, `MOUSE.DRV`, `VGA.DRV`, `SOUND.DRV`, `COMM.DRV`,
`USER.EXE`, `GDI.EXE`. `WIFEMAN.DLL` and `WINNLS.DLL` are correctly skipped on an English
locale. WOW32 calls in a run: **179 → 237**.

---

## ⚠⚠ Two instruments that lied, and one of them wasted the first run

### `dpmi_bp_load()` read 1023 bytes and said nothing about the rest

The first run of this session armed **five** breakpoints out of eleven. Not because of
`DPMI_BP_MAX` (32), and not because any site was refused — the parser read
`char buf[1024]` and the header comment explaining which two `0x0B` sites the list was
bisecting came to ~740 bytes, so the first five data lines fitted and the six that mattered
were dropped **in silence**. The log showed five confident arms and 42 hits and answered
nothing.

This is the same shape as session 36's one-shot that fired 512 times and retired before the
pass it existed to observe: *an instrument that discards its input without a word.* Now
8 KB, and it prints what it loaded:

```
DPMI-BP: pmbp.txt read 0x000006cf bytes, loaded 0000000b entries (total 0000000b)
```

with a loud line if the file was truncated at the buffer or `DPMI_BP_MAX` was reached.
`pmbp.txt` also now puts its **data lines first** and every comment below them, so the
layout itself cannot repeat the failure.

### `AH=3D open ... -> AX=0x0002` and a silent `AH=3E`

The open trace printed AX and nothing else, so a failed open and a successful one that
returned handle 2 were the same line. It now prints `AL` (which carries both the access
mode and the DOS sharing mode), `CF`, and on failure the Win32 error — which is the thing
that named this bug. `AH=3E close` logged nothing at all, so "who still holds that file"
was unanswerable from a trace that recorded every open and no close; that was the other
half of the evidence.

---

## ★ WOW32 `0x88 GetDriveType` — a real gap, and not the wall

After the module list, krnl386 called `0x88` **26 times in a row** with `nDrive`
`0x00..0x19` — A: through Z: — and every one was stepped over. Per session 35 that is not
"did not happen": the thunk's `sub sp,4` hole hands back stack litter and krnl386 branches
on it.

Its **one** caller pins the semantics to the byte:

```
1ea7  push dx / push di / push cs / call 0xb4b5   ; WOW32 0x88, 2 arg bytes
1eae  pop dx
1eaf  cmp al,2                                    ; DRIVE_REMOVABLE
```

`2` is Win32's `DRIVE_REMOVABLE`, so this is a straight pass-through rather than a
WOW-private encoding, and the host's drives *are* the guest's drives because our DOS layer
opens real paths on the real filesystem. Implemented against `GetDriveTypeA("X:\")`.

The guest's own behaviour is the receipt: it now asks about **three** drives instead of
twenty-six, and gets `A: = 2` removable, `B: = 1` no-root-dir, `C: = 3` fixed.

**It did not move the wall.** The run ends in the same place. Recorded as a closed gap, not
as progress.

---


---

## ★★★ WOW32 `0x80` IS `GetPrivateProfileString`, AND IT IS THE PROGRAM LAUNCH

Neither `0x80` nor `0x39` is self-named by krnl386's export table, so both were read off
their call sites — and the arguments point into DGROUP, which names them outright.
`seg1:0xcc08`:

```
cbee  mov ax,0x16b2
cbf1  push ds / push 0x158e      ; ds:0x158e = "BOOT"           lpAppName
cbf5  push ds / push ax          ; ds:0x16b2 = "WOWSHELL"       lpKeyName
cbf7  push ds / push [0x1492]    ; measured ds:0x16a6 =
                                 ;   "WOWEXEC.EXE"              lpDefault
cbfc  push ds / push 0x15f1      ; an empty 0x50-byte buffer    lpReturnedString
cc00  push 0x50                  ;                              nSize
cc02  push ds / push 0x1593      ; ds:0x1593 = "SYSTEM.INI"     lpFileName
cc08  call 0xb544                ; 22 arg bytes = 4+4+4+4+2+4   ✓
```

and thirty bytes later krnl386 does `mov di,0x15f1` and `lcall`s the LoadModule thunk at
`seg2:0x190a`, then `cmp ax,0x20 / jbe` — Win16's "failed to launch". **So this call is
krnl386 asking what Win16 program to run, and answering it is the launch itself.**

The second site, `seg1:0xca6d`, is the same signature over `[DEBUG] OUTPUTTO` with an empty
default, and it *does* test the result: `or ax,ax / je`, then `cmp ax,0x4e` — and `0x4e` is
`nSize - 2`, which is `GetPrivateProfileString`'s own "the answer did not fit" convention.
Two independent sites agreeing on six arguments and a return convention is a reading, not a
guess.

`0x39` is `GetProfileInt`: 10 arg bytes = 4 + 4 + 2, no filename, and its two sites read
`("KERNEL", "GPCONTINUE")` and `("ModuleCompatibility", <the module's own name>)` — one per
module just loaded, which is exactly what that section is for. **Which file it means is
unproven** and is recorded as such: this rig has neither section in `SYSTEM.INI` or
`WIN.INI` (fetched off the box and read), so both readings answer the caller's default.

⚠ The rig's `SYSTEM.INI` has **no `[boot]` section at all**, so the default wins —
`WOWEXEC.EXE`, which is present (10,368 bytes). Adding a `[boot] WOWSHELL=` line to the file
by hand changes nothing, because XP maps `SYSTEM.INI` reads through
`HKLM\...\CurrentVersion\IniFileMapping` into the registry. That is also what real WOW sees,
so the host's `GetPrivateProfileStringA` is right for the right reason. (The rig's file was
backed up and restored.)

**Result:** krnl386 completes its whole bootstrap and asks for the program **by name**:

```
"Please re-install the following module to your system32 directory:\r\n\t\tWOWEXEC.EXE"
```

The run no longer `#GP`s — it ends in `MessageBox` + `ExitKernelThunk(1)`, a deliberate,
traceable exit.

---

## ★ Two DOS-side gaps closed on the way

- **`PATH` is not decoration.** krnl386's search at `seg1:0x1dad` walks `PATH=` out of the
  environment block *we* build, and the module it looks for that way is the Win16 program,
  which arrives as a bare filename. `PATH=C:\` could never find it. The WOW path now passes
  the host's real `GetSystemDirectoryA` + `GetWindowsDirectoryA`; `dos_env_build` keeps the
  old value so a DOS guest is byte-identical.
  ⚠ And the block is now **bounded**, which it never was. It is `0x10` paragraphs, and
  linear `0x714` — a few bytes past its end — is the NT kernel's VDM interrupt-state dword
  that `dos_layout.h` records as breaking every guest. Unbounded was survivable while every
  string was a literal; it stopped being so the moment PATH and the program path became
  host-supplied.
- **`INT 21h AH=47h` now answers for any drive.** It answered only for the drive we happened
  to be on and returned "invalid drive" for every other, with a note saying so. Real DOS
  keeps a current directory per drive and so does Win32 — `GetFullPathNameA("X:")` reads it —
  so it now answers for the drive the caller named, and keeps the honest refusal (checked
  against `GetLogicalDrives`) for one that is not there.

---

---

## ★★★ AND THEN WOWEXEC.EXE RAN

Three more walls fell after the profile calls, and the last of them put a Win16 program
on the CPU.

### The drive-classification trio, and a whitelist whose comment was wrong

The remaining failure was traced link by link from `LoadModule("WOWEXEC.EXE") -> 2` down to
a single byte: `0x0e` in krnl386's per-drive flag table at DGROUP `0x2a2`, drive C:'s slot.
A flagged drive routes every later `AH=47h` through a pre-handler (`seg1:0x0728`) that ends
in `or byte [bp+6],1` — it **forces CF on every path through it** — so the path
canonicaliser `seg1:0x1f55` returned 0, `OpenFile` failed, and `LoadModule` reported "file
not found" *without ever touching the disk*.

krnl386 reads that table from two places in its own code and writes it from none, so a new
instrument was needed — see `pmchg.txt` below. It named the write in one run, and the
answer was ours:

```
if (ah == 0x44) {
    DWORD al = ax & 0xFF;
    if (al != 0x00 && al != 0x06 && al != 0x07) goto pm_int21_unhandled;
}
```

on the stated grounds that *"everything else takes a DS:DX buffer"*. True of most of
`AH=44h` and **false of exactly three**:

| | |
|---|---|
| `AL=08h` | is this block device removable |
| `AL=09h` | is it remote |
| `AL=0Eh` | get the logical drive map |

all register-only, and all three are how krnl386 classifies drives — it probes every drive
with all three in a loop. The V86 side had them too, in an `else { OKCF(); }` catch-all:
carry clear, meaning *success*, with the caller's own registers as the answer. The
*runs but lies* class. All three are answered from the host now. ⚠ Not yet checked against
the MS-DOS 6.22 oracle; the comment says so and names the fields to confirm.

**Result:** `open "C:\WINDOWS\SYSTEM32\WOWEXEC.EXE" -> AX=5 CF=0`. WOW32 calls in a run:
**237 → 2116**.

### A 1884-iteration loop, and a knob instead of a guess

That run ended in a `#SS` at `ss:sp = 0x1f:0x0002` — `push ebp` two bytes from the bottom
of the stack. `seg2:0x2a08` is a *retry* loop: allocate, ask **WOW32 `0x7d`** whether the
result is acceptable, and on `0` allocate another and ask again. It ran **1884 times**, and
`push ax` at `0x2a14` is not popped on the loop-back edge — two bytes per iteration, which
is the 4 KB stack exactly.

`0x7d` is one of the **53 ids krnl386's export table does not name**, so writing a `case`
for it would have been a guess. Instead: **`wow32ret.txt`**, one `<hex id> <hex dword>` per
line, changing the answer for one run, with every overridden call logged as
`** wow32ret.txt OVERRIDE -- an EXPERIMENT, not a service **`.

*Predicted before the run:* a non-zero `0x7d` makes the first call succeed at `seg2:0x29fe`,
the loop never runs, and the `#SS` disappears. *Measured:* `0x7d` called **once** instead of
1884, no `#SS`, and the run goes further than it ever has.

⚠ **The furthest point therefore depends on `7d 00000001` being in that file on the rig.**
It is an experiment, not an implementation, and the next session should pin the semantics
(the return is used only as a boolean on the first-call path at `seg1:0x29fe`, and as a
*value* on the retry path at `seg2:0x2a22` — which is why `1` is enough to test and not
enough to ship).

### The decoder was gated on one id, and the answer came through another

krnl386's own `MessageBox` is `0xc4`, and the string decoder was tied to it. The very next
message the guest tried to show came through **`0x140`, from a different module**, with the
same 7-word frame and the same `0x8008` style — so the run printed seven hex words where it
could have printed a sentence. Any argument that resolves through a selector and reads as a
NUL-terminated string is now decoded whoever passes it. That one change turned the run from
an address into this:

```
"Application Error"
"WOWEXEC caused a General Protection Fault in\r\nmodule KRNL386.EXE at 0001:229C.
 \r\n\r\nChoose close. WOWEXEC will close."
```

**That sentence is krnl386's, about the first Win16 program this project has executed.**
The same change also made visible, in one run, krnl386's full per-module search order and
its attempts to load `NETWORK.DRV` and `wfwnet.drv`.

---

## ★ `pmchg.txt` — who wrote this byte?

`pmbp.txt`'s dump column answers *"what is there when I stop here"*, which needs you to
already know where to stop. The question that cost this investigation is the other one, and
it had no instrument. `pmchg.txt` holds one line, `<hex offset> [segment]`, and logs the
first PM event at which that byte changed:

```
PMWATCH linear 0x0002b7a4 CHANGED 0x00 -> 0x0e -- first seen at cs:eip=0x1cf:0x2bf1 ax=0x0e02
```

`0x0e02` is `AH=0Eh / AL=2` — Select Disk C: — which put the whole drive-probe loop on the
screen. Sampling at PM events cannot name the instruction; it brackets the write between two
events that name themselves, which is a bisect's first step for one run instead of five.

⚠ It failed silently first, printing `watching` and never `armed`, because krnl386 **grows**
its DGROUP so the descriptor-limit match that fills `g_wow_pmbase[]` never fires for segment
4. Hence segment `0` = "the offset is already linear", taken by hand off a `dsbase=` line.

---

## ▶ RESUME HERE — session 37 handoff

**State: a Win16 program executes.** All eight system modules load, krnl386 completes its
bootstrap, reads `[boot] WOWSHELL` from `SYSTEM.INI`, finds and opens
`C:\WINDOWS\SYSTEM32\WOWEXEC.EXE`, loads it, and runs it — and then reports, in its own
words, that **WOWEXEC took a GP fault in KRNL386.EXE at `0001:229C`**. Nothing has drawn a
pixel yet.

⚠ **This depends on an EXPERIMENT.** `wow32ret.txt` on the rig must contain `7d 00000001`.
Without it krnl386 spins 1884 times in `seg2:0x2a08` and dies of a stack overflow. Pinning
`0x7d` properly is the first item below, not an optional tidy-up.

### The frontier, to the instruction

```
seg1:0x2290  push [bp+6]
seg1:0x2295  call 0x2200        ; -> AX = a module handle
seg1:0x2298  or ax,ax / je      ; non-zero, so it is a handle
seg1:0x229c  mov es,ax          ; ★ #GP -- the handle is NOT a valid selector
seg1:0x229e  mov ax,es:[0x3e]   ; and what it wants is an NE module database:
seg1:0x22a2  mov dx,es:[0x0c]   ;   +0x0c = ne_flags, and
seg1:0x22a7  and dx,0x2000      ;   0x2000 = the link-error bit
```

and it is now traced to the end. `AX = 0x0000FFFF` at the fault (error code `0xfffc`), and
`seg1:0x2200` — which is `GetExePtr` — **never returns `-1`**: its failure path is
`xor ax,ax` at `0x2281`. Breakpoints on both its `mov ax, es:[0x1e]` reads say where the
`0xFFFF` came from:

```
2233 ax=es:[1e]  AX(in)=0x03d6  ES=0x03b7   <- ordinary lookups, fine
2233 ax=es:[1e]  AX(in)=0x0000  ES=0x01ef   <- this one
2298 or ax,ax    AX=0xffff                  <- and this is what it produced
```

`[0x226]` in krnl386's DGROUP is the head of its **task list** — written at `seg1:0xc51c`
during bring-up and inserted into, sorted on `[+8]`, at `seg1:0x99ed`. Selector `0x01ef` has
limit `0x21f` (544 bytes) and holds `+0x02/+0x04 = 0x0fea/0x001f`, which is `SS:SP` on
krnl386's own stack — so it is **krnl386's own boot task database**, and the fields
`GetExePtr` uses are `+0x1c` (the instance handle it matches) and `+0x1e` (the module handle
it returns). That boot TDB's are **`0` and `0xFFFF`**.

⚠ *An earlier reading of this said `0x01ef` was the 64 KB selector our loader puts over the
staged file image (session 33 recorded stock using that number for exactly that). It is not
— the limit is 544 bytes and the contents are a task database. Same number, different thing,
and it was a whole wrong lead until the descriptor was actually read.*

### And the caller is the most ordinary call in Win16

The stack at the fault reaches back through USER into WOWEXEC, `0x03cf:0x081e`, and the
instruction before it is:

```
wowexec seg1:0x0812  push 0        \
        seg1:0x0814  push 0         > LoadCursor(hInstance = NULL, IDC_ARROW)
        seg1:0x0816  push 0x7f00   /
        seg1:0x0819  lcall <USER>
```

`0x7F00` is `IDC_ARROW`, and **`hInstance = NULL` is not a bug — it is the documented way to
ask for a system cursor.** USER passes it straight to `GetExpWinVer` (`seg1:0x228a`, which
reads `ne_expver` at `+0x3e` of the module database), krnl386's `GetExePtr(0)` walks the task
list, **matches its own boot TDB because that TDB's instance handle is `0`**, and hands back
`0xFFFF`, which USER loads into `ES`.

⇒ **The defect is that krnl386's boot task database has instance handle `0`.** On real
Windows nothing can match a NULL, `GetExePtr` returns `0`, and the caller's own
`or ax,ax / je 0x22ab` supplies a default. Ours matches, so the most ordinary call in Win16
takes the program down. ▸ Find what should have put krnl386's own instance (its DGROUP
selector, `0x01e7`) in `TDB+0x1c`, and why it is zero.

**Two `pmchg` runs and one experiment narrow that a long way:**
- `TDB+0x1e` (the `0xFFFF`) is written early, at the first PM events.
- `TDB+0x1c` (the instance handle) is **never written at all**, for the whole run.
- ⚠ The first of those was initially read as *"the block simply is not zeroed"* — wrong: a
  `pmchg` line says **CHANGED**, and a change is a **write**. The arena krnl386 carves from
  was zeroed to test it (it is now, on its own merits — this project has already paid for
  uninitialised memory that gets parsed) and **the field is still `0xFFFF`**. So krnl386
  puts it there on purpose for its own boot task, and the defect is the other field.

So krnl386 never sets `+0x1c`. A breakpoint at `seg1:0xc51c` — where the list head and
`[0x228]` (the current task) are both set to that selector during bring-up — dumps the block
as krnl386 registers it:

```
TDB base=0x2cc40, from +0x10:
  +0x10  00 00 00 00 00 00 00 00 00 00 00 04 00 00 ff ff
```

`+0x1a = 0x0400`, `+0x1c = 0`, `+0x1e = 0xFFFF`, everything else zero. So krnl386 writes
that record on purpose and registers it with a zero instance handle.

### ★★ AND STOCK ntvdm SETTLES IT: IT HAS NO SUCH TASK

`stockdump.bat` runs SYSEDIT under stock ntvdm with the IFEO key dropped and dumps the live
VDM from outside (`tools/vdmdump`). krnl386's **live** DGROUP is identifiable in that dump —
of the four copies of segment 4 in the low megabyte, exactly one has a non-zero task-list
head — and from there the list resolves through the dumped LDT:

```
stock live DGROUP @0x0162a0   head=0x1707  current=0x038f
  sel 0x1707 base 0x014360 | next=0x038f  SS:SP=0x16bf:0x245a  hInst=0x16be  hMod=0x171f
  sel 0x038f base 0x018ca0 | next=0x0000  SS:SP=0x03af:0x2066  hInst=0x03ae  hMod=0x037f
```

**Two tasks, and both have a real instance handle and a real module handle. There is no
zero-`hInstance` task in stock's list at all.** Ours has one — krnl386's own bring-up record,
still linked — and that is precisely what a `LoadCursor(NULL, …)` matches.

⇒ **Our run has a task in the list that a settled WOW session does not.**
⚠ **But that dump is the STEADY state, not the transient**, and the distinction matters: it
was taken with WOWEXEC *and* SYSEDIT running, long after boot. It does not say what the list
held at WOWEXEC's first instruction, which is the moment that matters.

### And krnl386 unlinks that record itself — we just never get there

`seg1:0x9a3e` is "remove from the list at `[0x226]`" (`mov es,[bp+4] / mov bx,0x226 /
mov dx,0 / call 0x8ab2`). It has three callers, and one of them is in the boot path:

```
cd2d  call 0xb544         ; WOW32 0x80 -- [boot] 386GRABBER from SYSTEM.INI
cd30  mov es,[0x228]      ; ★ the CURRENT task -- krnl386's own bring-up record
cd36  call 0x9a3e         ; ★ UNLINK IT
```

So krnl386 takes itself out of the task list, by design, shortly after loading the shell.
**We never reach it**, and the reason is exact:

```
seg1:0xcc66  lcall LoadModule("WOWEXEC.EXE")   svc=628   <- entered
seg1:0xcc6b  cmp ax,0x20                                 <- NEVER FIRES
seg1:0x229c  mov es,ax                          svc=970   AX=0x03a7  (fine)
seg1:0x229c  mov es,ax                          svc=979   AX=0xffff  (the #GP)
```

**Control never returns from `LoadModule`.** WOWEXEC's task is created *and runs* inside it —
342 services later it is executing its own code and faulting — so the boot task never gets
to `0xcd01`, never reads `[boot] 386GRABBER`, and never unlinks itself.

▸ **That forks the next step cleanly, and only one measurement decides it:**
1. If real `LoadModule` **returns before** the new task runs, then the ordering is ours to
   fix — we switch to the new task too early — and the unlink then happens on time.
2. If real `LoadModule` **also switches**, then the boot record's instance handle must be
   non-zero at that moment, and the question goes back to what should have filled
   `TDB+0x1c`.

The `0x229c` hit at svc 970 is worth carrying: `GetExpWinVer` is called twice, and the FIRST
call succeeds with a real handle. Only the `LoadCursor(NULL, …)` one fails.

### ★★ krnl386 hands WOWEXEC a CORRECT Win16 entry frame

A linear breakpoint on WOWEXEC's own entry point (`0x03b0d820 + 0x11b7`, its code selector's
base taken from the LDT-sync log) catches its first instruction:

```
cs:eip=0x03cf:0x11b7  after 689 svc
  DI=0x03d6  SI=0x0000  BX=0x2000  CX=0x0800
  DS=0x03d7  ES=0x03bf  BP=0  SS:SP=0x03d7:0x228a   stack= all zeros
```

That is the Win16 application entry convention exactly: **DI = hInstance, SI =
hPrevInstance (0, no previous instance), BX = stack size, CX = heap size, ES = the PSP, DS =
the automatic data segment**, on a clean stack with no return address — so krnl386 did not
*call* into it, it performed a proper task switch.

⇒ **Everything on the application side is right.** WOWEXEC knows its own instance handle;
its `LoadCursor(NULL, …)` passes `0` deliberately, as the interface says to. The loader, the
module, the task and the entry frame are all correct — the only thing wrong in the whole
picture is the stale bring-up task still sitting in the list.

★ It also dates the switch: `LoadModule` is entered at **svc 628** and WOWEXEC's first
instruction runs at **svc 689** — 61 services in, and `seg1:0xcd36` (the unlink) is hundreds
of services further on, after a return that never happens.

### The bring-up record IS a signed task database

The "is it a full-sized TDB?" check came back **yes, it is a real one**: `+0xFA` holds
`0x4454` — `"TD"`, the signature `seg2:0x2c02` writes. So the `0x100`-byte size difference is
not "it is a different kind of structure"; the likeliest reading is that a real task's block
carries a PSP the kernel's own does not need. That branch of the fork is closed, and the
defect stays where it was: `+0x1c` is `0`, and the record is still in the list when it
should not be.

### The fatal dialog is a symptom, and answering it proves that

krnl386's `MessageBox` (`0x140` here, from USER) is stepped over and answered `0`. Two runs
through the `wow32ret.txt` knob settle what that answer does, without claiming to know the
interface:

| answer | what happens |
|---|---|
| `0` (default) | the run stops at the dialog. No `EXC RETURN`, nothing after. |
| `1` | `EXC RETURN -> resume 0x1cf:0x229c` — krnl386 resumes **at the faulting instruction**, which faults again. Infinite loop; **268 MB of log** before the cap. |

So the answer chooses between "resume" and "do not", and neither gets past the fault. The
dialog is downstream of the defect, not a way around it. It is also a reminder that the knob
is a loaded weapon: one wrong value produced a quarter-gigabyte log.

### ★ And the bring-up record is the WRONG SIZE for a task

| | selector | limit |
|---|---|---|
| stock task 1 | `0x1707` | `0x31f` |
| stock task 2 | `0x038f` | `0x31f` |
| ours: WOWEXEC's task | `0x03b7` | `0x31f` |
| **ours: the bring-up record** | **`0x01ef`** | **`0x21f`** |

Every real task database in the panel is `0x320` bytes. The record that answers
`GetExePtr(NULL)` and kills `LoadCursor` is `0x100` bytes **shorter** — so it is either a
bootstrap structure a correct run never leaves in that list, or one krnl386 finishes later
and we interrupt. Either way, **"is the thing in the list a full-sized TDB?" is a one-line
check** that separates the two readings of the fork.

★ **And the oracle gives the value the field should hold.** In both of stock's tasks the
instance handle is the task's own stack selector with a different RPL:

```
sel 0x1707  SS=0x16bf  hInst=0x16be
sel 0x038f  SS=0x03af  hInst=0x03ae
```

i.e. `hInstance == SS` bar the low bits — which is Win16's `SS == DS` for a task, seen
through two aliases of one descriptor. Our boot record has `SS = 0x001f` and `hInst = 0`;
by that pattern it should be `0x001e`. That is a concrete expected value to aim a fix at, and
a concrete thing to check first if the fork above lands on branch 2.

⚠ Note also that the `WaitEvent` in this window is **WOWEXEC's own** (`from=0x03cf:0x11e9`),
not krnl386's scheduler, and the third `GetPrivateProfileString` is **USER's**
(`from=0x0327:0x582f`) — not the `seg1:0xcd2d` one. Both were tempting to misread as krnl386
getting further than it does; the `from=` field is what settles it.

⚠ **The oracle needed fixing before it could answer.** `vdmdump`'s LDT dump was
`LDT_ENTRY LdtEntries[512]`, which covers selectors `0x0000..0x0FFF` — and stock's task-list
head is `0x1707`, index 736. The first walk stopped at "NOT IN LDT" with nothing to say that
was the tool's limit rather than the guest's state. It is 8192 entries now, and the same run
went from 512 entries to 773.

### Next, in order

1. **Pin WOW32 `0x7d`** and replace the override with a service. Its one word is a handle
   from a just-completed allocation; `seg1:0x574d` (the other half of the loop) does
   `lsl ecx,[bp+6]`, so these are selectors and the question is about their limits. The
   return is a boolean on the first-call path and a value on the retry path.
2. **Why `LoadModule` never returns** — the fork above. krnl386 unlinks its own boot record
   at `seg1:0xcd36`, and it never gets there because WOWEXEC runs and faults inside
   `LoadModule`. The `#GP` at `seg1:0x229c` is only what that causes.
3. `NETWORK.DRV` / `wfwnet.drv` — krnl386 looks for both and neither is on the box's search
   path. Probably harmless; check before assuming.
4. ~~Oracle the `AH=44h` trio against MS-DOS 6.22 (#24).~~ **DONE** —
   `tools/dostest/p_ioctl.asm`, and the field that matters is not disputed: `4409h`'s remote
   bit reads `0` on MS-DOS 6.22, DOSBox-X and us alike. The three DISPUTED rows all have one
   cause — the 6.22 oracle boots from a floppy, so its default drive is A: — and the rationale
   is recorded in [`oracle-disagreements.md`](../../research/oracle-disagreements.md).

### Ruled out — do not re-try (all by measurement)

- **The `PATH` search, the `OFSTRUCT` pointer and the `#GP` at `seg1:0x1d69`.** Raised and
  killed in this same session: those breakpoints never fire and the fault is gone.
- **`DL = 0xF0` as the cause of anything.** It is a real gap and it is fixed, and it did not
  move the wall — `seg1:0x075e` forced CF regardless.
- **`0x88 GetDriveType` as the source of the drive flag**, and **a hand-edited `SYSTEM.INI`**
  (XP maps it into the registry, for us and for real WOW alike).
- **Growing krnl386's entry stack.** 4 KB → 32 KB, and the `#SS` was unchanged: krnl386 sets
  its own stack selector to a `0x0fff` limit with `INT 31h 0x0C`. The bigger entry stack is
  kept because we are the ones who choose it, but it fixed nothing.
- GDI.EXE's file, header, relocations, allocation and table read.

### The DOS side was re-validated on real hardware, not just off-VM

`dos_int21.c` and `dos_env.h` both changed this session, and *a fix measured on one guest is
a fix for none*. On the rig: `selftest.com` **8/8** (DOS memory, File I/O, XMS, EMS, PIT,
mouse, keyboard, video), and `dpmitest.com` for the other guest class. Against the oracle
panel, `p_dir`, `p_file`, `p_misc` and `p_err` all report **no disputes, no mismatches** —
including `int21.4700` (the current-directory call that now answers for any drive) and the
`AX=0002 / CF=1` open-failure codes that the sharing fix went through.

### How to drive it

```bash
PMBP=1 ARCHIVE=build/wowruns ./scripts/bmwow.sh      # deploy, run, collect
```
⚠ SMB writes to `/private/tmp/xpshare` need the sandbox disabled; remount with
`mount_smbfs -N //guest@192.168.1.29/ntvdmex /private/tmp/xpshare` after any drop.

The three files on the share that drive a run, none of which is in the repo:

| file | what it does |
|---|---|
| `pmbp.txt` | breakpoints: `<addr> [dump] [skip] [mode] [rep]`. Mode bit 1 = the address is a segment offset (bits 4..7 name it: `2` = seg 1, `0x22` = seg 2); **bit 2 (`4`) makes the dump column DS-relative** — mode `6`, `0x26`. `rep` 1 for anything hit twice. **Data lines first, comments below.** |
| `pmchg.txt` | `<hex offset> [segment]`, segment `4` = DGROUP, `0` = already linear. Logs the first PM event at which that byte changes. |
| `wow32ret.txt` | `<hex id> <hex dword>`, the answer for an unimplemented WOW32 id. **An experiment. Currently must contain `7d 00000001`.** |

⚠ Only use addresses seen as instruction boundaries in an **aligned** disassembly, and never
one shorter than two bytes — `dpmi_bp_arm` refuses those.
