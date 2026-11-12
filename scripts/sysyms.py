#!/usr/bin/env python3
"""Waiting/running stacks of WindowServer's main thread with QuartzCore/SkyLight symbols (macOS 26.5 25F71).

Usage: sysyms.py <stacks.txt> [R|W] [N]
Prints the N most common stacks (from the leaf, up to 14 frames) of running (R) or waiting (W) samples, each frame as
image:symbol+off. QuartzCore and SkyLight from scratch/sky/{qc,sky}-syms.txt (unslid cache addresses).
"""
import bisect
import collections
import re
import sys

SKY = '/Volumes/datos2/unixnu-akita/scratch/sky/'
BASES = {'QuartzCore': 0x18afd1000, 'SkyLight': 0x187093000}
SYMS = {}
for img, f in (('QuartzCore', 'qc-syms.txt'), ('SkyLight', 'sky-syms.txt')):
    pairs = []
    for line in open(SKY + f, errors='replace'):
        m = re.match(r'0x([0-9a-f]+): \(\w+\) (\S+)', line)
        if m:
            pairs.append((int(m.group(1), 16), m.group(2)))
    pairs.sort()
    SYMS[img] = ([a for a, _ in pairs], [n for _, n in pairs])

path = sys.argv[1]
want = sys.argv[2] if len(sys.argv) > 2 else 'W'
top = int(sys.argv[3]) if len(sys.argv) > 3 else 12
images, samples = {}, []
for line in open(path, errors='replace'):
    if line.startswith('image '):
        _, i, name = line.split(None, 2)
        images[int(i)] = name.strip()
    elif line[:1] == want:
        samples.append(line[1:].split())


def name(tok):
    if tok == '?':
        return '?'
    i, off = tok.split(':')
    img = images.get(int(i), '?')
    off = int(off, 16)
    if img in SYMS:
        addrs, names = SYMS[img]
        a = BASES[img] + off - 1
        k = bisect.bisect_right(addrs, a) - 1
        if k >= 0:
            return f'{img[:2]}:{names[k][:60]}+{a - addrs[k] + 1:#x}'
    return f'{img[:12]}+{off:#x}'


c = collections.Counter(tuple(name(t) for t in fr[:14]) for fr in samples)
print(f'{len(samples)} {want} samples')
for stack, n in c.most_common(top):
    print(f'\n{n}:')
    for f in stack:
        print('   ', f)
