"""Score the captured DMA ring (sb.raw) against itself, one ring-lap apart.

WHY THIS EXISTS. Every audio counter in the host infers a defect from ring
contents; none of them had ever been checked against the audio itself. This
reads the bytes the guest actually produced (sbdump.flag -> C:\\ntvdmex\\sb.raw,
copied off by doomrun.bat) and answers the two questions the counters cannot:

  1 Is REPLAYED_LOUD real, or is it counting quiet blocks as audible?
    -> the dynamic-range histogram of the blocks it flags. Measured 2026-08-25:
       67% are full-scale (range 65+), 16% are 33-64. The counter is honest.

  2 What does the defect LOOK like in time?
    -> the map. Measured: '###R####R#R#R#R########RR##R#####R##R#R#' -- roughly
       every 3rd-4th loud block is 11.6 ms of 186 ms-old audio dropped into the
       middle of the current sound. 562 isolated singles. That is the reported
       symptom, "the sample parts have gaps between them".

Usage: sbgaps.py <sb.raw> [--map N]     (block 256 B, ring 4096 B = 16 blocks)
"""
import sys, collections

BLK, LAP = 256, 16          # bytes per DMA block; blocks per ring lap
LOUD, FLAT = 32, 4          # range thresholds: audible content; host SB_FLAT_RANGE

def load(path):
    d = open(path, 'rb').read()
    n = len(d) // BLK
    rng, rep = [], []
    for i in range(n):
        b = d[i*BLK:(i+1)*BLK]
        rng.append(max(b) - min(b))
        if i >= LAP:
            p = d[(i-LAP)*BLK:(i-LAP+1)*BLK]
            rep.append(sum(1 for x, y in zip(b, p) if x == y) * 10 >= BLK * 9)
        else:
            rep.append(False)
    return d, n, rng, rep

def main():
    path = sys.argv[1]
    d, n, rng, rep = load(path)
    print("%s: %d bytes = %d blocks = %.1f s at 22050 B/s" % (path, len(d), n, len(d)/22050))

    loud = [i for i in range(n) if rng[i] > LOUD]
    lr   = [i for i in loud if rep[i]]
    print("loud blocks (range>%d): %d ; STALE (>=90%% identical to one lap back): %d = %.0f%%"
          % (LOUD, len(loud), len(lr), 100*len(lr)/max(1, len(loud))))

    # Is the counter honest? Range distribution of what it flags.
    h = collections.Counter()
    for i in lr:
        r = rng[i]
        h['0' if r == 0 else '1-4' if r <= 4 else '5-8' if r <= 8 else '9-16' if r <= 16
          else '17-32' if r <= 32 else '33-64' if r <= 64 else '65+'] += 1
    print("\ndynamic range of the STALE blocks (full-scale => the defect is audible):")
    for k in ['0', '1-4', '5-8', '9-16', '17-32', '33-64', '65+']:
        if h[k]:
            print("   range %-6s : %5d  (%.0f%%)" % (k, h[k], 100*h[k]/len(lr)))

    # Shape, not rate: isolated singles sound different from a sustained dropout.
    runs, cur = [], 0
    for i in loud:
        if rep[i]: cur += 1
        elif cur:  runs.append(cur); cur = 0
    if cur: runs.append(cur)
    c = collections.Counter(runs)
    print("\nstale-run lengths inside loud audio (x %.1f ms each):" % (BLK/22.05))
    for k in sorted(c): print("   run of %-3d : %4d" % (k, c[k]))

    if '--map' in sys.argv:
        w = int(sys.argv[sys.argv.index('--map')+1])
        s = max(range(max(1, n-w)), key=lambda t: sum(rng[t:t+w]))
        print("\nloudest %d-block window at block %d (t=%.1fs)" % (w, s, s*BLK/22050))
        print("   '#'=loud fresh  'R'=loud STALE  '.'=quiet fresh  'r'=quiet stale")
        line = ''.join((('R' if rep[i] else '#') if rng[i] > LOUD else
                        ('r' if rep[i] else '.')) for i in range(s, min(n, s+w)))
        for k in range(0, len(line), 40):
            print("   blk %5d  %s" % (s+k, line[k:k+40]))

main()
