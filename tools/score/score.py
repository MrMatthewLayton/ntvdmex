#!/usr/bin/env python3
"""score.py -- HOW DONE IS NTVDMEX?  One number, computed the same way every day.

    tools/score/score.py                 # the table
    tools/score/score.py --brief         # one line
    tools/score/score.py --append        # ...and add today's row to docs/PROGRESS.md
    tools/score/score.py --json

── WHY THIS IS A SCRIPT AND NOT A JUDGEMENT ────────────────────────────────
A percentage typed into a document is a claim that decays: it is re-judged each
time by a different reader against a different bar, so it drifts without anything
in the project changing. The 2026-09-04 review headlined 65% while scoring DOS
against the *games* bar and Win16 against the *completeness* bar -- two bars, one
number, and the number went up for free.

So the model lives in `model.json`, in the open, and the score is computed from
it. When the number moves, `--append` records WHICH ITEM moved, which makes the
daily delta auditable instead of atmospheric.

⚠ THE SCORE IS ONLY AS HONEST AS THE ATTESTED VALUES. Two items are measured
from the tree every run (Win16 breadth, guests confirmed); the rest are human
attestations with an `evidence` string. Raising one because something looks done
is how this becomes a vanity metric. See `model.json`'s _README.
"""
import json
import os
import subprocess
import sys
import datetime

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
MODEL = os.path.join(HERE, "model.json")
CACHE = os.path.join(HERE, "probe-cache.json")
PROGRESS = os.path.join(ROOT, "docs", "PROGRESS.md")
SHELF = os.path.join(ROOT, "guest", "win16")


# ── probes ──────────────────────────────────────────────────────────────────
# A probe returns (value 0..1, detail string). It may fail -- `guest/` is
# .gitignore'd, so a fresh checkout has no shelf to measure. When it does, fall
# back to the last cached reading and SAY SO in the detail, because a probe that
# silently substitutes a stale number is the `stale artefact worse than missing`
# trap this project has already paid for once.

def probe_win16_breadth(model):
    """Distinct WOW32 ids serviced / (serviced + to-do), unioned over the shelf."""
    if not os.path.isdir(SHELF):
        return None, "no guest/win16 -- shelf not present"
    exes = sorted(f for f in os.listdir(SHELF) if f.upper().endswith(".EXE"))
    if not exes:
        return None, "no guest/win16/*.EXE"
    cmd = [sys.executable, os.path.join(ROOT, "tools", "ne", "neneeds.py")]
    cmd += [os.path.join(SHELF, e) for e in exes]
    cmd += ["--todo", "--json"]
    out = subprocess.run(cmd, capture_output=True, text=True).stdout
    if "---JSON---" not in out:
        return None, "neneeds.py produced no JSON"
    doc = json.loads(out.split("---JSON---", 1)[1])
    s, t = doc["totals"]["serviced"], doc["totals"]["todo"]
    if s + t == 0:
        return None, "neneeds.py measured nothing"
    return s / (s + t), "%d serviced / %d to do, over %d programs" % (
        s, t, doc["totals"]["programs"])


def probe_win16_guests(model):
    """Guests confirmed on real hardware. `partial` counts half."""
    g = model.get("guests", {})
    if not g:
        return None, "no guests map"
    score = {"done": 1.0, "partial": 0.5, "todo": 0.0}
    got = sum(score.get(v.get("state", "todo"), 0.0) for v in g.values())
    done = sum(1 for v in g.values() if v.get("state") == "done")
    part = sum(1 for v in g.values() if v.get("state") == "partial")
    return got / len(g), "%d done + %d partial of %d on the shelf" % (
        done, part, len(g))


PROBES = {"win16_breadth": probe_win16_breadth, "win16_guests": probe_win16_guests}


def load_cache():
    try:
        with open(CACHE) as fh:
            return json.load(fh)
    except (OSError, ValueError):
        return {}


# ── scoring ─────────────────────────────────────────────────────────────────

