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

## ▶ RESUME HERE — session 37 handoff

**State:** krnl386 loads **all eight** 16-bit system modules and runs past the boot module
list. 237 WOW32 calls in a run (was 179). No Win16 application has run yet, and `wowexec`
has not been reached.

**Where it stops:** a `#GP` reflected to krnl386's own handler, at **`seg1:0x1d69`**:

```
1d63  c47610      les si, ptr [bp+0x10]     ; ES:SI = the caller's OFSTRUCT
1d66  8b4604      mov ax, word ptr [bp+4]
1d69  26894402    mov word ptr es:[si+2], ax   <- #GP
```

with `ES = 0x001f{base=0x0002ce70 lim=0x0fff}` and `SI = 0x15f1`. `0x1f` is krnl386's
**stack** selector, and `0x15f1` is past its 4 KB limit — so the fault is an ordinary limit
violation on a pointer that was handed in, not a bad descriptor.

**What that function is.** `seg1:0x1d33`, `ret 0x10`, is the failure tail of Win16
**`OpenFile`**: it writes the error code to `OFSTRUCT+2` and copies the path to
`OFSTRUCT+8` via `seg1:0x1f06`. Its callers are `seg1:0x187a` and `seg1:0x1978`; the one on
this path is `0x1978`, whose argument build is exact —

```
1965  les si,[bp+8]        ; the OFSTRUCT the ENCLOSING function was given
1968  mov bx,0x0e00
196b  push es / push si    ; -> 0x1d33's [bp+0x10] far pointer
196d  push ds / push bx    ; -> [bp+0xc], a string at ds:0x0e00
196f  push ds / push [0xe80]
1974  push [bp+6] / push ax
1978  call 0x1d33
```

and the enclosing function (`retf 0xa` = `lpFileName`, `lpReOpenBuff`, `uStyle` — Win16
`OpenFile`'s exact signature) takes that pointer from **its own** `[bp+8]`. It reaches the
failure tail because `call 0x1c17` at `0x194a` returned CF=1, i.e. **the open failed first**.

**So there are two questions, and they are separate:**
1. Who calls `OpenFile` with an `lpReOpenBuff` of `SS:0x15f1`, and where did that offset
   come from? The stepped-over IDs immediately before it are `0xc8` (25 calls), `0x087`,
   `0x080` and `0x006` — and a stepped-over call answers at random.
2. **What file did it fail to open, and why?** The path is at `ds:0x0e00` in krnl386's
   DGROUP and the trace already logs every `AH=3Dh`. Answer this one first: it is free, and
   an `OpenFile` that succeeded would never reach the faulting instruction at all.

**Ruled out — do not re-try (all by measurement):**
- `GDI.EXE`'s file, its header, its relocations, its allocation and its table read. All
  measured good; the module loads.
- `pm_int21_xfer`'s clamp as a cause of any short read — the cap is `0x4000` and the
  largest read in the set is `0x953`.
- WOW32 `0x88` as the source of the `0xf0` drive number that reaches
  `GetCurrentDirectory` just before the fault. It is implemented and truthful now, and the
  `0xf0` is unchanged.
- The `0xf0` being our argument extraction: nine of eleven `0xc9` calls in the same run
  read `drive = 3` and return `"Documents and Settings\Matthew"` correctly.

**How to drive it:**
```bash
PMBP=1 ARCHIVE=build/wowruns ./scripts/bmwow.sh      # deploy, run, collect
```
⚠ SMB writes to `/private/tmp/xpshare` need the sandbox disabled; remount with
`mount_smbfs -N //guest@192.168.1.29/ntvdmex /private/tmp/xpshare` after any drop.
⚠ `pmbp.txt` columns are `<addr> [dump] [skip] [mode] [rep]`; mode bit 1 = the address is
an offset into a krnl386 segment, bits 4..7 naming it (`2` = seg 1, `0x22` = seg 2), and
`rep` 1 for anything reached more than once. **Data lines first, comments below.**
⚠ Only use addresses seen as instruction boundaries in an **aligned** disassembly.
