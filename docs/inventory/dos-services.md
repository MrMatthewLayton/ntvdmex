# Inventory — DOS services: INT 21h, the other DOS interrupts, INT 2Fh, INT 13h/25h/26h

**Spec:** Ralf Brown's Interrupt List (INT 20h–2Fh); *Undocumented DOS* (Schulman et al.)
for the internals (SysVars, SDA, the DPB); MS-DOS 6.22 itself as the oracle, because DOS's
behaviour is the contract. ⚠ **Not held in the repo** —
[`../ref/SOURCES.md`](../ref/SOURCES.md). **XMS/EMS/the HMA** are [xms-ems.md](xms-ems.md);
INT 2Fh `1687h` (DPMI) is the DPMI surface (no inventory yet).
**Our implementation:** `dos_int21` in `src/dos/dos_int21.c` (`:553-2468`), one
`if/else if` chain on AH; the handle table in `dos_fh.h`, MCBs in `dos_mcb.h`, errors in
`dos_err.h`. The other DOS interrupts and INT 13h/25h/26h are the V86 BIOS arm in
`src/host/main.c` (≈`:29037-29383`); INT 2Fh is ≈`:29394-29530`.
**Oracles:** MS-DOS 6.22 under QEMU (a **real DOS**, so an agreeing row is **oracle**),
PCem (real AMI BIOS + 6.22), DOSBox-X. For the BIOS-side INT 13h rows the QEMU answer is
SeaBIOS (**provisional**). Stock XP NTVDM is the only oracle for NTVDM-private calls.
**Probes:** 30-odd `tools/dostest/p_*.asm` — the AH → probe map is in the Verification
column. **Off-VM:** `err_test.c`, `fh_test.c`, `mcb_test.c`, `disk_test.c`, `sysvars_test.c`.
**Marked:** 2026-10-01, **from the code**. Carried over from `docs/PARITY.md` (retired
2026-09-23) and re-marked; this file now enumerates **every INT 21h function**, where the
PARITY version only recorded the probes.

⚠ **"oracle" means the probe's rows agree, not that the function is complete.** Several
rows compare only CF (`int21.5700.getdate`, `int21.34.indos`); see §6.

---

## Headline

**The file, directory, handle, memory and process calls a program actually lives on are
implemented and agree with real DOS.** The gaps fall into four kinds:

1. **Calls that report success and do nothing** — the dangerous shape. `2Bh`/`2Dh` (set
   date / time) answer `AL=0` with no effect, the exact thing the INT 1Ah arm refuses to
   do for the same reason; IOCTL `44h` answers CF=0 for every sub-function it does not
   know; `05h`/`04h` discard printer and AUX output although an LPT spool and two UARTs
   now exist.
2. **Device handles that are not wired to the device.** Reading handle 0 with `3Fh`
   returns **EOF**, never the keyboard; AUX is not COM1 and PRN is not LPT1.
