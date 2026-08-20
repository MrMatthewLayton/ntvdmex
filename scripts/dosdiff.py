#!/usr/bin/env python3
"""Differential test harness: one .COM, several hosts, one diff.  GH #26.

Runs the same probe binary UNCHANGED on every available host and lines the
answers up side by side, so we are never grading our own homework.

    ./scripts/dosdiff.py tools/dostest/p_ver.com
    ./scripts/dosdiff.py tools/dostest/p_ver.com --json

THE VOTING RULE, and it is the point of the whole thing:

  * The **oracles** vote on truth.  Agreement between them IS truth.
  * **NTVDMEX does not vote.**  It is the subject under test -- the thing being
    graded.  Letting it into the consensus would be circular.
  * Disagreement between oracles is never resolved by majority or coin-flip; it
    is reported as DISPUTED and needs a recorded rationale (see
    docs/research/oracle-disagreements.md).

Host roles and weight follow epic #24:

  msdos622   oracle   genuine Microsoft kernel; for INT 21h it IS the standard
  dosbox-x   oracle   a fourth voice -- decades of distilled compatibility fixes
  ntvdm      oracle   what we are replacing; good for the DOS API, worthless for
                      devices/sound/VESA.  Never truth on its own.
  ntvdmex    SUBJECT  us

A host that cannot run is reported as unavailable WITH THE REASON, never
silently skipped -- a diff that quietly dropped a host would read as agreement.
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts", "dosoracle"))

REGS = ["AX", "BX", "CX", "DX", "SI", "DI", "DS", "ES", "FL", "CF"]

RULES_PATH = os.path.join(ROOT, "tools", "dostest", "oracle-rules.json")


def load_rules(path=RULES_PATH):
    """Recorded rationales for known oracle disagreements.

    The epic's rule is that a disagreement between oracles is a flagged decision
    with a recorded rationale, never a coin-flip.  Keeping those rationales HERE
    rather than only in prose means a settled dispute stops re-reporting itself:
    a permanent unexplained DISPUTED row trains you to ignore the tool, which is
    worse than not having the row at all.

    A rule makes named hosts ABSTAIN on one field.  Abstaining is not the same as
    being ignored -- the value is still printed, and the rationale is printed with
    it, so the reasoning stays visible every run.
    """
    if not os.path.exists(path):
        return []
    with open(path) as f:
        return json.load(f)


def mask_buf(hexs, ignore):
    """Blank byte ranges that cannot be compared across hosts.

    The buffer analogue of SIG. A country block embeds a FAR POINTER to DOS's
    case-map routine, which lives at a different address on every host -- the
    same class of thing as DS/ES, and comparing it manufactures a disagreement
    that means nothing. `ignore` is a list of [offset, length] in BYTES.
    """
    if not hexs or not ignore:
        return hexs
    b = list(hexs)
    for off, ln in ignore:
        for i in range(off * 2, min((off + ln) * 2, len(b))):
            b[i] = "."
    return "".join(b)


def rule_for(rules, probe, case, field):
    """All rules for this field, MERGED.

    Merged rather than first-match-wins: two rules for the same field is an easy
    mistake to make (one recording an abstention, one a byte mask), and
    first-match silently drops the second -- which looks exactly like the rule
    not working.
    """
    hits = [r for r in rules
            if (not r.get("probe") or r["probe"] == probe)
            and r["case"] == case and r["field"].upper() == field.upper()]
    if not hits:
        return None
    out = {"abstain": [], "ignore_bytes": [], "why": ""}
    whys = []
    for r in hits:
        out["abstain"] += r.get("abstain", [])
        out["ignore_bytes"] += r.get("ignore_bytes", [])
        if r.get("why") and r["why"] not in whys:
            whys.append(r["why"])
    out["abstain"] = sorted(set(out["abstain"]))
    out["why"] = " / ".join(whys)
    return out


def _deps(program):
    """Companion files listed in a `<probe>.deps` sidecar beside the probe."""
    side = os.path.splitext(program)[0] + ".deps"
    if not os.path.exists(side):
        return []
    out = []
    for line in open(side):
        line = line.strip()
        if line and not line.startswith("#"):
            out.append(os.path.join(os.path.dirname(program), line))
    return out


# ------------------------------------------------------------------ parsing

class Case(object):
    def __init__(self, name, sig, regs, buf=None):
        self.name, self.sig, self.regs = name, sig, regs
        self.buf = buf          # EMIT_BUF payload, as an upper-case hex string

    def field(self, f):
        """Resolve a SIG name to a value: a register, a half-register, or CF."""
        f = f.upper()
        if f in self.regs:
            return self.regs[f]
        if len(f) == 2 and f[1] in "LH" and (f[0] + "X") in self.regs:
            word = self.regs[f[0] + "X"]
            return (word & 0xFF) if f[1] == "L" else (word >> 8)
        return None

    def width(self, f):
        f = f.upper()
        if f == "CF":
            return 1
        if len(f) == 2 and f[1] in "LH":
            return 2
        return 4


def parse_dump(text):
    """Canonical probe output -> (probe name, [Case]).  Tolerates host noise."""
    probe, cases = None, []
    for line in text.replace("\r\n", "\n").split("\n"):
        line = line.strip()
        m = re.match(r"^#PROBE\s+(\S+)", line)
        if m:
            probe = m.group(1)
            continue
        if not line.startswith("CASE="):
            continue
        name = line.split()[0][5:]
        sigm = re.search(r"\bSIG=([A-Za-z0-9,]+)", line)
        sig = [s for s in (sigm.group(1).split(",") if sigm else []) if s]
        regs = {}
        for r in REGS:
            m = re.search(r"\b%s=([0-9A-Fa-f]+)\b" % r, line)
            if m:
                regs[r] = int(m.group(1), 16)
        cases.append(Case(name, sig, regs))

    # EMIT_BUF lines: "BUF=<name> <hex>". Buffer contents are the substance of a
    # lot of DOS calls -- the country block, the DTA, the AH=65h tables -- and
    # comparing only registers meant those were being checked by eye.
    for line in text.replace("\r\n", "\n").split("\n"):
        line = line.strip()
        if not line.startswith("BUF="):
            continue
        parts = line.split(None, 1)
        nm = parts[0][4:]
        hexs = (parts[1] if len(parts) > 1 else "").strip().upper()
        for c in cases:
            if c.name == nm and c.buf is None:
                c.buf = hexs
                break
        else:
            cases.append(Case(nm, [], {}, hexs))
    return probe, cases


# -------------------------------------------------------------------- hosts

class Host(object):
    role = "oracle"

    def __init__(self, name):
        self.name = name

    def available(self):
        return True, ""

    def run(self, com):
        raise NotImplementedError


class MsDos622(Host):
    """The primary oracle: genuine MS-DOS 6.22 under QEMU.  See GH #25."""

    def __init__(self):
        Host.__init__(self, "msdos622")

    def available(self):
        img = os.path.join(ROOT, "vm", "dos622.img")
        if not os.path.exists(img):
            return False, "no %s -- run scripts/dosoracle/build.py" % os.path.relpath(img, ROOT)
        return True, ""

    def run(self, com):
        from dosoracle import Oracle, OracleError
        try:
            return Oracle().run(com).stdout
        except OracleError as e:
            raise RuntimeError(str(e))


