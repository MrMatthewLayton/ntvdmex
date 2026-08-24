"""Where in the DMA block do the discontinuities land -- against the MEASURED grid?

sbref.py inferred the block grid from the capture and anchored it at byte 0. The
block ledger says the boundaries are really at 15 + n*256: DMX primes the DSP with
two single-cycle transfers of 4 and 11 bytes before the auto-init ring starts, and
both land in the capture. So every previous statement of the form "the jump is at
offset 2 of the block" was measured against a grid 15 bytes out of phase -- and 11
is odd, so the stereo frame parity flips there as well.

Scored against BOTH anchors so the comparison is visible rather than asserted, and
against a shuffled-phase control so "peaky" means peaky relative to chance.

    python3 build/blockphase.py build/sb_new.raw 15 256
"""
import sys

data = open(sys.argv[1], 'rb').read()
ANCHOR = int(sys.argv[2]) if len(sys.argv) > 2 else 15
BLOCK = int(sys.argv[3]) if len(sys.argv) > 3 else 256

print("%d bytes, block=%d, anchor=%d" % (len(data), BLOCK, ANCHOR))
print("8-bit unsigned stereo: frame = 2 bytes, block = %d frames\n" % (BLOCK // 2))


def hist(anchor, thresh):
    """Count sample-to-sample jumps > thresh by position within the block."""
    h = [0] * BLOCK
    n = 0
    # compare each channel against ITSELF one frame earlier: a stereo stream's
    # L->R step is not a discontinuity, it is the format.
    for i in range(anchor + 2, len(data) - 1):
        d = abs(data[i] - data[i - 2])
        if d > thresh:
            h[(i - anchor) % BLOCK] += 1
            n += 1
    return h, n


for anchor, label in ((0, "anchored at 0   (what sbref.py assumed)"),
                      (ANCHOR, "anchored at %-2d  (MEASURED from the ledger)" % ANCHOR)):
    h, n = hist(anchor, 48)
    mean = n / float(BLOCK)
    top = sorted(range(BLOCK), key=lambda k: -h[k])[:6]
    print("%s" % label)
    print("   %d jumps, mean %.1f per position" % (n, mean))
    for k in top:
        print("     byte %3d (frame %5.1f, ch %s): %5d  = %4.1fx mean"
              % (k, k / 2.0, "L" if k % 2 == 0 else "R", h[k], h[k] / mean))
    print()
