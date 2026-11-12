#!/usr/bin/env python3
"""Give a relinked Mach-O the LC_UUID of an earlier build (PanVK keys its shader caches on it: keeps the seeded cache valid when only
darwin-drm changed, not the compiler). Usage: keep-uuid.py <target> <reference>. Re-sign the target afterwards."""
import struct, sys
def uuid_at(data):
    ncmds = struct.unpack_from('<I', data, 16)[0]; off = 32
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', data, off)
        if cmd == 0x1b: return off + 8
        off += size
    raise SystemExit('no LC_UUID')
t = bytearray(open(sys.argv[1], 'rb').read()); r = open(sys.argv[2], 'rb').read()
assert struct.unpack_from('<I', t, 0)[0] == 0xfeedfacf and struct.unpack_from('<I', r, 0)[0] == 0xfeedfacf
a, b = uuid_at(t), uuid_at(r)
t[a:a + 16] = r[b:b + 16]
open(sys.argv[1], 'wb').write(t)
print('uuid', r[b:b + 16].hex())
