#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Write a kernel zero-page sparse for the akita DarwinBoot (ExpandSparseKernel in DarwinBoot.c).

Layout: "AKSPRS01", expanded size (u64), page size 4096 (u32), page count (u32), 8 reserved bytes,
a bitmap with one bit per page (LSB first), then the pages whose bit is set, in order; the last page
is stored at its actual length. Absent pages are zero.

  sparse-kernel.py IN OUT
"""
import struct
import sys

PAGE = 4096


def sparse(data):
    pages = (len(data) + PAGE - 1) // PAGE
    bitmap = bytearray((pages + 7) // 8)
    body = []
    for index in range(pages):
        page = data[index * PAGE:(index + 1) * PAGE]
        if any(page):
            bitmap[index // 8] |= 1 << (index % 8)
            body.append(page)
    return struct.pack('<8sQII8x', b'AKSPRS01', len(data), PAGE, pages) + bytes(bitmap) + b''.join(body)


def expand(blob):
    magic, size, page, pages = struct.unpack_from('<8sQII', blob)
    assert magic == b'AKSPRS01' and page == PAGE
    out = bytearray(size)
    cursor = 32 + (pages + 7) // 8
    for index in range(pages):
        if blob[32 + index // 8] >> (index % 8) & 1:
            length = min(PAGE, size - index * PAGE)
            out[index * PAGE:index * PAGE + length] = blob[cursor:cursor + length]
            cursor += length
    assert cursor == len(blob)
    return bytes(out)


if __name__ == '__main__':
    source = open(sys.argv[1], 'rb').read()
    result = sparse(source)
    if expand(result) != source:
        raise SystemExit('sparse round trip differs')
    open(sys.argv[2], 'wb').write(result)
    print(f'sparse kernel: {len(source)} -> {len(result)} bytes')
