# Spike-002: Does the WOW registry repoint launch our binary?

- **Status:** ✅ **Confirmed** (2026-06-02, Run 2) — repointing `cmdline` launches our binary as
  the DOS VDM support process. ADR-0002 validated. Key twist: the target program is **not** passed
  on the command line.
- **Risk addressed:** Validates **ADR-0002** (intercept by repointing the WOW registry, not by
  replacing the signed `ntvdm.exe`). The whole "true replacement, no WFP fight" premise rests on
  this. Also a prerequisite for Spike-001, which assumes the repoint works.
- **Time box:** tiny — a logging stub + a 4-byte DOS trigger + a few registry commands.

## Question
When `HKLM\SYSTEM\CurrentControlSet\Control\WOW\cmdline` is repointed at our binary, does XP
launch **us** (not the real `ntvdm.exe`) as the VDM support process for a 16-bit DOS program —
and **what exact command line** does it hand us?

## Hypothesis
Yes. XP reads the support-process command from `cmdline` at launch time and does not pin it to a
signed image, so repointing it runs our stub, passing a command line that identifies the 16-bit
program to run (the launch contract we must later honour).

## Harness (built — `tools/wowprobe/`)
- **`wowprobe.exe`** — a no-CRT stub (imports only kernel32+user32, loads on XP). On launch it
  records `GetCommandLineA()`, the current directory and its own module path, writes them to
  `wowprobe.log` next to the exe, pops a MessageBox as immediate proof, and exits. It does **no**
  VDM work.
- **`dosstub.com`** — a 4-byte 16-bit DOS `.COM` (`mov ah,4Ch / int 21h`) whose only purpose is
  to be a 16-bit image that triggers the VDM launch path. Build it with `tools/wowprobe/make-dosstub.sh`.

Build both: `./scripts/build.sh` → `build/wowprobe.exe` (+ `tools/wowprobe/dosstub.com`).

## Pairing: trigger type must match the key
The DOS and Win16 launch paths read **different** registry values, so the test binary must match
the value being repointed — mixing them gives a false negative:

| App type | Image format | Selects the host via |
|----------|--------------|----------------------|
| DOS | `.COM` / 16-bit MZ `.EXE` | `Control\WOW\cmdline` |
| Win16 | NE-format `.EXE` | `Control\WOW\wowcmdline` |

`dosstub.com` exercises the **DOS** path (`cmdline`). To exercise the **Win16** path, repoint
`wowcmdline` and launch an NE `.EXE` (none in the harness yet — a `win16stub` can be added).
Note the Win16/WOW VDM is a *shared, persistent* process, so its value is even more boot-cached.

## Method (run on the XP SP3 VM, as an Administrator)
Snapshot the VM first — this touches a system registry key.

