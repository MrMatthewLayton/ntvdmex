**What this changes**, and why. Closes #

**How you know it works:**

- [ ] `./scripts/offvm.sh` is green, with a test added or extended where the logic can run
      off the machine
- [ ] `./scripts/style.sh --check` passes
- [ ] For a change that should not alter behaviour: `FNCMP_ALL=1 ./tools/fncmp/fncmp.sh main .`
      reports IDENTICAL
- [ ] Run on Windows XP, if you have it: what you ran, and what happened

- [ ] This contains no Microsoft code, no third-party code or disassembly, and nothing
      from Microsoft's source code or confidential design, however it was seen
      ([CLEAN-ROOM.md](https://github.com/MrMatthewLayton/ntvdmex/blob/main/CLEAN-ROOM.md))
- [ ] I have **not** seen the source code or internal design of the component this
      change reimplements -- or, if I have, I say so here, and to what

The maintainer runs every change on a real XP machine before merging.
