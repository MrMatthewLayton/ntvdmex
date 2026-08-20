#!/usr/bin/env python3
"""Build the MS-DOS 6.22 oracle disk image.  GH #25.

Installs genuine MS-DOS 6.22 from the four retail floppy images in ./msdos-622
onto a 504 MB FAT16 hard-disk image at vm/dos622.img, without ever driving
SETUP.EXE's interactive screens.

WHY NOT SETUP.EXE: SETUP is a full-screen interactive installer that prompts for
a target directory, prompts on disk swaps, and rewrites CONFIG.SYS/AUTOEXEC.BAT
to taste.  Driving it means synthesising keystrokes against screen state, which
is exactly the kind of timing-fragile loop that produces a different disk every
time it runs.  We do not need SETUP: PACKING.LST on Disk 1 is Microsoft's own
authoritative compressed-name -> expanded-name table for all three disks, so the
install is a deterministic list of EXPAND/COPY commands.  Every pass is a
non-interactive QEMU launch driven by a generated AUTOEXEC.BAT, and the result
is byte-reproducible.

THE FILES ARE THE GENUINE MICROSOFT BINARIES either way -- we are choosing how
to unpack them, not what to install.

Passes (each one boot of QEMU, each ending in QUIT.COM):
  1  FDISK /MBR, FORMAT C: /S, install Disk 1        (control floppy in A:)
  2  install Disk 2                                   (source in B:)
  3  install Disk 3                                   (source in B:)
  4  install the supplemental disk                    (source in B:, optional)
  5  write the oracle's own CONFIG.SYS / AUTOEXEC.BAT

Usage:  python3 scripts/dosoracle/build.py [--keep-going] [--skip-supp]
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MEDIA = os.path.join(ROOT, "msdos-622")
VM = os.path.join(ROOT, "vm")
HERE = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(HERE, "_build")

IMG = os.path.join(VM, "dos622.img")

# 1024 x 16 x 63 -- the largest geometry that stays inside the 1024-cylinder BIOS
# limit, so no LBA translation is involved and the CHS in the partition table is
# honest.  504 MB, comfortably inside FAT16's 2 GB ceiling.
CYLS, HEADS, SECS = 1024, 16, 63
TOTAL_SECTORS = CYLS * HEADS * SECS
PART_START = 63                       # track 0 reserved for the MBR, as DOS expects
PART_SECTORS = TOTAL_SECTORS - PART_START
PART_OFFSET = PART_START * 512        # byte offset of the FAT16 volume, for mtools

QUIT_EXIT_CODE = 85                   # (0x2A << 1) | 1, see quit.asm

MTOOLS_ENV = dict(os.environ, MTOOLS_SKIP_CHECK="1")


def run(cmd, **kw):
    """Run a command, raising with the captured output if it fails."""
    p = subprocess.run(cmd, capture_output=True, text=True, env=MTOOLS_ENV, **kw)
    if p.returncode != 0:
        raise RuntimeError(
            "command failed (%d): %s\n%s\n%s" % (p.returncode, " ".join(cmd), p.stdout, p.stderr)
        )
    return p.stdout


def mdir(image, offset=None):
    """Return the 8.3 names present in the root of a FAT image."""
    target = "%s@@%d" % (image, offset) if offset else image
    out = run(["mdir", "-i", target, "::"])
    names = []
    for line in out.splitlines():
        m = re.match(r"^([A-Z0-9_~!@#$%^&()\-{}']{1,8})\s+([A-Z0-9_]{0,3})\s+\d", line)
        if m:
            base, ext = m.group(1).strip(), m.group(2).strip()
            names.append("%s.%s" % (base, ext) if ext else base)
    return names


def mtype(image, path):
    return run(["mtype", "-i", image, "::/" + path])


# ---------------------------------------------------------------- packing list

def parse_packing_list(text):
    """Microsoft's own compressed -> expanded name table, per disk.

    PACKING.LST is the authority here and it is not guessable: MONOUMB.38_
    expands to MONOUMB.386 but WNTOOLS.GR_ expands to WNTOOLS.GRB, and on the
    supplemental disk GORILLA.BA_ is a .BAS while DRVBOOT.BA_ is a .BAT.  Deriving
    names from the underscore would silently install wrongly-named files.
    """
    disks = {}
    current = None
    for line in text.splitlines():
        m = re.match(r"^Setup Disk #(\d)", line.strip())
        if m:
            current = int(m.group(1))
            disks[current] = {}
            continue
        if current is None:
            continue
        m = re.match(r"^([A-Z0-9_\-]{1,8}\.[A-Z0-9_]{1,3})\s+([A-Z0-9_\-]{1,8}\.[A-Z0-9_]{1,3})\s*$", line)
        if m:
            disks[current][m.group(1)] = m.group(2)
    return disks


# The supplemental disk ships no PACKING.LST, so its mapping comes from the disk's
# OWN INSTALL SCRIPT -- SETUP.BAT calls SD6COPY.BAT with the *expanded* name of
# every file it installs.  These rules are read off that list, not guessed:
#
#   NET.1X_  -> NET.1XE     REDIR.2X_ -> REDIR.2XE   NETBEUI.DO_ -> NETBEUI.DOS
#   CGA.GR_  -> CGA.GRB     CGA.IN_   -> CGA.INI     CGA.VI_     -> CGA.VID
#
# .BA_ is genuinely ambiguous and is the reason none of this is derived from the
# underscore: SETUP.BAT installs GORILLA/MONEY/NIBBLES/REMLINE as .BAS but
# DRVBOOT as .BAT.
SUPP_SUFFIX = {
    "CO_": "COM", "EX_": "EXE", "SY_": "SYS", "TX_": "TXT", "OV_": "OVL",
    "CP_": "CPI", "HL_": "HLP", "GR_": "GRB", "IN_": "INI", "VI_": "VID",
    "DO_": "DOS", "1X_": "1XE", "2X_": "2XE", "DL_": "DLL", "PR_": "PRO",
    "LS_": "LST", "38_": "386",
}
SUPP_OVERRIDE = {
    "DRVBOOT.BA_": "DRVBOOT.BAT",       # a batch file; the other .BA_ are BASIC
    "GORILLA.BA_": "GORILLA.BAS", "MONEY.BA_": "MONEY.BAS",
    "NIBBLES.BA_": "NIBBLES.BAS", "REMLINE.BA_": "REMLINE.BAS",
}


def supp_map(files):
    """Expand-name table for the supplemental disk, plus anything unresolved."""
    table, unresolved = {}, []
    for name in files:
        if not name.split(".")[-1].endswith("_"):
            continue
        if name in SUPP_OVERRIDE:
            table[name] = SUPP_OVERRIDE[name]
            continue
        base, ext = name.rsplit(".", 1)
        if ext in SUPP_SUFFIX:
            table[name] = "%s.%s" % (base, SUPP_SUFFIX[ext])
        else:
            unresolved.append(name)
    return table, unresolved


# ------------------------------------------------------------------- disk image

def make_blank_image(path):
    """Create the raw image and write an MBR with one FAT16 primary partition.

    We build the partition table on the host rather than driving FDISK's menus.
    FDISK /MBR (pass 1) then installs Microsoft's real MBR boot code into the
    code area, which it does non-interactively and which preserves this table.
    """
    with open(path, "wb") as f:
        f.truncate(TOTAL_SECTORS * 512)

    mbr = bytearray(512)
    #                    bootable, start CHS (c=0,h=1,s=1)
    entry = bytearray([0x80, 0x01, 0x01, 0x00,
                       0x06,                      # FAT16, >32 MB, CHS
                       HEADS - 1,                 # end head
                       (SECS & 0x3F) | ((CYLS - 1) >> 2 & 0xC0),
                       (CYLS - 1) & 0xFF])
    entry += PART_START.to_bytes(4, "little")
    entry += PART_SECTORS.to_bytes(4, "little")
    mbr[446:446 + 16] = entry
    mbr[510:512] = b"\x55\xaa"

    with open(path, "r+b") as f:
        f.write(mbr)


# ---------------------------------------------------------------- control disk

def build_control_floppy(dest, autoexec, extra_files=()):
    """A bootable copy of Disk 1 carrying the batch that drives one pass.

    Disk 1 is already bootable and already carries FORMAT/FDISK/SYS/EXPAND, so
    it is the natural control disk -- we only replace its AUTOEXEC.BAT (which
    otherwise launches SETUP) and drop in QUIT.COM.
    """
    shutil.copyfile(os.path.join(MEDIA, "Disk1.img"), dest)

    auto = os.path.join(BUILD, "AUTOEXEC.BAT")
    with open(auto, "w", newline="\r\n") as f:
        f.write(autoexec)

    # "Y\r\n" answers FORMAT's "Proceed with Format (Y/N)?".
    yes = os.path.join(BUILD, "YES.TXT")
    with open(yes, "wb") as f:
        f.write(b"Y\r\n" * 4)

    run(["mdel", "-i", dest, "::/AUTOEXEC.BAT"])
    run(["mcopy", "-i", dest, "-o", auto, "::/AUTOEXEC.BAT"])
    run(["mcopy", "-i", dest, "-o", yes, "::/YES.TXT"])
    run(["mcopy", "-i", dest, "-o", os.path.join(BUILD, "QUIT.COM"), "::/QUIT.COM"])
    for src, name in extra_files:
        run(["mcopy", "-i", dest, "-o", src, "::/" + name])


# ----------------------------------------------------------------------- passes

def install_batch(drive, files, packing):
    """EXPAND every compressed file and COPY the rest, one line per file."""
    lines = []
    for name in sorted(files):
        if name in ("IO.SYS", "MSDOS.SYS", "COMMAND.COM",
                    "AUTOEXEC.BAT", "CONFIG.SYS", "YES.TXT", "QUIT.COM"):
            continue                      # the boot files come from FORMAT /S
        target = packing.get(name, name)
        if name.split(".")[-1].endswith("_"):
            lines.append("EXPAND %s:\\%s C:\\DOS\\%s >> A:\\BUILD.LOG" % (drive, name, target))
        else:
            lines.append("COPY %s:\\%s C:\\DOS\\%s >> A:\\BUILD.LOG" % (drive, name, target))
    return "\n".join(lines)


def qemu(args, floppy0, floppy1=None, boot="a", timeout=240, label=""):
    cmd = [
        "qemu-system-i386", "-M", "pc", "-m", "16",
        "-drive", "file=%s,format=raw,if=ide,index=0,media=disk" % IMG,
        "-drive", "file=%s,format=raw,if=floppy,index=0" % floppy0,
    ]
    if floppy1:
        cmd += ["-drive", "file=%s,format=raw,if=floppy,index=1" % floppy1]
    cmd += [
        "-boot", boot,
        "-display", "none",
        "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
        "-no-reboot",
    ]
    if args.verbose:
        print("    $ %s" % " ".join(cmd))
    t0 = time.time()
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        rc = p.returncode
    except subprocess.TimeoutExpired:
        raise RuntimeError("%s: QEMU did not exit within %ds -- the guest never "
                           "reached QUIT.COM (see BUILD.LOG)" % (label, timeout))
    dt = time.time() - t0
    if rc != QUIT_EXIT_CODE:
        raise RuntimeError("%s: QEMU exited %d, expected %d (guest did not reach "
                           "QUIT.COM cleanly)\n%s" % (label, rc, QUIT_EXIT_CODE, p.stderr))
    return dt


def read_build_log(floppy):
    try:
        return mtype(floppy, "BUILD.LOG")
    except RuntimeError:
        return "(no BUILD.LOG)"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--skip-supp", action="store_true",
                    help="skip the optional supplemental disk")
    ap.add_argument("--verbose", "-v", action="store_true")
    args = ap.parse_args()

    for d in (VM, BUILD):
        os.makedirs(d, exist_ok=True)

    for n in ("Disk1.img", "Disk2.img", "Disk3.img"):
        if not os.path.exists(os.path.join(MEDIA, n)):
            sys.exit("missing install media: %s" % os.path.join(MEDIA, n))

    print("== assembling QUIT.COM")
    run(["nasm", "-f", "bin", os.path.join(HERE, "quit.asm"),
         "-o", os.path.join(BUILD, "QUIT.COM")])

    print("== reading PACKING.LST (Microsoft's own name table)")
    packing = parse_packing_list(mtype(os.path.join(MEDIA, "Disk1.img"), "PACKING.LST"))
    for n in (1, 2, 3):
        if n not in packing or not packing[n]:
            sys.exit("could not parse Setup Disk #%d out of PACKING.LST" % n)
        print("   disk %d: %d mapped names" % (n, len(packing[n])))

    print("== creating %s (%d MB, C/H/S %d/%d/%d)"
          % (os.path.relpath(IMG, ROOT), TOTAL_SECTORS * 512 // (1024 * 1024), CYLS, HEADS, SECS))
    make_blank_image(IMG)

    ctrl = os.path.join(BUILD, "control.img")

    # -- pass 1: partition boot code, format, install disk 1 -------------------
    print("== pass 1: FDISK /MBR + FORMAT C: /S + Setup Disk #1")
    files1 = mdir(os.path.join(MEDIA, "Disk1.img"))
    batch1 = "\n".join([
        "@ECHO OFF",
        "ECHO --- pass 1 > A:\\BUILD.LOG",
        "FDISK /MBR >> A:\\BUILD.LOG",
        "FORMAT C: /S /U /V:DOS622 < A:\\YES.TXT >> A:\\BUILD.LOG",
        "IF NOT EXIST C:\\COMMAND.COM GOTO FAIL",
        "MD C:\\DOS >> A:\\BUILD.LOG",
        install_batch("A", files1, packing[1]),
        "ECHO --- pass 1 done >> A:\\BUILD.LOG",
        ":FAIL",
        "QUIT",
        "",
    ])
    build_control_floppy(ctrl, batch1)
    dt = qemu(args, ctrl, label="pass 1", timeout=600)
    print("   %.1fs" % dt)

    if "COMMAND.COM" not in mdir(IMG, PART_OFFSET):
        print(read_build_log(ctrl))
        sys.exit("pass 1: FORMAT C: /S did not produce a bootable C:")
    print("   C: is formatted and bootable")

    # -- passes 2..4: the remaining disks in B: --------------------------------
    later = [(2, "Disk2.img", packing[2]), (3, "Disk3.img", packing[3])]
    if not args.skip_supp and os.path.exists(os.path.join(MEDIA, "Suppdisk.img")):
        later.append((4, "Suppdisk.img", None))       # table derived below

    for n, media_name, table in later:
        label = "Setup Disk #%d" % n if n < 4 else "supplemental disk"
        print("== pass %d: %s" % (n, label))
        src = os.path.join(BUILD, "src%d.img" % n)
        shutil.copyfile(os.path.join(MEDIA, media_name), src)
        files = mdir(src)
        if table is None:
            table, unresolved = supp_map(files)
            if unresolved:
                # Never drop files silently -- an install that quietly skipped
                # things would read as complete when it is not.
                print("   SKIPPING %d file(s) with no authoritative expanded "
                      "name: %s" % (len(unresolved), ", ".join(sorted(unresolved))))
                files = [f for f in files if f not in unresolved]
        batch = "\n".join([
            "@ECHO OFF",
            "ECHO --- pass %d >> A:\\BUILD.LOG" % n,
            install_batch("B", files, table),
            "ECHO --- pass %d done >> A:\\BUILD.LOG" % n,
            "QUIT",
            "",
        ])
        build_control_floppy(ctrl, batch)
        dt = qemu(args, ctrl, floppy1=src, label="pass %d" % n, timeout=600)
        print("   %.1fs, %d files" % (dt, len(files)))

    # -- pass 5: the oracle's own boot configuration ---------------------------
    print("== pass 5: writing CONFIG.SYS / AUTOEXEC.BAT")
    config = "\r\n".join([
        "DEVICE=C:\\DOS\\HIMEM.SYS",
        "DOS=HIGH",
        "FILES=40",
        "BUFFERS=20",
        "SHELL=C:\\COMMAND.COM C:\\ /P /E:512",
        "",
    ])
    autoexec = "\r\n".join([
        "@ECHO OFF",
        "PATH C:\\DOS",
        "SET COMSPEC=C:\\COMMAND.COM",
        "PROMPT $P$G",
        "REM -- the harness appends its run here (see dosoracle.py)",
        "IF EXIST A:\\RUN.BAT CALL A:\\RUN.BAT",
        "",
    ])
    for name, text in (("CONFIG.SYS", config), ("AUTOEXEC.BAT", autoexec)):
        p = os.path.join(BUILD, name)
        with open(p, "wb") as f:
            f.write(text.encode("ascii"))
        run(["mcopy", "-i", "%s@@%d" % (IMG, PART_OFFSET), "-o", p, "::/" + name])
    run(["mcopy", "-i", "%s@@%d" % (IMG, PART_OFFSET), "-o",
         os.path.join(BUILD, "QUIT.COM"), "::/DOS/QUIT.COM"])

    root = mdir(IMG, PART_OFFSET)
    print("\n== built %s" % os.path.relpath(IMG, ROOT))
    print("   root: %s" % ", ".join(sorted(root)))
    print("   run  scripts/dosoracle/dosoracle.py --selftest  to verify it boots")


if __name__ == "__main__":
    main()
