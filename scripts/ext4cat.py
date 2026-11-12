#!/usr/bin/env python3
"""Minimal read-only ext4 reader: list a directory or copy a file out of an ext4 image.
Usage: ext4cat.py <image> ls <path> | ext4cat.py <image> get <path> <out>"""
import struct
import sys


class Ext4:
    def __init__(self, path):
        self.f = open(path, 'rb')
        sb = self.read(1024, 1024)
        if struct.unpack_from('<H', sb, 0x38)[0] != 0xEF53:
            raise SystemExit('not ext4')
        self.bs = 1024 << struct.unpack_from('<I', sb, 0x18)[0]
        self.ipg = struct.unpack_from('<I', sb, 0x28)[0]
        self.isz = struct.unpack_from('<H', sb, 0x58)[0]
        incompat = struct.unpack_from('<I', sb, 0x60)[0]
        self.desc = struct.unpack_from('<H', sb, 0xFE)[0] if incompat & 0x80 else 32
        self.desc = self.desc or 32
        first = struct.unpack_from('<I', sb, 0x14)[0]
        self.gdt = (first + 1) * self.bs

    def read(self, off, n):
        self.f.seek(off)
        return self.f.read(n)

    def inode(self, num):
        g, i = divmod(num - 1, self.ipg)
        d = self.read(self.gdt + g * self.desc, self.desc)
        table = struct.unpack_from('<I', d, 8)[0] | ((struct.unpack_from('<I', d, 0x28)[0] << 32) if self.desc >= 64 else 0)
        return self.read(table * self.bs + i * self.isz, self.isz)

    def blocks(self, ino):
        flags = struct.unpack_from('<I', ino, 0x20)[0]
        if not flags & 0x80000:
            raise SystemExit('not an extent inode')
        return self.extents(ino[0x28:0x28 + 60])

    def extents(self, node):
        magic, entries, _, depth = struct.unpack_from('<HHHH', node, 0)
        if magic != 0xF30A:
            raise SystemExit('bad extent header')
        out = []
        for e in range(entries):
            o = 12 + e * 12
            if depth == 0:
                lblk, ln, hi, lo = struct.unpack_from('<IHHI', node, o)
                out.append((lblk, (hi << 32) | lo, ln & 0x7FFF))
            else:
                _, lo, hi = struct.unpack_from('<IIH', node, o)
                out += self.extents(self.read(((hi << 32) | lo) * self.bs, self.bs))
        return out

    def data(self, num):
        ino = self.inode(num)
        size = struct.unpack_from('<I', ino, 4)[0] | (struct.unpack_from('<I', ino, 0x6C)[0] << 32)
        buf = bytearray(size)
        for lblk, pblk, ln in self.blocks(ino):
            chunk = self.read(pblk * self.bs, ln * self.bs)
            start = lblk * self.bs
            if start < size:
                buf[start:start + len(chunk)] = chunk[:size - start]
        return bytes(buf[:size])

    def entries(self, num):
        d = self.data(num)
        off = 0
        while off + 8 <= len(d):
            ino, rec, nlen, ftype = struct.unpack_from('<IHBB', d, off)
            if rec < 8:
                break
            if ino:
                yield d[off + 8:off + 8 + nlen].decode('utf-8', 'replace'), ino, ftype
            off += rec

    def lookup(self, path):
        num = 2
        for part in [p for p in path.split('/') if p]:
            for name, ino, _ in self.entries(num):
                if name == part:
                    num = ino
                    break
            else:
                raise SystemExit('not found: ' + part)
        return num


fs = Ext4(sys.argv[1])
if sys.argv[2] == 'ls':
    for name, ino, ftype in fs.entries(fs.lookup(sys.argv[3])):
        print(ftype, name)
else:
    open(sys.argv[4], 'wb').write(fs.data(fs.lookup(sys.argv[3])))
