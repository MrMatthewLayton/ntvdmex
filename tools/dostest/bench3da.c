/* bench3da.c -- what ONE 0x3DA poll costs.
 *
 * ── WHY THIS EXISTS (session 67) ─────────────────────────────────────────────────
 *   0x3DA is the port a guest lives in. Lemmings reads it 73.8 MILLION times in a
 *   45-second run -- 1.6M/s -- and in its menu phase it does essentially NOTHING ELSE
 *   (measured poll-loop occupancy 101.5%). So work added inside status_in is
 *   multiplied by a number nothing else in the system comes close to.
 *
 *   Session 67 added a correctness fix -- deriving the vertical geometry from the CRTC
 *   registers instead of guessing it from the displayed height -- and paid for it by
 *   reassembling three scattered 10-bit fields, revalidating them and dividing, ON
 *   EVERY POLL:
 *
 *       1d1df10  16.0 ns     before
 *       12e3269  20.0 ns     +25%, and on the rig 77.0M polls/run -> 73.6M
 *       6e8fd70  16.1 ns     after caching it in crtc_vt_recompute() -- recovered
 *
 * ⚠⚠ THAT LAST NUMBER WAS 17.2 UNTIL I MEASURED IT HONESTLY. I first timed HEAD
 *   minutes after timing the baseline, with a test battery in between, and reported a
 *   1.2ns residual that does not exist. Rebuilding BOTH from source and running them
 *   ALTERNATELY gives old 16.51/16.32/16.46 against new 16.37/16.15/16.07 -- HEAD is
 *   at or just under the baseline. A benchmark compared against a number taken at a
 *   different moment on a machine doing different work is not a comparison; it is two
 *   unrelated measurements subtracted. Interleave them, always.
 *
 * ⚠ NOTHING ELSE WOULD HAVE CAUGHT THAT. The off-VM battery is pass/fail and was
 *   green throughout; the rig counters all looked normal because the guest still hit
 *   70 Hz -- it simply got less done between retraces. The only witness is a
 *   benchmark aimed at this one port.
 *
 * ── USE ──────────────────────────────────────────────────────────────────────────
 *   Run it BY HAND, before and after any change to status_in. It is deliberately NOT
 *   in run.sh: a wall-clock measurement on a shared machine is not a pass/fail check,
 *   and a battery that fails because something else was compiling is a battery people
 *   learn to ignore.
 *
 *       cc -std=c99 -O2 -I src/vdd -o /tmp/bench3da tools/dostest/bench3da.c \
 *          src/vdd/vdd_video.c src/vdd/vdd_bus.c && /tmp/bench3da
 *
 *   Run it twice and take the second; compare against the numbers above, and against
 *   the SAME binary rebuilt from the commit you are comparing to -- not against a
 *   figure quoted in a commit message, because the host machine is not a constant.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include "vdd_bus.h"
#include "vdd_video.h"

static uint8_t g_vmem[VID_APERTURE_SIZE];
static video_state vid;
static uint64_t g_us = 0;
static uint64_t clk(void) { return g_us; }

int main(void)
{
    vdd_bus bus; ntvdd_regs r; uint32_t v; long i;
    const long N = 20000000;
    struct timespec a, b; double ns;

    memset(&bus, 0, sizeof bus);
    memset(&vid, 0, sizeof vid);
    vid.vmem = g_vmem;
    vdd_video_init(&bus, &vid);

    /* Mode 0Dh: Lemmings' gameplay mode, so the CRTC path under test is the one a
       real guest drives -- 449 total / 400 active / 406 blank start. */
    memset(&r, 0, sizeof r); r.eax = 0x000D;
    vdd_bus_deliver_int(&bus, 0x10, &r);
    vid.time_us = clk;

    /* The clock advances a microsecond per poll, which is FASTER than a real guest
       manages (it sees no advance at all on 77% of polls) -- so this exercises the
       expensive path every time rather than flattering it. */
    clock_gettime(CLOCK_MONOTONIC, &a);
    for (i = 0; i < N; ++i) { g_us += 1; vdd_bus_io(&bus, 0x3DA, 1, 1, &v); }
    clock_gettime(CLOCK_MONOTONIC, &b);

    ns = ((double)(b.tv_sec - a.tv_sec) * 1e9 + (double)(b.tv_nsec - a.tv_nsec)) / (double)N;
    printf("0x3DA poll: %.2f ns each  (%ld polls, mode 0Dh)\n", ns, N);
    printf("  reference: 16.0 ns at 1d1df10, 20.0 ns at 12e3269, 16.1 ns at 6e8fd70 (interleave the A/B!)\n");
    return 0;
}
