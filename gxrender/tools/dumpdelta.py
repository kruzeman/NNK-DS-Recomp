import struct, sys
d = open(sys.argv[1], 'rb').read()
ver, unit, flags, size = struct.unpack('<IIII', d[4:20])
print('unit', unit, 'flags', flags, 'size', hex(size))
nruns, = struct.unpack('<I', d[20:24])
p = 24
for r in range(nruns):
    off, ln = struct.unpack('<II', d[p:p+8]); p += 8
    print(f'run {r}: offset={hex(off)} len={ln} bytes={d[p:p+ln].hex()}')
    p += ln
