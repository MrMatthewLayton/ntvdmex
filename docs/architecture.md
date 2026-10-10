# Architecture

```
        Windows launches ntvdm.exe
                  │   (IFEO "Debugger" value redirects it)
                  ▼
        ┌──────────────────────────────────────────────┐
        │  ntvdmhost.exe          src/host/main.c      │
        │                                              │
        │  UI thread ─────────── window, menu, status  │
        │                        strip, present, input │
        │  exec thread ───────── the guest             │
        └───────┬──────────────────────────────────────┘
                │ NtVdmControl(VdmStartExecution)
                ▼
        ┌──────────────────────────────────────────────┐
        │  THE REAL CPU, in Virtual-8086 mode          │
        │  (or 16-/32-bit protected mode, via DPMI)    │
        └───────┬──────────────────────────────────────┘
                │ traps back on I/O, INT, faults
                ▼
   ┌────────────┬────────────┬────────────┬────────────┐
   │  DOS       │  BIOS      │  DPMI      │  VDD bus   │
   │  src/dos/  │            │  src/vdm/  │  src/vdd/  │
   │  INT 21h   │  INT 10h   │  INT 31h   │  video     │
   │  MCB/PSP   │  INT 16h   │  LDT       │  audio     │
   │  loader    │  INT 13h✗  │  PM entry  │  input     │
   │  XMS/EMS   │  INT 15h   │  callbacks │  PIT/PIC   │
   └────────────┴────────────┴────────────┴────────────┘
```

---

## Layout