class DosBoxX(Host):
    """A fourth voice.  NOT truth on its own -- it is an emulator's opinion."""

    def __init__(self):
        Host.__init__(self, "dosbox-x")

    def available(self):
        if not shutil.which("dosbox-x"):
            return False, "dosbox-x not on PATH"
        return True, ""

    def run(self, com):
        # DOSBox-X insists on a real window: SDL_VIDEODRIVER=dummy makes
        # dosbox-x hang and makes dosbox-staging abort outright ("Could not
        # initialize video: OpenGL ... driver (dummy)").  So this adapter opens
        # a window briefly, and cannot run headless or over plain ssh.
        work = os.path.join(ROOT, "build", "dosdiff")
        if os.path.exists(work):
            shutil.rmtree(work)
        os.makedirs(work)
        name = os.path.basename(com).upper()
        shutil.copyfile(com, os.path.join(work, name))
        for dep in _deps(com):          # e.g. EXEC's child program
            shutil.copyfile(dep, os.path.join(work, os.path.basename(dep).upper()))
        subprocess.run(
            ["dosbox-x", "-nolog", "-fastlaunch",
             "-c", "mount c %s" % work, "-c", "c:",
             "-c", "%s > OUT.TXT" % name, "-c", "exit"],
            capture_output=True, timeout=120)
        out = os.path.join(work, "OUT.TXT")
        if not os.path.exists(out):
            raise RuntimeError("dosbox-x produced no output (needs a window "
                               "server -- it cannot run headless)")
        with open(out, "rb") as f:
            return f.read().decode("cp437", "replace")


