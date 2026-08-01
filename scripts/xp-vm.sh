#!/usr/bin/env bash
#
# xp-vm.sh -- the NTVDMEX test bench: Windows XP Professional (32-bit) under
# QEMU, hardware-accelerated through Hypervisor.framework (HVF) on this Intel
# Mac. XP runs on the real CPU at near-native speed, which also means genuine
# Virtual-8086 mode is available -- exactly what NTVDMEX needs to test against.
#
# The virtual machine approximates a late-1990s / early-2000s PC -- the class of
# hardware a Windows XP box running DOS/Win16 software actually had -- and
# deliberately uses devices XP supports out of the box (no F6 driver dance).
# Rationale for each choice is in docs/research/xp-test-vm.md.
#
# Usage:
#   ./scripts/xp-vm.sh install [iso]   # boot the installer (CD first)
#   ./scripts/xp-vm.sh run             # boot the installed disk
#   ./scripts/xp-vm.sh info            # print config + host<->guest port map
#
# The disk image and the ISO live OUTSIDE git on purpose (see .gitignore).
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VM_DIR="$ROOT/vm"
DISK="$VM_DIR/xp.qcow2"
ISO_DEFAULT="$ROOT/ms-windows-xp-professional-x86.iso"
TRANSFER_ISO="$VM_DIR/transfer.iso"   # read-only disc of host->guest binaries (mode: transfer)

DISK_SIZE="32G"      # PATA/IDE disk; qcow2 is sparse, so this is a ceiling
RAM_MB="1024"        # comfortable for XP (period machines ran 256MB-2GB)
VM_NAME="NTVDMEX-XP"

# Host ports forwarded into the guest, ready for later automation: XP's built-in
# Telnet service, or an SSH/FTP server you install. Nothing listens yet -- the
# forwards are harmless until something in the guest binds those ports.
HOSTFWD="hostfwd=tcp::2323-:23,hostfwd=tcp::2222-:22,hostfwd=tcp::2121-:21"

mode="${1:-info}"
iso="${2:-$ISO_DEFAULT}"

# Build the common QEMU command line into the QARGS array. $1 = -boot value.
build_args() {
    QARGS=(
        -name "$VM_NAME"

        # --- platform: period chipset, real silicon ---------------------------
        -accel hvf                 # Hypervisor.framework: native-speed x86
        -machine pc                # i440FX + PIIX3 southbridge + legacy SeaBIOS
        -cpu host                  # pass the real CPU through -> genuine V86 mode
        -smp 1                     # uniprocessor: avoids XP's multiproc-HAL hassle
        -m "$RAM_MB"
        -rtc base=localtime        # Windows keeps the RTC in local time, not UTC
        -boot "$1"

        # --- storage: PATA/IDE (XP has the driver built in; SATA would need F6) -
        -drive file="$DISK",if=ide,format=qcow2,cache=writeback

        # --- video: Cirrus GD5446, an era card XP drives with no extra driver --
        -vga cirrus

        # --- sound: Sound Blaster 16 (ISA) -- what DOS audio code targets, and
        #     the card our future sound VDD must emulate. IRQ 5 / DMA 1,5 are the
        #     canonical SB16 settings. -----------------------------------------
        -audiodev coreaudio,id=snd0
        -device sb16,audiodev=snd0

        # --- input: PS/2 (default) + a USB tablet for absolute, ungrabbed mouse -
        -usb -device usb-tablet

        # --- network: Realtek RTL8139, ubiquitous in the era, built-in XP driver;
        #     user-mode NAT with host port forwards for later automation. -------
        # tftp= serves build/ to the guest so the host can push fresh binaries:
        #   (guest)  tftp -i 10.0.2.2 GET vdmhost.exe C:\ntvdmex\vdmhost.exe
        -netdev "user,id=net0,$HOSTFWD,tftp=$ROOT/build"
        -device rtl8139,netdev=net0

        -display cocoa

        # --- COM1 -> host file: the DPMI/test harness log sink. ntvdmhost writes its
        #     log to \\.\COM1; QEMU appends the bytes to vm/serial.log so the host reads
        #     the log directly (no GUI screendump, no stale-host ambiguity). ---------
        -serial "file:$VM_DIR/serial.log"

        # --- host control socket: lets the host drive the VM (take snapshots via
        #     'savevm', hot-swap the transfer CD, send keys) without the GUI. ---
        -qmp "unix:$VM_DIR/qmp.sock,server=on,wait=off"
    )
}

