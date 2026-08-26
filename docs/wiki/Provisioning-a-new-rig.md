# Provisioning a new rig

How to turn a bare Windows XP machine into a test rig, from zero. Everything needed is in
the repository — nothing has to be recovered from an old box.

> Written because the existing rig was built incrementally over many sessions and the
> knowledge lived only in the scripts and the session log. If the box died, rebuilding it
> meant archaeology. This page is the answer to *"a test rig that has never seen this
> project"*.

---

## What you need

- A **32-bit Windows XP SP3** machine. Real hardware is the point — V86 and DPMI on real
  silicon cannot be tested in a VM, and QEMU+HVF aborts on DOS/4GW's paged 32-bit protected
  mode *even under stock `ntvdm`*.
- It on the same LAN as your build machine.
- An account you can log into interactively. **The VDM host needs an interactive session**
  (it is launched via the IFEO key from a real desktop), so a service or a headless login
  will not do.

---

## 1. Build everything on the dev machine

```bash
./scripts/build.sh              # -> build/ntvdmhost.exe   (the host)
./scripts/build-controld.sh     # -> build/controld.exe    (control daemon)
./scripts/build-rigshot.sh      # -> build/rigshot.exe     (desktop capture)
./scripts/check-imports.sh      # assert every import is an XP-shipped DLL
```

## 2. Make the share

On the XP box, create a folder and share it as **`ntvdmex`**, readable and writable by the
account you will use. The canonical path the scripts assume is:

```
C:\Documents and Settings\All Users\Documents\ntvdmex
```

From the dev machine:

```bash
mount_smbfs -N //guest@<box-ip>/ntvdmex /tmp/xpshare
```

> ⚠️ **The mount drops silently.** When it does, writes to `/tmp/xpshare` land in a *local*
> directory that shadows the mountpoint and everything succeeds against nothing. Tell:
> `controld.txt` and `bm/` are missing. Fix: delete the stray local files and remount.
>
> ⚠️ **Ping is filtered.** ICMP silence does not mean the box is down — write a probe file.

## 3. Populate the share

```bash
mkdir -p /tmp/xpshare/bm
cp build/ntvdmhost.exe build/controld.exe build/rigshot.exe /tmp/xpshare/bm/
cp scripts/bm/*.bat /tmp/xpshare/bm/
cp scripts/bm/rt.bat /tmp/xpshare/bm/
```

Guest programs go in the share too — MS-DOS's `COMMAND.COM` and `ATTRIB.EXE` are extracted
from the vendored floppy images with:

```bash
python3 tools/dostest/extract-dos-file.py COMMAND.COM guest/
```

> `guest/`, `games/`, `msdos-622/` and `vm/` are gitignored on purpose: they are Microsoft's
> and id's, not ours. Supply your own copies. `tools/doomoracle/` likewise needs a
> `DOOM1.WAD` you provide.

## 4. Start the watcher (once, at the box)

Log in interactively and run **`bm\runwatch.bat`** from the share. It:

- copies `rt.bat` to `C:\WINDOWS\` — *this is the copy the watcher actually runs*
- installs itself into **Startup**, so a reboot auto-recovers the watcher
- starts `controld.exe`
- then polls `cmd.txt` forever

Leave that window open.

> ⚠️ **This step needs a human at the box exactly once.** After it, the machine is
> remotely recoverable: `controld` accepts `exec`, so a dead watcher can be restarted from
> the dev machine. Before that existed, a broken watcher meant physical access.
>
> ⚠️ **Copy `runwatch.bat` with CRLF line endings.** Written with LF, `cmd.exe` cannot
> resolve `goto loop`, so the watcher runs one iteration and dies — while still writing
> `watcher.txt` once, which makes it look alive. That cost a session.

## 5. Verify the two channels independently

```bash
# control daemon -- should update within ~3 seconds
cat /tmp/xpshare/controld.txt

# watcher -- the ONLY reliable signal is cmd.txt being CONSUMED
printf 'selftest\r\n' > /tmp/xpshare/cmd.tmp && mv /tmp/xpshare/cmd.tmp /tmp/xpshare/cmd.txt
sleep 5 && ls /tmp/xpshare/cmd.txt 2>/dev/null || echo "consumed -- watcher alive"
```

> ⚠️ **`watcher.txt` merely existing means nothing** — a dead watcher and a busy watcher
> look identical. A *frozen* `watcher.txt` while `controld.txt` keeps beating is the
> signature of a dead watcher.

## 6. Set the interception key

```bat
reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe" ^
    /v Debugger /t REG_SZ /d "C:\ntvdmex\ntvdmhost.exe" /f
```

> ⚠️⚠️ **This is the single most important thing to get right, and the easiest to get
> silently wrong.** With the key **absent**, every test runs under *stock* `ntvdm` and still
> "passes" — more than one session has gone into debugging code that was never executed.
> With it **present**, every DOS *and Win16* launch on the box goes to NTVDMEX, and there is
> no WOW layer yet, so 16-bit Windows programs break.
>
> Treat it as a per-run setting, not a machine setting. `rt_stock.bat` drops it, restores it
> on every exit path, and proves the state with `reg query` — copy that pattern.

## 7. Smoke-test it

```bash
printf 'doom\r\n' > /tmp/xpshare/cmd.tmp && mv /tmp/xpshare/cmd.tmp /tmp/xpshare/cmd.txt
```

Then capture the screen to confirm what actually happened:

```bash
# via controld: exec cmd /c ""<share>\bm\rigshot.exe" shot "<share>\shot.bmp""
```

A good first run shows the caption `Microsoft Windows XP Virtual DOS Machine` and a status
strip reading `DOOM.EXE | 32-bit | Protected mode`.

---

## Before you trust any result from this box

Read **[Traps and lessons](Traps-and-lessons)**. In particular: checksum what you deploy
(there are two EXEs and deploying the wrong one still "passes" off a stale log), never edit
a `.bat` in Python text mode, and remember the box is **not necessarily unattended** —
desktop-level behaviour is not the program's until you know nobody is using it.
