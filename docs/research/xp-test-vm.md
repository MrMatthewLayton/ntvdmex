# The XP test VM — period-correct hardware, and why

The NTVDMEX dev loop runs Windows XP Pro (32-bit) as a QEMU guest on the Intel Mac, via
`scripts/xp-vm.sh`. This note explains the hardware the VM emulates and the reasoning, since
"approximately period-correct" pulls in two directions: **authentic to the era** vs. **what XP
installs on without a driver hunt** vs. **what NTVDMEX actually needs to test**. Where those
conflict, the call is recorded here.

## Hard requirements (non-negotiable)
- **32-bit x86 with real V86 mode.** The whole project executes 16-bit code on the *real* CPU in
  Virtual-8086 mode (ADR-0001). HVF passes this Intel Xeon through to the guest, so V86 is
  genuinely available — emulation (TCG) would defeat the point. `kern.hv_support = 1` here, so
  HVF acceleration is on.
- **Legacy BIOS, not UEFI.** XP predates UEFI; SeaBIOS (QEMU default) is correct.
- **Devices XP can drive unaided.** Every device below is one XP has an in-box driver for, so
  setup needs no F6 floppy / slipstream step.

## The chosen machine
| Component | Emulated as | Why |
|-----------|-------------|-----|
| Chipset | **i440FX + PIIX3** (`-machine pc`) | The canonical Pentium II/III-era chipset; what XP expects. Q35 (2009, ICH9) is too new and needs SATA drivers XP lacks. |
| CPU | **host, via HVF** (`-cpu host`) | Real silicon → real V86. See the tension below. |
| CPUs | **1** (`-smp 1`) | Uniprocessor sidesteps XP's ACPI multiprocessor-HAL switch; period-plausible. |
| RAM | **1024 MB** | Era machines ran 256 MB–2 GB; 1 GB is comfortable and well within XP-32's ~3.25 GB ceiling. |
| Disk | **PATA/IDE qcow2** (`if=ide`) | IDE is in-box on XP; SATA/AHCI (≈2004+) would need F6 drivers. qcow2 is sparse (32 GB ceiling). |
| Video | **Cirrus GD5446** (`-vga cirrus`) | A real era card with a built-in XP driver (1024×768). `std`/VBE is a fallback; QXL/virtio are too new. |
| Sound | **Sound Blaster 16** (`-device sb16`, IRQ 5 / DMA 1,5) | The card DOS audio code targets directly, and the one our future sound VDD (M7) must emulate. AC'97 is the period *Windows* option; we can add it later for Win-side audio. |
| Network | **Realtek RTL8139** (`-device rtl8139`) | Ubiquitous era NIC with an in-box XP driver. user-mode NAT + host port-forwards for automation. |
| Input | **PS/2 + USB tablet** | PS/2 is period-pure; the USB tablet (USB 1.1 / UHCI, also era-appropriate) gives an absolute, ungrabbed mouse for usability. |
| Clock | **RTC in local time** (`-rtc base=localtime`) | Windows reads the hardware clock as local time, not UTC. |

## The one real tension: CPU
"Period-correct" would mean a Pentium III/4. We deliberately **don't** do that:
- HVF accelerates by running guest code on the host CPU; it does not faithfully impersonate a
  1999 Pentium. Forcing an old `-cpu` model tends to drop HVF back to slow emulation.
- More importantly, NTVDMEX's premise is **execution on the real CPU**. Testing against real,
  current silicon in V86 is the honest target, not a synthetic old core. V86 semantics are
  identical across x86 generations in 32-bit mode, so this costs us nothing for correctness.
- The place period-accuracy genuinely matters is *timing-sensitive DOS software* (code that
  busy-loops assuming a slow CPU). That's a guest-software concern we'll address later with
  timing virtualization in the DOS layer — not something the VM chipset choice fixes.

If a specific title needs it, `-cpu` can be pinned (e.g. `Penryn`) at the price of speed.

## File & command bridge (how the dev loop closes)
- **v1 — transfer disc (in use).** `./scripts/xp-vm.sh transfer` builds a read-only CD
  (`vm/transfer.iso`, via `hdiutil`) carrying the current binaries plus a one-click
  `spike002-setup.cmd`; `run` mounts it as the guest CD. Host→guest only; results come back via
  the wowprobe MessageBox / `C:\ntvdmex\wowprobe.log`. Chosen because it needs nothing installed
  (QEMU's user-mode SMB wants Samba's `smbd` at `/usr/local/sbin/samba-dot-org-smbd`, absent
  here — Apple's `/usr/sbin/smbd` is incompatible).
- **Host control via QMP.** The launcher exposes `vm/qmp.sock`, so the host can drive the running
  VM — take live snapshots (`savevm`), hot-swap the transfer CD without rebooting, send keys —
  rather than only controlling it from the GUI.
- **v2 — Telnet control (set up).** XP's built-in Telnet server, reached over the forwarded port
  (`localhost:2323`). One-time enable in the guest: run `D:\enable-telnet.cmd` (creates an
  `ntvdmex`/`ntvdmex` admin account, starts `TlntSvr` auto, disables NTLM so password auth works,
  sets stream mode). Then drive XP from the host with **`scripts/xp.py`** (a minimal telnet client,
  since macOS has no `telnet` and Python 3.14 dropped `telnetlib`):
  ```
  ./scripts/xp.py "reg query \"HKLM\\SYSTEM\\CurrentControlSet\\Control\\WOW\" /v cmdline"
  ./scripts/xp.py "type C:\\ntvdmex\\vdmhost.log"
  ```
  This closes the loop: I can check the registry, read logs, set values, trigger programs, and
  `shutdown -r` (then reconnect after the auto-started service comes back) — no more
  screenshot-the-file round-trips.

## Not in git
The **ISO** (`ms-windows-xp-professional-x86.iso`) and the **disk image** (`vm/`) are gitignored
— large, and the media is licensed. Only the launcher script is tracked.

## Usage
```
./scripts/xp-vm.sh install     # boot installer from the repo-root ISO
./scripts/xp-vm.sh run         # boot the installed disk
./scripts/xp-vm.sh info        # config + host->guest port map
```
Snapshots via `qemu-img snapshot -c/-l/-a vm/xp.qcow2` — take one right after a clean install so
spikes that touch the registry/system can be rolled back instantly.