case "$mode" in
    install)
        [ -f "$iso" ] || { echo "ISO not found: $iso" >&2; exit 1; }
        mkdir -p "$VM_DIR"
        if [ ! -f "$DISK" ]; then
            echo "Creating disk: $DISK ($DISK_SIZE, sparse)"
            qemu-img create -f qcow2 "$DISK" "$DISK_SIZE" >/dev/null
        fi
        build_args "order=dc,menu=on"   # CD first; falls through to disk on reboot
        echo "Booting the XP installer from: $iso"
        echo "TIP: on the mid-install reboot, do NOT press a key at 'Press any key"
        echo "     to boot from CD' -- letting it time out boots the new disk."
        exec qemu-system-x86_64 "${QARGS[@]}" -cdrom "$iso"
        ;;

    run)
        [ -f "$DISK" ] || { echo "No disk yet. Install first:  $0 install" >&2; exit 1; }
        build_args "order=c,menu=on"    # boot the installed disk
        # Mount the transfer disc as the CD if it exists (built by 'transfer'),
        # otherwise fall back to the install ISO for convenience.
        cd_iso="$TRANSFER_ISO"; [ -f "$cd_iso" ] || cd_iso="$iso"
        exec qemu-system-x86_64 "${QARGS[@]}" ${cd_iso:+-cdrom "$cd_iso"}
        ;;

    transfer)
        # (Re)build the transfer disc: a read-only CD carrying the current binaries
        # plus a one-click setup script. Mounts as the CD (D:) in 'run' mode.
        # Rebuild after each code change. (Host->guest only; results come back via
        # the wowprobe MessageBox/log for now.)
        mkdir -p "$VM_DIR"
        [ -f "$ROOT/build/wowprobe.exe" ] && [ -f "$ROOT/build/ntvdmex.exe" ] || {
            echo "Build the binaries first:  ./scripts/build.sh" >&2; exit 1; }
        stage="$(mktemp -d)"
        trap 'rm -rf "$stage"' EXIT
        cp "$ROOT/build/ntvdmex.exe" "$ROOT/build/wowprobe.exe" "$stage"/
        [ -f "$ROOT/build/vdmhost.exe" ] && cp "$ROOT/build/vdmhost.exe" "$stage"/
        cp "$ROOT/tools/wowprobe/dosstub.com" "$stage"/
        [ -f "$ROOT/tools/wowprobe/dosstub.exe" ] && cp "$ROOT/tools/wowprobe/dosstub.exe" "$stage"/
        cat > "$stage/spike002-setup.cmd" <<'CMD'
@echo off
echo === NTVDMEX Spike-002 setup (use an Administrator account) ===
if not exist C:\ntvdmex md C:\ntvdmex
copy /y "%~dp0wowprobe.exe" C:\ntvdmex\ >nul
copy /y "%~dp0dosstub.com"  C:\ntvdmex\ >nul
copy /y "%~dp0dosstub.exe"  C:\ntvdmex\ >nul
copy /y "%~dp0ntvdmex.exe"  C:\ntvdmex\ >nul
if exist C:\ntvdmex\wow-backup.reg del /q C:\ntvdmex\wow-backup.reg
echo Backing up the WOW key to C:\ntvdmex\wow-backup.reg ...
reg export "HKLM\SYSTEM\CurrentControlSet\Control\WOW" C:\ntvdmex\wow-backup.reg
echo.
echo DEFAULT cmdline value (please record this):
reg query "HKLM\SYSTEM\CurrentControlSet\Control\WOW" /v cmdline
echo.
echo Repointing cmdline at wowprobe.exe ...
reg add "HKLM\SYSTEM\CurrentControlSet\Control\WOW" /v cmdline /t REG_EXPAND_SZ /d "C:\ntvdmex\wowprobe.exe" /f
echo NEW value (must show C:\ntvdmex\wowprobe.exe):
reg query "HKLM\SYSTEM\CurrentControlSet\Control\WOW" /v cmdline
echo.
echo Done. Now REBOOT, then run  C:\ntvdmex\dosstub.com
echo Restore later with:  reg import C:\ntvdmex\wow-backup.reg
pause
CMD
        cat > "$stage/spike001-setup.cmd" <<'CMD'
