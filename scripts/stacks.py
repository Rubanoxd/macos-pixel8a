#!/usr/bin/env python3
"""Call tree of WindowServer's main thread from /tmp/akita-prof-stacks.txt (plugin profiler, fullos156+).

Usage: stacks.py <stacks.txt> <symdir> [focus-function-substring ...]
  symdir holds NativeMaliMetal.dSYM and libvulkan_panfrost.unstripped.dylib of the same build.
Prints: inclusive % per function (running samples), the innermost plugin/PanVK function per sample (what the bridge
code is doing, callees in system libraries folded in), and for each focus function its direct callees.
"""
import collections
import re
import subprocess
import sys

path, symdir = sys.argv[1], sys.argv[2]
focus = sys.argv[3:]
images, samples = {}, []
for line in open(path, errors='replace'):
    if line.startswith('image '):
        _, idx, name = line.split(None, 2)
        images[int(idx)] = name.strip()
    elif line[:1] in 'RW':
        frames = []
        for tok in line[1:].split():
            if tok == '?':
                frames.append(None)
            else:
                i, off = tok.split(':')
                frames.append((int(i), int(off, 16)))
        samples.append((line[0] == 'R', frames))

binaries = {'NativeMaliMetal': f'{symdir}/NativeMaliMetal.dSYM/Contents/Resources/DWARF/NativeMaliMetal',
            'libvulkan_panfrost.dylib': f'{symdir}/libvulkan_panfrost.unstripped.dylib'}
names = {}
for idx, img in images.items():
    if img not in binaries:
        continue
    offs = sorted({f[1] for _, fr in samples for f in fr if f and f[0] == idx})
    # return addresses point after the call: look up off-1 so the line is the call's own
    out = subprocess.run(['xcrun', 'atos', '-arch', 'arm64e', '-o', binaries[img], '-l', '0'] + [hex(o - 1) for o in offs],
                         capture_output=True, text=True).stdout.splitlines()
    for o, s in zip(offs, out):
        s = re.sub(r' \(in [^)]*\)', '', s)
        s = re.sub(r'_RN[a-zA-Z0-9_]*?(metal2vulkan_runtime|metal2vulkan)[0-9]*', '', s)
        names[(idx, o)] = s


def label(f, with_line=False):
    if f is None:
        return '?'
    s = names.get(f)
    if s is None:
        return f'{images.get(f[0], "?")}+{f[1]:#x}'
    if not with_line:
        s = re.sub(r' \([^)]*:\d+\)$', '', s)
        s = re.sub(r' \+ \d+$', '', s)
    return s


run = [fr for r, fr in samples if r]
print(f'{len(samples)} samples, {len(run)} running')
incl = collections.Counter()
for fr in run:
    for name in {label(f) for f in fr}:
        incl[name] += 1
print('\n== inclusive (running)')
for name, n in incl.most_common(70):
    print(f'{100 * n / len(run):5.1f}% {n:5d} {name[:150]}')

inner = collections.Counter()
for fr in run:
    for f in fr:
        if f and images.get(f[0]) in binaries:
            inner[label(f, True)] += 1
            break
    else:
        inner['(no plugin frame) ' + label(fr[0])] += 1
print('\n== innermost plugin/PanVK frame (running)')
for name, n in inner.most_common(60):
    print(f'{100 * n / len(run):5.1f}% {n:5d} {name[:170]}')

for want in focus:
    callees = collections.Counter()
    total = 0
    for fr in run:
        labels = [label(f) for f in fr]
        hit = [i for i, l in enumerate(labels) if want in l]
        if not hit:
            continue
        total += 1
        i = hit[-1]   # the outermost frame of it
        callees[label(fr[i - 1], True) if i > 0 else '(self)'] += 1
    print(f'\n== callees of {want} ({total} samples, {100 * total / max(1, len(run)):.1f}%)')
    for name, n in callees.most_common(25):
        print(f'{100 * n / max(1, total):5.1f}% {n:5d} {name[:170]}')
