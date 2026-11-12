#!/usr/bin/env python3
"""Rebuild only the akita NativeUfs bundle (UFS + GOP display + recovery input) with the pipeline's own build()."""
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path('/Volumes/datos2/unixnu-akita/ref/macOS-ForSheng')
OUT = ROOT / 'out/macos26-xnu-arm64e'
os.environ['MACOS_BOARD'] = 'akita'
sys.path.insert(0, str(ROOT / 'scripts'))
from build_macos26_storage import build  # noqa: E402


def run(command, name, record, cwd=ROOT, env=None):
    command = [str(item) for item in command]
    log = OUT / 'logs' / (name + '.log')
    log.parent.mkdir(exist_ok=True)
    print(f'Running - {name}', flush=True)
    with log.open('w') as stream:
        result = subprocess.run(command, cwd=cwd, env=env, stdout=stream, stderr=subprocess.STDOUT)
    if result.returncode:
        print(log.read_text(errors='replace')[-6000:], file=sys.stderr)
        raise subprocess.CalledProcessError(result.returncode, command)


recipe = OUT / 'obj/DEVELOPMENT_ARM64_SHENG/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
compile_record = json.loads(recipe.read_text().strip().rstrip(','))
record = {}
build(ROOT, OUT, compile_record, OUT / 'source', run, record, dict(os.environ),
      transport='ufs', gop_display=True, recovery_input=True)
driver = record['ufs_driver']
print(json.dumps(dict(binary_sha256=driver['binary_sha256'],
                      gop_source_sha256=driver['native_gop_display']['source_sha256'],
                      gop_board_patches=driver['native_gop_display']['board_patches'],
                      input_source_sha256=driver['native_recovery_input']['source_sha256']), indent=2))