class NtvdmexRig(Host):
    """NTVDMEX itself, on the bare-metal XP rig.  THE SUBJECT UNDER TEST.

    Driven through the SMB watcher loop documented in return-ntvdm.md: drop the
    probe in bm/tests/, write cmd.txt, the watcher runs rt.bat, and the host log
    comes back as result_<name>.log with the guest's console output embedded.

    Mount the share first (the IP moves -- it has been .34 and .29):
        mkdir -p /tmp/xpshare
        mount_smbfs -N //guest@<box-ip>/ntvdmex /tmp/xpshare

    NOTE: LAN access is outside the dev sandbox's allowlist, so this adapter
    only works when dosdiff.py is run with the sandbox disabled (or from a
    normal terminal).
    """

    role = "subject"

    def __init__(self, share=None):
        Host.__init__(self, "ntvdmex")
        self.share = share or os.environ.get("NTVDMEX_SHARE", "/tmp/xpshare")

    def available(self):
        if not os.path.ismount(self.share):
            return False, ("share not mounted at %s -- mount_smbfs -N "
                           "//guest@<box-ip>/ntvdmex %s" % (self.share, self.share))
        beat = os.path.join(self.share, "watcher.txt")
        if not os.path.exists(beat):
            return False, "no watcher.txt on the share -- the rig watcher is not running"
        # The heartbeat must be MOVING.  A stale watcher.txt looks identical to a
        # live one, and every run would then time out with a confusing message.
        try:
            t0 = os.stat(beat).st_mtime
            time.sleep(6)
            if os.stat(beat).st_mtime == t0:
                return False, ("watcher.txt is not updating -- the rig watcher has "
                               "stopped (run bm/runwatch.bat on the box)")
        except OSError as e:
            return False, "cannot stat watcher.txt: %s" % e
        return True, ""

    def run(self, com, timeout=240):
        name = os.path.basename(com)
        shutil.copyfile(com, os.path.join(self.share, "bm", "tests", name))
        # rt.bat copies only the named test into C:\test, so a probe's companion
        # files are staged there directly and left in place between runs.
        deps = _deps(com)
        for dep in deps:
            shutil.copyfile(dep, os.path.join(self.share, "bm", "tests",
                                              os.path.basename(dep)))
        if deps:
            # rt.bat copies only the named test into C:\test, so stage the
            # companions with a preliminary run. The watcher interpolates
            # cmd.txt into a command line, so `&` chains a copy after rt.bat.
            # They persist in C:\test, but re-staging each time keeps a stale
            # companion from silently being the thing under test.
            chain = "dosstub.com"
            for dep in deps:
                chain += "&copy /y \"%s\\bm\\tests\\%s\" C:\\test\\" % (
                    "C:\\Documents and Settings\\All Users\\Documents\\ntvdmex",
                    os.path.basename(dep))
            with open(os.path.join(self.share, "cmd.txt"), "wb") as f:
                f.write((chain + "\r\n").encode())
            deadline2 = time.time() + 120
            while time.time() < deadline2:
                time.sleep(3)
                if not os.path.exists(os.path.join(self.share, "cmd.txt")):
                    break
            time.sleep(8)
        log = os.path.join(self.share, "result_%s.log" % name)
        if os.path.exists(log):
            os.unlink(log)

        with open(os.path.join(self.share, "cmd.txt"), "wb") as f:
            f.write((name + "\r\n").encode())

        deadline = time.time() + timeout
        while time.time() < deadline:
            time.sleep(3)
            if not os.path.exists(os.path.join(self.share, "cmd.txt")):
                break
        else:
            raise RuntimeError("watcher never consumed cmd.txt within %ds" % timeout)

        # rt.bat copies the log while the host may still be finishing, and SMB
        # caches the result -- a log read too early is a PARTIAL file.  Wait for
        # the known-final line rather than for a duration, and re-list the
        # directory each poll to force a fresh readdir.
        while time.time() < deadline:
            time.sleep(3)
            os.listdir(self.share)
            if not os.path.exists(log):
                continue
            with open(log, "rb") as f:
                text = f.read().decode("cp437", "replace")
            if "STAGE2: complete" in text or "==> DOS OUTPUT:" in text and "\n]" in text:
                return self._dos_output(text)
        raise RuntimeError("result log never completed (no 'STAGE2: complete')")

    @staticmethod
    def _dos_output(text):
        """Pull the guest's console output out of the host log."""
        marker = "==> DOS OUTPUT: ["
        i = text.find(marker)
        if i < 0:
            return text
        rest = text[i + len(marker):]
        out = []
        for line in rest.split("\n"):
            if line.strip() == "]":
                break
            out.append(line)
        return "\n".join(out)


