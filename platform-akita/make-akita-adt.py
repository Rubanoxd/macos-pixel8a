#!/usr/bin/env python3
"""Generate the minimal akita (Pixel 8a, Tensor G3 zuma) Apple-format DT for the first macOS boot.

A port of UniXNU-sheng's make-sheng-adt.py: same node structure, with the zuma values of
UniXNU-zuma-common/dts/zuma.dtsi (the DT the iPadOS akita port boots with):

  - GICv3 distributor 0x10400000/0x10000, redistributor 0x10440000/0x100000, stride 0x20000
    (arm-io child offsets under a peripheral range at 0x10000000)
  - CPU timebase 24576000 Hz (zuma architectural counter)
  - UFS host 0x13200000 (Exynos UFSHCI, matched as google,zuma-ufs)
  - DRAM base 0x80000000; size comes from the device and is passed in.

This is first-boot bring-up data: the platform has no matching platform
expert yet, so the kernel is expected to reach the video console and report
the genuine missing-driver panic on screen. No Apple device identity is
claimed and no property is invented beyond these pinned sources.
"""
from pathlib import Path
import argparse
import os
import struct
import zlib

ROOT = Path(__file__).resolve().parents[1]

DRAM_BASE = 0x80000000
ARMIO_BASE = 0x10000000          # peripheral window containing the GIC and the UFS host
ARMIO_SIZE = 0x10000000
GIC_DIST = 0x10400000
GIC_DIST_SIZE = 0x10000
GIC_REDIST = 0x10440000
GIC_REDIST_SIZE = 0x120000   # nine cores x 0x20000 (zuma.dtsi)
GIC_REDIST_STRIDE = 0x20000
TIMEBASE = 24576000
UFS_HC = 0x13200000
UFS_HC_SIZE = 0x200
PINCTRL_ALIVE = 0x154D0000        # gpa banks: the volume and power buttons (gpio-keys)


def node(name, properties=(), children=()):
    properties = [('name', name.encode() + b'\0'), *properties]
    result = struct.pack('<II', len(properties), len(children))
    for key, data in properties:
        result += struct.pack('<32sI', key.encode(), len(data))
        result += data + bytes(-len(data) % 4)
    return result + b''.join(children)


