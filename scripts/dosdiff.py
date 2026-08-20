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


def rule_for(rules, probe, case, field):
    for r in rules:
        if r.get("probe") and r["probe"] != probe:
            continue
        if r["case"] == case and r["field"].upper() == field.upper():
            return r
    return None


# ------------------------------------------------------------------ parsing

class Case(object):
    def __init__(self, name, sig, regs):
        self.name, self.sig, self.regs = name, sig, regs

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
                "needs the XP rig with the IFEO Debugger key dropped for the "
                "baseline run (not yet wired -- see #26)"),
        RigHost("ntvdmex", "subject",
                "needs the XP rig or an XP VM to run the NTVDMEX host "
                "(not yet wired -- see #26)"),
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
        for f in sig:
            vals = {}
            for h, c in cases.items():
                vals[h] = c.field(f) if c else None
            rule = rule_for(rules, probe, case_name, f)
            abstain = set(rule["abstain"]) if rule else set()
            ovals = [vals[h] for h in oracles
                     if vals.get(h) is not None and h not in abstain]
            if not ovals:
                verdict, truth = "NO-DATA", None
            elif len(set(ovals)) == 1:
                verdict, truth = "AGREE", ovals[0]
            else:
                verdict, truth = "DISPUTED", None

            match = None
            if truth is not None and subjects:
                sv = [vals[h] for h in subjects if vals.get(h) is not None]
                if sv:
                    match = all(v == truth for v in sv)

            width = next((c.width(f) for c in cases.values() if c), 4)
            rows.append({"case": case_name, "field": f, "values": vals,
                         "verdict": verdict, "truth": truth,
                         "subject_matches": match, "width": width,
                         "abstain": sorted(abstain), "why": rule["why"] if rule else None})
    return rows, oracles, subjects


def fmt(v, width):
    return "-" if v is None else ("%0*X" % (width, v))


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

    disputed = failed = 0
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
    if not disputed and not failed:
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
