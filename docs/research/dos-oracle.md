# The MS-DOS 6.22 oracle

GH #25. The primary reference for the M9 completeness programme: a genuine
Microsoft MS-DOS 6.22 install under QEMU on the dev Mac, queryable from the
command line in about three seconds, fully offline, no rig involvement.

**The rule it exists to serve:** never write a test expectation from memory of
what DOS does. Expected values come from Ralf Brown's Interrupt List *confirmed
here*. This is not a theoretical hazard — see "It has already caught one of
mine" below.

## Using it

```sh
./scripts/oracle.sh tools/dostest/dosver.com        # run a guest binary
./scripts/oracle.sh --batch "VER"                   # run DOS commands
./scripts/oracle.sh --selftest                      # prove it still answers
./scripts/oracle.sh --interactive                   # a real prompt, for poking about
./scripts/oracle.sh probe.com --dir ./corpus        # plus a host dir, read-only, as B:
./scripts/oracle.sh probe.com --serial              # also show what reached COM1
```

From Python, which is how the differential harness (#26) should drive it:

```python
from dosoracle import Oracle
r = Oracle().run("tools/dostest/dosver.com")
r.stdout      # the program's standard output
r.serial      # anything it wrote to COM1
r.seconds     # wall clock
```

## Building it

```sh
python3 scripts/dosoracle/build.py          # ~75 s, writes vm/dos622.img (504 MB)
```

Media is four retail 1.44 MB floppy images in `./msdos-622` (three MS-DOS setup
disks plus the optional supplemental disk). Both the media and `vm/` are
gitignored — licensed, and large.

Verified genuine before use: every file carries the `1994-05-31 6:22` build
stamp, and Disk 1 carries `IO.SYS` / `MSDOS.SYS` / `COMMAND.COM`. Note the boot
sector's OEM field says `MSDOS5.0` on all four disks — that is what 6.22's own
FORMAT writes and is **not** a version indicator; don't use it as one.

### Why the install does not drive SETUP.EXE

SETUP is a full-screen interactive installer: it prompts for a target, prompts on
every disk swap, and rewrites `CONFIG.SYS`/`AUTOEXEC.BAT` to taste. Driving it
means synthesising keystrokes against screen state — the timing-fragile loop that
produces a slightly different disk every run.

It isn't needed. `PACKING.LST` on Disk 1 is **Microsoft's own** compressed-name →
expanded-name table for all three disks, so the install reduces to a
deterministic list of `EXPAND`/`COPY` commands. The installed files are the
genuine Microsoft binaries either way; we are choosing how to unpack them, not
what to install.

The mapping is not guessable and must not be derived from the underscore:

| On disk | Expands to | |
|---|---|---|
| `MONOUMB.38_` | `MONOUMB.386` | |
| `WNTOOLS.GR_` | `WNTOOLS.GRB` | not `.GRP` |
| `GORILLA.BA_` | `GORILLA.BAS` | but... |
| `DRVBOOT.BA_` | `DRVBOOT.BAT` | ...same extension, different target |

The supplemental disk ships no `PACKING.LST`; its rules come from that disk's own
`SETUP.BAT`, which calls `SD6COPY.BAT` with the expanded name of every file it
installs (`NET.1X_`→`NET.1XE`, `CGA.VI_`→`CGA.VID`, and so on). Anything without
an authoritative expanded name is **skipped and logged**, never silently dropped.

Build passes, each one non-interactive QEMU launch ending in `QUIT.COM`:

1. `FDISK /MBR`, `FORMAT C: /S /U`, install Disk 1 (control floppy in A:)
2–4. install Disks 2, 3 and the supplemental disk (source in B:)
5. write the oracle's own `CONFIG.SYS` / `AUTOEXEC.BAT`

The partition table is written on the host (one FAT16 primary, type 06, LBA 63,
CHS 1024/16/63 — inside the 1024-cylinder BIOS limit, so no LBA translation);
`FDISK /MBR` then installs Microsoft's real MBR boot code into the code area
non-interactively, preserving that table.

Result: 194 files in `C:\DOS`, expansion confirmed real (`MEM.EXE` 32,502 bytes
against a 19,512-byte `MEM.EX_`).

## How capture works

The harness builds a scratch floppy holding the program and a generated
`RUN.BAT`; C:'s `AUTOEXEC.BAT` calls `A:\RUN.BAT` if present. `RUN.BAT` redirects
the program's output to `A:\OUT.TXT`, mirrors it to `AUX`, then runs `QUIT.COM`,
which writes QEMU's `isa-debug-exit` port so the host process ends by itself with
status 85. The host reads `OUT.TXT` back out of the floppy image with mtools.

No keystroke synthesis, no screen scraping, no timing assumptions.

Two capture paths, deliberately:

- **stdout via the floppy** — needs no cooperation from the program, so
  *unmodified* binaries work (`MEM`, `DIR`, third-party tools).
- **serial (COM1)** — a probe that writes its register dump straight to `AUX`
  gets it out even if it later hangs the guest. On a timeout the harness reports
  whatever serial captured before the hang.

## Things that bit me, so they don't bite you

- **DOS parses `<` and `>` inside `ECHO`.** The first sentinels were `---8<---`
  and `--->8---`; DOS read the `>` as redirection and wrote a file called `8---`.
  Sentinels are now `[BEGIN]`/`[END]`.
- **`ECHO foo > file` writes `"foo "`** — the space before the redirect is part
  of the text. Match sentinels stripped, or as substrings.
- **`FIND` is case-sensitive and 6.22 prints `54 file(s)` lower-case.** See below.
- **vvfat is not usable here.** `fat:ro:` as a hard disk is refused outright
  ("Block node is read-only"); as `fat:floppy:ro:` it served the **wrong file
  contents** — `TYPE` of a 20-byte file printed fragments of vvfat's own volume
  structures — and then hung the guest. `--dir` therefore synthesises a throwaway
  raw FAT image with mtools instead. Ceiling 1.44 MB.
- **The dev sandbox refuses to bind a unix socket anywhere**, `TMPDIR` included,
  so the QEMU monitor runs on **stdio**, not a QMP socket.

### It has already caught one of mine

The oracle's own selftest asserted `DIR C:\DOS\*.EXE | FIND "File(s)"`. That
expectation was written from memory of what `DIR` prints. Real 6.22 prints
`file(s)` in lower case and `FIND` is case-sensitive, so the command matched
nothing — and the check still reported **PASS**, because it only asserted "some
output appeared" and a stray sentinel line satisfied that.

Two lessons, both the epic's: an expectation from memory is wrong more often than
it feels, and a weak assertion turns a silent failure into a confident green
tick. Selftest cases now carry a substring transcribed from the oracle's actual
output.

## Scope — what this oracle is and is not truth for

- **INT 21h: this is the standard.** It is the genuine Microsoft kernel.
- **INT 10h / 16h: it is not.** QEMU runs SeaBIOS, so a BIOS answer here is just
  another reimplementation's opinion regardless of which DOS sits on top. Real
  BIOS truth means the Dell OptiPlex booted off a DOS stick against its actual
  VGA BIOS — the rare tiebreaker for disputed BIOS cases, not the daily cycle.
- **Known gap:** screenshots are captured only on timeout, as a diagnostic. There
  is no capture-the-screen-on-success path, so this oracle cannot currently
  answer "what pixels did that produce". Given the caveat above, QEMU video would
  be weak evidence anyway; video questions want the real box.

## First findings

### INT 21h AH=30h / AX=3306h — version (feeds #28)

`tools/dostest/dosver.com`, run against this oracle:

```
INT21.30   major=06 minor=16 oem=FF serial=00:0000
INT21.3306 major=06 minor=16 rev=00 flags=10 cf=00
```

| | Oracle (6.22) | NTVDMEX today |
|---|---|---|
| `AH=30h` → `AX` | `0x1606` (AL=06 major, AH=0x16=22 minor) | `0x0005` — i.e. "5.0" |
| `AH=30h` → `BH` | `0xFF` (OEM: generic MS-DOS) | — |
| `AH=30h` → `BL:CX` | `00:0000` (24-bit serial, zero) | — |
| `AX=3306h` → `BL:BH` | `06:0x16`, `DL` rev 0, CF clear | — |

**`DH=0x10` is configuration-dependent, not a constant.** Bit 4 means "DOS is in
the HMA", and this oracle boots with `DOS=HIGH` in `CONFIG.SYS`. A test that
hardcodes `DH=0x10` is asserting a property of *this image's config*, not of
MS-DOS. Anything asserting on `DH` must control `DOS=HIGH` too.
