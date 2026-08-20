#!/usr/bin/env python3
"""Query the MS-DOS 6.22 oracle.  GH #25.

Runs a guest program (or a few lines of DOS) on genuine MS-DOS 6.22 under QEMU
and brings its output back to the Mac, non-interactively, in a couple of seconds.
This is the primary reference for the M9 completeness programme: expected values
in tests come from Ralf Brown's Interrupt List *confirmed here*, never from
memory of what DOS does.

    dosoracle.py --batch "VER"                      # a few DOS commands
    dosoracle.py run probe.com                      # a guest binary
    dosoracle.py run probe.com --screenshot s.ppm   # ...and the final text screen
    dosoracle.py --interactive                      # a real prompt, for poking about

HOW THE CAPTURE WORKS: the harness builds a scratch floppy holding the program
and a generated RUN.BAT; C:'s AUTOEXEC.BAT calls A:\\RUN.BAT if it is present.
RUN.BAT redirects the program's output to A:\\OUT.TXT and then runs QUIT.COM,
which writes QEMU's isa-debug-exit port so the host process ends by itself.  The
host then reads OUT.TXT straight out of the floppy image with mtools.  No
keystroke synthesis, no screen scraping, no timing assumptions.

CAVEAT, and it is the important one: QEMU runs SeaBIOS.  For INT 21h this box is
the genuine Microsoft kernel and therefore *is* the standard.  For INT 10h/16h it
is only another reimplementation's opinion -- BIOS-layer disputes need the real
VGA BIOS on the Dell OptiPlex.  Do not quote this oracle as BIOS truth.

Importable:  from dosoracle import Oracle;  Oracle().batch("VER")
"""

import argparse
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HERE = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(HERE, "_build")
IMG = os.path.join(ROOT, "vm", "dos622.img")
PART_OFFSET = 63 * 512

QUIT_EXIT_CODE = 85           # (0x2A << 1) | 1, see quit.asm
MTOOLS_ENV = dict(os.environ, MTOOLS_SKIP_CHECK="1")


class OracleError(RuntimeError):
    pass


class Result(object):
    """What one oracle run produced.

    .stdout  the program's standard output, read back off the scratch floppy.
             Needs no cooperation from the program, so unmodified binaries work.
    .serial  anything that reached COM1.  A program that writes its register dump
             straight to AUX gets it out even if it later hangs the guest.
    """

    __slots__ = ("stdout", "serial", "seconds")

    def __init__(self, stdout, serial, seconds):
        self.stdout, self.serial, self.seconds = stdout, serial, seconds

    def __iter__(self):                 # so `out, dt = ...` still works
        return iter((self.stdout, self.seconds))

    def __str__(self):
        return self.stdout


def _indent(text, prefix="    "):
    return "\n".join(prefix + l for l in text.splitlines())


def _run(cmd, **kw):
    """Run a helper and decode as CP437, never UTF-8.

    A probe's output is DOS bytes, not text: a buffer dump can contain anything,
    and text=True made Python decode it as UTF-8 and abort the whole run on the
    first byte above 0x7F. The harness must never be the thing that fails when a
    probe returns unusual data -- that is the data we most want to see.
    """
    p = subprocess.run(cmd, capture_output=True, env=MTOOLS_ENV, **kw)
    out = p.stdout.decode("cp437", "replace")
    err = p.stderr.decode("cp437", "replace")
    if p.returncode != 0:
        raise OracleError("%s\n%s%s" % (" ".join(cmd), out, err))
    return out


