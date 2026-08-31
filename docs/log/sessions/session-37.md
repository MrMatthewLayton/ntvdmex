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

## ▶ RESUME HERE — session 37 handoff

**State:** all eight 16-bit system modules load, krnl386 completes its bootstrap, reads
`[boot] WOWSHELL` out of `SYSTEM.INI`, and calls `LoadModule("WOWEXEC.EXE")`. That call
returns **2**, `ERROR_FILE_NOT_FOUND`, and krnl386 exits cleanly saying so. No Win16
application has run yet.

### ★★★ The causal chain is complete, and every link is measured

```
LoadModule("WOWEXEC.EXE")                                  -> 2, and says so
  seg2:0x0812   the boot-time load ([0x46c] != 0 skips its own OpenFile)
  seg1:0x2193 -> seg1:0x1812   OpenFile
  seg1:0x1a4c   resolve the name
  seg1:0x1f55   CANONICALISE -- returns 0 (measured at seg1:0x185e: AX=0 for
                WOWEXEC, AX=1 for all nine others)
  seg1:0x1fd5   because INT 21h AH=47h came back CF=1
  seg1:0x0728   because the AH=47h PRE-HANDLER ran, and it ends in
  seg1:0x075e   `or byte [bp+6],1` -- it FORCES CF into the caller's saved flags,
                unconditionally, on every path through it
  seg1:0x0728   which it reached because seg1:0x083c said "this drive is flagged":
  seg1:0x0848   `add bx,0x2a2` + `cmp byte [bx],0` -- a per-drive byte table in
                krnl386's DGROUP at 0x2a2, indexed by DL-1
```

### ▸ THE ONE QUESTION LEFT: who writes `0x0e` to krnl386's DGROUP `0x2a4`?

That byte is drive **C:**'s slot. Dumped through a whole run with
`pmbp.txt` mode `6` (address is a segment offset, dump column is DS-relative):

```
seg1:cca4  DI=1641..1693   table[0..7] = 00 00 00 00 00 00 00 00   <- all eight modules
seg1:0848  DI=15f1         table[0..7] = 00 00 0e 00 00 00 00 00   <- the WOWEXEC load
```

so it is **zero for the entire module boot and non-zero by the time WOWEXEC is loaded**, and
the file image of DGROUP is zero there too. A flagged drive can only ever fail, so on real
WOW C: is not flagged — **this byte is wrong, and the write happens in that window.**

- krnl386's own segments 1-3 contain exactly **two** references to `0x2a2`, `seg1:0x0848` and
  `seg1:0x51ae`, and **both are reads** (scanned by decoding every occurrence of the
  immediate). So the writer is not an ordinary `[0x2a2+n]` store in krnl386.
- ▸ **The strongest hypothesis: DOS's own Current Directory Structure.** `seg1:0x5343` reads
  the current-drive byte through `[0x275]`, which is one of the six far pointers krnl386
  builds from `SysVars+0x6A` — the table `dos_wow_publish()` plants (session 31). If the CDS
  or one of those six pointers is wrong, krnl386 will classify C: from garbage. Two of the
  six are pinned (LASTDRIVE, the current-drive byte); **four are still unknown**, and this is
  the first thing that has depended on them.
- ▸ Second: WOW32 `0xc8`, called **25 times** from `seg1:0x538a` — the `AH=0Eh` Select Disk
  arm — with drives `0x19..0x01` descending, all stepped over. Its answer is stored at
  `[0x2a0]`, two bytes below the table. Unnamed by the export table; pin it from that arm.

### Ruled out — do not re-try (all by measurement)

- **The `PATH` search, the `OFSTRUCT` pointer, and the `#GP` at `seg1:0x1d69`.** All were
  session-37 leads and all are gone: breakpoints at `seg1:0x1dad`, `0x1c40`, `0x194d` and
  `0x1965` never fire, the run no longer faults, and the failure is upstream of the file
  system entirely — **no `AH=3Dh` is ever attempted for WOWEXEC.EXE**.
- **`DL = 0xF0` as the cause.** It is a real gap and it is fixed (it is krnl386's sentinel,
  emitted by `seg1:0x0834`, `mov dl,0xF0`), and the inner call now succeeds — but
  `seg1:0x075e` forces CF anyway, so **it did not move the wall.** Said plainly.
- **WOW32 `0x88 GetDriveType` as the source of the flag.** The flag was already `0x0e` in
  runs where `0x88` was stepped over *and* in runs where it answers truthfully.
- **A bad `SYSTEM.INI`.** XP maps it into the registry; editing the file changes nothing,
  for us or for real WOW.
- **GDI.EXE's file, header, relocations, allocation and table read.** It loads.

### How to drive it

```bash
PMBP=1 ARCHIVE=build/wowruns ./scripts/bmwow.sh      # deploy, run, collect
```
⚠ SMB writes to `/private/tmp/xpshare` need the sandbox disabled; remount with
`mount_smbfs -N //guest@192.168.1.29/ntvdmex /private/tmp/xpshare` after any drop.
⚠ `pmbp.txt` columns are `<addr> [dump] [skip] [mode] [rep]`. Mode bit 1 = the address is a
segment offset (bits 4..7 name the segment: `2` = seg 1, `0x22` = seg 2); **bit 2 (`4`) makes
the dump column DS-relative**, which is what makes a DGROUP table readable without knowing
the run's base — mode `6` for seg 1, `0x26` for seg 2. `rep` 1 for anything reached more than
once. **Data lines first, comments below.**
⚠ Only use addresses seen as instruction boundaries in an **aligned** disassembly, and never
one shorter than two bytes — `dpmi_bp_arm` refuses those, silently as far as the run is
concerned.
