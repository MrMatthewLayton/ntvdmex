# Clean-room policy

NTVDMEX is an independent reimplementation of the Windows XP NT Virtual DOS Machine and
WOW. It contains no Microsoft code, and it must never contain any. This page says where
the project's knowledge comes from, what may never enter the repository, and how to write
about another program's behaviour.

## Where the knowledge comes from

1. **Published specifications.** Hardware datasheets, the DPMI/XMS/EMS/VESA specifications,
   the Ralf Brown Interrupt List, Microsoft's own documentation and KB articles, and
   documented file formats. Cited in [`docs/ref/SOURCES.md`](docs/ref/SOURCES.md).
2. **Black-box measurement.** The probes in [`tests/probes/`](tests/probes/) run the same
   questions under NTVDMEX and under reference systems: XP's own NTVDM/WOW, MS-DOS 6.22,
   DOSBox-X and PCem. The answers stock XP gave are kept beside the probes, in each
   `stock/` folder. A probe result is the preferred evidence for any behavioural claim.
3. **Interoperability study.** XP's binaries were studied to learn the interface a host
   has to provide: the `NtVdmControl` contract, the `VDM_TIB` layout, the BOP encoding,
   and the calls the 16-bit WOW modules make into the host. Only the resulting interface
   facts are recorded here; the study itself is not part of the repository.
4. **Other open projects, as documentation.** ReactOS, Wine, dosemu, DOSBox and others
   describe semantics. Their code is never copied, and copyleft code cannot be copied into
   an MIT project at all. See [`docs/reference-projects.md`](docs/reference-projects.md)
   for what each is good for, and for the projects that must not be read.

## What never goes into the repository

- **Leaked Microsoft source code, or anything derived from it**, including patch files
  against it (a patch quotes its context).
- **Third-party code in any form**: disassembly listings, instruction sequences,
  decompiled code, code bytes copied into test fixtures, or addresses inside someone
  else's binary cited to explain how it works. This applies to Microsoft's system files
  and MS-DOS's programs as much as to the games NTVDMEX runs.

  *Not* in this category: values the host must recognise at run time because the guest
  hands them over — thunk ids, argument sizes, the return offsets a call carries,
  structure layouts shared with the operating system. Those are the interface. Say
  where each one is seen at run time.
- **Third-party files**: binaries, ROM images, fonts, game data, and copyrighted
  specifications. Link to or cite them instead.

## Writing about another program's behaviour

Comments and documents say **what was observed and how it was measured**, never how the
other program's code does it.

| ✅ Write | ❌ Not |
|---|---|
| "XP's COMMAND.COM prints *Incorrect DOS version* unless `INT 21h AH=30h` returns `AX=0005h` (*and how that was seen: the probe, the reference host, the run*)." | "COMMAND.COM at `0x1563` does `mov ah,30h / int 21h / cmp ax,5`." |
| "During start-up krnl386 creates a 64 KB selector over its own stack (observed at run time)." | "krnl386 `seg1:0xc17e` builds the selector, then calls `0xd45a`." |

Session numbers, probe names and "stock answers X" are all fine as provenance. If a value
in our code exists only because another program expects it, say so, and say how that was
observed.

## Contributing

By contributing you confirm that the work is your own and that it contains none of the
material listed above. If you have read leaked Windows source code, please do not
contribute to NTVDMEX: what you know cannot be separated from what you write, and the
risk would attach to the whole project.
