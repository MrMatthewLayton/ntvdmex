# The bare-metal rig

A real Windows XP box, driven from the build machine over SMB. **Not optional:** V86 and
DPMI on real silicon cannot be tested anywhere else. QEMU+HVF aborts (`mmu_gva_to_gpa`) on
DOS/4GW's paged 32-bit protected mode *even under stock `ntvdm`*, so every DOS/4GW game —
Doom, Duke, Descent — is untestable on the dev machine.

---

## Shape

```
   build machine  ──SMB──▶  \\<box>\ntvdmex  ◀──  two INDEPENDENT channels on the box
                                              │
                            ┌─────────────────┴─────────────────┐
                            │                                   │
                     runwatch.bat                          controld.exe
                     (test watcher:                        (control daemon:
                      polls cmd.txt,                        reboot | poweroff |
                      runs rt.bat)                          kill | quit | exec)
```

Two channels **on purpose**: `controld` never launches guest code, so it can never wedge,
which means it can recover the watcher when a run hangs it — and `exec` means a dead
watcher is restartable remotely. Before that existed, a broken watcher needed physical
access to the box.

```bash
mount_smbfs -N //guest@<box>/ntvdmex /tmp/xpshare
```

---

## Driving it

**Run a test** — write the target into `cmd.txt`, *atomically*:

```bash
printf 'doom\r\n' > /tmp/xpshare/cmd.tmp && mv /tmp/xpshare/cmd.tmp /tmp/xpshare/cmd.txt
```

**Control the box** — write `control.txt`:

```bash
printf 'kill\r\n'   > /tmp/xpshare/control.txt      # unwedge a hung host
printf 'reboot\r\n' > /tmp/xpshare/control.txt
printf 'exec cmd /c ""<share>\\some.bat""\r\n' > /tmp/xpshare/control.txt
```

**Launchers** in `scripts/bm/`:

| Script | Purpose |
|---|---|
| `cmdcom.bat` | Headless MS-DOS 6.22 `COMMAND.COM` test, driven from `keys.txt`. `cmdcom.bat xp` for XP's own shell. |
| `cmdex.bat` | **Interactive** shell for a human at the box. Deletes the `autoexit` marker (which otherwise kills the session mid-typing) and forces `qimode=0`. |
| `doomex.bat` / `doomrun.bat` | Interactive / headless Doom. |
| `rt_stock.bat`, `stockdoom.bat` | Run a target under **stock `ntvdm`** — the oracle. |
| `rigshot.exe` | Desktop capture and remote window poking. See below. |

---

## Seeing the screen

The host's own screenshot (Capture → Take Screenshot) captures `g_vid.frame` — the **guest
framebuffer**. That is right for "does Doom's status bar render" and useless for anything
about the *host*: the caption, the status strip, the menu bar, a dialog. None of those are
in the guest's framebuffer, which is why every UI change used to need a human at the box.

`scripts/bm/rigshot.c` closes that:

```
rigshot shot <out.bmp>     BitBlt the whole desktop to the share
rigshot cmd  <n>           PostMessage(WM_COMMAND, n) to the VDM window
rigshot click <x> <y>      a real click, through the real hit-test
rigshot key  <vk> [times]  synthetic keypress
rigshot fg   <caption>     bring a window forward (SW_RESTORE if minimized)
rigshot list               dump visible top-level window captions
```

⚠️ **Change a tab with `click`, not `TCM_SETCURSEL`.** The latter crosses a process boundary
fine and does *not* raise `TCN_SELCHANGE`; the notification that would (`WM_NOTIFY`) carries
a pointer Windows will not marshal between processes. Setting the selection without the page
following would "verify" a dialog that does not work.

---

## Traps — every one of these cost a reboot or a session

- **Two EXEs.** `ntvdmhost.exe` is the host, `ntvdmex.exe` is the launcher. Deploy the wrong
  one and it becomes the IFEO debugger for `ntvdm.exe` and relaunches into itself — while
  runs still "succeed", because the harness copies a **stale log**. `deploy.bat` writes the
  deployed size to `deployed.txt`; use it whenever a change appears not to have taken.
- **Never edit a Windows `.bat` in Python text mode on macOS.** It strips CRs, `cmd.exe` can
  no longer resolve `goto`, and the watcher loop dies one iteration in. Write binary, CRLF.
- **A swallowed `copy` error runs a stale binary.** `copy ... >nul` hid a failure while an
  interactive host still held the target open. The only symptom was one missing log line.
- **The mount drops silently.** Writes then land in a local directory that *shadows* the
  mountpoint and everything succeeds against nothing. Tell: `controld.txt` and `bm/` missing.
  Fix: delete the stray local files, remount.
- **Ping is filtered** — ICMP silence does not mean the box is down. Write a probe file.
- **SMB attribute caching lies** about mtime and size. Don't conclude "the run didn't happen"
  from one stale `stat`; look at the directory listing.
- **Stale `TN`.** `for /f ... do set TN=%%c` only assigns on a non-empty read, so an empty
  `cmd.txt` (an SMB create and write are two operations) made the watcher silently re-run the
  **last** target. A queued `skyroads` ran `p_ver` and the log looked plausible.
- **`watcher.txt` existing means nothing.** A dead watcher and a busy watcher look identical.
  The only reliable "it started" signal is `cmd.txt` being **consumed**.
- **`controld` exec quoting.** The share path has spaces, so
  `exec cmd /c "…\cmdcom.bat xp"` silently does nothing. The inner path needs its own quotes:
  `exec cmd /c ""…\cmdcom.bat" xp"`. A run that never starts looks exactly like a run that
  produced no output.
- **`bm\rt.bat` edits do not take effect until a reboot** — only `runwatch.bat` at startup
  copies it to `C:\WINDOWS\rt.bat`, which is what the watcher actually runs.
- **Wait for the run to finish** before checking a copied-back artefact. Reporting "the copy
  failed" when the batch had not yet reached the copy is a mistake that has been made.
- **The box is not necessarily unattended.** Desktop-level behaviour — focus, z-order,
  minimize, cursor — is not the program's until you have established nobody is using it.

---

## The one that matters most

**With the IFEO `Debugger` key absent, every test silently runs under stock `ntvdm` and
still passes.** More than one session has gone into debugging code that was never executed.
Any harness that runs a test should be able to say which VDM it actually used — `rt_stock.bat`
proves it with `reg query`, and so should anything else.

Corollary: leaving the key *set* is equally a trap for the next person, because it changes
every DOS and Win16 launch on that machine. `cmdcom.bat` sets the `qimode` gate for the run
and **puts it back** for exactly this reason. A knob left flipped by a test is a trap.
