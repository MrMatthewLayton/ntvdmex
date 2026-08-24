#!/usr/bin/env python3
"""sbref.py -- analyse the raw PCM the guest actually fed us (sbdump.flag -> sb.raw).

The DMA ring is the only place a game's sampled audio exists, and vdd_sb_render() is the
only thing that reads it, so a byte-for-byte record of what came out is the audio
equivalent of a screenshot. This asks the three questions that separate "the emulation is
wrong" from "the game sounds like that":

  duration   bytes / (2 if stereo) / rate against the run length -- a rate error shows
             here and nowhere else that is easy to read.
  framing    correlation between the two interleaved channels. Two channels of one sound
             effect are highly correlated (~0.98); consecutive MONO samples mistaken for
             a stereo pair would also correlate, so a LOW value is the useful signal --
             it means the framing is wrong.
  periodicity where the discontinuities sit WITHIN the DMA block. Game audio has sharp
             attacks, so a raw count of jumps says little; a jump rate that peaks at one
             fixed offset in every block is a defect at the block boundary, and its
             height above the mean is the evidence.

    ./sbref.py sb.raw --rate 11025 --stereo --block 256
"""
import sys


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    path = sys.argv[1]
    rate = int(sys.argv[sys.argv.index('--rate') + 1]) if '--rate' in sys.argv else 11025
    blk = int(sys.argv[sys.argv.index('--block') + 1]) if '--block' in sys.argv else 256
    stereo = '--stereo' in sys.argv
    thr = int(sys.argv[sys.argv.index('--thr') + 1]) if '--thr' in sys.argv else 48

    raw = open(path, 'rb').read()
    step = 2 if stereo else 1
    L = raw[0::step]
    n = len(L)
    print("%s: %d bytes = %.1f s at %d Hz %s"
          % (path, len(raw), len(raw) / step / rate, rate, "stereo" if stereo else "mono"))
    sil = sum(1 for b in raw if b == 0x80)
    print("  silence (0x80): %.1f%%" % (100.0 * sil / len(raw)))

    if stereo:
        R = raw[1::2]
        m = min(n, len(R))
        mL = sum(L[:m]) / m
        mR = sum(R[:m]) / m
        cov = sum((L[i] - mL) * (R[i] - mR) for i in range(0, m, 7))
        vL = sum((L[i] - mL) ** 2 for i in range(0, m, 7))
        vR = sum((R[i] - mR) ** 2 for i in range(0, m, 7))
        print("  corr(L,R) = %.3f   (low would mean the stereo framing is wrong)"
              % (cov / ((vL * vR) ** .5) if vL and vR else 0))

    frames = blk // step
    hist = [0] * frames
    big = 0
    for i in range(1, n):
        if abs(L[i] - L[i - 1]) > thr:
            hist[i % frames] += 1
            big += 1
    avg = big / frames if frames else 0
    mx = max(hist) if hist else 0
    print("  discontinuities |d|>%d: %d over %d frames (%.0f blocks of %d frames)"
          % (thr, big, n, n / frames, frames))
    if avg:
        print("  per-position-in-block: mean %.1f, MAX %d at offset %d -> peak/mean %.2f"
              % (avg, mx, hist.index(mx), mx / avg))
        top = sorted(range(frames), key=lambda k: -hist[k])[:8]
        print("  hottest offsets:", [(k, hist[k]) for k in top])
        if mx / avg > 3:
            print("  ^^ a peak that far above the mean is a DEFECT AT THE BLOCK BOUNDARY,"
                  " not the game's own audio")
    runs = {}
    r = 1
    for i in range(1, n):
        if L[i] == L[i - 1]:
            r += 1
        else:
            if r >= 16:
                runs[r] = runs.get(r, 0) + 1
            r = 1
    print("  runs of >=16 identical samples: %d, longest %d"
          % (sum(runs.values()), max(runs) if runs else 0))
    return 0


if __name__ == '__main__':
    sys.exit(main())
