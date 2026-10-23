# Inventory — MSCDEX (CD-ROM extensions: INT 2Fh `15xxh`) and CD audio

**Spec:** Microsoft *MS-DOS CD-ROM Extensions* specification (the INT 2Fh `1500h`–`1510h`
interface and the device-driver request headers it forwards); Ralf Brown's Interrupt List
(INT 2Fh `15xxh`). ⚠ **Not held in the repo** — [`../ref/SOURCES.md`](../ref/SOURCES.md).
**Our implementation (#241, 2026-10-23):** **an ISO image as the MSCDEX drive.** With
Settings > Drives > CD-ROM set to *Mount ISO Disk Image*, `src/host/host_cdrom.c` takes the
first free letter after the machine's last drive (MSCDEX's rule), maps it onto a cache
directory with `DefineDosDevice`, and answers INT 2Fh `15xxh` (the arm in
`src/host/host_bios.c`, search `CDROM_MULTIPLEX_ID`). The image is read by
`src/dos/dos_iso9660.c` (ECMA-119; tests/unit/iso9660_test.c). INT 21h reaches the files
because every path is handed to the host first (`DOS_MACHINE::PathPrepare`), which copies the
named file out of the image; write-class calls on the drive answer 5 (`dos_int21.c`, search
`THE CD-ROM IS READ-ONLY`). A blank path is an empty drive: reported, every read not ready.
⚠ **Image mode only.** With *Use The Physical CD-ROM Drive* (the default) INT 2Fh still
passes every `15xxh` call through, as before. There is no CD-ROM device driver, no ATAPI
device ([ide.md](ide.md)), and no Red Book audio path.
**What does work:** a host CD-ROM drive is an ordinary DOS drive letter. INT 21h file I/O
goes through Win32 (`v86_path` → `CreateFileA` …), so a program that simply opens
`D:\DATA\FILE.DAT` reads the disc. IOCTL `4408h` reports it removable
(`dos_int21.c:2216`).
**Marked:** 2026-10-01, **from the code**; §1 and §3 re-marked 2026-10-23 for #241 (image mode),
**from the code and the off-VM reader test** -- not yet run against a real program on the test machine.

---

## Headline

