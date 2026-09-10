#!/usr/bin/env python3
"""lemref.py -- a REFERENCE PICTURE of Lemmings gameplay from genuine MS-DOS 6.22.

Why this exists rather than another test card.  The cards proved, one path at a
time, that write mode 2, write mode 0 per-plane, Set/Reset with OR, partial Enable
Set/Reset, the Attribute Controller and the CRTC are all pixel-identical to real
hardware -- and Lemmings' gameplay STILL has a striped band.  Guessing the next
untested path is how the last three cards got written; a reference picture of the
actual failing screen says which pixels are wrong without any guess at all.

scripts/dosoracle cannot do this: it delivers a host directory as a READ-ONLY
floppy, and Lemmings writes its config (russell.dat) at startup.  So this drives
QEMU directly with a WRITABLE floppy, uses the HMP monitor to press the keys that
get past the sound menu and into a level, and screendumps.

    lemref.py --game <dir> --out ref.ppm [--keys "ret,f1,ret,spc" ] [--settle 25]

The monitor is on stdio (a unix socket cannot be bound in the dev sandbox -- the
same constraint dosoracle documents), so commands go to the child's stdin.
"""
import argparse
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMG = os.path.join(ROOT, "vm", "dos622.img")


def run(cmd):
    p = subprocess.run(cmd, capture_output=True)
    if p.returncode:
        sys.exit("%s failed: %s" % (cmd[0], p.stderr.decode(errors="replace")))


PART_OFFSET = 63 * 512          # same as dosoracle: the C: image is partitioned


def make_hd(scratch, gamedir):
    """Install the game into C:\GAME on a COPY of the DOS image.

    ⚠ NOT A FLOPPY. Lemmings refuses to run from one -- it prints "Lemmings Disk 1
    Not found, Insert into drive A or B and retry" and exits, because the original
    shipped on floppies and checks which disk it is on. It runs happily from a hard
    disk, which is how the rig runs it too, so the reference has to match.
    ⚠ A COPY, because this writes into the image. The real oracle image must not be
    touched -- dosoracle relies on it being pristine and runs it snapshot=on for
    exactly that reason."""
    import shutil
    shutil.copyfile(IMG, scratch)
    drv = "%s@@%d" % (scratch, PART_OFFSET)
    run(["mmd", "-i", drv, "::/GAME"])
    for f in sorted(os.listdir(gamedir)):
        src = os.path.join(gamedir, f)
        if os.path.isfile(src):
            run(["mcopy", "-i", drv, "-o", src, "::/GAME/" + f.upper()])
    return drv


def make_disk(path, gamedir, exe):
    """A 1.44 MB writable FAT image holding the game and a RUN.BAT that starts it."""
    files = [f for f in sorted(os.listdir(gamedir))
             if os.path.isfile(os.path.join(gamedir, f))]
    total = sum(os.path.getsize(os.path.join(gamedir, f)) for f in files)
    if total > 1395 * 1024:
        sys.exit("%s holds %.2f MB; a floppy tops out at ~1.40 MB usable. Trim it."
                 % (gamedir, total / 1048576.0))
    with open(path, "wb") as f:
        f.truncate(1474560)
    run(["mformat", "-i", path, "-f", "1440", "-v", "LEMREF", "::"])
    bat = path + ".bat"
    # A: is where DOS's AUTOEXEC looks for RUN.BAT (see dosoracle), so the game
    # lives on the same drive and is started from there.
    with open(bat, "w") as f:
        f.write("@echo off\r\nA:\r\nCD \\\r\n%s\r\n" % exe)
    run(["mcopy", "-i", path, "-o", bat, "::/RUN.BAT"])
    os.unlink(bat)
    for f in files:
        run(["mcopy", "-i", path, "-o", os.path.join(gamedir, f), "::/" + f.upper()])
    return len(files)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", required=True)
    ap.add_argument("--exe", default="VGALEMMI")
    ap.add_argument("--out", required=True)
    ap.add_argument("--keys", default="",
                    help="comma list of QEMU sendkey names, e.g. '1,ret,f1,ret'")
    ap.add_argument("--boot", type=float, default=22.0, help="seconds before the first key")
    ap.add_argument("--gap", type=float, default=2.0, help="seconds between keys")
    ap.add_argument("--settle", type=float, default=6.0, help="seconds before the dump")
    a = ap.parse_args()

    tmp = os.environ.get("TMPDIR", "/tmp")
    scratch = os.path.join(tmp, "lemref-c-%d.img" % os.getpid())
    make_hd(scratch, a.game)
    print("installed the game into C:\\GAME on %s" % scratch)
    # The A: floppy carries only RUN.BAT; C:'s AUTOEXEC calls it.
    disk = os.path.join(tmp, "lemref-a-%d.img" % os.getpid())
    with open(disk, "wb") as f:
        f.truncate(1474560)
    run(["mformat", "-i", disk, "-f", "1440", "::"])
    bat = disk + ".bat"
    with open(bat, "w") as f:
        f.write("@echo off\r\nC:\r\nCD \\GAME\r\n%s\r\n" % a.exe)
    run(["mcopy", "-i", disk, "-o", bat, "::/RUN.BAT"])
    os.unlink(bat)

    cmd = ["qemu-system-i386", "-M", "pc", "-m", "16",
           "-drive", "file=%s,format=raw,if=ide,index=0,media=disk" % scratch,
           "-drive", "file=%s,format=raw,if=floppy,index=0" % disk,
           "-boot", "c", "-no-reboot", "-display", "none", "-monitor", "stdio"]
    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def mon(line):
        proc.stdin.write((line + "\n").encode())
        proc.stdin.flush()

    try:
        print("booting (%.0fs)..." % a.boot)
        time.sleep(a.boot)
        for k in [k.strip() for k in a.keys.split(",") if k.strip()]:
            print("  sendkey %s" % k)
            mon("sendkey %s" % k)
            time.sleep(a.gap)
        print("settling (%.0fs)..." % a.settle)
        time.sleep(a.settle)
        mon("screendump %s" % os.path.abspath(a.out))
        time.sleep(2.0)
    finally:
        try:
            mon("quit")
            proc.wait(timeout=5)
        except Exception:
            proc.kill()
        for f in (disk, scratch):
            try:
                os.unlink(f)
            except OSError:
                pass
    print("wrote %s (%d bytes)" % (a.out, os.path.getsize(a.out)
                                   if os.path.exists(a.out) else 0))


if __name__ == "__main__":
    main()
