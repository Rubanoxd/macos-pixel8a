#!/usr/bin/env python3
"""What the bridge's threads do while WindowServer's main thread waits (profile-all stacks of fullos158+).

Usage: allstacks.py <allstacks.txt> <symdir>
Per watched thread: running share and its innermost plugin/PanVK function (running and waiting, separately). Then, over the rounds
where the main thread waits in nmb_command_buffer_wait_scheduled, what each bridge thread was doing in the same round.
"""
import collections
import re
import subprocess
import sys

path, symdir = sys.argv[1], sys.argv[2]
images, threads, samples = {}, {}, []
for line in open(path, errors='replace'):
    if line.startswith('image '):
        _, i, name = line.split(None, 2); images[int(i)] = name.strip()
    elif line.startswith('thread '):
        _, i, name = line.split(None, 2); threads[int(i)] = name.strip()
    elif line.startswith('T'):
        parts = line.split()
        th, rnd, st = int(parts[0][1:]), int(parts[1]), parts[2]
        fr = []
        for tok in parts[3:]:
            if tok == '?': fr.append(None)
            else:
                i, off = tok.split(':'); fr.append((int(i), int(off, 16)))
        samples.append((th, rnd, st == 'R', fr))

binaries = {'NativeMaliMetal': f'{symdir}/NativeMaliMetal.dSYM/Contents/Resources/DWARF/NativeMaliMetal',
            'libvulkan_panfrost.dylib': f'{symdir}/libvulkan_panfrost.unstripped.dylib'}
names = {}
for idx, img in images.items():
    if img not in binaries: continue
    offs = sorted({f[1] for *_, fr in samples for f in fr if f and f[0] == idx})
    out = subprocess.run(['xcrun', 'atos', '-arch', 'arm64e', '-o', binaries[img], '-l', '0'] + [hex(o - 1) for o in offs],
                         capture_output=True, text=True).stdout.splitlines()
    for o, s in zip(offs, out):
        s = re.sub(r' \(in [^)]*\)', '', s)
        s = re.sub(r'_RN[a-zA-Z0-9_]*?(metal2vulkan_runtime|metal2vulkan)[0-9]*', '', s)
        names[(idx, o)] = s


def inner(fr, depth=2):
    """the innermost plugin/PanVK frames (depth of them), with the system leaf in front"""
    got = []
    for f in fr:
        if f and images.get(f[0]) in binaries:
            got.append(names.get(f, '?')[:90])
            if len(got) == depth: break
    leaf = fr[0]
    leafs = f'{images.get(leaf[0], "?")}+{leaf[1]:#x}' if leaf else '?'
    return (leafs if leaf and images.get(leaf[0]) not in binaries else '') + ' <- ' + ' <- '.join(got)


rounds = max(r for _, r, _, _ in samples) if samples else 0
print(f'{len(samples)} samples over {rounds} rounds')
by = collections.defaultdict(list)
for s in samples: by[s[0]].append(s)
main = next((k for k, v in threads.items() if '(main)' in v or v == 'ws_main_thread'), None)
for th, ss in sorted(by.items()):
    run = [s for s in ss if s[2]]
    print(f'\n== {threads.get(th, th)}: {len(ss)} samples, running {100 * len(run) / max(1, len(ss)):.0f}%')
    for label, group in (('running', run), ('waiting', [s for s in ss if not s[2]])):
        c = collections.Counter(inner(s[3]) for s in group)
        for k, n in c.most_common(8):
            print(f'  {label} {100 * n / max(1, len(ss)):5.1f}% {k[:220]}')

if main is not None:
    waiting_rounds = {r for th, r, run, fr in samples if th == main and not run and any(
        f and 'wait_scheduled' in names.get(f, '') for f in fr)}
    print(f'\n== while the main thread waits in wait_scheduled ({len(waiting_rounds)} rounds)')
    for th, ss in sorted(by.items()):
        if th == main: continue
        hit = [s for s in ss if s[1] in waiting_rounds]
        c = collections.Counter(('R ' if s[2] else 'W ') + inner(s[3]) for s in hit)
        print(f' {threads.get(th, th)} ({len(hit)} samples)')
        for k, n in c.most_common(6):
            print(f'   {100 * n / max(1, len(hit)):5.1f}% {k[:220]}')