def empty_nvram_proxy(variables=()):
    """A fresh volatile CHRP bank for upstream IODTNVRAM, with no controller.

    `variables` are NAME=VALUE text records for the common (system) partition, for example
    prev-lang:kbd=es:87, which tells Recovery the language is already chosen."""
    def header(name, size):
        raw = bytearray(struct.pack('<BBH12s', 0x70, 0, size // 16, name))
        checksum = sum(raw)
        while checksum > 255:
            checksum = (checksum & 255) + (checksum >> 8)
        raw[1] = checksum
        return raw

    image = bytearray(8192)
    image[:16] = header(b'nvram', 32)
    image[32:48] = header(b'common', 16 + 2048)
    records = b''.join(item.encode('ascii') + b'\0' for item in variables)
    if len(records) > 2048 or any('=' not in item for item in variables):
        raise SystemExit('NVRAM variables must be NAME=VALUE and fit in 2048 bytes')
    image[48:48 + len(records)] = records
    free_at = 32 + 16 + 2048
    image[free_at:free_at + 16] = header(b'free', len(image) - free_at)
    struct.pack_into('<I', image, 16, zlib.adler32(image[20:]))
    return [('nvram-bank-size', struct.pack('<I', len(image))),
            ('nvram-bank-count', struct.pack('<I', 1)),
            ('nvram-current-bank', struct.pack('<I', 0)),
            ('nvram-proxy-data', bytes(image))]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path,
                        default=ROOT / 'out/akita/kernel.macho.adt')
    parser.add_argument('--dram-size', type=lambda value: int(value, 0), required=True,
                        help='Actual observed DRAM size, e.g. 0x230000000')
    parser.add_argument('--panic-log', action='store_true', help='Request 512 KiB panic RAM inside the owned DarwinBoot window')
    parser.add_argument('--owned-memory-size', type=lambda value: int(value, 0), default=None,
                        help='Explicit DarwinBoot contiguous window request, 256 MiB..8 GiB in 2 MiB steps')
    parser.add_argument('--root-match-volume-uuid', default=None,
                        help='/chosen/root-matching: select the root by the UUID of an IOMedia (the standard source mechanism)')
    parser.add_argument('--os-environment', choices=('main', 'recoveryos'), default='main',
                        help='Match the actual root volume OS variant')
    parser.add_argument('--unique-chip-id', type=lambda value: int(value, 16), default=None,
                        help='/chosen/unique-chip-id (8 bytes) and --chip-id (4 bytes) seed XNU IOPlatformUUID; software identity seeds, not Apple silicon IDs')
    parser.add_argument('--chip-id', type=lambda value: int(value, 16), default=None)
    parser.add_argument('--ipad-identity', action='store_true',
                        help='/product and /buttons describe an iPad-class device without a Home button (MobileGestalt reads these from the tree); '
                             'values follow the unchanged iPad7,1 tree, the panel geometry and button set are sheng\'s')
    parser.add_argument('--cpu-regs', default='0',
                        help='Comma-separated CPU MPIDR affinity values (the /cpus reg of each core); the first is the running boot CPU. '
                             'zuma numbers its cores 0x000..0x700 in Affinity1 (zuma.dtsi). More than one adds /psci')
    parser.add_argument('--cpu-compatible', action='append',
                        choices=['arm,cortex-a510', 'arm,cortex-a710', 'arm,cortex-a715', 'arm,cortex-x3'],
                        help='Actual compatible of each listed CPU, in --cpu-regs order')
    parser.add_argument('--psci-method', choices=['smc', 'hvc'], default='smc',
                        help='/psci method used by the secondary-CPU start (the board DT declares smc)')
    parser.add_argument('--packed-data-ramdisk', action='store_true', help='Hand off validated DBRDISK1 bytes to the native sparse block driver, without a dense memdev alias')
    parser.add_argument('--data-ramdisk', action='store_true',
                        help='/chosen/memory-map RAMDisk placeholder; the loader fills in the real range for a scratch Data seed')
    parser.add_argument('--persistent-data', action='store_true',
                        help='The Data disk keeps its writes across boots (sheng-data-persist): no ephemeral-storage, so first-boot data migration runs')
    parser.add_argument('--trust-cache', action='store_true', help='Zeroed TrustCache sidecar handoff placeholder')
    parser.add_argument('--gpu-addon', action='store_true', help='Zeroed GpuAddon sidecar handoff placeholder (the loader fills in kernel.macho.gpuaddon)')
    parser.add_argument('--text-fixup', action='append', default=[],
                        help='/chosen/text-fixups entry <file name>,<file offset>,<original>,<replacement> for the kernel\'s signed-text fixups')
    parser.add_argument('--nvram', action='append', default=[], metavar='NAME=VALUE',
                        help='Preset a system NVRAM variable in the volatile bank (repeatable)')
    parser.add_argument('--buttons', action='store_true', help='/arm-io/native-recovery-input for the side-button keyboard')
    parser.add_argument('--random-seed-bytes', type=int, choices=(64, 256), default=64,
                        help='Paired XNU bootloader entropy size')
    args = parser.parse_args()
    if (args.unique_chip_id is None) != (args.chip_id is None):
        parser.error('--unique-chip-id and --chip-id must be supplied together')
    if args.unique_chip_id is not None and not (0 < args.unique_chip_id < 1 << 64 and 0 < args.chip_id < 1 << 32):
        parser.error('--unique-chip-id must fit 8 bytes and --chip-id 4 bytes, both nonzero')
    if args.owned_memory_size is not None and (
            not 0x10000000 <= args.owned_memory_size <= 0x200000000
            or args.owned_memory_size % 0x200000):
        parser.error('Owned memory must be 256 MiB to 8 GiB, aligned to 2 MiB')

    try:
        cpu_regs = [int(value, 0) for value in args.cpu_regs.split(',')]
    except ValueError:
        parser.error('--cpu-regs must be integers')
    if (not cpu_regs or cpu_regs[0] != 0 or len(set(cpu_regs)) != len(cpu_regs) or len(cpu_regs) > 9
            or any(reg < 0 or reg > 0xffff or reg & 0xff for reg in cpu_regs)):
        parser.error('--cpu-regs: up to nine distinct Affinity1-numbered cores, boot CPU 0 first')
    compatibles = args.cpu_compatible or ['arm,cortex-a715'] * len(cpu_regs)
    if len(compatibles) != len(cpu_regs):
        parser.error('--cpu-compatible must describe every listed CPU')
    cpus = [node('cpu%d' % index, [('state', b'running\0' if index == 0 else b'waiting\0'),
                                   ('reg', struct.pack('<I', reg)),
                                   ('device_type', b'cpu\0'), ('compatible', compatibles[index].encode() + b'\0'),
                                   ('timebase-frequency', struct.pack('<I', TIMEBASE))])
            for index, reg in enumerate(cpu_regs)]
    tree = node('device-tree', [
        # hw.product/hw.machine/hw.target come from the root 'model'/'target-type' (IODTPlatformExpert);
        # libMobileGestalt prefix-matches hw.product against iPhone/iPod/iPad, so the iPad identity needs both here.
        ('model', b'iPad7,1\0' if args.ipad_identity else b'akita-first-boot\0'),
        *([('target-type', b'J120\0')] if args.ipad_identity else []),
        # Root compatible strings of the akita board DT.
        ('compatible', b'google,akita\0google,zuma\0'),
        ('#address-cells', struct.pack('<I', 2)),
        ('#size-cells', struct.pack('<I', 2)),
    ], [
        node('chosen', [('random-seed', os.urandom(args.random_seed_bytes)),
                        ('platform-uuid', bytes.fromhex('7d7748c354734f319be85c9d372fff4e')),
                    *([('embedded-panic-log-size', struct.pack('<I', 0x80000))] if args.panic_log else []),
                        # Without these XNU never publishes IOPlatformUUID and gethostuuid() blocks forever
                        # for callers (configd) that pass a zero timeout.
                        *([('unique-chip-id', struct.pack('<Q', args.unique_chip_id)), ('chip-id', struct.pack('<I', args.chip_id))]
                          if args.unique_chip_id is not None else []),
                        # BaseSystem uses recoveryos; normal installations use main.
                        ('osenvironment', args.os_environment.encode() + b'\0'),
                        # Security status as on a dev-fused, undemoted board (tuple 0,0,1,1), as read by MobileGestalt: mobileactivationd's
                        # DeviceType then takes its native "Hactivation" branch (Activated without an activation record; no SEP or
                        # baseband). Values established on hardware by github.com/dipa-studio/ios-sweet (tools/mkadt.py).
                        *([('certificate-production-status', struct.pack('<I', 0)), ('effective-production-status-ap', struct.pack('<I', 0)),
                           ('certificate-security-mode', struct.pack('<I', 1)), ('effective-security-mode-sep', struct.pack('<I', 1))]
                          if args.ipad_identity else []),
                        # This property describes the actual volatile Data RAM disk. A persistent one is not ephemeral: with the property
                        # datamigrator skips every migration (os_variant_uses_ephemeral_storage), so the staged system apps never install.
                        *([('ephemeral-storage', struct.pack('<I', 1))]
                          if (args.data_ramdisk or args.packed_data_ramdisk) and not args.persistent_data else []),
                        *( [('darwinboot-memory-size', struct.pack('<Q', args.owned_memory_size))]
                           if args.owned_memory_size else [] ),
                        ('dram-base', struct.pack('<Q', DRAM_BASE)),
                        ('dram-size', struct.pack('<Q', args.dram_size)),
                        # Image4 hash algorithm. The target iPad's DT specifies sha2-384 in /chosen; this
                        # selects the algorithm only and asserts nothing about Apple silicon or fuses.
                        ('crypto-hash-method', b'sha2-384\0'),
                        *( [('root-matching', ('<dict><key>IOProviderClass</key><string>IOMedia</string>'
                                               '<key>IOPropertyMatch</key><dict><key>UUID</key><string>' +
                                               args.root_match_volume_uuid.upper() + '</string></dict></dict>\0').encode())]
                           if args.root_match_volume_uuid else [] ),
                        # Strict policy for the original AMFI: platform code only. This keeps AMFI
                        # enforcing; it does not relax it.
                        ('amfi-only-platform-code', struct.pack('<I', 1)),
                        *([('text-fixups', b''.join(f.encode() + b'\0' for f in args.text_fixup))] if args.text_fixup else []),
                        *empty_nvram_proxy(args.nvram)],
             [node('memory-map', ([("PackedRAMDisk" if args.packed_data_ramdisk else "RAMDisk", bytes(16))] if (args.data_ramdisk or args.packed_data_ramdisk) else []) +
                                 ([('TrustCache', bytes(16))] if args.trust_cache else []) +
                                 ([('GpuAddon', bytes(16))] if args.gpu_addon else [])),
              # Read by the original AppleImage4 at start-up; present and empty here.
              node('manifest-properties')]),
        *([node('pram', [('reg', bytes(16))])] if args.panic_log else []),
        node('cpus', children=cpus),
        *([node('psci', [('compatible', b'arm,psci-1.0\0'), ('method', args.psci_method.encode() + b'\0')])] if len(cpus) > 1 else []),
        # Suspend/resume is not implemented for this board. Use the original
        # root-domain policy input rather than allowing the CPU to park forever.
        # kern.maxvnodes: this kernel is built with CONFIG_VNODES=1024 (xsmall), so desiredvnodes clips to 1024 and the desktop's
        # ~1950 live vnodes made vnode jetsam kill idle daemons about once a second (int-102 log). XNU reads the value from /defaults
        # before it clips; this is its own formula, memory/64K + 1024, for the iPad7,1's 4 GiB.
        node('defaults', [('sleep-disabled', struct.pack('<I', 1)), ('kern.maxvnodes', struct.pack('<I', 66560))]),
        # gethostuuid() (configd, opendirectoryd) waits for IOPlatformUUID, which the SHENG kernel takes from
        # /options platform-uuid; a fixed made-up UUID for this board.
        node('options', [('platform-uuid', bytes.fromhex('7d7748c354734f319be85c9d372fff4e'))]),
        # The original AppleImage4 panics without a product node. Name and model are the
        # board DT's own strings.
        *( [node('buttons', [('compatible', b'buttons\0'), ('device_type', b'buttons\0'),
                             # No 'menu' entry: akita has no Home button. 2 = the non-mechanical answer BSPlatform passes through.
                             ('button-names', b'hold\0voldown\0volup\0'),
                             ('home-button-type', struct.pack('<I', 2))])] if args.ipad_identity else [] ),
        node('product', [('product-name', b'iPad Pro (12.9-inch) (2nd generation)\0' if args.ipad_identity else b'Google Pixel 8a\0'),
                         ('model', b'google,akita\0'),
                         # About This Mac / System Information read these (a Mac carries all three in /product).
                         *( [('product-description', b'Google Pixel 8a\0'),
                             ('marketing-name', b'Google Pixel 8a\0'),
                             ('product-soc-name', b'Google Tensor G3\0')] if not args.ipad_identity else [] ),
                         *( [('product-description', b'iPad Pro (12.9-inch) (2nd generation)\0'),
                             ('artwork-device-idiom', b'pad\0'),
                             ('artwork-device-subtype', struct.pack('<I', 2732)),
                             ('artwork-scale-factor', struct.pack('<I', 2)),
                             ('artwork-display-gamut', b'P3\0'),
                             ('compatible-device-fallback', b'iPad6,7\0'),
                             ('thin-bezel', struct.pack('<I', 1))] if args.ipad_identity else [] )]),
        node('arm-io', [('device_type', b'arm-io\0'),
                        ('#address-cells', struct.pack('<I', 2)),
                        ('#size-cells', struct.pack('<I', 2)),
                        ('ranges', struct.pack('<QQQ', 0, ARMIO_BASE, ARMIO_SIZE))],
             [node('intc', [('compatible', b'arm,gic-v3\0'),
                            ('reg', struct.pack('<4Q',
                                                GIC_DIST - ARMIO_BASE, GIC_DIST_SIZE,
                                                GIC_REDIST - ARMIO_BASE, GIC_REDIST_SIZE)),
                            ('redistributor-stride', struct.pack('<Q', GIC_REDIST_STRIDE))]),
              # Class matching only; NativeUfs (NATIVE_UFS_ZUMA) takes its physical windows
              # from the constants in GccUfs.h.
              node('ufshc', [('compatible', b'google,zuma-ufs\0'),
                             ('reg', struct.pack('<2Q', UFS_HC - ARMIO_BASE, UFS_HC_SIZE))]),
              # The side buttons as a keyboard (platform/akita/input, native-recovery-input=1).
              *([node('native-recovery-input', [('compatible', b'google,akita-buttons\0'),
                                               ('reg', struct.pack('<2Q', PINCTRL_ALIVE - ARMIO_BASE, 0x1000))])]
                if args.buttons else [])]),
    ])
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(tree)
    print(f'akita ADT: {len(tree)} bytes, dram {args.dram_size:#x}, timebase {TIMEBASE}')


if __name__ == '__main__':
    main()
