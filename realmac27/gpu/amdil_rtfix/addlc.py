#!/usr/bin/env python3
# addlc.py <in> <out> <dylib path> - append one LC_LOAD_DYLIB to a thin x86_64 Mach-O, using header padding.
import struct, sys
src, dst, path = sys.argv[1], sys.argv[2], sys.argv[3].encode()
b = bytearray(open(src, 'rb').read())
magic, cpu, sub, ft, ncmds, sizeofcmds, flags, res = struct.unpack_from('<IiiIIIII', b, 0)
assert magic == 0xfeedfacf, "not a thin 64-bit Mach-O"
off, first = 32, None
for i in range(ncmds):
    cmd, cs = struct.unpack_from('<II', b, off)
    if cmd == 0xc and path in bytes(b[off+24:off+cs]): sys.exit("already loads it")
    if cmd == 0x19:
        for s in range(struct.unpack_from('<I', b, off+64)[0]):
            o = struct.unpack_from('<I', b, off+72+s*80+48)[0]
            if o and (first is None or o < first): first = o
    off += cs
name = path + b'\0'
cs = (24 + len(name) + 7) & ~7
assert 32 + sizeofcmds + cs <= first, "no header room"
lc = struct.pack('<IIIIII', 0xc, cs, 24, 2, 0x10000, 0x10000) + name
lc += b'\0' * (cs - len(lc))
b[32+sizeofcmds:32+sizeofcmds+cs] = lc
struct.pack_into('<II', b, 16, ncmds + 1, sizeofcmds + cs)
open(dst, 'wb').write(b)
print("added", path.decode(), "ncmds", ncmds + 1)
