#!/usr/bin/env python3
"""Split `objdump -d -r` output into one normalised file per function.

    objdump -d -r --no-show-raw-insn x.obj | code.py <outdir> <rdata.bin>

Normalised away: instruction addresses, branch-target addresses, compiler-numbered
suffixes (.L labels, name.1234). An operand relocated against .rdata (where string
literals and anonymous constants live) is replaced by the bytes it points at, so a literal
that moved within .rdata compares equal and a literal whose CONTENT changed does not.
Named variable sections (.bss$g_Name, .data$g_Name, .rdata$Name) keep their names.
"""
import re, sys, os

out, rdata_path = sys.argv[1], sys.argv[2]
rdata = open(rdata_path, 'rb').read()

def rdata_at(offset):
    if not 0 <= offset < len(rdata): return f'<rdata?{offset:#x}>'
    end = rdata.find(b'\0', offset, offset + 48)
    chunk = rdata[offset:end if end >= 0 else offset + 16]
    return '<rdata:' + chunk.hex() + '>'

cur = None; lines = []
def flush():
    if cur: open(os.path.join(out, cur), 'w').write('\n'.join(lines) + '\n')

for raw in sys.stdin:
    s = raw.rstrip()
    m = re.match(r'^[0-9a-f]+ <(.+)>:$', s.strip())
    if m:
        flush(); cur = re.sub(r'\.\d+$', '', m.group(1)).replace('/', '_'); lines = []; continue
    if cur is None: continue
    s = re.sub(r'^\s*[0-9a-f]+:\s*', '', s)
    if not s or s.startswith("Disassembly of section"): continue
    reloc = re.match(r'(dir32|DISP32|secrel32|rva32)\s+(\S+)', s)
    if reloc:
        target = reloc.group(2)
        if target == '.rdata' and lines:
            # the relocated field of the instruction above is the offset into .rdata
            prev = lines[-1]
            numbers = list(re.finditer(r'(\$?)(-?0x[0-9a-f]+)(?=\(|,|$)', prev))
            if numbers:
                n = numbers[0] if len(numbers) == 1 else next((x for x in numbers if x.group(1)), numbers[0])
                lines[-1] = prev[:n.start()] + n.group(1) + rdata_at(int(n.group(2), 16)) + prev[n.end():]
            s = 'reloc .rdata'
        else:
            s = 'reloc ' + re.sub(r'\.\d+\b', '', target)
        lines.append(s); continue
    s = re.sub(r'\b[0-9a-f]+ <([^>]+)>', lambda m: '<' + re.sub(r'\.\d+(?=\+|$)', '', m.group(1)) + '>', s)
    s = re.sub(r'\.L\w+', '.L', s)
    lines.append(s)
flush()
