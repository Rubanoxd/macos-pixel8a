#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Package a macOS 26.5 (25F71) RAM-boot candidate for the Pixel 8a (akita).

The akita counterpart of scripts/package_macos26_ufs.py, without its sheng qualification records:
  * kernel.macho: the akita security image (emit_macos26_security.py over the MACOS_BOARD=akita build,
    whose NativeUfs carries the Exynos UFS transport and roots at the userdata partition), zero-page
    sparse (platform/akita/sparse-kernel.py; DarwinBoot-macos expands it);
  * kernel.macho.adt: platform/akita/make-akita-adt.py, then the same native text state as sheng
    (development root, BaseSystem root hash and volume UUID, GOP display);
  * EFI/UniXNU/unixnu.toml: one xnu entry, booted by the UniXNU menu of the akita Mu-Silicium firmware;
  * the boot image: UniXNU-akita/scripts/akita-bootimg.py puts the FAT in the kernel field (the akita
    ABL drops the boot image ramdisk), behind the Mu firmware volume, LZ4-compressed with it.

Only `fastboot boot` is meant; nothing here writes a partition. The userdata root image
(prepare_macos26_ufs_root.py) is flashed separately.

  package-akita-macos.py --kernel-build out/akita-security-ufs --root-hash private/macos26-media/root-hash.bin \
      --output out/akita-macos-candidate