| Path | What lives there |
|---|---|
| `src/host/` | The host process, one module per area: `main.c` (`WinMain`), `host_startup.c` (the start-up steps), `host_exec.c` (the exec loop and NTVDM's own services), `host_dpmi*.c` (the DPMI host and its client sessions), `host_video.c` (with the two instruction interpreters, `v86interp.h` and `pm32interp.h`), `host_window.c`, `host_wow.c`, and the rest -- each with its header. `host_state.c` holds the state several modules share; `host_types.h` the shared types and constants. |
| `src/vdm/` | The VDM layer: `v86.c` (the kernel's VDM support), `dpmi.c` and `dpmi_enter.S` (protected mode), `csrss.c` (the console session). |
| `src/dos/` | The DOS kernel: `dos_int21.c` (the INT 21h surface), memory blocks, the PSP, the loader, the environment, XMS, EMS, the clock -- mostly pure logic over guest memory, so the off-machine battery can test it. |
| `src/vdd/` | The virtual devices: video, the Sound Blaster, OPL, MPU-401, Gravis UltraSound, EMU8000, the PC speaker, DMA, PIC, PIT, CMOS, keyboard, joystick, serial, IDE, floppy, NetBIOS -- and the bus that routes port I/O to them. `present_ddraw.c` presents the picture (GDI windowed, DirectDraw fullscreen). |
| `src/wow/` | Windows 3.x support: the WOW32 call dispatch (`wow32.c`) and USER, GDI, the message queue, windows, dialogs, resources, the scheduler -- one file per area, the call tables in `wowuser_calls.h` / `wowgdi_calls.h`. |
| `sdk/` | The device SDK: the interface a third-party virtual device builds against, with two samples. |
| `res/` | Win32 resources: the manifest that gives the Luna theme, the icon, the Settings dialog. |
| `tests/` | The off-machine unit tests (`tests/unit/`) and the DOS and Windows 3.x probes (`tests/probes/`); see [testing](testing.md). |
| `tools/` | The generators (`tools/gen/`), `fncmp` (proof that a refactor changed no code), `dlgcheck`, the NE linker for the Windows 3.x probes. |
| `scripts/` | `build.sh`, `offvm.sh` (the battery), `package.sh` (the release zip), `check-imports.sh`. |

---

## The two threads, and why it matters

The **UI thread** owns the window, the menu, the status strip, the present loop and input.
The **exec thread** is inside `NtVdmControl` running guest code.

Almost every hard bug in this project lives on the boundary between them:

- A DOS input call that **blocks** on the exec thread deadlocks any guest that needs an
  interrupt to make progress. `INT 21h AH=0Ah` did exactly this and made a shell impossible:
  what it waits for cannot arrive while it waits, because the BIOS key ring is filled by the
  guest's own `INT 09h` ISR, which cannot run because we are inside its `INT 21h` call.
  **Every blocking input call must poll via `retry`, never park the thread.**
- The status strip is *polled* from the UI tick rather than pushed from the exec thread,
  because the facts it reports (`g_dpmi_pm`, `g_dpmi_client32`) are set deep inside the DPMI
  mode switch.
- The frame timer asks for 5 ms; XP's default granularity is 15.6 ms, so it historically ran
  at ~64 Hz. Raising resolution with `timeBeginPeriod(1)` makes it fire at the 5 ms it always
  asked for — **tripling the frame work**, which saturates the UI thread, which is the thread
  that turns `WM_KEYUP` into a break code.

---

## How a guest instruction becomes a service call

1. Guest executes `INT 21h` (or an `IN`/`OUT`, or faults).
2. The CPU/kernel traps out; `NtVdmControl` returns with an event.
3. The exec loop (`host_exec.c`) decodes the event: which interrupt, which port, which fault.
4. It dispatches to the DOS layer, the BIOS layer, or the VDD bus.
5. Registers are written back into the VDM_TIB, `EIP` is advanced past the instruction, and
   execution resumes.

**Two subtleties that have caused real bugs:**

- **`EIP` is only 16-bit when its selector is.** Masking it unconditionally corrupts 32-bit
  protected-mode guests.
- **Flags must be copied back.** The RMCS copy-back once dropped FLAGS, so every DOS call a
  32-bit client made reported success regardless of what happened.

---

## DPMI, in brief

DOS extenders (DOS/4GW, which is every DOS/4GW game including Doom) ask for protected mode
via `INT 2Fh AX=1687h`, then far-call an entry point we supply.

- The mode switch installs an LDT and the initial CS/DS/SS. **Those must be D/B=0 even for a
  32-bit client** — the client's post-switch code has to be valid real-mode code on the
  failure path, so it cannot be 32-bit. A 32-bit client then far-jumps to its *own*
  `INT 31h`-allocated 32-bit selectors.
- `INT 31h` provides descriptor management, memory, real↔protected transitions (0300/0301/
  0303), callbacks, and raw mode switching (0305/0306).
- Because a spinning PM guest never voluntarily leaves protected mode, **interrupts are
  delivered asynchronously** by suspending the thread and rewriting its context
  (`SuspendThread`/`SetThreadContext`). The host owns IRQ vectors 08h–0Fh.
- `INT nn` sites in guest code are rewritten to BOPs so they trap to us. **This is dangerous
  and once corrupted Doom for five sessions** — a naive `CD nn` byte-pair match rewrote a
  `jle`'s displacement. `x86len.h` is a real length decoder plus a boundary vote, and exists
  entirely because of that bug.

---

## Video

Virtualised and blitted — the guest never touches the CRTC.

- Text mode renders with the character set of the Windows code page 437 fonts, read from the
  system's own .FON files at start-up (no font is shipped). Cursor shape comes from
  the guest via `INT 10h AH=01 CX`, and blink phase from the injected clock, so the VDD stays
  pure and off-VM testable.
- Mode 13h and mode-Y, including the page flipping Doom uses.
- **Mode 12h (planar) is special**: arming the A0000 `NOACCESS` page trap *freezes the guest*
  on real hardware, so planar stores are decoded and executed by the host. In planar modes
  the interpreter genuinely is the CPU.
- Presentation is GDI when windowed, exclusive DirectDraw when fullscreen. In fullscreen the
  primary surface covers the menu bar **and the status strip** — which is why the input-capture
  release chord is currently invisible in that mode
  ([#138](https://github.com/MrMatthewLayton/ntvdmex/issues/138)).

---

## Audio

`vdd_sb.c` (SB16 DSP) pulls data through `vdd_dma.c` (8237) and pushes it to `audio_wave.c`
(waveOut, loaded dynamically). `vdd_opl.c` + `vdd_opl_synth.c` are the clean-room FM
synthesiser; `vdd_mpu.c` is MPU-401 MIDI; `vdd_speaker.c` the PC speaker.

**The lesson buried in this subsystem:** PCM delivery was stuck below 100% and the cause was
not in the audio path at all — `host_pit_sync` ran only 65×/s, so timer ticks came out in
*bursts*, so the game's DMA refill arms collapsed. **Pacing the PIT fixed the audio.** When
a symptom is in one subsystem, the cause is often in the clock.

---

## Windows 3.x

A Windows 3.x program runs as guest code too. NTVDMEX starts XP's own 16-bit `krnl386.exe`,
`user.exe` and `gdi.exe` from `system32` as the guest's Windows -- nothing of Microsoft's is
shipped -- and implements the 32-bit half they call out to: the WOW32 interface. Each call
from 16-bit USER or GDI arrives as a BOP with a module and an id (the call tables are in
`src/wow/wowuser_calls.h` and `wowgdi_calls.h`) and is answered by the matching Win32 call.

- **A Windows 3.x window is a real Win32 window**, owned by the host's thread, so it is moved,
  painted and themed by XP like any other. Messages are translated in both directions.
- **The host calls back into 16-bit code** -- window procedures, dialog procedures, enumeration
  callbacks -- by running the guest until the procedure returns (`wowcall.c`).
- **Tasks are scheduled cooperatively**, as Windows 3.x did, with a task per program.
