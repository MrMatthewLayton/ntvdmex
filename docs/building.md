# Building

NTVDMEX is cross-compiled on macOS or Linux for 32-bit Windows XP. There is no other target:
Virtual-8086 mode exists only in 32-bit x86, so the build is always i686.

## Prerequisites

| Tool | Why |
|---|---|
| `mingw-w64` (i686) | The cross-compiler. |
| `cmake` | The build. |
| `nasm` | Assembles the DOS test probes and `selftest.com`, which the release zip ships. |
| `python3` | The generators in `tools/gen/` and the Win16 test probes. |
| a host `cc` | The off-machine test battery (below) builds natively. |

On macOS: `brew install mingw-w64 cmake nasm python3`. On Linux, your distribution's
packages of the same names.

## Build

```sh
./scripts/build.sh            # configure and build
./scripts/build.sh clean      # from scratch
```

It produces, under `build/`:

| Output | What it is |
|---|---|
| **`ntvdmhost.exe`** | **The VDM host**: the program that replaces `ntvdm.exe`. |
| `ntvdmex.exe` | The NTVDMEX manager: one tray icon for every program NTVDMEX is running. |
| `wowshim/WOW32.DLL`, `wowshim/NTVDM.EXE` | The shims the Windows 3.x support loads; they must match the host's version. |
| `intecho.dll`, `portecho.dll` | The two sample devices from the device SDK (`sdk/`). |
| `probes/*.com` | The DOS test probes (`tests/probes/dos/`). |

`./scripts/package.sh` assembles the release zip in `dist/` from these. Extracting it
anywhere on an XP machine and running `install.bat` is the whole install; see the
[quick start](quick-start.md).

### Why the build looks unusual

The host links with **no C runtime** (`-nostdlib -nostartfiles`); `src/runtime.c` supplies
the entry point and the `mem*` primitives. mingw-w64 defaults to the Universal CRT, which
does not exist on Windows XP, so a CRT-linked binary would not load there. The PE subsystem
and OS version fields are pinned to 5.01 for the same reason.

```sh
./scripts/check-imports.sh    # every import must be a DLL that ships with XP
```

Two consequences:

- No `printf`, `strlen` or `malloc`. Logging uses the helpers in `src/host/log.h`;
  formatting uses `wsprintfA` from user32.
- GCC turns a hand-written `strlen` or `memset` loop into a call to the library function
  unless it is told not to, so every compile uses `-ffreestanding -fno-builtin`.

## Test without a Windows machine

```sh
./scripts/offvm.sh            # the whole battery
./scripts/offvm.sh dos        # only the tests whose names contain "dos"
```

The off-machine battery builds and runs the unit tests in `tests/unit/` natively against the
real `src/` code -- the DOS kernel, the device models, the interpreters, the Windows 3.x
conversions -- in a minute or two, and exits non-zero if any check fails or any test
does not compile. Run it after every change. See [testing](testing.md) for what it covers
and what only a real XP machine can show.

## Run on Windows XP

Install from the release zip as the [quick start](quick-start.md) describes, or by hand:
NTVDMEX becomes the machine's DOS machine through an Image File Execution Options `Debugger`
value on `ntvdm.exe`, which `ntvdmhost.exe /install` sets and `/uninstall` removes. Stock
NTVDM stays on disk, untouched.

## Configuration

Settings live in `HKCU\Software\NTVDMEX` and are edited from **File > Settings**, in six
tabs: MS-DOS, Machine, Video, Audio, Input and Drives. A text file in the installation's
`cfg\` folder overrides the registry for the same setting, so a test can set up the host
without clicking through the dialog:

```
built-in default  <  registry  <  a file in cfg\
```
