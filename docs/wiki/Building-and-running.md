# Building and running

## Build

Cross-compiled from macOS or Linux to 32-bit Windows XP. There is no other valid target:
V86 mode exists only in 32-bit mode, so this is permanently i686.

```bash
brew install mingw-w64 cmake        # or your distro's equivalent
./scripts/build.sh                  # configure + build
./scripts/build.sh clean            # from scratch
```

Produces:

| Binary | What it is |
|---|---|
| **`build/ntvdmhost.exe`** | **The VDM host.** This is the one that matters. |
| `build/ntvdmex.exe` | A small separate launcher/shell preview. |
| `build/rigshot.exe` | Desktop capture + remote poking for the test box (`scripts/build-rigshot.sh`). |
| `build/controld.exe` | The rig's control daemon (`scripts/build-controld.sh`). |

> ⚠️ **Deploying the wrong one of the first two has cost real sessions.** If you deploy the
> launcher it becomes the IFEO debugger for `ntvdm.exe` and relaunches into itself — and runs
> still appear to "succeed" because the harness copies a stale log. The only tell is that two
> different targets produce the same md5. **Checksum what you deploy.**

### Why the build looks strange

It links with **no C runtime at all** (`-nostdlib -nostartfiles`), and `src/runtime.c`
supplies the entry point and `mem*` primitives.

This is not minimalism for its own sake: the mingw-w64 toolchain is **UCRT-default**, and
UCRT (`api-ms-win-crt-*.dll`) does not exist on Windows XP. A CRT-linked binary simply will
not load there. The PE subsystem and OS version fields are pinned to 5.01 for the same
reason.

```bash
./scripts/check-imports.sh          # asserts every import is an XP-shipped DLL
```

Two consequences worth knowing:

- No `printf`, no `strlen`, no `malloc`. Use the `zput`/`zhex` helpers in `src/host/log.h`
  and `wsprintfA` from user32.
- **GCC will turn a hand-rolled `strlen` loop into a call to `strlen`** unless you pass
  `-ffreestanding -fno-builtin`. It will do the same for `memset`. This is why those flags
  appear in every build script here.

---

## Test it without any Windows machine

```bash
cd tools/dostest
for t in *_test; do printf '%-16s ' "$t"; ./$t >/dev/null && echo PASS || echo FAIL; done
```

17 batteries, a couple of seconds, and they exercise the real DOS and device code. This is
the development loop — see [Testing and oracles](Testing-and-oracles).

---

## Run it on Windows XP

NTVDMEX becomes the machine's VDM through an **Image File Execution Options `Debugger`
value** on `ntvdm.exe`:

```bat
copy ntvdmhost.exe C:\ntvdmex\
reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe" ^
    /v Debugger /t REG_SZ /d "C:\ntvdmex\ntvdmhost.exe" /f
```

To put the machine back exactly as it was:

```bat
reg delete "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe" ^
    /v Debugger /f
```

Then launch any DOS program. `C:\ntvdmex\target.txt` overrides what gets run, which is how
the test harness drives it without depending on shell integration.

### ⚠️ Read this before installing it on a machine you care about

- **Win16 programs will break.** 16-bit *Windows* programs launch through `ntvdm.exe` too,
  and NTVDMEX has no WOW layer at all. Until
  [#129](https://github.com/MrMatthewLayton/ntvdmex/issues/129) (passthrough) lands, this key
  should be treated as a test-rig setting, not a daily driver.
- **Console behaviour differs.** DOS output is buffered and flushed to `CONOUT$` at exit, so
  redirection and piping from `cmd.exe` are bypassed and every DOS program pops a window
  ([#131](https://github.com/MrMatthewLayton/ntvdmex/issues/131)).
- **If the host will not start, every DOS and Win16 launch on the machine is broken** and the
  fix is the `reg delete` above. Know that before you need it
  ([#132](https://github.com/MrMatthewLayton/ntvdmex/issues/132)).
- **With the key absent, everything silently runs under stock `ntvdm` and still "works".**
  More than one session has gone into debugging code that was never executed. If a result
  surprises you, check the key first.

---

## Configuration

Settings live in `HKCU\Software\NTVDMEX`, edited from **File → Settings…** (six tabs:
General, CPU, Display, Audio, Input, Drives).

Precedence is:

```
built-in default   <   registry   <   text file on the test share
```

The text files win **on purpose** — the headless rig configures the host by writing files
and re-launching it, and a setting clicked in a dialog must never silently change what a
measurement is measuring. See [Motivations and decisions](Motivations-and-decisions).

> ⚠️ **40 of the 46 settings are stored and honoured by nothing** — they came from the old
> menu scaffold. `settings_apply()` in `src/host/main.c` is the honest list of the ones the
> emulator actually consults ([#136](https://github.com/MrMatthewLayton/ntvdmex/issues/136)).
