#!/usr/bin/env python3
"""The NMB_RUNTIME_ID the bridge build script derives (pin + the tree after all recorded patches): it keys the shader cache."""
import hashlib, json, os, re, subprocess
from pathlib import Path
ROOT = Path('/Volumes/datos2/unixnu-akita/UniXNU')
src = (ROOT / 'scripts/build-adreno-metal-bridge.py').read_text()
PIN = json.loads((ROOT / 'scripts/data/gpu-metal-vulkan-inputs.json').read_text())['translator']['commit']
SOURCE = ROOT / '.cache/gpu-reference/metal2vulkan'
names = re.findall(r"'(\d{4}-[^']+\.patch)'", src)
PATCHES = [ROOT / 'drivers/AdrenoMetal/bridge/patches' / n for n in names]
index = Path('/Volumes/datos2/tmp/bridge-index')
env = dict(os.environ, GIT_INDEX_FILE=str(index))
subprocess.run(['git', '-C', str(SOURCE), 'read-tree', 'HEAD'], env=env, check=True)
for p in PATCHES:
    subprocess.run(['git', '-C', str(SOURCE), 'apply', '--cached', str(p)], env=env, check=True)
tree = subprocess.check_output(['git', '-C', str(SOURCE), 'write-tree'], env=env, text=True).strip()
index.unlink()
subprocess.run(['git', '-C', str(SOURCE), 'read-tree', 'HEAD'], env=env, check=True)
subprocess.run(['git', '-C', str(SOURCE), 'add', '-A'], env=env, check=True)
cur = subprocess.check_output(['git', '-C', str(SOURCE), 'write-tree'], env=env, text=True).strip()
index.unlink()
print(len(PATCHES), 'patches; current tree is the patched one:', cur == tree)
print(hashlib.sha256((PIN + '\n' + tree).encode()).hexdigest()[:32])