1. **Stage files.** Copy `wowprobe.exe` and `dosstub.com` to e.g. `C:\ntvdmex\`.
2. **Capture + back up the current value** (this also confirms the default format, a `[VERIFY]`
   item in research/ntvdm-architecture.md):
   ```
   reg query  "HKLM\SYSTEM\CurrentControlSet\Control\WOW" /v cmdline
   reg export "HKLM\SYSTEM\CurrentControlSet\Control\WOW" C:\ntvdmex\wow-backup.reg
   ```
3. **Repoint `cmdline` at our stub** (one line — no `^` continuation), then confirm it stuck:
   ```
   reg add "HKLM\SYSTEM\CurrentControlSet\Control\WOW" /v cmdline /t REG_EXPAND_SZ /d "C:\ntvdmex\wowprobe.exe" /f
   reg query "HKLM\SYSTEM\CurrentControlSet\Control\WOW" /v cmdline
   ```
3a. **Reboot the VM** — the WOW command lines appear to be cached by the Base subsystem at boot
   (see Result/Run 1), so a live edit is not picked up until restart.
4. **Trigger the 16-bit launch path:** run `C:\ntvdmex\dosstub.com` (double-click, or from a
   fresh `cmd`). Try launching a couple of other 16-bit apps too (e.g. an old DOS `.exe`).
5. **Observe:** a MessageBox titled *"NTVDMEX wowprobe — we were launched!"* = success. Read the
   command line it shows and the saved `C:\ntvdmex\wowprobe.log`.
6. **Restore (important):**
   ```
   reg import C:\ntvdmex\wow-backup.reg
   reg query  "HKLM\SYSTEM\CurrentControlSet\Control\WOW" /v cmdline   :: confirm restored
   ```
   Then run a 16-bit app to confirm normal DOS support works again. (If anything is wrong,
   revert the VM snapshot.)

> Note: while repointed, **no** 16-bit app actually runs — every launch just pops our probe.
> Restore before using the VM for anything else.

## Success criteria
- [ ] Launching `dosstub.com` shows the wowprobe MessageBox / writes `wowprobe.log` → XP launched
      our binary as the VDM support process.
- [ ] The captured `GetCommandLineA()` is recorded verbatim below (the launch contract).
- [ ] Repeatable for a second/third 16-bit program.
- [ ] `cmdline` restores cleanly and normal 16-bit support returns.

## Possible outcomes & decision impact
- ✅ **Probe launches** → **ADR-0002 confirmed.** Record the command-line format; it feeds
  Spike-001 (which registers via the same repoint) and the eventual host argument parsing.
- ⚠️ **Probe launches but the command line is opaque/empty** → repoint works; we still need to
  learn how the host discovers *which* program to run (disassembly follow-up).
- ⛔ **Real `ntvdm.exe` runs anyway (our probe never appears)** → the DOS launch path does not key
  off `cmdline` the way we assumed. Re-examine: is it `wowcmdline`, a different key, an image-path
  cache, or a per-session setting? This would force a rethink of the interception mechanism
  (amend ADR-0002).

## Result

### Run 1 (2026-06-02) — in-session repoint ignored
Repointed `cmdline` and immediately launched `dosstub.com` in the same boot session: the **stock
`ntvdm.exe` ran, our probe never appeared.** Useful sub-finding: the `.COM` *did* trigger a VDM
support process, so the trigger and 16-bit routing work — only our redirection had no effect.

Two leading explanations, to disambiguate before concluding `cmdline` is the wrong mechanism:
1. **Write didn't apply** — `reg add` to HKLM needs an elevated/admin console; verify the value
   actually changed with `reg query` (rule out a silent access-denied or a malformed command).
2. **Boot-cached** — the Base subsystem (`csrss`/`basesrv.dll`) reads the `Control\WOW` command
   lines into its static server data at **system boot**; a live edit is not seen until **reboot**.
   `[BELIEF — to confirm]`

### Run 2 (2026-06-02) — SUCCESS ✅
On the QEMU XP VM: ran the setup (repoint `cmdline` → `C:\ntvdmex\wowprobe.exe`), **rebooted**,
then launched a 16-bit DOS program. Our probe ran — `C:\ntvdmex\wowprobe.log` was written:
```
NTVDMEX wowprobe -- XP launched us as the VDM support process.
GetCommandLineA():
  "C:\ntvdmex\wowprobe.exe"
CurrentDirectory:
  C:\ntvdmex
ModuleFileName:
  C:\ntvdmex\wowprobe.exe
```
Findings:
- `[FACT]` **ADR-0002 confirmed.** Repointing `Control\WOW\cmdline` makes XP launch our binary as
  the DOS VDM support process.
- `[FACT]` **Boot-cached.** The change had no effect in-session (Run 1); it took effect only after
  a reboot. The base subsystem (`csrss`/`basesrv`) reads the WOW command lines at boot.
- `[FACT]` **The target program is NOT on the command line.** XP launched us with a bare
  `"C:\ntvdmex\wowprobe.exe"` — no DOS app path, no handles.
- `[BELIEF]` Therefore the support process receives the program-to-run and its VDM state through
  the **CSRSS/VDM LPC channel + `NtVdmControl`**, not argv. Recovering that handshake is the job of
  Spike-001. (Our passive stub launched, logged, exited without the handshake → the 16-bit launch
  then failed with "not a valid Win32 application", as expected.)
- Open: capture the **default** `cmdline` value (was it bare too? likely
  `%SystemRoot%\system32\ntvdm.exe`) to confirm the format — grab from `C:\ntvdmex\wow-backup.reg`.

### Run 2 plan (executed above)
1. Set the value (single line, **admin** console):
   `reg add "HKLM\SYSTEM\CurrentControlSet\Control\WOW" /v cmdline /t REG_EXPAND_SZ /d "C:\ntvdmex\wowprobe.exe" /f`
2. **Confirm it stuck:** `reg query "HKLM\SYSTEM\CurrentControlSet\Control\WOW" /v cmdline` → must
   show our path.
3. **Reboot the VM.**
4. Launch `dosstub.com`. Probe fires → it was boot-cached (record that; it matters for install/
   uninstall UX). Still stock ntvdm → `cmdline` is not the DOS mechanism; move to follow-up.

### If Run 2 still fails — follow-up
The DOS support-process command is then not driven by `Control\WOW\cmdline`. Recover the real
path by disassembling `kernel32!BaseCheckVDM` / `CreateProcessInternalW` (DOS branch) and
`basesrv.dll` (`BaseSrvpStaticServerData` / WOW init) on XP SP3 to find which value/string builds
the VDM command line. Candidates to examine: `wowcmdline`, a hardcoded `ntvdm.exe` path, or a
CSRSS-side construction. Outcome feeds an amendment to ADR-0002.

### Data to capture
- The **default** `cmdline` value (reveals the expected format) — paste verbatim.
- Whether the probe fired pre- vs. post-reboot.
