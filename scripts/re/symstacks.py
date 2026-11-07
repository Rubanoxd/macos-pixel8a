#!/usr/bin/env python3
# Symbolize wswatch stack samples (unslid 25F71 shared-cache addresses). Usage: symstacks.py <stacks.txt> [last_n_samples]
import os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
IPSW = "/Volumes/datos2/unixnu-akita/tools/ipsw-3.1.739/bin/ipsw"
CACHE = os.path.join(HERE, "26.5-cache/25F71__MacOS/dyld_shared_cache_arm64e")
A2S = os.path.join(HERE, "26.5-cache/a2s-265.a2s")
DB = os.path.join(HERE, "a2s-265.cache")
env = dict(os.environ, TMPDIR="/Volumes/datos2/tmp")

known = {}
if os.path.exists(DB):
    for line in open(DB):
        a, _, s = line.rstrip("\n").partition(" ")
        known[a] = s

lines = [l.rstrip("\n") for l in open(sys.argv[1]) if l.startswith("  ")]
times = sorted({float(l.split()[0][:-2]) for l in lines})
keep = set(times[-int(sys.argv[2]):]) if len(sys.argv) > 2 else set(times)
lines = [l for l in lines if float(l.split()[0][:-2]) in keep]

def sym(a):
    if a in known:
        return known[a]
    v = int(a, 16)
    if not 0x180000000 <= v < 0x200000000:
        return "?"
    out = subprocess.run([IPSW, "dyld", "a2s", CACHE, "0x" + a, "--cache", A2S], capture_output=True, text=True, env=env)
    s = (out.stdout.strip().splitlines() or ["?"])[-1].strip()
    s = re.sub(r"\s+", " ", s)
    known[a] = s
    with open(DB, "a") as db:
        db.write(f"{a} {s}\n")
    return s

for l in lines:
    f = l.split()
    t, th = f[0], f[1]
    pc, lr, frames = f[3], f[5], f[6:]
    print(f"{t} {th}")
    for a in [pc, lr] + frames:
        print(f"    {a}  {sym(a)}")