3. **The machinery around a call**: Ctrl-C checking (INT 23h is never raised), critical
   errors (INT 24h: path calls since #34, 3Fh/40h since #275; not PRN/AUX, not DPMI), INT 28h is never called while DOS waits, and the
   InDOS byte is always 0.
4. **Internals served as stubs**: the List of Lists (SFT/CDS/DPB chains), the DPB from
   `1Fh`/`32h`, the SDA.

| Group | Units | IMPL | PART | STORE | MISS | N/A |
|---|---|---|---|---|---|---|
| §1 INT 21h `00h`–`2Fh` | 34 | 27 | 5 | — | 2 | — |
| §2 INT 21h `30h`–`4Fh` | 36 | 24 | 8 | — | 4 | — |
| §3 INT 21h `50h`–`6Ch` and above | 31 | 23 | 5 | — | 3 | — |
| §4 Other DOS interrupts, and DOS's own machinery | 11 | 6 | 1 | — | 3 | 1 |
| §5 INT 2Fh | 7 | 5 | — | — | 1 | 1 |
| §6 INT 13h / 25h / 26h | 13 | 7 | 1 | — | 4 | 1 |
| **Total** | **132** | **92** | **20** | **—** | **17** | **3** |

---

## 1. INT 21h `00h`–`2Fh`

The handler's CF goes to the guest's pushed FLAGS, or to the live EFLAGS for a DPMI
client (`g_dos_int21_pm`, `:11-15`). An input call with nothing waiting sets `retry` and
leaves EIP on the BOP, so the guest keeps running its ISRs while it "waits".

| AH | Unit | Status | Where / what is missing | Verification |
|---|---|---|---|---|
| `00h` | terminate (CP/M style) | **IMPL** | `:682-710`; = `4Ch` with code 0 | untested (Skyroads exits through it) |
| `01h`/`07h`/`08h` | character input (01 echoes) | **IMPL** | `:713-721`, non-blocking + retry. Ctrl-C checking is §4 | untested |
| `02h` | character output | **IMPL** | `:711-712` | via every probe's own output |
| `03h` | AUX input | **IMPL** | #251: V86 resumes in DOS's AUX driver code (`dos_auxprn.asm`): INT 14h `03h` then `02h`, through the IVT. PM: still `1Ah` | **oracle** (`p_auxprn`, 6.22 + PCem; DOSBox-X hangs on it) |
| `04h` | AUX output | **IMPL** | #251: INT 14h `03h` then `01h` (V86, through the IVT); PM straight to COM1 | **oracle** (`p_auxprn`) |
| `05h` | printer output | **IMPL** | #251: INT 17h `02h`, `02h`, `00h` (V86, through the IVT, so a printer redirector sees it); PM straight to the LPT1 spool. A BIOS error status is not acted on (no retry / INT 24h) | **oracle** (`p_auxprn`) |
| `06h` | direct console I/O | **IMPL** | `:786-794`; `DL=FFh` reads with ZF | untested |
| `09h` | print `$`-string | **IMPL** | `:795-798` (capped at 1024 characters) | untested |
| `0Ah` | buffered line input | **IMPL** | `:722-781`; collected across retries (the s79 shell fix); backspace. The DOS editing keys (F1–F6 and the template) are not | by hand (XP COMMAND.COM) |
| `0Bh` | input status | **IMPL** | `:782-785` | untested |
| `0Ch` | flush input, then run `AL` | **IMPL** | `:1566-1575` | untested |
| `0Dh` | disk reset | **IMPL** | `:2333-2334`; nothing is cached | — |
| `0Eh` | select drive → LASTDRIVE | **IMPL** | `:2297-2332`; a present-but-not-ready drive is still selected (`vdrive`) | **oracle** (`p_drv`) |
| `0Fh`/`16h` | FCB open / create | **IMPL** | `:1010-1041`; ⚠ opens with `FILE_SHARE_READ` only (`:1013`), unlike every handle open — a second open of a file held for writing fails | **oracle** (`p_fcb`) |
| `10h` | FCB close | **IMPL** | `:1042-1045` | **oracle** (`p_fcb`) |
| `11h`/`12h` | FCB find first / next (incl. extended FCB, volume label) | **IMPL** | `:1046-1165` | **oracle** (`p_fcb`); by hand (DIR) |
| `13h` | FCB delete (wildcards) | **PART** | `:1166-1177`: deletes `fd.cFileName` — the **bare** name, relative to the current directory, so the FCB's drive is lost and a delete on another drive removes the wrong file or none | **oracle** for the current-drive case (`p_fcb`) |
| `14h`/`15h`, `21h`/`22h`, `27h`/`28h` | sequential, random and block record I/O | **PART** | `:1188-1236`; ⚠ records over **512 bytes are cut to 512** (`:1194-1196`) | untested |
| `17h` | FCB rename | **PART** | `:1178-1187`: one `MoveFileA`, **no wildcard rename** (`?` in the new name copying from the old) | untested |
| `18h`/`1Dh`/`1Eh`/`20h` | DOS's null functions | **IMPL** | `dos622_defines` `:57-68`: AL=0, CF clear | **oracle** (`p_defs`) |
| `19h` | get current drive | **IMPL** | `:2290-2296` | **oracle** (`p_drv`, `p_curdir`) |
| `1Ah`/`2Fh` | set / get DTA | **IMPL** | `:2286-2289` | via `p_find` |
| `1Bh`/`1Ch` | allocation information | **IMPL** | `:1366-1385`; geometry from the host volume, media byte `F8h` | **oracle**, CF only (`p_rest`) |
| `1Fh` | default drive's DPB | **PART** | `:1386-1415`: a **synthesised** DPB — sector size, cluster size and count are real; FAT count, reserved sectors, root entries, media byte are constants; no FAT, no next-DPB chain | **oracle**, AL only |
| `23h` | FCB file size in records | **IMPL** | `:1237-1253` | untested |
| `24h` | set random record field | **IMPL** | `:1254-1258` | untested |
| `25h`/`35h` | set / get interrupt vector | **IMPL** | `:2242-2251` | untested |
| `26h` | create a new PSP | **PART** | `:1432-1445`: copies the **top-level** PSP (`DOS_PSP_SEG`), not the current one, and always names it the parent | untested |
| `29h` | parse a filename into an FCB | **IMPL** | `:1259-1309`; `*` expands to `?`s; AL bits 1–3 keep drive/name/extension (s81) | **oracle** (`p_fcb`, all four hosts) |
| `2Ah` | get date | **IMPL** | `:2369-2374`, host local time | untested |
| `2Bh` | set date | **MISS** | ⛔ `:2380-2382`: answers `AL=00h` (success) and changes nothing; an invalid date is never refused (`AL=FFh`). INT 1Ah `05h` declines the same request *because* this is the "runs but lies" shape | — |
| `2Ch` | get time | **IMPL** | `:2375-2379`, centiseconds from milliseconds | untested |
| `2Dh` | set time | **MISS** | ⛔ as `2Bh` | — |
| `2Eh` | set verify flag | **IMPL** | `:1576-1577`; stored, read by `54h` (writes are never verified, which is permitted) | **oracle** (`p_file`, via `54h`) |

## 2. INT 21h `30h`–`4Fh`

| AH | Unit | Status | Where / what is missing | Verification |
|---|---|---|---|---|
| `30h` | DOS version, OEM, serial | **IMPL** | `:918-927`; the version is a setting, per process (`dos_version_word`, #208) | **oracle** while the rig is set to 6.22 — see [sweep.md](sweep.md) |
| `31h` | terminate and stay resident | **IMPL** | `:1446-1463`; the host resizes and keeps vectors | **oracle** except `tsr.paras.still.held` (`0x26` vs `0x21`, open) |
| `32h` | DPB for drive DL | **PART** | as `1Fh` (`:1386-1415`) | **oracle**, AL only (`p_rest`) |
| `33h` | Ctrl-Break flag, boot drive, true version | **IMPL** | `:2335-2368`; `00h`–`02h` state, `05h` = C:, `06h` true version | **oracle** (`p_subfn`, `p_ver`) |
| `34h` | InDOS flag address | **PART** | `:1580-1581` returns `ES:BX` into the SDA, but **the byte is never set** — it reads 0 even while `01h`/`0Ah` wait, where real DOS shows 1 | **oracle**, CF only (`int21.34.indos`) |
| `36h` | free disk space | **IMPL** | `:2074-2093`; bad drive = `AX=FFFFh` with CF clear; counts clamped to 16 bits | **oracle** (`p_dir`) |
| `37h` | switch character | **IMPL** | `:1416-1424` | **oracle** (`p_rest`) |
| `38h` | country information (get / set) | **IMPL** | `:2094-2128`; country 1 only — what 6.22 without `COUNTRY.SYS` answers | **oracle** (`p_ctry`, `p_subfn`) |
| `39h`/`3Ah` | mkdir / rmdir | **IMPL** | `:1593-1608` | **oracle** (`p_file`, `p_drv`) |
| `3Bh` | chdir (never moves the current drive) | **IMPL** | `:2039-2073` | **oracle** (`p_drv`, `p_curdir`) |
| `3Ch` | create | **PART** | `:842-844`: **`CX` (attributes) is ignored** — read-only, hidden and system are dropped. Lowest free handle ✓ | **oracle** for the handle number (`p_redir`) |
| `3Dh` | open | **IMPL** | `:845-850`; access mode from AL; sharing bits deliberately not enforced (no SHARE); errors through `dos_err_from_win32` | **oracle** (`p_err` 3D rows, `p_file`) |
| `3Eh` | close | **IMPL** | `:870-876` | **oracle** (`p_redir`) |
| `3Fh` | read | **PART** | `:877-899`: files ✓; a hardware failure (19-31) → INT 24h (#275, `p_crit2`); ⛔ **an unredirected device handle returns 0 bytes (EOF)** (`:897-898`) — reading stdin by handle never reaches the keyboard | **oracle** for files (`p_redir`) |
| `40h` | write | **PART** | `:799-820`: files and the console ✓; a hardware failure (19-31) → INT 24h (#275, `p_crit2`); other WriteFile failures still answer CF=0 with the short count; **`CX=0` does not truncate or extend** the file to the current position (DOS's documented way to set a file's size). Handles 3/4 = AUX/PRN through INT 14h/17h (#251, `p_auxprn`) | **oracle** (`p_redir`, `p_tsr`) |
| `41h` | delete | **PART** | `:1609-1613`: every failure is `2` — a read-only file should be `5`, a missing path `3` | **oracle** for the absent case (`p_file`) |
| `42h` | seek | **IMPL** | `:900-917`; any bound handle, including a redirected low one (#133) | **oracle** (`p_redir`) |
| `43h` | get / set attributes | **IMPL** | `:1614-1632` | **oracle** (`p_file`, `p_subfn`) |
| `44h` `00h` | IOCTL: get device information | **PART** | `:2183`: handles 0–4 are **always** "device", even after a redirect to a file; 5 and up always "file on drive C" | **oracle** (`p_ioctl`, the cases it asks) |
| `44h` `01h` | IOCTL: set device information (raw mode) | **MISS** | falls to `else { OKCF(); }` (`:2233`) — success, no effect | — |
| `44h` `02h`–`05h` | IOCTL: control-channel read / write | **MISS** | same arm — success, nothing transferred | — |
| `44h` `06h`/`07h` | IOCTL: input / output status | **PART** | `:2184`: always `FFh` (ready) — for stdin this should say whether a key is waiting | untested |
| `44h` `08h` | IOCTL: removable media? | **IMPL** | `:2204-2216` from the host drive type | **oracle** (`p_ioctl`, BL=3) |
| `44h` `09h` | IOCTL: remote drive? | **IMPL** | `:2217-2220`; DX bit 12 only | **oracle** (`p_ioctl`) |
| `44h` `0Eh` | IOCTL: logical drive map | **IMPL** | `:2221-2225` | **oracle** (`p_ioctl`) |
| `44h` other | `0Ah`–`0Dh`, `0Fh`–`11h` (remote handle, sharing retry, generic IOCTL, drive map set, query) | **MISS** | `:2233`: CF clear, registers untouched — the "unimplemented call still answers" shape | — |
| `45h`/`46h` | dup / dup2 (files and devices) | **IMPL** | `:1633-1681` | **oracle** (`p_redir`, `p_file`) |
| `47h` | current directory (per drive; `DL=F0h` WOW sentinel) | **IMPL** | `:1965-2038` | **oracle** (`p_curdir`, `p_dir`, `p_drv`) |
| `48h`/`49h`/`4Ah` | allocate / free / resize, owner = current PSP | **IMPL** | `:2252-2281`, `dos_mcb.h` | **oracle** (`p_alloc`, `p_mcb`, `p_tsr`) |
| `4Bh` `00h`/`01h` | EXEC load-and-run / load-only | **IMPL** | `:1315-1332`; the host builds the child (`exec_pending`) | **oracle** (`p_exec`, `p_curdir`) |
| `4Bh` `03h` | load overlay | **IMPL** | `:1333-1347` | **oracle** (`p_ovl`) |
| `4Bh` `05h` | set execution state | **IMPL** | `:1348-1358`; no SETVER table | **oracle** (`p_4b05`, 6.22 + PCem) |
| `4Ch` | terminate with code | **IMPL** | `:675-681` | via every probe |
| `4Dh` | child return code | **IMPL** | `:1682-1684`; cleared by the read | **oracle** (`p_exec`) |
| `4Eh`/`4Fh` | find first / next | **IMPL** | `:928-1002`; AX=18 vs 3 measured | **oracle** (`p_find`) |
| `4Bh` other | `02h`, `04h`, … | **MISS** | `:1359-1365`: `AX=1` CF=1, logged | — |

## 3. INT 21h `50h` and above

| AH | Unit | Status | Where / what is missing | Verification |
|---|---|---|---|---|
| `50h`/`51h`/`62h` | set / get current PSP | **IMPL** | `:2282-2285` | **oracle** (`p_psp psp.50.dispatch`) |
| `52h` | List of Lists | **PART** | `:2163-2179`: the first MCB (`BX-2`), LASTDRIVE and the UMB head are real; **the SFT, CDS and DPB chains are stubs** — `p_sysvar`'s six BUF rows read back the probe's poison | **oracle** for the MCB head (`p_alloc`); 6 rows open (`p_sysvar`) |
| `53h` | BPB → DPB, and XP COMMAND.COM's private `AL` queries | **PART** | `:1464-1548`: `AL=00h`–`07h` from a table measured on **stock XP NTVDM** (`g_dos_int53`, `:22-31`); ⚠ `AL=05h` is context-dependent (the shell's interactive gate). The documented BPB→DPB translation is **not** implemented | stock NTVDM (`p_int53`) — `p_int53f` owed a supervised run |
| `54h` | get verify flag | **IMPL** | `:1578-1579` | **oracle** (`p_file`) |
| `55h` | create child PSP | **PART** | `:1432-1445`: as `26h`, copies the top-level PSP | untested |
| `56h` | rename / move | **PART** | `:1685-1691`: errors collapse to `5` or `2`; a missing path should be `3`, a cross-drive move `11h` (not same device) | **oracle** for success (`p_file`) |
| `57h` `00h`/`01h` | get / set file date and time | **IMPL** | `:1692-1709`; set works through a read-only open (#168) | **oracle** (`p_file int21.5700.stamp`) |
| `58h` | allocation strategy, UMB link | **IMPL** | `:2129-2162`; `5803h` refused, as 6.22 without UMBs | **oracle** (`p_alloc`, `p_umb`, `p_subfn`) |
| `59h` | extended error | **IMPL** | `:1804-1828`, `dos_err.h`; unmeasured codes logged, not invented | **oracle** (`p_err`, `p_misc`) |
| `5Ah` | create temporary file | **IMPL** | `:1710-1739` | untested |
| `5Bh` | create new file | **IMPL** | `:1710-1739`; exists → 80 (measured) | **oracle** (`p_file`) |
| `5Ch` | lock / unlock a range | **IMPL** | `:1740-1750` | **oracle** (`p_file`) |
| `5Dh` `06h` | swappable data area | **PART** | `:1588-1592`: a minimal SDA — critical-error flag and InDOS only | **oracle**, pointer only (`p_misc`) |
| `5Dh` `08h`/`09h` | redirector printer mode / flush | **IMPL** | `:1582-1587` | untested |
| `5Dh` other | server call (`00h`), `0Ah` set extended error, … | **MISS** | reaches the unhandled arm (`:2415-2427`): CF=1, logged | — |
| `5Eh` | machine name / printer setup | **IMPL** | `:1549-1555`; no network: `00h` answers an empty name, the rest `AX=1` | **oracle** (`p_rest`) |
| `5Fh` | redirection list | **IMPL** | `:1556-1558`; no redirector | untested |
| `60h` | truename | **IMPL** | `:1829-1848` | **oracle** (`p_misc`) |
| `61h` | reserved | **IMPL** | null (`:62`) | **oracle** (`p_defs`) |
| `63h` | DBCS lead-byte table | **IMPL** | `:2238-2241`; empty | untested |
| `64h` | device driver lookahead (internal) | **IMPL** | `:1559-1560` | — |
| `65h` | extended country info, capitalise, yes/no | **IMPL** | `:1849-1917` | **oracle** (`p_misc`, `p_subfn`) |
| `66h` | global code page | **IMPL** | `:1425-1431`; 437 only | **oracle** (`p_rest`) |
| `67h` | set handle count | **IMPL** | `:1751-1758` (fixed table) | informational (`p_file`) |
| `68h`/`6Ah` | commit file | **IMPL** | `:1759-1762` | **oracle** (`p_file`) |
| `69h` | get / set volume serial | **IMPL** | `:1918-1964`; `6901h` session-only by decision | **oracle** (`p_misc`, `p_4b05`) |
| `6Bh` | null function | **IMPL** | `:63` | **oracle** (`p_defs`) |
| `6Ch` | extended open / create | **IMPL** | `:1763-1803` | **oracle** (`p_file`) |
| `71h` | long-filename API | **MISS** | `:2383-2395`: the documented "no LFN API" answer (`AX=7100h`, CF=1). Stock XP NTVDM implements it, so XP's own tools expect it | **oracle** for the refusal shape (`p_subfn`) |
| `6Dh`+ | undefined on 6.22 | **IMPL** | `:2396-2414`: AL=0, CF clear | **oracle** (`p_defs`, `p_unimp`) |
| any defined AH not above | — | **MISS** | `:2415-2427`: CF=1 and listed in `unimpl21[]` | — |

## 4. The other DOS interrupts, and the machinery around a call

| Unit | Status | Where / what is missing | Verification |
|---|---|---|---|
| INT 20h terminate | **IMPL** | `main.c:29325-29335` (BOP `30h`); a child returns to its parent (#134) | **oracle** (`psp.00.int20`) |
| INT 22h terminate address | **IMPL** | routed to the INT 20h BOP (`main.c:27117-27119`) | **oracle** (`psp.0A.int22`) |
| INT 23h default handler | **IMPL** | a bare `IRET` (`:27120-27122`) | **oracle** (`psp.0E.int23`) |
| INT 24h default handler | **IMPL** | `MOV AL,3; IRET` — Fail (`:27123-27125`) | **oracle** (`psp.12.int24`, `psp.int24.live` abstained) |
| **Ctrl-C / Ctrl-Break checking → INT 23h** | **MISS** | no input or output call checks for Ctrl-C, and `BREAK=ON` (`33h`) changes nothing; INT 23h is never raised (keyboard.md §3: no Ctrl-Break either) | — |
| **Critical errors → INT 24h** | **PART** | #34: a hardware error (DOS 19-31) on a PATH call raises the program's INT 24h from the main V86 loop (`crit_raise`/`crit_return`, `main.c`; detection at the tail of `dos_int21`); FAIL/RETRY/ABORT, 59h=53h. #275: **3Fh/40h on an open file** now raise it too — AH=3Eh/3Fh (data area, IGNORE allowed), AL = the file's own drive (NT name → drive letter), FAIL → AX=0005, IGNORE → the call reports the bytes asked for; before #275 a failed ReadFile/WriteFile answered CF=0 (a false success). Where INT 24h cannot be raised — a DPMI client's INT 21h, the nested real-mode loops (0301h/0302h, reflected IRQs), inside the handler — 3Fh/40h answer as FAIL; path calls there keep the raw 19-31 code. ⚠ **Not raised:** DPMI reflection (spec: real-mode INT 24h → the client's PM handler via 0205h, else the RM vector; needs the 0301h nested-V86 run factored out); PRN/AUX device errors (the driver ignores the INT 17h status, AH bit 7 path never built); FCB record I/O (`21h`/`22h`/`27h`/`28h`). ⚠ 3Fh/40h values are from the MS-DOS 4.0 source, **unmeasured on 6.22** | **oracle** for path calls (`p_crit`); `p_crit2` (3Fh/40h + PRN) **awaiting its oracle run**; off-VM `err_test.c` |
| **INT 28h called while DOS waits** | **MISS** | the `01h`/`07h`/`08h`/`0Ah` retry waits never issue INT 28h, so a TSR that works in the background on INT 28h never runs at a prompt | — |
| INT 27h terminate and stay resident | **IMPL** | `main.c:29336-29351`; DX in bytes, rounded up | untested |
| INT 28h, when a guest calls it | **IMPL** | `main.c:29352-29353`, returns at once | — |
| INT 29h fast console output | **PART** | `main.c:29354-29358` → `vdd_video_putc`, i.e. INT 10h teletype — invisible in mode 13h, noise in the CGA modes ([video-bios.md](video-bios.md) §1 `0Eh`) | untested |
| INT 2Eh (COMMAND.COM's "execute command") | **N/A** | owned by COMMAND.COM, which installs it when it is the shell; DOS does not | — |

## 5. INT 2Fh

V86 arm `main.c:29394-29530`. **Anything not listed returns with the caller's registers**,
which for an installation check (`AL=00h` in, `AL=00h` out) is the correct "not installed".

| AX | Unit | Status | Where / what is missing | Verification |
|---|---|---|---|---|
| `4300h`/`4310h` | XMS installation / entry | **IMPL** | `:29438-29442` — [xms-ems.md](xms-ems.md) | **oracle** (`p_xms`) |
| `1687h` | DPMI installation / mode-switch entry | **IMPL** | `:29443-29457` — the DPMI surface | untested |
| `122Eh` | DOS error-message tables (XP COMMAND.COM) | **IMPL** | `:29458-29496`; pointers as measured, zero-filled tables (contents are build-specific) | **oracle** for the pointer shape (`p_int2f`, two real kernels) |
| `1684h` | device API entry point | **IMPL** | `:29497-29509`: `ES:DI=0` = none | untested |
| `1600h`, `1689h`, `168Ah` | the Windows queries krnl386 makes | **IMPL** | passed through on purpose; the reasoning per call is recorded at `:29511-29527` | by hand (Win16 boots) |
| `4A01h`/`4A02h` | query free HMA / allocate HMA space (DOS 5+) | **MISS** | passed through: `BX` and `ES:DI` come back as the caller's own — a plausible wrong answer rather than DOS-low's `BX=0`, `ES:DI=FFFF:FFFF` | — |
| other | installation checks for absent TSRs (`1000h` SHARE, `1100h` redirector, `1A00h` ANSI, `B700h` APPEND …) | **N/A** | absent, and answered as absent: `AL=00h` in, `AL=00h` out. ⚠ `1500h` (MSCDEX) answers in **`BX`** (drive count), so pass-through reads "none" only for a caller that zeroed `BX` | — |

## 6. INT 13h, 25h, 26h

The design (`src/dos/dos_disk.h:1-24`): **a drive is a disk image file, or it is absent.**
Only A: is backed, from `cfg\FLOPPY.IMG` (`disk_for`, `main.c:4814-4819`). V86 arm
`main.c:29272-29324` (INT 13h) and `:29359-29374` (INT 25h/26h). ⚠ INT 13h does not drive
the 82077AA ([fdc.md](fdc.md)); the FDC's head never moves.

| Unit | Status | Where / what is missing | Verification |
|---|---|---|---|
| `00h` reset | **IMPL** | `:29276` | provisional (`p_disk`) |
| `01h` last status | **IMPL** | `:29277` | provisional |
| `02h`/`03h` read / write sectors (CHS → LBA from the image's BPB) | **IMPL** | `:29300-29320` | provisional (`int13.02.read.boot`); disk_test |
| `04h` verify | **IMPL** | `:29311-29312`, bounds only | untested |
| `08h` drive parameters (`BH` zeroed, measured) | **IMPL** | `:29286-29294` | provisional (`int13.08.params`) |
| `15h` disk type (floppy, no change line) | **IMPL** | `:29295-29299` | provisional (`int13.15.type`) |
| `05h` format track | **MISS** | `:29321-29324`: `AH=01h` bad command | — |
| `16h` change-line status | **MISS** | bad command | — |
| `17h`/`18h` set media type | **MISS** | bad command | — |
| `0Ch`, `0Dh`, `10h`, `11h` seek, alternate reset, test ready, recalibrate | **MISS** | bad command | — |
| Fixed disks (`DL=80h`+), B:, and the EDD extensions (`41h`+) | **N/A** | by design: no raw access to the host's disks; answered "not ready" (`AH=80h`) | — |
| INT 25h / 26h absolute read / write (LBA) | **IMPL** | `:29359-29374`; A: only | provisional (`int25.absread`) |
| INT 13h drives the FDC chip (DMA channel 2, the head position) | **PART** | the image is read directly; the chip's state never changes, which is why `p_fdc dumpreg` and `p_dma dma.status.idle` differ from the oracles | — |

---

## Measured history (kept)

### The FCB parser (`p_fcb.asm`, `p_find.asm`)

**The gap (s72):** `AH=29h` stored `*` literally. QBasic parses its file pattern with
29h and then matches each directory entry against the parsed FCB, so nothing matched
and its Open dialog listed **no files** while the directory pane beside it was correct.
⛔ I twice guessed QB used the **FCB search**; it enumerates with `AH=4Eh/4Fh`. One
trace line settled what two rounds of reasoning had not.

A failed `4Eh` leaves a live search alone — mismatched in ONE run and clean in three
since, nothing changed: the *first run after a deploy* pattern.

### File / directory / PSP / memory (s72 evening)

The first harvest of the probes that were already written: 36 probes existed and 8 had
ever been diffed. `p_file` 24 rows, `p_alloc` 8, all AGREE; `p_err` 20 rows, **2 real gaps,
closed**; `p_curdir`, `p_psp`, `p_mcb` AGREE with abstentions (not contracts).

**`AH=3Dh` answered "file not found" (2) for every possible failure** (`b580c4d`):

| case | oracle | ours (before) |
|---|---|---|
| `err.after.3D.readonly` — read-only file opened for WRITE | `AX=0005` access denied | `AX=0002` |
| `err.after.3D.baddrive` — open on unclaimed `Y:` | `AX=0003` path not found | `AX=0002` |

Fixed by `dos_err_from_win32()` in `dos_err.h`, with **both sides of every row measured**
(the DOS side from the oracle, the Win32 side from the handler's log). Unmapped codes log
`UNMAPPED` and keep 2. Pinned off-VM by `err_test.c`. ⚠ **There is deliberately no
`ERROR_INVALID_DRIVE` (15) row**: measured, `Y:\...` arrives as 3. ▶ The same collapse is
still in `41h` and `56h` (§2, §3).

### Rows that are NOT contracts (abstentions in `oracle-rules.json`)

The oracle boots to `A:\` and the rig runs probes from deep inside the share, so some
rows compare two *environments*: `int21.19.curdrive`; `curdir.*` (4 rows — the contract,
GH #134, that an EXEC does not clobber the current directory, still holds); `psp.02.memtop`,
`psp.int24.live`; `mcb.head`, `mcb.block` BX/CX/DX (the `'M'` signature and the chain end
at `9FC0` are *not* abstained and agree). `dosdiff.py` separates **ABSTAINED** (a decision)
from **NO-DATA** (missing evidence).

### ⚠ What these probes do NOT cover, despite appearances

`p_file`'s own header claims `46h, 5Ch, 67h, 6Ch`. Several rows compare only `CF`, so "it
returned success" is checked while the returned *value* is not — `int21.5700.getdate` and
`int21.34.indos` are both this shape (and §2 shows InDOS is indeed never set). An all-AGREE
probe is not the same as a verified surface.

### ✅ The HMA (s72) — now in [xms-ems.md](xms-ems.md)

`p_xms` measured us refusing the HMA twice over. NT had mapped the VDM's HMA all along
(`VirtualQuery` showed it `MEM_COMMIT`); we were refusing to admit it. ▶ *Query before you
allocate; an error code that means "occupied" is good news wearing a bad hat.* The first
version of the proof passed while proving nothing (it compared the poison with itself).
No A20 *aliasing* is modelled — a recorded decision in `dos_xms.h`.

### ✅ INT 13h — it was never unimplemented, it had no disk (s72)

`p_disk` showed 13 mismatches, filed as *"INT 13h unimplemented"*. **That reading was
wrong**: with no `cfg\FLOPPY.IMG` the layer correctly answered "drive not ready", and the
registers that looked like untouched poison were a call that had properly **failed**.
Given a disk, **all 18 rows AGREE** (`CX=4F12`, `DX=0101`, `BX=0004`; type `0100h`; the boot
sector `55AA`/`MTOO`). `./scripts/mkfloppy.sh` builds the image. ▶ **Before calling a
surface unimplemented, check that it has something to work on.** ⚠ `cfg\FLOPPY.IMG` must
stay on the rig.

## What to fix, in order

1. `2Bh`/`2Dh`: decide once, with INT 1Ah `03h`/`05h` — refuse both, or keep a per-VDM
   clock offset for both. Today they disagree.
2. Wire the device handles: `3Fh` on stdin reads the keyboard (with the line-input
   rules), `03h`/`04h` go to COM1, `05h` and handle 4 to the LPT1 spool.
3. IOCTL `44h`: refuse what is not implemented; `00h` must report a redirected handle as
   a file; `06h` must report whether stdin has a key.
4. `3Ch` attributes, `40h` with `CX=0`, `41h`/`56h` error codes, `13h` across drives,
   FCB records over 512 bytes, `26h`/`55h` from the current PSP.
5. Ctrl-C → INT 23h, critical errors → INT 24h, INT 28h during waits, and the InDOS byte
   — the four pieces of DOS's own machinery that are absent.
6. The List of Lists' SFT/CDS/DPB chains (`p_sysvar`), which krnl386 walks.
