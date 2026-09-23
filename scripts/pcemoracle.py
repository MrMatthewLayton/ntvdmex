#!/usr/bin/env python3
"""pcemoracle.py -- ask the PCem oracle: genuine MS-DOS 6.22 on an AMI 486 with the
real IBM VGA BIOS, for the questions QEMU/SeaBIOS cannot answer (BIOS services, the
BIOS data area, VGA behaviour).

    scripts/pcemoracle.py run tools/dostest/p_bios.com          # run a program
    scripts/pcemoracle.py run PROG.COM --args "x y" --timeout 90
    scripts/pcemoracle.py boot                                   # just boot, leave it up

Same floppy protocol as the QEMU oracle (scripts/dosoracle): a scratch 1.44 MB A:
carrying the program and a generated RUN.BAT; C:'s AUTOEXEC.BAT (written by
dosoracle/build.py) calls A:\\RUN.BAT if present; the program's output goes to
A:\\OUT.TXT, terminated by an [END] marker. PCem cannot be told to quit from the
guest, so the host polls the floppy image for the marker and then kills PCem.

PCem is a GUI program (SDL + wxWidgets): it needs the desktop session, which this
script has when run from a terminal on the Mac. It is launched with --config and
--load_drive_a so no clicking is ever needed. The config file names vm/dos622.img
(1024/16/63, raw) as the IDE hard disk -- the SAME image the QEMU oracle boots.
"""
import argparse
import os
import shutil
import signal
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP = os.path.join(ROOT, "pcem", "PCem.app", "Contents", "MacOS")
PCEM = os.path.join(APP, "PCem")
CFG = os.path.join(APP, "configs", os.environ.get("PCEM_CFG", "NTVDMEX-DOS622.cfg"))
# ⚠⚠ THE CMOS MUST DESCRIBE THE MACHINE, OR THE BIOS WAITS FOR F1. (s74b, 14:20)
#   nvr/default/ami486.nvr describes two type-47 hard disks; our config has one image,
#   so AMI halted every boot on "D: drive failure -- Press F1", and the runs that
#   "worked" were the ones where the USER pressed F1 without saying so. Diagnosed
#   from here as everything else (display sync, budgets, flush timing) until they
#   mentioned it. CMOS 0x12 low nibble = 0, 0x1A = 0, 0x24..0x2C = 0, checksum
#   redone (big-endian sum of 0x10..0x2D at 0x2E). Unattended boot+run is now ~60 s.
#   enable_sync = 0 in both configs is kept (harmless) but was NOT the cause.
#   pcem/ and ~/PCem/nvr are gitignored; a clean copy of the CMOS is in
#   runs/s74b_lazy32/NTVDMEX-DOS622.ami486.nvr.noD.
# PCEM_CFG=NTVDMEX-VESA.cfg selects the same machine with a Diamond Stealth 32
# (Tseng ET4000/W32p, VESA BIOS in ROM) instead of the plain IBM VGA -- the VESA
# oracle. Both configs share the CMOS layout (nvr copied), so boot is identical.
IMG = os.path.join(ROOT, "vm", "dos622.img")
BUILD = os.path.join(ROOT, "scripts", "dosoracle", "_build")


class OracleError(Exception):
    pass


def _run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode != 0:
        raise OracleError("%s failed: %s" % (cmd[0], (r.stderr or r.stdout).strip()))
    return r.stdout


def make_floppy(path, run_bat, payload=()):
    with open(path, "wb") as f:
        f.truncate(1474560)
    _run(["mformat", "-i", path, "-f", "1440", "::"])
    bat = os.path.join(BUILD, "PCEM_RUN.BAT")
    os.makedirs(BUILD, exist_ok=True)
    with open(bat, "wb") as f:
        f.write(run_bat.encode("ascii", "replace"))
    _run(["mcopy", "-i", path, "-o", bat, "::/RUN.BAT"])
    for src, name in payload:
        _run(["mcopy", "-i", path, "-o", src, "::/" + name])


def read_out(path):
    try:
        return _run(["mtype", "-i", path, "::/OUT.TXT"])
    except OracleError:
        return None


