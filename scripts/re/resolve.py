#!/usr/bin/env python3
# Annotate "bl 0x19..." objc_msgSend stubs in an ipsw disassembly listing with their selector names.
# Usage: resolve.py <listing.s>  (cache of stub -> selector in stubs.cache next to this script)
import os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
IPSW = "/Volumes/datos2/unixnu-akita/tools/ipsw-3.1.739/bin/ipsw"
CACHE = "/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld/dyld_shared_cache_arm64e"
A2S = "/Volumes/datos2/tmp/C284A291-B68B-3D0D-A1A0-3F866F87EFDC.a2s"
DB = os.path.join(HERE, "stubs.cache")

known = {}
if os.path.exists(DB):
    for line in open(DB):
        parts = line.split()
        if len(parts) == 2:
            known[parts[0]] = parts[1]

text = open(sys.argv[1]).read()
stubs = sorted(set(re.findall(r"\bbl\s+(0x19[0-9a-f]+)", text)))
env = dict(os.environ, TMPDIR="/Volumes/datos2/tmp")
with open(DB, "a") as db:
    for a in stubs:
        if a in known:
            continue
        out = subprocess.run([IPSW, "dyld", "disass", CACHE, "--vaddr", a, "--count", "2", "--cache", A2S],
                             capture_output=True, text=True, env=env).stdout
        m = re.search(r";\s*(\S+)\s*$", out, re.M)
        known[a] = m.group(1) if m else "?"
        db.write(f"{a} {known[a]}\n")

for line in text.splitlines():
    m = re.search(r"\bbl\s+(0x19[0-9a-f]+)", line)
    if m and ";" not in line:
        line += f"  ; -[{known.get(m.group(1), '?')}]"
    print(line)