class RigHost(Host):
    """NTVDMEX and stock ntvdm both need the XP rig; neither runs on the Mac.

    Deliberately not faked.  The rig loop is the SMB watcher described in
    return-ntvdm.md; wiring it in is the remaining half of #26.
    """

    def __init__(self, name, role, why):
        Host.__init__(self, name)
        self.role = role
        self.why = why

    def available(self):
        return False, self.why

    def run(self, com):
        raise RuntimeError(self.why)


def all_hosts():
    return [
        MsDos622(),
        DosBoxX(),
        RigHost("ntvdm", "oracle",
                "needs an rt.bat variant that drops the IFEO Debugger key for the "
                "baseline run and restores it after -- AND a decision from the user: "
                "the repo notes warn a stock full-screen DOS run wedges the box's "
                "display, which costs a physical reboot (see #26)"),
        NtvdmexRig(),
    ]


# --------------------------------------------------------------------- diff

def diff(results, hosts, probe=None, rules=()):
    """Build the per-field agreement table.

    results: {host name: [Case]}
    """
    oracles = [h.name for h in hosts if h.role == "oracle" and h.name in results]
    subjects = [h.name for h in hosts if h.role == "subject" and h.name in results]

    order, seen = [], set()
    for name in results:
        for c in results[name]:
            if c.name not in seen:
                seen.add(c.name)
                order.append(c.name)

    rows = []
    for case_name in order:
        cases = {h: next((c for c in results[h] if c.name == case_name), None)
                 for h in results}
        sig = []
        for h in list(oracles) + list(subjects):
            if cases.get(h) and cases[h].sig:
                sig = cases[h].sig
                break
        fields = list(sig)
        if any(c is not None and c.buf is not None for c in cases.values()):
            fields.append("BUF")
        for f in fields:
            vals = {}
            for h, c in cases.items():
                if f == "BUF":
                    vals[h] = c.buf if c else None
                else:
                    vals[h] = c.field(f) if c else None
            rule = rule_for(rules, probe, case_name, f)
            abstain = set(rule.get("abstain", [])) if rule else set()
            if f == "BUF" and rule and rule.get("ignore_bytes"):
                for h in list(vals):
                    vals[h] = mask_buf(vals[h], rule["ignore_bytes"])
            ovals = [vals[h] for h in oracles
                     if vals.get(h) is not None and h not in abstain]
            if not ovals:
                verdict, truth = "NO-DATA", None
            elif len(set(ovals)) == 1:
                verdict, truth = "AGREE", ovals[0]
            else:
                verdict, truth = "DISPUTED", None

            match = None
            missing = [h for h in list(oracles) + list(subjects)
                       if vals.get(h) is None]
            if truth is not None and subjects:
                sv = [vals[h] for h in subjects if vals.get(h) is not None]
                if sv:
                    match = all(v == truth for v in sv)
                else:
                    # The subject produced nothing for this field. That is NOT
                    # agreement -- it is missing evidence, and reporting it as
                    # AGREE is exactly the false green this harness exists to
                    # prevent. It bit me once already, via a truncated log.
                    verdict = "NO-DATA"

            width = 4 if f == "BUF" else next((c.width(f) for c in cases.values() if c), 4)
            rows.append({"case": case_name, "field": f, "values": vals,
                         "verdict": verdict, "truth": truth,
                         "subject_matches": match, "width": width,
                         "abstain": sorted(abstain), "why": rule["why"] if rule else None,
                         "missing": missing})
    return rows, oracles, subjects


