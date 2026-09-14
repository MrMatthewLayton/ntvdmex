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
CFG = os.path.join(APP, "configs", "NTVDMEX-DOS622.cfg")
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
    if floppy:
        cmd += ["--load_drive_a", floppy]
    # PCem resolves roms/, configs/ and nvr/ relative to its own directory.
    return subprocess.Popen(cmd, cwd=APP, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def stop(proc):
    if proc.poll() is None:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(10)
        except subprocess.TimeoutExpired:
            proc.kill()


def run_program(program, args="", timeout=120):
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
    proc = launch(floppy)
    t0 = time.time()
    out = None
    try:
        while time.time() - t0 < timeout:
            time.sleep(2)
            if proc.poll() is not None:
                raise OracleError("PCem exited early (rc=%s)" % proc.returncode)
            out = read_out(floppy)
            if out and "[END]" in out:
                break
        else:
            raise OracleError("timeout after %ds; OUT.TXT so far: %r" % (timeout, out))
    finally:
        stop(proc)
    return out, time.time() - t0


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run"); r.add_argument("program"); r.add_argument("--args", default="")
    r.add_argument("--timeout", type=int, default=120)
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
    out, secs = run_program(a.program, a.args, a.timeout)
    sys.stdout.write(out)
    sys.stderr.write("[%.1fs]\n" % secs)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except OracleError as e:
        sys.stderr.write("pcemoracle: %s\n" % e)
        sys.exit(2)
