# Contributing to NTVDMEX

Thank you for wanting to help. NTVDMEX replaces `ntvdm.exe` on Windows XP: DOS and Windows
3.x programs run on the real processor, and everything they ask of the machine is
reimplemented here. This page is how to get a change in.

## Before you start

- **Read [CLEAN-ROOM.md](CLEAN-ROOM.md).** It is short, and it is the one rule that cannot
  bend: no Microsoft code, no third-party code or disassembly, and if you have read leaked
  Windows source code, please do not contribute.
- **Find or open an issue** for anything bigger than a small fix, and say there that you
  are working on it. Issues labelled `good first issue` are a good start.
- Read the [architecture](docs/architecture.md) for the shape of the code, and
  [lessons.md](docs/lessons.md) before trusting a measurement.

## Build and test

Everything builds and most of it tests on macOS or Linux; see [building](docs/building.md).

```sh
./scripts/build.sh            # the XP-32 cross build (mingw-w64 i686, cmake, nasm)
./scripts/offvm.sh            # the off-machine battery: must be green
./scripts/style.sh --check    # the house style: must pass (--fix rewrites)
./scripts/check-imports.sh    # every import must ship with Windows XP
```

A test that does not compile counts as a failure. If your change has logic that can run
off the machine -- DOS services, a device model, a conversion -- add or extend a test in
`tests/unit/`. [testing.md](docs/testing.md) explains the battery, the probes and the
oracles.

## The code style

[docs/STYLE.md](docs/STYLE.md) is the style. `scripts/style.sh --fix` applies the layout
and comment rules for you, and proves it changed no code; to run it on every commit,
`git config core.hooksPath scripts/hooks`. What the tool cannot do is yours:

- **Names** say what a thing is: PascalCase functions with their module's prefix, camelCase
  locals, `g_` globals, no single letters, no Hungarian notation. Windows types only.
- **No magic values**: a number or string that means something gets a name.
- **A long expression** is wrapped by hand, before an operator, so its structure shows.
- **A comment** says what the code cannot: why, and what was observed and how. Never how
  another program's code does it (see CLEAN-ROOM.md).

## Pull requests

All changes, the maintainer's included, go through a pull request, and CI must be green:
the cross-build, the battery and the style check.

- Keep a pull request to one subject, and say in it what changed and how you know it works.
  Name the issue it closes.
- A change that should not alter behaviour (a rename, a move, a restyle) can be proven:
  `FNCMP_ALL=1 ./tools/fncmp/fncmp.sh main .` compares the compiled code function by
  function. Say in the pull request that it reports IDENTICAL.
- **Before merging, the maintainer runs the change on a real Windows XP machine**: games
  for timing, graphics, sound and protected mode, and the Windows 3.x probes against stock
  NTVDM. CI cannot do that, because nothing but real XP can. If you have an XP machine, say
  what you ran on it.

## Reporting a bug

Open an issue with the program (name, version, where it came from), what you expected, what
happened, and the log of that run: `debug\out\ntvdmhost.log` in the installation folder
(the logs of the runs before it are kept beside it). A security problem goes to
[SECURITY.md](SECURITY.md) instead.

## Licence

NTVDMEX is [MIT](LICENSE)-licensed. By contributing you agree that your contribution is
licensed the same way, and you confirm what CLEAN-ROOM.md asks.