def fmt(v, width):
    if v is None:
        return "-"
    if isinstance(v, str):
        # A full hex dump would blow the table apart, so show a short digest and
        # let the diff verdict carry the meaning.
        return (v[:8] + "..") if len(v) > 10 else v
    return "%0*X" % (width, v)


def report(probe, rows, oracles, subjects, unavailable):
    cols = oracles + subjects
    w = max([len(c) for c in cols] + [8])
    print("\n== probe: %s" % (probe or "?"))
    print("   oracles: %s   subject: %s"
          % (", ".join(oracles) or "(none)", ", ".join(subjects) or "(none)"))
    for name, why in unavailable:
        print("   unavailable: %-10s %s" % (name, why))

    if not rows:
        print("\n   no cases parsed -- did the probe emit a canonical dump?")
        return 1

    head = "   %-16s %-6s " % ("case", "field")
    head += " ".join("%-*s" % (w, c) for c in cols)
    head += "  verdict"
    print("\n" + head)
    print("   " + "-" * (len(head) - 3))

    disputed = failed = nodata = 0
    notes = []
    for r in rows:
        line = "   %-16s %-6s " % (r["case"], r["field"])
        line += " ".join("%-*s" % (w, fmt(r["values"].get(c), r["width"]))
                         for c in cols)
        v = r["verdict"]
        if v == "AGREE" and r["subject_matches"] is False:
            v = "MISMATCH"
            failed += 1
        elif v == "DISPUTED":
            disputed += 1
        elif v == "NO-DATA":
            nodata += 1
        if r["abstain"]:
            notes.append(r)
            v += " [%d]" % len(notes)
        line += "  " + v
        print(line)

    print()
    for i, r in enumerate(notes, 1):
        print("   [%d] %s/%s: %s abstains -- %s"
              % (i, r["case"], r["field"], ", ".join(r["abstain"]), r["why"]))
    if notes:
        print()
    if disputed:
        print("   %d field(s) DISPUTED between oracles -- these need a recorded"
              % disputed)
        print("   rationale in docs/research/oracle-disagreements.md, not a coin-flip.")
    if failed:
        print("   %d field(s) where NTVDMEX disagrees with oracle consensus." % failed)
    if nodata:
        print("   %d field(s) with NO DATA from the subject -- missing evidence, not"
              % nodata)
        print("   agreement. Check for a truncated log or a probe that died early.")
    if not disputed and not failed and not nodata:
        print("   no disputes, no mismatches.")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("probe", help="a canonical probe .COM (see tools/dostest/probe.inc)")
    ap.add_argument("--host", action="append",
                    help="restrict to these hosts (repeatable)")
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()

    if not os.path.exists(a.probe):
        sys.exit("no such probe: %s" % a.probe)

    hosts = all_hosts()
    if a.host:
        hosts = [h for h in hosts if h.name in a.host]

    results, unavailable = {}, []
    probe_name = None
    for h in hosts:
        ok, why = h.available()
        if not ok:
            unavailable.append((h.name, why))
            continue
        try:
            text = h.run(a.probe)
        except Exception as e:
            unavailable.append((h.name, "run failed: %s" % e))
            continue
        name, cases = parse_dump(text)
        probe_name = probe_name or name
        if not cases:
            unavailable.append((h.name, "no canonical dump in output"))
            continue
        results[h.name] = cases

    rows, oracles, subjects = diff(results, hosts, probe_name, load_rules())

    if a.json:
        print(json.dumps({"probe": probe_name, "oracles": oracles,
                          "subjects": subjects,
                          "unavailable": [{"host": n, "why": w} for n, w in unavailable],
                          "rows": rows}, indent=2))
        return 0
    return report(probe_name, rows, oracles, subjects, unavailable)


if __name__ == "__main__":
    sys.exit(main())