def launch(floppy=None):
    for p in (PCEM, CFG, IMG):
        if not os.path.exists(p):
            raise OracleError("missing: " + p)
    cmd = [PCEM, "--config", CFG]
    # ⚠ --load_drive_a IS A NO-OP IN THIS wx BUILD (s74b, watched on screen: "Not ready
    #   reading drive A" with the flag given). The config's own `disc_a =` line is what
    #   mounts A:, so the scratch image is written INTO the config before each launch
    #   and cleared afterwards. The floppy path must be absolute.
    with open(CFG) as f:
        lines = f.read().splitlines()
    lines = [("disc_a = " + (os.path.abspath(floppy) if floppy else "")) if l.startswith("disc_a") else l
             for l in lines]
    with open(CFG, "w") as f:
        f.write("\n".join(lines) + "\n")
    # Two things the wx build needs on macOS, both found the hard way (s74/s74b):
    #  - it enumerates host optical drives while building its menu and blocks in
    #    open() on a device node (Full Disk Access); tools/pcem/libnodev.dylib
    #    interposes opendev() to fail instantly -- the emulated machine has no CD;
    #  - pcem_path is ~/Library/Application Support/PCem/, NOT the bundle, so roms/
    #    configs/ nvr/ must exist THERE (symlinked to the bundle's copies). "No ROMs
    #    present!" with a full roms/ in the bundle means exactly this.
    env = dict(os.environ)
    nodev = os.path.join(ROOT, "tools", "pcem", "libnodev.dylib")
    if os.path.exists(nodev):
        env["DYLD_INSERT_LIBRARIES"] = nodev
    # ⛔ THIS USED TO BE stdout=DEVNULL, stderr=DEVNULL, AND IT COST A SESSION.
    #    PCem needs the WindowServer; launched without one it dies instantly with a
    #    Swift crash out of XPC `hiservices` (rc=132). With both streams discarded
    #    that is indistinguishable from "booted fine, guest produced no output", so
    #    the harness reported a timeout and the blame went to the probe. Keep the
    #    output: a run that fails must be able to say why.
    log_path = os.path.join(ROOT, "runs", "pcem-last-launch.log")
    os.makedirs(os.path.dirname(log_path), exist_ok=True)
    log = open(log_path, "wb")
    print(f"   PCem output -> {log_path}")
    return subprocess.Popen(cmd, cwd=APP, env=env, stdout=log, stderr=subprocess.STDOUT)


def stop(proc):
    if proc.poll() is None:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(10)
        except subprocess.TimeoutExpired:
            proc.kill()


def run_program(program, args="", timeout=240, budget=None):
    """★ MEASURED (s74b): the guest POSTs and boots in ~100 s on this Mac, and PCem
      WRITES THE FLOPPY IMAGE THROUGH while running -- [END] was on disk at +105 s
      with PCem still up. So: poll the image for [END] up to `timeout` (default 240,
      not 120: the old default was shorter than the boot), then SIGTERM PCem and read
      once more in case a flush lagged. `budget`, if given, caps the run instead."""
    if not os.path.exists(program):
        raise OracleError("no such program: " + program)
    name = os.path.basename(program).upper()
    base, ext = os.path.splitext(name)
    if len(base) > 8 or ext not in (".COM", ".EXE", ".BAT"):
        raise OracleError("program must be an 8.3 .COM/.EXE/.BAT: " + name)
    lines = ["@ECHO OFF",
             "ECHO [BEGIN] > A:\\OUT.TXT",
             "A:",
             "%s %s >> A:\\OUT.TXT" % (name, args),
             "C:",
             "ECHO [END] >> A:\\OUT.TXT"]
    floppy = os.path.join(BUILD, "pcem_scratch.img")
    make_floppy(floppy, "\r\n".join(lines) + "\r\n", [(program, name)])
    limit = budget if budget else timeout
    proc = launch(floppy)
    t0 = time.time()
    out = None
    try:
        while time.time() - t0 < limit:
            time.sleep(3)
            if proc.poll() is not None:
                raise OracleError("PCem exited early (rc=%s)" % proc.returncode)
            out = read_out(floppy)
            if out and "[END]" in out:
                break
    finally:
        stop(proc)
    if not out or "[END]" not in out:
        out = read_out(floppy)                      # a flush that lagged the stop
    if not out or "[END]" not in out:
        raise OracleError("no [END] after %ds; OUT.TXT: %r" % (limit, out))
    return out, time.time() - t0


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run"); r.add_argument("program"); r.add_argument("--args", default="")
    r.add_argument("--timeout", type=int, default=240)
    r.add_argument("--budget", type=int, default=None, help="seconds to let the machine run before stopping it (default min(timeout,60))")
    sub.add_parser("boot")
    a = ap.parse_args()
    if a.cmd == "boot":
        proc = launch()
        print("PCem up (pid %d); Ctrl-C to stop" % proc.pid)
        try:
            proc.wait()
        except KeyboardInterrupt:
            stop(proc)
        return 0
    out, secs = run_program(a.program, a.args, a.timeout, a.budget)
    sys.stdout.write(out)
    sys.stderr.write("[%.1fs]\n" % secs)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except OracleError as e:
        sys.stderr.write("pcemoracle: %s\n" % e)
        sys.exit(2)
