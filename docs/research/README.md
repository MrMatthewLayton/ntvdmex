# Research

Findings that inform decisions. Each claim is tagged with a **confidence level** — this
project leans heavily on undocumented behaviour, so honesty about what is verified vs. assumed
is part of the engineering discipline.

- `[FACT]` — verified (docs, disassembly, or reproduced experiment)
- `[BELIEF]` — high confidence, not yet verified here
- `[VERIFY]` — assumption we must confirm before relying on it

## Documents

| File | Topic |
|------|-------|
| [ntvdm-architecture.md](ntvdm-architecture.md) | How real NTVDM is structured on XP-32 |
| [ntvdmcontrol-and-v86.md](ntvdmcontrol-and-v86.md) | The V86 / `NtVdmControl` execution contract |
| [signing-and-wfp.md](signing-and-wfp.md) | Why signing isn't the blocker; WFP is; how to avoid it |
| [reference-projects.md](reference-projects.md) | ReactOS, dosemu, DOSBox, etc. — what each is good for |
| [build-toolchain.md](build-toolchain.md) | mingw-w64 cross-build, no-CRT link, and the XP-compatibility traps |
| [xp-test-vm.md](xp-test-vm.md) | The XP-on-QEMU test bench: period-correct hardware choices and rationale |
