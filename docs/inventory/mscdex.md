# Inventory — MSCDEX (CD-ROM extensions: INT 2Fh `15xxh`) and CD audio

**Spec:** Microsoft *MS-DOS CD-ROM Extensions* specification (the INT 2Fh `1500h`–`1510h`
interface and the device-driver request headers it forwards); Ralf Brown's Interrupt List
(INT 2Fh `15xxh`). ⚠ **Not held in the repo** — [`../ref/SOURCES.md`](../ref/SOURCES.md).
**Our implementation:** **none.** INT 2Fh passes every `15xxh` call through with the
caller's registers (`main.c:29226-29362`; nothing matches `AX=15xxh`). There is no CD-ROM
device driver, no ATAPI device ([ide.md](ide.md)), and no Red Book audio path.
**What does work:** a host CD-ROM drive is an ordinary DOS drive letter. INT 21h file I/O
goes through Win32 (`v86_path` → `CreateFileA` …), so a program that simply opens
`D:\DATA\FILE.DAT` reads the disc. IOCTL `4408h` reports it removable
(`dos_int21.c:2203`).
**Marked:** 2026-10-01, **from the code**.

---

## Headline

**Data CDs are readable as files; nothing that *asks for* a CD-ROM finds one.** Every
game that checks "is MSCDEX loaded, and which drive is the CD?" before reading its data
calls `1500h` and reads the answer from **`BX`** (number of CD drives) and **`CX`** (first
drive). Because INT 2Fh passes the call through, the answer is whatever the caller had in
`BX`/`CX` — "none" only for a caller that zeroed `BX` first. And CD audio — which is how
ZAR and many period games carry their soundtrack (#191) — has no path at all.

Two smaller shapes follow from the same absence:

- IOCTL `4409h` (is the drive remote?) answers `0` for a CD drive (`dos_int21.c:2204-2207`,
  only `DRIVE_REMOTE` sets bit 12). Under real MSCDEX a CD drive *is* a redirected drive, and
  some programs find the CD by that bit.
- `5F02h` (redirection list) refuses (`dos_int21.c:1556-1558`), so a CD drive cannot be
  found by enumerating redirections either.

| Group | Units | IMPL | PART | STORE | MISS | N/A |
|---|---|---|---|---|---|---|
| §1 INT 2Fh `15xxh` | 15 | — | — | — | 13 | 2 |
| §2 Device requests a CD driver answers (via `1510h`) | 8 | — | — | — | 8 | — |
| §3 The DOS side | 3 | 1 | 1 | — | 1 | — |
| **Total** | **26** | **1** | **1** | **—** | **22** | **2** |

---

## 1. INT 2Fh `15xxh`

All reach the INT 2Fh arm's pass-through: no register is written.

| AX | Function | Status | Notes |
|---|---|---|---|
| `1500h` | installation check: `BX` = CD drive count, `CX` = first drive | **MISS** | ⛔ the answer is the caller's own `BX`/`CX` |
| `1501h` | drive device list (`ES:BX`) | **MISS** | |
| `1502h` | copyright file name | **MISS** | |
| `1503h` | abstract file name | **MISS** | |
| `1504h` | bibliographic file name | **MISS** | |
| `1505h` | read volume table of contents | **MISS** | |
| `1506h`/`1507h` | debugging on / off | **N/A** | MSCDEX's own debug build only |
| `1508h` | absolute disk read (sectors) | **MISS** | the call games use to read raw CD sectors |
| `1509h` | absolute disk write | **N/A** | CD-ROM is read-only; the spec reserves it |
| `150Ah` | reserved | — | not counted |
| `150Bh` | CD-ROM drive check (`BX=ADADh` signature) | **MISS** | |
| `150Ch` | MSCDEX version | **MISS** | |
| `150Dh` | drive letter list | **MISS** | |
| `150Eh` | get / set volume descriptor preference | **MISS** | |
| `150Fh` | get directory entry | **MISS** | |
| `1510h` | send device driver request | **MISS** | the door to every request in §2 |

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
| IOCTL `4408h` / `4409h` for a CD drive | **PART** | `4408h` removable ✓ (`dos_int21.c:2203`); `4409h` not remote (`:2204-2207`), where MSCDEX makes it so |
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