@echo off
echo === NTVDMEX Spike-001 setup: repoint cmdline at vdmhost.exe (admin) ===
if not exist C:\ntvdmex md C:\ntvdmex
copy /y "%~dp0vdmhost.exe" C:\ntvdmex\ >nul
copy /y "%~dp0dosstub.com" C:\ntvdmex\ >nul
copy /y "%~dp0dosstub.exe" C:\ntvdmex\ >nul
if exist C:\ntvdmex\wow-backup.reg del /q C:\ntvdmex\wow-backup.reg
reg export "HKLM\SYSTEM\CurrentControlSet\Control\WOW" C:\ntvdmex\wow-backup.reg
reg add "HKLM\SYSTEM\CurrentControlSet\Control\WOW" /v cmdline /t REG_EXPAND_SZ /d "C:\ntvdmex\vdmhost.exe" /f
reg query "HKLM\SYSTEM\CurrentControlSet\Control\WOW" /v cmdline
echo.
echo Done. Now REBOOT, then run a 16-bit program: C:\ntvdmex\dosstub.com (or .exe)
echo vdmhost will call GetNextVDMCommand and write C:\ntvdmex\vdmhost.log
echo Restore later with:  reg import C:\ntvdmex\wow-backup.reg
pause
CMD
        cat > "$stage/enable-telnet.cmd" <<'CMD'
@echo off
echo === NTVDMEX: enable Telnet so the host can drive this VM (run as admin) ===
net user ntvdmex ntvdmex /add
net localgroup Administrators ntvdmex /add
sc config TlntSvr start= auto
net start TlntSvr
net localgroup TelnetClients ntvdmex /add
tlntadmn config sec = -NTLM
tlntadmn config mode = stream
netsh firewall add portopening TCP 23 Telnet ENABLE ALL
echo.
echo Done. Host connects on port 2323 as ntvdmex/ntvdmex; auto-starts on boot.
pause
CMD
        cat > "$stage/README.txt" <<'TXT'
NTVDMEX transfer disc
  spike002-setup.cmd : copies binaries to C:\ntvdmex, backs up + repoints the WOW
                       cmdline key. Run it (admin), REBOOT, then run
                       C:\ntvdmex\dosstub.com. Watch for the wowprobe MessageBox
                       and C:\ntvdmex\wowprobe.log.
  dosstub.com / .exe : DOS triggers (.com is headerless; .exe is a 16-bit MZ).
                       If one is rejected as "not a valid Win32 application",
                       try the other.
  ntvdmex.exe        : the Luna shell preview -- double-click to sanity-check.
TXT
        rm -f "$TRANSFER_ISO"
        hdiutil makehybrid -iso -joliet -default-volume-name NTVDMEX \
            -o "$TRANSFER_ISO" "$stage" >/dev/null
        echo "Built $TRANSFER_ISO"
        echo "Launch the VM with:  $0 run   (the disc appears as the CD / D:)"
        ;;

    info)
        cat <<EOF
NTVDMEX test VM -- Windows XP Professional (32-bit), QEMU + HVF
  disk : $DISK  ($DISK_SIZE max, created on first 'install')
  iso  : $ISO_DEFAULT
  ram  : ${RAM_MB} MB     cpu: host (HVF, real V86)     smp: 1
  net  : user/NAT, host->guest  localhost:2323->23 (telnet)
                                localhost:2222->22 (ssh)
                                localhost:2121->21 (ftp)

Commands:
  $0 install [iso]    boot the installer
  $0 transfer         (re)build the transfer disc from build/ + tools/
  $0 run              boot the installed disk (mounts the transfer disc as CD)

Save / restore VM state (VM must be powered off for -a):
  qemu-img snapshot -c <name> "$DISK"    create snapshot
  qemu-img snapshot -l        "$DISK"    list
  qemu-img snapshot -a <name> "$DISK"    restore
EOF
        ;;

    *)
        echo "usage: $0 {install [iso]|transfer|run|info}" >&2
        exit 1
        ;;
esac
