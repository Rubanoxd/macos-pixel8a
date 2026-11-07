#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Extend the akita userdata root image with the full macOS 25F71 system, as an Android sparse image.

The existing image (private/macos26-media/akita-root/macosroot.img: SHNGROOT manifest, OS = paired BaseSystem, ISC) is
copied unchanged, then two windows are appended, each aligned to 1 MiB:
  MacOS     the full system disk image (IPSW "OS", 094-56453-087.dmg, decrypted)
  SystemOS  the OS cryptex disk image (IPSW "Cryptex1,SystemOS", 094-56679-089.dmg, decrypted)
and the manifest (block 0) is rewritten for four windows. NativeUfs in recovery mode publishes OS and ISC as before
and the extra windows as plain read-only disks (platform/akita/drivers/NativeUfs, RootPublication.h).

Output: a sparse image (RAW chunks, FILL for all-zero 4 KiB blocks) for `fastboot flash userdata`, plus build.json.
Nothing is written to the phone here.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

BLOCK = 4096
ALIGN = 1 << 20
ZERO = bytes(BLOCK)
CHUNK_RAW, CHUNK_FILL = 0xCAC1, 0xCAC2


def parse_manifest(block):
    if block[:8] != b'SHNGROOT':
        raise SystemExit('the base image has no SHNGROOT manifest')
    version, count, image_bytes = struct.unpack_from('<IIQ', block, 8)
    if version != 1 or not 1 <= count <= 4 or hashlib.sha256(block[:4064]).digest() != block[4064:4096]:
        raise SystemExit('unexpected base manifest')
    windows = []
    for i in range(count):
        entry = block[24 + 128 * i:24 + 128 * (i + 1)]
        name = entry[:32].split(b'\0')[0].decode()
        offset, size = struct.unpack_from('<QQ', entry, 32)
        windows.append(dict(name=name, offset=offset, bytes=size, entry=entry))
    return image_bytes, windows


def manifest(image_bytes, entries):
    block = bytearray(BLOCK)
    block[:8] = b'SHNGROOT'
    struct.pack_into('<IIQ', block, 8, 1, len(entries), image_bytes)
    for i, entry in enumerate(entries):
        block[24 + 128 * i:24 + 128 * (i + 1)] = entry
    block[4064:4096] = hashlib.sha256(bytes(block[:4064])).digest()
    return bytes(block)


def entry(name, offset, size, digest):
    e = bytearray(128)
    raw = name.encode()
    if not 0 < len(raw) < 32:
        raise SystemExit('bad window name')
    e[:len(raw)] = raw
    struct.pack_into('<QQII', e, 32, offset, size, 512, 1)
    e[56:88] = digest
    return bytes(e)


class Sparse:
    """Android sparse writer: consecutive RAW blocks merge into one chunk, zero blocks into FILL chunks."""

    def __init__(self, path, total_blocks):
        self.f = open(path, 'wb')
        self.total_blocks = total_blocks
        self.chunks = 0
        self.f.write(bytes(28))
        self.kind = None
        self.pending = []
        self.pending_blocks = 0
        self.raw_blocks = 0
        self.fill_blocks = 0
        self.written_blocks = 0

    def _flush(self):
        if not self.pending_blocks:
            return
        if self.kind == CHUNK_RAW:
            data = b''.join(self.pending)
            self.f.write(struct.pack('<HHII', CHUNK_RAW, 0, self.pending_blocks, 12 + len(data)) + data)
            self.raw_blocks += self.pending_blocks
        else:
            self.f.write(struct.pack('<HHII', CHUNK_FILL, 0, self.pending_blocks, 16) + bytes(4))
            self.fill_blocks += self.pending_blocks
        self.chunks += 1
        self.written_blocks += self.pending_blocks
        self.pending, self.pending_blocks, self.kind = [], 0, None

    def block(self, data):
        kind = CHUNK_FILL if data == ZERO else CHUNK_RAW
        if kind != self.kind or self.pending_blocks >= 16384:
            self._flush()
            self.kind = kind
        if kind == CHUNK_RAW:
            self.pending.append(data)
        self.pending_blocks += 1

    def close(self):
        self._flush()
        if self.written_blocks != self.total_blocks:
            raise SystemExit('sparse block count mismatch')
        self.f.seek(0)
        self.f.write(struct.pack('<IHHHHIIII', 0xED26FF3A, 1, 0, 28, 12, BLOCK, self.total_blocks, self.chunks, 0))
        self.f.close()


