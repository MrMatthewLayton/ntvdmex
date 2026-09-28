#!/usr/bin/env python3
"""hostprof.py <host.log> <nm -n output> -- name the STAGE2: HOSTPROF buckets (#183).
Build the nm file from the SAME binary that ran: i686-w64-mingw32-nm -n build/ntvdmhost.exe"""
import re, sys, bisect
from collections import Counter
syms = []
for l in open(sys.argv[2]):
    f = l.split()
    if len(f) == 3 and f[1].lower() in 'tw': syms.append((int(f[0], 16), f[2]))
syms.sort(); addrs = [a for a, _ in syms]
log = open(sys.argv[1], 'rb').read().decode('latin1')
m = re.search(r'HOSTPROF samples=(\d+) in_host_image=(\d+) image_base=0x([0-9a-f]+)', log)
samples, inimg, base = int(m.group(1)), int(m.group(2)), int(m.group(3), 16)
fn = Counter(); rows = []
for m in re.finditer(r'HOSTPROF rva=0x([0-9a-f]+) n=(\d+)', log):
    va = base + int(m.group(1), 16); n = int(m.group(2))
    i = bisect.bisect_right(addrs, va) - 1
    fn[syms[i][1]] += n; rows.append((va, n, syms[i][1], va - syms[i][0]))
print(f"samples {samples}, in host image {inimg} ({100*inimg/max(samples,1):.0f}%); top buckets cover {sum(r[1] for r in rows)}")
for va, n, name, off in rows[:25]: print(f"  {va:08x} {n:6d} {100*n/max(inimg,1):5.1f}%  {name}+0x{off:x}")
print("-- by function (top buckets only)")
for f, n in fn.most_common(15): print(f"  {n:6d} {100*n/max(inimg,1):5.1f}%  {f}")