class Oracle:
    def __init__(self, image=IMG, memory=16, timeout=120):
        if not os.path.exists(image):
            raise OracleError(
                "no oracle image at %s -- run scripts/dosoracle/build.py first" % image)
        self.image = image
        self.memory = memory
        self.timeout = timeout

    # ------------------------------------------------------------------ floppy

    def _make_floppy(self, path, run_bat, payload=()):
        """A 1.44 MB scratch A: carrying the payload and the generated RUN.BAT."""
        with open(path, "wb") as f:
            f.truncate(1474560)
        _run(["mformat", "-i", path, "-f", "1440", "::"])

        bat = os.path.join(BUILD, "RUN.BAT")
        with open(bat, "wb") as f:
            f.write(run_bat.encode("ascii", "replace"))
        _run(["mcopy", "-i", path, "-o", bat, "::/RUN.BAT"])
        for src, name in payload:
            _run(["mcopy", "-i", path, "-o", src, "::/" + name])

    def _read(self, floppy, name):
        try:
            return _run(["mtype", "-i", floppy, "::/" + name])
        except OracleError:
            return None

    # -------------------------------------------------------------------- qemu

    def _qemu(self, floppy, screenshot=None, interactive=False,
              serial=None, hostdir=None):
        cmd = [
            "qemu-system-i386", "-M", "pc", "-m", str(self.memory),
            # snapshot=on: the C: image is COPY-ON-WRITE for the run and every
            # write is discarded at exit. Probes call DOS functions with poisoned
            # registers, and some of those functions delete, create or truncate
            # files -- without this, one careless probe silently corrupts the
            # oracle and every later answer is suspect. It also makes runs
            # reproducible. The scratch floppy is a separate drive and stays
            # writable, so results still come back.
            "-drive", "file=%s,format=raw,if=ide,index=0,media=disk,snapshot=on" % self.image,
            "-boot", "c", "-no-reboot",
            "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
        ]
        if floppy:
            cmd += ["-drive", "file=%s,format=raw,if=floppy,index=0" % floppy]
        if hostdir:
            # Bulk delivery of a host directory as B:, read-only.
            #
            # NOT vvfat.  `fat:ro:` was tried and rejected on evidence: as a hard
            # disk QEMU refuses the read-only node outright ("Block node is
            # read-only"), and as `fat:floppy:ro:` it served the WRONG FILE
            # CONTENTS -- TYPE of a 20-byte file printed fragments of vvfat's own
            # volume structures -- and then hung the guest outright.  A synthesised
            # raw FAT image built with mtools is the same path the scratch floppy
            # uses, which has been reliable, and it is a throwaway file so there is
            # nothing to corrupt.
            cmd += ["-drive", "file=%s,format=raw,if=floppy,index=1,readonly=on"
                    % hostdir]
        if serial:
            cmd += ["-serial", "file:%s" % os.path.abspath(serial)]

        if interactive:
            cmd += ["-display", "default"]
            return subprocess.run(cmd).returncode, None

        # The monitor runs on STDIO rather than a QMP socket: the dev sandbox
        # refuses to bind a unix socket anywhere, TMPDIR included.  A pipe needs
        # no filesystem object, and HMP is enough for what we want it for
        # (screendump on timeout).
        cmd += ["-display", "none", "-monitor", "stdio"]
        proc = subprocess.Popen(cmd, stdin=subprocess.PIPE,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            rc = proc.wait(timeout=self.timeout)
        except subprocess.TimeoutExpired:
            # The guest never reached QUIT.COM.  Grab the text screen before
            # killing it -- that picture is usually the whole diagnosis.
            shot = False
            if screenshot:
                shot = self._monitor(proc, "screendump %s" % os.path.abspath(screenshot))
            self._monitor(proc, "quit")
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
            raise OracleError(
                "guest did not exit within %ds (never reached QUIT.COM)%s"
                % (self.timeout, " -- final screen in %s" % screenshot if shot else ""))

        if rc != QUIT_EXIT_CODE:
            err = (proc.stderr.read() or b"").decode("utf-8", "replace").strip()
            raise OracleError("QEMU exited %d, expected %d%s"
                              % (rc, QUIT_EXIT_CODE, "\n  " + err if err else ""))
        return rc, None

    @staticmethod
    def _monitor(proc, command):
        """Send one HMP command down the monitor pipe.  Best effort."""
        try:
            proc.stdin.write((command + "\n").encode())
            proc.stdin.flush()
            time.sleep(1.0)                 # let QEMU act before we tear it down
            return True
        except (BrokenPipeError, OSError, AttributeError):
            return False

    # ------------------------------------------------------------------- public

    def batch(self, commands, screenshot=None, echo=False, hostdir=None):
        """Run DOS commands, return whatever they wrote to stdout."""
        if isinstance(commands, str):
            commands = [commands]
        lines = ["@ECHO OFF"] if not echo else []
        lines += ["ECHO [BEGIN] > A:\\OUT.TXT"]
        for c in commands:
            lines.append("%s >> A:\\OUT.TXT" % c)
        lines += self._epilogue()
        return self._go("\r\n".join(lines), (), screenshot, hostdir)

    @staticmethod
    def deps(program):
        """Companion files a probe needs beside it, from a `<probe>.deps` sidecar.

        EXEC has to have something to execute, so its probe ships a child
        program. A sidecar keeps that general -- the harness does not need to
        know which probe needs what.
        """
        side = os.path.splitext(program)[0] + ".deps"
        if not os.path.exists(side):
            return []
        out = []
        for line in open(side):
            line = line.strip()
            if line and not line.startswith("#"):
                out.append(os.path.join(os.path.dirname(program), line))
        return out

    def run(self, program, args="", screenshot=None, hostdir=None):
        """Copy a .COM/.EXE onto A: and run it, returning its stdout."""
        if not os.path.exists(program):
            raise OracleError("no such program: %s" % program)
        name = os.path.basename(program).upper()
        base, ext = os.path.splitext(name)
        if len(base) > 8 or ext not in (".COM", ".EXE", ".BAT"):
            raise OracleError("program must be an 8.3 .COM/.EXE/.BAT: %s" % name)
        # Run the probe FROM the directory it lives in, so a companion file it
        # names relatively (EXEC's child program) resolves. The rig's rt.bat
        # already does `cd /d C:\test` and the DOSBox adapter runs from the mount
        # root, so this puts all three hosts on the same footing.
        lines = ["@ECHO OFF",
                 "ECHO [BEGIN] > A:\\OUT.TXT",
                 "A:",
                 "%s %s >> A:\\OUT.TXT" % (name, args),
                 "C:"]
        lines += self._epilogue()
        payload = [(program, name)]
        for dep in self.deps(program):
            payload.append((dep, os.path.basename(dep).upper()))
        return self._go("\r\n".join(lines), payload, screenshot, hostdir)

    @staticmethod
    def _epilogue():
        # Mirror the captured stdout to COM1 as well as leaving it on the floppy.
        # The floppy copy is what we normally read (it needs no cooperation from
        # the program under test, so unmodified binaries work); the serial copy
        # is the one that survives if a later run wedges the guest.
        return ["ECHO [END] >> A:\\OUT.TXT",
                "COPY A:\\OUT.TXT AUX > NUL",
                "C:\\DOS\\QUIT.COM", ""]

    def _make_hostdisk(self, path, hostdir):
        """Pack a host directory into a throwaway 1.44 MB FAT image for B:."""
        files = [f for f in sorted(os.listdir(hostdir))
                 if os.path.isfile(os.path.join(hostdir, f))]
        total = sum(os.path.getsize(os.path.join(hostdir, f)) for f in files)
        if total > 1400 * 1024:
            raise OracleError(
                "%s holds %.1f MB; B: delivery tops out at 1.44 MB. Put larger "
                "corpora on the C: image at build time." % (hostdir, total / 1048576.0))
        with open(path, "wb") as f:
            f.truncate(1474560)
        _run(["mformat", "-i", path, "-f", "1440", "-v", "ORACLE", "::"])
        for f in files:
            _run(["mcopy", "-i", path, "-o", os.path.join(hostdir, f),
                  "::/" + f.upper()])
        return len(files)

    def _go(self, run_bat, payload, screenshot, hostdir=None):
        os.makedirs(BUILD, exist_ok=True)
        floppy = os.path.join(BUILD, "scratch-%d.img" % os.getpid())
        serial = os.path.join(BUILD, "serial-%d.txt" % os.getpid())
        hostimg = None
        if hostdir:
            hostimg = os.path.join(BUILD, "hostdir-%d.img" % os.getpid())
            self._make_hostdisk(hostimg, hostdir)
        try:
            self._make_floppy(floppy, run_bat, payload)
            t0 = time.time()
            timed_out = None
            try:
                self._qemu(floppy, screenshot=screenshot, serial=serial,
                           hostdir=hostimg)
            except OracleError as e:
                timed_out = e           # still try to salvage the serial stream
            dt = time.time() - t0

            ser = ""
            if os.path.exists(serial):
                with open(serial, "rb") as f:
                    ser = f.read().decode("cp437", "replace")
            if timed_out:
                if ser.strip():
                    raise OracleError("%s\n  serial captured before the hang:\n%s"
                                      % (timed_out, _indent(_trim(ser))))
                raise timed_out

            out = self._read(floppy, "OUT.TXT")
            if out is None:
                raise OracleError("guest produced no OUT.TXT")
            return Result(_trim(out), _trim(ser), dt)
        finally:
            for p in (floppy, serial, hostimg):
                if p and os.path.exists(p):
                    os.unlink(p)

    def interactive(self):
        return self._qemu(None, interactive=True)[0]


def _trim(text):
    """Strip the sentinels the batch writes around the real output.

    Matched on the STRIPPED line: DOS's ECHO takes everything up to the
    redirection operator literally, so `ECHO [BEGIN] > A:\\OUT.TXT` writes
    "[BEGIN] " -- with the trailing space.  (The sentinels also may not contain
    < or >, which DOS would parse as redirection mid-line.)
    """
    text = text.replace("\r\n", "\n")
    # Substring, not whole-line: a program whose output does not end in a newline
    # leaves [END] welded onto its last line.
    if "[BEGIN]" in text:
        text = text.split("[BEGIN]", 1)[1]
    if "[END]" in text:
        text = text.rsplit("[END]", 1)[0]
    return text.strip("\n").rstrip()


# (command, what it proves, a substring the oracle ACTUALLY printed).
#
# The expected strings are transcribed from this oracle's own output, not from
# memory -- note "file(s)" is lower-case in 6.22, and DOS's FIND is
# case-sensitive, so the obvious "File(s)" matches nothing.  A selftest that only
# asserted "some output appeared" passed while producing nothing at all.
SELFTEST = [
    ("VER", "the kernel identifies itself", "MS-DOS Version 6.22"),
    ("MEM /C | FIND \"MS-DOS\"", "memory layout is sane", "high memory area"),
    ("DIR C:\\DOS\\*.EXE | FIND \"file(s)\"", "the file system works", "file(s)"),
    ("VOL C:", "the volume is ours", "DOS622"),
]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", nargs="?", default=None, choices=["run"])
    ap.add_argument("program", nargs="?")
    ap.add_argument("args", nargs="*")
    ap.add_argument("--batch", "-b", action="append", help="a DOS command (repeatable)")
    ap.add_argument("--selftest", action="store_true", help="prove the oracle boots and answers")
    ap.add_argument("--interactive", "-i", action="store_true", help="boot to a real prompt")
    ap.add_argument("--screenshot", help="PPM path for the text screen (captured on timeout)")
    ap.add_argument("--dir", dest="hostdir",
                    help="host directory to expose READ-ONLY as D: (bulk test corpora)")
    ap.add_argument("--serial", action="store_true",
                    help="also print whatever reached COM1")
    ap.add_argument("--timeout", type=int, default=120)
    a = ap.parse_args()

    try:
        o = Oracle(timeout=a.timeout)
    except OracleError as e:
        sys.exit(str(e))

    if a.interactive:
        sys.exit(o.interactive())

    if a.selftest:
        print("== MS-DOS 6.22 oracle selftest")
        fails = 0
        for cmd, why, expect in SELFTEST:
            try:
                out, dt = o.batch(cmd)
                ok = expect in out
                print("   [%s] %-42s %.1fs" % ("PASS" if ok else "FAIL", why, dt))
                for line in out.splitlines()[:4]:
                    if line.strip():
                        print("          | %s" % line.rstrip())
                if not ok:
                    print("          expected to contain: %r" % expect)
                fails += 0 if ok else 1
            except OracleError as e:
                print("   [FAIL] %-42s %s" % (why, e))
                fails += 1
        print("\n== %s" % ("all checks passed" if not fails else "%d check(s) FAILED" % fails))
        sys.exit(1 if fails else 0)

    try:
        if a.mode == "run":
            if not a.program:
                sys.exit("run: need a program")
            r = o.run(a.program, " ".join(a.args),
                      screenshot=a.screenshot, hostdir=a.hostdir)
        elif a.batch:
            r = o.batch(a.batch, screenshot=a.screenshot, hostdir=a.hostdir)
        else:
            ap.print_help()
            sys.exit(2)
    except OracleError as e:
        sys.exit("oracle: %s" % e)

    print(r.stdout)
    if a.serial:
        print("--- COM1 ---")
        print(r.serial)
    print("\n(%.1fs on MS-DOS 6.22)" % r.seconds, file=sys.stderr)


if __name__ == "__main__":
    main()