def compute(model):
    cache, fresh, stale = load_cache(), {}, []
    sections = {}
    for key, sec in model["sections"].items():
        rows, wsum, vsum = [], 0.0, 0.0
        for it in sec["items"]:
            if "probe" in it:
                val, detail = PROBES[it["probe"]](model)
                if val is None:
                    prev = cache.get(it["probe"])
                    if prev is None:
                        raise SystemExit(
                            "probe %r failed (%s) and no cached reading exists.\n"
                            "Fetch the guest shelf (see docs/STATE.md) or remove "
                            "the item." % (it["probe"], detail))
                    val, detail = prev["value"], prev["detail"] + " [STALE %s]" % prev["when"]
                    stale.append(it["probe"])
                else:
                    fresh[it["probe"]] = {
                        "value": val, "detail": detail,
                        "when": datetime.date.today().isoformat()}
                src = "measured"
            else:
                val, detail, src = it["value"], it.get("evidence", ""), "attested"
            rows.append((it["id"], it["w"], val, it["what"], detail, src))
            wsum += it["w"]
            vsum += it["w"] * val
        sections[key] = {"title": sec["title"], "weight": sec["weight"],
                         "score": 100.0 * vsum / wsum, "rows": rows}
    if fresh:
        cache.update(fresh)
        with open(CACHE, "w") as fh:
            json.dump(cache, fh, indent=1, sort_keys=True)
    tw = sum(s["weight"] for s in sections.values())
    overall = sum(s["weight"] * s["score"] for s in sections.values()) / tw
    return overall, sections, stale


def bar(v):
    return ("#" * int(round(v / 5))).ljust(20, ".")


def main():
    with open(MODEL) as fh:
        model = json.load(fh)
    overall, sections, stale = compute(model)

    if "--json" in sys.argv:
        print(json.dumps(
            {"date": datetime.date.today().isoformat(),
             "overall": round(overall, 1),
             "sections": {k: round(v["score"], 1) for k, v in sections.items()}},
            indent=1, sort_keys=True))
        return 0

    line = "NTVDMEX %.0f%%  [%s]" % (overall, "  ".join(
        "%s %.0f%%" % (v["title"], v["score"]) for v in sections.values()))
    if "--brief" in sys.argv:
        print(line)
        return 0

    for key in model["sections"]:
        s = sections[key]
        print("\n== %s -- %.1f%% (weight %.0f%%) %s"
              % (s["title"].upper(), s["score"], 100 * s["weight"], bar(s["score"])))
        for iid, w, val, what, detail, src in sorted(
                s["rows"], key=lambda r: (-r[1] * (1 - r[2]), r[0])):
            flag = " " if val >= 0.999 else ("~" if val > 0 else "*")
            print("  %s %-14s w%-3d %5.0f%%  %s" % (flag, iid, w, 100 * val, what))
            if val < 0.999 and detail:
                print("       %s %s" % ("->" if src == "measured" else "  ", detail))
    print("\n" + "=" * 72)
    print("  OVERALL  %.1f%%   %s" % (overall, bar(overall)))
    print("  (* not started   ~ partial   blank done;"
          " sorted by weight x remaining)")
    if stale:
        print("  !! STALE PROBES (guest shelf missing): %s" % ", ".join(stale))

    if "--append" in sys.argv:
        append_progress(overall, sections)
    return 0


def append_progress(overall, sections):
    today = datetime.date.today().isoformat()
    row = "| %s | **%.1f** | %.1f | %.1f | %.1f | %s |\n" % (
        today, overall, sections["dos"]["score"], sections["wow16"]["score"],
        sections["product"]["score"], os.environ.get("NOTE", ""))
    with open(PROGRESS) as fh:
        text = fh.read()
    marker = "<!-- SCORES -->\n"
    if marker not in text:
        raise SystemExit("docs/PROGRESS.md has no <!-- SCORES --> marker")
    head, tail = text.split(marker, 1)
    # ⚠ ONE ROW PER DAY. Re-running on the same day REPLACES that day's row
    # rather than appending a second -- otherwise a day you happened to score
    # three times reads as three days of work.
    kept = [ln for ln in tail.splitlines(keepends=True)
            if not ln.startswith("| " + today + " |")]
    with open(PROGRESS, "w") as fh:
        fh.write(head + marker + row + "".join(kept))
    print("  recorded in docs/PROGRESS.md")


if __name__ == "__main__":
    sys.exit(main())