"""
import argparse
import os
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import uuid

ROOT = Path(__file__).resolve().parents[2]
WORKSPACE = ROOT.parents[1]                      # /Volumes/datos2/unixnu-akita
sys.path.insert(0, str(ROOT / 'scripts'))
from native_devtree import configure_native_text_state  # noqa: E402

# DRAM and the DarwinBoot-owned window of the iPadOS akita board (UniXNU-akita/board.toml).
DRAM_SIZE = 0x180000000
OWNED_MEMORY = int(os.environ.get("AKITA_OWNED_MEMORY", "0x200000000"), 0)   # akita: up to 8 GiB; DarwinBoot takes its largest run (5.26 GiB seen by macOS, fullos183)
BOOT_ARGS = ('-v debug=0x14e keepsyms=1 cpus=1 -enable_kprintf_spam -sheng-early-console '
             'native-ufs-root=1 native-ufs-diagnostics=1 -sheng-development-no-ignition '
             'native-ufs-isc-overlay=1 native-gop-display=1 sheng-recovery-ui=1 '
             'sheng-boot-progress=1 sheng-stderr-log=2 sheng-stderr-name=WindowServer '
             'akita-wdt-pet-seconds=900')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--kernel-build', type=Path, required=True)
    parser.add_argument('--root-hash', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--boot-args', default=BOOT_ARGS)
    parser.add_argument('--cpus', type=int, choices=(1, 2, 4, 8, 9), default=1,
                        help='CPUs in the ADT: the four Cortex-A510 (0x000-0x300), then the four Cortex-A715 (0x400-0x700), then the Cortex-X3 (0x800)')
    parser.add_argument('--nvram', action='append', default=[], metavar='NAME=VALUE',
                        help='System NVRAM variables for the ADT bank, e.g. prev-lang:kbd=es:87 (skips the language chooser)')
    parser.add_argument('--mu', type=Path, default=WORKSPACE / 'Mu-Silicium/Mu-akita-0.img')
    parser.add_argument('--fullos-root-uuid', default=None,
                        help='boot the full macOS system volume with this UUID instead of the paired BaseSystem: '
                             'osenvironment main, root-matching by UUID, no root hash (unsealed development root)')
    parser.add_argument('--stock', type=Path, default=WORKSPACE / 'factory/stock/boot.img')
    parser.add_argument('--gpu-addon', type=Path, default=None,
                        help='NGPUADD2 file (scratch/make-akita-gpu-addon.py) carried as kernel.macho.gpuaddon; the kernel unpacks it at boot '
                             '(boot-arg akita-gpu-addon=1) and rc.server puts the Mali Metal plugin in place before WindowServer starts')
    args = parser.parse_args()

    kernel = args.kernel_build / 'kernel.development.sheng'
    relink = json.loads((args.kernel_build / 'security-relink.json').read_text())
    if relink['storage_driver']['build'].get('board') != 'akita':
        raise SystemExit('the security image must carry the MACOS_BOARD=akita UFS transport')
    recovery = json.loads((ROOT / 'config/macos26-recovery-root.json').read_text())
    root_uuid = uuid.UUID(recovery['volume_uuid'])
    root_hash = args.root_hash.read_bytes()
    evidence = json.loads(args.root_hash.with_suffix(args.root_hash.suffix + '.json').read_text())
    if (evidence.get('build') != '25F71' or evidence.get('raw_disk_sha256') != recovery['sha256']
            or evidence.get('payload_sha256') != hashlib.sha256(root_hash).hexdigest()):
        raise SystemExit('the root hash does not belong to the paired BaseSystem')

    args.output.mkdir(parents=True, exist_ok=False)
    with tempfile.TemporaryDirectory(dir=args.output) as temporary:
        work = Path(temporary)
        stage = work / 'EFI/UniXNU'
        files = stage / 'Payloads'
        files.mkdir(parents=True)
        # Zero-page sparse: the ABL's load area holds about 77 MB of payload, the kernel is ~120 MB.
        subprocess.run([sys.executable, ROOT / 'platform/akita/sparse-kernel.py', kernel, files / 'kernel.macho'],
                       check=True)
        adt = files / 'kernel.macho.adt'
        if args.gpu_addon:
            shutil.copy2(args.gpu_addon, files / 'kernel.macho.gpuaddon')
        subprocess.run([sys.executable, ROOT / 'platform/akita/make-akita-adt.py', '--output', adt,
                        '--dram-size', hex(DRAM_SIZE), '--owned-memory-size', hex(OWNED_MEMORY), '--panic-log',
                        '--cpu-regs', ','.join(hex(n << 8) for n in range(args.cpus)),
                        *[item for n in range(args.cpus)
                          for item in ('--cpu-compatible', 'arm,cortex-a510' if n < 4 else 'arm,cortex-a715' if n < 8 else 'arm,cortex-x3')],
                        '--random-seed-bytes', '256', '--buttons',
                        *(('--gpu-addon',) if args.gpu_addon else ()),
                        *[item for value in args.nvram for item in ('--nvram', value)],
                        *(('--os-environment', 'recoveryos', '--root-match-volume-uuid', str(uuid.UUID(args.fullos_root_uuid)))
                          if args.fullos_root_uuid else
                          ('--os-environment', 'recoveryos', '--root-match-volume-uuid', str(root_uuid)))], check=True)
        if args.fullos_root_uuid:
            configure_native_text_state(adt, development_root=True, native_gop_display=True)
        else:
            configure_native_text_state(adt, development_root=True, root_hash=root_hash,
                                        boot_volume_uuid=root_uuid, native_gop_display=True)
        identity = subprocess.check_output(['dwarfdump', '--uuid', str(kernel)], text=True).split()[1]
        (stage / 'unixnu.toml').write_text(
            'version = 1\n[boot]\ntimeout = 3\nfallback_timeout = 30\ndefault = "macos26"\n'
            '[kits]\nautoload = false\n[[entry]]\nid = "macos26"\n'
            f'title = "akita macOS 26.5 Recovery {identity[:8]}"\ntype = "xnu"\n'
            f'path = "Payloads/kernel.macho"\nargs = "{args.boot_args}"\n')
        content = sum(p.stat().st_size for p in stage.rglob('*') if p.is_file())
        mib = (content >> 20) + 3   # the akita ABL hangs on a kernel field much over ~76 MiB: keep the FAT slack small
        fat = work / 'payload.fat'
        subprocess.run(['mformat', '-C', '-F', '-T', str(mib * 2048), '-v', 'AKITAMAC',
                        '-N', '414b4d26', '-i', fat, '::'], check=True)
        subprocess.run(['mcopy', '-i', fat, '-s', '-Q', work / 'EFI', '::/'], check=True)
        image = args.output / 'akita-macos26-boot.img'
        subprocess.run([sys.executable, WORKSPACE / 'UniXNU-akita/scripts/akita-bootimg.py', '--mu', args.mu,
                        '--stock', args.stock, '--kernel-payload', fat, '-o', image], check=True)
        shutil.copy2(adt, args.output / 'kernel.macho.adt')
        shutil.copy2(stage / 'unixnu.toml', args.output / 'unixnu.toml')
    data = image.read_bytes()
    record = dict(image=image.name, image_bytes=len(data), image_sha256=hashlib.sha256(data).hexdigest(),
                  kernel_uuid=identity, kernel_sha256=hashlib.sha256(kernel.read_bytes()).hexdigest(),
                  payload_fat_mib=mib, boot_args=args.boot_args, nvram=args.nvram, cpus=args.cpus, root_volume_uuid=str(root_uuid),
                  dram_size=hex(DRAM_SIZE), owned_memory=hex(OWNED_MEMORY),
                  temporary_ram_boot=True, partitions_written=False)
    (args.output / 'build.json').write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps(record, indent=2))


if __name__ == '__main__':
    main()
