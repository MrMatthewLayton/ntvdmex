#!/usr/bin/env python3
"""One normalised file per named data section (.data$X, .rdata$X, .bss$X) of an object.

    data.py <x.obj> <outdir> <rdata.bin>

Each file holds the section's size, its bytes, and its relocations by target. A field
relocated against .rdata is shown as the bytes it points at, so a literal that moved within
.rdata compares equal; a pointer to a different variable or function does not.
"""
import re, subprocess, sys, os, collections, hashlib

obj, out, rdata_path = sys.argv[1:4]
rdata = open(rdata_path, 'rb').read()
OBJDUMP = os.environ.get('OBJDUMP', 'i686-w64-mingw32-objdump')
os.makedirs(out, exist_ok=True)

def named(section):
    return re.match(r'\.(data|rdata|bss)\$', section) is not None

def norm(section):
    return re.sub(r'\.\d+$', '', section)

sizes = {}
for line in subprocess.run([OBJDUMP, '-h', obj], capture_output=True, text=True).stdout.split('\n'):
    m = re.match(r'\s*\d+\s+(\S+)\s+([0-9a-f]+)\s', line)
    if m: sizes[m.group(1)] = int(m.group(2), 16)

contents = collections.defaultdict(bytearray); cur = None
for line in subprocess.run([OBJDUMP, '-s', obj], capture_output=True, text=True).stdout.split('\n'):
    m = re.match(r'Contents of section (\S+):', line)
    if m: cur = m.group(1); continue
    m = re.match(r'\s([0-9a-f]+)\s((?:[0-9a-f]{2,8}\s){1,4})', line)
    if m and cur: contents[cur] += bytes.fromhex(m.group(2).replace(' ', ''))

relocs = collections.defaultdict(list); cur = None
for line in subprocess.run([OBJDUMP, '-r', obj], capture_output=True, text=True).stdout.split('\n'):
    m = re.match(r'RELOCATION RECORDS FOR \[(\S+)\]:', line)
    if m: cur = m.group(1); continue
    m = re.match(r'([0-9a-f]{8})\s+(\S+)\s+(\S+)', line)
    if m and cur: relocs[cur].append((int(m.group(1), 16), m.group(2), m.group(3)))

for section, size in sizes.items():
    if not named(section): continue
    data = bytearray(contents.get(section, b''))
    notes = []
    for offset, kind, target in relocs.get(section, []):
        addend = int.from_bytes(data[offset:offset + 4], 'little')
        if target == '.rdata':
            end = rdata.find(b'\0', addend, addend + 48)
            what = 'rdata:' + rdata[addend:end if end >= 0 else addend + 16].hex()
        else:
            what = f'{norm(target)}+{addend:#x}'
        data[offset:offset + 4] = b'\0\0\0\0'
        notes.append(f'{offset:#x} {kind} {what}')
    text = f'size {size:#x}\n'
    text += ''.join(data[i:i + 32].hex() + '\n' for i in range(0, len(data), 32))
    text += '\n'.join(notes) + '\n'
    name = norm(section).lstrip('.').replace('/', '_').replace('$', '_')
    if name != section.lstrip('.').replace('/', '_').replace('$', '_'):
        # compiler-numbered (switch tables, function-local statics): named by content instead
        name += '_' + hashlib.sha1(text.encode()).hexdigest()[:10]
    open(os.path.join(out, name), 'w').write(text)