**Data CDs are readable as files; nothing that *asks for* a CD-ROM finds one.** Every
game that checks "is MSCDEX loaded, and which drive is the CD?" before reading its data
calls `1500h` and reads the answer from **`BX`** (number of CD drives) and **`CX`** (first
drive). Because INT 2Fh passes the call through, the answer is whatever the caller had in
`BX`/`CX` — "none" only for a caller that zeroed `BX` first. And CD audio — which is how
ZAR and many period games carry their soundtrack (#191) — has no path at all.

Two smaller shapes follow from the same absence:

- IOCTL `4409h` (is the drive remote?) answers `0` for a CD drive (`dos_int21.c:2217-2220`,
  only `DRIVE_REMOTE` sets bit 12). Under real MSCDEX a CD drive *is* a redirected drive, and
  some programs find the CD by that bit.
- `5F02h` (redirection list) refuses (`dos_int21.c:1556-1558`), so a CD drive cannot be
  found by enumerating redirections either.

| Group | Units | IMPL | PART | STORE | MISS | N/A |
|---|---|---|---|---|---|---|
| §1 INT 2Fh `15xxh` (image mode) | 15 | 10 | 2 | — | 1 | 2 |
| §2 Device requests a CD driver answers (via `1510h`) | 8 | — | — | — | 8 | — |
| §3 The DOS side | 4 | 2 | 1 | — | 1 | — |
| **Total** | **27** | **12** | **3** | **—** | **10** | **2** |

---

## 1. INT 2Fh `15xxh`

Image mode (#241): answered by `HostCdromInt2F` (`src/host/host_cdrom.c`). Values are Ralf
Brown's Interrupt List's; the MSCDEX spec itself is not held. Physical mode: all pass through.

| AX | Function | Status | Notes |
|---|---|---|---|
| `1500h` | installation check: `BX` = CD drive count, `CX` = first drive | **IMPL** | `BX=1`, `CX` = the image drive |
| `1501h` | drive device list (`ES:BX`) | **MISS** | |
| `1502h` | copyright file name | **IMPL** | PVD offset 702, 37 bytes + NUL |
| `1503h` | abstract file name | **IMPL** | PVD offset 739 |
| `1504h` | bibliographic file name | **IMPL** | PVD offset 776 |
| `1505h` | read volume table of contents | **IMPL** | `DX` = descriptor index from sector 16; `AX` = 1 primary, FFh terminator, 0 other |
| `1506h`/`1507h` | debugging on / off | **N/A** | MSCDEX's own debug build only |
| `1508h` | absolute disk read (sectors) | **IMPL** | `SI:DI` first sector, `DX` count, from the image |
| `1509h` | absolute disk write | **N/A** | CD-ROM is read-only; the spec reserves it |
| `150Ah` | reserved | — | not counted |
| `150Bh` | CD-ROM drive check (`BX=ADADh` signature) | **IMPL** | `AX` nonzero (FFFFh) for the image drive |
| `150Ch` | MSCDEX version | **IMPL** | `BX=0217h`, 2.23 (MS-DOS 6.22's) |
| `150Dh` | drive letter list | **IMPL** | |
| `150Eh` | get / set volume descriptor preference | **PART** | the primary descriptor only (`DX=0100h`); Joliet is not read |
| `150Fh` | get directory entry | **IMPL** | the record as recorded; `AX=1` (ISO 9660) |
| `1510h` | send device driver request | **PART** | no driver behind the drive: every request is answered status `8103h` (unknown command), never left looking like success |

## 2. Device requests a CD-ROM driver answers

These are what a program sends through `1510h` (or straight to the driver). With no
driver, all **MISS**.

| Request | Code | Status |
|---|---|---|
| IOCTL input (drive status, media changed, audio channel info, Q-channel, UPC, disc/track info) | `03h` | **MISS** |
| Input flush / IOCTL output (eject, lock door, reset, audio channel control) | `07h` / `0Ch` | **MISS** |
| Read Long / Read Long Prefetch | `80h` / `82h` | **MISS** |
| Seek | `83h` | **MISS** |
| **Play Audio** (Red Book) | `84h` | **MISS** |
| **Stop Audio** | `85h` | **MISS** |
| **Resume Audio** | `88h` | **MISS** |
| Device open / close | `0Dh` / `0Eh` | **MISS** |

## 3. The DOS side

| Unit | Status | Where / notes |
|---|---|---|
| Files on a host CD drive through INT 21h | **IMPL** | the ordinary file path; the drive letter is the host's |
| Files on the image drive through INT 21h | **IMPL** | #241: open/read/seek/find/chdir work through the cache directory; create, delete, rename, mkdir/rmdir, set attributes, and extended open for writing answer 5. A file's first open copies it out whole. The FCB calls do not prepare a path, so an FCB open of a file not yet copied out finds nothing |
| IOCTL `4408h` / `4409h` for a CD drive | **PART** | `4408h` removable ✓; `4409h` remote (bit 12) for the image drive (#241), not for a host CD drive |
| `5F02h` redirection list including the CD | **MISS** | `dos_int21.c:1556-1558` refuses |

---

## What to fix, in order

1. `1500h`, `150Bh`, `150Ch`, `150Dh` from the host's own CD drives
   (`GetDriveTypeA` = `DRIVE_CDROM`), so detection succeeds for the data CDs that already
   read. That alone serves "insert the CD" checks.
2. `1508h` (raw sector read) through the host's CD volume.
3. CD audio (`1510h` → `84h`/`85h`/`88h` and the Q-channel IOCTLs) — a host-side audio path
   to the CD, mixed like every other source. ZAR's music is this (#191, closed as "no music
   to find" — it is on the disc).
4. Decide `4409h` for CD drives with the above in place.