def stream(path, size, sink, digest):
    with open(path, 'rb') as f:
        remaining = size
        while remaining:
            data = f.read(min(remaining, 64 * BLOCK))
            if not data:
                raise SystemExit(f'{path} is shorter than expected')
            remaining -= len(data)
            digest.update(data)
            if len(data) % BLOCK:
                data += bytes(BLOCK - len(data) % BLOCK)
            for i in range(0, len(data), BLOCK):
                sink(data[i:i + BLOCK])


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--base', type=Path, required=True, help='existing macosroot.img')
    p.add_argument('--macos', type=Path, required=True, help='decrypted full system disk image (raw)')
    p.add_argument('--systemos', type=Path, required=True, help='decrypted SystemOS cryptex disk image (raw)')
    p.add_argument('--output', type=Path, required=True, help='output directory')
    args = p.parse_args()

    base_bytes = args.base.stat().st_size
    with open(args.base, 'rb') as f:
        base_image_bytes, windows = parse_manifest(f.read(BLOCK))
    if base_image_bytes != base_bytes or [w['name'] for w in windows] != ['OS', 'ISC']:
        raise SystemExit('expected the OS + ISC base image')
    sizes = {'MacOS': args.macos.stat().st_size, 'SystemOS': args.systemos.stat().st_size}
    for name, size in sizes.items():
        if not size or size % 512:
            raise SystemExit(f'{name}: size must be a positive multiple of 512')
    macos_offset = (base_bytes + ALIGN - 1) // ALIGN * ALIGN
    systemos_offset = (macos_offset + sizes['MacOS'] + ALIGN - 1) // ALIGN * ALIGN
    image_bytes = (systemos_offset + sizes['SystemOS'] + ALIGN - 1) // ALIGN * ALIGN
    args.output.mkdir(parents=True, exist_ok=True)
    sparse_path = args.output / 'userdata-full.sparse'
    sparse = Sparse(sparse_path, image_bytes // BLOCK)

    # Block 0 is written last (its digests are known only at the end): reserve it as a RAW chunk of its own.
    sparse.f.write(struct.pack('<HHII', CHUNK_RAW, 0, 1, 12 + BLOCK))
    manifest_at = sparse.f.tell()
    sparse.f.write(ZERO)
    sparse.chunks += 1
    sparse.written_blocks += 1
    sparse.raw_blocks += 1

    position = BLOCK
    with open(args.base, 'rb') as f:
        f.seek(BLOCK)
        while position < base_bytes:
            data = f.read(min(base_bytes - position, 64 * BLOCK))
            for i in range(0, len(data), BLOCK):
                sparse.block(data[i:i + BLOCK])
            position += len(data)
    for _ in range((macos_offset - position) // BLOCK):
        sparse.block(ZERO)
    digests = {}
    for name, path, offset in (('MacOS', args.macos, macos_offset), ('SystemOS', args.systemos, systemos_offset)):
        h = hashlib.sha256()
        stream(path, sizes[name], sparse.block, h)
        digests[name] = h.digest()
        end = offset + (sizes[name] + BLOCK - 1) // BLOCK * BLOCK
        following = systemos_offset if name == 'MacOS' else image_bytes
        for _ in range((following - end) // BLOCK):
            sparse.block(ZERO)
        print(f'{name}: {sizes[name]} bytes at {offset:#x}, sha256 {digests[name].hex()}', flush=True)

    entries = [w['entry'] for w in windows] + [entry('MacOS', macos_offset, sizes['MacOS'], digests['MacOS']),
                                               entry('SystemOS', systemos_offset, sizes['SystemOS'], digests['SystemOS'])]
    block0 = manifest(image_bytes, entries)
    sparse.close()
    with open(sparse_path, 'r+b') as f:
        f.seek(manifest_at)
        f.write(block0)
    with open(args.output / 'manifest.bin', 'wb') as f:
        f.write(block0)
    record = dict(build='25F71', image_bytes=image_bytes, sparse=sparse_path.name,
                  sparse_bytes=sparse_path.stat().st_size, raw_blocks=sparse.raw_blocks, fill_blocks=sparse.fill_blocks,
                  windows=[dict(name=w['name'], offset=w['offset'], bytes=w['bytes']) for w in windows] +
                  [dict(name='MacOS', offset=macos_offset, bytes=sizes['MacOS'], sha256=digests['MacOS'].hex(),
                        source=str(args.macos)),
                   dict(name='SystemOS', offset=systemos_offset, bytes=sizes['SystemOS'],
                        sha256=digests['SystemOS'].hex(), source=str(args.systemos))],
                  partition_writes_performed=False)
    (args.output / 'build.json').write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps(record, indent=2))


if __name__ == '__main__':
    main()
